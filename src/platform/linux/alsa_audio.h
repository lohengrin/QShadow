// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "audiopipeline.h"
#include "platform/platform.h"

#include <QString>

#include <atomic>
#include <memory>
#include <thread>

// ALSA capture from a sound card on its own thread. The format is negotiated with
// the hardware (S16_LE, mono or stereo, the best of a few common rates; the
// ShadowCast ends up at 96 kHz mono), falling back to ALSA's plug layer when the
// hardware offers none of that. The samples go through an AudioPipeline (48 kHz
// stereo, drift-compensated) into the AudioRing.
class AlsaAudioInput : public AudioInput {
public:
    AlsaAudioInput(int cardIndex, AudioRing *ring, Stats *stats);
    ~AlsaAudioInput() override;

    bool start() override;
    void stop() override;
    void join() override;
    QString errorMessage() const override { return m_error; }
    int driftPpm() const override { return m_pipeline ? m_pipeline->driftPpm() : 0; }
    uint64_t captureXruns() const override { return m_xruns.load(std::memory_order_relaxed); }

private:
    void run();

    int m_card;
    AudioRing *m_ring;
    Stats *m_stats;
    std::unique_ptr<AudioPipeline> m_pipeline; // created once the format is known
    size_t m_periodFrames = 0;
    size_t m_channels = 1;
    void *m_handle = nullptr;
    QString m_error;
    std::atomic<uint64_t> m_xruns{0};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
