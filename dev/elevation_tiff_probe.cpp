// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_tiff.hpp"

#include <tiffio.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using TiffPtr = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

[[nodiscard]] TiffPtr open_write(const std::filesystem::path& path) {
#ifdef _WIN32
    return TiffPtr(TIFFOpenW(path.c_str(), "w"), &TIFFClose);
#else
    return TiffPtr(TIFFOpen(path.c_str(), "w"), &TIFFClose);
#endif
}

[[nodiscard]] float sample_value(const std::uint32_t x, const std::uint32_t y) noexcept {
    return static_cast<float>(static_cast<double>(y) * 1000.0 +
                              static_cast<double>(x) + 0.25);
}

[[nodiscard]] bool configure_common(
    TIFF* tiff,
    const std::uint32_t width,
    const std::uint32_t height
) {
    return TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width) == 1 &&
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height) == 1 &&
        TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1U) == 1 &&
        TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 32U) == 1 &&
        TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP) == 1 &&
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) == 1 &&
        TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) == 1 &&
        TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK) == 1 &&
        TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) == 1;
}

[[nodiscard]] bool write_strip_fixture(const std::filesystem::path& path) {
    constexpr std::uint32_t width = 32U;
    constexpr std::uint32_t height = 16U;
    TiffPtr tiff = open_write(path);
    if (!tiff || !configure_common(tiff.get(), width, height) ||
        TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, 4U) != 1) {
        return false;
    }

    std::vector<float> row(width);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            row[x] = sample_value(x, y);
        }
        if (TIFFWriteScanline(tiff.get(), row.data(), y, 0U) < 0) return false;
    }
    return true;
}

[[nodiscard]] bool write_tiled_fixture(const std::filesystem::path& path) {
    constexpr std::uint32_t width = 32U;
    constexpr std::uint32_t height = 32U;
    constexpr std::uint32_t tile_width = 16U;
    constexpr std::uint32_t tile_height = 16U;
    TiffPtr tiff = open_write(path);
    if (!tiff || !configure_common(tiff.get(), width, height) ||
        TIFFSetField(tiff.get(), TIFFTAG_TILEWIDTH, tile_width) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_TILELENGTH, tile_height) != 1) {
        return false;
    }

    std::vector<float> tile(
        static_cast<std::size_t>(tile_width) * static_cast<std::size_t>(tile_height));
    for (std::uint32_t tile_y = 0U; tile_y < height; tile_y += tile_height) {
        for (std::uint32_t tile_x = 0U; tile_x < width; tile_x += tile_width) {
            for (std::uint32_t local_y = 0U; local_y < tile_height; ++local_y) {
                for (std::uint32_t local_x = 0U; local_x < tile_width; ++local_x) {
                    const std::size_t index =
                        static_cast<std::size_t>(local_y) * tile_width + local_x;
                    tile[index] = sample_value(tile_x + local_x, tile_y + local_y);
                }
            }
            const tmsize_t written = TIFFWriteTile(
                tiff.get(), tile.data(), tile_x, tile_y, 0U, 0U);
            if (written < 0) return false;
        }
    }
    return true;
}

[[nodiscard]] bool verify_fixture(
    const std::filesystem::path& path,
    const std::uint32_t expected_height,
    const bool expected_tiled
) {
    constexpr std::uint32_t expected_width = 32U;
    const auto inspected = aeris::desktop::inspect_single_band_float32_tiff(path);
    if (!inspected.ok() || inspected.info.width != expected_width ||
        inspected.info.height != expected_height ||
        inspected.info.tiled != expected_tiled ||
        inspected.info.compression != COMPRESSION_ADOBE_DEFLATE) {
        std::cerr << "TIFF preflight mismatch: " << inspected.diagnostic << '\n';
        return false;
    }

    std::uint32_t seen_rows = 0U;
    const auto read = aeris::desktop::read_single_band_float32_tiff(
        path,
        [&](const std::uint32_t y,
            const float* samples,
            const std::size_t count,
            std::string& diagnostic) {
            if (samples == nullptr || count != expected_width || y != seen_rows) {
                diagnostic = "row identity/count mismatch";
                return false;
            }
            for (std::uint32_t x = 0U; x < expected_width; ++x) {
                if (std::abs(samples[x] - sample_value(x, y)) > 1e-6F) {
                    diagnostic = "decoded Float32 value differs from written value";
                    return false;
                }
            }
            ++seen_rows;
            return true;
        });
    if (!read.ok() || read.rows_read != expected_height ||
        read.info.tiled != expected_tiled || seen_rows != expected_height) {
        std::cerr << "TIFF streaming mismatch: " << read.diagnostic << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("aeris-elevation-tiff-probe-" + std::to_string(stamp));
    std::error_code error;
    if (!std::filesystem::create_directories(root, error) || error) {
        std::cerr << "could not create TIFF probe directory\n";
        return EXIT_FAILURE;
    }

    struct Cleanup final {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code ignored;
            (void)std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};

    const std::filesystem::path strip = root / "strip-deflate.tif";
    const std::filesystem::path tiled = root / "tiled-deflate.tif";
    if (!write_strip_fixture(strip) || !verify_fixture(strip, 16U, false)) {
        std::cerr << "aeris-desktop-elevation-tiff-probe: FAIL strip\n";
        return EXIT_FAILURE;
    }
    if (!write_tiled_fixture(tiled) || !verify_fixture(tiled, 32U, true)) {
        std::cerr << "aeris-desktop-elevation-tiff-probe: FAIL tiled\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "aeris-desktop-elevation-tiff-probe: PASS"
        << " strip=32x16 tiled=32x32 compression=deflate\n";
    return EXIT_SUCCESS;
}
