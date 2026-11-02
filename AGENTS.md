# Strata Engine — Agent & Developer Guide

Strata is a production-grade 3D game engine written in C++20 with CMake, targeting Windows, Linux
(Ubuntu 24+) and macOS. It renders through [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) on Vulkan,
uses GLFW for windowing, glm for math, EnTT for the entity-component system, box3d for physics and
miniaudio for audio. Game logic is written in C++ script modules that are isolated from the engine
behind a C ABI (crash containment + hot reload).

This file is the source of truth for how to work on Strata. Read it fully before changing code. How the parts fit
together is described in [Docs/Architecture.md](Docs/Architecture.md).

Claude Code loads this file into every session, so it holds the rules every change follows and, for each area, a
digest of what must always or never be done there. The details are in [Docs/DeveloperGuide.md](Docs/DeveloperGuide.md):
read the guide's section for the area you work on before changing it (each digest links it). How the two files share
the work: checklist item 5.

## Golden rules

1. **Never read or use other folders of the machine you work on** (this repository, open-source projects fetched for
   the work and your own scratch directories are not "other"). Open-source code is used only under a permissive license,
   with its notice kept and its source named; copyleft code only for ideas
   ([Third-party code](Docs/DeveloperGuide.md#working-with-third-party-code)).
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
| `Strata/` | The engine: a static library per layer. `src/Strata/<Module>/` holds the engine modules, `src/Platform/<OS or backend>/` the platform implementations, `shaders/` the Slang sources, `vendor/` the pinned third-party submodules. |
| `StrataEditor/` | The editor: `src/Editor/` its core without UI (`StrataEditorCore`), `src/UI/`, `src/Panels/` and `EditorLayer` its ImGui interface (`StrataEditorUI`) ([details](Docs/DeveloperGuide.md#repository-layout)). |
| `StrataRuntime/` | Runtime executable that plays exported games (`GameRuntime`): the `.stgame` manifest next to it, or `--game <file>` ([options](Docs/DeveloperGuide.md#repository-layout)). |
| `StrataScriptCore/` | Script ABI (C header) and the header-only C++ SDK game scripts are written against. Script modules never link the engine. |
| `StrataCLI/` | Command-line client for the editor automation API; also an MCP server (`StrataCLI mcp`). |
| `StrataTests/` | doctest unit tests, test helpers, the feature test project, and the layer table (`Architecture/Layers.json`, `LayeringAllowlist.txt`). |
| `Samples/` | Games an AI agent made through the editor (`ThirdPerson`, `OpenWorld`, `PhysicsPlayground`, `Tetris`), offered as copies by the launcher; open one in place only to change it ([details](Docs/DeveloperGuide.md#samples)). |
| `CMake/` | CMake modules (configurations, compiler options, shader compilation, manifest, the build's git commit). |
| `Tools/` | Standard-library Python tools: generators whose outputs are committed, perf, lint and CI helpers ([details](Docs/DeveloperGuide.md#repository-layout)). |
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
files, re-run the configure step** (`cmake --preset <preset>`).

Requirements (CMake 3.25+, a C++20 compiler, a Vulkan 1.2+ driver, packages on Linux) and CMake options (precompiled
headers, compiler cache, optional modules): [Building](Docs/DeveloperGuide.md#building). CI always builds with warnings
as errors. `#if ST_WITH_<MODULE>` appears only in the composition root, the asset pipeline, the runtime and tests
([Optional modules](Docs/DeveloperGuide.md#optional-modules)).

**Parallel agents:** each concurrently working agent must use its own build directory, e.g.
`cmake --preset windows -B build/agent-<name>`, because two builds in one directory corrupt each other.

**CI before main:** CI (`.github/workflows/ci.yml`, [jobs](Docs/DeveloperGuide.md#building)) runs on pushes to `main`,
on pull requests and on pushes to branches named `ci/<name>`. Older compilers and other standard libraries reject code
that the local ones accept, so push a branch that lands many changes (a wave's integration) as `ci/<name>` and wait for
green CI before it moves `main`.

**Windows subsystem:** `StrataEditor` and `StrataRuntime` (and exported games) are GUI-subsystem programs on Windows;
output, headless and scripted runs are *Console* runs (`GetProgramRun`), and fatal startup failures go through
`Application::ReportFatalError` ([Windows subsystem](Docs/DeveloperGuide.md#windows-subsystem): how PowerShell waits).

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

- Unit tests live in `StrataTests/src/<Module>/*Tests.cpp` (doctest), in suites named after the module; a new suite
  prefix gets a shard (`StrataTests/CoreShards.json`) and corruption sweeps go in `*.Sweep` suites ([Unit
  tests](Docs/DeveloperGuide.md#unit-tests), [Shards, sweeps and locks](Docs/DeveloperGuide.md#shards-sweeps-and-locks)).
- `GPU*` suites (label `gpu`) share one device through `Tests::GPUContext` (never create devices in tests) and end with
  `CHECK(gpu.GetNewErrorCount() == 0)`; exact-pixel tests render with `Tests::ExactSpecification()` and
  `AddNeutralPostProcess` ([GPU tests](Docs/DeveloperGuide.md#gpu-tests): residency, text, drawn counts).
- **Perf tests** (`Perf.*`, `PerfGPU.*`, label `perf`, Release and Dist; CI skips them): limits live only in
  `StrataTests/Perf/Budgets.json`, measured on the reference machine (else `Provisional`); **budgets only go down** (at
  most 1.5 times the measurement; commits that add or lower one state the measured values); there, run `-L perf` before
  merging what can change performance, `Tools/PerfStability.py --runs 5` before merging changes to budgets or perf tests
  ([Perf tests](Docs/DeveloperGuide.md#perf-tests)).
- `EndToEnd*` suites start the built executables (no GPU) with private session directories, free ports and timeouts,
  and terminate what they started when they fail.
- Tests follow the transform contract, keep files in `Tests::CreateTemporaryDirectory()`, copy samples
  (`Tests::CopySampleProject`), never set their environment for a child, depend on no program outside the build and wait
  by the clock or a condition, never frames ([Test rules](Docs/DeveloperGuide.md#test-rules-and-helper-processes)).
  Check that a new regression test fails without its fix.
- Expectations that differ in Dist say so with `#if defined(ST_DIST)` ([Releases](Docs/DeveloperGuide.md#releases)).
- The **feature test** (golden rule 6, `StrataTests/FeatureTest/`) fails when something registered (a component, a
  property's non-default value, a settings section, a script field type, callback, host function or SDK name, an asset
  type) goes unexercised, a feature script fails, or the run logs a warning or error it does not expect ([Feature
  test](Docs/DeveloperGuide.md#feature-test): what counts, how to extend it).
- Package tests (label `package`) need CMake and a compiler, `StrataTools.*` tests Python 3; a change to a sample must
  keep its `Editor/<Sample>Commands.json.in` passing.

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
  fmt-formatted; glm types format directly; a background thread writes them
  ([Code style](Docs/DeveloperGuide.md#code-style)).
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
- Headers are self-contained and sources include what they use; precompiled headers hold only the standard library,
  vendor headers and the Core basics ([Precompiled headers](Docs/DeveloperGuide.md#precompiled-headers)).
- Tag components (empty structs) carry no data: use `HasComponent`/`AddComponent`, never `GetComponent`.
- Prefer clear, verbose code over clever abstractions; comment *why*, not *what*.
- Profile hot paths with `ST_PROFILE_FUNCTION()` / `ST_PROFILE_SCOPE("Name")` (Tracy); a unit of the main thread's frame
  work runs in a `ST_PROFILE_STAGE("Stage.Name")` scope ([Code style](Docs/DeveloperGuide.md#code-style)).

## Architecture rules

The threading model, frame loop and pipelines these rules protect are described in
[Docs/Architecture.md](Docs/Architecture.md).

- **Threading:** the main thread owns the scene, input, scripts and rendering submission; background work goes through
  `JobSystem`, and results return through `Application::SubmitToMainThread` or polled queues, never callbacks that touch
  engine state from worker threads.
- **Renderer conventions:** NVRHI flips the Vulkan viewport to D3D conventions: framebuffer origin is
  top-left, NDC +Y is up, depth range is [0, 1] and the engine uses reversed-Z (near = 1, far = 0).
  Fullscreen-pass UVs are `uv = (ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)`. Front faces are
  counter-clockwise (glTF convention).
- **Shaders:** HLSL-syntax Slang in `Strata/shaders`, compiled to SPIR-V by the pinned slangc and embedded; every
  entry is listed in `strata_add_shaders` in `Strata/CMakeLists.txt` (re-run configure after adding one). Bindings are
  `register(tN|sN|bN|uN, spaceS)`; push constants share the `b` registers with constant buffers: give them a slot no
  constant buffer of the pipeline's other layouts uses. HLSL only: no generics, `__target_switch` or `spirv_asm` outside
  `Include/Common.slang`; `mul` only with a matrix; vector comparisons through `all()`/`any()`; matrix constructors take
  rows; `SampleLevel` in compute. A lighting bug in unchanged code may be a conversion slip
  ([Shaders](Docs/DeveloperGuide.md#shaders)).
- **NVRHI pitfalls:** never take the address of a handle (`&m_Pipeline`); dispatches through one binding set that read
  each other's UAV writes need explicit barriers; drop render targets through `Renderer::ReleaseDeferred`.
- **Assets:** referenced by `AssetHandle` (UUID), never by path at runtime; code handles "not loaded yet" every frame
  instead of blocking, and code that works for a scene reaches assets through `Scene::GetAssetManager()`, never
  `AssetManager`'s static functions.
- **Layers:** every file under `Strata/src` belongs to a layer of `StrataTests/Architecture/Layers.json` and includes
  across layers only what it allows (`Architecture.Layering`). Never add a `LayeringAllowlist.txt` line or widen
  `MayInclude` to make a sideways or upward include pass: invert the dependency (an interface below, a registry, a scene
  or engine service); games link no `Tooling` layer.
- **Module registration:** modules extend the engine through registries (components, asset loaders and importers, scene
  systems, settings sections), never through lower layers calling higher ones. A component lives in its module
  (`<Module>/<Module>Components.h`); each module has one `Register<Module>Module`, called only by the composition root,
  `Engine::RegisterBuiltinModules`; games and tests pass theirs in `ModuleRegistrationOptions::Extra`; scene systems
  declare their order and are made with `MakeSceneSystemDescriptor<T>`.
- **Engine modules:** a service a program runs for its whole life is an `EngineModule` (`Core/EngineModule.h`) in its
  module's folder, listed by the programs that run it (`ApplicationSpecification::Modules`); `Application` never calls
  module code, and other code finds a module with `GetModules().Find<T>()` (null when absent)
  ([details](Docs/DeveloperGuide.md#architecture-rules)).
- **Scene caches and the transform contract:** `Scene` caches world transforms, the hierarchy, activity and the name and
  tag indexes, so **code that writes `TransformComponent` fields directly must signal the change**
  (`Entity::MarkModified<TransformComponent>()`, `registry.patch` or `Scene::MarkTransformChanged`), and so must writes
  to names, tags, `MeshRendererComponent` and light fields. Hierarchy changes go through `Scene` (`SetParent`,
  `DestroyEntities`...), never by editing `RelationshipComponent` or `HierarchyComponent`.
- **Collision layers** are the project's (`Scene.CollisionLayers`); `Scene::GetPhysicsLayers` is a working copy applied
  on the main thread ([details](Docs/DeveloperGuide.md#collision-layers)).
- **Physics tuning** is the project's `Physics.Simulation` section, applied by `PhysicsSystem` on the main thread;
  character presence bodies and kinematic bodies never recycle contacts, and what physics reports every frame stays
  constant time (`IPhysicsDiagnostics`) ([Physics](Docs/DeveloperGuide.md#physics)).

These rules in full: [Architecture rules](Docs/DeveloperGuide.md#architecture-rules).

## Asset pipeline

Details: [Asset pipeline](Docs/DeveloperGuide.md#asset-pipeline).

- Sample assets are CC0 or CC-BY, credited in the sample's `CREDITS.md` and `ThirdPartyNotices.md` (`Samples.Content`).
- Every asset file has a committed `<file>.meta` (its handle): move or rename through
  `EditorAssetManager::MoveAsset`/`MoveFolder`, delete through `MoveToTrash`, and read folders and referencers from the
  manager's indexes, never from the disk per frame.
- **Bump an importer's `GetVersion()` whenever its output changes** (cooked textures: `TextureImporter` and
  `GltfImporter`); importers read files only through `ReadImportSource` and `ReadImportDependency`
  ([Importers](Docs/DeveloperGuide.md#importers): what else they must do).
- Nothing in the editor waits for a background import: imports are cancelled instead, and editor code re-imports in
  the background (the blocking forms are for tools and tests). Registering and applying touch no file on the main
  thread: jobs prepare (`BeginScan`, outcomes carry cache sizes), `Update` inserts in time slices.
- A new document type, or a stored form that references assets, records its references (`AssetDependencies::Scan` or
  `ImportResult::References`; each valid handle once, never itself: `NormalizeReferences`): pruned exports, delete
  checks and prefetches follow them ([References and bounds](Docs/DeveloperGuide.md#references-and-bounds)).
- Scene, prefab and model documents stream: write them with `SceneDocumentWriter`, read them with `SceneDocumentReader`
  and an `EntitySink`, never through a DOM of a whole document (DOMs are for snapshots and small documents).
- Loaders reject truncated or garbage bytes without crashing, and those of large documents ask
  `AssetLoadData::IsCancelled` as they go; a pack format change extends the corruption tests of `AssetPackTests.cpp`, a
  cooked scene format change those of `CookedSceneTests.cpp`.
- Texture sizes come from `GetMipByteSize`, never from bytes per pixel; cooked textures are read through
  `ParseCookedTexture`.
- Hold `AssetHandle`s across frames, not `Ref`s, request what you use every frame,
  pin what must stay and never block on a load ([Residency](Docs/DeveloperGuide.md#residency)).
- GPU uploads go in steps of at most `c_AssetUploadStepBytes` within the frame's budget, and bindless slots are resolved
  every frame (`Texture::GetBindlessSlot`: mip streaming changes them; [Mip
  streaming](Docs/DeveloperGuide.md#mip-streaming)).

## Scripting

Game logic is C++ script classes in a module built against `StrataScriptCore` only (writing them: skill
`strata-scripting`; the gameplay API, builds, hot reload, crashes: [Scripting](Docs/DeveloperGuide.md#scripting)).

- Only plain C data crosses the ABI: no STL or engine types, no exceptions
  ([ABI rules](Docs/DeveloperGuide.md#abi-rules)).
- Every call into module code goes through `ScriptModule` (`CrashGuard`), reading module memory only inside guarded
  calls; host functions wrap their body in `HostCall`, start with `ResolveContext`, report misuse with
  `ScriptSystem::ReportProblem` and return failure values instead of asserting.
- Append host functions at the **end** of `StrataScriptHostAPI` and `ST_SCRIPT_HOST_FUNCTIONS` (never insert, reorder
  or remove; callbacks at the end of `StrataScriptClassDesc`, read only when its `StructSize` covers them), let the SDK
  check for them (`ST_SCRIPT_DETAIL_HOST_WITH`), and call them from the API test module, a `Scripting.*` test and the
  feature scripts ([Adding a host function](Docs/DeveloperGuide.md#adding-a-host-function)).
- Appending keeps `ST_SCRIPT_ABI_VERSION`; an incompatible change bumps it.
- Script code never runs while engine state it could invalidate is in use: entity destruction goes through
  `ScriptSystem::DestroyEntity`.
- Save files go through `IGameSaves` and the players' bindings only through `InputActions`; gameplay and UI read
  rebindable controls as input actions; in-process game loops provide the game's services
  ([Gameplay API](Docs/DeveloperGuide.md#gameplay-api)).
- SDK helpers named like engine classes (`Random`, `Timer`) are tested only inside a script module.
- Players' graphics options go through `IGameGraphics` (in-process game loops provide it), only `GameGraphicsService`
  touches `GraphicsOptions.json`, and every quality row has a traits line
  ([Quality settings](Docs/DeveloperGuide.md#quality-settings)).

## Audio

`AudioEngine`, clips, sources and the "Audio" scene system (Play mode only) live in `Strata/src/Strata/Audio/`
([Audio](Docs/DeveloperGuide.md#audio)). Game code plays sound through `AudioSystem` (scripts: the SDK's `AudioSource`
and `Audio`), never `AudioEngine` directly; tests mix on the null device.

## Animation

Skeletons, clips, controllers and the "Animation" scene system live in `Strata/src/Strata/Animation/`
([Animation](Docs/DeveloperGuide.md#animation)).

- Poses are not entities: never create entities per joint or write joint transforms into the scene.
- Other engine modules reach animation only through scene services (`ISkinningPoses`, `IAnimatorControl`,
  `IAnimationEventSource`); only the Engine and AssetPipeline layers include Animation.
- Evaluation is parallel and deterministic: jobs write only their animator's record; what changes the scene happens on
  the main thread afterwards.
- Root motion moves entities only in `AnimationSystem::ApplyRootMotion`.

## Worlds

A world is a persistent scene whose `WorldComponent` names a World asset; its cells stream in as sub-scenes
(`Strata/src/Strata/World/`, editor: `EditorWorld`; [World partition](Docs/DeveloperGuide.md#world-partition)).

- Never `Scene::Copy` a whole world: the edited scene is the persistent scene plus the loaded cells, which play copies.
- Cell membership changes only through `Scene::SetSubSceneOf` and `EditorWorld`; only `EditorWorld` writes cell
  documents, through the `DocumentSaver`. Worlds stay within 20 km of the origin (`WorldData::c_SupportedExtent`).
- Scenes that will run load with `SceneLoadOptions::PrepareSystems`; loading work draws from the frame's `SceneWorkBudget`.
- Physics never leaves a body without collision below its growth limit: box3d grows while bodies are created; only at
  the limit do bodies wait, never polled.

## Game UI

The game's UI (canvases, rects, widgets) lives in `Strata/src/Strata/GameUI/`; only the composition root includes the
module ([Game UI](Docs/DeveloperGuide.md#game-ui)).

- Code that writes UI component fields directly signals the change (`MarkModified`, `registry.patch`; Debug builds
  check) ([The UI contract](Docs/DeveloperGuide.md#the-ui-contract)).
- The renderer reaches the UI only through `SceneOverlayRegistry`, whose pass requests each UI texture and font once per
  frame, never per element; UI components require `UIRect`.
- UI input belongs to the "UI" scene system: widgets write with `registry.patch` and report through `IUIEvents`, and
  world-space elements are hit through the camera's ray (`GameUIPointer`) ([UI input](Docs/DeveloperGuide.md#ui-input)).

## Editor

`StrataEditorCore` (`EditorContext`, `EditorCommandRegistry`) is the editor without UI; `StrataEditorUI` draws it and
reaches the application only through `EditorHost`. Every command family, panel and view:
[Editor](Docs/DeveloperGuide.md#editor).

- **Every change to the scene or project goes through a command** (or `SceneEditTransaction`/`SetPropertyWithUndo` for
  continuous UI edits; project settings: `ProjectSettingsAction` steps, preferences: Session commands); panels only draw
  state and call commands, and never write, move or remove files.
- Commands (`<group>.<action>`) have a description written for agents, a complete JSON Schema and a kind
  (`EditorCommandKind`); they reject invalid input without side effects, track entities in a `SceneEditTransaction` and
  record one undo step per successful mutation ([Command conventions](Docs/DeveloperGuide.md#command-conventions)).
- Files commands write for clients go through `CommandUtils::ResolveOutputPath`
  ([Output paths](Docs/DeveloperGuide.md#output-paths)).
- A command that replaces the edited scene starts with `CommandUtils::CheckUnsavedChanges` (fails unless `force`, also
  on unwritten settings files it would release); UI code passes `force` only once the person answered
  `EditorShell::RequestDiscardChanges` or its release and quit forms, for the changes asked about.
- Saves never serialize on the main thread (`Editor/DocumentSaver`, `SettingsFileWriter`); commands that replace the
  scene, close the project or quit wrap their handler in `CommandUtils::AfterSaves`
  ([No silent data loss](Docs/DeveloperGuide.md#no-silent-data-loss)).
- Asset deletes go to the trash and refuse while something uses the asset; steps that change files check them first
  and fail with the reason when they no longer fit (never overwriting), and asset steps never mark the scene unsaved.
- A scene being opened is read-only (`EditorContext::CheckSceneEditable` guards edits outside `CommitEdit`); code that
  writes a prefab, syncs its instances or records scene undo steps keeps them exact (`PrefabSync::SetBaseline`,
  `PrefabInstances::MarkSynced`, `GetSceneChange`, `UpdatePrefabOverrides`: [Prefab
  instances](Docs/DeveloperGuide.md#prefab-instances)).
- Projects never choose what runs: the external editor is the user's setting (or `STRATA_EXTERNAL_EDITOR`), started
  without a shell, and `script.open` opens only code, CMake and text files of the project or engine.
- Commands never block a frame: one that waits returns `EditorCommandResult::Defer(poll)`, whose poll owns its data and
  allocates nothing per frame, and is registered with `MayDefer`. A command that answers from the whole asset registry sets
  `WaitsForAssets` (it waits for a background scan, as Edit commands do).
- Tools count game time in fixed steps (`Scene::GetFixedStepCount`), never frames
  ([Stepping a paused game](Docs/DeveloperGuide.md#stepping-a-paused-game)).
- A component property another property can turn off gets an `InactiveReason` (`ComponentRegistry::SetInactiveReason`,
  [Inspectors](Docs/DeveloperGuide.md#inspectors)).
- UI code takes colors only from theme tokens, sizes relative to the font, panels through `EditorPanelRegistry`, the
  editor's fonts and the widget kit ([Editor UI rules](Docs/DeveloperGuide.md#editor-ui-rules); selection shortcuts:
  [Shortcuts](Docs/DeveloperGuide.md#shortcuts-and-editor-status)).
- Never walk the scene or the asset directory per frame in UI code: draw cached views (`HierarchyView`,
  `AssetBrowserView`) rebuilt when versions change, and long lists through `ImGuiListClipper`.
- Only `Editor/SceneRecovery` writes recovery files, scenes load through `SceneLoader` (never `LoadAssetSync`) and are
  let go with `Scene::ReleaseInBackground`, thumbnails that would look different bump `ThumbnailCache::c_Version`, and
  command-line options go into `c_Arguments`.

## Rendering

`SceneRenderer` draws a scene through its `RenderScene`; every pass and feature:
[Rendering](Docs/DeveloperGuide.md#rendering).

- Never block a frame on an asset: what is loading is skipped or drawn with a fallback, and uploads go in budgeted
  steps through `Renderer::GetUploadQueue()` (command lists copying from released staging execute before its
  `SubmitBatch`); code that renders into the back buffer calls `Renderer::PrepareBackBuffer()` first.
- Never resolve assets per entity: go through the proxies and the frame's `AssetResolveCache`, whose references end
  with the frame; loaded materials are never changed in place (`ReplaceLoadedAsset`).
- Every pass draws skinned instances with the `ST_SKINNED` variants (prepass and forward pass agree to the bit;
  unskinned scenes pay nothing: `HasSkinProxies`) and mirrored ones (`c_InstanceMirrored`) with clockwise pipelines and
  flipped tangents; shaders derive normals with `TransformNormal`. Draw list entries carry a fade code: mask the
  instance out; opaque passes test `IsLODFadeDrawn`.
- Costly knobs are rows of `RendererQualitySettings` and fields of the `Renderer.Quality` section (a row per line, its four
  presets on it); Medium keeps 0.6.0's costs ([Quality settings](Docs/DeveloperGuide.md#quality-settings)).
- Shaders writing HDR targets clamp with `SanitizeHDR`; normal maps are read as X and Y with Z reconstructed.
- Post effects run in `Renderer/SceneRendererPost.cpp` between the TAA resolve and the tone mapping, add no
  `FrameConstants` fields and are off in `AddNeutralPostProcess`
  ([Post-processing](Docs/DeveloperGuide.md#post-processing)).
- Light keeps its energy and needs no history: specular is compensated for multiple scattering (`GetSpecularEnergy`)
  and screen-space lighting denoises within the frame; check lighting changes with the debug views and the white-furnace
  test ([Lighting](Docs/DeveloperGuide.md#lighting)).
- Temporal effects jitter only the frame's camera copy and keep history per renderer; single images settle or turn TAA
  off.
- Font files are untrusted input: extend `Renderer/FontValidation.h`, with crafted-font tests, before calling another
  stb_truetype function. Text is measured with `ShapeText`, never by rasterizing; passes that draw glyphs share the
  renderer's `FontAtlasCache`.
- Read GPU data back with `TextureReadback`, polled per frame; `Renderer::ReadTexture` blocks (tests and tools only).
- Wrap every new pass in a GPU stage (`GPUPassTimer`) registered with its module
  ([GPU timing](Docs/DeveloperGuide.md#gpu-timing)).
- Read mesh geometry only through the geometry pool, every frame (`Mesh::GetGeometry`: compaction moves it)
  ([Batching](Docs/DeveloperGuide.md#batching)).
- Per-instance additions go to parallel buffers (`InstanceData` stays 64 bytes), and render-scene changes are logged
  (`RecordInstanceChanges`, `RecordProxyChange`) so its GPU copy follows ([Render scene](Docs/DeveloperGuide.md#render-scene)).
- Scene shaders work in camera-relative space (`TransformPositionRelative`; rotation-only view matrices): upload anything
  positioned relative to the camera, subtracting in double ([Camera-relative space](Docs/DeveloperGuide.md#camera-relative-space)).
- The sun's shadow reaches opaque surfaces through the screen-space mask (SRV 28) and transparent ones through
  `SampleDirectionalShadow`: change both; the sun's filters use fixed noise, never `TemporalParams.z`, and cascade layouts
  come only from `ShadowFitTracker` on the CPU ([Sun shadows](Docs/DeveloperGuide.md#sun-shadows)).
- A new input of how casters are drawn changes a version the shadow caches compare (`PrepareShadowCache`,
  `FindLocalShadowCasters`). What is drawn and requested is decided by the CPU path and `Cull.comp`: change both, keep
  shading independent of draw order (derivatives only of the primitive's own interpolants and textures) and
  `GPU.SceneRenderer.GPUDriven` and `.Feedback` passing ([GPU-driven path](Docs/DeveloperGuide.md#gpu-driven-path)).

## Automation (editor RPC + MCP)

The editor serves JSON-RPC 2.0 on loopback (`RpcServer`); `StrataCLI` is its client and an MCP server. Driving it:
`.claude/skills/strata-editor-automation/SKILL.md`; protocol and sessions:
[Automation](Docs/DeveloperGuide.md#automation-editor-rpc--mcp).

- Commands are the API: every registered command is a method and an MCP tool of its group's toolset (`core` stays
  within 32 tools and 30 KiB: [MCP toolsets](Docs/DeveloperGuide.md#mcp-toolsets)), so a feature reaches agents by
  registering a command; no RPC code is needed.
- Loopback only, no server without a token; the token never crosses the wire and is never logged or printed; before
  authentication only the handshake is served, and the project's `.strata/EditorSession.json` is untrusted and never
  holds the port or token ([Security model](Docs/DeveloperGuide.md#security-model)).
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
`ThirdPartyNotices.md`, commit the submodule bump separately. Slang is no submodule:
`CMake/StrataSlang.cmake` pins its release archives by SHA-256, fetched once into a per-user cache (offline:
`STRATA_SLANG_ROOT`); bump it in its own commit.

The editor's fonts (Inter, JetBrains Mono, Lucide) are committed release files, not submodules: an upgrade replaces
them, updates `ThirdPartyNotices.md` and, for Lucide, regenerates `UI/Icons.h`
([Third-party code](Docs/DeveloperGuide.md#working-with-third-party-code)).
