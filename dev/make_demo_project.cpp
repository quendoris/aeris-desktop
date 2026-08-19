// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_bridge.hpp"
#include "aeris/project/world_layers.hpp"
#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/source/registry.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/util/sha256.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kTimestamp = "2026-08-19T20:00:00.000Z";
constexpr std::string_view kLandContentSha =
    "5a9d2b70be942d7d0602ef299afe0ef039463831ade478aae11091f8c202cf6e";
constexpr std::string_view kAdmin0ContentSha =
    "2d971b3c627462cb22fdcd1468a8972b2a66677585fabcaa520bc6937ef47fb0";
constexpr std::string_view kPhysicalSourceId = "world.land.natural-earth-110m";
constexpr std::string_view kPoliticalSourceId = "world.admin0.natural-earth-110m";

[[nodiscard]] aeris::source::SnapshotManifest land_manifest() {
    aeris::source::SnapshotManifest manifest{};
    manifest.provider = "Natural Earth";
    manifest.dataset = "ne_110m_land";
    manifest.snapshot = "v5.1.2";
    manifest.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/110m_physical";
    manifest.retrieved_at_utc = std::string(kTimestamp);
    manifest.resources.push_back({
        "geometry.shp",
        "ne_110m_land.shp",
        "8689e6932b8e370e2ca4587cf3ba21e460b1235db37b6ed3c172c35b4a6088de",
        89504U,
    });
    manifest.resources.push_back({
        "crs.prj",
        "ne_110m_land.prj",
        "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8",
        147U,
    });
    manifest.resources.push_back({
        "dataset.version",
        "ne_110m_land.VERSION.txt",
        "3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7",
        6U,
    });
    return manifest;
}

[[nodiscard]] bool add_local_resource(
    aeris::source::SnapshotManifest& manifest,
    const std::filesystem::path& root,
    std::string logical_name,
    std::filesystem::path relative_path,
    std::string& diagnostic
) {
    const std::filesystem::path path = root / relative_path;
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        diagnostic = "could not inspect " + relative_path.string() + ": " +
            size_error.message();
        return false;
    }
    const aeris::util::Sha256FileResult hash = aeris::util::sha256_file(path);
    if (!hash.ok()) {
        diagnostic = "could not hash " + relative_path.string();
        return false;
    }
    manifest.resources.push_back({
        std::move(logical_name),
        std::move(relative_path),
        hash.digest.hex(),
        size,
    });
    return true;
}

[[nodiscard]] std::optional<aeris::source::SnapshotManifest> admin0_manifest(
    const std::filesystem::path& root,
    std::string& diagnostic
) {
    aeris::source::SnapshotManifest manifest{};
    manifest.provider = "Natural Earth";
    manifest.dataset = "ne_110m_admin_0_countries";
    manifest.snapshot = "v5.1.2";
    manifest.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/110m_cultural";
    manifest.retrieved_at_utc = std::string(kTimestamp);

    if (!add_local_resource(manifest, root, "geometry.shp", "ne_110m_admin_0_countries.shp", diagnostic) ||
        !add_local_resource(manifest, root, "attributes.dbf", "ne_110m_admin_0_countries.dbf", diagnostic) ||
        !add_local_resource(manifest, root, "attributes.cpg", "ne_110m_admin_0_countries.cpg", diagnostic) ||
        !add_local_resource(manifest, root, "crs.prj", "ne_110m_admin_0_countries.prj", diagnostic) ||
        !add_local_resource(manifest, root, "dataset.version", "ne_110m_admin_0_countries.VERSION.txt", diagnostic)) {
        return std::nullopt;
    }
    return manifest;
}

[[nodiscard]] aeris::source::SourceBinding land_binding() {
    aeris::source::SourceBinding binding{};
    binding.adapter_id = "natural-earth.ne-110m-land.shapefile.v1";
    binding.capability = aeris::source::Capability::land;
    binding.snapshot = "v5.1.2";
    binding.expected_content_sha256 = std::string(kLandContentSha);
    return binding;
}

[[nodiscard]] aeris::source::SourceBinding admin0_binding() {
    aeris::source::SourceBinding binding{};
    binding.adapter_id = "natural-earth.ne-110m-admin0-countries.shapefile-dbf.v1";
    binding.capability = aeris::source::Capability::admin0;
    binding.snapshot = "v5.1.2";
    binding.worldview = "natural-earth.de-facto";
    binding.expected_content_sha256 = std::string(kAdmin0ContentSha);
    return binding;
}

