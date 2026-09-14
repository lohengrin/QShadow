#include "videowidget.h"

#include <QOpenGLContext>
#include <QPainter>

VideoWidget::VideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
}

QImage VideoWidget::currentFrame() const
{
    QMutexLocker locker(&m_mutex);
    return m_frame;
}

void VideoWidget::setFrame(const QImage &frame)
{
    {
        QMutexLocker locker(&m_mutex);
        if (frame.isNull())
            return;
        m_frame = frame;
    }
    update();
}

void VideoWidget::setPreserveAspect(bool on)
{
    m_preserveAspect = on;
    update();
}

void VideoWidget::paintGL()
{
    QPainter painter(this);

    painter.fillRect(rect(), Qt::black);

    QMutexLocker locker(&m_mutex);
    if (m_frame.isNull())
        return;

    QRectF target(0, 0, width(), height());
    if (m_preserveAspect) {
        const QSizeF src(m_frame.width(), m_frame.height());
        const QSizeF scaled = src.scaled(size(), Qt::KeepAspectRatio);
        target = QRectF((width() - scaled.width()) / 2.0,
                        (height() - scaled.height()) / 2.0,
                        scaled.width(), scaled.height());
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.drawImage(target, m_frame);
}