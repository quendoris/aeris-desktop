// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/view/scene.hpp"
#include "elevation_renderer.hpp"
#include "project_model.hpp"
#include "scene_controller.hpp"

#include <QPoint>
#include <QPointF>
#include <QWidget>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace aeris::desktop {

// MapView owns canonical map presentation and navigation. Tool-specific
// overlays may derive from it, but they must not reinterpret project geometry.
class MapView : public QWidget {
    Q_OBJECT

public:
    using SceneRequestCallback = std::function<void(const view::SceneRequest&)>;

    explicit MapView(QWidget* parent = nullptr);

    void set_project(
        std::shared_ptr<const ProjectModel> model,
        std::string project_uuid,
        std::uint64_t revision);
    void set_project_model(
        std::shared_ptr<const ProjectModel> model,
        std::uint64_t revision);
    void clear_project();

    void set_scene_request_callback(SceneRequestCallback callback);
    void set_frame(RenderFrame frame);
    void set_busy(bool busy);
    void set_surface_mode(view::SurfaceMode mode);

    // The projection cut is edited on the folded Globe without rebuilding map
    // geometry. Applying a planar surface later carries this value through the
    // ordinary verified SceneRequest boundary.
    void set_projection_central_meridian_deg(double degrees);

    void zoom_in();
    void zoom_out();
    void reset_viewport();
    [[nodiscard]] view::SurfaceMode surface_mode() const noexcept { return mode_; }
    [[nodiscard]] double projection_central_meridian_deg() const noexcept {
        return projection_central_meridian_deg_;
    }

    // Tool overlays need the camera that produced the frame actually visible
    // underneath them, not a newer camera whose async preview is still pending.
    [[nodiscard]] bool has_current_frame() const noexcept {
        return has_frame_ && frame_.request.mode == mode_;
    }
    [[nodiscard]] double displayed_camera_longitude_deg() const noexcept {
        return has_current_frame() ? frame_.request.camera_longitude_deg : longitude_deg_;
    }
    [[nodiscard]] double displayed_camera_latitude_deg() const noexcept {
        return has_current_frame() ? frame_.request.camera_latitude_deg : latitude_deg_;
    }

    // Read-only viewport state is intentionally exposed for deterministic
    // offscreen interaction proofs and tool overlays. Rendering policy remains
    // private to MapView.
    [[nodiscard]] double zoom_factor() const noexcept { return zoom_; }
    [[nodiscard]] QPointF viewport_pan() const noexcept { return viewport_pan_; }

protected:
    // Workspace overlays may compose presentation from the exact durable model
    // and frame already rendered underneath them. They are read-only views: all
    // world geometry construction and project ownership remain in MapView/core.
    [[nodiscard]] const ProjectModel* current_project_model() const noexcept {
        return model_.get();
    }
    [[nodiscard]] const RenderFrame* current_render_frame() const noexcept {
        return has_current_frame() ? &frame_ : nullptr;
    }

    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct ViewportState final {
        double zoom{1.0};
        QPointF pan{};
    };

    void request_scene(view::SceneQuality quality);
    void apply_zoom(double factor, const QPointF& anchor);
    void store_active_viewport() noexcept;
    void restore_active_viewport() noexcept;
    [[nodiscard]] static std::size_t viewport_index(view::SurfaceMode mode) noexcept;

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
    double projection_central_meridian_deg_{0.0};
    double zoom_{1.0};
    QPointF viewport_pan_{};
    std::array<ViewportState, 4U> viewports_{};
    ElevationSurfaceCache elevation_surface_cache_{};

    QPoint last_mouse_{};
    bool dragging_{false};
    SceneRequestCallback scene_request_callback_;
};

}  // namespace aeris::desktop
