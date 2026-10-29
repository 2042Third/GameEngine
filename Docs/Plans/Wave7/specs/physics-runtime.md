# Stream `physics-runtime`: physics that runs fast and settles, at the scale of Strata's worlds (wave 7)

The owner (2026-10-27, verbatim in `decisions.md`, last section): "I don't think physics object creation performance is a
big deal. But i care about runtime performance. For large box3d physics be sure to use box3d's contact recycling. With a
contact recycling of 0.7 or something will put more entities in sleep and making something more stable". The
orchestrator's reading there: body creation and loading (`Perf.Physics.Create10kBodies`, `Create100kStaticBodies`) are
not release blockers and get no wave-7 effort beyond being recorded; runtime is what counts (the step, Strata's glue
around it, hitches while playing, sleeping and stability); contact recycling is measured, exposed as a project physics
setting, turned off for character controllers, and the owner's "0.7" is found by measurement.

This stream is the wave's fifth. It is not in `plan.md`'s stream table (section 4), which predates the owner's words; it
touches no renderer file, no shader and no GPU test, so it neither waits for nor blocks the four rendering streams.

This spec is written against main at bc6ee123 (release 0.6.0). Every file:line below is bc6ee123's unless it names step
0, whose branch `agent/step0` (tip 42cb13bd, 31 commits, not landed when this was written) is cited as "step 0's tip";
its names and numbers are unverified until it lands, and where it lands with other names the implementer uses them and
keeps this design (`decisions.md`: the first commit's message and this spec's copy in `Docs/Plans/Wave7/specs/` record
any name found different).

Rules of the machine (AGENTS.md and `plan.md` 4.1, restated where they bind this stream):

- Worktree `.worktrees/physics-runtime` from step 0's tip, build directory `build/agent-physics-runtime` (never
  `build/windows`, which the owner runs), and `build/agent-physics-runtime-base` for the A/B base (section 7).
- Every commit: Debug build, Debug ctest (this stream adds no GPU work, but `ctest --preset windows-debug` runs the GPU
  suites too, under the validation layer, and they must stay green), `Tools/CompatLint.py` on the changed files. CI on
  `ci/wave7-physics-runtime` before landing (Linux GCC, macOS Clang, the no-physics configuration).
- Perf: no campaign and no `PerfStability.py` runs in the stream; A/Bs of its own metrics under `Tools/PerfLock.py`
  (section 7); new metrics enter `Budgets.json` as provisional entries with their targets.
- Read box3d (`Strata/vendor/box3d`, pinned at efca0c1) to learn its API; never copy it and never commit a change to the
  submodule (D8).
- Samples change only through editor commands; tests never open samples in place (`Tests::CopySampleProject`).

## 1. Goal, in priority order

1. **A step that costs less.** Strata's part of a fixed step (synchronization, characters, events, write-back, pair
   upkeep) falls from 0.60, 0.97 and 0.74 of box3d's own step (Boxes10k, Boxes30kTracked, Boxes70k) to at most 0.45,
   0.75 and 0.55; box3d's own step gets cheaper where Strata chooses for it (its worker count, contact recycling); the
   busiest frame of 70,000 falling boxes from 16.05 ms to at most 13.5 ms; 1,000 walking characters from 0.86 to at most
   0.70 ms a step. Sleeping bodies cost nothing measurable: a world of 70,000 sleeping and 1,000 awake bodies steps in at
   most 1.2 times the time of the 1,000 alone.
2. **Piles and stacks that settle.** The owner's request: contact recycling at a distance chosen from measurements on
   the engine's representative scenes and samples, so that resting bodies stop jittering and fall asleep sooner (a pit of
   2,000 balls, towers, the Physics Playground's stations), without ghost collisions (sliding and rolling across seams,
   characters, platforms). Each reading of "0.7" is measured and answered with numbers.
3. **No hitch that physics causes while playing, beyond creating bodies.** The 132.6 ms of `Grow65kMaxFrame` is split
   into creation (the owner's ruling: recorded) and growth (this stream's: at most 4 ms or explained); the Strata share
   of a cell's unload frame comes down to at most 1.25 times box3d's own removal time; contact storage that grows while
   playing is seen and sized ahead.
4. **Visible as data.** Awake and sleeping bodies, islands, contacts touching and recycled, box3d's stage times and
   Strata's parts, and a settling measure (residual speed, resting jitter, time to sleep) in `physics.stats`,
   `editor.status`, the runtime's perf log, the editor's Streaming panel and the perf lab, at constant cost per frame.
5. **Tunable by people and agents.** A project physics section (`Physics.Simulation`) for the recycle distance and the
   sleep threshold (and the contact tuning, if the sweep shows it matters), changed while playing with
   `project.setSettings`; character controllers' bodies never recycle.
6. **Recorded.** What creating and loading bodies costs on box3d, box3d's share and Strata's, in the roadmap's open
   items as the box3d port's cost, not optimized (owner's ruling).

Not the goal: creation and loading speed (owner, 2026-10-27; wave 5's owner ruling of 2026-10-20 said the same), bulk
insertion in box3d (an upstream change), joints or other new physics features, the renderer (the four other streams),
changes to box3d's sources, and slicing body creation over frames (wave 5's rule stands: every body gets its collision in
the update that creates it, `DeveloperGuide.md:1814-1821`).

## 2. Where we start

Paths: `P/` is `Strata/src/Strata/Physics/`, `B3/` is `Strata/vendor/box3d/`, `DG` is `Docs/DeveloperGuide.md`, `ED/` is
`StrataEditor/src/`, `T/` is `StrataTests/src/`.

### 2.1 Files

| File | Lines | Role for this stream |
| --- | --- | --- |
| `P/PhysicsWorld.cpp` | 5,073 | The world: constructor (4229-4287: `b3DefaultWorldDef` 4240, `enableSleep`/`enableContinuous` 4251-4252, the room 4253-4266, `b3CreateWorld` 4268), `MakeBodySetup` (1756-1806, `b3DefaultBodyDef` 1749, contact and sensor events on every shape 1772-1778), `Simulate` (4524-4601), `SyncEntitiesToBodies` (3176-3258), `ProcessStepEvents` (3380-3443), `CollectCheckedPairs` (3446-3470), `RefreshCheckedPairs` (3473-3491), `StepCharacters` (3788-3828), `WritePose`/`WriteBackDynamicBodies` (3866-4060), `ApplyChanges` (2736-2783), `GrowRoom` (2051-2067), `RemoveFromSimulation` (1363-1376), `GetStats` (5008-5072), `SanitizeSettings` (670-680), `c_SolverSubSteps` (67) (owned) |
| `P/PhysicsWorld.h` | 272 | The world's API and contract (owned) |
| `P/PhysicsSystem.cpp` / `.h` | 632 / 214 | The "Physics" scene system: services (58-67), `OnFixedUpdate` (162-174), `SyncLayers` (286-293), `GetStats` (295-300), `MakeWorldSettings` (504-517), `PrepareLoadedScene` (519-541), `CreateWorld`/`AdoptWorld` (549-578) (owned) |
| `P/PhysicsTypes.h` | 186 | `PhysicsSettings` (25-71), `PhysicsStats` (88-150) (owned) |
| `P/PhysicsTaskSystem.cpp` / `.h` | 99 / 67 | box3d's tasks as JobSystem jobs (13-48), `Configure`, `GetWorkerCount` (61-74) (owned) |
| `P/CharacterControllers.cpp` / `.h` | 1,069 / 124 | Presence bodies (`CreatePresenceBody` 292-305, `MovePresenceBody` 307-315), `CharacterControllerSet::Step` (909-971) (owned) |
| `P/PhysicsRegistration.cpp` | 25 | `RegisterPhysicsModule` (10-23): the section registers here (owned) |
| `P/PhysicsComponents.cpp` / `.h` | 187 / 87 | `RigidBody` registration (121-136): a property only if step 3 adds one (owned) |
| `Strata/src/Strata/Scene/SceneServices.h` | 547 | `PhysicsCapacityInfo`/`IPhysicsCapacity` (384-411); `IPhysicsDiagnostics` joins them (owned: no other stream edits it) |
| `Strata/src/Strata/Scene/Scene.cpp` | 2,303 | The fixed-step loop (2141-2201), `MaxFixedStepsPerFrame` (`Scene.h:63`) (read only) |
| `Strata/src/Strata/Runtime/GameRuntime.cpp` | 368 | The perf log's world and physics columns (329-366) (shared: `graphics-options` owns the file; this stream appends to that group only) |
| `ED/Editor/EditorWorldCommands.cpp` | 978 | `DescribePhysicsStats` (321-362), `physics.stats` (960-975) (owned) |
| `ED/Editor/CommandUtils.cpp` | 620 | `DescribeRunningScene` (338-...), used by `editor.status`, `play.state` and `scene.info` (owned hunk) |
| `ED/Panels/StreamingPanel.cpp` | 920 | Physics section (268-273, 366-376, `DrawPhysics` 450) (owned) |
| `T/Perf/PhysicsPerfTests.cpp` | 492 | `Perf.Physics` (falling boxes 77-159, characters 286-338, Grow65k 340-392, Create100k 394-441, Create10k 443-491) (owned) |
| `T/Perf/SceneGenerators.cpp` / `.h` | 281 / 104 | `CreateBoxStacks` (93-124; stacks of `c_BoxStackHeight` 10 with 0.2 m gaps, `.h:43`) (owned) |
| `T/Perf/WorldPerfTests.cpp` | | `Perf.World.CellUnloadMaxFrame` (91-134), drive metrics (owned cases) |
| `T/Physics/*` | 7,740 in 16 files | `Physics.Simulation` (awake-only work 835-912, identical runs 773), `Physics.Character` (push 287, threads 573), `Physics.Box3D` golden hash (`PhysicsBox3DTests.cpp:105`) (owned) |
| `T/TestMain.cpp` | | Helper modes (`world-drive` at 894): `physics-settle` joins them |
| `StrataTests/Perf/Budgets.json` | 213 | Physics entries at step 0's tip: 101-113, 181-187 (shared) |
| `StrataTests/FeatureTest/FeatureTest.stproj` | | `Settings` (18): the new section's non-default value (shared) |
| `StrataTests/Editor/PhysicsPlaygroundCommands.json.in` | | The Playground's regression: settle 180 steps, tower drift, ghosts, dominoes, pour (shared) |
| `B3/include/box3d/constants.h`, `types.h`, `box3d.h`; `B3/src/types.c`, `physics_world.c`, `contact.c`, `body.c`, `solver.c` | | Read only: section 2.3 |

### 2.2 A fixed step today

- **The loop.** `Scene::OnUpdateRuntime` runs fixed steps of `SceneSettings::FixedTimestep` (1/60 s), at most
  `MaxFixedStepsPerFrame` (8) a frame, counting the steps it drops (`Scene.cpp:2141-2201`). Physics is the "Physics" scene
  system, after Scripting, under the frame stage `Scene.Physics` (`PhysicsRegistration.cpp:16-20`). Each fixed step
  `PhysicsSystem::OnFixedUpdate` applies pending changes, gravity and the layer matrix, calls `PhysicsWorld::Simulate` and
  dispatches the step's collision events to the listeners on the main thread (`PhysicsSystem.cpp:162-174`, 606-630);
  every frame `OnUpdate` applies changes and dispatches again (156-160).
- **`Simulate`** (`PhysicsWorld.cpp:4524-4601`), in order: `ApplyChanges` (component, activity, hierarchy and destruction
  changes in rounds; at least 256 bodies to build go to the job threads), `GrowRoom` (2051-2067: bookkeeping only, see
  "Growth" below), `HoldForMeshes`, `SyncEntitiesToBodies` (3176-3258: the bodies awake after the last step from
  `AwakeBodies`, a parallel skip of those whose entity is where the last step put it from 1,024 awake bodies on, the rest
  sorted and synchronized one by one; kinematic bodies get `b3Body_SetTargetTransform`, dynamic ones moved from outside are
  teleported), `StepCharacters` (3788-3828), `CollectCheckedPairs` (3446-3470: one `EntityPairs` hash lookup per awake
  body every step, also when no pair is tracked), then `CollisionSteps` (default 1) times: pending forces,
  `b3World_Step(subStep, c_SolverSubSteps = 4)` (4563, constant at 67), `ProcessStepEvents` (3380-3443: every move event
  marks its body in a bitmap and the awake list; every contact and sensor begin event resolves its two shapes and asks
  `IsWanted`, 3266-3270, and every end event resolves its shapes and looks its pair up in `TouchingPairs`, `CountEnd`); then `WriteBackDynamicBodies` (3909-4060), the pair upkeep (`ResolveCharacterPairs`,
  `ResolveRecheckedPairs`, `EndUnwantedPairs`, `SeedWantedPairs`, `RefreshCheckedPairs`) and the contact demand
  (`b3World_GetMaxCapacity`, 4589-4592).
- **Timing** (4594-4600): `SyncTime` is from the start to the first box3d step (changes, sync, characters, checked pairs);
  `LibraryStepTime` the sum of `b3World_Step`; `WriteBackTime`; `ContactTime` the event processing inside the step loop
  plus everything after the write-back. The perf lab's `StrataStepTime` is `LastStepTime - LibraryStepTime`
  (`PhysicsPerfTests.cpp:117`) and `ActiveFrame` the whole `OnUpdateRuntime` (108-110), which also holds the scene's world
  transforms recomputed after the write-back (`Scene.Transforms`) and the other systems.
- **The task system** (`PhysicsTaskSystem.cpp:13-48`): box3d splits each stage into one task per worker; each task is a
  JobSystem job at `JobPriority::High`, and the finish callback waits on it, running it inline if no worker started it.
  The worker count is the JobSystem's workers plus the stepping thread, at most `B3_MAX_WORKERS` (32) (61-74); the
  JobSystem's default is the hardware threads minus one (`JobSystem.h:64`), so 24 on the reference machine's i7-13700K
  (8 performance cores with two threads each and 8 efficiency cores). box3d's own advice (`B3/include/box3d/types.h`,
  the `workerCount` comment of `b3WorldDef`): "Box3D performs best when using only performance cores and accessing a
  single L2 cache. Efficiency cores and hyper-threading provide little benefit and may even harm performance."
  `b3World_SetWorkerCount` changes it at run time (`B3/src/physics_world.c:2294-2310`). Never measured here.
