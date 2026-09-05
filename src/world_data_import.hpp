// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace aeris::desktop {

struct WorldDataImportResult final {
    bool success{false};
    bool changed{false};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Import the separately downloaded, exact Natural Earth v5.1.2 110m
// physical-land + admin0 pack into an already-created editable .aeris project.
// The acquisition directory is only an input boundary: successful recording
// copies normalized geometry, properties and provenance into durable project
// storage, so rendering never depends on the source directory afterwards.
[[nodiscard]] WorldDataImportResult import_natural_earth_110m_world(
    storage::ProjectStore& project,
    const std::filesystem::path& source_root,
    std::string_view modified_utc
);

}  // namespace aeris::desktop
