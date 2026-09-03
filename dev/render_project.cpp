// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_workspace_view.hpp"
#include "project_model.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/view/scene.hpp"
#include "aeris/view/surface.hpp"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr double kProofCutDeg = 37.0;
constexpr int kMapMarginPx = 24;

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

[[nodiscard]] double angular_difference_deg(
    const double left,
    const double right
) noexcept {
    return std::abs(std::remainder(left - right, 360.0));
}

[[nodiscard]] const aeris::view::ProjectionSeamSample* deepest_visible_seam_sample(
    const aeris::view::ProjectionSeamGeometry& seam
) noexcept {
    const aeris::view::ProjectionSeamSample* best = nullptr;
    double best_depth = -std::numeric_limits<double>::infinity();
    if (!seam.ok || seam.samples.size() < 3U) return nullptr;

    // Exclude the projection-frame poles: longitude, and therefore a draggable
    // cut meridian, is mathematically indeterminate exactly at either pole.
    for (std::size_t index = 1U; index + 1U < seam.samples.size(); ++index) {
        const auto& sample = seam.samples[index];
        if (sample.globe_visible && sample.globe_depth_normalized > best_depth) {
            best = &sample;
            best_depth = sample.globe_depth_normalized;
        }
    }
    return best;
}

[[nodiscard]] QTransform globe_device_transform(
    const aeris::desktop::MapWorkspaceView& view
) {
    const double radius = aeris::geo::authalic_radius_m();
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

[[nodiscard]] QPointF seam_sample_device_point(
    const aeris::desktop::MapWorkspaceView& view,
    const aeris::view::ProjectionSeamSample& sample
) {
    return globe_device_transform(view).map(QPointF(sample.globe.x, sample.globe.y));
}

[[nodiscard]] QPointF global_device_point(
    const aeris::desktop::MapWorkspaceView& view,
    const QPointF local_point
) {
    return QPointF(view.mapToGlobal(local_point.toPoint()));
}

[[nodiscard]] const aeris::view::ProjectionSeamSample* current_stable_seam_sample(
    const aeris::desktop::MapWorkspaceView& view
) {
    const auto seam = aeris::view::build_projection_seam_geometry(
        view.unfold_target_mode(),
        view.displayed_camera_longitude_deg(),
        view.displayed_camera_latitude_deg(),
        view.projection_central_meridian_deg()
    );
    return deepest_visible_seam_sample(seam);
}

[[nodiscard]] bool hover_projection_cut_with_mouse(
    QApplication& application,
    aeris::desktop::MapWorkspaceView& view
) {
    const auto seam = aeris::view::build_projection_seam_geometry(
        view.unfold_target_mode(),
        view.displayed_camera_longitude_deg(),
        view.displayed_camera_latitude_deg(),
        view.projection_central_meridian_deg()
    );
    const auto* sample = deepest_visible_seam_sample(seam);
    if (sample == nullptr) {
        std::cerr << "unable to find stable visible seam sample for hover proof\n";
        return false;
    }

    const QPointF local_position = seam_sample_device_point(view, *sample);
    QMouseEvent hover_event(
        QEvent::MouseMove,
        local_position,
        global_device_point(view, local_position),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(&view, &hover_event);
    application.processEvents();
    return true;
}

[[nodiscard]] bool drag_projection_cut_with_mouse(
    QApplication& application,
    aeris::desktop::MapWorkspaceView& view,
    const double target_cut_deg
) {
    const auto current_seam = aeris::view::build_projection_seam_geometry(
        view.unfold_target_mode(),
        view.displayed_camera_longitude_deg(),
        view.displayed_camera_latitude_deg(),
        view.projection_central_meridian_deg()
    );
    const auto target_seam = aeris::view::build_projection_seam_geometry(
        view.unfold_target_mode(),
        view.displayed_camera_longitude_deg(),
        view.displayed_camera_latitude_deg(),
        target_cut_deg
    );
    const auto* current_sample = deepest_visible_seam_sample(current_seam);
    const auto* target_sample = deepest_visible_seam_sample(target_seam);
    if (current_sample == nullptr || target_sample == nullptr) {
        std::cerr << "unable to find stable visible seam samples for mouse drag\n";
        return false;
    }

    const QPointF press_position = seam_sample_device_point(view, *current_sample);
    const QPointF move_position = seam_sample_device_point(view, *target_sample);

    QMouseEvent press_event(
        QEvent::MouseButtonPress,
        press_position,
        global_device_point(view, press_position),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(&view, &press_event);
    application.processEvents();
    if (!press_event.isAccepted()) {
        std::cerr << "projection seam mouse press was not accepted\n";
        return false;
    }

    QMouseEvent move_event(
        QEvent::MouseMove,
        move_position,
        global_device_point(view, move_position),
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(&view, &move_event);
    application.processEvents();
    if (!move_event.isAccepted()) {
        std::cerr << "projection seam mouse drag was not accepted\n";
        return false;
    }

    QMouseEvent release_event(
        QEvent::MouseButtonRelease,
        move_position,
        global_device_point(view, move_position),
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier
    );
    QApplication::sendEvent(&view, &release_event);
    application.processEvents();
    if (!release_event.isAccepted()) {
        std::cerr << "projection seam mouse release was not accepted\n";
        return false;
    }
    return true;
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

    std::size_t direct_cut_edits = 0U;
    double last_direct_cut = 0.0;
    QObject::connect(
        &view,
        &aeris::desktop::MapWorkspaceView::projectionCutEdited,
        [&](const double degrees) {
            ++direct_cut_edits;
            last_direct_cut = degrees;
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

    const std::size_t requests_before_hover = scene_requests;
    if (!hover_projection_cut_with_mouse(application, view)) {
        return EXIT_FAILURE;
    }
    const QImage hovered_seam = render_view(view);
    if (hovered_seam == default_seam) {
        std::cerr << "hovering projection seam did not reveal grab handle pixels\n";
        return EXIT_FAILURE;
    }
    if (scene_requests != requests_before_hover) {
        std::cerr << "hovering projection seam rebuilt world geometry\n";
        return EXIT_FAILURE;
    }
    std::cout << "projection seam hover handle: PASS (presentation only)\n";

    const std::size_t requests_before_cut_move = scene_requests;
    if (!drag_projection_cut_with_mouse(application, view, kProofCutDeg)) {
        return EXIT_FAILURE;
    }
    const double selected_cut_deg = view.projection_central_meridian_deg();
    if (angular_difference_deg(selected_cut_deg, kProofCutDeg) > 1e-7) {
        std::cerr
            << "direct projection seam drag recovered wrong cut: got "
            << selected_cut_deg << " expected " << kProofCutDeg << '\n';
        return EXIT_FAILURE;
    }
    if (direct_cut_edits == 0U ||
        !nearly_equal(last_direct_cut, selected_cut_deg)) {
        std::cerr << "direct projection seam drag did not publish edited cut state\n";
        return EXIT_FAILURE;
    }
    if (scene_requests != requests_before_cut_move) {
        std::cerr << "direct projection seam drag rebuilt world geometry before Apply\n";
        return EXIT_FAILURE;
    }

    const QImage moved_seam = render_view(view);
    if (moved_seam == default_seam || moved_seam == clean_globe) {
        std::cerr << "direct projection seam drag did not change seam pixels\n";
        return EXIT_FAILURE;
    }
    if (!moved_seam.save(QString::fromLocal8Bit(argv[3]), "PNG")) {
        std::cerr << "unable to save direct-drag Globe seam output PNG\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "direct Globe seam drag: PASS (cut "
        << selected_cut_deg << " deg, no world reprojection)\n";

    // Applying the selected surface is the first operation that is allowed to
    // request a heavy verified reprojection. The mouse-selected cut must cross
    // this boundary unchanged.
    view.set_surface_mode(aeris::view::SurfaceMode::sinu_mollweide);
    if (!last_request.has_value() ||
        last_request->mode != aeris::view::SurfaceMode::sinu_mollweide ||
        last_request->quality != aeris::view::SceneQuality::verified ||
        !nearly_equal(
            last_request->projection_central_meridian_deg,
            selected_cut_deg
        ) ||
        scene_requests != requests_before_cut_move + 1U) {
        std::cerr << "MapView did not apply the mouse-selected Sinu-Mollweide cut exactly once\n";
        return EXIT_FAILURE;
    }

    aeris::desktop::RenderFrame frame{};
    if (!build_sinu_mollweide_frame(*model_result.model, selected_cut_deg, frame)) {
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
              << " (clean Globe, hover grab affordance, direct seam mouse drag without reprojection, mouse-selected applied cut, durable Sinu-Mollweide, cursor anchor, trackpad zoom, Globe/flat viewport independence)\n";
    return EXIT_SUCCESS;
}
