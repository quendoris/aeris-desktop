// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "data_job_progress.hpp"
#include "etopo15_tile.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace aeris::desktop {

enum class Etopo2022Variant {
    ice_surface,
    bedrock,
};

struct Etopo2022SourceDescriptor final {
    Etopo2022Variant variant{Etopo2022Variant::ice_surface};
    std::string_view id;
    std::string_view display_name;
    std::string_view filename;
    std::string_view url;
};

[[nodiscard]] Etopo2022SourceDescriptor etopo2022_source_descriptor(
    Etopo2022Variant variant
) noexcept;

struct Etopo2022AcquisitionResult final {
    bool success{false};
    bool from_cache{false};
    std::filesystem::path geotiff_path;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Downloads one exact NOAA/NCEI ETOPO 2022 v1 global 60 arc-second GeoTIFF
// into a machine-local acquisition cache. Interrupted transfers keep a sibling
// .part file. Cross-process resume is attempted only when the previous response
// supplied an HTTP validator suitable for If-Range; otherwise the partial is
// safely restarted from zero. The completed file is structurally checked before
// publication and remains acquisition-only after import into .aeris.
[[nodiscard]] Etopo2022AcquisitionResult acquire_etopo2022_global_60s(
    Etopo2022Variant variant,
    const std::filesystem::path& cache_root,
    const DataJobProgressCallback& progress = {}
);

// Acquire exactly one official NOAA/NCEI ETOPO 2022 v1 15 arc-second
// Surface tile selected by the provider planner. The same cross-process safe
// Range/If-Range policy as the global importer is used, but the structural
// contract is one 3600x3600 Float32 GeoTIFF rather than a global grid.
[[nodiscard]] Etopo2022AcquisitionResult acquire_etopo2022_surface_15s_tile(
    const Etopo15SurfaceTileDescriptor& tile,
    const std::filesystem::path& cache_root,
    const DataJobProgressCallback& progress = {}
);

}  // namespace aeris::desktop
