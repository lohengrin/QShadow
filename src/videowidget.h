#pragma once

#include <QImage>
#include <QMutex>
#include <QSize>
#include <QVulkanWindow>

#include <atomic>

// Native-Vulkan display widget. Owns the QVulkanWindow plumbing and the
// thread-safe state the renderer consumes; all drawing happens on
// QVulkanWindow's render thread inside VulkanRenderer (see vulkanrenderer.h).
//
// The GUI thread only ever stores the newest decoded frame (drop-on-
// backpressure), exactly like the previous OpenGL widget did.
class VideoWidget : public QVulkanWindow {
    Q_OBJECT
public:
    enum class ShaderMode {
        None = 0,
        Bilinear,
        Hq4x,
        Xbrz,
        CrtGeom,
        CrtRoyale,
        // AMD FSR 1.0 (spatial) — presets are the reversible upscale ratios
        // of FSR 1.0, see README.
        FsrUltraQuality,
        FsrQuality,
        FsrBalanced,
        FsrPerformance,
        FsrSharpenOnly,
    };

    explicit VideoWidget(QWindow *parent = nullptr);
    ~VideoWidget() override;

    // State shared with the renderer. Fields are atomics so the GUI thread can
    // update them without locking while the render thread reads them.
    struct State {
        std::atomic<int> shaderMode{int(ShaderMode::None)};
        std::atomic<int> downscale{1};
        std::atomic<bool> preserveAspect{true};
    };

    QVulkanWindowRenderer *createRenderer() override;

public slots:
    void setFrame(const QImage &frame);     // may be called from any thread
    void setPreserveAspect(bool on);
    void setShaderMode(ShaderMode mode);
    void setDownscale(int factor);

private:
    // Renderer reads these; mutex only guards the pending frame image.
    bool takePendingFrame(QImage *out);

    friend class VulkanRenderer;

    State m_state;
    mutable QMutex m_frameMutex;
    QImage m_pending;
};