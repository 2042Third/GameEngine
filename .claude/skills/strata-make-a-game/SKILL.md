---
name: strata-make-a-game
description: End-to-end playbook for an AI agent making a complete, playable game with Strata through the editor alone (StrataCLI or its MCP server) - start an editor, create the project, make materials and sounds, write and build the C++ scripts, build the scene, verify it by playing with simulated input and looking at viewport captures, leave a regression command script, export and check the exported game. Use when asked to make, prototype or finish a game with Strata. Samples/Tetris was made exactly this way.
---

# Making a game with Strata (as an agent)

You drive the editor through its commands and write only two kinds of files yourself: the game's C++ script sources (in
the project's `Scripts/` folder) and raw source assets you generate outside the project (e.g. a WAV), which `asset.import`
copies in. Everything else - project, materials, scene, entities, fields, start scene, export - is an editor command.
Never edit `.stscene`, `.stmat`, `.stproj` or `.meta` files by hand. Command details: `strata-editor-automation`;
the script SDK: `strata-scripting`. `Samples/Tetris` is a finished example of everything below.

## 1. An editor

```sh
export STRATA_SESSION_DIR=<a private, not yet existing directory>   # optional: keeps you away from other agents' editors
StrataCLI launch --headless          # rendering without a window: viewport.capture works (needs a GPU)
StrataCLI launch --no-gpu            # no GPU at all: everything but captures
StrataCLI list                       # the commands this build has; check that input.* is there
```

- Binaries are in `build/<preset>/bin/<Config>/`. Use one configuration throughout: `script.build` builds the game's
  module with the editor's configuration, and the export ships that module.
- Pass parameters as files or stdin (`StrataCLI call entity.create @p.json`, `echo '{...}' | StrataCLI call x -`); shell
  quoting of inline JSON breaks on Windows. A tiny wrapper script (`cli.sh call "$@"` with the session directory set) and
  one for taps (`key.sh Left` -> `input.key {"key": "Left"}`) save a lot of typing.
- Rebuilding the engine while your editor runs fails (the executable is locked): `scene.save`, `editor.quit`, rebuild,
  `StrataCLI launch --headless --project <dir>` (it reopens the start scene and loads the built module).

## 2. Project and plan

`project.create {"directory": "<absolute>", "name": "Tetris"}` writes `Tetris.stproj`, `Assets/`, a `.gitignore` for the
`.strata/` cache, and `Scripts/` with its `CMakeLists.txt` and an example `Spinner.cpp` (delete it if unused).

Design the code for verification before writing it:

- Rules in a plain C++ class without engine calls (`Scripts/TetrisGame.h/.cpp`), a `Script` class for input, time and
  drawing (`TetrisBoard.cpp`). The rules stay simple and deterministic.
- Randomness from a seed field (`Random random(seed)`, a 7-bag shuffle): the same seed gives the same game, so moves can
  be planned and tests can expect exact outcomes. Each restart can use the next seed.
- Tuning as fields (`StartInterval`, `LinesPerLevel`, `RepeatDelay`...): tests and you can change them with
  `script.setField`, even while playing.
- `Log::Info` one line per meaningful event ("cleared 1 line; score 209"), and show state in `Text` entities or keep it in
  fields: that is what you can observe from outside (`log.read`, `component.get`, `script.get` for live field values).

## 3. Assets

- Materials: `material.create {"path": "Materials/Red.stmat", "properties": {...}}`. Properties: `BaseColor` [r,g,b,a],
  `Metallic`, `Roughness`, `EmissiveColor` [r,g,b], `EmissiveIntensity`, `AlphaMode` (`Opaque`, `Mask`, `Blend`; with an
  alpha below 1 for see-through), `AlphaCutoff`, `DoubleSided`, `Unlit`, texture maps by asset (`BaseColorMap`, ...),
  `UVTiling`, `UVOffset`. `material.get {"material": "Builtin/DefaultMaterial"}` describes them all.
- Sounds: write a small 16-bit PCM WAV with a script of your own (outside the project), then
  `asset.import {"file": "<absolute>", "directory": "Sounds"}`. Models, textures and fonts import the same way.
- Built-in meshes: `Builtin/Cube`, `Sphere`, `Plane`, `Quad`, `Cylinder`, `Capsule`, `Cone`, `Torus` (also reachable
  from scripts: `Assets::Find("Builtin/Cube")`).

## 4. Scripts

Write the sources, then `script.build` (configures CMake the first time; answers with compiler diagnostics on failure)
and `script.status` (classes, fields, defaults). Patterns that worked:

- Many visuals: create the entities once in `OnCreate` (`Scene::CreateEntity(name, GetEntity())`,
  `AddComponent("MeshRenderer")`, `SetProperty("MeshRenderer", "Mesh", mesh)`, `GetTransform().SetTranslation(...)`), then
  show and hide them with `SetActive` and recolor them with `SetProperty("MeshRenderer", "Material", handle)`. Remember
  what each one shows and touch only the ones that change.
- HUD: `Entity` fields that point at `Text` entities; `entity.SetProperty("Text", "Text", std::string(...))` when the
  value changes.
- Input: `Input::IsKeyPressed` for one-shot actions, `KeyRepeat` for held movement (construct it from fields in
  `OnCreate`), and update both keys of a pair every frame (`a.Update(dt) || b.Update(dt)` would skip `b`).
