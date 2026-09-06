# AERIS Desktop

Desktop application for AERIS (`.aeris`) maps.

The normative `.aeris` format, canonical cartographic core, storage/verifier contracts, source adapters, projection mathematics, elevation codec, and conformance fixtures live in [`quendoris/aeris-core`](https://github.com/quendoris/aeris-core). This repository is a Qt desktop consumer of that core and must not define an alternative interpretation of `.aeris`.

## Development layout

The easiest local layout is two sibling checkouts:

```text
work/
├── aeris-core/
└── aeris-desktop/
```

A different core checkout can be selected explicitly with `-DAERIS_CORE_SOURCE_DIR=/path/to/aeris-core`.

## Build

Linux dependencies: CMake, a C++17 compiler, Qt 6 Widgets/Concurrent development files, SQLite development files, libtiff development files, and the sibling `aeris-core` checkout.

On Arch Linux the required packages are available as:

```bash
sudo pacman -S --needed base-devel cmake qt6-base sqlite libtiff git curl
```

Then clone/update the two sibling repositories and select the current Desktop development branch plus the exact core revision pinned by Desktop CI:

```bash
mkdir -p ~/src/aeris
cd ~/src/aeris

git clone https://github.com/quendoris/aeris-core.git
git clone https://github.com/quendoris/aeris-desktop.git

git -C aeris-core fetch origin
git -C aeris-core checkout 533b5af0ed8d641c2517136009bd20e298d636c4

git -C aeris-desktop fetch origin
git -C aeris-desktop checkout agent/desktop-foundation-v0
git -C aeris-desktop pull --ff-only

cd aeris-desktop
cmake -S . -B build \
  -DAERIS_CORE_SOURCE_DIR=../aeris-core \
  -DCMAKE_BUILD_TYPE=Release
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

## Import real NOAA ETOPO 2022 terrain

AERIS currently accepts either official NOAA/NCEI ETOPO 2022 v1 global 60 arc-second GeoTIFF filename:

```text
ETOPO_2022_v1_60s_N90W180_surface.tif
ETOPO_2022_v1_60s_N90W180_bed.tif
```

The production importer deliberately validates both the official filename and the exact `21600x10800` global grid. Download one of those files separately from NOAA/NCEI; AERIS does not silently fetch acquisition data on project open.

With a mutable world project open in Desktop, use:

```text
Data -> Import NOAA ETOPO 2022 elevation…
```

Select the downloaded GeoTIFF. Import runs in the background and converts the source into durable numerical `.aeris` state:

- 72 canonical 30-degree detail tiles at 60 arc-seconds;
- one 15 arc-minute numerical overview;
- exact source SHA-256 and byte size;
- NOAA/NCEI provenance metadata.

After the import reports success, the original GeoTIFF is acquisition-only and is no longer required. You can close Desktop, move/delete the TIFF, reopen `demo.aeris`, and terrain must still render from the project alone.

At whole-world scale the renderer uses the overview. At roughly `3x` zoom and above it requests only viewport-relevant detail resources. Detail `ProjectStore` open/verification/decode is performed on a dedicated worker; the first high-zoom paint remains I/O-free and falls back to the overview until detail arrives. The in-memory detail cache is currently bounded to 16 tiles.

## Current interaction model

- Globe drag changes geographic camera orientation. Interactive preview generations are cancellable; releasing the mouse requests verified geometry.
- Wheel zoom immediately transforms the last valid vector frame, so a new generation never needs to blank the map.
- High-zoom elevation detail loads asynchronously while overview terrain remains visible.
- Sinu-Mollweide, Mollweide and Sinusoidal live under `Tools -> Unfold / projection`; flat views support panning and cursor-anchored wheel zoom.
- Physical/political presentation is layer composition, not an application mode switch; political geometry remains above terrain.
- Developer Inspector is hidden by default.

## Manual terrain pass

Useful things to inspect while exercising the real ETOPO build:

- ocean bathymetry should remain in the blue elevation range rather than the synthetic CI fixture's green/ochre artefacts;
- zoom through the overview/detail transition around `3x` and watch for blocking, flashing or tile-shaped seams;
- rotate the Globe while detail is loading and verify stale terrain does not pop into the new camera position;
- zoom/pan across 30-degree tile boundaries and inspect hillshade continuity;
- open `Tools -> Unfold / projection`, drag the seam, calculate each planar projection, and compare terrain/political alignment;
- return to Globe and confirm its independent viewport is restored;
- close/reopen the `.aeris` after moving/deleting the source ETOPO TIFF and confirm terrain is unchanged.

The final animated Globe-to-sheet unfold transition, richer multilevel terrain LOD, cities/routes and the GPU rendering path remain later slices.

License: AGPL-3.0-only.
