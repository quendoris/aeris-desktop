// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_workspace_view.hpp"
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
#include <utility>

namespace {

constexpr double kProofCutDeg = 37.0;

[[nodiscard]] QImage render_view(aeris::desktop::MapView& view) {
    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter);
    painter.end();
    return image;
}

[[nodiscard]] bool build_globe_preview_frame(
    const aeris::desktop::ProjectModel& model,
    aeris::desktop::RenderFrame& frame
) {
    frame = {};
    frame.request.mode = aeris::view::SurfaceMode::globe;
    frame.request.quality = aeris::view::SceneQuality::preview;
    frame.request.camera_longitude_deg = 15.0;
    frame.request.camera_latitude_deg = 20.0;

    for (const auto& entry : model.sources) {
        auto scene = aeris::view::build_scene_geometry(*entry.second, frame.request);
        if (!scene.ok || scene.canceled ||
            scene.fill_rings == 0U || scene.outline_parts == 0U) {
            std::cerr
                << "durable Globe preview scene failed for "
                << entry.first << ": " << scene.diagnostic << '\n';
            return false;
        }
        frame.source_scenes.emplace(entry.first, std::move(scene));
    }

    if (frame.source_scenes.size() != model.sources.size()) {
        std::cerr << "durable Globe preview frame lost a source scene\n";
        return false;
    }

    frame.ok = true;
    frame.diagnostic = "durable filled Globe preview frame";
    std::cout
        << "durable Globe preview frame: PASS ("
        << frame.source_scenes.size() << " sources)\n";
    return true;
}

[[nodiscard]] bool build_sinu_mollweide_frame(
    const aeris::desktop::ProjectModel& model,
    const double cut_deg,
    aeris::desktop::RenderFrame& frame
) {
    frame = {};
    frame.request.mode = aeris::view::SurfaceMode::sinu_mollweide;
    frame.request.quality = aeris::view::SceneQuality::verified;
    frame.request.projection_central_meridian_deg = cut_deg;

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
            std::abs(scene.projection_central_meridian_deg - cut_deg) > 1e-12) {
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
        << frame.source_scenes.size() << " sources, cut "
        << cut_deg << " deg)\n";
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
    if (argc != 4) {
        std::cerr
            << "usage: aeris-desktop-render-probe "
            << "<project.aeris> <surface.png> <seam.png>\n";
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

    aeris::desktop::MapWorkspaceView view;
    view.resize(1280, 820);

    std::optional<aeris::view::SceneRequest> last_request;
    std::size_t scene_requests = 0U;
    view.set_scene_request_callback(
        [&](const aeris::view::SceneRequest& request) {
            last_request = request;
            ++scene_requests;
        }
    );
    view.set_project(
        model_result.model,
        opened.store->metadata().project_uuid,
        opened.store->metadata().revision
    );

    aeris::desktop::RenderFrame globe_frame{};
    if (!build_globe_preview_frame(*model_result.model, globe_frame)) {
        return EXIT_FAILURE;
    }
    view.set_frame(std::move(globe_frame));
    view.show();
    application.processEvents();

    const QImage clean_globe = render_view(view);
    if (clean_globe.isNull()) {
        std::cerr << "clean Globe render produced a null image\n";
        return EXIT_FAILURE;
    }

    view.set_unfold_target_mode(aeris::view::SurfaceMode::sinu_mollweide);
    view.set_unfold_tool_active(true);
    application.processEvents();
    const QImage default_seam = render_view(view);
    if (default_seam == clean_globe) {
        std::cerr << "opening Unfold tool did not reveal the projection seam\n";
        return EXIT_FAILURE;
    }

    const std::size_t requests_before_cut_move = scene_requests;
    view.set_projection_central_meridian_deg(kProofCutDeg);
    application.processEvents();
    if (!nearly_equal(view.projection_central_meridian_deg(), kProofCutDeg)) {
        std::cerr << "projection cut state did not move on the folded Globe\n";
        return EXIT_FAILURE;
    }
    if (scene_requests != requests_before_cut_move) {
        std::cerr << "moving projection cut rebuilt world geometry before Apply\n";
        return EXIT_FAILURE;
    }

    const QImage moved_seam = render_view(view);
    if (moved_seam == default_seam || moved_seam == clean_globe) {
        std::cerr << "moving projection cut did not change seam pixels\n";
        return EXIT_FAILURE;
    }
    if (!moved_seam.save(QString::fromLocal8Bit(argv[3]), "PNG")) {
        std::cerr << "unable to save movable Globe seam output PNG\n";
        return EXIT_FAILURE;
    }

    // Applying the selected surface is the first operation that is allowed to
    // request a heavy verified reprojection. The chosen cut must cross this
    // boundary unchanged.
    view.set_surface_mode(aeris::view::SurfaceMode::sinu_mollweide);
    if (!last_request.has_value() ||
        last_request->mode != aeris::view::SurfaceMode::sinu_mollweide ||
        last_request->quality != aeris::view::SceneQuality::verified ||
        !nearly_equal(last_request->projection_central_meridian_deg, kProofCutDeg) ||
        scene_requests != requests_before_cut_move + 1U) {
        std::cerr << "MapView did not apply the selected Sinu-Mollweide cut exactly once\n";
        return EXIT_FAILURE;
    }

    aeris::desktop::RenderFrame frame{};
    if (!build_sinu_mollweide_frame(*model_result.model, kProofCutDeg, frame)) {
        return EXIT_FAILURE;
    }
    view.set_frame(std::move(frame));
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

    if (!baseline.save(QString::fromLocal8Bit(argv[2]), "PNG")) {
        std::cerr << "unable to save Sinu-Mollweide output PNG\n";
        return EXIT_FAILURE;
    }

    std::cout << "aeris_desktop_render_probe: PASS " << argv[2]
              << " + " << argv[3]
              << " (clean Globe, movable seam without reprojection, applied cut, durable Sinu-Mollweide, cursor anchor, trackpad zoom, Globe/flat viewport independence)\n";
    return EXIT_SUCCESS;
}
