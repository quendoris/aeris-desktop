// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "map_view.hpp"

#include <QDockWidget>
#include <QMessageBox>
#include <QStatusBar>

#include <utility>

namespace aeris::desktop {

bool MainWindow::open_project_path(const std::filesystem::path& path) {
    if (path.empty()) return false;

    auto opened = storage::ProjectStore::open(path);
    if (!opened.ok()) {
        QMessageBox::critical(
            this,
            QStringLiteral("AERIS project open failed"),
            QString::fromStdString(opened.status.diagnostic)
        );
        return false;
    }

    scene_controller_.cancel();
    project_ = std::move(opened.store);
    if (!load_render_model()) {
        project_.reset();
        model_.reset();
        scene_controller_.set_model(nullptr);
        map_view_->clear_project();
        refresh_project_ui();
        return false;
    }

    refresh_project_ui();
    if (model_ && model_->sources.empty() && !project_->metadata().frozen) {
        install_base_world();
    } else {
        layers_dock_->show();
        statusBar()->showMessage(QStringLiteral("Opening durable AERIS map…"), 3500);
    }
    return true;
}

}  // namespace aeris::desktop
