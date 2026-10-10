---
name: strata-editor-automation
description: Drive the Strata editor from outside, as an AI agent, through StrataCLI or its MCP server - start or find an editor (headless), discover its commands, create projects, import assets, build scenes from entities, components and prefabs, play and inspect (with simulated keyboard and mouse input), undo, save, capture the viewport and export a playable game. Use whenever you need the editor to do something (build or change a game, check a scene, export) rather than change the engine's code.
---

# Driving the Strata editor (StrataCLI and MCP)

Every editor feature is a **command** (`entity.create`, `scene.saveAs`, `project.export`, ...). The editor serves
all of them over a local, token-authenticated JSON-RPC connection (`EditorAutomation`), and two clients reach it:

- **StrataCLI** (`build/<preset>/bin/<Config>/StrataCLI[.exe]`): one command per call, JSON result on stdout.
- **MCP** (`StrataCLI mcp`): every command becomes a tool, `.` replaced by `_` (`entity.create` -> `entity_create`).

Both find the editor through its **session file** (`<user data>/Sessions/<pid>.json`, written by the editor,
holding the port and the secret token; `<user data>` is `%LOCALAPPDATA%\Strata` on Windows,
`~/.local/share/Strata` on Linux and `~/Library/Application Support/Strata` on macOS). You never handle ports or
tokens yourself.

## 1. Get an editor

```sh
StrataCLI status                      # is one running? lists known editor sessions (never the token)
StrataCLI launch --no-gpu             # start one without window and GPU, no project; waits until it answers
StrataCLI launch --headless --project <dir>   # existing project; no window, but rendering works (captures)
```

- `--no-gpu`: no graphics device at all (any machine, CI). `--headless`: no window but rendering works.
  Without either, the editor opens its window (a human can watch).
- Started by hand: `StrataEditor --no-gpu [--project <dir>]` (keep it running in the background). It serves
  automation by default; `--automation-port <n>` fixes the port (default: a free one), `--no-automation`
  turns it off. A headless editor runs until `editor.quit` (or a signal), or with `--idle-timeout <seconds>` until
  no client has been connected for that long; it paces itself at 60 frames/s. `launch` passes
  `--idle-timeout <s>` when given; editors started through MCP close after 10 minutes without a client.
- Several editors: pick one with `--project <dir>` on every call; otherwise the newest is used.
- `STRATA_SESSION_DIR` moves the session directory; editor and clients must agree on it (tests use it).

## 2. Call commands

```sh
StrataCLI list                              # every command with its one-line description
StrataCLI list --json                       # ... with the JSON Schema of each command's parameters
StrataCLI call editor.status                # start here: project, scene, play state, selection, undo, automation
StrataCLI call entity.create '{"name": "Player"}'
StrataCLI call entity.create @params.json   # params from a file (no shell quoting problems)
echo '{"frames": 60}' | StrataCLI call editor.wait -     # params from stdin
StrataCLI call viewport.capture --save-image shot.png    # image results are written to a file (needs rendering)
```

- On Windows, quoting JSON on the command line is fragile (PowerShell strips inner quotes): prefer `@file` or `-`.
- Exit codes: `0` success (result on stdout); `1` the editor answered with an error (`{"code", "message",
  "data"}` on stderr); `2` no editor reachable or the connection was lost; `3` invalid command line; `4` no
  answer within `--timeout` (the editor is still there and the command may still finish: wait or call
  `editor.status`, do not start another editor); `5` `--save-image` could not write the image.
- `--timeout <ms>` (default 30000) bounds the wait for one call. Commands that take frames (`editor.wait`,
  captures, builds) answer only when they finish.

### MCP

Register the server with your MCP client, e.g. `{"command": "<bin>/StrataCLI", "args": ["mcp"]}` (Claude Code:
`claude mcp add strata -- <bin>/StrataCLI mcp`). Tools:

- `strata_status`, `strata_launch_editor` (`project`, `headless`, `noGpu`; without `project` it starts an empty
  editor, or reconnects to the empty one it started before), `strata_list_methods`, `strata_call` (`method`,
  `params`) are always there.
- While an editor is connected, each command is a tool with the command's schema as `inputSchema`. The tool list
  changes (`notifications/tools/list_changed`) when an editor connects or goes away.
- Results come as text plus `structuredContent`; `{"Image": {"MimeType", "Data"}}` results come as image content.
- Once connected, the server stays with that editor (also when it opens another project, or restarts with the same
  project) and never silently switches to another one.

## 3. Discover before you act

