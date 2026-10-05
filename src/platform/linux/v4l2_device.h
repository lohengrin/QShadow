#pragma once

#include "device.h"

#include <QList>
#include <QString>

// V4L2 device discovery: which /dev/videoN is the ShadowCast, and what it offers.
namespace v4l2dev {

// The /dev/videoN capture node of the ShadowCast (uvcvideo), found through sysfs
// by USB vendor/product ID. Empty if not present.
QString findNode();

// All modes for a pixel format, largest first, read with a single open.
QList<device::Mode> enumerateModes(const QString &devicePath, device::PixelFormat format);

} // namespace v4l2dev
