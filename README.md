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
cmake --build build --target aeris-desktop --parallel
./build/aeris-desktop
```

The first desktop slice deliberately starts project-first: `File -> Open project` uses `aeris-core`'s `ProjectStore` acceptance path for `.aeris`. The old reference viewer remains in `aeris-core` temporarily as a cartographic proof/debug surface while the real renderer, durable layer reader, viewport/LOD pipeline, and Unfold tool move here.

## UI direction

A projection is a tool applied to the map, not a permanent application mode. Physical/political/terrain presentation belongs to the layer/style model rather than a fixed left-side mode switcher. The main map stays central; layers and the developer inspector are optional surfaces.

License: AGPL-3.0-only.
