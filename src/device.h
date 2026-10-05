#pragma once

#include <QList>
#include <QSize>
#include <QString>

#include <cstdint>

namespace device {

// Pixel formats the app can capture. MJPG is the dongle's compressed
// full-rate mode; YUYV is uncompressed (no JPEG artifacts) but limited by
// USB 2.0 bandwidth to low resolutions / frame rates.
enum class PixelFormat { Mjpg = 0, Yuyv = 1 };

// V4L2 fourcc for a PixelFormat.
uint32_t toFourcc(PixelFormat format);

// Returns the /dev/videoN node of the ShadowCast capture device (298f:1996),
// found by walking sysfs for the USB vendor/product IDs. Empty string if not
// present.
QString findShadowCastDevice();

// A capture size with the highest frame rate the device offers for it.
struct Mode {
    QSize size;
    int fps = 0;
    bool operator==(const Mode &other) const { return size == other.size && fps == other.fps; }
};

// All modes the device exposes for a pixel format, largest first, read with a
// single open of the device. Empty if the format is not offered.
QList<Mode> enumerateModes(const QString &devicePath, PixelFormat format);

// ALSA card index of the ShadowCast's USB audio interface, found by walking
// sysfs for the USB vendor/product IDs (-1 if absent).
int findShadowCastAudioCard();

} // namespace device