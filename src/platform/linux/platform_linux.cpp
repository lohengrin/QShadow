// Linux backend of the platform seam: V4L2 video, ALSA audio, sysfs discovery.

#include "platform/platform.h"

#include "alsa_audio.h"
#include "v4l2_capture.h"
#include "v4l2_device.h"

platform::DeviceIds platform::findDevice()
{
    DeviceIds ids;
    ids.video = v4l2dev::findNode();
    if (!ids.video.isEmpty()) {
        const int card = alsa::findCard();
        if (card >= 0)
            ids.audio = QString::number(card);
    }
    return ids;
}

QList<device::Mode> platform::enumerateModes(const QString &videoId, device::PixelFormat format)
{
    return v4l2dev::enumerateModes(videoId, format);
}

std::unique_ptr<CaptureSource> platform::createCapture(const QString &videoId, const QSize &target,
                                                       device::PixelFormat format, Stats *stats)
{
    return std::make_unique<V4l2Capture>(videoId, target, format, stats);
}

std::unique_ptr<AudioInput> platform::createAudioInput(const QString &audioId, AudioRing *ring,
                                                       Stats *stats)
{
    bool ok = false;
    const int card = audioId.toInt(&ok);
    return std::make_unique<AlsaAudioInput>(ok ? card : -1, ring, stats);
}
