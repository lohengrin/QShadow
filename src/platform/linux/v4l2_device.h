// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "device.h"

#include <QList>
#include <QSize>
#include <QString>

#include <cstdint>

// V4L2 device discovery: which /dev/videoN nodes can capture, and what they offer.
namespace v4l2dev {

// One V4L2 capture node.
struct Node {
    QString path;   // /dev/videoN
    QString name;   // VIDIOC_QUERYCAP card name
    QString usbDir; // sysfs USB device directory, empty for non-USB devices
    int vid = 0;
    int pid = 0;
};

// Every /dev/videoN node that can capture video (metadata nodes are skipped).
QList<Node> listNodes();

// Frame rates offered for one size on an open device, highest first.
QList<int> offeredRates(int fd, uint32_t fourcc, const QSize &size);

// All modes for a pixel format, largest first, read with a single open.
QList<device::Mode> enumerateModes(const QString &devicePath, device::PixelFormat format);

} // namespace v4l2dev
