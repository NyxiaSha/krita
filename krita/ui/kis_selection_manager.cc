/*
 *  Copyright (c) 2004 Boudewijn Rempt <boud@valdyas.org>
 *  Copyright (c) 2007 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  The outline algorith uses the limn algorithm of fontutils by
 *  Karl Berry <karl@cs.umb.edu> and Kathryn Hargreaves <letters@cs.umb.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_selection_manager.h"
#include "dialogs/kis_dlg_apply_profile.h" // If this isn't first, I get
                                   // errors in qslider.h. Weird... (BSAR)
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QTimer>

#include <kis_debug.h>
#include <kaction.h>
#include <ktoggleaction.h>
#include <klocale.h>
#include <kstandardaction.h>
#include <kactioncollection.h>

#include "KoChannelInfo.h"
#include "KoIntegerMaths.h"
#include <KoDocument.h>
#include <KoMainWindow.h>
#include <KoQueryTrader.h>
#include <KoViewConverter.h>
#include <KoSelection.h>
#include <KoShapeManager.h>
#include <KoLineBorder.h>
#include <KoColorSpace.h>
#include <KoCompositeOp.h>

#include "kis_adjustment_layer.h"
#include "canvas/kis_canvas2.h"
#include "kis_config.h"
#include "kis_convolution_painter.h"
#include "kis_debug.h"
#include "kis_doc2.h"
#include "kis_fill_painter.h"
#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_iterator_pixel_trait.h"
#include "kis_iterators_pixel.h"
#include "kis_layer.h"
#include "kis_statusbar.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_painter.h"
#include "kis_selected_transaction.h"
#include "kis_selection.h"
#include "kis_transaction.h"
#include "kis_types.h"
#include "kis_canvas_resource_provider.h"
#include "kis_undo_adapter.h"
#include "kis_pixel_selection.h"
#include "kis_shape_selection.h"
#include "commands/kis_selection_commands.h"

#include "kis_clipboard.h"
#include "kis_view2.h"


KisSelectionManager::KisSelectionManager(KisView2 * view, KisDoc2 * doc)
    : m_view(view),
      m_doc(doc),
      m_copy(0),
      m_cut(0),
      m_paste(0),
      m_pasteNew(0),
      m_cutToNewLayer(0),
      m_selectAll(0),
      m_deselect(0),
      m_clear(0),
      m_reselect(0),
      m_invert(0),
      m_toNewLayer(0),
      m_feather(0),
      m_smooth(0),
      m_load(0),
      m_save(0),
      m_fillForegroundColor(0),
      m_fillBackgroundColor(0),
      m_fillPattern(0)
{
    m_clipboard = KisClipboard::instance();

    QRgb white = QColor(Qt::white).rgb();
    QRgb black = QColor(Qt::black).rgb();

    for(int i=0; i<8; i++){
        QImage texture( 8, 8, QImage::Format_RGB32 );
        for(int y=0; y<8; y++)
            for(int x=0; x<8; x++)
                texture.setPixel(x, y, ((x+y+i)%8 < 4)? black : white);

        QBrush brush;
        brush.setTextureImage(texture);
        brushes << brush;
    }

    offset = 0;
    timer = new QTimer();

    // XXX: Make sure no timers are running all the time! We need to
    // provide a signal to tell the selection manager that we've got a
    // current selection now (global or local).
    connect(timer, SIGNAL(timeout()), this, SLOT(selectionTimerEvent()));

    KoSelection * selection = m_view->canvasBase()->globalShapeManager()->selection();
    Q_ASSERT( selection );
    connect(selection, SIGNAL(selectionChanged()), this, SLOT(shapeSelectionChanged()));
}

KisSelectionManager::~KisSelectionManager()
{
    while (!m_pluginActions.isEmpty())
        delete m_pluginActions.takeFirst();
}

void KisSelectionManager::setup(KActionCollection * collection)
{
    // XXX: setup shortcuts!

    m_cut = collection->addAction(KStandardAction::Cut,  "cut", this, SLOT(cut()));
    m_copy = collection->addAction(KStandardAction::Copy,  "copy", this, SLOT(copy()));
    m_paste = collection->addAction(KStandardAction::Paste,  "paste", this, SLOT(paste()));

    m_pasteNew  = new KAction(i18n("Paste into &New Image"), this);
    collection->addAction("paste_new", m_pasteNew );
    connect(m_pasteNew, SIGNAL(triggered()), this, SLOT(pasteNew()));

    m_selectAll = collection->addAction(KStandardAction::SelectAll,  "select_all", this, SLOT(selectAll()));

    m_deselect = collection->addAction(KStandardAction::Deselect,  "deselect", this, SLOT(deselect()));


    m_clear = collection->addAction(KStandardAction::Clear,  "clear", this, SLOT(clear()));

    m_reselect  = new KAction(i18n("&Reselect"), this);
    collection->addAction("reselect", m_reselect );
    m_reselect->setShortcut(QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_D));
    connect(m_reselect, SIGNAL(triggered()), this, SLOT(reselect()));

    m_invert  = new KAction(i18n("&Invert"), this);
    collection->addAction("invert", m_invert );
    m_invert->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_I));
    connect(m_invert, SIGNAL(triggered()), this, SLOT(invert()));

    m_toNewLayer  = new KAction(i18n("Copy Selection to New Layer"), this);
    collection->addAction("copy_selection_to_new_layer", m_toNewLayer );
    m_toNewLayer->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_J));
    connect(m_toNewLayer, SIGNAL(triggered()), this, SLOT(copySelectionToNewLayer()));

    m_cutToNewLayer  = new KAction(i18n("Cut Selection to New Layer"), this);
    collection->addAction("cut_selection_to_new_layer", m_cutToNewLayer );
    m_cutToNewLayer->setShortcut(QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_J));
    connect(m_cutToNewLayer, SIGNAL(triggered()), this, SLOT(cutToNewLayer()));

    m_feather  = new KAction(i18n("Feather"), this);
    collection->addAction("feather", m_feather );
    m_feather->setShortcut(QKeySequence(Qt::CTRL+Qt::ALT+Qt::Key_D));
    connect(m_feather, SIGNAL(triggered()), this, SLOT(feather()));

    m_fillForegroundColor  = new KAction(i18n("Fill with Foreground Color"), this);
    collection->addAction("fill_selection_foreground_color", m_fillForegroundColor );
    m_fillForegroundColor->setShortcut(QKeySequence(Qt::ALT+Qt::Key_Backspace));
    connect(m_fillForegroundColor, SIGNAL(triggered()), this, SLOT(fillForegroundColor()));

    m_fillBackgroundColor  = new KAction(i18n("Fill with Background Color"), this);
    collection->addAction("fill_selection_background_color", m_fillBackgroundColor );
    m_fillBackgroundColor->setShortcut(QKeySequence(Qt::Key_Backspace));
    connect(m_fillBackgroundColor, SIGNAL(triggered()), this, SLOT(fillBackgroundColor()));

    m_fillPattern  = new KAction(i18n("Fill with Pattern"), this);
    collection->addAction("fill_selection_pattern", m_fillPattern );
    connect(m_fillPattern, SIGNAL(triggered()), this, SLOT(fillPattern()));

    m_toggleDisplaySelection  = new KToggleAction(i18n("Display Selection"), this);
    collection->addAction("toggle_display_selection", m_toggleDisplaySelection );
    m_toggleDisplaySelection->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_H));
    connect(m_toggleDisplaySelection, SIGNAL(triggered()), this, SLOT(toggleDisplaySelection()));

    m_toggleDisplaySelection->setCheckedState(KGuiItem(i18n("Hide Selection")));
    m_toggleDisplaySelection->setChecked(true);

    m_smooth  = new KAction(i18n("Smooth..."), this);
    collection->addAction("smooth", m_smooth );
    connect(m_smooth, SIGNAL(triggered()), this, SLOT(smooth()));

//     m_load
//         = new KAction(i18n("Load..."),
//                   0, 0,
//                   this, SLOT(load()),
//                   collection, "load_selection");
//
//
//     m_save
//         = new KAction(i18n("Save As..."),
//                   0, 0,
//                   this, SLOT(save()),
//                   collection, "save_selection");

    QClipboard *cb = QApplication::clipboard();
    connect(cb, SIGNAL(dataChanged()), SLOT(clipboardDataChanged()));

}

void KisSelectionManager::clipboardDataChanged()
{
    updateGUI();
}


void KisSelectionManager::addSelectionAction(QAction * action)
{
    m_pluginActions.append(action);
}


void KisSelectionManager::updateGUI()
{
    Q_ASSERT(m_view);
    Q_ASSERT(m_clipboard);

    if (m_view == 0) {
        // "Eek, no parent!
        return;
    }

    if (m_clipboard == 0) {
        // Eek, no clipboard!
        return;
    }

    KisImageSP img = m_view->image();
    KisLayerSP l;
    KisPaintDeviceSP dev;

    bool enable = false;

    if (img && m_view->activeDevice() && m_view->activeLayer()) {
        l = m_view->activeLayer();

        enable = l && l->selection() && !l->locked() && l->visible();
#if 0 // XXX_SELECTION (how are we going to handle deselect and
      // reselectt now?
        if ( l->inherits( "KisAdjustmentLayer" )
        if(dev && !adjLayer)
            m_reselect->setEnabled( dev->selectionDeselected() );
        if (adjLayer) // There's no reselect for adjustment layers
            m_reselect->setEnabled(false);
#endif
    }

    m_cut->setEnabled(enable);
    m_cutToNewLayer->setEnabled(enable);
    m_selectAll->setEnabled(!img.isNull());
    m_deselect->setEnabled(enable);
    m_clear->setEnabled(enable);
    m_fillForegroundColor->setEnabled(enable);
    m_fillBackgroundColor->setEnabled(enable);
    m_fillPattern->setEnabled(enable);
    m_invert->setEnabled(enable);

    m_feather->setEnabled(enable);

    m_smooth->setEnabled(enable);
//    m_load->setEnabled(enable);
//    m_save->setEnabled(enable);


    if ( !m_pluginActions.isEmpty() ) {
        QListIterator<QAction *> i( m_pluginActions );

        while( i.hasNext() ) {
            i.next()->setEnabled(!img.isNull());
        }
    }

    // You can copy from locked layers and paste the clip into a new layer, even when
    // the current layer is locked.
    enable = false;
    if (img && l) {
        enable = l->selection() && l->visible();
    }

    m_copy->setEnabled(enable);
    m_paste->setEnabled(!img.isNull() && m_clipboard->hasClip());
    m_pasteNew->setEnabled(!img.isNull() && m_clipboard->hasClip());
    m_toNewLayer->setEnabled(enable);

    updateStatusBar();

}

void KisSelectionManager::updateStatusBar()
{
    if (m_view && m_view->statusBar()) {
        m_view->statusBar()->setSelection( m_view->image() );
    }
}

bool KisSelectionManager::selectionIsActive()
{
    KisImageSP img = m_view->image();
    if (img) {
        if ( m_view->selection() && ( m_view->selection()->hasPixelSelection() || m_view->selection()->hasShapeSelection())) {
            return true;
        }
    }

    return false;
}

void KisSelectionManager::selectionChanged()
{
    updateGUI();
    outline.clear();

    if ( KisSelectionSP selection = m_view->selection() ) {
        if(selection->hasPixelSelection() || selection->hasShapeSelection()) {
            if(!timer->isActive())
                timer->start ( 300 );
        }
        if(selection->hasPixelSelection()) {
            KisPixelSelectionSP getOrCreatePixelSelection = selection->getOrCreatePixelSelection();
            outline = getOrCreatePixelSelection->outline();
            updateSimpleOutline();
        }
    }
    else
        timer->stop();

    m_view->canvasBase()->updateCanvas();
}

void KisSelectionManager::updateSimpleOutline()
{
    simpleOutline.clear();
    foreach(QPolygon polygon, outline) {
        QPolygon simplePolygon;

        simplePolygon << polygon.at(0);
        QPoint previousDelta = polygon.at(1) - polygon.at(0);
        QPoint currentDelta;
        int pointsSinceLastRemoval = 3;
        for (int i = 1; i < polygon.size()-1; ++i) {

            //check for left turns and turn them into diagonals
            currentDelta = polygon.at(i+1) - polygon.at(i);
            if( (previousDelta.y() == 1 && currentDelta.x() == 1) || (previousDelta.x() == -1 && currentDelta.y() == 1) ||
                (previousDelta.y() == -1 && currentDelta.x() == -1) || (previousDelta.x() == 1 && currentDelta.y() == -1) )
            {
                //Turning point found. The point at position i won't be in the simple outline.
                //If there is a staircase, the points in between will be removed.
                if(pointsSinceLastRemoval == 2)
                    simplePolygon.pop_back();
                pointsSinceLastRemoval = 0;

            }
            else
                simplePolygon << polygon.at(i);

            previousDelta = currentDelta;
            pointsSinceLastRemoval++;
        }
        simplePolygon << polygon.at(polygon.size()-1);

        simpleOutline.push_back(simplePolygon);
    }
}

void KisSelectionManager::cut()
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisLayerSP layer = m_view->activeLayer();
    if (!layer) return;

    if ( !m_view->selection() ) return;

    copy();

    KisSelectedTransaction *t = 0;

    if (img->undo()) {
        t = new KisSelectedTransaction(i18n("Cut"), layer);
        Q_CHECK_PTR(t);
    }
#if 0 // XXX_SELECTION
    dev->clearSelection();
    dev->deselect();
#endif
    if (img->undo()) {
        img->undoAdapter()->addCommand(t);
    }
}

void KisSelectionManager::copy()
{
    KisImageSP img = m_view->image();
    if ( !img ) return;

    if ( !m_view->selection() ) return;

    KisPaintDeviceSP dev = m_view->activeDevice();
    if (!dev) return;

    KisSelectionSP selection = m_view->selection();

    QRect r = selection->selectedExactRect();

    KisPaintDeviceSP clip = new KisPaintDevice(dev->colorSpace(), "clip");
    Q_CHECK_PTR(clip);

    const KoColorSpace * cs = clip->colorSpace();

    // TODO if the source is linked... copy from all linked layers?!?

    // Copy image data
    KisPainter gc;
    gc.begin(clip);
    gc.bitBlt(0, 0, COMPOSITE_COPY, dev, r.x(), r.y(), r.width(), r.height());
    gc.end();

    // Apply selection mask.

    KisHLineIteratorPixel layerIt = clip->createHLineIterator(0, 0, r.width());
    KisHLineConstIteratorPixel selectionIt = selection->createHLineIterator(r.x(), r.y(), r.width());

    for (qint32 y = 0; y < r.height(); y++) {

        while (!layerIt.isDone()) {

            cs->applyAlphaU8Mask( layerIt.rawData(), selectionIt.rawData(), 1 );


            ++layerIt;
            ++selectionIt;
        }
        layerIt.nextRow();
        selectionIt.nextRow();
    }

    m_clipboard->setClip(clip);
    selectionChanged();
}


KisLayerSP KisSelectionManager::paste()
{
    KisImageSP img = m_view->image();
    if (!img) return KisLayerSP(0);

    KisPaintDeviceSP clip = m_clipboard->clip();

    if (clip) {
        KisPaintLayer *layer = new KisPaintLayer(img.data(), img->nextLayerName() + i18n("(pasted)"), OPACITY_OPAQUE);
        Q_CHECK_PTR(layer);

        QRect r = clip->exactBounds();
        KisPainter gc;
        gc.begin(layer->paintDevice());
        gc.bitBlt(0, 0, COMPOSITE_COPY, clip, r.x(), r.y(), r.width(), r.height());
        gc.end();

        //figure out where to position the clip
        // XXX: Fix this for internal points & zoom! (BSAR)
        QWidget * w = m_view->canvas();
        QPoint center = QPoint(w->width()/2, w->height()/2);
        QPoint bottomright = QPoint(w->width(), w->height());
        if(bottomright.x() > img->width())
            center.setX(img->width()/2);
        if(bottomright.y() > img->height())
            center.setY(img->height()/2);
        center -= QPoint(r.width()/2, r.height()/2);
        layer->setX(center.x());
        layer->setY(center.y());

/*XXX CBR have an idea of asking the user if he is about to paste a clip in another cs than that of
  the image if that is what he want rather than silently converting
  if ( ! ( *clip->colorSpace == *img ->colorSpace()) )
  if (dlg->exec() == QDialog::Accepted)
  layer->convertTo(img->colorSpace());
*/
	if(!img->addNode( layer ,
                          m_view->activeLayer()->parent(),
                          m_view->activeLayer().data() ) ) {
            return 0;
        }

        return KisLayerSP(layer);
    }
    return KisLayerSP(0);
}

