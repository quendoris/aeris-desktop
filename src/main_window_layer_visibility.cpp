// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "data_job_process.hpp"
#include "map_view.hpp"

#include "aeris/storage/layer.hpp"

#include <QDateTime>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace aeris::desktop {
namespace {

[[nodiscard]] std::string visibility_utc_now() {
    return QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODate)
        .toStdString();
}

}  // namespace

void MainWindow::install_layer_visibility_coordinator() {
    if (layer_tree_ == nullptr) return;

    // build_ui() installs the direct durable mutation path for standalone
    // MainWindow use. The production application upgrades that connection to
    // this writer-aware coordinator once all optional UI integrations exist.
    QObject::disconnect(layer_tree_, nullptr, this, nullptr);
    connect(
        layer_tree_,
        &QTreeWidget::itemChanged,
        this,
        [this](QTreeWidgetItem* item, const int column) {
            if (column == 0) set_layer_visibility_coordinated(item);
        }
    );
}

void MainWindow::set_layer_visibility_coordinated(QTreeWidgetItem* item) {
    if (rebuilding_layers_ || item == nullptr || !project_ || !model_) return;

    const bool writer_owns_current_project =
        data_job_ != nullptr &&
        data_job_->running() &&
        data_job_->project_path() == project_->path();
    if (!writer_owns_current_project) {
        set_layer_visibility(item);
        return;
    }

    const std::string layer_id = item->data(0, Qt::UserRole).toString().toStdString();
    const bool visible = item->checkState(0) == Qt::Checked;

    // Visibility does not change canonical geometry or resource bindings. Apply
    // it to the immutable presentation snapshot immediately, without touching
    // SQLite while the isolated importer owns the same project writer path.
    auto updated_model = std::make_shared<ProjectModel>(*model_);
    const auto layer = std::find_if(
        updated_model->layers.begin(),
        updated_model->layers.end(),
        [&](const storage::ProjectLayerRecord& record) {
            return record.layer_id == layer_id;
        }
    );
    if (layer == updated_model->layers.end()) {
        rebuild_layer_tree();
        return;
    }

    layer->visible = visible;
    model_ = std::move(updated_model);
    scene_controller_.set_model(model_);
    map_view_->set_presentation_model(model_, project_->metadata().revision);
    rebuild_layer_tree();

    // Coalesce repeated checkbox edits to the latest requested state. The queue
    // is keyed by durable project path so switching to another project while an
    // import continues never redirects a deferred mutation to the wrong file.
    deferred_layer_visibility_[project_->path()][layer_id] = visible;
    schedule_deferred_layer_visibility_flush();
    refresh_project_ui();
    statusBar()->showMessage(
        QStringLiteral(
            "Layer visibility applied · durable save queued behind the active data job"
        ),
        3000
    );
}

void MainWindow::schedule_deferred_layer_visibility_flush() {
    if (deferred_layer_visibility_.empty() ||
        deferred_layer_visibility_flush_scheduled_) {
        return;
    }
    deferred_layer_visibility_flush_scheduled_ = true;
    QTimer::singleShot(50, this, [this]() {
        deferred_layer_visibility_flush_scheduled_ = false;
        flush_deferred_layer_visibility();
    });
}

void MainWindow::flush_deferred_layer_visibility() {
    if (deferred_layer_visibility_.empty()) return;

    bool waiting_for_writer = false;
    bool current_project_changed = false;
    std::string last_failure;

    for (auto project_it = deferred_layer_visibility_.begin();
         project_it != deferred_layer_visibility_.end();) {
        const std::filesystem::path project_path = project_it->first;
        if (data_job_ != nullptr &&
            data_job_->running() &&
            data_job_->project_path() == project_path) {
            waiting_for_writer = true;
            ++project_it;
            continue;
        }

        storage::ProjectStore* store = nullptr;
        std::unique_ptr<storage::ProjectStore> temporary_store;
        if (project_ && project_->path() == project_path) {
            store = project_.get();
        } else {
            storage::ProjectStoreResult opened = storage::ProjectStore::open(project_path);
            if (!opened.ok()) {
                last_failure = opened.status.diagnostic.empty()
                    ? "could not reopen project for deferred layer visibility"
                    : opened.status.diagnostic;
                project_it = deferred_layer_visibility_.erase(project_it);
                continue;
            }
            temporary_store = std::move(opened.store);
            store = temporary_store.get();
        }

        for (const auto& [layer_id, visible] : project_it->second) {
            storage::LayerStateUpdate update{};
            update.modified_utc = visibility_utc_now();
            update.visible = visible;
            const storage::LayerMutationResult result =
                storage::update_layer_state(*store, layer_id, update);
            if (!result.ok()) {
                last_failure = result.status.diagnostic.empty()
                    ? "deferred layer visibility mutation was rejected"
                    : result.status.diagnostic;
                continue;
            }
            if (project_ && project_->path() == project_path && result.changed) {
                current_project_changed = true;
            }
        }

        project_it = deferred_layer_visibility_.erase(project_it);
    }

    if (current_project_changed && project_) {
        const storage::Status refreshed = project_->refresh_metadata();
        if (!refreshed.ok()) {
            last_failure = refreshed.diagnostic.empty()
                ? "could not refresh project metadata after deferred visibility commit"
                : refreshed.diagnostic;
        } else {
            load_render_model();
            refresh_project_ui();
        }
    }

    if (!last_failure.empty()) {
        statusBar()->showMessage(
            QStringLiteral("Deferred layer save failed: %1")
                .arg(QString::fromStdString(last_failure)),
            6500
        );
    } else if (current_project_changed) {
        statusBar()->showMessage(
            QStringLiteral("Deferred layer visibility committed to .aeris"),
            2500
        );
    }

    if (waiting_for_writer && !deferred_layer_visibility_.empty()) {
        schedule_deferred_layer_visibility_flush();
    }
}

}  // namespace aeris::desktop
