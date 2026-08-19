// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "map_view.hpp"

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
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

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    build_ui();
    apply_theme();
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
    unfold_layout->addWidget(new QLabel(
        QStringLiteral(
            "Projection is a tool applied to the current map, not a permanent application mode."
        ),
        unfold_widget
    ));
    projection_combo_ = new QComboBox(unfold_widget);
    projection_combo_->addItem(QStringLiteral("Sinusoidal"), QStringLiteral("aeris.projection.sinusoidal.v1"));
    projection_combo_->addItem(QStringLiteral("Mollweide"), QStringLiteral("aeris.projection.mollweide.v1"));
    projection_combo_->addItem(
        QStringLiteral("Lambert cylindrical equal-area"),
        QStringLiteral("aeris.projection.lambert-cylindrical-equal-area.v1")
    );
    unfold_layout->addWidget(projection_combo_);
    auto* apply_projection = new QPushButton(QStringLiteral("Apply / unfold"), unfold_widget);
    apply_projection->setEnabled(false);
    apply_projection->setToolTip(QStringLiteral("Enabled when the desktop render path is connected"));
    unfold_layout->addWidget(apply_projection);
    unfold_layout->addStretch(1);
    unfold_dock_->setWidget(unfold_widget);
    addDockWidget(Qt::RightDockWidgetArea, unfold_dock_);
    unfold_dock_->hide();

    connect(unfold_action, &QAction::toggled, unfold_dock_, &QDockWidget::setVisible);
    connect(unfold_dock_, &QDockWidget::visibilityChanged, unfold_action, &QAction::setChecked);

    layers_dock_ = new QDockWidget(QStringLiteral("Layers"), this);
    layers_dock_->setObjectName(QStringLiteral("layersDock"));
    auto* layers_placeholder = new QLabel(
        QStringLiteral("Project layer stack will appear here after the durable layer reader is connected."),
        layers_dock_
    );
    layers_placeholder->setWordWrap(true);
    layers_placeholder->setMargin(12);
    layers_dock_->setWidget(layers_placeholder);
    addDockWidget(Qt::RightDockWidgetArea, layers_dock_);
    layers_dock_->hide();
    view_menu->addAction(layers_dock_->toggleViewAction());

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

    auto opened = aeris::storage::ProjectStore::open(
        std::filesystem::path(selected.toStdString())
    );
    if (!opened.ok()) {
        QMessageBox::critical(
            this,
            QStringLiteral("AERIS project open failed"),
            QString::fromStdString(opened.status.diagnostic)
        );
        return;
    }

    project_ = std::move(opened.store);
    refresh_project_ui();
    statusBar()->showMessage(QStringLiteral("Project accepted by aeris-core"), 3500);
}

void MainWindow::close_project() {
    project_.reset();
    refresh_project_ui();
    statusBar()->showMessage(QStringLiteral("Project closed"), 2000);
}

void MainWindow::refresh_project_ui() {
    close_project_action_->setEnabled(project_ != nullptr);

    if (!project_) {
        map_view_->clear_project();
        project_path_value_->setText(QStringLiteral("—"));
        project_uuid_value_->setText(QStringLiteral("—"));
        project_revision_value_->setText(QStringLiteral("—"));
        project_format_value_->setText(QStringLiteral("—"));
        project_projection_value_->setText(QStringLiteral("—"));
        project_state_value_->setText(QStringLiteral("No project"));
        return;
    }

    const auto& metadata = project_->metadata();
    map_view_->set_project_summary(metadata.project_uuid, metadata.revision);
    project_path_value_->setText(QString::fromStdString(project_->path().string()));
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
