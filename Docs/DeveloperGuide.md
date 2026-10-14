# Strata Developer Guide

This guide holds the details of how to work on Strata: how each area works, how it is tested and extended, and the
conventions its code follows. [AGENTS.md](../AGENTS.md) has the rules every change follows (Claude Code loads it into
every session) and, for each area, a digest of its hard rules that links the sections here. Read the section of the area
you work on before changing it. A rule AGENTS.md shortens (in a digest, or one of the building steps, architecture rules
and third-party rules it states in short) is stated in full here, and a change to it changes both files; the rules
AGENTS.md keeps whole (the golden rules, the code style, the review checklist and the rest of its building steps,
architecture rules and third-party rules) are only there. Details go only here, so that AGENTS.md stays under 40,000
characters (the test `Docs.AgentGuide` fails beyond). How the parts fit together is described in
[Architecture.md](Architecture.md).

## Contents

- [Repository layout](#repository-layout)
- [Building](#building)
  - [macOS and MoltenVK](#macos-and-moltenvk)
  - [Windows subsystem](#windows-subsystem)
  - [Packages](#packages)
  - [Releases](#releases)
- [Testing](#testing)
  - [Unit tests](#unit-tests)
  - [GPU tests](#gpu-tests)
  - [Perf tests](#perf-tests)
  - [End-to-end tests](#end-to-end-tests)
  - [Test rules and helper processes](#test-rules-and-helper-processes)
  - [Feature test](#feature-test)
  - [Script modules and package tests](#script-modules-and-package-tests)
- [Architecture rules](#architecture-rules)
- [Asset pipeline](#asset-pipeline)
  - [Asset files](#asset-files)
  - [Importers](#importers)
  - [Engine-native assets, sub-assets and built-in assets](#engine-native-assets-sub-assets-and-built-in-assets)
  - [Asset packs](#asset-packs)
  - [References and bounds](#references-and-bounds)
  - [Textures](#textures)
  - [Residency](#residency)
  - [Adding an asset type](#adding-an-asset-type)
  - [Mip streaming](#mip-streaming)
- [Scripting](#scripting)
  - [Gameplay API](#gameplay-api)
  - [ABI rules](#abi-rules)
  - [Adding a host function](#adding-a-host-function)
  - [Building and loading scripts](#building-and-loading-scripts)
  - [The editor's script engine](#the-editors-script-engine)
  - [Scripts of exported games](#scripts-of-exported-games)
  - [Hosting modules and hot reload](#hosting-modules-and-hot-reload)
  - [Crash locations and containment](#crash-locations-and-containment)
- [Audio](#audio)
- [Animation](#animation)
- [Editor](#editor)
  - [Editor UI rules](#editor-ui-rules)
  - [Changes go through commands](#changes-go-through-commands)
  - [Asset files are never lost](#asset-files-are-never-lost)
  - [Prefab instances](#prefab-instances)
  - [Command conventions](#command-conventions)
  - [No silent data loss](#no-silent-data-loss)
  - [Autosave and recovery](#autosave-and-recovery)
  - [Undo](#undo)
  - [Exporting games](#exporting-games)
  - [Play mode and scene switches](#play-mode-and-scene-switches)
  - [Opening scenes](#opening-scenes)
  - [Scripts through commands](#scripts-through-commands)
  - [Deferred commands and command scripts](#deferred-commands-and-command-scripts)
  - [Hierarchy, Content Browser and thumbnails](#hierarchy-content-browser-and-thumbnails)
  - [Inspectors](#inspectors)
  - [Shortcuts and editor status](#shortcuts-and-editor-status)
  - [Asset streaming commands](#asset-streaming-commands)
  - [Edits while playing and value ranges](#edits-while-playing-and-value-ranges)
  - [Viewport state](#viewport-state)
  - [Templates and recent projects](#templates-and-recent-projects)
  - [Launcher](#launcher)
  - [Placing content](#placing-content)
  - [Viewport panel](#viewport-panel)
  - [View commands](#view-commands)
  - [Materials](#materials)
  - [Animation commands](#animation-commands)
  - [Simulated input](#simulated-input)
  - [Output paths](#output-paths)
- [Rendering](#rendering)
  - [Scene renderer and lighting](#scene-renderer-and-lighting)
  - [Render scene](#render-scene)
  - [Batching](#batching)
  - [Skinned meshes](#skinned-meshes)
  - [Lights, shadows and HDR](#lights-shadows-and-hdr)
  - [Streaming and uploads](#streaming-and-uploads)
  - [Material texture slots](#material-texture-slots)
  - [Overlays](#overlays)
  - [Text and fonts](#text-and-fonts)
  - [Readback, the game renderer and tests](#readback-the-game-renderer-and-tests)
- [Automation (editor RPC + MCP)](#automation-editor-rpc--mcp)
  - [Commands and requests](#commands-and-requests)
  - [Activity journal](#activity-journal)
  - [Errors](#errors)
  - [Lifecycle](#lifecycle)
  - [Security model](#security-model)
  - [Session files](#session-files)
  - [Clients and environment](#clients-and-environment)
  - [Other methods and tests](#other-methods-and-tests)
- [Working with third-party code](#working-with-third-party-code)

## Repository layout

The rows of the repository layout that AGENTS.md shortens, in full (`Docs/` also holds this guide), and the engine
modules:

| Path | Contents |
| --- | --- |
| `StrataEditor/` | The editor: `src/Editor/` the UI-independent core (`StrataEditorCore`), `src/UI/`, `src/Panels/` and `EditorLayer` the ImGui interface (`StrataEditorUI`: Bedrock theme, widget kit, panel registry), the executable (`EditorApplication.cpp`); `Resources/Fonts/` the embedded fonts, `Resources/Brand/` the strata mark (icons, generated by `Tools/GenerateBrandAssets.py`), `Tools/` the icon header generator. |
| `StrataRuntime/` | Runtime executable that plays exported games (`GameRuntime`, drawn by `GameRenderer`): it runs the `.stgame` manifest next to it, or `--game <file>`; `--headless` runs without window and GPU at 60 frames per second (servers, CI); `--screenshot out.png` with `--frames N` saves the last frame (and fails the run when it shows the missing-camera message, or a black frame because the start scene still loads: scenes load in the background, the manifest's optional `LoadingScene` plays meanwhile); `--asset-budget-mb <n>` sets the GPU texture budget of the game's assets; `--resolution <width>x<height>` renders in a window of exactly that many pixels whatever the display's scale (instead of the manifest's window, which scales with the display; implies `--windowed`); `--perf-log <csv>` writes a row per frame (frame time, frame profiler stages, job threads, asset streaming and mip streaming, the upload queue, the device's memory and its staging, the process's private memory and its peak), also headless; `--save-directory <dir>` keeps the game's save files there instead of the player's data directory (tests). Patch packs the manifest lists (`PatchPacks`) are mounted over the game's pack. |
| `Samples/` | Games made through the editor by an AI agent, as projects (`.stproj`, `Assets/` with `.meta` files, `Scripts/`): `Tetris` (played and exported by the CTest `StrataEditor.Tetris`). `Samples.json` lists them; the build copies them next to the executables (`StrataSamples`), where the launcher's Open Sample and `project.openSample` offer them as copies. Open one in place only to change the sample: `StrataEditor --project Samples/<Game>`. |
| `Tools/` | Standard-library Python tools: generators whose outputs are committed (`GenerateBrandAssets.py` draws the strata mark into `StrataEditor/Resources/Brand/`: PNGs, raw RGBA for the window icon, the `.ico` of the executables), `PerfStability.py` (runs the perf tests repeatedly and reports how stable every metric is, see [Testing](#testing)), `PerfLock.py` (runs a command under a lock in the user's temporary directory, so that the measurements of agents run by one user never overlap) `CompatLint.py` (compiles changed files with GCC 14 and LLVM 16, the compilers closest to CI's, before a push: AGENTS.md, "CI before main") and `CIStatus.py` (the state of a commit's CI runs and the failure annotations its jobs left, readable without a job log). |
| `Docs/` | Documentation of how the engine works, linked to the code: `README.md` (index), `Architecture.md` (targets and dependencies, modules, frame loop, threading, asset pipeline, streaming and residency, render scene, animation, scripting, editor, export and runtime), `Roadmap.md` (the pillars, the waves of workstreams and what each landed wave delivered; update its Status when a wave lands), `Manual/` (guides for people making games in the editor: `Scripting.md`), `Releases/` (the notes of each release, `<version>.md`, and the README of the release archives, `PackageReadme.md`). |
| `.claude/skills/` | Task-specific skills for agents (build/test, adding components, script API, editor automation, making a game end to end: `strata-make-a-game`). |

Engine modules (`Strata/src/Strata/`): `Core` (application, logging, jobs, platform services),
`Events`, `Input`, `Math`, `Reflection`, `Scene` (ECS, components, serialization, prefabs),
`Asset` (asset database, importers, cooking, streaming, packs), `Renderer`, `Physics`, `Audio`,
`Animation` (skeletons, clips, animation controllers, animators), `Scripting`, `Project` (projects, game manifests),
`Runtime` (running exported games), `Network` (sockets, JSON-RPC, editor automation sessions), `ImGui`, `Engine` (the
composition root that registers every module).

## Building

The commands, configurations and CI rules are in AGENTS.md, [Building](../AGENTS.md#building). The requirements and
the CMake options:

Requirements: CMake 3.25+, a C++20 compiler (MSVC 19.40+, GCC 13+, Clang 17+/Apple Clang 15+),
a Vulkan 1.2+ driver. The Vulkan SDK is **not** required to build (headers and the shader compiler
are vendored), but installing it enables the Khronos validation layers used by Debug builds.
Linux also needs: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev
libxkbcommon-dev libgtk-3-dev` (window system + native file dialogs).

macOS runs Vulkan on Metal through MoltenVK: the LunarG Vulkan SDK, or Homebrew's `molten-vk` and `vulkan-loader` (with
`vulkan-validationlayers` for the validation layer). Homebrew's layer manifest names its library without a directory,
which the macOS loader does not look for in Homebrew's `lib`: Debug builds then warn that the layer could not be loaded
and run without it. To validate, point `VK_ADD_LAYER_PATH` at a directory holding a copy of
`$(brew --prefix)/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json` whose `library_path` is the library's
full path, `$(brew --prefix)/lib/libVkLayer_khronos_validation.dylib` (the build-test skill has the commands).

Useful CMake options: `-DSTRATA_BUILD_EDITOR=OFF`, `-DSTRATA_BUILD_RUNTIME=OFF`,
`-DSTRATA_BUILD_CLI=OFF`, `-DSTRATA_BUILD_TESTS=OFF`, `-DSTRATA_ENABLE_TRACY=ON`,
`-DSTRATA_WARNINGS_AS_ERRORS=OFF` (local experiments only — CI always builds with warnings as errors).

### macOS and MoltenVK

On macOS the renderer runs as everywhere else, through NVRHI and Vulkan; MoltenVK translates Vulkan to Metal. Measured
on an Apple M2 Max with MoltenVK 1.4.2 (Homebrew), where every test of the `gpu` label passes:

- **Missing features**, which renderer code must gate on the device's features, never on the OS: `drawIndirectCount`,
  `samplerFilterMinmax`, geometry shaders, mesh shaders, pipeline statistics queries, 64-bit buffer atomics and a
  dedicated transfer queue (uploads run on the graphics queue). GPU-driven rendering (wave 4) must work without the first
  two: indirect draws without a count from the GPU (culled draws get no instances), depth pyramids reduced without
  min/max samplers.
- **Tight limits**: 31 storage buffers per shader stage and 32 KiB of compute shared memory.
- **Bindless tables**: the descriptor set support query rejects any set beyond `maxPerSetDescriptors` (1,212), also
  update-after-bind sets within the update-after-bind limits (a million sampled images), and MoltenVK samples every slot
  of far larger tables. The table takes its size from those limits, at most `GraphicsDevice::c_MaxBindlessTextures`
  (32,768: `GraphicsDeviceInfo::MaxBindlessTextures`); on MoltenVK alone the validation layer's report of the rejected
  query is traced instead of counted, while other drivers' queries decide the size (`ChooseBindlessTableSize`,
  `ClassifyVulkanMessage`).
- **Warnings that do not apply**: MoltenVK reports the R32_UINT entity-ID target of every prepass pipeline as "Blending
  is enabled", although no pipeline blends it; `ClassifyVulkanMessage` traces the message.
- **Debug builds**: Apple's C++ library destroys a `std::vector<uint8_t>` byte by byte without optimization (about 3 ms
  per MB), so frees of large CPU copies take frames the time budgets then spread; tests that count frames or loads lift
  the time budgets when their counts must not follow the machine's speed.
- **Perf**: `Budgets.json` holds the Windows reference machine's measurements; on a Mac `-L perf` informs, it is not the
  gate (Docs/Roadmap.md, the open items, lists what waits for the reference machine).

### Windows subsystem

**Windows subsystem:** `StrataEditor` and `StrataRuntime` (and so every exported game) are GUI-subsystem programs on
Windows, so that no console window opens with them; `StrataCLI` and `StrataTests` are console programs. Their `main`
(`Core/EntryPoint.h`) first asks the client what kind of run its command line is (`GetProgramRun`, next to
`CreateApplication`: `ProgramRun` in `Core/Platform.h`) and calls `Platform::ConnectStandardStreams` with it: pipes and
files their callers pass are kept, so tests and tools read their output and exit codes as before. A *Console* run
(output is the point: `--version`, `--help`, usage errors, headless and scripted runs: the editor's `--headless`,
`--no-gpu`, `--frames`, `--commands`, `--quit-after-commands`, the runtime's `--headless`, `--frames` and option
mistakes) prints into the terminal that started it; an *Interactive* run (a person's window) leaves that terminal alone,
so its Ctrl+C or closing it does not end the program and the log does not run into the shell's prompt. Then
`Platform::InstallInterruptHandler` turns Ctrl+C, Ctrl+Break and the console closing (Windows) or SIGINT and SIGTERM
(POSIX) into an orderly quit through the main loop (`Platform::IsInterruptRequested`; exit code
`Application::c_InterruptedExitCode`, 130, unless the run had failed); a second request ends the process at once. Fatal
startup failures go through `Application::ReportFatalError` (no graphics device, no game to run, a game that cannot
start): logged, and for an Interactive run whose stderr nobody reads (`Platform::ShouldShowErrorDialogs`: started from
the desktop, from a terminal it left alone, or with its output sent to the null device) also shown in a native dialog
(`Platform::ShowErrorDialog`: MessageBoxW, osascript, zenity or kdialog) with the advice and the log file's path; tests
read its text from the file `STRATA_ERROR_DIALOG_FILE` names instead. PowerShell does not wait for windowed programs it
starts directly: pipe their output (`StrataEditor --commands script.json --quit-after-commands | Out-Host`) or use
`Start-Process -Wait` when the exit code matters (cmd.exe and bash wait). `EndToEnd.StandardStreams` (pipes, a terminal,
Ctrl+C, the dialogs' text) and `StrataEditor.Install` check this and the executables' PE subsystem.

### Packages

**Packages:** `cmake --install build/<preset> --config Dist --prefix <dir>` installs Strata the way a release archive
holds it (`CMake/StrataPackaging.cmake`): the executables side by side, `StrataInstall.json` (which makes the editor
build scripts with the installation's SDK and a toolchain it finds on the machine, see Scripting, "Building and loading
scripts"), `README.md` (from `Docs/Releases/PackageReadme.md`), `ThirdPartyNotices.md`, `Samples/`, the script SDK in
`StrataScriptCore/` (with glm's headers in `ThirdParty/glm`), `Docs/Manual/` and the skills for making games in
`Skills/`. `cpack --config build/<preset>/CPackConfig.cmake -C Dist` packs it into
`build/<preset>/Packages/Strata-<version>-<windows-x64|linux-x64|macos-arm64>.<zip|tar.gz>` (Linux executables
stripped of their symbol tables, as Windows packages hold no PDBs; the release workflow annotates every package with its
size and largest files). A file that releases must
ship gets an install rule there and an entry in the layout `StrataEditor.Install` checks (Testing).

### Releases

**Releases** (`.github/workflows/release.yml`) are cut at the end of every wave (0.3.0 for wave 3, 0.4.0 for wave 4;
fixes get patch versions):

1. Bump `project(Strata VERSION)` in `CMakeLists.txt`; the About dialog, the launcher, `editor.status`, `--version` and
   the runtime's log report it.
2. Write `Docs/Releases/<version>.md` for the people who download it (what Strata can do, requirements, known issues,
   reporting problems), link it from `Docs/README.md`, and bring `Docs/Releases/PackageReadme.md` up to date.
3. Run `ctest --preset <platform>-dist -LE "gpu|perf"` locally: CI tests Release, the release workflow tests the Dist
   build it ships, where asserts are compiled out and `ST_DIST` code paths differ (a test whose expectations depend on
   the configuration says so with `#if defined(ST_DIST)`). Then push a tag `v<version>-rc1` for a dry run: the workflow
   builds Dist on CI's runners, runs the tests CI runs (`-LE "gpu|perf"`, with `StrataEditor.Install`) in Dist, packs
   the archives, unpacks each and runs its executables, and uploads them with SHA256 checksums as workflow artifacts
   without publishing. Download and try them.
4. Push the tag `v<version>`: the same, then `gh release create` publishes the archives, their checksums and
   `SHA256SUMS.txt` as "Strata <version>" with the notes. The workflow fails when the tag's version is not the project's
   or the notes are missing. Run by hand (workflow_dispatch) with a tag it does the same; without one it is a dry run of
   the branch.

## Testing

```sh
ctest --preset windows-debug            # all tests (Windows, Debug)
ctest --preset linux-debug -LE gpu      # skip tests that need a GPU
ctest --preset windows-release -L perf  # perf tests against their budgets (Release and Dist only)
build/windows/bin/Debug/StrataTests.exe --test-suite=Core*   # run a subset directly (doctest filters)
```

### Unit tests

- Unit tests live in `StrataTests/src/<Module>/*Tests.cpp` and use [doctest](https://github.com/doctest/doctest).
  Name suites after the module (`TEST_SUITE("Scene.Serialization")`). `TestMain.cpp` registers the engine's modules
  with the asset pipeline (`Engine::RegisterBuiltinModules`) before the suites and the helper modes run; a test that
  needs its own component registers it in a child process (`--strata-test-helper=custom-component`,
  `Engine/ModuleRegistrationHelpers.cpp`). The editor's UI is tested headless with `ImGuiHarness` (suites `Editor.UI`,
  `Editor.UI.Hierarchy`, `Editor.UI.Inspector`, `Editor.UI.AssetInspector`, `Editor.UI.ContentBrowser`,
  `Editor.UI.Placement`, `Editor.UI.Scripts`, `Editor.UI.Activity`, `Editor.UI.Notifications`, `Editor.UI.Dialogs`,
  `Editor.UI.Names`, `Editor.UI.Text`, `Editor.Theme`, `Editor.UI.Source`; see Editor, "Editor UI rules"). Commands that
  defer (`scene.open`, `project.open` and `project.openSample` of a project with a start scene, `asset.import`, a
  `play.start` that builds) are run to completion with `Tests::RunCommandToCompletion` (`Editor/CommandTestUtils.h`),
  and a UI test that brings a tab forward (the Asset tab, the Inspector) waits until it shows before it clicks in it:
  ImGui draws a tab hidden in its first frame, where clicks are lost.

### GPU tests

- Suites whose names start with `GPU` need a Vulkan device and are registered separately under the CTest label `gpu`.
  They share one device per process through `Tests::GPUContext` (never create devices in tests) and end with
  `CHECK(gpu.GetNewErrorCount() == 0)`, so validation errors fail the test. Run them under the Khronos validation layer
  locally by pointing `VK_ADD_LAYER_PATH` at its build (on macOS with Homebrew, at the manifest copy
  [Building](#building) describes). Rendering features are tested on pixels read back with `Renderer::ReadTexture`.
  `GPU.Startup` needs a desktop rather than the device, like the label's other windowed tests: it starts the editor in a
  window with a Vulkan loader that finds no driver and reads what its error dialog says.

### Perf tests

- **Perf tests** (the perf lab, `StrataTests/src/Perf/PerfUtils.h`): suites named `Perf.*` (CPU) and `PerfGPU.*`
  run as the CTests `StrataTests.Perf` and `StrataTests.PerfGPU` (label `perf`; PerfGPU also `gpu`) in Release and
  Dist only, one at a time: `ctest --preset windows-release -L perf`. No other CTest runs them; the lab's own logic is
  tested by the suite `PerfLab` in `StrataTests.Core`. A perf test measures metrics in a measurement function
  (`Perf::Measure(name, warmup, iterations, function)` times each call and returns the median, 95th percentile, minimum
  and maximum in milliseconds; `Platform::GetProcessMemory` reports private bytes and the working set with their peaks)
  and checks them with `Perf::CheckBudgetConfirmed("<Suite>.<Metric>", measure)`, or with
  `Perf::CheckBudgetsConfirmed({keys}, measure)` for metrics one measurement produces together (`measure` returns
  `Perf::MetricValues`). **Failures are confirmed:** a metric over its budget is measured again, by running `measure`
  afresh (its own warm-up, its counters relative to its own start), up to `Perf::c_ConfirmationAttempts` (3) attempts
  in all, and fails only when every attempt is over budget; a metric within its budget once is decided, because noise
  only adds to a measurement (the machine's other work, memory the driver keeps). Every attempt is printed, and the
  check records each metric in `build/<preset>/PerfResults/<config>.json`: one entry per metric with its value (the
  passing attempt's, else the smallest), unit, budget (`"Provisional": true` instead for a provisional metric), target
  and the values of all its attempts, and nothing else run-specific, so two runs compare with a diff (CTest clears the
  file first); acceptance limits of workstreams are
  budgets too (the budget is the limit when it lies below 1.5 times the measurement). **Statistics:** check
  medians of enough samples (a measurement the machine slowed down that much is what confirmation is for), maxima only
  for metrics that are maxima by nature (peak memory, the worst frame of a sweep), and give the reason for each metric's
  statistic in a comment. Budgets live in `StrataTests/Perf/Budgets.json`, whose `_comment` describes the format; a
  metric without an entry fails its test before anything is measured, and **limits live there, never in a test's code**.
  An entry holds the value `Measured` on the reference machine (i7-13700K, RTX 5090, Windows 11, Release, a quiet
  machine), a `Budget` of at most 1.5 times that (the parser enforces it) and optionally a `Target`: a roadmap goal
  below the budget that the metric does not reach on every run, printed with every check (the budget is the gate; the
  parser rejects a target the budget already enforces). A metric the reference machine has not measured yet (while it
  is away, Docs/Roadmap.md lists them) gets a provisional entry, `{ "Unit": ..., "Provisional": true }` with an optional
  `Target`: its checks measure it once, print and record it, and never fail, until the reference machine measures it
  and the entry gets its `Measured` and `Budget`; numbers of other machines never go into an entry. **Budgets only go
  down:** a change that makes a metric cheaper
  lowers its entry in the same commit, a metric over budget is a regression to fix rather than a budget to raise, and
  commits that add or lower budgets state the measured values. Lower must be better (time per item, not items per
  second). **Stability:** `Tools/PerfStability.py --runs 5` runs `-L perf` five times and reports per metric the
  minimum, median and maximum, the pass rate and how often it needed confirmation, and per run how busy the machine
  was, the attempts of every confirmed metric and the canary `Perf.Harness.Sort200k` against its reference. **Load or
  regression:** a check's attempts follow each other within about a minute, so other work that lasts longer (a build)
  can fail one metric as well as many, and a failure in a run on a busy machine says nothing about the code: run the
  failing suite again on a quiet machine, where a regression stays over its budget in every attempt. The canary is a
  single-threaded workload: it shows a throttled or oversubscribed machine, and load on many cores usually leaves it
  unchanged. On the reference machine's hybrid processor the scheduler's placement of a process's main thread lasts and
  shows in its measurements: an efficiency core makes an editor frame 1.3 to 1.9 times as long, the other hardware
  thread of a core the graphics driver's busy thread runs on about 1.12 times. Applications and the test runner opt out
  of Windows' execution-speed throttling (`Platform::DisableExecutionSpeedThrottling`), which otherwise moves processes
  whose windows are in the background or covered to the efficiency cores. `--wait-quiet <percent>` starts each run
  only once the machine is that quiet (machines shared with other work; the perf tests alone keep the reference machine
  about 20% busy), and
  `--results <files>` reports on kept results files. Run it on the reference machine before merging changes to budgets
  or perf tests. GPU perf suites get the shared device
  without validation layers (the CTest sets `STRATA_TEST_GPU_VALIDATION=0`; `PerfGPU.Harness` fails without it)
  and submit through `GPUContext::ExecuteAndWait`, which recycles the command buffer like the engine does every frame
  (a plain `executeCommandList` + `waitForIdle` makes every later `open()` create a command pool). Complexity claims
  belong in ordinary unit tests with deterministic counters, which run everywhere. CI's hosted runners skip the label
  `perf` because budgets hold for the reference machine only: run `-L perf` there before merging a change that can
  affect performance. Scale tests build their scenes with `Perf/SceneGenerators.h` (flat roots, a nested tree with ten
  children per entity, entities running the API test module's trivial script, grids of cubes and of point lights)
  under `Perf::ScopedApplicationJobSystem` (the job system as applications start it): `Perf.Scene` (transform updates and
  idle frames up to a million entities; copying a million-entity scene for play; instantiating a 10- and a 1,000-entity
  prefab, and 1,000 instances of a 10-entity prefab in one `InstantiateMany` call), `Perf.Runtime` (a headless
  `GameRuntime` switching to a scene of 100,000 entities and away from it again: the longest frame, a maximum by nature),
  `Perf.Scripting` (100,000 and 300,000 trivial scripts, frames that spawn and
  destroy entities, scripts destroying 10,000 of 100,000 sibling roots in one frame), `Perf.Physics` (box stacks of
  `CreateBoxStacks`: the first frames of 30,000 boxes with time dilation, 70,000 boxes that all get a body, the busy
  frames of 70,000 and 10,000 boxes with Strata's part of a step below Jolt's (a `ratio` metric), and of 30,000 boxes
  whose floor contacts are tracked; creating 10,000 bodies in one frame; the memory a world at the largest capacities and
  at the most bodies reserves; a fixed step of 1,000 character controllers walking over steps), `Perf.TextureCook` (a cold
  import of the stress project, the stored sizes of its largest
  2048x2048 texture, its 4096x4096 texture and all of them, its pack's size, the BC7 encoder's time per block on one
  thread), `Perf.Animation` (`Perf/AnimationPerfTests.cpp`: a frame of 1,000 animators of 60 joints, each cross-fading
  two clips, and a frame of 10,000 `WhenVisible` animators no renderer sees, both with the job system as applications
  start it), `PerfGPU.Renderer` (`SceneRenderer::Render` alone on 100,000 and 300,000 cubes and on 10,000 cubes with
  unique materials, with the opaque draw calls those take, and on 1,000 skinned characters of about 5,000 vertices posed
  anew every frame, in `Perf/AnimationPerfTests.cpp`; an idle frame, the asset manager's update and the renderer's, of
  3,000 streamed textures in view) and, in `src/Editor/`, `Perf.Editor` (entity commands among 100,000
  roots, and what the activity journal adds to a command among 100,000
  roots; the hierarchy's rows of 100,000 roots, idle and rebuilt; the Hierarchy panel's share of an editor frame with
  100,000 roots, open against closed; what 100,000 selected entities add to a frame; the longest editor frame while a
  4096x4096 texture re-imports uncompressed and then block-compressed again; `asset.findReferences` among 10,000
  assets; the frames while the recovery journal of edits of a 100,000-entity scene is written and the I/O threads' time
  per write, and the capture of a
  recovery snapshot; the longest frame while `scene.open` opens a scene of 10,000 up-to-date prefab instances, whose
  swap checks each against its prefab), `PerfGPU.Editor`, which runs the real
  `StrataEditor` maximized on generated 100,000- and 1,000,000-entity scenes and checks the median CPU frame time its
  `editor.wait` steps report, and maxima by nature: the longest frame while `scene.open` loads the 100,000 and the
  1,000,000 entities, while it opens the 100,000 again over the million and while it opens 100,000 cubes over those, whose
  render proxies their load builds before the swap (`frameTimes.maxMs` of its result), the frame
  that starts playing the million (`editor.perf`'s frame maximum) and the editor's working set once they are open
  (`editor.status`' `memory.workingSetMB`) (when the editor fails, the test describes the scene files on disk with
  `Tests::DescribeJsonFile`, which tells a damaged file from a failed read or parse), exports the stress project (the
  median and the longest frame meanwhile, and
  the export's time) and times the editor's frames on the shared device while that texture re-imports, uploads and is
  drawn (and, with 10,000 cubes drawn too, whose draws are extracted in parallel, while it is compressed), while
  `asset.import` brings in a 4096x4096 texture from outside the project (the copy on an I/O job, the import in the
  background, the upload; the longest frame) and while the
  thumbnails of a stress project's textures, materials, model and mesh are made, and the editor frames of `PerfGPU.Renderer` (cube grids of 100,000, also in the streaming view and with all of them selected,
  and of 300,000, a narrow view of 50,000, 10,000 point lights, 10,000 unique materials); `PerfGPU.Pack` compares pack
  versions 2 and 1 of the stress project (the size with its textures uncompressed, opening, streaming all of the world's
  bytes as it ships, whole textures without mip streaming; the acceptance limits, 0.8 of version 1's size and 110% of
  its time, are the budgets of `ratio` metrics, so they are checked like any budget). Time calls that take nanoseconds
  in batches (the clock's resolution is about 0.1 microseconds).
  A process's peak memory includes everything it did before, so a test that measures peak memory
  runs the measured work in a helper process (`--strata-test-helper=<mode>`, see `TestMain.cpp`): `PerfGPU.Streaming`
  runs its camera sweeps over the stress project (`src/Perf/StressProject.h`, also usable by other suites;
  `StrataTests --strata-test-helper=write-stress-project <new directory>` writes it to open in the editor, and
  `--strata-test-helper=write-stress-game <new directory>` imports it and writes its pack and the manifests the test runs,
  `Stress.stgame` and `StressTopDown.stgame`, to run the exported game by hand) that way:
  one that evicts within a small texture budget and one at 1920x1080 with every texture resident, both with whole
  textures (`StreamingSweepSettings::TextureStreaming` false), and mip streaming's, one sweep per attempt
  each. Every evicting sweep must stream correctly (no failed load or device error, every camera stop
  settles, the textures stay within the residency budget give or take the largest); its budgets are maxima of the
  sweep: peak private memory (the graphics driver keeps commit charge of freed device memory for a while, which counts
  in private bytes on Windows, so identical sweeps peak up to about 150 MB apart), resident texture memory, the longest
  frame finalization (a regression may show in one frame, which a percentile would hide) and the most frames a stop
  takes to settle. Every all-resident sweep must bring every texture in, evict none and fail no load; its budgets are its
  peak private memory (650 MB, texture cooking's limit) and its longest frame finalization. Mip streaming's sweep flies 3
  units above the textured ground of the stress project's `Flight` scene (`WriteStressFlightScene`: its terrain tiled
  with a texture, whose finest level the ground under the camera needs) over six districts at 1920x1080 for 600 frames,
  with mip streaming and a 96 MB texture budget, with mip streaming within 8 MB (a budget its needs exceed), and with
  whole textures within 96 MB (the baseline), one of each per attempt: all must stream correctly, streaming must raise and
  drop levels; within 96 MB it must reach level 0 of the ground's texture, hold less than whole textures and match the
  baseline's last frame by 40 dB PSNR or more over the frame and over its bottom third (checks, not budgets); within 8 MB
  it must drop levels for the budget (`LevelsDroppedForBudget`) and stay within it. Its budgets (`PerfGPU.Streaming.Mips.*`,
  maxima by nature, of both streaming runs) are the peak private memory (550 MB, mip streaming's limit), the most bytes
  and upload-step milliseconds of a frame (24 MB, 2 ms) and the longest frame (CPU time of the manager's update and the
  rendering, the first frame left out) to the baseline's (a `ratio`: budgets are not negative, and the runs' longest
  frames are about as long). It then runs the exported
  stress game in `StrataRuntime` (`STRATA_TEST_RUNTIME_PATH`, else next to the test executable) with `--perf-log` and
  `--resolution 1920x1080` (1920x1080 pixels whatever the display's scale; every run checks its swapchain), one run per
  attempt: every log has a row per frame, and the budget judges the slowest frame's `Assets.Finalize` stage; then the game
  with its own budgets from the world's overview camera (which shows about two thirds of the textures; the world's load
  brings in all of them, at their mip tails, which fit its budgets), whose last `PeakPrivateMB` is
  `RuntimePeakPrivateMemory`; then the whole world in view, `RuntimePeakPrivateMemoryAllInView` (budget 550 MB, mip
  streaming's limit): the game started in the stress project's `TopDown` scene
  (`WriteStressTopDownScene`: the world with its camera looking down on all of it), 300 frames with its own budgets,
  every texture at the level its view needs at the end (`MipsBelowWanted` 0), none evicted or loading.
  `Perf.FrameProfiler` measures the frame profiler's cost per frame and `Perf.EditorPanels` (in `src/Editor/`) the
  Streaming panel with 10,000 assets (with the job threads, which build its table), the Content Browser with 10,001 assets
  in a folder (and, in `PerfGPU.Editor`, with the thumbnails of the tiles in view shown) and the Performance panel with a
  full ring of frames, each drawn in the UI harness: the 95th percentile frame, and for the Streaming panel the frames that
  refresh its table, so a periodic hitch cannot hide in an average. Script dispatch prefetches instances ahead because
  the hardware prefetchers follow heap blocks only when they recognize the pattern by the reading instruction's address:
  a perf difference between builds of identical sources points at such a layout effect; compare the builds' profiles
  before blaming noise.

### End-to-end tests

- Suites whose names start with `EndToEnd` start the built `StrataEditor`, `StrataCLI` and `StrataRuntime` (paths in
  `STRATA_TEST_EDITOR_PATH`/`STRATA_TEST_CLI_PATH`/`STRATA_TEST_RUNTIME_PATH`, else next to the test executable) and run
  as the CTest `StrataEditor.Automation`, not in `StrataTests.Core`. They need no GPU (`--no-gpu`), use private session
  directories, free ports and timeouts, and terminate the processes they started when they fail.

### Test rules and helper processes

- Tests hold on any machine CI runs them on, the slowest included (its macOS runners have 3 hardware threads): waits go
  by the clock (`Tests::WaitUntil`) or by a condition, never by a count of frames or updates; tests size their own job
  pools (`Tests::ScopedJobSystem`) instead of relying on the application's defaults, which follow the hardware; a thread
  that races another paces itself from its previous step, not by a fixed schedule it would catch up with in bursts; and
  file times are not assumed to survive a copy (a copied file gets the current time on POSIX).
- Tests follow the transform contract (see [Architecture rules](../AGENTS.md#architecture-rules)): a direct write to a
  `TransformComponent` after the scene was updated is followed by `MarkModified<TransformComponent>()` (or
  `Scene::MarkTransformChanged` when the test is about listeners not being signaled), otherwise Debug runs assert.
  `StrataTests/src/Scene/SceneTestUtils.h` has `CheckSceneCaches` (both validators) for scene tests.
- Use `Strata::Tests::CreateTemporaryDirectory()` for files; never write into the source tree, and never open the
  samples in place (`Tests::CopySampleProject("Tetris")` copies one). The test process sets `STRATA_RUNTIME_DIR` to a
  private temporary directory, `STRATA_RECENT_PROJECTS` to a file in it and `STRATA_SCRIPT_BUILDS_DIR` (the per-user build
  directory of projects too deep for MSBuild) to a folder in it (`TestMain.cpp`), so runtime files such as script module
  copies never go to the user's runtime directory, editors the tests start never touch the user's recent projects and
  script builds never land in the user's data; helper processes inherit them. Exported games the CTests run keep their
  save files in the build tree (`StrataRuntime --save-directory`).
- `StrataTests.exe --strata-test-helper=<mode>` turns the test binary into a child process for
  process tests (see `TestMain.cpp`), so tests never depend on external programs: e.g. `record-arguments <file>` stands in
  for the external code editor (it writes its arguments to the file), `broad-phase-churn` adds and removes bodies until
  Jolt's broad phase would run out of nodes, `run-with-environment NAME=VALUE... -- <program>` runs a program with an
  environment of its own (a test never changes its process's environment for one child, e.g. a Vulkan loader without
  drivers for `GPU.Startup`) and, on Windows, `console-output [--interrupt-when <text>] <program>` runs a program as a
  terminal does, pressing Ctrl+C once the console shows the text (`Tests::RunInConsole`). With
  `STRATA_TEST_FAKE_CMAKE=succeed` it also stands in for CMake in script
  builds (`ScriptBuildSettings::CMake`), building nothing and printing the step it ran (`-- Fake CMake: -S` or
  `--build`); with `fail` it fails the build with an MSVC error and a warning in its output (diagnostics, the Problems
  panel), with `fail-configure` the configure step with a CMake error.

### Feature test

The feature test keeps [golden rule 6](../AGENTS.md#golden-rules): every component, script API and engine feature is
exercised by it.

- **Feature test** (golden rule 6): `StrataTests/FeatureTest/` is a real project (`FeatureTest.stproj`, `Assets/`
  with committed `.meta` files). Its scene `Scenes/Feature.stscene` contains every registered component, the project
  has assets of every type (the tiny binary ones come from `Tools/GenerateAssets.py`, whose outputs are committed), and
  its script module `Scripts/` (target `StrataTestScriptsFeatureTest`) exercises the whole script SDK. Feature scripts
  derive from `FeatureTest::FeatureScript`: `Expect(condition, "description")` counts checks and keeps the first
  failure, `Completed` marks the end of a scenario, and `Journal()` records events in the scene's "Journal" entity.
  The runners:
  - `StrataTests.FeatureTest` (label `feature`, no GPU) plays the scripted scenario (`PlayFeatureScene`: 200 frames,
    simulated input, a hot reload of the module halfway, audio on the null device; after every frame the scene's caches
    must match a full recomputation, `Scene::ValidateWorldTransforms`/`ValidateHierarchy`) three times: headless (suite
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

### Script modules and package tests

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
  must keep that command script passing. `StrataEditor.Install` (`Editor/InstallEndToEnd.cmake`) installs the build into
  a fresh prefix, checks the layout (exactly the top-level entries a release holds), and makes a game with the installed
  editor alone: `project.create` from `basic3d`, `script.build` (whose cache must name the installation as
  `STRATA_ENGINE_DIR`, never the source tree), play and export; the exported game runs headless. The `Package*` suites
  build in the system temp directory, where MSBuild does not track files (MSB8029) and may relink unchanged modules: do
  not rely on a build leaving the module unchanged there (use the fake CMake instead).

## Architecture rules

AGENTS.md, [Architecture rules](../AGENTS.md#architecture-rules), states every architecture rule, five of them in
short. Their full wording:

- **Threading:** the main thread owns the scene, input, scripts and rendering submission. Background work goes through
  `JobSystem` (`Submit` for CPU work, `SubmitIO` for blocking I/O, `JobPriority::Background` for long work no frame
  waits for, such as an import's texture compression or an export's pack compression: it runs on background threads
  below the normal thread priority, which leave four hardware threads to the frame but keep at least half of them
  (`JobSystem::GetDefaultBackgroundThreadCount`), so it never takes the processors from the frame, whose own work runs
  on the normal-priority workers). A thread that waits for a job runs that job itself if it has not started, and never
  other jobs. Results return to the main thread through `Application::SubmitToMainThread` or polled queues — never
  callbacks that touch engine state from worker threads.
- **NVRHI pitfalls:** never take the address of a handle (`&m_Pipeline`): `RefCountPtr::operator&`
  releases the reference and returns the raw pointer's address, so assigning through it leaves a
  dangling object. NVRHI places automatic barriers only when the binding sets change; consecutive
  dispatches through the *same* binding set that read each other's UAV writes need an explicit
  `setBufferState`/`setTextureState(..., UnorderedAccess)` plus `commitBarriers()`. NVRHI keeps what a
  command list references alive until the GPU finished it, but its Vulkan backend does not reference
  the textures of clears (`clearTextureFloat`/`UInt`, `clearDepthStencilTexture`) and resolves: a
  target that a frame in flight only cleared is destroyed with its last handle (a validation error,
  and freed memory in use can lose the device). Drop render targets, and what references them, through
  `Renderer::ReleaseDeferred` (`DeferredReleaseQueue`: held until the GPU finished the work submitted
  before, never waiting, then destroyed within a frame's budget, `c_FrameDestroyBudget`, since a texture costs the
  driver about 0.1 ms), as `SceneRenderer` does when it is resized.
- **Layers:** every file under `Strata/src` belongs to one layer of `StrataTests/Architecture/Layers.json`, and an
  include that crosses layers must be allowed there (a layer includes only itself and the lower layers it lists).
  `Architecture.Layering` (in `StrataTests.Core`) fails on any other include, on files in no layer (new folders and
  files get a pattern) and on stale lines of `LayeringAllowlist.txt`, the known exceptions, each with the workstream
  that removes it. Never add a line or widen `MayInclude` to make a sideways or upward include pass: invert the
  dependency (an interface in the lower layer, like `InputWindow`; a registry the higher module fills; a scene service,
  `Scene/SceneServices.h`, through which the systems of a playing scene use each other, or an engine service,
  `Core/EngineServices.h`, for what outlives scenes) instead. A
  proven downward dependency may widen `MayInclude`, with the reason in the layer's `Notes`. Removing an exception
  lowers `MaxAllowlistEntries`. Details: Docs/Architecture.md, "Layers".
- **Module registration:** modules extend the engine through registries (components, asset loaders and importers,
  built-in asset objects, scene systems), never through lower layers calling functions of higher ones. Each module
  has one registration function in its own folder (`Register<Module>Module` in `<Module>/<Module>Registration.cpp`);
  only `Engine::RegisterBuiltinModules` (`Engine/BuiltinModules.cpp`, the composition root) calls them, once per
  process, before anything reads a registry (the `Application` constructor; `StrataTests`' `main`). A new module adds
  its function there, in dependency order. Tooling stays out of the composition root, which shipped games link too: the
  asset pipeline's `RegisterAssetPipeline` is handed in by the programs that import assets
  (`ModuleRegistrationOptions::AssetPipeline`: the editor's `CreateApplication`, `StrataTests`' `main`), so the
  importers are not linked into StrataRuntime. Components can only be registered while the composition root runs (the
  registry freezes at its end): games and tests pass theirs in `ModuleRegistrationOptions::Extra`. Scene systems
  declare their update order (`SceneSystemDescriptor::After`/`Before`) instead of relying on registration order, and
  are made with `MakeSceneSystemDescriptor<T>` so that `Scene::GetSystem<T>` finds them and frames call only the
  per-frame callbacks `T` overrides (`SceneSystemDescriptor::CallsUpdate` and the others; declare overrides public).
  Scenes keep the components of modules a build lacks: they load with one warning per component name into
  `UnknownComponentsComponent` and are saved back unchanged (also through play mode, prefab snapshots, duplication and
  undo). Details: Docs/Architecture.md, "Composition root and registries" and "Scene runtime lifecycle".
- **Scene caches and the transform contract:** per-frame scene cost follows what changed. `Scene` mirrors the hierarchy
  as EnTT handle links (`HierarchyComponent`, `Scene/SceneHierarchy.h`), recomputes cached world transforms
  (`WorldTransformComponent`) only below entities whose transform changed, keeps `ActiveInHierarchy` exact as activity
  and parents change, and indexes names and tags for `FindEntityByName`/`FindEntitiesByTag`. So **code that writes
  `TransformComponent` fields directly must signal the change**: `Entity::MarkModified<TransformComponent>()` or
  `registry.patch` (`ComponentAccess`, `Scene::SetWorldTransform`, the script API and `TransformEdit` do), or
  `Scene::MarkTransformChanged` where listeners must not hear an `on_update` (physics writing simulated poses back).
  Writes to `NameComponent::Name` and `TagComponent::Tag` must signal the same way, and so must writes to
  `MeshRendererComponent` fields (the renderer's proxies, `RenderScene`, and physics' mesh colliders follow the
  component's `on_update`). Entities created since the last `UpdateWorldTransforms` (or the last frame rendered) are
  read anyway, so values written right after `AddComponent` need no signal. Hierarchy changes go through `Scene`
  (`SetParent`, `SetSiblingIndex`, `PlaceEntities`, `DestroyEntities`...); never edit `RelationshipComponent` or
  `HierarchyComponent` directly. Debug builds assert after every `OnUpdateRuntime` and `OnUpdateEditor` that the caches
  match a full recomputation (`Scene::ValidateWorldTransforms`, `ValidateHierarchy`); a "has inconsistent caches"
  assertion names the entity whose write was not signaled. They also compare the render proxies with the scene after
  every `RenderScene::Sync` ("The render proxies of scene ... differ"). Read sibling positions with
  `Scene::GetSiblingIndex` (cached), never by searching the child list, and remove many entities with one
  `DestroyEntities` call. A registry holds at most `Scene::c_MaxEntities` (1,048,575) live entities; creating more
  fails with an error.

## Asset pipeline

### Asset files

- A project's assets are the files under its asset directory (`Assets/`). Each file has a sidecar
  `<file>.meta` (JSON: handle + import settings) that is committed with it; moving or renaming must go
  through `EditorAssetManager::MoveAsset` or `MoveFolder` (or move the `.meta` along) so the handle survives, and the
  editor deletes through `MoveToTrash`/`RestoreFromTrash`. Code that lists folders or asks what uses an asset reads the
  manager's indexes (`GetFolders`, `GetAssetsInFolder`, `GetReferencers`), rebuilt views only when `GetChangeVersion`
  changed; never walk the disk or parse documents per frame.

### Importers

- Importers (`Asset/AssetImporter.h`, built-ins in `Asset/AssetImporters.cpp`) turn source files into
  the stored form, cached per handle in `<project>/.strata/Cache` (derived data, never committed). A
  cached import is redone when the source content, the import settings, the importer's `GetVersion()`
  or another file the import read change — **bump the version whenever an importer's output format
  changes.** Importers read files other than their source only through `ReadImportDependency`, which
  confines reads to the asset directory and records the file as a dependency. An importer with settings describes every
  one in `GetSettingsProperties` (one `ImportSettingsBuilder` entry each: type, default, range or choices, tooltip;
  choices show in words, "NormalMap" as "Normal Map", and `PowerOfTwoLimit` makes a size limit a choice of "No limit" or
  a power of two), matching the keys of `GetDefaultSettings`; the asset inspector draws them and
  `asset.setImportSettings` validates against them (`NormalizeImportSettings`), so a new setting is one entry there. A
  setting that other settings can leave without effect says so in `GetInactiveReason` (the inspector then shows it
  disabled, with the reason).
  Editors re-import in the background (`EditorAssetManager::SetImportSettingsInBackground`, `ReimportAssetInBackground`,
  `IsImportPending`); the blocking forms are for tools and tests. The editor also opens projects with their imports in the
  background (`Scan(ScanImports::InBackground)`, `EditorContextSpecification::ImportInBackground`; tests and scripted runs
  scan blocking, so every asset is imported when the project is open): a load of an asset whose first import is still
  running waits for it instead of failing (`AssetManagerBase::IsAwaitingImport`, `ResumeLoadsAwaitingImport`; frames apply
  finished imports, and a wait before the first frame, `EditorContext::WaitForSceneLoad`, applies them itself:
  `EditorAssetManager::WaitForImportProgress`).
  **Nothing in the editor waits for a background import** (only `WaitForImports` and `BuildAssetPack`, for tools and
  tests): imports are cancelled instead. Closing the project, opening another and quitting cancel them all
  (`EditorAssetManager::Close`); moving, deleting or re-importing an asset, new settings and file changes cancel only that
  asset's import (replaced at once where it is still wanted). A cancelled import stores nothing, and its job uses only
  what it shares with the manager (`BackgroundWork`), so nothing waits for it to stop. Importers check
  `ImportContext::IsCancelled` between pieces of work that take milliseconds (a new importer with long loops must too;
  the texture cooker checks per block) and read project files through `ReadImportSource`/`ReadImportDependency` (under the
  manager's file lock, nothing once cancelled). Docs/Architecture.md, "Cancelling imports". `asset.import` never imports
  in a frame either (`EditorAssetManager::ImportExternal`: copies on an I/O job, imports in the background, rolls back
  failed items, logs a failure once); an importer whose files name others (a glTF's buffers and images) lists them in
  `FindExternalFiles` (with `ReadsExternalFiles`), so the copy brings them along, and messages that can quote file bytes
  are made valid UTF-8 (`StringUtils::MakeValidUTF8`).

### Engine-native assets, sub-assets and built-in assets

- Engine-native assets (`.stscene`, `.stprefab`, `.stmat`, `.stanimator`, `.stworld`) are JSON documents with a
  `{ "Strata": { "Format": ..., "Version": ... } }` header, stored as-is. Their loaders validate fully;
  JSON entry points are named `FromJson`, byte entry points `Deserialize`.
- Importers may produce sub-assets (e.g. meshes of a model); their handles are derived from the parent
  handle and a stable key (`DeriveSubAssetHandle`), so they are stable across re-imports and machines.
- Built-in assets (primitive meshes, default material) have fixed handles 1–255 (`BuiltinAssets`) and
  exist in every asset manager.

### Asset packs

- Shipped games read asset packs (`.stpak`, `AssetPack`, version 2: entries 4 KiB-aligned, cut into 256 KiB
  zstd-compressed blocks with CRC-32C checksums (stored as they are when compression saves less than an eighth, and then
  read straight into place), read by byte range through one shared `FileReader`, with dependency and
  mesh bounds tables; version 1 packs still load) through `RuntimeAssetManager`, which mounts the game's pack and then
  the manifest's `PatchPacks` (later entries replace earlier ones by handle). The editor plans them
  (`EditorAssetManager::PlanAssetPack`: ordered by first use from the start scene; with pruning only what the scenes and
  `Export.AlwaysInclude` use) and writes them with `AssetPackWriter` (tooling: games link only zstd's decompressor).
  Pack readers validate everything before trusting it: extend the corruption tests (`StrataTests/src/Asset/AssetPackTests.cpp`)
  with every format change.

### References and bounds

- **References and bounds:** `AssetManagerBase::GetDependencies` and `GetBounds` answer from what the asset pipeline
  recorded (import records, version 2; engine-native assets have records of their own) and from packs' tables, the same
  in the editor and in games. `AssetDependencies::Scan` reads documents (asset properties of every registered component,
  script fields of asset type, material texture maps); importers add what their stored form does not show
  (`ImportResult::References`, e.g. a glTF mesh's materials) and mesh bounds (`Bounds`). A new asset property is found
  automatically; a new document type or a stored form with references needs `AssetDependencies::Scan` or the importer to
  record them (each valid handle once, never the asset itself: `AssetManagerBase::NormalizeReferences`, which code that
  compares scanned references with recorded ones uses too). Reads of requested assets other than scenes, prefabs and
  models prefetch their direct dependencies (only into the budgets' headroom beside what is on its way and a window of
  loads in flight, never evicting, not chaining: a finalizer must not request what an asset that arrives only prefetched
  references, `AssetFinalizeContext::Prefetched`, and its dependencies are prefetched once something requests it);
  loading screens pin what using an asset needs with `RequestLoadWithDependencies` (`AssetGroupRequest`: progress by
  bytes, cancel; its closure stops at other scenes, `AssetClosureScope::Load`: a level that names the next one does not
  load it), or a group they chose with `RequestLoadGroup` (scene loads take a pool's share of the closure only when all
  of it fits in the budget's room, estimated by what first loads read, a texture's mip tail while loads read prefixes
  (`AssetManagerBase::EstimateFirstLoadBytes`), and leave it to what the scene draws otherwise, `SceneLoader`).

### Textures

- **Textures** cook to GPU block-compressed formats by default ([Docs/Architecture.md](Architecture.md), "Texture
  cooking"): `TextureImporter::ChooseFormat` picks BC7 (color, sRGB, and linear data of several channels; at
  `"Quality": "Fast"` opaque color is BC1), BC5 (normal maps), BC4 (one-channel data, marked `Grayscale`: sampled as
  `(r, r, r, 1)`) or RGBA16F (HDR) unless the settings say `"Compression": "Uncompressed"`. BC7 is encoded by Strata's
  own encoder (`Asset/BC7Encoder`, written from the format's definition in `Renderer/BC7Format.h`, which the decoder
  shares; its tests compare every block it stores with the decoder), BC1, BC3, BC4 and BC5 by the vendored stb_dxt.
  Mips are filtered with a Mitchell filter; the cooked `STTX` version 2 stores the levels smallest first
  so the mip tail is one range after the header. Never compute texture sizes from bytes per pixel: use `GetMipByteSize`
  (whole 4x4 blocks) and `GetTextureFormatInfo`; read cooked data through `ParseCookedTexture`, which validates it. A
  change to the cooked output bumps the importers' versions (`TextureImporter` and `GltfImporter`). Devices without BC
  support get the blocks decoded at load time (`Texture::IsBlockCompressionSupported`, `BlockCompression`).

### Residency

- **Residency** ([Docs/Architecture.md](Architecture.md), "Streaming and residency"): memory is bounded by budgets
  per pool (`AssetResidencyBudgets`: GPU textures 50% and GPU buffers 15% of the device's memory budget, CPU unlimited;
  128 MiB of loads in flight, 16 MiB and 4 ms of finalization per frame with 2 ms of upload steps, 32 MiB of upload
  staging), and the least
  recently requested assets of a pool over budget are evicted. Hold `AssetHandle`s across frames, not `Ref`s, and
  request what you use every frame you use it (`GetAsset`): a held `Ref` keeps an asset from being evicted (it would
  free nothing), so it defeats the budget. Pin what gameplay must keep whether or not it is used
  (`AssetManagerBase::Pin`, `AssetPin`; scripts: `Assets::RequestLoad` until `Assets::Release`). Code that keeps an
  asset's data alive in other objects reports it (`Asset::IsDataShared`). A cache that keeps data derived from an asset
  while the asset is evicted (the render scene's mesh layouts) checks, on the asset's content changes, whether the
  source's revision is still the one its data came from (`AssetManagerBase::GetSourceRevisions`): evictions keep it,
  `ReloadAsset` (a reimport) changes it and announces that even when nothing is loaded. Never block on a load; scene
  owners trim what only the previous scene used after a switch (`AssetManagerBase::ScheduleTrim`, sparing what the new
  scene's load brought in, `SceneLoadRequest::GetAssets`).

### Adding an asset type

- Adding an asset type: an `Asset` subclass with a cooked/serialized form, a loader registered by the
  registration function of the module that owns the type (e.g. `RegisterRendererModule`; it takes the stored bytes over
  with `AssetLoadData::TakeBytes` when the asset keeps them, instead of copying them), an importer if it comes from
  external files (`RegisterAssetPipeline`), `GetMemoryUsage` reporting what it holds in each pool once finalized (and
  `GetFinalizedMemoryUsage` when finalizing moves data between pools), and tests for round trips and corrupt data
  (every loader must reject truncated or garbage bytes without crashing). GPU uploads in `FinalizeOnMainThread` go in
  steps of at most `c_AssetUploadStepBytes` within the context's budget (`AssetFinalizeContext`,
  `AssetFinalizeResult::Pending`) and report their bytes; never upload an unbounded amount in one call. They record into
  `AssetFinalizeContext::Upload` (the renderer's assets downcast it with `RendererUploadContext::From`): the asset layer
  knows no graphics API (`Architecture.Layering` refuses NVRHI and Vulkan headers there). An asset that can be partly
  resident registers a prefix function (`AssetLoaderRegistry::RegisterPrefix`: its loader then gets partial data,
  `AssetLoadData::IsPartial`), reads the rest by range (`AssetManagerBase::RequestRange`, polled), reports memory it gains
  or drops without being replaced (`AssetManagerBase::UpdateMemoryUsage`) and its levels (`Asset::GetDetailLevels`); the
  work a module does for every manager each frame is an `AssetManagerExtension` (registry filled by the module's
  registration function).
- A type whose documents or stored form reference other assets records those references
  ([References and bounds](#references-and-bounds)): pruned exports, the delete checks and dependency prefetches know
  an asset's references only from those records. An importer that makes sub-assets derives their handles with
  `DeriveSubAssetHandle` ([Engine-native assets, sub-assets and built-in
  assets](#engine-native-assets-sub-assets-and-built-in-assets)).

### Mip streaming

- **Mip streaming** ([Docs/Architecture.md](Architecture.md), "Streaming and residency", Mip streaming): a texture's
  first load holds its header and the levels up to 32x32 (its resident tail), and each manager's `TextureStreamer` raises
  and drops its levels to what the views drew it at (`SceneRenderer` reports after every frame the loaded texture maps it
  resolved, `GetVisibleTextures`, so reports look nothing up: texel density where each surface is nearest the camera,
  from the meshes' `Submesh::UVDensity` and the instances' scales, not the size of their bounds, worked out only for
  draws of textured materials; `RequestLevel` for other uses in a frame, `TextureStreamer::Hold` to keep a texture whole
  across frames nothing draws it in; a texture only requested, `GetAsset`, keeps what its first load holds, and thumbnails
  do not even load it: they read its cooked mip tail by byte range, `ThumbnailCache`). Under a
  texture budget the most needed textures come first: the less needed give up their finest levels
  (`LevelsDroppedForBudget`), and what does not fit gets the finest level that does. A texture's GPU texture and
  bindless slot change when its levels do: resolve the slot every frame (`Texture::GetBindlessSlot`), never cache it.
  Renderers and tests that compare exact pixels set `SceneRendererSpecification::TextureStreaming = false` (every level,
  pending until resident); tests of whole-asset residency turn the manager's partial loads off
  (`AssetManagerBase::SetPartialLoads(false)`).

## Scripting

Game logic is C++ script classes compiled into a **script module**: a shared library built against
`StrataScriptCore` only (it never links the engine). The engine loads it through `ScriptEngine`; playing scenes run
their Script components through the built-in "Scripting" scene system (`ScriptSystem`, not created in simulate mode).
Writing scripts is described in `.claude/skills/strata-scripting/SKILL.md`.

| Where | What |
| --- | --- |
| `StrataScriptCore/Include/StrataScript/ScriptABI.h` | The versioned C ABI: host API table, module/class/field descriptors, values. |
| `StrataScriptCore/Include/StrataScript/*.h` | Header-only C++ SDK (`StrataScript.h` includes all): `Script`, `Entity`, `Scene`, `Assets`, `Input` (keyboard, mouse, gamepads, the cursor mode), `Time`, `Log`, `RigidBody` and `Physics` (bodies, queries and collision layers; `Collision` for the contact callbacks of `Script`), `CharacterController` (walking, jumping, ground state), `AudioSource` and `Audio`, `Animator` (states, parameters, joints; `OnAnimationEvent` of `Script`), `Game` (quit, scene loads and their progress, the window's mode, resolution and vsync, save files), `SubScene` (additive scenes), `ST_SCRIPT_CLASS`/`ST_SCRIPT_FIELD`. |
| `StrataScriptCore/Source/ScriptModuleEntry.cpp` | The module entry points, compiled into every module by `strata_add_script_module()`. |
| `StrataScriptCore/CMake/` | `strata_add_script_module()` and the package game projects use (`StrataScriptCoreConfig.cmake`). |
| `Strata/src/Strata/Scripting/` | `ScriptEngine` (module, hot reload, faults, watchdog), `ScriptModule` (loading, validation, guarded calls), `ScriptSystem` (instances and lifecycle), `ScriptHostAPI` (the host table), `ScriptValue` (value conversion). |
| `StrataTests/Scripts/`, `StrataTests/src/Scripting/` | Test modules (API, reload V1/V2, faults, invalid modules) and the `Scripting.*` suites. |
| `StrataTests/FeatureTest/Scripts/`, `StrataTests/src/FeatureTest/` | The feature test's script module (the whole SDK in a real scene) and its runners (see [Testing](#testing)). |

### Gameplay API

Gameplay API (host functions appended to ABI version 1 and wrapped by the SDK; the skill has an overview with recipes):

- Physics: `RigidBody` (velocities, forces, impulses, `Teleport`) and `Physics` (raycasts, overlaps) go through the scene
  services the scene's `PhysicsSystem` offers (`IBodyMotionQuery`, `IBodyControl`, `IPhysicsQueries`), also in `OnCreate`:
  scenes start every system before the scripts (`SceneSystem::OnRuntimeStarted`). `Physics::SetLayersCollide`,
  `GetLayersCollide` and `GetLayerIndex` read and change the playing scene's collision layers
  (`SceneSettings::PhysicsLayers`; physics applies a change before its next step). Contacts reach scripts as
  `OnCollisionEnter/Exit` and `OnTriggerEnter/Exit` (class descriptor callbacks taking a `StrataScriptCollision`):
  `ScriptSystem` listens to the scene's `ICollisionEventSource` while it runs and calls the scripts of both entities,
  skipping removed, disabled and not yet started instances; contacts begin only for active entities, and each instance
  that got an Enter gets its Exit, also after its entity was deactivated.
- Character controllers: `CharacterController` (`Move`, `Jump`, `Teleport`, the ground state) goes through the scene's
  `ICharacterControl` (the `PhysicsSystem`, see Docs/Architecture.md, "Engine modules", Physics). A controller is an entity
  with an enabled `CharacterControllerComponent` and no `RigidBodyComponent`; its contacts reach the scripts like a body's.
- Audio: `AudioSource` and `Audio` go through the scene's `IAudioPlayback` (the `AudioSystem`); without it (simulate mode)
  the calls fail. The master volume belongs to the engine (the engine service `IAudioOutput`), so it works in any scene.
- Animation: `Animator` (`Entity::GetAnimator`: play and cross-fade states, play clips, parameters, speed, the current
  state, joint transforms; host functions `AnimatorPlay` to `AnimatorGetJointTransform`, appended to ABI version 1 after
  scene streaming's) goes through the scene's `IAnimatorControl` (the `AnimationSystem`); without it (simulate mode)
  the calls fail. Events of controller states reach the scripts of the animator's entity as `OnAnimationEvent(name)` (a
  class descriptor callback taking a `StrataScriptString`): `ScriptSystem` listens to the scene's `IAnimationEventSource`
  while it runs, and calls only scripts of active entities. Commands a script gives before the controller has loaded
  apply once it has (`GetCurrentState` reports the waiting state).
- Game flow: `Game::Quit`, `LoadScene` and `ReloadScene` set `Scene::RequestQuit`/`RequestSceneLoad`, which the scene's owner
  honors after the frame (see [Editor](#editor)); while the next scene loads, `Game::GetSceneLoadProgress` reads the
  progress the owner sets (`Scene::GetSceneLoadProgress`, negative without a switch).
- Scene streaming (host functions `LoadSceneAdditive`, `UnloadSubScene`, `IsSubSceneLoaded`, `GetSceneLoadProgress`,
  `GetAssetLoadProgress` and `InstantiateMany`, appended to ABI version 1 in that order): `Scene::LoadAdditive` loads a
  scene asset into the playing scene (`Scene::LoadSubScene`) and returns a `SubScene` (`IsLoaded`, `Unload`: the
  sub-scene leaves the game at once and its entities are destroyed over the next frames, `Scene::UnloadSubScene`);
  `Assets::GetLoadProgress` requests what using an asset needs (its dependency closure without other scenes it names)
  and returns the share of its bytes in
  (`ScriptSystem::GetAssetLoadProgress`: pinned like `RequestLoad` until `Assets::Release`; at 1 a prefab's instance draws
  complete in its first frame); `Scene::InstantiateMany` makes one instance of a prefab or model per transform in one call
  (`EntityTemplate::InstantiateMany`, 16 floats per transform across the ABI).
- Input: gamepads (`Input::IsGamepadConnected`, `IsGamepadButtonDown/Pressed/Released`, `GetGamepadAxis` and
  `GetGamepadStick` with dead zones) and the cursor (`Input::SetCursorMode`: Normal, Hidden, Locked for mouse look, which
  `Input::SetGameCursorMode` applies while the game has the input; play mode stopping gives it back).
- The game's shell belongs to the program running it, through engine services (`Core/EngineServices.h`, provided while it
  runs the game with `EngineServices::ScopedProvider`): `Game::SetWindowMode`, `SetResolution`, `SetVSync` and
  `GetWindowSize` go to `IGameWindow` (StrataRuntime's `GameWindowService` applies them after the frame; the editor's
  `EditorGameWindow` only records them for `editor.status` and the game view's notice), save files (`Game::WriteSaveFile`,
  `ReadSaveFile`/`ReadSaveText`, `DeleteSaveFile`, `ListSaveFiles`) to `IGameSaves` (`<user data>/<Game>/Saves` in a
  shipped game, `<project>/.strata/Saves` in the editor). Save names are checked in the engine before any path is formed
  (`SaveFiles::ValidateName`: no separators, no "..", no device names) and writes are atomic. Code that runs a game
  in-process (tests, tools) provides both, or the calls fail and are reported.
- Host function order: the character controller, gamepad, cursor, window, save file and collision layer functions
  (`CharacterMove` to `PhysicsGetLayerIndex`) are appended to ABI version 1 after the Animator functions.
- Assets: `Assets::RequestLoad` pins the asset for the playing scene (`ScriptSystem::RequestAsset`; never evicted to meet
  memory budgets, see [Asset pipeline](#asset-pipeline), "Residency") until `Assets::Release` (host function
  `ReleaseAsset`, appended to ABI version 1) or the end of play; requests do not add up.
- `Random`, `Timer` and `KeyRepeat` (`StrataScript/Gameplay.h`) run entirely in the module. The engine has classes named
  `Random` and `Timer` too, so SDK helpers are tested inside a script module, never in an engine translation unit (that
  would violate the one-definition rule).

### ABI rules

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

### Adding a host function

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

### Building and loading scripts

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
  name, `Project::MakeScriptModuleName`). Folders whose names start with '.' and build trees (folders holding a
  `CMakeCache.txt`) below it are neither compiled (`strata_add_script_module` leaves them out of its glob) nor watched
  (`ListScriptSources` and `StampScriptSources` skip them), so the module and the editor agree on its files.
  `project.create` writes that `CMakeLists.txt`, an example script and `CMakePresets.json` for IDEs
  (`Editor/ScriptProject.cpp`; `script.init` adds them to older projects; the presets carry the toolchain below and
  `STRATA_ENGINE_DIR`, are rewritten when Strata wrote them and the toolchain changed, and a `.gitignore` beside them
  keeps them out of version control: `UpdateScriptCMakePresets`); project text only goes into comments, and projects
  reject names and directories with control characters on load and save. `script.create` writes a new class
  (`MakeScriptSource`: only the validated class and field names go into the code, `ValidateNewScriptClass` refuses names
  the project's scripts or the SDK declare). The editor builds the scripts with
  `script.build` (`ScriptBuilder`, `Editor/ScriptBuild.cpp`): CMake configures `<project>/.strata/Scripts/Build` with the
  toolchain the engine was configured with (generator, platform, toolset, compiler, configuration and this checkout as
  `STRATA_ENGINE_DIR`, baked into `Editor/ScriptBuildConfig.h` at configure time; CMake is the engine's, else `cmake` on
  PATH) and builds the module into `<project>/.strata/Scripts/Bin`, in child processes polled once per frame (never
  blocking). An installed editor (`StrataInstall.json` beside its executables, `FindEngineInstallation`,
  `ScriptBuildSettings::Detect`) builds against the installation's SDK, and its first build finds a toolchain on the
  machine instead (`ScriptBuildSettings::FindToolchain`, `ResolveScriptToolchain` in `Editor/ScriptToolchain`, on an I/O
  job: phase `FindingToolchain`; later builds keep it while its files exist), as does an editor whose baked tools are
  gone (the compiler counts only for generators that are given one: a Visual Studio update replaces the versioned
  directory of `cl.exe`): Visual Studio 2022 17.10+ or 2026 with the C++ tools (vswhere, `CMAKE_GENERATOR_INSTANCE`) and
  a CMake 3.25+ that knows its generator on Windows (read from `cmake -E capabilities`, `ParseCMakeCapabilities`, never
  inferred from the version: Visual Studio 2026's own CMake 4.1.2-msvc8 knows it, CMake's 4.1 releases do not), the
  system compiler, Ninja or Make and CMake 3.25+ on Linux and macOS (modules only
  cross the C ABI, so another compiler of the platform builds them as well). Without one the build fails with what to
  install (its first diagnostic, which the Problems panel and the notification show; `script.status` -> `toolchain` says
  what the next build uses). The configure step only runs when the build tree is new, the toolchain changed (the tree is
  then removed and made again) or files were added to, removed from or renamed in the script directory (configured again
  where it is: the tree's stamp, `StrataScriptBuild.json`, holds the sorted list of the files it was configured with,
  taken from the `StampScriptSources` listing `EditorContext` passes to `ScriptBuilder::Start`; a listing cut short
  configures every build). A Visual Studio build compiles the projects as they were configured, even when the module's
  `CONFIGURE_DEPENDS` globs find new files during it, so a class New Script added would otherwise be missing from the
  module after one build. Generators that can (Ninja, Makefiles) write `compile_commands.json`. With a Visual Studio
  generator a project whose build tree would be too deep builds in the per-user build directory instead
  (`ResolveScriptBuildLocation`: `<user data>/Strata/Builds/<hash of the project directory>`, created private;
  `STRATA_SCRIPT_BUILDS_DIR` overrides the root, tests set it), and the build's log says why: MSBuild's file tracker
  cannot create its logs at paths over 259 characters, which lie up to 104 characters below the build tree (more for
  module names over about 40 characters), and fails with errors that do not name the cause
  (`CheckScriptBuildPathLength`). Only a project too deep for both is refused before anything runs, naming a shorter
  folder by its real path.
  Output is streamed to the log; errors are parsed into file/line/message diagnostics (MSVC, GCC, Clang, the GNU, Apple
  and MSVC linkers, CMake; failures with only summary lines report the end of the log). One build runs at a time: a
  second request fails until the running one finished. Cancelling a build (also by closing the project or the editor)
  ends every process it started (`ProcessSpecification::TerminateTree`: a job object on Windows, a process group on
  POSIX).

### The editor's script engine

- **The editor's script engine:** `EditorContext` owns a `ScriptEngine` per open project (active while it is open and
  set active again when play starts), loads the project's built module when the project opens (warning, once the scan of
  the project's scene and prefab files on an I/O thread found one that attaches a script, when the scripts are not built:
  `DoScenesUseScripts`; a new project's unused example is no reason to warn), keeps hot reload on for modules rebuilt
  outside the editor, and calls `Update()` once per frame before the scene. A successful `script.build` loads the module,
  or reloads it when it changed (hot reload while playing; the file watcher is paused during editor builds so the module
  reloads once). **Play builds first:** `play.start` (and the toolbar's Play, through the runner) builds the scripts when
  the sources of the script directory changed since the module was built (compared with what the editor's last successful
  build compiled while the module is still the file it left, so builds that leave the module unchanged settle it and
  removed files count; for a module built elsewhere, a source newer than it), or the module is not loaded while scenes use
  scripts (`GetPlayBuildReason`; never for a module loaded with `script.load`), waits for a build that runs, and plays
  once the build succeeded; a failed build fails `play.start` with its first diagnostics and asks the UI for the Problems
  panel (`RequestProblemsPanel`), a cancelled one fails it as `Cancelled`; `play.start {build: false}` plays what is
  loaded. While a scene is being opened `play.start` fails at once, before it builds anything. A crash while playing
  stops play mode and
  logs the class, callback and entity, and where it happened once symbolization (a job) finished (see Scripting,
  "Contained"); `play.start` then fails until the module is rebuilt (a successful build loads a crashed module again even
  when it did not change) or reloaded. `script.load {path}` runs another module file (tests use in-tree modules this
  way).

### Scripts of exported games

- **Exported games:** `project.export` writes the module the editor runs next to the game (the loaded module's file,
  verified against the digest taken when it was loaded, `EditorContext::ReadRunningScriptModule`; plus its PDB except
  in Dist builds or with `includeScriptSymbols: false`) and names it in the `.stgame` manifest (version 2,
  `"ScriptModule"`; version 1 manifests load without scripts). It refuses while a script build runs, when the module
  file changed since it was loaded (`script.reload` or `script.build` first), and when scenes or prefabs attach scripts
  but no module is loaded. `GameRuntime` loads the module (no hot reload) and makes its engine active before the start
  scene plays. A script crash disables the scripts for the session (`GameRuntime::GetScriptFault`): a headless
  `StrataRuntime` exits with code 2, a windowed one keeps running and logs it.

### Hosting modules and hot reload

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

### Crash locations and containment

- **Crash locations:** `CrashGuard` records the faulting instruction and, on Windows x64, its callers' return addresses
  (unwound from the exception's context, `CrashInfo::Stack`; no walk after a stack overflow). `ScriptModule` names each
  frame's module at once (`Symbolizer::Describe`) and resolves the module's own frames on a job (`Symbolizer::Resolve`:
  DbgHelp reads the PDB beside the module as it was built, also for hot-reload copies, serialized, with the calls the
  compiler inlined at each address as frames of their own before it, `ScriptCodeLocation::Inlined`; on Linux the ELF
  symbol table and on macOS dladdr give the function, never file or line). `ScriptFault::Location` is the first frame of
  the module with a source that is not C runtime or SDK entry code (in optimized builds too the line that ran, not the
  call of the function it was inlined into), `Stack` the frames; `Symbolized` says the job
  finished (`ScriptModule::GetFault` merges its result). `script.status`, the Console (a click opens the line), the
  Problems panel and the play-stop log show them. Script modules write a PDB in every configuration
  (`strata_add_script_module`).
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

## Animation

`Strata/src/Strata/Animation/` holds the assets `Skeleton`, `AnimationClip` and `AnimationController` (`.stanimator`),
`AnimatorInstance` (one animator's playback, no scene) and `AnimationSystem`, the built-in "Animation" scene system
(Play only, after Scripting and before Physics). How it all works: Docs/Architecture.md, "Animation".

- **Poses are not entities.** An `AnimatorComponent` (skeleton, controller or clip, speed, loop, play on start, root
  motion, culling mode) owns a pose in the system; meshes follow it through a `SkinComponent` naming the animator, other
  entities through an `AttachToJointComponent`. Never create entities per joint or write joint transforms into the
  scene: a character's joints must add no transform changes. The glTF importer follows this (joints with content only).
- **Through scene services:** the renderer reads poses through `ISkinningPoses` and reports what it drew; scripts and
  other modules use `IAnimatorControl` and `IAnimationEventSource` (`Scene/SceneServices.h`). Only the Engine and
  AssetPipeline layers include Animation.
- **Evaluation is parallel and deterministic:** jobs write only their animator's record (they read the registry and the
  asset sets, never write them; what needs the main thread, such as new asset sets or requests, is queued for it);
  everything that changes the scene (root motion, attachments, events) happens on the main thread afterwards, animator by
  animator. Keep it that way: a test compares runs with different thread counts bit for bit.
- **Root motion** moves entities only in `AnimationSystem::ApplyRootMotion` (signaled through the transform contract); on
  an entity with a character controller the controller walks the move instead (`ICharacterControl::AddCharacterMotion`,
  colliding as it walks) and only the turn is written.
- **Culling:** `WhenVisible` animators skip their pose while no renderer reports them (`ISkinningPoses::ReportVisible`:
  a mesh they skin in view, or its shadow on what the view shows); time, transitions, events and root motion go on. They
  publish their reach with the pose (`SkeletonPose::Reach`, where their clips take the joints), which bounds their meshes
  too, so a frozen pose never hides an animator from the view that would pose it. An animator that skins no mesh but
  places attachments poses every update. Code that needs a pose of a skipped animator (a joint transform) gets its latest
  pose. A renderer learns when it drew a pose older than its animator's time (`ReportVisible` returns it,
  `SceneRendererStats::StalePoses`), and the system poses such an animator for the next frame, also while the scene is
  paused (`SceneSystem::OnPausedUpdate`, which paused frames call instead of the updates): `viewport.capture` renders
  again until it shows the pose of the moment.

## Editor

- `StrataEditorCore` (`StrataEditor/src/Editor/`) is the editor without UI: `EditorContext` (project, asset
  manager, edited scene, play mode, selection, undo history) and `EditorCommandRegistry`. `StrataEditorUI`
  (`StrataEditor/src/UI/`, `Panels/`, `EditorLayer`) is the ImGui interface on top of it; it reaches the application only
  through `EditorHost` (implemented in `EditorApplication.cpp`), and the `StrataEditor` executable runs it. The panels
  only draw state and call commands; the tests link both libraries.

### Editor UI rules

- **Editor UI rules** (the 'Bedrock' look; `Editor.UI.Source` scans the UI code for the first three, with an empty
  allowlist):
  - Colors come only from the theme's tokens (`UI/Theme.h`: the `ThemePalette` and, preferably, what they mean in
    `ThemeColors`): no colors spelled in numbers outside `UI/Theme.cpp`, however they are written (`ImVec4`, `ImColor` or
    `glm::vec4` built or declared from numbers, `IM_COL32`, packed hex colors, braced numbers passed to color
    functions). `UI::ApplyTheme` styles every `ImGuiCol_`; an ImGui upgrade with new colors fails the build until they
    get a token. Rows of lists with a selection draw through `UI::PushSelectionColors`, so hovering stays neutral and a
    selected row keeps the accent under the mouse.
  - Sizes are relative to the font (multiples of `ImGui::GetFontSize()` or `GetFrameHeight()`, or the style's sizes),
    never fixed pixels above 16, so the UI follows the UI scale (the window's content scale, or `--ui-scale`). The scan
    judges every value passed to size-taking calls by the product it is part of, and follows names that hold sizes
    (`const float width = 200.0f` used later is found too). `UI::ApplyTheme` sets every size ImGui scales to an even
    base (hairlines stay one pixel), so 150% and 200% give exact multiples.
  - Panels are `EditorPanel`s registered with `EditorPanelRegistry` (`UI/EditorPanelRegistry.h`, built-ins in
    `EditorLayer::RegisterBuiltinPanels`): the registry begins their windows (icon and title, `###<id>`), toggles them in
    the View menu and keeps their open state in imgui.ini (`StrataPanels`); panels never call `ImGui::Begin`. The launcher
    (`Panels/WelcomePanel`) is the one panel with `EditorPanelPlacement::Launcher`: drawn instead of the dock space while
    no project is open. Panels ask the editor window for its dialogs (New Project, Open Sample, Open Project, About
    Strata, New Script, Recover Unsaved Changes), its panels and inspection (`ShowPanel`, `ShowActivity`,
    `ShowAsset`) and the unsaved-changes question through `EditorPanelContext::Shell` (`UI/EditorShell.h`).
  - Text uses the editor's fonts (`UI/EditorFonts.h`, embedded from `StrataEditor/Resources/Fonts`): Inter for the UI
    with the Lucide icons merged in, Inter SemiBold for headers, JetBrains Mono for logs, IDs and numbers, at the type
    scale's sizes (`UI::PushFont(EditorFont, TextSize)`: 12, 14, 17, 24). Icons are text (`UI/Icons.h`, generated from the
    font by `StrataEditor/Tools/GenerateIconHeader.py`). ImGui's built-in font is never added. Chinese, Japanese and
    Korean text comes from the system's fonts (`Platform::FindFallbackFontFiles`), read on an I/O thread at startup and
    merged behind the editor's own glyphs between frames (`EditorFonts::BeginLoadingFallback`, `UpdateFallback`).
  - Controls come from the widget kit (`UI/Widgets.h`: toolbar, icon and action buttons, links, chips, status pills,
    section headers, headings, cards and entry cards, text fields, copyable code, the brand mark, modal dialogs); every kit
    widget records its rectangle in `UI::ItemProbe` under its id.
  - UI tests draw the real `EditorLayer` without a window or GPU through `StrataTests/src/Editor/ImGuiHarness.h` (a fake
    `EditorHost`, ImGui's texture requests honored without a renderer, injected input and typed text, a clipboard of its
    own, `ClickItem` by probe key) and `HarnessEditor.h` (an editor on that harness, `ReplaceText` for kit text fields).
  - Idle throttling: a windowed editor runs at the full rate while anything happens (input in the last 0.5 s, a running
    unpaused scene or pending steps, loading assets, pending commands, an automation request in the last second, a
    script build, a panel's `IsAnimating`) and otherwise at 30 frames per second (10 without the focus), through
    `EditorHost::SetMaxFrameRate`. Headless editors, `--frames` runs and command scripts are never throttled; a panel that
    animates without input reports it through `EditorPanel::IsAnimating`. The frame time the status bar, the viewport's
    stats and `editor.status` (`editor.frameRate.frameMilliseconds`, with the measured rate) show is what frames cost
    (`Application::GetLastFrameWorkTime`: CPU time without the waits for the GPU, the display and the frame rate cap;
    `EditorPanelContext::Frame` for panels), so an idle editor does not read as a slow one.
  - Toasts tell the person what happened without their looking for it: `EditorContext::GetNotifications()`
    (`Editor/EditorNotifications`, core) holds at most four, merged when they repeat and expiring by level (errors last
    longest, hovering keeps one); `UI/Notifications` draws them at the bottom right above the status bar, never taking the
    keyboard focus and doing nothing while there are none. They come from commands the person runs (`RunEditorCommand`
    and the runner for `CommandSource::UI`: failures, and warnings such as edits of the running game,
    `NotifyCommandOutcome`), script builds (start and end) and crashes, finished exports, imports that failed while nobody
    waited for them (one toast counting them: `EditorAssetManager::TakeImportFailures`, `EditorNotification::Group`),
    files dropped on the editor that no importer reads, unsaved changes a scene replacement discarded (who did it: an
    agent by its name, a command script, or the person's own Don't Save; `EditorContext::NotifyDiscardedChanges`), assets
    an agent or a command script moved to the trash (`asset.delete`, `asset.deleteFolder`: who did it and that Undo
    restores them, `NotifyAssetsTrashed`) and recovery data left on offer; their actions are Show in
    Activity (the command's journal entry), Show Asset, Show Problems (failed script builds and script crashes), Show in
    Console and Recover. A command whose failure a notification of its own announced (a build it waited for) sets
    `EditorCommandResult::Announced`, so the person gets one toast per failure. Post one for news the person would otherwise miss; commands of tools and scripts get their errors
    as answers and post none.
  - Status pills say what is wrong and lead to it: errors (unread since the Console was last focused, not counting the
    frame its window appears in), a failed automation start (`--no-automation` is shown as off), scripts that scenes use
    but are not built (click to build), failed or crashed (click to see the Problems panel), assets that failed to load or
    pools close to or over their budget (the assets pill,
    `N ready · M streaming · <pool> X/Y MB` for the pool closest to its budget, `GetMostPressuredPool`, after
    `K importing` while imports run (`editor.status` has the count as `assets.importing`), in Sulfur from
    85% of it and Cinnabar beyond it or on failures; click to open the Streaming panel), bodies of the running game
    without collision (`N bodies without collision`, Cinnabar, only while its physics world is at its growth limit with
    bodies waiting; click to open the Streaming panel's Physics section). The frame pill (`ms · FPS · J
    jobs`, jobs running or queued averaged over 60 frames) opens the Performance panel. Where the pills' full texts do
    not fit side by side (a window about 1,000 points wide with every stream's pill) they all show compact ones, until the
    full ones fit with a margin again (`EditorLayer::StatusPillText`: who is connected without the port, the class count
    without the module, how many controls are held, no memory or jobs; the tooltips keep the details): a new pill passes
    its texts through it. Panels collect what the status bar shows about them in `OnUpdate`, which runs while they are
    closed. Widgets drawn every frame write tooltips that take work (formatted statistics) only while they show
    (`UI::IsTooltipDue`). `editor.showPanel {panel, scripts?}` (registered by
    `EditorLayer`, like `editor.showDialog {dialog: "export" | "newProject" | "openSample" | "recovery" | "newScript" |
    "externalEditor" | "about"}`, the commands of the window itself) opens a docked panel and brings it to the front, so
    agents can show the person what they look at (`scripts`: the Content Browser's Scripts root or its asset folders); it
    fails while the launcher shows. A panel asked for (`EditorPanelRegistry::Focus`)
    stays open through a layout reset and the saved open states read at the first frame, and is asked for again until its
    tab shows; while a modal dialog waits for the person it opens behind it (focusing it would close the dialog). Glyphs a font lacks show as its fallback glyph: draw color swatches as rectangles (the monospaced font has no
    icons), and check panels with `ImGuiHarness::CountFallbackGlyphs`.

### Changes go through commands

- **Every change to the scene or project goes through a command** (`EditorCommandRegistry::Execute`) or,
  for continuous UI edits, through `SceneEditTransaction` / `SetPropertyWithUndo`. That keeps the UI,
  automation (AI agents) and tests identical, and makes every edit undoable. Panels never write, move or remove files
  themselves (`Editor.UI.Source` scans `Panels/`): the project's files change through `asset.*` commands.

### Asset files are never lost

- **Asset files are never lost** (`Editor/AssetFileAction`, `Editor/AssetReferences`): `asset.delete` and
  `asset.deleteFolder` fail while scenes, prefabs, models, materials or the open scene (from memory, unsaved uses
  included) use what they delete, listing them in the error's data (`referencedBy`, `EditorCommandResult::ErrorData`),
  unless `force`; deleting moves the files with their `.meta` and cached imports to `<project>/.strata/Trash`
  (`EditorAssetManager::MoveToTrash`, kept 30 days) and is one undo step that restores the handle (the edited scene's: replacing
  the scene clears the history, so the toast of an agent's delete says so).
  `asset.findReferences`, `asset.move`, `asset.createFolder` and `asset.moveFolder` (handles kept) complete the set; each
  mutating one is one undo step that never marks the scene unsaved. Steps check the files before they act, since files
  change outside the history: an undo that cannot be done now fails and keeps the step applied (`EditorAction::Undo`
  returns false with the reason, which `edit.undo` reports), a redo that no longer fits (the path holds other assets, or
  the delete would be refused now) fails and leaves the history with the steps after it.

### Prefab instances

- **Prefab instances stay linked** (`Scene/PrefabInstances`, `Editor/PrefabSync`, `Editor/EditorPrefabCommands.cpp`):
  `prefab.instantiate` and `prefab.create` (unless `link: false`) link entities to their prefab (`PrefabInstanceComponent`);
  `prefab.status`, `prefab.apply` (the instance into the prefab, unmodified instances follow; one step for the file and the
  scene; not for models, not while playing), `prefab.revert` (UUIDs kept), `prefab.unpack` and `prefab.select` work on
  the instance an entity belongs to. An instance root's transform and name are the instance's own and never count as a
  change. When a prefab's file changes, `PrefabSync` gives unmodified instances of the edited scene its new values (one
  undo step); code that writes a prefab for the editor sets its baseline (`PrefabSync::SetBaseline`) so the change is not
  propagated twice. Instance roots record the version of their part they last took and a hash of their values then
  (`PrefabInstanceComponent::PrefabVersion`, `SyncedHash`): code that gives an instance its prefab's values calls
  `PrefabInstances::MarkSynced` (`Replace` does), so a prefab changed while a scene was closed updates the scene's
  unmodified instances when it opens (`PrefabSync::CatchUp`), and modified ones are outdated (`prefab.status`;
  `prefab.apply` refuses them unless `force`). Several actions that form one step go into an `EditorActionGroup`.

### Command conventions

- Commands are named `<group>.<action>`, take a JSON object and return a JSON value or an error. Each
  has a one-line description, a JSON Schema of its parameters (`CommandUtils::ObjectSchema` etc.) and a kind
  (`EditorCommandKind`: Edit, the default, for commands that change the scene, its assets or the project; Query for those
  that change nothing; Session for the editor's own state: open project, play mode, view, selection, input). Mark every
  new command that is not an edit, or replays (editor.activity.export) will repeat it, and every query whose results
  follow the editor's session rather than the project's content (the selection, the journal, what is loaded or running:
  `EditorCommandResults::Session`), or replays will bind IDs a copy of the project does not give;
  automation exposes them as tools, so descriptions must tell an agent what the command does. Use
  `CommandArguments` to read parameters (no exceptions), reject invalid input without side effects
  (roll back the transaction) and record exactly one undo step per successful mutating command.
  Failures carry an `EditorCommandError` kind: a request the command cannot accept (unknown, missing or
  mistyped parameter, a reference to nothing) is `InvalidParameters` (`CommandArguments::Fail`,
  `EditorCommandResult::InvalidParameters`); a valid request that cannot be carried out in the current
  state is `Failed` (`EditorCommandResult::Fail`). Automation reports them with different error codes.

### No silent data loss

- **No silent data loss:** every command that replaces the edited scene (`scene.open`, `scene.new`, `project.open`,
  `project.create`, `project.close`, `project.openSample`, `editor.quit`) starts with the shared guard
  `CommandUtils::CheckUnsavedChanges`: while the edited scene has unsaved changes (also while a copy of it plays) it fails
  (`Failed`, naming the scene and how to proceed) and changes nothing, unless `force: true`; results report
  `discardedChanges`. A new command that replaces the scene uses the same guard. The UI keeps asking (the unsaved-changes
  dialog, `EditorShell::RequestDiscardChanges`) and passes `force` once the person has answered, for the changes they were
  asked about only: the question's Don't Save asks again when the scene changed while it waited, a dialog that runs the
  command later asks again when the scene changed since (`UndoStack::GetVersion`), a modal dialog asks over itself
  (`EditorShell::DrawDiscardQuestion` between its `BeginModal` and `EndModal`), and a recorded `force` is never repeated
  for the person (the Activity panel's Re-run asks).

### Autosave and recovery

- **Autosave and recovery** (`Editor/SceneRecovery`, Docs/Architecture.md "Autosave and recovery"): the edited scene's
  unsaved changes are journaled in `<project>/.strata/Recovery` by following the undo stack (`UndoStackObserver`): the
  entity states of the steps since the last save, written 2 s after edits stop (at least every 10 s; the text worked out
  on a background worker, the file written on an I/O job), removed
  by a save. Replacing a modified scene in any way (`EditorContext::ResetScene`) writes them first, marked discarded, offers
  them at once (the directory is scanned again) and tells the person who discarded them; scans keep the newest
  `SceneRecovery::c_MaxOfferedDiscards` discards on offer and move older ones to `Recovery/Discarded` (never what a crash
  left). Opening a project offers what a crashed or discarding editor left (the Recover Unsaved Changes dialog,
  `scene.recoveryStatus`, `scene.recover {scene, action: "apply" | "discard", force}`: one undo step back to the saved
  scene). Entries find their scene by its handle, so a moved or renamed scene keeps its changes recoverable, and the
  journal follows the edited scene when it moves (`EditorContext::FollowSceneMoves`); the merges of a drag replace the
  states its step left waiting for the next write (`UndoStackObserver::OnApplied`'s `merged`). An undo action that
  changes the scene describes its effect as entity states (`GetSceneChange`,
  `GetEntityStates`, shared and immutable; an `EditorActionGroup` passes on those of its one member that changes the
  scene); one that cannot (kind Other) makes the journal snapshot the whole scene, which
  it does only up to `SceneRecovery::c_MaxSnapshotEntities`. Never write recovery files from anywhere else.

### Undo

- Undo works on entity snapshots: a `SceneEditTransaction` captures the entities an edit touches
  (`Track`, `TrackSubtree` before changing or deleting them, `TrackCreated` after creating them) and
  `EditorContext::CommitEdit` records the difference. Edits while playing are not recorded. Continuous edits merge
  into one step (`EditorAction::MergeWith`); a merged step that ends where it started (`IsNoOp`) is dropped. Capturing
  reads cached sibling positions and applying a step removes and places entities in batches (`Scene::DestroyEntities`,
  `Scene::PlaceEntities`), so edits of many entities in long sibling lists stay linear. Commands that act on several
  entities take `entities` (besides `entity` where one was taken before) and record one step: `entity.setParent {entities,
  parent, index}` (index: the final position of the first; reorders siblings and top-level entities too),
  `entity.duplicate {entities}`, `entity.copy {entities}` and `entity.paste {data, parent, index}` (fresh IDs, references
  among the pasted entities remapped, no prefab links added), `entity.setActive {entities}`, `component.add {entities}`
  (where the component is missing), `component.remove {entities}` (every listed entity must have the component, or
  nothing is removed) and `component.set {entities}`. Copies, pastes and prefab instances get names unique among their
  siblings ("Crate (1)", `SceneEdit::MakeNameUnique`, through `Scene::HasChildNamed`: the siblings or the name index,
  whichever are fewer), and the steps of commands
  say what they changed ("Create 'Wall'", "Delete 3 entities", "Set Mesh of 'Ground'": `SceneEdit::QuoteName`,
  `DescribeEntities`), which `edit.history` and the Activity panel show. The inspector edits a
  multi-selection through `PropertyEdit` (`GetSharedProperty`, multi-entity `SetPropertyWithUndo`, `RotationAnglesEdit`
  for rotations edited as Euler angles, `GetDefaultComponentValues` for Reset).

### Exporting games

- `project.export` writes a playable game outside the project in the background (`GameExport`; the command answers when
  it finished, so command scripts that export need frames: `--quit-after-commands`): the asset pack (`<Game>.stpak`,
  ordered by first use; `prune` leaves out what neither the scenes nor `Export.AlwaysInclude` use and lists it in
  `excludedAssets`), the script module, the manifest (`<Game>.stgame`, start scene, script module and window settings)
  and the runtime executable renamed after the game. It checks what it can before it starts (a usable directory outside
  the project, among others) and fails at once otherwise; the editor stays editable meanwhile, and a document saved
  during the export that references what its plan did not hold fails it, naming the document (export again).
  `project.exportStatus` reports its progress and stage, `project.cancelExport` stops it, `project.setExportSettings
  {alwaysInclude, loadingScene}` names the assets and folders scripts load by path and the scene the game plays while its
  start scene loads (saved in the project file; not the start scene, which the game would start twice: refused, and
  `project.setStartScene` of the loading scene clears it), and closing the project cancels a running export. The UI is the Export
  dialog (`UI/ExportDialog`: File > Export Game..., the toolbar's Export button), which also picks the loading scene and
  shows and cancels exports that commands and agents started. CTest exports a small game
  (`StrataEditor --no-gpu`) and runs it headless; `StrataEditor.Tetris` also exports Tetris pruned and runs it.

### Play mode and scene switches

- Play mode runs on the wall clock with time dilation (`Scene::SetTimeDilation`, `EditorContextSpecification::TimeDilation`;
  exported games too, `GameRuntimeOptions::TimeDilation`): when fixed steps cannot keep up with real time the game slows
  down instead of running ever more catch-up steps per frame. Scripted runs (`--frames`, `--commands`) and tests keep every
  due step, so they stay deterministic.
- A running game asks its owner to quit or to switch scenes through `Scene::RequestQuit`/`RequestSceneLoad` (scripts: the
  SDK's `Game`), honored after each update: the editor stops play mode, or loads the scene while the running one goes on
  and replaces it once the scene and everything it uses are in (`SceneLoader`; a paused game stays paused, with its pending
  steps; `play.stop` still returns to the edited scene); `GameRuntime` ends the game (`GetQuitRequest`; StrataRuntime exits
  with the code) or loads the scene from the pack the same way. A null handle restarts the running scene. Asking again for
  the scene that is loading keeps its load (scripts may ask in every update); a request for another scene replaces it.
  Scene loads never stop a frame: never load scenes with `LoadAssetSync` in the editor or games; go through `SceneLoader`
  (Docs/Architecture.md, "Scene runtime lifecycle"), and let go of a scene a switch replaces with
  `Scene::ReleaseInBackground` (stopped on the main thread, destroyed by a background job). Owners that render the scenes
  they load build their render proxies over frames before the swap (`SceneLoadOptions::Prepare` with
  `RenderScene::PrepareLoadedScene`, state `Preparing`: the editor with a GPU, `GameRuntimeOptions::PrepareRenderScenes`
  for StrataRuntime with a window), so the frame that shows a scene builds none.

### Opening scenes

- **Opening scenes** (`scene.open`, `EditorContext::BeginOpenScene`) loads in the background and answers once the scene
  replaced the edited one; meanwhile the edited scene is read-only (`CommitEdit` rolls edits back; mutating scene commands,
  undo, settings, script and property edits fail through `EditorContext::CheckSceneEditable`, which code editing the scene
  outside `Finish`/`CommitEdit` checks first; play mode is refused; the Hierarchy and the Inspector draw their contents
  disabled under a banner, `DrawSceneLoadBanner`, and the gizmo hides) and the viewport shows the load's progress
  (`ViewportPanel::DrawSceneLoadOverlay`).
  `scene.loadStatus` and `editor.status` (`sceneLoad`) report it (`scene.loadStatus` also the running game's sub-scenes,
  which `scene.loadAdditive {scene}` and `scene.unloadAdditive {subScene}` load and unload in play mode, see Scripting;
  `scene.loadStatus {wait: true}` answers once no scene is being opened); `EditorContext::OpenScene` waits for it (tools,
  tests). Opening a project (`project.open`, `project.openSample`, `EditorContext::BeginOpenProject`) opens the project at
  once and its start scene the same way (`EditorSceneLoadStatus::StartScene`): the commands answer once it is open
  (`startSceneOpened`, `startSceneError`), and a window started with `--project` shows the editor while it loads
  (`EditorOptions::OpenStartSceneInBackground`; scripted runs, headless editors and tests open it before the first frame,
  `EditorContext::OpenProject`). UI code opens scenes and projects through the runner (`RunEditorCommandDeferred`,
  `StartEditorCommand`).

### Scripts through commands

- **Scripts through commands** (see Scripting, "Project scripts"): `script.status` (loaded module, classes with fields
  and callbacks, faults with their location and stack, the running and last build with diagnostics and its build
  directory, whether scenes use scripts, why Play would build first, the external editor), `script.build {wait}`
  (deferred; fails with the first compiler errors), `script.reload`, `script.load {path}`, `script.init {example}`,
  `script.create {class, path?, template?: behaviour | empty, fields?, open?}` (a new class in the script directory),
  `script.list` (the script directory's code and CMake files with the classes each registers: `Editor/ScriptSources`,
  `ListScriptSources`, without hidden folders and build trees), `script.open {file?, line?, column?}` (the external
  editor at a line: code, CMake and text files of the project or the engine only; without file the script directory),
  `script.setExternalEditor {command}` (`Editor/ExternalEditor`:
  a template such as `code -g {file}:{line}:{column}`, split into a program and arguments and started without a shell;
  the program is a path or a name found in PATH, `Platform::FindProgram`, and one that is not there is refused; batch
  files are refused, except VS Code's launcher in PATH, which starts `Code.exe` beside it; the user's setting in
  `<user data>/Strata/EditorSettings.json`, written private and read
  only when trusted, `EditorContextSpecification::EditorSettingsFile`; `STRATA_EXTERNAL_EDITOR` overrides it; projects
  cannot name one, as a shared project must not choose what runs; without one, the default application), `script.add
  {entity, class, fields}`, `script.remove {entity, class}`, `script.setField {entity, class, field, value}` (null
  resets to the default) and `script.get {entity, class?}` (the field values: the live instances' while playing, which
  is how tools read game state kept in fields; the stored overrides over the class defaults otherwise). Adding scripts
  and setting fields need a loaded module, which validates class and field names and types; removing works without
  one. Script edits are undoable (`ScriptEdit`, shared with the inspector's Script drawer) and reach the live
  instances while playing. The UI builds with Scripts > Build Scripts (Ctrl+B) or the toolbar, and has Scripts > New
  Script... (`UI/ScriptDialogs`, also `EditorShell::ShowNewScriptDialog` for panels; it starts a build, as the Inspector
  offers the loaded module's classes), Open Scripts Folder, External Editor... (the program a command starts, and the
  editors found on the machine, `ExternalEditor::FindChoices`) and Show Problems. `UI/ScriptBrowser` is the view of the
  script sources a panel draws (the Content Browser's Scripts root, below the asset folders, with New Script... also in the
  browser's New menu): the files with their classes, listed on an I/O thread when it
  is drawn and every second while it is, never while it is not (`ScriptSourceList`, whose version changes only with the
  files); its search takes every word in a path or a class name, a double click opens a file in the external editor, and
  `DrawActions` offers New Script... and Open in IDE (`CalcActionsWidth` to place them). The **Problems panel**
  (`Panels/ProblemsPanel`, docked with the Console) lists the last build's diagnostics and the crash with its stack
  frames (without the SDK's internal `Strata::Detail` thunks, which `script.status` keeps); a double click opens one
  (`EditorContext::OpenScriptSource`); the status bar's "Build failed" and "Scripts crashed" pills open it, the
  "Building to play" pill cancels the build (`EditorContext::CancelScriptBuild`), and a project whose scenes use no
  script shows "Scripts unused" instead of asking for a build. The Console opens the
  source of messages that name one (`FindSourceReference`). People's guide: Docs/Manual/Scripting.md.

### Deferred commands and command scripts

- Commands never block a frame. One that has to wait (frames, a build, an import, a GPU readback) returns
  `EditorCommandResult::Defer(poll)`; `EditorCommandRunner` polls it once per frame, starting with the
  next frame, and reports through a completion callback. Command scripts, automation and UI actions that
  can take frames (e.g. Build Scripts) run commands through the runner; UI actions that finish at once
  call the registry through `RunEditorCommand`, which rejects pending results.
  Poll functions own their data (copy parameters, never capture them by reference) and allocate nothing per frame (frame
  times go into a `CommandUtils::FrameTimeRecorder`, reserved when the command starts: an allocation in a frame can wait
  milliseconds for the heap while a scene is freed in the background). `editor.wait
  {frames}` returns after that many frames, e.g. to let a playing scene run, with `frameTimes` (count, median, mean, 95th
  percentile and maximum in milliseconds) of the frames that ran entirely while it waited: the CPU time of each frame
  (`Application::GetLastFrameWorkTime`, recorded by `EditorLayer` through `EditorContext::RecordFrameTime`), without the
  waits for the GPU and the display, so command scripts can `expect` frame budgets. `editor.perf {frames?}` says where
  those frames went: the frame profiler's statistics (mean, nearest-rank p50 and p95, maximum) of the latest frames (120 by
  default, at most 600) for the frame, every stage (`stages/<name>`, self times, `inFrameTime` false for Present), the
  stages added up (`stageSum`, at most the frame), the rest of each frame (`untimed`) and the job threads (busy percent,
  running and queued jobs).
- `StrataEditor --commands script.json` runs a JSON array of `{"command", "parameters", "expect", "bind"}` at startup
  (`EditorCommandScript`); a pending command holds the script until it completes. `expect` (optional) maps JSON
  pointers into the command's result to conditions, e.g. `{"/values/Translation/1": {"min": 1.6, "max": 1.7}}` or
  `{"/state": {"equals": "Play"}}`; a result that does not meet them fails the step. `bind` (optional) names values of the
  result by JSON pointer (`{"crate": "/id"}`), which later parameters use as `{"$ref": "crate"}` (optionally with a
  `"pointer"` into the bound value): scripts then work with the fresh IDs every run gives created entities and assets. A
  reference to a name no earlier step binds fails the script before it starts; a step whose references have no value
  (their step failed) fails without running. If a command or an expectation fails, or the script has not finished by the
  last of `--frames N` frames or by `editor.quit`, the process exit code becomes 1. `editor.activity.export {path, since}`
  writes what a session did after a journal entry as such a script (`CommandJournal::BuildReplayScript`: the edits, also
  those made while playing that outlast play mode (assets, `scene.save`) but not edits of the running copy
  (`EditorCommandResult::RunningCopy`), with
  the IDs results gave bound and referenced, and the queries those IDs came from), to repeat it on a copy of the project as
  it was then, e.g. as a regression script; continuous UI edits (inspector, gizmos, material editor) record undo steps
  without a command, so the export counts them (`editsOutsideCommands`) instead (the UI's undo and redo run their
  commands).
  `--frames N` stops after N frames (without saving the panel layout), `--screenshot out.png` captures
  the last frame (viewport included; with `--quit-after-commands`, the frame the script ended on; a screenshot that
  cannot be saved fails the run), `--no-gpu` runs headless without a graphics device (export, asset processing),
  and `--quit-after-commands` closes the editor once the command script finished (for scripts of unknown length, e.g.
  with `script.build`, whose duration no frame budget can bound). `--help` and `--version` print and exit 0 before
  anything starts (no log file, window or session); an argument the editor does not know, or an option without its value,
  exits 3 with the usage, and so does a value an option does not take (`--frames abc`, `--frames 0`: `CheckValues`; the
  table of arguments is `c_Arguments` in `EditorApplication.cpp`: add new ones there). `--ui-scale <factor>` draws the
  UI at a fixed scale instead of the display's (e.g. 1 to check it at 100% on a 150% display), `--layout <file>` keeps
  the panel layout in that file instead of the user's (`<user data>/Strata/EditorLayout.ini`), and `--imgui-demo` offers
  ImGui's demo window (Help menu) for UI work.
  Without `--frames`, a headless editor runs until `editor.quit` (which, like the other commands that replace the scene,
  refuses to discard unsaved scene changes unless `force` is true), Ctrl+C in its terminal or a termination signal (both
  close it in order, exit code 130: see "Windows subsystem" under Building); headless editors run at most 60 frames
  per second.
  The editor serves automation by default (`EditorAutomation`, see [Automation](#automation-editor-rpc--mcp));
  `--no-automation` turns it off and `--automation-port <port>` picks the port (default 0: a free one).
  A headless editor without `--frames` whose automation cannot start exits with code 1. CTest runs of
  command scripts pass `--no-automation`.
  CTest runs `StrataTests/Editor/SmokeCommands.json.in` (configured into the build tree; it creates a project in the
  build tree, builds a scene and captures the viewport into `build/<preset>/StrataTests/SmokeCaptures/`), checks that a
  windowed editor without a project opens into the launcher (`Editor/LauncherCommands.json`) and that failing and
  unfinished scripts fail the process. `StrataEditor.Replay` (`Editor/ReplaySession.cmake`) runs a session with bindings
  on a new project, exports it with `editor.activity.export`, runs the export on a copy made before the session and
  compares the scenes and assets (`StrataTests --strata-test-helper=compare-documents`: assets by their paths, other IDs
  numbered by appearance).

### Hierarchy, Content Browser and thumbnails

- The hierarchy panel draws the rows of `Editor/HierarchyView` (the visible entities as a flat list; a filter with
  `c:`/`t:` terms shows matches with their ancestors), which are rebuilt only when the hierarchy, the expansion or the
  filter changed: never walk the scene per frame in UI code, and draw long lists through `ImGuiListClipper` (only the
  rows in view). The rows sit in one `ImGui::BeginMultiSelect` scope whose requests set `EditorContext::SetSelection`
  (rows identify themselves by their entity's handle, never by row index, which changes as rows appear; while filtering,
  Ctrl+A, ranges and boxes take the matches only); drops map to `entity.setParent {entities, parent, index}` through
  `HierarchyView::GetDropTarget`; F2 or a double click renames in place.
- The Content Browser (`Panels/ContentBrowserPanel`) draws `Editor/AssetBrowserView`'s rows (folder tree, grid or list,
  clipped), rebuilt only when `EditorAssetManager::GetChangeVersion` changed: never list the asset directory per frame.
  Its drops, renames, folders and deletes run `asset.*` commands; inspecting an asset elsewhere reveals it there.
- **Thumbnails** (`Editor/ThumbnailCache`, `EditorContext::GetThumbnails`, null without a GPU): ask for an asset's
  thumbnail every frame it is on screen (`Request`) and draw the type's icon until it is ready. Textures come from their
  mip tail by byte range (never a load), materials, meshes, models and prefabs are rendered one at a time offscreen and
  read back without stalling, once the textures they draw streamed in the levels the render needs (a bounded wait,
  `MaxMipWaitFrames`); PNG files in `<project>/.strata/Thumbnails`, keyed by the content stamps of the asset and
  its dependencies (`EditorAssetManager::GetContentStamp`), carry them across sessions. Bump `ThumbnailCache::c_Version`
  when thumbnails would look different. The Asset tab shows a preview rendered at the pixels it covers
  (`RequestPreview`; the thumbnail stands in until it is ready), a model's summary and an audio clip's waveform with Play and Stop on a
  preview voice that belongs to no scene (`Editor/AudioPreview`).

### Inspectors

- The inspectors show the selected entities or one asset (`EditorContext::SetInspectedAsset`, `selection.set {asset}`;
  the entities stay selected). Selecting an entity, deleting the asset or closing the project returns to the entities;
  `GetInspectionVersion` changes with every switch and every asset inspected (again), and the editor brings the
  matching tab (Inspector or Asset) forward without taking the keyboard focus. The Inspector edits every selected entity
  at once (shared components only; values that differ show as mixed; one undo step per edit), offers per component
  Reset, Copy Values and Paste Values (the values component.set can set; not for components without such properties)
  and Remove (not for components the engine adds), and shows the scene's settings (gravity, the fixed step, the physics
  world's capacities and the collision layers: names and a compact matrix, long names elided; bodies and controllers
  choose their layer among layer 0 and the named ones; how many entities sub-scenes create per frame) and sky with
  nothing selected, in sections that start closed with a summary on their headers
  (`UI::SectionHeader`'s summary: live widgets cost frame time while they are drawn); the
  Asset tab edits materials (`UI/MaterialEditor`: a drag previews, its release is one undo step) and import settings
  (Apply runs `asset.setImportSettings` through the runner; why the last import failed, and its warnings, show above
  them) and previews textures as their content looks, with the
  stored format and GPU size: draw textures in UI code through `UI::TextureImage`, which shows sRGB, grayscale and
  two-channel normal map layouts as materials read them (`ImGuiTextureDisplay`). Property widgets come from
  `UI/PropertyWidgets.h` (`DrawPropertyWidget` with `PropertyWidgetOptions` for mixed values, axis resets and probe keys).

### Shortcuts and editor status

- **Shortcuts:** `EditorLayer` handles the global ones (Ctrl+Z, Ctrl+Y, Ctrl+S, Ctrl+Shift+S, Ctrl+P, Ctrl+B).
  Shortcuts that act on the selection (Delete, Ctrl+D, Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+A, F2, End) are
  `ImGuiInputFlags_RouteFocused` shortcuts of the Hierarchy and the Viewport (`UI/EditorActions.h`,
  `HandleSelectionShortcuts`): a key pressed in another panel never edits the scene. The Edit menu runs the same actions.
- `editor.status` summarizes the editor (project, scene, play state, selection, undo history); other
  parts of the editor add sections to it through `EditorContext::SetStatusProvider`: `automation`, `assets` (the
  totals of `asset.stats`), `game` (the window requests of the running game, `null` without any, its cursor mode and its
  save directory) and `editor`: the window, UI scale and frame rate, whether the launcher shows (`launcher`),
  the GPU (`gpu`) and the startup time (`startupSeconds`: from the process's creation to its first frame on screen,
  `Platform::GetProcessUptime`, also logged as "Started in ... s").

### Asset streaming commands

- **Asset streaming commands** (`Editor/EditorStreamingCommands.cpp`): `asset.stats {assets?, limit?, sort?: "bytes" |
  "lastUsed"}` reports the active asset manager's `GetStats` (resident bytes and budget per pool, the load queue, bytes
  in flight, uploads, finalization time and the staging budget, evictions and the assets eviction looked at,
  cancellations, staging releases, dependency prefetches started and dropped, and `mipStreaming`: partial loads, range
  reads, mip levels raised and dropped, bytes read for them, textures below their wanted level) and, with `assets: true`,
  the assets that are resident, loading, failed or pinned (`GetResidencyInfo`; textures streaming their mips add
  `residentMip`, `wantedMip`, `mipCount` and `residentBytes`); `asset.setBudget {gpuTexturesMB?, gpuBuffersMB?, cpuMB?,
  inFlightMB?, uploadMBPerFrame?, finalizeMsPerFrame?, uploadMsPerFrame?, stagingMB?, reset?}` changes the budgets of the open project's
  manager until the project closes (it fails without a project; values greater than 0; MB are 2^20 bytes; no undo, as
  nothing in the project changes).
  Opening a scene, and a scene switch the game asks for in play mode, schedule the release of what only the previous
  scene used (`EditorContext::OpenScene`, `SwitchRuntimeScene`). The Streaming panel (`Panels/StreamingPanel`) shows the
  same numbers, read once per frame after the frame's commands, and the assets as a sortable, filtered table, failed
  assets first, sub-assets named by their file and key: the main thread copies `GetResidencySamples` (and the metadata
  when `GetMetadataVersion` changed) four times a second and a job names, sorts and filters the rows; a row click reveals
  the asset in the Content Browser; its Mip column shows a streaming texture's resident and wanted level ("4 / 2", coarser
  than wanted in the warning color). Where the table is narrow it drops Pins, CPU MB, Last used and then Mip before the
  asset's name gets less than 14 text heights, and the overview stacks above the table where beside it the table would be
  too narrow even then. Its Type column is as wide as the longest type it names (`StreamingPanel::GetTypeLabel`: the
  animation clip and controller go by "Animation" and "Controller"; the row's tooltip names the type in full), so keep new
  types' labels about as short, or the laptop layouts change.
- **`world.cook`** (`Editor/EditorWorldCommands.cpp`, deferred: it reads cells off the frame) measures a world's cells into
  its index (entities, bodies, bounds, closure bytes, `MaxBodiesInRadius`), re-reading only cells whose files changed; it
  fails, writing nothing, on entity IDs cells share; `apply` also sets the persistent scene's capacity (open in edit
  mode). **`world.info`** describes a world, its layers and how many cells its index misses.
- **`physics.stats`** (`Editor/EditorWorldCommands.cpp`, a query whose results follow the session) reports the running
  game's physics (`PhysicsSystem::GetStats` and `IPhysicsCapacity`): bodies by type, awake ones, character controllers and
  the body slots in use; capacities and the megabytes they reserve; bodies waiting for a slot (a world at its growth limit)
  or for other reasons; the bodies built and destroyed and the milliseconds spent on them in the last frame; the slices
  the bodies of a scene switched to were built in while it loaded (`preparedSlices`, 0 for bodies built as the scene
  started: a play copy); growths; demand; dropped contact steps; the last step's time, the steps run and held. `{running: false}` while editing. While a
  game runs, the Streaming panel's Physics section shows the same capacity, read once per frame through
  `IPhysicsCapacity` (constant time), and leads the status bar's pill of bodies without collision.

### Edits while playing and value ranges

- Mutating commands report a `warning` in their result while the scene is playing (and set
  `EditorCommandResult::RunningCopy`, which replays read): such changes apply
  to the running copy and are discarded by `play.stop`. Unknown or missing parameters are errors, and so are values out of
  a property's range (`PropertyInfo::IsInRange`, checked by `CommandUtils::ApplyComponents` for `component.set`,
  `component.add`, `entity.create` and `prefab.instantiate`, and by `material.set`/`material.create`; scene settings name
  their ranges too): `InvalidParameters` naming the range, nothing changed. The inspector's widgets keep clamping as they
  drag (`PropertyInfo::SetValue`).

### Viewport state

- **Viewport state** lives in the core: `EditorContext::GetViewport()` (`EditorViewport`) holds the editor camera
  (`EditorCamera`: a target that is also the orbit pivot, distance, yaw/pitch in degrees, FOV, clip planes, fly speed),
  the `ViewportSettings` (grid, selection outline, light/camera/collider shapes and whether colliders show for every
  entity, stats, preview lighting, game UI, gizmo mode and space, snap steps) and two `ViewportRenderer`s (the panel's
  and the captures'), created on first use and only with a GPU. Settings and the editor camera of every scene asset
  the project showed (`"Cameras": {"<scene handle>": ...}`) are saved per project in
  `<project>/.strata/EditorViewport.json` when it closes and restored when it opens;
  files from before per-scene cameras hold one `"Camera"`, given to the first scene that opens. `EditorContext::OpenScene`
  restores the scene's camera, or frames a scene shown for the first time (`EditorViewport::FrameScene`: along the
  scene's primary camera, `EditorCamera::FitBounds` on what renders, `SceneBounds`), again once meshes that were still
  loading arrive unless the camera moved. `ResolveViewportView` picks the camera: the scene's primary camera while
  playing (the editor camera with a notice when there is none), the editor camera when editing or simulating. Views of
  the editor camera outside play mode are *editor views* (`ViewportView::EditorView`): with `PreviewLighting` (on by
  default) a scene without any light (directional, point, spot or sky) gets a preview sun and procedural sky, and the
  game's screen-space text is hidden unless `ShowGameUI` is on (`GetViewportRenderOptions`). Without a project the
  context keeps an asset manager with only the built-in assets active, so built-in meshes render.

### Templates and recent projects

- **Templates and recent projects:** `project.create {template}` and `scene.new {template}` start from
  `Editor/ProjectTemplates` (`project.templates` lists them): `empty` (the commands' default, what they always did) or
  `basic3d` (a saved, lit start scene `Scenes/Main.stscene`: Main Camera with an audio listener, Sun shining from the
  preview sun's direction, procedural Sky, Ground, Post Process; the editor UI and the skills use it). Its Ground has a
  dark stone material of the project, `Materials/Ground.stmat` (made by `EditorContext::NewScene` when the project has
  none, the built-in default material without a project), so white boxes stand out against it, and a static box collider
  whose top is the plane (y = 0), so what falls on it stays there (`Editor.Templates` drops a crate on it). Every opened
  or created project goes to the front of `RecentProjects` (`Editor/RecentProjects`, at most 12, missing projects left out,
  `editor.recentProjects`), a JSON file in the user data directory shared by the user's editors
  (`RecentProjects::GetDefaultFile`; `STRATA_RECENT_PROJECTS` overrides it); `editor.removeRecentProject {path}` takes
  one off. Scripted runs (`--frames`, `--commands`) only read it, and keep their own changes in memory over every read,
  as a list does whose save failed; `EditorContextSpecification` keeps it in memory by default (tests). Changes look on
  disk only at the project being added: the launcher reads the file and checks the projects on an I/O thread
  (`RecentProjects::ReadFile`, `CheckProjects`), takes deleted projects off the list (`Forget`) and hides those out of
  reach (a disconnected drive or share). `project.samples` lists the samples next to
  the executable (`Editor/ProjectSamples`, `EditorContextSpecification::SamplesDirectory` in tests) and
  `project.openSample {sample, directory}` copies one (without `.strata`) into a new or empty directory and opens the
  copy; `project.close` closes the project.

### Launcher

- **Launcher** (`Panels/WelcomePanel`): while no project is open the editor shows it instead of the dock space (under a
  File and Help menu bar), until a project opens or the user picks Continue without a project; closing the project
  shows it again. It offers New Project (`UI/ProjectDialogs`: template cards, name, location; the location used last is
  remembered in imgui.ini, first `<home>/StrataProjects`, which keeps script build paths short), Open Project, Open
  Sample, the recent projects as cards with each project's picture (`Editor/ProjectThumbnail`: written after every scene
  save to `<project>/.strata/Thumbnail.png`, 480x270, from the scene's camera, rendered at a game window's 1440x810 so its
  text keeps its share, once its textures have the levels the view wants, and averaged down) and a search over their names and paths
  (context menu: show in folder, copy path, remove from list), template and sample cards, "Connect an AI agent" (the exact `claude mcp add strata -- "<bin>/StrataCLI" mcp` line with a copy button, and
  the automation state, also why it failed to start) and a footer with version, commit, GPU and startup time. Its
  sections share one grid in a content column centered in the main area, so they end at one right edge. Its actions
  report failures on it; dropping a `.stproj` on the window opens that project. File > Open Recent lists the recent
  projects in the editor too, and File > Show Launcher returns to the launcher from Continue without a project. Help >
  About Strata (`UI/AboutDialog`) shows the build, GPU, driver, Vulkan version, startup time and the compiled-in
  `ThirdPartyNotices.md`.

### Placing content

- **Placing content** (`Editor/SceneRaycast`, `Editor/EditorPlacement`): new top-level entities from the create menus
  appear under the view's pivot, resting on the surface there (`EditorPlacement::FindPivotSurface`, `RestOn` with the
  mesh's bounds), with the translation sent through `entity.create`; prefabs and models rest by the bounds of what they
  draw (`GetTemplateBounds`) on the surface under the cursor when dropped into the viewport, under the pivot from Add to
  Scene and drops on the Hierarchy's empty space (`EditorActions::InstantiatePrefab`), a dropped material goes to the
  mesh under the cursor (`component.set`), and
  `entity.dropToGround {entities}` (End) moves entities down onto what is below them, one undo step. Rays hit the mesh
  renderers through the render scene's spatial index and the loaded meshes' triangles: code that needs what is under a
  point uses `SceneRaycast::Cast`, never a loop over the scene's entities.

### Viewport panel

- **Viewport panel** (`Panels/ViewportPanel`): renders into a texture of the panel's pixel size and takes input only
  while hovered or focused: Alt + left drag orbits, middle drag pans, the wheel dollies, right drag flies (WASD, Q/E
  down/up, Shift faster, wheel = speed), F frames the selection, Home everything, W/E/R/Q pick the gizmo (as do the main
  toolbar's tool buttons, next to the gizmo space and the snap toggle, `ViewportSettings::Snap`; holding Ctrl inverts
  snapping). Chips over the image's top left hold the camera settings, the overlay toggles (grid, outline, gizmos,
  stats), the editor-view settings (preview lighting, game UI) and the debug view (named after the view, "Lit" without
  one, and lit while one colors the image; a click offers Lit, Streaming, LOD and Mip); clicking a chip does not focus the
  panel. A strip in the play state's color tops the image while the scene runs. In the game view only the stats and
  debug view chips remain, and only while the game does not have the input: once the view is clicked, every click on the
  image is the game's. A debug view shows a legend at the image's bottom left (`UI::Legend`: its colors with the number
  of entities in view per color, counts aligned in a column, and the placeholder boxes). The
  selection outline is the theme's accent and the debug views use its meanings (`ViewportSettings::SelectionColor` and
  `DebugColors`, set by the UI, not saved).
  Clicks pick without blocking (`EditorViewport::RequestPick` reads one pixel of the entity-ID buffer; Ctrl toggles,
  Shift adds, empty space clears) and never when they hit the gizmo. Gizmo drags go through `TransformDrag`
  (`Editor/TransformEdit.h`): selected entities without a selected ancestor follow the primary one, local transforms
  are recomputed under their parent (`TransformEdit::WorldToLocal`), writes emit the transform's update signal so
  physics follows, and a drag is one undo step (while playing: the running scene, no undo). Playing through the scene's
  camera makes the panel the game view: editor tools are off and `Input` is enabled only while it is focused (Shift+F1
  leaves it and frees a cursor the game locked; while the game holds a locked cursor the editor's UI ignores the mouse,
  and the view says so for a few seconds after each time the game took it: `c_MouseHint`). ImGuizmo only starts a drag
  while no ImGui item is hovered, so the image is a plain `Dummy` while the mouse is over the gizmo.

### View commands

- **View commands** change no scene data and record no undo: `camera.get`, `camera.set {position, target, yaw, pitch,
  distance, fov, near, far, flySpeed}` (position + target looks from one at the other) and `camera.focus {entities?}`
  (frames them, or the whole scene). `viewport.capture {width?, height?, camera?: "editor" | "scene", position?, target?,
  fov?, overlays?, debugView?: "none" | "streaming" | "lod" | "mip", waitForAssets?, path?, overwrite?}` renders on the
  next frame (again on the following frames, up to `c_MaxCaptureTextFrames`, while glyphs of the scene's text are still
  being rasterized, so text is never half drawn, and with `waitForAssets` (default true) up to `c_MaxCaptureAssetFrames`
  while assets of the view are loading, so the first capture after opening or playing a scene shows its materials, and up
  to `c_MaxCaptureMipFrames` while textures it draws are coarser than its view wants them, as a texture counts as loaded
  with its mip tail, and up to `c_MaxCapturePoseFrames` while animators it draws show a pose older than their time), reads
  the image back without stalling, encodes it on a job thread and returns `{"Image": {"MimeType": "image/png", "Data":
  <base64>}, "width", "height", "camera", "view": {"position", "forward", "fov"}, "waitedFrames", "overlays",
  "pendingAssets", "texturesBelowWanted", "stalePoses", "pendingTextGlyphs", "previewLighting", "screenSpaceTexts", "hiddenScreenSpaceTexts", "debugView":
  {"view", "tintedEntities", "pendingEntities", "failedEntities", "placeholderBoxes", "entitiesPerLOD",
  "entitiesPerMipShortfall"}, "notice"?, "path"?}` (the mip view counts entities with textures by the mip levels their
  worst texture lacks: none, 1, 2, 3 or more or still loading); it defaults to the viewport's size, camera, overlays and
  debug view and fails without a GPU. `position`, `target` and `fov` render through a camera of the capture's own (a copy
  of the editor camera with those changes; `camera.get` is unchanged, the result's camera is `"custom"`), so agents look
  around without moving the person's view. `viewport.getSettings` and
  `viewport.setSettings {grid, selectionOutline, sceneGizmos, allColliders, stats, previewLighting, gameUI, gizmo, space,
  snap, translateSnap, rotateSnap, scaleSnap, debugView}` read and change the view settings (validated as a whole: an
  invalid value changes nothing; the debug view is saved with them); `allColliders` draws the collider shapes of every
  entity instead of only the selection's.

### Materials

- Materials: `material.create {path, properties}`, `material.set {material, properties, save}` and `material.get {material}`
  (its values, whether they are saved, and every material property described like `component.list` describes component
  properties); an unknown property name fails with the list of known ones, and texture maps take a texture by path or handle
  like component asset properties (a reference to nothing or to another type of asset is refused). Edits of asset documents go through
  `Editor/AssetEdit` (also the asset inspector's): a saved edit writes the file and publishes the new document as the
  loaded asset at once (`AssetManagerBase::ReplaceLoadedAsset`), recording an `AssetDocumentAction` (the document's bytes
  before and after; merged by key for drags); `save: false` only previews the values in the loaded material. Such steps
  do not change the scene (`EditorAction::ChangesScene`), so they never mark it unsaved.

### Animation commands

- **Animation commands** (`Editor/EditorAnimationCommands.cpp`): `animation.createController {path, parameters, states,
  transitions, defaultState}` and `animation.setController {controller, ...}` (the sections given replace the
  controller's; one undo step through `AssetEdit::SetControllerData`, an `AssetDocumentAction`), `animation.info {asset
  | entity}` (a model's skeleton and clips, a skeleton's joints, a clip's channels, a controller's graph; an animator's
  assets and, while playing, its state and parameters) and `animation.preview {entity, state?, clip?, time?, stop?}`:
  poses of the edited scene's animators through the `EditorContext`'s `AnimationPreview` (the edited scene's
  `ISkinningPoses` while it does not play; attachments are drawn at their joints through render transforms), which change
  no scene data and are dropped with the scene; the command answers once its own request's pose shows, and the viewport
  names the previews in a chip whose click stops them (`ViewportPanel::DrawPreviewChip`), so a pose is never taken for
  scene data. The Asset tab
  draws skeletons, clips and controllers (`UI/AnimationAssetViews`: controllers as editable tables, one undo step per
  edit, whose fields never get narrower than their content needs: a narrow tab scrolls the table sideways); the
  Inspector shows a playing animator's state (a cross-fade as the state it comes from and the state it goes to,
  `AnimationSystem::GetBlendSource`) and, under a heading of their own, its parameters.

### Simulated input

- **Simulated input** (`Editor/EditorInputCommands.cpp`) lets tools play a running game (play mode only) as a person
  would: `input.key {key, action: tap|press|release, frames, wait}`, `input.mouseButton {button, action, frames, wait}`,
  `input.mouseMove {position, wait}` (pixels from the game view's top-left corner), `input.scroll {delta, wait}`,
  `input.gamepadButton {gamepad, button, action, frames, wait}`, `input.gamepadAxis {gamepad, axis, value, frames, wait}`
  (a value holds until replaced, `null` or, with `frames`, for that many frames; clamped to the axis' range),
  `input.releaseAll {wait}` and `input.state`. Keys, buttons and gamepad controls are named like the `Key::`/`Mouse::`/
  `GamepadButton::`/`GamepadAxis::` constants (`InputNames`, ignoring case); a gamepad simulated input used counts as
  connected until play stops. The commands feed the engine's virtual device (`Input::SimulateKey` and friends): its
  events apply at the next input frame, are merged with device input (one source pressing a key the other holds is no
  transition) and reach the game even while the viewport has no focus. Holds belong to commands (`SimulatedInput`, owned
  by `EditorContext`): a tap ends only its own hold, overlapping holds of a button keep it down until the last ends, a
  release ends every hold of its button, and `input.releaseAll` ends all of them and drops presses still queued
  (`Input::ReleaseAllSimulated`). Frames are input frames, and those follow the game's updates: `EditorContext` suspends
  them (`Input::SetSuspended`) while the game is paused without a pending step, so input given while paused arrives in
  the next frame `play.step` runs or after resuming, with its transitions, and a tap of N frames lasts N game updates. A
  command answers once the game has seen its input (a tap: its release; `seen: true`) or, with `wait: false` (the
  default while paused, where waiting would block a client that has to step the game), at once. Holds outlive the client
  that made them (no attempt is made to tie them to connections): the status bar lists them in a pill that releases them
  when clicked (`input.releaseAll`), and starting or stopping play drops them (`Input::ClearSimulated`).

### Output paths

- Files commands write for clients go through `CommandUtils::ResolveOutputPath`: relative paths are relative to the
  project directory (an error without a project), network/device paths and reserved device names are refused, and an
  existing file is replaced only with `overwrite: true`.

## Rendering

### Scene renderer and lighting

- `SceneRenderer` draws a `Scene` from a `SceneCamera` into its output texture or a given framebuffer:
  light clustering, directional shadow cascades, depth/normal/entity-ID prepass, ground-truth ambient
  occlusion, forward PBR (opaque surfaces with an EQUAL depth test and forced early depth testing), sky,
  transparent surfaces (back to front), then post-processing from the scene's `PostProcessComponent`
  (exposure, bloom, tone mapping and grading, FXAA). A target framebuffer must match the viewport size
  and have a non-sRGB UNORM color format; `Render` returns false (and logs once) instead of rescaling. The output
  texture is created by the first frame that needs it (no target, or overlays or text to compose), so renderers that
  draw into targets without overlays (games without text) never hold one.
- Image-based lighting comes from the first active `SkyLightComponent`: an equirectangular HDR map (`EnvironmentMap`) or,
  with `Source = Procedural`, `IBL/ProceduralSky.comp` (zenith, horizon and ground colors as linear radiance, and the
  sun's disk where the directional light the scene is lit by comes from, 45 degrees up without one). Either source only
  writes mip 0 of the environment cube; the mips, irradiance and prefiltered maps are computed by the same chain
  (`SceneRenderer::FilterEnvironment`). A procedural cube is computed again only when its parameters
  (`RenderData::ProceduralSkyParameters`, compared exactly) change, into the same textures, and
  `SceneRendererStats::EnvironmentUpdates` counts it; `Rotation` applies to maps only.
- `SceneRenderOptions` also shapes editor views: `PreviewEnvironment` lights a scene without any active light
  (`SceneRenderer::NeedsPreviewLighting`: no directional, point, spot or sky light, so a night scene lit by a torch
  stays dark) with a preview sun (`SceneRenderer::GetPreviewSunDirection`) and the default procedural sky
  (`SceneRendererStats::PreviewLighting`); `DrawScreenSpaceText = false` leaves the HUD out (`ScreenSpaceTexts`,
  `HiddenScreenSpaceTexts`). The defaults render a scene exactly as a game shows it.
- Light units are relative but physically consistent: directional intensity acts like illuminance,
  point and spot intensity like luminous intensity with inverse-square falloff (cut off at `Range`).
  Automatic exposure maps the average luminance of the non-black pixels to middle gray; tests that
  compare exact colors disable it (see `AddNeutralPostProcess` in `SceneRendererTestUtils.h`).

### Render scene

- **Render scene** (Docs/Architecture.md, "Render scene"): what a frame draws comes from the scene's `RenderScene`, one
  per scene (in its registry's context, shared by every renderer of it): render proxies of the mesh renderers, updated
  only where components were signaled or transforms changed, in a `SpatialIndex` (loose grid) that the view frustum, the
  prefetch sphere and the shadow cascades query. Only meshes and materials of proxies in view are requested (High above
  5% of the viewport height, Normal below), those within `SceneRendererSpecification::PrefetchDistance` at Low; each
  asset is looked up once per frame (`AssetResolveCache`), and the cache's references end with the frame (no mesh,
  material or texture it resolved, and not the asset manager, is held once `Render` returns), so what leaves the view
  becomes evictable, also for a renderer that stops rendering. A frame that uses the same material slots the same ways
  as the one before (still frames, and views that move over the same materials) takes their data over when the materials
  and textures are the loaded objects that frame found and the textures' bindless slots still hold them
  (`SceneRenderer::ReuseResolvedMaterials`, `BindlessTextureTable::AreSlotsUnchanged`;
  `SceneRendererStats::ReusedMaterials`), looking them up again in one call (`AssetResolveCache::Replay`,
  `AssetManagerBase::TouchLoadedAssets`). Loaded materials are never changed in place: an edit publishes a new object
  (`ReplaceLoadedAsset`). Two caches keep assets across frames: `SceneRenderer`'s environment cache holds the sky
  light's environment map until the environment changes (the maps computed from a map are shared by every renderer
  lighting with it, so a renderer that starts later, such as a capture renderer, takes them over after the map dropped
  to its mip tail; until one has computed them it holds the map whole in the streamer, `TextureStreamer::Hold`, also
  through frames it does not draw), and `TextRenderer`'s glyph atlases hold their fonts (an atlas lives as long as its
  font is loaded). Mesh layouts (bounds, submeshes) follow the asset manager's content changes in `RenderScene::Sync`,
  whoever renders the scene; an evicted mesh keeps its layout until a reimport changes its source revision
  (`AssetManagerBase::GetSourceRevisions`). Until a mesh's layout is known, the bounds the asset pipeline recorded for
  it place it (`AssetManagerBase::GetBounds`: import records, a pack's bounds table; read through
  `RenderScene::ResolveUnloadedBounds`, and a change of them is a content change), so imported and packed meshes are
  culled before they load. A mesh whose bounds are unknown (not loaded and no bounds recorded:
  `RenderScene::GetMeshesWithoutBounds`) is requested as if in view while any of its entities is active, since its
  proxies' placeholder bounds say nothing about where it is. Never resolve assets per entity in renderer code: go
  through the proxies and the cache.

### Batching

- Batching is material-free: instances of a mesh, submesh and level of detail with the same pipeline state share one
  draw whatever their materials (shaders read `u_Materials[instance.MaterialIndex]`); transparent surfaces are drawn
  one by one, back to front. Draws are gathered in parallel chunks merged in chunk order (results do not depend on
  the threads) and sorted with `RadixSort` (`Core/RadixSort.h`). `RenderData::InstanceData` (64 bytes: a 3x4 world
  matrix, the material slot, the entity ID, flags) persists in a buffer indexed through the frame's draw list
  (`u_DrawList[gl_InstanceIndex]`); only changed instances, materials and draw list ranges are uploaded, so a still
  frame uploads nothing (`SceneRendererStats::InstanceBytesUploaded`, `ProxyUpdates`). Shaders derive the normal
  matrix from the 3x4 rows (`TransformNormal` in `Include/Scene.glsl`).

### Skinned meshes

- **Skinned meshes** (Docs/Architecture.md, "Render scene"): a mesh renderer with a `SkinComponent` is posed by the
  animator it names through the scene service `ISkinningPoses` (`Scene/SceneServices.h`; the renderer never includes the
  Animation module). `RenderScene::Sync` computes posed proxies' palettes (per skin joint, 3x4) and posed bounds only when
  a pose, the mesh's skin binding or a placement changed; renderers upload only the changed joints, and posed instances
  (`c_InstanceSkinned`, `InstanceData::SkinPalette`) draw with the `ST_SKINNED` variants of `Mesh.vert` and
  `ShadowMesh.vert`, which every pass of a skinned mesh uses (the prepass and the forward pass must agree to the bit).
  Unskinned meshes keep their pipelines, data and uploads: anything that adds skinning work must cost unskinned scenes
  nothing (`RenderScene::HasSkinProxies`). Physics' mesh colliders use the bind pose; the entity-ID buffer (picking,
  outlines) shows the pose. Every skin influence must name a joint of the skin, also one without weight (the shaders read
  all four palette entries). A poses provider that may not change the scene (the editor's pose preview) draws
  attachments at their joints through render transforms (`ISkinningPoses::GetRenderTransforms`), never by writing
  transforms.

### Lights, shadows and HDR

- Point and spot lights are frustum culled on the CPU, ranked by relevance (projected size of their
  range) and capped at `MaxLights`; `Scene/LightClusters.comp` then lists the lights reaching each
  cluster of a 16x9x24 froxel grid (exponential depth slices) and `Forward.frag` loops only over its
  pixel's cluster. Only the directional light casts shadows.
- Shadows: cascade depth ranges are fitted to the casters toward the light (casters beyond
  `c_MaxShadowCasterDistance` are pancaked onto the near plane); acne is handled by the rasterizer
  depth bias, the light's depth and normal offsets and a receiver plane depth bias in the PCSS filter.
- HDR targets are RGBA16F: shaders writing them clamp with `SanitizeHDR` (Inf and NaN would turn
  black after tone mapping). Instances with a mirroring transform (negative determinant) use
  pipelines with clockwise front faces and flip their tangent handedness (`c_InstanceMirrored`). The ambient occlusion
  targets are R16F where the device stores the extended storage image formats
  (`GraphicsDeviceInfo::SupportsExtendedStorageFormats`, the `*Half.comp` variants of `GTAO` and `AOBlur`), else R32F.

### Streaming and uploads

- Everything streams: meshes, materials and textures that are still loading are skipped or drawn with
  fallbacks and counted in `SceneRendererStats::PendingAssets`; never block a frame on an asset. Asset uploads are
  budgeted per frame and go in steps (Docs/Architecture.md, "Streaming and residency") through one queue
  (`Renderer::GetUploadQueue()`, `GPUUploadQueue`: steps sized by `PlanStep` to the bytes and time left in the frame, its
  counters and completion tickets); texture steps and mesh ranges go through the staging buffers of
  `Renderer::GetStagingPool()` (`StagingPool`, all of it within the staging budget; texture levels are copied from them by
  `GraphicsDevice::RecordBufferToTextureCopies`, whose command lists do not keep the textures alive: the queue keeps them
  until its batch completed), reusable once the GPU has run the copies: the queue fences the
  managers' uploads when it submits a batch (`GPUUploadQueue::SubmitBatch`; every command list with copies from staging
  released before must have been executed by then) and `Renderer::BeginFrame` fences what else was released, so staging
  other code releases is reused only with device frames (without them, such staging stays in flight). Code that renders
  into the back buffer calls `Renderer::PrepareBackBuffer()` first (the application and the runtime do): the GPU waits
  for the display's image only from there on, so the frame's uploads never queue behind the display.

### Material texture slots

- Material texture slots (`SampleMaterialTexture` in `Include/Scene.glsl`) pack the bindless slot, the sampler index and a
  grayscale bit (`RenderData::c_TextureSlotGrayscale`, from `TextureSpecification::Grayscale`). Normal maps are read as X
  and Y with Z reconstructed (`Forward.frag`), so BC5 and RGBA8 normal maps shade alike; a shader that reads a normal map
  must do the same.

### Overlays

- Overlays (`SceneRenderOptions`, all off by default) are drawn after post-processing with exact display
  colors into the output texture (then copied into an external target): the infinite ground grid, the
  selection outline (from the entity-ID buffer, so alpha-blended surfaces get none) and `DebugDraw` line
  lists, depth-tested against the scene or always on top. Fill a `DebugDraw` each frame (gameplay
  debugging, or `DrawSceneGizmos` for light, camera and collider shapes) and pass it to `Render`. Give
  `DrawSceneGizmos` the view (`SceneGizmoSettings::View`): it then leaves out shapes outside it or beyond `MaxDistance`
  and draws lights and cameras whose shapes would be smaller than `MinShapePixels` on screen as icons of `IconPixels`,
  so thousands of lights cost what is near and in view; colliders can be limited to a list (the editor: the selection,
  unless `ViewportSettings::AllColliders`), and so can the range spheres and cones of lights (`AllLightRanges`,
  `LightRangeEntities`; the editor shows the selection's only, the others as icons). Entity tints (`EntityTints`, `Overlay/StatusTint.frag`) come first: listed
  entities' pixels in the entity-ID buffer are blended with their color and no other pixel changes (debug views such as
  the editor's streaming, LOD and mip views). Code that needs the level of detail the renderer draws uses `LODSelection`
  (`Renderer/LODSelection.h`), the functions `SceneRenderer` calls.

### Text and fonts

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

### Readback, the game renderer and tests

- Reading GPU data back: `TextureReadback` copies a texture region and reports when the GPU is done (poll `IsReady`
  once per frame, never wait in a frame); `Renderer::ReadTexture` is the blocking form for tests and tools. Picking
  uses `SceneRenderer::ReadEntityIDAsync` and `GetEntityFromID`; a renderer made with
  `SceneRendererSpecification::EntityIDs` off writes no entity-ID buffer (it picks nothing, and leaves selection
  outlines and entity tints out with an error).
- `GameRenderer` (`Runtime/`) draws a running game into the window's back buffer from the scene's primary camera, or a
  message frame naming the problem when the scene has none; it writes no entity-ID buffer (games do not pick by pixel).
- GPU tests of the scene renderer share `StrataTests/src/Renderer/SceneRendererTestUtils.h`. Verify that a
  new regression test fails without its fix before relying on it.

## Automation (editor RPC + MCP)

The editor exposes its features to tools and AI agents through `RpcServer` (`Strata/src/Strata/Network/`):
JSON-RPC 2.0, one compact JSON message per line, over TCP on loopback. `StrataCLI` is the client (`call`,
`list`, `status`, `launch`) and an MCP server on stdio (`StrataCLI mcp`). How an agent drives the editor is
described in `.claude/skills/strata-editor-automation/SKILL.md`.

### Commands and requests

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

### Activity journal

- **Activity journal** (`Editor/CommandJournal`, shown by the Activity panel, `Panels/ActivityPanel`: filters, the selected
  entry's JSON, Copy as script, Re-run, Undo to here): every command `EditorCommandRegistry::Execute` runs is recorded once
  with its origin (`CommandOrigin`), parameters, outcome, duration and undo step; a deferred one when its result becomes
  final. Pass `CommandOrigin { CommandSource::UI }` when UI code runs a command (`RunEditorCommand` and the panels do);
  automation passes the client's name, command scripts their source, and code that passes nothing counts as the editor
  itself (Internal). `editor.activity` reads it and `editor.activity.export` turns it into a command script (see the
  command scripts above). Clients name themselves in `rpc.handshake` (`clientName`, optional, at
  most 64 characters, sanitized, never trusted): `RpcClient::Connect`'s `clientName`, "StrataCLI", and the MCP client's
  `clientInfo.name` through `StrataCLI mcp`; `editor.status` lists them (`automation.connectedClients`).

### Errors

- **Errors** map from `EditorCommandError`: unknown command -32601 (`MethodNotFound`), `InvalidParameters`
  -32602 (`data`: `{"command", "parameters": <schema>}`), `Failed` -32005 (`OperationFailed`), `Cancelled`
  -32006, `Internal` -32603. Transport errors are -32001 to -32004 (`JsonRpc::ErrorCode`).

### Lifecycle

- **Lifecycle:** the editor starts automation after opening its project, on 127.0.0.1 with a fresh token and a
  free port (`--automation-port <port>` picks one, `--no-automation` turns it off), and publishes its session.
  The session moves along when the editor opens or creates another project. On exit the editor cancels pending
  commands (their clients get `Cancelled`), removes the session files and stops the server with a grace period
  (`RpcServer::Stop(gracePeriod)`) so the last answers, such as `editor.quit`'s, still arrive. A headless editor
  without `--frames` runs until `editor.quit`, Ctrl+C or a termination signal (which close it in order; a killed editor
  leaves a stale session file, which clients prune), or, with `--idle-timeout <seconds>`, until no client has been
  connected and no request
  pending for that long (unsaved changes are then discarded, with a warning). The status bar's automation pill shows
  the port, or who is connected ("Agent connected · <name>", from `clientName`); `editor.status` has an `automation`
  section. The viewport names the latest automation command in a chip at its top right ("Agent: entity.create · 3 s
  ago"; a click shows it in the Activity panel) and outlines the entities it changed for 600 ms in the accent
  (`CommandJournal::GetLatestAutomationActivity`, `ViewportPanel::DrawAgentFlash`).

### Security model

- **Security model:** any local process, and any web page in a local browser, can reach the port; only
  holders of the session token are trusted. The server binds loopback addresses only and refuses to start
  without a token (`EditorSession::GenerateSessionToken`, from the OS secure random generator). Every
  connection starts with a challenge-response handshake (`RpcAuthentication`) in which the token never
  crosses the wire: `rpc.handshake` sends a client nonce and returns a server nonce with the server's HMAC
  proof, which `RpcClient` verifies before it proves anything in return (so clients never talk to a process
  that took over a dead editor's port); `rpc.authenticate` then sends the client's HMAC proof. Anything else
  closes the connection. Clients refuse to connect without a token. Unauthenticated connections get tiny
  limits and a deadline; authenticated ones get size limits and backpressure. Never log or print tokens.

### Session files

- **Session files:** `<per-user data>/Strata/Sessions/<pid>.json` (`Platform::FindUserDataDirectory`, e.g.
  `%LOCALAPPDATA%\Strata\Sessions`) holds the full session (address, port, token,
  process start time). It is written owner-only (`Platform::WritePrivateFile`) into a private directory. A
  session counts only while its process id is alive with the recorded start time (a reused id does not
  match). Files of exited editors are pruned; a live process whose start time cannot be verified is skipped,
  never deleted. `<project>/.strata/EditorSession.json` only names the editor's process; it
  is untrusted (the project may be shared) and never contains the port or token. An editor that cannot publish
  its session does not serve automation (nothing could find it).

### Clients and environment

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

### Other methods and tests

- **Other methods:** endpoints that are not editor commands use `RpcServer::RegisterMethod` with a description
  and a JSON Schema (`ReplaceMethod` swaps one in a single step). Handlers run on the main thread from
  `ProcessRequests()`; keep the `Ref<RpcResponder>` to answer later. Names starting with `rpc.` are reserved.
- **Tests:** `Editor.Automation` drives `EditorAutomation` in-process with `RpcClient`; the CLI and MCP suites
  never need a real editor (`Tests::PumpedRpcServer` plays the editor, `Tests::LiveProcess` gives fake sessions
  a running process id, and `STRATA_TEST_FAKE_EDITOR=1` makes the test executable act as a launched editor, see
  `StrataTests/src/Network/FakeEditorProcess.h`); the `EndToEnd` suites run the real editor and StrataCLI.

## Working with third-party code

AGENTS.md, [Working with third-party code](../AGENTS.md#working-with-third-party-code), says how submodules are
upgraded and states the rule for the editor's fonts in short. In full:

The editor's fonts are not submodules: the release files of Inter, JetBrains Mono and Lucide (with Lucide's codepoint
map) and their licenses are committed in `StrataEditor/Resources/Fonts/` and compiled in (`strata_embed_file`). To
upgrade one, replace its files with those of the new release, update `ThirdPartyNotices.md` and, for Lucide, run
`StrataEditor/Tools/GenerateIconHeader.py --version <release>` and commit the regenerated `UI/Icons.h` with it.
