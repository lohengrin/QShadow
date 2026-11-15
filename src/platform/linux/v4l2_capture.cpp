// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "v4l2_capture.h"

#include "v4l2_common.h"
#include "v4l2_device.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <chrono>
#include <QDebug>

namespace {

// Host arrival time of a dequeued buffer. uvcvideo stamps buffers with
// CLOCK_MONOTONIC (the same clock as steady_clock on Linux) at the first USB
// packet of the frame, so this includes the transfer time. Anything implausible
// falls back to "now".
std::chrono::steady_clock::time_point frameStamp(const v4l2_buffer &buf)
{
    const auto now = std::chrono::steady_clock::now();
    if ((buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
        || (buf.timestamp.tv_sec == 0 && buf.timestamp.tv_usec == 0))
        return now;
    const auto ts = std::chrono::steady_clock::time_point(
        std::chrono::seconds(buf.timestamp.tv_sec)
        + std::chrono::microseconds(buf.timestamp.tv_usec));
    if (ts > now || now - ts > std::chrono::seconds(1))
        return now;
    return ts;
}

} // namespace

V4l2Capture::V4l2Capture(const QString &devicePath, const QSize &targetSize,
                         device::PixelFormat format, int fps, Stats *stats)
    : m_devicePath(devicePath), m_targetSize(targetSize), m_format(format),
      m_requestedFps(fps), m_stats(stats)
{
}

V4l2Capture::~V4l2Capture()
{
    stop();
    join();
}

QDebug V4l2Capture::warn()
{
    if (m_quiet) {
        m_discard.clear();
        return QDebug(&m_discard);
    }
    return qWarning();
}

bool V4l2Capture::start(FrameQueue *out)
{
    m_queue = out;

    m_fd = ::open(m_devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0) {
        warn() << "Capture: failed to open" << m_devicePath << strerror(errno);
        return false;
    }

    struct v4l2_capability cap;
    std::memset(&cap, 0, sizeof(cap));
    if (xioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        warn() << "Capture: VIDIOC_QUERYCAP failed:" << strerror(errno);
        close(m_fd);
        m_fd = -1;
        return false;
    }

    // Request the pixel format at the target size; let the driver clamp to the nearest
    // supported mode and read back the actual negotiated format.
    struct v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.pixelformat = toFourcc(m_format);
    fmt.fmt.pix.width = uint32_t(m_targetSize.width());
    fmt.fmt.pix.height = uint32_t(m_targetSize.height());
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        warn() << "Capture: VIDIOC_S_FMT failed:" << strerror(errno);
        close(m_fd);
        m_fd = -1;
        return false;
    }
    if (fmt.fmt.pix.pixelformat != toFourcc(m_format)) {
        warn() << "Capture: driver refused the requested pixel format";
        close(m_fd);
        m_fd = -1;
        return false;
    }
    m_negotiated = QSize(fmt.fmt.pix.width, fmt.fmt.pix.height);
    m_stride = int(fmt.fmt.pix.bytesperline);
    m_stride = std::max(m_stride, device::minStride(m_format, m_negotiated.width()));

    // What the driver says about the colour encoding (devicequirks.h may override).
    {
        const uint32_t cs = fmt.fmt.pix.colorspace;
        uint32_t enc = fmt.fmt.pix.ycbcr_enc;
        if (enc == V4L2_YCBCR_ENC_DEFAULT)
            enc = V4L2_MAP_YCBCR_ENC_DEFAULT(cs);
        uint32_t quant = fmt.fmt.pix.quantization;
        if (quant == V4L2_QUANTIZATION_DEFAULT)
            quant = V4L2_MAP_QUANTIZATION_DEFAULT(false, cs, enc);
        m_color.matrix = enc == V4L2_YCBCR_ENC_709 ? device::ColorMatrix::Bt709
                                                   : device::ColorMatrix::Bt601;
        m_color.fullRange = quant == V4L2_QUANTIZATION_FULL_RANGE;
    }

