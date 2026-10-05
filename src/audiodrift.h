#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// Clock-drift compensation for the audio path.
//
// The dongle and the host sound card run on independent clocks, so the nominal
// 2:1 conversion slowly over- or under-produces samples. Left alone the ring
// either grows until it overflows (a click, plus up to tens of ms of added
// latency) or runs dry (a dropout). Instead the resampling ratio is nudged by a
// tiny amount, driven by how far the ring's fill level is from a small
// target; the correction is at most +-kMaxAdjust, far below audible pitch.

// PI controller: ring fill (in frames) -> input samples consumed per output
// sample (1.0 == nominal). The integral term learns the steady clock offset so
// the fill settles on the target instead of at a drift-proportional offset. Greater than 1.0 produces fewer samples and
// drains the ring; less than 1.0 produces more and fills it.
class DriftController {
public:
    static constexpr double kMaxAdjust = 0.005;   // +-0.5% (~8 cents)
    // Fill-level low-pass: the ring oscillates by a few ms every sink pull, which
    // must not be mistaken for drift. ~1 s time constant at ~200 updates/s.
    static constexpr double kSmoothing = 0.005;
    // Proportional gain: closed-loop time constant ~5 s (adjust = kGain * error).
    static constexpr double kGain = 1.0 / (5.0 * 48000.0);
    // Integral gain per update: ~20 s integral time at ~200 updates/s.
    static constexpr double kIntegralGain = kGain / (20.0 * 200.0);

    explicit DriftController(double targetFrames) : m_target(targetFrames) {}

    void reset()
    {
        m_primed = false;
        m_adjust = 0.0;
        m_integral = 0.0;
    }

    // Call once per capture period with the current ring depth in frames.
    double update(double depthFrames)
    {
        if (!m_primed) {
            m_fill = depthFrames;
            m_primed = true;
        } else {
            m_fill += kSmoothing * (depthFrames - m_fill);
        }
        const double err = m_fill - m_target;
        // Clamped integral doubles as anti-windup while the output saturates.
        m_integral = std::clamp(m_integral + kIntegralGain * err, -kMaxAdjust, kMaxAdjust);
        m_adjust = std::clamp(kGain * err + m_integral, -kMaxAdjust, kMaxAdjust);
        return 1.0 + m_adjust;
    }

    double adjust() const { return m_adjust; }
    double smoothedFill() const { return m_fill; }

private:
    double m_target;
    double m_fill = 0.0;
    double m_adjust = 0.0;
    double m_integral = 0.0;
    bool m_primed = false;
};

// Linear-interpolation resampler with a per-call ratio, for mono float input
// already at (nominally) the output rate.
class DriftResampler {
public:
    void reset()
    {
        m_phase = 0.0;
        m_prev = 0.0f;
    }

    // step = input samples consumed per output sample. Appends to *out.
    void process(const float *in, size_t n, double step, std::vector<float> *out)
    {
        for (size_t i = 0; i < n; ++i) {
            const float x = in[i];
            while (m_phase < 1.0) {
                out->push_back(m_prev + (x - m_prev) * float(m_phase));
                m_phase += step;
            }
            m_phase -= 1.0;
            m_prev = x;
        }
    }

private:
    double m_phase = 0.0; // position between m_prev and the next input sample
    float m_prev = 0.0f;
};
