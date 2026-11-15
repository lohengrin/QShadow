// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "alsa_audio.h"

#include "alsa_capture_loop.h"
#include "sysfs_usb.h"

#include <alsa/asoundlib.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kCaptureRate = 96000;
constexpr snd_pcm_uframes_t kPeriodFrames = 480; // 5 ms @ 96 kHz
// The hardware buffer only has to survive a late read; it adds no latency because
// every period is read as soon as it is ready. 80 ms (it was 20 ms) makes an
// overrun, which loses data, far less likely when the machine is busy.
constexpr snd_pcm_uframes_t kBufferFrames = kPeriodFrames * 16;

} // namespace

namespace alsa {

int findCard()
{
    // Identify the card by the USB IDs, like the video node; fall back to the
    // ALSA card name only if sysfs does not give an answer.
    const int byUsb = sysfs_usb::findSoundCard(device::kUsbVendorId, device::kUsbProductId);
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

} // namespace alsa

AlsaAudioInput::AlsaAudioInput(int cardIndex, AudioRing *ring, Stats *stats)
    : m_card(cardIndex), m_ring(ring), m_stats(stats), m_pipeline(ring, kCaptureRate, 1)
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
        m_error = QStringLiteral("ShadowCast audio card not found");
        return false;
    }

    const QString hw = QStringLiteral("hw:%1,0").arg(m_card);
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
        pcm, kPeriodFrames, m_stop,
        [this](const int16_t *frames, size_t n) { m_pipeline.process(frames, n); }, &m_xruns,
        &error);
    if (!ok)
        m_error = QString::fromStdString(error);
}
