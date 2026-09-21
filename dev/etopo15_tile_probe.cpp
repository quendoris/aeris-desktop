// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "etopo15_tile.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

void test_known_noaa_tile() {
    using namespace aeris::desktop;
    const auto tile = etopo15_surface_tile_for_geographic(-44.9, 59.9);
    expect_true("known NOAA tile resolves", tile.has_value());
    if (!tile.has_value()) return;

    expect_true("known NOAA row", tile->row == 2U);
    expect_true("known NOAA column", tile->column == 9U);
    expect_true("known NOAA north", tile->north_deg == 60);
    expect_true("known NOAA west", tile->west_deg == -45);
    expect_true(
        "known NOAA filename",
        tile->filename == "ETOPO_2022_v1_15s_N60W045_surface.tif"
    );
    expect_true(
        "known sparse binding",
        tile->slot_id == "tile:15s:g12x24:r2:c9"
    );
}

void test_boundaries() {
    using namespace aeris::desktop;

    const auto origin_south = etopo15_surface_tile_for_geographic(0.0, -0.1);
    expect_true(
        "south of equator uses N00E000 tile",
        origin_south.has_value() &&
        origin_south->filename == "ETOPO_2022_v1_15s_N00E000_surface.tif" &&
        origin_south->row == 6U &&
        origin_south->column == 12U
    );

    const auto north_of_equator = etopo15_surface_tile_for_geographic(0.0, 0.1);
    expect_true(
        "north of equator uses N15E000 tile",
        north_of_equator.has_value() &&
        north_of_equator->filename == "ETOPO_2022_v1_15s_N15E000_surface.tif" &&
        north_of_equator->row == 5U &&
        north_of_equator->column == 12U
    );

    const auto south_east = etopo15_surface_tile_for_geographic(179.9, -89.9);
    expect_true(
        "south-east corner resolves",
        south_east.has_value() &&
        south_east->filename == "ETOPO_2022_v1_15s_S75E165_surface.tif" &&
        south_east->row == 11U &&
        south_east->column == 23U
    );

    const auto wrapped = etopo15_surface_tile_for_geographic(180.0, 90.0);
    expect_true(
        "+180 wraps to western edge",
        wrapped.has_value() &&
        wrapped->filename == "ETOPO_2022_v1_15s_N90W180_surface.tif" &&
        wrapped->row == 0U &&
        wrapped->column == 0U
    );
}

void test_invalid_input() {
    using namespace aeris::desktop;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    expect_true(
        "non-finite geographic focus is rejected",
        !etopo15_surface_tile_for_geographic(nan, 0.0).has_value()
    );
}

}  // namespace

int main() {
    test_known_noaa_tile();
    test_boundaries();
    test_invalid_input();

    if (failures != 0) {
        std::cerr << failures << " etopo15-tile assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "etopo15_tile_planner: PASS\n";
    return EXIT_SUCCESS;
}
