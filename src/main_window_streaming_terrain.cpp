// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "data_job_process.hpp"
#include "etopo15_tile.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStatusBar>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace aeris::desktop {
namespace {

[[nodiscard]] std::filesystem::path filesystem_path_from_qt_streaming(
    const QString& path
) {
    return QFile(path).filesystemFileName();
}

[[nodiscard]] std::filesystem::path etopo15_cache_path() {
    const QString cache_root =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache_root.isEmpty()) return {};
    return filesystem_path_from_qt_streaming(
        QDir(cache_root).filePath(
            QStringLiteral("etopo-2022-v1-15s-surface")
        )
    );
}

[[nodiscard]] bool layer_has_tile(
    const ProjectModel& model,
    const Etopo15SurfaceTileDescriptor& tile
) noexcept {
    const auto layer = std::find_if(
        model.layers.begin(),
        model.layers.end(),
        [&](const storage::ProjectLayerRecord& record) {
            return record.layer_id == tile.layer_id;
        }
    );
    if (layer == model.layers.end()) return false;

    return std::any_of(
        layer->resources.begin(),
        layer->resources.end(),
        [&](const storage::LayerResourceBinding& binding) {
            return binding.slot_id == tile.slot_id &&
                binding.resource_id == tile.resource_id;
        }
    );
}

}  // namespace

bool MainWindow::request_streamed_etopo15(
    const ViewportDataDemand& demand
) {
    if (!project_ || !model_ || project_->metadata().frozen) return false;

    const auto tile = etopo15_surface_tile_for_geographic(
        demand.focus_longitude_deg,
        demand.focus_latitude_deg
    );
    if (!tile.has_value()) return false;

    if (layer_has_tile(*model_, *tile)) {
        if (blocked_streaming_tile_slot_.has_value() &&
            *blocked_streaming_tile_slot_ == tile->slot_id) {
            blocked_streaming_tile_slot_.reset();
        }
        return true;
    }

    if (blocked_streaming_tile_slot_.has_value()) {
        if (*blocked_streaming_tile_slot_ == tile->slot_id) {
            // A failed/cancelled tile does not immediately retry forever while
            // the viewport is stationary. Leaving this NOAA cell and returning
            // to it clears the block and gives the user a natural retry path.
            return true;
        }
        blocked_streaming_tile_slot_.reset();
    }

    if (data_job_ != nullptr) {
        // One project writer at a time. Completion of the current job refreshes
        // the same ProjectModel and emits the latest viewport demand again.
        return true;
    }

    const std::filesystem::path cache_root = etopo15_cache_path();
    if (cache_root.empty()) {
        blocked_streaming_tile_slot_ = tile->slot_id;
        statusBar()->showMessage(
            QStringLiteral(
                "Viewport terrain unavailable · application cache directory could not be resolved"
            ),
            7000
        );
        return true;
    }

    const std::filesystem::path project_path = project_->path();
    const std::string slot_id = tile->slot_id;
    const std::string operation =
        "etopo15-auto-surface-r" +
        std::to_string(tile->row) +
        "-c" +
        std::to_string(tile->column);

    auto* job = new DataJobProcess(this);
    data_job_ = job;
    begin_data_job_ui(
        job,
        QStringLiteral("Preparing ETOPO 15″ terrain tile…")
    );
    refresh_project_ui();
    statusBar()->showMessage(
        QStringLiteral(
            "Viewport terrain r%1 c%2 · NOAA ETOPO 2022 15″ · background worker"
        )
            .arg(static_cast<qulonglong>(tile->row))
            .arg(static_cast<qulonglong>(tile->column))
    );

    const bool started = job->start(
        operation,
        project_path,
        cache_root,
        QDateTime::currentDateTimeUtc()
            .toString(Qt::ISODate)
            .toStdString(),
        [this, job, project_path, slot_id](DataJobResult result) {
            finish_data_job_ui(job);
            if (data_job_ == job) data_job_ = nullptr;
            job->deleteLater();
            refresh_project_ui();

            if (!project_ || project_->path() != project_path) {
                return;
            }

            if (result.cancelled || !result.ok()) {
                blocked_streaming_tile_slot_ = slot_id;
            } else {
                blocked_streaming_tile_slot_.reset();
            }

            const storage::Status refreshed = project_->refresh_metadata();
            if (!refreshed.ok()) {
                statusBar()->showMessage(
                    QStringLiteral("Viewport terrain metadata refresh failed: %1")
                        .arg(QString::fromStdString(refreshed.diagnostic)),
                    8000
                );
                return;
            }

            // Even a killed worker may have completed earlier short durable
            // resource transactions. Reconcile unconditionally; the blocked
            // slot above prevents an accidental retry loop on failure.
            if (!load_render_model()) return;
            refresh_project_ui();

            if (result.cancelled) {
                statusBar()->showMessage(
                    QStringLiteral(
                        "Viewport terrain cancelled · partial NOAA bytes retained for safe resume"
                    ),
                    6500
                );
                return;
            }
            if (!result.ok()) {
                statusBar()->showMessage(
                    QStringLiteral("Viewport terrain unavailable: %1")
                        .arg(QString::fromStdString(result.diagnostic)),
                    9000
                );
                return;
            }

            statusBar()->showMessage(
                result.changed
                    ? QStringLiteral(
                        "ETOPO 15″ viewport terrain committed to .aeris"
                      )
                    : QStringLiteral(
                        "ETOPO 15″ viewport terrain already durable"
                      ),
                6500
            );
        }
    );

    if (!started) {
        finish_data_job_ui(job);
        if (data_job_ == job) data_job_ = nullptr;
        job->deleteLater();
        blocked_streaming_tile_slot_ = slot_id;
        refresh_project_ui();
        statusBar()->showMessage(
            QStringLiteral(
                "Viewport terrain worker could not be started"
            ),
            7000
        );
    }
    return true;
}

}  // namespace aeris::desktop
