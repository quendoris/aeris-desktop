// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "flag_pack_import.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QStatusBar>

#include <filesystem>

namespace aeris::desktop {
namespace {

[[nodiscard]] std::filesystem::path filesystem_path_from_qt_flags(
    const QString& path
) {
    return QFile(path).filesystemFileName();
}

}  // namespace

void MainWindow::import_country_flags() {
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

    statusBar()->showMessage(
        QStringLiteral("Verifying and embedding country flags into .aeris…")
    );
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const FlagPackImportResult imported = import_country_flag_png_pack(
        *project_,
        filesystem_path_from_qt_flags(selected),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()
    );
    QApplication::restoreOverrideCursor();

    if (!imported.ok()) {
        if (imported.changed) {
            project_->refresh_metadata();
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

}  // namespace aeris::desktop
