// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "etopo15_tile.hpp"

#include "aeris/storage/project.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace aeris::desktop {

struct Etopo15TileImportResult final {
    bool success{false};
    bool changed{false};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Convert one verified official NOAA/NCEI 15 arc-second Surface GeoTIFF tile
// into one canonical embedded AERIS elevation resource. The source TIFF itself
// is recorded only as optional exact content identity/provenance; rendering
// depends exclusively on the canonical embedded tile after this returns.
[[nodiscard]] Etopo15TileImportResult import_etopo2022_surface_15s_tile(
    storage::ProjectStore& project,
    const Etopo15SurfaceTileDescriptor& tile,
    const std::filesystem::path& geotiff_path,
    std::string_view modified_utc
);

}  // namespace aeris::desktop
