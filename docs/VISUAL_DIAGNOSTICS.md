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


## Godot rendered-frame artifact

The Godot integration CI also captures one canonical rendered frame from the
actual walking scene. This complements the structural headless checks; it does
not replace them and it is not a pixel-perfect golden-image gate.

The capture contract is:

- Godot 4.7.2 runs with the project's `gl_compatibility` renderer on a virtual
  X11 display instead of the `--headless` display driver;
- the requested window size is fixed at 1600x900;
- `main.tscn` is instantiated with the normal seed-42 mountain demonstration
  spawn;
- automatic scene processing is disabled after startup, then the existing near
  and distant terrain update methods are advanced with zero delta until their
  initial work queues are empty, so the screenshot does not depend on runner
  speed or simulation time;
- capture waits for `RenderingServer.frame_post_draw` before reading the root
  viewport texture;
- the script rejects an empty image and a trivially uniform frame, writes
  `walk_spawn.png`, and CI uploads it as the
  `worldsim-godot-visual` workflow artifact.

Godot 4.7 documents that `Viewport.get_texture().get_image()` can be used for
screen capture and specifically requires waiting for
`RenderingServer.frame_post_draw` to avoid an empty or stale image. Its command
line contract exposes `--rendering-method`, `--windowed` and
`--resolution`; `--headless` selects the headless display driver and is kept
for correctness tests rather than rendered-frame review:

- https://docs.godotengine.org/en/4.7/classes/class_viewport.html
- https://docs.godotengine.org/en/4.7/tutorials/editor/command_line_tutorial.html

GitHub Actions artifacts explicitly support retaining screenshots and other test
outputs after a workflow completes. The rendered frame therefore stays a review
artifact with the same 14-day retention policy as the existing C++ visual
diagnostics:

- https://docs.github.com/en/actions/concepts/workflows-and-actions/workflow-artifacts
- https://docs.github.com/en/actions/tutorials/store-and-share-data

The screenshot is intentionally not compared byte-for-byte in CI. Mesa/software
renderer revisions, rasterization details and anti-aliasing can change pixels
without changing the world presentation contract. Numeric terrain invariants,
mesh/collision checks and runtime parsing remain the automated gates; the PNG is
for human/model visual inspection.
