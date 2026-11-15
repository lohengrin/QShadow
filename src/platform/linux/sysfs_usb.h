// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include <QString>

// sysfs helpers for locating the ShadowCast's interfaces by USB vendor/product ID.
namespace sysfs_usb {

// True if walking up from `sysfsPath` reaches a USB device node whose idVendor
// and idProduct match.
bool vidPidOnPath(QString sysfsPath, int vid, int pid);

// ALSA card index of the sound card belonging to that USB device (-1 if none).
int findSoundCard(int vid, int pid);

} // namespace sysfs_usb
