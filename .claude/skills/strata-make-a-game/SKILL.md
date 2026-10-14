---
name: strata-make-a-game
description: End-to-end playbook for an AI agent making a complete, playable game with Strata through the editor alone (StrataCLI or its MCP server) - start an editor, create the project, make materials and sounds, write and build the C++ scripts, build the scene, verify it by playing with simulated input and looking at viewport captures, leave a regression command script, export and check the exported game. Use when asked to make, prototype or finish a game with Strata. Samples/Tetris was made exactly this way.
---

# Making a game with Strata (as an agent)

You drive the editor through its commands and write only two kinds of files yourself: the game's C++ script sources (in
the project's `Scripts/` folder) and raw source assets you generate outside the project (e.g. a WAV), which `asset.import`
copies in. Everything else - project, materials, scene, entities, fields, start scene, export - is an editor command.
Never edit `.stscene`, `.stmat`, `.stproj` or `.meta` files by hand. Command details: `strata-editor-automation`;
the script SDK: `strata-scripting`. `Samples/Tetris` is a finished example of everything below; to study it in a running
editor without touching the sample, open a copy: `project.openSample {"sample": "Tetris", "directory": "<new absolute dir>"}`.
Strata's executables (StrataEditor, StrataRuntime, StrataCLI) and `Samples/` are in the Strata folder of a downloaded
release; in a source checkout the executables are in `build/<preset>/bin/<Config>/` and `Samples/` is at its root.

## 1. An editor

```sh
export STRATA_SESSION_DIR=<a private, not yet existing directory>   # optional: keeps you away from other agents' editors
StrataCLI launch --headless          # rendering without a window: viewport.capture works (needs a GPU)
StrataCLI launch --no-gpu            # no GPU at all: everything but captures
StrataCLI list                       # the commands this build has; check that input.* is there
```

- `script.build` builds the game's module with the editor's configuration, and the export ships that module: in a source
  checkout, use the executables of one configuration (`build/<preset>/bin/<Config>/`) throughout.
- Pass parameters as files or stdin (`StrataCLI call entity.create @p.json`, `echo '{...}' | StrataCLI call x -`); shell
  quoting of inline JSON breaks on Windows. A tiny wrapper script (`cli.sh call "$@"` with the session directory set) and
  one for taps (`key.sh Left` -> `input.key {"key": "Left"}`) save a lot of typing.
- In a source checkout, rebuilding the engine while your editor runs fails (the executable is locked): `scene.save`,
  `editor.quit`, rebuild, `StrataCLI launch --headless --project <dir>` (it reopens the start scene and loads the built
  module).

## 2. Project and plan

