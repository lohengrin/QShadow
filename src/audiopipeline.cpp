// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "audiopipeline.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>

namespace {

// Ring fill the drift control steers towards, sampled just before each capture
// chunk is pushed (so ~3 ms below the time-averaged depth). The sink pulls in
// bursts, so the ring swings by a few ms around it; 8 ms keeps the time-averaged
// depth near 11 ms: headroom against underruns without adding delay.
constexpr double kTargetRingFrames = 8.0 * 48.0; // 8 ms

} // namespace

AudioPipeline::AudioPipeline(AudioRing *ring, int inputRate, int channels)
    : m_ring(ring)
    , m_decimation(size_t(std::max(1, inputRate / 48000)))
    , m_nominalStep(double(std::max(1, inputRate)) / double(m_decimation) / 48000.0)
    , m_channels(size_t(std::max(1, channels)))
    , m_controller(kTargetRingFrames)
{
}

void AudioPipeline::process(const int16_t *interleaved, size_t frames)
{
    // Downmix to mono, keeping the samples at the input rate.
    const float invChannels = 1.0f / float(m_channels);
    for (size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (size_t c = 0; c < m_channels; ++c)
            sum += float(interleaved[f * m_channels + c]);
        m_pending.push_back(sum * invChannels);
    }

    // Decimate by the whole number of 48 kHz in the input rate by averaging
    // groups of samples, carrying the remainder across calls. What is left of
    // the rate (none for 96000, 44100/48000 for 44100) the resampler handles.
    const size_t groups = m_pending.size() / m_decimation;
    if (groups == 0)
        return;
    const float invDecimation = 1.0f / float(m_decimation);
    m_mono.resize(groups);
    for (size_t g = 0; g < groups; ++g) {
        float sum = 0.0f;
        for (size_t k = 0; k < m_decimation; ++k)
            sum += m_pending[g * m_decimation + k];
        m_mono[g] = sum * invDecimation;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + std::ptrdiff_t(groups * m_decimation));

    // Absorb the dongle/host clock difference by nudging the resampling ratio so
    // the ring stays near its target fill.
    const double step = m_controller.update(
        double(m_ring->depthBytes()) / double(AudioRing::kBytesPerMs) * 48.0);
    m_driftPpm.store(int(m_controller.adjust() * 1e6), std::memory_order_relaxed);
    m_resampled.clear();
    m_resampler.process(m_mono.data(), m_mono.size(), m_nominalStep * step, &m_resampled);
    if (m_resampled.empty())
        return;

    // Mono -> stereo S16.
    QByteArray chunk(int(m_resampled.size()) * 4, Qt::Uninitialized);
    auto *out = reinterpret_cast<int16_t *>(chunk.data());
    for (size_t i = 0; i < m_resampled.size(); ++i) {
        const float v = std::clamp(m_resampled[i], -32768.0f, 32767.0f);
        const int16_t s = int16_t(std::lrintf(v));
        out[2 * i] = s;
        out[2 * i + 1] = s;
    }
    m_ring->push(std::move(chunk));
}
