// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "alsa_audio.h"

#include "alsa_capture_loop.h"

#include <alsa/asoundlib.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

// 5 ms periods. The hardware buffer only has to survive a late read; it adds no
// latency because every period is read as soon as it is ready. 16 periods (80 ms)
// make an overrun, which loses data, far less likely when the machine is busy.
constexpr int kPeriodsPerSecond = 200;
constexpr snd_pcm_uframes_t kBufferPeriods = 16;

struct Format {
    unsigned channels;
    unsigned rate;
};

// Applies S16_LE interleaved with the given channels and exact rate, and the
// period/buffer sizes. Returns the error from the first step that failed.
int applyParams(snd_pcm_t *handle, snd_pcm_hw_params_t *params, const Format &f,
                snd_pcm_uframes_t *period)
{
    snd_pcm_uframes_t buffer = *period * kBufferPeriods;
    int err = snd_pcm_hw_params_any(handle, params);
    if (err == 0) err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err == 0) err = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    if (err == 0) err = snd_pcm_hw_params_set_channels(handle, params, f.channels);
    if (err == 0) err = snd_pcm_hw_params_set_rate(handle, params, f.rate, 0);
    if (err == 0) err = snd_pcm_hw_params_set_period_size_near(handle, params, period, nullptr);
    if (err == 0) err = snd_pcm_hw_params_set_buffer_size_near(handle, params, &buffer);
    if (err == 0) err = snd_pcm_hw_params(handle, params);
    return err;
}

// Tries the formats in order of preference: what the hardware does natively, mono
// before stereo (the pipeline downmixes anyway), 96 kHz (the ShadowCast) and 48 kHz
// before the rarer rates. Leaves `handle` configured and returns the format, or
// returns false with *error set.
bool negotiate(snd_pcm_t *handle, bool plug, Format *chosen, snd_pcm_uframes_t *period,
               QString *error)
{
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);

    // Through the plug layer ALSA converts anything, so one request is enough.
    static const unsigned kRates[] = {96000, 48000, 192000, 44100, 88200, 32000};
    static const unsigned kPlugRates[] = {48000};
    int err = -EINVAL;
    for (unsigned channels : {1u, 2u}) {
        for (unsigned rate : plug ? std::vector<unsigned>(std::begin(kPlugRates), std::end(kPlugRates))
                                  : std::vector<unsigned>(std::begin(kRates), std::end(kRates))) {
            *chosen = Format{channels, rate};
            *period = snd_pcm_uframes_t(rate / kPeriodsPerSecond);
            err = applyParams(handle, params, *chosen, period);
            if (err == 0)
                return true;
        }
    }
    *error = QStringLiteral("snd_pcm_hw_params: %1").arg(snd_strerror(err));
    return false;
}

} // namespace

AlsaAudioInput::AlsaAudioInput(int cardIndex, AudioRing *ring, Stats *stats)
    : m_card(cardIndex), m_ring(ring), m_stats(stats)
{
}

AlsaAudioInput::~AlsaAudioInput()
{
    stop();
    join();
}

bool AlsaAudioInput::start()
{
    if (m_card < 0) {
        m_error = QStringLiteral("Audio card not found");
        return false;
    }

    // The hardware directly (exact, no conversion) first; the plug layer only if
    // the card offers no S16 mono/stereo at a rate we know.
    Format format{1, 48000};
    snd_pcm_uframes_t period = 0;
    snd_pcm_t *handle = nullptr;
    QString failure;
    for (bool plug : {false, true}) {
        const QString name = QStringLiteral("%1:%2,0").arg(plug ? "plughw" : "hw").arg(m_card);
        if (snd_pcm_open(&handle, name.toLocal8Bit().constData(), SND_PCM_STREAM_CAPTURE,
                         SND_PCM_NONBLOCK) < 0) {
            failure = QStringLiteral("snd_pcm_open(%1) failed").arg(name);
            handle = nullptr;
            break; // a card that does not open directly will not open through plug
        }
        if (negotiate(handle, plug, &format, &period, &failure))
            break;
        snd_pcm_close(handle);
        handle = nullptr;
    }
    if (!handle) {
        m_error = failure;
        return false;
    }
    m_handle = handle;
    m_channels = format.channels;
    m_periodFrames = size_t(period);
    m_pipeline = std::make_unique<AudioPipeline>(m_ring, int(format.rate), int(format.channels));

    // The stream is started by the read loop (it starts any stopped stream).
    m_stats->audioUnderruns.store(0);
    m_xruns = 0;
    m_thread = std::thread([this] { run(); });
    return true;
}

void AlsaAudioInput::stop()
{
    m_stop.store(true);
}

void AlsaAudioInput::join()
{
    if (m_thread.joinable())
        m_thread.join();
    if (m_handle) {
        auto *h = static_cast<snd_pcm_t *>(m_handle);
        snd_pcm_drop(h);
        snd_pcm_close(h);
        m_handle = nullptr;
    }
}

void AlsaAudioInput::run()
{
    auto *handle = static_cast<snd_pcm_t *>(m_handle);

    alsaloop::Pcm pcm;
    pcm.wait = [handle](int ms) { return snd_pcm_wait(handle, ms); };
    pcm.read = [handle](int16_t *buf, size_t frames) {
        return long(snd_pcm_readi(handle, buf, snd_pcm_uframes_t(frames)));
    };
    pcm.recover = [handle](int err) { return snd_pcm_recover(handle, err, 1); };
    pcm.stopped = [handle] { return snd_pcm_state(handle) == SND_PCM_STATE_PREPARED; };
    pcm.start = [handle] { return snd_pcm_start(handle); };

    std::string error;
    const bool ok = alsaloop::run(
        pcm, m_periodFrames, m_stop,
        [this](const int16_t *frames, size_t n) { m_pipeline->process(frames, n); }, &m_xruns,
        &error, m_channels);
    if (!ok)
        m_error = QString::fromStdString(error);
}
