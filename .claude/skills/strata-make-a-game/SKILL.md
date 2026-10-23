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
- **Note where your work starts.** The editor's activity journal records every command you run (the person sees it in
  the Activity panel), and section 7 turns your session into a regression script from that point: before your first
  change, call `editor.activity {"newest": true, "limit": 1}` and keep `journal.latest`.
- Through MCP only the `core` toolset is listed at first; `strata_describe` lists the others and `strata_toolsets
  {"enable": ["scene"]}` adds them, and `strata_call` runs any command. Start every check with `editor.status`: its
  `errors` say whether a build failed, a script crashed, an import failed or errors were logged since your last
  `log.read`.

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
  `UVTiling`, `UVOffset`; `Clearcoat` (0-1) with `ClearcoatRoughness` for car paint, lacquer or a wet look; `Transmission`
  (1 for clear glass, water) with `IOR` (glass 1.5) and `Thickness` (0 for a window pane, more for solid glass that
  bends what lies behind it), tinted by `BaseColor` and `AttenuationColor`. Glass shows what is behind it, not other
  glass or `Blend` surfaces behind it. `material.get {"material": "Builtin/DefaultMaterial"}` describes them all.
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
- **Models, before you place them:**
  - Size: `asset.info {"asset": "<model>"}` gives `bounds` (`size` in meters, as placed; skinned models in their rest
    pose), and `entity.get {"entity", "world": true}` an instance's world transform and bounds. Characters are about 1.8
    m tall; a model 100 times too large was authored in centimeters: set its import `Scale` (`asset.setImportSettings
    {"asset", "settings": {"Scale": 0.01}}`). The scale is baked into the model's content (the root keeps scale 1), and
    instances placed before keep their size.
  - Facing: glTF models face +Z, while `GetForward()` (cameras, lights, scripts) is -Z. A character that walks
    backwards needs its facing computed for +Z (below) or a child entity turned 180 degrees holding the model.
  - Textures: normal maps and roughness maps import by their name tokens (`_normal`, `_n`, `_rough`...); `material.get`
    and `material.set` return `warnings` with a `fix` when a texture was imported for another use than its slot.

### Downloaded assets

