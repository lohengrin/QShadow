// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

// The ALSA capture read loop, written against a handful of PCM operations
// instead of snd_pcm_* directly, so the recovery logic can be tested with a fake
// PCM (tests/tst_alsa_loop.cpp). No ALSA header is needed here.
//
// Why this exists: a capture PCM only produces data while it is RUNNING. After an
// overrun, snd_pcm_recover() re-prepares the stream (PREPARED) but does not
// restart it, and in PREPARED state poll never signals because no data flows. A
// loop that just "waits again" after recovering therefore waits forever and the
// audio is silently dead until the app restarts. So the loop starts the stream
// whenever it finds it stopped.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace alsaloop {

struct Pcm {
    std::function<int(int timeoutMs)> wait;                // >0 ready, 0 timeout, <0 error
    std::function<long(int16_t *buf, size_t frames)> read; // frames read, or <0 error
    std::function<int(int err)> recover;                   // 0 if the stream was recovered
    std::function<bool()> stopped;                         // PREPARED: open but not running
    std::function<int()> start;
};

// Reads periods of `periodFrames` and hands them to `onFrames` until `stop` is
// set (returns true) or an error cannot be recovered (returns false, *error set).
// Overruns/suspends are counted in *xruns and recovered from.
// `channels` is the interleaving of the samples: `read` fills up to
// periodFrames * channels values and returns frames.
inline bool run(const Pcm &pcm, size_t periodFrames, const std::atomic<bool> &stop,
                const std::function<void(const int16_t *, size_t)> &onFrames,
                std::atomic<uint64_t> *xruns, std::string *error, size_t channels = 1)
{
    std::vector<int16_t> buf(periodFrames * std::max<size_t>(1, channels));

    // -EPIPE is an overrun and -ESTRPIPE a system suspend; -EAGAIN (non-blocking
    // read with nothing ready) is not a fault.
    const auto isXrun = [](long err) { return err == -EPIPE || err == -ESTRPIPE; };
    const auto recoverFrom = [&](long err, const char *what) {
        if (isXrun(err))
            xruns->fetch_add(1, std::memory_order_relaxed);
        if (pcm.recover(int(err)) < 0) {
            *error = std::string(what) + ": unrecoverable";
            return false;
        }
        return true;
    };

    while (!stop.load()) {
        // First start, and after every recover(): the stream is only PREPARED.
        if (pcm.stopped())
            pcm.start();

        const int w = pcm.wait(100);
        if (w == 0)
            continue;
        if (w < 0) {
            if (!recoverFrom(w, "wait"))
                return false;
            continue;
        }

        const long n = pcm.read(buf.data(), periodFrames);
        if (n < 0) {
            if (!recoverFrom(n, "read"))
                return false;
            continue;
        }
        if (n == 0)
            continue;

        onFrames(buf.data(), size_t(n));
    }
    return true;
}

} // namespace alsaloop
