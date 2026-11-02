# Step 0 of wave 8, lane Editor: placing assets with a ghost (S0.7), the editor's frame rate (S0.4), the Performance pause (S0.5)

Written on 2026-11-01 from `plan.md` section 3 ("Lane Editor"), `specs/decisions.md`, `owner_feedback.md` (items 10, 12 and 13)
and the `editor-ux` audit (`.worktrees/wave8-plan/editor-ux/audit.md`, its scripts `ray_cost.py`, `fps_probe.py`, `cli.py`).
Every file:line below is main at 76f28a21 (release 0.7.0) unless it says otherwise. The audit built and prototyped nothing:
its designs come from reading, its timings are rough (other auditors ran beside it), and this spec checked them against the
code where it could. Claims not checked say "unverified".

Paths: `E/` is `StrataEditor/src/Editor/`, `P/` is `StrataEditor/src/Panels/`, `U/` is `StrataEditor/src/UI/`, `C/` is
`Strata/src/Strata/Core/`, `R/` is `Strata/src/Strata/Renderer/`, `T/` is `StrataTests/src/`, `DG` is `Docs/DeveloperGuide.md`,
`Arch` is `Docs/Architecture.md`, `plan` is `.worktrees/wave8-plan/plan.md`.

Rules for the whole lane (AGENTS.md, plan 4.1, decisions, and this machine):

- Worktree `G:/GameEngineAI/.worktrees/wave8-editor`, branch `agent/wave8-editor` from main once S0.0 and S0.1 have landed,
  build tree `build/agent-wave8-editor` (`cmake --preset windows -B build/agent-wave8-editor`), CI branch `ci/wave8-editor`.
  Never the owner's `build/windows`; at most four build-heavy agents on the machine.
- Every commit: Debug build with warnings as errors, `ctest --test-dir build/agent-wave8-editor -C Debug -j 8` with every `GPU*`
  suite under the validation layer (the lane adds one GPU case), `python Tools/CompatLint.py --changed-since 76f28a21` clean.
  Release `ctest -C Release -LE perf` before each push to `ci/wave8-editor`.
- Perf only under `python Tools/PerfLock.py --label wave8-editor -- ...`, interleaved against the kept 76f28a21 Release build of
  S0.0 (`.worktrees/wave8-base`), as information; new metrics enter `StrataTests/Perf/Budgets.json` as `Provisional`, no
  budget changes, no `PerfStability.py` (decisions D4).
- Editors started for checks: private session directories (`STRATA_SESSION_DIR`), `StrataCLI call --project` always, scripts in
  files (never Python on stdin). The editor writes the person's preferences to `%LOCALAPPDATA%\Strata\Preferences.json`
  (`Platform::FindUserDataDirectory` through `SHGetKnownFolderPath`, `Strata/src/Platform/Windows/WindowsPlatform.cpp:221-235`;
  no environment variable redirects it): no check in this lane runs `preferences.set` against a real editor. Preferences are
  tested in-process (`EditorPreferences` with an empty file keeps them in memory, `E/EditorPreferences.h:43-48`).
- No code from other folders of this computer; open source per D10. Nothing in this lane needs code from outside the
  repository.

## 1. The owner's request and the lane's goal

The owner (2026-11-01, verbatim):

- **Item 10:** "Have drag and drop from the content browser to scene with preview ghost at the ray-cast of the mouse pointer into
  the scene when the ray hit an entity that make sense. (should be easy to do)"
- **Item 12:** "Expose the editor idle frame rate limit thing. Have reasonable choices exposed. Like the timeout length (with 0
  for unlimited), and options. (easy to do)"
- **Item 13:** "Make a button in performance view to pause either the editor render view or pause the scrolling frame breakdown,
  otherwise I can't hover my mouse on a frame breakdown and see what is composed of it in terms of time cost because it is
  literally scrolling at a thousand frames a second. (easy to do)"

The plan puts the three in step 0's lane Editor, in this order (plan 3, "Lane Editor"): S0.7 first, landing by itself, because the
showcase's graybox waits for it (an agent places the showcase's props on terrain through `asset.place`), then S0.4 and S0.5.
The lane's goals, in order of priority:

1. **Agents can place props where a ray lands (S0.7, commands).** `scene.raycast` and `asset.place`, so the showcase agent puts
   rocks and barrels on generated terrain in one call each instead of `prefab.instantiate` plus `entity.dropToGround`.
2. **People see where a dragged asset will land (S0.7, ghost).** A box ghost on a surface that takes the asset, a quieter one
   where a guess stands in, a tint on the mesh a material would go to, a label where nothing can be dropped; the drop runs
   `asset.place`, so a person and an agent get the same result, and hovering changes nothing.
3. **The idle limit is the person's (S0.4).** An `Editor.FrameRate` preference section whose defaults keep 0.7.0's behavior,
   `IdleAfterSeconds` 0 meaning "never idle", and an optional cap while active, with `editor.status` and `editor.perf` saying
   what is in effect.
4. **The frame breakdown holds still (S0.5).** Pause (or Space) freezes the Performance panel's timeline, tooltip, summary and
   table on a copy of the profiler's ring that has its GPU times, while the profiler, `editor.perf`, `--perf-log` and the status
   bar stay live.

Not the goal: the mesh ghost, the live material preview, HDR textures to the sky, a per-mesh ray BVH, hover and stepping aids in
the paused timeline, pause on spike, the precise frame wait (all `editor-ux`, tranche 3: plan 4.8), pausing the viewport (plan 9,
"Pausing the viewport").

What the lane does not touch: shaders (the ghost is debug lines), quality rows, the shadow caches, the render scene's data and
both draw paths (the ghost and the tint are overlays over the finished image), the script ABI, scene, prefab and asset formats.
Its one stored format is a new preference section, `Editor.FrameRate` version 1, in the existing preferences file.

## 2. Where we start

### 2.1 Files

| File | Lines | Role in this lane |
| --- | --- | --- |
| `P/ViewportPanel.cpp` / `.h` | 1,117 / 154 | The Scene view. `OnImGuiRender` 164-340: `AcceptAssetDrops` called right after the image item (214), the view resolved by a lambda (220-235) first at 254 and finally at 282, rendered at 294-295, overlays 336-337. `AcceptAssetDrops` 1053-1115 |
| `E/EditorPlacement.h` / `.cpp` | 61 / 162 | `FindPivotSurface` (72-80), `CastAt` (82-86, editor camera), `FindSurfaceAt` (88-93, falls back to the pivot at 92), `RestOn` (95-105), `GetMeshBounds` (107-116), `GetTemplateBounds` (118-147, `LoadAssetSync` at 125), `GetPlacementBounds` (149-158) |
| `E/SceneRaycast.h` / `.cpp` | 40 / 175 | `Cast` (99-171): the render scene's spatial index, then every LOD 0 triangle of each candidate mesh (`CastMesh` 51-92); uses `AssetManager::GetActive()` (102) and `AssetManager::GetAsset<Mesh>` (155) |
| `E/AssetBounds.h` / `.cpp` | 44 / 69 | `Measure` (16-67): caches only complete results (64-65), so a pending template is instantiated again at every call |
| `E/EditorPrefabCommands.cpp` | 690 | `prefab.instantiate` (314-380): `LoadAssetSync` at 340; the single-instance body 346-379 |
| `E/Commands/EntityCommands.cpp` | 758 | `entity.create` (170-194), `entity.dropToGround` (554-607): the patterns asset.place follows |
| `E/EditorCommands.h` / `.cpp` | 218 / 420 | `EditorCommand` (87-106: `Kind`, `Results`, `WaitsForAssets`, `MayDefer`), `MayDefer()` (109-113), registration functions (196-216); the registry's constructor (`.cpp` 41-63) |
| `E/CommandUtils.h` | 220 | Schemas (45-55), `ResolveAsset` (67), `DescribeBounds` (75), `ReadVec3` (77), `ApplyComponents` (88), `FinishEdit`/`RollBackEdit` (200, 202); functions defined in other files say where (e.g. 90-106) |
| `E/EditorAssetCommands.cpp` | 1,241 | `asset.info`'s deferral while bounds are pending (672-755; `c_MaxBoundsFrames` 300 at 37): the model of a command that waits for `AssetBounds` |
| `E/EditorViewport.h` / `.cpp` | 278 / 599 | `ViewportSettings` (87-144: `Snap` 112, `TranslateSnap` 114, `SelectionColor` 118, `PilotGameCamera` 131), `GetAspectRatio` (198), `c_DefaultWidth/Height` (178-179), `ResetState` (255) |
| `E/ViewportRenderer.h` / `.cpp` | 185 / 304 | `ResolveViewportView` (`.h` 60, `.cpp` 23-51); `Render` (`.h` 127-128, `.cpp` 162-258): `m_DebugDraw.Clear()` 206, streaming tints 244-245, debug shapes 251-252 |
| `R/DebugDraw.h` | 75 | `Box` (39, 41), `Circle` (42), `Arrow` (47), depth modes `Tested`/`OnTop` (15-19); lines are blended over the image (`R/SceneRenderer.cpp:3870-3875`, line pipelines 3898-3907, tested lines drawn at 3975-3976) |
| `R/SceneRenderer.h` | | `SceneEntityTint` (313-318); `EntityTints` keep an entity's first color (464-466) |
| `U/EditorActions.cpp` / `.h` | 331 / 98 | `RunEditorCommand` (32-59) asserts on pending results (44-50); `RunEditorCommandDeferred` (61-69); `InstantiatePrefab` (286-305) |
| `U/PropertyWidgets.h` | | `DragDrop::c_Asset` (18) |
| `P/ContentBrowserPanel.cpp` | 1,312 | `SetAssetPayload` (90-94): the drag source, an `AssetHandle` payload with the asset's name as its tooltip |
| `StrataCLI/src/CLI/McpToolsets.cpp` | 185 | Core list (17-29, 27 commands), exact names (32-40), prefixes (42-53), toolsets (55-68) |
| `StrataEditor/src/EditorLayer.cpp` / `.h` | 2,474 / 383 | Theme colors for views (`.cpp` 416-451, `c_DebugTintAlpha` 92); `editor.status`'s editor section (466-490, `frameRate` 484-489); the preferences' version applied (616-620); `UpdateSteppingFrameRate` call (635); `OnImGuiRender` (711-778, `UpdateFrameRate` at 777); throttling (825-887). `.h`: the throttling comment (117-121), constants (125-128), `m_CapBeforeSteps` (372) |
| `E/EditorInterface.h` / `.cpp` | 28 / 76 | The model of a simple user section and its reader (`MakeSettingsSection`, `GetScaleFactor`) |
| `E/EditorPreferences.h` / `.cpp` | 111 / 333 | `RegisterSections` (`.h` 41, `.cpp` 68-72) |
| `E/EditorPreferencesCommands.cpp` | 186 | `preferences.set` (121-156, Session) |
| `E/EditorStateCommands.cpp` | 1,094 | `DescribeFrameProfile` (149-177); `editor.perf` (912-) |
| `C/Timer.h` | 103 | `FramePacer` (49-101): `std::this_thread::sleep_until` (94), "Precision is that of the operating system's sleep" (52-53) |
| `C/FrameProfiler.h` / `.cpp` | 363 / 542 | `FrameSample` (94-109), `FrameProfileSummary` (129-145), capacity 600 (149), `c_MaxGPULatencyFrames` 8 (172), `FrameRecord` (259-273); `ToSample` (`.cpp` 328-343), `GetFrame` (345-350), `Summarize` (398-501) |
| `P/PerformancePanel.cpp` / `.h` | 497 / 103 | `OnImGuiRender` (132-167, refresh 137-143), `DrawSummary` (169-197: the right-aligned width 184-188, Copy as JSON 190-196), `DrawTimelineStrip` (236-390: hover 330-389, the mouse mapped to an age at 333), `UpdateTableRows` (392-444) |
| `T/Editor/PlacementTests.cpp` | 356 | `Editor.Placement` (74-208), `Editor.UI.Placement` (210-356): the drop test (235-309), the material drop (311-355) |
| `T/Editor/EditorUITests.cpp` | 1,872 | Throttling cases (594-670, 672-681) and the automation, loading and script-build cases after 821 |
| `T/Editor/EditorObservatoryUITests.cpp` | 940 | The Performance panel's cases (582-765) |
| `T/Core/FrameProfilerTests.cpp`, `T/Core/CoreTests.cpp` | 823, 346 | `Core.FrameProfiler`; the FramePacer cases (176-230) |
| `T/Editor/EditorPanelPerfTests.cpp` | 279 | `Perf.EditorPanels.Performance600FramesP95` (182-199) |
| `T/Editor/EditorMcpToolsetsTests.cpp` | 136 | `CLI.McpToolsets`: at most 32 default tools and 30,720 bytes (68-70), the settings toolset's commands (101-105) |
| `T/Editor/CommandJournalTests.cpp` | | `Editor.Journal`'s "Every built-in command declares the kind its effect calls for..." (209-239): every command must be in exactly one of `c_EditCommands` (102-), `c_QueryCommands` (121-), `c_SessionCommands` (145-), and in `c_DeferringCommands` exactly when `MayDefer` (230), so both new commands join these lists or the suite fails |
| `T/Editor/EditorValidationTests.cpp` | | The sweep over every command (111-136): an unknown parameter and each missing required one are refused and change nothing; it covers the new commands without edits |
| `Docs/Architecture.md` | | 3932-3946: `EditorPlacement`'s description names `FindSurfaceAt` and `CastAt`, which this lane removes |
| `T/Editor/GPUEditorViewportTests.cpp` | 2,691 | `GPU.Editor.Viewport` (277-): `ViewportGPUHarness` (157-) |
| `T/Editor/HarnessEditor.h`, `T/Editor/ImGuiHarness.h` | 189, | The editor on the ImGui harness (`Frames(n, seconds)` advances the host's clock) and its fake host (`ImGuiHarness.h:43-77`: `MaxFrameRate`, `FrameRateChanges` counting calls that changed the rate, `WindowFocused`) |

### 2.2 Facts each step depends on (checked at 76f28a21)

- **The drop runs at once and blocks.** `AcceptAssetDrops` instantiates through `EditorActions::InstantiatePrefab`
  (`P/ViewportPanel.cpp:1085-1093`), which runs `prefab.instantiate` with `RunEditorCommand` (`U/EditorActions.cpp:300`): a
  command that may not defer (`:44-50`), whose handler loads the template with `LoadAssetSync` (`E/EditorPrefabCommands.cpp:340`).
  Its bounds come from `GetPlacementBounds`, which falls back to `GetTemplateBounds` and its own `LoadAssetSync`
  (`E/EditorPlacement.cpp:125`, `:157`).
- **The drop ignores the pointer on a miss and the view's camera with Game camera.** `FindSurfaceAt` falls back to the pivot
  (`E/EditorPlacement.cpp:92`); the drop casts through the editor camera (`P/ViewportPanel.cpp:1080`), and while piloting the
  scene's camera it places under the pivot (1085-1088) and a material drop does nothing (1098).
- **Unsupported drops only log.** `ST_WARN` at `P/ViewportPanel.cpp:1106` and `:1111`: a person dragging does not see the
  Console. Mesh sub-assets are refused today (1111).
- **Hovering measures every frame.** While a prefab or model hovers, `AssetBounds::Measure` runs each frame
  (`P/ViewportPanel.cpp:1059-1068`); a pending result is not cached (`E/AssetBounds.cpp:64-65`). A template that is not
  resident yet returns at once without instantiating (`:39-42`); a resident template whose meshes or skeletons are still
  loading is instantiated into a private scene every frame (`:45-56`), which is the cost to remove. Meshes count as pending
  only while `Loading` (`E/SceneBounds.cpp:81-89`, waiting skeletons included), so a failed mesh never holds a measurement
  pending.
- **Rays need no physics and no GPU.** `SceneRaycast::Cast` works on the render scene's spatial index and the meshes'
  CPU positions (`E/SceneRaycast.cpp:99-171`); it runs headless. Cost (audit, rough, best of 7, vertical rays): 0.004 ms per ray
  at 200 triangles, 0.097 at 20,000, 0.95 at 199,712, 12.3 at 2,000,000; 100,000 other mesh renderers add about 0.003 ms per ray.
- **The scene's asset manager is the active one.** `Scene::GetAssetManager()` returns `AssetManager::GetActive().get()`
  (`Strata/src/Strata/Scene/Scene.cpp:2091-2094`); `AssetManagerBase` derives from `std::enable_shared_from_this`
  (`Strata/src/Strata/Asset/AssetManager.h:299`), so a `Ref` for `RenderScene::Sync` (`R/RenderScene.h:155`) comes from
  `shared_from_this()`. The renderer itself syncs with the active manager (`R/SceneRenderer.cpp:2847-2854`).
- **Loads publish a version.** `AssetManagerBase::GetContentVersion()` changes whenever an asset object is published, replaced
  or dropped (`Strata/src/Strata/Asset/AssetManager.h:497-501`); requests are never cancelled for lack of renewal, only by
  `CancelLoad` (`:358-361`), and eviction takes the least recently requested resident assets (`:290-294`).
- **Debug shapes are the box ghost's medium.** `DebugDraw` lines are drawn over the finished image after TAA, blended by
  their alpha, depth-tested or on top (`R/DebugDraw.h:15-19, 29-31`; `R/SceneRenderer.cpp:3870-3875, 3953-3976`); the Scene
  view's renderer fills one per frame (`E/ViewportRenderer.cpp:206, 251-252`) and the capture renderer is another
  `ViewportRenderer` (`E/EditorViewport.h:206`), so a ghost given only to the Scene view's `Render` never reaches captures.
  No shader is needed, so this lane never meets lane Slang.
- **Entity tints keep the first color.** `SceneRenderOptions::EntityTints` keeps an entity's first color
  (`R/SceneRenderer.h:464-466`, stable sort at `R/SceneRenderer.cpp:3681-3691`); the view passes the debug view's tints
  (`E/ViewportRenderer.cpp:244-245`).
- **ImGui 1.92.9** (`Strata/vendor/imgui/imgui.h:32`): `AcceptDragDropPayload` takes `AcceptBeforeDelivery`,
  `AcceptNoDrawDefaultRect` and `AcceptNoPreviewTooltip` (`imgui.h:1583-1585`); `IsDragDropActive` and `ClearDragDrop` are in
  `imgui_internal.h` (3952; `imgui.cpp:15725-15739`). A drag makes its source item own every keyboard key
  (`SetActiveIdUsingAllKeyboardKeys`, `imgui.cpp:15820`), but `ImGui::IsKeyPressed(key, repeat)` asks with
  `ImGuiKeyOwner_Any` (`imgui.cpp:10570-10573`), so Escape is seen. While the source item stays active and the button stays
  down, `BeginDragDropSource` starts the drag again (`imgui.cpp:15768-15821`): a cancel must also clear the active id.
  Space activates the navigated item only while the navigation cursor shows (`g.NavCursorVisible`, `imgui.cpp:14735-14738`,
  asked with `ImGuiKeyOwner_NoOwner`); a mouse click hides that cursor, an arrow or Tab press shows it.
- **Commands that wait run through the runner.** `EditorCommandRunner::Run` calls the completion at once for a command that
  finishes at once (`E/EditorCommandRunner.h:43-46`); panels have the runner in `EditorPanelContext::Runner`
  (`U/EditorPanelRegistry.h:41`). Edit commands wait for the asset scan themselves (`E/EditorCommands.h:127-128`). `editor.batch`
  refuses `MayDefer` commands (`:103-105`).
- **MCP's default list is full.** 27 core commands and 5 generic tools: 32 tools (`StrataCLI/src/CLI/McpToolsets.cpp:17-29`;
  `T/Editor/EditorMcpToolsetsTests.cpp:68-70`; DG 5348-5350). By prefix, `scene.raycast` falls in `scene` and `asset.place` in
  `assets` (`McpToolsets.cpp:42-45`).
