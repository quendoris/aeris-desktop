// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace aeris::desktop {

struct ElevationImportResult final {
    bool success{false};
    bool changed{false};
    std::size_t detail_tiles{0U};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Imports the separately downloaded NOAA/NCEI ETOPO 2022 v1 global 60 arc-sec
// surface or bed elevation GeoTIFF. The TIFF is acquisition-only: AERIS embeds
// one 15' numerical overview plus 72 canonical 30x30 degree 60" elevation tiles
// and source provenance into the .aeris project. The source TIFF may be deleted
// after successful import.
[[nodiscard]] ElevationImportResult import_etopo2022_global_60s(
    storage::ProjectStore& project,
    const std::filesystem::path& geotiff_path,
    std::string_view modified_utc);

}  // namespace aeris::desktop
