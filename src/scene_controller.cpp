// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_controller.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

#include <mutex>
#include <utility>

namespace aeris::desktop {
namespace {

class SceneTask final : public QRunnable {
public:
    SceneTask(
        QPointer<SceneController> target,
        std::shared_ptr<const ProjectModel> model,
        view::SceneRequest request,
        std::shared_ptr<std::atomic_bool> canceled,
        std::shared_ptr<std::mutex> delivery_mutex,
        const std::uint64_t generation
    )
        : target_(std::move(target)),
          model_(std::move(model)),
          request_(request),
          canceled_(std::move(canceled)),
          delivery_mutex_(std::move(delivery_mutex)),
          generation_(generation) {
        setAutoDelete(true);
    }

    void run() override {
        RenderFrame frame{};
        frame.request = request_;
        frame.source_scenes.reserve(model_->sources.size());

        const auto token = canceled_;
        for (const auto& entry : model_->sources) {
            if (token->load(std::memory_order_relaxed)) return;

            view::SceneGeometry scene = view::build_scene_geometry(
                *entry.second,
                request_,
                [token]() { return token->load(std::memory_order_relaxed); }
            );
            if (scene.canceled || token->load(std::memory_order_relaxed)) return;
            if (!scene.ok) {
                frame.ok = false;
                frame.diagnostic = "source '" + entry.first + "': " + scene.diagnostic;
                break;
            }
            frame.source_scenes.emplace(entry.first, std::move(scene));
        }

        // The controller destructor uses the same gate before invalidating this
        // token. If delivery wins the gate, invokeMethod is queued while the
        // QObject is still alive and Qt owns the queued-call lifetime. If close
        // wins, the token is already canceled and no QObject API is touched.
        std::lock_guard<std::mutex> delivery_guard(*delivery_mutex_);
        if (token->load(std::memory_order_relaxed)) return;
        QPointer<SceneController> target = target_;
        if (!target) return;
        QMetaObject::invokeMethod(
            target.data(),
            [target, generation = generation_, frame = std::move(frame)]() mutable {
                if (target) target->accept_frame(generation, std::move(frame));
            },
            Qt::QueuedConnection
        );
    }

private:
    QPointer<SceneController> target_;
    std::shared_ptr<const ProjectModel> model_;
    view::SceneRequest request_{};
    std::shared_ptr<std::atomic_bool> canceled_;
    std::shared_ptr<std::mutex> delivery_mutex_;
    std::uint64_t generation_{0U};
};

}  // namespace

SceneController::SceneController(QObject* parent)
    : QObject(parent),
      pool_(new QThreadPool),
      delivery_mutex_(std::make_shared<std::mutex>()) {
    pool_->setMaxThreadCount(1);
    pool_->setExpiryTimeout(1000);
}

SceneController::~SceneController() {
    cancel();
    if (pool_ == nullptr) return;

    // Never make top-level window destruction a join point. If cancellation has
    // already drained the pool, reclaim it normally. Otherwise the pool stays
    // alive while its cancellation-aware runnable unwinds; all runnable input
    // state and the delivery gate are shared-owned, and cancellation has closed
    // the only QObject handoff before the controller can be destroyed.
    pool_->clear();
    if (pool_->waitForDone(0)) delete pool_;
    pool_ = nullptr;
}

void SceneController::set_model(std::shared_ptr<const ProjectModel> model) {
    cancel();
    model_ = std::move(model);
}

void SceneController::set_frame_callback(FrameCallback callback) {
    frame_callback_ = std::move(callback);
}

void SceneController::set_busy_callback(BusyCallback callback) {
    busy_callback_ = std::move(callback);
}

void SceneController::request(const view::SceneRequest& request) {
    if (!model_ || pool_ == nullptr) return;

    if (request.quality == view::SceneQuality::preview && task_running_) {
        if (active_quality_ == view::SceneQuality::preview) {
            // Keep the already-running interactive frame useful and retain only
            // the newest camera request behind it. This bounds the queue to one
            // in-flight preview plus one latest viewport instead of repeatedly
            // canceling work for every mouse-move event.
            pending_preview_ = request;
            return;
        }

        // User interaction takes priority over a release-time verified build.
        // Preempt the expensive verified generation rather than making the
        // first visible drag frame wait behind it.
        invalidate_active_task();
    } else if (request.quality == view::SceneQuality::verified) {
        // A verified request is authoritative for the release-time camera.
        // Discard any queued preview target and invalidate currently running
        // work so the final camera cannot be followed by an older preview.
        pending_preview_.reset();
        if (task_running_) invalidate_active_task();
    }

    start_request(request);
}

void SceneController::cancel() {
    pending_preview_.reset();
    invalidate_active_task();
    set_busy_state(false);
}

void SceneController::accept_frame(
    const std::uint64_t generation,
    RenderFrame frame
) {
    if (generation != generation_) return;

    task_running_ = false;
    const bool completed_preview =
        frame.request.quality == view::SceneQuality::preview;

    if (frame_callback_) frame_callback_(std::move(frame));

    if (completed_preview && pending_preview_ && model_) {
        const view::SceneRequest next = *pending_preview_;
        pending_preview_.reset();
        start_request(next);
        return;
    }

    set_busy_state(false);
}

void SceneController::start_request(const view::SceneRequest& request) {
    if (!model_ || pool_ == nullptr) return;

    ++generation_;
    const std::uint64_t generation = generation_;
    cancel_token_ = std::make_shared<std::atomic_bool>(false);
    active_quality_ = request.quality;
    task_running_ = true;
    set_busy_state(true);
    pool_->start(new SceneTask(
        QPointer<SceneController>(this),
        model_,
        request,
        cancel_token_,
        delivery_mutex_,
        generation
    ));
}

void SceneController::invalidate_active_task() {
    if (cancel_token_) {
        std::lock_guard<std::mutex> delivery_guard(*delivery_mutex_);
        cancel_token_->store(true, std::memory_order_relaxed);
    }
    if (pool_ != nullptr) pool_->clear();
    ++generation_;
    task_running_ = false;
}

void SceneController::set_busy_state(const bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    if (busy_callback_) busy_callback_(busy_);
}

}  // namespace aeris::desktop
