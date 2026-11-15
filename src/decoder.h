// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "capture.h"
#include "videoframe.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>

// Geometry of the planar buffers for one JPEG (Y, Cb, Cr): real sample counts,
// row stride in bytes, and the allocated row count (whole iMCU rows).
struct YuvLayout {
    int width[3];
    int height[3];
    int stride[3];
    int rows[3];
};

// Decodes MJPG frames (libjpeg-turbo) or converts raw frames on its own
// thread. Each decoded frame is handed to the sink callback, which runs on the
// decoder thread and must be cheap and thread-safe (VideoWidget only stores the
// newest frame: drop-on-backpressure).
//
// Whenever the renderer says it can convert on the GPU (setGpuYuvCheck), MJPG
// is decoded to planar Y/Cb/Cr (if it is plain 3-component YCbCr; that skips
// the CPU colour conversion + chroma upsampling and halves the bytes uploaded)
// and full-range BT.601 YUYV is passed through untouched. Anything else (other
// raw formats, other colour interpretations) becomes RGBA on the CPU.
//
// Frames come from a small pool that is recycled once every other holder has
// released them, so there is no per-frame allocation or copy.
class Decoder {
public:
    using FrameSink = std::function<void(const VideoFramePtr &, std::chrono::steady_clock::time_point)>;
    Decoder(FrameQueue *queue, Stats *stats, FrameSink sink);
    ~Decoder();

    // Called on the decoder thread before each MJPG frame; return true while the
    // renderer can accept planar YUV frames. Set before start().
    void setGpuYuvCheck(std::function<bool()> check) { m_gpuYuv = std::move(check); }

    // Colour interpretation that replaces the driver's (see devicequirks.h).
    // Set before start().
    void setColorOverride(std::optional<device::ColorInfo> color) { m_colorOverride = color; }

    void start();
    void stop();
    void join();

private:
    void run();
    VideoFramePtr acquireRgba(int w, int h);
    VideoFramePtr acquireYuv(const YuvLayout &layout);
    static void convertRaw(const Frame &frame, const device::ColorInfo &color, QImage &dst);

    FrameQueue *m_queue;
    Stats *m_stats;
    FrameSink m_sink;
    std::function<bool()> m_gpuYuv;
    std::optional<device::ColorInfo> m_colorOverride;
    std::array<VideoFramePtr, 4> m_pool; // producer + consumers in flight + slack
    size_t m_poolNext = 0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