- **New top-level entities join a world's cells by transaction.** `EditorWorld` gives the entities a `SceneEditTransaction`
  created to the edit layer's cell at their position (`E/EditorWorld.cpp:323-343`; read, not run, as the audit says).
- **Idle throttling.** `UpdateFrameRate` returns early when throttling is off (`StrataEditor/src/EditorLayer.cpp:877-881`) and
  otherwise sets 30, 10 or 0 (882-886). `UpdateSteppingFrameRate` lifts the cap to 0 while steps are pending and gives back the
  cap it found (860-873), in `OnUpdate` (635), before `UpdateFrameRate` runs at the end of `OnImGuiRender` (777). `IsBusy`
  (843-858) counts input in the last 0.5 s, an automation request in the last 1.0 s, animating panels, a running or stepping
  game, loading assets, pending commands and requests, a script build and a pending pick. Headless, `--frames` and
  command-script runs never throttle (825-829). The preferences' version is applied once per change (616-620), never parsed per
  frame.
- **`--idle-timeout` means something else:** close a headless editor after so long without an automation client
  (`StrataEditor/src/EditorApplication.cpp:77`).
- **FramePacer** sleeps with `sleep_until` and cannot wake on input (`C/Timer.h:94`); the editor waits in it after every frame
  (`C/Application.cpp:259`). Nothing in the engine raises the Windows timer resolution (no `timeBeginPeriod` or high-resolution
  waitable timer outside `Strata/vendor/box3d/samples`). Its precision at 120 frames per second and above is unmeasured.
- **The editor is not paced by FIFO here.** With `VSync = true` (FIFO) the audit saw 190-720 frames per second on a 120 Hz
  display while playing, Present p50 0.10-0.26 ms (audit, "What the measurements say"). Why is S0.6's question (lane Platform).
- **The ring is under a second at those rates.** 600 records (`C/FrameProfiler.h:149`), each with two arrays of 64 floats
  (`:267, 272`): about 552 bytes, 331 KB in all (computed). GPU times arrive 2-3 frames late, at most 8 (`:172`).
- **The panel's hover follows the newest frame.** `DrawTimelineStrip` puts the newest frame at the right edge and maps the
  mouse's x to an age (`P/PerformancePanel.cpp:256-257, 333`): a still mouse is over another frame in every frame.

## 3. Order, landings and the cut line

1. **S0.7a: the placement core and the commands** (sections 4.1-4.5): `EditorPlacement::Place`, the `SceneRaycast` and
   `AssetBounds` fixes, `ResolveSceneView`, `scene.raycast`, `asset.place`, the toolset entry.
2. **S0.7b: the drop and the box ghost** (sections 4.6-4.9): `PlacementPreview`, the panel's drag path through `asset.place`,
   the ghost through `DebugDraw`, labels, Escape.
3. **S0.4: the frame rate preferences** (section 5), the FramePacer measurement first, because it decides which active caps
   ship.
4. **S0.5: the Performance pause** (section 6).

**Landings.** S0.7 lands on main by itself (review, Debug ctest with the GPU suites under the validation layer, Release ctest
without perf, CompatLint, `ci/wave8-editor` green), ahead of the lane's rest, because the showcase's graybox waits for it (plan
3, "Landing step 0"). If S0.7a is verified while S0.7b is still in review, S0.7a lands first: the graybox needs the commands,
not the ghost. S0.4 and S0.5 then land together through the same CI branch. `build/windows` is rebuilt by the orchestrator after
each landing (the owner looks at the editor there).

**Cut line.** Never cut: `scene.raycast`; `asset.place` for prefabs and models with all three location forms; the drop
through `asset.place`; the fallbacks and the quieter ghost; the box ghost and its labels; Escape; "hover writes nothing"; the
`Editor.FrameRate` section with its three idle fields and both traps; `editor.status`'s and `editor.perf`'s additions; the
pause with its snapshot. Cut first to last if the lane runs long: (1) `asset.place`'s `rotation` and `scale`; (2) the material
target's tint (its outline and label stay); (3) mesh sub-asset drops; (4) `ActiveFrameRate` with its exceptions (the second
"S" of S0.4; the owner asked for the idle limit). Everything the audit listed beyond these is `editor-ux`'s (plan 4.8).

## 4. S0.7 Ray, placement and the box ghost

### 4.1 Placement core: `E/EditorPlacement.{h,cpp}`

One function decides where an asset lands for a ray, a pixel or a point, and both the ghost and `asset.place` call it with the
same request, so the ghost is the drop by construction.

Added to `namespace EditorPlacement` (header comments abbreviated here; write them as the file's style does):

```cpp
// Where an asset placed along a ray landed (Place).
enum class PlacementSource : uint8_t
{
	Surface = 0, // A surface of the scene the ray hit (SceneRaycast::Cast): a mesh renderer's triangles, or its bounds while it loads
	Grid,        // Nothing hit: where the ray meets the grid plane y = 0, within c_GridFallbackDistance of its origin
	InFront,     // Neither: on the ray at the request's fallback distance
	Point        // The point the request gave (asset.place {point})
};
const char* PlacementSourceToString(PlacementSource source); // "surface", "grid", "inFront", "point"

// How far along a ray that hits nothing the grid plane still counts (the visible grid fades well before).
constexpr float c_GridFallbackDistance = 500.0f;
// The in-front distance through a scene's camera, and the bounds of any in-front distance.
constexpr float c_DefaultFallbackDistance = 10.0f;
constexpr float c_MinFallbackDistance = 0.5f;
// Snapping finds the height at the snapped point (still at the hit's height) with a ray straight down from this far above
// it, this deep.
constexpr float c_SnapLift = 1.0f;
constexpr float c_SnapDepth = 1000.0f;

struct PlacementRequest
{
	// The point form (asset.place {point, normal}): the asset rests on it, no ray is cast. Otherwise CastRay is used.
	std::optional<Surface> At;
	Ray CastRay;
	float MaxDistance = std::numeric_limits<float>::max(); // How far the ray looks for a surface
	float FallbackDistance = c_DefaultFallbackDistance;    // InFront's distance (clamped to [c_MinFallbackDistance, c_GridFallbackDistance])
	bool RequireSurface = false;                           // No Grid or InFront: a miss places nothing
	std::optional<float> Snap;                             // The grid step in meters while snapping
	glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // Over the asset's own (world space)
	float Scale = 1.0f;                                    // Uniform, over the asset's own
};

struct Placement
{
	PlacementSource Source = PlacementSource::Surface;
	Surface RestingOn;      // The point and normal it rests on (InFront: the point on the ray, normal up)
	Entity HitEntity;       // The entity hit (Surface only)
	AABB PlacedBounds;      // The asset's bounds around its origin, rotated and scaled as placed (invalid: unknown)
	glm::vec3 Translation = glm::vec3(0.0f); // Of its root, in world space
	bool Snapped = false;
};

// A ray from a view's camera through a point of its image in normalized device coordinates ([-1, 1], +Y up), computed
// camera-relative (the rotation-only view matrix, then the camera's position added) so it stays exact far from the origin.
Ray MakeViewRay(const SceneCamera& camera, const glm::vec2& ndc);
// The in-front distance of a view: the editor camera's distance to its target (null: a scene's camera, c_DefaultFallbackDistance),
// clamped to [c_MinFallbackDistance, c_GridFallbackDistance].
float GetFallbackDistance(const EditorCamera* editorCamera);
// The request of a ray placement (asset.place {ray} and the drag's preview alike): Ray(origin, direction) (which normalizes
// the direction), MaxDistance, FallbackDistance clamped as Place clamps it, Snap. The preview passes its pointer ray's
// stored Origin and Direction, the command the same floats read back from its parameters (float to JSON double to float is
// exact), so both build bit-identical requests and the drop lands exactly where the ghost stood.
PlacementRequest MakeRayRequest(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, float fallbackDistance,
	std::optional<float> snap);
// Where an asset with `bounds` (around its origin, unrotated: AssetBounds' result) lands for the request; nullopt only when
// RequireSurface and the ray hits nothing. Main thread (it casts through SceneRaycast).
std::optional<Placement> Place(Scene& scene, const PlacementRequest& request, const AABB& bounds);
```

`Place`, in this order:

1. **Bounds as placed.** `PlacedBounds = bounds.IsValid() ? bounds.Transformed(glm::mat4_cast(request.Rotation) * glm::scale(glm::mat4(1.0f), glm::vec3(request.Scale))) : AABB()`.
2. **Where.** With `At`: `Source = Point`, `RestingOn = *At` (normal normalized, `+Y` when zero). Otherwise `hit =
   SceneRaycast::Cast(scene, CastRay, MaxDistance)`: a hit gives `Surface` (`RestingOn = {hit->Point, hit->Normal, true}`,
   `HitEntity`). A miss with `RequireSurface` returns nullopt. Else the grid: when `|CastRay.Direction.y| > 1e-6` and `t =
   -CastRay.Origin.y / CastRay.Direction.y` lies in `(0, min(MaxDistance, c_GridFallbackDistance)]`, `Source = Grid`,
   `RestingOn = {CastRay.GetPoint(t), Direction.y < 0 ? +Y : -Y, false}`. Else `Source = InFront`, `RestingOn =
   {CastRay.GetPoint(d), +Y, false}` with `d = min(max(FallbackDistance, c_MinFallbackDistance), min(MaxDistance,
   c_GridFallbackDistance))` (never `std::clamp` with bounds that may cross: a `MaxDistance` under 0.5 m wins).
3. **Snapping** (with `Snap`; `step = *Snap`): the dominant axis of `RestingOn.Normal` (the largest absolute component; `Y` for
   `Grid` and `InFront`) keeps its value, the two others are rounded to multiples of `step` (`std::round(v / step) * step`), as
   the gizmo snaps translations to `TranslateSnap`. Then, for `Surface` whose dominant axis is `Y` with `Normal.y > 0`, the height
   is found again: `SceneRaycast::Cast(scene, Ray(snapped + (0, c_SnapLift, 0), -Y), c_SnapLift + c_SnapDepth)`; a hit replaces
   `RestingOn` (and `HitEntity`), a miss keeps the snapped point at the original height. `Snapped = true`.
4. **Translation.** `InFront`: the bounds' center on the point (`Translation = RestingOn.Point - PlacedBounds.GetCenter()`, or
   the point itself without bounds): nothing to rest on, so the ghost straddles the pointer. Every other source:
   `Translation = RestOn(RestingOn, PlacedBounds)` (`E/EditorPlacement.cpp:95-105`, unchanged).

For a drop with no rotation, scale or snap that hits a surface, this is 0.7.0's drop (`FindSurfaceAt` then `RestOn` with the
measured bounds), so `Editor.UI.Placement`'s drop test (`T/Editor/PlacementTests.cpp:235-309`) keeps its positions; its one
call of `CastAt` (262) moves to `MakeViewRay` (test 10).

`MakeViewRay`: `const glm::mat4 rotation(glm::mat3(camera.View)); const Ray relative = Ray::FromNDC(ndc,
glm::inverse(camera.Projection * rotation)); return Ray(camera.Position + relative.Origin, relative.Direction);` (`SceneCamera::View`
is the full view matrix: `R/SceneRenderer.cpp:167`, `E/EditorCamera.cpp:276`; `Ray::FromNDC` handles reversed-Z perspective and
orthographic projections, `Strata/src/Strata/Math/Ray.h:26-28`).

