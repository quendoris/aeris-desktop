// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "flag_pack_import.hpp"

#include "aeris/project/source_reader.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/storage/resource.hpp"
#include "aeris/util/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aeris::desktop {
namespace {

constexpr std::string_view kFlagLayerId = "builtin.political.flags";
constexpr std::string_view kFlagResourcePrefix = "builtin.flag.iso3166.";

struct FlagInput final {
    std::string code;
    std::filesystem::path path;
    storage::ProjectResourceIdentity identity;
};

[[nodiscard]] FlagPackImportResult failure(
    const bool changed,
    const std::size_t flag_count,
    std::string diagnostic
) {
    return {false, changed, flag_count, std::move(diagnostic)};
}

[[nodiscard]] std::string ascii_lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

[[nodiscard]] std::string ascii_upper(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return value;
}

[[nodiscard]] bool iso_alpha2(const std::string& value) noexcept {
    if (value.size() != 2U) return false;
    for (const char c : value) {
        const bool upper = c >= 'A' && c <= 'Z';
        const bool lower = c >= 'a' && c <= 'z';
        if (!upper && !lower) return false;
    }
    return true;
}

[[nodiscard]] const source::FeatureProperty* property(
    const source::Feature& feature,
    const std::string_view key
) noexcept {
    for (const source::FeatureProperty& candidate : feature.properties) {
        if (candidate.key == key) return &candidate;
    }
    return nullptr;
}

[[nodiscard]] std::filesystem::path resolve_png_root(
    const std::filesystem::path& pack_root
) {
    std::error_code error;
    const std::filesystem::path nested = pack_root / "png" / "256";
    if (std::filesystem::is_directory(nested, error) && !error) return nested;
    return pack_root;
}

[[nodiscard]] const storage::ProjectLayerRecord* find_political_layer(
    const std::vector<storage::ProjectLayerRecord>& layers
) noexcept {
    for (const storage::ProjectLayerRecord& layer : layers) {
        if (layer.role_id == storage::kLayerRolePoliticalCountryFillV1) return &layer;
    }
    return nullptr;
}

[[nodiscard]] std::string properties_source_id(
    const storage::ProjectLayerRecord& layer
) {
    for (const storage::LayerSourceBinding& binding : layer.sources) {
        if (binding.slot_id == "properties") return binding.source_id;
    }
    return {};
}

[[nodiscard]] std::vector<std::string> desired_layer_order(
    const std::vector<storage::ProjectLayerRecord>& layers
) {
    std::vector<std::string> order;
    order.reserve(layers.size());
    for (const storage::ProjectLayerRecord& layer : layers) {
        if (layer.layer_id != kFlagLayerId) order.push_back(layer.layer_id);
    }

    std::size_t insert_at = 0U;
    for (const storage::ProjectLayerRecord& layer : layers) {
        if (layer.role_id != storage::kLayerRoleCountryLabelV1) continue;
        const auto found = std::find(order.begin(), order.end(), layer.layer_id);
        if (found != order.end()) {
            insert_at = static_cast<std::size_t>(std::distance(order.begin(), found)) + 1U;
        }
        break;
    }
    order.insert(
        order.begin() + static_cast<std::ptrdiff_t>(insert_at),
        std::string(kFlagLayerId)
    );
    return order;
}

}  // namespace

