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

**Wave 3 landed (2026-10-12).** Six workstreams merged into one integration branch on 2026-10-12, in the order
mip-streaming, scene-streaming, animation, content-and-prefabs, gameplay-and-scripting, agent-activity (animation and
gameplay-and-scripting came from the product audit after wave 2, which found whole systems a 3D engine needs that no
workstream covered); the integration merged the conflicting files as unions of both sides, did the wiring the
workstreams left to whichever of them merged second and closed the review findings their rechecks had left open. Numbers
are Release builds on the reference machine, measured by the workstreams while up to six of them shared it, mostly as
interleaved A/Bs against wave 2's build; the quiet campaign that decided the budgets comes last (Perf campaign):

- `mip-streaming`: the asset layer knows no graphics API (uploads go into an `AssetUploadContext` the renderer provides;
  the allowlist from 9 to 8 exceptions); every upload goes through one budgeted queue (`GPUUploadQueue` on the staging
  pool: 16 MiB and 2 ms of upload steps per frame, steps planned to the measured speed with a tenth of the time in
  reserve, so a frame's steps take 1.78-1.82 ms of the 2 ms instead of 1.99-2.07; an optional transfer queue, off by
  default since it loaded no faster), and the swapchain's acquire wait moved to `PrepareBackBuffer`, so uploads no
  longer queue behind the display; textures load as their mip tail (the levels up to 32x32: 1.4 KiB of a 2048x2048 BC7
  texture), and each manager's `TextureStreamer` raises and drops their levels to the texel density the views draw them
  at, the most needed first under a texture budget (a Mip column in the Streaming panel, a Mip debug view, `asset.stats`
  and `--perf-log` columns). The exported stress game with the whole world in view at 1920x1080 peaks at 454-470 MB of
  private memory (median 466; 595-606 MB with every texture whole; 536-542 against 671 MB at 2880x1620), and its world
  is complete after 5.5 frames (3-9; 10, 7-18, before); the mip sweep over the textured ground of the stress project's
  `Flight` scene peaks at about 437 MB within a 96 MB texture budget, its last frame 67 dB PSNR from the whole-texture
  baseline's.
- `scene-streaming`: scene and prefab documents are read once, on the loading worker, and prefabs compile into
  prototypes: a 1,000-entity prefab instantiates in 0.25 ms (about 17 ms before), 1,000 instances of a 10-entity prefab
  in one `InstantiateMany` in 3.1 ms; scenes load in the background (`SceneLoader`, in the editor and in games, with
  progress): the editor's longest frame while `scene.open` loads 100,000 entities is 2.0-2.6 ms (one frame of 821-887 ms
  before) and 18-21 ms for 1,000,000, opening 100,000 over the million takes a 0.9 ms frame (70 ms before: the replaced
  scene is destroyed off the frame), and the million open takes 542-544 MB of working set (2.45 GB before); play starts
  by copying the scene storage by storage on the job threads (a million entities in 43-51 ms, the frame that starts
  playing them 45-53 ms, about 700 ms before); a project whose start scene holds a million entities shows its first
  frame after 1.0 s instead of 6.8 s; additive sub-scenes come in slices (5,000 entities per frame by default) and
  unload in one step, from scripts (six host functions) and commands; exported games can play a loading scene while the
  start scene loads (manifest version 4), and switch to a 100,000-entity scene with a longest frame of 0.06-0.14 ms.
- `animation`: skeletons, animation clips and controllers (`.stanimator`) as assets, glTF skins and animations imported,
  GPU skinning with posed bounds in every pass (poses are never entities, and unskinned scenes pay nothing), and the
  "Animation" scene system: states, cross-fades, events, root motion, joint attachments and culling of what no view
  shows, evaluated on the job threads with the same results on any number of them; twelve host functions and
  `OnAnimationEvent`; `animation.*` commands, controller tables in the Asset tab, pose previews in edit mode and the
  live state in the Inspector. 1,000 animators of 60 joints, each cross-fading two clips, take 0.39-0.41 ms a frame
  (acceptance limit 2 ms), 10,000 animators no view shows 0.13-0.15 ms (limit 0.5), and `SceneRenderer::Render` of 1,000
  skinned characters of 5,152 vertices, posed anew every frame, 0.75 ms.
- `content-and-prefabs`: asset deletes refuse while anything uses the asset (unsaved scenes included) or move it to the
  project's trash as one undo step that keeps its handle, with undoable moves and folders and `asset.findReferences`
  (0.11 ms among 10,000 assets); `asset.import` never imports in a frame (the editor's longest frame while a 4096x4096
  texture comes in: 3.3-4.1 ms, 2,380 ms before), brings a glTF's buffers and images along, imports folders and
  meshopt-compressed models and rolls failed items back; Content Browser v2 (folder tree, grid and list, filters, drag
  and drop, reveal; a frame of 10,001 assets costs 0.03-0.07 ms at the 95th percentile, 0.036-0.041 ms with thumbnails);
  thumbnails (textures from their mip tail by byte range, never loaded whole; materials, meshes, models and prefabs
  rendered offscreen one at a time; cached as PNG files; the longest frame while a stress project's are made 2.7-4.1 ms)
  and previews in the Asset tab (an audio clip's waveform and a preview voice); linked prefab instances (status, apply,
  revert, unpack, changes reaching unmodified instances, also of scenes that were closed); new content resting on the
  surface under the view's pivot or the cursor, drop to ground; project pictures and a search on the launcher.
- `gameplay-and-scripting`: the `basic3d` ground has a collider; character controllers on Jolt's `CharacterVirtual`
  (steps, slopes, moving ground, pushing bodies, the same results on any number of threads: 1,000 walking controllers
  take 0.79-0.86 ms a step); named collision layers with a matrix in the scene's settings, the Inspector and scripts;
  gamepads, cursor modes, the game's window mode, resolution and vsync, and atomic save files for scripts (22 host
  functions, 119 in all); scripts made, listed, opened in the person's code editor and built from the editor (New
  Script, External Editor, `CMakePresets.json` for IDEs, a per-user build directory for projects too deep for MSBuild),
  Play building stale scripts first, a Problems panel for build errors and crashes, crashes located by file and line
  (inlined calls too) and a people's guide, `Docs/Manual/Scripting.md`; physics worlds keep an anchor body in each broad
  phase tree, which ends Jolt's "Out of nodes" abort of worlds that add and remove bodies between steps.
- `agent-activity`: no silent data loss (the seven commands that replace the edited scene refuse its unsaved changes
  without `force`, the UI asks first, and what is discarded stays recoverable); autosave and recovery (the unsaved
  changes journaled 2 s after edits stop, at least every 10 s: a frame asking for the write of an edit of 10,000 of
  100,000 entities costs 0.04 ms); an activity journal of every command (about 1.5 us each), the Activity panel and
  replays of a session as a command script with bindings (a 51-command agent session rebuilds the same scene and assets
  on a copy); agents present in the status bar and the viewport; toasts; captures from a camera of their own that wait
  for what they show; out-of-range values refused; copies named uniquely and undo steps named after what they changed;
  `--help`, `--version` and exit code 3 for unknown arguments.
- Integration: the skin stream uploads through the upload queue; root motion walks character controllers; animators and
  controllers play in sub-scenes and in `InstantiateMany` instances; scene loads estimate a texture by its mip tail; a
  headless editor's captures keep their sky (`TextureStreamer::Hold`) and wait for the mip levels they show, and so do
  thumbnails; prefab instances catch up in the frame `scene.open` swaps the scene in; the Content Browser's Scripts
  root, skinned models' thumbnails in their bind pose, animation assets as drag sources and their filter chip; toasts
  for imports that failed unobserved, for assets agents moved to the trash and for script builds and crashes (Show
  Problems); a test pins the journal kind of all 117 commands; the default layout (version 4) docks the Problems and
  Activity panels; status pills fit a 1600x900 window; the Export dialog picks the loading scene, the Asset tab counts a
  model's skeletons and clips, and the Activity panel's undo step names and the controller tables' headers show whole
  (the visual pass at 150%, 100% and in a 1600x900 window). The script ABI keeps version 1 with its host functions in
  merge order (wave 2's 79, then 6, 12 and 22). Full Debug ctest under the validation layer (47 tests) and Release ctest
  without the perf label (47) pass.
- Perf campaign (2026-10-12, the merged tree at 410468a, Release, nothing else running, the memory at its rated 3600
  MT/s, at which `Budgets.json`'s reference values were measured): `PerfStability.py --runs 5 --wait-quiet 20`, then
  five runs more with the budgets it set: 6 of 10 runs passed, 110 metrics each, with the canary `Perf.Harness.Sort200k`
  at 0.98-0.99 of its reference and the machine 29-32% busy (the perf tests alone) in every run; `StrataTests.Perf`
  passed all ten runs, `StrataTests.PerfGPU` six. Three failures were `PerfGPU.Editor.ImportSettings4kMaxFrame10kCubes`
  (over its 4.819 ms in all three attempts, with frames of 6-27 ms; it needed a confirmation in 7 of the 10 runs): its
  long frames wait in the graphics driver's `vkBeginCommandBuffer` (NVRHI's `CommandList::open`, which resets a recycled
  command buffer: 2-21 ms, timed with temporary timers) while the BC7 compression keeps every background thread busy,
  and main's build (a007c1b) exceeds the same budget on this machine too (first attempts of 2.4-8.0 ms, run alone in
  fresh processes, one run passing only in its third attempt), so the budget stays and the cause is open. The fourth was
  `PerfGPU.Editor.ThumbnailsMaxFrame` under the budget the first five runs set (1.149 ms): in 2 of 18 runs (the ten and
  eight of the case alone) every attempt's longest frame took 1.2-2.0 ms instead of 0.5-1.0, and the attempts after the
  first made the thumbnails in 43-45 frames instead of 59-60.
