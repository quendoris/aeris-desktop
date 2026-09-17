// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "flag_pack_import.hpp"
#include "project_model.hpp"
#include "world_data_import.hpp"

#include "aeris/project/world_layers.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/storage/resource.hpp"
#include "aeris/surface/classification.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view kTimestamp = "2026-09-05T11:00:00Z";
constexpr std::string_view kRepairTimestamp = "2026-09-05T11:01:00Z";
constexpr std::string_view kPoliticalSourceId = "world.admin0.natural-earth-110m";
constexpr std::string_view kSurfaceSourceId =
    "world.surface.antarctic-ice-shelves-natural-earth-50m";
constexpr std::string_view kCustomLandName = "User-renamed land layer";

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "aeris_desktop_project_lifecycle_probe: FAIL " << diagnostic << '\n';
    return code;
}

[[nodiscard]] bool floating_ice_shelf_feature(const aeris::source::Feature& feature) {
    bool found = false;
    for (const auto& property : feature.properties) {
        if (property.key != aeris::surface::kSurfaceClassPropertyKey) continue;
        if (found) return false;
        const auto* value = std::get_if<std::string>(&property.value);
        if (value == nullptr) return false;
        const auto parsed = aeris::surface::parse_surface_class_id(*value);
        if (!parsed.has_value() ||
            *parsed != aeris::surface::SurfaceClass::floating_ice_shelf) {
            return false;
        }
        found = true;
    }
    return found;
}