void KisSelectionManager::pasteNew()
{
    KisPaintDeviceSP clip = m_clipboard->clip();
    if (!clip) return;

    QRect r = clip->exactBounds();
    if (r.width() < 1 && r.height() < 1) {
        // Don't paste empty clips
        return;
    }

    const QByteArray mimetype = KoDocument::readNativeFormatMimeType();
    KoDocumentEntry entry = KoDocumentEntry::queryByMimeType( mimetype );

    KisDoc2 * doc = dynamic_cast<KisDoc2*>(  entry.createDoc() );
    if ( !doc ) return;

    Q_ASSERT(doc->undoAdapter() != 0);
    doc->undoAdapter()->setUndo(false);

    KisImageSP img = new KisImage(doc->undoAdapter(), r.width(), r.height(), clip->colorSpace(), "Pasted");
    KisPaintLayerSP layer = new KisPaintLayer(img.data(), clip->objectName(), OPACITY_OPAQUE, clip->colorSpace());

    KisPainter p(layer->paintDevice());
    p.bitBlt(0, 0, COMPOSITE_COPY, clip, OPACITY_OPAQUE, r.x(), r.y(), r.width(), r.height());
    p.end();

    img->addNode(layer.data(), img->rootLayer());
    doc->setCurrentImage(img);

    doc->undoAdapter()->setUndo(true);

    KoMainWindow *win = new KoMainWindow( doc->componentData() );
    win->show();
    win->setRootDocument( doc );
}

