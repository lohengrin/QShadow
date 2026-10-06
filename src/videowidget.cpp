#include "videowidget.h"

#include "vulkanrenderer.h"

#include <QMouseEvent>

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

void VideoWidget::submitFrame(const VideoFramePtr &frame,
                              std::chrono::steady_clock::time_point stamp)
{
    if (!frame)
        return;
    {
        QMutexLocker locker(&m_frameMutex);
        m_pending = frame;
        m_pendingStamp = stamp;
    }
    // requestUpdate() must run on the GUI thread; coalesce so at most one
    // wake-up event is ever queued, however fast frames arrive.
    if (!m_updatePosted.exchange(true)) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                m_updatePosted.store(false);
                requestUpdate();
            },
            Qt::QueuedConnection);
    }
}

void VideoWidget::mouseReleaseEvent(QMouseEvent *event)
{
    // The video is a native window inside the main window, so the widget layer
    // never sees its mouse events: forward the right click.
    if (event->button() == Qt::RightButton)
        emit contextMenuRequested(event->globalPosition().toPoint());
    else
        QVulkanWindow::mouseReleaseEvent(event);
}

bool VideoWidget::gpuYuvSupported() const
{
    static const bool forcedCpu = !qEnvironmentVariableIsEmpty("QSHADOW_CPU_CONVERT");
    return !forcedCpu && m_gpuYuv.load(std::memory_order_relaxed);
}

void VideoWidget::setGpuYuvSupported(bool supported)
{
    m_gpuYuv.store(supported, std::memory_order_relaxed);
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

void VideoWidget::noteSubmitted(std::chrono::steady_clock::time_point stamp)
{
    const int us = int(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - stamp).count());
    const int prev = m_latencyUs.load(std::memory_order_relaxed);
    // ~1 s smoothing at 60 fps.
    m_latencyUs.store(prev == 0 ? us : prev + (us - prev) / 60, std::memory_order_relaxed);
}

bool VideoWidget::takePendingFrame(VideoFramePtr *out, std::chrono::steady_clock::time_point *stamp)
{
    QMutexLocker locker(&m_frameMutex);
    if (!m_pending)
        return false;
    *stamp = m_pendingStamp;
    *out = std::move(m_pending);
    m_pending.reset();
    return true;
}