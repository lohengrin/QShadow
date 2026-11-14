#pragma once

#include "device.h"

#include <QByteArray>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>

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

// True when an MJPG buffer holds an actual picture. At 1920x1080 and the other
// sizes above 720p the ShadowCast pads its 60 fps stream with 4-byte buffers
// (just the SOI and EOI markers); they are not frames and must not be counted
// or decoded. Any real JPEG carries tables and a scan, far more than this.
inline bool isPlausibleJpeg(const char *data, size_t size)
{
    constexpr size_t kMinJpegBytes = 64;
    return size >= kMinJpegBytes && static_cast<unsigned char>(data[0]) == 0xff
           && static_cast<unsigned char>(data[1]) == 0xd8;
}

// One captured frame: a JPEG for MJPG, or raw packed pixels for YUYV
// (width/height/stride describe the raw layout; unused for MJPG).
struct Frame {
    QByteArray data;
    device::PixelFormat format = device::PixelFormat::Mjpg;
    int width = 0;
    int height = 0;
    int stride = 0;
    // When the frame reached the host (the driver's capture timestamp when it
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

// A small pool of reusable frame buffers, so a capture backend does not
// allocate for every frame. QByteArray is implicitly shared: a pooled array is
// free again once every consumer (queue, decoder, widget, renderer) has dropped
// its copy, which isDetached() reports.
class BufferPool {
public:
    // Capture queue (2) + decoder + widget pending + renderer + slack.
    static constexpr size_t kCount = 8;

    // Pre-sizes every pooled array so recycling never reallocates.
    void reserve(size_t capacityBytes)
    {
        for (QByteArray &b : m_pool) {
            b = QByteArray();
            b.reserve(qsizetype(capacityBytes));
        }
    }

    // A QByteArray holding `used` bytes copied from `src`. Falls back to a fresh
    // allocation if every pooled array is still in use (never blocks).
    QByteArray take(const char *src, size_t used)
    {
        for (QByteArray &b : m_pool) {
            // isDetached(): nobody else shares this array, so overwriting is safe.
            if (b.isDetached() && size_t(b.capacity()) >= used) {
                b.resize(qsizetype(used)); // within capacity: no reallocation
                std::memcpy(b.data(), src, used);
                return b; // shared copy; the pool keeps its reference
            }
        }
        return QByteArray(src, qsizetype(used));
    }

private:
    std::array<QByteArray, kCount> m_pool;
};
