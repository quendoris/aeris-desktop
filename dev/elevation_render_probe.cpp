// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_renderer.hpp"
#include "map_view.hpp"
#include "project_model.hpp"

#include "aeris/storage/layer.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/view/scene.hpp"
#include "aeris/view/surface.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

constexpr double kProofCutDeg = 37.0;
constexpr std::size_t kMinimumChangedPixels = 1000U;
constexpr std::size_t kMinimumDetailSamples = 1000U;

struct RenderProof final {
    QImage image;
    bool detail_lod_active{false};
    std::size_t detail_cached_tiles{0U};
    std::size_t detail_tile_loads{0U};
    std::size_t detail_samples_used{0U};
    double zoom{1.0};
};

[[nodiscard]] bool build_frame(
    const aeris::desktop::ProjectModel& model,
    const aeris::view::SurfaceMode mode,
    aeris::desktop::RenderFrame& frame
) {
    frame = {};
    frame.request.mode = mode;
    frame.request.quality = aeris::view::SceneQuality::verified;
    frame.request.camera_longitude_deg = 15.0;
    frame.request.camera_latitude_deg = 20.0;
    frame.request.projection_central_meridian_deg = kProofCutDeg;

    for (const auto& entry : model.sources) {
        auto scene = aeris::view::build_scene_geometry(*entry.second, frame.request);
        if (!scene.ok || scene.canceled || scene.mode != mode) {
            std::cerr
                << "terrain pixel proof could not build "
                << aeris::view::surface_mode_name(mode)
                << " scene for " << entry.first << ": "
                << scene.diagnostic << '\n';
            return false;
        }
        frame.source_scenes.emplace(entry.first, std::move(scene));
    }

    if (frame.source_scenes.size() != model.sources.size()) {
        std::cerr << "terrain pixel proof lost a durable source scene\n";
        return false;
    }
    frame.ok = true;
    frame.diagnostic = "terrain pixel proof frame";
    return true;
}

[[nodiscard]] RenderProof render_model(
    QApplication& application,
    std::shared_ptr<const aeris::desktop::ProjectModel> model,
    const std::string& project_uuid,
    const std::uint64_t revision,
    const aeris::desktop::RenderFrame& frame,
    const bool detail_zoom
) {
    aeris::desktop::MapView view;
    view.resize(960, 640);
    view.set_project(std::move(model), project_uuid, revision);
    if (frame.request.mode != aeris::view::SurfaceMode::globe) {
        view.set_surface_mode(frame.request.mode);
    }
    view.set_frame(frame);
    view.show();
    application.processEvents();

    if (detail_zoom) {
        for (int attempt = 0;
             attempt < 16 &&
                 view.zoom_factor() < aeris::desktop::kElevationDetailLodZoom;
             ++attempt) {
            view.zoom_in();
        }
        application.processEvents();
    }

    RenderProof proof{};
    proof.image = QImage(view.size(), QImage::Format_ARGB32_Premultiplied);
    proof.image.fill(Qt::transparent);
    QPainter painter(&proof.image);
    view.render(&painter);
    painter.end();
    application.processEvents();

    proof.detail_lod_active = view.elevation_detail_lod_active();
    proof.detail_cached_tiles = view.elevation_detail_cached_tiles();
    proof.detail_tile_loads = view.elevation_detail_tile_loads();
    proof.detail_samples_used = view.elevation_detail_samples_used();
    proof.zoom = view.zoom_factor();
    view.hide();
    application.processEvents();
    return proof;
}

[[nodiscard]] std::size_t changed_pixels(
    const QImage& left,
    const QImage& right
) noexcept {
    if (left.size() != right.size() ||
        left.format() != QImage::Format_ARGB32_Premultiplied ||
        right.format() != QImage::Format_ARGB32_Premultiplied) {
        return 0U;
    }

    std::size_t changed = 0U;
    for (int y = 0; y < left.height(); ++y) {
        const auto* left_row = reinterpret_cast<const QRgb*>(left.constScanLine(y));
        const auto* right_row = reinterpret_cast<const QRgb*>(right.constScanLine(y));
        for (int x = 0; x < left.width(); ++x) {
            if (left_row[x] != right_row[x]) ++changed;
        }
    }
    return changed;
}

