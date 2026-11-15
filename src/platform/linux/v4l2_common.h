// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "device.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <cstdint>

// V4L2 fourcc for a PixelFormat.
inline uint32_t toFourcc(device::PixelFormat format)
{
    switch (format) {
    case device::PixelFormat::Yuyv: return V4L2_PIX_FMT_YUYV;
    case device::PixelFormat::Nv12: return V4L2_PIX_FMT_NV12;
    case device::PixelFormat::Uyvy: return V4L2_PIX_FMT_UYVY;
    case device::PixelFormat::Rgb24: return V4L2_PIX_FMT_RGB24;
    case device::PixelFormat::Bgr24: return V4L2_PIX_FMT_BGR24;
    case device::PixelFormat::Bgrx: return V4L2_PIX_FMT_BGR32;
    case device::PixelFormat::Mjpg: break;
    }
    return V4L2_PIX_FMT_MJPEG;
}

// ioctl() restarted on EINTR.
inline int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}