Removed: `FindSurfaceAt` (its only caller is the drop) and `CastAt` (the drop and the drop test at
`T/Editor/PlacementTests.cpp:262`, which moves to `MakeViewRay`; the rays case 76-111 never called it).
`FindPivotSurface`, `RestOn`, `GetMeshBounds`, `GetTemplateBounds` and `GetPlacementBounds` stay for the create menus and Add to
Scene (plan: "`FindPivotSurface` stays for Add to Scene"); Add to Scene keeps its `LoadAssetSync` (not this lane's).

### 4.2 `SceneRaycast` and `AssetBounds`

- **`E/SceneRaycast.cpp`** reaches the asset manager through the scene: `AssetManagerBase* assets = scene.GetAssetManager();`
  syncs with `renderScene.Sync(assets ? assets->shared_from_this() : nullptr)` (102) and reads meshes with
  `assets->GetAsset<Mesh>(meshHandle)` (155; null without a manager: the bounds stand in, as today). No behavior changes (the
  scene's manager is the active one today), so no test changes; the header comment (`E/SceneRaycast.h:27-29`) names the scene's
  manager.
- **`E/AssetBounds.{h,cpp}`: pending results are kept too.** `CacheEntry` gains `bool Pending`, `uint64_t ContentVersion` (the
  manager's `GetContentVersion()` when measured), `std::vector<AssetHandle> Waiting` (what the measurement waited for: the
  `pending` list `SceneBounds::GetSceneBounds` filled, `E/AssetBounds.cpp:54-55`, which holds loading meshes and waiting
  skeletons) and `std::chrono::steady_clock::time_point MeasuredAt`; `AssetBounds` gains a member `std::vector<AssetHandle>
  m_Changes` whose capacity is reused. `Measure` answers a pending entry of the same content stamp from the cache, without
  instantiating anything or counting a measurement, while all of these hold: less than `c_PendingRemeasureSeconds` (1.0 s, a
  safety net for waits that publish nothing; unverified that any exist) passed; and either the content version is unchanged,
  or `GetContentChanges(entry.ContentVersion, m_Changes)` (after `m_Changes.clear()`: it appends) returns true and lists neither
  the asset nor any handle of `Waiting`
  (`Strata/src/Strata/Asset/AssetManager.h:501-505`). Otherwise it measures again, as today, and stores the result either way.
  So unrelated assets publishing every frame during streaming (the content version alone would move each frame) never
  re-instantiate the template; only the arrival of something it waits for does. The header comment says so ("measures a
  pending asset again once something it waits for was published, or after a second"). `GetMeasureCount` keeps counting real
  measurements only. `Clear` is unchanged. An entry's `Waiting` allocates only when it is measured, and `m_Changes` reaches its
  working capacity within a few frames, so answering from the cache allocates nothing in steady state.
- **Residency.** A cached pending answer requests nothing; the preview and `asset.place`'s wait request the template itself
  every frame they run (`RequestLoad`), so what the drop needs stays the most recently requested and is not evicted (AGENTS.md,
  "request what you use every frame").

### 4.3 The Scene view's view, in the core: `ResolveSceneView`

`E/ViewportRenderer.h` gains, beside `ResolveViewportView` (60):

```cpp
// The Scene view's 3D view at `aspectRatio`: through the scene's primary camera with Game camera on
// (ViewportSettings::PilotGameCamera; EditorOverlays and EditorView set), else the editor camera (with a notice when Game
// camera finds no primary camera). What the Scene view draws, and what scene.raycast and asset.place {viewport} cast through.
std::optional<ViewportView> ResolveSceneView(EditorContext& context, float aspectRatio);
```

Its body is the panel's lambda (`P/ViewportPanel.cpp:220-235`) moved verbatim; the panel calls it at 254 and 282. The 2D canvas
mode keeps its own view (237-251).

### 4.4 Commands: `E/Commands/PlacementCommands.cpp` (new)

`void RegisterPlacementCommands(EditorCommandRegistry& registry); // scene.raycast, asset.place (Commands/PlacementCommands.cpp)`
is declared in `E/EditorCommands.h` after `RegisterBatchCommands` and called last in the registry's constructor
(`E/EditorCommands.cpp:63`). Re-run configure after adding the file (sources are globbed).

Two schemas shared by both commands, in the file's anonymous namespace:

- `ViewportPointSchema(description)`: `{"type": "object", "description": ..., "properties": {"x": number (pixels from the image's
  left edge, continuous: the middle of the first column is 0.5), "y": number (from the top edge), "width": integer 1-16384 (the
  image size the pixel refers to; default: the Scene view's size, or 1280 while it has none), "height": integer 1-16384 (default:
  the Scene view's, or 720)}, "required": ["x", "y"]}`. The handler refuses unknown members, `x` outside `[0, width]`, `y`
  outside `[0, height]`. NDC: `(x / width * 2 - 1, 1 - y / height * 2)` (the panel's own mapping, `P/ViewportPanel.cpp:1077`).
  The camera: `ResolveSceneView(context, width / height)`; the 2D canvas mode is ignored (its view is not a 3D one). Defaults
  1280x720 are `EditorViewport::c_DefaultWidth/Height` (`E/EditorViewport.h:178-179`), the size `viewport.capture` takes.
- `RaySchema(description)`: `{"type": "object", "properties": {"origin": Vec3Schema, "direction": Vec3Schema (not
  normalized, never zero)}, "required": ["origin", "direction"]}`.

Positive numbers are `NumberSchema` with `"exclusiveMinimum": 0` (and `"maximum"` where named). Every refusal below is
`InvalidParameters` and changes nothing.

#### `scene.raycast` (Query, results Content, does not wait for assets)

Description (its first sentence is what MCP shows, at most 160 characters): "What a ray, or a pixel of the Scene view, hits among what
the active scene draws: its mesh renderers, on their triangles (their bounds while a mesh loads). Give origin and direction, or
viewport {x, y, width?, height?} (a pixel of the Scene view's image, through its camera: the editor camera, or the scene's
primary camera with Game camera on). maxDistance limits the ray (default: unlimited for origin and direction, the view's far
plane for a pixel); ignore leaves entities out with their descendants. Returns {hit: true, entity, name, point, normal (facing
the ray's origin), distance, onTriangle (false: a loading mesh's bounds were hit), ray: {origin, direction}} or {hit: false, ray};
ray is what asset.place {ray} takes. Colliders, gizmos and UI are not hit. While playing it casts into the running game."

Parameters: `origin` (Vec3), `direction` (Vec3), `viewport` (ViewportPointSchema), `maxDistance` (number > 0), `ignore`
(EntityArraySchema). Exactly one of `origin`+`direction` (both) and `viewport`; `direction` of length at most 1e-6 or not finite
is refused; an unknown entity in `ignore` is refused (`CommandArguments::GetEntities`). The handler: the ray; `SceneRaycast::Cast(
*context.GetActiveScene(), ray, maxDistance, ignore)`; the answer, vectors as arrays of three numbers, ids as 16-digit hex
strings, `name` the entity's name. No undo step, no scene change.

#### `asset.place` (Edit, `MayDefer`, results Content)

Description (its first sentence is 146 characters; with "(one undo step)" inside it, it was 162 and MCP would cut it): "Places a
prefab, model or mesh where a ray, a Scene view pixel or a point lands, resting on the surface there, as a drop into the Scene
view does. One undo step. Exactly one of: ray {origin, direction}; viewport {x, y, width?, height?} (a pixel
of the Scene view's image through its camera, as scene.raycast); point [x, y, z] with normal (default [0, 1, 0]), e.g. a
scene.raycast hit. A ray or pixel that hits nothing lands on the grid plane y = 0 within 500 m, else on the ray at
fallbackDistance (default 10 m; for a pixel the editor camera's distance to its target), unless requireSurface: then it fails.
snap (meters) puts the point on that grid along the surface and finds the height there; rotation ([x, y, z] degrees) and scale
(uniform) apply over the asset's own. A mesh becomes an entity with a Mesh Renderer, named after it. While the prefab or model
loads it waits (at most 30 s), then up to 5 s for what its size needs (after that it places by the size known so far:
bounds.pending true). Returns {id (the root), entities (every root), placedOn: surface | grid | inFront | point, hitEntity (null
unless surface), point, normal, translation, bounds {min, max, size, center, pending}, snapped}; while playing it changes the
running copy only."

Parameters:

| Name | Schema | Rule |
| --- | --- | --- |
| `asset` | `AssetSchema` (required) | A Prefab, Model or Mesh; anything else is refused: "'X' is a Material: asset.place places prefabs, models and meshes (component.set gives a mesh renderer a material)" |
| `ray` | `RaySchema` | One of three |
| `viewport` | `ViewportPointSchema` | One of three |
| `point` | `Vec3Schema` | One of three |
| `normal` | `Vec3Schema` | With `point` only; zero-length refused |
| `parent` | `OptionalEntitySchema` | The roots go under it, keeping the world transform `Place` chose |
| `rotation` | `Vec3Schema` | Euler degrees, as the Inspector shows them (`Math::EulerDegreesToQuat`) |
| `scale` | number, exclusiveMinimum 0, maximum 1000 | Uniform factor |
| `snap` | number, exclusiveMinimum 0, maximum 10000 (`ViewportSettings::c_MaxTranslateSnap`) | Meters |
| `maxDistance` | number > 0 | With `ray` or `viewport`; default unlimited (`ray`) or the view's far plane (`viewport`) |
| `fallbackDistance` | number, minimum 0.5, maximum 500 | With `ray` only (a pixel takes the view's) |
| `requireSurface` | `BoolSchema` | Default false |

#### The schemas exactly

Both commands' parameter schemas as the helpers of `E/CommandUtils.h:45-55` build them (`Vec3Schema` gives an array of three
numbers, `EntityArraySchema` at least one id, `OptionalEntitySchema` a string or null: `E/CommandUtils.cpp:51-89`). Property
descriptions start with a sentence that says what the value is: MCP keeps only that first sentence (`McpToolsets.cpp:87-108`).

```cpp
namespace
{
	constexpr int64_t c_MaxViewportPixels = 16384;

	nlohmann::json PositiveNumberSchema(std::string description, std::optional<double> maximum = std::nullopt)
	{
		nlohmann::json schema = NumberSchema(std::move(description));
		schema["exclusiveMinimum"] = 0;
		if (maximum)
			schema["maximum"] = *maximum;
		return schema;
	}

	nlohmann::json ViewportPointSchema(std::string description)
	{
		nlohmann::json schema = ObjectSchema({
			{ "x", NumberSchema("Pixels from the image's left edge, continuous: the middle of the first column is 0.5") },
			{ "y", NumberSchema("Pixels from the image's top edge, continuous") },
			{ "width", IntegerSchema("The width of the image the pixel refers to (default: the Scene view's, or 1280 while it has none)", 1, c_MaxViewportPixels) },
			{ "height", IntegerSchema("The height of that image (default: the Scene view's, or 720)", 1, c_MaxViewportPixels) } }, { "x", "y" });
		schema["description"] = std::move(description);
		return schema;
	}

	nlohmann::json RaySchema(std::string description)
	{
		nlohmann::json schema = ObjectSchema({
			{ "origin", Vec3Schema("Where the ray starts, in world space") },
			{ "direction", Vec3Schema("Where it points; any length but zero") } }, { "origin", "direction" });
		schema["description"] = std::move(description);
		return schema;
	}
}

// scene.raycast
ObjectSchema({
	{ "origin", Vec3Schema("Where the ray starts, in world space (with direction)") },
	{ "direction", Vec3Schema("Where the ray points, any length but zero (with origin)") },
	{ "viewport", ViewportPointSchema("A pixel of the Scene view's image, cast through its camera (instead of origin and direction)") },
	{ "maxDistance", PositiveNumberSchema("How far along the ray to look, in meters (default: unlimited for a ray, the view's far plane for a pixel)") },
	{ "ignore", EntityArraySchema("Entities left out, with their descendants") } })

// asset.place
ObjectSchema({
	{ "asset", AssetSchema("The prefab, model or mesh to place") },
	{ "ray", RaySchema("Place where this ray lands (one of ray, viewport, point)") },
	{ "viewport", ViewportPointSchema("Place where this pixel of the Scene view lands, through its camera (one of ray, viewport, point)") },
	{ "point", Vec3Schema("Place resting on this point, e.g. a scene.raycast hit (one of ray, viewport, point)") },
	{ "normal", Vec3Schema("With point: the surface's normal there (default [0, 1, 0])") },
	{ "parent", OptionalEntitySchema("Parent of the placed roots; null or omitted for the top level") },
	{ "rotation", Vec3Schema("Euler angles in degrees over the asset's own rotation (default [0, 0, 0])") },
	{ "scale", PositiveNumberSchema("Uniform scale over the asset's own (default 1)", 1000.0) },
	{ "snap", PositiveNumberSchema("Grid step in meters: the point snaps to it along the surface, and the height is found there", ViewportSettings::c_MaxTranslateSnap) },
	{ "maxDistance", PositiveNumberSchema("With ray or viewport: how far the ray looks for a surface, in meters (default: unlimited, or the view's far plane)") },
	{ "fallbackDistance", /* NumberSchema with minimum 0.5 and maximum 500 */ NumberSchema("With ray: where on the ray an asset lands when nothing is hit and the grid is beyond 500 m (default 10)") },
	{ "requireSurface", BoolSchema("Fail instead of falling back to the grid or the ray when nothing is hit (default false)") } }, { "asset" })
```

`fallbackDistance`'s schema adds `"minimum": 0.5` and `"maximum": 500` (`EditorPlacement::c_MinFallbackDistance`,
`c_GridFallbackDistance`). The handlers check what a schema cannot say: exactly one location form, `normal` only with `point`,
`fallbackDistance` only with `ray`, `maxDistance` not with `point`, no unknown members inside `ray` and `viewport`, finite
numbers, non-zero directions and normals, `x` and `y` within the image.

Handler, in this order:

1. **Arguments.** As listed; the asset through `ResolveAsset(context, arguments, "asset", AssetType::None)` and its type checked.
   `parent` through `FindEntity`. `CheckSceneEditable` before anything waits: a scene being opened fails at once ("The scene is
   being opened: nothing was placed"), as `FinishEdit` would at the end.
2. **The request.** `PlacementRequest` from the form: `ray` through `EditorPlacement::MakeRayRequest(origin, direction,
   maxDistance, fallbackDistance, snap)` (the preview's function, 4.1); `viewport` through `ResolveSceneView` and `MakeViewRay`,
   `MaxDistance` the view camera's `Far`, `FallbackDistance` `GetFallbackDistance(view.FromScene ? nullptr :
   &context.GetViewport().GetCamera())`; `point` as `At`. The ray is resolved now, so a deferred placement lands where the call
   asked, whatever the camera does meanwhile.
3. **Readiness, in two waits, both by `steady_clock`, never by frames.** `EditorAssetManager* assets = context.GetAssetManager()`
   (none: fail "No project is open"). A mesh is ready when `AssetBounds::Measure(*assets, asset)` is not pending (it need not be
   resident: the renderer requests it). A prefab or model first waits for its template (`assets->PeekLoadedAsset(asset)` casts to
   `EntityTemplate`), at most `c_PlaceWaitSeconds` (30 s: an import may run); then for its size (`Measure` not pending), at most
   `c_PlaceBoundsSeconds` (5 s, asset.info's give-up of about 300 frames at 60 frames per second, `E/EditorAssetCommands.cpp:36-37`,
   by the clock here), after which it places by the bounds measured so far and answers `bounds.pending: true`. Not ready:
   `assets->RequestLoad(asset)` and `EditorCommandResult::Defer(poll)`. The poll owns copies of the request, the parameters'
   asset and parent ids, a `std::weak_ptr<Scene>` of the active scene, the start time and the time the template became
   resident; each frame it fails, in this order: with `Cancelled` when the project closed (`context.GetAssetManager()` null,
   as asset.info's poll, `E/EditorAssetCommands.cpp:746-748`); when the active scene is another one ("The active scene changed
   (play started or stopped, or another scene opened) before 'X' was ready: nothing was placed"); when the scene is being opened
   (`CheckSceneEditable`); with the load error when the asset's state is `Failed` (`GetAssetError`); after `c_PlaceWaitSeconds`
   without the template ("'X' did not load within 30 s (still importing?)"); otherwise it requests the asset again and places
   once ready or once the size wait ran out. With the `AssetBounds` cache (4.2) a frame that finds nothing it waits for
   allocates nothing (AGENTS.md, "Commands never block a frame").
4. **Place.** `bounds = Measure(...).Bounds` (complete; or, after the size wait ran out, the partial bounds, and
   `bounds.pending` is true); `Place(scene, request, bounds)`; nullopt (requireSurface) fails with "Nothing under the ray within
   N m: nothing was placed" (Failed, not InvalidParameters).
5. **Create, one transaction.** `SceneEditTransaction transaction(scene, "Place Asset", {})`.
   - Prefab or model: `CommandUtils::InstantiateTemplate(context, transaction, *entityTemplate, asset, parent, &error)` (extracted from
     `prefab.instantiate`'s single-instance body `E/EditorPrefabCommands.cpp:348-367`: instantiate, `TrackCreated`, `MarkSynced`
     against the stored prefab; declared in `E/CommandUtils.h` with "(EditorPrefabCommands.cpp)", and used by `prefab.instantiate`
     itself, whose behavior does not change). An instantiation error rolls back (`RollBackEdit`, Failed).
   - Mesh: `parent ? scene.CreateChildEntity(parent, name) : scene.CreateEntity(name)` with the mesh's asset name
     (`assets->GetMetadata(asset)->Name`); a full scene fails as `entity.create` does (188); `TrackCreated`; `ApplyComponents(entity,
     {"MeshRenderer": {"Mesh": <asset>}}, &error)`.
   - The root's transform, with `Ro` and `So` the root's own local rotation and scale as made (identity for a mesh). Without a
     parent: its `TransformComponent` gets `Translation`, `Rotation = request.Rotation * Ro`, `Scale = request.Scale * So`, and
     the change is signaled (`Entity::MarkModified<TransformComponent>()`, the transform contract); no decomposition, so a drop
     without rotation or scale keeps the prefab root's own values bit for bit. With a parent: `scene.SetWorldTransform(root,
     translate(Translation) * mat4_cast(request.Rotation * Ro) * scale(request.Scale * So))`; false (a parent whose transform
     cannot hold it, e.g. rotated non-uniform scale) rolls back: "The parent's transform cannot hold the placed transform".
   - A template with several roots (`AssetBounds` measures all of them around the first root's origin,
     `E/AssetBounds.cpp:55-61`) moves rigidly with the first: each other root's world transform becomes `firstPlaced *
     inverse(firstAsMade) * itsAsMade` (the matrices as instantiated and as placed), through `scene.SetWorldTransform`, so the
     placed bounds hold for every root. 0.7.0's drop moved only the first root (`U/EditorActions.cpp:298`), leaving the others
     where the template put them; models import with one root, so the common drop does not change.
   - `SceneEdit::MakeNameUnique` for the roots, `transaction.SetName(fmt::format("Place {}", SceneEdit::DescribeEntities(roots)))`,
     `FinishEdit(context, transaction, result)`.
6. **Result.** `{id, entities, placedOn: PlacementSourceToString, hitEntity, point: RestingOn.Point, normal: RestingOn.Normal,
   translation, bounds: DescribeBounds(world bounds = PlacedBounds moved by Translation, pending), snapped}`.

Registered as `registry.Register(MayDefer({ "asset.place", ..., handler }))` with the default kind Edit (one undo step, waits for
the asset scan). `editor.batch` refuses it, as every `MayDefer` command; agents that place many assets call it once each or cast
with `scene.raycast` and make them with `prefab.instantiate {instances}` (documented in the skill).

`T/Editor/CommandJournalTests.cpp` lists both, or `Editor.Journal` fails: `asset.place` in `c_EditCommands` and
`c_DeferringCommands`, `scene.raycast` in `c_QueryCommands`; neither in `c_SessionResultCommands` nor `c_AssetScanCommands`
(`WaitsForAssets` stays false: Edit commands wait for the scan by their kind).

### 4.5 The MCP toolset

`StrataCLI/src/CLI/McpToolsets.cpp`: `{ "asset.place", "scene" }` joins `c_ExactToolsets` (32-40), so an agent that enables
`scene` gets `scene.raycast` (by prefix), `asset.place`, `prefab.instantiate` and `entity.dropToGround` together. Neither joins
`core` (plan: "`core` is at its 32 tools"). `T/Editor/EditorMcpToolsetsTests.cpp` checks `GetGroupToolset("asset.place") ==
"scene"`, that the `scene` toolset lists both commands, and keeps its checks that core is 27 commands and at most 32 tools and
30,720 bytes (68-70). DG "MCP toolsets" names `asset.place` among the scene toolset's exact names.

### 4.6 `E/PlacementPreview.{h,cpp}` (new, core)

What a dragged asset would do if dropped now. Owned by `EditorViewport` (`PlacementPreview& GetPlacementPreview()`, cleared by
`ResetState`), fed by the Scene view's panel, read by the Scene view's renderer in the same frame. It writes nothing: no entity,
component, undo step, selection or version.

```cpp
// The ghost's colors (ViewportSettings::Placement): display colors set by the editor's theme, not saved.
struct PlacementColors
{
	glm::vec4 Hit;   // A surface that takes the asset; a material's target outline
	glm::vec4 Guess; // The quieter box of a fallback (Grid, InFront)
	glm::vec4 Tint;  // A material target's tint (alpha: its opacity)
	// Without the editor's theme (core code, tests): Hit is SceneRenderOptions().SelectionColor, Guess and Tint the same color at
	// alpha 0.5 and 0.35.
	static PlacementColors Default();
};

// The pointer of the Scene view over its image this frame, with an asset dragged.
struct PlacementPointer
{
	AssetHandle Asset;
	Ray PointerRay;                 // EditorPlacement::MakeViewRay of the view's camera at the pointer
	float MaxDistance = 0.0f;       // The view camera's far plane
	float FallbackDistance = 0.0f;  // EditorPlacement::GetFallbackDistance of the view
	std::optional<float> Snap;      // ViewportSettings::TranslateSnap while snapping (Snap, inverted by Ctrl)
	bool CanvasMode = false;        // The view shows the 2D canvas
};

class PlacementPreview
{
public:
	enum class Kind : uint8_t
	{
		None = 0, // Nothing is dragged over the view
		Place,    // A prefab, model or mesh: GetPlacement says where
		Material, // A material over a mesh renderer: GetTarget
		NoTarget, // A material over anything else
		Refused   // An asset that cannot be placed, or a view that takes none now (canvas mode, a scene being opened)
	};
	enum class LabelStyle : uint8_t { Normal = 0, Quiet, Warning };

	// Once per frame while an asset is dragged over the view's image: casts again only when what it depends on changed.
	void Update(EditorContext& context, const PlacementPointer& pointer);
	// The drag ended, left the view, or the view is hidden.
	void Clear();

	Kind GetKind() const;
	const std::optional<EditorPlacement::Placement>& GetPlacement() const; // Kind::Place
	bool IsSizePending() const;          // Kind::Place: AssetBounds is still measuring (the box waits for it)
	Entity GetTarget() const;            // Kind::Material
	const AABB& GetTargetBounds() const; // Kind::Material: the target's own bounds in world space
	const std::string& GetLabel() const; // The text the view shows at the pointer (made when the state changes, not per frame)
	LabelStyle GetLabelStyle() const;
	// asset.place's parameters for a drop now: {asset, ray, maxDistance, fallbackDistance, snap?} (Kind::Place); null otherwise.
	nlohmann::json MakeDropParameters() const;
	// The ghost's lines (section 4.8). Nothing for kinds other than Place and Material.
	void Draw(DebugDraw& draw, const PlacementColors& colors) const;
	// Updates that cast (tests: a still pointer casts nothing).
	uint64_t GetCastCount() const;
};
```

`Update`:

1. **Kind by asset type** (`assets->GetAssetType`): Prefab, Model, Mesh: Place; Material: Material or NoTarget; any other type:
   Refused with "Cannot place a <Type> here" (`AssetTypeToString`). `pointer.CanvasMode`: Refused, "Leave the 2D canvas mode to
   place assets". `context.IsSceneLoading()`: Refused, "The scene is being opened".
2. **The key.** First the active scene's render scene is synced (`RenderScene::Get(scene).Sync(assets->shared_from_this())`,
   as `SceneRaycast::Cast` would), so that the count read next already includes what this frame's cast would sync and a still
   pointer over a still scene never casts twice. Then `{asset, PointerRay.Origin, PointerRay.Direction, MaxDistance,
   FallbackDistance, Snap, the active scene's RenderScene::GetId(), its GetStats().TotalUpdates, assets->GetContentVersion(),
   IsSceneLoading, CanvasMode, IsPlaying}`. Equal to the last update's key: nothing is done (no cast, no Measure, no label), but
   the template is requested (`assets->RequestLoad(asset)`, Place only). `TotalUpdates` moves when the render scene synced
   changes (a moving door shows one frame later; the renderer syncs every frame); the content version moves when any load
   finished (a mesh hit on its bounds may now be hit on its triangles; the size may be known now).
3. **Place:** `measured = context.GetAssetBounds().Measure(*assets, asset)` (cached, 4.2); `Place(scene, request, measured.Bounds)`
   with `request = EditorPlacement::MakeRayRequest(PointerRay.Origin, PointerRay.Direction, MaxDistance, FallbackDistance, Snap)`
   (no rotation, no scale, `RequireSurface` false), so it never returns nullopt and is bit for bit the request `asset.place`
   builds from `MakeDropParameters()`; `IsSizePending = measured.Pending`. `MakeDropParameters` writes `PointerRay`'s floats
   unchanged.
4. **Material:** `SceneRaycast::Cast(scene, PointerRay, MaxDistance)`; a hit on an entity with a `MeshRendererComponent` is the
   target (its own bounds: `SceneBounds::GetEntityBounds(scene, target, false)`), else NoTarget.
5. **Label** (only when the key changed): Place on Surface: "<asset> on '<entity>'"; Grid: "<asset> on the grid (nothing under
   the pointer)", Quiet; InFront: "<asset> in front of the camera (nothing under the pointer)", Quiet; a pending size adds " (its
   size is loading)"; Material: "<material> onto '<entity>'"; NoTarget: "Drop a material onto a mesh", Warning; Refused: its
   reason, Warning. While playing every Place and Material label ends with " (the running game)": the drop changes the running
   copy, which play.stop discards. Names come from the asset's metadata (`Name`) and `Entity::GetName`.

`Clear` resets the kind and the key, so the next drag starts fresh. The preview holds an `Entity` only between an `Update` and
the `Render` of the same frame: the panel calls `Update` or `Clear` in every frame it renders (4.7), and `OnHidden`/`Reset` call
`Clear`.

### 4.7 The Scene view: drag, labels, drop, Escape (`P/ViewportPanel.{h,cpp}`)

- **`AcceptAssetDrops(EditorPanelContext&)`** (the signature takes the panel context, for its runner) stays where it is (214),
  right after the image item, and only reads ImGui: `BeginDragDropTarget`, then `AcceptDragDropPayload(DragDrop::c_Asset,
  ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptNoPreviewTooltip)`:
  no rectangle frames the whole view during a drag, and the source's tooltip (the asset's name) gives way to the view's label.
  A payload of the right size sets `m_Drag = {asset, delivered = payload->IsDelivery()}`; no target this frame leaves `m_Drag`
  empty. Every payload type of `c_Asset` is accepted, so a drop over the view always ends the drag there; refused ones do nothing.
- **After the final view** (after 282, before `Render` at 294): with `m_Drag` and a view, `pointer = {asset,
  MakeViewRay(view->Camera, ndc of the mouse), view->Camera.Far, GetFallbackDistance(view->FromScene ? nullptr :
  &viewport.GetCamera()), snap, canvasMode}` with `snap = (settings.Snap != io.KeyCtrl) ? std::optional(settings.TranslateSnap) :
  std::nullopt` (the gizmo's rule, `P/ViewportPanel.cpp:953`), then `preview.Update(context, pointer)`. Without `m_Drag`:
  `preview.Clear()` when it is active (the idle cost is that one check).
- **Delivery** (`m_Drag->Delivered`), after the update, by kind: Place runs `EditorActions::PlaceAsset(panelContext.Runner,
  context, panelContext.Commands, preview.MakeDropParameters())`; Material runs `component.set {entity: target, component:
  "MeshRenderer", values: {Material: asset}}` through `RunEditorCommand` (immediate, as today 1101-1102); NoTarget and Refused do
  nothing and log nothing (the label said why). Then `preview.Clear()`.
- **`EditorActions::PlaceAsset`** (new, `U/EditorActions.{h,cpp}`): `runner.Run(context, commands, "asset.place", parameters,
  completion, CommandOrigin { CommandSource::UI })`; the completion selects the returned root (`context.Select`) on success. A
  failure or a warning (the running copy) shows as a notification (the runner does this for UI commands,
  `E/EditorCommandRunner.h:63`). The completion captures the context by reference: the context outlives the runner
  (`StrataEditor/src/EditorLayer.h:288, 293`), whose destruction cancels pending commands first.
- **Game camera works:** the pointer's ray comes from `view->Camera`, which is the scene's camera with Game camera on; the
  `m_Piloting` branches of the drop (1085-1088, 1098) go away.
- **The label** after the overlays (337): with an active preview, `ViewportToolbar::DrawTextBox(drawList, position, {label},
  color)` at the mouse plus `(GetFontSize(), GetFontSize())`, kept inside the image; color `colors.Text` (Normal),
  `colors.TextSecondary` (Quiet) or `colors.Warning` (Warning). `UI::ItemProbe::RecordRect("Viewport.PlacementLabel", min, max,
  true, ImGui::GetColorU32(color), label)` for tests (the probe takes an `ImU32`, `U/ItemProbe.h:37`).
- **Escape.** `UI::CancelDragDropOnEscape()` (new, `U/Widgets.{h,cpp}`): `if (ImGui::IsDragDropActive() &&
  ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { ImGui::ClearDragDrop(); ImGui::ClearActiveID(); }`, called once per frame by
  `EditorLayer::OnImGuiRender` before the panels (after `BindingCapture::BeginFrame`, 716). Clearing the active id matters: with
  the button still down the source would start the drag again (2.2). It cancels any drag of the editor's payloads, not only
  over the view: Escape means the same everywhere. The view sees no drag that frame and clears its preview.
- `Reset` (365-374) and `OnHidden` clear the preview and `m_Drag`.

### 4.8 Drawing the ghost (`E/ViewportRenderer.{h,cpp}`)

`ViewportRenderer::Render` gains a last parameter `const PlacementPreview* placement = nullptr`; the Scene view's panel passes
`&viewport.GetPlacementPreview()`, every other caller (the Game view, captures, the project's picture, probe bakes, tests) passes
nothing, so no capture can show a ghost. With `overlays` and a preview of kind Place or Material, after the gizmos and before
the debug view's shapes: `placement->Draw(m_DebugDraw, settings.Placement)`; for Material, `m_PlacementTints` (a member vector,
its capacity reused) holds `{target, settings.Placement.Tint}` followed by the debug view's tints, and `options.EntityTints` points
at it (the target's tint wins, `R/SceneRenderer.h:464-466`); otherwise `options.EntityTints` is the debug view's as today.
`ViewportSettings` gains `PlacementColors Placement = PlacementColors::Default();` (not saved, like `SelectionColor`), which
`EditorLayer::OnAttach` sets from the theme beside the selection color (421): `Hit = Accent`, `Guess = WithAlpha(TextSecondary,
c_PlacementGuessAlpha)` (0.7), `Tint = WithAlpha(Accent, c_DebugTintAlpha)` (0.6, `EditorLayer.cpp:92`).

`PlacementPreview::Draw`, in world space (the renderer subtracts the camera's position in double, `R/SceneRenderer.cpp:3963-3968`):

| State | Lines |
| --- | --- |
| Place on Surface or Point, size known | `Box(placed box, Hit, Tested)`; the same box `OnTop` in `Hit` at alpha `Hit.a * c_HiddenEdgeAlpha` (0.3), so hidden edges read faintly; `Circle(RestingOn.Point, RestingOn.Normal, r, Hit, Tested)` with `r = clamp(0.5 * max(placed extents across the normal), 0.1, 5)` m: where it touches |
| Place on Grid or InFront, size known | `Box(placed box, Guess, OnTop)` only: no contact ring, no tested box, so a guess never looks like a hit |
| Place, size pending | `Circle(point, normal, 0.25 m)` and `Arrow(point, point + normal * 0.5 m)` in `Hit` (Surface) or `Guess` (fallbacks), `OnTop` |
| Material | `Box(target bounds, Hit, Tested)`, plus the tint |

The placed box is `AABB(PlacedBounds.Min + Translation, PlacedBounds.Max + Translation)`. One-pixel lines over the finished image,
after TAA: no history, no entity id (picks are unaffected), inside `GPU.Overlays` (no new GPU stage), no shader. The mesh ghost
that replaces the box is `editor-ux`'s (plan 4.8) and reads the same `PlacementPreview`.

### 4.9 What makes sense, per asset type

| Asset | A hit that takes it | During the drag | On the drop |
| --- | --- | --- | --- |
| Prefab, Model | Any surface the ray hits (mesh renderers; a loading mesh's bounds) | Box ghost; on a miss the quieter box on the grid or in front | `asset.place` |
| Mesh (a model's sub-asset, a built-in mesh) | Same | Same | `asset.place`: an entity with a Mesh Renderer |
| Material | A mesh renderer | Tint and outline of that mesh; elsewhere "Drop a material onto a mesh" | `component.set` on it; elsewhere nothing |
| Anything else (textures, HDR images, scenes, sounds, scripts...) | None | "Cannot place a <Type> here" | Nothing, no log |

HDR images to the sky are `editor-ux`'s, below its cut (plan 4.8). Any surface counts, ceilings and walls included, as 0.7.0's
drop: a lamp hangs under a ceiling, a sign rests on a wall (`RestOn` pushes the box out along the normal).

### 4.10 Tests

Every case below fails without the step that adds it (the probes, commands and functions do not exist before).

`Editor.Placement` (`T/Editor/PlacementTests.cpp`, the existing `PlacementHarness`, a basic3d project whose Ground's top is y = 0):

1. **"A placement rests on the surface the ray hits, else on the grid within 500 m, else in front of the camera"**: a ray down
   onto the Ground: `Surface`, `HitEntity` Ground, a cube's translation y 0.5. A ray from (0, 10, 0) that meets y = 0 at x = 50
   (beyond the 20 m Ground): `Grid`, point (50, 0, z), translation y 0.5. A ray that meets y = 0 at 1,000 m: `InFront` at the
   fallback distance, the cube's center on the point. Upward: `InFront`. `RequireSurface` with a miss: nullopt. Rotation 90
   degrees about Z of a 2x1x1 box: its placed bounds 1x2x1, resting with its bottom on the ground.
2. **"Snapping puts the placement on the grid step and finds the height there"**: step 1 m; a ray hitting the empty Ground at
   (0.8, 0, 0.3) lands at x 1, z 0, y 0. Then a cube scaled 0.5 standing at (1, 0.25, 0) (its top at y 0.5) and the same ray,
   which still hits the Ground beside it: the snapped point (1, 0) finds the cube's top (y 0.5) from c_SnapLift above. A wall
   (normal -X): x stays on the wall's plane, y and z snap. A snapped point over nothing (beyond the Ground's edge) keeps the
   height of the original hit. `Snapped` true in each.
3. **"The view's ray goes through the pointer from the camera, also far from the origin"**: `MakeViewRay` of a camera at
   (10000, 50, -10000) looking at a point 30 m away projected to NDC (the projection and the distance from the point to the ray
   computed in double) passes within 5e-3 m of it; a perspective and an orthographic camera. (A float's spacing at 10 km is
   about 1e-3 m, so a 1e-3 m tolerance would fail on rounding alone.)
4. **"AssetBounds measures a pending template again only once something it waits for was published"**: a prefab of an imported
   mesh, the mesh unloaded (`AssetManagerBase::UnloadAsset`, `Strata/src/Strata/Asset/AssetManager.h:362-364`): two `Measure`
   calls count one measurement and answer pending; another asset published between them (a memory asset added: the content
   version moves) still counts one; once the mesh loaded (the harness's context updated until `!assets.HasPendingLoads()`, by
   the clock), the next call measures and completes. Without 4.2's `Waiting` check the middle call measures again.
5. The pivot, template and rays cases (76-208) stay as they are; the rays case never used `CastAt`.

The commands (new cases in `T/Editor/PlacementTests.cpp`, suite `Editor.Placement`, through `PlacementHarness::Run`; the
harness's `Run` requires an answer at once, so test 9 calls `Commands.Execute` itself):

6. **"scene.raycast hits what the scene draws from a ray or a Scene view pixel, and misses cleanly"**: origin and direction onto
   a cube: `hit`, its id, `name` "Cube", point, normal, `onTriangle`, `distance`, `ray`; `ignore` the cube: the Ground; a short
   `maxDistance`: `{hit: false, ray}`; `viewport {x: 640, y: 360}` with `camera.set` looking at the cube: the cube (headless: the
   1280x720 default); with Game camera on (`viewport.setSettings {pilotGameCamera: true}` and a primary camera aimed elsewhere):
   what that camera sees. Refused, changing nothing: both forms, neither, origin alone, a zero direction, `viewport.x` 2000 with
   width 1280, an unknown member in `viewport`, an unknown entity in `ignore`. `editor.commands` lists it as Query; `edit.history`
   does not change.
7. **"asset.place puts a prefab, a model or a mesh where the ray lands, in one undo step"**: a crate prefab by `ray`: the root
   rests at the hit, `placedOn` "surface", `hitEntity` Ground, linked to its prefab (`PrefabInstances::GetPrefab`), the undo step
   named "Place Crate", `edit.history` + 1, `edit.undo` removes it. `point` with `normal` [-1, 0, 0]: rests against it. `Builtin/Cube`
   (a mesh): an entity named after the mesh with `MeshRenderer.Mesh` set. `rotation [0, 90, 0]` and `scale 2`: the root's rotation
   is the prefab root's own times 90 degrees about Y, its scale doubled, and it still rests on the ground (its bounds' bottom at
   y 0 within 1e-3). Under a parent translated to (5, 0, 0): the same world position as without. `snap 1`: integral x and z. A
   world scene (made as `T/Editor/EditorWorldTests.cpp` makes its worlds; unverified which helper it uses): the new root joins
   the edit layer's cell at its position.
8. **"asset.place refuses what it cannot place and changes nothing"**: a material, a texture, two location forms, none, `normal`
   without `point`, `fallbackDistance` with `viewport`, `scale` 0, `snap` -1, an unknown parent: InvalidParameters;
   `requireSurface` with a ray at the sky: Failed. Each: `edit.history` and the entity count unchanged.
9. **"asset.place waits for a template that is not resident, and gives up on one that fails"**: the crate prefab unloaded: the
   first `Execute` is pending (0.7.0's path would have finished at once through `LoadAssetSync`); `Tests::CompleteCommand`
   (`T/Editor/CommandTestUtils.h:23`) places it. The prefab file overwritten with garbage and unloaded: the command fails with
   the load error and places nothing (the harness's context does not watch files; unverified that nothing re-registers it, else
   use a prefab whose stored file was deleted). `play.start` between the call and the load: fails naming the scene change.
   `project.close` between them: fails `Cancelled`.

`Editor.UI.Placement` (`T/Editor/PlacementTests.cpp`, `HarnessEditor`, the drag helper of the existing drop test made a local
function `DragAsset(editor, from, to, steps)` that leaves the button down):

10. The existing **drop test (235-309)** keeps its positions and checks; its reference ray (262) is made with `MakeViewRay`
    of the editor camera's `GetSceneCamera(aspect)` instead of the removed `CastAt`. It adds that the newest journal entry
    (`editor.activity {newest: true, limit: 1}`) is `asset.place` from the UI with a `ray`.
11. **"Dragging a prefab over the Scene view shows where it will land and changes nothing until the drop"**: during the drag
    `GetPlacementPreview().GetKind() == Place`, `Source` Surface; over six hover frames `edit.history`'s size,
    `UndoStack::GetVersion`, `IsSceneModified` and the active scene's `GetHierarchyVersion` do not change; with the mouse still
    and once `!assets->HasPendingLoads()` (`FramesUntil`, so no load moves the content version), ten frames add no cast
    (`GetCastCount`); one pixel of movement adds one; the label probe reads "Crate on 'Ground'"; after the release the root's
    translation equals the last preview's `Translation` exactly (`==`: both requests come from `MakeRayRequest` on the same
    floats) and the preview is cleared.
12. **"Where nothing is under the pointer the ghost is quieter and the drop lands on the grid"**: a camera looking past the
    Ground's edge: `Source` Grid, the label's color is `TextSecondary`; `Draw` into a `DebugDraw` gives no `Tested` lines and the
    box's 24 `OnTop` vertices in the guess color; on a surface it gives tested lines and the contact circle. The drop lands on y = 0
    at the preview's translation.
13. **"Assets that cannot be placed say so at the pointer, and dropping them does nothing"**: a texture dragged over the view:
    "Cannot place a Texture here" in the warning color; the drop: `edit.history` unchanged and no new warning in `Log::GetBuffer()`
    (0.7.0 logged one, `P/ViewportPanel.cpp:1111`).
14. **"Escape cancels a drag: the ghost goes and the release drops nothing"**: Escape mid-drag: `ImGui::IsDragDropActive()` false,
    the preview's kind None; three frames later, with the button still down, still no drag; the release over the view leaves
    `edit.history` unchanged.
15. **"A material over a mesh tints it, and elsewhere says where to drop it"**: kind Material with the Ground as target; over the
    sky NoTarget with its label; the existing material drop case (311-355) passes unchanged.
16. **"With Game camera a drop lands under the pointer through the scene's camera"**: a primary camera at (0, 6, 12) aimed at the
    origin, Game camera on, the editor camera elsewhere: the root lands where `MakeViewRay(sceneCamera, ndc)` meets the Ground
    (0.7.0 put it under the editor camera's pivot).
17. **"Snapping follows the toolbar's Snap, inverted by Ctrl"**: Snap on with step 1: integral x and z; Ctrl held: not snapped.

`CLI.McpToolsets` (`T/Editor/EditorMcpToolsetsTests.cpp`): 4.5's checks.

`Editor.Journal` (`T/Editor/CommandJournalTests.cpp`): the kind lists of 4.4's last paragraph; the case at 209 fails until both
commands are listed.

A template with two roots placed by `asset.place {ray}` (in test 7): the second root keeps its offset from the first, rotated
with it (`rotation [0, 90, 0]`), within 1e-4 m.

`GPU.Editor.Viewport` (`T/Editor/GPUEditorViewportTests.cpp`, `ViewportGPUHarness`):

18. **"The placement ghost shows in the Scene view's image and never in captures"**: `AddScene()`, a preview of a cube's
    placement on the ground made through `PlacementPreview::Update` with test colors whose `Hit` is pure magenta (no pixel of the
    scene has it); the Scene view's renderer (`GetViewport().GetRenderer()->Render(..., &preview)`) read back with
    `Renderer::ReadTexture`: at least 40 pixels are exactly magenta inside the projected box's screen rectangle and none outside it;
    a `viewport.capture` of the same view at the same size: no magenta pixel; `CHECK(gpu.GetNewErrorCount() == 0)`.

### 4.11 Docs

- **DG "Viewport panel" / "Placing content"** (3173-3184), rewritten: drops go through `asset.place` and `EditorPlacement::Place`
  (the request, the four sources, the 500 m grid, the in-front distance, snapping's rule); the ghost (`Editor/PlacementPreview`,
  fed by the panel, drawn by the Scene view's renderer only, `DebugDraw`, its styles, the tint first in `EntityTints`); hovering
  writes nothing and casts only when its key changed; labels instead of Console warnings; Escape (`UI::CancelDragDropOnEscape`);
  `AssetBounds` keeps pending results until something is published; Add to Scene and the create menus unchanged.
- **DG "View commands"** (3227-): `scene.raycast` with its forms and answer; **DG "Command conventions"** (2530-) where deferred
  Edit commands are listed: `asset.place` as the model of an Edit command that waits by the clock.
- **DG "MCP toolsets"** (5329-5350): `asset.place` in `scene` by exact name; the core counts unchanged (27 core commands, 32
  tools); "every command (124 tools)" becomes 126 with its size measured again.
- **Arch "Editor"** (near 3245): `PlacementPreview` among the core's parts; `EditorViewport` owns it.
- **Arch, scene picking and placement** (3932-3946): `EditorPlacement` is `Place`, `MakeViewRay`, `MakeRayRequest`,
  `FindPivotSurface` and `RestOn`; `FindSurfaceAt` and `CastAt` are gone; the drop goes through `asset.place`.
- **`Docs/Manual/Prefabs.md` "Place instances"** (15-22): the ghost, the quieter guess, Escape, Snap and Ctrl, Game camera, meshes,
  materials, what says "Cannot place"; "For agents" (56-61): `scene.raycast`, `asset.place` (the `scene` toolset).
- **Skill `.claude/skills/strata-editor-automation/SKILL.md` "Placing"** (420-425): `scene.raycast` and `asset.place`, the toolset
  to enable, the batch caveat; **`strata-make-a-game`**: one line under props ("place props on terrain with `asset.place {ray}`").
- `E/EditorPlacement.h`'s header comment (20-22), `E/SceneRaycast.h` (27-29), `E/AssetBounds.h` (14-20), `P/ViewportPanel.h`'s
  class comment (30-42: drops, the ghost, Escape).

### 4.12 Acceptance (measured in the lane's worktree on this machine)

| What | Target | How |
| --- | --- | --- |
| The drop is the ghost | The root's translation equals the preview's, bit for bit | Test 11 |
| Hover writes nothing | Four versions unchanged over the drag | Test 11 |
| Casts only on change | 0 casts over 10 still frames; 1 per moved pixel | Test 11 |
| Unsupported drops | No Console warning; the label says why | Test 13 |
| Captures never show the ghost | 0 magenta pixels in the capture, at least 40 in the view | Test 18, Debug, validation layer, 0 errors |
| Hover cost at 199,712 triangles | Median `PlacementPreview::Update` with a moving pointer at most 1.2 ms (audit: 0.95 ms per ray) | `Perf.Editor.PlacementHover200kTriangles` (section 8), Release, under PerfLock, 3 runs, as information |
| No frame waits at the drop | `asset.place` defers instead of `LoadAssetSync` | Test 9; code review (no `LoadAssetSync` on the drag path) |
| Idle cost | `Perf.EditorPanels.*` and `PerfGPU.Editor.EditFrameNested100k` within the A/B's noise of 76f28a21 | 3 interleaved pairs under PerfLock (section 8) |
| MCP | 27 core commands, at most 32 tools and 30,720 bytes | `CLI.McpToolsets` |

### 4.13 Commits (in order)

1. "Reach meshes through the scene's asset manager in SceneRaycast; keep pending bounds until something loads" (4.2, test 4).
2. "Place assets along a ray: surface, grid, in front, snapping" (4.1, 4.3, tests 1-3, 5).
3. "Add scene.raycast and asset.place" (4.4, 4.5, tests 6-9, the toolset test, `Editor.Journal`'s kind lists).
4. "Drop assets through asset.place with a box ghost in the Scene view" (4.6-4.8, tests 10-18).
5. "Document placing assets and the placement commands" (4.11, the perf metric of section 8, the skill).

## 5. S0.4 Editor frame rate preferences

### 5.1 The section: `E/EditorFrameRate.{h,cpp}` (new)

Modeled on `E/EditorInterface.{h,cpp}`. Registered by `EditorPreferences::RegisterSections` (`E/EditorPreferences.cpp:68-72`, and
the header comment at `.h:39-41`), drawn by the generic Preferences page, set with `preferences.set` (Session, no undo).

```cpp
namespace EditorFrameRate
{
	inline constexpr const char* c_SettingsSection = "Editor.FrameRate";
	inline constexpr const char* c_IdleAfterSecondsField = "IdleAfterSeconds";
	inline constexpr const char* c_IdleFrameRateField = "IdleFrameRate";
	inline constexpr const char* c_UnfocusedIdleFrameRateField = "UnfocusedIdleFrameRate";
	inline constexpr const char* c_ActiveFrameRateField = "ActiveFrameRate";

	// 0.7.0's constants (EditorLayer::c_IdleFrameRate, c_UnfocusedIdleFrameRate, c_InputActivitySeconds), now the defaults.
	inline constexpr double c_DefaultIdleAfterSeconds = 0.5;
	inline constexpr uint32_t c_DefaultIdleFrameRate = 30;
	inline constexpr uint32_t c_DefaultUnfocusedIdleFrameRate = 10;
	inline constexpr double c_MaxIdleAfterSeconds = 3600.0;
	// No rate below this is offered: FramePacer sleeps without waking on input, so a lower rate would hold the first input back
	// for more than a fifth of a second.
	inline constexpr uint32_t c_MinFrameRate = 5;

	struct Policy
	{
		double IdleAfterSeconds = c_DefaultIdleAfterSeconds; // 0: never idle
		uint32_t IdleFrameRate = c_DefaultIdleFrameRate;
		uint32_t UnfocusedIdleFrameRate = c_DefaultUnfocusedIdleFrameRate;
		uint32_t ActiveFrameRate = 0; // 0: unlimited

		bool operator==(const Policy& other) const = default;
	};

	SettingsSection MakeSettingsSection();
	// The policy the preferences choose; the defaults while the section is not set or its stored value is refused.
	Policy GetPolicy(const EditorPreferences& preferences);
}
```

Section: `Id` "Editor.FrameRate", `Title` "Frame Rate", `Category` "Editor", `Scope` User, `Version` 1, `Description` "How fast the
editor redraws: as fast as allowed while anything happens, slower while idle. Applies at once to editors with a window; headless,
--frames and command-script runs ignore it."

| Field | Type | Choices | Default | Title and description (written for agents) |
| --- | --- | --- | --- | --- |
| `IdleAfterSeconds` | Float, 0-3600, `DragSpeed` 0.1 | | 0.5 | "Idle after (seconds)": "How long after the last input (and after the last automation request, at least 1 s) the editor counts as idle and redraws at the idle frame rate. 0 never counts as idle: the editor redraws at the active frame rate all the time, also behind other windows, which with an unlimited active rate is hundreds of frames per second on a fast machine. Not the --idle-timeout option, which closes a headless editor without clients." |
| `IdleFrameRate` | Enum | "60", "30", "20", "15", "10", "5" | "30" | "Idle frame rate": "Frames per second while idle with the editor's window focused. No effect while Idle after is 0." |
| `UnfocusedIdleFrameRate` | Enum | "60", "30", "15", "10", "5" | "10" | "Idle frame rate in the background": "Frames per second while idle and another window has the focus. No effect while Idle after is 0." |
| `ActiveFrameRate` | Enum | "Unlimited", "240", "165", "144", "120", "90", "60" (subject to 5.4) | "Unlimited" | "Active frame rate": "The most frames per second while anything happens (input, a running game, loading, an animation). Unlimited leaves the pace to the display's vertical sync where the graphics device paces frames, otherwise as fast as frames go. play.step's steps and commands that wait for frames always run uncapped. A cap above the display's refresh rate shows nothing more." |

Not labelled "idle timeout" anywhere (UI, docs, commands). `GetPolicy` maps the stored option strings through the tables in the
`.cpp` (an unknown string, which validation already refuses, gives the default), as `EditorInterface::GetScaleFactor` does
(`E/EditorInterface.cpp:61-74`). Settings fields have no inactive reasons (unverified that the generic page could show one), so
the descriptions say when a field has no effect.

### 5.2 The policy in `EditorLayer`: the two traps

- `EditorLayer.h`: `c_IdleFrameRate`, `c_UnfocusedIdleFrameRate` and `c_InputActivitySeconds` (125-127) are removed (their values
  are `EditorFrameRate`'s defaults; tests read those); `c_AutomationActivitySeconds` stays. New member `EditorFrameRate::Policy
  m_FrameRatePolicy;` beside the idle members (368-374). The class comment (117-121) describes the preferences.
- `OnUpdate`'s preferences block (616-620) also sets `m_FrameRatePolicy = EditorFrameRate::GetPolicy(m_Context.GetPreferences());`,
  so nothing parses JSON per frame.
- `IsBusy(now)` (843-846): input within `m_FrameRatePolicy.IdleAfterSeconds`, an automation request within
  `std::max(c_AutomationActivitySeconds, m_FrameRatePolicy.IdleAfterSeconds)`; the rest unchanged.
- New `bool RunsUncapped() const`: `m_Context.HasPendingSteps() || m_CommandRunner.GetPendingCount() > 0 ||
  m_Automation.GetPendingRequestCount() > 0`. Frames that run work someone waits on by frames (steps, `editor.wait`, `scene.open`,
  `viewport.capture`, a pending `asset.place`) are never capped.
- `UpdateFrameRate()` (875-887) becomes:

  ```cpp
  void EditorLayer::UpdateFrameRate()
  {
  	if (!IsThrottlingEnabled())
  	{
  		m_Idle = false;
  		return;
  	}
  	// Idle after 0 never idles. This is a branch of its own, not a reason to turn throttling off: the early return above
  	// keeps whatever cap the last idle frame set, which would leave an idle editor stuck at its idle rate.
  	// Pending steps, commands and automation requests run uncapped, idle or not: UpdateSteppingFrameRate's lift (cap 0 while
  	// steps are pending) must not be overwritten by an active or idle cap.
  	const bool uncapped = RunsUncapped();
  	m_Idle = m_FrameRatePolicy.IdleAfterSeconds > 0.0 && !uncapped && !IsBusy(m_Host->GetTime());
  	uint32_t cap = 0;
  	if (m_Idle)
  		cap = m_Host->IsWindowFocused() ? m_FrameRatePolicy.IdleFrameRate : m_FrameRatePolicy.UnfocusedIdleFrameRate;
  	else if (!uncapped)
  		cap = m_FrameRatePolicy.ActiveFrameRate;
  	m_Host->SetMaxFrameRate(cap);
  }
  ```

  (`IsBusy` already counts pending commands and requests, 856; `RunsUncapped` in the idle test also covers steps pending
  outside a running game's step count, so no path writes an idle cap over the lift either.)

  Trap 1 is the early return (877-881): implementing 0 through `IsThrottlingEnabled()` would keep the 30 an idle frame set. Trap 2
  is the stepping lift (860-873): before this change busy meant cap 0, so writing it during steps was harmless; with an active cap
  applied whenever busy it would cap stepping frames. `RunsUncapped` is what avoids it: it holds exactly while
  `UpdateSteppingFrameRate`'s condition (`HasPendingSteps`) does, so this function writes 0 during steps, as the lift does. No
  separate `m_CapBeforeSteps` guard is added: it would be dead logic (with steps pending the cap written here is 0 anyway), and
  the one frame where steps end inside `OnImGuiRender` (play.stop from the toolbar) is set right by this function in the same
  frame and by the lift's restore plus this function in the next, as in 0.7.0. Both traps are tested (5.5, tests 3 and 4).
- `UpdateFrameStats` and the status bar are unchanged (`FrameStats.Idle` follows `m_Idle`).
- Headless, `--frames` and command-script runs take the early return and never call `SetMaxFrameRate` from here
  (`FrameRateChanges` stays 0); their stepping lift is unchanged.

### 5.3 `editor.status` and `editor.perf`

- `editor.status`'s editor section (`EditorLayer.cpp:484-489`) adds to `frameRate`: `idleAfterSeconds`, `idleFrameRate`,
  `unfocusedIdleFrameRate`, `activeFrameRate` (0: unlimited), from `m_FrameRatePolicy`, also when `throttling` is false (they are
  the preferences; `throttling` says whether they apply). The compact answer is unchanged (DG 2995-: at most 1.5 KiB).
- `editor.perf` (`E/EditorStateCommands.cpp:149-177`): `DescribeFrameProfile` adds `{"interval", DescribeFrameTimeStats(summary.
  Interval)}` after `framesPerSecond`: the time from each frame's start to the next one's (`C/FrameProfiler.h:135`), mean, p50,
  p95 and maximum in milliseconds. It is how an agent verifies a cap (`interval.p50Ms` near `1000 / cap`). The description
  (912-957) names it: "interval (the time from one frame's start to the next: what a frame-rate cap, vsync or idle throttling
  sets)". Copy as JSON gets it too (it is the same function).

### 5.4 The FramePacer measurement, and what ships at 120 and above

The plan: "Measure `FramePacer` at 120 and above; if `sleep_until` cannot hold them, the precise wait goes to `editor-ux`." The
measurement comes first in S0.4, since it decides the `ActiveFrameRate` choices.

- **Test.** New `T/Perf/FramePacerPerfTests.cpp`, suite `Perf.FramePacer` (run by the `StrataTests.Perf` CTest through its `Perf.*`
  filter, `StrataTests/CoreShards.json`), case **"FramePacer holds the editor's frame rate choices"**: for each of 30, 60, 90, 120,
  144, 165 and 240 frames per second, a `FramePacer` paces a loop for 3 s whose frames each busy-wait 1 ms (a light editor frame);
  after 10 warm-up frames the intervals between returns of `WaitForNextFrame` are recorded. Per rate: `error = |frames /
  seconds / rate - 1|` and `p95 = nearest-rank p95 interval / period`, both in a `MESSAGE`. It records the worst over the rates as
  `Perf.FramePacer.WorstRateError` and `Perf.FramePacer.WorstIntervalP95` (ratios, `Provisional`, targets 0.02 and 1.25, section 8).
  It never fails on the values (they are the machine's), only if it cannot run. It stays as the regression test of the precise
  wait.
- **Run** in Release, under `python Tools/PerfLock.py --label wave8-editor -- <build>/bin/Release/StrataTests.exe
  --test-case="FramePacer holds*"`, three times, with no other agent's GPU suites in flight if PerfLock's status shows any.
- **Rule.** An *active* rate holds when, in at least two of the three runs, `error <= 0.02` and `p95 <= 1.25`: an active cap
  that keeps its average but alternates long and short frames stutters, which is worse than no cap. An *idle* rate holds on
  `error <= 0.02` alone: nobody watches an idle editor's frame pacing, only its cost and how soon it wakes. `ActiveFrameRate`
  offers "Unlimited" and the listed rates that hold; `IdleFrameRate` and `UnfocusedIdleFrameRate` offer those of the plan's
  lists that hold. Choices that do not hold are left out of the option tables before S0.4 lands (never shipped, so no section
  version changes), the commit states the measured values, and `Docs/Roadmap.md`'s `editor-ux` entry (S0.12) says that the
  precise wait brings them back. Why both criteria can split: `FramePacer` keeps an absolute schedule (`C/Timer.h:82-94`), so
  with Windows' default 15.6 ms timer a 60 Hz loop wakes on the tick after each target, 15.6 ms apart with a 31.2 ms gap
  about every 15 frames: 60 on average with a p95 of about 1.9 periods, while 120 and above lose the average too (a model
  from the code, unverified; the audit's idle 10 frames per second held, which says nothing about short periods).
- **Where it is measured.** The test process has no window. Since Windows 10 2004 a process gets a finer timer only if it asks
  (or a library it loads asks), and Windows 11 may withhold it from occluded windows, so the editor's own pacing may differ
  from the test's either way (unverified on this machine). The test is the gate because it is reproducible; the owner's look
  after the orchestrator rebuilds `build/windows`, with `editor.perf`'s `interval` read through `StrataCLI call --project`, is
  the check in the real editor (`preferences.set` there writes the owner's file, so only the owner changes the cap; this lane
  never does).
- **If an active 60 does not hold,** every active cap would stutter and the item would ship hollow: then the
  precise wait comes into S0.4 (recommended decision, Q1 in section 10) as `Platform::SleepUntil(std::chrono::steady_clock::
  time_point)` in `C/Platform.h` (Windows, `Strata/src/Platform/Windows/`: a per-thread waitable timer made with
  `CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)`, `SetWaitableTimerEx` with a
  relative due time, `WaitForSingleObject`, falling back to `sleep_until` when the timer cannot be made; Linux and macOS:
  `sleep_until`), used by `FramePacer::WaitForNextFrame` (`C/Timer.h:94`), with the measurement run again. No
  `timeBeginPeriod`: it changes the whole system's timer and power use. The handle is a `thread_local` RAII holder
  (`t_FrameTimer`) closed when its thread ends; `Timer.h` then includes `Strata/Core/Platform.h`, and the Windows code stays
  under `Strata/src/Platform/Windows/` (S0.8's `ExternalOwners` rule allows OS headers there). Whether 120 failing alone also
  brings it in is the orchestrator's call (openQuestions); this spec's recommendation is yes, see Q1.

### 5.5 Tests

`Editor.UI` (`T/Editor/EditorUITests.cpp`, `HarnessEditor` with its fake host; preferences in memory):

1. **"Idle throttling lowers the frame rate only while nothing happens"** (594-670) and the automation, loading and script-build
   cases (821-) pass with `EditorFrameRate::c_Default*` instead of the removed constants: the defaults keep 0.7.0's behavior.
2. **"A longer idle delay holds the full rate that long"**: `IdleAfterSeconds` 2: after a mouse move, `Frames(1, 1.5f)` keeps cap
   0, `Frames(1, 0.6f)` reaches 30.
3. **"Idle after 0 never idles, and lifts an idle cap at once"**: idle at 30 (after one second without input); `preferences.set
   {IdleAfterSeconds: 0}`; the next frame's cap is 0 and stays 0 over 10 s of frames without input, focused and with
   `WindowFocused = false`; `IsIdle()` false. Without the separate branch it stays 30 (trap 1).
4. **"An active cap applies while busy, but steps and waiting commands run uncapped"**: `ActiveFrameRate` "60": after input cap 60;
   playing: 60; paused with `play.step {steps: 5}` through the runner: 0 while the steps run (trap 2: an implementation that
   applies the active cap whenever busy, without `RunsUncapped`, reads 60 here), then 60 or the idle rate; `editor.wait
   {frames: 3}` through the runner: 0 while pending. Idle still lowers it to 30.
5. **"Idle rates in front and in the background follow the preferences"**: `IdleFrameRate` "15" and `UnfocusedIdleFrameRate` "5".
6. **"Scripted and frame-limited editors ignore the frame rate preferences"**: the existing case (672-681) also sets every field;
   `FrameRateChanges` stays 0.
7. **"Frame rate preferences refuse values outside their choices and change nothing"**: `IdleFrameRate` "1", `ActiveFrameRate`
   "1000", `IdleAfterSeconds` -1 and 3601: InvalidParameters; the preferences' version and the cap unchanged. `reset: true` gives
   the defaults back.
8. **"editor.status reports the frame rate preferences, and editor.perf the frame intervals"**: the four fields with their
   defaults, then after a change; `editor.perf`'s `interval` has `avgMs`, `p50Ms`, `p95Ms`, `maxMs` and `p50Ms > 0` after profiled
   frames with a 2 ms sleep between them.

`Editor.Preferences` (`T/Editor/EditorPreferencesTests.cpp`): **"The frame rate section is a user section with 0.7.0's defaults"**:
registered by `RegisterSections`, scope User, `GetDefault()` equals `{IdleAfterSeconds: 0.5, IdleFrameRate: "30",
UnfocusedIdleFrameRate: "10", ActiveFrameRate: "Unlimited"}`, `GetPolicy` of an unset section equals `Policy{}`. The settings
windows' walk of every section (`T/Editor/EditorSettingsUITests.cpp:505-524`) draws the new section with no change. The feature
test counts project sections only (DG 723), so it needs none.

`Perf.FramePacer` (5.4). The existing `FramePacer` cases (`T/Core/CoreTests.cpp:176-230`) are unchanged unless the precise wait
comes in, in which case they also pass with it.

### 5.6 Docs

- **DG "Editor UI rules", Idle throttling** (2434-2441): the policy from `Editor.FrameRate`, 0 never idles, the active cap, what
  runs uncapped, `editor.status`'s fields, `editor.perf`'s `interval`.
- **DG "Preferences"** (1349-1380): the third editor section and its fields; **DG `play.step`** (3341-3342): "lifts its frame-rate
  cap (headless editors' 60 frames per second, the idle rates and the active cap)".
- **DG `editor.perf`** (2840-2848): `interval`.
- **Arch** (3245-3249): the idle throttling sentence follows the preferences.
- **`Docs/Manual/Settings.md` "Preferences"** (117-133): a **Frame Rate** bullet in the person's words (what each field does, 0
  never idles, the active cap and the display).
- **`EditorLayer.h`'s class comment** (117-121).

### 5.7 Acceptance

| What | Target | How |
| --- | --- | --- |
| Defaults | 0.7.0's behavior: 0.5 s, 30, 10, uncapped | Test 1 and the existing throttling cases unchanged but for names |
| Idle after 0 | Never idle, cap lifted the next frame | Test 3 |
| Active cap | Applied while busy; 0 during steps and waiting commands | Test 4 |
| Scripted runs | No rate changes | Test 6 |
| The pacer at the offered rates | Every shipped `ActiveFrameRate` choice holds within 2% with p95 at most 1.25 periods; every idle choice within 2% | 5.4's three runs, the values in the commit |
| Agents can verify a cap | `editor.perf`'s `interval.p50Ms` | Test 8 |

### 5.8 Commits

1. "Measure FramePacer at the editor's frame rate choices" (5.4's test; the measured values in the message).
2. "Make the editor's frame rate a preference" (5.1-5.3, tests 1-8; choices per 5.4's rule).
3. "Document the editor's frame rate preferences" (5.6). If 5.4 brought in the precise wait, it is its own commit before 2.

## 6. S0.5 Performance pause

### 6.1 `FrameProfileSnapshot` and `FrameProfiler::TakeSnapshot` (`C/FrameProfiler.{h,cpp}`)

```cpp
// A copy of a FrameProfiler's recorded frames taken at one moment (FrameProfiler::TakeSnapshot): what the Performance panel
// shows while it is paused. A value: the profiler recording on, being reset or registering stages changes nothing of it.
class FrameProfileSnapshot
{
public:
	FrameProfileSnapshot() = default; // Holds no frame

	uint32_t GetCapacity() const;           // The ring's capacity when taken (the timeline's columns)
	uint32_t GetRecordedFrameCount() const; // Frames held
	FrameSample GetFrame(uint32_t age) const; // Age 0: the newest frame held; spans valid while the snapshot lives
	uint64_t GetFirstFrameIndex() const;    // FrameSample::Index of the oldest and the newest frame held
	uint64_t GetLastFrameIndex() const;
	uint32_t GetStageCount() const;         // The stages registered when taken; later ones are not in it
	const std::string& GetStageName(FrameStageID stage) const;
	bool IsStageInWorkTime(FrameStageID stage) const;
	FrameStageKind GetStageKind(FrameStageID stage) const;
	bool AreGPUTimesExpected() const;
	// FrameProfiler::Summarize over the frames held (the job system's thread counts as they were when taken).
	FrameProfileSummary Summarize(uint32_t frames) const;
private:
	friend class FrameProfiler;
	std::vector<FrameProfiler::FrameRecord> m_Records; // Newest first
	uint32_t m_Capacity = 0;
	uint32_t m_StageCount = 0;
	std::array<std::string, FrameProfiler::c_MaxStages> m_StageNames;
	std::array<bool, FrameProfiler::c_MaxStages> m_StageInWorkTime = {};
	std::array<FrameStageKind, FrameProfiler::c_MaxStages> m_StageKinds = {};
	uint32_t m_WorkerThreads = 0;
	uint32_t m_IOThreads = 0;
	uint32_t m_BackgroundThreads = 0;
	bool m_GPUTimesExpected = false;
};
```

`FrameProfiler` declares `friend class FrameProfileSnapshot;` (for `FrameRecord`, private at 259-273) and gains
`FrameProfileSnapshot TakeSnapshot(uint64_t newestFrame) const;` (the frame thread): the records still in the ring whose `Index`
is at most `newestFrame`, newest first, with the stage tables, the capacity, the GPU expectation and the job system's thread counts
now. A `newestFrame` older than every frame in the ring gives an empty snapshot. It allocates once (about 331 KB for a full ring)
and copies; it is called once per pause.

**One summary for both.** `Summarize`'s body (`.cpp` 398-501) moves into a function template in the `.cpp`'s anonymous namespace,
`template<typename RecordAt> FrameProfileSummary SummarizeFrames(uint32_t count, const RecordAt& recordAt, const StageTables&
stages, const ThreadCounts& threads)`, where `recordAt(age)` returns the record of that age (a private type the template never
names), `StageTables` holds the count and spans of the three arrays, and `ThreadCounts` the three counts.
`FrameProfiler::Summarize` calls it with the ring's indexing (`(m_NextRecord + m_Capacity - 1 - age) % m_Capacity`) and
`JobSystem::GetStats()`'s counts, as today; `FrameProfileSnapshot::Summarize` with its vector and its counts. `ToSample`
(328-343) becomes a private static taking the record and a stage count, used by both `GetFrame`s. No behavior of
`FrameProfiler` changes (the `Core.FrameProfiler` cases pass unchanged).

### 6.2 The panel (`P/PerformancePanel.{h,cpp}`)

New members: `std::optional<uint64_t> m_PauseFrame` (the newest frame at the click, while its snapshot waits for GPU times),
`uint32_t m_PauseResets` (the profiler's `GetResetCount()` at the click),
`std::optional<FrameProfileSnapshot> m_Snapshot`, `JobSystemStats m_SnapshotJobs` (the panel's `m_Jobs` when taken, for Copy as
JSON's running and queued jobs), `std::string m_PausedLabel`. Public: `bool IsPaused() const` (`m_PauseFrame || m_Snapshot`),
`const FrameProfileSnapshot* GetSnapshot() const`, for tests.

- **The toggle.** `UI::ToolbarButton("Performance.Pause", paused ? Icons::Play : Icons::Pause, paused ? "Resume" : "Pause", tooltip,
  ButtonStyle { .Enabled = profiler.GetRecordedFrameCount() > 0, .Active = paused })` left of Copy as JSON in `DrawSummary`. The
  right-aligned width (184-188) becomes `UI::GetToolbarButtonWidth(Icons::Pause, "Resume") + style.ItemSpacing.x +
  UI::GetToolbarButtonWidth(Icons::Copy, "Copy as JSON")` (the longer label, so the line does not move when it toggles). Tooltips:
  "Hold the frame breakdown still to read it (Space); the profiler keeps measuring" and "Back to the latest frames (Space)".
- **Space.** In `OnImGuiRender`, `!ImGui::GetCurrentContext()->NavCursorVisible && ImGui::Shortcut(ImGuiKey_Space,
  ImGuiInputFlags_RouteFocused)` toggles while the panel or a window inside it has the keyboard focus. Keyboard navigation is
  on in the editor (`Strata/src/Strata/ImGui/ImGuiLayer.cpp:48`), and Space activates the navigated item only while the
  navigation cursor shows (`imgui.cpp:14735-14738`): after a mouse click it is hidden, so Space toggles the pause and nothing
  else; after an arrow or Tab press it shows, and then Space presses the item the cursor is on, as everywhere in the editor,
  and does not toggle. So one key press never does two things, with no key-ownership call (`imgui_internal.h` is already
  included by UI code, `U/EditorActions.cpp:24`).
- **Pausing** (not paused): `m_PauseFrame = profiler.GetTotalFrameCount() - 1` and `m_PauseResets = profiler.GetResetCount()`
  (new member). Each frame while it waits: if `profiler.GetResetCount() != m_PauseResets` it resumes (frame indices restart at a
  reset, so the index no longer names the clicked frame); else once `!profiler.AreGPUTimesExpected()` or
  `profiler.GetTotalFrameCount() - 1 >= *m_PauseFrame + FrameProfiler::c_MaxGPULatencyFrames`, `m_Snapshot =
  profiler.TakeSnapshot(*m_PauseFrame)`. So the frames shown are those on screen at the click, their GPU times attached (they
  arrive at most 8 frames late; a few milliseconds at the rates the owner sees, about a quarter second at the idle 30, where the
  click itself is input and runs the editor at full rate anyway). An empty snapshot (the clicked frame left the ring)
  resumes. On taking it: `m_SnapshotJobs = m_Jobs`, `m_Summary = m_Snapshot->Summarize(m_Snapshot->GetCapacity())`,
  `UpdateTableRows()`, `m_PausedLabel = fmt::format("Paused \xC2\xB7 frames {}-{}", first, last)` with `StringUtils::
  FormatThousands`.
- **Resuming** (button, Space, `OnHidden`): both optionals reset, the snapshot freed, `m_LastSummaryTime = -1.0` so the live
  statistics come at once. Hiding the panel (closed, collapsed or behind another tab) resumes it: a pause is for reading what is in
  front of you, and it frees the copy (Q5).
- **What freezes** (reads `*m_Snapshot` while it exists): both timeline strips (columns, stacks, spike markers, the reference
  lines and the scale label), the hover's highlight and tooltip (the mouse maps to an age of the snapshot, so a still mouse stays on
  one frame), the summary line (frame times, FPS, the job threads' busy percentages, and the jobs running and queued as they were),
  the stage table and the GPU section (made once from the snapshot's summary: `GetTableBuildCount` does not move while paused), the
  timeline's scale and the spike threshold (both from `m_Summary`), and Copy as JSON. The summary line shows `m_PausedLabel` in
  `colors.Accent` after the frame times (`UI::ItemProbe::Record("Performance.Paused", true, 0, m_PausedLabel)`). In
  `OnImGuiRender` (132-148) the refresh condition gains `!m_Snapshot` (a paused panel never recomputes `m_Summary`, whatever
  the clock), and the "No frames were profiled yet" early return asks the shown source (`m_Snapshot`'s
  `GetRecordedFrameCount()` while paused), so a `FrameProfiler::Reset` after the snapshot leaves the paused view standing.
- **What stays live:** the frame profiler (it never stops recording), `editor.perf`, frame listeners, `--perf-log`, the status
  bar's frame pill and its tooltip (its own `Summarize`, `EditorLayer.cpp:1943`, and the panel's `OnUpdate` job samples, 112-120),
  the viewport's stats overlay. Pausing the profiler itself would feed agents and logs stale data, and at 700 frames per second a
  frame index anchor is overwritten within a second.
- **The strips read either source** through one member template, `template<typename Frames> void DrawTimelineStrip(const Frames&
  frames, const ImVec2& size, bool gpu)`, declared in the header ("Frames: FrameProfiler while live, FrameProfileSnapshot while
  paused: the same accessors") and instantiated only in the `.cpp`; `DrawTimeline` picks the source. The calls inside the strip
  (`GetCapacity`, `GetRecordedFrameCount`, `GetStageCount`, `GetStageKind`, `IsStageInWorkTime`, `GetStageName`, `GetFrame`,
  `AreGPUTimesExpected`) exist under the same names on both. `ShowsGPU` takes the snapshot's expectation while paused.
- **Copy as JSON while paused:** `DescribeFrameProfile(m_Snapshot->Summarize(capacity), m_SnapshotJobs)` plus `"paused":
  {"firstFrame", "lastFrame"}`; its tooltip: "Copy the statistics of the paused frames as JSON (editor.perf's form; editor.perf itself
  stays live)". A new static `DescribeSnapshot(const FrameProfileSnapshot&, const JobSystemStats&)` beside `DescribeProfile` (107-110).
- The class comment (`P/PerformancePanel.h:19-25`) names the pause.

### 6.3 Tests

`Core.FrameProfiler` (`T/Core/FrameProfilerTests.cpp`, a profiler with a test clock):

1. **"A snapshot keeps its frames while the profiler records on, is reset and registers stages"**: 20 frames with known stage times,
   `TakeSnapshot(19)`; 700 more frames (the ring wraps), `Reset()`, a new stage: the snapshot still holds 20 frames, indices 0-19,
   their `StageMs`, `GetStageCount()` as before, and `Summarize(600)` equal to the profiler's summary taken right before the
   snapshot (every `FrameTimeStats` field, the GPU summary, the stage list).
2. **"A snapshot up to a frame leaves out the newer ones and keeps GPU times that arrived late"**: frames 0-19, GPU times for 5-12
   recorded after frame 19 (`RecordGPUFrame`), `TakeSnapshot(12)`: newest index 12, frames 5-12 `HasGPU` with their busy times,
   frames 13-19 absent.
3. **"A snapshot of a frame no longer in the ring holds nothing"**: capacity 8, 30 frames, `TakeSnapshot(3)`: no frames.

`Editor.UI` (`T/Editor/EditorObservatoryUITests.cpp`, the profiled-frame helpers of 595-698):

4. **"Pausing the Performance panel holds its timeline, tooltip and table while the profiler records on"**: frames whose work is
   1 ms; `Click("Performance.Pause")`, `c_MaxGPULatencyFrames + 1` profiled frames: `IsPaused()`, the probe `Performance.Paused`
   names the frames; the mouse at a fixed timeline point: the tooltip's first line ("Frame N: ...") is the same over 30 more profiled
   frames; then frames whose work is 5 ms for 2 s of harness time (past `c_SummaryRefreshSeconds`): `GetTableBuildCount` and
   `GetSummary()->Work.P50` (1 ms) unchanged, while `FrameProfiler::Get().GetTotalFrameCount()` advanced and `editor.perf {frames:
   30}` reports a 5 ms p50. Copy as JSON while paused: `frame.p50Ms` 1, `paused.lastFrame` the click's frame. Space with the panel
   focused resumes (and does not copy again: the clipboard is unchanged); the summary turns to 5 ms at once. Then Tab (the
   navigation cursor shows on a button) and Space: the pause does not toggle (`IsPaused()` false), so the key never does two
   things; without the `NavCursorVisible` test it would toggle and press the button. A `FrameProfiler::Reset()` between the
   click and the snapshot resumes instead of showing frames of the new count.
5. **"Pause waits for the GPU times of the frames it shows"**: GPU times expected and faked 3 frames late (as at 638-698): the
   snapshot's newest frame is the click's and `HasGPU` is true for it.
6. **"Hiding the Performance panel resumes it and frees its copy"**: paused, the Console's tab selected: `IsPaused()` false and
   `GetSnapshot()` null.
7. The existing panel cases (582-765) pass unchanged; the layout case (790-) holds with the second button at 150%.

`Perf.EditorPanels` (`T/Editor/EditorPanelPerfTests.cpp`): after the live measurement (182-199) the panel is paused and the same
frames measured: `Perf.EditorPanels.Performance600FramesPausedP95` (section 8). The live metric
`Perf.EditorPanels.Performance600FramesP95` must not move beyond the A/B's noise (the template changes no work).

### 6.4 Docs

- **Arch "Editor", Instruments** (3282-3294): the pause, its snapshot, what freezes and what stays live, Space, hiding resumes.
- **Arch "Frame profiler"** (870-890): `TakeSnapshot` and `FrameProfileSnapshot`, a value, one summary code for both.
- **DG `editor.perf`** (2840-): "The Performance panel's pause freezes only the panel".
- **Skill `strata-editor-automation`** (553): the person can pause the panel; agents use `editor.perf`.

### 6.5 Acceptance

| What | Target | How |
| --- | --- | --- |
| A still mouse reads one frame | The same frame index over 30 later frames | Test 4 |
| Statistics freeze, the profiler does not | Table builds and the panel's p50 constant; `editor.perf` and the total frame count move | Test 4 |
| GPU times in the paused frames | The click's frame has them | Test 5 |
| Cost | Live panel within the A/B's noise of 76f28a21; paused at most the live budget (0.0924 ms p95) | Section 8 |

### 6.6 Commits

1. "Take snapshots of the frame profiler's ring" (6.1, tests 1-3).
2. "Pause the Performance panel's frame breakdown" (6.2, tests 4-7, the perf metric).
3. "Document the Performance pause" (6.4).

## 7. AGENTS.md, release notes and the roadmap

- **AGENTS.md** (Editor section, after the deferred-commands bullet), 179 characters, within plan 4.1's 250 and S0.1's room:
  "- Content is placed through `asset.place` (`EditorPlacement::Place`): the Scene view's drop and its ghost
  (`Editor/PlacementPreview`) use one request, and hovering writes nothing." The rule is whole there; DG "Placing content" gives
  the details (checklist item 5). S0.4 and S0.5 add no rule. `Docs.AgentGuide` runs after the bullet. Lane Render's spec
  (`specs/step0.md` 5.4) counts only lanes Slang (about 450) and Platform (about 200) against S0.1's 3,456 free code points;
  with this bullet step 0 takes about 830, leaving about 2,626 when the streams branch, still above the 2,250 nine streams of
  250 need, but below that spec's stated "at least 2,800" (openQuestions).
- **Release notes (0.8.0), What's new, Editor:**
  - "Drag a prefab, model or mesh from the Content Browser into the Scene view: a ghost box shows where it will land, dimmed where
    nothing is under the pointer (then it lands on the grid, or in front of the camera). A material shows the mesh it would go to.
    Snap applies, Ctrl inverts it, Escape cancels. Assets that cannot be dropped say so at the pointer."
  - "File > Preferences > Frame Rate: how long until the editor idles (0: never), how fast it redraws while idle, in front and in
    the background, and an optional cap while it is busy. The defaults are 0.7.0's."
  - "Performance panel: Pause (or Space) holds the frame breakdown still so its frames can be read; the profiler, `editor.perf` and
    the status bar keep measuring."
- **What's new, AI agents:** "`scene.raycast` (what a ray or a Scene view pixel hits) and `asset.place` (place a prefab, model or
  mesh where it lands, in one undo step), in the `scene` toolset. `editor.perf` reports frame intervals (`interval`),
  `editor.status` the frame rate preferences."
- **Upgrading from 0.7.0:** "A prefab dropped through Game camera lands under the pointer as the scene's camera sees it (0.7.0 put
  it under the editor camera's pivot); one dropped where nothing is under the pointer lands on the grid or in front of the camera,
  not under the pivot. Assets that cannot be dropped no longer log a Console warning." Plan section 6's row "Editor frame rate:
  constants, preferences with the same defaults" stands. If 5.4 left choices out: "The active frame rate offers <list> on Windows
  for now; higher caps come with a precise frame wait."
- **`Docs/Roadmap.md`** (S0.12 writes the wave's entries): the lane gives S0.12 the FramePacer measurement and, if choices were left
  out, the line for `editor-ux`'s precise wait.

## 8. Perf metrics and A/Bs

New `Provisional` entries in `StrataTests/Perf/Budgets.json` (no budget changes; the reference memory is away, D4):

| Metric | Unit | Entry | Test |
| --- | --- | --- | --- |
| `Perf.Editor.PlacementHover200kTriangles` | ms | `Provisional`, `Target` 1.2 | `T/Editor/EditorPerfTests.cpp`, `Perf.Editor`, "Hovering an asset over a mesh of 199,712 triangles casts within a millisecond": a 316x316-quad grid mesh as a memory asset under a mesh renderer, a cube dragged (`PlacementPreview::Update` with a pointer ray moved across the grid each update, so each one casts), the median of 200 updates |
| `Perf.EditorPanels.Performance600FramesPausedP95` | ms | `Provisional`, `Target` 0.0924 (the live metric's budget) | 6.3 |
| `Perf.FramePacer.WorstRateError` | ratio | `Provisional`, `Target` 0.02 | 5.4 |
| `Perf.FramePacer.WorstIntervalP95` | ratio | `Provisional`, `Target` 1.25 | 5.4 |

**A/Bs of the lane's own work, as information** (plan 4.1: no campaign, interleaved, under PerfLock): against
`.worktrees/wave8-base`'s Release `StrataTests.exe`, three interleaved pairs (base, lane, base, lane, base, lane), each under
`python Tools/PerfLock.py --label wave8-editor -- <exe> --test-suite=<suite> --test-case=<case>`, medians compared; a ratio over
1.05 is looked at before landing, never claimed as a pass or fail of a budget:

- S0.7: `Perf.EditorPanels` (the three panel metrics) and `PerfGPU.Editor`'s "EditFrameNested100k" case: what an idle Scene view
  adds (one check of the drag and one null pointer per frame).
- S0.5: `Perf.EditorPanels.Performance600FramesP95` (the strip's template) and `Perf.FrameProfiler.Frame16Stages` (the profiler's
  frame path is untouched; this confirms it).
- S0.4: none: it adds two comparisons per frame and a policy read per preferences change.

Readings beside running streams are information only (plan 4.1); the commit messages state the medians and ratios.

## 9. What the lane shares, and how merges resolve (plan section 4)

- **`showcase`** consumes S0.7a: its graybox (plan 4.2 step 4) waits for lane Render and S0.7, and places props with
  `asset.place {ray}` and checks with `scene.raycast`. Its terrain (inner 300 m at 1 m spacing: about 180,000 triangles, computed)
  casts in about a millisecond per ray (2.2); a 2M-triangle mesh would cost about 12 ms per ray, the BVH being `editor-ux`'s cut.
  If S0.7 slips, the showcase uses `prefab.instantiate` and `entity.dropToGround` (plan 4.2's table).
- **`editor-ux`** (tranche 3) builds on this lane: it owns `Editor/PlacementPreview.*`, `Panels/ViewportPanel.cpp` and
  `Panels/PerformancePanel.cpp` then (plan 4's table). The mesh ghost replaces `PlacementPreview::Draw`'s box for kind Place and
  keeps the quieter style; the live material preview replaces the tint; the paused timeline's aids work on `FrameProfileSnapshot`;
  the precise wait follows 5.4's result. This lane leaves those seams and nothing half-built.
- **Lane Platform**: S0.6's answer to why FIFO does not pace the editor changes what "Unlimited" means in practice; the section's
  description already says "where the graphics device paces frames", so no text depends on the answer. S0.8 moves windowing behind
  `Platform/GLFW` and touches `Application`; this lane changes neither `Application.cpp` nor `C/Timer.h` (unless 5.4 brings in the
  precise wait: then lane Editor merges main before its last verification, and the precise wait goes in `Strata/src/Platform/`,
  which S0.8's `ExternalOwners` rule allows for OS headers). S0.9 (`Scene.Timing`) shares no file with this lane.
- **Lane Render** (`specs/step0.md`): its S0.3 acceptance measures layout changes "at `ActiveFrameRate` 60 and 240 once lane
  Editor's S0.4 has landed (else ... through the CPU case of 7.5)". If 5.4 leaves 240 out of the choices, that measurement takes
  its CPU-case fallback for 240; the lane tells the orchestrator which caps shipped when S0.4 lands.
- **Lane Slang**: no shader here (the ghost is debug lines), so nothing to convert.
- **`physics-timing`** (tranche 1): `ActiveFrameRate` caps Play's frame pacing; interpolation makes any cap smooth. It owns
  `Scene.cpp`'s fixed loop and shares `EditorContext.cpp`; this lane does not touch `EditorContext.cpp`.
- **`local-lights`** shares `E/EditorStateCommands.cpp` (stats): this lane adds one member in `DescribeFrameProfile`; the later
  lander keeps both.
- **`animation-graph`** and others add commands: `McpToolsets.cpp`'s tables are additive; `asset.place`'s exact entry merges with
  theirs line by line.
- **Shared, additive:** `StrataTests/Perf/Budgets.json` (entries appended), `AGENTS.md` (one bullet; the later lander measures with
  `Docs.AgentGuide` after merging main), DG and Arch sections, the release notes.
- **Within the lane:** S0.7, then S0.4 and S0.5 on its tip; S0.4 and S0.5 touch different files (`EditorLayer`,
  `EditorFrameRate`, `EditorStateCommands`; `FrameProfiler`, `PerformancePanel`) except `EditorStateCommands.cpp`, which only S0.4
  edits.

## 10. Risks and open questions, with the decision recommended

| # | Question or risk | Decision |
| --- | --- | --- |
| Q1 | `sleep_until` on Windows may not hold 120 and above (or an even, unstuttered 60) without a raised timer resolution | Measure first (5.4). If an active 60 fails, the precise wait (S, Windows high-resolution waitable timer, no `timeBeginPeriod`) comes into S0.4: every active cap would stutter otherwise. Reviewer's recommendation, for the orchestrator: bring it in also when only 120 fails. The owner's display runs at 120 Hz, so a cap at the refresh rate is the most reasonable choice the owner asked for, and `editor-ux` is tranche 3, "wave 9 unless usage remains", so the plan's route would ship 0.8.0 without it; the wait is about half a day inside this lane |
| Q2 | Space also activates ImGui's navigated item | Resolved by reading 1.92.9: activation needs the navigation cursor to show (`imgui.cpp:14735`). The shortcut acts only while it is hidden (6.2), so a key press never toggles and presses at once; test 4 covers both cases |
| Q3 | The UI drop sends the pointer's ray, not its pixel | Ray: the drop then equals the ghost by construction, the journal entry replays whatever the panel's size, and Game camera needs no special case. Agents use `viewport` for pixels |
| Q4 | `asset.place` is `MayDefer`, so `editor.batch` refuses it | Accepted: the plan asks for the deferral instead of `LoadAssetSync`. Agents place one per call, or cast with `scene.raycast` and make many with `prefab.instantiate {instances}` (skill text) |
| Q5 | Hiding the panel (another tab) resumes the pause | Resume: it frees the copy, and a pause is for reading what is in front of you. Revisit only if the owner asks |
| Q6 | Which toolset holds `asset.place` | `scene` (exact name): it creates entities, and the agent that places props enables `scene` for `scene.raycast` and `entity.dropToGround` anyway |
| Q7 | A box ghost is not the mesh the owner pictured | The plan's first form; the mesh ghost is `editor-ux` step 1 and reuses `PlacementPreview`. The label names the asset, so the box is never mistaken |
| Q8 | `AssetBounds`' pending cache could miss a wait that publishes nothing | The 1 s safety net (4.2); test 4 of 4.10 covers the published case and the unrelated publish |
| Q13 | The FramePacer test runs in a windowless process, whose timer precision may differ from the editor's | The test gates the choices because it is reproducible; the owner's look at the rebuilt editor, reading `editor.perf`'s `interval`, is the real check. If the editor holds less than the test, the precise wait comes in (Q1) |
| Q9 | A 2M-triangle surface costs about 12 ms per moving drag frame | Accepted during drags only; the BVH is `editor-ux`'s cut. The showcase's terrain is about 180k triangles |
| Q10 | Rotated templates rest by an axis-aligned box | As 0.7.0: `RestOn` takes the measured AABB; `asset.place`'s `rotation` transforms the box conservatively, so rotated props may float by up to the box's slack. Documented; `entity.dropToGround` afterwards settles them |
| Q11 | The drop of an asset whose size is still loading lands later than the release | The drop defers through the runner and the root is selected when it lands; the ghost showed a marker meanwhile. Test 9 |
| Q12 | `ActiveFrameRate` below the display's refresh in Play makes the game's motion coarser | It is the person's choice and off by default; `physics-timing`'s interpolation keeps motion smooth at any cap |
| R1 | Escape cancels any editor drag, not only asset drags | Wanted: Escape means "cancel the drag" everywhere. Text edits are not active during a drag |
| R2 | The preview holds an `Entity` between frames of a scene that was replaced | It is read only in the frame it was updated; the key includes the render scene's id; `Clear` on hide and reset |
| R3 | Labels made per frame would allocate | Made only when the key changes; a still pointer formats nothing |

## 11. First hour

1. Read `plan.md` section 3 and 4.8, `specs/decisions.md`, this spec and the `editor-ux` audit. Check that S0.0 and S0.1 are on
   main (`git -C G:/GameEngineAI log --oneline -5`) and `python Tools/PerfLock.py --status`.
2. `git -C G:/GameEngineAI worktree add .worktrees/wave8-editor -b agent/wave8-editor main`; configure `cmake --preset windows -B
   build/agent-wave8-editor` there; build Debug `StrataTests` and `StrataEditor`.
3. Run `StrataTests.exe --test-suite=Editor.Placement,Editor.UI.Placement,CLI.McpToolsets,Editor.Preferences,Editor.Journal` and
   the throttling cases of `Editor.UI` to see them green before any change.
4. Write 4.10's tests 1-3 against the new `EditorPlacement` declarations (they fail to build), then implement 4.1 and 4.3.
5. Read `E/EditorAssetCommands.cpp:672-755` (a command that waits for `AssetBounds`) and `E/EditorPrefabCommands.cpp:314-380`
   before writing `asset.place`.

## Review corrections

An independent review on 2026-11-01 checked this spec against main at 76f28a21, the plan, the rulings, the `editor-ux` audit and
the sibling specs (`step0.md`, `step0-platform.md`, `radiance-cascades.md`; `step0-slang`, `showcase`, `reflections` and
`physics-timing` were not written yet). Most file:line claims held. These were changed, and win over anything above that
still reads otherwise:

1. **The drop is the ghost, bit for bit, made true.** `Ray`'s constructor normalizes its direction (`Math/Ray.h:18-21`), so the
   preview using its stored `PointerRay` while `asset.place` rebuilt `Ray(origin, direction)` from the parameters could differ
   in the last bit and fail test 11's `==`. Both now build their request with the new `EditorPlacement::MakeRayRequest` on the
   same floats (4.1, 4.4 step 2, 4.6 step 3).
2. **`asset.place` could wait forever, or contradicted itself, on a size that stays pending.** Readiness said "Measure not
   pending" while step 4 spoke of placing with partial bounds. Now two clock waits: the template (30 s, then fail) and its size
   (5 s, asset.info's give-up, `E/EditorAssetCommands.cpp:36-37`, then place with `bounds.pending: true`). The poll also fails
   `Cancelled` when the project closes and checks `CheckSceneEditable` each frame (4.4 step 3; test 9 gains `project.close`).
3. **The `AssetBounds` pending cache keyed on the global content version would re-instantiate the template on every frame that
   any asset publishes** (most frames while streaming), defeating its purpose and the "allocates nothing" claim for the poll. It
   now remembers what the measurement waited for (`SceneBounds`' pending list) and measures again only when
   `GetContentChanges` lists the asset or one of those, or after 1 s (4.2; test 4 gains the unrelated-publish case). The 2.2
   fact was also corrected: a template not yet resident returns early without instantiating (`E/AssetBounds.cpp:39-42`); the
   costly case is a resident template whose meshes or skeletons load.
4. **`Editor.Journal` would fail.** `T/Editor/CommandJournalTests.cpp:209-239` requires every command in one kind list and
   `MayDefer` to match `c_DeferringCommands`. Both commands are now listed (4.4, 4.10, 4.13, first hour; 2.1 table).
5. **The existing drop test does not "pass unchanged".** It calls the removed `CastAt` at `T/Editor/PlacementTests.cpp:262`;
   the rays case (76-111) that test 5 named never did. Test 10 now moves line 262 to `MakeViewRay`; test 5 leaves 76-208 alone.
6. **Test 3's tolerance was below a float's spacing at 10 km** (about 1e-3 m): 5e-3 m with the reference in double.
7. **Test 11's "ten still frames add no cast" was flaky by design**: any finished load moves the content version in the key,
   and the cast's own render-scene sync could move `TotalUpdates` after the key was read. The preview now syncs the render
   scene before reading its key, and the test waits for `!HasPendingLoads()` first (4.6 step 2, test 11).
8. **Templates with several roots** were measured as a whole around the first root's origin but only the first root was
   moved, so the ghost could not match the drop. All roots now move rigidly with the first (4.4 step 5; a case in test 7).
9. **`asset.place`'s first sentence was 162 characters**, above MCP's 160 (`McpToolsets.cpp:13`): "(one undo step)" moved to
   its own sentence (146).
10. **S0.4's stepping guard was dead logic and test 4's claim about it was false.** With `RunsUncapped` covering pending
    steps, `if (!m_CapBeforeSteps)` never changed what was written, and "without the guard it reads 60" could not fail. The
    guard is gone; `RunsUncapped` also keeps `m_Idle` false, so no idle cap can overwrite the lift either; test 4 now names
    what it catches (an active cap applied whenever busy) (5.2, 5.5).
11. **The FramePacer rule judged idle rates on jitter.** From the code, a 60 Hz `FramePacer` on Windows' default 15.6 ms timer
    holds its average with a p95 near 1.9 periods; that stutter matters for an active cap, not an idle one. Active rates need
    both criteria, idle rates the average only; the windowless test process versus the editor's timer precision is now a named
    risk (Q13) with the owner's look as the real check (5.4, 5.7).
12. **Space could not double-press as Q2 feared after a mouse click**: ImGui 1.92.9 activates a navigated item only while the
    navigation cursor shows (`imgui.cpp:14735-14738`), which a click hides. So test 4's "Space does not copy again" passed
    without any design. The shortcut now acts only while the cursor is hidden, with no key-ownership call, and test 4 adds the
    Tab-then-Space case that fails without it (6.2, 6.3, Q2 resolved).
13. **A profiler reset between the click and the snapshot** would have shown frames of the new count under the old index; the
    pause records `GetResetCount()` and resumes instead. A paused panel's refresh and its "No frames" early return now read the
    snapshot (6.2).
14. **Docs:** Architecture 3932-3946 describes `FindSurfaceAt` and `CastAt` and must change; DG "MCP toolsets" counts "every
    command (124 tools)", now 126 (4.11).
15. **Line claims fixed:** `EditorInterface::GetScaleFactor` is at `E/EditorInterface.cpp:61-74` (the file has 76 lines, not
    89-102); the gizmo's snap rule is `P/ViewportPanel.cpp:953`; `EditorMcpToolsetsTests.cpp` has 136 lines; `ItemProbe`
    takes an `ImU32` color (`U/ItemProbe.h:37`); `*template` (a keyword) became `*entityTemplate`.
16. **Shared with the other specs:** `step0.md` 5.4 leaves lane Editor's 179-character bullet out of step 0's AGENTS.md budget
    (still fits: about 2,626 free when the streams branch against the 2,250 the streams need) and measures S0.3 at
    `ActiveFrameRate` 240, which falls back to its CPU case if 5.4 leaves 240 out (7, 9). `step0-platform.md` 10 guessed
    `scene.raycast` might go into `E/EditorSceneCommands.cpp`; it goes into the new `E/Commands/PlacementCommands.cpp`, so that
    row has nothing to resolve.

Checked and left as written: every other file:line of 2.1 and 2.2 (ImGui 1.92.9's drag-and-drop and key-owner lines, the
throttling code at `EditorLayer.cpp:825-887`, `FramePacer` at `C/Timer.h:49-101`, `FrameProfiler`'s records and `Summarize`,
the Performance panel's strip and hover, the MCP tables and the 32-tool test, `DebugDraw`'s API and the overlay order in
`SceneRenderer.cpp:3864-3976`, `SceneCamera`'s finite far planes, which keep `maxDistance` JSON-safe); the design keeps
every renderer rule (the ghost and tint are overlays after TAA on the existing passes: no shader, no quality row, no shadow
cache or draw path changes, nothing that needs TAA).
