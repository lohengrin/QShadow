// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include <QString>

// sysfs helpers to relate a capture device's interfaces (video node, sound card)
// through the USB device they share.
namespace sysfs_usb {

// The sysfs directory of the USB device (the ancestor carrying idVendor/idProduct)
// that a sysfs path belongs to, or an empty string for a non-USB device.
QString usbDeviceDir(const QString &sysfsPath);

// Reads the vendor/product IDs of a USB device directory returned by usbDeviceDir.
bool readUsbIds(const QString &usbDir, int *vid, int *pid);

// Index of the ALSA card that lives on the given USB device directory, or -1.
// `sysRoot` is where sysfs is mounted (a fixture directory in tests).
int findSoundCardOnUsbDevice(const QString &usbDir, const QString &sysRoot = QStringLiteral("/sys"));

} // namespace sysfs_usb
