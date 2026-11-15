// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#include "sysfs_usb.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace sysfs_usb {

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
bool usbVidPidOnPathImpl(QString dir, int vid, int pid)
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

bool vidPidOnPath(QString sysfsPath, int vid, int pid)
{
    return usbVidPidOnPathImpl(std::move(sysfsPath), vid, pid);
}

int findSoundCard(int vid, int pid)
{
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
            && usbVidPidOnPathImpl(resolved, vid, pid))
            return index;
    }
    return -1;
}

} // namespace sysfs_usb
