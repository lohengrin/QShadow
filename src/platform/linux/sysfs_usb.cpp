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

} // namespace

QString usbDeviceDir(const QString &sysfsPath)
{
    QString dir = sysfsPath;
    while (true) {
        while (dir.endsWith('/'))
            dir.chop(1);
        if (dir.isEmpty())
            return {};
        if (QFile::exists(dir + "/idVendor") && QFile::exists(dir + "/idProduct"))
            return dir;
        const int slash = dir.lastIndexOf('/');
        if (slash <= 0)
            return {};
        dir = dir.left(slash);
    }
}

bool readUsbIds(const QString &usbDir, int *vid, int *pid)
{
    bool okV = false;
    bool okP = false;
    const int v = readSysfs(usbDir + "/idVendor").toInt(&okV, 16);
    const int p = readSysfs(usbDir + "/idProduct").toInt(&okP, 16);
    if (!okV || !okP)
        return false;
    *vid = v;
    *pid = p;
    return true;
}

int findSoundCardOnUsbDevice(const QString &usbDir, const QString &sysRoot)
{
    if (usbDir.isEmpty())
        return -1;
    // /sys/class/sound/cardN -> .../usbX/.../sound/cardN ; the card belongs to the
    // USB device when walking up from it reaches the same device directory.
    QDir root(sysRoot + QStringLiteral("/class/sound"));
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        if (!entry.startsWith(QLatin1String("card")))
            continue;
        bool ok = false;
        const int index = entry.mid(4).toInt(&ok);
        if (!ok)
            continue; // not a bare cardN node
        const QString resolved = QFileInfo(root.filePath(entry)).symLinkTarget();
        if (resolved.startsWith(sysRoot + QStringLiteral("/devices"))
            && usbDeviceDir(resolved) == usbDir)
            return index;
    }
    return -1;
}

} // namespace sysfs_usb
