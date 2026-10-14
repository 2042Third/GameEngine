# Strata Engine — Agent & Developer Guide

Strata is a production-grade 3D game engine written in C++20 with CMake, targeting Windows, Linux
(Ubuntu 24+) and macOS. It renders through [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) on Vulkan,
uses GLFW for windowing, glm for math, EnTT for the entity-component system, Jolt for physics and
miniaudio for audio. Game logic is written in C++ script modules that are isolated from the engine
behind a C ABI (crash containment + hot reload).

This file is the source of truth for how to work on Strata. Read it fully before changing code. How the parts fit
together (targets, modules, frame loop, threading, asset pipeline, scripting, editor, export) is described in
[Docs/Architecture.md](Docs/Architecture.md).

Claude Code loads this file into every session, so it holds the rules every change follows and, for each area, a
digest of what must always or never be done there. The details are in [Docs/DeveloperGuide.md](Docs/DeveloperGuide.md):
read the guide's section for the area you work on before changing it (each digest links it). How the two files share
the work: checklist item 5.

## Golden rules

1. **Never use code from outside this repository as a reference.** Vendored dependencies under
   `Strata/vendor/` are part of the repository and may be read (to learn their APIs), never copied
   into engine code.
2. **Everything ships with tests.** New behavior gets unit tests; bug fixes get a regression test.
   All tests must pass before a commit. Never disable, skip or weaken a test to make it pass.
