// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "project_model.hpp"
#include "world_data_import.hpp"

#include "aeris/storage/project.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kTimestamp = "2026-09-05T11:00:00Z";

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "aeris_desktop_project_lifecycle_probe: FAIL " << diagnostic << '\n';
    return code;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 3) {
        return fail(2, "usage: <natural-earth-directory> <output.aeris>");
    }

    const std::filesystem::path source_root = argv[1];
    const std::filesystem::path output = argv[2];
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
        !empty.model->sources.empty()) {
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
    if (expanded.model->layers.size() != 5U || expanded.model->sources.size() != 2U) {
        return fail(8, "world import did not expand project to 5 layers / 2 sources");
    }

    const auto integrity = created.store->verify_integrity();
    if (!integrity.ok()) {
        return fail(9, "expanded project integrity failed: " + integrity.diagnostic);
    }

    std::cout
        << "aeris_desktop_project_lifecycle_probe: PASS"
        << " empty_layers=0 empty_sources=0"
        << " expanded_layers=" << expanded.model->layers.size()
        << " expanded_sources=" << expanded.model->sources.size()
        << " revision=" << created.store->metadata().revision
        << '\n';
    return EXIT_SUCCESS;
}
