// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

// The ALSA capture read loop against a fake PCM that behaves like the real thing:
// a capture stream only signals readiness and produces data while RUNNING, and
// snd_pcm_recover() leaves it merely PREPARED (not restarted).

#include "platform/linux/alsa_capture_loop.h"

#include <QtTest>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

constexpr size_t kPeriod = 480;

struct FakePcm {
    bool running = false;
    int starts = 0;
    int recovers = 0;
    int readsDone = 0;
    int overrunAtRead = -1;     // read number that fails with -EPIPE (once)
    int waitErrorAtRead = -1;   // read number before which wait() reports -EPIPE (once)
    int eagainAtRead = -1;      // read number that returns -EAGAIN (once)
    bool recoverFails = false;
    bool startsPrepared = true; // like a freshly opened stream
    bool injectedWait = false;

    alsaloop::Pcm ops()
    {
        alsaloop::Pcm p;
        p.wait = [this](int) {
            if (!injectedWait && waitErrorAtRead >= 0 && readsDone == waitErrorAtRead) {
                injectedWait = true;
                running = false; // an xrun stops the stream
                return -EPIPE;
            }
            // Like poll(): a stream that is not running never becomes ready.
            if (!running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return 0;
            }
            return 1;
        };
        p.read = [this](int16_t *buf, size_t frames) -> long {
            ++readsDone;
            if (readsDone == eagainAtRead)
                return -EAGAIN;
            if (readsDone == overrunAtRead) {
                running = false;
                return -EPIPE;
            }
            for (size_t i = 0; i < frames; ++i)
                buf[i] = int16_t(readsDone);
            return long(frames);
        };
        p.recover = [this](int err) {
            ++recovers;
            if (recoverFails)
                return -EIO;
            if (err == -EAGAIN)
                return 0;     // nothing to do, as in ALSA
            running = false;  // snd_pcm_prepare(): PREPARED, not started
            return 0;
        };
        p.stopped = [this] { return !running; };
        p.start = [this] {
            ++starts;
            running = true;
            return 0;
        };
        return p;
    }
};

struct Outcome {
    bool ok = false;
    bool finished = false;
    int frames = 0;
    int chunks = 0;
    uint64_t xruns = 0;
    std::string error;
};

// Runs the loop until `wantChunks` periods were delivered. A broken loop (the bug
// this guards against) never delivers them: give up after 3 s instead of hanging.
Outcome drive(FakePcm &fake, int wantChunks)
{
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> xruns{0};
    Outcome out;
    std::string error;
    auto fut = std::async(std::launch::async, [&] {
        return alsaloop::run(
            fake.ops(), kPeriod, stop,
            [&](const int16_t *, size_t n) {
                out.frames += int(n);
                if (++out.chunks >= wantChunks)
                    stop = true;
            },
            &xruns, &error);
    });
    if (fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready) {
        out.finished = true;
        out.ok = fut.get();
    } else {
        stop = true; // let the thread end so the future can be joined
        out.ok = fut.get();
    }
    out.xruns = xruns.load();
    out.error = error;
    return out;
}

} // namespace

class TstAlsaLoop : public QObject {
    Q_OBJECT
private slots:
    void startsAStreamThatWasOnlyPrepared()
    {
        FakePcm fake; // freshly opened: PREPARED
        const Outcome o = drive(fake, 20);
        QVERIFY2(o.finished, "stuck waiting on a stream nobody started");
        QCOMPARE(o.chunks, 20);
        QCOMPARE(o.frames, 20 * int(kPeriod));
        QCOMPARE(fake.starts, 1);
        QCOMPARE(o.xruns, uint64_t(0));
    }

    // Regression: after an overrun the stream is only PREPARED. The old loop waited
    // forever from here on and the audio stayed dead until the app was restarted.
    void restartsTheStreamAfterAnOverrunInRead()
    {
        FakePcm fake;
        fake.overrunAtRead = 10;
        const Outcome o = drive(fake, 40);
        QVERIFY2(o.finished, "audio never resumed after an overrun");
        QCOMPARE(o.chunks, 40);
        QCOMPARE(o.xruns, uint64_t(1));
        QCOMPARE(fake.recovers, 1);
        QCOMPARE(fake.starts, 2); // initial start + restart after the overrun
    }

    void restartsTheStreamAfterAnOverrunReportedByWait()
    {
        FakePcm fake;
        fake.waitErrorAtRead = 10;
        const Outcome o = drive(fake, 40);
        QVERIFY2(o.finished, "audio never resumed after an overrun");
        QCOMPARE(o.chunks, 40);
        QCOMPARE(o.xruns, uint64_t(1));
        QCOMPARE(fake.starts, 2);
    }

    void survivesRepeatedOverruns()
    {
        // Several in a row: each needs its own restart.
        int total = 0;
        for (int round = 0; round < 5; ++round) {
            FakePcm fake;
            fake.overrunAtRead = 3;
            const Outcome o = drive(fake, 10);
            QVERIFY(o.finished);
            QCOMPARE(fake.starts, 2);
            total += o.chunks;
        }
        QCOMPARE(total, 50);
    }

    void eagainIsNotAFaultAndIsNotCountedAsAnXrun()
    {
        FakePcm fake;
        fake.eagainAtRead = 5;
        const Outcome o = drive(fake, 20);
        QVERIFY(o.finished);
        QCOMPARE(o.chunks, 20);
        QCOMPARE(o.xruns, uint64_t(0));
    }

    void anUnrecoverableErrorEndsTheLoopWithAMessage()
    {
        FakePcm fake;
        fake.overrunAtRead = 4;
        fake.recoverFails = true;
        const Outcome o = drive(fake, 100);
        QVERIFY(o.finished);
        QVERIFY(!o.ok);
        QVERIFY(!o.error.empty());
        QVERIFY(o.chunks < 100);
    }

    void stopRequestEndsAnIdleLoopPromptly()
    {
        // A stream that never produces data must not block stop().
        FakePcm fake;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> xruns{0};
        std::string error;
        alsaloop::Pcm p = fake.ops();
        p.start = [] { return 0; }; // never actually runs
        auto fut = std::async(std::launch::async, [&] {
            return alsaloop::run(p, kPeriod, stop, [](const int16_t *, size_t) {}, &xruns, &error);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop = true;
        QVERIFY(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        QVERIFY(fut.get());
    }
};

QTEST_GUILESS_MAIN(TstAlsaLoop)
#include "tst_alsa_loop.moc"
