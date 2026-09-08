// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_view.hpp"

#include <cmath>

namespace aeris::desktop {

void MapView::set_projection_central_meridian_deg(const double degrees) {
    // Editing the cut while a flat frame is visible would immediately make the
    // control state disagree with the authoritative geometry on screen. The
    // product workflow therefore edits the cut only on the folded Globe and
    // performs one verified rebuild when the user applies the unfold.
    if (!model_ || mode_ != view::SurfaceMode::globe || !std::isfinite(degrees)) {
        return;
    }

    const double normalized = std::remainder(degrees, 360.0);
    if (std::abs(normalized - projection_central_meridian_deg_) <= 1e-12) {
        return;
    }

    projection_central_meridian_deg_ = normalized;
    // Deliberately no SceneRequest here: the world frame did not change. Only
    // the lightweight tool overlay needs repainting until Apply / unfold.
    update();
}

}  // namespace aeris::desktop
