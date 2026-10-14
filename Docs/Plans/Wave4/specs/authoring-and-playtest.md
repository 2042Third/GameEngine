# Workstream `authoring-and-playtest`: Authoring fidelity and exact play-testing: what people and agents make behaves as the editor shows it (wave 4, size XL, new)

Read `common.md` and `plan.md` first. A new workstream. It merges the person audit's `authoring-fidelity`, `prefabs-v2`,
`play-in-editor` and `editor-polish` with the agent audit's `agent-playtest` and `agent-surface`. It **merges last** of the
six streams. Facts below were checked against main 685aef7 (`.worktrees/verify`); locate code by symbol, line numbers move.

Evidence lives in `scratchpad\wave4\audit\person\` (shots, `shots\crops`, scripts, `projects\Grove`, `STATE.md`) and
`scratchpad\wave4\audit\agent\` (`game\FoxCoins`, `regression\`, `tools\mcp_probe.py`, `tools\play_stepped.py`, `logs\`,
`shots\`, `STATE.md`), plus the rendering audit's `shots\q02_plainman.png` and `q03_playing_custom.png`.

**Step 0 check.** The New Script fix (the script directory's sorted source list in the configure stamp, `MakeStamp` in
`ScriptBuild.cpp`, fed by `StampScriptSources`) is part of step 0. Before anything else, confirm on your base that a
`script.create` followed by one `script.build` finds the new class (the step-0 package test). If step 0 did not land it,
it becomes this stream's item 0, with the tests `plan.md` names.

## Why

- **Skinned characters lie in their raw mesh-space bind pose in edit mode, thumbnails and Asset-tab previews (person C,
  rendering M L5).** CesiumMan lies half under the ground in edit mode and stands in play (person `13_man_side_edit.png`
  against `13_man_side_play.png`, `25_focus_man.png`, `10_grove_import.png`, `38_gizmos.png`; rendering
  `q02_plainman.png` against `q03_playing_custom.png`). `RenderScene::EvaluatePose` leaves a proxy unposed when the scene
  has no `ISkinningPoses` pose for its animator, and edited scenes have none unless `animation.preview` asked: the mesh
  then draws its vertices as stored, without the skeleton's rest pose and without the ancestors that orient it (glTF's
  `Z_UP` node, a Mixamo `Armature` rotated and scaled). `SkinComponent`'s comment in `Scene/Components.h` documents it.
  Thumbnails render a fresh `Scene("Thumbnail")` (`ThumbnailCache::BuildScene`) with no provider at all, and
  `SceneBounds::GetRenderableLocalBounds` uses the bind-pose mesh bounds, so framing, `entity.dropToGround` and placement
  are wrong too.
- **Changing a model's import Scale does nothing to instances already in the scene (person M, agent M F4).**
  `GltfImport::BuildModel` puts `Settings.Scale` on the model root's `TransformComponent::Scale`, and the root's
  transform belongs to the instance (`PrefabInstances.h`: left out of comparisons, kept by `Replace`). Person `18_fox_controller.png`
  and `fox2.log`; agent `02_fox_scaled.png`, `03_two_foxes.png`.
- **Poly Haven normal maps import as sRGB color, and nothing warns (person M).** `TextureImportSettings::GetDefaults`
  matches a suffix list with `EndsWith`, so `aerial_rocks_02_nor_gl_1k` (trailing `_1k`) imports as `Color` (`texinfo.log`,
  crops `30_B_material_right.png`, `30_A_texture_right.png`); the material editor accepts it in the Normal Map slot
  silently. DirectX-convention normal maps (`_nor_dx_`) have no green flip.
- **`play.step` answers before its steps run, and unrun steps survive resuming (agent M F1).** The command calls
  `Scene::Step` (adds `m_StepFrames`, one step per frame while paused) and answers at once; `Scene::SetPaused(false)` keeps
  `m_StepFrames`, so leftover steps run at the next pause. Measured: `play.step 120` then a read saw 14 steps.
  `FeatureCommands.json.in` works around it with `editor.wait 160` after `play.step 150`; the agent's
  `tools/play_stepped.py` does the same.
- **No command names the scene that runs after a game-requested switch (agent M F6).** `scene.info` and `editor.status`
  name the edited scene while the game runs Level 2 (`scene.info` in `EditorSceneCommands.cpp`), with the running scene's
  entity count.
- **No command returns world transforms or bounds (agent M F3).** The agent had to take captures to learn that the Fox
  was 100 times too large. `SceneBounds::GetEntityBounds` (used by `entity.dropToGround`) and
  `AssetManagerBase::GetBounds` (recorded at import) exist but no query exposes them.
- **Simulated input is timed in render frames, one control per command (agent M F2).** Headless at 60 frames per
  second, W held for 60 frames moved the player 2.50 m; in a windowed editor running uncapped (about 900 fps), 0.125 m
  (`logs/windowed_editor3.txt`: `input.key {"frames":60}` took 66.9 ms). `SimulatedHold` counts `Input::GetFrameIndex`.
- **The MCP tool list costs about 27k tokens and is 7 short of a 128-tool API limit (agent M F8).** 121 tools in
  106,527 bytes, `editor_status` alone 8,348 bytes (`logs/mcp_probe.txt`); `McpServer::BuildEditorTools` exposes every
  editor method. Wave 4 adds about 15 commands.
- **While the game plays there is no scene view, and the game view has no resolution or aspect (person M).**
  `ResolveViewportView` (`ViewportRenderer.cpp`) looks through the primary camera in play mode, and `ViewportPanel`
  switches to its game-view mode (no picking, no gizmos). Person `08_tetris_play.png`, `21_play_fox.png`,
  `22_export_dialog.png`.
- **Prefab changes skip every instance with any override, overrides are invisible, and prefab actions hide in the
  Hierarchy's context menu (person M).** `prefab.apply` and `PrefabSync::Propagate` replace only instances whose
  `PrefabInstances::Compare(...)` is empty (`16_prefab_apply_undo.log`: an instance with only CastShadows changed kept the
  old material). The Inspector shows a pill (`InspectorPanel::DrawHeader`) and the raw link fields (Prefab, Prefab Entity
  ID, `14_prefab_modified.png`); Apply/Revert live in `SceneHierarchyPanel`'s context menu.
- **Minor findings** (person m, agent m): primitives without colliders; the recovery dialog nags for deliberate
  discards; a stale "Building scripts" toast next to "Scripts built"; "3 errors" in the status bar against Problems' "1
  error" (`19_build_error.png`); toasts over Add Component (`20_script_attached.png`); inactive properties shown as
  active; raw link fields and raw JSON in Activity; no File > Open Scene; `project.create {directory: ""}` accepted;
  StrataRuntime ignores `--help` and runs windowed forever; `script.status` hides contact and animation-event callbacks
  (`ScriptEdit.cpp` `DescribeClass`); sound is unobservable; logs and the journal page oldest-first with `maxCount`
  vs `limit`; `entity.delete`/`entity.rename` return nothing useful; play-only commands warn about discards; the presence
  chip names queries and covers the game's HUD (agent `15_editor_agent.png`); no batch requests or multi-instance
  `prefab.instantiate`; skill gaps (the journal note comes late; no play-testing, third-person or model-check recipes).

## Scope

Numbers follow the brief's delivery order. Every item lands with its tests, the feature-test coverage it needs, docs
and skills in the same commit as the behavior (common.md sections 10 and 11).

### 1) Rest poses in edit mode, thumbnails and previews (critical)

**Provider.** `AnimationPreview` (`Animation/AnimationPreview.*`) becomes the edit poses of a scene that does not play:
- `AnimationPreview(Scene&, const AnimationPreviewSpecification& = {})` with `struct AnimationPreviewSpecification { bool
  RestPoses = false; }`. With `RestPoses`, every `AnimatorComponent` of the scene that has no `Set` request is posed with
  its skeleton's rest pose. Requests (`animation.preview`) keep working unchanged and win over the rest pose.
- The rest pose is what `AnimatorInstance::Evaluate` gives with nothing playing (`m_Count == 0`): the model transforms of
  `Skeleton::GetRestPose()` through `Skeleton::ComputeModelTransforms`, ancestors included (they are joints of the
  skeleton, so `Z_UP` and `Armature` orient it). Put it in one function, `Animation/RestPose.h`:
  `void ComputeRestPose(const Skeleton&, std::vector<glm::mat4>& outModel)`, used by `Evaluate`'s empty case too, so
  edit mode and play mode cannot drift apart.
- **Cache and idle cost.** A rest pose is computed once per skeleton handle (`std::unordered_map<AssetHandle,
  RestPoseEntry>`: state Waiting/Ready/Failed, the matrices, a version, the source revision it came from) and shared by
  every animator of that skeleton; an animator entry holds only its skeleton handle. Animators are found through the
  component's signals (`ComponentInfo::Observe` on `AnimatorComponent` with updates, plus destruction), which fill a dirty
  list: an animator is looked at only when it is added, removed or its component signaled. A skeleton's pose is computed
  again only when the asset manager reports a content change for it (`GetContentVersion`/`GetContentChanges`: a
  reimport); an eviction keeps the matrices (they are data, not a held `Ref`). While a skeleton loads, only its animators
  are waiting and `Update` requests only those skeletons. Idle frames (nothing dirty, content version unchanged, nothing
  waiting): no asset lookup, no pose computed, `GetPosesVersion` unchanged, so `RenderScene::UpdateSkinnedPoses` returns
  at its version check. `AnimationPreview::GetStats()` exposes counters (rest poses computed, asset requests, animators
  looked at) for the tests.
- **Attachments.** Rest poses do not place attachments: the importer already put joint-attached entities at the rest
  pose (`m_JointRestModel`), and `AttachToJointComponent` says the entity keeps its transform without a pose.
  `GetAttachmentWorld` considers request entries only. `UpdateRenderTransforms` runs only when the requests, the poses
  version, `Scene::GetTransformsVersion` or `GetHierarchyVersion` changed (today it walks every `AttachToJointComponent`
  on every `Update` while any preview is posed).
- **Edit pose (optional, last of this item).** `ViewportSettings::EditPose`: `Rest` (default) or `ClipStart` (what the
  animator plays on start, at time 0: its controller's default state's clip, else `Clip`). Saved with the viewport
  settings, offered in `viewport.setSettings {editPose}` and the Scene view's camera chip. A viewport setting, not a
  component property, so no component changes.

**Owners.**
- `EditorContext` creates the edited scene's preview with `RestPoses` whenever a scene becomes the edited scene (open,
  new, recovery, project open) instead of lazily in `GetAnimationPreview`; it is destroyed with the scene as today.
  `EditorContext::Update` keeps calling `Update` before the viewports render.
- `ThumbnailCache::BuildScene` gives the thumbnail scene its own `Scope<AnimationPreview>` with `RestPoses` (a member of
  `SceneJob`), updated once per frame before `Render`; the job waits while a rest pose is waiting, like it waits for
  loading meshes. `ThumbnailCache::c_Version` goes from 1 to 2 so cached thumbnails are made again. The Asset tab's large
  preview (`RequestPreview`) goes through the same path.
- Play mode is unchanged (the Animation system poses the running scene).

**Posed bounds.** `SceneBounds::GetRenderableLocalBounds` measures an entity with `SkinComponent` and
`MeshRendererComponent` posed: when the scene's `ISkinningPoses` has a pose for the animator (`SkinComponent::Animator`
resolved to an entity), the bounds are the union over the mesh's joints of
`MeshSkinBinding::JointBounds[j].Transformed(animatorToMesh * pose[skin.Joints[j]] * skin.InverseBindMatrices[j])`, the
same expression as `RenderScene::ComputePalette` (computed in `SceneBounds`, so no `Renderer/*` file changes; a test
compares both). While the animator's pose is waiting, the mesh counts as pending (`outPendingMeshes`), so
`EditorViewport::FrameScene` frames again when it arrives. Everything that measures through `SceneBounds` gets posed
bounds: `camera.focus`, scene framing, `entity.dropToGround`, `EditorPlacement` (drops and presets), thumbnail framing and
`entity.get {world}` (item 3).

**Fixtures.** `StrataTests/src/Animation/SkinnedGltf.h`'s `SkinnedArmOptions` gains `ArmatureRotationDegrees` (about +X)
and `ArmatureScale`, with vertices and inverse bind matrices authored in the armature's space as Blender and Mixamo export
them: a Z-up variant (the arm along +Z under an `Armature` rotated -90 degrees, like CesiumMan's `Z_UP`) and a
Mixamo-like variant (`Armature` rotated 90 degrees about X and scaled 0.01, vertices in centimeters). Both stand along
+Y from 0 to 2 once posed.

### 2a) Import scale baked into the model (major)

`GltfImport` applies `Settings.Scale` to the content and leaves the model root's `Scale` at 1:
- the translations of every entity below the root (decomposed matrices too) and the attachments' rest transforms
  (`m_JointRestModel` translations);
- mesh positions, before LODs are generated and bounds are taken, so simplification error and recorded bounds
  (`ImportResult::Bounds`) are in final units; a skinned mesh's joint bounds (`MeshSkinBinding::JointBounds`);
- the skeleton's rest-pose translations and the inverse bind matrices' translations (`IBM' = S * IBM * S^-1`, i.e. the
  translation times s for a uniform scale);
- animation clips' translation keys (and cubic-spline tangents of translation channels); rotation and scale keys are
  unchanged. Root motion follows from the root joint's translations.
- Cameras' clip planes and lights' ranges keep their existing scaling.

The `Scale` tooltip says the scale is baked and that placed instances follow a change. Bump `GltfImporter::GetVersion()`
(if gpu-driven-rendering bumps it too, the integrator leaves one bump). Mesh colliders built from the mesh follow.

**Migration of instances placed before the bump** (their root `Scale` is the old setting, so baking would apply it
twice):
- `PrefabSync::Propagate` has the previous snapshot: when a model's template root `Scale` changes from `a` to `b`, each
  instance root's `Scale` is multiplied by `b / a` per axis, in the same "Update Instances" undo step.
- `PrefabSync::CatchUp` has no previous snapshot: an outdated model instance whose root `Scale` equals the model's
  current import `Scale` setting (uniformly, within 1e-5 relative) while the new template root's `Scale` is 1 gets
  `Scale` 1, logged once per scene ("N instances of Fox kept their size: the import scale is now part of the model").

### 2b) Normal maps that import as normal maps (major)

- **Names.** `TextureImportSettings::GetDefaults` lowercases the stem and splits it into tokens at `_`, `-`, `.`, spaces and
  lower-to-upper case changes (`NormalGL` -> `normal`, `gl`). Resolution and variant tokens are dropped before matching:
  `1k` to `16k`, powers of two from 256 to 16384, `<w>x<h>`, `gl`, `ogl`, `opengl`, `dx`, `directx`. Tokens match anywhere:
  - normal: `nor`, `nrm`, `norm`, `normal`, `normals`, `normalmap`, `nml`, `normalgl`, `normaldx`, and `n` only as the
    last remaining token;
  - data: `rough`, `roughness`, `metal`, `metallic`, `metalness`, `occlusion`, `ao`, `orm`, `arm`, `rma`, `mask`, `height`,
    `displacement`, `disp`, `bump`, `spec`, `specular`, `gloss`, `glossiness`;
  - everything else stays `Color`.
- **Green flip.** A new setting `FlipGreen` (bool, default false; display name "Flip Green (DirectX normal map)"; tooltip;
  `GetInactiveReason` says it applies only to `NormalMap` usage), defaulted to true when the name has `dx`, `directx` or
  `normaldx`. The cooker inverts green before mips are filtered, for RGBA8 and BC5 output. Bump `TextureImporter::GetVersion()`
  from 4 to 5. Existing `.meta` files keep their settings; the feature project's committed `.meta` files must not be
  rewritten by importing (add the key where the manager would write it back).
- **Warnings with a fix.** `AssetEdit::FindTextureUsageMismatches(EditorContext&, const MaterialProperties&)` returns
  `TextureUsageMismatch {Property, Texture, Usage, Expected}`: the Normal Map slot needs `NormalMap`; Metal/Rough and
  Occlusion need `Data` (a `Color` texture is sRGB-decoded there); Base Color and Emissive with `NormalMap` or `Data` warn
  too. Usage comes from the texture's import settings (`EditorAssetManager::GetImportSettings`); textures imported with a
  model are skipped (the importer chose their usage from the slot). `UI/MaterialEditor` draws a warning row under the
  slot ("aerial_rocks_02_nor_gl_1k is imported as Color: its normals will be wrong") with a button "Import as Normal Map"
  (or "as Data") that runs `asset.setImportSettings {asset, settings: {Usage}}` through the runner (a background
  reimport). `material.set`, `material.create` and `material.get` return `warnings: [{property, texture, usage, expected,
  fix: {command, parameters}}]`.

### 3) Agent exactness (major F1, F6, F3; minor F9, F15)

**3a `play.step`.**
- `Scene` gains `uint64_t GetFixedStepCount() const`: fixed steps run since `OnRuntimeStart`, counted on both paths of
  `OnUpdateRuntime` (stepping and the accumulator). Input timing (item 4) uses it too.
- `Scene::SetPaused(false)` drops pending steps (`m_StepFrames = 0`); `OnRuntimeStop` too. Decision: dropped, not run
  first: resuming hands the game to real time, and a count asked for while paused must not leak into a later pause.
  `EditorContext::ReplaceRuntimeScene` keeps carrying pending steps across a game-requested switch (documented).
- `play.step {steps (default 1, 1..100000; frames stays an accepted alias with the same meaning), wait (default true)}`
  becomes deferred and answers once the running scene's fixed step count reached its start plus `steps`:
  `{state, paused, stepsRun, frameIndex, time, fixedSteps}`. If the game is resumed, stopped, crashes or quits first, it
  fails with `stepsRun` so far ("play mode was resumed after 37 of 120 steps; the rest were dropped"). `wait: false`
  answers at once as today, with `pendingSteps`. A second `play.step` while one waits adds its steps and answers when its
  own last step ran.
- While steps are pending (`EditorContext::HasPendingSteps()`), the frame cap is lifted: `Application::SetMaxFrameRate`
  (through `EditorHost`), which a headless editor sets to 60 (`c_HeadlessFrameRate` in `EditorApplication.cpp`) and
  `EditorLayer`'s idle throttling lowers, goes to 0 and is restored afterwards, so 600 steps take the frames' work, not
  10 s. Each frame still runs exactly one step.
- `FeatureCommands.json.in` drops its `editor.wait 160`; `play_stepped`-style loops become one `play.step` per move.

**3b The running scene.** `DescribeRunningScene(EditorContext&)`: null in edit mode, else `{scene (the running scene
asset's handle, null while it is the edited scene's copy), name, path, entityCount, subScenes: [{id, scene, name, state,
entities}], switching: {scene, progress} | null}`. It goes into `play.state` and `editor.status`'s `play` as `running`,
and into `scene.info` as `running` (`name` keeps naming the edited scene). `EditorContext::ReplaceRuntimeScene` logs
"Play mode switched to 'Level2' (Scenes/Level2.stscene)" and records an Internal entry in the activity journal
(`CommandJournal::RecordEvent("play.sceneSwitched", {from, to})`: an entry that is not a command, kind Session, shown by
the Activity panel and skipped by replays).

**3c World transforms and bounds.**
- `entity.get {entity, world?: bool}`: with `world`, adds `world: {position, rotation: {euler (degrees, as the Inspector
  shows), quaternion [x, y, z, w]}, scale}` decomposed from the cached world transform, and `bounds: {min, max, size,
  center, pending}`: `SceneBounds::GetEntityBounds` of the entity with its descendants, posed for skinned meshes (item 1),
  `pending` when a placeholder box stood in for a loading mesh.
- `entity.find {..., box?: {min, max}, limit?}`: entities whose own world bounds (renderable, else physical shape, else
  their origin) intersect the box, in hierarchy order.
- `asset.info` adds `bounds: {min, max, size}` for meshes (loaded mesh, else `GetBounds`), models and prefabs: the
  template instantiated into a temporary scene with a rest-pose preview and measured by `SceneBounds`, cached per content
  stamp (`EditorAssetManager::GetContentStamp`). When its meshes or skeletons are not resident, `asset.info` requests them
  and answers once they arrived (deferred, at most 300 frames; then `boundsPending: true`), so it carries `MayDefer`
  (item 9); `bounds: false` answers at once without them.

**3d `script.status`** lists every callback a class implements: `ScriptEdit`'s `DescribeClass` loops over the whole
`ScriptCallback` range (a new `c_ScriptCallbackCount` next to the enum in `Scripting/ScriptTypes.h`), so callbacks game-ui
appends show without edits here.

**3e `audio.state`** (Query, session results, play mode only, else `Failed`).
- `AudioSystem` gains `GetVoices()`: per voice `{entity (null for one-shots), clip, volume, pitch, playing, paused,
  waiting, looping, spatial, position, seconds}`; `AudioSystemStats` gains `OneShotsPlayed` and `OneShotsDropped`. A
  dropped one-shot (beyond `c_MaxOneShots`, a clip that waited longer than `c_MaxOneShotClipWait`, a clip that failed)
  logs a warning naming the clip and the reason, at most once per clip per second.
- `audio.state` returns `{voices: [...] (clip as handle and path), oneShots: {playing, played, dropped}, listener,
  masterVolume}`.

### 4) Input in game time and `input.sequence` (major F2)

- **Durations count fixed steps.** `SimulatedHold` gains `FirstStep` and `LastStep` (from the running scene's
  `GetFixedStepCount`); `SimulatedInput::Update(uint64_t fixedSteps)` ends taps once their steps ran. `input.key`,
  `input.mouseButton`, `input.gamepadButton` and `input.gamepadAxis` take `steps` (default 1) or `seconds` (game time,
  rounded up to whole fixed steps of `SceneSettings::FixedTimestep`); `frames` stays an accepted alias of `steps` (its
  schema says so). A tap answers once the step after its last has been seen. Exactness: a tap of N steps covers exactly N
  fixed steps when frames run at most one step each (always while stepping and in `input.sequence`); unpaused, a release
  lands at the end of the frame in which its last step ran, which with several steps per frame may cover up to
  `MaxFixedStepsPerFrame - 1` more (documented in the schemas and the skill).
- **`input.sequence {events, steps, observe?, keepHeld?, wait?}`** (kind Session, journaled as one entry like other input
  commands):
  - `events`: `[{step, key | mouseButton | gamepadButton (+ gamepad), action: press | release | tap (+ steps)}, {step,
    gamepadAxis, gamepad?, value}, {step, mouseMove: [x, y]}, {step, scroll: [x, y]}, {step, command: "input.<name>",
    parameters}]`. `step` counts fixed steps from the sequence's start (0-based, below `steps`); events of one step apply
    in order. The `command` form runs any `input.*` command that answers at once with `wait: false`, so game-ui's
    `input.click {entity}` can be scheduled (this stream checks it in its rebase, after game-ui merged).
  - `steps`: fixed steps to run, at most 100,000, at least the last event's step plus 1.
  - `observe`: `[{entity, component, property?, every?}, {entity, script: <class>, field, every?}, {entity, world: true,
    every?}]`, sampled after every `every` steps (default: only at the end) and at the end.
  - It pauses the game if it runs (remembering that), then each frame queues the events of the coming step into
    `SimulatedInput` (its holds belong to the sequence) and runs one step (`EditorContext::Step(1)`), so the result does
    not depend on the frame rate. At the end it releases its holds unless `keepHeld`, and resumes the game if it was
    running.
  - Answer: `{stepsRun, frameIndex, time, seen: [{step, event}], samples: [{step, values}], interrupted}`. A second
    `input.sequence` or a `play.step` while one runs fails ("an input sequence is running"); play stopping fails it with
    `stepsRun`.
- `input.state` reports holds with their steps.

### 5) The agent surface (major F8; minor F10, F11, F13, F16)

**5a MCP toolsets** (`StrataCLI/src/CLI/McpToolsets.{h,cpp}`, new; `McpServer` changes function-level beside world-io's
launch wait).
- A toolset table by command-group prefix, exact names first: `core` (an explicit list), `scene` (`scene.`, `entity.`,
  `component.`, `prefab.`, `selection.`, `edit.`), `assets` (`asset.`, `material.`, `project.`), `play` (`play.`, `input.`,
  `audio.`), `scripts` (`script.`), `animation` (`animation.`), `viewport` (`viewport.`, `camera.`), `streaming`
  (`asset.stats`, `asset.setBudget`), `perf` (`editor.perf`), `editor` (`editor.`, `log.`), `ui` (`ui.`), `world`
  (`world.`, `physics.`). `GetToolsets(method)` returns the core set when listed there plus the group's set; a group the
  table does not know gets a toolset named after its prefix, so new groups map automatically.
- `core` (27 commands): `editor.status`, `editor.batch`, `log.read`, `project.create`, `project.open`, `scene.open`,
  `scene.save`, `scene.info`, `scene.hierarchy`, `entity.create`, `entity.get`, `entity.find`, `entity.delete`,
  `component.add`, `component.set`, `prefab.instantiate`, `asset.list`, `asset.info`, `asset.import`, `material.create`,
  `script.build`, `script.status`, `play.start`, `play.stop`, `play.step`, `input.sequence`, `viewport.capture`.
- Generic tools (5): `strata_status`, `strata_launch_editor`, `strata_call`, `strata_describe {command? | toolset?}` (one
  command's full description and schema; a toolset's commands with one line each; without arguments the toolsets; it
  replaces `strata_list_methods`, which stays callable for older clients but is not listed) and `strata_toolsets {enable?,
  disable?}` (sends `notifications/tools/list_changed`).
- `StrataCLI mcp --toolsets <list> | all` picks the exposed sets (default `core`); the server's `instructions` text names
  the toolsets and `strata_describe`.
- Compact definitions (`MakeCompactToolDefinition`): the description's first sentence (at most 160 characters, plus
  "(details: strata_describe)" when cut), property descriptions cut to their first sentence (at most 100 characters),
  enums and `required` kept. `editor.commands` and `strata_describe` keep the full text.
- **`editor.status {sections?}`.** Default compact: `{engineVersion, project {open, name, path}, scene {name, scene,
  modified, entityCount}, play {state, paused, pendingSteps, running}, selection {count, primary}, undo {undo, redo},
  errors {unread, scriptBuild {failed, errors}, scriptFault, importFailures}, assets {ready, loading, importing, scanning,
  failed}, saving, editor {launcher}}`. `assets.scanning` and `saving` are world-io's, in its shapes (`{done, total,
  phase}` or null; `[{saveId, label, documents, state, progress}]`): `StrataCLI launch` polls `assets.scanning` through
  the compact form. `sections: [...]` adds `engine`, `undo` (full), `sceneLoad`, `memory` and the provider
  sections (`automation`, `assets` (full), `game`, `recovery`, `editor` (full)); `["all"]` gives today's whole answer.
  Tests and command scripts that read other sections ask for them.

**5b Results and paging (F10).**
- `log.read {after?, before?, newest?, logger?, limit?, minLevel?}`: `newest: true` returns the tail newest first (page
  with `before`); `logger` filters by exact logger name; `limit` is the parameter (`maxCount` stays an accepted alias).
- `editor.activity {newest?: true, before?}` lists newest first.
- `entity.delete` returns `{deleted: [ids listed], count (with descendants)}`; `entity.rename` returns `{entity, name}`.

**5c Play-only targets (F16).** Commands whose only target is the running game (`scene.loadAdditive`,
`scene.unloadAdditive`, `script.setField`/`script.add`/`script.remove` while playing) give no "discarded by play.stop"
warning, and stay `RunningCopy` in the journal so replays skip them.

**5d Presence chip (F11).** `CommandJournal::GetLatestAutomationActivity` follows mutating entries only (the journal's
`mutating`: kind Edit, or an edit of the running copy). Both viewports get a toolbar strip above the image that holds
their chips (camera, overlays, debug view, resolution, the agent chip); the image starts below it, so no chip ever
covers the game's HUD or game-ui's canvases. The play-state color strip becomes the toolbar's bottom border.

**5e Skills (F13).** `strata-make-a-game`: the journal note moves to section 1; deterministic play-testing (`play.step`
semantics, `input.sequence` with `observe`, input durations in steps); a third-person controller and follow-camera recipe
distilled from FoxCoins (`FoxPlayer.cpp`, `FollowCamera.cpp`: camera-relative movement, `CharacterController::Move`,
facing with `atan2`); a model checklist (size through `asset.info`/`entity.get {world}` bounds; glTF faces +Z while
`GetForward()` is -Z; import Scale is baked); the replay prerequisite (copy `Scripts/` first); the recovery entry's
`complete` field (`SceneRecovery`: a snapshot of every entity of a never-saved scene). `strata-editor-automation`:
toolsets, `strata_describe`, compact `editor.status`, `editor.batch`, world queries, `audio.state`. `strata-scripting`:
input timing.

### 6) A Game view beside the Scene view (major)

- **Panel.** `Panels/GameViewPanel.{h,cpp}`: id `EditorPanels::c_Game` ("Game"), title "Game", icon `Icons::Gamepad2` (already in
  `UI/Icons.h`), registered in `EditorLayer::RegisterBuiltinPanels`
  after the Viewport and docked as a tab in the Viewport's node. `EditorLayer::c_LayoutVersion` goes from 4 to 5 (once
  for the wave). The Viewport panel's title becomes "Scene"; its id stays `Viewport`, so saved layouts, probes and
  `editor.showPanel {panel: "Viewport"}` keep working (`"Scene"` is accepted as an alias).
- **What it shows.** Always the active scene's primary camera (`ResolveViewportView(..., ViewportCameraSource::Scene,
  ...)`): the edited scene's in edit mode, the running scene's in play mode; as the game shows it (no preview lighting,
  HUD on, no overlays, no picking, no gizmos). Without a primary camera it shows the message `GameRenderer` shows.
- **Resolution.** A menu in its toolbar strip: Free (fills the panel), 16:9, 16:10, 4:3, 21:9 (the largest image of that
  aspect inside the panel, letterboxed with bars in the theme's Chrome color), fixed 1920x1080 and 1280x720 (the export's
  default window), Custom (width x height), and "Game's request" when the game asked for a resolution
  (`EditorGameWindow`). Fixed sizes render at exactly those pixels and are scaled to fit. Stored in
  `ViewportSettings::GameView {Mode, Width, Height}`, saved per project with the viewport settings; `viewport.getSettings`
  and `viewport.setSettings {gameView: {mode, width, height}}`. `viewport.capture {camera: "scene"}` defaults to the Game
  view's resolution when its mode is fixed or an aspect.
- **Input.** The focused Game view feeds `Input` while playing (`Input::SetEnabled`), and Shift+F1 leaves it (moved from
  `ViewportPanel::UpdateGameInput`). `Input::SetViewport(origin, size, resolution)` gains the render resolution, so
  positions reach scripts (and game-ui) in the game's pixels; `EditorGameWindow::GetState` reports that resolution. The
  `Input` change is game-ui's file: keep it to that parameter (function-level). Play (toolbar, Ctrl+P) brings the Game
  tab forward when it is docked behind another tab (`EditorPanelRegistry::Focus`); Stop brings the Scene tab back when
  Play had switched.
- **Scene view while playing.** It keeps the editor camera, picking, gizmos, outline and its chips on the running scene;
  edits apply to the running copy, as commands do. It is an editor view (preview lighting setting, HUD only with the
  Game UI chip). A toolbar toggle "Game camera" (F8, `ViewportSettings::PilotGameCamera`) shows the primary camera in the
  Scene view with the editor's overlays. `ViewportCameraSource::Automatic` keeps its meaning for captures (the game's
  camera while playing); the panels pass explicit sources.
- **Renderer.** `EditorViewport::GetGameRenderer()`: a third `ViewportRenderer`, created on first use. Sharing the capture
  renderer would recreate its targets whenever a capture and the Game view differ in size; measure the third renderer's
  memory at 1920x1080 and report it. `ReleaseTargets()` when the panel is hidden, closed or detached.
- **Idle cost.** Closed or hidden behind a tab: no render, no targets, no per-frame work beyond the registry's open check.
  Shown while nothing plays: it renders the edited scene's camera like any viewport (the person chose to show it).

### 7) Prefabs v2: per-property overrides (major)

- **Data.** `PrefabInstanceComponent` (stays in `Scene/Components.h`) gains `std::vector<PrefabOverride> Overrides`, used
  on instance roots only: `struct PrefabOverride { UUID Entity (the prefab entity id); std::string Component;
  std::string Property; PrefabOverrideKind Kind (Property, AddedComponent, RemovedComponent, RemovedEntity) }`. Script
  fields are `Component "Script"`, `Property "<Class>.<Field>"`; an added or removed script is `"Script"`, `"<Class>"`
  with the component kinds. Serialized through `ComponentInfo::SerializeExtra`/`DeserializeExtra` as `"Overrides"` only
  when not empty (scenes without overrides are byte-identical); validated on load (unknown kinds and malformed entries
  are dropped with a warning). Not part of `MakeTemplate`, `Compare` or `GetContentHash` (links are stripped). Hidden in
  the Inspector, reported by `component.get`.
- **Meaning.** An override is a value of the instance that differs from its prefab as stored. The root's transform and
  name stay the instance's own and are never overrides. An edit back to the prefab's value clears the override.
- **Maintenance.** `Scene/PrefabOverrides.{h,cpp}` (Scene layer): `ComputeOverrides(const Scene&, Entity root, const
  nlohmann::json& part)` diffs an instance against its part of the prefab (per entity by `PrefabEntityID`, per component,
  per property, JSON value equality as `PropertyJson` writes it; script fields through `ScriptComponent`'s extra data).
  `EditorContext::CommitEdit` updates the override lists of the instances an edit touched before it captures the
  after-states: it tracks their roots and diffs only the touched linked entities against `PrefabSync::GetBaseline`. So
  `SetPropertyWithUndo`, `component.set/add/remove`, `script.*`, `entity.*`, gizmo drags (`TransformDrag`) and the Inspector
  all maintain it, and undo restores the list with the entities. Edits while playing record nothing.
- **Propagation.** `PrefabOverrides::Propagate(Scene&, Entity root, const nlohmann::json& previousPart, const
  nlohmann::json& nextPart, std::vector<UUID>* outChanged)`: properties that changed between the two parts take the new
  value on every instance where they are not overridden (set through reflection with the component's signal, keeping
  the transform contract); components and entities the prefab added are created unless a `Removed*` override says
  otherwise; ones it removed go unless the instance added them (`AddedComponent`, unlinked entities). Then the list is
  computed against the new part. `prefab.apply` and `PrefabSync::Propagate`/`CatchUp` use it for **every** instance; the
  undo step tracks only entities that changed. Instances without a recorded list (scenes from before) use `previous` when
  there is one (`Propagate`); `CatchUp` without one keeps today's rule (whole replace when unchanged since sync, else
  outdated).
- **Instantiation stays fast.** `Prefab::Instantiate`/`InstantiateMany` and `EntityPrototype` are unchanged: new
  instances have empty lists (nothing per instance). Component values given to `prefab.instantiate` are applied after the
  prototype's fast path and recorded as overrides by the commit. Measure `Perf.Scene.InstantiatePrefab1k`,
  `Perf.Editor.SceneOpen10kPrefabInstancesMaxFrame` and the play start's `Scene::Copy` against the base: the component
  grows by one empty vector; if the A/B shows a regression, move the list into a root-only `PrefabOverridesComponent`
  declared in `Scene/PrefabOverrides.h` and registered by `RegisterSceneModule` (say so in the report).
- **Hierarchy marks** read the list (non-empty: modified) instead of comparing each instance (`PrefabSync::IsModified`
  falls back to `Compare` only for instances without a list).
- **Commands.** `prefab.status` adds `overrides: [{entity (scene id), prefabEntity, component, property, kind}]`.
  `prefab.apply {entity, component?, property?, force?}` writes all overrides (as today) or one component's or property's
  into the prefab and propagates; `prefab.revert {entity, component?, property?}` reverts all, a component or a property.
  Each is one undo step.
- **Inspector.** `InspectorPanel::DrawPrefabBar` under the header for linked entities: the prefab's icon and name (click:
  inspect the asset), Select (the instance root), Open (the prefab in the Asset tab and the Content Browser; the prefab
  edit mode is wave 5), and an Overrides menu (count; Apply All, Revert All, per component Apply and Revert, each override
  listed). Overridden properties show a SemiBold label and an accent bar at the row's left edge (`UI::PropertyLabel`
  gains `PropertyLabelOptions {Overridden, InactiveReason}`), with right-click Apply to Prefab and Revert. Component
  headers of overridden components carry the mark and the same items in their menu (`DrawComponentMenu`). The raw link
  fields are no longer drawn. The Hierarchy's context menu keeps its items. Models (imported) offer Revert only.
- **Feature test.** `Feature.stscene` gets an instance of `Prefabs/Crate.stprefab` with one property override below its
  root (for example the lid's mesh renderer's `CastShadows`), saved with its `Overrides` list; the feature runners check
  `prefab.status` lists exactly that override, and that the play runs as before.

### 8) Polish (person m, side notes)

- **Primitives with colliders.** `SceneHierarchyPanel`'s `GetPresets()`: Cube gets a `BoxCollider`, Plane a thin box,
  Sphere a `SphereCollider`, Capsule a `CapsuleCollider`, Cylinder, Cone and Torus a convex `MeshCollider`, sized from the
  built-in meshes' bounds (static: no `RigidBody`). A "Mesh only" submenu keeps the plain variants. The table is shared
  with game-ui's UI presets (a union at merge): game-ui renames today's "Text" preset "3D Text" (a world-space
  `TextComponent`) and adds a UI submenu whose "Text" is UI text; keep those labels.
- **Recovery.** `RecoveryDialog::HasOffer(context, leftBefore)` opens the dialog only for entries editors left behind
  (`RecoveryEntry::Discarded == false`: crashes, kills). Deliberately discarded entries are offered in File > Recover
  Unsaved Changes... and by a non-modal toast once per project open (`EditorNotification::ShowRecovery`).
- **Notifications replaced by id.** `EditorNotifications::Replace(uint64_t id, EditorNotification)` keeps the toast's place
  and restarts its lifetime (Post when the id is gone). `EditorContext` keeps the build toast's id: "Building scripts"
  becomes "Scripts built" or "Build failed".
- **Build pill.** "Build failed · 1 error" counts the last `ScriptBuildResult`'s error diagnostics, as Problems does;
  errors the script build logs are not counted again by the unread-errors pill.
- **Toasts** are placed at the bottom right of the Scene/Game dock node (`EditorPanelRegistry`'s window rect of the
  Viewport), not over the Inspector; the main viewport's corner without it.
- **Inactive properties.** `PropertyInfo` gains `std::function<std::string(const void* component)> InactiveReason`, set
  through `ComponentRegistry::SetInactiveReason<T>(property, function)` while registration is open. On the base all hooks
  live in one block, `Scene/ComponentInactiveReasons.cpp: RegisterComponentInactiveReasons()`, called from
  `RegisterSceneModule` after the components. In this stream's rebase onto engine-modules (which merged first and moved
  the components), the rows move next to their components' registrations, because a Scene-layer file cannot name the
  moved types: SkyLight and PostProcess into `Renderer/RendererComponents.cpp`, Animator into
  `Animation/AnimationComponents.cpp`, Camera stays in Scene. After game-ui merged, add its UI rows in
  `GameUI/GameUIComponents.cpp` (`SliceBorders`, `SliceScale` unless `Sliced`; `FillAmount`, `FillDirection` unless
  `Filled`; `CellSize`, `Columns` unless `Grid`; `ReferenceResolution`, `MatchWidthOrHeight` unless `ScaleWithScreen`; the
  selectable's state images unless `ImageSwap`). Rows, each checked against the code that reads it:
  - `SkyLight`: `EnvironmentMap` and `Rotation` while `Source` is Procedural; `ZenithColor`, `HorizonColor`, `GroundColor`,
    `SunSize`, `SunIntensity` while it is EnvironmentMap; `AmbientColor` while a map or the procedural sky lights the
    scene;
  - `PostProcess`: `AutoExposureMinEV`, `AutoExposureMaxEV`, `AutoExposureSpeed` while `AutoExposure` is off;
    `BloomIntensity`, `BloomThreshold` while `Bloom` is off; `AmbientOcclusionRadius`, `AmbientOcclusionIntensity` while
    `AmbientOcclusion` is off. `Exposure` is active in both modes (it is the compensation `SceneRenderer` applies with
    automatic exposure too), so it is not marked;
  - `Camera`: the perspective fields with `Orthographic` projection and the reverse;
  - `Animator`: `Clip` while a `Controller` is set.
  The Inspector draws inactive rows disabled with the reason as tooltip; a multi-selection shows a row inactive only when
  it is inactive on every entity. `component.get` and `entity.get` add `inactive: {Property: reason}`.
- **Activity panel.** Rows show a summary (the undo step's name with entity names, "Rename 'Crate' to 'Box'"); the raw
  parameters and result JSON move into an expandable detail row. `CommandJournal::Describe` gains `summary`.
- **File > Open Scene...** (Ctrl+O): a file dialog in the project's asset directory filtered to `.stscene`, then
  `scene.open` through the runner, with the unsaved-changes question through `EditorPanelContext::Shell`.
- **`project.create {directory: ""}`** is refused as invalid parameters.
- **StrataRuntime arguments.** `Core/CommandLineOptions.{h,cpp}` (new): an option table (name, value name, description),
  `FindUnknownArgument` and `FormatUsage`, used by the editor's `EditorApplication.cpp` and by `RuntimeApplication.cpp`:
  `--help` prints the usage and exits 0, an unknown argument prints the problem and the usage on stderr and exits 3, like
  the editor (`c_UsageExitCode`).

### 9) Batches (minor F12, last)

- **`editor.batch {steps: [{command, parameters?, bind?, expect?}], atomic?: true}`** (kind Edit): parsed by
  `EditorCommandScript::FromJson` (bind/`$ref`), run in the frame it arrives through `EditorCommandRegistry::Execute`. A new
  registration flag `EditorCommand::MayDefer` marks commands that can take frames; the batch refuses those before
  anything runs, and Debug builds assert when a command's handler without the flag returns a pending result (so the
  flag stays right; `CommandJournalTests.cpp` gets a `c_DeferringCommands` list checked like the kind lists).
  - This stream merges last: in its rebase it sets `MayDefer` on every deferring command of the merged tree, its own
    (`play.step`, `input.*` with `wait`, `input.sequence`, `asset.info`) and those the other streams list in their merge
    notes (world-io: `scene.save`, `scene.saveAs`, the project and quit commands, the commands behind
    `CheckUnsavedChanges`; world-partition: `world.cook`, `world.loadCells`, `world.create`; game-ui: `input.click`,
    `input.type`; and the existing ones, such as `viewport.capture`, `script.build`, `asset.import`, `editor.wait`).
  - world-io's registry-level deferral during a background scan (`Edit` commands and `WaitsForAssets` queries) happens
    before any handler runs, so it never trips the assert, and `editor.batch` (an `Edit` command) is deferred whole
    during a scan (common.md section 8). The steps' undo steps
  become one group (`UndoStack::BeginGroup(name)`/`EndGroup()`, recording an `EditorActionGroup`); with `atomic`, a failed
  step undoes the group, drops it from the history and fails the batch with the step's index and error. The batch is one
  journal entry with its steps as children (`parent`); replays repeat the steps, never the batch and its steps both.
  Answer: `{steps: [{command, result | error}], bindings}`. At most 1,000 steps.
- **`prefab.instantiate {prefab, instances: [{parent?, components?}]}`** (instead of `parent`/`components`): one undo step;
  instances that give only a Transform (or nothing) go through `Prefab::InstantiateMany`, other components are applied
  after; returns `{instances: [{id, entities}]}`.
- JSON-RPC batches stay refused (`RpcServer`).

### 10) Command splits, docs, contract updates

- **Splits first** (common.md section 8, own commits, no behavior change): `entity.*` and `component.*` move from
  `RegisterSceneCommands` (897 lines) to `Editor/Commands/EntityCommands.cpp` (`RegisterEntityCommands`), `prefab.instantiate`
  to `EditorPrefabCommands.cpp`; `play.*` move from `EditorStateCommands.cpp` to `Editor/Commands/PlayCommands.cpp`.
- **Contract updates** (this stream is the last to touch them): `FeatureCommands.json.in` (no wait after `play.step`),
  `TetrisCommands.json.in`, `SmokeCommands.json.in`, `LauncherCommands.json` (`editor.status` sections), `ScriptsEndToEnd.cmake`
  and `ReplaySessionCommands.json.in` if they read changed results, and every skill.
- **Docs.** `Docs/Architecture.md`: Editor (Game view and Scene view, viewport toolbars, prefab overrides, stepping and
  input timing, `input.sequence`, `editor.batch`, MCP toolsets, compact `editor.status`, inactive properties), Animation
  (rest poses in edit mode and thumbnails), Asset pipeline (baked import scale, texture name tokens and `FlipGreen`).
  `AGENTS.md` rules: `play.step` answers after its steps; input durations are fixed steps; new command groups map to MCP
  toolsets and the default set stays at most 32 tools and 30 KiB; prefab overrides are maintained by `CommitEdit` (edits
  outside transactions do not mark); deferring commands carry `MayDefer`; component properties that can be inactive get
  an `InactiveReason`. `Docs/Manual/Prefabs.md` and `Docs/Manual/GameView.md` (playing and testing in the editor) for
  people. Remove the Roadmap open item "per-property overrides ... are later work" (keep the prefab edit mode there).

## Acceptance criteria

Tests that fail on the base (confirm each: run it on the base build or with the fix reverted, and say how in the report):

- **Rest poses (1).**
  - `Animation.Preview`: the rest pose equals `AnimatorInstance::Evaluate` with nothing playing, bitwise, for the arm, its
    Z-up variant and its Mixamo-like variant; a second `Update` with nothing changed computes no pose and requests no
    asset (`GetStats` counters); a skeleton reimport recomputes it once; adding, removing or changing one animator looks
    at that animator only. 1,000 animators sharing one skeleton compute one rest pose.
  - `GPU.Animation`: the Z-up arm and the Mixamo-like arm in an edited scene (with the rest-pose preview) render the same
    pixels within 1/255 as the same models in play mode with `PlayOnStart` off; their entity IDs cover the column x = 0,
    y from 0.1 to 1.9; 0 validation errors. On the base they lie along an axis of the ground.
  - `Editor.Viewport`: `SceneBounds::GetEntityBounds` of a placed instance equals [-0.1, 0, -0.1]..[0.1, 2, 0.1] within
    1e-4 (and equals the render proxy's world bounds); `entity.dropToGround` puts its lowest point on the ground at y = 0
    within 1e-4; `camera.focus` frames it upright.
  - `GPU.Editor.Thumbnails`: the model's thumbnail shows the arm upright (the bounding box of its non-sky pixels is taller
    than wide); same for the Asset tab's preview.
- **Import (2).**
  - `Asset.Gltf`: at Scale 0.01 the model root's `Scale` is 1; mesh bounds, skeleton rest translations, inverse bind
    translations and clip translation keys are 0.01 times Scale 1's; a pose (rest and a clip at 0.37 s) at Scale 0.01
    equals Scale 1's times 0.01 within 1e-5 relative.
  - `Editor.Prefab`: an instance placed at (3, 0, 1) rotated 30 degrees, then `asset.setImportSettings {Scale}` from 0.01
    to 0.02 and the reimport: its world bounds double, its root Transform is unchanged, `prefab.status` says `modified:
    false`. The migration: a scene whose instance root has `Scale` 0.01 from a model document of the previous importer
    version ends, after the reimport, with the same world bounds as a new instance (both the `Propagate` and the
    `CatchUp` path).
  - `Asset.TextureImport`: the name table (`aerial_rocks_02_nor_gl_1k`: NormalMap; `rocks_nor_dx_2k` and
    `T_Brick_NormalDX`: NormalMap with FlipGreen; `brick_Rough_4k`, `wood_ao_1k`: Data; `wood_diff_1k`, `banner_1k`,
    `snow`: Color; `stone_n`: NormalMap); `FlipGreen` inverts green of a cooked RGBA8 and a BC5 normal map (decoded pixels).
  - `Editor.Commands`: `material.set` putting a Color texture into the Normal Map slot returns the warning with its fix;
    running the fix (and `WaitForImports`) makes the texture NormalMap and `material.get`'s warnings empty.
    `Editor.UI.AssetInspector`: the warning row and its button (by probe key) run the command.
- **Exactness (3).**
  - `Editor.GameFlow`: right after `play.step {steps: 120}` answers, `Time` is 120 fixed steps and `stepsRun` is 120
    (the base answers after 1 frame with fewer steps run); pause, `play.step {steps: 100, wait: false}`, resume, pause
    again: no step runs in the next 10 paused frames (the base runs the leftovers); a resume while `play.step` waits fails
    it with `stepsRun`.
  - `Editor.GameFlow`: after a game-requested switch, `play.state.running` names the new scene (handle, name, path,
    entity count) and `scene.info.name` the edited one; the journal has the `play.sceneSwitched` event.
  - `Editor.Commands`: `entity.get {world: true}` of a child under a rotated, scaled parent matches
    `Scene::GetWorldTransform`; bounds of a cube scaled 2 at (1, 2, 3) are [0, 1, 2]..[2, 3, 4]; `entity.find {box}`
    returns exactly the entities inside; `asset.info` bounds of the built-in Cube, of a model at Scale 0.01 (0.01 times
    Scale 1's) and of a prefab; `entity.delete` and `entity.rename` results.
  - `Scripting.*` (API module): a class implementing `OnTriggerEnter` and `OnAnimationEvent` lists them in `script.status`.
  - `Editor.Audio`: a looping source and two one-shots show as three voices in `audio.state`; 65 one-shots in one frame
    report one dropped and log the warning (an expected message).
- **Input (4).** `Editor.Input`:
  - the same `input.sequence` (W held for 60 steps, Space at step 20, `observe` every 10 steps) on a character-controller
    scene gives bitwise-equal samples with frame timesteps of 1 ms, 16.7 ms and 50 ms, and the expected travel within
    1e-4;
  - `input.key {key: W, steps: 30}` in a running game whose frames last 1 ms holds for exactly 30 fixed steps (the base,
    with `frames: 30`, holds for 30 render frames: about 2 steps);
  - the sequence restores the pause state, releases its holds, refuses events beyond `steps`, and fails a concurrent
    `play.step`.
  - CTest `StrataEditor.InputSequence`: the same command script (`Editor/InputSequenceCommands.json.in`, on a copy of the
    FeatureTest project) runs headless (`--no-gpu`) and windowed (unthrottled `--frames` run, label `gpu`), each
    `expect`ing the identical final position.
- **Agent surface (5).**
  - `CLI.McpToolsets` (new, against the real `EditorCommandRegistry` built in-process and turned into
    `rpc.listMethods`' form): the default toolset exposes at most 32 tools and at most 30,720 bytes of `tools/list`
    result JSON; every registered command (and `editor.showPanel`) maps to a toolset; all toolsets together expose every
    command; `strata_call` reaches a non-core command; `strata_describe {command}` returns its full schema;
    `strata_toolsets {enable: ["scene"]}` sends `tools/list_changed`. The base exposes 121 tools in 106,527 bytes.
  - `CLI.Mcp`: updated for the generic tools (`strata_list_methods` still answers).
  - `Editor.Commands`: compact `editor.status` is at most 1,536 bytes on the FeatureTest project and has the listed keys;
    `sections: ["all"]` equals today's answer; `log.read {newest, logger, limit}` and `editor.activity {newest}` page
    correctly.
  - `Editor.UI`: no chip rect of either viewport intersects its image rect, nor the rects of screen-space text the game
    draws (and, after this stream's rebase onto game-ui, its canvases' rects); the agent chip names the latest mutating
    command and not a later `editor.status`.
- **Game view (6).** `Editor.UI` (harness): the Game view is registered, docked as a tab in the Viewport's node in layout
  5, and Play brings it forward; 16:9 in a 4:3 panel gives an image of aspect 16:9 within 1 pixel with bars, 4:3 likewise;
  `Input` is enabled only while the Game view is focused in play mode, Shift+F1 leaves; a click in the Scene view while
  playing picks a running-scene entity; the "Game camera" toggle (F8) shows the primary camera in the Scene view.
  `GPU.Editor.Viewport`: a fixed 1280x720 Game view renders a 1280x720 image; a closed Game view has no renderer targets.
- **Prefabs (7).** `Editor.Prefab` (the existing suite):
  - instance A overrides the material, instance B only `CastShadows`, instance C nothing; a change of the prefab's
    `MeshRenderer.Material` and `Transform` of a child, applied from C, reaches A's child transform, B's material and child
    transform, not A's material and not B's `CastShadows` (on the base B keeps the old material: `16_prefab_apply_undo.log`);
    one undo step restores all three and the prefab file;
  - the override list follows edits through `component.set`, `SetPropertyWithUndo`, `script.setField`, a gizmo drag and
    undo/redo; an edit back to the prefab's value clears it; it round-trips through save and load; scenes without
    overrides are byte-identical to the base's writer output;
  - `prefab.apply {component, property}` and `prefab.revert {component}` touch only what they name;
  - a prefab change made while the scene was closed reaches the non-overridden properties of a modified instance when the
    scene opens (CatchUp with a recorded list);
  - entities and components added by the instance survive propagation; an entity the instance removed stays removed.
  - `Editor.UI.Inspector`: the prefab bar's buttons and Overrides menu (probes), override marks on exactly the overridden
    rows, right-click Revert reverts one property, the raw link fields are gone.
- **Polish (8).** `Editor.UI.Hierarchy`: Create > Cube makes an entity with `MeshRenderer` and `BoxCollider`; Mesh only
  makes none. `Editor.Recovery` (existing `RecoveryTests.cpp` suite): a discarded entry opens no dialog but posts the
  toast; a left-behind entry opens it. `Editor.Notifications` (`NotificationTests.cpp`): `Replace` keeps one toast; a
  build shows one toast that changes. `Editor.UI`: the build pill shows the diagnostics' error count; toasts never
  intersect the Inspector's Add Component button; inactive rows are drawn disabled with their reason (sky Procedural:
  Environment Map; post Auto Exposure off: Min EV). `Editor.Commands`: `project.create {directory: ""}` is
  `InvalidParameters`. A CTest `StrataRuntime.Arguments`: `--help` exits 0 with the usage, `--bogus` exits 3.
- **Batches (9).** `Editor.Commands`: a batch creating three entities with `$ref` parents is one undo step; a failing step
  undoes the earlier ones (atomic) and reports its index; a `MayDefer` command is refused before anything runs;
  `prefab.instantiate {instances}` of 1,000 instances is one undo step.
- **Kind lists.** `CommandJournalTests.cpp` lists `input.sequence` (Session), `audio.state` (Query, session results) and
  `editor.batch` (Edit), plus every command the other wave-4 streams added (the union at merge).

Everything else:
- **Suites pass:** `Editor.Commands`, `Editor.Prefab`, `Editor.UI.*`, `Editor.Input`, `Editor.GameFlow`, `CLI.Mcp`,
  `CLI.McpToolsets`, `Animation.*`, `Asset.*`, the GPU suites under the validation layer (0 errors), the feature runners,
  `StrataEditor.Tetris`, Smoke, Export, Scripts, `StrataEditor.FeatureTest` (without the wait), the full Debug ctest and
  Release `-LE perf`.
- **Feature test:** `Feature.stscene` holds the Crate instance with its override; coverage passes (no new component, host
  function or SDK symbol; the new `PrefabInstanceComponent` data has a non-default value there).
- **Perf** (interleaved A/Bs under the perf lock against the step-0 base build, common.md section 9):
  - no regression in `PerfGPU.Editor.{Edit,Play}FrameNested{100k,1M}` with the Game view closed and as a hidden tab (ratio
    of medians within noise, at least 3 pairs), in `Perf.Editor`'s entity commands, `Perf.Editor.SceneOpen10kPrefabInstancesMaxFrame`,
    `Perf.Scene.InstantiatePrefab10` and `InstantiatePrefab1k`;
  - a new metric `Perf.Editor.PrefabApply10kInstances`: `prefab.apply` of one property change from one of 10,000
    instances of a 3-entity prefab, until every other instance has it (median of at least 5 confirmed measurements; the
    budget is 1.5 times the measured value, "to confirm in the integration campaign");
  - report, without a budget: the Game view's cost open while playing 100,000 cubes (`editor.perf` `Render.*` stages,
    Scene and Game views both visible against one), the third renderer's GPU memory at 1920x1080, and an idle edit frame
    with 1,000 rest-posed characters against none (`editor.perf`).
  - `play.step` and `input.sequence` correctness is gated by unit tests with counters, not by timings.
- **Screenshots** (Release editor, `scratchpad\wave4\authoring-and-playtest\shots`, at the display's 150%, at
  `--ui-scale 1` and in a 1600x900 window, looked at and fixed where they look unfinished):
  - CesiumMan standing in edit mode (the Grove project copy) and its Content Browser thumbnail and Asset-tab preview;
  - the Game view beside the Scene view while playing (a `--layout` file that splits them), at 16:9 and 4:3;
  - the prefab bar, the Overrides menu and the override marks;
  - inactive rows (sky Procedural, post-process with Auto Exposure off);
  - the material editor's normal-map warning;
  - the viewport toolbars with the agent chip while a game with a HUD plays.

## Deliver in this order if time runs short

1. Item 1, rest poses (critical): provider, owners, posed bounds, thumbnails.
2. Item 2a, baked import scale with the migration; then 2b, normal-map names, `FlipGreen` and the warnings.
3. Item 3a and 3b: `play.step` answering after its steps, steps dropped on resume, the running scene. Then 3c world
   queries; 3d and 3e.
4. Item 4: input in fixed steps, then `input.sequence`.
5. Item 5a, MCP toolsets and the compact `editor.status`: **required before merge** (with wave 4's commands the tool count
   passes 128). Then 5b to 5e.
6. Item 6, the Game view.
7. Item 7, prefab overrides: data, maintenance and propagation first; then commands; then the Inspector bar and marks.
8. Item 8, polish, in the listed order.
9. Item 9, batches.

Item 10's splits come first in the branch (they make the later diffs readable); its docs, skills and contract updates go
with each item. If time runs out, items 8 and 9 are the ones to defer, with a report entry each.

## Open items this stream owns

- Person: skinned bind pose in edit mode, thumbnails and previews (critical); prefab propagation, override marks and the
  Inspector's prefab actions; import Scale on placed instances; normal-map usage; no scene view while playing and no
  game-view resolution; primitives without colliders; the recovery nag; contradicting feedback (stale toast, "3 errors",
  toasts over Add Component, inactive properties, raw link fields, raw JSON in Activity); File > Open Scene.
- Agent: F1 `play.step`, F2 input timing, F3 world queries, F4 import Scale, F6 running scene, F8 MCP tool count, F9
  callbacks, F10 paging and results, F11 presence chip, F12 batches, F13 skills, F15 sound, F16 warning noise; F5 in part
  (StrataRuntime reports unknown arguments and `--help`).
- Rendering: L5 (skinned characters lie flat); side note `project.create {directory: ""}`.
- Roadmap open item "per-property overrides are later work" (the prefab edit mode stays later).
- Out of scope (wave 5): exported-game automation (`StrataRuntime --commands` with input and expectations), the
  animation preview UI, project settings and input actions, onboarding templates and an in-editor manual, the prefab edit
  mode. No host functions are appended by this stream.

## Files expected to be touched

- `Strata/src/Strata/Animation/AnimationPreview.*`, `AnimatorInstance.cpp`, `RestPose.h/.cpp` (new)
- `Strata/src/Strata/Asset/GltfImporter.*`, `TextureImporter.*`, `TextureCooker.*` (green flip)
- `Strata/src/Strata/Scene/Scene.h/.cpp` (`GetFixedStepCount`, pending steps on resume and stop; function-level),
  `Components.h` (`PrefabInstanceComponent::Overrides`), `ComponentRegistration.cpp` (its serialization and hidden fields),
  `PrefabOverrides.h/.cpp` (new), `PrefabInstances.*`, `ComponentInactiveReasons.cpp` (new), `SceneRegistration.cpp` (one
  call)
- `Strata/src/Strata/Reflection/Property.h`, `ComponentRegistry.*`, `PropertyBuilder.*` (`InactiveReason`)
- `Strata/src/Strata/Audio/AudioSystem.*` (`GetVoices`, one-shot counters and warnings)
- `Strata/src/Strata/Scripting/ScriptTypes.h` (`c_ScriptCallbackCount`)
- `Strata/src/Strata/Input/Input.h/.cpp` (`SetViewport` resolution; game-ui's file, function-level)
- `Strata/src/Strata/Core/CommandLineOptions.h/.cpp` (new), `StrataRuntime/src/RuntimeApplication.cpp`,
  `StrataEditor/src/EditorApplication.cpp`
- `StrataEditor/src/Editor/`: `Commands/EntityCommands.cpp`, `Commands/PlayCommands.cpp` (new, splits),
  `EditorSceneCommands.cpp`, `EditorStateCommands.cpp`, `EditorInputCommands.cpp`, `SimulatedInput.*`, `ScriptEdit.cpp`,
  `EditorAssetCommands.cpp` (`asset.info`, `material.*` warnings, `project.create`), `EditorPrefabCommands.cpp`,
  `PrefabSync.*`, `EditorContext.*` (preview ownership, `CommitEdit` overrides, stepping, running scene, build toast;
  function-level), `EditorViewport.*`, `ViewportRenderer.*`, `SceneBounds.*`, `ThumbnailCache.*`, `AssetEdit.*`,
  `EditorNotifications.*`, `CommandJournal.*` (`RecordEvent`, mutating activity, summaries), `EditorCommands.*`
  (`MayDefer`, `editor.batch`), `EditorCommandRunner.*`, `UndoStack.*` (groups), `EditorAudioCommands.cpp` (new, or in an
  existing file), `EditorGameWindow.*`
- `StrataEditor/src/Panels/`: `GameViewPanel.*` (new), `ViewportPanel.*`, `InspectorPanel.*`, `SceneHierarchyPanel.cpp`,
  `ActivityPanel.*`; `StrataEditor/src/UI/`: `PropertyWidgets.*`, `MaterialEditor.*`, `RecoveryDialog.*`, `Notifications.*`,
  `Icons.h` (if an icon is generated); `StrataEditor/src/EditorLayer.*` (`RegisterBuiltinPanels`, `c_LayoutVersion`, File
  menu, status pills)
- `StrataCLI/src/CLI/McpServer.*`, `McpToolsets.*` (new), `CliApplication.cpp` (`--toolsets`)
- `StrataTests/src/`: `Animation/SkinnedGltf.h`, `Animation/AnimationSystemTests.cpp` (`Animation.Preview`), `Renderer/GPUAnimationTests.cpp`,
  `Asset/GltfImporterTests.cpp`, `Asset/TextureImport*Tests.cpp`, `Editor/PrefabWorkflowTests.cpp`,
  `Editor/EditorInputTests.cpp`, `Editor/EditorGameFlowTests.cpp`, `Editor/EditorCommandTests.cpp`,
  `Editor/EditorEntityCommandTests.cpp`, `Editor/EditorAudioTests.cpp`, `Editor/EditorUITests.cpp`,
  `Editor/EditorAuthoringUITests.cpp` (`Editor.UI.Inspector`, `Editor.UI.Hierarchy`, `Editor.UI.AssetInspector`),
  `Editor/EditorViewportTests.cpp`, `Editor/GPUEditorViewportTests.cpp`,
  `Editor/GPUThumbnailTests.cpp`, `Editor/RecoveryTests.cpp`, `Editor/NotificationTests.cpp`,
  `Editor/CommandJournalTests.cpp`, `Editor/EditorPerfTests.cpp` (`PrefabApply10kInstances`), `CLI/McpServerTests.cpp`,
  `CLI/McpToolsetsTests.cpp` (new), `Scripting/*` (callbacks listed)
- `StrataTests/Editor/FeatureCommands.json.in`, `TetrisCommands.json.in`, `SmokeCommands.json.in`, `LauncherCommands.json`,
  `InputSequenceCommands.json.in` (new), `StrataTests/CMakeLists.txt` (new CTests in its own block),
  `StrataTests/Perf/Budgets.json`, `StrataTests/FeatureTest/Assets/Scenes/Feature.stscene` (+ texture `.meta` files if the
  new key is written)
- `Docs/Architecture.md`, `AGENTS.md`, `Docs/Manual/Prefabs.md` (new), `Docs/Manual/GameView.md` (new), `Docs/Roadmap.md`
  (one open item removed), `.claude/skills/strata-make-a-game`, `strata-editor-automation`, `strata-scripting`

## Merge notes and conflict risks

Merges **sixth, last**. The integrator re-runs Smoke, Tetris, Export, Scripts, Install and the feature runners after it.

**What this stream guarantees the others in the meantime.**
- Until it merges, `play.step` answers at once and input durations count render frames: other streams' command scripts
  keep `editor.wait` after `play.step` and must not rely on the new semantics. After it merges, `play.step {frames}` and
  `input.* {frames}` stay accepted (aliases of `steps`), and in headless runs at 60 frames per second results are the same
  (one step per frame), so scripts the others wrote keep passing; this stream updates the shared ones.
- The MCP tool list may exceed 128 tools on the integration tree until this stream merges; integration agents use
  `StrataCLI call`.
- `editor.status {sections: ["all"]}` returns today's whole answer, so a test another stream wrote against a section
  keeps working by asking for it (this stream adds `sections` in its rebase wherever a merged test or command script
  reads beyond the compact form, such as world-io's `memory.peakPrivateMB` gate and world-partition's `world` section).
- `AnimationPreview` keeps its API; `ISkinningPoses` is unchanged; the Renderer is not edited for rest poses.

**engine-modules** (merges first).
- It moves components into their modules (Animator and AttachToJoint to Animation, colliders and the character
  controller to Physics, lights, sky, post-process and text to Renderer, AudioSource to Audio, Script to Scripting).
  `SkinComponent` and `MeshRendererComponent` keep their types in `Scene/Components.h` (Animation and Renderer register
  them). Rebase includes in `SceneBounds`, `SceneHierarchyPanel` presets and the inactive reasons.
  `PrefabInstanceComponent` and its override list stay in Scene. The `InactiveReason` rows move next to their
  components' new registrations in this stream's rebase (item 8).
- Its `ComponentInfo::ExtraReferences::Equals` replaces `Prefab.cpp`'s `ScriptsMatch`: the per-property comparison and
  `ComputeOverrides` compare script fields through it (or through the Script component's extra JSON), never by naming
  `ScriptComponent` in Scene-layer code.
- `CreateApplication` returns an `ApplicationLaunch` (`Engine/EntryPoint.h`): `RuntimeApplication.cpp`'s argument parsing
  (`--help`, exit code 3) returns `std::nullopt` with `GetStartupExitCode()` set, as the editor's does.
- Per-layer libraries: `Core/CommandLineOptions` goes in Core; `McpToolsets` in StrataCLI. New test suites fall into a
  Core shard (`Editor.*`, `CLI.*`); new CTests list their programs' modules.
- Soft system constraints: none needed here.

**world-io** (second).
- `scene.save` becomes deferred: it must carry `MayDefer`, `editor.batch` refuses it, and the skills say so. Its other
  deferring commands carry the flag too (`c_DeferringCommands` union).
- `editor.status`'s compact form includes `assets.scanning` (`EditorStatus` section union); its commands go into the kind
  lists and the toolset table (`world.*` maps automatically; `scene.*` additions go to `scene`).
- `McpServer`: world-io changes the launch wait, this stream the tool set and definitions; keep both in their own
  functions.
- `PrefabInstanceComponent`'s overrides serialize through `SerializeExtra`: world-io's cooked scenes store JSON-only
  extras as MessagePack (common.md section 12), so the list needs no binary hook; check with a cooked Feature scene.
- `Prefab.*` read path and `EntityPrototype` are world-io's: this stream does not change instantiation.
- Its command splits (item 10) keep world-io's `WaitsForAssets` flags on every command they move.
- `MayDefer` goes on world-io's deferring commands (item 9).

**world-partition** (third).
- `Scene.cpp` and `EditorContext` function-level (sliced unloads, cells in edit mode against stepping, pending steps,
  the running scene). `DescribeRunningScene` gains the world's cells (this stream, in its rebase).
- `entity.get {world}`, `entity.find {box}` and bounds work on cell entities; `play.step` and `input.sequence` run a
  world with streaming cells (this stream's check in its rebase, on the feature world). The Game view plays a world.
- In an edited world, new top-level entities go into the persistent scene unless an edit layer is picked
  (`world.edit`), so this stream's commands and tests on the FeatureTest copy (now a world's persistent scene) keep
  their meaning.
- `world.*` and `physics.stats` map to the `world` toolset and the kind lists.

**gpu-driven-rendering** (fourth).
- Rest-pose palettes reach its GPU path through `ISkinningPoses` like play-mode poses (this stream's check in its
  rebase: `GPU.Animation`'s rest-pose case on the GPU path, a culled and indirect-drawn skinned proxy). Posed bounds are computed in `SceneBounds` with `MeshSkinBinding`; if the geometry pool
  moves the skin binding off `Mesh`, keep `Mesh::GetSkinBinding()` (CPU data) or give `SceneBounds` the new accessor.
- `EditorViewport` gets a third renderer (this stream) while it adds GPU lines to the stats overlay and `renderStats` to
  `viewport.capture`: function-level. The Game view's renderer reports GPU stages and its `renderStats` come from
  `CommandUtils::DescribeRenderStats` (this stream's check in its rebase).
- `GltfImporter`'s version: one bump if both change it.

**game-ui** (fifth).
- `input.sequence`'s `command` events schedule `input.click {entity}`; verify with a button in the feature canvas (this
  stream's test, in its rebase). `ui.*` maps to the `ui` toolset; its commands go into the kind lists.
- `input.click {frames}` becomes `steps` (with `frames` as an alias) like the other input commands, in this stream's
  rebase.
- The Game view's resolution drives canvas scaling through the renderer's viewport size and `IGameWindow::GetState`.
  `Input::SetViewport`'s resolution parameter is this stream's one change in game-ui's `Input` files. The Game view
  takes over setting the input viewport every play frame from `ViewportPanel::UpdateGameInput`. With a resolution given,
  `Input::GetViewportSize()` returns it (game pixels), because game-ui takes the UI's input screen from it: hit testing
  then uses the size that is drawn.
- The Game view renders a camera-less scene with UI as `GameRenderer` does after game-ui (its UI over black, through
  `SceneRenderer::HasOverlayContent`), and shows the missing-camera message only without UI.
- Chips live in the toolbar strip above the image, so they never cover canvases; this stream's UI-harness test adds
  game-ui's rects (`GameUIScene::GetVisibleRects`) in its rebase.
- The Inspector's inactive rows include game-ui's UI properties (item 8).
- The Hierarchy preset table is the union of UI presets and primitives with colliders. Its `UIRect` drawer in
  `InspectorPanel` stays in its own function. Class callbacks it appends show in `script.status` through
  `c_ScriptCallbackCount` (it updates the constant with the enum).
- If game-ui migrates Tetris's HUD to a canvas, its `TetrisCommands.json.in` changes merge before this stream's contract
  updates.

**Unions at merge:** `Feature.stscene` (the Crate override beside world-partition's world and game-ui's canvas),
`Budgets.json`, `CommandJournalTests.cpp` kind lists and `c_DeferringCommands`, the MCP toolset table, the skills,
`AGENTS.md`, `Docs/Architecture.md`, `EditorLayer::RegisterBuiltinPanels` and `c_LayoutVersion` (5, once), the status bar
pills, `StrataTests/CMakeLists.txt`, `Editor/UndoStack.*` (this stream's groups beside world-io's save points and
world-partition's observer list).

## Answers from the planner (cross-check of 2026-10-13)

1. **Auto exposure rows:** accepted: `AutoExposureMinEV`, `AutoExposureMaxEV` and `AutoExposureSpeed` are inactive while
   Auto Exposure is off; `Exposure` is never inactive.
2. **Suite name:** `Editor.Prefab`, the existing suite.
3. **Game view placement:** a tab beside the Scene view in the default layout 5; Play brings it forward. The side-by-side
   screenshots use a `--layout` file. No default split.
4. **The Scene view while playing is an editor view;** captures keep `ViewportCameraSource::Automatic` (the game's
   camera while playing): confirmed.
5. **Instantiation unchanged:** your reading is the intended one. New instances have empty override lists; values given
   to `prefab.instantiate` are applied after the fast path and recorded by the commit; scenes keep storing instances in
   full.
6. **Pending steps are dropped** on resume and stop, and a waiting `play.step` fails with `stepsRun`: confirmed.
7. **`frames` as an alias of `steps`** (input and `play.step`): accepted. Headless results are identical; the skills and
   schemas say windowed scripts now count game updates.
8. **`MayDefer` on other streams' commands:** they cannot set a flag that does not exist until this stream merges. Each
   lists its deferring commands in its report's merge notes (their specs now say which), and this stream sets the flag
   on all of them in its rebase (item 9). common.md section 8 records the rule and the scan-deferral interplay.
9. **The MCP core set** (27 commands plus 5 generic tools, `strata_list_methods` folded into `strata_describe`):
   confirmed. `input.click`, `ui.*` and `world.*` stay out of `core` and map to `play`, `ui` and `world`.
10. **Additions from the cross-check:** compact `editor.status` keeps world-io's `saving`; the `InactiveReason` rows
    move next to the moved registrations in this stream's rebase and cover game-ui's UI properties; the checks this
    stream needs from earlier streams (game-ui's rects, `input.click` in sequences, rest poses on the GPU path, worlds)
    are done in its rebase, not by the integrator.
