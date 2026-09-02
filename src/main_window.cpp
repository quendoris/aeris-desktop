// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "map_view.hpp"

#include "aeris/storage/layer.hpp"

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>
#include <utility>

namespace aeris::desktop {
namespace {

QLabel* selectable_value(QWidget* parent) {
    auto* value = new QLabel(QStringLiteral("—"), parent);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setWordWrap(true);
    return value;
}

[[nodiscard]] std::string utc_now() {
    return QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODateWithMs)
        .toStdString();
}

[[nodiscard]] std::filesystem::path filesystem_path_from_qt(const QString& path) {
    return QFile(path).filesystemFileName();
}

[[nodiscard]] QString filesystem_path_to_qt(const std::filesystem::path& path) {
    return QFile(path).fileName();
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      scene_controller_(this) {
    build_ui();
    apply_theme();

    map_view_->set_scene_request_callback(
        [this](const view::SceneRequest& request) {
            scene_controller_.request(request);
        }
    );
    scene_controller_.set_frame_callback(
        [this](RenderFrame frame) {
            if (!frame.ok) {
                statusBar()->showMessage(
                    QStringLiteral("Scene failed: %1")
                        .arg(QString::fromStdString(frame.diagnostic))
                );
            } else {
                statusBar()->showMessage(
                    frame.request.quality == view::SceneQuality::verified
                        ? QStringLiteral("Verified map geometry ready")
                        : QStringLiteral("Interactive preview")
                );
            }
            map_view_->set_frame(std::move(frame));
        }
    );
    scene_controller_.set_busy_callback(
        [this](const bool busy) { map_view_->set_busy(busy); }
    );

    refresh_project_ui();
    resize(1440, 900);
    setWindowTitle(QStringLiteral("AERIS Desktop"));
}

void MainWindow::build_ui() {
    map_view_ = new MapView(this);
    setCentralWidget(map_view_);

    auto* file_menu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* open_action = file_menu->addAction(QStringLiteral("&Open project…"));
    open_action->setShortcut(QKeySequence::Open);
    connect(open_action, &QAction::triggered, this, &MainWindow::open_project);

    close_project_action_ = file_menu->addAction(QStringLiteral("&Close project"));
    close_project_action_->setShortcut(QKeySequence::Close);
    connect(close_project_action_, &QAction::triggered, this, &MainWindow::close_project);

    file_menu->addSeparator();
    auto* exit_action = file_menu->addAction(QStringLiteral("E&xit"));
    exit_action->setShortcut(QKeySequence::Quit);
    connect(exit_action, &QAction::triggered, this, &QWidget::close);

    auto* tools_menu = menuBar()->addMenu(QStringLiteral("&Tools"));
    auto* navigate_action = tools_menu->addAction(QStringLiteral("Navigate"));
    navigate_action->setCheckable(true);
    navigate_action->setChecked(true);

    auto* unfold_action = tools_menu->addAction(QStringLiteral("Unfold / projection"));
    unfold_action->setCheckable(true);

    auto* view_menu = menuBar()->addMenu(QStringLiteral("&View"));

    auto* toolbar = addToolBar(QStringLiteral("Map tools"));
    toolbar->setObjectName(QStringLiteral("mapTools"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->addAction(open_action);
    toolbar->addSeparator();
    toolbar->addAction(navigate_action);
    toolbar->addAction(unfold_action);

    unfold_dock_ = new QDockWidget(QStringLiteral("Unfold / projection"), this);
    unfold_dock_->setObjectName(QStringLiteral("unfoldDock"));
    auto* unfold_widget = new QWidget(unfold_dock_);
    auto* unfold_layout = new QVBoxLayout(unfold_widget);
    auto* explanation = new QLabel(
        QStringLiteral(
            "Choose an equal-area unfolding for the current map. "
            "The globe remains the natural folded state."
        ),
        unfold_widget
    );
    explanation->setWordWrap(true);
    unfold_layout->addWidget(explanation);

    projection_combo_ = new QComboBox(unfold_widget);
    projection_combo_->addItem(
        QStringLiteral("Sinusoidal"),
        static_cast<int>(view::SurfaceMode::sinusoidal)
    );
    projection_combo_->addItem(
        QStringLiteral("Mollweide"),
        static_cast<int>(view::SurfaceMode::mollweide)
    );
    unfold_layout->addWidget(projection_combo_);

    apply_projection_button_ = new QPushButton(
        QStringLiteral("Apply / unfold"),
        unfold_widget
    );
    connect(
        apply_projection_button_,
        &QPushButton::clicked,
        this,
        &MainWindow::apply_selected_projection
    );
    unfold_layout->addWidget(apply_projection_button_);

    return_globe_button_ = new QPushButton(
        QStringLiteral("Return to globe"),
        unfold_widget
    );
    connect(return_globe_button_, &QPushButton::clicked, this, [this]() {
        map_view_->set_surface_mode(view::SurfaceMode::globe);
    });
    unfold_layout->addWidget(return_globe_button_);
    unfold_layout->addStretch(1);
    unfold_dock_->setWidget(unfold_widget);
    addDockWidget(Qt::RightDockWidgetArea, unfold_dock_);
    unfold_dock_->hide();

    connect(unfold_action, &QAction::toggled, unfold_dock_, &QDockWidget::setVisible);
    connect(unfold_dock_, &QDockWidget::visibilityChanged, unfold_action, &QAction::setChecked);

    layers_dock_ = new QDockWidget(QStringLiteral("Layers"), this);
    layers_dock_->setObjectName(QStringLiteral("layersDock"));
    layer_tree_ = new QTreeWidget(layers_dock_);
    layer_tree_->setHeaderHidden(true);
    layer_tree_->setRootIsDecorated(false);
    layer_tree_->setAlternatingRowColors(false);
    layers_dock_->setWidget(layer_tree_);
    addDockWidget(Qt::RightDockWidgetArea, layers_dock_);
    layers_dock_->hide();
    view_menu->addAction(layers_dock_->toggleViewAction());
    connect(
        layer_tree_,
        &QTreeWidget::itemChanged,
        this,
        [this](QTreeWidgetItem* item, const int column) {
            if (column == 0) set_layer_visibility(item);
        }
    );

    inspector_dock_ = new QDockWidget(QStringLiteral("Developer Inspector"), this);
    inspector_dock_->setObjectName(QStringLiteral("developerInspectorDock"));
    auto* inspector = new QWidget(inspector_dock_);
    auto* form = new QFormLayout(inspector);
    project_path_value_ = selectable_value(inspector);
    project_uuid_value_ = selectable_value(inspector);
    project_revision_value_ = selectable_value(inspector);
    project_format_value_ = selectable_value(inspector);
    project_projection_value_ = selectable_value(inspector);
    project_state_value_ = selectable_value(inspector);
    form->addRow(QStringLiteral("Path"), project_path_value_);
    form->addRow(QStringLiteral("UUID"), project_uuid_value_);
    form->addRow(QStringLiteral("Revision"), project_revision_value_);
    form->addRow(QStringLiteral("Format"), project_format_value_);
    form->addRow(QStringLiteral("Projection"), project_projection_value_);
    form->addRow(QStringLiteral("State"), project_state_value_);
    inspector_dock_->setWidget(inspector);
    addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
    inspector_dock_->hide();
    view_menu->addAction(inspector_dock_->toggleViewAction());

    statusBar()->showMessage(QStringLiteral("Ready"));
}

void MainWindow::apply_theme() {
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: #15171a; color: #e5e7ea; }
        QMenuBar, QMenu, QToolBar, QStatusBar { background: #1d2024; color: #e5e7ea; }
        QMenuBar::item:selected, QMenu::item:selected { background: #303640; }
        QToolBar { border: none; spacing: 5px; padding: 5px; }
        QToolButton { background: #292d33; border: 1px solid #373c44; border-radius: 5px; padding: 7px 11px; }
        QToolButton:checked { background: #3a4655; border-color: #61748b; }
        QDockWidget::title { background: #1d2024; padding: 7px; }
        QComboBox, QPushButton { background: #292d33; border: 1px solid #3a4049; border-radius: 5px; padding: 7px; }
        QPushButton:disabled { color: #737982; }
        QTreeWidget { background: #1b1e22; border: none; outline: none; }
        QTreeWidget::item { padding: 6px 4px; }
        QTreeWidget::item:selected { background: #303640; }
        QLabel { background: transparent; }
    )"));
}

void MainWindow::open_project() {
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open AERIS project"),
        QString(),
        QStringLiteral("AERIS projects (*.aeris);;All files (*)")
    );
    if (selected.isEmpty()) return;

    auto opened = aeris::storage::ProjectStore::open(filesystem_path_from_qt(selected));
    if (!opened.ok()) {
        QMessageBox::critical(
            this,
            QStringLiteral("AERIS project open failed"),
            QString::fromStdString(opened.status.diagnostic)
        );
        return;
    }

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
    statusBar()->showMessage(QStringLiteral("Opening durable AERIS map…"), 3500);
}

void MainWindow::close_project() {
    scene_controller_.cancel();
    scene_controller_.set_model(nullptr);
    model_.reset();
    project_.reset();
    map_view_->clear_project();
    rebuild_layer_tree();
    refresh_project_ui();
    statusBar()->showMessage(QStringLiteral("Project closed"), 2000);
}

bool MainWindow::load_render_model() {
    if (!project_) return false;

    ProjectModelLoadResult loaded = load_project_model(*project_);
    if (!loaded.ok()) {
        QMessageBox::critical(
            this,
            QStringLiteral("AERIS map load failed"),
            QString::fromStdString(loaded.diagnostic)
        );
        return false;
    }

    model_ = std::move(loaded.model);
    scene_controller_.set_model(model_);
    const auto& metadata = project_->metadata();
    map_view_->set_project(model_, metadata.project_uuid, metadata.revision);
    rebuild_layer_tree();
    return true;
}

void MainWindow::rebuild_layer_tree() {
    rebuilding_layers_ = true;
    layer_tree_->clear();
    if (model_) {
        for (const storage::ProjectLayerRecord& layer : model_->layers) {
            auto* item = new QTreeWidgetItem(layer_tree_);
            item->setText(0, QString::fromStdString(layer.name));
            item->setData(0, Qt::UserRole, QString::fromStdString(layer.layer_id));
            item->setToolTip(0, QString::fromStdString(layer.role_id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, layer.visible ? Qt::Checked : Qt::Unchecked);
        }
    }
    rebuilding_layers_ = false;
}

void MainWindow::set_layer_visibility(QTreeWidgetItem* item) {
    if (rebuilding_layers_ || item == nullptr || !project_ || !model_) return;

    const std::string layer_id = item->data(0, Qt::UserRole).toString().toStdString();
    storage::LayerStateUpdate update{};
    update.modified_utc = utc_now();
    update.visible = item->checkState(0) == Qt::Checked;
    const storage::LayerMutationResult result =
        storage::update_layer_state(*project_, layer_id, update);
    if (!result.ok()) {
        statusBar()->showMessage(
            QStringLiteral("Layer mutation rejected: %1")
                .arg(QString::fromStdString(result.status.diagnostic)),
            5000
        );
        rebuild_layer_tree();
        return;
    }

    ProjectModelLoadResult loaded = load_project_model(*project_);
    if (!loaded.ok()) {
        statusBar()->showMessage(
            QStringLiteral("Layer committed, reload failed: %1")
                .arg(QString::fromStdString(loaded.diagnostic)),
            5000
        );
        return;
    }

    model_ = std::move(loaded.model);
    scene_controller_.set_model(model_);
    map_view_->set_project_model(model_, project_->metadata().revision);
    rebuild_layer_tree();
    refresh_project_ui();
    statusBar()->showMessage(
        result.changed
            ? QStringLiteral("Layer visibility committed to .aeris")
            : QStringLiteral("Layer visibility unchanged"),
        2200
    );
}

void MainWindow::apply_selected_projection() {
    if (!project_) return;
    const auto raw = projection_combo_->currentData().toInt();
    const auto mode = static_cast<view::SurfaceMode>(raw);
    if (mode != view::SurfaceMode::sinusoidal &&
        mode != view::SurfaceMode::mollweide) {
        return;
    }
    map_view_->set_surface_mode(mode);
}

void MainWindow::refresh_project_ui() {
    const bool has_project = project_ != nullptr;
    close_project_action_->setEnabled(has_project);
    apply_projection_button_->setEnabled(has_project);
    return_globe_button_->setEnabled(has_project);

    if (!project_) {
        project_path_value_->setText(QStringLiteral("—"));
        project_uuid_value_->setText(QStringLiteral("—"));
        project_revision_value_->setText(QStringLiteral("—"));
        project_format_value_->setText(QStringLiteral("—"));
        project_projection_value_->setText(QStringLiteral("—"));
        project_state_value_->setText(QStringLiteral("No project"));
        return;
    }

    const auto& metadata = project_->metadata();
    project_path_value_->setText(filesystem_path_to_qt(project_->path()));
    project_uuid_value_->setText(QString::fromStdString(metadata.project_uuid));
    project_revision_value_->setText(QString::number(static_cast<qulonglong>(metadata.revision)));
    project_format_value_->setText(
        QStringLiteral("%1.%2")
            .arg(metadata.format_major)
            .arg(metadata.format_minor)
    );
    project_projection_value_->setText(QString::fromStdString(metadata.projection_id));
    project_state_value_->setText(metadata.frozen ? QStringLiteral("Frozen") : QStringLiteral("Editable"));
}

}  // namespace aeris::desktop
