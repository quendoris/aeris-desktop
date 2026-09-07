# AERIS codebase maturity audit — 2026-09-07

> Non-normative audit note. This file describes the exact repository state listed
> below. It is not a new AERIS contract and must not override a canonical core
> specification.

## 1. Exact audited state

The audit intentionally measures the active development stack rather than the
three default branches:

```text
Desktop product head
  quendoris/aeris-desktop
  agent/desktop-foundation-v0
  726f6611128cba8c09c5af73503bbbd343e48359

Core dependency pinned by Desktop CI
  quendoris/aeris-core
  533b5af0ed8d641c2517136009bd20e298d636c4

Android current stacked development head
  quendoris/aeris-android
  agent/routing-contract-v0
  4bca659f9eb5c91efbbe23ede3c3c601b24f3331
```

The counting implementation and audit workflow live only on
`aeris-desktop:audit/codebase-stats`. Their own 444 physical / 380 nonblank /
336 code-ish lines are excluded from the product totals below.

## 2. Scale snapshot

| Block | Files | Physical lines | Nonblank | Code-ish |
|---|---:|---:|---:|---:|
| Core | 220 | 46,464 | 39,833 | 34,568 |
| Desktop product | 47 | 10,029 | 8,900 | 8,159 |
| Android | 28 | 3,154 | 2,781 | 2,467 |
| **Total** | **295** | **59,647** | **51,514** | **45,194** |

Cross-repository production and verification mass:

```text
Production code: 34,062 physical lines / about 28,996 code-ish lines
Tests + probes:  17,812 physical lines / about 15,188 code-ish lines
```

The verification-to-production ratio is therefore already unusually high for an
early product: about 0.52 by the language-neutral code-ish metric.

The metric is deliberately approximate for non-Python languages. It removes
blank/comment-only C/C++/Kotlin lines but is not a compiler/parser. Its purpose
is scale and trend tracking, not language benchmarking.

## 3. Documentation mass

Tracked Markdown across the three audited repositories totals approximately
4,812 physical lines.

A manual role audit gives a more useful split than filename heuristics:

```text
Core mathematical/cartographic contracts + conformance   2,285 lines
Core project/storage/source contracts                    1,451 lines
Core UI architecture contract                              238 lines
Engineering + attribution policy                           308 lines
General README / notices / compatibility notes             530 lines
                                                        -------
                                                          4,812 lines
```

Thus roughly 3,974 / 4,812 lines (about 83% of all tracked Markdown, and about
93% of `core/docs/*`) are engineering contracts, architecture, or conformance
material rather than ordinary user documentation.

### 3.1 Mathematical/cartographic contract block

The principal files are:

- `docs/CANONICAL-GEOMETRY.md`
- `docs/GLOBE-HORIZON-TOPOLOGY.md`
- `docs/PROJECTION-SEAM-TOPOLOGY.md`
- `docs/PROJECTION-SINU-MOLLWEIDE.md`
- `docs/REAL-WORLD-CONFORMANCE.md`
- `docs/UNFOLD-TRANSITION.md`

This is the strongest contract/proof area in the project. It defines canonical
edge/ring semantics, WGS84/authalic area, seam-derived topology, globe horizon
semantics, finite-geometry verification, explanatory-vs-normative Unfold state,
and real pinned Natural Earth conformance evidence.

### 3.2 Project/storage/source contract block

The principal files are:

- `docs/AERIS-PROJECT-FORMAT.md`
- `docs/PROJECT-GEOMETRY.md`
- `docs/PROJECT-PROVENANCE.md`
- `docs/SOURCE-ADAPTERS.md`
- `docs/SOURCE-PIPELINE.md`
- `docs/STORAGE-FOUNDATION.md`

The architectural direction remains coherent: source transport is separated
from snapshot verification and adapter decoding; canonical geography is upstream
of projection/rendering; SQLite persistence is source-neutral; `.aeris` is the
canonical durable project; UI/toolkit state is not allowed to define file-format
semantics.

### 3.3 UI architecture

`docs/UI-ARCHITECTURE.md` is a real contract, not a widget sketch. It establishes
at least these important boundaries:

- UI is a view/command surface, not project truth;
- camera inspection does not silently mutate projection parameters;
- final Globe and planar endpoints are normative while animation frames are not;
- user-visible mutations should cross an explicit reusable command boundary.

