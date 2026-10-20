# Workstream `physics-box3d`: Strata's physics on box3d instead of Jolt (wave 5)

Owner's request (2026-10-20): "I would like to switch to box3d (https://github.com/erincatto/box3d) as its physics
engine". This stream does the physics part; the `showcase-samples` stream builds new sample projects at the same time
and lands first.

AGENTS.md is the source of truth for rules. The machine's rules (wave 4's `Docs/Plans/Wave4/mac.md`, removed with wave
4's plan in 11b2a133, restated here because this stream depends on them):

- The Windows perf reference machine is away. On the Mac, `-L perf` informs and is not the gate; existing
  `StrataTests/Perf/Budgets.json` entries (`Measured`, `Budget`) are never added, raised or lowered from Mac numbers; a
  new metric gets a provisional entry (`{ "Unit": ..., "Provisional": true }`, with its acceptance limit as `Target`
  when it has one), which measures, prints and never fails.
- A/Bs: Release builds, interleaved, at least 3 pairs, every measurement under
  `python3 Tools/PerfLock.py --label physics-box3d -- <command>`, judged by the ratio of the medians. A criterion in the
  reference machine's absolute numbers is reported with the Mac value and deferred to the reference machine.
- Every commit: `python3 Tools/CompatLint.py --changed-since 580eee9e` after a Debug build (GCC 14 and LLVM 16 with its
  libc++, warnings as errors) comes back clean; CI's GCC 14.2 at `-O3` (the release workflow's Dist builds) must stay
  warning-free too.

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
  against the base build (Jolt) on this Mac, with ratios in the report.
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
| Library lifetime | `PhysicsRuntime`: allocator, type factory, trace and assert hooks, reference counted | `PhysicsRuntime` keeps counting its users, but box3d's hooks are installed **once per process** (at the first `Acquire`, under `std::call_once`) and never swapped or restored: cooked hulls and meshes outlive every world and are freed through the allocator that made them, and box3d reads the hooks as plain globals from cook jobs. `b3SetAllocator` (aligned allocation, thread-safe), `b3SetAssertFcn` routed through `Detail::HandleAssertFailure` exactly as Jolt's asserts are today (logging, test handlers, a debugger break; the default handler aborts, so box3d never continues past a failed invariant), counted in `PhysicsRuntime::GetAssertionCount()`, and `b3SetLogFcn` (`ST_CORE_WARN`, except inside a thread's `Box3DLogSuppression` scope, used only around hull attempts that are expected to fail). No type registry is needed. |
| Worlds | one `JPH::PhysicsSystem` per `PhysicsWorld` | one `b3WorldId` per `PhysicsWorld`; box3d holds at most `B3_MAX_WORLDS` (128, `constants.h`) at once in a static array: `PhysicsWorld` checks `b3GetWorldCount() < B3_MAX_WORLDS` first (box3d's `b3GetMaxWorldCount` is a high-water mark, not the limit), and a scene that finds none plays without physics with one error (never box3d's assert). `b3CreateWorld` and `b3DestroyWorld` scan and clear the slots without a lock, so worlds are created and destroyed only on the main thread: nothing destroys a world on a background job (today's `RetireJoltSystem` goes with migration; a prepared world is dropped on the main thread, as today). |
| Jobs | `PhysicsJobSystem` (Jolt `JobSystem` on Strata's) | `PhysicsTaskSystem`: `b3WorldDef::enqueueTask` submits a Strata job (pooled handles: box3d starts at most `B3_MAX_TASKS`, 256, per step), `finishTask` waits for it (Strata's `Wait` runs a job that has not started itself, so a step never deadlocks, even with every worker busy); `workerCount` = the job system's workers plus the main thread, at most `B3_MAX_WORKERS` (32), 1 without a job system. `PhysicsStats::JobCount`/`WorkerJobCount` stay. |
| Bodies, motion types | `BodyCreationSettings`, `EMotionType` | `b3BodyDef` + `b3CreateBody`: `b3_staticBody`, `b3_kinematicBody`, `b3_dynamicBody`; `userData` = Strata's record slot. |
| Box collider | `BoxShape` (convex radius 0.05) | hull: `b3MakeTransformedBoxHull` with half extents already scaled by Strata (non-uniform and mirrored scale, offset and rotation baked), `b3CreateHullShape`; sharp corners (box3d's linear slop 5 mm, no convex radius). Not `b3MakeScaledBoxHull`: it clamps half widths to 4 x `B3_LINEAR_SLOP` (2 cm), where `b3MakeTransformedBoxHull` clamps to 0.2 x slop (1 mm), today's `c_MinColliderExtent`. |
| Sphere collider | `SphereShape` | `b3CreateSphereShape` with the offset as its center; radius scaled by the largest absolute axis scale (as today). |
| Capsule collider | `CapsuleShape` | `b3CreateCapsuleShape` (two centers in body space); radius by the larger of `\|X\|` and `\|Z\|`, half height by `\|Y\|` (as today). |
| Several colliders | `StaticCompoundShape` of rotated/translated/scaled children | several shapes on one body (box3d's runtime compound; dynamic and kinematic too); box3d's baked compound (`b3CreateBakedCompoundShape`) is static only and used only for an offset mesh collider (next row). |
| Mesh collider, convex | `ConvexHullShape` cooked from the positions | `b3CreateHull(points, count, maxVertexCount)` cooked on jobs. box3d caps vertices, faces and edges at 128 each (`B3_MAX_HULL_VERTICES/FACES/EDGES`); a triangulated hull of V vertices has 2V - 4 faces and 6V - 12 half edges (at most 256), so the built-in sphere (1,225 points) fails at 128, 96, 64 and 48 vertices. Cooking tries `min(points, 64)` vertices and, if box3d refuses, 44 (any hull of 44 vertices fits: 84 faces, 252 half edges), inside a `Box3DLogSuppression` scope so the expected refusal logs nothing; only a hull that fails at 44 (degenerate input) is "unusable" with a warning, as today. Then `b3CreateTransformedHullShape` with the collider's scale and local transform. |
| Mesh collider, triangles (static only) | `MeshShape` cooked from `PhysicsMeshData` | `b3CreateMesh` (its tree) cooked on jobs and cached per data object; `b3CreateMeshShape(body, def, mesh, scale)` (scale includes mirroring); a mesh collider placed away from its body's origin (a child of a static rigid body) becomes a one-mesh baked compound (`b3CompoundMeshDef` with transform and scale; the triangles are shared, not copied). Baked compounds cannot be sensors (`shape.c` asserts `isSensor == false`): an offset triangle-mesh collider on a trigger body is unusable with a warning (a mesh collider at its trigger body's origin is a sensor mesh shape, which box3d supports). box3d creates mesh contacts only on static bodies, which is Strata's rule already. |
| Mesh shape cache | `PhysicsMeshShapes`: Jolt binary state per data object, restored per world | same class, same contract, but the cooked objects are **shared**: a `Ref<CookedMesh>` (owning `b3MeshData*`, and the `b3CompoundData*` of an offset mesh collider) and a `Ref<CookedHull>` (owning `b3HullData*`), freed with `b3DestroyMesh`/`b3DestroyHull`/`b3DestroyCompound` when the last reference goes. box3d's mesh shapes and baked compounds keep pointers to that data for their lifetime (`box3d.h`: "must remain valid for the lifetime of this shape"), so every body record whose shapes use one holds a reference: the cache drops only its own when the data object goes, and a mesh unloaded on purpose leaves running colliders their shape, as today. |
| Mass | `MassPropertiesOverride` from `RigidBodyComponent::Mass` | shapes with density 1 and `updateBodyMass = false`, then `b3Body_ApplyMassFromShapes`, `b3Body_GetMassData`, scaled to `Mass` (mass and rotational inertia), `b3Body_SetMassData`. |
| Friction, restitution | per body, combined `sqrt(a*b)` and `max` | `b3ShapeDef::baseMaterial` (`b3SurfaceMaterial` friction, restitution); box3d's defaults combine the same way. |
| Damping, gravity scale | body settings | `b3BodyDef::linearDamping`, `angularDamping`, `gravityScale`; `b3Body_SetGravityScale`. box3d damps by `1/(1 + h c)` per sub-step where Jolt multiplied by `1 - h c`: tests compare with their tolerance (5.3). |
| Rotation locks (world axes) | `EAllowedDOFs` | `b3MotionLocks::angularX/Y/Z`, which box3d applies to the world angular velocity (`solver.c`): world axes, as today. |
| Continuous collision | `EMotionQuality::LinearCast` | box3d always sweeps dynamic bodies against static ones (`enableContinuous`); `ContinuousCollision` sets `isBullet`, which now means: also swept against kinematic and dynamic bodies (bullets do not sweep against each other). The component's description, the scripting and make-a-game skills and the tests say so (5.3). |
| Collision steps | `PhysicsSettings::CollisionSteps` Jolt collision steps per update | the fixed step runs `CollisionSteps` box3d steps of `dt / CollisionSteps`, each with box3d's 4 sub-steps (`c_SolverSubSteps`); default 1. box3d clears forces and torques at the end of every `b3World_Step` (`solver.c`) and its event arrays at the start of the next: Strata keeps each body's force and torque of the fixed step and applies them before **every** box3d step of it, and reads contact, sensor and move events after every box3d step, merging them into the fixed step's (a touch that begins and ends inside one fixed step reports Begin and End). |
| Gravity | `SetGravity` | `b3World_SetGravity` (wakes nothing by itself: Strata wakes the awake-able bodies, as today). |
| Collision layers | object layers `layer * 2 + moving`, broad phase layers, pair filter from the 32x32 matrix | `b3Filter`: `categoryBits = 1 << layer`, `maskBits = matrix row of the layer \| c_QueryCategory` (bit 63). Two shapes collide only if both masks allow it (`b3ShouldShapesCollide`: the matrix's rule). A matrix change sets the filter of every shape (`b3Shape_SetFilter`, rare, O(shapes)) and wakes the bodies. Static bodies are in box3d's static tree, which only moving proxies are tested against. |
| Query layer masks | `ObjectLayerFilter` on the user layer | `b3QueryFilter{ categoryBits = c_QueryCategory, maskBits = layerMask }`: every shape carries the query bit, so a body on a layer that collides with nothing is still found by queries. |
| Triggers | sensors (`IsSensor`) | `isSensor` shapes with `enableSensorEvents` (box3d: "false by default, even for sensors"); every other shape gets `enableSensorEvents` (visitor). box3d sensors query the static, kinematic and dynamic trees for every sensor, static sensors included, and see convex visitors of other bodies, other sensors included. Strata keeps Jolt's rule when it reads sensor events: an event where **neither body moves** (a static trigger and a static collider or another static trigger) is dropped; kinematic and dynamic triggers keep detecting static colliders, static triggers detect kinematic and dynamic bodies. Two triggers that see each other give two box3d visits that fold into one entity pair (below), so sensor-vs-sensor pairs exist exactly when one of the two moves, as in Jolt. **Visitors must be convex**: a trigger does not report a static triangle-mesh collider (Jolt's did; documented difference, pinned by a test). Events: `b3World_GetSensorEvents` (begin/end, deterministic order), which carry no point or normal: a trigger event's `Point` is the center of the intersection of the two shapes' world bounds (inside both bounds) and its `Normal` points from the trigger's shape center to the visitor's (up when they coincide). A watch begun while a body is inside a trigger is seeded from `b3Shape_GetSensorOverlaps`. |
| Contacts and collision events | `ContactListener` on job threads, per-thread buffers, tracked pairs only for listeners | `b3World_GetContactEvents` after every box3d step (begin/end, on the main thread, deterministic). box3d fixes a contact's event flag when the contact is created (`contact.c`), so every non-sensor shape enables contact events (cost: one event per touch change, not per contact per step, unlike Jolt's persisted callbacks) and Strata keeps only wanted pairs. box3d keys contacts by shapes and child index, so Strata **folds them into entity pairs**: each tracked pair counts its touching shape contacts (and sensor visits); Begin is dispatched when the count goes from 0 to 1, End when it returns to 0, and each box3d step's End array is applied before its Begin array. Shapes are resolved through Strata's own map from the shape's index and generation to its record, never through `b3Shape_GetUserData` (End events may name shapes that are gone, `types.h`). End events of shapes Strata destroyed to rebuild a body are ignored: the rebuilt body's pairs stay begun but unconfirmed, the next step's Begin events of the new shapes confirm them silently, and an unconfirmed pair that no new shape touches gets its End after that step (today's report-based behavior: rebuilding a body keeps its contacts). A pair that becomes wanted while touching is seeded from `b3Body_GetContactData` (touching manifolds) and begins at the next dispatch, as today ("tracked from the first step that reports it"). Begin's point and normal come from the manifold; End carries the last point read (at Begin, refreshed each step for tracked pairs with an awake body). The per-entity pair lists (constant-time add and remove, `PairIndexOperations`) stay. |
| Activation, write-back | `BodyActivationListener`, active body list, body locks | `b3World_GetBodyEvents` move events after every box3d step (bodies that moved, with `userData` and the transform, `fellAsleep`), merged per fixed step; Strata keeps its order (the bitmap over body indices, parents first, parallel chunks for bodies without a dynamic ancestor, the rare cases on the main thread). |
| Sync before the step | `BodyInterface::SetPositionAndRotation`, `MoveKinematic` | dynamic: `b3Body_SetTransform` (teleport) and waking the bodies it touches (`b3Body_GetContactData`) and, at the new place, a tree query (`b3World_OverlapAABB`, counted in `WakeQueryCount`); kinematic: `b3Body_SetTargetTransform(target, dt, wake)`; static: `b3Body_SetTransform` and waking what touches it. |
| Leaving the simulation | `RemoveBodies` in one batch, wake queries in 16 m regions | inactive entities: `b3Body_Disable` / `b3Body_Enable` (the body keeps its ID and shapes); destroyed: `b3DestroyBody`. Both wake the bodies they touched (`b3DestroyBodyContacts(..., wakeBodies = true)`), so the region queries go; "what rested on them wakes" stays tested. |
| Sleeping, waking | `IsActive`, `ActivateBody` | `b3Body_IsAwake`, `b3Body_SetAwake`; box3d's island sleep (0.5 s, the same as Jolt's default). |
| Body API | forces, impulses, velocities, point velocity, teleport | `b3Body_ApplyForce(ToCenter)`, `ApplyTorque`, `ApplyLinearImpulse(ToCenter)`, `ApplyAngularImpulse`, `Set/GetLinearVelocity`, `Set/GetAngularVelocity`, `GetWorldPointVelocity`, `SetTransform`; forces last one step (box3d clears them after the step, as Jolt did). |
| Raycasts | `CastRay` with collectors, triggers skipped, ignored entity, inside starts ignored | `b3World_CastRay` with a callback (skips sensors unless asked, the ignored entity, entities pending destruction; returns the fraction for the closest hit); `RaycastAll` keeps each body's closest hit and sorts by distance. `maxDistance` clamped to 1e5 (= `B3_HUGE` at one unit per meter). Rays starting inside a convex collider must not hit it: if box3d reports a zero-fraction hit there, the callback ignores it (test). |
| Overlaps | `CollideShape` with sphere and rotated box | `b3World_OverlapShape` with a `b3ShapeProxy` (a point with the radius; the box's 8 rotated corners), shapes to bodies to entities, de-duplicated, in the deterministic order of today. |
| Character controllers | `CharacterVirtual` + `ExtendedUpdate` (slide, stairs, stick to floor), presence bodies, pushes on the main thread | Strata's own mover on box3d's geometric mover API (`b3World_CastMover`, `b3World_CollideMover`, `b3SolvePlanes`, `b3ClipVector`; capsule only, experimental in box3d): see 6.4. Presence bodies (moved with `b3Body_SetTransform`, zero velocity, as today's `SetPositionAndRotation`), pushes (`PushBodies`), parallel moves (box3d's queries are read-only and may run on many threads, `docs/faq.md`) and contacts stay. |
| Capacities | fixed buffers, migration growth, overflow | reservations and growth in place: see 6.5. |
| Parallel body creation | Jolt bodies built on jobs, IDs given in record order | box3d's creation API is single-threaded: jobs prepare everything that costs (merged collider lists, transforms, box hulls, hull cooking, mass data, body and shape definitions) in fixed chunks; the main thread creates bodies and shapes in record order (so IDs and results do not depend on the thread count, `ParallelBuildCount`). Only the build before a world's first step (immediate or prepared slices) creates static shapes with `invokeContactCreation = false`: box3d marks a new static proxy for pair finding only when it forces pair creation (`broad_phase.c`), so every static shape created afterwards (rebuilds, streamed cells) forces it, or bodies resting there would not touch it until they left their fat bounds. |
| Prepared physics (loaded scenes, streamed worlds) | `PhysicsBuildMode::Sliced`, `PrepareLoadedScene`, `Reserve` | unchanged contract: a prepared world is a box3d world built in slices before the scene runs; `Reserve` (6.5). |
| Body IDs for tests | 32-bit Jolt ID (index, sequence) | `uint64_t`: `generation << 32 \| index1` of the `b3BodyId` (without the world index, so two worlds built alike compare equal); `c_NoBodyID = UINT64_MAX`. |
| Determinism | Jolt deterministic per platform | box3d is deterministic across thread counts and, compiled with `-ffp-contract=off`, across platforms; Strata's glue keeps its ordering rules. |
| Time dilation, dropped steps | `PhysicsStats::DroppedSteps` from `Scene::GetDroppedFixedStepCount` | unchanged (the scene's fixed-step loop drops the steps; physics reports them); "Physics.Simulation" keeps covering it. |
| Large coordinates | single precision | single precision (`BOX3D_DOUBLE_PRECISION` off; `B3_HUGE` = 1e5 m): a body whose world position is beyond ±1e5 waits like a degenerate transform, with a warning, instead of tripping box3d's asserts (the world tests' 20 km stays well inside). |
| `physics.stats`, Streaming panel, pill | capacity fields of Jolt | same command and views; fields follow 6.5 (`maxBodyPairs`, `contactOverflowSteps`, `demand.bodyPairs` go; `growthLimit` comes). |
| Script API, gizmos, collider presets, templates | through services and components | unchanged. |
| Feature test | physics, collision and character scripts | scripts and scenes unchanged (the scenes keep their 0.4.0 `MaxBodyPairs` key, which proves that it loads without a warning); the harness's capacity checks (`FeatureTestUtils.cpp`) drop the pair field and `ContactOverflowSteps` and still prove that `MaxBodies` and `MaxContactConstraints` serialize and size the world; must pass on box3d with `c_ExpectedLogMessages` unchanged. |
| Build without physics | `STRATA_WITH_PHYSICS=OFF`: no Physics library, no Jolt | the same, without box3d. |
| `strata_engine_cpu_options` | mirrors Jolt's options for every engine object, checked against Jolt's | becomes Strata's own (6.7): the same flags for the same configurations, options `STRATA_USE_*`, `STRATA_CROSS_PLATFORM_DETERMINISTIC` and `STRATA_CROSS_COMPILE_ARM` of Strata's, the Jolt check and `JPH_*` definitions gone. |

## 5. Plan of the replacement

### 5.1 The replacement, in place (no backend switch)

The port replaces Jolt inside the Physics module's own files instead of running two backends behind a CMake switch. A
switch would keep a dormant backend that no CI job compiles until a flip, and the box3d build could not link before its
world and its characters were both complete (they implement one interface: the world, its six services, its character
controllers and its cooked shapes), so its intermediate commits would hold stubs (golden rule 5) or code that nothing
compiles. So:

- **D1-D3 land next to Jolt**: the vendored library (built by all six CI jobs from D1), Strata's own CPU options and the
  neutral names (D2), and the box3d pieces that stand alone (the runtime hooks, `PhysicsTaskSystem`, the golden hash
  case), with the Physics layer linking both libraries (D3), so CI's six jobs build and test them before the move.
- **D4 moves the module to box3d**: `PhysicsWorld`, `CharacterControllerSet`, `PhysicsMeshShapes` and `PhysicsRuntime`
  on box3d in their own files (the Strata glue of `PhysicsWorld.cpp`, records, merging, change lists, pending reasons,
  mesh waits, ordering and pair lists, is kept and reworked where box3d differs), with the test changes of 5.3, in one
  commit that builds and passes every suite and is pushed to CI at once. D5-D7 (contacts and triggers, capacities,
  characters) are parts of D4, not commits that could pass alone; their acceptance lists stay, for D4's commit. D8 is
  D4's commit passing the full Debug and Release suites; there is no flip (the former D10 is D4).
- **D9's A/Bs** compare D4's Release build with the base build (580eee9e's Release build, Jolt) in a scratch directory
  of this worktree, deleted afterwards.
- **D11 removes Jolt itself**: the submodule, its vendor block and checks, `JPH_*` definitions, `PhysicsJobSystem`,
  `PhysicsStateBuffer`, `PhysicsLayers.h`, the font test's Jolt font, and the Jolt-only model in settings, scene files,
  commands, the Inspector and the perf tests (5.2, 6.6).

Stopping early: before D4 the branch holds box3d next to Jolt (unused by the world) and can merge with CI green; from D4
on it is box3d only. There is never a dormant backend, and nothing merges while two physics libraries serve the world.

### 5.2 Interfaces kept

- `PhysicsWorld`'s and `PhysicsSystem`'s public functions, the six scene services and their types
  (`Scene/SceneServices.h`), the components and their reflection, `PhysicsMeshProvider`, `PhysicsBuildMode`,
  `PreparedPhysicsWorld`, `PhysicsSystem::PrepareLoadedScene`, `CountBodyCandidates`.
- Changes, all from Jolt's model, with every user: what Jolt can implement too lands first (D2: the
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
| `Physics.Capacity` "Bodies waiting for a slot cost the steps nothing and get freed slots in the order they started waiting", "At its growth limit a full world keeps bodies waiting", and `Physics.Lifecycle` "Rebuilding a body in a full world keeps the body and its motion" | kept, at `GrowthLimit` | only the growth limit makes bodies wait |
| `Physics.Capacity` "Automatic capacities follow the bodies a scene starts with" | the room follows 6.5 (next power of two of twice the candidates, at least 1,024; contacts two per body); no pair or `TempAllocatorSize` expectations | box3d sizes its own arena and has no pair buffer |
| `Physics.Capacity` "A scene's capacities size its world, before the default settings and automatic sizing" | `MaxBodies` and `MaxContactConstraints` checks kept (they size the room, `b3WorldDef::capacity`); the `MaxBodyPairs` checks (4 x 16, 1000) go | no pair capacity |
| `Physics.Capacity` "An automatic capacity that overflowed grows in the scene's next world, and only in that scene's" | "An automatic room that a world outgrew is reserved by the scene's next world, and only in that scene's": the body room (`Demand.Bodies`) instead of `MaxBodyPairs` 256 to 512 and `Demand.BodyPairs == 512` | growth in place replaces pair overflow |
| `Physics.Capacity` "IPhysicsCapacity reports bodies, waits, the last update's work and what the world reserves, and reserves room" | fields of 6.5 (`GrowthLimit`, growths); `ReservedBytes` checked equal to its constant-time formula (6.5) | the service is called every editor frame |
| `Physics.Simulation` "Continuous collision keeps fast projectiles from tunneling through thin walls" | "Bullets do not tunnel through thin kinematic walls": a thin kinematic wall; the non-bullet control passes through it, the bullet (`ContinuousCollision`) stops in front of it | box3d sweeps every dynamic body against static ones, so the static-wall control no longer tunnels; the flag now means "also against moving bodies" |
| `Physics.Simulation` "Collision sub-steps catch fast bodies that one collision step misses" | the wall becomes kinematic (box3d does not sweep a non-bullet against it); one collision step passes it, four catch the projectile in or before it, with the bound derived from box3d's push-out and stated in the test | the same reason; nothing is widened |
| `FeatureTestUtils.cpp` (feature harness) | the `MaxBodyPairs` and `ContactOverflowSteps` checks go; `MaxBodies` and `MaxContactConstraints` are still non-default, serialized and reported by the world | no pair capacity; box3d cannot overflow |
| `Scene.Serialization` (`SerializationTests.cpp`, the `MaxBodyPairs` -5 warning) | a 0.4.0 `MaxBodyPairs` of any value loads without a warning and is not written back; the invalid-value warning stays covered by `MaxBodies` | compatibility with 0.4.0 files |
| `Editor.Commands` (`EditorCommandTests.cpp`, `maxBodyPairs`) and the authoring UI test (`EditorAuthoringUITests.cpp`, the Max Body Pairs row) | `scene.setSettings` accepts `maxBodyPairs`, ignores it and returns a `warning`; `scene.getSettings` and `physics.stats` lack it; the Inspector has no such row | 6.6 |
| `Perf.Physics` "A world at the largest capacities ..." (`c_MaxBodyPairsLimit`) | the largest `MaxBodies` and `MaxContactConstraints` only | no pair capacity |
| `Perf.Physics.Boxes30k.DroppedContactWarnings` | redefined as the physics warnings the run logs (box3d's log hook and Strata's physics warnings), still 0; the entry is kept unchanged | box3d drops no contacts; a warning of any kind in that run is still a defect |
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
- `Physics.Queries` "Bodies on a layer that collides with nothing are still hit by queries" (the query bit).
- `Physics.Events` "A watch begun while bodies touch reports Begin at the next step and End when they part" (seeding);
  "Triggers report convex colliders and characters, not triangle meshes" (the documented difference).
- `Physics.MeshColliders` "A convex collider of a mesh with more than 128 hull vertices is reduced, not refused"; "A mesh
  collider offset from its static body is placed where its entity is".
- `Physics.Capacity` "IPhysicsCapacity reports box3d's growth: reserved room, growths, the growth limit".
- `Physics.Character` (6.4) "Characters stick to the ground walking down slopes and steps", "Characters ride moving
  platforms" and "Characters slide down steep ground", if not already covered by the existing cases.
- From the review of this spec: `Physics.Box3D` "A scene's first world gets physics" (the world limit is `B3_MAX_WORLDS`,
  not box3d's high-water mark) and "box3d's asserts go through Strata's assert handling" (a `Continue` handler, in a
  helper process if needed); `Physics.Events` "A static trigger touching static colliders and other static triggers
  reports nothing", "A body of several shapes touching another body begins one contact", "Rebuilding a touching body
  reports neither End nor Begin", "Trigger events' points lie where the two overlap", "A watch begun while a body is
  inside a trigger reports Begin"; `Physics.Simulation` with `CollisionSteps = 4`: "A force changes the velocity as much
  as with one collision step" and "A touch that begins and ends within one step reports Begin and End";
  `Physics.Lifecycle` "A static floor rebuilt or created under a resting box keeps holding it"; `Physics.AssetMeshes`
  "Unloading a mesh while a world uses its collider keeps the shape until the world stops" (the cooked mesh's live
  count); `Physics.MeshColliders` "A convex collider of the built-in sphere is reduced, not refused" (no warning), "A
  1 cm box collider keeps its size", "A large crate rests on a dense mesh floor" (box3d keeps at most 512 triangles per
  convex shape and mesh), "An offset triangle-mesh collider on a trigger body is unusable with a warning".

### 5.4 Must-fail checks

Each is run once on the Mac by breaking the code locally, and the report quotes the failure (the "World limit" row:
the helper process creates the 129th world without the check):

| Check | Break | Fails |
| --- | --- | --- |
| Floating-point flags | build box3d without `-ffp-contract=off` (arm64 clang contracts by default) | the golden hash case |
| Task adapter | return from `finishTask` without waiting for the job | the golden hash case with 4 workers (the step reads results not yet written) |
| Query bit | drop `c_QueryCategory` from the shapes' masks | "Bodies on a layer that collides with nothing are still hit by queries" |
| Contact seeding | do not seed wanted pairs from `b3Body_GetContactData` | "A watch begun while bodies touch ..." |
| Contact filter | track every box3d contact event | `Physics.Scale` "Contacts are tracked only while someone wants them" |
| Body ID order | create bodies in job order | `Physics.Capacity` "Bodies built on the job threads get the same IDs ..." |
| Write-back order | write children before parents (nested bodies by depth, deepest first; or with their parents' writes) | `Physics.Hierarchy` "A chain of moving dynamic bodies writes each one under its parent's new pose" ("Parented dynamic bodies ..." has one body under a static parent, which no order changes) |
| Character determinism | push bodies from the move jobs | `Physics.Character` "Characters move the same with any number of threads" |
| World limit | create the 129th world without the check | "A scene that finds no free box3d world ..." (box3d asserts) |
| Mass override | keep box3d's density mass | `Physics.Simulation` "Mass and damping shape the response ..." |
| Font kern case | append the kern table with a pair count too large | the kern case's `checkKerningDisabled` |
| First world | check against `b3GetMaxWorldCount()` | "A scene's first world gets physics" |
| Static triggers | keep sensor events where neither body moves | "A static trigger touching static colliders ..." |
| Pair folding | dispatch each shape contact's Begin and End | "A body of several shapes touching another body begins one contact" |
| Rebuild events | dispatch End events of the shapes a rebuild destroyed | "Rebuilding a touching body reports neither End nor Begin" |
| Forces per collision step | apply forces only before the first box3d step | "A force changes the velocity as much as with one collision step" |
| Pair creation of new statics | create every static shape with `invokeContactCreation = false` | "A static floor rebuilt or created under a resting box ..." |
| Hull limits | cook convex hulls at up to 128 vertices only | "A convex collider of the built-in sphere is reduced, not refused" |
| Cooked mesh lifetime | free the cooked mesh when the cache drops it | "Unloading a mesh while a world uses its collider ..." (live count) |

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

### 6.3 Contacts, triggers, events (D5, part of D4)

As in the table (4): every box3d step's events (contacts, sensors, moves) are read after that step and merged into the
fixed step's; contacts and sensor visits fold into entity pairs by count (Begin at 0 to 1, End at 1 to 0, each step's
End array before its Begin array); shapes resolve through Strata's map from index and generation, never through a shape
that may be gone; End events of shapes destroyed for a rebuild are ignored and the rebuilt body's pairs are confirmed or
ended by the next step; sensor events where neither body moves are dropped (Jolt's rule); trigger events get a `Point`
(the center of the intersection of the two shapes' world bounds) and a `Normal` (trigger shape center to visitor center,
up when they coincide); wanted pairs are seeded from `b3Body_GetContactData` and `b3Shape_GetSensorOverlaps`. Event
order: box3d's events come in a deterministic order; Strata dispatches in its existing order (End events of removals
first, then the step's events by pair), and the per-entity pair lists keep their constant-time updates. Characters'
contacts join the pairs as today.

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
   (kinematic capsule of 90% of the radius, moved with `b3Body_SetTransform` so that they keep zero velocity, as today:
   a target transform would give them a velocity that carries resting bodies by friction and pushes past
   `PushStrength`) and the entities' translations are applied on the main thread in entity order, so results do not
   depend on the thread count.

### 6.5 Capacities on box3d (D6)

box3d grows its own storage (bodies, shapes, contacts, tree nodes, arena) as needed; `b3WorldDef::capacity`
(`b3Capacity`: static and dynamic body and shape counts, contacts) only reserves it up front, and
`b3World_GetMaxCapacity` reports the most a world held. So:

- `MaxBodies` and `MaxContactConstraints` (settings and scene) are the **room a world reserves** when it is created:
  0 sizes them from the scene (`ResolveCapacities`: bodies the next power of two of twice the body candidates, at least
  1,024; contacts two per body; each at least the remembered demand), split into static and moving counts by the
  candidates' types. box3d reserves the room up to 131,072 bodies and 262,144 contacts
  (`PhysicsWorld::c_MaxBodiesReservedAtCreation`, `c_MaxContactsReservedAtCreation`; more only when the automatic room
  is larger): the largest room a scene may ask for would otherwise reserve about 4.6 GB before any body needs it, and a
  room beyond the bound counts for growth only (decided in the review round; the implementation first reserved only the
  automatic room, which left a scene's own room without effect on box3d). Exceeding the room is no error and no warning: box3d grows, the world counts a growth
  (`GrowthCount`, `LastGrowthFrom/To`: the room doubled; `LastGrowthMs`: the round of changes that grew it) and records
  the demand, so the scene's next world reserves enough (`GetRememberedCapacityDemand`, per lineage as today).
- `GrowthLimit` (default the engine's limit, 2^21 bodies, and box3d's `B3_MAX_SHAPES`, 2^22, for shapes) is the only
  place bodies wait: beyond it they queue in the order they failed, get freed room first, and are reported in one error
  (`WaitingForSlot`, the pill, `Perf.World.PhysicsBodiesWaiting`).
- `Reserve(bodies)` (the world streamer's) reserves Strata's record storage at once and raises the room and demand, so a
  streamed world's next world starts large enough; it cannot pre-grow a running box3d world (its API has no reserve), so
  the cost of box3d's growth while a cell loads is measured (`Perf.Physics.Grow65kMaxFrame`, `Perf.World.*`).
- `PhysicsCapacityInfo::ReservedBytes` stays constant-time (`IPhysicsCapacity` is "cheap": the Streaming panel calls it
  every editor frame while playing, `GameRuntime` for every perf-log row): `EstimateReservedBytes` of the room (the
  larger of what box3d reserved and `b3World_GetMaxCapacity`, which box3d keeps as a value), box3d's storage only
  (Strata's records are not counted: the estimate is fitted to and tested against box3d's own count). `b3World_GetCounters().byteCount` walks every island, sensor and hull (O(islands + sensors + hulls)), so
  it is read only on demand (`physics.stats`, tests), never per frame. `EstimateReservedBytes(settings)` (also the world
  cook's report, before any world exists) uses per-body, per-shape and per-contact constants fitted to box3d's counts,
  tested within 15% of `byteCount`, and the service's value is tested equal to the formula.
- Gone: migration, `Grow`'s system swap, `Rebind`, `PhysicsStateBuffer`, the anchor bodies, the pair and contact
  overflow counts, the broad phase's single-thread preparation. `PhysicsWorld::Grow` stays as "apply the pending
  changes and reserve what `Reserve` asked for" (its callers and tests keep working), returning whether the room grew.

### 6.6 Scene files and commands (D11)

- Components unchanged: no migration of entities.
- `"PhysicsCapacity"`: `MaxBodies` and `MaxContactConstraints` are read and written as before (their meaning is room,
  6.5); `MaxBodyPairs` in 0.4.0 files is accepted and ignored (no warning: 0.4.0 wrote it in every scene), and no longer
  written. 0.4.0 reading a 0.5.0 file sees no `MaxBodyPairs` and sizes it automatically, so files stay compatible both
  ways. Cooked scenes carry the same JSON.
- `scene.setSettings` drops `maxBodyPairs` from its schema's documented fields but accepts it, ignores it and reports a
  `warning` naming the reason (box3d has no pair buffer), so command scripts written against 0.4.0's automation skill
  (and the showcase samples' regression scripts) keep working, as scene files do; `scene.getSettings` and
  `physics.stats` drop it; `physics.stats` drops
  `contactOverflowSteps` and `demand.bodyPairs` and adds `capacity.growthLimit`. The Inspector loses the Max Body Pairs
  row. The automation skill and the guide say so.

### 6.7 Build options (D2, D11)

- `strata_engine_cpu_options` stays the engine's code generation, now Strata's own: the same flags for every
  configuration it produces today (x86-64: `/arch:AVX2` on MSVC with the Visual Studio generators, `-mavx2 -mbmi
  -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse` on GCC and Clang; nothing on ARM64; `-pthread` off Windows), from cache
  options `STRATA_USE_SSE4_1 ... STRATA_USE_FMADD`, `STRATA_CROSS_PLATFORM_DETERMINISTIC` (drops `-mfma`, as Jolt's
  `CROSS_PLATFORM_DETERMINISTIC` did) and `STRATA_CROSS_COMPILE_ARM`, with Jolt's former defaults
  (`strata_jolt_instruction_sets` becomes `strata_cpu_instruction_sets`). So no engine object's code changes with the
  port (checked by comparing the compile flags of a non-physics translation unit in `compile_commands.json` before and
  after; the comparison ignores the `JPH_*` definitions that D11 removes from every engine object, which only Jolt's
  headers read). Jolt's `USE_*` cache names no
  longer apply (the guide's Building section says so).
- box3d is compiled with its own flags (6.1), not `StrataEngineCodegen`'s, so its results do not depend on the engine's
  instruction sets.
- `STRATA_WITH_PHYSICS` keeps its meaning ("Build the physics module (box3d)"); the no-physics CI job is unchanged.

## 7. Deliverables and acceptance

Each deliverable is one or more commits that build and pass every suite with the lint clean (5.1). Suite names below are
the doctest suites (`--test-suite=`); `ctest` runs use the `macos-debug` and `macos-release` presets (`build/macos`).

**D1 Vendor box3d.** Submodule `Strata/vendor/box3d` (`shallow = true`) at `efca0c1`, the wrapper (6.1), the notices
row; a commit of its own. *Accept:* the `box3d` target builds in Debug and Release on the Mac with no diagnostics; CI's
six jobs green on the branch (MSVC, GCC 14.2, Apple Clang 15 build it; the no-physics job does not).

**D2 Strata's CPU options and neutral names.** `strata_cpu_instruction_sets` (6.7), `PhysicsStats::LibraryStepTime`
(Jolt's step until D4), `StrataToLibraryStep` (provisional), 64-bit `PhysicsWorld::GetBodyID`. *Accept:* the build
compiles every engine object with the flags it had (`Scene.cpp`'s compile command identical to 580eee9e's);
`Architecture.*` and `Physics.*` pass.

**D3 box3d's runtime hooks, task system and golden hash, next to Jolt.** The Physics layer links box3d too; box3d's
hooks (6.1, 4) installed once per process by `PhysicsRuntime`; `PhysicsTaskSystem`; the golden hash case. *Accept:*
`Physics.Box3D` (golden hash with 1 and 4 workers, hooks, assert routing and count) and `Physics.*` pass; CI's six jobs
green.

**D4 The module on box3d** (with D5-D7 as its parts). `PhysicsWorld`, `CharacterControllerSet`, `PhysicsMeshShapes`,
`PhysicsRuntime` on box3d; the tests of 5.3. *Accept (bodies, shapes, sync, write-back, layers, queries):*
`Physics.Simulation`, `Physics.Lifecycle`, `Physics.Hierarchy`, `Physics.Queries`, `Physics.Layers`,
`Physics.MeshColliders`, `Physics.AssetMeshes` pass in Debug, with their tolerances unchanged except where 5.3 says why.

**D5 Contacts and triggers (part of D4).** *Accept:* `Physics.Events`, `Physics.Scale`, `Scripting.Collision*` pass; the
seeding, filter, static-trigger, pair-folding and rebuild must-fail checks fail as stated.

**D6 Capacities (part of D4).** 6.5. *Accept:* `Physics.Capacity` (with the cases of `PhysicsPrepareTests.cpp`, the
rewritten cases of 5.3), `World.Streaming*`, `Editor.World*` pass; `physics.stats` adds `capacity.growthLimit`.

**D7 Character controllers (part of D4).** 6.4. *Accept:* every `Physics.Character` case and the `Scripting.Physics`
character cases pass, and the determinism case with 1, 2 and 4 threads.

**D8 Whole engine on box3d (D4's commit).** *Accept:* `ctest --preset macos-debug -LE perf` and `ctest --preset
macos-release -LE perf` pass every test (counts in the report), the feature test included, with no new expected log
messages; CI's six jobs green.

**D9 Perf A/B.** D4's Release build against the base build (580eee9e's Release build, Jolt), interleaved, at least 3
pairs each, under `Tools/PerfLock.py --label physics-box3d`, ratio of medians (box3d / Jolt):

| Metric (test case) | Acceptance |
| --- | --- |
| `Perf.Physics.Boxes10k.ActiveFrame`, `Boxes30kTracked.ActiveFrame`, `Boxes70k.ActiveFrame` | at most 1.10 |
| `PhysicsStats::LastStepTime` of the same cases (the whole step) | at most 1.10 |
| `*.StrataStepTime` (Strata's part of a step) | reported, not gated: on box3d it includes event folding, pair updates and contact-point refresh that ran inside Jolt's step (and counted as Jolt's) before; the reference machine checks the existing budgets before merge (Roadmap list) |
| `Boxes30k.StartAnd30Frames` | at most 1.10 |
| `Create10kBodies`, `Create100kStaticBodies` | at most 1.10 (creation is serial on box3d) |
| `Characters1000Step` | at most 1.25 (new controller code) |
| `MostBodies.ReservedMemory`, `LargestCapacities.ReservedMemory` | at most 1.00 |
| `Boxes30k.DroppedContactWarnings` (5.3), `Boxes70k.BodyErrors` | 0 |
| `Perf.World.CellUnloadMaxFrame`, `CellDriveMaxFrame`, `PhysicsBodiesWaiting`, `PerfGPU.World.CellDriveMaxFrame` | at most 1.10, waiting 0 |
| `Perf.Physics.Grow65kMaxFrame` (new) | Mac value reported against its 16.7 ms target |

`LibraryStepTime` is the wall time of the `b3World_Step` calls. A metric over its limit is profiled (box3d's own
`b3World_GetProfile` parts against Strata's) and, where Strata's part is the cause, fixed; where box3d's is, its
settings (worker count, sub-steps) are tried and the result reported. New metrics get provisional entries
(`StrataToLibraryStep` for the three cases, `Grow65kMaxFrame`); existing entries keep their values (7.1).

**D11 Jolt removed.** The submodule, Jolt's vendor block and checks, `JPH_*` definitions, `PhysicsJobSystem`,
`PhysicsStateBuffer`, `PhysicsLayers.h`, the notices row; the API changes of 5.2 and 6.6 with every user (editor
commands, Inspector, panels, tests, perf tests); the font test. *Accept:* `git grep -i jolt` finds only history
(`Docs/Releases/0.3.0.md`, `0.4.0.md`, the Roadmap's past waves, this spec); full suites in Debug and Release; lint
clean; CI green on all six jobs.

**D12 Docs and skills** (with the commits that change behavior, section 8). *Accept:* `Docs.AgentGuide` passes
(AGENTS.md under 40,000 characters); the docs describe box3d and no Jolt behavior.

**D13 Samples.** After `showcase-samples` lands on main and is merged into this branch: its samples' regression command
scripts (and `Editor/TetrisCommands.json.in`) pass on box3d. *Accept:* they pass; every gameplay difference box3d's
tuning causes (bounces, friction, character feel, continuous collision) is listed in the report, and fixed in the engine
when it is a defect.

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
- `Docs/DeveloperGuide.md`: Building (the `STRATA_USE_*` options), Optional modules, Perf
  tests (the physics metrics' names), Test rules (the broad phase helper), Working with third-party code (box3d's
  wrapper: why it is built from its sources, `-ffp-contract=off`, the golden hash on upgrade).
- `README.md` (Physics line), `ThirdPartyNotices.md`, `Docs/Roadmap.md` (Status when the stream lands; the open items:
  triggers and triangle meshes, the reference machine's metrics; "Later": joints, ragdolls and vehicles now on box3d's
  joints, height fields for `world-environment`).
- Skills: `strata-editor-automation` (`physics.stats` fields), `strata-scripting` (any character feel or physics
  tuning a script author sees: step and slope behavior, bounce threshold), `strata-make-a-game` (the same, where it
  advises values), `strata-build-test` (the golden hash on box3d upgrades).
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
| Small shapes: box hulls clamp half widths (`b3MakeTransformedBoxHull`: 1 mm, `b3MakeScaledBoxHull`: 2 cm) | `b3MakeTransformedBoxHull` with Strata's own scaling, matching today's 1 mm `c_MinColliderExtent`; "A 1 cm box collider keeps its size" |
| Mesh contacts keep at most `B3_MAX_MESH_CONTACT_TRIANGLES` (512) triangles per convex shape and mesh, silently | large bodies on dense render-mesh colliders (terrain tiles) could lose support: "A large crate rests on a dense mesh floor"; if it fails, the wrapper raises the limit (`#ifndef`) |
| An offset triangle-mesh collider cannot be a sensor (baked compounds are static solids only) | unusable with a warning, tested; a mesh collider at its trigger body's origin works |
| box3d's world slots are not thread-safe | worlds are created and destroyed on the main thread only (4) |
| CI meets box3d's glue code only at D4 | D1 and D3 put the library and the standalone pieces on all six jobs first; D4 is pushed to CI right away, and nothing merges before it is green |

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

1. D1 vendor box3d (submodule, wrapper, notices), its own commit; push `ci/physics-box3d` to see MSVC build it.
2. D2 Strata's CPU options and the neutral names.
3. D3 box3d's runtime hooks, the task system and the golden hash case, next to Jolt; CI.
4. D4 (with D5-D7) the module on box3d, one commit that passes every suite (D8); CI.
5. D9 perf A/Bs against the base build; provisional entries.
6. D11 remove Jolt and its model (API, scene key, commands, Inspector, font test, budgets entries).
7. D12 docs and skills (each behavior's docs go with its commit; this item finishes them).
8. D13 after `showcase-samples` lands: merge main, run its samples' command scripts on box3d, fix or report.
9. Final: full Debug and Release suites, lint, CI green, report.

If time runs short, stop after a complete step: before D4 the branch has box3d next to Jolt; D4 must not merge without
D11 following before the stream merges.

## 12. Merge notes

- No backend switch (5.1): the branch never has two physics backends; until D11, Jolt is still vendored.
- Shared files touched at function level: `CMakeLists.txt` (the option's description), `Strata/CMakeLists.txt` (the
  Physics link, the codegen block), `Strata/vendor/CMakeLists.txt` (the Jolt block becomes box3d's),
  `CMake/StrataCompilerOptions.cmake`, `.gitmodules`, `ThirdPartyNotices.md`, `StrataTests/Perf/Budgets.json` (four
  entries), `StrataTests/CMakeLists.txt` (the test-only box3d scenario library), `AGENTS.md` (one line, maybe one
  rule), the docs' physics paragraphs, `SceneSerializer.cpp` and `Scene.h` (`MaxBodyPairs`), `EditorSceneCommands.cpp`,
  `EditorWorldCommands.cpp`, `InspectorPanel.cpp`, `StreamingPanel.cpp`.
- `showcase-samples` lands first: merge main before D13; its samples use only the engine's API, so conflicts are
  limited to docs and `Budgets.json` lines.
- The script ABI and the feature test's scene are unchanged; no host function is added.
