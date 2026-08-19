// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "project_model.hpp"

#include "aeris/project/source_reader.hpp"

#include <unordered_set>
#include <utility>

namespace aeris::desktop {

ProjectModelLoadResult load_project_model(const storage::ProjectStore& project) {
    const storage::ProjectLayerListResult listed = storage::list_project_layers(project);
    if (!listed.ok()) {
        return {
            nullptr,
            listed.status.diagnostic.empty()
                ? "unable to enumerate project layers"
                : listed.status.diagnostic,
        };
    }

    auto model = std::make_shared<ProjectModel>();
    model->layers = listed.records;

    std::unordered_set<std::string> source_ids;
    for (const storage::ProjectLayerRecord& layer : model->layers) {
        for (const storage::LayerSourceBinding& binding : layer.sources) {
            source_ids.insert(binding.source_id);
        }
    }

    for (const std::string& source_id : source_ids) {
        project::DurableSourceLoadResult loaded =
            project::load_durable_source_result(project, source_id);
        if (!loaded.ok()) {
            return {
                nullptr,
                "unable to load durable source '" + source_id + "': " +
                    loaded.diagnostic,
            };
        }
        model->sources.emplace(
            source_id,
            std::make_shared<const source::Result>(std::move(loaded.source))
        );
    }

    return {std::move(model), {}};
}

}  // namespace aeris::desktop