| Question | Call |
| --- | --- |
| What is open, is it playing, unsaved changes? | `editor.status` |
| Which commands and parameters exist? | `editor.commands` (or `list --json`) |
| Which components and properties (types, ranges, enum values)? | `component.list` |
| Which assets exist? | `asset.list` (`type`, `path` filters), `asset.info` |
| What does a material hold, which properties exist? | `material.get` (values, and each property's type, range, options) |
| What state do the scripts hold (a score, a timer)? | `script.get` (live field values while playing) |
| What is in the scene? | `scene.hierarchy`, `entity.find` (`name`, `tag`, `component`), `entity.get` |
| What happened (errors, script output)? | `log.read` (`after` = the previous `latest` to page; `minLevel`) |

Commands added at run time (for example `script.*`, `viewport.*`, `camera.*` when those features are present)
appear in `editor.commands` and as tools automatically. The viewport commands need rendering: they fail in an
editor started with `--no-gpu` (use `--headless`):

- `viewport.capture {width?, height?, camera?: "editor" | "scene", overlays?, path?}` returns
  `{"Image": {"MimeType": "image/png", "Data"}, width, height, camera, overlays, pendingAssets, pendingTextGlyphs, notice?,
  path?}`. Text is complete in the picture (the capture waits a few frames for new glyphs).
- `camera.get`; `camera.set {position?, target?, yaw?, pitch?, distance?, fov?, near?, far?, flySpeed?}`;
  `camera.focus {entities?}` frames the given (or selected) entities.
- Game logic: write C++ scripts into the project's `Scripts/` folder and use `script.build`, `script.status`,
  `script.add`, `script.setField`, `script.reload` (see `.claude/skills/strata-scripting/SKILL.md`, "The workflow in a
  game project"). Builds need CMake and the engine's compiler on the machine; they work with `--no-gpu`.

## 4. Conventions

- **IDs** are strings of 16 hexadecimal digits (`"00000000000000AB"`), for entities and assets alike.
- **Assets** are referenced by handle, by path relative to the project's `Assets/` directory
  (`"Materials/Red.stmat"`), or as built-ins: `Builtin/Cube`, `Sphere`, `Plane`, `Quad`, `Cylinder`, `Capsule`,
  `Cone`, `Torus`, `Builtin/DefaultMaterial`.
- **Component values** use the scene file format: `{"Transform": {"Translation": [0, 1, 0], "Rotation": [0, 90, 0],
  "Scale": [1, 1, 1]}}`. Rotations accept Euler degrees `[pitch, yaw, roll]` or a quaternion `[x, y, z, w]` (read
  back as a quaternion). Property names are case-insensitive. `component.get` shows the accepted format of every value.
- **Undo**: every successful mutating command is one undo step (`edit.undo`, `edit.redo`, `edit.history`).
- **Play mode**: `play.start` runs a copy of the scene. Changes made while playing apply to that copy, carry a
  `warning` in the result, and are discarded by `play.stop`. Undo and redo are not available while playing;
  `scene.save` saves the edited scene, never the running copy.
- **Errors** (JSON-RPC codes): `-32602` invalid parameters (the error's `data.parameters` is the command's schema:
  fix the request and retry), `-32005` the request was valid but cannot be done now (message says why, e.g.
  "Nothing to undo", unsaved changes), `-32601` unknown command, `-32006` cancelled (editor closing), `-32603`
  internal error. Unknown or missing parameters are always errors, never ignored.

## 5. Workflows

**New game from scratch**

```text
project.create      {"directory": "<absolute dir>", "name": "Tetris"}
material.create     {"path": "Materials/Red.stmat", "properties": {"BaseColor": [1, 0, 0, 1]}}
entity.create       {"name": "Camera", "components": {"Camera": {}, "Transform": {"Translation": [0, 5, 10], "Rotation": [-26, 0, 0]}}}
entity.create       {"name": "Sun", "components": {"DirectionalLight": {"Intensity": 3}}}
entity.create       {"name": "Block", "components": {"MeshRenderer": {"Mesh": "Builtin/Cube", "Material": "Materials/Red.stmat"}}}
scene.saveAs        {"path": "Scenes/Main.stscene"}
project.setStartScene {"scene": "Scenes/Main.stscene"}
```

**Assets**: `asset.import {"file": "<absolute path>", "directory": "Models"}` copies a file into the project and
imports it: models `.gltf`/`.glb`, textures `.png`/`.jpg`/`.tga`/`.bmp`/`.psd`/`.gif`/`.hdr`, audio
`.wav`/`.mp3`/`.flac`/`.ogg`, fonts `.ttf`/`.otf`. `asset.info` reports import errors and warnings;
`asset.setImportSettings` changes them. Models expose sub-assets (meshes, materials) and are instantiated whole
with `prefab.instantiate`.

**Hierarchy and prefabs**: `entity.create {"parent": "<id>"}`, `entity.setParent`, `entity.duplicate`,
`prefab.create {"entities": ["<id>"], "path": "Prefabs/Block.stprefab"}`, then
`prefab.instantiate {"prefab": "Prefabs/Block.stprefab", "components": {"Transform": {"Translation": [2, 0, 0]}}}`.

**Edit**: `component.add` (adds if missing, then sets values), `component.set` (component must exist),
`component.remove`, `entity.rename`, `entity.setActive`, `entity.delete`, `scene.setSettings` (gravity, timestep).

**Play and inspect**: `play.start` -> `editor.wait {"frames": 120}` (about two seconds) -> `entity.get` /
`component.get` / `log.read` -> `play.stop`. `play.pause {"paused": true}` and `play.step {"frames": n}` advance
a paused scene by fixed steps. `play.simulate` runs physics only.

**Play it (simulated input)**: while the game runs (`play.start`), the `input.*` commands press keys and buttons the way a
player would; scripts see them through `Input::IsKeyDown/IsKeyPressed/IsKeyReleased` (also while the viewport has no focus).

```text
input.key         {"key": "Left"}                         # tap: down for 1 frame, answers after the game saw the release
input.key         {"key": "Space", "frames": 30}          # held for half a second
input.key         {"key": "Right", "action": "press"}     # held across commands ... until
input.key         {"key": "Right", "action": "release"}
input.mouseMove   {"position": [640, 360]}                # pixels from the game view's top-left corner
input.mouseButton {"button": "Left"}                      # Left, Right, Middle, Button3..7; same actions as keys
input.scroll      {"delta": [0, 1]}
input.state       {}                                      # what simulated input holds; input.releaseAll lets go of it
```

Keys are named like the SDK's `Key::` constants (`Left`, `Up`, `Space`, `Enter`, `Escape`, `A`, `D1`, `F5`, `LeftShift`;
case does not matter). Each command answers once the game has seen its input (`"seen": true`), so issue them one after another
and read the effect (`component.get`, `log.read`, `viewport.capture`). Notes:

- Input only reaches play mode (not edit or simulate); `play.stop` drops held keys.
- Frames are game frames. While the game is paused (`play.pause`), input waits for the next frame `play.step` runs (or for
  `play.pause {"paused": false}`) and arrives with its press: `input.key {"key": "Space"}` then `play.step {"frames": 1}` makes the
  game see Space pressed in that step, and released in the following one. Commands answer at once while paused
  (`"seen": false`), because waiting would block you from stepping; pass `"wait": true` to wait anyway.
- Each command ends only its own hold: overlapping taps of a key keep it down until the last ends; `"action": "release"` ends
  every hold of the key and `input.releaseAll` every hold of every button. Holds outlive your connection; the editor's status bar
  lists them in a pill that releases them when a person clicks it.
- A tap of 1 frame is one key press for games that act on `IsKeyPressed` or `KeyRepeat`; use `frames` (or press/release) to
  hold a key for repeats.

**Look at it** (needs rendering, not `--no-gpu`): `camera.focus` or `camera.set`, then
`StrataCLI call viewport.capture '{"camera": "scene"}' --save-image shot.png` and open `shot.png`; with MCP the
capture arrives as an image. `pendingAssets` above zero means assets were still loading: wait a few frames and
capture again.

**Export**: `project.export {"directory": "<absolute dir outside the project>"}` writes `<Game>.stpak`,
`<Game>.stgame` and the runtime renamed `<Game>[.exe]` (`"includeRuntime": false` skips it). Check the result
headless: `<dir>/<Game> --headless --frames 120` (exit code 0).

**Finish**: save (`scene.save`), then `editor.quit`. It refuses while the scene has unsaved changes;
`{"force": true}` discards them. The editor answers, closes and removes its session file.

## 6. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `No running Strata editor was found` (exit 2) | Start one (`StrataCLI launch --no-gpu`). The editor and the client must use the same `STRATA_SESSION_DIR` (and user). |
| Several editors, the wrong one answers | Pass `--project <dir>` to every call (MCP: `strata_launch_editor` with `project`). |
| `-32602 Unknown parameter 'x'` | Misspelled or unsupported parameter; read `data.parameters` or `editor.commands`. |
| A call times out (exit 4) | The command takes many frames (or the editor is busy): raise `--timeout`. The command still finishes in the editor; do not launch another one. |
| Changes vanished after `play.stop` | They were made while playing; make them in edit mode. |
| `editor.quit` fails with unsaved changes | `scene.save` / `scene.saveAs` first, or `{"force": true}`. |
| Headless editor exits at once with code 1 | Its automation could not start (session directory not private, port in use); read its output or `<user data>/Logs/StrataEditor.log`. |
| The editor went away on its own | It was started with `--idle-timeout` (MCP: 10 minutes) and no client stayed connected; start it again. |
| `viewport.*` / `camera.*` fail or are missing | The editor runs with `--no-gpu` (restart it with `--headless`), or this build has no viewport commands (check `editor.commands`). |
| Script or import errors | `log.read {"minLevel": "Warn"}`, `asset.info`. |

Session files of editors that exited are removed by the clients automatically; never edit or copy them (they
hold the token).
