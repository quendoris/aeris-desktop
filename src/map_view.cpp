// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_view.hpp"

#include "aeris/storage/layer.hpp"

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

[[nodiscard]] bool combined_bounds(
    const RenderFrame& frame,
    double& min_x,
    double& min_y,
    double& max_x,
    double& max_y
) {
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

    const bool fill_land = role == storage::kLayerRolePhysicalLandFillV1;
    const bool draw_coast = role == storage::kLayerRolePhysicalCoastlineV1;
    const bool fill_country = role == storage::kLayerRolePoliticalCountryFillV1;
    const bool draw_border = role == storage::kLayerRolePoliticalBoundaryV1;
    if (!fill_land && !draw_coast && !fill_country && !draw_border) return;

    if (fill_land || fill_country) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(
            fill_land
                ? QColor(190, 194, 181)
                : QColor(123, 143, 157, 82)
        );
        for (const view::SceneFeatureGeometry& feature : scene.features) {
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
    : QWidget(parent) {
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
    model_ = std::move(model);
    project_uuid_ = std::move(project_uuid);
    revision_ = revision;
    has_frame_ = false;
    frame_error_.clear();
    mode_ = view::SurfaceMode::globe;
    longitude_deg_ = 15.0;
    latitude_deg_ = 20.0;
    projection_central_meridian_deg_ = 0.0;
    viewports_ = {};
    restore_active_viewport();
    update();
    request_scene(view::SceneQuality::verified);
}

void MapView::clear_project() {
    model_.reset();
    project_uuid_.clear();
    revision_ = 0U;
    has_frame_ = false;
    frame_ = {};
    frame_error_.clear();
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

void MapView::apply_zoom(const double factor, const QPointF& anchor) {
    if (!model_ || !std::isfinite(factor) || factor <= 0.0) return;

    const double old_zoom = zoom_;
    const double new_zoom = std::clamp(
        old_zoom * factor,
        kMinimumZoom,
        kMaximumZoom
    );
    if (new_zoom == old_zoom) return;

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
    if (!model_) return;
    zoom_ = 1.0;
    viewport_pan_ = {};
    store_active_viewport();
    update();
}

void MapView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(18, 21, 24));

    const QRect content = rect().adjusted(48, 48, -48, -48);
    if (!model_) {
        painter.setPen(QColor(224, 227, 231));
        painter.drawText(content, Qt::AlignCenter, QStringLiteral("Open an .aeris project to begin"));
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
            }

            for (auto layer_it = model_->layers.rbegin();
                 layer_it != model_->layers.rend(); ++layer_it) {
                const storage::ProjectLayerRecord& layer = *layer_it;
                if (!layer.visible) continue;
                for (const storage::LayerSourceBinding& binding : layer.sources) {
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

void MapView::wheelEvent(QWheelEvent* event) {
    if (!model_) {
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
    if (!model_) {
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
    if (!model_ || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    last_mouse_ = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void MapView::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint delta = event->pos() - last_mouse_;
    last_mouse_ = event->pos();
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
    dragging_ = false;
    unsetCursor();
    if (mode_ == view::SurfaceMode::globe) {
        request_scene(view::SceneQuality::verified);
    }
    event->accept();
}

void MapView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!model_ || event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    apply_zoom(kDoubleClickZoomFactor, event->position());
    event->accept();
}

void MapView::request_scene(const view::SceneQuality quality) {
    if (!model_ || !scene_request_callback_) return;
    view::SceneRequest request{};
    request.mode = mode_;
    request.quality = quality;
    request.camera_longitude_deg = longitude_deg_;
    request.camera_latitude_deg = latitude_deg_;
    request.projection_central_meridian_deg = projection_central_meridian_deg_;
    scene_request_callback_(request);
}

}  // namespace aeris::desktop
