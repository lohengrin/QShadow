#pragma once

#include <QList>
#include <QSize>
#include <QString>

namespace device {

// Returns the /dev/videoN node of the ShadowCast capture device (298f:1996),
// found by walking sysfs for the USB vendor/product IDs. Empty string if not
// present.
QString findShadowCastDevice();

// All MJPG capture sizes the device exposes, largest first.
QList<QSize> enumerateMjpgSizes(const QString &devicePath);

// Maximum frame rate offered for the given size.
int maxFpsForSize(const QString &devicePath, const QSize &size);

} // namespace device