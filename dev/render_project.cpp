// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_view.hpp"
#include "project_model.hpp"
#include "scene_controller.hpp"

#include "aeris/projection/ring.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/view/scene.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QWheelEvent>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kPoliticalSourceId = "world.admin0.natural-earth-110m";

[[nodiscard]] QImage render_view(aeris::desktop::MapView& view) {
    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter);
    painter.end();
    return image;
}

[[nodiscard]] bool verify_flat_projection(
    const aeris::desktop::ProjectModel& model,
    const aeris::projection::EqualAreaPrimitive primitive,
    const std::string_view label
) {
    aeris::projection::RingProjectionOptions options{};
    options.primitive = primitive;
    options.central_meridian_rad = 0.0;
    options.relative_area_tolerance = 1e-7;
    options.absolute_area_tolerance_m2 = 10'000.0;
    options.initial_geometric_tolerance_m = 2'000.0;
    options.initial_local_area_tolerance_m2 = 1.0e8;
    options.max_refinement_rounds = 18U;
    options.subdivision_max_depth = 32U;
    options.subdivision_max_segments_per_edge = 1'000'000U;
    options.max_projection_pieces = 4096U;

    for (const auto& source_entry : model.sources) {
        const auto& source_id = source_entry.first;
        const auto& source = *source_entry.second;
        for (const auto& feature : source.features) {
            for (std::size_t ring_index = 0U;
                 ring_index < feature.rings.size();
                 ++ring_index) {
                const auto& ring = feature.rings[ring_index].geometry;
                const auto projected =
                    aeris::projection::project_wgs84_linear_ring_piecewise_verified(
                        ring,
                        options
                    );
                if (projected.ok()) continue;

                std::cerr
                    << label << " flat projection failed"
                    << " source=" << source_id
                    << " feature=" << feature.stable_id
                    << " ring=" << ring_index
                    << " winding=" << ring.longitude_winding
                    << " interior_side=" << static_cast<unsigned>(ring.interior_side)
                    << " error=" << static_cast<unsigned>(projected.error)
                    << " piece_error=" << static_cast<unsigned>(projected.piece_error)
                    << " seam_error=" << static_cast<unsigned>(projected.seam_error)
                    << " geographic_error=" << static_cast<unsigned>(projected.geographic_error)
                    << " subdivision_error=" << static_cast<unsigned>(projected.subdivision_error)
                    << " sample_error=" << static_cast<unsigned>(projected.sample_error)
                    << " failed_piece=" << projected.failed_piece
                    << " failed_edge=" << projected.failed_edge
                    << " source_area_m2=" << projected.source_signed_area_m2
                    << " planar_area_m2=" << projected.planar_signed_area_m2
                    << " area_error_m2=" << projected.absolute_area_error_m2
                    << " allowed_m2=" << projected.allowed_area_error_m2
                    << '\n';
                return false;
            }
        }
    }

    std::cout << label << " durable flat projection: PASS\n";
    return true;
}

[[nodiscard]] bool verify_globe_preview_fill(
    const aeris::desktop::ProjectModel& model
) {
    const auto source_it = model.sources.find(std::string(kPoliticalSourceId));
    if (source_it == model.sources.end()) {
        std::cerr << "durable political source is missing\n";
        return false;
    }

    aeris::view::SceneRequest request{};
    request.mode = aeris::view::SurfaceMode::globe;
    request.quality = aeris::view::SceneQuality::preview;
    request.camera_longitude_deg = 41.0;
    request.camera_latitude_deg = 17.0;
    const auto preview = aeris::view::build_scene_geometry(
        *source_it->second,
        request
    );
    if (!preview.ok || preview.canceled || preview.fill_rings == 0U) {
        std::cerr
            << "durable political globe preview has no fill geometry: "
            << preview.diagnostic << '\n';
        return false;
    }

    std::size_t filled_features = 0U;
    for (const auto& feature : preview.features) {
        if (!feature.fill_rings.empty()) ++filled_features;
    }
    if (filled_features == 0U) {
        std::cerr << "durable political globe preview has no filled features\n";
        return false;
    }

    std::cout
        << "durable political globe preview: PASS ("
        << filled_features << " filled features, "
        << preview.fill_rings << " fill rings)\n";
    return true;
}

