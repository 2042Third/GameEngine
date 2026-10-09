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
| `StrataRuntime/` | Runtime executable used to play and ship games (no editing). |
| `StrataScriptCore/` | Script ABI (C header) and the header-only C++ SDK game scripts are written against. Script modules never link the engine. |
| `StrataCLI/` | Command-line client for the editor automation API; also an MCP server (`StrataCLI mcp`). |
| `StrataTests/` | doctest unit tests, test helpers, and the feature test project. |
| `CMake/` | CMake modules (configurations, compiler options, shader compilation, manifest). |
| `Docs/` | Architecture and API documentation. |
| `.claude/skills/` | Task-specific skills for agents (build/test, adding components, script API, game creation). |

Engine modules (`Strata/src/Strata/`): `Core` (application, logging, jobs, platform services),
`Events`, `Input`, `Math`, `Reflection`, `Scene` (ECS, components, serialization, prefabs),
`Asset` (asset database, importers, cooking, streaming, packs), `Renderer`, `Physics`, `Audio`,
`Scripting`, `Project`, `ImGui`.

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
- Use `Strata::Tests::CreateTemporaryDirectory()` for files; never write into the source tree.
- `StrataTests.exe --strata-test-helper=<mode>` turns the test binary into a child process for
  process tests (see `TestMain.cpp`), so tests never depend on external programs.
- The feature test project exercises every component and the entire scripting API in a real scene,
  run headless by CTest. Extend it whenever you add a component or script API.

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
  in the GPOS lookups it searches per glyph pair, and in the work to validate it). Bound time as
  well as reads: offsets in font tables may share targets, so count work with repeats. Before
  calling another stb_truetype function, extend the validator to cover what it reads, with
  crafted-font tests.
- GPU tests of the scene renderer share `StrataTests/src/Renderer/SceneRendererTestUtils.h`. Verify that a
  new regression test fails without its fix before relying on it.

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
