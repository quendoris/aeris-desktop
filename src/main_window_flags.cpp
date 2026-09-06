// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "flag_pack_import.hpp"

#include <QAction>
#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <string>

namespace aeris::desktop {
namespace {

[[nodiscard]] std::filesystem::path filesystem_path_from_qt_flags(
    const QString& path
) {
    return QFile(path).filesystemFileName();
}

}  // namespace

void MainWindow::import_country_flags() {
    if (property("aerisFlagImportBusy").toBool()) {
        statusBar()->showMessage(
            QStringLiteral("Country flag import is already running"),
            2500
        );
        return;
    }
    if (!project_ || !model_ || model_->sources.empty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Country flags need political data"),
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
            QStringLiteral("Thaw or copy the project before adding a flag pack.")
        );
        return;
    }

    const QString selected = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select downloaded ISO country flag PNG pack")
    );
    if (selected.isEmpty()) return;

    const std::filesystem::path project_path = project_->path();
    const std::filesystem::path pack_root = filesystem_path_from_qt_flags(selected);
    const std::string modified_utc = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODate)
        .toStdString();

    setProperty("aerisFlagImportBusy", true);
    if (auto* action = findChild<QAction*>(QStringLiteral("importCountryFlagsAction"))) {
        action->setEnabled(false);
    }
    statusBar()->showMessage(
        QStringLiteral(
            "Importing country flags in the background · verifying and embedding resources…"
        )
    );

    auto* watcher = new QFutureWatcher<FlagPackImportResult>(this);
    connect(
        watcher,
        &QFutureWatcher<FlagPackImportResult>::finished,
        this,
        [this, watcher, project_path]() {
            const FlagPackImportResult imported = watcher->result();
            watcher->deleteLater();
            setProperty("aerisFlagImportBusy", false);
            if (auto* action = findChild<QAction*>(QStringLiteral("importCountryFlagsAction"))) {
                action->setEnabled(true);
            }

            // The worker owns an independent ProjectStore handle. A user may
            // keep navigating or even open another project while the import is
            // running; never apply stale UI/model state to a different project.
            if (!project_ || project_->path() != project_path) {
                statusBar()->showMessage(
                    imported.ok()
                        ? QStringLiteral("Country flag import finished in its original .aeris project")
                        : QStringLiteral("Country flag import failed in its original .aeris project"),
                    5000
                );
                return;
            }

            const storage::Status refreshed = project_->refresh_metadata();
            if (!refreshed.ok()) {
                QMessageBox::critical(
                    this,
                    QStringLiteral("Country flag metadata reload failed"),
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
                    QStringLiteral("Country flag import failed"),
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
                        "Country flags embedded in .aeris · source pack is no longer required"
                      )
                    : QStringLiteral("Country flag pack is already present in this project"),
                6500
            );
        }
    );

    watcher->setFuture(QtConcurrent::run(
        [project_path, pack_root, modified_utc]() -> FlagPackImportResult {
            storage::ProjectStoreResult opened = storage::ProjectStore::open(project_path);
            if (!opened.ok()) {
                return {
                    false,
                    false,
                    0U,
                    "could not reopen target .aeris project for background flag import: " +
                        opened.status.diagnostic,
                };
            }
            return import_country_flag_png_pack(
                *opened.store,
                pack_root,
                modified_utc
            );
        }
    ));
}

}  // namespace aeris::desktop
