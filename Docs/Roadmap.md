# Strata roadmap

> Strata: worlds built in layers. Streamed on a budget, scaled to the view, built by people and AI agents together.

This document records what Strata is meant to be and the order in which it gets there. It is updated when a
wave lands. How the engine works today is described in [Architecture.md](Architecture.md).

## Pillars

- **Layered by construction.** The engine modules form an enforced dependency stack: Core, then Asset, Reflection and
  Scene, then Renderer, Physics, Audio and Scripting, then Runtime. Tooling (asset pipeline, network, ImGui, editor)
  sits beside the stack and never links into a shipped game. One composition root registers every module, the
  registries are open to modules, games and tests, and a CTest fails the build on any forbidden include.
- **Streamed on a budget.** Every resident byte belongs to a pool whose budget comes from VK_EXT_memory_budget. Content
  is cooked for partial reads: BC-compressed textures with the mip tail first, and zstd-blocked, range-readable packs
  ordered by first use with dependency and bounds tables. Textures refine to the mip the screen needs, and scenes and
  sub-scenes load asynchronously. No frame waits on disk and no frame uploads unbounded bytes.
- **Cost follows change and view.** Transforms are dirty-tracked, hierarchy queries are cached, render proxies persist
  in a spatial index with parallel extraction and material-free instancing, and physics is correctly sized and batched.
  Per-frame cost tracks what moves and what is on screen, not how big the world is.
- **Layered worlds.** A world is one scene organized into data layers, cooked into grid cells that stream additively
  around streaming sources, so a world can be larger than memory or than one ECS registry.
- **Instruments in the product, gates in CI.** Budgets, residency, upload queues and frame stages are status pills,
  panels and viewport views (Streaming, LOD, Mip). The same numbers are commands (asset.stats, editor.perf) and Release
  perf tests whose checked-in budgets can only go down.
- **Agent-native and crash-contained.** Every action is one undoable command with a JSON Schema, shared by the UI,
  command scripts, JSON-RPC and MCP. The Activity journal shows what people and agents did and replays it as a
  regression script. Game code is hot-reloaded C++ behind a versioned, crash-contained C ABI.
- **Its own face, lit from the first frame.** The 'Bedrock' mineral look uses Basalt surfaces, Limestone text and an
  Ochre accent shared with the selection outline. Type is Inter and JetBrains Mono with line icons, and the four-band
  strata mark gives the motif. The editor cold-starts in 0.4 s into a launcher whose templates open lit under a
  procedural sky.

## Status

**Wave 1 landed (2026-10-10).** Six workstreams merged into one integration branch, in the order perflab,
module-foundation, scene-core, asset-residency, editor-shell, first-light (the perf lab, planned inside
module-foundation, ran as its own workstream):

- `perflab`: Release-only perf suites (`StrataTests.Perf`, `StrataTests.PerfGPU`, label `perf`) that check metrics
  against `StrataTests/Perf/Budgets.json` (at most 1.5 times the value measured on the reference machine; budgets only
  go down), and `Platform::GetProcessMemory` with peak private bytes and working set on every platform.
- `module-foundation`: the layer table `StrataTests/Architecture/Layers.json` enforced by `Architecture.Layering` with a
  shrinking allowlist (14 known exceptions, each owned by a later workstream), one composition root
  (`Engine::RegisterBuiltinModules`) that replaced the hidden link-time back-edges, registries open to modules, games
  and tests, scene systems ordered by declared constraints, and scenes that keep the components of modules a build
  lacks.
- `scene-core`: a cached hierarchy, world transforms recomputed only below what changed (in parallel for wide changes),
  name and tag indices, cached script dispatch lists and batched deletes and undo. A clean transform update of a million
  entities visits nothing, deleting 10,000 of 100,000 roots takes about 45 ms (54.6 s before), and an editor frame of a
  100,000- or 1,000,000-entity scene costs about 0.25 ms of CPU with the new shell (100,000 idle entities cost 14.3 ms
  before); `editor.wait` reports frame times, which `PerfGPU.Editor` gates.
- `asset-residency`: per-pool memory budgets derived from the device, least-recently-used eviction, pins, a prioritized
  and cancellable load queue with bounded bytes in flight, GPU uploads in budgeted steps through reused staging, loaders
  that take their bytes over, `asset.stats` and `asset.setBudget`, scripts that keep and release assets, and
  `StrataRuntime --asset-budget-mb`; `PerfGPU.Streaming` sweeps a 447 MB stress project within a 128 MB texture budget.
- `editor-shell`: the editor UI as its own library behind `EditorHost`, the Bedrock theme with Inter, JetBrains Mono and
  Lucide icons at the display's scale, a widget kit and panel registry, a main toolbar, status pills that lead to what
  is wrong, a viewport-first default layout, idle throttling (30 frames per second, 10 without focus) and headless UI
  tests that scan the UI code for literal colors and sizes.
- `first-light`: a procedural sky that feeds image-based lighting, preview lighting and a hidden HUD in editor views,
  the `basic3d` template, per-scene editor cameras that frame what a scene renders, recent projects, a launcher with New
  Project, Open Sample, About and "Connect an AI agent", the strata mark as window and executable icon, and the build's
  git commit in its version.

