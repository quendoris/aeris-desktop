// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "flag_pack_import.hpp"
#include "project_model.hpp"
#include "world_data_import.hpp"

#include "aeris/storage/project.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <variant>

namespace {

constexpr std::string_view kTimestamp = "2026-09-05T11:00:00Z";
constexpr std::string_view kPoliticalSourceId = "world.admin0.natural-earth-110m";

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "aeris_desktop_project_lifecycle_probe: FAIL " << diagnostic << '\n';
    return code;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 4) {
        return fail(2, "usage: <natural-earth-directory> <country-flags-directory> <output.aeris>");
    }

    const std::filesystem::path source_root = argv[1];
    const std::filesystem::path flags_root = argv[2];
    const std::filesystem::path output = argv[3];
    if (std::filesystem::exists(output)) {
        return fail(3, "output project already exists");
    }

    aeris::storage::ProjectCreateOptions options{};
    options.timestamp_utc = std::string(kTimestamp);
    options.producer = "aeris-desktop";
    options.producer_version = "0.1.0";
    auto created = aeris::storage::ProjectStore::create(output, options);
    if (!created.ok()) {
        return fail(4, "empty project creation failed: " + created.status.diagnostic);
    }

    auto empty = aeris::desktop::load_project_model(*created.store);
    if (!empty.ok() || !empty.model || !empty.model->layers.empty() ||
        !empty.model->sources.empty() || !empty.model->resources.empty()) {
        return fail(5, "fresh .aeris is not a valid empty desktop project");
    }

    const auto imported = aeris::desktop::import_natural_earth_110m_world(
        *created.store,
        source_root,
        kTimestamp
    );
    if (!imported.ok()) {
        return fail(6, "world data import failed: " + imported.diagnostic);
    }

    auto expanded = aeris::desktop::load_project_model(*created.store);
    if (!expanded.ok() || !expanded.model) {
        return fail(7, "expanded project reload failed: " + expanded.diagnostic);
    }
    if (expanded.model->layers.size() != 5U || expanded.model->sources.size() != 2U ||
        !expanded.model->resources.empty()) {
        return fail(8, "world import did not expand project to 5 layers / 2 sources / 0 resources");
    }

    const auto political = expanded.model->sources.find(std::string(kPoliticalSourceId));
    if (political == expanded.model->sources.end() || !political->second ||
        !political->second->feature_properties_complete) {
        return fail(9, "durable political source is missing its complete property channel");
    }

    std::set<std::int64_t> palette_assignments;
    for (const auto& feature : political->second->features) {
        bool found_assignment = false;
        for (const auto& property : feature.properties) {
            if (property.key != "mapcolor7") continue;
            const auto* value = std::get_if<std::int64_t>(&property.value);
            if (value == nullptr || *value < 1 || *value > 7) {
                return fail(10, "durable mapcolor7 property has wrong type or value");
            }
            palette_assignments.insert(*value);
            found_assignment = true;
            break;
        }
        if (!found_assignment) {
            return fail(11, "durable political feature lost mapcolor7 during .aeris round-trip");
        }
    }
    if (palette_assignments.size() != 7U) {
        return fail(12, "durable political source does not retain all seven palette classes");
    }

    const auto flags = aeris::desktop::import_country_flag_png_pack(
        *created.store,
        flags_root,
        kTimestamp
    );
    if (!flags.ok()) {
        return fail(13, "country flag pack import failed: " + flags.diagnostic);
    }
    if (flags.flag_count < 150U || flags.flag_count > 256U) {
        return fail(14, "country flag importer produced an implausible ISO flag count");
    }

    auto decorated = aeris::desktop::load_project_model(*created.store);
    if (!decorated.ok() || !decorated.model) {
        return fail(15, "flag-decorated project reload failed: " + decorated.diagnostic);
    }
    if (decorated.model->layers.size() != 6U || decorated.model->sources.size() != 2U ||
        decorated.model->resources.size() != flags.flag_count) {
        return fail(16, "flag import did not produce 6 layers / 2 sources / exact embedded resources");
    }

    const aeris::storage::ProjectLayerRecord* flag_layer = nullptr;
    for (const auto& layer : decorated.model->layers) {
        if (layer.role_id == aeris::storage::kLayerRoleCountryFlagV1) {
            flag_layer = &layer;
            break;
        }
    }
    if (flag_layer == nullptr || flag_layer->sources.size() != 1U ||
        flag_layer->resources.size() != flags.flag_count) {
        return fail(17, "Country flags layer bindings are incomplete after reopen");
    }

    for (const auto& resource_entry : decorated.model->resources) {
        const auto& resource = resource_entry.second;
        if (!resource || resource->media_type != "image/png" || resource->bytes.empty() ||
            resource->raster_image.isNull()) {
            return fail(18, "embedded flag resource failed durable PNG reconstruction");
        }
    }

    const auto integrity = created.store->verify_integrity();
    if (!integrity.ok()) {
        return fail(19, "decorated project integrity failed: " + integrity.diagnostic);
    }

    std::cout
        << "aeris_desktop_project_lifecycle_probe: PASS"
        << " empty_layers=0 empty_sources=0"
        << " world_layers=5 world_sources=2"
        << " political_palette_classes=" << palette_assignments.size()
        << " decorated_layers=" << decorated.model->layers.size()
        << " embedded_flags=" << flags.flag_count
        << " revision=" << created.store->metadata().revision
        << '\n';
    return EXIT_SUCCESS;
}