Assets made by others must be CC0 or CC-BY 4.0 (with the attribution it asks for); leave out anything whose license is
unclear. Download each pack into a new, empty directory, inspect and trim it with scripts kept elsewhere (`python3 -I`),
and bring files in only through `asset.import`. Fix files the importer rejects before importing (Kenney's Nature Kit lists
a child node as its scene's root: list the root nodes instead) and record every change. A sample in `Samples/` credits
every imported file in its `CREDITS.md` table (`| Files | Source | Author | License | Attribution | Changes |`) and each
pack in `ThirdPartyNotices.md`'s "Sample assets" table; `Samples.Content` checks both and the size budgets.

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
  game UI") and test them with `input.click`. A name or high-score entry is an `inputField` whose script reads the text in
  `OnUISubmit`.
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

  Register the fields with `ST_SCRIPT_FIELD`. Test it with `input.key {"key": "W", "steps": 60}` (or `input.gamepadAxis
  {"axis": "LeftY", "value": 1, "steps": 60}`) and read the player's `Translation` with `component.get`: it walks on the
  template's ground and stops at walls. In the editor the cursor stays locked only while the Game view has the focus
  (Shift+F1 frees it); `input.mouseMove` moves the simulated pointer, so mouse look works from commands too.
- A third-person player and a follow camera: the player entity has a `CharacterController` and the model (the root of
  a model instance, or a child holding it); the primary camera is a separate entity (not a child) with the second
  script, its `Target` field set to the player. Movement is relative to the camera, and the model turns to face where it
  walks:

  ```cpp
  class ThirdPersonPlayer : public Script
  {
  public:
  	float Speed = 4.0f;
  	float JumpSpeed = 5.0f;
  	Entity Camera;   // The follow camera: movement is relative to where it looks

  	void OnUpdate(float) override
  	{
  		glm::vec2 input = Input::GetGamepadStick(GamepadStick::Left);    // +y forward
  		input.x += (Input::IsKeyDown(Key::D) ? 1.0f : 0.0f) - (Input::IsKeyDown(Key::A) ? 1.0f : 0.0f);
  		input.y += (Input::IsKeyDown(Key::W) ? 1.0f : 0.0f) - (Input::IsKeyDown(Key::S) ? 1.0f : 0.0f);
  		if (glm::length(input) > 1.0f)
  			input = glm::normalize(input);
  		// The camera's forward and right, flattened onto the ground.
  		glm::vec3 forward = Camera ? Camera.GetTransform().GetForward() : glm::vec3(0.0f, 0.0f, -1.0f);
  		forward = glm::normalize(glm::vec3(forward.x, 0.0f, forward.z));
  		const glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
  		const glm::vec3 direction = forward * input.y + right * input.x;
  		CharacterController controller = GetEntity().GetCharacterController();
  		controller.Move(direction * Speed);
  		// Face the walking direction. glTF models face +Z: yaw = atan2(x, z) turns +Z onto it.
  		if (glm::length(direction) > 0.01f)
  			GetTransform().SetEulerAngles(glm::vec3(0.0f, std::atan2(direction.x, direction.z), 0.0f));
  		if (controller.IsGrounded() && (Input::IsKeyPressed(Key::Space) || Input::IsGamepadButtonPressed(GamepadButton::A)))
  			controller.Jump(JumpSpeed);
  		GetEntity().GetAnimator().SetFloat("Speed", glm::length(controller.GetVelocity())); // False without an Animator
  	}
  };

  class FollowCamera : public Script
  {
  public:
  	Entity Target;
  	float Distance = 6.0f;
  	float Height = 3.0f;
  	float Smoothing = 8.0f; // Higher follows faster
  	float Yaw = 0.0f;       // Degrees around the target; the right stick turns it

  	void OnUpdate(float deltaTime) override
  	{
  		if (!Target)
  			return;
  		Yaw -= Input::GetGamepadStick(GamepadStick::Right).x * 120.0f * deltaTime;
  		const glm::vec3 target = Target.GetTransform().GetWorldPosition() + glm::vec3(0.0f, 1.0f, 0.0f);
  		const float yaw = glm::radians(Yaw);
  		const glm::vec3 wanted = target + glm::vec3(std::sin(yaw) * Distance, Height, std::cos(yaw) * Distance);
  		TransformComponent transform = GetTransform();
  		const float blend = 1.0f - std::exp(-Smoothing * deltaTime); // The same smoothing at any frame rate
  		const glm::vec3 position = glm::mix(transform.GetWorldPosition(), wanted, blend);
  		transform.SetWorldPosition(position);
  		// Cameras look along -Z: quatLookAt gives that rotation.
  		transform.SetWorldRotation(glm::quatLookAt(glm::normalize(target - position), glm::vec3(0.0f, 1.0f, 0.0f)));
  	}
  };
  ```

  Register the fields with `ST_SCRIPT_FIELD` (each class in its own `.cpp`, which includes `<cmath>` and, for
  `glm::quatLookAt`, `<glm/gtc/quaternion.hpp>`). Test it with `input.sequence` (section 6) observing the player's `world` position: W
  held walks away from the camera, A and D walk across its view.
- Hot reload replaces the instance: non-field members are gone in `OnReload`; find your entities again (e.g. children of
  the script's entity by name) instead of creating more.
- Script builds use the compiler's default warnings; compile your sources with `-Wall -Wextra` once (GCC or Clang with
  `-I StrataScriptCore/Include` and, for glm, `-isystem StrataScriptCore/ThirdParty/glm` in the Strata folder of a
  download or `-isystem Strata/vendor/glm` in a source checkout) to catch what other platforms' compilers would report.

## 5. Scene

Build it with `entity.create` (with `components`), `component.set`, `script.add`; then `scene.save` (the template's
start scene), or for a scene of your own `scene.saveAs {"path": "Scenes/Main.stscene"}` and `project.setStartScene`.
Save before switching to another scene or project: `scene.open`, `scene.new` and the `project.*` commands refuse while
the scene has unsaved changes (pass `"force": true` only to throw away edits you made yourself). Saves write in the
background: `scene.save` answers once the file is written (with `bytes`, `entities` and `seconds`); for a large scene
you can go on with `{"wait": false}` and watch `editor.status`'s `saving` (the scene-replacing commands wait for it).

- Things that appear many times (blocks, enemies, pickups): build one, `prefab.create` it and `prefab.instantiate` the
  rest. Instances stay linked, so a change to one instance followed by `prefab.apply {"entity"}` updates the prefab and
  every instance at once, except values an instance overrides (those of other scenes when the scene opens next: save it
  then). `prefab.status` lists an instance's overrides; `prefab.apply` and `prefab.revert` take `component` and `property`
  to apply or revert one of them.

- Collision layers keep groups apart (bullets that pass through their shooter, debris the player walks through): name
  them and set which pairs collide with `scene.setSettings {"layerNames": {"1": "Player", "2": "Debris"},
  "layerCollisions": [{"layers": ["Player", "Debris"], "collide": false}]}`, then put bodies and controllers on them by
  name (`component.set {"entity", "component": "RigidBody", "values": {"Layer": "Debris"}}`). Scripts change pairs while
  the game plays (`Physics::SetLayersCollide`) and mask queries by layer (`1u << Physics::GetLayerIndex("Player")`).
- Camera: `{"Camera": {"Primary": true, "PerspectiveFOV": 45}}`; cameras and lights look along their -Z, so a camera at
  `+Z` looking at the origin needs no rotation. Rotations are Euler degrees `[pitch, yaw, roll]`.
- Light: a `DirectionalLight` (Intensity ~3, pitch -35) plus a `SkyLight`: `"Source": "Procedural"` for a sky (its sun
  follows the directional light; `ZenithColor`, `HorizonColor`, `GroundColor`, `SunSize`, `SunIntensity`), or an
  environment map, or just an `AmbientColor` for fill (`ShowBackground: false` keeps the camera's clear color). With
  an HDR sky that shows a sun, `lighting.extractSun` points the directional light where the sky's sun is, with its color
  and brightness, and `asset.setImportSettings {"asset": <map>, "settings": {"RemoveSun": true}}` then paints the sun out
  of the map, so it does not light the scene twice (unshadowed); bake Reflection Probes again afterwards.
- Lighting you see in editor-camera captures may not be the game's: a scene without any light (directional, point, spot
  or sky light) is shown with *preview lighting* (`"previewLighting": true` in the capture result). The game renders it
  unlit; judge the look from `{"camera": "scene"}` captures. A scene with only point or spot lights shows as the game
  does: as dark as they leave it.
- Dark scenes: add a `PostProcess` with `AutoExposure: false` (automatic exposure brightens a dark backdrop to gray).
- Anti-aliasing (`PostProcess`): 3D games use `"AntiAliasingMethod": "TAA"` with the default `TAAPreset` `"Sharp"`
  (crisp, smooth edges); `"Original"` gives a softer, steadier look; when transparent effects ghost as they move, set
  `"TAAPreset": "Custom"` with a higher `TAATransparentWeight`. 2D games and pixel art keep `"FXAA"` (TAA softens
  their pixels). `component.set` applies the values it is given, and the tunables start at Sharp's: to change one
  technique against Original, start Custom from Original's values, `{"TAAPreset": "Custom", "TAAHistoryFrames": 8,
  "TAAMotionResponse": 0.03125, "TAAMaxMotionWeight": 0.5, "TAAResampleWeight": 0, "TAAHistoryClamp": "MinMax",
  "TAATransparentWeight": 0, "TAADisocclusionRejection": false, "TAASharpening": 0}`. See Docs/Manual/AntiAliasing.md.
- Reflections: with TAA, `"ScreenSpaceReflections": true` on the `PostProcess` makes smooth floors, water and metal
  reflect what the screen shows near them (only surfaces below `ScreenSpaceReflectionsMaxRoughness`, 0.5: matte kit
  materials at roughness 1 show none). For what the screen does not show, bake Reflection Probes (Docs/Manual/Lighting.md).
- HUD and menus: a canvas, so they keep their proportions on every screen. An entity with `UICanvas`
  (`"ScaleMode": "ScaleWithScreen"`, `"ReferenceResolution": [1280, 720]`, `"MatchWidthOrHeight": 1`, and
  `"ReceivesInput": false` for a HUD that must never block clicks) and children with `UIRect` (anchors and pivot 0..1 of
  the parent's rect, top-left origin; `Position` and `Size` in canvas units, which are pixels at the reference
  resolution) and `UIText`, `UIImage` (`CornerRadius`, `BorderWidth` for rounded, framed panels; a translucent `Color`;
  or an imported PNG as a nine-slice image: `"Type": "Sliced"`, `SliceBorders` in texels, as the Playground's and the
  Third-Person's buttons and panels use the Kenney UI Pack) or `UILayout` (a vertical or horizontal stack; children with `"FitContent": "Both"` take their text's size).
  `Samples/Tetris` shows the score on a rounded panel in the top-left corner and its messages on a centered panel. `Text`
  stays for world text (`ScreenSpace: false`, `FontSize` in world units); screen-space `Text` keeps its pixel size on
  every screen, so do not build HUDs with it. Panels with buttons in the level (a terminal, a shop sign) are canvases
  with `"RenderMode": "WorldSpace"`: the player clicks them through the camera.
- `script.add {"entity", "class", "fields": {...}}`: asset fields take paths (`"Materials/Red.stmat"`, `"Builtin/Cube"`),
  entity fields take IDs from `entity.create`. Fields left out keep the class defaults.
- A large level is a world: `world.create {"path": "Worlds/Island", "cellSize": 64}` makes a World asset and its
  persistent scene (opened), with layers Terrain, Buildings, Props and Gameplay (`world.setLayers` changes them). Keep
  cameras, lights, game managers and the player in the persistent scene (the default edit layer, "Persistent"); place
  level content by layer: `world.edit {"editLayer": "Props"}`, then create entities at their positions (each goes into
  the cell under it; `world.moveEntities` moves existing ones). A `StreamingSource` on the player (or the primary camera)
  streams the cells around it while the game plays. `scene.save` writes the dirty cells, `world.cook` sizes physics for
  the busiest radius, and `project.export` cooks and packs the world. Scripts that teleport or spawn far away prefetch
  first (`World::RequestCellsAround`, `World::IsLoadedAround`, strata-scripting). Worlds stay within 20 km of the origin.

## 6. Verify: play it and look at it

```text
play.start
input.key {"key": "Left"}                 # tap: answers after the game saw the release; issue taps one after another
input.key {"key": "Down", "steps": 20}    # held for 20 fixed steps (KeyRepeat repeats)
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
- **Play-test deterministically.** Gravity and other timers keep running between your calls (a call takes about 0.1 s),
  and how many fixed steps run per frame depends on the machine. For exact results:
  - `input.sequence` plays a whole script of input in the game's fixed steps and samples what happened: it pauses the
    game, runs exactly one step per frame with that step's events, resumes, and answers with the samples. For example
    W held for 60 steps, then a jump, watching the player:
    `{"steps": 90, "events": [{"step": 0, "key": "W", "action": "press"}, {"step": 60, "key": "W", "action": "release"},
    {"step": 60, "key": "Space", "action": "tap"}], "observe": [{"entity": "<player>", "world": true, "every": 30},
    {"entity": "<game>", "script": "GameManager", "field": "Coins"}]}`. The same sequence gives the same samples at any
    frame rate, in a headless editor and in a window: assert on them in your regression script.
  - `play.pause {"paused": true}` and `play.step {"steps": n}` answer once those n fixed steps ran (`fixedSteps` in the
    result counts them); input given while paused arrives in the next step.
  - Input durations (`input.key {"steps": 20}`, `{"seconds": 0.5}`) count the game's fixed steps, not frames: a key held
    for 60 steps moves a player the same distance however fast the editor draws.
    A pause menu at `Time::SetTimeScale(0)` runs no steps: taps then last frames instead, so `input.click` on its buttons
    works too.
  - Or set slow timings with `script.setField` while exploring.
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

