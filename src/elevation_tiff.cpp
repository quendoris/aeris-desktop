// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_tiff.hpp"

#include <tiffio.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace aeris::desktop {
namespace {

using TiffPtr = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

[[nodiscard]] TiffPtr open_tiff(const std::filesystem::path& path) {
#ifdef _WIN32
    return TiffPtr(TIFFOpenW(path.c_str(), "r"), &TIFFClose);
#else
    return TiffPtr(TIFFOpen(path.c_str(), "r"), &TIFFClose);
#endif
}

[[nodiscard]] Float32TiffReadResult failure(
    const Float32TiffInfo info,
    const std::uint32_t rows_read,
    std::string diagnostic
) {
    return {false, info, rows_read, std::move(diagnostic)};
}

[[nodiscard]] bool checked_product(
    const std::size_t left,
    const std::size_t right,
    std::size_t& product
) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

}  // namespace

Float32TiffReadResult read_single_band_float32_tiff(
    const std::filesystem::path& path,
    const Float32TiffRowConsumer& consume_row
) {
    Float32TiffInfo info{};
    if (path.empty() || !consume_row) {
        return failure(info, 0U, "TIFF reader requires a path and row consumer");
    }

    TiffPtr tiff = open_tiff(path);
    if (!tiff) {
        return failure(info, 0U, "could not open TIFF input");
    }

    std::uint16_t samples_per_pixel = 1U;
    std::uint16_t bits_per_sample = 0U;
    std::uint16_t sample_format = SAMPLEFORMAT_UINT;
    std::uint16_t planar_config = PLANARCONFIG_CONTIG;
    std::uint16_t orientation = ORIENTATION_TOPLEFT;
    std::uint16_t compression = COMPRESSION_NONE;

    if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &info.width) != 1 ||
        TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &info.height) != 1) {
        return failure(info, 0U, "TIFF is missing image dimensions");
    }
    (void)TIFFGetFieldDefaulted(
        tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
    (void)TIFFGetFieldDefaulted(
        tiff.get(), TIFFTAG_BITSPERSAMPLE, &bits_per_sample);
    (void)TIFFGetFieldDefaulted(
        tiff.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
    (void)TIFFGetFieldDefaulted(
        tiff.get(), TIFFTAG_PLANARCONFIG, &planar_config);
    (void)TIFFGetFieldDefaulted(
        tiff.get(), TIFFTAG_ORIENTATION, &orientation);
    (void)TIFFGetFieldDefaulted(
        tiff.get(), TIFFTAG_COMPRESSION, &compression);
    info.compression = compression;

    if (info.width == 0U || info.height == 0U) {
        return failure(info, 0U, "TIFF dimensions are empty");
    }
    if (samples_per_pixel != 1U || bits_per_sample != 32U ||
        sample_format != SAMPLEFORMAT_IEEEFP ||
        planar_config != PLANARCONFIG_CONTIG) {
        return failure(
            info,
            0U,
            "TIFF must be one contiguous IEEE float32 sample per pixel"
        );
    }
    if (orientation != ORIENTATION_TOPLEFT) {
        return failure(
            info,
            0U,
            "TIFF orientation must be top-left for geographic row streaming"
        );
    }

    const std::size_t width = static_cast<std::size_t>(info.width);
    std::size_t expected_scanline_bytes = 0U;
    if (!checked_product(width, sizeof(float), expected_scanline_bytes)) {
        return failure(info, 0U, "TIFF scanline size overflows process address space");
    }

    std::uint32_t rows_read = 0U;
    std::string consumer_diagnostic;

    if (TIFFIsTiled(tiff.get()) == 0) {
        const tmsize_t scanline_bytes = TIFFScanlineSize(tiff.get());
        if (scanline_bytes <= 0 ||
            static_cast<std::uint64_t>(scanline_bytes) !=
                static_cast<std::uint64_t>(expected_scanline_bytes)) {
            return failure(info, 0U, "TIFF decoded scanline size is not width*sizeof(float)");
        }

        std::vector<float> row(width);
        for (std::uint32_t y = 0U; y < info.height; ++y) {
            if (TIFFReadScanline(tiff.get(), row.data(), y, 0U) < 0) {
                return failure(info, rows_read, "libtiff failed to decode a scanline");
            }
            consumer_diagnostic.clear();
            if (!consume_row(y, row.data(), row.size(), consumer_diagnostic)) {
                if (consumer_diagnostic.empty()) {
                    consumer_diagnostic = "TIFF row consumer rejected decoded samples";
                }
                return failure(info, rows_read, std::move(consumer_diagnostic));
            }
            ++rows_read;
        }
        return {true, info, rows_read, {}};
    }

    std::uint32_t tile_width = 0U;
    std::uint32_t tile_height = 0U;
    if (TIFFGetField(tiff.get(), TIFFTAG_TILEWIDTH, &tile_width) != 1 ||
        TIFFGetField(tiff.get(), TIFFTAG_TILELENGTH, &tile_height) != 1 ||
        tile_width == 0U || tile_height == 0U) {
        return failure(info, 0U, "tiled TIFF is missing canonical tile dimensions");
    }

    std::size_t tile_samples = 0U;
    if (!checked_product(
            static_cast<std::size_t>(tile_width),
            static_cast<std::size_t>(tile_height),
            tile_samples)) {
        return failure(info, 0U, "TIFF tile sample count overflows process address space");
    }
    std::size_t tile_bytes_expected = 0U;
    if (!checked_product(tile_samples, sizeof(float), tile_bytes_expected)) {
        return failure(info, 0U, "TIFF tile byte count overflows process address space");
    }
    const tmsize_t tile_bytes = TIFFTileSize(tiff.get());
    if (tile_bytes <= 0 ||
        static_cast<std::uint64_t>(tile_bytes) !=
            static_cast<std::uint64_t>(tile_bytes_expected)) {
        return failure(info, 0U, "TIFF decoded tile size is not tile_width*tile_height*sizeof(float)");
    }

    std::vector<float> tile(tile_samples);
    for (std::uint32_t tile_y = 0U; tile_y < info.height; tile_y += tile_height) {
        const std::uint32_t band_rows = std::min(tile_height, info.height - tile_y);
        std::size_t band_samples = 0U;
        if (!checked_product(
                width,
                static_cast<std::size_t>(band_rows),
                band_samples)) {
            return failure(info, rows_read, "TIFF decoded tile band exceeds process address space");
        }
        std::vector<float> band(band_samples);

        for (std::uint32_t tile_x = 0U; tile_x < info.width; tile_x += tile_width) {
            const tmsize_t decoded = TIFFReadTile(
                tiff.get(),
                tile.data(),
                tile_x,
                tile_y,
                0U,
                0U
            );
            if (decoded < 0 ||
                static_cast<std::uint64_t>(decoded) !=
                    static_cast<std::uint64_t>(tile_bytes_expected)) {
                return failure(info, rows_read, "libtiff failed to decode a tile");
            }

            const std::uint32_t copy_columns =
                std::min(tile_width, info.width - tile_x);
            for (std::uint32_t local_y = 0U; local_y < band_rows; ++local_y) {
                const float* source = tile.data() +
                    static_cast<std::size_t>(local_y) *
                        static_cast<std::size_t>(tile_width);
                float* destination = band.data() +
                    static_cast<std::size_t>(local_y) * width +
                    static_cast<std::size_t>(tile_x);
                std::copy_n(source, static_cast<std::size_t>(copy_columns), destination);
            }
        }

        for (std::uint32_t local_y = 0U; local_y < band_rows; ++local_y) {
            const std::uint32_t y = tile_y + local_y;
            const float* row = band.data() +
                static_cast<std::size_t>(local_y) * width;
            consumer_diagnostic.clear();
            if (!consume_row(y, row, width, consumer_diagnostic)) {
                if (consumer_diagnostic.empty()) {
                    consumer_diagnostic = "TIFF row consumer rejected decoded samples";
                }
                return failure(info, rows_read, std::move(consumer_diagnostic));
            }
            ++rows_read;
        }
    }

    return {true, info, rows_read, {}};
}

}  // namespace aeris::desktop
