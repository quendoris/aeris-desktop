// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/view/scene.hpp"
#include "project_model.hpp"

#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace aeris::desktop {

struct RenderFrame final {
    view::SceneRequest request{};
    std::unordered_map<std::string, view::SceneGeometry> source_scenes;
    bool ok{true};
    std::string diagnostic;
};

class SceneController final : public QObject {
    Q_OBJECT

public:
    using FrameCallback = std::function<void(RenderFrame)>;
    using BusyCallback = std::function<void(bool)>;

    explicit SceneController(QObject* parent = nullptr);
    ~SceneController() override;

    void set_model(std::shared_ptr<const ProjectModel> model);
    void set_frame_callback(FrameCallback callback);
    void set_busy_callback(BusyCallback callback);
    void request(const view::SceneRequest& request);
    void cancel();

    // Public only as the queued delivery boundary used by the private worker.
    // Callers should request scenes through request().
    void accept_frame(std::uint64_t generation, RenderFrame frame);

private:
    void start_request(const view::SceneRequest& request);
    void set_busy_state(bool busy);

    std::shared_ptr<const ProjectModel> model_;
    QThreadPool pool_;
    FrameCallback frame_callback_;
    BusyCallback busy_callback_;
    std::shared_ptr<std::atomic_bool> cancel_token_;
    std::optional<view::SceneRequest> pending_preview_;
    view::SceneQuality active_quality_{view::SceneQuality::preview};
    std::uint64_t generation_{0U};
    bool task_running_{false};
    bool busy_{false};
};

}  // namespace aeris::desktop
