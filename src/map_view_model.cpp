// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_view.hpp"

#include <utility>

namespace aeris::desktop {

void MapView::set_project_model(
    std::shared_ptr<const ProjectModel> model,
    const std::uint64_t revision
) {
    model_ = std::move(model);
    revision_ = revision;
    update();
}

}  // namespace aeris::desktop
