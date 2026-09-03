// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "map_view.hpp"

namespace aeris::desktop {

// Tool overlays are presentation-only. The base MapView remains responsible
// for canonical scene/layer rendering and navigation, while this workspace can
// add transient editing guides such as the projection cut.
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

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool unfold_tool_active_{false};
    view::SurfaceMode unfold_target_mode_{view::SurfaceMode::sinu_mollweide};
};

}  // namespace aeris::desktop
