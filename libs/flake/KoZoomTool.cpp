/*
 *  Copyright (c) 1999 Matthias Elter <me@kde.org>
 *  Copyright (c) 2002 Patrick Julien <freak@codepimps.org>
 *  Copyright (c) 2007 Casper Boemann <cbr@boemann.dk>
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

#include <kaction.h>
#include <klocale.h>
#include <kactioncollection.h>

#include "QPainter"
#include "KoPointerEvent.h"
#include "KoCanvasBase.h"
#include <kstandarddirs.h>
#include <kcursor.h>

#include "KoZoomTool.h"


KoZoomTool::KoZoomTool(KoCanvasBase *canvas)
    : super(canvas)
{
    setObjectName("tool_zoom");
    m_dragging = false;
    m_startPos = QPointF(0, 0);
    m_endPos = QPointF(0, 0);
    QPixmap plusPixmap, minusPixmap;
    plusPixmap.load(KStandardDirs::locate("data", "koffice/icons/tool_zoom_plus_cursor.png"));
    minusPixmap.load(KStandardDirs::locate("data", "koffice/icons/tool_zoom_minus_cursor.png"));
    m_plusCursor = KCursor::sizeVerCursor();//QCursor(plusPixmap);
    m_minusCursor = QCursor(minusPixmap);
    useCursor(m_plusCursor);
    connect(&m_timer, SIGNAL(timeout()), SLOT(slotTimer()));
}

KoZoomTool::~KoZoomTool()
{
}

void KoZoomTool::mousePressEvent(KoPointerEvent *e)
{
    if (!m_dragging) {
        if (e->button() == Qt::LeftButton) {
            m_startPos = e->pos();
            m_endPos = e->pos();
            m_dragging = true;
        }
    }
}

void KoZoomTool::mouseMoveEvent(KoPointerEvent *e)
{
    if (m_dragging) {
	QRectF bound;
        bound.setTopLeft(m_startPos);
        bound.setBottomRight(m_endPos);
        m_canvas->updateCanvas(bound.normalized());
        m_endPos = e->pos();
        bound.setBottomRight(m_endPos);
        m_canvas->updateCanvas(bound.normalized());
    }
}

void KoZoomTool::mouseReleaseEvent(KoPointerEvent *e)
{
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_endPos = e->pos();
        m_dragging = false;

        QPointF delta = m_endPos - m_startPos;
/*
        if (sqrt(delta.x() * delta.x() + delta.y() * delta.y()) < 10) {
            if (e->modifiers() & Qt::ControlModifier) {
                controller->zoomOut(m_endPos.x(), m_endPos.y());
            } else {
                controller->zoomIn(m_endPos.x(), m_endPos.y());
            }
        } else {
            controller->zoomTo(QRect(m_startPos, m_endPos));
        }
*/    }
}

void KoZoomTool::mouseDoubleClickEvent(KoPointerEvent *e)
{
}


void KoZoomTool::activate()
{
    if(m_controller == 0)
        emit sigDone();
    m_timer.start(50);
}

void KoZoomTool::deactivate()
{
    m_timer.stop();
}

void KoZoomTool::slotTimer()
{
/*    int state = QApplication::keyboardModifiers() & (Qt::ShiftModifier|Qt::ControlModifier|Qt::AltModifier);

    if (state & Qt::ControlModifier) {
        m_subject->canvasController()->setCanvasCursor(m_minusCursor);
    } else {
        m_subject->canvasController()->setCanvasCursor(m_plusCursor);
    }
*/
}


void KoZoomTool::paint(QPainter &painter, KoViewConverter &converter)
{
    if (m_canvas && m_dragging) {
        QPen old = painter.pen();
        QPen pen(Qt::DotLine);
        QPoint start;
        QPoint end;

        start = QPoint(static_cast<int>(m_startPos.x()), static_cast<int>(m_startPos.y()));
        end = QPoint(static_cast<int>(m_endPos.x()), static_cast<int>(m_endPos.y()));
        painter.drawRect(QRect(start, end));
        painter.setPen(old);
    }
}

#include "KoZoomTool.moc"
