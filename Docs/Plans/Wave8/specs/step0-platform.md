# Step 0 of wave 8, lane Platform: GLFW sealed (S0.8), why FIFO does not pace the editor (S0.6), the project's fixed rate (S0.9)

Lane Platform runs on branch `agent/wave8-platform` in `G:/GameEngineAI/.worktrees/wave8-platform` (build tree
`build/agent-platform`, `cmake --preset windows -B build/agent-platform`), branched from main once S0.0 and S0.1 have
landed (plan section 3, "Landing step 0"). S0.0 and S0.1 change only documents, so every code line number below, taken at
76f28a21, holds at the lane's base; AGENTS.md's line numbers move with S0.1 (its bullets are named, not numbered).

Paths: `R/` is `Strata/src/Strata/Renderer/`, `C/` is `Strata/src/Strata/Core/`, `S/` is `Strata/src/Strata/Scene/`,
`P/` is `Strata/src/Platform/`, `E/` is `StrataEditor/src/Editor/`, `T/` is `StrataTests/src/`, `DG` is
`Docs/DeveloperGuide.md`, `plan` is `.worktrees/wave8-plan/plan.md`, `decisions` is `.worktrees/wave8-plan/specs/decisions.md`.
The audits are `.worktrees/wave8-plan/platform-seams/audit.md` ("the platform audit") and
`.worktrees/wave8-plan/physics-timing/audit.md` ("the timing audit"); `editor-ux/audit.md` holds the FIFO measurements.

Rules for the whole lane (AGENTS.md, plan 4.1, decisions):

- Every commit: Debug build with warnings as errors, `ctest --preset windows-debug -j 8` with every `GPU*` suite under the
  validation layer on the RTX 5090, `python Tools/CompatLint.py --changed-since <lane base>` clean. Release `ctest -LE perf`
  before each `ci/` push.
- Never the owner's `build/windows`; at most four build-heavy agents on the machine; editors this lane starts get private
  session directories and are reached with `StrataCLI call --project`; scripts in files (a script on stdin hangs).
- Perf only as interleaved A/Bs under `python Tools/PerfLock.py --label wave8-platform -- ...` against the kept 76f28a21
  Release build in `.worktrees/wave8-base`, as information: no campaign, no `Budgets.json` change (D4).
- No code from outside the repository. GLFW's and the Vulkan headers' APIs may be read in `Strata/vendor/`.

## 1. The owner's request and the lane's goal

The owner (2026-11-01, `owner_feedback.md`), item 15: "Make sure glfw is wrapped for future console port". Item 11:
"Currently the physics frame rate is constant or something, change this to an option or something and/or make it tick by
tick following the frame refresh. Currently physics refresh implementation is locked at 60 and is terrible at high frames
(character appear to be jittering). (easy to do)". Items 12 and 13 rest on frame rates the editor reaches with vsync on,
which nobody can explain yet. In priority order:

1. **GLFW sealed (S0.8, item 15).** Only the window backend names GLFW, and a rule keeps it so: the Vulkan device presents
   through an interface the window gives it (steps 1-2, never cut). Window creation fails through the fatal-error path
   instead of aborting, a stand-in window drives the frame loop in tests, CI builds the engine without GLFW, and the docs
   list what else a console port would need (steps 3-6). No thread, file or process abstraction: without an SDK they would
   be guesses (plan section 9).
2. **The fixed rate as the project's option (S0.9, item 11's "option").** A project section `Scene.Timing` whose
   `FixedRate` (Scene, 30, 60, 120, 240) overrides every scene's step, with the most steps an update runs scaled to it, and
   every direct reader of the scene's step moved onto one accessor. It does not cure the jitter: that is drawing between
   fixed poses, the stream `physics-timing` (plan 4.6, D5), which builds on this section.
3. **Why FIFO does not pace the editor (S0.6).** A boxed investigation of half a day: measure, name the cause or fix it,
   and correct what the docs claim (`Docs/Architecture.md:809`: "Windowed applications are paced by vsync when it is on").

Not the goal: a console port, another graphics API or shader target (lane Slang), interpolation of drawn poses
(`physics-timing`), the editor's frame-rate preferences (S0.4, lane Editor).

## 2. Where we start

### 2.1 Files

| File | Lines | Role in this lane |
| --- | --- | --- |
| `C/Window.h` | 130 | The window interface. `GetNativeWindow()` (125) is the `void*` escape hatch the device uses; `Create` (127) is defined by the backend |
| `P/GLFW/GLFWWindow.h` / `.cpp` | 81 / 628 | The backend: 51 `glfw*` symbols (platform audit). `Window::Create` (`.cpp:45-48`), constructor (50-292) with `ST_CORE_VERIFY` on `glfwInit` (65) and `glfwCreateWindow` (99), destructor (294-305), `s_GLFWWindowCount` (26) |
| `P/GLFW/GLFWApplicationIcon.h` / `.cpp` | 19 / 138 | The macOS Dock icon through the Objective-C runtime; no GLFW include (only the folder) |
| `P/Vulkan/VulkanLoader.h` / `.cpp` | 19 / 91 | In the App layer (`Layers.json:21-22`): GLFW initializes its Vulkan loader from it (`GLFWWindow.cpp:62-63`) |
| `P/Vulkan/VulkanGraphicsDevice.cpp` | 1,603 | The leak: `#include <GLFW/glfw3.h>` (20-21), `glfwGetRequiredInstanceExtensions` (480), `glfwGetPhysicalDevicePresentationSupport` (699), `glfwCreateWindowSurface` through `GetNativeWindow()` (1035). Present mode (1125-1132), swapchain log (1222-1223), `BeginFrame` (1400-1475), `EndFrame` (1519-1567) |
| `R/GraphicsDevice.h` | 196 | `GraphicsDeviceSpecification` (17-27, `Headless` 24), presentation API (144-150) |
| `R/GraphicsModule.cpp` | 128 | The only windowed device creator: `Headless = m_Window == nullptr` (33), swapchain (42-43), vsync and `BeginFrame` (61-75) |
| `Strata/src/Strata/ImGui/ImGuiModule.cpp` | 66 | Requires a window and a device (28-33) |
| `C/Application.h` / `.cpp` | 143 / 352 | `ApplicationSpecification` (25-43), `ReportFatalError` (`.cpp:42-62`), window creation without a failure path (`.cpp:84-92`), `StartModules`' failure path, the model for a window failure (114-148), minimized frames (295-300) |
| `StrataTests/Architecture/Layers.json` | 126 | App (20-24), Asset's `MayNotIncludeExternal` (31), Renderer (43-48) |
| `T/Architecture/LayeringAnalyzer.h` / `.cpp` | 105 / 508 | `Layer`, `LayerTable` (`.h:39-54`), `Violation` (73-83); `ParseLayerTable` (`.cpp:301-350`), the third-party branch of `CheckLayering` (456-466), `Describe` (497-506) |
| `T/Architecture/LayeringTests.cpp` | 521 | `Architecture.Layering`: the tree's check (113-135), injected include (168-196), prefixes (459-485), "The asset layer knows no graphics API" (487-520) |
| `T/Architecture/LibrariesTests.cpp` | 178 | `IsOtherPlatform` (33-49), used at 154 |
| `CMake/StrataLayers.cmake` | 423 | The per-OS source filter (104-111) |
| `CMakeLists.txt` (root) | 84 | Submodule canary on glfw (13-15), module options and the editor's check (25-41), the runtime's link closure (71) |
| `Strata/CMakeLists.txt` | 195 | `ST_WITH_*` definitions (25-28), vendor links: AppKit (52-56), App (61), Renderer with `glfw` (64); the shader list (74-180) is lane Slang's |
| `Strata/vendor/CMakeLists.txt` | 254 | GLFW (50-55), unconditional; glslang (226-235) is lane Slang's |
| `.github/workflows/ci.yml` | 244 | The `Linux (no Audio, no Physics)` row (50-60); tests `-LE "gpu|perf"` plus the row's `exclude` (191-193) |
| `StrataTests/CMakeLists.txt` | 985 | `STRATA_TESTS_OF_<MODULE>` (73-97), `StrataTests.DockIcon` (321-328), `StrataRuntime.NoTooling` (334-340), runtime argument tests (499-508) |
| `StrataTests/Architecture/CheckNoTooling.cmake` | 71 | The runtime's linker-map check: forbidden objects (19-30) and libraries (32-33), required objects (36) |
| `T/ImGui/ImGuiPlatformTests.cpp`, `T/Scripting/ScriptShellTests.cpp` | 449, 404 | The two fake `Window`s (`TestWindow` 22-70, `CountingWindow` 63-111) |
| `T/Core/GPUWindowTests.cpp`, `T/Platform/DockIconTests.cpp`, `T/Core/ApplicationIconTests.cpp` | 112, 192, 34 | Real windows (`Window::Create` at 39, 86 and 162) and the icon's header (3) |
| `T/TestMain.cpp` | 1,317 | Helper modes (`--strata-test-helper=`): `run-with-environment` (284-316), `create-graphics-device` (317-332) |
| `Strata/src/Strata/Input/KeyCodes.h`, `MouseCodes.h`, `GamepadCodes.h` | 150, 40, 50 | Comments "Values match GLFW" (10, 10, 8); the values are script ABI (`StrataScriptCore/Include/StrataScript/Input.h:19, 175`) |
| `S/Scene.h` / `.cpp` | 847 / 2,315 | `SceneSettings` (`.h:60-78`), `GetFixedTimestep` (436), layers' working copy (574-598, members 739-743); `ApplyProjectLayers` (`.cpp:216-225`), `Copy` (237-244), the fixed loop (2153-2216, max steps 2174) |
| `S/ProjectCollisionLayers.h` / `.cpp` | 42 / 114 | The model of a project section a scene takes at hand-over |
| `S/SceneRegistration.cpp`, `S/Prefab.cpp`, `S/SceneLoader.cpp` | 39, 431, 341 | Section registration (18); hand-overs (`Prefab.cpp:412, 427`, `SceneLoader.cpp:328`) |
| `Strata/src/Strata/Runtime/GameRuntime.cpp` / `.h` | 432 / 201 | `LimitToFixedSteps` (`.cpp:28-41`), `Update` (294-310); option comments (`.h:35-44, 97-103`) |
| `Strata/src/Strata/Scripting/ScriptHostAPI.cpp` | 3,668 | `HostGetFixedDeltaTime` reads the settings (318-328) |
| `E/EditorInputCommands.cpp` | 1,363 | Durations in seconds (70-80, 83-107; the reader at 98) |
| `E/EditorWorldCommands.cpp` | 1,090 | `physics.stats`' `effectiveContactHertz` (421) |
| `E/EditorSceneCommands.cpp` | 654 | `DescribeSettings` (38-52), `scene.info` (314-330), `scene.setSettings` (560-651) |
| `E/EditorContext.cpp` / `.h` | 2,935 / 1,027 | `ApplyProjectLayers`, `AdoptProjectLayers` (`.cpp:549-564`, `.h:213-216`), play start's copy (1374), play's scene switch (1529) |
| `E/ProjectSettingsAction.cpp` | 106 | Applies the project's sections to the scenes after every change (75-77) |
| `StrataEditor/src/Panels/InspectorPanel.cpp` / `.h` | 1,677 / 242 | `DescribePhysics` (255-262), the rows (294-323), the summary cache (`.cpp:1293-1295`, `.h:150-157`), the Physics section (1318-1338), the layers link (1374-1388) |
| `StrataTests/FeatureTest/FeatureTest.stproj` | 199 | Project sections the feature test sets (`Scene.CollisionLayers` 174-193) |
| `StrataTests/FeatureTest/Scripts/TimeFeatures.cpp` | 70 | Expects the scene's 0.02 s step (25, 52); `Feature.stscene` stores `"FixedTimestep": 0.02` |
| `T/Runtime/GameRuntimeTests.cpp` | 966 | `WriteGame`, `RewriteManifest` (103-144), the long-frame test (265-341) |

### 2.2 Facts each step depends on (checked at 76f28a21)

- **Two objects reference GLFW** in the owner's Release libraries: `GLFWWindow.obj` (51 symbols) and
  `VulkanGraphicsDevice.obj` (3), from `nm -u` (platform audit, `scan_glfw_symbols.py`). Only `Strata_App` and
  `Strata_Renderer` see GLFW's include path (glfw is linked `PRIVATE`: `Strata/CMakeLists.txt:61, 64`).
- **The device knows the window only at swapchain time.** `GraphicsDevice::Create` gets no window: `CreateInstance`
  (`VulkanGraphicsDevice.cpp:477-488`) and `SelectPhysicalDevice` (699) call GLFW globals, which work because the window
  exists, and so GLFW is initialized, before `GraphicsModule::OnInit` (`Application.cpp:84-95`). `CreateSwapchain(Window&,
  bool)` (1006-1024) then keeps `m_Window` and `CreateSurface` (1033-1052) casts `GetNativeWindow()` to `GLFWwindow*`.
- **Every other device is headless**: `GPUTestUtils.cpp:37`, `GPURendererTests.cpp:76`, `TestMain.cpp:323` set
  `Headless = true`; `GraphicsModule.cpp:40` is the only other `GraphicsDevice::Create` caller and the only caller of
  `CreateSwapchain` (43). `VulkanGraphicsDevice` is the only `GraphicsDevice` implementation.
- **`GetNativeWindow` has one real caller** (`VulkanGraphicsDevice.cpp:1035`) and two overrides in test fakes
  (`ImGuiPlatformTests.cpp:57`, `ScriptShellTests.cpp:102`) besides `GLFWWindow.h:46`.
- **Window creation aborts.** `ST_CORE_VERIFY` stays in Dist (AGENTS.md); `Application.cpp:89-91` dereferences the result
  without a check. AGENTS.md ("Windows subsystem") requires fatal startup failures to go through `ReportFatalError`.
- **The derived applications already survive a failed start**: `EditorApplication.cpp:221-223` and
  `RuntimeApplication.cpp:328` test `IsRunning()`/`GetWindow()` before using the window, as `StartModules`' failure resets
  the window (`Application.cpp:119-120, 141-142`).
- **The analyzer checks third-party headers only by layer** (`LayeringAnalyzer.cpp:456-466`): an include that resolves to
  no engine file is checked against the including layer's `MayNotIncludeExternal` prefixes, matched case-sensitively with
  `starts_with`. A per-layer rule cannot separate `Application.cpp` from `Platform/GLFW` (both App). `<GLFW/glfw3.h>`
  resolves to no layer (the prefix `GLFW/` does not match `Platform/GLFW/**`).
