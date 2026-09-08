# C++ visual diagnostics

`worldsim_visual_dump` is a developer-only inspection executable for world-generation output. It samples the authoritative C++ `TerrainGenerator` and `TectonicModel` directly and does not depend on Godot, GPU rendering, or engine presentation code.

The purpose is twofold:

- allow visual review of world-generation geometry in environments where Godot is unavailable;
- preserve raw floating-point fields beside the colored previews so a suspicious visual artifact can be checked numerically instead of being confused with a palette or renderer issue.

## Usage

Build the normal development preset, then generate one seed:

```bash
cmake --preset dev
cmake --build --preset dev
./out/dev/worldsim_visual_dump --seed 42 --width 512 --height 256 --output out/visual-dump
```

Generate the fixed review suite (`1, 7, 19, 42, 63`):

```bash
./out/dev/worldsim_visual_dump --suite --width 512 --height 256 --output out/visual-dump
```

Each seed directory contains:

- `elevation.pfm` / `elevation.ppm` — authoritative terrain;
- `macro_elevation.pfm` / `macro_elevation.ppm` — tectonic macro relief before bounded terrain detail;
- `crust_affinity.pfm` / `crust_affinity.ppm`;
- `boundary_forcing.pfm` / `boundary_forcing.ppm` — legacy nearest-boundary diagnostic;
- `uplift_forcing.pfm` / `uplift_forcing.ppm`;
- `divergence_forcing.pfm` / `divergence_forcing.ppm`;
- `tectonic_response.pfm` / `tectonic_response.ppm` — `uplift_forcing - divergence_forcing`;
- `plates.ppm` — plate ownership with a diagnostic pixel boundary;
- `metrics.json` — area-weighted global summary, terrain/macro correlation and longitude-seam diagnostics.

PFM files are single-channel IEEE-754 float rasters using the standard negative scale marker for little-endian samples. Rows are written bottom-to-top as required by PFM. PPM files are presentation-only previews; model conclusions should be checked against the corresponding PFM data or metrics when color mapping could be misleading.

The fixed suite is intentionally small. Multi-seed numerical robustness remains the responsibility of core tests; the five-seed suite exists for human/model visual review rather than exhaustive statistical validation.

## CI

The core CI job runs a small smoke invocation through CTest and then generates the five-seed 512x256 suite. The resulting directory is uploaded as the `worldsim-visual-dump` workflow artifact so it can be inspected without a local Godot installation.