- Medians of the ten runs (wave 2's final campaign in parentheses): the exported stress game with the whole world in
  view 452 MB (449-457; 595 MB with every texture resident), now budgeted at mip streaming's 550 MB limit, from the
  overview camera 452 MB (562), the all-resident sweep 483 MB (503), the evicting sweep 400 MB (404), mip streaming's
  sweep 430 MB; the longest finalization of the evicting sweep 2.89 ms (3.81) and of the exported game 3.21 ms (4.14),
  now in its first frame, which finalizes within the frame's budgets what the start scene's load read while the window
  opened (in mip-streaming's own build the slowest was one of frames 8-11, at 1.80-1.97 ms); idle editor frames 0.206 ms
  editing 100,000 nested entities (0.176), 0.200 editing 1,000,000 (0.165), 0.194 and 0.198 playing them (0.239, 0.242);
  `SceneRenderer::Render` of 100,000 cubes 0.69 ms (0.68), of 300,000 2.43, of 10,000 unique materials 1.37, of 1,000
  skinned characters 0.64, and an editor frame of 100,000 cubes 0.87 ms (0.92); 1,000 animators 0.37 ms and 10,000
  culled ones 0.14; a step of 1,000 character controllers 0.81 ms; opening 100,000 entities a longest frame of 1.91 ms,
  1,000,000 13.7 ms and 543 MB of working set, the 100,000 again over them 0.68 ms, the frame that starts playing the
  million 48.1 ms (42-53, so the 50 ms target stays); the stress pack's world in 1.046 times version 1's time (1.045);
  `Perf.Physics.Create10kBodies` 7.97 ms, without the broad phase's abort. The 27 entries the workstreams set on the
  shared machine and the 5 they lowered took the first five runs' medians as Measured, with budgets at most 1.5 times
  them (`RuntimePeakPrivateMemoryAllInView` went to its 550 MB limit and lost its target), and `PlayFrameNested100k` was
  lowered to 0.1952/0.2928 ms.
- Against main's build, in three interleaved pairs of the whole perf label run directly (first attempts): slower are
  `EntityCreate100kRoots` 1.21x (0.0042 to 0.0051 ms: the activity journal's 1.5 us a command), the Streaming panel's
  95th percentile frame 1.11x and its refresh frames 1.07x (the Mip column), `IdlePlayFrame1M` 1.07x (a fourth scene
  system), `TrivialUpdate300k` 1.03x and `TrivialUpdate100k` 1.01x (the 4-6% measured by a workstream did not reproduce)
  and `MoveLeaf100k` 1.02x (not 9%); faster are the streaming finalizations (0.61-0.73x), the exported game's peak
  memory (0.80x), `SelectionFrame100kSelected` (0.87x) and `Render10kMaterials` (0.90x). The editor's idle frames came
  out 1.18-1.23x when editing and 0.88-0.92x when playing in the perf test's runs; `editor.perf` profiles of interleaved
  runs on a 100,000-entity scene put the difference in the UI stage, 0.11 against 0.09-0.10 ms a frame (the wave's
  pills, chips and panels).
- Review round (2026-10-12, after the campaign): five reviews of the merged tree (fidelity to the workstreams' branches,
  the interactions between the workstreams, the product as a person meets it, an agent's use, perf) found 25 problems,
  which 21 commits addressed, with tests. The largest: a headless editor started with `--project` on a fresh copy of a
  project waited out a two-minute timeout for imports no frame applied and then served an empty scene, so `StrataCLI
  launch` and the MCP server gave up on every fresh copy after 60 s (the wait before the first frame now applies
  finished imports itself, `EditorAssetManager::WaitForImportProgress`); `scene.recover` opened its scene in one frame
  (2.5 s for 300,000 entities) and ignored opens in progress (it now opens in the background like `scene.open`), and the
  toast of an agent's or a command script's forced `scene.open` blamed the person's Don't Save; `WhenVisible` animators
  showed in captures the pose of the capture before, also while paused (a renderer learns that it drew a stale pose,
  `ISkinningPoses::ReportVisible`, and paused scenes pose such animators, `SceneSystem::OnPausedUpdate`). Four were
  costs that grew with content: the prefab catch-up of a scene of 20,000 instances held the frame that swapped it in for
  about 300 ms (each prefab part's version is now worked out once: 4.6-4.9 ms); with 3,000 streamed textures in view an
  idle editor frame took 1.57 ms against main's 0.93, the texture streamer looking at every texture each frame (now only
  at those whose levels may change: 1.22-1.25 ms, `Assets.Finalize` from 0.39 to 0.012 ms; the rest is mip streaming's
  texel density per draw); the frame that swapped in a scene of 100,000 mesh renderers built all their render proxies
  (51-52 ms; a load now builds them over frames before the swap, `RenderScene::PrepareLoadedScene`: 15.5-17.0 ms); and
  the recovery journal worked out its text on the I/O threads that asset loads share (46 ms a write; now on a background
  worker, leaving 5.5 ms of writing there). The smaller ones: New Script refused "Player" and "Spinner" (names in the
  SDK's comments), the start scene could be set as the exported game's loading scene, the project's picture showed
  textures at their mip tails and the HUD oversized, material texture maps took no paths, `animation.preview` refused
  unknown states only after its deferral, the prefab catch-up's hashes showed in the Inspector, the trash toast promised
  an Undo that the next scene drops, three dialogs put their primary button first (two of them ignored Escape), F framed
  characters around their feet, nothing in the viewport named pose previews, `ImportSettings4kMaxFrame` timed windows
  that held no upload of the whole texture, and `TextureStreamer::GetPreview` was documented but unused (removed). Four
  metrics came with the fixes, set from their locked A/B runs: `SceneOpen10kPrefabInstancesMaxFrame` 4.96 ms (budget
  7.44), `IdleFrame3kStreamedTextures` 1.017 (1.525), `SceneOpen100kCubesMaxFrame` 20.9 (31.35) and
  `RecoveryWriteIOTime100k` 5.51 (8.26).
- Final verification (2026-10-12, the review round's tree with corrections of its documentation; Release, nothing else
  running, the memory at 3600 MT/s): `ctest --preset windows-debug` under the validation layer passed all 47 tests and
  `ctest --preset windows-release` with the perf label 49 of 50, `StrataTests.PerfGPU` failing on
  `SceneOpenOver1MMaxFrame` (3.92, 3.07 and 3.32 ms against 1.143); `PerfStability.py --runs 3 --wait-quiet 20` then
  passed 2 of 3 runs (114 metrics each, the canary at 0.98 of its reference), the other failing on the same metric
  (1.44, 3.43 and 3.25 ms). A build of the campaign's tree (883bb2c) shows that metric's slow mode at least as often as
  the tip in interleaved runs of its test case, so the review round did not cause it and its budget stays (see the open
  items' perf to watch). Every other metric passed in all four runs, after confirmations of the import-settings and
  export maxima (first attempts of up to 23.5 ms, the known stalls), `RuntimeMaxFinalizeStage` and `Mips.MaxFrameRatio`;
  the review round's four metrics had medians of 3.47 ms (`SceneOpen10kPrefabInstancesMaxFrame`), 1.05
  (`IdleFrame3kStreamedTextures`), 21.0 (`SceneOpen100kCubesMaxFrame`) and 5.36 (`RecoveryWriteIOTime100k`). The GCC
  lint of 506 translation units, the brace check and the compat check found nothing.

**Wave 4 landed (2026-10-19).** Step 0 landed on main first, on 2026-10-13: wave 3's finishing round, release 0.3.0, the
Dist CI job, the perf lock, the CI-compiler lint and the CI status tool, provisional perf budgets, New Script's missing
class fixed and the `Scene::GetAssetManager` seam. The six streams branched from it, and each merged into main when it
was ready, in the order engine-modules (2026-10-18), game-ui, authoring-and-playtest, world-io, gpu-driven-rendering,
world-partition (2026-10-19); the plan's order put world-io, world-partition and gpu-driven-rendering before game-ui and
authoring-and-playtest, which were ready first. Each merge did its wiring to the streams merged before it, after an
independent review whose findings the stream fixed or answered (17, 18, 15, 16, 13 and 12, and 3, 3, 3 and 7 more that
the last four landings found), and only once CI was green on all six jobs (Windows Release with a compiler cache,
Windows Debug, the Windows Dist build with the package tests, Linux, Linux without audio and physics, macOS). The
Windows reference machine went away on 2026-10-13 and the work moved to a Mac (Apple M2 Max, 12 threads, MoltenVK
1.4.2); a usage limit paused the streams from 2026-10-14 to 2026-10-18. Numbers are Release builds on that Mac, measured
while other streams built and tested on it, mostly as interleaved A/Bs under the perf lock against step 0's build. Every
metric the wave added is provisional and there was no perf campaign: the open items' "Waiting for the Windows reference
machine" lists what waits for it.

- `engine-modules`: subsystems start and stop as `EngineModule`s in dependency order (graphics, audio, ImGui, the
  project), so `Application` wires none of them and the layering allowlist went from 8 exceptions to 0; each layer of
  `Layers.json` is a static library of its own that links only what the table allows (`Architecture.Libraries` reads
  the real links); StrataRuntime links no tooling (no ImGui, ImGuizmo, asset pipeline or network layer:
  `StrataRuntime.NoTooling`; the Release runtime 7.3% smaller); every module registers its own components and orders
  its systems with soft constraints, so physics, audio and animation can be left out of a build (`STRATA_WITH_*`, CI's
  "Linux (no Audio, no Physics)" job); precompiled headers and lighter headers, a sharded Core CTest and a fuzz run of
  the sweeps (`StrataTests.Fuzz`), a compiler cache and a Windows Debug job in CI. On the Mac a clean Debug build takes
  13% less CPU time (1,036 against 1,194 CPU-s) and an edit of `Scene/Components.h` 28% less, Debug ctest 94-163 s with
  `-j 8` instead of 500-804 s serially, and a headless game 18 threads instead of 24 (windowed 29 instead of 36); warm
  CI builds take 2-6 minutes instead of 13-22.
- `game-ui`: the GameUI module: canvases in screen and world space (reference resolution, scale modes), rects with
  anchors and pivots, images (rounded, sliced, filled), text shaped from font faces with shared glyph atlases, buttons,
  toggles, sliders, input fields, layout groups and masks, drawn in one batched overlay pass; hit tests, focus, keyboard
  and gamepad navigation and keyboard capture for typed text; 18 host functions, the callbacks `OnUIClick`,
  `OnUIValueChanged`, `OnUIFocusChanged` and `OnUISubmit`, the SDK's `UI.h` and `Motion`, and unscaled time for pause
  menus; `ui.*` commands and `input.click`/`input.type`; UI presets in the Hierarchy, the UI Rect drawer, the viewport's
  2D canvas mode and UI picking; the Tetris HUD on a canvas; a people's guide, `Docs/Manual/GameUI.md`. On the Mac
  10,000 elements are laid out in 0.30 ms and drawn in 0.21 ms (target 1 ms), and an idle frame of them takes 0.0003 ms.
- `authoring-and-playtest`: skinned characters in their rest pose in edit mode, thumbnails and previews (or at the first
  frame of what they play, `ViewportSettings::EditPose`), poses shared and computed again only when their assets change;
  a model's import scale baked and reaching the instances already placed, normal maps recognized by name, with a
  material warning and its fix; `play.step` answering after its steps, input counted in fixed steps (the same samples
  at 1, 16.7 and 50 ms frames) and `input.sequence`, world transforms and bounds in queries, the running scene named;
  MCP toolsets (the default list 32 tools and 18,634 bytes, against 124 tools and 73,093 bytes for every command) and a
  compact `editor.status` within 1.5 KiB; the Game view beside the Scene view, with fixed resolutions and aspect
  ratios; per-property prefab overrides that every other prefab change reaches, with the prefab bar and marks in the
  Inspector and apply and revert per property; `editor.batch` (atomic, one undo step) and `prefab.instantiate` of many
  instances in one step; Retina scaling, File > Open Scene, disabled properties with their reasons and other polish;
  people's guides, `Docs/Manual/Prefabs.md` and `Docs/Manual/GameView.md`. On the Mac a drag inside a 5,000-entity
  prefab instance costs 0.49 ms a frame (24.5 ms before the review round's fix, which compares only the entities an
  edit touched), and applying a prefab to 10,000 instances 702 ms.
- `world-io`: saves off the frame (`DocumentSaver` writes snapshots of one or several documents in the background with
  undo save points, through a streamed scene writer; `scene.save`, closing and quitting answer once written; moves and
  deletes of documents being saved are refused); streamed, cancellable scene reads with parallel parsing; cooked binary
  scenes and prefabs in exported packs, loaded straight into prototypes on workers; asset registration at scale (an
  append-only import record store, first scans in background slices, projects and automation available while the scan
  runs); imports admitted by their memory estimates within a budget (a quarter of physical memory, 1-4 GiB). On the
  Mac: saving 1,000,000 entities takes 2.4-6.1 s with the editor's longest frame 18-53 ms (the audit measured a 22 s
  freeze on the reference machine); opening them 810 MB of working set instead of 2,047 MB and a longest frame of
  15.7 ms instead of 25.0; an exported million-entity world starts in 526 ms cooked (2,201 ms from JSON) with 97 MB of
  transient memory; a 30,000-asset project answers `editor.status` 1.48 s after its launch; an import of 600 files
  holds no frame over 4.7 ms (188 ms before); 24 textures of 4096x4096 import within 572 MB of peak private memory,
  against 1,556 MB without admission. Pack version 3, cold reads and the background-thread cap moved to wave 5.
- `gpu-driven-rendering`: GPU timer queries per frame stage (the Performance panel, `editor.perf`, `--perf-log`,
  GPU-time metrics), with the display's wait at the back buffer's first use counted as `Present`, outside the work time;
  shadow casters found in parallel, without sub-texel ones; one geometry pool drawn with indirect multi-draw buckets;
  the scene mirrored on the GPU (instances, meshes, materials, a stable texture table), culled by frustum, size and
  two-phase depth-pyramid occlusion for the view and each shadow cascade and drawn with indirect draws, skinned
  instances included; asset requests and mip streaming driven by the GPU's visibility feedback; cascades fitted on the
  GPU and kept while nothing that reaches them changes; camera-relative rendering; render scenes cloned at play start;
  lights as render-scene records culled on the GPU; mip levels kept until idle for 30 s and faded in; levels of detail
  simplified with their attributes (cooked meshes version 3) and chosen by their projected error, with dithered
  cross-fades. On the Mac, against step 0's build: `SceneRenderer::Render` of 100,000 cubes 0.50 ms instead of 1.98, of
  300,000 0.54 instead of 4.98, of 1,000,000 0.60 ms with the GPU busy 12.4 ms; 10,000 unique meshes in 4 draw commands
  (4.83 ms, MoltenVK turning each indirect draw into a Metal draw); 10,000 point lights in view all drawn (the CPU path
  keeps 4,096); playing 1,000,000 cubes copies their render scene in 22-27 ms where the first render took 329 ms; the
  same image at 0, 20 and 100 km from the origin; an idle editor's extraction 0.018 ms instead of 0.349. Compute
  skinning and copy-on-write render scenes are later work.
- `world-partition`: physics at world scale (bodies that wait for room queued, growth between steps by migration,
  parallel body creation, constant-time record lookups, the physics capacity as a scene service, `physics.stats` with
  a Streaming panel section and a pill); sub-scenes unloaded in budgeted slices, with their physics prepared before they
  run; the World module: World assets of layers cut into grid cells, `WorldStreamer` and the World system streaming
  cells around streaming sources in games and in the editor, and `world.cook` writing each cell's bounds, bodies and
  asset closure and sizing the persistent scene's physics; worlds edited in the editor (`world.create`, `edit`,
  `moveEntities`, `loadCells`, `setLayers`, `status`; the cell map, the Hierarchy's strip with its `l:` and `cell:`
  terms, cell bounds in the viewport) and played there; the scripts' World API (5 host functions) and coordinates
  checked 20 km from the origin; a people's guide, `Docs/Manual/Worlds.md`. On the Mac the longest frame of unloading a
  25,000-entity cell is 2.6-2.7 ms (target 4; the audit measured 51-56 ms freeing it whole on the reference machine),
  of driving through a world 2.65-5.5 ms (target 8), the exported world's run through 36 cells leaves none without
  collision, and `Perf.Physics.Create10kBodies` takes 0.63 times step 0's time.
- Landing: game-ui landed before the streams that build on it, so world-partition appended its host functions after
  game-ui's and gpu-driven-rendering took the UI pass into its renderer. The wiring: `input.click` in fixed steps and
  inside `input.sequence`, camera-less UI scenes in the Game view, the UI's inactive reasons and the game UI's commands
  in the toolsets (authoring-and-playtest); `MayDefer` on the saving commands, saves and the scan in the compact
  `editor.status`, prefab overrides and game UI through cooked scenes (world-io); the UI pass on the new renderer with
  its `GPU.UI` stage and camera-relative world canvases, texture derivatives before discards in the scene shaders
  (`Architecture.Shaders`), rest poses on the GPU-culled path, no context copiers for save snapshots and the Game view's
  frame in `editor.perf` (gpu-driven-rendering); world saves through the `DocumentSaver` with streamed cell reads,
  cooked cells and their physics capacity in exports, the runtime perf log's Render columns, cell loads within the
  instance upload budget, render-scene clones at a world's play start, and worlds in `play.step`, `input.sequence` and
  the Game view (world-partition). AGENTS.md's details moved into the developer guide to keep it under 40,000
  characters. The script ABI keeps version 1 with its host functions in merge order (wave 3's 119, then game-ui's 18
  and world-partition's 5: 142). Release 0.4.0 holds the wave.

**Wave 5 landed (2026-10-20).** The owner asked for it on 2026-10-20: physics on box3d instead of Jolt, and more
projects that show the engine, with assets downloaded online, since Tetris alone was too barebones; the wave-4 audits'
list ("Later") waited. Two streams were planned, showcase-samples and physics-box3d, each with a spec that an
independent review corrected before work began (20 and 18 corrections); both branched from release 0.4.0 (580eee9e)
and ran at the same time on the Mac (Apple M2 Max, 12 threads, MoltenVK 1.4.2), the Windows reference machine still
away. showcase-samples merged into main first, after its review round (17 findings, fixed or answered); physics-box3d
then merged main, ran the samples' regressions on box3d, fixed what box3d changed in them, and merged after its review
round and the landing's review (18 and 2 findings), on the same day. Each merge waited for CI to be green on all six
jobs. Numbers are Release builds on that Mac, A/Bs interleaved under the perf lock against 0.4.0's build; every metric
the wave added is provisional (the open items' "Waiting for the Windows reference machine").

- `showcase-samples`: three sample projects beside Tetris, made through the editor by an agent the way Tetris was, with
  CC0 and CC-BY assets downloaded from Kenney, ambientCG, Poly Haven, the three.js examples and Khronos' glTF samples
  (each sample within 25 MB of source assets and 20 MB of documents, credited in its `CREDITS.md` and in
  `ThirdPartyNotices.md`'s "Sample assets", checked by `Samples.Content`): the Physics Playground (615 bodies in six
  stations; on Jolt 64 of 64 dominoes fell, the tower drifted 0.11-0.15 m, 500 balls stayed in the pit, 8 ghosts sank
  through the balls and the goal caught 5 of 5), the Open World (a 768 m island of 97 generated terrain tiles with mesh
  colliders, a three-layer world in 64 m cells, 20,000 props scattered by biome, 30 foxes, five camps, day and night
  and a tour; the start's render draws 5,468 instances in 13 draw commands) and the Third-Person Adventure "Sky Islands"
  (a skinned robot with an animation controller and footstep events, a follow camera, coins, jump pads and a pushable
  crate, a main menu, pause and settings menus of sliced images, save files, a loading scene and an attract mode). One
  runner (`StrataTests/Editor/SampleProject.cmake`) plays each sample's regression command script, exports it in full
  and pruned and runs both games headless, and windowed where a GPU is; command scripts gained `until` (exact waits in
  fixed steps while paused), and the launcher and `project.samples` show the samples' pictures. Fixed on the way:
  `prefab.instantiate {instances}` lost the prefab root's transform, simulated taps at time scale 0 never ended, and the
  project thumbnail did not wait for game UI glyphs. On the Mac the Open World tour's frame work takes 1.43 ms (median;
  p95 2.71 ms) with the GPU busy 4.37 ms, over 958 frames at 1920x1080; the samples add about 13.9 MB to a compressed
  archive.
- `physics-box3d`: box3d (MIT, pinned at efca0c1 of 2026-10-15, 40 commits after v0.1.0) replaced Jolt, which is gone
  (submodule, options, notices, tests), behind the same components, scene services and script API (the ABI unchanged):
  box3d's tasks on the job system, its asserts and log through the engine's, box hulls shared per size, convex hulls of
  at most 64 vertices (44 when 64 do not fit) and triangle meshes cooked on jobs and shared by reference, contact and
  sensor events folded per entity pair, collision layers as filters with a query bit, character controllers as
  Strata's own move on box3d's mover queries, capacities as the room box3d reserves at play start (up to 131,072
  bodies) that grows in place, worlds created on the main thread only and within box3d's 128, Strata's own CPU options
  (`STRATA_USE_*`) and a golden-hash case that pins determinism with 1 and 5 workers. 0.4.0 scenes load unchanged
  (`MaxBodyPairs` is ignored; `scene.setSettings` warns about it). On the Mac, against 0.4.0's build (3 interleaved
  pairs, ratios of medians): busy frames 0.58-0.63 (`Boxes70k.ActiveFrame` 24.7 to 14.4 ms), `Characters1000Step` 0.41,
  `Boxes30k.StartAnd30Frames` 0.66, `PerfGPU.World.CellDriveMaxFrame` 0.73; creating and removing bodies costs more,
  since box3d inserts every shape one at a time on the main thread (`Create100kStaticBodies` 3.40, 28.8 to 98 ms;
  `Create10kBodies` 2.89; `CellUnloadMaxFrame` 1.66). The samples passed on box3d before any change: the tower settles
  with 1 cm of drift, the robot pushes the crate 3.9 m in the regression's 2 s (2.9 m on Jolt, whose character slowed
  against it), and the Playground's pour, which spawned overlapping 1 m balls that box3d pushes apart at up to 3 m/s,
  now spawns them at the prefab's scale with all 600 in the pit.
- Landing: showcase-samples landed first, so physics-box3d merged main and checked the samples on box3d (one conflict,
  in `Docs/DeveloperGuide.md`, both blocks kept; box3d's `physics.stats` keeps the `pendingOther` that the samples'
  regressions wait for): the Open World's perf checks bound the bodies waiting for their mesh colliders
  (`PhysicsBodiesPending`), since box3d's bodies wait for room only at the growth limit, and the SDK's `Instantiate`
  says which forms replace a prefab root's scale. The script ABI keeps version 1 and its 142 host functions. Release
  0.5.0 holds the wave.

**Wave 6 landed.** Its two streams, `project-settings` and `render-quality`, each planned in a spec that an
independent review corrected before work began, branched from release 0.5.0 (6425b57b) on 2026-10-22 and ran at the
same time on the Mac (Apple M2 Max), the Windows reference machine still away. `project-settings` landed first,
`render-quality` second.

- `project-settings`: settings are sections that modules register (`SettingsRegistry`), read through `IProjectSettings`
  on the main thread. The project file (version 3) keeps them, sections the build does not know included; exports carry
  them in the game's manifest (version 5), which the runtime validates before the game starts; the editor writes the
  project file on a job (`SettingsFileWriter`) and refuses to release a project whose last write failed unless forced.
  Project changes are undo steps that leave the scene saved (`project.settings`, `project.setSettings` and the rest of
  the MCP toolset `settings`); preferences (`Preferences.json` in the user's data folder: the external editor, imported
  once from 0.5.0's `EditorSettings.json`, and the interface scale) are Session commands. File > Project Settings... is
  a panel of pages (Game, Export, Collision Layers, Input Actions, and a generic page for any other section), File >
  Preferences... a dialog. Collision layers are the project's: one table for every scene, prefab, world cell and
  exported game (`project.setCollisionLayers`), a working copy per scene applied on the main thread; a 0.5.0 project
  migrates once when it opens (the start scene's names first, a pair any scene kept apart stays apart, disagreements
  logged), and the samples and the feature test project did. Input actions are maps of Button, Axis and Vector2 actions
  bound to keys, mouse buttons, gamepad controls and 1D and 2D composites, evaluated once per input frame in the Input
  stage; the game UI navigates with the reserved map `UI`; players rebind from scripts (a capture takes the next control
  pressed) and their bindings stay in the game's save directory; thirteen host functions were appended (155 in all; the
  ABI keeps version 1). The editor's Input Actions page captures bindings, `input.action` holds an action whatever its
  bindings, and the Third-Person Adventure reads its controls as actions (on the way, pushing its left stick forward no
  longer walks backward, nor does pushing its right stick up look down). The game's identity, a title, version, company,
  copyright and icon (`project.setGameInfo`), reaches the exported game's window, log and manifest; on Windows the
  executable's version information and icon, which Strata's own resource writer stamps into the copied runtime (CI reads
  them back through Win32); on macOS the Dock's icon; on Linux a desktop entry beside the game. Every sample has a
  title, a version and an icon. Fixed on the way: meshes that an unbudgeted wait finalized failed when the geometry pool
  had to grow in their batch (scripted Open World starts logged "Finalization did not finish without an upload budget"
  under load, and those terrain tiles did not draw; it happened on 0.5.0 too), and `Core.FrameProfiler`'s busy-workers
  case failed on CI's Windows Debug runner. On the Mac `Perf.Input.Actions256` measured 6.76 us against its 20 us
  target; the A/Bs are in the reference machine's list in the open items.
- `render-quality`: temporal anti-aliasing with motion vectors, each renderer jittering only its frame's copy of the
  camera and keeping its own motion history (the previous frame's instances and poses) and TAA history. Presets Sharp
  (the default: variance clipping, a resampling term, a reactive mask for blended surfaces, disocclusion rejection and
  luminance sharpening in the tone mapping, which never enters the history), Original (the first TAA, softer and
  steadier) and Custom; the Basic 3D template and the three 3D samples use TAA Sharp, FXAA stays the method's default.
  The editor keeps the selection outline and overlays steady under it, captures and project thumbnails wait until the
  history and exposure settled, `viewport.capture {live, atFixedStep, frames}` and `play.start {paused}` show motion as
  it plays, and the `motion` debug view shows the motion vectors. Shadow cascades blend across their seams. Lights have
  units (lux, candela, nits; `Docs/Manual/Lighting.md`), the HDR target a pre-exposure so a real 100,000 lux sun keeps
  its detail, and automatic exposure reaches EV 16. Point and spot lights cast shadows (`CastShadows`) from one atlas,
  16 per view, cached while nothing in their range moves, on the CPU and GPU-driven paths alike. Height fog
  (`HeightFog`), a physical atmosphere (`SkyLight` `Source` Atmosphere, the sky, its light and the sunset following the
  directional light), reflection probes baked from the edited scene (`lighting.bakeProbes`, a cube array: the device's
  `imageCubeArray` is now required) and screen-space reflections (with TAA). `lighting.extractSun` makes the directional
  light the sun of an HDR sky, whose `RemoveSun` import setting paints it out. Materials gained clear coat and
  transmission (glass seen through, refracted by its thickness, tinted by its volume), imported from glTF's
  `KHR_materials_clearcoat`, `_transmission`, `_volume` and `_ior` (the glTF importer's version changed, so models
  import once more). The Open World has fog, the atmosphere, a shadowed lantern and campfires; the Third-Person
  Adventure screen-space reflections, the sun of its sky, shadowed lanterns and probes baked along the course; the
  Physics Playground clear-coated balls and a glass pit (`Docs/Manual/AntiAliasing.md` and `Lighting.md` describe it all
  for people). Fixed on the way: a scene renderer rendering frames back to back ran out of the versions of its frame
  constants (it now waits for its own oldest frames; GPU tests failed under load); a probe bake cancelled while its
  captures were copied in removed their staging folder under the copy and left them in the project; and the first bake
  after opening a scene in a fresh session reported most probes incomplete, since it waited a count of frames and its
  later views ended the texture streamer's holds on captures the earlier ones waited for. The A/Bs on the Mac are in the
  reference machine's list.
- Landing: `project-settings` landed first, so `render-quality` merged main (conflicts only in the Inspector's scene
  settings, where the collision layers became a link to Project Settings and the sky gained its buttons, and in the
  appended lists of the journal tests, Budgets.json and the feature commands); it appends no host function, so the
  script ABI keeps version 1 and 155 host functions, and its `AGENTS.md` lines were shortened to fit. Release 0.6.0
  holds the wave.

**Wave 7 landed (2026-10-29); release 0.7.0 holds it.** The owner asked for the wave on 2026-10-26
("### Wave 7" below, with what each stream landed and what was open at the integration on 2026-10-28); its plan,
reviewed specs and release notes are in `Docs/Plans/Wave7/`, kept while wave 8 builds on them. Step 0 ran on the
Windows reference machine, back with a replaced CPU, and landed on main before the wave's streams branch from its tip:

- The GPU suites ran on NVIDIA under the validation layer for the first time since wave 3: the atmosphere's
  environment cube, irradiance and prefiltered maps lost their earlier levels when made in a frame whose multiple
  scattering ran too (per-level transitions from an undefined layout on driver 617.42), so mirrors stayed black after a
  sky switch until the sun moved; they are now made whole in the UnorderedAccess state first. A timer check that read a
  reset before the GPU ran it, and a test that waited for an import by the clock, were fixed.
- `AGENTS.md` made room for the streams (37,213 of its 40,000 code points with step 0's own lines); post-processing
  and ambient occlusion moved into `SceneRendererPost.cpp` and `SceneRendererAmbientOcclusion.cpp` without changing a
  pixel; the scene layout's slots and the streams' GPU stage positions were assigned (`Include/Scene.glsl`, DG "GPU
  timing").
- The renderer's costly knobs became `RendererQualitySettings` and the `Renderer.Quality` project section: Low, Medium
  (0.6.0's costs), High (the default: 4096-texel sun cascades from 1440 output rows, Medium below) and Ultra, or Custom
  values. Games, the editor's views and captures follow it and change it while they run (`SceneRenderer::SetQuality`
  makes again only what a changed knob sizes), the Scene view can use a preset of its own (`viewport.setSettings
  {quality}`), captures draw at their view's quality and match it (0.6.0's captures used half the local shadows'
  atlas), and `StrataRuntime --quality` plays a run at a preset. Thumbnails and the probe bake keep 0.6.0's values.
- `Tools/LookDev.py` captures 23 shots of the samples and a look-dev scene at 1920x1080 and 3840x2160, stills and live,
  with their GPU stages. The 0.6.0 baseline (bc6ee123, 122 stills of 5 repeats) is kept on the reference machine in
  `.worktrees/wave7-plan/lookdev/0.6.0`, beside step 0's tip at Medium and at the projects' quality
  (`lookdev/step0-medium`, `lookdev/step0`).
- The perf budgets were measured again (five runs on bc6ee123's Release build): the Mac's provisional metrics got
  budgets and the cheaper ones went down; four metrics over budget are regressions of waves 4-6, blockers of 0.7.0,
  and creating bodies, slower on box3d, was re-baselined by the owner's ruling (the open items' "Waiting for the Windows
  reference machine"). Step 0 at Medium against 0.6.0 (bc6ee123), in five
  interleaved pairs: `PerfGPU.Renderer`'s 43 metrics at a geometric mean of 0.994, four CPU medians above 1.02, as far
  as two builds of step 0's own sources whose code lies 48 bytes apart differ (up to 1.067): the code's layout, not a
  cost of step 0's (`Docs/Plans/Wave7/specs/step0.md`, "As built"); the look-dev stills identical but where the
  captures' atlas changed (shots with shadowed point and spot lights) and under the physical sky (the atmosphere's
  fix), their GPU busy time at a median ratio of 1.003. High at 3840x2160 costs the samples' scene cameras 0 to 0.52 ms
  of GPU time over Medium.

**Wave 8 in progress (step 0 landed, 2026-11-02).** The owner asked for the wave on 2026-11-01 ("### Wave 8" below,
with what each step 0 lane landed); its plan, reviewed specs, the orchestrator's rulings and the release notes that each
landing adds to are in `Docs/Plans/Wave8/`. Step 0's lanes landed on main one by one, and the rendering streams branch
from its tip:

- The 0.7.0 baseline: every look-dev shot of 76f28a21's Release build, both sizes, all three anti-aliasing modes, 5
  repeats and the live shots, is kept on the reference machine in `.worktrees/wave8-plan/lookdev/0.7.0`, its Medium
  stills in `lookdev/0.7.0-medium`. `.worktrees/wave8-base` keeps that build for interleaved A/Bs (its 4K 84-set in
  `lookdev/wave8-base-84`): on the reference machine's temporary memory costs are judged only that way, and no budget
  changes.
- The object store, damaged by the crash of 2026-10-28 (7 corrupt and 12 missing loose objects; history walks stopped at
  f99f3a41), was repaired from a mirror of origin: `git fsck` is clean and every tracked file hashes equal to its index.
- `AGENTS.md` made room for the streams: 37,663 of its 40,000 code points after step 0's lanes, and each stream adds
  one digest bullet of at most 250.
- Golden rule 1 now forbids only other folders of the machine (the owner, 2026-11-01): open-source projects are read as
  much as helps, and their code is copied or ported only under a permissive license (MIT, BSD, Apache-2.0, zlib, Boost,
  public domain or CC0), with its notice in `ThirdPartyNotices.md`'s "Code derived from other projects" and its source
  named in the commit message; copyleft code is read for ideas only.
- Left open by step 0: Slang's modules are not yet measured to cost what glslang's did, nor run on a Mac's GPU (open
  items, "Shaders in Slang"); vsync does not pace windowed mode on the reference machine (open items, "Frame pacing");
  an object that stays near the camera still re-fits the sun cascades once, and one under a tenth of the view waits up
  to 0.5 s in a coarser cascade; without contact shadows the 5 cm plate's shadow edge shows the shadow map's texel
  steps at High (wave 7's `sun-shadows` line).

Open items and deferrals later waves own:

- GPU-driven rendering (wave 4's `gpu-driven-rendering`): skinning still runs per draw (compute skinning once per
  frame, measured by a GPU-time metric, is later work); the shadow cascade cache follows the GPU-driven path only (the
  CPU path redraws every layer each frame); render scenes are copied whole at play start, not shared by copy-on-write
  pages; on tile-based drivers a timestamp inside a render pass may land at the pass's end, so `GPU.UI` is checked
  for presence, not for its time.
- Tests: `Editor.ImportSettings`' close check (2.0 s) measured 2.012 s once on CI's Windows Release runner;
  `Editor.UI`'s "No uploads in the last 120 frames" fails when GPU suites ran before it in the same process (CTest's
  shards never combine them).
- World I/O (wave 4's `world-io`): pack version 3 (small entries bundled, a zstd dictionary, shared blocks decoded
  once; the 10.7x inflation of small assets in `Perf.Asset.Pack30kMaterialsRatio`), cold reads without the system
  cache with 16 outstanding reads (`PerfGPU.Pack.ColdStreamStressWorld`) and a background-thread cap while loads run
  are later work. Additive loads have no prototype form yet (`SceneLoadOptions::PrepareAs`,
  `SceneLoadRequest::TakePrototype`): cells and sub-scenes load through `SceneLoader` or `SceneDocumentReader` with
  `EntityPrototypeBuilder`. Prefab and asset-editor saves stay synchronous.
- World partition (wave 4's `world-partition`): `PhysicsSettings::GrowthLimit` has no scene or project setting, so the
  "bodies without collision" pill appears only in tests; a world's save writes its World document in a second request
  once the cells' stamps exist, so a crash between the two leaves new cells unlisted (the scene is marked saved, and the
  recovery journal rebased, only after the second, so the journal keeps their edits and the next save lists them); the
  running game's cells are summed up in `play.state` and `editor.status` (`running.world`), never listed.
- Tests: `Strata::Tests::ScopedJobSystem` is defined twice (`Asset/AssetTestUtils.h` and `Physics/PhysicsTestUtils.h`,
  with different constructors), so a test file cannot include both.
- Tests that start the CLI's fake editor set its environment variables in the test process (`CLI.Discovery`,
  `CLI.Application`, `CLI.Mcp`), against the testing rule: `ProcessSpecification` has no environment of its own, and
  the launcher finds the session of the process it started by its ID, so a wrapper process cannot set it.
- Play-testing (wave 4's `authoring-and-playtest`): an `input.sequence` step whose command event fails a check only the
  command can make (a key name, `input.click`'s hit test) releases its holds and resumes the game, but the step's
  earlier events have already reached the game's input (checking every value up front needs a dry run of each
  `input.*` command); the Game view at a fixed resolution reports the editor's UI scale as the game's content scale.
- Flaky tests seen once, not reproduced since: Windows `CrashGuard`'s "repeated faults on other threads", the
  Mac's `GPU.AssetResidency` "Arrivals that need room..." and, in full parallel runs while the other wave-6 stream
  loaded the Mac, `GPU.TextureStreaming`'s tight-budget case, and on CI's Windows Release runner
  `Editor.ScriptWorkflow`'s "script.open starts the external editor at the line..." (its stand-in editor wrote no file
  within the test's 10 seconds; the rerun of the job passed).
- Game UI (wave 4's `game-ui`): world-space canvases draw unlit, after post-processing, and the game's hit tests on
  them ignore scene geometry in front (the editor's picks check meshes).
- Optional subsystems: a build without the scripting module (`STRATA_WITH_SCRIPTING`) is not offered yet, because
  `GameRuntime`, the editor and many tests use the script engine directly; physics, audio and animation can be left
  out (`STRATA_WITH_PHYSICS`, `STRATA_WITH_AUDIO`, `STRATA_WITH_ANIMATION`, wave 4's `engine-modules`).
- Physics at runtime (wave 7's `physics-runtime`, steps 1-5 landed, and below its cut the deferred removal of a leaving
  cell's static bodies): the step's parts, box3d's stages and counters, sleeping bodies and how bodies settle are
  measured (`physics.stats`, `editor.status`, the perf log, the Streaming panel); the project's `Physics.Simulation`
  tunes recycling, sleeping and the contacts (defaults: box3d's 5 cm, a sleep threshold of 0.08 m/s); character
  controllers and kinematic bodies never recycle; contacts nobody tracks cost no lookups, and tracked ones are looked up
  only for bodies that have some; the write-back and the synchronization read records and components on the job threads
  (Strata's part of a 70,000-box step 11.1 -> 7.7 ms, then the sync 3.3 -> 2.2 ms, in A/Bs on the loaded machine). Not
  done: poses written back from box3d's move events (a move event sits at the body's place in box3d's awake set, the
  same random access `b3Body_GetTransform` makes, and copying it in the serial event loop moved that access to the main
  thread: no gain in two A/Bs), a new rule for box3d's workers (today's, the job system's workers and the stepping
  thread, stays: a sweep of 4-24 workers, five repeats interleaved on the loaded machine, found box3d's step falling up
  to 12-16 workers and level beyond, 70,000 boxes 27.6 ms at 4, 18.4 at 12, 18.2 at 16 and 17.6 at 24, the 30,000
  tracked 9.1, 5.8, 5.7 and 6.2, and no count beating today's by the 3% a change needs; the quiet machine with its own
  memory may differ, `Tools/PhysicsSweep.py workers` measures it), the characters' prelude (0.05 of a 0.8 ms crowd step,
  below the 3% an item needs; the moves take 0.37 ms in parallel and the serial part after them, the entities' writes,
  the presence bodies' teleports through box3d and the pushes, 0.3 ms), and box3d's cost of sleeping bodies
  (`Perf.Physics.Sleeping70k.StepRatio` 4.4-7.3 against its 1.2 target: 1,000 falling boxes beside 70,000 sleeping ones
  take box3d 2.35 ms instead of 0.23, its transforms stage enlarging moving proxies up a dynamic tree that holds the
  sleeping ones, while Strata's part stays at 0.1-0.2 ms). The unload's Strata share is within its target (at most 1.25
  times box3d's own removal): in the request's frame physics' changes took 7.9-9.2 ms (medians of seven interleaved runs
  each, the stream's base and the stream), of which box3d's first disable of the 25,000 bodies in a fresh world takes
  6.6 alone (`Perf.World.CellUnload.LibraryOnly`, which times it that way since: a second disable of the same bodies
  takes 3.3) and Strata's own work (the activity checks, finding the records, their bookkeeping) about 1.1 ms timed in
  process. Then box3d's removals left the request's frame: a static body without contacts that leaves the simulation is
  parked (its shapes collide with nothing at once, and box3d disables it a second later in slices unless it came back or
  went first, as a cell's entities do), and the request's frame went from 10.3 to 6.8 ms (medians of six interleaved
  runs on the loaded machine; 8.1-10.0 against 3.5-4.0 in its quieter runs, so near `CellUnloadMaxFrame`'s 4 ms target).
  Not done from below the cut: the scene's world matrices from physics (a Scene API whose cache contract the Scene
  module decides), rolling resistance (not needed: the pits sleep with the 0.08 m/s threshold), a per-scene override of
  `Physics.Simulation` (no project needs one yet), and upstream requests to box3d (a settable recycle angle, a run-time
  reserve, bulk insertion and removal, sleeping proxies kept apart from moving ones). The sweep found too: 1 m tiles
  bump sliding boxes and rolling balls at their inner edges (44 mm at 2 m/s, 630 mm at 8 m/s, with or without
  recycling), which a merged floor or a triangle mesh avoids; a pile's time to sleep is chaotic, so the pits are compared
  over nine layouts each (the distance does not change how soon the 500-ball pit sleeps, the 2,000-ball pit slept soonest
  at 5 cm, and 0.08 m/s beats 0.05 in every layout); and the recycle angle was not measured in a throwaway build: the
  share recycled keeps rising with the distance while bodies move, so nothing shows the angle limiting it. Piles of balls
  grow box3d's contact storage once, as they form (4.7 to 6.1 contacts a body against the automatic room's 2; the
  Physics Playground's first play 3,152 in 2,048), which `PhysicsStats::ContactRoomGrowths` counts; the perf lab's box
  stacks and the other settle scenes stay within their room, so `c_ContactsPerBody` stays 2.
- Physics: dynamic bodies below other dynamic bodies are still written back one by one; a collider built from a mesh
  that is reimported while evicted keeps the old data until the mesh loads again (the source revisions render-scene uses
  would tell).
- Physics on box3d (wave 5's `physics-box3d`): creating and removing many bodies costs about three times Jolt's time,
  because box3d inserts every shape into its trees one at a time on the main thread (the numbers are in the reference
  machine's list below), and Strata's own part of it grew with allocations per body. Wave 7's `physics-runtime` measured
  box3d alone beside each metric (`*.LibraryOnly`, 2026-10-28, the reference machine with its temporary memory and other
  builds running, so both shares about 1.3 to 1.6 times the campaign's): `Create10kBodies` 19.1 ms of which box3d's 10.3;
  `Create100kStaticBodies` 214-259 ms of which 128.9; the `Grow65kMaxFrame` frame 198-220 ms of which box3d's 102.8 in a
  world reserved for 65,536 and 90.7 in one reserved for 131,072 (the growth's 12 ms lies within the 30 ms spread of five
  worlds: `Grow65k.GrowthMs` is within noise); the unload's request frame 8-12 ms of which box3d's first disable of the
  25,000 bodies 6-8 (step 4), and 3.5-7 ms since its static bodies are parked and leave box3d a second later.
  The owner on 2026-10-27: "I don't think physics object creation performance is a big deal. But i care about runtime
  performance", so creation was not optimized in wave 7; the way down stays bulk insertion in box3d (an upstream change
  and a later bump) and fewer allocations per body in Strata. The owner left it as it is on
  2026-10-20 ("creation speed is not too important") after a check found no easy fix: bodies created disabled pay the
  tree insert when enabled (12-15 ms per 25,000 either way); creating a streamed cell's statics across frames with a
  filter that collides with nothing but keeps their category, then setting the real filter without a pair search at
  activation, costs 0.1 ms per 25,000, but box3d then makes no contact for bodies already resting within a proxy's
  margin (a box pushed into a newly active wall went 58 mm in; sleeping boxes sank 51 mm), so that route needs the
  overlapping movers re-marked and woken at activation and mesh and compound shapes measured; disabling 25,000 bodies
  at an unload costs about what destroying them does (2.7 against 2.4 ms). The other ways down are bulk insertion in
  box3d (an upstream change and a later bump) or fewer allocations per body in Strata; triggers do not see triangle-mesh colliders (box3d's sensors skip meshes), and a
  triangle-mesh collider away from its trigger body's origin is unusable, with a warning; box3d holds at most 128
  worlds; its mover, on which the character controllers are built, is experimental upstream. MSVC builds under Ninja
  get no `/arch:AVX2` where the Visual Studio generator's do (as with Jolt): unifying them changes what CI's Windows
  Release job builds, an owner's choice.
- Samples (wave 5's `showcase-samples`): their windowed and image checks (label `gpu`) have run on the Mac only, not on
  Windows or NVIDIA; the Open World has no sound and the Third-Person Adventure no music (no CC0 loop that fit was
  found); Quaternius' larger packs refused a scripted download (itch.io, Google Drive); the importer skips the robot's
  facial morph targets; the Open World's editor run allows physics' warning that it started simulating after waiting
  10 s for meshes (waiting for cells steps the paused game, which spends that simulation time on slow runners, and
  `MeshWaitTimeout` is no scene setting), while its exported games must not print it.
- Textures: HDR textures stay RGBA16F until a BC6H encoder exists.
- Two caches keep assets between frames: `SceneRenderer`'s environment
  cache (the sky's map) and `TextRenderer`'s glyph atlases (their fonts). Streaming the stress world from pack version 2
  takes 1.03-1.09 times version 1's time (zstd blocks and their checksums; `PerfGPU.Pack` reads whole textures, without
  mip streaming), close to the 1.1 acceptance limit, which needs a confirmation in about one run in five.
- Imports: a few long steps of an import cannot be interrupted (stb_image decoding a large PNG, the resize to `MaxSize`,
  one mip level's filtering, meshoptimizer simplifying one primitive); they never hold a frame, but delay when a
  cancelled import frees its thread. Moving or deleting a file that another asset's import reads as a dependency does
  not cancel that import (the file watcher's change event replaces it). A project whose large scenes have no reference
  records yet spends seconds of its first scan in the reference search: it streams the document, so it holds only the
  text and the references, but on one thread (about 3 s on the Mac for world-io's realistic 1,000,000-entity world).
- Scene loads (wave 3): a load's progress weighs a partly loaded texture by its whole stored size, so the bar jumps when
  a large texture's tail arrives; `AssetManagerBase::EstimateFirstLoadBytes` counts a partial load as up to 64 KiB, more
  than a small tail, so a very tight texture budget can leave textures to the view that would have fitted; a sub-scene
  holds its prepared scene and its compiled prototype together until the prototype is made; a `SubScene` a script keeps
  in a member is reset by hot reload (only fields survive, so scripts keep its ID in a field).
- Prefabs: applying, reverting and propagating compare and rebuild instances through their JSON form
  (`PrefabInstances`), not the compiled prototypes scene loads use; a prefab edit mode is later work.
- Animation and characters: root motion through a character controller is walked at the frame's first fixed step, and on
  a tilted entity its vertical part becomes the controller's velocity (gravity and the ground then decide the height).
- Mip streaming: its limit "the longest frame at most 2 ms over the whole-texture baseline's" is gated as a ratio
  (`Mips.MaxFrameRatio`, budgets cannot be negative) and was measured on a shared machine only; the transfer queue stays
  off by default (it loaded the stress game no faster and held more memory).
- Editor: in a 1600x900 window the default layout's bottom dock shows one row of Content Browser tiles and a few rows of
  the Activity panel; the audio preview was checked on the null device only, not by ear; script builds under the
  system's temporary directory or 8.3 short paths show MSBuild's warnings MSB8029 and MSB8012 in the Problems panel.
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
- Frame pacing (wave 8's S0.6): vsync does not pace windows in windowed mode on the reference machine, focused or not.
  Third-Person played in the Release editor (focused, visible, windowed 2400 x 1350) and in its exported game (focused,
  windowed 1280 x 720) on the 120 Hz display, 2026-11-02, instruments counting each `BeginFrame`'s waits over 600-frame
  windows:

  | Run | Frames/s | Acquire p50 / p95 | Frames-in-flight wait p50 / p95 | Mode chosen |
  | --- | --- | --- | --- | --- |
  | Editor, playing (`editor.perf`: 1,100/s, `Present` p50 0.05 ms) | 1,230-1,280 | 0.002 / 0.003 ms | 0.15 / 0.8 ms | FIFO |
  | Exported game | 1,340-1,550 | 0.002 / 0.003 ms | 0.13 / 1.1 ms | FIFO |
  | The same without `VK_LAYER_RTSS`, without `VK_LAYER_NV_present`, without both | 1,580 / 1,470 / 1,490 | 0.002 / 0.003 ms | 0.2 / 1.0 ms | FIFO |
  | The same with vsync off | 1,900-2,100 | 0.000 / 0.001 ms | 0.000 / 0.2 ms | MAILBOX |

  The surface offers FIFO, FIFO_RELAXED, MAILBOX, IMMEDIATE and FIFO_LATEST_READY; each run made its swapchain once
  (3 images; the surface allows 2 to 8). The loader inserts the implicit layers `VK_LAYER_NV_optimus`,
  `VK_LAYER_NV_present` and `VK_LAYER_RTSS`; disabling them changed nothing. Windows' "Optimizations for windowed games"
  is on (`SwapEffectUpgradeEnable=1`). So the engine asks for FIFO and the presentation engine hands every image back at
  once: not the engine's code, not focus or occlusion, not those layers, but a driver or system setting, unconfirmed.
  What NVIDIA Control Panel's "Vertical sync" and "Vulkan/OpenGL present method" say (globally and for
  `StrataEditor.exe`) is the one fact an agent cannot read; the owner is asked. Until then only a frame-rate cap paces
  windows here (the editor's `ActiveFrameRate`, S0.4, unlimited by default), and the Performance panel and
  `physics-timing` assume the frame rates seen. Not measured again: an unfocused or occluded editor (the audits saw
  190-900 frames a second). Not measured at all: a borderless or fullscreen game (S0.6's case E), whose presents can
  take another path (independent flip), so the finding holds for windowed mode only.
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
  Debug test run) all passed, and Windows logged no hardware error. On 2026-10-12, while wave 3 was integrated, the
  machine stopped twice with a MEMORY_MANAGEMENT bug check (0x1A), the second time with the memory slowed from its rated
  3600 to 3200 MT/s; it runs at the rated 3600 MT/s again, at which `Budgets.json`'s reference values were measured, and
  compilers kept crashing now and then (files that compiled on the retry). Until a memory and CPU stress test passes,
  one-off crashes on this machine are suspect.
- Perf to watch after wave 3's campaign, and what its finishing round (2026-10-13) did about it, measured in interleaved
  runs against the build before each change (Release, the reference machine):
  - `SceneOpenOver1MMaxFrame` (bimodal, about 0.6-1.1 ms or 1.3-4.1 ms against 1.143): its scene.open starts as
    play.stop lets a background job free the million-entity play copy (80-94 ms), and the loading worker then frees the
    parsed document. Meanwhile the main thread's allocations of sizes the low-fragmentation heap does not serve wait for
    the process heap's backend: 0.6-2.7 ms. A synthetic benchmark shows it alone: allocations of 1 to 16 KB stall up to
    2.5 ms while another thread frees 3 million blocks, whatever that thread's priority, also when it frees in slices
    with a 1 ms sleep between them, and stay under 0.12 ms once it is done. Two of the waits were removed: the console
    sink's writes, which blocked the first log line after a quiet spell for 2.4-9 ms (log sinks are now written by a
    background thread, `BackgroundLogSink`; warnings and errors are still written when the call returns), and
    scene.open's growing vector of frame times, the longest frame in 7 of 10 diagnosed runs (frame times of editor.wait,
    scene.open and project.export now go into storage reserved when the command starts,
    `CommandUtils::FrameTimeRecorder`).
    First attempts over the budget went from 5 of 6 runs to 6 of 24 (and once all three attempts of a run); the
    remaining long frames wait in the renderer's command recording while the document is freed. Removing those needs the
    memory of scenes and of parsed documents off the process heap (a registry allocator, an arena per load); the Segment
    Heap removes the stalls but kept freed memory committed for about 0.6 s (the working set right after opening the
    million entities grew from 541 to 1300-2200 MB) and slowed extraction (`GatherDraws` 0.31 to 0.46 ms on 100,000
    cubes), so it was not adopted.
  - `ImportSettings4kMaxFrame10kCubes`' 6-27 ms frames in `vkBeginCommandBuffer` while BC7 compresses: the background
    pool ran a thread on every hardware thread but one; below normal priority, they still delayed the graphics driver's
    own threads by a scheduling quantum. It now leaves four hardware threads to the frame, keeping at least half for
    itself (`JobSystem::GetDefaultBackgroundThreadCount`: 20 of 24 on the reference machine): measured 2.0-2.1 ms in the
    A/B runs, its budget lowered from 4.819 to 3.09; the round's final PerfStability runs measured 1.7-2.8 ms, up to 92%
    of that budget, so it is one to watch.
  - `ThumbnailsMaxFrame`'s second mode (1.2-2.0 ms, 43-45 frames): its confirmations measured another state than the
    first attempt, whose frame then destroyed the previous attempt's 20 thumbnail textures at once (2.1-2.3 ms, about
    0.1 ms of driver time each) and found the project's textures already streamed. Completed GPU resources are now
    destroyed within 0.25 ms per frame (`DeferredReleaseQueue::c_FrameDestroyBudget`), and every attempt starts cold:
    61-64 frames, 0.70-0.98 ms in 13 of 14 runs (one run's attempts 1.2-2.2 ms in one process, as a main thread on an
    efficiency core shows).
  - Still frames with 3,000 streamed textures (1.15-1.27 times main's): resolving 3,000 materials and their textures one
    by one was 0.58 of the frame's 1.0 ms; frames that use the same materials now take their data over from the frame
    before when the materials, the textures and their bindless slots are unchanged
    (`SceneRenderer::MaterialResolution`): `IdleFrame3kStreamedTextures` 1.016 to 0.614 ms, `Render10kMaterials` 1.332
    to 0.608, `EditFrameMaterials10k` 1.815 to 0.993, their budgets lowered. What a still frame still does per draw
    (culling, shadow casters, gathering, the streamer's report) costs about 0.3 ms of the rest.
  - `RuntimeMaxFinalizeStage` (98-105% of 3.723 ms) stays as it is: the exported game's frame 0 finalizes what the start
    scene's load read before the first frame (17 materials, 17 mip tails at 0.06-0.64 ms each, mostly the driver's image
    creation, and the first step of a mesh) until the engine's per-frame finalization budget (`FinalizeMsPerFrame`, 4
    ms) ends it: 3.7-4.2 ms in three runs, frames 1 and 2 1.8-2.1 ms, every later one under 0.7. That is the spreading
    the budgets already do; spreading it further would mean a smaller finalization budget for every frame (slower loads
    everywhere), and moving the work before the perf log's first frame would hide it rather than remove it. The metric's
    budget lies below the engine's own limit since the backlog moved to frame 0, so its first attempts exceed it now and
    then; cheaper texture creation (suballocated GPU memory) is the way down.
  - The Activity panel's Result column at 150% in the default layout: it is now as wide as the longest result it lists
    (at least six text heights), and Parameters takes what is left.
  - The round's final verification (its tip, 0a97046): `ctest --preset windows-debug` under the validation layer and
    `ctest --preset windows-release -LE perf` passed all 47 tests each, and `PerfStability.py --runs 3 --wait-quiet 20`
    passed 3 of 3 runs (114 metrics each, the canary at 0.96-0.98 of its reference). `SceneOpenOver1MMaxFrame` measured
    0.66-0.86 ms in all three first attempts; confirmations decided `RuntimeMaxFinalizeStage` twice (first attempts
    3.77 and 3.93 ms, see above), `ExportMaxFrameStress` once (6.83, then 4.19 ms) and `SceneSwitch100kMaxFrame` once
    (0.215, then 0.078 ms).
  - Unchanged: `EntityCreate100kRoots` at 85-90% of its budget and the Streaming panel's refresh frames at up to 93%;
    `SceneSwitch100kMaxFrame` (0.31-0.37 ms in 2 of 10 first attempts against 0.2002) and `Mips.MaxFrameRatio` (1.28 in
    1 against 1.19), which need a confirmation now and then; `ContentBrowser10kThumbnailsP95` once at 0.053 ms in 10
    runs (0.036-0.042 otherwise) against 0.057; and `PerfGPU.Editor.ImportSettings4kMaxFrame`, whose first attempt
    measures 6.9-7.6 ms when run alone in a fresh process (main's build 2.9-3.6 ms; 2.1 ms in the perf label's runs,
    where the cases before it warmed the process): its view draws the texture whole, as main did, and its windows end
    once the texture is whole (later attempts 2.97-5.23 ms), so the first attempt's hold is not the upload's. Platform
    code only CI compiles: the POSIX `Platform::FindProgram` and symbolizer (function names only on Linux and macOS).
- Rendering (wave 6's `render-quality`): glass is seen through only to what was drawn before it (glass behind glass and
  blended surfaces behind glass do not show), gets no ambient occlusion and casts no shadow, and its thickness comes
  from the material, not a thickness map; screen-space reflections cover a clear coat's base only and trace no
  transparent surface; TAA keeps a few softer frames where a surface uncovers what it hid, cannot reject surfaces that
  recede fast, and Original stays soft in slow pans; probes are baked, so a moving sun does not relight them; at most 16
  shadowed point and spot lights per view. Left for later (also in "Later" below): MSAA (rendering below native
  resolution and temporal upscaling are ruled out: the owner's direction of 2026-10-26, in "Later"); layered shadow
  rendering, occlusion culling of local shadow views, more than 16 shadowed lights, sized local lights (PCSS) and their
  contact shadows (wave 7 gave the sun both); run-time probe capture, cube texture assets and clustered probe lists; SSR
  for transparent surfaces; volumetric fog and light shafts, the atmosphere from altitude (wave 7 added aerial
  perspective); thickness maps, sheen, specular, iridescence and anisotropy, EXR import and area lights; shadows of a
  second directional light (the moon).
- Medium's Checkerboard screen-space reflections are the one effect that needs TAA to look right (they leave their noise
  to its history, as Strata 0.6.0's did): the named exception to the owner's direction in "Later", kept for Medium's
  costs.
- Spatial screen-space reflections (High and Ultra) still add a crawl in motion. Wave 8's R1 halved what they add to the
  per-pixel flicker of a strafe past the specular lab's rough sheet at Spatial without TAA (1.36% to 0.57%), not
  elsewhere: at SpatialHigh 0.92% to 0.57%, on wet ground 0.49% to 0.28%, with TAA 0.94% to 0.60% on the sheet and
  0.36% to 0.40% on wet ground (`GPU.SceneRenderer.SSR`'s strafe case holds guard bounds). Maps of the per-pixel
  flicker put it in the reflections of the ladder's shiny balls (their limbs and glints), of thin posts and of edges;
  with TAA at every reflected edge, because the history follows the surface's motion, not the reflected image's. Tried:
  rough surfaces' color level from the lobe's footprint at the hit (3-9% less without TAA, 18% less with it); the
  previous frame's TAA output as the color source (15% less on wet ground with TAA, nothing on the sheet); the march
  without its 4x4 offsets (15% more). Leads: reprojecting reflections along their reflected image's motion, and
  coverage for hits at thin and curved edges.
- Spatial screen-space reflections (wave 7's `light-and-materials`, High and Ultra) are not always repeatable on the
  integration machine (RTX 5090, driver 617.42). `Tools/LookDev.py`'s `lookdev.mirror` at 3840x2160, captured again
  and again in one editor after the look-dev scene's debug views, comes out as one of two images (12-60% of the
  repeats the second): with FXAA and a manual exposure 33 pixels of the floors' reflections differ by 1-7 codes, with
  automatic exposure about 800 pixels by a code through the histogram. Never without the reflections, nor with the
  mirror scene shot alone. The trace's output differs at about 300 isolated texels (the same confidence, the color
  0.1-4% apart), yet everything the engine gives it is the same: byte-identical frame and push constants in every
  picture; in one frame, a second trace of saved copies of the first picture's inputs (unchanged, checked by their
  hashes) through another pipeline of the same shader with a plain constant buffer always gives the frame's own image
  of the two; Vulkan's core and synchronization validation report nothing. The same shader on the same data gives one
  of two results by the frame, below the API. More frames in the capture's wait cannot help (each picture traces once
  from identical inputs). Next: other GPUs and drivers, and a reduced compute test outside the renderer.
- Project settings and input actions (wave 6's `project-settings`), left for later: game-shell's macOS app bundle (the
  Finder's icon, the version and copyright in `Info.plist`), installing desktop entries, signing exports (quality
  presets came with wave 7's `Renderer.Quality`) and input only while the game's window has the focus (gamepads are
  still read while a shipped game is in the background, as in 0.5.0); local multiplayer (devices assigned to players, an
  action state per player) and per-world input contexts; input processors (invert, sensitivity, curves), modifiers and
  chords, and mouse motion and scroll as action sources; binding names that follow the keyboard layout in games
  (`Window::GetLayoutKey` is out of scripts' reach); scripts reading the game's title and version; moving the Physics
  Playground, the Open World and Tetris onto actions, and a rebinding row in the Third-Person Adventure's Settings menu;
  copying settings sections between projects, rebinding editor shortcuts and theme variants; gravity as a project
  setting (it changes every scene's behaviour: the owner's choice; the fixed rate is the project's `Scene.Timing` since
  0.8.0). Opening a scene clears the undo
  history, the project's steps with it (the project file was written at each step, so nothing is lost).
  `Platform.DockIcon` needs a window server, so it runs on the Mac only (`StrataTests.DockIcon`, label `gpu`) and CI
  never runs it; no Linux desktop has listed an exported game's desktop entry yet (CI's Linux jobs check the file).
- Waiting for the Windows reference machine (i7-13700K, RTX 5090), which came back on 2026-10-26 with its CPU replaced
  by the same model (the canary `Perf.Harness.Sort200k` at 9.95-10.03 ms against its 10 ms reference). Wave 7's step 0
  ran the GPU suites there under the validation layer, the first time on NVIDIA since wave 3, and a campaign of five
  `-L perf` runs on bc6ee123's Release build (`Tools/PerfStability.py`, the machine 19-24% busy), whose medians gave the
  Mac's provisional metrics their `Measured` and `Budget` and lowered the cheaper ones (the step-0 commit lists every
  entry). The Mac's numbers that waited here are in git history (this file at bc6ee123). What stays open:
  - Blockers of 0.7.0, over budget in every attempt when measured alone and, interleaved with wave 3's Release build
    (3b87411c, built on 2026-10-13 under these budgets; 3 pairs, medians), within budget there: regressions of waves 4-6
    (3b87411c..bc6ee123), not the machine. `Perf.EditorPanels.Performance600FramesP95` 0.0993 ms against wave 3's 0.069
    (budget 0.0924); `PerfGPU.Editor.ThumbnailsMaxFrame` 2.073 against 0.978 ms (budget 1.149);
    `PerfGPU.Editor.SceneOpenOver1MMaxFrame` 1.839 against 0.664 ms (budget 1.143); and
    `PerfGPU.Editor.EditFrameNested100k`, within its budget alone on bc6ee123 (0.2769 ms against 0.2808) but 1.26 times
    wave 3's build in the same pairs (0.2708 against 0.2153 ms) and over budget in every attempt of step 0's `-L perf`
    run (0.287-0.299 ms). Each is fixed before the release, not budgeted; `PlayFrameNested100k` (1.43 times wave 3's,
    within its budget) goes with the last. Wave 7's `editor-perf` took them on in today's code, no feature removed
    (its verifier's interleaved A/Bs against step 0's tip cc9524a1 on the quiet machine, under the temporary 2133 MT/s
    memory, so not comparable with `Budgets.json`):
    - Fixed: `Performance600FramesP95` 0.0566 against 0.1055 ms (5 pairs, every attempt within budget): the panel's
      statistics, table text and timeline no longer grow with every registered stage. `EditFrameNested100k` 0.240
      against 0.287 ms and `PlayFrameNested100k` 0.244 against 0.295 (23 pairs, every attempt within budget): frames of
      a render scene without proxies record no GPU culling, occlusion or feedback.
    - Fixed, closed only after the campaign on the reference memory: `ThumbnailsMaxFrame` median 1.011 against 2.384 ms
      (15 pairs; 13 of 15 first attempts within budget, every run passed): released small scenes hand their GPU buffers
      to the next one, thumbnails draw through the CPU path and timestamp slots reset on the host.
    - Still open: `SceneOpenOver1MMaxFrame` median 1.231 against 4.177 ms (ratio 0.295, 23 pairs; 8 of 23 first
      attempts within budget, 4 runs over it in all three). Its 4-11 ms frames were wave 4's parallel read taking every
      processor at the frame's priority, so the driver's threads waited scheduling quanta (the read now runs on the
      background threads); the loaded scene's GPU copy is now made a frame before the swap. Frames of 1.2-6.5 ms remain
      in about half the opens, inside `Render.Submit`, while the background parse or the million entities' destruction
      runs. Re-measure on the reference memory first; then, in this order: contention between the parse's
      allocation-heavy jobs (a JSON DOM per entity on 20 threads) or the destruction's frees and the driver's
      allocations in `vkQueueSubmit` (timers around the submissions, a time-critical sampler; fewer allocations in
      `PrepareChunk`); the driver's thread on an efficiency core or a busy hyperthread sibling (an efficiency-QoS
      background pool, which changes import speed: the owner's call); play.stop's first frame (0.7-1.08 ms), whose
      scene binding sets a second cache entry keyed on every bound input would keep (about 0.1 ms); and a time-based
      window for `RenderSceneGPUCache`'s recycled buffers (300 frames are 0.3 s at the editor's rates, so an open's
      mirror usually gets fresh buffers, 0.4-1.2 ms).
  - Re-baselined by the owner's ruling, not a blocker: `Perf.Physics.Create10kBodies`, 16.50 ms against wave 3's 9.07 in
    the same pairs (budget 11.99, set on Jolt). Wave 5's box3d inserts every shape into its trees one at a time on the
    main thread, where Jolt built them in bulk; timed around its calls on this machine, box3d's own creation
    (`b3CreateBody`, the shapes, the mass) takes 11.4 of the 18.1 ms, more than wave 3's 9.07 for all of it, and
    Strata's part 6.7 ms (1.6 around those calls, 5.1 gathering the records and building the shapes on jobs). The owner
    on 2026-10-27: "I don't think physics object creation performance is a big deal. But i care about runtime
    performance" (`Docs/Plans/Wave7/specs/decisions.md`), so step 0 re-baselined it once at the campaign's median
    (`Measured` 15.76 ms, `Budget` 23.64; alone on step 0's tip 15.63-15.78) with Jolt's 7.995 ms as its `Target`. The
    way down stays bulk insertion with a later box3d bump, then Strata's allocations per body; wave 7's
    `physics-runtime` records it and does not optimize it.
  - Over budget in the loaded campaign only, within budget alone, entry unchanged:
    `Perf.EditorPanels.Streaming10kAssetsRefreshFrame` (0.1933 ms against 0.2205).
  - Unstable in the campaign (a spread above 25% of the median; entries unchanged, to measure alone):
    `Perf.Runtime.SceneSwitch100kMaxFrame`, `Perf.Jobs.WakeLatency`, `Perf.Editor.SaveMaxFrame250k` (its helper crashed
    once, 0xC0000005, in the campaign's second run and left no crash report; 220 runs since, of bc6ee123's and step 0's
    builds, on one project and on fresh ones, one and two at a time, did not crash), `Perf.Scene.CancelLoad1M`,
    `Perf.Runtime.QuitDuringLoad1M`, `Perf.Editor.SceneOpen10kPrefabInstancesMaxFrame`,
    `Perf.Physics.Boxes10k.ActiveFrame` and `.StrataStepTime`, `PerfGPU.Editor.ImportSettings4kMaxFrame` and
    `...10kCubes`, `SceneOpen100kMaxFrame`, `ExportMaxFrameStress`, `PerfGPU.Renderer.EditFrameCubes300k`,
    `Render300kCubes`, `PlayStartRender1MCubes`, `GPUTimerCost`, `Render100kCubesShadowed`, `Render300kCubesShadowed`,
    `AtmosphereSunMovingMaxFrame`, `ReflectionProbes16`, `PerfGPU.Streaming.MaxFinalizeTime`,
    `PerfGPU.Streaming.Mips.MaxFrameRatio` and `PerfGPU.GameUI.RenderIdle10k`. `PerfGPU.Renderer.Render1MCubes` has two
    states on every build, 0.27 and 0.55-0.65 ms: the campaign's runs met the first (its budget, 0.4275 ms, is theirs),
    its case run alone starts in the second and reaches the first by its third attempt.
  - Targets not met, kept below their new budgets (medians of 5): `Perf.Asset.FirstScan10k` 5,567 ms against 5,000,
    `PerfGPU.Editor.WorldOpen1MMaxFrame` 44.06 against 20.56, `Perf.Physics.Grow65kMaxFrame` 132.6 against 16.7 and
    `Create100kStaticBodies` 166.7 against 25 (box3d's insertion, above), `Perf.World.CellUnloadMaxFrame` 7.92 against
    4, `PerfGPU.Editor.EditFrameWorldIdle` 0.334 against 0.281 and `PerfGPU.World.CellDriveMaxFrame` 8.885 against 8.
  - Provisional until the integration's campaign: the Open World tour's three metrics (the campaign's tour ended early
    in two runs, fixed since: 12 seconds of game time; alone: work median 0.416 ms, GPU busy 0.669 ms) and step 0's
    `PerfGPU.Renderer.Render100kCubesShadowed4K` and `...High4K` with their GPU times.
  - The visual pass at 150% and at `--ui-scale 1` on a Windows display (CI builds and tests Debug on Windows since wave
    4, but shows nothing).
- Shaders in Slang (wave 8's lane Slang, S0.10; [Shaders](DeveloperGuide.md#shaders)):
  - Slang's SPIR-V has not run on a Mac's GPU: CI converts every module to Metal with MoltenVK's converter and compiles
    it, a lower bound of what MoltenVK does at run time. The 15 entries that read the bindless table are refused by the
    offline conversion whatever compiled them (`c_KnownFailures` of `.github/scripts/convert_shaders_to_metal.sh`:
    "Runtime sized variables must be in device storage argument buffers"); the first `ctest -L gpu` through MoltenVK is
    the release's Mac gate.
  - Besides NVIDIA's driver only Mesa's lavapipe has run Slang's modules, in a CI try of the lane (`ci/slang-mesa*`): 529
    of 553 GPU cases pass with Slang's modules and with glslang's alike, and the same 24 fail with both (editor viewport
    pictures and selection, on a CPU driver). AMD's and Intel's drivers have not run them; a permanent lavapipe job (its
    24 failures understood first) is later work.
  - Slang's modules are not yet measured to cost what glslang's did. The lane's GPU-time gate (interleaved A/B captures of
    the 4K look-dev 84-set, glslang's modules against Slang's) gave four readings, and in each the two A runs differed by
    more than its 3% rule allows, so none counts. B/A was 1.011, 1.004, 1.045 and 1.003 for `gpuBusyMs` (the 1.045 from
    a slow first B run), between 0.975 and 1.037 for `GPU.Opaque`, `GPU.Prepass` and `GPU.Post`, and between 0.87 and
    0.95 for `GPU.Shadows` (Slang's faster, the pictures identical); Medium at 1080p 1.014. A reading on a quiet machine
    settles it.
  - The converted files keep GLSL's stage variables as `st_*` globals that each entry point fills (`Include/Common.slang`'s
    last section): moving them into entry-point parameters, file by file, is later work (new shaders take parameters).
  - What a D3D-style target would still need: `SV_VulkanInstanceID`/`SV_VulkanVertexID` replaced, registers for the push
    constant blocks, set 1's register space, HLSL's constant buffer packing, the Forward variants' 3,600-byte
    `ReflectionProbeBuffer` element within DXC's 2,048.
  - A shader debug-information option (`-g`, for RenderDoc) would make shader outputs depend on the configuration.
  - Three findings about Slang 2026.20, recorded, not reported (filing them is the owner's call): it has no way to say
    `invariant` (the build patches the decoration); a `spirv_asm` `OpDecorate` is ignored without a word; `f32tof16`
    lowers to 16-bit types on SPIR-V (`Float16`, `Int16`), so `Include/Common.slang` wraps `PackHalf2x16`.
  - `IBL/BRDFLut.comp` writes its RG16F table with `shaderStorageImageExtendedFormats`, which device selection does not
    require (older than Slang; every desktop GPU and MoltenVK have it).
  - The main checkout's untracked `Strata/vendor/glslang` and `.git/modules/Strata/vendor/glslang` stay until 0.8.0
    ships (`.worktrees/wave8-base` and commits before glslang's removal build with them), then go with that worktree.

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
- **`animation`: Skeletal animation: skins, clips, animator states and cross-fades, GPU skinning, root motion,
  scripting.** Characters that move: glTF skins and animations import as skeletons, clips and skinned meshes,
  controllers blend states, the GPU skins with posed bounds, poses never become entities, and scripts and the editor
  drive and preview them. Depends on `render-scene`, `texture-cook-v2`, `module-foundation`.
- **`content-and-prefabs`: Content Browser v2, safe asset operations, dependency-aware async import, prefab basics,
  project thumbnails.** Make managing and reusing content professional. Depends on `editor-authoring`, `pack-v2`,
  `texture-cook-v2`.
- **`gameplay-and-scripting`: Character controllers, collision layers, gamepads, the game's window and save files, and
  scripts made, built, located and debugged from the editor.** The first physics experience and the scripting workflow
  people expect. Depends on `physics-at-scale`, `editor-authoring`.
- **`agent-activity`: Agent-native editor: no silent discards, autosave and recovery, activity journal and replay,
  agent presence, notifications, captures for agents, command contracts.** Make the AI-drivable editor visible and
  trustworthy. Depends on `editor-shell`, `editor-authoring`.

### Wave 4

Planned after five audits of wave 3's tree, by a person and by an AI agent each making a game, and by the rendering,
scale and streaming, and architecture lenses. The three streams planned here before stayed; the scale audit split
world-partition in two, and two streams joined for what people and agents meet first. Before they branched, main took
wave 3's finish work, the release packaging (0.3.0) and a CI that is green on Windows, Linux and macOS. Landed on
2026-10-19 with release 0.4.0, each stream merging when it was ready rather than in this order (Status, "Wave 4
landed", says what each delivered and what moved to later waves).

- **`engine-modules`: Engine modules: EngineModule lifecycle, per-layer libraries, module-owned components, a
  tooling-free runtime, faster builds, optional subsystems.** Turn the layer table into the build.
  - Subsystems (graphics, audio, ImGui, assets) start and stop through an EngineModule lifecycle, in dependency order,
    instead of code in Application (the allowlist from 8 exceptions to 1 or 0).
  - Each layer of `Layers.json` becomes its own static library, generated from the table, so a library links only what
    its layer may include.
  - StrataRuntime structurally excludes Dear ImGui, ImGuizmo, the asset pipeline and the network layer, with a CTest
    that checks it. Before this wave the runtime linked 425 KiB of ImGui, and every program started 48 threads.
  - Every module declares and registers its own components. Ordering constraints that name a system a build lacks are
    soft, and Audio, Physics, Animation and Scripting can be left out of a build.
  - Precompiled headers for the editor and test libraries and a sharded Core CTest (1,672 cases in one serial process
    before) cut the clean build and the test run.
  - Per-scene contexts moved to wave 5, after the renderer and scene loading this wave rewrites; new code reaches the
    asset manager through the scene.
  - Depends on `module-foundation`, `physics-at-scale`, `mip-streaming`, `scene-streaming`, `content-and-prefabs`,
    `animation`.
- **`world-io`: World I/O: saves off the frame, streamed and binary scenes, asset registration and imports at the scale
  of tens of thousands of assets.** Before this wave the paths around the frame failed before a world existed:
  - saving a million entities froze the editor for 22 s;
  - opening them peaked at about 4 GB;
  - registering a new asset cost 3-10 ms on the main thread (a 600-file import froze one frame for 3.6 s, and a fresh
    30,000-asset project timed out `StrataCLI launch`);
  - every import thread decoded a whole texture at once.

  What changes:
  - Saves snapshot the scene and write it in the background.
  - Scene documents stream in and out one entity at a time, and loads can be cancelled.
  - Exported scenes and prefabs are cooked into a binary form that loads straight into prototypes on workers (a
    million entities in under 1.5 s with under 300 MB of transient memory).
  - Registration's file work runs on I/O threads in batches, with automation answering while a first scan runs.
  - Imports run within a memory budget.
  - Pack version 3 bundles small entries (a 30,000-material pack is 10.7 times its content; moved to wave 5).

  Depends on `scene-streaming`, `pack-v2`, `content-and-prefabs`, `agent-activity`.
- **`world-partition`: Layered world partition: data layers, grid cells, streaming sources, physics at world scale.**
  Deliver the signature feature.
  - A world is authored as one World asset organized into named data layers and cut into grid cells. The editor
    streams cells around its camera, edits them in place and saves the cells that changed.
  - Games stream them additively around streaming sources, within per-frame budgets for creation, destruction, bodies
    and proxies.
  - A cook writes each cell with its bounds, body count and asset closure.
  - Physics is sized for the most bodies any streaming radius holds, and grows between steps when something still
    overflows, so a streamed body never lacks collision. Before this wave streamed bodies beyond the play-start
    capacity of 65,536 got none.
  - Cells unload in slices, so no frame frees a whole cell (51-56 ms before for 25,000 entities).
  - A world can be much larger than memory and than one ECS registry (1,048,575 entities) while frames stay within
    budget.
  - HLOD and origin rebasing come later.
  - Depends on `scene-streaming`, `render-scene`, `physics-at-scale`, `pack-v2`, `streaming-observatory`, `world-io`.
- **`gpu-driven-rendering`: GPU instruments, shadows at scale, shared geometry, GPU culling and indirect drawing,
  camera-relative rendering.** Scale rendering to millions of instances and thousands of distinct meshes.
  - GPU timer queries come first: frame stages, the Performance panel, `editor.perf`, `--perf-log` and GPU-time gates.
  - Shadow casters are found in parallel and culled below a texel. 100,000 shadowed cubes cost 6-7 ms of CPU before.
  - Meshes live in a shared geometry pool, so 10,000 distinct meshes no longer cost 37,000 draw calls and 20 ms.
  - Proxies mirrored on the GPU are frustum-, size- and occlusion-culled and LOD-selected per view and per shadow
    cascade, then drawn with indirect draws, skinned proxies included, so the CPU cost of drawing stays constant in
    instance count.
  - Visibility feedback keeps asset requests and mip streaming following what is seen.
  - Rendering is relative to the camera, so worlds stay precise tens of kilometers out.
  - Render statistics and GPU times reach agents through `viewport.capture` and `editor.perf`.
  - Depends on `render-scene`, `mip-streaming`, `animation`, `streaming-observatory`.
- **`game-ui`: Game UI: canvases, layout, widgets, focus navigation, script events, editor and agent authoring.** Give
  games menus, HUDs and settings screens. Before this wave the only UI was screen-space text sized in raw pixels.
  - Canvases have a reference resolution and safe areas. Rects have anchors and pivots.
  - Images (with nine-slice), text, buttons, toggles, sliders and layout groups are drawn in one batched pass.
  - Mouse, keyboard and gamepad focus navigation; script callbacks; unscaled time, so pause menus move.
  - A 2D canvas view in the editor and `ui.*` commands for agents.
  - Depends on `render-scene`, `gameplay-and-scripting`, `editor-authoring`.
- **`authoring-and-playtest`: Authoring fidelity and exact play-testing.** What people and agents make should behave
  as the editor shows it.
  - Skinned characters stand in their rest pose in edit mode, thumbnails and previews; whole classes of characters lay
    on their backs before.
  - A model's import scale reaches the instances already placed, and normal maps are recognized and checked.
  - Agents play-test exactly: `play.step` answers once its steps ran, input lasts game updates rather than render
    frames, and `input.sequence` drives whole input timelines.
  - Agents can query world transforms and bounds, and the running scene names itself.
  - The MCP server offers compact toolsets: before, 121 tools cost about 27,000 tokens, close to clients' 128-tool
    limit.
  - A Game view with fixed resolutions sits beside the Scene view while playing.
  - Prefab instances keep per-property overrides while every prefab change reaches them.
  - Small fixes go to what a person meets every day.
  - Depends on `animation`, `content-and-prefabs`, `gameplay-and-scripting`, `agent-activity`, `editor-authoring`.

### Wave 5

Asked for by the owner on 2026-10-20: "switch to box3d (https://github.com/erincatto/box3d) as its physics engine, and
include some more projects that you think could demonstrate this engine, you can also download assets online, current
sample project is too barebones". The two streams took the place of the wave-4 audits' list, which stays in "Later".
Each had a spec that an independent review corrected before work began (`Docs/Plans/Wave5/specs/`, removed when the
wave landed and kept in git history at 40fbb852); both branched from release 0.4.0 and ran at the same time on the Mac,
and `showcase-samples` landed first so that `physics-box3d` could check its samples on box3d before it merged. Landed
on 2026-10-20 with release 0.5.0 (Status, "Wave 5 landed", says what each delivered, and the open items what moved to
later waves). What earlier waves had moved to wave 5 (pack version 3, cold reads, the background-thread cap, per-scene
contexts, compute skinning, input actions) stays open, in the open items and "Later".

- **`showcase-samples`: Sample projects that show what Strata does.** Before this wave `Samples/` held only Tetris, a
  small scripted 2.5D game with built-in meshes and generated sounds: a person who opened the launcher saw nothing of
  the GPU-driven rendering, world streaming, physics, skinned animation, game UI, audio and export that waves 1-4 built.
  - Three samples were made through the editor the way Tetris was (StrataCLI, command scripts, C++ game scripts), with
    assets downloaded from sources with clear licenses: CC0 first, CC-BY 4.0 only with its attribution, within 25 MB
    of source assets and 20 MB of documents each.
  - The Physics Playground: about 600 bodies in stations of stacks, dominoes, a ball pit, triggers, collision layers,
    raycasts and impulses, with a tool panel and impact sounds.
  - The Third-Person Adventure: a skinned character with an animation controller, a character controller and a follow
    camera, triggers, several scenes, menus, save files and display settings.
  - The Open World: a streamed world of 20,000 instanced props on generated terrain with mesh colliders, animated foxes
    in streamed cells, day and night, and prefetching before teleports.
  - Each is played by a CTest that fails when the sample breaks, with exact waits in fixed steps (`until`), exported in
    full and pruned and run headless and in a window; `Samples.Content` checks their files, credits, licenses, links
    and budgets, and the launcher shows them with pictures.
- **`physics-box3d`: Strata's physics on box3d instead of Jolt.** Everything physics did on Jolt moved to box3d,
  behind the same interfaces.
  - box3d became the only physics library: Jolt went, with its submodule, options, notices and the tests that named it.
  - `PhysicsSystem`, `PhysicsWorld`, the scene services and the components kept their contracts, so scripts, audio,
    animation, world partition, the editor and the runtime changed only where they named Jolt's capacity model.
  - Every physics test kept its meaning, the determinism tests held, 0.4.0 scene files load unchanged, and the script
    ABI kept version 1.
  - Interleaved A/Bs against the Jolt build measured the port on the Mac: steps faster, bodies created and removed more
    slowly (Status).
  - New physics features (joints, height fields, double precision, recordings) were left to later streams, so that the
    port's diff stayed about parity and its regressions attributable.

### Wave 6

Asked for by the owner on 2026-10-22: "Start working on render-quality and project-settings", the two streams the
wave-4 audits ranked first. Each had a spec that an independent review corrected before work began
(`Docs/Plans/Wave6/specs/`, removed when the wave landed and kept in git history at ea66297b); both branched from
release 0.5.0 (6425b57b) and ran at the same time on the Mac, and `project-settings` landed first, so that
`render-quality` merged its version-3 projects, the samples' icons and its host functions before it landed. Landed on
2026-10-23 with release 0.6.0 (Status, "Wave 6 landed", says what each delivered, and the open items what moved to
later waves).

- **`render-quality`: the rendering of a production engine.** Ordered by dependency, then by what the owner sees, with
  a cut line after the core; everything below it landed too.
  - The core: motion vectors and TAA, with the editor's selection outline, captures and thumbnails steady under it;
    cascade blending; height fog; light units stated on every light, the sky and emission; shadows of point and spot
    lights on both draw paths.
  - Below the cut line: a pre-exposed HDR target with a physical atmosphere, reflection probes baked in the editor,
    screen-space reflections, the sun extracted from HDR skies, and clear coat and transmission in materials and glTF;
    and TAA's presets (Sharp, Original, Custom), which the owner asked for after seeing the first TAA.
  - Scenes render as in 0.5.0 unless they opt in, apart from three deliberate changes of defaults (cascade blending,
    the pre-exposure, `AutoExposureMaxEV` 16), and the script ABI is unchanged: scripts reach every new setting through
    components' properties.
- **`project-settings`: settings for the whole project.**
  - Project settings as sections that modules register, in a Project Settings panel through undoable commands, written
    off the main thread; preferences per user in a Preferences dialog.
  - Input actions with rebinding, with the game UI's navigation as their first map; the Third-Person Adventure reads
    its controls through them.
  - Collision layers for the whole project, migrated once from 0.5.0's scenes.
  - The game's title, version, company and icon in the exported game and its executable.

### Wave 7

Asked for by the owner on 2026-10-26, when the Windows reference machine came back with a new CPU and showed the engine
at 3840x2160: "the current image quality is simply not great. The shadows are very pixelated, maybe add more settings
to improve that"; never upscaling through TAA ("The blurry mess that current gen engine has (like unreal) is completely
the opposite of what this engine is planning to be"); "great realistic graphics, which doesn't mean sharp or clinical",
with the Fox Engine of Metal Gear Solid V as the baseline look; and a graphical showcase after the wave. Five audits
measured the image on the reference machine (shadows; anti-aliasing, tone and the camera; lighting and materials;
quality settings; architecture and cost) and planned the wave from their numbers: one shadow texel spanned 2.2 to 8.7
screen pixels, the soft-shadow filter left 4.6% noise without TAA and blurred contacts over three texels on a sun twice
its real size, every scene needed +1 EV because the tone curve showed middle gray at sRGB 91, the template's sky lit it
like an overcast day (sun to sky 1.6), rough metals lost up to 69% of their light, ambient occlusion and reflections
leaned on TAA, and no person, project or player could change any of it. Step 0 runs on main first: the GPU and Release
tests on NVIDIA for the first time since wave 3, with fixes for what fails there; room in AGENTS.md; the atmosphere's
black reflections after a sky switch; post-processing and ambient occlusion in files of their own; the `Renderer.Quality`
project section with Low, Medium (0.6.0's costs), High (the new default: 4096-texel cascades from 1440 rows) and Ultra
presets that every renderer follows and changes while it runs (`viewport.setSettings {quality}`, `viewport.capture
{quality}`, `StrataRuntime --quality`), with captures that match the views; a look-dev capture harness
(`Tools/LookDev.py`) with the 0.6.0 baseline; and the perf budgets measured again after the CPU replacement. The four
rendering streams branch from step 0's tip and merge in the order listed, each with a cut line below which its steps
move to "Later" if they did not land; a fifth, `physics-runtime`, added on 2026-10-27 after the owner's words on
physics, touches no renderer file and lands when it is ready, and a sixth, `editor-perf`, fixes the four editor
regressions of waves 4-6 that step 0 measured. What every rendering stream keeps: native resolution only,
nothing that needs TAA to look right (every screen-space effect has a spatial filter and is judged with FXAA and without
anti-aliasing too), nothing that costs anything when off, the CPU and GPU-driven paths and the shadow caches in
agreement, and Medium at 0.6.0's costs. The wave is accepted on the reference machine against the harness's baseline:
about one shadow texel per screen pixel where receivers are, no shadow noise without anti-aliasing, middle gray where a
camera puts it at Exposure 0, at most 5 ms of GPU time for each 3D sample at 3840x2160 on High, and Medium at 1920x1080
within 1.15 times step 0's.

- **`graphics-options`: graphics options for people, agents and players.** Quality is found where people look for it,
  set by agents with one command and changed by players in the game's options menu.
  - A Graphics Quality page in Project Settings: a preset picker and a table of every row by preset, Custom editable,
    with costs; a quality chip in the Scene view's toolbar.
  - `IGameGraphics`, an engine service beside `IGameWindow`, provided by the runtime and the editor's play mode; the
    player's choices saved by the engine in `GraphicsOptions.json` in the game's save directory.
  - Host functions appended for named options, with the SDK's `StrataScript/Graphics.h`, so the rows later waves add
    reach scripts without changing the ABI; `graphics.resetPlayerOptions` and `editor.status`'s `game.graphics`.
  - A Graphics row in the Third-Person Adventure's settings menu; Post Process grouped into sections in the Inspector,
    the sun's properties described with their units and linked to the page; `Docs/Manual/Graphics.md`.
  - Below the cut: a showcase budget class for samples (landed); supersampled stills (`viewport.capture {supersample}`)
    were not started and moved to "Later".
- **`sun-shadows`: shadows of a real sun.** At 3840x2160 the sun's shadows are as fine as the screen where receivers
  are, sharp at contact and softening by the sun's real size, the same in every frame without TAA, and grounded below
  the shadow map's resolution.
  - A `shadows` debug view of cascades and screen pixels per shadow texel, with statistics for agents.
  - A physical, deterministic filter: the penumbra at its physical width on a 0.53 degree sun (the new default), a fixed
    tent instead of rotated noise where the penumbra is narrow, a blocker search over a conservative depth pyramid that
    never misses thin casters, and wide penumbrae on a disk of 16 to 32 taps by quality.
  - The sun's shadow evaluated once per pixel in a screen-space pass after the depth prepass, with an edge-aware blur
    sized by the penumbra; transparent surfaces keep the per-pixel filter.
  - Contact shadows in that pass (`ContactShadowLength`, on at High and Ultra).
  - Cascades fitted to what the camera sees (SDSM): to the depth range and the receivers' extent, with a per-light split
    distribution.
  - Landed: at 3840x2160 on High the samples' scene cameras and editor views have a median of 0.31-1.35 screen pixels
    per shadow texel, none above 4 (0.6.0: 2.2 at splits, 5.5-8.7 at near ends); the same capture twice gives the same
    pixels without TAA; the contact strip under the look-dev scene's 5 cm plate at 2.6 m gets at most half the sun on
    92.5% of its points without anti-aliasing (a solid contact shadow; since 0.8.0 contact shadows are off in every
    preset and the fitted cascades give the strip, still solid at High, its edge showing the shadow map's texel steps,
    about ten teeth at 3840x2160, as Medium's did in 0.7.0); the penumbra's grain is 0.95% by the RMS of its
    second difference (the integration's metric; the shadows audit's 9-pixel moving average reads 1.6% in every
    anti-aliasing mode, the profile of a physically narrow penumbra). Its cost is the wave's open item below.
  - Below the cut, not started and moved to "Later": sized point and spot lights with the same filter, up to 8
    cascades, distant shadows for open worlds, 16-bit depth for far cascades, and cascades kept while the camera moves.
- **`camera-and-tone`: the image of a camera.** What the renderer shows reads like a photograph from a good camera,
  restrained by default.
  - A dithered 8-bit output (no banding) and FXAA that keeps more texture.
  - Filmic, a tone curve of Strata's own that rolls highlights to white without skewing hues, calibrated so that new
    projects and the samples need no +1 EV; white balance and slope, offset and power.
  - Exposure that adapts like an eye (brighter faster than darker, center-weighted metering, a key that keeps interiors
    darker than sunlight), bloom of the same size at every resolution, a lens's vignette.
  - Motion blur of the camera and of moving objects, with quality rows and players' comfort switches. It ships off in
    the template and the samples ("when in doubt, off"): the owner's before/after sheet, with the full criteria table
    of the live shots (failing edges included), decides before it is turned on anywhere.
  - Landed: the widest run of one 8-bit code in the Physics Playground's sky is 4-5 pixels (step 0: 99-113); Filmic
    shows a metered middle gray at sRGB 117.6 at Exposure 0 (ACES: 91) with hue drifts of at most 7.95 in ICtCp (ACES:
    23-29); bloom keeps its size and energy within 10% from 1080 to 2160 rows. The template and the samples take their
    exposure by the median rule (the integration ruling, in place of the plan's Exposure 0 for the template): measured
    on the integrated Release build, the template 0.188 at +0.25 (0.148 at 0), the Third-Person course 0.178 and its
    menu 0.171 at 0, the Physics Playground 0.187 at -0.25 and the Open World 0.161 at 0, all within [0.15, 0.22]. Not
    met: FXAA keeps 0.87 of the texture detail without anti-aliasing (target 0.88, at the spec's fallback subpixel blend
    0.25) and the dither costs 0.021 ms at 3840x2160 (target 0.01).
  - Below the cut (landed): depth of field with a physical lens, film grain and chromatic aberration, a morphological
    anti-aliasing of Strata's own, 3D LUTs and lens flares.
- **`light-and-materials`: believable light.** A new project opens sunny with a real sun to sky ratio, rough metals keep
  their energy, occlusion grounds every object without TAA, highlights hold at 4K without sparkle, and distance fades
  into the atmosphere's haze.
  - Lighting debug views (lighting only, ambient occlusion, indirect light, luminance in EV, albedo range).
  - Multiple-scattering energy compensation, checked by a white-furnace test.
  - GTAO by quality, denoised spatially without TAA, with a 1 m radius and multi-bounce.
  - The template under the physical atmosphere with a 120,000 lux sun, sun and sky presets (`lighting.applySkyPreset`),
    the samples' skies and suns balanced and the Open World's leaves corrected.
  - Specular anti-aliasing, a 512 prefiltered sky at High, and aerial perspective from the atmosphere.
  - Landed: a 0.95 metal reflects 0.955-0.991 of its color in the white furnace from roughness 0 to 1 (0.6.0: 0.31 at
    1); the template and the Physics Playground light flat ground with a sun to sky ratio of 8.1 (0.6.0: 1.6); ambient
    occlusion shows no pattern with FXAA or without anti-aliasing, and at Medium costs 0.84-1.09 times step 0's; the
    Open World's leaves have an albedo of 0.45; `GPU.Aerial` takes 0.01 ms. The Open World's opaque pass grows
    0.053-0.058 ms at 3840x2160 on High with the always-on shading (+0.049 at Medium, against 0.05): re-measured in the
    campaign.
  - Below the cut: screen-space reflections without TAA (landed: a full-resolution spatial trace and resolve at High and
    Ultra, the `Reflections` row); volumetric fog and light shafts, a baked irradiance volume for sky visibility and
    bounce light, bent normals with cone specular occlusion and "Fit to Room", and alpha cutouts that hold at distance
    were not started and moved to "Later".
- **`physics-runtime`: physics that runs fast and settles.** The owner on 2026-10-27: "I don't think physics object
  creation performance is a big deal. But i care about runtime performance. For large box3d physics be sure to use
  box3d's contact recycling."
  - Strata's part of a fixed step from 0.60, 0.97 and 0.74 of box3d's own step (10,000 boxes, 30,000 with tracked
    contacts, 70,000) to at most 0.45, 0.75 and 0.55; the busiest frame of 70,000 falling boxes from 16.05 to at most
    13.5 ms, 1,000 walking characters from 0.86 to at most 0.70 ms a step, and sleeping bodies at no measurable cost.
    Landed: the ratios fell to 0.39, 0.66 and 0.47 in the verifier's A/B on the loaded machine (the integration campaign
    confirms the milliseconds); the characters stayed at about 0.89 ms and 70,000 sleeping boxes still make box3d's own
    step 4 to 7 times slower, both in the open items ("Physics at runtime").
  - Contact recycling at a distance chosen by measurement on piles, towers and the Physics Playground (each reading of
    the owner's "0.7" answered with numbers); the distance and the sleep threshold in a `Physics.Simulation` project
    section, changed while the game plays; character controllers' bodies and kinematic bodies never recycle contacts.
    Landed with box3d's 5 cm kept (its ghost cost accepted at integration and bounded by `Physics.Settling`) and a sleep
    threshold of 0.08 m/s.
  - No physics hitch while playing beyond creating bodies: of the frame that brings a world to 65,536 bodies, at most
    4 ms grow its storage; Strata's part of a cell's unload at most 1.25 times box3d's own removal. Landed: Strata's
    part of the unload is at most 0.4 times box3d's, and with parked bodies the unload's frame fell from 7.8 to 3.4 ms
    in the verifier's A/B; the growth measured 12 ms within a 30 ms spread, so it is not resolved from noise.
  - Sleeping bodies, islands, contacts, box3d's stage times and how piles settle in `physics.stats`, `editor.status`,
    the runtime's perf log, the Streaming panel and the perf lab.
  - Creating and loading bodies recorded with box3d's share, not optimized (the owner's ruling).
  - Below the cut: deferred removal of an unloading cell's static bodies (landed: they are parked, then disabled in
    slices); the scene's world matrices from physics, a per-scene override of the section and requests to box3d upstream
    were not started and stay in the open items ("Physics at runtime"); rolling resistance was not needed (the piles
    sleep with the 0.08 m/s threshold).
- **`editor-perf`: the editor regressions of waves 4-6.** Added on 2026-10-27 from step 0's measurements: four editor
  metrics over budget against wave 3's build (the open items' "Blockers of 0.7.0"), each taken on without removing a
  feature, with its numbers there.
  - The Performance panel's statistics, table text and timeline no longer grow with every registered stage (landed).
  - Thumbnails: a released small scene's GPU buffers go to the next scene, thumbnails draw through the CPU path with the
    same pixels, and GPU timestamp slots reset on the host instead of in a submission per frame (landed; the metric is
    closed after the campaign on the reference memory).
  - A render scene without proxies records no GPU culling, occlusion or feedback (the editor's frames of 100,000 nested
    entities, landed; `renderStats.gpuCulled`).
  - A scene asset's parallel read runs on the background threads, and a loaded scene's GPU copy is made a frame before
    it is shown (landed). `SceneOpenOver1MMaxFrame` fell to 0.295 of step 0's but stays over its budget in about half
    the opens: open, with the next hypotheses in the open items.
- **Open at the integration (2026-10-28),** with the integration's rulings (`Docs/Plans/Wave7/specs/decisions.md`):
  - The wave's perf pass and campaign. The pass (interleaved look-dev A/Bs against cc9524a1, three pairs, medians, the
    samples' scene cameras with TAA): the sun's shadow comparisons take level 0 by LOD instead of zero gradients, and the
    contact march takes one step at a time, neither changing a pixel. `GPU.ScreenShadows` went from 0.11-0.16 to
    0.06-0.10 ms at 1920x1080 Medium and from 0.53-0.83 to 0.35-0.52 ms at 3840x2160 High; GPU busy at Medium 1080 from
    1.20-1.32 to 1.10-1.20 times step 0's (Third-Person 1.10, Open World 1.15, Physics Playground 1.20). Still open, with
    their causes:
    - The Physics Playground at 1920x1080 Medium: 1.20 times step 0's GPU busy time (0.590 against 0.491 ms; target 1.15).
      The mask costs 0.062 ms (evaluation 0.027, blur 0.024, the fit's reduction 0.011), the post chain 0.014 more
      (`camera-and-tone`), aerial perspective 0.009 (a fixed cost: each of 1,024 threads marches 64 segments in turn) and
      ambient occlusion 0.008 more (`light-and-materials`' denoiser).
    - `GPU.ScreenShadows` in the Third-Person view at 3840x2160 High: 0.52 ms (target 0.45; the Open World 0.35, the
      Playground 0.39). The evaluation takes 0.20 ms (the disk's 24 taps and the range cells' loop; pixels in blend bands
      sample two cascades), contact shadows 0.20 (12 steps), the blur 0.10, the reduction 0.02.
    - Next levers: the blur as compute passes that read depth, normals and the mask once per tile (the fragment passes
      read them again for every tap); the aerial volume integrated per slice in parallel; High's contact shadows at 10
      steps (about 0.02 ms at 4K, an image change to judge); the reduction's clear, dispatch and copy. Measured and
      dropped: gathering the range cells four at a time (slower), writing only the mask pixels a pass changes (no gain),
      the reduction beside the mask's passes (graphics and compute work on one queue do not overlap).
    - Met: the Open World's `GPU.Opaque` at 3840x2160 is 0.020 ms (Medium) and 0.032 ms (High) below step 0's for the
      whole wave (the mask's saving holds `light-and-materials`' shading); `PerfGPU.Renderer.Post4K` 0.249 ms (target 0.25).
    The campaign (`PerfStability.py --runs 5`, on the reference memory) then measures the streams' provisional metrics, the
    acceptance of plan section 1.1 (High at 3840x2160 measured 1.84-2.59 ms of GPU busy time after `sun-shadows`,
    against 5 ms), the Open World's opaque pass at High, and the budgets that may go down
    (`Performance600FramesP95`, `ThumbnailsMaxFrame`, `PerfGPU.Renderer.GPUTimerCost`).
  - Penumbra noise is recorded with both metrics: 1.6% by the 9-pixel moving average, 0.95% by the RMS of the second
    difference across the penumbra; the 1% target is met by the second, the grain metric.
  - Motion blur ships off everywhere, pending the owner's judgment on the before/after sheet with the full criteria
    table of the live shots: 3 of 8 measured edges within 15% of the model's width (0.991, 1.012, 1.073), the others at
    0.525, 2.508, 1.464, 0.683 and 0.556 times it, and a fast fence post's noise 0.047 RMS against 0.028; the robot
    keeps its detail (1.0003 and 0.993). `MotionBlur4K` is 0.178 ms (target 0.30). No Motion Blur row in the
    Third-Person menu while it ships off.
  - The editor: `SceneOpenOver1MMaxFrame` stays open (median 1.231 against 4.177 ms in step 0's build, budget 1.143;
    the next hypotheses, in order, are in the open items' "Blockers of 0.7.0": allocation contention between the parse
    or the destruction and the driver's `vkQueueSubmit`, the driver's thread on an efficiency core, play.stop's binding
    sets, a time-based window for recycled buffers); `ThumbnailsMaxFrame` (median 1.011 against 2.384 ms) is closed
    only after the campaign on the reference memory. Borderline and older than the wave:
    `Perf.EditorPanels.Streaming10kAssetsRefreshFrame`, `Perf.Runtime.SceneSwitch100kMaxFrame`,
    `Perf.Editor.HierarchyPanelFrame100kRoots`; a cooked scene's parallel decode still runs at the frame's priority.
  - Physics: `Characters1000Step` measured 0.889 ms against its 0.70 target (the characters' prelude is 0.05 of the
    step), and `Sleeping70k.StepRatio` 4.4-7.3 against 1.2 (box3d's own cost of sleeping proxies in its dynamic tree, an
    upstream request); the millisecond targets of the step (`Boxes70k.StrataStepTime` 4.5 ms, projected 3.85;
    `Boxes30kTracked.StrataStepTime` 2.3, projected 2.17; `Boxes70k.ActiveFrame` 13.5, projected 12.6) and
    `CellUnloadMaxFrame`'s 4 ms are confirmed by the campaign, which also sets `BallPit2k.SettleCpuMs`'s target at 0.8
    times its measurement. The recycle distance stays box3d's 5 cm by ruling: the sweep's rule had no valid outcome
    (2.5 cm, the only distance through both gates, sleeps the 2,000-ball pit 8 s later and keeps a 47 mm bump on
    triangle meshes that 5 cm removes), and 5 cm's ghost cost, seams 5.0 mm and ledges 1.7 cm over distance 0, is
    accepted and bounded by `Physics.Settling` at 1.25 times the measured cost.
  - Platforms: wave 7 was built and tested on Windows with NVIDIA only. Its GPU suites have not run on the Mac
    (MoltenVK) yet: they run there before the release, or the release notes and these items say they did not. The
    integrated branch's CI is the first build of the wave on Linux GCC, macOS Apple Clang with libc++ and the
    configuration without physics (the local lint ran Clang against libstdc++).
  - Smaller: morphological anti-aliasing keeps 0.88 of the texture detail (target 0.95); `Post4K` sits at its 0.25 ms
    provisional target; contact shadows' 1-pixel fringes beside thin foliage moved to "Later" with their replacement;
    a face exactly parallel to the sun gets a visibility of 0.5 (unseen: such faces get no direct sun); the Open World
    scene's captures with TAA differ between repeats (also in 0.6.0's); the ambient occlusion debug view shows dashed
    streaks at High that the lit image does not; the Graphics Quality page was checked by the UI harness, not yet by
    eye in an editor window.

### Wave 8

Asked for by the owner on 2026-11-01, after judging 0.7.0: "Make prioritization yourself, but showcase needs to land
this wave, you can update the showcase later if there are more things", with fifteen items, and the same day Esoterica
as the engine the animation system "should be based on". Eleven audits measured each item on 0.7.0 and planned the
wave from their numbers: a ball passing the camera re-cut every sun cascade and sent half the receivers to a cascade 3
to 4.5 times coarser for up to 354 ms; contact shadows shimmered from binary hits in a thin shell; the specular shading
held up in stills, but screen-space reflections left a fixed screen door on metal at every quality; a plain unlock of
shadowed lights left some light without a shadow in 28% to 57% of frames; a character moved on 60 Hz steps under a
camera sliding every frame, a 29 pixel sawtooth; all 103 shaders went through a Slang spike with 553 of 554 GPU test
cases passing, as 0.7.0's build does; two objects reference GLFW; the owner's three metal and wet shots already work on
0.7.0 as highlights on blocks, with the right sun and materials. Plan: `Docs/Plans/Wave8/plan.md`.

Wave 7 is the only measured capacity, and its integration was cut short at 99% of the owner's weekly usage, so this wave
runs in tranches gated on usage. Tranche 1 is committed and ends with landing 1, after which main is releasable.
Tranche 2 is intended. Tranche 3 is wave 9 unless usage remains. Each landing keeps a quarter of a week's usage, and no
integration starts without it.

**Step 0** ran on main first, in parallel lanes that landed one by one (2026-11-02):

- Lane Render: contact shadows off in every Graphics Quality preset (the row stays for Custom; `ScreenShadows4KHigh`
  went from 0.2335 to 0.1761 ms in three interleaved pairs against 0.7.0, Medium's unchanged). The sun cascades' fit
  counts seconds, and its near end follows a near object only once it has stayed for 0.5 s (0.12 s while the camera or
  the sun moves); receivers holding a tenth of the receiver pixels, the nearest transparent surface and narrow or
  orthographic views move it at once, and every bound shrinks only after 0.5 s. The Physics Playground's ball, 6.85% of
  the pixels at its spawn, pulled the near end from 11.31 to 0.84 m and sent half the receivers to a cascade 3.2 to 4.5
  times coarser for 116 to 354 ms; it now leaves the layout alone at 60, 240 and 1000 frames per second. Captures and
  thumbnails keep 0.7.0's fit rules.
- Lane Editor: `scene.raycast`, `asset.place` and a box ghost where a prefab, model or mesh dragged from the Content
  Browser will land; the editor's frame rate preferences (idle timeout with 0 for never, idle and background rates, a
  cap while busy), with a frame pacer that waits on a high-resolution timer (a 120 Hz cap had run 20% slow); a pause in
  the Performance panel that holds the frame breakdown still while the profiler keeps recording.
- Lane Platform: the project's `Scene.Timing` section with `FixedRate` (30, 60, 120 or 240 steps a second, or each
  scene's own); GLFW sealed behind `Window` by the layer table's `ExternalOwners`, with a build without it on CI; why
  FIFO does not pace the editor: on the reference machine vsync paces no window in windowed mode, focused or not, a
  driver or system setting not yet confirmed (open items).
- Lane Slang landed within its box: every shader in Slang's HLSL syntax through slangc 2026.20, pinned by SHA-256 in a
  per-user cache, with pictures that differ from 0.7.0's by rounding only; CI checks every module with spirv-val and
  converts it to Metal, and Mesa's lavapipe passes 529 of 553 GPU cases with the new modules as with the old. Its
  GPU-time gate got no reading within its noise rule (open items).
- Last: the forward shader's indirect light split into a diffuse and a specular gather over shared inputs
  (`Include/IndirectLight.slang`), so that `reflections` and `radiance-cascades` each change their own: every picture of
  the 4K look-dev 84-set is identical to its base's (the two Open World TAA shots that differed between the first base
  and tip captures are identical in interleaved base, tip, base, tip captures); wave 8's scene slots, GPU stages and
  debug views assigned; this entry.

What every rendering stream keeps from wave 7: native resolution only, nothing that needs TAA to look right (Medium's
Checkerboard reflections are the named exception), nothing that costs anything when off, the two draw paths and the
shadow caches in agreement, and Medium at 0.6.0's costs. Wave 7 rejected screen-space global illumination for needing
temporal accumulation; the cascades here accumulate nothing and keep no history buffer. Their input from the past is
the previous frame's lit color, as screen-space reflections' is.

**Tranche 1:**

- **`showcase`: High Desert Outpost.** A derelict fuel stop in a rocky high-desert valley the morning after rain, made
  through the editor from Poly Haven's CC0 scans and committed generators (no bus or oil tank exists in the sources),
  72.2 MB of source assets in the 80 MB class. Six shots with a camera tour between them: a dawn vista, the side of a
  weathered steel bus, an oil tank at low sun, the wet apron, the hut's interior, dusk to night. Three baked stations of
  the sun on six probe entities, a first-person walk and a photo mode. It needs nothing from the streams: each has a
  stated fallback and an adoption pass when it lands, and its graybox checks all six shots on main before the dressing
  starts. `PerfGPU.Samples.Showcase.{High,Ultra}`, `showcase.*` look-dev shots, `StrataEditor.Showcase`.
- **`reflections`: metal and wet ground.** At High and Ultra: a direction-aware edge fade, a resolve that cancels the
  screen door, hits validated after the march, specular occlusion off screen-space hits, the misplaced probe patch,
  landing alone before the showcase tunes its materials; then F0 from the IOR, the night sky's bands, 256-texel probe
  slots. Proposed targets: under 1% noise in a sheet's reflection (3.5-5.0% today) and at most 1% flicker in motion.
  Highlights lead R2 (the owner: "highlights are extremely important just like in a drawing, because they should be
  cheap"): the sun's size in specular, lantern and fire highlights on metal and wet ground, and a source radius for
  highlights only, each judged with no AA, FXAA and TAA. Below the cut: keyframed probe captures, hit-distance blur, a
  wet-surface model, normal-map-aware filtering, anisotropy, SSR on clear coat.
- **`physics-timing`: smooth at any frame rate.** Drawn transforms between the last two fixed poses, on in games and
  editor Play, with the simulation untouched: roots blend and what hangs under them composes, so attachments stay on
  their joints. Target: at most 1 pixel of sawtooth from 30 to 1000 fps (29.4 at 702 fps today). Stepping with the
  frame's delta is rejected on its measured cost and repeatability.
- **`radiance-cascades`, through its gate.** The Path of Exile 2 technique, adapted for a free camera from its published
  paper: image-plane wedges with an elevation bitmask, a spatial filter, with today's probes and sky for what the screen
  does not show. Numeric gate G1 behind a debug view: at most 1.2 and 2.5 times `GPU.AO` at 3840x2160, at most 0.5%
  flicker over blocks, 1% on foliage, with a degrade path that is expected. A foliage miss alone does not stop the join.

**Tranche 2:**

- **`animation-graph`: the animation graph, first slice.** First claim on the tranche. Strata's own module on
  Esoterica's architecture, reading Esoterica's source at c99a946 and porting its runtime semantics under its MIT notice
  (the owner allowed open-source code on 2026-11-01): pose tasks apart from graph update, sync tracks, Blend1D, masked
  layers and state machines as nodes, authored by agents in `.stanimator` version 2 and proved on the Third-Person
  robot, whose walk and run are 27% of a cycle apart in their cross-fade today. No graph canvas yet: wave 9.
- **`radiance-cascades`: join and hardening.** Shading from the gathers, a settle stat for captures, and the owner's live
  look in motion before any sample adopts it. Below the cut: the Ultra layout, bent direction, and a baked irradiance
  volume, which moves above the cut if G1 fails.
- **`local-lights`: many shadowed lights.** No fixed limit, 64 at High and 256 at Ultra at the same atlas sizes; tiles
  that keep their shadow while they resize, no global layouts, a scheduler that starves nothing, a grid for change
  tracking, the filter on fixed noise. Below the cut: a time governor.

**Tranche 3, wave 9 unless usage remains:**

- **`editor-ux`: the rest of drag and drop.** A mesh ghost in an overlay pass and a live material preview. Below the
  cut: a per-mesh ray structure, pause on spike, HDRI drops.
- **`volumetric`: fog with shafts and cones.** A froxel grid without history that reads shadows coarsely, lit indoors by
  probes, with point and spot lights. It joins only with its cones.
- **`showcase-engine`: layers on materials, stills.** One blended material layer with wetness on both draw paths. Below
  the cut: supersampled stills.

### Later (wave 9 and beyond)

The owner's direction for rendering (2026-10-26): realistic, filmic images at native resolution, with the Fox Engine of
Metal Gear Solid V as the baseline look; neither the smear of temporal upscaling ("The blurry mess that current gen
engine has (like unreal) is completely the opposite of what this engine is planning to be") nor a sharp, clinical image.
Strata never renders below native resolution: no render scaling below 1, no dynamic resolution, no TAA or other temporal
upscaling, and nothing may need temporal accumulation to look right (TAA stays one anti-aliasing choice).

- **Wave 9, recorded in wave 8's step 0** (`Docs/Plans/Wave8/plan.md`, section 11). These stand whatever happens to
  `animation-graph`:
  - **`animation-tools`:** the node canvas through `EditorPanelRegistry` with every edit a command, live debugging and a
    task view, a clip timeline for events and sync markers, a mask editor, recording.
  - **`animation-runtime-2`:** Blend2D with Strata's own triangulation, selectors, referenced and external graphs,
    variations, typed clip events, additive clips, float curves, two-bone and foot IK grounded through
    `IPhysicsQueries`, warps, forced transitions, skeleton LOD, compressed clips if measured to help, and whether version
    1 controllers move onto the graph path.
  - **Ragdoll nodes,** after a physics joints stream.
  - **Skin and a character** (the owner's subsurface scattering): pre-integrated skin, thin translucency if `volumetric`
    did not land it, screen-space scattering only for close-ups of faces, and a distant figure in the showcase if a
    realistic CC0 or CC-BY body is found.
  - What tranche 3 and the streams' cut lines leave.
- **Rendering after wave 8:** what fell below wave 7's cut lines (supersampled stills,
  `viewport.capture {supersample: 2}`: rendered at twice the size and filtered down with the output's dither, for the
  showcase's screenshots; point and spot lights with a source radius and the sun's physical filter, up to 8 sun
  cascades, distant shadows for open worlds, 16-bit depth for far cascades, per-cascade cache keys that keep cascades
  while the camera moves; bent normals with cone specular occlusion and reflection probes' "Fit to Room",
  coverage-preserving alpha mips for cutouts; the Open World's real sun and moon values, its DayNight's 3 lux sun and
  0.05 lux moon under automatic exposure; motion blur in the template and the samples once the owner's sheet clears it);
  a replacement for contact shadows, off in every preset since 0.8.0 (a crossing test instead of a thin shell, noise
  not locked to the screen, look-dev in every sample; 0.7.0's could also leave 1-pixel fringes beside thin foliage);
  gap-aware cascade slices for a near object that stays (with hysteresis of their own: since 0.8.0 such an object still
  re-fits the cascades once); the Physics Playground's empty first cascade (its nearest receiver at 18.9 m, the first
  split at 18.66 m); MSAA; virtual shadow maps if the shadows debug view shows cascades falling short; frame-level
  variants of the forward shader if `GPU.Opaque` shows the need; more shadowed lights with occlusion culling of their
  views; run-time probe capture for moving suns; render scale above 1 for live views; area lights; the rest of the open
  items' list.
- **Sunlight thrown off bright metal onto nearby surfaces** (the owner wants it, "not a priority"), planned after
  `radiance-cascades` lands, its technique chosen then.
- **Physics after wave 7:** what fell below `physics-runtime`'s cut line or did not land ("Physics at runtime" in the
  open items has the measurements): the scene's world matrices written from physics (a Scene API whose cache contract
  the Scene module decides), a per-scene override of `Physics.Simulation`, poses written back from box3d's move events
  if a later measurement shows a gain, and requests to box3d upstream (a settable recycle angle, a run-time reserve,
  bulk insertion and removal, sleeping proxies kept apart from moving ones).
- **`game-shell`:**
  - Crash reports in shipped games.
  - Exports per platform and configuration, with a macOS app bundle.
  - Texture quality and the other options wave 7's graphics options leave to later.
- **`manual-and-samples`:** a people's manual reachable from the Help menu and playable templates (the 3D samples came
  with wave 5; what they left open is in Status).
- **`scene-contexts`:** per-scene asset managers and script engines instead of process-wide active ones.
- **`vfx`:** particles, trails and decals on the GPU-driven path.
- **`mesh-streaming`:** Mesh v2 with quantized vertices, per-LOD streaming on the geometry pool, and a collision
  sub-asset, so physics stops depending on the renderer's Mesh.
- **HLOD:** merged, simplified cells with baked atlases for distant parts of a world.
- **`world-environment`:** terrain cooked into world cells, with height-field colliders (box3d's height-field shapes)
  and sculpt and paint tools.
- **Animation authoring:** wave 9's `animation-tools` and `animation-runtime-2` above.
- **Exported-game automation:** command scripts, simulated input and expectations in StrataRuntime.
- **Interpolation, the rest (below `physics-timing`'s cut in wave 8):** world-space game UI and editor gizmos and picking
  on drawn poses; reflection probes, fog and post-process volumes, the sky and the sun on drawn poses; attachments placed
  outside their animator's hierarchy following its drawn pose; a cap on interpolated bodies and parallel proxy placement,
  if a real scene's per-frame proxy work caps its frame rate.

Further:
- JobSystem v2 with work stealing and a frame task graph, once `Perf.Jobs` shows a need.
- 64-bit entity handles and origin rebasing.
- Per-world audio listeners and input contexts.
- Native game modules and an exported engine package.
- A console port: script modules linked statically (no `DynamicLibrary` path exists), crash containment without SEH or
  signals, the console's window and present behind `Window` and `GraphicsDevice`, file, thread and suspend behavior;
  DeveloperGuide.md's "Platform seams" lists the seams.
- Audio streaming from packs, and an audio mixer with buses and effects.
- Localization with string tables and font fallback chains.
- BC6H.
- ImGui multi-viewports.
- Physics joints, ragdolls and vehicles, on box3d's joints (revolute, prismatic, spherical, weld, wheel, distance,
  motor): components, gizmos, a script API and the feature test, in a stream of their own now that physics runs on
  box3d.
- Triggers that meet triangle-mesh colliders (box3d's sensors skip meshes), and box3d's recordings and replays as an
  editor debugging tool.
- Level-design tools beyond placement: orthographic views, surface and vertex snapping, hiding and locking in the editor
  only.
- OBJ and FBX import.
- Morph targets.
- A prefab edit mode.
- A timestep display and other editor polish.
