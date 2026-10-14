// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace aeris::desktop {

inline constexpr std::uint32_t kEtopo15SurfaceGridRows = 12U;
inline constexpr std::uint32_t kEtopo15SurfaceGridColumns = 24U;
inline constexpr std::uint32_t kEtopo15ResolutionArcsec = 15U;
inline constexpr std::uint32_t kEtopo15TilePixels = 3600U;
inline constexpr int kEtopo15TileSpanDegrees = 15;

struct Etopo15SurfaceTileDescriptor final {
    std::uint32_t row{0U};
    std::uint32_t column{0U};
    int north_deg{0};
    int west_deg{0};
    std::string filename;
    std::string url;
    std::string layer_id;
    std::string resource_id;
    std::string slot_id;
};

[[nodiscard]] inline std::string etopo15_two_digits(const int value) {
    std::ostringstream stream;
    stream << std::setw(2) << std::setfill('0') << value;
    return stream.str();
}

[[nodiscard]] inline std::string etopo15_three_digits(const int value) {
    std::ostringstream stream;
    stream << std::setw(3) << std::setfill('0') << value;
    return stream.str();
}

[[nodiscard]] inline std::optional<Etopo15SurfaceTileDescriptor>
etopo15_surface_tile_for_indices(
    const std::uint32_t row,
    const std::uint32_t column
) {
    if (row >= kEtopo15SurfaceGridRows ||
        column >= kEtopo15SurfaceGridColumns) {
        return std::nullopt;
    }

    const int north = 90 -
        static_cast<int>(row) * kEtopo15TileSpanDegrees;
    const int west = -180 +
        static_cast<int>(column) * kEtopo15TileSpanDegrees;

    const std::string latitude_label =
        (north >= 0 ? "N" : "S") + etopo15_two_digits(std::abs(north));
    const std::string longitude_label =
        (west >= 0 ? "E" : "W") + etopo15_three_digits(std::abs(west));

    Etopo15SurfaceTileDescriptor descriptor{};
    descriptor.row = row;
    descriptor.column = column;
    descriptor.north_deg = north;
    descriptor.west_deg = west;
    descriptor.filename =
        "ETOPO_2022_v1_15s_" +
        latitude_label +
        longitude_label +
        "_surface.tif";
    descriptor.url =
        "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/15s/"
        "15s_surface_elev_gtif/" +
        descriptor.filename;
    descriptor.layer_id =
        "builtin.physical.elevation.etopo2022.v1.15s.surface";
    descriptor.resource_id =
        "builtin.elevation.etopo2022.v1.15s.surface.tile.r" +
        std::to_string(row) +
        ".c" +
        std::to_string(column);
    descriptor.slot_id =
        "tile:15s:g12x24:r" +
        std::to_string(row) +
        ":c" +
        std::to_string(column);
    return descriptor;
}

[[nodiscard]] inline std::optional<Etopo15SurfaceTileDescriptor>
etopo15_surface_tile_for_geographic(
    double longitude_deg,
    double latitude_deg
) {
    if (!std::isfinite(longitude_deg) || !std::isfinite(latitude_deg)) {
        return std::nullopt;
    }

    longitude_deg = std::fmod(longitude_deg + 180.0, 360.0);
    if (longitude_deg < 0.0) longitude_deg += 360.0;
    longitude_deg -= 180.0;
    latitude_deg = std::clamp(latitude_deg, -90.0, 90.0);

    const auto column = static_cast<std::uint32_t>(std::clamp(
        std::floor(
            (longitude_deg + 180.0) /
            static_cast<double>(kEtopo15TileSpanDegrees)
        ),
        0.0,
        static_cast<double>(kEtopo15SurfaceGridColumns - 1U)
    ));
    const auto row = static_cast<std::uint32_t>(std::clamp(
        std::floor(
            (90.0 - latitude_deg) /
            static_cast<double>(kEtopo15TileSpanDegrees)
        ),
        0.0,
        static_cast<double>(kEtopo15SurfaceGridRows - 1U)
    ));
    return etopo15_surface_tile_for_indices(row, column);
}

}  // namespace aeris::desktop
