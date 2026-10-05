#include "videowidget.h"

#include "vulkanrenderer.h"

#include <algorithm>

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

bool VideoWidget::isFsrMode(int mode)
{
    return mode >= int(ShaderMode::FsrUltraQuality);
}

float VideoWidget::fsrPresetRatio(int mode)
{
    switch (mode) {
    case int(ShaderMode::FsrUltraQuality):
        return 1.30f;
    case int(ShaderMode::FsrQuality):
        return 1.50f;
    case int(ShaderMode::FsrBalanced):
        return 1.70f;
    case int(ShaderMode::FsrPerformance):
        return 2.00f;
    default:
        return 1.0f;
    }
}

int VideoWidget::roundToEven(float v)
{
    int r = qRound(v);
    if (r & 1) {
        if (v > float(r))
            r += 1;
        else
            r -= 1;
    }
    return std::max(r, 2);
}

QSize VideoWidget::fsrUpscaleSize(const QSize &src, const QSize &target, int mode)
{
    if (mode == int(ShaderMode::FsrSharpenOnly))
        return src;
    const float ratio = fsrPresetRatio(mode);
    const int w =
        std::min(target.width(),
                 std::max(std::max(2, src.width()), roundToEven(float(src.width()) * ratio)));
    const int h =
        std::min(target.height(),
                 std::max(std::max(2, src.height()), roundToEven(float(src.height()) * ratio)));
    return QSize(w, h);
}

QSize VideoWidget::pipelineOutputSize(const QSize &frame, int mode, int downscale)
{
    QSize src = frame;
    if (downscale > 1)
        src = QSize(std::max(2, src.width() / downscale),
                    std::max(2, src.height() / downscale));

    if (isFsrMode(mode)) {
        if (mode == int(ShaderMode::FsrSharpenOnly))
            return src;
        return QSize(roundToEven(float(src.width()) * fsrPresetRatio(mode)),
                     roundToEven(float(src.height()) * fsrPresetRatio(mode)));
    }
    // hq4x is a fixed integer upscaler; everything else renders at the source
    // grid (xBRZ / CRT are free-resolution and sample at whatever the window
    // gives them, so 1:1 means one source pixel per window pixel).
    if (mode == int(ShaderMode::Hq4x))
        return QSize(src.width() * 4, src.height() * 4);
    return src;
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