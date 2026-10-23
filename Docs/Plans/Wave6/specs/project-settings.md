# Workstream `project-settings`: project settings, preferences, input actions, project-wide collision layers and the game's identity (wave 6)

Owner's request (2026-10-22): start working on `render-quality` and `project-settings`, which the wave-4 audits ranked
first for wave 6 (Docs/Roadmap.md, "Later (wave 6 and beyond)"). This stream's scope, from the roadmap:

- Project Settings and Preferences panels.
- Input actions with rebinding; `game-ui`'s navigation becomes their first map.
- Collision layers for the whole project.
- The game's name, version, company and icon in the exported executable.

`render-quality` runs at the same time on this Mac in its own worktree; both branched from main at 6425b57b (release
0.5.0). The owner works hands-off and judges the engine by running the editor: what this stream delivers must be visible
and working in the editor's menus and in the samples.

AGENTS.md is the source of truth for rules. The machine's rules (restated from wave 5's specs, still true; checked
against `Tools/` and `CMakePresets.json` at 6425b57b):

- The Windows perf reference machine is away. On the Mac, `-L perf` informs and is not the gate; existing
  `StrataTests/Perf/Budgets.json` entries (`Measured`, `Budget`) are never added, raised or lowered from Mac numbers; a
  new metric gets a provisional entry (`{ "Unit": ..., "Provisional": true }`, with its acceptance limit as `Target`
  when it has one), which measures, prints and never fails. Mac measurements go into commit messages and reports only.
- A/Bs: Release builds, interleaved, at least 3 pairs, every measurement under
  `python3 Tools/PerfLock.py --label project-settings -- <command>`, judged by the ratio of the medians. The base build is
  6425b57b's Release build in a scratch directory of this worktree, deleted afterwards.
- Every commit: `python3 Tools/CompatLint.py --changed-since 6425b57b` after a Debug build (GCC 14 and LLVM 16 with its
  libc++, warnings as errors) comes back clean; CI's GCC 14.2 at `-O3` (the release workflow's Dist builds) must stay
  warning-free too.
- The stream builds in its own build directory inside its worktree (`cmake --preset macos` there gives
  `.worktrees/project-settings/build/macos`); `ctest` runs use the `macos-debug` and `macos-release` presets. Vendor
  sources are read from `/Users/mikeyang/Documents/gameengine/Strata/vendor` (the same pins) while the worktree's
  submodules check out. The main checkout and the `render-quality` worktree are never modified.

## 1. Goal

At the end of the stream:

