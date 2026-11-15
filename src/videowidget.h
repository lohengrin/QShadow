// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "videoframe.h"

#include <QMutex>
#include <QSize>
#include <QVulkanWindow>

#include <atomic>
#include <chrono>

// Native-Vulkan display widget. Owns the QVulkanWindow plumbing and the
// thread-safe state the renderer consumes; all drawing happens on
// QVulkanWindow's render thread inside VulkanRenderer (see vulkanrenderer.h).
//
// The GUI thread only ever stores the newest decoded frame (drop-on-
// backpressure), exactly like the previous OpenGL widget did.
class QMouseEvent;

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

    // Smoothed capture -> render-submitted latency in microseconds (0 until the
    // first frame). Excludes the display's scanout/vsync wait.
    int latencyUs() const { return m_latencyUs.load(std::memory_order_relaxed); }

    QVulkanWindowRenderer *createRenderer() override;

    // Hands the newest decoded frame to the renderer. Safe to call from any
    // thread (the decoder thread calls it directly, with no copy and no
    // queued payload); older undelivered frames are replaced.
    void submitFrame(const VideoFramePtr &frame,
                     std::chrono::steady_clock::time_point stamp = std::chrono::steady_clock::now());

    // True once the renderer can convert planar YUV frames on the GPU (and the
    // user has not forced the CPU path with QSHADOW_CPU_CONVERT=1). Thread-safe.
    bool gpuYuvSupported() const;
    void setGpuYuvSupported(bool supported); // renderer only

signals:
    // Right click on the video, at this global screen position.
    void contextMenuRequested(const QPoint &globalPos);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;

public slots:
    void setPreserveAspect(bool on);
    void setShaderMode(ShaderMode mode);
    void setDownscale(int factor);

private:
    // Renderer reads these; mutex only guards the pending frame image.
    bool takePendingFrame(VideoFramePtr *out, std::chrono::steady_clock::time_point *stamp);
    void noteSubmitted(std::chrono::steady_clock::time_point stamp);

    friend class VulkanRenderer;

    State m_state;
    std::atomic<bool> m_updatePosted{false};
    mutable QMutex m_frameMutex;
    VideoFramePtr m_pending;
    std::atomic<bool> m_gpuYuv{false};
    std::chrono::steady_clock::time_point m_pendingStamp;
    std::atomic<int> m_latencyUs{0};
};