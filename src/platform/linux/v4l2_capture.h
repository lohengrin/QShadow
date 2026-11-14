#pragma once

#include "platform/platform.h"

#include <QDebug>

#include <atomic>
#include <thread>

// V4L2 MJPG/YUYV capture using mmap streaming, run on its own thread.
// One captured frame in -> one Frame out; never blocks the decoder.
class V4l2Capture : public CaptureSource {
public:
    V4l2Capture(const QString &devicePath, const QSize &targetSize,
                device::PixelFormat format, Stats *stats);
    ~V4l2Capture() override;

    bool start(FrameQueue *out) override;
    void stop() override;
    void join() override;

    QSize negotiatedSize() const override { return m_negotiated; }
    int negotiatedFps() const override { return m_fps; }
    bool isRunning() const override { return m_running.load(); }
    void setQuiet(bool quiet) override { m_quiet = quiet; }

private:
    void run();
    QDebug warn();

    QString m_devicePath;
    QSize m_targetSize;
    bool m_quiet = false;
    QString m_discard;
    device::PixelFormat m_format;
    int m_stride = 0;
    Stats *m_stats;
    FrameQueue *m_queue = nullptr;

    int m_fd = -1;
    int m_bufferCount = 0;
    BufferPool m_pool;
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
    } m_buffers[4];

    QSize m_negotiated;
    int m_fps = 0;
    bool m_streaming = false; // VIDIOC_STREAMON succeeded and not yet undone
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