Outcomes that take game time (a domino run falling, a player reaching a coin) are waited for, not guessed: give the query
step `"until": {"steps": 1200}` and it repeats until its `expect` holds, failing after 1,200 fixed steps (or `"seconds"`,
300 by default). Pause first (`play.pause`): a paused wait runs one fixed step per attempt, so the check is exact on any
machine. A check of a difference or an "or" (moved 3 m, Walk or Run) is a script field that computes it, since `expect`
compares with constants only:

```json
{ "command": "play.pause", "parameters": { "paused": true } },
{ "command": "script.get", "parameters": { "entity": "<game id>", "class": "Playground" },
  "expect": { "/scripts/0/fields/DominoesDown": { "min": 60 } }, "until": { "steps": 1200 } }
```

Steps that create something can name what they made for later steps: `"bind": {"ball": "/id"}` on the step, then
`{"$ref": "ball"}` wherever a later step's parameters need the ID. To turn how you built the scene into a script, export
your session: `editor.activity.export {"path": "Build.json", "since": <the journal.latest you noted in section 1>}`
writes it with those bindings, ready to run on a fresh copy of the project as it was then (take IDs for it from
`entity.find` or creating results rather than from the selection, which the script does not repeat). Files you wrote
yourself are not commands: to replay on a fresh copy, copy the project's `Scripts/` (and source assets you generated)
into it first, then run the script, which builds them.