void KisSelectionManager::selectAll()
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisLayerSP layer = m_view->activeLayer();
    if(!layer) return;

    QUndoCommand* selectionCmd = new QUndoCommand(i18n("Select All"));

    if (!m_view->selection())
        new KisSetGlobalSelectionCommand(img, selectionCmd);
    KisSelectionSP selection = m_view->selection();

    KisSelectedTransaction * t = new KisSelectedTransaction(QString(), layer, selectionCmd);
    Q_CHECK_PTR(t);

    selection->getOrCreatePixelSelection()->select(img->bounds());

    m_view->selectionManager()->selectionChanged();
    m_view->document()->addCommand(selectionCmd);
}

void KisSelectionManager::deselect()
{

    // XXX_SELECTION
    KisImageSP img = m_view->image();
    if (!img) return;

    KisSelectionSP sel = m_view->selection();
    if (!sel) return;
#if 0
    KisSelectedTransaction * t = 0;
    if (img->undo()) t = new KisSelectedTransaction(i18n("Deselect"), dev);
    Q_CHECK_PTR(t);
#endif
    sel->clear();
#if 0
    if (img->undo())
        img->undoAdapter()->addCommand(t);
#endif
}


void KisSelectionManager::clear()
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisSelectionSP sel = m_view->selection();
    if (!sel) return;

#if 0 // XXX_SELECTION
    KisTransaction * t = 0;

    if (img->undo()) {
        t = new KisTransaction(i18n("Clear"), dev);
    }
#endif

    sel->clear();

#if 0
    if (img->undo()) img->undoAdapter()->addCommand(t);
#endif    
}

void KisSelectionManager::fill(const KoColor& color, bool fillWithPattern, const QString& transactionText)
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisPaintDeviceSP dev = m_view->activeDevice();
    if (!dev) return;

    KisSelectionSP selection = m_view->selection();
    if ( !selection ) return;

    KisPaintDeviceSP filled = new KisPaintDevice(dev->colorSpace());

    KisFillPainter painter(filled);

    if (fillWithPattern) {
        painter.fillRect(0, 0, img->width(), img->height(),
                         m_view->resourceProvider()->currentPattern());
    } else {
        painter.fillRect(0, 0, img->width(), img->height(), color);
    }

    painter.end();

    KisPainter painter2(dev, selection);

    if (img->undo()) painter2.beginTransaction(transactionText);
    painter2.bltSelection(0, 0, COMPOSITE_OVER, filled, OPACITY_OPAQUE,
                          0, 0, img->width(), img->height());

    dev->setDirty();

    if (img->undo()) {
        img->undoAdapter()->addCommand(painter2.endTransaction());
    }
}

