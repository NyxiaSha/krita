/* This file is part of the KDE project

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
*/

#include "TreeShape.h"
#include "TreeTool.h"
#include "SelectionDecorator.h"
#include "TreeShapeMoveStrategy.h"
#include "KoGradientBackground.h"

#include <KoPointerEvent.h>
#include <KoToolSelection.h>
#include <KoToolManager.h>
#include <KoSelection.h>
#include <KoShapeController.h>
#include <KoShapeManager.h>
#include <KoDocument.h>
#include <KoCanvasBase.h>
#include <KoResourceManager.h>
#include <KoShapeRubberSelectStrategy.h>
#include <TreeShapeMoveCommand.h>
#include <commands/KoShapeDeleteCommand.h>
#include <commands/KoShapeCreateCommand.h>
#include <KoSnapGuide.h>

#include <QKeyEvent>

#include "kdebug.h"

class SelectionHandler : public KoToolSelection
{
public:
    SelectionHandler(TreeTool *parent)
        : KoToolSelection(parent), m_selection(parent->koSelection())
    {
        Q_ASSERT(m_selection);
    }

    bool hasSelection() {
        return m_selection->count();
    }

private:
    KoSelection *m_selection;
};

TreeTool::TreeTool(KoCanvasBase *canvas)
    : KoInteractionTool(canvas),
    m_hotPosition(KoFlake::TopLeftCorner),
    m_moveCommand(0),
    m_selectionHandler(new SelectionHandler(this))
{
//     KoShapeManager * manager = canvas->shapeManager();
//     connect(manager, SIGNAL(selectionChanged()), this, SLOT(updateActions()));
}

TreeTool::~TreeTool()
{
}

void TreeTool::activate(ToolActivation, const QSet<KoShape*> &)
{
    useCursor(Qt::ArrowCursor);
    koSelection()->deselectAll();
    repaintDecorations();
}

void TreeTool::paint(QPainter &painter, const KoViewConverter &converter)
{
    KoInteractionTool::paint(painter, converter);
    if (currentStrategy() == 0 && koSelection()->count() > 0) {
        SelectionDecorator decorator;
        decorator.setSelection(koSelection());
        decorator.paint(painter, converter);
    }
}

void TreeTool::mousePressEvent(KoPointerEvent *event)
{
    KoInteractionTool::mousePressEvent(event);
}

void TreeTool::mouseMoveEvent(KoPointerEvent *event)
{
    KoInteractionTool::mouseMoveEvent(event);
}

void TreeTool::mouseReleaseEvent(KoPointerEvent *event)
{
    KoInteractionTool::mouseReleaseEvent(event);
}

void TreeTool::mouseDoubleClickEvent(KoPointerEvent *event)
{
    Q_UNUSED(event);
    kDebug() << "doubleclick";
}

void TreeTool::keyPressEvent(QKeyEvent *event)
{
    KoInteractionTool::keyPressEvent(event);
    KoShape *root;
    switch (event->key()) {
        case Qt::Key_Tab:
            foreach (root, canvas()->shapeManager()->selection()->selectedShapes()){
                TreeShape *tree = dynamic_cast<TreeShape*>(root->parent());
                if (tree){
                    kDebug() << "Adding child...";
                    KoShapeController *controller = canvas()->shapeController();
                    QUndoCommand *command = new QUndoCommand;
                    foreach(KoShape* shape, tree->addNewChild()){
                        controller->addShapeDirect(shape, command);
                    }
                    canvas()->addCommand(command);
                }
            }
            event->accept();
            break;
        case Qt::Key_Return:
            foreach (root, canvas()->shapeManager()->selection()->selectedShapes()){
                TreeShape *tree = dynamic_cast<TreeShape*>(root->parent());
                if (tree)
                    if (tree = dynamic_cast<TreeShape*>(tree->parent())){
                        kDebug() << "Adding child...";
                        KoShapeController *controller = canvas()->shapeController();
                        QUndoCommand *command = new QUndoCommand;
                        foreach(KoShape* shape, tree->addNewChild()){
                            controller->addShapeDirect(shape, command);
                        }
                        canvas()->addCommand(command);
                    }
            }
            event->accept();
            break;
        case Qt::Key_Delete:
            foreach (root, canvas()->shapeManager()->selection()->selectedShapes()){
                TreeShape *tree = dynamic_cast<TreeShape*>(root->parent());
                if (tree){
                    KoShapeController *controller = canvas()->shapeController();
                    QUndoCommand *command = new QUndoCommand;
                    controller->removeShape(tree,command);
                    TreeShape *grandparent = dynamic_cast<TreeShape*>(tree->parent());
                    if (grandparent){
                        controller->removeShape(grandparent->connector(tree),command);
                    }
                    canvas()->addCommand(command);
                }
            }
            event->accept();
            break;
        default:
            return;
    }
}

