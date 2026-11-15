// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

// Smoke test of the platform backend against the first real capture device.
// Skipped when none is connected, so it is safe to run anywhere.

#include "platform/platform.h"

#include <QtTest>

#include <chrono>
#include <thread>

class TstPlatform : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const auto devices = platform::listDevices();
        if (devices.isEmpty())
            QSKIP("no video capture device connected");
        for (const auto &d : devices) {
            QVERIFY(!d.videoId.isEmpty());
            QVERIFY(!d.name.isEmpty());
        }
        // Prefer a device whose MJPG modes the tests below can use.
        m_dev = devices.first();
        for (const auto &d : devices) {
            if (!platform::enumerateModes(d.videoId, device::PixelFormat::Mjpg).isEmpty()) {
                m_dev = d;
                break;
            }
        }
        if (platform::enumerateModes(m_dev.videoId, device::PixelFormat::Mjpg).isEmpty())
            QSKIP("no connected device offers MJPG");
    }

    void modesAreLargestFirst()
    {
        const auto mjpg = platform::enumerateModes(m_dev.videoId, device::PixelFormat::Mjpg);
        QVERIFY(!mjpg.isEmpty());
        for (qsizetype i = 1; i < mjpg.size(); ++i) {
            const QSize a = mjpg[i - 1].size, b = mjpg[i].size;
            QVERIFY(a.width() * a.height() > b.width() * b.height());
        }
        for (const auto &mode : mjpg)
            QVERIFY(mode.fps > 0);
        // YUYV may or may not be offered; if it is, it must be well-formed too.
        for (const auto &mode : platform::enumerateModes(m_dev.videoId, device::PixelFormat::Yuyv))
            QVERIFY(mode.size.isValid());
    }

    void captureDeliversFramesAndRestarts()
    {
        const auto modes = platform::enumerateModes(m_dev.videoId, device::PixelFormat::Mjpg);
        QVERIFY(!modes.isEmpty());
        for (int round = 0; round < 3; ++round) {
            Stats stats;
            FrameQueue queue(2);
            auto capture = platform::createCapture(m_dev.videoId, modes.last().size,
                                                   device::PixelFormat::Mjpg, 0, &stats);
            QVERIFY(capture->start(&queue));
            QVERIFY(capture->isRunning());
            QCOMPARE(capture->negotiatedSize(), modes.last().size);

            Frame frame;
            QVERIFY(queue.pop(&frame, false, 3000)); // a frame arrives
            QCOMPARE(frame.format, device::PixelFormat::Mjpg);
            QVERIFY(frame.data.size() > 64);
            QVERIFY(frame.stamp <= std::chrono::steady_clock::now());
            QVERIFY(frame.stamp > std::chrono::steady_clock::now() - std::chrono::seconds(2));

            capture->stop();
            capture->join();
            QVERIFY(!capture->isRunning());
        }
    }

    void destroyingAStartedCaptureIsSafe()
    {
        const auto modes = platform::enumerateModes(m_dev.videoId, device::PixelFormat::Mjpg);
        Stats stats;
        FrameQueue queue(2);
        {
            auto capture = platform::createCapture(m_dev.videoId, modes.last().size,
                                                   device::PixelFormat::Mjpg, 0, &stats);
            QVERIFY(capture->start(&queue));
        } // destructor stops and joins while samples may be in flight
        QVERIFY(true);
    }

    void audioStartsAndStops()
    {
        if (m_dev.audioId.isEmpty())
            QSKIP("no audio interface paired with the device");
        for (int round = 0; round < 3; ++round) {
            Stats stats;
            AudioRing ring;
            auto audio = platform::createAudioInput(m_dev.audioId, &ring, &stats);
            QVERIFY2(audio->start(), qPrintable(audio->errorMessage()));
            QTRY_VERIFY_WITH_TIMEOUT(ring.depthBytes() > 0, 3000);
            audio->stop();
            audio->join();
        }
    }

private:
    platform::DeviceInfo m_dev;
};

QTEST_GUILESS_MAIN(TstPlatform)
#include "tst_platform.moc"
