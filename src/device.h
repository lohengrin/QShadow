#pragma once

#include <QList>
#include <QSize>

namespace device {

// USB IDs of the GENKI ShadowCast, used by every platform backend to find it.
inline constexpr int kUsbVendorId = 0x298f;
inline constexpr int kUsbProductId = 0x1996;

// Pixel formats the app can capture. MJPG is the dongle's compressed
// full-rate mode; YUYV is uncompressed (no JPEG artifacts) but limited by
// USB 2.0 bandwidth to low resolutions / frame rates.
enum class PixelFormat { Mjpg = 0, Yuyv = 1 };

// A capture size with the highest frame rate the device offers for it.
struct Mode {
    QSize size;
    int fps = 0;
    bool operator==(const Mode &other) const { return size == other.size && fps == other.fps; }
};

} // namespace device
