// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "v4l2_device.h"

#include "sysfs_usb.h"
#include "v4l2_common.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <functional>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace v4l2dev {

QList<Node> listNodes()
{
    QList<Node> result;
    QDir root(QStringLiteral("/sys/class/video4linux"));
    QStringList nodes = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    // videoN in numeric order (video10 after video2).
    auto number = [](const QString &n) { return n.mid(5).toInt(); };
    std::sort(nodes.begin(), nodes.end(), [&](const QString &a, const QString &b) {
        return number(a) < number(b);
    });
    for (const QString &node : std::as_const(nodes)) {
        if (!node.startsWith(QLatin1String("video")))
            continue;
        const QString devPath = QStringLiteral("/dev/") + node;
        const int fd = ::open(devPath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        struct v4l2_capability cap;
        std::memset(&cap, 0, sizeof(cap));
        const bool ok = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
        close(fd);
        if (!ok)
            continue;

        // device_caps describes this node alone (capabilities covers the whole
        // device, so a UVC camera's metadata node would otherwise look like video).
        const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                        : cap.capabilities;
        if (!(caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)))
            continue;
        // Only single-planar capture is implemented by the backend.
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING))
            continue;

        Node n;
        n.path = devPath;
        n.name = QString::fromLocal8Bit(reinterpret_cast<const char *>(cap.card));
        const QString resolved = QFileInfo(root.filePath(node)).symLinkTarget();
        if (resolved.startsWith(QStringLiteral("/sys/devices"))) {
            n.usbDir = sysfs_usb::usbDeviceDir(resolved);
            if (!n.usbDir.isEmpty())
                sysfs_usb::readUsbIds(n.usbDir, &n.vid, &n.pid);
        }
        result.append(n);
    }
    return result;
}

namespace {

// Every discrete frame rate offered for one frame size, highest first. A
// continuous/stepwise interval range contributes its maximum rate only.
QList<int> frameRates(int fd, uint32_t fourcc, const QSize &size)
{
    struct v4l2_frmivalenum fiv;
    std::memset(&fiv, 0, sizeof(fiv));
    fiv.pixel_format = fourcc;
    fiv.width = uint32_t(size.width());
    fiv.height = uint32_t(size.height());

    QList<int> rates;
    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fiv) == 0) {
        if (fiv.type == V4L2_FRMIVAL_TYPE_DISCRETE && fiv.discrete.denominator > 0
            && fiv.discrete.numerator > 0) {
            rates.append(int(double(fiv.discrete.denominator) / double(fiv.discrete.numerator)
                             + 0.5));
        } else if (fiv.type != V4L2_FRMIVAL_TYPE_DISCRETE && fiv.stepwise.min.numerator > 0
                   && fiv.stepwise.min.denominator > 0) {
            rates.append(int(double(fiv.stepwise.min.denominator)
                                 / double(fiv.stepwise.min.numerator) + 0.5));
            break; // the range is the only entry
        }
        ++fiv.index;
    }
    std::sort(rates.begin(), rates.end(), std::greater<int>());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

// Sizes of a continuous/stepwise range: a webcam-style list of the standard
// resolutions that fit, never the full width x height grid.
void appendStandardSizes(QList<QSize> &sizes, const v4l2_frmsize_stepwise &r)
{
    static const QSize kStandard[] = {
        {160, 120},  {320, 180},  {320, 240},  {352, 288},   {424, 240},  {640, 360},
        {640, 480},  {800, 448},  {800, 600},  {960, 540},   {1024, 576}, {1280, 720},
        {1280, 960}, {1600, 896}, {1600, 1200}, {1920, 1080}, {2560, 1440}, {3840, 2160},
    };
    const uint32_t sw = std::max<uint32_t>(r.step_width, 1);
    const uint32_t sh = std::max<uint32_t>(r.step_height, 1);
    for (const QSize &sz : kStandard) {
        const uint32_t w = uint32_t(sz.width());
        const uint32_t h = uint32_t(sz.height());
        if (w >= r.min_width && w <= r.max_width && h >= r.min_height && h <= r.max_height
            && (w - r.min_width) % sw == 0 && (h - r.min_height) % sh == 0)
            sizes.append(sz);
    }
    // Always offer the extremes so a device with an unusual range is still usable.
    sizes.append(QSize(int(r.max_width), int(r.max_height)));
    sizes.append(QSize(int(r.min_width), int(r.min_height)));
}

} // namespace

QList<int> offeredRates(int fd, uint32_t fourcc, const QSize &size)
{
    return frameRates(fd, fourcc, size);
}

QList<device::Mode> enumerateModes(const QString &devicePath, device::PixelFormat format)
{
    QList<device::Mode> modes;
    const uint32_t fourcc = toFourcc(format);
    const int fd = ::open(devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return modes;

    // Confirm the format is offered.
    struct v4l2_fmtdesc fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool offered = false;
    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        if (fmt.pixelformat == fourcc)
            offered = true;
        ++fmt.index;
    }
    if (!offered) {
        close(fd);
        return modes;
    }

    QList<QSize> sizes;
    struct v4l2_frmsizeenum fsize;
    std::memset(&fsize, 0, sizeof(fsize));
    fsize.pixel_format = fourcc;
    while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) == 0) {
        switch (fsize.type) {
        case V4L2_FRMSIZE_TYPE_DISCRETE:
            sizes.append(QSize(fsize.discrete.width, fsize.discrete.height));
            break;
        case V4L2_FRMSIZE_TYPE_STEPWISE:
            appendStandardSizes(sizes, fsize.stepwise);
            break;
        case V4L2_FRMSIZE_TYPE_CONTINUOUS:
            appendStandardSizes(sizes, fsize.stepwise);
            break;
        default:
            break;
        }
        ++fsize.index;
    }

    std::sort(sizes.begin(), sizes.end(), [](const QSize &a, const QSize &b) {
        return a.width() * a.height() > b.width() * b.height();
    });
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());

    for (const QSize &size : std::as_const(sizes)) {
        device::Mode mode;
        mode.size = size;
        mode.rates = frameRates(fd, fourcc, size);
        mode.fps = mode.rates.isEmpty() ? 0 : mode.rates.first();
        modes.append(mode);
    }
    close(fd);
    return modes;
}

} // namespace v4l2dev
