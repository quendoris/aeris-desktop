// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "project_model.hpp"
#include "elevation_style.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/project/source_reader.hpp"
#include "aeris/storage/resource.hpp"

#include <QImage>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace aeris::desktop {
namespace {

[[nodiscard]] bool eager_resource_binding(
    const storage::ProjectLayerRecord& layer,
    const storage::LayerResourceBinding& binding
) noexcept {
    // Country flags are optional annotations and are selected by viewport only
    // after the canonical scene exists. Their PNG bytes must therefore not be
    // streamed merely because a project opened.
    if (layer.role_id == storage::kLayerRoleCountryFlagV1) return false;
    if (layer.role_id != storage::kLayerRolePhysicalElevationV1) return true;
    if (binding.slot_id == "provenance") return true;
    constexpr std::string_view overview_prefix = "overview:";
    return binding.slot_id.size() > overview_prefix.size() &&
        binding.slot_id.compare(0U, overview_prefix.size(), overview_prefix) == 0;
}

[[nodiscard]] bool parse_png_dimensions(
    const std::vector<std::uint8_t>& bytes,
    std::uint32_t& width,
    std::uint32_t& height
) noexcept {
    constexpr std::array<std::uint8_t, 8U> signature{
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
    };
    if (bytes.size() < 24U ||
        !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        bytes[12] != static_cast<std::uint8_t>('I') ||
        bytes[13] != static_cast<std::uint8_t>('H') ||
        bytes[14] != static_cast<std::uint8_t>('D') ||
        bytes[15] != static_cast<std::uint8_t>('R')) {
        return false;
    }

    const auto be32 = [&](const std::size_t offset) noexcept {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
            (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
            static_cast<std::uint32_t>(bytes[offset + 3U]);
    };
    width = be32(16U);
    height = be32(20U);
    return width > 0U && height > 0U;
}

[[nodiscard]] QImage build_elevation_preview(
    const elevation::ElevationTile& tile
) {
    if (tile.width == 0U || tile.height == 0U ||
        tile.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        tile.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        tile.samples_m.size() !=
            static_cast<std::size_t>(tile.width) * static_cast<std::size_t>(tile.height)) {
        return {};
    }

    QImage image(
        static_cast<int>(tile.width),
        static_cast<int>(tile.height),
        QImage::Format_ARGB32_Premultiplied
    );
    if (image.isNull()) return {};

    constexpr double microarcsec_per_degree = 3600.0 * 1000000.0;
    const double lat_step_deg =
        static_cast<double>(tile.latitude_step_microarcsec) /
        microarcsec_per_degree;
    const double north_deg =
        static_cast<double>(tile.north_microarcsec) / microarcsec_per_degree;
    const double step_rad = lat_step_deg * geo::kPi / 180.0;
    const double radius_m = geo::authalic_radius_m();

    constexpr double light_azimuth_rad = 315.0 * geo::kPi / 180.0;
    constexpr double light_altitude_rad = 45.0 * geo::kPi / 180.0;
    const double light_east =
        std::sin(light_azimuth_rad) * std::cos(light_altitude_rad);
    const double light_north =
        std::cos(light_azimuth_rad) * std::cos(light_altitude_rad);
    const double light_up = std::sin(light_altitude_rad);

    const auto sample = [&](const std::uint32_t x, const std::uint32_t y)
        -> std::optional<double> {
        const std::size_t index = static_cast<std::size_t>(y) *
            static_cast<std::size_t>(tile.width) + static_cast<std::size_t>(x);
        const std::int16_t value = tile.samples_m[index];
        if (value == elevation::kNoDataMeters) return std::nullopt;
        return static_cast<double>(value);
    };

    for (std::uint32_t y = 0U; y < tile.height; ++y) {
        auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(static_cast<int>(y)));
        const std::uint32_t north_y = y == 0U ? y : y - 1U;
        const std::uint32_t south_y = y + 1U < tile.height ? y + 1U : y;
        const double latitude_deg = north_deg -
            (static_cast<double>(y) + 0.5) * lat_step_deg;
        const double cos_latitude = std::max(
            1e-6,
            std::abs(std::cos(latitude_deg * geo::kPi / 180.0))
        );
        const double east_west_m = radius_m * step_rad * cos_latitude;
        const double north_south_m = radius_m * step_rad;

        for (std::uint32_t x = 0U; x < tile.width; ++x) {
            const auto center = sample(x, y);
            if (!center.has_value()) {
                pixels[x] = qRgba(0, 0, 0, 0);
                continue;
            }

            const std::uint32_t west_x = x == 0U ? tile.width - 1U : x - 1U;
            const std::uint32_t east_x = x + 1U < tile.width ? x + 1U : 0U;
            const auto west = sample(west_x, y);
            const auto east = sample(east_x, y);
            const auto north = sample(x, north_y);
            const auto south = sample(x, south_y);

            double illumination = light_up;
            if (west && east && north && south &&
                east_west_m > 0.0 && north_south_m > 0.0) {
                const double dz_east = (*east - *west) / (2.0 * east_west_m);
                const double dz_north = (*north - *south) / (2.0 * north_south_m);
                const double normal_east = -dz_east;
                const double normal_north = -dz_north;
                const double normal_up = 1.0;
                const double normal_length = std::sqrt(
                    normal_east * normal_east +
                    normal_north * normal_north +
                    normal_up * normal_up
                );
                if (normal_length > 0.0 && std::isfinite(normal_length)) {
                    illumination =
                        (normal_east * light_east +
                         normal_north * light_north +
                         normal_up * light_up) /
                        normal_length;
                }
            }

            pixels[x] = neutral_elevation_relief_pixel(illumination);
        }
    }
    return image;
}

}  // namespace

