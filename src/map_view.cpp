// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_view.hpp"

#include "aeris/storage/layer.hpp"
#include "aeris/surface/classification.hpp"
#include "aeris/view/surface.hpp"
#include "aeris/view/surface_inverse.hpp"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace aeris::desktop {
namespace {

constexpr int kMapMarginPx = 24;
constexpr double kMinimumZoom = 0.45;
constexpr double kMaximumZoom = 256.0;
constexpr double kWheelZoomBase = 1.18;
constexpr double kTrackpadPixelsPerStep = 40.0;
constexpr double kKeyboardZoomFactor = 1.25;
constexpr double kDoubleClickZoomFactor = 1.8;
constexpr int kTerrainInteractionRefineDelayMs = 120;

[[nodiscard]] double wrap_longitude(double value) noexcept {
    value = std::fmod(value + 180.0, 360.0);
    if (value < 0.0) value += 360.0;
    return value - 180.0;
}

[[nodiscard]] QPainterPath fill_path(const view::SceneFeatureGeometry& feature) {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const auto& ring : feature.fill_rings) {
        if (ring.size() < 3U) continue;
        path.moveTo(ring.front().x, ring.front().y);
        for (std::size_t index = 1U; index < ring.size(); ++index) {
            path.lineTo(ring[index].x, ring[index].y);
        }
        path.closeSubpath();
    }
    return path;
}

[[nodiscard]] QPainterPath surface_path(
    const view::PlanarSurfaceGeometry& surface
) {
    QPainterPath path;
    if (!surface.ok || surface.outline.size() < 3U) return path;
    path.moveTo(surface.outline.front().x, surface.outline.front().y);
    for (std::size_t index = 1U; index < surface.outline.size(); ++index) {
        path.lineTo(surface.outline[index].x, surface.outline[index].y);
    }
    path.closeSubpath();
    return path;
}

[[nodiscard]] bool combined_bounds(
    const RenderFrame& frame,
    double& min_x,
    double& min_y,
    double& max_x,
    double& max_y
) {
    if (frame.request.mode != view::SurfaceMode::globe) {
        const view::PlanarSurfaceGeometry surface =
            view::build_planar_surface_geometry(frame.request.mode);
        if (surface.ok) {
            min_x = surface.min_x;
            min_y = surface.min_y;
            max_x = surface.max_x;
            max_y = surface.max_y;
            return max_x > min_x && max_y > min_y;
        }
    }

    bool found = false;
    for (const auto& entry : frame.source_scenes) {
        const view::SceneGeometry& scene = entry.second;
        if (!scene.ok) continue;
        if (!found) {
            min_x = scene.min_x;
            min_y = scene.min_y;
            max_x = scene.max_x;
            max_y = scene.max_y;
            found = true;
        } else {
            min_x = std::min(min_x, scene.min_x);
            min_y = std::min(min_y, scene.min_y);
            max_x = std::max(max_x, scene.max_x);
            max_y = std::max(max_y, scene.max_y);
        }
    }
    return found && max_x > min_x && max_y > min_y;
}

