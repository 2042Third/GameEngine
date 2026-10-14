# authoring-and-playtest spec — STATE (for a successor)

Started 2026-10-13. Task: write specs/authoring-and-playtest.md (format of wave3/specs/animation.md).
Read-only on the repo; code at <repo>/.worktrees/verify.

## Progress
- [ ] read plan.md, common.md, wave3 animation.md format
- [ ] read audits (person, agent, rendering q02/q03)
- [ ] code reading per item (1)..(9)
- [ ] write spec

## Notes (facts found in code)
- [x] read plan.md, common.md, wave3 animation.md format; person/agent audit STATE + key shots (13_man_side_edit/play, q02/q03, 14, 15, 20, 21, 30_B crop)
### (1) rest pose
- RenderScene::EvaluatePose: no pose from ISkinningPoses -> proxy unposed (raw mesh = bind pose in mesh space). Renderer may not include Animation.
- AnimationPreview (Animation/AnimationPreview.*) is the edit-scene ISkinningPoses; created lazily by EditorContext::GetAnimationPreview(); Update() in EditorContext::Update before viewports. Requests via AssetManager::GetActive().
- Rest pose = Skeleton::ComputeModelTransforms(GetRestPose()) (Evaluate with m_Count 0); ancestors are joints so Z_UP/Armature orientation included.
- ThumbnailCache::BuildScene makes Scene "Thumbnail" w/o ISkinningPoses -> bind pose. SceneBounds::GetRenderableLocalBounds uses Mesh::GetBounds (bind). RenderScene::ComputePalette(static) gives posed bounds from MeshSkinBinding::JointBounds.
### (2a) import scale: GltfImporter.cpp BuildModel sets root Scale (line ~1867); root transform belongs to instance (PrefabInstances.h doc). Cameras/lights ranges scaled already. PrefabSync::Propagate has previous+snapshot; CatchUp lacks previous -> migration needed for legacy instances (root scale == old s -> double scale).
### (2b) TextureImporter::GetDefaults suffix EndsWith list; "aerial_rocks_02_nor_gl_1k" fails. Version 4 now.
### (3) play.step: EditorStateCommands play.step immediate; Scene::Step adds m_StepFrames, 1 step/frame while paused; SetPaused(false) keeps m_StepFrames (bug); ReplaceRuntimeScene carries steps. DescribePlayState = {state, paused}. No fixed-step counter in Scene (add GetFixedStepCount).
- scene.info names edit scene; running scene asset = EditorContext::m_RuntimeSceneAsset.
- entity.get/find/delete/rename in EditorSceneCommands.cpp (RegisterSceneCommands 897 lines -> split rule).
### (4) SimulatedInput holds count Input::GetFrameIndex (render frames). Input suspended while paused w/o step.
### (5) McpServer: tools = every editor method + 4 generic (strata_status, strata_launch_editor, strata_list_methods, strata_call). editor.commands exists (EditorCommands.cpp). editor.status has status providers (game, recovery, assets, automation, editor).
### (6) ResolveViewportView Automatic -> scene camera in Play; ViewportPanel m_GameView; UpdateGameInput sets Input::SetViewport/SetEnabled; EditorViewport has m_Renderer + m_CaptureRenderer. EditorGameWindow (IGameWindow) reports game view size.
### (7) PrefabInstanceComponent {Prefab, PrefabEntityID, PrefabVersion, SyncedHash}; ComponentInfo has SerializeExtra/DeserializeExtra. prefab.apply updates only instances with Compare(...).IsEmpty(). PrefabSync::Propagate same.
### (8) presets SceneHierarchyPanel GetPresets(); RecoveryEntry::Discarded exists; HasOffer(leftBefore) uses WasDiscardedHere; errors pill = console unread errors; EditorNotification has Group (merge) but no replace-by-id.
- NOTE: PostProcess Exposure is compensation in BOTH modes (SceneRenderer.cpp ~904): brief's "Exposure inactive while Auto Exposure on" is wrong -> AutoExposureMin/Max/Speed inactive while AutoExposure off.
- [x] code reading done (13:xx). Decisions taken (write into spec):
  - rest pose: AnimationPreview gains RestPoses spec flag; per-skeleton cache; observe AnimatorComponent signals; no per-frame lookups; attachments untouched by rest poses; UpdateRenderTransforms only on version change. EditorContext always creates it for edited scene; ThumbnailCache per thumbnail scene (c_Version 1->2). SceneBounds computes posed bounds itself (no Renderer/* change).
  - import scale baked; migration: PrefabSync Propagate multiplies root scale new/old; CatchUp legacy rule (root scale == import Scale -> 1).
  - play.step {steps, alias frames, wait}; Scene::GetFixedStepCount; SetPaused(false) drops steps; headless uncapped while steps pending.
  - input: steps/seconds, frames alias=steps; input.sequence pauses+steps 1/frame; events incl {command:"input.*"} for game-ui input.click.
  - MCP: StrataCLI/src/CLI/McpToolsets.*; core 27 cmds + 5 generic (strata_status, strata_launch_editor, strata_call, strata_describe, strata_toolsets); strata_list_methods folded into strata_describe.
  - editor.status {sections}; compact default.
  - Game view: Panels/GameViewPanel, id "Game"; Viewport title -> "Scene" (id stays); third renderer EditorViewport::GetGameRenderer; chips into a strip above the image (both views).
  - prefabs: PrefabInstanceComponent::Overrides on roots (SerializeExtra), maintained in EditorContext::CommitEdit by diff vs baseline; propagate per property; Scene/PrefabOverrides.*.
  - editor.batch needs EditorCommand::MayDefer flag.
  - split: entity.*/component.* -> Editor/Commands/EntityCommands.cpp; play.* -> Editor/Commands/PlayCommands.cpp.
- Questions for planner: Exposure claim wrong; suite name Editor.Prefab (not Prefabs); tab vs beside; legacy scale migration heuristic; editor.status compact breaks others' tests; Scene view in play = editor view; "compiled prototypes apply overrides" interpretation; steps dropped on resume; frames alias semantics; MayDefer flag touches other streams' registrations.
- [x] spec written (authoring-and-playtest.md); small fixes applied (Gamepad2 icon exists, frame cap mechanism, suite files). DONE except StructuredOutput.
