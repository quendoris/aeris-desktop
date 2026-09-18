// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QColor>

#include <algorithm>
#include <cmath>

namespace aeris::desktop {

// Numerical elevation owns geometry/relief, never surface material. This
// presentation helper intentionally accepts illumination only: land/water/ice
// hue must already have been chosen by durable semantic/cartographic channels.
//
// The returned opaque grayscale pixel is composed with Multiply over the
// existing surface material. White preserves the material; darker values add
// hillshade. No elevation value or sign can select a material color here.
[[nodiscard]] inline QRgb neutral_elevation_relief_pixel(
    const double illumination
) noexcept {
    const double light = std::isfinite(illumination)
        ? std::max(0.0, illumination)
        : 0.0;
    const double shade = std::clamp(0.62 + 0.38 * light, 0.62, 1.0);
    const int channel = static_cast<int>(std::lround(255.0 * shade));
    return qRgba(channel, channel, channel, 255);
}

}  // namespace aeris::desktop
