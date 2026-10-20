---
name: strata-build-test
description: Build the Strata engine, editor and runtime, run the test suites (including GPU tests under the Vulkan validation layer and the editor/export smoke tests), and verify a change in a clean worktree before committing. Use whenever you compile, test or prepare a commit in this repository.
---

# Building and testing Strata

## Configure and build

CMake presets live in `CMakePresets.json`. On Windows the configure preset is `windows`; build and test
presets are `windows-debug`, `windows-release` and `windows-dist`. Linux and macOS use `linux-*` and
`macos-*` the same way.

```powershell
cmake --preset windows                       # configure (re-run after adding source or shader files)
cmake --build --preset windows-debug         # build everything (engine, editor, runtime, CLI, tests)
cmake --build --preset windows-debug --target StrataTests   # just the tests
```

- Source files are globbed: a new `.cpp`/`.h` needs a re-configure. Shaders are not globbed: add every new
  shader to `strata_add_shaders` in `Strata/CMakeLists.txt`.
- The engine is one static library per layer of `StrataTests/Architecture/Layers.json` (`Strata_<Layer>`, made by
  `CMake/StrataLayers.cmake`): a new engine file needs a layer pattern there (the configure names files in no layer or
  in two), a new layer is a table entry, and a vendor library a layer needs is a `strata_layer_link` row in
  `Strata/CMakeLists.txt`. Programs link `Strata` (the runtime layers) and, for tools, the tooling layers
  (`Strata::AssetPipeline`, `Strata::Network`, `Strata::ImGui`, or `StrataTooling`). A configure that fails with
  "StrataRuntime links tooling that games must not contain" names the path through which a runtime layer reached a
  tooling one: invert that dependency.
- Warnings are errors (`/W4 /WX`, `-Wall -Wextra -Werror`). Fix them; never silence them locally.
- Targets build with precompiled headers (`stpch.h`, `StrataEditor/src/EditorPch.h`, `StrataTests/src/TestsPch.h`),
  which hide a missing include: `python3 Tools/CompatLint.py --changed-since <base>` compiles without them, and so does a
  build configured with `-DSTRATA_PRECOMPILED_HEADERS=OFF`. Never put an engine, editor or test header that changes into
  them (`Architecture.PrecompiledHeaders`).
- `-DSTRATA_COMPILER_CACHE=ON` builds through ccache or sccache as CI does (precompiled headers off by default). On
  Windows it needs the `windows-ninja` preset, configured and built from a Developer PowerShell.
- A game without a module: `-DSTRATA_WITH_PHYSICS=OFF` (and `_AUDIO`, `_ANIMATION`) with `-DSTRATA_BUILD_EDITOR=OFF`, in a
  build directory of its own (`cmake --preset <platform> -B build/<name> ...`). A test case that needs a module is
  guarded with `#if ST_WITH_<MODULE>` (or `ST_TESTS_HAVE_EVERY_MODULE`); CI builds and tests the configuration without
  audio and physics, so check a change to such tests there or locally.
- Cached options survive a re-configure. If a target you expect is missing, check `build/<preset>/CMakeCache.txt`
  (for example `STRATA_BUILD_RUNTIME`) and pass `-D<OPTION>=ON`.

## Run the tests

```powershell
$env:VK_ADD_LAYER_PATH = "<repo>\build\_tools\Vulkan-ValidationLayers\build\layers\Release"   # optional, enables validation
ctest --preset windows-debug                  # all CTest tests
ctest --preset windows-debug -LE gpu          # machines without a GPU
ctest --preset windows-release -L perf        # perf tests against their budgets (Release/Dist only, reference machine)
python Tools/PerfStability.py --runs 5        # the perf tests five times: per-metric spread, pass rate, confirmations
build\windows\bin\Debug\StrataTests.exe --test-suite="Asset.*"        # one module (doctest filters)
build\windows\bin\Debug\StrataTests.exe --test-case="*export*"       # wildcard test names
```

