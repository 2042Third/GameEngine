# engine-modules spec: state notes

Started 2026-10-13. Spec target: wave4/specs/engine-modules.md. Read-only on repo; verified tree <repo>/.worktrees/verify (685aef7).
Scratch scripts: wave4/engine-modules-spec/ (suites.py aggregates Core durations per suite prefix).

## Progress
- [x] read plan.md, common.md, audit STATE + evidence, wave3 animation.md (format)
- [x] read code: Application, EntryPoint, BuiltinModules, Layers.json, allowlist, Strata/CMakeLists, vendor CMake,
      CMake/*.cmake, editor/runtime/CLI/tests CMakeLists, Components.h, ComponentRegistration.cpp, SceneSystem, regs,
      ScriptHostAPI reads, AudioSystem Doppler, SceneGizmos, AssetMeshProvider, ci.yml, presets
- [ ] write spec (next)

## Design decisions (so far)
- EngineModule in App layer: files Strata/Core/EngineModule.{h,cpp} (+registry), App Paths + Core Exclude.
  Hooks: GetName, GetDependencies (list of {Name, Required}), OnInit(EngineContext&)->bool, OnRelease (reverse, before
  JobSystem stops), OnShutdown (reverse, after), OnBeginFrame(FrameContext&)->Continue/Skip/Close (forward),
  OnUpdate(ts) after layers (forward), OnRenderUI(UIFrame&) (forward), OnEndFrame (REVERSE: Graphics presents last), OnEvent (before layers).
- Modules: Renderer/GraphicsModule (device, swapchain, Renderer frames, back-buffer captures, vsync), Audio/AudioModule
  (null-device advance), ImGui/ImGuiModule (overlay + Editor.UI stage; dep Graphics), Project/ProjectModule (OnRelease:
  AssetManager::SetActive(nullptr), Project::SetActive(nullptr); optional deps Graphics, Audio).
  Audio & Project MayInclude += App (downward, Notes).
- Composition root: EntryPoint.h moves to Engine layer (Strata/Engine/EntryPoint.h); CreateApplication returns
  ApplicationLaunch {Engine::ModuleRegistrationOptions Registration; std::function<Scope<Application>()> Create};
  main calls RegisterBuiltinModules(Registration) then Create.
- BuiltinModules.cpp: table of {Name, Register} guarded by ST_WITH_* (generated Engine/EngineFeatures.h);
  order Scene, Renderer, Physics, Audio, Animation, Scripting (keeps ComponentRegistry::GetAll order identical);
  ModuleRegistrationOptions::Exclude for tests.
- Soft constraints AfterIfPresent/BeforeIfPresent on SceneSystemDescriptor; Physics/Audio/Animation use them.
- Components: Scene keeps types ID, Name, Transform, Relationship, WorldTransform, Tag, Inactive, PrefabInstance, Camera,
  MeshRenderer (Physics+Animation read), Skin (RenderScene signals). MeshRenderer REGISTERED by Renderer, Skin by Animation
  (order). Renderer/RendererComponents.h: lights, SkyLight, PostProcess, Text. Physics/PhysicsComponents.h, Audio/AudioComponents.h,
  Animation/AnimationComponents.h (Animator, AttachToJoint), Scripting/ScriptingComponents.h (Script + ScriptEntry/FieldValue).
- Script move needs Reflection hook ComponentInfo::ExtraReferences {Visit, Set, Equals, VisitJson} used by SceneSerializer
  remap, EntityPrototype refs, SceneInstantiation, Prefab compare, AssetDependencies.
- Cross reads: Audio Doppler -> IBodyMotionQuery::GetSimulatingBody; host: IBodyMotionQuery::IsDynamicBody,
  IAudioPlayback::HasSource, IAnimatorControl::HasAnimator; gizmos -> Scene/ComponentGizmos.h registry (Physics fills).
- AssetMeshProvider -> Asset/MeshGeometry.h MeshGeometryRegistry (Renderer registers Mesh reader) => allowlist 0.
- CMake: CMake/StrataLayers.cmake reads Layers.json, globs->regex like LayeringAnalyzer, Strata_<Layer> + Strata::<Layer> alias;
  Strata (runtime umbrella INTERFACE) + StrataTooling (AssetPipeline, Network, ImGui). Engine MayInclude narrowed (no Network/ImGui).
  Jolt PUBLIC flags (/arch:AVX2, NDEBUG in Release) applied to all layers via StrataCodegen INTERFACE (keep codegen).
  NoTooling CTest via linker map (/MAP, -Map, -map) + configure-time closure check.
- Shards: StrataTests/CoreShards.json read by CMake + Testing.Shards test; fuzz CTest (Release/Dist) with STRATA_TEST_SWEEP=full.
- Optional modules: Audio/Physics off require STRATA_BUILD_EDITOR=OFF; CI job Linux no-audio-no-physics.
- CI: STRATA_COMPILER_CACHE option (ccache/sccache launcher; MSVC /Z7), PCH off in cached CI builds; Windows Debug job.

## Facts gathered
- Core Release per-prefix secs: Editor 84 (576 cases), Asset 45 (Pack 33), Scene 18, Network 11, Core 9, CLI 9, Scripting 9,
  Renderer 7, Runtime 3.4, Architecture 3, Physics 1.3, Audio .8, Engine .6, Project .4, Animation .2, PerfLab, ImGui, Math, Reflection ~0
- Runtime only uses Background priority for Scene::ReleaseInBackground.

## More decisions (pre-write)
- Gizmo hook on ComponentInfo (Reflection/ComponentGizmo.h: ComponentGizmoShape {Box,Sphere,Capsule,MeshBounds}, group Collider);
  SceneGizmos iterates GetAll() infos with a gizmo; Physics registers them (before the move: in Scene's ComponentRegistration).
- ExtraReferences hook on ComponentInfo {Visit, Set, Equals, VisitJson}.
- PropertyOptions helpers -> Reflection/PropertyOptions.h (Range, Slider, Color, WithTooltip, c_Unbounded).
- Strata.h umbrella removed (EditorLayer.h included it); Engine MayInclude narrowed to Core..Scripting (no Project/Runtime/Network/ImGui).
- Layers.json new keys: "Tooling": true (AssetPipeline, Network, ImGui), "Option": "STRATA_WITH_X"; App Paths += Strata/Core/EngineModule.*;
  EntryPoint.h -> Strata/Engine/EntryPoint.h + Strata/Engine/ApplicationLaunch.h. Audio, Project MayInclude += App.
- Targets Strata_<Layer> + alias Strata::<Layer>; Strata INTERFACE (runtime) and StrataTooling INTERFACE.
- Generated dirs stay build/<preset>/Strata/Generated and /EmbeddedShaders (lint script args).
- stb split into per-header objects; keep rect_pack with truetype in one object.
- Shards: StrataTests/CoreShards.json; suites *.Sweep run sampled; StrataTests.Fuzz (Release/Dist) full.
- package-label tests RESOURCE_LOCK toolchain.
- Test files in other folders using physics/audio (~20) guarded with #if ST_WITH_X; module folders excluded by CMake.
- ST_WITH_* as PUBLIC compile definitions on Strata_Core (not a header).
- Writing spec now (13 Oct).

## Status
- 13 Oct: spec WRITTEN to wave4/specs/engine-modules.md (verified names: GetPrimaryCameraEntity, AnimationSystem implements
  IAnimatorControl, AudioSystem's PlaybackService is IAudioPlayback, Tetris scene Samples/Tetris/Assets/Scenes/Main.stscene).
- Remaining: StructuredOutput to orchestrator (summary, risks, questions). Nothing else.
