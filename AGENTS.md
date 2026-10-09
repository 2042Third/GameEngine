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
| `StrataRuntime/` | Runtime executable that plays exported games (`GameRuntime`): it runs the `.stgame` manifest next to it, or `--game <file>`; `--headless` runs without window and GPU (servers, CI). |
| `StrataScriptCore/` | Script ABI (C header) and the header-only C++ SDK game scripts are written against. Script modules never link the engine. |
| `StrataCLI/` | Command-line client for the editor automation API; also an MCP server (`StrataCLI mcp`). |
| `StrataTests/` | doctest unit tests, test helpers, and the feature test project. |
| `CMake/` | CMake modules (configurations, compiler options, shader compilation, manifest). |
| `Docs/` | Architecture and API documentation. |
| `.claude/skills/` | Task-specific skills for agents (build/test, adding components, script API, game creation). |

Engine modules (`Strata/src/Strata/`): `Core` (application, logging, jobs, platform services),
`Events`, `Input`, `Math`, `Reflection`, `Scene` (ECS, components, serialization, prefabs),
`Asset` (asset database, importers, cooking, streaming, packs), `Renderer`, `Physics`, `Audio`,
`Scripting`, `Project` (projects, game manifests), `Runtime` (running exported games), `ImGui`.

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
  CTest label `gpu`.
- Use `Strata::Tests::CreateTemporaryDirectory()` for files; never write into the source tree.
- `StrataTests.exe --strata-test-helper=<mode>` turns the test binary into a child process for
  process tests (see `TestMain.cpp`), so tests never depend on external programs.
- The feature test project exercises every component and the entire scripting API in a real scene,
  run headless by CTest. Extend it whenever you add a component or script API.
- Script modules the tests load are CMake targets in `StrataTests/CMakeLists.txt` (sources in `StrataTests/Scripts/`),
  built with the tests. The CTest `StrataScriptCore.Package` (label `package`) builds `StrataTests/PackageProject` through
  the StrataScriptCore package the way a game project does (and checks that the package's glm definitions match the
  engine's glm target); it needs CMake and the compiler at test time.

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
| `StrataScriptCore/Include/StrataScript/*.h` | Header-only C++ SDK (`StrataScript.h` includes all): `Script`, `Entity`, `Scene`, `Assets`, `Input`, `Time`, `Log`, `ST_SCRIPT_CLASS`/`ST_SCRIPT_FIELD`. |
| `StrataScriptCore/Source/ScriptModuleEntry.cpp` | The module entry points, compiled into every module by `strata_add_script_module()`. |
| `StrataScriptCore/CMake/` | `strata_add_script_module()` and the package game projects use (`StrataScriptCoreConfig.cmake`). |
| `Strata/src/Strata/Scripting/` | `ScriptEngine` (module, hot reload, faults, watchdog), `ScriptModule` (loading, validation, guarded calls), `ScriptSystem` (instances and lifecycle), `ScriptHostAPI` (the host table), `ScriptValue` (value conversion). |
| `StrataTests/Scripts/`, `StrataTests/src/Scripting/` | Test modules (API, reload V1/V2, faults, invalid modules) and the `Scripting.*` suites. |

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
2. Implement it in `ScriptHostAPI.cpp` and assign it in `CreateHostAPI()` (callbacks: `ScriptModule` reads them only
   when the descriptor's `StructSize` covers them, see `ST_SCRIPT_HAS_MEMBER`).
3. Wrap it in the SDK. Functions appended after an ABI version's initial set are optional for modules: check
   `ST_SCRIPT_HAS_MEMBER(StrataScriptHostAPI, host, Name) && host->Name` and degrade gracefully.
4. Exercise it in the API test module (`StrataTests/Scripts/API`) and test it in `StrataTests/src/Scripting/` (and the
   feature test project).
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
  Use the engine's compiler and configuration. The module is `<Name>.dll`/`.so`/`.dylib` (`ScriptEngine::GetModuleFileName`).
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
- Contained: access violations, division by zero, stack overflow, `abort()` (also from a failed `assert()` and from
  `std::terminate`; POSIX catches SIGABRT, Windows modules turn it into `ST_SCRIPT_ABORT_EXCEPTION_CODE` through a
  SIGABRT handler `ScriptModuleEntry.cpp` installs in their static C runtime) and C++ exceptions escaping module code.
- Limitations: native code cannot be preempted (an infinite loop blocks the main thread; `SetWatchdogTimeout` reports
  long calls); a crash inside a module's static initializers or destructors fails the load or abandons the library (the
  Windows loader contains it itself; elsewhere it is reported), but may make the process crash when it exits, and outside
  Windows may leave the platform loader in an undefined state;
  after `std::terminate` the C++ runtime keeps the abandoned exception; stray writes into
  engine memory are not detected; memory of instances abandoned after a crash is leaked. Not contained (the process
  ends): Windows fail-fast terminations (`__fastfail`: `/GS` buffer overrun checks, C runtime invalid-parameter
  failures, heap corruption the system detects), `abort()` in Windows modules with a dynamically linked C runtime
  (`/MD`) or without the SDK's entry points (`NO_SDK_ENTRY`), and calls that end the process (`exit`,
  `TerminateProcess`).

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
- Undo works on entity snapshots: a `SceneEditTransaction` captures the entities an edit touches
  (`Track`, `TrackSubtree` before changing or deleting them, `TrackCreated` after creating them) and
  `EditorContext::CommitEdit` records the difference. Edits while playing are not recorded.
- `project.export` writes a playable game outside the project: the asset pack (`<Game>.stpak`), the
  manifest (`<Game>.stgame`, start scene and window settings) and the runtime executable renamed after
  the game. CTest exports a small game (`StrataEditor --no-gpu`) and runs it headless.
- `StrataEditor --commands script.json` runs a JSON array of `{"command", "parameters"}` at startup; if
  one fails, the process exit code becomes 1. `--frames N` stops after N frames (without saving the
  panel layout), `--screenshot out.png` captures the last frame, `--no-gpu` runs headless without a
  graphics device (export, asset processing). CTest runs `StrataTests/Editor/SmokeCommands.json` and
  checks that a failing script fails the process.
- Mutating commands report a `warning` in their result while the scene is playing: such changes apply
  to the running copy and are discarded by `play.stop`. Unknown or missing parameters are errors.

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