## 4. Documentation freshness finding

The main documentation risk is no longer absence. It is **mixed temporal
layers that are not clearly labelled as historical**.

The audited Core code is currently:

```text
kProjectSchemaGeneration = 8
kDraftFormatMajor        = 0
kDraftFormatMinor        = 8
```

But several implementation-contract/checkpoint documents still describe much
earlier state as current:

### `PROJECT-GEOMETRY.md`

Claims:

```text
Implemented project draft: schema generation 3 / format 0.3
```

and its non-claims still list layer ordering/configuration, styles, resources and
frozen projects as future work.

### `PROJECT-PROVENANCE.md`

Claims current generation 3 / 0.3 and describes the provenance/geometry bridge as
still awaiting later product ingestion boundaries that have since evolved.

### `STORAGE-FOUNDATION.md`

Explicitly describes a 0.2-era implementation checkpoint. Its non-claims list
canonical layers, styles, general resources, frozen portability and read-only
operation as unimplemented, while the audited tree already contains substantial
`storage/layer`, `storage/style`, `storage/resource`, projection and related
project machinery plus dedicated tests.

These documents remain valuable historical design records, but reading them as
"current implementation" is now unsafe.

### Recommended documentation rule

Every architecture document should carry one of four explicit roles:

```text
NORMATIVE-DRAFT
  intended current semantics; code must be reviewed against it

IMPLEMENTATION-CHECKPOINT
  describes one exact historical commit/generation; never silently updated

CONFORMANCE-RECORD
  records what an exact proof demonstrated and its non-claims

PRODUCT-GUIDE
  describes current user/developer operation, not format semantics
```

An implementation checkpoint should record the exact commit or schema generation
it describes and point to a current architecture index. This preserves valuable
reasoning without allowing an old checkpoint to masquerade as the current
contract.

## 5. Contract → implementation → proof map

The ratings below are audit shorthand, not release claims:

```text
0 = idea only
1 = explicit contract/design exists
2 = implementation exists
3 = deterministic proof/CI exists
4 = product behavior including failure/recovery path is demonstrated
```

### 5.1 Canonical WGS84 geometry and area — 3/4

**Contracts**

- `CANONICAL-GEOMETRY.md`
- relevant parts of `AERIS-PROJECT-FORMAT.md`

**Implementation**

- `src/geo/*`
- `src/geometry/*`
- canonical geometry portions of `src/storage/*`

**Proof**

- authalic and geographic-area tests
- canonical geometry corruption/storage tests
- pinned Natural Earth real-world proofs

**Open boundary**

Pre-1.0 semantics are not frozen; the stable historical compatibility contract
has therefore intentionally not been claimed.

### 5.2 Projection, seam topology and globe geometry — 3/4

**Contracts**

- `PROJECTION-SINU-MOLLWEIDE.md`
- `PROJECTION-SEAM-TOPOLOGY.md`
- `GLOBE-HORIZON-TOPOLOGY.md`
- `UNFOLD-TRANSITION.md`

**Implementation**

- `src/projection/*`
- `src/view/*`

**Proof**

- extensive synthetic tests
- arbitrary seam and polar regressions
- exact pinned Natural Earth compatibility gates
- all current Desktop projection catalog acceptance probes

**Open boundary**

The final historically verified Philbrick Sinu-Mollweide composition/interruption
contract remains explicitly unresolved. That unresolved item is correctly
recorded rather than hidden.

### 5.3 `.aeris` storage/project model — 3/4

**Contracts**

- `AERIS-PROJECT-FORMAT.md`
- historical/checkpoint storage/project documents

**Implementation mass**

The Core storage subsystem alone is approximately:

```text
31 source/header files
9,164 physical lines
8,038 code-ish lines
```

It now contains project/session, provenance, canonical geometry, feature
properties, datasets, layers/layer stack, resources, styles and structured
projection persistence.

**Proof**

Dedicated storage/project workflows and tests cover corruption, atomicity,
concurrency, abrupt process exit, Unicode paths and durable reopen behavior.

**Open boundary**

The current implementation has outrun its generation-specific documentation.
Before Format 1.0 review, the generation-8 schema needs one current source of
truth rather than a chain of checkpoints that stop at generation 3.