ProjectModelLoadResult load_project_model(const storage::ProjectStore& project) {
    const storage::ProjectLayerListResult listed = storage::list_project_layers(project);
    if (!listed.ok()) {
        return {
            nullptr,
            listed.status.diagnostic.empty()
                ? "unable to enumerate project layers"
                : listed.status.diagnostic,
        };
    }

    auto model = std::make_shared<ProjectModel>();
    model->project_path = project.path();
    model->layers = listed.records;

    std::unordered_set<std::string> source_ids;
    std::unordered_set<std::string> resource_ids;
    for (const storage::ProjectLayerRecord& layer : model->layers) {
        for (const storage::LayerSourceBinding& binding : layer.sources) {
            source_ids.insert(binding.source_id);
        }
        for (const storage::LayerResourceBinding& binding : layer.resources) {
            if (eager_resource_binding(layer, binding)) {
                resource_ids.insert(binding.resource_id);
            }
        }
    }

    for (const std::string& source_id : source_ids) {
        project::DurableSourceLoadResult loaded =
            project::load_durable_source_result(project, source_id);
        if (!loaded.ok()) {
            return {
                nullptr,
                "unable to load durable source '" + source_id + "': " +
                    loaded.diagnostic,
            };
        }
        model->sources.emplace(
            source_id,
            std::make_shared<const source::Result>(std::move(loaded.source))
        );
    }

    if (!resource_ids.empty()) {
        const storage::ProjectResourceListResult listed_resources =
            storage::list_project_resources(project);
        if (!listed_resources.ok()) {
            return {
                nullptr,
                "unable to enumerate project resources: " +
                    listed_resources.status.diagnostic,
            };
        }

        std::unordered_map<std::string, const storage::ProjectResourceRecord*> records;
        records.reserve(listed_resources.records.size());
        for (const storage::ProjectResourceRecord& record : listed_resources.records) {
            records.emplace(record.identity.resource_id, &record);
        }

        for (const std::string& resource_id : resource_ids) {
            const auto found = records.find(resource_id);
            if (found == records.end()) {
                return {
                    nullptr,
                    "layer references missing project resource '" + resource_id + "'",
                };
            }
            const storage::ProjectResourceRecord& record = *found->second;
            if (record.storage_mode != storage::ResourceStorageMode::embedded) {
                // External resources have no machine-local path in the durable
                // model. They remain valid project metadata but are unavailable
                // to the renderer until a retrieval/import step embeds them.
                continue;
            }
            if (record.identity.size_bytes >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return {
                    nullptr,
                    "embedded resource is too large for this process: " + resource_id,
                };
            }

            auto resource = std::make_shared<EmbeddedProjectResource>();
            resource->media_type = record.identity.media_type;
            resource->bytes.reserve(
                static_cast<std::size_t>(record.identity.size_bytes)
            );
            const storage::Status streamed = storage::stream_embedded_resource(
                project,
                resource_id,
                [&](const void* data, const std::size_t size) {
                    const auto* begin = static_cast<const std::uint8_t*>(data);
                    resource->bytes.insert(
                        resource->bytes.end(),
                        begin,
                        begin + size
                    );
                    return storage::Status::success();
                }
            );
            if (!streamed.ok()) {
                return {
                    nullptr,
                    "unable to reconstruct embedded resource '" + resource_id + "': " +
                        streamed.diagnostic,
                };
            }
            if (resource->bytes.size() !=
                static_cast<std::size_t>(record.identity.size_bytes)) {
                return {
                    nullptr,
                    "embedded resource size changed during reconstruction: " + resource_id,
                };
            }

            if (resource->media_type == "image/png") {
                // Non-flag PNG resources that are part of eager presentation
                // keep the existing compact-byte path. Country flags never
                // reach this branch; they are loaded by FlagResourceLoader.
                if (!parse_png_dimensions(
                        resource->bytes,
                        resource->raster_width,
                        resource->raster_height
                    )) {
                    return {
                        nullptr,
                        "embedded PNG resource has invalid signature/IHDR: " + resource_id,
                    };
                }
            } else if (resource->media_type == elevation::kElevationTileMediaType) {
                elevation::ElevationTileDecodeResult decoded =
                    elevation::decode_elevation_tile_v1(resource->bytes);
                if (!decoded.ok()) {
                    return {
                        nullptr,
                        "embedded elevation resource failed decoding '" + resource_id + "': " +
                            decoded.diagnostic,
                    };
                }
                resource->elevation_tile = std::move(*decoded.tile);
                resource->elevation_preview_image =
                    build_elevation_preview(*resource->elevation_tile);
                if (resource->elevation_preview_image.isNull()) {
                    return {
                        nullptr,
                        "embedded elevation resource could not build presentation overview: " +
                            resource_id,
                    };
                }
                // The decoded tile is canonical render state. Do not retain a
                // second in-memory copy of the serialized numerical payload.
                resource->bytes.clear();
                resource->bytes.shrink_to_fit();
            }
            model->resources.emplace(resource_id, std::move(resource));
        }
    }

    return {std::move(model), {}};
}

}  // namespace aeris::desktop
