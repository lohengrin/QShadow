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

QList<QSize> enumerateMjpgSizes(const QString &devicePath)
{
    QList<QSize> sizes;
    const int fd = ::open(devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return sizes;

    // Confirm MJPEG is offered.
    struct v4l2_fmtdesc fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool mjpeg = false;
    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        if (fmt.pixelformat == V4L2_PIX_FMT_MJPEG)
            mjpeg = true;
        ++fmt.index;
    }
    if (!mjpeg) {
        close(fd);
        return sizes;
    }

    struct v4l2_frmsizeenum fsize;
    std::memset(&fsize, 0, sizeof(fsize));
    fsize.pixel_format = V4L2_PIX_FMT_MJPEG;
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
    close(fd);

    std::sort(sizes.begin(), sizes.end(), [](const QSize &a, const QSize &b) {
        return a.width() * a.height() > b.width() * b.height();
    });
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

int maxFpsForSize(const QString &devicePath, const QSize &size)
{
    const int fd = ::open(devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return 0;

    struct v4l2_frmivalenum fiv;
    std::memset(&fiv, 0, sizeof(fiv));
    fiv.pixel_format = V4L2_PIX_FMT_MJPEG;
    fiv.width = uint32_t(size.width());
    fiv.height = uint32_t(size.height());

    double bestUs = 0.0; // largest interval = lowest rate; track intervention instead
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
    close(fd);
    if (!any)
        return 0;
    return int(1e6 / bestUs + 0.5);
}

} // namespace device