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

**Wave 2 landed (2026-10-11).** Seven workstreams merged into one integration branch on 2026-10-10, in the order
perf-gates, physics-at-scale, texture-cook-v2, pack-v2, render-scene, streaming-observatory, editor-authoring; the
integration then did the wiring the workstreams left to whichever of them merged second, a review round fixed what its
reviewers found, and three branches fixed what the review's verification still found; the merged tree's verification
then corrected two outcomes recorded here and brought the stress world's loading speed back in code. Numbers are
Release builds on the reference machine, quiet:

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
  texture is compressed to BC7 on every core (10-30 ms before); the editor opens projects at once and imports in the
  background (a fresh copy of the stress project: the scan takes 38 ms instead of 7.6-7.9 s, the editor is on screen
  after 1.1 s and the 18 textures arrive within about 8 s, loads of them waiting for their imports); pack blocks that
  compress by less than an eighth are stored and read straight into place, which brought streaming from version 2 back
  from 1.08-1.14 to 1.02-1.07 times version 1's time with BC textures; every perf check of the wave is confirmed, with
  the workstreams' acceptance limits as budgets.
- The review's verification left three problems, each fixed on a branch of its own and merged on 2026-10-11:
  - `idle-frames`: the merged editor's idle frame cost 0.04-0.06 ms more than wave 1's (0.29-0.32 ms against 0.23-0.26
    ms, `PerfGPU.Editor`'s command script on the generated nested scenes), and main's budgets, restored, failed about
    one perf run in five. That gap is gone. Measured against wave 1's editor in interleaved rounds of the same test
    (medians of 16 rounds at the verification and of 7 later, wave 1 against now), editing 100,000 entities is 0.06 ms
    cheaper (0.252 against 0.192 ms, 0.274 against 0.208), and the other three idle frames cost what wave 1's do within
    the spread between processes: editing 1,000,000 entities 0.220 against 0.216 and 0.252 against 0.206 ms, playing
    100,000 0.237 against 0.244 and 0.244 against 0.246, playing 1,000,000 0.255 against 0.272 and 0.264 against 0.261.
    The playing frames spread most, wave 1's as well (0.18-0.32 ms between processes, and up to 0.1 ms between
    consecutive waits of one process, in later waits as in the first after `play.start`, so an unmeasured warm-up wait
    would not narrow it). ImGui's platform layer is the engine's own (`ImGuiPlatform`, fed by the window's events): the
    vendored GLFW backend spent 0.05-0.06 ms of every frame on multi-viewport support the editor does not use (the
    window's mouse passthrough, the monitors, a hook of the window procedure). The Inspector's scene settings start
    closed, with summaries (open, their rows cost 0.026 ms of a frame), the UI's vertices go straight into mapped
    buffers, the status pills write their tooltips only while they show, and applications and the test runner opt out
    of Windows' execution-speed throttling. `SceneRenderer::Render` costs what it did in wave 1.
  - `import-cancel`: opening projects at once had moved the wait for their imports to closing the project, quitting
    and moving or deleting assets (about 7 s with the stress project's imports running, past Windows' 5 s "not
    responding"). Nothing in the editor waits for a background import any more: imports are cancelled instead
    (closing the project, opening another or a sample and quitting cancel them all; moving, deleting or re-importing an
    asset only its own, which a move queues again at the new place), importers stop at their cancellation checks (the
    texture cooker per block) and a cancelled import stores nothing. With 17 imports of a fresh stress project copy
    running, `project.close` answers in 0.06-0.34 s instead of 5.6-12 s, `asset.move` and `asset.delete` in 0.05-0.19
    s instead of 5.6 s or more, and the editor quits in 0.1-0.2 s instead of 5.8-6.3 s; the cold import costs what it
    did. In a window on the reference display (4 rounds): `project.close` 0.13-0.21 s, its frame 15 ms, the editor's
    quit 0.17-0.20 s.
  - `runtime-memory`: texture cooking's acceptance criterion ("the exported stress game at 1920x1080 over 300 frames,
    with all textures resident, peaks at 650 MB private or less") is now gated as it reads.
    `PerfGPU.Streaming.RuntimePeakPrivateMemoryAllResident` runs the exported stress game in `StrataRuntime` from the
    stress project's `TopDown` scene (the world with its camera looking down on all of it), rendering 1920x1080 pixels
    (`--resolution`, whatever the display's scale; the run checks its swapchain) for 300 frames with the game's own
    budgets; every texture of the pack (111.8 MB) must be resident at the end, none evicted and none loading; budget
    650 MB. The criterion's earlier reading, the overview camera (about two thirds of the textures resident), stays as
    `RuntimePeakPrivateMemory`. With every texture resident the game peaked at 682-690 MB at the review, over the
    limit; the game now keeps what it needs: upload staging is bounded as a whole within 32 MiB, mesh uploads included,
    and reused once an event query says the GPU is done (`StagingPool`; about -50 MB: the host memory of staging held
    through loading went from 136 to 82 MiB), the output texture exists only when a frame composes overlays or text
    (-8 MB), games write no entity-ID buffer (`SceneRendererSpecification::EntityIDs`, -8 MB), meshes release their
    vertex attributes once uploaded (physics keeps positions and indices, -9.5 MB), and ambient occlusion is half
    precision where the device stores r16f images (-8 MB). Halving the staging budget made the world with every
    texture in view load later, though: resident by frame 10-11 instead of 6-7, because staging was reused two frames
    after its release (about 13 instead of 22 MB uploaded per frame).
- The three merged cleanly (one conflict, in this file). The draw sort then got cheaper: `RadixSort` reads the keys once
  and builds histograms and passes only for the bytes that differ (the 100,000 cubes of a grid share one key), which
  took `SceneRenderer::Render` of 100,000 cubes from 0.91 to 0.64 ms and of 300,000 from 2.92 to 2.44 ms (medians of 8
  interleaved rounds) and ended the failures of `PerfGPU.Renderer.Render100kCubes` (1.36-1.40 ms against 1.331 in all
  three attempts in 2 of 10 runs, in processes where the sort took 2.5 times as long after heavy work). The final perf
  campaign, `PerfStability.py --runs 5 --wait-quiet 20` on the merged tree after the reference machine's BIOS update
  (version 1836, microcode 0x133; the canary `Perf.Harness.Sort200k` at 0.97-0.99 of its reference in all 20 runs of
  the round, as before the update, so no budget changed for the machine): 5 of 5 runs passed, 84 metrics each, at most
  two confirmations per run (`ImportSettings4kMaxFrame10kCubes` and `ExportMaxFrameStress`, whose first attempts catch
  the worst frame of a busy moment, and `StreamStressWorldRatio`). Medians of the five: editor idle frames 0.176
  (editing 100,000 entities), 0.165 (1,000,000), 0.239 and 0.242 ms (playing them); the exported game with every
  texture resident 599 MB (594-605, budget 650), from the overview camera 562 MB; the all-resident sweep 503 MB, the
  evicting sweep 404 MB; the longest finalization of the evicting sweep 4.70 ms (4.00-5.07, budget 5.44) and of the
  exported game 4.26 ms; `SceneRenderer::Render` of 100,000 cubes 0.68 ms, an editor frame of them 0.92 ms; the
  stress pack's world streams in 1.07 times version 1's time (1.03-1.09, the acceptance limit is 1.1). Budgets went
  down with the round's measurements (1.5 times the median of quiet runs): the six the sort made cheaper and four
  editor frames the UI work made cheaper.
- The verification of the merged tree found two of these outcomes misstated (the idle frames against wave 1's and the
  stress world's loading, corrected above). The loading came back in code: staging is reused once the GPU has run the
  copies that read it (the asset managers fence their uploads when they execute them, `Renderer::ExecuteUploads`, and
  the pool polls its fences when a step finds no room), and the steps after a frame's first leave one step of the
  budget to the next frame's first, so staging stays within its 32 MiB. With every texture
  in view the world is resident from frame 9 (6-10) instead of 10 (10-15), 33 instead of 80 ms after the first frame
  (medians of 10 interleaved runs each), at the same peak memory (606 against 603 MB); frames that come faster than
  the GPU runs the copies still upload about half the budget each. `PerfStability.py --runs 3 --wait-quiet 20`
  afterwards: 3 of 3 runs passed, 84 metrics each, none needed a confirmation (the canary at 0.95-0.96 of its
  reference); medians: the exported game with every texture resident 595 MB (590-603), the exported game's longest
  finalization 4.14 ms and the evicting sweep's 3.81 ms (3.73-4.22), the stress pack's world 1.045 times version 1's
  time.

Open items and deferrals later waves own:

- The allowlist's 8 exceptions: the application shell's service wiring (7, `engine-modules`) and physics reading the
  renderer's `Mesh` (Mesh v2, later). Ordering
  constraints that name systems a build may lack need a policy once subsystems become optional (`engine-modules`).
- Physics: what an overflowing world needed is remembered per scene for the process, not saved with the scene;
  automatic capacities reserve Jolt's buffers generously (70,000 bodies reserve about a million body pairs), and a world
  at the largest capacities still reserves about 940 MiB at play start; dynamic bodies below other dynamic bodies are
  still written back one by one; a collider built from a mesh that is reimported while evicted keeps the old data until
  the mesh loads again (the source revisions render-scene uses would tell).
- Textures: HDR textures stay RGBA16F until a BC6H encoder exists.
- `GetResidencyInfo` and `GetResidencySamples` walk every registered asset (`GetStats` keeps counts instead), which a world of many
  registered assets will feel (`world-partition`). Two caches keep assets between frames: `SceneRenderer`'s environment
  cache (the sky's map) and `TextRenderer`'s glyph atlases (their fonts). Streaming the stress world from pack version 2
  takes 1.03-1.09 times version 1's time (zstd blocks and their checksums), close to the 1.1 acceptance limit, which
  needs a confirmation in about one run in five (range reads of `mip-streaming`).
- Imports (`content-and-prefabs`, dependency-aware asynchronous import): a few long steps of an import cannot be
  interrupted (stb_image decoding a large PNG, the resize to `MaxSize`, one mip level's filtering, meshoptimizer
  simplifying one primitive); they never hold a frame, but delay when a cancelled import frees its thread. Moving or
  deleting a file that another asset's import reads as a dependency does not cancel that import (the file watcher's
  change event replaces it). A project whose large scenes have no reference records yet spends seconds of its scan in
  the reference search (4 s for `PerfGPU.Editor`'s 1,000,000-entity scene).
- Where the exported stress game's memory goes with every texture resident (MiB of private bytes at 1920x1080 pixels,
  from temporary stage logging): the process, its job threads and window 10; the NVIDIA driver's Vulkan device 59 and
  its first swapchain 93 (whatever its size: the driver presents through a layered DXGI swapchain, with a D3D12 device
  of its own); the renderer's shared resources 3; audio, the pack's index and the scene 14; the scene renderer's compute
  pipelines and resources 34 (mostly the driver's first shader compilation); render targets 58 (HDR 16, LDR, normals
  and depth 8 each, ambient occlusion 8, bloom 5); the shadow cascades 66 (4 x 2048x2048, 32-bit depth); the first
  frame's draw pipelines 12 on the GPU; then the textures 106.7 and the terrain's buffers 23.5 on the GPU and the
  assets' CPU data 17 (the terrain's positions and indices for physics, the scene); the rest, about 60, is what the
  driver keeps of loading's transient memory once freed (staging's host memory peaked 35 above its start) and of the
  first frames' pipeline compilation. In all about 556 MiB after loading, 563 MiB (590 MB) at the peak. An empty scene
  at 1920x1080 takes 282 MiB (295 MB), with the sun's shadow cascades 349 MiB (366 MB), the stress scene without
  textures 442 MiB (464 MB). Mip streaming (`mip-streaming`, wave 3) keeps only the levels the screen needs of the
  106.7 MiB of textures and uploads in smaller steps: with the whole world in view (the `TopDown` scene at 1920x1080) the
  game holds 0.02 MiB of textures, its device 268 MiB and staging at most 24.6 MiB, and peaks at 448 MiB (470 MB;
  454-470 MB over that stream's runs, against 595-606 MB with every texture resident; 536-542 MB at 2880x1620). What is
  left to win belongs to later renderer work: shadow cascades sized to what a view needs (fewer cascades or 16-bit depth
  for far ones, with the depth bias retuned) would halve the 66 MiB, and the render targets take 58 MiB. Overlays that
  hook every windowed Vulkan process count too: RivaTuner Statistics Server, when it
  runs, adds about 55 MiB to every game and editor (its hook library allocates it as the window opens), which takes the
  stress game to about the 650 MB limit; the gates' measurements must be taken without it.
- Editor (later editor work, not planned in a wave yet): the Inspector reads every selected entity's values each frame,
  so 100,000 selected entities add about 8 ms to a frame (`Perf.Editor.SelectionFrame100kSelected`) until a cache
  follows component signals; the color picker clamps HDR colors while dragged (the channel fields keep them); deleting
  one entity among 100,000 sibling roots renumbers the sibling list (about 0.5 ms).
- The editor starts in 0.7-1.0 s (Release, to its first frame on screen), not the 0.4 s the pillar names. An idle
  editor answers input and automation requests on its next throttled frame (up to 33 ms, 100 ms unfocused) until it
  waits on events instead (later editor work).
- Perf budgets hold for the reference machine only, so CI's hosted runners skip the `perf` label; a self-hosted runner
  on the reference machine would make them an automatic gate. On its hybrid processor where the scheduler places a
  process's main thread lasts for the process's life and shows in what it measures: pinned, an idle editor frame took
  0.25-0.28 ms on a performance core, about 12% more on the other hardware thread of the core the graphics driver's busy
  thread runs on, and 1.3-1.9 times as long on an efficiency core. Applications and the test runner opt out of Windows'
  execution-speed throttling, which moves background windows to the efficiency cores, and `PerfGPU.Editor` confirms a
  metric in a fresh editor process; what remains of the spread between processes (0.16-0.23 ms for one idle frame of the
  merged editor in the final campaign) is the scheduler's placement and the machine's other work. Right after heavy work
  some processes ran single-threaded work up to 2.5 times as long for their whole life (the draw sort above: 0.77
  instead of 0.31 ms; not page faults, which a probe counted in such a process, and slower than the whole process pinned
  to the efficiency cores, 0.53 ms; the cause is open); `ImportSettings4kMaxFrame10kCubes` and `ExportMaxFrameStress`,
  maxima by nature, need a confirmation in about one run in three or four. Platform code first compiled by CI on Linux
  and macOS, or run only on Windows so far: `GetProcessMemory`, `GetProcessUptime`, `FindHomeDirectory`,
  `FindFallbackFontFiles`, `PosixFileReader`, `SetCurrentThreadPriority`, the window calls of `ImGuiPlatform` (layout
  keys, cursor shapes, the clipboard) and the Retina fix-up of `StrataRuntime --resolution`.
- The reference machine, not the engine: once in about 45 perf runs, under load, the editor failed to open
  `PerfGPU.Editor`'s generated million-entity scene ("Invalid JSON"; the test now describes the files on disk when that
  happens), and while `pack-v2` was measured a Release editor saving a 200,000-entity scene in a loop crashed once and
  once refused a save with "Duplicate entity ID". The crash dump shows the main thread reading a `std::map` node at an
  address one bit (bit 34) away from the live node its registers hold, while every other thread waited and every heap
  pointer in the dump lies at the other address; one flipped bit also turns the entity ID that exists into the reported
  duplicate, and the saved scenes are intact. 108 saves alongside exports, background reimports and material saves on
  the merged editor never failed. The machine's i7-13700K ran microcode 0x123 then (BIOS 1658 of May 2024, before
  Intel's 0x129 and 0x12B fixes for the 13th and 14th generations' instability) with DDR4-3600 XMP memory, and the
  compiler crashed in `c1xx.dll` and `c2.dll` at changing places more than a dozen times in these days. The BIOS was
  updated on 2026-10-10 (version 1836, microcode 0x133), but compilers still crashed afterwards under full load (an MSVC
  internal compiler error, C1001, and a GCC segmentation fault, in files that compiled on the retry): until a CPU and
  memory stress test passes (or XMP is off), one-off crashes and corrupt data on this machine are suspect; the save
  path's validation before writing, which turned the corruption into a refused save, stays. The integration's last round
  on the updated machine (2026-10-11: Debug and Release builds, 402 GCC translation units, full test runs in both
  configurations, 20 perf runs) saw no crash; the round after it did. The compiler crashed again (`c2.dll`, an access
  violation, in a file that compiled on the retry), and within four minutes three processes failed around
  `PerfGPU.Editor`'s million-entity scene: wave 1's Release editor opening it (an access violation), the test runner
  writing it (an access violation) and the merged editor opening it (no progress for 10 minutes, the test's timeout).
  The 55 runs of the same steps that followed (40 driving the editor directly, 15 through the test, some under a full
  Debug test run) all passed, and Windows logged no hardware error.

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
