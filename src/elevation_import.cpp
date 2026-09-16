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

constexpr std::int64_t kMicroarcsecondsPerArcsecond = 1000000LL;
constexpr std::int64_t kMicroarcsecondsPerDegree =
    3600LL * kMicroarcsecondsPerArcsecond;
constexpr std::int64_t kWestMicroarcsec = -180LL * kMicroarcsecondsPerDegree;
constexpr std::int64_t kNorthMicroarcsec = 90LL * kMicroarcsecondsPerDegree;
constexpr std::string_view kSurfaceFilename =
    "ETOPO_2022_v1_60s_N90W180_surface.tif";
constexpr std::string_view kBedFilename =
    "ETOPO_2022_v1_60s_N90W180_bed.tif";

struct Variant final {
    std::string id;
    std::string display_name;
    std::string source_uri;
};

struct ElevationImportSpec final {
    std::uint32_t source_width{0U};
    std::uint32_t source_height{0U};
    std::uint32_t detail_tile_pixels{0U};
    std::uint32_t overview_factor{0U};
    std::int64_t detail_step_microarcsec{0LL};
    std::string layer_prefix;
    std::string resource_prefix;
    std::string provider;
    std::string dataset;
    std::string version;
    std::string variant_id;
    std::string display_name;
    std::string source_uri;
    std::string diagnostic_label;
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

[[nodiscard]] ElevationImportSpec etopo_spec(const Variant& variant) {
    ElevationImportSpec spec{};
    spec.source_width = 21600U;
    spec.source_height = 10800U;
    spec.detail_tile_pixels = 1800U;
    spec.overview_factor = 15U;
    spec.detail_step_microarcsec = 60LL * kMicroarcsecondsPerArcsecond;
    spec.layer_prefix = "builtin.physical.elevation.etopo2022.v1.60s";
    spec.resource_prefix = "builtin.elevation.etopo2022.v1.60s";
    spec.provider = "NOAA/NCEI";
    spec.dataset = "ETOPO 2022";
    spec.version = "1";
    spec.variant_id = variant.id;
    spec.display_name = variant.display_name;
    spec.source_uri = variant.source_uri;
    spec.diagnostic_label = "ETOPO";
    return spec;
}

[[nodiscard]] ElevationImportSpec deterministic_fixture_spec() {
    ElevationImportSpec spec{};
    spec.source_width = 360U;
    spec.source_height = 180U;
    spec.detail_tile_pixels = 30U;
    spec.overview_factor = 15U;
    spec.detail_step_microarcsec = 3600LL * kMicroarcsecondsPerArcsecond;
    spec.layer_prefix = "test.physical.elevation.fixture.v1.3600s";
    spec.resource_prefix = "test.elevation.fixture.v1.3600s";
    spec.provider = "AERIS CI";
    spec.dataset = "Deterministic full-world elevation fixture";
    spec.version = "1";
    spec.variant_id = "surface";
    spec.display_name = "Deterministic full-world elevation fixture (1 degree)";
    spec.source_uri = "fixture://aeris/deterministic-global-elevation-v1";
    spec.diagnostic_label = "deterministic elevation fixture";
    return spec;
}

[[nodiscard]] std::string layer_id(const ElevationImportSpec& spec) {
    return spec.layer_prefix + "." + spec.variant_id;
}

[[nodiscard]] std::string resource_prefix(const ElevationImportSpec& spec) {
    return spec.resource_prefix + "." + spec.variant_id;
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

[[nodiscard]] std::optional<std::string> validate_spec(
    const ElevationImportSpec& spec
) {
    if (spec.source_width == 0U || spec.source_height == 0U ||
        spec.detail_tile_pixels == 0U || spec.overview_factor == 0U ||
        spec.detail_step_microarcsec <= 0LL || spec.layer_prefix.empty() ||
        spec.resource_prefix.empty() || spec.variant_id.empty() ||
        spec.display_name.empty() || spec.source_uri.empty()) {
        return "invalid elevation import specification";
    }
    if (spec.source_width % spec.detail_tile_pixels != 0U ||
        spec.source_height % spec.detail_tile_pixels != 0U ||
        spec.source_width % spec.overview_factor != 0U ||
        spec.source_height % spec.overview_factor != 0U) {
        return "elevation import grid is not exactly divisible by tile/overview dimensions";
    }
    const std::uint32_t detail_columns =
        spec.source_width / spec.detail_tile_pixels;
    const std::uint32_t detail_rows =
        spec.source_height / spec.detail_tile_pixels;
    if (detail_columns == 0U || detail_rows == 0U ||
        detail_columns > 100U || detail_rows > 100U) {
        return "elevation import tile grid is outside canonical two-digit tile indexing";
    }
    const std::int64_t longitude_span =
        static_cast<std::int64_t>(spec.source_width) * spec.detail_step_microarcsec;
    const std::int64_t latitude_span =
        static_cast<std::int64_t>(spec.source_height) * spec.detail_step_microarcsec;
    if (longitude_span != 360LL * kMicroarcsecondsPerDegree ||
        latitude_span != 180LL * kMicroarcsecondsPerDegree) {
        return "elevation import fixture must describe one exact full-world grid";
    }
    if (spec.detail_step_microarcsec % kMicroarcsecondsPerArcsecond != 0LL) {
        return "elevation import resolution must be an integral number of arc-seconds";
    }
    if (spec.detail_step_microarcsec >
        std::numeric_limits<std::int64_t>::max() /
            static_cast<std::int64_t>(spec.overview_factor)) {
        return "elevation overview step overflows canonical georeferencing";
    }
    return std::nullopt;
}

[[nodiscard]] elevation::ElevationTile detail_tile(
    const ElevationImportSpec& spec,
    const std::uint32_t tile_row,
    const std::uint32_t tile_column
) {
    elevation::ElevationTile tile{};
    tile.width = spec.detail_tile_pixels;
    tile.height = spec.detail_tile_pixels;
    tile.west_microarcsec = kWestMicroarcsec +
        static_cast<std::int64_t>(tile_column) *
            static_cast<std::int64_t>(spec.detail_tile_pixels) *
            spec.detail_step_microarcsec;
    tile.north_microarcsec = kNorthMicroarcsec -
        static_cast<std::int64_t>(tile_row) *
            static_cast<std::int64_t>(spec.detail_tile_pixels) *
            spec.detail_step_microarcsec;
    tile.longitude_step_microarcsec = spec.detail_step_microarcsec;
    tile.latitude_step_microarcsec = spec.detail_step_microarcsec;
    tile.vertical_reference = elevation::VerticalReference::egm2008_orthometric;
    tile.samples_m.assign(
        static_cast<std::size_t>(spec.detail_tile_pixels) *
            static_cast<std::size_t>(spec.detail_tile_pixels),
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

[[nodiscard]] ElevationImportResult import_global_elevation(
    storage::ProjectStore& project,
    const std::filesystem::path& geotiff_path,
    const std::string_view modified_utc,
    const ElevationImportSpec& spec
) {
    if (geotiff_path.empty() || modified_utc.empty()) {
        return failure(
            false,
            0U,
            spec.diagnostic_label + " import requires a GeoTIFF path and timestamp"
        );
    }
    if (const auto invalid = validate_spec(spec); invalid.has_value()) {
        return failure(false, 0U, *invalid);
    }

    const std::uint32_t detail_tile_columns =
        spec.source_width / spec.detail_tile_pixels;
    const std::uint32_t detail_tile_rows =
        spec.source_height / spec.detail_tile_pixels;
    const std::uint32_t overview_width =
        spec.source_width / spec.overview_factor;
    const std::uint32_t overview_height =
        spec.source_height / spec.overview_factor;
    const std::int64_t overview_step_microarcsec =
        spec.detail_step_microarcsec * static_cast<std::int64_t>(spec.overview_factor);
    const std::uint32_t resolution_arcsec = static_cast<std::uint32_t>(
        spec.detail_step_microarcsec / kMicroarcsecondsPerArcsecond
    );
    const std::uint32_t overview_resolution_arcsec =
        resolution_arcsec * spec.overview_factor;

    const storage::ProjectLayerListResult before = storage::list_project_layers(project);
    if (!before.ok()) {
        return failure(false, 0U, "could not inspect project layers: " + before.status.diagnostic);
    }
    const std::string elevation_id = layer_id(spec);
    if (has_layer_id(before.records, elevation_id)) {
        return {
            true,
            false,
            static_cast<std::size_t>(detail_tile_columns) *
                static_cast<std::size_t>(detail_tile_rows),
            spec.diagnostic_label + " elevation layer already present",
        };
    }
    if (project.metadata().frozen) {
        return failure(false, 0U, spec.diagnostic_label + " import refuses a frozen project");
    }
    if (find_land_layer(before.records) == nullptr) {
        return failure(
            false,
            0U,
            spec.diagnostic_label +
                " elevation currently requires the built-in physical world layer; import Natural Earth first"
        );
    }

    const Float32TiffInspectResult inspected =
        inspect_single_band_float32_tiff(geotiff_path);
    if (!inspected.ok()) {
        return failure(
            false,
            0U,
            spec.diagnostic_label + " TIFF preflight failed: " + inspected.diagnostic
        );
    }
    if (inspected.info.width != spec.source_width ||
        inspected.info.height != spec.source_height) {
        return failure(
            false,
            0U,
            spec.diagnostic_label + " global TIFF must be exactly " +
                std::to_string(spec.source_width) + "x" +
                std::to_string(spec.source_height) + " pixels"
        );
    }

    std::error_code size_error;
    const std::uintmax_t source_size = std::filesystem::file_size(geotiff_path, size_error);
    if (size_error || source_size == 0U) {
        return failure(false, 0U, "could not inspect elevation source file size");
    }
    const util::Sha256FileResult source_hash = util::sha256_file(geotiff_path);
    if (!source_hash.ok()) {
        return failure(false, 0U, "could not hash elevation source GeoTIFF");
    }

    std::vector<std::int64_t> overview_sums(
        static_cast<std::size_t>(overview_width) *
            static_cast<std::size_t>(overview_height),
        0
    );
    std::vector<std::uint32_t> overview_counts(overview_sums.size(), 0U);
    std::vector<std::uint32_t> overview_column(spec.source_width);
    std::vector<std::uint32_t> detail_column(spec.source_width);
    std::vector<std::uint32_t> detail_local_x(spec.source_width);
    for (std::uint32_t x = 0U; x < spec.source_width; ++x) {
        overview_column[x] = x / spec.overview_factor;
        detail_column[x] = x / spec.detail_tile_pixels;
        detail_local_x[x] = x % spec.detail_tile_pixels;
    }

    const std::string prefix = resource_prefix(spec);
    std::vector<storage::LayerResourceBinding> detail_bindings;
    detail_bindings.reserve(
        static_cast<std::size_t>(detail_tile_columns) *
        static_cast<std::size_t>(detail_tile_rows)
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
            if (count != static_cast<std::size_t>(spec.source_width) ||
                row >= spec.source_height) {
                diagnostic = "decoded TIFF row disagrees with preflight dimensions";
                return false;
            }

            const std::uint32_t tile_row = row / spec.detail_tile_pixels;
            const std::uint32_t local_y = row % spec.detail_tile_pixels;
            if (local_y == 0U) {
                band_tiles.clear();
                band_tiles.reserve(detail_tile_columns);
                for (std::uint32_t column = 0U;
                     column < detail_tile_columns;
                     ++column) {
                    band_tiles.push_back(detail_tile(spec, tile_row, column));
                }
            }
            if (band_tiles.size() != static_cast<std::size_t>(detail_tile_columns)) {
                diagnostic = "internal elevation tile band was not initialized";
                return false;
            }

            const std::uint32_t overview_y = row / spec.overview_factor;
            const std::size_t overview_row_offset =
                static_cast<std::size_t>(overview_y) *
                static_cast<std::size_t>(overview_width);
            const std::size_t detail_row_offset =
                static_cast<std::size_t>(local_y) *
                static_cast<std::size_t>(spec.detail_tile_pixels);

            for (std::uint32_t x = 0U; x < spec.source_width; ++x) {
                std::int16_t value = 0;
                if (!quantize_elevation(samples[x], value)) {
                    diagnostic =
                        spec.diagnostic_label +
                        " contains a non-finite or out-of-range elevation sample at row " +
                        std::to_string(row) + ", column " + std::to_string(x);
                    return false;
                }

                const std::size_t tile_index =
                    static_cast<std::size_t>(detail_column[x]);
                const std::size_t sample_index =
                    detail_row_offset + static_cast<std::size_t>(detail_local_x[x]);
                band_tiles[tile_index].samples_m[sample_index] = value;

                const std::size_t overview_index =
                    overview_row_offset + static_cast<std::size_t>(overview_column[x]);
                overview_sums[overview_index] += static_cast<std::int64_t>(value);
                if (overview_counts[overview_index] ==
                    std::numeric_limits<std::uint32_t>::max()) {
                    diagnostic = "overview aggregation count overflow";
                    return false;
                }
                ++overview_counts[overview_index];
            }

            if (local_y + 1U == spec.detail_tile_pixels) {
                for (std::uint32_t column = 0U;
                     column < detail_tile_columns;
                     ++column) {
                    std::vector<std::uint8_t> bytes =
                        elevation::encode_elevation_tile_v1(band_tiles[column]);
                    if (bytes.empty()) {
                        diagnostic = "could not encode canonical elevation detail tile";
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
                            "could not embed elevation detail tile r" + row_id + " c" +
                            column_id + ": " + embedded.status.diagnostic;
                        return false;
                    }
                    changed = changed || embedded.inserted ||
                        embedded.representation_changed || embedded.durably_committed;
                    detail_bindings.push_back({
                        "tile:" + std::to_string(resolution_arcsec) + "s:r" +
                            row_id + ":c" + column_id,
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
            spec.diagnostic_label + " streaming decode/import failed: " + decoded.diagnostic
        );
    }
    if (decoded.rows_read != spec.source_height ||
        embedded_detail_tiles !=
            static_cast<std::size_t>(detail_tile_columns) *
                static_cast<std::size_t>(detail_tile_rows)) {
        return failure(
            changed,
            embedded_detail_tiles,
            spec.diagnostic_label + " decode completed with an incomplete row or tile count"
        );
    }

    elevation::ElevationTile overview{};
    overview.width = overview_width;
    overview.height = overview_height;
    overview.west_microarcsec = kWestMicroarcsec;
    overview.north_microarcsec = kNorthMicroarcsec;
    overview.longitude_step_microarcsec = overview_step_microarcsec;
    overview.latitude_step_microarcsec = overview_step_microarcsec;
    overview.vertical_reference = elevation::VerticalReference::egm2008_orthometric;
    overview.samples_m.resize(overview_sums.size(), elevation::kNoDataMeters);
    for (std::size_t index = 0U; index < overview_sums.size(); ++index) {
        if (overview_counts[index] == 0U) {
            return failure(
                changed,
                embedded_detail_tiles,
                spec.diagnostic_label + " overview aggregation produced an empty cell"
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
                spec.diagnostic_label +
                    " overview elevation is outside canonical int16 metre bounds"
            );
        }
        overview.samples_m[index] = static_cast<std::int16_t>(rounded);
    }

    std::vector<std::uint8_t> overview_bytes =
        elevation::encode_elevation_tile_v1(overview);
    if (overview_bytes.empty()) {
        return failure(
            changed,
            embedded_detail_tiles,
            "could not encode elevation overview tile"
        );
    }
    const std::string overview_id =
        prefix + ".overview." + std::to_string(overview_resolution_arcsec) + "s";
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
            "could not embed elevation overview: " + overview_embedded.status.diagnostic
        );
    }
    changed = changed || overview_embedded.inserted ||
        overview_embedded.representation_changed || overview_embedded.durably_committed;

    std::ostringstream manifest;
    manifest
        << "aeris-elevation-import-v1\n"
        << "provider=" << spec.provider << "\n"
        << "dataset=" << spec.dataset << "\n"
        << "version=" << spec.version << "\n"
        << "variant=" << spec.variant_id << "\n"
        << "vertical_reference=EGM2008 orthometric\n"
        << "resolution_arcsec=" << resolution_arcsec << "\n"
        << "width=" << spec.source_width << "\n"
        << "height=" << spec.source_height << "\n"
        << "west_deg=-180\n"
        << "north_deg=90\n"
        << "source_uri=" << spec.source_uri << "\n"
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
            "could not embed elevation provenance: " + manifest_embedded.status.diagnostic
        );
    }
    changed = changed || manifest_embedded.inserted ||
        manifest_embedded.representation_changed || manifest_embedded.durably_committed;

    storage::LayerCreateRequest elevation_layer{};
    elevation_layer.layer_id = elevation_id;
    elevation_layer.role_id = std::string(storage::kLayerRolePhysicalElevationV1);
    elevation_layer.name = spec.display_name;
    elevation_layer.visible = true;
    elevation_layer.resources.reserve(detail_bindings.size() + 2U);
    elevation_layer.resources.push_back({"provenance", manifest_id});
    elevation_layer.resources.push_back({
        "overview:" + std::to_string(overview_resolution_arcsec) + "s",
        overview_id,
    });
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
            spec.diagnostic_label +
                " elevation layer creation failed: " + appended.status.diagnostic
        );
    }
    changed = changed || appended.changed || appended.durably_committed;

    const storage::ProjectLayerListResult after_append = storage::list_project_layers(project);
    if (!after_append.ok()) {
        return failure(
            changed,
            embedded_detail_tiles,
            "could not inspect layer order after elevation import: " +
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
            spec.diagnostic_label +
                " elevation layer ordering failed: " + reordered.status.diagnostic
        );
    }
    changed = changed || reordered.changed || reordered.durably_committed;

    const storage::Status integrity = project.verify_integrity();
    if (!integrity.ok()) {
        return failure(
            changed,
            embedded_detail_tiles,
            "project integrity failed after elevation import: " + integrity.diagnostic
        );
    }

    return {
        true,
        changed,
        embedded_detail_tiles,
        spec.diagnostic_label + " numerical elevation imported into durable .aeris storage",
    };
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
    return import_global_elevation(
        project,
        geotiff_path,
        modified_utc,
        etopo_spec(*variant)
    );
}

namespace testing {

ElevationImportResult import_deterministic_global_elevation_fixture(
    storage::ProjectStore& project,
    const std::filesystem::path& geotiff_path,
    const std::string_view modified_utc
) {
    return import_global_elevation(
        project,
        geotiff_path,
        modified_utc,
        deterministic_fixture_spec()
    );
}

}  // namespace testing

}  // namespace aeris::desktop