Open items and deferrals later waves own:

- The allowlist's 14 exceptions: the application shell's service wiring (7, `engine-modules`), asset finalization on
  the renderer's command list (`mip-streaming`), physics reading the renderer's `Mesh` (Mesh v2, later) and scene
  systems that call each other directly (5, `physics-at-scale`). Ordering constraints that name systems a build may
  lack need a policy once subsystems become optional (`engine-modules`).
- A world seen all at once still peaks at about 970 MiB of private memory in the exported runtime: no budget can evict
  what is in view, so this needs BC textures and mip streaming (`texture-cook-v2`, `mip-streaming`). Mesh uploads still
  go through NVRHI's upload manager until the budgeted upload queue (`mip-streaming`); `AssetManagerBase::GetStats`
  and eviction scans are linear in the registered assets (`render-scene`, `world-partition`).
- Deleting one entity among 100,000 sibling roots renumbers the sibling list (about 0.5 ms), and the Hierarchy panel
  draws every root each frame (Hierarchy v2 in `editor-authoring`). Starting play copies a 1,000,000-entity scene in
  about 0.7 s (`scene-streaming`).
- The editor starts in 0.7-1.0 s (Release, to its first frame on screen), not the 0.4 s the pillar names. An idle
  editor answers input and automation requests on its next throttled frame (up to 33 ms, 100 ms unfocused) until it
  waits on events instead. Project thumbnails and search in the recent projects move to `content-and-prefabs`.
- Perf budgets hold for the reference machine only, so CI's hosted runners skip the `perf` label; a self-hosted runner
  on the reference machine would make them an automatic gate. Even there, `PerfGPU.Streaming` passes only some runs: its
  750 MB limit on the best of three sweeps' peak private memory and its 6 ms finalization budget sit inside the
  run-to-run spread of the graphics driver's memory and of a maximum over 600 frames, so they need steadier measures.
  Platform code first compiled by CI on Linux and macOS: `GetProcessMemory`, `GetProcessUptime`, `FindHomeDirectory`
  and `FindFallbackFontFiles`.

## Waves

Workstreams of a wave run in parallel in separate branches and merge in the listed order. A wave starts when
the workstreams it depends on have merged.

### Wave 1

- **`module-foundation`: Module foundation: enforced layers, one composition root, open registries, ordered systems,
  perf lab.** Turn the 15 module folders into checked layers before four more waves add code. A CTest fails on new
  forbidden includes. The four hidden link-time back-edges become one explicit composition root. The component registry
  opens to modules, games and tests, scene systems declare their order, and scenes keep the components of modules a
  build lacks. Every later workstream gets a Release perf harness with checked-in budgets (built as the separate
  `perflab` workstream).
- **`scene-core`: Scene core at scale: cached hierarchy, dirty transforms, cached queries, lean script dispatch, cheap
  edits.** Make per-frame scene cost proportional to what changed, not to entity count, and remove the quadratic editor
  bookkeeping.
- **`asset-residency`: Asset residency: budgets, eviction, pins, a re-prioritizable cancellable queue, bounded uploads
  and streaming stats.** Bound memory by budget instead of by everything the session ever touched. Make load requests
  re-prioritizable, cancellable and capped in flight. Bound per-frame uploads and release staging after bursts. Make all
  of it measurable by people, agents and CI.
- **`editor-shell`: Editor shell: Bedrock theme, real typography and icons, DPI, viewport-first layout, panel registry,
  pills, idle throttling, testable UI.** Answer the owner's 'make it professional' directly. Before this wave the editor
  ran Hazel's verbatim theme on ImGui's debug font (no font was ever added), spent about 64% of the window on chrome,
  idled at 1268-1899 FPS and had no test of any panel. It should become a recognizable product with a testable UI.
- **`first-light`: First light: launcher, templates, procedural sky, preview lighting, framed scenes, brand mark.**
  Nothing a person or an agent opens should start black, empty or badly framed: the editor opens into a launcher, new
  projects start from a template lit by a procedural sky, scenes open framed, and the editor and games carry the strata
  mark.

### Wave 2

- **`texture-cook-v2`: Texture cooking v2: BC compression, better mips and a mip-tail-first layout.** Cut texture VRAM,
  pack size, disk I/O and upload bandwidth 4-8x with GPU block compression. Store mips smallest first so mip streaming
  can read the tail by byte range. Depends on `asset-residency`, `module-foundation`.
- **`pack-v2`: Pack v2 and export: compressed, range-readable, dependency- and bounds-aware packs, group loading,
  non-blocking Export.** Ship content in a form streaming can use: packs of compressed blocks that are read by byte
  range, ordered by first use and carry dependency and bounds tables, loaded by group, and written by an export that
  never blocks the editor. Depends on `asset-residency`, `module-foundation`, `editor-shell`.