- Sounds: `Assets::RequestLoad` in `OnCreate`, `Audio::PlayOneShot(clip)` later.
- Hot reload replaces the instance: non-field members are gone in `OnReload`; find your entities again (e.g. children of
  the script's entity by name) instead of creating more.
- Script builds use the compiler's default warnings; compile your sources with `-Wall -Wextra` once (GCC or Clang with
  `-I StrataScriptCore/Include -isystem Strata/vendor/glm`) to catch what other platforms' compilers would report.

## 5. Scene

Build it with `entity.create` (with `components`), `component.set`, `script.add`; then `scene.saveAs {"path":
"Scenes/Main.stscene"}` and `project.setStartScene`.

- Camera: `{"Camera": {"Primary": true, "PerspectiveFOV": 45}}`; cameras and lights look along their -Z, so a camera at
  `+Z` looking at the origin needs no rotation. Rotations are Euler degrees `[pitch, yaw, roll]`.
- Light: a `DirectionalLight` (Intensity ~3, pitch -35) plus a `SkyLight` with an `AmbientColor` for fill.
- Dark scenes: add a `PostProcess` with `AutoExposure: false` (automatic exposure brightens a dark backdrop to gray).
- Text: `ScreenSpace: true`, `ScreenAnchor` (0..1, top-left origin), `ScreenOffset` (pixels, +Y down), `FontSize` in
  pixels, and `Alignment` - it defaults to `Center`, so a left-hand HUD needs `"Alignment": "Left"`. World text
  (`ScreenSpace: false`) uses `FontSize` world units.
- `script.add {"entity", "class", "fields": {...}}`: asset fields take paths (`"Materials/Red.stmat"`, `"Builtin/Cube"`),
  entity fields take IDs from `entity.create`. Fields left out keep the class defaults.

## 6. Verify: play it and look at it

```text
play.start
input.key {"key": "Left"}                 # tap: answers after the game saw the release; issue taps one after another
input.key {"key": "Down", "frames": 20}   # held for 20 frames (KeyRepeat repeats)
component.get {"entity": "<HUD id>", "component": "Text"}      # -> {"values": {"Text": "Score: 209", ...}}
log.read {"minLevel": "Info"}             # the script's Log::Info lines
viewport.capture {"camera": "scene", "width": 1280, "height": 720}   # CLI: --save-image shot.png, then look at it
play.stop
```

- Look at every capture you take. `pendingAssets` above 0: wait (`editor.wait`) and capture again.
- Gravity and other timers keep running between your calls (a call takes about 0.1 s): plan sequences and send them in
  one go, set slow timings with `script.setField` while exploring, or `play.pause` the game and advance it with `play.step`:
  input given while paused arrives in the next stepped frame.
- Check every feature you built: movement, rotation, repeats, scoring, level changes, game over, restart, pause.

## 7. Leave a regression test

A command script runs the same steps without you and fails the process (exit code 1) when an `expect` does not hold:

```sh
StrataEditor --no-gpu --no-automation --quit-after-commands --project <dir> --commands play.json
```

```json
[
	{ "command": "script.build", "expect": { "/success": { "equals": true } } },
	{ "command": "script.setField", "parameters": { "entity": "<board id>", "class": "TetrisBoard", "field": "StartInterval", "value": 100 } },
	{ "command": "play.start" },
	{ "command": "input.key", "parameters": { "key": "Space" } },
	{ "command": "component.get", "parameters": { "entity": "<score id>", "component": "Text" }, "expect": { "/values/Text": { "equals": "Score: 36" } } }
]
```

Entity IDs are stable once the scene is saved. Make timing-dependent outcomes deterministic with fields (no gravity
while checking an exact score), then turn them back to let time play its part (fast gravity to reach game over).
`StrataTests/Editor/TetrisCommands.json.in` and `TetrisSample.cmake` are the full example (CTest `StrataEditor.Tetris`).

## 8. Export and check the game

```text
project.export {"directory": "<absolute, outside the project>"}   # <Game>[.exe], .stgame, .stpak, the script module
<dir>/<Game> --headless --frames 600       # 10 s of game time at 60 frames per second; exit code 0, the scripts' log lines
<dir>/<Game> --windowed --frames 120 --screenshot shot.png        # look at shot.png
```

The exported game has no `--help`: unknown arguments are ignored and the game starts (fullscreen if the manifest says so).

## Pitfalls

| Symptom | Cause and fix |
| --- | --- |
| `input.key` fails "start it with play.start" | Input only reaches play mode (not edit or `play.simulate`). |
| A tap does nothing | The game is paused: the tap waits for the next `play.step` (or resuming). Or the game reacts to keys held over time: hold longer with `frames` or press/release. |
| HUD text cut off at the left edge | `Text` alignment defaults to `Center`; set `Alignment: Left`. |
| Everything looks washed out or gray | Automatic exposure; add `PostProcess` with `AutoExposure: false`. |
| The score differs by a few points between runs | Gravity ticked between commands; make the check deterministic (slow gravity via a field). |
| `editor.quit` refuses | Unsaved changes: `scene.save` (or `edit.undo` the test's edits), or `{"force": true}`. |
| Script edits did nothing | Not built: `script.build` (it hot reloads while playing and restarts instances via `OnReload`). |
| The exported game logs no gameplay | The headless run was too short: frames are paced at 60 per second. |
