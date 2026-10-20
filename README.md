# Strata

[![CI](https://github.com/2042Third/GameEngine/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/2042Third/GameEngine/actions/workflows/ci.yml)

Strata is a 3D game engine written in C++20 for Windows, Linux (Ubuntu 24+) and macOS. It renders through
[NVRHI](https://github.com/NVIDIA-RTX/NVRHI) on Vulkan and comes with an editor that people and AI agents
can drive in the same way, plus a runtime that plays exported games.

## Download

Releases for Windows (x64), Linux (x64, Ubuntu 24.04 or newer) and macOS (Apple silicon) are on the
[releases page](https://github.com/2042Third/GameEngine/releases): unpack the archive and run `StrataEditor`. Its
`README.md` lists what each platform needs (a GPU with a Vulkan 1.2 driver; started without one, the editor says so in a
message with what to install and where its log is); [Docs/Releases/](Docs/Releases/) has the notes of every release
([0.4.0](Docs/Releases/0.4.0.md) is the latest, [0.3.0](Docs/Releases/0.3.0.md) the first).

## Features

- **Engine core:** an entity-component system (EnTT) with reflection-driven components, hierarchy and
  prefabs, a job system, and UTF-8 file handling. Scenes hold up to 1,048,575 entities and a frame costs what changed
  (transforms, activity and lookups are cached); an always-on frame profiler times the stages of every frame,
  on the CPU and the GPU. Engine services are modules, and games can be built without physics, audio or animation.
- **Assets:** stable handles stored in `.meta` sidecars, cached imports that run in the background and are redone when
  sources change, hot reload, and asynchronous streaming within memory budgets. Textures are block-compressed on import
  (BC7, BC1, BC4, BC5) and stream mip by mip, from their smallest levels up to the resolution the screen needs. Shipped
  games read compressed, checksummed asset packs by byte range, optionally pruned to what their scenes use. Importers
  cover glTF 2.0 models (also meshopt-compressed, with skins and animations), textures (PNG, JPEG, TGA, BMP, PSD, GIF,
  Radiance HDR), audio (WAV, MP3, FLAC, Ogg Vorbis) and TrueType fonts (validated before use).
- **Scenes:** scenes load in the background, in the editor and in games. Additive sub-scenes stream in slices of
  entities per frame and unload in one step, prefabs compile into prototypes that spawn many instances in one call, and
  placed instances stay linked to their prefab (per-property overrides, apply, revert, unpack). Worlds larger than
  memory are split into cells and layers that stream around the camera or other sources, in games and in the editor.
  Saves run in the background, large projects open while their assets are scanned, and exports cook scenes into a
  binary form.
- **Rendering:** physically based shading with image-based lighting from HDR environment maps or a procedural sky,
  clustered point and spot lights, cascaded soft shadows (PCSS), ground-truth ambient occlusion, HDR with automatic
  exposure, bloom, tone mapping, color grading and FXAA, GPU skinning, signed-distance-field text, and editor overlays
  (grid, selection outlines, debug shapes). The scene is kept on the GPU, which culls it (frustum, size and two-phase
  occlusion, for the view and the shadow cascades) and draws it indirectly from one geometry pool; what the GPU sees
  decides which textures and mip levels stream in, rendering is camera-relative so far-away worlds keep their precision,
  and levels of detail are chosen by their projected error.
- **Physics:** Jolt rigid bodies with box, sphere, capsule and mesh colliders, triggers, collision events,
  raycasts and overlap queries, named collision layers, and character controllers that climb steps and slopes, ride
  moving ground and push bodies. When the fixed steps cannot keep up with real time, the game slows down instead of
  falling further behind.
- **Game UI:** screen-space and world-space canvases with anchored layouts, text, images, masks and widgets (buttons,
  toggles, sliders, input fields) driven by the mouse, keyboard and gamepads, built in the editor or from scripts.
- **Audio:** miniaudio playback with spatial sources, a listener that follows the camera or a listener
  component, and one-shot sounds.
- **Animation:** skeletons, clips and skinned meshes from glTF, and animation controllers (`.stanimator`) with states,
  parameters, cross-fades, events, root motion and joints that other entities attach to, evaluated on the job threads
  with the same results on any number of them.
- **Scripting:** game logic in C++ script modules built against a small SDK and a C ABI, so they never link the engine.
  Scripts create, change and destroy entities and components, spawn prefabs, load sub-scenes, read input (keyboard,
  mouse, gamepads, cursor modes), drive physics (forces, velocities, raycasts, collision and trigger callbacks,
  character controllers) and animators, play audio, build game UI, stream a world's cells, set the game's window mode,
  keep save files, and load scenes or quit the game. Script crashes (access violations, exceptions, and on Windows
  aborts) disable the script instead of taking down the editor or the game, and modules hot reload while the game plays,
  keeping their fields.
- **Editor:** a launcher (templates, samples, recent projects) and dockable panels (viewport with gizmos, hierarchy,
  inspector that edits many entities at once, asset tab, content browser with thumbnails, console, problems, activity,
  streaming and performance) in its own "Bedrock" look (Inter and JetBrains Mono type, Lucide icons, sharp at any
  display scale), undo/redo for every edit, autosave and crash recovery, deletes that go to the project's trash, play
  and simulate modes with a Game view beside the Scene view, worlds edited cell by cell, game scripts made, built and
  hot reloaded from the editor (opened in your code editor, with CMake presets for IDEs), and game export in the
  background. Every operation is an editor command with a JSON Schema, so scripts, tests and AI agents use the same API
  as the UI.
- **Automation:** the editor serves its commands over a local, token-authenticated JSON-RPC connection. `StrataCLI`
  drives it from the command line and is also an MCP server, so AI agents can build a game from scratch without anyone
  touching the editor. Agents play-test deterministically (fixed steps, scripted input sequences) and look through
  viewport captures, and the Activity panel lists every command with who ran it and exports a session as a command
  script that repeats it.
- **Runtime:** plays exported games (`<Game>` executable, `.stgame` manifest, `.stpak` asset pack and the
  game's script module), which can play a loading scene while the start scene loads. With `--headless` it runs the
  simulation without a window or GPU, at 60 frames per second.

## Building

```sh
git clone --recursive https://github.com/2042Third/GameEngine.git
cd GameEngine
cmake --preset <platform>                   # windows, windows-vs2022, linux or macos
cmake --build --preset <platform>-release
ctest --preset <platform>-release -LE perf  # -LE "gpu|perf" on machines without a GPU
```

The `perf` tests compare timings with budgets measured on one reference machine, so other machines leave them out.
Requirements: CMake 3.25+, a C++20 compiler (MSVC 19.40+, GCC 13+, Clang 17+ or Apple Clang 15+) and a
Vulkan 1.2+ driver; macOS runs Vulkan on Metal through MoltenVK (the Vulkan SDK, or Homebrew's `molten-vk` and
`vulkan-loader`). The Vulkan SDK is optional elsewhere (it provides the validation layers used by Debug builds). Linux
also needs the X11, Wayland and GTK 3 development packages. `Docs/DeveloperGuide.md` lists them ("Building").

Binaries land in `build/<preset>/bin/<Config>/`:

| Executable | Purpose |
| --- | --- |
| `StrataEditor` | The editor. Without a project it opens its launcher (recent projects, templates, samples, connecting an AI agent). `--project <dir>` opens a project, `--commands <script.json>` runs editor commands at startup, `--headless` runs without UI for automation. It is a windowed program: PowerShell does not wait for it unless its output is piped (`StrataEditor --commands script.json --quit-after-commands \| Out-Host`). |
| `StrataRuntime` | Plays an exported game; the export copies it next to the game files under the game's name. |
| `StrataCLI` | Command-line client and MCP server (`StrataCLI mcp`) for controlling a running editor. |
| `StrataTests` | Unit, integration, GPU and end-to-end tests (doctest). |

## Making a game

1. Start the editor and create a project from its launcher (**New Project**, or **File > New Project**): pick a
   template (Basic 3D starts lit, with a camera, a sun, a sky and a ground), a name and a location. The project gets a
   `Scripts/` folder with a CMake project for its C++ scripts. Drop models, textures and sounds onto the window to
   import them.
2. Build scenes in the viewport, hierarchy and inspector. Write scripts in `Scripts/` (**Scripts > New Script...**
   starts one), build them with **Scripts > Build Scripts** (Ctrl+B), attach them in the inspector, and try the scene
   with **Play**, which builds changed scripts first. Rebuilding while the game plays hot reloads the scripts.
3. Choose the start scene (**File > Set as Start Scene**) and export with **File > Export Game...** (or the
   `project.export` command). The output folder contains everything the game needs. Run it with
   `<Game> --headless --frames 600` (in PowerShell add `| Write-Output` to wait for it) to check that it plays without
   errors, or with `--windowed --frames 120 --screenshot shot.png` to see it.

Games can be play-tested without a person: while a game runs in the editor, the `input.key`, `input.mouseButton`,
`input.mouseMove` and `input.scroll` commands press keys and buttons the way a player would, and `viewport.capture`
returns the frame as a PNG. [`Samples/PhysicsPlayground`](Samples/PhysicsPlayground) (six physics stations, about 600
bodies) and [`Samples/Tetris`](Samples/Tetris) are complete games an AI agent made this way, entirely
through editor commands: open a copy of one from the launcher (**Open Sample**) and press Play, which builds its scripts
first.

Every step is also available as an editor command. For example, this command script creates a lit cube
and saves the scene:

```json
[
	{ "command": "entity.create", "parameters": { "name": "Sun", "components": { "DirectionalLight": { "Intensity": 3 } } } },
	{ "command": "entity.create", "parameters": { "name": "Box", "components": { "MeshRenderer": { "Mesh": "Builtin/Cube" } } } },
	{ "command": "scene.saveAs", "parameters": { "path": "Scenes/Main.stscene" } }
]
```

Run `editor.commands` (or `StrataCLI list`) for the full list of commands and their parameters.
`.claude/skills/strata-make-a-game/SKILL.md` is the end-to-end playbook an AI agent follows to make a game (project,
assets, scripts, scene, play-testing, export); `.claude/skills/strata-editor-automation/SKILL.md` and
`.claude/skills/strata-scripting/SKILL.md` cover the commands and the script SDK in depth.

## Repository layout

| Path | Contents |
| --- | --- |
| `Strata/` | The engine library: modules in `src/Strata/`, platform code in `src/Platform/`, shaders, vendored dependencies. |
| `StrataEditor/` | The editor: `src/Editor/` is the UI-independent core (commands, undo, automation, project, scene and script state), `src/Panels/`, `src/UI/` and `EditorLayer` the interface, `Resources/Fonts/` its embedded fonts. |
| `StrataRuntime/` | The game player. |
| `StrataCLI/` | The automation client and MCP server. |
| `StrataScriptCore/` | The script SDK and ABI that game code is written against. |
| `StrataTests/` | Tests, test script modules, and the feature test project, which uses every component and the whole script API. |
| `Samples/` | Example games, made through the editor by an AI agent: `OpenWorld` (a streamed island of 20,000 props), `PhysicsPlayground` (600 bodies to play with), `Tetris`. |
| `.claude/skills/` | Task guides for AI agents working on the engine or making games with it. |

## Contributing

`AGENTS.md` holds the rules for people and AI agents: code style (Hazel conventions), architecture rules, the
hard rules of the asset pipeline, scripting, the editor and testing, and the review checklist every change goes
through. `Docs/DeveloperGuide.md` has the details of every area: read its section before changing one.
`Docs/Architecture.md` explains how the engine fits together: targets, modules, the frame
loop, threading, the asset pipeline, scripting, the editor and export. Third-party components and their licenses
are listed in `ThirdPartyNotices.md`.
