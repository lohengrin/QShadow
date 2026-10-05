#pragma once

#include "audiopipeline.h"
#include "platform/platform.h"

#include <QString>

#include <atomic>
#include <thread>

namespace alsa {

// ALSA card index of the ShadowCast's USB audio interface: by USB vendor/product
// ID through sysfs, falling back to the ALSA card name. -1 if absent.
int findCard();

} // namespace alsa

// ALSA capture from the ShadowCast card (S16_LE mono 96 kHz) on its own thread.
// The samples go through an AudioPipeline (48 kHz stereo, drift-compensated)
// into the AudioRing.
class AlsaAudioInput : public AudioInput {
public:
    AlsaAudioInput(int cardIndex, AudioRing *ring, Stats *stats);
    ~AlsaAudioInput() override;

    bool start() override;
    void stop() override;
    void join() override;
    QString errorMessage() const override { return m_error; }
    int driftPpm() const override { return m_pipeline.driftPpm(); }

private:
    void run();

    int m_card;
    AudioRing *m_ring;
    Stats *m_stats;
    AudioPipeline m_pipeline;
    void *m_handle = nullptr;
    QString m_error;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
