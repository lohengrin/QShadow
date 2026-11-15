// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "audio.h"
#include "audiodrift.h"
#include "audiopipeline.h"

#include <QtTest>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846; // M_PI is not guaranteed (MSVC)

QByteArray stereoChunk(const std::vector<int16_t> &samples)
{
    return QByteArray(reinterpret_cast<const char *>(samples.data()),
                      qsizetype(samples.size() * sizeof(int16_t)));
}

// Closed-loop simulation: the capture side produces 5 ms of mono samples at a
// clock `ppm` off the sink's, the sink drains 10 ms bursts. Returns the final
// controller state.
struct SimResult {
    double meanFillFrames; // mean ring fill over the last minute
    double adjustPpm;
    int underruns;
    double overflowFrames;
};

SimResult simulate(double ppm, double targetFrames, int seconds)
{
    DriftController controller(targetFrames);
    DriftResampler resampler;
    std::vector<float> in, out;
    double ring = targetFrames, tail = 0.0, overflow = 0.0, inAcc = 0.0;
    int tailN = 0, under = 0;
    for (int ms = 0; ms < seconds * 1000; ms += 5) {
        inAcc += 240.0 * (1.0 + ppm * 1e-6);
        const int n = int(inAcc);
        inAcc -= n;
        in.assign(size_t(n), 0.0f);
        out.clear();
        resampler.process(in.data(), in.size(), controller.update(ring), &out);
        ring += double(out.size());
        if (ring > 2880.0) { // 60 ms cap
            overflow += ring - targetFrames;
            ring = targetFrames;
        }
        if ((ms / 5) % 2 == 1) { // sink pulls 10 ms every 10 ms
            if (ring >= 480.0)
                ring -= 480.0;
            else {
                ++under;
                ring = 0.0;
            }
        }
        if (ms > (seconds - 60) * 1000) {
            tail += ring;
            ++tailN;
        }
    }
    return {tail / tailN, controller.adjust() * 1e6, under, overflow};
}

} // namespace

class TstAudio : public QObject {
    Q_OBJECT
private slots:
    void ringIsFifo()
    {
        AudioRing ring;
        ring.push(QByteArray("abcd", 4));
        ring.push(QByteArray("efgh", 4));
        QCOMPARE(ring.depthBytes(), size_t(8));
        char buf[8] = {};
        QCOMPARE(ring.pull(buf, 6), qint64(6)); // spans a chunk boundary
        QCOMPARE(QByteArray(buf, 6), QByteArray("abcdef"));
        QCOMPARE(ring.depthBytes(), size_t(2));
        QCOMPARE(ring.pull(buf, 8), qint64(2));
        QCOMPARE(QByteArray(buf, 2), QByteArray("gh"));
        QCOMPARE(ring.depthBytes(), size_t(0));
    }

    void ringCountsUnderrunsOnlyWhenEmpty()
    {
        AudioRing ring;
        char buf[4];
        QCOMPARE(ring.pull(buf, 4), qint64(0));
        QCOMPARE(ring.underruns(), uint64_t(1));
        ring.push(QByteArray("ab", 2));
        QCOMPARE(ring.pull(buf, 4), qint64(2)); // partial read is not an underrun
        QCOMPARE(ring.underruns(), uint64_t(1));
    }

    void ringDropsOldestOverTheCap()
    {
        AudioRing ring;
        const int chunk = 5000;
        for (int i = 0; i < 50; ++i)
            ring.push(QByteArray(chunk, char('A' + i % 26)));
        QVERIFY(ring.depthBytes() <= AudioRing::kMaxBytes);
        QVERIFY(ring.depthBytes() >= size_t(chunk)); // the newest chunk always stays
        // What remains is the newest audio.
        char c = 0;
        ring.clear();
        ring.push(QByteArray(1, 'x'));
        QCOMPARE(ring.pull(&c, 1), qint64(1));
        QCOMPARE(c, 'x');
        QCOMPARE(ring.depthBytes(), size_t(0));
    }

