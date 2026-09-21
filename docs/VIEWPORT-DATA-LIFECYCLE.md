# Viewport-driven data lifecycle

Status: DRAFT implementation boundary

This document defines the Desktop acquisition boundary for the zero-start AERIS workflow. It is intentionally non-normative for the `.aeris` format itself; canonical storage, geometry, projection, provenance and integrity contracts remain owned by `aeris-core`.

## 1. Product rule

A normal AERIS launch must be able to begin with a newly created, structurally valid local `.aeris` project and no pre-downloaded geographic dataset.

The empty project is not an error state and is not a temporary file format. It is the same durable `.aeris` container that later owns verified world data, user layers and project state.

The application-owned startup project is created from zero on first use and then reused. AERIS must not recreate it on every launch because doing so would discard already acquired canonical content and user state.

## 2. One-way flow

The intended data flow is:

```
visible viewport / requested resolution
        |
        v
Desktop viewport-demand coordinator
        |
        v
provider coverage decision
        |
        v
isolated acquisition worker
        |
        v
verified immutable acquisition bytes
        |
        v
canonical decode / normalization
        |
        v
atomic durable mutation of the same .aeris
        |
        v
ProjectModel refresh -> renderer
```

The renderer never performs network access. AERIS Core never performs network access. View code reports demand; Desktop orchestration decides whether durable coverage is missing.

## 3. Bootstrap tier

The first implemented demand action is the minimum verified world.

If the current mutable project lacks the built-in land/coastline/political/label/surface-semantic baseline, the first viewport demand automatically starts the existing isolated Natural Earth acquisition/import worker.

This replaces the old user-facing requirement to select “Install base world” after creating an empty project. The manual action remains available as repair/reacquisition and the local snapshot import remains an advanced/offline fallback.

The bootstrap dataset is intentionally global and low resolution. Its small size and whole-world purpose make one-time acquisition reasonable.

## 4. Higher-resolution tiers

Higher detail must not copy the bootstrap strategy blindly.

In particular, entering a zoom threshold must **not** automatically download the existing full global ETOPO GeoTIFF. That is a whole-dataset importer, not viewport streaming.

Future providers should expose immutable geographic chunks that can be requested by coverage and target resolution. Examples of logical tiers are:

- low-resolution world overview;
- progressively finer vector geometry;
- regional terrain/elevation chunks;
- labels, settlements, roads and other optional thematic chunks.

The exact provider and resolution thresholds are implementation policy, not project semantics.

## 5. Durable vs transport state

Machine-local acquisition cache files exist only to make transport resumable and efficient. They are not project truth.

Once a chunk is accepted, the canonical content needed by the project is committed into the `.aeris` through the ordinary verified storage APIs. Removing the acquisition cache must not reinterpret already materialized project content.

No cache path may become canonical project identity.

## 6. Request coalescing

Navigation can generate demand faster than the network can respond. The coordinator therefore owns coalescing and stale-work policy.

The target behavior is:

1. rapid wheel/pan/rotate interaction updates the latest desired viewport;
2. duplicate coverage requests collapse;
3. already-running verified acquisition is not duplicated;
4. presentation never waits synchronously for acquisition;
5. switching to another project prevents stale completion from replacing the visible model;
6. a worker may finish committing valid content to its original project even when that project is no longer visible.

Desktop now coalesces rapid navigation through a 120 ms quiet period and derives a deterministic detail tier plus focus-cell key from the latest visible geographic focus. This key is deliberately not an exact viewport polygon: it is an orchestration identity for deduplication. Regional providers that require full coverage must conservatively expand from the visible footprint rather than clip project geometry to the focus cell.

## 7. Failure and offline behavior

Network failure does not invalidate the project.

A new empty project remains a valid `.aeris` even when acquisition is unavailable. Existing durable chunks remain renderable. Partial transport bytes may remain in the machine-local cache only when the corresponding resume contract can safely validate them.

Manual local import stays available for offline workflows.

## 8. Freeze semantics

A frozen project must remain reproducible without network acquisition for every layer/resource that participates in its project semantics.

Viewport demand must not mutate a frozen project. Missing online detail in a frozen project is a capability/coverage absence, not permission to silently thaw or fetch.

## 9. Acceptance gates

The zero-start lifecycle is acceptable when all of the following hold:

- first launch can create a valid `.aeris` without any prebuilt project asset;
- no modal dataset setup is required for the minimum map;
- the base-world acquisition runs outside the GUI process;
- the UI remains navigable and can close immediately while acquisition is active;
- acquired canonical data survives cache deletion and application restart;
- reopening the same startup project does not redownload already durable data;
- higher-detail providers fetch only missing geographic coverage/resolution rather than an unrelated whole-world payload;
- stale results cannot replace the visible state of another project;
- manual import remains optional fallback rather than the normal product path.

## 10. Current implementation boundary

The current Desktop slice implements:

- empty durable project as normal startup state;
- viewport-demand callback separated from scene rendering;
- automatic minimum-world acquisition/repair through that callback;
- existing isolated-worker, cancellation, resume and project-identity safeguards;
- normalized detail tiers and deterministic focus-cell demand keys;
- 120 ms latest-request coalescing;
- same-project model refresh without resetting camera, zoom, pan or selected surface.

Still pending:

- exact/conservative geographic viewport footprint calculation suitable for provider queries;
- 50m/10m vector detail providers;
- regional/chunk-capable terrain provider;
- durable coverage indexing where a provider requires more than the existing source/resource/layer model;
- eviction/storage-budget UX, if AERIS later permits selectively non-durable online caches.
