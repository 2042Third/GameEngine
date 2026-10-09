---
name: strata-scripting
description: How to write, build and debug Strata game scripts (C++ script classes in a script module built against the StrataScriptCore SDK). Use when creating gameplay logic, adding script classes or fields, spawning prefabs from scripts, or diagnosing script crashes and hot reload.
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
- `Assets`: `Find(path)`, `IsLoaded`, `RequestLoad`. Instantiating an asset that is not loaded yet stalls once (loaded
  synchronously, with a warning) - request it early.
- `Input`: `IsKeyDown/Pressed/Released(Key::W)`, `IsMouseButtonDown/Pressed/Released(Mouse::ButtonLeft)`,
  `GetMousePosition` (viewport pixels), `GetMouseDelta`, `GetScrollDelta`.
- `Time`: `GetDeltaTime`, `GetFixedDeltaTime`, `GetElapsedTime`, `GetFrameIndex`, `Get/SetTimeScale`.
- `Log::Info/Warn/Error/Trace(args...)` - arguments are concatenated (`Log::Info("Health ", health)`).

Every call on a missing entity, unknown component or wrong value type fails harmlessly (`false`, empty `optional`,
null entity) and the engine logs a warning naming the calling script.

## Rules

- Use the engine only from callbacks, on the thread that calls you. Never call it from threads you start.
- Do not keep `T*` from `GetScript<T>()` across frames - fetch it when needed (the target may be destroyed or reloaded).
  Store `Entity` values instead; check `IsValid()`.
- Exceptions thrown by a script disable that instance (logged with the message); other scripts keep running.
- A crash (null pointer, division by zero, stack overflow) disables the whole module (`ScriptEngine::IsFaulted`; hosts
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

Rebuilding while the game runs is safe: the engine runs a private copy of the module and, with hot reload enabled
(`ScriptEngine::SetHotReloadEnabled`), reloads the new build once the file is completely written. A build that fails to load (or crashes while loading) leaves the running version in
place. Field values survive the reload when the field keeps its name and type.

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
   `PhysicsFeatures`, `LogFeatures`), or add a class deriving from `FeatureTest::FeatureScript` on a new entity of the
   scene. Verify the effect with `Expect(condition, "what is checked")` and set `Completed = true` when the scenario
   ended; journal `OnCreate` with `Journal(*this, "<Class>", "OnCreate")`. Keep state that spans frames in fields: the
   runner hot reloads the module after frame 100. The coverage check counts a call only on the class it belongs to:
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