- **OS headers in `Strata/src`**: only under `Platform/Windows` (`Windows.h`, `winsock2.h`, `ws2tcpip.h`, `mstcpip.h`,
  `shlobj.h`, `shellapi.h`, `psapi.h`, `bcrypt.h`, `aclapi.h`, `DbgHelp.h`, `io.h`, `malloc.h`, `fcntl.h`) and
  `Platform/Posix` (`unistd.h`, `sys/*`, `pthread.h`, `dlfcn.h`, `poll.h`, `spawn.h`, `sched.h`, `ucontext.h`, `elf.h`,
  `netdb.h`, `netinet/*`, `arpa/*`, `mach/*`, `mach-o/*`, `libproc.h`, `crt_externs.h`, `fcntl.h`); `Vulkan` and `nvrhi/vulkan.h`
  only under `Platform/Vulkan` (grep of `#include <...>` over `Strata/src`, 2026-11-01). `Platform/GLFW/GLFWApplicationIcon.cpp`
  includes `objc/` (left unowned, section 5.2). `Platform/MacOS` is listed in Core's paths (`Layers.json:9`) and holds no file.
- **The source filter** (`StrataLayers.cmake:104-111`) drops other OSes' `Platform/` folders by regex;
  `LibrariesTests.cpp:34-49` must agree with it, and `Architecture.Layering` reads every file whatever the build.
- **No test opens a window outside the `gpu` label** (`GPUWindowTests.cpp:28`, `StrataTests.DockIcon` labelled gpu at
  `StrataTests/CMakeLists.txt:327`). CI's Linux runners have no display.
- **GPU tests share one device** (AGENTS.md "Testing"). One case makes a second bare headless device in process
  (`GPURendererTests.cpp:69-91`), but an `Application` with the Graphics module also calls `Renderer::Init`, which is
  process-wide state the shared `GPUContext` already holds. So a device made by an application runs in a child through a
  helper mode, as `GPURendererTests.cpp:110-126` does with `create-graphics-device`. Helper modes log only warnings and
  errors unless they initialize the log themselves (`TestMain.cpp:1261-1266`), and the builtin modules are registered
  before any helper mode runs (`TestMain.cpp:1284-1287`).
- **`Core*` and `Scene*` suites already have shards** (`StrataTests/CoreShards.json:15, 17`): `Core.WindowBackend` and
  `Scene.Timing` need no shard entry (the platform audit left this open).
- **The fixed loop** (`Scene.cpp:2153-2216`): the step is `GetFixedTimestep()`, the cap `std::max(1u,
  m_Settings.MaxFixedStepsPerFrame)` (2174); beyond the cap the backlog is dropped (2210-2215). At 240 steps/s eight steps
  are 33 ms of time: longer frames lose time (timing audit).
- **Scenes take the project's collision layers at hand-over on the main thread** (`Scene.cpp:216-225`; callers
  `Prefab.cpp:412, 427`, `SceneLoader.cpp:328`, `EditorContext.cpp:549-559`, `ProjectSettingsAction.cpp:77`; DG "Collision
  layers"). `Scene::Copy` keeps the copy and its version (`Scene.cpp:239-242`); play starts from a copy
  (`EditorContext.cpp:1374`). The editor always provides `IProjectSettings` (`EditorContext.cpp:152`, none set without a
  project); a game provides it from a version-5 manifest's settings (`GameRuntime.cpp:57-68`), which the export fills with
  the project's sections (`GameExport.cpp:317`).
- **Simple sections** validate, default and describe themselves from their fields; an `Enum` field stores the option's
  name (`SettingsRegistry.h:32-60`); sizes are stored as option names already (`"4096"`, `RendererQuality.cpp:280`).
- **The feature test** fails when a registered project section is unset or at its default in `FeatureTest.stproj`
  (DG "Feature test"). The feature scene stores a 0.02 s step that `TimeFeatures.cpp:25, 52` checks.

### 2.3 Every reader of the scene's step and its cap (S0.9)

From `grep -rn 'FixedTimestep\|MaxFixedStepsPerFrame\|GetFixedTimestep'` over `Strata/src`, `StrataEditor/src`,
`StrataRuntime/src` at 76f28a21:

| Reader | Reads | After S0.9 |
| --- | --- | --- |
| `Scene.cpp:2153` | `GetFixedTimestep()` | unchanged (the accessor gains the project's rate) |
| `Scene.cpp:2174` | `m_Settings.MaxFixedStepsPerFrame` | `GetMaxFixedSteps()` |
| `GameRuntime.cpp:38-39` (`LimitToFixedSteps`) | `GetSettings().MaxFixedStepsPerFrame`, `GetFixedTimestep()` | `GetMaxFixedSteps()`, `GetFixedTimestep()` |
| `GameRuntime.cpp:306` | `GetFixedTimestep()` | unchanged |
| `AudioSystem.cpp:696` | `GetFixedTimestep()` | unchanged |
| `ScriptHostAPI.cpp:325-326` | `GetSettings().FixedTimestep` | `GetFixedTimestep()` |
| `EditorInputCommands.cpp:98` | `GetSettings().FixedTimestep`, or 1/60 while not playing | the active scene's `GetFixedTimestep()` in both cases |
| `EditorWorldCommands.cpp:421` | `GetSettings().FixedTimestep` | `GetFixedTimestep()` |
| `InspectorPanel.cpp:260` (`DescribePhysics`) | `settings.FixedTimestep` | the scene's `GetFixedTimestep()` when the project sets the rate |
| `EditorSceneCommands.cpp:43-44` (`DescribeSettings`) | the stored values | kept, plus the effective `fixedRate` object |
| `SceneSerializer.cpp:175-176, 200-203`, `EditorSceneCommands.cpp:603-616`, `InspectorPanel.cpp:1323-1326, 1366-1367` | the stored values | unchanged: they read and write the scene's own setting |
| Comments: `SimulatedInput.h:52`, `StrataRuntime/src/RuntimeApplication.cpp:77`, `GameRuntime.cpp:28-31`, `GameRuntime.h:39-44, 101-102` | name `MaxFixedStepsPerFrame` as the cap | name `Scene::GetMaxFixedSteps` |

The implementer repeats the grep at the lane's base and adds any reader merged since.

## 3. Order, landings and the cut line

1. **S0.9 first, landed by itself** through `ci/wave8-platform-timing`, as S0.7 lands ahead of lane Editor:
   `physics-timing` starts when lane Render has landed (plan 4) and extends `Scene.Timing` (its step 4) and the same lines of
   `Scene.cpp`, `ScriptHostAPI.cpp`, `GameRuntime.cpp` and `EditorContext.cpp`; landing S0.9 first makes those merges empty.
   It touches nothing S0.8 touches. (Plan section 3 lists S0.8 first; it fixes no order for S0.9. Section 11, Q-P1.)
2. **S0.8 step 1** (VulkanPresentation), with step 2's regression proof taken first on the unchanged tree (section 5.2).
3. **S0.6**, boxed at half a day, after step 1 because both edit `VulkanGraphicsDevice.cpp` (plan section 3).
4. **S0.8 steps 2 to 6.** The lane lands through `ci/wave8-platform` when step 6 is done: review, Debug ctest with the GPU
   suites under the validation layer, Release ctest without perf, `CompatLint.py`, the windowed CTests on the reference
   machine (label `gpu`: the only runs of the surface path, section 5.1), CI green on every job.

**Never cut:** S0.8 steps 1-2, S0.9. **Cut first, in this order, if the lane runs long:** S0.8 step 5 (the build without
GLFW: steps 1-4 and 6 still hold; the docs then say the OFF build is "Later"), then step 2's OS-header owners (keep GLFW and
Vulkan's). **Below the cut:** S0.8's optional step 7 (`static_assert`s on the key layouts, section 5.7). S0.6 ends at its
box whatever it found; what it did not find is written down (section 6.4).

## 4. S0.9 The project's fixed rate: `Scene.Timing`

A project section, taken by each scene on the main thread like `Scene.CollisionLayers` (plan S0.9: not
`Physics.Simulation`, because the step drives scripts, character controllers and root motion too).

### 4.1 `S/ProjectSceneTiming.h` / `.cpp` (Scene layer, new)

```cpp
#pragma once

#include <cstdint>

namespace Strata
{

	struct SettingsSection;

	// The project's fixed rate: the project settings section Scene.Timing (registered by RegisterSceneModule), a simple
	// section whose FixedRate overrides the fixed step of every scene of the project and its exported game unless it is
	// "Scene". Each scene takes it on the main thread when it is handed over and after every change of the project's
	// settings (Scene::ApplyProjectTiming), as it takes the collision layers. Docs/DeveloperGuide.md, "Fixed rate".
	namespace ProjectSceneTiming
	{

		inline constexpr const char* c_SectionId = "Scene.Timing";
		inline constexpr const char* c_FixedRateField = "FixedRate";
		// FixedRate's first choice, the default: each scene's own SceneSettings::FixedTimestep.
		inline constexpr const char* c_SceneRate = "Scene";
		// The other choices, steps per second, stored as their names ("120").
		inline constexpr uint32_t c_Rates[] = { 30, 60, 120, 240 };
		// The most steps an update runs at a project rate is the steps of 2/15 s at that rate (GetMaxSteps), the time
		// SceneSettings::MaxFixedStepsPerFrame's default of 8 covers at 60; never more than this.
		inline constexpr uint32_t c_MaxStepsLimit = 64;

		// The section as RegisterSceneModule registers it (section 4.1 of the spec: one Enum field).
		SettingsSection MakeSection();

		struct Timing
		{
			uint32_t FixedRate = 0; // Steps per second; 0: each scene's own step
			uint64_t Version = 0;   // Changes only when FixedRate does; never 0
		};

		// The project's rate as the engine service IProjectSettings provides it (the default, 0, when the project does not
		// set the section), parsed again only when the provider's version changes. Null when nothing provides
		// IProjectSettings (tools, tests, games exported before 0.6.0): scenes keep their own steps. Main thread only; the
		// pointer is valid until the next call.
		const Timing* Find();

		// The most fixed steps one update runs at `stepsPerSecond`: ceil(stepsPerSecond * 2 / 15), from 1 to c_MaxStepsLimit
		// (4 at 30, 8 at 60, 16 at 120, 32 at 240).
		uint32_t GetMaxSteps(uint32_t stepsPerSecond);

	}

}
```

`MakeSection()`: `Id` `Scene.Timing`, `Title` "Timing", `Category` "Physics" (beside Collision Layers and Simulation, where a
person looks for the physics rate), `Scope` Project, `Version` 1, one field:

| Member | Value |
| --- | --- |
| `Name` / `Title` | `FixedRate` / "Fixed Rate" |
| `Type` / `Options` / `Default` | `Enum` / `{ "Scene", "30", "60", "120", "240" }` / `"Scene"` |
| `Description` (agents and the settings page's tooltip read it) | "Fixed steps per second of every scene: physics, character controllers, scripts' OnFixedUpdate and root motion run at it whatever the frame rate. Scene (the default) keeps each scene's own Fixed Timestep (the Inspector's scene settings, scene.setSettings; 60 unless changed). A rate here overrides it in every scene and in the exported game, and an update then runs at most 2/15 s of steps (4, 8, 16 or 32), dropping a longer backlog. Physics costs at least grow with the rate: 240 runs four times the steps of 60. 30 makes contacts soft: box3d stiffens contacts at most to an eighth of its substep rate, 15 Hz at 30 steps/s against 30 Hz at 60, so stacks sag and settle more slowly. A higher rate halves the step-to-step motion a moving character shows above the step rate, but does not remove it." |

The section's `Description`: "How often the simulation steps (Physics > Timing): the project's fixed rate overrides each
scene's Fixed Timestep unless it is Scene. Set it with project.setSettings."

This is where the plan's "30 warns about soft contacts" lives: in the field's description, which the settings page shows
and `project.settings` returns. No log warning and no command warning: a project's deliberate choice is not an incident,
and the feature test fails on unexpected warnings (section 11, Q-P7).

`Find()` mirrors `ProjectCollisionLayers::Find` (`ProjectCollisionLayers.cpp:91-112`): a file-scope cache of the provider's
version, the parsed rate and a `NextVersion` counter from 1; the value is read with `JsonUtils::GetString(*value,
c_FixedRateField, c_SceneRate)` and turned into a number by matching `c_Rates`' names (anything else, which the section
refuses before a provider holds it, is 0). `GetMaxSteps(rate)`: `std::clamp<uint32_t>((rate * 2 + 14) / 15, 1,
c_MaxStepsLimit)`.

Registration: `SceneRegistration.cpp:18` gains `SettingsRegistry::GetProcessRegistry().Register(ProjectSceneTiming::MakeSection());`
after the collision layers, with the comment "The fixed rate is Scene's: the step drives scripts and animation as well as
physics, so a build without physics keeps it."

### 4.2 `Scene`

`Scene.h`, replacing the inline accessor at 435-436 and beside it:

```cpp
		// The simulation time of one fixed step: the project's (Scene.Timing's FixedRate, taken by ApplyProjectTiming) when it
		// sets one, else SceneSettings::FixedTimestep, or 1/60 s when that is not positive. Read the step here, never from
		// GetSettings(): the project's rate wins over the scene's setting.
		float GetFixedTimestep() const
		{
			if (m_ProjectFixedRate > 0)
				return 1.0f / static_cast<float>(m_ProjectFixedRate);
			return m_Settings.FixedTimestep > 0.0f ? m_Settings.FixedTimestep : 1.0f / 60.0f;
		}
		// The most fixed steps one update runs before it drops the backlog: at the project's rate the steps of 2/15 s
		// (ProjectSceneTiming::GetMaxSteps), else SceneSettings::MaxFixedStepsPerFrame (at least 1).
		uint32_t GetMaxFixedSteps() const;
		// Steps per second the project imposes (0: the scene's own FixedTimestep applies).
		uint32_t GetProjectFixedRate() const { return m_ProjectFixedRate; }
```

and next to `ApplyProjectLayers` (590-595):

```cpp
		// Makes the project's fixed rate (ProjectSceneTiming::Find) the scene's unless it came from that value already.
		// Returns whether the rate the scene runs at changed; without a project (no IProjectSettings) nothing changes. Main
		// thread: everywhere ApplyProjectLayers is called (the scene asset's hand-over, SceneLoadRequest::TakeScene, the
		// editor's adoption of a scene and every change of the project's settings). Not saved; Copy keeps it.
		bool ApplyProjectTiming();
```

