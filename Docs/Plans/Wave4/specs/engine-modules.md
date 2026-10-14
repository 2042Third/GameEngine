# Workstream `engine-modules`: Engine modules: an EngineModule lifecycle, per-layer libraries generated from the layer table, module-owned components, a tooling-free runtime, faster builds and tests, optional subsystems (wave 4, size XL)

Read `common.md` first. This stream **merges first**: every other wave-4 stream rebases onto it. It changes the build and
the application shell that everything else compiles in, and no feature behavior.

## Why

The owner asked for a "modular" engine. The architecture audit of 685aef7 (`wave4\audit\architecture\`, `STATE.md` lists
the evidence) found that Strata is modular in its includes only.

- **The application shell hard-codes the engine's services.** `Core/Application.cpp` calls `Engine::RegisterBuiltinModules()`
  in its constructor and then drives everything itself: `GraphicsDevice::Create`, `Renderer::Init`/`BeginFrame`/
  `PrepareBackBuffer`, `AudioEngine::Init`/`AdvanceNullDevice`/`Update`, `new ImGuiLayer`. Its destructor calls
  `AssetManager::SetActive(nullptr)`, `Project::SetActive(nullptr)`, `AudioEngine::Shutdown` and `Renderer::Shutdown`.
  - These are 7 of the 8 lines of `LayeringAllowlist.txt` (`MaxAllowlistEntries` 8).
  - `ApplicationSpecification` carries subsystem flags (`EnableRenderer`, `EnableImGui`, `EnableAudio`,
    `TransferQueueUploads`, `GraphicsValidation`, `ImGuiLayoutFile`).
  - Every program, a 2-asset game too, starts 23 workers, 2 I/O and 23 background threads (`smoke/run_window_1.log`).
- **One static library with public tooling dependencies.** `Strata/CMakeLists.txt` builds `add_library(Strata STATIC)` from
  one `GLOB_RECURSE` and links `nvrhi`, `imgui` and `ImGuizmo` PUBLIC.
  - `runtime_size_breakdown.txt` (Release `StrataRuntime.exe`, 7.33 MB): imgui 401 KiB plus Strata's ImGui layer 24 KiB
    (5.8%), pulled in because `Application.cpp` references `ImGuiLayer`; NVRHI's validation wrapper 131 KiB
    (`VulkanGraphicsDevice.cpp:757`); `vulkan-raytracing` 51 KiB; `ComponentRegistration.obj` 554 KiB, the largest object.
  - ImGuizmo is used only by `StrataEditor/src/Panels/ViewportPanel.cpp` (and one UI test).
  - `Docs/Architecture.md` says "The runtime has no ImGui", which is false.
- **Every module's components live and register in Scene.** `Scene/ComponentRegistration.cpp` registers 27 components in
  one TU (6.4 s alone, 13.8 s in a contended clean build). `Scene/Components.h` has 251 dependent objects: an edit costs
  86.2 s (`incr_components.log`).
  - Cross-module reads: `Audio/AudioSystem.cpp:650` reads `RigidBodyComponent`; `Renderer/SceneGizmos.cpp` reads every
    collider and the character controller; `Scripting/ScriptHostAPI.cpp` reads `RigidBodyComponent`,
    `AudioSourceComponent` and `AnimatorComponent` (lines 1195, 1594, 1962).
  - `Audio/AudioRegistration.cpp` declares `After = {"Physics"}` and `Animation/AnimationRegistration.cpp`
    `After = {"Scripting"}`, `Before = {"Physics"}`. `Scene/SceneSystem.cpp` (`ComputeUpdateOrder`) refuses a system whose
    constraint names an unregistered one, so Audio cannot exist without Physics.
- **Builds are slow where nothing helps them.** A clean Debug build is 3,099 CPU-s (`build_debug_breakdown.txt`):
  StrataTests 52%, StrataEditorCore 12%, Strata 10%, StrataEditorUI 6.5%. Only `Strata` has a precompiled header.
  - TUs spend 85-95% in the front end (`tu_summary_debug.txt`). `EditorUITests` parses 729 headers.
  - `nlohmann/json.hpp` is included by 18 engine headers; EnTT by every scene TU.
  - An `EditorContext.h` edit rebuilds 99 objects (42.7 s).
- **Tests run serially.** `StrataTests.Core` runs 1,672 cases as one CTest (`StrataTests/CMakeLists.txt`): 475 s of the
  658 s Debug `ctest`. 27 cases of 1 s or more take 81 s in Release (`core_durations_summary.txt`); "Version 1 packs that are
  corrupt are still refused" alone takes 26.4 s.
- **CI has no compiler cache** (Windows build 15-19 min, Linux 10-16, macOS 6-10, `ci_durations.txt`) and **no Debug job**.
- **Scale audit (minor):** threading is not the bottleneck, but nothing measures it. `Perf.Jobs` lets JobSystem v2 be
  decided on numbers.
- **Roadmap open items:** the allowlist's exceptions, and a policy for ordering constraints once subsystems are optional.

## Scope

### 1. EngineModule lifecycle (App layer)

**The interface.** New `Strata/Core/EngineModule.h/.cpp`, assigned to the App layer: `Layers.json` adds
`"Strata/Core/EngineModule.*"` to App's `Paths` and to Core's `Exclude`.

```cpp
struct EngineModuleDependency { std::string_view Name; bool Required = true; }; // Required false: order only, if present
struct EngineContext { Application& App; Window* MainWindow; EngineModuleRegistry& Modules; }; // MainWindow null headless
enum class EngineFrameAction : uint8_t { Continue, Skip, Close };
struct EngineFrame { Timestep Step; double WaitSeconds = 0.0; LayerStack& Layers; };

class EngineModule
{
public:
	virtual ~EngineModule() = default;
	virtual std::string_view GetName() const = 0;
	virtual std::span<const EngineModuleDependency> GetDependencies() const { return {}; }
	[[nodiscard]] virtual bool OnInit(EngineContext& context) = 0; // false: the program cannot run
	virtual EngineFrameAction OnBeginFrame(EngineFrame&) { return EngineFrameAction::Continue; }
	virtual void OnUpdate(EngineFrame&) {}
	virtual void OnRenderUI(EngineFrame&) {}
	virtual void OnEndFrame(EngineFrame&) {}
	virtual void OnEvent(Event&) {}
	virtual void OnStop() {}     // The loop ended; layers still exist
	virtual void OnRelease() {}  // Layers are gone; the job system still runs
	virtual void OnShutdown() {} // The job system has stopped
};
using EngineModuleFactory = std::function<Scope<EngineModule>()>;
```

- `EngineModuleRegistry` (same files) owns the modules.
  - It sorts them topologically: a stable sort that keeps the program's listing order where dependencies allow.
  - It refuses to start, naming the modules, for a missing required dependency, a cycle or two modules with one name.
  - `template<typename T> T* Find()` matches `T::c_Name` (no RTTI). `GetAll()` returns the modules in order.
- **Hook order** (the frame and shutdown order are data):

  | Hook | Order | When, and what the built-in modules do there |
  | --- | --- | --- |
  | `OnInit` | dependency | Application constructor, after `JobSystem::Init`, the window and `Input` |
  | `OnEvent` | dependency | Each event, after the application's own bookkeeping (minimized), **before** the layers. A handled event stops there. |
  | `OnBeginFrame` | dependency | After the Input stage. Graphics: vsync, `GraphicsDevice::BeginFrame` (its wait added to `frame.WaitSeconds`), `Renderer::BeginFrame`. `Skip`: the frame is not counted and the loop waits 16 ms; `Close`: device lost. |
  | `OnUpdate` | dependency | After the layers' `OnUpdate`. Audio: `AdvanceNullDevice` on the null device, `AudioEngine::Update`. |
  | `OnRenderUI` | dependency | ImGui: the `Editor.UI` stage, `ImGuiLayer::Begin`, every layer's `OnImGuiRender`, `Renderer::PrepareBackBuffer`, `End(backBuffer)`. The frame's work time ends after this hook. |
  | `OnEndFrame` | **reverse** | Rendered frames only. Graphics, last because everything depends on it: `PrepareBackBuffer`, back-buffer captures, the `Present` stage, `EndFrame`. |
  | `OnStop` | reverse | Once, when `Run`'s loop ends. Graphics answers outstanding captures with an empty image and waits for the device to idle. |
  | `OnRelease` | reverse | In `~Application`, after the layers are cleared. Project: `AssetManager::SetActive(nullptr)`, `Project::SetActive(nullptr)`. |
  | `OnShutdown` | reverse | After the main-thread queue, `JobSystem::Shutdown` and the queue again. Audio: `AudioEngine::Shutdown`; Graphics: `Renderer::Shutdown`, then the device. |

- A module whose `OnInit` fails:
  - logs `Engine module '<name>' could not start; the application cannot run` (Graphics keeps its existing "No usable
    graphics device" message);
  - leaves `m_Running` false and exit code 1;
  - only the modules that started get `OnRelease` and `OnShutdown`, in reverse. A module cleans up its own failed
    `OnInit`.

**`Application` only creates the window and runs the loop.** It keeps its current duties:

- the throttling opt-out;
- the working directory;
- `JobSystem::Init(spec.Jobs)`;
- the window and `Input`;
- the frame profiler's frame;
- the frame pacer and the work time;
- `SubmitToMainThread`;
- exit codes;
- `PushLayer`/`PushOverlay`;
- `GetModules()`.

It no longer includes any engine module header, so the 7 `Core/Application.cpp` lines leave the allowlist.

- Removed from `Application`: `GetGraphicsDevice`, `GetImGuiLayer`, `RequestBackBufferCapture`, `InitializeGraphics`,
  `RenderImGui` and `ProcessBackBufferCaptures`, plus the `ReadbackImage` and `GraphicsDevice` forward declarations.
- Removed from `ApplicationSpecification`: `EnableRenderer`, `EnableImGui`, `EnableAudio`, `ImGuiLayoutFile`,
  `GraphicsValidation` and `TransferQueueUploads`. It gains `std::vector<EngineModuleFactory> Modules`, the modules the
  program runs. The specification stays copyable.

**The built-in modules, each in its own folder and registered by its own code:**

- `Renderer/GraphicsModule.{h,cpp}`, `c_Name = "Graphics"`, `GraphicsModuleSpecification { std::optional<bool> Validation;
  bool TransferQueueUploads = false; }`.
  - It owns what `InitializeGraphics` does today, the frame calls and the back-buffer captures:
    `RequestBackBufferCapture(std::function<void(const ReadbackImage&)>)` and `GetDevice()`.
  - It creates a swapchain only when `EngineContext::MainWindow` exists.
- `Audio/AudioModule.{h,cpp}`, `"Audio"`, `AudioModuleSpecification { bool NullDevice = false; }`.
  - `OnInit` returns true when the output device fails ("Audio is unavailable; continuing without sound", as today).
- `ImGui/ImGuiModule.{h,cpp}`, `"ImGui"`, requires `Graphics`, `ImGuiModuleSpecification { std::filesystem::path LayoutFile; }`.
  - It creates the `ImGuiLayer` and pushes it as an overlay. It fails without a window.
  - `GetLayer()` gives the layer to the editor (theme, fonts, UI scale).
- `Project/ProjectModule.{h,cpp}`, `"Project"`, no dependencies: releases the active asset manager and project in
  `OnRelease`.
- `Layers.json`: Audio's and Project's `MayInclude` gain `App`, with Notes ("implements its EngineModule,
  `Core/EngineModule.h`"). App is listed below both, so this is a proven downward dependency, not a sideways one.

**The composition root moves to the entry point.**

- `Core/EntryPoint.h` moves to `Strata/Engine/EntryPoint.h` (Engine layer: it calls the composition root), with
  `Strata/Engine/ApplicationLaunch.h`:

  ```cpp
  struct ApplicationLaunch
  {
  	Engine::ModuleRegistrationOptions Registration; // the asset pipeline (tools), a game's components
  	std::function<Scope<Application>()> Create;     // runs after the composition root
  };
  std::optional<ApplicationLaunch> CreateApplication(const CommandLine& commandLine); // nullopt: exit GetStartupExitCode()
  ```

- `main` builds the `CommandLine`, calls `CreateApplication`, calls `Engine::RegisterBuiltinModules(launch->Registration)`,
  then `launch->Create()`, `Run()`, deletes the application and calls `Log::Shutdown`.
  - The `Application` constructor no longer calls the composition root. A program that forgot it fails the registries'
    existing `ST_CORE_VERIFY` at first use.
  - `StrataTests`' `main` already calls the composition root itself.
- The umbrella header `Strata.h` is deleted. `EditorLayer.h`, `EditorApplication.cpp` and `RuntimeApplication.cpp` include
  what they use. `EditorLayer.h` dragged every module into every UI TU.
  - Engine's `Paths` become `Strata/Engine/**`.
  - Engine's `MayInclude` narrows to Core, Input, App, Asset, Reflection, Scene, Renderer, Physics, Audio, Animation and
    Scripting: no Project, Runtime, Network or ImGui, so the Engine library links no tooling. world-partition and game-ui
    append `World` and `GameUI` to it (a union at merge).

**Every program lists its modules** (`specification.Modules`):

| Program | Modules |
| --- | --- |
| StrataEditor, windowed | Graphics, Audio, ImGui (layout file), Project |
| StrataEditor `--headless` (GPU, offscreen) | Graphics, Audio (null device), Project |
| StrataEditor `--no-gpu` | Audio (null device), Project |
| StrataRuntime, windowed | Graphics, Audio, Project. No ImGui. |
| StrataRuntime `--headless` | Audio (null device), Project. No window, no Graphics. |
| StrataTests' `Core.Application` cases | none, or Audio (null device) for the audio case |

- `ApplicationEditorHost` reaches the device, UI scale and screenshots through `GetModules().Find<GraphicsModule>()` and
  `Find<ImGuiModule>()`.
- Without the Graphics module, a screenshot request is answered with an empty image on the next frame
  (`SubmitToMainThread`), as captures are answered today without a device.

**Each program sets its `JobSystemSpecification`.**

- StrataRuntime: workers as today, 2 I/O and **2 background threads**. Its only background work is
  `Scene::ReleaseInBackground`.
- The editor and the tests keep the defaults: imports and compression use the background pool.
- Measure the minimal game's thread count and private memory, headless and windowed, before and after.

### 2. Physics' mesh geometry through the asset layer (allowlist 0)

The last allowlist line is `Physics/AssetMeshProvider.cpp -> Renderer/Mesh.h`.

- New `Asset/MeshGeometry.{h,cpp}`:
  - `struct MeshGeometry { std::vector<glm::vec3> Positions; std::vector<uint32_t> Indices; }`;
  - `using MeshGeometryReader = bool (*)(const Asset&, MeshGeometry&)`;
  - `MeshGeometryRegistry::BeginRegistration/Register(AssetType, MeshGeometryReader)/Read(const Asset&, MeshGeometry&)`.
- The composition root opens it with the other registries.
- `RegisterRendererModule` registers the `Mesh` reader. `CreateMeshData`'s logic moves into it: the full-detail LOD of
  every submesh, indices offset by `BaseVertex`, with the same range checks.
- `AssetMeshProvider` reads through the registry and moves the result into `PhysicsMeshData`. That is one copy, as today.
- The allowlist becomes empty (comments only) and `MaxAllowlistEntries` drops to 0. `Strata_Physics` never links
  `Strata_Renderer`.

### 3. The layer table becomes the build

**`CMake/StrataLayers.cmake`**, `strata_add_layer_libraries(TABLE <Layers.json> SOURCE_ROOT Strata/src)`, called from
`Strata/CMakeLists.txt`:

- It reads the table with `file(READ)` and `string(JSON)`.
- It globs every engine source once (`CONFIGURE_DEPENDS`; the extensions `LayeringAnalyzer` reads: `.h .hpp .inl .c .cpp
  .m .mm`) and drops other operating systems' `Platform/<OS>/` folders as today.
- It assigns each file to the one layer whose `Paths` match and whose `Exclude` does not.
  - The glob-to-regex conversion has the same semantics as `MatchesGlob`: `**/` is any directories or none, `**` is
    anything, `*` and `?` stay within one path component.
  - A file in no layer or in two fails the configure, naming it: the same rule `Architecture.Layering` enforces.
- One `STATIC` library per built layer: `Strata_<Layer>`, with an `ALIAS Strata::<Layer>`. The prefix avoids the
  executable `StrataRuntime`.
  - Each library gets `strata_configure_target`, a PCH (part 5) and `FOLDER "Strata/Layers"`.
  - It links `PUBLIC` exactly the built layers of its `MayInclude`. A library can only link what its layer may include.
  - It links the shared code-generation target described below.
- New optional keys in `Layers.json` (the analyzer ignores unknown keys):
  - `"Tooling": true` marks layers games never contain: AssetPipeline, Network, ImGui. Their libraries are
    `EXCLUDE_FROM_ALL`.
  - `"Option": "STRATA_WITH_PHYSICS"` (and `_AUDIO`, `_ANIMATION`, `_SCRIPTING`) marks optional layers (part 6).
- The description says that the table is the build.
- `Strata` becomes an `INTERFACE` umbrella of every built non-tooling layer, so existing `target_link_libraries(... Strata)`
  lines keep working. `StrataTooling` is `INTERFACE` for the tooling layers.
- CMake writes `<build>/Strata/LayerLibraries.json`: each layer's target, sources and linked layers. The
  `Architecture.Libraries` test reads it.
- `World` (world-partition) and `GameUI` (game-ui) become libraries from their `Layers.json` entries alone.

**Vendor and system libraries attach to the layers that use them.** A table in `Strata/CMakeLists.txt`,
`strata_layer_link(<Layer> PUBLIC|PRIVATE ...)`, kept where OS conditions are easy:

| Layer | Links |
| --- | --- |
| Core | PUBLIC `glm`, `EnTT`, `spdlog::spdlog`, `nlohmann_json::nlohmann_json`, Tracy when enabled. PRIVATE `shell32 ole32 bcrypt dbghelp` (Windows), CoreFoundation (Apple), Threads and dl (Linux). Generated `Version.h`/`VersionCommit.h` and `StrataVersionCommit`. |
| App | PRIVATE `glfw`, `Vulkan::Headers` (`VulkanLoader`), `stb` (window icon) |
| Asset | PRIVATE `zstd_decompress` |
| Renderer | PUBLIC `nvrhi`. PRIVATE `nvrhi_vk`, `Vulkan::Headers`, `glfw`, `stb`, `MikkTSpace`, `meshoptimizer`. The shaders (`strata_add_shaders(Strata_Renderer ...)`, same list and format) and the default font. |
| Physics | PRIVATE `Jolt` |
| Audio | PRIVATE `miniaudio` |
| Scripting | PRIVATE `StrataScriptCore` (`ScriptABI.h` only) |
| Network | PRIVATE `ws2_32` (Windows) |
| ImGui | PUBLIC `imgui` |
| AssetPipeline | PRIVATE `stb`, `cgltf`, `meshoptimizer`, `zstd_compress` |

- **Code generation stays identical.** Today Jolt's PUBLIC options apply to every Strata TU, because Strata links Jolt
  privately and compiles with Jolt's flags:
  - `/arch:AVX2` on MSVC x64; `-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c` on x86 GCC and Clang;
  - `NDEBUG` in Release and Dist, which turns off EnTT's and glm's `assert`s;
  - the `JPH_*` definitions.

  One `INTERFACE` target, `StrataEngineCodegen`, copies Jolt's `INTERFACE_COMPILE_OPTIONS`/`DEFINITIONS` (or sets the same
  options when Physics is not built) and is linked `PRIVATE` by every engine layer.
  - Check: a script compares `compile_commands.json` of the base and the stream for every engine TU. Flags must be
    identical except include directories and PCH flags.
- Programs:
  - `StrataRuntime` links `Strata`.
  - `StrataEditorCore` links `Strata`, `Strata::AssetPipeline` and `Strata::Network`.
  - `StrataEditorUI` adds `Strata::ImGui`, and `ImGuizmo` and `nfd` PRIVATE. ImGuizmo leaves the engine.
  - `StrataCLILib` links `Strata::Network` only (Core comes with it). The CLI includes only Core and Network headers
    today.
  - `StrataTests` links `StrataEditorUI` (or `Strata` and `StrataTooling` without the editor), plus `ImGuizmo` for its UI
    test.
- `Strata/vendor/CMakeLists.txt`:
  - `imgui`, `ImGuizmo`, `cgltf`, `zstd_compress` and `nfd` become `EXCLUDE_FROM_ALL`.
  - `Jolt` is added only with Physics and `miniaudio` only with Audio.
  - `StbImplementation.c` splits into one implementation file per stb header in `Implementations/`, still in the `stb`
    library, so the linker leaves out what the runtime never calls (`stb_dxt`, `stb_image_resize2`). Keep
    `stb_rect_pack` and `stb_truetype` in one file: stb_truetype uses rect_pack's packer when it sees it.
- `Platform/Vulkan/VulkanGraphicsDevice.cpp` references `nvrhi::validation::createValidationLayer` only
  `#if !defined(ST_DIST)`. In Dist, a validation request logs one warning and creates the plain device.
- Generated directories stay at `build/<preset>/Strata/Generated` and `build/<preset>/Strata/EmbeddedShaders`. The GCC
  lint invocation in common.md section 13 keeps working.

**Proof that games link no tooling.**

- At configure time, `strata_check_link_closure(StrataRuntime FORBID ...)` walks `LINK_LIBRARIES` and
  `INTERFACE_LINK_LIBRARIES` and fails the configure if the closure holds:
  - a `Tooling` layer;
  - `imgui`, `ImGuizmo`, `nfd`, `cgltf` or `zstd_compress`;
  - `StrataEditorCore`, `StrataEditorUI`, `StrataCLILib` or `doctest`.
- A CTest `StrataRuntime.NoTooling`:
  - The runtime is linked with a map file (`/MAP`, `-Wl,-Map=`, `-Wl,-map,`) next to it. Exports copy the executable only.
  - `StrataTests/Architecture/CheckNoTooling.cmake` fails when the map names any of: ImGui, ImGuizmo, the AssetPipeline
    objects (`EditorAssetManager`, `AssetImporters`, `GltfImporter`, `TextureImporter`, `TextureCooker`, `BC7Encoder`,
    `AssetPackWriter`, `AssetDependencies`), Network objects (`RpcServer`, `RpcClient`, `JsonRpc`, `EditorSession`,
    `*Socket`), or zstd's compressor.
  - It runs on all three platforms (the map formats differ; the check matches object and library basenames).
- Push `ci/engine-modules` through the orchestrator as soon as the libraries build on Windows. Fix what GNU ld and Apple ld
  report: static-library order follows the layer DAG, and registration is explicit, so nothing self-registers. Do this
  before the component move.

### 4. Module-owned components

**Where components go.**

- Scene keeps:
  - **types and registrations** of ID, Name, Transform, Relationship, Tag, Inactive, PrefabInstance (authoring-and-playtest
    adds its override list there) and Camera. Camera stays because `Scene::GetPrimaryCameraEntity` and the audio listener
    fallback read it.
  - the runtime-only `WorldTransformComponent`.
  - two **types registered by their owner module**, because a sibling layer reads them:
    - `MeshRendererComponent`: Physics' mesh colliders default to its mesh, Animation's preview and pose bounds read it, and
      the renderer's proxies follow its signals. `RegisterRendererModule` registers it.
    - `SkinComponent`: `RenderScene` connects to its signals to make skinned proxies, and the renderer must not include
      Animation. `RegisterAnimationModule` registers it.

  `Scene/Components.h` keeps its name. Most of its 114 includers only need Transform and Name, so they do not change.
- New headers, each with a `.cpp` that registers its components (called from the module's `Register<Module>Module` and
  declared in its `<Module>Registration.h`):
  - `Renderer/RendererComponents.h`: DirectionalLight, PointLight, SpotLight, SkyLight (+`SkyLightSource`), PostProcess
    (+`TonemapOperator`), Text (+`TextAlignment`).
  - `Physics/PhysicsComponents.h`: RigidBody (+`RigidBodyType`), Box/Sphere/Capsule/MeshCollider, CharacterController.
  - `Audio/AudioComponents.h`: AudioSource, AudioListener.
  - `Animation/AnimationComponents.h`: Animator (+`AnimatorCullingMode`), AttachToJoint.
  - `Scripting/ScriptingComponents.h`: Script, `ScriptEntry`, `ScriptFieldValue`, their `Find*` functions and the Script
    component's JSON (de)serialization.
- **Type names, registration names, categories, flags, properties and serialization stay exactly as they are.**
  - Registration order is today's: the composition root registers Scene, Renderer, Physics, Audio, Animation, then
    Scripting. `ComponentRegistry::GetAll()` lists the 27 components in the order they have now, so the Inspector, Add
    Component and `component.list` are unchanged.
  - This needs soft constraints, because Physics and Animation name Scripting, which now registers last.

**Prerequisites, each its own commit before the move:**

- **Soft ordering constraints.** `SceneSystemDescriptor` gains `AfterIfPresent` and `BeforeIfPresent`.
  - `ComputeUpdateOrder` adds their edges for registered systems and ignores absent names.
  - A system registered later picks up the edges that name it (the order is recomputed over every registered descriptor,
    as today). Cycles through soft edges are refused like hard ones.
  - `Unregister` is not blocked by soft references.
  - Hard `After`/`Before` keep refusing absent names: they are for a required module.
  - Policy (documented in `SceneSystem.h` and AGENTS.md): constraints toward another module's system are soft; hard ones
    are for systems of one module or of a module it cannot work without.
  - The composition root logs one info line: the update order and any soft constraints that named absent systems.
  - Physics `AfterIfPresent {"Scripting"}`. Audio `AfterIfPresent {"Physics"}`. Animation `AfterIfPresent {"Scripting"}`,
    `BeforeIfPresent {"Physics"}`.
- **Components describe their extra references.** `ComponentInfo::ExtraReferences` (Reflection) has four functions:
  - `Visit(const void*, visitor(slot, subslot, PropertyType, UUID))`;
  - `Set(void*, slot, subslot, UUID)`;
  - `Equals(const void*, const void*, remap)`;
  - `VisitJson(const nlohmann::json& extra, visitor(PropertyType, UUID))`.

  `ComponentInfoBuilder::ExtraReferences(...)` sets them. This stream is the only owner of extra-reference hooks:
  world-io adds `ExtraBinary` (binary extras for its cooked scenes) beside them and uses `ExtraReferences` for references
  after its rebase. Generic code then never names `ScriptComponent`:
  - `SceneSerializer`'s `RemapEntityReferences`;
  - `EntityPrototype`'s reference list (`EntityReference` keeps the info, slot and subslot);
  - `SceneInstantiation`'s reference writes;
  - `Prefab`'s comparison (`ScriptsMatch` becomes `Equals`);
  - `AssetDependencies::ScanEntities`.

  The Script registration supplies the four functions. Existing prefab, copy, paste and dependency tests prove the
  behavior.
- **Gizmos the modules fill.** `Reflection/ComponentGizmo.h` defines:
  - `ComponentGizmoShape { Box, Sphere, Capsule, MeshBounds; Transform, HalfExtents, Radius, HalfHeight, Mesh }`;
  - `ComponentGizmoGroup::Collider`;
  - `ComponentInfoBuilder::Gizmo(group, bool (*)(const entt::registry&, entt::entity, const void* component, const
    glm::mat4& world, ComponentGizmoShape&))`.

  `Renderer/SceneGizmos.cpp` draws every registered collider gizmo through `registry.storage(info->TypeId)`, with today's
  culling, colors and draw order (registration order: box, sphere, capsule, mesh, controller).
  - A mesh collider's shape is `MeshBounds` with the resolved mesh handle (its own, else the MeshRenderer's). The renderer
    keeps drawing the loaded mesh's bounds and nothing while the mesh loads.
  - The shape math (scale rules, offsets, the upright controller capsule) moves out of the renderer with the
    registrations. `Renderer.DebugDraw` tests pass unchanged.
