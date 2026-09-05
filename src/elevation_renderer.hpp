// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "project_model.hpp"
#include "scene_controller.hpp"

#include "aeris/storage/layer.hpp"
#include "aeris/view/scene.hpp"

#include <QImage>
#include <QPointF>

#include <string>

class QPainter;

namespace aeris::desktop {

struct ElevationSurfaceCache final {
    const ProjectModel* model{nullptr};
    std::string layer_id;
    view::SurfaceMode mode{view::SurfaceMode::globe};
    double camera_longitude_deg{0.0};
    double camera_latitude_deg{0.0};
    double projection_central_meridian_deg{0.0};
    double zoom{0.0};
    QPointF pan{};
    int width{0};
    int height{0};
    QImage image;
};

// Reprojects a derived geographic hillshade/hypsometric overview into the exact
// surface currently rendered by MapView. Numerical elevation remains the only
// durable state; this cache is screen-space presentation and can be discarded.
void draw_elevation_overview(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const RenderFrame& frame,
    const ProjectModel& model,
    double zoom,
    QPointF pan,
    ElevationSurfaceCache& cache);

}  // namespace aeris::desktop