3. **Review before every commit.** Walk the [review checklist](#pre-commit-review-checklist) for the
   full diff. Fix findings before committing, not after.
4. **Follow the code style** below exactly (Hazel conventions).
5. **No hacks or shortcuts.** Handle every error path, no `TODO` stubs or placeholder implementations
   in committed code, no silenced warnings without a comment explaining why the warning is wrong.
6. **Keep the feature test current.** Any new component, script API or engine feature must be
   exercised by the feature test project (`StrataTests/FeatureTest/`, see [Testing](#testing)).

## Repository layout

| Path | Contents |
| --- | --- |
| `Strata/` | The engine: a static library per layer. `src/Strata/<Module>/` holds the engine modules, `src/Platform/<OS or backend>/` the platform implementations, `shaders/` the GLSL sources, `vendor/` the pinned third-party submodules. |
| `StrataEditor/` | The editor: `src/Editor/` its core without UI (`StrataEditorCore`), `src/UI/`, `src/Panels/` and `EditorLayer` its ImGui interface (`StrataEditorUI`) ([details](Docs/DeveloperGuide.md#repository-layout)). |
| `StrataRuntime/` | Runtime executable that plays exported games (`GameRuntime`): the `.stgame` manifest next to it, or `--game <file>` ([options](Docs/DeveloperGuide.md#repository-layout)). |
| `StrataScriptCore/` | Script ABI (C header) and the header-only C++ SDK game scripts are written against. Script modules never link the engine. |
| `StrataCLI/` | Command-line client for the editor automation API; also an MCP server (`StrataCLI mcp`). |
| `StrataTests/` | doctest unit tests, test helpers, the feature test project, and the layer table (`Architecture/Layers.json`, `LayeringAllowlist.txt`). |
| `Samples/` | Games an AI agent made through the editor (`Tetris`), offered as copies by the launcher; open one in place only to change it ([details](Docs/DeveloperGuide.md#repository-layout)). |
| `CMake/` | CMake modules (configurations, compiler options, shader compilation, manifest, the build's git commit). |
| `Tools/` | Standard-library Python tools: generators whose outputs are committed, `PerfStability.py`, `PerfLock.py`, `CompatLint.py` and `CIStatus.py` ([details](Docs/DeveloperGuide.md#repository-layout)). |
| `Docs/` | `README.md` (index), `DeveloperGuide.md` (this file's details), `Architecture.md`, `Roadmap.md` (update its Status when a wave lands), `Manual/`, `Releases/` ([details](Docs/DeveloperGuide.md#repository-layout)). |
| `.claude/skills/` | Skills for agents: build and test, components, scripting, editor automation, making a game (`strata-make-a-game`). |

## Building

Dependencies are git submodules. After cloning:

```sh
git submodule update --init --recursive --depth 1
```

| Platform | Configure | Build |
| --- | --- | --- |
| Windows (VS 2026) | `cmake --preset windows` | `cmake --build build/windows --config Debug --parallel` |
| Windows (VS 2022) | `cmake --preset windows-vs2022` | `cmake --build build/windows-vs2022 --config Debug --parallel` |
| Linux | `cmake --preset linux` | `cmake --build build/linux --config Debug --parallel` |
| macOS | `cmake --preset macos` | `cmake --build build/macos --config Debug --parallel` |

Configurations: `Debug`, `Release` (optimized, asserts on), `Dist` (shipping; asserts compiled out).
Binaries land in `build/<preset>/bin/<Config>/`.

Sources are collected with `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`. **After adding or removing source
files, re-run the configure step** (`cmake --preset <preset>`): the Visual Studio generator re-globs during
the build but compiles that build with the stale project, so new files would only appear in the next build.

Requirements (CMake 3.25+, a C++20 compiler, a Vulkan 1.2+ driver, packages on Linux) and CMake options:
[Building](Docs/DeveloperGuide.md#building). CI always builds with warnings as errors.

**Parallel agents:** each concurrently working agent must use its own build directory, e.g.
`cmake --preset windows -B build/agent-<name>`, because two builds in one directory corrupt each other.

**CI before main:** CI (`.github/workflows/ci.yml`: Windows with MSVC, Linux with GCC 14, macOS with Apple Clang 15,
each building and testing Release, and a Dist build of every target on Windows) runs on pushes to `main`, on pull
requests and on pushes to branches named `ci/<name>`. Older compilers and other standard libraries reject code that the
local ones accept (Apple Clang 15's libc++ has no floating-point `std::from_chars`; `size_t` and `uint64_t` are
different types on macOS; GCC 14 warns where newer GCCs do not), so push a branch that lands many changes (a wave's
integration) as `ci/<name>` and wait for green CI before it moves `main`.

**Windows subsystem:** `StrataEditor` and `StrataRuntime` (and exported games) are GUI-subsystem programs on Windows;
output, headless and scripted runs are *Console* runs (`GetProgramRun`), and fatal startup failures go through
`Application::ReportFatalError`. PowerShell waits for them only when their output is piped (`| Out-Host`) or with
`Start-Process -Wait` ([Windows subsystem](Docs/DeveloperGuide.md#windows-subsystem)).

**Packages and releases:** a file releases must ship gets an install rule (`CMake/StrataPackaging.cmake`) and an entry
in the layout `StrataEditor.Install` checks. A release, cut at the end of every wave, first runs
`ctest --preset <platform>-dist -LE "gpu|perf"` and a dry run with the tag `v<version>-rc1`
([Packages](Docs/DeveloperGuide.md#packages), [Releases](Docs/DeveloperGuide.md#releases)).

## Testing

```sh
ctest --preset windows-debug -j 8       # all tests (Windows, Debug), side by side
ctest --preset linux-debug -LE gpu      # skip tests that need a GPU
ctest --preset windows-release -L perf  # perf tests against their budgets (Release and Dist only)
build/windows/bin/Debug/StrataTests.exe --test-suite=Core*   # run a subset directly (doctest filters)
```

Every runner, suite and helper: [Testing](Docs/DeveloperGuide.md#testing).

- Unit tests live in `StrataTests/src/<Module>/*Tests.cpp` (doctest), in suites named after the module
  (`TEST_SUITE("Scene.Serialization")`), which run in the CTest shards of `StrataTests/CoreShards.json`
  (`StrataTests.Core.<Shard>`; a new suite prefix gets a shard, `Testing.Shards`); byte and random corruption sweeps go in
  `*.Sweep` suites (a sample in the shard, all of it in `StrataTests.Fuzz`, Release and Dist); the editor's UI is tested headless with `ImGuiHarness`. Deferred commands run
  with `Tests::RunCommandToCompletion`, UI tests click in a tab only once it shows, and a test's own component is
  registered in a child process (`--strata-test-helper=custom-component`).
- `GPU*` suites (label `gpu`) share one device through `Tests::GPUContext` (never create devices in tests) and end with
  `CHECK(gpu.GetNewErrorCount() == 0)`: validation errors fail them. Exact-pixel tests set
  `SceneRendererSpecification::TextureStreaming = false` and `AddNeutralPostProcess`, tests of whole-asset residency
  `SetPartialLoads(false)`; text tests render until no glyph is pending.
- **Perf tests** (`Perf.*`, `PerfGPU.*`, label `perf`, Release and Dist; CI skips them, so run `-L perf` on the
  reference machine before merging a change that can affect performance): limits live only in
  `StrataTests/Perf/Budgets.json`, measured on the reference machine (a metric it has not measured yet is `Provisional`:
  recorded, never checked); **budgets only go down** (at most 1.5 times the measurement, lower is better; commits that
  add or lower one state the measured values; a metric over budget is a regression to fix); failures are confirmed by
  measuring again; check medians, maxima only for maxima by nature, saying why; peak memory is measured in a helper
  process; run `Tools/PerfStability.py --runs 5` there before merging changes to budgets or perf tests ([Perf
  tests](Docs/DeveloperGuide.md#perf-tests)).
- `EndToEnd*` suites start the built executables (no GPU) with private session directories, free ports and timeouts,
  and terminate what they started when they fail.
- Tests follow the transform contract, use `Tests::CreateTemporaryDirectory()` for files (never the source tree), never
  open samples in place (`Tests::CopySampleProject`) or change their process's environment for a child, and depend on
  no program outside the build: stand-ins are the test binary's helper modes (`--strata-test-helper=<mode>`). Check
  that a new regression test fails without its fix. Tests hold on CI's slowest machines (3 hardware threads): they wait
  by the clock or a condition, never by a count of frames, and size their own job pools.
- Expectations that differ in Dist (asserts compiled out, host calls not counted) say so with `#if defined(ST_DIST)`:
  CI tests Release, and only the release steps run the tests in Dist.
- The **feature test** (golden rule 6, `StrataTests/FeatureTest/`) fails when a registered component is missing from
  `Feature.stscene` or a property has its default value on every entity, a script field type is not overridden there,
  a script callback or host function is never called, a script class never runs, a public SDK class, function or macro
  goes unused (calls count only on receivers of a type its reader knows), a feature script fails a check or does not
  complete, the run logs a warning or error not in `c_ExpectedLogMessages`, or an asset type has no asset, imports with
  warnings or has its `.meta` rewritten ([Feature test](Docs/DeveloperGuide.md#feature-test): how to extend it).
- Package tests (label `package`) need CMake and a compiler; a change to `Samples/Tetris` must keep
  `Editor/TetrisCommands.json.in` passing.

## Code style (Hazel conventions)

Naming:

| Kind | Style | Example |
| --- | --- | --- |
| Classes, structs, enums, functions, methods, namespaces, files | PascalCase | `SceneRenderer`, `LoadAsset()`, `Strata::Utils`, `SceneRenderer.cpp` |
| Enum values | PascalCase (prefer `enum class`) | `ProjectionType::Perspective` |
| Local variables, parameters | camelCase | `frameIndex`, `const Ref<Scene>& scene` |
| Private/protected member variables | `m_` + PascalCase | `m_ViewportSize` |
| Static variables (class or file scope) | `s_` + PascalCase | `s_Instance` |
| `thread_local` variables | `t_` + PascalCase | `t_CurrentFrame` |
| Constants (`constexpr` at namespace or file scope) | `c_` + PascalCase | `c_MaxKeyCode` |
| Public data members of plain structs (components, specifications, descriptors) | PascalCase, no prefix | `TransformComponent::Translation` |
| Macros | `ST_` + UPPER_SNAKE | `ST_CORE_ASSERT`, `ST_PROFILE_FUNCTION` |

Formatting (enforced by `.clang-format` and `.editorconfig`):

- Tabs for indentation, LF line endings, final newline, UTF-8.
- Allman braces for namespaces, types, functions and control blocks; namespace contents are indented.
- Constructor initializer lists start on a new, indented line with the leading colon.
- `Type* pointer`, `const Type& reference` (attached to the type).
- `#pragma once` in every header. Every engine `.cpp` starts with `#include "stpch.h"` followed by
  its own header, then other engine headers, then third-party and standard headers.
- Single-statement `if`/`for` bodies may omit braces when the statement fits on one line.

Conventions:

- `Ref<T>` / `CreateRef<T>()` (shared ownership) and `Scope<T>` / `CreateScope<T>()` (unique ownership).
- Log with `ST_CORE_*` in engine code and `ST_*` in applications (`ST_CORE_INFO("Loaded {}", path)`). Messages are
  fmt-formatted; glm types format directly. The console and the log file are written by a thread of their own
  (`Core/BackgroundLogSink`): a log call waits for them only for warnings and errors (flushed before the call returns)
  and while 65,536 messages are queued (a burst the writer cannot keep up with); what is queued is written at exit and
  by `Log::Shutdown`.
- `ST_CORE_ASSERT(condition, "message {}", args)` for programmer errors (compiled out of Dist),
  `ST_CORE_VERIFY` for checks that must stay in shipping builds. Recoverable failures (I/O, bad data,
  user input) return errors (`bool`, `std::optional`, result structs) and log; they never assert.
- Engine code does not throw. Exceptions from third-party libraries (e.g. nlohmann::json) are caught
  at the call site.
- Strings are UTF-8 `std::string`. Convert at `std::filesystem` boundaries with
  `FileSystem::FromUTF8` / `FileSystem::ToUTF8`. Never construct a `path` from a narrow string on
  Windows without these helpers.
- Never include `<Windows.h>` in headers. Platform code lives in `src/Platform/`. Avoid identifiers
  that collide with Windows macros (`near`, `far`, `CopyFile`, `DeleteFile`, `GetObject`,
  `CreateWindow`, `LoadImage`, `OPAQUE`, `TRANSPARENT`, `interface`, `small`). Never include X11
  headers in engine code: X11 defines `None` as a macro, and Strata uses `None` in scoped enums (Hazel
  style). Unscoped enums must not use `None` at all.
- Files and directories only the current user may change (tokens, code the engine loads) go through
  `Platform::EnsurePrivateDirectory`, `CreatePrivateDirectory`, `GetUserRuntimeDirectory`, `WritePrivateFile` and
  `ReadTrustedFile`, which follow one contract (`Strata/Core/Platform.h`); never create them with default permissions.
  Windows security descriptors and their checks live in `Platform/Windows/WindowsFileSecurity`.
- Headers must be self-contained (compile without the precompiled header): include what you use,
  e.g. `Strata/Core/Assert.h` for `ST_CORE_ASSERT`.
- Tag components (empty structs) carry no data: use `HasComponent`/`AddComponent`, never `GetComponent`.
- Prefer clear, verbose code over clever abstractions; comment *why*, not *what*.
- Profile hot paths with `ST_PROFILE_FUNCTION()` / `ST_PROFILE_SCOPE("Name")` (Tracy). Work that belongs to a stage of
  the main thread's frame runs in a `ST_PROFILE_STAGE("Stage.Name")` scope (the always-on `FrameProfiler`, Docs/Architecture.md,
  "Frame profiler"; `editor.perf` and `--perf-log` report it): a stage records its self time, so put it around a unit of the
  frame's work, not around calls that take nanoseconds (`Scene::UpdateWorldTransforms` times only updates that recompute
  something). Scene systems name their stage in `SceneSystemDescriptor::ProfileStage`.

## Architecture rules

The threading model, frame loop and pipelines these rules protect are described in
[Docs/Architecture.md](Docs/Architecture.md).

- **Threading:** the main thread owns the scene, input, scripts and rendering submission. Background work goes through
  `JobSystem` (`Submit` for CPU work, `SubmitIO` for blocking I/O, `JobPriority::Background` for long work no frame
  waits for: below normal priority, leaving four hardware threads to the main thread, the driver and the frame's helpers
  but keeping at least half of them, `JobSystem::GetDefaultBackgroundThreadCount`). A thread that waits for a job runs
  that job itself if it has not started, and never other jobs. Results return to the main thread through
  `Application::SubmitToMainThread` or polled queues — never callbacks that touch engine state from worker threads.
- **Renderer conventions:** NVRHI flips the Vulkan viewport to D3D conventions: framebuffer origin is
  top-left, NDC +Y is up, depth range is [0, 1] and the engine uses reversed-Z (near = 1, far = 0).
  Fullscreen-pass UVs are `uv = (ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)`. Front faces are
  counter-clockwise (glTF convention).
- **Shaders:** GLSL in `Strata/shaders`, compiled to SPIR-V at build time and embedded; every shader
  must be listed in `strata_add_shaders` in `Strata/CMakeLists.txt` (re-run configure after adding one).
  Bindings use the `ST_SRV/ST_SAMPLER/ST_CBV/ST_UAV(slot)` macros of `Include/Common.glsl`, matching
  NVRHI's register mapping. Push constants share the `b` registers with constant buffers: give them a
  slot no constant buffer of the pipeline's other layouts uses.
- **NVRHI pitfalls:** never take the address of a handle (`&m_Pipeline`: `RefCountPtr::operator&` releases the
  reference, leaving a dangling object). Consecutive dispatches through the *same* binding set that read each other's
  UAV writes need an explicit `setBufferState`/`setTextureState(..., UnorderedAccess)` plus `commitBarriers()`. The
  Vulkan backend does not keep the textures of clears (`clearTextureFloat`/`UInt`, `clearDepthStencilTexture`) and
  resolves alive for the GPU: drop render targets, and what references them, through `Renderer::ReleaseDeferred`
  (`DeferredReleaseQueue`), as `SceneRenderer` does when it is resized.
- **Scripting isolation:** game scripts only see `StrataScriptCore`. Every call into script code goes
  through `CrashGuard`; anything crossing the ABI is plain data (no STL types, no exceptions).
- **Assets:** referenced by `AssetHandle` (UUID), never by path at runtime. Loading is asynchronous; code must handle
  "not loaded yet" every frame instead of blocking. See [Asset pipeline](#asset-pipeline). New code that works for a
  scene (scene systems, loading into it, rendering it) reaches the asset manager through `Scene::GetAssetManager()`
  (typed: `GetAsset<T>`, `PeekLoadedAsset<T>`), never through `AssetManager`'s static functions (`GetActive`,
  `GetAsset<T>`, `PeekLoadedAsset<T>`, `LoadAssetSync<T>`, `GetAssetState`, `GetAssetType`, `IsHandleValid`,
  `HasActive`), which all use the process's active manager (per-scene managers replace it later).
- **Layers:** every file under `Strata/src` belongs to one layer of `StrataTests/Architecture/Layers.json`, and an
  include that crosses layers must be allowed there; `Architecture.Layering` fails on any other include, on files in no
  layer and on stale lines of `LayeringAllowlist.txt`. Never add a line or widen `MayInclude` to make a sideways or
  upward include pass: invert the dependency (an interface in the lower layer, a registry, a scene service,
  `Scene/SceneServices.h`, or an engine service, `Core/EngineServices.h`); only a proven downward dependency widens
  `MayInclude`, with the reason in the layer's `Notes`. Removing an exception lowers `MaxAllowlistEntries`. The table is
  the build: each layer is a library (`Strata_<Layer>`) linking its `MayInclude`; a new layer is a table entry, its vendor
  libraries a `strata_layer_link` row, and games link no `Tooling` layer (`StrataRuntime.NoTooling`).
- **Module registration:** modules extend the engine through registries (components, asset loaders and importers, scene
  systems...), never through lower layers calling functions of higher ones. Each module has one registration function
  (`Register<Module>Module` in `<Module>/<Module>Registration.cpp`), called only by the composition root,
  `Engine::RegisterBuiltinModules`, in dependency order, before anything reads a registry. Shipped games link the root,
  so tooling is handed in (`ModuleRegistrationOptions::AssetPipeline`), and games and tests pass their components in
  `ModuleRegistrationOptions::Extra` (the registry freezes when the root returns). Scene systems declare their order
  (`SceneSystemDescriptor::After`/`Before` for a system of their module or one it cannot work without,
  `AfterIfPresent`/`BeforeIfPresent` toward other modules) and are made with `MakeSceneSystemDescriptor<T>` (overrides
  public). Scenes
  keep the components of modules a build lacks (`UnknownComponentsComponent`), also through play mode, prefab snapshots,
  duplication and undo.
- **Engine modules:** a service a program runs for its whole life (device, audio, UI, project) is an `EngineModule`
  (`Core/EngineModule.h`) in its module's folder, listed by the programs that run it (`ApplicationSpecification::Modules`);
  `Application` never calls module code, and other code finds a module with `GetModules().Find<T>()` (null when absent)
  ([details](Docs/DeveloperGuide.md#architecture-rules)).
- **Scene caches and the transform contract:** `Scene` caches world transforms, the hierarchy, activity and the name and
  tag indexes, recomputing only what changed. So **code that writes `TransformComponent` fields directly must signal the
  change**: `Entity::MarkModified<TransformComponent>()` or `registry.patch` (`ComponentAccess`,
  `Scene::SetWorldTransform`, the script API and `TransformEdit` do), or `Scene::MarkTransformChanged` where listeners
  must not hear an `on_update` (physics writing poses back). Writes to `NameComponent::Name`, `TagComponent::Tag` and
  `MeshRendererComponent` fields (render proxies and mesh colliders follow its `on_update`) signal the same way; values
  written right after `AddComponent` need none. Hierarchy changes go through `Scene` (`SetParent`, `SetSiblingIndex`,
  `PlaceEntities`, `DestroyEntities`...), never by editing `RelationshipComponent` or `HierarchyComponent`. Debug builds
  assert after every update that the caches, and after every `RenderScene::Sync` that the render proxies, match a full
  recomputation (the caches' assertion names the entity whose write was not signaled). Read sibling positions with
  `Scene::GetSiblingIndex`, remove many entities with one `DestroyEntities` call, and stay within `Scene::c_MaxEntities`
  (1,048,575) live entities.

The threading, NVRHI, layer, registration, engine module and transform rules in full:
[Architecture rules](Docs/DeveloperGuide.md#architecture-rules).

## Asset pipeline

Details: [Asset pipeline](Docs/DeveloperGuide.md#asset-pipeline).

- Every asset file has a committed `<file>.meta` (its handle): move or rename through
  `EditorAssetManager::MoveAsset`/`MoveFolder`, delete through `MoveToTrash`, and read folders and referencers from the
  manager's indexes, never from the disk per frame.
- **Bump an importer's `GetVersion()` whenever its output changes** (cooked textures: `TextureImporter` and
  `GltfImporter`). Importers read their source through `ReadImportSource` and other files only through
  `ReadImportDependency`, list in `FindExternalFiles` the files their sources name, make messages quoting file bytes
  valid UTF-8 (`MakeValidUTF8`), check `ImportContext::IsCancelled` in long loops, describe every setting in
  `GetSettingsProperties` and derive sub-asset handles with `DeriveSubAssetHandle` (stable across re-imports and
  machines) ([Importers](Docs/DeveloperGuide.md#importers)).
- Nothing in the editor waits for a background import: imports are cancelled instead, and editor code re-imports in
  the background (the blocking forms are for tools and tests).
- A new document type, or a stored form that references assets, records its references (`AssetDependencies::Scan` or
  `ImportResult::References`; each valid handle once, never itself: `NormalizeReferences`): pruned exports, delete
  checks and prefetches follow them ([References and bounds](Docs/DeveloperGuide.md#references-and-bounds)).
- Loaders reject truncated or garbage bytes without crashing; a pack format change extends the corruption tests of
  `AssetPackTests.cpp`.
- Texture sizes come from `GetMipByteSize`, never from bytes per pixel; cooked textures are read through
  `ParseCookedTexture`.
- Hold `AssetHandle`s across frames, not `Ref`s (a held `Ref` defeats the budget), request what you use every frame,
  pin what must stay and never block on a load ([Residency](Docs/DeveloperGuide.md#residency)).
- GPU uploads go in steps of at most `c_AssetUploadStepBytes` within the frame's budget
  ([Adding an asset type](Docs/DeveloperGuide.md#adding-an-asset-type)). Resolve bindless slots every frame
  (`Texture::GetBindlessSlot`): mip streaming changes them, and a texture only requested keeps its mip tail (more
  through `RequestLevel` or `TextureStreamer::Hold`: [Mip streaming](Docs/DeveloperGuide.md#mip-streaming)).

## Scripting

Game logic is C++ script classes in a script module built against `StrataScriptCore` only; writing scripts:
`.claude/skills/strata-scripting/SKILL.md`; the gameplay API, builds, hot reload and crashes:
[Scripting](Docs/DeveloperGuide.md#scripting).

- Only plain C data crosses the ABI (UTF-8 strings as pointer and size, UUIDs, float arrays, `bool`): no STL or engine
  types, no exceptions ([ABI rules](Docs/DeveloperGuide.md#abi-rules)).
- Every call into module code goes through `ScriptModule` (`CrashGuard`), reading module memory only inside guarded
  calls; host functions wrap their body in `HostCall`, start with `ResolveContext`, report misuse with
  `ScriptSystem::ReportProblem` and return failure values instead of asserting.
- Append host functions at the **end** of `StrataScriptHostAPI` and `ST_SCRIPT_HOST_FUNCTIONS` (never insert, reorder
  or remove; callbacks at the end of `StrataScriptClassDesc`, read only when its `StructSize` covers them), let the SDK
  check for them (`ST_SCRIPT_DETAIL_HOST_WITH`), and call them from the API test module, a `Scripting.*` test and the
  feature scripts ([Adding a host function](Docs/DeveloperGuide.md#adding-a-host-function)).
- Appending keeps `ST_SCRIPT_ABI_VERSION`; an incompatible change (a signature, a meaning, the layout of the value
  structs, a removal) bumps it.
- Script code never runs while engine state it could invalidate is in use: entity destruction goes through
  `ScriptSystem::DestroyEntity`.
- Save files go through `IGameSaves`: names pass `SaveFiles::ValidateName` before any path is formed, and writes are
  atomic; code that runs a game in-process provides `IGameWindow` and `IGameSaves`
  ([Gameplay API](Docs/DeveloperGuide.md#gameplay-api)).
- SDK helpers named like engine classes (`Random`, `Timer`) are tested only inside a script module.

## Audio

`AudioEngine`, clips, sources and the "Audio" scene system (Play mode only) live in `Strata/src/Strata/Audio/`
([Audio](Docs/DeveloperGuide.md#audio)). Game code plays sound through `AudioSystem` (scripts: the SDK's `AudioSource`
and `Audio`), never `AudioEngine` directly, so that it pauses and stops with the scene; tests mix on the null device.

## Animation

Skeletons, clips, controllers and the "Animation" scene system live in `Strata/src/Strata/Animation/`
([Animation](Docs/DeveloperGuide.md#animation)).

- Poses are not entities: never create entities per joint or write joint transforms into the scene.
- Other engine modules reach animation only through scene services (`ISkinningPoses`, `IAnimatorControl`,
  `IAnimationEventSource`); only the Engine and AssetPipeline layers include Animation.
- Evaluation is parallel and deterministic: jobs write only their animator's record; what changes the scene happens on
  the main thread afterwards (a test compares thread counts bit for bit).
- Root motion moves entities only in `AnimationSystem::ApplyRootMotion`.

## Editor

`StrataEditorCore` (`EditorContext`, `EditorCommandRegistry`) is the editor without UI; `StrataEditorUI` draws it and
reaches the application only through `EditorHost`. Every command family, panel and view:
[Editor](Docs/DeveloperGuide.md#editor).

- **Every change to the scene or project goes through a command** (or `SceneEditTransaction`/`SetPropertyWithUndo` for
  continuous UI edits); panels only draw state and call commands, and never write, move or remove files.
- Commands (`<group>.<action>`) have a description written for agents, a complete JSON Schema and a kind
  (`EditorCommandKind`: mark every query and session command, and queries whose results follow the session as
  `EditorCommandResults::Session`, or replays go wrong); they read parameters with `CommandArguments`, reject invalid or
  out-of-range input without side effects (`InvalidParameters` or `Failed`), track entities in a
  `SceneEditTransaction` (`Track` before changing or deleting, `TrackCreated` after creating), record one undo step per
  successful mutation (`EditorActionGroup` for several actions) and, while playing, report a `warning` and set
  `RunningCopy` ([Command conventions](Docs/DeveloperGuide.md#command-conventions)).
- Files commands write for clients go through `CommandUtils::ResolveOutputPath` (relative to the project; network and
  device paths refused; an existing file replaced only with `overwrite: true`).
- A command that replaces the edited scene starts with `CommandUtils::CheckUnsavedChanges` (fails unless `force`); UI
  code passes `force` only once the person answered `EditorShell::RequestDiscardChanges`, for the changes asked about.
- Asset deletes go to the trash and refuse while something uses the asset; steps that change files check them first
  and fail with the reason when they no longer fit (never overwriting), and asset steps never mark the scene unsaved.
- A scene being opened is read-only: code that edits the scene outside `CommitEdit` checks
  `EditorContext::CheckSceneEditable`. Code that writes a prefab sets `PrefabSync::SetBaseline`, code that gives an
  instance its prefab's values calls `PrefabInstances::MarkSynced`, and an undo action that changes the scene
  describes it as entity states (`GetSceneChange`), or the recovery journal snapshots the whole scene.
- Projects never choose what runs: the external editor is the user's setting (or `STRATA_EXTERNAL_EDITOR`), started
  without a shell, and `script.open` opens only code, CMake and text files of the project or engine.
- Commands never block a frame: one that waits returns `EditorCommandResult::Defer(poll)`, whose poll owns its data and
  allocates nothing per frame.
- UI code takes colors only from theme tokens, sizes relative to the font (no fixed pixels above 16), panels through
  `EditorPanelRegistry` (never `ImGui::Begin`), the editor's fonts and the widget kit (`UI/Widgets.h`);
  `Editor.UI.Source` enforces the first three ([Editor UI rules](Docs/DeveloperGuide.md#editor-ui-rules)). Selection
  shortcuts are `ImGuiInputFlags_RouteFocused` shortcuts of the Hierarchy and the Viewport.
- Never walk the scene or the asset directory per frame in UI code: draw cached views (`HierarchyView`,
  `AssetBrowserView`) rebuilt when versions change, and long lists through `ImGuiListClipper`.
- Only `Editor/SceneRecovery` writes recovery files, scenes load through `SceneLoader` (never `LoadAssetSync`) and are
  let go with `Scene::ReleaseInBackground`, thumbnails that would look different bump `ThumbnailCache::c_Version`, and
  command-line options go into `c_Arguments`.

## Rendering

`SceneRenderer` draws a scene through its `RenderScene` (proxies in a spatial index); passes, batching, skinning,
lights, overlays and text: [Rendering](Docs/DeveloperGuide.md#rendering).

- Never block a frame on an asset: what is loading is skipped or drawn with a fallback, and uploads go in budgeted
  steps through `Renderer::GetUploadQueue()` (command lists with copies from released staging are executed before
  its `SubmitBatch`); code that renders into the back buffer calls `Renderer::PrepareBackBuffer()` first.
- Never resolve assets per entity: go through the proxies and the frame's `AssetResolveCache`, whose references end
  with the frame; loaded materials are never changed in place (`ReplaceLoadedAsset`).
- Every pass draws skinned instances with the `ST_SKINNED` variants (the prepass and the forward pass agree to the bit)
  and mirrored ones (`c_InstanceMirrored`) with clockwise pipelines and flipped tangents; shaders derive normals with
  `TransformNormal`.
- Shaders writing HDR targets clamp with `SanitizeHDR`; normal maps are read as X and Y with Z reconstructed.
- Skinning costs unskinned scenes nothing (`RenderScene::HasSkinProxies`).
- Font files are untrusted input: extend `Renderer/FontValidation.h`, with crafted-font tests, before calling another
  stb_truetype function.
- Read GPU data back with `TextureReadback`, polled per frame; `Renderer::ReadTexture` blocks (tests and tools only).

## Automation (editor RPC + MCP)

The editor serves JSON-RPC 2.0 on loopback (`RpcServer`); `StrataCLI` is its client and an MCP server. Driving it:
`.claude/skills/strata-editor-automation/SKILL.md`; protocol and sessions:
[Automation](Docs/DeveloperGuide.md#automation-editor-rpc--mcp).

- Commands are the API: every registered command is a method and an MCP tool, so a feature reaches agents by
  registering a command (IDs as 16-digit hex strings, camelCase result keys); no RPC code is needed.
- Loopback only, no server without a token; the token never crosses the wire (challenge-response; clients check the
  server first) and is never logged or printed; before authentication only the handshake is served, and the project's
  `.strata/EditorSession.json` is untrusted and never holds the port or token
  ([Security model](Docs/DeveloperGuide.md#security-model)).
- UI code that runs a command passes `CommandOrigin { CommandSource::UI }` for the activity journal.

## Where the details are

| Adding or changing | Read first |
| --- | --- |
| A component | skill `strata-add-component`; [Feature test](Docs/DeveloperGuide.md#feature-test) |
| An asset type | [Asset pipeline](Docs/DeveloperGuide.md#asset-pipeline): Adding an asset type, References and bounds, sub-assets |
| Streaming or residency | [Asset pipeline](Docs/DeveloperGuide.md#asset-pipeline): Residency, Mip streaming; Architecture.md, "Streaming and residency" |
| An editor command | [Editor](Docs/DeveloperGuide.md#editor): Command conventions, Undo, Edits while playing, Deferred commands, No silent data loss, Output paths |
| A panel or other UI | [Editor](Docs/DeveloperGuide.md#editor): Editor UI rules, Shortcuts and editor status |
| A shader or the renderer | "Shaders" above; [Rendering](Docs/DeveloperGuide.md#rendering): Render scene, Batching, Skinned meshes, Streaming and uploads |

## Pre-commit review checklist

Before every commit, review the complete diff (`git diff --staged`) and confirm:

1. **Correctness** — edge cases handled (empty inputs, missing files, zero sizes, invalid handles),
   no undefined behavior, no data races, resources released on every path.
2. **Tests** — new behavior is covered, all tests pass locally (`ctest --preset <platform>-debug`), the
   feature test project covers new components/script APIs.
3. **Style** — naming table above, tabs, braces, include order, no dead code or commented-out code.
4. **Portability** — no platform APIs outside `src/Platform/`, compiles warning-free with MSVC,
   GCC and Clang (watch for missing includes that MSVC tolerates).
5. **Documentation** — public APIs have a short comment where intent is not obvious; `Docs/` and
   this file are updated when behavior or workflows change.
   A rule this file shortens is stated in full in `Docs/DeveloperGuide.md` (change both), a rule kept whole here is
   only here, and details go only into the guide: this file stays under 40,000 characters (`Docs.AgentGuide`).
6. **Commit hygiene** — one logical change per commit, imperative subject line under 72 characters,
   body explaining what and why.

## Working with third-party code

All dependencies are pinned shallow submodules in `Strata/vendor/`, wrapped by
`Strata/vendor/CMakeLists.txt` (vendor code builds with warnings disabled and is consumed as SYSTEM
includes). To upgrade: check out the new tag in the submodule, rebuild, run all tests, update
`ThirdPartyNotices.md`, commit the submodule bump separately.

The editor's fonts (Inter, JetBrains Mono, Lucide) are committed release files, not submodules: an upgrade replaces
them, updates `ThirdPartyNotices.md` and, for Lucide, regenerates `UI/Icons.h`
([Third-party code](Docs/DeveloperGuide.md#working-with-third-party-code)).
