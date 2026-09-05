// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "elevation_import.hpp"

#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <string>

namespace aeris::desktop {
namespace {

[[nodiscard]] std::filesystem::path filesystem_path_from_qt_elevation(
    const QString& path
) {
    return QFile(path).filesystemFileName();
}

}  // namespace

void MainWindow::import_etopo_elevation() {
    if (property("aerisElevationImportBusy").toBool()) {
        statusBar()->showMessage(
            QStringLiteral("ETOPO elevation import is already running"),
            2500
        );
        return;
    }
    if (!project_ || !model_ || model_->sources.empty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Elevation needs a world project"),
            QStringLiteral(
                "Create or open a project and import the Natural Earth world data first."
            )
        );
        return;
    }
    if (project_->metadata().frozen) {
        QMessageBox::warning(
            this,
            QStringLiteral("Project is frozen"),
            QStringLiteral("Thaw or copy the project before adding elevation data.")
        );
        return;
    }

    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select NOAA ETOPO 2022 v1 global 60 arc-second GeoTIFF"),
        QString(),
        QStringLiteral("GeoTIFF elevation (*.tif *.tiff);;All files (*)")
    );
    if (selected.isEmpty()) return;

    const std::filesystem::path project_path = project_->path();
    const std::filesystem::path tiff_path = filesystem_path_from_qt_elevation(selected);
    const std::string modified_utc = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODateWithMs)
        .toStdString();

    setProperty("aerisElevationImportBusy", true);
    menuBar()->setEnabled(false);
    layers_dock_->setEnabled(false);
    statusBar()->showMessage(
        QStringLiteral(
            "Importing ETOPO elevation in the background · decoding, tiling and embedding numerical data…"
        )
    );

    auto* watcher = new QFutureWatcher<ElevationImportResult>(this);
    connect(
        watcher,
        &QFutureWatcher<ElevationImportResult>::finished,
        this,
        [this, watcher, project_path]() {
            const ElevationImportResult imported = watcher->result();
            watcher->deleteLater();
            setProperty("aerisElevationImportBusy", false);
            menuBar()->setEnabled(true);
            layers_dock_->setEnabled(true);

            // The worker owns an independent ProjectStore handle. If this window
            // moved to another project while the task was finishing, never apply
            // stale model/UI state to the newly opened project.
            if (!project_ || project_->path() != project_path) {
                statusBar()->showMessage(
                    imported.ok()
                        ? QStringLiteral("ETOPO import finished in its original .aeris project")
                        : QStringLiteral("ETOPO import failed in its original .aeris project"),
                    5000
                );
                return;
            }

            const storage::Status refreshed = project_->refresh_metadata();
            if (!refreshed.ok()) {
                QMessageBox::critical(
                    this,
                    QStringLiteral("Elevation import metadata reload failed"),
                    QString::fromStdString(refreshed.diagnostic)
                );
                return;
            }

            if (!imported.ok()) {
                if (imported.changed) {
                    load_render_model();
                    refresh_project_ui();
                }
                QMessageBox::critical(
                    this,
                    QStringLiteral("ETOPO elevation import failed"),
                    QString::fromStdString(imported.diagnostic)
                );
                return;
            }

            if (!load_render_model()) return;
            refresh_project_ui();
            layers_dock_->show();
            statusBar()->showMessage(
                imported.changed
                    ? QStringLiteral(
                        "ETOPO numerical elevation committed to .aeris · source GeoTIFF is no longer required"
                      )
                    : QStringLiteral("ETOPO elevation is already present in this project"),
                8000
            );
        }
    );

    watcher->setFuture(QtConcurrent::run(
        [project_path, tiff_path, modified_utc]() -> ElevationImportResult {
            storage::ProjectStoreResult opened = storage::ProjectStore::open(project_path);
            if (!opened.ok()) {
                return {
                    false,
                    false,
                    0U,
                    "could not reopen target .aeris project for background elevation import: " +
                        opened.status.diagnostic,
                };
            }
            return import_etopo2022_global_60s(
                *opened.store,
                tiff_path,
                modified_utc
            );
        }
    ));
}

}  // namespace aeris::desktop
