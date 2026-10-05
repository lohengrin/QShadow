#include "audio.h"
#include "audiodrift.h"
#include "device.h"

#include <QAudio>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

#include <alsa/asoundlib.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int kCaptureRate = 96000;
constexpr int kOutputRate = 48000;
constexpr snd_pcm_uframes_t kPeriodFrames = 480; // 5 ms @ 96 kHz
constexpr snd_pcm_uframes_t kBufferFrames = kPeriodFrames * 4; // 20 ms
// Ring fill the drift control steers towards, sampled just before each capture
// chunk is pushed (so ~3 ms below the time-averaged depth). The sink pulls in
// bursts, so the ring swings by a few ms around it; 8 ms keeps the time-averaged
// depth near 11 ms: headroom against underruns without adding delay.
constexpr double kTargetRingFrames = 8.0 * 48.0; // 8 ms

} // namespace

int findShadowCastAudioCard()
{
    // Identify the card by the USB IDs, like the video node; fall back to the
    // ALSA card name only if sysfs does not give an answer.
    const int byUsb = device::findShadowCastAudioCard();
    if (byUsb >= 0)
        return byUsb;

    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char *name = nullptr;
        if (snd_card_get_name(card, &name) == 0) {
            const bool match = std::strstr(name, "ShadowCast") != nullptr;
            free(name);
            if (match)
                return card;
        }
    }
    return -1;
}

bool AudioSink::open(AudioRing *ring, QString *error)
{
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        if (error)
            *error = QStringLiteral("No default audio output device");
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(kOutputRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);

    bool floatOutput = false;
    if (!device.isFormatSupported(format)) {
        floatOutput = true;
        format.setSampleFormat(QAudioFormat::Float);
        if (!device.isFormatSupported(format)) {
            if (error)
                *error = QStringLiteral("Host output rejects 48 kHz stereo S16/F32");
            return false;
        }
    }

    m_host = new HostAudioSink(ring, floatOutput);
    if (!m_host->open(QIODevice::ReadOnly)) {
        delete m_host;
        m_host = nullptr;
        if (error)
            *error = QStringLiteral("Failed to open pull device");
        return false;
    }

    m_sink = new QAudioSink(device, format);
    m_sink->setBufferSize(int(double(kOutputRate) * 2 * (floatOutput ? 4 : 2) * 0.04)); // ~40 ms
    m_sink->setVolume(m_volume);
    m_sink->start(m_host);

    const QAudio::State st = m_sink->state();
    m_active = (st == QAudio::ActiveState || st == QAudio::IdleState);
    if (!m_active) {
        if (error)
            *error = QStringLiteral("Audio output failed to start (error %1)")
                         .arg(int(m_sink->error()));
        close();
        return false;
    }
    return true;
}

void AudioSink::setVolume(qreal volume)
{
    m_volume = qBound<qreal>(0.0, volume, 1.0);
    if (m_sink)
        m_sink->setVolume(m_volume);
}

void AudioSink::close()
{
    if (m_sink) {
        if (m_sink->state() != QAudio::StoppedState)
            m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    if (m_host) {
        m_host->close();
        delete m_host;
        m_host = nullptr;
    }
    m_active = false;
}

AudioCapture::AudioCapture(AudioRing *ring, Stats *stats)
    : m_ring(ring), m_stats(stats)
{
}

AudioCapture::~AudioCapture()
{
    stop();
    join();
}

bool AudioCapture::start(int cardIndex)
{
    if (cardIndex < 0) {
        m_error = QStringLiteral("ShadowCast audio card not found");
        return false;
    }

    const QString hw = QStringLiteral("hw:%1,0").arg(cardIndex);
    snd_pcm_t *handle = nullptr;
    if (snd_pcm_open(&handle, hw.toLocal8Bit().constData(),
                     SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) < 0) {
        m_error = QStringLiteral("snd_pcm_open(%1) failed").arg(hw);
        return false;
    }
    m_handle = handle;

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);

    snd_pcm_uframes_t period = kPeriodFrames;
    snd_pcm_uframes_t buffer = kBufferFrames;

    int err = snd_pcm_hw_params_any(handle, params);
    if (err == 0) err = snd_pcm_hw_params_set_access(handle, params,
                                                     SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err == 0) err = snd_pcm_hw_params_set_format(handle, params,
                                                     SND_PCM_FORMAT_S16_LE);
    if (err == 0) err = snd_pcm_hw_params_set_channels(handle, params, 1);
    if (err == 0) err = snd_pcm_hw_params_set_rate(handle, params, kCaptureRate, 0);
    if (err == 0) err = snd_pcm_hw_params_set_period_size_near(handle, params,
                                                               &period, nullptr);
    if (err == 0) err = snd_pcm_hw_params_set_buffer_size_near(handle, params,
                                                               &buffer);
    if (err == 0) err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        m_error = QStringLiteral("snd_pcm_hw_params: %1").arg(snd_strerror(err));
        snd_pcm_close(handle);
        m_handle = nullptr;
        return false;
    }

    snd_pcm_start(handle);
    m_stats->audioUnderruns.store(0);
    m_thread = std::thread([this] { run(); });
    return true;
}

