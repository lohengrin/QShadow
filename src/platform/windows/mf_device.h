// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "com_common.h"
#include "platform/platform.h"

#include <QString>

#include <mfidl.h>

namespace mfdev {

struct Video {
    QString link; // symbolic link, the opaque device id
    QString name; // friendly name
};

// Every video capture device Media Foundation knows.
QList<Video> listVideo();

// The video-capture IMFMediaSource for a symbolic link (nullptr on failure).
// MFStartup must be active.
HRESULT createSource(const QString &symbolicLink, IMFMediaSource **source);

// Every mode the device offers for a pixel format, largest first, with the
// highest frame rate offered at each size.
QList<device::Mode> enumerateModes(const QString &videoId, device::PixelFormat format);

// FOURCC-style subtype the Media Foundation type system uses for a format.
GUID subtypeFor(device::PixelFormat format);

} // namespace mfdev
