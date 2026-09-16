// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "data_job_progress.hpp"

#include <filesystem>
#include <string>

namespace aeris::desktop {

struct NaturalEarthAcquisitionResult final {
    bool success{false};
    std::filesystem::path snapshot_root;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Materializes the exact pinned Natural Earth world snapshot into a machine-
// local acquisition cache. Large resources download through immutable upstream
// commit URLs into sibling .part files and resume with HTTP Range after an
// interruption. A completed resource is published only after exact SHA-256
// verification. The returned directory is acquisition-only and may be deleted
// after the project importer commits canonical data into .aeris.
[[nodiscard]] NaturalEarthAcquisitionResult acquire_natural_earth_110m_world(
    const std::filesystem::path& cache_root,
    const DataJobProgressCallback& progress = {}
);

}  // namespace aeris::desktop