Members after `m_ProjectLayersVersion` (742): `uint32_t m_ProjectFixedRate = 0;` and `uint64_t m_ProjectTimingVersion =
0; // The ProjectSceneTiming value the rate came from (0: none)`.

`Scene.cpp`:

- `ApplyProjectTiming()` after `ApplyProjectLayers` (225): `Find()`; null or same version returns false; otherwise keep
  the old rate, store rate and version, return `old != new`.
- `GetMaxFixedSteps()`: `m_ProjectFixedRate > 0 ? ProjectSceneTiming::GetMaxSteps(m_ProjectFixedRate) : std::max(1u,
  m_Settings.MaxFixedStepsPerFrame)`.
- `Copy` (239-242) copies `m_ProjectFixedRate` and `m_ProjectTimingVersion`.
- `Scene.cpp:2174`: `const uint32_t maxSteps = GetMaxFixedSteps();`. Nothing else in the loop changes: the accumulator holds
  seconds, so a rate changed between updates carries its remainder over.
- The comments that name `MaxFixedStepsPerFrame` as the cap (`Scene.h:448-449, 456-457`) name `GetMaxFixedSteps()`.

With the project at 60 and a scene at its default, `GetFixedTimestep()` returns `1.0f / 60.0f`, the same float as
`SceneSettings::FixedTimestep`'s initializer (`Scene.h:63`), and `GetMaxSteps(60)` is 8: the run is bit for bit the
scene's own (tested, 4.8).

### 4.3 Hand-over: the call sites

`ApplyProjectTiming()` is called right after each `ApplyProjectLayers()`, with the comment updated to name both:

| Site | Change |
| --- | --- |
| `Prefab.cpp:411-412` (`SceneAsset::TakePreparedScene`) | `scene->ApplyProjectTiming();` |
| `Prefab.cpp:427` (`SceneAsset::CreateScene`) | the same |
| `SceneLoader.cpp:325-328` (`SceneLoadRequest::TakeScene`) | `m_Prepared->ApplyProjectTiming();` |
| `EditorContext.cpp:549-554` (`ApplyProjectLayers`: the edited and the running scene) | `ApplyProjectTiming()` on both; the function keeps its name (a rename would touch lines `physics-timing` and lane Editor edit); its comment (`EditorContext.h:213-216`) says "the project's collision layers and fixed rate" |
| `EditorContext.cpp:556-558` (`AdoptProjectLayers`) | `scene.ApplyProjectTiming();` before the layers |

`ProjectSettingsAction.cpp:75-77` already calls `EditorContext::ApplyProjectLayers()` after every change, undo and redo:
its comment becomes "The scenes take a changed collision layer table and fixed rate now, the running game's too". So
`project.setSettings` changes a running game's rate from its next update, as the command's description promises
(`EditorProjectSettingsCommands.cpp:263-264`).

### 4.4 The readers (table 2.3)

- `GameRuntime.cpp:38`: `const float maxSteps = static_cast<float>(scene.GetMaxFixedSteps());`; the comment at 28-31 and
  `GameRuntime.h:39-44, 101-102` say "its scene's most steps per update (`Scene::GetMaxFixedSteps`)".
- `ScriptHostAPI.cpp:325-326`: `return system->GetScene().GetFixedTimestep();` (the host function keeps its signature: no
  ABI change).
- `EditorInputCommands.cpp:98`: `const float fixedStep = context.GetActiveScene()->GetFixedTimestep();` in both modes: while
  not playing the active scene is the edited scene, whose step (with the project's rate) is the one play will run, where
  1/60 was right only for default scenes (section 11, Q-P6). The schema texts (74-77): "rounded up to whole fixed steps of
  the scene's step (`scene.info`'s `settings.fixedRate.fixedTimestep`)" and "which may run up to the scene's
  `maxFixedSteps` - 1 more".
- `EditorWorldCommands.cpp:421`: `world->GetEffectiveContactHertz(scene->GetFixedTimestep())`.
- `InspectorPanel.cpp:256-262`: `DescribePhysics(const SceneSettings& settings, uint32_t projectRate)`: steps per second
  `projectRate` when non-zero, else from `settings.FixedTimestep`; the text "Gravity 9.81 m/s² · 120 steps/s (project)" when
  the project sets the rate.

### 4.5 Commands

No new command (plan S0.9: `project.setSettings`).

- `DescribeSettings` (`EditorSceneCommands.cpp:38-52`) becomes `DescribeSettings(const SceneSettings& settings, const Scene&
  scene)` (callers 328, 633) and adds `"fixedRate": { "stepsPerSecond": 1 / GetFixedTimestep() rounded to 0.001,
  "fixedTimestep": GetFixedTimestep(), "maxFixedSteps": GetMaxFixedSteps(), "source": "project" | "scene" }`; `fixedTimestep`
  and `maxFixedStepsPerFrame` stay the scene's stored values.
- `scene.info`'s description (314-317) adds: "settings.fixedRate is the step the active scene runs: source project when the
  project's Scene.Timing FixedRate sets it (project.setSettings), scene for its own fixedTimestep."
- `scene.setSettings` (633-647): when `scene.GetProjectFixedRate() > 0` and the call set `fixedTimestep` or
  `maxFixedStepsPerFrame`, the result's `warning` gains (joined with `"; "` to the warnings there): "The project's fixed rate
  (Scene.Timing, <n> steps/s) overrides fixedTimestep and maxFixedStepsPerFrame; they apply when its FixedRate is Scene".
  The values are still stored (one undo step as today). Its description adds the same sentence.

### 4.6 The Inspector

- `InspectorPanel.h:151-156`: `PhysicsSummary` gains `uint32_t ProjectRate = 0;` and `std::string InactiveReason;`; the
  cache test at `InspectorPanel.cpp:1294` compares `scene.GetProjectFixedRate()` too.
- The draw lambda (1305-1317) takes `std::string_view inactiveReason` and passes `UI::PropertyLabelOptions {
  .InactiveReason = inactiveReason }` to `UI::PropertyLabel`, and wraps the widget in `ImGui::BeginDisabled(!inactiveReason.empty())`
  as component properties with a reason are (`InspectorPanel.cpp:963-987`).
- The Fixed Timestep and Max Steps per Frame rows (1323-1326) get the cached reason when the project sets the rate: "The
  project's Fixed Rate (Project Settings > Physics > Timing: <n> steps/s) overrides it; it applies when the Fixed Rate is
  Scene". The probes then record it under `Inspector.Scene.FixedTimestep.Inactive` and
  `Inspector.Scene.MaxFixedStepsPerFrame.Inactive` (`PropertyWidgets.h:84-88`).
- `DrawFixedRateLink(EditorPanelContext&)`, declared beside `DrawCollisionLayersLink` (`InspectorPanel.h:137`) and called
  after it (1340): the text "Fixed rate: the project's, <n> steps/s" or "Fixed rate: the scene's, <x> steps/s" (both
  numbers formatted `{:.4g}` from `1 / GetFixedTimestep()`, as `DescribePhysics` formats them at `InspectorPanel.cpp:261`;
  probe `Inspector.FixedRate`, recorded like `Inspector.Layers` at 1381) and `UI::LinkButton("Inspector.FixedRate.Open", "Project
  Settings", Icons::ExternalLink, "Choose the project's fixed rate")`, which calls
  `Shell->ShowProjectSettings(ProjectSceneTiming::c_SectionId)`. Theme tokens and the widget kit only (AGENTS.md "Editor").

The settings window draws the section with its generic page (simple sections, DG "Settings pages"): no page of its own.

### 4.7 The feature test

- `FeatureTest.stproj` (after `Scene.CollisionLayers`, keys sorted as the writer sorts them): `"Scene.Timing": { "Value":
  { "FixedRate": "60" }, "Version": 1 }`. Non-default, and the smallest change of behavior: the runner advances 1/60 s per
  frame (`TimeFeatures.cpp:8`), so the feature scene, which stores 0.02 s, now runs one step per frame.
- `TimeFeatures.cpp`: the header comment says "the scene stores a 0.02 s step; the project's Scene.Timing sets 60 steps/s,
  which wins"; line 25 becomes `Expect(Near(Time::GetFixedDeltaTime(), 1.0f / 60.0f), "the project's fixed rate (Scene.Timing)
  overrides the scene's 0.02 s step")`; line 52 checks `Near(fixedDeltaTime, 1.0f / 60.0f) && fixedDeltaTime ==
  Time::GetFixedDeltaTime()`. Without the `ScriptHostAPI.cpp` change, `GetFixedDeltaTime` returns 0.02 while `OnFixedUpdate`
  receives 1/60: both checks and `LifecycleFeatures.cpp:44` fail.
- The implementer runs `StrataTests.FeatureTest` (both suites) and reads every feature script's expectation that could
  depend on 50 steps/s; one that does is changed with its reason in the commit message (section 11, risk 5).

### 4.8 Tests

New file `T/Scene/SceneTimingTests.cpp`, `TEST_SUITE("Scene.Timing")` (shard Core.Scene by `Scene*`; `physics-timing` adds
its cases to this suite later). Providers as `TestProject` in `T/Physics/PhysicsLayerLoadTests.cpp:33-53` makes them (that
file is left out of builds without physics, so the helper is written again here, not included): a local `SettingsRegistry`
with `ProjectSceneTiming::MakeSection()`, a `ProjectSettingsValues` and an `EngineServices::ScopedProvider<IProjectSettings>`
over it (`T/Core/SettingsTestUtils.h` holds only test sections, no provider). Steps counted with `Scene::GetFixedStepCount`
after `OnRuntimeStart`.

| Case | Checks | Fails without |
| --- | --- | --- |
| "The project's fixed rate overrides the scene's step and scales the steps one update may run" | A scene at 0.02 s and 8 steps; the project at "240": `ApplyProjectTiming()` true, `GetFixedTimestep() == 1.0f / 240.0f`, `GetMaxFixedSteps() == 32`; an update of 0.1 s runs at least 23 steps and drops none; an update of 0.25 s runs 32 and drops some | S0.9 (the scene's 0.02 s: at most 5 steps, cap 8) |
| "Scene keeps each scene's own step, and a reset gives it back" | "Scene" leaves 0.02 s and 8; "120" then "Scene" again restores them; `ApplyProjectTiming()` returns whether the rate changed | S0.9 |
| "A program without project settings keeps the scene's step" | No provider: `ApplyProjectTiming()` false, 0.02 s, 8 | (guards the null path) |
| "A copy keeps the project's rate, and a changed section reaches a running scene at its next update" | `Scene::Copy` keeps 120; the provider changes to 30 while the copy runs; after `ApplyProjectTiming()` the next update steps at 1/30 with a cap of 4 | `Copy` not copying the members |
| "FixedRate 60 changes nothing in a default scene" | Two default scenes, one taking "60": 1,000 updates of 1/144 s give equal step counts, dropped counts and `GetTime()` exactly | a rate computed differently from the default |
| "The section offers named rates and refuses others" | `Validate`: "Scene", "30", "60", "120", "240" pass; `120` (a number), `"100"` and `"scene"` fail naming `FixedRate`; the default is "Scene"; the field's description contains "soft" and "30" | the section |
| "Every hand-over takes the project's fixed rate, also a rate changed while the scene loaded" | Modelled on `Physics.Layers`' "A scene loaded through SceneLoader takes the project's table at hand-over..." (`PhysicsLayerLoadTests.cpp:135-177`): a scene through `SceneLoader` takes "120"; a change to "30" during the load reaches it at `TakeScene`; `SceneAsset::CreateScene` takes it too | a missing call site of 4.3 |

`Runtime.Game` (`T/Runtime/GameRuntimeTests.cpp`, outside `#if ST_WITH_PHYSICS` so the module-less CI row runs it): "A game
at 240 steps per second keeps every step of long frames". `WriteGame` with `MakeScene("Start", 10)`, then
`RewriteManifest(manifest, 5, { { "Scene.Timing", { { "Version", 1 }, { "Value", { { "FixedRate", "240" } } } } } })`:

- default options: `Update(Timestep(0.1f))` runs at least 23 steps, `GetDroppedFixedStepCount()` unchanged;
- `KeepEveryFixedStep`: `Update(Timestep(0.25f))` runs at least 31 and at most 32 steps, drops none, and
  `GetPlayedSeconds()` grows by the steps counted times `1.0f / 240.0f` (approx). Not "exactly 32": the limit is exactly
  32 steps of time, but the accumulator is a float and loses a few ulps over 32 subtractions, so the 32nd step falls due
  at the next update. A float32 model of `Scene.cpp:2173-2215` and `LimitToFixedSteps` that gives today's 8 steps at 60
  (the case at 265-341) gives 31 at 240 and 16 at 120 for every start remainder tried. A second `Update(Timestep(0.25f))`
  then runs at least 32 steps, so two updates together run at least 63.

Without S0.9 the manifest's section is claimed by no module and not provided, the scene steps at 1/60 and the long frame
runs 8 steps (6 at 0.1 s): both bounds fail.

`Editor.Commands` (`T/Editor/EditorCommandTests.cpp`, beside "project.setSettings tunes the running game's physics..." at
1657-1701, whose harness and steps it follows): "project.setSettings sets the project's fixed rate: scenes step at it at
once, and undo gives the scene's own back". In this order:

1. `project.create` in `Tests::CreateTemporaryDirectory`; `Tests::CreateGround` and `Tests::CreateDynamicBox` in the edited
   scene (`physics.stats` reports `settings` only while a physics world exists).
2. `scene.setSettings {fixedTimestep: 0.02}`; `project.setSettings {section: "Scene.Timing", value: {FixedRate: "120"}}`.
3. `scene.info`: `settings.fixedRate` is `{stepsPerSecond: 120, fixedTimestep: 1/120 (approx), maxFixedSteps: 16, source:
   "project"}`; `settings.fixedTimestep` is still 0.02 (the stored value).
4. `scene.setSettings {fixedTimestep: 0.01}` succeeds, its `warning` contains "Scene.Timing"; `settings.fixedTimestep` 0.01.
5. `play.start`, `harness.Context.Update(Timestep(1.0f / 60.0f))`: the running scene's `GetFixedTimestep() == 1.0f / 120.0f`
   and `GetMaxFixedSteps() == 16`.
