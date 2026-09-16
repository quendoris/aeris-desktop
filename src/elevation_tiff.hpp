// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace aeris::desktop {

struct Float32TiffInfo final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint16_t compression{0U};
    bool tiled{false};
};

struct Float32TiffInspectResult final {
    bool success{false};
    Float32TiffInfo info{};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

using Float32TiffRowConsumer = std::function<bool(
    std::uint32_t row,
    const float* samples,
    std::size_t count,
    std::string& diagnostic)>;

struct Float32TiffReadResult final {
    bool success{false};
    Float32TiffInfo info{};
    std::uint32_t rows_read{0U};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Performs the cheap structural pass before any durable import begins.
[[nodiscard]] Float32TiffInspectResult inspect_single_band_float32_tiff(
    const std::filesystem::path& path);

// Reads a single-band, top-left oriented IEEE float32 TIFF without exposing its
// compression/layout details to the elevation importer. libtiff performs strip
// or tile decompression; AERIS consumes one decoded scanline at a time.
[[nodiscard]] Float32TiffReadResult read_single_band_float32_tiff(
    const std::filesystem::path& path,
    const Float32TiffRowConsumer& consume_row);

}  // namespace aeris::desktop
