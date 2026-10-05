#pragma once

#include "capture.h"

#include <QImage>

#include <array>
#include <atomic>
#include <functional>
#include <thread>

// Decodes MJPG frames (libjpeg-turbo) or converts raw YUYV frames to RGBA on
// its own thread. Each decoded frame is handed to the sink callback, which runs
// on the decoder thread and must be cheap and thread-safe (VideoWidget only
// stores the newest frame: drop-on-backpressure). Frames are decoded into a
// small pool of images that is recycled once the consumer has released them,
// so there is no per-frame allocation or copy.
class Decoder {
public:
    using FrameSink = std::function<void(const QImage &)>;
    Decoder(FrameQueue *queue, Stats *stats, FrameSink sink);
    ~Decoder();

    void start();
    void stop();
    void join();

private:
    void run();
    QImage &acquireImage(int w, int h);
    void convertYuyv(const Frame &frame, QImage &dst);

    FrameQueue *m_queue;
    Stats *m_stats;
    FrameSink m_sink;
    std::array<QImage, 4> m_pool; // producer + consumer in flight + slack
    size_t m_poolNext = 0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};