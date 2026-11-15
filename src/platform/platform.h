// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

// The seam between the portable application and the operating system's capture
// stack. MainWindow, the decoder, the renderer and the audio pipeline only know
// these interfaces; each platform backend (src/platform/<os>/) implements them:
//
//   Linux:   V4L2 video + ALSA audio, found through sysfs
//   Windows: Media Foundation video + WASAPI audio, found by USB ID in the device path
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

// A running audio capture: reads the dongle's audio, conditions it with an
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

// Opaque identifiers of the ShadowCast's video and audio interfaces.
struct DeviceIds {
    QString video; // empty if the dongle is not present
    QString audio; // empty if its audio interface was not found
};

// Looks the dongle up by its USB IDs (never by a hard-coded node or name).
DeviceIds findDevice();

// Every mode the device offers for a pixel format, largest first, read with a
// single open of the device. Empty if the format is not offered.
QList<device::Mode> enumerateModes(const QString &videoId, device::PixelFormat format);

std::unique_ptr<CaptureSource> createCapture(const QString &videoId, const QSize &target,
                                             device::PixelFormat format, Stats *stats);
std::unique_ptr<AudioInput> createAudioInput(const QString &audioId, AudioRing *ring,
                                             Stats *stats);

} // namespace platform
