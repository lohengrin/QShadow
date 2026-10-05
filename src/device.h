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

// All capture sizes the device exposes for a pixel format, largest first.
// Empty if the format is not offered.
QList<QSize> enumerateSizes(const QString &devicePath, PixelFormat format);

// Maximum frame rate offered for the given size and format.
int maxFpsForSize(const QString &devicePath, const QSize &size, PixelFormat format);

} // namespace device