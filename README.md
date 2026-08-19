# AERIS Desktop

Desktop application for AERIS (`.aeris`) maps.

The normative `.aeris` format, canonical cartographic core, storage/verifier contracts, source adapters, projection mathematics, and conformance fixtures live in [`quendoris/aeris-core`](https://github.com/quendoris/aeris-core). This repository is a Qt desktop consumer of that core and must not define an alternative interpretation of `.aeris`.

## Development layout

The easiest local layout is two sibling checkouts:

```text
work/
├── aeris-core/
└── aeris-desktop/
```

A different core checkout can be selected explicitly with `-DAERIS_CORE_SOURCE_DIR=/path/to/aeris-core`.

## Build

Dependencies on Linux: CMake, a C++17 compiler, Qt 6 Widgets development files, SQLite development files, and the sibling `aeris-core` checkout.

```bash
git clone https://github.com/quendoris/aeris-core.git
git clone https://github.com/quendoris/aeris-desktop.git
cd aeris-desktop

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target aeris-desktop aeris-demo-project --parallel
```

## Build a real demo `.aeris`

The development fixture goes through the same verified Natural Earth adapters, canonical project bridge and durable layer-stack API as the product path. It does not manufacture SQLite rows directly.

From the `aeris-desktop` checkout:

```bash
cmake \
  -DDESTINATION=dev-data/natural-earth-v5.1.2 \
  -P ../aeris-core/scripts/fetch_demo_world.cmake

./build/aeris-demo-project \
  dev-data/natural-earth-v5.1.2 \
  demo.aeris
```

`demo.aeris` contains the durable canonical land + admin0 sources and the built-in five-layer world stack. After it is created, the acquisition directory may be removed; opening/rendering the project must not need the original SHP/DBF files.

```bash
rm -rf dev-data/natural-earth-v5.1.2
./build/aeris-desktop
```

Use `File -> Open project…` and select `demo.aeris`.

The map is rendered from the durable project source/layer model. Layer visibility changes in the Layers dock are acknowledged `.aeris` transactions rather than unsaved Qt state.

## Current interaction model

- Globe drag changes geographic camera orientation. Interactive preview generations are cancellable; releasing the mouse requests verified geometry.
- Wheel zoom immediately transforms the last valid vector frame, so a new generation never needs to blank the map.
- Sinusoidal and Mollweide live under `Tools -> Unfold / projection`; flat views support panning and cursor-anchored wheel zoom.
- Physical/political presentation is layer composition, not an application mode switch.
- Developer Inspector is hidden by default.

Detail-tier/LOD queries, adaptive unfold animation, terrain/elevation and the GPU rendering path are subsequent slices; the current desktop slice establishes the durable project + live vector viewport boundary they will use.

License: AGPL-3.0-only.
