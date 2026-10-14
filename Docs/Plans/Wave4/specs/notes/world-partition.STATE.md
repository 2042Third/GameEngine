# world-partition spec — STATE (for a successor)

Task: write scratchpad/wave4/specs/world-partition.md (format of wave3/specs/animation.md). Read-only on repo.
Code: <repo>/.worktrees/verify (== main 685aef7).

## Progress
- [ ] read plan.md, common.md, wave3 animation.md (format)
- [ ] read audit/scale (STATE.md, findings.md, logs)
- [ ] read Roadmap pillar 'Layered worlds'
- [ ] read code: SceneSubScenes, PhysicsWorld, GameRuntime, GameManifest, SceneServices, EditorSceneCommands, StreamingPanel, GameExport, AssetDependencies, Layers.json
- [ ] write spec

## Notes (facts gathered)
- [x] read plan.md, common.md, animation.md format (Why/Scope numbered areas/Deliver order/Acceptance/Open items/Files/Merge notes)
- [x] audit/scale STATE.md: runcells (4x25k cells, 65536 limit at 3rd cell, frames 13.6 med/51 p95 with waiting; unload 51-56 ms frame Physics 50.9), rungrid (10x10 cells 2502 ent / 501 bodies; loads 19-23 ms over ~30 frames, max frame 7.7-8.5 ms), 1M: play start 599 ms (Extract 360, Physics 157), 1.44 GB edit/2.88 GB play.
- scene.unloadAdditive result "entities" = remaining scene entity count (not destroyed count)
- Scene.h: SceneSettings{Gravity,FixedTimestep,MaxFixedStepsPerFrame,PhysicsCapacity{MaxBodies,MaxBodyPairs,MaxContactConstraints; limits 2^21,2^22,2^19},MaxEntitiesInstantiatedPerFrame=5000,PhysicsLayers}; c_MaxInstantiationMillisecondsPerFrame=2.0
- SceneSubScenes.cpp: LoadSubScene(AssetHandle, SubSceneOptions{Priority,RequiredProgress}) -> SceneLoader::LoadAsync; BeginAdditiveLoad(prepared); UnloadSubScene -> DestroySubSceneEntities -> DestroyEntities(roots) (one call); UpdateSubScenes (stage Scene.Load), AdvanceSubScene(budget, deadline), KeepIDs=true; SubSceneComponent{SubScene, Source} runtime-only; SubSceneState{Loading,Instantiating,Loaded,Failed}; m_DeferredSubSceneUnloads
- Physics: PhysicsWorld(Scene, settings, demand) ctor: CountBodyCandidates -> ResolveCapacities (c_MinAutomaticBodies 65536, pairs 4x, contacts 2x bodies min 10240), Jolt Init(MaxBodies+2 anchors), StructureChanges for every physics entity in hierarchy order -> ApplyChanges (serial, main thread) -> OptimizeBroadPhase. BodyRecord in std::map<entt::entity,BodyRecord> Bodies; BodySlots per Jolt index; PendingAdds/PendingActivatedAdds batches (AddBodiesPrepare/Finalize in FlushBodyBatches). BuildFailure::BodyLimit retried EVERY STEP via SyncBody (PolledBodies; NeedsPolling true for BodyLimit) -> 34k waiting = 13.6 ms frames. ReportBodyLimit message ~line 1202. Demand remembered per lineage in PhysicsSystem static map (process only), never saved.
- Jolt API available: BodyInterface::CreateBodyWithoutID (const, thread-safe) + AssignBodyID(body[,id]) ; UnassignBodyIDs + DestroyBodyWithoutID; AddBodiesPrepare (any thread)/Finalize; PhysicsSystem::SaveState/RestoreState (needs same bodies+IDs -> usable for migration into 2x system with same BodyIDs: velocities, sleep, contact cache).
- CharacterControllerSet(scene, JPH::PhysicsSystem&, settings): holds the Jolt system -> migration must rebind/recreate CharacterVirtuals.
- PhysicsSystem: services ICollisionEventSource, IBodyMotionQuery, IBodyControl, IPhysicsQueries, ICharacterControl; registered "Physics" After Scripting, stage Scene.Physics, runs in Simulate. PhysicsStats has BodyCount, PendingBodyCount, MaxBodies..., BodyOverflowCount, Demand, AddBatchCount, BuildCount...
- Scene systems only exist while scene runs (OnRuntimeStart..Stop); edited scene runs none -> edit-mode streaming must be editor-owned (EditorWorld drives a WorldStreamer core). DECISION: WorldStreamer core class (World module) owned by WorldStreamingSystem in Play/Simulate and by EditorWorld in Edit; both profile World.Streaming.
- ComponentFlags: Hidden, NotRemovable, NoCopy, NoSerialize, EngineAdded. Feature test requires EVERY registered component in Feature.stscene with non-default props (no exemption). -> cell membership stays runtime-only (unregistered), via SubSceneComponent{SubScene, Source=cell scene handle}; EntityState (Editor/SceneEdit.h) gains Cell field.
- SceneEdit: EntityState{ID, Exists, Components json, Parent, SiblingIndex}; SceneEditAction; ApplyEntities; undo may fail & keep step (existing contract).
## Design decisions so far (2026-10-13)
- World = persistent scene (regular .stscene with a WorldComponent{World asset}) + .stworld doc {Name, PersistentScene, GridOrigin, Layers[{ID,Name,CellSize,StreamingDistance,EnabledByDefault,LoadBeforeStart}], Cells[{Layer,X,Z,Scene}], Index(cook: per-cell Bounds/Entities/Bodies/ClosureBytes/Stamp; MaxBodiesInRadius)}; cells at <WorldDir>/<WorldName>/<LayerName>/<X>_<Z>.stscene, handle = DeriveSubAssetHandle(world,"Cell/<layerID>/<x>/<z>") (deterministic). NO manifest bump: start scene = persistent scene; WorldStreamingSystem (World module, After Scripting Before Physics, RunsInSimulate) streams.
- Core class WorldStreamer (World layer) used by WorldStreamingSystem (play/sim) and EditorWorld (edit, editor camera source). Ranking single-threaded deterministic: (priority desc, distance asc, key asc); hysteresis H=max(cell/2, 10% R); max concurrent loads.
- Cell membership = SubSceneComponent.Source (cell scene handle) on top-level entities; new Scene API: AdoptSubScene, SetSubSceneOf, FindSubSceneBySource, GetSubSceneOf, sliced UnloadSubScene (state Unloading; InactiveComponent on roots at once; post-order slices within SceneSettings::MaxEntitiesDestroyedPerFrame (new, 5000) and shared 2 ms); reload of an unloading source waits.
- EntityState gains Cell (AssetHandle); SceneEdit::CheckCells pre-check: undo of a step whose cell isn't loaded fails with reason + EditorWorld loads it (existing 'undo may fail' contract).
- Runtime start: SceneSystemDescriptor gains PrepareLoadedScene hook (runtime loads only: SceneLoadOptions::PrepareSystems); World's loads LoadBeforeStart cells into the prepared scene; Physics builds PreparedPhysicsWorld (scene ctx) in parallel slices, adopted at OnRuntimeStart. Shared SceneWorkBudget passed to Prepare hooks (RenderScene::PrepareLoadedScene signature change -> one line, flag gpu-driven).
- Physics: parallel build (CreateBodyWithoutID on jobs, AssignBodyID main in order, AddBodiesPrepare chunks); growth by migration between steps (new JPH system 2x, same BodyIDs, SaveState/RestoreState Bodies|Contacts, CharacterControllerSet::Rebind, old system destroyed on background job); waiters FIFO woken on slot frees (not polled); IPhysicsCapacity service {GetInfo, Reserve(bodies)} streamer pre-reserves from cooked counts; physics.stats.
- Commands: world.create, world.info, world.setLayers, world.edit (session: editLayer, visibleLayers), world.moveEntities, world.loadCells, world.cook, world.status, physics.stats.
- SDK: World::RequestCellsAround/ReleaseCells(WorldCellRequest), IsLoadedAround, SetLayerEnabled, IsLayerEnabled (5 host fns: WorldRequestCellsAround, WorldReleaseCells, WorldIsLoadedAround, WorldSetLayerEnabled, WorldIsLayerEnabled) through IWorldStreaming scene service.
- Questions for planner: edit-mode 'scene system' = shared core; FramePerfLog column API (we merge before gpu-driven); SceneSystemDescriptor hook vs engine-modules; no manifest bump; residency info walk (Roadmap says world-partition, table says world-io); Prepare signature change touching RenderScene.
## Writing progress
- [x] spec part 1 written: header, Why, Design paragraph, Scope 1-8 (physics, sliced unload+budget, World module, editor, commands, cook, observability, SDK+feature)
- [ ] part 2: "What it guarantees others meanwhile", Deliver order, Acceptance (tests/perf/screens/docs), Open items, Files, Merge notes
- [x] part 2 appended (guarantees, deliver order, acceptance, perf, screens, docs, open items, files, merge notes)
- [ ] review pass: fix world.create undo (AssetCreateAction), check consistency, then StructuredOutput
- [x] review pass done: world.create undo -> AssetCreateAction (only without open); UndoStack single observer -> observer list (added to scope+files); growth only at Simulate start/prep slices; area refs fixed
- [ ] StructuredOutput (summary, specPath, risks, questionsForPlanner)