    // The requested rate if the device offers it, else the nearest one (0: the
    // fastest). 30 if the driver cannot list intervals.
    {
        const QList<int> rates = v4l2dev::offeredRates(m_fd, toFourcc(m_format), m_negotiated);
        m_fps = 30;
        if (!rates.isEmpty()) {
            m_fps = rates.first();
            if (m_requestedFps > 0) {
                for (int r : rates) {
                    if (std::abs(r - m_requestedFps) < std::abs(m_fps - m_requestedFps))
                        m_fps = r;
                }
            }
        }
    }

    struct v4l2_streamparm parm;
    std::memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_G_PARM, &parm) == 0
        && (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = uint32_t(m_fps);
        xioctl(m_fd, VIDIOC_S_PARM, &parm);
    }

    struct v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        warn() << "Capture: VIDIOC_REQBUFS failed:" << strerror(errno);
        close(m_fd);
        m_fd = -1;
        return false;
    }
    m_bufferCount = int(req.count);

    for (int i = 0; i < m_bufferCount; ++i) {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = uint32_t(i);
        if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            warn() << "Capture: VIDIOC_QUERYBUF failed:" << strerror(errno);
            return false;
        }
        m_buffers[i].length = buf.length;
        m_buffers[i].start = mmap(nullptr, buf.length,
                                  PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);
        if (m_buffers[i].start == MAP_FAILED) {
            warn() << "Capture: mmap failed:" << strerror(errno);
            m_buffers[i].start = nullptr;
            return false;
        }
        if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            warn() << "Capture: VIDIOC_QBUF failed:" << strerror(errno);
            return false;
        }
    }

    {
        size_t largest = 0;
        for (int i = 0; i < m_bufferCount; ++i)
            largest = std::max(largest, m_buffers[i].length);
        m_pool.reserve(largest);
    }

    if (xioctl(m_fd, VIDIOC_STREAMON, &fmt.type) < 0) {
        warn() << "Capture: VIDIOC_STREAMON failed:" << strerror(errno);
        return false;
    }

    qInfo() << "Capture:" << m_negotiated.width() << "x" << m_negotiated.height();

    m_streaming = true;
    m_running = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void V4l2Capture::stop()
{
    m_stop.store(true);
}

void V4l2Capture::join()
{
    if (m_thread.joinable())
        m_thread.join();
    if (m_fd >= 0) {
        if (m_streaming) {
            uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(m_fd, VIDIOC_STREAMOFF, &type);
            m_streaming = false;
        }
        for (int i = 0; i < m_bufferCount; ++i) {
            if (m_buffers[i].start)
                munmap(m_buffers[i].start, m_buffers[i].length);
        }
        close(m_fd);
        m_fd = -1;
    }
    m_bufferCount = 0;
}

void V4l2Capture::run()
{
    struct pollfd pfd;
    pfd.fd = m_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    while (!m_stop.load()) {
        const int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;

        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN)
                continue;
            if (errno == EPIPE) {
                // Queue the buffer back and carry on.
                xioctl(m_fd, VIDIOC_QBUF, &buf);
                continue;
            }
            break;
        }

        const size_t used = buf.bytesused;
        // A short raw frame (dropped USB packets) can't be converted: skip it.
        const size_t rawSize = device::rawFrameBytes(m_format, m_stride, m_negotiated.height());
        // An MJPG buffer must hold a real JPEG: the dongle pads some modes with
        // empty 4-byte markers that are not frames.
        const bool complete = m_format == device::PixelFormat::Mjpg
                                  ? buf.index < uint32_t(m_bufferCount)
                                        && isPlausibleJpeg(
                                            static_cast<const char *>(m_buffers[buf.index].start),
                                            used)
                                  : used >= rawSize;
        if (used > 0 && complete && buf.index < uint32_t(m_bufferCount)) {
            Frame frame;
            const char *src = static_cast<const char *>(m_buffers[buf.index].start);
            frame.stamp = frameStamp(buf);
            frame.format = m_format;
            frame.width = m_negotiated.width();
            frame.height = m_negotiated.height();
            frame.stride = m_stride;
            frame.color = m_color;
            frame.data = m_pool.take(src, m_format == device::PixelFormat::Mjpg ? used : rawSize);
            m_stats->captured.fetch_add(1);
            m_queue->push(std::move(frame));
        }

        if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            qWarning() << "Capture: re-VIDIOC_QBUF failed:" << strerror(errno);
            break;
        }
    }
    m_running.store(false);
}