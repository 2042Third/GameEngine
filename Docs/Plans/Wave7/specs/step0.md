# Step 0 of wave 7: room in AGENTS.md, the atmosphere fix, seams, the quality foundation, slots, the look-dev harness, the roadmap and the budgets

Step 0 runs on branch `agent/step0` in `G:/GameEngineAI/.worktrees/step0` (build tree `build/windows` of that worktree),
after the five commits already there (`git log --oneline bc6ee123..HEAD`: `e65353b5` the CommandLineOptions test's
static table for MSVC 14.50, `c6aea916` the validation layer retried whatever `vkCreateInstance` fails with, `5398f10e`
`StrataRuntime --seconds`, `9953c8b9` `Render.UI` registered with its module, `4409b03b` Debug whole-scene checks paced by
`Core/AmortizedCheck`). Those commits touch no renderer file except `R/RenderScene.cpp/.h`, so every renderer line
number below is the same at `bc6ee123` and at the branch's tip; `StrataRuntime/src/RuntimeApplication.cpp` changed
(`--seconds`), and its lines are given at the branch's tip (503 lines).

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `E/` is `StrataEditor/src/Editor/`, `T/` is
`StrataTests/src/`, `DG` is `Docs/DeveloperGuide.md`, `plan` is `.worktrees/wave7-plan/plan.md`.

Rules that hold for the whole step (AGENTS.md, plan 4.1, and this machine):

- Until the perf campaign of S0.8 has ended (`python Tools/PerfLock.py --status` names no holder, and
  `.worktrees/wave7-plan/perf-campaign-bc6ee123/run5.json` exists; planned until about 03:00 on 2026-10-27), nothing is
  built and no test runs: only reading, docs and Python written without running editors.
- Every commit: Debug build with warnings as errors, `ctest --preset windows-debug -j 8` and every `GPU*` suite under the
  validation layer on the RTX 5090, `python Tools/CompatLint.py --changed-since bc6ee123` clean. Release `ctest
  --preset windows-release -LE perf` before the branch goes to `ci/step0`.
- Perf measurements only under `python Tools/PerfLock.py --label step0 -- ...`, never while a build runs.
- `build/windows` of the main checkout stays at bc6ee123 until step 0 lands on main: it is the A side of every A/B and
  the build of the 0.6.0 look-dev baseline. Rebuild it (the owner judges the editor from it) only after main moves.

## 1. The owner's request and step 0's goal

The owner (2026-10-26, plan section 1): "the current image quality is simply not great. The shadows are very pixelated,
maybe add more settings to improve that", never TAA upscaling, "great realistic graphics, which doesn't mean sharp or
clinical", the Fox Engine (MGS V) as the baseline look, native resolution only. Step 0 makes the ground the four streams
build on; in order of priority:

