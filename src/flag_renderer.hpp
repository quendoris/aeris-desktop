// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "project_model.hpp"

#include "aeris/source/adapter.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/view/scene.hpp"

class QPainter;

namespace aeris::desktop {

void draw_country_flags(
    QPainter& painter,
    const storage::ProjectLayerRecord& layer,
    const view::SceneGeometry& scene,
    const source::Result& source_result,
    const ProjectModel& model);

}  // namespace aeris::desktop
