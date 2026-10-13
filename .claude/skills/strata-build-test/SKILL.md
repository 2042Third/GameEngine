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
- Warnings are errors (`/W4 /WX`, `-Wall -Wextra -Werror`). Fix them; never silence them locally.
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

- doctest filters split on commas. A test name containing a comma must be matched with `?`/`*`, e.g.
  `--test-case="Hierarchy?*"`.
- CTest runs: `StrataTests.Core` (everything but the GPU, EndToEnd, `Package*`, perf and feature test suites; it
  includes `Architecture.Layering`, which fails on an include that crosses the layers of
  `StrataTests/Architecture/Layers.json` - fix the dependency, see AGENTS.md "Architecture rules"),
  `StrataTests.GPU` (label `gpu`), the perf tests `StrataTests.Perf` and `StrataTests.PerfGPU` (suites `Perf.*`
  and `PerfGPU.*`, label `perf`, Release and Dist only, after `StrataTests.PerfResults.Clean`; budgets, confirmed
  checks and the ratchet rule in AGENTS.md, "Testing": a metric over its budget is measured again, up to three attempts,
  and fails only when all are over; the attempts follow each other within about a minute, so load that lasts longer
  (another build) can still fail one metric or many: before treating "over the budget in all 3 attempts" as a
  regression, run the suite again on a quiet machine (the canary `Perf.Harness.Sort200k` is single-threaded and usually
  unchanged under load on many cores, so a normal canary does not rule load out); `PerfGPU.Editor` writes a 100,000-
  and a 1,000,000-entity scene to a temporary project and opens a maximized StrataEditor on them for about 20 seconds,
  again to confirm a failure; `PerfGPU.Streaming` writes and imports the stress project (17 large textures, 111.8 MB
  once block-compressed) and runs its camera sweeps in helper processes: the evicting one with whole textures, about
  15 s, the one with all textures resident at 1920x1080, and mip streaming's fly-through 3 units over the textured
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
  and `IdleFrame3kStreamedTextures` times an idle frame of 3,000 settled streamed textures in view;
  `Perf.Editor` adds `FindReferences10k`, `JournalPerCommand`, the recovery journal's write (the frames and the I/O
  threads' time, `RecoveryWriteIOTime100k`) and snapshot, and the longest frame while `scene.open` opens 10,000 prefab
  instances (`SceneOpen10kPrefabInstancesMaxFrame`); `Perf.EditorPanels` and `PerfGPU.Editor` time the Content Browser
  with 10,001 assets (icons; thumbnails); and `PerfGPU.Editor` the longest frames of `scene.open` of the 100,000- and
  1,000,000-entity scenes (and of the 100,000 over the million, and of 100,000 cubes over the 100,000, whose render
  proxies the load builds before the swap: `SceneOpen100kCubesMaxFrame`), of starting play of the million, of
  `asset.import` of a 4096x4096 texture and of making a stress project's thumbnails, and the editor's working set with
  the million open),
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
  without a GPU, driven by StrataCLI and its MCP server), and the feature test (label `feature`):
  `StrataTests.FeatureTest` (suites `FeatureTest` and `Editor.FeatureTest`) plus `StrataFeatureTest.Clean` → `.Copy` →
  `StrataEditor.FeatureTest` → `StrataRuntime.FeatureTest` (the real executables with a copy of
  `StrataTests/FeatureTest`). See AGENTS.md ("Testing") for what the feature test enforces when you add components or
  script API. Label `package` marks the tests that build script modules with CMake and the compiler at test time:
  `StrataScriptCore.Package` and `.PackageDist`, `StrataTests.Package` (suites `Package*`), `StrataEditor.Scripts` (the
  real editor builds, plays and exports a scripted game, `StrataTests/Editor/ScriptsEndToEnd.cmake`) and
  `StrataEditor.Tetris` (the editor plays `Samples/Tetris` with simulated input and checks its HUD, then exports it in
  full and pruned and runs both games headless, `StrataTests/Editor/TetrisSample.cmake`) and `StrataEditor.Install`
  (installs the build into a fresh prefix, as a release archive holds it, and makes, builds, plays and exports a game
  with the installed editor alone, `StrataTests/Editor/InstallEndToEnd.cmake`); `-LE package` skips them on machines
  without a toolchain. Packaging and releases: AGENTS.md, "Packages" and "Releases".
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
