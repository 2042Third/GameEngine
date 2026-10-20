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

Open items and deferrals later waves own:

- GPU-driven rendering (wave 4's `gpu-driven-rendering`): skinning still runs per draw (compute skinning once per
  frame, measured by a GPU-time metric, is wave 5's); the shadow cascade cache follows the GPU-driven path only (the
  CPU path redraws every layer each frame); render scenes are copied whole at play start, not shared by copy-on-write
  pages; on tile-based drivers a timestamp inside a render pass may land at the pass's end, so `GPU.UI` is checked
  for presence, not for its time.
- Tests: `Editor.ImportSettings`' close check (2.0 s) measured 2.012 s once on CI's Windows Release runner;
  `Editor.UI`'s "No uploads in the last 120 frames" fails when GPU suites ran before it in the same process (CTest's
  shards never combine them).
- World I/O (wave 4's `world-io`): pack version 3 (small entries bundled, a zstd dictionary, shared blocks decoded
  once; the 10.7x inflation of small assets in `Perf.Asset.Pack30kMaterialsRatio`), cold reads without the system
  cache with 16 outstanding reads (`PerfGPU.Pack.ColdStreamStressWorld`) and a background-thread cap while loads run
  are wave 5's. Additive loads have no prototype form yet (`SceneLoadOptions::PrepareAs`,
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
- Flaky tests seen once, not reproduced since: Windows `CrashGuard`'s "repeated faults on other threads" and the
  Mac's `GPU.AssetResidency` "Arrivals that need room...".
