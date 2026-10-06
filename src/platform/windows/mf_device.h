#pragma once

#include "com_common.h"
#include "platform/platform.h"

#include <QString>

#include <mfidl.h>

namespace mfdev {

// Symbolic link of the ShadowCast's video capture interface, found by USB
// vendor/product ID. Empty if the dongle is not present.
QString findVideo();

// The video-capture IMFMediaSource for a symbolic link (nullptr on failure).
// MFStartup must be active.
HRESULT createSource(const QString &symbolicLink, IMFMediaSource **source);

// Every mode the device offers for a pixel format, largest first, with the
// highest frame rate offered at each size.
QList<device::Mode> enumerateModes(const QString &videoId, device::PixelFormat format);

// FOURCC-style subtype the Media Foundation type system uses for a format.
GUID subtypeFor(device::PixelFormat format);

} // namespace mfdev
