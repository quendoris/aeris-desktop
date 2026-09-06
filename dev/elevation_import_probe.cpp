// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_import.hpp"

#include "aeris/storage/layer.hpp"
#include "aeris/storage/project.hpp"

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using TiffPtr = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

constexpr std::uint32_t kWidth = 360U;
constexpr std::uint32_t kHeight = 180U;
constexpr std::size_t kExpectedDetailTiles = 72U;
constexpr std::size_t kExpectedElevationBindings = 74U;

[[nodiscard]] TiffPtr open_write(const std::filesystem::path& path) {
#ifdef _WIN32
    return TiffPtr(TIFFOpenW(path.c_str(), "w"), &TIFFClose);
#else
    return TiffPtr(TIFFOpen(path.c_str(), "w"), &TIFFClose);
#endif
}

[[nodiscard]] float sample_value(
    const std::uint32_t x,
    const std::uint32_t y
) noexcept {
    constexpr double pi = 3.141592653589793238462643383279502884;
    const double longitude_deg = -180.0 + static_cast<double>(x) + 0.5;
    const double latitude_deg = 90.0 - static_cast<double>(y) - 0.5;
    const double longitude = longitude_deg * pi / 180.0;
    const double latitude = latitude_deg * pi / 180.0;
    const double broad_relief = 2100.0 * std::sin(latitude) * std::cos(longitude);
    const double secondary = 850.0 * std::cos(2.0 * longitude) * std::cos(latitude);
    const double basin = -1200.0 * std::cos(latitude) * std::cos(latitude);
    return static_cast<float>(broad_relief + secondary + basin);
}

[[nodiscard]] bool write_fixture(const std::filesystem::path& path) {
    TiffPtr tiff = open_write(path);
    if (!tiff ||
        TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, kWidth) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, kHeight) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, 1U) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 32U) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, 15U) != 1) {
        return false;
    }

    std::vector<float> row(kWidth);
    for (std::uint32_t y = 0U; y < kHeight; ++y) {
        for (std::uint32_t x = 0U; x < kWidth; ++x) {
            row[x] = sample_value(x, y);
        }
        if (TIFFWriteScanline(tiff.get(), row.data(), y, 0U) < 0) return false;
    }
    return true;
}

[[nodiscard]] const aeris::storage::ProjectLayerRecord* elevation_layer(
    const std::vector<aeris::storage::ProjectLayerRecord>& layers
) noexcept {
    const auto found = std::find_if(
        layers.begin(),
        layers.end(),
        [](const aeris::storage::ProjectLayerRecord& layer) {
            return layer.role_id == aeris::storage::kLayerRolePhysicalElevationV1;
        }
    );
    return found == layers.end() ? nullptr : &*found;
}

[[nodiscard]] bool verify_elevation_layer(
    const aeris::storage::ProjectStore& project,
    std::string& diagnostic
) {
    const auto listed = aeris::storage::list_project_layers(project);
    if (!listed.ok()) {
        diagnostic = "could not enumerate layers: " + listed.status.diagnostic;
        return false;
    }
    const auto* layer = elevation_layer(listed.records);
    if (layer == nullptr) {
        diagnostic = "durable project does not contain a physical elevation layer";
        return false;
    }
    if (!layer->visible || layer->resources.size() != kExpectedElevationBindings) {
        diagnostic = "physical elevation layer has unexpected visibility/resource count";
        return false;
    }

    const bool has_overview = std::any_of(
        layer->resources.begin(),
        layer->resources.end(),
        [](const aeris::storage::LayerResourceBinding& binding) {
            constexpr std::string_view prefix = "overview:";
            return binding.slot_id.size() > prefix.size() &&
                binding.slot_id.compare(0U, prefix.size(), prefix) == 0;
        }
    );
    const bool has_provenance = std::any_of(
        layer->resources.begin(),
        layer->resources.end(),
        [](const aeris::storage::LayerResourceBinding& binding) {
            return binding.slot_id == "provenance";
        }
    );
    if (!has_overview || !has_provenance) {
        diagnostic = "physical elevation layer lacks overview/provenance bindings";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: aeris-desktop-elevation-import-probe <project.aeris>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path project_path = argv[1];
    const std::filesystem::path fixture_path =
        project_path.parent_path() / "aeris-deterministic-elevation-fixture.tif";
    std::error_code remove_error;
    (void)std::filesystem::remove(fixture_path, remove_error);

    if (!write_fixture(fixture_path)) {
        std::cerr << "could not create deterministic full-world elevation TIFF\n";
        return EXIT_FAILURE;
    }

    auto opened = aeris::storage::ProjectStore::open(project_path);
    if (!opened.ok()) {
        std::cerr << "could not open target project: " << opened.status.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    const auto imported =
        aeris::desktop::testing::import_deterministic_global_elevation_fixture(
            *opened.store,
            fixture_path,
            "2026-09-06T00:00:00.000Z"
        );
    if (!imported.ok() || !imported.changed ||
        imported.detail_tiles != kExpectedDetailTiles) {
        std::cerr << "fixture elevation import failed: " << imported.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    const auto integrity = opened.store->verify_integrity();
    if (!integrity.ok()) {
        std::cerr << "integrity failed after elevation import: "
                  << integrity.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    std::string layer_diagnostic;
    if (!verify_elevation_layer(*opened.store, layer_diagnostic)) {
        std::cerr << layer_diagnostic << '\n';
        return EXIT_FAILURE;
    }

    opened.store.reset();
    if (!std::filesystem::remove(fixture_path, remove_error) || remove_error) {
        std::cerr << "could not delete acquisition-only elevation TIFF\n";
        return EXIT_FAILURE;
    }

    auto reopened = aeris::storage::ProjectStore::open(project_path);
    if (!reopened.ok()) {
        std::cerr << "could not reopen project after deleting TIFF: "
                  << reopened.status.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    const auto reopened_integrity = reopened.store->verify_integrity();
    if (!reopened_integrity.ok()) {
        std::cerr << "reopened project integrity failed: "
                  << reopened_integrity.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    if (!verify_elevation_layer(*reopened.store, layer_diagnostic)) {
        std::cerr << layer_diagnostic << '\n';
        return EXIT_FAILURE;
    }

    // Idempotence is checked after the source file is gone. The durable layer
    // must be sufficient to recognize the completed import without touching the
    // acquisition path again.
    const auto repeated =
        aeris::desktop::testing::import_deterministic_global_elevation_fixture(
            *reopened.store,
            fixture_path,
            "2026-09-06T00:00:01.000Z"
        );
    if (!repeated.ok() || repeated.changed ||
        repeated.detail_tiles != kExpectedDetailTiles) {
        std::cerr << "source-independent repeated import check failed: "
                  << repeated.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    std::cout
        << "aeris-desktop-elevation-import-probe: PASS"
        << " fixture=360x180 detail_tiles=" << imported.detail_tiles
        << " source_deleted=yes reopen=yes idempotent=yes\n";
    return EXIT_SUCCESS;
}
