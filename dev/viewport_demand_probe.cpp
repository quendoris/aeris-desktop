// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "viewport_data_demand.hpp"

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

void test_detail_tiers() {
    using namespace aeris::desktop;
    expect_true("overview below 2x",
                viewport_detail_tier_for_zoom(1.99) == ViewportDetailTier::overview);
    expect_true("regional from 2x",
                viewport_detail_tier_for_zoom(2.0) == ViewportDetailTier::regional);
    expect_true("local from 5x",
                viewport_detail_tier_for_zoom(5.0) == ViewportDetailTier::local);
    expect_true("fine from 12x",
                viewport_detail_tier_for_zoom(12.0) == ViewportDetailTier::fine);
}

void test_longitude_normalization() {
    using namespace aeris::desktop;
    expect_true("+180 wraps to -180", normalize_longitude_deg(180.0) == -180.0);
    expect_true("+540 wraps to -180", normalize_longitude_deg(540.0) == -180.0);
    expect_true("-181 wraps to +179", normalize_longitude_deg(-181.0) == 179.0);
}

void test_focus_cell_coalescing() {
    using namespace aeris::desktop;

    ViewportDataDemand first{};
    first.zoom = 6.0;
    first.detail_tier = viewport_detail_tier_for_zoom(first.zoom);
    first.focus_longitude_deg = 13.2;
    first.focus_latitude_deg = 52.4;

    ViewportDataDemand nearby = first;
    nearby.focus_longitude_deg = 18.9;
    nearby.focus_latitude_deg = 58.1;

    const ViewportCoverageKey a = viewport_coverage_key(first);
    const ViewportCoverageKey b = viewport_coverage_key(nearby);
    expect_true("nearby local demands collapse to one cell", a == b);

    ViewportDataDemand moved = first;
    moved.focus_longitude_deg = 35.0;
    const ViewportCoverageKey c = viewport_coverage_key(moved);
    expect_true("crossing local cell creates new key", !(a == c));

    ViewportDataDemand overview = first;
    overview.zoom = 1.0;
    overview.detail_tier = viewport_detail_tier_for_zoom(overview.zoom);
    overview.focus_longitude_deg = -150.0;
    overview.focus_latitude_deg = -70.0;
    const ViewportCoverageKey global = viewport_coverage_key(overview);
    expect_true("overview is one global demand key",
                global.detail_tier == ViewportDetailTier::overview &&
                global.longitude_cell == 0 &&
                global.latitude_cell == 0);
}

}  // namespace

int main() {
    test_detail_tiers();
    test_longitude_normalization();
    test_focus_cell_coalescing();

    if (failures != 0) {
        std::cerr << failures << " viewport-demand assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "viewport_data_demand: PASS\n";
    return EXIT_SUCCESS;
}
