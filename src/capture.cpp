#include "capture.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>

#include <chrono>
#include <QDebug>

namespace {

int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

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

Capture::Capture(const QString &devicePath, const QSize &targetSize,
                 device::PixelFormat format, Stats *stats)
    : m_devicePath(devicePath), m_targetSize(targetSize), m_format(format), m_stats(stats)
{
}

Capture::~Capture()
{
    stop();
    join();
}

QByteArray Capture::takeBuffer(const char *src, size_t used)
{
    for (QByteArray &b : m_pool) {
        // isDetached(): nobody else shares this array, so overwriting is safe.
        if (b.isDetached() && size_t(b.capacity()) >= used) {
            b.resize(qsizetype(used)); // within capacity: no reallocation
            std::memcpy(b.data(), src, used);
            return b; // shared copy; the pool keeps its reference
        }
    }
    return QByteArray(src, qsizetype(used));
}

QDebug Capture::warn()
{
    if (m_quiet) {
        m_discard.clear();
        return QDebug(&m_discard);
    }
    return qWarning();
}

bool Capture::start(FrameQueue *out)
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
    fmt.fmt.pix.pixelformat = device::toFourcc(m_format);
    fmt.fmt.pix.width = uint32_t(m_targetSize.width());
    fmt.fmt.pix.height = uint32_t(m_targetSize.height());
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        warn() << "Capture: VIDIOC_S_FMT failed:" << strerror(errno);
        close(m_fd);
        m_fd = -1;
        return false;
    }
    if (fmt.fmt.pix.pixelformat != device::toFourcc(m_format)) {
        warn() << "Capture: driver refused the requested pixel format";
        close(m_fd);
        m_fd = -1;
        return false;
    }
    m_negotiated = QSize(fmt.fmt.pix.width, fmt.fmt.pix.height);
    m_stride = int(fmt.fmt.pix.bytesperline);
    if (m_format == device::PixelFormat::Yuyv && m_stride < m_negotiated.width() * 2)
        m_stride = m_negotiated.width() * 2;

    // Fastest offered interval for the negotiated size.
    {
        struct v4l2_frmivalenum fiv;
        std::memset(&fiv, 0, sizeof(fiv));
        fiv.pixel_format = device::toFourcc(m_format);
        fiv.width = uint32_t(m_negotiated.width());
        fiv.height = uint32_t(m_negotiated.height());
        double bestUs = 0.0;
        bool any = false;
        while (xioctl(m_fd, VIDIOC_ENUM_FRAMEINTERVALS, &fiv) == 0) {
            if (fiv.type == V4L2_FRMIVAL_TYPE_DISCRETE && fiv.discrete.denominator > 0) {
                const double us = double(fiv.discrete.numerator) * 1e6
                                  / double(fiv.discrete.denominator);
                if (!any || us < bestUs) {
                    bestUs = us;
                    any = true;
                }
            }
            ++fiv.index;
        }
        m_fps = any ? int(1e6 / bestUs + 0.5) : 30;
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
        for (QByteArray &b : m_pool) {
            b = QByteArray();
            b.reserve(qsizetype(largest));
        }
    }

    if (xioctl(m_fd, VIDIOC_STREAMON, &fmt.type) < 0) {
        warn() << "Capture: VIDIOC_STREAMON failed:" << strerror(errno);
        return false;
    }

    qInfo() << "Capture:" << m_negotiated.width() << "x" << m_negotiated.height();

    m_running = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void Capture::stop()
{
    m_stop.store(true);
}

void Capture::join()
{
    if (m_thread.joinable())
        m_thread.join();
    if (m_fd >= 0) {
        if (m_running.load()) {
            uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(m_fd, VIDIOC_STREAMOFF, &type);
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

void Capture::run()
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
        // A short YUYV frame (dropped USB packets) can't be converted: skip it.
        const size_t rawSize = size_t(m_stride) * size_t(m_negotiated.height());
        const bool complete = m_format == device::PixelFormat::Mjpg || used >= rawSize;
        if (used > 0 && complete && buf.index < uint32_t(m_bufferCount)) {
            Frame frame;
            const char *src = static_cast<const char *>(m_buffers[buf.index].start);
            frame.stamp = frameStamp(buf);
            frame.format = m_format;
            frame.width = m_negotiated.width();
            frame.height = m_negotiated.height();
            frame.stride = m_stride;
            frame.data = takeBuffer(src, m_format == device::PixelFormat::Yuyv ? rawSize : used);
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