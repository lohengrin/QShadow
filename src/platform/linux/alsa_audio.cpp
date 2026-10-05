#include "alsa_audio.h"

#include "sysfs_usb.h"

#include <alsa/asoundlib.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int kCaptureRate = 96000;
constexpr snd_pcm_uframes_t kPeriodFrames = 480; // 5 ms @ 96 kHz
constexpr snd_pcm_uframes_t kBufferFrames = kPeriodFrames * 4; // 20 ms

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

    snd_pcm_start(handle);
    m_stats->audioUnderruns.store(0);
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
        snd_pcm_drain(h);
        snd_pcm_close(h);
        m_handle = nullptr;
    }
}

void AlsaAudioInput::run()
{
    auto *handle = static_cast<snd_pcm_t *>(m_handle);

    std::vector<int16_t> input(kPeriodFrames);

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

        m_pipeline.process(input.data(), size_t(n));
    }
}
