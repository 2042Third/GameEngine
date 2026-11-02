# Stream `physics-timing`: smooth at any frame rate (wave 8)

The owner's item 11 (2026-11-01, verbatim in `owner_feedback.md`): "Currently the physics frame rate is constant or something,
change this to an option or something and/or make it tick by tick following the frame refresh. Currently physics refresh
implementation is locked at 60 and is terrible at high frames (character appear to be jittering). (easy to do)". The
orchestrator's ruling 8 (`specs/decisions.md`, D5): draw between the last two fixed poses, on by default in games and Play,
adding about half a step of mean latency; step 0's report gives the owner the numbers. The option the owner asked for is
S0.9's `Scene.Timing.FixedRate`; this stream is the fix (plan 4.6). It delivers the owner's "tick by tick following the frame
refresh" as what the eye sees, without stepping physics by the frame's delta (rejected on its numbers, plan section 9).

The technique is the published one of fixed steps with state interpolation (G. Fiedler, "Fix Your Timestep!", 2004): the
design below is Strata's own and copies nothing. D10 allows reading permissive code; nothing here needs any.

This spec is written against main at 76f28a21 (0.7.0). Every file:line is 76f28a21's. Step 0 had not landed when this was
written: what this spec takes from S0.1, S0.3, S0.4, S0.6 and S0.9 is in 2.5, checked against their specs
(`specs/step0.md`, `specs/step0-editor.md`, `specs/step0-platform.md`) by the review, not against landed code. Where step 0
lands with other names the implementer uses them, keeps this design and records the differences in the first commit's message. Paths: `S/` is `Strata/src/Strata/Scene/`,
`R/` is `Strata/src/Strata/Renderer/`, `P/` is `Strata/src/Strata/Physics/`, `SC/` is `StrataScriptCore/Include/StrataScript/`,
`ED/` is `StrataEditor/src/`, `T/` is `StrataTests/src/`, `DG` is `Docs/DeveloperGuide.md`, `AUD/` is
`.worktrees/wave8-plan/physics-timing/` (the audit's folder).

Rules of the machine (AGENTS.md, plan 4.1, restated where they bind this stream):

- Worktree `.worktrees/physics-timing` from main once lane Render (S0.2, S0.3) has landed (plan 4, start conditions). Lane
  Platform's S0.9 is needed only from step 4 on: merge main when it lands, before step 4 starts. Build directory
  `build/agent-physics-timing`; never `build/windows`. The stream edits no shader, so it does not wait for lane Slang.
- Every commit: Debug build, Debug ctest with every `GPU*` suite under the validation layer, `Tools/CompatLint.py` on the
  changed files. CI green on `ci/wave8-physics-timing` before landing.
- Perf: no campaign, no `PerfStability.py`, nothing against `Budgets.json`; A/Bs under `Tools/PerfLock.py`, interleaved, as
  information (section 8); new metrics `Provisional`.
- Samples' scenes change only through editor commands; their scripts are code and change as code. Tests never open samples in
  place (`Tests::CopySampleProject`). Editors run with private session directories and `StrataCLI call --project`.
- box3d is read to learn its API, never changed. Nothing in `PhysicsWorld` changes beyond the teleport signals (plan 4).

## 1. Goal, in priority order

1. **Motion that reads smooth at any frame rate.** The Third-Person robot running at 6.5 m/s across the view, followed by its
   camera at a 6.5 m arm, moves on a 3840x2160 screen within **1 px** peak-to-peak of its own 100 ms average at 30, 60, 120,
   144, 240 and uncapped (700-1000) frames per second, and at S0.9's `FixedRate` 30, 120 and 240. Today: 2.67 to 29.4 px
   (2.3).
2. **The simulation untouched.** With interpolation on and off, the same frame times give the same poses at the same fixed
   step counts, bit for bit. Every tool that counts steps (`play.step`, `input.*` durations, `input.sequence`,
   `viewport.capture {live, atFixedStep}`, `GameRuntime::GetPlayedSeconds`, the samples' command regressions) counts as
   before; paused and stepped frames draw the simulated pose exactly.
3. **Everything drawn agrees.** Mesh proxies on both draw paths, skinned palettes, point and spot light records (and so their
   shadows), the sun's cascades, motion vectors, the scene camera and its lens' focus, world-space text and joint attachments
   are drawn at the same interpolated pose on every frame, with or without a step.
4. **Teleports stay teleports.** `Teleport`, `CharacterTeleport`, scripts' `ResetInterpolation`, per-frame moves and a speed
   safety net never draw a streak.
5. **Cheap.** Interpolation off costs nothing measurable. On, a frame without a step pays what a step frame pays today in
   proxy updates, plus one blend per moving entity; resting and sleeping bodies cost nothing; a run that draws nothing never
   computes a drawn pose.
6. **Reachable.** The project's `Scene.Timing.Interpolation` (default on) for people and agents; `play.state` and
   `physics.stats` say what runs; three host functions appended to the script ABI; the Third-Person camera follows the
   drawn pose and its arm lets out smoothly.

Not the goal: stepping physics by the frame's delta (plan 9: 10 to 14 times the cost at 700 fps, lost repeatability, every
step-counting tool broken); extrapolation (it overshoots at every stop and collision); substeps as a setting (plan 9);
changes to box3d; world-space UI, gizmos, picking, audio, reflection probes, fog and post-process volumes on drawn poses (the
cut line, 4.7).

## 2. Where we start

### 2.1 Files

| File | Lines | Role for this stream |
| --- | --- | --- |
| `S/Scene.h` | 847 | `SceneSettings` (60-78: `FixedTimestep` 63, `MaxFixedStepsPerFrame` 64), the transform API (344-407: `UpdateWorldTransforms` 347, `GetWorldTransform` 350, `FindCurrentWorldTransform` 354, `MarkTransformChanged(s)` 361-363, `GetTransformsVersion` 374, `GetWorldTransformChanges` 379-380), simulation (409-458: `SetPaused` 426, `Step` 429, `GetFixedStepCount` 434, `GetFixedTimestep` 436, `SetTimeDilation` 454), members (`m_DirtyTransforms` 754, `m_TransformChanges` 759, `m_FixedTimeAccumulator` 801, `m_TimeDilation` 803) (owned: interpolation API and members) |
| `S/Scene.cpp` | 2,315 | `c_ParallelTransformWidth` (30), `ApplyProjectLayers` (216-225), `Copy` (227-...), `MarkTransformDirty` (1451-1458), `FindCurrentWorldTransform` (1475-1491), `RecordTransformChanges` (1533-1548), `UpdateWorldTransforms` (1561-1671: `compute` 1596-1603, parallel 1614-1654, serial 1656-1666), `GetWorldTransform` (1673-1705), `AssertCachesValid` (1932-1937), `CheckCachesAfterUpdate` (1940-1948), `OnRuntimeStart` (2004-2054), `OnRuntimeStop` (2056-2084), `OnUpdateRuntime` (2113-2229: paused 2135-2151, flush 2161, `OnUpdate` 2163, stepping 2165-2170, the loop 2171-2216, `OnLateUpdate` 2218, final transforms 2225), `SetPaused` (2231-2243) (owned: the fixed loop and the world matrix update, plan 4) |
| `S/SceneHierarchy.h` | 36 | `HierarchyComponent` (14-34; `TransformDirty` 31): gains `HasWorldTransform` (owned) |
| `S/Components.h` | | `TransformComponent` (50-67), `WorldTransformComponent` (77-81) (read only) |
| `S/TransformInterpolation.h` (new) | | The set's types (owned) |
| `S/SceneInterpolation.cpp` (new) | | `Scene`'s interpolation members, like `SceneSubScenes.cpp` (owned) |
| `R/RenderScene.h` / `.cpp` | 564 / 1,644 | `Sync` (314-380: `UpdateWorldTransforms` 317, pending 323-337, log loop 339-351, `FullRefresh` 352-356, lights 358, skins 360, Debug `Validate` 365-378), `GetDrawnTransform` (397-405), `UpdateRenderTransforms` (407-438), `UpdateProxyTransform` (607-622), `EvaluatePose` (animator world 1271, proxy world 1276, `AnimatorToProxy` 1280), `UpdateSkinnedPoses`' skip (1314-1335), `Validate` (1462-...: drawn 1496-1500, lights 1639), clones (`CopyIntoScene` 164-171) (shared with `editor-ux`) |
| `R/RenderLights.h` / `.cpp` | 126 / 379 | `ComputeRecord` (149-186: `world->Matrix` 175, 183), `Sync` (269-298), `Validate` (329-377) (owned, plan 4) |
| `R/SceneRenderer.cpp` | 4,125 | `SceneCamera::FromEntity` (154-176: `GetWorldTransform` 164); the render scene's sync (2848-2854) before text (3502) (one hunk) |
| `R/SceneRendererPost.cpp` | 1,358 | `CameraLens::FromEntity` (243-273: camera 263, focus entity 268) (one hunk) |
| `R/TextRenderer.cpp` | 300 | `Prepare` (102-...): world-space text placed from `world.Matrix` (165) (one hunk) |
| `R/RenderSceneGPU.h` | | "Changes of resident proxies upload at once, every update" (49-53): both paths see per-frame moves with no change (read only) |
| `R/SceneRenderer.h` / `.cpp` | | Motion history copies what the scene's GPU copy wrote since the renderer's previous frame (`SceneRenderer.h:401-413`, `RecordMotionCopy` `SceneRenderer.cpp:1225-...`): no change (read only) |
| `P/PhysicsWorld.cpp` | 5,689 | `Teleport` (5300-5351: `WriteWorldTransform` 5318), `TeleportCharacter` (5368-...) (teleports only, plan 4) |
| `P/CharacterControllers.cpp` | 1,088 | `CharacterControllerSet::Teleport` (1025-1048), reached through `TeleportCharacter` (read only) |
| `S/ProjectSceneTiming.*` (S0.9, `specs/step0-platform.md` 4.1-4.3, 2.5) | | `Scene.Timing`: `ProjectSceneTiming::Timing` gains `Interpolation` (shared with S0.9's result) |
| `Strata/src/Strata/Runtime/GameRuntime.h` / `.cpp` | 201 / 432 | `GameRuntimeOptions` (30-57: `TimeDilation` 35-38), `m_TimeDilation` (187); `StartScene` (210-222: `SetTimeDilation` 216) (shared) |
| `StrataRuntime/src/RuntimeApplication.cpp` | 532 | `endsByItself` (79), `TimeDilation` (80), `renders` (87) (one line) |
| `ED/Editor/EditorContext.h` / `.cpp` | 1,027 / 2,935 | `EditorContextSpecification::TimeDilation` (117-119); `StartRuntime` (1355-1398: `SetTimeDilation` 1389), `ReplaceRuntimeScene` (1504-...: 1530) (shared) |
| `ED/EditorLayer.cpp` | 2,474 | `specification.TimeDilation` (106) (one line) |
| `ED/Editor/CommandUtils.cpp` | 630 | `DescribeRunningScene` (339-400), used by `play.state`, `editor.status`, `scene.info` (one hunk) |
| `ED/Editor/Commands/PlayCommands.cpp` | 276 | `play.state` (258-273: description) (one hunk) |
| `ED/Editor/EditorWorldCommands.cpp` | 1,090 | `physics.stats` (1027-...), `DescribePhysicsStats` (one hunk) |
| `SC/ScriptABI.h` | 842 | `ST_SCRIPT_ABI_VERSION` 1 (33), `StrataScriptTransform` (143-148), the host table (312-740; time 322-328, transform fast paths 362-366, `Teleport` 419, `CharacterTeleport` 548, graphics options 730-737, "appended here" 739) (shared: append only) |
| `Strata/src/Strata/Scripting/ScriptHostAPI.cpp` | 3,668 | Time (305-337), transforms (714-804: `HostGetWorldTransform` 755-774), `HostTeleport` (1339-1359), `HostCharacterTeleport` (1539-1556), `ST_SCRIPT_HOST_FUNCTIONS` (3383-3545, last `X(GraphicsSaveOptions)` 3545), its `static_assert` (3571) (shared: append only) |
| `SC/Time.h`, `SC/Entity.h`, `SC/Host.h` | 71, 463, 111 | `Time` (10-69), `TransformComponent` (160-233, implementations 411-462), `ST_SCRIPT_DETAIL_HOST_WITH` (109-111) (shared: additions) |
| `Samples/ThirdPerson/Scripts/FollowCamera.cpp` | 112 | `OnLateUpdate` (40-78: pivot from the target's world position 70), `Place` (80-90: the arm 84-86) (shared with nobody in wave 8 but `animation-graph`'s commands file) |
| `Strata/src/Strata/Animation/AnimationSystem.cpp` | 1,153 | `ApplyRootMotion` (788-827; controllers 801-810), `PlaceAttachments` (829-888, called in `OnUpdate` 712 and `OnPausedUpdate` 966; through a parent other than the animator 859-865) (read only) |
| `T/Scene/SceneTests.cpp` | 713 | `ScopedCountingSystem` (108-120), the fixed-step and time dilation cases (485-578): the pattern of the new suite |
| `T/Renderer/GPUSceneMotionTests.cpp` | 829 | `MotionScene` (34-85: it registers its own `TestSkinningPoses` as `ISkinningPoses`, 55), `CheckMotion`, `ReadMotionFrame` (28-259), `GPU.SceneRenderer.MotionVectors` (261-...) (shared: a suite appended) |
| `T/Renderer/GPUSceneRendererGPUDrivenTests.cpp` | | `Comparison` and its `Check` (78-160: color within 1 level, entity IDs and velocities equal), `GPU.SceneRenderer.GPUDriven` (603-...) (a case appended) |
| `T/Renderer/GPUSceneShadowTests.cpp` | | "Cascades are kept while nothing reaches them..." (417-493): the cached-against-redrawn pattern, `MaxDifference <= 1` (read only) |
| `T/Physics/PhysicsInterpolationTests.cpp` (new) | | The cases that need physics (6): left out of builds without physics by its folder (`StrataTests/CMakeLists.txt:76-77`) |
| `T/Physics/PhysicsSettingsTests.cpp` | | `Physics.Settings` already compares Strata's defaults with `b3DefaultWorldDef()` (115-118; the file includes `box3d/box3d.h`, 17) (a check appended) |
| `T/Renderer/RenderSceneTests.cpp` | 1,362 | `Renderer.RenderScene` (133-...; lights 1128) (a case appended) |
| `T/Animation/AnimationSystemTests.cpp` | 1,698 | Attachments (593), root motion with a controller (832) (a suite appended at the end, `animation-graph` owns the rest) |
| `T/Scripting/ScriptAPITests.cpp`, `ScriptModuleTests.cpp` | 710, 839 | "Transform API" (230-243); the API module's class list (100-108) |
| `StrataTests/Scripts/API/TransformScripts.cpp` | 58 | The API module's transform probe (a class appended) |
| `StrataTests/FeatureTest/Scripts/TimeFeatures.cpp` | 70 | Time's feature script (calls added); a new `InterpolationFeatures.cpp` |
| `T/FeatureTest/FeatureTestUtils.cpp` | 538 | `PlayFeatureScene` (377-410: per-frame cache checks 400-406) |
| `T/Perf/ScenePerfTests.cpp`, `RendererPerfTests.cpp` | 259, 1,098 | `Perf.Scene` (81-...), `PerfGPU.Renderer` cases |
| `StrataTests/Editor/ThirdPersonCommands.json.in` | 87 | The sample's regression (FollowCamera lines 48-52); run by `StrataEditor.ThirdPerson` (`StrataTests/CMakeLists.txt:920-...`) |

### 2.2 A frame today

`Scene::OnUpdateRuntime` (`Scene.cpp:2113-2229`) runs, in order:

1. Sub-scenes (2119-2125); a scene that is not running only updates world transforms (2126-2133).
2. Paused without a pending step: world transforms, `OnPausedUpdate`, world transforms (2135-2151).
3. `deltaTime` is the frame's timestep times the time scale, or one fixed step while stepping (2153-2158).
4. `UpdateWorldTransforms()` (2161), then every system's `OnUpdate` (2163): scripts, animation (`PlaceAttachments`
   writes attachments' local transforms here, `AnimationSystem.cpp:712`; root motion through a controller only writes the
   turn, 801-810, and queues the move for the next fixed step), the UI, world streaming, physics' `OnUpdate`.
5. Stepping (paused with `Step`): exactly one `OnFixedUpdate` (2165-2170). Otherwise the accumulator loop (2171-2216): up to
   `MaxFixedStepsPerFrame` steps of `GetFixedTimestep()`; time dilation stops catch-up steps once they took longer than they
   simulated (2180-2209); beyond the maximum the backlog is dropped and the accumulator set to one step (2210-2215). In each
   step physics syncs bodies, steps characters and box3d and writes poses back into `TransformComponent` with
   `MarkTransformsChanged` (no `on_update`); scripts' `OnFixedUpdate` runs before physics.
6. `OnLateUpdate` (2218): scripts (the Third-Person camera places itself here), audio.
7. Deferred destruction, then `UpdateWorldTransforms()` (2224-2225), and in Debug the cache check (2226-2228).

The accumulator's remainder (`m_FixedTimeAccumulator`, `Scene.h:801`) is never read by anything that draws: the renderer
draws each entity's `WorldTransformComponent::Matrix` (`RenderScene.cpp:397-405`, through the transform change log
339-351), which changes only in frames that ran a step. Above the step rate most frames run no step (91.5% at 702 fps,
60 Hz steps) and the character stands still in them, while the camera, written every frame in `OnLateUpdate`, slides on:
the audit's raw series shows +29 px on each step frame and a slide back of about 1.75 px per ms until the next
(`AUD/audit.md`, "The jitter, measured").

