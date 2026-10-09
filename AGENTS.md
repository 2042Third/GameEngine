# Strata Engine — Agent & Developer Guide

Strata is a production-grade 3D game engine written in C++20 with CMake, targeting Windows, Linux
(Ubuntu 24+) and macOS. It renders through [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) on Vulkan,
uses GLFW for windowing, glm for math, EnTT for the entity-component system, Jolt for physics and
miniaudio for audio. Game logic is written in C++ script modules that are isolated from the engine
behind a C ABI (crash containment + hot reload).

This file is the source of truth for how to work on Strata. Read it fully before changing code.

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
| `Strata/` | Engine static library. `src/Strata/<Module>/` holds the engine modules, `src/Platform/<OS or backend>/` the platform implementations, `shaders/` the GLSL sources, `vendor/` the pinned third-party submodules. |
| `StrataEditor/` | Editor executable (ImGui docking UI, gizmos, undo/redo, automation server). |
| `StrataRuntime/` | Runtime executable that plays exported games (`GameRuntime`, drawn by `GameRenderer`): it runs the `.stgame` manifest next to it, or `--game <file>`; `--headless` runs without window and GPU at 60 frames per second (servers, CI); `--screenshot out.png` with `--frames N` saves the last frame (and fails the run when it shows the missing-camera message). |
| `StrataScriptCore/` | Script ABI (C header) and the header-only C++ SDK game scripts are written against. Script modules never link the engine. |
| `StrataCLI/` | Command-line client for the editor automation API; also an MCP server (`StrataCLI mcp`). |
| `StrataTests/` | doctest unit tests, test helpers, and the feature test project. |
| `Samples/` | Games made through the editor by an AI agent, as projects (`.stproj`, `Assets/` with `.meta` files, `Scripts/`): `Tetris` (played and exported by the CTest `StrataEditor.Tetris`). Open one with `StrataEditor --project Samples/<Game>`. |
| `CMake/` | CMake modules (configurations, compiler options, shader compilation, manifest). |
| `Docs/` | Architecture and API documentation. |
| `.claude/skills/` | Task-specific skills for agents (build/test, adding components, script API, editor automation, making a game end to end: `strata-make-a-game`). |

Engine modules (`Strata/src/Strata/`): `Core` (application, logging, jobs, platform services),
`Events`, `Input`, `Math`, `Reflection`, `Scene` (ECS, components, serialization, prefabs),
`Asset` (asset database, importers, cooking, streaming, packs), `Renderer`, `Physics`, `Audio`,
`Scripting`, `Project` (projects, game manifests), `Runtime` (running exported games), `Network` (sockets,
JSON-RPC, editor automation sessions), `ImGui`.

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

