// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/view/scene.hpp"
#include "project_model.hpp"
#include "scene_controller.hpp"

#include <QPoint>
#include <QPointF>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>

class QMouseEvent;
class QWheelEvent;

namespace aeris::desktop {

class MapView final : public QWidget {
    Q_OBJECT

public:
    using SceneRequestCallback = std::function<void(const view::SceneRequest&)>;

    explicit MapView(QWidget* parent = nullptr);

    void set_project(
        std::shared_ptr<const ProjectModel> model,
        std::string project_uuid,
        std::uint64_t revision);
    void clear_project();

    void set_scene_request_callback(SceneRequestCallback callback);
    void set_frame(RenderFrame frame);
    void set_busy(bool busy);
    void set_surface_mode(view::SurfaceMode mode);
    [[nodiscard]] view::SurfaceMode surface_mode() const noexcept { return mode_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void request_scene(view::SceneQuality quality);

    std::shared_ptr<const ProjectModel> model_;
    std::string project_uuid_;
    std::uint64_t revision_{0U};
    RenderFrame frame_{};
    bool has_frame_{false};
    bool busy_{false};
    std::string frame_error_;

    view::SurfaceMode mode_{view::SurfaceMode::globe};
    double longitude_deg_{15.0};
    double latitude_deg_{20.0};
    double zoom_{1.0};
    QPointF flat_pan_{};

    QPoint last_mouse_{};
    bool dragging_{false};
    SceneRequestCallback scene_request_callback_;
};

}  // namespace aeris::desktop