void KisSelectionManager::fillForegroundColor()
{
    fill(m_view->resourceProvider()->fgColor(), false, i18n("Fill with Foreground Color"));
}

void KisSelectionManager::fillBackgroundColor()
{
    fill(m_view->resourceProvider()->bgColor(), false, i18n("Fill with Background Color"));
}

void KisSelectionManager::fillPattern()
{
    fill(KoColor(), true, i18n("Fill with Pattern"));
}

void KisSelectionManager::reselect()
{

#if 0 // XXX_SELECTION
    KisImageSP img = m_view->image();
    if (!img) return;

    KisPaintDeviceSP dev = m_view->activeDevice();
    if (!dev) return;

    KisSelectedTransaction * t = 0;
    if (img->undo()) t = new KisSelectedTransaction(i18n("&Reselect"), dev);
    Q_CHECK_PTR(t);

    dev->reselect(); // sets hasSelection=true
    dev->setDirty(img->bounds());

    if (img->undo())
        img->undoAdapter()->addCommand(t);
#endif
}


void KisSelectionManager::invert()
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisSelectionSP selection = m_view->selection();
    if ( !selection ) return;

    KisLayerSP layer = m_view->activeLayer();
    if(!layer) return;

    KisPixelSelectionSP s = selection->getOrCreatePixelSelection();

    KisSelectedTransaction * t = new KisSelectedTransaction(i18n("Invert"), layer);
    Q_CHECK_PTR(t);

    s->invert();
    s->setDirty(img->bounds());

    m_view->selectionManager()->selectionChanged();
    m_view->document()->addCommand(t);
}

void KisSelectionManager::copySelectionToNewLayer()
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisLayerSP layer = m_view->activeLayer();
    if (!layer) return;

    copy();
    paste();
}

void KisSelectionManager::cutToNewLayer()
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisPaintDeviceSP dev = m_view->activeDevice();
    if (!dev) return;

    cut();
    paste();
}


void KisSelectionManager::feather()
{
#if 0 // XXX_SELECTION
    KisImageSP img = m_view->image();
    if (!img) return;
    if (!m_view->selection()) {
        // activate it, but don't do anything with it
        dev->selection();

        return;
    }

    KisPixelSelectionSP selection = m_view->selection()->getOrCreatePixelSelection();
    KisSelectedTransaction * t = 0;
    if (img->undo()) t = new KisSelectedTransaction(i18n("Feather..."), dev);
    Q_CHECK_PTR(t);


    // XXX: we should let gaussian blur & others influence alpha channels as well
    // (on demand of the caller)

    KisConvolutionPainter painter(KisPaintDeviceSP(selection.data()));

    KisKernelSP k = KisKernelSP(new KisKernel());
    k->width = 3;
    k->height = 3;
    k->factor = 16;
    k->offset = 0;
    k->data = new qint32[9];
    k->data[0] = 1;
    k->data[1] = 2;
    k->data[2] = 1;
    k->data[3] = 2;
    k->data[4] = 4;
    k->data[5] = 2;
    k->data[6] = 1;
    k->data[7] = 2;
    k->data[8] = 1;

    QRect rect = selection->selectedExactRect();
    // Make sure we've got enough space around the edges.
    rect = QRect(rect.x() - 3, rect.y() - 3, rect.width() + 6, rect.height() + 6);
    rect &= QRect(0, 0, img->width(), img->height());
    painter.setChannelFlags( selection->colorSpace()->channelFlags( false, true, false, false ) );
    painter.applyMatrix(k, rect.x(), rect.y(), rect.width(), rect.height(), BORDER_AVOID);
    painter.end();
#if 0
    dev->setDirty(img->bounds());
#endif
    if (img->undo())
        img->undoAdapter()->addCommand(t);

#endif
}

void KisSelectionManager::toggleDisplaySelection()
{
    // XXX_SELECTION: Re-activate later! (BSAR)
    //m_view->selectionDisplayToggled(displaySelection());
}

bool KisSelectionManager::displaySelection()
{
    return m_toggleDisplaySelection->isChecked();
}

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

void KisSelectionManager::grow (qint32 xradius, qint32 yradius)
{
    KisImageSP img = m_view->image();
    if (!img) return;

    if ( !m_view->selection() ) return;
    KisPixelSelectionSP selection = m_view->selection()->getOrCreatePixelSelection();

    //determine the layerSize
    QRect layerSize = img->bounds();

    /*
      Any bugs in this function are probably also in thin_region
    */

    quint8  **buf;  // caches the region's pixel data
    quint8  **max;  // caches the largest values for each column

    if (xradius <= 0 || yradius <= 0)
        return;
#if 0
    KisSelectedTransaction *t = 0;

    if (img->undo()) {
        t = new KisSelectedTransaction(i18n("Grow"), dev);
        Q_CHECK_PTR(t);
    }
#endif
    max = new quint8* [layerSize.width() + 2 * xradius];
    buf = new quint8* [yradius + 1];
    for (qint32 i = 0; i < yradius + 1; i++)
    {
        buf[i] = new quint8[layerSize.width()];
    }
    quint8* buffer = new quint8[ ( layerSize.width() + 2 * xradius ) * ( yradius + 1 ) ];
    for (qint32 i = 0; i < layerSize.width() + 2 * xradius; i++)
    {
        if (i < xradius)
            max[i] = buffer;
        else if (i < layerSize.width() + xradius)
            max[i] = &buffer[(yradius + 1) * (i - xradius)];
        else
            max[i] = &buffer[(yradius + 1) * (layerSize.width() + xradius - 1)];

        for (qint32 j = 0; j < xradius + 1; j++)
            max[i][j] = 0;
    }
    /* offset the max pointer by xradius so the range of the array
       is [-xradius] to [region->w + xradius] */
    max += xradius;

    quint8* out = new quint8[ layerSize.width() ]; // holds the new scan line we are computing

    qint32* circ = new qint32[ 2 * xradius + 1 ]; // holds the y coords of the filter's mask
    computeBorder (circ, xradius, yradius);

    /* offset the circ pointer by xradius so the range of the array
       is [-xradius] to [xradius] */
    circ += xradius;

    memset (buf[0], 0, layerSize.width());
    for (qint32 i = 0; i < yradius && i < layerSize.height(); i++) // load top of image
    {
        selection->readBytes(buf[i + 1], layerSize.x(), layerSize.y() + i, layerSize.width(), 1);
    }

    for (qint32 x = 0; x < layerSize.width() ; x++) // set up max for top of image
    {
        max[x][0] = 0;         // buf[0][x] is always 0
        max[x][1] = buf[1][x]; // MAX (buf[1][x], max[x][0]) always = buf[1][x]
        for (qint32 j = 2; j < yradius + 1; j++)
        {
            max[x][j] = MAX(buf[j][x], max[x][j-1]);
        }
    }

    for (qint32 y = 0; y < layerSize.height(); y++)
    {
        rotatePointers (buf, yradius + 1);
        if (y < layerSize.height() - (yradius))
            selection->readBytes(buf[yradius], layerSize.x(), layerSize.y() + y + yradius, layerSize.width(), 1);
        else
            memset (buf[yradius], 0, layerSize.width());
        for (qint32 x = 0; x < layerSize.width(); x++) /* update max array */
        {
            for (qint32 i = yradius; i > 0; i--)
            {
                max[x][i] = MAX (MAX (max[x][i - 1], buf[i - 1][x]), buf[i][x]);
            }
            max[x][0] = buf[0][x];
        }
        qint32 last_max = max[0][circ[-1]];
        qint32 last_index = 1;
        for (qint32 x = 0; x < layerSize.width(); x++) /* render scan line */
        {
            last_index--;
            if (last_index >= 0)
            {
                if (last_max == 255)
                    out[x] = 255;
                else
                {
                    last_max = 0;
                    for (qint32 i = xradius; i >= 0; i--)
                        if (last_max < max[x + i][circ[i]])
                        {
                            last_max = max[x + i][circ[i]];
                            last_index = i;
                        }
                    out[x] = last_max;
                }
            }
            else
            {
                last_index = xradius;
                last_max = max[x + xradius][circ[xradius]];
                for (qint32 i = xradius - 1; i >= -xradius; i--)
                    if (last_max < max[x + i][circ[i]])
                    {
                        last_max = max[x + i][circ[i]];
                        last_index = i;
                    }
                out[x] = last_max;
            }
        }
        selection->writeBytes(out, layerSize.x(), layerSize.y() + y, layerSize.width(), 1);
    }
    /* undo the offsets to the pointers so we can free the malloced memmory */
    circ -= xradius;
    max -= xradius;
    //XXXX: replace delete by delete[] where it is necessary to avoid memory leaks!
    delete[] circ;
    delete[] buffer;
    delete[] max;
    for (qint32 i = 0; i < yradius + 1; i++)
        delete[] buf[i];
    delete[] buf;
    delete[] out;
#if 0
    dev->setDirty(img->bounds());

    if (t) {
        img->undoAdapter()->addCommand(t);
    }
#endif
}

