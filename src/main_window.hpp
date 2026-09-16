// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"
#include "project_model.hpp"
#include "scene_controller.hpp"

#include <QMainWindow>
#include <QString>

#include <memory>

class QAction;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace aeris::desktop {

class DataJobProcess;
class MapView;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Open the application-owned starter .aeris. During source-development the
    // file is created lazily in application data and populated through the same
    // verified acquisition/import path as a user project. A packaged release
    // may ship an already-populated starter without changing this ownership
    // boundary.
    void open_startup_world();

    // Public UI commands so optional data-pack integrations can add themselves
    // to the Data menu without taking ownership of project/storage internals.
    void import_country_flags();
    void import_etopo_elevation();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void build_ui();
    void apply_theme();
    void new_project();
    void open_project();
    void close_project();
    void install_base_world();
    void import_world_data();
    void begin_data_job_ui(DataJobProcess* job, const QString& initial_phase);
    void finish_data_job_ui(DataJobProcess* job);
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
    QWidget* data_job_widget_{nullptr};
    QLabel* data_job_label_{nullptr};
    QProgressBar* data_job_progress_{nullptr};
    QPushButton* data_job_cancel_button_{nullptr};
    QAction* close_project_action_{nullptr};
    QAction* install_base_world_action_{nullptr};
    QAction* import_world_data_action_{nullptr};
    QAction* zoom_in_action_{nullptr};
    QAction* zoom_out_action_{nullptr};
    QAction* reset_view_action_{nullptr};

    std::unique_ptr<aeris::storage::ProjectStore> project_;
    std::shared_ptr<const ProjectModel> model_;
    SceneController scene_controller_;
    DataJobProcess* data_job_{nullptr};
    bool rebuilding_layers_{false};
};

}  // namespace aeris::desktop