[[nodiscard]] bool verify_expected_identity(
    const aeris::source::VerifiedSnapshot& snapshot,
    const std::string_view expected,
    const char* label
) {
    if (snapshot.content_sha256() == expected) return true;
    std::cerr << label << " aggregate content identity mismatch\n";
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: aeris-demo-project <natural-earth-directory> <output.aeris>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path root(argv[1]);
    const std::filesystem::path output(argv[2]);
    if (std::filesystem::exists(output)) {
        std::cerr << "output already exists: " << output << '\n';
        return EXIT_FAILURE;
    }

    auto land_verified = aeris::source::verify_local_snapshot(root, land_manifest());
    if (!land_verified.ok() ||
        !verify_expected_identity(*land_verified.snapshot, kLandContentSha, "land")) {
        std::cerr << "land snapshot verification failed: " << land_verified.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    std::string manifest_diagnostic;
    auto admin_manifest = admin0_manifest(root, manifest_diagnostic);
    if (!admin_manifest) {
        std::cerr << "admin0 manifest failed: " << manifest_diagnostic << '\n';
        return EXIT_FAILURE;
    }
    auto admin_verified = aeris::source::verify_local_snapshot(root, *admin_manifest);
    if (!admin_verified.ok() ||
        !verify_expected_identity(*admin_verified.snapshot, kAdmin0ContentSha, "admin0")) {
        std::cerr << "admin0 snapshot verification failed: " << admin_verified.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    aeris::source::AdapterRegistry registry{};
    if (registry.add(std::make_unique<aeris::source::NaturalEarthLand110mAdapter>()) !=
            aeris::source::RegistryError::none ||
        registry.add(std::make_unique<aeris::source::NaturalEarthAdmin0Countries110mAdapter>()) !=
            aeris::source::RegistryError::none) {
        std::cerr << "unable to register Natural Earth adapters\n";
        return EXIT_FAILURE;
    }

    aeris::storage::ProjectCreateOptions create{};
    create.timestamp_utc = std::string(kTimestamp);
    create.producer = "aeris-demo-project";
    create.producer_version = "0.1.0";
    auto created = aeris::storage::ProjectStore::create(output, create);
    if (!created.ok()) {
        std::cerr << "project create failed: " << created.status.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    aeris::project::VerifiedSourceRecordRequest land_request{};
    land_request.source_id = std::string(kPhysicalSourceId);
    land_request.binding = land_binding();
    land_request.modified_utc = std::string(kTimestamp);
    const auto land_result = aeris::project::record_verified_source_snapshot(
        *created.store,
        registry,
        *land_verified.snapshot,
        land_request
    );
    if (!land_result.ok()) {
        std::cerr << "land project ingest failed: " << land_result.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    aeris::project::VerifiedSourceRecordRequest admin_request{};
    admin_request.source_id = std::string(kPoliticalSourceId);
    admin_request.binding = admin0_binding();
    admin_request.modified_utc = std::string(kTimestamp);
    const auto admin_result = aeris::project::record_verified_source_snapshot(
        *created.store,
        registry,
        *admin_verified.snapshot,
        admin_request
    );
    if (!admin_result.ok()) {
        std::cerr << "admin0 project ingest failed: " << admin_result.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    aeris::project::BuiltinWorldLayerSources sources{};
    sources.physical_source_id = std::string(kPhysicalSourceId);
    sources.political_source_id = std::string(kPoliticalSourceId);
    const auto layers = aeris::project::initialize_builtin_world_layer_stack(
        *created.store,
        sources,
        kTimestamp
    );
    if (!layers.ok()) {
        std::cerr << "layer stack initialization failed: " << layers.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    const auto integrity = created.store->verify_integrity();
    if (!integrity.ok()) {
        std::cerr << "project integrity failed: " << integrity.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    std::cout
        << "aeris_demo_project: PASS\n"
        << "path: " << output << '\n'
        << "revision: " << created.store->metadata().revision << '\n'
        << "sources: 2\n"
        << "layers: 5\n";
    return EXIT_SUCCESS;
}
