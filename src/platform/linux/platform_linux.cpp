// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

// Linux backend of the platform seam: V4L2 video, ALSA audio, sysfs discovery.

#include "platform/platform.h"

#include "alsa_audio.h"
#include "sysfs_usb.h"
#include "v4l2_capture.h"
#include "v4l2_device.h"

#include <QFile>

#include <alsa/asoundlib.h>

#include <cstdlib>

QList<platform::DeviceInfo> platform::listDevices()
{
    QList<DeviceInfo> devices;
    for (const v4l2dev::Node &node : v4l2dev::listNodes()) {
        DeviceInfo info;
        info.name = node.name;
        info.videoId = node.path;
        info.vid = node.vid;
        info.pid = node.pid;
        const int card = sysfs_usb::findSoundCardOnUsbDevice(node.usbDir);
        if (card >= 0)
            info.audioId = QString::number(card);
        devices.append(info);
    }
    return devices;
}

QList<platform::AudioInputInfo> platform::listAudioInputs()
{
    QList<AudioInputInfo> inputs;
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        // Only cards with a capture PCM (pcm0c: the device the backend opens).
        if (!QFile::exists(QStringLiteral("/proc/asound/card%1/pcm0c").arg(card)))
            continue;
        char *name = nullptr;
        AudioInputInfo info;
        if (snd_card_get_name(card, &name) == 0) {
            info.name = QString::fromLocal8Bit(name);
            free(name);
        } else {
            info.name = QStringLiteral("Card %1").arg(card);
        }
        info.id = QString::number(card);
        inputs.append(info);
    }
    return inputs;
}

QList<device::Mode> platform::enumerateModes(const QString &videoId, device::PixelFormat format)
{
    return v4l2dev::enumerateModes(videoId, format);
}

std::unique_ptr<CaptureSource> platform::createCapture(const QString &videoId, const QSize &target,
                                                       device::PixelFormat format, int fps,
                                                       Stats *stats)
{
    return std::make_unique<V4l2Capture>(videoId, target, format, fps, stats);
}

std::unique_ptr<AudioInput> platform::createAudioInput(const QString &audioId, AudioRing *ring,
                                                       Stats *stats)
{
    bool ok = false;
    const int card = audioId.toInt(&ok);
    return std::make_unique<AlsaAudioInput>(ok ? card : -1, ring, stats);
}
