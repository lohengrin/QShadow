#include "device.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace device {

namespace {

QString readSysfs(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(f.readAll().trimmed());
}

// Walk up from the video4linux dir towards the root looking for a sysfs USB
// device node with the given vendor/product IDs.
bool usbVidPidOnPath(QString dir, int vid, int pid)
{
    while (true) {
        while (dir.endsWith('/'))
            dir.chop(1);
        if (dir.isEmpty() || dir == "/" || dir.startsWith("/sys/class"))
            break;

        const QString vidPath = dir + "/idVendor";
        const QString pidPath = dir + "/idProduct";
        if (QFile::exists(vidPath) && QFile::exists(pidPath)) {
            bool okV = false;
            bool okP = false;
            const int v = readSysfs(vidPath).toInt(&okV, 16);
            const int p = readSysfs(pidPath).toInt(&okP, 16);
            if (okV && okP && v == vid && p == pid)
                return true;
        }
        const int slash = dir.lastIndexOf('/');
        if (slash <= 0)
            break;
        dir = dir.left(slash);
    }
    return false;
}

} // namespace

QString findShadowCastDevice()
{
    constexpr int kVendor = 0x298f;
    constexpr int kProduct = 0x1996;

    QDir root(QStringLiteral("/sys/class/video4linux"));
    const QStringList nodes = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &node : nodes) {
        const QString resolved = QFileInfo(root.filePath(node)).symLinkTarget();
        if (resolved.isEmpty() || !resolved.startsWith(QStringLiteral("/sys/devices")))
            continue;
        if (!usbVidPidOnPath(resolved, kVendor, kProduct))
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

        const char *driver = reinterpret_cast<const char *>(cap.driver);
        const bool uvc = std::strcmp(driver, "uvcvideo") == 0;
        const bool caps = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
        if (uvc && caps)
            return devPath;
    }
    return {};
}

uint32_t toFourcc(PixelFormat format)
{
    return format == PixelFormat::Yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
}

namespace {

// Highest frame rate for one frame size on an already open device.
int maxFps(int fd, uint32_t fourcc, const QSize &size)
{
    struct v4l2_frmivalenum fiv;
    std::memset(&fiv, 0, sizeof(fiv));
    fiv.pixel_format = fourcc;
    fiv.width = uint32_t(size.width());
    fiv.height = uint32_t(size.height());

    double bestUs = 0.0;
    bool any = false;
    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fiv) == 0) {
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
    return any ? int(1e6 / bestUs + 0.5) : 0;
}

} // namespace

QList<Mode> enumerateModes(const QString &devicePath, PixelFormat format)
{
    QList<Mode> modes;
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
            for (uint32_t w = fsize.stepwise.min_width; w <= fsize.stepwise.max_width;
                 w += fsize.stepwise.step_width) {
                for (uint32_t h = fsize.stepwise.min_height;
                     h <= fsize.stepwise.max_height; h += fsize.stepwise.step_height)
                    sizes.append(QSize(int(w), int(h)));
            }
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

    for (const QSize &size : std::as_const(sizes))
        modes.append(Mode{size, maxFps(fd, fourcc, size)});
    close(fd);
    return modes;
}

int findShadowCastAudioCard()
{
    constexpr int kVendor = 0x298f;
    constexpr int kProduct = 0x1996;

    // /sys/class/sound/cardN -> .../usbX/.../sound/cardN ; walk up to the USB
    // device node carrying the vendor/product IDs.
    QDir root(QStringLiteral("/sys/class/sound"));
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        if (!entry.startsWith(QLatin1String("card")))
            continue;
        bool ok = false;
        const int index = entry.mid(4).toInt(&ok);
        if (!ok)
            continue; // not a bare cardN node
        const QString resolved = QFileInfo(root.filePath(entry)).symLinkTarget();
        if (resolved.startsWith(QStringLiteral("/sys/devices"))
            && usbVidPidOnPath(resolved, kVendor, kProduct))
            return index;
    }
    return -1;
}

} // namespace device