If the editor crashed or was killed, nothing you did is lost: the project's recovery journal kept the unsaved changes.
`scene.recoveryStatus` lists entries (`complete: true` for a scene never saved: a snapshot of every entity rather than
the changes), and `scene.recover {"scene": "<id>", "action": "apply"}` brings them back.
In a source checkout, `StrataTests/Editor/TetrisCommands.json.in` and `SampleProject.cmake` are the full example (CTest
`StrataEditor.Tetris`; the runner fails on any warning the test does not list as expected).

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

## Worked examples and recipes

The samples in `Samples/` were made this way; their `README.md` says how, and `StrataTests/Editor/<Sample>Commands.json.in`
is each one's regression:

- **Tetris**: scripted rules on a grid, a HUD, generated sounds.
- **PhysicsPlayground**: ~600 bodies placed with `prefab.instantiate {instances}`; shooting through the cursor (the camera's
  field of view and the window size give the ray), grabbing (a velocity toward a point in front of the camera each fixed
  step), impact sounds by material with a voice budget shared by every body, a trigger that counts balls, a collision
  layer that passes through another (`scene.setSettings {layerNames, layerCollisions}`), R reloading the scene to reset.
- **OpenWorld**: a generated terrain (`Tools/GenerateIslandTerrain.py`) kept in a world's persistent scene with static
  mesh colliders; 20,000 props scattered into a world layer (`world.edit {editLayer}`, `world.loadCells` to pin the
  region first, then `prefab.instantiate {instances}` steps); foxes with an animation controller; teleports that prefetch
  cells and wait for the ground (`World::IsLoadedAround` plus a raycast); a day-night script driving the sun and the
  procedural sky's colors; a camera tour; the export's loading scene with a progress bar.
