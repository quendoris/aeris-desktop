// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "data_job_process.hpp"
#include "map_view.hpp"
#include "map_workspace_view.hpp"

#include "aeris/storage/layer.hpp"
#include "aeris/view/projection_catalog.hpp"

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
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
        .toString(Qt::ISODate)
        .toStdString();
}

[[nodiscard]] std::filesystem::path filesystem_path_from_qt(const QString& path) {
    return QFile(path).filesystemFileName();
}

[[nodiscard]] QString filesystem_path_to_qt(const std::filesystem::path& path) {
    return QFile(path).fileName();
}

[[nodiscard]] QString ensure_aeris_suffix(QString path) {
    if (!path.endsWith(QStringLiteral(".aeris"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".aeris");
    }
    return path;
}

[[nodiscard]] std::filesystem::path base_world_cache_path() {
    const QString cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache_root.isEmpty()) return {};
    return filesystem_path_from_qt(
        QDir(cache_root).filePath(QStringLiteral("natural-earth-v5.1.2"))
    );
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
    auto* workspace_view = new MapWorkspaceView(this);
    map_view_ = workspace_view;
    setCentralWidget(map_view_);

    auto* file_menu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* new_action = file_menu->addAction(QStringLiteral("&New project…"));
    new_action->setShortcut(QKeySequence::New);
    connect(new_action, &QAction::triggered, this, &MainWindow::new_project);

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

    auto* data_menu = menuBar()->addMenu(QStringLiteral("&Data"));
    install_base_world_action_ = data_menu->addAction(
        QStringLiteral("Install / repair base political world")
    );
    install_base_world_action_->setToolTip(QStringLiteral(
        "Acquire the exact pinned Natural Earth base map automatically, verify every resource, and commit it into the current .aeris project"
    ));
    connect(
        install_base_world_action_,
        &QAction::triggered,
        this,
        &MainWindow::install_base_world
    );

    data_menu->addSeparator();
    import_world_data_action_ = data_menu->addAction(
        QStringLiteral("Import local Natural Earth snapshot…")
    );
    import_world_data_action_->setToolTip(QStringLiteral(
        "Advanced/offline fallback: import an already acquired exact Natural Earth v5.1.2 110m snapshot"
    ));
    connect(
        import_world_data_action_,
        &QAction::triggered,
        this,
        &MainWindow::import_world_data
    );

    auto* tools_menu = menuBar()->addMenu(QStringLiteral("&Tools"));
    auto* navigate_action = tools_menu->addAction(QStringLiteral("Navigate"));
    navigate_action->setCheckable(true);
    navigate_action->setChecked(true);
    navigate_action->setEnabled(false);
    navigate_action->setToolTip(QStringLiteral(
        "Active map navigation tool: drag, wheel/trackpad, double-click and shortcuts"
    ));

    zoom_in_action_ = tools_menu->addAction(QStringLiteral("Zoom in"));
    zoom_in_action_->setShortcuts(QKeySequence::keyBindings(QKeySequence::ZoomIn));
    connect(zoom_in_action_, &QAction::triggered, map_view_, &MapView::zoom_in);

    zoom_out_action_ = tools_menu->addAction(QStringLiteral("Zoom out"));
    zoom_out_action_->setShortcuts(QKeySequence::keyBindings(QKeySequence::ZoomOut));
    connect(zoom_out_action_, &QAction::triggered, map_view_, &MapView::zoom_out);

    reset_view_action_ = tools_menu->addAction(QStringLiteral("Reset view"));
    reset_view_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(reset_view_action_, &QAction::triggered, map_view_, &MapView::reset_viewport);

    tools_menu->addSeparator();
    auto* unfold_action = tools_menu->addAction(QStringLiteral("Unfold / projection"));
    unfold_action->setCheckable(true);

    auto* view_menu = menuBar()->addMenu(QStringLiteral("&View"));

    auto* toolbar = addToolBar(QStringLiteral("Map tools"));
    toolbar->setObjectName(QStringLiteral("mapTools"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->addAction(new_action);
    toolbar->addAction(open_action);
    toolbar->addSeparator();
    toolbar->addAction(navigate_action);
    toolbar->addAction(zoom_out_action_);
    toolbar->addAction(zoom_in_action_);
    toolbar->addAction(reset_view_action_);
    toolbar->addSeparator();
    toolbar->addAction(unfold_action);

    unfold_dock_ = new QDockWidget(QStringLiteral("Unfold / projection"), this);
    unfold_dock_->setObjectName(QStringLiteral("unfoldDock"));
    auto* unfold_widget = new QWidget(unfold_dock_);
    auto* unfold_layout = new QVBoxLayout(unfold_widget);
    auto* explanation = new QLabel(
        QStringLiteral(
            "Choose a planar surface while the world remains folded as a globe. "
            "Grab the orange cut directly on the globe or use the precision control, "
            "then calculate one verified unfold from that exact cut."
        ),
        unfold_widget
    );
    explanation->setWordWrap(true);
    unfold_layout->addWidget(explanation);

    projection_combo_ = new QComboBox(unfold_widget);
    for (const auto& descriptor : view::projection_catalog()) {
        projection_combo_->addItem(
            QString::fromLatin1(descriptor.display_name),
            static_cast<int>(descriptor.mode)
        );
    }
    connect(
        projection_combo_,
        &QComboBox::currentIndexChanged,
        this,
        [this, workspace_view](const int) {
            const auto mode = static_cast<view::SurfaceMode>(
                projection_combo_->currentData().toInt()
            );
            workspace_view->set_unfold_target_mode(mode);
        }
    );
    unfold_layout->addWidget(projection_combo_);

    cut_value_label_ = new QLabel(
        QStringLiteral("Cut: 0.0° · projection frame"),
        unfold_widget
    );
    unfold_layout->addWidget(cut_value_label_);

    cut_slider_ = new QSlider(Qt::Horizontal, unfold_widget);
    cut_slider_->setRange(-1800, 1800);
    cut_slider_->setSingleStep(10);
    cut_slider_->setPageStep(150);
    cut_slider_->setValue(0);
    cut_slider_->setToolTip(QStringLiteral(
        "Move the physical projection cut without rebuilding the globe"
    ));
    connect(cut_slider_, &QSlider::valueChanged, this, [this](const int value) {
        const double degrees = static_cast<double>(value) / 10.0;
        cut_value_label_->setText(
            QStringLiteral("Cut: %1° · projection frame").arg(degrees, 0, 'f', 1)
        );
        map_view_->set_projection_central_meridian_deg(degrees);
    });
    connect(
        workspace_view,
        &MapWorkspaceView::projectionCutEdited,
        this,
        [this](const double degrees) {
            const QSignalBlocker blocker(cut_slider_);
            const int slider_value = static_cast<int>(std::lround(degrees * 10.0));
            cut_slider_->setValue(slider_value);
            cut_value_label_->setText(
                QStringLiteral("Cut: %1° · projection frame")
                    .arg(degrees, 0, 'f', 1)
            );
        }
    );
    unfold_layout->addWidget(cut_slider_);

    apply_projection_button_ = new QPushButton(
        QStringLiteral("Calculate / unfold"),
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
        refresh_unfold_controls();
    });
    unfold_layout->addWidget(return_globe_button_);
    unfold_layout->addStretch(1);
    unfold_dock_->setWidget(unfold_widget);
    addDockWidget(Qt::RightDockWidgetArea, unfold_dock_);
    unfold_dock_->hide();

    connect(unfold_action, &QAction::toggled, unfold_dock_, &QDockWidget::setVisible);
    connect(
        unfold_dock_,
        &QDockWidget::visibilityChanged,
        this,
        [unfold_action, workspace_view](const bool visible) {
            unfold_action->setChecked(visible);
            workspace_view->set_unfold_tool_active(visible);
        }
    );

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

    data_job_widget_ = new QWidget(statusBar());
    data_job_widget_->setObjectName(QStringLiteral("dataJobStrip"));
    auto* data_job_layout = new QHBoxLayout(data_job_widget_);
    data_job_layout->setContentsMargins(8, 1, 4, 1);
    data_job_layout->setSpacing(7);
    data_job_label_ = new QLabel(QStringLiteral("Preparing data…"), data_job_widget_);
    data_job_label_->setMinimumWidth(220);
    data_job_progress_ = new QProgressBar(data_job_widget_);
    data_job_progress_->setFixedWidth(180);
    data_job_progress_->setTextVisible(true);
    data_job_progress_->setRange(0, 0);
    data_job_cancel_button_ = new QPushButton(QStringLiteral("Cancel"), data_job_widget_);
    data_job_cancel_button_->setToolTip(QStringLiteral(
        "Stop this data job immediately. Verified downloaded bytes are kept for resume."
    ));
    data_job_layout->addWidget(data_job_label_);
    data_job_layout->addWidget(data_job_progress_);
    data_job_layout->addWidget(data_job_cancel_button_);
    data_job_widget_->hide();
    statusBar()->addPermanentWidget(data_job_widget_);
    connect(data_job_cancel_button_, &QPushButton::clicked, this, [this]() {
        if (data_job_ == nullptr) return;
        data_job_cancel_button_->setEnabled(false);
        data_job_label_->setText(QStringLiteral("Cancelling…"));
        data_job_progress_->setRange(0, 0);
        data_job_->request_cancel();
    });

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
        QPushButton:disabled, QComboBox:disabled, QSlider:disabled, QLabel:disabled { color: #737982; }
        QSlider::groove:horizontal { height: 4px; background: #343941; border-radius: 2px; }
        QSlider::handle:horizontal { width: 14px; margin: -5px 0; background: #b7c0ca; border-radius: 7px; }
        QProgressBar { background: #17191c; border: 1px solid #3a4049; border-radius: 4px; min-height: 14px; text-align: center; }
        QProgressBar::chunk { background: #586b7f; border-radius: 3px; }
        QTreeWidget { background: #1b1e22; border: none; outline: none; }
        QTreeWidget::item { padding: 6px 4px; }
        QTreeWidget::item:selected { background: #303640; }
        QLabel { background: transparent; }
    )"));
}

void MainWindow::new_project() {
    QString selected = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Create AERIS project"),
        QStringLiteral("world.aeris"),
        QStringLiteral("AERIS projects (*.aeris);;All files (*)")
    );
    if (selected.isEmpty()) return;
    selected = ensure_aeris_suffix(std::move(selected));

    const std::filesystem::path path = filesystem_path_from_qt(selected);
    std::error_code exists_error;
    if (std::filesystem::exists(path, exists_error)) {
        QMessageBox::warning(
            this,
            QStringLiteral("AERIS project already exists"),
            QStringLiteral("Choose a new project path. Existing .aeris files are never overwritten by New Project.")
        );
        return;
    }
    if (exists_error) {
        QMessageBox::critical(
            this,
            QStringLiteral("AERIS project create failed"),
            QString::fromStdString(exists_error.message())
        );
        return;
    }

    storage::ProjectCreateOptions options{};
    options.timestamp_utc = utc_now();
    options.producer = "aeris-desktop";
    options.producer_version = "0.1.0";
    storage::ProjectStoreResult created = storage::ProjectStore::create(path, options);
    if (!created.ok()) {
        QMessageBox::critical(
            this,
            QStringLiteral("AERIS project create failed"),
            QString::fromStdString(created.status.diagnostic)
        );
        return;
    }

    const storage::Status integrity = created.store->verify_integrity();
    if (!integrity.ok()) {
        QMessageBox::critical(
            this,
            QStringLiteral("AERIS project verification failed"),
            QString::fromStdString(integrity.diagnostic)
        );
        return;
    }

    scene_controller_.cancel();
    project_ = std::move(created.store);
    if (!load_render_model()) {
        project_.reset();
        model_.reset();
        scene_controller_.set_model(nullptr);
        map_view_->clear_project();
        refresh_project_ui();
        return;
    }

    refresh_project_ui();
    install_base_world();
}

void MainWindow::open_project() {
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open AERIS project"),
        QString(),
        QStringLiteral("AERIS projects (*.aeris);;All files (*)")
    );
    if (selected.isEmpty()) return;

    auto opened = storage::ProjectStore::open(filesystem_path_from_qt(selected));
    if (!opened.ok()) {
        QMessageBox::critical(
            this,
            QStringLiteral("AERIS project open failed"),
            QString::fromStdString(opened.status.diagnostic)
        );
        return;
    }

    scene_controller_.cancel();
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
    if (model_ && model_->sources.empty() && !project_->metadata().frozen) {
        install_base_world();
    } else {
        statusBar()->showMessage(QStringLiteral("Opening durable AERIS map…"), 3500);
    }
}

void MainWindow::close_project() {
    scene_controller_.cancel();
    scene_controller_.set_model(nullptr);
    model_.reset();
    project_.reset();
    map_view_->clear_project();
    cut_slider_->setValue(0);
    rebuild_layer_tree();
    refresh_project_ui();
    statusBar()->showMessage(QStringLiteral("Project closed"), 2000);
}

void MainWindow::begin_data_job_ui(
    DataJobProcess* job,
    const QString& initial_phase
) {
    if (job == nullptr) return;
    data_job_label_->setText(initial_phase);
    data_job_progress_->setRange(0, 0);
    data_job_progress_->setValue(0);
    data_job_cancel_button_->setEnabled(true);
    data_job_widget_->show();

    job->set_progress_callback([this, job](const DataJobProgress& progress) {
        if (data_job_ != job) return;
        if (!progress.phase.empty()) {
            data_job_label_->setText(QString::fromStdString(progress.phase));
        }
        const int percent = progress.percent();
        if (percent < 0) {
            data_job_progress_->setRange(0, 0);
        } else {
            data_job_progress_->setRange(0, 100);
            data_job_progress_->setValue(percent);
        }
    });
}

void MainWindow::finish_data_job_ui(DataJobProcess* job) {
    if (data_job_ != job) return;
    data_job_widget_->hide();
    data_job_progress_->setRange(0, 0);
    data_job_progress_->setValue(0);
    data_job_cancel_button_->setEnabled(true);
}

void MainWindow::install_base_world() {
    if (data_job_ != nullptr) {
        statusBar()->showMessage(
            QStringLiteral("Another project data job is already running"),
            2500
        );
        return;
    }
    if (!project_) return;
    if (project_->metadata().frozen) {
        QMessageBox::warning(
            this,
            QStringLiteral("Project is frozen"),
            QStringLiteral("Thaw or copy the project before adding base world data.")
        );
        return;
    }

    const std::filesystem::path cache_root = base_world_cache_path();
    if (cache_root.empty()) {
        QMessageBox::critical(
            this,
            QStringLiteral("Base world acquisition unavailable"),
            QStringLiteral("Qt could not resolve a writable application cache directory.")
        );
        return;
    }

    const std::filesystem::path project_path = project_->path();
    auto* job = new DataJobProcess(this);
    data_job_ = job;
    begin_data_job_ui(job, QStringLiteral("Preparing base political world…"));
    refresh_project_ui();
    statusBar()->showMessage(
        QStringLiteral("Base world acquisition is running in an isolated worker · the interface remains available")
    );

    const bool started = job->start(
        "world-auto",
        project_path,
        cache_root,
        utc_now(),
        [this, job, project_path](DataJobResult result) {
            finish_data_job_ui(job);
            if (data_job_ == job) data_job_ = nullptr;
            job->deleteLater();
            refresh_project_ui();

            if (!project_ || project_->path() != project_path) {
                statusBar()->showMessage(
                    result.cancelled
                        ? QStringLiteral("Base world job cancelled · verified cache bytes were retained")
                        : (result.ok()
                            ? QStringLiteral("Base world committed to its original .aeris project")
                            : QStringLiteral("Base world job failed in its original .aeris project")),
                    5500
                );
                return;
            }

            const storage::Status refreshed = project_->refresh_metadata();
            if (!refreshed.ok()) {
                QMessageBox::critical(
                    this,
                    QStringLiteral("Base world metadata reload failed"),
                    QString::fromStdString(refreshed.diagnostic)
                );
                return;
            }

            // A killed worker may have completed earlier short SQLite
            // transactions even though no final result marker reached stdout.
            // Always reconcile the current project after interactive cancel or
            // failure instead of trusting an in-process changed flag.
            if (!load_render_model()) return;
            refresh_project_ui();

            if (result.cancelled) {
                statusBar()->showMessage(
                    QStringLiteral("Base world job cancelled · partial download retained for automatic resume"),
                    6500
                );
                return;
            }
            if (!result.ok()) {
                QMessageBox::warning(
                    this,
                    QStringLiteral("Base world is not ready"),
                    QString::fromStdString(result.diagnostic)
                );
                return;
            }

            layers_dock_->show();
            statusBar()->showMessage(
                result.changed
                    ? QStringLiteral("Base political world verified and committed to .aeris")
                    : QStringLiteral("Base political world is already present and verified"),
                6500
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
            QStringLiteral("Base world acquisition failed to start"),
            QStringLiteral("Could not launch the isolated AERIS data worker.")
        );
    }
}

void MainWindow::import_world_data() {
    if (data_job_ != nullptr) {
        statusBar()->showMessage(
            QStringLiteral("Another project data job is already running"),
            2500
        );
        return;
    }
    if (!project_) return;
    if (project_->metadata().frozen) {
        QMessageBox::warning(
            this,
            QStringLiteral("Project is frozen"),
            QStringLiteral("Thaw or copy the project before adding external datasets.")
        );
        return;
    }

    const QString selected = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select local Natural Earth v5.1.2 110m snapshot")
    );
    if (selected.isEmpty()) return;

    const std::filesystem::path project_path = project_->path();
    const std::filesystem::path source_root = filesystem_path_from_qt(selected);
    const std::string modified_utc = utc_now();

    auto* job = new DataJobProcess(this);
    data_job_ = job;
    begin_data_job_ui(job, QStringLiteral("Importing local Natural Earth snapshot…"));
    refresh_project_ui();
    statusBar()->showMessage(
        QStringLiteral(
            "Importing Natural Earth in isolated data worker · map remains interactive…"
        )
    );

    const bool started = job->start(
        "world",
        project_path,
        source_root,
        modified_utc,
        [this, job, project_path](DataJobResult imported) {
            finish_data_job_ui(job);
            if (data_job_ == job) data_job_ = nullptr;
            job->deleteLater();
            refresh_project_ui();

            if (!project_ || project_->path() != project_path) {
                statusBar()->showMessage(
                    imported.cancelled
                        ? QStringLiteral("Natural Earth import cancelled in its original .aeris project")
                        : (imported.ok()
                            ? QStringLiteral("Natural Earth import finished in its original .aeris project")
                            : QStringLiteral("Natural Earth import failed in its original .aeris project")),
                    5000
                );
                return;
            }

            const storage::Status refreshed = project_->refresh_metadata();
            if (!refreshed.ok()) {
                QMessageBox::critical(
                    this,
                    QStringLiteral("World data metadata reload failed"),
                    QString::fromStdString(refreshed.diagnostic)
                );
                return;
            }

            if (imported.cancelled) {
                load_render_model();
                refresh_project_ui();
                statusBar()->showMessage(
                    QStringLiteral("Natural Earth import cancelled · durable committed state reconciled"),
                    5500
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
                    QStringLiteral("World data import failed"),
                    QString::fromStdString(imported.diagnostic)
                );
                return;
            }

            if (!load_render_model()) return;
            refresh_project_ui();
            layers_dock_->show();
            statusBar()->showMessage(
                imported.changed
                    ? QStringLiteral("World data committed to .aeris · source directory is no longer required for rendering")
                    : QStringLiteral("World data is already present in this project"),
                6500
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
            QStringLiteral("World data import failed to start"),
            QStringLiteral("Could not launch the isolated AERIS data worker.")
        );
    }
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
    cut_slider_->setValue(0);
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
    const bool visible = item->checkState(0) == Qt::Checked;
    storage::LayerStateUpdate update{};
    update.modified_utc = utc_now();
    update.visible = visible;
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

    if (result.changed) {
        // Visibility changes neither source geometry nor resource bindings.
        // Preserve the already-decoded immutable sources/resources and the
        // verified render frame; copying this snapshot only duplicates the
        // tiny layer vector and shared_ptr maps. Full ProjectModel reload here
        // used to re-stream/decode every flag PNG and the terrain overview.
        auto updated_model = std::make_shared<ProjectModel>(*model_);
        const auto layer = std::find_if(
            updated_model->layers.begin(),
            updated_model->layers.end(),
            [&](const storage::ProjectLayerRecord& record) {
                return record.layer_id == layer_id;
            }
        );
        if (layer == updated_model->layers.end()) {
            // This should be impossible after a successful mutation against the
            // same snapshot; reconcile from durable storage rather than guess.
            if (!load_render_model()) return;
        } else {
            layer->visible = visible;
            model_ = std::move(updated_model);
            scene_controller_.set_model(model_);
            map_view_->set_presentation_model(model_, project_->metadata().revision);
            rebuild_layer_tree();
        }
    } else {
        rebuild_layer_tree();
    }

    refresh_project_ui();
    statusBar()->showMessage(
        result.changed
            ? QStringLiteral("Layer visibility committed to .aeris")
            : QStringLiteral("Layer visibility unchanged"),
        2200
    );
}

void MainWindow::apply_selected_projection() {
    if (!project_ || map_view_->surface_mode() != view::SurfaceMode::globe) return;
    const auto raw = projection_combo_->currentData().toInt();
    const auto mode = static_cast<view::SurfaceMode>(raw);
    if (mode == view::SurfaceMode::globe) return;

    statusBar()->showMessage(
        QStringLiteral("Calculating verified %1 from cut %2°…")
            .arg(QString::fromLatin1(view::surface_mode_name(mode)))
            .arg(map_view_->projection_central_meridian_deg(), 0, 'f', 1)
    );
    map_view_->set_surface_mode(mode);
    refresh_unfold_controls();
}

void MainWindow::refresh_unfold_controls() {
    const bool has_project = project_ != nullptr;
    const bool has_map_data = has_project && model_ && !model_->sources.empty();
    const bool on_globe = has_map_data &&
        map_view_->surface_mode() == view::SurfaceMode::globe;

    projection_combo_->setEnabled(on_globe);
    cut_slider_->setEnabled(on_globe);
    cut_value_label_->setEnabled(on_globe);
    apply_projection_button_->setEnabled(on_globe);
    return_globe_button_->setEnabled(has_map_data && !on_globe);
}

void MainWindow::refresh_project_ui() {
    const bool has_project = project_ != nullptr;
    const bool has_map_data = has_project && model_ && !model_->sources.empty();
    const bool can_write_project_data =
        has_project && !project_->metadata().frozen && data_job_ == nullptr;

    close_project_action_->setEnabled(has_project);
    install_base_world_action_->setEnabled(can_write_project_data);
    import_world_data_action_->setEnabled(can_write_project_data);
    if (auto* action = findChild<QAction*>(QStringLiteral("importCountryFlagsAction"))) {
        action->setEnabled(can_write_project_data && has_map_data);
    }
    if (auto* action = findChild<QAction*>(QStringLiteral("importEtopo2022ElevationAction"))) {
        action->setEnabled(can_write_project_data && has_map_data);
    }
    zoom_in_action_->setEnabled(has_map_data);
    zoom_out_action_->setEnabled(has_map_data);
    reset_view_action_->setEnabled(has_map_data);
    refresh_unfold_controls();

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
    project_state_value_->setText(
        metadata.frozen
            ? QStringLiteral("Frozen")
            : (has_map_data ? QStringLiteral("Editable · world data")
                            : QStringLiteral("Editable · base world pending"))
    );
}

}  // namespace aeris::desktop
