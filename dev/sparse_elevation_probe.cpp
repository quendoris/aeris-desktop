// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_renderer.hpp"

#include "aeris/elevation/grid.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/view/scene.hpp"

#include <QImage>
#include <QPainter>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

aeris::elevation::ElevationTile sparse_fixture_tile() {
    using namespace aeris;
    elevation::ElevationTile tile{};
    tile.width = 30U;
    tile.height = 30U;
    tile.west_microarcsec = 0LL;
    tile.north_microarcsec = 30LL * 3600LL * 1000000LL;
    tile.longitude_step_microarcsec = 3600LL * 1000000LL;
    tile.latitude_step_microarcsec = 3600LL * 1000000LL;
    tile.vertical_reference = elevation::VerticalReference::egm2008_orthometric;
    tile.samples_m.assign(
        static_cast<std::size_t>(tile.width) * static_cast<std::size_t>(tile.height),
        static_cast<std::int16_t>(321)
    );
    return tile;
}

aeris::storage::ProjectLayerRecord sparse_layer() {
    aeris::storage::ProjectLayerRecord layer{};
    layer.layer_id = "test.elevation.sparse";
    layer.role_id = std::string(aeris::storage::kLayerRolePhysicalElevationV1);
    layer.name = "Sparse elevation";
    layer.visible = true;
    // 6x12 global layout -> each cell spans exactly 30x30 degrees. Only one
    // cell is materialized: row 2, column 6 (0..30 E, 0..30 N).
    layer.resources.push_back({
        "tile:3600s:g6x12:r2:c6",
        "test.elevation.sparse.r2.c6"
    });
    return layer;
}

aeris::desktop::RenderFrame sparse_frame() {
    aeris::desktop::RenderFrame frame{};
    frame.ok = true;
    frame.request.mode = aeris::view::SurfaceMode::globe;
    frame.request.quality = aeris::view::SceneQuality::verified;
    frame.request.camera_longitude_deg = 5.0;
    frame.request.camera_latitude_deg = 5.0;
    frame.request.projection_central_meridian_deg = 0.0;
    return frame;
}

void test_sparse_probe() {
    using namespace aeris::desktop;

    ProjectModel model{};
    model.project_path = "/tmp/aeris-sparse-elevation-fixture.aeris";
    const auto layer = sparse_layer();

    ElevationSurfaceCache cache{};
    expect_true(
        "sparse tile enters cache",
        accept_elevation_detail_tile(
            cache,
            "test.elevation.sparse.r2.c6",
            sparse_fixture_tile()
        )
    );

    const ElevationProbeSample inside =
        probe_elevation_at_geographic(layer, model, cache, 5.0, 5.0);
    expect_true(
        "materialized sparse cell samples",
        inside.detail_m.has_value() &&
        *inside.detail_m == static_cast<std::int16_t>(321) &&
        inside.detail_resource_id == "test.elevation.sparse.r2.c6"
    );

    const ElevationProbeSample outside =
        probe_elevation_at_geographic(layer, model, cache, -45.0, 5.0);
    expect_true(
        "unmaterialized sparse cell is simply absent",
        !outside.detail_m.has_value() &&
        outside.detail_resource_id.empty()
    );
}

void test_sparse_detail_without_overview() {
    using namespace aeris::desktop;

    ProjectModel model{};
    model.project_path = "/tmp/aeris-sparse-elevation-fixture.aeris";
    const auto layer = sparse_layer();
    const auto frame = sparse_frame();

    QImage target(128, 128, QImage::Format_ARGB32_Premultiplied);
    target.fill(Qt::white);

    ElevationSurfaceCache missing_cache{};
    {
        QPainter painter(&target);
        draw_elevation_overview(
            painter,
            layer,
            frame,
            model,
            4.0,
            {},
            false,
            missing_cache
        );
    }
    expect_true(
        "detail-only layer activates without overview",
        missing_cache.detail_lod_active
    );
    expect_true(
        "missing visible sparse tile becomes one async request",
        elevation_detail_requests(missing_cache).size() == 1U &&
        elevation_detail_requests(missing_cache).front() ==
            "test.elevation.sparse.r2.c6"
    );

    ElevationSurfaceCache loaded_cache{};
    (void)accept_elevation_detail_tile(
        loaded_cache,
        "test.elevation.sparse.r2.c6",
        sparse_fixture_tile()
    );
    target.fill(Qt::white);
    {
        QPainter painter(&target);
        draw_elevation_overview(
            painter,
            layer,
            frame,
            model,
            4.0,
            {},
            false,
            loaded_cache
        );
    }
    expect_true(
        "loaded sparse detail contributes rendered samples",
        loaded_cache.detail_lod_active &&
        loaded_cache.detail_samples_used > 0U &&
        !loaded_cache.image.isNull()
    );
    expect_true(
        "loaded sparse tile no longer requests itself",
        elevation_detail_requests(loaded_cache).empty()
    );
}

}  // namespace

int main() {
    test_sparse_probe();
    test_sparse_detail_without_overview();

    if (failures != 0) {
        std::cerr << failures << " sparse-elevation assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "sparse_elevation_grid: PASS\n";
    return EXIT_SUCCESS;
}
