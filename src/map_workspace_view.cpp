// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_workspace_view.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/view/surface.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace aeris::desktop {
namespace {

constexpr int kMapMarginPx = 24;

[[nodiscard]] geometry::PlanarPoint horizon_intersection(
    const view::ProjectionSeamSample& first,
    const view::ProjectionSeamSample& second
) noexcept {
    const double denominator = first.globe_depth_normalized - second.globe_depth_normalized;
    if (std::abs(denominator) <= 1e-15) {
        return first.globe;
    }
    const double t = std::clamp(
        first.globe_depth_normalized / denominator,
        0.0,
        1.0
    );
    return {
        first.globe.x + (second.globe.x - first.globe.x) * t,
        first.globe.y + (second.globe.y - first.globe.y) * t,
    };
}

[[nodiscard]] QPainterPath visible_seam_path(
    const view::ProjectionSeamGeometry& seam
) {
    QPainterPath path;
    if (!seam.ok || seam.samples.size() < 2U) return path;

    for (std::size_t index = 1U; index < seam.samples.size(); ++index) {
        const auto& first = seam.samples[index - 1U];
        const auto& second = seam.samples[index];

        if (first.globe_visible && second.globe_visible) {
            path.moveTo(first.globe.x, first.globe.y);
            path.lineTo(second.globe.x, second.globe.y);
            continue;
        }
        if (first.globe_visible == second.globe_visible) continue;

        const geometry::PlanarPoint horizon = horizon_intersection(first, second);
        if (first.globe_visible) {
            path.moveTo(first.globe.x, first.globe.y);
            path.lineTo(horizon.x, horizon.y);
        } else {
            path.moveTo(horizon.x, horizon.y);
            path.lineTo(second.globe.x, second.globe.y);
        }
    }
    return path;
}

}  // namespace

MapWorkspaceView::MapWorkspaceView(QWidget* parent)
    : MapView(parent) {}

void MapWorkspaceView::set_unfold_tool_active(const bool active) {
    if (unfold_tool_active_ == active) return;
    unfold_tool_active_ = active;
    update();
}

void MapWorkspaceView::set_unfold_target_mode(const view::SurfaceMode mode) {
    if (mode == view::SurfaceMode::globe || unfold_target_mode_ == mode) return;
    unfold_target_mode_ = mode;
    update();
}

void MapWorkspaceView::paintEvent(QPaintEvent* event) {
    MapView::paintEvent(event);

    if (!unfold_tool_active_ ||
        surface_mode() != view::SurfaceMode::globe ||
        !has_current_frame()) {
        return;
    }

    const view::ProjectionSeamGeometry seam = view::build_projection_seam_geometry(
        unfold_target_mode_,
        displayed_camera_longitude_deg(),
        displayed_camera_latitude_deg(),
        projection_central_meridian_deg()
    );
    const QPainterPath path = visible_seam_path(seam);
    if (path.isEmpty()) return;

    const double radius = geo::authalic_radius_m();
    const double available_width = std::max(1, width() - 2 * kMapMarginPx);
    const double available_height = std::max(1, height() - 2 * kMapMarginPx);
    const double base_scale = std::min(
        available_width / (2.0 * radius),
        available_height / (2.0 * radius)
    );

    QTransform transform;
    transform.translate(
        static_cast<double>(width()) * 0.5 + viewport_pan().x(),
        static_cast<double>(height()) * 0.5 + viewport_pan().y()
    );
    transform.scale(base_scale * zoom_factor(), -base_scale * zoom_factor());

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setWorldTransform(transform);
    painter.setBrush(Qt::NoBrush);

    QPen halo(QColor(18, 21, 24, 220));
    halo.setCosmetic(true);
    halo.setWidthF(4.2);
    halo.setCapStyle(Qt::RoundCap);
    halo.setJoinStyle(Qt::RoundJoin);
    painter.setPen(halo);
    painter.drawPath(path);

    QPen cut(QColor(244, 184, 92));
    cut.setCosmetic(true);
    cut.setWidthF(1.8);
    cut.setCapStyle(Qt::RoundCap);
    cut.setJoinStyle(Qt::RoundJoin);
    painter.setPen(cut);
    painter.drawPath(path);
}

}  // namespace aeris::desktop