[[nodiscard]] const aeris::storage::ProjectLayerRecord* find_layer(
    const std::vector<aeris::storage::ProjectLayerRecord>& layers,
    const std::string_view layer_id
) noexcept {
    for (const auto& layer : layers) {
        if (layer.layer_id == layer_id) return &layer;
    }
    return nullptr;
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
    if (expanded.model->layers.size() != 6U || expanded.model->sources.size() != 3U ||
        !expanded.model->resources.empty()) {
        return fail(8, "world import did not expand project to 6 layers / 3 sources / 0 eager resources");
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

    const auto surface = expanded.model->sources.find(std::string(kSurfaceSourceId));
    if (surface == expanded.model->sources.end() || !surface->second ||
        !surface->second->feature_properties_complete || surface->second->features.empty()) {
        return fail(13, "durable surface-classification source is missing after reopen");
    }
    for (const auto& feature : surface->second->features) {
        if (!floating_ice_shelf_feature(feature)) {
            return fail(14, "durable Antarctic semantic feature lost floating-ice-shelf class");
        }
    }

    const aeris::storage::ProjectLayerRecord* surface_layer = nullptr;
    for (const auto& layer : expanded.model->layers) {
        if (layer.role_id == aeris::storage::kLayerRolePhysicalSurfaceClassificationV1) {
            surface_layer = &layer;
            break;
        }
    }
    if (surface_layer == nullptr || surface_layer->sources.size() != 2U) {
        return fail(15, "surface-classification layer is missing canonical source bindings");
    }

    // Simulate a pre-semantic application-owned starter that the user has
    // already customized. Repair may add the missing semantic layer, but must
    // never rebuild or normalize the existing five-layer stack.
    aeris::storage::LayerStateUpdate user_land_state{};
    user_land_state.modified_utc = std::string(kRepairTimestamp);
    user_land_state.name = std::string(kCustomLandName);
    user_land_state.visible = false;
    const auto user_state = aeris::storage::update_layer_state(
        *created.store,
        aeris::project::kBuiltinPhysicalLandLayerId,
        user_land_state
    );
    if (!user_state.ok() || !user_state.changed) {
        return fail(16, "could not install user land-layer state before starter repair");
    }

    const auto removed_surface = aeris::storage::remove_layer(
        *created.store,
        aeris::project::kBuiltinSurfaceClassificationLayerId,
        kRepairTimestamp
    );
    if (!removed_surface.ok() || !removed_surface.changed) {
        return fail(17, "could not simulate a starter project missing surface classification");
    }

    const std::vector<std::string> custom_base_order{
        std::string(aeris::project::kBuiltinPhysicalLandLayerId),
        std::string(aeris::project::kBuiltinPoliticalLabelsLayerId),
        std::string(aeris::project::kBuiltinPoliticalCountriesLayerId),
        std::string(aeris::project::kBuiltinPhysicalCoastlineLayerId),
        std::string(aeris::project::kBuiltinPoliticalBordersLayerId),
    };
    const auto reordered = aeris::storage::set_layer_order(
        *created.store,
        custom_base_order,
        kRepairTimestamp
    );
    if (!reordered.ok() || !reordered.changed) {
        return fail(18, "could not install custom base-layer order before starter repair");
    }

    const auto repaired = aeris::desktop::import_natural_earth_110m_world(
        *created.store,
        source_root,
        kRepairTimestamp
    );
    if (!repaired.ok() || !repaired.changed) {
        return fail(19, "starter semantic repair did not restore the missing durable layer");
    }

    auto repaired_model = aeris::desktop::load_project_model(*created.store);
    if (!repaired_model.ok() || !repaired_model.model ||
        repaired_model.model->layers.size() != 6U ||
        repaired_model.model->sources.size() != 3U) {
        return fail(20, "starter semantic repair did not reopen as 6 layers / 3 sources");
    }

    const auto* repaired_land = find_layer(
        repaired_model.model->layers,
        aeris::project::kBuiltinPhysicalLandLayerId
    );
    if (repaired_land == nullptr || repaired_land->name != kCustomLandName ||
        repaired_land->visible) {
        return fail(21, "starter semantic repair overwrote mutable user land-layer state");
    }
    for (std::size_t index = 0U; index < custom_base_order.size(); ++index) {
        if (repaired_model.model->layers[index].layer_id != custom_base_order[index]) {
            return fail(22, "starter semantic repair reordered the existing base stack");
        }
    }
    if (repaired_model.model->layers.back().layer_id !=
            aeris::project::kBuiltinSurfaceClassificationLayerId) {
        return fail(23, "starter semantic repair did not append the canonical surface layer");
    }

    const auto repair_retry = aeris::desktop::import_natural_earth_110m_world(
        *created.store,
        source_root,
        kRepairTimestamp
    );
    if (!repair_retry.ok() || repair_retry.changed) {
        return fail(24, "exact starter semantic repair retry is not idempotent");
    }

    const auto flags = aeris::desktop::import_country_flag_png_pack(
        *created.store,
        flags_root,
        kTimestamp
    );
    if (!flags.ok()) {
        return fail(25, "country flag pack import failed: " + flags.diagnostic);
    }
    if (flags.flag_count < 150U || flags.flag_count > 256U) {
        return fail(26, "country flag importer produced an implausible ISO flag count");
    }

    auto decorated = aeris::desktop::load_project_model(*created.store);
    if (!decorated.ok() || !decorated.model) {
        return fail(27, "flag-decorated project reload failed: " + decorated.diagnostic);
    }
    if (decorated.model->layers.size() != 7U || decorated.model->sources.size() != 3U ||
        !decorated.model->resources.empty()) {
        return fail(28, "flag import did not produce 7 layers / 3 sources / 0 eager PNG resources");
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
        return fail(29, "Country flags layer bindings are incomplete after reopen");
    }

    // Lazy frontend loading must not weaken durable truth. The .aeris file still
    // owns every exact PNG resource; load_project_model() simply stops streaming
    // those optional presentation bytes during cold open.
    const auto durable_resources = aeris::storage::list_project_resources(*created.store);
    if (!durable_resources.ok()) {
        return fail(30, "unable to enumerate durable flag resources: " + durable_resources.status.diagnostic);
    }

    std::size_t embedded_png_flags = 0U;
    for (const auto& record : durable_resources.records) {
        if (record.identity.media_type != "image/png") continue;
        if (record.storage_mode != aeris::storage::ResourceStorageMode::embedded ||
            record.identity.size_bytes == 0U || record.identity.sha256.size() != 64U ||
            record.chunk_count == 0U) {
            return fail(31, "durable flag resource lost embedded content identity");
        }
        ++embedded_png_flags;
    }
    if (embedded_png_flags != flags.flag_count ||
        durable_resources.records.size() != flags.flag_count) {
        return fail(32, "durable .aeris flag resource count differs from imported bindings");
    }

    const auto integrity = created.store->verify_integrity();
    if (!integrity.ok()) {
        return fail(33, "decorated project integrity failed: " + integrity.diagnostic);
    }

    std::cout
        << "aeris_desktop_project_lifecycle_probe: PASS"
        << " empty_layers=0 empty_sources=0"
        << " world_layers=6 world_sources=3"
        << " surface_features=" << surface->second->features.size()
        << " starter_repair_preserved_user_state=1"
        << " political_palette_classes=" << palette_assignments.size()
        << " decorated_layers=" << decorated.model->layers.size()
        << " embedded_flags=" << embedded_png_flags
        << " eager_flag_resources=" << decorated.model->resources.size()
        << " revision=" << created.store->metadata().revision
        << '\n';
    return EXIT_SUCCESS;
}