- **ThirdPerson**: a character controller whose child is a skinned model (scaled from `asset.info` bounds, turned 180
  degrees: glTF faces +Z) with an animation controller (`animation.createController`: float, bool and trigger parameters,
  cross-fades, `Footstep` events at times read off the clip's foot keys; the events reach scripts on the animator's
  entity); camera-relative movement with coyote time; a follow camera whose arm a raycast shortens; triggers that collect
  and launch; a crate the controller pushes; a main menu scene that loads the course (`Game::LoadScene`, the scene as an
  asset field so pruned exports keep it); settings in a save file applied with `Audio::SetMasterVolume`,
  `Game::SetWindowMode` and `Game::SetVSync`; a pause menu at time scale 0; an attract mode kept in the script module's
  own state across scene loads; a flag in a hand (`AttachToJoint`: the joint's transform carries the model's scale, so
  the child's scale divides by it).

Every sample has a `Picture.png`: save the start scene in an editor with a GPU and copy `.strata/Thumbnail.png`; the
launcher shows it and copies start with it.

## Pitfalls

| Symptom | Cause and fix |
| --- | --- |
| `input.key` fails "start it with play.start" | Input only reaches play mode (not edit or `play.simulate`). |
| A tap does nothing | The game is paused: the tap waits for the next `play.step` (or resuming). Or the game reacts to keys held over time: hold longer with `steps` (fixed steps of the game) or press/release. |
| HUD text cut off at the left edge | `UIText` and `Text` alignment default to `Center`; set `Alignment: Left`. |
| Everything looks washed out or gray | Automatic exposure; add `PostProcess` with `AutoExposure: false`. |
| Lit in captures, dark in the game | The capture used preview lighting (`"previewLighting": true`): add a `DirectionalLight` and a `SkyLight`. |
| The HUD is missing from a capture | Editor views hide screen-space text and canvases: capture with `{"camera": "scene"}` or while playing. |
| The score differs by a few points between runs | Gravity ticked between commands; play the moves with `input.sequence` (exact in game steps), or slow gravity via a field. |
| The model is huge, tiny or walks backwards | Check `asset.info` `bounds` and set the import `Scale`; glTF faces +Z while `GetForward()` is -Z (see section 3). |
| `editor.quit` refuses | Unsaved changes: `scene.save` (or `edit.undo` the test's edits), or `{"force": true}`. |
| Script edits did nothing | Not built: `script.build` (it hot reloads while playing and restarts instances via `OnReload`). |
| The exported game logs no gameplay | The headless run was too short: frames are paced at 60 per second. |