KoToolSelection* TreeTool::selection()
{
    return m_selectionHandler;
}

void TreeTool::resourceChanged(int key, const QVariant & res)
{
    if (key == HotPosition) {
        m_hotPosition = static_cast<KoFlake::Position>(res.toInt());
        repaintDecorations();
    }
}

KoInteractionStrategy *TreeTool::createStrategy(KoPointerEvent *event)
{
    // reset the move by keys when a new strategy is created otherwise we might change the
    // command after a new command was added. This happend when you where faster than the timer.
    m_moveCommand = 0;

    KoShapeManager *shapeManager = canvas()->shapeManager();
    KoSelection *select = shapeManager->selection();

    bool selectMultiple = event->modifiers() & Qt::ControlModifier;
    bool selectNextInStack = event->modifiers() & Qt::ShiftModifier;

//     if ((event->buttons() == Qt::LeftButton) && !(selectMultiple || selectNextInStack)) {
//         const QPainterPath outlinePath = select->transformation().map(select->outline());
//         if (outlinePath.contains(event->point) ||
//             outlinePath.intersects(handlePaintRect(event->point))) {
//                 kDebug() << 1;
//                 return new TreeShapeMoveStrategy(this, event->point);
//         }
//     }

    if ((event->buttons() & Qt::LeftButton) == 0)
        return 0;  // Nothing to do for middle/right mouse button

    KoFlake::ShapeSelection sel;
    sel = selectNextInStack ? KoFlake::NextUnselected : KoFlake::ShapeOnTop;
    KoShape *shape = shapeManager->shapeAt(event->point, sel);

    if (!shape) {
        if (!selectMultiple) {
            repaintDecorations();
            select->deselectAll();
        }
        kDebug() << "KoShapeRubberSelectStrategy(this, event->point)";
        return new KoShapeRubberSelectStrategy(this, event->point);
    }

    if (select->isSelected(shape)) {
        kDebug() << "isSelected";
        if (selectMultiple) {
            repaintDecorations();
            select->deselect(shape);
            kDebug() << "deselecting already selected shape";
        }
    } else { // clicked on shape which is not selected
        repaintDecorations();
        if (!selectMultiple){
            kDebug() << "deselecting all";
            shapeManager->selection()->deselectAll();
        }
        select->select(shape, selectNextInStack ? false : true);
        kDebug() << "selecting shape and creating TreeShapeMoveStrategy";
        repaintDecorations();
        return new TreeShapeMoveStrategy(this, event->point);
    }
    return 0;
}

void TreeTool::repaintDecorations()
{
    Q_ASSERT(koSelection());
    if (koSelection()->count() > 0)
        canvas()->updateCanvas(koSelection()->boundingRect());
}

KoSelection *TreeTool::koSelection()
{
    Q_ASSERT(canvas());
    Q_ASSERT(canvas()->shapeManager());
    return canvas()->shapeManager()->selection();
}

#include <TreeTool.moc>
