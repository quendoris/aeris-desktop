// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "flag_renderer.hpp"
#include "flag_resource_loader.hpp"
#include "map_view.hpp"

#include <filesystem>
#include <vector>

namespace aeris::desktop {

// Tool overlays are presentation-only. The base MapView remains responsible
// for canonical scene/layer rendering and navigation, while this workspace can
// add transient editing guides such as the projection cut and rebuildable
// viewport-bounded symbol caches.
class MapWorkspaceView final : public MapView {
    Q_OBJECT

public:
    explicit MapWorkspaceView(QWidget* parent = nullptr);

    void set_unfold_tool_active(bool active);
    void set_unfold_target_mode(view::SurfaceMode mode);

    [[nodiscard]] bool unfold_tool_active() const noexcept {
        return unfold_tool_active_;
    }
    [[nodiscard]] view::SurfaceMode unfold_target_mode() const noexcept {
        return unfold_target_mode_;
    }

signals:
    // Emitted only for direct manipulation on the Globe. MainWindow mirrors
    // this value into precision controls without feeding it back into the map.
    void projectionCutEdited(double degrees);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void synchronize_flag_project(const ProjectModel* model);
    void dispatch_flag_resource_requests();
    void accept_flag_resource_results(
        std::filesystem::path project_path,
        std::vector<FlagResourceLoadResult> results);

    bool unfold_tool_active_{false};
    bool dragging_projection_cut_{false};
    bool projection_cut_pointer_active_{false};
    QPointF projection_cut_pointer_device_{};
    view::SurfaceMode unfold_target_mode_{view::SurfaceMode::sinu_mollweide};

    FlagResourceLoader flag_resource_loader_;
    FlagRenderCache flag_render_cache_;
    std::filesystem::path flag_project_path_;
};

}  // namespace aeris::desktop
