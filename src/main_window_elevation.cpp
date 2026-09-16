// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "data_job_process.hpp"

#include <QAbstractButton>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>

#include <filesystem>
#include <string>

namespace aeris::desktop {
namespace {

[[nodiscard]] std::filesystem::path filesystem_path_from_qt_elevation(
    const QString& path
) {
    return QFile(path).filesystemFileName();
}

[[nodiscard]] std::filesystem::path etopo_cache_path() {
    const QString cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache_root.isEmpty()) return {};
    return filesystem_path_from_qt_elevation(
        QDir(cache_root).filePath(QStringLiteral("etopo-2022-v1-60s"))
    );
}

}  // namespace

void MainWindow::import_etopo_elevation() {
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
            QStringLiteral("Elevation needs a world project"),
            QStringLiteral(
                "Create or open a project and let AERIS prepare the base political world first."
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

    QMessageBox chooser(this);
    chooser.setWindowTitle(QStringLiteral("Add NOAA ETOPO 2022 elevation"));
    chooser.setIcon(QMessageBox::Information);
    chooser.setText(QStringLiteral(
        "Choose which official NOAA/NCEI global 60 arc-second relief surface to add."
    ));
    chooser.setInformativeText(QStringLiteral(
        "AERIS downloads into its acquisition cache, resumes an interrupted transfer when the server provides a safe HTTP validator, checks the global Float32 TIFF structure, then converts it into durable numerical .aeris tiles.\n\n"
        "Ice Surface: land + ocean bathymetry + the top of the Greenland and Antarctic ice sheets.\n"
        "Bedrock: land + ocean bathymetry + bedrock below the major ice sheets."
    ));
    QAbstractButton* surface_button = chooser.addButton(
        QStringLiteral("Download Ice Surface"),
        QMessageBox::AcceptRole
    );
    QAbstractButton* bed_button = chooser.addButton(
        QStringLiteral("Download Bedrock"),
        QMessageBox::AcceptRole
    );
    QAbstractButton* local_button = chooser.addButton(
        QStringLiteral("Import local GeoTIFF…"),
        QMessageBox::ActionRole
    );
    chooser.addButton(QMessageBox::Cancel);
    chooser.exec();

    const QAbstractButton* clicked = chooser.clickedButton();
    if (clicked == nullptr || clicked == chooser.button(QMessageBox::Cancel)) return;

    std::string operation;
    std::filesystem::path source_path;
    QString initial_phase;
    QString running_message;
    bool automatic_acquisition = false;

    if (clicked == surface_button || clicked == bed_button) {
        source_path = etopo_cache_path();
        if (source_path.empty()) {
            QMessageBox::critical(
                this,
                QStringLiteral("ETOPO acquisition cache unavailable"),
                QStringLiteral("AERIS could not resolve a writable application cache directory.")
            );
            return;
        }
        automatic_acquisition = true;
        if (clicked == bed_button) {
            operation = "etopo-auto-bed";
            initial_phase = QStringLiteral("Preparing ETOPO 2022 Bedrock acquisition…");
            running_message = QStringLiteral(
                "Downloading NOAA ETOPO 2022 Bedrock in the isolated data worker · the map remains interactive…"
            );
        } else {
            operation = "etopo-auto-surface";
            initial_phase = QStringLiteral("Preparing ETOPO 2022 Ice Surface acquisition…");
            running_message = QStringLiteral(
                "Downloading NOAA ETOPO 2022 Ice Surface in the isolated data worker · the map remains interactive…"
            );
        }
    } else if (clicked == local_button) {
        const QString selected = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select NOAA ETOPO 2022 v1 global 60 arc-second GeoTIFF"),
            QString(),
            QStringLiteral("GeoTIFF elevation (*.tif *.tiff);;All files (*)")
        );
        if (selected.isEmpty()) return;
        operation = "etopo";
        source_path = filesystem_path_from_qt_elevation(selected);
        initial_phase = QStringLiteral("Preparing local ETOPO elevation import…");
        running_message = QStringLiteral(
            "Importing local ETOPO in the isolated data worker · the map remains interactive…"
        );
    } else {
        return;
    }

    const std::filesystem::path project_path = project_->path();
    const std::string modified_utc = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODate)
        .toStdString();

    auto* job = new DataJobProcess(this);
    data_job_ = job;
    begin_data_job_ui(job, initial_phase);
    refresh_project_ui();
    statusBar()->showMessage(running_message);

    const bool started = job->start(
        std::move(operation),
        project_path,
        source_path,
        modified_utc,
        [this, job, project_path, automatic_acquisition](DataJobResult imported) {
            finish_data_job_ui(job);
            if (data_job_ == job) data_job_ = nullptr;
            job->deleteLater();
            refresh_project_ui();

            if (!project_ || project_->path() != project_path) {
                statusBar()->showMessage(
                    imported.cancelled
                        ? QStringLiteral("ETOPO data job cancelled in its original .aeris project")
                        : (imported.ok()
                            ? QStringLiteral("ETOPO data job finished in its original .aeris project")
                            : QStringLiteral("ETOPO data job failed in its original .aeris project")),
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

            if (imported.cancelled) {
                load_render_model();
                refresh_project_ui();
                statusBar()->showMessage(
                    automatic_acquisition
                        ? QStringLiteral(
                            "ETOPO acquisition cancelled · partial download retained for safe resume"
                          )
                        : QStringLiteral(
                            "ETOPO import cancelled · durable committed state reconciled"
                          ),
                    6000
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
                    automatic_acquisition
                        ? QStringLiteral("ETOPO download / import failed")
                        : QStringLiteral("ETOPO elevation import failed"),
                    QString::fromStdString(imported.diagnostic)
                );
                return;
            }

            if (!load_render_model()) return;
            refresh_project_ui();
            layers_dock_->show();
            statusBar()->showMessage(
                imported.changed
                    ? (automatic_acquisition
                        ? QStringLiteral(
                            "ETOPO downloaded, validated and committed to .aeris · future rendering no longer depends on the GeoTIFF"
                          )
                        : QStringLiteral(
                            "ETOPO numerical elevation committed to .aeris · source GeoTIFF is no longer required"
                          ))
                    : QStringLiteral("ETOPO elevation is already present in this project"),
                9000
            );
        }
    );
    if (!started) {
        finish_data_job_ui(job);
        if (data_job_ == job) data_job_ = nullptr;
        job->deleteLater();
        refresh_project_ui();
        QMessageBox::critical(
            this,
            QStringLiteral("ETOPO data job failed to start"),
            QStringLiteral("Could not launch the isolated AERIS data worker.")
        );
    }
}

}  // namespace aeris::desktop
