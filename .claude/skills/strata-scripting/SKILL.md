---
name: strata-scripting
description: How to write, build, attach, debug and ship Strata game scripts (C++ script classes in a game project's script module, built against the StrataScriptCore SDK) through the editor's script.* commands - the lifecycle, the whole gameplay API (entities, components, transforms, input with keyboard, mouse, gamepads and the cursor, time, physics bodies, raycasts, contacts and collision layers, character controllers, audio, quitting and loading scenes, the game window, save files, random numbers, timers, key repeat) and how to test it. Use when creating gameplay logic or whole games for a project, adding script classes or fields, spawning prefabs from scripts, iterating with hot reload, diagnosing script build errors or crashes, or exporting a scripted game.
---

# Writing Strata game scripts

Game logic lives in C++ script classes compiled into a **script module** (a shared library). Scripts see only the
SDK in `StrataScriptCore/Include/StrataScript/` - never engine headers. Include one header:

```cpp
#include "StrataScript/StrataScript.h"
```

The SDK (`StrataScriptCore/`) and Strata's executables (StrataEditor, StrataRuntime, StrataCLI) are in the Strata
folder of a downloaded release; in a source checkout the SDK is at its root and the executables are in
`build/<preset>/bin/<Config>/`. In a source checkout, the engine-side rules (ABI, adding host functions) are in
AGENTS.md, section "Scripting", and their details in Docs/DeveloperGuide.md, section "Scripting".

## A script class

```cpp
#include "StrataScript/StrataScript.h"

#include "Projectile.h" // Declares class Projectile (its ST_SCRIPT_CLASS is in Projectile.cpp)

using namespace Strata;

class Turret : public Script
{
public:
	// Fields: public members registered below. Shown in the inspector, stored per entity in the scene and kept across
	// hot reloads. The initializer is the default.
	float FireInterval = 0.5f;
	AssetHandle Bullet;            // Set in the editor (a prefab)
	Entity Target;                 // Another entity of the scene
	std::string Label = "Turret";

	void OnCreate() override
	{
		// Fields already hold their per-entity values here. Request assets you will spawn later.
		Assets::RequestLoad(Bullet);
	}

	void OnUpdate(float deltaTime) override
	{
		m_Cooldown -= deltaTime;
		if (m_Cooldown > 0.0f || !Target.IsValid())
			return;
		m_Cooldown = FireInterval;

		TransformComponent transform = GetTransform();
		Entity bullet = Scene::Instantiate(Bullet, transform.GetWorldPosition(), transform.GetWorldRotation());
		if (Projectile* projectile = bullet.GetScript<Projectile>()) // Exists right away; its OnCreate runs later
			projectile->Speed = 20.0f;
	}
private:
	float m_Cooldown = 0.0f; // Not a field: reset by hot reload
};

ST_SCRIPT_CLASS(Turret)
{
	ST_SCRIPT_FIELD(FireInterval);
	ST_SCRIPT_FIELD(Bullet);
	ST_SCRIPT_FIELD(Target);
	ST_SCRIPT_FIELD(Label);
}
```

- One `ST_SCRIPT_CLASS` per class, in a `.cpp` file (never in a header). The class name used by Script components is
  the spelled type name: `Turret`, or `Game::Turret` for `ST_SCRIPT_CLASS(Game::Turret)`.
- Field types: `bool`, `int32_t`, `float`, `glm::vec2/3/4`, `glm::quat`, `std::string`, `Strata::Entity`,
  `Strata::AssetHandle`. Fields must be public data members (of the class or a base class).
- Classes must be default constructible. The module constructs each class once while loading (without an entity) to
  read the defaults, so constructors must not have side effects - initialize in `OnCreate`.
- To use a script class from another `.cpp` (e.g. `GetScript<Projectile>()`), declare the class in a header and keep
  its `ST_SCRIPT_CLASS` in exactly one `.cpp`.

## Lifecycle

| Callback | When |
| --- | --- |
| constructor | Instance created; then the entity's stored field values are applied. |
| `OnCreate()` | Once, before the first update, after every script that exists at that moment was constructed. Physics and audio already run. |
| `OnUpdate(dt)` | Every frame. Order: entity hierarchy (parents first), then the order of scripts on the entity. |
| `OnFixedUpdate(dt)` | Zero or more times per frame at the fixed timestep (gameplay physics). |
| `OnLateUpdate(dt)` | Every frame after the fixed updates. |
| `OnDestroy()` | Entity destroyed (still valid here), script removed, or play stopped (descendants first). |
| `OnReload()` | After a hot reload, instead of `OnCreate`. Re-acquire cached pointers and non-field state. |
| `OnCollisionEnter/Exit(collision)` | The entity's physics body starts/stops touching another body (after a fixed step). |
| `OnTriggerEnter/Exit(collision)` | The same for contacts where either body is a trigger. |
| `OnAnimationEvent(name)` | The entity's animator passed an event of its controller's current state (after the animators were evaluated). |

- Inactive entities (or children of inactive entities) get no update callbacks.
- Entities destroyed during a frame stay valid and keep updating until the frame ends.
- Scripts added or spawned during a frame exist immediately and start (`OnCreate`) before their first update.
- Implement only the callbacks you need; the engine skips the others entirely.

## API cheat sheet

- `GetEntity()`, `GetTransform()` - the script's own entity.
- `Entity`: `IsValid`, `GetName/SetName`, `GetTag/SetTag`, `IsActive/IsActiveInHierarchy/SetActive`,
  `GetParent/SetParent/GetChildren`, `Destroy` (end of frame), `HasComponent/AddComponent/RemoveComponent(name)`,
  `GetProperty<T>(component, property)` / `SetProperty(component, property, value)` for **every** registered component
  ("Camera", "MeshRenderer", "Text", "RigidBody", ...; names and properties as in the inspector, case-insensitive),
  `GetComponent(name)` proxy, `GetScript<T>/AddScript<T>/HasScript/RemoveScript`.
  Enum properties are integers, colors are `glm::vec3/vec4`, rotations `glm::quat`.
- `TransformComponent`: local `Get/SetTranslation`, `Get/SetRotation`, `Get/SetScale`, `Get/SetEulerAngles` (radians),
  world `Get/SetWorldPosition`, `Get/SetWorldRotation`, `GetWorldScale`, `GetForward/Right/Up` (-Z is forward).
- `Scene`: `CreateEntity(name, parent)`, `GetEntity(id)`, `FindEntityByName`, `FindEntitiesByTag`, `GetRootEntities`,
  `GetPrimaryCamera`, `Instantiate(prefabOrModel [, translation, rotation, scale] [, parent])` by handle or asset path.
- `Assets`: `Find(path)`, `IsLoaded`, `RequestLoad`, `Release`. `Instantiate` never waits: for an asset that is not
  loaded yet it starts the load and returns a null entity - request assets early (in `OnCreate`) and spawn once
  `IsLoaded`. A requested asset stays loaded while the scene plays (the engine does not unload it to stay within its
  memory budgets); `Release(asset)` ends that once it is no longer needed (e.g. a level's assets after leaving it), and
  stopping play releases everything the scripts requested.
- `Input`: `IsKeyDown/Pressed/Released(Key::W)`, `IsMouseButtonDown/Pressed/Released(Mouse::ButtonLeft)`,
  `GetMousePosition` (viewport pixels), `GetMouseDelta`, `GetScrollDelta`. In the editor, scripts receive device input only
  while the viewport is the focused game view (playing through the scene's primary camera); positions are relative to
  the image. Tools play the game with the `input.*` editor commands (simulated input, see the editor automation skill),
  which reach the scripts whether or not the viewport has the focus. Gamepads and the cursor: see
  [Gamepads and the cursor](#gamepads-and-the-cursor).
- `Time`: `GetDeltaTime`, `GetFixedDeltaTime`, `GetElapsedTime`, `GetFrameIndex`, `Get/SetTimeScale`.
- `Log::Info/Warn/Error/Trace(args...)` - arguments are concatenated (`Log::Info("Health ", health)`).
- `RigidBody` (`GetEntity().GetRigidBody()`), `Physics` (raycasts, overlaps), `Collision` (contact callbacks),
  `CharacterController` (`GetEntity().GetCharacterController()`: walk, jump, ground state),
  `AudioSource` (`GetEntity().GetAudioSource()`), `Audio` (one-shots, master volume), `Animator`
  (`GetEntity().GetAnimator()`: states, parameters, joints), `Game` (quit, load scenes, the window, save files),
  `Random`, `Timer`, `KeyRepeat`: see [Gameplay API](#gameplay-api).

Every call on a missing entity, unknown component or wrong value type fails harmlessly (`false`, empty `optional`,
null entity) and the engine logs a warning naming the calling script.

## Gameplay API

### Physics

Bodies are entities with a `RigidBody` component and colliders (`BoxCollider`, `SphereCollider`, `CapsuleCollider`,
`MeshCollider` on the entity or its descendants). Set up type (Static/Dynamic/Kinematic), mass, friction, layer,
`IsTrigger`, `LockRotation*` and `GravityScale` as component properties (in the scene, or with `SetProperty`).

```cpp
RigidBody body = GetEntity().GetRigidBody();
body.SetLinearVelocity({ 0.0f, 5.0f, 0.0f });      // Dynamic bodies only (velocities can be read from any body)
body.AddImpulse({ 0.0f, 7.0f, 0.0f });             // Changes the velocity at once (jumps, hits)
body.AddForce({ 10.0f, 0.0f, 0.0f });              // Acts during the next fixed step only: apply in OnFixedUpdate
body.Teleport({ 0.0f, 2.0f, 0.0f });               // Moves body and entity at once (respawns), keeps velocities

// Queries see the bodies as of the last fixed step. Layer masks select RigidBody layers (bit n = layer n).
const glm::vec3 down(0.0f, -1.0f, 0.0f);
if (std::optional<RaycastHit> hit = Physics::Raycast(origin, down, 1.1f, Physics::c_AllLayers, GetEntity()))
	grounded = hit->Distance < 1.05f;        // hit->HitEntity, hit->Point, hit->Normal
std::vector<Entity> nearby = Physics::OverlapSphere(center, 3.0f);
```

- `RigidBody`: `Get/SetLinearVelocity`, `Get/SetAngularVelocity` (radians/s), `AddForce`, `AddForceAtPosition`,
  `AddImpulse`, `AddImpulseAtPosition`, `AddTorque`, `AddAngularImpulse`, `Teleport(position [, rotation])`.
- `Physics`: `Raycast` (closest hit), `RaycastAll` (sorted by distance), `OverlapSphere`, `OverlapBox`; triggers are
  skipped unless `includeTriggers` is set, `ignore` skips one entity (usually the caster).
- Collision layers: every body and character controller is on one layer (0-31, `Layer`), and two of them collide only
  when the scene's layer matrix lets their layers (everything collides by default). Name layers and set the matrix in
  the editor (Inspector with nothing selected, Collision Layers; or `scene.setSettings {layerNames, layerCollisions}`),
  then use names in code: `Physics::GetLayerIndex("Debris")` (-1 for a name the scene lacks), `1u << layer` as a query
  mask, `Physics::SetLayersCollide(a, b, false)` to let two layers pass through each other while the scene plays (e.g.
  a ghost power-up), `Physics::GetLayersCollide(a, b)`.
- Moving a body by writing its transform works too (kinematic platforms, editor-like placement) but takes effect at the
  next fixed step; `Teleport` is immediate. Results always name the entity that owns the body (the RigidBody entity,
  also for colliders on its children).

### Character controllers

A player or NPC that walks rather than tumbles: an entity with a `CharacterController` component (an upright capsule
whose bottom is the entity's origin: `Height`, `Radius`, `StepHeight`, `MaxSlopeDegrees`, `Layer`, `GravityScale`,
`PushStrength`, `Enabled`) and no `RigidBody`. Physics moves it every fixed step: it falls, slides along walls, walks up
steps and walkable slopes, follows the ground down and pushes dynamic bodies, and writes the entity's position (never its
rotation: turn the entity yourself).

```cpp
CharacterController controller = GetEntity().GetCharacterController();
controller.Move(direction * Speed);   // Walk velocity (m/s, world space), kept until the next Move; Move(glm::vec3(0)) stops
if (controller.IsGrounded() && Input::IsKeyPressed(Key::Space))
	controller.Jump(5.0f);            // Upward speed at the next fixed step (also in the air: check IsGrounded)
controller.Teleport(spawnPoint);      // Feet at that point, at once (respawns)
controller.GetGroundState();          // GroundState::OnGround, OnSteepGround (sliding), NotSupported, InAir
controller.GetGroundNormal(); controller.GetGroundEntity(); controller.GetGroundVelocity(); // What it stands on
controller.GetVelocity();             // How it actually moved in the last step (zero against a wall)
```

Move from `OnUpdate` (input) or `OnFixedUpdate`; the vertical part of the walk velocity counts only with `GravityScale` 0
(flying, swimming). What it walks on and into reaches its script as `OnCollisionEnter/Exit` (the ground too: the normal
points down to it), triggers it walks through as `OnTriggerEnter/Exit`; raycasts and overlaps find it, and dynamic
bodies land on it. An entity with both a `RigidBody` and a `CharacterController` is the rigid body (with a warning).

### Contacts

Override the contact callbacks on the script of an entity that owns a body (or a character controller):

```cpp
void OnCollisionEnter(const Collision& collision) override  // Solid contacts begin
{
	if (collision.Other.GetTag() == "Enemy" && collision.Normal.y < -0.7f) // Normal points from us to the other
		collision.Other.Destroy();                                        // Safe: destruction happens at frame end
}
void OnCollisionExit(const Collision& collision) override {}
void OnTriggerEnter(const Collision& collision) override {}   // Either body is a trigger (IsTrigger): pickups, zones
void OnTriggerExit(const Collision& collision) override {}
```

They run after the fixed step that found the change, for the scripts of both entities. Every Enter is followed by one
Exit, also when the entity is deactivated meanwhile (a pooled pickup ends its contacts when it is disabled), so
counters of touching bodies stay balanced. `collision.Other` may name an entity that is already gone in an Exit caused by
its destruction: check `IsValid()` before using it.

### Audio

```cpp
AudioSource music = GetEntity().GetAudioSource(); // Entity with an AudioSource component (clip, volume, loop, 3D...)
music.Play();  music.Pause();  music.Stop();  music.Seek(1.5f);  music.IsPlaying();  music.GetPlaybackPosition();
Audio::PlayOneShot(clip, 0.8f);                    // Fire and forget (UI, pickups); pauses and stops with the scene
Audio::PlayOneShotAt(clip, position);              // 3D one-shot heard from the listener
Audio::SetMasterVolume(0.5f);                      // Game-wide volume option
```

Request clips early (`Assets::RequestLoad(clip)` in `OnCreate`): a one-shot whose clip takes longer than a quarter
second to load is dropped. Clip, volume, pitch, looping and spatial settings are `AudioSource` properties.

### Animation

Characters are entities with an `Animator` component (a model's root gets one on import: its skeleton and first clip)
whose meshes carry `Skin` components naming it. With an animation controller (`.stanimator`: states, transitions,
parameters, events; made with `animation.createController`, see the editor automation skill) scripts mostly set
parameters and let the transitions do the rest:

```cpp
Animator animator = GetEntity().GetAnimator();
animator.SetFloat("Speed", glm::length(velocity));   // Idle <-> Walk transitions test it
animator.SetBool("Grounded", grounded);
if (Input::IsKeyPressed(Key::Space))
	animator.SetTrigger("Jump");                     // Consumed by the transition that fires on it
animator.Play("Wave");                               // Jump to a state; CrossFade("Wave", 0.2f) blends to it
animator.Play(clip);                                 // A clip asset directly, without the controller's transitions
animator.SetSpeed(1.5f);
std::string state = animator.GetCurrentState();      // "Walk" (empty while a clip plays directly)
float progress = animator.GetNormalizedTime();       // 1.5: halfway through the second loop
if (std::optional<JointTransform> hand = animator.GetJointTransform("Hand_R"))
	sword.GetTransform().SetWorldPosition(hand->Position);

void OnAnimationEvent(std::string_view name) override // Events of the controller's states ("Footstep" at 0.25 s)
{
	if (name == "Footstep")
		Audio::PlayOneShotAt(step, GetTransform().GetWorldPosition());
}
```

- Parameters and states are named as in the controller; commands given while it loads apply once it has loaded.
- Prefer an `AttachToJoint` component (Animator entity and joint name) over copying joint transforms every frame: the
  animation system places the entity after each pose.
- `ApplyRootMotion` on the component moves the entity by the clip's root travel (walk cycles that do not slide); on an
  entity with a `CharacterController` the controller walks that travel, so walls stop it and it keeps to the ground;
  `CullingMode` `WhenVisible` skips the pose of characters no camera sees, neither them nor their shadows, wherever their
  clips take them (time, events and root motion go on; `GetJointTransform` then reads the latest pose). An animator that
  skins no mesh but moves attached parts poses every frame.

### Game flow

`Game::Quit(exitCode)` ends the game (an exported game exits with the code, the editor stops play mode).
`Game::LoadScene(handleOrPath)` replaces the running scene; `Game::ReloadScene()` restarts it (e.g. after a game over).
The next scene and what it uses load in the background while the running scene goes on (its scripts keep
running); once it is in (with what it uses where that fits in the memory budgets, else the new scene streams that in as
it draws it), it replaces the running scene between
frames: every entity of the old scene goes away (scripts get `OnDestroy`), so pass state to the next scene through fields
set in that scene, or rebuild it there. Meanwhile `Game::GetSceneLoadProgress()` returns how far the next scene got, in
[0, 1] (a loading bar on a Text entity), and a negative value while no switch is under way. Asking again for the scene
that is loading changes nothing, so `if (lives <= 0) Game::LoadScene(GameOver);` in `OnUpdate` is fine; asking for
another scene replaces the load. A scene named in a field (the next level) is not loaded with the scene that names it.
In the editor `ReloadScene` restarts what play mode runs, including unsaved edits.

### Streaming: sub-scenes, loading progress, many instances

```cpp
SubScene m_Room;                                         // A member: re-create it after a hot reload if needed

void OnCreate() override
{
	m_Room = Scene::LoadAdditive("Levels/Room2.stscene");  // Loads next to what the scene holds; null if not a scene
	Assets::GetLoadProgress(Tree);                         // Starts loading the prefab and all it uses
}

void OnUpdate(float) override
{
	if (m_Room.IsLoaded() && PlayerLeft())
		m_Room.Unload();                                   // Its entities go at the end of the frame (OnDestroy runs)
	if (!m_Planted && Assets::GetLoadProgress(Tree) >= 1.0f)
	{
		std::vector<glm::mat4> transforms;                 // Translation, rotation and scale per instance
		for (int i = 0; i < 100; i++)
			transforms.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(i * 3.0f, 0.0f, 0.0f)));
		const std::vector<Entity> trees = Scene::InstantiateMany(Tree, transforms); // Roots, in order
		m_Planted = !trees.empty();
	}
}
```

- A sub-scene's entities appear over the frames after its scene asset and everything it uses loaded, at most the scene's
  `MaxEntitiesInstantiatedPerFrame` per frame (5000 by default, scene settings), and behave like any other: their scripts
  get `OnCreate`, physics, audio and rendering pick them up. Destroying one of them while the rest still appear is fine:
  its children that did not exist yet are not created (they would have gone with it). They keep the IDs they have in
  their file, so loading the same
  sub-scene twice fails (an error is logged): unload it first. Keep the `SubScene` (or its `GetID()`) to unload it. A
  hot reload starts members that are not fields over, while the sub-scene stays loaded: keep its ID in an `int32_t` field
  as well (IDs count up from 1 per scene) and get it back in `OnReload` with
  `m_Room = SubScene(static_cast<uint64_t>(m_RoomID));`.
- `Assets::GetLoadProgress(asset)` loads what using an asset needs (a prefab's meshes, materials and textures; not other
  scenes it names) and keeps it loaded like `Assets::RequestLoad` until `Assets::Release(asset)`; at 1, instances draw
  complete in their first frame.
- `Scene::InstantiateMany(prefabOrModel, transforms, parent)` makes all instances in one call (much cheaper than a loop
  of `Instantiate`), or none when the asset is not loaded yet or a transform has a zero scale.

### Gamepads and the cursor

```cpp
if (Input::IsGamepadConnected())                                    // Gamepad 0 unless you pass another (0 to 3)
{
	const glm::vec2 stick = Input::GetGamepadStick(GamepadStick::Left); // +x right, +y up; round dead zone, length <= 1
	controller.Move((right * stick.x + forward * stick.y) * Speed);
	if (Input::IsGamepadButtonPressed(GamepadButton::A))             // Xbox layout: A bottom, B right, X left, Y top
		controller.Jump(5.0f);
	const float throttle = Input::GetGamepadAxis(GamepadAxis::RightTrigger); // 0 released to 1 pressed, dead zone 0.15
}
Input::SetCursorMode(CursorMode::Locked);  // Mouse look: hidden and kept in the window; read Input::GetMouseDelta()
Input::SetCursorMode(CursorMode::Normal);  // Menus (Hidden hides it over the game without locking)
```

`GetGamepadAxis(axis, gamepad, deadZone)` takes a dead zone (pass 0 for the raw value). The cursor mode applies while the
game has the input: in the editor while the game view is focused (Shift+F1 frees the cursor, clicking the game view
takes it again, stopping play gives it back). Tools press buttons and move sticks with `input.gamepadButton` and
`input.gamepadAxis`.

### The window and save files

```cpp
Game::SetWindowMode(WindowMode::Fullscreen);   // Windowed, Fullscreen (exclusive), Borderless
Game::SetResolution(1920, 1080);               // Pixels; false outside 1 to 16384
Game::SetVSync(false);
glm::uvec2 size = Game::GetWindowSize();       // What the game renders at now

Game::WriteSaveFile("Slot 1", "level=3;score=1200");     // Text or std::span<const uint8_t>; replaces the file whole
std::optional<std::string> slot = Game::ReadSaveText("Slot 1"); // Nothing when there is no such save (yet)
std::vector<std::string> slots = Game::ListSaveFiles();  // Sorted names
Game::DeleteSaveFile("Slot 1");
```

Window requests apply after the frame in an exported game; the editor's play mode only records them (`editor.status`
section `game`, a notice over the game view), its own window stays. Saves go to `<user data>/<Game>/Saves` in an exported
game and to `<project>/.strata/Saves` in the editor. Names are 1 to 64 ASCII letters, digits, spaces, `-`, `_` and `.`
(not first or last), without `..` (no paths); the calls read and write the disk before they return, so save at quiet
moments (checkpoints, menus) and keep saves small.

### Text, spawning and other building blocks

- On-screen text (score, lives, messages): an entity with a `Text` component in screen space; change it with
  `entity.SetProperty("Text", "Text", std::string("Score: ") + std::to_string(score))`.
- Spawning: `Scene::Instantiate(prefab, position)` (request the prefab early), `Scene::InstantiateMany` for many at once,
  `Scene::CreateEntity` plus `AddComponent`/`SetProperty` for simple shapes; `entity.Destroy()` removes it at the end of
  the frame.
- `Random` (StrataScript/Gameplay.h): seedable and deterministic on every platform - `Random random(seed);
  random.Range(1, 6); random.Range(-1.0f, 1.0f); random.NextFloat(); random.Chance(0.25f); random.Seed(seed)`.
- `Timer`: `Timer spawn(2.0f, true);` then `for (int32_t i = spawn.Update(deltaTime); i > 0; i--) SpawnEnemy();`
  (`Start`, `Stop`, `IsRunning`, `GetRemaining`, `GetProgress`). Pass scaled delta times so pauses stop it. A duration that is
  not positive leaves the timer stopped (a designer's 0 never floods the game).
- `KeyRepeat`: `KeyRepeat left { Key::Left, 0.2f, 0.05f };` and `if (left.Update(deltaTime)) MoveLeft();` - fires on
  the press, after the delay, then at the interval while held (menus, falling-block games).
- Helpers and other members that are not fields start over on hot reload: keep what matters in fields, re-create the
  rest in `OnReload`.

### Recipes

A platformer player: a dynamic capsule body with `LockRotationX/Y/Z` set (so it does not tip over), friction 0, and a
`Text` entity named "HUD" for the score.

```cpp
class Player : public Script
{
public:
	float Speed = 6.0f;
	float JumpSpeed = 7.0f;
	int32_t Coins = 0;
	AssetHandle CoinSound;

	void OnCreate() override { Assets::RequestLoad(CoinSound); }

	void OnFixedUpdate(float) override
	{
		RigidBody body = GetEntity().GetRigidBody();
		glm::vec3 velocity = body.GetLinearVelocity();
		velocity.x = (Input::IsKeyDown(Key::D) ? Speed : 0.0f) - (Input::IsKeyDown(Key::A) ? Speed : 0.0f);
		body.SetLinearVelocity(velocity);
	}

	void OnUpdate(float) override
	{
		const glm::vec3 feet = GetTransform().GetWorldPosition();
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		const bool grounded = Physics::Raycast(feet, down, 1.1f, Physics::c_AllLayers, GetEntity()).has_value();
		if (grounded && Input::IsKeyPressed(Key::Space))
		{
			RigidBody body = GetEntity().GetRigidBody();
			glm::vec3 velocity = body.GetLinearVelocity();
			velocity.y = JumpSpeed;
			body.SetLinearVelocity(velocity);
		}
		if (feet.y < -20.0f)
			Game::ReloadScene(); // Fell off the level
	}

	void OnTriggerEnter(const Collision& collision) override
	{
		if (collision.Other.GetTag() != "Coin")
			return;
		Entity coin = collision.Other;
		coin.Destroy();
		Coins++;
		Audio::PlayOneShot(CoinSound);
		Scene::FindEntityByName("HUD").SetProperty("Text", "Text", "Coins: " + std::to_string(Coins));
	}
};
```

A falling-block game needs no physics: keep the grid in the script, show cells as entities and move them with
`TransformComponent::SetTranslation`.

```cpp
class Board : public Script
{
public:
	int32_t Seed = 1;
	float DropInterval = 0.5f;

	void OnCreate() override
	{
		m_Random.Seed(static_cast<uint64_t>(Seed));    // The same seed gives the same pieces
		m_Drop.Start(DropInterval, true);
	}

	void OnUpdate(float deltaTime) override
	{
		if (m_Left.Update(deltaTime))
			TryMove(-1, 0);
		if (m_Right.Update(deltaTime))
			TryMove(1, 0);
		for (int32_t drops = m_Drop.Update(deltaTime); drops > 0; drops--)
		{
			if (!TryMove(0, -1))
				LockPieceAndSpawn(m_Random.Range(0, 6)); // Destroys full rows' cell entities, Game::Quit on game over
		}
	}
private:
	bool TryMove(int32_t dx, int32_t dy);         // Definitions omitted: grid bookkeeping
	void LockPieceAndSpawn(int32_t pieceType);

	Random m_Random;
	Timer m_Drop;
	KeyRepeat m_Left { Key::Left, 0.2f, 0.05f };
	KeyRepeat m_Right { Key::Right, 0.2f, 0.05f };
};
```

## Rules

- Use the engine only from callbacks, on the thread that calls you. Never call it from threads you start.
- Do not keep `T*` from `GetScript<T>()` across frames - fetch it when needed (the target may be destroyed or reloaded).
  Store `Entity` values instead; check `IsValid()`.
- Exceptions thrown by a script disable that instance (logged with the message); other scripts keep running.
- A crash (null pointer, division by zero, stack overflow; on Windows also `abort()` or a failed `assert()`, which end
  the process on Linux and macOS) disables the whole module (`ScriptEngine::IsFaulted`; hosts
  such as the editor stop play mode) and the log names the class, callback and entity. Fix it and rebuild; reloading the
  module clears the fault.
- Never block: no sleeps, no busy loops, no synchronous file or network I/O. An infinite loop freezes the editor.

## The workflow in a game project (through editor commands)

An agent drives the editor through its commands (`StrataCLI` / MCP tools, or `StrataEditor --commands script.json`);
every step below is a command, and source files are written with your own file tools.

1. **Project.** `project.create {"directory": "<absolute>", "name": "Tetris"}` creates `Tetris.stproj`, `Assets/` and
   `Scripts/` with a `CMakeLists.txt` (do not change the module's name or output directory) and an example script
   `Scripts/Spinner.cpp`; with `"template": "basic3d"` it also saves a lit start scene (camera, sun, sky, ground). Older projects get the same with `script.init {"example": true}`. `script.status` shows the
   script directory (`sourceDirectory`), the module file the build produces (`projectModule`, and `projectModuleBuilt`)
   and, once a module is loaded, its name (`moduleName`, empty before) and file (`module`).
2. **Write scripts.** `script.create {"class": "Player", "fields": {"Speed": "float", "Target": "entity"}}` writes
   `Scripts/Player.cpp` with the class, `OnCreate`/`OnUpdate` and the fields (`"template": "empty"` for the bare class;
   `"path": "Gameplay/Player.cpp"` for another place; it refuses names the project or the SDK declare); or put
   `.cpp`/`.h` files anywhere under `Scripts/` yourself (one `ST_SCRIPT_CLASS` per class, see above). New files are
   picked up by the next build. `script.list` names every source file with the classes it registers (find the file of
   a class before changing it). `script.open {"file": "Scripts/Player.cpp", "line": 12}` shows a file to the person at
   the editor in their code editor.
3. **Build.** `script.build` (waits by default) configures CMake on the first run (a few seconds) and builds with the
   editor's configuration and a C++ toolchain (`script.status` -> `toolchain`) into `.strata/Scripts/Bin`, then loads
   the module. In a source checkout that is the engine's toolchain; a downloaded Strata finds one on the machine
   (Windows: Visual Studio 2022 17.10 or later or 2026 with the C++ workload, or its Build Tools, and CMake; Linux: g++
   or clang++, Ninja or Make, and CMake 3.25 or later; macOS: the Xcode Command Line Tools and CMake), and without one
   the build fails naming what to install. On failure the command fails with the first errors
   (`File(Line,Column): error C2065: ...`, undefined references or symbols of the linker, CMake
   errors), followed by the end of the build log when only summaries such as `ld returned 1 exit status` were found;
   `script.status` -> `build.last` lists every diagnostic (`file`, `line`, `column`, `severity`, `code`, `message`)
   and the end of the build log (`log`). Fix and build again. One build runs at a time (a second request fails while
   one runs; `script.build {"wait": false}` returns at once and `script.status` -> `build.running` tells when it is
   done). The build output also streams into the editor log (`log.read`). On Windows (Visual Studio generator) a project
   whose folder path is too long for MSBuild (more than about 133 characters) builds in a per-user folder instead
   (`script.status` -> `build.last.buildDirectory`); only when even that is too deep does the build fail with "The
   project's path is too long". **Play builds first:** `play.start` builds the scripts when their sources changed since
   the last build (or the module is not loaded while scenes use scripts) and answers once the game plays (with
   `built: {build, seconds, reason}`), or fails with the build's errors; `script.status` -> `playBuildsFirst` says
   whether it will. `play.start {"build": false}` plays what is loaded.
4. **Inspect.** `script.status` -> `classes` lists every class with its fields (`name`, `type`, `default`) and
   implemented callbacks. `script.get {"entity": "<id>"}` shows the field values of an entity's scripts: what the scene
   stores, or while playing what the live instances hold now.
5. **Attach.** `script.add {"entity": "<id>", "class": "Player", "fields": {"Speed": 4.5, "Target": "<entity id>",
   "Bullet": "Prefabs/Bullet.stprefab"}}`; change one field with `script.setField {"entity", "class", "field", "value"}`
   (value `null` resets to the class default); detach with `script.remove {"entity", "class"}`. These are undoable,
   validated against the loaded module (class names, field names and types), and need a built module. Field values:
   bool, integer, number, arrays for vec2/3/4, `[x, y, z, w]` or Euler degrees `[pitch, yaw, roll]` for quaternions,
   text, an entity ID, an asset handle or path. Save the scene (`scene.save`) to keep them.
6. **Play and observe.** `play.start`, `editor.wait {"frames": 60}`, then read state with `component.get`,
   `entity.find`, `scene.hierarchy`; script `Log::Info(...)` output is in `log.read`. Play it like a person with
   simulated input: `input.key {"key": "Left"}` (a tap), `{"key": "Space", "frames": 20}` (held), `input.mouseMove`,
   `input.mouseButton`. While playing, `script.setField` changes the live instance too (and the running copy only:
   `play.stop` discards it).
7. **Iterate with hot reload.** Edit the sources and `script.build` again while the scene plays: the module is reloaded
   in place, every instance keeps its field values (same name and type) and gets `OnReload` instead of `OnCreate`.
   A build that fails keeps the running module. `script.reload` reloads the module file without building.
8. **Crashes.** A crash (null pointer, division by zero, stack overflow) stops play mode; the log and `script.status` ->
   `fault`/`lastFault` name the module, class, callback, entity and the crash, and, once `symbolized` is true (a moment
   later), `location` (`file` and `line` on Windows from the module's PDB, the `function` elsewhere) and the `stack`
   (innermost first; frames with `inlined: true` are calls the compiler inlined at the same address as the frames after
   them, so `location` is the line that ran even in an optimized build). `play.start` fails until the module is
   rebuilt (`script.build`; after a crash a successful build loads the module again even when nothing had to be
   compiled, e.g. when you fixed a field value with `script.setField`) or reloaded (`script.reload`). An exception
   thrown by a script only disables that instance (the message is logged).
9. **Ship.** `project.setStartScene`, then `project.export {"directory": "<absolute, outside the project>"}` writes the
   game: the runtime executable, the asset pack, the script module (the one the editor runs) and the `.stgame`
   manifest naming it. The exported game loads the module before its start scene; run it headless with
   `<Game> --headless --frames 600` to check it (exit code 2 means the scripts crashed). Export refuses while a build
   runs, when the module file changed since the editor loaded it (e.g. a build whose module did not load: fix it, or
   `script.reload`), and when scenes use scripts but no module is loaded. A pruned export (`"prune": true`) ships only
   the assets scenes reference: assets scripts load by path need `project.setExportSettings {"alwaysInclude": [...]}`.

The UI does the same with Scripts > New Script... (which starts a build, so that the new class can be attached once it
finished), Scripts > Build Scripts (Ctrl+B), the toolbar's Play (which builds first when the sources changed since the last
build) and Build Scripts buttons, the Problems panel (build errors and crashes; a double click opens the line in the
external editor, Scripts > External Editor..., which lists the editors found on the machine), the Content Browser's
Scripts root (the script files with their classes, below the asset folders; what `script.list` returns) and the
inspector's Script section (add a class from the module, edit fields with their default shown and a reset button). People's guide:
`Docs/Manual/Scripting.md` in the Strata folder (a download or a source checkout).

## Building outside the editor

A game's `Scripts/CMakeLists.txt` uses the StrataScriptCore package (`STRATA_ENGINE_DIR` names the Strata folder that
holds `StrataScriptCore/`, a downloaded release or a source checkout; `script.build` sets it):

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGameScripts CXX)
set(STRATA_ENGINE_DIR "" CACHE PATH "The Strata folder, a source checkout or an installation")
find_package(StrataScriptCore CONFIG REQUIRED PATHS "${STRATA_ENGINE_DIR}/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module(MyGameScripts SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
```

Build it with the toolchain and configuration `script.status` -> `toolchain` names once its `found` is true (a
downloaded Strata finds its toolchain in the first `script.build`; `engineDirectory` is the value for
`STRATA_ENGINE_DIR`). The editor watches the module it runs and reloads it when the file changes (an IDE build works
too). Rebuilding while the game runs is safe with hot reload enabled
(`ScriptEngine::SetHotReloadEnabled`, on in the editor): the engine then runs a private copy of the module and reloads
the new build once the file is completely written. A build that fails to load (or crashes while loading) leaves the
running version in place. Field values survive the reload when the field keeps its name and type. Without hot reload
(shipped games) the module runs from its file. In a source checkout, the engine's test modules use
`strata_add_script_module(<Target> SOURCE_DIR <dir>)` directly and load with `script.load {"path": "<module file>"}`.

Prefer static libraries for third-party code in scripts. Shared libraries the module links against must sit next to the
module file (Windows) or be reachable through its RUNPATH (Linux, macOS; `$ORIGIN` does not work under hot reload,
because the module then runs from a private copy; Docs/DeveloperGuide.md in a source checkout has the details).

## Testing scripts

A game project tests its scripts with command scripts (`StrataEditor --commands`, see the `strata-make-a-game` skill,
"Leave a regression test"). The rest of this section is for a source checkout of the engine.
Engine-side tests load a module, play a scene and inspect fields: see `StrataTests/src/Scripting/` (helpers in
`ScriptTestUtils.h`: `ScopedScriptEngine`, `AddScriptEntry`, `AddFieldOverride`, `GetField<T>`, `RunFrames`).
Editor-side tests drive the script commands on a new project with in-tree modules (`script.load`):
`StrataTests/src/Editor/EditorScriptTests.cpp` (`ScriptHarness`). Tests that compile a project's scripts for real
(`script.build`, hot reload, diagnostics) are `package` tests: doctest suites named `Package*`
(`ScriptBuildPackageTests.cpp`) and `StrataTests/Editor/ScriptsEndToEnd.cmake` (the real editor and exported game).

## Extending the script API: the feature test

Source checkout only (changing the engine's script API): `StrataTests/FeatureTest/Scripts/` is a script module that uses
the **whole** SDK in the feature scene
(`StrataTests/FeatureTest/Assets/Scenes/Feature.stscene`), and `StrataTests.FeatureTest` fails when any public SDK
function or macro is unused there, or any host function of `StrataScriptHostAPI` is never called during the run.
When you add or change script API:

1. Use it in the feature script whose topic fits (`EntityFeatures`, `ComponentFeatures`, `TransformFeatures`,
   `FieldFeatures`, `LifecycleFeatures`, `TimeFeatures`, `InputFeatures`, `SpawnFeatures`, `ScriptFeatures`,
   `PhysicsFeatures`, `PhysicsApiFeatures`, `CollisionFeatures`, `CharacterFeatures`, `AudioFeatures`,
   `AnimationFeatures`, `GameFeatures`, `LogFeatures`), or
   add a class deriving from `FeatureTest::FeatureScript` on a new entity of the scene. Verify the effect with
   `Expect(condition, "what is checked")` and set `Completed = true` when the scenario ended; journal `OnCreate` with
   `Journal(*this, "<Class>", "OnCreate")`. Keep state that spans frames in fields: the runner hot reloads the module
   after frame 100. The coverage check counts a call only on the class it belongs to:
   call static functions as `Class::Name(...)` and member functions on a receiver of declared type (`Entity target =
   ...; target.Name(...)`), see `StrataTests/src/FeatureTest/SDKReader.h`.
2. A new host function is also appended to `ST_SCRIPT_HOST_FUNCTIONS` in `ScriptHostAPI.cpp` (the build fails until
   it is). A new field type, callback or asset type is picked up automatically by the coverage checks: declare a field
   of the new type in a feature script, register it with `ST_SCRIPT_FIELD` and override it in the scene; implement the
   callback in a feature script that runs and journal its first call (`Journal(*this, "<Class>", "<Callback>")`, as
   `LifecycleFeatures` does); add such an asset.
3. Input the scripts need is simulated by `PlayFeatureScene` (`StrataTests/src/FeatureTest/FeatureTestUtils.cpp`);
   messages logged on purpose belong in `c_ExpectedLogMessages` there (any other warning or error fails the run).
4. Run `StrataTests.exe --test-suite=FeatureTest,Editor.FeatureTest` (or `ctest -L feature`). A failure names the
   class, entity and check, the unused SDK function or the uncalled host function.
