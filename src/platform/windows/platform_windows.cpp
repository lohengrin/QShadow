// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

// Windows backend of the platform seam: Media Foundation video, WASAPI audio.

#include "platform/platform.h"

#include "mf_capture.h"
#include "mf_device.h"
#include "wasapi_audio.h"

QList<platform::DeviceInfo> platform::listDevices()
{
    QList<DeviceInfo> devices;
    for (const mfdev::Video &video : mfdev::listVideo()) {
        DeviceInfo info;
        info.name = video.name;
        info.videoId = video.link;
        wincom::parseVidPid(video.link, &info.vid, &info.pid);
        if (info.vid != 0)
            info.audioId = wasapi::findCaptureEndpoint(info.vid, info.pid);
        devices.append(info);
    }
    return devices;
}

QList<platform::AudioInputInfo> platform::listAudioInputs()
{
    return wasapi::listCaptureEndpoints();
}

QList<device::Mode> platform::enumerateModes(const QString &videoId, device::PixelFormat format)
{
    return mfdev::enumerateModes(videoId, format);
}

std::unique_ptr<CaptureSource> platform::createCapture(const QString &videoId, const QSize &target,
                                                       device::PixelFormat format, int fps,
                                                       Stats *stats)
{
    return std::make_unique<MfCapture>(videoId, target, format, fps, stats);
}

std::unique_ptr<AudioInput> platform::createAudioInput(const QString &audioId, AudioRing *ring,
                                                        Stats *stats)
{
    return std::make_unique<WasapiAudioInput>(audioId, ring, stats);
}
