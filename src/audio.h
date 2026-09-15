#pragma once

#include "capture.h"

#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QString>

#include <atomic>
#include <deque>
#include <thread>

class QAudioSink;

// Bounded ring of S16 stereo 48 kHz chunks feeding the host audio device.
// Pulled by Qt's audio thread; filled by the ALSA capture thread. Drops the
// oldest chunk when full so capture never blocks.
class AudioRing {
public:
    void push(QByteArray &&chunk)
    {
        {
            QMutexLocker lock(&m_mutex);
            if (m_chunks.size() >= m_capacity && !m_chunks.empty())
                m_chunks.pop_front();
            m_chunks.push_back(std::move(chunk));
        }
    }

    qint64 pull(char *dst, qint64 maxBytes)
    {
        if (maxBytes <= 0)
            return 0;
        QMutexLocker lock(&m_mutex);
        qint64 total = 0;
        while (total < maxBytes && !m_chunks.empty()) {
            QByteArray &front = m_chunks.front();
            const qint64 need = maxBytes - total;
            const qint64 take = qMin<qint64>(need, front.size());
            std::memcpy(dst + total, front.constData(), size_t(take));
            total += take;
            if (take == front.size())
                m_chunks.pop_front();
            else
                front.remove(0, int(take));
        }
        if (total == 0 && maxBytes > 0) {
            // Real starvation: nothing buffered at all when the sink pulled.
            m_underruns.fetch_add(1, std::memory_order_relaxed);
        }
        return total;
    }

    void clear()
    {
        QMutexLocker lock(&m_mutex);
        m_chunks.clear();
    }

    uint64_t underruns() const { return m_underruns.load(); }

    size_t depthBytes() const
    {
        QMutexLocker lock(&m_mutex);
        size_t bytes = 0;
        for (const QByteArray &c : m_chunks)
            bytes += size_t(c.size());
        return bytes;
    }

private:
    static constexpr size_t m_capacity = 16;
    mutable QMutex m_mutex;
    std::deque<QByteArray> m_chunks;
    std::atomic<uint64_t> m_underruns{0};
};

// QIODevice Qt's audio output pulls PCM from (pull model). Data comes from the
// ring; returns only what is actually buffered (never fabricates silence) so
// the sink is paced by real data. Logger counts only true starvation.
class HostAudioSink : public QIODevice {
public:
    explicit HostAudioSink(AudioRing *ring) : m_ring(ring) {}

    qint64 readData(char *data, qint64 maxlen) override
    {
        return m_ring ? m_ring->pull(data, maxlen) : qint64(0);
    }

    qint64 writeData(const char *, qint64) override { return -1; }

    qint64 bytesAvailable() const override
    {
        return m_ring ? qint64(m_ring->depthBytes()) : qint64(0);
    }

private:
    AudioRing *m_ring;
};

// Finds the ALSA card index of the ShadowCast audio device (-1 if absent).
int findShadowCastAudioCard();

// Qt/QAudioSink output to the host default speaker via the ring.
class AudioSink {
public:
    AudioSink() = default;
    ~AudioSink() { close(); }
    AudioSink(const AudioSink &) = delete;
    AudioSink &operator=(const AudioSink &) = delete;

    bool open(AudioRing *ring, QString *error);
    void close();
    bool active() const { return m_active; }
    void setVolume(qreal volume);
    qreal volume() const { return m_volume; }

private:
    QAudioSink *m_sink = nullptr;
    HostAudioSink *m_host = nullptr;
    bool m_active = false;
    qreal m_volume = 1.0;
};

// ALSA capture from the ShadowCast card (S16_LE mono 96 kHz), resampled to
// S16 stereo 48 kHz and pushed into the ring. Runs on its own thread.
class AudioCapture {
public:
    AudioCapture(AudioRing *ring, Stats *stats);
    ~AudioCapture();

    bool start(int cardIndex);
    void stop();
    void join();
    QString errorMessage() const { return m_error; }

private:
    void run();

    AudioRing *m_ring;
    Stats *m_stats;
    void *m_handle = nullptr;
    QString m_error;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};