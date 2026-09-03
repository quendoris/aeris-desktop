// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_workspace_view.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/view/surface.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace aeris::desktop {
namespace {

constexpr int kMapMarginPx = 24;
constexpr double kSeamHitRadiusPx = 9.0;

struct SeamSegment final {
    geometry::PlanarPoint first{};
    geometry::PlanarPoint second{};
};

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

[[nodiscard]] std::vector<SeamSegment> visible_seam_segments(
    const view::ProjectionSeamGeometry& seam
) {
    std::vector<SeamSegment> segments;
    if (!seam.ok || seam.samples.size() < 2U) return segments;
    segments.reserve(seam.samples.size());

    for (std::size_t index = 1U; index < seam.samples.size(); ++index) {
        const auto& first = seam.samples[index - 1U];
        const auto& second = seam.samples[index];

        if (first.globe_visible && second.globe_visible) {
            segments.push_back({first.globe, second.globe});
            continue;
        }
        if (first.globe_visible == second.globe_visible) continue;

        const geometry::PlanarPoint horizon = horizon_intersection(first, second);
        if (first.globe_visible) {
            segments.push_back({first.globe, horizon});
        } else {
            segments.push_back({horizon, second.globe});
        }
    }
    return segments;
}

[[nodiscard]] QPainterPath visible_seam_path(
    const std::vector<SeamSegment>& segments
) {
    QPainterPath path;
    for (const SeamSegment& segment : segments) {
        path.moveTo(segment.first.x, segment.first.y);
        path.lineTo(segment.second.x, segment.second.y);
    }
    return path;
}

[[nodiscard]] QTransform globe_device_transform(const MapWorkspaceView& view) {
    const double radius = geo::authalic_radius_m();
    const double available_width = std::max(1, view.width() - 2 * kMapMarginPx);
    const double available_height = std::max(1, view.height() - 2 * kMapMarginPx);
    const double base_scale = std::min(
        available_width / (2.0 * radius),
        available_height / (2.0 * radius)
    );

    QTransform transform;
    transform.translate(
        static_cast<double>(view.width()) * 0.5 + view.viewport_pan().x(),
        static_cast<double>(view.height()) * 0.5 + view.viewport_pan().y()
    );
    transform.scale(base_scale * view.zoom_factor(), -base_scale * view.zoom_factor());
    return transform;
}

[[nodiscard]] view::ProjectionSeamGeometry current_projection_seam(
    const MapWorkspaceView& view
) {
    return view::build_projection_seam_geometry(
        view.unfold_target_mode(),
        view.displayed_camera_longitude_deg(),
        view.displayed_camera_latitude_deg(),
        view.projection_central_meridian_deg()
    );
}

[[nodiscard]] double point_segment_distance(
    const QPointF point,
    const QPointF first,
    const QPointF second
) noexcept {
    const QPointF edge = second - first;
    const double length2 = edge.x() * edge.x() + edge.y() * edge.y();
    if (length2 <= 1e-12) {
        return std::hypot(point.x() - first.x(), point.y() - first.y());
    }

    const QPointF from_first = point - first;
    const double projection =
        (from_first.x() * edge.x() + from_first.y() * edge.y()) / length2;
    const double t = std::clamp(projection, 0.0, 1.0);
    const QPointF closest = first + t * edge;
    return std::hypot(point.x() - closest.x(), point.y() - closest.y());
}

[[nodiscard]] bool seam_hit_test(
    const MapWorkspaceView& view,
    const QPointF device_point
) {
    const auto seam = current_projection_seam(view);
    const auto segments = visible_seam_segments(seam);
    if (segments.empty()) return false;

    const QTransform transform = globe_device_transform(view);
    double best = std::numeric_limits<double>::infinity();
    for (const SeamSegment& segment : segments) {
        const QPointF first = transform.map(QPointF(segment.first.x, segment.first.y));
        const QPointF second = transform.map(QPointF(segment.second.x, segment.second.y));
        best = std::min(best, point_segment_distance(device_point, first, second));
        if (best <= kSeamHitRadiusPx) return true;
    }
    return false;
}

[[nodiscard]] bool device_to_globe_point(
    const MapWorkspaceView& view,
    const QPointF device_point,
    geometry::PlanarPoint& globe_point
) {
    bool invertible = false;
    const QTransform inverse = globe_device_transform(view).inverted(&invertible);
    if (!invertible) return false;
    const QPointF local = inverse.map(device_point);
    if (!std::isfinite(local.x()) || !std::isfinite(local.y())) return false;
    globe_point = {local.x(), local.y()};
    return true;
}

[[nodiscard]] bool cut_tool_can_interact(const MapWorkspaceView& view) noexcept {
    return view.unfold_tool_active() &&
        view.surface_mode() == view::SurfaceMode::globe &&
        view.has_current_frame();
}

}  // namespace

MapWorkspaceView::MapWorkspaceView(QWidget* parent)
    : MapView(parent) {}

void MapWorkspaceView::set_unfold_tool_active(const bool active) {
    if (unfold_tool_active_ == active) return;
    unfold_tool_active_ = active;
    if (!active && dragging_projection_cut_) {
        dragging_projection_cut_ = false;
        unsetCursor();
    }
    update();
}

void MapWorkspaceView::set_unfold_target_mode(const view::SurfaceMode mode) {
    if (mode == view::SurfaceMode::globe || unfold_target_mode_ == mode) return;
    unfold_target_mode_ = mode;
    update();
}

void MapWorkspaceView::paintEvent(QPaintEvent* event) {
    MapView::paintEvent(event);

    if (!cut_tool_can_interact(*this)) return;

    const auto segments = visible_seam_segments(current_projection_seam(*this));
    const QPainterPath path = visible_seam_path(segments);
    if (path.isEmpty()) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setWorldTransform(globe_device_transform(*this));
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

void MapWorkspaceView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton &&
        cut_tool_can_interact(*this) &&
        seam_hit_test(*this, event->position())) {
        dragging_projection_cut_ = true;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    MapView::mousePressEvent(event);
}

void MapWorkspaceView::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_projection_cut_) {
        geometry::PlanarPoint globe_point{};
        if (device_to_globe_point(*this, event->position(), globe_point)) {
            const view::ProjectionCutPickResult picked = view::pick_projection_cut_from_globe(
                unfold_target_mode_,
                displayed_camera_longitude_deg(),
                displayed_camera_latitude_deg(),
                globe_point
            );
            if (picked.ok) {
                set_projection_central_meridian_deg(
                    picked.projection_central_meridian_deg
                );
                emit projectionCutEdited(projection_central_meridian_deg());
            }
        }
        event->accept();
        return;
    }

    if (event->buttons() == Qt::NoButton && cut_tool_can_interact(*this)) {
        if (seam_hit_test(*this, event->position())) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
    }
    MapView::mouseMoveEvent(event);
}

void MapWorkspaceView::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_projection_cut_ && event->button() == Qt::LeftButton) {
        dragging_projection_cut_ = false;
        if (cut_tool_can_interact(*this) && seam_hit_test(*this, event->position())) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
        event->accept();
        return;
    }

    MapView::mouseReleaseEvent(event);
}

}  // namespace aeris::desktop