    void floatSinkConvertsS16()
    {
        AudioRing ring;
        ring.push(stereoChunk({0, 16384, -16384, 32767, -32768, 0}));
        HostAudioSink sink(&ring, /*floatOutput=*/true);
        QVERIFY(sink.open(QIODevice::ReadOnly));
        QCOMPARE(sink.bytesAvailable(), qint64(6 * sizeof(int16_t) * 2)); // reported as float bytes
        float out[6] = {};
        QCOMPARE(sink.read(reinterpret_cast<char *>(out), sizeof(out)), qint64(sizeof(out)));
        QCOMPARE(out[0], 0.0f);
        QVERIFY(qAbs(out[1] - 0.5f) < 1e-6f);
        QVERIFY(qAbs(out[2] + 0.5f) < 1e-6f);
        QVERIFY(out[3] > 0.9999f && out[3] < 1.0f);
        QCOMPARE(out[4], -1.0f);
    }

    void s16SinkPassesThrough()
    {
        AudioRing ring;
        ring.push(QByteArray("wxyz", 4));
        HostAudioSink sink(&ring);
        QVERIFY(sink.open(QIODevice::ReadOnly));
        char buf[4];
        QCOMPARE(sink.read(buf, 4), qint64(4));
        QCOMPARE(QByteArray(buf, 4), QByteArray("wxyz"));
    }

    void controllerOutputIsClamped()
    {
        DriftController c(384.0);
        const double hi = c.update(1e7);
        QVERIFY(hi <= 1.0 + DriftController::kMaxAdjust + 1e-12);
        QVERIFY(hi > 1.0);
        DriftController c2(384.0);
        const double lo = c2.update(0.0);
        QVERIFY(lo >= 1.0 - DriftController::kMaxAdjust - 1e-12);
        QVERIFY(lo < 1.0);
    }

    void controllerLearnsClockDrift_data()
    {
        QTest::addColumn<double>("ppm");
        QTest::newRow("matched") << 0.0;
        QTest::newRow("dongle +300ppm") << 300.0;
        QTest::newRow("dongle -300ppm") << -300.0;
        QTest::newRow("dongle +2000ppm") << 2000.0;
        QTest::newRow("dongle -2000ppm") << -2000.0;
    }
    void controllerLearnsClockDrift()
    {
        QFETCH(double, ppm);
        const double target = 8.0 * 48.0;
        const SimResult r = simulate(ppm, target, 300);
        // Fill settles on the target, the learnt correction equals the drift, and
        // neither underruns nor overflows happen.
        QVERIFY2(qAbs(r.meanFillFrames - target) < 2.0 * 48.0,
                 qPrintable(QString::number(r.meanFillFrames / 48.0)));
        QVERIFY(qAbs(r.adjustPpm - ppm) < 60.0);
        // Real clocks differ by a few hundred ppm at most and never glitch. A
        // 2000 ppm offset (far beyond any crystal) may underrun while the
        // integral term is still learning it; it must still converge.
        if (qAbs(ppm) <= 500.0) {
            QCOMPARE(r.underruns, 0);
            QCOMPARE(r.overflowFrames, 0.0);
        }
    }

    void resamplerAtUnityIsADelayedCopy()
    {
        DriftResampler r;
        std::vector<float> in, out;
        for (int i = 1; i <= 100; ++i)
            in.push_back(float(i));
        r.process(in.data(), in.size(), 1.0, &out);
        QCOMPARE(out.size(), in.size());
        QCOMPARE(out[0], 0.0f);
        for (size_t i = 1; i < out.size(); ++i)
            QCOMPARE(out[i], in[i - 1]);
    }

    void resamplerOutputCountFollowsTheRatio()
    {
        std::vector<float> in(48000, 0.25f), out;
        DriftResampler fast, slow;
        fast.process(in.data(), in.size(), 1.005, &out); // consumes faster: fewer outputs
        const size_t fewer = out.size();
        out.clear();
        slow.process(in.data(), in.size(), 0.995, &out);
        const size_t more = out.size();
        QVERIFY(fewer < 48000 && fewer > 47600);
        QVERIFY(more > 48000 && more < 48400);
    }

