// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_renderer.hpp"

#include "aeris/elevation/grid.hpp"
#include "aeris/view/surface_inverse.hpp"

#include <QPainter>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace aeris::desktop {
namespace {

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

    // Whole-world interaction is intentionally coarse. As the user zooms in,
    // overview reprojection sharpens before future detail-tile LOD takes over.
    const int block = zoom >= 3.0 ? 2 : 4;
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

            const auto pixel = geographic_preview_pixel(
                resource,
                geographic.longitude_deg,
                geographic.latitude_deg
            );
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
