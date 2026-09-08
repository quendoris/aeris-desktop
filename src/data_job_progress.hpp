// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace aeris::desktop {

struct DataJobProgress final {
    std::uint64_t current{0U};
    std::uint64_t total{0U};
    std::string phase;

    [[nodiscard]] int percent() const noexcept {
        if (total == 0U) return -1;
        const std::uint64_t bounded = current > total ? total : current;
        return static_cast<int>((bounded * 100U) / total);
    }
};

using DataJobProgressCallback = std::function<void(const DataJobProgress&)>;

inline void report_data_job_progress(
    const DataJobProgressCallback& callback,
    const std::uint64_t current,
    const std::uint64_t total,
    std::string phase
) {
    if (!callback) return;
    callback(DataJobProgress{current, total, std::move(phase)});
}

}  // namespace aeris::desktop
