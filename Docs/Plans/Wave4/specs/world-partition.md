# Workstream `world-partition`: Layered world partition: data layers, grid cells, streaming sources, physics at world scale (wave 4, size XL)

Read `common.md` first. This is the signature feature of the Roadmap's pillar "Layered worlds". It merges **third**, after
`engine-modules` and `world-io`, and its host-function block is the first of the wave (common.md section 7).

Facts below were checked against main 685aef7 (`.worktrees/verify`); locate code by symbol, line numbers move.

## Why

- **Streamed bodies lose collision (scale audit, critical).** `scratchpad\wave4\audit\scale\runcells.log`: a main scene plus
  four 25,000-body cells loaded with `scene.loadAdditive`. After the third cell the log says "Physics: cannot create a body
  for 'Block' and 1359 more entities: the world's limit of 65536 bodies is reached; they get one when bodies are removed
  (the scene's next play start sizes the world for them)". 9,465, then 34,465 entities had no body. With bodies waiting,
  play frames went from a median of 2.3 ms to 13.6 ms (p95 51 ms; `Scene.Physics` up to 54 ms).
  - Cause: `PhysicsWorld::ResolveCapacities` sizes automatic capacities from the bodies the scene has at play start
    (`c_MinAutomaticBodies` = 65,536 in `Physics/PhysicsWorld.cpp`). A body that finds the world full gets
    `BuildFailure::BodyLimit`; `NeedsPolling` keeps every such record in `PolledBodies`, and `SyncBody` retries each one
    every step (`case BuildFailure::BodyLimit: retry = GetUsedBodySlots(data) < data.Settings.MaxBodies`).
  - The overflow is remembered per scene lineage in a process-wide map (`PhysicsSystem::GetRememberedCapacityDemand`) and
    never saved, so exported games never grow. No command reports bodies or capacity (`PhysicsStats` reaches nothing but
    tests).
- **Nothing partitions a world (scale audit, critical "What world partition must deliver").**
  - No code is named StreamingSource, WorldPartition, DataLayer or HLOD.
  - Sub-scenes load only into a running game: `scene.loadAdditive` fails outside play mode (`EditorSceneCommands.cpp`,
    "Sub-scenes load into a running game").
  - Play mode copies the whole edited scene (`EditorContext::StartRuntime` -> `Scene::Copy`): 1.44 GB editing and 2.88 GB
    playing the 1M-entity world; its play start was a 599 ms frame (`Render.Extract` 360 ms, `Scene.Physics` 157 ms).
  - A whole-world view costs 14.7-16.2 ms of `Render.Extract`.
  - One registry holds at most `Scene::c_MaxEntities` (1,048,575) entities: a 1.1M-entity scene does not open.
- **Large cells hitch (scale audit, major).**
  - 25,000-entity cells load in 0.52-0.70 s with frames up to 12-17 ms, and unload in one 51-56 ms frame (`Scene.Physics`
    50.9 ms): `Scene::UnloadSubScene` -> `DestroySubSceneEntities` -> one `DestroyEntities` call.
  - `rt250k_window.csv`: `Scene.Load` took 4-15 ms in each of the 16 frames before a 250k scene swap (above
    `RenderScene::c_PrepareFrameBudget`'s 4 ms), and the swap frame spent 33.6 ms in `Scene.Physics` creating its bodies.
  - Small cells already stream well: 10x10 cells of 2,502 entities (`rungrid.log`) load in about 20 ms over about 30
    frames, with frames of at most 7.7-8.5 ms.
- **The play start's physics (scale audit, major).** `Scene.Physics` took 147-157 ms creating 100,000 static bodies in one
  frame: `PhysicsWorld`'s constructor builds every body on the main thread, one by one.
- **Invisible (scale audit, minor).** No `physics.stats`, no Physics section or cell map in the Streaming panel, no
  status-bar problem while bodies wait. `StrataRuntime` with a window spins at about 900 frames per second while only the
  empty frame draws during the start scene's load (`rt1M_window.csv`: 900 frames in about 1 s).
- **Roadmap open items:** the physics overflow "is remembered per scene for the process, not saved with the scene";
  "automatic capacities reserve Jolt's buffers generously ... a world at the largest capacities still reserves about
  940 MiB at play start".

## Design in one paragraph

A **world** is an ordinary scene, its *persistent scene*, whose `WorldComponent` names a **World asset** (`.stworld`). The
World asset defines named **data layers**, each with its own grid **cell size** and **streaming distance**, and lists
the world's **cells**: one scene document per (layer, cell coordinate), stored in a folder beside the world. Cells are
additive sub-scenes (`Scene::LoadSubScene`). One streaming core, `WorldStreamer`, decides which cells must be loaded
around **streaming sources** and loads and unloads them within per-frame budgets. In games and play mode it is owned by
the `WorldStreamingSystem` scene system; in edit mode, where no scene system exists, it is owned by the editor's
`EditorWorld` around the editor camera. Cell membership is the `SubSceneComponent` of a cell's top-level entities, so a
loaded cell is a sub-scene of the persistent scene. Physics is sized from the cook, grows by migration when it must, and
never leaves a body without collision. Exported games start the persistent scene like any start scene, so the manifest
does not change.

## Scope

### 1) Physics at world scale (`Physics/*`, first: closes the critical finding for today's sub-scenes too)

- **No polling of waiting bodies.**
  - A record that cannot get a body slot gets `BuildFailure::WaitingForSlot` (renaming `BodyLimit`) and goes into a FIFO
    `SlotWaiters` list in `PhysicsWorldData`, in the order it failed. It leaves `PolledBodies`: `NeedsPolling` no longer
    lists it.
  - `FlushBodyDestructions` pops at most as many waiters as slots it freed into `OwnerRefreshes`. Growth (below) pops
    all of them. A frame with N waiting bodies costs nothing per waiting body.
  - `ReportBodyLimit` keeps its one aggregated error, now saying whether the world will grow or is at its limit.
- **Growth by migration** (`PhysicsWorld::Grow`, internal `GrowWorld(PhysicsWorldData&, uint32_t bodies)`).
  - Trigger: a round of changes leaves waiters, or `IPhysicsCapacity::Reserve` (below) asks for more than the capacity.
    Growth runs **between steps**, at the start of `Simulate` before characters and Jolt step, so bodies that waited
    during the frame's updates never miss a step. It also runs in a preparation slice (below). It never runs inside a
    query, a body call or a collision dispatch, so nothing observes a half-built system. A paused scene grows at its
    next step.
  - New capacity: `NextPowerOfTwo(max(2 * MaxBodies, used + waiting + reserved))`, clamped to
    `PhysicsCapacitySettings::c_MaxBodiesLimit` (2^21). Automatic body-pair and contact capacities scale with it
    (`ResolveCapacities`' ratios). Explicit capacities from `SceneSettings::PhysicsCapacity` are exceeded only with a warning
    naming the setting: correctness comes before the setting.
  - Steps:
    1. `Init` a new `JPH::PhysicsSystem` and a new temp allocator.
    2. Recreate every body from `Body::GetBodyCreationSettings()` (shapes are shared references, not rebuilt):
       `BodyInterface::CreateBodyWithoutID` on the job threads in fixed chunks of `c_MigrationChunk` (4,096) bodies, then
       `AssignBodyID(body, sameID)` on the main thread in body-index order. Every `JPH::BodyID`, index and sequence number
       stays, so `BodySlots`, `BodyRecord::BodyID`, contact keys and anything that saved a body ID stay valid. The two
       broad-phase anchors are recreated at indices 0 and 1.
    3. Add bodies that were in the old broad phase in the same chunks (`AddBodiesPrepare` per chunk on the job threads,
       `AddBodiesFinalize` in chunk order on the main thread): static bodies without activation, others as they were.
    4. Copy the dynamic state with `JPH::PhysicsSystem::SaveState(recorder, EStateRecorderState::Bodies |
       EStateRecorderState::Contacts)` into `RestoreState` of the new system: positions, velocities, sleep timers and the
       contact cache (warm starting), so a resting stack keeps resting and no contact begins again.
    5. Swap the systems; re-set the contact and activation listeners. `BodySlots` and `MovedBodyBits` grow.
       `ContactCollector::Initialize(newMax)` keeps each body's interest. `CharacterControllerSet::Rebind(JPH::PhysicsSystem&)`
       (new) recreates every `CharacterVirtual` in the new system with its position, velocity and ground state; its
       presence bodies migrated with their IDs.
    6. Hand the old system and allocator to a `JobPriority::Background` job holding a `PhysicsRuntimeReference`, so
       destroying 100,000 bodies costs the frame nothing.
    7. Pop every waiter into `OwnerRefreshes`; they get bodies in the same round.
  - It is deterministic: chunking is by body index with a fixed chunk size, independent of the thread count.
  - Reporting:
    - `PhysicsStats` appends `GrowthCount`, `LastGrowthMs`, `LastGrowthFrom` and `LastGrowthTo`.
    - One info log: "Physics: the world of scene 'X' grew from 65,536 to 131,072 bodies in 14.2 ms; cook the world
      (world.cook) or raise its physics capacity to start at that size".
    - `Demand` is recorded as today.
  - A world at the limit keeps waiters in `SlotWaiters`, with the error and the status-bar problem (area 7).
- **Parallel body creation** (`PhysicsWorld::ApplyChanges`, the constructor, and every round with at least
  `c_ParallelBuildThreshold` (256) new owners). Three phases:
  1. Main thread: bring world transforms current (`Scene::UpdateWorldTransforms`), then collect the owners in record
     order and resolve their colliders' entities.
  2. Job threads, in fixed chunks: decompose transforms (`Scene::FindCurrentWorldTransform`, which workers may call),
     build parts and shapes, and call `CreateBodyWithoutID`. Shared primitive shapes come from `PrimitiveShapes` behind a
     `std::shared_mutex` (read on hit, exclusive insert). Mesh shapes stay on the main thread's path: they wait for cooked
     data anyway.
  3. Main thread: `AssignBodyID` in record order, fill records and `BodySlots`, queue the batch adds (`AddBodiesPrepare`
     per chunk on the jobs, `AddBodiesFinalize` in order).
  - Results equal the serial path's bit for bit: body IDs, broad-phase batches and the simulation do not depend on the
    thread count.
  - The serial path stays for small rounds.
- **Prepared physics before the first step.** A loaded scene's bodies are built while its load is `Preparing`, so the
  swap frame builds none (area 3, "Preparation hooks").
  - `PhysicsSystem::PrepareLoadedScene(Scene&, SceneWorkBudget&)` creates a `PhysicsWorld` in the scene's registry
    context (`PreparedPhysicsWorld`, like `RenderScene` lives in the context). The world is constructed with deferred
    building (`PhysicsWorld::Create(scene, settings, demand, PhysicsBuildMode::Sliced)`) and built in parallel slices of
    `c_PrepareBodiesPerSlice` (8,192) owners within the budget.
  - `PhysicsSystem::OnRuntimeStart` adopts it when present (its signals were connected from the start, so it is exact)
    instead of creating one.
  - Editor play starts (`Scene::Copy`) have no load to prepare in: they build in parallel in the start frame.
- **Capacity from the cook, through the scene.**
  - `PhysicsSystem::CreateWorld` keeps reading `SceneSettings::PhysicsCapacity`, which `world.cook` sets for a world's
    persistent scene (area 6). So exported games start at the cooked size.
  - The process-wide remembered demand stays for scenes that are not worlds, and is reported by `physics.stats`.
- **Scene services** (`Scene/SceneServices.h`, appended), offered by `PhysicsSystem`:
  - `struct PhysicsCapacityInfo { uint32_t Bodies, MaxBodies, WaitingForSlot, PendingOther, AddedLastUpdate,
    RemovedLastUpdate; uint32_t GrowthCount; float LastGrowthMs; uint64_t ReservedBytes; }`
  - `class IPhysicsCapacity { virtual PhysicsCapacityInfo GetCapacityInfo() const = 0; virtual void Reserve(uint32_t
    bodies) = 0; }`. `Reserve` asks for room for that many bodies in all; the world grows before the next step if it lacks
    it. The world streamer calls it with the cooked body counts of the cells it is about to load, so cells never meet a
    full world.
- **What a capacity reserves.** `PhysicsWorld::EstimateReservedBytes(const PhysicsSettings&)`, from the per-capacity
  costs `PhysicsCapacitySettings` documents (about 80 bytes per body, 90 per body pair, 830 per contact constraint, plus
  the temp allocator). `physics.stats` and `world.cook` report it. The test `Perf.Physics.MostBodies.ReservedMemory` checks
  the estimate within 15% of the measured value.
- **Counters for budgets and tests**, appended to `PhysicsStats`: `BodiesAddedLastUpdate`, `BodiesRemovedLastUpdate`,
  `ParallelBuildCount`, `ChangeTime` (ms spent applying changes in the last update: the "physics adds and removes" of
  the cell-drive metric) and `WaitingForSlot`.
- **`physics.stats`** (area 5): bodies (total, static, dynamic, kinematic, active), capacity (`maxBodies`,
  `maxBodyPairs`, `maxContactConstraints`, `reservedMB`), `waitingForSlot`, `pendingOther`, `addedLastFrame`,
  `removedLastFrame`, `growths {count, lastMs, from, to}`, `demand`, `lastStepMs`, `heldSteps`, `waitingForMeshes`.

### 2) The sliced unload and one load budget per frame (`Scene/SceneSubScenes.cpp`, `SubScene*.h`, `Scene.*` function-level)

- **`Scene::UnloadSubScene(id)` becomes sliced:**
  1. At once: cancel its load and add `InactiveComponent` to each of its roots that lacks it, recording which ones got it.
     The cell leaves rendering, scripts' updates, physics (one batched `RemoveBodies` through `ActivityChanges`) and audio
     in that frame.
  2. The sub-scene stays in `m_SubScenes` in the new state `SubSceneState::Unloading`. `GetSubSceneStatus` reports it
     until its last entity is gone; then its ID is unknown. `IsSubSceneLoaded` is false.
  3. Each `UpdateSubScenes` destroys a slice: a post-order walk from its still-living roots collects at most the frame's
     destruction budget of entities whose descendants are already gone, and destroys them with one `DestroyEntities`
     call. Running systems hear `OnEntityDestroying`, scripts get `OnDestroy`, physics removes bodies in one batch per
     slice.
  - What a game moved out of the cell's subtrees meanwhile is not destroyed: each slice walks from the roots as they are.
  - `SubSceneStatus` appends `DestroyedEntities`; `UnloadSubScene` of an `Unloading` sub-scene returns true and changes
    nothing.
- **Reload while unloading:** `LoadSubScene` of a source whose previous sub-scene is still `Unloading` keeps the new one in
  `Loading` until the old one is gone, instead of failing on colliding IDs.
- **Budgets.**
  - `SceneSettings` appends `MaxEntitiesDestroyedPerFrame` (default 5,000, 1 to `c_MaxEntitiesDestroyedLimit` = 2^20).
    It is serialized (SceneSerializer, function-level with world-io), set by `scene.setSettings`
    (`maxEntitiesDestroyedPerFrame`) and shown in the Inspector's streaming section next to the instantiation budget
    (function-level with authoring-and-playtest).
  - `Scene::c_MaxInstantiationMillisecondsPerFrame` (2 ms) becomes the time limit of **creation and destruction slices
    together** (renamed `c_MaxSubSceneMillisecondsPerFrame`, with an alias kept for one wave).
- **One per-frame budget for the loaders' main-thread work.**
  - `Scene/SceneWorkBudget.h` (new): `struct SceneWorkBudget { std::chrono::steady_clock::time_point Deadline; uint32_t
    EntitiesToCreate, EntitiesToDestroy; bool HasTime() const; }`, made once per frame by the owner (`GameRuntime::Update`,
    `EditorContext::Update`): `SceneWorkBudget::ForFrame(const SceneSettings&, c_DefaultLoadWorkMilliseconds = 4.0)`.
  - It is shared by: the running or edited scene's sub-scene slices (`Scene::UpdateSubScenes(SceneWorkBudget&)`; the
    parameterless form makes its own budget from the scene's settings, for tests and tools), the pending scene loads'
    preparation (`SceneLoadRequest::Update(SceneWorkBudget*)`), and the world streamer.
  - `SceneLoadOptions::Prepare` takes the budget: `std::function<bool(Scene&, SceneWorkBudget&)>`.
    `RenderScene::PrepareLoadedScene` changes by one line to draw from it (it already takes a budget in
    `RenderScene::Prepare`). gpu-driven-rendering keeps the finer-grained checks inside its preparation (the 14.9 ms
    outlier frame is its chunking); this stream only guarantees one budget is handed down.
- **Edit-mode sub-scenes.** The scene already advances sub-scenes in `OnUpdateEditor`. New `Scene` functions
  (function-level in `SceneSubScenes.cpp`) let the editor and the world system treat cells as sub-scenes in every mode:
  - `SubSceneID AdoptSubScene(AssetHandle source, std::span<const Entity> roots)`: registers existing top-level entities
    as a `Loaded` sub-scene (a play-mode copy's cells, a new cell the editor makes; empty roots allowed).
  - `bool SetSubSceneOf(Entity root, SubSceneID id)`: moves a top-level entity into a sub-scene, or out of every
    sub-scene with 0. It updates `SubSceneComponent` and the loads' root lists.
  - `SubSceneID GetSubSceneOf(Entity entity) const`: the sub-scene of its top-level ancestor, costing its depth.
  - `SubSceneID FindSubSceneBySource(AssetHandle source) const`.
  - Entities reparented under another sub-scene's entity belong to that sub-scene, through their new top-level ancestor;
    a root that becomes a child loses its `SubSceneComponent` (`SetParent`/`PlaceEntities` keep that consistent, checked by
    `ValidateHierarchy`).

### 3) The World module, the World asset and runtime streaming (`Strata/src/Strata/World/`, new layer)

- **Layer.** `World` in `Layers.json`: `MayInclude` Core, Asset, Reflection, Scene. `Engine` includes it (append `World`
  to `Engine`'s `MayInclude`; `RegisterWorldModule`, one line in `BuiltinModules.cpp` at the end: after
  `RegisterAnimationModule` on the base, after Scripting's row once engine-modules' table lands, before game-ui's row).
  `AssetPipeline` gains World
  (a proven downward dependency: `AssetDependencies::Scan` reads world documents), with the reason in `Notes`. Runtime does
  not need it: games start a world as a scene, and their perf-log columns read scene services. Physics, Renderer and
  Scripting never include World: they talk through `IWorldStreaming` and `IPhysicsCapacity`.
- **The World asset** (`World/WorldAsset.h/.cpp`).
  - `AssetType::World` is appended after `AnimationController` (the values are stored in packs: append only), with
    `AssetTypeToString` "World" and native extension `.stworld`.
  - `class WorldAsset : public Asset` holds `WorldData`. The loader is registered by `RegisterWorldModule` and validates
    fully (`WorldData::FromJson`, `Deserialize`). `GetMemoryUsage` reports the CPU bytes of its tables.
  - The document:

    ```json
    { "Strata": { "Format": "World", "Version": 1 },
      "World": {
        "Name": "Island",
        "PersistentScene": "<scene handle>",
        "GridOrigin": [0, 0],
        "Layers": [ { "ID": 1, "Name": "Terrain", "CellSize": 256, "StreamingDistance": 1024,
                      "EnabledByDefault": true, "LoadBeforeStart": true } ],
        "Cells": [ { "Layer": 1, "X": 0, "Z": -1, "Scene": "<scene handle>" } ],
        "Index": { "Version": 1, "MaxBodiesInRadius": 41210, "PersistentBodies": 12,
                   "Cells": [ { "Layer": 1, "X": 0, "Z": -1, "Stamp": "<content stamp>", "Entities": 2502, "Bodies": 501,
                                "Bounds": { "Min": [0, -1, -256], "Max": [256, 30, 0] }, "ClosureBytes": 1048576 } ] } } }
    ```

  - Validation rules (each with a corruption test):
    - the header (Format "World", Version 1);
    - 1 to 32 layers, with unique `ID` 1-255 (stable: cells and `WorldCellKey` name layers by ID, so renames are cheap) and
      unique, non-empty names without control characters;
    - `CellSize` in [1, 65,536] m, `StreamingDistance` in [0, 1,000,000] m;
    - cells: a known layer, unique (layer, X, Z), each cell within `WorldData::c_MaxExtent` (1,000 km) of the origin, a
      valid and unique scene handle;
    - an optional `Index` whose counts are non-negative, bounds finite with Min <= Max, and cells known.
    - Unknown keys are ignored (forward compatibility).
  - The cells' documents live in `<world folder>/<WorldName>/<LayerName>/<X>_<Z>.stscene`. Their handles are deterministic,
    `DeriveSubAssetHandle(worldHandle, "Cell/<layerID>/<X>/<Z>")`, so an undo step can name a cell before its file exists.
  - `AssetDependencies::Scan(AssetType::World)` records the persistent scene and the cells (scenes stop load closures,
    `AssetClosureScope::Load`, so loading a world's persistent scene never pins its cells). `NativeAssetImporter` lists
    `.stworld` in `RegisterAssetPipeline`. The feature project gets a world (area 8).
- **Grid math** (`World/WorldGrid.h`):
  - `struct WorldCellKey { uint8_t Layer; int32_t X; int32_t Z; uint64_t Pack() const; }`;
  - `WorldGrid::CellAt(const WorldLayer&, glm::vec3)` and `CellBounds(...)`: XZ squares from `GridOrigin`, Y unbounded;
  - `Distance(cell, point)`: 2D distance to the square, 0 inside.
- **Components** (`World/WorldComponents.h`, registered in `RegisterWorldModule`, category "World"; never in
  `Scene/Components.h`):
  - `WorldComponent`: `World` (asset, AssetType::World) and `DistanceScale` (float, 0.1-10, default 1: multiplies every
    layer's streaming distance, a quality knob for low-memory devices). The first active one in hierarchy order makes the
    scene a world's persistent scene.
  - `StreamingSourceComponent`:
    - `Enabled` (bool, default true);
    - `Radius` (float, 0 to 1,000,000, default 0 = each layer's streaming distance);
    - `Layers` (string: comma-separated layer names this source streams; empty: every enabled layer; unknown names warn
      once);
    - `Priority` (int, -100 to 100, default 0: higher-priority sources' cells load first);
    - `ViewBias` (float 0-1, default 0.25: cells ahead of the source's forward axis rank as if that share of the radius
      nearer; ordering only, never what is wanted).
  - With no active source, the primary camera is the source.
- **Cell membership.** A loaded cell is a sub-scene whose `SubSceneComponent::Source` is the cell's scene handle. It stays
  runtime-only, as today: unregistered, never serialized, not copied by `Scene::Copy`. The editor records it in undo
  snapshots explicitly (area 4). No registered component carries it, so the feature test's rule that every registered
  component appears in `Feature.stscene` stays meaningful.
- **`WorldStreamer`** (`World/WorldStreamer.h/.cpp`): the core both owners use.
  - Inputs per update:
    - the scene and its `WorldAsset` (requested every frame, never held as `Ref` across frames);
    - the sources (`WorldStreamingSource { glm::vec3 Position; glm::vec3 Forward; float Radius; uint32_t LayerMask; int32_t
      Priority; float ViewBias; }`);
    - enabled layers, pins (`PinCell`, `UnpinCell`, owner-defined: dirty or selected cells in the editor, script requests);
    - the frame's `SceneWorkBudget`.
  - Wanted set:
    - per source and layer, the cells within the source's radius (`Radius`, or the layer's `StreamingDistance` times
      `DistanceScale`) that exist in the cell table;
    - hysteresis: a loaded cell stays while within radius + `H`, `H = max(CellSize / 2, 0.1 * radius)`;
    - pinned cells are always wanted.
  - Loads are ranked by (priority descending, view-biased distance ascending, packed key ascending) and started through
    `Scene::LoadSubScene(cell, {Priority by rank band: High within half the radius, Normal beyond})`, at most
    `c_MaxConcurrentCellLoads` (4) at once. A loading cell that leaves radius + H is cancelled (`UnloadSubScene`); a loaded
    one is unloaded (sliced). Before starting loads it calls `IPhysicsCapacity::Reserve` with the loaded cells' and the
    starting cells' cooked body counts (from the index; without an index, nothing is reserved and growth covers it).
  - Deterministic: decisions are made on the main thread in a total order, so they do not depend on the thread count. When
    no source moved by more than `c_SourceEpsilon` (1 cm), no pin or layer changed and no load or unload finished, an
    update returns after O(sources) work: an idle world costs nothing measurable.
  - Cells are found through a hash map `WorldCellKey -> cell index`, built when the World asset's content version changes.
    The grid squares within a radius are iterated (O(radius^2 / cellSize^2) per source and layer), not the cell table.
  - `GetStats() -> WorldStreamingStats { CellsWanted, CellsLoading, CellsLoaded, CellsUnloading, CellsPinned,
    CellsFailed, LoadsStarted, UnloadsStarted, EntitiesCreatedLastUpdate, EntitiesDestroyedLastUpdate, StateVersion }`
    and `GetCellStates(std::vector<WorldCellState>&)` for the cell map (state, sub-scene, entities, failure).
  - A cell that fails to load is not retried until its asset's content version changes (the map shows it failed, with
    its error).
- **`WorldStreamingSystem`** (`World/WorldStreamingSystem.h/.cpp`):
  - Registered with `MakeSceneSystemDescriptor<WorldStreamingSystem>("World", true, ...)`, `After = { "Scripting" }`,
    `Before = { "Physics" }` on the base; in its rebase onto engine-modules this stream turns them into
    `AfterIfPresent { "Scripting" }`, `BeforeIfPresent { "Physics" }` (both are optional modules).
    `ProfileStage = "World.Streaming"`. It runs in Play and Simulate.
  - It costs nothing in scenes without a `WorldComponent`: it checks a component storage's size once per update.
  - `OnRuntimeStart` adopts the sub-scenes whose sources are cells of its world (a prepared start's or the editor's
    adopted ones). `OnUpdate` gathers the sources from `StreamingSourceComponent`s (a cached view: entities with the
    component, read through `GetWorldTransform`), else the primary camera, plus script requests, and runs the streamer.
  - It offers the scene service **`IWorldStreaming`** (`Scene/SceneServices.h`, appended):
    - `RequestCellsAround(glm::vec3, float radius) -> uint64_t` (a temporary source with priority 100 until released or
      play stops) and `ReleaseCells(uint64_t)`;
    - `IsLoadedAround(glm::vec3, float)`: every existing cell of every enabled layer within that radius is loaded;
    - `SetLayerEnabled(std::string_view, bool)` and `IsLayerEnabled(std::string_view) -> std::optional<bool>`, starting
      from `EnabledByDefault`;
    - `GetStats() -> WorldStreamingStats`.
- **Preparation hooks: a world's start without pop-in or falling.**
  - `SceneSystemDescriptor` appends `std::function<bool(Scene&, SceneWorkBudget&)> PrepareLoadedScene` (function-level
    with engine-modules, which reshapes `SceneSystem.*`).
  - `SceneLoadOptions` appends `bool PrepareSystems = false`. With it, `SceneLoadRequest::Update` calls the registered
    systems' hooks in update order, then the owner's `Prepare`, each frame while the state is `Preparing`, until all
    return true.
  - `GameRuntime` sets it for every scene it loads (start, switches, the loading scene), and so does
    `EditorContext::SwitchRuntimeScene`. `scene.open` does not: edited scenes never run their systems.
  - World's hook: in a prepared scene with a `WorldComponent`, load the wanted cells of layers flagged `LoadBeforeStart`
    around the scene's sources (`Scene::LoadSubScene`, advanced with `UpdateSubScenes(budget)`), returning true once they
    are loaded or failed. Physics' hook (area 1) then builds bodies, and the renderer's proxies come last.
  - The game therefore swaps in a world whose ground exists and has collision. The loading scene (manifest `LoadingScene`)
    plays meanwhile.
  - A world started without preparation (tests, tools calling `OnRuntimeStart` directly) streams its start cells over
    its first frames; `WorldStreamingSystem::WaitForStartCells(timeout)` serves tools and tests.
- **Coordinates.** Scene and physics stay float32; gpu-driven-rendering renders relative to the camera. The precision limit
  is documented in Architecture.md and Docs/Manual/Worlds.md:
  - float32 resolves positions to 2 mm up to 32 km and 4 mm up to 65 km;
  - worlds are supported within **20 km** of the origin (`WorldData::c_SupportedExtent`), and `world.cook` warns about
    cells beyond it;
  - the test `World.Coordinates` covers streaming, physics and picking 20 km out;
  - origin rebasing and double precision are wave 5.
- **HLOD is out of scope (wave 5):** distant cells are simply not loaded. The docs say so, and `world.info` reports each
  layer's streaming distance so a game can set fog or far planes to match.

### 4) The editor: worlds open, stream and save by cell (`Editor/EditorWorld.*`, `EditorContext` world functions)

- **`EditorWorld`** (`StrataEditor/src/Editor/EditorWorld.h/.cpp`, owned by `EditorContext`, `GetWorld()` returns null
  when the edited scene is not a world).
  - Attached when the edited scene has an active `WorldComponent` whose World asset is registered: after `scene.open`'s
    swap (also a project's start scene), after `world.create {open}`, and after `scene.recover` of a world. A missing or failed World asset logs one warning and the scene
    is edited as a plain scene.
  - It owns a `WorldStreamer` for the edited scene:
    - the source is the editor camera's position and forward;
    - radius: each layer's streaming distance times `world.edit {radiusScale}` (default 1);
    - all layers stream unless hidden with `world.edit {visibleLayers}`;
    - loads use `SceneLoadOptions::Reload` (cells are read as the files are now).
  - `EditorWorld::Update(SceneWorkBudget&)` runs in `EditorContext::Update` before `OnUpdateEditor`, under the
    `World.Streaming` stage.
  - Pins: dirty cells (never unloaded until saved), cells holding selected entities, cells pinned through
    `world.loadCells`, and the cells an undo step needs (below).
- **Membership in undo, redo and recovery.**
  - `EntityState` (`Editor/SceneEdit.h`) appends `AssetHandle Cell`: the cell scene of the entity when it is top-level
    (null: the persistent scene; descendants follow their root). `CaptureEntity` reads it through `Scene::GetSubSceneOf`
    and the sub-scene's source; `ApplyEntities` restores it through `Scene::SetSubSceneOf`.
  - `SceneEdit::CheckCells(scene, states, outError)` runs first in `SceneEditAction::Execute` and `Undo`. A state naming a
    cell that is not loaded fails the step before anything changes. That uses the existing contract ("an undo that cannot
    be done now fails and keeps the step applied"), with the reason "the step changes cell Props (3, -1), which is not
    loaded; it is loading now, undo again once it is in". `EditorWorld` pins and loads that cell at once.
  - `SceneRecovery` writes and reads `Cell` in its entity states, so recovered edits return to their cells.
- **Dirty documents.**
  - `EditorWorld` is an `UndoStackObserver`. `UndoStack` has one observer today (`SetObserver`, taken by `SceneRecovery`),
    so it gains a list (`AddObserver`, `RemoveObserver`), in a commit of its own. On every executed, undone or redone step,
    `EditorWorld` marks the documents the step touches: cells and the persistent scene, from the step's entity states
    before and after. A `SceneSettings` step marks the persistent
    scene, and a step of `SceneChangeKind::Other` marks every loaded cell and the persistent scene.
  - Dirty flags clear when a document is saved. They are conservative: undoing back to the saved state leaves the
    document dirty, and its save writes identical bytes.
  - `IsSceneModified` stays the undo stack's, so `CheckUnsavedChanges`, recovery and the title already cover worlds.
- **Saves write only dirty documents.**
  - `EditorContext::SaveScene` on a world calls `EditorWorld::Save`, which saves:
    - the persistent scene's entities without membership;
    - each dirty cell's entities (its sub-scene's roots and their descendants, in hierarchy order, without settings: cells
      use their host scene's settings);
    - new cells: a document, a `.meta` with the derived handle, and an entry in the World document's `Cells`.
  - For every cell it saves, the World document's `Index` entry is refreshed from memory: bounds, entities, bodies,
    closure bytes, stamp. The World document is written when it changed; that write is part of the save, not an undo step.
  - Documents are written through world-io's entity-subset writer and multi-document background save, one
    `UndoStack::MarkSaved` per save, as one `DocumentSaveRequest` (world-io's frozen API):
    - one snapshot of the edited scene (`Scene::Copy` with `SerializedOnly`), taken on the main thread;
    - per document an item whose `SceneWriteOptions::Roots` are the **UUIDs** of its top-level entities, listed on the
      main thread when the snapshot is taken (the snapshot keeps IDs, not the runtime-only `SubSceneComponent`), with
      `WriteSettings` false for cells;
    - new cells reserved with `ReserveNativeAsset(path, derivedHandle)`, so they register with their derived handles;
    - the refreshed World document as an item with ready `Document` bytes.

    Until world-io merges, all cell I/O sits behind one seam, `Editor/WorldCellIO.h` (`SerializeCellDocument`,
    `WriteDocuments`, `ReadCellSummary`), so the switch is one place.
  - A dirty cell that is still loading or instantiating holds the save (the deferred `scene.save` waits for it).
  - `scene.save` of a world answers with world-io's result for the request plus `documents: [{path, kind (persistent,
    cell, world), bytes, entities}]`, one entry per document written.
  - `scene.saveAs` refuses worlds: "a world is saved in place; world.create makes another".
- **Re-celling by position, one undo step.**
  - `EditorContext::CommitEdit` calls `EditorWorld::RecellTracked(transaction)` before recording. For each tracked
    top-level entity of a cell, the cell at its world position in its layer is computed (`WorldGrid::CellAt`); when it
    differs, the entity moves into that cell (`SetSubSceneOf`) inside the same transaction.
  - So a gizmo drag across a border (merged steps), `component.set` of a translation, `entity.setParent` to the top level
    and paste are each one step that undo reverses in both cells.
  - A target cell that is not loaded is loaded and pinned. One that does not exist yet is created in memory
    (`AdoptSubScene(derivedHandle, {})`) and written at the next save.
- **New top-level entities** (create, paste, duplicate, prefab instances, model drops) go into the **edit layer**'s cell
  at their position: `world.edit {editLayer}`, a layer or "Persistent", **default "Persistent"**. Screen-space canvases,
  cameras, lights, skies and game managers that commands create (game-ui's `ui.create` makes a top-level canvas) must not
  stream out with a cell, and every existing test and command script that edits the feature scene (now a world) keeps
  its meaning. People pick a layer in the Hierarchy's world strip, agents with `world.edit`; the choice lasts for the
  session. `world.moveEntities` moves entities between layers and the persistent scene.
- **Play a world in the editor.**
  - `EditorContext::StartRuntime` copies the edited scene as today. That is exactly the persistent scene plus the cells
    the editor has loaded (bounded by the edit radius), never the whole world.
  - Then `EditorWorld::PrepareRuntimeCopy(copy)`:
    - cells mid-instantiation in the edited scene have their partly created entities destroyed in the copy, so the game's
      streamer loads them whole;
    - fully loaded cells are adopted (`AdoptSubScene` by root UUID);
    - dirty cells are pinned in the play streamer for the session, so unsaved edits stay playable.
  - Further cells stream around the play camera from their saved documents. `play.stop` releases the copy as today.
  - The play-start frame builds physics in parallel (area 1). gpu-driven-rendering's render-scene clone hook keeps
    `Render.Extract` out of it (gpu-driven-rendering checks that in its rebase: it merges after this stream).
- **Hierarchy and inspection.**
  - `HierarchyView` filter terms (function-level): `l:<layer>` (entities of that layer's cells; `l:persistent` for the
    persistent scene) and `cell:<x>,<z>`.
  - A compact world strip at the top of the Hierarchy panel shows the world's name, the edit layer (a combo that runs
    `world.edit`), loaded and dirty cell counts, and a button that opens the cell map.
  - The Inspector's header names an entity's cell ("Cell: Props (3, -1)") in secondary text, read through the root's
    sub-scene: O(depth), only for the primary selection.
- **Viewport overlay of cell bounds** (`ViewportSettings::CellBounds`, saved, `viewport.setSettings {cellBounds}`, a chip
  next to the overlay toggles while a world is open): the loaded and loading cells' squares as `DebugDraw` lines at
  their index bounds' height (or y = 0), colored by state with the theme's meanings, drawn only within the edit radius.
  It is built in a function of its own in `ViewportRenderer` and costs nothing while off.

### 5) Commands (`Editor/EditorWorldCommands.cpp`, new; journal kinds in `CommandJournalTests.cpp`)

Every command follows common.md section 8: a description for agents, a full schema, refusal of invalid input without
side effects, one undo step per successful edit, and `Defer` for work that takes frames.

| Command | Kind (results) | What it does |
| --- | --- | --- |
| `world.create {path, cellSize, layers?, open?, force?}` | Edit | Writes `<path>.stworld` and the persistent scene `<path>.stscene`: the basic3d content, a "World" entity with `WorldComponent`, the source being the camera. Layers default to Terrain, Buildings, Props and Gameplay with `cellSize` and a streaming distance of 4 cells. With `open` (default true), opens it (unsaved-changes guard, `force`), which starts a new history like `scene.open`; without it, one undo step (`AssetCreateAction`, which removes the files). Returns `{world, persistentScene, layers}` |
| `world.info {world}` | Query | The World asset: layers (id, name, cellSize, streamingDistance, enabledByDefault, loadBeforeStart, cells), extent, cell count, `index {cooked, staleCells, maxBodiesInRadius, reservedMB}`, persistentScene |
| `world.setLayers {world, layers, deleteCells?}` | Edit | Replaces the layer list (add, rename, reorder, distances, flags). A rename moves the layer's folder (`EditorAssetManager::MoveFolder`, handles kept). Removing a layer that has cells, or changing a cell size that has cells, is refused unless `deleteCells` (cells to the trash). One undo step: an `EditorActionGroup` of the `AssetDocumentAction` and the folder moves |
| `world.edit {editLayer?, visibleLayers?, radiusScale?}` | Session | How the editor edits the open world: the layer new top-level entities go into ("Persistent" or a layer), which layers it streams, and its radius scale |
| `world.moveEntities {entities, layer}` | Edit | Moves top-level entities into a layer's cell at their position, or into the persistent scene. One undo step |
| `world.loadCells {cells?, region?, layers?, pin?, unpin?, unpinAll?, wait?}` | Session | Loads cells by key, or in a region (`{center, radius}` or `{min:[x,z], max:[x,z]}`), pinned by default. `wait` answers once they are loaded: `{cells: [{layer, x, z, state, entities}]}` |
| `world.cook {world?, apply?}` | Edit (deferred) | Cooks the world (area 6): reads every cell off the frame, writes the World document's `Index`, and with `apply` (default true) sets the persistent scene's `PhysicsCapacity.MaxBodies` to the cooked requirement. One undo step (`AssetDocumentAction` plus `SceneSettingsAction` in a group). Returns `{cells, staleBefore, maxBodiesInRadius, maxBodies, reservedMB, warnings (duplicate IDs, cells beyond 20 km, empty cells), seconds}` |
| `world.status {cells?, limit?}` | Query (Session) | The open world (null when none): sources, `cells {wanted, loading, loaded, unloading, dirty, pinned, failed}`, the cell list with `cells`, editLayer, visibleLayers, radiusScale. While playing, the running scene's `IWorldStreaming` stats too |
| `physics.stats` | Query (Session) | The active scene's `PhysicsSystem` (area 1). `{running: false}` in edit mode |

- `scene.loadStatus` gains `world` (function-level in `DescribeSceneLoads`): the editor's and, while playing, the game's
  `{cellsWanted, cellsLoading, cellsLoaded, cellsUnloading, cellsPinned, cellsFailed}`. `subScenes` entries show
  `Unloading` with `destroyedEntities`.
- `scene.unloadAdditive` answers at once, as today, with the sub-scene now `Unloading`. Its result appends
  `unloadingEntities`, and the description says the entities go over the following frames.
- `editor.status` gains a compact `world` section through `SetStatusProvider` (name, cells loaded, loading, dirty).

### 6) The cook (`world.cook` and export: `Editor/WorldCook.h/.cpp`, `GameExport` function-level with world-io)

- **What it computes, off the frame.**
  - It reads every cell document on I/O jobs through world-io's cancellable reader, behind the seam
    `WorldCellIO::ReadCellSummary` until it merges, for cells whose content stamp (`EditorAssetManager::GetContentStamp`)
    differs from the index entry's `Stamp`. Loaded dirty cells are summarized from memory.
  - Per cell:
    - entity count;
    - body count, through `PhysicsWorld::CountBodyCandidates(const Scene&)`, made public: the same rule physics sizes by;
    - bounds: the union of every entity's world position and its renderers' recorded mesh bounds
      (`AssetManagerBase::GetBounds`) and its colliders;
    - closure bytes: the stored sizes of the cell's dependency closure without other scenes.
  - Duplicate entity IDs across cells and the persistent scene (an error that names them), cells beyond the supported
    extent and empty cells are warnings.
- **The most bodies any streaming radius can hold.**
  - For each layer, take the largest radius any source of the persistent scene uses (else its streaming distance times
    `DistanceScale`) plus its hysteresis. For each cell, sum the bodies of the cells whose squares lie within that radius
    of any point of the cell: a conservative bound on what a source anywhere in the cell keeps loaded. Sum over layers.
  - `MaxBodiesInRadius` is the largest such sum times the number of streaming sources in the persistent scene (at least
    1), plus `PersistentBodies`. The capacity written is `ceil(1.25 * MaxBodiesInRadius)` for spawns, clamped to the limit.
  - `world.cook {apply}` sets it on the persistent scene's `SceneSettings::PhysicsCapacity.MaxBodies` (saved with the
    scene), and reports `EstimateReservedBytes` of the result.
- **Export.**
  - `GameExport` runs the cook for stale cells before it plans: `WaitingForImports`, then a new `CookingWorlds` state,
    then `Packing`.
  - The packed World asset carries the fresh `Index`.
  - When the saved persistent scene's capacity is below the cooked requirement, the packed persistent scene gets the
    cooked capacity, through world-io's export transform of scene documents (`AssetPackCookOptions::AdjustSceneSettings`,
    frozen with world-io's cook), so a game never starts smaller than its world needs.
  - Cells are cooked to world-io's binary form once world-io has merged.
  - Pack order: after the start scene's closure, a world's cells follow in order of their distance from the start sources,
    layer by layer (`AssetPackBuildOptions` appends `SceneOrder`, filled by `GameExport` for worlds; function-level with
    world-io), so a game's first streaming reads the pack front to back.
- Per-cell bounds, entity and body counts and closures live in the World asset's `Index`. The game's streamer reads them
  for `Reserve`, the cell map and `world.info`. No new pack table is needed.

### 7) Observability (Streaming panel, status bar, perf log, runtime pacing)

- **Cell map** (`UI/CellMap.h/.cpp`, drawn in the Streaming panel's new "World" section, function-level).
  - Drawn while the editor has a world open, or a running scene streams one.
  - A 2D map of the cells of the visible layers, one layer at a time (a chip row picks the layer, "All" overlays them).
    Each cell's square is colored by state with the theme's meanings: unloaded (surface), wanted (secondary text),
    loading (Sulfur), loaded (Ochre accent), unloading (secondary), dirty (an outline in the warning color), pinned (a pin
    glyph), failed (Cinnabar).
  - Sources are drawn as dots with their radius circles and the camera's facing.
  - Pan with a drag and zoom with the wheel; it draws only the cells in view, with `ImDrawList` rectangles, no per-cell
    widgets.
  - Hovering shows the cell's key, entities, bodies and state. Clicking selects the cell; its context menu offers Load and
    pin, Unpin and Show in Content Browser.
  - Its cell array is rebuilt only when `WorldStreamingStats::StateVersion` or the World asset's content version changed,
    and nothing is drawn or rebuilt while the panel is closed.
  - Sizes follow the font, colors come from tokens, controls from the widget kit (`Editor.UI.Source` passes).
- **Physics section** of the Streaming panel: a capacity bar (bodies against `MaxBodies`, colored by
  `GetBudgetPressure`), waiting bodies, adds and removes of the last frame, growths with the last one's time, reserved MB.
  It is read once per frame in `OnUpdate` from `physics.stats`' source.
- **Status-bar problem:** while the running scene has bodies waiting for a slot, a Cinnabar pill "N bodies without
  collision" (compact: "N no collision") opens the Streaming panel's Physics section, through `StatusPillText`. It is read
  in `OnUpdate`: one `GetCapacityInfo` per frame while playing.
- **Perf log** (`StrataRuntime --perf-log`): the `World.Streaming` stage column comes with the stage. Columns
  `WorldCellsWanted`, `WorldCellsLoading`, `WorldCellsLoaded`, `WorldCellsUnloading`, `PhysicsBodies`,
  `PhysicsBodiesWaiting` and `PhysicsMaxBodies` are filled by a column group `RuntimeApplication` passes, which reads the
  running scene's `IWorldStreaming` and `IPhysicsCapacity`.
  - **This stream lands `FramePerfLog`'s column groups**, in one commit of its own, function-level in
    gpu-driven-rendering's file, in exactly the shape common.md section 12 gives (`FramePerfRow::Add`/`AddEmpty`,
    `FramePerfColumnGroup {Names, Write}`, `FramePerfLogSpecification {Columns}`, the new `Create` parameter): group
    columns follow every built-in column, and a group that writes another number of values than it names fails the log.
    Its test goes into `Runtime.FramePerfLog`. gpu-driven-rendering (merging after) builds its `Render` group and GPU
    columns on it.
- **Runtime pacing while loading:** while `GameRuntime` has no scene to show (`RenderEmpty`), `RuntimeLayer` caps the
  application at `c_HeadlessFrameRate` (60) through `Application::SetMaxFrameRate`. It restores the manifest's pacing (vsync,
  uncapped) once a scene draws. `rt1M_window.csv`'s 900 frames per second become at most 60, and the loading thread gets
  the CPU.

### 8) Script SDK (first host-function block of wave 4) and the feature test

- **Host functions**, appended after `PhysicsGetLayerIndex` (119 -> 124) in `ScriptABI.h` and `ST_SCRIPT_HOST_FUNCTIONS`,
  each `Host<Name>` in `ScriptHostAPI.cpp` through `HostCall`, `ResolveContext` and the scene's `IWorldStreaming`. Calls
  in a scene that streams no world fail and are reported once (`ReportProblem`).
  - `uint64_t (*WorldRequestCellsAround)(StrataScriptContext*, const float* position, float radius)`: a request ID, 0 on
    failure.
  - `bool (*WorldReleaseCells)(StrataScriptContext*, uint64_t request)`
  - `bool (*WorldIsLoadedAround)(StrataScriptContext*, const float* position, float radius)`
  - `bool (*WorldSetLayerEnabled)(StrataScriptContext*, StrataScriptString layer, bool enabled)`
  - `int32_t (*WorldIsLayerEnabled)(StrataScriptContext*, StrataScriptString layer)`: 1, 0, or -1 for an unknown layer.
- **SDK** `StrataScript/World.h` (included by `StrataScript.h`), optional functions through
  `ST_SCRIPT_DETAIL_HOST_WITH`:
  - `class WorldCellRequest { uint64_t GetID() const; bool IsValid() const; bool Release() const; }`;
  - `class World { static WorldCellRequest RequestCellsAround(const glm::vec3&, float); static bool IsLoadedAround(const
    glm::vec3&, float); static bool SetLayerEnabled(std::string_view, bool); static std::optional<bool>
    IsLayerEnabled(std::string_view); }`.
  - Recipe in the skill: prefetch before a teleport or spawn, poll `IsLoadedAround`, then teleport.
- **Tests:** the API test module (`StrataTests/Scripts/API`) calls each function, with a `Scripting.World` suite. The
  feature scripts get `WorldFeatures.cpp`: request cells around a far point, expect them loaded within the scenario,
  disable "Props" and expect its cells unloaded, re-enable it, read `IsLayerEnabled`, release. A script entity inside a
  Props cell journals `OnCreate` and `OnDestroy` as cells stream.
- **The feature project's tiny world:**
  - `Assets/Worlds/FeatureWorld.stworld` (+ `.meta`), with two layers: "Ground" (cell 32 m, distance 40, LoadBeforeStart)
    and "Props" (cell 16 m, distance 20, not LoadBeforeStart), each 2x2 cells: 8 cell documents under
    `Assets/Worlds/FeatureWorld/`, with committed `.meta` files and a cooked `Index`. Ground cells hold a static floor box;
    Props cells hold a few renderers, a dynamic body and the journaling script.
  - `Feature.stscene` becomes the world's persistent scene: an entity "World" with `WorldComponent {World: FeatureWorld,
    DistanceScale: 1.5}`, and two sources: "Streamer" (`Radius` 30, `Layers` "Ground, Props", `Priority` 5, `ViewBias`
    0.5) moved by a feature script along the 2x2 world, and "Idle Source" (`Enabled` false). Every property is non-default
    somewhere.
  - IDs do not collide with the scene's or `FeatureSub.stscene`'s.
  - All runners pass: `StrataTests.FeatureTest` (headless, editor commands, exported game), `GPU.FeatureTest`,
    `StrataEditor.FeatureTest` and `StrataRuntime.FeatureTest`. Messages logged on purpose go into
    `c_ExpectedLogMessages`.

## What this stream guarantees the other streams meanwhile

This stream changes pipelines other streams use. Until and after it merges, it keeps these promises:

- **Sub-scenes:**
  - `Scene::UnloadSubScene` keeps its signature and return value.
  - What changes: the entities go over the following updates (state `Unloading`, roots inactive at once), and
    `HasPendingSubScenes` is true until they are gone.
  - `Scene::FlushSubSceneUnloads()` (new) destroys what is left at once, for tools and tests that need it now.
    `OnRuntimeStop` and `ReleaseInBackground` finish unloads without slicing.
  - Scripts' `SubScene::Unload` and `scene.unloadAdditive` keep their meaning: removed from the game in that frame,
    because it is inactive.
- **Physics:** `PhysicsWorld`'s public API stays.
  - Parallel creation gives the serial path's body IDs and simulation, bit for bit.
  - Growth keeps every `JPH::BodyID`, velocity, sleep state and touching pair. Code that keeps body IDs (character
    controllers, contacts) stays valid.
  - `PhysicsStats` only gains fields at its end. `BuildFailure::BodyLimit` is internal (renamed).
- **Scene loads:** `SceneLoadOptions::Prepare` gains a `SceneWorkBudget&` parameter.
  - The one caller outside this stream's files, `RenderScene::PrepareLoadedScene`, changes in the same commit with one
    line, drawing from the budget.
  - `SceneLoadOptions::PrepareSystems` defaults to false, so a load that does not ask behaves as today.
- **Appended only:** `SceneSettings::MaxEntitiesDestroyedPerFrame`, `SceneSystemDescriptor::PrepareLoadedScene`,
  `EntityState::Cell`, `AssetType::World`, `PhysicsStats` fields and the scene services `IPhysicsCapacity` and
  `IWorldStreaming`. Existing files and documents load unchanged.
- **No manifest change.** A world's start scene is its persistent scene, so `GameManifest` stays at version 4.
- **Idle cost:** a scene without a `WorldComponent` pays one storage-size check per update for the world system. An editor
  without an open world pays nothing for `EditorWorld`.

## Deliver in this order if time runs short

1. **Physics at world scale** (area 1): waiting bodies out of the per-step retry list (FIFO waiters), growth by
   migration, parallel creation, `IPhysicsCapacity`, `physics.stats`, the Physics section and the status pill. This closes
   the critical finding for today's sub-scenes and `scene.loadAdditive`.
2. **The sliced unload and the load budget** (area 2), and prepared physics during `Preparing` (area 1, area 3's hooks).
3. **The World module** (area 3) with the cook (area 6), plus the feature project's tiny world (area 8: the new
   components must be in `Feature.stscene` the day they are registered).
   - The World asset, its components, `WorldStreamer`, `WorldStreamingSystem`, `IWorldStreaming` and the preparation
     hooks;
   - `world.cook`, `world.info` and the capacity in exports;
   - runtime pacing and the perf-log columns.

   With this, a world can be cooked and driven in games: it closes the critical findings even if the editor's world
   editing slips.
4. **Editor-side cell streaming and editing** (area 4):
   - `EditorWorld`, and membership in undo and recovery;
   - dirty-cell saves, re-celling and the edit layer;
   - `world.create`, `world.setLayers`, `world.edit`, `world.moveEntities`, `world.loadCells` and `world.status`;
   - the cell map, the Hierarchy filter and strip, and the cell-bounds overlay last.
5. **Playing a world in the editor** (area 4: adopted cells, dirty cells pinned in play).
6. **The SDK block and the coordinates test** (area 8, `World.Coordinates`).

Docs, skills and screenshots go with each item that lands, never at the end only.

## Acceptance criteria

### Tests (confirm that each fails on the base where it applies, by running it on the step-0 base build or with the fix reverted; say how in the report)

The suites below run in Debug Core shards: keep them within about 60 s each in Debug by scaling the setups down with
the same ratios (a capacity of 4,096 against four cells of 2,500 bodies instead of 65,536 against four of 25,000; drives
of fewer frames over smaller grids), as common.md section 10 asks. The full-size cases are the Release perf metrics
below (`Perf.World.PhysicsBodiesWaiting`, `CellUnloadMaxFrame`, `CellDriveMaxFrame`). A must-fail claim holds for the
scaled setup too (the base still leaves bodies waiting at 4,096).

- **`Physics.Capacity`** (`StrataTests/src/Physics/PhysicsCapacityTests.cpp`):
  - *Growth instead of waiting:*
    - Setup: four sub-scenes of 25,000 static boxes each (`Tests::` world generator, the shape of `gen_cells.py`), plus
      1,000 dynamic bodies moving with known velocities and 200 resting asleep on a floor, loaded into a running scene
      whose world was created at 65,536 bodies.
    - After the loads: `WaitingForSlot` 0, `BodyCount` equal to every candidate, `GrowthCount` >= 1.
    - Every dynamic body's linear and angular velocity just after the growth equals its velocity just before, within
      1e-6.
    - Sleeping bodies are still asleep, and no `OnCollisionEnter` repeats for a pair that touched before the growth.
    - Fails on the base: 34,465 bodies wait.
  - *Waiters are not polled:*
    - Setup: capacity 1,024 with `PhysicsSettings::GrowthLimit` = 1,024 (new: the most a world may grow to; default the
      engine's limit; also a memory cap for games), and 3,000 bodies.
    - 1,976 bodies wait, and `SyncedBodyCount` per step counts no waiting record.
    - Destroying 100 bodies gives exactly the first 100 waiters, in their order, bodies in the next update.
    - Fails on the base: every waiting record is synced every step.
  - *Parallel equals serial:* 20,000 bodies (static, dynamic, kinematic, compound, colliders on children), built with one
    worker and with the application's workers (`Perf::ScopedApplicationJobSystem`). Same body ID per entity, and poses
    identical bit for bit after 120 steps.
  - *Prepared world:* a scene loaded with `PrepareSystems` builds its bodies over its `Preparing` frames, none above
    `c_PrepareBodiesPerSlice` owners per frame, and its first running update builds 0 bodies (`BuildCount` unchanged).
  - *Character controllers survive growth:* a walking controller keeps position, velocity and ground state across a
    growth.
- **`Scene.SubScenes`** (extended, `StrataTests/src/Scene/SubSceneTests.cpp`):
  - *Sliced unload:*
    - Unloading a 25,001-entity sub-scene destroys 0 entities in the requesting frame and leaves every one of them
      inactive in the hierarchy.
    - Each later update destroys at most `MaxEntitiesDestroyedPerFrame` (and within a test budget's time). It is done after
      `ceil(25,001 / 5,000)` updates, with `CheckSceneCaches` after each.
    - Fails on the base: everything goes in the requesting frame.
  - *One budget:* creation and destruction in the same update together stay within the `SceneWorkBudget` counts.
  - *Reload while unloading* waits, then loads without an ID collision.
  - *Membership:* `AdoptSubScene`, `SetSubSceneOf` and `GetSubSceneOf` follow reparenting both ways, with
    `ValidateHierarchy` after each.
  - An entity reparented out of an unloading sub-scene survives.
- **`World.Asset`** (`StrataTests/src/World/WorldAssetTests.cpp`):
  - JSON and byte round trips;
  - every validation rule of area 3 rejected with a reason;
  - truncated and garbage bytes rejected without crashing;
  - `AssetDependencies::Scan` lists the persistent scene and every cell once;
  - the world survives a pack round trip (`AssetPackTests` style).
- **`World.Streaming`** (`StrataTests/src/World/WorldStreamingTests.cpp`, on a generated world mounted from a pack):
  - *Hysteresis:* a source oscillating across a border by less than `H` starts no load or unload; beyond `H` the far cells
    unload.
  - *Deterministic across thread counts:* the list of (frame, load or unload, cell) decisions of a 600-frame drive is
    identical with one worker and with the application's workers.
  - *Budgets hold (deterministic counters):* over a drive, per frame:
    - entities created <= `MaxEntitiesInstantiatedPerFrame`, and entities destroyed <= `MaxEntitiesDestroyedPerFrame`;
    - physics bodies added and removed per update <= those (`BodiesAddedLastUpdate`, `BodiesRemovedLastUpdate`);
    - cell loads in flight <= `c_MaxConcurrentCellLoads`.

    `GPU.World` checks render proxies prepared per frame against the same entity budget (`SceneRendererStats::ProxyUpdates`).
  - *Layers and sources:*
    - a disabled layer's cells unload, and a source's `Layers` filter limits what it streams;
    - higher-priority sources' cells start first, and with no source the primary camera streams;
    - `RequestCellsAround` loads a far region, and `IsLoadedAround` tells the truth.
  - *Reserve before loading:* with a cooked index, physics grows before the cells' bodies arrive, and `WaitingForSlot`
    stays 0 at every frame of the drive.
  - *Idle:* with a still source, an update evaluates no cell (counter `EvaluatedCells` 0).
  - *Prepared start:* a world loaded with `PrepareSystems` is Ready with its `LoadBeforeStart` cells loaded and their
    bodies built. A dynamic body resting on a Ground cell is still resting after 60 steps: it did not fall through a
    missing floor.
- **`World.Coordinates`:** a world whose cells lie 20 km from the origin.
  - A source driven there streams the right cells.
  - A box dropped on a ground cell there comes to rest and drifts less than 1 cm over 600 steps.
  - Physics' `Raycast` and the editor's `SceneRaycast::Cast` from 20 km hit the right entity.
  - `GPU.World` picks it through the entity-ID buffer (`EditorViewport::RequestPick`) 20 km out. gpu-driven-rendering
    runs it again in its rebase, through its camera-relative rendering.
- **`Editor.World`** (`StrataTests/src/Editor/EditorWorldTests.cpp`, through commands):
  - *Opens and streams in edit mode:* `scene.open` of a persistent scene loads the cells around the editor camera, and
    `camera.set` far away loads and unloads cells. `world.status` and `scene.loadStatus`'s `world` agree.
  - *Saves only the dirty cell:*
    - Setup: edit an entity of cell A with `component.set`, then `scene.save`.
    - A's file changed. The persistent scene's file and every other cell's file are byte-identical and keep their
      modification times.
    - The World document's index entry for A is refreshed.
  - *Border move is one undo step:*
    - Setup: a `component.set` translation that crosses into cell B.
    - The entity belongs to B. One `edit.undo` puts it back in A with its old transform; `edit.redo` moves it again.
    - Both cells are dirty, and the save writes both. Reopening the world shows it in B.
  - *A dirty cell is never lost:*
    - Setup: edit in A, move the camera ten radii away, run 300 frames.
    - A stays loaded while the others unload, and `scene.save` writes A's edit.
    - Before saving, `project.close` without `force` is refused.
    - The recovery journal holds A's state with its `Cell`, and `scene.recover` puts it back into A.
  - *Undo of an unloaded cell:* after a save, cell A unloads and `edit.undo` fails with the reason and starts loading A;
    once A is in, the undo succeeds.
  - *New cells and layers:*
    - With the default edit layer, `entity.create` puts the entity into the persistent scene (no cell changes).
    - With `world.edit {editLayer: "Props"}`, an entity placed where no cell exists yields, at the save, a document with
      the derived handle, its `.meta` and a `Cells` entry.
    - `world.moveEntities` moves entities to "Persistent" and back.
  - *Play copies only loaded cells:*
    - Setup: a 64-cell world with 9 cells loaded, then `play.start`.
    - The running scene holds the persistent scene's entities plus those 9 cells' entities; nothing half-instantiated is
      copied.
    - Moving the play camera streams further cells from their saved documents, and a dirty cell stays loaded through play.
  - *Commands:* invalid input to each `world.*` command changes nothing, and each edit is one undo step. The new commands
    are in `CommandJournalTests`' lists: Edit `world.create`, `world.setLayers`, `world.moveEntities`, `world.cook`; Query
    `world.info`, `world.status`, `physics.stats`; Session `world.edit`, `world.loadCells`; session results
    `world.status`, `physics.stats`.
- **`Editor.WorldCook`:**
  - On a generated world, the index's bounds, entity counts, body counts and closure bytes equal an independent count,
    and `MaxBodiesInRadius` equals a brute-force evaluation on a 5x5 world.
  - `apply` sets the persistent scene's capacity in one undo step.
  - Duplicate IDs across cells are reported by name.
  - A second cook re-reads only the cells whose stamps changed (counter).
  - An export packs the fresh index and the cooked capacity, and the game reads both.
- **`Editor.UI.World`** (`ImGuiHarness`):
  - the cell map draws every state with `CountFallbackGlyphs` 0, and its hover tooltip and context menu work;
  - the Physics section draws;
  - the "bodies without collision" pill appears while bodies wait and opens the Streaming panel;
  - the Hierarchy strip's edit-layer combo runs `world.edit`;
  - `Editor.UI.Source` passes;
  - with a world open and the Streaming panel closed, the cell map rebuilds nothing (counter).
- **An exported world streams headless in StrataRuntime** (CTest `StrataRuntime.World`, label `feature`):
  - Setup:
    - `StrataTests --strata-test-helper=write-world-game <dir>` generates a 6x6-cell world: Ground cells of 2,500
      entities with 500 static bodies, and a Props layer.
    - A test script module (`StrataTests/Scripts/World`, target `StrataTestScriptsWorld`) moves a `StreamingSource`
      across every cell. At each cell it waits for `World::IsLoadedAround`, raycasts down and counts the cell's bodies
      with `Physics::OverlapBox`.
    - The helper exports the world and runs `StrataRuntime --headless --frames 1800 --perf-log`.
  - The process exits 0 and logs "World drive: 36 cells visited, 0 without collision". Every perf-log row has
    `PhysicsBodiesWaiting` 0.
- **Feature test:**
  - `Feature.stscene` is the tiny world's persistent scene, and the project holds `FeatureWorld` (2x2 cells, two layers)
    and two streaming sources with every property non-default.
  - The five host functions, the SDK's `World` and `WorldCellRequest` and the journaled cell script are covered.
  - All runners pass: `StrataTests.FeatureTest`, `GPU.FeatureTest`, `StrataEditor.FeatureTest` and
    `StrataRuntime.FeatureTest`.
- **`Architecture.Layering`** passes with the World layer, AssetPipeline -> World (with `Notes`), and no new allowlist
  line.
- **The full Debug `ctest --preset windows-debug`** passes under the validation layer, and so does `ctest --preset
  windows-release -LE perf`. That includes Tetris, Smoke, Feature and Scripts, whose sub-scene unloads are now sliced.

### Perf

Release builds. Checked with `Perf::CheckBudgetConfirmed`. Entries go in `Budgets.json`, marked "to confirm in the
integration campaign", from interleaved A/Bs under the perf lock against the step-0 base build.

| Metric | Unit, statistic | How it is measured | Budget basis |
| --- | --- | --- | --- |
| `Perf.World.CellDriveMaxFrame` | ms, maximum by nature ("no frame over") | Setup: a 16x16-cell grid world (world-io's `Perf/WorldFixture.h` grid, else `Perf/WorldGenerators.h`: 2,502 entities and 501 static bodies per cell, `gen_grid.py`'s shape) mounted from a pack, driven headless by a source at 40 m/s along a row for 900 frames under `ScopedApplicationJobSystem`. Per frame: `Scene.Load` + `World.Streaming` + `PhysicsStats::ChangeTime`; the longest frame | Acceptance limit 8 ms |
| `Perf.World.CellUnloadMaxFrame` | ms, maximum by nature | One cell of 25,000 entities with 25,000 static bodies: the longest frame of the same sum, from the unload request to the last entity destroyed | Limit 4 ms (base: 51-56 ms) |
| `Perf.World.PhysicsBodiesWaiting` | count, maximum | Four 25,000-body cells loaded into a world created at 65,536 bodies: the most bodies waiting for a slot in any frame after the first load started | 0 |
| `Perf.Physics.Grow65kMigration` | ms, median of 5 | A fresh world of 65,536 bodies (90% static, 10% dynamic and awake) grows to 131,072: the growth's time (`LastGrowthMs`) | 1.5 x measured; target 16.7 ms |
| `Perf.Physics.Create100kStaticBodies` | ms, median of 5 | The update that creates 100,000 static box bodies (the 1M world's play start: 147-157 ms on the base) | 1.5 x measured; target 25 ms |
| `Perf.World.DrivePeakPrivate` | MB, maximum by nature | Helper process `--strata-test-helper=world-drive <dir>`: a 100x100-cell world of 2,500 entities per cell (25M entities, cells in world-io's binary form), driven headless along its diagonal; its last `PeakPrivateMB` | 1.5 x measured; target 2,048 MB (the audit's "a 25M-entity, 4 GB world peaks under 2 GB") |
| `Perf.World.DriveFrameP95` | ms, 95th percentile | The same drive's frame work | 1.5 x measured; target 16.7 ms (60 FPS) |
| `PerfGPU.Editor.PlayStartWorldMaxFrame` | ms, maximum by nature | Setup: the real editor, maximized, opens a generated 32x32-cell world (2.56M entities in cells, 10% bodies); its edit radius loads about 25 cells (about 64,000 entities). Then `play.start`; `editor.perf`'s frame maximum | Limit 50 ms |
| `PerfGPU.Editor.EditFrameWorldIdle` | ms, median | The same world open with a still camera: an idle editor frame | 1.5 x measured, and not above `PerfGPU.Editor.EditFrameNested100k`'s budget (an acceptance limit) |

- **No regression**, by A/B ratios of the medians, at most 1.03 or within the base's spread:
  - `PerfGPU.Editor.{Edit,Play}FrameNested{100k,1M}`, all of `Perf.Physics.*`, `Perf.Runtime.SceneSwitch100kMaxFrame`
    and `PerfGPU.Editor.SceneOpen*`.
  - `Perf.Physics.Create10kBodies` should drop with parallel creation; report the ratio and leave the lowering to the
    campaign.
  - `Perf.Physics.MostBodies.ReservedMemory` also checks that `EstimateReservedBytes` lies within 15% of what it measures.
- **Idle cost:** with `editor.perf`, the `World.Streaming` stage's mean in an idle editor with a world open is below
  0.01 ms. The cell map costs nothing while the Streaming panel is closed.

### Screenshots

Take them with the Release editor, `--frames N --screenshot`, a private `STRATA_SESSION_DIR`, `STRATA_RECENT_PROJECTS` and
`--layout`, into `scratchpad\wave4\world-partition\shots`. Look at each one and fix what looks unfinished.

- A world (the generated 32x32 grid, or a hand-made island-like world of four layers) open in the editor with the
  Streaming panel's World section: the cell map shows loaded, loading and wanted cells around the camera, and the cell
  bounds overlay is on in the viewport. At the display's 150%, at `--ui-scale 1` and in a 1600x900 window.
- The Physics section and the "bodies without collision" pill (a world whose `GrowthLimit` makes bodies wait), at 150%.
- The Hierarchy's world strip with the `l:` filter, and the Inspector's cell line, at 150%.
- The exported world in `StrataRuntime --resolution 1920x1080` after streaming has settled.

### Docs and skills

- `Docs/Architecture.md`:
  - a new "World partition" section covering the World asset, layers, cells and their files, membership, the streamer and
    its two owners, the system and its preparation hooks, budgets and the sliced unload, physics capacity, growth and
    parallel creation, the cook, the editor's world (dirty cells, re-celling, play), the precision limit and HLOD's absence;
  - updates to "Scene runtime lifecycle" (sliced unloads, `SceneWorkBudget`, preparation hooks), the Physics part of
    "Engine modules", and the layer table.
- `AGENTS.md`, rules only:
  - never `Scene::Copy` a whole world: the edited scene is the persistent scene plus loaded cells;
  - cell membership changes only through `Scene::SetSubSceneOf` and `EditorWorld`, and cell documents are written only by
    `EditorWorld` through `WorldCellIO`;
  - worlds stay within 20 km;
  - scenes that will run load with `PrepareSystems`;
  - loading work draws from the frame's `SceneWorkBudget`;
  - physics never leaves a body without collision below its growth limit.
- `Docs/Manual/Worlds.md` (new, for people): what a world, layers and cells are, creating and editing one, the cell map,
  saving, cooking, playing, exporting, sources and the script API, limits.
- Skills:
  - `strata-editor-automation`: `world.*`, `physics.stats`, `scene.loadStatus`'s `world`, the `l:` and `cell:` filters;
  - `strata-make-a-game`: a world recipe (create, layers, place by layer, cook, play with a source, export);
  - `strata-scripting`: the `World` API and the prefetch-before-teleport recipe;
  - `strata-add-component`, where it names `Components.h` for engine-module components (common.md section 2).
- Remove the two physics lines this stream closes from `Docs/Roadmap.md`'s open items, and say so in the report.

## Open items this stream owns

- **Closes:**
  - scale C "Streamed bodies beyond the play-start physics capacity get no collision";
  - scale C "What world partition must deliver";
  - scale M "Large cells hitch on load and unload": its scene part, the sliced unload and one per-frame load budget;
  - scale M "Starting play on a real 1M world is a 0.6 s frame": its physics part, parallel batches and preparation;
  - scale m: `physics.stats`, the Physics section and pill, the cell map and runtime pacing;
  - the Roadmap's open items on the unsaved overflow and the reserved memory.
- **Limits documented, not fixed:**
  - Dynamic bodies stay members of the cell they loaded with: unloading that cell takes them along. Objects that roam
    the world belong in the persistent scene.
  - References between entities of different cells resolve only while both are loaded.
  - Prefab changes reach cells as they load in the editor (the catch-up marks them modified); updating every cell of a
    world in one batch is later work.
  - Changing the cell size of a layer that has cells (re-gridding) is refused; later work.
- **Out of scope, with their owners:**
  - HLOD (w5; distant cells are not loaded);
  - origin rebasing and double precision (w5);
  - project-wide settings and collision layers (w5 `project-settings`; cells use their host scene's);
  - terrain cooked into cells (w5 `world-environment`);
  - mesh, audio and collision streaming (w5 `mesh-streaming`);
  - instance uploads within the upload budget, the render-scene clone at play start and camera-relative rendering
    (gpu-driven-rendering);
  - residency info walking every registered asset (world-io, by common.md section 12's table, although the Roadmap names
    this stream);
  - binary cells, batched registration of thousands of cell documents and the background multi-document save
    (world-io);
  - per-world audio listeners and input contexts, and 64-bit entity handles (Later).

## Files expected to be touched

- `Strata/src/Strata/World/*` (new):
  - `WorldAsset.h/.cpp`, `WorldGrid.h/.cpp`, `WorldComponents.h`;
  - `WorldStreamer.h/.cpp`, `WorldStreamingSystem.h/.cpp`;
  - `WorldRegistration.h/.cpp` (`RegisterWorldModule`).
- `Strata/src/Strata/Engine/BuiltinModules.cpp` (one line). Not `Strata/src/Strata.h`: engine-modules deletes the
  umbrella header, so nothing is added to it.
- `Strata/src/Strata/Runtime/FramePerfLog.h/.cpp` (the column-group commit, function-level in gpu-driven-rendering's
  file), `StrataTests/src/Runtime/FramePerfLogTests.cpp`.
- `Strata/src/Strata/Physics/*`:
  - `PhysicsWorld.h/.cpp`: waiters, growth, parallel build, sliced build, `EstimateReservedBytes`, `CountBodyCandidates`
    made public;
  - `PhysicsSystem.h/.cpp`: services, prepare hook, adoption;
  - `PhysicsTypes.h` (stats, `GrowthLimit`), `CharacterControllers.h/.cpp` (`Rebind`), `PhysicsRegistration.cpp` (hook).
- `Strata/src/Strata/Scene/`:
  - `SceneSubScenes.cpp`, `SubScene.h`, `SubSceneLoad.h`;
  - `Scene.h/.cpp` (function-level: sub-scene membership, `UpdateSubScenes(SceneWorkBudget&)`);
  - `SceneWorkBudget.h` (new), `SceneServices.h` (appended services), `SceneSystem.h` (descriptor field);
  - `SceneLoader.h/.cpp` (`PrepareSystems`, budget; function-level with world-io);
  - `SceneSerializer.cpp` (one settings field; function-level with world-io).
- `Strata/src/Strata/Renderer/RenderScene.h/.cpp`: the one-line `PrepareLoadedScene` signature change.
- `Strata/src/Strata/Asset/`: `AssetTypes.h/.cpp` (World), `AssetImporters.cpp` (native `.stworld`),
  `AssetDependencies.cpp` (world documents).
- `Strata/src/Strata/Runtime/GameRuntime.h/.cpp` (`PrepareSystems`, the frame's budget), `StrataRuntime/src/RuntimeApplication.cpp`
  (pacing, perf-log provider).
- `Strata/src/Strata/Scripting/ScriptHostAPI.cpp`; `StrataScriptCore/Include/StrataScript/ScriptABI.h`, `World.h` (new),
  `StrataScript.h`.
- `StrataEditor/src/Editor/`:
  - new: `EditorWorld.h/.cpp`, `WorldCook.h/.cpp`, `WorldCellIO.h/.cpp`, `EditorWorldCommands.cpp`;
  - `EditorContext.h/.cpp` (world functions), `EditorCommands.cpp` (registration call);
  - `SceneEdit.h/.cpp` (`EntityState::Cell`, `CheckCells`), `SceneRecovery.cpp` (`Cell`), `UndoStack.h/.cpp`
    (observers list);
  - `HierarchyView.h/.cpp` (filter terms), `EditorViewport.h/.cpp` (`CellBounds`), `ViewportRenderer.cpp` (overlay
    function), `GameExport.h/.cpp` (cook state, capacity, order);
  - `EditorSceneCommands.cpp` (`DescribeSceneLoads` world section, `scene.setSettings` field, `scene.unloadAdditive`
    text; function-level).
- `StrataEditor/src/Panels/`: `StreamingPanel.h/.cpp` (World and Physics sections), `SceneHierarchyPanel.cpp` (world
  strip), `InspectorPanel.cpp` (cell line, settings field; function-level), `ViewportPanel.cpp` (chip),
  `AssetInspectorPanel.cpp` (World asset summary with Open and Cook buttons).
- `StrataEditor/src/UI/CellMap.h/.cpp` (new), `EditorLayer.cpp` (pill).
- Tests:
  - `StrataTests/src/World/*` (new), `Physics/PhysicsCapacityTests.cpp`, `Scene/SubSceneTests.cpp`;
  - `Editor/EditorWorldTests.cpp`, `Editor/WorldCookTests.cpp`, `Editor/EditorWorldUITests.cpp`,
    `Scripting/WorldScriptingTests.cpp`;
  - `Perf/WorldPerfTests.cpp`, `Perf/WorldGenerators.h` (until world-io's fixture), `Renderer/GPUWorldTests.cpp`;
  - `TestMain.cpp` (helper modes `world-drive` and `write-world-game`), `Editor/CommandJournalTests.cpp`;
  - `StrataTests/Scripts/World/*` (new module), `StrataTests/Scripts/API/*`, `StrataTests/CMakeLists.txt` (module
    target, CTest `StrataRuntime.World`);
  - `StrataTests/FeatureTest/Assets/Worlds/*` and `Scenes/Feature.stscene`, `FeatureTest/Scripts/WorldFeatures.cpp`,
    `src/FeatureTest/FeatureTestUtils.cpp`;
  - `StrataTests/Perf/Budgets.json`, `StrataTests/Architecture/Layers.json`.
- `Docs/Architecture.md`, `Docs/Manual/Worlds.md` (new), `AGENTS.md`, `.claude/skills/*`.

## Merge notes and conflict risks

Merges **third**, after engine-modules and world-io.

- **engine-modules** (merges first):
  - Rebase onto its per-layer libraries: the `World` entry in `Layers.json` becomes a library without CMake of this
    stream's.
  - Its physics components move to `Physics/PhysicsComponents.h` (include changes in `PhysicsWorld.cpp`, `WorldCook.cpp`).
  - Its soft constraints let `WorldStreamingSystem` say "before Physics when present".
  - **Conflict risk:** both streams edit `SceneSystem.h`. This stream appends one descriptor field
    (`PrepareLoadedScene`) in a block of its own; if engine-modules turns descriptors into an EngineModule interface, the
    hook moves there with the same signature.
  - `RegisterWorldModule` is one line in its module list.
  - New helper modes (`world-drive`, `write-world-game`) list their engine modules, and the new suites fall into a Core
    shard.
- **world-io** (merges second):
  - This stream uses its entity-subset writer, multi-document background save, cancellable reader, binary scene form,
    export transform hook and `WorldFixture.h`.
  - Until its merge, every cell read and write goes through `Editor/WorldCellIO` (one place to switch), and the perf tests
    use `Perf/WorldGenerators.h`. In this stream's rebase onto world-io, switch to world-io's writer, saver, reader and
    fixtures, and keep in `WorldGenerators.h` only what `WorldFixture.h` lacks (World assets, cell grids, cell drives).
  - `GameRuntime.cpp`: keep world-io's "Started '<scene>' in N s" log line, which its runtime gate reads.
  - Shared at function level:
    - `SceneLoader` (`PrepareSystems` and the budget beside its cancellable read path);
    - `SceneSerializer` (the settings field);
    - `GameExport` (`CookingWorlds` before its scene cooking);
    - `PlanAssetPack` (`SceneOrder`);
    - `EditorContext::SaveScene` (the world branch calls its background save).
  - A world's first scan registers thousands of cell documents through its batched registration.
  - `scene.save` becomes deferred with world-io: `Editor.World` tests run it through `Tests::RunCommandToCompletion`.
- **gpu-driven-rendering** (merges fourth):
  - **FramePerfLog:** this stream lands the column groups in gpu-driven-rendering's shape (common.md section 12, area 7
    above); gpu-driven-rendering rebases its restructuring over that commit.
  - `RenderScene::PrepareLoadedScene` gets the budget parameter (one line).
  - **gpu-driven-rendering checks in its rebase** (it merges after this stream, so this is its scope):
    - cell loads and unloads stay within the instance upload budget (25,000 proxies deactivated in one frame by the
      sliced unload);
    - `Perf.World.CellDriveMaxFrame` runs again with GPU culling in a GPU variant;
    - the 20 km pick in `GPU.World` runs through camera-relative instances;
    - `PerfGPU.Editor.PlayStartWorldMaxFrame` holds with its render-scene clone hook.
  - The cell-bounds overlay uses `DebugDraw` only.
- **game-ui** (merges fifth): it appends its host block after this stream's five functions (124 -> its block). No other
  shared file.
- **authoring-and-playtest** (merges last):
  - Its MCP toolsets and the command-kind lists must include `world.create`, `world.info`, `world.setLayers`, `world.edit`,
    `world.moveEntities`, `world.loadCells`, `world.cook`, `world.status` and `physics.stats`.
  - `entity.get {world}` and the bounds queries must work on cell entities (membership is only a component on roots; world
    transforms are the scene's).
  - The Game view plays a world: its renderer draws the running copy, whose streaming is unchanged.
  - Function-level overlaps:
    - `EditorSceneCommands.cpp` (`DescribeSceneLoads`, `scene.setSettings`);
    - `InspectorPanel.cpp` (the streaming settings section, the cell line);
    - `SceneHierarchyPanel.cpp` (the world strip beside its presets);
    - `SceneEdit.h` (`EntityState` gains `Cell`; its per-property prefab overrides may extend it too: union of fields).
  - Its `play.step` and input contract changes touch the command scripts; `FeatureCommands.json.in` must keep its frames
    enough for the tiny world's cells.
- **Unions:**
  - `Feature.stscene` and the feature project (this stream's world beside game-ui's canvas and authoring's prefab
    override);
  - `Budgets.json`, `Layers.json`, `BuiltinModules.cpp`, `CommandJournalTests.cpp`, the skills, `AGENTS.md` and
    `Architecture.md` sections;
  - the status-bar pills in `EditorLayer.cpp` (`DrawStatusBar`).
- **No docked panel is added** (the cell map is a section of the Streaming panel), so `c_LayoutVersion` is not this
  stream's concern.
- **Deferring commands** (for authoring-and-playtest's `MayDefer`, listed in the report's merge notes): `world.cook`,
  `world.loadCells {wait}`, `world.create {open}`, and `scene.save` of a world while a dirty cell still loads.

## Answers from the planner (cross-check of 2026-10-13)

1. **One streaming core with two owners** (`WorldStreamingSystem` in Play and Simulate, `EditorWorld` in Edit): accepted.
   Scene systems keep their play-only lifecycle; engine-modules gives them no edit mode.
2. **FramePerfLog columns:** this stream lands the column groups in gpu-driven-rendering's shape, exactly as common.md
   section 12 declares them (not `AddColumnProvider`). gpu-driven-rendering builds on them in its rebase.
3. **`PrepareLoadedScene` hook:** in `SceneSystemDescriptor`, as specified. engine-modules' EngineModule interface is the
   application's lifecycle and does not replace scene-system descriptors.
4. **`SceneLoadOptions::Prepare` taking `SceneWorkBudget&`, and the one-line change in `RenderScene::PrepareLoadedScene`:**
   accepted; gpu-driven-rendering's spec now names it.
5. **No manifest bump, and only Engine includes World:** confirmed.
6. **Residency info walking every registered asset:** world-io's (common.md section 12), whatever the Roadmap says.
7. **`UndoStack` observers:** this stream may turn the single observer into a list, in a commit of its own; common.md
   section 12 lists `UndoStack.*` as a function-level union with world-io's save points and authoring-and-playtest's
   groups.
8. **`EntityState`:** authoring-and-playtest does not extend it (its overrides live in `PrefabInstanceComponent`, which
   entity snapshots already capture), so `Cell` is the only new field.
9. **Additions from the cross-check:** the default edit layer is "Persistent" (area 4); saves go through world-io's
   request API with UUID roots, reserved derived handles and a `Document` item for the World document; the export's
   capacity goes through `AssetPackCookOptions::AdjustSceneSettings`; no edit to `Strata.h`; Debug suites scaled
   down.
