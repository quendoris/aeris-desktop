// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/view/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aeris::desktop {

enum class ViewportDetailTier : std::uint8_t {
    overview = 0U,
    regional = 1U,
    local = 2U,
    fine = 3U,
};

struct ViewportDataDemand final {
    view::SurfaceMode surface_mode{view::SurfaceMode::globe};
    double zoom{1.0};
    double focus_longitude_deg{0.0};
    double focus_latitude_deg{0.0};
    double projection_central_meridian_deg{0.0};
    ViewportDetailTier detail_tier{ViewportDetailTier::overview};
};

struct ViewportCoverageKey final {
    ViewportDetailTier detail_tier{ViewportDetailTier::overview};
    std::int32_t longitude_cell{0};
    std::int32_t latitude_cell{0};

    friend bool operator==(
        const ViewportCoverageKey& a,
        const ViewportCoverageKey& b
    ) noexcept {
        return a.detail_tier == b.detail_tier &&
            a.longitude_cell == b.longitude_cell &&
            a.latitude_cell == b.latitude_cell;
    }
};

[[nodiscard]] inline double normalize_longitude_deg(double longitude_deg) noexcept {
    if (!std::isfinite(longitude_deg)) return 0.0;
    longitude_deg = std::fmod(longitude_deg + 180.0, 360.0);
    if (longitude_deg < 0.0) longitude_deg += 360.0;
    return longitude_deg - 180.0;
}

[[nodiscard]] inline ViewportDetailTier viewport_detail_tier_for_zoom(
    const double zoom
) noexcept {
    if (!std::isfinite(zoom) || zoom < 2.0) {
        return ViewportDetailTier::overview;
    }
    if (zoom < 5.0) return ViewportDetailTier::regional;
    if (zoom < 12.0) return ViewportDetailTier::local;
    return ViewportDetailTier::fine;
}

[[nodiscard]] inline double viewport_coverage_cell_span_deg(
    const ViewportDetailTier tier
) noexcept {
    switch (tier) {
    case ViewportDetailTier::overview:
        return 360.0;
    case ViewportDetailTier::regional:
        return 60.0;
    case ViewportDetailTier::local:
        return 20.0;
    case ViewportDetailTier::fine:
        return 5.0;
    }
    return 360.0;
}

// This is intentionally a stable focus-cell key, not an exact viewport polygon.
// It gives request coalescing a deterministic identity before provider-specific
// footprint logic exists. Providers that need full visible coverage must expand
// the focus cell conservatively; they must not interpret this key as clipping
// canonical project geometry.
[[nodiscard]] inline ViewportCoverageKey viewport_coverage_key(
    const ViewportDataDemand& demand
) noexcept {
    if (demand.detail_tier == ViewportDetailTier::overview) {
        return {ViewportDetailTier::overview, 0, 0};
    }

    const double span = viewport_coverage_cell_span_deg(demand.detail_tier);
    const double longitude = normalize_longitude_deg(demand.focus_longitude_deg);
    const double latitude = std::clamp(demand.focus_latitude_deg, -90.0, 90.0);

    const auto longitude_cell = static_cast<std::int32_t>(
        std::floor((longitude + 180.0) / span)
    );
    const double clamped_latitude = std::min(latitude, std::nextafter(90.0, -90.0));
    const auto latitude_cell = static_cast<std::int32_t>(
        std::floor((clamped_latitude + 90.0) / span)
    );
    return {demand.detail_tier, longitude_cell, latitude_cell};
}

}  // namespace aeris::desktop
