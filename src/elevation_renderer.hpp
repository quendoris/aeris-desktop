// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "project_model.hpp"
#include "scene_controller.hpp"

#include "aeris/elevation/grid.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/view/scene.hpp"

#include <QImage>
#include <QPointF>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class QPainter;

namespace aeris::desktop {

inline constexpr double kElevationDetailLodZoom = 3.0;
inline constexpr std::size_t kElevationDetailCacheTileLimit = 16U;

struct ElevationDetailTileCacheEntry final {
    std::string resource_id;
    elevation::ElevationTile tile;
    std::uint64_t last_used{0U};
    std::uint64_t render_epoch{0U};
};

struct ElevationSurfaceCache final {
    const ProjectModel* model{nullptr};
    std::string layer_id;
    view::SurfaceMode mode{view::SurfaceMode::globe};
    double camera_longitude_deg{0.0};
    double camera_latitude_deg{0.0};
    double projection_central_meridian_deg{0.0};
    double zoom{0.0};
    QPointF pan{};
    int width{0};
    int height{0};
    QImage image;

    // Detail state is deliberately separate from the discardable screen-space
    // image. At high zoom a second read-only handle opens the same durable
    // .aeris file and only requested numerical tiles are decoded. The raw tile
    // cache has a hard entry bound; a render never evicts a tile already used in
    // the same epoch, so an unexpectedly wide viewport falls back to overview
    // instead of thrashing through the entire world dataset.
    std::filesystem::path detail_project_path;
    std::unique_ptr<storage::ProjectStore> detail_store;
    std::vector<ElevationDetailTileCacheEntry> detail_tiles;
    std::unordered_set<std::string> detail_failed_resources;
    std::uint64_t detail_use_clock{0U};
    std::uint64_t detail_render_epoch{0U};
    std::size_t detail_tile_loads{0U};
    std::size_t detail_samples_used{0U};
    bool detail_lod_active{false};
};

// Reprojects durable numerical elevation into the exact surface currently
// rendered by MapView. Whole-world navigation uses the eagerly reconstructed
// overview. At sufficiently high zoom the same path lazily samples bounded
// detail tiles directly from .aeris; both representations are derived frontend
// presentation and numerical elevation remains the only durable state.
void draw_elevation_overview(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const RenderFrame& frame,
    const ProjectModel& model,
    double zoom,
    QPointF pan,
    ElevationSurfaceCache& cache);

}  // namespace aeris::desktop
