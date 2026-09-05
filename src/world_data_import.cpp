// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "world_data_import.hpp"

#include "aeris/project/source_bridge.hpp"
#include "aeris/project/world_layers.hpp"
#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/source/registry.hpp"
#include "aeris/util/sha256.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aeris::desktop {
namespace {

constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kLandContentSha =
    "5a9d2b70be942d7d0602ef299afe0ef039463831ade478aae11091f8c202cf6e";
constexpr std::string_view kAdmin0ContentSha =
    "2d971b3c627462cb22fdcd1468a8972b2a66677585fabcaa520bc6937ef47fb0";
constexpr std::string_view kPhysicalSourceId = "world.land.natural-earth-110m";
constexpr std::string_view kPoliticalSourceId = "world.admin0.natural-earth-110m";

[[nodiscard]] WorldDataImportResult failure(std::string diagnostic) {
    return {false, false, std::move(diagnostic)};
}

[[nodiscard]] source::SnapshotManifest land_manifest(const std::string_view retrieved_at) {
    source::SnapshotManifest manifest{};
    manifest.provider = "Natural Earth";
    manifest.dataset = "ne_110m_land";
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/110m_physical";
    manifest.retrieved_at_utc = std::string(retrieved_at);
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
    source::SnapshotManifest& manifest,
    const std::filesystem::path& root,
    std::string logical_name,
    std::filesystem::path relative_path,
    std::string& diagnostic
) {
    const std::filesystem::path full = root / relative_path;
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(full, size_error);
    if (size_error) {
        diagnostic = "could not inspect " + relative_path.string() + ": " +
            size_error.message();
        return false;
    }

    const util::Sha256FileResult hash = util::sha256_file(full);
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

[[nodiscard]] std::optional<source::SnapshotManifest> admin0_manifest(
    const std::filesystem::path& root,
    const std::string_view retrieved_at,
    std::string& diagnostic
) {
    source::SnapshotManifest manifest{};
    manifest.provider = "Natural Earth";
    manifest.dataset = "ne_110m_admin_0_countries";
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/110m_cultural";
    manifest.retrieved_at_utc = std::string(retrieved_at);

    if (!add_local_resource(
            manifest,
            root,
            "geometry.shp",
            "ne_110m_admin_0_countries.shp",
            diagnostic
        ) ||
        !add_local_resource(
            manifest,
            root,
            "attributes.dbf",
            "ne_110m_admin_0_countries.dbf",
            diagnostic
        ) ||
        !add_local_resource(
            manifest,
            root,
            "attributes.cpg",
            "ne_110m_admin_0_countries.cpg",
            diagnostic
        ) ||
        !add_local_resource(
            manifest,
            root,
            "crs.prj",
            "ne_110m_admin_0_countries.prj",
            diagnostic
        ) ||
        !add_local_resource(
            manifest,
            root,
            "dataset.version",
            "ne_110m_admin_0_countries.VERSION.txt",
            diagnostic
        )) {
        return std::nullopt;
    }
    return manifest;
}

[[nodiscard]] source::SourceBinding land_binding() {
    source::SourceBinding binding{};
    binding.adapter_id = "natural-earth.ne-110m-land.shapefile.v1";
    binding.capability = source::Capability::land;
    binding.snapshot = std::string(kSnapshot);
    binding.expected_content_sha256 = std::string(kLandContentSha);
    return binding;
}

[[nodiscard]] source::SourceBinding admin0_binding() {
    source::SourceBinding binding{};
    binding.adapter_id = "natural-earth.ne-110m-admin0-countries.shapefile-dbf.v1";
    binding.capability = source::Capability::admin0;
    binding.snapshot = std::string(kSnapshot);
    binding.worldview = "natural-earth.de-facto";
    binding.expected_content_sha256 = std::string(kAdmin0ContentSha);
    return binding;
}

}  // namespace

WorldDataImportResult import_natural_earth_110m_world(
    storage::ProjectStore& project,
    const std::filesystem::path& source_root,
    const std::string_view modified_utc
) {
    if (source_root.empty() || modified_utc.empty()) {
        return failure("world import requires a source directory and canonical timestamp");
    }

    source::SnapshotVerificationResult land_verified =
        source::verify_local_snapshot(source_root, land_manifest(modified_utc));
    if (!land_verified.ok() || !land_verified.snapshot.has_value()) {
        return failure(
            "Natural Earth land pack verification failed: " + land_verified.diagnostic
        );
    }
    if (land_verified.snapshot->content_sha256() != kLandContentSha) {
        return failure("Natural Earth land pack aggregate content identity mismatch");
    }

    std::string manifest_diagnostic;
    const auto admin_manifest = admin0_manifest(
        source_root,
        modified_utc,
        manifest_diagnostic
    );
    if (!admin_manifest.has_value()) {
        return failure("Natural Earth admin0 manifest failed: " + manifest_diagnostic);
    }
    source::SnapshotVerificationResult admin_verified =
        source::verify_local_snapshot(source_root, *admin_manifest);
    if (!admin_verified.ok() || !admin_verified.snapshot.has_value()) {
        return failure(
            "Natural Earth admin0 pack verification failed: " + admin_verified.diagnostic
        );
    }
    if (admin_verified.snapshot->content_sha256() != kAdmin0ContentSha) {
        return failure("Natural Earth admin0 pack aggregate content identity mismatch");
    }

    source::AdapterRegistry registry{};
    if (registry.add(std::make_unique<source::NaturalEarthLand110mAdapter>()) !=
            source::RegistryError::none ||
        registry.add(std::make_unique<source::NaturalEarthAdmin0Countries110mAdapter>()) !=
            source::RegistryError::none) {
        return failure("could not register built-in Natural Earth adapters");
    }

    bool changed = false;

    project::VerifiedSourceRecordRequest land_request{};
    land_request.source_id = std::string(kPhysicalSourceId);
    land_request.binding = land_binding();
    land_request.modified_utc = std::string(modified_utc);
    const project::SourceBridgeResult land = project::record_verified_source_snapshot(
        project,
        registry,
        *land_verified.snapshot,
        land_request
    );
    if (!land.ok()) {
        return {
            false,
            land.inserted || land.durably_committed,
            "Natural Earth land import failed: " + land.diagnostic,
        };
    }
    changed = changed || land.inserted;

    project::VerifiedSourceRecordRequest admin_request{};
    admin_request.source_id = std::string(kPoliticalSourceId);
    admin_request.binding = admin0_binding();
    admin_request.modified_utc = std::string(modified_utc);
    const project::SourceBridgeResult admin = project::record_verified_source_snapshot(
        project,
        registry,
        *admin_verified.snapshot,
        admin_request
    );
    if (!admin.ok()) {
        return {
            false,
            changed || admin.inserted || admin.durably_committed,
            "Natural Earth admin0 import failed: " + admin.diagnostic,
        };
    }
    changed = changed || admin.inserted;

    project::BuiltinWorldLayerSources sources{};
    sources.physical_source_id = std::string(kPhysicalSourceId);
    sources.political_source_id = std::string(kPoliticalSourceId);
    const project::WorldLayerStackResult layers =
        project::initialize_builtin_world_layer_stack(project, sources, modified_utc);
    if (!layers.ok()) {
        return {
            false,
            changed || layers.changed || layers.durably_committed,
            "built-in world layer initialization failed: " + layers.diagnostic,
        };
    }
    changed = changed || layers.changed;

    const storage::Status integrity = project.verify_integrity();
    if (!integrity.ok()) {
        return {
            false,
            changed,
            "project integrity failed after world import: " + integrity.diagnostic,
        };
    }

    return {
        true,
        changed,
        changed
            ? "verified Natural Earth world imported into durable .aeris storage"
            : "verified Natural Earth world already present",
    };
}

}  // namespace aeris::desktop
