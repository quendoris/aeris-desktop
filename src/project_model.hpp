// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/storage/project.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aeris::desktop {

struct ProjectModel final {
    std::vector<storage::ProjectLayerRecord> layers;
    std::unordered_map<std::string, std::shared_ptr<const source::Result>> sources;
};

struct ProjectModelLoadResult final {
    std::shared_ptr<const ProjectModel> model;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return model != nullptr; }
};

[[nodiscard]] ProjectModelLoadResult load_project_model(
    const storage::ProjectStore& project);

}  // namespace aeris::desktop
