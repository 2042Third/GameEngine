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

**Wave 2 (in progress).** Workstreams land in the order perf-gates, physics-at-scale, texture-cook-v2, pack-v2,
render-scene, streaming-observatory, editor-authoring:

- `perf-gates`: perf checks confirm a failure by measuring again (up to three attempts, all printed and recorded in
  the results file) instead of failing on one measurement; every limit lives in `StrataTests/Perf/Budgets.json`, with
  an optional roadmap `Target` (the streaming sweep's 750 MB, until `texture-cook-v2`'s block-compressed textures brought
  its budget below that) next to the budget that gates; each metric's statistic is
  justified in its test; `Tools/PerfStability.py` runs the perf label repeatedly and reports each metric's spread, pass
  rate and confirmations. Script dispatch prefetches instances ahead: the hardware prefetchers had followed the
  instances only in some code layouts, so a frame of 300,000 trivial scripts cost 6.6 or 9.8 ms with identical sources
  (now about 6.2 ms in both). On the machine the seven wave-2 workstreams shared, `-L perf` passed five runs in a row
  once it was quiet: all 11 runs at up to 22% CPU load (the perf tests' own) passed without a single confirmation, while
  runs during the other workstreams' builds failed single metrics as well as many. Open: once in about 45 runs, under
  load, the editor failed to open `PerfGPU.Editor`'s generated million-entity scene ("Invalid JSON", cause unknown); the
  test now describes the scene files on disk when the editor fails (`Tests::DescribeJsonFile`).
- `physics-at-scale`: scene services (`Scene/SceneServices.h`, `Scene::RegisterService`/`GetService`) through which
  scripts and audio reach physics and audio, and engine services (`Core/EngineServices.h`) for what outlives scenes,
  which took the five scene-system exceptions off the allowlist (14 to 9); physics capacities sized from the scene
  (`SceneSettings::PhysicsCapacity`, automatic by default, `scene.setSettings`, limited to what a world can reserve),
  with overflow counted, reported once and remembered so the scene's next world grows; contacts recorded in per-thread
  buffers and tracked only for listeners, with constant-time pair bookkeeping; bodies added and removed in batches;
  poses written back and bodies synchronized in parallel; time dilation instead of catch-up spirals. On the loaded
  reference machine (`Perf.Physics`, best of three): 30,000 boxes start and play 30 frames in 274 ms (18.3 s before),
  70,000 boxes all get a body (4,465 errors before) and step in 24 ms frames with Strata's part at 6.6 ms below Jolt's,
  10,000 boxes in 2.3 ms frames (9.4 ms before), and 10,000 bodies are created in one frame in 8.9 ms (24.9 ms before).
- `texture-cook-v2`: textures cook to GPU block compression - BC7 for color and for data of several channels, encoded
  by Strata's own encoder (`Asset/BC7Encoder`: all eight modes, partitions ranked by an estimate and the best fitted;
  the feature test's checker exact, a smooth gradient at 54 dB where BC1 reached 45.6), BC1 for opaque color at `Fast`
  quality (data keeps the full BC7 search there, which keeps its channels apart), BC4 for one-channel data (sampled as
  gray), BC5 for normal maps (Z reconstructed when shading) - with mips filtered by a Mitchell filter (sRGB in linear
  space, alpha-weighted, wrapping like the texture, normal maps renormalized), encoded in parallel, and stored in
  `STTX` version 2 with the mip tail first, so streaming can read it by byte range. Devices without BC support decode
  at load time. The stress project's 447.4 MB of RGBA8 textures cook
  to 111.8 MB (5.6 MB per 2048² texture, 22.4 MB for the 4096² one) in about 7.7 s with 23 workers
  (`Perf.TextureCook`; BC7 takes about 15 us per block on one thread at `High`, 1.2 us at `Fast`), and its pack
  shrinks from 472.7 MB to 137.2 MB. `PerfGPU.Streaming`'s all-resident sweep (1920x1080, 300 frames, every texture
  resident) peaks at 518-531 MB of private memory instead of 970-987 MB with RGBA8 textures (450-461 MB while color was
  BC1); with BC1 color the exported game peaked at 543-558 MB rendering 1920x1080 pixels (a 1280x720 window on the 150%
  reference display) and at 658-659 MB in a 1920x1080 window there, which renders 2880x1620 pixels.

  Open:
  - The editor's `asset.import` still cooks on the main thread (encoding on every worker; about 1.5 s for a 4096x4096
    BC7 color texture), which `content-and-prefabs` makes asynchronous; `asset.reimport` and `asset.setImportSettings`
    import in the background since `editor-authoring`.
  - HDR textures stay RGBA16F (8 bytes per texel) until a BC6H encoder exists.
  - With BC7 color an upload step carries twice the bytes, and `PerfGPU.Streaming.MaxFinalizeTime` (measured before
    `perf-gates` merged as the median of three sweeps' longest finalization) measured 4.1, 5.3 and 7.2 ms in three runs
    against its 5.44 ms budget (the last on a loaded machine, where one sweep peaked at 14.7 ms; 3.6-4.4 ms with BC1).
    It is now a sweep's longest finalization confirmed by up to two more sweeps; if it still fails on a quiet machine
    it needs a steadier measure or smaller upload steps (`mip-streaming`).
- `pack-v2`: asset packs version 2 (zstd-compressed 256 KiB blocks with CRC-32C checksums from 4 KiB-aligned entries,
  read by byte range through one shared file handle, with dependency and mesh bounds tables; version 1 still loads),
  dependencies and bounds recorded at import and answered identically by the editor and games, dependency prefetch
  within the budgets and group loads (`RequestLoadWithDependencies`), packs ordered by first use with optional pruning
  (`Export.AlwaysInclude`), patch packs, and an export that runs on an I/O thread behind the Export dialog. On the
  stress project (Release): the pack is 0.74 of version 1 (349.6 MB against 472.7 MB, before textures are
  block-compressed), opens in 0.06 ms, streams the world in 0.98 to 1.06 times the time of version 1 (the budget
  `PerfGPU.Pack.StreamStressWorldRatio` holds it at 1.1), and exports in 1.45 s with every editor frame from the one
  that starts it under 28 ms. The pack size with block-compressed textures (target 140 MB) is recorded by whichever of
  `texture-cook-v2` and `pack-v2` merges second.
- `render-scene`: a persistent render scene per scene (proxies in structure-of-arrays form, updated only where mesh
  renderers were signaled or transforms changed) in a loose-grid spatial index that the view, the prefetch sphere and
  the shadow cascades query; draws gathered in parallel chunks with a deterministic merge and a radix sort; instancing
  without the material in the batch key (10,000 unique materials: 1 opaque draw); 64-byte instances in a buffer that
  persists, with only changed instances, materials and draw-list ranges uploaded (a still frame uploads nothing);
  asset requests that follow visibility (High above 5% of the screen, Low within the prefetch distance, each asset
  looked up once per frame, no mesh, material or texture a frame resolved, and no asset manager, held once the frame
  is rendered, so what is out of view is evictable also for idle renderers); mesh layouts that follow loads and
  reloads in the render scene itself, whoever renders it, and that evicted meshes keep until a reimport changes their
  source revision (`AssetSourceRevisions`, which tell a reimport from an eviction); gizmos culled to the view with
  icons for small lights and cameras and colliders of the selection only; the outline list cached; requests that move
  an asset in the residency's recency orders without a search.
  `SceneRenderer::Render` costs about 0.9 ms for 100,000 cubes and 2.9 ms for 300,000; edit frames of grids under one
  root take 1.6 and 4.4 ms (the scaling audit measured 42.9 and 113.6 ms with the cubes as roots, which also pays the
  Hierarchy panel's cost of about 14 ms per 100,000 roots), 10,000 point lights 1.6 ms and 10,000 unique materials
  1.9 ms (19.4 and 7.47 in the audit); `GetVisibleMaterials` publishes what mip streaming needs. Meshes whose bounds
  are unknown (never loaded) are requested as if in view, as every mesh was before, so a world's worth of meshes not
  yet seen loads in a burst at its start, until `pack-v2`'s recorded bounds are wired into
  `RenderScene::ResolveUnloadedBounds` (the integrator's step, with a test of a recorded mesh culled before it loads).
  Editor frames of scenes with 100,000 or more root entities are still dominated by the Hierarchy panel drawing every
  root (`editor-authoring`). Two caches besides the resolve cache still hold assets between frames: `SceneRenderer`'s
  environment cache the sky light's environment map until the environment changes (`first-light`), and
  `TextRenderer`'s glyph atlases their fonts while the fonts are loaded, so a font once drawn is not evicted while its
  renderer lives (the text renderer's atlas cache).
- `streaming-observatory`: an always-on frame profiler (`FrameProfiler`, `ST_PROFILE_STAGE`: Input, Assets.Update,
  Assets.Finalize, Scene.Scripts, Scene.Physics, Scene.Audio, Scene.Transforms, Render.Extract, Render.Submit, Editor.UI,
  Present; self times, a 600-frame ring and job-thread busy percentages, about 0.6 microseconds per frame of 16 stages),
  `editor.perf`, `StrataRuntime --perf-log` (a CSV row per frame, which `PerfGPU.Streaming` reads from three runs of the
  exported stress game: the slowest frame's finalization stays within 8 ms by the median run; single runs measured
  4.4-5.6 ms here, and one spike of 12 ms under heavy load from other work), the Streaming panel (budget bars by
  pressure, loads, uploads, a 10,000-asset table built on a job thread: every frame within about 0.15 ms at the 95th
  percentile, the frames that refresh the table included, with `AssetManagerBase::GetStats` no longer visiting every
  asset) and the Performance panel (a stacked stage timeline, about 0.065 ms per frame at the 95th percentile),
  status pills that open them, `editor.showPanel`, and the viewport's streaming and LOD debug views
  (`SceneRenderOptions::EntityTints`, `Overlay/StatusTint.frag`, `LODSelection`; `viewport.capture {debugView}`).
- `editor-authoring`, phase A (the editor core; the panels follow in phase B): an inspection target (entities or one
  asset); material edits that show in the same frame, preview while dragged and undo as one step
  (`AssetDocumentAction`, `AssetManagerBase::ReplaceLoadedAsset`); every importer's settings described as properties and
  validated, and re-imports in the background (`asset.setImportSettings`, `asset.reimport` answer deferred; the longest
  editor frame while a 4096x4096 texture re-imports is about 3 ms, 4 ms with its upload and drawing on a GPU, it was the
  whole import); `HierarchyView`, the
  hierarchy's rows without UI (an unchanged frame costs about 10 ns at 100,000 roots, a rebuild 0.6 ms), with
  `c:`/`t:` filters, reveal and drop zones; one-step commands for many entities (`entity.setParent {entities, index}`,
  `entity.copy`, `entity.paste`, `entity.duplicate {entities}`, `entity.setActive {entities}`, `component.add` and
  `component.remove {entities}`, `component.set {entities}`) and multi-entity property edits. Undo now restores
  sibling orders whose recorded positions agree by chance, and copying many roots is linear.
- `editor-authoring`, phase B (the panels): an Asset tab next to the Inspector (materials edited live, a drag one undo
  step; import settings as a draft applied in the background; texture preview); a Hierarchy that draws only the rows in
  view (about 0.012 ms per frame at 100,000 roots, it was 12.9 ms), with multi-selection (Ctrl, Shift, a box), drops
  before, after and into rows, rename in place, icons, an eye, prefab badges, a search box and reveal of entities
  selected elsewhere; an Inspector that edits every selected entity at once (mixed values, axis labels that reset,
  component menus with Copy and Paste Values, searchable entity and asset pickers, the scene's settings and sky with
  nothing selected, New Material and the material in place); a play-mode banner; and selection shortcuts (Delete, Ctrl+D,
  Ctrl+C/X/V, Ctrl+A, F2) that act only where the keyboard focus is.
- `editor-authoring`, after review: the Inspector shows values and labels whole in a laptop's window (shared resizable
  label column, numbers without trailing zeros, ellipses with tooltips where nothing else fits), colors as channels in
  the axis colors with a picker, mixed colors and texts as mixed, sliders as a track beside the number; Hierarchy ranges
  anchored on entities and, while filtering, taking only the matches; Apply sending only changed import settings and
  out-of-range settings refused; duplicates of several sibling lists remapped together; multi-entity yaw edits past and
  through 90 degrees that keep each entity's pitch and roll (its angles read once per edit), and rotation fields that
  follow an undo; the Asset tab naming types and choices in words, with size limits as choices.

Open items and deferrals later waves own:

- The allowlist's 9 exceptions: the application shell's service wiring (7, `engine-modules`), asset finalization on
  the renderer's command list (`mip-streaming`) and physics reading the renderer's `Mesh` (Mesh v2, later). Ordering
  constraints that name systems a build may lack need a policy once subsystems become optional (`engine-modules`).
- Physics: what an overflowing world needed is remembered per scene (its asset, or the scene and its copies) for the
  process (an editor or game session), not saved with the scene; automatic capacities reserve Jolt's buffers for the
  bodies a scene starts with (70,000 bodies reserve about a million body pairs), and a world at the largest capacities a
  scene may ask for still reserves about 940 MiB at play start; dynamic bodies below other dynamic bodies are still
  written back one by one; the Inspector shows the capacities once `editor-authoring`'s scene section merges.
- A world seen all at once still peaks at about 970 MiB of private memory in the exported runtime: no budget can evict
  what is in view, so this needs BC textures and mip streaming (`texture-cook-v2`, `mip-streaming`; with BC textures the
  stress world peaks at about 520 MB, see wave 2). Mesh uploads still
  go through NVRHI's upload manager until the budgeted upload queue (`mip-streaming`); `AssetManagerBase::GetResidencyInfo`
  and `GetResidencySamples` walk every registered asset (`GetStats` keeps counts instead; eviction walks only the
  resident assets of the pools over budget, least recently requested first), which a world of many registered assets
  will feel (`world-partition`).
- Deleting one entity among 100,000 sibling roots renumbers the sibling list (about 0.5 ms). Starting play copies a
  1,000,000-entity scene in about 0.7 s (`scene-streaming`). The Inspector reads the values of every selected entity each
  frame (to show mixed values): a selection of 100,000 entities adds about 8 ms to a frame while it lasts
  (`Perf.Editor.SelectionFrame100kSelected`); a cache invalidated by component signals and the transforms version would
  make it follow changes instead.
- The editor starts in 0.7-1.0 s (Release, to its first frame on screen), not the 0.4 s the pillar names. An idle
  editor answers input and automation requests on its next throttled frame (up to 33 ms, 100 ms unfocused) until it
  waits on events instead. Project thumbnails and search in the recent projects move to `content-and-prefabs`.
- Perf budgets hold for the reference machine only, so CI's hosted runners skip the `perf` label; a self-hosted runner
  on the reference machine would make them an automatic gate. Platform code first compiled by CI on Linux and macOS:
  `GetProcessMemory`, `GetProcessUptime`, `FindHomeDirectory` and `FindFallbackFontFiles`.
- While `pack-v2` was measured, a Release editor saving a 200,000-entity scene in a loop crashed once (an access
  violation iterating a JSON object) and once refused a save with "Duplicate entity ID"; neither came back in more than
  75 saves since, so the cause is open until someone reproduces it.

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
