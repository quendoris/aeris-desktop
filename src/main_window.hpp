// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

#include <QMainWindow>

#include <memory>

class QAction;
class QComboBox;
class QDockWidget;
class QLabel;

namespace aeris::desktop {

class MapView;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void build_ui();
    void apply_theme();
    void open_project();
    void close_project();
    void refresh_project_ui();

    MapView* map_view_{nullptr};
    QDockWidget* unfold_dock_{nullptr};
    QDockWidget* layers_dock_{nullptr};
    QDockWidget* inspector_dock_{nullptr};
    QComboBox* projection_combo_{nullptr};
    QLabel* project_path_value_{nullptr};
    QLabel* project_uuid_value_{nullptr};
    QLabel* project_revision_value_{nullptr};
    QLabel* project_format_value_{nullptr};
    QLabel* project_projection_value_{nullptr};
    QLabel* project_state_value_{nullptr};
    QAction* close_project_action_{nullptr};

    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

}  // namespace aeris::desktop