[[nodiscard]] bool prove_surface(
    QApplication& application,
    const std::shared_ptr<const aeris::desktop::ProjectModel>& model,
    const std::shared_ptr<const aeris::desktop::ProjectModel>& without_elevation,
    const std::string& project_uuid,
    const std::uint64_t revision,
    const aeris::view::SurfaceMode mode
) {
    aeris::desktop::RenderFrame frame{};
    if (!build_frame(*model, mode, frame)) return false;

    const RenderProof overview = render_model(
        application,
        model,
        project_uuid,
        revision,
        frame,
        false
    );
    const RenderProof overview_without = render_model(
        application,
        without_elevation,
        project_uuid,
        revision,
        frame,
        false
    );
    if (overview.image.isNull() || overview_without.image.isNull()) {
        std::cerr << "terrain pixel proof produced a null overview image\n";
        return false;
    }

    const std::size_t overview_changed = changed_pixels(
        overview.image,
        overview_without.image
    );
    if (overview_changed < kMinimumChangedPixels) {
        std::cerr
            << aeris::view::surface_mode_name(mode)
            << " elevation visibility changed only " << overview_changed
            << " overview pixels; terrain renderer appears disconnected from the map stack\n";
        return false;
    }
    if (overview.detail_lod_active || overview.detail_cached_tiles != 0U ||
        overview.detail_tile_loads != 0U || overview.detail_samples_used != 0U) {
        std::cerr
            << aeris::view::surface_mode_name(mode)
            << " used detail elevation below the LOD threshold\n";
        return false;
    }

    const RenderProof detail = render_model(
        application,
        model,
        project_uuid,
        revision,
        frame,
        true
    );
    const RenderProof detail_without = render_model(
        application,
        without_elevation,
        project_uuid,
        revision,
        frame,
        true
    );
    if (detail.image.isNull() || detail_without.image.isNull()) {
        std::cerr << "terrain pixel proof produced a null detail image\n";
        return false;
    }

    const std::size_t detail_changed = changed_pixels(
        detail.image,
        detail_without.image
    );
    if (detail_changed < kMinimumChangedPixels) {
        std::cerr
            << aeris::view::surface_mode_name(mode)
            << " high-zoom elevation changed only " << detail_changed
            << " pixels\n";
        return false;
    }
    if (!detail.detail_lod_active ||
        detail.zoom < aeris::desktop::kElevationDetailLodZoom ||
        detail.detail_cached_tiles == 0U ||
        detail.detail_cached_tiles > aeris::desktop::kElevationDetailCacheTileLimit ||
        detail.detail_tile_loads == 0U ||
        detail.detail_tile_loads > aeris::desktop::kElevationDetailCacheTileLimit ||
        detail.detail_samples_used < kMinimumDetailSamples) {
        std::cerr
            << aeris::view::surface_mode_name(mode)
            << " did not use bounded durable detail samples at high zoom: active="
            << detail.detail_lod_active
            << " zoom=" << detail.zoom
            << " cached=" << detail.detail_cached_tiles
            << " loads=" << detail.detail_tile_loads
            << " samples=" << detail.detail_samples_used << '\n';
        return false;
    }
    if (detail_without.detail_lod_active ||
        detail_without.detail_cached_tiles != 0U ||
        detail_without.detail_tile_loads != 0U ||
        detail_without.detail_samples_used != 0U) {
        std::cerr << "hidden elevation layer unexpectedly activated detail LOD\n";
        return false;
    }

    std::cout
        << aeris::view::surface_mode_name(mode)
        << " terrain pixels: PASS overview_changed=" << overview_changed
        << " detail_changed=" << detail_changed
        << " detail_zoom=" << detail.zoom
        << " cached_tiles=" << detail.detail_cached_tiles
        << " tile_loads=" << detail.detail_tile_loads
        << " detail_samples=" << detail.detail_samples_used << '\n';
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr
            << "usage: aeris-desktop-elevation-render-probe <project.aeris>\n";
        return EXIT_FAILURE;
    }

    QApplication application(argc, argv);
    auto opened = aeris::storage::ProjectStore::open(std::filesystem::path(argv[1]));
    if (!opened.ok()) {
        std::cerr << "project open failed: " << opened.status.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    auto loaded = aeris::desktop::load_project_model(*opened.store);
    if (!loaded.ok()) {
        std::cerr << "project model failed: " << loaded.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    auto without_elevation =
        std::make_shared<aeris::desktop::ProjectModel>(*loaded.model);
    bool found_elevation = false;
    for (auto& layer : without_elevation->layers) {
        if (layer.role_id != aeris::storage::kLayerRolePhysicalElevationV1) continue;
        if (!layer.visible) {
            std::cerr << "physical elevation layer is unexpectedly hidden in proof project\n";
            return EXIT_FAILURE;
        }
        layer.visible = false;
        found_elevation = true;
    }
    if (!found_elevation) {
        std::cerr << "proof project does not contain a physical elevation layer\n";
        return EXIT_FAILURE;
    }

    const auto& metadata = opened.store->metadata();
    if (!prove_surface(
            application,
            loaded.model,
            without_elevation,
            metadata.project_uuid,
            metadata.revision,
            aeris::view::SurfaceMode::globe
        ) ||
        !prove_surface(
            application,
            loaded.model,
            without_elevation,
            metadata.project_uuid,
            metadata.revision,
            aeris::view::SurfaceMode::sinu_mollweide
        )) {
        return EXIT_FAILURE;
    }

    std::cout
        << "aeris-desktop-elevation-render-probe: PASS "
        << "overview + bounded high-zoom detail change durable Globe and Sinu-Mollweide pixels\n";
    return EXIT_SUCCESS;
}