- **Project Settings** (per project, committed with it) and **Preferences** (per user, in the user's data folder) are
  two windows in the editor's File menu, drawn from state that commands change. Their sections are registered by the
  modules that own them (`SettingsRegistry`), so a later stream (game-shell's quality presets, render-quality's defaults)
  adds a section with one registration call and the windows show it without knowing it. Every change runs a command with
  a complete schema and an agent-facing description; project changes are undoable, preference changes are not (D4).
- **Input actions:** a project defines action maps (actions of button, axis and 2D kinds, bound to keys, mouse buttons,
  gamepad buttons, axes and sticks, and WASD-style composites). Scripts and the game UI read actions; players rebind at
  run time and their bindings are saved through `IGameSaves`. The game UI's six navigation actions are the built-in `UI`
  map, rebindable like any other. The editor edits maps (with a binding capture) and its `input.*` simulated input
  reaches actions.
- **Collision layers** are the project's: one table of names and one collision matrix for every scene, prefab, world
  cell and exported game. 0.5.0 projects are migrated once, when they first open, with the same behaviour.
- **The game's identity:** a title, a version, a company, a copyright line and an icon (an image asset of the project)
  reach the exported game: its window title and icon everywhere, on Windows the executable's version information and
  icon (written into the copied runtime), and, as the cut line's extras (section 10), its Dock icon on macOS and a
  desktop entry on Linux.
- At least one sample (Third-Person Adventure) reads actions instead of keys, and every sample exports with a title,
  version and icon. Every sample's `StrataTests/Editor/<Sample>Commands.json.in` regression keeps passing.

Not the goal: game-shell's work (crash reports, quality presets, per-platform exports, the macOS app bundle), local
multiplayer device assignment, input processors and chords, layout-aware key names, code signing of exports, moving the
fixed timestep or gravity to the project (section 9).

## 2. Where we start (6425b57b, release 0.5.0)

Paths are relative to the worktree. Facts below were checked against the code; line numbers are 6425b57b's.

### 2.1 The project file and the manifest

- `Strata/src/Strata/Project/` (744 lines): `Project.h/.cpp` (107/377), `ProjectModule.h/.cpp`, `GameManifest.h/.cpp`
  (57/152). `ProjectConfig {Name, AssetDirectory, StartScene, Scripts, Export}` (Project.h:37-44);
  `ProjectExportSettings {AlwaysInclude, LoadingScene}`; `c_FormatVersion = 2`; `Load` refuses versions above it
  (Project.cpp:168-170); "Export" arrived as an optional key without a version bump.
- `Project::Save` rebuilds the whole document from `m_Config` (Project.cpp:286-301): **keys it does not know are
  dropped**, so a 0.5.0 editor saving a newer project loses its newer sections. The write is `FileSystem::WriteText`
  (atomic), on the calling (main) thread.
- On disk: `StrataTests/FeatureTest/FeatureTest.stproj` is Version 1; the samples are Version 2 (ThirdPerson and
  OpenWorld set `Export.LoadingScene`, PhysicsPlayground `Export.AlwaysInclude`).
- `GameManifest` (`.stgame`, format version 4: Name, AssetPack, PatchPacks, StartScene, LoadingScene, ScriptModule,
  Window). The runtime never reads `.stproj`: anything project-wide a game needs must travel in the manifest or the
  pack. `FromJson` ignores unknown keys and refuses newer versions.
- The runtime reads the manifest twice: in `CreateApplication` (RuntimeApplication.cpp:402), which `EntryPoint.h:25`
  calls **before** `Engine::RegisterBuiltinModules` (:30), and again in `GameRuntime::Create` (GameRuntime.cpp:29), after
  registration.
- The game's name drives the window title (`RuntimeApplication.cpp:436`), the log directory
  (`MakeGameDirectoryName(name)`, :406) and the save directory (`SaveDirectory::ForGame(m_Manifest.Name)`,
  `GameRuntime.cpp:82`): renaming the game moves players' saves.

### 2.2 Editor commands, undo, the shell and MCP

- Project commands live in `RegisterAssetCommands` (`StrataEditor/src/Editor/EditorAssetCommands.cpp`, 1,204 lines):
  `project.setStartScene` (:396) and `project.setExportSettings` (:530) mutate `GetConfig()`, call `Project::Save` on
  the main thread and roll back on failure (:414-420, :587-591). **Neither records an undo step.**
- `EditorCommandKind {Edit, Query, Session}` (`CommandJournal.h:25-31`): Session covers "the editor's own files".
- Undo: `EditorAction::ChangesScene()` (default true) lets non-scene steps leave the scene saved
  (`AssetDocumentAction`); `EditorContext::ResetScene` clears the stack when a scene opens (EditorContext.cpp:524).
  `Undo` and `Redo` fail while playing (EditorContext.cpp:1671-1678); asset steps are recorded while playing too
  (AssetEdit.cpp:243 records unconditionally), so they are undone after play stops. The guide's Undo section says "Edits
  while playing are not recorded" (DeveloperGuide.md:1744), meaning scene edits.
- Saves: "never serialize a document on the main thread for a save" (DeveloperGuide "No silent data loss");
  `Editor/DocumentSaver` serves engine-native assets only (it refuses items that are not assets of the project).
  `CommandUtils::AfterSaves` (CommandUtils.h:159) and `editor.status`'s `saving` section (EditorStateCommands.cpp:755)
  follow it. `CommandUtils::CheckUnsavedChanges` guards the commands that replace the edited scene (`scene.open`,
  `scene.new`, `project.open`, `project.create`, `project.close`, `project.openSample`, `editor.quit`) against unsaved
  **scene** changes only (DeveloperGuide.md:1694-1700).
- Asset references: `AssetReferences::Find` (used by the delete dialog and the delete commands, EditorAssetCommands.cpp:856)
  finds referencers among assets; nothing names the project's start or loading scene as a referencer. A pruned export's
  roots are the start scene, `Export.AlwaysInclude` and the loading scene (GameExport.cpp:181-229).
- The shell: `EditorPanelRegistry` (descriptors with `MenuPath`, `OpenByDefault`); `EditorShell` (85 lines) for dialogs;
  `editor.showDialog`'s enum is `export, newProject, openSample, recovery, newScript, externalEditor, about`
  (EditorLayer.cpp:201). **Docked panels are not drawn while the launcher shows** (only File and Help menus there):
  a window that must work without a project is a modal dialog drawn by `EditorLayer`, like `ExternalEditorDialog`.
- `EditorUISourceTests.cpp:631` ("Panels change files only through commands") scans `Panels/` for file writes.
  `HeaderWeightTests.cpp:48-57` keeps `EditorContext.h` declaring, not including, `Editor/ExternalEditor.h` and others;
  its `c_FullJsonHeaders` (13 entries, from :32) lists the only engine headers allowed to include the complete JSON header,
  each with its reason.
- MCP: `c_CoreCommands` holds 27 commands (`StrataCLI/src/CLI/McpToolsets.cpp:17-28`); with the 5 generic tools that is
  the 32-tool limit `EditorMcpToolsetsTests.cpp:65-67` checks. **Core is full.** Toolsets are mapped by exact name and
  by prefix (`project.` goes to `assets`).

### 2.3 Per-user settings

- `<user data>/Strata/EditorSettings.json` holds only `{"ExternalEditor": ...}`, no format header
  (`ExternalEditor.cpp`, 393 lines): read once through `Platform::ReadTrustedFile`, written through
  `Platform::WritePrivateFile` keeping other keys; `STRATA_EXTERNAL_EDITOR` wins. The path reaches the context through
  `EditorContextSpecification::EditorSettingsFile` (empty: in memory, the tests' pattern).
- The file is shared by every editor version on the machine (`FindUserDataDirectory("Strata")`, ExternalEditor.cpp:167):
  0.5.0 reads only the top-level `"ExternalEditor"`, and when it writes it keeps the other keys and sets or erases that
  top-level key (:325-335).
- UI scale is not persisted: the display's, or `--ui-scale`. `ImGuiLayer::SetContentScaleOverride` already changes it
  at run time ("e.g. a user's choice", ImGuiLayer.h:60-62). There is one theme palette.
- The only preference UI is `ExternalEditorDialog` (Scripts > External Editor..., `script.setExternalEditor`).

### 2.4 Collision layers (per scene today)

- `PhysicsLayerSettings` (`Strata/src/Strata/Scene/PhysicsLayerSettings.h/.cpp`, 96/77 lines, std only): 32 names
  and a symmetric matrix of masks, `ValidateName`, `FindLayer`. It is `SceneSettings::PhysicsLayers` (Scene.h:69), and
  the serializer writes `Settings.PhysicsLayers {Names, NoCollisions}` for **every** scene, as an empty table when it is
  the default (`WritePhysicsLayers`, SceneSerializer.cpp:170-188, written unconditionally at :280; read :204-263); the
  streaming writer writes `Settings` for every scene (SceneDocumentWriter.cpp:366-371) and cooked scenes keep and read
  them (CookedScene.cpp:926-927).
- Readers: physics (`CombineLayerMasks` ANDs the process default `PhysicsSettings::LayerCollisionMasks` with the
  scene's, PhysicsSystem.cpp:47; `SyncLayers` per fixed step), scripts (`ScriptHostAPI.cpp:2340-2386`, three host
  functions, `SetLayersCollide` writes the running scene's matrix), the editor (`CommandUtils.cpp:510-517` layer names
  in `component.set`, `PropertyWidgets.cpp:242-262`, `InspectorPanel::DrawCollisionLayers` :1339-1495,
  `scene.setSettings`'s `layerNames`, `resetLayerCollisions`, `layerCollisions`, `scene.info`'s `physicsLayers`): 18 uses
  of `SceneSettings::PhysicsLayers` in 7 engine and editor files, and 4 test files.
- Which table applies: the scene's. Sub-scenes and world cells use the host's ("its settings (gravity...) are not
  applied", Scene.h:201; cells are written with `WriteSettings = false`, EditorWorld.cpp:604). A scene switch brings the
  new scene's names, so `Physics::GetLayerIndex("Terrain")` is -1 in a scene that does not name it, and prefabs (which
  store indices) mean whatever the instantiating scene says.
- Data: PhysicsPlayground `Main.stscene` names 1 "Ball", 2 "Ghost" with `NoCollisions [[1,2]]` (its command script
  checks `GhostsOnPitFloor == 8`); OpenWorld `Worlds/Island.stscene` names 1 "Terrain" (`Wander.cpp` looks it up);
  `Feature.stscene` names 2, 3 and 7 with two pairs (checked at `FeatureTestUtils.cpp:446-451`). ThirdPerson's
  `Course`, `Loading` and `MainMenu.stscene` and OpenWorld's `Scenes/Loading.stscene` store the default table
  (`{"Names": {}, "NoCollisions": []}`); Tetris's `Main.stscene`, saved before layers existed, stores no key. So at most
  one scene per project stores a table other than the default. Older documents put `Settings` after `Entities`
  (Feature.stscene), the streaming writer before them (SceneDocumentWriter.cpp:366-373).

### 2.5 Input and UI actions

- `Strata/src/Strata/Input/` (1,723 lines): `Input` is one static class (`s_State`), queried by fixed codes; the
  Input layer may include only Core (`StrataTests/Architecture/Layers.json`). Application's "Input" stage runs
  `Input::BeginFrame()` then `Window::ProcessEvents()`; an input frame is a `BeginFrame` that is not suspended
  (`GetFrameIndex`). The editor suspends input frames while play is paused without a pending step
  (EditorContext.cpp:1431; Input.h:118-125: device events keep updating the state meanwhile).
- `UIActions.h` (80 lines): `UIAction` (Submit, Cancel, Up, Down, Left, Right), the `constexpr c_UIActionMap` whose
  comment says it is "written as data in the shape an input-actions asset will have"; binding strings `Key/<name>`,
  `Gamepad/<button>`, `Gamepad/<axis>+|-`; no `Mouse/`. `UIActionState` (repeat 0.4 s then 0.1 s, stick hysteresis
  0.5/0.35) lives in `GameUISystem` (`m_Actions`).
- **Bug:** `GameUISystem::OnUpdate` returns before `m_Actions.Update` when no canvas exists (GameUISystem.cpp:184-197
  vs :214), so `UI::IsActionPressed` keeps the last update's `Pressed` while there is no UI.
- Scripts: ABI version 1, 142 host functions (counted in `StrataScriptHostAPI` and `ST_SCRIPT_HOST_FUNCTIONS`); the
  tail ends with the `World*` functions and the marker at ScriptABI.h:608. `UIIsActionPressed/Down` take a
  `StrataScriptUIAction`. The SDK's `Input` (StrataScript/Input.h, 367 lines) has dead zones only for sticks.
- Saves: `IGameSaves::GetSaveDirectory()` (EngineServices.h:119-126); `SaveFiles` stores `<name>.save` and `List`
  returns only those (SaveFiles.cpp:18, :185). The editor's provider is `ProjectGameSaves` (`<project>/.strata/Saves`).
- Editor: `input.*` commands (EditorInputCommands.cpp, 1,150 lines) simulate keys, buttons, axes and the mouse, held in
  fixed steps (`SimulatedInput`); none names actions.
- In-process game loops outside `Application` start input frames themselves: the feature test's
  `PlayFeatureScene` calls `Input::BeginFrame` (FeatureTestUtils.cpp:390), and 13 test files call it in all
  (ScriptAPITests, ScriptShellTests, ScriptUITests, ScriptHelperTests, InputTests, UIActionTests, FeatureTestUtils,
  GameUIInputTests, GameUIPerfTests, EditorUITests, EditorGameShellTests, EditorInputTests, EditorUICommandTests). The
  feature test provides only `IGameWindow` and `IGameSaves` (FeatureTests.cpp:211-219) and loads the project with
  `Project::Load` and an `EditorAssetManager` (FeatureTestUtils.cpp:315-330), not through `EditorContext`.
- Samples read raw codes. **Bugs:** `ThirdPersonPlayer.cpp:192` subtracts `LeftY` ("The stick's Y grows downward"),
  but the engine reports +Y up since da56e6b2 (GLFWWindow.cpp:317-321): pushing the stick forward walks backward.
  `FollowCamera.cpp:55` subtracts `RightY` from `Pitch` (negative looks down, :23), so pushing the stick up looks down,
  while the mouse (:52, moving down looks down) is not inverted with `InvertY` off. No command script moves a stick, so
  nothing catches either.

### 2.6 Exported executables

- `GameExport` (StrataEditorCore, `GameExport.cpp` 728 lines) copies `StrataRuntime` byte for byte to
  `<ToFileName(Name)>` (+`.exe`) (:589-599) and adds the exec bits on POSIX. `ToFileName` keeps ASCII letters, digits,
  space, `-`, `_`.
- Windows: `StrataRuntime.rc.in` gives every game the strata mark as `GLFW_ICON` (GLFW's Win32 backend uses that name
  for the window class) and `VS_VERSION_INFO` "Strata Runtime" / "Strata" with the engine's version; the linker embeds
  `CMake/Strata.manifest` (RT_MANIFEST). `StrataRuntime.Icon` (StrataTests/CMakeLists.txt:505) checks the exported
  SmokeGame carries the mark (`--strata-test-helper=check-icon`, Windows only).
- macOS: a bare Mach-O with an ad-hoc linker signature; patching it would invalidate the signature, which arm64 enforces.
  `GLFWWindow::SetIcon` returns early on Cocoa and Wayland (GLFWWindow.cpp:557-562). There is no Objective-C in the
  engine. Core links CoreFoundation on Apple (Strata/CMakeLists.txt:53), and every program links Core, StrataCLI
  included (through `Strata::Network`, StrataCLI/CMakeLists.txt:15); the App layer owns `Platform/GLFW/**` and links
  GLFW, which brings Cocoa.
- Linux: no window icon (the runtime never calls `SetIcon`; `WindowSpecification::IconPath` exists, Window.h:66), no
  `GLFW_X11_CLASS_NAME` or `GLFW_WAYLAND_APP_ID` hint (GLFW 3.4 has both), no desktop entry.
- Helpers that exist: `Renderer/ImageReader` (stb_image decode), `Renderer/ImageWriter::EncodePNG`, stb_image_resize2
  (compiled in `vendor/Implementations`), `Tools/GenerateBrandAssets.py` (the brand `.ico`: sizes 16-64 as 32-bit
  bitmaps, 256 as PNG; `c_EditorIconSizes` in EditorIcon.h).
- The feature project is exported and run: `StrataEditor.FeatureTest` exports it, `StrataRuntime.FeatureTest` runs the
  export with `-DEXPECT` log lines and `-DEXPECT_PATHS` (StrataTests/CMakeLists.txt:638-655). `FeatureTest.stproj` has no
  game identity.

### 2.7 Constraints that shape the design

- Layers: Project may include Core, App, Asset, Reflection (not Scene or Input); Engine (the composition root) may not
  include Project; Input only Core; App Core and Input; GameUI and Scripting include Input; `MaxAllowlistEntries` is 0.
- `Engine::RegisterBuiltinModules` calls Scene, Renderer, Physics, Audio, Animation, Scripting, World, GameUI
  (BuiltinModules.cpp:60-73); Physics, Audio and Animation are optional (`#if ST_WITH_<MODULE>`, and
  `ModuleRegistrationOptions::Exclude`); Scene and Renderer cannot be excluded (`c_RequiredModules`). Input has no
  registration function.
- Engine services (`Core/EngineServices.h`): process-wide interfaces, `ScopedProvider` for what a running program
  provides (`IGameWindow`, `IGameSaves`), **main thread only** (EngineServices.h:16).
- Scenes are built on the asset loader's worker: `SceneAsset::Deserialize` creates and fills the scene there
  (Prefab.cpp:368, Prefab.h:123-126), and so does `CookedSceneReader::ReadScene` for exported games
  (CookedScene.cpp:924-927); `SceneLoadState::LoadingScene` is "read and its scene prepared (on the loading worker)",
  then `Preparing` runs `SceneLoadOptions::Prepare` (`PrepareSystems` creates physics bodies) on the main thread
  (SceneLoader.h:19-47).
- AGENTS.md is 39,622 characters of its 40,000 (`Docs.AgentGuide`): new rules are paid for by shortening.
- Shards (`StrataTests/CoreShards.json`): `Core*`, `Project*` (Scene shard), `Physics*`/`Scripting*` (Systems),
  `GameUI*`/`Runtime*` (Renderer), `Editor.UI*`, `Editor.Export*` (EditorData), other `Editor*`; own CTests for `GPU*`
  (label `gpu`, sharing `Tests::GPUContext`), `FeatureTest`, `Perf.*`. CI excludes `gpu|perf` (ci.yml:191). Every suite
  below fits an existing prefix except `Platform.DockIcon`, which gets its own CTest (S11).

## 3. Design decisions

### D1 Settings sections: a registry in Core, values read through an engine service

`Strata/Core/SettingsRegistry.h` (Core) holds section descriptors; `Strata/Core/ProjectSettings.h` declares the engine
service `IProjectSettings` and `ProjectSettingsValues`.

```cpp
enum class SettingsScope : uint8_t { Project, User };
enum class SettingsFieldType : uint8_t { Bool, Int, Float, String, Enum, Asset };
struct SettingsField   // a simple section's member: validated, described and drawn from this
{
	std::string Name, Title, Description;            // Description is written for agents
	SettingsFieldType Type;
	double Min, Max;                                 // Int, Float
	uint32_t MaxLength;                              // String
	std::vector<std::string> Options;                // Enum
	std::string AssetType;                           // Asset: AssetTypeToString's name ("Texture")
	nlohmann::json Default;
};
struct SettingsSection
{
	std::string Id;          // "<Module>.<Name>": "Scene.CollisionLayers", "Input.Actions", "Editor.Interface"
	std::string Title, Category, Description;
	SettingsScope Scope;
	uint32_t Version = 1;    // stored with the value; Read upgrades older ones
	std::vector<SettingsField> Fields;          // simple sections
	// Complex sections (no Fields) give the first three, and References when the value names assets:
	nlohmann::json Schema;                      // JSON Schema of the value, for commands and agents
	std::function<nlohmann::json()> MakeDefault;
	std::function<std::optional<nlohmann::json>(const nlohmann::json& value, uint32_t version, std::string* error)> Read;
	std::function<std::vector<UUID>(const nlohmann::json& value)> References;  // asset handles the value names
};
class IProjectSettings   // provided while a program runs a project (ScopedProvider); main thread only, like every service
{
public:
	virtual const nlohmann::json* FindSection(std::string_view id) const = 0;  // validated value; null: the default.
	                                                                           // Valid until GetVersion() changes
	virtual uint64_t GetVersion() const = 0;                                   // changes when any section changes
};
```

- Modules register their sections in their registration functions; the process registry freezes with the component
  registry at the end of `RegisterBuiltinModules`. The editor registers its preference sections in its `Extra`
  registration.
- Everything that validates takes a `const SettingsRegistry&` (default: the process registry): `ProjectSettingsValues`,
  `Project`'s section handling, `GameRuntime`'s manifest validation (`GameRuntimeOptions`) and the editor's settings
  pages. Unit tests therefore build a local registry with a simple and a complex test section in-process; the
  child-process helper (`--strata-test-helper=settings-section`, the `custom-component` pattern of
  `StrataTests/src/Engine/ModuleRegistrationHelpers.cpp`) is kept for what must go through the process registry: a
  section a game registers through `Extra`, seen by the editor's commands.
- Modules **pull**: a consumer reads its section through `EngineServices::Get<IProjectSettings>()` **on the main
  thread** and keeps its parsed form until `GetVersion()` changes. Worker threads never read it (D7 says how scenes,
  which are built on the loader's worker, get their layers). The providers are the editor's `EditorContext` (while a
  project is open), `GameRuntime` (from the manifest) and in-process harnesses (D11). `ProjectSettingsValues` (Core) is
  the plain implementation all use.
- `Read` is the single validation: project load, the runtime's manifest check, commands and the panel all go through
  it. It never asserts; it returns the reason.
- **References:** fields of type `Asset` report their handle automatically; complex sections that name assets give
  `References`. The project's references are every set section's, normalized as `AssetManager::NormalizeReferences`
  does (each valid handle once). The project referencer and the export's roots follow them (D16), as the asset rules
  require of a stored form that references assets.
- Headers: `SettingsRegistry.h` needs the complete JSON type (`SettingsField::Default`, `SettingsSection::Schema`) and
  joins `HeaderWeightTests`' `c_FullJsonHeaders` with that reason; `ProjectSettingsValues` keeps its JSON in a private
  implementation, so `ProjectSettings.h`, `Project.h` and `GameManifest.h` stay on `<nlohmann/json_fwd.hpp>`.

Rejected:
- *The registry in Reflection with `PropertyInfo` fields:* the Input layer (Core only) could not register, and
  `PropertyInfo` carries entity and component concerns settings do not have. `SettingsField` is the small subset the
  generic page and the JSON Schema need.
- *Push (each section's `Apply` callback called by the providers):* every program would have to call every module's
  apply at start, change and close, in order; pulling through an engine service is the documented pattern for what
  lower layers need from a program, and costs one pointer read and an integer compare per use.
- *Values as typed members of `ProjectConfig`:* Project may not include the modules (layers), and a section added by a
  later stream would have to edit Project.
- *Tests registering sections only in child processes:* every `Project.*`, `Runtime.*` and page test would spawn a
  process; a registry parameter keeps them in-process.

The project's own fields (name, start and loading scenes, export inclusions, the game's identity, D16) stay typed in
`ProjectConfig`: they are Project's, the export and the runtime read them typed, and Project has no registration
function.

### D2 Storage: `.stproj` version 3 and manifest version 5

- `.stproj` gains `"Settings": { "<Id>": { "Version": n, "Value": {...} } }` and `"Game"` (D16). A section is stored
  once it was set; resetting it removes it (absent: the module's default). **Format version 3**: 0.5.0's `Save` drops
  unknown keys, so a version it accepts would lose the sections silently; a version it refuses fails loudly instead.
  Versions 1 and 2 load, and opening one runs the collision-layer migration once (D8).
- `Project` keeps what it does not know: sections no registered module claims (a build without a module) and unknown
  members of `"Project"` are written back unchanged. A section whose `Read` fails keeps its stored JSON (never
  overwritten unless the person changes it), its default applies, and the editor reports the problem when the project
  opens (`project.open`'s result `settingsProblems`, a warning in the log).
- `GameManifest` **version 5** gains `"Settings"` (every project section the project sets, validated by the export) and
  the identity of D16. `GameManifest` keeps the sections as raw JSON (bounded as below) and never consults the
  registry: the runtime reads the manifest in `CreateApplication`, before the modules register (2.1). `GameRuntime::Create`,
  which reads it again after registration, validates every registered section through `Read`; one that fails stops the
  game through `Application::ReportFatalError` naming the section (the export wrote it, so a bad one is a broken
  install, and a game with broken bindings must not start silently). Sections no registered module claims are ignored.
  Versions 1-4 load; a version-4 manifest gets no `IProjectSettings` provider, so its packed scenes keep their own
  stored layers (D7).

Rejected: *separate files per section* (`ProjectSettings/<Id>.json`): fewer merge conflicts in teams, but several
files to keep consistent on undo and to version; the sections are small (bounded below) and one atomic file keeps one
write per change. *A version-2 file with optional keys* (the "Export" precedent): silent loss in 0.5.0. *Validating
the manifest's sections in `GameManifest::Load`:* the first load runs before registration, so it would read a registry
before the composition root filled it.

Bounds (checked by `Read`, the loaders and the commands): a section's stored JSON at most 512 KiB (compact), the project
file and the manifest's `Settings` at most 1 MiB, the preferences file 64 KiB. A change that would take the project
file over its bound is refused naming the bound. The largest section the stream defines, a maximal input action set
(D10), is about 240 KB compact, so every set the model allows fits its section.

### D3 Writing: a settings file writer off the main thread

`Editor/SettingsFileWriter` (StrataEditorCore) writes the project file and the preferences file: the main thread hands
it a snapshot (copies of the small in-memory values), a job (`JobSystem::SubmitIO`) dumps and writes it atomically
(`FileSystem::WriteText` for the project, `Platform::WritePrivateFile` for preferences), the result is published in
`Update`. Writes coalesce (one in flight, the newest snapshot next). `project.save` (new, Edit, MCP `settings` toolset)
writes the project file now.

- **Waiting.** Commands wait through `CommandUtils::AfterSaves` (a deferred poll, never `Wait`), like documents of
  the `DocumentSaver`. `EditorContext::ReleaseProject`, reached by commands only after `AfterSaves` found nothing in
  flight and otherwise only on the editor's shutdown, waits for a write still in flight and, by the job system's rule,
  runs one that has not started itself: that path may write on the main thread, and only that one.
- **Failed writes.** A failed write keeps the change in memory, is reported (a notification, `editor.status`
  `saving`), and is retried with the next change or by `project.save`. Until a later write succeeds the file has
  **unwritten settings**, and `CommandUtils::CheckUnsavedChanges` fails, for the commands that release the project
  (`project.open`, `project.create`, `project.close`, `project.openSample`, `editor.quit`; for `editor.quit` also
  unwritten preferences), naming the file and the error, unless `force: true`; results report `discardedSettings`. The
  UI's unsaved-changes question names the file the same way and passes `force` only once the person answered. Commands
  that keep the project open (`scene.open`, `scene.new`) lose nothing and are not refused for it.

Existing `project.setStartScene` and `project.setExportSettings` move onto it (and gain undo, D4): one write path for
the project file, and the main-thread write of 0.5.0 goes.

Rejected: *extending `DocumentSaver`*: it publishes assets of the asset manager (scans, references, reserved handles);
the project file is none of these. *Commands that defer until the file is written:* commands do not queue behind a
deferred command, so a rollback after a failed write could undo a later change; keeping the change, reporting the
failure and guarding the project's release loses nothing.

### D4 Commands, undo, play mode and the MCP toolset

| Command | Kind | What it does |
| --- | --- | --- |
| `project.settings` | Query | The registered project sections (id, title, description, schema, value, whether default, problems) and the project's own fields; `section` narrows to one |
| `project.setSettings` | Edit | Replaces one section's value (`section`, `value`; `reset: true` for the default) |
| `project.setCollisionLayers` | Edit | Names (`names {"<0-31>": name or null}`), `resetCollisions`, `collisions [{layers: [a or name, b or name], collide}]` (D7) |
| `inputActions.setMap` | Edit | Adds, renames, removes a map; sets whether it starts enabled (D15) |
| `inputActions.setAction` | Edit | Adds, renames, moves, removes an action; sets kind, dead zone, bindings (D15) |
| `inputActions.resetPlayerBindings` | Session | Deletes the editor's players' bindings file and, while playing, restores the project's bindings (D13) |
| `project.setGameInfo` | Edit | `title`, `version`, `company`, `copyright`, `icon` (a texture or null) (D16) |
| `project.save` | Edit | Writes the project file now (D3) |
| `preferences.get` | Query, session results | The registered user sections and their values |
| `preferences.set` | Session | Replaces one user section's value |
| `input.action` | Session (play) | Simulates an action (D15) |

- Every command validates fully before changing anything (`InvalidParameters`, `Failed`), through the section's `Read`.
- **Project changes are undoable**: one `ProjectSettingsAction` per successful command (before and after values of the
  section or field), `ChangesScene() == false`, so the scene stays saved; Execute and Undo set the value and queue a
  write. They are the project's committed content, and a wrong click in the layer matrix or a removed action must come
  back with Ctrl+Z. Like asset steps, they go when a scene opens (`ResetScene` clears the stack); documented.
- **Preference changes are not undoable** (Session): the person's own environment, not the project's content, reverted
  in place in the same dialog; this matches `script.setExternalEditor` today.
- **While playing**, project changes are allowed, persist (they are not scene edits, so no `RunningCopy`), and reach
  the running game at once: actions through `IProjectSettings`' version (D11), layers through a refresh of the running
  copy (D7). Their steps join the edit history like asset steps recorded in play (AssetEdit.cpp:243): Undo and Redo are
  refused while playing (EditorContext.cpp:1671-1678), so they are undone after `play.stop`. The command descriptions
  say so, and the guide's "Edits while playing are not recorded" (DeveloperGuide.md:1744) becomes "Scene edits while
  playing are not recorded" with a sentence on project and asset steps.
- **MCP:** a new toolset `settings` ("Project settings, collision layers, input actions, the game's identity and the
  editor's preferences"): exact names for the `project.*` commands above (exact wins over the `project.` prefix),
  prefixes `inputActions.` and `preferences.`. `input.action` stays in `play`. Core is unchanged (32 of 32).
- `editor.showDialog` gains `preferences` (with an optional `section`); `externalEditor` stays and opens Preferences at
  that section. `editor.showPanel` opens `ProjectSettings` through the panel registry.
- `editor.status`' `project` section reports the settings file's state (writing, unwritten, the last error);
  `project.info` the game's identity.

### D5 Project Settings panel and Preferences dialog

- **Project Settings** is a panel (`EditorPanelRegistry`, id `ProjectSettings`, PascalCase like the other panels' ids, not open by default): it needs a project,
  and a docked, non-modal window can stay open while playing to tune actions and layers. File > Project Settings...
  opens it.
- **Preferences** is a modal dialog drawn by `EditorLayer` (`PreferencesDialog`, `EditorShell::ShowPreferencesDialog`):
  it must work from the launcher, where docked panels are not drawn. File > Preferences... in both the launcher's and
  the project's menu bar; Scripts > External Editor... opens it at that section.
- Both: a section list on the left (grouped by `Category`, a search field), the selected page on the right. Pages:
  - built-in pages for the project's own fields: **Game** (start scene and loading scene from S3; title, version,
    company, copyright and the icon with its preview from S5) and **Export** (always-include list);
  - a **generic page** for every simple section, drawn from its `Fields` with the widget kit (`UI/Widgets.h`) and the
    asset picker of `PropertyWidgets`; it runs `project.setSettings` or `preferences.set` by the section's scope;
  - **custom pages** for complex sections, registered by the editor UI in an `EditorSettingsPageRegistry` keyed by
    section id (Collision Layers, Input Actions, External Editor). A complex section without a custom page fails
    `Editor.UI.ProjectSettings` "Every registered section has a page" (registration is checked by a test, not at run
    time), so **each section's page lands in the commit that registers the section**: External Editor in S3, Collision
    Layers in S4, Input Actions in S7 (the composition root starts calling `RegisterInputModule` in that commit, D9);
    `Editor.Interface` (S11) is simple and uses the generic page.
- Every edit runs its command through the command runner with `CommandOrigin { CommandSource::UI }`; pages draw from a
  copy of the section values refreshed when `IProjectSettings::GetVersion()` (or the preferences' version) changes,
  never parsing per frame. Colors from theme tokens, sizes from the font, probe keys `ProjectSettings.<Id>.<Field>` and
  `Preferences.<Id>.<Field>`.

### D6 Preferences store

`Editor/EditorPreferences` (StrataEditorCore, forward-declared in `EditorContext.h`) owns a new file,
`<user data>/Strata/Preferences.json`: `{"Strata": {"Format": "Preferences", "Version": 1}, "Settings": {...}}` with the
same `{Version, Value}` envelope. `EditorSettings.json` stays 0.5.0's: when `Preferences.json` does not exist, the store
imports its top-level `"ExternalEditor"` once (read only) and never writes it, so a 0.5.0 editor on the same machine
keeps its setting and its writes never meet the new file. Read through `Platform::ReadTrustedFile` (64 KiB), written
through D3 with `Platform::WritePrivateFile`; the folder is found with `Platform::FindUserDataDirectory` (no shared temp
fallback). `EditorContextSpecification` names both files (empty: preferences in memory and no import, the tests'
pattern). `ExternalEditorLauncher` reads and writes its value through the store; `STRATA_EXTERNAL_EDITOR` still wins and
`script.setExternalEditor` stays (the same store). Sections:

- `Editor.ExternalEditor` (S3): the external editor (the custom page is today's dialog content).
- `Editor.Interface` (S11, cuttable): `Scale`, an enum of percentages of the automatic scale (Automatic, 75, 90, 100,
  110, 125, 150, 175, 200), applied through `EditorHost::SetUIScale` (new) to `ImGuiLayer::SetContentScaleOverride`;
  `--ui-scale` wins for its session.

The theme has one palette, so there is no theme preference.

Rejected: *upgrading `EditorSettings.json` in place* (a 0.5.0 editor reads only its top-level key and would lose the
setting, and its later writes would add back a top-level key the new reader must reconcile); *mirroring the top-level
key on every write* (two copies of one value, and which one wins after a 0.5.0 write is a guess).

### D7 Collision layers: one project table, a working copy per scene, applied on the main thread

- The table is the project section `Scene.CollisionLayers` (title "Collision Layers", category Physics), registered by
  `RegisterSceneModule`. Scene is a required module and owns `PhysicsLayerSettings`, the parser, the working copy and
  every reader of names (`component.set`, the Inspector's combo, `scene.info`), so a build or run without Physics
  (`STRATA_WITH_PHYSICS` off, `ModuleRegistrationOptions::Exclude`) keeps the project's names, and no lower layer depends
  on a section a higher, optional one validates. Its value has the JSON shape scene files use today
  (`{"Names": {"<i>": name}, "NoCollisions": [[a, b]]}`), whose reader and writer move from `SceneSerializer.cpp` to
  `PhysicsLayerSettings::FromJson/ToJson`.
- `SceneSettings::PhysicsLayers` goes. `Scene` holds a **working copy**, `Scene::GetPhysicsLayers()` (mutable, not
  saved, not in scene undo or the recovery journal), and the document's stored table as plain data
  (`Scene::GetStoredPhysicsLayers()`, empty when the document stores none or the default table). The 18 uses in 7 files
  (2.4) move from `GetSettings().PhysicsLayers` to the copy; physics keeps ANDing the process default
  `PhysicsSettings::LayerCollisionMasks`, so there are still two matrices (the process default, unchanged and set only by
  tests, and the scene's copy of the project's), never three.
- **Where the copy comes from.** Scenes are built on the loader's worker and engine services are main-thread only
  (2.7), so the worker never reads `IProjectSettings`: it parses the document's stored layers (validated as today) into
  the stored table and makes them the working copy, which is all a program without a provider gets (tools, tests, a
  version-4 manifest: 0.5.0's behaviour). The project's table is applied **on the main thread** by
  `Scene::ApplyProjectLayers()` at every hand-over:
  - `SceneLoadRequest::Update`, when the worker's scene arrives, before the first preparation hook (`PrepareSystems`
    creates bodies there), and `TakeScene` again when the table changed since: the editor's opens, the runtime's
    scene switches and exported games' loads all go through the loader;
  - `EditorContext::ResetScene`, for every scene the editor adopts (`scene.new` included);
  - after `project.setCollisionLayers`, its undo and its redo, for the edited scene and the running copy (a script's
    change in the running copy is replaced; documented).
  `Scene::Copy` copies the working copy. The parsed table is cached on the main thread (`ProjectCollisionLayers`, Scene
  layer), reparsed when `IProjectSettings::GetVersion()` changes, with a table version that changes only when the table
  itself does; a scene records the table version its copy came from, so an edit of another section (actions while
  playing) never replaces a script's change.
- **Stored layers.** `SceneSerializer` stops writing `PhysicsLayers`; it still reads the key. A stored table equal to
  the default (no names, empty `NoCollisions`, what every 0.5.0 scene without named layers stores, 2.4) counts as
  storing nothing. When a provider exists and a non-default stored table differs from the project's, `ApplyProjectLayers`
  reports it and the editor logs one warning naming the scene the first time it loads in a session (saving the scene
  drops the key); exported games apply the project's table without a warning (their scenes were cooked from the
  project, and a log line per load would only repeat what the editor said).
- Scripts: `Physics::GetLayerIndex`, `SetLayersCollide`, `GetLayersCollide` act on the running scene's copy, so a name
  means the same in every scene and prefab. A script's `SetLayersCollide` lasts for the scene it runs in, as in 0.5.0
  (a switch takes the project's table again). The ABI is unchanged.
- Editor: `project.setCollisionLayers` takes the parameters `scene.setSettings` had (the parsers `ReadLayer`,
  `ReadLayerNames`, `ReadLayerCollisions` move with them); `scene.setSettings` no longer accepts `layerNames`,
  `resetLayerCollisions` or `layerCollisions` and refuses them with `InvalidParameters` naming the new command (no
  forwarding: one command per change, and a scene command must not write the project file). `scene.info` keeps
  reporting `physicsLayers` (the scene's copy). `component.set` layer names and the Inspector's layer combo read the
  copy, i.e. the project's names.
- The Inspector's scene section loses Collision Layers (`DrawCollisionLayers` moves to the Project Settings page
  `Scene.CollisionLayers`, same matrix and naming UI, now running `project.setCollisionLayers`) and shows one row
  "Collision layers: <n> named" with a link that opens the page.
- World cells are sub-scenes of the persistent scene and use its physics world and copy; exported games read the table
  from the manifest (D2) and every scene they load takes it at hand-over.

Rejected: *taking the copy when the `Scene` is constructed* (the constructor runs on the loader's worker: a data race
with the main thread's changes, and `FindSection`'s pointer into JSON the main thread replaces); *a snapshot of the
table in `SceneLoadOptions` when the load starts* (stale by hand-over if the table changed meanwhile, and it misses
scenes built outside the loader); *a project matrix ANDed with per-scene matrices* (keeps every 0.5.0 scene's exact
matrix but leaves two tables to edit and explain, and names would still need one owner); *the table only in the engine
service with no per-scene copy* (scripts' runtime changes would leak across scene switches and into the editor's edited
scene); *forwarding `scene.setSettings`' layer parameters* (two commands for one change); *registering the section in
`RegisterPhysicsModule`* (optional, while its type and readers are Scene's).

### D8 Migration of 0.5.0 projects

A project file read with **format version 1 or 2** (`Project::Load` knows the version it read) runs the migration when it
opens (every path: `project.open`, `project.openSample`, `--project`, the launcher), as an upgrade job before the start
scene loads (`project.open` already defers). The job walks the asset directory for `.stscene` files and reads each one's
`Settings` with `SceneDocumentReader` (stopping at the first entity when `Settings` come first, as `SceneDocumentWriter`
writes them; older documents and world cells, which have no `Settings`, are read to the end, once per project). A stored
table equal to the default counts as storing nothing, for names and matrix (D7). Then, on the main thread:

- names: per index, the start scene's name, else the first by path; a different name for the same index elsewhere is a
  conflict;
- matrix: a pair is disabled in the project when any scene disabled it; scenes whose non-default matrices disagree are
  a conflict;
- the section is set when any scene stored layers (no undo step: the project did not have one), and the project file is
  written as version 3 **in every case**, also when no section results: the version records that the migration ran, so
  it runs once per project. A version-3 file never migrates: not a project created by this editor (`Project::Create`
  writes version 3, so new projects and the perf fixtures, `WorldFixture.cpp:532`, :602, are never scanned), and not one
  whose section was reset (absent means the default; the scenes' stale keys never come back);
- scene files are not touched (their stale key is ignored and dropped at their next save);
- the result reports `migrated: {collisionLayers: {scenes, conflicts}}`; conflicts are logged as one warning listing
  the pairs and names; a project whose scenes store nothing logs nothing.

The samples and the feature test project are migrated in place once (opened with the new editor, in step S4) and the
migrated `.stproj` files committed, so tests that copy them never migrate. They stay at version 2 until then: no earlier
step rewrites them, which is why the identity step (S5, which sets the samples' game info) comes after S4. A version-3
file written by this branch's own builds before S4 skips the migration; no release writes one (0.5.0 writes 2, 0.6.0
migrates). For every committed project the rule gives exactly its 0.5.0 behaviour, because at most one scene per project
stores a table other than the default (2.4).

Rejected: *triggering on the section's absence* (a project whose scenes name no layers gets no section, so ThirdPerson,
Tetris and every new project would be rescanned at every open, the 1M-entity world fixture's measured launch included;
and a section reset to its default would bring the scenes' stale keys back over the person's choice); *lifting each
scene's layers when that scene first opens* (the project's table would depend on the order scenes are opened in);
*rewriting the scene files* (opening a project would touch every scene; it changes nothing behaviour needs).

### D9 Input actions are a project settings section, not an asset type

The actions are the section `Input.Actions`, stored in the project file and the manifest.

- A game has one set of actions, needed from its first frame: an asset loads asynchronously, so menus would have no
  navigation until it loaded, and the built-in UI map would need a second, compiled-in source.
- The Input layer may include only Core. A settings section needs only a parser in Input; an asset type needs an
  `Asset` subclass and loader in a layer that sees both Input and Asset (GameUI, Scripting or a new layer) around a
  model that lives in Input.
- Players' bindings are keyed by action name; one set leaves no question of which asset is active.

Rejected: *an InputActions asset type* (several sets per project and copying between projects by file; it would need
the `.meta`, loader, references, export inclusion and the feature test's asset-type coverage). Groups are maps within
the one set; copying sets between projects is deferred (section 9).

`RegisterInputModule` (new, `Strata/Input/InputRegistration.cpp`) registers the section; the composition root calls it
first, and it joins `c_RequiredModules` (every engine has input). The composition root starts calling it in the commit
that adds the section's editor page (S7, D5); until then the Input tests reach the section through a local registry
(D1), and every program evaluates the default set.

### D10 Model and binding grammar

- `InputActionSet { Maps }`, `InputActionMap { Name, Enabled (at start), Actions }`, `InputAction { Name, Kind, Bindings,
  DeadZone }`, `Kind` is `Button`, `Axis` (one value, -1 to 1) or `Vector2`.
- Names: 1-64 characters of letters, digits, space, `_`, `-`, `.`, no leading or trailing space. Map names are unique in
  the set and action names unique within their map (ignoring case). An action is named `<Map>/<Action>` (`/` cannot
  occur in a name), or by its bare name when that is unique in the whole set; an ambiguous bare name is refused naming
  every match ("'Left' is ambiguous: Gameplay/Left, UI/Left"). So a game may have its own `Left`, `Right`, `Up`, `Down`
  or `Cancel` beside the UI map's. Limits: 32 maps, 256 actions, 8 bindings per action.
- One text grammar for the project file, commands, scripts and the players' file (`InputBinding::Parse/ToString`, at
  most 96 bytes: the longest valid text, a 2D composite of four half axes such as `Gamepad/RightTrigger+`, is 90),
  extending the UI map's:
  - `Key/<InputNames key>`, `Mouse/<InputNames mouse button>`, `Gamepad/<button>` (any gamepad);
  - `Gamepad/<axis>` (whole axis), `Gamepad/<axis>+` or `-` (half axis; triggers are `+`), `Gamepad/LeftStick` and
    `Gamepad/RightStick` (two axes, +Y up);
  - composites `1D:<negative>,<positive>` and `2D:<up>,<down>,<left>,<right>`, whose parts are button-like bindings
    (keys, mouse buttons, gamepad buttons, half axes).
- A set at every limit (32 maps, 256 actions of 8 bindings of 96 bytes, 64-character names) is about 240 KB compact,
  inside the section bound of 512 KiB (D2); a test round-trips one through the project file.
- Which bindings fit which kind: Button takes button-like bindings; Axis takes those (as 1), whole and half axes and
  1D composites; Vector2 takes sticks and 2D composites. `Read` refuses a binding its action's kind cannot use.
- Values: Button is down when any binding is (half axes with the UI's hysteresis, press 0.5, release 0.35); Axis takes
  the binding of the largest magnitude; Vector2 the largest length, 2D composites normalized (a diagonal has length 1).
  Axis and Vector2 apply the action's dead zone (default 0.15, radial for sticks, rescaled, as the SDK's
  `GetGamepadStick`) and are also down (with Pressed and Released) beyond 0.5 with the same hysteresis.
- No modifiers or chords (no action needs them; section 9). Mouse motion and scroll stay on the raw API.

Rejected: *action names unique across the whole set*: the reserved UI map would take `Left`, `Right`, `Up`, `Down` and
`Cancel` from every game (a platformer's or Tetris' natural names).

### D11 Evaluation once per input frame, in the Input layer

`InputActions` (Input layer, static like `Input`): the active set, per-action state (down, pressed, released, value),
map enables, the players' bindings, simulated actions and the capture.

- `Application`'s Input stage calls `InputActions::Update()` after `ProcessEvents` (App may include Input; it already
  calls `Input::BeginFrame`). Update evaluates actions only when `Input::GetFrameIndex()` advanced, so suspended (paused)
  frames keep their state and transitions are per input frame, like keys. Evaluation needs no time step (repeat is the
  UI's, D12), so it is deterministic for given input. The capture (D13) is evaluated at every call, also while input
  frames are suspended.
- Update first checks `IProjectSettings::GetVersion()` and reparses `Input.Actions` when it changed (the editor's edits
  reach a running game at once); without a provider, or without the section, the default set (the UI map) applies. So
  **every program evaluates the UI map every input frame** (section 6's A/B measures it).
- Reads go through the gameplay queries, so keyboard and pointer capture by the game UI hide bindings as they hide keys.
- **In-process loops** that start input frames themselves call `InputActions::Update()` where `Application` does, after
  the frame's input is in: the feature test's `PlayFeatureScene` (after `Input::BeginFrame` and its simulated input),
  and every loop among the 13 test files of 2.5 that reads actions or UI navigation. Those that run a project also
  provide `IProjectSettings`: the feature test's `FeatureProject` provides it from the loaded `Project` before
  `LoadStartScene` (S4). AGENTS.md's "code that runs a game in-process provides `IGameWindow` and `IGameSaves`" becomes
  "provides `IGameWindow`, `IGameSaves` and `IProjectSettings` and updates input actions after its frame's input".
- Programs: `EditorContext` resets the session state on play start and stop (map enables to their defaults, simulated
  actions, capture) and loads the players' bindings at play start; `GameRuntime` loads them when it opens.
- Cost: at most 256 actions of 8 bindings, flat arrays resolved at parse time; a provisional perf metric (section 6).

Rejected: *lazy evaluation at the first query of a frame* (stick hysteresis would compare against the last evaluated
frame, so a press could appear frames late); *a scene system* (actions are per process like input, and two running
scenes would evaluate twice); *evaluating inside `Input::BeginFrame`* (the frame's device events arrive after it).

### D12 The UI map

- Every set contains the reserved map `UI` with the six actions `Submit`, `Cancel`, `Up`, `Down`, `Left`, `Right`
  (Button kind), whose default bindings are today's `c_UIActionMap` (it becomes `InputActions`' default data). Their
  names, kinds and map cannot change and they cannot be removed (`Read` adds missing ones with defaults); their bindings
  can. The game UI reads them by their qualified names (`UI/Submit`...), so a game's own actions of the same bare names
  never interfere. The panel marks them "used by game UI navigation".
- `UIActionState` keeps the repeat (0.4 s, 0.1 s, on unscaled time) and reads Down and Pressed from `InputActions`
  instead of compiling `c_UIActionMap`; `UIAction`, `IUIControl::IsActionPressed/IsActionDown`, the
  `UIIsActionPressed/Down` host functions and `UI::IsActionPressed` keep their meaning.
- The stale-state bug (2.5) is fixed: `GameUISystem` updates `m_Actions` before its no-canvas return, with a regression
  test.
- Text fields keep their hard-coded Enter, keypad Enter and Escape (text editing, not navigation).

### D13 Rebinding and the players' bindings file

- A player's bindings replace an action's binding list (validated by its kind); `ResetActionBindings` restores the
  project's.
- Stored at `<IGameSaves directory>/InputBindings.json`
  (`{"Strata": {"Format": "InputBindings", "Version": 1}, "Actions": {"Gameplay/Jump": ["Key/Space"]}}`, keys always
  map-qualified): a fixed name the engine owns, outside `SaveFiles`' `*.save` namespace, so it never appears in
  `Game::ListSaveFiles` and never collides with a game's names; the directory comes from `IGameSaves` as for save files,
  the file name is a constant that passes `SaveFiles::ValidateName` as a stem; written atomically
  (`FileSystem::WriteText`), read bounded (64 KiB).
- Reading is forgiving per entry: unknown actions (the game changed) and invalid bindings are dropped with one warning;
  a garbage file is ignored with a warning and the project's bindings apply.
- Saving is explicit (`Input::SaveBindings()` from the game, e.g. when its options menu closes): the game decides when
  a rebind is final. Documented in the scripting skill.
- **In the editor** the players' file lives in `<project>/.strata/Saves` (`ProjectGameSaves`) and is loaded at every
  play start, so a play session that saved bindings would silently override later edits of those actions on the
  Project Settings page. The editor therefore reports the actions whose bindings the players' file replaces
  (`input.state`'s `playerBindings`, a mark and a "Reset player bindings" button on the Input Actions page), and
  `inputActions.resetPlayerBindings` (Session) deletes the file through `IGameSaves` and, while playing, restores the
  project's bindings.
- **Capture** (the editor's binding capture and the games' rebind menus): `BeginCapture(devices)` watches device
  transitions (keys, mouse buttons, gamepad buttons, axes beyond 0.5 from rest), including while the game's input is
  disabled (the editor in Edit mode) and while input frames are suspended (play paused): it compares the device state
  at every `Update` call with the state when it began, so a capture from the panel works while play is paused. The first
  transition ends it with its binding text. While a capture runs every action reads as up, and the control it captured
  stays up for actions until it is released, so the press that is captured never also triggers an action (Escape does
  not close the menu that asked), also when play resumes with the key still held.

### D14 Script API

Thirteen host functions appended at the end of `StrataScriptHostAPI` and `ST_SCRIPT_HOST_FUNCTIONS` (after
`WorldIsLayerEnabled`), `ST_SCRIPT_ABI_VERSION` stays 1, each wrapped in `HostCall` with `ResolveContext`, misuse
reported through `ScriptSystem::ReportProblem` (once per unknown or ambiguous name) with failure values. Action names
are qualified or unambiguous bare names (D10). The first ten land in S8; the three capture functions and their enum
are S11's last, cuttable extra, appended after them (if cut, a later wave appends them):

```c
bool     InputIsActionDown(ctx, StrataScriptString action);
bool     InputIsActionPressed(ctx, StrataScriptString action);
bool     InputIsActionReleased(ctx, StrataScriptString action);
bool     InputGetActionValue(ctx, StrataScriptString action, float outValue[2]);   /* Button: x 0 or 1; Axis: x */
bool     InputSetActionMapEnabled(ctx, StrataScriptString map, bool enabled);
int32_t  InputIsActionMapEnabled(ctx, StrataScriptString map);                     /* 1, 0, -1 unknown */
uint64_t InputGetActionBindings(ctx, StrataScriptString action, char* buffer, uint64_t capacity); /* text buffer protocol, '\n' after each */
bool     InputSetActionBindings(ctx, StrataScriptString action, StrataScriptString bindings);      /* '\n'-separated */
bool     InputResetActionBindings(ctx, StrataScriptString action);                 /* empty: every action */
bool     InputSaveBindings(ctx);
/* S11: */
bool     InputBeginBindingCapture(ctx, uint32_t devices);                          /* StrataScriptInputDevices bits */
uint64_t InputPollBindingCapture(ctx, char* buffer, uint64_t capacity);            /* 0 while listening or idle */
bool     InputCancelBindingCapture(ctx);
```

- A new ABI enum `StrataScriptInputDevices` (Keyboard 1, Mouse 2, Gamepad 4), with the capture functions.
  `StrataScriptUIAction`'s comments stop listing default bindings (they are rebindable).
- SDK (`StrataScript/Input.h`, through `ST_SCRIPT_DETAIL_HOST_WITH`): `Input::IsActionDown`, `IsActionPressed`,
  `IsActionReleased`, `GetActionValue` (vec2), `GetActionAxis` (float, x of the value), `SetActionMapEnabled`,
  `IsActionMapEnabled`, `GetActionBindings` (vector of strings), `SetActionBindings`, `ResetActionBindings`,
  `SaveBindings`; with the capture: `BeginBindingCapture(InputDevices)`, `PollBindingCapture` (optional string),
  `CancelBindingCapture`, `enum class InputDevices`. Every name is exercised by the feature scripts (SDK coverage) and
  every host function by the feature test, the API test module (`StrataTests/Scripts/API`, a new
  `InputActionScripts.cpp`) and `Scripting.InputActions`, in the step that adds it.

Rejected: *integer action IDs from a lookup function*: IDs would change when the editor edits the set during play, and
the per-call cost of a name lookup (a hash of a short string, a few calls per frame) is negligible.

### D15 Editor: commands, simulated actions, the Input Actions page

- `inputActions.setMap {map, rename?, remove?, enabled?}` and
  `inputActions.setAction {action, map? (to add or move), rename?, remove?, kind?, deadZone?, bindings?: [text]}`,
  both Edit, validated through the section's `Read`, one undo step each; the reserved UI actions refuse renames,
  removal and kind changes. `inputActions.resetPlayerBindings` (D13). (S7, the reset S9.)
- `input.action {action, mode: tap|press|release, steps?, seconds?, value?: [x, y]}` simulates an action directly
  (independent of its bindings), held in fixed steps by `SimulatedInput` like `input.key`; `input.sequence` gains the
  step kind `action`; `input.state` reports `actions` (down, value, map), `maps` (enabled), `playerBindings` and
  `capture`. Simulated keys already reach actions through `Input`. (S9.)
- The Project Settings page `Input.Actions`: maps as a list (enable at start, rename, remove), the selected map's
  actions as rows (name, kind, dead zone, bindings as chips with remove buttons, "+" to add from a combo of
  `InputNames`), composites added part by part (S7). The capture (S9) adds "+" by capture: it uses
  `InputActions::BeginCapture` (D13), takes the keyboard from ImGui while it listens (`ImGui::SetKeyOwner`), so captured
  keys run no shortcut, works while play is paused, and ends after 10 s or with its Cancel button. Actions the players'
  file overrides are marked, with "Reset player bindings" (S9).

### D16 The game's identity

`ProjectConfig::Game` (`ProjectGameInfo`): `Title` (UTF-8, at most 128 characters, no control characters; empty: the
project name), `Version` (one to four dot-separated integers of at most 65,535, default "1.0.0"), `Company` and
`Copyright` (optional, at most 128 characters), `Icon` (a Texture asset of the project, or none). Set by
`project.setGameInfo` (undoable), shown on the Game page and summarized in the Export dialog with a link to it.
`project.setGameInfo` refuses, with `InvalidParameters` and the reason, an icon that is a sub-asset (a texture of a
glTF model has no source file of its own) or whose source is not PNG, JPEG, TGA or BMP (what the icon conversion
decodes, D20); it reads only the asset's metadata, so it never blocks the frame, and a source that still fails to decode
fails the export with the reason.

- The manifest's `Name` stays the project name: it keys the save and log directories, so changing the title or the
  company never moves players' saves. The manifest gains `Title`, `Version`, `Company`, `Copyright` and `Icon` (a
  file next to it).
- The runtime: the window title is `Title`; the log banner names title and version; the window icon is the manifest's
  icon file (`WindowSpecification::IconPath`); `Application::ReportFatalError` dialogs use the title.
- References: asset deletes refuse an asset the project names, and pruned exports keep it. `AssetReferences::Find`
  gains a project referencer (S2), which also closes a 0.5.0 gap: it names the start scene, the loading scene and every
  set section's references (D1), and the icon from S5. The export adds the sections' references to the pack's roots
  beside the start scene, the loading scene and `AlwaysInclude`.
- The icon need not be in the pack (pruned or not): the export writes `<File>.png` (256 x 256, D20) next to the
  manifest on every platform, and the runtime reads that file.

### D17 Windows: the executable's resources, rewritten in engine code

Exports copy a prebuilt runtime, so the game's resources are written into the copy. `Editor/PEResources` (StrataEditor
core, platform-independent C++, no Win32 API, so it builds and is tested on every platform) reads a PE32+ image and
writes a new one with replaced resources; the export uses it on Windows hosts in place of `FileSystem::Copy` (read,
stamp in memory, one atomic write).

- Reading validates everything it touches: `MZ` and `e_lfanew` within the file, the `PE\0\0` signature, the COFF header
  (AMD64 or ARM64), the PE32+ optional header and its data directory count, the section table within `SizeOfHeaders`,
  each section's raw range within the file, the resource directory inside one section, a tree of at most three levels
  with entry counts bounded by the directory's size, each directory visited once (no cycles), name strings and data
  entries within the section. Any violation is an error naming the field; nothing asserts.
- Writing replaces `RT_GROUP_ICON` `GLFW_ICON` and the `RT_ICON` entries it references (new IDs) when the project has an
  icon, and `RT_VERSION` 1 always; every other resource (the `RT_MANIFEST` of `Strata.manifest` above all) is kept byte
  for byte. The new `.rsrc` is rebuilt in place; sections after it (the linker puts only `.reloc` there) move up by the
  growth with their file offsets, RVAs and data directory entries updated (relocations name RVAs of other sections, so
  moving `.reloc` changes no entry); `SizeOfImage`, `SizeOfInitializedData` and the resource directory's size follow;
  the checksum is recomputed. A layout the writer does not support (another section after `.rsrc`, overlay data, a
  certificate table) fails the export with the reason instead of writing a damaged executable; the runtime CMake build
  is unsigned, so the certificate case means someone signed it and must sign the game after export.
- `VS_VERSIONINFO`: `FILEVERSION`/`PRODUCTVERSION` from `Version` (missing parts 0), `StringFileInfo` 040904B0 with
  `CompanyName`, `FileDescription` (title), `FileVersion`, `InternalName` (project name), `LegalCopyright`,
  `OriginalFilename` (`<File>.exe`), `ProductName` (title), `ProductVersion`; `VarFileInfo` 0x409, 1200. Strings are
  converted from UTF-8 to UTF-16 by the engine's own conversion.
- The subsystem, the manifest and the code are untouched: `StrataEditor.Install`'s subsystem check of the exported game
  and `EndToEnd.StandardStreams` keep passing.

Rejected: *Win32 `BeginUpdateResourceW`* (Windows only, so the writer could not be tested on Linux and macOS CI jobs,
and it hides the layout decisions); *appending a second resource section* (simpler, but resource tools look for one
`.rsrc` and the old section stays as dead bytes); *compiling a resource script at export* (needs the platform SDK).

### D18 macOS: the Dock icon at run time, no bundle (S11, cuttable)

The bare executable must not change (its ad-hoc signature), and an app bundle is game-shell's. So:

- `SetApplicationIcon(width, height, pixels)` lives in the **App layer**, next to its only caller:
  `Platform/GLFW/GLFWApplicationIconMacOS.cpp` (declared in `Platform/GLFW/GLFWApplicationIcon.h`; elsewhere it returns
  false), with AppKit linked to App on Apple. Core stays without AppKit, so programs that link Core but no window
  (StrataCLI and its MCP server) do not load it; App already brings Cocoa through GLFW. `GLFWWindow::SetIcon` calls it
  on Cocoa instead of returning, so the game's Dock icon is its icon while it runs, and the editor's Dock icon becomes
  the strata mark (visible to the owner at once).
- The implementation includes only `<objc/runtime.h>` and `<objc/message.h>` and no CoreFoundation, CoreGraphics or
  AppKit header: CompatLint compiles the macOS branches with Homebrew GCC 14 (CompatLint.py:9-10), which accepts the
  Objective-C runtime headers but fails on CoreGraphics' blocks syntax. The image is built as an `NSBitmapImageRep`
  (`initWithBitmapDataPlanes:...`, RGBA, 8 bits per sample) wrapped in an `NSImage` (`initWithSize:` with a locally
  declared struct of `NSSize`'s layout, two `double`s) and set with `NSApplication`'s `setApplicationIconImage:`, every
  `objc_msgSend` cast to the exact signature of its call.
- The title, version and company reach the window title, the log and the manifest; Finder's icon and version need the
  bundle (section 9).

Rejected: *an Objective-C++ (`.mm`) file*: CompatLint's GCC cannot compile it, and the C runtime functions suffice for
a few calls. *The function in Core* (`Platform/MacOS`): AppKit would load into every program that links Core. *Finder
icons through extended attributes of the executable:* lost when archived, and a stop-gap for the bundle.

### D19 Linux: window icon, application id, desktop entry

- The runtime sets the window icon from the manifest's icon file (X11; GLFW ignores it on Wayland), and the window hints
  `GLFW_X11_CLASS_NAME` and `GLFW_WAYLAND_APP_ID` to the executable's file name (`WindowSpecification::AppId`, new) (S5).
- The export writes `<File>.desktop` next to the game (`Type=Application`, `Name` = title, `Exec` and `Icon` the
  export directory's absolute paths quoted per the Desktop Entry rules, `StartupWMClass`, `Terminal=false`,
  `Categories=Game;`), so a person can install it; moving the folder means editing it, which the README of exports
  says. Installing it into `~/.local/share/applications` is game-shell's packaging. Writing it is platform-independent
  text generation (`Editor/DesktopEntry`), tested everywhere, used on Linux hosts (S11, cuttable).

### D20 Icon conversion

The icon's source is the texture's source file (the asset manager's import source; `project.setGameInfo` already
refused sub-assets and other formats, D16), decoded with `ImageReader` (PNG, JPEG, TGA, BMP; at most 4,096 x 4,096 and
16 MiB), made square by centering on transparent pixels, resized with stb_image_resize2 in linear premultiplied alpha
to 16, 20, 24, 28, 32, 40, 48, 56, 64 (32-bit bitmaps with an AND mask in `RT_ICON`) and 256 (PNG, through
`ImageWriter::EncodePNG`), the sizes of the brand icon. A source under 256 pixels is upscaled with a warning in the
export result; a source that does not decode fails the export with the reason. The 256-pixel PNG is the runtime's
`<File>.png`.

## 4. Deliverables and acceptance

Each step is one or more commits that build and pass every suite with the lint clean, and could land alone. Suite names
are doctest suites; new cases are named as they will be. Steps are numbered in delivery order (section 10).

**S1 Settings registry, the engine service, project file v3, manifest v5.** `Core/SettingsRegistry`,
`Core/ProjectSettings` (`IProjectSettings`, `ProjectSettingsValues`), the registry parameter (D1), `Project` sections
and unknown keys kept, `GameManifest` v5 `Settings` kept raw, `GameRuntime` validating and providing them. Tests use
local registries with a simple and a complex test section. *Accept:*
- `Core.Settings`: "A simple section validates its fields and reports the first bad one", "A section's Read upgrades
  an older version", "Registering after the registry froze fails", "Section ids follow <Module>.<Name>", "Asset fields
  and References report a section's references, each once".
- `Project.Settings`: "Sections round-trip through the project file", "A section the build does not know survives a
  save", "Unknown members of Project survive a save", "A section that fails Read keeps its stored value and reports it",
  "Version 1 and 2 project files load; saving writes version 3", "A project file over 1 MiB is refused".
- `Project.GameManifest`: "Version 5 settings round-trip unvalidated", "Version 4 manifests load without settings",
  "Manifest settings over 1 MiB are refused".
- `Runtime.Game`: "An exported game provides its manifest's settings while it runs, and a version-4 manifest none", "A
  manifest section that fails Read stops the game naming it" (moved here from the manifest: D2).
- `Architecture.HeaderWeight`: `SettingsRegistry.h` joins `c_FullJsonHeaders` (14 entries) with its reason;
  `ProjectSettings.h`, `Project.h` and `GameManifest.h` include only `<nlohmann/json_fwd.hpp>`.

**S2 Editor commands, undo and the writer.** `SettingsFileWriter`, `ProjectSettingsAction`, `project.settings`,
`project.setSettings`, `project.save`, `setStartScene`/`setExportSettings` on the writer with undo, the unwritten-settings
guard in `CheckUnsavedChanges`, the `EditorContext` provider, the project referencer and the export's section roots,
the MCP `settings` toolset, the child-process helper that registers a game's test section (with an Asset field).
*Accept:*
- `Editor.ProjectSettings`: "setSettings rejects a bad value and changes nothing", "One undo step per change, and the
  scene stays saved", "Undo and redo restore the section and rewrite the file", "The project file is written by a job,
  never by the main thread" (the test changes a value, polls `Update` until the write is published, then checks the
  thread the writer's test hook recorded), "A failed write keeps the change and reports it; the next change retries",
  "project.close after a failed write fails unless force", "project.close waits for a write in flight", "A section
  registered by a game appears in project.settings and is validated by its fields" (child process), "Deleting an asset
  a section names is refused" (child process), "Deleting the start or loading scene is refused while the project names
  it", "Changes while playing persist, report no RunningCopy and are undone after play stops".
- `Editor.Export`: "A pruned export keeps an asset a section names" (child process), and "An export refuses a project
  whose section fails Read, naming it" (child process: S1's export copies the project's sections into the manifest and
  refuses invalid ones, testable only once a process registry has a section).
- `Editor.ProjectSettings`: "project.open reports settingsProblems for a section that fails Read" (child process; S1's
  `Project::Load` already logs the warning and lists `GetSettingsProblems`).
- `Editor.ProjectSettings`: "setStartScene and setExportSettings record undo steps and no longer write on the main
  thread"; the existing callers (EditorCommandTests, EditorExportDialogTests, EditorGameFlowTests) pass.
- `CLI.McpToolsets` (EditorMcpToolsetsTests.cpp) passes with core unchanged (27 commands, 32 tools) and maps every new
  command to the `settings` or `play` toolset.
- *As built (what later steps build on):* `SettingsFileWriter` is generic (a write function per snapshot; S3's preferences
  file uses its own instance); project changes go through `ChangeProjectSettings` (the project's fields,
  `ProjectSettingsFields`, which S5 extends with `Game`) or `ChangeProjectSection` (a section's stored entry), so
  `project.setCollisionLayers` (S4) and `project.setGameInfo` (S5) record their steps the same way;
  `CheckUnsavedChanges` takes a `DiscardScope` and the UI asks through `EditorShell::RequestReleaseProject` (S3 adds
  unwritten preferences to `editor.quit`'s question there); `EditorContextSpecification::Settings` gives the editor a
  local registry in tests; `Tests::RunInSettingsSectionProcess` runs a test case again in the
  `--strata-test-helper=settings-section` child, whose process registry has `Game.Tuning`.

**S3 Preferences and the two windows.** `EditorPreferences` (`Preferences.json`, the one-time import),
`preferences.get/set`, `Editor.ExternalEditor`, `PreferencesDialog`, the `ProjectSettings` panel with the Game page
(start and loading scene) and the Export page, the generic page, `EditorSettingsPageRegistry`, menu items,
`editor.showDialog preferences`. *Accept:*
- `Editor.Preferences`: "A 0.5.0 EditorSettings.json is imported once and left as it was" (a file a 0.5.0 editor wrote
  after the import changes nothing), "The file is private and read bounded", "STRATA_EXTERNAL_EDITOR still wins", "An
  empty path keeps preferences in memory", "Preference commands are Session commands and record no undo".
- `Editor.UI.ProjectSettings` (ImGuiHarness): "File > Project Settings opens the panel", "The Game page sets the start
  and loading scenes through their commands", "Editing a generic field runs its section's command" (a local registry's
  simple project section drawn by the generic page), "Every registered section has a page", "No id conflicts at 100%
  and 150%".
- `Editor.UI.Preferences`: "File > Preferences opens from the launcher and from a project", "Scripts > External Editor
  opens Preferences at its section".
- `Editor.Launcher` keeps its 22 cases; `EditorUISourceTests` (theme, font-relative sizes, registry, no file writes in
  `Panels/`) and `HeaderWeightTests` (`EditorContext.h` declares `EditorPreferences`) pass.
- *As built (what later steps build on):* the panel's id is `ProjectSettings` (`EditorPanels::c_ProjectSettings`, docked
  beside the Scene view); both windows draw a `SettingsBrowser` (`UI/SettingsPages.h`), and a complex section's page is a
  `SettingsPage` registered in `EditorLayer`'s constructor (`m_SettingsPages.Register(id, factory)`, the factory taking
  the `EditorContext`, the section and the window), as the External Editor's is: S4's Collision Layers page and S7's
  Input Actions page register there. Generic pages run a section's command on each finished edit. `EditorOptions::Settings`
  gives a `HarnessEditor` a local registry. Preferences have `preferences.save` (the quit question's Retry) besides
  `preferences.get/set`; `editor.quit` guards unwritten preferences (`DiscardScope::Editor`, `EditorShell::RequestQuit`).

**S4 Collision layers for the project, with the migration.** The section in `RegisterSceneModule`, the working copy
applied at hand-over, `Scene::GetStoredPhysicsLayers`, the readers, `project.setCollisionLayers`, `scene.setSettings`
without layers, the Collision Layers page and the Inspector's link, the version-triggered upgrade job, the feature
test's provider; the samples and `FeatureTest.stproj` migrated in place and committed, and `Feature.stscene`'s
`PhysicsLayers` key removed. *Accept:*
- `Physics.Layers`, new: "A scene loaded through SceneLoader takes the project's table at hand-over, also when the
  table changed while it loaded", "Scenes loaded through SceneLoader read the project settings only on the main thread"
  (a test provider records the threads that read it), "A scene's stored layers do not replace the project's, and a
  difference is reported", "A document storing the default table reports nothing", "Without a project a scene file's
  layers apply as in 0.5.0", "A script's SetLayersCollide lasts for its scene, and an edit of another section keeps
  it", "Sub-scenes and world cells collide by the host's table".
- Existing cases, each with what it becomes (golden rule 2: none is dropped or weakened silently). `Physics.Layers`:
  "Layer settings keep the matrix symmetric and find layers by name" unchanged; "Layer names and the matrix round trip
  through scene files" becomes "Layers round-trip through the project section (PhysicsLayerSettings::ToJson/FromJson),
  and saved scenes store none"; "Scenes saved before layer settings load with every layer colliding, and invalid
  entries are reported" unchanged (documents are still read and validated); "Bodies on layers the scene's matrix keeps
  apart pass through each other", "Changes of the scene's matrix while it plays apply at the next step, also back",
  "Changing the matrix while bodies touch ends only the pairs whose layers it parts", "Letting two layers collide while
  the scene plays makes their bodies collide" and "The default settings' matrix still restricts every scene's" keep
  their meaning on the scene's copy. `Editor.PhysicsLayers`: "scene.setSettings names layers and changes the matrix in
  one undo step" becomes "setCollisionLayers names, resets and sets pairs with one undo step" and "scene.setSettings
  refuses layer parameters naming the new command"; "scene.setSettings sets sub-scene creation and collision layers
  together, saved with the scene" becomes "scene.setSettings sets sub-scene creation, saved with the scene, and the
  saved scene stores no layers"; "Bodies and character controllers take their layer by name" becomes "... by the
  project's layer names" (with "component.set accepts a project layer name"); "Layer changes while playing reach the
  running physics and end with play mode" becomes "A change while playing reaches the running copy and persists after
  play mode"; "The Inspector names collision layers and edits the matrix, one undo step per edit" becomes "The
  Collision Layers page names layers and edits the matrix through setCollisionLayers, one undo step per edit" and "The
  Inspector links to the page"; "The Inspector offers a body's layer by the scene's layer names" becomes "... by the
  project's layer names"; "The layer matrix fits long names into a compact grid, elided where too long" is unchanged,
  on the page.
- `Editor.ProjectMigration`: "A 0.5.0 project's scene layers become the project's", "Different names for one index are
  reported; the start scene's wins", "A pair any scene disabled is disabled in the project", "A 0.5.0 project whose
  scenes store no layers or the default table gets no section and is written as version 3", "A version-3 project
  without the section is not migrated", "A migrated or new project is not scanned again", "Resetting the layers survives
  a reopen", "The upgrade reads scenes on a job before the start scene opens", "Copies of PhysicsPlayground, OpenWorld
  and the feature test migrate to their committed sections", "A copy of OpenWorld loads Loading.stscene without a layer
  warning".
- FeatureTest: `FeatureProject` provides `IProjectSettings` from the loaded project before `LoadStartScene`;
  `FeatureTestUtils.cpp`'s layer checks read `scene.GetPhysicsLayers()`, and with `Feature.stscene` storing no layers
  they prove the table came from `FeatureTest.stproj`; new case "Every registered project section is set in
  FeatureTest.stproj with a non-default value".
- `Scripting.Physics` layer cases and `Scripting.Module` pass; `Runtime.Game` "An exported game's second scene collides
  by the project's table"; `StrataEditor.PhysicsPlayground` (`GhostsOnPitFloor == 8`) and `StrataEditor.OpenWorld`
  (`Wander`'s Terrain raycasts) pass.
- A/B (section 6): `Perf.Physics.*`, `Perf.Scene.*` and `PerfGPU.Editor.WorldOpen1M*` against the base build; ratios in
  the report, no metric expected to move.

**S5 The game's identity in exports.** `ProjectGameInfo`, `project.setGameInfo` (with its icon checks), the Game page's
identity fields with the icon's preview, the icon in the project referencer, manifest identity, the runtime's title,
log, window icon and app id (D19), the icon conversion and `<File>.png`; each sample gets a title, a version and an icon
(a 256-pixel `viewport.capture` of its own scene imported as `Icon.png`, credited in its `CREDITS.md` as a render of
the sample, set through `project.setGameInfo`), and `FeatureTest.stproj` gets a title, version, company, copyright and
`Textures/Checker.png` as its icon. Comes after S4: the samples' `.stproj` must be migrated before this step rewrites
them as version 3 (D8). *Accept:*
- `Project.Settings`: "Game info validates title, version, company and copyright".
- `Editor.ProjectSettings`: "project.setGameInfo refuses a sub-asset texture and an icon source it cannot convert".
- `Editor.UI.ProjectSettings`: "The Game page sets the icon through the asset picker".
- `Editor.Export.GameIcon`: "A square icon is resized to every size", "A non-square icon is centered on transparent
  pixels", "A small icon is upscaled with a warning", "An icon that does not decode fails the export".
- `Editor.Export`: "The manifest carries title, version, company, copyright and the icon file; the PNG decodes at 256
  pixels", "Deleting the icon texture is refused while the project names it".
- `Runtime.Game`: "The window title is the game's title", "Changing the title or the company keeps the save and log
  directories", "The window icon comes from the manifest's icon file".
- `StrataEditor.Export` (ExportCommands.json.in) exports SmokeGame a third time with game info and checks the files;
  `StrataRuntime.FeatureTest` expects the log banner's title and version and `<File>.png` next to the exported feature
  game (`-DEXPECT`, `-DEXPECT_PATHS`).
- *As built (what S6 builds on):* `Project/GameInfo.h` (`ProjectGameInfo`: `Validate`, `ValidateText`, `ValidateVersion`,
  `ParseVersion` giving the four 16-bit numbers of `FILEVERSION`); `Editor/GameIcon` (`FindSource` from metadata,
  `Convert`/`ConvertFile` giving `Images::Sizes`, one RGBA8 straight-alpha image per `c_Sizes` = 16 to 64 and 256, and
  `EncodePNG`), which the export calls in its I/O job before the pack; `GameExportResult::Icon` and `Warnings`;
  `Runtime/GamePresentation` (the runtime's title, banner, data directory name, icon file and `WindowSpecification::AppId`).
  The third SmokeGame is `ExportSmoke/BuildIdentity/SmokeGame` (title "Smoke Adventure", version 2.0.1, company and
  copyright "Strata Tests"), whose icon source is `StrataTests/FeatureTest/Assets/Textures/Checker.png` (64 x 64, so the
  export enlarges it with a warning); `StrataRuntime.Identity` runs it headless and checks its banner and `SmokeGame.png`.
  ThirdPerson's title is its game's name, "Sky Islands".

**S6 Windows executable resources.** `PEResources`, the `VS_VERSIONINFO` builder, `RT_ICON`/`RT_GROUP_ICON` from the
icon, the export on Windows hosts; a test-only PE image builder (`StrataTests/src/TestHelpers`) that makes valid PE32+
images with `.text`, `.rsrc` (icons, a version, a manifest) and `.reloc`. Pushed to `ci/project-settings` as soon as the
writer lands, and again after the export stamps. *Accept:*
- `Editor.Export.ExecutableResources` (every platform): "Stamping replaces the icon group and version and keeps every
  other resource byte for byte", "Sections after .rsrc move with their directories", "SizeOfImage and the checksum
  follow", "A second stamp of a stamped image gives the same bytes", "Unsupported layouts (a section after .reloc,
  overlay, a certificate table) fail with the reason", "Version strings are UTF-16 and the version quad comes from
  Version".
- `Editor.Export.ExecutableResources.Sweep`: every truncation of the header region and byte flips across the
  directories and resource tree fail cleanly.
- Windows only: `StrataRuntime.Icon` stays (SmokeGame has no icon: the strata mark); `StrataRuntime.GameIcon` checks the
  third SmokeGame's icon sizes and pixels against its source (`check-icon` takes a PNG); `StrataRuntime.VersionInfo`
  (`--strata-test-helper=check-version`, `GetFileVersionInfoW`) checks the strings; `StrataRuntime.Smoke` runs the
  stamped game; `StrataEditor.Install` (label `package`) keeps its subsystem checks.

**S7 Input actions in the Input layer, and their editor page.** `InputActionSet`, `InputBinding`, `InputActions`
(evaluation, maps, qualified names, players' bindings, simulated actions, the capture), `RegisterInputModule`, the UI
map, `UIActionState` on actions, the stale-state fix, `InputActions::Update` in `Application` and in the in-process loops
(D11); then `inputActions.setMap/setAction`, the Input Actions page without the capture, and an `Input.Actions` section
in `FeatureTest.stproj` (a map with a Button, an Axis 1D composite and a Vector2 stick and 2D composite), in the commit
where the composition root starts registering the section. *Accept:*
- `Core.InputActions`: "Binding text round-trips for every control and composite", "Read refuses names, kinds and
  bindings that do not fit, with the reason", "The UI map is added when missing and cannot lose an action", "A gameplay
  action named Left coexists with UI/Left, and an ambiguous bare name is refused naming both", "A set at every limit
  fits its section bound and round-trips through the project file", "Button, Axis and Vector2 values combine bindings
  as specified", "Dead zones are radial for sticks and rescaled", "Half axes press at 0.5 and release at 0.35",
  "Pressed and Released last one input frame and survive suspended frames", "Disabled maps' actions read as up",
  "Keyboard capture hides key bindings", "Players' bindings replace an action's list and reset restores it", "The
  bindings file round-trips, drops unknown actions and invalid bindings with one warning, and a garbage file changes
  nothing", "A capture returns the first control and no action sees it", "The captured control stays up for actions
  until it is released", "A capture runs while input frames are suspended", "A new project version reparses the set at
  the next update".
- `Core.InputActions.Sweep`: truncations and byte flips of binding text, section JSON and bindings files never crash and
  never accept an invalid set.
- `Core.UIActions` adapted (the map is data of the default set); `GameUI.Navigation`: "Rebinding Submit to another key
  submits with it", "UI actions do not stay pressed while no canvas exists" (regression for 2.5).
- `Editor.InputActions`: "Maps and actions are added, renamed, moved and removed with one undo step each", "The UI
  map's actions refuse renames and removal", "Bindings are validated by kind", "An edit while playing changes the
  running game's actions in the next frame".
- `Editor.UI.ProjectSettings`: "The Input Actions page edits maps, actions and bindings through the commands".
- `Engine.Modules`: the module list has Input first and refuses excluding it. `Perf.Input.Actions256` provisional
  (section 6). FeatureTest passes (the section is set with non-default values; the in-process loop updates actions).
- A/B (section 6): GameUI and editor frame metrics against the base build.
- *As built (what later steps build on):* `Input/InputActionSet` (the model, `InputBinding::Parse/ToString/FitsKind`,
  `InputActionSet::FromJson/ToJson/FindAction/GetQualifiedName/IsReservedAction/MakeSection/GetDefaultUIActions`,
  `InputPlayerBindings::Read/Write` for `InputBindings.json`); `Input/InputActions` (static: `Update`, `Reset`, which
  `Input::Reset` calls, `ResetSession`; `GetState`, `IsDown`, `IsPressed`, `IsReleased`, `GetValue` and
  `GetQualifiedName` by name with the reason on failure; `SetMapEnabled`/`IsMapEnabled`; `GetBindings`, `SetBindings`,
  `ResetBindings` (empty name: every action), `GetPlayerBindings` (those that apply, for S9's marks), `SaveBindings`,
  `LoadBindings`; `SimulateAction(name, down, value)`, `ReleaseAllSimulated`, `ClearSimulated` for S9's `input.action`;
  `BeginCapture(InputCaptureDevices { Keyboard, Mouse, Gamepad })`, `IsCapturing`, `TakeCapturedBinding` (the text once),
  `CancelCapture` for S9 and S11, whose `StrataScriptInputDevices` bits map to that struct); the capture reads
  `Input::IsRawKeyDown`, `IsRawMouseButtonDown`, `IsRawGamepadButtonDown` and `GetRawGamepadAxis`. The commands live in
  `Editor/EditorInputActionsCommands.cpp`, the page in `UI/InputActionsPage` (S9 adds the capture there).

**S8 The script API.** The ten reading and rebinding host functions, the SDK names, `InputActionScripts.cpp` in the API
module, `Scripting.InputActions`, `InputFeatures.cpp` reading `FeatureTest.stproj`'s actions, with a rebind, a save and a
reset driven by simulated input. *Accept:* `Scripting.InputActions` ("Scripts read actions through every host
function", "Unknown names fail and are reported once", "Qualified names resolve and an ambiguous bare name is reported",
"SetActionBindings validates by kind", "SaveBindings writes through IGameSaves and a new session reads it");
`Scripting.API` call counters cover the ten; FeatureTest (host function and SDK coverage, no new warnings) passes;
`ST_SCRIPT_ABI_VERSION` is 1.

**S9 Capture and simulated actions in the editor.** The page's capture, the players' bindings marks and
`inputActions.resetPlayerBindings`, `input.action`, the `input.state` and `input.sequence` additions. *Accept:*
- `Editor.InputActions`: "Player bindings saved in play are shown and can be reset".
- `Editor.Input`: "input.action taps, presses and releases an action for the given steps", "input.sequence runs action
  steps", "input.state reports actions, maps and player bindings".
- `Editor.UI.InputActions`: "The capture binds the next key and runs setAction", "Keys pressed while capturing run no
  shortcut", "Cancel and the timeout end a capture", "A capture while play is paused binds the next key".

**S10 Third-Person Adventure on actions.** Through the editor's commands: an `Input.Actions` section for ThirdPerson
(`Gameplay` map: Move as Vector2 with `2D:Key/W,Key/S,Key/A,Key/D`, `2D:Key/Up,...` and `Gamepad/LeftStick`; Jump, Run,
Wave, Dance, Respawn, Pause as Buttons; Look as a Vector2 on `Gamepad/RightStick`, mouse look staying raw), and its
scripts read them, which fixes both inverted sticks (2.5): Move forward walks forward, and Look up looks up (`Pitch`
grows with the action's Y; `InvertY` still applies). The command script gains two steps that fail on 0.5.0's signs:
`input.gamepadAxis` pushes `LeftY` forward and expects the robot to move forward, and holds `RightY` up and expects
`FollowCamera.Pitch` to increase. *Accept:* `StrataEditor.ThirdPerson` passes (its `input.key` steps reach actions);
`Samples.Content` passes.

**S11 Cuttable extras.** One commit each, delivered in this order and dropped from the end (section 10):
- (a) The macOS Dock icon (D18). *Accept:* `Core.ApplicationIcon` "SetApplicationIcon refuses empty or inconsistent
  images" (every platform); `Platform.DockIcon` "The Dock icon is the image set", its own CTest
  (`StrataTests.DockIcon`, an `OwnTests` entry in `CoreShards.json`, label `gpu`, created on Apple only): it needs the
  window server, uses no GPU (so it is not a `GPU*` suite and needs no `GPUContext`), and runs only on the Mac, since CI
  excludes `gpu`.
- (b) `Editor.Interface` (D6) and `EditorHost::SetUIScale`. *Accept:* `Editor.Preferences` "Scale applies through the
  host and --ui-scale wins"; `Editor.UI.Preferences` "The Interface page's scale runs preferences.set".
- (c) The Linux desktop entry (D19). *Accept:* `Editor.Export.DesktopEntry` (every platform) "The desktop entry parses
  back with escaped values"; `Editor.Export` "On Linux hosts the export writes the desktop entry".
- (d) The script-side binding capture (D14's last three functions, `StrataScriptInputDevices`, the SDK names, the API
  module, a feature script capture driven by simulated input). *Accept:* `Scripting.InputActions` "A capture driven by
  simulated input returns its binding"; `Scripting.API` counters cover the thirteen; FeatureTest coverage passes.

**S12 Docs, skills and the stream's end.** Section 7's list, the Roadmap's Status, the full Debug and Release suites,
the lint, CI green on `ci/project-settings`.

## 5. Must-fail checks

Each is run once on the Mac by breaking the code locally; the report quotes the failure. The Windows-only rows run on
CI through a throwaway `ci/project-settings-mustfail` branch.

| Check | Break | Fails |
| --- | --- | --- |
| Unknown sections kept | rebuild `Settings` from registered sections only | `Project.Settings` "A section the build does not know survives a save" |
| Version bump | keep writing version 2 | `Project.Settings` "...saving writes version 3" |
| Manifest validation | skip `Read` in `GameRuntime::Create` | `Runtime.Game` "A manifest section that fails Read stops the game naming it" |
| Writer thread | write the snapshot in `Update` | `Editor.ProjectSettings` "...written by a job, never by the main thread" |
| Non-scene undo | leave `ChangesScene()` true | "One undo step per change, and the scene stays saved" |
| No partial changes | apply fields before validating all | "setSettings rejects a bad value and changes nothing" |
| Unwritten settings | drop the unwritten-settings check from `CheckUnsavedChanges` | "project.close after a failed write fails unless force" |
| Section references | leave the sections' references out of the export's roots | `Editor.Export` "A pruned export keeps an asset a section names" |
| Main-thread hand-over | read the provider in `SceneAsset::Deserialize` | `Physics.Layers` "Scenes loaded through SceneLoader read the project settings only on the main thread" |
| Project table wins | apply a document's stored layers when a provider exists | `Physics.Layers` "A scene's stored layers do not replace the project's ..." |
| Default table is nothing | count a stored default table as stored layers | `Physics.Layers` "A document storing the default table reports nothing" and `Editor.ProjectMigration` "A copy of OpenWorld loads Loading.stscene without a layer warning" |
| Migration once | trigger the migration on the section's absence | `Editor.ProjectMigration` "A version-3 project without the section is not migrated" |
| Migration matrix | OR the scenes' masks (keep a pair if any scene collides) | `Editor.ProjectMigration` "A pair any scene disabled ..." and the PhysicsPlayground copy case |
| Running copy refresh | skip the refresh after `project.setCollisionLayers` while playing | `Editor.PhysicsLayers` "A change while playing reaches the running copy ..." |
| Feature test provider | remove `FeatureProject`'s `IProjectSettings` provider | FeatureTest's layer checks (`Names[3] == "Triggers"`) |
| Frames, not queries | evaluate actions at their first query | `Core.InputActions` "Half axes press at 0.5 ..." (a press seen frames late) |
| Suspended frames | evaluate while `Input` is suspended | "Pressed and Released last one input frame and survive suspended frames" |
| Capture while paused | evaluate the capture only when the input frame advanced | `Core.InputActions` "A capture runs while input frames are suspended" |
| Capture swallows | let actions see the captured control | "A capture returns the first control and no action sees it" |
| Qualified names | resolve a bare name to its first match | `Core.InputActions` "... an ambiguous bare name is refused naming both" |
| Stale UI actions | update `m_Actions` after the no-canvas return again | `GameUI.Navigation` "UI actions do not stay pressed while no canvas exists" |
| UI map reserved | let `Read` drop a missing UI action | "The UI map is added when missing ..." |
| Bindings file validation | accept an invalid binding from the file | "The bindings file ... drops ... invalid bindings" |
| Host function coverage | remove one feature script call | FeatureTest's host function coverage |
| ThirdPerson stick | restore the subtraction of `LeftY` | `StrataEditor.ThirdPerson`'s forward-stick step |
| ThirdPerson look | keep subtracting the Look action's Y from `Pitch` | `StrataEditor.ThirdPerson`'s look step |
| Resources kept | rebuild `.rsrc` from the replaced types only | `Editor.Export.ExecutableResources` "...keeps every other resource byte for byte" |
| Moved sections | move `.reloc` without updating its data directory | "Sections after .rsrc move with their directories" |
| Image size | leave `SizeOfImage` unchanged | "SizeOfImage and the checksum follow" (and, on Windows CI, `StrataRuntime.Smoke`) |
| Directory cycles | drop the visited-directory check | `Editor.Export.ExecutableResources.Sweep` (a self-referencing directory) |
| Saves keyed by name | key the save directory by the title | `Runtime.Game` "Changing the title or the company keeps the save and log directories" |
| Dock icon | let `GLFWWindow::SetIcon` return early on Cocoa again | `Platform.DockIcon` (Mac only) |

## 6. Budgets

- Untouched: every existing entry's `Measured` and `Budget`.
- New, provisional: `Perf.Input.Actions256`, the median per input frame of evaluating 256 actions of 4 bindings with
  every gamepad connected, added in S7 as `{ "Unit": "us", "Provisional": true, "Target": 20 }`. The Mac median goes
  into the commit message and the report only, never into `Budgets.json`. The reference machine's list in the Roadmap
  names it.
- A/Bs, Release, at least 3 interleaved pairs under `PerfLock`, reported as ratios of the medians:
  - S4: `Perf.Physics.*`, `Perf.Scene.*` and `PerfGPU.Editor.WorldOpen1M*` (the open path gains the version check and
    the layers' hand-over; the 1M-entity world fixture is a version-3 project and must not scan).
  - S7: `Perf.GameUI.*`, `PerfGPU.GameUI.*`, `PerfGPU.Editor.EditFrame*` and `PlayFrame*`, `Perf.Scene.IdlePlayFrame1M`:
    every program now evaluates the UI map every input frame (D11), and UI navigation moves from `GameUISystem`, which
    ran it only while canvases existed, into the Input stage.

## 7. Docs and skills

- `AGENTS.md` (stays under 40,000 characters; each line paid for by shortening the same digest): the Editor digest
  (project settings and preferences are sections modules register in `SettingsRegistry`, read through
  `IProjectSettings` on the main thread; project changes are undoable commands, preferences Session commands; a failed
  settings write blocks releasing the project unless `force`); Scripting ("code that runs a game in-process provides
  `IGameWindow`, `IGameSaves` and `IProjectSettings` and updates input actions after its frame's input"; gameplay and UI
  read input actions where a player could rebind; the players' bindings file is the engine's); the transform contract's
  neighbour (collision layers are the project's; `Scene::GetPhysicsLayers` is a working copy applied on the main
  thread); the command conventions' "while playing, report a `warning` and set `RunningCopy`" says it is for scene
  changes. Details go to the guide.
- `Docs/DeveloperGuide.md`: a "Project settings" section (sections, `Read`, references, the registry parameter, the
  generic and custom pages, storage, versions, the writer and failed writes, adding a section), "Preferences"
  (`Preferences.json`, the import), "Input actions" (model, qualified names, grammar, evaluation, in-process loops,
  capture, players' bindings), the collision-layer paragraphs (hand-over, stored tables, scripts, Inspector, migration),
  Export (identity per platform, the PE writer's supported layout), Command conventions (project settings undo, play
  mode), Undo (DeveloperGuide.md:1744 "Scene edits while playing are not recorded"; project and asset steps recorded in
  play are undone after it), No silent data loss (the unwritten-settings guard), References and bounds (sections'
  references), MCP toolsets (`settings`), the feature test (sections and game info in `FeatureTest.stproj`, the
  section coverage case), Simulated input (`input.action`).
- `Docs/Architecture.md`: the settings flow (registry, providers, pull on the main thread), input actions in the frame
  loop, the UI map, collision layers' ownership and hand-over (replacing :340 "Collision layers are the scene's
  (`SceneSettings::PhysicsLayers` ..." and the `SceneSettings` paragraph's layer sentences at :967-968), the export's
  identity.
- `Docs/Manual/` (Scripting: actions and rebinding; the editor's Project Settings and Preferences).
- Skills: `strata-scripting` (actions API, qualified names, rebind menus, `SaveBindings`; layers project-wide: its
  collision-layer paragraph at :174 switches from `scene.setSettings {layerNames, layerCollisions}` to
  `project.setCollisionLayers`), `strata-editor-automation` (the `settings` toolset, `input.action`,
  `inputActions.resetPlayerBindings`, `project.setCollisionLayers` replacing `scene.setSettings`' layer parameters),
  `strata-make-a-game` (define actions first, set game info before exporting; its collision-layer bullet at :266-267 and
  the PhysicsPlayground line at :414 switch to `project.setCollisionLayers`), `strata-add-component` (only if a
  component's property names a layer: they resolve against the project).
- `Docs/Roadmap.md`: Status when the stream lands; open items (section 9).
- Stale text removed: `UIActions.h:13`'s "Rebinding is not supported yet", the ABI's UI-action binding comments, the
  guide's and Architecture's fixed-map paragraphs.

## 8. Risks

| Risk | Mitigation |
| --- | --- |
| The PE writer's layout assumption (`.rsrc` followed only by `.reloc`) does not hold for some toolchain | refused with the reason, never a damaged file; Windows CI stamps the real runtime in `StrataEditor.Export` and runs it; S6 comes early and is pushed to `ci/project-settings` as soon as the writer lands |
| No Windows machine here: S6 is verified only by CI | the writer is platform-independent and tested on every job; the Windows-only CTests read the result through Win32 |
| A path adopts a scene without applying the project's layers, or reads the provider off the main thread | the hand-over points are listed (D7); `Physics.Layers` loads through `SceneLoader` with a provider that records its reading threads |
| `objc_msgSend` cast wrongly on arm64 crashes | one function, a typed cast per call, no framework headers; `Platform.DockIcon` reads the icon back on the Mac |
| A user's project whose scenes disagree on layers changes behaviour after migration | the AND rule and the conflict report in the open result and the log; scene files untouched, so `git` shows nothing lost |
| A version-3 file written by this branch before S4 skips the migration | only this branch's own builds write one; the committed samples and feature project stay at version 2 until S4 migrates them, and S5 comes after S4 |
| Agents' scripts that set layers through `scene.setSettings` break | `InvalidParameters` naming `project.setCollisionLayers`; skills and guide updated in S4 |
| 0.5.0 editors cannot open migrated projects (version 3) | intended (2.1: they would drop sections); release notes say so |
| Action evaluation shows in frame time (the UI map is evaluated every input frame in every program) | flat arrays, the provisional metric, S7's A/B of the GameUI and editor frame metrics |
| The undo stack's clearing on scene open drops project steps | documented, as for asset steps; the file is written at every step, so nothing is lost |
| A project-file write fails and the person closes the project | the unwritten-settings guard refuses unless `force`, and the UI asks |
| `render-quality` fills AGENTS.md's headroom before this stream lands | each line here is paid for by shortening; `Docs.AgentGuide` re-measured after merging main |
| Gamepads are polled while a shipped game is in the background | unchanged from 0.5.0 (section 9) |
| The Dock icon case needs a window server | its own `gpu`-labelled CTest, run on the Mac only; the validation case runs everywhere |

## 9. Deferred

- **game-shell:** the macOS app bundle (Finder icon, `CFBundleShortVersionString`, `NSHumanReadableCopyright`),
  installing desktop entries, signing exports, quality presets (as a project section through `SettingsRegistry`), input
  only while the game's window has focus.
- Local multiplayer: assigning devices to players, per-player action state; per-world input contexts ("Further").
- Input processors (invert, sensitivity, curves), modifiers and chords, mouse motion and scroll as action sources.
- Layout-aware display names of bindings for games (`Window::GetLayoutKey` is in the App layer, out of scripts' reach).
- Scripts reading the game's title and version.
- Moving PhysicsPlayground, OpenWorld and Tetris onto actions, and an in-game rebind row in ThirdPerson's Settings menu.
- Whatever S11 extras the cut line drops (section 10).
- Copying settings sections between projects; editor shortcut rebinding; theme variants.
- The fixed timestep and gravity as project settings (they change every scene's behaviour; an owner's choice).

## 10. Deliver in this order

Every step keeps every test passing (golden rule 2) and names its commits; each behaviour's docs go with its commit.
The identity and the Windows resources come right after the layers step, before the input actions: they depend on
neither layers nor actions, the Windows writer is the riskiest part and only CI can verify it, and they must follow S4
only because S5 rewrites the samples' project files (D8).

1. **S1** Commits: "Add settings sections and the project settings service" (Core, the registry parameter,
   `Core.Settings`, the header-weight list); "Keep settings sections in the project file" (Project v3,
   `Project.Settings`, guide's project file paragraph); "Carry project settings in the game manifest" (v5 kept raw,
   `GameRuntime`'s validation and provider, `Project.GameManifest`, `Runtime.Game`).
2. **S2** Commits: "Write the project file off the main thread" (writer, `project.save`, the two existing commands with
   undo, the unwritten-settings guard); "Add the project settings commands" (`project.settings/setSettings`, provider,
   project referencer and export roots, `settings` toolset, `Editor.ProjectSettings`, guide's command and MCP
   paragraphs).
3. **S3** Commits: "Keep editor preferences as sections" (store, import, `preferences.*`, `Editor.Preferences`); "Add
   the Project Settings panel and the Preferences dialog" (UI, menus, Game and Export pages, generic page, External
   Editor page, `Editor.UI.*`, Manual page).
4. **S4** Commits: "Make collision layers the project's" (section in Scene, working copy at hand-over, readers, commands,
   page, the feature test's provider and section coverage case, the existing cases replaced as listed, docs); "Migrate
   scene layers into the project when it opens" (version-triggered job, `Editor.ProjectMigration`); "Migrate the samples
   and the feature test project" (in-place opens, committed `.stproj` files, `Feature.stscene` without its layers,
   command scripts green); the A/B.
5. **S5** Commits: "Give projects a game title, version, company and icon" (fields, command, Game page, referencer);
   "Show the game's identity in exported games" (manifest, runtime title, log, window icon, app id, PNG); "Give the
   samples and the feature test titles, versions and icons" (with `StrataRuntime.FeatureTest`'s expectations).
6. **S6** Commits: "Write the game's version and icon into Windows executables" (writer, builder, tests, sweep), pushed
   to `ci/project-settings` at once; "Stamp exported Windows games" (export, Windows CTests). **CI checkpoint:**
   `ci/project-settings` green on every job before S7.
7. **S7** Commits: "Add input actions to the Input layer" (model, grammar, qualified names, evaluation, capture,
   players' bindings, `InputActions::Update` in `Application` and the in-process loops, `Core.InputActions`, sweep, perf
   metric; tests through a local registry); "Read game UI navigation from input actions" (UI map, `UIActionState`, the
   stale-state fix with its regression test); "Edit input actions in the editor" (`RegisterInputModule` in the
   composition root, `inputActions.setMap/setAction`, the Input Actions page, `FeatureTest.stproj`'s section,
   `Editor.InputActions`, `Engine.Modules`); the A/B.
8. **S8** Commit: "Let scripts read and rebind input actions" (ten host functions, SDK, API module,
   `Scripting.InputActions`, feature test, scripting skill and manual).
9. **S9** Commits: "Capture bindings on the Input Actions page" (capture, player-binding marks,
   `inputActions.resetPlayerBindings`, `Editor.UI.InputActions`); "Simulate input actions" (`input.action`, sequence,
   state, `Editor.Input`, automation skill).
10. **S10** Commit: "Move the Third-Person Adventure onto input actions" (section through commands, scripts, both stick
    fixes, the command script's stick and look steps).
11. **S11** Commits, in this order: "Show the game's icon in the macOS Dock"; "Remember the editor's interface scale";
    "Write a desktop entry next to exported Linux games"; "Let scripts capture bindings".
12. **S12** Docs and skills completed, Roadmap Status, full Debug and Release suites, lint, CI green on
    `ci/project-settings`, report.

**Cut line.** The core the stream must land: S1-S10 and S12. That is the settings system, both windows, project-wide
layers with migration, the game's identity in every export including the Windows executable's version information and
icon, input actions with rebinding and saved players' bindings, scripts and the editor (capture and simulated
actions), and a sample on actions. S11's extras may move to a later wave if the usage limit runs out; they are
delivered in the order listed and dropped from the end: first the script-side binding capture (games rebind by
choosing a control and calling `SetActionBindings`; the editor's capture stays), then the Linux desktop entry (Linux
games keep their window icon and application id), then the `Editor.Interface` scale preference (the display's scale and
`--ui-scale` stay), then the macOS Dock icon (the title, log and manifest still carry the identity). Stop only after a
complete step, or a complete S11 commit.

## 11. Merge notes

- `render-quality`'s plan (its worktree's `Docs/Plans/Wave6/specs/render-quality.md`, uncommitted when this was
  written) states that it appends no host functions (:53, :806-808: "the tail is theirs"), gives `SceneSettings`
  nothing (:262), adds no project-wide rendering defaults, and expects to land second (:816). So this stream expects to
  land first, and `render-quality` merges this stream's ABI tail, the version-3 sample `.stproj` files, the new sample
  icons and the AGENTS.md lines.
- Files both streams edit: `Feature.stscene`, the feature scripts, `c_ExpectedLogMessages` and `FeatureTestUtils.cpp`
  (here: the `IProjectSettings` provider, `InputActions::Update`, the layer checks); `InspectorPanel.cpp` and
  `PropertyWidgets.cpp` (here: the layer rows leave `DrawSceneSettings`; there: light and probe rows); `EditorLayer.cpp`
  (menus, panel registration); `McpToolsets.cpp` (both add prefixes); `ProjectTemplates.cpp` (here: project files;
  there: the template's `Post Process`); `EditorStateCommands.cpp` (here: `editor.status`' project section; there: GPU
  stage lists); the samples' scenes and command scripts `StrataTests/Editor/<Sample>Commands.json.in` (here:
  `ThirdPersonCommands.json.in`'s stick and look steps; there: rendering checks); `StrataTests/CMakeLists.txt`,
  `Budgets.json` (appended provisional entries); `AGENTS.md`, `Docs/Architecture.md`, `Docs/DeveloperGuide.md`,
  `Docs/Roadmap.md` Status.
- **The second to land** merges main into its branch and resolves at function level: host functions keep landing order
  (the second stream's block goes after the first's, both appended, `ST_SCRIPT_ABI_VERSION` stays 1); feature-test
  additions are unions; `AGENTS.md` is re-measured (`Docs.AgentGuide`) and the second stream shortens for its own lines;
  then the full Debug suites, FeatureTest and every `StrataEditor.<Sample>` command script run again before it moves
  `main`, through `ci/<stream>`.
- Sample icons: S5 captures each sample's icon from its own scene with `viewport.capture`. `render-quality` changes the
  samples' look (TAA, lights, fog), so whichever stream lands second recaptures the icons with S5's procedure if the
  scenes' look changed.
- If `render-quality`'s plan changes and brings project-wide rendering defaults, they register a section
  (`Renderer.Defaults`) in `RegisterRendererModule` and appear in Project Settings with the generic page, and its
  section is set in `FeatureTest.stproj` (the section coverage case of S4).
- The script ABI stays at version 1; 0.5.0 script modules keep loading (functions only appended).
- Formats: `.stproj` version 3 and manifest version 5 are written from S1 on; both still load every older version.
