# Strata

[![CI](https://github.com/2042Third/GameEngine/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/2042Third/GameEngine/actions/workflows/ci.yml)

Strata is a 3D game engine written in C++20 for Windows, Linux (Ubuntu 24+) and macOS. It renders through
[NVRHI](https://github.com/NVIDIA-RTX/NVRHI) on Vulkan and comes with an editor that people and AI agents
can drive in the same way, plus a runtime that plays exported games.

## Features

- **Engine core:** an entity-component system (EnTT) with reflection-driven components, hierarchy and
  prefabs, a job system, and UTF-8 file handling.
- **Assets:** stable handles stored in `.meta` sidecars, cached imports that are redone when sources change,
  hot reload, asynchronous streaming with GPU upload budgets, and asset packs for shipped games.
  Importers cover glTF 2.0 models, textures (PNG, JPEG, TGA, BMP, PSD, GIF, Radiance HDR), audio
  (WAV, MP3, FLAC, Ogg Vorbis) and TrueType fonts (validated before use).
- **Rendering:** physically based shading with image-based lighting from HDR environment maps,
  clustered point and spot lights, cascaded soft shadows (PCSS), ground-truth ambient occlusion, HDR with
  automatic exposure, bloom, tone mapping and FXAA, signed-distance-field text, and editor overlays (grid,
  selection outlines, debug shapes).
- **Physics:** Jolt rigid bodies with box, sphere, capsule and mesh colliders, triggers, collision events,
  raycasts and overlap queries.
- **Audio:** miniaudio playback with spatial sources, a listener that follows the camera or a listener
  component, and one-shot sounds.
- **Scripting:** game logic in C++ script modules built against a small SDK and a C ABI, so they never link
  the engine. Scripts create, change and destroy entities and components, spawn prefabs, read input, drive
  physics (forces, velocities, raycasts, collision and trigger callbacks), play audio, and load scenes or
  quit the game. Script crashes (access violations, exceptions, and on Windows aborts) disable the script
  instead of taking down the editor or the game, and modules hot reload while the game plays, keeping their
  fields.
- **Editor:** dockable panels (viewport with gizmos, hierarchy, inspector, content browser, console) in its own
  "Bedrock" look (Inter and JetBrains Mono type, Lucide icons, sharp at any display scale),
  undo/redo for every edit, play and simulate modes, building and hot reloading game scripts, and game
  export. Every operation is an editor command with a JSON Schema, so scripts, tests and AI agents use the
  same API as the UI.
- **Automation:** the editor serves its commands over a local, token-authenticated JSON-RPC connection.
  `StrataCLI` drives it from the command line and is also an MCP server, so AI agents can build a game
  from scratch without anyone touching the editor.
- **Runtime:** plays exported games (`<Game>` executable, `.stgame` manifest, `.stpak` asset pack and the
  game's script module). With `--headless` it runs the simulation without a window or GPU, at 60 frames per second.

## Building

```sh
git clone --recursive https://github.com/2042Third/GameEngine.git
cd GameEngine
cmake --preset windows          # or windows-vs2022, linux, macos
cmake --build --preset windows-release
ctest --preset windows-release  # add -LE gpu on machines without a GPU
```

Requirements: CMake 3.25+, a C++20 compiler (MSVC 19.40+, GCC 13+, Clang 17+ or Apple Clang 15+) and a
Vulkan 1.2+ driver. The Vulkan SDK is optional (it provides the validation layers used by Debug
builds). Linux also needs the X11, Wayland and GTK 3 development packages. `AGENTS.md` lists them.

Binaries land in `build/<preset>/bin/<Config>/`:

| Executable | Purpose |
| --- | --- |
| `StrataEditor` | The editor. Without a project it opens its launcher (recent projects, templates, samples, connecting an AI agent). `--project <dir>` opens a project, `--commands <script.json>` runs editor commands at startup, `--headless` runs without UI for automation. |
| `StrataRuntime` | Plays an exported game; the export copies it next to the game files under the game's name. |
| `StrataCLI` | Command-line client and MCP server (`StrataCLI mcp`) for controlling a running editor. |
| `StrataTests` | Unit, integration, GPU and end-to-end tests (doctest). |

## Making a game

1. Start the editor and create a project from its launcher (**New Project**, or **File > New Project**): pick a
   template (Basic 3D starts lit, with a camera, a sun, a sky and a ground), a name and a location. The project gets a
   `Scripts/` folder with a CMake project for its C++ scripts. Drop models, textures and sounds onto the window to
   import them.
2. Build scenes in the viewport, hierarchy and inspector. Write scripts in `Scripts/`, build them with
   **Scripts > Build Scripts** (Ctrl+B), attach them in the inspector, and try the scene with **Play**. Rebuilding
   while the game plays hot reloads the scripts.
3. Choose the start scene (**File > Set as Start Scene**) and export with the `project.export` command.
   The output folder contains everything the game needs. Run it with `<Game> --headless --frames 600` to check that it
   plays without errors, or with `--windowed --frames 120 --screenshot shot.png` to see it.

Games can be play-tested without a person: while a game runs in the editor, the `input.key`, `input.mouseButton`,
`input.mouseMove` and `input.scroll` commands press keys and buttons the way a player would, and `viewport.capture`
returns the frame as a PNG. [`Samples/Tetris`](Samples/Tetris) is a complete game an AI agent made this way, entirely
through editor commands: open a copy of it from the launcher (**Open Sample**), build its scripts (Ctrl+B) and press
Play (arrow keys, Space, Z, P, R).

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
| `Samples/` | Example games, made through the editor by an AI agent (`Tetris`). |
| `.claude/skills/` | Task guides for AI agents working on the engine or making games with it. |

## Contributing

`AGENTS.md` is the development guide for people and AI agents: code style (Hazel conventions),
architecture rules, the asset pipeline, scripting, editor commands, testing, and the review checklist every
change goes through. `Docs/Architecture.md` explains how the engine fits together: targets, modules, the frame
loop, threading, the asset pipeline, scripting, the editor and export. Third-party components and their licenses
are listed in `ThirdPartyNotices.md`.
