// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/elevation/grid.hpp"

#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aeris::desktop {

struct ElevationDetailLoadResult final {
    std::string resource_id;
    std::optional<elevation::ElevationTile> tile;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return tile.has_value(); }
};

// Owns the storage/decode side of high-zoom terrain LOD. Requests are issued on
// the UI thread, but every ProjectStore open, embedded-resource hash verification
// and tile decode happens in a single bounded worker. A newer viewport request
// invalidates the previous generation, so stale detail cannot arrive after a
// pan/projection/project change and repaint an old view.
class ElevationDetailLoader final : public QObject {
public:
    using ResultCallback = std::function<void(
        std::filesystem::path,
        std::vector<ElevationDetailLoadResult>)>;

    explicit ElevationDetailLoader(QObject* parent = nullptr);
    ~ElevationDetailLoader() override;

    void set_result_callback(ResultCallback callback);
    void request(
        std::filesystem::path project_path,
        std::vector<std::string> resource_ids);
    void cancel();

    [[nodiscard]] bool busy() const noexcept { return task_running_; }

    // Public only as the queued delivery boundary used by the private worker.
    void accept_batch(
        std::uint64_t generation,
        std::filesystem::path project_path,
        std::vector<ElevationDetailLoadResult> results);

private:
    void start_request(
        const std::filesystem::path& project_path,
        const std::vector<std::string>& resource_ids);

    QThreadPool pool_;
    ResultCallback result_callback_;
    std::shared_ptr<std::atomic_bool> cancel_token_;
    std::filesystem::path active_project_path_;
    std::vector<std::string> active_resource_ids_;
    std::uint64_t generation_{0U};
    bool task_running_{false};
};

}  // namespace aeris::desktop
