#pragma once

#include "capture.h"

#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QString>

#include <atomic>
#include <cstring>
#include <cstdint>
#include <deque>
#include <thread>

class QAudioSink;

// Bounded ring of S16 stereo 48 kHz chunks feeding the host audio device.
// Pulled by Qt's audio thread; filled by the ALSA capture thread. Drops the
// oldest audio when over the cap so capture never blocks. The cap is only a
// safety valve: AudioCapture's drift control keeps the fill near a small target.
class AudioRing {
public:
    static constexpr size_t kBytesPerMs = 48 * 2 * 2; // 48 kHz stereo S16
    static constexpr size_t kMaxBytes = 60 * kBytesPerMs;

    void push(QByteArray &&chunk)
    {
        QMutexLocker lock(&m_mutex);
        m_bytes += size_t(chunk.size());
        m_chunks.push_back(std::move(chunk));
        while (m_bytes > kMaxBytes && m_chunks.size() > 1) {
            m_bytes -= size_t(m_chunks.front().size());
            m_chunks.pop_front();
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
            m_bytes -= size_t(take);
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
        m_bytes = 0;
    }

    uint64_t underruns() const { return m_underruns.load(); }

    size_t depthBytes() const
    {
        QMutexLocker lock(&m_mutex);
        return m_bytes;
    }

private:
    mutable QMutex m_mutex;
    std::deque<QByteArray> m_chunks;
    size_t m_bytes = 0;
    std::atomic<uint64_t> m_underruns{0};
};

// QIODevice Qt's audio output pulls PCM from (pull model). Data comes from the
// ring; returns only what is actually buffered (never fabricates silence) so
// the sink is paced by real data. Logger counts only true starvation.
class HostAudioSink : public QIODevice {
public:
    // floatOutput: the host sink only accepts 32-bit float PCM, so the ring's
    // S16 samples are converted on the way out.
    explicit HostAudioSink(AudioRing *ring, bool floatOutput = false)
        : m_ring(ring), m_float(floatOutput)
    {
    }

    qint64 readData(char *data, qint64 maxlen) override
    {
        if (!m_ring)
            return 0;
        if (!m_float)
            return m_ring->pull(data, maxlen);

        const qint64 samples = maxlen / qint64(sizeof(float));
        if (samples <= 0)
            return 0;
        m_scratch.resize(int(samples * qint64(sizeof(int16_t))));
        const qint64 got = m_ring->pull(m_scratch.data(), m_scratch.size());
        const qint64 n = got / qint64(sizeof(int16_t));
        const auto *in = reinterpret_cast<const int16_t *>(m_scratch.constData());
        auto *out = reinterpret_cast<float *>(data);
        for (qint64 i = 0; i < n; ++i)
            out[i] = float(in[i]) * (1.0f / 32768.0f);
        return n * qint64(sizeof(float));
    }

    qint64 writeData(const char *, qint64) override { return -1; }

    qint64 bytesAvailable() const override
    {
        if (!m_ring)
            return 0;
        const qint64 bytes = qint64(m_ring->depthBytes());
        return m_float ? bytes * 2 : bytes;
    }

private:
    AudioRing *m_ring;
    bool m_float;
    QByteArray m_scratch;
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
    // Current drift correction applied to the resampling ratio, in ppm
    // (positive: draining a ring that was running full).
    int driftPpm() const { return m_driftPpm.load(std::memory_order_relaxed); }

private:
    void run();

    AudioRing *m_ring;
    Stats *m_stats;
    void *m_handle = nullptr;
    QString m_error;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_driftPpm{0};
    std::thread m_thread;
};