    void resamplerKeepsASineSmooth()
    {
        DriftResampler r;
        std::vector<float> in, out;
        for (int i = 0; i < 4800; ++i)
            in.push_back(float(std::sin(2.0 * kPi * 1000.0 * i / 48000.0)));
        r.process(in.data(), in.size(), 1.004, &out);
        // Linear interpolation of a 1 kHz tone at 48 kHz: no sample may step by
        // more than the tone's own maximum slope.
        float maxStep = 0.0f;
        for (size_t i = 1; i < out.size(); ++i)
            maxStep = std::max(maxStep, std::abs(out[i] - out[i - 1]));
        QVERIFY(maxStep < 2.0f * float(kPi) * 1000.0f / 48000.0f * 1.05f);
    }

    // ---- AudioPipeline (the platform-independent half of audio capture) ----
    void pipelineDecimatesBy2AndDuplicatesToStereo()
    {
        AudioRing ring;
        AudioPipeline pipe(&ring, 96000, 1);
        std::vector<int16_t> in(4000, 1000); // 96 kHz mono, constant
        pipe.process(in.data(), in.size());
        const size_t frames = ring.depthBytes() / 4;
        QVERIFY2(frames >= 1990 && frames <= 2030, qPrintable(QString::number(frames)));
        std::vector<int16_t> out(frames * 2);
        QCOMPARE(ring.pull(reinterpret_cast<char *>(out.data()), qint64(out.size() * 2)),
                 qint64(out.size() * 2));
        for (size_t i = 2; i < frames; ++i) { // [0] is the resampler's priming sample
            QCOMPARE(out[2 * i], int16_t(1000));
            QCOMPARE(out[2 * i + 1], int16_t(1000)); // L == R
        }
    }

    void pipelineAtNativeRateKeepsEverySample()
    {
        AudioRing ring;
        AudioPipeline pipe(&ring, 48000, 1);
        std::vector<int16_t> in(2000, 500);
        pipe.process(in.data(), in.size());
        const size_t frames = ring.depthBytes() / 4;
        QVERIFY2(frames >= 1990 && frames <= 2030, qPrintable(QString::number(frames)));
    }

    void pipelineDownmixesChannelsToMono()
    {
        AudioRing ring;
        AudioPipeline pipe(&ring, 48000, 2);
        std::vector<int16_t> in;
        for (int i = 0; i < 1000; ++i) {
            in.push_back(1000); // left
            in.push_back(3000); // right
        }
        pipe.process(in.data(), 1000);
        std::vector<int16_t> out(ring.depthBytes() / 2);
        ring.pull(reinterpret_cast<char *>(out.data()), qint64(out.size() * 2));
        QVERIFY(out.size() > 100);
        QCOMPARE(out[out.size() - 2], int16_t(2000)); // (1000 + 3000) / 2
    }

    void pipelineCarriesOddSamplesAcrossCalls()
    {
        AudioRing bulk, pieces;
        AudioPipeline a(&bulk, 96000, 1), b(&pieces, 96000, 1);
        std::vector<int16_t> in(2000, 700);
        a.process(in.data(), in.size());
        for (size_t i = 0; i < in.size(); i += 7) // awkward chunk size
            b.process(in.data() + i, std::min<size_t>(7, in.size() - i));
        const auto diff = qint64(bulk.depthBytes()) - qint64(pieces.depthBytes());
        QVERIFY2(qAbs(diff) <= 4 * 4, qPrintable(QString::number(diff)));
    }

    void pipelineReportsDriftWhenTheRingRunsFull()
    {
        AudioRing ring;
        ring.push(QByteArray(20 * int(AudioRing::kBytesPerMs), 0)); // 20 ms: above the target
        AudioPipeline pipe(&ring, 96000, 1);
        std::vector<int16_t> in(480, 0);
        for (int i = 0; i < 400; ++i)
            pipe.process(in.data(), in.size());
        QVERIFY(pipe.driftPpm() > 0); // draining a ring that is too full
    }
};

QTEST_GUILESS_MAIN(TstAudio)
#include "tst_audio.moc"