- Game UI (wave 4's `game-ui`): world-space canvases draw unlit, after post-processing, and the game's hit tests on
  them ignore scene geometry in front (the editor's picks check meshes); UI action bindings use the engine's input
  names (`Key/KPEnter`, `Gamepad/A`) until an input-actions asset names actions (wave 5).
- Optional subsystems: a build without the scripting module (`STRATA_WITH_SCRIPTING`) is not offered yet, because
  `GameRuntime`, the editor and many tests use the script engine directly; physics, audio and animation can be left
  out (`STRATA_WITH_PHYSICS`, `STRATA_WITH_AUDIO`, `STRATA_WITH_ANIMATION`, wave 4's `engine-modules`).
- Physics: dynamic bodies below other dynamic bodies are still written back one by one; a collider built from a mesh
  that is reimported while evicted keeps the old data until the mesh loads again (the source revisions render-scene uses
  would tell).
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
- Waiting for the Windows reference machine (i7-13700K, RTX 5090), away since 2026-10-13 while work goes on on a Mac
  (Apple M2 Max, MoltenVK): nothing here may be decided from the Mac's numbers.
  - Budgets: every `Budgets.json` entry stays the reference machine's; a change on the Mac that makes a metric cheaper
    lists it here, measured A/B on the Mac, until the reference machine confirms it. Wave 3's finishing round left three
    perf findings for it: `RuntimeMaxFinalizeStage` (its budget lies below the engine's designed frame-0 cost),
    `ThumbnailsMaxFrame` (redefined with a cold first attempt, its `Measured` not taken again) and
    `SceneOpenOver1MMaxFrame` (still over its budget in some runs).
  - GPU tests on Windows and NVIDIA: CI runs no test of the `gpu` label, so renderer changes are validated on the Mac
    alone until `ctest --preset windows-debug` runs under the validation layer there again (drivers differ: NVIDIA has
    `drawIndirectCount`, a transfer queue and min/max samplers, which MoltenVK lacks).
  - Wave 4's `engine-modules`: the `Perf.Jobs` metrics are provisional (the Mac measured `SubmitWait` 3.14 us,
    `SubmitMany` 0.41 us, `ParallelForOverhead` 95.9 us and `WakeLatency` 19.6 us), and its throughput targets are
    stated in the reference machine's numbers: a clean Debug build 30% below 3,099 CPU-seconds, Debug `ctest -j 8` in
    at most half of 658 s, and at most 28 threads in an editor.
  - Wave 4's `game-ui`: the `Perf.GameUI.*` and `PerfGPU.GameUI.*` metrics are provisional.
  - Wave 4's `authoring-and-playtest`: `Perf.Editor.PrefabApply10kInstances` (Mac median 701.7 ms) and the prefab drag
    metrics (`DragInPrefabInstance5kMedian`, `DragPrefabInstanceRoot5kMedian`, `Drag100PrefabInstancesMedian`) are
    provisional, and the idle editor frame's criterion (`PerfGPU.Editor`) cannot be shown on the Mac, where frames wait
    for the display (8.3 ms).
  - Wave 4's `world-io`: its metrics are provisional (`Perf.Editor.ImportFolder24x4kPeakPrivate` measured 571-574 MB
    on the Mac against a 1,280 MB target), and `PerfGPU.Editor.SceneOpenOver1MMaxFrame` measured 1.08 times the step-0
    base (8.51-9.21 ms over 10 pairs), on a Mac where that frame mostly waits for the display.
  - Wave 4's `gpu-driven-rendering`: its metrics are provisional, among them `PlayStartRender1MCubes` (target 5 ms;
    the whole copy measured 22-27 ms on the Mac) and `Render10kUniqueMeshes` (MoltenVK turns each indirect draw into
    a Metal draw); `Render10kMaterials` measured 1.15 times the step-0 base on the Mac (0.844 against 0.737 ms, 7
    pairs), with the GPU busy 2.3-2.5 ms a frame; the `drawIndirectCount`, min/max sampler and device-minimum
    `maxDrawIndirectCount` paths are chosen by device features but run only on drivers that have them.
  - Wave 4's `world-partition`: its metrics are provisional (Mac: `Perf.World.CellUnloadMaxFrame` 2.6-2.7 ms against
    4 ms, `CellDriveMaxFrame` 2.65-5.5 ms and `PerfGPU.World.CellDriveMaxFrame` 9.5 ms against 8 ms,
    `Perf.World.DrivePeakPrivate` 280 MB against 2,048 MB, `DriveFrameP95` 4.2 ms, `Perf.Physics.Create100kStaticBodies`
    29.1 ms against 25 ms, `PerfGPU.Editor.PlayStartWorldMaxFrame` 37-61 ms against 50 ms); the editor-frame
    no-regression A/Bs (`PerfGPU.Editor.{Edit,Play}FrameNested{100k,1M}`, `SceneOpen*`) cannot be judged on the Mac,
    and `Perf.Physics.Boxes30kTracked.StrataStepTime` measured 1.022 times the step-0 base (36 pairs).
  - Wave 5's `physics-box3d` (physics on box3d instead of Jolt; Mac A/Bs against step 0's build, 3 interleaved pairs,
    ratios of medians): busy frames 0.58-0.63 (`Boxes70k.ActiveFrame` 24.7 to 14.4 ms, `Boxes10k` 3.53 to 2.23,
    `Boxes30kTracked` 11.0 to 6.9), whole steps 0.60-0.63, `Boxes30k.StartAnd30Frames` 0.66, `Characters1000Step` 0.41,
    `PerfGPU.World.CellDriveMaxFrame` 0.73 and `Perf.World.DrivePeakPrivate` 0.74, no body waiting and no dropped
    contact. Creating and removing bodies costs more, because box3d inserts every shape into its trees one at a time on
    the main thread (its API allows no other thread), where Jolt built them in parallel and in bulk:
    `Create100kStaticBodies` 3.40 (28.8 to 98 ms; box3d's `b3InsertLeaf` alone is 54% of it, and box3d by itself takes
    69-73 ms for those bodies), `Create10kBodies` 2.89 (3.4 to 9.7 ms; box3d alone 6.3 ms; this one has a confirmed
    budget, 11.99 ms from Jolt's 7.995 ms on the reference machine, which box3d is expected to exceed there at about
    23 ms, so `-L perf` will fail on it: the owner either grants an exception to "budgets only go down", to be measured
    again on the reference machine, or holds the merge until creating bodies costs less), `CellUnloadMaxFrame` 1.66
    (2.6 to 4.4 ms; box3d alone disables 25,000 static bodies in 2.0-2.5 ms), `CellDriveMaxFrame` 1.31 and
    `DriveFrameP95` 1.23 (both noisy), and `Grow65kMaxFrame` (65,536 bodies created in one frame) 73-80 ms against its
    16.7 ms target. The port keeps the rule that every body gets its collision at once (a body a frame leaves without
    collision is a gameplay defect, a slow frame of a large spawn is not), so the way down is bulk insertion and removal
    in box3d (its trees already build in bulk, `b3DynamicTree_Rebuild`, but its API inserts every shape), taken with a
    later box3d bump. Strata's own part of creating the 100,000 static bodies grew from about 17 to 27 ms in sampled
    runs, mostly allocations per body: a follow-up, since removing all of it would still leave about 2.5 times Jolt's
    time.
    A world now reserves a scene's own room in box3d at once, up to 131,072 bodies (it reserved only the automatic room
    of the starting bodies before the review round): Mac A/B of that change, 4 interleaved pairs, `CellDriveMaxFrame`
    0.85 (noisy), `CellUnloadMaxFrame` 0.94, `Create100kStaticBodies` and `Grow65kMaxFrame` 1.00, and the pages
    touched at play start for the largest room 0.7 to 25.9 MB (Windows commits the whole reservation: up to 343 MB by
    the estimate, below the 1,033 MB budget). New provisional metrics: `StrataToLibraryStep`
    (Mac: `Boxes10k` 0.45, `Boxes30kTracked` 0.92, `Boxes70k` 0.61) and `Grow65kMaxFrame` (the median of 5 fresh
    worlds' growth frames); the reference machine measures them and checks `StrataStepTime` against its existing
    budgets (Mac ratios 0.87, 1.01 and 0.87) and `Create10kBodies` against its budget (above).
  - The visual pass at 150% and at `--ui-scale 1` on a Windows display (CI builds and tests Debug on Windows since
    wave 4, but shows nothing).

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

### Later (wave 5 and beyond)

The wave-4 audits rank these first for wave 5:
- **`render-quality`:** TAA with motion vectors, shadows of point and spot lights, reflection probes and screen-space
  reflections, height fog and a physical atmosphere, extracting the sun from HDR skies with documented light units,
  cascade blending, transmission and clearcoat. It builds on `gpu-driven-rendering`'s per-view culling and instance
  history.
- **`project-settings`:**
  - Project Settings and Preferences panels.
  - Input actions with rebinding; `game-ui`'s navigation becomes their first map.
  - Collision layers for the whole project.
  - The game's name, version, company and icon in the exported executable.
- **`game-shell`:**
  - Crash reports in shipped games.
  - Game settings with quality presets mapped to the renderer.
  - Exports per platform and configuration, with a macOS app bundle.
- **`manual-and-samples`:** a people's manual reachable from the Help menu, playable templates, and a 3D sample made
  through the editor with a CTest that plays it.
- **`scene-contexts`:** per-scene asset managers and script engines instead of process-wide active ones.
- **`vfx`:** particles, trails and decals on the GPU-driven path.
- **`mesh-streaming`:** Mesh v2 with quantized vertices, per-LOD streaming on the geometry pool, and a collision
  sub-asset, so physics stops depending on the renderer's Mesh.
- **HLOD:** merged, simplified cells with baked atlases for distant parts of a world.
- **`world-environment`:** terrain cooked into world cells, with height-field colliders (box3d's height-field shapes)
  and sculpt and paint tools.
- **`animation-authoring`:** previews and scrubbing of animators, clips and transitions in the Inspector and the Asset
  tab.
- **Exported-game automation:** command scripts, simulated input and expectations in StrataRuntime.

Further:
- JobSystem v2 with work stealing and a frame task graph, once `Perf.Jobs` shows a need.
- 64-bit entity handles and origin rebasing.
- Per-world audio listeners and input contexts.
- Native game modules and an exported engine package.
- Audio streaming from packs, and an audio mixer with buses and effects.
- Localization with string tables and font fallback chains.
- BC6H.
- ImGui multi-viewports.
- Physics joints, ragdolls and vehicles, on box3d's joints (revolute, prismatic, spherical, weld, wheel, distance,
  motor): components, gizmos, a script API and the feature test, in a stream of their own after the port to box3d.
- Triggers that meet triangle-mesh colliders (box3d's sensors skip meshes), and box3d's recordings and replays as an
  editor debugging tool.
- Level-design tools beyond placement: orthographic views, surface and vertex snapping, hiding and locking in the editor
  only.
- OBJ and FBX import.
- Morph targets.
- A prefab edit mode.
- A timestep display and other editor polish.