void KisSelectionManager::shrink (qint32 xradius, qint32 yradius, bool edge_lock)
{

    KisImageSP img = m_view->image();
    if (!img) return;

    if ( !m_view->selection() ) return;
    KisPixelSelectionSP selection = m_view->selection()->getOrCreatePixelSelection();
#if 0
    KisSelectedTransaction *t = new KisSelectedTransaction(i18n("Shrink"), dev);
    Q_CHECK_PTR(t);
#endif

    //determine the layerSize
    QRect layerSize = img->bounds();
    /*
      pretty much the same as fatten_region only different
      blame all bugs in this function on jaycox@gimp.org
    */
    /* If edge_lock is true  we assume that pixels outside the region
       we are passed are identical to the edge pixels.
       If edge_lock is false, we assume that pixels outside the region are 0
    */
    quint8  **buf;  // caches the the region's pixels
    quint8  **max;  // caches the smallest values for each column
    qint32    last_max, last_index;

    if (xradius <= 0 || yradius <= 0)
        return;

    max = new quint8* [layerSize.width() + 2 * xradius];
    buf = new quint8* [yradius + 1];
    for (qint32 i = 0; i < yradius + 1; i++)
    {
        buf[i] = new quint8[layerSize.width()];
    }

    qint32 buffer_size = (layerSize.width() + 2 * xradius + 1) * (yradius + 1);
    quint8* buffer = new quint8[buffer_size];

    if (edge_lock)
        memset(buffer, 255, buffer_size);
    else
        memset(buffer, 0, buffer_size);

    for (qint32 i = 0; i < layerSize.width() + 2 * xradius; i++)
    {
        if (i < xradius)
            if (edge_lock)
                max[i] = buffer;
            else
                max[i] = &buffer[(yradius + 1) * (layerSize.width() + xradius)];
        else if (i < layerSize.width() + xradius)
            max[i] = &buffer[(yradius + 1) * (i - xradius)];
        else
            if (edge_lock)
                max[i] = &buffer[(yradius + 1) * (layerSize.width() + xradius - 1)];
            else
                max[i] = &buffer[(yradius + 1) * (layerSize.width() + xradius)];
    }
    if (!edge_lock)
        for (qint32 j = 0 ; j < xradius + 1; j++) max[0][j] = 0;

    // offset the max pointer by xradius so the range of the array is [-xradius] to [region->w + xradius]
    max += xradius;

    quint8* out = new quint8[layerSize.width()]; // holds the new scan line we are computing

    qint32* circ = new qint32[2 * xradius + 1]; // holds the y coords of the filter's mask

    computeBorder (circ, xradius, yradius);

    // offset the circ pointer by xradius so the range of the array is [-xradius] to [xradius]
    circ += xradius;

    for (qint32 i = 0; i < yradius && i < layerSize.height(); i++) // load top of image
        selection->readBytes(buf[i + 1], layerSize.x(), layerSize.y() + i, layerSize.width(), 1);

    if (edge_lock)
        memcpy (buf[0], buf[1], layerSize.width());
    else
        memset (buf[0], 0, layerSize.width());


    for (qint32 x = 0; x < layerSize.width(); x++) // set up max for top of image
    {
        max[x][0] = buf[0][x];
        for (qint32 j = 1; j < yradius + 1; j++)
            max[x][j] = MIN(buf[j][x], max[x][j-1]);
    }

    for (qint32 y = 0; y < layerSize.height(); y++)
    {
        rotatePointers (buf, yradius + 1);
        if (y < layerSize.height() - yradius)
            selection->readBytes(buf[yradius], layerSize.x(), layerSize.y() + y + yradius, layerSize.width(), 1);
        else if (edge_lock)
            memcpy (buf[yradius], buf[yradius - 1], layerSize.width());
        else
            memset (buf[yradius], 0, layerSize.width());

        for (qint32 x = 0 ; x < layerSize.width(); x++) // update max array
        {
            for (qint32 i = yradius; i > 0; i--)
            {
                max[x][i] = MIN (MIN (max[x][i - 1], buf[i - 1][x]), buf[i][x]);
            }
            max[x][0] = buf[0][x];
        }
        last_max =  max[0][circ[-1]];
        last_index = 0;

        for (qint32 x = 0 ; x < layerSize.width(); x++) // render scan line
        {
            last_index--;
            if (last_index >= 0)
            {
                if (last_max == 0)
                    out[x] = 0;
                else
                {
                    last_max = 255;
                    for (qint32 i = xradius; i >= 0; i--)
                        if (last_max > max[x + i][circ[i]])
                        {
                            last_max = max[x + i][circ[i]];
                            last_index = i;
                        }
                    out[x] = last_max;
                }
            }
            else
            {
                last_index = xradius;
                last_max = max[x + xradius][circ[xradius]];
                for (qint32 i = xradius - 1; i >= -xradius; i--)
                    if (last_max > max[x + i][circ[i]])
                    {
                        last_max = max[x + i][circ[i]];
                        last_index = i;
                    }
                out[x] = last_max;
            }
        }
        selection->writeBytes(out, layerSize.x(), layerSize.y() + y, layerSize.width(), 1);
    }

    // undo the offsets to the pointers so we can free the malloced memmory
    circ -= xradius;
    max -= xradius;
    //free the memmory
    //XXXX: replace delete by delete[] where it is necessary to avoid memory leaks!
    delete[] circ;
    delete[] buffer;
    delete[] max;
    for (qint32 i = 0; i < yradius + 1; i++)
        delete buf[i];
    delete[] buf;
    delete[] out;
#if 0
    img->undoAdapter()->addCommand(t);
    dev->setDirty(img->bounds());
    dev->emitSelectionChanged();
#endif
}

