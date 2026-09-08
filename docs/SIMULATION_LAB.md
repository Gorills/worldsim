# Full-world simulation laboratory

## Decision and references (2026-09-08, before implementation)

The existing first-person scene and global tectonic map both initialize the
terrain-only simulation. They can validate authoritative geological elevation,
walking reconstruction and collision, but they cannot expose the implemented
`climate -> hydrology -> geology -> soil -> vegetation -> fauna` chain. The
GDExtension already exposes the full simulation, active-cell render packet,
field descriptors and aligned field arrays; the missing contract is a generic,
time-aware diagnostic client.

Add a separate `simulation_lab.tscn`. It initializes the full default simulation
and remains a read-only presentation client except for existing focus/time
controls. It must not become authoritative state and it must not change the
terrain walker or global seed/tectonic viewer.

Authoritative API and comparable visualization practice checked for this
decision:

- Godot **4.7** `Image`: runtime images are created from explicitly sized packed
  byte data and can back an `ImageTexture`:
  https://docs.godotengine.org/en/4.7/classes/class_image.html
- Godot **4.7** `OptionButton`: the field selector uses indexed items, metadata
  and the `item_selected` signal:
  https://docs.godotengine.org/en/4.7/classes/class_optionbutton.html
- Godot **4.7** `Control` / `CanvasItem`: map inspection uses `_gui_input`, while
  the history plot uses `_draw` and `queue_redraw()`:
  https://docs.godotengine.org/en/4.7/classes/class_control.html
  https://docs.godotengine.org/en/4.7/classes/class_canvasitem.html
- ParaView 6.1 color-map reference: scalar arrays are selected by name, each
  array retains its transfer function, legends expose the mapped range, and a
  grow-only range mode avoids silently shrinking/rescaling the colors at every
  timestep:
  https://docs.paraview.org/en/latest/ReferenceManual/colorMapping.html

ParaView is a design reference, not a dependency and not a claim that this
small Godot client provides a general scientific-visualization pipeline.

## Presentation contract revision (2026-09-08, before UI revision)

The first implementation exposed the complete field registry directly. That
is useful as an adapter smoke test, but it gives every raw key equal visual
weight and does not tell a person which relationship to inspect. The normal UI
therefore presents five curated questions -- world overview, climate, water
cycle, ecosystem and adaptive LOD -- with a small related-layer selector. The
complete registry remains available only behind an explicit advanced switch.
Each question owns its explanation, relevant cell values and expected behavior;
changing a question never changes authoritative simulation state.

The visualization host uses a 1920 x 1080 design viewport, starts in fullscreen
and scales 2D controls with `canvas_items`. Containers and anchors remain the
layout authority so the lab still behaves on displays with a different physical
resolution. This is a project-wide window default; headless tests may override
the window size without changing the simulation contract.

Sources checked for this revision:

- Godot **4.7** `ProjectSettings`: `display/window/size/mode = 3` selects
  fullscreen, while viewport width/height define the base size:
  https://docs.godotengine.org/en/4.7/classes/class_projectsettings.html
- Godot **4.7** multiple-resolution guidance uses 1920 x 1080 as a supported
  desktop base and documents `canvas_items` scaling:
  https://docs.godotengine.org/en/4.7/tutorials/rendering/multiple_resolutions.html
- Godot **4.7** container/anchor guidance: container children are laid out by
  their parent and should not depend on hand-positioned offsets:
  https://docs.godotengine.org/en/4.7/tutorials/ui/size_and_anchors.html
- ParaView 6.1 display guidance uses an active view plus property/display
  controls instead of presenting every data property as equal top-level
  content. WorldSim applies that practice as one dominant map, one contextual
  side panel and explicit diagnostic modes:
  https://docs.paraview.org/en/latest/UsersGuide/displayingData.html

## Data contract

`WorldSimulationNode::sample_field_equirectangular()` resolves every pixel
center to the actual active leaf. Intensive fields are displayed directly.
When density normalization is requested, extensive fields are divided by the
represented active-cell area. This prevents a coarse LOD cell from appearing
more intense merely because it owns more area. Raw authoritative values remain
available through `get_field_values()` and the cell inspector.

`sample_lod_equirectangular()` exposes active simulation level as a categorical
diagnostic. `inspect_direction()` returns the resolved active cell, its area and
all raw registered field values. These methods are adapter reads; no new kernel
state or serialization is introduced.

The laboratory initially uses a 512 x 256 equirectangular texture. Color ranges
are initialized from the active-cell 2nd/98th percentiles per field and then
only grow during a run. The user can explicitly reset the range. A visible
legend always reports the mapped endpoints. Non-finite values are rendered
magenta and remain a test failure in the authoritative core.

## Interaction and performance boundary

- reset creates a full default world for the selected seed;
- play/pause advances one selected time quantum at a bounded wall-clock cadence;
- manual steps support 1 hour, 1 day and 30 days;
- advances longer than one day are divided into at most 24-hour chunks across
  rendered frames; scene-tree reads and redraws remain on the main thread;
- selecting a field redraws the scalar map and selected-field mean history;
- clicking the map inspects the active cell without changing simulation state;
- focus/clear-focus changes simulation interest policy and takes effect on the
  next authoritative tick, as required by `Simulation::step()`;
- map textures are rebuilt only after a complete requested step,
  layer/range change or reset, not
  every rendered frame.

The map sampler is intentionally CPU-side diagnostic code. Interactive cost on
target hardware is **NOT VERIFIED**; geometry, GPU compute and production
planet rendering are outside this slice.

## Validation contract

Headless integration must verify:

- generic elevation sampling agrees with the existing authoritative elevation
  map;
- extensive density sampling equals raw cell value divided by active-cell area;
- direction inspection returns aligned fields and valid cell metadata;
- the LOD map reports refinement after a focused authoritative tick;
- the scene initializes a full world, renders a non-empty map, exposes
  hydrology/ecology fields, advances time, updates statistics/history and can
  inspect a cell.