Today's drawn time lags the wall clock by 0 to one step (the accumulator), a mean of half a step.

### 2.3 What the audit measured

Character's chest projected to 3840x2160 at 60 degrees, peak-to-peak residual from its own 100 ms centered average
(`AUD/tools/analyze.py`, `jitter`), frames with the arm at full length (`AUD/audit.md`):

| frames/s | steps/s | 0.7.0, px p2p (RMS) | interpolated, px p2p |
| --- | --- | --- | --- |
| 702 | 60 | 29.4 (8.57) | 0.25 in engine (at 463 fps) |
| 240 | 60 | 23.6 (8.22) | 0.54 replay |
| 144 | 60 | 27.0 (7.94) | 0.15 in engine |
| 120 | 60 | 16.7 (7.25) | 0.71 replay |
| 60 | 60 | 2.67 (0.49) | 0.20 in engine |
| 30 | 60 | 2.75 (0.39) | 0.29 in engine |
| 772 | 30 | 58.8 (17.2) | 0.81 replay |
| 475 | 120 | 14.75 (4.33) | 0.21 in engine (at 904 fps) |
| 469 | 240 | 7.38 (2.22) | 0.25 replay |

- A higher rate only halves the jitter per doubling (29.4, 14.75, 7.38 px at 60, 120, 240 Hz).
- Drawing only the character between steps while the camera follows the simulated pose leaves 1.3 px at 702 fps and 2.1 px at
  144 fps: the camera must follow the drawn pose too.
- Latency in the prototype: the drawn pose a mean of 6.9 ms (at most 16.5 ms) behind the simulation at 60 Hz, 4.3 ms (at most
  8.2 ms) at 120 Hz. Drawn time becomes a constant one step behind the wall clock: mean latency rises by half a step, the worst
  case is unchanged.
- Cost on step frames today (editor Play, loaded machine): `Render.Extract` p95 0.50 ms at 4,096 moving boxes and 3.6 ms at
  32,768, about 0.11-0.12 us per moving proxy; non-step frames 0.004 ms. `Scene.Transforms` 0.36 ms at 32,768.
- `FollowCamera`'s arm shortened from 6.5 to about 3.05 m in one frame and back 75 frames later (frames 2453 and 2528 of
  `AUD/data/run1.csv`): a 131-141 px jump each way (`FollowCamera.cpp:85-86`).

### 2.4 Who reads world transforms to draw

| Reader | Where | After this stream |
| --- | --- | --- |
| Mesh proxies | `RenderScene::Sync` and `FullRefresh` through `GetDrawnTransform` (`RenderScene.cpp:339-356`, 382-405), `UpdateProxyFromEntity` (501, 507) | Drawn transform |
| Skinned palettes | `EvaluatePose`: proxy's drawn transform (1276) against the animator's **world** transform (1271) | Both drawn (a bug once proxies interpolate: the skinned robot would be drawn at its simulated pose) |
| Point and spot lights | `RenderLights::ComputeRecord` (`RenderLights.cpp:175`, 183) | Drawn transform |
| Scene camera | `SceneCamera::FromEntity` (`SceneRenderer.cpp:164`), used by `GameRenderer.cpp:128`, `ViewportRenderer.cpp:32`, `GameUIHitTest.cpp:204` | Drawn transform |
| Lens focus | `CameraLens::FromEntity` (`SceneRendererPost.cpp:263`, 268) | Drawn transform |
| World-space text | `TextRenderer::Prepare` (`TextRenderer.cpp:165`) | Drawn transform |
| Motion vectors | Each renderer's copy of the instances it drew (`SceneRenderer.h:401-413`) | Unchanged: they follow the drawn instances |
| GPU-driven path | `RenderSceneGPU` uploads every change of resident proxies at once (`RenderSceneGPU.h:49-53`) | Unchanged |
| Sky, sun direction, fog, post-process volumes, reflection probes | `SceneRenderer.cpp:877`, 1336, 1388, 2512, 2529; `SceneRendererReflectionProbes.cpp:183` | Simulated (cut line: none rides a body in any sample) |
| Editor gizmos, picking rays, world-space game UI canvases | `SceneGizmos.cpp:184-300`, `GameUIScene.cpp:884` | Simulated (cut line; paused frames agree exactly) |
| Audio | `AudioSystem.cpp:670-698` (already smooths step-only motion) | Simulated |
| World streaming source | `WorldStreamingSystem.cpp:129-131` | Simulated (right: streaming follows the game) |

### 2.5 What step 0 provides (from its specs; not yet landed)

- **S0.9** (lane Platform, `specs/step0-platform.md` section 4; it lands by itself before this stream branches, its Q-P1):
  `S/ProjectSceneTiming.{h,cpp}`, namespace `ProjectSceneTiming` with `c_SectionId = "Scene.Timing"`, `c_FixedRateField`,
  `c_SceneRate`, `c_Rates[] = { 30, 60, 120, 240 }`, `MakeSection()` (a simple section, `Category` "Physics", version 1,
  one Enum field `FixedRate`: "Scene" (default), "30", "60", "120", "240"), `struct Timing { uint32_t FixedRate = 0;
  uint64_t Version = 0; }` and `const Timing* Find()` (null without `IProjectSettings`; parsed again only when the provider's
  version changes, as `ProjectCollisionLayers::Find`, `ProjectCollisionLayers.cpp:91-112`), `GetMaxSteps(rate)`; registered
  in `RegisterSceneModule` (`S/SceneRegistration.cpp:18`). On `Scene`: `GetFixedTimestep()` (the project's rate when it sets
  one), `GetMaxFixedSteps()` (read at `Scene.cpp:2174` and `GameRuntime.cpp:38`), `GetProjectFixedRate()`, and
  `bool ApplyProjectTiming()` ("whether the rate the scene runs at changed"), with members `m_ProjectFixedRate` and
  `m_ProjectTimingVersion` that `Scene::Copy` copies (step0-platform 4.2). `ApplyProjectTiming()` is called right after each
  `ApplyProjectLayers()`: `Prefab.cpp:411-412`, 427, `SceneLoader.cpp:325-328`, `EditorContext.cpp:549-558` (both scenes, and
  `AdoptProjectLayers`), and through `EditorContext::ApplyProjectLayers` after every project settings change
  (`ProjectSettingsAction.cpp:75-77`). `HostGetFixedDeltaTime` (`ScriptHostAPI.cpp:325-326`) reads `GetFixedTimestep()`.
  `FeatureTest.stproj` stores `"Scene.Timing": { "Value": { "FixedRate": "60" }, "Version": 1 }`, so the feature runs, which
  advance 1/60 s a frame, run one step per frame. Its suite `Scene.Timing` lives in `T/Scene/SceneTimingTests.cpp`.
- **S0.3** (lane Render, `specs/step0.md` section 7): `ShadowFitTracker`'s persistence in seconds; the regression cases
  `Renderer.ShadowFit` "A ball shot from the camera leaves the layout alone at any frame rate" and `GPU.SceneRenderer.ShadowFit`
  "A ball shot from the camera leaves the cascades alone" (7.5); the layout-change count is the existing
  `SceneRendererStats::ShadowFitChanges` (`R/SceneRenderer.h:683`), which S0.3 reports as `editor.perf {frames: 1}`'s
  `gameRenderStats.shadowFit.changes` (and `holding`) (7.4); its acceptance scripts are the shadow-jump audit's, adapted to
  read that count and kept in `.worktrees/wave8-plan/step0/render/` (7.8).
- **S0.1**: room in AGENTS.md, about 2,330 code points free when the streams branch for nine streams' 2,250, at most 250 per
  stream's bullet; a lander that would pass 40,000 shortens a bullet of its own area (`specs/step0.md` section 12 and its
  review item 6). AGENTS.md is 39,292 code points (39,308 bytes) at 76f28a21; `Docs.AgentGuide` bounds it
  at 40,000.
- **S0.4**: `Editor.FrameRate.ActiveFrameRate` (Unlimited, 240, 165, 144, 120, 90, 60) caps the editor's frame rate; the
  acceptance runs use it where it has the value. **S0.6** may find why FIFO does not pace the editor; the acceptance runs
  state the frame rates they measured, whatever paces them.

## 3. Decisions that hold for every step

**D1 The simulation is untouched.** Nothing the steps read changes: `TransformComponent`, `WorldTransformComponent`, the
accumulator, the step count, physics. Interpolation adds a second, drawn transform beside the world transform, read only by
what draws (2.4). The one change to the loop's order, a world transform update before the fixed steps while interpolating
(4.1), computes matrices earlier, never other values: `UpdateWorldTransforms` and `GetWorldTransform` compute with the same
association (`Scene.cpp:1601`, 1698), and physics reads `FindCurrentWorldTransform` or `GetWorldTransform`. A test proves it
bit for bit (6, `Scene.Timing`).

**D2 Off below the applications.** A bare `Scene` does not interpolate (`SetInterpolation(false)`, as `SetTimeDilation`,
`Scene.h:454`). The programs that play on the wall clock turn it on where they turn time dilation on: the editor's Play and
Simulate outside scripted runs (`EditorLayer.cpp:106`) and `StrataRuntime` outside `--frames` and `--seconds` runs, when it
renders (`RuntimeApplication.cpp:79-88`). The project's `Scene.Timing.Interpolation` can turn it off. Tests, tools, command
files and headless runs keep drawing exactly what they simulate.

**D3 Roots blend, descendants compose (plan 4.6, review section 12).** An entity whose own transform changed during the fixed
steps (its `HierarchyComponent::TransformDirty` was set when the update after the steps recomputed it) is a *Blend* entry:
drawn between its world pose before the stepping frame's steps and after them. An entity recomputed only because an ancestor
changed is a *Compose* entry: drawn as its parent's drawn matrix times its current local transform, so it stays on its parent
exactly (a joint attachment that `PlaceAttachments` writes every frame stays on its joint). *Exact* entries are drawn where
they are. The set is kept in walk order, parents first.

**D4 Exact when in doubt.** A teleport, a per-frame translation of a Blend entry, a move through a per-frame write to an
ancestor, a reparent, a sheared world matrix, a step faster than box3d's maximum speed and a new entity without an
interpolated parent all draw exact poses. A wrong exact pose costs at most one step of judder; a wrong blend draws a streak.

**D5 Drawn transforms are computed when something draws.** The set is captured in the update (it needs the matrices before
the steps), but the drawn matrices are computed by `Scene::UpdateDrawnTransforms()`, which `RenderScene::Sync` calls: a game
run without a renderer never blends. `Scene::GetDrawnTransform` is exact at any time (cached, or computed on demand).

**D6 One read path for drawing.** Renderer code places what it draws with `Scene::FindDrawnTransform` (after
`UpdateDrawnTransforms`) or `Scene::GetDrawnTransform`, falling back to the world matrix; nothing else reads interpolation
state. DG states it as a rule (9).

**D7 Nothing new in the renderer's inputs.** No shader, no quality row, no `FrameConstants` field, no frame-profiler stage
(the capture and the blend run in the existing `Scene.Transforms` stage), no new shadow-cache version: interpolation changes
proxies' transforms through `UpdateProxyTransform` and light records through `UpdateRecord`, the inputs the caches already
compare (`PrepareShadowCache`, `FindLocalShadowCasters`). Moving casters are redrawn at the frame rate instead of the step
rate: that is correct (a shadow must follow its drawn caster) and is measured (8).

**D8 The ABI grows by appending.** Three host functions after `GraphicsSaveOptions`, `ST_SCRIPT_ABI_VERSION` stays 1, the SDK
checks for them with `ST_SCRIPT_DETAIL_HOST_WITH` and degrades to the world pose (4.5).

## 4. Steps

Each step lands working, tested and documented before the next starts (section 12 lists the commits).

### 4.1 Step 1: capture and blend (M)

**Types** (`S/TransformInterpolation.h`, new, Scene layer; included by `Scene.h`):

```cpp
namespace Strata
{
	inline constexpr uint32_t c_NoInterpolationSlot = ~0u;

	// How an entity of the interpolated set is drawn (Scene::GetDrawnTransform; Docs/DeveloperGuide.md, "Interpolation").
	enum class InterpolationRole : uint8_t
	{
		Blend,   // Its own transform changed in the fixed steps: drawn between its world pose before them and after them
		Compose, // Moved only with an ancestor: its parent's drawn transform times its current local transform
		Exact    // Drawn where it is: a teleport, a per-frame move, a reparent, shear, or new without an interpolated parent
	};

	// A world pose in parts, as the blend reads them.
	struct InterpolationPose
	{
		glm::vec3 Translation = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 Scale = glm::vec3(1.0f);
	};

	// An entry of the set besides its entity and drawn matrix (Scene keeps the three in parallel arrays, by slot).
	struct InterpolatedEntry
	{
		uint32_t ParentSlot = c_NoInterpolationSlot; // Its hierarchy parent's slot when the parent is in the set
		InterpolationRole Role = InterpolationRole::Exact;
		InterpolationPose From; // Blend: the world pose before the stepping frame's steps
		InterpolationPose To;   // Blend: after them; per-frame rotation and scale writes keep it current
	};

	struct InterpolationStats
	{
		uint32_t Entities = 0;
		uint32_t Blended = 0;
		uint32_t Composed = 0;
		uint32_t Exact = 0;
	};
}
```

**Scene's public API** (`Scene.h`, a new block after "Simulation", 409-458):

