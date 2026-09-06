// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_renderer.hpp"

#include "aeris/elevation/grid.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/storage/resource.hpp"
#include "aeris/view/surface_inverse.hpp"

#include <QPainter>
#include <QTransform>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace aeris::desktop {
namespace {

constexpr std::int64_t kMicroarcsecondsPerArcsecond = 1000000LL;
constexpr std::int64_t kMicroarcsecondsPerDegree =
    3600LL * kMicroarcsecondsPerArcsecond;
constexpr std::int64_t kWorldLongitudeMicroarcsec =
    360LL * kMicroarcsecondsPerDegree;
constexpr std::int64_t kWorldLatitudeMicroarcsec =
    180LL * kMicroarcsecondsPerDegree;
constexpr std::int64_t kWestMicroarcsec =
    -180LL * kMicroarcsecondsPerDegree;
constexpr std::int64_t kNorthMicroarcsec =
    90LL * kMicroarcsecondsPerDegree;

struct Rgb final {
    double r{0.0};
    double g{0.0};
    double b{0.0};
};

struct ParsedDetailBinding final {
    const storage::LayerResourceBinding* binding{nullptr};
    std::uint32_t resolution_arcsec{0U};
    std::uint32_t row{0U};
    std::uint32_t column{0U};
};

struct DetailGrid final {
    bool valid{false};
    std::uint32_t resolution_arcsec{0U};
    std::uint32_t rows{0U};
    std::uint32_t columns{0U};
    std::int64_t tile_longitude_span_microarcsec{0LL};
    std::int64_t tile_latitude_span_microarcsec{0LL};
    std::vector<const storage::LayerResourceBinding*> bindings;
};

[[nodiscard]] Rgb mix(const Rgb a, const Rgb b, const double t) noexcept {
    const double clamped = std::clamp(t, 0.0, 1.0);
    return {
        a.r + (b.r - a.r) * clamped,
        a.g + (b.g - a.g) * clamped,
        a.b + (b.b - a.b) * clamped,
    };
}

[[nodiscard]] Rgb hypsometric_color(const double elevation_m) noexcept {
    if (elevation_m < -6000.0) return {18.0, 34.0, 66.0};
    if (elevation_m < -1000.0) {
        return mix(
            {18.0, 34.0, 66.0},
            {42.0, 78.0, 111.0},
            (elevation_m + 6000.0) / 5000.0
        );
    }
    if (elevation_m < 0.0) {
        return mix(
            {42.0, 78.0, 111.0},
            {67.0, 111.0, 137.0},
            (elevation_m + 1000.0) / 1000.0
        );
    }
    if (elevation_m < 500.0) {
        return mix(
            {83.0, 119.0, 83.0},
            {111.0, 133.0, 87.0},
            elevation_m / 500.0
        );
    }
    if (elevation_m < 1500.0) {
        return mix(
            {111.0, 133.0, 87.0},
            {149.0, 130.0, 96.0},
            (elevation_m - 500.0) / 1000.0
        );
    }
    if (elevation_m < 3000.0) {
        return mix(
            {149.0, 130.0, 96.0},
            {166.0, 149.0, 128.0},
            (elevation_m - 1500.0) / 1500.0
        );
    }
    if (elevation_m < 5000.0) {
        return mix(
            {166.0, 149.0, 128.0},
            {203.0, 199.0, 190.0},
            (elevation_m - 3000.0) / 2000.0
        );
    }
    return mix(
        {203.0, 199.0, 190.0},
        {239.0, 239.0, 237.0},
        (elevation_m - 5000.0) / 3500.0
    );
}

[[nodiscard]] bool parse_uint32(
    const std::string_view text,
    std::uint32_t& value
) noexcept {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

[[nodiscard]] std::optional<ParsedDetailBinding> parse_detail_binding(
    const storage::LayerResourceBinding& binding
) noexcept {
    constexpr std::string_view prefix = "tile:";
    const std::string_view slot(binding.slot_id);
    if (slot.size() <= prefix.size() ||
        slot.compare(0U, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }

    const std::size_t resolution_end = slot.find("s:r", prefix.size());
    if (resolution_end == std::string_view::npos) return std::nullopt;
    const std::size_t column_marker = slot.find(":c", resolution_end + 3U);
    if (column_marker == std::string_view::npos) return std::nullopt;

    ParsedDetailBinding parsed{};
    parsed.binding = &binding;
    if (!parse_uint32(
            slot.substr(prefix.size(), resolution_end - prefix.size()),
            parsed.resolution_arcsec
        ) ||
        !parse_uint32(
            slot.substr(
                resolution_end + 3U,
                column_marker - (resolution_end + 3U)
            ),
            parsed.row
        ) ||
        !parse_uint32(slot.substr(column_marker + 2U), parsed.column) ||
        parsed.resolution_arcsec == 0U) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] DetailGrid detail_grid(
    const storage::ProjectLayerRecord& layer
) {
    DetailGrid grid{};
    std::vector<ParsedDetailBinding> parsed;
    for (const storage::LayerResourceBinding& binding : layer.resources) {
        constexpr std::string_view prefix = "tile:";
        const std::string_view slot(binding.slot_id);
        if (slot.size() <= prefix.size() ||
            slot.compare(0U, prefix.size(), prefix) != 0) {
            continue;
        }
        const auto detail = parse_detail_binding(binding);
        if (!detail.has_value()) return {};
        parsed.push_back(*detail);
    }
    if (parsed.empty()) return {};

    grid.resolution_arcsec = parsed.front().resolution_arcsec;
    std::uint32_t max_row = 0U;
    std::uint32_t max_column = 0U;
    for (const ParsedDetailBinding& detail : parsed) {
        if (detail.resolution_arcsec != grid.resolution_arcsec) return {};
        max_row = std::max(max_row, detail.row);
        max_column = std::max(max_column, detail.column);
    }
    if (max_row == std::numeric_limits<std::uint32_t>::max() ||
        max_column == std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    grid.rows = max_row + 1U;
    grid.columns = max_column + 1U;
    if (grid.rows == 0U || grid.columns == 0U ||
        kWorldLongitudeMicroarcsec % static_cast<std::int64_t>(grid.columns) != 0LL ||
        kWorldLatitudeMicroarcsec % static_cast<std::int64_t>(grid.rows) != 0LL) {
        return {};
    }

    const std::size_t expected =
        static_cast<std::size_t>(grid.rows) *
        static_cast<std::size_t>(grid.columns);
    if (parsed.size() != expected) return {};
    grid.bindings.assign(expected, nullptr);
    for (const ParsedDetailBinding& detail : parsed) {
        const std::size_t index =
            static_cast<std::size_t>(detail.row) *
                static_cast<std::size_t>(grid.columns) +
            static_cast<std::size_t>(detail.column);
        if (index >= grid.bindings.size() || grid.bindings[index] != nullptr) return {};
        grid.bindings[index] = detail.binding;
    }
    if (std::any_of(
            grid.bindings.begin(),
            grid.bindings.end(),
            [](const storage::LayerResourceBinding* binding) {
                return binding == nullptr;
            }
        )) {
        return {};
    }

    grid.tile_longitude_span_microarcsec =
        kWorldLongitudeMicroarcsec / static_cast<std::int64_t>(grid.columns);
    grid.tile_latitude_span_microarcsec =
        kWorldLatitudeMicroarcsec / static_cast<std::int64_t>(grid.rows);
    grid.valid = true;
    return grid;
}

[[nodiscard]] const EmbeddedProjectResource* overview_resource(
    const storage::ProjectLayerRecord& layer,
    const ProjectModel& model
) noexcept {
    constexpr std::string_view prefix = "overview:";
    for (const storage::LayerResourceBinding& binding : layer.resources) {
        if (binding.slot_id.size() <= prefix.size() ||
            binding.slot_id.compare(0U, prefix.size(), prefix) != 0) {
            continue;
        }
        const auto found = model.resources.find(binding.resource_id);
        if (found == model.resources.end() || !found->second ||
            !found->second->elevation_tile.has_value() ||
            found->second->elevation_preview_image.isNull()) {
            continue;
        }
        return found->second.get();
    }
    return nullptr;
}

[[nodiscard]] bool cache_matches(
    const ElevationSurfaceCache& cache,
    const ProjectModel& model,
    const storage::ProjectLayerRecord& layer,
    const RenderFrame& frame,
    const double zoom,
    const QPointF pan,
    const QRect viewport
) noexcept {
    return cache.model == &model &&
        cache.layer_id == layer.layer_id &&
        cache.mode == frame.request.mode &&
        cache.camera_longitude_deg == frame.request.camera_longitude_deg &&
        cache.camera_latitude_deg == frame.request.camera_latitude_deg &&
        cache.projection_central_meridian_deg ==
            frame.request.projection_central_meridian_deg &&
        cache.zoom == zoom && cache.pan == pan &&
        cache.width == viewport.width() && cache.height == viewport.height() &&
        !cache.image.isNull();
}

[[nodiscard]] std::optional<QRgb> geographic_preview_pixel(
    const EmbeddedProjectResource& resource,
    const double longitude_deg,
    const double latitude_deg
) noexcept {
    if (!resource.elevation_tile.has_value() ||
        resource.elevation_preview_image.isNull() ||
        !std::isfinite(longitude_deg) || !std::isfinite(latitude_deg)) {
        return std::nullopt;
    }

    const elevation::ElevationTile& tile = *resource.elevation_tile;
    const QImage& image = resource.elevation_preview_image;
    if (tile.width == 0U || tile.height == 0U ||
        image.width() != static_cast<int>(tile.width) ||
        image.height() != static_cast<int>(tile.height)) {
        return std::nullopt;
    }

    constexpr double microarcsec_per_degree = 3600.0 * 1000000.0;
    const double west_deg =
        static_cast<double>(tile.west_microarcsec) / microarcsec_per_degree;
    const double north_deg =
        static_cast<double>(tile.north_microarcsec) / microarcsec_per_degree;
    const double lon_step_deg =
        static_cast<double>(tile.longitude_step_microarcsec) /
        microarcsec_per_degree;
    const double lat_step_deg =
        static_cast<double>(tile.latitude_step_microarcsec) /
        microarcsec_per_degree;
    if (!(lon_step_deg > 0.0) || !(lat_step_deg > 0.0)) return std::nullopt;

    double x = (longitude_deg - west_deg) / lon_step_deg - 0.5;
    const double longitude_span =
        static_cast<double>(tile.width) * lon_step_deg;
    if (std::abs(longitude_span - 360.0) <= 1e-9) {
        x = std::fmod(x, static_cast<double>(tile.width));
        if (x < 0.0) x += static_cast<double>(tile.width);
    }
    double y = (north_deg - latitude_deg) / lat_step_deg - 0.5;

    if (x < -0.5 || x > static_cast<double>(tile.width) - 0.5 ||
        y < -0.5 || y > static_cast<double>(tile.height) - 0.5) {
        return std::nullopt;
    }
    x = std::clamp(x, 0.0, static_cast<double>(tile.width - 1U));
    y = std::clamp(y, 0.0, static_cast<double>(tile.height - 1U));

    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    const QRgb pixel = image.pixel(ix, iy);
    if (qAlpha(pixel) == 0) return std::nullopt;
    return pixel;
}

[[nodiscard]] bool ensure_detail_store(
    const ProjectModel& model,
    ElevationSurfaceCache& cache
) {
    if (model.project_path.empty()) return false;
    if (cache.detail_project_path == model.project_path) {
        return cache.detail_store != nullptr;
    }

    cache.detail_store.reset();
    cache.detail_tiles.clear();
    cache.detail_failed_resources.clear();
    cache.detail_project_path = model.project_path;
    cache.detail_use_clock = 0U;
    cache.detail_render_epoch = 0U;
    cache.detail_tile_loads = 0U;
    cache.detail_samples_used = 0U;

    storage::ProjectStoreResult opened = storage::ProjectStore::open(model.project_path);
    if (!opened.ok()) return false;
    cache.detail_store = std::move(opened.store);
    return true;
}

[[nodiscard]] bool validate_detail_tile(
    const elevation::ElevationTile& tile,
    const DetailGrid& grid,
    const std::uint32_t row,
    const std::uint32_t column
) noexcept {
    if (!grid.valid || row >= grid.rows || column >= grid.columns ||
        tile.width == 0U || tile.height == 0U ||
        tile.samples_m.size() !=
            static_cast<std::size_t>(tile.width) *
                static_cast<std::size_t>(tile.height)) {
        return false;
    }

    const std::int64_t expected_step =
        static_cast<std::int64_t>(grid.resolution_arcsec) *
        kMicroarcsecondsPerArcsecond;
    if (tile.longitude_step_microarcsec != expected_step ||
        tile.latitude_step_microarcsec != expected_step) {
        return false;
    }

    const std::int64_t longitude_span =
        static_cast<std::int64_t>(tile.width) *
        tile.longitude_step_microarcsec;
    const std::int64_t latitude_span =
        static_cast<std::int64_t>(tile.height) *
        tile.latitude_step_microarcsec;
    if (longitude_span != grid.tile_longitude_span_microarcsec ||
        latitude_span != grid.tile_latitude_span_microarcsec) {
        return false;
    }

    const std::int64_t expected_west =
        kWestMicroarcsec +
        static_cast<std::int64_t>(column) *
            grid.tile_longitude_span_microarcsec;
    const std::int64_t expected_north =
        kNorthMicroarcsec -
        static_cast<std::int64_t>(row) *
            grid.tile_latitude_span_microarcsec;
    return tile.west_microarcsec == expected_west &&
        tile.north_microarcsec == expected_north;
}

[[nodiscard]] const elevation::ElevationTile* detail_tile(
    const DetailGrid& grid,
    const std::uint32_t row,
    const std::uint32_t column,
    ElevationSurfaceCache& cache
) {
    if (!grid.valid || cache.detail_store == nullptr ||
        row >= grid.rows || column >= grid.columns) {
        return nullptr;
    }
    const std::size_t binding_index =
        static_cast<std::size_t>(row) *
            static_cast<std::size_t>(grid.columns) +
        static_cast<std::size_t>(column);
    if (binding_index >= grid.bindings.size() ||
        grid.bindings[binding_index] == nullptr) {
        return nullptr;
    }
    const std::string& resource_id = grid.bindings[binding_index]->resource_id;
    if (cache.detail_failed_resources.find(resource_id) !=
        cache.detail_failed_resources.end()) {
        return nullptr;
    }

    ++cache.detail_use_clock;
    for (ElevationDetailTileCacheEntry& entry : cache.detail_tiles) {
        if (entry.resource_id != resource_id) continue;
        entry.last_used = cache.detail_use_clock;
        entry.render_epoch = cache.detail_render_epoch;
        return &entry.tile;
    }

    ElevationDetailTileCacheEntry* victim = nullptr;
    if (cache.detail_tiles.size() >= kElevationDetailCacheTileLimit) {
        for (ElevationDetailTileCacheEntry& entry : cache.detail_tiles) {
            if (entry.render_epoch == cache.detail_render_epoch) continue;
            if (victim == nullptr || entry.last_used < victim->last_used) {
                victim = &entry;
            }
        }
        if (victim == nullptr) {
            // This render already touched the entire bounded cache. Do not evict
            // a tile still needed by the same frame; fall back to overview for
            // the remainder rather than reading the world in a loop.
            return nullptr;
        }
    }

    std::vector<std::uint8_t> bytes;
    const storage::Status streamed = storage::stream_embedded_resource(
        *cache.detail_store,
        resource_id,
        [&](const void* data, const std::size_t size) {
            const auto* begin = static_cast<const std::uint8_t*>(data);
            bytes.insert(bytes.end(), begin, begin + size);
            return storage::Status::success();
        }
    );
    if (!streamed.ok()) {
        cache.detail_failed_resources.insert(resource_id);
        return nullptr;
    }

    elevation::ElevationTileDecodeResult decoded =
        elevation::decode_elevation_tile_v1(bytes);
    if (!decoded.ok() ||
        !validate_detail_tile(*decoded.tile, grid, row, column)) {
        cache.detail_failed_resources.insert(resource_id);
        return nullptr;
    }

    ElevationDetailTileCacheEntry loaded{};
    loaded.resource_id = resource_id;
    loaded.tile = std::move(*decoded.tile);
    loaded.last_used = cache.detail_use_clock;
    loaded.render_epoch = cache.detail_render_epoch;
    ++cache.detail_tile_loads;

    if (victim != nullptr) {
        *victim = std::move(loaded);
        return &victim->tile;
    }
    cache.detail_tiles.push_back(std::move(loaded));
    return &cache.detail_tiles.back().tile;
}

[[nodiscard]] std::optional<QRgb> styled_detail_pixel(
    const elevation::ElevationTile& tile,
    const double longitude_deg,
    const double latitude_deg
) noexcept {
    if (tile.width == 0U || tile.height == 0U ||
        tile.samples_m.size() !=
            static_cast<std::size_t>(tile.width) *
                static_cast<std::size_t>(tile.height) ||
        !std::isfinite(longitude_deg) || !std::isfinite(latitude_deg)) {
        return std::nullopt;
    }

    constexpr double microarcsec_per_degree = 3600.0 * 1000000.0;
    const double west_deg =
        static_cast<double>(tile.west_microarcsec) / microarcsec_per_degree;
    const double north_deg =
        static_cast<double>(tile.north_microarcsec) / microarcsec_per_degree;
    const double lon_step_deg =
        static_cast<double>(tile.longitude_step_microarcsec) /
        microarcsec_per_degree;
    const double lat_step_deg =
        static_cast<double>(tile.latitude_step_microarcsec) /
        microarcsec_per_degree;
    if (!(lon_step_deg > 0.0) || !(lat_step_deg > 0.0)) return std::nullopt;

    double x = (longitude_deg - west_deg) / lon_step_deg - 0.5;
    double y = (north_deg - latitude_deg) / lat_step_deg - 0.5;
    if (x < -0.5 || x > static_cast<double>(tile.width) - 0.5 ||
        y < -0.5 || y > static_cast<double>(tile.height) - 0.5) {
        return std::nullopt;
    }
    x = std::clamp(x, 0.0, static_cast<double>(tile.width - 1U));
    y = std::clamp(y, 0.0, static_cast<double>(tile.height - 1U));
    const std::uint32_t ix = static_cast<std::uint32_t>(std::lround(x));
    const std::uint32_t iy = static_cast<std::uint32_t>(std::lround(y));

    const auto sample = [&](const std::uint32_t sx, const std::uint32_t sy)
        -> std::optional<double> {
        const std::size_t index =
            static_cast<std::size_t>(sy) * static_cast<std::size_t>(tile.width) +
            static_cast<std::size_t>(sx);
        const std::int16_t value = tile.samples_m[index];
        if (value == elevation::kNoDataMeters) return std::nullopt;
        return static_cast<double>(value);
    };

    const auto center = sample(ix, iy);
    if (!center.has_value()) return std::nullopt;
    const std::uint32_t west_x = ix == 0U ? ix : ix - 1U;
    const std::uint32_t east_x = ix + 1U < tile.width ? ix + 1U : ix;
    const std::uint32_t north_y = iy == 0U ? iy : iy - 1U;
    const std::uint32_t south_y = iy + 1U < tile.height ? iy + 1U : iy;
    const auto west = sample(west_x, iy);
    const auto east = sample(east_x, iy);
    const auto north = sample(ix, north_y);
    const auto south = sample(ix, south_y);

    constexpr double light_azimuth_rad = 315.0 * geo::kPi / 180.0;
    constexpr double light_altitude_rad = 45.0 * geo::kPi / 180.0;
    const double light_east =
        std::sin(light_azimuth_rad) * std::cos(light_altitude_rad);
    const double light_north =
        std::cos(light_azimuth_rad) * std::cos(light_altitude_rad);
    const double light_up = std::sin(light_altitude_rad);

    double illumination = light_up;
    const double latitude_rad = latitude_deg * geo::kPi / 180.0;
    const double east_west_m =
        geo::authalic_radius_m() *
        (lon_step_deg * geo::kPi / 180.0) *
        std::max(1e-6, std::abs(std::cos(latitude_rad)));
    const double north_south_m =
        geo::authalic_radius_m() *
        (lat_step_deg * geo::kPi / 180.0);
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

    const double shade = std::clamp(
        0.62 + 0.58 * std::max(0.0, illumination),
        0.62,
        1.20
    );
    const Rgb base = hypsometric_color(*center);
    const auto channel = [&](const double value) noexcept {
        return static_cast<int>(
            std::lround(std::clamp(value * shade, 0.0, 255.0))
        );
    };
    return qRgba(
        channel(base.r),
        channel(base.g),
        channel(base.b),
        255
    );
}

[[nodiscard]] std::optional<QRgb> geographic_detail_pixel(
    const DetailGrid& grid,
    double longitude_deg,
    const double latitude_deg,
    ElevationSurfaceCache& cache
) {
    if (!grid.valid || !std::isfinite(longitude_deg) ||
        !std::isfinite(latitude_deg)) {
        return std::nullopt;
    }

    longitude_deg = std::fmod(longitude_deg + 180.0, 360.0);
    if (longitude_deg < 0.0) longitude_deg += 360.0;
    longitude_deg -= 180.0;
    const double latitude = std::clamp(latitude_deg, -90.0, 90.0);
    const double longitude_span = 360.0 / static_cast<double>(grid.columns);
    const double latitude_span = 180.0 / static_cast<double>(grid.rows);

    const double raw_column = std::floor((longitude_deg + 180.0) / longitude_span);
    const double raw_row = std::floor((90.0 - latitude) / latitude_span);
    const auto column = static_cast<std::uint32_t>(std::clamp(
        raw_column,
        0.0,
        static_cast<double>(grid.columns - 1U)
    ));
    const auto row = static_cast<std::uint32_t>(std::clamp(
        raw_row,
        0.0,
        static_cast<double>(grid.rows - 1U)
    ));

    const elevation::ElevationTile* tile = detail_tile(grid, row, column, cache);
    if (tile == nullptr) return std::nullopt;
    return styled_detail_pixel(*tile, longitude_deg, latitude);
}

void begin_detail_render_epoch(ElevationSurfaceCache& cache) noexcept {
    if (cache.detail_render_epoch == std::numeric_limits<std::uint64_t>::max()) {
        cache.detail_render_epoch = 1U;
        for (ElevationDetailTileCacheEntry& entry : cache.detail_tiles) {
            entry.render_epoch = 0U;
        }
        return;
    }
    ++cache.detail_render_epoch;
}

void rebuild_cache(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const RenderFrame& frame,
    const ProjectModel& model,
    const EmbeddedProjectResource& resource,
    const double zoom,
    const QPointF pan,
    ElevationSurfaceCache& cache
) {
    const QRect viewport = painter.viewport();
    cache.detail_samples_used = 0U;
    if (viewport.width() <= 0 || viewport.height() <= 0) {
        cache.image = {};
        return;
    }

    bool invertible = false;
    const QTransform device_to_surface =
        painter.worldTransform().inverted(&invertible);
    if (!invertible) {
        cache.image = {};
        return;
    }

    const DetailGrid grid = detail_grid(layer);
    const bool use_detail =
        zoom >= kElevationDetailLodZoom &&
        grid.valid &&
        ensure_detail_store(model, cache);
    cache.detail_lod_active = use_detail;
    if (use_detail) begin_detail_render_epoch(cache);

    const int block = use_detail
        ? (zoom >= 8.0 ? 1 : 2)
        : (zoom >= 3.0 ? 2 : 4);
    const int sample_width = std::max(1, (viewport.width() + block - 1) / block);
    const int sample_height = std::max(1, (viewport.height() + block - 1) / block);
    QImage image(sample_width, sample_height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        cache.image = {};
        return;
    }
    image.fill(Qt::transparent);

    const double device_step_x =
        static_cast<double>(viewport.width()) / static_cast<double>(sample_width);
    const double device_step_y =
        static_cast<double>(viewport.height()) / static_cast<double>(sample_height);

    for (int y = 0; y < sample_height; ++y) {
        auto* output = reinterpret_cast<QRgb*>(image.scanLine(y));
        const double device_y = static_cast<double>(viewport.top()) +
            (static_cast<double>(y) + 0.5) * device_step_y;
        for (int x = 0; x < sample_width; ++x) {
            const double device_x = static_cast<double>(viewport.left()) +
                (static_cast<double>(x) + 0.5) * device_step_x;
            const QPointF surface = device_to_surface.map(QPointF(device_x, device_y));
            const view::SurfaceGeographicPickResult geographic =
                view::pick_geographic_from_surface(
                    frame.request.mode,
                    {surface.x(), surface.y()},
                    frame.request.camera_longitude_deg,
                    frame.request.camera_latitude_deg,
                    frame.request.projection_central_meridian_deg
                );
            if (!geographic.ok) continue;

            std::optional<QRgb> pixel;
            if (use_detail) {
                pixel = geographic_detail_pixel(
                    grid,
                    geographic.longitude_deg,
                    geographic.latitude_deg,
                    cache
                );
                if (pixel.has_value()) ++cache.detail_samples_used;
            }
            if (!pixel.has_value()) {
                pixel = geographic_preview_pixel(
                    resource,
                    geographic.longitude_deg,
                    geographic.latitude_deg
                );
            }
            if (pixel.has_value()) output[x] = *pixel;
        }
    }

    cache.model = &model;
    cache.layer_id = layer.layer_id;
    cache.mode = frame.request.mode;
    cache.camera_longitude_deg = frame.request.camera_longitude_deg;
    cache.camera_latitude_deg = frame.request.camera_latitude_deg;
    cache.projection_central_meridian_deg =
        frame.request.projection_central_meridian_deg;
    cache.zoom = zoom;
    cache.pan = pan;
    cache.width = viewport.width();
    cache.height = viewport.height();
    cache.image = std::move(image);
}

}  // namespace

void draw_elevation_overview(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const RenderFrame& frame,
    const ProjectModel& model,
    const double zoom,
    const QPointF pan,
    ElevationSurfaceCache& cache
) {
    if (!layer.visible || layer.role_id != storage::kLayerRolePhysicalElevationV1) return;
    const EmbeddedProjectResource* resource = overview_resource(layer, model);
    if (resource == nullptr) return;

    const QRect viewport = painter.viewport();
    if (!cache_matches(cache, model, layer, frame, zoom, pan, viewport)) {
        rebuild_cache(painter, layer, frame, model, *resource, zoom, pan, cache);
    }
    if (cache.image.isNull()) return;

    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(viewport), cache.image);
    painter.restore();
}

}  // namespace aeris::desktop
