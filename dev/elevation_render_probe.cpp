// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_renderer.hpp"
#include "elevation_style.hpp"
#include "map_view.hpp"
#include "project_model.hpp"

#include "aeris/storage/layer.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/surface/classification.hpp"
#include "aeris/view/scene.hpp"
#include "aeris/view/surface.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr double kProofCutDeg = 37.0;
constexpr std::size_t kMinimumChangedPixels = 1000U;
constexpr std::size_t kMinimumSemanticChangedPixels = 64U;
constexpr std::size_t kMinimumDetailSamples = 1000U;
constexpr qint64 kDetailWorkerProofTimeoutMs = 10000;
constexpr qint64 kTerrainRefineProofTimeoutMs = 1000;

struct RenderProof final {
    QImage image;
    bool detail_lod_active{false};
    std::size_t detail_cached_tiles{0U};
    std::size_t detail_tile_loads{0U};
    std::size_t detail_samples_used{0U};
    std::size_t detail_pending_resources{0U};
    bool detail_loader_busy{false};
    std::size_t initial_detail_tile_loads{0U};
    std::size_t initial_detail_samples_used{0U};
    std::size_t initial_pending_resources{0U};
    bool initial_loader_busy{false};
    std::size_t raster_samples_used{0U};
    std::size_t initial_raster_samples_used{0U};
    bool interactive_quality{false};
    bool initial_interactive_quality{false};
    double zoom{1.0};
};

[[nodiscard]] bool build_frame(
    const aeris::desktop::ProjectModel& model,
    const aeris::view::SurfaceMode mode,
    aeris::desktop::RenderFrame& frame,
    const double camera_longitude_deg = 15.0,
    const double camera_latitude_deg = 20.0
) {
    frame = {};
    frame.request.mode = mode;
    frame.request.quality = aeris::view::SceneQuality::verified;
    frame.request.camera_longitude_deg = camera_longitude_deg;
    frame.request.camera_latitude_deg = camera_latitude_deg;
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

[[nodiscard]] QImage render_view(aeris::desktop::MapView& view) {
    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter);
    painter.end();
    return image;
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
        // Do not process events here. The first explicit render below must prove
        // that high-zoom paint itself only records missing detail and never waits
        // for storage/hash/decode work on the UI thread.
    }

    RenderProof proof{};
    proof.image = render_view(view);
    proof.initial_detail_tile_loads = view.elevation_detail_tile_loads();
    proof.initial_detail_samples_used = view.elevation_detail_samples_used();
    proof.initial_pending_resources = view.elevation_detail_pending_resources();
    proof.initial_loader_busy = view.elevation_detail_loader_busy();
    proof.initial_raster_samples_used = view.elevation_raster_samples_used();
    proof.initial_interactive_quality = view.elevation_interactive_quality();

    if (detail_zoom && proof.initial_loader_busy) {
        QElapsedTimer timer;
        timer.start();
        while (view.elevation_detail_loader_busy() &&
               timer.elapsed() < kDetailWorkerProofTimeoutMs) {
            application.processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1UL);
        }
        application.processEvents();
        proof.image = render_view(view);
        application.processEvents();
    }

    if (detail_zoom && view.elevation_interactive_quality()) {
        QElapsedTimer refine_timer;
        refine_timer.start();
        while (view.elevation_interactive_quality() &&
               refine_timer.elapsed() < kTerrainRefineProofTimeoutMs) {
            application.processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1UL);
        }
        application.processEvents();
        proof.image = render_view(view);
    }

    proof.detail_lod_active = view.elevation_detail_lod_active();
    proof.detail_cached_tiles = view.elevation_detail_cached_tiles();
    proof.detail_tile_loads = view.elevation_detail_tile_loads();
    proof.detail_samples_used = view.elevation_detail_samples_used();
    proof.detail_pending_resources = view.elevation_detail_pending_resources();
    proof.detail_loader_busy = view.elevation_detail_loader_busy();
    proof.raster_samples_used = view.elevation_raster_samples_used();
    proof.interactive_quality = view.elevation_interactive_quality();
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
        overview.detail_tile_loads != 0U || overview.detail_samples_used != 0U ||
        overview.detail_pending_resources != 0U || overview.detail_loader_busy) {
        std::cerr
            << aeris::view::surface_mode_name(mode)
            << " used/requested detail elevation below the LOD threshold\n";
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

    // The first high-zoom paint must have returned before any durable tile was
    // accepted or sampled. It may only have produced a bounded async request.
    if (detail.initial_detail_tile_loads != 0U ||
        detail.initial_detail_samples_used != 0U ||
        detail.initial_pending_resources == 0U ||
        detail.initial_pending_resources > aeris::desktop::kElevationDetailCacheTileLimit ||
        !detail.initial_loader_busy ||
        !detail.initial_interactive_quality ||
        detail.initial_raster_samples_used == 0U ||
        detail.initial_raster_samples_used >
            aeris::desktop::kElevationInteractiveDetailRasterSampleBudget) {
        std::cerr
            << aeris::view::surface_mode_name(mode)
            << " high-zoom paint did not stay I/O-free: initial_loads="
            << detail.initial_detail_tile_loads
            << " initial_samples=" << detail.initial_detail_samples_used
            << " initial_pending=" << detail.initial_pending_resources
            << " initial_worker=" << detail.initial_loader_busy
            << " initial_interactive=" << detail.initial_interactive_quality
            << " initial_raster_samples=" << detail.initial_raster_samples_used << '\n';
        return false;
    }

    if (!detail.detail_lod_active ||
        detail.zoom < aeris::desktop::kElevationDetailLodZoom ||
        detail.detail_cached_tiles == 0U ||
        detail.detail_cached_tiles > aeris::desktop::kElevationDetailCacheTileLimit ||
        detail.detail_tile_loads == 0U ||
        detail.detail_tile_loads > aeris::desktop::kElevationDetailCacheTileLimit ||
        detail.detail_samples_used < kMinimumDetailSamples ||
        detail.detail_loader_busy ||
        detail.interactive_quality ||
        detail.raster_samples_used == 0U ||
        detail.raster_samples_used >
            aeris::desktop::kElevationFinalDetailRasterSampleBudget ||
        detail.raster_samples_used <= detail.initial_raster_samples_used) {
        std::cerr
            << aeris::view::surface_mode_name(mode)
            << " did not consume a completed bounded async detail batch: active="
            << detail.detail_lod_active
            << " zoom=" << detail.zoom
            << " cached=" << detail.detail_cached_tiles
            << " loads=" << detail.detail_tile_loads
            << " samples=" << detail.detail_samples_used
            << " pending=" << detail.detail_pending_resources
            << " worker=" << detail.detail_loader_busy
            << " interactive=" << detail.interactive_quality
            << " raster_samples=" << detail.raster_samples_used
            << " initial_raster_samples=" << detail.initial_raster_samples_used << '\n';
        return false;
    }

    if (detail_without.initial_pending_resources != 0U ||
        detail_without.initial_loader_busy ||
        detail_without.detail_lod_active ||
        detail_without.detail_cached_tiles != 0U ||
        detail_without.detail_tile_loads != 0U ||
        detail_without.detail_samples_used != 0U ||
        detail_without.detail_pending_resources != 0U ||
        detail_without.detail_loader_busy) {
        std::cerr << "hidden elevation layer unexpectedly activated async detail LOD\n";
        return false;
    }

    std::cout
        << aeris::view::surface_mode_name(mode)
        << " terrain pixels: PASS overview_changed=" << overview_changed
        << " detail_changed=" << detail_changed
        << " detail_zoom=" << detail.zoom
        << " initial_pending=" << detail.initial_pending_resources
        << " interactive_samples=" << detail.initial_raster_samples_used
        << " refined_samples=" << detail.raster_samples_used
        << " cached_tiles=" << detail.detail_cached_tiles
        << " async_tile_loads=" << detail.detail_tile_loads
        << " detail_samples=" << detail.detail_samples_used << '\n';
    return true;
}

