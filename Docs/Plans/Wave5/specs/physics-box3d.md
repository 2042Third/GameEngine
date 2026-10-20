# Workstream `physics-box3d`: Strata's physics on box3d instead of Jolt (wave 5)

Owner's request (2026-10-20): "I would like to switch to box3d (https://github.com/erincatto/box3d) as its physics
engine". This stream does the physics part; the `showcase-samples` stream builds new sample projects at the same time
and lands first.

AGENTS.md is the source of truth for rules; Docs/Plans/Wave4/mac.md's rules for the machine and perf apply (the Windows
reference machine is away: on the Mac `-L perf` informs, existing `Budgets.json` entries are never changed from Mac
numbers, new metrics are `Provisional`).

## 1. Goal

At the end of the stream:

- box3d is the only physics library: Jolt is gone (submodule, CMake, options, notices, docs, tests that name it).
- Everything physics did on Jolt it does on box3d, behind the same interfaces: `PhysicsSystem`, `PhysicsWorld`, the
  scene services (`ICollisionEventSource`, `IBodyMotionQuery`, `IBodyControl`, `IPhysicsQueries`,
  `ICharacterControl`, `IPhysicsCapacity`) and the components (`RigidBodyComponent`, the four colliders,
  `CharacterControllerComponent`), so scripts, audio, animation, world partition, the editor and the runtime change
  only where they named Jolt's capacity model.
- Every physics test keeps its meaning (tests that named Jolt internals are rewritten, with the reason in their
  commit), the determinism tests hold, scene files of 0.4.0 load unchanged, and the perf tests ran as interleaved A/Bs
  against the Jolt build on this Mac, with ratios in the report.
- The script ABI is unchanged (`ST_SCRIPT_ABI_VERSION` stays 1, no host function changes).

Not the goal: new physics features. box3d brings joints, height fields, a double-precision mode and recordings; each is
a later stream's (section 10).

## 2. Where we start (580eee9e, release 0.4.0)

### 2.1 The Physics module

`Strata/src/Strata/Physics/` (8,828 lines), the only code that includes Jolt (tests name it, and one reads a font from
its submodule: 2.4):

| File | Lines | What it is | Jolt-specific |
| --- | --- | --- | --- |
| `PhysicsWorld.h/.cpp` | 262 / 5,178 | the world: records per entity, body building, hierarchy merging, change lists, sync, write-back, contacts, capacities, growth, queries, stats | about 40% (389 mentions): body interface, shapes, broad phase layers, contact listener, activation listener, state recorder, migration |
| `CharacterControllers.h/.cpp` | 136 / 1,018 | controllers: Jolt `CharacterVirtual` + kinematic presence bodies, parallel moves, pushes on the main thread | the move itself (`ExtendedUpdate`), `Rebind` |
| `PhysicsSystem.h/.cpp` | 212 / 622 | the scene system and the services | none (one comment) |
| `PhysicsRuntime.h/.cpp` | 31 / 93 | reference-counted Jolt lifetime (allocator, factory, trace and assert hooks) | all |
| `PhysicsJobSystem.h/.cpp` | 28 / 210 | Jolt's `JobSystem` on Strata's `JobSystem` | all |
| `PhysicsMeshShapes.h/.cpp` | 51 / 220 | process-wide cache of cooked mesh shapes (Jolt binary state), cooking on jobs | the cooking and the format |
| `PhysicsStateBuffer.h` | 57 | Jolt `StateRecorder` in memory, for migration | all |
| `PhysicsLayers.h` | 30 | user layer to Jolt object layer | all |
| `PhysicsTypes.h` | 189 | `PhysicsSettings`, `PhysicsStats`, `PhysicsCapacityDemand`, `PhysicsMeshProvider` | fields named after Jolt's model (2.3) |
| `PhysicsComponents.h/.cpp` | 87 / 187 | components, reflection, collider gizmos | none |
| `AssetMeshProvider.h/.cpp` | 61 / 117 | mesh data from the asset manager (`MeshGeometryRegistry`) | none |
| `PhysicsRegistration.h/.cpp` | 14 / 25 | `RegisterPhysicsModule` | none |

### 2.2 Users outside the module

- Engine: `Engine/BuiltinModules.cpp` (registration). Scripting and Audio reach physics only through scene services;
  `World/WorldStreamer` reserves room through `IPhysicsCapacity`; `Runtime/GameRuntime` logs `PhysicsBodiesWaiting` and
  `PhysicsMaxBodies` from it; `WorldStreamingSystem` sets `SceneSettings::PhysicsCapacity.MaxBodies` from the cook.