Requirements: CMake 3.25+, a C++20 compiler (MSVC 19.40+, GCC 13+, Clang 17+/Apple Clang 15+),
a Vulkan 1.2+ driver. The Vulkan SDK is **not** required to build (headers and the shader compiler
are vendored), but installing it enables the Khronos validation layers used by Debug builds.
Linux also needs: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev
libxkbcommon-dev libgtk-3-dev` (window system + native file dialogs).

Useful CMake options: `-DSTRATA_BUILD_EDITOR=OFF`, `-DSTRATA_BUILD_RUNTIME=OFF`,
`-DSTRATA_BUILD_CLI=OFF`, `-DSTRATA_BUILD_TESTS=OFF`, `-DSTRATA_ENABLE_TRACY=ON`,
`-DSTRATA_WARNINGS_AS_ERRORS=OFF` (local experiments only — CI always builds with warnings as errors).

**Parallel agents:** each concurrently working agent must use its own build directory, e.g.
`cmake --preset windows -B build/agent-<name>`, because two builds in one directory corrupt each other.

## Testing

```sh
ctest --preset windows-debug            # all tests (Windows, Debug)
ctest --preset linux-debug -LE gpu      # skip tests that need a GPU
build/windows/bin/Debug/StrataTests.exe --test-suite=Core*   # run a subset directly (doctest filters)
```

- Unit tests live in `StrataTests/src/<Module>/*Tests.cpp` and use [doctest](https://github.com/doctest/doctest).
  Name suites after the module (`TEST_SUITE("Scene.Serialization")`).
- Suites whose names start with `GPU` need a Vulkan device and are registered separately under the
  CTest label `gpu`. They share one device per process through `Tests::GPUContext` (never create
  devices in tests) and end with `CHECK(gpu.GetNewErrorCount() == 0)`, so validation errors fail the
  test. Run them under the Khronos validation layer locally by pointing `VK_ADD_LAYER_PATH` at its
  build. Rendering features are tested on pixels read back with `Renderer::ReadTexture`.
- Suites whose names start with `EndToEnd` start the built `StrataEditor` and `StrataCLI` (paths in
  `STRATA_TEST_EDITOR_PATH`/`STRATA_TEST_CLI_PATH`, else next to the test executable) and run as the CTest
  `StrataEditor.Automation`, not in `StrataTests.Core`. They need no GPU (`--no-gpu`), use private session
  directories, free ports and timeouts, and terminate the processes they started when they fail.
- Use `Strata::Tests::CreateTemporaryDirectory()` for files; never write into the source tree. The test process sets
  `STRATA_RUNTIME_DIR` to a private temporary directory (`TestMain.cpp`), so runtime files such as script module copies
  never go to the user's runtime directory; helper processes inherit it.
- `StrataTests.exe --strata-test-helper=<mode>` turns the test binary into a child process for
  process tests (see `TestMain.cpp`), so tests never depend on external programs. With `STRATA_TEST_FAKE_CMAKE=succeed`
  it also stands in for CMake in script builds (`ScriptBuildSettings::CMake`), building nothing.
- **Feature test** (golden rule 6): `StrataTests/FeatureTest/` is a real project (`FeatureTest.stproj`, `Assets/`
  with committed `.meta` files). Its scene `Scenes/Feature.stscene` contains every registered component, the project
  has assets of every type (the tiny binary ones come from `Tools/GenerateAssets.py`, whose outputs are committed), and
  its script module `Scripts/` (target `StrataTestScriptsFeatureTest`) exercises the whole script SDK. Feature scripts
  derive from `FeatureTest::FeatureScript`: `Expect(condition, "description")` counts checks and keeps the first
  failure, `Completed` marks the end of a scenario, and `Journal()` records events in the scene's "Journal" entity.
  The runners:
  - `StrataTests.FeatureTest` (label `feature`, no GPU) plays the scripted scenario (`PlayFeatureScene`: 200 frames,
    simulated input, a hot reload of the module halfway, audio on the null device) three times: headless (suite
    `FeatureTest`, `src/FeatureTest/`), through editor commands in-process, and in the exported game in `GameRuntime`
    (suite `Editor.FeatureTest`);
  - `GPU.FeatureTest` (in `StrataTests.GPU`) renders the scene for 4 frames without playing it (no scripts) and checks
    entities in the ID buffer, text and stats;
  - `StrataEditor.FeatureTest` and `StrataRuntime.FeatureTest` (label `feature`): the real executables open a copy of
    the project, load the feature scripts (`script.load`), step the physics (`expect` conditions on the ball and on
    `script.status`), export it with the module and run it headless; they must exit cleanly and print what the
    scripts log (`StrataTests/Editor/RunAndExpect.cmake`).
- **The feature test enforces coverage.** It fails when:
  - a registered component is missing from the feature scene, or a property has its default value on every entity
    with the component (new properties need a non-default value there, which also proves that they serialize);
  - a script field type is not overridden in the scene: every C++ type the SDK accepts for fields
    (`Detail::c_IsFieldType` in `StrataScript/Script.h`) needs a feature script field of that type, registered with
    `ST_SCRIPT_FIELD` and overridden in `Feature.stscene` (and every engine property type fields map to is overridden);
  - a `ScriptCallback` was never called during the run (feature scripts journal their callbacks, `LifecycleFeatures`
    the first call of each, and the runners look for an entry of every callback in the journal);
  - a host function of `StrataScriptHostAPI` was never called during the run. `GetScriptHostCallCounts()`
    (`Scripting/ScriptHostAPI.h`) counts calls per table entry (Dist builds do not count); the table is built from
    `ST_SCRIPT_HOST_FUNCTIONS` in `ScriptHostAPI.cpp`, and a `static_assert` fails the build when that list and the
    struct disagree;
  - a public SDK class, function or macro (`StrataScript/*.h` outside `Detail`) is not used by the feature scripts.
    `SDKCoverageTests.cpp` reads the headers and the scripts with `src/FeatureTest/SDKReader.h`: a function counts only
    when a script calls it on its class — static functions as `Class::Name`, member functions on a receiver whose type
    the reader knows (a variable, field or parameter declared with the type, `auto` from such an expression, a call
    returning it, `this`), inherited ones unqualified inside a script class, virtual ones by an `override`. Overloads
    count together; calls on receivers of unknown type do not count, and the failure lists them;
  - a feature script fails a check or does not complete, a script class never runs, or the run logs a warning or error
    other than the messages the scripts log on purpose (`c_ExpectedLogMessages` in `FeatureTestUtils.cpp`);
  - an asset type has no asset in the project, an asset imports with warnings, or importing rewrites a `.meta` file.
- **Extending the feature test:** a new component gets an entity (or joins one) in `Feature.stscene` with a non-default
  value for every property; edit the JSON, or open a copy of the project in the editor, edit and save, and copy the
  scene back. A new script API is called from a feature script (extend the matching `Scripts/*Features.cpp`, or add a
  class with an entity in the scene and journal its `OnCreate`) that `Expect`s its effect; a new host function is also
  listed in `ST_SCRIPT_HOST_FUNCTIONS`. A new asset type gets a small asset with its `.meta` in `Assets/`. Messages
  scripts log on purpose go into `c_ExpectedLogMessages`.
- Script modules the tests load are CMake targets in `StrataTests/CMakeLists.txt` (sources in `StrataTests/Scripts/`),
  built with the tests. The CTest `StrataScriptCore.Package` (label `package`) builds `StrataTests/PackageProject` through
  the StrataScriptCore package the way a game project does (and checks that the package's glm definitions match the
  engine's glm target), `StrataScriptCore.PackageDist` the same in the Dist configuration; they need CMake and the
  compiler at test time, like the other `package` tests: `StrataTests.Package` (doctest suites named `Package*`, e.g.
  `Package.ScriptBuild`: script.build, hot reload while playing, compiler diagnostics, export) and
  `StrataEditor.Scripts` (`Editor/ScriptsEndToEnd.cmake`: the real editor creates a project, builds and attaches a
  script, plays and exports, and the game runs headless) and `StrataEditor.Tetris` (`Editor/TetrisSample.cmake`: the real
  editor builds a copy of `Samples/Tetris`, plays it with `input.*` commands and `expect`s on its HUD from
  `Editor/TetrisCommands.json.in`, and exports it; the game runs headless). A change to the sample's scene or scripts
  must keep that command script passing. The `Package*` suites build in the system temp directory,
  where MSBuild does not track files (MSB8029) and may relink unchanged modules: do not rely on a build leaving the
  module unchanged there (use the fake CMake instead).

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
- Log with `ST_CORE_*` in engine code and `ST_*` in applications (`ST_CORE_INFO("Loaded {}", path)`).
  Messages are fmt-formatted; glm types format directly.
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
- Profile hot paths with `ST_PROFILE_FUNCTION()` / `ST_PROFILE_SCOPE("Name")`.

## Architecture rules

- **Threading:** the main thread owns the scene, input, scripts and rendering submission. Background
  work goes through `JobSystem` (`Submit` for CPU work, `SubmitIO` for blocking I/O). Results return to
  the main thread through `Application::SubmitToMainThread` or polled queues — never callbacks that
  touch engine state from worker threads.
- **Renderer conventions:** NVRHI flips the Vulkan viewport to D3D conventions: framebuffer origin is
  top-left, NDC +Y is up, depth range is [0, 1] and the engine uses reversed-Z (near = 1, far = 0).
  Fullscreen-pass UVs are `uv = (ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)`. Front faces are
  counter-clockwise (glTF convention).
- **Shaders:** GLSL in `Strata/shaders`, compiled to SPIR-V at build time and embedded; every shader
  must be listed in `strata_add_shaders` in `Strata/CMakeLists.txt` (re-run configure after adding one).
  Bindings use the `ST_SRV/ST_SAMPLER/ST_CBV/ST_UAV(slot)` macros of `Include/Common.glsl`, matching
  NVRHI's register mapping. Push constants share the `b` registers with constant buffers: give them a
  slot no constant buffer of the pipeline's other layouts uses.
- **NVRHI pitfalls:** never take the address of a handle (`&m_Pipeline`): `RefCountPtr::operator&`
  releases the reference and returns the raw pointer's address, so assigning through it leaves a
  dangling object. NVRHI places automatic barriers only when the binding sets change; consecutive
  dispatches through the *same* binding set that read each other's UAV writes need an explicit
  `setBufferState`/`setTextureState(..., UnorderedAccess)` plus `commitBarriers()`.
- **Scripting isolation:** game scripts only see `StrataScriptCore`. Every call into script code goes
  through `CrashGuard`; anything crossing the ABI is plain data (no STL types, no exceptions).
- **Assets:** referenced by `AssetHandle` (UUID), never by path at runtime. Loading is asynchronous;
  code must handle "not loaded yet" every frame instead of blocking. See [Asset pipeline](#asset-pipeline).

## Asset pipeline

- A project's assets are the files under its asset directory (`Assets/`). Each file has a sidecar
  `<file>.meta` (JSON: handle + import settings) that is committed with it; moving or renaming must go
  through `EditorAssetManager::MoveAsset` (or move the `.meta` along) so the handle survives.
- Importers (`Asset/AssetImporter.h`, built-ins in `Asset/AssetImporters.cpp`) turn source files into
  the stored form, cached per handle in `<project>/.strata/Cache` (derived data, never committed). A
  cached import is redone when the source content, the import settings, the importer's `GetVersion()`
  or another file the import read change — **bump the version whenever an importer's output format
  changes.** Importers read files other than their source only through `ReadImportDependency`, which
  confines reads to the asset directory and records the file as a dependency.
- Engine-native assets (`.stscene`, `.stprefab`, `.stmat`) are JSON documents with a
  `{ "Strata": { "Format": ..., "Version": ... } }` header, stored as-is. Their loaders validate fully;
  JSON entry points are named `FromJson`, byte entry points `Deserialize`.
- Importers may produce sub-assets (e.g. meshes of a model); their handles are derived from the parent
  handle and a stable key (`DeriveSubAssetHandle`), so they are stable across re-imports and machines.
- Built-in assets (primitive meshes, default material) have fixed handles 1–255 (`BuiltinAssets`) and
  exist in every asset manager.
- Shipped games read an asset pack (`.stpak`, `AssetPack`) through `RuntimeAssetManager`; the editor
  builds it with `EditorAssetManager::BuildAssetPack`.
- Adding an asset type: an `Asset` subclass with a cooked/serialized form, a loader in
  `Asset/AssetRegistration.cpp`, an importer if it comes from external files, and tests for round trips
  and corrupt data (every loader must reject truncated or garbage bytes without crashing).

## Scripting

Game logic is C++ script classes compiled into a **script module**: a shared library built against
`StrataScriptCore` only (it never links the engine). The engine loads it through `ScriptEngine`; playing scenes run
their Script components through the built-in "Scripting" scene system (`ScriptSystem`, not created in simulate mode).
Writing scripts is described in `.claude/skills/strata-scripting/SKILL.md`.

| Where | What |
| --- | --- |
| `StrataScriptCore/Include/StrataScript/ScriptABI.h` | The versioned C ABI: host API table, module/class/field descriptors, values. |
| `StrataScriptCore/Include/StrataScript/*.h` | Header-only C++ SDK (`StrataScript.h` includes all): `Script`, `Entity`, `Scene`, `Assets`, `Input`, `Time`, `Log`, `RigidBody` and `Physics` (bodies and queries; `Collision` for the contact callbacks of `Script`), `AudioSource` and `Audio`, `Game` (quit, scene loads), `ST_SCRIPT_CLASS`/`ST_SCRIPT_FIELD`. |
| `StrataScriptCore/Source/ScriptModuleEntry.cpp` | The module entry points, compiled into every module by `strata_add_script_module()`. |
| `StrataScriptCore/CMake/` | `strata_add_script_module()` and the package game projects use (`StrataScriptCoreConfig.cmake`). |
| `Strata/src/Strata/Scripting/` | `ScriptEngine` (module, hot reload, faults, watchdog), `ScriptModule` (loading, validation, guarded calls), `ScriptSystem` (instances and lifecycle), `ScriptHostAPI` (the host table), `ScriptValue` (value conversion). |
| `StrataTests/Scripts/`, `StrataTests/src/Scripting/` | Test modules (API, reload V1/V2, faults, invalid modules) and the `Scripting.*` suites. |
| `StrataTests/FeatureTest/Scripts/`, `StrataTests/src/FeatureTest/` | The feature test's script module (the whole SDK in a real scene) and its runners (see [Testing](#testing)). |

Gameplay API (host functions appended to ABI version 1 and wrapped by the SDK; the skill has an overview with recipes):

- Physics: `RigidBody` (velocities, forces, impulses, `Teleport`) and `Physics` (raycasts, overlaps) go through the scene's
  `PhysicsSystem`, also in `OnCreate`: scenes start every system before the scripts (`SceneSystem::OnRuntimeStarted`).
  Contacts reach scripts as `OnCollisionEnter/Exit` and `OnTriggerEnter/Exit` (class descriptor callbacks taking a
  `StrataScriptCollision`): `ScriptSystem` listens to the `PhysicsSystem` while it runs and calls the scripts of both
  entities, skipping removed, disabled and not yet started instances; contacts begin only for active entities, and each
  instance that got an Enter gets its Exit, also after its entity was deactivated.
- Audio: `AudioSource` and `Audio` go through the scene's `AudioSystem`; without it (simulate mode) the calls fail.
- Game flow: `Game::Quit`, `LoadScene` and `ReloadScene` set `Scene::RequestQuit`/`RequestSceneLoad`, which the scene's owner
  honors after the frame (see [Editor](#editor)).
- `Random`, `Timer` and `KeyRepeat` (`StrataScript/Gameplay.h`) run entirely in the module. The engine has classes named
  `Random` and `Timer` too, so SDK helpers are tested inside a script module, never in an engine translation unit (that
  would violate the one-definition rule).

ABI rules:

- Only plain C data crosses the boundary: strings as (pointer, size) UTF-8, entities and assets as 64-bit UUIDs, math
  as float arrays (quaternions x, y, z, w), booleans as `bool`. No STL types, no engine types, no exceptions: the SDK
  catches every exception in the module and reports it through `ReportException` (the instance is disabled).
- The module description (`StrataScriptModuleAPI`) is the only extensible struct a module writes into engine memory:
  the engine announces its size in `StructSize`, the module writes at most that much and reports its own size
  (`Detail::WriteModuleAPI`), so modules of a newer SDK with appended members load into older engines safely.
- Every call into module code goes through `ScriptModule` (`CrashGuard`), including loading and unloading the library.
  Module memory (descriptors, strings) is read only inside guarded calls; copy it into locals of the guarded lambda,
  then move the complete result out, so a fault can never leave engine objects half-written.
- Host functions (`ScriptHostAPI.cpp`) wrap their body in `HostCall` (no exception may unwind into the module), start
  with `ResolveContext` (rejects null/stale contexts, other threads, calls outside script callbacks and calls from a
  crashed module), report misuse with `ScriptSystem::ReportProblem` and return a failure value instead of asserting.
- Script code must never run while engine state it could invalidate is in use: entity destruction requested by scripts
  goes through `ScriptSystem::DestroyEntity`, removed instances are flagged and destroyed at the next sync point, and
  `Scene` defers destruction while systems run (`SceneSystem::OnEntityDestroying` lets systems react first).

Adding a host function (or a module callback):

1. Append it at the **end** of `StrataScriptHostAPI` (callbacks: `StrataScriptClassDesc`) with a comment. Never insert,
   reorder or remove members; that is an incompatible change.
2. Implement it in `ScriptHostAPI.cpp` as `Host<Name>` and append `X(<Name>)` to `ST_SCRIPT_HOST_FUNCTIONS` there,
   which assigns it in `CreateHostAPI()` with a call counter (a `static_assert` fails the build until the list matches
   the struct). Callbacks: `ScriptModule` reads them only when the descriptor's `StructSize` covers them, see
   `ST_SCRIPT_HAS_MEMBER`.
3. Wrap it in the SDK. Functions appended after an ABI version's initial set are optional for modules: check
   `ST_SCRIPT_HAS_MEMBER(StrataScriptHostAPI, host, Name) && host->Name` and degrade gracefully (the SDK's
   `ST_SCRIPT_DETAIL_HOST_WITH(Name)`, in `StrataScript/Host.h`, returns the table only then).
4. Exercise it in the API test module (`StrataTests/Scripts/API`), test it in `StrataTests/src/Scripting/`, and call it
   from the feature scripts (`StrataTests/FeatureTest/Scripts`): the feature test fails while a host function or a
   public SDK function is never used there.
5. Appending keeps `ST_SCRIPT_ABI_VERSION`. Any incompatible change (signature, meaning, struct layout of
   `StrataScriptValue`/`StrataScriptTransform`/`StrataScriptString`, removals) bumps it; the engine then refuses older
   modules with a clear error. On a bump, move the SDK's baseline check in `Detail::LoadModule` to the new version's
   last host function.

Building and loading scripts:

- Inside this repository: `strata_add_script_module(<Target> SOURCE_DIR <dir>)` (or `SOURCES`). Game projects build
  their `Scripts/` folder through the package:
  ```cmake
  cmake_minimum_required(VERSION 3.25)
  project(MyGameScripts CXX)
  find_package(StrataScriptCore CONFIG REQUIRED PATHS "<engine>/StrataScriptCore/CMake" NO_DEFAULT_PATH)
  strata_add_script_module(MyGameScripts SOURCE_DIR Scripts)
  ```
  Use the engine's compiler and configuration (the package defines Dist with the Release flags, like the engine;
  multi-config generators get it added to `CMAKE_CONFIGURATION_TYPES`). The module is `<Name>.dll`/`.so`/`.dylib`
  (`ScriptEngine::GetModuleFileName`).
- **Project scripts.** A project's scripts live in its script directory (`ProjectScriptSettings::SourceDirectory`,
  "Scripts"), whose `CMakeLists.txt` builds every `.cpp`/`.h` below it into the module `ModuleName` (stored in the
  `.stproj`, version 2: `"Scripts": {"SourceDirectory", "ModuleName"}`; version 1 files derive the name from the project
  name, `Project::MakeScriptModuleName`). `project.create` writes that `CMakeLists.txt` and an example script
  (`Editor/ScriptProject.cpp`; `script.init` adds them to older projects); project text only goes into its comments,
  and projects reject names and directories with control characters on load and save. The editor builds the scripts with
  `script.build` (`ScriptBuilder`, `Editor/ScriptBuild.cpp`): CMake configures `<project>/.strata/Scripts/Build` with the
  toolchain the engine was configured with (generator, platform, toolset, compiler, configuration and this checkout as
  `STRATA_ENGINE_DIR`, baked into `Editor/ScriptBuildConfig.h` at configure time; CMake is the engine's, else `cmake` on
  PATH) and builds the module into `<project>/.strata/Scripts/Bin`, in child processes polled once per frame (never
  blocking). The configure step only runs when the build tree is new or the toolchain changed. Output is streamed to
  the log; errors are parsed into file/line/message diagnostics (MSVC, GCC, Clang, the GNU, Apple and MSVC linkers,
  CMake; failures with only summary lines report the end of the log). One build runs at a time: a second request
  fails until the running one finished. Cancelling a build (also by closing the project or the editor) ends every
  process it started (`ProcessSpecification::TerminateTree`: a job object on Windows, a process group on POSIX).
- **The editor's script engine:** `EditorContext` owns a `ScriptEngine` per open project (active while it is open and
  set active again when play starts), loads the project's built module when the project opens (warning when the
  scripts exist but are not built), keeps hot reload on for modules rebuilt outside the editor, and calls `Update()`
  once per frame before the scene. A successful `script.build` loads the module, or reloads it when it changed (hot
  reload while playing; the file watcher is paused during editor builds so the module reloads once). A crash while
  playing stops play mode and logs the class, callback and entity; `play.start` then fails until the module is rebuilt
  (a successful build loads a crashed module again even when it did not change) or reloaded. `script.load {path}` runs
  another module file (tests use in-tree modules this way).
- **Exported games:** `project.export` writes the module the editor runs next to the game (the loaded module's file,
  verified against the digest taken when it was loaded, `EditorContext::ReadRunningScriptModule`; plus its PDB except
  in Dist builds or with `includeScriptSymbols: false`) and names it in the `.stgame` manifest (version 2,
  `"ScriptModule"`; version 1 manifests load without scripts). It refuses while a script build runs, when the module
  file changed since it was loaded (`script.reload` or `script.build` first), and when scenes or prefabs attach scripts
  but no module is loaded. `GameRuntime` loads the module (no hot reload) and makes its engine active before the start
  scene plays. A script crash disables the scripts for the session (`GameRuntime::GetScriptFault`): a headless
  `StrataRuntime` exits with code 2, a windowed one keeps running and logs it.
- The host: `ScriptEngine::LoadModule(path)`, `ScriptEngine::SetActive(engine)` before scenes start playing,
  `SetHotReloadEnabled(true)` (before loading) and `Update()` once per frame (outside scene updates) for hot reload. With
  hot reload the module runs from a private copy in a directory only the user can modify
  (`Platform::GetUserRuntimeDirectory`; one directory per process, removed with its last copy; the process holds a
  `FileLock` in it while it runs, so other sessions remove only directories whose owner is gone), so the build can
  overwrite the original at any time; without it (shipped games) the module loads in place. A file that is already
  loaded (a reload, another engine) is always loaded from a copy, because loading it again would share the running
  module's state. A failed (re)load keeps the running module. Poll `IsFaulted()`/`GetFault()` to stop play mode after a
  crash; reloading clears the fault.
- Shared libraries a module links against: on Windows they are found next to the module file, also when it runs from a
  copy (the original's directory is searched, never the current directory or PATH). On Linux and macOS the loader
  resolves them through the module's RUNPATH: CMake's default (absolute) build RPATH works, but `$ORIGIN` /
  `@loader_path` name the directory of the file actually loaded, which is the private copy's directory under hot reload.
- Hot reload during play snapshots every instance's fields, deletes the instances (no `OnDestroy`), loads the new
  module, recreates the instances, restores fields that still exist with the same name and type and calls `OnReload`
  (not `OnCreate`). Classes that disappeared lose their instances; new classes start normally.
- Contained: access violations, division by zero, stack overflow, C++ exceptions escaping module code and, on Windows,
  `abort()` (also from a failed `assert()` and from `std::terminate`: modules turn it into
  `ST_SCRIPT_ABORT_EXCEPTION_CODE` through a SIGABRT handler `ScriptModuleEntry.cpp` installs in their static C
  runtime before the module's own static initializers run; an abort in one of those fails the load). On Linux and
  macOS `abort()` is reported on stderr and ends the process: the C library also aborts on heap corruption while it
  holds allocator locks, and jumping out would leave them locked (the next allocation would hang).
- Limitations: native code cannot be preempted (an infinite loop blocks the main thread; `SetWatchdogTimeout` reports
  long calls); a crash inside a module's static initializers or destructors fails the load or abandons the library (the
  Windows loader contains it itself; elsewhere it is reported), but may make the process crash when it exits, and
  outside Windows may leave the platform loader in an undefined state (the engine logs that a restart is recommended
  and loads that file only from copies until then, so the loader never hands out the broken library again); after
  `std::terminate` the C++ runtime keeps the abandoned exception; stray writes into engine memory are not detected;
  memory of instances abandoned after a crash is leaked. Not contained (the process
  ends): Windows fail-fast terminations (`__fastfail`: `/GS` buffer overrun checks, C runtime invalid-parameter
  failures, heap corruption the system detects), `abort()` in Windows modules with a dynamically linked C runtime
  (`/MD`) or without the SDK's entry points (`NO_SDK_ENTRY`), and calls that end the process (`exit`,
  `TerminateProcess`).

## Audio

`Strata/src/Strata/Audio/` holds `AudioEngine` (the miniaudio mixer, output device and listener), `AudioClip` (sound data,
decoded or streamed; `AudioClipAsset` is its asset), `AudioSource` (a voice with volume, pitch, looping and 3D settings)
and `AudioSystem`, the built-in "Audio" scene system.

- **Initialization:** `Application` initializes the `AudioEngine` (`ApplicationSpecification::EnableAudio`, on by default)
  and shuts it down after the layers, so editor play mode and exported games produce sound without further setup.
  Headless runs (`--headless`, the editor's `--no-gpu`) and machines without an output device mix without a device (the
  null device); the application advances it by the frame time (`AudioEngine::AdvanceNullDevice`), so sounds still
  progress and end. Tests initialize the null device themselves and pull the mix with `AudioEngine::ReadFrames` to
  measure levels (`StrataTests/src/Audio/AudioTestUtils.h`).
- **Scenes:** `AudioSystem` runs in Play mode only (not in Simulate mode) and updates in `OnLateUpdate`, after scripts and
  physics. Every active entity with an `AudioSourceComponent` owns an `AudioSource`, released when the component, the
  entity or its activity goes away. Clips load asynchronously and start once ready (`PlayOnStart` or `Play`). Component
  values are compared with the applied ones every frame, so plain field writes apply without a signal. Spatial sources
  follow their world transform; their velocity (Doppler) is their rigid body's, or else derived from their moves. The
  listener is the first active `AudioListenerComponent` in hierarchy order, else the primary camera. Pausing the scene
  (`Scene::SetPaused`, which calls `SceneSystem::OnPausedChanged`) pauses its sound; stopping it releases every voice.
- **Gameplay API:** `AudioSystem::Play`, `Pause`, `Stop`, `IsPlaying`, `Seek` and `GetPlaybackPosition` per entity,
  `PlayOneShot`/`PlayOneShotAt` by clip handle and the engine-wide master volume. Game code goes through the system rather
  than `AudioEngine` directly, so that its sounds pause and stop with the scene. Scripts reach the same API through the SDK's
  `AudioSource` (`Entity::GetAudioSource`) and `Audio` (`StrataScript/Audio.h`).

## Editor

- `StrataEditorCore` (`StrataEditor/src/Editor/`) is the editor without UI: `EditorContext` (project, asset
  manager, edited scene, play mode, selection, undo history) and `EditorCommandRegistry`. The ImGui
  panels (`StrataEditor/src/Panels/`, `UI/`) only draw state and call commands; the tests link the core.
- **Every change to the scene or project goes through a command** (`EditorCommandRegistry::Execute`) or,
  for continuous UI edits, through `SceneEditTransaction` / `SetPropertyWithUndo`. That keeps the UI,
  automation (AI agents) and tests identical, and makes every edit undoable.
- Commands are named `<group>.<action>`, take a JSON object and return a JSON value or an error. Each
  has a one-line description and a JSON Schema of its parameters (`CommandUtils::ObjectSchema` etc.);
  automation exposes them as tools, so descriptions must tell an agent what the command does. Use
  `CommandArguments` to read parameters (no exceptions), reject invalid input without side effects
  (roll back the transaction) and record exactly one undo step per successful mutating command.
  Failures carry an `EditorCommandError` kind: a request the command cannot accept (unknown, missing or
  mistyped parameter, a reference to nothing) is `InvalidParameters` (`CommandArguments::Fail`,
  `EditorCommandResult::InvalidParameters`); a valid request that cannot be carried out in the current
  state is `Failed` (`EditorCommandResult::Fail`). Automation reports them with different error codes.
- Undo works on entity snapshots: a `SceneEditTransaction` captures the entities an edit touches
  (`Track`, `TrackSubtree` before changing or deleting them, `TrackCreated` after creating them) and
  `EditorContext::CommitEdit` records the difference. Edits while playing are not recorded. Continuous edits merge
  into one step (`EditorAction::MergeWith`); a merged step that ends where it started (`IsNoOp`) is dropped.
- `project.export` writes a playable game outside the project: the asset pack (`<Game>.stpak`), the script module, the
  manifest (`<Game>.stgame`, start scene, script module and window settings) and the runtime executable renamed after
  the game. CTest exports a small game (`StrataEditor --no-gpu`) and runs it headless.
- A running game asks its owner to quit or to switch scenes through `Scene::RequestQuit`/`RequestSceneLoad` (scripts: the
  SDK's `Game`), honored after each update: the editor stops play mode, or replaces the running scene (a paused game stays
  paused, with its pending steps; `play.stop` still returns to the edited scene); `GameRuntime` ends the game
  (`GetQuitRequest`; StrataRuntime exits with the code) or loads the scene from the pack. A null handle restarts the running
  scene.
- **Scripts through commands** (see Scripting, "Project scripts"): `script.status` (loaded module, classes with fields
  and callbacks, faults, the running and last build with diagnostics), `script.build {wait}` (deferred; fails with the
  first compiler errors), `script.reload`, `script.load {path}`, `script.init {example}`, `script.add {entity, class,
  fields}`, `script.remove {entity, class}`, `script.setField {entity, class, field, value}` (null resets to the
  default) and `script.get {entity, class?}` (the field values: the live instances' while playing, which is how tools read
  game state kept in fields; the stored overrides over the class defaults otherwise). Adding scripts and setting fields need a loaded module, which validates class and field names and types;
  removing works without one. Script edits are undoable (`ScriptEdit`, shared with the inspector's Script drawer) and
  reach the live instances while playing. The UI builds with Scripts > Build Scripts (Ctrl+B) or the toolbar.
- Commands never block a frame. One that has to wait (frames, a build, a GPU readback) returns
  `EditorCommandResult::Defer(poll)`; `EditorCommandRunner` polls it once per frame, starting with the
  next frame, and reports through a completion callback. The UI, command scripts and automation all
  run commands through the runner (UI helpers that expect an immediate result reject pending ones).
  Poll functions own their data (copy parameters, never capture them by reference). `editor.wait
  {frames}` returns after that many frames, e.g. to let a playing scene run.
- `StrataEditor --commands script.json` runs a JSON array of `{"command", "parameters", "expect"}` at startup
  (`EditorCommandScript`); a pending command holds the script until it completes. `expect` (optional) maps JSON
  pointers into the command's result to conditions, e.g. `{"/values/Translation/1": {"min": 1.6, "max": 1.7}}` or
  `{"/state": {"equals": "Play"}}`; a result that does not meet them fails the step. If a command or an expectation
  fails, or the script has not finished by the last of `--frames N` frames or by `editor.quit`, the process exit code
  becomes 1.
  `--frames N` stops after N frames (without saving the panel layout), `--screenshot out.png` captures
  the last frame (viewport included), `--no-gpu` runs headless without a graphics device (export, asset processing),
  and `--quit-after-commands` closes the editor once the command script finished (for scripts of unknown length, e.g.
  with `script.build`, whose duration no frame budget can bound).
  Without `--frames`, a headless editor runs until `editor.quit` (which refuses to discard unsaved
  scene changes unless `force` is true) or a signal; headless editors run at most 60 frames per second.
  The editor serves automation by default (`EditorAutomation`, see [Automation](#automation-editor-rpc--mcp));
  `--no-automation` turns it off and `--automation-port <port>` picks the port (default 0: a free one).
  A headless editor without `--frames` whose automation cannot start exits with code 1. CTest runs of
  command scripts pass `--no-automation`.
  CTest runs `StrataTests/Editor/SmokeCommands.json.in` (configured into the build tree; it builds a scene and captures
  the viewport into `build/<preset>/StrataTests/SmokeCaptures/`) and checks that failing and unfinished scripts fail the
  process.
- `editor.status` summarizes the editor (project, scene, play state, selection, undo history); other
  parts of the editor add sections to it through `EditorContext::SetStatusProvider`.
- Mutating commands report a `warning` in their result while the scene is playing: such changes apply
  to the running copy and are discarded by `play.stop`. Unknown or missing parameters are errors.
- **Viewport state** lives in the core: `EditorContext::GetViewport()` (`EditorViewport`) holds the editor camera
  (`EditorCamera`: a target that is also the orbit pivot, distance, yaw/pitch in degrees, FOV, clip planes, fly speed),
  the `ViewportSettings` (grid, selection outline, light/camera/collider shapes, stats, gizmo mode and space, snap
  steps) and two `ViewportRenderer`s (the panel's and the captures'), created on first use and only with a GPU. Camera
  and settings are saved per project in `<project>/.strata/EditorViewport.json` when it closes and restored when it
  opens. `ResolveViewportView` picks the camera: the scene's primary camera while playing (the editor camera with a
  notice when there is none), the editor camera when editing or simulating. Without a project the context keeps an
  asset manager with only the built-in assets active, so built-in meshes render.
- **Viewport panel** (`Panels/ViewportPanel`): renders into a texture of the panel's pixel size and takes input only
  while hovered or focused: Alt + left drag orbits, middle drag pans, the wheel dollies, right drag flies (WASD, Q/E
  down/up, Shift faster, wheel = speed), F frames the selection, Home everything, W/E/R/Q pick the gizmo, Ctrl snaps.
  Clicks pick without blocking (`EditorViewport::RequestPick` reads one pixel of the entity-ID buffer; Ctrl toggles,
  Shift adds, empty space clears) and never when they hit the gizmo. Gizmo drags go through `TransformDrag`
  (`Editor/TransformEdit.h`): selected entities without a selected ancestor follow the primary one, local transforms
  are recomputed under their parent (`TransformEdit::WorldToLocal`), writes emit the transform's update signal so
  physics follows, and a drag is one undo step (while playing: the running scene, no undo). Playing through the scene's
  camera makes the panel the game view: editor tools are off and `Input` is enabled only while it is focused (Shift+F1
  leaves it and frees a cursor the game locked). ImGuizmo only starts a drag while no ImGui item is hovered, so the
  image is a plain `Dummy` while the mouse is over the gizmo.
- **View commands** change no scene data and record no undo: `camera.get`, `camera.set {position, target, yaw, pitch,
  distance, fov, near, far, flySpeed}` (position + target looks from one at the other) and `camera.focus {entities?}`
  (frames them, or the whole scene). `viewport.capture {width?, height?, camera?: "editor" | "scene", overlays?, path?,
  overwrite?}` renders on the next frame (again on the following frames, up to `c_MaxCaptureTextFrames`, while glyphs of
  the scene's text are still being rasterized, so text is never half drawn), reads the image back without stalling,
  encodes it on a job thread and returns `{"Image": {"MimeType": "image/png", "Data": <base64>}, "width", "height",
  "camera", "overlays", "pendingAssets", "pendingTextGlyphs", "notice"?, "path"?}`; it defaults to the viewport's size,
  camera and overlays and fails without a GPU.
- Materials: `material.create {path, properties}`, `material.set {material, properties}` and `material.get {material}` (its
  values and every material property described like `component.list` describes component properties); an unknown property
  name fails with the list of known ones.
- **Simulated input** (`Editor/EditorInputCommands.cpp`) lets tools play a running game (play mode only) as a person would:
  `input.key {key, action: tap|press|release, frames, wait}`, `input.mouseButton {button, action, frames, wait}`,
  `input.mouseMove {position, wait}` (pixels from the game view's top-left corner), `input.scroll {delta, wait}`,
  `input.releaseAll {wait}` and `input.state`. Keys and buttons are named like the `Key::`/`Mouse::` constants (`InputNames`,
  ignoring case). The commands feed the engine's virtual device (`Input::SimulateKey` and friends): its events apply at the
  next input frame, are merged with device input (one source pressing a key the other holds is no transition) and reach the
  game even while the viewport has no focus. Holds belong to commands (`SimulatedInput`, owned by `EditorContext`): a tap ends
  only its own hold, overlapping holds of a button keep it down until the last ends, a release ends every hold of its button,
  and `input.releaseAll` ends all of them and drops presses still queued (`Input::ReleaseAllSimulated`). Frames are input
  frames, and those follow the game's updates: `EditorContext` suspends them (`Input::SetSuspended`) while the game is paused
  without a pending step, so input given while paused arrives in the next frame `play.step` runs or after resuming, with its
  transitions, and a tap of N frames lasts N game updates. A command answers once the game has seen its input (a tap: its
  release; `seen: true`) or, with `wait: false` (the default while paused, where waiting would block a client that has to step
  the game), at once. Holds outlive the client that made them; starting or stopping play drops them
  (`Input::ClearSimulated`).
- Files commands write for clients go through `CommandUtils::ResolveOutputPath`: relative paths are relative to the
  project directory (an error without a project), network/device paths and reserved device names are refused, and an
  existing file is replaced only with `overwrite: true`.

## Rendering

- `SceneRenderer` draws a `Scene` from a `SceneCamera` into its output texture or a given framebuffer:
  light clustering, directional shadow cascades, depth/normal/entity-ID prepass, ground-truth ambient
  occlusion, forward PBR (opaque surfaces with an EQUAL depth test and forced early depth testing), sky,
  transparent surfaces (back to front), then post-processing from the scene's `PostProcessComponent`
  (exposure, bloom, tone mapping and grading, FXAA). A target framebuffer must match the viewport size
  and have a non-sRGB UNORM color format; `Render` returns false (and logs once) instead of rescaling.
- Light units are relative but physically consistent: directional intensity acts like illuminance,
  point and spot intensity like luminous intensity with inverse-square falloff (cut off at `Range`).
  Automatic exposure maps the average luminance of the non-black pixels to middle gray; tests that
  compare exact colors disable it (see `AddNeutralPostProcess` in `SceneRendererTestUtils.h`).
- Point and spot lights are frustum culled on the CPU, ranked by relevance (projected size of their
  range) and capped at `MaxLights`; `Scene/LightClusters.comp` then lists the lights reaching each
  cluster of a 16x9x24 froxel grid (exponential depth slices) and `Forward.frag` loops only over its
  pixel's cluster. Only the directional light casts shadows.
- Shadows: cascade depth ranges are fitted to the casters toward the light (casters beyond
  `c_MaxShadowCasterDistance` are pancaked onto the near plane); acne is handled by the rasterizer
  depth bias, the light's depth and normal offsets and a receiver plane depth bias in the PCSS filter.
- HDR targets are RGBA16F: shaders writing them clamp with `SanitizeHDR` (Inf and NaN would turn
  black after tone mapping). Instances with a mirroring transform (negative determinant) use
  pipelines with clockwise front faces and flip their tangent handedness (`c_InstanceMirrored`).
- Everything streams: meshes, materials and textures that are still loading are skipped or drawn with
  fallbacks and counted in `SceneRendererStats::PendingAssets`; never block a frame on an asset.
- Overlays (`SceneRenderOptions`, all off by default) are drawn after post-processing with exact display
  colors into the output texture (then copied into an external target): the infinite ground grid, the
  selection outline (from the entity-ID buffer, so alpha-blended surfaces get none) and `DebugDraw` line
  lists, depth-tested against the scene or always on top. Fill a `DebugDraw` each frame (gameplay
  debugging, or `DrawSceneGizmos` for light, camera and collider shapes) and pass it to `Render`.
- `TextComponent`s are drawn with the overlays by `TextRenderer`: signed distance field glyphs from a
  per-font `FontAtlas` (stb_truetype, filled on demand), laid out by `LayoutText` (UTF-8, lines,
  alignment). World-space text is depth-tested, screen-space text goes over everything. Text without a
  font, or whose font is loading, uses `Font::GetDefault()` (Roboto, embedded with
  `strata_embed_file` from `CMake/StrataEmbeddedFiles.cmake`).
- Glyph atlases stay bounded: glyphs live in cells of up to `FontAtlasSpecification::MaxPages` texture
  array pages, the least recently used ones (never those of the current frame) are evicted, and only
  changed rows are uploaded. New glyphs are rasterized within `TextRenderer::c_FrameRasterBudget` per
  frame; the rest are drawn on later frames (`SceneRendererStats::PendingTextGlyphs`), so GPU tests of
  text with many distinct glyphs render until no glyphs are pending. A glyph whose rasterization would
  cost more than `FontAtlas::c_MaxGlyphRasterCost` (four frame budgets; `FontAtlas::GetRasterCost`
  counts texels times vertices, curves and composite assembly) is rasterized at half or a quarter of
  the resolution, and not drawn beyond that.
- Font files are untrusted input and stb_truetype does no bounds checking: `Font::Create` validates
  everything stb_truetype can read (`Renderer/FontValidation.h`), rejects malformed fonts and fonts with
  CFF outlines (OTTO), and disables kerning that is not fully bounded (in what stb_truetype reads,
  in the GPOS lookups it searches per glyph pair, and in the work to validate it). Inconsistent
  format 4 character map search parameters, which stb_truetype trusts, are corrected in the font's
  copy (`Font::GetData`) rather than rejected. Bound time as
  well as reads: offsets in font tables may share targets, so count work with repeats. Before
  calling another stb_truetype function, extend the validator to cover what it reads, with
  crafted-font tests.
- Reading GPU data back: `TextureReadback` copies a texture region and reports when the GPU is done (poll `IsReady`
  once per frame, never wait in a frame); `Renderer::ReadTexture` is the blocking form for tests and tools. Picking
  uses `SceneRenderer::ReadEntityIDAsync` and `GetEntityFromID`.
- `GameRenderer` (`Runtime/`) draws a running game into the window's back buffer from the scene's primary camera, or a
  message frame naming the problem when the scene has none.
- GPU tests of the scene renderer share `StrataTests/src/Renderer/SceneRendererTestUtils.h`. Verify that a
  new regression test fails without its fix before relying on it.

## Automation (editor RPC + MCP)

The editor exposes its features to tools and AI agents through `RpcServer` (`Strata/src/Strata/Network/`):
JSON-RPC 2.0, one compact JSON message per line, over TCP on loopback. `StrataCLI` is the client (`call`,
`list`, `status`, `launch`) and an MCP server on stdio (`StrataCLI mcp`). How an agent drives the editor is
described in `.claude/skills/strata-editor-automation/SKILL.md`.

- **Commands are the API:** `EditorAutomation` (`StrataEditor/src/Editor/`) owns the editor's server and offers
  every command of the `EditorCommandRegistry` as a method with the command's name, description and parameter
  schema; commands registered later (or again, with another description or schema) are picked up on the next
  frame. `rpc.listMethods` lists them, and `StrataCLI mcp` turns each into a tool (`entity.create` ->
  `entity_create`, `inputSchema` = the parameter schema). So a new feature reaches agents by registering a
  command; no RPC code is needed. Write commands for agents: a description that says what the command does
  and returns, a complete schema (property descriptions, `required`), IDs as 16-digit hex strings, assets by
  handle or path, results as JSON objects with camelCase keys, and images as
  `{"Image": {"MimeType": ..., "Data": <base64>}}` (MCP clients receive image content; `StrataCLI call
  --save-image` writes the file).
- **Requests** run on the main thread from `EditorAutomation::Update` (once per frame, after the runner's
  update) through the `EditorCommandRunner`: deferred commands answer when they complete; a client that
  disconnects meanwhile only loses the answer. Requests are logged at trace level (`Automation: #<n> <method>
  <params> -> <outcome> (<time>)`).
- **Errors** map from `EditorCommandError`: unknown command -32601 (`MethodNotFound`), `InvalidParameters`
  -32602 (`data`: `{"command", "parameters": <schema>}`), `Failed` -32005 (`OperationFailed`), `Cancelled`
  -32006, `Internal` -32603. Transport errors are -32001 to -32004 (`JsonRpc::ErrorCode`).
- **Lifecycle:** the editor starts automation after opening its project, on 127.0.0.1 with a fresh token and a
  free port (`--automation-port <port>` picks one, `--no-automation` turns it off), and publishes its session.
  The session moves along when the editor opens or creates another project. On exit the editor cancels pending
  commands (their clients get `Cancelled`), removes the session files and stops the server with a grace period
  (`RpcServer::Stop(gracePeriod)`) so the last answers, such as `editor.quit`'s, still arrive. A headless editor
  without `--frames` runs until `editor.quit` or a signal (a killed editor leaves a stale session file, which
  clients prune), or, with `--idle-timeout <seconds>`, until no client has been connected and no request
  pending for that long (unsaved changes are then discarded, with a warning). The status bar shows the port
  and the connected clients; `editor.status` has an `automation` section.
- **Security model:** any local process, and any web page in a local browser, can reach the port; only
  holders of the session token are trusted. The server binds loopback addresses only and refuses to start
  without a token (`EditorSession::GenerateSessionToken`, from the OS secure random generator). Every
  connection starts with a challenge-response handshake (`RpcAuthentication`) in which the token never
  crosses the wire: `rpc.handshake` sends a client nonce and returns a server nonce with the server's HMAC
  proof, which `RpcClient` verifies before it proves anything in return (so clients never talk to a process
  that took over a dead editor's port); `rpc.authenticate` then sends the client's HMAC proof. Anything else
  closes the connection. Clients refuse to connect without a token. Unauthenticated connections get tiny
  limits and a deadline; authenticated ones get size limits and backpressure. Never log or print tokens.
- **Session files:** `<per-user data>/Strata/Sessions/<pid>.json` (`Platform::FindUserDataDirectory`, e.g.
  `%LOCALAPPDATA%\Strata\Sessions`) holds the full session (address, port, token,
  process start time). It is written owner-only (`Platform::WritePrivateFile`) into a private directory. A
  session counts only while its process id is alive with the recorded start time (a reused id does not
  match). Files of exited editors are pruned; a live process whose start time cannot be verified is skipped,
  never deleted. `<project>/.strata/EditorSession.json` only names the editor's process; it
  is untrusted (the project may be shared) and never contains the port or token. An editor that cannot publish
  its session does not serve automation (nothing could find it).
- **Clients:** discovery prefers `--port` (+ `STRATA_EDITOR_TOKEN`), then `--project <dir>`, then the newest
  session. A connection that has connected stays with that editor process (also after it opens another
  project) or, after a restart, an editor with the same project; it never switches to another editor silently.
  `launch`/`strata_launch_editor` start an editor (optionally for a project; `--headless`, `--no-gpu`) that runs
  until `editor.quit`; an editor that does not become reachable in time is stopped. Editors the MCP server
  starts get `--idle-timeout 600` (`StrataCLI mcp --idle-timeout <s>`, 0: never) so they do not outlive the agent
  session, and a request for another editor without a project reuses the one it started. `launch` passes
  `--idle-timeout` only when given. `call` reads params as JSON text, from stdin (`-`) or a file (`@path`).
  Exit codes: 0 success, 1 the editor answered with an error, 2 no editor reachable or connection lost, 3 usage,
  4 no answer within `--timeout` (the command may still finish), 5 `--save-image` failed.
- **Environment:** `STRATA_SESSION_DIR` overrides the session directory for editors and clients alike (tests
  use it to stay isolated from real editors). `STRATA_EDITOR_PORT`/`STRATA_EDITOR_TOKEN` select an explicit
  endpoint, and `STRATA_EDITOR_PATH` the editor executable for `launch`/`strata_launch_editor`.
- **Other methods:** endpoints that are not editor commands use `RpcServer::RegisterMethod` with a description
  and a JSON Schema (`ReplaceMethod` swaps one in a single step). Handlers run on the main thread from
  `ProcessRequests()`; keep the `Ref<RpcResponder>` to answer later. Names starting with `rpc.` are reserved.
- **Tests:** `Editor.Automation` drives `EditorAutomation` in-process with `RpcClient`; the CLI and MCP suites
  never need a real editor (`Tests::PumpedRpcServer` plays the editor, `Tests::LiveProcess` gives fake sessions
  a running process id, and `STRATA_TEST_FAKE_EDITOR=1` makes the test executable act as a launched editor, see
  `StrataTests/src/Network/FakeEditorProcess.h`); the `EndToEnd` suites run the real editor and StrataCLI.

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
6. **Commit hygiene** — one logical change per commit, imperative subject line under 72 characters,
   body explaining what and why.

## Working with third-party code

All dependencies are pinned shallow submodules in `Strata/vendor/`, wrapped by
`Strata/vendor/CMakeLists.txt` (vendor code builds with warnings disabled and is consumed as SYSTEM
includes). To upgrade: check out the new tag in the submodule, rebuild, run all tests, update
`ThirdPartyNotices.md`, commit the submodule bump separately.