//Simple convolution filter to smooth a mask (1bpp)

void KisSelectionManager::smooth()
{
    KisImageSP img = m_view->image();
    if ( !img ) return;

    if ( !m_view->selection() ) return;
    if ( !m_view->activeLayer() ) return;
    KisPixelSelectionSP selection = m_view->selection()->getOrCreatePixelSelection();

    //determine the layerSize
    QRect layerSize = m_view->activeLayer()->exactBounds();

    quint8      *buf[3];

    qint32 width = layerSize.width();

    for (qint32 i = 0; i < 3; i++) buf[i] = new quint8[width + 2];

    quint8* out = new quint8[width];

    // load top of image
    selection->readBytes(buf[0] + 1, layerSize.x(), layerSize.y(), width, 1);

    buf[0][0]         = buf[0][1];
    buf[0][width + 1] = buf[0][width];

    memcpy (buf[1], buf[0], width + 2);

    for (qint32 y = 0; y < layerSize.height(); y++)
    {
        if (y + 1 < layerSize.height())
        {
            selection->readBytes(buf[2] + 1, layerSize.x(), layerSize.y() + y + 1, width, 1);

            buf[2][0]         = buf[2][1];
            buf[2][width + 1] = buf[2][width];
        }
        else
        {
            memcpy (buf[2], buf[1], width + 2);
        }

        for (qint32 x = 0 ; x < width; x++)
        {
            qint32 value = (buf[0][x] + buf[0][x+1] + buf[0][x+2] +
                            buf[1][x] + buf[2][x+1] + buf[1][x+2] +
                            buf[2][x] + buf[1][x+1] + buf[2][x+2]);

            out[x] = value / 9;
        }

        selection->writeBytes(out, layerSize.x(), layerSize.y() + y, width, 1);

        rotatePointers (buf, 3);
    }

    for (qint32 i = 0; i < 3; i++)
        delete[] buf[i];

    delete[] out;
#if 0
    dev->setDirty(img->bounds());
#endif
}

// Erode (radius 1 pixel) a mask (1bpp)

void KisSelectionManager::erode()
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisSelectionSP selection = m_view->selection();
    if (!selection) return;

    KisLayerSP layer = m_view->activeLayer();
    //determine the layerSize
    QRect layerSize = layer->exactBounds();

    quint8* buf[3];


    qint32 width = layerSize.width();

    for (qint32 i = 0; i < 3; i++)
        buf[i] = new quint8[width + 2];

    quint8* out = new quint8[width];

    // load top of image
    selection->readBytes(buf[0] + 1, layerSize.x(), layerSize.y(), width, 1);

    buf[0][0]         = buf[0][1];
    buf[0][width + 1] = buf[0][width];

    memcpy (buf[1], buf[0], width + 2);

    for (qint32 y = 0; y < layerSize.height(); y++)
    {
        if (y + 1 < layerSize.height())
        {
            selection->readBytes(buf[2] + 1, layerSize.x(), layerSize.y() + y + 1, width, 1);

            buf[2][0]         = buf[2][1];
            buf[2][width + 1] = buf[2][width];
        }
        else
        {
            memcpy (buf[2], buf[1], width + 2);
        }

        for (qint32 x = 0 ; x < width; x++)
        {
            qint32 min = 255;

            if (buf[0][x+1] < min) min = buf[0][x+1];
            if (buf[1][x]   < min) min = buf[1][x];
            if (buf[1][x+1] < min) min = buf[1][x+1];
            if (buf[1][x+2] < min) min = buf[1][x+2];
            if (buf[2][x+1] < min) min = buf[2][x+1];

            out[x] = min;
        }

        selection->writeBytes(out, layerSize.x(), layerSize.y() + y, width, 1);

        rotatePointers (buf, 3);
    }

    for (qint32 i = 0; i < 3; i++)
        delete[] buf[i];

    delete[] out;
#if 0
    dev->setDirty();
#endif
}

// dilate (radius 1 pixel) a mask (1bpp)

void KisSelectionManager::dilate()
{
    KisImageSP img = m_view->image();
    if (!img) return;

    KisSelectionSP selection = m_view->selection();
    if ( !selection ) return;

    KisLayerSP layer = m_view->activeLayer();
    if ( !layer ) return;
    //determine the layerSize
    QRect layerSize = layer->exactBounds();

    quint8* buf[3];

    qint32 width = layerSize.width();

    for (qint32 i = 0; i < 3; i++)
        buf[i] = new quint8[width + 2];

    quint8* out = new quint8[width];

    // load top of image
    selection->readBytes(buf[0] + 1, layerSize.x(), layerSize.y(), width, 1);

    buf[0][0]         = buf[0][1];
    buf[0][width + 1] = buf[0][width];

    memcpy (buf[1], buf[0], width + 2);

    for (qint32 y = 0; y < layerSize.height(); y++)
    {
        if (y + 1 < layerSize.height())
        {
            selection->readBytes(buf[2] + 1, layerSize.x(), layerSize.y() + y + 1, width, 1);

            buf[2][0]         = buf[2][1];
            buf[2][width + 1] = buf[2][width];
        }
        else
        {
            memcpy (buf[2], buf[1], width + 2);
        }

        for (qint32 x = 0 ; x < width; x++)
        {
            qint32 max = 0;

            if (buf[0][x+1] > max) max = buf[0][x+1];
            if (buf[1][x]   > max) max = buf[1][x];
            if (buf[1][x+1] > max) max = buf[1][x+1];
            if (buf[1][x+2] > max) max = buf[1][x+2];
            if (buf[2][x+1] > max) max = buf[2][x+1];

            out[x] = max;
        }

        selection->writeBytes(out, layerSize.x(), layerSize.y() + y, width, 1);

        rotatePointers (buf, 3);
    }

    for (qint32 i = 0; i < 3; i++)
        delete[] buf[i];

    delete[] out;

    layer->setDirty();
}