[[nodiscard]] std::optional<aeris::surface::SurfaceClass> semantic_class(
    const aeris::source::Feature& feature
) {
    for (const aeris::source::FeatureProperty& property : feature.properties) {
        if (property.key != aeris::surface::kSurfaceClassPropertyKey) continue;
        const auto* value = std::get_if<std::string>(&property.value);
        if (value == nullptr) return std::nullopt;
        return aeris::surface::parse_surface_class_id(*value);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int16_t> overview_elevation_at(
    const aeris::desktop::ProjectModel& model,
    const double longitude_deg,
    const double latitude_deg
) {
    for (const auto& layer : model.layers) {
        if (layer.role_id != aeris::storage::kLayerRolePhysicalElevationV1) continue;
        for (const auto& binding : layer.resources) {
            if (binding.slot_id.rfind("overview:", 0U) != 0U) continue;
            const auto resource_it = model.resources.find(binding.resource_id);
            if (resource_it == model.resources.end() || !resource_it->second ||
                !resource_it->second->elevation_tile.has_value()) {
                continue;
            }
            const auto& tile = *resource_it->second->elevation_tile;
            constexpr double microarcsec_per_degree = 3600.0 * 1000000.0;
            const double west =
                static_cast<double>(tile.west_microarcsec) / microarcsec_per_degree;
            const double north =
                static_cast<double>(tile.north_microarcsec) / microarcsec_per_degree;
            const double lon_step =
                static_cast<double>(tile.longitude_step_microarcsec) /
                microarcsec_per_degree;
            const double lat_step =
                static_cast<double>(tile.latitude_step_microarcsec) /
                microarcsec_per_degree;
            if (!(lon_step > 0.0) || !(lat_step > 0.0) ||
                tile.width == 0U || tile.height == 0U) {
                continue;
            }

            const double raw_x = (longitude_deg - west) / lon_step - 0.5;
            const double raw_y = (north - latitude_deg) / lat_step - 0.5;
            if (raw_x < -0.5 || raw_y < -0.5 ||
                raw_x > static_cast<double>(tile.width) - 0.5 ||
                raw_y > static_cast<double>(tile.height) - 0.5) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(std::lround(std::clamp(
                raw_x, 0.0, static_cast<double>(tile.width - 1U)
            )));
            const auto y = static_cast<std::uint32_t>(std::lround(std::clamp(
                raw_y, 0.0, static_cast<double>(tile.height - 1U)
            )));
            const std::int16_t sample = tile.samples_m[
                static_cast<std::size_t>(y) * static_cast<std::size_t>(tile.width) +
                static_cast<std::size_t>(x)
            ];
            if (sample == aeris::elevation::kNoDataMeters) return std::nullopt;
            return sample;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool role_contains_surface_origin(
    const aeris::desktop::ProjectModel& model,
    const aeris::desktop::RenderFrame& frame,
    const std::string_view role_id
) {
    for (const auto& layer : model.layers) {
        if (!layer.visible || layer.role_id != role_id) continue;
        for (const auto& binding : layer.sources) {
            if (binding.slot_id != "geometry" &&
                role_id == aeris::storage::kLayerRolePhysicalSurfaceClassificationV1) {
                continue;
            }
            const auto scene_it = frame.source_scenes.find(binding.source_id);
            if (scene_it == frame.source_scenes.end()) continue;
            for (const auto& feature : scene_it->second.features) {
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
                if (!path.isEmpty() && path.contains(QPointF(0.0, 0.0))) {
                    return true;
                }
            }
        }
    }
    return false;
}

[[nodiscard]] bool prove_positive_non_land_stays_water_material(
    QApplication& application,
    const std::shared_ptr<const aeris::desktop::ProjectModel>& model,
    const std::shared_ptr<const aeris::desktop::ProjectModel>& without_elevation,
    const std::string& project_uuid,
    const std::uint64_t revision
) {
    // This point is deliberately in the open Southern Ocean and lands exactly
    // on a deterministic 15-degree overview cell center. The CI elevation
    // fixture has a positive numerical sample here. Positive height must not
    // manufacture ordinary-land hue.
    constexpr double longitude_deg = 157.5;
    constexpr double latitude_deg = -52.5;
    const auto sample = overview_elevation_at(*model, longitude_deg, latitude_deg);
    if (!sample.has_value() || *sample <= 0) {
        std::cerr
            << "positive non-land proof expected a positive overview elevation at "
            << longitude_deg << "," << latitude_deg << "\n";
        return false;
    }

    aeris::desktop::RenderFrame frame{};
    if (!build_frame(
            *model,
            aeris::view::SurfaceMode::globe,
            frame,
            longitude_deg,
            latitude_deg
        )) {
        return false;
    }
    if (role_contains_surface_origin(
            *model,
            frame,
            aeris::storage::kLayerRolePhysicalLandFillV1
        ) ||
        role_contains_surface_origin(
            *model,
            frame,
            aeris::storage::kLayerRolePhysicalSurfaceClassificationV1
        )) {
        std::cerr
            << "positive non-land proof coordinate is covered by durable land/ice semantics\n";
        return false;
    }

    const RenderProof with_relief = render_model(
        application, model, project_uuid, revision, frame, false
    );
    const RenderProof without_relief = render_model(
        application, without_elevation, project_uuid, revision, frame, false
    );
    if (with_relief.image.isNull() || without_relief.image.isNull()) {
        std::cerr << "positive non-land material proof produced a null image\n";
        return false;
    }

    const QPoint center(
        with_relief.image.width() / 2,
        with_relief.image.height() / 2
    );
    const QRgb styled = with_relief.image.pixel(center);
    const QRgb base = without_relief.image.pixel(center);
    if (styled == base) {
        std::cerr << "positive non-land proof did not receive numerical relief\n";
        return false;
    }
    if (!(qBlue(styled) > qGreen(styled) && qGreen(styled) > qRed(styled))) {
        std::cerr
            << "positive non-land elevation changed water into a non-water hue: rgb="
            << qRed(styled) << ',' << qGreen(styled) << ',' << qBlue(styled)
            << " elevation=" << *sample << "\n";
        return false;
    }

    // Unit-level guard for both overview and detail paths: the elevation style
    // can only emit neutral grayscale. Material hue necessarily comes from the
    // already-rendered durable surface beneath the Multiply composition.
    for (const double illumination : {-1.0, 0.0, 0.5, 1.0, 2.0}) {
        const QRgb relief =
            aeris::desktop::neutral_elevation_relief_pixel(illumination);
        if (qAlpha(relief) != 255 ||
            qRed(relief) != qGreen(relief) ||
            qGreen(relief) != qBlue(relief)) {
            std::cerr << "numerical elevation relief unexpectedly contains material hue\n";
            return false;
        }
    }

    std::cout
        << "positive non-land material: PASS elevation=" << *sample
        << " rgb=" << qRed(styled) << ',' << qGreen(styled) << ',' << qBlue(styled)
        << "\n";
    return true;
}

[[nodiscard]] bool prove_semantic_surface(
    QApplication& application,
    const std::shared_ptr<const aeris::desktop::ProjectModel>& model,
    const std::string& project_uuid,
    const std::uint64_t revision
) {
    auto without_semantic =
        std::make_shared<aeris::desktop::ProjectModel>(*model);
    const aeris::storage::ProjectLayerRecord* semantic_layer = nullptr;
    std::string classification_source_id;

    for (auto& layer : without_semantic->layers) {
        if (layer.role_id !=
            aeris::storage::kLayerRolePhysicalSurfaceClassificationV1) {
            continue;
        }
        if (semantic_layer != nullptr) {
            std::cerr << "proof project contains multiple semantic surface layers\n";
            return false;
        }
        if (!layer.visible) {
            std::cerr << "semantic surface layer is unexpectedly hidden in proof project\n";
            return false;
        }
        semantic_layer = &layer;
        for (const auto& binding : layer.sources) {
            if (binding.slot_id == "classification") {
                classification_source_id = binding.source_id;
                break;
            }
        }
        layer.visible = false;
    }

    if (semantic_layer == nullptr || classification_source_id.empty()) {
        std::cerr << "proof project lacks the durable semantic surface layer/wiring\n";
        return false;
    }
    const auto source_it = model->sources.find(classification_source_id);
    if (source_it == model->sources.end() || !source_it->second) {
        std::cerr << "semantic surface layer references a missing durable source\n";
        return false;
    }
    const aeris::source::Result& classification = *source_it->second;
    if (!classification.feature_properties_complete || classification.features.empty()) {
        std::cerr << "semantic source lacks a complete non-empty feature-property channel\n";
        return false;
    }

    std::size_t classified_features = 0U;
    for (const aeris::source::Feature& feature : classification.features) {
        const auto value = semantic_class(feature);
        if (!value.has_value() ||
            *value != aeris::surface::SurfaceClass::floating_ice_shelf) {
            std::cerr
                << "semantic source contains a feature without canonical floating_ice_shelf classification\n";
            return false;
        }
        ++classified_features;
    }

    for (const aeris::view::SurfaceMode mode : {
             aeris::view::SurfaceMode::globe,
             aeris::view::SurfaceMode::sinu_mollweide,
         }) {
        aeris::desktop::RenderFrame frame{};
        const double camera_latitude =
            mode == aeris::view::SurfaceMode::globe ? -75.0 : 20.0;
        if (!build_frame(*model, mode, frame, 0.0, camera_latitude)) return false;

        const RenderProof with_semantic = render_model(
            application,
            model,
            project_uuid,
            revision,
            frame,
            false
        );
        const RenderProof without = render_model(
            application,
            without_semantic,
            project_uuid,
            revision,
            frame,
            false
        );
        if (with_semantic.image.isNull() || without.image.isNull()) {
            std::cerr << "semantic surface proof produced a null image\n";
            return false;
        }
        const std::size_t changed = changed_pixels(
            with_semantic.image,
            without.image
        );
        if (changed < kMinimumSemanticChangedPixels) {
            std::cerr
                << aeris::view::surface_mode_name(mode)
                << " semantic classification changed only " << changed
                << " pixels; durable material channel appears disconnected\n";
            return false;
        }
        std::cout
            << aeris::view::surface_mode_name(mode)
            << " semantic surface: PASS classified_features="
            << classified_features
            << " changed_pixels=" << changed << '\n';
    }
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
        ) ||
        !prove_semantic_surface(
            application,
            loaded.model,
            metadata.project_uuid,
            metadata.revision
        ) ||
        !prove_positive_non_land_stays_water_material(
            application,
            loaded.model,
            without_elevation,
            metadata.project_uuid,
            metadata.revision
        )) {
        return EXIT_FAILURE;
    }

    std::cout
        << "aeris-desktop-elevation-render-probe: PASS "
        << "numerical relief + durable semantic materials change Globe/Sinu-Mollweide pixels without elevation-sign material inference\n";
    return EXIT_SUCCESS;
}