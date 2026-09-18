#include "videowidget.h"

#include "vulkanrenderer.h"

VideoWidget::VideoWidget(QWindow *parent)
    : QVulkanWindow(parent)
{
}

VideoWidget::~VideoWidget() = default;

QVulkanWindowRenderer *VideoWidget::createRenderer()
{
    return new VulkanRenderer(this);
}

void VideoWidget::setFrame(const QImage &frame)
{
    if (frame.isNull())
        return;
    {
        QMutexLocker locker(&m_frameMutex);
        m_pending = frame;
    }
    requestUpdate();
}

void VideoWidget::setPreserveAspect(bool on)
{
    m_state.preserveAspect.store(on);
    requestUpdate();
}

void VideoWidget::setShaderMode(ShaderMode mode)
{
    m_state.shaderMode.store(int(mode));
    requestUpdate();
}

void VideoWidget::setDownscale(int factor)
{
    if (factor != 2 && factor != 4)
        factor = 1;
    m_state.downscale.store(factor);
    requestUpdate();
}

bool VideoWidget::takePendingFrame(QImage *out)
{
    QMutexLocker locker(&m_frameMutex);
    if (m_pending.isNull())
        return false;
    *out = m_pending;
    m_pending = QImage();
    return true;
}