FlagPackImportResult import_country_flag_png_pack(
    storage::ProjectStore& project,
    const std::filesystem::path& pack_root,
    const std::string_view modified_utc
) {
    if (pack_root.empty() || modified_utc.empty()) {
        return failure(false, 0U, "flag import requires a source directory and canonical timestamp");
    }
    if (project.metadata().frozen) {
        return failure(false, 0U, "flag import refuses a frozen project");
    }

    const storage::ProjectLayerListResult listed = storage::list_project_layers(project);
    if (!listed.ok()) {
        return failure(false, 0U, "could not inspect project layers: " + listed.status.diagnostic);
    }
    const storage::ProjectLayerRecord* political_layer = find_political_layer(listed.records);
    if (political_layer == nullptr) {
        return failure(false, 0U, "country flags require the built-in political country layer");
    }
    const std::string source_id = properties_source_id(*political_layer);
    if (source_id.empty()) {
        return failure(false, 0U, "political country layer has no properties source binding");
    }

    project::DurableSourceLoadResult political =
        project::load_durable_source_result(project, source_id);
    if (!political.ok() || !political.source.feature_properties_complete) {
        return failure(
            false,
            0U,
            "could not load complete durable political properties: " + political.diagnostic
        );
    }

    std::set<std::string> codes;
    for (const source::Feature& feature : political.source.features) {
        const source::FeatureProperty* iso = property(feature, "iso_a2");
        if (iso == nullptr) continue;
        const auto* text = std::get_if<std::string>(&iso->value);
        if (text == nullptr || !iso_alpha2(*text)) continue;
        codes.insert(ascii_lower(*text));
    }
    if (codes.empty()) {
        return failure(false, 0U, "political source exposes no usable ISO alpha-2 country codes");
    }
    if (codes.size() > storage::kMaxLayerBindings) {
        return failure(false, 0U, "flag pack exceeds the current 256-resource layer bound");
    }

    const std::filesystem::path png_root = resolve_png_root(pack_root);
    std::vector<FlagInput> inputs;
    inputs.reserve(codes.size());
    for (const std::string& code : codes) {
        const std::string filename = ascii_upper(code) + ".png";
        std::filesystem::path path = png_root / filename;
        std::error_code error;
        std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size == 0U) {
            error.clear();
            path = png_root / (code + ".png");
            size = std::filesystem::file_size(path, error);
        }
        if (error || size == 0U) {
            return failure(
                false,
                0U,
                "flag pack is incomplete: missing or empty " + filename
            );
        }
        const util::Sha256FileResult hash = util::sha256_file(path);
        if (!hash.ok()) {
            return failure(false, 0U, "could not hash flag " + path.filename().string());
        }

        storage::ProjectResourceIdentity identity{};
        identity.resource_id = std::string(kFlagResourcePrefix) + code;
        identity.sha256 = hash.digest.hex();
        identity.media_type = "image/png";
        identity.size_bytes = static_cast<std::uint64_t>(size);
        identity.required_for_reproduction = true;
        inputs.push_back({code, path, std::move(identity)});
    }

    bool changed = false;
    std::size_t embedded_count = 0U;
    for (const FlagInput& input : inputs) {
        const storage::ResourceMutationResult embedded = storage::embed_resource_file(
            project,
            input.identity,
            input.path,
            modified_utc
        );
        if (!embedded.ok()) {
            return failure(
                changed || embedded.inserted || embedded.representation_changed ||
                    embedded.durably_committed,
                embedded_count,
                "flag resource import failed for " + input.code + ": " +
                    embedded.status.diagnostic
            );
        }
        changed = changed || embedded.inserted || embedded.representation_changed;
        ++embedded_count;
    }

    storage::LayerCreateRequest flag_layer{};
    flag_layer.layer_id = std::string(kFlagLayerId);
    flag_layer.role_id = std::string(storage::kLayerRoleCountryFlagV1);
    flag_layer.name = "Country flags";
    flag_layer.visible = true;
    flag_layer.sources.push_back({"properties", source_id});
    flag_layer.resources.reserve(inputs.size());
    for (const FlagInput& input : inputs) {
        flag_layer.resources.push_back({"flag:" + input.code, input.identity.resource_id});
    }

    const storage::LayerMutationResult appended = storage::append_layer(
        project,
        flag_layer,
        modified_utc
    );
    if (!appended.ok()) {
        return failure(
            changed || appended.changed || appended.durably_committed,
            embedded_count,
            "Country flags layer creation failed: " + appended.status.diagnostic
        );
    }
    changed = changed || appended.changed;

    const storage::ProjectLayerListResult after_append = storage::list_project_layers(project);
    if (!after_append.ok()) {
        return failure(
            changed,
            embedded_count,
            "could not inspect layer order after flag import: " + after_append.status.diagnostic
        );
    }
    const std::vector<std::string> order = desired_layer_order(after_append.records);
    const storage::LayerMutationResult reordered = storage::set_layer_order(
        project,
        order,
        modified_utc
    );
    if (!reordered.ok()) {
        return failure(
            changed || reordered.changed || reordered.durably_committed,
            embedded_count,
            "Country flags layer ordering failed: " + reordered.status.diagnostic
        );
    }
    changed = changed || reordered.changed;

    const storage::Status integrity = project.verify_integrity();
    if (!integrity.ok()) {
        return failure(
            changed,
            embedded_count,
            "project integrity failed after flag import: " + integrity.diagnostic
        );
    }

    return {
        true,
        changed,
        embedded_count,
        changed
            ? "country flag PNG pack embedded into durable .aeris storage"
            : "country flag pack already present",
    };
}

}  // namespace aeris::desktop
