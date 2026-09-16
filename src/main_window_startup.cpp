// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "map_view.hpp"

#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace aeris::desktop {
namespace {

[[nodiscard]] std::string startup_utc_now() {
    return QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODate)
        .toStdString();
}

[[nodiscard]] std::filesystem::path filesystem_path_from_qt_startup(
    const QString& path
) {
    return QFile(path).filesystemFileName();
}

[[nodiscard]] QString starter_filename() {
    return QStringLiteral("starter-world-draft-%1.%2.aeris")
        .arg(storage::kDraftFormatMajor)
        .arg(storage::kDraftFormatMinor);
}

}  // namespace

void MainWindow::open_startup_world() {
    if (project_) return;

    const QString data_root =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (data_root.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("Starter world unavailable · application data directory could not be resolved"),
            7000
        );
        return;
    }
    if (!QDir().mkpath(data_root)) {
        statusBar()->showMessage(
            QStringLiteral("Starter world unavailable · application data directory could not be created"),
            7000
        );
        return;
    }

    const std::filesystem::path path = filesystem_path_from_qt_startup(
        QDir(data_root).filePath(starter_filename())
    );

    storage::ProjectStoreResult opened{};
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        statusBar()->showMessage(
            QStringLiteral("Starter world unavailable: %1")
                .arg(QString::fromStdString(exists_error.message())),
            7000
        );
        return;
    }

    if (exists) {
        opened = storage::ProjectStore::open(path);
        if (!opened.ok()) {
            // An application-owned draft is still durable user-visible data.
            // Never delete or replace a project just because this executable no
            // longer understands it; a new draft uses a different filename.
            QMessageBox::warning(
                this,
                QStringLiteral("Starter world could not be opened"),
                QString::fromStdString(opened.status.diagnostic)
            );
            return;
        }
    } else {
        storage::ProjectCreateOptions options{};
        options.timestamp_utc = startup_utc_now();
        options.producer = "aeris-desktop";
        options.producer_version = "0.1.0";
        opened = storage::ProjectStore::create(path, options);
        if (!opened.ok()) {
            statusBar()->showMessage(
                QStringLiteral("Starter world could not be created: %1")
                    .arg(QString::fromStdString(opened.status.diagnostic)),
                7000
            );
            return;
        }

        const storage::Status integrity = opened.store->verify_integrity();
        if (!integrity.ok()) {
            QMessageBox::critical(
                this,
                QStringLiteral("Starter world verification failed"),
                QString::fromStdString(integrity.diagnostic)
            );
            return;
        }
    }

    scene_controller_.cancel();
    project_ = std::move(opened.store);
    if (!load_render_model()) {
        project_.reset();
        model_.reset();
        scene_controller_.set_model(nullptr);
        map_view_->clear_project();
        refresh_project_ui();
        return;
    }

    refresh_project_ui();
    if (model_ && model_->sources.empty() && !project_->metadata().frozen) {
        install_base_world();
        return;
    }

    layers_dock_->show();
    statusBar()->showMessage(QStringLiteral("Starter political world ready"), 3500);
}

}  // namespace aeris::desktop