- **`render-scene`: Persistent render scene: spatial index, parallel extraction, material-free instancing,
  visibility-driven requests, scalable overlays.** Make renderer CPU cost and asset traffic scale with what is visible
  and what changed. Depends on `scene-core`, `asset-residency`, `module-foundation`.
- **`editor-authoring`: Authoring essentials: asset inspector (materials, import settings), Hierarchy v2, Inspector v2,
  clipboard, focus-scoped shortcuts.** Let a person build and dress a scene by hand at professional speed and at any
  scene size. Depends on `editor-shell`, `first-light`, `scene-core`.
- **`streaming-observatory`: Streaming observatory: frame profiler, Streaming and Performance panels, LOD and streaming
  viewport views, perf logs.** Show Strata's streaming and scaling pillars where people and agents work: a frame
  profiler, Streaming and Performance panels, LOD and streaming views of the viewport and perf logs, with the same
  numbers available as commands. Depends on `editor-shell`, `asset-residency`, `module-foundation`.
- **`physics-at-scale`: Physics at scale: sized capacities in scene settings, O(1) contact bookkeeping, batched bodies,
  parallel write-back, scene services.** Keep physics correct and cheap at 100k bodies, and while cells stream bodies in
  and out: no dropped contacts, no 65,536-body wall, and Strata's glue cheaper than Jolt's own step. Remove the
  Scripting-to-Physics and Audio-to-Physics edges. Depends on `scene-core`, `module-foundation`.

### Wave 3

- **`mip-streaming`: Mip streaming and a budgeted upload queue (GPU-free asset layer).** Make texture memory scale with
  screen resolution instead of content size. Textures arrive as their mip tail, refine to the mip the screen needs, and
  give back top mips first under pressure. All uploads go through a staging ring with per-frame byte and time budgets,
  optionally on the transfer queue. Remove NVRHI from the asset layer. Depends on `texture-cook-v2`, `pack-v2`,
  `render-scene`, `asset-residency`, `streaming-observatory`.
- **`scene-streaming`: Asynchronous scenes, additive sub-scenes and compiled prefabs.** No scene change or large spawn
  freezes a frame. Scenes are built on workers, their dependency closure is prefetched within budget, and sub-scenes
  merge additively in time-sliced batches and unload in one step; world cells later use the same mechanism. Prefabs
  spawn without parsing JSON. Depends on `asset-residency`, `pack-v2`, `scene-core`, `physics-at-scale`.
- **`agent-activity`: Agent-native editor: activity journal and replay, agent presence, notifications, problems panel,
  script-build path fix.** Make the AI-drivable editor visible and trustworthy: an activity journal of what people and
  agents did that replays as a regression script, the presence of connected agents, notifications, a problems panel, and
  script builds whose paths no longer break them. Depends on `editor-shell`, `editor-authoring`.
- **`content-and-prefabs`: Content Browser v2, safe asset operations, dependency-aware async import, prefab basics,
  project thumbnails.** Make managing and reusing content professional. Depends on `editor-authoring`, `pack-v2`,
  `texture-cook-v2`.

### Wave 4

- **`engine-modules`: Engine modules: per-layer libraries, EngineModule lifecycle, scene contexts, optional subsystems,
  tooling-free runtime.** Turn the layer table into the build. Each layer becomes its own static library, and
  subsystems start through an EngineModule lifecycle instead of hard-coded Application code. Per-scene contexts replace
  the process-wide 'active' asset manager and script engine. Physics, Audio, Scripting and Network can be compiled out,
  and StrataRuntime structurally excludes tooling and ImGui (today Dear ImGui ships in StrataRuntime.exe although the
  runtime disables it). Depends on `module-foundation`, `physics-at-scale`, `mip-streaming`, `scene-streaming`,
  `content-and-prefabs`.
- **`world-partition`: Layered world partition: data layers, grid cells and streaming sources.** Deliver the signature
  feature. A world is authored as one scene organized into named data layers. Strata cooks each layer into grid cells
  that stream additively around streaming sources, so a world can be much larger than memory or than one ECS registry
  (1,048,575 entities) while frames stay within budget. Depends on `scene-streaming`, `render-scene`,
  `physics-at-scale`, `pack-v2`, `streaming-observatory`.
- **`gpu-driven-rendering`: GPU-driven culling and indirect drawing.** Scale rendering to millions of instances.
  Proxies are frustum-culled, LOD-selected and occlusion-tested on the GPU, then drawn with indirect draws from shared
  geometry buffers, so the CPU cost of drawing stays constant in instance count. Depends on `render-scene`,
  `mip-streaming`.

### Later (wave 5 and beyond)

- Mesh v2: quantized vertices, per-LOD streaming, and a MeshGeometry asset so physics stops depending on the renderer's
  Mesh.
- A binary cooked scene format.
- JobSystem v2 with work stealing and a frame task graph.
- 64-bit entity handles.
- Per-world audio listeners and input contexts.
- Native game modules and an exported engine package.
- Per-property prefab overrides.
- Audio streaming from packs.
- BC6H.
- ImGui multi-viewports.
- An editor preferences panel (UI scale, idle FPS, external editor).
