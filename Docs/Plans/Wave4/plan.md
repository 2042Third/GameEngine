# Wave 4 plan

The specs go in `wave4\specs\`. `common.md` holds what every agent needs to know about Strata after wave 3, the
shared-file ownership of wave 4 and the report format. The five audits of main 685aef7 (person, agent, rendering, scale,
architecture) are under `wave4\audit\<lens>\`. Each lens's `STATE.md` holds its running notes and points to its
evidence; the harness refused the `findings.md` files, so the full findings exist only in the orchestrator's task text.
Every finding and its owner is listed in "Findings and their owners" below, and the stream briefs carry the evidence
each spec needs.

The owner's goal comes first: "an actual production engine with its own identity: high scaling, modular, assets
streaming optimized". Four streams serve it directly:
- `engine-modules` makes the engine modular;
- `world-io`, `world-partition` and `gpu-driven-rendering` provide scale and streaming.

The other two take what the person and agent audits rank highest:
- `game-ui`: in-game UI, the largest missing system.
- `authoring-and-playtest`: wave 3's workflows that break past the demo, and exact play-testing for agents.

## The streams

| # | Key | Size | Theme | Closes (critical findings first) | Builds on (merged) |
| --- | --- | --- | --- | --- | --- |
| 1 | `engine-modules` | XL | EngineModule lifecycle, per-layer libraries generated from `Layers.json`, a runtime without tooling, module-owned components with soft ordering, faster builds and sharded tests, optional subsystems | architecture: Application wiring (7 allowlist lines), ImGui in every game, components in Scene, missing PCHs, the serial Core CTest | module-foundation, physics-at-scale, mip-streaming, scene-streaming, content-and-prefabs, animation |
| 2 | `world-io` (new) | XL | Saves off the frame, streamed JSON and binary cooked scenes, asset registration and first scans at tens of thousands of assets, an import memory budget, pack v3 for small entries | scale (critical): the 22 s save, the 4 GB scene I/O, 3-10 ms per registered asset and launch timeouts. Major: unbounded import memory, small entries 10.7x larger in packs | scene-streaming, pack-v2, content-and-prefabs, agent-activity |
| 3 | `world-partition` | XL | A World asset with data layers and grid cells, cells streamed in the editor and in games around streaming sources, a cell cook, physics sized for the world (no streamed body ever lacks collision), sliced unloads | scale (critical): bodies beyond the play-start capacity, world partition itself. Major: cell unload hitches, the play start's physics | scene-streaming, physics-at-scale, render-scene, pack-v2, streaming-observatory, world-io (this wave) |
| 4 | `gpu-driven-rendering` | XL | GPU timers first; shadows at scale; a shared geometry pool; GPU culling (frustum, size, HZB occlusion), LOD selection and indirect draws per view and cascade, with visibility feedback; camera-relative rendering; render stats for agents | rendering (critical): serial shadow casters, one draw per distinct mesh. Major: no GPU time, linear extraction and no occlusion, large coordinates, the 4,096-light cap, LOD pops. Person: GPU timings | render-scene, mip-streaming, animation, streaming-observatory |
| 5 | `game-ui` (new) | XL | A GameUI module: canvases with a reference resolution, rects, images, text, buttons, toggles, sliders, layouts, focus and gamepad navigation, script events, unscaled time, a 2D canvas view, `ui.*` commands | architecture (critical): no in-game UI system. Person and agent (major): game UI is text only | render-scene, gameplay-and-scripting, editor-authoring |
| 6 | `authoring-and-playtest` (new) | XL | Skinned characters in their rest pose in edit mode; import scale and normal maps that behave; exact play-testing (`play.step`, input in game time, `input.sequence`, world queries); compact MCP toolsets; a Game view beside the Scene view; per-property prefab overrides; daily polish | person (critical): skinned characters lie flat. Person (major): prefabs, import scale, normal maps, no scene view while playing. Agent (major): play.step, input timing, world queries, the running scene's name, the MCP tool count | animation, content-and-prefabs, gameplay-and-scripting, agent-activity, editor-authoring |

There are six streams, the maximum, and each is XL. Every spec lists "Deliver in this order if time runs short", with
the items that close critical findings first.

## Before branching (step 0)

Streams branch from main only after all of step 0 has merged. Step 0 runs in parallel with the spec writing.

1. **Land the branches in flight**, each after its own verification:
   - Each one passes: Debug `ctest --preset windows-debug` under the validation layer, Release `-LE perf`, the GCC
     lint, the brace check and the compat check.
   - Each one is pushed as `ci/<name>` and green on all three platforms before main moves.
   - The branches:
     - **`agent/wave3-finish`**: wave 3's open perf items.
     - **`agent/release`**: 7 commits on 6b23652:
       - the installed SDK and toolchain detection (`Editor/ScriptToolchain`, `ScriptBuildSettings::Detect`/`FindToolchain`);
       - release archives (`CMake/StrataPackaging.cmake`, CTest `StrataEditor.Install`);
       - the macOS 14 deployment target;
       - `release.yml` with version 0.3.0;
       - the Dist editor build fix (`EditorIcon.h`);
       - toolchain errors that lead with what to install.

       It rewrites `ScriptBuild.cpp` and parts of `EditorContext`, so the streams must start from it.
2. **A step-0 branch (`agent/wave4-step0`, small)**, on top of both:
   - **New Script fix (critical, person audit).** In a project whose scripts were already built, New Script (or a
     `.cpp` added by hand) followed by one build gives a module without the class.
     - Cause: the configure stamp ignores the source list. Under the Visual Studio generator, `CONFIGURE_DEPENDS`
       re-globs during the build but compiles the stale project.
     - Fix: put the sorted source list of the script directory into the configure stamp (`MakeStamp` in
       `ScriptBuild.cpp`, fed by the `StampScriptSources` set `EditorContext` already takes), so a changed file set
       configures first.
     - Tests:
       - a package test: build once, `script.create` a second class, build once, and the class is found; the same
         for a `.cpp` written directly;
       - a fake-CMake unit test: a changed file set produces a configure invocation.
   - **Main green on CI on Linux and macOS.** The tests that failed there at 6b23652 (named by 4762916's
     annotations) are fixed. `ci.yml` gains a Windows Dist build of all targets with warnings as errors: nobody built
     Dist, and wave 3 broke it.
   - **The scene-context seam.** `AssetManagerBase* Scene::GetAssetManager() const` returns the active manager for
     now. New wave-4 code in scene systems, loaders and the renderer reaches the manager through the scene. Wave 5's
     scene contexts then change one function instead of new call sites. The 22 engine and 28 editor
     `AssetManager::GetActive()` calls stay as they are.
   - **Docs drift (architecture audit, minor):**
     - `README.md`: its feature list gains waves 2-3, and its test line becomes `ctest --preset windows-release -LE perf`.
     - `AGENTS.md`: trimmed to rules plus links (169 KB, 23,240 words). Descriptive paragraphs move into
       `Docs/Architecture.md` with no rule changed, and a size check fails `AGENTS.md` above 80 KB.
     - All six streams edit `AGENTS.md` and every agent reads it whole, so this happens before branching or not in
       this wave.
3. **The base build for A/Bs.**
   - The integrator builds the step-0 tip once, in Release and Debug, in a worktree that nobody edits
     (`.worktrees/wave4-base`, `build/windows`). Every stream's interleaved A/Bs run against it.
   - One `PerfStability.py --runs 3 --wait-quiet 20` on it records the canary and the base's values.
   - On 2026-10-12 the machine stopped twice with a MEMORY_MANAGEMENT bug check (0x1A). Until a memory stress test
     passes, one-off crashes stay suspect (common.md section 1).
4. **The worktrees.** Create them with `scratchpad\create_worktree.sh <key> <step-0 tip>`. Give each agent:
   - its spec and `common.md`;
   - its worktree and its build directory;
   - the base build's path;
   - the validation layer path (`<repo>\build\_tools\Vulkan-ValidationLayers\build\layers\Release`);
   - the perf lock command;
   - the commit attribution lines.

## Merge order and why

**1. engine-modules, 2. world-io, 3. world-partition, 4. gpu-driven-rendering, 5. game-ui, 6. authoring-and-playtest.**

- **engine-modules first** (as the Roadmap planned). It changes the build and the shell that everything else compiles
  in, and no feature behavior:
  - `Layers.json` becomes the libraries, so the new layers of world-partition (`World`) and game-ui (`GameUI`) become
    libraries without CMake work of their own.
  - `Application` becomes a module lifecycle.
  - Components move into their modules.

  The later streams rebase onto moved headers. That is mechanical: include lines change, behavior does not. Their own
  new components live in their own module files from day 0 (common.md section 2), so nothing new is added to
  `Scene/Components.h` that would have to move.

  Fallback: if engine-modules is not merge-ready when the others are, it merges last and redoes its component move on
  top of the others. That is the same mechanical change, and the later merges must not wait for it.
- **world-io second.**
  - world-partition's cells are scene documents: the editor writes them with world-io's streamed writer and saves
    dirty cells through its background save; exports cook them into its binary format; loads use its cancellable
    reader.
  - world-io's `WorldFixture.h` (realistic worlds, many-asset projects) is what world-partition's and
    gpu-driven-rendering's gates reuse.
  - It touches no renderer file.
- **world-partition third.**
  - It is the signature feature, built on world-io's formats.
  - Its host-function block comes first in the wave's ABI appends.
  - It changes `SceneSubScenes`, `PhysicsWorld` and `GameRuntime`, which no later stream owns.
- **gpu-driven-rendering fourth.**
  - Its rewrite of `SceneRenderer`, `RenderScene`, `Mesh` storage and the shaders lands after the scene-side streaming
    changes. At its merge the integrator checks that cell loads and unloads stay within the instance upload budget.
  - It lands before game-ui, whose overlay pass then rebases onto the final renderer instead of the reverse.
- **game-ui fifth.**
  - Its UI pass is self-contained (`GameUI/GameUIRenderer.*` plus one call in the overlay section of `SceneRenderer`
    and one in `GameRenderer`), so it rebases onto the rewritten renderer easily.
  - Its host block and class callbacks come second.
  - It adds `ui.*` commands that the last stream's MCP toolsets must cover.
- **authoring-and-playtest last.**
  - Its MCP toolsets and the command-kind lists (`CommandJournalTests.cpp`) must cover every command the wave added
    (`world.*`, `physics.stats`, `ui.*`, `input.click`).
  - `input.sequence` must be able to click game-ui's buttons.
  - The Game view must draw game-ui's canvases at the chosen resolution.
  - Its contract changes are the last to touch the command scripts every stream runs (`FeatureCommands`,
    `TetrisCommands`, `SmokeCommands`): `play.step` answers after its steps, and input durations count game updates.
  - Its many small editor fixes are the easiest to rebase.

After each merge:
- build Debug and Release;
- run the fast suites (Debug `StrataTests.exe --test-suite-exclude=GPU*,EndToEnd*,Package*,Perf.*,PerfGPU.*`);
- run the GPU suites under the validation layer;
- run the feature runners;
- run `compat_check.py` (0 hits).

After the last merge:
1. the full Debug ctest under validation, and the Release ctest `-LE perf`;
2. one Dist build of all targets;
3. `PerfStability.py --runs 5 --wait-quiet 20` with the canary (the campaign that decides the budgets, below);
4. the GCC lint, the brace check and the compat check;
5. the visual pass at 150%, at `--ui-scale 1` and in a 1600x900 window;
6. push `ci/wave4`, and move main only once Windows, Linux and macOS are green;
7. release 0.4.0 (agent/release's process: `Docs/Releases/0.4.0.md`, the version bump, the tag);
8. the "Wave 4 landed" entry in `Docs/Roadmap.md`.

## Wiring left to whichever stream merges second (the integrator does what neither could)

The stream that merges second does the wiring in its own rebase, because it merges after the code it needs (common.md
section 12). The integrator checks it and does only what neither stream could.

- **engine-modules x every stream:**
  - The `World` and `GameUI` layers become libraries (their `Layers.json` entries; each stream appends its layer to
    `Engine`'s `MayInclude`).
  - Their scene systems get soft ordering constraints where they name optional systems: the world streaming system runs
    `AfterIfPresent {"Scripting"}`, `BeforeIfPresent {"Physics"}`, and the UI system `BeforeIfPresent {"Scripting"}`
    (before the scripts, so a click the UI takes is hidden from them in the same frame; layout runs lazily at render
    and still sees the scripts' changes).
  - Their composition-root rows go at the end of the table, World then GameUI.
  - Code that uses moved components includes their module headers.
  - New test suites land in a Core shard; the shard-coverage test fails otherwise.
  - New programs or helper modes list their engine modules.
- **world-io x world-partition:**
  - Cells are written by the streamed writer (entity subsets).
  - Dirty cells are saved through the background save, one undo-stack mark per save.
  - Exports cook cells to the binary format.
  - Cell loads use the cancellable reader.
  - A world's first scan registers thousands of cell documents through the batched registration.
- **world-io x authoring-and-playtest:**
  - `scene.save` is deferred, so skills and the MCP description say so.
  - `editor.status` shows `assets.scanning` and `saving` in its compact form.
  - world-io adds no command; its deferring commands get `MayDefer`, and its `WaitsForAssets` flags survive
    authoring-and-playtest's command splits.
- **world-partition x gpu-driven-rendering** (gpu-driven-rendering's rebase):
  - world-partition lands `FramePerfLog`'s column groups; gpu-driven-rendering builds on them.
  - Cell loads and unloads upload instance ranges within the upload budget (the instance buffer grows by GPU copy).
  - The cell-drive gate runs with GPU culling.
  - The world test 20 km out renders through camera-relative instances.
  - The play start of a world uses the render scene's clone hook.
- **world-partition x authoring-and-playtest:** `entity.get {world}` and the bounds queries work on cell entities, and
  the Game view plays a world.
- **gpu-driven-rendering x game-ui:** the UI pass runs after post-processing in the final `SceneRenderer`, with a
  `GPU.UI` timer stage. The hook is game-ui's (`Renderer/SceneOverlay.h`); game-ui re-applies it and adds the stage
  in its rebase.
- **gpu-driven-rendering x authoring-and-playtest:**
  - Rest-pose palettes of edit mode are drawn by the GPU path.
  - The Game view's renderer reports GPU stages.
  - `renderStats` appears in `viewport.capture`.
- **game-ui x authoring-and-playtest** (authoring-and-playtest's rebase):
  - `input.sequence` events can click UI entities (game-ui owns `input.click {entity}` and `ui.hitTest`); `input.click`
    counts fixed steps like the other input commands.
  - The Game view's resolution drives canvas scaling, and it shows camera-less UI scenes as `GameRenderer` does.
  - The presence chips avoid UI rects.
  - The Hierarchy's preset table is the union of UI presets and primitives with colliders ("Text" becomes "3D Text").
  - Inactive-property reasons cover the UI components' settings.
- **engine-modules x authoring-and-playtest** (authoring-and-playtest's rebase):
  - The inactive-property reasons (`InactiveReason`) and the hidden prefab link fields go on the registrations wherever
    engine-modules moved them.
  - `PrefabInstanceComponent` (with its new override list) stays in Scene.
- **`MayDefer`** (authoring-and-playtest's rebase): set on every deferring command of the merged tree, from the lists
  the other streams' reports give (common.md section 8).
- **Host-function order** (`ScriptABI.h`, `ST_SCRIPT_HOST_FUNCTIONS`, SDK headers): world-partition (5, 119 -> 124),
  then game-ui (18, 124 -> 142) (authoring-and-playtest appends none).
  - The class callbacks game-ui appends to `StrataScriptClassDesc` follow `OnAnimationEvent`.
  - After each merge the integrator checks the `static_assert`, the API module and feature coverage.
- **Importer versions:** `GltfImporter` (authoring: import scale; gpu-driven: LOD error, if it lands) and
  `TextureImporter` (authoring: green flip) end with one bump each beyond main's.
- **Unions:**
  - `Feature.stscene` and the feature project's assets: world-partition's tiny world, game-ui's canvas, authoring's
    prefab override;
  - `Layers.json`;
  - `Budgets.json`;
  - `BuiltinModules.cpp`;
  - component registrations;
  - `EditorLayer::RegisterBuiltinPanels` (the Game view);
  - the status bar pills;
  - `c_LayoutVersion` (4 to 5, once: the Game view docks as a tab beside the Viewport);
  - the command-kind lists in `CommandJournalTests.cpp`;
  - the MCP toolset table;
  - the skills;
  - `StrataTests/CMakeLists.txt` shards.
- **Docs:** the integrator writes the Roadmap's "Wave 4 landed" entry and `Docs/Releases/0.4.0.md` from the reports.
  The streams do not edit Roadmap's Status.

## Decisions from the cross-check of the specs (2026-10-13)

The spec writers' questions are answered in each spec's "Answers from the planner" section. The decisions that touch
more than one stream:

- **One owner per shared change.**
  - Entity and asset references in components' extra data: engine-modules' `ComponentInfo::ExtraReferences`. world-io
    adds only `ExtraBinary` (its `ForEachExtraEntityReference` is dropped).
  - The overlay hook: game-ui's `Renderer/SceneOverlay.h`. gpu-driven-rendering defines none; game-ui adds the `GPU.UI`
    stage in its rebase.
  - `FramePerfLog` column groups: world-partition lands them (it merges first and needs them) in the shape
    gpu-driven-rendering specified; gpu-driven-rendering builds on them.
- **Ordering.** The UI system runs before Scripting (`BeforeIfPresent`), not after.
- **Components.** `MeshRendererComponent` and `SkinComponent` keep their types in `Scene/Components.h`; Renderer and
  Animation register them.
- **The EngineModule interface** stays in the App layer; Audio's and Project's `MayInclude` gain App (downward, with
  Notes).
- **Snapshots and copies.** world-io's `SerializedOnly` snapshot copies run no context copiers (no render-scene clone
  for a save).
- **Cooked scenes** store JSON extras without binary hooks as MessagePack, so authoring-and-playtest's prefab override
  list needs no binary code.
- **`DocumentSaver`** takes ready document bytes (the World document) and reserved assets with given handles (cells'
  derived handles); entity subsets name their roots by UUID, taken on the main thread with the snapshot.
- **Worlds in the editor.** New top-level entities go into the persistent scene unless the person or agent picks a layer
  (`world.edit {editLayer}`): screen-space canvases, cameras, lights and game managers made by commands must not stream
  out with a cell, and every existing test that edits the feature scene keeps its meaning.
- **`MayDefer`** is set by authoring-and-playtest (last) on every deferring command of the merged tree; the registry's
  deferral during scans never trips its assert (common.md section 8).
- **Import admission** defaults to 25% of physical memory, clamped to 1-4 GiB.
- **Screen-size culling** is on by default (0.5 px views, 1 texel cascades), with before and after screenshots.
- **The render-scene clone:** copy-on-write pages only if they leave `RenderScene`'s hot loops within noise; otherwise a
  deep parallel copy, with the 5 ms limit kept as a target.

## Perf in wave 4

- Every stream adds the metrics its acceptance criteria name, with `Budgets.json` entries. It measures them on the
  reference machine with interleaved A/Bs against the step-0 base build under the perf lock (common.md section 9).
  Entries set that way are marked "to confirm in the integration campaign" in the stream's report.
- No stream runs `PerfStability.py` or the `perf` label. No stream raises a budget.
- The integration's one quiet campaign on the merged tree (`--runs 5 --wait-quiet 20`, nothing else on the machine)
  decides every new and lowered budget, as in wave 3.
- New fixtures:
  - world-io: `Perf/WorldFixture.h`, a realistic 250k/1M-entity world with renderers and 10% static colliders, a
    10,000-asset project and a folder of 4096x4096 textures, built from the scale audit's generators;
  - world-partition: grid worlds and cell drives;
  - gpu-driven-rendering: shadowed cube grids, 10,000 unique meshes and a 1,000,000-instance grid;
  - game-ui: 10,000 widgets.
- Idle editor frames (`PerfGPU.Editor.{Edit,Play}FrameNested*`) have little room. The Game view, cell map, UI pass and
  GPU timers must cost nothing measurable while idle, measured with `editor.perf`.

## Findings and their owners

Critical findings are marked **C**, major ones M, minor ones m. "w5" means wave 5 or later, with the reason.

**Person audit**

| Finding | Owner |
| --- | --- |
| **C** New Script's class missing after one build | step 0 (`agent/wave4-step0`) |
| **C** Skinned characters in their mesh-space bind pose in edit mode, thumbnails and previews | authoring-and-playtest (1) |
| M Prefab apply skips overridden instances, no override marks, actions only in the Hierarchy | authoring-and-playtest (prefabs v2). Prefab edit mode: w5 |
| M Import Scale does not reach placed instances | authoring-and-playtest |
| M Poly Haven normal maps import as color, no warning | authoring-and-playtest |
| M No in-game UI beyond text | game-ui |
| M No scene view while playing, no game-view resolution | authoring-and-playtest (Game view) |
| M Animation preview only as a command | w5 `animation-authoring`. Agents can already preview through `animation.preview`, and the viewport chip names it |
| M No Project Settings, input actions or project-wide collision layers | w5 `project-settings`. game-ui ships a fixed UI action set designed as that asset's first map, and world cells use their host scene's layers |
| M Onboarding: two templates, no manual in the editor | w5 `manual-and-samples`, after game-ui so the 3D template and sample have menus (FoxCoins, the agent's game, is the candidate). File > Open Scene: authoring-and-playtest (polish) |
| M No particles | w5 `vfx`: particles cull and draw through gpu-driven-rendering's path |
| M Fog, terrain, reflection probes, depth of field | w5 `render-quality` (fog, probes, depth of field) and `world-environment` (terrain cooked into world-partition's cells) |
| M No GPU time in the Performance panel | gpu-driven-rendering (1) |
| m Primitives without colliders | authoring-and-playtest |
| m The recovery dialog nags for deliberate discards | authoring-and-playtest |
| m Feedback that contradicts itself (stale toast, "3 errors", toasts over Add Component, inactive properties, raw link fields, raw JSON in Activity) | authoring-and-playtest |

**Agent audit**

| Finding | Owner |
| --- | --- |
| M `play.step` answers before its steps run, and leftover steps survive resuming | authoring-and-playtest |
| M Input timed in render frames, one control per command | authoring-and-playtest (`input.sequence`) |
| M No world transforms or bounds | authoring-and-playtest |
| M Import Scale | authoring-and-playtest (same as the person audit's) |
| M Exported games cannot be driven by an agent | w5 `exported-game automation`: `EditorCommandScript` must move into a layer both programs link, which engine-modules' per-layer libraries make possible. In wave 4, StrataRuntime reports unknown arguments (authoring-and-playtest) |
| M No command names the running scene after a switch | authoring-and-playtest |
| M Game UI is text only | game-ui (`ui.hitTest`, `input.click {entity}`) |
| M The MCP tools cost about 27k tokens, and their count is 7 short of a 128-tool limit | authoring-and-playtest. Wave 4 adds about 15 commands, so this cannot wait |
| m `script.status` hides contact and animation-event callbacks | authoring-and-playtest |
| m Logs and the journal page oldest-first; inconsistent names and results | authoring-and-playtest |
| m Presence chips cover the HUD; read-only queries count as activity | authoring-and-playtest |
| m No batch requests or multi-instance `prefab.instantiate` | authoring-and-playtest (last item) |
| m Skills: the journal note comes late; play-testing, a third-person recipe and model checks are missing | authoring-and-playtest |
| m No unscaled time; few camera helpers | game-ui (pause menus animate in unscaled time; `LookAt`/`SmoothDamp` in the SDK) |
| m Sound is unobservable | authoring-and-playtest (`audio.state`, a warning on dropped one-shots) |
| m Play-mode-only commands warn about discards | authoring-and-playtest |

**Rendering audit**

| Finding | Owner |
| --- | --- |
| **C** C1: shadow casting is a serial per-caster CPU cost, ungated | gpu-driven-rendering (2) |
| **C** C2: CPU draw submission scales with distinct meshes | gpu-driven-rendering (3) |
| M L1: no GPU time | gpu-driven-rendering (1) |
| M L2: extraction linear in the frustum, no occlusion | gpu-driven-rendering (4) |
| M L3: lights: a CPU walk and a 4,096 cap | gpu-driven-rendering (7, if time). Else w5 `render-quality` |
| M L4: an HDRI's sun is lit twice | w5 `render-quality` |
| M L5: skinned characters lie flat | authoring-and-playtest (1) |
| M L6: FXAA only, no motion vectors | w5 `render-quality`: TAA needs the previous instance transforms, which gpu-driven-rendering's instance mirror makes cheap |
| M L7: no local reflections | w5 `render-quality` |
| M L8: only the sun casts shadows | w5 `render-quality`: per-light views need gpu-driven-rendering's per-view culling |
| M L9: generated LODs pop and tear | gpu-driven-rendering (7, if time; LOD selection by projected error on the GPU). Else w5 with Mesh v2 |
| M L10: no fog or atmosphere | w5 `render-quality` |
| M L11: no camera-relative rendering | gpu-driven-rendering (5). Physics and scene coordinates stay float32, with the limit world-partition documents; origin rebasing is w5 |
| m m1: cascade seams and PCSS noise | w5 `render-quality` |
| m m2: transparency and the material model | w5 `render-quality` |
| m m3: mip levels dropped after 2 s with budget to spare | gpu-driven-rendering (with visibility feedback) |
| m m4: render statistics out of reach; `--perf-log` misses Scene.Animation | gpu-driven-rendering (1) |
| side note: export stuck in WaitingForImports while `editor.status` showed 0 imports | world-io |
| side note: `project.create {directory: ""}` | authoring-and-playtest |
| side note: a 1M-entity JSON scene peaks at 2.95 GB | world-io |

**Scale audit**

| Finding | Owner |
| --- | --- |
| **C** Streamed bodies beyond the play-start capacity get no collision | world-partition (early) |
| **C** Scenes load and save as one JSON document on one thread (4 GB, the 1,048,575 cap) | world-io (streamed and binary I/O). Live entities stay capped per registry: world-partition keeps what is loaded below it, and 64-bit handles stay Later |
| **C** `scene.save` freezes the main thread (22 s at 1M) | world-io (1) |
| **C** Registering assets costs 3-10 ms each; a fresh 30k-asset project times out `StrataCLI launch` | world-io (2) |
| **C** What world partition must deliver | world-partition. HLOD is w5: it needs merged, simplified meshes and atlases (Mesh v2) and gpu-driven-rendering's LOD error; until then distant cells are simply not loaded |
| M Play start of a real 1M world is a 0.6 s frame | split: physics bodies in parallel batches (world-partition), the render scene cloned instead of rebuilt (gpu-driven-rendering), a realistic gate (world-io's fixture) |
| M Large cells hitch on load and unload; instance uploads bypass the budget | sliced unload and one per-frame load budget: world-partition. Instance buffer growth and budgeted uploads: gpu-driven-rendering |
| M Import memory is unbounded | world-io |
| M Pack v2 inflates small assets 10x | world-io (last item). Else w5 |
| M Meshes, audio and physics geometry stay whole | w5 `mesh-streaming` (Mesh v2 with per-LOD streaming and a collision sub-asset): it builds on gpu-driven-rendering's geometry pool, which changes mesh storage this wave. Audio streaming goes with the audio mixer |
| M Perf gates do not cover a world | each stream's fixtures and gates. Cold-I/O mode and read depth: world-io (if time) |
| m Threading is not the bottleneck; `Perf.Jobs` | engine-modules (`Perf.Jobs` overhead and wake-up metrics, so JobSystem v2 is decided on numbers). The background-thread cap while loads run: world-io (if time) |
| m Physics capacity, command work and cells are invisible; the exported game spins unpaced while loading | `physics.stats`, cell map, runtime pacing: world-partition. `Editor.Commands` profiler stage: world-io |

**Architecture audit**

| Finding | Owner |
| --- | --- |
| **C** Main fails CI on Linux and macOS; Dist does not compile | step 0. 60fe71f fixed the compile errors, agent/release fixes the Dist editor, step 0 fixes the remaining Linux/macOS test failures and adds a Dist CI job, and `ci/` branches gate main. Compiler cache and a Debug CI job: engine-modules |
| **C** No in-game UI system | game-ui |
| M Exported games ship the editor's configuration, only for its platform | partly agent/release: release archives are Dist builds, so installed editors export Dist games. The export's platform and configuration choice and the macOS bundle: w5 `game-shell` |
| M Application hard-codes renderer, audio, ImGui, assets and project | engine-modules (1) |
| M No crash reporting in shipped games | w5 `game-shell` |
| M Game settings stop at window mode, resolution and vsync | w5 `game-shell`: it needs game-ui for the screen, and its quality presets map onto the renderer gpu-driven-rendering rewrites |
| M Input is raw devices only | w5 `project-settings` (input actions, rebinding). game-ui: fixed UI navigation and typed text for scripts |
| M One static library; every game links Dear ImGui | engine-modules (2) |
| M Every module's components live in Scene; constraints naming absent systems refuse registration | engine-modules (3) |
| M Process globals; five service shapes | w5 `scene-contexts`: it edits `SceneRenderer`, `RenderScene` and `SceneLoader`, which gpu-driven-rendering, world-io and world-partition rewrite this wave. The seam (`Scene::GetAssetManager`) is in step 0 |
| M No PCH where 70% of the compile time is | engine-modules (4) |
| M StrataTests.Core: 1,672 cases in one serial CTest | engine-modules (4) |
| M Audio: master volume only | w5+ (audio mixer) |
| M No localization | w5+, after game-ui |
| M No particles | w5 `vfx` |
| M A one-page manual; the only sample uses none of waves 2-3 | w5 `manual-and-samples` |
| m Docs drift; AGENTS.md has tripled | step 0 (README, the AGENTS.md trim with a size check). Architecture.md's "the runtime has no ImGui" becomes true with engine-modules |
| m 900-line registration functions, 4,000-line files | rule (common.md section 8): a stream that substantially edits a command group's registration splits that group into its own function or file. No big-bang refactor |

## What changed from the roadmap and why

The Roadmap planned three wave-4 streams: engine-modules, world-partition and gpu-driven-rendering. They stay, with
these changes, and three streams join from the audits.

1. **engine-modules, narrowed and widened.**
   - **Scene contexts (its step 5) move to wave 5.** They edit `SceneRenderer`, `RenderScene` and `SceneLoader`, which
     gpu-driven-rendering, world-io and world-partition rewrite this wave; the step-0 seam keeps new code off the
     globals.
   - **It gains the architecture audit's build throughput:** PCHs for the editor and test libraries, `json_fwd.hpp` in
     headers, the Core CTest in shards, a compiler cache and a Debug CI job, and `Perf.Jobs`. The per-layer libraries
     need new CMake anyway.
   - **Optional subsystems come last in its order.** Builds without Audio and Physics are proven by a CTest
     configuration. Network becomes tooling, absent from the runtime by construction. Physics-off stubs for the
     character controller are needed only if Physics is optional.
   - **The exported game's identity** (wave 3's recommendation) moves to wave 5 with project settings.
2. **world-partition is split in two.** The scale audit found that the paths around the frame fail first, before any
   partition exists, and that fixing them is XL on its own:
   - a 22 s save;
   - a 4 GB parse;
   - a 4-minute first scan, with `StrataCLI launch` timing out;
   - streamed bodies without collision.

   So **world-io** (new) owns scene and asset I/O at world scale. The binary cooked scene format moves here from the
   Later list. **world-partition** owns the partition itself, plus physics at world scale (capacity from the cook,
   growth, parallel creation, sliced unloads). HLOD and origin rebasing move to wave 5.
3. **gpu-driven-rendering is widened.**
   - **GPU timers come first.** The rendering and person audits both found that no GPU time is measured, and the
     stream's own gates need it.
   - **Shadows at scale are fixed now** (critical, ungated: 6-7 ms for 100,000 shadowed cubes).
   - **It takes camera-relative rendering** from world-partition (its instance format) and the play start's render
     scene clone.
   - **It adds render statistics for agents.**
   - **TAA, motion vectors, local-light shadows, reflections and fog go to wave 5's render-quality**, which builds on
     this stream's per-view culling and instance history.
4. **game-ui is new.** wave 3's plan recommended it, and all three product lenses rank it highest among missing
   systems (critical in the architecture audit).
   - Input actions and rebinding (architecture "land before or with game-ui") are an L of their own. They go to wave
     5's project-settings, with project-wide collision layers and settings.
   - game-ui ships a fixed UI action set (Submit, Cancel, Navigate on keys, gamepad and mouse), designed as the
     asset's first map, and typed text for scripts.
5. **authoring-and-playtest is new.** It merges the person audit's proposed `authoring-fidelity`, `prefabs-v2`,
   `play-in-editor` and `editor-polish` with the agent audit's `agent-playtest` and `agent-surface`:
   - Two wave-3 workflows break past the demo: New Script (now fixed in step 0) and skinned characters in edit mode.
   - Agents need exact play-testing.
   - The MCP tool list must shrink before wave 4's commands pass 128 tools.
   - **Per-property prefab overrides move here from the Later list.** The prefab edit mode stays Later.
6. **Not taken from wave 3's recommendations:**
   - `rendering-quality` goes to wave 5, after gpu-driven-rendering.
   - `docs-and-samples` goes to wave 5 as `manual-and-samples`, after game-ui, so a 3D sample can have menus.
7. **CI gates are step 0, not a stream.** CI on `ci/` branches (6b23652), green main, and a Dist job are days of work,
   needed before six branches merge. Build health belongs to engine-modules.
8. **The Later list** (`roadmap_wave4.md`):
   - Removed: the binary cooked scene format, per-property prefab overrides, and the preferences panel (it joins
     project-settings).
   - Added, as wave 5 candidates: `render-quality`, `project-settings`, `game-shell`, `manual-and-samples`, `vfx`,
     `scene-contexts`, `mesh-streaming` (with Mesh v2), HLOD, `world-environment`, `animation-authoring`, and
     exported-game automation for agents, besides what was already there.

## Risks

- **Six XL streams on one 24-thread machine.**
  - Builds contend and perf numbers are noisy: A/Bs under the perf lock against the fixed base build, and the
    integrator's quiet campaign decides.
  - Expect two review rounds at integration, as in waves 2 and 3.
  - Each spec has a delivery order.
- **engine-modules and CI on three platforms.**
  - Per-layer static libraries change link order on GNU ld and Apple ld. CMake handles that through
    `target_link_libraries` along the layer DAG.
  - Registration is explicit (the composition root), so the linker cannot drop self-registering objects.
  - The one remaining allowlist line (Physics reads the renderer's `Mesh`) needs either an inverted dependency (a
    mesh-geometry provider the Renderer module registers: allowlist 0) or an explicit, listed link exception.
  - Linux and macOS only build in CI, so the orchestrator pushes the stream's tip as `ci/engine-modules` before its
    review, and the stream fixes what CI reports.
- **world-partition scope.**
  - Editing cells inside one edited scene touches undo, saves, the Hierarchy and play start at once.
  - The spec delivers runtime streaming with sources and physics capacity before editor-side cell editing. A world
    that can be cooked and driven but edited only cell by cell still closes the critical findings.
  - HLOD is out.
- **gpu-driven-rendering correctness.**
  - Two-phase occlusion must not pop.
  - Devices without `drawIndirectCount` (the device enables it only where supported) need a fallback.
  - GPU visibility feedback arrives frames late. Asset requests and mip streaming must not regress the streaming
    gates (`PerfGPU.Streaming.*`, `Mips.*`), so GPU culling replaces CPU marks only once the feedback drives them as
    well.
  - The geometry pool changes mesh memory accounting (`GetMemoryUsage`, eviction), so residency tests must keep
    passing.
- **game-ui scope.** The runtime, SDK and commands come before the editor's 2D canvas view. Agents author UI through
  `ui.*` commands even if the view is late.
- **authoring-and-playtest breadth** (about fifteen items).
  - The delivery order puts the critical rest pose and the agent's exactness items first.
  - Prefabs v2 (L) lands as per-property propagation plus the Inspector bar; the edit mode is w5.
- **Contract changes.**
  - `play.step` now answers after its steps run, and input durations count game updates. Command scripts, tests,
    skills and agents rely on both.
  - authoring-and-playtest updates them all and merges last; the integrator re-runs Smoke, Tetris, Export, Scripts and
    the feature runners after it.
- **The MCP tool count.** Until authoring-and-playtest merges, the integration tree may expose more than 128 tools. The
  integration's agents use `StrataCLI call`, so only outside MCP clients would notice.
- **Idle-frame budgets.** The Game view, the UI pass, cell maps and GPU timers are new always-present code. Each spec
  requires zero idle cost, measured with `editor.perf` before and after.
- **The branches in flight.** If `agent/release` or `agent/wave3-finish` change before they land (another
  `ScriptBuild` shape, other perf fixes), the specs' references to them need a refresh before the agents start.
- **The machine.** Two 0x1A bug checks on 2026-10-12, and compiler crashes now and then. One-off crashes are retried,
  and only reproducible ones are chased (common.md section 1).
