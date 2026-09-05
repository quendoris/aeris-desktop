// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_workspace_view.hpp"
#include "project_model.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/view/projection_catalog.hpp"
#include "aeris/view/scene.hpp"
#include "aeris/view/surface.hpp"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>

namespace {

constexpr double kProofCutDeg = 37.0;
constexpr int kMapMarginPx = 24;

[[nodiscard]] double angular_difference_deg(
    const double left,
    const double right
) noexcept {
    return std::abs(std::remainder(left - right, 360.0));
}

[[nodiscard]] bool build_frame(
    const aeris::desktop::ProjectModel& model,
    const aeris::view::SceneRequest& request,
    aeris::desktop::RenderFrame& frame
) {
    frame = {};
    frame.request = request;

    for (const auto& entry : model.sources) {
        auto scene = aeris::view::build_scene_geometry(*entry.second, request);
        if (!scene.ok || scene.canceled ||
            scene.mode != request.mode ||
            scene.fill_rings == 0U ||
            scene.outline_parts == 0U ||
            scene.vertices == 0U) {
            std::cerr
                << aeris::view::surface_mode_name(request.mode)
                << " durable scene failed for " << entry.first
                << ": " << scene.diagnostic << '\n';
            return false;
        }
        if (request.mode != aeris::view::SurfaceMode::globe &&
            angular_difference_deg(
                scene.projection_central_meridian_deg,
                request.projection_central_meridian_deg
            ) > 1e-9) {
            std::cerr
                << aeris::view::surface_mode_name(request.mode)
                << " durable scene lost selected cut for " << entry.first << '\n';
            return false;
        }
        frame.source_scenes.emplace(entry.first, std::move(scene));
    }

    if (frame.source_scenes.size() != model.sources.size()) {
        std::cerr << "durable frame lost a source scene\n";
        return false;
    }

    frame.ok = true;
    frame.diagnostic = "projection catalog durable frame";
    return true;
}

[[nodiscard]] const aeris::view::ProjectionSeamSample* deepest_visible_seam_sample(
    const aeris::view::ProjectionSeamGeometry& seam
) noexcept {
    const aeris::view::ProjectionSeamSample* best = nullptr;
    double best_depth = -std::numeric_limits<double>::infinity();
    if (!seam.ok || seam.samples.size() < 3U) return nullptr;

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
        std::cerr << "unable to find visible seam sample for catalog mouse drag\n";
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
    if (!press_event.isAccepted()) return false;

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
    if (!move_event.isAccepted()) return false;

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
    return release_event.isAccepted();
}

[[nodiscard]] QImage render_view(aeris::desktop::MapWorkspaceView& view) {
    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter);
    painter.end();
    return image;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: aeris-desktop-projection-catalog-probe <project.aeris>\n";
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

    const auto& catalog = aeris::view::projection_catalog();
    if (catalog.size() != aeris::view::kProjectionCatalogSize || catalog.size() != 3U) {
        std::cerr << "unexpected projection catalog size\n";
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
    view.set_unfold_tool_active(true);
    view.show();
    application.processEvents();

    aeris::view::SceneRequest globe_request{};
    globe_request.mode = aeris::view::SurfaceMode::globe;
    globe_request.quality = aeris::view::SceneQuality::preview;
    globe_request.camera_longitude_deg = 15.0;
    globe_request.camera_latitude_deg = 20.0;

    aeris::desktop::RenderFrame globe_frame{};
    if (!build_frame(*model_result.model, globe_request, globe_frame)) {
        return EXIT_FAILURE;
    }

    for (const auto& descriptor : catalog) {
        if (view.surface_mode() != aeris::view::SurfaceMode::globe) {
            view.set_surface_mode(aeris::view::SurfaceMode::globe);
        }
        view.set_frame(globe_frame);
        view.reset_viewport();
        view.set_projection_central_meridian_deg(0.0);
        view.set_unfold_target_mode(descriptor.mode);
        application.processEvents();

        const std::size_t requests_before_drag = scene_requests;
        if (!drag_projection_cut_with_mouse(application, view, kProofCutDeg)) {
            std::cerr << descriptor.display_name << " direct seam drag was not accepted\n";
            return EXIT_FAILURE;
        }

        const double selected_cut_deg = view.projection_central_meridian_deg();
        if (angular_difference_deg(selected_cut_deg, kProofCutDeg) > 1e-7) {
            std::cerr
                << descriptor.display_name << " direct seam drag recovered "
                << selected_cut_deg << " deg instead of " << kProofCutDeg << " deg\n";
            return EXIT_FAILURE;
        }
        if (scene_requests != requests_before_drag) {
            std::cerr << descriptor.display_name << " drag rebuilt world geometry before Apply\n";
            return EXIT_FAILURE;
        }

        view.set_surface_mode(descriptor.mode);
        if (!last_request.has_value() ||
            last_request->mode != descriptor.mode ||
            last_request->quality != aeris::view::SceneQuality::verified ||
            angular_difference_deg(
                last_request->projection_central_meridian_deg,
                selected_cut_deg
            ) > 1e-9 ||
            scene_requests != requests_before_drag + 1U) {
            std::cerr << descriptor.display_name
                      << " Apply did not issue one verified request with the mouse-selected cut\n";
            return EXIT_FAILURE;
        }

        aeris::desktop::RenderFrame flat_frame{};
        if (!build_frame(*model_result.model, *last_request, flat_frame)) {
            return EXIT_FAILURE;
        }
        view.set_frame(std::move(flat_frame));
        application.processEvents();

        const QImage rendered = render_view(view);
        if (rendered.isNull()) {
            std::cerr << descriptor.display_name << " durable render is null\n";
            return EXIT_FAILURE;
        }

        std::cout
            << descriptor.display_name
            << ": PASS (mouse cut " << selected_cut_deg
            << " deg, no pre-Apply reprojection, one verified durable frame)\n";
    }

    std::cout << "aeris_desktop_projection_catalog_probe: PASS ("
              << catalog.size() << " catalog projections)\n";
    return EXIT_SUCCESS;
}