void AudioCapture::stop()
{
    m_stop.store(true);
}

void AudioCapture::join()
{
    if (m_thread.joinable())
        m_thread.join();
    if (m_handle) {
        auto *h = static_cast<snd_pcm_t *>(m_handle);
        snd_pcm_drain(h);
        snd_pcm_close(h);
        m_handle = nullptr;
    }
}

void AudioCapture::run()
{
    auto *handle = static_cast<snd_pcm_t *>(m_handle);

    std::vector<int16_t> input(kPeriodFrames);
    std::vector<int16_t> pending; // leftover odd sample between reads
    std::vector<float> mono;      // 48 kHz mono after the 2:1 average
    std::vector<float> resampled; // after drift-compensated resampling

    DriftController controller(kTargetRingFrames);
    DriftResampler resampler;

    while (!m_stop.load()) {
        const int wr = snd_pcm_wait(handle, 100);
        if (wr == 0)
            continue;
        if (wr < 0) {
            int err = snd_pcm_recover(handle, wr, 1);
            if (err < 0) {
                m_error = QStringLiteral("snd_pcm_recover: %1").arg(snd_strerror(err));
                break;
            }
            continue;
        }

        const snd_pcm_sframes_t n = snd_pcm_readi(handle, input.data(), kPeriodFrames);
        if (n < 0) {
            int err = snd_pcm_recover(handle, int(n), 1);
            if (err < 0) {
                m_error = QStringLiteral("snd_pcm_readi: %1").arg(snd_strerror(err));
                break;
            }
            continue;
        }
        if (n == 0)
            continue;

        // 96 kHz mono S16 -> 48 kHz mono by averaging sample pairs, carrying a
        // trailing odd sample across reads.
        pending.insert(pending.end(), input.begin(), input.begin() + n);
        const size_t pairs = pending.size() / 2;
        if (pairs == 0)
            continue;
        mono.resize(pairs);
        for (size_t i = 0; i < pairs; ++i)
            mono[i] = (float(pending[2 * i]) + float(pending[2 * i + 1])) * 0.5f;
        pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(2 * pairs));

        // Absorb the dongle/host clock difference by nudging the resampling
        // ratio so the ring stays near its target fill.
        const double step =
            controller.update(double(m_ring->depthBytes()) / double(AudioRing::kBytesPerMs)
                              * 48.0);
        m_driftPpm.store(int(controller.adjust() * 1e6), std::memory_order_relaxed);
        resampled.clear();
        resampler.process(mono.data(), mono.size(), step, &resampled);
        if (resampled.empty())
            continue;

        // Mono -> stereo S16.
        QByteArray chunk(int(resampled.size()) * 4, Qt::Uninitialized);
        auto *out = reinterpret_cast<int16_t *>(chunk.data());
        for (size_t i = 0; i < resampled.size(); ++i) {
            const float v = std::clamp(resampled[i], -32768.0f, 32767.0f);
            const int16_t s = int16_t(std::lrintf(v));
            out[2 * i] = s;
            out[2 * i + 1] = s;
        }
        m_ring->push(std::move(chunk));
    }
}