### 5.4 Source adapters and verified acquisition boundary — 3/4

**Contracts**

- `SOURCE-ADAPTERS.md`
- `SOURCE-PIPELINE.md`

**Implementation**

- `src/source/*`
- project source bridge/read path
- pinned Natural Earth adapters

**Proof**

- acquisition/hash/path tests
- Shapefile/DBF/provider tests
- source compatibility workflow against immutable Natural Earth bytes

**Open boundary**

The core correctly specifies that acquisition is separate from decoding, but the
product still lacks a general automatic downloader/resume/cache job layer.
Transport architecture exists; product acquisition UX does not yet fulfill it.

### 5.5 Desktop vector map/runtime — 2.5/4

**Implementation**

- Qt workbench/map view
- async scene generation
- Globe + Sinu-Mollweide + Mollweide + Sinusoidal
- persistent layer visibility
- independent Globe/planar viewport state

**Proof**

Desktop CI proves durable project lifecycle, projection catalog, independent
rendering and offscreen pixel output.

**Open boundary**

The first real user pass exposed frame-time/interaction problems that synthetic
acceptance did not model. The current head reduces synchronous terrain work and
removes several GUI-thread data operations, but interactive frame-time and
shutdown latency need explicit product-level budgets/proofs.

### 5.6 Numerical elevation / terrain — 2.5/4

**Implementation**

- exact NOAA ETOPO 2022 60-arc-second TIFF validation
- durable numerical overview + 72 detail resources
- source hash/size provenance
- lazy viewport detail delivery
- bounded in-memory detail cache
- CPU hypsometric/hillshade rendering

**Proof**

- TIFF fixture acceptance
- source deletion/reopen durability
- overview/detail pixel-change acceptance
- I/O-free first high-zoom paint boundary

**Important documentation gap**

There is no dedicated Core/format-level `ELEVATION-CONTRACT.md` describing the
canonical elevation resource semantics, LOD identity, surface-vs-bed semantics,
render material classification, or durability/future compatibility of the tile
codec.

The implementation is therefore substantially ahead of the written architecture
in this area.

### 5.7 Desktop isolated data jobs / shutdown — 2/4

Current Desktop now has:

```text
aeris-desktop
      |
      +-- QProcess --> aeris-data-worker
```

Natural Earth / flags / ETOPO mutations can execute outside the GUI process.
`MainWindow::closeEvent()` cancels map work and kills the active data worker;
`DataJobProcess::cancel()` uses unconditional `QProcess::kill()` rather than
waiting for a graceful importer shutdown.

This is an important process-ownership correction.

**Missing proof**

The current Desktop CI does not yet contain a dedicated acceptance that:

1. starts a deliberately long data job;
2. closes the Desktop and measures process disappearance within a bounded time;
3. kills the data worker during a mutation;
4. reopens and deep-verifies the `.aeris`;
5. retries the job and reaches one correct idempotent final state.

Until that exists, the user's hard "close means close" and power-interruption
contracts are implemented directionally but not proven end-to-end.

### 5.8 Product acquisition/download/recovery — 1/4

The architectural ingredients exist in Core (`VerifiedSnapshot`, immutable
identity, source adapters, content hashes) and the Desktop now has an isolated
writer process.

Still missing as a product subsystem:

- resource catalog/descriptor;
- primary + fallback endpoints;
- resumable `.part` download;
- ETag/Last-Modified or equivalent resume validation;
- persisted job/checkpoint state;
- automatic SHA-256/size verification;
- user-visible progress/cancel/retry;
- safe restart after Desktop/process/machine interruption;
- ready-made starter world so first launch requires no manual acquisition.

This is the clearest current architecture-to-product gap.

### 5.9 Surface classification / Antarctica semantics — 0.5/4

The numerical terrain renderer currently derives presentation mainly from signed
elevation. The real ETOPO Ice Surface dataset demonstrated why this is
insufficient: positive ice-shelf/surface elevation must not automatically mean
"green terrestrial land".

A separate semantic surface classification contract is not yet present:

```text
ocean | land | grounded ice | permanent ice shelf | ...
```

Elevation should remain numerical truth while material/surface classification
drives presentation. This needs a source/data contract before a one-off renderer
mask is added.