On macOS with Homebrew's layers (`brew install molten-vk vulkan-loader vulkan-validationlayers`), the layer's manifest
names its library without a directory, which the loader does not find: give it a copy that names the full path.

```sh
mkdir -p build/_tools/vulkan-layers
python3 -c "import json; p='$(brew --prefix)'; d=json.load(open(p+'/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json')); d['layer']['library_path']=p+'/lib/libVkLayer_khronos_validation.dylib'; json.dump(d, open('build/_tools/vulkan-layers/VkLayer_khronos_validation.json', 'w'), indent=4)"
export VK_ADD_LAYER_PATH="$PWD/build/_tools/vulkan-layers"
ctest --preset macos-debug -LE perf
```

- doctest filters split on commas. A test name containing a comma must be matched with `?`/`*`, e.g.
  `--test-case="Hierarchy?*"`.
- Run the CTests side by side: `ctest --preset <platform>-debug -j 8`.
- CTest runs: the shards `StrataTests.Core.<Shard>` (label `core`: Core, Asset, Scene, Systems, Renderer, Network,
  EditorUI, EditorData, Editor; every suite but the GPU, EndToEnd, `Package*`, perf and feature test suites, split as
  `StrataTests/CoreShards.json` says: a new suite prefix needs an entry there, or `Testing.Shards` fails naming it; the
  Core shard includes `Architecture.Layering`, which fails on an include that crosses the layers of
  `StrataTests/Architecture/Layers.json` - fix the dependency, see AGENTS.md "Architecture rules"),
  `StrataTests.Fuzz` (label `fuzz`, Release and Dist: the `*.Sweep` suites in full, `STRATA_TEST_SWEEP=full`; their
  shards test a sample),
  `StrataTests.GPU` (label `gpu`), the perf tests `StrataTests.Perf` and `StrataTests.PerfGPU` (suites `Perf.*`
  and `PerfGPU.*`, label `perf`, Release and Dist only, after `StrataTests.PerfResults.Clean`; budgets, confirmed
  checks and the ratchet rule in Docs/DeveloperGuide.md, "Perf tests": a metric over its budget is measured again, up to
  three attempts, and fails only when all are over; the attempts follow each other within about a minute, so load that
  lasts longer (another build) can still fail one metric or many: before treating "over the budget in all 3 attempts" as
  a regression, run the suite again on a quiet machine (the canary `Perf.Harness.Sort200k` is single-threaded and
  usually unchanged under load on many cores, so a normal canary does not rule load out); `PerfGPU.Editor` writes a
  100,000- and a 1,000,000-entity scene to a temporary project and opens a maximized StrataEditor on them for about 20
  seconds, again to confirm a failure; `PerfGPU.Streaming` writes and imports the stress project (17 large textures,
  111.8 MB once block-compressed) and runs its camera sweeps in helper processes: the evicting one with whole textures,
  about 15 s, the one with all textures resident at 1920x1080, and mip streaming's fly-through 3 units over the textured
  ground of the `Flight` scene (1920x1080, 600 frames, about 30 s: with a 96 MB texture budget, again within 8 MB, and
  its whole-texture baseline; `PerfGPU.Streaming.Mips.*`: peak private memory, the most bytes and upload-step
  milliseconds of a frame, the longest frame to the baseline's; within 96 MB it must reach level 0 and its last frame
  match the baseline's by 40 dB PSNR, also over the bottom third; within 8 MB it must drop levels for the budget and
  stay within it), each with up to two more to confirm a failure, then the exported game in
  `StrataRuntime` (from the overview camera, then the whole world in view: `RuntimePeakPrivateMemoryAllInView`, every
  texture at its wanted level at the end); `Perf.TextureCook` imports the same project cold and times the BC7 encoder
  per block; `PerfGPU.Pack` writes the project twice, with its textures block-compressed and uncompressed, in pack
  versions 2 and 1; `Perf.Scene` also copies a million entities for play (`Copy1M`) and instantiates prefabs
  (`InstantiatePrefab10`, `InstantiatePrefab1k`, `InstantiateMany1000x10`); `Perf.Runtime` switches a headless
  `GameRuntime` to a 100,000-entity scene and away again (the longest frame); `Perf.Animation` evaluates 1,000
  cross-fading animators of 60 joints and steps 10,000 `WhenVisible` ones nobody sees; `Perf.Physics.Characters1000Step`
  steps 1,000 character controllers; `PerfGPU.Renderer.Skinned1000` renders 1,000 skinned characters posed every frame
  and `IdleFrame3kStreamedTextures` times an idle frame of 3,000 settled streamed textures in view; the renderer's GPU
  side comes from `Perf/RendererPerfUtils.h` (`Perf::MeasureFrames` runs frames as an application does and collects the
  GPU's busy time of each, `Perf::MeasureGPU` its median, which fails when a device that writes timestamps delivered none;
  without timestamps the GPU metrics are left out, `Perf::MeasuresGPU`): `Render100kCubesGPU` is the GPU's median busy time
  of the 100,000 cubes, `Render1MCubes` and `Render1MCubesGPU` the CPU's and the GPU's for a million (the GPU-driven path's
  CPU cost does not grow with instances: target 1.5 ms), `GPUTimerCost` the GPU profiler's own CPU time in the median
  frame (target 0.05 ms), `Render100kCubesShadowed` and `Render300kCubesShadowed` the CPU's median Render of the 100,000
  and 300,000 cube grids under the sun's shadows, every cube a caster (targets 1.5 and 3 ms), `Render10kUniqueMeshes` the
  CPU's median Render (extraction, recording and submitting) of 10,000 distinct meshes (target 2 ms) with
  `Render10kUniqueMeshes.DrawCommands` the draw calls it recorded, and `PlayStartRender1MCubes` what starting play on a
  million rendered cubes costs: the render scene's clone in `Scene::Copy` plus the copy's first Render beyond the
  source's median (target 5 ms);
  `Perf.Editor` adds `FindReferences10k`, `JournalPerCommand`, the recovery journal's write (the frames and the I/O
  threads' time, `RecoveryWriteIOTime100k`) and snapshot, and the longest frame while `scene.open` opens 10,000 prefab
  instances (`SceneOpen10kPrefabInstancesMaxFrame`), and saving the realistic worlds in a helper process (the longest frame
  from the save's start to its publication, `SaveMaxFrame250k` and `SaveMaxFrame1M`; the 1M save's `SaveSeconds1M` and
  `SavePeakPrivateIncrease1M`; `--strata-test-helper=save-world <project> <scene> <result file>` runs one by hand);
  `Perf.Scene.CancelLoad1M` cancels loads of the realistic 1M world at 25, 50 and 75% of the read and
  `RefuseOverCap1M` loads a 1,100,445-entity document (both through `SceneLoader`); `Perf.Runtime.World1MStart`,
  `World1MTransientMemory` (a helper process: `--strata-test-helper=start-world-game <manifest> <result file>`) and
  `QuitDuringLoad1M` start and quit the realistic 1M world exported as a game (cooked, and with its JSON);
  `Perf.Editor.ImportFolder24x4kPeakPrivate` imports 24 textures of 4096x4096 with a 1 GiB import memory budget (a
  helper process: `--strata-test-helper=import-folder <project> <folder> <budget MB> <result file>`);
  `Perf.EditorPanels` and `PerfGPU.Editor` time the Content Browser
  with 10,001 assets (icons; thumbnails); and `PerfGPU.Editor` the longest frames of `scene.open` of the 100,000- and
  1,000,000-entity scenes (and of the 100,000 over the million, and of 100,000 cubes over the 100,000, whose render
  proxies the load builds before the swap: `SceneOpen100kCubesMaxFrame`), of starting play of the million, of
  `asset.import` of a 4096x4096 texture and of making a stress project's thumbnails, and the editor's working set with
  the million open; `PerfGPU.Editor.WorldOpen1M*` opens the realistic 1M world in a headless editor through `StrataCLI`
  (its time, longest frame and peak memory; `WorldOpen1MSaved*` once the editor saved it); world-scale gates write their projects with `Perf/WorldFixture.h`: realistic 250k and 1M worlds,
  many-asset projects, texture folders, exported world games; to look at one, `StrataTests
  --strata-test-helper=write-world-project <new dir> World1M:1000000:colliders`, `write-many-assets-project <new dir>
  30000` or `write-texture-folder <new dir> 24 4096`),
  `StrataEditor.Smoke`
  (the real editor in a window with `StrataTests/Editor/SmokeCommands.json.in`, configured into the build tree, label
  `gpu`; it creates a project in `build/<preset>/StrataTests/SmokeProject` (emptied first by `StrataEditor.SmokeClean`),
  so the editor rather than the launcher shows, captures the viewport to `SmokeViewport.png`/`SmokeSceneCamera.png` in
  `build/<preset>/StrataTests/SmokeCaptures/`, checked by `StrataEditor.SmokeCaptureCheck`, and quits once its script is
  done), `StrataEditor.QuitScreenshot` → `.QuitScreenshotCheck` (`--screenshot` with `--quit-after-commands` saves
  the frame the script ended on), `StrataEditor.LauncherSmoke` (a windowed editor without a project opens into the
  launcher, `Editor/LauncherCommands.json`), `StrataEditor.Replay` (`Editor/ReplaySession.cmake`: a session exported
  with `editor.activity.export` and run on a copy of the project makes the same scene and assets), `StrataEditor.Icon`
  and `StrataRuntime.Icon` (Windows: the editor and the exported game's executable carry the strata mark, checked with
  `StrataTests --strata-test-helper=check-icon <exe> <.ico>`),
  the editor script checks (`StrataEditor.FailingScript`, `WaitingScript`,
  `UnfinishedScript`, `QuitScript`, `QuitBeforeScriptEnds`, `CaptureWithoutGPU`), the export chain `StrataExport.Clean`
  → `StrataEditor.Export` (`--no-gpu`; it exports the smoke game twice, the second time starting in a scene without a
  camera; exports run in the background, so the run quits after its commands) → `StrataRuntime.Smoke` (the exported
  game, headless; `StrataRuntime.HeadlessPacing` checks that headless runs keep 60 frames per second,
  `StrataRuntime.AssetBudget` and `.InvalidAssetBudget` the `--asset-budget-mb` option, `StrataRuntime.Resolution` →
  `.ResolutionCheck` (label `gpu`: a window of exactly 400x250 pixels whatever the display's scale, checked with
  `check-image --size`) and `.InvalidResolution` the `--resolution` option, `.InvalidSaveDirectory` the
  `--save-directory` option, `StrataRuntime.PerfLog` →
  `.PerfLogCheck` and `.InvalidPerfLog` the `--perf-log` CSV, read back by
  `StrataTests --strata-test-helper=check-perf-log <csv> <rows> [--nonzero <column>...]`) →
  `StrataRuntime.Render` (windowed, saves
  `build/<preset>/StrataTests/ExportSmoke/RuntimeScreenshot.png`, label `gpu`; `StrataRuntime.PerfLogWindowed` →
  `.PerfLogWindowedCheck` check that windowed perf logs time rendering and presenting) → `StrataRuntime.RenderCheck`
  (`StrataTests --strata-test-helper=check-image <png> [--dominant red|green|blue <percent>]`: not black, not a single
  color, and here at least 1% of the blue box), plus `StrataRuntime.NoCameraScreenshot` (a screenshot of the
  missing-camera message fails the run), `StrataEditor.Automation` (the `EndToEnd*` suites: the real editor, headless
  without a GPU, driven by StrataCLI and its MCP server), `StrataRuntime.NoTooling` (the runtime's linker map,
  `StrataRuntime.map` next to it, names no object or library of ImGui, ImGuizmo, the asset pipeline, the network or
  zstd's compressor: `StrataTests/Architecture/CheckNoTooling.cmake`), and the feature test (label `feature`):
  `StrataTests.FeatureTest` (suites `FeatureTest` and `Editor.FeatureTest`) plus `StrataFeatureTest.Clean` → `.Copy` →
  `StrataEditor.FeatureTest` → `StrataRuntime.FeatureTest` (the real executables with a copy of
  `StrataTests/FeatureTest`). See Docs/DeveloperGuide.md ("Feature test") for what the feature test enforces when you
  add components or script API. Label `package` marks the tests that build script modules with CMake and the compiler at
  test time: `StrataScriptCore.Package` and `.PackageDist`, `StrataTests.Package` (suites `Package*`),
  `StrataEditor.Scripts` (the real editor builds, plays and exports a scripted game,
  `StrataTests/Editor/ScriptsEndToEnd.cmake`) and `StrataEditor.Tetris` (the editor plays `Samples/Tetris` with
  simulated input and checks its HUD, then exports it in full and pruned and runs both games headless; every sample's test
  runs on `StrataTests/Editor/SampleProject.cmake`, which also fails on warnings its `ALLOWED_MESSAGES` do not list:
  `StrataEditor.Tetris`, `.PhysicsPlayground`, `.OpenWorld` and `.ThirdPerson`; their exported games' windowed checks
  `StrataRuntime.<Sample>Windowed*` and `StrataEditor.OpenWorldRender` are `gpu` tests on their exports, and
  `StrataTests.PerfSamples` records the Open World tour's provisional metrics in Release) and `StrataEditor.Install` (installs the build into a fresh prefix, as a
  release archive holds it, and makes, builds, plays and exports a game with the installed editor alone,
  `StrataTests/Editor/InstallEndToEnd.cmake`); `-LE package` skips them on machines without a toolchain. Packaging and
  releases: Docs/DeveloperGuide.md, "Packages" and "Releases".
- Scripted editor runs in CTest pass `--no-automation`, so they never publish sessions for real clients.
- Editor tests: commands that defer (`scene.open`, `project.open` of a project with a start scene, `asset.import`, a
  `play.start` that builds) need `Tests::RunCommandToCompletion` (`StrataTests/src/Editor/CommandTestUtils.h`); UI
  tests that bring a tab forward wait until it shows before clicking in it (ImGui draws it hidden in its first frame);
  every new editor command gets its kind in `Editor.Journal`'s lists (`CommandJournalTests.cpp`).
- Tests that expect a process to fail with exit code 1 and a message use `StrataTests/ExpectFailure.cmake`.
- To look at rendering changes, read those PNGs, or run `StrataEditor --windowed --frames N --commands <script>
  --screenshot out.png` with a script that builds a scene and uses `camera.set`/`camera.focus`.
- GPU tests must end with `CHECK(gpu.GetNewErrorCount() == 0)` so validation errors fail them.
- Debug builds assert after every `Scene::OnUpdateRuntime`/`OnUpdateEditor` that the scene's caches match a full
  recomputation. A test aborting with "Scene '...' has inconsistent caches: entity '...'" wrote that entity's
  `TransformComponent` (or name or tag) without `MarkModified` after the scene was updated (AGENTS.md, "Architecture
  rules"); in Release the same test would render or simulate stale world transforms.
- Run both Debug and Release before committing: some bugs (uninitialized memory, timing) only show in one.

## Debugging a crash in a test

doctest only reports "test case CRASHED". To locate it, add temporary `std::fprintf(stderr, ...)`
checkpoints between statements (flush stderr), run the single test case, and remove them afterwards.
Keep a copy of the file before inserting them.

## Verify a commit in a clean worktree

The working tree often holds unrelated work. Before committing, verify exactly what is staged:

```bash
git diff --cached --binary > staged.patch
cd .worktrees/verify && git reset --hard <base> && git clean -fd
git apply --index ../../staged.patch
cmake --preset windows && cmake --build --preset windows-debug && ctest --preset windows-debug
cmake --build --preset windows-release && ctest --preset windows-release
```

Then commit in the main checkout, ending the message with the attribution line the session requires.
Review the full staged diff against the "Pre-commit review checklist" in `AGENTS.md` first.
