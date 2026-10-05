#pragma once

#include "device.h"

#include <QByteArray>
#include <QDebug>
#include <QSize>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

struct Stats {
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> droppedCapture{0};
    std::atomic<uint64_t> decoded{0};
    std::atomic<uint64_t> droppedDecode{0};
    std::atomic<uint64_t> audioUnderruns{0};
    std::atomic<uint64_t> decodeNs{0};
    std::atomic<int> frameW{0};
    std::atomic<int> frameH{0};
    std::atomic<bool> frameYuv{false}; // last frame went to the GPU as planar YUV
};

// One captured frame: a JPEG for MJPG, or raw packed pixels for YUYV
// (width/height/stride describe the raw layout; unused for MJPG).
struct Frame {
    QByteArray data;
    device::PixelFormat format = device::PixelFormat::Mjpg;
    int width = 0;
    int height = 0;
    int stride = 0;
    // When the frame reached the host (V4L2 monotonic timestamp when the driver
    // provides one, else the dequeue time); origin of the latency readout.
    std::chrono::steady_clock::time_point stamp;
};

// Bounded queue between the capture and decode threads. Pushes drop the oldest
// frame when the queue is full so the pipeline always stays as fresh as
// possible (latency over completeness).
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity) : m_capacity(capacity) {}

    void push(Frame &&frame)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.size() >= m_capacity && !m_queue.empty()) {
                m_queue.pop_front();
                m_drops.fetch_add(1);
            }
            m_queue.push_back(std::move(frame));
        }
        m_notEmpty.notify_one();
    }

    uint64_t drops() const { return m_drops.load(); }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }

    // Blocks up to timeoutMs. Returns false on timeout or when stopped and empty.
    bool pop(Frame *out, bool stopped, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return stopped || !m_queue.empty();
        });
        if (m_queue.empty())
            return false;
        *out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

private:
    const size_t m_capacity;
    std::atomic<uint64_t> m_drops{0};
    std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::deque<Frame> m_queue;
};

// V4L2 MJPG/YUYV capture using mmap streaming, run on its own thread.
// One captured frame in -> one Frame out; never blocks the decoder.
class Capture {
public:
    Capture(const QString &devicePath, const QSize &targetSize,
            device::PixelFormat format, Stats *stats);
    ~Capture();

    bool start(FrameQueue *out);
    void stop();
    void join();

    QSize negotiatedSize() const { return m_negotiated; }
    int negotiatedFps() const { return m_fps; }
    // False once the capture thread has exited (device unplugged, stream error)
    // or if start() failed.
    bool isRunning() const { return m_running.load(); }
    // Suppress start() failure warnings (used while retrying a known failure).
    void setQuiet(bool quiet) { m_quiet = quiet; }

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
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
    } m_buffers[4];

    QSize m_negotiated;
    int m_fps = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};