void KisSelectionManager::border(qint32 xradius, qint32 yradius)
{
    KisImageSP img = m_view->image();
    if (!img) return;

    if (!m_view->selection()) return;
    KisPixelSelectionSP selection = m_view->selection()->getOrCreatePixelSelection();

    //determine the layerSize
    QRect layerSize = img->bounds();
#if 0
    KisSelectedTransaction *t = new KisSelectedTransaction(i18n("Border"), dev);
    Q_CHECK_PTR(t);
#endif
    quint8  *buf[3];
    quint8 **density;
    quint8 **transition;

    if (xradius == 1 && yradius == 1) // optimize this case specifically
    {
        quint8* source[3];

        for (qint32 i = 0; i < 3; i++)
            source[i] = new quint8[layerSize.width()];

        quint8* transition = new quint8[layerSize.width()];

        selection->readBytes(source[0], layerSize.x(), layerSize.y(), layerSize.width(), 1);
        memcpy (source[1], source[0], layerSize.width());
        if (layerSize.height() > 1)
            selection->readBytes(source[2], layerSize.x(), layerSize.y() + 1, layerSize.width(), 1);
        else
            memcpy (source[2], source[1], layerSize.width());

        computeTransition (transition, source, layerSize.width());
        selection->writeBytes(transition, layerSize.x(), layerSize.y(), layerSize.width(), 1);

        for (qint32 y = 1; y < layerSize.height(); y++)
        {
            rotatePointers (source, 3);
            if (y + 1 < layerSize.height())
                selection->readBytes(source[2], layerSize.x(), layerSize.y() + y + 1, layerSize.width(), 1);
            else
                memcpy(source[2], source[1], layerSize.width());
            computeTransition (transition, source, layerSize.width());
            selection->writeBytes(transition, layerSize.x(), layerSize.y() + y, layerSize.width(), 1);
        }

        for (qint32 i = 0; i < 3; i++)
            delete[] source[i];
        delete[] transition;
#if 0
        img->undoAdapter()->addCommand(t);
        dev->setDirty(img->bounds());
#endif
        return;
    }

    qint32* max = new qint32[layerSize.width() + 2 * xradius];
    for (qint32 i = 0; i < (layerSize.width() + 2 * xradius); i++)
        max[i] = yradius + 2;
    max += xradius;

    for (qint32 i = 0; i < 3; i++)
        buf[i] = new quint8[layerSize.width()];

    transition = new quint8*[yradius + 1];
    for (qint32 i = 0; i < yradius + 1; i++)
    {
        transition[i] = new quint8[layerSize.width() + 2 * xradius];
        memset(transition[i], 0, layerSize.width() + 2 * xradius);
        transition[i] += xradius;
    }
    quint8* out = new quint8[layerSize.width()];
    density = new quint8*[2 * xradius + 1];
    density += xradius;

    for (qint32 x = 0; x < (xradius + 1); x++) // allocate density[][]
    {
        density[ x]  = new quint8[2 * yradius + 1];
        density[ x] += yradius;
        density[-x]  = density[x];
    }
    for (qint32 x = 0; x < (xradius + 1); x++) // compute density[][]
    {
        double tmpx, tmpy, dist;
        quint8 a;

        if (x > 0)
            tmpx = x - 0.5;
        else if (x < 0)
            tmpx = x + 0.5;
        else
            tmpx = 0.0;

        for (qint32 y = 0; y < (yradius + 1); y++)
        {
            if (y > 0)
                tmpy = y - 0.5;
            else if (y < 0)
                tmpy = y + 0.5;
            else
                tmpy = 0.0;
            dist = ((tmpy * tmpy) / (yradius * yradius) +
                    (tmpx * tmpx) / (xradius * xradius));
            if (dist < 1.0)
                a = (quint8)(255 * (1.0 - sqrt (dist)));
            else
                a = 0;
            density[ x][ y] = a;
            density[ x][-y] = a;
            density[-x][ y] = a;
            density[-x][-y] = a;
        }
    }
    selection->readBytes(buf[0], layerSize.x(), layerSize.y(), layerSize.width(), 1);
    memcpy (buf[1], buf[0], layerSize.width());
    if (layerSize.height() > 1)
        selection->readBytes(buf[2], layerSize.x(), layerSize.y() + 1, layerSize.width(), 1);
    else
        memcpy (buf[2], buf[1], layerSize.width());
    computeTransition (transition[1], buf, layerSize.width());

    for (qint32 y = 1; y < yradius && y + 1 < layerSize.height(); y++) // set up top of image
    {
        rotatePointers (buf, 3);
        selection->readBytes(buf[2], layerSize.x(), layerSize.y() + y + 1, layerSize.width(), 1);
        computeTransition (transition[y + 1], buf, layerSize.width());
    }
    for (qint32 x = 0; x < layerSize.width(); x++) // set up max[] for top of image
    {
        max[x] = -(yradius + 7);
        for (qint32 j = 1; j < yradius + 1; j++)
            if (transition[j][x])
            {
                max[x] = j;
                break;
            }
    }
    for (qint32 y = 0; y < layerSize.height(); y++) // main calculation loop
    {
        rotatePointers (buf, 3);
        rotatePointers (transition, yradius + 1);
        if (y < layerSize.height() - (yradius + 1))
        {
            selection->readBytes(buf[2], layerSize.x(), layerSize.y() + y + yradius + 1, layerSize.width(), 1);
            computeTransition (transition[yradius], buf, layerSize.width());
        }
        else
            memcpy (transition[yradius], transition[yradius - 1], layerSize.width());

        for (qint32 x = 0; x < layerSize.width(); x++) // update max array
        {
            if (max[x] < 1)
            {
                if (max[x] <= -yradius)
                {
                    if (transition[yradius][x])
                        max[x] = yradius;
                    else
                        max[x]--;
                }
                else
                    if (transition[-max[x]][x])
                        max[x] = -max[x];
                    else if (transition[-max[x] + 1][x])
                        max[x] = -max[x] + 1;
                    else
                        max[x]--;
            }
            else
                max[x]--;
            if (max[x] < -yradius - 1)
                max[x] = -yradius - 1;
        }
        quint8 last_max =  max[0][density[-1]];
        qint32 last_index = 1;
        for (qint32 x = 0 ; x < layerSize.width(); x++) // render scan line
        {
            last_index--;
            if (last_index >= 0)
            {
                last_max = 0;
                for (qint32 i = xradius; i >= 0; i--)
                    if (max[x + i] <= yradius && max[x + i] >= -yradius && density[i][max[x+i]] > last_max)
                    {
                        last_max = density[i][max[x + i]];
                        last_index = i;
                    }
                out[x] = last_max;
            }
            else
            {
                last_max = 0;
                for (qint32 i = xradius; i >= -xradius; i--)
                    if (max[x + i] <= yradius && max[x + i] >= -yradius && density[i][max[x + i]] > last_max)
                    {
                        last_max = density[i][max[x + i]];
                        last_index = i;
                    }
                out[x] = last_max;
            }
            if (last_max == 0)
            {
                qint32 i;
                for (i = x + 1; i < layerSize.width(); i++)
                {
                    if (max[i] >= -yradius)
                        break;
                }
                if (i - x > xradius)
                {
                    for (; x < i - xradius; x++)
                        out[x] = 0;
                    x--;
                }
                last_index = xradius;
            }
        }
        selection->writeBytes(out, layerSize.x(), layerSize.y() + y, layerSize.width(), 1);
    }
    delete [] out;

    for (qint32 i = 0; i < 3; i++)
        delete buf[i];

    max -= xradius;
    delete[] max;

    for (qint32 i = 0; i < yradius + 1; i++)
    {
        transition[i] -= xradius;
        delete transition[i];
    }
    delete[] transition;

    for (qint32 i = 0; i < xradius + 1 ; i++)
    {
        density[i] -= yradius;
        delete density[i];
    }
    density -= xradius;
    delete[] density;

#if 0
    img->undoAdapter()->addCommand(t);
    dev->setDirty(img->bounds());
#endif
}

