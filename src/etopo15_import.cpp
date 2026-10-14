// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "etopo15_import.hpp"

#include "elevation_tiff.hpp"

#include "aeris/elevation/grid.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/storage/resource.hpp"
#include "aeris/util/sha256.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aeris::desktop {
namespace {

constexpr std::int64_t kMicroarcsecondsPerDegree = 3600LL * 1000000LL;
constexpr std::int64_t kTileStepMicroarcsec =
    static_cast<std::int64_t>(kEtopo15ResolutionArcsec) * 1000000LL;
constexpr std::string_view kDatasetProvenanceId =
    "builtin.elevation.etopo2022.v1.15s.surface.provenance";

[[nodiscard]] Etopo15TileImportResult failure(
    const bool changed,
    std::string diagnostic
) {
    return {false, changed, std::move(diagnostic)};
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
    identity.required_for_reproduction = false;
    return storage::embed_resource_bytes(
        project,
        identity,
        bytes,
        modified_utc
    );
}

[[nodiscard]] std::vector<std::uint8_t> bytes_from_string(
    const std::string& text
) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

[[nodiscard]] const storage::ProjectLayerRecord* find_layer(
    const std::vector<storage::ProjectLayerRecord>& layers,
    const std::string& layer_id
) noexcept {
    const auto found = std::find_if(
        layers.begin(),
        layers.end(),
        [&](const storage::ProjectLayerRecord& layer) {
            return layer.layer_id == layer_id;
        }
    );
    return found == layers.end() ? nullptr : &*found;
}

[[nodiscard]] bool has_land_layer(
    const std::vector<storage::ProjectLayerRecord>& layers
) noexcept {
    return std::any_of(
        layers.begin(),
        layers.end(),
        [](const storage::ProjectLayerRecord& layer) {
            return layer.role_id == storage::kLayerRolePhysicalLandFillV1;
        }
    );
}

[[nodiscard]] bool has_resource_binding(
    const storage::ProjectLayerRecord& layer,
    const std::string_view slot_id,
    const std::string_view resource_id
) noexcept {
    return std::any_of(
        layer.resources.begin(),
        layer.resources.end(),
        [&](const storage::LayerResourceBinding& binding) {
            return binding.slot_id == slot_id &&
                binding.resource_id == resource_id;
        }
    );
}

[[nodiscard]] bool slot_bound_to_other_resource(
    const storage::ProjectLayerRecord& layer,
    const std::string_view slot_id,
    const std::string_view resource_id
) noexcept {
    return std::any_of(
        layer.resources.begin(),
        layer.resources.end(),
        [&](const storage::LayerResourceBinding& binding) {
            return binding.slot_id == slot_id &&
                binding.resource_id != resource_id;
        }
    );
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

    const auto land = std::find_if(
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

[[nodiscard]] std::string source_identity_resource_id(
    const Etopo15SurfaceTileDescriptor& tile
) {
    return
        "source.etopo2022.v1.15s.surface.r" +
        std::to_string(tile.row) +
        ".c" +
        std::to_string(tile.column);
}

[[nodiscard]] std::vector<std::uint8_t> dataset_provenance_bytes() {
    std::ostringstream manifest;
    manifest
        << "aeris-elevation-provider-v1\n"
        << "provider=NOAA/NCEI\n"
        << "dataset=ETOPO 2022\n"
        << "version=1\n"
        << "variant=surface\n"
        << "vertical_reference=EGM2008 orthometric\n"
        << "resolution_arcsec=15\n"
        << "grid_rows=12\n"
        << "grid_columns=24\n"
        << "tile_width=3600\n"
        << "tile_height=3600\n"
        << "tile_span_deg=15\n"
        << "source_identity_template=source.etopo2022.v1.15s.surface.r{row}.c{column}\n"
        << "source_uri_base=https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/15s/15s_surface_elev_gtif/\n";
    return bytes_from_string(manifest.str());
}

}  // namespace

Etopo15TileImportResult import_etopo2022_surface_15s_tile(
    storage::ProjectStore& project,
    const Etopo15SurfaceTileDescriptor& tile,
    const std::filesystem::path& geotiff_path,
    const std::string_view modified_utc
) {
    if (geotiff_path.empty() ||
        !storage::is_canonical_utc_timestamp(modified_utc)) {
        return failure(false, "ETOPO 15s tile import requires a GeoTIFF and canonical UTC timestamp");
    }

    const auto canonical =
        etopo15_surface_tile_for_indices(tile.row, tile.column);
    if (!canonical.has_value() ||
        canonical->filename != tile.filename ||
        canonical->url != tile.url ||
        canonical->resource_id != tile.resource_id ||
        canonical->slot_id != tile.slot_id ||
        canonical->layer_id != tile.layer_id) {
        return failure(false, "ETOPO 15s tile descriptor is not canonical for its grid cell");
    }

    if (geotiff_path.filename().string() != tile.filename) {
        return failure(false, "ETOPO 15s tile filename does not match its geographic descriptor");
    }
    if (project.metadata().frozen) {
        return failure(false, "ETOPO 15s tile import refuses a frozen project");
    }

    const storage::ProjectLayerListResult before =
        storage::list_project_layers(project);
    if (!before.ok()) {
        return failure(false, "could not inspect project layers: " + before.status.diagnostic);
    }
    if (!has_land_layer(before.records)) {
        return failure(
            false,
            "ETOPO 15s viewport terrain requires the verified base world first"
        );
    }

    const storage::ProjectLayerRecord* existing_layer =
        find_layer(before.records, tile.layer_id);
    if (existing_layer != nullptr &&
        existing_layer->role_id != storage::kLayerRolePhysicalElevationV1) {
        return failure(false, "ETOPO 15s layer ID exists with an incompatible semantic role");
    }
    if (existing_layer != nullptr &&
        slot_bound_to_other_resource(
            *existing_layer,
            tile.slot_id,
            tile.resource_id
        )) {
        return failure(false, "ETOPO 15s tile slot is already bound to different durable content");
    }

    const Float32TiffInspectResult inspected =
        inspect_single_band_float32_tiff(geotiff_path);
    if (!inspected.ok()) {
        return failure(
            false,
            "ETOPO 15s TIFF preflight failed: " + inspected.diagnostic
        );
    }
    if (inspected.info.width != kEtopo15TilePixels ||
        inspected.info.height != kEtopo15TilePixels) {
        return failure(
            false,
            "ETOPO 15s tile must be exactly 3600x3600 Float32 samples"
        );
    }

    std::error_code size_error;
    const std::uintmax_t source_size =
        std::filesystem::file_size(geotiff_path, size_error);
    if (size_error || source_size == 0U ||
        source_size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::uint64_t>::max()
        )) {
        return failure(false, "could not inspect ETOPO 15s source file size");
    }
    const util::Sha256FileResult source_hash =
        util::sha256_file(geotiff_path);
    if (!source_hash.ok()) {
        return failure(false, "could not hash ETOPO 15s source GeoTIFF");
    }

    elevation::ElevationTile canonical_tile{};
    canonical_tile.width = kEtopo15TilePixels;
    canonical_tile.height = kEtopo15TilePixels;
    canonical_tile.west_microarcsec =
        static_cast<std::int64_t>(tile.west_deg) *
        kMicroarcsecondsPerDegree;
    canonical_tile.north_microarcsec =
        static_cast<std::int64_t>(tile.north_deg) *
        kMicroarcsecondsPerDegree;
    canonical_tile.longitude_step_microarcsec = kTileStepMicroarcsec;
    canonical_tile.latitude_step_microarcsec = kTileStepMicroarcsec;
    canonical_tile.vertical_reference =
        elevation::VerticalReference::egm2008_orthometric;
    canonical_tile.samples_m.resize(
        static_cast<std::size_t>(kEtopo15TilePixels) *
        static_cast<std::size_t>(kEtopo15TilePixels),
        elevation::kNoDataMeters
    );

    const Float32TiffReadResult decoded =
        read_single_band_float32_tiff(
            geotiff_path,
            [&](const std::uint32_t row,
                const float* samples,
                const std::size_t count,
                std::string& diagnostic) {
                if (row >= kEtopo15TilePixels ||
                    count != static_cast<std::size_t>(kEtopo15TilePixels)) {
                    diagnostic =
                        "decoded ETOPO 15s row disagrees with the 3600x3600 contract";
                    return false;
                }

                const std::size_t offset =
                    static_cast<std::size_t>(row) *
                    static_cast<std::size_t>(kEtopo15TilePixels);
                for (std::uint32_t column = 0U;
                     column < kEtopo15TilePixels;
                     ++column) {
                    std::int16_t value = 0;
                    if (!quantize_elevation(samples[column], value)) {
                        diagnostic =
                            "ETOPO 15s contains a non-finite or out-of-range sample at row " +
                            std::to_string(row) +
                            ", column " +
                            std::to_string(column);
                        return false;
                    }
                    canonical_tile.samples_m[
                        offset + static_cast<std::size_t>(column)
                    ] = value;
                }
                return true;
            }
        );
    if (!decoded.ok() || decoded.rows_read != kEtopo15TilePixels) {
        return failure(
            false,
            "ETOPO 15s decode failed: " + decoded.diagnostic
        );
    }

    std::vector<std::uint8_t> tile_bytes =
        elevation::encode_elevation_tile_v1(canonical_tile);
    if (tile_bytes.empty()) {
        return failure(false, "could not encode canonical ETOPO 15s elevation tile");
    }

    bool changed = false;
    const storage::ResourceMutationResult embedded_tile =
        embed_generated(
            project,
            tile.resource_id,
            std::string(elevation::kElevationTileMediaType),
            tile_bytes,
            modified_utc
        );
    if (!embedded_tile.ok()) {
        return failure(
            changed,
            "could not embed ETOPO 15s canonical tile: " +
                embedded_tile.status.diagnostic
        );
    }
    changed = changed ||
        embedded_tile.inserted ||
        embedded_tile.representation_changed ||
        embedded_tile.durably_committed;

    storage::ProjectResourceIdentity source_identity{};
    source_identity.resource_id = source_identity_resource_id(tile);
    source_identity.sha256 = source_hash.digest.hex();
    source_identity.media_type = "image/tiff";
    source_identity.size_bytes = static_cast<std::uint64_t>(source_size);
    source_identity.retrieval_uri = tile.url;
    source_identity.required_for_reproduction = false;
    const storage::ResourceMutationResult recorded_source =
        storage::store_external_resource(
            project,
            source_identity,
            modified_utc
        );
    if (!recorded_source.ok()) {
        return failure(
            changed,
            "could not record exact ETOPO 15s source identity: " +
                recorded_source.status.diagnostic
        );
    }
    changed = changed ||
        recorded_source.inserted ||
        recorded_source.representation_changed ||
        recorded_source.durably_committed;

    const std::vector<std::uint8_t> provenance_bytes =
        dataset_provenance_bytes();
    const storage::ResourceMutationResult embedded_provenance =
        embed_generated(
            project,
            std::string(kDatasetProvenanceId),
            "text/plain; charset=utf-8",
            provenance_bytes,
            modified_utc
        );
    if (!embedded_provenance.ok()) {
        return failure(
            changed,
            "could not embed ETOPO 15s dataset provenance: " +
                embedded_provenance.status.diagnostic
        );
    }
    changed = changed ||
        embedded_provenance.inserted ||
        embedded_provenance.representation_changed ||
        embedded_provenance.durably_committed;

    // Refresh the layer snapshot because the resource writes above may have
    // advanced project metadata but cannot change the layer graph.
    const storage::ProjectLayerListResult current =
        storage::list_project_layers(project);
    if (!current.ok()) {
        return failure(
            changed,
            "could not re-read project layers before ETOPO 15s binding: " +
                current.status.diagnostic
        );
    }
    existing_layer = find_layer(current.records, tile.layer_id);

    if (existing_layer == nullptr) {
        storage::LayerCreateRequest layer{};
        layer.layer_id = tile.layer_id;
        layer.role_id =
            std::string(storage::kLayerRolePhysicalElevationV1);
        layer.name = "ETOPO 2022 Surface elevation · streamed 15 arc-sec";
        layer.visible = true;
        layer.resources.push_back({
            "provenance",
            std::string(kDatasetProvenanceId),
        });
        layer.resources.push_back({
            tile.slot_id,
            tile.resource_id,
        });

        const storage::LayerMutationResult appended =
            storage::append_layer(
                project,
                layer,
                modified_utc
            );
        if (!appended.ok()) {
            return failure(
                changed || appended.changed || appended.durably_committed,
                "could not create streamed ETOPO 15s layer: " +
                    appended.status.diagnostic
            );
        }
        changed = changed || appended.changed || appended.durably_committed;

        const storage::ProjectLayerListResult after_append =
            storage::list_project_layers(project);
        if (!after_append.ok()) {
            return failure(
                changed,
                "could not inspect layer order after ETOPO 15s import: " +
                    after_append.status.diagnostic
            );
        }
        const storage::LayerMutationResult reordered =
            storage::set_layer_order(
                project,
                elevation_layer_order(
                    after_append.records,
                    tile.layer_id
                ),
                modified_utc
            );
        if (!reordered.ok()) {
            return failure(
                changed || reordered.changed || reordered.durably_committed,
                "could not position ETOPO 15s layer below physical land: " +
                    reordered.status.diagnostic
            );
        }
        changed = changed || reordered.changed || reordered.durably_committed;
    } else {
        storage::LayerBindingAppendRequest append{};
        if (!has_resource_binding(
                *existing_layer,
                "provenance",
                kDatasetProvenanceId
            )) {
            if (slot_bound_to_other_resource(
                    *existing_layer,
                    "provenance",
                    kDatasetProvenanceId
                )) {
                return failure(
                    changed,
                    "ETOPO 15s layer provenance slot conflicts with existing content"
                );
            }
            append.resources.push_back({
                "provenance",
                std::string(kDatasetProvenanceId),
            });
        }
        if (!has_resource_binding(
                *existing_layer,
                tile.slot_id,
                tile.resource_id
            )) {
            append.resources.push_back({
                tile.slot_id,
                tile.resource_id,
            });
        }

        if (!append.resources.empty()) {
            const storage::LayerMutationResult grown =
                storage::append_layer_bindings(
                    project,
                    tile.layer_id,
                    append,
                    modified_utc
                );
            if (!grown.ok()) {
                return failure(
                    changed || grown.changed || grown.durably_committed,
                    "could not attach ETOPO 15s tile to streamed layer: " +
                        grown.status.diagnostic
                );
            }
            changed = changed || grown.changed || grown.durably_committed;
        }
    }

    const storage::Status integrity = project.verify_integrity();
    if (!integrity.ok()) {
        return failure(
            changed,
            "project integrity failed after ETOPO 15s tile import: " +
                integrity.diagnostic
        );
    }

    return {
        true,
        changed,
        "ETOPO 15s tile materialized into durable .aeris storage"
    };
}

}  // namespace aeris::desktop
