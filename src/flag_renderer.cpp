// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "flag_renderer.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aeris::desktop {
namespace {

[[nodiscard]] std::string ascii_lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
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
    return inside(rect.topLeft()) && inside(rect.topRight()) &&
        inside(rect.bottomLeft()) && inside(rect.bottomRight());
}

struct FlagCandidate final {
    const QImage* image{nullptr};
    QRectF rect;
    double score{0.0};
};

}  // namespace

void draw_country_flags(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const view::SceneGeometry& scene,
    const source::Result& source_result,
    const ProjectModel& model
) {
    if (!layer.visible || !source_result.feature_properties_complete) return;

    std::unordered_map<std::string, const EmbeddedProjectResource*> flag_resources;
    flag_resources.reserve(layer.resources.size());
    for (const storage::LayerResourceBinding& binding : layer.resources) {
        constexpr std::string_view prefix = "flag:";
        if (binding.slot_id.size() <= prefix.size() ||
            binding.slot_id.compare(0U, prefix.size(), prefix) != 0) {
            continue;
        }
        const auto resource = model.resources.find(binding.resource_id);
        if (resource == model.resources.end() || !resource->second ||
            resource->second->media_type != "image/png" ||
            resource->second->raster_image.isNull()) {
            continue;
        }
        flag_resources.emplace(
            binding.slot_id.substr(prefix.size()),
            resource->second.get()
        );
    }
    if (flag_resources.empty()) return;

    std::unordered_map<std::string, const source::Feature*> source_features;
    source_features.reserve(source_result.features.size());
    for (const source::Feature& feature : source_result.features) {
        source_features.emplace(feature.stable_id, &feature);
    }

    const QTransform world = painter.worldTransform();
    const double device_area_scale = std::abs(world.determinant());
    const bool globe = scene.mode == view::SurfaceMode::globe && scene.globe_radius_m > 0.0;
    QPointF globe_center{};
    double globe_radius_px = 0.0;
    if (globe) {
        globe_center = world.map(QPointF(0.0, 0.0));
        const QPointF edge = world.map(QPointF(scene.globe_radius_m, 0.0));
        globe_radius_px = std::hypot(
            edge.x() - globe_center.x(),
            edge.y() - globe_center.y()
        );
    }

    std::vector<FlagCandidate> candidates;
    candidates.reserve(scene.features.size());
    for (const view::SceneFeatureGeometry& geometry_feature : scene.features) {
        const auto source_found = source_features.find(geometry_feature.stable_id);
        if (source_found == source_features.end()) continue;
        const auto iso = text_property(*source_found->second, "iso_a2");
        if (!iso.has_value() || iso->size() != 2U) continue;
        const std::string code = ascii_lower(*iso);
        const auto flag = flag_resources.find(code);
        if (flag == flag_resources.end()) continue;

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
        if (!anchor.has_value()) continue;

        const double projected_area = largest_area * device_area_scale;
        // This is intentionally screen-space detail selection. At whole-world
        // scale only large countries get a symbol; zooming naturally admits
        // smaller countries without requesting new geographic geometry.
        if (projected_area < 700.0) continue;

        const QImage& image = flag->second->raster_image;
        if (image.width() <= 0 || image.height() <= 0) continue;
        const double aspect = static_cast<double>(image.width()) /
            static_cast<double>(image.height());
        double width = 24.0;
        double height = width / aspect;
        if (height > 16.0) {
            height = 16.0;
            width = height * aspect;
        }
        width = std::max(width, 10.0);
        height = std::max(height, 8.0);

        const QPointF device = world.map(QPointF(anchor->x, anchor->y));
        // Country labels use the same geographic anchor in the base renderer.
        // Keep the flag as a companion symbol above that anchor rather than
        // covering the label text while both systems still have independent
        // collision passes.
        const double vertical_offset = 10.0 + height * 0.5;
        const QRectF rect(
            device.x() - width * 0.5,
            device.y() - vertical_offset - height * 0.5,
            width,
            height
        );
        if (!painter.viewport().intersects(rect.toAlignedRect())) continue;
        if (globe && !rect_inside_circle(rect, globe_center, globe_radius_px - 2.0)) continue;
        candidates.push_back({&image, rect, projected_area});
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const FlagCandidate& left, const FlagCandidate& right) {
            return left.score > right.score;
        }
    );

    painter.save();
    painter.resetTransform();
    if (globe && globe_radius_px > 1.0) {
        QPainterPath clip;
        clip.addEllipse(globe_center, globe_radius_px - 1.0, globe_radius_px - 1.0);
        painter.setClipPath(clip, Qt::IntersectClip);
    }

    std::vector<QRectF> occupied;
    occupied.reserve(80U);
    for (const FlagCandidate& candidate : candidates) {
        const QRectF collision = candidate.rect.adjusted(-3.0, -3.0, 3.0, 3.0);
        bool collides = false;
        for (const QRectF& used : occupied) {
            if (used.intersects(collision)) {
                collides = true;
                break;
            }
        }
        if (collides) continue;

        painter.setPen(QPen(QColor(235, 237, 239, 190), 1.0));
        painter.setBrush(QColor(18, 21, 24, 180));
        painter.drawRoundedRect(candidate.rect.adjusted(-1.5, -1.5, 1.5, 1.5), 1.5, 1.5);
        painter.drawImage(candidate.rect, *candidate.image);
        occupied.push_back(collision);
        if (occupied.size() >= 80U) break;
    }
    painter.restore();
}

}  // namespace aeris::desktop
