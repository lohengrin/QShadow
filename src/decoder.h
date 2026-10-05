#pragma once

#include "capture.h"

#include <QImage>

#include <atomic>
#include <thread>

class QObject;

// Decodes MJPG frames (libjpeg-turbo) or converts raw YUYV frames to RGBA on
// its own thread. Each decoded frame is handed to the GUI thread via a queued
// QMetaObject call on the target widget, which only ever stores the newest
// frame (drop-on-backpressure).
class Decoder {
public:
    Decoder(FrameQueue *queue, Stats *stats, QObject *frameTarget, const char *slot);
    ~Decoder();

    void start();
    void stop();
    void join();

private:
    void run();
    void convertYuyv(const Frame &frame);

    FrameQueue *m_queue;
    Stats *m_stats;
    QObject *m_target;
    const char *m_slot;
    QImage m_frame; // reused when dimensions match
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};