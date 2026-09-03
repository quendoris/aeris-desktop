// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_view.hpp"
#include "project_model.hpp"

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
#include <optional>
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

[[nodiscard]] bool build_sinu_mollweide_frame(
    const aeris::desktop::ProjectModel& model,
    aeris::desktop::RenderFrame& frame
) {
    frame = {};
    frame.request.mode = aeris::view::SurfaceMode::sinu_mollweide;
    frame.request.quality = aeris::view::SceneQuality::verified;
    frame.request.projection_central_meridian_deg = 0.0;

    for (const auto& entry : model.sources) {
        auto scene = aeris::view::build_scene_geometry(*entry.second, frame.request);
        if (!scene.ok || scene.canceled) {
            std::cerr
                << "durable Sinu-Mollweide scene failed for "
                << entry.first << ": " << scene.diagnostic << '\n';
            return false;
        }
        if (scene.mode != aeris::view::SurfaceMode::sinu_mollweide ||
            scene.fill_rings == 0U || scene.outline_parts == 0U ||
            scene.vertices == 0U ||
            std::abs(scene.projection_central_meridian_deg) > 1e-12) {
            std::cerr
                << "durable Sinu-Mollweide scene is structurally incomplete for "
                << entry.first << '\n';
            return false;
        }
        frame.source_scenes.emplace(entry.first, std::move(scene));
    }

    if (frame.source_scenes.size() != model.sources.size()) {
        std::cerr << "durable Sinu-Mollweide frame lost a source scene\n";
        return false;
    }

    frame.ok = true;
    frame.diagnostic = "verified durable Sinu-Mollweide frame";
    std::cout
        << "durable Sinu-Mollweide world frame: PASS ("
        << frame.source_scenes.size() << " sources)\n";
    return true;
}

[[nodiscard]] bool nearly_equal(const QPointF& left, const QPointF& right) noexcept {
    return std::hypot(left.x() - right.x(), left.y() - right.y()) <= 1e-9;
}

[[nodiscard]] bool nearly_equal(const double left, const double right) noexcept {
    return std::abs(left - right) <= 1e-12;
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

    if (!verify_globe_preview_fill(*model_result.model)) {
        return EXIT_FAILURE;
    }

    aeris::desktop::RenderFrame frame{};
    if (!build_sinu_mollweide_frame(*model_result.model, frame)) {
        return EXIT_FAILURE;
    }

    aeris::desktop::MapView view;
    view.resize(1280, 820);

    std::optional<aeris::view::SceneRequest> last_request;
    view.set_scene_request_callback(
        [&](const aeris::view::SceneRequest& request) {
            last_request = request;
        }
    );
    view.set_project(
        model_result.model,
        opened.store->metadata().project_uuid,
        opened.store->metadata().revision
    );
    view.set_surface_mode(aeris::view::SurfaceMode::sinu_mollweide);
    if (!last_request.has_value() ||
        last_request->mode != aeris::view::SurfaceMode::sinu_mollweide ||
        last_request->quality != aeris::view::SceneQuality::verified ||
        !nearly_equal(last_request->projection_central_meridian_deg, 0.0)) {
        std::cerr << "MapView did not request the primary Sinu-Mollweide surface/cut\n";
        return EXIT_FAILURE;
    }

    view.set_frame(std::move(frame));
    view.show();
    application.processEvents();

    const QImage baseline = render_view(view);
    if (baseline.isNull()) {
        std::cerr << "durable Sinu-Mollweide render produced a null image\n";
        return EXIT_FAILURE;
    }

    // Deliberately zoom away from the center. The same projected point must
    // remain under the cursor; zoom is a viewport transform and does not rebuild
    // the verified Sinu-Mollweide geometry merely to scale it.
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
        std::cerr << "Sinu-Mollweide wheel interaction was not accepted\n";
        return EXIT_FAILURE;
    }
    if (view.zoom_factor() <= old_zoom || view.viewport_pan() == old_pan) {
        std::cerr << "off-center Sinu-Mollweide zoom did not update viewport state\n";
        return EXIT_FAILURE;
    }

    const QPointF anchor_after =
        (local_position - center - view.viewport_pan()) / view.zoom_factor();
    if (!nearly_equal(anchor_before, anchor_after)) {
        std::cerr
            << "cursor-anchored Sinu-Mollweide zoom drifted: before=("
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
        std::cerr << "pixel-delta Sinu-Mollweide trackpad zoom did not advance continuously\n";
        return EXIT_FAILURE;
    }

    const QImage zoomed = render_view(view);
    if (zoomed == baseline) {
        std::cerr << "Sinu-Mollweide wheel interaction did not change rendered pixels\n";
        return EXIT_FAILURE;
    }

    // The complete unfold surface owns an independent navigation context. Globe
    // camera navigation must not overwrite the user's flat-map zoom/pan state.
    const double flat_zoom = view.zoom_factor();
    const QPointF flat_pan = view.viewport_pan();

    view.set_surface_mode(aeris::view::SurfaceMode::globe);
    if (!nearly_equal(view.zoom_factor(), 1.0) ||
        !nearly_equal(view.viewport_pan(), QPointF{})) {
        std::cerr << "Globe inherited the Sinu-Mollweide viewport\n";
        return EXIT_FAILURE;
    }

    QWheelEvent globe_zoom_event(
        local_position,
        QPointF(global_point),
        QPoint(),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::ScrollUpdate,
        false
    );
    QApplication::sendEvent(&view, &globe_zoom_event);
    const double globe_zoom = view.zoom_factor();
    const QPointF globe_pan = view.viewport_pan();
    if (globe_zoom <= 1.0 || globe_pan.isNull()) {
        std::cerr << "Globe viewport did not accept independent navigation\n";
        return EXIT_FAILURE;
    }

    view.set_surface_mode(aeris::view::SurfaceMode::sinu_mollweide);
    if (!nearly_equal(view.zoom_factor(), flat_zoom) ||
        !nearly_equal(view.viewport_pan(), flat_pan)) {
        std::cerr << "Sinu-Mollweide viewport was not restored after Globe work\n";
        return EXIT_FAILURE;
    }

    view.set_surface_mode(aeris::view::SurfaceMode::globe);
    if (!nearly_equal(view.zoom_factor(), globe_zoom) ||
        !nearly_equal(view.viewport_pan(), globe_pan)) {
        std::cerr << "Globe viewport was not restored after Sinu-Mollweide work\n";
        return EXIT_FAILURE;
    }

    view.set_surface_mode(aeris::view::SurfaceMode::sinu_mollweide);
    view.reset_viewport();
    if (!nearly_equal(view.zoom_factor(), 1.0) ||
        !nearly_equal(view.viewport_pan(), QPointF{})) {
        std::cerr << "reset viewport did not restore the Sinu-Mollweide default\n";
        return EXIT_FAILURE;
    }

    // Save the verified complete surface before interaction so the CI artifact
    // is a direct durable-project rendering rather than a diagnostic primitive.
    if (!baseline.save(QString::fromLocal8Bit(argv[2]), "PNG")) {
        std::cerr << "unable to save Sinu-Mollweide output PNG\n";
        return EXIT_FAILURE;
    }

    std::cout << "aeris_desktop_render_probe: PASS " << argv[2]
              << " (durable Sinu-Mollweide, cursor anchor, trackpad zoom, Globe/flat viewport independence)\n";
    return EXIT_SUCCESS;
}
