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

    // Canonical per-mode sizing. The renderer and the "Fit" toolbar action both
    // use these so the 1:1 target window size always matches the pipeline.
    static bool isFsrMode(int mode);
    static float fsrPresetRatio(int mode);
    // FSR output for a src frame inside a given target, capped by the target.
    static QSize fsrUpscaleSize(const QSize &src, const QSize &target, int mode);
    // The pipeline's native output size for a decoded frame in the current
    // mode/downscale, uncapped: what a 1:1 ("Fit") window should show.
    static QSize pipelineOutputSize(const QSize &frame, int mode, int downscale);
    static int roundToEven(float v);

    // State shared with the renderer. Fields are atomics so the GUI thread can
    // update them without locking while the render thread reads them.
    struct State {
        std::atomic<int> shaderMode{int(ShaderMode::None)};
        std::atomic<int> downscale{1};
        std::atomic<bool> preserveAspect{true};
    };

    QVulkanWindowRenderer *createRenderer() override;

    // Hands the newest decoded frame to the renderer. Safe to call from any
    // thread (the decoder thread calls it directly, with no copy and no
    // queued payload); older undelivered frames are replaced.
    void submitFrame(const QImage &frame);

public slots:
    void setFrame(const QImage &frame);     // GUI-thread convenience wrapper
    void setPreserveAspect(bool on);
    void setShaderMode(ShaderMode mode);
    void setDownscale(int factor);

private:
    // Renderer reads these; mutex only guards the pending frame image.
    bool takePendingFrame(QImage *out);

    friend class VulkanRenderer;

    State m_state;
    std::atomic<bool> m_updatePosted{false};
    mutable QMutex m_frameMutex;
    QImage m_pending;
};