```cpp
// Interpolation of what is drawn (Scene/SceneInterpolation.cpp; Docs/DeveloperGuide.md, "Interpolation"). While the scene
// interpolates, every entity the fixed steps moved (bodies, characters, what scripts move in OnFixedUpdate, and what hangs
// below them) is drawn between its pose before the latest stepping frame's steps and its pose after them, a constant one
// fixed step behind the scene's clock (without interpolation the latest pose is drawn, between none and one step behind it),
// so motion is smooth at any frame rate. The simulation itself is untouched: world transforms, the accumulator and the step
// count are those of a scene that does not interpolate.
//
// The program's switch, off by default so that tests and tools draw exactly what they simulate (GameRuntimeOptions::
// Interpolation, EditorContextSpecification::Interpolation turn it on where time dilation is on). Turning it off draws
// every entity where it is from the next read.
void SetInterpolation(bool enabled);
bool IsInterpolationEnabled() const { return m_InterpolationEnabled; }
// In effect: the switch, the project's Scene.Timing Interpolation (ApplyProjectTiming; on without a project) and a running
// scene.
bool IsInterpolating() const;
// The fraction of the way from the poses before the latest stepping frame's steps to those after them that this frame
// draws, in [0, 1]: 1 - (1 - accumulator / step) / steps. 1 while not interpolating, paused or stepping. Final after the
// frame's fixed steps (OnLateUpdate); during OnUpdate it is the previous frame's.
float GetStepAlpha() const { return IsInterpolating() ? m_StepAlpha : 1.0f; }
// Where the entity is drawn this frame: its interpolated transform while it is in the set, else GetWorldTransform. Exact at
// any time: the cache of UpdateDrawnTransforms when it is current, otherwise computed from the set (costs the entity's
// depth). Entities of another scene ask theirs.
glm::mat4 GetDrawnTransform(Entity entity) const;
// The drawn matrix of an entity of the set as of the last UpdateDrawnTransforms, else null (draw its world matrix). Only
// reads, so worker threads may call it while nothing changes the scene. Readers call UpdateDrawnTransforms first in the frame.
const glm::mat4* FindDrawnTransform(entt::entity handle) const;
// Brings the drawn matrices up to date after UpdateWorldTransforms; returns at once while nothing changed (no set, the same
// step alpha, no write since). RenderScene::Sync calls it, so a run that draws nothing never blends. Large sets on the job
// system. Main thread.
void UpdateDrawnTransforms();
// Changes whenever drawn matrices may have changed (UpdateDrawnTransforms recomputed them, the set was rebuilt, cleared or
// broken): readers of GetDrawnMatrices compare it.
uint64_t GetDrawnTransformsVersion() const { return m_DrawnVersion; }
// The set in walk order (parents first) and its drawn matrices as of the last UpdateDrawnTransforms, index for index. Empty
// while the scene does not interpolate. Entities may have been destroyed since: check them.
std::span<const entt::entity> GetInterpolatedEntities() const { return m_InterpolatedEntities; }
std::span<const glm::mat4> GetDrawnMatrices() const { return m_DrawnMatrices; }
// Draws the entity and its descendants where they are until the next fixed step moves them again: a teleport.
// PhysicsWorld::Teleport and TeleportCharacter call it, and scripts through ResetInterpolation. During the fixed steps it
// applies to the poses those steps leave; outside them at once. Nothing for entities outside the set or while the scene does
// not interpolate.
void BreakInterpolation(Entity entity);
InterpolationStats GetInterpolationStats() const;
// Checks the set (its slot map, walk order, Compose entries' parents, finite poses): false with the first problem. Debug
// builds run it with the caches' check (CheckCachesAfterUpdate).
bool ValidateInterpolation(std::string* outError = nullptr) const;
// A step longer than this speed times the time it simulated is a teleport, never blended: box3d's maximum linear speed
// (b3WorldDef::maximumLinearSpeed, 400 m/s, Strata/vendor/box3d/src/types.c:26; PhysicsWorld keeps box3d's default,
// which Physics.Settings checks).
static constexpr float c_MaxInterpolatedSpeed = 400.0f;
```

Private additions: `enum class TransformPass : uint8_t { Update, Capture }`; `void UpdateWorldTransforms(TransformPass pass)`
(the public overload calls it with `Update`); `void CaptureInterpolation(uint32_t steps, float timestep)`;
`void ClearInterpolation()`; `uint32_t FindInterpolationSlot(entt::entity handle) const` (the slot only when
`m_InterpolatedEntities[slot] == handle`, the whole handle with its version, so an entity that reuses a destroyed one's index
is never taken for it); `InterpolationPose GetWorldPose(entt::entity handle, const glm::mat4& world, bool& outExact) const`
(`outExact`: the pose cannot be blended, a sheared matrix or a failed `Math::DecomposeTransform`, zero scale or non-finite);
`glm::mat4 BlendPose(const InterpolatedEntry& entry, float alpha) const`. The walk's per-entity hooks are chosen once per call,
outside `compute`: `UpdateWorldTransforms` instantiates its walk for three cases (no hook: an `Update` pass while the set is
empty, which is every pass of a scene that does not interpolate; the per-frame rules; the capture), so a scene that does not
interpolate runs today's loop with no added branch per entity (only `compute`'s store of `HasWorldTransform`). A capture
record:

```cpp
struct TransformCapture
{
	entt::entity Entity = entt::null;
	glm::mat4 Previous;      // The cached world matrix before this pass
	bool OwnChange = false;  // HierarchyComponent::TransformDirty was set: the entity's own transform or parent changed
	bool HadWorld = false;   // HierarchyComponent::HasWorldTransform was set: Previous is a computed matrix
};
```

Members (beside the runtime members, 795-811):

```cpp
bool m_InterpolationEnabled = false;  // SetInterpolation
bool m_ProjectInterpolation = true;   // Scene.Timing Interpolation (ApplyProjectTiming)
std::vector<entt::entity> m_InterpolatedEntities;   // By slot, parents first
std::vector<InterpolatedEntry> m_InterpolatedEntries; // By slot
std::vector<glm::mat4> m_DrawnMatrices;             // By slot, as of the last UpdateDrawnTransforms
std::vector<uint32_t> m_InterpolationSlots;         // By entt::to_entity: the slot, or c_NoInterpolationSlot
std::vector<TransformCapture> m_Captures;           // Scratch of a capture pass
std::vector<entt::entity> m_NextEntities;           // Scratch of the set a capture builds (swapped in)
std::vector<InterpolatedEntry> m_NextEntries;
std::vector<uint8_t> m_KeptSlots;                   // Scratch: old slots the new set keeps
std::vector<entt::entity> m_PendingBreaks;          // BreakInterpolation during the fixed steps
float m_StepAlpha = 1.0f;
uint32_t m_InterpolationSteps = 1;                  // Steps of the stepping frame that built the set
float m_InterpolationTimestep = 0.0f;               // Their fixed timestep
float m_DrawnAlpha = -1.0f;                         // The alpha m_DrawnMatrices hold
bool m_DrawnStale = true;
bool m_InFixedSteps = false;
uint64_t m_DrawnVersion = 0;
```

`HierarchyComponent` (`SceneHierarchy.h:14-34`) gains `bool HasWorldTransform = false; // UpdateWorldTransforms computed the
cached world matrix at least once` after `ChildIndicesValid` (33): it fits the struct's padding (40 bytes either way).
`compute` (`Scene.cpp:1596-1603`) sets it.

**The frame** (`Scene::OnUpdateRuntime`, changes only while `IsInterpolating()`; off, the function is today's):

```cpp
// After the paused branch (2135-2151), before the accumulator:
if (interpolating && m_InterpolationTimestep != 0.0f && fixedTimestep != m_InterpolationTimestep)
	ClearInterpolation();                                 // A new rate (S0.9's FixedRate, scene.setSettings): poses of the old one
...
ForEachSystemCall(m_UpdateCalls, ...);                    // 2163
if (stepping)
{
	ClearInterpolation();                                 // Stepped frames draw exactly (already empty: SetPaused cleared it)
	... one step as today ...
}
else
{
	if (interpolating)
		UpdateWorldTransforms(TransformPass::Update);     // Per-frame writes of OnUpdate settle before the steps
	m_FixedTimeAccumulator += deltaTime;
	... maxSteps, stepsStart, as today ...
	m_InFixedSteps = interpolating;
	while (...) { ... }                                   // 2185-2200, unchanged
	m_InFixedSteps = false;
	... dilated / backlog, unchanged (2202-2215) ...
	if (interpolating)
	{
		if (steps > 0)
			CaptureInterpolation(steps, fixedTimestep);   // UpdateWorldTransforms(Capture) and the new set
		const float behind = 1.0f - std::min(m_FixedTimeAccumulator / fixedTimestep, 1.0f); // In steps
		m_StepAlpha = std::clamp(1.0f - behind / static_cast<float>(m_InterpolationSteps), 0.0f, 1.0f);
	}
}
ForEachSystemCall(m_LateUpdateCalls, ...);                // 2218: scripts read GetDrawnTransform (computed on demand)
... deferred destruction, UpdateWorldTransforms() (2224-2225), Debug check ...
```

Why the formula: after the loop the latest step ended at simulation time T, the accumulator holds `a` (`0 <= a < h`, or
`a == h` after a dropped backlog), and the set's poses are those at `T - n h` (before the stepping frame's `n` steps) and at
`T`. Drawing at `T + a - h`, one step behind the wall clock, lies at the fraction `(n h - h + a) / (n h) = 1 - (1 - a / h) / n`
between them. Frames without a step keep `n` and `T`, so the fraction grows with `a` and the drawn time keeps pace with the
clock. Hitches, time dilation and dropped backlogs only ever move the drawn time forward: with `d` dropped steps the drawn
time advances by `dt - d h >= h - a_before > 0` (each dropped step was due). At time scale 0 nothing accumulates, so the alpha
freezes; slow motion accumulates slowly and is drawn smoothly instead of a step at a tenth of the rate.

**The capture pass.** `UpdateWorldTransforms(TransformPass pass)` is today's function with a hook beside `compute` in the
walks that need one (the instantiations of the private additions above), in the serial (1656-1666) and parallel (1621-1649)
walks alike:

- *Capture*: before overwriting `worlds.get(handle).Matrix`, append a `TransformCapture` (the old matrix, `node.TransformDirty`,
  `node.HasWorldTransform`). The serial walk appends to `m_Captures`; each parallel job appends to a local vector merged under
  the walk's mutex, like `changes` (1641-1648), but uncapped. Each frontier subtree stays whole in one job and in walk order
  (the job's stack pops a node before pushing its children), so the merged records are parents first wherever the jobs' lists
  land.
- *Update*, while the set is not empty: the per-frame write rules below, for every recomputed entity that is in the set (one
  slot lookup per recomputed entity); `m_DrawnStale = true` when at least one recomputed entity has a slot (a flag the jobs
  set with a relaxed atomic store). Writes to entities outside the set (the Third-Person camera, placed every frame in
  `OnLateUpdate`) leave the drawn matrices alone, so a frozen alpha (time scale 0) recomputes nothing and the render scene
  skips its walk.

`CaptureInterpolation(steps, timestep)` runs `UpdateWorldTransforms(TransformPass::Capture)` (the pass that today runs at 2225
recomputes exactly these entities, so this moves the recomputation, it does not add one), then builds the new set:

1. **Fill** (on the job system with `JobSystem::ParallelFor` above `c_ParallelTransformWidth` records, `Scene.cpp:30`; serial
   below): record `i` becomes slot `i` of `m_NextEntities`/`m_NextEntries`. With `old` its slot in the current set (if any):
   - `To` = `GetWorldPose`: for an entity without a parent its `TransformComponent` (translation, rotation, scale: its world
     matrix is exactly their composition, `Scene.cpp:1602`); with a parent, `Math::DecomposeTransform` of the new world matrix,
     *exact* when its columns are not orthogonal (`|dot(ci, cj)| > 1e-4 * |ci| * |cj|`) or the decomposition fails
     (`Math.cpp:9-33`: non-finite, zero scale).
   - `From` = the old entry's `To` when the old entry is a Blend (the per-frame rules keep it equal to the matrix before the
     steps), else `GetWorldPose` of `Previous` (a decomposition: only when an entity starts moving or changes role).
   - Role: `HadWorld` false (created during the steps): decided in pass 3. `OwnChange` false: Compose. Otherwise Blend, made
     Exact when `Previous` equals the new matrix bit for bit, when either pose is exact (`outExact`), or when
     `length(To.Translation - From.Translation) > c_MaxInterpolatedSpeed * steps * timestep` (the safety net: 6.67 m at 60 Hz,
     1.67 m at 240 Hz).
   - `m_KeptSlots[old] = 1` (each old slot belongs to one entity, so jobs never write the same byte).
2. **Exits and slots** (serial): every old entry not kept whose entity is still valid has left the set: its last drawn matrix
   was a blend and it must be drawn where it is once. They are logged with
   `RecordTransformChanges(++m_TransformsVersion, exits, exits.size() > c_MaxTransformChanges)`, so every reader of the change
   log (`RenderScene`, `RenderLights`, `GameUIScene`) rewrites them from their drawn transform, now their world matrix. Then
   the old slots are cleared from `m_InterpolationSlots` (only where they still name that slot) and the new ones set (the
   vector grows to the largest entity index plus one), and the new arrays are swapped in (the old ones become the next
   capture's scratch), so passes 3 and 4 and `FindInterpolationSlot` read the new set.
3. **Parents** (parallel above the width): `ParentSlot` = the slot of the entity's `HierarchyComponent::Parent`. A record with
   `HadWorld` false becomes Compose when its parent has a slot, else Exact. A Compose entry whose parent has no slot (it
   cannot happen: an entity recomputed without its own change has its parent recomputed in the same subtree) becomes Exact.
4. **Breaks** (serial, only when `m_PendingBreaks` is not empty): every entity broken during the steps, and every Blend entry
   below one (one forward pass: an entry is broken when its parent slot is), becomes Exact; `m_PendingBreaks` is cleared.
5. `m_DrawnMatrices.resize(count)`, `m_InterpolationSteps = steps`,
   `m_InterpolationTimestep = timestep`, `m_DrawnStale = true`, `++m_DrawnVersion`.

An entity that stopped moving (a body fell asleep, a character stands) is not recomputed, so it leaves at the next stepping
frame with one exact final write and costs nothing afterwards. Entries stay Exact rather than being removed until the next
capture rebuilds the set (at most one step period).

Because exits and breaks reach readers through the transform change log, `Scene.h`'s comments change with them: the class
comment (90-101) gains the drawn transform beside the cached world transforms, and `GetWorldTransformChanges` (375-380) lists
"the entities whose `WorldTransformComponent` changed, and those whose drawn transform jumped back to it (they left the
interpolated set, or `BreakInterpolation`)". Its readers (`RenderScene`, `RenderLights`, `GameUIScene.cpp:884`) only re-read
those entities, so the extra entries cost a lookup each. `GetInterpolationStats` counts the roles by a walk of the set: for
commands and tests, never per frame.

**Per-frame writes** (outside the capture: the `Update` pass before the steps, the one after `OnLateUpdate`, those of
`RenderScene::Sync` and of commands). For a recomputed entity in the set:

