// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_tiff.hpp"

#include <tiffio.h>

#include <limits>
#include <memory>

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

[[nodiscard]] Float32TiffInspectResult failure(
    const Float32TiffInfo info,
    std::string diagnostic
) {
    return {false, info, std::move(diagnostic)};
}

[[nodiscard]] bool expected_byte_count(
    const std::uint64_t width,
    const std::uint64_t height,
    std::uint64_t& bytes
) noexcept {
    if (width != 0U && height > std::numeric_limits<std::uint64_t>::max() / width) {
        return false;
    }
    const std::uint64_t samples = width * height;
    if (samples > std::numeric_limits<std::uint64_t>::max() / sizeof(float)) {
        return false;
    }
    bytes = samples * sizeof(float);
    return true;
}

}  // namespace

Float32TiffInspectResult inspect_single_band_float32_tiff(
    const std::filesystem::path& path
) {
    Float32TiffInfo info{};
    if (path.empty()) return failure(info, "TIFF inspection requires a path");

    TiffPtr tiff = open_tiff(path);
    if (!tiff) return failure(info, "could not open TIFF input");

    std::uint16_t samples_per_pixel = 1U;
    std::uint16_t bits_per_sample = 0U;
    std::uint16_t sample_format = SAMPLEFORMAT_UINT;
    std::uint16_t planar_config = PLANARCONFIG_CONTIG;
    std::uint16_t orientation = ORIENTATION_TOPLEFT;
    std::uint16_t compression = COMPRESSION_NONE;

    if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &info.width) != 1 ||
        TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &info.height) != 1) {
        return failure(info, "TIFF is missing image dimensions");
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
    info.tiled = TIFFIsTiled(tiff.get()) != 0;

    if (info.width == 0U || info.height == 0U) {
        return failure(info, "TIFF dimensions are empty");
    }
    if (samples_per_pixel != 1U || bits_per_sample != 32U ||
        sample_format != SAMPLEFORMAT_IEEEFP ||
        planar_config != PLANARCONFIG_CONTIG) {
        return failure(
            info,
            "TIFF must be one contiguous IEEE float32 sample per pixel"
        );
    }
    if (orientation != ORIENTATION_TOPLEFT) {
        return failure(
            info,
            "TIFF orientation must be top-left for geographic row streaming"
        );
    }

    if (!info.tiled) {
        std::uint64_t expected = 0U;
        if (!expected_byte_count(info.width, 1U, expected)) {
            return failure(info, "TIFF scanline byte count overflows canonical bounds");
        }
        const tmsize_t actual = TIFFScanlineSize(tiff.get());
        if (actual <= 0 || static_cast<std::uint64_t>(actual) != expected) {
            return failure(info, "TIFF decoded scanline size is not width*sizeof(float)");
        }
        return {true, info, {}};
    }

    std::uint32_t tile_width = 0U;
    std::uint32_t tile_height = 0U;
    if (TIFFGetField(tiff.get(), TIFFTAG_TILEWIDTH, &tile_width) != 1 ||
        TIFFGetField(tiff.get(), TIFFTAG_TILELENGTH, &tile_height) != 1 ||
        tile_width == 0U || tile_height == 0U) {
        return failure(info, "tiled TIFF is missing tile dimensions");
    }

    std::uint64_t expected = 0U;
    if (!expected_byte_count(tile_width, tile_height, expected)) {
        return failure(info, "TIFF tile byte count overflows canonical bounds");
    }
    const tmsize_t actual = TIFFTileSize(tiff.get());
    if (actual <= 0 || static_cast<std::uint64_t>(actual) != expected) {
        return failure(
            info,
            "TIFF decoded tile size is not tile_width*tile_height*sizeof(float)"
        );
    }
    return {true, info, {}};
}

}  // namespace aeris::desktop
