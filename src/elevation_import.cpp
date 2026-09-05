// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_import.hpp"

#include "elevation_tiff.hpp"

#include "aeris/elevation/grid.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/storage/resource.hpp"
#include "aeris/util/sha256.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aeris::desktop {
namespace {

constexpr std::uint32_t kSourceWidth = 21600U;
constexpr std::uint32_t kSourceHeight = 10800U;
constexpr std::uint32_t kDetailTilePixels = 1800U;
constexpr std::uint32_t kDetailTileColumns = 12U;
constexpr std::uint32_t kDetailTileRows = 6U;
constexpr std::uint32_t kOverviewFactor = 15U;
constexpr std::uint32_t kOverviewWidth = kSourceWidth / kOverviewFactor;
constexpr std::uint32_t kOverviewHeight = kSourceHeight / kOverviewFactor;
constexpr std::int64_t kMicroArcsecondsPerArcsecond = 1000000LL;
constexpr std::int64_t kDetailStepMicroarcsec =
    60LL * kMicroArcsecondsPerArcsecond;
constexpr std::int64_t kOverviewStepMicroarcsec =
    900LL * kMicroArcsecondsPerArcsecond;
constexpr std::int64_t kWestMicroarcsec = -180LL * 3600LL * 1000000LL;
constexpr std::int64_t kNorthMicroarcsec = 90LL * 3600LL * 1000000LL;
constexpr std::string_view kSurfaceFilename =
    "ETOPO_2022_v1_60s_N90W180_surface.tif";
constexpr std::string_view kBedFilename =
    "ETOPO_2022_v1_60s_N90W180_bed.tif";

struct Variant final {
    std::string id;
    std::string display_name;
    std::string source_uri;
};

[[nodiscard]] ElevationImportResult failure(
    const bool changed,
    const std::size_t detail_tiles,
    std::string diagnostic
) {
    return {false, changed, detail_tiles, std::move(diagnostic)};
}

[[nodiscard]] std::optional<Variant> variant_from_filename(
    const std::filesystem::path& path
) {
    const std::string filename = path.filename().string();
    if (filename == kSurfaceFilename) {
        return Variant{
            "surface",
            "ETOPO 2022 surface elevation (60 arc-sec)",
            "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/"
            "60s_surface_elev_gtif/ETOPO_2022_v1_60s_N90W180_surface.tif",
        };
    }
    if (filename == kBedFilename) {
        return Variant{
            "bed",
            "ETOPO 2022 bed elevation (60 arc-sec)",
            "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/"
            "60s_bed_elev_gtif/ETOPO_2022_v1_60s_N90W180_bed.tif",
        };
    }
    return std::nullopt;
}

[[nodiscard]] std::string layer_id(const Variant& variant) {
    return "builtin.physical.elevation.etopo2022.v1.60s." + variant.id;
}

[[nodiscard]] std::string resource_prefix(const Variant& variant) {
    return "builtin.elevation.etopo2022.v1.60s." + variant.id;
}

[[nodiscard]] std::string two_digits(const std::uint32_t value) {
    std::ostringstream stream;
    stream << std::setw(2) << std::setfill('0') << value;
    return stream.str();
}

[[nodiscard]] bool has_layer_id(
    const std::vector<storage::ProjectLayerRecord>& layers,
    const std::string& id
) noexcept {
    return std::any_of(
        layers.begin(),
        layers.end(),
        [&](const storage::ProjectLayerRecord& layer) {
            return layer.layer_id == id;
        }
    );
}

[[nodiscard]] const storage::ProjectLayerRecord* find_land_layer(
    const std::vector<storage::ProjectLayerRecord>& layers
) noexcept {
    const auto found = std::find_if(
        layers.begin(),
        layers.end(),
        [](const storage::ProjectLayerRecord& layer) {
            return layer.role_id == storage::kLayerRolePhysicalLandFillV1;
        }
    );
    return found == layers.end() ? nullptr : &*found;
}

[[nodiscard]] std::vector<std::string> elevation_layer_order(
    const std::vector<storage::ProjectLayerRecord>& layers,
    const std::string& elevation_id
) {
    std::vector<std::string> order;
    order.reserve(layers.size());
    for (const storage::ProjectLayerRecord& layer : layers) {
        if (layer.layer_id != elevation_id) order.push_back(layer.layer_id);
    }

    auto land = std::find_if(
        layers.begin(),
        layers.end(),
        [](const storage::ProjectLayerRecord& layer) {
            return layer.role_id == storage::kLayerRolePhysicalLandFillV1;
        }
    );
    if (land == layers.end()) {
        order.push_back(elevation_id);
        return order;
    }

    const auto land_id = std::find(order.begin(), order.end(), land->layer_id);
    order.insert(land_id, elevation_id);
    return order;
}

[[nodiscard]] elevation::ElevationTile detail_tile(
    const std::uint32_t tile_row,
    const std::uint32_t tile_column
) {
    elevation::ElevationTile tile{};
    tile.width = kDetailTilePixels;
    tile.height = kDetailTilePixels;
    tile.west_microarcsec = kWestMicroarcsec +
        static_cast<std::int64_t>(tile_column) *
            static_cast<std::int64_t>(kDetailTilePixels) *
            kDetailStepMicroarcsec;
    tile.north_microarcsec = kNorthMicroarcsec -
        static_cast<std::int64_t>(tile_row) *
            static_cast<std::int64_t>(kDetailTilePixels) *
            kDetailStepMicroarcsec;
    tile.longitude_step_microarcsec = kDetailStepMicroarcsec;
    tile.latitude_step_microarcsec = kDetailStepMicroarcsec;
    tile.vertical_reference = elevation::VerticalReference::egm2008_orthometric;
    tile.samples_m.assign(
        static_cast<std::size_t>(kDetailTilePixels) *
            static_cast<std::size_t>(kDetailTilePixels),
        elevation::kNoDataMeters
    );
    return tile;
}

[[nodiscard]] bool quantize_elevation(
    const float input,
    std::int16_t& output
) noexcept {
    if (!std::isfinite(input)) return false;
    const double rounded = std::round(static_cast<double>(input));
    if (rounded <= static_cast<double>(std::numeric_limits<std::int16_t>::min()) ||
        rounded > static_cast<double>(std::numeric_limits<std::int16_t>::max())) {
        return false;
    }
    output = static_cast<std::int16_t>(rounded);
    return output != elevation::kNoDataMeters;
}

[[nodiscard]] storage::ResourceMutationResult embed_generated(
    storage::ProjectStore& project,
    const std::string& resource_id,
    const std::string& media_type,
    const std::vector<std::uint8_t>& bytes,
    const std::string_view modified_utc
) {
    storage::ProjectResourceIdentity identity{};
    identity.resource_id = resource_id;
    identity.sha256 = util::sha256_bytes(
        bytes.empty() ? nullptr : bytes.data(),
        bytes.size()
    ).hex();
    identity.media_type = media_type;
    identity.size_bytes = static_cast<std::uint64_t>(bytes.size());
    // Derived resources remain optional until the layer binding is committed;
    // append_layer() promotes every referenced resource atomically.
    identity.required_for_reproduction = false;
    return storage::embed_resource_bytes(project, identity, bytes, modified_utc);
}

[[nodiscard]] std::vector<std::uint8_t> bytes_from_string(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

}  // namespace

ElevationImportResult import_etopo2022_global_60s(
    storage::ProjectStore& project,
    const std::filesystem::path& geotiff_path,
    const std::string_view modified_utc
) {
    if (geotiff_path.empty() || modified_utc.empty()) {
        return failure(false, 0U, "ETOPO import requires a GeoTIFF path and timestamp");
    }

    const auto variant = variant_from_filename(geotiff_path);
    if (!variant.has_value()) {
        return failure(
            false,
            0U,
            "expected the official ETOPO 2022 v1 global 60 arc-sec surface or bed GeoTIFF filename"
        );
    }

    const storage::ProjectLayerListResult before = storage::list_project_layers(project);
    if (!before.ok()) {
        return failure(false, 0U, "could not inspect project layers: " + before.status.diagnostic);
    }
    const std::string elevation_id = layer_id(*variant);
    if (has_layer_id(before.records, elevation_id)) {
        return {
            true,
            false,
            static_cast<std::size_t>(kDetailTileColumns) *
                static_cast<std::size_t>(kDetailTileRows),
            "ETOPO 2022 elevation layer already present",
        };
    }
    if (project.metadata().frozen) {
        return failure(false, 0U, "ETOPO import refuses a frozen project");
    }
    if (find_land_layer(before.records) == nullptr) {
        return failure(
            false,
            0U,
            "ETOPO elevation currently requires the built-in physical world layer; import Natural Earth first"
        );
    }

    const Float32TiffInspectResult inspected =
        inspect_single_band_float32_tiff(geotiff_path);
    if (!inspected.ok()) {
        return failure(false, 0U, "ETOPO TIFF preflight failed: " + inspected.diagnostic);
    }
    if (inspected.info.width != kSourceWidth || inspected.info.height != kSourceHeight) {
        return failure(
            false,
            0U,
            "ETOPO 60 arc-sec global TIFF must be exactly 21600x10800 pixels"
        );
    }

    std::error_code size_error;
    const std::uintmax_t source_size = std::filesystem::file_size(geotiff_path, size_error);
    if (size_error || source_size == 0U) {
        return failure(false, 0U, "could not inspect ETOPO source file size");
    }
    const util::Sha256FileResult source_hash = util::sha256_file(geotiff_path);
    if (!source_hash.ok()) {
        return failure(false, 0U, "could not hash ETOPO source GeoTIFF");
    }

    std::vector<std::int64_t> overview_sums(
        static_cast<std::size_t>(kOverviewWidth) *
            static_cast<std::size_t>(kOverviewHeight),
        0
    );
    std::vector<std::uint16_t> overview_counts(overview_sums.size(), 0U);
    std::vector<std::uint16_t> overview_column(kSourceWidth);
    std::vector<std::uint16_t> detail_column(kSourceWidth);
    std::vector<std::uint16_t> detail_local_x(kSourceWidth);
    for (std::uint32_t x = 0U; x < kSourceWidth; ++x) {
        overview_column[x] = static_cast<std::uint16_t>(x / kOverviewFactor);
        detail_column[x] = static_cast<std::uint16_t>(x / kDetailTilePixels);
        detail_local_x[x] = static_cast<std::uint16_t>(x % kDetailTilePixels);
    }

    const std::string prefix = resource_prefix(*variant);
    std::vector<storage::LayerResourceBinding> detail_bindings;
    detail_bindings.reserve(
        static_cast<std::size_t>(kDetailTileColumns) *
        static_cast<std::size_t>(kDetailTileRows)
    );
    std::vector<elevation::ElevationTile> band_tiles;
    bool changed = false;
    std::size_t embedded_detail_tiles = 0U;

    const Float32TiffReadResult decoded = read_single_band_float32_tiff(
        geotiff_path,
        [&](const std::uint32_t row,
            const float* samples,
            const std::size_t count,
            std::string& diagnostic) {
            if (count != static_cast<std::size_t>(kSourceWidth) ||
                row >= kSourceHeight) {
                diagnostic = "decoded TIFF row disagrees with preflight dimensions";
                return false;
            }

            const std::uint32_t tile_row = row / kDetailTilePixels;
            const std::uint32_t local_y = row % kDetailTilePixels;
            if (local_y == 0U) {
                band_tiles.clear();
                band_tiles.reserve(kDetailTileColumns);
                for (std::uint32_t column = 0U;
                     column < kDetailTileColumns;
                     ++column) {
                    band_tiles.push_back(detail_tile(tile_row, column));
                }
            }
            if (band_tiles.size() != kDetailTileColumns) {
                diagnostic = "internal elevation tile band was not initialized";
                return false;
            }

            const std::uint32_t overview_y = row / kOverviewFactor;
            const std::size_t overview_row_offset =
                static_cast<std::size_t>(overview_y) *
                static_cast<std::size_t>(kOverviewWidth);
            const std::size_t detail_row_offset =
                static_cast<std::size_t>(local_y) *
                static_cast<std::size_t>(kDetailTilePixels);

            for (std::uint32_t x = 0U; x < kSourceWidth; ++x) {
                std::int16_t value = 0;
                if (!quantize_elevation(samples[x], value)) {
                    diagnostic =
                        "ETOPO contains a non-finite or out-of-range elevation sample at row " +
                        std::to_string(row) + ", column " + std::to_string(x);
                    return false;
                }

                const std::size_t tile_index = detail_column[x];
                const std::size_t sample_index = detail_row_offset + detail_local_x[x];
                band_tiles[tile_index].samples_m[sample_index] = value;

                const std::size_t overview_index = overview_row_offset + overview_column[x];
                overview_sums[overview_index] += static_cast<std::int64_t>(value);
                if (overview_counts[overview_index] ==
                    std::numeric_limits<std::uint16_t>::max()) {
                    diagnostic = "overview aggregation count overflow";
                    return false;
                }
                ++overview_counts[overview_index];
            }

            if (local_y + 1U == kDetailTilePixels) {
                for (std::uint32_t column = 0U;
                     column < kDetailTileColumns;
                     ++column) {
                    std::vector<std::uint8_t> bytes =
                        elevation::encode_elevation_tile_v1(band_tiles[column]);
                    if (bytes.empty()) {
                        diagnostic = "could not encode canonical ETOPO detail tile";
                        return false;
                    }
                    const std::string row_id = two_digits(tile_row);
                    const std::string column_id = two_digits(column);
                    const std::string resource_id =
                        prefix + ".tile.r" + row_id + ".c" + column_id;
                    const storage::ResourceMutationResult embedded = embed_generated(
                        project,
                        resource_id,
                        std::string(elevation::kElevationTileMediaType),
                        bytes,
                        modified_utc
                    );
                    if (!embedded.ok()) {
                        diagnostic =
                            "could not embed ETOPO detail tile r" + row_id + " c" +
                            column_id + ": " + embedded.status.diagnostic;
                        return false;
                    }
                    changed = changed || embedded.inserted ||
                        embedded.representation_changed || embedded.durably_committed;
                    detail_bindings.push_back({
                        "tile:60s:r" + row_id + ":c" + column_id,
                        resource_id,
                    });
                    ++embedded_detail_tiles;
                }
                band_tiles.clear();
            }
            return true;
        }
    );

    if (!decoded.ok()) {
        return failure(
            changed,
            embedded_detail_tiles,
            "ETOPO streaming decode/import failed: " + decoded.diagnostic
        );
    }
    if (decoded.rows_read != kSourceHeight ||
        embedded_detail_tiles !=
            static_cast<std::size_t>(kDetailTileColumns) *
                static_cast<std::size_t>(kDetailTileRows)) {
        return failure(
            changed,
            embedded_detail_tiles,
            "ETOPO decode completed with an incomplete row or tile count"
        );
    }

    elevation::ElevationTile overview{};
    overview.width = kOverviewWidth;
    overview.height = kOverviewHeight;
    overview.west_microarcsec = kWestMicroarcsec;
    overview.north_microarcsec = kNorthMicroarcsec;
    overview.longitude_step_microarcsec = kOverviewStepMicroarcsec;
    overview.latitude_step_microarcsec = kOverviewStepMicroarcsec;
    overview.vertical_reference = elevation::VerticalReference::egm2008_orthometric;
    overview.samples_m.resize(overview_sums.size(), elevation::kNoDataMeters);
    for (std::size_t index = 0U; index < overview_sums.size(); ++index) {
        if (overview_counts[index] == 0U) {
            return failure(
                changed,
                embedded_detail_tiles,
                "ETOPO overview aggregation produced an empty cell"
            );
        }
        const double average = static_cast<double>(overview_sums[index]) /
            static_cast<double>(overview_counts[index]);
        const double rounded = std::round(average);
        if (rounded <= static_cast<double>(std::numeric_limits<std::int16_t>::min()) ||
            rounded > static_cast<double>(std::numeric_limits<std::int16_t>::max())) {
            return failure(
                changed,
                embedded_detail_tiles,
                "ETOPO overview elevation is outside canonical int16 metre bounds"
            );
        }
        overview.samples_m[index] = static_cast<std::int16_t>(rounded);
    }

    std::vector<std::uint8_t> overview_bytes =
        elevation::encode_elevation_tile_v1(overview);
    if (overview_bytes.empty()) {
        return failure(changed, embedded_detail_tiles, "could not encode ETOPO overview tile");
    }
    const std::string overview_id = prefix + ".overview.900s";
    const storage::ResourceMutationResult overview_embedded = embed_generated(
        project,
        overview_id,
        std::string(elevation::kElevationTileMediaType),
        overview_bytes,
        modified_utc
    );
    if (!overview_embedded.ok()) {
        return failure(
            changed,
            embedded_detail_tiles,
            "could not embed ETOPO overview: " + overview_embedded.status.diagnostic
        );
    }
    changed = changed || overview_embedded.inserted ||
        overview_embedded.representation_changed || overview_embedded.durably_committed;

    std::ostringstream manifest;
    manifest
        << "aeris-elevation-import-v1\n"
        << "provider=NOAA/NCEI\n"
        << "dataset=ETOPO 2022\n"
        << "version=1\n"
        << "variant=" << variant->id << "\n"
        << "vertical_reference=EGM2008 orthometric\n"
        << "resolution_arcsec=60\n"
        << "width=21600\n"
        << "height=10800\n"
        << "west_deg=-180\n"
        << "north_deg=90\n"
        << "source_uri=" << variant->source_uri << "\n"
        << "source_size_bytes=" << source_size << "\n"
        << "source_sha256=" << source_hash.digest.hex() << "\n";
    const std::vector<std::uint8_t> manifest_bytes = bytes_from_string(manifest.str());
    const std::string manifest_id = prefix + ".provenance";
    const storage::ResourceMutationResult manifest_embedded = embed_generated(
        project,
        manifest_id,
        "text/plain; charset=utf-8",
        manifest_bytes,
        modified_utc
    );
    if (!manifest_embedded.ok()) {
        return failure(
            changed,
            embedded_detail_tiles,
            "could not embed ETOPO provenance: " + manifest_embedded.status.diagnostic
        );
    }
    changed = changed || manifest_embedded.inserted ||
        manifest_embedded.representation_changed || manifest_embedded.durably_committed;

    storage::LayerCreateRequest elevation_layer{};
    elevation_layer.layer_id = elevation_id;
    elevation_layer.role_id = std::string(storage::kLayerRolePhysicalElevationV1);
    elevation_layer.name = variant->display_name;
    elevation_layer.visible = true;
    elevation_layer.resources.reserve(detail_bindings.size() + 2U);
    elevation_layer.resources.push_back({"provenance", manifest_id});
    elevation_layer.resources.push_back({"overview:900s", overview_id});
    elevation_layer.resources.insert(
        elevation_layer.resources.end(),
        detail_bindings.begin(),
        detail_bindings.end()
    );

    const storage::LayerMutationResult appended = storage::append_layer(
        project,
        elevation_layer,
        modified_utc
    );
    if (!appended.ok()) {
        return failure(
            changed || appended.changed || appended.durably_committed,
            embedded_detail_tiles,
            "ETOPO elevation layer creation failed: " + appended.status.diagnostic
        );
    }
    changed = changed || appended.changed || appended.durably_committed;

    const storage::ProjectLayerListResult after_append = storage::list_project_layers(project);
    if (!after_append.ok()) {
        return failure(
            changed,
            embedded_detail_tiles,
            "could not inspect layer order after ETOPO import: " +
                after_append.status.diagnostic
        );
    }
    const storage::LayerMutationResult reordered = storage::set_layer_order(
        project,
        elevation_layer_order(after_append.records, elevation_id),
        modified_utc
    );
    if (!reordered.ok()) {
        return failure(
            changed || reordered.changed || reordered.durably_committed,
            embedded_detail_tiles,
            "ETOPO elevation layer ordering failed: " + reordered.status.diagnostic
        );
    }
    changed = changed || reordered.changed || reordered.durably_committed;

    const storage::Status integrity = project.verify_integrity();
    if (!integrity.ok()) {
        return failure(
            changed,
            embedded_detail_tiles,
            "project integrity failed after ETOPO import: " + integrity.diagnostic
        );
    }

    return {
        true,
        changed,
        embedded_detail_tiles,
        "ETOPO 2022 numerical elevation imported into durable .aeris storage",
    };
}

}  // namespace aeris::desktop
