// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "project_model.hpp"

#include "aeris/project/source_reader.hpp"
#include "aeris/storage/resource.hpp"

#include <cstring>
#include <limits>
#include <unordered_map>
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
    std::unordered_set<std::string> resource_ids;
    for (const storage::ProjectLayerRecord& layer : model->layers) {
        for (const storage::LayerSourceBinding& binding : layer.sources) {
            source_ids.insert(binding.source_id);
        }
        for (const storage::LayerResourceBinding& binding : layer.resources) {
            resource_ids.insert(binding.resource_id);
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

    if (!resource_ids.empty()) {
        const storage::ProjectResourceListResult listed_resources =
            storage::list_project_resources(project);
        if (!listed_resources.ok()) {
            return {
                nullptr,
                "unable to enumerate project resources: " +
                    listed_resources.status.diagnostic,
            };
        }

        std::unordered_map<std::string, const storage::ProjectResourceRecord*> records;
        records.reserve(listed_resources.records.size());
        for (const storage::ProjectResourceRecord& record : listed_resources.records) {
            records.emplace(record.identity.resource_id, &record);
        }

        for (const std::string& resource_id : resource_ids) {
            const auto found = records.find(resource_id);
            if (found == records.end()) {
                return {nullptr, "layer references missing project resource '" + resource_id + "'"};
            }
            const storage::ProjectResourceRecord& record = *found->second;
            if (record.storage_mode != storage::ResourceStorageMode::embedded) {
                // External resources have no machine-local path in the durable
                // model. They remain valid project metadata but are unavailable
                // to the renderer until a retrieval/import step embeds them.
                continue;
            }
            if (record.identity.size_bytes >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return {nullptr, "embedded resource is too large for this process: " + resource_id};
            }

            auto resource = std::make_shared<EmbeddedProjectResource>();
            resource->media_type = record.identity.media_type;
            resource->bytes.reserve(static_cast<std::size_t>(record.identity.size_bytes));
            const storage::Status streamed = storage::stream_embedded_resource(
                project,
                resource_id,
                [&](const void* data, const std::size_t size) {
                    const auto* begin = static_cast<const std::uint8_t*>(data);
                    resource->bytes.insert(resource->bytes.end(), begin, begin + size);
                    return storage::Status::success();
                }
            );
            if (!streamed.ok()) {
                return {
                    nullptr,
                    "unable to reconstruct embedded resource '" + resource_id + "': " +
                        streamed.diagnostic,
                };
            }
            if (resource->bytes.size() != static_cast<std::size_t>(record.identity.size_bytes)) {
                return {nullptr, "embedded resource size changed during reconstruction: " + resource_id};
            }
            model->resources.emplace(resource_id, std::move(resource));
        }
    }

    return {std::move(model), {}};
}

}  // namespace aeris::desktop