- **Cross-module reads through services** (`Scene/SceneServices.h`, appended functions):
  - `IBodyMotionQuery::GetSimulatingBody(Entity) -> Entity`: the entity whose body moves this one, itself or its nearest
    ancestor with a RigidBody. Audio's Doppler (`AudioSystem::GetBodyVelocity`) calls it, then `GetPointVelocity`.
  - `IBodyMotionQuery::IsDynamicBody(Entity)`: `RequireBody` in the script host.
  - `IAudioPlayback::HasSource(Entity)`: `RequireAudioSource`. With no `IAudioPlayback` it reports "the scene plays without
    audio".
  - `IAnimatorControl::HasAnimator(Entity)`: `RequireAnimator`.

  Problem messages stay word for word wherever the module is present (`c_ExpectedLogMessages`).
- **Shared property helpers.** `Range`, `Slider`, `Color`, `WithTooltip` and `c_Unbounded` move from
  `ComponentRegistration.cpp`'s anonymous namespace to `Reflection/PropertyOptions.h`. `PhysicsLayer(...)` goes to Physics.
- **A schema snapshot**, committed before the move from the base's registry:
  - `StrataTests/Architecture/ComponentSchema.json`, written by `--strata-test-helper=write-component-schema`. It records
    each component's name, display name, category, flags, description, and each property's name, type, flags, options and
    enum values.
  - Test `Reflection.ComponentSchema`: every component in the snapshot exists with identical metadata and in the same
    relative order. Components it does not list (other streams' new ones) are allowed.

**The move itself is one mechanical commit:**

- types and registrations cut and pasted verbatim into the module files;
- `Scene/ComponentRegistration.cpp` keeps Scene's;
- `Components.cpp`'s Script functions move to `ScriptingComponents.cpp`;
- `Entity.h`, `SceneRenderer.h`, `TextLayout.h` and `AudioSystem.h` include what they now need;
- include lines change in engine, editor and test files;
- `BuiltinModules.cpp` changes its call order.

If the integrator has to redo it on top of the other streams (the fallback in plan.md), this commit is the recipe.

### 5. Build and test throughput

**Precompiled headers.**

- `stpch.h` gains `<entt/entt.hpp>` and `<nlohmann/json.hpp>`.
- Every layer library builds its PCH from `stpch.h`. Vendor definitions differ between layers, so `REUSE_FROM` across
  layers is not reliable on MSVC with `/WX` (C4605).
- `StrataEditorCore` and `StrataEditorUI` get `StrataEditor/src/EditorPch.h`: `stpch.h`, plus `imgui.h` for UI.
- `StrataTests` gets `StrataTests/src/TestsPch.h` (`stpch.h`, doctest, glm). Alternatively it can `REUSE_FROM
  StrataEditorUI`: move its `STRATA_TEST_*` definitions into a generated `TestConfig.h` so that its flags match. Pick by
  measured CPU time.
- PCHs hold only the standard library, vendor headers and Core basics (`Base.h`, `Log.h`, `Assert.h`, `Profiling.h`),
  never a header that changes often.
- Sources and headers still include what they use. The GCC lint compiles without PCH.
- Option `STRATA_PRECOMPILED_HEADERS` (default ON).

**Lighter headers.**

- The 18 engine headers that include `nlohmann/json.hpp` switch to `<nlohmann/json_fwd.hpp>` where they only declare JSON
  parameters or returns. Headers with JSON members keep the full header.
- `EditorContext.h` stops including headers whose types it uses only by reference, pointer or `Scope` member. Candidates:
  `GameExport.h`, `ScriptBuild.h`, `ThumbnailCache.h`, `SceneRecovery.h`, `ProjectThumbnail.h`, `ExternalEditor.h` and
  `CommandJournal.h`. Members become `Scope<>` with the destructor in the `.cpp`. Report each narrowed header's dependents
  before and after.

**`StrataTests.Core` becomes about ten CTests.**

- `StrataTests/CoreShards.json` lists shards: `{ "Name", "Suites": [doctest filters], "Exclude": [...] }`. It also lists
  the suites that have their own CTests (`GPU*`, `EndToEnd*`, `Package*`, `Perf.*`, `PerfGPU.*`, `FeatureTest`,
  `Editor.FeatureTest`).
- CMake makes one CTest `StrataTests.Core.<Name>` (label `core`) per shard. `ctest -R StrataTests.Core` still selects them
  all.
- First split, tuned so that no shard runs over about 60 s in Debug. Release seconds of the base:

  | Shard | Suites | Release s |
  | --- | --- | --- |
  | Core | Core, Math, Reflection, Engine, Architecture, PerfLab, TestHelpers, Testing | 13 |
  | Asset | Asset | 45, about 15 after sampling |
  | Scene | Scene, Project, Animation | 19 |
  | Systems | Physics, Audio, Scripting | 11 |
  | Renderer | Renderer, ImGui, Runtime | 10 |
  | Network | Network, CLI | 19 |
  | EditorUI | `Editor.UI*`, `Editor.Theme*` | about 35 |
  | EditorData | `Editor.Recovery`, `Replay`, `DataSafety`, `Launcher`, `Export`, `Scripts`, `ScriptWorkflow`, `Automation` | about 25 |
  | Editor | `Editor*` minus the above | about 25 |

- Test `Testing.Shards`, "Every suite runs in exactly one Core shard or in its own CTest":
  - It reads `CoreShards.json` and the registered suites: doctest's registry, or the binary's own `--list-test-suites`.
  - It matches doctest's filter semantics: `*` and `?`, case-insensitive.
  - It fails naming each suite in no shard or in two. New suites of other streams then land in a shard.
  - Patterns matching no suite are only reported. Pre-add `World*` to the Systems shard and `GameUI*` to the Renderer
    shard now, so world-partition's and game-ui's suites land without a `CoreShards.json` edit.
- `RESOURCE_LOCK`:
  - Audit every CTest for shared ports, files and directories, and lock what is shared.
  - Default `toolchain` lock for every `package`-label test: concurrent MSBuild and mspdbsrv runs are a known source of
    flaky C1090 and LNK errors.
  - Add a `gpu` lock only if parallel runs show device errors (history: VkInstance churn under load).
  - Perf tests stay `RUN_SERIAL`.
  - Shake the result out with 10 parallel runs (`ctest -j8 --repeat until-fail:10` on the Core shards).
- **Corruption sweeps.**
  - The byte-offset sweeps move into suites ending in `.Sweep`: `Asset.Pack.Sweep` (both pack corruption cases) and
    `Renderer.Font.Sweep` (randomly corrupted fonts). `Scene.Transforms.Sweep` gets the randomized cache test.
  - By default they test a deterministic sample: every offset in the headers and tables, ±8 bytes around each 4 KiB block
    boundary the format has, and 512 seeded random offsets. Use `Tests::SelectSweepOffsets(size, mustInclude)` and
    `Tests::IsFullSweep()` (`STRATA_TEST_SWEEP=full`).
  - The new CTest `StrataTests.Fuzz` (`CONFIGURATIONS Release Dist`, label `fuzz`, `--test-suite=*.Sweep`) runs the full
    sweeps. Every Release `ctest -LE perf` (the orchestrator's verification, CI's Release jobs) includes it, so no
    offset a sweep checks today goes unchecked: the sample makes Debug faster, it does not weaken a test.
  - `*.Sweep` suites run sampled in their Core shard and in full in `StrataTests.Fuzz`: `Testing.Shards` treats Fuzz as
    an extra run of those suites, not as a second shard.

**CI** (`.github/workflows/ci.yml`, on top of step 0's Windows Dist job):

- **A compiler cache for every job.** Option `STRATA_COMPILER_CACHE` (default OFF):
  - when set, it uses `ccache` or `sccache` as `CMAKE_<LANG>_COMPILER_LAUNCHER`;
  - on MSVC it switches debug information to `Embedded` (`/Z7`), which the caches need;
  - it turns PCH off by default, because compiler caches do not cache MSVC PCH builds.
- Linux and macOS: ccache.
- Windows: sccache.
  - If the runner's CMake honors the launcher with the Visual Studio generator, keep `windows-vs2022`.
  - Otherwise, cached Windows jobs configure a new `windows-ninja` preset (Ninja Multi-Config, `cl`) inside the MSVC
    developer environment.
  - One Windows job keeps the Visual Studio generator people build with: the Dist job.
- Caches are keyed per job, restored from main's, and kept below GitHub's 10 GB.
- If no launcher works on Windows, the fallback is caching the vendor libraries by submodule SHA.
- **A Debug build-and-test job on Windows:** `ctest -LE "gpu|perf" -j 4`.
- Report cold and warm build times per job (`ci_durations.py`).

**`Perf.Jobs`** (`StrataTests/src/Perf/JobsPerfTests.cpp`, under `Perf::ScopedApplicationJobSystem`), checked with
`Perf::CheckBudgetsConfirmed`. Every metric is in µs, a median, lower is better:

- `Perf.Jobs.SubmitWait`: one empty job `Submit` plus `Wait` from the main thread, timed in batches of 1,000, median of 30
  batches.
- `Perf.Jobs.SubmitMany`: 100,000 empty jobs submitted, then `WaitAll`, per job.
- `Perf.Jobs.ParallelForOverhead`: `ParallelFor` over workers × 64 items with an empty body and a batch size of 1, per
  call, median of 200 calls.
- `Perf.Jobs.WakeLatency`: from `Submit` to the job's first instruction, after 50 ms of idle (parked workers). Median of
  100 samples: a latency's tail is the machine, not the code.

Budgets come from locked measurements and are marked "to confirm in the integration campaign".

### 6. Optional subsystems

- CMake options `STRATA_WITH_PHYSICS`, `STRATA_WITH_AUDIO`, `STRATA_WITH_ANIMATION` and `STRATA_WITH_SCRIPTING`, default
  ON.
  - Each sets a PUBLIC compile definition on `Strata_Core`: `ST_WITH_PHYSICS=1/0` and so on. A definition, not a generated
    header, so a missing include can never read as 0.
  - A layer whose option is OFF is not built. Links to it are skipped, and its vendor library is not added.
  - Physics or Audio OFF requires `STRATA_BUILD_EDITOR=OFF`: the configure fails with that message. The editor is a tool
    with every module.
- **`BuiltinModules.cpp` becomes a table**, `{ "Scene", RegisterSceneModule }, { "Renderer", RegisterRendererModule }`,
  then Physics, Audio, Animation and Scripting, each under `#if ST_WITH_<X>`. world-partition and game-ui add one row each.
  - `ModuleRegistrationOptions::Exclude` (names, refused for Scene and Renderer) lets tests register a subset in a full
    build.
  - `Engine::GetRegisteredModules()` lists what registered.
- **`#if ST_WITH_<X>` appears only in:**
  - the composition root's table;
  - `AssetImporters.cpp` (the audio importer);
  - `GltfImporter.cpp` (without Animation, skins and animations are skipped features with a warning, as before wave 3);
  - `AssetDependencies.cpp` (controller clips);
  - `GameRuntime` (without Scripting, a manifest naming a script module fails to start with "this runtime was built
    without scripting");
  - tests.

  Module code reaches other modules only through services, so it needs no guard.
- Scenes keep the components of absent modules (`UnknownComponentsComponent`, existing). Scripts' calls into absent
  services report the missing module and return their failure values.
- Tests:
  - With a module OFF, CMake leaves out the test folders `src/<Module>/` and the feature-test runners (they need every
    module).
  - Test cases elsewhere that use the module are guarded with `#if ST_WITH_<X>`: about 20 files in `Scene/`,
    `Scripting/`, `Animation/`, `Renderer/DebugDrawTests.cpp`, `Core/ApplicationAudioTests.cpp` and
    `Asset/EditorAssetManagerTests.cpp`.
- A CI job, **"Linux (no Audio, no Physics)"**: `-DSTRATA_WITH_AUDIO=OFF -DSTRATA_WITH_PHYSICS=OFF
  -DSTRATA_BUILD_EDITOR=OFF`, Release, `ctest -LE "gpu|perf|package"`.
  - Check locally once that the all-four-OFF configuration builds and passes its Core shards.
- Network is tooling (part 3): no game contains it.

### 7. Docs and skills (in the commits that change the behavior)

- **`Docs/Architecture.md`:**
  - "Targets and dependencies": the layer libraries, `Strata`/`StrataTooling`, the vendor table, a new graph, the
    NoTooling check.
  - "Engine modules": registration functions versus EngineModules, the built-in modules, where components go.
  - "Layers": the table as the build, `Tooling` and `Option`, allowlist 0.
  - "Composition root and registries": the entry point, the module table, `Exclude`, soft constraints, the new registries.
  - "Application and frame loop": the hook table above. Delete "The runtime has no ImGui" or make it true by pointing at
    NoTooling.
- **`AGENTS.md` (rules only):**
  - where components go;
  - how to add an engine module (and that `Application` never calls module code);
  - how to add a layer (`Layers.json` makes the library; vendor links in the `strata_layer_link` table);
  - soft versus hard constraints;
  - the PCH content rule;
  - `json_fwd.hpp` in headers;
  - the build options (`STRATA_WITH_*`, `STRATA_PRECOMPILED_HEADERS`, `STRATA_COMPILER_CACHE`);
  - a new suite needs a `CoreShards.json` entry;
  - `ctest ... -j 8`.

  `StrataTests.Core` becomes `StrataTests.Core.<Shard>`.
- **Skills:**
  - `strata-add-component`: the component in its module's files, registered by the module, and Scene only for a type a
    sibling must read.
  - `strata-build-test`: the shards, `-j 8`, the options, the `fuzz` label, the no-module configuration, NoTooling.

## Acceptance criteria

Must-fail tests: confirm that each fails on the base, or with its part reverted, and say how.

- **`Core.EngineModules`** (`StrataTests/src/Core/EngineModuleTests.cpp`, new):
  - modules start in dependency order (listing order where free) and release and shut down in reverse;
  - each frame calls `OnBeginFrame` forward, the layers, `OnUpdate` and `OnRenderUI` forward, `OnEndFrame` in reverse;
  - `Skip` skips a frame without counting it and `Close` ends the loop;
  - a failing `OnInit` gives exit code 1, a log line naming the module, and `OnRelease`/`OnShutdown` only for the modules
    that started;
  - a missing required dependency, a cycle or a duplicate name refuses to start, naming them;
  - events reach the modules in order before the layers, and a handled event stops;
  - the wait a module reports is left out of `GetLastFrameWorkTime`;
  - `Find<T>`.

  Existing `Core.Application` cases pass with module lists.
- **`Architecture.Layering`:**
  - `MaxAllowlistEntries` 0 (1 only if the mesh inversion slips, kept as an explicit, listed link exception);
  - no `MayInclude` widened for a sideways edge (Audio→App and Project→App are downward, with Notes);
  - Engine's `MayInclude` narrowed.
- **`Architecture.Libraries`** (new, reads `LayerLibraries.json`): every engine source compiles into the library of its
  layer (same assignment as the analyzer, this OS's platform folders), and each library links exactly the built layers of
  its `MayInclude`.
- **`StrataRuntime.NoTooling`** passes on Windows, Linux and macOS. On the base build it fails (imgui and ImGui layer
  objects in the map).
  - The configure-time closure check refuses a runtime that links a tooling target.
  - Report the runtime's Release and Dist sizes before and after. Expect at least the 425 KiB of imgui and the ImGui layer
    less, plus NVRHI validation (131 KiB) in Dist and stb's encoders.
  - Report the CLI's size before and after.
- **Threads and memory** (Release, the audit's 2-asset smoke game, `smoke_run.ps1` method, 3 runs each, base and stream
  interleaved under the perf lock):
  - thread count from the JobSystem log line and the process: 48 engine threads before, at most 28 after;
  - private memory headless (15.7 MB before) and windowed (335 MB before; check for overlays, common.md section 9).

  No regression.
- **Composition root:**
  - `Engine.Modules` checks the registered module list and order.
  - A helper `--strata-test-helper=register-without <modules>` runs the composition root with `Exclude`:
    - without Physics, Audio and Animation register and a scene plays;
    - the order is Scripting, Animation, Audio;
    - physics components stay as unknown components and save back unchanged;
    - the API module's physics calls report the missing module and return failure.
- **`Scene.Systems`:**
  - the existing case "The built-in systems update in the order Scripting, Animation, Physics, Audio" passes with the new
    registration order;
  - new cases: soft constraints order present systems, ignore absent ones, apply to systems registered later and refuse
    cycles; soft references do not block `Unregister`.
- **Components:**
  - `Reflection.ComponentSchema` passes unchanged across the move commit;
  - `Scene.Serialization`: every committed scene loads with no unknown components and no warnings, and re-serializes
    stably (`Feature.stscene`, `FeatureSub.stscene`, `Samples/Tetris/Assets/Scenes/Main.stscene`; the `basic3d` template
    scene in an `Editor.*` case, since templates belong to the editor);
  - all feature runners pass (coverage needs every component in `Feature.stscene`);
  - `Renderer.DebugDraw`, prefab, copy, paste and dependency tests pass unchanged;
  - new tests for `GetSimulatingBody`, `IsDynamicBody`, `HasSource` and `HasAnimator`, and for a custom component's gizmo
    and extra references (the `custom-component` helper mode).
  - An edit of each moved header rebuilds only its dependents. Report the object counts for `PhysicsComponents.h`,
    `RendererComponents.h`, `AudioComponents.h`, `AnimationComponents.h`, `ScriptingComponents.h` and `Scene/Components.h`
    against 251. Target: each module header at most 125.
- **Build times**, measured with the audit's method:
  - `audit\architecture\build.ps1`: out-of-source Ninja Multi-Config plus `cl`, 24 jobs;
  - `ninjalog.py`: CPU seconds summed from `.ninja_log`;
  - `incremental.ps1`: deletes an edit's dependents and rebuilds.

  Measure the step-0 base and the stream under the perf lock, 2 interleaved pairs.
  - Clean Debug CPU time down 30% from the base (target; 3,099 CPU-s at 685aef7).
  - Report the wall time and the incremental cost of `Scene/Components.h`, `EditorContext.h` (99 objects, 42.7 s before),
    `Scene.cpp` and `InspectorPanel.cpp`.
- **Tests:**
  - `ctest --preset windows-debug -j 8` under the validation layer takes at most half of 658 s (target, on the base's
    machine state). Report the base and the stream back to back.
  - `Testing.Shards` passes and fails when a suite is unsharded: confirm with a temporary suite.
  - 10 parallel repeats of the Core shards pass.
  - `StrataTests.Fuzz` passes in Release.
- **`Perf.Jobs`** entries in `Budgets.json`, set from locked measurements, "to confirm in the integration campaign".
- **No behavior change:**
  - Smoke, Export, Scripts, Install, Tetris and the feature runners (`StrataTests.FeatureTest`, `GPU.FeatureTest`,
    `StrataEditor.FeatureTest`, `StrataRuntime.FeatureTest`) pass.
  - `compile_commands.json` flags match the base for every engine TU (apart from include directories and PCH).
  - Interleaved A/Bs under the perf lock, 3 pairs each, ratio of medians, no regression beyond noise:
    - `PerfGPU.Editor.{Edit,Play}FrameNested{100k,1M}` (single test case);
    - editor `startupSeconds` (`editor.status`);
    - the runtime's first frame (row 0 `WorkMs` of `--perf-log`, windowed 1280x720);
    - `Perf.Scene.*`.
- **Dist** builds every target with warnings as errors.
- **CI** is green on Windows, Linux and macOS for `ci/engine-modules`, including the new Windows Debug job and the
  "Linux (no Audio, no Physics)" job.
- **Screenshots** (`scratchpad\wave4\engine-modules\shots`, base and stream side by side): look at them and confirm that
  they match.
  - The Release editor on a FeatureTest copy, the feature scene's entity with the most components selected (Inspector order
    and Add Component menu), at 150% and `--ui-scale 1`.
  - The exported smoke game windowed (the runtime renders the same frame).

## Deliver in this order if time runs short

1. **EngineModule lifecycle** (part 1): the interface and registry, the four built-in modules, the entry point and
   `ApplicationLaunch`, the module lists, the runtime's job counts, the `Core.EngineModules` tests, the 7 allowlist lines
   removed. This closes the first major finding and unblocks part 3.
2. **The mesh-geometry inversion** (part 2): allowlist 0.
3. **Per-layer libraries** (part 3): `StrataLayers.cmake`, the vendor table, `Strata`/`StrataTooling`, `Strata.h` removed,
   NoTooling (configure check and CTest), the NVRHI validation reference, the stb split, `Architecture.Libraries`, then
   `ci/engine-modules` green on all three platforms.
4. **Test shards** (part 5, cheap and used by every stream): `CoreShards.json`, `Testing.Shards`, resource locks, sampled
   sweeps and `StrataTests.Fuzz`.
5. **Module-owned components** (part 4): the prerequisite commits (soft constraints, extra references, gizmos, services,
   property helpers, schema snapshot), then the one mechanical move.
6. **The rest of throughput** (part 5): PCHs, `json_fwd.hpp`, `EditorContext.h`, the CI cache and Debug job, `Perf.Jobs`,
   before/after build numbers.
7. **Optional subsystems** (part 6): options, the composition root table, guards, the CI job. Scripting OFF is the first
   thing to defer.

Docs and skills go with each commit (part 7).

## Open items this stream owns

**Closes:**

- Architecture (major): the application hard-codes renderer, audio, ImGui, assets and project; one static library and
  ImGui in every game; every module's components in Scene, and constraints that refuse absent systems; no PCH where 70% of
  the compile time is; `StrataTests.Core` as one serial CTest.
- Architecture (critical, the CI part left after step 0): the compiler cache and the Debug CI job.
- Scale (minor): `Perf.Jobs`.
- Roadmap open items: the allowlist's exceptions, and the policy for ordering constraints.
- Docs drift: "The runtime has no ImGui" becomes true. You may remove the allowlist and constraint items from Roadmap's
  "Open items"; say so in the report.

**Out of scope:**

- scene contexts: `AssetManager::GetActive`, `ScriptEngine::GetActive` in `ScriptingRegistration.cpp`, `GameRuntime`'s
  globals (wave 5 `scene-contexts`);
- the exported game's identity (wave 5);
- splitting command-registration files (common.md section 8);
- new features;
- `vulkan-raytracing` (51 KiB, NVRHI's Vulkan device references it; vendor code);
- ImGui's two shaders, which stay in the renderer's registry (one registry per build).

**Observed, not owned (report it):** the windowed smoke game's first frame spends 276 ms in the Input stage (`perf_window_1.csv`).
Name that cost for the integrator: wave 5 `game-shell`.

## Files expected to be touched

**CMake and CI:**

- `CMakeLists.txt`, `CMakePresets.json` (`windows-ninja` if needed);
- `CMake/StrataLayers.cmake` (new), `StrataCompilerOptions.cmake` (`StrataEngineCodegen`, PCH, compiler cache),
  `StrataShaders.cmake` (target argument only);
- `Strata/CMakeLists.txt`, `Strata/vendor/CMakeLists.txt`, `Strata/vendor/Implementations/Stb*.c`;
- `StrataEditor/CMakeLists.txt`, `StrataRuntime/CMakeLists.txt`, `StrataCLI/CMakeLists.txt`, `StrataTests/CMakeLists.txt`;
- `.github/workflows/ci.yml`.

**App and Engine:**

- `Strata/src/Strata/Core/Application.{h,cpp}`, `Core/EngineModule.{h,cpp}` (new), `Core/EntryPoint.h` (moved to
  `Engine/EntryPoint.h`), `Engine/ApplicationLaunch.h` (new);
- `Engine/BuiltinModules.{h,cpp}`, `Strata/src/Strata.h` (deleted), `stpch.h`.

**Modules:**

- `Renderer/GraphicsModule.{h,cpp}`, `RendererComponents.{h,cpp}`, `RendererRegistration.cpp`, `SceneGizmos.cpp`,
  `SceneRenderer.h`, `TextLayout.h`;
- `Platform/Vulkan/VulkanGraphicsDevice.cpp`;
- `Audio/AudioModule.{h,cpp}`, `AudioComponents.{h,cpp}`, `AudioRegistration.cpp`, `AudioSystem.{h,cpp}`;
- `ImGui/ImGuiModule.{h,cpp}`;
- `Project/ProjectModule.{h,cpp}`;
- `Physics/PhysicsComponents.{h,cpp}`, `PhysicsRegistration.cpp`, `AssetMeshProvider.cpp`, `PhysicsSystem.{h,cpp}`
  (services);
- `Animation/AnimationComponents.{h,cpp}`, `AnimationRegistration.cpp`, `AnimationSystem.*` (`IAnimatorControl::HasAnimator`);
- `Audio/AudioSystem.*` also implements `IAudioPlayback::HasSource` (its `PlaybackService`);
- `Scripting/ScriptingComponents.{h,cpp}`, `ScriptingRegistration.cpp`, `ScriptHostAPI.cpp` (the three reads);
- `Runtime/GameRuntime.*` (Scripting guard only), `Runtime/GameRenderer.cpp` (includes).

**Scene, Reflection and Asset:**

- `Scene/Components.{h,cpp}`, `ComponentRegistration.cpp`, `SceneSystem.{h,cpp}`, `SceneServices.h`, `SceneSerializer.cpp`,
  `EntityPrototype.{h,cpp}`, `SceneInstantiation.cpp`, `Prefab.cpp`, `Entity.h`;
- `Reflection/ComponentRegistry.h`, `PropertyOptions.h` (new), `ComponentGizmo.h` (new);
- `Asset/MeshGeometry.{h,cpp}` (new), `AssetDependencies.cpp`, `AssetImporters.cpp` and `GltfImporter.cpp` (guards).
- The 18 JSON-including headers (`json_fwd.hpp` where possible).

**Programs and editor:** `StrataRuntime/src/RuntimeApplication.cpp`; `StrataEditor/src/EditorApplication.cpp`,
`EditorLayer.h`, `Editor/EditorContext.{h,cpp}` (narrowing), `EditorPch.h` (new); include lines across `StrataEditor/src`.

**Tests:**

- `StrataTests/Architecture/Layers.json`, `LayeringAllowlist.txt`, `ComponentSchema.json` (new), `CheckNoTooling.cmake`
  (new);
- `StrataTests/CoreShards.json` (new);
- `StrataTests/src/Core/EngineModuleTests.cpp` (new), `Core/Application*Tests.cpp`, `Engine/*`,
  `Architecture/LayeringTests.cpp` and a libraries test, `Scene/SceneSystemTests.cpp`, `Reflection` schema test,
  `Testing/ShardTests.cpp` (new), `Perf/JobsPerfTests.cpp` (new), the sweep tests, `TestMain.cpp` (helper modes),
  `TestsPch.h` (new), and include lines across `StrataTests/src`;
- `StrataTests/Perf/Budgets.json`.

**Docs and skills:** `Docs/Architecture.md`, `AGENTS.md`, `.claude/skills/strata-add-component/SKILL.md`,
`.claude/skills/strata-build-test/SKILL.md`, `Docs/Roadmap.md` ("Open items" removals only).

## Merge notes and conflict risks

Merges **first**. What it guarantees to the streams that branch from the step-0 tip before it lands:

- **`Strata` stays a valid link target** (now an `INTERFACE` umbrella). Targets and `strata_add_shaders` lists keep their
  format, and generated directories keep their paths. A stream adding sources, shaders or tests in its own blocks rebases
  without CMake conflicts.
- **New layers** (`World`, `GameUI`) are `Layers.json` entries only.
  - On the base they compile into the single library. After the rebase they become `Strata_World`/`Strata_GameUI`, linking
    their `MayInclude`. Their streams append them to `Engine`'s `MayInclude` (and `World` to `AssetPipeline`'s).
  - A layer that needs a vendor library adds one `strata_layer_link` row; tell the integrator.
  - Their `Register<Module>Module` becomes one row in the composition root table, at the end (after Scripting), World
    then GameUI, so the component order stays today's with theirs after `Script`.
- **Components keep their type names.** Rebases change include lines only: `Physics/PhysicsComponents.h`,
  `Renderer/RendererComponents.h`, and so on. New components were in their own module files from day 0 (common.md
  section 2).
- **Scene systems** that name an optional module's system switch to `AfterIfPresent`/`BeforeIfPresent`, done by each
  later stream in its rebase: world streaming `AfterIfPresent {"Scripting"}`, `BeforeIfPresent {"Physics"}`; the UI system
  `BeforeIfPresent {"Scripting"}`.
- **Programs and helper modes** that construct an `Application` list their engine modules. `CreateApplication` returns an
  `ApplicationLaunch`.
- **New Core test suites** need a `CoreShards.json` entry. `Testing.Shards` names any that miss one.

Per stream:

- **world-io** (merges second):
  - It owns `SceneSerializer`, `EntityPrototype` and the `Prefab` read path. This stream changes their reference handling
    to the `ComponentInfo::ExtraReferences` hooks, in `RemapEntityReferences` and the prototype's reference list.
    world-io's streamed and binary writers rebase onto that. The Script component's extra data still goes through
    `SerializeExtra`/`DeserializeExtra`, unchanged.
  - world-io adds `ComponentInfo::ExtraBinary` (`SerializeExtraBinary`/`DeserializeExtraBinary`) beside `ExtraReferences`
    in `ComponentRegistry.h`, with the Script registration's binary hooks in one block; this stream's move carries that
    block into `ScriptingComponents.cpp` (or world-io's rebase does, whichever lands second).
  - `EditorApplication.cpp`: world-io changes session publishing (function-level); this stream changes `CreateApplication`
    and the `EditorApplication` class.
  - `EditorContext.h` narrowing may touch includes near world-io's save code: the integrator keeps both.
- **world-partition:**
  - It owns `Physics/*` and `GameRuntime`. This stream adds `PhysicsComponents.*`, gizmo registrations, two
    `IBodyMotionQuery` functions and the `MeshGeometry` reader path in `AssetMeshProvider`, and changes includes in
    `PhysicsWorld.cpp`/`PhysicsSystem.cpp`.
  - Expect include-line conflicts only. Its `World` layer and components follow the rules above, and its host block
    appends after this stream's merge (this stream appends no host function).
- **gpu-driven-rendering:**
  - It owns `Renderer/*` and the shaders. This stream adds `GraphicsModule.*` and `RendererComponents.*`, and changes
    `SceneGizmos.cpp` (registry loop), `RendererRegistration.cpp` (mesh reader), `VulkanGraphicsDevice.cpp` (validation
    under `ST_DIST`), `SceneRenderer.h`/`TextLayout.h` (includes) and `strata_add_shaders`' target.
  - GPU timers that need a per-frame hook go in `GraphicsModule::OnBeginFrame`/`OnEndFrame`, not in `Application`.
- **game-ui:**
  - `TextComponent` moves to `Renderer/RendererComponents.h`, and game-ui's `TextRenderer` changes rebase on that include.
  - Its `GameUI` layer is a library from `Layers.json`. Its UI system uses `BeforeIfPresent {"Scripting"}`.
  - Its `ComponentInfoBuilder::Requires` is a member beside this stream's hooks in `ComponentRegistry.h`.
  - Input capture for UI stays in its layers and systems, not in `Application`.
- **authoring-and-playtest** (merges last):
  - `PrefabInstanceComponent` (with its override list) stays in Scene.
  - Its `InactiveReason` hooks on the light, sky, post-process and animator registrations go into
    `Renderer/RendererComponents.cpp` and `Animation/AnimationComponents.cpp`, where this stream moved those
    registrations; authoring-and-playtest does that in its rebase (a Scene-layer file cannot name the moved types).
  - Its per-property prefab overrides rebase onto `ComponentInfo::ExtraReferences::Equals` (script fields) in
    `Prefab.cpp`.
  - Its StrataRuntime argument parsing (function-level) rebases onto `CreateApplication` returning `ApplicationLaunch`.
  - Its Game view uses the Graphics module for any device access.
  - The new commands go into the command-kind lists as usual; this stream adds no command.
- **Later streams in their rebase** (each merges after this one, so this is their scope): the `World`/`GameUI` rows of
  the composition root table, soft constraints for their systems, include lines of moved components, new suite prefixes
  in `CoreShards.json`, module lists of new programs and helper modes.
- **Integrator:**
  - re-run NoTooling after each merge: a stream that makes a runtime layer include a tooling header fails it at configure;
  - keep `MaxAllowlistEntries` at 0.

Conflict risks to watch: `Layers.json`, `BuiltinModules.cpp`, `StrataTests/CMakeLists.txt`, `SceneServices.h`,
`Budgets.json`, `AGENTS.md` and the skills (unions); `Prefab.cpp` and `SceneSerializer.cpp` (function-level); include
blocks everywhere after the move.

**Fallback:** if this stream is not merge-ready when the others are, it merges last. The integrator redoes the mechanical
move commit (part 4) on top of the merged tree, using the prerequisite commits and the schema snapshot as the check.

## Answers from the planner (cross-check of 2026-10-13)

1. **EngineModule in the App layer, Audio and Project widened with App:** accepted as specified. App lies below both and
   includes neither, so the edge is downward; write the reason in each layer's `Notes`. (An interface in Core would
   have to forward-declare `Event`, an Input-layer type, in a Core header.)
2. **`MeshRendererComponent` and `SkinComponent` stay in `Scene/Components.h`, registered by Renderer and Animation:**
   confirmed. common.md section 2 and the other specs now say so.
3. **`ExtraReferences` hooks versus `ScriptComponent` in Scene:** the hooks, owned by this stream. world-io drops its own
   entity-reference hook and adds only `ExtraBinary`; authoring-and-playtest's per-property comparison in `Prefab.cpp`
   rebases onto `ExtraReferences::Equals`.
4. **`CreateApplication` returning `ApplicationLaunch`, `EntryPoint.h` moved, `Strata.h` deleted:** accepted. common.md
   section 2 tells every stream not to touch `Strata.h`; world-io's and authoring-and-playtest's specs name the
   `ApplicationLaunch` change in their merge notes.
5. **`StrataTests.Core` becoming shards:** common.md section 10 now says so (CTest names and counts change after this
   stream merges). Pre-add `World*` and `GameUI*` (part 5).
6. **Physics- or Audio-off builds require `STRATA_BUILD_EDITOR=OFF`:** accepted. The editor is a tool with every module;
   the CI job builds the runtime and the tests.
7. **The `toolchain` resource lock and `StrataTests.Fuzz` in Release and Dist only:** accepted. The orchestrator's Release
   `-LE perf` run includes the `fuzz` label.
8. **`Layers.json` stays at `StrataTests/Architecture/Layers.json`:** yes; every spec and doc names that path.
9. **The audit's build scripts against the step-0 base worktree:** allowed, read-only on the worktree, building out of
   source into this stream's own scratch directory, under the perf lock.