- **Write-back** (3909-4060): the moved bodies in body order from the bitmap; a serial prelude per write on the main
  thread (`registry.get<HierarchyComponent>`, `get<TransformComponent>`, the parent's world transform, 3943-3974); from
  512 writes the poses in parallel chunks (3976-3992), each reading `b3Body_GetTransform` (`ReadBodyPose`, 3847-3853), a
  random access through box3d's body and solver-set arrays, although box3d's move event of the same body already carries
  that transform (`b3BodyMoveEvent::transform`, `types.h:1233-1246`; one event per awake body every step,
  `B3/src/solver.c:760-767`); the outcomes serially in body order, one `Scene::MarkTransformsChanged` for all, then
  descendants and nested dynamics serially.
- **Contacts and sensors.** Every solid shape enables contact events and every shape sensor events (1772-1778), because
  box3d fixes a contact's event flag when the contact is made (`B3/src/contact.c:241-244`); Strata drops the pairs nobody
  wants and seeds a pair that becomes wanted later from box3d's contact data (`SeedWantedPairs`, 3672). So box3d reports
  every begin and end, and Strata resolves each, also in a scene where nobody listens. `TrackAll` is on only while an
  `AllEntities` listener exists (`PhysicsSystem::UpdateContactTracking`, `PhysicsSystem.cpp:220-227`); scripts' collision
  callbacks use a `WatchedEntities` listener (`ScriptSystem.cpp:99`), so a game with any watched entity has a non-empty
  `ContactInterest` and resolves every begin event.
- **Characters** (`CharacterControllerSet::Step`, `CharacterControllers.cpp:909-971`): a main-thread prelude per
  controller (its world transform, a teleport if moved from outside, its parent's inverse), the moves in parallel batches
  on box3d's mover queries, then per controller on the main thread the write-back, the presence body teleported
  (`b3Body_SetTransform` and `SetAwake`, 307-315) and the pushes. Presence bodies are kinematic bodies made from
  `b3DefaultBodyDef` (292-305), so they recycle contacts. box3d pairs a kinematic body only with dynamic ones (its broad
  phase collides the dynamic tree with itself and with the static and kinematic trees, `B3/src/broad_phase.c:614`,
  682-688; no kinematic-static or kinematic-kinematic pairs), so presence bodies have contacts only with dynamic bodies
  (a crate on a head, a pushed box), never with floors, steps or each other: `Characters1000Step`'s crowd on static
  ground has none. Their teleport (`b3Body_SetTransform`, `B3/src/body.c:1142-1192`) does not clear a contact's recycle
  cache, so a step's move shorter than the recycle distance keeps the contact's manifold.
- **Growth.** A world reserves its room at creation (4253-4266: up to `c_MaxBodiesReservedAtCreation` 131,072 bodies and
  262,144 contacts, or the automatic room of the scene's bodies when larger); box3d has no run-time reserve (only
  `b3WorldDef::capacity`; `b3World_GetMaxCapacity`, `box3d.h:244-245`), so beyond it box3d's own arrays grow while bodies
  and contacts are created. `GrowRoom` only counts a growth and records the demand that sizes the scene's next world.
  `Perf.Physics.Grow65kMaxFrame` times the update that creates 65,536 bodies (90% static) in a world reserved for 65,536
  (`PhysicsPerfTests.cpp:340-392`): creation and growth in one frame.
- **Stats.** `PhysicsWorld::GetStats` walks every record to count bodies by type (5008-5072): fine for `physics.stats`,
  never per frame. The constant-time path is `IPhysicsCapacity` (`SceneServices.h:384-411`), which the runtime's perf log
  (`GameRuntime.cpp:329-366`: `PhysicsBodies`, `...Waiting`, `...Pending`, `PhysicsMaxBodies`) and the Streaming panel
  (`StreamingPanel.cpp:268-273`) read. Nothing reports islands, sleeping bodies, recycled contacts, box3d's stage times
  or how a pile settles.
- **Settings.** `PhysicsSettings` (`PhysicsTypes.h:25-71`: capacities, `CollisionSteps`, `MeshWaitTimeout`, `GrowthLimit`,
  the layer masks) is a process default (`PhysicsSystem::SetDefaultSettings`, used by tests and tools) that scenes
  override for capacities (`Scene.h:44-77`). The only project physics section is `Scene.CollisionLayers` (DG 988-1036).
  World and bodies take box3d's defaults for everything this stream is about.

### 2.3 Contact recycling, sleep and contact tuning in box3d (efca0c1)

- **The distance.** `B3_CONTACT_RECYCLE_DISTANCE` is 10 times `B3_LINEAR_SLOP` (5 mm), so 5 cm (`constants.h:47`,
  83-84), set when a world is made (`physics_world.c:339`), changed at any time with `b3World_SetContactRecycleDistance`
  (2178-2190: clamped to at least 0; 0 turns recycling off) and read by the collide task every step (601). Contacts that do
  not touch yet use at most `B3_SPECULATIVE_DISTANCE` (4 slops, 2 cm; `constants.h:74`, `physics_world.c:603`), so a
  distance above 2 cm only lengthens the recycling of touching contacts.
- **The angle.** `B3_CONTACT_RECYCLE_ANGULAR_DISTANCE` 0.99240388 is cos^2 of half of 10 degrees (`constants.h:86-88`); it
  is a plain `#define` without `#ifndef`, so it cannot be changed without editing box3d (D8).
- **When a contact is recycled** (`physics_world.c:649-737`): neither body is fast (moves more than `safetyFactor` times
  its smallest extent in a step, `solver.c:789-797`), the contact has a cached relative pose, both bodies enable
  recycling and the contact has speculative points (`contact.c:251-256`; `enableSpeculativeContact` defaults to true,
  `types.c:83`), each body turned less than 10 degrees since the cache, the relative translation is within the distance,
  and the arc of the relative rotation over the bodies' extents fits the remaining slack. A recycled contact keeps its
  manifold's anchors and normal and moves each point's separation linearly, "same as sub-stepping. This eliminates
  jitter" (box3d's comment, 705), sets the points' `normalVelocity` to 0 (719) and skips the narrow phase and the material
  refresh (`continue` at 734). `normalVelocity` is the approach speed hit events report, refilled by the contact prepare
  when hit events are on; restitution is armed from the relative velocity the prepare samples from the bodies every step
  (`B3/src/contact_solver.c:143`, 215-221, 755; `contact_solver_wide.inl:843-860`, 1434-1438), so a recycled contact still
  bounces: what it keeps stale is the geometry (anchors, normal). Because the `continue` comes before the touching-state
  update (765-776), a recycled contact neither begins nor stops touching: no begin or end event and no island link change
  while it is recycled. Mass changes and pre-solve rejections drop the cache (`body.c:1062-1074`, 1966-1977;
  `contact.c:705-718`); a teleport (`b3Body_SetTransform`, `body.c:1142-1192`) does not, so a teleport shorter than the
  distance and the angle keeps the old manifold.
- **Per body.** `b3BodyDef::enableContactRecycling` (`types.h:362-364`, true by default, `types.c:47`), whose comment says
  recycling "improves performance but may lead to ghost collision that should be avoided on characters";
  `b3Body_EnableContactRecycling` (`body.c:2526-2548`) changes a body later, but "Existing contacts retain their prior
  setting; only contacts created after this call see the new value" (`box3d.h:753-757`).
- **Counters.** `b3Counters::recycledContactCount` (`types.h:604-605`) sums the workers' counts of the last collide pass
  (reset at 889, summed at 2660-2668). The reset sits after `b3Collide`'s early return for a step without awake contacts
  (844), so once every contact sleeps the counter keeps the last pass's value instead of reading 0: Strata must report 0
  for a box3d step after which `awakeContactCount` is 0 (exact for the stale case; it misses only the recycled contacts
  of the step in which the last island fell asleep). `awakeContactCount` is computed when `b3World_GetCounters` is called,
  after the step (`types.h:600-602`, 2650-2658): the graph's and the awake set's contacts as they are then, which can be
  fewer than the pass visited (islands that fell asleep in the solve, contacts destroyed as disjoint), so the ratio is
  the recycled share only approximately and recycled can exceed it in a step where an island fell asleep; recycled never
  exceeds `contactCount` (all contacts; a recycled contact is never destroyed in its step). `b3World_GetCounters` also
  computes `byteCount`, which walks every island, sensor and shared hull (2347-2420, 2645): the reason `PhysicsWorld`
  calls it only for diagnostics today (`PhysicsWorld.cpp:4472-4484`). `b3World_GetProfile` is a copy of the last step's
  stage times (2240-2248; `b3Profile`, `types.h:553-578`), reset at every step's start (1067). Its top-level stages are
  `pairs`, `collide`, `solve` and `sensors` (1099, 1138, 1146, 1162; `step` is their sum plus small overhead); `refit`,
  `bullets`, `sleepIslands`, `transforms`, `constraints`, `solverSetup` and `hitEvents` are measured inside `solve`
  (`solver.c:1888-2333`), and `splitIslands` in a task that runs concurrently with the solve (`island.c:658-671`,
  `solver.c:1803-1813`), so the sub-stages are parts of `solve`, never summed with it.
- **Peak, not room.** `b3World_GetMaxCapacity` returns the largest counts seen at the start of the steps so far
  (`physics_world.c:1078-1093`), not box3d's storage: storage is reserved once from `b3WorldDef::capacity` (265-310) and
  grows (array doubling) only when a count passes it.
- **Sleep.** A body's `sleepThreshold` is 0.05 m/s (`types.c:41`; `b3Body_SetSleepThreshold`, `box3d.h:712`); an island
  sleeps when all its bodies stayed below theirs for `B3_TIME_TO_SLEEP`, 0.5 s, a constant (`constants.h:100`,
  `solver.c:831`). One moving body keeps its whole island awake: a pit of touching balls sleeps only when every ball does.
- **Contact tuning.** `contactHertz` 30, `contactDampingRatio` 10 and `contactSpeed` (the overlap push-out speed) 3 m/s
  (`types.c:21-23`), changed at run time with `b3World_SetContactTuning` (`physics_world.c:2163-2176`).
- **Rolling.** `b3SurfaceMaterial::rollingResistance` (`types.h:426-427`, spheres and capsules) is 0 in every Strata
  shape: balls on a floor roll on with only their damping (0.05).

So, against the owner's sentence: Strata already uses box3d's contact recycling, at box3d's defaults (5 cm, 10 degrees),
on every body including the characters' presence bodies; nothing turns it off, nothing measures it and nothing exposes
it.

### 2.4 Measurements that set the targets

From the campaign on the reference machine (`perf-campaign-bc6ee123.log`, five `-L perf` runs of bc6ee123's Release
build, the machine 19-24% busy; medians of the five runs' deciding attempts; box3d's own step from the tests' messages in
`perf-campaign-bc6ee123/run*.log`):

| Metric | Median (min-max) | Budget or target at step 0's tip |
| --- | --- | --- |
| `Boxes10k.ActiveFrame` / `.StrataStepTime` / library step | 1.811 (1.697-2.432) / 0.557 (0.540-0.709) / 0.915 (0.898-1.225) ms | 3.3 / 0.7581 ms |
| `Boxes10k.StrataToLibraryStep` | 0.599 (0.578-0.620) | 0.8982 |
| `Boxes30kTracked.ActiveFrame` / `.StrataStepTime` / library | 6.798 (6.589-7.477) / 2.998 (2.870-3.433) / 3.168 (2.995-3.553) ms | 10.19 / 3.816 ms |
| `Boxes30kTracked.StrataToLibraryStep` | 0.966 (0.906-1.041) | 1.449 |
| `Boxes70k.ActiveFrame` / `.StrataStepTime` / library | 16.05 (15.70-17.14) / 6.378 (6.336-7.256) / 8.622 (8.501-9.243) ms | 24.07 / 8.809 ms |
| `Boxes70k.StrataToLibraryStep` | 0.745 (0.739-0.785) | 1.116 |
| `Boxes70k` busy frames' p95 (the tests' messages) | 22.9 ms (21.6-28.7), 1.43 times the median | none |
| `Characters1000Step` (controllers / library) | 0.864 (0.846-0.929) ms (0.439 / 0.146) | 1.2 ms |
| `Boxes30k.StartAnd30Frames` | 206.3 ms | 309.4 ms |
| `Grow65kMaxFrame` | 132.6 (130.6-134.5) ms | budget 198.8, target 16.7 |
| `Create10kBodies` | 15.76 ms, over budget in every attempt (134%) | 11.99 ms (Jolt's 7.995 x 1.5) |
| `Create100kStaticBodies` | 166.7 (165.8-167.6) ms | budget 250, target 25 |
| `Perf.World.CellUnloadMaxFrame` (the request frame) | 7.92 (7.73-8.70) ms | budget 11.87, target 4 |
| `Perf.World.CellDriveMaxFrame` / `PerfGPU.World.CellDriveMaxFrame` / `DriveFrameP95` | 7.26 / 8.885 / 4.67 ms | 8 / 13.32 (target 8) / 446 MB peak |

What they say:

- **Strata's glue per box and step** (the busy phase; the test checks only that more than half the boxes are awake,
  `PhysicsPerfTests.cpp:126`, and these figures divide by every box, so they are lower bounds per awake box): 56 ns at
  10,000, 100 ns at 30,000 with the floor's contacts tracked, 91 ns at 70,000; box3d's own: 92, 106 and 123 ns. The frame
  beyond the step (the scene's transforms after the write-back and the other systems) is about 0.34, 0.63 and 1.05 ms.
- **Busy frames hitch within themselves**: p95 1.3-1.7 times the median at 70,000 (the scattered library times of run 1
  say the job system's placement on the hybrid processor plays a part; unverified).
- **`Grow65kMaxFrame` is mostly creation.** Creation costs 1.58 us per dynamic box (`Create10kBodies`) and 1.67 us per
  static box (`Create100kStaticBodies`); at those rates the 65,536 bodies of the growth frame take about 104-109 ms of its
  132.6 ms, so about four fifths of that "growth" hitch is creating bodies, which the owner ruled out of scope, and at most
  about 25 ms is box3d's arrays growing plus the step (an estimate from two other metrics; step 1 measures the split).
- **The unload frame** spends 7.9 ms taking 25,000 static bodies out of the simulation (plus the scene's own work); box3d
  alone took 2.0-2.5 ms of 2.6-4.4 ms for the same on the Mac in wave 5 (`Docs/Roadmap.md:861`): Strata's share on
  Windows is unknown (step 1). The metric is the longest frame of the whole unload, the request's and the 11 destroying
  frames' (`WorldPerfTests.cpp:118-131`); the request's frame was the longest in all five runs (the tests' messages).
- **Unknown until step 1**: how Strata's 6.4 ms at 70,000 splits (sync, events, checked pairs, write-back, pair upkeep);
  box3d's stages (pairs, collide, solve, islands); the recycled share at 5 cm; how long the Playground's pit and tower take
  to sleep, and whether they do; whether any perf scene or sample grows box3d's contact storage while playing; what
  `b3World_GetCounters` costs at 70,000 boxes (7,000 islands once the stacks stand, up to 70,000 at the busy phase's start:
  every box is its own island until it lands on the one below, 0.2 m down).

### 2.5 What step 0 provides (assumed)

From step 0's tip (unverified until it lands):

- `Budgets.json`'s physics entries measured on the reference machine: `Boxes70k.ActiveFrame` 16.05 (budget 24.07), the
  three `StrataToLibraryStep` ratios measured (0.5988, 0.9663, 0.7446; budgets 0.8982, 1.449, 1.116), `Grow65kMaxFrame`
  132.6 (198.8, target 16.7), `Create100kStaticBodies` 166.7 (250, target 25), `Perf.World.CellUnloadMaxFrame` 7.92
  (11.87, target 4), and `Create10kBodies` left at 7.995 / 11.99 and failing (`Budgets.json:101-113`, 181-187; commit
  ba279ffb's message: measured alone 16.50 ms against wave 3's build's 9.07 ms on this CPU).
- The roadmap's open items (`Docs/Roadmap.md:855-882` at step 0's tip) name `Create10kBodies` a blocker of 0.7.0, written
  before the owner's ruling of the next morning; step 5 reconciles it (section 10, Q1).
- `Tools/PerfStability.py --baseline <files> --results <files>` (medians of two builds' results and their ratios), used by
  this stream's A/Bs.
- AGENTS.md at 37,213 of its 40,000 code points, 2,787 left for the wave's streams' digest bullets.

## 3. Decisions that hold for every step

**D1 Determinism.** Results stay independent of the number of threads (`Physics.Simulation`'s identical runs, 773;
`Physics.Character`, 573; the golden hash, `PhysicsBox3DTests.cpp:105`, which runs box3d's own scenario and is untouched by
Strata's settings). Every new reduction (settling statistics) runs in body order on the main thread or in fixed chunks
reduced in chunk order. A setting changes results only when it changes, and only between steps on the main thread
(`b3World_SetContactRecycleDistance` asserts an unlocked world, `physics_world.c:2181-2186`).

**D2 Per-frame instruments cost constant time.** What the perf log, the Streaming panel and `editor.status` read every
frame is filled during the step from data the step already touches; walks of every record (`GetStats`) stay for
`physics.stats` and tests. The instruments add at most 1% to `Boxes70k.StrataStepTime`, timed in process (4.1's
acceptance: an A/B of 3 pairs cannot resolve 1%; it is reported as information, section 7).

**D3 Where each setting lives.**

| Setting | Kind | Values | Step |
| --- | --- | --- | --- |
| `Physics.Simulation.ContactRecycleDistance` | project section field (and `PhysicsSettings`) | meters, 0-1, 0 off (the range holds every reading of the owner's "0.7", 0.7 m included, so that any of them is one command); default box3d's 0.05 until step 3 sets Strata's from the sweep | 2, 3 |
| `Physics.Simulation.SleepThreshold` | project section field (and `PhysicsSettings`) | m/s, 0-1; default box3d's 0.05 unless step 3's rule changes it | 2, 3 |
| `ContactHertz`, `ContactDampingRatio`, `ContactPushSpeed` | `PhysicsSettings` (process default: tests, the sweep helper); project fields only if step 3 shows they matter | box3d's 30, 10, 3 m/s | 2, 3 |
| Presence bodies' recycling | fixed: off | box3d's advice | 2 |
| Kinematic bodies' recycling | fixed: on, off only if step 3's platform probe fails | | 3 |
| `RigidBody.ContactRecycling` | component property, only if step 3 finds a body kind it fixes | bool, default true | 3 |
| box3d's worker count | engine rule in `PhysicsTaskSystem::GetWorkerCount` | from step 4's sweep | 4 |
| Per-scene overrides | none (below the cut) | | |

Scripts get no host function: tuning is the project's, and ABI additions are permanent. A per-body property, if it lands,
reaches scripts through `Entity::GetProperty`/`SetProperty` like every component property, without an ABI change. Agents
reach the section through `project.setSettings`, `project.settings` and `physics.stats` (whose `settings` reports the
values in effect and their source).

**D4 Precedence.** A world uses the project's `Physics.Simulation` value when the program provides `IProjectSettings` and
the project stores the section; otherwise the process defaults (`PhysicsSettings`, whose defaults are the section's).
`PhysicsSystem` reads the section on the main thread only (`IProjectSettings`' contract, `Core/ProjectSettings.h:20-35`),
at world creation (also for a prepared world) and before a fixed step when the provider's version changed, keeping what
it parsed with that version as `ProjectCollisionLayers::Find` does (`Scene/ProjectCollisionLayers.cpp:91-111`).
(`SyncLayers`, `PhysicsSystem.cpp:286-293`, is not that pattern: it compares the scene's copy of the matrix every step,
the copy `Scene::ApplyProjectLayers` refreshes, `Scene.cpp:216-225`.) Physics may include Core (`Layers.json`: Physics'
`MayInclude` is Core, Asset, Reflection, Scene), so no layer changes.

**D5 Every body gets its collision at once.** Nothing in this stream slices creation, delays a body's collision or
leaves an inactive entity's body in the simulation; hitch work is cut or moved off the main thread, never deferred into
missing collision.

**D6 Deterministic quantities are tests, timings are perf metrics.** Time to sleep in simulated seconds, recycled counts,
drift and ghost-probe excursions are deterministic for a build on a platform, whatever the thread count (D1), and are
bounded in unit tests that run in CI; across compilers they may differ in the last bits (box3d is built without
floating-point contraction for its golden hash, `PhysicsBox3DTests.cpp:31-35`, but Strata's own transform math around it
is not held to that), which is why the bounds carry a margin (4.3) and run on every CI platform. CPU times are perf
metrics (provisional) and A/Bs under `PerfLock.py`.

**D7 No renderer.** No file under `Strata/src/Strata/Renderer/`, `Strata/shaders/` or the GPU tests changes; this stream
adds no GPU work, no debug view and no quality row.

**D8 box3d stays as pinned.** No edit of the submodule is committed. What box3d does not offer (a settable angle, a run-time
reserve, bulk insertion) goes to the roadmap's "Later" as an upstream request. A local, uncommitted edit of a box3d constant
for one measurement is allowed only where section 4.3 says so, in a separate throwaway worktree with its own build
directory (never the stream's worktree or its submodule checkout, which a later commit could pick up), removed after the
measurement, and is named in the commit message that reports it. A compile definition cannot replace the edit: the
angle is a plain `#define` that would redefine it (`constants.h:88`).

## 4. Steps

Each step lands working, tested and documented before the next starts (section 11 lists the commits). The steps differ
from the order in the stream's brief in one place: the project section (step 2) comes before the sweep (step 3), because
the sweep of the samples runs through the editor's commands on copies of them (`project.setSettings` while playing), as
an agent tunes a game, and needs the section; the generated scenes' sweep does not (it sets `PhysicsSettings` in a helper
process). The characters' recycling goes off in step 2, decided by box3d's own documentation, and the sweep then measures
the state that ships.

### 4.1 Step 1: instruments

**What people and agents see.** `physics.stats` reports per step what box3d and Strata did (awake and sleeping bodies,
islands, contacts touching and recycled, box3d's stage times, Strata's parts) and how the bodies settle (the fastest
body, how many are still settling, the jitter of resting ones); `editor.status` gives the step's time and awake bodies in
its compact answer; the runtime's `--perf-log` gets physics columns; the Streaming panel's Physics section gets a step row.
The perf lab gets settle scenes, a settling measurement and library-only measurements of creation and removal.

**Design.**

1. **`PhysicsStats` fields** (`PhysicsTypes.h:88-150`), of the last fixed step unless stated:
   - box3d: `IslandCount`, `ContactCount` (box3d's contacts, touching or not), `AwakeContactCount` (the awake contacts
     after the step, 2.3), `RecycledContactCount` (summed over the fixed step's box3d steps, each read after its step and
     taken as 0 for a box3d step after which `AwakeContactCount` is 0: box3d's counter is stale then, 2.3),
     `LibraryProfile` (a new `PhysicsLibraryProfile`, ms summed over the box3d steps from `b3World_GetProfile`, in two
     levels as box3d measures them: the stages `Pairs`, `Collide`, `Solve`, `Sensors`, whose sum is the library step; and
     the parts of `Solve`: `Constraints`, `Transforms`, `Refit`, `Continuous` (bullets), `SleepIslands`, plus
     `SplitIslands`, which runs beside the solve and is never added to anything), `LibraryWorkers`
     (`b3World_GetWorkerCount`), `LibraryContactPeak` (`b3World_GetMaxCapacity().contactCount`: the most contacts seen, not
     box3d's storage, 2.3), `ContactRoomGrowths` (steps after which the peak passed the contact room box3d reserved,
     `LibraryRoomContacts`, or its next doubling: when box3d's contact arrays grew) and `LastContactGrowthStepMs` (that
     step's library time).
   - bodies: `SleepingBodyCount` (moving bodies in the simulation, presence bodies included as `ActiveBodyCount` includes
     them, minus `b3World_GetAwakeBodyCount`). The moving bodies in the simulation become a counter kept where bodies enter
     and leave it (`AddToSimulation`, `RemoveFromSimulation`, `InstallBody`, `DestroyBox3DBody`) plus the presence bodies
     the character controllers make and destroy (`CreatePresenceBody` and its destruction, `CharacterControllers.cpp:280-305`,
     which bypass those functions). `GetStats`, which walks every record anyway, asserts in Debug that its count equals the
     counter; no walk is added to `ApplyChanges` (`CheckRecordIndex` is constant time by design, `PhysicsWorld.cpp:921-928`,
     and a walk per round would cost every Debug test of a large world one walk per frame and per fixed step).
   - settling, of the dynamic bodies the step wrote back: `MaxBodySpeed` (m/s), `SettlingBodyCount` (written bodies faster
     than their sleep threshold) and `RestingJitter` (mm: the RMS of the step's displacement of written bodies at or below
     their threshold). `WritePose` (3866-3897) computes each body's displacement from its record's last world position
     before overwriting it; the outcome loop (in body order on the main thread, 4000-4016) reduces them (D1). These speeds
     are displacement over the fixed step, a proxy: box3d's sleep test uses its own sleep velocity (linear speed plus the
     angular arc over the body's extent, and half the position correction, `solver.c:738-751`), so a body counted settling
     can still fall asleep and the docs say so.
   - Strata's parts: `SyncTime` keeps its meaning and gains the parts `ApplyTime` (changes and growth at the step's start)
     and `CheckedPairTime`; `ContactTime` gains the part `EventTime` (`ProcessStepEvents`); cumulative `TotalStepMs` and
     `TotalLibraryMs` (double, since the world was created) so that tools difference two reads.
   - Reading cost: `b3World_GetProfile` after every box3d step (a struct copy). `b3World_GetCounters` after every box3d step
     only if its median at `Boxes70k` is at most 0.5% of that scene's library step (about 43 us), measured both at the busy
     phase's start (up to 70,000 islands) and once the stacks stand (7,000); otherwise only while detailed statistics are
     on (`PhysicsWorld::SetDetailedStats(true)`, reached through `IPhysicsDiagnostics::SetDetailedStats` by code: the perf
     lab, the settle helper, a game run with `--perf-log`), the stats saying `DetailedStats` false and those fields 0
     otherwise. Then `physics.stats {detail: true}` reads `b3World_GetCounters` once at the query (the last box3d step's
     counts, with the stale rule above) and switches nothing on: a query changes no state. The commit states the measured
     cost and which branch it took.
2. **A constant-time scene service, `IPhysicsDiagnostics`** (`Scene/SceneServices.h`, beside `IPhysicsCapacity`):
   `PhysicsStepInfo GetStepInfo() const` with the fields above that need no walk (the step's and library's ms, Strata's
   ms, awake and sleeping bodies, islands, contacts, awake and recycled contacts, maximum speed, settling bodies, resting
   jitter, steps, cumulative ms) and `void SetDetailedStats(bool)` (main thread). `PhysicsSystem` registers it with its
   other services (`PhysicsSystem.cpp:58-67`, and unregisters it in the destructor, 69-78) and fills it from the world after
   each fixed step. So the runtime, the editor's UI and `editor.status` read physics without including the module, and a
   build without physics reports nothing.
3. **Surfaces.**
   - `physics.stats` (`EditorWorldCommands.cpp:321-362`): `step {ms, libraryMs, strataMs, parts {apply, sync, characters,
     checkedPairs, events, writeBack, pairs}, library {pairs, collide, solve, refit, continuous, islands, transforms,
     sensors}, workers, totalMs, totalLibraryMs}`, `sleep {awake, sleeping, islands}`, `contacts {total, awake, recycled,
     recycledShare, tracked, capacity, roomGrowths}`, `settling {maxSpeed, settling, restingJitterMm}`, and from step 2
     `settings`; its description (960-972) and schema (the optional `detail` only if item 1 made counters on-demand)
     follow. `awake` stays where it is (`bodies.active`).
   - `editor.status`: `play.running.physics {bodies, awake, stepMs}` (`DescribeRunningScene`, `CommandUtils.cpp:338`; null
     without a physics world), compact like `running.world`.
   - The runtime's perf log: `PhysicsStepMs`, `PhysicsLibraryMs`, `PhysicsAwakeBodies`, `PhysicsSleepingBodies`,
     `PhysicsContacts`, `PhysicsRecycledContacts`, `PhysicsMaxBodySpeed`, appended after `PhysicsMaxBodies` in
     `GameRuntime::MakePerfLogColumns` (`GameRuntime.cpp:332-363`), from `IPhysicsDiagnostics` (empty cells without it).
   - The Streaming panel: a row under its Physics section (`StreamingPanel.cpp:366-376`) "Step 1.42 ms (box3d 0.91) - awake
     1,204 of 9,876 - recycled 63%", read once per frame through `IPhysicsDiagnostics`, theme colors, probe key
     `Streaming.Physics.Step`; the recycled share only when the counters are read every step (item 1), otherwise left out.
4. **Settle scenes and the settling measurement** (`T/Physics/PhysicsScenes.h/.cpp`, new; tests' code):
   `CreateBallPit(scene, count)` (a walled pit; balls of radius 0.5 m in a lattice with fixed, deterministic offsets, a
   quarter each of the Playground's four materials: 0.5 kg friction 0.6 restitution 0.6 (`Ball.stprefab`), 3 kg 0.3/0.15
   (`BallMetal`), 0.4 kg 0.4/0.45 (`BallPlastic`), 0.6 kg 0.6/0.3 (`BallWood`)), `CreateTower(scene, height)` (1 m crates
   placed touching), `CreatePyramid(scene, base)`, `CreateDominoRun(scene, count)` (the Playground's 0.55 m pitch on a
   serpentine), `CreateSeamProbe(scene)` (boxes sliding and balls rolling at 0.25, 0.5, 1, 2, 4 and 8 m/s across a floor of
   1 m static box tiles at one height, and across a static triangle-mesh floor of a 1 m grid; and the same boxes and balls
   leaving a 1 m high ledge at 0.25, 0.5 and 1 m/s. Recycling only happens while the motion since the cached pose stays
   within the distance, so a contact is recycled at all only below about distance x 60 m/s: 3 m/s at 5 cm, 0.6 m/s at 1 cm,
   0.2 m/s at 0.35 cm; the slow speeds are what exercise the small distances, and the ledge is where a stale manifold shows
   as support past the edge), `CreateCratePush(scene)` (a character
   walking a 1 m crate over tiles for 2 s, the Third-Person Adventure's case; and a crate resting on a walking character's
   capsule), `CreateKinematicPlatform(scene)` (boxes resting on a kinematic platform moving at 2 m/s and turning at 30
   degrees/s) and `CreateCharacterCrowd(scene, count)` (moved from the `Characters1000Step` case, 286-338). Box stacks stay
   in `Perf/SceneGenerators`. `Tests::MeasureSettling(scene, seconds)` steps fixed steps and returns the series' summary:
   seconds until 99% and until all moving bodies sleep (capped), the cumulative step and library ms over the run, the mean
   recycled share, the maximum speed at 5 s, the resting jitter between 5 and 10 s, and the probes' values (the highest
   ghost excursion above the resting height and the largest upward speed of sliding and rolling bodies, how far past the
   ledge each body travels before it has dropped 5 cm, the crate's distance and its slip on the capsule, the platform
   boxes' slip, the tower's top drift). A pit of touching balls is one island and sleeps only as a whole (2.3), so its
   "99% asleep" and "all asleep" are expected to coincide; the summary reports both anyway.
5. **The settle helper**: `--strata-test-helper=physics-settle <scene> <seconds> <result.json> [--recycle-distance m]
   [--sleep-threshold m/s] [--contact-hertz hz] [--contact-damping ratio] [--contact-push-speed m/s] [--workers n]
   [--collision-steps n]` (`T/TestMain.cpp`, beside `world-drive` at 894; `T/Physics/PhysicsSettleProcess.cpp`), which sets
   `PhysicsSettings` through `PhysicsSystem::SetDefaultSettings` (the worker count through the task system's override,
   `PhysicsTaskSystem::SetWorkerLimit`, which this step adds for the helper and tests; step 4 adds the rule) and writes
   `MeasureSettling`'s summary: the engine of the generated scenes' sweep (step 3).
6. **Library-only measurements** (perf lab, provisional, informational): box3d alone, called directly as
   `PhysicsBox3DTests.cpp` does, creating the bodies of `Create10kBodies`, of `Create100kStaticBodies` and of the Grow65k
   frame (into a world made for 65,536, and the same into one made for 131,072), and disabling the 25,000 static bodies of
   the unload frame. With the existing metrics they split creation into box3d's and Strata's shares (step 5) and the
   Grow65k frame into creating and growing (step 4).

**Tests.**
- `Physics.Instruments` (new, `T/Physics/PhysicsInstrumentTests.cpp`): a 10-box stack dropped on a floor: while it settles
  `RecycledContactCount` is above 0 at box3d's default and at most `ContactCount` in every step, and at most
  `AwakeContactCount` in the steps without a move event that fell asleep (2.3: the awake count is taken after the step);
  once asleep the stats read 10 sleeping, 0 awake, 0 awake contacts, 0 recycled (the stale-counter rule: this check fails
  without it, since box3d keeps the last pass's count) and `SettlingBodyCount` 0; a box with `LinearDamping` 0 (the
  component's default 0.05, `PhysicsComponents.h:31`, would leave it about 5% slow after a second) dropped from 5 m reports
  `MaxBodySpeed` within 2% of the speed `b3Body_GetLinearVelocity` gives in the same step, from 0.5 s of fall until it
  lands (the displacement speed lags the velocity by about 0.4 of a step under box3d's sub-stepped integration); the
  library profile's stages (`Pairs`, `Collide`, `Solve`, `Sensors`) sum to at most the library step plus 0.05 ms, and
  `Solve`'s parts other than `SplitIslands` to at most `Solve` plus 0.05 ms; the moving-body counter equals `GetStats`'
  walk after adds, removals, deactivations, rebuilds, a character controller's creation and removal and a cell's unload
  (the Debug assertion in `GetStats` and an explicit case); `GetStepInfo` equals the matching `GetStats` fields between
  steps and is unchanged by a call of `GetStats`.
- `Physics.Simulation`'s "Per-step work grows with the awake bodies" (835-912) gains the new fields' expectations while
  everything sleeps: 0 awake contacts, 0 recycled, 0 settling bodies, every moving body counted as sleeping.
- `Editor.Commands` / the world commands' tests: `physics.stats` reports the new objects while playing and `{running:
  false}` while editing; `editor.status`' `play.running.physics`; `play.state` and `scene.info` carry it too.
- `Runtime.Game` (`GameRuntimeTests.cpp:401-424`, the case that reads `MakePerfLogColumns`: today it requires exactly 8
  names and 8 cells and checks the physics cells, empty without physics under `#if ST_WITH_PHYSICS`): 15 names and cells,
  the seven new ones filled in the headless `GameRuntime` and empty without physics. (`Runtime.FramePerfLog`,
  `FramePerfLogTests.cpp:189`, tests the log writer, not these columns.)
- `Editor.UI` (Streaming panel): the step row's probe while a game runs.
- `TestHelpers`: the `physics-settle` helper on `tower` for 1 s writes a summary with every field.
- `Perf.Physics`: the four library-only cases.

**Docs.** DG gets "## Physics" (after "## Animation", 1753; section 8) with "### Physics instruments" (fields, cost rule,
service, surfaces); DG's `physics.stats` bullet (2615-2622) and the runtime's perf-log line (125); the
`strata-editor-automation` skill's physics rows (130-131).

**Acceptance.** The instruments add at most 1% to `Boxes70k.StrataStepTime` and nothing measurable to `Boxes10k`, judged
by timing the instruments' own code inside `Simulate` (a clock around the reads and reductions, its median over the
busy phase against `StrataStepTime`'s median, reported by the perf case's message): 1% is 64 us, below what 3 A/B pairs
resolve (the five campaign runs' `Boxes70k.StrataStepTime` medians span 6.34-7.26 ms), so the A/B (3 pairs) is reported
as information only. The commit message and DG hold the decomposition 2.4 lacks: Strata's step parts and box3d's stages at 10k, 30k
tracked and 70k; the recycled share at 5 cm in those scenes, the ball pit and the Playground; the Playground's and the
ball pit's time to sleep (or "never within 30 s"); Grow65k's creation and growth shares; the unload's box3d and Strata
shares; `b3World_GetCounters`' cost and the branch taken.

### 4.2 Step 2: the physics settings section, and characters without recycling

**What people and agents see.** Project Settings gains a **Simulation** page under Physics (beside Collision Layers),
drawn by the generic page (DG 955-986) with Contact Recycle Distance and Sleep Threshold; `project.setSettings
{"section": "Physics.Simulation", "value": {...}}` changes them, also while playing (the running world follows at its next
fixed step; Undo after play stops); `physics.stats` reports `settings`. Characters' presence bodies no longer recycle
contacts. Nothing else changes: the section's defaults are box3d's.

**Design.**

1. `PhysicsSettings` (`PhysicsTypes.h:25-71`) gains `ContactRecycleDistance`, `SleepThreshold`, `ContactHertz`,
   `ContactDampingRatio` and `ContactPushSpeed`, with constants `c_DefaultContactRecycleDistance` (0.05),
   `c_DefaultSleepThreshold` (0.05), `c_DefaultContactHertz` (30), `c_DefaultContactDampingRatio` (10) and
   `c_DefaultContactPushSpeed` (3) that the section's field defaults use too, each with a comment naming box3d's value
   it equals and why Strata keeps or changes it (step 3). `SanitizeSettings` (`PhysicsWorld.cpp:670-680`) clamps them
   (distance 0-1 m, threshold 0-1 m/s, hertz 1-240, damping 0-100, push speed 0.01-100 m/s) and replaces non-finite
   values with the defaults.
2. **The section** `Physics.Simulation` (`RegisterPhysicsModule`, `PhysicsRegistration.cpp:10-23`): a simple section,
   title "Simulation", category "Physics", version 1, fields `ContactRecycleDistance` (Float, 0-1) and `SleepThreshold`
   (Float, 0-1), each with a description written for agents: what box3d does with it, what a larger and a smaller value
   change (narrow-phase work and steadier resting contacts against stale manifolds: support past an edge, bumps at seams,
   and a moved or teleported body keeping its old contact for up to the distance; sleeping sooner against slow bodies
   stopping), 0's meaning, that it is a distance in meters (5 cm by default, not a fraction), and that characters never
   recycle. Contact tuning stays in
   `PhysicsSettings` until step 3's rule says otherwise (D3). A build without physics keeps the stored section as one no
   registered module claims and writes it back unchanged (DG 892, 909; no change needed).
3. **Applying it** (D4): `PhysicsSystem::MakeWorldSettings` (504-517) takes the section's values over the defaults;
   `PhysicsSystem::SyncSimulationSettings()` (new, called in `OnFixedUpdate` beside `SyncLayers`, 170-171) re-reads the
   section when `IProjectSettings::GetVersion` changed and hands changed values to `PhysicsWorld::SetSimulationTuning`
   (new): `b3World_SetContactRecycleDistance` and `b3World_SetContactTuning` at once, and the sleep threshold
   (`b3Body_SetSleepThreshold`, `body.c:2134-2142`, any body) to every non-static record that has a box3d body, in the
   simulation or not (a body out of the simulation keeps its threshold when `AddToSimulation` enables it again), and to the
   character controllers' presence bodies (one walk per change, never per step; new bodies get it in `MakeBodySetup`,
   1756, and `CreatePresenceBody`). The constructor applies the settings right after `b3CreateWorld` (4268; contact tuning
   through the definition's fields). `PrepareLoadedScene` (519-541) runs on the main thread (`SceneLoader.cpp:309`) and
   reads the section the same way; a world prepared before a change gets it at its first fixed step like a running one.
4. **Characters**: `CreatePresenceBody` (`CharacterControllers.cpp:292-305`) sets `enableContactRecycling = false`. Their
   contacts with what rests on them, what they push and what lands on them are made afresh each step.
5. `physics.stats` reports `settings {contactRecycleDistance, sleepThreshold, contactHertz, contactDampingRatio,
   contactPushSpeed, source: "project" | "defaults"}`.
6. The feature project: `FeatureTest.stproj` stores `Physics.Simulation` with non-default values that keep the feature
   scripts' checks (DG's coverage rule; the values are chosen with the feature run, e.g. a distance of 0.04 m and a
   threshold of 0.06 m/s, and stated in the commit).

**Tests.**
- `Physics.Settings` (new): the section's registration (defaults equal `PhysicsSettings`' and box3d's, ranges, types,
  descriptions present); a world created while the project stores the section uses its values
  (`b3World_GetContactRecycleDistance`, `b3Body_GetSleepThreshold` of a body), also a prepared world; a running world
  takes a changed value at the next fixed step and not before; without a provider the process defaults apply; an
  exported game's manifest section applies in `GameRuntime`.
- `Editor.ProjectSettings` (or the settings commands' suite): `project.setSettings` on the section while playing changes
  the running world and `physics.stats.settings`; Undo after play stops restores the stored value.
- `Physics.Recycling` (new): a presence body's `b3Body_IsContactRecyclingEnabled` is false (fails before the change; the
  presence body is internal to `CharacterControllerSet`, so the world gains a read-only
  `PhysicsWorld::IsContactRecyclingEnabled(Entity)` that answers for a body record's body or a controller's presence
  body); a crate resting on a walking character stays on top over 2 s; a pushed crate
  travels within 5% of the distance it travelled before the change (measured on the parent commit and stated in the
  commit; no test-only switch).
- `FeatureTest` runners pass with the section stored; `Editor.UI.ProjectSettings` shows the generic page.

**Docs.** DG "### Physics simulation" under "## Project settings" (section, precedence, application points, characters;
section 8); `Docs/Manual/Settings.md`
gains "Simulation (under Physics)" after Collision Layers (31-36); Architecture.md's Physics paragraph (343-433) and its
character paragraph (435-457, presence bodies without recycling).

**Acceptance.** All `Physics*` suites, the samples' regressions and the feature runners pass; the A/B of `Perf.Physics`
shows no change outside noise (the defaults are box3d's), `Characters1000Step` included: its crowd walks on static ground
with no dynamic body, and box3d makes no contact between a kinematic presence body and static or kinematic bodies (2.2),
so the change does nothing there. Its cost shows only where presence bodies touch dynamic bodies (`CratePush`, the
Third-Person crate), measured in step 3's sweep.

### 4.3 Step 3: the contact recycling sweep, and Strata's defaults

**What people and agents see.** A table that says what each recycle distance (and the owner's "0.7", in every reading)
does to step time, sleeping, stability and ghost collisions in the engine's scenes and samples; Strata's defaults chosen
from it by a stated rule; deterministic settling tests that keep them honest.

**Design.**

1. **`Tools/PhysicsSweep.py`** (standard library; output outside the repository, like `Tools/LookDev.py`), two drivers:
   - generated scenes through the `physics-settle` helper (step 1), one fresh process per point under `PerfLock.py`:
     `Boxes10k`, `Boxes30kTracked`, `Boxes70k` (the busy phase as the perf lab measures it, then a 10 s settle),
     `BallPit2k`, `Tower20`, `Pyramid15`, `Dominoes64`, `SeamProbe`, `CratePush`, `KinematicPlatform`, `Characters1000`;
   - the samples through a headless Release editor and `StrataCLI` on copies (`project.openSample`): the Physics Playground
     (AutoDemo off, as its regression does; 20 s in fixed steps of `play.step`, `physics.stats` every 15 steps; the
     script's `TowerTopDrift`, `GhostsOnPitFloor` (the sample's eight bodies tagged "Ghost" resting on the pit floor,
     `Samples/PhysicsPlayground/Scripts/Playground.cpp:157-166`: a station of the sample, not ghost collisions),
     `BallsInPit`, then `DominoesDown` after `StartDominoes`, as in
     `PhysicsPlaygroundCommands.json.in`), the Third-Person Adventure (its regression's 2 s crate push and the robot's
     path), and the Open World (the first 20 s of its tour). The distance and threshold go through `project.setSettings`
     on the copy.
   - The Open World's cells hold 9,449 static rigid bodies and its persistent scene one character controller, with no
     dynamic body (a count of `Samples/OpenWorld/Assets/Worlds` at bc6ee123): static pairs make no contacts, so there is
     nothing to recycle there; its run is the check that nothing changes.
2. **Points.** Distances 0, 0.35 cm, 0.7 cm, 1, 2.5, 3.5, 5 (box3d's), 7.5 and 10 cm, and 70 cm on the probes only. CPU
   measures three times, interleaved by distance, under `PerfLock.py`; deterministic measures once (D6). Then, at the
   chosen distance and one knob at a time on the generated scenes: sleep threshold 0.03, 0.05, 0.08, 0.12 m/s; contact
   hertz 20, 30, 45; damping ratio 5, 10, 20; push speed 0.5, 1, 3 m/s; collision steps 1 and 2 (information only).
3. **Measures per point** (the table's columns): `Boxes70k` library and Strata step medians, box3d's collide stage and the
   recycled share; seconds to 99% and to all asleep, the settle CPU (cumulative step ms over 10 s) and resting jitter of
   `BallPit2k`, `Boxes10k` and `Tower20`; `Tower20`'s top drift; `Dominoes64` all down and when; `SeamProbe`'s highest
   ghost excursion (mm above the resting height) and largest upward speed, for boxes and balls, tiles and mesh;
   `CratePush`'s distance and the head crate's slip; `KinematicPlatform`'s slip; the Playground's seconds until at most 1%
   of its moving bodies are awake, its drift, ghosts, pit and dominoes; the Third-Person crate's distance; the Open World's
   step ms.
4. **The owner's "0.7", read every way it can be**, each answered in the table's summary with its numbers:

   | Reading | Value in box3d's terms | How it is measured |
   | --- | --- | --- |
   | 0.7 cm (7 mm; 1.4 linear slops) | distance 0.007 | point 0.7 cm |
   | 0.7 of box3d's default | distance 0.035 | point 3.5 cm |
   | 0.7 m (the API's unit is meters) | distance 0.7 | point 70 cm, probes only (stale manifolds over 70 cm of relative motion: the expected ghost case) |
   | 0.7 of the linear slop | distance 0.0035 | point 0.35 cm |
   | 0.7 as the angle's stored form (cos^2 of half the angle 0.7: about 66 degrees instead of 10) | constant, not settable (`constants.h:86-88`) | only if the distance sweep shows the angle limits recycling (the recycled share flat across distances in `Dominoes64` and `BallPit2k` while bodies turn): one throwaway local build with the constant changed (D8), reported, never committed; otherwise "not measured: no evidence the angle limits it" |
   | 70% of the narrow phase recycled | an outcome, not a parameter | the recycled share per distance: which distance reaches 0.7 in each scene |

5. **The rule that picks Strata's distance `d*`.** (a) Ghost gate, against distance 0: `SeamProbe`'s excursion at most 2 mm
   more (box3d calls its 5 mm slop "visually insignificant"; 2 mm keeps a bump invisible) for boxes and balls on tiles and
   mesh at every speed; the ledge's travel before the drop at most 1 cm more; `CratePush` within 5%; the head crate's slip within 1 cm more; `Dominoes64` all down; `Tower20` drift at most the
   larger of 10 mm and 1.2 times distance 0's; the samples' regression values within their checks. (b) Cost gate: the
   `Boxes70k` library median within 3% of the best distance's. (c) Among the distances that pass both, the shortest
   `BallPit2k` time to 99% asleep; then the lowest settle CPU; then the nearest to box3d's 5 cm. If 5 cm wins, Strata keeps
   box3d's default and says so with the numbers; if only 0 passes the ghost gate, Strata turns recycling off and says so.
   The sleep threshold's default changes only if a value shortens `BallPit2k`'s and the Playground's time to sleep by at
   least 20% while every probe and the Playground's stations (dominoes, pit) still pass. A contact tuning knob becomes a
   project field only if some swept value changes a scene's time to sleep by at least 10% or its step by at least 5%
   without failing a probe; otherwise it stays a process default and DG says why.
6. **What changes with the rule's outcome**: the constants of step 2 (the section's defaults follow; version 1, nothing
   shipped yet); kinematic bodies' recycling off if `KinematicPlatform` fails the ghost gate at `d*` (in `MakeBodySetup`,
   for `Kinematic`); `RigidBody.ContactRecycling` (bool, default true; inactive for triggers, `InactiveReason`; `Feature.stscene`
   gets a body with it false; Inspector, schema and serialization tests through the component registry; a change while
   playing calls `b3Body_EnableContactRecycling`, which affects only contacts made afterwards, `box3d.h:753-757`, and the
   property's description says so) only if a probe fails at `d*` for a body kind that turning recycling off on that body
   fixes; and the Playground's command script gains
   `physics.stats` checks after its 180 settle steps (`physics.stats {detail: true}` when the counters are read on demand,
   item 1 of step 1: `contacts.recycled` at least 1 when `d*` is above 0, and `sleep.awake` at most the measured value
   times 1.25 after the ghosts settle).

**Tests.**
- `Physics.Recycling`: a new world's `b3World_GetContactRecycleDistance` is `d*` and a project value overrides it;
  distance 0 gives 0 recycled contacts in a resting stack and `d*` (if above 0) more than half of its awake contacts,
  both counted over the steps between landing and falling asleep (a sleeping stack has no awake contacts, and its
  counter is taken as 0, step 1).
- `Physics.Settling` (new, deterministic, CI on every platform; bounds are the measured values at `d*` times 1.25, rounded
  up, stated in the commit with the measurements): `Tower20` drift and time to sleep; `BallPit500` (a quarter of the perf
  pit, to keep Debug runs short) to 99% asleep; `Pyramid15` asleep; `SeamProbe` excursion at most 2 mm above distance 0's
  for each case and the ledge's travel; `Dominoes64` all down; `CratePush` distance; `KinematicPlatform` slip. Each is
  checked to fail with an absurd setting (70 cm or a 1 m/s threshold) once, stated in the commit (the regression-test rule:
  a bound that cannot fail checks nothing). The suite runs in the `Systems` shard, which `CoreShards.json` keeps under
  about 60 s in Debug: the commit states the suite's Debug time and the shard's; if the shard would pass 60 s, the suite
  gets a shard of its own (`Physics.Settling` in its `Suites`, excluded from `Systems`), as the file's rules allow.
- The samples' regressions (`StrataEditor.PhysicsPlayground`, `.ThirdPerson`, `.OpenWorld`) and `Perf.Physics`.

**Docs.** DG "### Contact recycling" (box3d's mechanism of 2.3, the sweep's table in short, the rule and `d*`, the readings
of "0.7", what ghost collisions look like and when to lower the distance), "### Settling and the sweep" (scenes, helper,
tool, how to rerun it); release notes (section 8).

**Acceptance.** The table complete for every point and scene; `d*` and the threshold chosen by the rule; every reading of
"0.7" answered with numbers; no sample check fails; at `d*`, `BallPit2k`'s time to sleep and settle CPU at most box3d's
default's (equal when `d*` is 5 cm). Before this step decides, nothing here promises that a distance other than 5 cm is
better: box3d's comment claims recycling removes jitter; the sweep is what says whether a different distance puts more
bodies to sleep.

### 4.4 Step 4: runtime cost

**What people and agents see.** Busy frames that cost less, characters that cost less, sleeping bodies that cost nothing,
and no growth or unload hitch beyond box3d's own work, with each item's gain in its commit message.

Each item lands only with a measured gain on the metric it targets (an A/B of at least 3% under `PerfLock.py`, or a
deterministic counter that falls), in the order of the gains step 1's decomposition predicts; the order below is the
expected one.

1. **Contacts nobody wants cost nothing.** `CollectCheckedPairs` (3446-3470) returns at once while no pair is tracked
   (`TouchingPairs` empty), and otherwise visits only records with pairs: a per-record pair count replaces the
   `EntityPairs` hash lookup per awake body. The count must follow every change of an entity's list, which is more than
   `AddTouchingPair` and `RemovePairFromIndex` (1213-1257): `EndContactsOf` erases an entity's whole list at once
   (1266) without `RemovePairFromIndex` for that entity, so it zeroes the count there, and every other place that erases
   or clears `EntityPairs` does the same; pairs name entities by UUID, and a pair's entity need not have a body record (a
   character controller's entity), so the count lives where `CollectCheckedPairs` looks (the record of
   `slot.Record->EntityID`) and a Debug check in `GetStats` compares it with `EntityPairs`. `ProcessStepEvents` skips the
   begin loops while nothing can be wanted (`!TrackAll` and no `ContactInterest`) and the end loops while no pair exists
   (`CountEnd` only finds pairs and ends them, so an empty `TouchingPairs` makes the loop a no-op);
   `SeedWantedPairs` already seeds a pair that becomes wanted while touching from box3d's data, so skipping is exact.
   With a watched entity (any script with collision callbacks, 2.2) the begin loops still run; whether a per-shape
   interest flag to spare their two hash lookups per event pays is measured, not assumed.
   `EndContactsOf` (1259) skips records without pairs, which also serves the unload (item 7). A new counter
   `PairLookups` per step falls to 0 in `Boxes70k` (no listener); `Boxes30kTracked` keeps its pairs and events
   (`PhysicsPerfTests.cpp:129-136`).
2. **Poses from box3d's move events.** `ProcessStepEvents` stores each move event's transform beside the moved-body
   bitmap (`MovedTransforms`, by body index, the latest of the fixed step's box3d steps; a body that fell asleep in an
   earlier box3d step of the same fixed step keeps that step's transform), and `WritePose` reads it instead of
   `b3Body_GetTransform` (3847-3853). The moved bodies are exactly those with events, so nothing else needs a read. Nested
   dynamics (4033-4059) read it too. A test-visible counter `LibraryPoseReads` is 0 in a step without nested or failed
   writes.
3. **The write-back's prelude in parallel.** The per-write registry reads (`HierarchyComponent`, `TransformComponent`, the
   parent's world transform; 3958-3974) move into the parallel part for writes from 512 on: the two storages are fetched on
   the main thread first (`registry.storage<T>()` may create storage and is not thread-safe; `get` on an existing storage
   only reads), and nothing writes the registry or the scene during the parallel section. The parent's world transform is
   read on the workers with `Scene::FindCurrentWorldTransform` (read-only, meant for workers, `Scene.h:349-352`, as
   `SyncEntitiesToBodies`' parallel skip does, 3206), never `Scene::GetWorldTransform`, which writes the scene's mutable
   `m_PathScratch` when anything is dirty (`Scene.cpp:1673-1705`, `Scene.h:747`) and would race; a write whose parent's
   cached transform is not current (null) and every non-invertible parent (`SuspendUnwritableBody` writes the record and
   the world) go to the serial path, in body order. `isWrittenAbove` (ancestors' `WriteBackPass`, `FindRecord`) only reads
   once the marking loop before it has finished. Bodies whose parent was written in the step (nested) are still found and
   deferred serially, in body order (D1).
4. **box3d's workers.** The sweep (helper `--workers`) of 4, 8, 12, 16 and 24 on `Boxes10k`, `Boxes30kTracked`, `Boxes70k`
   and `Characters1000` (library ms and frame ms, three interleaved repeats) chooses a rule for
   `PhysicsTaskSystem::GetWorkerCount` (61-74) expressed in the JobSystem's worker count (for example "the JobSystem's
   workers plus one, at most 16"), not in the hardware's threads or the reference machine's numbers, so that tests running
   a `ScopedJobSystem(N)` see the same count on every machine; the rule never exceeds today's count and stays at least 1,
   and it keeps 5 for 4 job workers, which the golden-hash case checks (`PhysicsBox3DTests.cpp:115-117`: that expectation
   stays; a rule that would change it is rejected, not the test edited). The helper's override and
   `PhysicsTaskSystem::SetWorkerLimit` (step 1) are the only other inputs. box3d gives the same results for any worker
   count (`Physics.Box3D` golden hash; `Physics.Simulation` identical runs). A smaller count only splits box3d's stages
   into fewer tasks; any JobSystem thread, performance or efficiency core, may still run them (the JobSystem does not
   place jobs by core type), which the sweep's numbers include.
5. **Characters.** Measure the main-thread prelude (`CharacterControllers.cpp:924-946`: a `GetWorldTransform` per
   controller, another and the parent's inverse per parented one), the presence-body teleports and `StepCharacters`' loop over moved controllers (`FindRecord`,
   `UpdateDescendantsOfMovedBody`, 3793-3802); cut what step 1 shows costs (candidates: skip the parent reads for
   controllers without a parent; skip the descendants walk for controllers without children, read from
   `HierarchyComponent`). Target `Characters1000Step` at most 0.70 ms.
6. **Growth while playing.** From step 1's library-only Grow65k measurements: creation's share is recorded (step 5);
   growth's share (the same creation into a world reserved for 131,072 against one reserved for 65,536, a new provisional
   metric `Perf.Physics.Grow65k.GrowthMs`, target 4 ms; a difference of two medians of about 100 ms each, so the case
   reports the two medians and their spread with it, and a growth share smaller than that spread is reported as "within
   noise", not as a value) is explained: box3d's array doubling, which only a larger room at
   creation avoids (the remembered demand and `IPhysicsCapacity::Reserve` already give the scene's next world and a world's
   streamed cells that room). If step 1's contact counters show box3d's contact storage growing while the perf scenes or
   samples play, the automatic contact room for moving bodies goes from 2 per body (`c_ContactsPerBody`, 90) to the
   measured need (at most 4), kept within `Perf.Physics.*.ReservedMemory`'s budgets (151.2 and 250 MB).
7. **The unload frame's Strata share.** `Perf.World.CellUnloadMaxFrame`'s request frame (7.92 ms) against
   `Perf.World.CellUnload.LibraryOnly` (box3d disabling the same 25,000 bodies): Strata's per-body work of
   `RefreshActivity`/`RemoveFromSimulation` (1363-1376: `SaveVelocities` already skips statics; `EndContactsOf`'s lookup
   goes with item 1) and the change rounds' bookkeeping come down to at most 1.25 times box3d's own time; the scene's own
   activity work in that frame is the Scene module's (measured, named, not changed here). The metric is the longest frame
   of the whole unload (2.4), so once the request's frame falls below the destroying frames, those decide it; the commit
   reports both.
8. **Sleeping bodies cost nothing.** A new perf case, 70,000 sleeping boxes (stacks that settled) and 1,000 awake ones in
   another part of the world, against the 1,000 alone: `Perf.Physics.Sleeping70k.StepRatio`, target 1.2. What it shows
   (box3d's dynamic tree holding sleeping proxies, Strata's per-step lists) is fixed in Strata or recorded as box3d's.

**Tests.** Counters in `Physics.Simulation`'s awake-work case: `PairLookups` 0 with no listener, `LibraryPoseReads` 0
without nested writes; `Physics.Events` and `Physics.Box3D` event suites unchanged (begin, end, seeding, triggers, rebuilds);
watches begun while bodies touch and while a body is inside a trigger (seeded at the next step, then ended when they part
or the listener goes: `PhysicsBox3DBehaviorTests.cpp:224`, 345), the listener removals of `Physics.Events`
(`PhysicsEventTests.cpp:215`) and a deactivated or destroyed touching entity (104, 137, which go through `EndContactsOf`
and so check the pair count's upkeep), all rerun under the early-outs; nested dynamics, mirrored and scaled bodies
(`Physics.Simulation` 417-555) under the move-event poses; `Physics.Character` with every thread count (573) after item 5;
the worker rule's bounds; identical runs with 1 and N workers.

**Docs.** Architecture.md's Physics paragraph (write-back from move events, workers, pair early-outs, unload); DG
"### Physics instruments" (what the parts mean after the changes); the roadmap's open item "dynamic bodies below other
dynamic bodies are still written back one by one" (`Docs/Roadmap.md:614`) stays true and stays.

**Acceptance (targets; the integration campaign lowers budgets when they are met, section 7).** `Boxes70k.StrataStepTime`
at most 4.5 ms (6.378) and its ratio at most 0.55; `Boxes10k` at most 0.40 ms (0.557), ratio 0.45; `Boxes30kTracked` at
most 2.3 ms (2.998), ratio 0.75; `Boxes70k.ActiveFrame` at most 13.5 ms (16.05); `Characters1000Step` at most 0.70 ms
(0.864); `Sleeping70k.StepRatio` at most 1.2; `Grow65k.GrowthMs` at most 4 ms or its cause stated with the numbers; the
unload's Strata share at most 1.25 times box3d's. The targets are estimates from 2.4's per-box costs (removing about a
third of Strata's 91 ns per box at 70,000). Strata's part alone takes the 70,000-box frame to about 14.2 ms (16.05 - 1.88):
the 13.5 ms frame also needs about 0.7 ms off box3d's own step (item 4's worker rule or step 3's distance), so it is met
only if those deliver. Where step 1's decomposition shows a lever cannot reach a target, the commit says so with its
numbers and the target moves to the roadmap's open items.

### 4.5 Step 5: recording creation and loading

**What changes.** Documents only, plus the budget decision of Q1 (section 10).

- The roadmap's open items: the "Physics on box3d" item (`Docs/Roadmap.md:617-631` at bc6ee123, as step 0 leaves it)
  states, with step 1's library-only numbers: `Create10kBodies` (total, box3d's own, Strata's), `Create100kStaticBodies`,
  the Grow65k frame (creation and growth), the unload frame (box3d's and Strata's after step 4), and the drive frames'
  physics share; the owner's ruling of 2026-10-27 quoted; "not optimized in wave 7; the way down is bulk insertion in box3d
  (an upstream change and a later bump) and fewer allocations per body in Strata". Step 0's blocker entry for
  `Create10kBodies` (`Roadmap.md:858-860` at step 0's tip) moves into that item as recorded, per `decisions.md`.
- DG "Perf tests" (477-482): the library-only metrics and what they are for.
- `Budgets.json`: as Q1 decides.

**Acceptance.** Every creation and loading metric of 2.4 has its box3d and Strata shares in the roadmap; no wave-7 code
changes them.

### 4.6 Cut line, and what lies below it

Steps 1-5 are the stream; in step 4 items 1-4, 6 and 8 are above the cut, items 5 and 7 too unless step 1 shows a gain
under 3% for them (then they move below). Below the cut, in this order, each only when everything above has landed; what is
not done moves to the roadmap's "Later" when the wave lands:

9. **The scene's world matrices from physics.** The write-back computes each written body's world matrix
   (`LastWorldTransform`, 3894) and `Scene.Transforms` computes it again (about 1 ms at 70,000, 2.4); handing the scene
   those matrices needs a Scene API that keeps its cache contract (the Scene module's owner decides).
10. **Rolling resistance** (`RigidBody.RollingResistance`, spheres and capsules, `b3SurfaceMaterial::rollingResistance`):
    if step 3 shows the ball pit stays awake whatever the distance and threshold, this is the lever; a property with
    default 0 and the Playground's balls set through commands.
11. **Deferred removal of an unloading cell's static bodies**: masks that collide with nothing at the request (no tree
    change), queries skipping records out of the simulation, and the box3d removals in budgeted slices afterwards; the
    risks are sensors, queries and contacts of statics that box3d still holds (D5 forbids a body that collides while its
    entity is inactive, so the masks must change in the request's update).
12. **A per-scene override** of `Physics.Simulation` (a puzzle scene and a large world in one project), once a project
    needs it.
13. **Upstream requests** (D8): a settable recycle angle, a run-time reserve, bulk insertion and removal.

## 5. Settings and defaults

| Setting | 0.6.0 | After the stream | Step |
| --- | --- | --- | --- |
| Contact recycle distance | box3d's 5 cm, not settable | `Physics.Simulation.ContactRecycleDistance`, default `d*` (the sweep's) | 2, 3 |
| Sleep threshold | box3d's 0.05 m/s, not settable | `Physics.Simulation.SleepThreshold`, default 0.05 unless the rule moves it | 2, 3 |
| Contact hertz, damping ratio, push speed | box3d's 30, 10, 3 m/s | process defaults; project fields only by the rule | 2, 3 |
| Characters' presence bodies | recycle | never recycle | 2 |
| Kinematic bodies | recycle | recycle, unless the platform probe fails | 3 |
| box3d's workers | JobSystem workers + 1 (24 here) | step 4's rule | 4 |

Defaults that change (release notes, section 8): characters' presence bodies stop recycling; the recycle distance if `d*`
is not 5 cm; the sleep threshold and kinematic bodies' recycling if the rule moves them; box3d's worker count. Each changes
every scene's simulation the same way on every platform (D1); stored scenes need no migration (the section is new).

## 6. Tests: suites and must-fail checks

| Suite (file) | Step | Cases | Fails without the change |
| --- | --- | --- | --- |
| `Physics.Instruments` (new) | 1 | stack settles: recycled, sleeping, awake contacts, 0 recycled once asleep; free fall speed; profile stages and solve's parts; moving counter; `GetStepInfo` | the fields and counter; the stale-counter rule |
| `Physics.Simulation` (existing) | 1, 4 | awake-only work with the new counters; identical runs; nested, mirrored, scaled write-backs | `PairLookups`, `LibraryPoseReads` |
| `Editor.Commands` / world commands, `Editor.UI` | 1, 2 | `physics.stats` objects and `settings`; `editor.status` `play.running.physics`; the panel row | the result keys |
| `Runtime.Game` (`GameRuntimeTests.cpp:401-424`) | 1 | the perf log's 15 columns, the seven new ones filled, empty without physics | the column count (8 today) |
| `TestHelpers` | 1 | `physics-settle` writes a complete summary | the helper |
| `Physics.Settings` (new), `Editor.ProjectSettings` | 2 | registration, creation, change while running, no provider, exported game, undo | the section |
| `Physics.Recycling` (new) | 2, 3 | presence bodies off; head crate and push bounds; default `d*`; distance 0 gives 0 recycled | presence bodies (fails on bc6ee123) |
| `Physics.Settling` (new) | 3 | tower, pit, pyramid, seams and ledge, dominoes, crate, platform bounds | each bound fails once with an absurd setting |
| `Physics.Character`, `Physics.Events`, `Physics.Box3D` (existing) | 2, 4 | all kept; threads; seeding | |
| `FeatureTest`, `StrataEditor.FeatureTest`, `StrataRuntime.FeatureTest` | 2, 3 | the section stored; the property if added | coverage of the section |
| `StrataEditor.PhysicsPlayground`, `.ThirdPerson`, `.OpenWorld` | 2-4 | regressions; the Playground's new `physics.stats` checks | |
| `Perf.Physics`, `Perf.World` | 1, 4 | library-only cases, `Sleeping70k`, `Grow65k.GrowthMs`, settle CPU | |

A regression test is checked to fail without its fix (AGENTS.md, Testing): each "fails without" above is run once against
its step's parent commit and the result stated in the commit message. New suites keep the `Physics.` prefix and so the
`Systems` shard (`StrataTests/CoreShards.json:18`), unless `Physics.Settling` would take that shard past the file's
60 s in Debug (4.3: then a shard of its own).

## 7. Perf: metrics, targets and the stream's A/Bs

**New provisional entries** (`StrataTests/Perf/Budgets.json`, appended; measured by `T/Perf/PhysicsPerfTests.cpp` and
`WorldPerfTests.cpp`):

```json
"Perf.Physics.BallPit2k.SettleCpuMs": { "Unit": "ms", "Provisional": true },
"Perf.Physics.Boxes10k.SettleCpuMs": { "Unit": "ms", "Provisional": true },
"Perf.Physics.Boxes70k.CollideMs": { "Unit": "ms", "Provisional": true },
"Perf.Physics.Sleeping70k.StepRatio": { "Unit": "ratio", "Provisional": true, "Target": 1.2 },
"Perf.Physics.Grow65k.GrowthMs": { "Unit": "ms", "Provisional": true, "Target": 4 },
"Perf.Physics.Create10kBodies.LibraryOnly": { "Unit": "ms", "Provisional": true },
"Perf.Physics.Create100kStaticBodies.LibraryOnly": { "Unit": "ms", "Provisional": true },
"Perf.Physics.Grow65k.LibraryOnly": { "Unit": "ms", "Provisional": true },
"Perf.World.CellUnload.LibraryOnly": { "Unit": "ms", "Provisional": true },
```

`BallPit2k.SettleCpuMs` gets its `Target` in step 3, once its value at box3d's 5 cm is measured: 0.8 times that value if
the measurement at `d*` is at or below it, and no target otherwise (a target the chosen default cannot reach is not a
goal this stream sets). `SettleCpuMs` is the sum of the fixed steps' wall time over the first 10 simulated
seconds (lower is better; it rewards sleeping sooner as well as cheaper steps); `CollideMs` is box3d's collide stage
median in the busy phase (what recycling saves); `Sleeping70k.StepRatio` the median step with 70,000 sleeping and 1,000
awake boxes over the median with the 1,000 alone (a ratio of medians, D6's CPU kind).

**Targets added to existing entries** (each below its budget, as the parser requires): `Boxes10k.StrataStepTime` 0.40,
`Boxes30kTracked.StrataStepTime` 2.3, `Boxes70k.StrataStepTime` 4.5 ms; the three `StrataToLibraryStep` 0.45, 0.75 and
0.55; `Boxes70k.ActiveFrame` 13.5 ms; `Characters1000Step` 0.70 ms. A ratio can rise when box3d's step gets cheaper (step 3
and item 4 of step 4 may do that); the absolute `StrataStepTime` is what judges Strata's glue, the ratio is reported with
it.

**Budgets only go down, and not in the stream**: the stream adds provisional entries and targets and changes no existing
`Measured` or `Budget`. A/Bs of 3 pairs are information (below), `plan.md` 4.1 keeps `PerfStability.py` campaigns out of
the streams, and AGENTS.md asks for `PerfStability.py --runs 5` on the reference machine before budgets change; so the
integration campaign (`plan.md` 7.2) lowers the entries this stream made cheaper (`--propose-budgets` classes them
"cheaper") and measures every provisional entry. Each step's commit states the medians that would justify it.

**A/Bs** (information, never budgets): a Release build of the stream against step 0's tip's Release build, interleaved,
3 pairs. The base is a separate worktree of step 0's tip (`git worktree add .worktrees/physics-runtime-base <tip>`, then
`git submodule update --init --recursive --depth 1` in it) built in `build/agent-physics-runtime-base`, both removed when
the stream lands: `git archive` would leave out the submodules (`Strata/vendor/*`), so the base would not build. Each run
is `python Tools/PerfLock.py --label physics-runtime -- StrataTests.exe --test-suite=Perf.Physics,Perf.World`, after
deleting that build's `PerfResults/Release.json` (CTest's `StrataTests.PerfResults.Clean` does it only for ctest runs,
`StrataTests/CMakeLists.txt:268-269`; a direct run appends) and keeping a copy of it after the run; the copies are
compared with `Tools/PerfStability.py --results <stream's> --baseline <base's>` (report mode only: it runs no tests, so
`plan.md` 4.1's rule holds). `PerfLock.py` keeps them off other agents' measurements, and the stream measures only when no
build of its own runs (four other streams build on the machine). Each step's commit states its metrics' medians against
the base. The sweep (step 3) and the worker sweep (step 4) run under the same lock, one point per process.

## 8. Docs, skills and release notes

- **DG**: a new "## Physics" after "## Animation" (1753), before "## World partition" (1783), and its line in the guide's
  table of contents: "### Contact recycling" (step 3), "### Physics instruments" (steps 1, 4), "### Settling and the sweep"
  (steps 1, 3); the section itself as "### Physics simulation" under "## Project settings", after "### Collision layers"
  (988-1036), where the project's other sections are described (Collision layers, Input actions 1037), with a link from
  "## Physics" (step 2); "Perf tests" (477-482: the new cases); `physics.stats` (2615-2622); the perf log line (125).
- **Architecture.md**: the Physics paragraph (343-433: the section and its application, the instruments' service, the
  write-back from move events, the worker rule, the pair early-outs) and the characters' paragraph (435-457: presence
  bodies never recycle).
- **AGENTS.md** (the room step 0 freed; about 260 code points), one bullet in "Architecture rules" after "Collision
  layers": "**Physics tuning** is the project's `Physics.Simulation` section, applied by `PhysicsSystem` on the main
  thread; character presence bodies never recycle contacts, and what physics reports every frame stays constant time
  (`IPhysicsDiagnostics`) ([Physics](Docs/DeveloperGuide.md#physics))." The guide states it in full. If the four rendering
  streams have used the room by the time this lands, the bullet is shortened to its first clause and the rest stays in
  the guide.
- **Manual**: `Docs/Manual/Settings.md` "Simulation (under Physics)": what the recycle distance and the sleep threshold
  change, in a player-facing developer's words, and when to lower the distance (bodies bumping over seams).
- **Skills**: `strata-editor-automation` (the `physics.stats` rows, 130-131: sleep, contacts, settling, settings; tuning a
  running game with `project.setSettings` on `Physics.Simulation`); `strata-make-a-game` (its Physics Playground line,
  452: piles that never sleep: read `physics.stats.sleep` and `settling`, then the section).
- **Roadmap**: the wave 7 entry gains the stream's line; the open items as step 5 says; "Later" gets 4.6's items not done.
- **`plan.md`** (the orchestrator's, at integration): a fifth row in section 4's table.
- **Release-note lines** (0.7.0), filled with the measured numbers:
  - "Physics steps cost less: Strata's part of a 70,000-box step from 6.4 to N ms, 1,000 characters from 0.86 to N ms,
    and a world's sleeping bodies cost nothing measurable."
  - "Contact recycling, box3d's reuse of resting contacts, is a project setting (Project Settings > Physics > Simulation:
    Contact Recycle Distance, default N cm, chosen by measurement; Sleep Threshold), changeable while the game plays."
  - "Character controllers' bodies no longer recycle contacts, as box3d advises, so what rests on or is pushed by a
    character meets it without ghost contacts."
  - "`physics.stats` and the runtime's perf log report sleeping bodies, islands, recycled contacts, box3d's stage times and
    how piles settle."
  - "Creating many bodies at once still costs what box3d's one-at-a-time insertion costs (N ms for 10,000 bodies, N of them
    box3d's); recorded, not changed in this release."

## 9. What the stream shares and how merges resolve

`plan.md`'s merge order covers the four rendering streams; this stream lands when its steps above the cut are done, at any
point in that order, after merging main like every stream. Its shared files and their resolutions:

| Shared | This stream's change | Resolution |
| --- | --- | --- |
| `Strata/src/Strata/Runtime/GameRuntime.cpp` (`graphics-options` owns it) | seven names and writes appended to the physics part of `MakePerfLogColumns` (332-363) | a separate hunk from `graphics-options`' `IGameGraphics` provider; keep both |
| `StrataTests/FeatureTest/FeatureTest.stproj` | the `Physics.Simulation` section | `Renderer.Quality` rows of step 0, `graphics-options` and `sun-shadows` are other keys of `Settings`: keep all, rerun `StrataTests.FeatureTest` |
| `Feature.stscene` | a `RigidBody` with `ContactRecycling` false, only if step 3 adds the property | additive |
| `StrataTests/Perf/Budgets.json` | entries appended; targets on eight physics entries | other streams append renderer entries; physics lines are only this stream's |
| `StrataTests/Editor/PhysicsPlaygroundCommands.json.in` | `physics.stats` checks after the settle steps (step 3) | `camera-and-tone` and `light-and-materials` add `component.get` lines at the top (`camera-and-tone.md:140`; `light-and-materials.md:1292`): one command per line, keep all |
| DG | the new "## Physics" section; the `physics.stats`, perf-log and "Perf tests" lines | the rendering streams edit "## Rendering" and "View commands": no overlap |
| AGENTS.md | one bullet in "Architecture rules" | the rendering streams' bullets go to "Rendering"; the room is the only contention (section 10, R9) |
| `Docs/Roadmap.md` | the wave 7 entry's line; the physics open items | additive; the open items' physics item is only this stream's |
| `Docs/Manual/Settings.md` | a "Simulation (under Physics)" bullet | `graphics-options` adds the Graphics Quality page's: keep both, in the page order the panel shows |
| `.claude/skills/strata-editor-automation/SKILL.md` | the physics rows | the rendering streams add debug views and capture parameters elsewhere in it |

No new layer, include exception or `MayInclude` entry: the section and the service are Physics' and Scene's, the editor
reads physics through `IPhysicsDiagnostics` and the commands it already includes, and the runtime through the scene
service. Samples' scenes and pictures are not changed by this stream (unless step 3 adds the per-body property or rolling
resistance and a sample needs it, through editor commands).

## 10. Risks and open questions, with the decisions recommended

1. **Q1: `Create10kBodies` fails its budget in every run, and the owner ruled creation out of scope.** Step 0 named it a
   0.7.0 blocker (`Roadmap.md:858-860` at step 0's tip) before the owner's words; "budgets only go down" forbids raising it,
   and `-L perf` stays red while it fails. *Recommended decision (the orchestrator's to take): treat the owner's ruling as
   the reason step 0's procedure asks for, re-baseline the entry once with `PerfStability.py --propose-budgets --rebaseline
   Perf.Physics.Create10kBodies="box3d port; owner 2026-10-27: creation is not a release blocker"` at its box3d median, keep
   Jolt's 7.995 ms as its `Target` (what bulk insertion would reach), and record it in the open items with step 1's
   library-only split.* If the orchestrator declines, the entry stays failing and the open items say why; this stream does
   not optimize creation either way. *Review note:* the recommendation conflicts with `decisions.md`' step 0 rule as
   written ("only a machine change shown that way raises a budget"; "a library change the owner asked for ... is not a
   machine change: physics metrics slower since that port are regressions to explain and, where Strata's own glue is the
   cause, to fix") and with AGENTS.md's "budgets only go down"; the owner's ruling of 2026-10-27 is a reason to stop
   optimizing creation, not by its words a permission to raise a budget. So the stream changes the entry only after the
   orchestrator records that exception in `decisions.md` (or the owner grants it); until then it leaves the entry as step
   0 left it, failing, and step 5's open item gives the measured box3d and Strata shares. Note that step 0's own
   measurement puts box3d's part of the 10,000 bodies at 11.4 of 18.1 ms on this machine (`Roadmap.md:860-862` at step 0's
   tip), so `decisions.md`' "where Strata's own glue is the cause, to fix" applies to at most the rest.
2. **Q2: what the owner meant by "0.7".** *Decision: measure every reading (4.3, item 4) and report each with numbers; the
   default follows the rule, not a reading. If the owner then names a reading, the section sets it for his projects in one
   command, and the release notes say what it does.*
3. **R1: a different default changes every scene's simulation.** The samples' regressions (tower drift at most 0.25 m,
   dominoes at least 60 down, 600 balls in the pit, the sample's 8 "Ghost" bodies resting on the pit floor, the crate's
   2 s push) and the feature run decide; `Physics.Settling` bounds the new behavior; results stay identical across thread
   counts (D1) and within the bounds' margin across platforms (D6).
4. **R2: ghost collisions at larger distances.** Recycled contacts keep stale anchors and normals while a body slides,
   rolls or is teleported up to the distance, begin or end no touch meanwhile, and still bounce (restitution comes from
   velocities sampled every step, 2.3). *Mitigation: the ghost gate (seams at slow and fast speeds, the ledge) decides the
   default; kinematic bodies and a per-body switch follow the probes; the Manual says when to lower the distance.*
5. **R3: the claim that recycling puts more bodies to sleep may not hold.** box3d's comment says recycling eliminates
   jitter; whether a different distance changes time to sleep is unmeasured, and a pit of balls with no rolling resistance
   may stay awake for reasons recycling does not touch. *Decision: the stream reports what it measures; the sleep threshold
   and rolling resistance (below the cut) are the next levers, measured in the same table.*
6. **R4: the worker rule is tuned on one hybrid machine.** The Mac (12 threads, M2 Max) and CI runners (2-4 threads) differ.
   *Mitigation: a rule in the JobSystem's worker count (so tests see the same on every machine), never above today's
   count; the Mac's run of `Perf.Physics` at integration (`plan.md` 7.4) is unverified until then.*
7. **R5: the write-back reads poses from events.** Bodies whose events come from an earlier box3d step of the fixed step,
   bodies that fell asleep, teleports between steps and nested dynamics must read the right pose. *Mitigation: the
   existing write-back suites under both settings of `CollisionSteps`, plus the counter.*
8. **R6: registry and scene reads on job threads.** *Mitigation: storages fetched on the main thread first; parents' world
   transforms only through `Scene::FindCurrentWorldTransform` (never `GetWorldTransform`, which writes `m_PathScratch`),
   with the writes it cannot answer done serially; no registry or scene write during the parallel section (asserted in
   Debug by a write guard around it, if the scene offers one; otherwise documented and reviewed); the serial path below
   512 writes is unchanged.*
9. **R7: instruments that cost.** `b3World_GetCounters`' walk at large island counts. *Decision: measured, then per-step or
   on demand (4.1); the 1% rule (D2).*
10. **R8: machine load.** Five streams build and test on one machine. *Mitigation: perf only under `PerfLock.py`, measured
    when the stream's own build is idle; the sweep's deterministic measures need no quiet machine.*
11. **R9: AGENTS.md room.** About 260 code points after four rendering bullets of the 2,787 step 0 freed. *Decision: the
    bullet shortens to its first clause if needed (section 8).*
12. **R10: `GameRuntime.cpp` belongs to `graphics-options`.** *Decision: one appended hunk in the perf-log group; if
    `graphics-options` restructures that function, this stream re-appends after merging main.*
13. **Q3: kinematic bodies.** A moving platform's riders could see stale contacts. *Decision: on, unless the platform probe
    fails the ghost gate at `d*`.*
14. **Q4: a per-body property.** *Decision: only with a probe that it fixes (4.3, item 6); otherwise not added: a property
    nobody needs costs the feature test, the Inspector and the schema for nothing.*
15. **Q5: `Grow65kMaxFrame`'s 16.7 ms target.** About four fifths of the frame is creation (2.4). *Decision: keep the
    target as the goal bulk insertion would serve, record the creation share as the owner's ruling, and judge this stream
    on `Grow65k.GrowthMs`.*
16. **Q6: the unload frame's target of 4 ms.** box3d's own removal may be most of it on this CPU. *Decision: this stream
    owns Strata's share (at most 1.25 times box3d's); deferred removal is below the cut; the target stays an open item if
    box3d alone exceeds it.*

## 11. Deliver in this order

1. **Step 1** (commits): `PhysicsStats` fields and the moving-body counter with `Physics.Instruments`; the counters' cost
   measured and the branch chosen; `IPhysicsDiagnostics`; `physics.stats`, `editor.status`, the perf log, the Streaming
   row; `PhysicsScenes` and `MeasureSettling`; the `physics-settle` helper; the library-only perf cases; DG "Physics
   instruments", the skill. The decomposition tables in the last commit's message.
2. **Step 2**: `PhysicsSettings` fields and sanitizing; the `Physics.Simulation` section; `SyncSimulationSettings` and
   `SetSimulationTuning`; presence bodies without recycling with `Physics.Recycling`'s first cases; `physics.stats.settings`;
   the feature project; DG, Manual, Architecture.md. Push `ci/wave7-physics-runtime` once for the compilers.
3. **Step 3**: `Tools/PhysicsSweep.py`; the sweep (generated scenes, then samples); the table and the readings of "0.7" in
   the commit message and DG; the defaults by the rule (and kinematic, property, contact tuning fields as the rule says);
   `Physics.Settling`; the Playground's checks; release-note lines.
4. **Step 4**: items 1-8 in the order step 1's numbers give, each with its A/B and counters; the medians stated for the
   integration campaign, which lowers the budgets (section 7).
5. **Step 5**: the roadmap's record; Q1 as decided.
6. **Finish**: merge main; an independent review; Debug ctest with every suite (GPU suites under validation included),
   Release ctest without perf, `CompatLint.py`; CI green on `ci/wave7-physics-runtime`; the A/B table for the landing
   message; land (`plan.md` 7.1).

If time runs short, stop after a complete step; every step leaves the branch mergeable. Steps 1-3 answer the owner's
request on their own; step 4 is the larger runtime gain and its items land one by one.

## 12. The first hour

1. Read `decisions.md`' last section, `plan.md` 4.1 and 7, this spec, AGENTS.md, Architecture.md's Physics and Character
   paragraphs (343-457), DG "Perf tests" (414-500) and the code of 2.1 (`Simulate`, `SyncEntitiesToBodies`,
   `ProcessStepEvents`, `WriteBackDynamicBodies`, `CharacterControllerSet::Step`, `PhysicsTaskSystem`), and box3d's
   `physics_world.c:590-760` and `types.h` world and body definitions.
2. Confirm step 0 is on main: `Budgets.json`'s physics entries (2.5), `PerfStability.py --baseline`, the roadmap's open
   items, AGENTS.md's free room; note any name that differs from 2.5.
3. Create `.worktrees/physics-runtime` from step 0's tip, initialize submodules, configure `build/agent-physics-runtime`,
   build Debug and Release; add the base worktree of step 0's tip with its submodules (section 7: not `git archive`, which
   drops them) and build its Release in `build/agent-physics-runtime-base`.
4. Run `StrataTests.exe --test-suite=Physics*` in Debug as the stream's baseline, then, under `PerfLock.py` and alone,
   `--test-suite=Perf.Physics,Perf.World` on both Release builds twice (each run after deleting its `PerfResults/Release.json`
   and keeping a copy, section 7), to see the noise of the A/B before changing anything.
5. Start step 1 with its tests: `PhysicsLibraryProfile` and the counters read after `b3World_Step` (4563), timing
   `b3World_GetCounters` at `Boxes70k` first, since its cost decides how the rest of the instruments read box3d, and the
   stale-counter case of `Physics.Instruments` (a stack that sleeps must read 0 recycled) before the rule that makes it
   pass.

## Review corrections

The independent review (2026-10-27) checked this spec against bc6ee123, box3d at efca0c1 (`src/physics_world.c`,
`contact.c`, `contact_solver.c`, `contact_solver_wide.inl`, `solver.c`, `body.c`, `island.c`, `broad_phase.c`,
`types.c`, `include/box3d/*.h`), step 0's tip (`agent/step0`, 42cb13bd: `Budgets.json`, `Roadmap.md`,
`PerfStability.py`), `plan.md`, `decisions.md` and the campaign's summary and run logs. The campaign's numbers in 2.4, the
budgets of 2.5, box3d's constants and defaults and nearly every file:line of section 2 held. Changed, and why:

1. **Major: restitution claim (2.3, R2).** The spec said a recycled point's `normalVelocity = 0` means "no restitution".
   Restitution is armed from the relative velocity the contact prepare samples from the bodies every step
   (`contact_solver.c:143`, 215-221, 755; `contact_solver_wide.inl:843-860`, 1434-1438); `normalVelocity` only feeds hit
   events. Recycled contacts still bounce; what they keep stale is the anchors and the normal. 2.3 also now says that a
   recycled contact changes no touching state (the `continue` at 734 precedes 765-776: no begin or end event, no island
   link change) and that teleports (`b3Body_SetTransform`, `body.c:1142-1192`) keep the recycle cache.
2. **Major: the recycled counter is stale while everything sleeps (2.3, 4.1, tests).** `b3Collide` returns before
   resetting the workers' `recycledContactCount` when no contact is awake (`physics_world.c:844` against 889), so
   `b3World_GetCounters` keeps reporting the last pass's count. `Physics.Simulation`'s new "0 recycled while everything
   sleeps" would have failed and the sweep's sums would overcount. Strata now takes a box3d step's recycled count as 0
   when its awake contact count after the step is 0, and `Physics.Instruments` checks it (must-fail).
3. **Major: `awakeContactCount` is post-step (2.3, 4.1 test).** It is computed at the `b3World_GetCounters` call, not by
   the collide pass, so "recycled at most awake contacts" fails in a step where an island falls asleep. The test now
   checks recycled against `ContactCount` every step and against awake contacts only in steps without a fell-asleep event.
4. **Major: box3d's profile is nested (2.3, 4.1, test).** `refit`, `bullets`, `sleepIslands`, `transforms` and
   `constraints` are measured inside `solve` (`solver.c:1888-2333`) and `splitIslands` in a concurrent task
   (`island.c:658-671`); the spec's flat list summed them with `Solve`, so "parts sum to at most 1.05 times the library
   step" could not hold. `PhysicsLibraryProfile` now has two levels and the test checks each level.
5. **Major: data race in step 4 item 3.** Moving "the parent's world transform" to job threads would call
   `Scene::GetWorldTransform`, which writes the scene's mutable `m_PathScratch` (`Scene.cpp:1673-1705`, `Scene.h:747`).
   The parallel prelude now uses `Scene::FindCurrentWorldTransform` (read-only, `Scene.h:349-352`, as
   `SyncEntitiesToBodies` does at 3206) and sends what it cannot answer, and `SuspendUnwritableBody`, to the serial path.
6. **Major: the pair count would drift (step 4 item 1).** `EndContactsOf` erases an entity's whole `EntityPairs` list
   (`PhysicsWorld.cpp:1266`) without `RemovePairFromIndex` for that entity, so a count kept only in `AddTouchingPair` and
   `RemovePairFromIndex` would go stale and skip pairs. The count is now kept wherever a list changes, checked in Debug
   by `GetStats`, and the destroy and deactivate event tests (`PhysicsEventTests.cpp:104`, 137) are named as its checks.
   2.2 now also says that scripts' collision callbacks use watched entities (`ScriptSystem.cpp:99`), so the begin-loop
   early-out does not apply to games with any watched entity.
7. **Major: free-fall speed test (4.1).** With Strata's default `LinearDamping` 0.05 (`PhysicsComponents.h:31`) a box
   falling for about a second is some 5% slower than g t, and the displacement speed lags the velocity under box3d's
   sub-stepped integration: "within 2% of g t" would fail. The test now zeroes the damping and compares with
   `b3Body_GetLinearVelocity` from 0.5 s of fall.
8. **Major: the 0.7 m reading could not be set (D3, 4.2).** Q2 promises that the owner's reading is one command away, but
   the field's range (0-0.5 m) excluded 0.7 m. The range is now 0-1 m, and the field's description says it is a distance
   in meters.
9. **Major: probes that cannot see small distances (4.1 item 4, 4.3 gate).** A contact is recycled only while its motion
   since the cached pose stays within the distance, that is below about distance x 60 m/s (3 m/s at 5 cm, 0.2 m/s at
   0.35 cm); the seam probe's 2, 4 and 8 m/s would show every distance under 3.5 cm as identical to 0. Slow speeds
   (0.25, 0.5, 1 m/s) and a ledge case (stale support past an edge) are added to the probes, the gate and
   `Physics.Settling`.
10. **Major: `Characters1000Step` rationale (2.2, 4.2 acceptance).** box3d pairs kinematic bodies only with dynamic ones
    (`broad_phase.c:614`, 682-688), so the crowd on static ground has no contacts and turning presence bodies' recycling
    off cannot change that metric; the acceptance now expects no change there and measures the cost where presence bodies
    touch dynamic bodies.
11. **Major: wrong suite for the perf log (4.1, section 6).** The perf-log columns are tested in `Runtime.Game`
    (`GameRuntimeTests.cpp:401-424`, which requires exactly 8 columns today), not `Runtime.FramePerfLog`.
12. **Major: the A/B base would not build, and direct runs append results (sections 7, 12).** `git archive` leaves out
    the submodules; the base is now a worktree with its submodules. Running `StrataTests.exe` directly appends to
    `PerfResults/Release.json` (only CTest's clean step deletes it, `StrataTests/CMakeLists.txt:268-269`), so each run now
    starts by deleting it and keeps a copy.
13. **Major: budgets lowered inside the stream (sections 7, 4.4, 11).** `plan.md` 4.1 keeps `PerfStability.py` campaigns
    out of the streams and AGENTS.md requires `PerfStability.py --runs 5` on the reference machine before budgets change;
    3 A/B pairs are not that. The stream now adds provisional entries and targets only, and the integration campaign
    lowers budgets. `PerfStability.py` is used in its report mode only.
14. **Major: Q1's recommendation conflicts with `decisions.md` (section 10).** Its step 0 rule allows a re-baseline only
    for a machine change and calls the box3d port's slower metrics regressions to explain. The recommendation stays for
    the orchestrator, with a note that the stream does not touch the entry until the exception is recorded in
    `decisions.md` or granted by the owner, and with step 0's own split (box3d 11.4 of 18.1 ms).
15. **Major: the Debug counter check would walk every record per round (4.1).** `CheckRecordIndex`, the model the spec
    named, is constant time by design (`PhysicsWorld.cpp:921-928`); a full walk after every `ApplyChanges` round runs
    every frame and every fixed step in Debug. The check moved into `GetStats`, which walks anyway; the counter also
    counts presence bodies, which bypass `AddToSimulation` (`CharacterControllers.cpp:280-305`).
16. **Minor: sleep threshold walk (4.2).** "Every moving body in the simulation" would leave disabled bodies with the old
    threshold when they come back; the walk now covers every non-static record with a body and the presence bodies.
17. **Minor: `physics.stats {detail: true}` (4.1).** It switched per-step counters on until play stopped: a query with a
    side effect. It now reads the counters once at the query; code turns per-step reading on through
    `IPhysicsDiagnostics::SetDetailedStats`. The Playground's new checks pass `detail: true` when needed, and the
    Streaming row shows the recycled share only when the counters are read every step.
18. **Minor: `b3World_GetMaxCapacity` is a peak, not storage (2.3, 4.1).** It returns the largest counts seen at step
    starts (`physics_world.c:1078-1093`). The field is now `LibraryContactPeak`, and `ContactRoomGrowths` counts the steps
    after which the peak passed box3d's reserved contact room or its next doubling.
19. **Minor: worker rule (4.4 item 4, R4).** A rule in hardware threads would make tests machine-dependent and could break
    the golden-hash case's `WorkerCount == 5` with 4 job workers (`PhysicsBox3DTests.cpp:115-117`). It is now expressed in
    the JobSystem's worker count and must keep that expectation. `SetWorkerLimit` moved to step 1, whose helper already
    takes `--workers`.
20. **Minor: the instruments' 1% acceptance was not measurable (D2, 4.1).** 1% of `Boxes70k.StrataStepTime` is 64 us,
    and the five runs' medians span 6.34-7.26 ms. It is now judged by timing the instruments' own code in process, with
    the A/B as information.
21. **Minor: D6 overclaimed cross-platform determinism.** Results are deterministic per build and independent of the thread
    count; across compilers Strata's own math may differ in the last bits, which the bounds' margin absorbs.
22. **Minor: D4's precedent and D8's throwaway edit.** `SyncLayers` compares the scene's copy every step; the
    version-cached read is `ProjectCollisionLayers::Find` (`ProjectCollisionLayers.cpp:91-111`). The angle edit must be
    made in a separate throwaway worktree, not the stream's submodule checkout (a `-D` cannot override a plain `#define`).
23. **Minor: docs placement (section 8, 4.2).** DG's "## Project settings" does describe its sections (Collision layers
    988, Input actions 1037); the spec said it listed none. `Physics.Simulation` is now documented there, linked from the
    new "## Physics", which also goes into the guide's table of contents.
24. **Minor: facts.** `T/Physics` holds 7,740 lines, not 8,232; `SceneGenerators.h` is 104 lines; busy frames' p95 is
    1.3-1.7 times the median; the falling-box test checks that more than half the boxes are awake, not nearly all, so the
    per-box figures are lower bounds per awake box; `Boxes70k` has up to 70,000 islands at the busy phase's start, not
    about 7,000; the character prelude reads one world transform per controller plus one per parented controller; end
    events look pairs up rather than asking `IsWanted`; `GhostsOnPitFloor` counts the Playground's "Ghost"-tagged bodies
    (`Playground.cpp:157-166`), not ghost collisions; `CellUnloadMaxFrame` is the longest frame of the whole unload,
    which was the request's frame in all five runs; `Grow65k.GrowthMs` is a difference of two medians of about 100 ms
    each and is reported with their spread; the 13.5 ms frame target needs about 0.7 ms off box3d's own step on top of
    the Strata target.
25. **Added: `Physics.Settling`'s Debug time.** `CoreShards.json` keeps each shard under about 60 s in Debug; the suite's
    time is stated, and it gets its own shard if `Systems` would pass that.

Unverified here (left to the implementer): the cost of `b3World_GetCounters` at `Boxes70k`; how Strata's step splits;
the Playground's and the ball pit's time to sleep; step 0's names until it lands; whether `Scene` offers a write guard
for R6.

## As built

Notes of the implementation where it differs from the text above (main at cc9524a1, step 0 landed):

- **Step 0's names.** `Perf.Physics.Create10kBodies` was re-baselined by step 0 itself (`Measured` 15.76, `Budget` 23.64,
  `Target` 7.995; the roadmap's open items say so), so Q1 needed nothing from this stream. The stream builds in its
  worktree's `build/windows` (the streams' brief) rather than `build/agent-physics-runtime`.
- **Step 1.** The counters' branch: `b3World_GetCounters` costs 0.09-0.10 ms at 70,000 boxes, above the rule's 43 us, so
  they are read every step only while detailed statistics are on. The perf log turns them on for the scene it logs (its
  column writer calls `IPhysicsDiagnostics::SetDetailedStats`). `PhysicsStats` also reports `MovingBodyCount` and
  `InstrumentTime`. The settle helper takes `--workers` and `--collision-steps` from step 1 and the tuning options from
  step 2, which adds the settings. The ball pits use the Physics Playground's balls as it spawns them (its 0.5 m prefab
  scaled by 0.5: 0.25 m radius), not 0.5 m; the crate push's pushed crate weighs 5 kg (the Third-Person Adventure's 25 kg
  crate stops a character's push after one step: its script pushes it), and the head crate rests on a standing
  character, then rides a walk at 0.5 m/s (a teleported presence body carries nothing, so a walking character loses it
  whatever the tuning). The seam probes' excursion is the range of a body's height along its lane once the contact
  settled (a triangle mesh holds bodies a few millimeters above its nominal top).
- **Step 2.** The contact tuning (hertz, damping ratio, push speed) became fields of the section by 4.3 item 5's rule.
  Its commit stated no A/B of `Perf.Physics`; an interleaved A/B of the stream against cc9524a1 (three pairs, Release, the
  loaded machine, the medians of the pairs' first attempts) measured `Characters1000Step` at 0.92 ms on both sides (base
  0.98, 0.89, 0.92; stream 0.97, 0.92, 0.88): no change outside noise, as step 2's acceptance asks. The same pairs:
  Strata's part of a `Boxes70k` step 10.4 -> 6.0 ms, `Boxes30kTracked` 4.3 -> 3.1, `Boxes10k` 0.61 -> 0.44.
- **Step 3: the rule's outcome, for the orchestrator.** Only 2.5 cm passes the rule's gates (ghost and cost). The rule's
  acceptance asks the 2,000-ball pit to fall asleep at `d*` no later than at 5 cm; a pile's time to sleep is chaotic, so
  it was measured over nine layouts of the pit (`Tools/PhysicsSweep.py pits`): at 0.08 m/s 2.5 cm's median is 22.6 s
  against 5 cm's 14.7 s (one layout of nine not asleep within 40 s at 2.5 cm), at 0.05 m/s 34.3 against 21.0. So 2.5 cm
  fails the acceptance, no other distance passes the gates, and the stream kept box3d's 5 cm, which fails the ghost gate
  (the seams' worst excursion 5.0 mm above distance 0's, against 2 mm; the ledge's travel 1.7 cm more, against 1 cm) and
  removes a 47 mm bump on triangle meshes that every distance up to 2.5 cm keeps. The rule names no outcome for this case:
  the orchestrator rules on it. (The first sweep decided the same from one run of the pit per distance, which said 2.5 cm
  never slept; that claim was the pit's chaos, and the medians replaced it.) The integration ruling (2026-10-28):
  box3d's 5 cm stays the default, recycling on for every body but characters (and kinematic bodies), and its ghost cost
  (seams 5.0 mm above distance 0, ledges 1.7 cm) is accepted and measured by `Physics.Settling`'s bounds.
- **Step 3: what follows from it.** `Physics.Settling`'s seam and ledge bounds are the cost measured at 5 cm times 1.25
  (6.3 mm, 2.2 cm), not "at most 2 mm above distance 0's", which 5 cm does not meet; the ball pit's bound is the slowest
  time seen over its layouts and over contact tuning a few bits off the defaults (27.0 s) times 1.25, 34 s, not 1.25 times
  one run, which another platform's trajectory could exceed. The sleep threshold moved to 0.08 m/s by the rule (0.08
  shortened both pits' time to sleep in every layout: medians 14.7 against 21.0 s and 11.4 against 15.1). Kinematic bodies
  stopped recycling (the platform probe, item 6). The angle reading was not measured: the share recycled keeps rising with
  the distance while bodies move (the 2,000-ball pit's busy phase 26% at 0.35 cm, 44% at 5 cm, 49% at 10 cm), so the
  spec's trigger did not fire.
- **Step 3: the sweep's samples.** The Third-Person Adventure and Open World drivers were not built. The Open World has no
  dynamic body (its cells' bodies are static and static pairs make no contacts), and the Third-Person crate is moved by its
  script (step 1's note), so the generated `CratePush` stands for its push; both samples' regressions
  (`StrataEditor.ThirdPerson`'s crate displacement, `StrataEditor.OpenWorld`'s tour) pass at the new defaults. The Physics
  Playground was swept at 0.05 m/s and, since, at 0.08 m/s at every distance (its pit 99% asleep after 10.0 to 15.8 s, one
  run each, within the pits' spread).
- **Step 4.** Item 2 did not land (the same random access, no gain); items 5 and 7 went below the cut by the 3% rule;
  item 11, below the cut, landed: static bodies leaving the simulation without contacts are parked and disabled 60 steps
  later in slices of 1,024 a step. Spec 2.4's question whether contact storage grows while playing: piles of balls grow it
  once as they form (4.7 to 6.1 contacts a body against the automatic room's 2); `c_ContactsPerBody` stays 2.
- **Release notes.** The lines this stream's defaults and features need are in `Docs/Plans/Wave7/ReleaseNotes.md`.
