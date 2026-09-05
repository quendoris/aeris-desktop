// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"
#include "project_model.hpp"
#include "scene_controller.hpp"

#include <QMainWindow>

#include <memory>

class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QPushButton;
class QSlider;
class QTreeWidget;
class QTreeWidgetItem;

namespace aeris::desktop {

class MapView;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void build_ui();
    void apply_theme();
    void new_project();
    void open_project();
    void close_project();
    void import_world_data();
    void refresh_project_ui();
    void refresh_unfold_controls();
    bool load_render_model();
    void rebuild_layer_tree();
    void set_layer_visibility(QTreeWidgetItem* item);
    void apply_selected_projection();

    MapView* map_view_{nullptr};
    QDockWidget* unfold_dock_{nullptr};
    QDockWidget* layers_dock_{nullptr};
    QDockWidget* inspector_dock_{nullptr};
    QComboBox* projection_combo_{nullptr};
    QSlider* cut_slider_{nullptr};
    QLabel* cut_value_label_{nullptr};
    QPushButton* apply_projection_button_{nullptr};
    QPushButton* return_globe_button_{nullptr};
    QTreeWidget* layer_tree_{nullptr};
    QLabel* project_path_value_{nullptr};
    QLabel* project_uuid_value_{nullptr};
    QLabel* project_revision_value_{nullptr};
    QLabel* project_format_value_{nullptr};
    QLabel* project_projection_value_{nullptr};
    QLabel* project_state_value_{nullptr};
    QAction* close_project_action_{nullptr};
    QAction* import_world_data_action_{nullptr};
    QAction* zoom_in_action_{nullptr};
    QAction* zoom_out_action_{nullptr};
    QAction* reset_view_action_{nullptr};

    std::unique_ptr<aeris::storage::ProjectStore> project_;
    std::shared_ptr<const ProjectModel> model_;
    SceneController scene_controller_;
    bool rebuilding_layers_{false};
};

}  // namespace aeris::desktop
