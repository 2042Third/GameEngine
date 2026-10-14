# Wave 4: what every agent needs to know about Strata after wave 3

Read this file and AGENTS.md before your own spec. Your spec overrides nothing here. Where a spec and this file
disagree, say so in your report to the orchestrator instead of guessing.

The quality bar is the end result: a professional, fast, stable engine and editor that the owner judges by running it.
- Regressions that can be fixed are fixed in code, not accepted.
- Nothing is "done" until its tests, docs and screenshots say so.

Facts here were checked against the code of main 685aef7 (`.worktrees/verify`). Locate code by symbol: line numbers
move.

## 1. Where you start

### Base

The step-0 tip of `main` (see `wave4\plan.md`, "Before branching"). It is 685aef7 plus:
- 60fe71f: CI fixes for GCC 14 and Apple Clang 15;
- 6b23652: CI on `ci/` branches;
- 4762916: CI annotations that name failed test cases;
- `agent/wave3-finish`: wave 3's open perf items;
- `agent/release`, which adds:
  - installed editors with their own SDK and toolchain detection (`Editor/ScriptToolchain`,
    `ScriptBuildSettings::Detect`/`FindToolchain`, a `FindingToolchain` build phase);
  - release archives (`CMake/StrataPackaging.cmake`, CTest `StrataEditor.Install`);
  - `release.yml` with version 0.3.0 and the Dist presets;
  - the Dist editor build fix;
  - the macOS 14 deployment target;
- `agent/wave4-step0`:
  - New Script needs one build (the source list is in the script build's configure stamp);
  - CI is green on Linux and macOS, and runs a Windows Dist job;
  - the `Scene::GetAssetManager()` seam;
  - the README;
  - AGENTS.md trimmed to rules plus links (descriptions moved into Docs/Architecture.md).

The orchestrator gives you the exact commit and the path of the base's Release and Debug builds (your A/B baseline).

### What wave 3 changed

Six streams merged in this order: mip-streaming, scene-streaming, animation, content-and-prefabs,
gameplay-and-scripting, agent-activity. Docs/Roadmap.md "Wave 3 landed" has the numbers. What you build on:

- **Mip streaming.**
  - The asset layer knows no graphics API. `Layers.json` forbids NVRHI and Vulkan headers in Asset
    (`MayNotIncludeExternal`).
  - Assets record uploads into an `AssetUploadContext` (`AssetFinalizeContext::Upload`). The renderer provides it
    through the engine service `IAssetGraphics` (`Asset/AssetUploadContext.h`).
  - Every upload goes through `GPUUploadQueue` on `StagingPool`. Budgets (`AssetResidencyBudgets`):
    - 16 MiB and 2 ms of upload steps per frame (`UploadBytesPerFrame`, `UploadMsPerFrame`);
    - 4 ms of finalization (`FinalizeMsPerFrame`);
    - 128 MiB in flight;
    - 32 MiB of staging;
    - GPU textures 50% and buffers 15% of the device budget.
    - AGENTS.md still says "64 MiB of finalization per frame": the code says 16 MiB of uploads.
  - Textures load as their mip tail (`Texture::c_ResidentTailDimension` = 32). Each manager's `TextureStreamer` (an
    `AssetManagerExtension`) raises and drops levels to the texel density the views draw them at
    (`SceneRenderer::GetVisibleMaterials`, `VisibleMaterialUse`). Levels unneeded for `c_DropDelayFrames` (120 frames)
    drop.
  - `asset.stats` and the Streaming panel report mips.
- **Scene streaming.**
  - Scenes load in the background (`SceneLoader::LoadAsync`, states `LoadingScene`, `LoadingDependencies`,
    `Preparing`, `Ready`, `Failed`, `Cancelled`), in the editor and in games.
  - Scenes that are let go are destroyed off the frame (`Scene::ReleaseInBackground`).
  - Prefabs and scenes compile into prototypes (`Scene/EntityPrototype`, `Prefab::Instantiate`, `InstantiateMany`).
  - Additive sub-scenes (`Scene::LoadSubScene`, `BeginAdditiveLoad`, `UnloadSubScene`):
    - created in slices of `SceneSettings::MaxEntitiesInstantiatedPerFrame` (5,000) and a time limit;
    - their top-level entities carry the runtime-only `SubSceneComponent`;
    - an unload is one `DestroyEntities` call.
  - The editor loads sub-scenes only into a running game (`scene.loadAdditive`).
  - Play mode copies the scene storage by storage on the job threads (`Scene::Copy`).
  - A load prepares render proxies over frames before the swap (`RenderScene::PrepareLoadedScene`).
  - Manifest version 4 adds `LoadingScene`.
- **Animation** (`Strata/src/Strata/Animation/`, its own layer):
  - assets: `Skeleton`, `AnimationClip`, `AnimationController` (`.stanimator`);
  - components `Skin`, `Animator`, `AttachToJoint`;
  - the "Animation" scene system (after Scripting, before Physics), evaluated on the job threads;
  - poses reach the renderer only through the scene service `ISkinningPoses`;
  - `InstanceData::SkinPalette` indexes a joint buffer, and posed bounds go into the proxies;
  - `AnimationPreview` provides `ISkinningPoses` for the edited scene (`animation.preview`);
  - without a pose, a skinned mesh shows its raw bind pose (the person audit's critical finding, which
    authoring-and-playtest owns).
- **Content and prefabs:**
  - Assets go to the project's trash as one undo step (`AssetTrashAction`), with references (`asset.findReferences`).
  - `asset.import` never imports in a frame. It brings a glTF's buffers and images along, and handles folders and
    meshopt.
  - Content Browser v2.
  - Thumbnails (`ThumbnailCache`): textures from their mip tail by byte range, the rest rendered offscreen.
  - Linked prefab instances (`PrefabInstanceComponent` with `PrefabVersion`/`SyncedHash`, `Scene/PrefabInstances`,
    `Editor/PrefabSync`): status, apply, revert and unpack. Changes reach only unmodified instances, whole.
  - Placement on surfaces (`EditorPlacement`, `SceneRaycast`, `entity.dropToGround`).
- **Gameplay and scripting:**
  - character controllers (`ICharacterControl`);
  - collision layers (`SceneSettings::PhysicsLayers`, per scene);
  - gamepads, cursor modes, the game window (`IGameWindow`) and save files (`IGameSaves`, `Core/SaveFiles`);
  - scripts made, built (a per-user build directory for deep projects), opened in an IDE and located from the editor;
  - Play builds stale scripts first;
  - the Problems panel;
  - `Docs/Manual/Scripting.md`.
- **Agent activity:**
  - The seven commands that replace the edited scene refuse its unsaved changes without `force`.
  - Autosave and recovery (`Editor/SceneRecovery`, `RecoveryDialog`).
  - The activity journal (`Editor/CommandJournal`) of every command, with its kind; the Activity panel; replay.
  - Agent presence in the status bar and the viewport.
  - Notifications (`Editor/EditorNotifications`: `Post`, groups, at most 4, toasts in `UI/Notifications`).
  - Captures that wait for what they show.
  - Out-of-range values refused.
  - `--help`, `--version`, and exit code 3 for unknown editor arguments.

### Known open items

Docs/Roadmap.md "Open items and deferrals" has the full list. The ones a stream owns are named in its spec.

### Your worktree and branch