`project.create {"directory": "<absolute>", "name": "Tetris", "template": "basic3d"}` writes `Tetris.stproj`, `Assets/`, a
`.gitignore` for the `.strata/` cache, and `Scripts/` with its `CMakeLists.txt` and an example `Spinner.cpp` (delete it if
unused). With the `basic3d` template (`project.templates` lists them) it also saves a lit start scene,
`Scenes/Main.stscene`, already set as the start scene: "Main Camera" (primary, audio listener), "Sun" (directional light,
shadows), "Sky" (procedural sky light), "Ground" (a 20 x 20 plane with the dark `Materials/Ground.stmat`, which you can
restyle with `material.set`, and a static box collider with its top at y = 0: dynamic bodies and character controllers rest
on it) and "Post Process". Keep what fits your game and change or delete the rest
(`entity.find {"name": "Ground"}` gives the IDs). Without a template the project has no scene.

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
  `asset.import {"file": "<absolute>", "directory": "Sounds"}` (it answers once the import is done, and fails, removing
  the copy, when the file cannot be imported). Models, textures and fonts import the same way; a `.gltf` brings the
  `.bin` and images it names, and `"directories": [...]` imports whole folders with their tree. Delete assets with
  `asset.delete` (refused while something uses them: `asset.findReferences` lists the users; deleted files go to the
  project's trash and `edit.undo` brings them back).
- Built-in meshes: `Builtin/Cube`, `Sphere`, `Plane`, `Quad`, `Cylinder`, `Capsule`, `Cone`, `Torus` (also reachable
  from scripts: `Assets::Find("Builtin/Cube")`).
- Animated characters: import a skinned `.glb`/`.gltf` (`asset.import`), then `animation.info {"asset": "<model>"}` for
  its clips (handles, names, durations). `prefab.instantiate` the model: the root is the animator, already playing the
  first clip on start. For more than one animation make a controller (`animation.createController`: states per clip,
  parameters, transitions; the editor automation skill has an example), set it as the root's `Animator.Controller`, and
  drive it from a script on the root (`GetEntity().GetAnimator().SetFloat("Speed", speed)`, `SetTrigger("Jump")`, events
  in `OnAnimationEvent`). Walk cycles that move the root use `ApplyRootMotion` so the feet do not slide (with a
  `CharacterController` on the root, the controller walks the root motion and collides as it walks); otherwise move
  the entity from the script and keep the clips in place. Check poses in edit mode with `animation.preview` and a
  `viewport.capture`; while playing, `animation.info {"entity": "<root>"}` reports the state and parameters.

## 4. Scripts

Write the sources (`script.create {"class": "Player"}` starts a class), then `script.build` (configures CMake on the
first run and after files were added or removed; answers with compiler diagnostics on failure) and `script.status`
(classes, fields, defaults); `play.start` also builds changed sources first. Patterns that worked:

- Many visuals: create the entities once in `OnCreate` (`Scene::CreateEntity(name, GetEntity())`,
  `AddComponent("MeshRenderer")`, `SetProperty("MeshRenderer", "Mesh", mesh)`, `GetTransform().SetTranslation(...)`), then
  show and hide them with `SetActive` and recolor them with `SetProperty("MeshRenderer", "Material", handle)`. Remember
  what each one shows and touch only the ones that change.
- HUD: a UI canvas (below) with `UIText` elements; `Entity` fields that point at them, and
  `entity.GetUI().SetText(...)` when the value changes. A panel that shows only sometimes (a pause or game-over message):
  `panel.SetActive(...)`. Menus: buttons whose `EventTarget` is the entity of a menu script, which gets
  `OnUIClick(widget)`; keyboard and gamepad navigation work by themselves, and `UI::IsActionPressed(UIAction::Cancel)`
  closes a menu (the scripting skill's "Game UI"). Build them with `ui.create` (the editor automation skill's "Build the
  game UI") and test them with `input.click`.
- A main menu: a scene without a camera whose canvas holds the title and the buttons (the game shows its UI over black);
  the Play button's script calls `Game::LoadScene(level)`. A pause screen: a panel set active with `Time::SetTimeScale(0)`;
  the UI keeps working (it runs on unscaled time) and `Time::GetUnscaledDeltaTime()` animates what the script fades.
- Input: `Input::IsKeyPressed` for one-shot actions, `KeyRepeat` for held movement (construct it from fields in
  `OnCreate`), and update both keys of a pair every frame (`a.Update(dt) || b.Update(dt)` would skip `b`).
- Sounds: `Assets::RequestLoad` in `OnCreate`, `Audio::PlayOneShot(clip)` later.
- A walking player (first person, mouse look, keyboard and gamepad): an entity "Player" with a `CharacterController`
  (no `RigidBody`) and this script, and the primary camera as its child at eye height (`Translation [0, 1.6, 0]`); set the
  script's `Camera` field to the camera's ID. The controller moves the entity; the script turns the entity (yaw) and the
  camera (pitch):

  ```cpp
  class PlayerController : public Script
  {
  public:
  	float Speed = 4.0f;
  	float JumpSpeed = 5.0f;
  	float MouseSensitivity = 0.1f;  // Degrees per pixel
  	float StickTurnSpeed = 120.0f;  // Degrees per second at full tilt
  	Entity Camera;                  // A child of the player at eye height
  	float Yaw = 0.0f;
  	float Pitch = 0.0f;

  	void OnCreate() override { Input::SetCursorMode(CursorMode::Locked); }

  	void OnUpdate(float deltaTime) override
  	{
  		const glm::vec2 look = Input::GetGamepadStick(GamepadStick::Right);
  		Yaw -= Input::GetMouseDelta().x * MouseSensitivity + look.x * StickTurnSpeed * deltaTime;
  		Pitch = glm::clamp(Pitch - Input::GetMouseDelta().y * MouseSensitivity + look.y * StickTurnSpeed * deltaTime, -85.0f, 85.0f);
  		GetTransform().SetEulerAngles(glm::vec3(0.0f, glm::radians(Yaw), 0.0f));
  		if (Camera)
  			Camera.GetTransform().SetEulerAngles(glm::vec3(glm::radians(Pitch), 0.0f, 0.0f));

  		glm::vec2 move = Input::GetGamepadStick(GamepadStick::Left);    // +y forward
  		move.x += (Input::IsKeyDown(Key::D) ? 1.0f : 0.0f) - (Input::IsKeyDown(Key::A) ? 1.0f : 0.0f);
  		move.y += (Input::IsKeyDown(Key::W) ? 1.0f : 0.0f) - (Input::IsKeyDown(Key::S) ? 1.0f : 0.0f);
  		if (glm::length(move) > 1.0f)
  			move = glm::normalize(move);
  		const glm::vec3 forward = GetTransform().GetForward(), right = GetTransform().GetRight();
  		CharacterController controller = GetEntity().GetCharacterController();
  		controller.Move((glm::normalize(glm::vec3(forward.x, 0.0f, forward.z)) * move.y + right * move.x) * Speed);
  		if (controller.IsGrounded() && (Input::IsKeyPressed(Key::Space) || Input::IsGamepadButtonPressed(GamepadButton::A)))
  			controller.Jump(JumpSpeed);
  		if (Input::IsKeyPressed(Key::Escape))
  			Input::SetCursorMode(CursorMode::Normal); // A menu would go here
  	}
  };
  ```

  Register the fields with `ST_SCRIPT_FIELD`. Test it with `input.key {"key": "W", "frames": 60}` (or `input.gamepadAxis
  {"axis": "LeftY", "value": 1, "frames": 60}`) and read the player's `Translation` with `component.get`: it walks on the
  template's ground and stops at walls. In the editor the cursor stays locked only while the game view has the focus
  (Shift+F1 frees it); `input.mouseMove` moves the simulated pointer, so mouse look works from commands too.
- Hot reload replaces the instance: non-field members are gone in `OnReload`; find your entities again (e.g. children of
  the script's entity by name) instead of creating more.
- Script builds use the compiler's default warnings; compile your sources with `-Wall -Wextra` once (GCC or Clang with
  `-I StrataScriptCore/Include` and, for glm, `-isystem StrataScriptCore/ThirdParty/glm` in the Strata folder of a
  download or `-isystem Strata/vendor/glm` in a source checkout) to catch what other platforms' compilers would report.

## 5. Scene

Build it with `entity.create` (with `components`), `component.set`, `script.add`; then `scene.save` (the template's
start scene), or for a scene of your own `scene.saveAs {"path": "Scenes/Main.stscene"}` and `project.setStartScene`.
Save before switching to another scene or project: `scene.open`, `scene.new` and the `project.*` commands refuse while
the scene has unsaved changes (pass `"force": true` only to throw away edits you made yourself).

- Things that appear many times (blocks, enemies, pickups): build one, `prefab.create` it and `prefab.instantiate` the
  rest. Instances stay linked, so a change to one instance followed by `prefab.apply {"entity"}` updates the prefab and
  every unmodified instance at once (those of other scenes when the scene opens next: save it then); `prefab.status`
  shows which instances differ, and which are `outdated` (modified while the prefab changed elsewhere: `prefab.revert`
  them before changing them again, as `prefab.apply` refuses them without `force`).

- Collision layers keep groups apart (bullets that pass through their shooter, debris the player walks through): name
  them and set which pairs collide with `scene.setSettings {"layerNames": {"1": "Player", "2": "Debris"},
  "layerCollisions": [{"layers": ["Player", "Debris"], "collide": false}]}`, then put bodies and controllers on them by
  name (`component.set {"entity", "component": "RigidBody", "values": {"Layer": "Debris"}}`). Scripts change pairs while
  the game plays (`Physics::SetLayersCollide`) and mask queries by layer (`1u << Physics::GetLayerIndex("Player")`).
- Camera: `{"Camera": {"Primary": true, "PerspectiveFOV": 45}}`; cameras and lights look along their -Z, so a camera at
  `+Z` looking at the origin needs no rotation. Rotations are Euler degrees `[pitch, yaw, roll]`.
- Light: a `DirectionalLight` (Intensity ~3, pitch -35) plus a `SkyLight`: `"Source": "Procedural"` for a sky (its sun
  follows the directional light; `ZenithColor`, `HorizonColor`, `GroundColor`, `SunSize`, `SunIntensity`), or an
  environment map, or just an `AmbientColor` for fill (`ShowBackground: false` keeps the camera's clear color).
- Lighting you see in editor-camera captures may not be the game's: a scene without any light (directional, point, spot
  or sky light) is shown with *preview lighting* (`"previewLighting": true` in the capture result). The game renders it
  unlit; judge the look from `{"camera": "scene"}` captures. A scene with only point or spot lights shows as the game
  does: as dark as they leave it.
- Dark scenes: add a `PostProcess` with `AutoExposure: false` (automatic exposure brightens a dark backdrop to gray).
- HUD and menus: a canvas, so they keep their proportions on every screen. An entity with `UICanvas`
  (`"ScaleMode": "ScaleWithScreen"`, `"ReferenceResolution": [1280, 720]`, `"MatchWidthOrHeight": 1`, and
  `"ReceivesInput": false` for a HUD that must never block clicks) and children with `UIRect` (anchors and pivot 0..1 of
  the parent's rect, top-left origin; `Position` and `Size` in canvas units, which are pixels at the reference
  resolution) and `UIText`, `UIImage` (`CornerRadius`, `BorderWidth` for rounded, framed panels; a translucent `Color`)
  or `UILayout` (a vertical or horizontal stack; children with `"FitContent": "Both"` take their text's size).
  `Samples/Tetris` shows the score on a rounded panel in the top-left corner and its messages on a centered panel. `Text`
  stays for world text (`ScreenSpace: false`, `FontSize` in world units); screen-space `Text` keeps its pixel size on
  every screen, so do not build HUDs with it.
- `script.add {"entity", "class", "fields": {...}}`: asset fields take paths (`"Materials/Red.stmat"`, `"Builtin/Cube"`),
  entity fields take IDs from `entity.create`. Fields left out keep the class defaults.

## 6. Verify: play it and look at it

```text
play.start
input.key {"key": "Left"}                 # tap: answers after the game saw the release; issue taps one after another
input.key {"key": "Down", "frames": 20}   # held for 20 frames (KeyRepeat repeats)
component.get {"entity": "<HUD id>", "component": "UIText"}    # -> {"values": {"Text": "Score: 209", ...}}
log.read {"minLevel": "Info"}             # the script's Log::Info lines
viewport.capture {"camera": "scene", "width": 1280, "height": 720}   # CLI: --save-image shot.png, then look at it
play.stop
```

- Look at every capture you take. A capture waits (up to about five seconds) for the meshes, materials and textures it
  shows; `pendingAssets` above 0 means some were still loading: wait (`editor.wait`) and capture again. To look from
  elsewhere without moving the person's view, pass `position` and `target` (`camera.get` stays as it was).
- Editor-camera captures in edit mode leave the HUD out (`hiddenScreenSpaceTexts`, `ui.hiddenCanvases`); the scene's camera
  and play mode show it.
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
	{ "command": "component.get", "parameters": { "entity": "<score id>", "component": "UIText" }, "expect": { "/values/Text": { "equals": "Score: 36" } } }
]
```

Entity IDs are stable once the scene is saved. Make timing-dependent outcomes deterministic with fields (no gravity
while checking an exact score), then turn them back to let time play its part (fast gravity to reach game over).

Steps that create something can name what they made for later steps: `"bind": {"ball": "/id"}` on the step, then
`{"$ref": "ball"}` wherever a later step's parameters need the ID. To turn how you built the scene into a script, export
your session: `editor.activity.export {"path": "Build.json", "since": <journal.latest of editor.activity before you began>}`
writes it with those bindings, ready to run on a fresh copy of the project as it was then (take IDs for it from
`entity.find` or creating results rather than from the selection, which the script does not repeat).
In a source checkout, `StrataTests/Editor/TetrisCommands.json.in` and `TetrisSample.cmake` are the full example (CTest
`StrataEditor.Tetris`).

## 8. Export and check the game

```text
project.export {"directory": "<absolute, outside the project>"}   # <Game>[.exe], .stgame, .stpak, the script module
project.export {"directory": "<another one>", "prune": true}       # smaller: only what the scenes use (see below)
<dir>/<Game> --headless --frames 600       # 10 s of game time at 60 frames per second; exit code 0, the scripts' log lines
<dir>/<Game> --windowed --frames 120 --screenshot shot.png        # look at shot.png
```

In PowerShell pipe a game's or the editor's output (`... | Write-Output`) to wait for it and get its exit code: they are
windowed programs on Windows, which PowerShell does not wait for otherwise.

The exported game has no `--help`: unknown arguments are ignored and the game starts (fullscreen if the manifest says so).
The export runs in the background and answers when it finished (give the CLI call a long `--timeout` for big projects).
A pruned export leaves out assets no scene references (its result lists them in `excludedAssets`): when the scripts load
assets by path (`Assets::Find`, prefabs spawned by path), name them first with `project.setExportSettings
{"alwaysInclude": [...]}`, then run the pruned game headless to make sure it still finds everything. Games load their
start scene in the background (a black frame meanwhile): a large one can show a scene of its own while it loads,
`project.setExportSettings {"loadingScene": "Scenes/Loading.stscene"}` (a small scene with a camera and a `Text`; its
scripts read the start scene's progress with `Game::GetSceneLoadProgress()` for a loading bar).

## Pitfalls

| Symptom | Cause and fix |
| --- | --- |
| `input.key` fails "start it with play.start" | Input only reaches play mode (not edit or `play.simulate`). |
| A tap does nothing | The game is paused: the tap waits for the next `play.step` (or resuming). Or the game reacts to keys held over time: hold longer with `frames` or press/release. |
| HUD text cut off at the left edge | `UIText` and `Text` alignment default to `Center`; set `Alignment: Left`. |
| Everything looks washed out or gray | Automatic exposure; add `PostProcess` with `AutoExposure: false`. |
| Lit in captures, dark in the game | The capture used preview lighting (`"previewLighting": true`): add a `DirectionalLight` and a `SkyLight`. |
| The HUD is missing from a capture | Editor views hide screen-space text and canvases: capture with `{"camera": "scene"}` or while playing. |
| The score differs by a few points between runs | Gravity ticked between commands; make the check deterministic (slow gravity via a field). |
| `editor.quit` refuses | Unsaved changes: `scene.save` (or `edit.undo` the test's edits), or `{"force": true}`. |
| Script edits did nothing | Not built: `script.build` (it hot reloads while playing and restarts instances via `OnReload`). |
| The exported game logs no gameplay | The headless run was too short: frames are paced at 60 per second. |
