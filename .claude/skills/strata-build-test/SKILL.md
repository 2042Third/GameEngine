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
build\windows\bin\Debug\StrataTests.exe --test-suite="Asset.*"        # one module (doctest filters)
build\windows\bin\Debug\StrataTests.exe --test-case="*export*"       # wildcard test names
```

- doctest filters split on commas. A test name containing a comma must be matched with `?`/`*`, e.g.
  `--test-case="Hierarchy?*"`.
- CTest runs: `StrataTests.Core` (everything but the GPU and EndToEnd suites), `StrataTests.GPU` (label `gpu`),
  `StrataEditor.Smoke` (the real editor with `StrataTests/Editor/SmokeCommands.json.in`, configured into the build tree,
  label `gpu`; it captures the viewport to `SmokeViewport.png`/`SmokeSceneCamera.png` in
  `build/<preset>/StrataTests/SmokeCaptures/`, checked by `StrataEditor.SmokeCaptureCheck`), the editor script checks
  (`StrataEditor.FailingScript`, `WaitingScript`, `UnfinishedScript`, `QuitScript`, `QuitBeforeScriptEnds`,
  `CaptureWithoutGPU`), the export chain `StrataExport.Clean` → `StrataEditor.Export` (`--no-gpu`; it exports the smoke
  game twice, the second time starting in a scene without a camera) → `StrataRuntime.Smoke` (the exported game,
  headless) → `StrataRuntime.Render` (windowed, saves `build/<preset>/StrataTests/ExportSmoke/RuntimeScreenshot.png`,
  label `gpu`) → `StrataRuntime.RenderCheck` (`StrataTests --strata-test-helper=check-image <png> [--dominant
  red|green|blue <percent>]`: not black, not a single color, and here at least 1% of the blue box), plus
  `StrataRuntime.NoCameraScreenshot` (a screenshot of the missing-camera message fails the run), and
  `StrataEditor.Automation` (the `EndToEnd*` suites: the real editor, headless without a GPU, driven by StrataCLI and its
  MCP server).
- Scripted editor runs in CTest pass `--no-automation`, so they never publish sessions for real clients.
- Tests that expect a process to fail with exit code 1 and a message use `StrataTests/ExpectFailure.cmake`.
- To look at rendering changes, read those PNGs, or run `StrataEditor --windowed --frames N --commands <script>
  --screenshot out.png` with a script that builds a scene and uses `camera.set`/`camera.focus`.
- GPU tests must end with `CHECK(gpu.GetNewErrorCount() == 0)` so validation errors fail them.
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