1. **Quality people, agents and players can choose (S0.4).** One `RendererQualitySettings` with today's costly knobs, a
   `Renderer.Quality` project section (Low, Medium = 0.6.0's costs, High = the default, Ultra, Custom) that every
   program follows and changes while it runs, captures that match the views they capture, and High's 4096-texel sun
   cascades from 1440 output rows: the first visible answer to "the shadows are very pixelated" (one shadow texel per
   1.07 screen pixels at every cascade's far end at 2160 rows, from 2.15; `shadows/cascades_2048.txt`'s invariant
   4400 / size at 60 degrees).
2. **A bug in shipped code fixed (S0.2):** the atmosphere's black reflections after a sky-source switch.
3. **The tool every stream proves its change with (S0.6):** the look-dev harness and the 0.6.0 baseline the owner's
   before/after sheet starts from.
4. **Parallel streams that only add (S0.1, S0.3, S0.5):** room in AGENTS.md for four digest bullets, post-processing and
   ambient occlusion in files of their own, scene binding slots and GPU stage names assigned.
5. **The record (S0.7, S0.8):** the roadmap of the wave, and perf budgets measured again on the replaced CPU with an A/B
   proving that S0.3 and S0.4 cost nothing at Medium.

Not the goal: any new image feature (each stream's), the Graphics Quality page, players' options and the script API
(`graphics-options`), any change of pixels at Medium.

## 2. Where we start

### 2.1 Files

| File | Lines | Role in step 0 |
| --- | --- | --- |
| `AGENTS.md` | 39,962 code points | `Docs.AgentGuide` allows 40,000 (`T/Docs/AgentGuideTests.cpp:25`): 38 free (the plan's "22" counted bytes, 39,978) |
| `Docs/DeveloperGuide.md` | 4,017 | Full wording of the rules AGENTS.md shortens; new sections of S0.4 and S0.6. Its line numbers in this spec are the branch tip's (the step-0 commits added one line near 153 and three near 845, so they are 4 more than bc6ee123's after line 860); `Docs/Architecture.md`'s likewise |
| `R/SceneRenderer.h` / `.cpp` | 2,035 / 3,853 | Specification (100-249), stats (356-), class (573-); `RenderFrame` (2676-3339) |
| `R/SceneRendererExtraction.cpp` | 1,354 | `FindShadowCasters` (cascade count 425, map size 439, `m_CascadeCount` 420/508), `MinProjectedPixels` (962), `ShadowParams` (1174) |
| `R/SceneRendererGPUDriven.cpp` | 1,601 | Cull constants (705, 740), `EnsureShadowPyramid` (1036-1080), `RunShadowOcclusionPhase` (1090), cache key (1234-1244), `PrepareShadowCache` (1266-1337), `CommitShadowCache` (1342-1353), `DrawGPUShadowBuckets` (1389) |
| `R/SceneRendererLocalShadows.cpp` | 897 | Tile constants (36-37), ranking (238), tile sizing (281-298), LOD (562), `EnsureLocalShadowAtlas` (793-814) |
| `R/SceneRendererAtmosphere.cpp` | 345 | `UpdateAtmosphere` (195-344): tables (233-262), environment (264-318) |
| `R/SceneRendererScreenReflections.cpp`, `...Glass.cpp`, `...ReflectionProbes.cpp` | 157, 122, 394 | Wave 6's split files: the pattern S0.3 follows |
| `R/RendererRegistration.cpp` | 123 | `RegisterRendererModule` (63-): the section registers here |
| `R/GPUProfiler.cpp` | 377 | `RendererGPUStages::Get` (44-61): registered in a fixed order that sets the stages' ids and perf-log columns ("Registered in this order, so the stages have the same ids (and perf log columns) in every program", 46) |
| `R/SceneRenderData.h` | 508 | `c_MaxCascades` 4, `c_MaxShadowedLocalLights` 16 (237) |
| `Strata/src/Strata/Core/SettingsRegistry.h`, `ProjectSettings.h` | 136, 100 | Simple sections (missing members take the field's default at any version, `SettingsRegistry.cpp:213-217`), `IProjectSettings` (versions unique across providers, `ProjectSettings.h:32-34`) |
| `Strata/src/Strata/Scene/ProjectCollisionLayers.cpp` | 114 | The model of a module reading its section by version (`Find`, 91-113) |
| `Strata/src/Strata/Runtime/GameRenderer.h` / `.cpp` | 65 / 156 | The game's renderer (spec 23-29, `Render` 50-) |
| `StrataRuntime/src/RuntimeApplication.cpp` (branch tip) | 503 | Options (48-57), renderer (98), render call (169), `c_Arguments` (328-341), `CheckOptions` (355-) |
| `E/ViewportRenderer.h` / `.cpp` | 151 / 242 | The editor's view renderer; capture atlas override (79-89) |
| `E/EditorViewport.h` / `.cpp` | 273 / 590 | `ViewportSettings` (87-139; JSON 33-130), renderers (321-340) |
| `E/EditorViewportCommands.cpp` | 1,149 | `viewport.setSettings` (887-970), `viewport.capture` (979-1147), `StartCapture` (586-) |
| `E/EditorStateCommands.cpp` | 1,039 | `DescribeRenderStats` (206-250), `editor.perf` (871-922) |
| `E/ThumbnailCache.cpp`, `E/ProbeBake.cpp`, `E/ProjectThumbnail.cpp` | 1,265, 161, 206 | Hand-made specifications (854-867, 135-147); the project's picture (114-146) |
| `StrataEditor/src/Panels/ViewportPanel.cpp`, `GameViewPanel.cpp` | 958, 327 | View render calls (241, 122) |
| `SH/Include/Scene.glsl` | 371 | The scene layout's slots |
| `StrataTests/Perf/Budgets.json` | 189 entries | 111 measured, 78 provisional (most added on the Mac in waves 4-6) |
| `Tools/PerfStability.py`, `PerfLock.py` | 334, 179 | Campaign and lock |
| `T/Renderer/GPUAtmosphereTests.cpp` | 416 | `GPU.Atmosphere` (S0.2's test goes here) |
| `T/Editor/GPUEditorViewportTests.cpp` | 1,923 | `GPU.Editor.Viewport` (capture tests) |
| `T/Runtime/GPUGameRendererTests.cpp` | 288 | `GPU.Runtime.GameRenderer` |
| `T/Perf/RendererPerfTests.cpp` | 716 | `PerfGPU.Renderer` |

### 2.2 Facts each item depends on (checked at bc6ee123)

- **The specification is fixed for a renderer's life.** `SceneRenderer` has only `GetSpecification` (`R/SceneRenderer.h:719`);
  the constructor clamps `ShadowCascades` to 1-4 and `ShadowMapSize` to 256-8192, floors `LocalShadowAtlasSize` to a
  power of two in 512-8192 and resets the atlas allocator with 128-texel tiles (`R/SceneRenderer.cpp:235-242`).
- **The cascades always have four layers and live as long as the renderer.** `EnsureShadowMap` makes a D32 array of
  `c_MaxCascades` layers of `ShadowMapSize` and a framebuffer per layer, once (`R/SceneRenderer.cpp:1681-1715`); the
  occlusion pyramid likewise (`R/SceneRendererGPUDriven.cpp:1036-1080`); `ReleaseRenderTargets`
  (`R/SceneRenderer.cpp:1938-2034`) releases neither. Every shader loop is bounded by the frame's cascade count
  (`SH/Include/Shadows.glsl:35-36`, `SH/Scene/CascadeFit.comp:39`, `SH/Scene/Cull.comp:249, 323, 411, 557`), so an array of
  `ShadowCascades` layers is enough.
- **The binding sets already follow a new map.** The scene sets compare `m_BoundShadowMap` and `m_BoundLocalShadowAtlas`
  (`R/SceneRenderer.cpp:2938-2939`), the culling set `m_CullBoundShadowPyramid` (`R/SceneRendererGPUDriven.cpp:871, 883`).
- **The cascade cache.** Its key holds the camera, viewport, light view, box, cascade count, `ShadowMapSize` and the LOD
  criteria (`R/SceneRendererGPUDriven.cpp:1234-1244`), its content the shadow map's pointer (`R/SceneRenderer.h:1937-1945`,
  set at `R/SceneRendererGPUDriven.cpp:1280`); the CPU path redraws every layer (`R/SceneRenderer.cpp:3059`).
- **Ten places read the shadow map's size:** `R/SceneRenderer.cpp:1687-1688, 1838`; `R/SceneRendererExtraction.cpp:439,
  1174`; `R/SceneRendererGPUDriven.cpp:740, 1040, 1090, 1289, 1389`. Two read the cascade count
  (`R/SceneRenderer.cpp:235`, `R/SceneRendererExtraction.cpp:425`), two `LODErrorPixels` (`R/SceneRenderer.cpp:1725`,
  `R/SceneRendererLocalShadows.cpp:562`), two `MinProjectedPixels` (`R/SceneRendererExtraction.cpp:962`,
  `R/SceneRendererGPUDriven.cpp:705`), one `MaxShadowedLocalLights` (`R/SceneRendererLocalShadows.cpp:238`).
- **The local tile cap is a constant:** `c_MaxLocalShadowTile = 1024` (`R/SceneRendererLocalShadows.cpp:37`), used at 281.
- **Four hand-made specifications drifted apart:** the game's (`Strata/src/Strata/Runtime/GameRenderer.cpp:25-29`), the editor's views'
  and the capture renderer's 2048 atlas (`E/ViewportRenderer.cpp:79-89`), the thumbnails' 1024 x 1 cascade with a 2048
  atlas (`E/ThumbnailCache.cpp:854-867`), the bake's 2048 atlas (`E/ProbeBake.cpp:135-147`).
- **Settings.** Simple sections validate, default and upgrade from their fields (`Strata/src/Strata/Core/SettingsRegistry.h:44-90`);
  wave 6's generic page draws any simple section (`StrataEditor/src/UI/SettingsPages.cpp:146-220`); the editor and
  `GameRuntime` provide `IProjectSettings` (`E/EditorContext.cpp`, `Strata/src/Strata/Runtime/GameRuntime.cpp:51`); the
  feature test fails when a registered project section is not set, or set to its default, in `FeatureTest.stproj` (DG
  "Feature test").
- **The plan's "about 80 test sites" are 13 lines.** The six knobs that move appear in tests only at
  `T/Renderer/GPUSceneLocalShadowTests.cpp:379, 386, 631, 645`, `T/Renderer/GPUSceneRendererGPUDrivenTests.cpp:89, 619,
  1085`, `T/Renderer/GPUSceneShadowTests.cpp:518, 540`, `T/Renderer/GPUSceneTAATests.cpp:291`,
  `T/Editor/GPUEditorViewportTests.cpp:1461`, `T/Editor/GPUStreamingViewTests.cpp:468`, `T/Editor/GPUThumbnailTests.cpp:852`
  (plus comments). The 95 `SceneRendererSpecification` constructions in 32 test files are what keeps its default Medium:
  they keep their pixels without an edit.
- **No `Perf.Renderer` suite exists.** The renderer's CPU metrics are the non-`GPU` entries of `PerfGPU.Renderer`
  (`T/Perf/RendererPerfTests.cpp`); the plan's "`Perf.Renderer.*`" means them.
- **Perf tests that would see the new default:** the editor's frame tests open their projects in a maximized window
  (`T/Editor/EditorFramePerfTests.cpp:214, 251, 379`), whose Scene view has at least 1440 rows on the reference machine's
  3840x2160 display, with a shadowed sun (`T/Perf/SceneGenerators.cpp:127-138`); the stress project too
  (`T/Perf/StressProject.cpp:323-327, 480`). The tour and the streaming game run at 1920x1080
  (`T/Perf/SamplePerfTests.cpp:69`, `T/Perf/StreamingPerfTests.cpp:219`), where High equals Medium.
- **No CTest runs Python today**, and AGENTS.md's tests "depend on no program outside the build" (`AGENTS.md:123`); the
  package tests' CMake and compiler are the documented exception (`AGENTS.md:132`).

## 3. Order and the cut line

The plan lists S0.1 to S0.8. Two changes of order, both for the proof they enable (section 14, Q1):

1. **S0.1** room in AGENTS.md (docs only: can run while the campaign measures).
2. **S0.6** the look-dev harness (capture mode, `compare`, stills), then the **0.6.0 baseline** with `build/windows`: the
   instrument that proves S0.3 and S0.4 change no pixel (a capture set of the branch is taken right before S0.3).
3. **S0.8a** budgets from the campaign (any time after it ends; independent of the code).
4. **S0.2** the atmosphere fix.
5. **S0.3** the seams.
6. **S0.4** the quality foundation (commits in section 7.14).
7. **S0.5** slots and stage names (one comment and DG lines).
8. **S0.7** the roadmap and the wave's plan and specs in `Docs/Plans/Wave7/`.
9. **S0.8b** the interleaved A/B of S0.3 and S0.4 at Medium; the step-0 baseline of the harness.

**Cut line.** Everything above is needed before the streams branch, except, in this order, what may follow on main while
they run (nothing in the streams depends on it): the harness's live shots and its HTML before/after sheet (stills,
crops, the stats table and `compare`'s numbers are above the cut); `PerfStability.py --self-test`'s CTest (the modes
themselves are above). Main gets step 0 after Debug and Release ctest pass on NVIDIA and CI is green on `ci/step0`.
`camera-and-tone` judges motion blur on live shots (its spec's 5.6.5 appends three pan shots): if the live mode follows
the cut, it lands on main before that stream's step 6, which merges main to get it. In that case only, so that the live
shots' 0.6.0 baseline can still be taken, a Release build of bc6ee123 is kept for baselines in
`G:/GameEngineAI/build/baseline-0.6.0` (from `git archive bc6ee123`, built once the campaign has ended and before
anything rebuilds `build/windows`; copying `build/windows/bin/Release` instead is unverified to run from another folder).

## 4. S0.1 Room in AGENTS.md

Checklist item 5: a rule this file shortens is stated in full in the guide, a rule kept whole is only here. Every
bullet below already has its full wording in the guide (checked: the sections named), so the guide gains nothing but
the two lines marked; AGENTS.md keeps each rule's musts and its link.

| AGENTS.md (branch tip) | Before | After | Saves | Full wording in DG |
| --- | --- | --- | --- | --- |
| Testing, unit tests (102-106) | 571 | 344 | 227 | "Unit tests", "Shards, sweeps and locks" |
| Testing, perf tests (112-118) | 724 | 523 | 201 | "Perf tests" (ratchet, 1.5x, medians, helper processes, stability) |
| Testing, feature test (127-131) | 558 | 426 | 132 | "Feature test" ("The feature test enforces coverage") |
| Architecture, layers (222-228) | 762 | 405 | 357 | "Architecture rules", Layers |
| Architecture, module registration (229-238) | 1,103 | 540 | 563 | "Architecture rules", Module registration |
| Architecture, scene caches (243-250) | 917 | 542 | 375 | "Architecture rules", Scene caches |
| Asset pipeline, importers (264-269) | 571 | 282 | 289 | "Importers" (1245-) |
| Asset pipeline, uploads and slots (285-289) | 510 | 250 | 260 | "Adding an asset type" (1395), "Mip streaming" (1410-) |
| Editor, command conventions (367-373) | 746 | 341 | 405 | "Command conventions", "Undo", "Edits while playing" |
| Editor, prefab instances (385-390) | 575 | 371 | 204 | "Prefab instances", "Autosave and recovery", "Opening scenes" |
| **Total** | | | **3,013** | |

The replacements, exactly (counts measured with the test's code-point rule on the branch tip's file):

```markdown
- Unit tests live in `StrataTests/src/<Module>/*Tests.cpp` (doctest), in suites named after the module; a new suite
  prefix gets a shard (`StrataTests/CoreShards.json`) and corruption sweeps go in `*.Sweep` suites ([Unit
  tests](Docs/DeveloperGuide.md#unit-tests), [Shards, sweeps and locks](Docs/DeveloperGuide.md#shards-sweeps-and-locks)).
- **Perf tests** (`Perf.*`, `PerfGPU.*`, label `perf`, Release and Dist; CI skips them): limits live only in
  `StrataTests/Perf/Budgets.json`, measured on the reference machine (else `Provisional`); **budgets only go down** (at
  most 1.5 times the measurement; commits that add or lower one state the measured values); there, run `-L perf` before
  merging what can change performance, `Tools/PerfStability.py --runs 5` before merging changes to budgets or perf tests
  ([Perf tests](Docs/DeveloperGuide.md#perf-tests)).
- The **feature test** (golden rule 6, `StrataTests/FeatureTest/`) fails when something registered (a component, a
  property's non-default value, a settings section, a script field type, callback, host function or SDK name, an asset
  type) goes unexercised, a feature script fails, or the run logs a warning or error it does not expect ([Feature
  test](Docs/DeveloperGuide.md#feature-test): what counts, how to extend it).
- **Layers:** every file under `Strata/src` belongs to a layer of `StrataTests/Architecture/Layers.json` and includes
  across layers only what it allows (`Architecture.Layering`). Never add a `LayeringAllowlist.txt` line or widen
  `MayInclude` to make a sideways or upward include pass: invert the dependency (an interface below, a registry, a scene
  or engine service); games link no `Tooling` layer.
- **Module registration:** modules extend the engine through registries (components, asset loaders and importers, scene
  systems, settings sections), never through lower layers calling higher ones. A component lives in its module
  (`<Module>/<Module>Components.h`); each module has one `Register<Module>Module`, called only by the composition root,
  `Engine::RegisterBuiltinModules`; games and tests pass theirs in `ModuleRegistrationOptions::Extra`; scene systems
  declare their order and are made with `MakeSceneSystemDescriptor<T>`.
- **Scene caches and the transform contract:** `Scene` caches world transforms, the hierarchy, activity and the name and
  tag indexes, so **code that writes `TransformComponent` fields directly must signal the change**
  (`Entity::MarkModified<TransformComponent>()`, `registry.patch` or `Scene::MarkTransformChanged`), and so must writes
  to names, tags, `MeshRendererComponent` and light fields. Hierarchy changes go through `Scene` (`SetParent`,
  `DestroyEntities`...), never by editing `RelationshipComponent` or `HierarchyComponent`.
- **Bump an importer's `GetVersion()` whenever its output changes** (cooked textures: `TextureImporter` and
  `GltfImporter`); importers read files only through `ReadImportSource` and `ReadImportDependency`
  ([Importers](Docs/DeveloperGuide.md#importers): what else they must do).
- GPU uploads go in steps of at most `c_AssetUploadStepBytes` within the frame's budget, and bindless slots are resolved
  every frame (`Texture::GetBindlessSlot`: mip streaming changes them; [Mip
  streaming](Docs/DeveloperGuide.md#mip-streaming)).
- Commands (`<group>.<action>`) have a description written for agents, a complete JSON Schema and a kind
  (`EditorCommandKind`); they reject invalid input without side effects, track entities in a `SceneEditTransaction` and
  record one undo step per successful mutation ([Command conventions](Docs/DeveloperGuide.md#command-conventions)).
- A scene being opened is read-only (`EditorContext::CheckSceneEditable` guards edits outside `CommitEdit`); code that
  writes a prefab, syncs its instances or records scene undo steps keeps them exact (`PrefabSync::SetBaseline`,
  `PrefabInstances::MarkSynced`, `GetSceneChange`, `UpdatePrefabOverrides`: [Prefab
  instances](Docs/DeveloperGuide.md#prefab-instances)).
```

What each shortened bullet leaves only to the guide (the implementer checks each against the guide's text before the
commit and moves any phrase the guide lacks into it, in the section named): deferred commands through
`Tests::RunCommandToCompletion`, tabs clicked once shown, a test's own component in a child process; "lower is better",
"a metric over budget is a regression to fix", medians and maxima, peak memory in a helper process; the feature
test's list in full; proven downward dependencies, `MaxAllowlistEntries`, the table as the build; `IProjectSettings` on
the main thread, `Scene/Components.h`'s contents, the order of registration, tooling handed in, overrides public,
`UnknownComponentsComponent`; `on_update`-free signals, values written right after `AddComponent`, Debug assertions,
`GetSiblingIndex`, one `DestroyEntities` call, `c_MaxEntities`; `FindExternalFiles`, `MakeValidUTF8`, `IsCancelled`,
`GetSettingsProperties`, `DeriveSubAssetHandle`; `GetTableIndex`, `RequestLevel`, `TextureStreamer::Hold`; marking queries
and session results, `CommandArguments`, `InvalidParameters`/`Failed`, `Track`/`TrackCreated`, `EditorActionGroup`, the
`warning` and `RunningCopy` of scene changes while playing; the recovery journal's whole-scene snapshot. DG's
"Architecture rules" opening sentence ("seven of them in short") stays true.

Step 0's own additions (section 7.13): the Rendering digest's quality bullet, 232 code points, and 32 on the package
tests' line. **Acceptance:** after S0.1, 36,949 code points (3,051 free); when the streams branch, 37,213 (2,787 free,
about 690 per stream; the plan's floor is 2,500; the streams' specs ask for about 330, 380, 480 and 400: about 1,600 in all). `Docs.AgentGuide`
passes (its link check counts the new links: every target above exists). The perf bullet keeps the rule's two
occasions apart: `-L perf` before merging what can change performance, `PerfStability.py` only before merging changes to
budgets or perf tests (plan 4.1 keeps `PerfStability.py` out of the streams; a shorter text that asked for it before every
performance-relevant merge would have changed the rule, not shortened it).

Commit: "Make room in AGENTS.md: the guide states ten rules in full" (body: the table above).

## 5. S0.2 The atmosphere's black environment after a sky-source switch

### 5.1 What is known

The lighting audit (L6): in its Test scene, (1) Atmosphere with the sun active, a mirror sphere reads sRGB (44, 77, 120);
(2) switch the Sky Light to Procedural and deactivate the sun; (3) reactivate the sun and switch back to Atmosphere;
(4) the mirror reads (0, 0, 0) but for the sun's highlight, and surfaces up to roughness about 0.5 lose their
reflections. It survives `scene.open` of the saved scene and clearing the sky's `EnvironmentMap`; turning the sun beyond
the refresh angle fixes it; a fresh atmosphere scene is fine (`lighting/cap/spheres_atmo_close2_small.png`,
`test_reopen_mirror.png`, `test_sunrot_mirror.png`, `fresh_atmo_mirror.png`). The auditor's repro script was not kept;
the captures came from the headless editor's capture renderer.

Reading the code does not explain it (cause **unverified**): a switch away from the atmosphere destroys its state
(`R/SceneRenderer.cpp:864-865`: `if (!atmosphere) m_Atmosphere.reset()`), so the switch back takes the first-environment
path with fresh tables (`R/SceneRendererAtmosphere.cpp:233-234`: `TableSettings` is empty, `SkyViewValid` false) and new
maps (`takeMaps`, 267-275: no `Spare`, `CreateEnvironmentMaps`) and records all 14 steps in that frame (277-285). That
the black survives `scene.open` and is cured by a refresh says the maps the renderer keeps are wrong, not their binding.
Candidates to rule in or out, in this order:

1. **A baked reflection probe** in the audit's Test scene (the auditor baked probes there for the room, L1): a probe
   captured while the sky was procedural with the sun off would be dark, reopen with the scene, and dominate a mirror
   inside its box. A sun rotation curing it argues against this; the first check is whether the scene had a
   `ReflectionProbe` (the repro in 5.2 has none, so a pass there with a failure in the editor points here).
2. **Work recorded but not executed:** a frame that records environment steps and returns without `ExecuteFrame`
   (every path executes: 2989-2999, 3018-3027, the HDR output at 3183-3204, 3248-3262, the end at 3321-3325; the early
   return at 2961 precedes `UpdateEnvironment`).
3. **The sky-view constants:** `WriteAtmosphereConstants` runs only when the sky view is recomputed (235-236); a first
   environment computed in a frame that did not write them reads the constants of an older configuration.
4. **Binding:** the scene sets are rebuilt when the environment's *cube* changes (`m_BoundEnvironment`, 2986), not its
   irradiance or prefiltered maps (bound at 990-991). No path is known that replaces those two alone, but the check is
   one comparison short of exact.
5. **The editor's renderers:** the capture renderer renders only when asked, so it may never see the procedural frames
   (it would then keep the first environment, which was fine); the Scene view renders every frame while the panel shows.

### 5.2 Procedure

1. Write the regression test first (5.4) at the renderer level, both with `SpreadLightingUpdates` (view-like) and without
   (capture-like). If it fails today: instrument with the stats the renderer has (`EnvironmentUpdates`,
   `AtmosphereTableUpdates`, `AtmosphereSkyViewUpdates`, `EnvironmentRefreshSteps`) and read back the cube, irradiance and
   prefiltered maps (`GetEnvironmentCube`, `GetIrradianceMap`, `GetPrefilteredMap`) after each step to find the first that
   goes black.
2. If it passes, reproduce through the editor in `GPU.Editor.Lighting` with the audit's command sequence on the capture
   renderer (and the Scene view's renderer), then with a probe baked between the steps; the step that turns the mirror
   black names the cause.
3. Fix the cause found. Its commit message states it, with the step of 5.1 it confirms.

### 5.3 The fix, whatever the cause

- The cause found, fixed where it lies.
- **A sun's activation or deactivation under the atmosphere computes the next environment at once**, as a source switch
  does: `AtmosphereState` (not the renderer, so a switch away resets it with the rest) gains `std::optional<bool> HadSun`
  (`hasSun` of its previous frame; empty in its first). The first-environment branch (`SceneRendererAtmosphere.cpp:277`)
  is taken when `!m_Environment || !m_Environment->Atmosphere || (atmosphere.HadSun && *atmosphere.HadSun != hasSun)`.
  Before `takeMaps()` in that branch, the maps in hand are kept, never dropped: a refresh under way gives its maps to the
  spare (`atmosphere.Spare = std::move(*atmosphere.Refresh); atmosphere.Refresh.reset();`), and otherwise, when the
  current environment is the atmosphere's and there is no spare, the current maps become the spare (as a completed refresh
  does, 307-313), so `takeMaps` reuses them and allocates nothing. `HadSun = hasSun` at the end of every atmosphere
  frame. So the scene is never lit for 14 frames by the environment of the 1-lux default sun (or of a sun that is gone).
  Cost: that frame records the 14 steps a source switch records today (counted by `EnvironmentUpdates`, not
  `EnvironmentRefreshSteps`); toggling the sun is rare (no sample does it: `Samples/OpenWorld/Scripts/DayNight.cpp`
  changes the intensity only).
- **The rebind follows all three maps:** `m_BoundEnvironment` becomes three pointers (`m_BoundEnvironmentCube`,
  `m_BoundIrradiance`, `m_BoundPrefiltered`), compared at 2986 with the current maps (the binding set holds them, so their
  addresses cannot be reused while it lives).
- **An intensity change from 0 changes no table and starts no refresh** unless the test proves it must: the tables are
  per lux of sun (214-221) and the existing case "Changes of the light's intensity alone recompute none of the
  atmosphere's tables and refresh none of its environment" (`T/Renderer/GPUAtmosphereTests.cpp:328-366`) holds that
  contract. The test covers intensity 0 and back; if it fails there, the fix makes that step a refresh and the existing
  case's comment says why.

### 5.4 Tests

`GPU.Atmosphere` (`T/Renderer/GPUAtmosphereTests.cpp`), new case **"Switching the sky's source and the sun back and forth
lights the scene as a fresh renderer does"**: a scene with `AddNeutralPostProcess`, the atmosphere, a 50,000 lux sun at
30 degrees, a mirror sphere (base color 1, metallic 1, roughness 0) and a sphere of roughness 0.4 in view, pre-exposure
forced to 1/65536 (as the neighboring cases), a 128 x 128 viewport. For each of four sequences, rendered by a renderer with
`SpreadLightingUpdates` and one without:

1. Atmosphere and sun; Procedural; sun inactive (`Entity::SetActive(false)`); sun active; Atmosphere (the audit's order).
2. Atmosphere and sun; sun inactive; Procedural; Atmosphere; sun active.
3. Atmosphere and sun; sun intensity 0; Procedural; Atmosphere; intensity back.
4. Atmosphere without a sun; sun added (activation from none).

Each step renders one frame; after the last, frames until `EnvironmentRefreshPending` is false (at most
`c_AtmosphereRefreshSteps + 1`). The image is compared with a fresh renderer's of the final scene after the same number
of frames: at the mirror's and the rough sphere's center 5 x 5 pixels, every channel within 1/255; the mirror's center
luminance above 0.05 (not black). Must fail without the fix if 5.2's step 1 reproduced the bug; if only the editor
reproduces it, the editor case of 5.2 step 2 is the regression test (`GPU.Editor.Lighting`, "A sky switch and back keeps
the atmosphere's reflections in captures"), with the same comparison against a capture of a freshly opened copy.
Both end with `CHECK(gpu.GetNewErrorCount() == 0)`.

### 5.5 Docs and release note

DG "Fog and atmosphere" (3476-): what a source switch and a sun's (de)activation recompute. Release-note line (0.7.0,
"Smaller changes and fixes"): "The atmosphere keeps its reflections after the sky switches to another source and back
(0.6.0 could leave mirrors black until the sun moved)."

## 6. S0.3 Seams that make the streams additive

Moves only: no line of logic changes, so no pixel and no command order changes. Members stay declared in
`R/SceneRenderer.h`, regrouped under a comment naming their file, as wave 6's split files did.

### 6.1 Post-processing: `R/SceneRendererPost.cpp`

What moves (functions and blocks, in their order):

| From | To |
| --- | --- |
| `CreateResources`: the tone-mapping layout (`R/SceneRenderer.cpp:319-329`), the exposure and histogram layout, pipelines, histogram, exposure buffer and readbacks (492-523), the bloom layout and pipeline (525-533), the FXAA layout (553-556) | `void SceneRenderer::CreatePostResources()`, called from `CreateResources` where 492 was; it verifies what it made with `ST_CORE_VERIFY` (the shared verify at 559-565 drops those names) |
| `CreateRenderTargets`: the LDR target (2055) and framebuffer (2106), the exposure set (2112-2120), the bloom chain and its sets (2122-2172), the tone-mapping and FXAA sets (2174-2182) | `bool SceneRenderer::CreatePostTargets(uint32_t width, uint32_t height, std::string& outFailure)`, called where 2112 was; `CreateRenderTargets` reports `outFailure` through its `fail` |
| `ReleaseRenderTargets`: exposure set, bloom texture and sets, tone mapping, LDR, FXAA (1963-1975) | `void SceneRenderer::ReleasePostTargets(std::vector<nvrhi::ResourceHandle>& released)`, called at the same place |
| `UpdateExposure` (1351-1404), `RenderBloom` (1539-1564), `SubmitExposureReadback` (1462-1469) | the same functions, in the new file |
| `RenderFrame` 3206-3218 (resolved, exposure, bloom) | `bool SceneRenderer::RenderPostHDR(nvrhi::ICommandList*, const PostProcessComponent&, bool resolved)` returning whether bloom ran |
| `RenderFrame` 3220-3232 (tone-mapping parameters) and 3281-3293 (tone mapping, FXAA) | `void SceneRenderer::RenderPostDisplay(nvrhi::ICommandList*, const PostProcessComponent&, const TemporalAntiAliasingSettings&, bool resolved, bool bloom, nvrhi::IFramebuffer* output)` |
| the `drawFullscreen` lambda (3267-3279) | `void SceneRenderer::DrawFullscreen(nvrhi::ICommandList*, nvrhi::IGraphicsPipeline*, nvrhi::IFramebuffer*, nvrhi::IBindingSet*, const void* pushConstants, size_t pushConstantSize)` (also used by the overlays' copy, 3316) |
| the local `createComputePipeline` lambda (435-443) | `nvrhi::ComputePipelineHandle SceneRenderer::CreateComputePipeline(const char* shader, nvrhi::IBindingLayout* layout)` in `SceneRenderer.cpp` (the same verify), used by the three files |

`gpuTimer.Next(gpuStages.Post)` (3207) stays in `RenderFrame` before `RenderPostHDR`, and the text and overlay
preparation (3234-3265) stays between the two calls, so the commands and their GPU stages are recorded exactly as
before. The plan's "RenderPost" is this pair: `camera-and-tone` adds motion blur (and, below its cut, depth of field)
before `RenderPostHDR`'s exposure, under its own `gpuTimer.Next(gpuStages.MotionBlur)` in `RenderFrame` before the
`Next(Post)` (the timer is `RenderFrame`'s local: a pass that times its own stage is recorded there, or takes the timer
as a parameter), and the dither in `RenderPostDisplay`. What stays in `SceneRenderer.cpp`: the temporal history's own
post sets, one per history texture (`m_TemporalExposureSets`, `m_TemporalBloomSets`, `m_TemporalTonemapSets`, made
by `EnsureTemporalResources`, 1565-1646, and released at 2021-2023): they belong to the TAA's resources, and a change of a post
layout (`camera-and-tone`'s tone-mapping constant buffer) edits them there too. The file opens with a comment in the manner of
`SceneRendererGlass.cpp:12-15`: the passes from the TAA resolve to the display, what each reads and writes, and that
post passes take their parameters by push constants or buffers of their own, never `FrameConstants`.

### 6.2 Ambient occlusion: `R/SceneRendererAmbientOcclusion.cpp`

| From | To |
| --- | --- |
| `CreateResources`: AO layout and format choice, GTAO and blur pipelines (424-447; the cluster pipeline at 448 stays) | `void SceneRenderer::CreateAmbientOcclusionResources()` |
| `CreateRenderTargets`: the AO storage textures (2056-2069) and their sets (2072-2089) | `bool SceneRenderer::CreateAmbientOcclusionTargets(uint32_t width, uint32_t height)` (its failure "ambient occlusion targets"; the depth and normal targets it binds are made first, as now) |
| `ReleaseRenderTargets` (1955-1958) | `void SceneRenderer::ReleaseAmbientOcclusionTargets(std::vector<nvrhi::ResourceHandle>&)` |
| `RenderAmbientOcclusion` (1330-1349) | the same function |

The constants `c_AOFormat`, `c_AOFallbackFormat` move with them. `light-and-materials` owns the new file.

### 6.3 Proof that nothing changed

- Each move is its own commit (post, then AO), and no test file changes in them.
- **GPU suites** under validation, unedited: `GPU.SceneRenderer.PostProcess` (exposure, histogram metering, contrast,
  bloom with and without threshold, FXAA, AO creases and transparency, resize, external framebuffer, pre-exposure,
  `T/Renderer/GPUScenePostProcessTests.cpp:98-694`), `GPU.SceneRenderer.TAA` (resolve and sharpening in tone mapping),
  `GPU.SceneRenderer.GPUDriven` and `.Feedback` (the two paths equal), `GPU.SceneRenderer.Overlays` (the copy into a
  target), `GPU.SceneRenderer.SSR`, `GPU.Editor.Viewport` (captures), `GPU.Runtime.GameRenderer`, `GPU.FeatureTest`.
- **Images:** the harness (S0.6) captures every still shot at both sizes and all three anti-aliasing modes with the
  branch before the move and after it: `LookDev.py compare` reports 0 differing pixels in every shot (the two builds
  differ only by these moves; any difference is a mistake to find).
- **Cost:** the A/B of S0.8b.

Docs: DG "Scene renderer and lighting" names the two files with what they hold; Architecture.md's renderer file list
(if it lists the split files) gains them.

## 7. S0.4 The quality foundation

### 7.1 `R/RendererQuality.h` / `.cpp` (Renderer layer)

```cpp
#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Strata
{

	struct SettingsSection;

	// The presets of RendererQualitySettings. The numbers are stored and passed by scripts (graphics options): never
	// renumber them; a new preset takes the next number.
	enum class RendererQualityPreset : uint8_t
	{
		Low = 0,
		Medium = 1, // Strata 0.6.0's costs; SceneRendererSpecification's default
		High = 2,   // The programs' default (the Renderer.Quality section's)
		Ultra = 3,
		Custom = 4  // Values of the section's fields
	};
	constexpr size_t c_RendererQualityPresetCount = 4; // Low to Ultra: the presets with values of their own

	// The resolution of each sun shadow cascade: Auto and AutoFine follow the output's rows
	// (RendererQuality::ResolveShadowMapSize), the others are fixed.
	enum class ShadowMapResolution : uint8_t
	{
		Auto = 0,  // 2048 below RendererQuality::c_HighResolutionRows output rows, 4096 from them
		AutoFine,  // 4096 below, 8192 from them
		Size1024,
		Size2048,
		Size4096,
		Size8192
	};

	// The costly knobs of a renderer: what a person, project or player chooses for a machine, never a scene's look (that
	// stays on lights, Post Process and the sky). The defaults are Medium, 0.6.0's costs. Docs/DeveloperGuide.md,
	// "Quality settings": a row of the presets per knob, and what adding one involves.
	struct RendererQualitySettings
	{
		RendererQualityPreset Preset = RendererQualityPreset::Medium; // What these values are; reported, never compared
		ShadowMapResolution ShadowResolution = ShadowMapResolution::Size2048;
		uint32_t ShadowCascades = 4;           // 1 to RenderData::c_MaxCascades
		uint32_t LocalShadowAtlasSize = 4096;  // A power of two from 512 to 8192 (64 MiB at 4096)
		uint32_t LocalShadowMaxTile = 1024;    // A power of two from 128 to the atlas size: the largest tile a light view gets
		uint32_t MaxShadowedLocalLights = RenderData::c_MaxShadowedLocalLights; // 0 to c_MaxShadowedLocalLights
		float LODErrorPixels = 1.0f;           // SceneRendererSpecification's former field (its comment moves here)
		float MinProjectedPixels = 0.5f;       // Likewise

		// Every member equal, Preset included.
		bool operator==(const RendererQualitySettings& other) const = default;
		// Every knob equal (Preset is a label): copies whose Preset is made equal, compared with operator==, so a knob a
		// stream appends needs no line here.
		bool HasSameKnobs(const RendererQualitySettings& other) const;
	};

	namespace RendererQuality
	{
		constexpr std::string_view c_SectionId = "Renderer.Quality";
		constexpr uint32_t c_SectionVersion = 1; // Each stream's landing adds one (plan 4.1, "Quality rows")
		constexpr uint32_t c_HighResolutionRows = 1440;
		constexpr uint32_t c_MaxShadowMapSize = 8192;

		// A preset's values, labeled with it. Custom: High's values labeled Custom (the section's field defaults).
		RendererQualitySettings GetPreset(RendererQualityPreset preset);
		// The knobs within their ranges (what SceneRenderer applies): cascades clamped, sizes floored to powers of two
		// within their ranges, the tile at most the atlas, non-finite or negative pixels replaced by Medium's.
		RendererQualitySettings Clamp(const RendererQualitySettings& settings);
		// The cascades' texels each way for an output of `outputRows` rows (0: below the threshold).
		uint32_t ResolveShadowMapSize(ShadowMapResolution resolution, uint32_t outputRows);

		const char* PresetToString(RendererQualityPreset preset);                       // "Low" ... "Custom"
		std::optional<RendererQualityPreset> PresetFromString(std::string_view name, bool ignoreCase = false);
		const char* ShadowResolutionToString(ShadowMapResolution resolution);           // "Auto", "AutoFine", "1024" ...
		std::optional<ShadowMapResolution> ShadowResolutionFromString(std::string_view name);

		// The Renderer.Quality section (simple; registered by RegisterRendererModule).
		SettingsSection MakeSection();
		// The settings a validated value of the section stands for (null: the section's default, High): a preset's values,
		// or the fields' with Preset Custom.
		RendererQualitySettings FromSectionValue(const nlohmann::json* value);
		// A value of the section for these settings: their Preset and every field (a preset's own fields hold its values).
		nlohmann::json ToSectionValue(const RendererQualitySettings& settings);
		// The project's quality: the section read through IProjectSettings, kept until its version changes (a project that
		// does not set the section: its default, High). Medium, SceneRendererSpecification's default, while no program
		// provides the service: only tests and tools render so (the editor provides it whenever it runs, the game from its
		// manifest). Main thread only.
		const RendererQualitySettings& GetProjectQuality();
	}

}
```

`GetProjectQuality` keeps a function-local cache `{ bool Valid; uint64_t SettingsVersion; RendererQualitySettings Quality; }`
exactly as `ProjectCollisionLayers::Find` does (`Strata/src/Strata/Scene/ProjectCollisionLayers.cpp:91-113`); without a
provider it returns a function-local `static const RendererQualitySettings` of Medium (no cache entry). Why Medium there:
`GameRenderer` tests (`GPU.Runtime.GameRenderer`) run without a provider, and they keep their pixels when a stream's row
makes High differ from Medium below 1440 rows (plan 1: "`SceneRendererSpecification{}` stays Medium so tests keep their
meaning"). Every program that runs a project provides the service (`EditorContext.cpp:151`, `GameRuntime.cpp:51`), so
no player or person ever gets the fallback.

### 7.2 The presets: a knob per line

`RendererQuality.cpp` holds the table as typed rows, one line per knob with its four values in preset order, so each
stream's rows are separate lines (plan 4.1):

```cpp
		template<typename T>
		using PresetRow = std::array<T, c_RendererQualityPresetCount>; // Low, Medium (0.6.0), High, Ultra

		constexpr PresetRow<ShadowMapResolution> c_ShadowResolutionRow = { ShadowMapResolution::Size1024, ShadowMapResolution::Size2048, ShadowMapResolution::Auto, ShadowMapResolution::AutoFine };
		constexpr PresetRow<uint32_t> c_ShadowCascadesRow = { 3, 4, 4, 4 };
		constexpr PresetRow<uint32_t> c_LocalShadowAtlasSizeRow = { 2048, 4096, 4096, 8192 };
		constexpr PresetRow<uint32_t> c_LocalShadowMaxTileRow = { 512, 1024, 1024, 2048 };
		constexpr PresetRow<uint32_t> c_MaxShadowedLocalLightsRow = { 4, 16, 16, 16 };
		constexpr PresetRow<float> c_LODErrorPixelsRow = { 2.0f, 1.0f, 1.0f, 0.5f };
		constexpr PresetRow<float> c_MinProjectedPixelsRow = { 1.0f, 0.5f, 0.5f, 0.25f };
```

`GetPreset`, `MakeSection`, `FromSectionValue` and `ToSectionValue` each read a knob in one line, so a new knob is: a field
of the struct, a row, a line in each of those four functions, a field of the section, `Clamp`'s line, the knob's
use in the renderer, and a test of its Medium value (`HasSameKnobs` and `renderStats` need no line: 7.1, 7.8). DG
"Quality settings" lists these touch points; 7.11's case 1 catches a forgotten one.

`GetPreset` builds the four presets once (a function-local `static const std::array<RendererQualitySettings, 4>`, each
line `settings.<Knob> = c_<Knob>Row[index];`) and returns a copy: the Scene view resolves its quality every frame.
`FromSectionValue` has this shape, so that a row can also read its field under every preset (the players' comfort rows
`graphics-options` and `camera-and-tone` add, which the presets never set):

```cpp
		RendererQualitySettings FromSectionValue(const nlohmann::json* value)
		{
			// The preset: the value's, else the section's default (High). Members are read with JsonUtils::Find; a missing or
			// unreadable member keeps the struct's value (callers pass validated values, which have every field).
			const RendererQualityPreset preset = ...;
			RendererQualitySettings settings = preset == RendererQualityPreset::Custom ? GetPreset(RendererQualityPreset::High) : GetPreset(preset);
			settings.Preset = preset;
			if (value && preset == RendererQualityPreset::Custom)
			{
				// One line per knob that follows the preset, in the rows' order.
				ReadField(*value, "ShadowResolution", settings.ShadowResolution);
				...
			}
			// Knobs read under every preset (none in step 0) follow here, one line each.
			return Clamp(settings);
		}
```

(`ReadField` is an overload set in the .cpp's anonymous namespace: Enum fields through their `FromString`, Int fields
as `uint32_t`, Float fields as `float`; the section stores `LocalShadowAtlasSize` and `LocalShadowMaxTile` as Enum options
"1024"... and the struct as numbers, so their `ReadField` parses the option.)

What the presets cost (D32 shadow maps, per view renderer with a shadowed sun):

| | Low | Medium (0.6.0) | High | Ultra |
| --- | --- | --- | --- | --- |
| Cascades below / from 1440 rows | 3 x 1024 (12 MiB) | 4 x 2048 (64 MiB) | 4 x 2048 (64 MiB) / 4 x 4096 (256 MiB) | 4 x 4096 (256 MiB) / 4 x 8192 (1 GiB) |
| Screen pixels per texel at each far end, 60 degrees | 2.15 at 1080 rows | 1.07 at 1080, 2.15 at 2160 | 1.07 at 2160 | 0.54 at 2160 |
| Local atlas / largest tile | 16 MiB / 512 | 64 MiB / 1024 | 64 MiB / 1024 | 256 MiB / 2048 |
| Shadowed local lights | 4 | 16 | 16 | 16 |
| LOD error / smallest drawn (pixels) | 2 / 1 | 1 / 0.5 | 1 / 0.5 | 0.5 / 0.25 |

High equals Medium in every knob below 1440 rows (7.11 checks it), so every test, perf scene and 1080p view keeps
0.6.0's work under the new default.

### 7.3 Auto by output rows

`ResolveShadowMapSize(resolution, rows)`: Auto is `rows >= 1440 ? 4096 : 2048`, AutoFine `rows >= 1440 ? 8192 : 4096`, the
fixed values their size. The renderer resolves with its viewport's height (`m_ViewportSize.y`), its output: the Scene
view's and Game view's panel pixels, the window's back buffer for games, the image for captures. The result is then
limited by the device (`GraphicsDeviceInfo::MaxTextureDimension2D`, floored to a power of two) and by
`SceneRendererSpecification::MaxShadowMapSize` (new; 0 for no limit beyond 8192: "tests set it low to check the limit",
as `MaxDrawsPerIndirectCall` is). A resize across 1440 rows changes the size: the resize already makes the targets again,
and the cascades follow (7.5). There is no hysteresis: rows change only on resizes, which rebuild the targets anyway.

### 7.4 The specification and the named profiles

`SceneRendererSpecification` (`R/SceneRenderer.h:98-249`):

- `LODErrorPixels` (103), `MinProjectedPixels` (141), `ShadowMapSize` (142), `ShadowCascades` (143),
  `LocalShadowAtlasSize` (219) and `MaxShadowedLocalLights` (220) move into **`RendererQualitySettings Quality;`** (its
  default: Medium). Their comments move to the struct; the specification's comment says the programs follow the
  project through `SetQuality`. (The plan said four fields; the two LOD knobs are rows too, and one place for each knob
  keeps `GetSpecification()` honest.)
- **`static SceneRendererSpecification ForThumbnails()`**: today's thumbnail renderer (`E/ThumbnailCache.cpp:854-867`):
  `DebugName` "Thumbnails", `EntityIDs` false, `Quality` { Custom, Size1024, 1 cascade, atlas 2048, tile 1024, 16 lights,
  1.0, 0.5 }, `PrefetchDistance` 0, both fades off, `TemporalAntiAliasing` false, `SpreadLightingUpdates` false.
- **`static SceneRendererSpecification ForBake()`**: today's bake renderer (`E/ProbeBake.cpp:135-147`): "ProbeBake",
  `EntityIDs` false, TAA off, `TextureStreaming` false, `SpreadLightingUpdates` false, both fades off, `Quality` { Custom,
  Size2048, 4, atlas 2048, tile 1024, 16, 1.0, 0.5 }. Fixed: a bake never depends on a person's settings, and keeping
  0.6.0's values keeps `GPU.Editor.Lighting`'s baked values (decision Q3).
- `E/ThumbnailCache.cpp` and `E/ProbeBake.cpp` construct from them (only `DebugName` and what they set today differently
  remains at the call site: nothing).

### 7.5 `SceneRenderer::SetQuality`

```cpp
		// Changes the quality knobs (RendererQualitySettings, clamped as the constructor clamps the specification's) while
		// the renderer lives. Only what a changed knob sizes is made again, released through Renderer::ReleaseDeferred: the
		// cascades (their ShadowCascades layers at the resolved size, framebuffers, occlusion pyramid and cache) when the
		// resolved size or the count changed, the local shadows' atlas (with its tiles, views and cache) when its size
		// changed; a changed tile cap draws every light's views again; the other knobs apply from the next frame. Returns
		// whether anything was released (the temporal history then starts again). GetSpecification().Quality is the result.
		bool SetQuality(const RendererQualitySettings& quality);
		const RendererQualitySettings& GetQuality() const { return m_Specification.Quality; }
		// The cascades' texels each way at the current viewport (the quality's resolution resolved and limited).
		uint32_t GetShadowMapSize() const { return m_ShadowMapSize; }
		// The cascades' depth array (ShadowCascades layers): null until a frame draws a shadowed sun, and after the
		// cascades were released (SetQuality, a zero viewport).
		nvrhi::ITexture* GetShadowMap() const { return m_ShadowMap; }
```

New private state: `uint32_t m_RequestedShadowMapSize` (the quality's resolution resolved for the rows and limited: what
`SetQuality` and resizes compare), `uint32_t m_ShadowMapSize` (what `EnsureShadowMap` makes and every reader of the size
uses: the requested size, or less after an allocation failure), both set by the constructor, `SetViewportSize` and
`SetQuality`; `uint32_t m_ShadowMapLayers` (the layers the current map has). Comparing the requested size, never the made
one, keeps a failed 8192 allocation from being released and tried again by every later `SetQuality` with an unrelated
change; a resize or a quality change of the resolution tries the requested size again. New private functions:

- `void UpdateShadowMapSize()`: resolves for the current rows; when the result differs from `m_RequestedShadowMapSize`,
  calls `ReleaseShadowMap()` and stores it in both members. Called at the end of `SetViewportSize` (for non-zero sizes)
  and by `SetQuality`.
- `void ReleaseShadowMap()`: deferred release of `m_ShadowMap`, `m_ShadowFramebuffers`, `m_ShadowPyramid`,
  `m_ShadowPyramidSets` (sizes and levels to 0); `InvalidateShadowCache()`; `m_BoundShadowMap = nullptr` and
  `m_CullBoundShadowPyramid = nullptr` (the next frame's comparisons rebuild the sets: `R/SceneRenderer.cpp:2938`,
  `R/SceneRendererGPUDriven.cpp:883`); `m_ShadowOcclusionPays = true`, `m_ShadowOcclusionRender = 0`; `m_ResetVisibility =
  true` (the cascades' visibility history named the old layers).
- `void InvalidateShadowCache()`: `m_ShadowCacheValid = 0`, `m_ShadowCachePending = false`, both key arrays and contents to
  their defaults, `m_ShadowCacheBounds.clear()`, `m_ShadowCacheProxyVersion = 0`.
- `void ReleaseLocalShadowAtlas(uint32_t newSize)`: deferred release of `m_LocalShadowAtlas` and
  `m_LocalShadowFramebuffer`, `m_BoundLocalShadowAtlas = nullptr`, `m_LocalShadowAllocator.Reset(newSize,
  c_MinLocalShadowTile)`, `ResetLocalShadows()` (`R/SceneRendererLocalShadows.cpp:170-180`: lights, views, bounds, content,
  epoch).

`SetQuality(requested)`:

1. `quality = RendererQuality::Clamp(requested)`; if it has the same knobs as `m_Specification.Quality`, store its label and
   return false.
2. Cascades: `size = Resolve(quality)` (resolved for `m_ViewportSize.y` and limited, 7.3); if
   `size != m_RequestedShadowMapSize` or `quality.ShadowCascades != current.ShadowCascades`: `ReleaseShadowMap()`.
3. Local atlas: if its size changed, `ReleaseLocalShadowAtlas(quality.LocalShadowAtlasSize)`; else if the tile cap changed,
   `ResetLocalShadows()` (every view drawn again at its new size, both paths: the views are the CPU's decision on both).
4. `m_Specification.Quality = quality`; when step 2 released, `m_RequestedShadowMapSize = m_ShadowMapSize = size`; if
   anything was released, `ResetHistory()`.

Callers apply the quality after `SetViewportSize` in the same frame (7.7), so `Auto` resolves for the rows the frame
draws at and a resize plus a quality change release the cascades once.

What follows the change, per knob, on both draw paths:

| Knob | Released | Reads the new value | Caches |
| --- | --- | --- | --- |
| `ShadowResolution` (resolved size) | cascades, pyramid, framebuffers | `EnsureShadowMap` (1687-1688), CPU draw (1838), fit and snapping (Extraction 439), `ShadowParams` (Extraction 1174), cull constants (GPUDriven 740), pyramid (1040, 1090), GPU draw (1389) through `m_ShadowMapSize` | cascade cache invalidated (and its key compares the size: 1242, 1289) |
| `ShadowCascades` | as above (layer count) | `FindShadowCasters` (Extraction 425), arrays of `m_ShadowMapLayers` layers in `EnsureShadowMap` and `EnsureShadowPyramid` (`layers` at GPUDriven 1059) | key compares the count (1242) |
| `LocalShadowAtlasSize` | atlas and framebuffer | `m_LocalShadowAllocator.GetAtlasSize()` (LocalShadows 280), `EnsureLocalShadowAtlas` (798-799) | local views' state reset (epoch) |
| `LocalShadowMaxTile` | none | `maxTile` (LocalShadows 281, the constant 37 removed) | views reset |
| `MaxShadowedLocalLights` | none | ranking (LocalShadows 238) | lights beyond the rank give their tiles back (299-303) |
| `LODErrorPixels` | none | `GetLODCriteria` (1725, per frame at 2562), local views (562) | the cascade key compares the LOD criteria (1242-1243); local view keys compare theirs |
| `MinProjectedPixels` | none | CPU cull (Extraction 962), GPU cull (GPUDriven 705) | view only (casters use the sub-texel rule) |

`EnsureShadowMap` (1681-1715) makes `m_Specification.Quality.ShadowCascades` layers of `m_ShadowMapSize` and records
`m_ShadowMapLayers`. When `createTexture` fails it tries half the size, down to 1024, keeps the size it made in
`m_ShadowMapSize` and reports once through `m_Errors` ("the 8192-texel sun shadow cascades could not be allocated; using
4096"); `SceneRendererStats::ShadowMapSize` shows what was made. The limit of 7.3 is the testable form of the same
fallback (7.11). `EnsureLocalShadowAtlas` (793-814) falls back the same way (Ultra's 8192 atlas is 256 MiB): on a failed
`createTexture` it halves down to 2048, and with the smaller atlas made calls `m_LocalShadowAllocator.Reset(half,
c_MinLocalShadowTile)` and `ResetLocalShadows()` and returns false for this frame (it runs at `SceneRenderer.cpp:2888`,
after `FindLocalShadowCasters` placed the frame's tiles for the larger size, LocalShadows 324): the frame draws no local
shadows, as a failure does today, and the next places its tiles in the atlas it has. It reports once ("the 8192-texel
shadow atlas of point and spot lights could not be allocated; using 4096"); below 2048 it fails as today (793-812's
error). The atlas is already
released by every resize (`ReleaseRenderTargets`, 2030-2032, with `ResetLocalShadows`), so its size is tried again there.

**Renderers of single images release the cascades with their targets:** `SetViewportSize(0, 0)`'s "released altogether"
branch (`R/SceneRenderer.cpp:1919-1925`) also calls `ReleaseShadowMap()`. The capture renderer (`ViewportRenderer::
ReleaseTargets`, after every picture), the project's picture and the bake release them; a minimized game (size 0) frees
them until it shows again. Thumbnails keep one 4 MiB layer (from 16 MiB).

The constructor applies `Clamp` to `m_Specification.Quality` (replacing 235-242's clamps), resolves `m_ShadowMapSize` for
a zero viewport, and resets the allocator with `Quality.LocalShadowAtlasSize`.

### 7.6 The `Renderer.Quality` section

Registered in `RegisterRendererModule` (`R/RendererRegistration.cpp:63-`) with
`SettingsRegistry::GetProcessRegistry().Register(RendererQuality::MakeSection())`. Simple section: Id `Renderer.Quality`,
Title "Graphics Quality", Category "Rendering", Scope Project, Version 1. Description: "The rendering quality of the
project's games and of the editor's views and captures: the costly knobs every renderer draws with, as a preset or as
Custom values. Medium keeps Strata 0.6.0's costs; High, the default, draws 4096-texel sun shadow cascades from 1440 output
rows (as Medium below them). Scenes keep their look on lights, Post Process and the sky; no setting renders below the
output's resolution. Set it with project.setSettings; players may change it in games that offer graphics options, and
StrataRuntime --quality for one run."

Fields, in order (each default is High's value, so choosing Custom from High changes nothing):

| Name | Type | Options or range | Default | Description (for agents) |
| --- | --- | --- | --- | --- |
| `Preset` | Enum | Low, Medium, High, Ultra, Custom | High | "The preset: Low, Medium (Strata 0.6.0's costs), High (the default), Ultra, or Custom, which uses the fields below. Under a preset the fields are ignored." |
| `ShadowResolution` | Enum | Auto, AutoFine, 1024, 2048, 4096, 8192 | Auto | "Custom only: texels each way of every sun shadow cascade. Auto: 2048 below 1440 output rows, 4096 from 1440 (about one texel per screen pixel at each cascade's far end); AutoFine: 4096 and 8192. Memory per view: cascades x size x size x 4 bytes (4 x 4096: 256 MiB)." |
| `ShadowCascades` | Int | 1-4 | 4 | "Custom only: sun shadow cascades. Fewer cost less but spread the same texels over the shadow distance." |
| `LocalShadowAtlasSize` | Enum | 1024, 2048, 4096, 8192 | 4096 | "Custom only: texels each way of the shadow atlas point and spot lights share (4096: 64 MiB per view)." |
| `LocalShadowMaxTile` | Enum | 256, 512, 1024, 2048 | 1024 | "Custom only: the largest tile a point or spot light's shadow view gets (at most the atlas); tiles follow how large the light looks on screen." |
| `MaxShadowedLocalLights` | Int | 0-16 | 16 | "Custom only: point and spot lights that cast shadows in a view, the most relevant first." |
| `LODErrorPixels` | Float | 0-8 | 1 | "Custom only: levels of detail are chosen so that their error spans at most this many pixels on screen (0: full detail)." |
| `MinProjectedPixels` | Float | 0-8 | 0.5 | "Custom only: meshes smaller on screen than this many pixels are not drawn (0: all)." |

The fields are the only knobs agents and the generic page reach: wave 6's generic page draws the section until
`graphics-options` gives it its own (`StrataEditor/src/UI/SettingsPages.cpp:146-220`), `project.settings` lists it and
`project.setSettings {section: "Renderer.Quality", value}` sets it (one undo step, written in the background,
exported to the manifest). A simple section upgrades by its fields (new fields take their defaults), so each later
landing bumps `c_SectionVersion` by one and stored version-1 values stay valid.

### 7.7 Every program follows it

- **`GameRenderer`** (`Strata/src/Strata/Runtime/GameRenderer.h/.cpp`):

  ```cpp
		// The quality the game draws with: the project's Renderer.Quality section (IProjectSettings, followed every frame by
		// its version), unless an override is set (StrataRuntime --quality; players' graphics options). nullopt clears it.
		void SetQualityOverride(std::optional<RendererQualitySettings> quality);
		const RendererQualitySettings& GetQuality() const { return m_Renderer->GetQuality(); }
  ```

  `Render` and `RenderEmpty` call `m_Renderer->SetQuality(m_Override ? *m_Override : RendererQuality::GetProjectQuality())`
  right after their `SetViewportSize` (`GameRenderer.cpp:54`; 7.5: `Auto` resolves for the rows this frame draws at;
  equal knobs: one comparison). When the effective preset or its source changes it logs once,
  at info: "Graphics quality: High (the project's)" or "(--quality)"/"(override)".
- **`StrataRuntime --quality <preset>`** (`StrataRuntime/src/RuntimeApplication.cpp`): `c_Arguments` (328-341) gains
  `{ "--quality", "<low|medium|high|ultra>", "Draw with this Graphics Quality preset for this run (default: the project's); nothing is saved" }`;
  `RuntimeOptions` (48-57) gains `std::optional<RendererQualityPreset> Quality`; `CheckOptions` (355-) refuses a missing or
  unknown value ("--quality needs one of low, medium, high or ultra", exit 3; Custom is not a value) and `--quality`
  with `--headless` ("--quality needs a window: a headless run draws nothing", exit 3); after the renderer is made (98):
  `m_Renderer->SetQualityOverride(RendererQuality::GetPreset(*m_Options.Quality))`. Values are matched ignoring case.
- **The editor's views** (`E/ViewportRenderer.h/.cpp`):

  ```cpp
	// What quality an editor view draws with: the project's Renderer.Quality section, or a preset of the person's for the
	// view (ViewportSettings::Quality, viewport.capture's quality).
	enum class ViewportQuality : uint8_t { Project = 0, Low, Medium, High, Ultra };
	const char* ViewportQualityToString(ViewportQuality quality); // "project", "low", "medium", "high", "ultra"
	std::optional<ViewportQuality> ViewportQualityFromString(std::string_view name);
	RendererQualitySettings ResolveViewportQuality(ViewportQuality quality); // Project: RendererQuality::GetProjectQuality()
  ```

  `ViewportRenderer::Render` gains a last parameter `const RendererQualitySettings& quality`, applied with `SetQuality`
  right after its `SetViewportSize` (`E/ViewportRenderer.cpp:119`, before the zero-size return). Its four callers pass: the Scene view (`StrataEditor/src/Panels/ViewportPanel.cpp:241`)
  `ResolveViewportQuality(settings.Quality)`; the Game view (`GameViewPanel.cpp:122`) the project's; captures (7.8) the
  request's; the project's picture (`E/ProjectThumbnail.cpp:139`) the project's (the game's look). The capture renderer's
  2048 atlas (`E/ViewportRenderer.cpp:85-87`) goes: captures draw what the view they capture draws, and release
  everything after each picture.
- **`ViewportSettings::Quality`** (`E/EditorViewport.h:87-139`): `ViewportQuality Quality = ViewportQuality::Project;`, saved
  with the viewport state as `"Quality": "project"` (`ToJson`/`FromJson`, `E/EditorViewport.cpp:33-130`; an unknown value
  fails with "'Quality' must be \"project\", \"low\", \"medium\", \"high\" or \"ultra\""). Only the Scene view follows it:
  the person's machine, separate from the project's choice. (`graphics-options` adds the toolbar chip.)
- **Thumbnails and the bake** construct from `ForThumbnails()` and `ForBake()` and never call `SetQuality`.

### 7.8 Commands, stats and schemas

- **`viewport.setSettings {quality}`** and `viewport.getSettings`: `c_ViewportSettingNames`
  (`E/EditorViewportCommands.cpp:108-127`) gains `{ "quality", "Quality" }`; the schema (887-) gains `"quality": { "type":
  "string", "enum": ["project", "low", "medium", "high", "ultra"], "description": "What the Scene view draws with: the
  project's Graphics Quality (Renderer.Quality, the default) or a preset of this editor's for the view (a view setting,
  saved with the editor's state, no undo step); the Game view and the scene's camera always draw at the project's" }`.
- **`viewport.capture {quality}`**: schema `"quality": { "type": "string", "enum": ["project", "low", "medium", "high",
  "ultra"], "description": "The quality the picture is drawn at (default: the quality of the view it shows: the Scene
  view's for the editor camera and cameras of the capture's own, the project's for the scene's camera)" }`. The handler
  parses it into `CaptureRequest::Quality` (`std::optional<ViewportQuality>`; an unknown value: `InvalidParameters`
  "Parameter 'quality' must be ..."); `quality` with `live: true` is refused like the other render parameters (add it to
  the list at 494). `StartCapture` resolves `request.Quality.value_or(view->FromScene ? ViewportQuality::Project :
  viewport.GetSettings().Quality)` and passes it to `Render` (613). The description adds one sentence: "quality draws the
  picture at a preset (default: the view's); renderStats.quality says what it drew with."
- **`SceneRendererStats`** (`R/SceneRenderer.h:356-`) gains `RendererQualitySettings Quality` (set each frame),
  `uint32_t ShadowMapSize` (`m_ShadowMapSize`) and `std::array<uint16_t, RenderData::c_MaxShadowedLocalLights>
  LocalShadowTileSizes` (the first `LocalShadowLights` entries: each shadowed light's tile, by rank).
- **`DescribeRenderStats`** (`E/EditorStateCommands.cpp:206-250`; used by `editor.perf`'s `renderStats` and
  `gameRenderStats` and by every capture's `renderStats`) adds:

  ```json
  "quality": { "Preset": "High", "ShadowResolution": "Auto", "ShadowCascades": 4, "LocalShadowAtlasSize": "4096",
               "LocalShadowMaxTile": "1024", "MaxShadowedLocalLights": 16, "LODErrorPixels": 1.0, "MinProjectedPixels": 0.5 },
  "shadowMapSize": 4096,
  "localShadowTileSizes": [1024, 512]
  ```

  `quality` is `RendererQuality::ToSectionValue(stats.Quality)` as it is: the section's own field names and stored values
  (data keys, as `project.settings` shows them and `project.setSettings` takes them, so an agent can copy it there), and
  the rows the streams append appear in it without a line here. `shadowMapSize` is what the frame drew (after the limit
  and an allocation fallback). `editor.perf`'s description (871-905) names the three.

### 7.9 Perf tests keep measuring 0.6.0's work

Perf entries measured for 0.6.0 keep their meaning, and the new default gets metrics of its own:

- Projects the editor's perf tests open store `"Renderer.Quality": {"Preset": "Medium"}` (`T/Editor/EditorFramePerfTests.cpp:214,
  379`; `T/Perf/StressProject.cpp:480`: after `Project::Create`, `project->GetSettings().Set(RendererQuality::c_SectionId,
  {{"Preset", "Medium"}}, SettingsRegistry::GetProcessRegistry())` and `project->Save()`, both checked), and the runtimes perf tests start pass `--quality medium`
  (`T/Perf/SamplePerfTests.cpp:69`, `T/Perf/StreamingPerfTests.cpp:219`): those at 1080 rows would not change today, but
  the streams' High rows will.
- `T/Perf/RendererPerfUtils.h`'s specification is `SceneRendererSpecification{}` (Medium): unchanged.
- New provisional metrics (section 11).

DG "Perf tests": "Perf tests measure Medium, 0.6.0's costs, unless their metric names a preset (`...High4K`)."

### 7.10 The test sites

| Site | Before | After |
| --- | --- | --- |
| `T/Renderer/GPUSceneLocalShadowTests.cpp:379, 645` | `fewer.MaxShadowedLocalLights = 3;` | `fewer.Quality.MaxShadowedLocalLights = 3;` |
| `...LocalShadowTests.cpp:386, 631` | `small.LocalShadowAtlasSize = 2048;` | `small.Quality.LocalShadowAtlasSize = 2048;` |
| `T/Renderer/GPUSceneRendererGPUDrivenTests.cpp:89, 1085` | `.LODErrorPixels = ...` | `.Quality.LODErrorPixels = ...` (209's comment names `RendererQualitySettings::LODErrorPixels`) |
| `...GPUDrivenTests.cpp:619` | `everything.MinProjectedPixels = 0.0f;` | `everything.Quality.MinProjectedPixels = 0.0f;` |
| `T/Renderer/GPUSceneShadowTests.cpp:518, 540` | `specification.ShadowCascades` | `specification.Quality.ShadowCascades` |
| `T/Renderer/GPUSceneTAATests.cpp:291` | `GetSpecification().ShadowCascades` | `GetQuality().ShadowCascades` |
| `T/Editor/GPUEditorViewportTests.cpp:1461` | capture atlas `== 2048` | the capture's quality equals the view's (`GetQuality().HasSameKnobs(ResolveViewportQuality(...))`): the behavior changed on purpose |
| `T/Editor/GPUStreamingViewTests.cpp:468` | `GetSpecification().LODErrorPixels` | `GetQuality().LODErrorPixels` |
| `T/Editor/GPUThumbnailTests.cpp:852` | `GetSpecification().LocalShadowAtlasSize == 2048` | `GetQuality().HasSameKnobs(SceneRendererSpecification::ForThumbnails().Quality)` |

Comments naming the old fields: `R/LODSelection.h`, `R/GPUSceneData.h`, `R/RendererComponents.h`, `R/SceneRenderData.h`,
DG 3113, 3369-3371, 3740, Architecture.md 325, 2369 (branch-tip lines; 3109, 3365-3367, 3736 and 2365 at bc6ee123). The
rename is one commit, before anything else of S0.4 changes
behavior, so its diff is mechanical.

### 7.11 Tests

**`Renderer.Quality`** (new, `T/Renderer/RendererQualityTests.cpp`, shard Renderer, no GPU):

1. "Medium and the defaults are 0.6.0's values": `RendererQualitySettings{}`, `GetPreset(Medium)` and
   `SceneRendererSpecification{}.Quality` equal Size2048, 4, 4096, 1024, 16, 1.0, 0.5, knob by knob. And the touch points:
   for every preset p, `GetPreset(p)`'s knobs equal column p of the test's own copy of the table (one line per knob, as
   plan section 5 lays it out; the rows are the .cpp's private constants), so a missing `GetPreset` line, which leaves
   Medium's value, fails in every column that differs from Medium; and the Custom round trip: `s =
   GetPreset(p)` with `s.Preset = Custom`, `v = section.Validate(ToSectionValue(s), c_SectionVersion)` succeeds (a
   member `MakeSection` lacks is refused at the current version) and `FromSectionValue(&*v)` has the same knobs as `s`
   (a missing `ToSectionValue` or `FromSectionValue` line yields the field's default, High's, which Low's column differs
   from). Named presets alone would not catch the last two: under a named preset `FromSectionValue` ignores the fields.
2. "Presets keep their numbers and names": Low 0 ... Custom 4; names round-trip; `PresetFromString("high", true)`;
   unknown names refused.
3. "Auto resolves by output rows": Auto 1439 -> 2048, 1440 -> 4096, 2160 -> 4096, 0 -> 2048; AutoFine 4096/8192; fixed
   sizes ignore rows.
4. "High is Medium below 1440 rows": at 720, 1080 and 1439 rows every resolved knob of High equals Medium's; at 1440 the
   resolution differs.
5. "The section validates its fields": in a local registry, `Register(MakeSection())` succeeds; the default is High with
   High's fields; refused: Preset "Extreme", ShadowCascades 5 and 0, ShadowResolution "3000", LocalShadowMaxTile "4096",
   LODErrorPixels -1 and 9, an unknown member; accepted: a full Custom value.
6. "Presets ignore the fields, Custom uses them": `FromSectionValue({Preset: Low, ShadowCascades: 1})` is Low's;
   `{Preset: Custom, ...}` the fields with Preset Custom; null is High.
7. "A value without a field takes the field's default": `Validate({Preset: "Custom"}, 1)` fills every field with its
   default (`SettingsRegistry.cpp:213-217`, at any version: the mechanism later landings rely on when a value saved at
   version 1 is read by a section with more fields; an actual older version can only be tested once version 2 exists,
   by the first stream that lands a row).
8. "The project's quality follows IProjectSettings by its version": without a provider, Medium; with an empty
   `ProjectSettingsValues` provider (the section not set), High; set to Low, Low; after `Set` (new version), the new
   value; provider gone, Medium.
9. "Clamp keeps every knob in range": out-of-range and non-finite inputs.

**`GPU.SceneRenderer.Quality`** (new, `T/Renderer/GPUSceneQualityTests.cpp`; every case on the CPU path and the GPU-driven
path, ending with `CHECK(gpu.GetNewErrorCount() == 0)`):

1. "SetQuality makes again only what a changed knob sizes": a shadowed sun and a shadowed spot light; equal knobs: false,
   same `GetShadowMap()` and `GetLocalShadowAtlas()`; LOD change: same textures; cascades 2: a new map of 2 layers, the
   atlas kept; Size4096: a new map 4096 wide; atlas 2048: a new atlas, the cascades kept. Validation clean proves the
   deferred releases (a destroyed-in-use texture is a validation error).
2. "Changing the quality and back draws the same image": exact-pixel settings (`TextureStreaming` false,
   `AddNeutralPostProcess`, `SpreadLightingUpdates` false so every local view is drawn in the frame after its reset);
   Medium, then Ultra-like Custom (Size4096, 2 cascades, atlas 2048, tile 512), then Medium
   again: the third image equals the first exactly; the CPU and GPU paths' images within 1/255 (as `GPU.SceneRenderer.
   GPUDriven`).
3. "The cascade cache's exact case holds across a quality change": GPU path, a renderer with `CacheShadowCascades` and one
   without; after `SetQuality` changes the resolution, then a still frame (all cascades cached on the first renderer):
   both images equal exactly (as `T/Renderer/GPUSceneShadowTests.cpp:496-570`'s exact comparison).
4. "Renderers of single images release their cascades with their targets": `SetViewportSize(0, 0)` leaves
   `GetShadowMap()` null; the next frame makes it again; a `ForThumbnails()` renderer's map has one layer.
5. "Auto follows the viewport's rows": Custom {Auto, 1 cascade}: 256 x 1439 -> 2048, 256 x 1440 -> 4096 (a new map),
   back -> 2048.
6. "The cascades' size is limited": `MaxShadowMapSize` 2048 with Size8192: 2048, stats report it.
7. "A light's tile follows LocalShadowMaxTile": a point light filling the view: tile cap 512 -> every tile 512
   (`LocalShadowTileSizes`); cap 2048 with an 8192 atlas -> 2048 (fails today: the constant 1024).
8. "The stats report the effective quality": `Quality` equals what was set, `ShadowMapSize` the resolved size.

**`GPU.Editor.Viewport`** (`T/Editor/GPUEditorViewportTests.cpp`):

1. **"A capture equals the Game view for a scene with a shadowed point light near the camera"** (fails today: the
   capture's 2048 atlas gives 512-texel tiles where the view has 1024): a ground, boxes, a shadowed point light of
   `Range` 10 m, 1.5 m from the camera, filling the view (its screen fraction, `LODSelection.cpp:16`, is about 11.5 at 60
   degrees, so both renderers want 1024-texel faces, `SceneRendererLocalShadows.cpp:281-291`; six of them do not fit a
   2048 atlas, which steps them down to 512); FXAA, manual exposure, bloom off; the Game view at 1280 x 720 rendered until
   `LocalShadowViewsPending` is 0 and the fades are done; `viewport.capture {camera: "scene", width: 1280, height: 720}`:
   equal `renderStats.localShadowTileSizes`, every pixel within 1/255.
2. "viewport.capture draws at the quality asked for, else the view's": `quality: "low"` -> `renderStats.quality.Preset`
   "Low", `renderStats.shadowMapSize` 1024; Scene view set to medium -> an editor-camera capture is "Medium"; the project at Ultra ->
   a scene-camera capture is "Ultra"; "extreme" and `quality` with `live` refused with nothing changed.
3. "The Scene view follows its quality setting and the Game view the project": the renderers' `GetQuality()` after a frame.

**`Editor.Viewport`** (no GPU): "Viewport settings save and load their quality, and refuse an unknown one";
"viewport.setSettings {quality} changes nothing on an invalid value".

**`GPU.Runtime.GameRenderer`**: "A game follows the project's quality by its version, and an override wins": without a
provider, Medium; a provider with Custom {Size1024} -> 1024; `Set` to Low -> the next frame follows;
`SetQualityOverride(GetPreset(Ultra))` wins; `nullopt` returns to the project's.

**`GPU.Editor.Thumbnails`**, **`GPU.Editor.Lighting`**: the thumbnail and bake renderers' quality equals their profile's
with the project at Ultra (they ignore it).

**CTests** (`StrataTests/CMakeLists.txt`, beside `StrataRuntime.InvalidFrames`): `StrataRuntime.InvalidQuality.<value>` for
`extreme` and `custom` (exit 3, "--quality needs one of low, medium, high or ultra"); `StrataRuntime.QualityHeadless`
(exit 3, "--quality needs a window"); `StrataRuntime.Quality` (label `gpu`: `--windowed --resolution 640x360 --frames 30
--quality ultra` through `Editor/RunAndExpect.cmake`, expecting "Graphics quality: Ultra (--quality)"). All four with
`FIXTURES_REQUIRED StrataExportedGame` and `TIMEOUT 60`, as `StrataRuntime.InvalidFrames` has
(`StrataTests/CMakeLists.txt:555-560` at the branch tip).

**Feature test:** `FeatureTest.stproj` gets `"Renderer.Quality": {"Version": 1, "Value": {"Preset": "Custom",
"ShadowResolution": "1024", "ShadowCascades": 3, "LocalShadowAtlasSize": "2048", "LocalShadowMaxTile": "512",
"MaxShadowedLocalLights": 8, "LODErrorPixels": 1.5, "MinProjectedPixels": 0.75}}` (set in a copy opened in the editor,
copied back; DG "Feature test"); `GPU.FeatureTest` checks that `RendererQuality::GetProjectQuality()` under
`FeatureProject`'s provider is that Custom value.

### 7.12 Acceptance

- Every GPU suite passes with only the edits of 7.10; `GPU.SceneRenderer.GPUDriven`, `.Feedback` and the shadow cache's
  exact case unchanged.
- Harness: every still shot at quality `medium` identical to the 0.6.0 baseline, except shots whose 0.6.0 frame drew a
  shadowed local light (`renderStats.localShadowLights > 0`: the capture's atlas is now the views'); those are listed in
  the commit with their tile sizes before and after.
- High at 3840x2160: `renderStats.shadowMapSize` 4096 in every 4K shot; GPU busy at most Medium's + 0.6 ms in each
  3D sample's scene-camera shot with the cascades redrawn (the shadows audit's estimate for GPU.Shadows at 4096 is
  0.3-0.6 ms from 0.13-0.18), reported per shot in the commit.
- The A/B of S0.8b.

### 7.13 Docs

- DG **"Quality settings"** (new subsection of Rendering, after "Scene renderer and lighting"): the knobs and the preset
  table with costs (7.2), Auto by rows, `SetQuality` and what it rebuilds on both paths and in the caches, single images
  releasing their cascades, who follows what (programs, views, captures, thumbnails, bake, `--quality`), the stats, and
  **adding a row**: the touch points of 7.2, Medium = 0.6.0's cost, the four presets on one line, the section's version +1
  per landing (the integrator renumbers when two streams chose the same), a field described for agents, a non-default
  Custom value in the feature project, the cost in the table.
- DG "Lights, shadows and HDR" and "Local light shadows" (3324-3409): layers = `ShadowCascades`, resolution and tile cap
  from the quality. "View commands": `quality` of `viewport.setSettings` and `viewport.capture`, `renderStats.quality`.
  "Readback, the game renderer and tests": `GameRenderer` follows the project. Repository layout's `StrataRuntime` row:
  `--quality`. "Perf tests": 7.9's rule. "Feature test": the section's value.
- `Docs/Manual/Settings.md` ("Project Settings"): a **Graphics Quality** page: the presets, what each costs, Custom, the
  Scene view's own setting, `--quality`, and that nothing renders below native resolution.
- `Docs/Architecture.md`: the specification's `Quality`, `SetQuality` in the renderer's lifecycle (325, 2365 renamed).
- `Docs/Releases/PackageReadme.md` (78), `README.md`: `--quality` beside `--seconds`.
- Skills: `strata-editor-automation` (`viewport.setSettings {quality}`, `viewport.capture {quality}`,
  `renderStats.quality`, `project.setSettings` for `Renderer.Quality`), `strata-make-a-game` (one line: games default to
  High; Medium for 0.6.0's costs).
- **AGENTS.md**, Rendering digest (232 code points): "- Costly knobs are rows of `RendererQualitySettings` and fields of the
  `Renderer.Quality` section (a row per line, its four presets on it); Medium keeps 0.6.0's costs ([Quality
  settings](Docs/DeveloperGuide.md#quality-settings))."

### 7.14 Commits (in order)

1. `RendererQuality.h/.cpp`, the struct moved into the specification, the 13 test lines and the comments (no behavior).
2. The named profiles (thumbnails, bake).
3. Cascade layers = `ShadowCascades`; release with a zero viewport; `GetShadowMap`.
4. `SetQuality`, Auto resolution, the limit and the fallback; `GPU.SceneRenderer.Quality`.
5. `LocalShadowMaxTile`; stats.
6. The section, `GetProjectQuality`, the feature project; `Renderer.Quality` tests.
7. `GameRenderer`, `--quality` and its CTests; perf tests pinned to Medium.
8. The editor: views, `ViewportSettings::Quality`, commands, captures matching views; editor tests.
9. Docs, skills, AGENTS.md bullet.

Each commit message states the A/B ratio of `PerfGPU.Renderer` it measured where it touches frame code (4, 5, 7, 8).

### 7.15 Release-note lines (0.7.0)

- **What's new:** "Graphics quality presets. Project Settings has a Graphics Quality page (Renderer.Quality) with Low,
  Medium (0.6.0's costs), High and Ultra, and Custom values for shadow resolution and cascades, point and spot light
  shadows and levels of detail. Games, the editor's Scene and Game views and captures follow it and change it while they
  run; the Scene view can use a preset of its own (`viewport.setSettings {quality}`), and `StrataRuntime --quality`
  plays a game at a preset for one run without saving anything."
- **What's new:** "Captures match the views: `viewport.capture` draws point and spot lights' shadows as the view does
  (0.6.0 used half the atlas), and `quality` draws a picture at a preset."
- **Upgrading:** "Projects use High unless they choose: at 1440 rows and above, sun shadows are drawn into 4096-texel
  cascades, 256 MiB per view with a shadowed sun instead of 64 MiB (below 1440 rows High is Medium). Choose Medium to keep
  0.6.0's costs."
- **Upgrading (engine code):** "`SceneRendererSpecification`'s `ShadowMapSize`, `ShadowCascades`, `LocalShadowAtlasSize`,
  `MaxShadowedLocalLights`, `LODErrorPixels` and `MinProjectedPixels` moved into its `Quality`
  (`RendererQualitySettings`); `ShadowMapSize` became `ShadowResolution`."

## 8. S0.5 Scene binding slots and stage names

**Slots.** A comment after `SH/Include/Scene.glsl:5`:

```glsl
// The scene layout's slots (SceneRenderer::CreateResources): SRV 0-21 and 27, samplers 0-5 (materials), 8, 9 and 10, CBV 0.
// SRV 22-26 belong to the compute layouts of temporal anti-aliasing and screen-space reflections, never to this one.
// Assigned for wave 7: SRV 28 the sun's screen-space shadow mask, 32 the cascades' depth ranges and 33 the shadows'
// blue-noise tile (sun-shadows); 29 the aerial perspective volume, 30 and 31 the irradiance volume, 34 volumetric fog
// (light-and-materials). A new scene resource takes the next free slot (35), and a pass that does not run binds a dummy.
```

Stream assignments (plan 4, and what the streams' specs ask for): `sun-shadows` SRV 28 (the mask), 32 (the cascades'
depth ranges) and 33 (the blue-noise tile), `specs/sun-shadows.md` D1; `light-and-materials` SRV 29 (froxel volume),
30-31 (irradiance volume, below its cut) and 34 (volumetric fog, below its cut: its spec first named 32 by the "next free
slot" rule, which `sun-shadows` takes first, and now names 34); `camera-and-tone` none (post passes have their own layouts and push constants). NVRHI's Vulkan
mapping gives SRVs bindings 0-127 (`SH/Include/Common.glsl:6-9`), so 32-35 are valid. MoltenVK's 16 samplers per stage
leave 7 free: no stream adds a sampler to the scene layout (they reuse 8, 9, 10).

**GPU stages** (`RendererGPUStages::Get`, `R/GPUProfiler.cpp:44-61`): each stream registers its stage at its own
position, near the stage its pass follows in the frame, so the lines never meet. The
plan's "stages appended" is read as "added", not "at the end": the three stream specs follow this table
(`sun-shadows.md`, `camera-and-tone.md` and `light-and-materials.md` name these positions). Registration order fixes
the ids and perf-log columns (46); inserting moves the later stages' ids and columns, which nothing depends on: ids are
only compared within one build, and perf-log readers find columns by name (`T/Perf/SamplePerfTests.cpp:45, 85`). The
five hard-coded lists take the same positions: `E/EditorStateCommands.cpp:880-884` (`editor.perf`'s description),
`T/Editor/EditorProfilerTests.cpp:91-92`, `T/Runtime/FramePerfLogTests.cpp:284` (branch tip), `Docs/Architecture.md`'s
stage table (887-895, branch tip) and `.claude/skills/strata-editor-automation/SKILL.md:486-488`.

| Stage | Stream | Registered after | Covers |
| --- | --- | --- | --- |
| `GPU.Aerial` | light-and-materials | `GPU.Setup` | aerial perspective's froxel volume |
| `GPU.Volumetric` | light-and-materials (below cut) | `GPU.Shadows` | volumetric fog |
| `GPU.ScreenShadows` | sun-shadows | `GPU.Prepass` | the shadow mask, contact shadows, its blur, the depth-range reduction |
| `GPU.MotionBlur` | camera-and-tone | `GPU.TAA` | tile max, neighbor max, gather |
| `GPU.DepthOfField` | camera-and-tone (below cut) | `GPU.MotionBlur` | depth of field (timed before motion blur; registered after it, as its spec says) |

The debug views streams add go after `Motion` in `ViewportDebugView` (`StrataEditor/src/Editor/StreamingView.h:26-33`)
in landing order (`shadows`, then `lighting`, `ao`, `indirect`, `luminance`, `albedo`): one enum line each, resolved at
merge (the names, not the numbers, are stored: `StreamingView.h:35`).

Docs: DG "GPU timing" (3795-) states the rule (a new stage's position, the five lists); this table lives in the plan and
`Docs/Plans/Wave7/` (S0.7).

## 9. S0.6 The look-dev harness

### 9.1 Files

- `Tools/LookDev.py` (standard library only; run as `python -I Tools/LookDev.py ...`).
- `Tools/LookDev/Shots.json` (the projects and shots), `Tools/LookDev/LookDevScene.json` (the look-dev project's command
  script, with `@LOOKDEV_DIRECTORY@` and `@LEAF_TEXTURE@` the tool replaces, JSON-escaped).
- `StrataTests/Tools/LookDevTest.cmake` only if the CTest needs a wrapper (9.7).

Every command it uses exists in 0.6.0 (bc6ee123), so the same tool takes the 0.6.0 baseline with `build/windows`; it
sends `quality` only when `--quality` is not `project`, and refuses `--quality` with a build whose `viewport.capture`
schema lacks it (read once through `editor.commands`).

### 9.2 Command line

```
python -I Tools/LookDev.py capture --build <bin dir> [--label <text>] [--output <dir>] [--projects a,b] [--shots <glob>]
       [--sizes 3840x2160,1920x1080] [--aa taa,fxaa,off] [--quality project|low|medium|high|ultra] [--repeat 5] [--live]
       [--keep-work] [--work <dir> [--fresh-work]]
python -I Tools/LookDev.py compare <before dir> <after dir> --output <dir>
python -I Tools/LookDev.py selftest --build <bin dir> --output <dir>
```

- `--build`: the folder with `StrataEditor` and `StrataCLI` (e.g. `G:/GameEngineAI/build/windows/bin/Release`).
- `--output`: default `<temp>/StrataLookDev/<YYYYMMDD-HHMM>-<label>`; refused when inside the repository's tracked tree:
  inside the tool's root (known from its own path) unless the first directory below the root starts with `build` or `.`
  (CMake's binary trees, `.worktrees/`), so the CTest of 9.7 can write into its binary directory and nothing lands among
  sources. Exit codes: 0 every shot captured, 1 some failed (listed), 2 usage.
- Work: `--work <dir>`, default `<output>/work/` (deleted at the end unless `--keep-work` or `--work` is given): copies of
  `Samples/ThirdPerson`, `PhysicsPlayground` and `OpenWorld` (never opened in place), the generated projects, a session
  directory passed to every child as `STRATA_SESSION_DIR` (a path the tool does not create: the editor and the CLI make it
  private, `Strata/src/Strata/Network/EditorSession.cpp:166`), and `STRATA_EDITOR_PATH` for `StrataCLI launch`. A given
  `--work` that already holds the copies is reused as it is (their imported assets with them), so repeated runs and A/Bs
  skip the samples' first import; the copies are made again with `--fresh-work`. The tool changes its children's
  environment only; tests never change their own. `run.json` reports the time spent opening and importing each project
  apart from the captures' time.

### 9.3 Projects

| Key | Made by | Scene shots open |
| --- | --- | --- |
| `ThirdPerson` | copy of the sample | `Scenes/Course.stscene` |
| `PhysicsPlayground` | copy | `Scenes/Main.stscene` |
| `OpenWorld` | copy | `Worlds/Island.stscene` |
| `Basic3D` | a one-step command script the tool writes, `project.create {directory, name: "Basic3D", template: "basic3d"}`, run as the look-dev script is | `Scenes/Main.stscene` |
| `LookDev` | `StrataEditor --headless --no-automation --commands <work>/LookDevScene.json --quit-after-commands --frames 36000` | `Scenes/LookDev.stscene`, `Scenes/Furnace.stscene` |

Each project gets one editor: `StrataCLI launch --headless --project <dir> --idle-timeout 900` (windowed, without
`--headless`, with `--live`), then `StrataCLI --project <dir> --timeout 300000 call <command> @<params file>` per step
(parameters written to files: no shell quoting), and `editor.quit {force: true}` at the end, also when a shot failed.

### 9.4 The look-dev project (`LookDevScene.json`)

Built from the `empty` template so that template changes never change it; it sets geometry, materials, the lights'
direction and intensity, the sky's source and the cameras, and leaves every look property at its component default, so
each build shows what new content gets. Before running it the tool writes `@LEAF_TEXTURE@`: a 256 x 256 RGBA PNG of
seven leaf shapes (ellipses at fixed positions, alpha 255 inside, 0 outside, color (60, 110, 40)).

Steps (`project.create {directory: "@LOOKDEV_DIRECTORY@", name: "LookDev", template: "empty"}`, `scene.new`, then
`entity.create {name, components}` per entity, `material.create {path, properties}` per material, `asset.import {file:
"@LEAF_TEXTURE@", directory: "Textures"}` bound for the leaves' `BaseColorMap`, `scene.saveAs`, `project.setStartScene`,
`project.save`). Meshes are the built-in ones by handle (`Strata/src/Strata/Asset/BuiltinAssets.h:29-37`: cube 01, sphere
02, plane 03, quad 04, cylinder 05). Coordinates in meters; rotations as Euler degrees [pitch, yaw, roll].

`Scenes/LookDev.stscene`:

| Entity | Components |
| --- | --- |
| Main Camera | Camera (Primary, 60 degrees), Transform (0, 1.6, 7), rotation [-6.5, 0, 0] |
| Sun | DirectionalLight (Intensity 100000, CastShadows), rotation [-40, 30, 0] (40 degrees high, from +X+Z) |
| Sky | SkyLight (Source Atmosphere, Intensity 1) |
| Post Process | PostProcess (defaults; the tool sets anti-aliasing per shot) |
| Ground | plane, scale (60, 1, 60), `Materials/Ground.stmat` (BaseColor 0.35, Roughness 0.85) |
| Room Floor, Room Ceiling | cubes (6.4, 0.1, 6.4) at (-10, 0.05, -2) and (6.4, 0.2, 6.4) at (-10, 3.1, -2), `Materials/Plaster.stmat` (BaseColor 0.58, Roughness 0.9) |
| Room Wall West, East, North | cubes (0.2, 3, 6.4) at (-13.1, 1.5, -2) and (-6.9, 1.5, -2); (6.4, 3, 0.2) at (-10, 1.5, -5.1) |
| Room Wall South (four pieces around a 2 x 1.3 m window, sill 0.9 m) | (2.2, 3, 0.2) at (-12.1, 1.5, 1.1) and (-7.9, 1.5, 1.1); (2, 0.9, 0.2) at (-10, 0.45, 1.1); (2, 0.8, 0.2) at (-10, 2.6, 1.1) |
| Room Box, Room Ball | cube 0.5 at (-10.5, 0.35, -3); sphere scale 0.6 at (-8.5, 0.4, -2.5) |
| Room Lamp | PointLight (Intensity 150, Range 6, CastShadows) at (-11.5, 2.2, -4) |
| Plate | cube (0.3, 0.05, 0.3) at (2, 0.025, 0), Plaster: the 5 cm plate of the acceptance table |
| Pole 3cm, 5cm, 10cm | cylinders (0.03, 2.5, 0.03), (0.05, 2.5, 0.05), (0.1, 2.5, 0.1) at (4, 1.25, 0), (5, ...), (6, ...) |
| Fence | parent at (7.5, 0, 0): ten slats, cubes (0.02, 1, 0.08) at x 0, 0.1 ... 0.9, y 0.5; a rail (1, 0.04, 0.04) at (0.45, 0.9, 0) |
| Leaves | quad scale (1, 1.5, 1) at (-3, 0.75, 0) facing +Z, `Materials/Leaves.stmat` (AlphaMode Mask, AlphaCutoff 0.5, DoubleSided, Roughness 0.8, BaseColorMap the leaf texture) |

`Scenes/Furnace.stscene`: Camera (Primary, 45 degrees) at (0, 0.8, 7) rotation [-4, 0, 0]; SkyLight (Procedural, zenith,
horizon and ground colors 0.5, SunIntensity 0, Intensity 1); no sun; PostProcess (AutoExposure off, Exposure 0, Tonemapper
None, Bloom off, AmbientOcclusion off, AntiAliasing off, Vignette 0); ten spheres (scale 1, y 0.5): metals (BaseColor 0.95,
Metallic 1) at z 0 and dielectrics (BaseColor 0.5) at z -1.5, roughness 0, 0.25, 0.5, 0.75, 1 at x -3, -1.5, 0, 1.5, 3, one
material each (`Materials/Furnace<Metal|Dielectric><roughness x 100>.stmat`). With Tonemapper None the display value is
the linear value sRGB-encoded, so the furnace reads straight off the PNG.

### 9.5 Shots (`Shots.json`)

```json
{ "Format": "StrataLookDevShots", "Version": 1,
  "Projects": { "ThirdPerson": { "Sample": "ThirdPerson", "Scene": "Scenes/Course.stscene" }, "...": {} },
  "Shots": [ { "Name": "tp.feet", "Project": "ThirdPerson", "Position": [1.2, 1.2, 4.6], "Target": [-0.196, -0.08, 2.273],
               "Fov": 60, "Crops": [ { "Name": "center", "Rect": [0.35, 0.35, 0.65, 0.65] } ] } ] }
```

A shot has `Name`, `Project`, optional `Scene` (else the project's), `Camera` ("scene" or "editor": the editor's opening
view, captured first after `scene.open {force: true}`, overlays off) or `Position`/`Target`/`Fov` (a camera of the
capture's own), optional `Setup` (commands run before it, with `"entityName"` replaced by `entity.find {name}`'s first id;
the scene is opened again after a shot with setup), optional `AntiAliasing` (the modes it runs in; default all three),
`Crops` (normalized `[x0, y0, x1, y1]`, the same region at every size), and for live shots `Live: {AtFixedStep, Frames,
Input}`. Anti-aliasing per mode: `component.set` on every entity `entity.find {component: "PostProcess"}` returns:
`taa` {AntiAliasing true, AntiAliasingMethod "TAA"}, `fxaa` {true, "FXAA"}, `off` {AntiAliasing false}. Nothing is saved.

| Shot | Camera | Crops (normalized) | Why |
| --- | --- | --- | --- |
| `tp.scene` | scene | robot-shadow [0.28, 0.60, 0.40, 0.74] (holds the audit's noise row 1450, x 1170-1290 at 4K), feet [0.44, 0.64, 0.555, 0.755], far-coins [0.43, 0.12, 0.57, 0.27], sign-shadow [0.62, 0.52, 0.86, 0.70] | the player's view; noise, far blobs |
| `tp.editor` | editor | island [0.35, 0.35, 0.65, 0.65] | the editor's opening view, all in the last cascade today |
| `tp.feet` | (1.2, 1.2, 4.6) -> (-0.196, -0.08, 2.273), 60 | center [0.35, 0.35, 0.65, 0.65] | foot contact |
| `tp.sign` | (1.0, 1.6, 3.0) -> (1.208, -0.06, 0.51), 60 | center | thin caster |
| `tp.lantern` | (4.8, 2.2, -18.5) -> (3.191, 1.042, -20.752), 60 | center | shadowed local light |
| `pp.scene` | scene | dominoes [0.195, 0.435, 0.415, 0.665], pins [0.56, 0.74, 0.64, 0.85], sky [0.74, 0.0, 0.82, 0.24] (the audit's column x = 3000), pillar [0.29, 0.12, 0.37, 0.40] | small casters, banding |
| `pp.editor` | editor | center | |
| `ow.scene` | scene | near-forest [0.25, 0.70, 0.55, 0.95], mid-forest [0.45, 0.52, 0.70, 0.72], mountain [0.10, 0.20, 0.45, 0.55], sky [0.70, 0.0, 0.80, 0.30] | forest, haze, sky |
| `ow.editor` | editor | center | |
| `ow.lowsun` | scene; Setup: Sun's Transform rotation [-10, 70, 0] (the sample's sun is [-55, 70, 0]) | near-forest, mid-forest | long shadows ending at the shadow distance |
| `ow.water` | (120, 36.6, 256) -> (120, 30, 1256), 55 | horizon [0.0, 0.40, 1.0, 0.60] | water and horizon |
| `basic3d.scene`, `basic3d.editor` | scene, editor | center | the first image of a new project |
| `lookdev.overview` | scene | plate [0.55, 0.55, 0.70, 0.70] | |
| `lookdev.room` | (-9, 1.5, -4.5) -> (-10, 1.2, 1.1), 70 | window [0.3, 0.2, 0.7, 0.6], floor [0.2, 0.6, 0.8, 1.0] | sky occlusion, window light |
| `lookdev.contact` | (2, 1.0, 2.4) -> (2, 0, 0), 40 | plate [0.35, 0.40, 0.65, 0.70] | the 5 cm plate at 2.6 m |
| `lookdev.thin` | (6, 1.5, 8) -> (6, 0.8, 0), 40 | poles [0.2, 0.3, 0.6, 0.9], fence [0.6, 0.3, 0.95, 0.9] | thin casters |
| `lookdev.foliage.near`, `.far` | (-3, 1, 3) and (-3, 1.5, 30) -> (-3, 0.75, 0) | center | alpha cutouts near and far |
| `lookdev.sky` | (0, 1.6, 6) -> (0, 3, -20), 60 | column [0.48, 0.0, 0.52, 0.5] | banding |
| `lookdev.furnace` | scene of `Scenes/Furnace.stscene`; AntiAliasing `off` only | metals (the nearer row) [0.05, 0.45, 0.95, 0.75], dielectrics [0.05, 0.30, 0.95, 0.55] | energy |
| `tp.live.pan` (live) | scene, `AtFixedStep` 120, `Frames` 4, Input: `Gameplay/Look` pressed with value [0.5, 0] from step 1 (`input.sequence`) | center | a turning camera at 4K |
| `ow.live.tour` (live) | scene, `AtFixedStep` 900, `Frames` 4 | near-forest | the tour in motion |

Before the 0.6.0 baseline is taken, the implementer looks at every shot once (`capture --repeat 1`) and corrects in
`Shots.json` any camera that does not show what its row names (the `ow.water` and `lookdev.*` cameras are computed, not
seen); the commit states it.

Live shots (with `--live`, windowed editors): `editor.showPanel {panel: "Game"}`, `viewport.setSettings {gameView: {mode:
"custom", width, height}}`, `play.start {paused: true}`, the live capture armed in the background (`viewport.capture {live:
true, camera: "scene", atFixedStep, frames, path}`), then `input.sequence` or `play.step` to run past it, then `play.stop`.

### 9.6 Outputs

`<output>/`:
- `run.json`: date, label, `--build`, `StrataEditor --version`'s line, options, GPU name (`editor.status {sections:
  ["editor"]}`), per shot the outcome.
- `shots/<shot>-<W>x<H>-<aa>.png` (the first of the repeats) and `shots/<shot>-<W>x<H>-live-<n>.png`.
- `crops/<shot>-<W>x<H>-<aa>-<crop>.png`: the region, enlarged 4 times by nearest neighbor.
- `stats/<shot>-<W>x<H>-<aa>.json`: each repeat's `renderStats` (with `gpuMs`, `quality`), `temporalFrames`, `pendingAssets`,
  `texturesBelowWanted`, and the repeats' pixel differences with the first (the self-difference: 0 expected).
- `stages.csv`: per shot, size and mode: GPU busy median of the repeats (sum of `gpuMs`), each stage's median, triangles,
  draws, shadow draws, cached cascades, `quality.Preset`, `shadowMapSize` (both of `renderStats`, 7.8).
- `index.html`: a static page: per shot its images and crops, and the table of `stages.csv`.

`compare <before> <after> --output <dir>`: per matching file, differing pixels, the largest channel difference and the mean,
a difference image (|after - before| x 8) and the crops side by side; per shot the ratio after / before of GPU busy and of
each stage; `compare.json` and `index.html`. It never fails on differences (it reports them); exit 1 only when a side
lacks a shot the other has.

The PNG reader and writer are written in the tool (zlib and struct: 8-bit RGB and RGBA, the five filter types), with no
code from elsewhere.

### 9.7 The CTest

`StrataTests/CMakeLists.txt`, when `TARGET StrataEditor AND TARGET StrataCLI`: `find_package(Python3 3.10 COMPONENTS
Interpreter)`; found: `add_test(NAME StrataTools.LookDev COMMAND "${Python3_EXECUTABLE}" -I
"${PROJECT_SOURCE_DIR}/Tools/LookDev.py" selftest --build "$<TARGET_FILE_DIR:StrataEditor>" --output
"${CMAKE_CURRENT_BINARY_DIR}/LookDevSelfTest")`, labels `gpu;tools`, timeout 900; not found: the same name runs a
`cmake -P` script that fails with "StrataTools.LookDev needs Python 3.10 or later on PATH when CMake configures" (so a
machine without Python sees why, rather than a silently missing test).

`selftest`: builds the look-dev project from `LookDevScene.json` (the whole script, so it keeps working), captures
`lookdev.contact` at 320 x 180 with FXAA twice, writes every output, runs `compare` of the output with itself, and checks:
both PNGs decode to 320 x 180; the self-difference is 0 pixels; the crop is the rect's size times 4; the stats hold
`renderStats` with `pendingAssets` 0; `index.html` names the shot; the editor quit. It also round-trips a generated image
through its PNG writer and reader. Under 3 minutes.

AGENTS.md's package line names the exception (32 code points, counted in section 4: "Package tests (label `package`) need CMake and a
compiler, `StrataTools.*` tests Python 3; a change to a sample must keep its `Editor/<Sample>Commands.json.in` passing.");
it names the label's tests, not one of them, because `StrataTools.PerfStability` (11.2, below the cut) needs Python too.
DG "Test rules and helper processes" states it in full, and the label `tools` (`StrataTools.*`) in "Testing". CI runs
`tools` tests without `gpu` (`.github/workflows/ci.yml:191-193` excludes only `gpu|perf`), so `StrataTools.PerfStability`
runs there: the hosted runners have Python (the workflow's annotate step already runs it, `ci.yml:201-202`).

### 9.8 Baselines and how streams use it

- Every capture run whose GPU times are kept runs under `python Tools/PerfLock.py --label lookdev-<label> --limit 90 --
  python -I Tools/LookDev.py capture ...` (PerfLock's default limit, 25 minutes, `Tools/PerfLock.py:115`, would end a
  full run), alone on the machine, with `--work` reused between the runs of one build.
- **0.6.0:** after the campaign ends and before `build/windows` is rebuilt: `capture --build G:/GameEngineAI/build/windows/bin/Release
  --label 0.6.0` with every shot, both sizes, all three modes, 5 repeats (and `--live`). Kept outside the repository at
  `G:/GameEngineAI/.worktrees/wave7-plan/lookdev/0.6.0/` (the plan's folder, untracked) and named in the roadmap's wave
  entry.
- **Right before S0.3** and **after each of its commits** at the branch: the identity check of 6.3.
- **Step 0's tip:** twice, `--quality medium` (compared with 0.6.0: 7.12) and `--quality project` (High: the streams'
  baseline), saved beside the first.
- **Streams:** before and after a change, `capture --shots <theirs>` under `Tools/PerfLock.py --label <stream>`, `compare`
  as information (plan 4.1: no campaigns); they add shots to `Shots.json` and content to `LookDevScene.json` (new entities
  appended, existing ones unchanged so earlier shots keep their meaning).
- **The owner's sheet** at the wave's end: `compare` of 0.6.0, step 0 and wave 7.

Run time on the reference machine: about 21 still shots x 2 sizes x 3 modes x 5 repeats = 630 captures (the furnace only
without anti-aliasing: about 610); at 2-4 s each, 25 to 45 minutes of captures (acceptance: under 45, with `--work`
reused; the first import of the three samples' copies is extra and reported in `run.json`; its length is unverified).

Docs: DG "Testing" gains **"Look-dev harness"** (after "Perf tests"): what it captures, the projects and shots, outputs,
`compare`, the CTest, the baselines and the rule that image changes are shown with it; Repository layout's `Tools/` row
names `LookDev.py`.

## 10. S0.7 The roadmap and the wave's documents

- Apply `.worktrees/wave7-plan/roadmap_wave7.md`'s three edits to `Docs/Roadmap.md`: the "### Wave 7" entry after
  "### Wave 6" (1125-1153), "### Later (wave 7 and beyond)" (1154-) replaced by "### Later (wave 8 and beyond)" with the
  owner's ruling against rendering below native resolution, and the open items' sentence at 797. Where this spec changed a
  detail (the order of section 3, the 13 test sites), the entry says what was done.
- Status: "Wave 7 in progress (step 0 landed)", with the 0.6.0 look-dev baseline's location, the budgets re-measured
  (S0.8a), and in the open items any metric re-baselined up and any regression left as a release blocker (11.3).
- Commit the plan and the reviewed specs as `Docs/Plans/Wave7/plan.md` and `Docs/Plans/Wave7/specs/<key>.md`, as wave 6's
  were (removed when the wave lands, `1bdcf0e0`): the streams' worktrees then have them.

## 11. S0.8 The campaign's results and the A/B

### 11.1 Input

`.worktrees/wave7-plan/perf-campaign-bc6ee123/`: `run1.log` ... `run5.log` and `run1.json` ... `run5.json` (each run's
`PerfResults/Release.json`: per metric `Value`, `Unit`, `Budget` or `Provisional`, `Target`, `Attempts`), and the report
in `.worktrees/wave7-plan/perf-campaign-bc6ee123.log` (`Tools/PerfStability.py --runs 5 --wait-quiet 20 --wait-timeout
30`, under `PerfLock.py --label step0-campaign`, on bc6ee123's Release build).

Validity first: the canary `Perf.Harness.Sort200k` within 1.10 times its reference (10 ms) in every run, each run's machine
load at most 30% (the perf tests alone keep it about 20%). A run that fails either is left out; fewer than three valid runs
means no budget changes (measure again alone). Tests that failed for anything but a budget are bugs fixed in step 0 (their
suite measured again alone under `PerfLock.py`).

What the first three runs show (read at 00:00 on 2026-10-27 while the campaign ran; the implementer reads all five):
the canary at 9.97, 9.99 and 10.01 ms, so the replaced CPU runs single-threaded work at its reference speed; over budget in
all three attempts: `Perf.Physics.Create10kBodies` (about 16.0 ms against 11.99, runs 1-3),
`Perf.EditorPanels.Performance600FramesP95` (0.099-0.102 ms against 0.0924, runs 1-3), `PerfGPU.Editor.ThumbnailsMaxFrame`
(2.29-2.77 ms against 1.149, runs 1-2), `PerfGPU.Editor.SceneOpenOver1MMaxFrame` (run 1), `PerfGPU.Editor.
EditFrameNested100k` (0.285 ms against 0.2808, run 2), `Perf.EditorPanels.Streaming10kAssetsRefreshFrame` (0.22-0.23 ms
against 0.2205, run 3); failures that are not budgets: `SamplePerfTests.cpp:101` (`elapsedMs >= c_TourWindowMs`, runs 1
and 2: the Open World tour ended before its window; possibly the unpaced exported-game window the audits saw, unverified)
and `EditorWorldPerfTests.cpp:65` (`measurement.has_value()`, run 2). The machine was 21-24% busy (other agents).

### 11.2 `PerfStability.py` gains three modes

- `--propose-budgets <out.json> [--rebaseline <metric>=<reason> ...]` (with `--results <files>`): applies 11.3 to the
  current `Budgets.json`, writes the proposed file (same layout, `_comment` kept, entries in the same order) and prints
  each changed entry and its class. An entry over budget goes up only when named by a `--rebaseline` (one per metric,
  its reason the evidence of 11.3's "Over budget" procedure); every other one is reported and left unchanged.
- `--baseline <files>` (with `--results <files>`): per metric present in both, the median of each side and the ratio
  results / baseline; `--max-ratio R` flags ratios above R (exit 1); counts compared for equality.
- `--self-test`: the rules of 11.3 on synthetic entries (each class below, rounding, targets, counts); CTest
  `StrataTools.PerfStability` (label `tools`, no GPU; Python found as in 9.7). Below the cut (section 3).

### 11.3 The rules (AGENTS.md "Perf tests"; Budgets.json's `_comment`)

Per metric with values v1..vn of the valid runs (n >= 3): M = the median, spread = (max - min) / M. Numbers keep four
significant digits: `Measured` rounded to nearest, `Budget` rounded down.

| Class | When | Entry becomes |
| --- | --- | --- |
| Unstable | spread above 0.25 (not a count) | unchanged; listed to measure alone |
| Provisional measured | `Provisional` | `Measured` M, `Budget` floor(1.5 M); with a `Target`: if every vi <= Target, `Budget` = min(floor(1.5 M), Target) and the target dropped (a budget lowered to its target drops it); else the target kept while below the budget (an acceptance not met: listed in the roadmap's open items) |
| Cheaper | M below 0.9 x `Measured` | `Measured` M, `Budget` min(old, floor(1.5 M)) (goes down) |
| Same | M within 0.9-1.1 x `Measured` | unchanged |
| Dearer, within | M above 1.1 x `Measured`, every vi within `Budget` | `Measured` M, `Budget` unchanged (still at most 1.5 M) |
| **Over budget** | some vi over `Budget` (a failure confirmed in three attempts) | unchanged by the proposal; the procedure below decides, metric by metric |
| Counts (`Unit` count) | all vi equal | `Measured` = `Budget` = that count; else as above |
| Not recorded | no value in the runs | unchanged; listed |

**Over budget: the machine or the code.** The budgets over which the campaign measures were set on this machine (a
`Measured` entry is the reference machine's, `Budgets.json`'s `_comment`) by commits up to wave 3; waves 4-6 ran only on
the Mac. The replaced CPU is the same model and the canary shows it at its reference speed (11.1), so the CPU does not
explain an entry over budget, and raising budgets for it would hide regressions of waves 4-6 (golden rule 2: never weaken
a test to make it pass; AGENTS.md: "a metric over budget is a regression to fix"). For each entry over budget:

1. Its suite runs alone under `PerfLock.py` on bc6ee123's Release build (nothing else running). Within budget: the
   campaign's load caused it; the entry stays.
2. Still over: `git log -S"<metric>" -- StrataTests/Perf/Budgets.json` names the commit C that last set the budget. C's
   `StrataTests` target is built in Release in a scratch worktree (`.worktrees/perf-<sha>`, `build/perf-<sha>`) and its
   suite measured alone, interleaved with bc6ee123's, 3 pairs. C as far over its budget (median ratio bc6ee123 / C within
   1.10): the machine changed; the entry is re-baselined with `--rebaseline <metric>="<C, both medians>"` (`Measured` M,
   `Budget` floor(1.5 M)) and listed in the roadmap's open items. C within its budget (ratio above 1.10): a regression
   between C and bc6ee123; bisected when the range builds in reasonable time, fixed in step 0 when the fix is small (its
   own commit, with its A/B), otherwise the budget stays, the test keeps failing, and the roadmap's open items name the
   metric, the range and what is known, as a blocker of 0.7.0 (plan 7.2: a regression is fixed before the release, not
   budgeted). C that no longer builds with this machine's compiler (MSVC 14.50 already needed `e65353b5` for a test):
   the budget stays and the open item says so.

The step-0 commit lists every decision with its numbers. DG "Perf tests" gains the procedure as the one way a budget goes
up (a machine change shown by the commit that set the budget measuring as far over on the same machine); AGENTS.md's
"budgets only go down" stays as it is.

The Mac's provisional metrics (most of the 78) are measured by this campaign like any other provisional entry; the
roadmap's list of metrics waiting for the reference machine loses those measured.

### 11.4 The commit

"Measure the perf budgets again on the replaced CPU" (S0.8a): `Budgets.json` from the proposal, reviewed by hand; its
message lists every changed entry as `<metric>: Measured <old> -> <new>, Budget <old> -> <new> (median of n, min-max)`,
grouped by class, with the canary per run, as AGENTS.md requires of commits that add or lower budgets, and each entry
over budget with its decision (load, re-baselined with C's numbers, fixed, or a release blocker). Then `ctest --preset
windows-release -L perf` once under `PerfLock.py` passes but for the metrics the commit names as release blockers.

### 11.5 The A/B of S0.3 and S0.4 at Medium (S0.8b)

- A: `G:/GameEngineAI/build/windows/bin/Release` (bc6ee123). B: the branch's Release build
  (`.worktrees/step0/build/windows/bin/Release`). Both with `STRATA_TEST_GPU_VALIDATION=0`.
- Five pairs, alternating which side runs first (A B, B A, A B, B A, A B): each run deletes its build's
  `PerfResults/Release.json`, runs `python Tools/PerfLock.py --label step0-ab -- <bin>/StrataTests.exe
  --test-suite=PerfGPU.Renderer --no-intro=true`, and copies the results file to `ab/<side>-<n>.json`; nothing else runs
  meanwhile.
- `python Tools/PerfStability.py --results ab/B-*.json --baseline ab/A-*.json --max-ratio 1.02`: every metric present in
  both (CPU and GPU) within 1.02, or within 0.01 ms where both medians are below 0.5 ms (the GPU timers' resolution);
  counts equal. Above it: the cause is found and fixed before step 0 lands (the plan's rule: a regression is fixed, not
  budgeted).
- One pair of `PerfGPU.Editor`, as information: A renders 0.6.0's fixed values, B the Medium its perf projects now store
  (7.9); a ratio above 1.02 is investigated before step 0 lands.
- The harness at `--quality medium`, 1920x1080 and 3840x2160: GPU busy per shot within 1.02 of 0.6.0's (information).
- The S0.4 commits that touch frame code state their ratios (7.14).

## 12. Perf metrics

New provisional entries (S0.4), in `T/Perf/RendererPerfTests.cpp` beside "Render cost of 100,000 and 300,000 cubes whose
shadows cover the view" (602-), measured at 3840x2160 with the cascade cache off (every cascade drawn each frame):

| Metric | Unit | Entry |
| --- | --- | --- |
| `PerfGPU.Renderer.Render100kCubesShadowed4K` / `...GPU` | ms | Provisional (Medium at 4K: the reference for High) |
| `PerfGPU.Renderer.Render100kCubesShadowedHigh4K` / `...GPU` | ms | Provisional, `Target` for `...GPU`: the Medium4K GPU median + 0.6 ms, set when the integration campaign measures Medium4K (until then none) |

The integration's campaign measures them with the streams' metrics (plan 7.2). Step 0 adds nothing else to `Budgets.json`
but S0.8a's changes.

## 13. What step 0 leaves the streams, and how their merges resolve

Step 0 lands on main before any stream branches, so it merges with nothing; what it shares is the layout it leaves:

- **Quality rows** (`R/RendererQuality.h/.cpp`): a stream adds a field, a row line, four function lines, a section field,
  a clamp line (7.2) and bumps `c_SectionVersion` by one; two streams that both bump to 2 are renumbered by the later
  lander (3), and both rows' lines are kept. `graphics-options` builds on `GetPreset`, `FromSectionValue`,
  `ToSectionValue`, `MakeSection`'s field list, `GameRenderer::SetQualityOverride` and `ViewportQuality`, as its spec's
  section 2.2 lists them (it was revised against this spec: these names stay). A players' comfort row (`camera-and-tone`'s
  `AntiAliasing`, `MotionBlur`) reads its field under every preset in `FromSectionValue`'s last block (7.2).
- **Slots and stages:** section 8: SRV 28, 32, 33 `sun-shadows`; 29-31, 34 `light-and-materials`; the next free 35, taken in
  landing order. GPU stages at section 8's positions, in `GPUProfiler.cpp` and the five lists, so the streams' lines do
  not meet.
- **Tests that draw at the program default:** the editor provides `IProjectSettings` whenever it runs
  (`E/EditorContext.cpp:151`) and an exported game from its manifest, so editor and game tests draw at the section's
  default, High, which equals Medium below 1440 rows at step 0. A stream whose row makes High differ from Medium below
  1440 rows (`ShadowFilterQuality` 24, `ContactShadows` 12, `AmbientOcclusionQuality` 3x8, `EnvironmentResolution` 512)
  changes those tests' images: it updates their expectations, or sets `Renderer.Quality` Medium in the test's project where
  the test is about something else. `GameRenderer` tests without a provider and every `SceneRendererSpecification{}` stay
  Medium (7.1).
- **AGENTS.md:** 2,787 code points free; each stream one digest bullet (their specs ask for about 330, 380, 400 and 480: about 1,600 in all);
  the later lander measures with `Docs.AgentGuide` after merging main.
- **The harness:** shots and look-dev content appended; the integrator keeps both sides' lines.
- **`Budgets.json`:** provisional entries appended by each stream; S0.8a's numbers are main's.
- **Files S0.3 created:** `SceneRendererPost.cpp` (camera-and-tone), `SceneRendererAmbientOcclusion.cpp`
  (light-and-materials); `RenderFrame` keeps two calls where each stream's edits would have been.

## 14. Risks and open questions, with the decision recommended

| # | Question or risk | Decision |
| --- | --- | --- |
| Q1 | The plan orders S0.6 after S0.3/S0.4, but only the harness can show that they change no image against the 0.6.0 build | Build the harness second (section 3); it needs no engine change, and the 0.6.0 baseline must be taken before `build/windows` is rebuilt anyway |
| Q2 | "About 80 test sites" | 13 lines (2.2); the 95 constructions keep Medium by default. No renamed-site risk worth a separate step |
| Q3 | The settings audit proposed baking probes at Ultra | `ForBake()` keeps 0.6.0's values: faces are 256 pixels (`E/ProbeBake.h:25`), where 2048-texel cascades already give several texels per pixel; Ultra would add up to 1.25 GiB to a bake and change `GPU.Editor.Lighting`'s values for no visible gain |
| Q4 | Ultra's Auto needs a second rule (4096 / 8192) | Two values, `Auto` and `AutoFine`, so the Custom column of the page and the section say exactly what each preset does |
| Q5 | `--quality` with `--headless` | Refused (exit 3), like `--screenshot` there: the option would do nothing |
| Q6 | The atmosphere bug's cause is unverified | Test first at the renderer level, then in the editor (5.2); the fix includes the sun-activation refresh and the three-map rebind regardless; an intensity change from 0 refreshes only if the test proves it must |
| Q7 | Memory at High and Ultra: 256 MiB (High) and 1.25 GiB (Ultra) per view renderer with a shadowed sun at 4K; the editor has three | The capture renderer releases after every picture; the Scene view's own quality lets a person drop to Medium; allocation failures halve the size with one error; Ultra is opt-in; `graphics-options`' page shows memory per row |
| Q8 | The editor's perf tests run maximized at 1440+ rows and would measure High | Their projects store Medium (7.9); High gets metrics of its own (section 12) |
| Q9 | A CTest that needs Python | Labels `gpu;tools` (`StrataTools.LookDev`) and `tools` (`StrataTools.PerfStability`), one stated exception for the `StrataTools.*` tests beside the package tests, a failing stand-in test when Python is missing; CI's runners have Python (`ci.yml:201-202`) and run the `tools`-only test |
| Q10 | Re-baselining up cannot tell the CPU from a regression of waves 4-6 | Not on the campaign's word: the canary shows the replaced CPU at its reference speed (11.1). An entry over budget is measured alone, then against the commit that set its budget (11.3); only a machine change proven that way raises a budget, and a regression is fixed or stays a 0.7.0 blocker in the open items |
| Q11 | A shot's camera computed from data may miss its subject | One look at every shot before the 0.6.0 baseline (9.5) |
| Q12 | Determinism of captures between builds (needed for 6.3 and 7.12) | Measured: the harness reports each shot's self-difference; `selftest` requires 0 with FXAA; if a mode is not deterministic on this driver, its comparisons use the self-difference as the floor and the commit says so |
| Q13 | `SetQuality` called every editor frame | One knob comparison when nothing changed; the A/B (11.5) and `PerfGPU.Editor` cover it |
| Q14 | Auto changes the cascades when a window is dragged across 1440 rows | Resizes rebuild every target already; the cascades follow in the same frames |
| R1 | A move in S0.3 changes evaluation order (MSVC inlining, FP contraction) and so pixels | The harness's 0-pixel check per commit; if a difference appears, the moved code is compared line by line before anything else lands |
| R2 | The streams' look-dev additions change existing shots | Rule: append entities and shots, never change existing ones (9.8) |
| R3 | High's 4096 cascades halve the PCSS caps in world units: today's search and filter radii are clamped to 32 texels (`SH/Include/Shadows.glsl:102, 121`), so wide penumbrae in the nearest cascade (casters more than about 5 m above their receivers in ThirdPerson's first cascade, 5.6 mm texels at 2048) come out narrower at High than at Medium, while the rest only gets finer | Accepted for the time between step 0 and `sun-shadows`' step 2, which replaces the filter (its D8 caps the half-width at 31 texels: about today's world width at 4096). Step 0 leaves `Shadows.glsl` alone, so Medium stays bit-identical; the harness's `tp.scene` and `lookdev.thin` shots show it, and the step-0 commit of High names it |
| R4 | The perf tests' re-measured numbers come from a machine other agents loaded (21-24% busy) | Entries over budget are measured again alone before any decision (11.3, step 1); lowered budgets (the "Cheaper" class) only get stricter, so load cannot loosen them |

## 15. First hour

1. `python Tools/PerfLock.py --status`: while the campaign holds the lock, read only (no build, no editor).
2. Read `plan.md`, this spec, `git -C .worktrees/step0 log --stat bc6ee123..HEAD`, and the wave-6 spec's sections on
   captures and the shadow cache (`git show ea66297b:Docs/Plans/Wave6/specs/render-quality.md`).
3. S0.1: apply section 4's ten replacements, check each dropped phrase against the guide section named, count with
   `python -c` the code points (target 36,949), and run `Docs.AgentGuide` once builds are allowed.
4. Start `Tools/LookDev.py`: the PNG codec and its round trip, `Shots.json` and `LookDevScene.json` as in 9.4-9.5 (no
   editor runs until the campaign has ended).
5. When the campaign has ended: check its validity (11.1), list its entries over budget and its failures that are not
   budgets (11.1 names those of runs 1-3), and run `--propose-budgets` once the mode exists; take the 0.6.0 look-dev
   baseline before anything rebuilds `build/windows`. Then start 11.3's "machine or code" measurements of the entries over
   budget (each alone under `PerfLock.py`; the commits that set their budgets built in scratch worktrees), which can run
   while S0.2-S0.4 are written.

## Review corrections

The independent review (2026-10-27) checked this spec against bc6ee123 and `agent/step0`'s five commits (none touches a
renderer file but `RenderScene.*`; `AGENTS.md` is unchanged there, `Docs/DeveloperGuide.md` gained four lines), the plan,
the audits, the campaign's first runs and the four stream specs beside it (`graphics-options` already revised against this
spec, `sun-shadows`, `camera-and-tone`, `light-and-materials`). Most file:line claims held: the 38 free code points and
the ten bullets' counts (recounted with the test's rule), the 13 test lines, the cascade and pyramid sites, the capture
atlas, the local tile constant, the atmosphere paths, the post and AO ranges, the runtime's options and the editor's
commands. Changed, and why:

1. **Major: SRV 32 was promised twice (section 8, 13).** `sun-shadows` takes scene SRV 32 and 33 (its D1: the cascades'
   depth ranges and the blue-noise tile), and this spec's "next free slot (32)" rule had given 32 to `light-and-materials`'
   volumetric fog, as that spec first wrote. The slot comment now assigns 28, 32, 33 to `sun-shadows` and 29-31, 34 to
   `light-and-materials` (whose revised spec now names 34); the next free is 35.
2. **Minor: GPU stage positions (section 8).** The positions are kept: the three stream specs, revised meanwhile, follow
   them. Added why inserting is safe against the plan's "stages appended" (registration order fixes ids and perf-log
   columns, `GPUProfiler.cpp:46`, but ids are compared only within a build and perf-log readers find columns by name),
   and the hard-coded lists are five, not three (the Architecture.md stage table and the editor-automation skill list them
   too).
3. **Major: the re-baseline rule would have hidden regressions (11.1, 11.2, 11.3, 11.4, Q10, first hour).** The canary
   reads 9.97-10.01 ms against its 10 ms reference in runs 1-3, so the replaced CPU is not slower, yet
   `Perf.Physics.Create10kBodies` (134%), `PerfGPU.Editor.ThumbnailsMaxFrame` (about 2x) and others failed all attempts;
   raising them to 1.5 times the new median would have weakened tests (golden rule 2) with no evidence. Over-budget
   entries are now measured alone, then against the commit that set their budget; only a machine change shown that way
   raises a budget (`--rebaseline <metric>=<reason>`, per metric), and a regression is fixed or stays a named 0.7.0
   blocker. The runs' interim findings, including two failures that are not budgets, are listed in 11.1.
4. **Major: the shortened perf rule changed the rule (S0.1).** The replacement asked for `PerfStability.py` before merging
   anything that can change performance; the rule asks for it only before changing budgets or perf tests, and plan 4.1
   keeps it out of the streams. The bullet keeps both occasions: 46 more code points, so S0.1 saves 3,013 (36,949 after
   it; 37,213 and 2,787 free when the streams branch), still above the plan's 2,500.
5. **Medium: `GetProjectQuality()` without a provider is Medium, not High (7.1, 7.11, 13).** Only tests and tools run
   without one (the editor provides it whenever it runs, `EditorContext.cpp:151`; games from their manifest), and
   `GPU.Runtime.GameRenderer` would otherwise change pixels as soon as a stream's row makes High differ from Medium below
   1440 rows. Section 13 now tells the streams that editor and game tests draw at High and what to do about it.
6. **Medium: the shadow map's requested and made sizes (7.5).** `SetQuality` compared the resolved size with the size
   made, so after an 8192 allocation fell back to 4096 every later quality change would release and retry it.
   `m_RequestedShadowMapSize` is now what changes compare. The local atlas gets the same halving fallback (Ultra's is
   256 MiB), placed where `EnsureLocalShadowAtlas` actually runs (after the frame's tiles, `SceneRenderer.cpp:2888`).
7. **Medium: the touch points of a row (7.1, 7.2, 7.8, 7.11).** `HasSameKnobs` uses a defaulted `operator==` on copies (an
   added, public operator; `graphics-options` uses `HasSameKnobs` and is unaffected), `renderStats.quality` is
   `ToSectionValue` as it is plus `shadowMapSize`, so neither needs a line per row. `FromSectionValue`'s shape is given,
   with the block where `graphics-options`' review requires comfort rows to read their field under every preset.
   `GetPreset` caches its four presets (the Scene view resolves every frame). Case 1 of `Renderer.Quality` now checks
   each preset against a table in the test and a Custom round trip: the named-preset round trip could not catch a missing
   `ToSectionValue` or `FromSectionValue` line, since a named preset ignores the fields.
8. **Medium: S0.2's sun toggle (5.3).** `HadSun` lives in `AtmosphereState` (reset with it on a switch away), and the maps
   in hand go to the spare instead of being dropped; its cost (one frame of the 14 steps a source switch already records)
   is stated.
9. **Minor: ordering and caches.** Callers apply the quality right after `SetViewportSize` (`GameRenderer.cpp:54`,
   `ViewportRenderer.cpp:119`), so `Auto` resolves for the frame's rows; the TAA's per-history post sets stay in
   `SceneRenderer.cpp` (6.1), and `camera-and-tone`'s motion blur is timed under its own stage in `RenderFrame`.
10. **Minor: tests that would pass without the change.** The capture-equals-view case now gives the point light a 10 m
    range, so the view wants 1024-texel faces that a 2048 atlas cannot hold (without a range, both renderers could pick
    512 and the case would pass today); the quality round trip turns `SpreadLightingUpdates` off; the `--quality` CTests
    name their fixture and timeout.
11. **Minor: the harness (9.2, 9.7, 9.8, section 3).** Its output rule allowed nothing under the repository, which refused
    its own CTest's binary directory; it now refuses only the tracked tree. Full runs go under `PerfLock.py --limit 90`
    (the default 25 minutes would end them), `--work` reuses imported copies, and import time is reported apart. The
    AGENTS.md exception names `StrataTools.*` (PerfStability's self-test needs Python too) at the same 32 code points. If
    the live mode falls below the cut, a bc6ee123 build is kept for its baseline, and `camera-and-tone` merges main for it.
12. **Minor: wrong numbers.** `ProjectSettings.h` has 100 lines (not 128), `ProjectCollisionLayers.cpp` 114 (not 140);
    the guide's lines are the branch tip's (3113, 3369-3371, 3740; Architecture.md 2369), stated once in 2.1.
13. **Added risk R3:** High's 4096 cascades halve today's 32-texel PCSS caps in world units, so tall casters' penumbrae in
    the nearest cascade are narrower at High than Medium until `sun-shadows`' filter lands; accepted, not patched, so
    Medium stays bit-identical. R4 covers the loaded campaign.

Answers to the author's open questions: the order (harness before S0.3/S0.4), accepted. S0.2's test-first procedure,
accepted with 5.3's corrections. `ForBake()` at 0.6.0's values, accepted (later streams add their own bake values, e.g.
`light-and-materials`' AO and environment rows). `AutoFine`, accepted: `graphics-options` already builds on the name. The
Python CTest exception, accepted for all `StrataTools.*` tests. Perf projects pinned to Medium with `*High4K` metrics,
accepted. The one-time re-baseline, rejected as written (correction 3). `--quality` with `--headless` refused, accepted
(`graphics-options` relies on it).

## As built

Step 0 landed as the branch `agent/step0` (on main after its review). Where it differs from the spec above, and the names
the streams must know:

- **Order.** The items landed in the order S0.1, S0.6, S0.2, S0.3, S0.4, S0.5, S0.8a, S0.7 (S0.8a waited for the
  campaign's last runs and the measurements alone); the review's eleven findings on the branch's first commits came
  first, in commits grouped by file. Two failures that were not budgets were fixed on the way: `GPU.Renderer.Timers` read a
  reset timestamp before the GPU ran the reset (Reset's contract allows it; it failed about once in ten runs), and the
  Open World tour ran a count of frames, which an unpaced window drew before the island's first cell arrived; it runs
  12 seconds of game time (`--seconds`) now. `Editor.Import`'s open-while-importing case waited by the clock and is
  gated by the import now.
- **S0.2.** The cause: on the RTX 5090 (driver 617.42), the environment cube's, irradiance's and prefiltered map's
  per-level transitions from the undefined layout lost the levels written earlier in a frame whose multiple-scattering
  dispatch ran too (levels 0-3 of the prefiltered map black). `SceneRenderer::CreateEnvironmentMaps(commandList)` moves
  each whole texture to UnorderedAccess before any level is written; `AtmosphereState::HadSun` and the rebinding of all
  three maps (`m_BoundEnvironmentCube`, `m_BoundIrradiance`, `m_BoundPrefiltered`) are as 5.3 says. The regression test
  fails 8 checks without the fix.
- **S0.3.** Besides the moved functions, `SceneRenderer::CreateComputePipeline` (a member) and `CreateRenderTarget` (a
  static member) are shared helpers, and the formats are static constexpr members (`c_HDRFormat`, `c_OutputFormat`,
  `c_AOFormat`, `c_AOFallbackFormat`). The harness found no image change: after each move every shot equal to the
  previous capture but the 4K TAA shots of the Open World's scene camera, which differ by at most one code value from
  run to run on the same build, and one 4K TAA look-dev shot that was identical when captured again alone.
- **S0.4.** Nine commits as 7.14, with these details: `ViewportRenderer::Render`'s quality has no default (tests pass
  `RendererQuality::GetProjectQuality()`); `GameRenderer::SetQualityOverride(quality, source)` takes the name the log
  line gives its source ("--quality"); `SceneRendererSpecification::MaxShadowMapSize` is floored to a power of two of at
  least 256; a failed cascade allocation keeps the frame (its cascades were fitted for the larger size, one frame), a
  failed atlas allocation skips the frame's local shadows as 7.5 says; `ProbeBake::GetRendererSpecification` is gone
  (`ForBake()`); `GPU.Editor.Lighting` checks the bake renderer's quality by what it writes, since the renderer lives in
  the command's state: the same bake with the project at Low and at Ultra writes the same file (a bake renderer made to
  follow the project writes other bytes at both);
  `GPU.Runtime.GameRenderer` does not read the log line (the test process logs warnings only; `StrataRuntime.Quality`
  does). On this machine High's 4096-texel cascades cost `GPU.Shadows` 0.142 -> 0.633 ms at 3840x2160 for 100,000
  shadowed cubes (`Render100kCubesShadowedHigh4KGPU` 2.476 against Medium's 1.763 ms): above the plan's 0.6 ms in that
  worst case. In the samples' scene-camera shots at 3840x2160 (captures: cascades
  cached, not redrawn) High costs 0 to 0.52 ms of GPU time over Medium (`tp.scene` with TAA 0.52, `basic3d.scene`
  0.25-0.32, the others below 0.03), with `shadowMapSize` 4096 in every 4K shot.
- **S0.5** as section 8.
- **S0.6.** `Shots.json`'s Open World project has `"LoadWorld": true`: the harness streams the island's cells in
  (`world.loadCells` over the whole world, waiting) before its shots, which otherwise showed the sea. Sessions live in
  `%TEMP%/StrataLookDev-<pid>-<time>` (a private directory cannot be made on every drive), removed at the end. Live
  captures are named `<shot>-<W>x<H>-live.png`. The shots were corrected on the 0.6.0 build: `tp.lantern` looks from
  (1.0, 2.6, -18.0) at (2.3, 0.9, -22.0) (a tree hid the lantern), `tp.feet` crops the feet at (0.3, 0.62)-(0.62, 0.88),
  the furnace's dielectric spheres stand at y 1.7 so the `metals` and `dielectrics` crops do not overlap. `compare`
  draws side-by-side crops only for pictures that differ, in a pool.
- **S0.8a.** `PerfStability.py` decides "over budget" before the spread: a metric failing every attempt is over budget
  even when its runs scatter. The four entries still over budget alone were not measured against builds of the commits
  that set their budgets (9e9cbd92, ebc2792e, 1f44d522) but against wave 3's finishing build (3b87411c, built on this
  machine on 2026-10-13 with the same budgets), which is within every one of them: regressions of waves 4-6, blockers of
  0.7.0 in the roadmap's open items. `PerfGPU.Editor.EditFrameNested100k`, within its budget alone on bc6ee123 but 1.26
  times wave 3's build, failed every attempt of step 0's own `-L perf` run and joined them. The campaign's second run
  once crashed the 250k world's save helper (0xC0000005) and Windows kept no report of it; 220 runs of the helper since
  (100 on one project with step 0's build, one at a time; 60 on fresh projects with it and 60 with bc6ee123's, two at a
  time) did not crash, so it stays listed with the unstable metrics in the roadmap. `Perf.Physics.Create10kBodies`,
  timed around box3d's calls in a scratch build: 11.4 of its 18.1 ms are box3d's own creation calls (`b3CreateBody`, the
  shapes, the mass), more than wave 3's 9.07 ms for all of it with Jolt; Strata's part is 6.7 ms (1.6 around those
  calls, 5.1 gathering the records and building the shapes on jobs). After the owner's ruling of 2026-10-27
  (`decisions.md`, `physics-runtime` Q1: whoever lands first), step 0 re-baselined it once with `--rebaseline` at the
  campaign's median, `Measured` 15.76 ms and `Budget` 23.64, Jolt's 7.995 ms kept as its `Target` (alone on step 0's tip
  15.63-15.78 ms, three runs): not a blocker; the four others stay blockers. DG "Perf tests" names the owner's ruling as
  the only other way a budget goes up.
- **S0.8b.** Five interleaved pairs of `PerfGPU.Renderer` (bc6ee123 against step 0's tip, both with bc6ee123's budget
  entries so neither confirms more attempts; `PerfStability.py --baseline --max-ratio 1.02`): 43 metrics, geometric mean
  0.994; above 1.02 `Skinned1000` 1.032, `Render10kMaterials` 1.038, `IdleFrame3kStreamedTextures` 1.024 (0.011 ms, the
  slack's edge) and the unstable maximum `AtmosphereSunMovingMaxFrame` 1.094. Six rounds of three builds (bc6ee123,
  e84fde21 after S0.3, the tip) placed `Skinned1000`'s 2% and `Render10kMaterials`' 2.5% in S0.4's commits, whose
  per-frame additions (a copy of the quality into the stats, a loop over at most 16 tile sizes) take well under a
  microsecond. They are the code's layout: the tip built again with its engine code 16, 32 and 48 bytes further on (a
  function of `__nop()`s before `main` in `TestMain.cpp`, nothing else changed) measured `Skinned1000` at 1.000 to 1.018
  of bc6ee123 (10 rounds); five rounds of the whole suite pinned to the P-cores put two builds of the tip 48 bytes apart
  at 0.932 to 1.067 of each other over its 19 CPU metrics (the two-state `Render300kCubes` and `Render1MCubes` left out;
  geometric mean 1.000; six above 1.02, among them `Render10kMaterials` 1.032), and both at a geometric mean of 0.977 of
  bc6ee123, with `Skinned1000` 1.014, `Render10kMaterials` 1.008 and `IdleFrame3kStreamedTextures` 0.994 but other
  metrics above 1.02 (`Render100kCubes` 1.109 and 1.115, while the ten other 100,000-cube frames, which add a feature to
  its path, scatter from 0.915 to 1.053); every GPU time within 0.974 to 1.008. A cost of step 0's would raise the
  metrics that share its path together, in every layout; these rises change metric with the layout and the run. The 1.02
  rule cannot tell such small CPU costs from layout; DG "Perf tests" says how an A/B is read now. One pair of
  `PerfGPU.Editor` (information): the editor frames 0.94-0.96, the open, export and play-start maxima 0.85-1.69 (maxima
  the campaign found unstable), the 1M world's open 1.04 and saved open 1.15. The harness at Medium
  (`lookdev/step0-medium`, 5 repeats): 52 of 122 stills identical to 0.6.0's; 57 differ in shots whose 0.6.0 frame drew
  shadowed point and spot lights (its 2048 atlas; tiles now as the views draw them), those of the look-dev project and
  the Open World also under the physical sky (S0.2's fix, which the captures taken right after it already showed); 12
  more under the physical sky alone (`lookdev.contact`, `lookdev.thin`); one 4K TAA shot by one code value in 13 pixels
  (run to run); GPU busy per shot at a median ratio of 1.003 (0.83-1.64, small 1080p shots). Step 0's tip at the
  projects' quality (High) is `lookdev/step0`, the streams' baseline.
- **After the second review.** `LocalShadowContent` holds the quality's `LODErrorPixels`, which 7.5 left out: a Custom
  quality that changed only it kept the cached point and spot views at their old levels of detail; a new error draws
  them again in their tiles. The Scene view draws the scene's camera ("Game camera", the canvas mode) at the project's
  quality, as 7.8's schema text says and captures of it draw (7.7 passed the view's setting for every camera):
  `GetSceneViewQuality`, which the panel and `viewport.capture` share. A minimized game keeps its cascades, as 0.6.0
  kept its targets (7.5 said it frees them): `Application::RunFrame` skips a minimized window's frames, so no zero
  viewport reaches its renderer; the comments and the guide name the editor's hidden Game view instead.
  `SceneRenderer::ResolveShadowMapSize` applies `RendererQuality::c_MaxShadowMapSize`, which nothing read.