| Role | What recomputed it | Rule |
| --- | --- | --- |
| Blend | its own write, translation moved more than `1e-5 * max(1, length(old translation))` | Exact: it asked to be there (a script's per-frame move, an edit while playing, a teleport outside the steps) |
| Blend | its own write, translation within that | The write is a turn or a scale (the player turning to walk, root motion's turn, `AnimationSystem.cpp:801-810`): `From.Rotation = To.Rotation` and `From.Scale = To.Scale` = the written pose's, `To.Translation` = the written one. The turn is drawn as written on every frame, the translation keeps blending. (Blending the turn from the captured rotation would draw it lagging and catching up at every step: a sawtooth of its own.) The tolerance absorbs the rounding of `SetWorldTransform` under a parent (`Scene.cpp:1722`). |
| Blend | an ancestor's write | Exact: its world poses no longer describe it |
| Compose | anything, its parent unchanged | Nothing: it composes from its current local transform every frame |
| Compose | a reparent (its hierarchy parent is no longer its parent slot's entity) | Exact |
| Exact | anything | Nothing |

A write to a descendant cancels nothing (plan 4.6): attachments that `PlaceAttachments` writes every frame
(`AnimationSystem.cpp:829-888`, in `OnUpdate`, 712) are Compose entries under their animator.

**Drawn matrices** (`UpdateDrawnTransforms`): returns at once unless `IsInterpolating()`, the set is not empty and
`m_DrawnStale || m_DrawnAlpha != m_StepAlpha`. Otherwise, in the `Scene.Transforms` stage:

1. Blend and Exact entries (parallel with `JobSystem::ParallelFor` above `c_ParallelTransformWidth`, serial below; they read
   only the registry's storages and their own entry, and write only their own drawn matrix): an invalid entity is skipped;
   Exact draws `WorldTransformComponent::Matrix`; Blend draws the world matrix when `m_StepAlpha >= 1` (exact at alpha 1, bit
   for bit, whatever the decomposition), else `BlendPose`:
   `t = From.T + (To.T - From.T) * a` (this form, not `glm::mix`, keeps positions monotonic and exact at the ends far from
   the origin), `q = normalize(From.R * (1 - a) + (dot(From.R, To.R) < 0 ? -To.R : To.R) * a)` (nlerp along the shorter arc:
   at most 0.03 degrees from a slerp at 30 degrees a step, 0.9 at 90, 4.5 at 150: a body spinning faster than half a turn a
   step cannot be told from one turning back), `s = From.S + (To.S - From.S) * a`, `Math::ComposeTransform(t, q, s)`.
2. Compose entries, serially in slot order (parents first): the parent's drawn matrix (`m_DrawnMatrices[ParentSlot]`) times
   `TransformComponent::GetTransform()`, the association `compute` uses (1602), so a Compose entry under a parent drawn where
   it is equals its world matrix bit for bit.
3. `m_DrawnAlpha = m_StepAlpha`, `m_DrawnStale = false`, `++m_DrawnVersion`.

**`GetDrawnTransform(Entity)`**: another scene's entity asks its scene; outside the set, or not interpolating:
`GetWorldTransform`. In the set with a current cache (`!m_DrawnStale`, `m_DrawnAlpha == m_StepAlpha`,
`m_DirtyTransforms.empty()`): its drawn matrix. Otherwise computed as the walk would, applying a pending write's rule the way
the next update will (without changing the entry): Exact, or Blend at alpha 1, its world transform (`GetWorldTransform`);
Blend with its path dirty (`FindCurrentWorldTransform` null): when only its own `TransformDirty` is set (no dirty ancestor)
and the translation of `GetWorldTransform` lies within the per-frame rule's tolerance of its cached matrix's, `BlendPose`
with the rotation and scale of `GetWorldTransform`'s decomposition in place of both poses' (a turn written this frame is
drawn as written, the translation keeps blending), otherwise its world transform (the next update makes it Exact); Blend,
`BlendPose`; Compose, `GetDrawnTransform(parent) * TransformComponent::GetTransform()` while its hierarchy parent is its
`ParentSlot`'s entity, else its world transform. Scripts call it in `OnLateUpdate` (the cache is stale then: the capture
just rebuilt the set, or the alpha moved) and get exactly what will be drawn, unless they write the entity or an ancestor
afterwards.

**`BreakInterpolation(Entity)`**: nothing unless interpolating. During the fixed steps (`m_InFixedSteps`): append the handle to
`m_PendingBreaks` (pass 4 of the capture) and make its current entry Exact, if any. Outside them: make its entry and every
Blend entry below it Exact (a forward pass from its slot), log it as a transform change (its drawn matrix jumps to its world
matrix), `m_DrawnStale = true`, `++m_DrawnVersion`.

**`ClearInterpolation()`**: log every valid entity of the set as a transform change, clear the arrays and the slots they hold,
`m_PendingBreaks.clear()`, `m_StepAlpha = 1`, `m_InterpolationSteps = 1`, `m_InterpolationTimestep = 0`, `++m_DrawnVersion`.
Called by `SetPaused(true)` while running (2231-2243: a paused game is drawn exactly, so gizmos, picking and captures agree
with what is drawn), stepping frames, `SetInterpolation(false)`, `ApplyProjectTiming` turning the field off, a change of the
fixed timestep, `OnRuntimeStart` and `OnRuntimeStop`. Resuming needs nothing: the next stepping frame builds a set whose
`From` is the paused pose, so the drawn pose only ever moves forward.

**Copies.** `Scene::Copy` copies no set: the copy's set is empty and its switch off (programs set it, as they set time
dilation). It copies `m_ProjectInterpolation` beside S0.9's `m_ProjectFixedRate` and `m_ProjectTimingVersion`
(`specs/step0-platform.md` 4.2): the copy carries the version it took the project's values from, so `ApplyProjectTiming` on
the copy returns at once and would never read the field again. Without that copy, Play (`EditorContext.cpp:1374` copies the
edited scene) would interpolate in a project that turned it off.

**Debug.** `AssertCachesValid` (1932-1937) also runs `ValidateInterpolation` while the set is not empty, in the same paced
check (`c_CacheCheckEntitiesPerUpdate`): every valid entity's slot maps back to it; Compose entries' parent slots are smaller
than their own and name their hierarchy parent; Blend poses are finite with unit quaternions (within 1e-3). It does not check
that the drawn cache is current (it is computed lazily); `RenderScene::Validate` checks proxies against it after its sync.

### 4.2 Step 2: to the renderer (M)

- **`RenderScene::Sync`** (`RenderScene.cpp:314-380`): right after `m_Scene->UpdateWorldTransforms()` (317),
  `m_Scene->UpdateDrawnTransforms()`, so pending proxies (336-337), the log loop and `FullRefresh` read current drawn
  matrices. After the log loop or the full refresh (339-357): when `m_Scene->GetDrawnTransformsVersion() != m_DrawnVersion`,
  walk `GetInterpolatedEntities()`: for each valid entity with a proxy,
  `UpdateProxyTransform(proxy, GetDrawnTransform(entity, world), world.ActiveInHierarchy, false)`. `SameBytes`
  (`RenderScene.cpp:611`) makes a frozen alpha (time scale 0) and a second renderer's sync of the same frame free; the
  version makes them skip the walk altogether. Entities in both the log and the set are written twice in a step frame; the
  second write compares equal (about 10 ns). Then `m_DrawnVersion` = the scene's.
- **`RenderScene::GetDrawnTransform(entity, world)`** (397-405): the poses' provider's render transform (the editor's pose
  preview) first, then `m_Scene->FindDrawnTransform(entity)`, then `world.Matrix`. Every proxy write goes through it
  (`UpdateProxyFromEntity` 501, 507; log loop 349; `FullRefresh` 392; `UpdateRenderTransforms` 429).
- **Skinned proxies.** `EvaluatePose` (1271): the animator's transform is `m_Scene->FindDrawnTransform(inputs.Animator)`, else
  its world matrix (`RenderScene::Validate`'s palette check, which goes through `EvaluatePose`, follows by itself). Without
  this, a posed proxy whose animator interpolates (the Third-Person robot: `Robot`, a child of `Player`, holds the
  `Animator` with `ApplyRootMotion` false, `Course.stscene:152`, so it is a Compose entry) would get `AnimatorToProxy = inverse(drawn proxy) * simulated animator` and
  be skinned back to its simulated pose. `UpdateSkinnedPoses`' skip (1323-1335) also compares the scene's drawn version
  (`m_SkinDrawnVersion`), so a skin whose animator and proxy lie in different hierarchies follows on frames without a step.
  Animated skins already run the pass every frame (their poses' version changes), so this costs nothing new for them.
- **Lights.** `RenderLights::Sync(const Scene& scene, std::span<const entt::entity> transformChanges, bool allTransforms,
  bool drawnChanged)` and `RenderLights::Validate(const Scene& scene, std::string* outError)` replace the registry
  parameters (`RenderLights.h:53-55`, 76-78; their one caller is `RenderScene`, 358 and 1639). The private
  `UpdateRecord` and `ComputeRecord` (`RenderLights.h:87-89`) take `const Scene& scene` in place of the registry as well and
  read `scene.GetRegistry()`; `RenderLights.h` forward-declares `class Scene` (Renderer may include Scene). `ComputeRecord`
  places a light at `scene.FindDrawnTransform(entity)`, else its world matrix (175, 183); `MakeRecord` keeps its signature.
  The caller passes `drawnChanged` as the same version comparison its proxy walk makes (above). When `drawnChanged`, after
  the change list, `Sync` updates the records of `scene.GetInterpolatedEntities()` (both types, `Find` first); the early
  return without lights
  (278-279) keeps light-less scenes free. A light that rides a body (a lantern on a cart, a torch on a character) moves every
  frame, and its shadow views follow through the record changes the local shadows already compare.
- **`RenderScene::Validate`** (1496-1497): the expected transform is the render transform, else the scene's drawn transform,
  else the world matrix. `CopyIntoScene` (164-171) sets the clone's `m_DrawnVersion = ~0ull`.
- **The camera.** `SceneCamera::FromEntity` (`SceneRenderer.cpp:164`) and `CameraLens::FromEntity`
  (`SceneRendererPost.cpp:263`, 268) read `scene.GetDrawnTransform`, never `FindDrawnTransform`: their callers build the
  camera before the frame's render scene sync (`GameRenderer.cpp:127-128`, `ViewportRenderer.cpp:32-33` call them, then
  `Render`, whose sync runs `UpdateDrawnTransforms`), so the cache still holds the previous frame's alpha and only the
  on-demand path gives this frame's pose. A camera under a body (the showcase's first-person walk
  on a controller, the Open World's explorer, `Samples/OpenWorld/Scripts/Explorer.cpp:1`) is a Compose entry and is drawn with
  the body; a camera a script places in `OnLateUpdate` is not in the set and is drawn where it was placed. The Game view
  (`ViewportRenderer.cpp:32`), `GameRenderer` (128) and the game UI's pointer rays (`GameUIHitTest.cpp:204`) all go through
  it, so a world-space button is hit where the image shows it, relative to the camera.
- **Text.** `TextRenderer::Prepare` (`TextRenderer.cpp:102`) calls `scene.UpdateDrawnTransforms()` first (a no-op after the
  render scene's sync, `SceneRenderer.cpp:2854`, which precedes it at 3502; the call keeps the order from mattering) and places
  world-space text at `FindDrawnTransform`, else `world.Matrix` (165): a name above a character stays above it.
- **Motion vectors and both paths** need no change (2.4): the GPU-driven path uploads every change of resident proxies in the
  frame it happens, and each renderer's motion history holds what its previous frame drew, so a body now moves a frame's worth
  in every frame instead of zero then a step's worth (better TAA reprojection and motion blur around moving bodies above the
  step rate; reasoned, the GPU test below measures the vectors).

### 4.3 Step 3: teleports (S)

- `PhysicsWorld::Teleport` (`PhysicsWorld.cpp:5300-5351`): after `WriteWorldTransform` succeeded (5318),
  `scene.BreakInterpolation(entity)`; the entity's descendants, which `UpdateDescendantsOfMovedBody` moves (5349), are broken
  with it by the scene's rule.
- `PhysicsWorld::TeleportCharacter` (5368-...): after `data.Characters->Teleport` succeeded,
  `data.OwnerScene->BreakInterpolation(entity)`.
- Both reach scripts already: `HostTeleport` (`ScriptHostAPI.cpp:1339-1359`, through `IBodyControl::Teleport`) and
  `HostCharacterTeleport` (1539-1556). The Third-Person respawn (`ThirdPersonPlayer.cpp`, `Respawn`) teleports in `OnUpdate`,
  before the steps: the per-frame rule would cancel it anyway; the break makes it exact for a rotation-only teleport too.
- Scripts that move an entity far inside `OnFixedUpdate` by setting its transform call `ResetInterpolation` (4.5); the speed
  net (4.1, fill) catches what they forget beyond 400 m/s.
- `Physics.Settings` (`T/Physics/PhysicsSettingsTests.cpp`, beside its comparisons with `b3DefaultWorldDef()` at 115-118)
  gains `CHECK(Scene::c_MaxInterpolatedSpeed == b3DefaultWorldDef().maximumLinearSpeed)`. No world Strata creates sets
  another speed: `PhysicsWorld`'s constructor starts from `b3DefaultWorldDef()` (`PhysicsWorld.cpp:4680`) and grep finds no
  `maximumLinearSpeed` in `Strata/src`; DG's Interpolation section says a change of either must change both.
- Paused or stepping, the set is empty (4.1), so `play.step`, `input.sequence` (which pauses and steps,
  DG "Simulated input") and `viewport.capture {live, atFixedStep}` armed after `play.start {paused: true}` (its documented use,
  `EditorViewportCommands.cpp:1180-1183`) draw exact poses. A live capture armed while the game runs on the clock shows the
  first frame drawn after step N, which lies between steps N-1 and N: DG "View commands" says so.

### 4.4 Step 4: the switch (S)

**The field.** `ProjectSceneTiming::MakeSection()` (S0.9) gains, after `FixedRate`:

```cpp
SettingsField interpolation;
interpolation.Name = "Interpolation";
interpolation.Title = "Interpolation";
interpolation.Type = SettingsFieldType::Bool;
interpolation.Default = true;
interpolation.Description = "Draw what the fixed steps move (physics bodies, character controllers, entities scripts move in "
	"OnFixedUpdate, and what rides on them: children, lights, cameras) between its last two fixed poses, so that motion is smooth "
	"at any frame rate; the simulation itself is unchanged. What is drawn is then a constant fixed step behind the game's clock "
	"instead of between none and one step: half a step more on average (about 8 ms at 60 steps per second; FixedRate 120 halves "
	"it), the same at worst. Off draws each fixed step's pose as it is, which judders whenever the frame rate is above the step "
	"rate. Applies to exported games and the editor's Play and Simulate from the next frame; scripted runs (--frames, --seconds, "
	"--commands) and tests never interpolate.";
```

`ProjectSceneTiming::Timing` (S0.9, `specs/step0-platform.md` 4.1) gains `bool Interpolation = true;` and the constant
`c_InterpolationField = "Interpolation"`. `Find()` reads it with `JsonUtils::GetBool(*value, c_InterpolationField, true)`
(`Core/JsonUtils.h:41`) beside the rate, and its cached `Version` changes when either field changes (S0.9's comment "Changes
only when FixedRate does" becomes "when FixedRate or Interpolation does"). `Scene::ApplyProjectTiming()` copies
`Interpolation` to `m_ProjectInterpolation` and calls `ClearInterpolation()` when it turns off; its return keeps S0.9's
meaning (whether the rate changed), and its comment names both fields. `Scene::Copy` copies the member (4.1, Copies). The
section's version stays 1 whatever releases ship: a simple section validates a missing member to the field's default at any
version (`SettingsRegistry.cpp:364`), so a value S0.9 stored reads as `Interpolation: true` (S0.9's own spec says the same,
its section 10). The generic settings page draws the Bool with no UI work (DG "Settings pages").

**The programs.**

- `GameRuntimeOptions::Interpolation` (`GameRuntime.h`, after `TimeDilation`, 38): "Draw what fixed steps move between its
  last two poses (Scene::SetInterpolation, as the project's Scene.Timing allows): for games that render on the wall clock. Off
  by default, so that tools and tests that drive the runtime with chosen timesteps draw what they simulate." Kept in
  `m_Interpolation` (beside `m_TimeDilation`, 187) and applied in `StartScene` beside `SetTimeDilation` (216).
  `RuntimeApplication.cpp` sets `runtimeOptions.Interpolation = !endsByItself && renders;` (79-88): a headless server draws
  nothing, and `--frames`/`--seconds` runs end on a frame that must not depend on the machine.
- `EditorContextSpecification::Interpolation` (`EditorContext.h`, after `TimeDilation`, 117-119), applied in `StartRuntime`
  (1389) and `ReplaceRuntimeScene` (1530) beside `SetTimeDilation`, for Play and Simulate alike (both draw a running scene on
  the wall clock). `EditorLayer.cpp:106` sets it with time dilation:
  `specification.Interpolation = specification.TimeDilation;`. So interactive and automation sessions interpolate, command
  files (`--commands`: every sample's CTest, `StrataTests/CMakeLists.txt:797-...` from `StrataEditor.Tetris` to
  `StrataEditor.ThirdPerson` at 920, and `StrataEditor.FeatureTest`, 720-724) and tests do not; nor do the exported games
  those CTests run with `--frames` (`GAME_FRAMES`, `StrataRuntime.FeatureTest`'s `--headless --frames 240`, 730-733).
- A change of the field while playing (`project.setSettings`) reaches the running copy through `ApplyProjectTiming`, which S0.9
  calls after every change of the project's settings (`ProjectSettingsAction.cpp:77` calls `ApplyProjectLayers` there today);
  its undo after play changes the edited scene only.

**Commands** (no new command; schemas unchanged, results gain members):

- `DescribeRunningScene` (`CommandUtils.cpp:339-400`, so `play.state`'s `running`, and `editor.status` and `scene.info`
  where they use it) gains `interpolation: {enabled, stepAlpha, entities}`: `enabled` is `IsInterpolating()`, `stepAlpha` the
  frame's alpha, `entities` the set's size. `play.state`'s description (`PlayCommands.cpp:259-263`) names it: "interpolation
  {enabled: drawn between fixed steps (the project's Scene.Timing Interpolation in games, Play and Simulate outside scripted
  runs), stepAlpha: how far between the last two steps' poses this frame draws, entities: how many are drawn between them}".
- `physics.stats` gains `interpolation: {enabled, stepAlpha, entities, blended, composed, exact}`
  (`GetInterpolationStats`) while a game runs (it answers `{running: false}` while editing, as today), and its description a
  sentence: "interpolation: what is drawn between the last two fixed steps (blended: moved by the steps, composed: carried by
  what moved, exact: drawn where it is)".

### 4.5 Step 5: script API and the Third-Person camera (S)

**ABI** (`SC/ScriptABI.h`, appended after `GraphicsSaveOptions`, 737, before the "appended here" comment, 739):

```c
	/*
	 * Interpolation (added after GraphicsSaveOptions, ABI version 1: check ST_SCRIPT_HAS_MEMBER before use). While the
	 * scene interpolates (the project's Scene.Timing Interpolation, in exported games and the editor's Play and Simulate),
	 * what the fixed steps move is drawn between its pose before the latest stepping frame's steps and its pose after them,
	 * a constant one fixed step behind the scene's clock. GetStepAlpha is the fraction of the way this frame draws, from 0 to 1
	 * (1 while the scene does not interpolate, is paused or is stepped), final after the frame's fixed steps (OnLateUpdate;
	 * OnUpdate reads the previous frame's). GetDrawnTransform writes the entity's world transform as it is drawn this frame
	 * (its world transform when nothing interpolates it): cameras and effects that follow a moving body read it in
	 * OnLateUpdate. ResetInterpolation draws the entity and its descendants where they are until the next fixed step moves
	 * them again: call it after moving an entity far by setting its transform in OnFixedUpdate (Teleport and
	 * CharacterTeleport do it themselves). It fails only for an entity that does not exist.
	 */
	float (*GetStepAlpha)(StrataScriptContext* context);
	bool (*GetDrawnTransform)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptTransform* outTransform);
	bool (*ResetInterpolation)(StrataScriptContext* context, StrataScriptEntityID entity);
```

**The host-function checklist** (DG "Adding a host function", 1754-1773):

1. Appended at the end of `StrataScriptHostAPI` as above; nothing inserted, reordered or removed.
2. `ScriptHostAPI.cpp`, a new section after the graphics options (3373), each body in `HostCall`, starting with
   `ResolveContext`:
   - `float HostGetStepAlpha(StrataScriptContext* context)`: `system->GetScene().GetStepAlpha()`, `1.0f` when the context
     fails (draw the latest pose: the value of a scene that does not interpolate).
   - `bool HostGetDrawnTransform(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptTransform* outTransform)`:
     `HostGetWorldTransform`'s body (755-774) with `GetDrawnTransform`; a degenerate matrix is reported with
     `ReportProblem("GetDrawnTransform", "the drawn transform of '<name>' is degenerate")` and returns false.
   - `bool HostResetInterpolation(StrataScriptContext* context, StrataScriptEntityID entity)`: `RequireEntity`, then
     `BreakInterpolation`, true (also when the entity is not interpolated: nothing to reset is no misuse).
   `X(GetStepAlpha) X(GetDrawnTransform) X(ResetInterpolation)` are appended to `ST_SCRIPT_HOST_FUNCTIONS` after
   `X(GraphicsSaveOptions)` (3545); the `static_assert` (3571) fails the build until list and struct agree.
3. The SDK, each through `ST_SCRIPT_DETAIL_HOST_WITH(<Name>)` (`SC/Host.h:109-111`):
   - `SC/Time.h`: `static float GetStepAlpha()`: "How far between the poses before and after the latest fixed steps this
     frame draws what they moved (0 to 1; 1 while nothing interpolates). Final in OnLateUpdate." An older engine: `1.0f`.
   - `SC/Entity.h`, `TransformComponent` (160-233): `glm::vec3 GetDrawnPosition() const` and `glm::quat GetDrawnRotation()
     const` ("Where the entity is drawn this frame: between its last two fixed poses while it is interpolated, else its world
     position (rotation). Cameras and effects that follow a moving body read these in OnLateUpdate."), through a private
     `StrataScriptTransform GetDrawn() const` that falls back to `GetWorld()` on an older engine; `bool ResetInterpolation()`
     ("Draws the entity and its descendants where they are until the next fixed step moves them: after moving it far by
     setting its transform in OnFixedUpdate."), false on an older engine.
4. Exercised in the API test module, tested in `Scripting.API`, called by the feature scripts (6).
5. `ST_SCRIPT_ABI_VERSION` stays 1 (`ScriptABI.h:33`). DG's "Host function order" bullet (1712-1716) gains "then wave 8's
   interpolation functions (`GetStepAlpha` to `ResetInterpolation`)".

**The Third-Person camera** (`Samples/ThirdPerson/Scripts/FollowCamera.cpp`):

- The pivot follows the drawn pose: `Target.GetTransform().GetDrawnPosition()` instead of `GetWorldPosition()` (70). The
  teleport test beside it (72-73: more than 8 m snaps) stays; `OnCreate` (35-36) keeps the world position (nothing is
  interpolated before the first step).
- The arm lets out smoothly. A new field `float ArmReturnSharpness = 6.0f; // How quickly the arm grows back once nothing is
  in the way (1/s)`, registered with `ST_SCRIPT_FIELD`. `Place(float deltaTime)` computes the free length as today (84-86);
  shorter than `ArmLength`, or `deltaTime <= 0`, it is taken at once (a wall never hides the player, the README's promise);
  longer, `ArmLength += (free - ArmLength) * (1 - exp(-ArmReturnSharpness * deltaTime))`. `OnCreate` calls `Place(0)`;
  `OnLateUpdate` passes its unscaled delta time. In the audit's run 1 the arm came back from 3.05 to 6.5 m in one frame
  (`AUD/data/run1.csv`, frame 2528); with the fix it grows by about 0.02 m in a 1.4 ms frame and is 95% back in half a
  second. The pull-in stays a cut, the standard behavior of a collision arm (section 11, Q1).
- `Samples/ThirdPerson/README.md`'s camera row: "shortens its arm with a raycast so that walls never hide the player, and
  lets it out smoothly; it follows the robot as drawn between physics steps (`GetDrawnPosition`)".
- The commands file (`StrataTests/Editor/ThirdPersonCommands.json.in`) gains a check (section 6, `StrataEditor.ThirdPerson`).
  Its setup is fixed, not found by trial: `component.set` the player's Translation to (0, 0.02, -19) (on the course at
  x = 0, 2.6 m before where the audit's run 1 lost and regained the arm, z = -21.6 to -22.3, `AUD/data/run1.csv` rows
  2453-2528), `script.setField` the camera's `Yaw` 90 and `Pitch` -18, `play.step {steps: 30}` to settle, then the
  sequence. The implementer runs that block once on the unfixed sample to read K (the step whose sample is back at 6.5 m)
  and writes K into the file's two `expect`s; if the run shows no shortened arm from that start, the start moves along
  -z in 0.5 m steps until it does, and the commit names the start used.

### 4.6 Step 6: perf, S0.3's checks, the acceptance runs, docs (S)

The metrics and A/Bs of section 8, the acceptance runs of section 7 (S0.3's included), the docs of section 9.

### 4.7 Cut line

Cut first, in order, only once everything above is done and the usage rule allows:

1. A cap on interpolated bodies and parallel proxy placement (`UpdateProxyPlacement`): only if section 8 shows the per-frame
   proxy work capping frame rates in a real scene (the audit's estimate: 100,000 awake bodies cost about 11 ms a frame in the
   serial proxy path, paid per step today). "Later" otherwise.
2. World-space game UI and editor gizmos on drawn poses (`GameUIScene`, `SceneGizmos`). "Later".
3. Reflection probes, fog and post-process volumes, the sky and the sun on drawn poses. "Later".
4. Attachments placed outside their animator's subtree (`PlaceAttachments` through the parent's world,
   `AnimationSystem.cpp:859-865`) follow the simulated animator. "Later"; the documented usual case is under the animator.

Never cut: capture, the roles, render, lights, skins, camera, teleports, the switch, the script API and the camera fix (the
sample's camera is the owner's case).

## 5. Settings and defaults

| Setting | 0.7.0 | After the stream |
| --- | --- | --- |
| What games, Play and Simulate draw | Each entity's last fixed pose | Between the last two fixed poses, a constant step behind the game's clock |
| `Scene.Timing.Interpolation` (project) | n/a | Bool, default true |
| `GameRuntimeOptions::Interpolation`, `EditorContextSpecification::Interpolation` | n/a | false; the applications set them where time dilation is on (and, for `StrataRuntime`, where it renders) |
| A bare `Scene` | n/a | Off |
| Paused, stepped frames | Simulated pose | Simulated pose, exactly |
| Third-Person camera | Follows the simulated pose; arm snaps both ways | Follows the drawn pose; arm snaps in, lets out at 6/s |

Defaults that change (release notes, section 9): what games and Play draw (mean latency up half a step, worst case unchanged;
an interpolated non-step frame pays a step frame's proxy work); the sample's camera. Stored values never change.

## 6. Tests: suites and must-fail checks

A test helper `T/Scene/InterpolationTestUtils.h` (new): `FixedMoverSystem`, a scene system that in `OnFixedUpdate` moves listed
entities as physics' write-back does (`Translation += Velocity * step`, `Rotation = angleAxis(TurnRate * step, axis) *
Rotation`, one `Scene::MarkTransformsChanged` for all, no `on_update`), and `ScopedFixedMover`, which registers it in
`SceneSystemRegistry` for the scope as `ScopedCountingSystem` does (`SceneTests.cpp:108-120`). Steps of 1/64 s and frames that
are binary fractions of it keep step counts exact. Interpolation is in effect only in a running scene with the switch on:
every case below calls `scene.SetInterpolation(true)` (except where it checks the default) and `OnRuntimeStart()`, which also
creates every registered system of the test process (physics, scripts, animation, audio, UI), as the `Scene` suite's cases do.
The suite `Scene.Timing` is shared with S0.9's `T/Scene/SceneTimingTests.cpp`; this stream's cases go in their own file.

| Suite (file) | Step | Cases | Fails without the change |
| --- | --- | --- | --- |
| `Scene.Timing` (new: `T/Scene/SceneInterpolationTests.cpp`; Scene shard, `CoreShards.json` `Scene*`) | 1 | **The owner's case, core**: "An entity moved only by fixed steps is drawn moving every frame at 1000 frames per second": a mover at 6.5 m/s, steps 1/64 s, frames 1/1024 s, 1,000 frames; every frame's drawn increment along the motion is `6.5 * dt` within 0.1 mm, every one positive. Also with frame times drawn from a seeded uniform 0.5-1.5 ms: increments `6.5 * dt_i` within 0.1 mm. With interpolation off the same measure finds increments of 0 and 101.6 mm (asserted, so the measure is shown to see the owner's jitter) | Yes: 15 of 16 increments are 0 |
| `Scene.Timing` | 1 | "The alpha draws one step behind the clock": at 1, 4.17, 6.94, 8.33, 16.67, 33.3 ms frames and through a hitch of 200 ms, `GetStepAlpha` equals `1 - (1 - a/h)/n` within 1e-6 and the drawn time `T + a - h` (recovered from a linear mover) never decreases. `Scene` has no accessor for the accumulator: the test keeps a float32 copy of it, updated with the loop's operations in the loop's order (`Scene.cpp:2173-2215`, no time dilation), and takes `n` from `GetFixedStepCount()` before and after the stepping frame (float steps may fall due an update later, `specs/step0-platform.md` review item 1) | Yes |
| `Scene.Timing` | 1 | "Roots blend, descendants compose": a moved root with a child written every frame in `OnUpdate` (an attachment's pattern) and a grandchild: the child's drawn matrix is the root's drawn matrix times its current local, bit for bit, on every frame; the roles are Blend, Compose, Compose | Yes |
| `Scene.Timing` | 1 | "Per-frame writes": a translation write of a Blend entry makes it Exact (drawn at the written pose that frame and until the next step); a rotation-only write keeps the translation blending and draws the written rotation exactly; an ancestor's per-frame translation makes a Blend descendant Exact; a reparent makes a Compose entry Exact; a write to a Compose entry changes nothing but its local | Yes |
| `Scene.Timing` | 1 | "Entities created during the steps": a child spawned under a moved root in `OnFixedUpdate` is Compose in that frame; a root spawned there is Exact; neither is drawn at the identity | Yes |
| `Scene.Timing` | 1 | "Leaving the set": a mover that stops is drawn at its exact pose from the next stepping frame and appears in `GetWorldTransformChanges` then; destroyed entities are skipped; an entity reusing a destroyed one's index is not in the set | Yes |
| `Scene.Timing` | 1 | "Pause, step, resume, time scale": pausing draws exact poses at once; `Step` frames draw exact poses; after resuming the drawn pose never moves backward; at time scale 0 the alpha and drawn poses freeze; at 0.1 the increments are uniform | Yes |
| `Scene.Timing` | 1, 3 | "Breaks": `BreakInterpolation` outside the steps draws the entity and its Blend descendants where they are; inside `OnFixedUpdate` the steps' poses are drawn exactly in the frame that captures them; a 10 m jump in one step (no break) is drawn exactly (the speed net) | Yes |
| `Scene.Timing` | 1 | "Off by default": a bare scene's `GetDrawnTransform` equals `GetWorldTransform` bit for bit for every entity after frames with steps, the set stays empty, `GetStepAlpha` is 1, `FindDrawnTransform` is null | (guards D2) |
| `Scene.Timing` | 1 | "A fixed timestep change clears the set" | Yes |
| `Scene.Timing.Sweep` (new, Fuzz in Release) | 1 | "Random frames, edits, steps, pauses, spawns and breaks leave the simulation as without interpolation and every drawn transform consistent": 300 entities in hierarchies up to depth 3, a seeded mix per frame (frame lengths 0.2-40 ms, movers on a random subset, per-frame translations and turns, reparents, creation and destruction inside and outside the steps, breaks, pause, step, resume, time scale); a twin scene runs the same with interpolation off. Every frame: world transforms of the twins equal bit for bit; for every entity drawn equals world when not in the set or Exact, drawn equals parent's drawn times local for Compose, a Blend's drawn translation lies on its segment; `ValidateInterpolation` passes. `Tests::SelectSweepIterations` (2,000 frames in the shard, 20,000 in Fuzz) | Yes (any rule wrong) |
| `Physics.Interpolation` (new: `T/Physics/PhysicsInterpolationTests.cpp`, Systems shard by `Physics*`; the folder is left out of builds without physics, `StrataTests/CMakeLists.txt:76-77`, so no `#if`) | 1, 3 | **The owner's case, through physics**: "A character walking at 6.5 m/s is drawn within a pixel of its path at every frame rate a following camera sees": per frame rate a fresh scene with `CreateGround` (`T/Physics/PhysicsTestUtils.h:72`, 100 m square) and a character controller at (-20, 0, 0) (as `CreateCharacter`, `PhysicsCharacterTests.cpp:23-33`), `ICharacterControl::MoveCharacter({6.5, 0, 0})` every frame, steps 1/64 s, 0.5 s to settle, then 1.5 s measured (it ends near x = -7): a camera filter as `FollowCamera`'s (pivot toward drawn + 1.5 m with `1 - exp(-14 dt)`, arm 6.5 m, yaw 90, pitch -18) and the chest (drawn + 1 m) projected to 3840x2160 at 60 degrees; residual from its 100 ms centered average, p99 - p1, as `AUD/tools/analyze.py`'s `jitter`, at frames of 1/1024, 1/240, 1/144, 1/120, 1/64 and 1/32 s: at most 1 px. With interpolation off at 1/1024 s: at least 10 px (the measure sees the bug) | Yes (about 29 px) |
| `Physics.Interpolation` | 1 | **Simulation untouched**: "Interpolation changes no simulated pose": 64 dynamic boxes falling on a ground, a character walking, a body a mover pushes in `OnFixedUpdate`, 400 frames of seeded lengths 0.5-40 ms, two scenes run in lockstep with interpolation on and off: every `TransformComponent` and `WorldTransformComponent` byte-equal after every frame, the same `GetFixedStepCount` and `GetDroppedFixedStepCount` | (guards D1) |
| `Physics.Interpolation` | 3 | "Teleports draw no streak": `IBodyControl::Teleport` and `TeleportCharacter` called in `OnFixedUpdate` (a test system) and in `OnUpdate`: on the frames after, drawn equals world until the body moves again; a child of the teleported body follows | Yes |
| `Physics.Settings` (`T/Physics/PhysicsSettingsTests.cpp`, which already includes `box3d/box3d.h` and compares Strata's defaults with `b3DefaultWorldDef()`, 115-118) | 3 | Beside those comparisons: `CHECK(Scene::c_MaxInterpolatedSpeed == b3DefaultWorldDef().maximumLinearSpeed)` (`types.c:26`, at box3d's default length unit) | Yes (if either changes) |
| `Animation.Interpolation` (new block at the end of `T/Animation/AnimationSystemTests.cpp`; Scene shard), inside `#if ST_WITH_PHYSICS` as the controller case at 831-832 is | 1, 2 | "An attachment under an interpolated character stays on its joint on frames without a step": an animator moved by a character controller (as 832-...), an `AttachToJoint` child (as 593-...), 1 ms frames: the attachment's drawn world equals the animator's drawn world times the joint's pose within 1e-4 m every frame. Run again after `animation-graph` lands (plan 4.5) | Yes, by the local edit that blends Compose entries as Blend (10.8 cm at 6.5 m/s, plan 4.6) |
| `Renderer.RenderScene` (`T/Renderer/RenderSceneTests.cpp`) | 2 | "Interpolated proxies and lights are placed at their drawn transforms on frames without a step, exactly once they leave": mesh renderers and a point and a spot light on movers; after each `Sync` proxies' `GetWorldTransform` and light records match `GetDrawnTransform`; after the movers stop, the world matrices; a second `Sync` in a frame updates nothing (`LastSyncUpdates == 0`); `Validate` passes | Yes |
| `GPU.SceneRenderer.Interpolation` (new, appended to `T/Renderer/GPUSceneMotionTests.cpp`, reusing `MotionScene`, `CheckMotion`) | 2 | "An interpolated cube moves on frames without a fixed step, with its motion vectors, on both paths": cube 3 on a mover at 0.6 m a step, steps 1/64 s, frames 1/256 s, `MotionVectors`; on frames without a step `CheckMotion` with the cube's displacement 0.15 m and the camera still; CPU and GPU-driven paths. The same frames with `SetInterpolation(false)` show no motion on 3 of 4 frames (asserted in the case) | Yes (no motion on 3 of 4 frames) |
| `GPU.SceneRenderer.GPUDriven` (`T/Renderer/GPUSceneRendererGPUDrivenTests.cpp`, a case appended, reusing its `Comparison`) | 2 | "Both paths draw an interpolated frame alike": 16 cubes on a mover, a shadowed point light on another, the sun's cascades, a running interpolating scene, 8 frames of 1/512 s between steps of 1/64 s: `Comparison::Check` every frame (color within 1 level, entity IDs and velocities equal, as 131-160) | (guards both paths) |
| `GPU.SceneRenderer.Interpolation` | 2 | "Cached shadows follow interpolated casters": the same scene drawn each frame by a renderer with the caches (defaults, `FitShadowsToView = false`) and one with `CacheShadowCascades = false`, `CacheLocalShadows = false`, `SpreadLightingUpdates = false`: `MaxDifference(expected, image) <= 1` on every frame, as `GPUSceneShadowTests.cpp:446-451` compares them; returns early without `SupportsDrawIndirectFirstInstance` (the cascade cache is the GPU-driven path's, 421-422) | (guards the shadow caches) |
| `GPU.SceneRenderer.Interpolation` | 2 | "A skinned mesh is drawn at its animator's drawn pose": the posed arm (`MotionScene::Arm`, its own animator) on a mover: on frames without a step its pixels move by the drawn displacement | Yes (skinned back to the simulated pose without the `EvaluatePose` change) |
| `GPU.SceneRenderer.Interpolation` | 2 | "A camera under an interpolated body sees what rides with it still": a camera and a cube both children of a moved body: the cube's pixels stay put across frames without a step | Yes (`SceneCamera::FromEntity` on the world pose) |
| `GPU.SceneRenderer.ShadowFit` (S0.3's file) | 6 | S0.3's "A ball shot from the camera leaves the cascades alone" (`specs/step0.md` 7.5) again with the ball on a mover, the scene running and interpolating, frames at 1/240 and 1/1000 s | (S0.3's recheck, plan 4) |
| `Scripting.API` (`T/Scripting/ScriptAPITests.cpp`), API module `InterpolationProbe` (appended to `StrataTests/Scripts/API/TransformScripts.cpp`) | 5 | "Interpolation API": the probe moves its entity in `OnFixedUpdate`; in `OnLateUpdate` `Time::GetStepAlpha()` equals the scene's, `GetDrawnPosition`/`GetDrawnRotation` equal the scene's `GetDrawnTransform`, `ResetInterpolation()` makes them equal the world pose; with a truncated host table (`ScopedHost`, as `OlderEnginePhysics`, `PhysicsScripts.cpp:255-294`) and with null entries: alpha 1, drawn = world, reset false; a missing entity: false and a reported problem. `ScriptModuleTests.cpp:100-108`'s class list gains `InterpolationProbe` | Yes |
| `FeatureTest`, `Editor.FeatureTest`, `StrataEditor.FeatureTest`, `StrataRuntime.FeatureTest` | 5 | `TimeFeatures` calls `Time::GetStepAlpha()` in `OnLateUpdate` (a fraction in [0, 1]); a new `InterpolationFeatures` script on an entity "Interpolation Features" (a root) in `Feature.stscene` moves itself 0.1 m and 5 degrees per step in `OnFixedUpdate` and counts the steps of the frame; it records its world pose in `OnUpdate` (`m_BeforeSteps`); in `OnLateUpdate`, when steps ran in the frame, it sets `m_From = m_BeforeSteps` and `m_To` = its world pose (frames without a step keep both, which is what the scene blends between), and expects `GetDrawnPosition` within 1e-4 m of `mix(m_From, m_To, GetStepAlpha())` and `GetDrawnRotation` within 1e-3 of the normalized lerp along the shorter arc (4.1's formulas); before its first stepping frame, and whenever `GetStepAlpha()` is 1, it expects the world pose. It counts frames with alpha below 1 in a field `BetweenSteps`, and at frame 120 (after the hot reload at 100) calls `ResetInterpolation()` and expects the drawn pose to be the world pose. The in-process runners turn interpolation on and require `BetweenSteps > 0`: the headless run (`FeatureTests.cpp:229-...`, `SetInterpolation(true)` on the scene), the editor context and the game it exports (`EditorFeatureTests.cpp:39`, 157-160: `EditorContextSpecification::Interpolation`, `GameRuntimeOptions::Interpolation`). The two executables' CTests run with `--commands` and `--frames` (`StrataTests/CMakeLists.txt:720-733`), so they do not interpolate: the script's checks hold at alpha 1 and they require nothing of `BetweenSteps`. With S0.9's `FixedRate` "60" and frames of 1/60 s, one step runs per frame (alpha 0) until `TimeFeatures`' half-speed frame (21) leaves half a step in the accumulator (alpha 0.5 from then on). `PlayFeatureScene` (`FeatureTestUtils.cpp:400-406`) also requires `ValidateInterpolation` after every frame. `FeatureTest.stproj` keeps `Interpolation` true (S0.9's `FixedRate` makes the section non-default, `FeatureTests.cpp:132-150`) | Coverage of the three host functions and four SDK functions |
| `Editor.Commands` | 4 | `play.state`'s `running.interpolation` and `physics.stats`' `interpolation` with interpolation on (a context with the specification's switch) and off; `project.setSettings` turning `Interpolation` off while playing clears the set from the next frame, and its undo after play | The result keys |
| `Runtime.Game` | 4 | `GameRuntimeOptions::Interpolation` off by default (drawn = world); on with a manifest storing `Scene.Timing.Interpolation: false`: off; on otherwise | (guards D2) |
| `StrataEditor.ThirdPerson` (`StrataTests/Editor/ThirdPersonCommands.json.in`) | 5 | After the existing steps, the setup of 4.5 (player at (0, 0.02, -19), camera `Yaw` 90 and `Pitch` -18, 30 steps), then `input.sequence` holding `Gameplay/Move` `[1, 0]` and `Gameplay/Run` for 240 steps with `observe` of `FollowCamera.ArmLength` every step; `expect` on the samples around the step where the arm's ray clears the obstacle (K: the step where an unfixed run is back at 6.5, read once from the unfixed sample): the sample at K+1 is below 6.0 m and the one at K+60 at least 6.4 m (each step lets out `1 - exp(-6/60)`, 9.5% of what is left). The stepped run draws exactly (no interpolation in command files) | Yes (6.5 m at K+1) |

**Running scenes in the GPU suites.** Interpolation needs a running scene, and `OnRuntimeStart` creates the animation system,
which registers itself as the scene's `ISkinningPoses` (`AnimationSystem.cpp:80`), while `MotionScene` registers its own
`TestSkinningPoses` in its constructor (`GPUSceneMotionTests.cpp:55`): the system's registration would then fail with an error
log and the test's poses stay. The interpolation cases take the poses over after the start instead: `MotionScene` gains a
constructor flag `registerPoses` (default true), these cases pass false, run `ScopedFixedMover`, `SetInterpolation(true)`
and `OnRuntimeStart()`, then `if (ISkinningPoses* running = Contents.GetService<ISkinningPoses>())
Contents.UnregisterService<ISkinningPoses>(*running);` and `REQUIRE(Contents.RegisterService<ISkinningPoses>(Poses))`, and
unregister `Poses` before `OnRuntimeStop()` (the system's own unregistration in its destructor then finds nothing, which
`UnregisterServiceProvider` allows, `Scene.cpp:2106-2111`). No Animation header is needed, so the file stays outside the
animation-only list (`StrataTests/CMakeLists.txt:79-80`). Cases that compare renderers with the sun's fit on while something
comes nearer than the near bound pass `AdaptationSeconds` to both (S0.3's rule, `specs/step0.md` 7.5); the interpolation cases
keep their movers beyond it or set `FitShadowsToView = false`, as `CreateRenderer` does (`GPUSceneMotionTests.cpp:93-102`).

Every "fails without" is checked once and the result stated in the commit message: for cases that measure the bug with
interpolation off, in the same test; for cases of the new API (which would not compile on the parent commit), by a local edit
that disables the rule the case guards (Compose entries blended as Blend; `EvaluatePose` reading the animator's world matrix;
`SceneCamera::FromEntity` reading `GetWorldTransform`; the per-frame rules skipped; breaks ignored; `RenderLights` placing at
the world matrix), seen to fail and restored, as S0.3's regression check does (`specs/step0.md` 7.5). Suites keep their
prefixes' shards; if `Scene.Timing` takes the Scene shard past about 60 s in Debug, it gets a shard of its own in
`StrataTests/CoreShards.json`.

## 7. Acceptance numbers and how each is measured on this machine

| What | Target | How |
| --- | --- | --- |
| A1. The owner's jitter | At most 1 px p2p at 30, 60, 120, 144, 240 fps and uncapped (700-1000), 60 Hz steps; also uncapped at `FixedRate` 30, 120 and 240 | The audit's method on the real build: a copy of ThirdPerson (`project.openSample` into a temporary folder, a private session, `StrataCLI call --project`) with a recording script class added to the copy only, writing `AUD/data/run*.csv`'s columns with the robot's **drawn** position (`GetDrawnPosition`) in `rx, ry, rz` and the camera's world pose; `AUD/tools/run_case.sh`'s sequence (yaw 90, Move `[1, 0]` and Run held); frame rates held with S0.4's `ActiveFrameRate` where it has the value, else the recording class's spin (`AUD/tp/Scripts/FollowCamera.cpp:191-207`); analysed with `AUD/tools/analyze.py`'s `jitter(screen_series(rows))` on frames at full arm. A table of p2p and RMS per case in step 6's commit and the landing message |
| A2. Simulation untouched | Bit for bit | `Physics.Interpolation` "Interpolation changes no simulated pose" and `Scene.Timing.Sweep`'s twin; the in-process feature runs with interpolation on |
| A3. Latency | Mean about half a step more than 0.7.0, worst case unchanged | `AUD/tools/latency.py` on A1's recordings (simulated against drawn), with the 0.7.0 numbers from `AUD/data/run1.csv`: reported to the owner with step 0's numbers (decision 8) |
| A4. S0.3's rows with interpolation on (plan 1.1, 4) | 0 layout changes during the Playground's three shots at 60, 240 and 1000 fps, at most 2,400 changed pixels; layout changes in scripted motion at most 100, 80, 65 (Playground, Third-Person, Open World) | S0.3's own acceptance scripts (`specs/step0.md` 7.8: the shadow-jump audit's `exp3_variant.py`, `exp4_motion.py`, `exp5_sample.py` and `diff_masked.py`, adapted to read `editor.perf {frames: 1}`'s `gameRenderStats.shadowFit.changes`, kept in `.worktrees/wave8-plan/step0/render/`), run on this stream's Release editor in a private automation session, which interpolates (`EditorLayer.cpp:106`), each run once with the project's `Scene.Timing` `Interpolation` true and once false. Which parts exercise interpolation: the real-time shots of `exp3_variant.py` and the on-the-clock motion of `exp4_motion.py` (`input.key {seconds}`) and `exp5_sample.py` (`input.action` press and release); `exp3_variant.py`'s 16-frame burst runs through `input.sequence`, which pauses and steps (exact poses), so its pixel row repeats S0.3's number by construction and stands as the control. A row that holds off and fails on is a finding: it goes to S0.3's owner as a `ShadowFitTracker` change reviewed against S0.3's spec, never a workaround here |
| A5. Everything drawn agrees | No frame where a light, shadow, skinned mesh or attachment trails its body | The GPU suite (6), and a look at A1's run in the rebuilt editor (TAA, FXAA and no AA) |
| A6. The arm | No one-frame arm increase above 0.1 m on the return in A1's recordings | The largest one-frame increases of the `arm` column over A1's files, listed as `AUD/tools/latency.py` lists run 1's (its listing is hard-wired to `run1.csv`: point it at A1's files); the pull-in remains a cut by design (11, Q1) |
| A7. Cost | Section 8's targets | Section 8 |
| A8. Clean | 0 validation errors, Debug ctest and the feature test green, CI green on every job | As every stream |

## 8. Perf: metrics, targets and the stream's A/Bs

**New `Provisional` entries** (`StrataTests/Perf/Budgets.json`, appended):

```json
"Perf.Scene.StepFrame100k": { "Unit": "ms", "Provisional": true },
"Perf.Scene.InterpolatedStepFrame100k": { "Unit": "ms", "Provisional": true },
"Perf.Scene.InterpolatedStepFrame100k.Ratio": { "Unit": "ratio", "Provisional": true, "Target": 1.5 },
"Perf.Scene.InterpolatedFrame100k": { "Unit": "ms", "Provisional": true, "Target": 0.5 },
"PerfGPU.Renderer.StepFrame32k": { "Unit": "ms", "Provisional": true },
"PerfGPU.Renderer.InterpolatedFrame32k": { "Unit": "ms", "Provisional": true },
"PerfGPU.Renderer.InterpolatedFrame32k.StepRatio": { "Unit": "ratio", "Provisional": true, "Target": 1.15 },
```

- `Perf.Scene` (`T/Perf/ScenePerfTests.cpp`, a case "Interpolating 100,000 moving entities costs a capture per step and a
  blend per frame"): 100,000 root entities moved by `FixedMoverSystem` (each its own seeded velocity and turn), steps 1/64 s.
  `StepFrame100k`: the median `OnUpdateRuntime(1/64)` (one step a frame) with interpolation off; `InterpolatedStepFrame100k`
  the same with it on (capture and set every frame), its `.Ratio` to the former (target: the capture adds at most half of a
  step frame); `InterpolatedFrame100k`: frames of 1/1024 s without a step, the median of `OnUpdateRuntime` plus
  `UpdateDrawnTransforms()` (what a renderer's sync triggers): the blend of 100,000 entities on the job system (target 0.5 ms:
  about 25 ns each over the workers; the audit estimated 15 ns serial).
- `PerfGPU.Renderer` (`T/Perf/RendererPerfTests.cpp`, beside "Render cost of 100,000 ... cubes", a case "32,768 moving cubes
  cost a step frame's proxy work in every interpolated frame"): 32,768 cubes on a mover, default lighting with the sun's
  shadows, the harness' frame (scene update, `RenderScene::Sync`, render). `StepFrame32k`: a frame with a step, interpolation
  off (0.7.0's busiest frame); `InterpolatedFrame32k`: a frame without a step, interpolation on; `.StepRatio` the second over
  the first (target 1.15: proxy work as on a step frame, plus the blend). Also reported as information: the same frames'
  `Render.Extract` and `Scene.Transforms` stage times and GPU time.
- Existing entries that must not move with interpolation off (the switch costs a branch per frame, none per entity: 4.1's
  three instantiations of the walk): `Perf.Scene.IdlePlayFrame1M`,
  `MoveRoot100k`, `MoveLeaf100k`, `Perf.Physics.Boxes10k.ActiveFrame`, `PerfGPU.Renderer.Render100kCubes`.

**A/Bs** (information, never budgets; plan 4.1):

1. *On against off in one binary* (the cost of interpolation): the cases above measure both. With
   `STRATA_TEST_GPU_VALIDATION=0` in the environment (the PerfGPU CTest sets it, `StrataTests/CMakeLists.txt:282`;
   `PerfGPU.Harness` fails without it, DG 509), under `/c/msys64/mingw64/bin/python.exe Tools/PerfLock.py --label
   physics-timing -- build/agent-physics-timing/bin/Release/StrataTests.exe --test-suite=Perf.Scene,PerfGPU.Renderer
   --test-case=*nterpolat*`, three runs, each after deleting the build's `PerfResults/Release.json` and keeping a copy;
   medians and spreads in step 6's commit.
2. *Off against the kept base* (off costs nothing): `.worktrees/wave8-base` (76f28a21 Release, S0.0) against the stream's
   Release, three interleaved pairs (A B, B A, A B) under `PerfLock.py`, `--test-suite=Perf.Scene,Perf.Physics`, each run's
   `PerfResults/Release.json` copied away; compared metric by metric (median of three against median of three, with each
   side's spread) by a scratch script in the stream's private folder, never committed. Not `PerfStability.py` (plan 4.1).
   Step 0's own changes to the loop (S0.9's `GetMaxFixedSteps`) are within these cases too; a difference beyond the runs'
   spread is explained or fixed.
3. *In the editor* (what a person sees): a copy of the Physics Playground with 4,096 boxes dropped into the pit, Play in a
   private automation session, `editor.perf {frames: 600}` windows alternating `project.setSettings` `Scene.Timing`
   `{Interpolation: true}` and `false` (six windows, interleaved, under `PerfLock.py`): fps, `Scene.Transforms`,
   `Render.Extract` and `Scene.Physics` medians and p95, and the GPU busy time (moving casters redraw their shadow views every
   frame instead of every step). Expected from the audit: a non-step frame's `Render.Extract` rises from about 0.004 ms to
   about a step frame's 0.5 ms (the blend itself is `Scene.Transforms` nested inside it: stages record self time,
   `FrameProfiler.h:158`); frame rate at 4,096 boxes from about 1,025 to about 700 fps.

Readings beside running streams are ratios to a stage in the same frames, or information, never a pass (plan 4.1). The
combined case of section 7 of the plan (interpolation with 64 and 256 shadowed lights and 1,000 moving bodies) is landing 2's;
`local-lights` measures its main-thread metric with this stream merged (plan 4.7, step 8).

## 9. Docs, skills and release notes

- **DG**: a new `### Interpolation` (anchor `#interpolation`) at the end of `## Physics` (after "Leaving the simulation",
  2205-2222), and its line in the guide's contents. It states in full: the set, the three roles and why roots blend and
  descendants compose; the frame (flush, steps, capture, alpha, lazy drawn matrices); the per-frame write rules; breaks, the
  speed net, pause and step, time scale, rate changes, new and destroyed entities; who reads drawn transforms (2.4's table:
  what follows, what stays simulated and why); the switch and where it is on; the rule for renderer code (place what is
  drawn with `FindDrawnTransform` after `UpdateDrawnTransforms`, or `GetDrawnTransform`; never read the set); the rule for
  game code (follow moving bodies with `GetDrawnPosition` in `OnLateUpdate`; teleport with `Teleport`, `CharacterTeleport` or
  `ResetInterpolation`; move what others ride in `OnFixedUpdate`, risk 2a); memory (about 330 bytes per interpolated entity across the two sets and the drawn matrix); costs
  (section 8's measurements). Also: "Project settings" gets the field in S0.9's `Scene.Timing` paragraph with a link; "Render
  scene" (3650), "Motion vectors and TAA" (3836) and "Skinned meshes" (3966) a sentence each; "Gameplay API" (1629) a bullet
  for the three functions and the host function order (1712-1716); "Stepping a paused game" (3330) that paused and stepped
  frames draw exactly; "Simulated input" that `input.sequence` draws exactly; "Perf tests" the new metrics; the Architecture
  rules' scene-caches bullet a link.
- **Architecture.md**: the frame loop block (987-998) gains the flush, the capture and the alpha; "Scene caches" (1216) the set
  as a cache of the scene with its log of exits; "Render scene" (2176) the drawn walk; the time dilation paragraph (1032-1039)
  a sentence that the same owners interpolate.
- **AGENTS.md** (S0.1's room), one bullet in "Architecture rules" after "Scene caches and the transform contract", 248 code
  points with its line break and indent (counted by the review; within plan 4.1's 250):

  ```markdown
  - **Drawn transforms:** fixed-step motion is drawn between its last two poses (`Scene.Timing.Interpolation`):
    renderers read `Scene::FindDrawnTransform`, teleports call `BreakInterpolation` ([Interpolation](Docs/DeveloperGuide.md#interpolation)).
  ```
- **Manual**: `Docs/Manual/Settings.md`, S0.9's Timing bullet gains "**Interpolation** (on by default) draws moving bodies,
  characters and what scripts move in fixed updates between their last two physics steps, so they move smoothly at any frame
  rate. What you see is then always one step (about 17 ms at 60 steps per second) behind the game's clock instead of between
  none and one step: about 8 ms more on average, no more at worst. Turn it off only for a game that wants each physics step
  shown as it is." `Docs/Manual/Scripting.md`: a paragraph on following a moving body with `GetDrawnPosition` and on
  `ResetInterpolation`.
- **Skills**: `strata-scripting` (the three functions in the API overview; "cameras and effects that follow a body read its
  drawn pose in `OnLateUpdate`; teleport with `Teleport`, `CharacterTeleport` or `ResetInterpolation`"); `strata-make-a-game`
  (its third-person `FollowCamera` recipe reads `GetWorldPosition` of the target, line 242: `GetDrawnPosition`);
  `strata-editor-automation` (`play.state`'s and `physics.stats`' `interpolation`; captures while a game runs on the clock show
  drawn poses, paused and stepped captures exact ones).
- **Roadmap**: the wave 8 entry's line for this stream (S0.12 writes the entry); "Later": the cut items of 4.7 that were not
  built.
- **Release notes**, `Docs/Plans/Wave8/ReleaseNotes.md`, a block **physics-timing** under each of its headings ("What's
  new", "Defaults that change", "Upgrading from 0.7.0", "Known issues": `specs/step0.md` 4.5; the release writes
  `Docs/Releases/0.8.0.md` from it), filled with A1's and section 8's numbers. "Defaults that change" gets the row
  `| What games, Play and Simulate draw | each body's last fixed pose | between its last two fixed poses, a step behind the
  clock | physics-timing |`, as plan section 6 has it (plan.md:945). The lines:
  - "Games and the editor's Play and Simulate draw bodies, characters and what scripts move in fixed updates between their
    last two physics steps, so motion is smooth at any frame rate: the Third-Person robot running across the view moved by up
    to N px on a 4K screen at 700 fps and now by N px. The simulation is unchanged. What you see is a constant step behind the
    game's clock, about half a step (8 ms at 60 steps per second) more on average than before and no more at worst; Project
    Settings > Physics > Timing > Interpolation turns it off."
  - Known issues: "While a game runs on the clock, world-space game UI, editor gizmos, picking, reflection probes, fog and
    post-process volumes, and attachments placed outside their animator's hierarchy use the simulated poses, up to one step
    off what is drawn (exact while paused or stepped)", for whichever of 4.7's cut items were not built.
  - Upgrade row: "Scripts that place something from a moving body's position every frame (a follow camera, an effect) read
    `GetDrawnPosition()`/`GetDrawnRotation()` instead of the world position, or the body seems to jitter against them; scripts
    that move an entity far by setting its transform in `OnFixedUpdate` call `GetTransform().ResetInterpolation()`."
  - "New script API: `Time::GetStepAlpha`, `TransformComponent::GetDrawnPosition`, `GetDrawnRotation`, `ResetInterpolation`."
  - "The Third-Person camera follows the robot as it is drawn and lets its arm out smoothly after something passed between
    them."
  - "`play.state` and `physics.stats` report interpolation."

## 10. What the stream shares and how merges resolve

Landing 1 merges, in order, `reflections` R2, `physics-timing`, then `showcase` (plan 7.3); `physics-timing` lands before
`editor-ux` (`RenderScene.cpp`); `local-lights` branches after landing 1 and builds on step 2's `RenderLights` signatures.

| Shared | This stream's change | Resolution |
| --- | --- | --- |
| `S/Scene.h`, `S/Scene.cpp` with S0.9 | Interpolation API and members; hooks in `compute`; the loop's flush, capture and alpha; `SetPaused`, start, stop | S0.9 lands first: its `GetMaxFixedSteps()` and `GetFixedTimestep()` stay as they are, the capture goes around them |
| `S/ProjectSceneTiming.*` (S0.9) | The `Interpolation` field, `Timing::Interpolation`, `Find`'s parse and version, `ApplyProjectTiming`'s copy, `Copy`'s member | Additive (S0.9 lands alone before this stream branches, `specs/step0-platform.md` section 3) |
| `T/Scene/SceneTimingTests.cpp` (S0.9) | None: this stream's `Scene.Timing` cases go in `SceneInterpolationTests.cpp` | Nothing to merge |
| `T/Renderer/GPUSceneMotionTests.cpp`, `GPUSceneRendererGPUDrivenTests.cpp` | `MotionScene`'s `registerPoses` flag; a suite and a case appended | Additive; S0.3's rule for compared renderers (`AdaptationSeconds`) already merged |
| `T/Physics/PhysicsSettingsTests.cpp` | One check | Additive |
| `R/RenderScene.cpp` / `.h` (`editor-ux`) | `Sync`'s drawn walk, `GetDrawnTransform`, `EvaluatePose`, the skins' skip, `Validate`, clone version | This stream lands first; `editor-ux` merges main |
| `R/SceneRenderer.cpp` (`radiance-cascades`, `reflections`, `local-lights`) | `SceneCamera::FromEntity` (164) only | Separate hunks |
| `R/SceneRendererPost.cpp` | `CameraLens::FromEntity` (263, 268) only | Separate hunks |
| `R/TextRenderer.cpp` | `Prepare` (102, 165) | Nobody else |
| `P/PhysicsWorld.cpp` | Two calls in the teleports | No other wave 8 stream edits it |
| `SC/ScriptABI.h`, `ScriptHostAPI.cpp`, `SC/*` | Three functions appended; S0.9 edits `HostGetFixedDeltaTime` (325) | No other stream appends (`animation-graph`: no ABI change, plan 4.5) |
| `GameRuntime.h/.cpp`, `RuntimeApplication.cpp` (S0.8, S0.9) | One option, one member, one call, one line | Additive |
| `EditorContext.h/.cpp`, `EditorLayer.cpp` (S0.4, S0.7, `editor-ux`) | One option, two calls, one line | Additive |
| `CommandUtils.cpp`, `PlayCommands.cpp`, `EditorWorldCommands.cpp` | Result members and descriptions | Additive |
| `Samples/ThirdPerson/Scripts/FollowCamera.cpp`, `README.md`, `ThirdPersonCommands.json.in` (`animation-graph`: the robot's animator and its commands lines) | The camera; a commands block at the end | One command per line, keep all; `animation-graph` reruns the sample with this merged |
| `T/Animation/AnimationSystemTests.cpp` (`animation-graph`) | A suite block at the end | Kept as a block; `animation-graph` reruns it (plan 4.5) |
| `StrataTests/FeatureTest/*` (every stream) | A script, an entity, runner switches, `TimeFeatures` calls | Additive; rerun `StrataTests.FeatureTest` after merging |
| `StrataTests/Perf/Budgets.json` | Seven entries | Additive |
| DG, Architecture, Manual, AGENTS.md, Roadmap, skills | Section 9 | Additive; AGENTS.md's room is S0.1's |
| `showcase` | Its walk adopts a character controller once this lands (plan 4.2's table): a first-person camera under the controller is a Compose entry | The adoption pass after landing 1 |

No layer changes: Scene owns the set; Renderer, Physics and Scripting may include Scene (`Layers.json`); the editor and the
runtime read through `Scene`.

## 11. Risks and open questions, with the decisions recommended

1. **A teleport path that misses the signal draws a one-step streak.** *Mitigation:* breaks in both physics teleports, the
   per-frame translation rule (every teleport outside the steps), the speed net, `ResetInterpolation`, DG's rule for game code,
   the teleport and sweep tests.
2. **Script cameras that follow a body's world position now see it jitter against them** (the body is drawn up to a step
   behind its simulated pose). Every sample is covered (ThirdPerson fixed here; the Open World's camera is a child of the
   player; the Playground's fly camera and Tetris follow nothing); the release notes' upgrade row and the skills' recipes say
   what to read. *Decision:* no automatic fix (the engine cannot tell a follow from any other placement).
2a. **Riders of per-frame movers.** A character or body that stands on something a script moves every frame in `OnUpdate`
   (a platform placed with `SetWorldPosition`) is drawn a step behind its carrier, which is drawn where it is: at 3 m/s, a
   constant 5 cm at 60 steps per second (today the same gap flickers between 0 and 5 cm). No sample has such a carrier
   (grep of `Samples/*/Scripts` at 76f28a21: the per-frame writers are cameras, the coins' bob, the Open World's fly mode
   and its wanderers, `Wander.cpp:65`, which nothing rides). *Decision:* DG's rule for game code says to move what others ride in `OnFixedUpdate` (or as a kinematic body),
   so carrier and rider are drawn on the same clock; nothing in the engine guesses.
3. **Mean latency rises by half a step.** Camera look is unaffected (cameras placed every frame are not in the set). *Decision:*
   on by default (ruling 8); `FixedRate` 120 halves the added latency; the numbers go to the owner with step 0's report.
4. **Per-frame proxy work at high frame rates** (every frame pays a step frame's `Render.Extract`; 4,096 boxes from about
   1,025 to about 700 fps). *Mitigation:* section 8 measures it; the cut line's cap and parallel placement if a real scene
   needs them; sleeping bodies leave the set.
5. **Interpolation and many shadowed lights multiply** (plan 8): moving casters redraw local shadow views at the frame rate.
   *Mitigation:* `local-lights` measures with this merged; landing 2's combined case.
6. **The sun's cascades with casters moving every frame** (plan 8). *Mitigation:* S0.3 lands first; A4 repeats its rows; a
   miss goes to `ShadowFitTracker`, not here.
6a. **GPU time of moving casters above the step rate.** A caster that moves now redraws the cascades and local views it
   reaches on every frame, not on step frames only. A frame costs at most what a step frame costs today, so frame times at or
   below the step rate and the worst frame above it are unchanged at every preset (Medium keeps 0.6.0's costs per frame);
   only the average frame above the step rate rises toward the step frame's. Plan 1.1's GPU-busy rows measure still scene
   cameras and do not see it. *Mitigation:* A/B 3 reports GPU busy with interpolation on and off; `local-lights` and landing
   2 measure it with many lights (risk 5).
7. **Skinned meshes skinned back to the simulated pose** if `EvaluatePose` read the animator's world matrix. *Mitigation:* 4.2
   and its GPU test.
8. **Data races in the parallel capture and blend.** *Mitigation:* jobs read the registry's storages only, write their own
   entries and drawn matrices only; the old set is read-only during the fill; slot maps are written serially; the sweep and
   the feature runs check every frame.
9. **S0.9's names differ.** *Mitigation:* 2.5 now takes them from `specs/step0-platform.md` (`Timing`, `Find`,
   `ApplyProjectTiming`, `GetMaxFixedSteps`, `GetProjectFixedRate`); if the landed code differs, the first commit records the
   differences.
10. **World-space UI, gizmos and picking sit up to a step off while a game runs on the clock.** Paused frames agree exactly.
    *Decision:* cut line; "Later".
11. **Memory.** About 330 bytes per interpolated entity (two sets and the drawn matrices), 33 MB at 100,000 awake bodies.
    *Decision:* accepted (box3d's own storage for them is larger); recorded in DG.
12. **Tests through `GameRuntime` shift if the default leaks.** *Mitigation:* D2, `Runtime.Game`'s case.
13. **Q1: the arm's pull-in stays a cut.** The audit's pair of 131-141 px jumps becomes one cut where something comes between
    and a smooth return. Pulling in at a finite speed would show a thin post over the player for about 0.1 s, and the inside of
    a wall when the camera backs into one. *Recommended decision:* keep "in at once, out smoothly" (the standard collision arm
    and the README's promise); if the owner still sees the cut on the sheet, pull in at 30/s when the hit is a thin obstacle
    (a second ray 0.3 m to each side does not hit), as an update of the sample.
14. **Q2: interpolate in Simulate as well as Play?** *Recommended decision:* yes: both run on the wall clock and show the same
    judder; one switch.
15. **Q3: `Scene.Timing`'s version.** *Decided (review):* version 1 in every case. A simple section validates a missing
    member to the field's default at any version (`SettingsRegistry.cpp:364`), so values stored without `Interpolation` read as
    true; a version bump is for a change of a stored value's meaning, which this is not (4.4).
16. **Q4: extrapolate to cut the latency?** *Decision:* no: it overshoots at every stop, collision and turn, and the owner's
    complaint is judder, not latency.

## 12. Deliver in this order

1. **Step 1** (commits): `TransformInterpolation.h`, `HierarchyComponent::HasWorldTransform`, the capture pass and the set,
   the alpha, the per-frame rules, `UpdateDrawnTransforms`, `GetDrawnTransform`, `FindDrawnTransform`, `BreakInterpolation`,
   `ClearInterpolation`'s callers, `ValidateInterpolation` in the Debug check; `InterpolationTestUtils.h`, `Scene.Timing`'s
   mover cases with the owner's core case, `Scene.Timing.Sweep`.
2. **Step 2**: `RenderScene`, `RenderLights`, `EvaluatePose` and the skins' skip, `Validate`, the camera, the lens, text;
   `Renderer.RenderScene`'s case, `GPU.SceneRenderer.Interpolation`.
3. **Step 3**: the two teleport breaks, the speed check in `Physics.Settings`; `Physics.Interpolation` (the owner's case
   through a character, simulation untouched, teleports); `Animation.Interpolation`. Push `ci/wave8-physics-timing` once for
   the compilers and for the `Linux (no Audio, no Physics)` row, which builds the tests without `T/Physics/`.
4. **Step 4** (after merging main with S0.9): the field, `ApplyProjectTiming`, the programs' options, the commands' members;
   `Editor.Commands`, `Runtime.Game`, the feature runners' switches. Rebuild a private editor and look at the Third-Person in
   Play at uncapped and capped rates.
5. **Step 5**: ABI, host functions, SDK, `InterpolationProbe` and `Scripting.API`, `InterpolationFeatures` and
   `TimeFeatures`, `FollowCamera` and its commands check, README, skills' recipes.
6. **Step 6**: perf cases and entries, the A/Bs, A1-A6's runs and tables, DG, Architecture, Manual, AGENTS.md bullet,
   Roadmap, release-note lines.
7. **Finish**: merge main; an independent review; Debug ctest with every suite (GPU suites under validation), Release ctest
   without perf, `CompatLint.py`; CI green on `ci/wave8-physics-timing`; the landing message with A1's table, the latency and
   the A/Bs.

If time runs short, stop after a complete step: steps 1-2 already smooth the drawn image of every running game the program
interpolates (tests turn it on); steps 3-5 make it safe, switchable and reachable; step 6 measures and documents.

## 13. The first hour

1. Read `specs/decisions.md`, `plan.md` 4.1, 4.6 and 7, `AUD/audit.md`, this spec, AGENTS.md, DG "Architecture rules",
   "Scene caches" in Architecture.md (1216) and its frame loop (937-1040), then the code of 2.1: `Scene::OnUpdateRuntime` and
   `UpdateWorldTransforms`, `RenderScene::Sync`, `EvaluatePose`, `RenderLights`, `SceneCamera::FromEntity`, the prototype
   `AUD/tp/Scripts/FollowCamera.cpp:119-151`.
2. Confirm what step 0 landed: lane Render on main (S0.3's tests, `ShadowFitChanges` in `editor.perf`, the adapted scripts in
   `.worktrees/wave8-plan/step0/render/`), S0.9's names against 2.5 (`specs/step0-platform.md` 4.1-4.3; or that lane
   Platform is still out: steps 1-3 do not need it), AGENTS.md's free room. Note differences for the first commit message.
3. Create `.worktrees/physics-timing` from main with `.worktrees/create_worktree.sh` (submodules from the main checkout's
   local copies, `specs/step0.md` 2.1), configure `build/agent-physics-timing`, build Debug and Release; run
   `StrataTests.exe --test-suite=Scene*,Physics*,Renderer.RenderScene,GPU.SceneRenderer.MotionVectors` in Debug as the
   baseline.
4. Write `InterpolationTestUtils.h` and the owner's core case of `Scene.Timing` first, with its interpolation-off half that
   shows the 0 and 101.6 mm increments, before any engine change; then the types, the capture and the alpha until it passes.
5. Keep `.worktrees/wave8-base`'s Release build (S0.0) for section 8's second A/B; measure nothing until a step's tests pass.

## Review corrections

The independent review (2026-11-02) checked this spec against main at 76f28a21 (every file of 2.1: line counts, and the
code paths of `Scene::OnUpdateRuntime`, `UpdateWorldTransforms`, `GetWorldTransform`, `Copy`, `RenderScene::Sync`,
`EvaluatePose`, `UpdateSkinnedPoses`, `Validate`, `CopyIntoScene`, `RenderLights`, `SceneCamera::FromEntity`,
`CameraLens::FromEntity`, `TextRenderer::Prepare`, the physics teleports and write-back, `AnimationSystem`'s root motion and
attachments, the host table and SDK, `GameRuntime`, `EditorContext`, the commands, the feature runners and the CTests),
box3d's `types.c`, the plan, `decisions.md`, the audit and its data, and the specs written beside it (`step0.md`,
`step0-platform.md`, `step0-editor.md`, `reflections.md`, `radiance-cascades.md`, with their own review corrections). The
design held: capture after the steps, roots blend and descendants compose, exact when in doubt, the alpha
`1 - (1 - a/h)/n` (its monotonicity holds through dilation and dropped backlogs), the renderer reading one drawn transform,
the skinning fix (`RenderScene.cpp:1271-1280` really would skin the robot back to its simulated pose), the append-only ABI,
and D1: the pre-step flush changes which bodies `SyncEntitiesToBodies` skips through `FindCurrentWorldTransform`
(`PhysicsWorld.cpp:3469`) and which it syncs one by one, but `SyncBody` does nothing to a body whose transform equals its
`LastWorldTransform` (3344-3356), so the simulation is the same; the test proves it. Nearly every file:line of section 2 held.
Changed, and why (these win over the body where they differ):

**Steps an agent could not implement as written**

1. **Major: S0.9's names were guesses; `specs/step0-platform.md` now exists** (2.5, 4.4, 10, risk 9). The section's value
   is `ProjectSceneTiming::Timing { FixedRate, Version }`, not a `Table`; `ApplyProjectTiming()` returns whether the rate
   changed. 2.5 now cites S0.9's API and call sites; 4.4 says how `Timing` gains `Interpolation`, how `Find` parses it
   (`JsonUtils::GetBool`) and that its `Version` changes with either field.
2. **Major: a play copy would have ignored a project that turned interpolation off** (4.1 Copies, 4.4). S0.9's `Copy` copies
   the project timing version, so `ApplyProjectTiming` on the copy returns at once; the spec left `m_ProjectInterpolation` to
   "programs" that never set it. `Copy` now copies it.
3. **Major: the physics-dependent cases would not build on CI's `Linux (no Audio, no Physics)` row** (6). They were in
   `T/Scene/` without a guard. They move to `T/Physics/PhysicsInterpolationTests.cpp` (suite `Physics.Interpolation`, left
   out by folder, `StrataTests/CMakeLists.txt:76-77`); `Animation.Interpolation` goes inside `#if ST_WITH_PHYSICS` as its
   model at `AnimationSystemTests.cpp:831` does; the box3d speed check moves to `Physics.Settings`, whose file already
   includes box3d (`PhysicsSettingsTests.cpp:17`, 115-118), where `PhysicsSimulationTests.cpp` does not.
4. **Major: the GPU cases needed a running scene, which takes `ISkinningPoses` from `MotionScene`'s test poses** (6). The
   animation system registers itself at construction (`AnimationSystem.cpp:80`) and `MotionScene` registers its poses first
   (`GPUSceneMotionTests.cpp:55`), so the skinned case would run with an error log and the system's registration refused. The
   spec now says how these cases start the scene and take the service over without including Animation headers.
5. **Major: the feature test's expectations were wrong on frames without a step and in two runners** (6). "Drawn = blend of
   the pose recorded in `OnUpdate` and the current pose" fails on any frame without a step (the scene then blends between the
   last stepping frame's poses); the script now keeps that frame's `From` and `To`. `StrataEditor.FeatureTest` and
   `StrataRuntime.FeatureTest` run the executables with `--commands` and `--frames` (`StrataTests/CMakeLists.txt:720-733`),
   which never interpolate (D2), so only the three in-process runs require `BetweenSteps > 0`.
6. **Minor: the alpha case read an accumulator `Scene` does not expose** (6). It now mirrors the accumulator in float32 and
   counts steps through `GetFixedStepCount` (S0.9's review found float steps falling due an update late).
7. **Minor: "fails without the change" could not be run against the parent commit for new-API cases** (6). They are now
   checked by a named local edit that disables the guarded rule, as S0.3's regression check is.
8. **Minor: the ThirdPerson commands check depended on finding an obstacle by trial** (4.5, 6). The block now starts from a
   fixed position taken from the audit's run 1 (the arm was cut at z = -21.6 to -22.3), with a rule for moving it.
9. **Minor: `RenderLights`' private `UpdateRecord`/`ComputeRecord` also need the scene** (4.2); `SceneCamera::FromEntity` must
   use the on-demand `GetDrawnTransform` because its callers run before the frame's sync (`GameRenderer.cpp:127-128`,
   `ViewportRenderer.cpp:32-33`); the capture's passes 3 and 4 looked slots up while the new arrays were not yet swapped in,
   so the swap moves to the end of pass 2 (4.1).

**Rules of AGENTS.md and the plan**

10. **Major: A/B 2 used `Tools/PerfStability.py`**, which plan 4.1 forbids in streams (section 8). It now compares three
    interleaved pairs' saved results with a scratch script. A/B 1 now sets `STRATA_TEST_GPU_VALIDATION=0`, without which
    `PerfGPU.Harness` fails (`StrataTests/CMakeLists.txt:282`, DG 509).
11. **Minor: off was not free per entity.** Hooks inside `compute` would put a branch on the hot walk of every scene; the walk
    is now instantiated per case so that a scene that does not interpolate runs today's loop (4.1), which A/B 2 checks.
12. **Minor: a frozen alpha was not free.** `m_DrawnStale` was set by any recomputed entity, so the Third-Person camera's
    every-frame write in `OnLateUpdate` would recompute all drawn matrices and walk every proxy at time scale 0. Staleness now
    follows only recomputed entities that are in the set (4.1).
13. **Minor: the latency was described wrongly in user-facing texts** (4.1's API comment, the ABI comment, the field's
    description, the Manual, the release note): drawing is a constant step behind the game's clock, not "one step behind the
    simulation" (the drawn pose trails the latest simulated pose by `h - a`, half a step on average; audit, "Latency").
14. **Minor: release notes had no file or headings**; they now go into `Docs/Plans/Wave8/ReleaseNotes.md` under step 0's
    headings, with the "Defaults that change" row of plan section 6 and a "Known issues" line for the cut items (9).

**Wrong claims, corrected**

15. AGENTS.md is 39,292 code points (39,308 bytes); step 0 leaves about 2,330 free for nine streams, not 3,000; the bullet is
    248 code points, not 246 (2.5, 9). `c_ParallelTransformWidth` is at `Scene.cpp:30`. The samples' command-file CTests are
    at `StrataTests/CMakeLists.txt:797-...`, not 388-456 (smoke tests). S0.3's GPU case is "A ball shot from the camera leaves
    the cascades alone" and its layout count already exists (`SceneRendererStats::ShadowFitChanges`, reported by S0.3 as
    `editor.perf`'s `gameRenderStats.shadowFit.changes`), so the author's question about A4's count is closed (2.5, 7).
16. **A4 would not have measured interpolation everywhere** (7): `exp3_variant.py`'s burst runs through `input.sequence`,
    which pauses and steps, so its pixel row is a control by construction; the on-the-clock parts are named. A4 now uses
    S0.3's adapted scripts rather than the audit's originals.
17. **Q3 is decided, not open** (4.4, 11): a simple section validates missing members to their defaults at any version
    (`SettingsRegistry.cpp:364`), so `Scene.Timing` stays version 1 whatever ships.
18. **"Both paths' images identical" and "cached and uncached identical" had no defined comparison** (6). The paths' case now
    lives in `GPU.SceneRenderer.GPUDriven` with its `Comparison::Check` (color within 1 level, IDs and velocities equal); the
    caches' case compares as `GPUSceneShadowTests.cpp:446-451` does (`MaxDifference <= 1`) and skips devices without the
    GPU-driven path, which owns the cascade cache.

**Added**

19. `GetDrawnTransform` on an entity whose write is still pending now applies the rule the next update will (a turn keeps the
    translation blending), instead of returning the world pose, which would have made a script camera jump for a frame (4.1);
    `FindInterpolationSlot` compares the whole handle; a failed decomposition draws exact.
20. Risks 2a (riders of per-frame movers, with DG's rule for game code) and 6a (GPU time of moving casters above the step rate:
    no frame costs more than a step frame does today, so Medium keeps 0.6.0's costs per frame).

Kept after checking: interpolation in `physics.stats` and `DescribeRunningScene` (three numbers; `play.state` is outside the
MCP `core` toolset, `McpToolsets.cpp:27`, and descriptions there are cut at 160 characters, 13); DG's section at the end of
`## Physics` (DG:1983); `HasWorldTransform` fits `HierarchyComponent`'s padding (40 bytes either way) and survives `Copy`,
which copies the nodes whole (`Scene.cpp:315`); the arm's pull-in staying a cut (Q1, for the orchestrator).