[[nodiscard]] const source::Feature* find_source_feature(
    const source::Result& source_result,
    const std::string& stable_id
) noexcept {
    for (const source::Feature& feature : source_result.features) {
        if (feature.stable_id == stable_id) return &feature;
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::string> text_property(
    const source::Feature& feature,
    const std::string_view key
) {
    for (const source::FeatureProperty& property : feature.properties) {
        if (property.key != key) continue;
        const auto* text = std::get_if<std::string>(&property.value);
        if (text != nullptr) return *text;
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<surface::SurfaceClass> surface_class_property(
    const source::Feature& feature
) {
    const auto id = text_property(feature, surface::kSurfaceClassPropertyKey);
    if (!id.has_value()) return std::nullopt;
    return surface::parse_surface_class_id(*id);
}

[[nodiscard]] std::optional<QColor> surface_class_color(
    const surface::SurfaceClass value
) noexcept {
    switch (value) {
    case surface::SurfaceClass::unknown:
        return std::nullopt;
    case surface::SurfaceClass::water:
        return QColor(58, 96, 126, 245);
    case surface::SurfaceClass::land:
        return QColor(178, 184, 168, 245);
    case surface::SurfaceClass::grounded_ice:
        return QColor(229, 234, 235, 248);
    case surface::SurfaceClass::floating_ice_shelf:
        return QColor(211, 226, 233, 248);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> manifest_field(
    const EmbeddedProjectResource& resource,
    const std::string_view key
) {
    if (resource.bytes.empty() || key.empty()) return std::nullopt;
    const std::string_view text(
        reinterpret_cast<const char*>(resource.bytes.data()),
        resource.bytes.size()
    );
    const std::string prefix = std::string(key) + "=";
    std::size_t line_start = 0U;
    while (line_start <= text.size()) {
        const std::size_t line_end = text.find('\n', line_start);
        const std::size_t end =
            line_end == std::string_view::npos ? text.size() : line_end;
        const std::string_view line = text.substr(line_start, end - line_start);
        if (line.size() >= prefix.size() &&
            line.compare(0U, prefix.size(), prefix) == 0) {
            return std::string(line.substr(prefix.size()));
        }
        if (line_end == std::string_view::npos) break;
        line_start = line_end + 1U;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> integer_property(
    const source::Feature& feature,
    const std::string_view key
) noexcept {
    for (const source::FeatureProperty& property : feature.properties) {
        if (property.key != key) continue;
        const auto* value = std::get_if<std::int64_t>(&property.value);
        if (value != nullptr) return *value;
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] QColor political_country_color(
    const source::Feature* feature
) noexcept {
    constexpr int alpha = 214;
    if (feature != nullptr) {
        const auto assignment = integer_property(*feature, "mapcolor7");
        if (assignment.has_value()) {
            switch (*assignment) {
            case 1: return QColor(189, 128, 119, alpha);
            case 2: return QColor(154, 171, 113, alpha);
            case 3: return QColor(108, 154, 181, alpha);
            case 4: return QColor(189, 156, 96, alpha);
            case 5: return QColor(143, 124, 176, alpha);
            case 6: return QColor(105, 169, 148, alpha);
            case 7: return QColor(181, 118, 151, alpha);
            default: break;
            }
        }
    }
    // Backward-compatible fallback for older .aeris projects without the
    // cartographic enrichment channel.
    return QColor(123, 143, 157, 120);
}

[[nodiscard]] double ring_area2(
    const std::vector<geometry::PlanarPoint>& ring
) noexcept {
    if (ring.size() < 3U) return 0.0;
    double result = 0.0;
    for (std::size_t index = 0U; index < ring.size(); ++index) {
        const auto& a = ring[index];
        const auto& b = ring[(index + 1U) % ring.size()];
        result += a.x * b.y - b.x * a.y;
    }
    return result;
}

[[nodiscard]] std::optional<geometry::PlanarPoint> ring_anchor(
    const std::vector<geometry::PlanarPoint>& ring
) noexcept {
    if (ring.size() < 3U) return std::nullopt;
    const double area2 = ring_area2(ring);
    if (std::abs(area2) <= 1e-12) return std::nullopt;

    double x = 0.0;
    double y = 0.0;
    for (std::size_t index = 0U; index < ring.size(); ++index) {
        const auto& a = ring[index];
        const auto& b = ring[(index + 1U) % ring.size()];
        const double cross = a.x * b.y - b.x * a.y;
        x += (a.x + b.x) * cross;
        y += (a.y + b.y) * cross;
    }
    return geometry::PlanarPoint{x / (3.0 * area2), y / (3.0 * area2)};
}

[[nodiscard]] bool rect_inside_circle(
    const QRectF& rect,
    const QPointF& center,
    const double radius
) noexcept {
    if (radius <= 0.0) return false;
    const double radius2 = radius * radius;
    const auto inside = [&](const QPointF& point) noexcept {
        const double dx = point.x() - center.x();
        const double dy = point.y() - center.y();
        return dx * dx + dy * dy <= radius2;
    };
    return inside(rect.topLeft()) &&
        inside(rect.topRight()) &&
        inside(rect.bottomLeft()) &&
        inside(rect.bottomRight());
}

void draw_country_labels(
    QPainter& painter,
    const view::SceneGeometry& scene,
    const source::Result& source_result
) {
    if (!source_result.feature_properties_complete) return;

    struct Candidate final {
        QString text;
        QPointF point;
        QRectF collision;
        double score{0.0};
    };

    const QTransform world = painter.worldTransform();
    const double device_area_scale = std::abs(world.determinant());
    const bool globe_labels =
        scene.mode == view::SurfaceMode::globe && scene.globe_radius_m > 0.0;
    QPointF globe_center{};
    double globe_radius_px = 0.0;
    if (globe_labels) {
        globe_center = world.map(QPointF(0.0, 0.0));
        const QPointF edge = world.map(QPointF(scene.globe_radius_m, 0.0));
        globe_radius_px = std::hypot(
            edge.x() - globe_center.x(),
            edge.y() - globe_center.y()
        );
    }

    QFont font = painter.font();
    font.setPixelSize(11);
    font.setWeight(QFont::DemiBold);
    const QFontMetricsF metrics(font);
    std::vector<Candidate> candidates;

    for (const view::SceneFeatureGeometry& geometry_feature : scene.features) {
        const source::Feature* source_feature =
            find_source_feature(source_result, geometry_feature.stable_id);
        if (source_feature == nullptr) continue;
        const auto name = text_property(*source_feature, "name");
        if (!name || name->empty()) continue;

        const std::vector<geometry::PlanarPoint>* largest = nullptr;
        double largest_area = 0.0;
        for (const auto& ring : geometry_feature.fill_rings) {
            const double area = std::abs(ring_area2(ring));
            if (area > largest_area) {
                largest_area = area;
                largest = &ring;
            }
        }
        if (largest == nullptr) continue;

        const auto anchor = ring_anchor(*largest);
        if (!anchor) continue;
        const QPointF device = world.map(QPointF(anchor->x, anchor->y));
        const QString label = QString::fromStdString(*name);
        const QRectF text_rect = metrics.boundingRect(label);
        const double text_area = std::max(1.0, text_rect.width() * text_rect.height());
        const double projected_area = largest_area * device_area_scale;
        if (projected_area < text_area * 1.6) continue;

        QRectF collision(
            device.x() - text_rect.width() * 0.5 - 6.0,
            device.y() - text_rect.height() * 0.5 - 4.0,
            text_rect.width() + 12.0,
            text_rect.height() + 8.0
        );
        if (!painter.viewport().intersects(collision.toRect())) continue;
        if (globe_labels &&
            !rect_inside_circle(collision, globe_center, globe_radius_px - 3.0)) {
            continue;
        }
        candidates.push_back({label, device, collision, projected_area});
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.score > right.score;
        }
    );

    painter.save();
    painter.resetTransform();
    if (globe_labels && globe_radius_px > 1.0) {
        QPainterPath globe_clip;
        globe_clip.addEllipse(
            globe_center,
            globe_radius_px - 1.0,
            globe_radius_px - 1.0
        );
        painter.setClipPath(globe_clip, Qt::IntersectClip);
    }
    painter.setFont(font);
    painter.setPen(QColor(232, 233, 229));
    std::vector<QRectF> occupied;
    occupied.reserve(48U);
    for (const Candidate& candidate : candidates) {
        bool collides = false;
        for (const QRectF& used : occupied) {
            if (used.intersects(candidate.collision)) {
                collides = true;
                break;
            }
        }
        if (collides) continue;
        painter.drawText(
            candidate.collision,
            Qt::AlignCenter,
            candidate.text
        );
        occupied.push_back(candidate.collision);
        if (occupied.size() >= 48U) break;
    }
    painter.restore();
}

void draw_surface_classification(
    QPainter& painter,
    const view::SceneGeometry& scene,
    const source::Result& source_result
) {
    if (!source_result.feature_properties_complete) return;

    painter.save();
    painter.setPen(Qt::NoPen);
    for (const view::SceneFeatureGeometry& geometry_feature : scene.features) {
        const source::Feature* source_feature =
            find_source_feature(source_result, geometry_feature.stable_id);
        if (source_feature == nullptr) continue;
        const auto semantic_class = surface_class_property(*source_feature);
        if (!semantic_class.has_value()) continue;
        const auto color = surface_class_color(*semantic_class);
        if (!color.has_value()) continue;

        const QPainterPath path = fill_path(geometry_feature);
        if (path.isEmpty()) continue;
        painter.setBrush(*color);
        painter.drawPath(path);
    }
    painter.restore();
}

void draw_layer_geometry(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const view::SceneGeometry& scene,
    const source::Result& source_result
) {
    if (!layer.visible) return;

    const std::string_view role(layer.role_id);
    if (role == storage::kLayerRoleCountryLabelV1) {
        draw_country_labels(painter, scene, source_result);
        return;
    }
    if (role == storage::kLayerRolePhysicalSurfaceClassificationV1) {
        draw_surface_classification(painter, scene, source_result);
        return;
    }

    const bool fill_land = role == storage::kLayerRolePhysicalLandFillV1;
    const bool draw_coast = role == storage::kLayerRolePhysicalCoastlineV1;
    const bool fill_country = role == storage::kLayerRolePoliticalCountryFillV1;
    const bool draw_border = role == storage::kLayerRolePoliticalBoundaryV1;
    if (!fill_land && !draw_coast && !fill_country && !draw_border) return;

    if (fill_land) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(190, 194, 181));
        for (const view::SceneFeatureGeometry& feature : scene.features) {
            const QPainterPath path = fill_path(feature);
            if (!path.isEmpty()) painter.drawPath(path);
        }
    }

    if (fill_country) {
        painter.setPen(Qt::NoPen);
        for (const view::SceneFeatureGeometry& feature : scene.features) {
            const source::Feature* source_feature =
                find_source_feature(source_result, feature.stable_id);
            painter.setBrush(political_country_color(source_feature));
            const QPainterPath path = fill_path(feature);
            if (!path.isEmpty()) painter.drawPath(path);
        }
    }

    if (draw_coast || draw_border) {
        QPen pen(draw_border ? QColor(224, 222, 214) : QColor(145, 151, 147));
        pen.setCosmetic(true);
        pen.setWidthF(draw_border ? 0.85 : 1.05);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (const view::SceneFeatureGeometry& feature : scene.features) {
            for (const auto& part : feature.outlines) {
                if (part.size() < 2U) continue;
                QPainterPath path;
                path.moveTo(part.front().x, part.front().y);
                for (std::size_t index = 1U; index < part.size(); ++index) {
                    path.lineTo(part[index].x, part[index].y);
                }
                painter.drawPath(path);
            }
        }
    }
}

}  // namespace

MapView::MapView(QWidget* parent)
    : QWidget(parent),
      elevation_detail_loader_(this) {
    elevation_detail_loader_.set_result_callback(
        [this](
            std::filesystem::path project_path,
            std::vector<ElevationDetailLoadResult> results
        ) {
            accept_elevation_detail_results(
                std::move(project_path),
                std::move(results)
            );
        }
    );
    terrain_refine_timer_ = new QTimer(this);
    terrain_refine_timer_->setSingleShot(true);
    terrain_refine_timer_->setInterval(kTerrainInteractionRefineDelayMs);
    connect(
        terrain_refine_timer_,
        &QTimer::timeout,
        this,
        [this]() { end_interactive_terrain(); }
    );
    setMinimumSize(720, 480);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

std::size_t MapView::viewport_index(const view::SurfaceMode mode) noexcept {
    switch (mode) {
    case view::SurfaceMode::globe:
        return 0U;
    case view::SurfaceMode::sinusoidal:
        return 1U;
    case view::SurfaceMode::mollweide:
        return 2U;
    case view::SurfaceMode::sinu_mollweide:
        return 3U;
    }
    return 0U;
}

void MapView::store_active_viewport() noexcept {
    viewports_[viewport_index(mode_)] = {zoom_, viewport_pan_};
}

void MapView::restore_active_viewport() noexcept {
    const ViewportState& state = viewports_[viewport_index(mode_)];
    zoom_ = state.zoom;
    viewport_pan_ = state.pan;
}

void MapView::set_project(
    std::shared_ptr<const ProjectModel> model,
    std::string project_uuid,
    const std::uint64_t revision
) {
    if (terrain_refine_timer_ != nullptr) terrain_refine_timer_->stop();
    terrain_interaction_active_ = false;
    elevation_detail_loader_.cancel();
    model_ = std::move(model);
    project_uuid_ = std::move(project_uuid);
    revision_ = revision;
    has_frame_ = false;
    frame_error_.clear();
    elevation_surface_cache_ = {};
    mode_ = view::SurfaceMode::globe;
    longitude_deg_ = 15.0;
    latitude_deg_ = 20.0;
    projection_central_meridian_deg_ = 0.0;
    viewports_ = {};
    restore_active_viewport();
    update();
    request_scene(view::SceneQuality::verified);
}

void MapView::set_project_model(
    std::shared_ptr<const ProjectModel> model,
    const std::uint64_t revision
) {
    if (terrain_refine_timer_ != nullptr) terrain_refine_timer_->stop();
    terrain_interaction_active_ = false;
    elevation_detail_loader_.cancel();
    model_ = std::move(model);
    revision_ = revision;
    has_frame_ = false;
    frame_error_.clear();
    elevation_surface_cache_ = {};
    update();
    request_scene(view::SceneQuality::verified);
}

void MapView::clear_project() {
    if (terrain_refine_timer_ != nullptr) terrain_refine_timer_->stop();
    terrain_interaction_active_ = false;
    elevation_detail_loader_.cancel();
    model_.reset();
    project_uuid_.clear();
    revision_ = 0U;
    has_frame_ = false;
    frame_ = {};
    frame_error_.clear();
    elevation_surface_cache_ = {};
    busy_ = false;
    mode_ = view::SurfaceMode::globe;
    projection_central_meridian_deg_ = 0.0;
    viewports_ = {};
    restore_active_viewport();
    update();
}

void MapView::set_scene_request_callback(SceneRequestCallback callback) {
    scene_request_callback_ = std::move(callback);
}

void MapView::set_surface_probe_callback(SurfaceProbeCallback callback) {
    surface_probe_callback_ = std::move(callback);
}

void MapView::set_frame(RenderFrame frame) {
    if (!frame.ok) {
        frame_error_ = std::move(frame.diagnostic);
        update();
        return;
    }
    frame_error_.clear();
    frame_ = std::move(frame);
    has_frame_ = true;
    update();
}

void MapView::set_busy(const bool busy) {
    busy_ = busy;
    update();
}

void MapView::set_surface_mode(const view::SurfaceMode mode) {
    if (mode_ == mode && has_frame_ && frame_.request.mode == mode) return;
    store_active_viewport();
    mode_ = mode;
    restore_active_viewport();
    request_scene(view::SceneQuality::verified);
    update();
}

void MapView::begin_interactive_terrain() {
    if (!model_ || model_->sources.empty()) return;
    terrain_interaction_active_ = true;
    if (terrain_refine_timer_ != nullptr) {
        terrain_refine_timer_->start(kTerrainInteractionRefineDelayMs);
    }
}

void MapView::end_interactive_terrain() {
    if (terrain_refine_timer_ != nullptr) terrain_refine_timer_->stop();
    if (!terrain_interaction_active_) return;
    terrain_interaction_active_ = false;

    // Keep decoded detail tiles; only the projection raster is preview-quality.
    // Clearing it forces one final-quality rebuild after interaction settles.
    elevation_surface_cache_.image = {};
    update();
}

void MapView::apply_zoom(const double factor, const QPointF& anchor) {
    if (!model_ || model_->sources.empty() ||
        !std::isfinite(factor) || factor <= 0.0) {
        return;
    }

    const double old_zoom = zoom_;
    const double new_zoom = std::clamp(
        old_zoom * factor,
        kMinimumZoom,
        kMaximumZoom
    );
    if (new_zoom == old_zoom) return;

    begin_interactive_terrain();
    const QPointF center(
        static_cast<double>(width()) * 0.5,
        static_cast<double>(height()) * 0.5
    );
    const double applied = new_zoom / old_zoom;
    viewport_pan_ = anchor - center - applied * (anchor - center - viewport_pan_);
    zoom_ = new_zoom;
    update();
}

void MapView::zoom_in() {
    apply_zoom(
        kKeyboardZoomFactor,
        QPointF(static_cast<double>(width()) * 0.5, static_cast<double>(height()) * 0.5)
    );
}

void MapView::zoom_out() {
    apply_zoom(
        1.0 / kKeyboardZoomFactor,
        QPointF(static_cast<double>(width()) * 0.5, static_cast<double>(height()) * 0.5)
    );
}

void MapView::reset_viewport() {
    if (!model_ || model_->sources.empty()) return;
    end_interactive_terrain();
    zoom_ = 1.0;
    viewport_pan_ = {};
    store_active_viewport();
    update();
}

std::optional<SurfaceProbeResult> MapView::surface_probe_at(
    const QPointF device_position
) const {
    if (!model_ || !has_current_frame() || model_->sources.empty()) {
        return std::nullopt;
    }

    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
    if (!combined_bounds(frame_, min_x, min_y, max_x, max_y)) {
        return std::nullopt;
    }

    const double available_width = std::max(1, width() - 2 * kMapMarginPx);
    const double available_height = std::max(1, height() - 2 * kMapMarginPx);
    const double span_x = max_x - min_x;
    const double span_y = max_y - min_y;
    const double base_scale = std::min(
        available_width / span_x,
        available_height / span_y
    );
    const double center_x = 0.5 * (min_x + max_x);
    const double center_y = 0.5 * (min_y + max_y);

    QTransform surface_to_device;
    surface_to_device.translate(
        static_cast<double>(width()) * 0.5 + viewport_pan_.x(),
        static_cast<double>(height()) * 0.5 + viewport_pan_.y()
    );
    surface_to_device.scale(base_scale * zoom_, -base_scale * zoom_);
    surface_to_device.translate(-center_x, -center_y);
    bool invertible = false;
    const QTransform device_to_surface = surface_to_device.inverted(&invertible);
    if (!invertible) return std::nullopt;

    const QPointF surface_point = device_to_surface.map(device_position);
    const view::SurfaceGeographicPickResult geographic =
        view::pick_geographic_from_surface(
            frame_.request.mode,
            {surface_point.x(), surface_point.y()},
            frame_.request.camera_longitude_deg,
            frame_.request.camera_latitude_deg,
            frame_.request.projection_central_meridian_deg
        );
    if (!geographic.ok) return std::nullopt;

    SurfaceProbeResult result{};
    result.longitude_deg = geographic.longitude_deg;
    result.latitude_deg = geographic.latitude_deg;
    result.presentation_material = "water/background";

    const auto assign_source = [&](const std::string& source_id) {
        result.material_source_id = source_id;
        const auto source_it = model_->sources.find(source_id);
        if (source_it == model_->sources.end() || !source_it->second) return;
        const source::Provenance& provenance = source_it->second->provenance;
        result.material_provider = provenance.provider;
        result.material_dataset = provenance.dataset;
        result.material_snapshot = provenance.snapshot;
        result.material_version = provenance.dataset_version;
    };

    // Project layer order is top-to-bottom while paintEvent renders it in
    // reverse. Walk top-to-bottom here and stop at the first material layer
    // that actually covers the picked point, so the diagnostic reports the
    // material visible after composition rather than a preferred role type.
    bool material_resolved = false;
    for (const storage::ProjectLayerRecord& layer : model_->layers) {
        if (!layer.visible) continue;

        if (layer.role_id == storage::kLayerRolePhysicalSurfaceClassificationV1) {
            for (const storage::LayerSourceBinding& binding : layer.sources) {
                if (binding.slot_id != "classification") continue;
                const auto scene_it = frame_.source_scenes.find(binding.source_id);
                const auto source_it = model_->sources.find(binding.source_id);
                if (scene_it == frame_.source_scenes.end() ||
                    source_it == model_->sources.end() || !source_it->second) {
                    continue;
                }

                std::optional<surface::SurfaceClass> visible_class;
                for (const view::SceneFeatureGeometry& geometry_feature :
                     scene_it->second.features) {
                    const QPainterPath path = fill_path(geometry_feature);
                    if (path.isEmpty() || !path.contains(surface_point)) continue;
                    const source::Feature* feature = find_source_feature(
                        *source_it->second,
                        geometry_feature.stable_id
                    );
                    if (feature == nullptr) continue;
                    const auto value = surface_class_property(*feature);
                    if (!value.has_value() ||
                        *value == surface::SurfaceClass::unknown) {
                        continue;
                    }
                    // Later features in the same painter pass win if source
                    // geometry overlaps, matching draw_surface_classification.
                    visible_class = value;
                }
                if (!visible_class.has_value()) continue;

                result.canonical_surface_class_id =
                    std::string(surface::surface_class_id(*visible_class));
                result.presentation_material = result.canonical_surface_class_id;
                assign_source(binding.source_id);
                material_resolved = true;
                break;
            }
        } else if (layer.role_id == storage::kLayerRolePhysicalLandFillV1) {
            for (const storage::LayerSourceBinding& binding : layer.sources) {
                if (binding.slot_id != "geometry") continue;
                const auto scene_it = frame_.source_scenes.find(binding.source_id);
                if (scene_it == frame_.source_scenes.end()) continue;
                const bool contains = std::any_of(
                    scene_it->second.features.begin(),
                    scene_it->second.features.end(),
                    [&](const view::SceneFeatureGeometry& feature) {
                        const QPainterPath path = fill_path(feature);
                        return !path.isEmpty() && path.contains(surface_point);
                    }
                );
                if (!contains) continue;
                result.presentation_material = "land";
                assign_source(binding.source_id);
                material_resolved = true;
                break;
            }
        }

        if (material_resolved) break;
    }

    for (const storage::ProjectLayerRecord& layer : model_->layers) {
        if (!layer.visible ||
            layer.role_id != storage::kLayerRolePhysicalElevationV1) {
            continue;
        }
        const ElevationProbeSample elevation = probe_elevation_at_geographic(
            layer,
            *model_,
            elevation_surface_cache_,
            geographic.longitude_deg,
            geographic.latitude_deg
        );
        if (!elevation.overview_m.has_value() &&
            !elevation.detail_m.has_value()) {
            continue;
        }
        result.overview_elevation_m = elevation.overview_m;
        result.detail_elevation_m = elevation.detail_m;
        result.detail_resource_id = elevation.detail_resource_id;
        result.elevation_layer_id = layer.layer_id;
        result.elevation_layer_name = layer.name;

        for (const storage::LayerResourceBinding& binding : layer.resources) {
            if (binding.slot_id != "provenance") continue;
            const auto resource_it = model_->resources.find(binding.resource_id);
            if (resource_it == model_->resources.end() || !resource_it->second) {
                break;
            }
            const EmbeddedProjectResource& provenance = *resource_it->second;
            const auto assign = [&](const std::string_view key, std::string& target) {
                if (const auto value = manifest_field(provenance, key);
                    value.has_value()) {
                    target = *value;
                }
            };
            assign("provider", result.elevation_provider);
            assign("dataset", result.elevation_dataset);
            assign("version", result.elevation_version);
            assign("variant", result.elevation_variant);
            assign("source_uri", result.elevation_source_uri);
            assign("source_sha256", result.elevation_source_sha256);
            break;
        }
        break;
    }

    return result;
}

void MapView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(18, 21, 24));

    const QRect content = rect().adjusted(48, 48, -48, -48);
    if (!model_) {
        painter.setPen(QColor(224, 227, 231));
        painter.drawText(
            content,
            Qt::AlignCenter,
            QStringLiteral("Create or open an .aeris project to begin")
        );
        return;
    }
    if (model_->sources.empty()) {
        painter.setPen(QColor(224, 227, 231));
        painter.drawText(
            content,
            Qt::AlignCenter | Qt::TextWordWrap,
            QStringLiteral(
                "Empty AERIS project\n\n"
                "The durable project is ready. Install the verified base world from:\n"
                "Data → Install / repair base political world\n\n"
                "A local Natural Earth snapshot is available only as an advanced/offline fallback."
            )
        );
        return;
    }

    const bool frame_matches_mode = has_frame_ && frame_.request.mode == mode_;
    if (!frame_matches_mode) {
        painter.setPen(QColor(190, 195, 202));
        painter.drawText(content, Qt::AlignCenter, QStringLiteral("Building verified map geometry…"));
    } else {
        double min_x = 0.0;
        double min_y = 0.0;
        double max_x = 0.0;
        double max_y = 0.0;
        if (combined_bounds(frame_, min_x, min_y, max_x, max_y)) {
            const double available_width = std::max(1, width() - 2 * kMapMarginPx);
            const double available_height = std::max(1, height() - 2 * kMapMarginPx);
            const double span_x = max_x - min_x;
            const double span_y = max_y - min_y;
            const double base_scale = std::min(
                available_width / span_x,
                available_height / span_y
            );
            const double center_x = 0.5 * (min_x + max_x);
            const double center_y = 0.5 * (min_y + max_y);

            QTransform transform;
            transform.translate(
                static_cast<double>(width()) * 0.5 + viewport_pan_.x(),
                static_cast<double>(height()) * 0.5 + viewport_pan_.y()
            );
            transform.scale(base_scale * zoom_, -base_scale * zoom_);
            transform.translate(-center_x, -center_y);
            painter.setWorldTransform(transform);

            if (mode_ == view::SurfaceMode::globe) {
                double radius = 0.0;
                if (!frame_.source_scenes.empty()) {
                    radius = frame_.source_scenes.begin()->second.globe_radius_m;
                }
                if (radius > 0.0) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(38, 46, 54));
                    painter.drawEllipse(QPointF(0.0, 0.0), radius, radius);
                }
            } else {
                const view::PlanarSurfaceGeometry surface =
                    view::build_planar_surface_geometry(mode_);
                const QPainterPath sheet = surface_path(surface);
                if (!sheet.isEmpty()) {
                    QPen edge(QColor(82, 91, 100));
                    edge.setCosmetic(true);
                    edge.setWidthF(1.0);
                    painter.setPen(edge);
                    painter.setBrush(QColor(38, 46, 54));
                    painter.drawPath(sheet);
                }
            }

            for (auto layer_it = model_->layers.rbegin();
                 layer_it != model_->layers.rend(); ++layer_it) {
                const storage::ProjectLayerRecord& layer = *layer_it;
                if (!layer.visible) continue;

                // Numerical elevation participates in the same durable layer
                // stack as vector content. This call is pure presentation: it
                // samples overview/already-delivered detail and only records
                // missing detail ids. Storage I/O is dispatched after painting.
                draw_elevation_overview(
                    painter,
                    layer,
                    frame_,
                    *model_,
                    zoom_,
                    viewport_pan_,
                    terrain_interaction_active_,
                    elevation_surface_cache_
                );

                for (const storage::LayerSourceBinding& binding : layer.sources) {
                    if (layer.role_id == storage::kLayerRolePhysicalSurfaceClassificationV1 &&
                        binding.slot_id != "classification") {
                        continue;
                    }
                    const auto scene_it = frame_.source_scenes.find(binding.source_id);
                    const auto source_it = model_->sources.find(binding.source_id);
                    if (scene_it == frame_.source_scenes.end() ||
                        source_it == model_->sources.end()) {
                        continue;
                    }
                    draw_layer_geometry(
                        painter,
                        layer,
                        scene_it->second,
                        *source_it->second
                    );
                }
            }
            dispatch_elevation_detail_requests();
        }
    }

    painter.resetTransform();
    if (busy_) {
        painter.setPen(QColor(183, 188, 195));
        painter.drawText(
            QRect(22, 18, width() - 44, 28),
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("verifying geometry…")
        );
    }

    if (!frame_error_.empty()) {
        painter.setPen(QColor(232, 105, 105));
        painter.drawText(
            QRect(22, height() - 74, width() - 44, 54),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
            QString::fromStdString(frame_error_)
        );
    } else {
        painter.setPen(QColor(125, 132, 141));
        painter.drawText(
            QRect(22, height() - 42, width() - 44, 24),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("%1 · revision %2 · %3 · %4×")
                .arg(QString::fromStdString(project_uuid_))
                .arg(static_cast<qulonglong>(revision_))
                .arg(QString::fromLatin1(view::surface_mode_name(mode_)))
                .arg(zoom_, 0, 'f', 2)
        );
    }
}

void MapView::dispatch_elevation_detail_requests() {
    const auto& resource_ids = elevation_detail_requests(elevation_surface_cache_);
    if (!model_ || resource_ids.empty()) {
        if (elevation_detail_loader_.busy()) elevation_detail_loader_.cancel();
        return;
    }
    elevation_detail_loader_.request(model_->project_path, resource_ids);
}

void MapView::accept_elevation_detail_results(
    std::filesystem::path project_path,
    std::vector<ElevationDetailLoadResult> results
) {
    if (!model_ || model_->project_path != project_path) return;

    bool changed = false;
    for (ElevationDetailLoadResult& result : results) {
        if (result.ok()) {
            changed = accept_elevation_detail_tile(
                elevation_surface_cache_,
                std::move(result.resource_id),
                std::move(*result.tile)
            ) || changed;
        } else {
            reject_elevation_detail_resource(
                elevation_surface_cache_,
                result.resource_id
            );
        }
    }
    if (changed) update();
}

void MapView::wheelEvent(QWheelEvent* event) {
    if (!model_ || model_->sources.empty()) {
        QWidget::wheelEvent(event);
        return;
    }

    double steps = 0.0;
    if (!event->pixelDelta().isNull()) {
        steps = static_cast<double>(event->pixelDelta().y()) / kTrackpadPixelsPerStep;
    } else if (!event->angleDelta().isNull()) {
        steps = static_cast<double>(event->angleDelta().y()) / 120.0;
    }
    if (std::abs(steps) <= 1e-12) {
        event->accept();
        return;
    }

    apply_zoom(std::pow(kWheelZoomBase, steps), event->position());
    event->accept();
}

void MapView::keyPressEvent(QKeyEvent* event) {
    if (!model_ || model_->sources.empty()) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->matches(QKeySequence::ZoomIn)) {
        zoom_in();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::ZoomOut)) {
        zoom_out();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Home ||
        (event->key() == Qt::Key_0 && event->modifiers().testFlag(Qt::ControlModifier))) {
        reset_viewport();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void MapView::mousePressEvent(QMouseEvent* event) {
    if (!model_ || model_->sources.empty() || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    drag_moved_ = false;
    last_mouse_ = event->pos();
    press_mouse_ = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void MapView::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (!drag_moved_) {
        if ((event->pos() - press_mouse_).manhattanLength() <= 4) {
            event->accept();
            return;
        }
        drag_moved_ = true;
    }

    const QPoint delta = event->pos() - last_mouse_;
    last_mouse_ = event->pos();
    begin_interactive_terrain();
    if (mode_ == view::SurfaceMode::globe) {
        const double sensitivity = 0.32 / std::sqrt(std::max(zoom_, 1.0));
        longitude_deg_ = wrap_longitude(
            longitude_deg_ - static_cast<double>(delta.x()) * sensitivity
        );
        latitude_deg_ = std::clamp(
            latitude_deg_ + static_cast<double>(delta.y()) * sensitivity,
            -89.5,
            89.5
        );
        request_scene(view::SceneQuality::preview);
    } else {
        viewport_pan_ += QPointF(delta);
        update();
    }
    event->accept();
}

void MapView::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool moved = drag_moved_;
    const bool probe_requested =
        !moved && event->modifiers().testFlag(Qt::ShiftModifier);

    dragging_ = false;
    drag_moved_ = false;
    unsetCursor();
    end_interactive_terrain();
    if (mode_ == view::SurfaceMode::globe && moved) {
        request_scene(view::SceneQuality::verified);
    }
    if (probe_requested && surface_probe_callback_) {
        const auto probe = surface_probe_at(event->position());
        if (probe.has_value()) surface_probe_callback_(*probe);
    }
    event->accept();
}

void MapView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!model_ || model_->sources.empty() || event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    apply_zoom(kDoubleClickZoomFactor, event->position());
    event->accept();
}

void MapView::request_scene(const view::SceneQuality quality) {
    if (!model_ || model_->sources.empty() || !scene_request_callback_) return;
    view::SceneRequest request{};
    request.mode = mode_;
    request.quality = quality;
    request.camera_longitude_deg = longitude_deg_;
    request.camera_latitude_deg = latitude_deg_;
    request.projection_central_meridian_deg = projection_central_meridian_deg_;
    scene_request_callback_(request);
}

}  // namespace aeris::desktop