6. Still playing, `project.setSettings {... FixedRate: "30"}` and one more update: the running scene's step is 1/30, and
   `physics.stats`' `settings.effectiveContactHertz` is 15 (approx; `min(30, 0.125 * 4 * 30)`). Without the reader at
   `EditorWorldCommands.cpp:421` it is 30: the copy's stored 0.01 s gives `min(30, 50)`.
7. `play.stop`, then `edit.undo` three times (the rate 30, the scene's 0.01, the rate 120): `scene.info`'s
   `settings.fixedRate` has `source: "scene"` and `stepsPerSecond: 50`, `settings.fixedTimestep` is 0.02, and the project
   holds no `Scene.Timing` value.

`Editor.Input` (`T/Editor/EditorInputTests.cpp`, beside the timed tap at 522-523): "Durations in seconds count in the
project's fixed rate". The project at "120": while playing, `input.key {key: "Space", seconds: 0.1, wait: false}` returns
`steps: 12` (6 without the reader at `EditorInputCommands.cpp:98`); not playing, the same call also returns 12 (6 before:
the 1/60 constant).

`Editor.UI.Inspector` (`T/Editor/EditorAuthoringUITests.cpp:1022`): "The scene's step rows are inactive while the project
sets the fixed rate". With the project at "120" and nothing selected, the test opens the Physics section first (its header
`Inspector.Physics` starts closed, `InspectorPanel.cpp:1291-1292, 1318`): the probes `Inspector.Scene.FixedTimestep.Inactive`
and `Inspector.Scene.MaxFixedStepsPerFrame.Inactive` hold the reason, the header's summary ends "120 steps/s (project)",
`Inspector.FixedRate` reads "Fixed rate: the project's, 120 steps/s"; at "Scene" neither row is inactive and the link reads
"Fixed rate: the scene's, 60 steps/s".

The feature test (4.7) covers `ScriptHostAPI.cpp`.

### 4.9 Docs

- DG: a new `### Fixed rate` after "Physics simulation" (`DG:1168`), under "Project settings": the section and its field,
  precedence (project over scene, none without a provider), the 2/15 s cap and its table, the hand-over (the collision
  layers' call sites, `Copy`), "read the step through `Scene::GetFixedTimestep` and `GetMaxFixedSteps`, never
  `SceneSettings`", what each command reports, the Inspector's inactive rows, and that a rate does not cure the jitter of a
  moving character (drawing between steps does; `physics-timing`). "Architecture rules", Physics tuning, states the reader
  rule in full (checklist item 5). "Simulated input" (`DG:3347`; its lines 3376-3381) and the runtime's row (`DG:132`) name
  the scene's step and `GetMaxFixedSteps` instead of `FixedTimestep` and `MaxFixedStepsPerFrame`.
- `Docs/Architecture.md`: the frame diagram (992: "at most `GetMaxFixedSteps()`: `MaxFixedStepsPerFrame`, 8, or 2/15 s at
  the project's rate"), time dilation (1037), `SceneSettings` (1040-1046: the project's rate overrides the step and the cap),
  the runtime's `--seconds` paragraph (4081).
- `.claude/skills/strata-editor-automation/SKILL.md:595`: "`maxFixedSteps - 1` steps late (`scene.info`'s
  `settings.fixedRate`)".
- `Docs/Manual/Settings.md`: after Simulation (37), "**Timing** (under Physics): **Fixed Rate** ...", the description's
  words for people, with the soft-contact warning at 30.
- `Docs/Roadmap.md:907-908`: "the fixed timestep and gravity as project settings" becomes "gravity as a project setting (the
  fixed rate is the project's `Scene.Timing` since 0.8.0)".
- `.claude/skills/strata-scripting/SKILL.md:138`: `GetFixedDeltaTime` "(the project's `Scene.Timing` rate when it sets one)".
- AGENTS.md: section 9.

### 4.10 Commits

Every commit passes every test (AGENTS.md, golden rule 2), so the readers move first and the section comes with the feature
test's value: a registered section left at its default fails `StrataTests.FeatureTest` (DG "Feature test"), and
`TimeFeatures.cpp`'s new checks need the `ScriptHostAPI.cpp` reader.

1. "Read the fixed step and its cap through the scene everywhere" (`Scene::GetMaxFixedSteps` returning the scene's own cap,
   the readers of 4.4 and the comments of table 2.3; `Editor.Input`'s not-playing half, which this commit already changes:
   a scene stored at 0.02 s gives `steps: 5` for 0.1 s where 1/60 gave 6).
2. "Add the project's fixed rate: Scene.Timing overrides each scene's step" (`ProjectSceneTiming`, the members and
   `ApplyProjectTiming`, hand-overs, the commands of 4.5, the `Scene.Timing` suite, `Runtime.Game`, `Editor.Commands`, the
   rest of `Editor.Input`, `FeatureTest.stproj` and `TimeFeatures.cpp`).
