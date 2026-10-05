#pragma once

#include "device.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <cstdint>

// V4L2 fourcc for a PixelFormat.
inline uint32_t toFourcc(device::PixelFormat format)
{
    return format == device::PixelFormat::Yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
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
