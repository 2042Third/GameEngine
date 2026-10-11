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

**Wave 2 landed (2026-10-10).** Seven workstreams merged into one integration branch, in the order perf-gates,
physics-at-scale, texture-cook-v2, pack-v2, render-scene, streaming-observatory, editor-authoring; the integration then
did the wiring the workstreams left to whichever of them merged second. Numbers are Release builds on the reference
machine, quiet:

- `perf-gates`: perf checks that confirm a failure by measuring again (up to three attempts, each printed and recorded)
  and every limit in `StrataTests/Perf/Budgets.json` (an optional roadmap `Target` next to the gating budget), each
  metric's statistic justified in its test, `Tools/PerfStability.py` (spread, pass rate and confirmations per metric,
  machine load per run, `--wait-quiet`), and script dispatch that prefetches instances ahead, so 300,000 trivial scripts
  cost about 6 ms in every code layout (6.6 or 9.8 ms with identical sources before).
- `physics-at-scale`: scene and engine services through which scripts and audio reach physics (the allowlist from 14 to
  9 exceptions); capacities sized from the scene (`SceneSettings::PhysicsCapacity`, automatic by default, bounded, an
  overflow remembered so the next world grows); contacts in per-thread buffers, tracked only for listeners; bodies added
  and removed in batches; poses written back in parallel; time dilation instead of catch-up spirals. 30,000 boxes start
  and play 30 frames in about 245 ms (18.3 s before), 70,000 boxes all get a body (4,465 errors before) and step in
  22 ms frames with Strata's part at about 0.42 of Jolt's, 10,000 bodies are created in one frame in 8.8 ms (24.9 ms).
