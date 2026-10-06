// Windows backend of the platform seam: Media Foundation video, WASAPI audio.

#include "platform/platform.h"

#include "mf_capture.h"
#include "mf_device.h"
#include "wasapi_audio.h"

platform::DeviceIds platform::findDevice()
{
    DeviceIds ids;
    ids.video = mfdev::findVideo();
    if (!ids.video.isEmpty())
        ids.audio = wasapi::findCaptureEndpoint();
    return ids;
}

QList<device::Mode> platform::enumerateModes(const QString &videoId, device::PixelFormat format)
{
    return mfdev::enumerateModes(videoId, format);
}

std::unique_ptr<CaptureSource> platform::createCapture(const QString &videoId, const QSize &target,
                                                       device::PixelFormat format, Stats *stats)
{
    return std::make_unique<MfCapture>(videoId, target, format, stats);
}

std::unique_ptr<AudioInput> platform::createAudioInput(const QString &audioId, AudioRing *ring,
                                                        Stats *stats)
{
    return std::make_unique<WasapiAudioInput>(audioId, ring, stats);
}
