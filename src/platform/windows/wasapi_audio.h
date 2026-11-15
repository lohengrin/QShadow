// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "audiopipeline.h"
#include "platform/platform.h"

#include <QString>

#include <atomic>
#include <future>
#include <memory>
#include <thread>

namespace wasapi {

// Endpoint ID of the capture endpoint on the USB device with these IDs (read
// from the endpoint's topology connector). Empty if there is none.
QString findCaptureEndpoint(int vid, int pid);

// Every active capture endpoint: friendly name and endpoint id.
QList<platform::AudioInputInfo> listCaptureEndpoints();

} // namespace wasapi

// WASAPI shared-mode, event-driven capture from the device's endpoint on its
// own thread, at the smallest engine period the driver allows (IAudioClient3).
// Samples go through an AudioPipeline (48 kHz stereo, drift-compensated) into
// the AudioRing.
class WasapiAudioInput : public AudioInput {
public:
    WasapiAudioInput(const QString &endpointId, AudioRing *ring, Stats *stats);
    ~WasapiAudioInput() override;

    bool start() override;
    void stop() override;
    void join() override;
    QString errorMessage() const override { return m_error; }
    int driftPpm() const override { return m_driftPpm.load(std::memory_order_relaxed); }

private:
    void run(std::promise<bool> *ready);

    QString m_endpointId;
    AudioRing *m_ring;
    Stats *m_stats;
    QString m_error;
    std::atomic<int> m_driftPpm{0};
    void *m_stopEvent = nullptr;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
