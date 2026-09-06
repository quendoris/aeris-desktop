// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "data_job_process.hpp"

#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QStatusBar>

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
    if (data_job_ != nullptr) {
        statusBar()->showMessage(
            QStringLiteral("Another project data job is already running"),
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

    auto* job = new DataJobProcess(this);
    data_job_ = job;
    refresh_project_ui();
    statusBar()->showMessage(
        QStringLiteral(
            "Importing country flags in isolated data worker · map remains interactive…"
        )
    );

    const bool started = job->start(
        "flags",
        project_path,
        pack_root,
        modified_utc,
        [this, job, project_path](DataJobResult imported) {
            if (data_job_ == job) data_job_ = nullptr;
            job->deleteLater();
            refresh_project_ui();

            // The import process owns an independent ProjectStore. A user may
            // navigate or open another project while it runs; never apply stale
            // model/UI state to a different project.
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
    if (!started) {
        if (data_job_ == job) data_job_ = nullptr;
        job->deleteLater();
        refresh_project_ui();
        QMessageBox::critical(
            this,
            QStringLiteral("Country flag import failed to start"),
            QStringLiteral("Could not launch the isolated AERIS data worker.")
        );
    }
}

}  // namespace aeris::desktop