3. "Show the project's fixed rate in the Inspector" (4.6 and its UI test).
4. "Document the project's fixed rate" (4.9, AGENTS.md's S0.9 words).

## 5. S0.8 GLFW sealed

### 5.1 Step 1: the device presents through `VulkanPresentation`

**`P/Vulkan/VulkanPresentation.h`** (new, App layer):

```cpp
#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Strata
{

	// What a window system gives the Vulkan graphics device to present into one of its windows (Window::
	// GetVulkanPresentation): the instance extensions its surfaces need, whether a queue family can present to it, and a
	// surface for the window. The window backend implements it (Platform/GLFW), so the device names no window system.
	// Called on the main thread while the window lives.
	class VulkanPresentation
	{
	public:
		virtual ~VulkanPresentation() = default;

		// Appends the instance extensions the window system's surfaces need (VK_KHR_surface and the platform's) to
		// `outExtensions`; false, appending nothing, when it cannot present through Vulkan on this system (no loader, no
		// surface extension).
		virtual bool GetRequiredInstanceExtensions(std::vector<std::string>& outExtensions) const = 0;
		// Whether `queueFamily` of `physicalDevice` can present to the window system's windows.
		virtual bool SupportsPresentation(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t queueFamily) const = 0;
		// A surface for the window, which the caller destroys (vkDestroySurfaceKHR), or the error that prevented it.
		virtual VkResult CreateSurface(VkInstance instance, VkSurfaceKHR& outSurface) const = 0;
	};

}
```

**`Layers.json`**: App's `Paths` (22) add `"Platform/Vulkan/VulkanPresentation.h"`; Renderer's `Exclude` (46) adds it; App's
`Notes` (21): "...the window and its GLFW implementation. VulkanLoader and VulkanPresentation belong here, not to Renderer:
the window system implements them (GLFW initializes its Vulkan loader from VulkanLoader and creates surfaces for the
device through VulkanPresentation), and they depend only on Core."

**`C/Window.h`**:

- Before the class, `class VulkanPresentation; // Platform/Vulkan/VulkanPresentation.h` (the only Vulkan name in the file).
- The class comment (72-73) becomes "Platform window. The graphics device owns the swapchain and presents through the
  window's VulkanPresentation...".
- `GetNativeWindow()` (125) is deleted; in its place:

```cpp
		// What the Vulkan graphics device presents into this window through (VulkanPresentation); null for a window that
		// cannot present through Vulkan (a test's stand-in, another API's window): a device made for it renders offscreen.
		virtual const VulkanPresentation* GetVulkanPresentation() const { return nullptr; }
```

- `Create`'s comment: "Opens a window of the platform's window system (GLFW; none in a build without it,
  Platform/NoWindow). Null, with the reason logged, when it cannot be opened." (null returns arrive in step 3).

**`P/GLFW/GLFWWindow.h` / `.cpp`**: `class GLFWWindow : public Window, private VulkanPresentation`; `#include
"Platform/Vulkan/VulkanPresentation.h"`; `GetNativeWindow()` (`.h:46`) deleted; `const VulkanPresentation*
GetVulkanPresentation() const override { return this; }`; the three overrides, private, in `GLFWWindow.cpp` (which
includes `VulkanLoader.h` before `glfw3.h`, so GLFW declares its Vulkan functions, `.cpp:12`):

```cpp
	bool GLFWWindow::GetRequiredInstanceExtensions(std::vector<std::string>& outExtensions) const
	{
		uint32_t count = 0;
		const char** names = glfwGetRequiredInstanceExtensions(&count);
		if (!names)
			return false; // GLFW reported why through its error callback
		outExtensions.insert(outExtensions.end(), names, names + count);
		return true;
	}

	bool GLFWWindow::SupportsPresentation(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t queueFamily) const
	{
		return glfwGetPhysicalDevicePresentationSupport(instance, physicalDevice, queueFamily) == GLFW_TRUE;
	}

	VkResult GLFWWindow::CreateSurface(VkInstance instance, VkSurfaceKHR& outSurface) const
	{
		return glfwCreateWindowSurface(instance, m_Window, nullptr, &outSurface);
	}
```

The three calls are today's, one for one (risk 1).

**`R/GraphicsDevice.h`**: `GraphicsDeviceSpecification` gains, after `Headless` (24):

```cpp
		// The window the device presents into (CreateSwapchain): its VulkanPresentation decides the instance extensions and
		// the queue family. A device that is not headless without a window that can present renders offscreen instead.
		Window* PresentationWindow = nullptr;
```

and the class gains, beside `HasSwapchain` (147): `// Whether CreateSwapchain may be called: the device was made for a
window it can present into (not headless).` `virtual bool CanPresent() const = 0;`.

**`P/Vulkan/VulkanGraphicsDevice.cpp`**:

- 20-21 deleted (`GLFW_INCLUDE_NONE`, `<GLFW/glfw3.h>`); `#include "Platform/Vulkan/VulkanPresentation.h"` after
  `VulkanLoader.h` (6).
- Member `const VulkanPresentation* m_Presentation = nullptr;` beside `m_Window` (410). The constructor (255-259) resolves it:
  when not headless, `m_Presentation = m_Specification.PresentationWindow ? m_Specification.PresentationWindow->
  GetVulkanPresentation() : nullptr`; if null, `ST_CORE_INFO("Graphics device: the window cannot present through Vulkan;
  rendering offscreen only")` and `m_Specification.Headless = true`. Info, not a warning: it is the defined behavior of such
  a window, and the GPU helper of step 4 must log no warning.
- `CanPresent() const override { return !m_Specification.Headless; }`.
- `CreateInstance` (477-488): `std::vector<std::string> extensions; if (!m_Presentation->GetRequiredInstanceExtensions(extensions))
  { ST_CORE_ERROR("The window system reports no Vulkan presentation support on this system"); return false; }` then appended
  to `m_InstanceExtensions`.
- `SelectPhysicalDevice` (699): `!m_Presentation->SupportsPresentation(m_Instance, physicalDevice, family)`.
- `CreateSurface` (1033-1041): `const VulkanPresentation* presentation = m_Window->GetVulkanPresentation(); if (!presentation)
  { ST_CORE_ERROR("The window cannot present through Vulkan"); return false; }` then `const VkResult result =
  presentation->CreateSurface(m_Instance, m_Surface);`, the rest unchanged.

**`R/GraphicsModule.cpp`** (31-43): `deviceSpecification.PresentationWindow = m_Window;` beside `Headless`; the swapchain is
created only `if (started && m_Window && m_Device->CanPresent())`. **`ImGuiModule.cpp:28`** adds `||
!m_Graphics->GetDevice()->CanPresent()` to its refusal, with the message "The ImGui module needs a window the Graphics
module presents into": the one combination nothing tests (a window that cannot present, with ImGui) is refused instead of
drawn into a missing back buffer.

**Build**: `Strata/CMakeLists.txt:64` drops `glfw`: `strata_layer_link(Renderer PRIVATE nvrhi_vk Vulkan::Headers stb
MikkTSpace meshoptimizer)`. From then on the compiler refuses `<GLFW/glfw3.h>` in the Renderer's sources (no include path).

**Test fakes**: the two `GetNativeWindow()` overrides (`ImGuiPlatformTests.cpp:57`, `ScriptShellTests.cpp:102`) are deleted
here, so that the commit compiles; step 4 replaces the fakes.

**Checks in this commit**: first, as a quick check, `ctest --preset windows-debug -R
"StrataEditor\.(Smoke|QuitScreenshot|LauncherSmoke)$"`; then the full Debug ctest with the `gpu` label. Every windowed
CTest creates a surface, not only the editor's two: `StrataEditor.Smoke`, `.QuitScreenshot` and `.LauncherSmoke`
(`StrataTests/CMakeLists.txt:387-407`), the exported game's windowed, resolution, quality, perf-log and no-camera runs
(`:643-695`), the feature game's windowed run (`:741`) and the samples' windowed runs (`:833` on). They are the only runs of
the surface path anywhere: hosted CI has no GPU.

### 5.2 Step 2: `ExternalOwners` in the layer table

**Regression proof first.** Before step 1 is committed, on the unchanged tree with step 2's analyzer and table applied, run
`StrataTests --test-case="The engine's includes follow the layer table"` and keep its output: it must report exactly
`Platform/Vulkan/VulkanGraphicsDevice.cpp(21): includes GLFW/glfw3.h, but only Platform/GLFW/** may include GLFW/*` and
nothing else. The output goes into step 2's commit message. Step 1 is then committed, step 2 after it, so each commit
passes its tests.

**`Layers.json`**, a member after `Layers`, and the `Description` (2) gains: "ExternalOwners names third-party and system
headers only some folders may include, whatever their layer: an include whose path starts with one of an entry's Prefixes
(ignoring case) from a file none of its Paths matches is a violation (the window system, the graphics API and the OS stay in
their Platform folders)."

```json
"ExternalOwners": [
	{ "Prefixes": [ "GLFW/" ], "Paths": [ "Platform/GLFW/**" ],
	  "Notes": "The window backend. Everything else reaches windows through Core/Window.h, and the Vulkan device presents through Platform/Vulkan/VulkanPresentation.h" },
	{ "Prefixes": [ "vulkan/", "nvrhi/vulkan" ], "Paths": [ "Platform/Vulkan/**" ],
	  "Notes": "The graphics backend. The renderer uses Renderer/GraphicsDevice.h and NVRHI's API-neutral headers" },
	{ "Prefixes": [ "Windows.h", "winsock2.h", "ws2tcpip.h", "mstcpip.h", "shlobj.h", "shellapi.h", "psapi.h", "bcrypt.h", "aclapi.h", "DbgHelp.h", "io.h", "malloc.h" ],
	  "Paths": [ "Platform/Windows/**" ], "Notes": "The Windows implementations of Core's interfaces" },
	{ "Prefixes": [ "unistd.h", "sys/", "pthread.h", "dlfcn.h", "poll.h", "spawn.h", "sched.h", "ucontext.h", "elf.h", "netdb.h", "netinet/", "arpa/", "mach/", "mach-o/", "libproc.h", "crt_externs.h" ],
	  "Paths": [ "Platform/Posix/**" ], "Notes": "The Linux and macOS implementations of Core's interfaces" },
	{ "Prefixes": [ "fcntl.h" ], "Paths": [ "Platform/Windows/**", "Platform/Posix/**" ], "Notes": "File descriptors, on both" }
]
```

`objc/` (the Dock icon's Objective-C runtime in `Platform/GLFW`) and `TargetConditionals.h` (`Core/PlatformDetection.h`)
stay unowned: owning them would describe where they happen to be, not a seam.

**`LayeringAnalyzer.h`**:

```cpp
	// Third-party and system headers only some files may include, whatever their layer (Layers.json "ExternalOwners"): an
	// include whose path, resolved from the source root as MayNotIncludeExternal's prefixes are matched, starts with one of
	// Prefixes ignoring ASCII case (Windows' file systems ignore it: <windows.h> is <Windows.h>), from a file that none of
	// Paths matches.
	struct ExternalOwner
	{
		std::vector<std::string> Prefixes;
		std::vector<std::string> Paths; // Globs relative to the source root
	};
```

`LayerTable` gains `std::vector<ExternalOwner> ExternalOwners;` (after `MaxAllowlistEntries`). `Violation` gains, last
(aggregate initializations elsewhere stay valid), `std::vector<std::string> Owners; // External headers with owners: the
Paths that may include them (empty for a layer's MayNotIncludeExternal)`. The `CheckLayering` comment (94-98) says owners are
checked before the layer's prefixes.

**`ParseLayerTable`** (`LayeringAnalyzer.cpp:301-350`): an optional `ExternalOwners` array; each entry an object with a
non-empty `Prefixes` array and a non-empty `Paths` array of non-empty strings (read with `ReadStringArray`); `Notes` is
ignored. Errors: "ExternalOwners entry <i> needs 'Prefixes' and 'Paths'", "ExternalOwners: the prefix '<p>' is listed twice"
(equal ignoring case) and "ExternalOwners: the prefix '<p>' starts with '<q>'" (one prefix of another, in either entry: the
owner of an include must be unique).

**`CheckLayering`** (456-466): the branch for an include with no engine layer becomes:

1. `external` as today. If an owner has a prefix `external` starts with (ASCII case-insensitive, a helper
   `StartsWithIgnoringCase` beside `MatchesAny`), count the include in `CheckedIncludes`; if `MatchesAny(owner.Paths, path)` is
   false, the violation is `{ path, include.Line, external, from.Name, <the matched prefix>, true, owner.Paths }`.
2. Otherwise, or when the owner allows it, the layer's `MayNotIncludeExternal` as today (an owned header in its folder still
   meets its layer's prefixes).

Allowlist entries cover owner violations as they cover the others (`allowlistUse`, 476-485): `MaxAllowlistEntries` stays 0.

**`Describe`** (497-506): with `Owners`, `"{file}({line}): includes {header}, but only {owners joined with ' and '} may
include {prefix}*"`.

**Tests**, `Architecture.Layering` (`LayeringTests.cpp`):

| Case | Checks | Fails without |
| --- | --- | --- |
| "Headers with owners are reported outside them, in either include form and any case" | Two-layer table with `{ Prefixes { "GLFW/" }, Paths { "base/glfw/**" } }`: `base/glfw/w.cpp` including `<GLFW/glfw3.h>` passes; `base/r.cpp` with `"GLFW/glfw3.h"`, `top/t.cpp` with `<glfw/glfw3native.h>` and `base/glfw2/x.cpp` with `<GLFW/glfw3.h>` are reported, with the `Describe` text; `<GLFWx.h>` and `<glm/glm.hpp>` pass; an owned header in its folder that the layer's `MayNotIncludeExternal` forbids is reported as the layer's; an allowlist entry covers an owner violation | the analyzer |
| "External owners name each prefix once, with paths" | `ParseLayerTable`: an entry without `Paths`, a duplicate prefix (other case), a prefix of another prefix are refused with their messages; a valid table round-trips | the parser |
| "0.7.0's GLFW include in the Vulkan device is reported" | Like "A forbidden include is reported with its file and line" (168-196): the engine's sources with `#include <GLFW/glfw3.h>` appended to `Platform/Vulkan/VulkanGraphicsDevice.cpp`: exactly one violation, its line and text | the table's GLFW entry |
| "The window system, the graphics API and the OS stay in their platform folders" | Modelled on "The asset layer knows no graphics API" (487-520): the table holds the GLFW, Vulkan, Windows and POSIX entries with the paths above; whatever the table says, no engine file outside `Platform/GLFW/` includes a header starting with `GLFW/` (any case), none outside `Platform/Vulkan/` one starting with `vulkan/`, and no allowlist line names such a header | the table |

The tree check (113-135) fails on today's tree (the regression proof) and passes after step 1.

### 5.3 Step 3: window creation can fail, and can be replaced

**`GLFWWindow`**: the constructor becomes `GLFWWindow() = default;` and the work moves into `bool Open(const
WindowSpecification& specification)` (public, called only by `Window::Create`):

```cpp
	Scope<Window> Window::Create(const WindowSpecification& specification)
	{
		Scope<GLFWWindow> window = CreateScope<GLFWWindow>();
		if (!window->Open(specification))
			return nullptr; // GLFW's error callback and Open logged why
		return window;
	}
```

In `Open`, the two `ST_CORE_VERIFY`s (65, 99) become failures:

- `glfwInit()` failing (only when `s_GLFWWindowCount == 0`): `ST_CORE_ERROR("The window system could not be initialized
  (GLFW): no display, or none GLFW supports")`, return false.
- `glfwCreateWindow` failing: `ST_CORE_ERROR("The window '{}' ({} x {}) could not be created", ...)`; if
  `s_GLFWWindowCount == 0`, `glfwTerminate()` (GLFW was initialized for this window alone); return false.
- `s_GLFWWindowCount++` only after a window exists (100, as today).

The destructor (294-305) starts with `if (!m_Window) return; // Never opened: no window and no share of GLFW's
initialization`.

**`ApplicationSpecification`** (`Application.h:25-43`), after `Window`:

```cpp
		// Makes the window instead of Window::Create (empty: Window::Create): tests run the frame loop on a stand-in
		// (Tests::StubWindow), an embedder its own window. Null is a window that could not be opened.
		std::function<Scope<Strata::Window>(const WindowSpecification&)> WindowFactory;
```

(`Strata::Window` because the member `Window` above names the specification.)

**`Application::Application`** (`Application.cpp:84-92`):

```cpp
		bool windowOpened = true;
		if (!m_Specification.Headless)
		{
			WindowSpecification windowSpecification = m_Specification.Window;
			if (windowSpecification.Title.empty())
				windowSpecification.Title = m_Specification.Name;
			m_Window = m_Specification.WindowFactory ? m_Specification.WindowFactory(windowSpecification) : Window::Create(windowSpecification);
			if (m_Window)
			{
				m_Window->SetEventCallback(ST_BIND_EVENT_FN(OnEvent));
				Input::SetWindow(m_Window.get());
			}
			else
			{
				// The window backend logged why (GLFW's error, or a build without a window backend).
				ReportFatalError(m_Specification.Name, fmt::format("{} cannot start: its window could not be opened.", m_Specification.Name),
					"It needs a desktop session with a display; to run without a window, start it with --headless.");
				windowOpened = false;
			}
		}
		Input::Reset();

		if (!windowOpened || !StartModules())
		{
			m_Running = false;
			m_ExitCode = 1;
		}
```

This replaces `Application.cpp:84-99` whole. `Input::Reset()` still runs on the failure path (an early `return` would skip
it, and tests run several applications in one process). No module starts; the destructor shuts down the job system as
after any failed start (`ShutDownModules` with none started, `Application.cpp:150-165`). Both programs with windows accept `--headless` (DG "Windows subsystem"), so the advice holds for
them; DG's list of fatal startup failures (`DG:309-311`) adds "no window".

### 5.4 Step 4: `Tests::StubWindow` and `Core.WindowBackend`

**`T/StubWindow.h` / `.cpp`** (beside `TestHelpers.h`, in every build):

```cpp
namespace Strata::Tests
{

	// A Window without a window system: it records what is asked of it in public members, and ProcessEvents delivers what a
	// test queued, in order, as GLFWWindow does: into Input (Input::Process*) and then through the event callback. Screen
	// coordinates are pixels unless ContentScale is set. It cannot present (GetVulkanPresentation is null), so a device made
	// for it renders offscreen. Replaces the tests' own fake windows.
	class StubWindow final : public Window
	{
	public:
		void QueueKey(KeyCode key, bool down, bool repeat = false); // KeyPressed (repeat: no Input change) or KeyReleased
		void QueueText(uint32_t codepoint);                          // KeyTyped
		void QueueMouseButton(MouseCode button, bool down);
		void QueueMouseMove(glm::vec2 position);
		void QueueScroll(glm::vec2 offset);
		void QueueMouseLeave();
		void QueueResize(uint32_t width, uint32_t height);           // Width, Height and Framebuffer; WindowResize
		void QueueFocus(bool focused);                               // Focused; WindowFocus or WindowLostFocus
		void QueueContentScale(float scale);
		void QueueFileDrop(std::vector<std::filesystem::path> paths);
		void QueueClose();                                           // WindowClose
		// What every ProcessEvents reports for a gamepad (Input::ProcessGamepad for each of c_MaxGamepads, as GLFW's backend
		// polls them); nullopt, the default, is disconnected.
		struct GamepadState
		{
			std::array<float, c_GamepadAxisCount> Axes {};
			std::array<bool, c_GamepadButtonCount> Buttons {};
		};
		void SetGamepad(uint32_t gamepad, std::optional<GamepadState> state);
		// Called at the start of every ProcessEvents with its index from 0, before the queue is delivered: per-frame scripts
		// (minimize for some frames, queue a close at the third).
		std::function<void(StubWindow&, uint32_t call)> OnProcessEvents;

		// Window: every pure virtual, reading and writing the members below. Every setter counts every call, also one that
		// sets the value it already has, as the fakes it replaces do: tests prove that their code calls the window only on a
		// change. SetResolution(w, h) with both non-zero sets Width, Height and Framebuffer (scaled by ContentScale) and
		// counts ResolutionChanges; SetVSync counts VSyncChanges; SetMode counts ModeChanges; SetCursorShape counts
		// ShapeChanges; Maximize counts MaximizeCalls; SetIcon counts IconSets; GetLayoutKey reads Layout.

		uint32_t Width = 1280;
		uint32_t Height = 720;
		glm::uvec2 Framebuffer = glm::uvec2(1280, 720);
		float ContentScale = 1.0f;
		bool Minimized = false;
		bool Focused = true;
		bool VSync = true;
		int VSyncChanges = 0;
		std::string Title;
		WindowMode Mode = WindowMode::Windowed;
		int ModeChanges = 0;
		int ResolutionChanges = 0;
		CursorMode Cursor = CursorMode::Normal;
		CursorShape Shape = CursorShape::Arrow;
		int ShapeChanges = 0;
		std::string Clipboard;
		std::map<KeyCode, KeyCode> Layout; // Keys the layout names otherwise
		void* Handle = nullptr;            // GetPlatformHandle
		int MaximizeCalls = 0;
		int IconSets = 0;
		uint32_t ProcessEventsCalls = 0;
	};

}
```

`ImGuiPlatformTests.cpp:22-70` (`TestWindow`) and `ScriptShellTests.cpp:63-111` (`CountingWindow`) are deleted and their
tests use `Tests::StubWindow`, with every assertion unchanged:

- The ImGui tests set `Handle` to their own target where they read it (`TestWindow`'s default). Their checks that the
  platform layer sets a shape only when it changes (`ImGuiPlatformTests.cpp:311-354`, `ShapeChanges == initial + 1` after
  the same shape is asked twice) stay meaningful only because `SetCursorShape` counts every call.
- The shell tests start the stub at `CountingWindow`'s 640 x 360 (`Width`, `Height` and `Framebuffer`), so that the
  service's request for 1280 x 720 is a change, and a "player resize" writes `Width`, `Height` and `Framebuffer` together
  where it wrote `Framebuffer` (`ScriptShellTests.cpp:340`). `VSyncChanges == 1` after 30 frames
  (`ScriptShellTests.cpp:332-345`) proves that the service applies a request once, again only because every call counts.

The four fake `InputWindow`s stay: `InputWindow` is three functions.

**`T/Core/WindowBackendTests.cpp`**, `TEST_SUITE("Core.WindowBackend")` (shard Core.Core by `Core*`). Each case runs an
`Application` with `Headless = false`, one worker and one I/O thread (as `ApplicationFrameTests.cpp:108-115`), no modules unless
named, a `WindowFactory` returning a `StubWindow` whose pointer the test keeps, and a recording `Layer` (events by
`EventType`, and a per-frame callback that reads `Input`):

| Case | Checks | Fails without |
| --- | --- | --- |
| "A window it is given runs the frame loop: its events reach Input and the layers" | Frame 1's queue: key A down, mouse move (10, 20), left button down, scroll (0, 1), text 'a', resize 800 x 600, gamepad 0 with A down and LeftX 0.5. In frame 1's update: `Input::IsKeyPressed(Key::A)`, `GetMousePosition() == (10, 20)`, `IsMouseButtonPressed`, `IsGamepadConnected(0)`, `IsGamepadButtonPressed(0, A)`, `GetGamepadAxis(0, LeftX) == 0.5`, `GetTypedText() == "a"`; the layer saw KeyPressed, MouseMoved, MouseButtonPressed, MouseScrolled, KeyTyped, WindowResize in order; frame 2 releases A: `IsKeyReleased`, gamepad 0 disconnected; `GetWindow()` is the stub; `ProcessEventsCalls == GetFrameCount()` | `WindowFactory` |
| "Frames while the window is minimized are skipped and not counted" | `OnProcessEvents` minimizes calls 1-3; `MaxFrames` 5: `ProcessEventsCalls == 8`, `GetFrameCount() == 5`, the layer updated 5 times | (guards `RunFrame`, `Application.cpp:295-300`) |
| "A close request ends the loop unless a layer handles it" | A close queued at call 2 ends `Run` with 3 frames counted; a layer that handles the first close keeps the loop to a second one | (guards `OnEvent`, 208-211) |
| "A window that cannot be opened ends the start through the fatal-error path with exit code 1" | The factory returns null: `IsRunning()` false, `GetExitCode() == 1`, `GetWindow()` null, a module listed in `Modules` never got `OnInit` (a journaling module as in `EngineModuleTests.cpp:241`), `Run()` returns at once, and the log (`Tests::LogCapture`) holds one critical entry containing "its window could not be opened" | step 3 (null dereference) |
| "Window::Create returns null without a display instead of aborting" (`#if defined(ST_PLATFORM_LINUX) && ST_WITH_GLFW`) | A child, `--strata-test-helper=run-with-environment DISPLAY= WAYLAND_DISPLAY= XDG_RUNTIME_DIR= -- <tests> --strata-test-helper=open-window`, as `GPURendererTests.cpp:110-126` runs one: exit code 0, output "No window", its log a GLFW error and no crash report | step 3 (`ST_CORE_VERIFY` aborts) |
| "A build without a window backend opens no window and says how to run" (`#if !ST_WITH_GLFW`) | `Window::Create` returns null and logs an error naming `STRATA_WITH_GLFW`; an `Application` without `WindowFactory` and `Headless = false` exits 1 through the fatal-error path | step 5 |

Two helper modes in `T/TestMain.cpp`, after `create-graphics-device` (317-332):

- `open-window`: `Window::Create` of a hidden 320 x 200 window; prints `Window opened` or `No window`; exit 0 either way (a
  crash or abort is the failure).
- `stub-window-graphics [--imgui]`: first `Log::Init` at `LogLevel::Info` (TestMain logs only warnings and errors in
  helper modes, `TestMain.cpp:1261-1266`; helpers that log more initialize the log themselves). Then an `Application`
  (`Name` "StubWindowGraphics", `Headless` false, `WindowFactory` making a `StubWindow`, `Modules = {
  GraphicsModule::Factory({}) }` and, with `--imgui`, `ImGuiModule::Factory({})` after it, `MaxFrames` 10, one worker and
  one I/O thread); after `Run`, prints `Frames: <n>, can present: <yes|no>, errors: <device error count>` (the Graphics
  module's device, while the application still exists; `no device` when it has none) and returns the exit code.

`GPU.Window` (`T/Core/GPUWindowTests.cpp`): "A window that cannot present through Vulkan gives a device that renders
offscreen, and ImGui refuses it": runs `stub-window-graphics` as a child (an application's device never runs in the test
process, section 2.2): exit code 0 and `Frames: 10, can present: no, errors: 0`; the child's output holds the line
"cannot present through Vulkan" at info level and no line at error level. (Not "no warning": a machine whose validation
layer is missing logs a warning at device creation, `VulkanGraphicsDevice.cpp:514`, which says nothing about this path.)
Then `stub-window-graphics --imgui`: exit code 1 and the line "The ImGui module needs a window the Graphics module presents
into". Without step 1 the device asks GLFW, never initialized, for extensions and fails: exit 1 on the first run. Without
the `ImGuiModule.cpp:28` change the second run starts ImGui against a device with no back buffer.

### 5.5 Step 5: a build without GLFW (cut first)

**Root `CMakeLists.txt`**, after the module options (30):

```cmake
# The window backend (Strata/src/Platform/GLFW). Off: the engine has no window system (Platform/NoWindow: Window::Create
# opens nothing), so its programs run headless only: the configuration a console port starts from, which CI builds
# (Docs/DeveloperGuide.md, "Platform seams"). Code tests ST_WITH_GLFW, a definition like ST_WITH_PHYSICS.
option(STRATA_WITH_GLFW "Build the GLFW window backend" ON)
```

and before the editor's check (37): `if(NOT STRATA_WITH_GLFW) list(APPEND strataAbsentModules "STRATA_WITH_GLFW") endif()`,
with the check's message (39) "The editor is a tool with every engine module and a window: ...". Line 13's canary stays
(every checkout has every submodule; a build without GLFW only does not configure it). Line 71 stays: with GLFW off no `glfw`
target exists, so a configure-time `FORBID glfw` would prove nothing; the map check below does.

**`Strata/vendor/CMakeLists.txt:50-55`**: the GLFW block inside `if(STRATA_WITH_GLFW)`, its banner "GLFW (glfw; the window
backend's: STRATA_WITH_GLFW)". Nothing else in the file.

**`Strata/CMakeLists.txt`**:

- 25-28: `"ST_WITH_GLFW=$<BOOL:${STRATA_WITH_GLFW}>"` after `ST_WITH_ANIMATION`, the comment (23-24) "Which optional modules
  and backends the engine has".
- 52-56: the AppKit row inside `if(STRATA_WITH_GLFW)` (the Dock icon is the window's).
- 61: `strata_layer_link(App PRIVATE Vulkan::Headers stb)` and `if(STRATA_WITH_GLFW) strata_layer_link(App PRIVATE glfw)
  endif()`.
- (64 changed in step 1.) Nothing at 74-180 (lane Slang's).

**`CMake/StrataLayers.cmake`**: after the OS filter (104-111):

```cmake
	# The window backend: GLFW's, or none (Platform/NoWindow, STRATA_WITH_GLFW OFF). LibrariesTests' IsLeftOutOfBuild agrees.
	if(STRATA_WITH_GLFW)
		list(FILTER sources EXCLUDE REGEX "^Platform/NoWindow/")
	else()
		list(FILTER sources EXCLUDE REGEX "^Platform/GLFW/")
	endif()
```

and the header comment (5-6) "other operating systems' Platform/<OS>/ folders and the window backend not built left out".

**`Layers.json`**: App's `Paths` add `"Platform/NoWindow/**"`.

**`P/NoWindow/NoWindow.cpp`** (new, App layer):

```cpp
#include "stpch.h"
#include "Strata/Core/Window.h"

namespace Strata
{

	// The window backend of a build without GLFW (STRATA_WITH_GLFW OFF): none. Programs run headless (--headless,
	// ApplicationSpecification::Headless) or with a window their embedder makes (ApplicationSpecification::WindowFactory);
	// Application reports a windowed start through its fatal-error path.
	Scope<Window> Window::Create(const WindowSpecification& specification)
	{
		ST_CORE_ERROR("Cannot open the window '{}': this build has no window backend (STRATA_WITH_GLFW is OFF); run it headless",
			specification.Title);
		return nullptr;
	}

}
```

It is the defined behavior of that build, not a stub (no `TODO`, golden rule 5).

**`LibrariesTests.cpp:33-49`**: `IsOtherPlatform` becomes `IsLeftOutOfBuild` (its call at 154 too): the OS folders as today,
plus `Platform/NoWindow/` when `ST_WITH_GLFW` is 1 and `Platform/GLFW/` when it is 0, with the comment pointing at
`StrataLayers.cmake`.

**`StrataTests/CMakeLists.txt`**:

- After `STRATA_TESTS_OF_ANIMATION` (79-80): `set(STRATA_TESTS_OF_GLFW "src/Core/GPUWindowTests.cpp"
  "src/Core/ApplicationIconTests.cpp" "src/Platform/DockIconTests.cpp")` and, after the module loop (82-87), `if(NOT
  STRATA_WITH_GLFW) list(APPEND STRATA_TESTS_LEFT_OUT ${STRATA_TESTS_OF_GLFW}) endif()`. GLFW does not count toward
  `STRATA_TESTS_HAVE_EVERY_MODULE` (the feature test needs no window).
- 323: `if(APPLE AND STRATA_WITH_GLFW)` for `StrataTests.DockIcon`.
- 336-340: the map check names what a build without GLFW must lack:

```cmake
	set(STRATA_RUNTIME_MAP_CHECKS "")
	# A build without a window backend: no GLFW in the game, and Platform/NoWindow's Window::Create in its place.
	if(NOT STRATA_WITH_GLFW)
		list(APPEND STRATA_RUNTIME_MAP_CHECKS "-DEXTRA_OBJECTS=GLFWWindow|GLFWApplicationIcon" "-DEXTRA_LIBRARIES=glfw3|glfw" "-DEXTRA_REQUIRED=NoWindow")
	endif()
	add_test(NAME StrataRuntime.NoTooling
		COMMAND "${CMAKE_COMMAND}" "-DMAP=$<TARGET_FILE_DIR:StrataRuntime>/StrataRuntime.map" ${STRATA_RUNTIME_MAP_CHECKS}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/Architecture/CheckNoTooling.cmake")
```

**`CheckNoTooling.cmake`**: the usage (9) `cmake -DMAP=<file.map> [-DEXTRA_OBJECTS=a|b] [-DEXTRA_LIBRARIES=a|b]
[-DEXTRA_REQUIRED=a|b] -P CheckNoTooling.cmake`, with the header "EXTRA_*: '|'-separated object and library basenames a build
that leaves more out must not contain, and objects it must"; after the lists (19-36) the three variables' `|` become `;` and
are appended to `forbiddenObjects`, `forbiddenLibraries` and `requiredObjects`; the failure message (69) "The game executable
contains tooling or what this build leaves out (...)". (`glfw3` is GLFW's library name, `vendor/glfw/src/CMakeLists.txt:118`;
GNU ld's map names it `libglfw3.a(...)`.)

**`.github/workflows/ci.yml`**, the row (50-60) and nothing else:

```yaml
          # A game without the audio and physics modules and without a window backend (Docs/DeveloperGuide.md, "Optional
          # modules", "Platform seams"): no editor, and no package tests (they build games of every module).
          - name: Linux (no Audio, no Physics, no GLFW)
            os: ubuntu-24.04
            preset: linux
            config: release
            cache: ccache
            # Not prefixed by the Linux job's key: restoring by prefix must not cross between the two configurations.
            key: linux-without-modules-release
            options: -DSTRATA_WITH_AUDIO=OFF -DSTRATA_WITH_PHYSICS=OFF -DSTRATA_WITH_GLFW=OFF -DSTRATA_BUILD_EDITOR=OFF
            exclude: package
```

The key stays (the compiler cache is keyed by the commands, and the row stays one configuration). The name changes only if
main's branch protection does not require the old check name (section 11, Q-P2).

On that row the build proves: no engine layer, `StrataRuntime` or `StrataTests` needs GLFW to compile or link (the build), the
runtime's map holds no GLFW and holds `NoWindow` (`StrataRuntime.NoTooling`), the layer libraries agree with the table
(`Architecture.Libraries`), and the `#if !ST_WITH_GLFW` case of `Core.WindowBackend` runs. It does not prove that anything
compiles for a console, presents through anything but Vulkan, or behaves like a console's files, threads or suspend
(platform audit, "What the stub test and the OFF build prove").

Before the `ci/` push, configure (only) an OFF tree on this machine, `cmake --preset windows -B build/agent-platform-noglfw
-DSTRATA_WITH_GLFW=OFF -DSTRATA_WITH_AUDIO=OFF -DSTRATA_WITH_PHYSICS=OFF -DSTRATA_BUILD_EDITOR=OFF`: a configure costs
seconds and catches CMake mistakes; the build itself runs on CI (four build-heavy agents at most).

### 5.6 Step 6: the docs

- **DG**, a new `### Platform seams` at the end of "Architecture rules" (`DG:833-972`, so before `## Project settings` at
  `DG:973`): the window seam (`Core/Window.h`, the
  backend chosen at link time like `GraphicsDevice::Create` and `Platform::*`, `Window::Create` null on failure,
  `ApplicationSpecification::WindowFactory`), the presentation seam (`VulkanPresentation`, `GraphicsDeviceSpecification::
  PresentationWindow`, `CanPresent`), `ExternalOwners` in full (prefixes, case, owners before the layer's prefixes, the
  allowlist), `STRATA_WITH_GLFW` and `Platform/NoWindow`, `Tests::StubWindow`, and the inventory of what else a console port
  needs behind the same seams, documents only: the platform audit's table (window and input, graphics and present, OS
  services `Core/Platform.h`, file reads `Core/FileReader.h`, script modules `DynamicLibrary`/`CrashGuard`/`FileLock`/
  `Symbolizer` (the biggest gap: no static-link path for script modules), hot reload, processes, sockets, threads, file
  paths, process start, audio, the four places that name the OS list), with "no abstraction for threads, paths or processes
  until an SDK says what they need". The "Architecture rules" Layers bullet states the `ExternalOwners` rule in full
  (checklist item 5). "Optional modules" (`DG:241-266`): `STRATA_WITH_GLFW` (a backend, not a module: no layer option, the
  editor needs it, `ST_WITH_GLFW`, `STRATA_TESTS_OF_GLFW`) and the CI row's new name. "Windows subsystem" (`DG:309-311`): "no
  window" among the fatal startup failures.
- **`Docs/Architecture.md`**: the link table (77, 79: App links glfw only with `STRATA_WITH_GLFW`, Renderer no glfw); the
  platform paragraph (148-151: `Platform/NoWindow`; "Engine code outside `src/Platform/` includes no OS headers" becomes
  "...no OS headers but `TargetConditionals.h`, which `Core/PlatformDetection.h:12` needs to tell Apple's platforms apart;
  `ExternalOwners` enforces it for the headers it lists"); the layer table (577: App holds `Platform/Vulkan/VulkanPresentation.h` and `Platform/NoWindow/**`;
  581: Renderer excludes it); the layering paragraph (595-601: owners before the layer's prefixes).
- **`KeyCodes.h:10`, `MouseCodes.h:10`, `GamepadCodes.h:8`**: "The engine's key codes, named by where the key is on a US
  keyboard. They are script ABI (StrataScript/Input.h) and never renumbered. They were taken from GLFW's, so the GLFW backend
  passes its codes through; another backend translates its own." (mouse and gamepad likewise).
- **`Docs/Roadmap.md`**, "Later": "A console port: script modules linked statically (no `DynamicLibrary` path exists), crash
  containment without SEH or signals, the console's window and present behind `Window` and `GraphicsDevice`, file, thread
  and suspend behavior; DG 'Platform seams' lists the seams." Its open items: S0.6's finding (section 6.4).
- **`.claude/skills/strata-build-test/SKILL.md:36`**: "(and `_AUDIO`, `_ANIMATION`, `_GLFW`)".
- AGENTS.md: section 9.

### 5.7 Optional step 7 (below the cut)

`static_assert`s in `GLFWWindow.cpp` that the engine's key, mouse and gamepad values equal GLFW's (`Key::Space ==
GLFW_KEY_SPACE`, ...), so that the cast at 192 and 233 stays a translation. Both sides are frozen (GLFW's API and the script
ABI), so it guards little (platform audit, step 7).

### 5.8 Commits

1. "Present through the window's VulkanPresentation; the renderer links no GLFW" (5.1).
2. "Give GLFW, Vulkan and OS headers owners in the layer table" (5.2; the regression proof's output in the body).
3. "Let window creation fail through the fatal-error path" (5.3, with `T/Core/WindowBackendTests.cpp` created here holding
   its two failure cases, the null factory and the Linux child without a display, and the `open-window` helper, so the
   commit ships with its tests).
4. "Drive the frame loop through Tests::StubWindow" (5.4: the stub, the frame-loop cases, `stub-window-graphics`, the
   `GPU.Window` case, the two fakes replaced).
5. "Build without a window backend: STRATA_WITH_GLFW and Platform/NoWindow" (5.5).
6. "Document the platform seams" (5.6).

## 6. S0.6 Why FIFO does not pace the editor (box: half a day)

### 6.1 What is known

- The editor's window asks for vsync (`Window.h:61`), the device chooses FIFO when vsync is on (`VulkanGraphicsDevice.cpp:1125`)
  and logs it: "Swapchain: 2400 x 1350, 3 images, vsync" (`physics-timing/editor.log:6`, once: the swapchain was not rebuilt).
- Yet the editor ran at 190-720 FPS in play on a 120 Hz display, hidden behind a maximized window, and 273-299 visible and
  unfocused, with Present near 0.1-0.26 ms (`editor-ux/audit.md:13-17`), and 460-900 FPS in the timing audit. Nobody
  measured a focused window, the present mode in use, or a driver override (`editor-ux/audit.md:216`).
- With FIFO and 3 images (`imageCount = max(minImageCount + 1, MaxFramesInFlight + 1)`, 1134), the CPU should block in
  `vkAcquireNextImageKHR` (1442, no timeout) or in the frames-in-flight wait (1409-1415) once the display holds the images:
  at most the refresh rate. Frames above it mean the presentation engine returns images faster than the display shows them.
- What the device waits for is not reported: `GraphicsModule::OnBeginFrame` adds it to `frame.WaitSeconds` (67-69), which
  only the work time subtracts; the `Present` stage times `EndFrame` (82-83).

### 6.2 Hypotheses, in the order they are cheap to test

1. **Occluded or unfocused windows are not throttled** by the compositor or the driver's present path: then a focused,
   visible window is paced and the audits' cases are not.
2. **A driver setting overrides the application**: NVIDIA's "Vertical sync" (Off or Fast) or its "Vulkan/OpenGL present
   method" (native or layered on DXGI). Then FIFO behaves like IMMEDIATE or MAILBOX everywhere, focused too.
3. **An implicit Vulkan layer intercepts present** (overlays, recorders, frame limiters).
4. **Windows' "Optimizations for windowed games"** or VRR change the windowed present path
   (`HKCU\Software\Microsoft\DirectX\UserGpuPreferences`, value `DirectXUserGlobalSettings`: `SwapEffectUpgradeEnable`,
   `VRROptimizeEnable`).
5. **Our code**: the swapchain recreated every frame (several "Swapchain:" lines), MAILBOX or IMMEDIATE chosen (the log
   would say), frames that never acquire.

### 6.3 Procedure

Instruments, local to the lane's worktree and never committed (except 6.4's log line):

- `CreateSwapchainResources` logs the surface's offered present modes, `minImageCount` and `maxImageCount`.
- `BeginFrame` accumulates the time of the frames-in-flight wait (1409-1415) and of `vkAcquireNextImageKHR` (1442) separately
  and logs their p50 and p95 every 600 frames with the frames per second over them.

Runs, each 600 frames or more after 2 s of settling, Release build of the lane, Third-Person copied into a temporary
directory (never the sample in place), playing, `editor.perf {frames: 600}` read through `StrataCLI call --project`:

| Case | How |
| --- | --- |
| A. Focused, visible, windowed editor (the owner's normal case) | Started without `--headless`; nothing else in front. Takes the desktop's focus for about two minutes: run it when no other agent runs a windowed program and at a time the orchestrator gives (the owner may be at the machine), and say so in the lane's report |
| B. Visible, unfocused | Another window focused beside it |
| C. Occluded | A maximized window in front |
| D. The exported Third-Person runtime, focused, windowed | `StrataRuntime --game <copy>/Build/ThirdPerson.stgame --resolution 1280x720 --seconds 20 --perf-log run.csv` |
| E. The same, Borderless | The game's Graphics menu, or `Game::SetWindowMode` through a test script |
| F. Case A with vsync off | `Window::SetVSync(false)` through a temporary switch: MAILBOX or IMMEDIATE, the uncapped reference |

Environment, read once: the display's refresh (`Get-CimInstance Win32_VideoController | Select-Object CurrentRefreshRate`),
the implicit layers (`reg query "HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers"` and the same under `HKCU`), the layers the
loader actually loads into the editor's instance (case A once with `VK_LOADER_DEBUG=layer`, stderr captured from bash), and
the DirectX user preferences value above.

The table every run fills: frames per second, frame interval p50 and p95, acquire wait p50 and p95, frames-in-flight wait p50,
`Present` p50, present mode chosen and offered, swapchain lines in the log.

### 6.4 How the result lands

| Finding | What lands |
| --- | --- |
| A paced at the refresh rate, B and C not (hypothesis 1) | The named cause: Architecture.md (809-818) "vsync paces a visible, focused window; this machine's driver does not throttle presents of unfocused or occluded windows, where the frame rate cap does (`Editor.FrameRate`'s `UnfocusedIdleFrameRate` and `ActiveFrameRate`, S0.4)"; a Roadmap open item with the table; no engine change |
| A not paced, the acquire never waits, FIFO offered and chosen, no implicit layer (hypothesis 2 or 4) | The orchestrator asks the owner what NVIDIA Control Panel's "Vertical sync" and "Vulkan/OpenGL present method" are set to (globally and for `StrataEditor.exe`): the one fact an agent cannot read. The open item names the setting once known, else "a driver or system setting, unconfirmed", with the table |
| An implicit layer present in the loader's list (hypothesis 3) | Case A again with that layer disabled for the run (`VK_LOADER_LAYERS_DISABLE=<name>`, the child's environment only); the open item names the layer |
| Our code (hypothesis 5) | The fix, with a regression test that fails without it (a `GPU.Window` case through a child helper, as 5.4's, asserting the present mode the device reports or the swapchain count) |

In every case, one permanent change: the swapchain's log line (1222-1223) names the offered modes, "Swapchain: 2400 x 1350, 3
images, vsync (FIFO; the surface offers FIFO, MAILBOX, IMMEDIATE)", so the next report needs no instrument. The line keeps
its "Swapchain: <w> x <h>" start, which `StreamingPerfTests.cpp:233` reads from the runtime's output. No field is added
to `editor.status` (lane Editor's S0.4 edits its `frameRate`; `editor.perf`'s `boundBy` already says `capped` when the display
or a cap sets the pace). Release notes name the finding. When the box ends without a cause, the open item lists what was
measured and ruled out. Until then, the Performance panel's pause (S0.5) and `physics-timing` assume the frame rates the owner
sees (plan S0.6).

## 7. Acceptance, and how each number is measured on this machine

| What | Target | Measured by |
| --- | --- | --- |
| `glfw*` symbols outside `Platform/GLFW` | 0 (0.7.0: 3, `VulkanGraphicsDevice.obj`) | `/c/msys64/mingw64/bin/python.exe .worktrees/wave8-plan/platform-seams/scan_glfw_symbols.py build/agent-platform/lib/Release /c/msys64/mingw64/bin/nm.exe` on the lane's Release build: only `Strata_App.lib: GLFWWindow.obj` lists symbols |
| The table rule | fails on 76f28a21 with exactly one violation, `VulkanGraphicsDevice.cpp(21)`; passes after step 1 | `StrataTests --test-case="The engine's includes follow the layer table"`, output in step 2's commit |
| Window failure | exit code 1 through `ReportFatalError`, no abort | `Core.WindowBackend` (every build), its Linux child case on CI |
| The surface path | every windowed CTest (label `gpu`, section 5.1) passes in Debug (validation layer) and Release | the full Debug and Release ctest on the reference machine, then the owner's look after the orchestrator rebuilds `build/windows` |
| A window that cannot present | `GPU.Window`'s child: 10 frames offscreen, 0 errors; ImGui refused with exit code 1 | Debug ctest, `GPU*` suites |
| A build without GLFW | the CI row green, `StrataRuntime.NoTooling` passing with the GLFW forbids and `NoWindow` required | `ci/wave8-platform` |
| GPU validation | 0 errors | Debug ctest, `GPU*` suites; the stub helper prints `errors: 0` |
| Project rate | 240 steps/s: a 0.1 s update drops no step; a 0.25 s update with `KeepEveryFixedStep` runs 31 or 32 and drops none (float rounding carries the 32nd, 4.8); without it a 0.25 s update runs the cap, 32, and drops the rest | `Runtime.Game` and `Scene.Timing` |
| Project rate 60 on a default scene | bit-identical steps and time to the scene's own | `Scene.Timing` |
| Samples | every `Editor/<Sample>Commands.json.in` passes unchanged (no sample sets `Scene.Timing`) | Debug ctest's sample tests |
| S0.6 | the table of 6.3 filled for cases A-F, a named cause or a fix | the lane's report and the open item |

## 8. Perf

No new metric and no `Budgets.json` change: nothing in the lane runs per frame beyond one branch in the fixed loop
(`GetMaxFixedSteps`) and a virtual call at swapchain creation. As information, two interleaved A/Bs against
`.worktrees/wave8-base`, three pairs each, under `python Tools/PerfLock.py --label wave8-platform -- <exe> --test-case=...`:

| Metric (test case) | Expect | Why |
| --- | --- | --- |
| `Perf.Scene.IdlePlayFrame1M` ("A million nested entities cost nothing per frame while nothing changes", `T/Perf/ScenePerfTests.cpp:83`) | within noise | S0.9's change in the fixed loop |
| `PerfGPU.Editor.EditFrameNested100k` ("Editor frames of 100,000 and 1,000,000 nested entities, opened, edited and played", `T/Editor/EditorFramePerfTests.cpp:209`) | within noise | A windowed editor's frames through the new presentation path |

The commit messages state the medians. A ratio outside the pair-to-pair spread is investigated before the lane lands.

## 9. Docs, AGENTS.md and release notes

AGENTS.md (after S0.1, at most 250 code points for the lane, `Docs.AgentGuide` counts them): 201 code points before line
wrapping, in two existing bullets (counted at review: 81 and 120). AGENTS.md holds 39,292 characters at 76f28a21, and the
3,000 S0.1 frees are planned for the nine streams' bullets and Slang's rules (plan S0.1), so these 201 come on top of them:
S0.1 frees them too, or the orchestrator decides (open question). If the room is not there when the lane lands, the Layers
clause (81) stays and the reader rule is stated in DG's Physics tuning rule only.

- Architecture rules, **Layers**, after "games link no `Tooling` layer.": " `ExternalOwners` keeps GLFW, Vulkan and OS headers
  in their `Platform/` folders." (81)
- Architecture rules, **Physics tuning**, before its link: "; code reads the fixed step through `Scene::GetFixedTimestep` and
  `GetMaxFixedSteps` (the project's `Scene.Timing` wins)" (120)

Both rules are stated in full in DG (sections 4.9 and 5.6). The full list of documents is in 4.9, 5.6 and 6.4.

Release-note lines (0.8.0):

- Engine: "GLFW is sealed behind the window: only `Platform/GLFW` includes it, which the layer table's `ExternalOwners`
  enforces with Vulkan's and the OS headers; the Vulkan device presents through `Window::GetVulkanPresentation`."
- Upgrade row: "`Window::GetNativeWindow` is gone. A `Window` of your own drops its override; code that needed the OS
  window uses `GetPlatformHandle`, code that presents through Vulkan `GetVulkanPresentation`."
- "A window that cannot be opened ends the program through the fatal-error path (a dialog for a person, exit code 1)
  instead of aborting."
- "`-DSTRATA_WITH_GLFW=OFF` builds the engine without a window backend: its programs run headless only. CI builds it."
- Project settings: "Physics > Timing: **Fixed Rate** (Scene, 30, 60, 120, 240) sets every scene's fixed step; with a rate an
  update runs at most 2/15 s of steps (8 at 60, 32 at 240). The default, Scene, keeps each scene's own."
- Scripts: "`Time::GetFixedDeltaTime` returns the step the scene runs: the project's rate when it sets one."
- Editor: "`input.*` durations in seconds count in the scene's step also while not playing (1/60 s before)."
- S0.6's finding, as 6.4 words it.

## 10. What the lane shares, and how merges resolve

| File | Others | This lane's hunks | Resolution |
| --- | --- | --- | --- |
| `Strata/CMakeLists.txt` | lane Slang (the shader list 74-180, perhaps a host tool target) | 23-28 (`ST_WITH_GLFW`), 52-56 (AppKit), 61 (App), 64 (Renderer) | Disjoint hunks: take both |
| `Strata/vendor/CMakeLists.txt` | lane Slang (glslang, 226-235, leaves) | 50-55 (GLFW block) | Disjoint: take both |
| `.github/workflows/ci.yml` | lane Slang (a `spirv-val` step and apt package on Linux, a Metal conversion step on macOS, perhaps a Mesa job) | 50-60 (the row) | Disjoint: take both. If Slang adds a matrix row, it goes after this one |
| Root `CMakeLists.txt` | lane Slang (perhaps `include(StrataSlang)` near 56) | 25-41 (option and editor check) | Disjoint: take both |
| `StrataTests/CMakeLists.txt` | lane Slang (SPIR-V and HLSL tests), `showcase` (its CTests) | 79-90 (`STRATA_TESTS_OF_GLFW`), 323, 336-340 | Disjoint: take both |
| `T/TestMain.cpp` | any lane adding helper modes | two modes after 332 | Keep both blocks |
| `P/Vulkan/VulkanGraphicsDevice.cpp` | S0.6 (this lane); lane Slang only if its shader path changes the device (unverified, platform audit) | 6-21, 255-259, 410, 477-488, 699, 1033-1041, 1222-1223 | In-lane order (S0.6 after step 1); otherwise disjoint |
| `S/Scene.h`, `S/Scene.cpp` | `physics-timing` owns the fixed loop (plan 4) | `Scene.h` 435-436, 590-595, 742; `Scene.cpp` 216-244 and 2174 | S0.9 lands before `physics-timing` branches (section 3): nothing to resolve. If not, `physics-timing` keeps `GetMaxFixedSteps()` at 2174 |
| `ScriptHostAPI.cpp`, `GameRuntime.cpp`, `EditorContext.cpp` | `physics-timing` (host functions appended, the Play switch beside 1389 and 1530) | 325-326; 28-41; 549-559 | Same as above |
| `S/ProjectSceneTiming.*` | `physics-timing` adds `Interpolation` (its step 4) | created here | `physics-timing` appends the field; the section stays version 1 (missing members take the field's default at any version), as no release ships between them |
| `E/EditorSceneCommands.cpp` | S0.7 if it registers `scene.raycast` there (unverified) | 38-52, 314-330, 560-651 | Disjoint registrations: take both |
| `StrataEditor/src/Panels/InspectorPanel.*` | none in step 0 | 4.6 | |
| `StrataTests/FeatureTest/FeatureTest.stproj` | any stream adding a project section | the `Scene.Timing` entry | Keep both entries in key order |
| AGENTS.md | S0.1 first, every stream's digest bullet | two bullets (section 9) | Count again after the merge (`Docs.AgentGuide`) |
| `Docs/Roadmap.md` | S0.12 (the wave's entry, wave 9's) | 907-908, a "Later" line, an open item | S0.12 merges after the lanes: keep both |
| DG, Architecture.md | every lane | new sections and the lines named | Text merges by section |

Lane Slang merges main before its last verification (plan section 3), so if this lane lands first, Slang's merge takes these
hunks as they are; if Slang lands first, this lane merges main before `ci/wave8-platform` and takes Slang's.

## 11. Risks and open questions, with the decision recommended

Risks:

1. **The surface path runs only on Windows here** (the windowed `gpu` CTests of section 5.1; hosted CI has no GPU, no Mac is
   reachable, D9). Mitigation: the three calls move one for one; the Linux and macOS jobs compile them; the owner looks at
   the rebuilt editor.
2. **The window failure path is tested on Linux only**: Windows offers no way to make `glfwCreateWindow` fail on demand.
   The code is shared; the null path through `Application` is tested everywhere with `WindowFactory`.
3. **Whether empty `DISPLAY`, `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` stop GLFW on every Linux desktop is unverified**; CI's
   runners have no display at all, so the case holds there. If a desktop still finds a display, the case reports it, and the
   implementer narrows the environment rather than skipping.
4. **The OFF build has never been compiled** (platform audit, "Not verified"). Mitigation: the local configure, then one CI
   round trip; step 5 is cut first if it does not converge.
5. **The feature scene moves from 50 to 60 steps per second** (4.7). Game time per frame is unchanged, so time-based
   expectations hold; a check tuned to 50 steps is changed with its reason. `ContactHertz` 25 is under both ceilings (25 Hz at
   50 steps/s, 30 at 60), so contacts behave alike.
6. **240 steps/s may run 32 heavy steps in one long frame** where 8 ran before. Editor Play and games keep time dilation
   (`Scene.cpp:2181-2209`), which stops catch-up steps that take longer than they simulate; tests and `--frames` runs run
   them all by design.
7. **`Testing.Shards` in the OFF build** sees `Core.ApplicationIcon` and `Platform.DockIcon` absent. The latter is absent on
   every non-Apple build already; the implementer checks the shard test on the CI row.
8. **S0.6's case A takes the desktop's focus for about two minutes on the owner's machine.** Besides other agents' windows,
   the owner may be using it. The orchestrator schedules the run (open question); cases B-F do not take focus.
9. **The `stub-window-graphics` child runs `Renderer::Init` on a device with no swapchain.** The headless editor (`--headless`)
   takes the same path in `GraphicsModule::OnInit` today, so it is expected to hold, but no test has made it through an
   application with a window that cannot present. If it fails, the failure is the finding: the fallback of 5.1 is not done.
10. **Float steps at other rates.** The float32 model of the fixed loop gives one step fewer than the time limit at 240
    (4.8). Tests state bounds that hold in float, never "exactly N" for a project rate other than 60.

Open questions:

- **Q-P1, the lane's order.** S0.9 first, landed alone, before `physics-timing` branches. Recommended: yes (section 3).
- **Q-P2, the CI row's name.** Rename to "Linux (no Audio, no Physics, no GLFW)" unless `gh api
  repos/2042Third/GameEngine/branches/main/protection` lists the old name among required checks; then keep the name and change
  only the comment and options.
- **Q-P3, case in owner prefixes.** Ignore ASCII case: MSVC accepts `<windows.h>`, which a case-sensitive prefix would let
  through. Recommended: ignore case.
- **Q-P4, which OS headers.** The full list from the tree (5.2), not the audit's five: the five leave `<shlobj.h>` or
  `<netdb.h>` in Core unnoticed. `objc/` and `TargetConditionals.h` stay unowned.
- **Q-P5, a window that cannot present.** The device renders offscreen with an info line, the module skips the swapchain and
  ImGui refuses to start. Recommended as written; an error would make the stub window useless to the GPU helper.
- **Q-P6, input durations while not playing.** Count in the edited scene's step (the one play will run), not 1/60.
  Recommended: yes; a release-note line says so.
- **Q-P7, "30 warns about soft contacts" (plan S0.9).** In the field's description, which the settings page and
  `project.settings` show; no log or command warning. Recommended.
- **Q-P8, `FixedRate`'s type.** An Enum of option names, as the plan lists and as `Renderer.Quality` stores sizes; an Int
  field would accept 37 Hz. Recommended: Enum.
- **Q-P9, the scene's own cap under a project rate.** Ignored (the row is inactive); the cap is 2/15 s at the project's rate.
  Recommended: yes, so that 240 Hz never inherits a 60 Hz scene's 8.
- **Q-P10, S0.6's permanent instruments.** Only the swapchain log line; no `editor.status` field. Recommended: yes.

## 12. The first hour

1. Read plan sections 3 (lane Platform), 4.1 and 4.6, decisions, this spec, and the two audits; `python Tools/PerfLock.py
   --status`.
2. `git worktree add G:/GameEngineAI/.worktrees/wave8-platform -b agent/wave8-platform main` once S0.1 is on main;
   `cmake --preset windows -B build/agent-platform`; start the Debug build in the background.
3. Repeat section 2.3's grep and the OS-header grep of 2.2 at the base; note any reader or include merged since.
4. While the build runs, write S0.9 commit 1 (`Scene::GetMaxFixedSteps` and the readers of 4.4), then `ProjectSceneTiming`
   for commit 2.
5. Check `gh api repos/2042Third/GameEngine/branches/main/protection` for Q-P2.
6. Before touching `VulkanGraphicsDevice.cpp`, stash step 2's analyzer and table in a scratch commit on a side branch, run the
   tree check on the unchanged tree, and save its output for step 2's commit message.
7. Read the review corrections below before writing tests: the float bound of `Runtime.Game`, the stub's counters, the
   helper's log level and the commit order of S0.9 change what a correct first draft looks like.

## 13. Review corrections

An independent review on 2026-11-01 checked this spec against main at 76f28a21, the plan, the rulings (`decisions.md`) and
the two audits. No other step 0 or stream spec existed in `specs/` yet, so conflicts were checked against plan sections 3
and 4 only. Every line count of table 2.1 and almost every file:line held, and so did the main designs: the
`VulkanPresentation` seam, the owner check in the analyzer, the failure path, the OFF build, `Scene.Timing` with its
hand-overs and readers, and S0.6's boxed procedure. With the table of 5.2 the analyzer reports exactly one violation on
the unchanged tree: a grep of every `#include` over `Strata/src` finds no listed OS header outside its folder and no
cross-OS header inside `Platform/Windows` or `Platform/Posix`. The corrections below win over the body where they differ
(`decisions.md`).

**Tests that were wrong or would pass without the change**

1. **`Runtime.Game`'s "exactly 32 steps" fails on a correct implementation** (4.8, section 7). The fixed loop accumulates
   in float (`Scene.cpp:2173-2215`). A float32 model of the loop and `LimitToFixedSteps`, which gives the existing case's 8
   steps at 60 (`GameRuntimeTests.cpp:265-341`), gives 31 at 240 for every start remainder tried: the 32nd step is a few ulps
   short and falls due next update. The case now asserts at least 31 and at most 32 steps, none dropped, played seconds
   equal to the steps counted times the step, and at least 63 over two updates. The 0.1 s case's "at least 23" was already
   right (the model gives 23). Risk 10 records the rule.
2. **`Tests::StubWindow`'s counters weakened two existing tests** (5.4). The spec counted `SetVSync` and `SetCursorShape`
   only when the value changed. The fakes it replaces count every call, and that is what the tests rely on:
   `ImGuiPlatformTests.cpp:311-354` proves that the platform layer sets a shape only on a change, and
   `ScriptShellTests.cpp:332-345` that the game window service applies a request once. With change-only counters both
   would pass with redundant calls (golden rule 2). Every setter now counts every call. The shell tests also keep their
   fake's 640 x 360 start, or the request for 1280 x 720 might not be a change at all.
3. **`GPU.Window`'s child would never print the info line it checks** (5.4). TestMain logs only warnings and errors in
   helper modes (`TestMain.cpp:1261-1266`), so the helper now initializes the log at Info itself. "No `[warning]` line" became
   "no error-level line": a machine without the validation layer logs a warning at device creation
   (`VulkanGraphicsDevice.cpp:514`) that says nothing about this path.
4. **The ImGui refusal of 5.1 had no test.** `stub-window-graphics --imgui` now runs it: exit code 1 and the refusal line.
5. **`Editor.Commands`' case was not implementable as written** (4.8). `physics.stats` reports `settings` only while a
   physics world exists, so the case now makes ground and a box, as its model at `EditorCommandTests.cpp:1657-1701` does. One
   `edit.undo` would undo only the last of the three steps recorded since, so it now undoes three after `play.stop`. The
   "30 without the reader" figure is explained (the copy's stored 0.01 s gives `min(30, 50)`).
6. **`Editor.UI.Inspector` would read no row probes**: the Physics section starts closed (`InspectorPanel.cpp:1291-1292`). The
   case now opens it first, and the link's number format is fixed (`{:.4g}`, as `DescribePhysics`).
7. **`Editor.Input`'s not-playing half** now names its observable result (`steps` in the command's result, 12 against 6).

**Steps an agent could not land as written**

8. **S0.9's commit order broke "every commit passes every test"** (4.10). Commit 1 registered `Scene.Timing` while the
   feature test's value came in commit 2, and a registered section at its default fails `StrataTests.FeatureTest`. The
   readers now move first (commit 1, behavior-preserving except the input reader, which has its own test), and the section
   lands with the feature test's value and `TimeFeatures.cpp` (commit 2).
9. **S0.8 commit 3 shipped without tests** (5.8). It now creates `WindowBackendTests.cpp` with its two failure cases and the
   `open-window` helper. Commit 4 adds the stub and the frame-loop cases.
10. **The failure branch of 5.3 returned before `Input::Reset()`.** The code now replaces `Application.cpp:84-99` whole with
    one flag, so the reset runs on every path. Tests run several applications in one process.

**Wrong claims, corrected**

11. **"`StrataEditor.Smoke` and `QuitScreenshot` are the only runs of the surface path"** (sections 3, 5.1, 7, risk 1). Every
    windowed CTest creates a surface: `LauncherSmoke`, the exported game's windowed, resolution, quality, perf-log and
    no-camera runs, the feature game's and the samples' windowed runs (`StrataTests/CMakeLists.txt:387-407, 643-695, 741,
    833`). The gate is the full Debug and Release ctest with the `gpu` label; the `-R` run is a quick first check.
12. **"GPU tests never create devices in process"** (2.2): `GPURendererTests.cpp:69-91` makes a second bare device in
    process. The child process is still right, for another reason: an application's Graphics module calls `Renderer::Init`,
    which is process-wide state the shared `GPUContext` holds.
13. **Docs**: DG's "Stepping a paused game" lines (3376-3381) are in "Simulated input" (`DG:3347`). "The end of Architecture
    rules" is before `DG:973`, not at 833. `Architecture.md:4081`, `strata-editor-automation/SKILL.md:595` and the comments at
    `SimulatedInput.h:52` and `RuntimeApplication.cpp:77` also name `MaxFixedStepsPerFrame` (now in 2.3 and 4.9).
    `Architecture.md`'s "includes no OS headers" has an exception at `Core/PlatformDetection.h:12` (`TargetConditionals.h`),
    which the docs now name instead of calling the sentence enforced. The settings-provider helper lives in
    `PhysicsLayerLoadTests.cpp:33-53`, not in `SettingsTestUtils.h`.
14. **The S0.6 log line** keeps its "Swapchain: <w> x <h>" start, which `StreamingPerfTests.cpp:233` parses.

**Budgets and scheduling**

15. **AGENTS.md** (section 9): S0.1's 3,000 characters are planned for the nine streams' bullets and Slang's rules, and
    AGENTS.md holds 39,292 characters at 76f28a21. The lane's 201 come on top of that, so the orchestrator decides. A
    fallback keeps the Layers clause.
16. **S0.6's case A takes the owner's desktop focus** (6.3, risk 8): the orchestrator gives the time.

**Reviewed and kept**: the map check replaces the plan's "link closure check" for the OFF build, because with GLFW off no
`glfw` target exists for `strata_check_link_closure` to forbid. The `|`-separated `EXTRA_*` arguments pass through CTest
unquoted. Case-insensitive owner prefixes, the full OS header list, the offscreen fallback, the Enum `FixedRate`, the
description-only warning at 30, and the inactive scene cap under a project rate (Q-P3 to Q-P9) are sound as recommended.
The MCP `core` toolset's 30 KiB bound is not at risk from the longer `scene.info` description, because tool descriptions are
cut at 160 characters (`McpToolsets.cpp:13`). Nothing in the lane renders, so the image rules (both draw paths, shadow
caches, Medium's costs, no TAA dependence) have nothing to bind.
