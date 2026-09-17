// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "world_data_import.hpp"

#include "aeris/project/source_bridge.hpp"
#include "aeris/project/world_layers.hpp"
#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/source/natural_earth_cartography.hpp"
#include "aeris/source/registry.hpp"
#include "aeris/storage/provenance.hpp"
#include "aeris/util/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aeris::desktop {
namespace {

constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kLandContentSha =
    "5a9d2b70be942d7d0602ef299afe0ef039463831ade478aae11091f8c202cf6e";
constexpr std::string_view kAdmin0ContentSha =
    "2d971b3c627462cb22fdcd1468a8972b2a66677585fabcaa520bc6937ef47fb0";
constexpr std::string_view kSurfaceContentSha =
    "8b37e1c1612be041756a7062a6e5d12d7c130892bd26dcfc10c5be7884c7d281";
constexpr std::string_view kPhysicalSourceId = "world.land.natural-earth-110m";
constexpr std::string_view kPoliticalSourceId = "world.admin0.natural-earth-110m";
constexpr std::string_view kSurfaceSourceId =
    "world.surface.antarctic-ice-shelves-natural-earth-50m";

struct SourceEnsureResult final {
    bool success{false};
    bool inserted{false};
    bool durably_committed{false};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

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

[[nodiscard]] source::SnapshotManifest surface_manifest(
    const std::string_view retrieved_at
) {
    source::SnapshotManifest manifest{};
    manifest.provider = "Natural Earth";
    manifest.dataset = "ne_50m_antarctic_ice_shelves_polys";
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/50m_physical";
    manifest.retrieved_at_utc = std::string(retrieved_at);
    manifest.resources.push_back({
        "geometry.shp",
        "ne_50m_antarctic_ice_shelves_polys.shp",
        "05d06b075deb3e4119f0510788b03af7100f2c25b060d5cfdd126fb6817004db",
        83960U,
    });
    manifest.resources.push_back({
        "crs.prj",
        "ne_50m_antarctic_ice_shelves_polys.prj",
        "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8",
        147U,
    });
    manifest.resources.push_back({
        "dataset.version",
        "ne_50m_antarctic_ice_shelves_polys.VERSION.txt",
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
    binding.adapter_id = "natural-earth.ne-110m-admin0-cartography.shapefile-dbf.v1";
    binding.capability = source::Capability::admin0;
    binding.snapshot = std::string(kSnapshot);
    binding.worldview = "natural-earth.de-facto";
    binding.expected_content_sha256 = std::string(kAdmin0ContentSha);
    return binding;
}

[[nodiscard]] source::SourceBinding surface_binding() {
    source::SourceBinding binding{};
    binding.adapter_id =
        "natural-earth.ne-50m-antarctic-ice-shelves.surface-classification.v1";
    binding.capability = source::Capability::surface_classification;
    binding.snapshot = std::string(kSnapshot);
    binding.expected_content_sha256 = std::string(kSurfaceContentSha);
    return binding;
}

[[nodiscard]] bool resource_identity_matches(
    const storage::SourceResourceRecord& stored,
    const source::ResourceSpec& expected
) noexcept {
    if (stored.logical_name != expected.logical_name || stored.sha256 != expected.sha256 ||
        stored.size_bytes.has_value() != expected.size_bytes.has_value()) {
        return false;
    }
    if (!stored.size_bytes.has_value()) return true;
    return static_cast<std::uintmax_t>(*stored.size_bytes) == *expected.size_bytes;
}

[[nodiscard]] bool source_resources_match(
    const storage::SourceSnapshotRecord& stored,
    const source::SnapshotManifest& manifest
) noexcept {
    if (stored.resources.size() != manifest.resources.size()) return false;
    for (const source::ResourceSpec& expected : manifest.resources) {
        const auto found = std::find_if(
            stored.resources.begin(),
            stored.resources.end(),
            [&](const storage::SourceResourceRecord& candidate) {
                return candidate.logical_name == expected.logical_name;
            }
        );
        if (found == stored.resources.end() ||
            !resource_identity_matches(*found, expected)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] SourceEnsureResult ensure_verified_source(
    storage::ProjectStore& project,
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const project::VerifiedSourceRecordRequest& request
) {
    const storage::SourceSnapshotListResult listed = storage::list_source_snapshots(project);
    if (!listed.ok()) {
        return {
            false,
            false,
            false,
            "could not inspect durable source provenance: " + listed.status.diagnostic,
        };
    }

    const auto existing = std::find_if(
        listed.records.begin(),
        listed.records.end(),
        [&](const storage::SourceSnapshotRecord& record) {
            return record.source_id == request.source_id;
        }
    );
    if (existing == listed.records.end()) {
        const project::SourceBridgeResult recorded =
            project::record_verified_source_snapshot(project, registry, snapshot, request);
        return {
            recorded.ok(),
            recorded.inserted,
            recorded.durably_committed,
            recorded.diagnostic,
        };
    }

    const source::RegistryLoadResult expected = registry.load(request.binding, snapshot);
    if (!expected.ok()) {
        return {
            false,
            false,
            false,
            expected.diagnostic.empty()
                ? "source registry rejected verified snapshot during durable-source reuse"
                : expected.diagnostic,
        };
    }
    const source::Adapter* adapter = registry.find(request.binding.adapter_id);
    if (adapter == nullptr) {
        return {false, false, false, "source adapter disappeared during durable-source reuse"};
    }

    const source::Provenance& provenance = expected.source.provenance;
    const source::SnapshotManifest& manifest = snapshot.manifest();
    const source::AdapterDescriptor descriptor = adapter->descriptor();
    const bool immutable_identity_matches =
        existing->adapter_id == request.binding.adapter_id &&
        existing->capability_bits == source::capability_bit(request.binding.capability) &&
        existing->temporal_class == static_cast<std::uint8_t>(descriptor.temporal_class) &&
        existing->provider == provenance.provider &&
        existing->dataset == provenance.dataset &&
        existing->snapshot == provenance.snapshot &&
        existing->dataset_version == provenance.dataset_version &&
        existing->source_uri == provenance.source_uri &&
        existing->license_id == provenance.license_id &&
        existing->content_sha256 == provenance.content_sha256 &&
        existing->worldview == provenance.worldview &&
        source_resources_match(*existing, manifest);

    if (!immutable_identity_matches) {
        return {
            false,
            false,
            false,
            "durable source '" + request.source_id +
                "' conflicts with the exact verified built-in source identity",
        };
    }

    // retrieved_at_utc intentionally remains the timestamp of the acquisition
    // that originally created this durable source. A later repair validates the
    // same immutable bytes and adapter identity but must not rewrite provenance
    // merely because the application happened to run again at a new time.
    return {true, false, false, {}};
}

[[nodiscard]] const storage::ProjectLayerRecord* find_layer(
    const std::vector<storage::ProjectLayerRecord>& layers,
    const std::string_view layer_id
) noexcept {
    const auto found = std::find_if(
        layers.begin(),
        layers.end(),
        [&](const storage::ProjectLayerRecord& layer) {
            return layer.layer_id == layer_id;
        }
    );
    return found == layers.end() ? nullptr : &*found;
}

[[nodiscard]] bool exact_source_slots(
    const storage::ProjectLayerRecord& layer,
    const std::initializer_list<std::pair<std::string_view, std::string_view>> expected
) noexcept {
    if (!layer.resources.empty() || layer.sources.size() != expected.size()) return false;
    for (const auto& [slot, source_id] : expected) {
        const auto found = std::find_if(
            layer.sources.begin(),
            layer.sources.end(),
            [&](const storage::LayerSourceBinding& binding) {
                return binding.slot_id == slot && binding.source_id == source_id;
            }
        );
        if (found == layer.sources.end()) return false;
    }
    return true;
}

[[nodiscard]] std::optional<bool> builtin_world_stack_present(
    storage::ProjectStore& project,
    std::string& diagnostic
) {
    const storage::ProjectLayerListResult listed = storage::list_project_layers(project);
    if (!listed.ok()) {
        diagnostic = "could not inspect existing world layer stack: " + listed.status.diagnostic;
        return std::nullopt;
    }

    const auto valid = [&] (
        const std::string_view id,
        const std::string_view role,
        const std::initializer_list<std::pair<std::string_view, std::string_view>> slots
    ) {
        const storage::ProjectLayerRecord* layer = find_layer(listed.records, id);
        return layer != nullptr && layer->role_id == role && exact_source_slots(*layer, slots);
    };

    return
        valid(
            project::kBuiltinPoliticalLabelsLayerId,
            storage::kLayerRoleCountryLabelV1,
            {{"properties", kPoliticalSourceId}}
        ) &&
        valid(
            project::kBuiltinPoliticalBordersLayerId,
            storage::kLayerRolePoliticalBoundaryV1,
            {{"geometry", kPoliticalSourceId}}
        ) &&
        valid(
            project::kBuiltinPoliticalCountriesLayerId,
            storage::kLayerRolePoliticalCountryFillV1,
            {{"geometry", kPoliticalSourceId}, {"properties", kPoliticalSourceId}}
        ) &&
        valid(
            project::kBuiltinPhysicalCoastlineLayerId,
            storage::kLayerRolePhysicalCoastlineV1,
            {{"geometry", kPhysicalSourceId}}
        ) &&
        valid(
            project::kBuiltinPhysicalLandLayerId,
            storage::kLayerRolePhysicalLandFillV1,
            {{"geometry", kPhysicalSourceId}}
        );
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

    source::SnapshotVerificationResult surface_verified =
        source::verify_local_snapshot(source_root, surface_manifest(modified_utc));
    if (!surface_verified.ok() || !surface_verified.snapshot.has_value()) {
        return failure(
            "Natural Earth Antarctic surface-classification verification failed: " +
            surface_verified.diagnostic
        );
    }
    if (surface_verified.snapshot->content_sha256() != kSurfaceContentSha) {
        return failure("Natural Earth surface-classification aggregate content identity mismatch");
    }

    source::AdapterRegistry registry{};
    if (registry.add(std::make_unique<source::NaturalEarthLand110mAdapter>()) !=
            source::RegistryError::none ||
        registry.add(std::make_unique<source::NaturalEarthAdmin0Cartography110mAdapter>()) !=
            source::RegistryError::none ||
        registry.add(std::make_unique<source::NaturalEarthAntarcticIceShelves50mAdapter>()) !=
            source::RegistryError::none) {
        return failure("could not register built-in Natural Earth adapters");
    }

    bool changed = false;

    project::VerifiedSourceRecordRequest land_request{};
    land_request.source_id = std::string(kPhysicalSourceId);
    land_request.binding = land_binding();
    land_request.modified_utc = std::string(modified_utc);
    const SourceEnsureResult land = ensure_verified_source(
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
    const SourceEnsureResult admin = ensure_verified_source(
        project,
        registry,
        *admin_verified.snapshot,
        admin_request
    );
    if (!admin.ok()) {
        return {
            false,
            changed || admin.inserted || admin.durably_committed,
            "Natural Earth admin0 cartography import failed: " + admin.diagnostic,
        };
    }
    changed = changed || admin.inserted;

    project::VerifiedSourceRecordRequest surface_request{};
    surface_request.source_id = std::string(kSurfaceSourceId);
    surface_request.binding = surface_binding();
    surface_request.modified_utc = std::string(modified_utc);
    const SourceEnsureResult surface = ensure_verified_source(
        project,
        registry,
        *surface_verified.snapshot,
        surface_request
    );
    if (!surface.ok()) {
        return {
            false,
            changed || surface.inserted || surface.durably_committed,
            "Natural Earth Antarctic surface-classification import failed: " +
                surface.diagnostic,
        };
    }
    changed = changed || surface.inserted;

    std::string stack_diagnostic;
    const std::optional<bool> existing_world =
        builtin_world_stack_present(project, stack_diagnostic);
    if (!existing_world.has_value()) {
        return {false, changed, std::move(stack_diagnostic)};
    }
    if (!*existing_world) {
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
    }

    const project::WorldLayerStackResult surface_layer =
        project::ensure_builtin_surface_classification_layer(
            project,
            kSurfaceSourceId,
            modified_utc
        );
    if (!surface_layer.ok()) {
        return {
            false,
            changed || surface_layer.changed || surface_layer.durably_committed,
            "surface-classification layer initialization failed: " +
                surface_layer.diagnostic,
        };
    }
    changed = changed || surface_layer.changed;

    // A newly repaired semantic layer is appended by the storage primitive. If
    // an older project already contains numerical elevation, that would put the
    // semantic material below the elevation raster and make it visually inert.
    // Move only the newly-created semantic layer immediately above the first
    // elevation layer. Every pre-existing layer keeps its relative order; an
    // already-existing semantic layer is never normalized or moved here.
    if (surface_layer.changed) {
        const storage::ProjectLayerListResult listed = storage::list_project_layers(project);
        if (!listed.ok()) {
            return {
                false,
                changed,
                "could not inspect layer order after semantic repair: " +
                    listed.status.diagnostic,
            };
        }

        const auto semantic = std::find_if(
            listed.records.begin(),
            listed.records.end(),
            [](const storage::ProjectLayerRecord& layer) {
                return layer.layer_id == project::kBuiltinSurfaceClassificationLayerId;
            }
        );
        const auto elevation = std::find_if(
            listed.records.begin(),
            listed.records.end(),
            [](const storage::ProjectLayerRecord& layer) {
                return layer.role_id == storage::kLayerRolePhysicalElevationV1;
            }
        );
        if (semantic == listed.records.end()) {
            return {false, changed, "new semantic layer disappeared before order repair"};
        }

        if (elevation != listed.records.end() && semantic->ordinal > elevation->ordinal) {
            std::vector<std::string> ordered;
            ordered.reserve(listed.records.size());
            bool inserted_semantic = false;
            for (const storage::ProjectLayerRecord& layer : listed.records) {
                if (layer.layer_id == project::kBuiltinSurfaceClassificationLayerId) {
                    continue;
                }
                if (!inserted_semantic &&
                    layer.role_id == storage::kLayerRolePhysicalElevationV1) {
                    ordered.emplace_back(project::kBuiltinSurfaceClassificationLayerId);
                    inserted_semantic = true;
                }
                ordered.push_back(layer.layer_id);
            }
            if (!inserted_semantic) {
                return {false, changed, "elevation layer disappeared during semantic order repair"};
            }

            const storage::LayerMutationResult reordered =
                storage::set_layer_order(project, ordered, modified_utc);
            if (!reordered.ok()) {
                return {
                    false,
                    changed || reordered.changed || reordered.durably_committed,
                    "could not place repaired semantic layer above elevation: " +
                        reordered.status.diagnostic,
                };
            }
            changed = changed || reordered.changed;
        }
    }

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
            ? "verified Natural Earth world + semantic surface classification imported into durable .aeris storage"
            : "verified Natural Earth world and semantic surface classification already present",
    };
}

}  // namespace aeris::desktop
