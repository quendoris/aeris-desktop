// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_controller.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

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
        const std::uint64_t generation
    )
        : target_(std::move(target)),
          model_(std::move(model)),
          request_(request),
          canceled_(std::move(canceled)),
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

        QPointer<SceneController> target = target_;
        QMetaObject::invokeMethod(
            target_,
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
    std::uint64_t generation_{0U};
};

}  // namespace

SceneController::SceneController(QObject* parent)
    : QObject(parent) {
    pool_.setMaxThreadCount(1);
}

SceneController::~SceneController() {
    cancel();
    pool_.waitForDone();
}

void SceneController::set_model(std::shared_ptr<const ProjectModel> model) {
    cancel();
    model_ = std::move(model);
    if (busy_callback_) busy_callback_(false);
}

void SceneController::set_frame_callback(FrameCallback callback) {
    frame_callback_ = std::move(callback);
}

void SceneController::set_busy_callback(BusyCallback callback) {
    busy_callback_ = std::move(callback);
}

void SceneController::request(const view::SceneRequest& request) {
    cancel();
    if (!model_) return;

    const std::uint64_t generation = generation_;
    cancel_token_ = std::make_shared<std::atomic_bool>(false);
    if (busy_callback_) busy_callback_(true);
    pool_.start(new SceneTask(
        QPointer<SceneController>(this),
        model_,
        request,
        cancel_token_,
        generation
    ));
}

void SceneController::cancel() {
    if (cancel_token_) cancel_token_->store(true, std::memory_order_relaxed);
    // A one-thread pool can otherwise accumulate obsolete mouse-move previews
    // behind the currently running generation. They are already stale before
    // they start, so discard them and let only the newest request queue next.
    pool_.clear();
    ++generation_;
}

void SceneController::accept_frame(
    const std::uint64_t generation,
    RenderFrame frame
) {
    if (generation != generation_) return;
    if (busy_callback_) busy_callback_(false);
    if (frame_callback_) frame_callback_(std::move(frame));
}

}  // namespace aeris::desktop
