#pragma once

#include "audio.h"
#include "audiodrift.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// The platform-independent half of audio capture. A backend reads interleaved
// S16 frames from the dongle (whatever its native rate/channel layout) and feeds
// them here; the pipeline downmixes to mono, decimates to 48 kHz, absorbs the
// dongle/host clock drift (audiodrift.h) and pushes stereo S16 48 kHz chunks into
// the AudioRing that the host output pulls from.
class AudioPipeline {
public:
    // inputRate must be a multiple of 48000 (the ShadowCast's 96000 is a factor
    // of 2); channels are averaged into one.
    AudioPipeline(AudioRing *ring, int inputRate, int channels);

    // Feeds `frames` interleaved sample frames. Pushes zero or one chunk.
    void process(const int16_t *interleaved, size_t frames);

    // Current drift correction on the resampling ratio, in ppm.
    int driftPpm() const { return m_driftPpm.load(std::memory_order_relaxed); }

private:
    AudioRing *m_ring;
    const size_t m_decimation;
    const size_t m_channels;
    std::vector<float> m_pending;   // mono samples at the input rate, < one group left over
    std::vector<float> m_mono;      // 48 kHz mono after decimation
    std::vector<float> m_resampled; // after drift-compensated resampling
    DriftController m_controller;
    DriftResampler m_resampler;
    std::atomic<int> m_driftPpm{0};
};
