// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/storage/project.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace aeris::desktop {

struct FlagPackImportResult final {
    bool success{false};
    bool changed{false};
    std::size_t flag_count{0U};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Imports a directory of ISO-3166 alpha-2 SVG files (e.g. ru.svg, us.svg).
// Every flag used by the current durable political source is embedded in the
// .aeris project as a content-addressed resource before the Country flags layer
// is attached. The caller's source directory is never persisted as project state.
[[nodiscard]] FlagPackImportResult import_country_flag_svg_pack(
    storage::ProjectStore& project,
    const std::filesystem::path& pack_root,
    std::string_view modified_utc);

}  // namespace aeris::desktop