- `texture-cook-v2`: textures cooked to BC7 (Strata's own encoder, all eight modes; color and multi-channel data),
  BC1 for opaque color at `Fast`, BC4 for one channel (sampled as gray), BC5 for normal maps (Z reconstructed) or
  RGBA16F for HDR, with Mitchell-filtered mips stored smallest first (`STTX` version 2) and a decoding fallback for
  devices without BC. The stress project's 447.4 MB of RGBA8 textures cook to 111.9 MB in a cold import of about 6.9 s
  on 23 workers (BC7: 15 us per block on one thread at `High`), and the all-resident stress sweep peaks at about
  540 MB of private memory (970-987 MB with RGBA8).
- `pack-v2`: asset packs version 2 (256 KiB zstd blocks with CRC-32C checksums, read by byte range through one shared
  file handle, dependency and mesh bounds tables; version 1 still loads), references and bounds recorded at import and
  answered alike by the editor and games, dependency prefetch within the budgets' headroom, group loads
  (`RequestLoadWithDependencies`), packs ordered by first use with pruning (`Export.AlwaysInclude`), patch packs, and an
  export on an I/O thread behind the Export dialog. With BC textures the stress pack is 122.0 MB (pack-v2's
  target: 140 MB; 472.7 MB in version 1 with RGBA8 textures, where version 2 is 0.72 of version 1), opens in 0.04 ms,
  streams the world in 1.02-1.07 times version 1's time and exports in about 0.47 s, the editor's longest frame
  meanwhile typically 4-6 ms.
- `render-scene`: a persistent render scene (proxies updated only where components were signaled or transforms changed,
  in a loose-grid spatial index the view, the prefetch sphere and the shadow cascades query), extraction in parallel
  chunks with a deterministic merge and a radix sort, instancing without the material in the batch key, instances,
  materials and draw lists uploaded only where they changed, asset requests that follow visibility and hold nothing once
  a frame is rendered, mesh layouts that follow loads, evictions and reimports (`AssetSourceRevisions`), gizmos culled
  to the view. `SceneRenderer::Render` costs about 0.88 ms for 100,000 cubes and 2.8 ms for 300,000; editor frames of
  those grids 1.2 and 3.2 ms (42.9 and 113.6 ms in the scaling audit), 10,000 point lights 1.6 ms, 10,000 unique
  materials 1.9 ms in one opaque draw.
- `streaming-observatory`: an always-on frame profiler (`FrameProfiler`, `ST_PROFILE_STAGE`, eleven built-in stages,
  about 0.6 us per frame of 16 stages), `editor.perf`, `StrataRuntime --perf-log`, the Streaming panel (budgets by
  pressure, loads, uploads, a 10,000-asset table built on a job: about 0.16 ms per frame at the 95th percentile) and the
  Performance panel (a stacked stage timeline, about 0.07 ms), status pills that open them, `editor.showPanel`, and the
  viewport's streaming and LOD debug views.
- `editor-authoring`: an inspection target (entities or one asset); material edits that preview while dragged and undo
  as one step; every importer's settings described and validated, re-imported in the background; one-step commands for
  many entities; a Hierarchy that draws only the rows in view (0.013 ms per frame at 100,000 roots, 12.9 ms before) with
  multi-selection, drops, rename in place, filters and reveal; an Inspector that edits every selected entity at once
  (mixed values, axis labels that reset, Copy and Paste Values, pickers, the scene's settings and sky with nothing
  selected); the Asset tab (materials, import settings with a preview); selection shortcuts that act where the focus is.
- Integration: meshes that have not loaded are placed by the bounds the asset pipeline recorded
  (`AssetManagerBase::GetBounds` through `RenderScene::ResolveUnloadedBounds`; a change of them is a content change), so
  a world's meshes not yet seen are culled before they load; texture compression and quality are import settings of the
  Asset tab and `asset.setImportSettings`; the Inspector's scene settings show physics capacities; the default layout
  (version 3) has the Asset tab behind the Inspector and the instruments in the bottom area; imports and exports
  compress on background threads below the main thread's priority, frame work keeps workers at the normal priority, and
  a thread that waits for jobs runs only the job it waits for, so the editor's frames stay at 3-4 ms while a 4096x4096
  texture is compressed to BC7 on every core (10-30 ms before), and a frame never picks up an import's compression
  while it waits for its own work; the editor opens projects at once and imports in the background (a fresh copy of the
  stress project: the scan takes 38 ms instead of 7.6-7.9 s, the editor is on screen after 1.1 s and the 18 textures
  arrive within about 8 s, loads of them waiting for their imports); pack blocks that compress by less than an eighth
  are stored and read straight into place, which brought streaming from version 2 back from 1.08-1.14 to 1.02-1.07
  times version 1's time with BC textures; every perf check of the wave is confirmed, with the workstreams' acceptance
  limits as budgets. `PerfStability.py --runs 5 --wait-quiet 20` on the merged tree after the review round, with
  main's idle-frame budgets back and finalization at 5.44 ms: 4 of 5 runs passed, at most three confirmations per run,
  all of them editor idle frames; the failed run's `PerfGPU.Editor.PlayFrameNested100k` stayed over its budget in all
  three attempts (0.36-0.39 ms against 0.3313). Tight but passing in every run: `PerfGPU.Streaming.MaxFinalizeTime`
  (4.19-5.31 ms against 5.44) and `RuntimePeakPrivateMemory` (632-646 MB against 650). The editor's idle frames were
  then made cheaper than wave 1's: `PerfGPU.Editor`'s command script on the generated nested scenes, interleaved runs,
  medians of 8 rounds, takes 0.18-0.23 ms per frame against 0.23-0.26 ms for wave 1's editor (0.29-0.32 ms merged
  before). ImGui's platform layer is the engine's own (`ImGuiPlatform`, fed by the window's events): the vendored GLFW
  backend spent 0.05-0.06 ms of every frame on multi-viewport support the editor does not use (the window's mouse
  passthrough, the monitors, a hook of the window procedure). The Inspector's scene settings start closed, with
  summaries (open, their rows cost 0.026 ms of a frame), the UI's vertices go straight into mapped buffers, and the
  status pills write their tooltips only while they show. `SceneRenderer::Render` costs what it did in wave 1 (sampled:
  0.06-0.075 ms of an idle frame in both). Applications and the test runner opt out of Windows' execution-speed
  throttling. The idle-frame budgets went down to 0.2808, 0.3 and 0.3133 ms for editing 100,000 and 1,000,000 entities
  and playing 1,000,000 (0.358, 0.3294 and 0.3238 before; playing 100,000 keeps 0.3313), the export's median frame to
  0.6349 ms (0.7674), all at 1.5 times the median of four runs on a machine 38-68% busy with other work; then
  `PerfStability.py --runs 3 --wait-quiet 20` passed 3 of 3 runs, every idle frame on its first attempt (0.171-0.284
  ms). The editor frames of `PerfGPU.Renderer`, which got cheaper by the same 0.05 ms, wait for a quiet campaign.

Open items and deferrals later waves own:

- The allowlist's 9 exceptions: the application shell's service wiring (7, `engine-modules`), asset finalization on
  the renderer's command list (`mip-streaming`) and physics reading the renderer's `Mesh` (Mesh v2, later). Ordering
  constraints that name systems a build may lack need a policy once subsystems become optional (`engine-modules`).
- Physics: what an overflowing world needed is remembered per scene for the process, not saved with the scene;
  automatic capacities reserve Jolt's buffers generously (70,000 bodies reserve about a million body pairs), and a world
  at the largest capacities still reserves about 940 MiB at play start; dynamic bodies below other dynamic bodies are
  still written back one by one; a collider built from a mesh that is reimported while evicted keeps the old data until
  the mesh loads again (the source revisions render-scene uses would tell).
- Textures: `asset.import` still cooks on the main thread (about 1.5 s for a 4096x4096 BC7 color texture; asynchronous
  import is `content-and-prefabs`'); HDR textures stay RGBA16F until a BC6H encoder exists; a BC7 upload step carries
  twice BC1's bytes, and the frame's first step goes whatever the deadline says (later finalizations of a frame past
  its deadline wait for the next frame), so a frame that finalizes a large texture still takes 4.2-5.3 ms in the
  evicting sweep (budget 5.44 ms) until smaller upload steps (`mip-streaming`).
- A world seen all at once still keeps every texture in view resident: no budget evicts what is in view, so the stress
  world peaks at about 540 MB with BC textures until mip streaming (`mip-streaming`). Mesh uploads still go through
  NVRHI's upload manager until the budgeted upload queue (`mip-streaming`). `GetResidencyInfo` and `GetResidencySamples`
  walk every registered asset (`GetStats` keeps counts instead), which a world of many registered assets will feel
  (`world-partition`). Two caches keep assets between frames: `SceneRenderer`'s environment cache (the sky's map) and
  `TextRenderer`'s glyph atlases (their fonts).
- Texture cooking's 650 MB limit for the exported stress game "at 1920x1080": with BC1 color `texture-cook-v2` measured
  543-558 MB rendering 1920x1080 pixels and 658-659 MB in a 1920x1080 window on the 150% reference display (2880x1620
  pixels); with BC7 color the review measured 623-631 MB and 729-751 MB. `PerfGPU.Streaming.RuntimePeakPrivateMemory`
  now gates the game itself at 1920x1080 pixels (its 1280x720 window at 150%, its own budgets, about 73 MB of textures
  resident from the overview camera): 642 MB against 650 MB, little room. Read as a 1920x1080 window at 150%, the
  criterion fails by about 80-100 MB; which reading applies is still to be decided.
- Editor: the Inspector reads every selected entity's values each frame, so 100,000 selected entities add about 8 ms to
  a frame (`Perf.Editor.SelectionFrame100kSelected`) until a cache follows component signals; the color picker clamps
  HDR colors while dragged (the channel fields keep them);
  deleting one entity among 100,000 sibling roots renumbers the sibling list (about 0.5 ms); starting play copies a
  1,000,000-entity scene in about 0.7 s (`scene-streaming`).
- The editor starts in 0.7-1.0 s (Release, to its first frame on screen), not the 0.4 s the pillar names. An idle
  editor answers input and automation requests on its next throttled frame (up to 33 ms, 100 ms unfocused) until it
  waits on events instead. Project thumbnails and search in the recent projects move to `content-and-prefabs`.
- Perf budgets hold for the reference machine only, so CI's hosted runners skip the `perf` label; a self-hosted runner
  on the reference machine would make them an automatic gate. On its hybrid processor where the scheduler places a
  process's main thread lasts for the process's life and shows in what it measures: pinned, an idle editor frame took
  0.25-0.28 ms on a performance core, about 12% more on the other hardware thread of the core the graphics driver's busy
  thread runs on, and 1.3-1.9 times as long on an efficiency core. Applications and the test runner opt out of Windows'
  execution-speed throttling, which moves background windows to the efficiency cores, and `PerfGPU.Editor` confirms a
  metric in a fresh editor process; what remains of the spread between processes (0.15-0.28 ms for one idle frame of
  the merged editor) is the scheduler's placement and the machine's other work.
  Platform code first compiled by CI on Linux and macOS: `GetProcessMemory`, `GetProcessUptime`, `FindHomeDirectory`,
  `FindFallbackFontFiles`, `PosixFileReader` and `SetCurrentThreadPriority`.
- The reference machine, not the engine: once in about 45 perf runs, under load, the editor failed to open
  `PerfGPU.Editor`'s generated million-entity scene ("Invalid JSON"; the test now describes the files on disk when that
  happens), and while `pack-v2` was measured a Release editor saving a 200,000-entity scene in a loop crashed once and
  once refused a save with "Duplicate entity ID". The crash dump shows the main thread reading a `std::map` node at an
  address one bit (bit 34) away from the live node its registers hold, while every other thread waited and every heap
  pointer in the dump lies at the other address; one flipped bit also turns the entity ID that exists into the reported
  duplicate, and the saved scenes are intact. 108 saves alongside exports, background reimports and material saves on
  the merged editor never failed. The machine's i7-13700K ran microcode 0x123 then (BIOS 1658 of May 2024, before Intel's
  0x129 and 0x12B fixes for the 13th and 14th generations' instability) with DDR4-3600 XMP memory, and the compiler
  crashed in `c1xx.dll` and `c2.dll` at changing places more than a dozen times in these days. The BIOS was updated on
  2026-10-10 (version 1836, microcode 0x133), but compilers still crashed afterwards under full load (an MSVC internal
  compiler error, C1001, and a GCC segmentation fault, in files that compiled on the retry): until a CPU and memory
  stress test passes (or XMP is off), one-off crashes and corrupt data on this machine are suspect; the save path's
  validation before writing, which turned the corruption into a refused save, stays.

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