#define RINT(x) floor ((x) + 0.5)

void KisSelectionManager::computeBorder (qint32  *circ, qint32  xradius, qint32  yradius)
{
    qint32 i;
    qint32 diameter = xradius * 2 + 1;
    double tmp;

    for (i = 0; i < diameter; i++)
    {
        if (i > xradius)
            tmp = (i - xradius) - 0.5;
        else if (i < xradius)
            tmp = (xradius - i) - 0.5;
        else
            tmp = 0.0;

        circ[i] = (qint32) RINT (yradius / (double) xradius * sqrt (xradius * xradius - tmp * tmp));
    }
}

void KisSelectionManager::rotatePointers (quint8  **p, quint32 n)
{
    quint32  i;
    quint8  *tmp;

    tmp = p[0];

    for (i = 0; i < n - 1; i++) p[i] = p[i + 1];

    p[i] = tmp;
}

void KisSelectionManager::computeTransition (quint8* transition, quint8** buf, qint32 width)
{
    qint32 x = 0;

    if (width == 1)
    {
        if (buf[1][x] > 127 && (buf[0][x] < 128 || buf[2][x] < 128))
            transition[x] = 255;
        else
            transition[x] = 0;
        return;
    }
    if (buf[1][x] > 127)
    {
        if ( buf[0][x] < 128 || buf[0][x + 1] < 128 ||
             buf[1][x + 1] < 128 ||
             buf[2][x] < 128 || buf[2][x + 1] < 128 )
            transition[x] = 255;
        else
            transition[x] = 0;
    }
    else
        transition[x] = 0;
    for (qint32 x = 1; x < width - 1; x++)
    {
        if (buf[1][x] >= 128)
        {
            if (buf[0][x - 1] < 128 || buf[0][x] < 128 || buf[0][x + 1] < 128 ||
                buf[1][x - 1] < 128           ||          buf[1][x + 1] < 128 ||
                buf[2][x - 1] < 128 || buf[2][x] < 128 || buf[2][x + 1] < 128)
                transition[x] = 255;
            else
                transition[x] = 0;
        }
        else
            transition[x] = 0;
    }
    if (buf[1][x] >= 128)
    {
        if (buf[0][x - 1] < 128 || buf[0][x] < 128 ||
            buf[1][x - 1] < 128 ||
            buf[2][x - 1] < 128 || buf[2][x] < 128)
            transition[x] = 255;
        else
            transition[x] = 0;
    }
    else
        transition[x] = 0;
}

void KisSelectionManager::selectionTimerEvent()
{
    KisSelectionSP selection = m_view->selection();
    if ( !selection ) return;

    if (selectionIsActive()) {
        KisPaintDeviceSP dev = m_view->activeDevice();
        if(dev) {
            offset++;
            if(offset>7) offset = 0;

            QRect bound = selection->selectedRect();
            double xRes = m_view->image()->xRes();
            double yRes = m_view->image()->yRes();
            QRectF rect( int(bound.left()) / xRes, int(bound.top()) / yRes,
                         int(1 + bound.right()) / xRes, int(1 + bound.bottom()) / yRes);
            m_view->canvasBase()->updateCanvas(rect);
        }
    }
}

void KisSelectionManager::shapeSelectionChanged()
{
    KoShapeManager* shapeManager = m_view->canvasBase()->globalShapeManager();

    KoSelection * selection = shapeManager->selection();
    QList<KoShape*> selectedShapes = selection->selectedShapes();

    KoLineBorder* border = new KoLineBorder(0, Qt::lightGray);
    foreach(KoShape* shape, shapeManager->shapes())
    {
        if(dynamic_cast<KisShapeSelection*>(shape->parent()))
        {
            if(selectedShapes.contains(shape))
                shape->setBorder(border);
            else
                shape->setBorder(0);
        }
    }
}

void KisSelectionManager::paint(QPainter& gc, const KoViewConverter &converter)
{
    KisSelectionSP selection = m_view->selection();

    double sx, sy;
    converter.zoom(&sx, &sy);

    if (selection && selection->hasPixelSelection()) {

        QMatrix matrix;
        matrix.scale(sx/m_view->image()->xRes(), sy/m_view->image()->yRes());

        QMatrix oldWorldMatrix = gc.worldMatrix();
        gc.setWorldMatrix( matrix, true);

        QTime t;
        t.start();
        gc.setRenderHints(0);

        QPen pen(brushes[offset], 0);

        int i=0;
        gc.setPen(pen);
        if(1/m_view->image()->xRes()*sx<3)
            foreach(QPolygon polygon, simpleOutline)
            {
                gc.drawPolygon(polygon);
                i++;
            }
        else
            foreach(QPolygon polygon, outline)
            {
                gc.drawPolygon(polygon);
                i++;
            }

        dbgRender <<"Polygons :" << i;
        dbgRender <<"Painting marching ants :" << t.elapsed();

        gc.setWorldMatrix( oldWorldMatrix);
    }
    if (selection && selection->hasShapeSelection()) {
        KisShapeSelection* shapeSelection = static_cast<KisShapeSelection*>(selection->shapeSelection());

        QVector<qreal> dashes;
        qreal space = 4;
        dashes << 4 << space;

        QPainterPathStroker stroker;
        stroker.setWidth(0);
        stroker.setDashPattern(dashes);
        stroker.setDashOffset(offset-4);

        gc.setRenderHint(QPainter::Antialiasing);
        QColor outlineColor = Qt::black;

        QMatrix zoomMatrix;
        zoomMatrix.scale(sx, sy);

        QPainterPath stroke = stroker.createStroke(zoomMatrix.map(shapeSelection->selectionOutline()));
        gc.fillPath(stroke, outlineColor);
    }
}

#include "kis_selection_manager.moc"