[[nodiscard]] bool nearly_equal(const QPointF& left, const QPointF& right) noexcept {
    return std::hypot(left.x() - right.x(), left.y() - right.y()) <= 1e-9;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: aeris-desktop-render-probe <project.aeris> <output.png>\n";
        return EXIT_FAILURE;
    }

    QApplication application(argc, argv);
    auto opened = aeris::storage::ProjectStore::open(std::filesystem::path(argv[1]));
    if (!opened.ok()) {
        std::cerr << "project open failed: " << opened.status.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    auto model_result = aeris::desktop::load_project_model(*opened.store);
    if (!model_result.ok()) {
        std::cerr << "project model failed: " << model_result.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    if (!verify_flat_projection(
            *model_result.model,
            aeris::projection::EqualAreaPrimitive::sinusoidal,
            "Sinusoidal"
        ) ||
        !verify_flat_projection(
            *model_result.model,
            aeris::projection::EqualAreaPrimitive::mollweide,
            "Mollweide"
        ) ||
        !verify_globe_preview_fill(*model_result.model)) {
        return EXIT_FAILURE;
    }

    aeris::desktop::RenderFrame frame{};
    frame.request.mode = aeris::view::SurfaceMode::globe;
    frame.request.quality = aeris::view::SceneQuality::verified;
    frame.request.camera_longitude_deg = 15.0;
    frame.request.camera_latitude_deg = 20.0;
    for (const auto& entry : model_result.model->sources) {
        auto scene = aeris::view::build_scene_geometry(*entry.second, frame.request);
        if (!scene.ok) {
            std::cerr << "scene failed for " << entry.first << ": " << scene.diagnostic << '\n';
            return EXIT_FAILURE;
        }
        frame.source_scenes.emplace(entry.first, std::move(scene));
    }

    aeris::desktop::MapView view;
    view.resize(1280, 820);
    view.set_project(
        model_result.model,
        opened.store->metadata().project_uuid,
        opened.store->metadata().revision
    );
    view.set_frame(std::move(frame));
    view.show();
    application.processEvents();

    const QImage baseline = render_view(view);

    // Deliberately zoom away from the center. A correct viewport transform
    // must preserve the projected point below this cursor position.
    const QPointF local_position(
        static_cast<qreal>(view.width()) * 0.68,
        static_cast<qreal>(view.height()) * 0.39
    );
    const QPointF center(
        static_cast<qreal>(view.width()) * 0.5,
        static_cast<qreal>(view.height()) * 0.5
    );
    const double old_zoom = view.zoom_factor();
    const QPointF old_pan = view.viewport_pan();
    const QPointF anchor_before = (local_position - center - old_pan) / old_zoom;

    const QPoint global_point = view.mapToGlobal(local_position.toPoint());
    QWheelEvent zoom_event(
        local_position,
        QPointF(global_point),
        QPoint(),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::ScrollUpdate,
        false
    );
    QApplication::sendEvent(&view, &zoom_event);
    application.processEvents();

    if (!zoom_event.isAccepted()) {
        std::cerr << "map wheel interaction was not accepted\n";
        return EXIT_FAILURE;
    }
    if (view.zoom_factor() <= old_zoom || view.viewport_pan() == old_pan) {
        std::cerr << "off-center globe zoom did not update viewport state\n";
        return EXIT_FAILURE;
    }

    const QPointF anchor_after =
        (local_position - center - view.viewport_pan()) / view.zoom_factor();
    if (!nearly_equal(anchor_before, anchor_after)) {
        std::cerr
            << "cursor-anchored globe zoom drifted: before=("
            << anchor_before.x() << ',' << anchor_before.y()
            << ") after=(" << anchor_after.x() << ',' << anchor_after.y() << ")\n";
        return EXIT_FAILURE;
    }

    // A pixel-delta wheel event exercises the continuous trackpad path rather
    // than the traditional 120-unit mouse-wheel path.
    const double angle_zoom = view.zoom_factor();
    QWheelEvent trackpad_event(
        local_position,
        QPointF(global_point),
        QPoint(0, 20),
        QPoint(),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::ScrollUpdate,
        false
    );
    QApplication::sendEvent(&view, &trackpad_event);
    application.processEvents();
    if (!trackpad_event.isAccepted() || view.zoom_factor() <= angle_zoom) {
        std::cerr << "pixel-delta trackpad zoom path did not advance continuously\n";
        return EXIT_FAILURE;
    }

    const QImage zoomed = render_view(view);
    if (zoomed == baseline) {
        std::cerr << "map wheel interaction did not change rendered pixels\n";
        return EXIT_FAILURE;
    }

    if (!baseline.save(QString::fromLocal8Bit(argv[2]), "PNG")) {
        std::cerr << "unable to save output PNG\n";
        return EXIT_FAILURE;
    }

    std::cout << "aeris_desktop_render_probe: PASS " << argv[2]
              << " (cursor-anchored globe + trackpad zoom changed pixels)\n";
    return EXIT_SUCCESS;
}
