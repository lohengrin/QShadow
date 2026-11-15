// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

// The seam between the portable application and the operating system's capture
// stack. MainWindow, the decoder, the renderer and the audio pipeline only know
// these interfaces; each platform backend (src/platform/<os>/) implements them:
//
//   Linux:   V4L2 video + ALSA audio, paired through the shared USB device in sysfs
//   Windows: Media Foundation video + WASAPI audio, paired by the USB ID in the device path
//
// Device identifiers are opaque strings owned by the backend (a /dev node, a
// Media Foundation symbolic link, ...); the application only stores and passes
// them back.

#include "audio.h"
#include "capture.h"
#include "device.h"

#include <QList>
#include <QSize>
#include <QString>

#include <memory>

// A running video capture. start() spawns a capture thread that pushes Frames
// into the queue and never blocks on the consumer. The destructor stops and
// joins, so destroying a started source is always safe.
class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    // Opens the device, negotiates the format and starts streaming. False if
    // that failed (the source is then simply not running).
    virtual bool start(FrameQueue *out) = 0;
    virtual void stop() = 0; // asks the thread to finish; does not block
    virtual void join() = 0; // waits for it and releases the device

    virtual QSize negotiatedSize() const = 0;
    virtual int negotiatedFps() const = 0;
    // False once the capture thread has exited (device unplugged, stream error)
    // or if start() failed.
    virtual bool isRunning() const = 0;
    // Suppress start() failure warnings (used while retrying a known failure).
    virtual void setQuiet(bool quiet) = 0;
};

// A running audio capture: reads the device's audio, conditions it with an
// AudioPipeline and feeds the AudioRing. Destroying it stops and joins.
class AudioInput {
public:
    virtual ~AudioInput() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void join() = 0;
    virtual QString errorMessage() const = 0;
    // Current drift correction on the resampling ratio, in ppm.
    virtual int driftPpm() const = 0;
    // Capture-side overruns recovered from so far (data the dongle produced that
    // was not read in time). Backends that cannot tell report 0.
    virtual uint64_t captureXruns() const { return 0; }
};

namespace platform {

// One capture device as the user sees it: a video interface and, if the
// backend could pair one with it, the audio interface of the same hardware.
struct DeviceInfo {
    QString name;    // human-readable (V4L2 card name, Media Foundation friendly name)
    QString videoId; // opaque, valid until the device is unplugged
    QString audioId; // opaque; empty if no audio interface was found
    int vid = 0;     // USB IDs; 0 for non-USB devices. Only used to recognise
    int pid = 0;     // hardware (quirks, pairing), never to select a backend path.

    // Stable across unplug/replug (unlike the ids): what the app persists.
    QString key() const
    {
        return QStringLiteral("%1:%2:%3")
            .arg(vid, 4, 16, QLatin1Char('0')).arg(pid, 4, 16, QLatin1Char('0')).arg(name);
    }
};

// One audio capture endpoint (a sound card's input, a microphone).
struct AudioInputInfo {
    QString name;
    QString id; // opaque, as for DeviceInfo::audioId
};

// Every audio input currently present, in a stable order. Lets the user pick a
// different audio source than the one paired with the video device.
QList<AudioInputInfo> listAudioInputs();

// Every video capture device currently present, in a stable order.
QList<DeviceInfo> listDevices();

// Every mode the device offers for a pixel format, largest first, read with a
// single open of the device. Empty if the format is not offered.
QList<device::Mode> enumerateModes(const QString &videoId, device::PixelFormat format);

// fps 0 selects the highest rate the device offers for the size; otherwise the
// nearest offered rate is used.
std::unique_ptr<CaptureSource> createCapture(const QString &videoId, const QSize &target,
                                             device::PixelFormat format, int fps, Stats *stats);
std::unique_ptr<AudioInput> createAudioInput(const QString &audioId, AudioRing *ring,
                                             Stats *stats);

} // namespace platform