### 5.10 Flags / symbolic resources — 2/4

Country flags are durable resources and have lifecycle/rendering support.

The recent user test exposed two runtime issues:

- import used to run synchronously on the GUI thread;
- layer visibility used to reload the full ProjectModel, including eager PNG
  reconstruction.

The current Desktop head fixes those two paths, but initial project load still
eagerly reconstructs PNG resources. A lazy/background image resource cache would
better match the same viewport/bounded-work principles used for terrain.

### 5.11 Android platform boundary — 2/4

The Android stack has a comparatively small codebase but unusually explicit
responsibility boundaries:

- Core owns canonical `.aeris`, geographic/source/projection/storage semantics;
- Android owns SAF/FD transport, platform lifecycle, touch/render surface, JNI,
  and later platform key integration;
- no silent whole-file copy for stream-only document providers;
- viewport generations reject stale async rendering batches;
- route freshness/privacy and pairing flows have JVM tests.

The decisive missing milestone is still canonical Core verification/rendering of
a Desktop-created `.aeris` through the Android FD/VFS path. Until that exists,
Android remains a well-bounded frontend shell rather than an independent AERIS
reader proof.

## 6. Architecture documentation gaps by priority

### P0 — current truth can be confused with historical truth

1. Create one current format/schema map for generation 8.
2. Mark `PROJECT-GEOMETRY.md`, `PROJECT-PROVENANCE.md` and
   `STORAGE-FOUNDATION.md` explicitly as historical checkpoints or update/split
   them so "current" statements cannot contradict the code.
3. Add an architecture index that labels every document by role and freshness.

### P1 — implementation exists without a canonical contract

1. Numerical elevation / tile codec / LOD / provenance semantics.
2. Desktop runtime responsiveness boundaries: no project I/O in paint/input,
   bounded synchronous work, stale-generation ownership.
3. Isolated data-job/shutdown/recovery contract.
4. Resource acquisition/resume/cache contract.
5. Surface classification separate from numerical elevation.

### P2 — useful product/platform boundaries currently live mostly in README/PRs

1. Android FD/VFS integration boundary.
2. Android viewport-generation contract.
3. Route freshness/privacy shared-vs-platform ownership.
4. Private Vault / pairing capsule ownership once the Core cryptographic
   contract starts to exist.

## 7. Proposed architecture index shape

A future canonical `AERIS-ARCHITECTURE-MAP.md` should be an index, not another
large specification. Each row should answer:

```text
Subsystem
Canonical contract(s)
Current implementation owner
Current schema/model identifier
Proof / workflow / fixture
Known non-claims
Freshness / superseded documents
```

Example:

| Subsystem | Contract | Implementation | Proof | Current gap |
|---|---|---|---|---|
| Canonical geometry | CANONICAL-GEOMETRY | core geo/geometry | unit + real-world | pre-1.0 freeze |
| Projection topology | PROJECTION-* | core projection/view | synthetic + NE | final Philbrick layout |
| Project storage | AERIS-PROJECT-FORMAT + current schema map | core storage/project | storage/project CI | docs lag generation 8 |
| Terrain | new elevation contract needed | core codec + Desktop import/render | Desktop terrain probe | semantic surface + richer LOD |
| Acquisition | SOURCE-* + new job contract | future resource manager/worker | kill/resume proof needed | no resumable product flow |
| Desktop runtime | new runtime contract needed | aeris-desktop | frame/shutdown proofs needed | user-facing responsiveness |
| Android reader | Core format + Android boundary | aeris-android + future VFS | cross-platform fixture needed | no canonical reader yet |

The index should link to existing contracts instead of duplicating their
content. Its purpose is to answer "where is the truth for this subsystem?" in
one minute.

## 8. Main audit conclusion

AERIS is not documentation-poor. It has the opposite emerging problem:

> the mathematical and early storage architecture is documented with unusual
> depth, while implementation velocity has begun to outrun the generation and
> runtime documents.

The next documentation work should therefore prioritize **freshness, authority
and indexing**, not raw page count.

The strongest current areas are canonical geometry, equal-area/seam/globe
mathematics, and durable storage verification. The weakest documentation-to-code
boundaries are current schema evolution, numerical elevation, Desktop runtime,
resumable acquisition/recovery and semantic surface classification.
