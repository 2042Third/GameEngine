---
name: strata-scripting
description: How to write, build and debug Strata game scripts (C++ script classes in a script module built against the StrataScriptCore SDK) - the lifecycle, the whole gameplay API (entities, components, transforms, input, time, physics bodies, raycasts and contacts, audio, quitting and loading scenes, random numbers, timers, key repeat) and how to test it. Use when creating gameplay logic or whole games, adding script classes or fields, spawning prefabs from scripts, or diagnosing script crashes and hot reload.
---

# Writing Strata game scripts

Game logic lives in C++ script classes compiled into a **script module** (a shared library). Scripts see only the
SDK in `StrataScriptCore/Include/StrataScript/` - never engine headers. Include one header:

```cpp
#include "StrataScript/StrataScript.h"
```

The engine-side rules (ABI, adding host functions) are in AGENTS.md, section "Scripting".

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
| `OnCreate()` | Once, before the first update, after every script that exists at that moment was constructed. |
| `OnUpdate(dt)` | Every frame. Order: entity hierarchy (parents first), then the order of scripts on the entity. |
| `OnFixedUpdate(dt)` | Zero or more times per frame at the fixed timestep (gameplay physics). |
| `OnLateUpdate(dt)` | Every frame after the fixed updates. |
| `OnDestroy()` | Entity destroyed (still valid here), script removed, or play stopped (descendants first). |
| `OnReload()` | After a hot reload, instead of `OnCreate`. Re-acquire cached pointers and non-field state. |
| `OnCollisionEnter/Exit(collision)` | The entity's physics body starts/stops touching another body (after a fixed step). |
| `OnTriggerEnter/Exit(collision)` | The same for contacts where either body is a trigger. |

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
- `Assets`: `Find(path)`, `IsLoaded`, `RequestLoad`. `Instantiate` never waits: for an asset that is not loaded yet it
  starts the load and returns a null entity - request assets early (in `OnCreate`) and spawn once `IsLoaded`.
- `Input`: `IsKeyDown/Pressed/Released(Key::W)`, `IsMouseButtonDown/Pressed/Released(Mouse::ButtonLeft)`,
  `GetMousePosition` (viewport pixels), `GetMouseDelta`, `GetScrollDelta`. In the editor, scripts receive input only
  while the viewport is the focused game view (playing through the scene's primary camera); positions are relative to
  the image.
- `Time`: `GetDeltaTime`, `GetFixedDeltaTime`, `GetElapsedTime`, `GetFrameIndex`, `Get/SetTimeScale`.
- `Log::Info/Warn/Error/Trace(args...)` - arguments are concatenated (`Log::Info("Health ", health)`).
- `RigidBody` (`GetEntity().GetRigidBody()`), `Physics` (raycasts, overlaps), `Collision` (contact callbacks),
  `AudioSource` (`GetEntity().GetAudioSource()`), `Audio` (one-shots, master volume), `Game` (quit, load scenes),
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
- Moving a body by writing its transform works too (kinematic platforms, editor-like placement) but takes effect at the
  next fixed step; `Teleport` is immediate. Results always name the entity that owns the body (the RigidBody entity,
  also for colliders on its children).

### Contacts

Override the contact callbacks on the script of an entity that owns a body:

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

They run after the fixed step that found the change, for the scripts of both entities. `collision.Other` may name an
entity that is already gone in an Exit caused by its destruction: check `IsValid()` before using it.

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

### Game flow

`Game::Quit(exitCode)` ends the game (an exported game exits with the code, the editor stops play mode).
`Game::LoadScene(handleOrPath)` replaces the running scene; `Game::ReloadScene()` restarts it (e.g. after a game over).
Both happen after the current frame; every entity of the old scene goes away (scripts get `OnDestroy`), so pass state to
the next scene through fields set in that scene, or rebuild it there.

### Text, spawning and other building blocks

- On-screen text (score, lives, messages): an entity with a `Text` component in screen space; change it with
  `entity.SetProperty("Text", "Text", std::string("Score: ") + std::to_string(score))`.
- Spawning: `Scene::Instantiate(prefab, position)` (request the prefab early), `Scene::CreateEntity` plus
  `AddComponent`/`SetProperty` for simple shapes; `entity.Destroy()` removes it at the end of the frame.
- `Random` (StrataScript/Gameplay.h): seedable and deterministic on every platform - `Random random(seed);
  random.Range(1, 6); random.Range(-1.0f, 1.0f); random.NextFloat(); random.Chance(0.25f); random.Seed(seed)`.
- `Timer`: `Timer spawn(2.0f, true);` then `for (int32_t i = spawn.Update(deltaTime); i > 0; i--) SpawnEnemy();`
  (`Start`, `Stop`, `IsRunning`, `GetRemaining`, `GetProgress`). Pass scaled delta times so pauses stop it.
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
- A crash (null pointer, division by zero, stack overflow, `abort()` or a failed `assert()`) disables the whole module
  (`ScriptEngine::IsFaulted`; hosts
  such as the editor stop play mode) and the log names the class, callback and entity. Fix it and rebuild; reloading the
  module clears the fault.
- Never block: no sleeps, no busy loops, no synchronous file or network I/O. An infinite loop freezes the editor.

## Building

Inside this repository (tests): `strata_add_script_module(<Target> SOURCE_DIR <dir>)` in CMake.

A game project builds its `Scripts/` folder with this `CMakeLists.txt` (same compiler and configuration as the engine):

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGameScripts CXX)
find_package(StrataScriptCore CONFIG REQUIRED PATHS "<engine>/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module(MyGameScripts SOURCE_DIR Scripts)
```

Rebuilding while the game runs is safe with hot reload enabled (`ScriptEngine::SetHotReloadEnabled`): the engine then
runs a private copy of the module and reloads the new build once the file is completely written. A build that fails to
load (or crashes while loading) leaves the running version in place. Field values survive the reload when the field
keeps its name and type. Without hot reload (shipped games) the module runs from its file.

Prefer static libraries for third-party code in scripts. Shared libraries the module links against must sit next to the
module file (Windows) or be reachable through its RUNPATH (Linux, macOS; `$ORIGIN` does not work under hot reload,
because the module then runs from a private copy - see AGENTS.md).

## Testing scripts

Engine-side tests load a module, play a scene and inspect fields: see `StrataTests/src/Scripting/` (helpers in
`ScriptTestUtils.h`: `ScopedScriptEngine`, `AddScriptEntry`, `AddFieldOverride`, `GetField<T>`, `RunFrames`).

## Extending the script API: the feature test

`StrataTests/FeatureTest/Scripts/` is a script module that uses the **whole** SDK in the feature scene
(`StrataTests/FeatureTest/Assets/Scenes/Feature.stscene`), and `StrataTests.FeatureTest` fails when any public SDK
function or macro is unused there, or any host function of `StrataScriptHostAPI` is never called during the run.
When you add or change script API:

1. Use it in the feature script whose topic fits (`EntityFeatures`, `ComponentFeatures`, `TransformFeatures`,
   `FieldFeatures`, `LifecycleFeatures`, `TimeFeatures`, `InputFeatures`, `SpawnFeatures`, `ScriptFeatures`,
   `PhysicsFeatures`, `PhysicsApiFeatures`, `CollisionFeatures`, `AudioFeatures`, `GameFeatures`, `LogFeatures`), or
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