- Editor: `EditorWorldCommands.cpp` (`physics.stats`, `world.cook` applying `RecommendedMaxBodies`), `WorldCook.cpp`
  (`PhysicsWorld::CountBodyCandidates`, `EstimateReservedBytes(ResolveCapacities(...))`), `WorldCellIO.cpp`,
  `SceneBounds.cpp`, `ProjectTemplates.cpp`, `UI/EntityIcons.cpp` (components only), `EditorSceneCommands.cpp`
  (`scene.setSettings`/`getSettings`: `maxBodies`, `maxBodyPairs`, `maxContactConstraints`), `InspectorPanel.cpp` (the
  three capacity rows), `StreamingPanel.cpp` and `EditorLayer.cpp` (the physics section and the "bodies without
  collision" pill), `GameExport.cpp` (cell capacities).
- Scene format: `SceneSerializer.cpp` reads and writes `"PhysicsCapacity": {MaxBodies, MaxBodyPairs,
  MaxContactConstraints}`; cooked scenes carry the same settings JSON (`CookedScene.h`).
- Build: `CMakeLists.txt` (`STRATA_WITH_PHYSICS`, described as Jolt), `Strata/vendor/CMakeLists.txt` (Jolt's options,
  `add_subdirectory(JoltPhysics/Build)`), `Strata/CMakeLists.txt` (`strata_layer_link(Physics PRIVATE Jolt)`, the check
  that `strata_engine_cpu_options` equals Jolt's interface options, Jolt's `JPH_*` definitions on
  `StrataEngineCodegen`), `CMake/StrataCompilerOptions.cmake` (`strata_jolt_instruction_sets`,
  `strata_engine_cpu_options`), zstd's `/arch:AVX2` condition, `.gitmodules`, `ThirdPartyNotices.md` (Jolt 5.6.0).
- CI: "Linux (no Audio, no Physics)" (`-DSTRATA_WITH_PHYSICS=OFF`) builds without the module and its vendor library.

### 2.3 Jolt's model the engine's API shows

- Fixed capacities: `PhysicsSettings::MaxBodies`, `MaxBodyPairs`, `MaxContactConstraints`, `TempAllocatorSize`;
  `SceneSettings::PhysicsCapacity` (three fields, limits 2^21, 2^22, 2^19); automatic sizing (at least 65,536 bodies);
  growth by migration into a new Jolt system (`Grow`, chunks on jobs, old system freed by a background job, character
  `Rebind`, `PhysicsStateBuffer`); bodies waiting for a slot; pair and contact buffers that overflow
  (`ContactOverflowSteps`, `Demand.BodyPairs`); anchor bodies against Jolt's "Out of nodes" abort;
  `EstimateReservedBytes` fitted to the reference machine's commit charge.
- `PhysicsStats::JoltStepTime`; `PhysicsWorld::GetBodyID` and `CharacterContactReport` carry 32-bit Jolt body IDs.
- Per-thread contact buffers: Jolt calls its listener on job threads for every touching pair every step
  (`OnContactPersisted`), so Strata records only wanted pairs.
- Batched adds (`AddBodiesPrepare/Finalize`, `AddBatchCount`), broad phase batches prepared on one thread (a Jolt 5.6
  ordering bug on Apple silicon), wake queries in merged 16 m regions (`WakeQueryCount`).

### 2.4 Tests and perf tests that name Jolt

- `Physics.Lifecycle` "Jolt is initialized while a physics world exists"; `Physics.Capacity` (growth by migration,
  slots, overflowing buffers, the broad phase helper `PhysicsTestUtils.h` "ends where Jolt aborts"); `Physics.Scale`
  ("contacts cost nothing beyond Jolt's step", "Reports from Jolt's job threads go into per-thread buffers");
  `Physics.Simulation` ("Jolt's jobs run on Strata's worker pool", "exhaust Jolt's job pool").
- `Perf.Physics.*.StrataToJoltStep` (3 budgets: 0.6019, 0.4863, 0.741), `PhysicsStats::JoltStepTime` in
  `PhysicsPerfTests.cpp`, "Growing a world of 65,536 bodies to 131,072" (`Perf.Physics.Grow65kMigration`, provisional).
- `Renderer.Font` reads `Strata/vendor/JoltPhysics/Assets/Fonts/Roboto-Regular.ttf`: in its list of vendored TrueType
  fonts, and as the only vendored font with both a `kern` and a `GPOS` table (checked: none of imgui's, tracy's or the
  editor's fonts has a `kern` table), for "Without GPOS, the kern table is read".

## 3. The pin

box3d (MIT, Erin Catto) at **`efca0c1` "Gyro tolerance (#191)", main of 2026-10-15**, 40 commits after `v0.1.0`
(2026-06-30), the newest commit when this spec was written. Why not `v0.1.0`: the commits since are fixes and speed the
engine needs at scale (broad phase and tree performance #159 #179, SAT optimizations #170, solver and gyroscopic fixes
#189 #191, compound fixes #172 #183, the fixes of #175 #187, runtime-dispatched AVX2 kernels #174), and the mover's API
(shape casts for movers, #180) that the character controllers build on is newer than the tag.

Checked on this Mac (scratch clone, compile only, `-std=gnu17 -O3 -ffp-contract=off -Wall -Wextra -pedantic
-Wmissing-prototypes`, every file of `src/`):

| Compiler | Result |
| --- | --- |
| LLVM 16 (stands in for Apple Clang 15), arm64 (NEON) | clean |
| LLVM 16, `-target x86_64-apple-macos13` (SSE2 and the AVX2 kernels behind `#pragma clang attribute`) | clean |
| Apple Clang 21 (this Mac's) | clean |
| GCC 14.4 (Homebrew; CI has 14.2), arm64 | 8 `-Wmaybe-uninitialized` false positives inside box3d's own sources (`replay.c` inlining `b3MakeAABB`, GCC's `arm_neon.h`); none with the wrapper's `-w` |
| The public headers (`box3d.h`, `collision.h`, `math_functions.h`) in C++20, not as system headers: GCC 14 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, LLVM 16 with its libc++ `-Wall -Wextra -Wpedantic -Wshadow` | clean |

MSVC was not checked here: box3d's own CI builds MSVC (`/W4`, warnings as errors), clang-cl and Windows ARM64, and
Strata's CI builds the wrapper on MSVC from the vendoring commit on (D1). If a later main commit is needed (a fix found
while porting), it is a submodule bump of its own, with the same checks.

## 4. Inventory: today on Jolt, and on box3d

For each feature: box3d's equivalent (`b3*` API, read from the vendored headers and docs), or what Strata builds on top.

| Feature | Today (Jolt) | On box3d |
| --- | --- | --- |
| Library lifetime | `PhysicsRuntime`: allocator, type factory, trace and assert hooks, reference counted | `PhysicsRuntime` keeps its contract: the first `Acquire` installs `b3SetAllocator` (aligned allocation through Strata's allocator, thread-safe), `b3SetAssertFcn` (logs `ST_CORE_ERROR` with the condition, file and line, counts it in `PhysicsRuntime::GetAssertionCount()`, breaks only in Debug with a debugger attached) and `b3SetLogFcn` (`ST_CORE_WARN`); the last `Release` restores box3d's defaults. No type registry is needed. |
| Worlds | one `JPH::PhysicsSystem` per `PhysicsWorld` | one `b3WorldId` per `PhysicsWorld`; box3d holds at most `B3_MAX_WORLDS` (128) at once in a static array: `PhysicsWorld` checks `b3GetWorldCount() < b3GetMaxWorldCount()` first, and a scene that finds none plays without physics with one error (never box3d's assert). |
| Jobs | `PhysicsJobSystem` (Jolt `JobSystem` on Strata's) | `PhysicsTaskSystem`: `b3WorldDef::enqueueTask` submits a Strata job (pooled handles: box3d starts at most `B3_MAX_TASKS`, 256, per step), `finishTask` waits for it (Strata's `Wait` runs a job that has not started itself, so a step never deadlocks, even with every worker busy); `workerCount` = the job system's workers plus the main thread, at most `B3_MAX_WORKERS` (32), 1 without a job system. `PhysicsStats::JobCount`/`WorkerJobCount` stay. |
| Bodies, motion types | `BodyCreationSettings`, `EMotionType` | `b3BodyDef` + `b3CreateBody`: `b3_staticBody`, `b3_kinematicBody`, `b3_dynamicBody`; `userData` = Strata's record slot. |
| Box collider | `BoxShape` (convex radius 0.05) | hull: `b3MakeTransformedBoxHull` / `b3MakeScaledBoxHull` (exact non-uniform and mirrored scale, offset and rotation baked), `b3CreateHullShape`; sharp corners (box3d's linear slop 5 mm, no convex radius). |
| Sphere collider | `SphereShape` | `b3CreateSphereShape` with the offset as its center; radius scaled by the largest absolute axis scale (as today). |
| Capsule collider | `CapsuleShape` | `b3CreateCapsuleShape` (two centers in body space); radius by the larger of `\|X\|` and `\|Z\|`, half height by `\|Y\|` (as today). |
| Several colliders | `StaticCompoundShape` of rotated/translated/scaled children | several shapes on one body (box3d's runtime compound; dynamic and kinematic too); box3d's baked compound (`b3CreateBakedCompoundShape`) is static only and used only for an offset mesh collider (next row). |
| Mesh collider, convex | `ConvexHullShape` cooked from the positions | `b3CreateHull(points, count, maxVertexCount)` cooked on jobs (at most `B3_MAX_HULL_VERTICES`, 128: larger point clouds are reduced to 128 vertices; a hull that cannot be built is "unusable" with a warning, as today), then `b3CreateTransformedHullShape` with the collider's scale and local transform. |
| Mesh collider, triangles (static only) | `MeshShape` cooked from `PhysicsMeshData` | `b3CreateMesh` (its tree) cooked on jobs and cached per data object; `b3CreateMeshShape(body, def, mesh, scale)` (scale includes mirroring); a mesh collider placed away from its body's origin (a child of a static rigid body) becomes a one-mesh baked compound (`b3CompoundMeshDef` with transform and scale; the triangles are shared, not copied). box3d creates mesh contacts only on static bodies, which is Strata's rule already. |
| Mesh shape cache | `PhysicsMeshShapes`: Jolt binary state per data object, restored per world | same class, same contract: holds `b3MeshData*` and `b3HullData*` (owned, freed with `b3DestroyMesh`/`b3DestroyHull` when the data object goes), shared by every world (box3d's mesh shapes reference the data). |
| Mass | `MassPropertiesOverride` from `RigidBodyComponent::Mass` | shapes with density 1 and `updateBodyMass = false`, then `b3Body_ApplyMassFromShapes`, `b3Body_GetMassData`, scaled to `Mass` (mass and rotational inertia), `b3Body_SetMassData`. |
| Friction, restitution | per body, combined `sqrt(a*b)` and `max` | `b3ShapeDef::baseMaterial` (`b3SurfaceMaterial` friction, restitution); box3d's defaults combine the same way. |
| Damping, gravity scale | body settings | `b3BodyDef::linearDamping`, `angularDamping`, `gravityScale`; `b3Body_SetGravityScale`. box3d damps by `1/(1 + h c)` per sub-step where Jolt multiplied by `1 - h c`: tests compare with their tolerance (5.3). |
| Rotation locks (world axes) | `EAllowedDOFs` | `b3MotionLocks::angularX/Y/Z`, which box3d applies to the world angular velocity (`solver.c`): world axes, as today. |
| Continuous collision | `EMotionQuality::LinearCast` | box3d always sweeps dynamic bodies against static ones (`enableContinuous`); `ContinuousCollision` sets `isBullet` (sweeps against dynamic bodies too; bullets do not sweep against each other). |
| Collision steps | `PhysicsSettings::CollisionSteps` Jolt collision steps per update | the fixed step runs `CollisionSteps` box3d steps of `dt / CollisionSteps`, each with box3d's 4 sub-steps (`c_SolverSubSteps`); default 1. |
| Gravity | `SetGravity` | `b3World_SetGravity` (wakes nothing by itself: Strata wakes the awake-able bodies, as today). |
| Collision layers | object layers `layer * 2 + moving`, broad phase layers, pair filter from the 32x32 matrix | `b3Filter`: `categoryBits = 1 << layer`, `maskBits = matrix row of the layer \| c_QueryCategory` (bit 63). Two shapes collide only if both masks allow it (`b3ShouldShapesCollide`: the matrix's rule). A matrix change sets the filter of every shape (`b3Shape_SetFilter`, rare, O(shapes)) and wakes the bodies. Static bodies are in box3d's static tree, which only moving proxies are tested against. |
| Query layer masks | `ObjectLayerFilter` on the user layer | `b3QueryFilter{ categoryBits = c_QueryCategory, maskBits = layerMask }`: every shape carries the query bit, so a body on a layer that collides with nothing is still found by queries. |
| Triggers | sensors (`IsSensor`) | `isSensor` shapes with `enableSensorEvents`; every other shape gets `enableSensorEvents` (visitor). box3d sensors see static, kinematic and dynamic convex visitors, never shapes of their own body; **visitors must be convex**: a trigger does not report a static triangle-mesh collider (Jolt's did; documented difference, pinned by a test). Events: `b3World_GetSensorEvents` (begin/end, deterministic order). |
| Contacts and collision events | `ContactListener` on job threads, per-thread buffers, tracked pairs only for listeners | `b3World_GetContactEvents` after the step (begin/end, on the main thread, deterministic). box3d fixes a contact's event flag when the contact is created (`contact.c`), so every non-sensor shape enables contact events (cost: one event per touch change, not per contact per step, unlike Jolt's persisted callbacks) and Strata keeps only wanted pairs. A pair that becomes wanted while touching is seeded from `b3Body_GetContactData` (touching manifolds) and begins at the next dispatch, as today ("tracked from the first step that reports it"). Begin's point and normal come from `b3Contact_GetData`; End carries the last point read (at Begin, refreshed each step for tracked pairs with an awake body). The per-entity pair lists (constant-time add and remove, `PairIndexOperations`) stay. |
| Activation, write-back | `BodyActivationListener`, active body list, body locks | `b3World_GetBodyEvents` move events (bodies that moved, with `userData` and the transform, `fellAsleep`); Strata keeps its order (the bitmap over body indices, parents first, parallel chunks for bodies without a dynamic ancestor, the rare cases on the main thread). |
| Sync before the step | `BodyInterface::SetPositionAndRotation`, `MoveKinematic` | dynamic: `b3Body_SetTransform` (teleport) and waking the bodies it touches (`b3Body_GetContactData`) and, at the new place, a tree query (`b3World_OverlapAABB`, counted in `WakeQueryCount`); kinematic: `b3Body_SetTargetTransform(target, dt, wake)`; static: `b3Body_SetTransform` and waking what touches it. |
| Leaving the simulation | `RemoveBodies` in one batch, wake queries in 16 m regions | inactive entities: `b3Body_Disable` / `b3Body_Enable` (the body keeps its ID and shapes); destroyed: `b3DestroyBody`. Both wake the bodies they touched (`b3DestroyBodyContacts(..., wakeBodies = true)`), so the region queries go; "what rested on them wakes" stays tested. |
| Sleeping, waking | `IsActive`, `ActivateBody` | `b3Body_IsAwake`, `b3Body_SetAwake`; box3d's island sleep (0.5 s, the same as Jolt's default). |
| Body API | forces, impulses, velocities, point velocity, teleport | `b3Body_ApplyForce(ToCenter)`, `ApplyTorque`, `ApplyLinearImpulse(ToCenter)`, `ApplyAngularImpulse`, `Set/GetLinearVelocity`, `Set/GetAngularVelocity`, `GetWorldPointVelocity`, `SetTransform`; forces last one step (box3d clears them after the step, as Jolt did). |
| Raycasts | `CastRay` with collectors, triggers skipped, ignored entity, inside starts ignored | `b3World_CastRay` with a callback (skips sensors unless asked, the ignored entity, entities pending destruction; returns the fraction for the closest hit); `RaycastAll` keeps each body's closest hit and sorts by distance. `maxDistance` clamped to 1e5 (= `B3_HUGE` at one unit per meter). Rays starting inside a convex collider must not hit it: if box3d reports a zero-fraction hit there, the callback ignores it (test). |
| Overlaps | `CollideShape` with sphere and rotated box | `b3World_OverlapShape` with a `b3ShapeProxy` (a point with the radius; the box's 8 rotated corners), shapes to bodies to entities, de-duplicated, in the deterministic order of today. |
| Character controllers | `CharacterVirtual` + `ExtendedUpdate` (slide, stairs, stick to floor), presence bodies, pushes on the main thread | Strata's own mover on box3d's geometric mover API (`b3World_CastMover`, `b3World_CollideMover`, `b3SolvePlanes`, `b3ClipVector`; capsule only, experimental in box3d): see 6.4. Presence bodies, pushes (`PushBodies`), parallel moves (box3d's queries are read-only and may run on many threads, `docs/faq.md`) and contacts stay. |
| Capacities | fixed buffers, migration growth, overflow | reservations and growth in place: see 6.5. |
| Parallel body creation | Jolt bodies built on jobs, IDs given in record order | box3d's creation API is single-threaded: jobs prepare everything that costs (merged collider lists, transforms, box hulls, hull cooking, mass data, body and shape definitions) in fixed chunks; the main thread creates bodies and shapes in record order (so IDs and results do not depend on the thread count, `ParallelBuildCount`). Static shapes are created with `invokeContactCreation = false`. |
| Prepared physics (loaded scenes, streamed worlds) | `PhysicsBuildMode::Sliced`, `PrepareLoadedScene`, `Reserve` | unchanged contract: a prepared world is a box3d world built in slices before the scene runs; `Reserve` (6.5). |
| Body IDs for tests | 32-bit Jolt ID (index, sequence) | `uint64_t`: `generation << 32 \| index1` of the `b3BodyId` (without the world index, so two worlds built alike compare equal); `c_NoBodyID = UINT64_MAX`. |
| Determinism | Jolt deterministic per platform | box3d is deterministic across thread counts and, compiled with `-ffp-contract=off`, across platforms; Strata's glue keeps its ordering rules. |
| Large coordinates | single precision | single precision (`BOX3D_DOUBLE_PRECISION` off; `B3_HUGE` = 1e5 m): a body whose world position is beyond ±1e5 waits like a degenerate transform, with a warning, instead of tripping box3d's asserts (the world tests' 20 km stays well inside). |
| `physics.stats`, Streaming panel, pill | capacity fields of Jolt | same command and views; fields follow 6.5 (`maxBodyPairs`, `contactOverflowSteps`, `demand.bodyPairs` go; `growthLimit` comes). |
| Script API, gizmos, collider presets, templates | through services and components | unchanged. |
| Feature test | physics, collision and character scripts | unchanged scripts and scene; must pass on box3d with `c_ExpectedLogMessages` unchanged. |
| Build without physics | `STRATA_WITH_PHYSICS=OFF`: no Physics library, no Jolt | the same, without box3d. |
| `strata_engine_cpu_options` | mirrors Jolt's options for every engine object, checked against Jolt's | becomes Strata's own (6.7): the same flags for the same configurations, options `STRATA_USE_*` of Strata's, the Jolt check and `JPH_*` definitions gone. |

## 5. Plan of the replacement

### 5.1 A temporary backend switch

The port lands in steps that each build and pass. While it runs, a CMake option selects the backend:

- `STRATA_PHYSICS_BACKEND` (`Jolt` default, `Box3D`), defining `ST_PHYSICS_BOX3D=0/1` for the Physics layer; the
  Physics layer links Jolt or box3d; the vendor wrapper builds what the option needs.
- box3d's implementation lives in new files (`PhysicsWorldBox3D.cpp`, `CharacterControllersBox3D.cpp`,
  `PhysicsRuntimeBox3D.cpp`, `PhysicsMeshShapesBox3D.cpp`, `PhysicsTaskSystem.h/.cpp`, `PhysicsFilters.h`), each whole
  file inside `#if ST_PHYSICS_BOX3D`; the Jolt files get `#if !ST_PHYSICS_BOX3D`. They implement the same classes, so
  nothing outside the module knows which runs. The Strata glue of `PhysicsWorld.cpp` (records, merging, change lists,
  pending reasons, mesh waits, ordering, pair lists) is carried into the box3d file and reworked there, not shared
  through a new abstraction that would be deleted with Jolt.
- A second build directory, `build/macos-jolt` (Debug and Release), holds the Jolt backend once the default flips;
  before the flip `build/macos` is Jolt and `build/macos-box3d` box3d. Both are deleted when Jolt is removed (disk:
  under 5 GB together).
- Tests that only make sense for one backend (Jolt's migration and overflow cases; box3d's golden hash) are compiled for
  that backend only, until the removal commit rewrites or drops them (5.3).

When the box3d backend passes every suite in Debug and Release and the A/Bs are measured (D9), one commit makes box3d
the default and the next removes Jolt; the box3d files then take the plain names (`PhysicsWorld.cpp`, ...).

### 5.2 Interfaces kept

- `PhysicsWorld`'s and `PhysicsSystem`'s public functions, the six scene services and their types
  (`Scene/SceneServices.h`), the components and their reflection, `PhysicsMeshProvider`, `PhysicsBuildMode`,
  `PreparedPhysicsWorld`, `PhysicsSystem::PrepareLoadedScene`, `CountBodyCandidates`.
- Changes, all from Jolt's model, with every user: what both backends can implement lands with the switch (D2: the
  step time's neutral name, the 64-bit body IDs), what only Jolt needed goes with it (D11):
  - `PhysicsSettings`: `MaxBodyPairs` and `TempAllocatorSize` go (box3d has no pair buffer and sizes its own arena);
    `MaxBodies`, `MaxContactConstraints` and `GrowthLimit` stay with the meanings of 6.5.
  - `PhysicsCapacitySettings` (scene): `MaxBodyPairs` goes (6.6).
  - `PhysicsCapacityDemand`: `BodyPairs` goes.
  - `PhysicsStats`: `JoltStepTime` becomes `LibraryStepTime` (the physics library's own step; D2); `MaxBodyPairs`,
    `ContactOverflowSteps` go;
    `SharedShapeCount` counts the box hulls the world shares by size; `GrowthLimit` comes; `AddBatchCount` counts
    rounds of changes that created bodies (box3d adds bodies one by one; the test's meaning, one round per update,
    holds).
  - `PhysicsWorld::GetBodyID` and `CharacterContactReport`/`CharacterBodyChange` IDs become `uint64_t` (4).
  - `CharacterControllerSet` takes the box3d world (internal header); `Rebind` and `PhysicsStateBuffer` go.

### 5.3 Tests

Every suite keeps its cases' meaning. Cases that named Jolt internals change, each with its reason in the commit:

| Case | Change | Why |
| --- | --- | --- |
| `Physics.Lifecycle` "Jolt is initialized while a physics world exists" | "The physics library's hooks are installed while a physics world exists" | `PhysicsRuntime` installs box3d's allocator, assert and log hooks |
| `Physics.Capacity` "A full world grows before its next step, beyond a capacity the scene gives with a warning" | "A world grows past its reserved room in place, without a warning, and remembers it" | box3d grows its storage while bodies are created; nothing is full below the growth limit |
| "Growth keeps every body's ID, motion, sleep and touching pairs" and "A walking character controller keeps ... across a growth" | kept, growth in place | still the promise; now trivially true, and must stay so |
| "Bodies waiting for a slot cost the steps nothing and get freed slots in the order they started waiting", "At its growth limit a full world keeps bodies waiting", "Rebuilding a body in a full world keeps the body and its motion" | kept, at `GrowthLimit` | only the growth limit makes bodies wait |
| "Steps that overflow the contact buffers are counted, warned about once and remembered" | "Contacts beyond the reserved count are counted in the demand and reserved by the scene's next world" | box3d has no contact buffer to overflow |
| "The reserved bytes of a world follow its capacities" | "The reserved bytes are box3d's count of the world's bytes plus Strata's records, and `EstimateReservedBytes` is within 15% of them" | box3d reports exact bytes (`b3World_GetCounters().byteCount`); deterministic, runs everywhere |
| "Bodies spawned and removed again before every step never exhaust the broad phase" | kept in its helper process | box3d's tree must not leak either; the helper's comment no longer names Jolt |
| `Physics.Scale` "Bodies created in one frame join the simulation in one batch per activation mode" | "... are created in one round of changes" | no batch API in box3d |
| `Physics.Scale` "Contact events do not depend on how many threads report them" | kept | box3d's events are produced in a deterministic order |
| `Physics.Simulation` "Mass and damping shape the response ..." | tolerance re-derived from box3d's damping formula, never widened beyond what the formula needs | different, documented damping integration |
| `Renderer.Font` (Jolt's Roboto-Regular) | the font leaves the list; the kern case appends a `kern` table (one format-0 subtable with an A-V pair) to imgui's `Roboto-Medium.ttf` with `GPOS` renamed, in the test | the only vendored font with a `kern` table goes with Jolt; the case still checks that the kern table is read and that a bad pair count disables kerning |
| `Perf.Physics.*` "StrataToJoltStep" | `StrataToLibraryStep` (provisional), from `LibraryStepTime`, from D2 on | the denominator is whichever library runs; box3d's differs from the Jolt step the budgets measured |
| `Perf.Physics` "Growing a world of 65,536 bodies to 131,072" | measures the frame that creates 65,536 more bodies in a world reserved for 65,536 (`Perf.Physics.Grow65kMaxFrame`, provisional, target 16.7 ms) | growth happens inside box3d while bodies are created |

New tests (each must fail without its code; 5.4):

- `Physics.Box3D` "box3d's cross-platform determinism scenario matches its golden hash": compiles box3d's own scenario
  (`shared/determinism.c`, `human.c`, `utils.c`, from the vendor directory, into a test-only library; nothing copied)
  and checks box3d's float golden values (sleep step 291, hash `0xFF4C605B`) with 1 worker and with Strata's task
  system on 4: proves the wrapper's floating-point flags and the task adapter on every CI compiler and instruction set.
- `Physics.Box3D` "box3d's asserts and logs reach Strata's log and count", "A scene that finds no free box3d world plays
  without physics, with one error" (a helper process holds 128 worlds), "Bodies beyond ±1e5 wait like degenerate
  transforms".
- `Physics.Query` "Bodies on a layer that collides with nothing are still hit by queries" (the query bit).
- `Physics.Event` "A watch begun while bodies touch reports Begin at the next step and End when they part" (seeding);
  "Triggers report convex colliders and characters, not triangle meshes" (the documented difference).
- `Physics.MeshCollider` "A convex collider of a mesh with more than 128 hull vertices is reduced, not refused"; "A mesh
  collider offset from its static body is placed where its entity is".
- `Physics.Capacity` "IPhysicsCapacity reports box3d's growth: reserved room, growths, the growth limit".
- `Physics.Character` (6.4) "Characters stick to the ground walking down slopes and steps", "Characters ride moving
  platforms" and "Characters slide down steep ground", if not already covered by the existing cases.

### 5.4 Must-fail checks

Each is run once on the Mac by breaking the code locally, and the report quotes the failure:

| Check | Break | Fails |
| --- | --- | --- |
| Floating-point flags | build box3d without `-ffp-contract=off` (arm64 clang contracts by default) | the golden hash case |
| Task adapter | return from `finishTask` without waiting for the job | the golden hash case with 4 workers (the step reads results not yet written) |
| Query bit | drop `c_QueryCategory` from the shapes' masks | "Bodies on a layer that collides with nothing are still hit by queries" |
| Contact seeding | do not seed wanted pairs from `b3Body_GetContactData` | "A watch begun while bodies touch ..." |
| Contact filter | track every box3d contact event | `Physics.Scale` "Contacts are tracked only while someone wants them" |
| Body ID order | create bodies in job order | `Physics.Capacity` "Bodies built on the job threads get the same IDs ..." |
| Write-back order | write children before parents | `Physics.Simulation` "Parented dynamic bodies write their world transform through the hierarchy" |
| Character determinism | push bodies from the move jobs | `Physics.Character` "Characters move the same with any number of threads" |
| World limit | create the 129th world without the check | "A scene that finds no free box3d world ..." (box3d asserts) |
| Mass override | keep box3d's density mass | `Physics.Simulation` "Mass and damping shape the response ..." |
| Font kern case | append the kern table with a pair count too large | the kern case's `checkKerningDisabled` |

## 6. Design decisions

### 6.1 The vendor wrapper (D1)

`Strata/vendor/CMakeLists.txt` builds `box3d` itself, like zstd, instead of `add_subdirectory(box3d)`: box3d's
top-level file sets a `FETCHCONTENT_BASE_DIR` cache entry inside the source tree, `CMAKE_EXPORT_COMPILE_COMMANDS`,
install rules and a debug postfix, and its `src/CMakeLists.txt` adds `/W4` (MSVC then warns that `/W4` overrides the
vendor's `/W0`) and uses the including project's version. The wrapper:

- `add_library(box3d STATIC)` from `box3d/src/*.c` (globbed with `CONFIGURE_DEPENDS`), C17 with extensions (box3d's
  `_Static_assert`, anonymous unions), `SYSTEM PUBLIC` include `box3d/include`, `PRIVATE` `box3d/src`, `m` on Linux;
  vendor warnings off (`-w`, `/W0`), only when `STRATA_WITH_PHYSICS` is on;
- `-ffp-contract=off` on GCC and Clang (`/clang:-ffp-contract=off` for clang-cl): the one transform that changes
  results, which box3d's determinism needs (its top-level CMake does the same);
- MSVC x64 (not clang-cl): `wide8.c` with `/arch:AVX2` (box3d dispatches to its AVX2 kernels at run time, `simd.c`);
  GCC and Clang compile them through `#pragma GCC target` / `clang attribute`;
- `B3_ENABLE_ASSERT` in Debug and Release (PUBLIC, so box3d's inline functions in Strata's objects agree), none in Dist,
  like `ST_CORE_ASSERT`; `BOX3D_VALIDATE` off (box3d's heavy validation makes Debug test runs several times slower; the
  golden hash and the physics suites cover correctness);
- `BOX3D_DOUBLE_PRECISION` off.

`ThirdPartyNotices.md` gets box3d (MIT, `efca0c1`, "Yes", Physics) in the vendoring commit; Jolt's row goes in D11.

### 6.2 Bodies and shapes (D4)

- The body's frame is its owner entity's world position and rotation; each collider's place relative to it (offset,
  rotation, scale, mirroring) is baked into its shape as today. Shear stays approximated by the decomposed scale.
- Box hulls are made per size and kept per world (`SharedShapeCount`); spheres and capsules are values; cooked hulls
  and meshes come from the process-wide cache.
- Records keep their slots; `BodySlot` maps box3d's body index (`index1 - 1`) to the record in constant time.
- Non-finite and degenerate inputs never reach box3d (`Physics.Lifecycle` "Invalid component data never produces
  non-finite state"), and neither do positions beyond ±1e5.

### 6.3 Contacts, triggers, events (D5)

As in the table (4). Event order: box3d's begin and end events and sensor events come in a deterministic order; Strata
dispatches in its existing order (End events of removals first, then the step's events by pair), and the per-entity
pair lists keep their constant-time updates. Characters' contacts join the pairs as today.

### 6.4 Character controllers on box3d's mover (D7)

Behavior is what `CharacterControllerComponent`, `ICharacterControl` and the `Physics.Character` tests say; Jolt's
`ExtendedUpdate` is replaced by Strata's move, per controller and fixed step:

1. Velocity: the walk velocity, a pending jump, root motion, gravity times `GravityScale`; on walkable ground it adds
   the ground's velocity at the contact point (`b3Body_GetWorldPointVelocity`; moving platforms).
2. Slide: up to 4 iterations of `b3World_CastMover` (capsule from the entity's origin: centers at `Radius` and
   `Height - Radius`, a sphere when `Height < 2 Radius`), moving the safe fraction, `b3World_CollideMover` gathering the
   planes (a `b3MoverFilterFcn` and the plane callback skip sensors, the entity's own bodies and its presence body, and
   use the world's live layer matrix), `b3SolvePlanes`, `b3ClipVector` on the velocity. The capsule and the planes are
   relative to an origin at the character (box3d's advice for precision far from the world origin).
3. Steps: when the slide is blocked while on walkable ground, try up by `StepHeight`, across, and down; keep it if it
   lands on walkable ground and got further.
4. Stick to the ground: when it was on walkable ground and is not rising (no jump), cast down by `StepHeight` and snap.
5. Ground state from the planes below: walkable (angle to up at most `MaxSlopeDegrees`) is `OnGround`, only steeper ones
   `OnSteepGround` (it slides down), planes only to the side or above `NotSupported`, none `InAir`; `GroundEntity`,
   `GroundNormal`, `GroundVelocity` from the supporting plane's shape.
6. The moves run in parallel batches (read-only queries); pushes (`PushBodies`, unchanged formula), presence bodies
   (kinematic capsule of 90% of the radius, `b3Body_SetTargetTransform`) and the entities' translations are applied on
   the main thread in entity order, so results do not depend on the thread count.

### 6.5 Capacities on box3d (D6)

box3d grows its own storage (bodies, shapes, contacts, tree nodes, arena) as needed; `b3WorldDef::capacity`
(`b3Capacity`: static and dynamic body and shape counts, contacts) only reserves it up front, and
`b3World_GetMaxCapacity` reports the most a world held. So:

- `MaxBodies` and `MaxContactConstraints` (settings and scene) are the **room a world reserves** when it is created:
  0 sizes them from the scene (`ResolveCapacities`: bodies the next power of two of twice the body candidates, at least
  1,024; contacts two per body; each at least the remembered demand), split into static and moving counts by the
  candidates' types. Exceeding the room is no error and no warning: box3d grows, the world counts a growth
  (`GrowthCount`, `LastGrowthFrom/To`: the room doubled; `LastGrowthMs`: the round of changes that grew it) and records
  the demand, so the scene's next world reserves enough (`GetRememberedCapacityDemand`, per lineage as today).
- `GrowthLimit` (default the engine's limit, 2^21 bodies, and box3d's `B3_MAX_SHAPES`, 2^22, for shapes) is the only
  place bodies wait: beyond it they queue in the order they failed, get freed room first, and are reported in one error
  (`WaitingForSlot`, the pill, `Perf.World.PhysicsBodiesWaiting`).
- `Reserve(bodies)` (the world streamer's) reserves Strata's record storage at once and raises the room and demand, so a
  streamed world's next world starts large enough; it cannot pre-grow a running box3d world (its API has no reserve), so
  the cost of box3d's growth while a cell loads is measured (`Perf.Physics.Grow65kMaxFrame`, `Perf.World.*`).
- `PhysicsCapacityInfo::ReservedBytes` is exact: `b3World_GetCounters().byteCount` plus Strata's records;
  `EstimateReservedBytes(settings)` (the world cook's report, before any world exists) uses per-body and per-contact
  constants fitted to box3d's counts, tested within 15%.
- Gone: migration, `Grow`'s system swap, `Rebind`, `PhysicsStateBuffer`, the anchor bodies, the pair and contact
  overflow counts, the broad phase's single-thread preparation. `PhysicsWorld::Grow` stays as "apply the pending
  changes and reserve what `Reserve` asked for" (its callers and tests keep working), returning whether the room grew.

### 6.6 Scene files and commands (D11)

- Components unchanged: no migration of entities.
- `"PhysicsCapacity"`: `MaxBodies` and `MaxContactConstraints` are read and written as before (their meaning is room,
  6.5); `MaxBodyPairs` in 0.4.0 files is accepted and ignored (no warning: 0.4.0 wrote it in every scene), and no longer
  written. 0.4.0 reading a 0.5.0 file sees no `MaxBodyPairs` and sizes it automatically, so files stay compatible both
  ways. Cooked scenes carry the same JSON.
- `scene.setSettings` drops `maxBodyPairs` from its schema and rejects it with `InvalidParameters` naming the reason
  (box3d has no pair buffer); `scene.getSettings` and `physics.stats` drop it; `physics.stats` drops
  `contactOverflowSteps` and `demand.bodyPairs` and adds `capacity.growthLimit`. The Inspector loses the Max Body Pairs
  row. The automation skill and the guide say so.

### 6.7 Build options (D2, D11)

- `strata_engine_cpu_options` stays the engine's code generation, now Strata's own: the same flags for every
  configuration it produces today (x86-64: `/arch:AVX2` on MSVC with the Visual Studio generators, `-mavx2 -mbmi
  -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse` on GCC and Clang; nothing on ARM64; `-pthread` off Windows), from cache
  options `STRATA_USE_SSE4_1 ... STRATA_USE_FMADD` with Jolt's former defaults (`strata_jolt_instruction_sets` becomes
  `strata_cpu_instruction_sets`). So no engine object's code changes with the port (checked by comparing the compile
  flags of a non-physics translation unit in `compile_commands.json` before and after). Jolt's `USE_*` cache names no
  longer apply (the guide's Building section says so).
- box3d is compiled with its own flags (6.1), not `StrataEngineCodegen`'s, so its results do not depend on the engine's
  instruction sets.
- `STRATA_WITH_PHYSICS` keeps its meaning ("Build the physics module (box3d)"); the no-physics CI job is unchanged.

## 7. Deliverables and acceptance

Each deliverable is one or more commits that build and pass (default backend) with the lint clean; items D3-D8 pass
their suites on the box3d build too.

**D1 Vendor box3d.** Submodule `Strata/vendor/box3d` (`shallow = true`) at `efca0c1`, the wrapper (6.1), the notices
row; a commit of its own. *Accept:* the `box3d` target builds in Debug and Release on the Mac with no diagnostics; CI's
six jobs green on the branch (MSVC, GCC 14.2, Apple Clang 15 build it; the no-physics job does not).

**D2 Backend switch and Strata's CPU options.** `STRATA_PHYSICS_BACKEND`, `ST_PHYSICS_BOX3D`, the Physics layer's link,
`strata_cpu_instruction_sets`. *Accept:* both backends configure; the default build compiles every engine object with
the flags it had (`Scene.cpp`'s compile command identical to 580eee9e's but for the new definition); `Architecture.*`
and the physics suites pass on the default build; `PhysicsWorld::GetBodyID` returns 64-bit IDs.

**D3 Runtime, tasks, mesh cooking (box3d).** `PhysicsRuntime`, `PhysicsTaskSystem`, `PhysicsMeshShapes` on box3d, the
golden hash case. *Accept:* `Physics.Box3D` (golden hash with 1 and 4 workers, hooks, assert count),
`Physics.AssetMesh` and the cooking cases pass on the box3d build.

**D4 Bodies, shapes, sync, write-back, layers, queries (box3d).** *Accept:* `Physics.Simulation`, `Physics.Lifecycle`,
`Physics.Hierarchy`, `Physics.Query`, `Physics.Layer`, `Physics.MeshCollider` pass on the box3d build in Debug, with
their tolerances unchanged except where 5.3 says why.

**D5 Contacts and triggers (box3d).** *Accept:* `Physics.Event`, `Physics.Scale`, `Scripting.Collision*` pass; the
seeding and filter must-fail checks fail as stated.

**D6 Capacities (box3d).** 6.5. *Accept:* `Physics.Capacity` (rewritten cases of 5.3), `Physics.Prepare`,
`World.Streaming*`, `World.Streamer*`, `Editor.World*` pass; `physics.stats` adds `capacity.growthLimit`.

**D7 Character controllers (box3d).** 6.4. *Accept:* every `Physics.Character` case and the script character cases pass
on the box3d build, and the determinism case with 1, 2 and 4 threads.

**D8 Whole engine on box3d.** *Accept:* `ctest --preset macos-debug -LE perf` and `--preset macos-release -LE perf` on
the box3d build pass every test (counts in the report), the feature test included, with no new expected log messages.
CI keeps building the default backend (Jolt) until the flip; box3d's backend reaches CI's six jobs with D10.

**D9 Perf A/B.** Release builds of the same commit, box3d against Jolt, interleaved, at least 3 pairs each, under
`Tools/PerfLock.py --label physics-box3d`, ratio of medians (box3d / Jolt):

| Metric (test case) | Acceptance |
| --- | --- |
| `Perf.Physics.Boxes10k.ActiveFrame`, `Boxes30kTracked.ActiveFrame`, `Boxes70k.ActiveFrame` | at most 1.10 |
| `*.StrataStepTime` (Strata's part of a step) | at most 1.00 (the glue gets simpler) |
| `Boxes30k.StartAnd30Frames` | at most 1.10 |
| `Create10kBodies`, `Create100kStaticBodies` | at most 1.10 (creation is serial on box3d) |
| `Characters1000Step` | at most 1.25 (new controller code) |
| `MostBodies.ReservedMemory`, `LargestCapacities.ReservedMemory` | at most 1.00 |
| `Boxes30k.DroppedContactWarnings`, `Boxes70k.BodyErrors` | 0 |
| `Perf.World.CellUnloadMaxFrame`, `CellDriveMaxFrame`, `PhysicsBodiesWaiting`, `PerfGPU.World.CellDriveMaxFrame` | at most 1.10, waiting 0 |
| `Perf.Physics.Grow65kMaxFrame` (new) | Mac value reported against its 16.7 ms target |

A metric over its limit is profiled (box3d's own `b3World_GetProfile` parts against Strata's) and, where Strata's part
is the cause, fixed; where box3d's is, its settings (worker count, sub-steps) are tried and the result reported. New
metrics get provisional entries (`StrataToLibraryStep` for the three cases, `Grow65kMaxFrame`); existing entries keep
their values (7.1).

**D10 Flip.** box3d becomes the default backend. *Accept:* the full Debug and Release suites as D8 on the default
build; CI green.

**D11 Jolt removed.** The Jolt files, `PhysicsStateBuffer`, `PhysicsLayers.h`, the switch, the submodule, Jolt's vendor
block and checks, `JPH_*` definitions, the notices row; the API changes of 5.2 and 6.6 with every user (editor
commands, Inspector, panels, tests, perf tests); the font test; box3d files renamed to the plain names. *Accept:*
`git grep -i jolt` finds only history (`Docs/Releases/0.3.0.md`, `0.4.0.md`, the Roadmap's past waves, this spec);
full suites in Debug and Release; lint clean; CI green on all six jobs.

**D12 Docs and skills** (with the commits that change behavior, section 8). *Accept:* `Docs.AgentGuide` passes
(AGENTS.md under 40,000 characters); the docs describe box3d and no Jolt behavior.

**D13 Samples.** After `showcase-samples` lands on main and is merged into this branch: its samples' regression command
scripts (and `Editor/TetrisCommands.json.in`) pass on box3d. *Accept:* they pass; every gameplay difference box3d's
tuning causes (bounces, friction, character feel) is listed in the report, and fixed in the engine when it is a defect.

### 7.1 Budgets

- Untouched: every existing entry's `Measured` and `Budget`.
- The three `Perf.Physics.*.StrataToJoltStep` entries measure Strata's part against Jolt's step: in D2 the metric
  becomes `Perf.Physics.*.StrataToLibraryStep`, provisional (unit `ratio`), and the three entries are removed with the
  name they measured (not raised, not lowered); D9 reports both backends' Mac values.
- `Perf.Physics.Grow65kMigration` (provisional, no measurement) becomes `Perf.Physics.Grow65kMaxFrame` (provisional,
  target 16.7). The Roadmap's reference-machine list names all four for the reference machine.

## 8. Docs and skills

- `AGENTS.md`: the opening line (box3d for physics); the Worlds digest's physics rule ("bodies wait for a slot ... and
  the world grows between steps" becomes: bodies wait only at the growth limit, never polled; below it box3d grows
  while bodies are created); a Physics rule only if one is new (box3d's API is single-threaded outside its step and
  its read-only queries; its asserts route to Strata's log); details go to the guide. Stays under 40,000 characters.
- `Docs/Architecture.md`: the Physics paragraph (box3d world, tasks, shapes, filters, events, capacities as room,
  growth in place), Character controllers (Strata's mover on box3d), the layer table's vendor row, the system table's
  "Jolt world", `StrataEngineCodegen`'s description, the scene settings' `PhysicsCapacity`.
- `Docs/DeveloperGuide.md`: Building (the `STRATA_USE_*` options; the switch while it exists), Optional modules, Perf
  tests (the physics metrics' names), Test rules (the broad phase helper), Working with third-party code (box3d's
  wrapper: why it is built from its sources, `-ffp-contract=off`, the golden hash on upgrade).
- `README.md` (Physics line), `ThirdPartyNotices.md`, `Docs/Roadmap.md` (Status when the stream lands; the open items:
  triggers and triangle meshes, the reference machine's metrics; "Later": joints, ragdolls and vehicles now on box3d's
  joints, height fields for `world-environment`).
- Skills: `strata-editor-automation` (`physics.stats` fields), `strata-scripting` (any character feel or physics
  tuning a script author sees: step and slope behavior, bounce threshold), `strata-make-a-game` (the same, where it
  advises values), `strata-build-test` (the backend switch while it exists; the golden hash on box3d upgrades).
- `Docs/Manual/Scripting.md` where it describes physics behavior that changes.

## 9. Risks

| Risk | Mitigation |
| --- | --- |
| box3d is young (created 2026-05, first tag 2026-06-30) and its mover API is marked experimental | pin a known commit; Strata's tests cover every behavior; asserts routed and counted; a fix upstream is a separate bump |
| box3d's `B3_MAX_WORLDS` (128) static worlds | checked before creation, graceful failure, tested |
| Character feel differs from Jolt's `CharacterVirtual` (steps, slopes, sticking) | the existing tests define it; new cases for sticking, platforms, steep ground; samples' scripts (D13) |
| Contact events for every shape cost more than Jolt's filtered recording in very busy scenes | events are per touch change; measured (`Boxes30kTracked`, `Boxes70k`); D9 limits |
| Serial body creation slower than Jolt's parallel creation for 100k bodies | heavy work stays on jobs; measured (`Create100kStaticBodies`, world gates) |
| Growth inside box3d (array reallocation) hitches a frame while a big cell loads | measured (`Grow65kMaxFrame`, `CellDriveMaxFrame`); worlds remember demand, so the next world reserves it |
| Triggers miss triangle-mesh colliders | documented, tested; a later change can query meshes for triggers if games need it |
| Hulls limited to 128 vertices | reduction by `b3CreateHull`'s `maxVertexCount`; tested on a high-poly mesh |
| Rays from inside convex shapes, sharp box corners, damping formula change results tests rely on | each case's tolerance or expectation re-derived from box3d's behavior, never widened beyond that, with the reason |
| MSVC builds not checked on this Mac | CI from D1; the wrapper mirrors box3d's own MSVC flags |
| GCC 14.2 at `-O3` false positives in Strata's new physics code | lint on every commit; CI Linux and the release workflow's Dist builds |
| The showcase samples land first and assume Jolt's tuning | D13 after merging main; differences reported |

## 10. Deferred

- **Joints, ragdolls and vehicles** (box3d has revolute, prismatic, spherical, weld, wheel, distance, motor and filter
  joints): new components, gizmos, script API and feature test: a stream of its own after this one lands, so this
  port's diff stays about parity and its regressions are attributable.
- **Height-field colliders** (`b3CreateHeightFieldShape`): `world-environment`'s terrain.
- **Double precision** (`BOX3D_DOUBLE_PRECISION`) for worlds beyond 100 km: with 64-bit entity handles and origin
  rebasing ("Further").
- **Recordings and replays** (`b3World_StartRecording`, `b3RecPlayer_*`) as an editor debugging tool.
- **Triggers on triangle meshes** (6.3) unless a sample needs it.
- **Budgets** of the new and changed metrics, and every acceptance limit in the reference machine's numbers: the
  reference machine (Docs/Roadmap.md's list).
- **MSVC Ninja builds' instruction sets**: today (and after D2) `cl` under Ninja gets no `/arch:AVX2` while the Visual
  Studio generator does (Jolt read the VS platform); unifying them changes the code CI's Windows Release job builds,
  an owner's choice (open question), not part of this port.

## 11. Deliver in this order

1. D1 vendor box3d (submodule, wrapper, notices), its own commit; push `ci/physics-box3d` once to see MSVC build it.
2. D2 backend switch and Strata's CPU options.
3. D3 runtime, task system, mesh cooking, the golden hash case.
4. D4 bodies, shapes, sync, write-back, layers, queries.
5. D5 contacts and triggers.
6. D6 capacities.
7. D7 character controllers.
8. D8 whole engine on the box3d build (Debug and Release suites, feature test).
9. D9 perf A/Bs, box3d against Jolt, on the same commit; provisional entries.
10. D10 flip the default; CI.
11. D11 remove Jolt and its model (API, scene key, commands, Inspector, font test, budgets entries, renames).
12. D12 docs and skills (each behavior's docs go with its commit; this item finishes them).
13. D13 after `showcase-samples` lands: merge main, run its samples' command scripts on box3d, fix or report.
14. Final: Release A/B of the physics and world metrics against the base build (580eee9e), full Debug and Release
    suites, lint, CI green, report.

If time runs short, stop after a complete step: D1-D8 leave a working box3d backend behind the switch (Jolt still the
default), D10 must not land without D11 following before the stream merges.

## 12. Merge notes

- Shared files touched at function level: `CMakeLists.txt` (the option's description), `Strata/CMakeLists.txt` (the
  Physics link, the codegen block), `Strata/vendor/CMakeLists.txt` (the Jolt block becomes box3d's),
  `CMake/StrataCompilerOptions.cmake`, `.gitmodules`, `ThirdPartyNotices.md`, `StrataTests/Perf/Budgets.json` (four
  entries), `StrataTests/CMakeLists.txt` (the test-only box3d scenario library), `AGENTS.md` (one line, maybe one
  rule), the docs' physics paragraphs, `SceneSerializer.cpp` and `Scene.h` (`MaxBodyPairs`), `EditorSceneCommands.cpp`,
  `EditorWorldCommands.cpp`, `InspectorPanel.cpp`, `StreamingPanel.cpp`.
- `showcase-samples` lands first: merge main before D13; its samples use only the engine's API, so conflicts are
  limited to docs and `Budgets.json` lines.
- The script ABI and the feature test's scene are unchanged; no host function is added.
