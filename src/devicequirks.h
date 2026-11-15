// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

#pragma once

#include "device.h"

#include <optional>

// Known deviations of specific hardware from what its driver reports. Keep this
// table small and data-driven: everything else follows the driver.
namespace quirks {

// A colour interpretation that replaces what the driver reports for this device
// and pixel format, or nothing when the driver can be trusted.
inline std::optional<device::ColorInfo> colorOverride(int vid, int pid, device::PixelFormat format)
{
    // The GENKI ShadowCast sends YUYV as FULL range BT.601 (the same values as
    // its JPEGs), although the driver reports it as limited range: measured
    // against the MJPG stream of the same scene, limited range looked darker.
    if (vid == device::kUsbVendorId && pid == device::kUsbProductId
        && format == device::PixelFormat::Yuyv)
        return device::ColorInfo{device::ColorMatrix::Bt601, true};
    return std::nullopt;
}

} // namespace quirks