The orchestrator makes them: `<repo>\.worktrees\<key>` on `agent/<key>`.
- Build in your worktree's own `build/windows` (`cmake --preset windows`, then `cmake --build build/windows --config
  Debug --parallel`) so the ctest presets work.
- Never build in another worktree's or the main checkout's build directory.
- Re-run the configure step after adding or removing source files or shaders.
- Never touch `main`, other worktrees, the main checkout's build, `git config` or remotes.
- Never push. A stream whose spec needs a CI run on Linux and macOS (engine-modules) asks the orchestrator to push
  `ci/<key>` and reads the result with `scratchpad\ci_status.py <sha>` and `ci_annotations.py <sha>`.

### No new third-party code

Everything wave 4 needs is vendored:
- NVRHI: timer queries, `drawIndexedIndirect`/`drawIndexedIndirectCount`. The device enables `multiDrawIndirect` and
  `drawIndirectCount` where supported.
- meshoptimizer: `meshopt_simplifyWithAttributes`.
- nlohmann json: `sax_parse`.
- zstd: the sources of `lib/dictBuilder` are vendored, so the tooling-only `zstd_compress` target may add them.
- stb, cgltf, Jolt, miniaudio, EnTT, glm, spdlog, imgui and ImGuizmo (editor only), glslang, doctest, tracy.

If you believe you need a new submodule, stop and report.

### The machine

Windows 11, i7-13700K (8 performance and 8 efficiency cores, 24 threads), RTX 5090, MSVC (VS 2026), CMake and Ninja
from MSYS2, no Vulkan SDK. The validation layer: `$env:VK_ADD_LAYER_PATH =
"<repo>\build\_tools\Vulkan-ValidationLayers\build\layers\Release"`. Six agents share it during wave 4.

It has rare faults:
- compiler crashes (`c1xx.dll`/`c2.dll` access violations, MSVC C1001, D8040) under full load;
- two MEMORY_MANAGEMENT bug checks (0x1A) on 2026-10-12.

What to do:
- If a compile crashes, retry it once before suspecting code.
- If an engine process crashes and you cannot reproduce it in 10 or more attempts, report it with its evidence (log,
  exit code, what ran) instead of chasing it for hours. A crash you can reproduce is a bug to fix.

### Commits

- One logical change per commit.
- An imperative subject under 72 characters, and a body with what and why (with measured values for budget changes).
- The attribution lines the orchestrator gives you.
- Walk AGENTS.md's review checklist on `git diff --staged` before each commit.

## 2. Layers, registries and modules

- **Layers.** `StrataTests/Architecture/Layers.json` assigns every file under `Strata/src` to a layer.
  `Architecture.Layering` fails on a forbidden include, a file in no layer, or a stale allowlist line. Layers, with
  what each may include:

  | Layer | May include | Notes |
  | --- | --- | --- |
  | Core | nothing | |
  | Input | Core | |
  | App | Core, Input | `Application`, layers, `Window`, `EntryPoint.h`, GLFW, `VulkanLoader` |
  | Asset | Core | no NVRHI or Vulkan headers |
  | Reflection | Core, Asset | |
  | Scene | Core, Asset, Reflection | |
  | Renderer | Core, Input, App, Asset, Reflection, Scene | |
  | Physics | Core, Asset, Reflection, Scene | |
  | Audio | Core, Asset, Reflection, Scene | |
  | Animation | Core, Asset, Reflection, Scene | |
  | Scripting | Core, Input, Asset, Reflection, Scene | |
  | Project | Core, Asset, Reflection | |
  | Runtime | Core, Input, App, Asset, Reflection, Scene, Renderer, Physics, Audio, Scripting, Project | not Animation |
  | Network | Core | |
  | ImGui | Core, Input, App, Renderer | |
  | AssetPipeline | Core, Asset, Reflection, Scene, Renderer, Audio, Animation | `EditorAssetManager`, importers, `TextureCooker`, `BC7Encoder`, `AssetPackWriter`, `AssetDependencies` |
  | Engine | everything but AssetPipeline | |

- **New layers.** A new folder under `Strata/src/Strata` needs a layer. A new layer goes in the list below the layers
  that include it, with `Notes`. Wave 4 adds `World` (world-partition) and `GameUI` (game-ui). A proven downward
  dependency may widen `MayInclude`, with the reason in `Notes`.
  - engine-modules narrows `Engine`'s `MayInclude` to the runtime layers (no Project, Runtime, Network or ImGui).
    world-partition and game-ui each append their layer to `Engine`'s `MayInclude` (a union at merge); world-partition
    also appends `World` to `AssetPipeline`'s. No other layer includes `World` or `GameUI`.
  - engine-modules also deletes the umbrella header `Strata/src/Strata.h`: no stream adds includes to it.
- **The allowlist.** `LayeringAllowlist.txt` holds exactly `MaxAllowlistEntries` (8) lines:
  - 7 lines of `Core/Application.cpp` (engine-modules removes them);
  - `Physics/AssetMeshProvider.cpp -> Renderer/Mesh.h` (Mesh v2, or engine-modules by inverting it).

  Never add a line or widen `MayInclude` for a sideways or upward include. Invert the dependency instead: an interface
  in the lower layer, a registry, a scene service in `Scene/SceneServices.h`, or an engine service in
  `Core/EngineServices.h`.
- **Composition root.** `Engine::RegisterBuiltinModules` (`Engine/BuiltinModules.cpp`) opens the registries, then calls:
  1. `RegisterSceneModule`, `RegisterRendererModule`, `RegisterScriptingModule`, `RegisterPhysicsModule`,
     `RegisterAudioModule`, `RegisterAnimationModule`;
  2. `options.AssetPipeline` (importers, handed in by tools; StrataRuntime does not link them);
  3. `options.Extra`;

  and then freezes the component registry. A new module adds its `Register<Module>Module` (in
  `<Module>/<Module>Registration.cpp`) there, in dependency order. engine-modules reshapes this file into a module
  list; keep your addition to one line.
  - **Wave-4 rows go at the end, World then GameUI:** after `RegisterAnimationModule` on the base, and after Scripting
    once engine-modules' table (Scene, Renderer, Physics, Audio, Animation, Scripting) lands. That keeps
    `ComponentRegistry::GetAll()` in today's order with the new components after `Script`.
- **Components.** Today every built-in component is a plain struct in `Scene/Components.h`, registered in
  `Scene/ComponentRegistration.cpp`. There are 27 of them, among them ID, Name, Transform, Relationship, Tag, Inactive,
  PrefabInstance, Camera, MeshRenderer, the lights, Sky, PostProcess, Text, RigidBody, the colliders,
  CharacterController, AudioSource and AudioListener, Skin, Animator, AttachToJoint and Script.

  **Wave-4 rule:** a new component lives in the module that owns it. Declare it in `<Module>/<Module>Components.h` and
  register it in that module's `Register<Module>Module`: `ComponentRegistry::Register` works from any registration
  function before the freeze. Give it a `Category` for the Add Component menu. Never add a component to
  `Scene/Components.h`: engine-modules moves the existing ones into their modules this wave. Follow
  `.claude/skills/strata-add-component/SKILL.md`, except where it names `Components.h`.

  Where engine-modules puts them (rebase your includes onto this): Scene keeps the types and registrations of ID, Name,
  Transform, Relationship, Tag, Inactive, PrefabInstance and Camera, and the **types** of `MeshRendererComponent` and
  `SkinComponent` (sibling layers read them), which Renderer and Animation register. Lights, Sky, PostProcess and Text
  go to `Renderer/RendererComponents.h`; RigidBody, the colliders and CharacterController to
  `Physics/PhysicsComponents.h`; AudioSource and AudioListener to `Audio/AudioComponents.h`; Animator and AttachToJoint
  to `Animation/AnimationComponents.h`; Script to `Scripting/ScriptingComponents.h`. Generic Scene-layer code reaches
  a component's extra data (today only Script's) through `ComponentInfo` hooks, never by naming the type:
  `ExtraReferences` (entity and asset references: engine-modules) and `ExtraBinary` (the cooked scene format: world-io).
- **Scene systems.**
  - Made with `MakeSceneSystemDescriptor<T>`.
  - Ordered by `After`/`Before`. Today a constraint that names an absent system refuses the registration:
    engine-modules adds soft constraints (`AfterIfPresent`/`BeforeIfPresent`). A stream that merges after it turns its
    own constraints toward another module's system into soft ones in its rebase: the "World" system runs
    `AfterIfPresent {"Scripting"}`, `BeforeIfPresent {"Physics"}`; the "UI" system runs `BeforeIfPresent {"Scripting"}`
    (before the scripts, so a click the UI takes is hidden from them in the same frame).
  - Each has a `ProfileStage` (`Scene.<Name>`, a frame profiler stage).
  - Systems of different modules talk through scene services (`Scene::RegisterService<T>`/`GetService<T>`, null when
    absent): `ICollisionEventSource`, `IBodyMotionQuery`, `IBodyControl`, `IPhysicsQueries`, `ICharacterControl`,
    `IAudioPlayback`, `ISkinningPoses`, `IAnimatorControl`, `IAnimationEventSource`.
  - Engine services (`Core/EngineServices.h`): `IAudioOutput` (declared in `SceneServices.h`), `IGameWindow`,
    `IGameSaves`, `IAssetGraphics`.
- **Loaders and importers.** Loaders register in the owning module's registration function. Importers register in
  `RegisterAssetPipeline` (`Asset/AssetImporters.cpp`).
- **Scenes keep unknown components.** Scenes keep the components of modules a build lacks (`UnknownComponentsComponent`)
  through copies, prefabs and undo.
- **The asset manager.** New code reaches it through the scene where it has one (`Scene::GetAssetManager()`, the
  step-0 seam that returns the active manager until wave 5's scene contexts), never through new
  `AssetManager::GetActive()` calls in scene systems, loaders or the renderer.

## 3. Scene contracts

- **The transform contract.** A direct write to `TransformComponent` fields signals the change:
  `MarkModified<TransformComponent>()` or `registry.patch`, or `Scene::MarkTransformChanged`/`MarkTransformsChanged`
  where listeners must not hear it.
  - The same holds for `NameComponent::Name`, `TagComponent::Tag` and `MeshRendererComponent` fields.
  - Values written right after `AddComponent` need no signal.
  - Debug builds assert the scene caches (`ValidateWorldTransforms`, `ValidateHierarchy`) after every
    `OnUpdateRuntime`/`OnUpdateEditor`, and the render proxies (`RenderScene::Validate`) after every `Sync`.
  - Tests use `CheckSceneCaches` (`Scene/SceneTestUtils.h`).
- **Hierarchy changes** go only through `Scene` (`SetParent`, `SetSiblingIndex`, `PlaceEntities`, `DestroyEntities`),
  many entities in one batch call.
  - `Scene::c_MaxEntities` is 1,048,575 live entities per registry.
  - `Scene::Copy` keeps `GetSourceAsset`/`GetLineageID`: physics remembers capacities by lineage.
- **`SceneSettings`** (gravity, `FixedTimestep`, `MaxFixedStepsPerFrame`, `PhysicsCapacity`,
  `MaxEntitiesInstantiatedPerFrame`, `PhysicsLayers`):
  - serialized and changed by `scene.setSettings` (one undo step);
  - shown by the Inspector with nothing selected, in closed sections with summaries.
  - Append fields only, and keep the nothing-selected view cheap.
- **Scene documents** (`.stscene`, JSON, header `{"Strata": {"Format", "Version": 1}}`) are written by
  `SceneSerializer` with tab indentation (`JsonUtils::Dump(json, 1, '\t')`) and read whole into a DOM:
  - the editor's save does this on the main thread (`EditorContext::SaveScene`/`SaveSceneAs`);
  - a load does it on the loading worker.

  world-io changes all three: streamed writes, streamed and cancellable reads, saves off the frame, and a binary form
  for packs.
- **Play mode.**
  - It plays a `Scene::Copy` of the edited scene (`EditorContext::StartRuntime`).
  - Paused play advances by `play.step` (`Scene::Step` adds pending steps that run one per frame).
  - Requests to quit or switch scenes (`Scene::RequestQuit`/`RequestSceneLoad`) are honored by the owner after the
    update.
  - Time dilation applies outside scripted runs.

## 4. Assets, residency, packs, imports

- **Requests.** Hold `AssetHandle`s across frames, and request (`GetAsset`) every frame you use an asset.
  - Never keep `Ref`s: they block eviction.
  - Pin what gameplay must keep (`AssetPin`).
  - Never block on a load. `LoadAssetSync` is for tools and tests.
- **State changes.** Every change of an asset entry's state goes through `SetStateLocked`, `AddEntryLocked`,
  `RemoveEntryLocked` and `Pin`/`Unpin`. The per-state counts `GetStats` reads depend on it; `StateCountsMatch`
  (`AssetTestUtils.h`) checks it.
- **Finalization** (`Asset::FinalizeOnMainThread(const AssetFinalizeContext&)`):
  - GPU uploads go through the context's `Upload` in steps of at most `c_AssetUploadStepBytes` (4 MiB), within the
    call's `UploadBudget`, `UploadMsBudget`, `Deadline` and staging room (`GetStagingRoom`).
  - Only the frame's first step (`FirstOfFrame`) may exceed them.
  - Report `UploadedBytes`/`UploadMs`.
  - `Pending` continues in the next frame.
  - Assets report memory per pool (`GetMemoryUsage`, `GetFinalizedMemoryUsage`), and partially resident textures report
    what is resident.
- **Dependencies, bounds and reads.**
  - Dependencies and bounds are recorded at import (import records version 2; engine-native assets get native records)
    and answered by `GetDependencies`/`GetBounds`, the same in the editor and from packs.
  - Prefetch goes into the budgets' headroom only.
  - Group loads: `RequestLoadWithDependencies` returns an `AssetGroupRequest`.
  - Ranged reads: `AssetManagerBase::ReadStoredRange`.
  - Other entry points: `PeekLoadedAsset`, `ReplaceLoadedAsset` (document edits), `GetContentVersion`/
    `GetContentChanges`, and `GetSourceRevisions` (reimports change them, evictions do not).
- **Imports.**
  - Importers describe every setting (`GetSettingsProperties`, `ImportSettingsBuilder`, `GetInactiveReason`).
  - They bump `GetVersion()` when output changes.
  - They read other files only through `ReadImportSource`/`ReadImportDependency`.
  - They derive sub-asset handles with `DeriveSubAssetHandle(parent, key)`.
  - They check `ImportContext::IsCancelled` between pieces of work that take milliseconds.
  - Editors import in the background (`Scan(ScanImports::InBackground)`, `SetImportSettingsInBackground`,
    `ReimportAssetInBackground`). Nothing in the editor waits for a background import: imports are cancelled instead.
  - Background jobs share a reference-counted `BackgroundWork` and never touch the manager.
  - Registration of new files (`RegisterAddedFile`: the `.meta` file, import records, the reference search) still
    runs on the main thread, about 6 ms per file. world-io moves it.
- **Textures** cook to BC7, BC1, BC4, BC5 or RGBA16F (`TextureImporter::ChooseFormat`).
  - `STTX` version 2 stores levels smallest first.
  - Compute sizes only through `GetMipByteSize`/`GetTextureFormatInfo`, and parse with `ParseCookedTexture`.
  - Normal maps reconstruct Z.
  - UI draws textures through `UI::TextureImage`.
- **Packs version 2** (`AssetPack`, written by `AssetPackWriter`, which is tooling only):
  - 256 KiB zstd blocks with CRC-32C checksums;
  - entries 4 KiB-aligned (`c_EntryAlignment`);
  - dependency and bounds tables;
  - ordered by first use.

  Every format change extends the corruption tests (`AssetPackTests.cpp`).
- **Adding an asset type:** follow AGENTS.md "Adding an asset type":
  - a loader in the owning module;
  - round-trip and garbage/truncation tests;
  - `GetMemoryUsage`;
  - `AssetDependencies::Scan` or importer references when it references other assets;
  - the native-extension table for engine-native documents;
  - an asset in the feature project.

## 5. Renderer

- **`RenderScene`** (per scene, in its registry context): proxies of mesh renderers in a loose-grid `SpatialIndex`,
  updated from component signals and `Scene::GetWorldTransformChanges`.
- **`SceneRenderer::Render`** runs, in this order:
  1. `Sync`;
  2. `CullView`;
  3. `FindShadowCasters` (serial today);
  4. `ResolveAssets` (`AssetResolveCache`: one lookup per asset per frame, nothing held after the frame);
  5. `GatherDraws`/`GatherShadowDraws` (parallel chunks, a deterministic merge, `RadixSort`);
  6. `UploadSceneData` (only changed instances, materials and draw-list ranges).
- **Requests follow visibility:** High above 5% of the viewport height, Normal below, Low within `PrefetchDistance`.
  Unloaded meshes are placed by recorded bounds (`ResolveUnloadedBounds`). Never resolve assets per entity in renderer
  code.
- **`RenderData::InstanceData`** is 64 bytes:
  - an `AffineTransform` (3x4);
  - `MaterialIndex`;
  - `EntityID`;
  - `Flags`;
  - `SkinPalette` (the first joint matrix of a posed instance).

  Batching ignores materials: shaders read `u_Materials[instance.MaterialIndex]` through `u_DrawList[gl_InstanceIndex]`.
- **Meshes** own their own position, attribute, skin and index buffers today (`Mesh.h`), so instances share a draw only
  per mesh. gpu-driven-rendering pools them.
- **Statistics.** `SceneRendererStats` is published only to the viewport's stats overlay. `MaxLights` = 4096,
  `MaxLightsPerCluster` = 256, and `ShadowMapSize` 2048 with 4 cascades. The frame profiler has 12 built-in stages
  (`c_BuiltinFrameStages`: Input, Assets.Update, Assets.Finalize, Scene.Load, Scene.Scripts, Scene.Physics,
  Scene.Audio, Scene.Transforms, Render.Extract, Render.Submit, Editor.UI, Present). Systems register further stages,
  such as Scene.Animation.
- **Deferred releases.** Resources a frame in flight may use go through `Renderer::ReleaseDeferred`, and bindless slots
  release deferred.
- **NVRHI pitfalls** (AGENTS.md): never `&handle`; explicit UAV barriers between dispatches through one binding set;
  clears do not keep textures alive.
- **Shaders:** GLSL in `Strata/shaders`, every file listed in `strata_add_shaders` (re-configure after adding one),
  bindings through `ST_SRV/ST_SAMPLER/ST_CBV/ST_UAV`. Reversed-Z, D3D viewport conventions, CCW front faces.
- **Debug views:** `SceneRenderOptions::EntityTints`. `Editor/StreamingView` builds them from proxies without
  requesting assets. `LODSelection` is the renderer's own LOD choice.
- **GPU tests:**
  - share one device (`Tests::GPUContext`);
  - end with `CHECK(gpu.GetNewErrorCount() == 0)`;
  - read pixels with `Renderer::ReadTexture`;
  - use the helpers in `SceneRendererTestUtils.h` (`AddNeutralPostProcess` for exact colors).

  A test that has to fail without a fix: confirm that it does (revert the fix locally, run, restore).

## 6. Threading

- **The main thread** owns the scene, input, scripts, editor state and render submission.
- **Jobs:**
  - `JobSystem::Submit`/`ParallelFor`: workers at normal priority, for frame work;
  - `SubmitIO`: I/O threads;
  - `JobPriority::Background`: threads below normal priority, for imports, compression and anything long no frame
    waits for.
- **Waiting.** A thread waiting for a job runs only that job if it has not started, never other queued jobs.
- **Results** return by polling (completion queues, `JobHandle`, `SubmitToMainThread`), never through callbacks into
  engine state from workers.
- **Thread counts.** Every program, a minimal game too, starts 23 workers, 2 I/O threads and 23 background threads.
  `ApplicationSpecification::Jobs` can change that; engine-modules sets it per program.

## 7. Scripting ABI (two wave-4 streams append)

- **The host table.** `StrataScriptHostAPI` has 119 host functions (`ST_SCRIPT_HOST_FUNCTIONS` in
  `Scripting/ScriptHostAPI.cpp`, the last `PhysicsGetLayerIndex`), ABI version 1. To add one:
  - append it at the end only;
  - write `Host<Name>`, wrapped in `HostCall` and starting with `ResolveContext`;
  - add `X(Name)` to the list (a `static_assert` checks it);
  - write its SDK wrapper through `ST_SCRIPT_DETAIL_HOST_WITH(Name)`: it is optional for older modules, so degrade
    gracefully.
- **Class callbacks** (`StrataScriptClassDesc`): OnCreate, OnUpdate, OnFixedUpdate, OnLateUpdate, OnDestroy, OnReload,
  OnCollisionEnter/Exit, OnTriggerEnter/Exit and OnAnimationEvent. New ones are appended and read only when the
  module's `StructSize` covers them (`ST_SCRIPT_HAS_MEMBER`).
- **Script code** reaches other modules only through scene services. The Scripting layer may not include World,
  GameUI, Physics or Animation, so give your module's scripts a service in `Scene/SceneServices.h`, as
  `IAnimatorControl` does.
- **Wave-4 append order is the merge order:** world-partition, then game-ui (authoring-and-playtest appends none). The
  later stream re-appends its block after the earlier one when it rebases.
  - world-partition: 5 host functions after `PhysicsGetLayerIndex` (119 -> 124: `WorldRequestCellsAround`,
    `WorldReleaseCells`, `WorldIsLoadedAround`, `WorldSetLayerEnabled`, `WorldIsLayerEnabled`), no class callback.
  - game-ui: 18 host functions after those (124 -> 142, `UIGetText` ... `GetTypedText`), and the class callbacks
    `OnUIClick`, `OnUIValueChanged`, `OnUIFocusChanged` after `OnAnimationEvent` (`ScriptCallback` gains the three).
  - authoring-and-playtest adds `c_ScriptCallbackCount` next to the `ScriptCallback` enum, defined from the enum's last
    value, so callbacks game-ui appended are counted without edits.
- **Coverage.** Every host function, public SDK class, function and macro, and callback must be exercised:
  - by the feature scripts (`StrataTests/FeatureTest/Scripts`; coverage is enforced by `GetScriptHostCallCounts` and by
    `SDKCoverageTests.cpp` with `SDKReader.h`'s receiver rules);
  - by the API test module (`StrataTests/Scripts/API`), with a `Scripting.*` test.

  Messages scripts log on purpose go in `c_ExpectedLogMessages`. SDK helpers that run in the module are tested inside a
  module.

## 8. Editor rules (StrataEditorCore + StrataEditorUI)

- **Commands.** Every scene or project change is a command (`EditorCommandRegistry::Execute`), or a
  `SceneEditTransaction`/`SetPropertyWithUndo` for continuous UI edits.
  - One undo step per successful mutating command.
  - Invalid input changes nothing (`InvalidParameters`); valid but impossible requests are `Failed`.
  - Values outside a schema's range are refused.
  - Long work returns `EditorCommandResult::Defer(poll)`, polled by `EditorCommandRunner` once per frame. Poll functions
    own their data.
  - **Two new `EditorCommand` registration flags this wave** (members appended to the struct; a union at merge):
    - `WaitsForAssets` (world-io): while a background scan runs, `EditorCommandRegistry::Execute` defers every `Edit`
      command and every query with this flag **before its handler runs**, and runs it once the scan ended.
    - `MayDefer` (authoring-and-playtest): the command's own handler can return a pending result. `editor.batch`
      refuses such commands, and Debug builds assert when a handler without the flag returns one. The registry's scan
      deferral is not the handler's, so it never trips the assert; `editor.batch` is itself an `Edit` command and is
      deferred whole during a scan, so its steps never meet one.
    - The flag does not exist until authoring-and-playtest merges (last). Every other stream lists, in its report's merge
      notes, the commands whose handlers it made (or makes) deferring; authoring-and-playtest sets `MayDefer` on all of
      them in its rebase and adds them to `c_DeferringCommands`.
  - UI actions that finish at once use `RunEditorCommand` (`UI/EditorActions.h`).
  - Write commands for agents: a description that says what it does and returns, a complete JSON schema, camelCase
    results, IDs as 16-digit hex strings, images as `{"Image": {"MimeType", "Data"}}`.
  - Update the skills (`strata-editor-automation`, `strata-make-a-game`, `strata-scripting`) in the same commit.
- **The journal.** Every command is registered with its kind (`EditorCommandKind::Edit`, `Query` or `Session`) and its
  results kind (`EditorCommandResults::Content` or `Session`).
  - `StrataTests/src/Editor/CommandJournalTests.cpp` lists every command in `c_EditCommands` (47), `c_QueryCommands`
    (37) or `c_SessionCommands` (31), plus `c_SessionResultCommands`. **A new command goes into its list**, or the test
    fails.
  - Commands that replace the edited scene refuse unsaved changes without `force`.
  - Edits of the running game report a warning.
- **`editor.status` becomes compact by default** when authoring-and-playtest merges (last): `sections: [...]` adds the
  rest, `["all"]` gives today's answer. The compact form keeps `assets.scanning` and `saving` (world-io) and
  `play.running`. A test or command script of another stream that reads a section outside the compact form keeps
  working: authoring-and-playtest adds `sections` to it in its rebase.
- **Notifications** (`EditorNotifications::Post`): a level, a title, a message, optional actions (Show Problems, Show
  Asset, Recover...) and a group. Toasts are drawn by `UI/Notifications`.
- **Asset documents.** Edits of asset documents go through `Editor/AssetEdit` (`AssetDocumentAction`,
  `ReplaceLoadedAsset`).
- **UI rules** (scanned by `Editor.UI.Source`, empty allowlist):
  - colors only from `UI/Theme.h` tokens;
  - sizes relative to the font (no fixed pixels above 16);
  - panels are `EditorPanel`s registered in `EditorLayer::RegisterBuiltinPanels` (never `ImGui::Begin`);
  - controls from `UI/Widgets.h` (each records its rect in `UI::ItemProbe`);
  - fonts and Lucide icons from `UI/EditorFonts.h`/`UI/Icons.h` (new icons: `StrataEditor/Tools/GenerateIconHeader.py`);
  - long lists through `ImGuiListClipper`;
  - selection rows through `UI::PushSelectionColors`;
  - property widgets from `UI/PropertyWidgets.h`;
  - dialogs through `EditorPanelContext::Shell`.

  Status pills say what is wrong and lead to it. The editor's UI namespace is `Strata::UI`, so engine code must not use
  it (the game UI module is `GameUI`).
- **Panels.** The registered panels (ids in `EditorLayer.h`, `EditorPanels`): Viewport, Hierarchy, Inspector,
  AssetInspector ("Asset"), Console, Problems, ContentBrowser, Welcome (the launcher), Streaming, Performance and
  Activity. The last three are closed by default.
  - The default layout is version 4 (`EditorLayer::c_LayoutVersion`).
  - authoring-and-playtest adds the Game view and bumps it to 5, once. A stream that must add another docked panel says
    so in its report: the integrator bumps once for the wave.
- **UI tests:** `StrataTests/src/Editor/ImGuiHarness.h` (the real `EditorLayer`, a fake `EditorHost`, injected input,
  probes, `CountFallbackGlyphs`) and `HarnessEditor.h`.
- **Idle-frame cost.** `PerfGPU.Editor.{Edit,Play}FrameNested{100k,1M}` gate an idle editor frame. Budgets 0.2808,
  0.2928, 0.300 and 0.3133 ms, measured 0.187-0.209 ms.
  - Anything drawn or updated every frame (pills, toasts, chips, the Game view, a cell map, a UI pass, GPU timers) must
    cost nothing measurable while idle: no per-frame scans, allocations or file-system calls; rebuild on change
    versions.
  - Measure with `editor.perf` (stages) before and after. A regression is fixed, never budgeted.
- **Large registration functions.** `RegisterAssetCommands` is 918 lines and `RegisterSceneCommands` 897. A stream that
  substantially edits a command group's registration moves that group into its own function or file (e.g.
  `Editor/Commands/<Group>.cpp`), in its own commit. Never refactor command files you do not otherwise touch.
- **Visual verification** (the owner judges by running the editor):
  - Take screenshots of every new or changed UI with the Release editor, using `--frames N --screenshot <png>` on a
    copy of a project:
    - `StrataTests --strata-test-helper=write-stress-project`;
    - a FeatureTest copy;
    - a Tetris copy;
    - a basic3d project made by a command script.
  - Use a private `STRATA_SESSION_DIR`, `STRATA_RECENT_PROJECTS` and `--layout <file>`.
  - Shoot at the display's 150%, at `--ui-scale 1`, and in a laptop-sized window (`--windowed`, 1600x900).
  - Look at them, and fix what looks unfinished.
  - Put them under `scratchpad\wave4\<key>\shots` and list them in your report.

## 9. Perf gates

- **Wave-4 directive** (the same as wave 3's, which saved hours). Six agents share this machine. Stream agents do not
  run `Tools/PerfStability.py`, `ctest -L perf`, a Release ctest that includes the perf label, or load gates
  (`--wait-quiet`, "wait until alone").
  - Verify with `ctest --preset windows-debug` (validation layer) and `ctest --preset windows-release -LE perf`.
  - Gather perf evidence only as **interleaved A/Bs**, only for metrics your stream's code affects:
    - the base build (the step-0 tip's Release build, its path from the orchestrator) and yours alternate;
    - at least 3 pairs of the same command;
    - single test cases (`StrataTests.exe --test-case=...`) or single editor and runtime runs;
    - judged by the ratio of the medians.
  - Run every measurement through the machine-wide perf lock:
    `/c/msys64/mingw64/bin/python.exe -I <scratch>/perf_lock.py --label <your key> -- <command> [args...]`
    - It waits up to 30 minutes for the lock, then exits 75. Do other work meanwhile.
    - It kills a command after 25 minutes (exit 124).
    - `--status` shows who holds it.
    - At most two locked A/Bs per item.
  - Memory metrics hardly depend on CPU load: measure them under the lock without any other gate.
- **Checks.** Check every metric with `Perf::CheckBudgetConfirmed(key, measure)` or `CheckBudgetsConfirmed(keys,
  measure)` (`StrataTests/src/Perf/PerfUtils.h`; up to `c_ConfirmationAttempts` = 3 fresh measurements).
  - Every limit lives in `StrataTests/Perf/Budgets.json` (114 entries now): `{Measured, Budget, Unit, Target?}`, in
    ms, us, MB, count or ratio.
  - A budget is at most 1.5 times its measured value, or an acceptance limit below that. A target is a roadmap goal
    below the budget.
  - Use medians of enough samples. Use maxima only for metrics that are maxima by nature, and say why in a comment.
  - Measure peak memory in a helper process (`--strata-test-helper=<mode>`, `TestMain.cpp`).
  - Complexity claims are unit tests with deterministic counters, which run everywhere.
- **Your new metrics** need entries, set from your locked measurements and named in your report as "to confirm in the
  integration campaign".
  - **Budget basis.** Every new metric has one: an acceptance limit (the budget is that limit, or 1.5 times the
    measurement when that is lower) or "1.5 x measured". "Reported" metrics still get an entry: the check records them.
  - **Targets** a spec names go into the entry only while they lie below the budget (the parser rejects a target the
    budget already enforces). One the measurement already beats is left out, and the report says so.
  - Do not lower or raise existing budgets from numbers measured on this busy machine. **Never raise a budget.**
  - A metric over its budget on your branch: run the same case on the base build back to back before concluding
    anything.
  - Not yours to chase: metrics the base fails too, or that your code does not touch. Write one line in your report and
    move on.
- **The machine's speed.** The canary is `Perf.Harness.Sort200k`, reported by `PerfStability.py` as a ratio to its
  10.0 ms reference. Away from 1.00 by more than about 3% means a disturbed machine.
- **Overlays.** RivaTuner Statistics Server and similar overlays add about 55 MiB of private bytes to every windowed
  Vulkan process. Before measuring memory, check `tasklist`. If one runs, do not stop it (it is the owner's program):
  measure the base in the same conditions and say so.
- **The decision.** The integrator's quiet campaign (`PerfStability.py --runs 5 --wait-quiet 20` on the merged tree)
  decides every budget. A perf criterion you could not prove on this machine is `deferred` in your report, with the A/B
  ratio you have.

## 10. Tests

- **Suites.** doctest suites are named after the module (`TEST_SUITE("World.Streaming")`, `TEST_SUITE("GameUI.Layout")`).
  - GPU suites start with `GPU` (label `gpu`).
  - Editor process suites start with `EndToEnd` (CTest `StrataEditor.Automation`).
  - Package suites start with `Package`.
  - Perf suites are as in section 9.
- **CTests.** The base has 47 CTests in Debug, 47 in Release without the perf label, and 50 with it.
  `StrataTests.Core` is every suite except GPU, EndToEnd, Package, Perf and FeatureTest. engine-modules shards it by
  suite prefix: once it merges, a new suite must fall into a shard (a coverage test checks).
  - After engine-modules merges, `StrataTests.Core` is about ten CTests `StrataTests.Core.<Shard>` (label `core`;
    `ctest -R StrataTests.Core` selects them all), `StrataTests/CoreShards.json` lists the shards, and the CTest counts
    above change. engine-modules pre-adds `World*` and `GameUI*` to shards. A stream that merges after it puts any
    other new suite prefix into `CoreShards.json` in its rebase (`Testing.Shards` names the ones missing).
  - `StrataTests.Fuzz` (engine-modules, label `fuzz`, Release and Dist) runs the full corruption sweeps; Debug runs a
    deterministic sample. Every Release `ctest -LE perf` includes it, so no sweep is lost.
- **World-scale fixtures in Debug.** A Core shard should stay under about 60 s in Debug. Correctness tests on
  world-scale data (hundreds of thousands of entities, tens of thousands of assets) run scaled down with lowered limits
  (an entity cap, a capacity, a cell count passed as a parameter), and the full-size cases run in Release-only CTests
  (the perf suites, or `StrataTests.Fuzz`).
- **Before you report:**
  - The full Debug `ctest --preset windows-debug` passes under the validation layer.
  - `ctest --preset windows-release -LE perf` passes.
  - Temporary files go only through `Tests::CreateTemporaryDirectory()`.
  - Samples are used only as copies (`Tests::CopySampleProject`).
  - Helper child processes go through `--strata-test-helper=<mode>`, and fake CMake through
    `STRATA_TEST_FAKE_CMAKE=succeed`.
- **Feature test coverage** (golden rule 6):
  - every registered component in `Feature.stscene`, with every property non-default somewhere;
  - every script field type;
  - every callback journaled;
  - every host function called;
  - every public SDK symbol used on a receiver of known type;
  - every asset type present, with a committed `.meta` (tiny binaries from `Tools/GenerateAssets.py`, outputs
    committed);
  - no unexpected warnings;
  - importing never rewrites a `.meta`.

  The runners: `StrataTests.FeatureTest` (headless, editor commands, exported game), `GPU.FeatureTest`,
  `StrataEditor.FeatureTest` and `StrataRuntime.FeatureTest`. The project also has `FeatureSub.stscene` (sub-scenes).
- **Tests that must fail without the work.** Confirm that each acceptance test that claims to catch a problem fails on
  the base (or with the fix reverted) and passes with it. Say in your report which ones you confirmed and how.
- **Samples and command scripts.** `StrataEditor.Tetris` plays a copy of `Samples/Tetris` with
  `Editor/TetrisCommands.json.in`. A change to command behavior must keep these passing, or update them in the same
  commit:
  - Tetris;
  - `SmokeCommands.json.in`;
  - `FeatureCommands.json.in` (it waits 160 frames after `play.step 150`);
  - `LauncherCommands.json`;
  - `ScriptsEndToEnd.cmake`;
  - `StrataEditor.Install`.

## 11. Docs, skills, roadmap

- **Docs and skills.** Update `Docs/Architecture.md` (the section that names your code) and `AGENTS.md` (rules only;
  descriptions go to Architecture.md) in the same commit as the behavior. Update the skills under `.claude/skills/`
  when agents' workflows change.
  - People's documentation goes in `Docs/Manual/` (it only has `Scripting.md` today). Add a page when your stream gives
    people a workflow: game UI, worlds.
- **Do not edit the Status section of `Docs/Roadmap.md`.**
  - The integrator writes the wave-4 entry and `Docs/Releases/0.4.0.md` from your report. Put your measured numbers,
    open items and deferrals in the report.
  - You may edit Roadmap's "Open items" only to remove an item your stream closed; say so in the report.

## 12. Shared files in wave 4 (who owns what)

Merge order: engine-modules, world-io, world-partition, gpu-driven-rendering, game-ui, authoring-and-playtest.
"Function-level" means each stream keeps its change in functions of its own, so that merges are unions.

| File or area | Owner in wave 4 | Others |
| --- | --- | --- |
| `Core/Application.*`, `Core/EntryPoint.h`, the new EngineModule interface, `CMake/*`, every `CMakeLists.txt`'s structure, `Strata/vendor/CMakeLists.txt`, `stpch.h`, `.github/workflows/ci.yml` | engine-modules | Others add sources, shaders, tests and targets inside their own blocks. New layers become libraries through `Layers.json`, with no CMake of their own |
| `Scene/Components.h`, `Scene/ComponentRegistration.cpp`, `Scene/SceneSystem.*` (constraints) | engine-modules (moves components into their modules, soft constraints) | authoring-and-playtest: `PrefabInstanceComponent`'s override list and registration flags, which stay in Scene; `InactiveReason` hooks on the sky, post-process, camera and animator registrations, kept in one block on its branch and moved next to the moved registrations in its rebase (it merges after engine-modules). world-io: the Script registration's `ExtraBinary` block (moves with the component). world-partition: `SceneSystemDescriptor::PrepareLoadedScene` (appended). Nobody else adds components here |
| `Engine/BuiltinModules.cpp` | engine-modules (shape) | world-partition (`RegisterWorldModule`) and game-ui (`RegisterGameUIModule`): one line each, at the end, World then GameUI |
| `Layers.json`, `LayeringAllowlist.txt` | engine-modules (allowlist, notes, the `Tooling`/`Option` keys, `Engine`'s narrowed `MayInclude`) | world-partition (`World` layer; `World` appended to `Engine`'s and `AssetPipeline`'s `MayInclude`), game-ui (`GameUI` layer; appended to `Engine`'s), world-io (its AssetPipeline files in AssetPipeline's `Paths` and Asset's `Exclude`), gpu-driven-rendering (patterns for new files, if needed): append; unions |
| `Scene/SceneSerializer.*`, `SceneLoader.*` (read path, cancel), `EntityPrototype.*`, `Prefab.*` read path, the new cooked scene format, `Reflection/PropertyBinary.*` | world-io | engine-modules (merges first): their `ScriptComponent` special cases become `ComponentInfo::ExtraReferences` calls. world-partition writes cell documents through world-io's writer options, and adds one settings field and `SceneLoadOptions::PrepareSystems` and the budget (function-level) |
| `Asset/EditorAssetManager.*` (registration, scan, records index, import admission), `AssetPack*`, `AssetPackWriter`, `AssetPackCook`, `AssetDependencies.*` (streamed scan), `RuntimeAssetManager`, `AssetManager` residency info, `Platform/*/FileReader` | world-io | engine-modules (`AssetDependencies`: `ExtraReferences`, an `ST_WITH_ANIMATION` guard); world-partition registers the World asset type (`AssetTypes.h`, the native-extension table, `AssetDependencies::Scan` of world documents, `AssetPackBuildOptions::SceneOrder`): function-level, append |
| `EditorContext::SaveScene`/`SaveSceneAs`, close and quit paths, `scene.save`, `StrataCLI` launch and MCP wait, `EditorApplication.cpp` session publishing, `Editor/DocumentSaver.*` | world-io | function-level |
| `Scene/SceneSubScenes.cpp`, `SubScene*.h`, `Physics/*`, `Runtime/GameRuntime.*`, `Project/GameManifest.*` | world-partition | engine-modules (moves physics components, with includes; the `IBodyMotionQuery` additions; the Scripting guard in `GameRuntime`); world-io (one log line in `GameRuntime`, "Started '<scene>' in N s", which its runtime gate reads) |
| `Scene/Scene.*` | shared, function-level: world-partition (edit-mode sub-scenes, sliced unload, `UpdateSubScenes(SceneWorkBudget&)`), world-io (the `SceneCopyOptions` overload of `Scene::Copy`), gpu-driven-rendering (the context copiers: they run for full copies only, never for world-io's `SerializedOnly` snapshots), game-ui (unscaled time, in `OnUpdateRuntime`), authoring-and-playtest (`GetFixedStepCount`, in `OnUpdateRuntime`; pending steps cleared on resume and stop), step 0 (the seam) | - |
| `Scene/SceneServices.h` | appended by everyone, a union: engine-modules (functions appended to `IBodyMotionQuery`, `IAudioPlayback`, `IAnimatorControl`), world-partition (`IPhysicsCapacity`, `IWorldStreaming`), game-ui (`IUIControl`, `IUIEvents`) | - |
| `Reflection/ComponentRegistry.h`, `Property.h`, `PropertyBuilder.*` | engine-modules (struct shape: `ExtraReferences`, gizmos, `PropertyOptions.h`) | function-level members, a union: world-io (`ExtraBinary`), game-ui (`Requires`), authoring-and-playtest (`InactiveReason`) |
| `Renderer/*` except `TextRenderer`/`TextLayout`/`Font*`; `Strata/shaders/*`; `Core/FrameProfiler.*`; `Runtime/FramePerfLog.*`; `Runtime/GameRenderer.*` | gpu-driven-rendering | game-ui: `Renderer/SceneOverlay.h` (its overlay hook, see its spec), the four single-call hooks in `SceneRenderer`, one branch in `GameRenderer` for camera-less UI scenes, its shaders. world-partition: the column-group API of `FramePerfLog` (below) and the one-line `SceneWorkBudget&` parameter of `RenderScene::PrepareLoadedScene`. engine-modules: `GraphicsModule.*`, `RendererComponents.*`, the mesh-geometry reader, `SceneGizmos.cpp`'s registry loop, NVRHI validation under `ST_DIST`. Every stream's new frame stages through `ST_PROFILE_STAGE` |
| `Renderer/TextRenderer.*`, `TextLayout.*`, `Font*`, `Input/*` (UI capture, typed text) | game-ui | gpu-driven-rendering: a GPU timer scope around the text pass and camera-relative world-text vertices, at one call site. authoring-and-playtest: `Input::SetViewport`'s resolution parameter only |
| `Core/EngineServices.h`, `Runtime/GameWindowService.*`, `Editor/EditorGameWindow.*` | function-level union | game-ui (`GameWindowState::ContentScale`), authoring-and-playtest (the Game view's resolution in `EditorGameWindow`) |
| `StrataRuntime/src/RuntimeApplication.cpp`, `StrataEditor/src/EditorApplication.cpp` | engine-modules (`CreateApplication` returns `ApplicationLaunch`, module lists, job counts) | function-level: world-io (session publishing), world-partition (runtime pacing, perf-log columns), gpu-driven-rendering (the `Render` column group), authoring-and-playtest (`Core/CommandLineOptions`, `--help`, exit code 3) |
| `Editor/UndoStack.*`, `Editor/SceneEdit.*` | function-level union | world-io (`SavePoint`, `GetSavePoint`, `MarkSaved(SavePoint)`), world-partition (observer list `AddObserver`/`RemoveObserver`; `EntityState::Cell`, `CheckCells`), authoring-and-playtest (`BeginGroup`/`EndGroup`) |
| `Editor/EditorCommands.*`, `EditorCommandRunner.*` | function-level union | world-io (`WaitsForAssets`, the deferral during scans, the `Editor.Commands` stage), authoring-and-playtest (`MayDefer`, `editor.batch`) |
| `ScriptABI.h`, `ScriptHostAPI.cpp`, SDK headers, `StrataTests/Scripts/API`, feature scripts | world-partition, then game-ui (append blocks in merge order) | - |
| `Feature.stscene`, `FeatureTest/Assets` | world-partition (a tiny world), game-ui (a canvas), authoring-and-playtest (a prefab override): append | unions |
| `Budgets.json`, `CommandJournalTests.cpp` command lists, skills, `AGENTS.md`, `Docs/Architecture.md` | everyone appends, in their own entries and sections | unions |
| `EditorContext.*` (other than saves) | function-level: world-partition (worlds and cells in edit mode, play start of a world), authoring-and-playtest (stepping, the Game view, the input timeline) | - |
| `EditorStateCommands.cpp`, `EditorSceneCommands.cpp`, `EditorInputCommands.cpp`, `SimulatedInput.*`, `ScriptEdit.*`, `EditorAssetCommands.cpp` (info), log and activity commands, `StrataCLI/src/CLI/McpServer.*`, StrataRuntime arguments | authoring-and-playtest | world-io (`scene.save`, launch wait), gpu-driven-rendering (`editor.perf` GPU block), world-partition (`scene.loadStatus` cells): function-level |
| `Panels/InspectorPanel.*`, `UI/PropertyWidgets.*`, Reflection property metadata (`InactiveReason`) | authoring-and-playtest | game-ui: a `UIRect` anchor-preset drawer, in its own function |
| `Panels/ViewportPanel.*`, `Editor/ViewportRenderer.*`, `Editor/EditorViewport.*` | authoring-and-playtest (the Game view, chip placement) | game-ui (2D canvas mode), gpu-driven-rendering (GPU lines in the stats overlay), world-partition (an optional cell overlay): each in its own functions |
| `Panels/SceneHierarchyPanel.cpp` presets | authoring-and-playtest (primitives with colliders) | game-ui (UI presets): union of the table |
| `Panels/StreamingPanel.*`, `Editor/StreamingView.*`, `EditorStreamingCommands.cpp` | world-partition (cell map, Physics section) | world-io (the residency info it reads), gpu-driven-rendering (feedback-driven visibility): function-level |
| `Panels/PerformancePanel.*`, status bar frame pill | gpu-driven-rendering | - |
| `Editor/GameExport.*` | world-io (cooking scenes to binary) | world-partition (cells and capacities), function-level |
| `EditorLayer::RegisterBuiltinPanels`, `c_LayoutVersion` | authoring-and-playtest (Game view; bumps 4 to 5) | Others add no docked panel; if one must, say so |
| `Asset/GltfImporter.*`, `Asset/TextureImporter.*` | authoring-and-playtest (import scale, normal maps) | gpu-driven-rendering bumps `GltfImporter`'s version if its LOD error changes cooked meshes; the integrator leaves one bump each. world-io: `EstimateImportMemory` overrides only (no output change, no bump). engine-modules: the `ST_WITH_ANIMATION` guard in `GltfImporter.cpp` |
| `Animation/AnimationPreview.*`, `Editor/ThumbnailCache.*`, `Editor/SceneBounds.*` | authoring-and-playtest (rest poses) | - |
| `Samples/Tetris`, `TetrisCommands.json.in`, `SmokeCommands.json.in`, `FeatureCommands.json.in` | authoring-and-playtest (contract changes) | game-ui (Tetris HUD on a canvas, if it migrates it) |

The integrator does the "whichever merges second wires X" items listed in `plan.md` when neither stream could. Where
the later stream can (it merges after the code it needs), the wiring is in its own scope and done in its rebase, not
left to the integrator.

### Interfaces between streams (one owner each; both sides use these exact shapes)

- **`ComponentInfo::ExtraReferences`** (engine-modules, `Reflection/ComponentRegistry.h`): `Visit`, `Set`, `Equals`,
  `VisitJson`, set by `ComponentInfoBuilder::ExtraReferences(...)`; the Script registration supplies them. world-io's
  readers and writers use them for entity and asset references in extra data; until engine-modules merges, world-io's
  new code calls the existing generic paths (`EntityPrototype`'s reference list, `RemapEntityReferences`,
  `AssetDependencies`' per-entity scan) and names no component type.
- **`ComponentInfo::ExtraBinary`** (world-io): `SerializeExtraBinary`/`DeserializeExtraBinary`, set by
  `ComponentInfoBuilder::ExtraBinary(...)`. A component with JSON extras (`SerializeExtra`) and no binary hooks, such as
  authoring-and-playtest's prefab override list, has its extra JSON stored as MessagePack in cooked scenes, so it needs
  no hook of its own.
- **`FramePerfLog` column groups** (world-partition lands them in one commit of its own, function-level in
  gpu-driven-rendering's `Runtime/FramePerfLog.*`; gpu-driven-rendering builds its `Render` group and GPU columns on
  them in its rebase):

  ```cpp
  class FramePerfRow { public: void Add(double value, int decimals); void AddEmpty(); };
  struct FramePerfColumnGroup { std::vector<std::string> Names; std::function<void(FramePerfRow&)> Write; };
  struct FramePerfLogSpecification { std::vector<FramePerfColumnGroup> Columns; };
  static Scope<FramePerfLog> Create(const std::filesystem::path& path, const FrameProfiler& profiler,
  	const FramePerfLogSpecification& specification = {}, std::string* outError = nullptr);
  ```

  Group columns follow every built-in column, in the order given; a group that writes another number of values than it
  names fails the log like a write error.
- **The overlay hook** (game-ui): `Renderer/SceneOverlay.h`, `ISceneOverlayPass` (`Prepare`, `Draw`,
  `HasScreenContent`) and `SceneOverlayRegistry`, filled by `RegisterGameUIModule`. gpu-driven-rendering defines no
  hook of its own; game-ui re-applies its four single calls on the final `SceneRenderer` in its rebase, wraps `Draw` in
  the `GPU.UI` stage (gpu-driven-rendering's `GPUPassTimer`) and registers `GPU.UI` in `RegisterGameUIModule`.
- **`DocumentSaver`** (world-io, `Editor/DocumentSaver.h`, frozen in its early commit): the request API that
  world-partition saves dirty cells and the World document through (world-io's spec, item 1).
- **`Perf/WorldFixture.h`** (world-io, first commit, add-only): realistic worlds and many-asset projects.
- **`SceneWorkBudget`** (world-partition, `Scene/SceneWorkBudget.h`): the frame's load work budget; `SceneLoadOptions::
  Prepare` takes it (`std::function<bool(Scene&, SceneWorkBudget&)>`).
- **`Scene::RegisterContextCopier`** (gpu-driven-rendering): copiers run at the end of full `Scene::Copy`s, including
  world-partition's play start of a world, and never for world-io's `SerializedOnly` snapshot copies.

## 13. Your report

- **Commits:** hash and subject of each.
- **Acceptance:** what each acceptance criterion measured: numbers, runs, the canary ratio where relevant.
- **Must-fail tests:** which tests you confirmed fail without the work, and how.
- **Test runs:** the full ctest results (Debug with the validation layer; Release `-LE perf`).
- **Perf:** the A/B ratios you measured under the perf lock (base build, pairs, medians), and the budgets you added with
  their measured values ("to confirm in the integration campaign").
- **Screenshots:** paths.
- **Docs and skills changed.**
- **Lint**, on your tree. Each must come back clean (0 hits for the compat check):
  - `export PATH=/c/msys64/mingw64/bin:$PATH; GCC_LINT_OUT=<scratch>/gcc_lint sh <scratchpad>/gcc_lint.sh <worktree>
    <worktree>/build/windows/Strata/Generated <worktree>/build/windows/Strata/EmbeddedShaders
    <worktree>/build/windows/StrataEditor/Generated`: MinGW GCC with CI's warning flags. It skips `src/Platform`, so
    read POSIX code there with care.
  - `/c/msys64/mingw64/bin/python.exe -I <scratchpad>/brace_check.py <worktree>`.
  - `/c/msys64/mingw64/bin/python.exe -I <scratchpad>/compat_check.py <worktree>`: constructs CI's GCC 14 and Apple
    Clang 15 reject (floating-point `from_chars`, braced lists mixing `size_t` and `uint64_t`, const structured
    bindings by value over braced lists, members named like their type, lambdas capturing structured bindings).

  Linux and macOS code is only compiled by CI. Say which POSIX paths you could not run.
- **Open items and deferrals,** with reasons.
- **Merge notes for the integrator:** files you expect to conflict, and wiring you left to the later stream.
- **`remainingItems` vs merge notes.** `remainingItems` is only for work your stream can still do in its own worktree,
  because each one starts a continuation agent. "Run a quiet perf campaign", waiting for another stream's merge and
  integrator wiring go in the merge notes.
