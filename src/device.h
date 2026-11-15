// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include <QList>
#include <QSize>

#include <cstddef>

namespace device {

// USB IDs of the GENKI ShadowCast, the one device with known quirks.
inline constexpr int kUsbVendorId = 0x298f;
inline constexpr int kUsbProductId = 0x1996;

// Pixel formats the app can capture. MJPG is a compressed stream (JPEG per
// frame); the others are uncompressed and artifact-free but limited by USB
// bandwidth to lower resolutions / frame rates. Values are persisted: append only.
enum class PixelFormat {
    Mjpg = 0,
    Yuyv = 1, // packed 4:2:2, memory order Y0 U Y1 V
    Nv12 = 2, // planar Y then interleaved UV, 4:2:0
    Uyvy = 3, // packed 4:2:2, memory order U Y0 V Y1
    Rgb24 = 4, // memory order R G B
    Bgr24 = 5, // memory order B G R
    Bgrx = 6,  // memory order B G R X (32 bit, alpha ignored)
};

inline constexpr PixelFormat kAllFormats[] = {
    PixelFormat::Mjpg, PixelFormat::Yuyv, PixelFormat::Nv12, PixelFormat::Uyvy,
    PixelFormat::Rgb24, PixelFormat::Bgr24, PixelFormat::Bgrx,
};

inline const char *formatName(PixelFormat f)
{
    switch (f) {
    case PixelFormat::Mjpg: return "MJPG";
    case PixelFormat::Yuyv: return "YUYV";
    case PixelFormat::Nv12: return "NV12";
    case PixelFormat::Uyvy: return "UYVY";
    case PixelFormat::Rgb24: return "RGB24";
    case PixelFormat::Bgr24: return "BGR24";
    case PixelFormat::Bgrx: return "BGRX";
    }
    return "?";
}

// Smallest valid row stride in bytes for an uncompressed format (0 for MJPG).
inline int minStride(PixelFormat f, int width)
{
    switch (f) {
    case PixelFormat::Yuyv:
    case PixelFormat::Uyvy: return width * 2;
    case PixelFormat::Nv12: return width;
    case PixelFormat::Rgb24:
    case PixelFormat::Bgr24: return width * 3;
    case PixelFormat::Bgrx: return width * 4;
    case PixelFormat::Mjpg: break;
    }
    return 0;
}

// Bytes of one uncompressed frame with the given stride (0 for MJPG).
inline size_t rawFrameBytes(PixelFormat f, int stride, int height)
{
    const size_t luma = size_t(stride) * size_t(height);
    switch (f) {
    case PixelFormat::Nv12: return luma + luma / 2; // chroma plane: stride x height/2
    case PixelFormat::Mjpg: return 0;
    default: return luma;
    }
}

// How YCbCr samples map to RGB. Backends fill it from what the driver reports;
// the default (BT.601, full range) is what JPEG/JFIF uses.
enum class ColorMatrix { Bt601, Bt709 };
struct ColorInfo {
    ColorMatrix matrix = ColorMatrix::Bt601;
    bool fullRange = true; // false: Y 16..235, CbCr 16..240
    bool operator==(const ColorInfo &o) const { return matrix == o.matrix && fullRange == o.fullRange; }
};

// A capture size with the frame rates the device offers for it.
struct Mode {
    QSize size;
    int fps = 0;         // the highest rate
    QList<int> rates;    // every offered rate, highest first (may be empty: only `fps` known)
    bool operator==(const Mode &o) const { return size == o.size && fps == o.fps && rates == o.rates; }
};

} // namespace device
