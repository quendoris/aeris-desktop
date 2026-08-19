# AERIS Desktop

Desktop application for AERIS (`.aeris`) maps.

AERIS Desktop owns the Qt workbench, interactive Globe/Flat presentation, Layers UI, canvas interaction, visual styling, and desktop-specific orchestration. It deliberately does **not** define the `.aeris` format, canonical geographic encodings, projection mathematics, source verification, or storage semantics; those belong to the platform-neutral AERIS core.

## Repository boundary

Dependency direction is one-way:

```text
AERIS core  <-  aeris-desktop
```

The desktop must never carry an alternative implementation of `.aeris`. Core source is consumed through CMake `FetchContent` at an exact commit, so a desktop build is reproducibly tied to one normative core revision.

The initial split is behavior-preserving and was migrated from `quendoris/cartography-aeris` at core commit:

```text
6e79d1e9ca52e99d7a1e12580386a30d9104a15d
```

The old in-tree viewer remains in the core repository until this standalone build passes its own CI and screenshot/probe equivalence checks; removal is a later cleanup change.

## Build

Requirements:

- CMake 3.16+
- C++17 compiler
- Qt 6 Core + Widgets
- Git/network access during configure to obtain the pinned AERIS core revision

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target aeris_viewer --parallel 2
```

The core dependency can be overridden deliberately for compatibility testing:

```sh
cmake -S . -B build \
  -DAERIS_CORE_GIT_REPOSITORY=https://github.com/quendoris/cartography-aeris.git \
  -DAERIS_CORE_GIT_TAG=<exact-commit>
```

Do not use a moving branch such as `main` for release builds.

## Verified demo world

The current workbench proof uses pinned Natural Earth source bytes. Fetch them explicitly:

```sh
cmake -DDESTINATION=dev-data/natural-earth-v5.1.2 \
  -P scripts/fetch_demo_world.cmake
```

Then run:

```sh
./build/aeris_viewer --snapshot dev-data/natural-earth-v5.1.2
```

The fetch script verifies individual SHA-256 identities; the runtime source loader verifies the aggregate source identity again before rendering.

## Desktop conformance probes

The migration retains the existing frontend proofs as independent targets:

- `aeris_viewer_layer_stack_probe` — stable layer-role and visibility behavior;
- `aeris_viewer_scene_probe` — verified Globe/Sinusoidal/Mollweide scenes and Unfold endpoints;
- `aeris_viewer_controller_probe` — stale-result ownership across asynchronous scene/source/unfold changes.

CI also performs an offscreen startup smoke test and renders physical, political, and Unfold screenshots.

## Scope after the split

Desktop-only work belongs here: pan/zoom interaction, presentation styling, layer controls, inspectors, desktop cache/network integration, packaging, and visual regression tests.

Format/storage/source/projection invariants and cross-platform conformance fixtures belong in the AERIS core repository so the desktop and Android applications consume exactly the same semantics.

## License

AGPL-3.0-only. Source files carry SPDX identifiers; the repository will retain the same full AGPL-3.0 license text as the AERIS core.
