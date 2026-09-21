// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "etopo15_import.hpp"
#include "etopo15_tile.hpp"

#include "aeris/elevation/grid.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/storage/resource.hpp"

#include <tiffio.h>

#include <algorithm>
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

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] TiffPtr open_write(const std::filesystem::path& path) {
#ifdef _WIN32
    return TiffPtr(TIFFOpenW(path.c_str(), "w"), &TIFFClose);
#else
    return TiffPtr(TIFFOpen(path.c_str(), "w"), &TIFFClose);
#endif
}

[[nodiscard]] bool write_etopo_tile_fixture(
    const std::filesystem::path& path
) {
    constexpr std::uint32_t width = aeris::desktop::kEtopo15TilePixels;
    constexpr std::uint32_t height = aeris::desktop::kEtopo15TilePixels;

    TiffPtr tiff = open_write(path);
    if (!tiff ||
        TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, width) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, height) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, 1U) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 32U) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, 32U) != 1) {
        return false;
    }

    std::vector<float> row(width, 321.0F);
    for (std::uint32_t y = 0U; y < height; ++y) {
        if (TIFFWriteScanline(tiff.get(), row.data(), y, 0U) < 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<std::uint8_t> embedded_bytes(
    const aeris::storage::ProjectStore& project,
    const std::string& resource_id
) {
    std::vector<std::uint8_t> bytes;
    const aeris::storage::Status status =
        aeris::storage::stream_embedded_resource(
            project,
            resource_id,
            [&](const void* data, const std::size_t size) {
                const auto* first =
                    static_cast<const std::uint8_t*>(data);
                bytes.insert(bytes.end(), first, first + size);
                return aeris::storage::Status::success();
            }
        );
    if (!status.ok()) bytes.clear();
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr
            << "usage: aeris-desktop-etopo15-import-probe <base-world.aeris>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path input_project(argv[1]);
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "aeris-etopo15-import-probe";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error) {
        std::cerr << "could not create ETOPO 15s import probe directory\n";
        return EXIT_FAILURE;
    }

    struct Cleanup final {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};

    const std::filesystem::path project_path = root / "world.aeris";
    std::filesystem::copy_file(
        input_project,
        project_path,
        std::filesystem::copy_options::overwrite_existing,
        error
    );
    if (error) {
        std::cerr << "could not copy base world fixture: " << error.message() << '\n';
        return EXIT_FAILURE;
    }

    const auto tile =
        aeris::desktop::etopo15_surface_tile_for_indices(2U, 9U);
    expect_true("fixture tile descriptor exists", tile.has_value());
    if (!tile.has_value()) return EXIT_FAILURE;

    const std::filesystem::path tiff_path = root / tile->filename;
    expect_true(
        "3600x3600 compressed Float32 fixture writes",
        write_etopo_tile_fixture(tiff_path)
    );

    auto opened = aeris::storage::ProjectStore::open(project_path);
    expect_true("copied base world opens", opened.ok());
    if (!opened.ok()) return EXIT_FAILURE;

    const auto imported =
        aeris::desktop::import_etopo2022_surface_15s_tile(
            *opened.store,
            *tile,
            tiff_path,
            "2026-09-21T09:45:00Z"
        );
    expect_true(
        "ETOPO 15s tile imports",
        imported.ok() && imported.changed
    );
    expect_true(
        "project remains integral after tile import",
        opened.store->verify_integrity().ok()
    );

    opened.store.reset();
    std::filesystem::remove(tiff_path, error);
    error.clear();

    auto reopened = aeris::storage::ProjectStore::open(project_path);
    expect_true("project reopens after source TIFF deletion", reopened.ok());
    if (!reopened.ok()) return EXIT_FAILURE;
    expect_true(
        "reopened project passes deep integrity",
        reopened.store->verify_integrity().ok()
    );

    const auto layers =
        aeris::storage::list_project_layers(*reopened.store);
    expect_true("reopened layer graph lists", layers.ok());
    if (layers.ok()) {
        const auto layer = std::find_if(
            layers.records.begin(),
            layers.records.end(),
            [&](const aeris::storage::ProjectLayerRecord& record) {
                return record.layer_id == tile->layer_id;
            }
        );
        expect_true("streamed terrain layer persists", layer != layers.records.end());
        if (layer != layers.records.end()) {
            const bool has_tile = std::any_of(
                layer->resources.begin(),
                layer->resources.end(),
                [&](const aeris::storage::LayerResourceBinding& binding) {
                    return binding.slot_id == tile->slot_id &&
                        binding.resource_id == tile->resource_id;
                }
            );
            expect_true("streamed tile binding persists", has_tile);
        }
    }

    const auto resources =
        aeris::storage::list_project_resources(*reopened.store);
    expect_true("reopened resources list", resources.ok());
    if (resources.ok()) {
        const auto canonical = std::find_if(
            resources.records.begin(),
            resources.records.end(),
            [&](const aeris::storage::ProjectResourceRecord& record) {
                return record.identity.resource_id == tile->resource_id;
            }
        );
        expect_true(
            "canonical tile is embedded and required",
            canonical != resources.records.end() &&
            canonical->storage_mode ==
                aeris::storage::ResourceStorageMode::embedded &&
            canonical->identity.required_for_reproduction
        );

        const std::string source_id =
            "source.etopo2022.v1.15s.surface.r2.c9";
        const auto source = std::find_if(
            resources.records.begin(),
            resources.records.end(),
            [&](const aeris::storage::ProjectResourceRecord& record) {
                return record.identity.resource_id == source_id;
            }
        );
        expect_true(
            "exact source identity survives without source bytes",
            source != resources.records.end() &&
            source->storage_mode ==
                aeris::storage::ResourceStorageMode::external &&
            !source->identity.required_for_reproduction &&
            source->identity.retrieval_uri == tile->url &&
            source->identity.sha256.size() == 64U
        );
    }

    const std::vector<std::uint8_t> bytes =
        embedded_bytes(*reopened.store, tile->resource_id);
    expect_true("canonical tile bytes stream from aeris", !bytes.empty());
    const auto decoded =
        aeris::elevation::decode_elevation_tile_v1(bytes);
    expect_true("canonical tile decodes after reopen", decoded.ok());
    if (decoded.ok()) {
        const auto& stored = *decoded.tile;
        expect_true(
            "canonical tile georeferencing persists",
            stored.width == aeris::desktop::kEtopo15TilePixels &&
            stored.height == aeris::desktop::kEtopo15TilePixels &&
            stored.west_microarcsec == -45LL * 3600LL * 1000000LL &&
            stored.north_microarcsec == 60LL * 3600LL * 1000000LL &&
            stored.longitude_step_microarcsec == 15LL * 1000000LL &&
            stored.latitude_step_microarcsec == 15LL * 1000000LL
        );
        expect_true(
            "canonical tile samples persist",
            !stored.samples_m.empty() &&
            stored.samples_m.front() == static_cast<std::int16_t>(321) &&
            stored.samples_m.back() == static_cast<std::int16_t>(321)
        );
    }

    const auto retry =
        aeris::desktop::import_etopo2022_surface_15s_tile(
            *reopened.store,
            *tile,
            // Source bytes were deliberately deleted. Exact project retry
            // should therefore not be attempted through the importer; the
            // durable layer/resource state itself is the idempotence proof.
            root / "missing-source.tif",
            "2026-09-21T09:45:01Z"
        );
    expect_true(
        "missing source is not silently accepted as a new import",
        !retry.ok()
    );

    if (failures != 0) {
        std::cerr
            << failures
            << " ETOPO 15s import assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "etopo15_import: PASS"
        << " slot=" << tile->slot_id
        << " canonical_bytes=" << bytes.size()
        << "\n";
    return EXIT_SUCCESS;
}
