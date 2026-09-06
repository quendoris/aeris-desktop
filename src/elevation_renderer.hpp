// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "project_model.hpp"
#include "scene_controller.hpp"

#include "aeris/elevation/grid.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/view/scene.hpp"

#include <QImage>
#include <QPointF>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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

    // The UI thread owns only decoded tiles already delivered by the background
    // loader. paintEvent never opens ProjectStore or streams SQLite blobs. A
    // render records the missing resource ids it would benefit from; MapView
    // sends that bounded set to ElevationDetailLoader after painting and uses
    // the durable overview until the verified tile batch arrives.
    std::vector<ElevationDetailTileCacheEntry> detail_tiles;
    std::vector<std::string> detail_pending_resources;
    std::unordered_set<std::string> detail_failed_resources;
    std::uint64_t detail_use_clock{0U};
    std::uint64_t detail_render_epoch{0U};
    std::size_t detail_tile_loads{0U};
    std::size_t detail_samples_used{0U};
    bool detail_lod_active{false};
};

// Reprojects durable numerical elevation into the exact surface currently
// rendered by MapView. Whole-world navigation uses the eagerly reconstructed
// overview. At sufficiently high zoom, already-delivered detail tiles are used
// directly and missing ids are exposed as non-blocking background requests.
void draw_elevation_overview(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const RenderFrame& frame,
    const ProjectModel& model,
    double zoom,
    QPointF pan,
    ElevationSurfaceCache& cache);

[[nodiscard]] const std::vector<std::string>& elevation_detail_requests(
    const ElevationSurfaceCache& cache) noexcept;

// Called only on the UI thread after the background loader has opened the
// project, hash-verified the embedded resource and decoded the numerical tile.
// Returns true when the bounded cache changed and the screen cache must rebuild.
[[nodiscard]] bool accept_elevation_detail_tile(
    ElevationSurfaceCache& cache,
    std::string resource_id,
    elevation::ElevationTile tile);

void reject_elevation_detail_resource(
    ElevationSurfaceCache& cache,
    std::string_view resource_id);

}  // namespace aeris::desktop
