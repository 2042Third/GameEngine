---
name: strata-scripting
description: How to write, build, attach, debug and ship Strata game scripts (C++ script classes in a game project's script module, built against the StrataScriptCore SDK) through the editor's script.* commands. Use when creating gameplay logic for a project, adding script classes or fields, spawning prefabs from scripts, iterating with hot reload, diagnosing script build errors or crashes, or exporting a scripted game.
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

## The workflow in a game project (through editor commands)

An agent drives the editor through its commands (`StrataCLI` / MCP tools, or `StrataEditor --commands script.json`);
every step below is a command, and source files are written with your own file tools.

1. **Project.** `project.create {"directory": "<absolute>", "name": "Tetris"}` creates `Tetris.stproj`, `Assets/` and
   `Scripts/` with a `CMakeLists.txt` (do not change the module's name or output directory) and an example script
   `Scripts/Spinner.cpp`. Older projects get the same with `script.init {"example": true}`. `script.status` shows the
   script directory (`sourceDirectory`), the module file the build produces (`projectModule`, and `projectModuleBuilt`)
   and, once a module is loaded, its name (`moduleName`, empty before) and file (`module`).
2. **Write scripts.** Put `.cpp`/`.h` files anywhere under `Scripts/` (one `ST_SCRIPT_CLASS` per class, see above). New
   files are picked up by the next build.
3. **Build.** `script.build` (waits by default) configures CMake on the first run (a few seconds) and builds with the
   engine's compiler and configuration into `.strata/Scripts/Bin`, then loads the module. On failure the command fails
   with the first errors (`File(Line,Column): error C2065: ...`, undefined references or symbols of the linker, CMake
   errors), followed by the end of the build log when only summaries such as `ld returned 1 exit status` were found;
   `script.status` -> `build.last` lists every diagnostic (`file`, `line`, `column`, `severity`, `code`, `message`)
   and the end of the build log (`log`). Fix and build again. One build runs at a time (a second request fails while
   one runs; `script.build {"wait": false}` returns at once and `script.status` -> `build.running` tells when it is
   done). The build output also streams into the editor log (`log.read`).
4. **Inspect.** `script.status` -> `classes` lists every class with its fields (`name`, `type`, `default`) and
   implemented callbacks.
5. **Attach.** `script.add {"entity": "<id>", "class": "Player", "fields": {"Speed": 4.5, "Target": "<entity id>",
   "Bullet": "Prefabs/Bullet.stprefab"}}`; change one field with `script.setField {"entity", "class", "field", "value"}`
   (value `null` resets to the class default); detach with `script.remove {"entity", "class"}`. These are undoable,
   validated against the loaded module (class names, field names and types), and need a built module. Field values:
   bool, integer, number, arrays for vec2/3/4, `[x, y, z, w]` or Euler degrees `[pitch, yaw, roll]` for quaternions,
   text, an entity ID, an asset handle or path. Save the scene (`scene.save`) to keep them.
6. **Play and observe.** `play.start`, `editor.wait {"frames": 60}`, then read state with `component.get`,
   `entity.find`, `scene.hierarchy`; script `Log::Info(...)` output is in `log.read`. While playing, `script.setField`
   changes the live instance too (and the running copy only: `play.stop` discards it).
7. **Iterate with hot reload.** Edit the sources and `script.build` again while the scene plays: the module is reloaded
   in place, every instance keeps its field values (same name and type) and gets `OnReload` instead of `OnCreate`.
   A build that fails keeps the running module. `script.reload` reloads the module file without building.
8. **Crashes.** A crash (null pointer, division by zero, stack overflow) stops play mode; the log and `script.status` ->
   `fault`/`lastFault` name the module, class, callback, entity and the crash. `play.start` fails until the module is
   rebuilt (`script.build`; after a crash a successful build loads the module again even when nothing had to be
   compiled, e.g. when you fixed a field value with `script.setField`) or reloaded (`script.reload`). An exception
   thrown by a script only disables that instance (the message is logged).
9. **Ship.** `project.setStartScene`, then `project.export {"directory": "<absolute, outside the project>"}` writes the
   game: the runtime executable, the asset pack, the script module (the one the editor runs) and the `.stgame`
   manifest naming it. The exported game loads the module before its start scene; run it headless with
   `<Game> --headless --frames 600` to check it (exit code 2 means the scripts crashed). Export refuses while a build
   runs, when the module file changed since the editor loaded it (e.g. a build whose module did not load: fix it, or
   `script.reload`), and when scenes use scripts but no module is loaded.

The UI does the same with Scripts > Build Scripts (Ctrl+B), the toolbar's Build Scripts button and the inspector's
Script section (add a class from the module, edit fields with their default shown and a reset button).

## Building outside the editor

A game's `Scripts/CMakeLists.txt` uses the StrataScriptCore package (`STRATA_ENGINE_DIR` names the engine checkout;
`script.build` sets it):

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGameScripts CXX)
set(STRATA_ENGINE_DIR "" CACHE PATH "The Strata engine checkout")
find_package(StrataScriptCore CONFIG REQUIRED PATHS "${STRATA_ENGINE_DIR}/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module(MyGameScripts SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
```

Build it with the engine's compiler and configuration. The editor watches the module it runs and reloads it when the
file changes (an IDE build works too); the engine runs a private copy, so rebuilding while the game runs is safe.
Inside this repository, test modules use `strata_add_script_module(<Target> SOURCE_DIR <dir>)` directly and load with
`script.load {"path": "<module file>"}`.

## Testing scripts

Engine-side tests load a module, play a scene and inspect fields: see `StrataTests/src/Scripting/` (helpers in
`ScriptTestUtils.h`: `ScopedScriptEngine`, `AddScriptEntry`, `AddFieldOverride`, `GetField<T>`, `RunFrames`).
Editor-side tests drive the script commands on a new project with in-tree modules (`script.load`):
`StrataTests/src/Editor/EditorScriptTests.cpp` (`ScriptHarness`). Tests that compile a project's scripts for real
(`script.build`, hot reload, diagnostics) are `package` tests: doctest suites named `Package*`
(`ScriptBuildPackageTests.cpp`) and `StrataTests/Editor/ScriptsEndToEnd.cmake` (the real editor and exported game).

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
   runner hot reloads the module after frame 100.
2. A new host function is also appended to `ST_SCRIPT_HOST_FUNCTIONS` in `ScriptHostAPI.cpp` (the build fails until
   it is). A new field type, callback or asset type is picked up automatically by the coverage checks: override such a
   field in the scene, implement the callback, add such an asset.
3. Input the scripts need is simulated by `PlayFeatureScene` (`StrataTests/src/FeatureTest/FeatureTestUtils.cpp`);
   messages logged on purpose belong in `c_ExpectedLogMessages` there (any other warning or error fails the run).
4. Run `StrataTests.exe --test-suite=FeatureTest,Editor.FeatureTest` (or `ctest -L feature`). A failure names the
   class, entity and check, the unused SDK function or the uncalled host function.
