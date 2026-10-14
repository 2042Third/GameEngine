# Workstream `world-io`: World I/O: saves off the frame, streamed and binary scenes, asset registration and imports at the scale of tens of thousands of assets (wave 4, size XL, new)

Read `common.md` first, then `plan.md`. A new workstream, split out of world-partition: the scale audit of main 685aef7
(`wave4\audit\scale\`, `STATE.md` lists every run) found that the paths around the frame fail before any partition
exists. Saving, loading, registering, importing and packing must never freeze a frame or blow memory, at a million
entities and at tens of thousands of assets. This serves the owner's "high scaling, assets streaming optimized".

**Merges second**: after engine-modules, before world-partition, whose cells are written, saved, cooked and read
through this stream's formats and paths.

## Why

Findings this stream closes (scale audit unless noted; line numbers are main 685aef7's, locate code by symbol):

- **C: `scene.save` freezes the editor's main thread for seconds.** 22 s at 1,000,404 entities and 4.8 s at 250k
  (`runsave.log`: `editor.perf` shows a 4,799 ms frame, all of it "untimed"); private memory peaks at 8.76 GB during the
  1M save (`run1M.log`). `EditorContext::SaveScene`/`SaveSceneAs` build the whole document as a DOM
  (`SceneSerializer::Serialize`), scan its references (`AssetDependencies::ScanDocument`), dump it with tab indentation
  (`JsonUtils::Dump(json, 1, '\t')`) and call `EditorAssetManager::SaveNativeAsset`, which **runs the scene loader on the
  bytes to validate them**: the whole document is parsed into a DOM and built into a scene a second time, on the main
  thread. The save also grows the file: 263 MB of compact JSON become 541 MB (`SceneSerializer::SaveToFile` and the
  editor's save indent with tabs).
- **C: scenes load and save as one JSON document on one thread.** `scene.open` of the realistic 1M world (1,000,404
  cubes in 2,500-cube districts, 10% static colliders) takes 8.2 s, peaks at 3.95 GB private and holds 1.44 GB once open
  (`run1M.log`, `runopen1M.log`). The exported game's 1M start scene plays at 8.5 s and peaks at 3.96 GB
  (`rt1M_headless.csv`); quitting during that load took 8 s, because the parse cannot be cancelled (`rt1M_window.log`).
  1,100,445 entities are refused only after 7 s of parsing (`runover.log`): `SceneSerializer::DeserializeEntities`
  checks the cap only once the DOM exists. The gated `PerfGPU.Editor.SceneOpen1MWorkingSet` uses empty entities, so no
  gate sees this. (Rendering audit side note: a 1M JSON scene peaks at 2.95 GB while rendering.)
- **C: registering assets costs about 3-10 ms each on the main thread and in the first scan; a fresh 30,000-asset
  project times out `StrataCLI launch`.** `asset.import` of a folder froze one frame for 1,707 ms with 300 small PNGs
  (`runsmall.log`) and 3,605 ms with 600 (`runsmall600.log`, all in `Assets.Update`): `UpdateExternalImports` calls
  `RegisterAddedFile` per copied file (`EditorAssetManager.cpp:2691`), which reads and writes the `.meta` file, stats the
  file three times, reads the import record and queues the import; `Update` then rewrites the whole `AssetIndex.json`
  (`SaveAssetIndex`). `Core/FileSystem.cpp:56-86` makes every write a temporary file plus a rename. First scans: 26.4 s
  for 10,000 materials on a quiet machine, 233 s for 30,000 under load; a warm scan of 30,000 takes 13.3 s.
  `EditorAssetManager::Scan` registers files one by one on the calling thread (`RegisterSourceFile`) and writes one
  record file per asset (`<cache>/<handle>.import`). `EditorLayer::OnAttach` opens the project (and so scans it) before
  it starts automation, so the session file appears only after the scan; `StrataCLI launch` gives up after a fixed
  60 s (`StrataCLI/src/CLI/EditorLauncher.h:26`, `McpServer.h:32`): "Timed out after 60000 ms waiting for the editor".
- **M: import memory is unbounded.** Importing 24 4096x4096 PNGs took private memory from 737 MB to 3.74 GB with a
  2.83 GB plateau (`runimport2.log`): `SubmitImport` (`EditorAssetManager.cpp:1700-1730`) submits every import at once;
  the only limit is `RunImport`'s `s_LargeImportMutex` for sources above 64 MB.
- **M: pack v2 inflates small assets 10.7x.** 30,001 materials with 11.5 MB stored become a 122.5 MB pack
  (`runmany.log`): every entry starts at a 4 KiB boundary (`AssetPack::c_EntryAlignment`, `AssetPack.h:95-96`;
  `AssetPackWriter.cpp:82`) and is compressed on its own.
- **m: command work is invisible to the frame profiler.** The 4,799 ms save frame shows as "untimed".
- **Rendering audit side note:** `project.export` sat in `WaitingForImports` for more than 15 minutes after three large
  scenes were written into the open project, while `editor.status` reported 0 imports: `HasPendingImports`
  (`EditorAssetManager.cpp:3003`) counts searches of what native documents reference, and `QueueNativeReferences`
  queues a new whole-document search for every change event without cancelling the previous one.
- **M (part): the perf gates do not cover a world.** No fixture has realistic worlds or many-asset projects.
- **Roadmap open items** (Docs/Roadmap.md "Open items"): an applied import still writes its `.meta` file and the asset
  index on the main thread (about 2 ms); the first scan of large scenes without reference records spends seconds in the
  reference search (4 s for the 1M scene); `GetResidencyInfo` and `GetResidencySamples` walk every registered asset.

## Scope

### 0) Fixtures first (`StrataTests/src/Perf/WorldFixture.{h,cpp}`)

Ports of the audit's generators, deterministic (fixed seeds and handles), writing only through `Tests::CreateTemporaryDirectory()`
paths. world-partition's and gpu-driven-rendering's gates reuse them, so the header lands in this stream's **first
commit** and its API does not change afterwards (add, never rename).

- `WorldProjectSpec { std::vector<WorldSceneSpec> Scenes; uint32_t Materials = 8; }` with
  `WorldSceneSpec { std::string Name; uint32_t Cubes; bool Colliders; }` and
  `WorldProject WriteWorldProject(const std::filesystem::path& directory, const WorldProjectSpec&)`: `gen_world.py`'s
  worlds. A camera at walking height near the centre, a sun, a procedural sky, a ground plane, and `Cubes` cubes (the
  built-in cube mesh, one of 8 materials) on a square grid 4 units apart, grouped under district entities of 2,500 cubes
  (50x50); with `Colliders` every 10th cube also gets a static `BoxCollider` and `RigidBody`. Scene files are written
  compact (as tools write them) **and** through `SceneDocumentWriter` once it exists (`WorldSceneSpec::Layout`), so gates
  can measure both. Presets `c_World250k` (250,104 entities) and `c_World1M` (1,000,404).
- `ManyAssetsProject WriteManyAssetsProject(directory, uint32_t materials, bool withMetaFiles = true)`: `gen_many.py`:
  materials in folders of 1,000 and a scene that uses all of them (one cube each).
- `WriteTextureFolder(directory, uint32_t count, uint32_t size, uint32_t seed)`: `gen_textures.py`: RGB PNGs of
  gradient plus noise (stb_image_write, compression level 1), and `WriteSmallTextureFolder(directory, 600, 64)` for
  the registration gate.
- Helper modes (`TestMain.cpp`, like `write-stress-project`): `--strata-test-helper=write-world-project <dir>
  <name>:<count>[:colliders]...`, `write-many-assets-project <dir> <count>`, `write-texture-folder <dir> <count> <size>`,
  so the stream (and the integrator) can make the audit's projects for manual runs and screenshots.
- `WriteWorldGame(project, outputDirectory)`: exports a world project with `GameExport` (headless, no GPU) for the
  runtime gates.

### 1) Saves off the frame

The critical item. The main thread only takes a snapshot; serializing, scanning references, hashing and writing run on a
background job; the result is published on the main thread in one cheap step.

- **Streamed writer, written here because the save needs it** (the reader half is item 3):
  `Scene/SceneDocumentWriter.{h,cpp}`.
  - `SceneDocumentWriter::Write(const Scene& scene, DocumentOutput& output, const SceneWriteOptions& options,
    SceneWriteResult* outResult, std::string* outError)`. `DocumentOutput` is a small interface (`Write(std::string_view)`,
    `GetBytesWritten()`); `Core/AtomicFileWriter.{h,cpp}` implements it for files (a 1 MiB buffer into
    `<path>.tmp<n>`, `Commit()` flushes, closes and renames, `Abort()` removes the temporary file, the destructor aborts),
    and a string output serves small documents and tests.
  - Layout (format and version unchanged, `{"Strata": {"Format": "Scene", "Version": 1}}`): the header object first,
    then `"Scene": {"Name", "Settings", "EntityCount", "Entities": [` and **one entity per line**, in hierarchy order
    (parents first, sibling order), then `]}}` and a final LF. Each entity line is byte-identical to
    `JsonUtils::Dump(<the entity object today's SerializeEntity builds>)` (compact, keys sorted as nlohmann sorts them:
    `Components`, `ID`, `Parent`; components and properties by name), so a re-save of an old file differs only in
    whitespace and the files diff by entity. `EntityCount` is new and optional: readers that know it refuse an over-cap
    document at once and reserve storage; older readers ignore it. About half of today's tab-indented size.
  - Floats are formatted directly (no per-value nlohmann parse as `FloatToJson` does now), producing exactly the text the
    DOM path produces: a test compares the fast formatting with `JsonUtils::Dump(FloatToJson(value))` over every
    exponent and the edge values (zero, negative zero, subnormals, the extremes, non-finite values, which become 0 as
    today). Strings get JSON escaping and invalid UTF-8 replaced by U+FFFD like `JsonUtils::Dump`.
  - Entities are serialized in parallel chunks (`SceneWriteOptions::ChunkEntities`, 4,096) on the background threads
    (`JobPriority::Background`) and written in chunk order, so output never depends on thread count.
  - While writing it collects what the document references (asset-typed properties and script fields of asset type, each
    handle once in first-appearance order: equal to `AssetDependencies::Scan` of the written bytes, which a test checks),
    counts entities and bytes, and hashes the bytes (SHA-256, for the recovery journal).
  - `SceneWriteOptions`: `Kind` (`Scene`, `Prefab`, `Model`), `Roots` (`std::vector<UUID>`; empty: the whole scene; else
    the entities with these IDs with their descendants, parents kept only within the subset: **entity-subset writes**,
    which world-partition uses for cells; IDs, not handles, because the writer runs on a snapshot copy, which keeps
    IDs; the requester lists them on the main thread when it takes the snapshot), `WriteSettings` (default true; cells
    write none, they use their host scene's), `Name` (overrides the scene's), `Cancel` (checked between chunks),
    `Progress`, `ChunkEntities`, `Priority`.
  - `SceneSerializer::SaveToFile` uses it. `SceneSerializer::Serialize`/`SerializeEntities` (DOM) stay for small
    documents (undo snapshots, clipboard, commands) and keep their signatures.
- **Snapshot.** `Scene::Copy(source, SceneCopyOptions{ .SerializedOnly = true })` (function-level in `Scene.*`):
  copies, storage by storage on the job threads as `Scene::Copy` does, the IDs, hierarchy links, unknown components and
  every serializable registered component (whatever `NoCopy` says), and skips cached world transforms and other runtime
  caches. It runs no context copiers (gpu-driven-rendering's `Scene::RegisterContextCopier`: a save needs no render-scene
  clone). Measure it on the realistic 1M world (today's `Perf.Scene.Copy1M` is 43 ms for empty entities). If it cannot
  fit the 60 ms frame gate, fall back to a **sliced snapshot**: storages copied over consecutive frames while the edited
  scene refuses edits (`CheckSceneEditable`, as during scene loads; edits through commands defer), and say in the report
  which one shipped and why.
- **`Editor/DocumentSaver.{h,cpp}`** (StrataEditorCore; new), the one background save path, **taking several documents
  with one completion** (world-partition saves dirty cells through it):
  - `DocumentSaveRequest { Ref<const Scene> Snapshot; std::vector<DocumentSaveItem> Items; UndoStack::SavePoint SavePoint;
    std::string Label; }` with `DocumentSaveItem { AssetHandle Asset; std::string NewPath; SceneWriteOptions Write;
    std::optional<std::string> Document; bool Reload; }`: an existing native asset, or a new one at `NewPath`, which
    `EditorAssetManager::ReserveNativeAsset(relativePath, AssetHandle handle = {})` reserved when the request was made
    (with a given handle, such as a world cell's derived handle, or a new one). An item with `Document` writes those bytes
    as they are instead of the snapshot's entities: small engine-native documents made on the main thread (a World
    document's refreshed index), validated by their loader on the job like `SaveNativeAsset` validates documents from
    elsewhere. `Snapshot` may be null when every item has a `Document`.
  - `uint64_t Submit(DocumentSaveRequest, std::function<void(const DocumentSaveStatus&)> onComplete)`, `Update()` (main
    thread, once per frame from `EditorContext::Update`), `GetStatus(id)` (`Queued`, `Writing`, `Publishing`, `Done`,
    `Failed`; progress; bytes; seconds; per-item results and errors), `IsBusy()`, `IsSaving(AssetHandle)`, and
    `FinishAll()` (blocking: only for tools, tests and the editor's teardown after its last frame).
  - One request writes at a time; **a second save queues behind the first**. A queued, unstarted request for the same
    documents is replaced by the newer one (both completions fire when the newer one is written), so repeated Ctrl+S never
    holds two 1M snapshots.
  - The job writes each item through `AtomicFileWriter` next to its target (a new asset also gets its `.meta`, written by
    the job), then renames under the manager's exclusive file lock (`DocumentWriteClaim::Commit`; the claim keeps the
    manager's `BackgroundWork` alive, so closing the project never waits for or breaks a write). Items fail
    independently (each file is atomic); the request fails when any item failed, naming them.
  - Publication (main thread, one step per request): `EditorAssetManager::PublishWrittenDocument(const WrittenDocument&)`
    registers new assets with their reserved handles (no disk reads), updates the source's size and time and the stored
    size, records the job's references (`RecordReferences` and the records store, item 2), reloads loaded copies when asked,
    and releases the claim. **No validation parse**: the writer is the engine's own; `SaveNativeAsset` keeps validating
    documents that come from elsewhere (materials, `AssetEdit`).
  - The file watcher leaves claimed paths alone until publication (`DocumentWriteClaim`, like `ExternalImport`'s claimed
    files), so a save never triggers its own reference search or reload.
- **`EditorContext`** (function-level): `BeginSaveScene(std::string* outError) -> uint64_t` and
  `BeginSaveSceneAs(relativePath, outError) -> uint64_t` take the snapshot, call `UndoStack::BreakMerge()` and capture
  `UndoStack::GetSavePoint()` (new: position and the serial of the action there), and submit. On completion:
  `UndoStack::MarkSaved(SavePoint)` (new overload: marks that point saved; when the history no longer holds it, because
  the person undid past it and edited, no point is saved and the scene stays modified), `OnSceneSaved()`, and
  `SceneRecovery::OnSaved`. **Edits made during the save keep the scene dirty**: the save
  point is the snapshot's. `SaveSceneAs` makes the reserved handle the edited scene's at once (so a following
  `scene.save` targets it) and restores the previous handle and name if the save fails. A save whose edited scene was
  replaced meanwhile (`scene.open`, `scene.new`) still publishes its file but marks nothing and notes nothing in the new
  scene's journal. `SaveScene`/`SaveSceneAs` stay as blocking wrappers (Begin, then `FinishAll`) for tools, tests and
  `CreateProject`'s template scene. `HasSavesInFlight()`, `GetSaveStatuses()`.
- **Recovery journal**: `SceneRecovery::OnSaved(handle, sceneFile, scenePath, std::string baseHash, UndoStack::SavePoint)`
  gets the SHA-256 the job computed of the written document (no 270 MB string is kept for it), and **carries the steps
  applied after the save point into the new journal** (today it clears `m_PendingSteps` and starts an empty journal, which
  would lose edits made while a background save ran). Steps are tagged with the undo serial they came from.
- **`scene.save` becomes deferred** (function-level in `EditorSceneCommands.cpp`; no new command): `scene.save {wait}`
  and `scene.saveAs {path, wait}`. With `wait` (default true) the command answers once the document is written and
  published: `{scene, path, bytes, entities, seconds, frames, frameTimes {count, medianMs, p95Ms, maxMs}, memory
  {privateMBBefore, peakPrivateMBDuring}}` (frame times and private memory sampled once per frame by its poll, like
  `scene.open`'s). With `wait: false` it answers at once with `{scene, path, saveId, state: "Writing"|"Queued"}`.
  Descriptions say so (agents read them). Failures keep `EditorCommandError::Failed`.
- **Commands that replace the scene** (`CommandUtils::CheckUnsavedChanges`, function-level): while a save of the edited
  scene is in flight they defer until it is published and then decide as today, so `scene.save {wait:false}` followed by
  `scene.open` works without `force`.
- **Closing and quitting never lose a save in flight, and show progress.**
  - `project.close`, `project.open`, `project.create`, `project.openSample` and `editor.quit` defer while saves are in
    flight (`force: true` discards unsaved changes, never a save in flight) and answer once they are published.
  - `EditorLayer::RequestDiscardChanges` (the gate of every UI action that replaces the scene, the window's close button
    included) shows a modal through `EditorPanelContext::Shell` while saves run ("Saving 'World1M'... 63%", the label of
    each request; Cancel closes the dialog and drops the pending action, never the save) and runs the action once they
    finish.
  - `EditorContext::ReleaseProject` and the editor's teardown call `DocumentSaver::FinishAll()`. Through commands and the
    UI they run only once saves finished (above), so it blocks only in the destructor, after the last frame.
  - A status pill "Saving 'World1M' 45%" (widget kit, theme tokens) while saves run; clicking it opens the Activity
    panel. A failed save posts a notification (`EditorNotifications::Post`, level Error, "Show Problems").
  - `editor.status` gains `saving: [{saveId, label, documents, state, progress}]` (empty when idle).
  - `GameExport` stays in its `WaitingForImports` state while saves are in flight, and plans once they are published.
- **UI**: Ctrl+S, File > Save and the unsaved-changes dialog's Save run `scene.save` through the `EditorCommandRunner`
  (like Build Scripts), not `RunEditorCommand`, which rejects pending results; the dialog continues its pending action in
  the command's completion.
- **Frame profiler stage `Editor.Commands`**: `ST_PROFILE_STAGE("Editor.Commands")` around
  `EditorCommandRegistry::Execute` and `PollCommandResult` (nested calls count as self time), so `editor.perf` names
  command work instead of "untimed". No edit of `Core/FrameProfiler.*` (gpu-driven-rendering owns it): stages register
  dynamically.
- **Tests and harnesses**: `HarnessEditor::Run` and the other test harnesses' `Run` helpers complete pending results
  (`CompleteCommand`), so the ~40 existing `harness.Run("scene.save")`/`("scene.saveAs")` call sites keep working;
  command scripts (`SmokeCommands`, `TetrisCommands`, `FeatureCommands`, `ScriptsEndToEnd`) already run commands through
  the runner, which holds the script until a deferred command completes.

### 2) Asset registration at scale

The file work of registration moves to job threads in batches; the main thread only inserts prepared entries, in slices
of about 1 ms.

- **`Asset/ImportRecordStore.{h,cpp}`** (AssetPipeline; new): one append-only records index per project,
  `<cache>/ImportRecords.log`, instead of one atomic file per asset.
  - A header line (`{"Format":"ImportRecords","Version":1}`), then one entry per line: `<CRC-32C of the JSON text, 8
    hex digits> <compact JSON>` with `{"Handle", "Record": {...}}` (today's import or native record, unchanged
    contents), `{"Handle", "Path"}` (what `AssetIndex.json` held: the path a handle belongs to, for duplicated `.meta`
    files) or `{"Handle", "Removed": true}`. The latest entry of a handle wins.
  - **Crash-safe**: appends are whole lines from one writer (an I/O-thread queue; the main thread only enqueues), flushed
    per batch. A reader skips lines whose checksum or JSON is bad (a torn last line after a crash, or corruption): their
    assets are simply imported or searched again, never wrong. Cache files are renamed into place **before** their record
    is appended, so no record ever names a missing cache file (and `LoadCurrentImport` still checks).
  - Loaded once per scan into memory on an I/O thread (`LoadCurrentImport` and `FindNativeReferences` read it there
    instead of a file per asset). Compaction (dead entries more than live ones and above 1 MiB) rewrites the file
    atomically on an I/O job from the in-memory map.
  - Migration: a handle without an entry whose `<handle>.import` file exists is read once, appended, and the old file is
    removed after the store committed it, so an upgraded project neither re-imports its textures nor keeps two sources of
    truth. `AssetIndex.json` is read once the same way. `c_ImportRecordVersion` stays.
- **Registration pipeline** (`Asset/AssetRegistration.{h,cpp}`, AssetPipeline; new, used by `EditorAssetManager`):
  - Prepare jobs (batches of 256 files; background threads during a scan, I/O threads for additions) stat each file
    once, read its `.meta`, pick the importer and settings, check the cached import against the in-memory record, and for
    native documents take the recorded references or scan them (the streamed reference scan of item 3), producing
    `PreparedSource`s. Handles taken twice are resolved like today (the persisted path wins, else the shortest path) once
    a scan's batches are in; files that need a new handle or a missing `.meta` get it written by the job.
  - `EditorAssetManager::Update` inserts prepared sources into `m_Sources`, the path and folder indexes and the base's
    entries (a new batched `AssetManagerBase::RegisterAssets(std::span<const AssetMetadata>)`, one lock per slice) until
    about 1 ms has passed (`AssetRegistrationOptions::SliceMicroseconds`, checked every 64 entries), applies their current
    imports and references, and queues the imports of the rest. No file is touched on the main thread.
  - `RegisterAddedFile` (copies of `ImportExternal`, restores from the trash, file-watcher additions) goes the same way:
    the copy job prepares what it copied, the main thread inserts it. `SaveAssetIndex` disappears (paths go into the
    records store as they change). Applied imports no longer write `.meta` files or indexes on the main thread (roadmap
    open item).
- **Scans in the background with progress**: `EditorAssetManager::BeginScan(ScanImports)` returns at once (the directory
  walk is a job too); `IsScanning()`, `GetScanProgress() -> {Phase: Walking|Registering|Applying, Done, Total}`;
  `Scan(ScanImports)` stays (Begin plus wait: tools, tests and scripted runs, which therefore stay deterministic).
  - `EditorContext::BeginOpenProject` begins the scan and returns; the project's start scene opens once the scan ends.
  - The editor's interactive and headless-automation runs scan in the background; `--commands`/`--frames` runs and tests
    keep the blocking scan (`EditorContextSpecification`), as today for imports.
  - While the scan runs, `EditorCommandRegistry::Execute` defers commands that need the whole registry: kind `Edit`, and
    queries that set the new `EditorCommand::WaitsForAssets` (asset, scene, prefab, material and project queries that
    answer from the registry). `editor.status`, `editor.wait`, `editor.perf`, `log.*` and other session queries answer at
    once. `RunEditorCommand` (UI) reports a deferred command as refused with "the project's assets are still being
    scanned (12,345 of 30,000)".
  - The deferral happens in the registry before the command's handler runs. authoring-and-playtest's `MayDefer` flag
    (merging last) describes handlers that defer by themselves, so this deferral never trips its Debug assert, and its
    `editor.batch` (an `Edit` command) is deferred whole during a scan (common.md section 8).
  - `editor.status` `assets.scanning`: `{done, total, phase}` while scanning, null otherwise (authoring-and-playtest's
    compact `editor.status` must keep it). The assets status pill leads with "Scanning 12,345 / 30,000"; the Content
    Browser shows the same line above its listing.
- **Session before the scan ends**: `EditorLayer::OnAttach` starts automation right after `BeginOpenProject` (which no
  longer blocks), so the session file appears within the editor's startup, not after the scan. The automation session
  "moves along" with the project as it does today.
- **`StrataCLI launch` and the MCP server wait for the session and its progress, not a fixed 60 s**
  (`CLI/EditorLauncher.*`, function-level in `McpServer.cpp`):
  - The session wait keeps a timeout only for the session file to appear (`--timeout`, 60 s, while the process lives).
  - Then `launch` connects and polls `editor.status` every 250 ms while `assets.scanning` is non-null, printing progress to
    stderr; it fails only when the editor exits or the scan's `done` stalls for `--stall-timeout` (60 s). It returns once
    the scan ended, so an agent gets a ready editor; `--no-wait-scan` returns at the session.
  - `strata_launch_editor` does the same and sends MCP `notifications/progress` when the client gave a `progressToken`.
- **Reference searches never block exports**: `HasPendingImports()` counts imports only; `HasPendingReferenceSearches()`
  is separate and `editor.status` `assets.referenceSearches` reports it. `QueueNativeReferences` cancels the source's
  previous search before it queues the next and starts it only once the file has not changed for 500 ms (debounce, so a
  file written over seconds is searched once). `GameExport` waits for imports and saves only; for a planned document whose
  search is still pending it scans that document itself on its job (`AssetDependencies::Scan`, streamed). Test: the
  export no longer sits in `WaitingForImports`.
- **Incremental residency info**: `AssetManagerBase` keeps the set of listed entries (resident, loading, failed or
  pinned) with every state change (`SetStateLocked`, `AddEntryLocked`, `RemoveEntryLocked`, `Pin`/`Unpin`, publication
  and release of loaded objects), so `GetResidencyInfo` and `GetResidencySamples` cost what is listed, not what is
  registered. `StateCountsMatch` (`AssetTestUtils.h`) keeps holding and also checks the listed set against a brute-force
  filter.

### 3) Streamed JSON reads (and prefabs the same way)

- **`Scene/SceneDocumentReader.{h,cpp}`** (new): `nlohmann::json::sax_parse` with a handler that builds one entity's
  object at a time (a small DOM of that entity only) and hands it to an `EntitySink` (`Scene/EntitySink.h`):
  - `SceneBuilder` creates the entity and its components in the target scene's registry as it goes (what
    `DeserializeEntities` pass 2 does today, `ComponentAccess::Deserialize` per component), keeps only IDs and parent IDs
    (16 bytes per entity), and links the hierarchy, remaps references and signals components at the end with today's
    rules (cycles and missing parents attached at the top level with the same warnings, unknown components kept with one
    warning per name).
  - `EntityPrototypeBuilder` builds an `EntityPrototype` directly (component value arrays per type), without the
    temporary scene `EntityPrototype::FromSnapshot` makes today: prefabs, models and additive loads.
  - `ReferenceSink` only collects asset references: `AssetDependencies::Scan` of scenes, prefabs and models uses it
    (bounded memory; the first scan's reference search of large scenes, roadmap open item).
- Accepts any key order and whitespace (old tab-indented files, compact files, the new layout). The header may come
  after the entities (old files): it is validated when it arrives and the load fails if it is wrong or missing.
- **Bounded transient memory**: the file's bytes plus one entity's DOM plus the per-entity tables; never a whole-document
  DOM.
- **Cancellation between entities**: the handler returns false (which ends `sax_parse`) once the read is cancelled.
  `AssetLoadData::IsCancelled()` (new; RunLoad sets it from `IsLoadCurrent` through the manager's shared `LoadState`, so
  it also turns true when the manager shuts down) tells loaders; `SceneAsset::Deserialize` and the prefab and model
  loaders pass it on. `SceneLoadRequest::Cancel`, `scene.open` of another scene, closing the project and quitting the
  editor or `StrataRuntime` during a load therefore end the read within milliseconds.
- **Early refusal over the cap**: from `EntityCount` when the document has it, else at the first entity beyond
  `Scene::c_MaxEntities - existing`; never after the whole parse.
- Progress: `SceneReadProgress` keeps working (bytes parsed, then linking).
- **Parallel line reader (if the 1M open still exceeds 4 s single-threaded)**: for documents in the writer's own layout
  (one entity per line, detected and validated per line), the entity lines are cut into chunks at line ends and parsed on
  the worker threads into per-chunk component columns, merged in order; any line that does not parse alone falls back to
  the SAX path for the whole document.
- **Prefab documents** go the same way: `Prefab::WriteDocument(DocumentOutput&)` writes through `SceneDocumentWriter`
  from the prototype (`SceneDocumentWriter::WritePrototype`), and the code that writes prefab files (`prefab.create`,
  `prefab.apply`, `AssetEdit`) uses it; `Prefab::Serialize`/`Model::Serialize` (DOM) stay for small uses.
  `Prefab::Deserialize`/`Model::Deserialize` read through `EntityPrototypeBuilder`.
- **Prepared form for additive loads (offered to world-partition)**: `SceneLoadOptions::PrepareAs`
  (`PreparedForm::Scene`, default, or `Prototype`) and `SceneLoadRequest::TakePrototype()`: a cell or sub-scene load gets
  its prototype straight from the reader, without the prepared scene that `SceneSubScenes` compiles today (roadmap: a
  sub-scene holds both until the prototype is made). world-partition decides whether its cell loads use it.

### 4) Binary cooked scenes for packs

- **`Reflection/PropertyBinary.{h,cpp}`** (new, beside `PropertyJson`): `WritePropertyValue(BinaryWriter&,
  PropertyType, const PropertyValue&)` and `ReadPropertyValue(BinaryReader&, PropertyType, std::string* outError) ->
  std::optional<PropertyValue>` for every `PropertyType` (a `static_assert` on the enum's count fails the build when a
  type is added without a binary form). Bool `u8` (0 or 1), Int `i32`, UInt `u32`, Float `f32` (non-finite refused),
  Vec2/3/4 and Color3/4 `f32` arrays, Quat `f32 x, y, z, w`, String `u32` length plus UTF-8 bytes (bounded by
  `c_MaxBinaryStringBytes`, 1 MiB), Enum `i32`, Asset and Entity `u64`. Values go through `PropertyInfo::SetValue` on load,
  which validates ranges and enum options as JSON loads do.
- **Component hooks** (`Reflection/ComponentRegistry.h`, function-level): `ComponentInfo::SerializeExtraBinary` /
  `DeserializeExtraBinary`, set with `ComponentInfoBuilder::ExtraBinary(...)` by the module that registers the
  component. The Script component's registration provides them (scripts, classes and typed field overrides), in one
  block that engine-modules' component move carries into `ScriptingComponents.cpp`.
  - **References in extra data are engine-modules' hooks, not this stream's.** engine-modules (merging first) replaces
    today's `ScriptComponent` special cases (`EntityPrototype::Compile`, `RemapEntityReferences`, `SceneInstantiation`,
    `Prefab`'s comparison, `AssetDependencies`) with `ComponentInfo::ExtraReferences` (`Visit`, `Set`, `Equals`,
    `VisitJson`). This stream's new code never names `ScriptComponent`: until that merge it computes references through
    the existing generic paths (the prototype's reference list as `EntityPrototype::Compile` builds it,
    `RemapEntityReferences`, `AssetDependencies`' per-entity scan), and after its rebase through `ExtraReferences`.
  - **Components with JSON extras and no binary hooks** (`SerializeExtra` only, such as authoring-and-playtest's prefab
    override list on `PrefabInstanceComponent`) keep their extra JSON in the cooked form as a per-value MessagePack blob
    (bounded and validated like unknown components), so a JSON-only extra round-trips without code of its own.
- **Format** (`Scene/CookedScene.{h,cpp}`: `CookedSceneFormat` structs, `CookedSceneWriter`, `CookedSceneReader`),
  little endian, "STSC", version 1:
  - header (64 bytes): magic, version, flags (none defined; unknown flags refused), header size, kind (`Scene`, `Prefab`,
    `Model`), entity count, component type count, string count, string bytes, section table offset and count, a CRC-32C of
    the section table and one of the header;
  - section table: `{kind, index, offset, size, count, CRC-32C}` per section; every section 16-byte aligned, inside the
    file, not overlapping;
  - strings (offsets, sizes, bytes); the scene's name and settings as compact JSON text (small, and validated by the
    existing settings reader); entity IDs (`u64` each) and parent indices (`u32`, `0xFFFFFFFF` for top level, each less
    than its own index: parents first);
  - component types: name and, per property, name, `PropertyType` and flags, so a reader of another build maps columns by
    name (an unknown property is skipped with a warning, a missing one keeps its default, as JSON does);
  - one column per component type (the shape of `EntityPrototype`'s `ComponentValueArray`): a dense flag (every entity
    has it: no index array) or strictly increasing entity indices, then each property's values together (structure of
    arrays: decodes fast and compresses well in packs), then per-value extra blobs for types with binary extras;
  - unknown components: `{entity index, size, MessagePack bytes}` of their JSON (`nlohmann::json::from_msgpack` with
    exceptions off), preserved;
  - entity references are **recomputed** from the values on load (as `EntityPrototype::Compile` does), not stored:
    a stored table would be a second source of truth to validate (see questions to the planner).
- **Loading**: `CookedSceneReader::ReadScene(bytes, Scene&, options)` creates the entities, then decodes the columns
  into their storages in parallel (one storage per job), then links the hierarchy; `ReadPrototype` fills an
  `EntityPrototypeBuilder` the same way. No DOM, transient memory about the cooked bytes. Progress per decoded section
  (`SceneReadProgress`); cancellation between sections and every 4,096 values.
- **Loaders recognize it by its header** (`Scene/SceneRegistration.cpp`, function-level): the Scene, Prefab and Model
  loaders take `CookedScene::IsCooked(bytes)` documents to the binary reader and everything else to the JSON reader. So
  `RuntimeAssetManager` (and the editor, which only ever has JSON in the project) loads both. JSON stays the editor's
  document format: the editor never writes cooked scenes into projects.
- **Cooking at export**: `Asset/AssetPackCook.{h,cpp}` (AssetPipeline; new) `CookForPack(const AssetMetadata&,
  std::vector<uint8_t>& data, std::string* outError)`: scenes, prefabs and models are read with the JSON reader and
  written with `CookedSceneWriter`. `EditorAssetManager::WriteAssetPack`'s data provider calls it in place of its
  reference check's separate parse: the one streamed read yields both the references the check compares with the plan and
  the cooked bytes (`AssetPackWriteOptions::CookDocuments`, true for exports; tests compare both). Its plan and dependency
  tables are unchanged. World cells are scenes, so they are cooked without anything of world-partition's.
  - **The export's scene transform** (offered to world-partition): `CookForPack` takes `const AssetPackCookOptions&` with
    `std::function<void(AssetHandle scene, SceneSettings&)> AdjustSceneSettings` (null: none), applied to a scene's
    settings between the read and the write, for JSON and cooked output alike. world-partition's export raises a
    world's persistent scene's physics capacity through it. Frozen with the cook's first commit.
- **Full validation**: truncated and garbage data are rejected; every count and index is bounded before anything is
  allocated (counts against section sizes, entity count against `Scene::c_MaxEntities`, string sizes, entity indices,
  parent indices, duplicate or null IDs, property types, MessagePack sizes); section checksums are checked before
  decoding. Corruption tests like `AssetPackTests.cpp`.

### 5) Import memory admission

- **A budget in `BackgroundWork`**: `EditorAssetManager::SetImportMemoryBudget(uint64_t bytes)`; default 25% of physical
  memory clamped to 1-4 GiB (`Platform::GetPhysicalMemorySize()`, new in `Core/Platform.h` with Windows and POSIX
  implementations). The upper bound makes the default bind on large machines too: 24 4096x4096 textures estimate about
  8 GB, which 25% of a 32 or 64 GB machine would admit at once.
- **Estimates from the source header**: `AssetImporter::EstimateImportMemory(std::span<const uint8_t> head, uint64_t
  sourceSize, const nlohmann::json& settings) const` (new virtual; default 4 times the source size).
  `TextureImporter` reads the header (`stbi_info_from_memory`) and returns width x height x (4 + 16); `GltfImporter`
  sums its buffers and images; native documents return their size.
- **Admission without blocked threads**: an import job reads the head (`c_AssetPrefixHeadBytes`), computes its estimate
  and asks `BackgroundWork::Admit`: it runs while the sum of admitted estimates fits the budget, and the largest runs alone
  when it exceeds the budget by itself; otherwise the request goes back into the work's ordered queue with its estimate and
  the thread is freed. Finishing (or cancelling) an import releases its estimate and submits what now fits.
  `RunImport`'s `s_LargeImportMutex` goes.
- **Reporting**: `asset.stats` and `editor.status` `assets.importMemoryMB {admitted, budget, queued}`;
  `asset.setBudget {importMB}` changes the budget until the project closes (function-level in
  `EditorStreamingCommands.cpp`).

### 6) If time (otherwise wave 5)

- **Pack version 3** (`AssetPack*`, `AssetPackWriter`, `RuntimeAssetManager`):
  - entries under 64 KiB bundled into shared 256 KiB blocks, ordered by first use; `EntryRecord` version 3 gains an
    offset within its first block;
  - a per-pack zstd dictionary trained at export (`ZDICT_trainFromBuffer`: zstd's `lib/dictBuilder` sources added only
    to the tooling-only `zstd_compress` target) and used for bundled blocks (`ZSTD_DDict` in the reader);
  - 4 KiB alignment only for entries read by range (types with a prefix function, `AssetLoaderRegistry::FindPrefix`);
  - a reader that decodes a shared block once, with a small cache (at most 4 MiB, reported in `AssetPackStats::BufferBytes`
    and `asset.stats`);
  - version 2 (and 1) still load; corruption tests for bundled entries (ranges outside their block, overlaps, a
    dictionary out of range, a bundle block's checksum).
- **Cold I/O and read depth**: `FILE_FLAG_NO_BUFFERING` behind `STRATA_COLD_IO` in `WindowsFileReader` (aligned bounce
  buffers; POSIX keeps `pread`), and at least 16 outstanding pack reads (overlapped reads issued ahead in
  `AssetPack::ReadBlocks`), measured as `PerfGPU.Pack.ColdStreamStressWorld`.
- **A cap on active background threads while a scene or cell load runs** (`JobSystem::SetBackgroundConcurrencyLimit`,
  function-level): the audit's contention runs (`contend_*.log`) opened the 250k world in 2.2 s alone and 3.5-4.6 s
  during the 24-texture import, with frames up to 211 ms.

## Deliver in this order if time runs short

0. `WorldFixture.h` (API frozen in the first commit) and the `Editor.Commands` stage.
1. Saves off the frame, with the streamed writer, `DocumentSaver` (multi-document API frozen early for world-partition),
   deferred `scene.save`, close and quit paths, the recovery carry-over. **Critical.**
2. Asset registration at scale: the records store, the registration pipeline, background scans with progress, the
   session before the scan, launch and MCP waiting on progress, reference searches out of `HasPendingImports`,
   incremental residency info. **Critical.**
3. Streamed reads with cancellation and early cap refusal; prefabs; `ReferenceSink`; the prototype form for additive
   loads. **Critical** (the 4 GB loads); the parallel line reader only if the 1M open exceeds 4 s.
4. Binary cooked scenes in packs. **Critical** for the runtime's 1M start (8.5 s and 3.96 GB today).
5. Import memory admission. Major.
6. Pack v3, cold I/O and read depth, the background-thread cap: if time, else wave 5 with the reasons in the report.

Docs, skills, tests and screenshots land with each item, not at the end.

## Acceptance criteria

Tests that fail on the base (confirm each, by running it against the step-0 base build or with the work reverted):

- **`Editor.Saves`** (new, `StrataTests/src/Editor/DocumentSaveTests.cpp`):
  - a save's frame does no serialization (a scene of 50,000 entities: the frame that starts `scene.save {wait:false}`
    serializes nothing, by a counter of `SceneDocumentWriter` calls on the main thread);
  - **edits made during a save stay dirty**: edit, save without waiting, edit again, wait: `IsSceneModified` is true and
    undoing the second edit makes it false; a continuous edit begun before the save does not merge into the saved step;
  - **quitting with a save in flight loses nothing**: `editor.quit` (also with `force: true`) answers only after the
    file is written; the window close shows the saving dialog; the file on disk then loads equal to the snapshot;
  - closing the project, opening another, and `scene.open`/`scene.new` during a save: the file is written, nothing is
    marked saved in the wrong scene, `CheckUnsavedChanges` defers instead of refusing;
  - a second save queues behind the first; a queued save of the same scene is replaced by a newer one; both completions
    fire;
  - **multi-document requests**: three entity subsets of one snapshot written in one request, one completion, each file
    loads to exactly its subset; one item failing (a read-only target) fails the request naming it and leaves the others
    written; a `Document` item writes its bytes, and a new asset reserved with a given handle registers with that handle;
  - `SaveSceneAs` during a failed save restores the previous handle and name;
  - the file watcher does not search or reload a document its own save wrote.
- **`Editor.Recovery`** (extend `RecoveryTests.cpp`): edits made during a background save are in the journal after the
  save published (a simulated crash after it recovers them); `OnSaved` keeps no copy of the document.
- **`Scene.Documents`** (new, `StrataTests/src/Scene/SceneDocumentTests.cpp`):
  - every line of the new layout's entity list equals `JsonUtils::Dump` of today's per-entity object, for
    `Feature.stscene` and a generated world; the whole document parses to today's `Serialize` DOM plus `EntityCount`;
  - old files (tab-indented, compact, header after the entities, children before parents) load to the same scene as
    today's reader (compare with `Tests::SceneComparison` or serialized DOMs);
  - entity subsets: roots with descendants, nested roots, parents outside the subset dropped;
  - references collected by the writer equal `AssetDependencies::Scan` of the bytes;
  - warnings for unknown properties and components equal today's;
  - an over-cap document is refused before 1% of its entities are read when it has `EntityCount`, and at the first
    entity beyond the cap without it. The suite runs in Debug: it passes the reader a lowered entity limit (a reader
    option standing in for `Scene::c_MaxEntities - existing`) with a small document; the real 1,100,445-entity document
    is the Release metric `Perf.Scene.RefuseOverCap1M` (common.md section 10, world-scale fixtures);
  - **cancellation**: a read of a generated document cancelled at 10%, 50% and 90% stops within one chunk of entities
    after the cancel (a deterministic count of entities read after it) and creates no scene; `SceneLoadRequest::Cancel`
    during `LoadingScene` ends the loader job; a manager destroyed during a load ends it too (the time limits are the
    perf gate `Perf.Scene.CancelLoad1M`, not this test, which also runs in Debug);
  - `Prefab`/`Model` documents round trip through the streamed reader and writer, and `EntityPrototypeBuilder` equals
    `EntityPrototype::Compile` of the same document read into a scene.
- **`Reflection.PropertyBinary`** (new): a round trip of every `PropertyType` (extremes, empty and maximal strings,
  enum values), rejection of truncated values, non-finite floats, oversized strings and booleans other than 0 and 1.
- **`Scene.CookedScene`** (new): **`Feature.stscene` through the binary form equals its JSON load** (every component,
  every property, script fields of every type, unknown components, a test component with JSON-only extras, settings,
  names, IDs, hierarchy and sibling order); `AdjustSceneSettings` changes the cooked and the JSON output alike;
  prefabs and models the same; the cooked bytes are deterministic (two cooks are byte-identical) and independent of the
  thread count; **corruption**: truncation at every byte of a small cooked scene, every single-bit flip of its header and
  section table, counts and offsets set to huge values with the checksums rewritten so validation is reached
  (`RewriteCookedChecksums` test helper), overlapping or unaligned sections, unsorted or out-of-range entity indices,
  parent indices not below their entity, duplicate or null IDs, property type mismatches, oversized MessagePack: each
  rejected with an error, never a crash or an allocation beyond the file's size.
- **`Asset.Pack`** (extend `AssetPackTests.cpp`): an exported pack holds cooked scenes and prefabs; `RuntimeAssetManager`
  loads them equal to the JSON sources; a pack written without cooking still loads (JSON in packs stays readable).
- **`Asset.ImportRecords`** (new, `ImportRecordStoreTests.cpp`): append, reopen, latest wins, removal, compaction; **the
  records index survives a crash**: the file truncated at every byte offset of its last three entries reopens with every
  complete entry and none of the torn one; a helper process (`--strata-test-helper=append-import-records`) killed while
  appending five times leaves a store that opens and whose every record names cache files that exist; migration of
  `.import` files and `AssetIndex.json` (no re-import of an already imported texture project after the upgrade).
- **`Asset.Registration`** (new, `AssetRegistrationTests.cpp`): **registration batches**: 10,000 prepared sources are
  inserted in slices that each stay within the slice budget (by a fake clock, deterministic); no file is touched on the
  main thread during `Update` while a 600-file folder registers (a thread-local count of `FileSystem` and
  `AtomicFileWriter` operations, new: `FileSystem::GetThreadOperationCount()`); duplicated `.meta` handles resolve as
  today; `StateCountsMatch` holds after every slice; a file added during a scan registers once; `Close` during a scan
  cancels its jobs without waiting.
- **`Asset.Residency`** (extend): the listed set equals the brute-force filter after loads, evictions, pins and
  unregistrations; `GetResidencySamples` with 30,000 registered and 100 listed assets visits 100 entries (a counter).
- **`Asset.ImportAdmission`** (new): with a budget of 4 units and imports estimating 2 units each, at most two run at once
  (a test importer counts); one estimating 6 units runs alone; cancelled and failed imports release their estimates; no
  background thread waits for admission (`JobSystem` stats: queued imports hold no thread).
- **`Editor.Export`** (extend `EditorExportTests.cpp`): **the export no longer sits in `WaitingForImports`**: a large
  scene rewritten on disk ten times during an export does not delay planning beyond its imports, and only one reference
  search per document runs at a time.
- **`CLI.Commands` / `CLI.Mcp`** (extend; `STRATA_TEST_FAKE_EDITOR=1`): **launch and MCP wait on progress**: a fake
  editor that publishes its session at once and reports `assets.scanning` advancing for 90 s is waited for (no 60 s
  timeout); one whose progress stalls for the stall timeout fails with a message naming the stall; `--no-wait-scan`
  returns at the session; `strata_launch_editor` sends progress notifications.
- **`Editor.Automation`/`EndToEnd`**: the real editor on a fresh copy of a 10,000-asset project publishes its session and
  answers `editor.status` with `assets.scanning` before the scan ends.
- **Feature test**: all runners pass; the exported-game run of `StrataTests.FeatureTest` (suite `Editor.FeatureTest`)
  checks that the pack's scene and prefab entries are cooked and plays them, and `StrataRuntime.FeatureTest` passes
  reading them.
- **Layering**: `Architecture.Layering` passes with the new AssetPipeline files listed in `Layers.json` (AssetPipeline
  `Paths`, Asset `Exclude`), no new allowlist line.

Perf (Release; confirmed checks; `Budgets.json` entries set from locked interleaved A/Bs against the step-0 base, "to
confirm in the integration campaign"; the acceptance limit is the budget where it lies below 1.5 times the measurement):

| Metric | How it is measured | Basis |
| --- | --- | --- |
| `Perf.Editor.SaveMaxFrame250k` (ms) | in-process `HarnessEditor` (no GPU) on the realistic 250k world: open (blocking), one edit, `scene.save {wait:false}` issued through the runner so that its snapshot runs inside a measured frame, frames until published; the longest frame from the save's start to its publication. Maximum by nature: the save's hitch | acceptance 16 (today 4,799) |
| `Perf.Editor.SaveMaxFrame1M` (ms) | the same on the realistic 1M world | acceptance 60 (today about 22,000) |
| `Perf.Editor.SaveSeconds1M` (ms), `Perf.Editor.SavePeakPrivateIncrease1M` (MB) | the same run: the save's total time; its peak private memory over the private memory before it (a maximum by nature), in a helper process (`--strata-test-helper=save-world`) | budgets 1.5x measured; targets: total under 3,000 ms, increase under 600 MB, each entered only while below its budget (today 8.76 GB peak) |
| `PerfGPU.Editor.WorldOpen1MPeakPrivate` (MB) | real `StrataEditor` on a fresh project with the realistic 1M world: `scene.open`, then `editor.status` `memory.peakPrivateMB`. Maximum by nature | acceptance 2,048 (today 3,950) |
| `PerfGPU.Editor.WorldOpen1MMaxFrame` (ms) and `WorldOpen1MSeconds` (ms) | the same run's `scene.open` result; the max frame is a maximum by nature, the seconds one run's time (one open per attempt) | max frame: 1.5 x measured, and not above `SceneOpen1MMaxFrame`'s budget; seconds: 1.5 x measured, target 4,000 (today 8,200) |
| `Perf.Scene.CancelLoad1M` (ms) | `SceneLoader` load of the realistic 1M JSON world cancelled at 25%, 50% and 75% of its read: time until the loader job ended; the maximum of the three | acceptance 100 |
| `Perf.Scene.RefuseOverCap1M` (ms) | `SceneLoader` load of a 1,100,445-entity JSON document written with `EntityCount`: time from the load's start to its failure; the median of 3. The same document without `EntityCount` must fail too (checked, its time reported) | acceptance 100 (today about 7,000) |
| `Perf.Runtime.World1MStart` (ms) | `StrataRuntime --headless --perf-log` on the exported realistic 1M world (cooked scene): from the game opening to its start scene playing (`GameRuntime` logs "Started '...' in N s"; the test reads it). No GPU needed | acceptance 1,500 (today 8,500) |
| `Perf.Runtime.World1MTransientMemory` (MB) | the same run's perf log: peak private during the load minus private 60 frames after the start | acceptance 300 (today about 3,200) |
| `Perf.Runtime.QuitDuringLoad1M` (ms) | `StrataRuntime --headless --frames 2` on the exported 1M world, once with cooked and once with JSON scenes in its pack (`WriteWorldGame(..., cookDocuments)`): from its last frame to the process's exit; the larger of the two | 1.5 x measured; target 500 (today 8,000) |
| `Perf.Asset.FirstScan10k` (ms) | `EditorAssetManager::Scan` (blocking) on a fresh copy of the 10,000-material project | acceptance 5,000 (today 26,400) |
| `Perf.Asset.WarmScan10k` (ms) | the second scan of the same copy | 1.5 x measured; target 1,000 (30k warm: 13.3 s today) |
| `PerfGPU.Editor.Launch30kStatus` (ms) | `StrataCLI launch --headless --no-wait-scan` on a fresh copy of the 30,000-material project, then `StrataCLI call editor.status`: from the launch to the answer | acceptance 5,000; the full `launch` (waiting for the scan) must succeed and its time is reported |
| `Perf.Editor.ImportFolder600MaxFrame` (ms) | in-process harness: `asset.import {directories}` of 600 64x64 PNGs, `wait: false`; the longest frame until every import is applied. Maximum by nature | acceptance 16 (today 3,605) |
| `Perf.Editor.ImportFolder24x4kPeakPrivate` (MB) | helper process: the project scanned, then `ImportExternal` of 24 4096x4096 PNGs with the admission budget set to 1,024 MiB (the default, 25% of physical memory, would not bind on a large machine); peak private over the private before the import. Maximum by nature | acceptance 1,280: the admission budget plus 256 MB for cache writes and the editor's own work (today about 3,000) |
| `Perf.Asset.Pack30kMaterialsRatio` (ratio) | only if pack v3 lands: pack size of the 30,000-material project over its stored bytes | acceptance 1.5 (today 10.7) |

No regression (interleaved A/Bs, ratio of medians within noise) in `PerfGPU.Editor.{Edit,Play}FrameNested*` (the idle
frames: the saving and scanning pills cost nothing while idle, measured with `editor.perf` before and after),
`PerfGPU.Editor.SceneOpen1M*`, `Perf.Runtime.SceneSwitch100kMaxFrame`, `PerfGPU.Pack.*`, `PerfGPU.Streaming.*`,
`Perf.EditorPanels.Streaming10kAssets*` and `Perf.Scene.InstantiatePrefab*` (prototypes now come from the builder).

Everything else:

- **Docs**: `Docs/Architecture.md` "Asset pipeline" (registration pipeline, records store, background scans, admission,
  reference searches), "Streaming and residency" (load cancellation, listed entries, cooked scenes, pack v3 if it lands),
  "Scene runtime lifecycle" (a new "Scene documents" subsection: the streamed layout, the reader and its sinks, the cooked
  format with its validation), "Editor" (background saves, `DocumentSaver`, scanning), "Export and the runtime" (cooking).
  `AGENTS.md` rules only: saves are deferred and never serialize on the main thread (`DocumentSaver`); documents stream
  (never build a whole-scene DOM; use `SceneDocumentWriter`/`Reader`); registration touches no file on the main thread;
  loaders of large documents check `AssetLoadData::IsCancelled`; every format change of cooked scenes extends
  `CookedSceneTests.cpp`. Remove the closed items from Roadmap's "Open items" (not its Status).
- **Skills**: `strata-editor-automation` (`scene.save` is deferred and takes `wait`; `editor.status` `assets.scanning`
  and `saving`; launch waits for the scan, `--no-wait-scan`), `strata-make-a-game` (saving a large scene),
  `strata-build-test` (the new perf metrics and fixtures).
- **Screenshots** (Release editor; 150%, `--ui-scale 1`, and `--windowed` 1600x900; private `STRATA_SESSION_DIR`,
  `STRATA_RECENT_PROJECTS`, `--layout`; under `scratchpad\wave4\world-io\shots`), each looked at and fixed when it looks
  unfinished:
  - the "Saving 'World1M' NN%" pill while the realistic 1M world saves;
  - the saving dialog after closing the window with a save in flight;
  - the assets pill and the Content Browser's scanning line during the first scan of a fresh 30,000-material copy;
  - the error notification of a failed save (a read-only scene file);
  - the editor after the 1M world opened (no change expected; proves the reader).

## Open items this stream owns

Scale audit: **C** `scene.save` freezes the main thread; **C** scenes load and save as one JSON document on one thread
(streamed and binary I/O; the per-registry cap of 1,048,575 live entities stays, world-partition keeps what is loaded
below it); **C** registration at 3-10 ms per asset and the launch timeout; M unbounded import memory; M pack v2's
small entries (item 6, else wave 5); m the `Editor.Commands` stage; M (part) realistic world fixtures, and cold I/O and
read depth (item 6). Rendering audit side notes: the export stuck in `WaitingForImports`; the 2.95 GB peak of a 1M
JSON scene. Roadmap open items: the `.meta` and index writes of applied imports, the reference search of large scenes in
first scans, residency info walking every registered asset.

Out of scope: 64-bit entity handles; world partition itself; mesh and audio streaming (wave 5); background saves of
prefab edits (`prefab.apply`, `AssetEdit` documents stay synchronous; a 100,000-entity prefab still writes in a frame:
reported as an open item); the editor writing cooked scenes into projects.

## Files expected to be touched

- New engine files: `Strata/src/Strata/Scene/SceneDocumentWriter.*`, `SceneDocumentReader.*`, `EntitySink.h`,
  `CookedScene.*`; `Strata/src/Strata/Reflection/PropertyBinary.*`; `Strata/src/Strata/Core/AtomicFileWriter.*`;
  `Strata/src/Strata/Asset/ImportRecordStore.*`, `AssetRegistration.*`, `AssetPackCook.*` (AssetPipeline).
- Changed engine files: `Scene/SceneSerializer.*`, `SceneLoader.*`, `SceneReadProgress.*`, `EntityPrototype.*`,
  `Prefab.*`, `SceneRegistration.cpp`, `Scene.*` (the `SceneCopyOptions` overload, function-level),
  `ComponentRegistration.cpp` (the Script component's binary hooks; one block, movable by engine-modules);
  `Reflection/ComponentRegistry.h` (binary extras only; references are engine-modules' `ExtraReferences`);
  `Asset/AssetPackCook.*` (`AssetPackCookOptions`);
  `Asset/EditorAssetManager.*`, `AssetManager.*` and `AssetResidency.cpp` (listed entries, `RegisterAssets`,
  `AssetLoadData::IsCancelled`), `AssetDependencies.*`, `AssetImporter.*` (`EstimateImportMemory`),
  `TextureImporter.*` and `GltfImporter.*` (their estimates only), `AssetPack*`, `AssetPackWriter.*`,
  `RuntimeAssetManager.*` (item 6), `Platform/Windows/WindowsFileReader.cpp` (item 6), `Core/Platform.h`,
  `Platform/Windows/WindowsPlatform.cpp`, `Platform/Posix/PosixPlatform.cpp` (`GetPhysicalMemorySize`),
  `Core/JobSystem.*` (item 6's cap, function-level), `Runtime/GameRuntime.cpp` (the "Started in N s" line only).
- Editor: `StrataEditor/src/Editor/DocumentSaver.*` (new), `EditorContext.*` (saves, close and quit paths, background
  scan and the pending start scene), `EditorSceneCommands.cpp` (`scene.save`, `scene.saveAs`), `CommandUtils.*`
  (`CheckUnsavedChanges`), `EditorCommands.*` (`WaitsForAssets`, the deferral during scans, the stage),
  `EditorCommandRunner.cpp` (the stage), `EditorStateCommands.cpp` (`editor.quit` deferral, function-level),
  `EditorStreamingCommands.cpp` (`asset.setBudget {importMB}`, `asset.stats` import memory, function-level),
  `GameExport.*` (waits for saves, scans pending documents, cooking on), `SceneRecovery.*` (`OnSaved`), `UndoStack.*`
  (`SavePoint`), `EditorLayer.*` (automation before the scan, Ctrl+S through the runner, `RequestDiscardChanges` and the
  saving dialog, the saving and scanning pills), `Panels/ContentBrowserPanel.cpp` (the scanning line).
- CLI: `StrataCLI/src/CLI/EditorLauncher.*`, `CliApplication.cpp` (`--no-wait-scan`, `--stall-timeout`),
  `McpServer.cpp` (`strata_launch_editor`'s wait and progress, function-level).
- Tests: `StrataTests/src/Perf/WorldFixture.*` (new), `StrataTests/src/Perf/WorldIOPerfTests.cpp` (new: `Perf.Scene`,
  `Perf.Asset`, `Perf.Runtime` cases), `StrataTests/src/Editor/EditorWorldPerfTests.cpp` (new: `Perf.Editor` and
  `PerfGPU.Editor` world cases), `StrataTests/src/Editor/DocumentSaveTests.cpp`, `StrataTests/src/Scene/SceneDocumentTests.cpp`,
  `StrataTests/src/Scene/CookedSceneTests.cpp`, `StrataTests/src/Reflection/PropertyBinaryTests.cpp`,
  `StrataTests/src/Asset/ImportRecordStoreTests.cpp`, `AssetRegistrationTests.cpp`, `ImportAdmissionTests.cpp` (new);
  extended: `AssetPackTests.cpp`, `AssetResidencyTests.cpp`, `AssetTestUtils.h`, `EditorExportTests.cpp`,
  `RecoveryTests.cpp`, `SerializationTests.cpp`, `SceneLoaderTests.cpp`, `CLI/CliApplicationTests.cpp`,
  `CLI/McpServerTests.cpp`, `CLI/FakeEditor.h`, `Editor/HarnessEditor.h` and the other harness `Run` helpers,
  `TestMain.cpp` (helper modes), `StrataTests/Perf/Budgets.json`, `StrataTests/Architecture/Layers.json`.
- Docs and skills: `Docs/Architecture.md`, `AGENTS.md`, `Docs/Roadmap.md` (open items only),
  `.claude/skills/strata-editor-automation`, `strata-make-a-game`, `strata-build-test`.

## Merge notes and conflict risks

Merges **second**, after engine-modules and before world-partition.

- **engine-modules** (merges first):
  - Rebase onto its per-layer libraries: the new AssetPipeline files go into `Layers.json`'s AssetPipeline `Paths` and the
    Asset layer's `Exclude` (append), which then puts them into the right library with no CMake of this stream's own.
  - It moves components into their modules. This stream names no module component in Scene-layer code: script fields go
    through `ComponentInfo`'s binary hooks (this stream's `ExtraBinary`) and engine-modules' `ExtraReferences`,
    registered with the Script component wherever it lives after the move. The binary-hook registration in
    `ComponentRegistration.cpp` is one block that moves with it.
  - `ComponentRegistry.h` gains `ExtraBinary` (function-level) beside engine-modules' `ExtraReferences`; `EntityPrototype`,
    `SceneSerializer` and the new readers use `ComponentRegistry::GetAll()` generically.
  - `CreateApplication` returns an `ApplicationLaunch` after engine-modules: this stream's session publishing in
    `EditorApplication.cpp` rebases onto that shape (function-level).
  - New suites (`Scene.Documents`, `Scene.CookedScene`, `Reflection.PropertyBinary`, `Asset.ImportRecords`,
    `Asset.Registration`, `Asset.ImportAdmission`, `Editor.Saves`) must fall into its Core shards (its coverage test
    says which); new helper modes list their engine modules.
- **world-partition** (merges next, builds on these formats):
  - Guarantee in the meantime: `SceneSerializer::Serialize`, `Deserialize`, `SerializeEntities` and
    `DeserializeEntities` keep their signatures and behavior; everything new is additive. world-partition writes cells
    through one function of its own that calls today's `SceneSerializer` until this stream merges, then
    `SceneDocumentWriter::Write` with `SceneWriteOptions::Roots`.
  - `DocumentSaver`'s request API (`DocumentSaveRequest`, `DocumentSaveItem` with `Document` bytes, `Submit`, the
    completion, `ReserveNativeAsset(path, handle)`) is committed early and frozen, so world-partition can save dirty
    cells, new cells with their derived handles and the World document through it in one request, with one undo-stack
    save point per request.
  - `AssetPackCookOptions::AdjustSceneSettings` is frozen with the cook: world-partition's export raises a world's
    physics capacity through it.
  - `GameRuntime.cpp` keeps this stream's "Started '<scene>' in N s" log line through world-partition's changes.
  - Cells are scenes: exports cook them with `AssetPackCook` (no cell-specific code), cell loads read through the
    cancellable reader (and may take `PreparedForm::Prototype`), and a world's first scan registers thousands of cell
    documents through the batched registration.
  - Shared files: `GameExport.*` (world-io: cooking and waits; world-partition: cells and capacities; function-level),
    `EditorStreamingCommands.cpp` (world-io: the import budget and statistics; world-partition: cell map and physics),
    `GameRuntime.cpp` (world-partition owns it; world-io adds only the start-time log line), `Scene.*` (world-io: the copy
    option only), `AssetTypes.h` (world-partition appends the World type). `SceneSubScenes.cpp` is world-partition's: this
    stream only offers the prototype form.
- **gpu-driven-rendering**: reuses `WorldFixture.h` (API frozen in the first commit; additions only). This stream
  touches no renderer file, shader or `FrameProfiler.*`; `Editor.Commands` registers through `ST_PROFILE_STAGE` (an
  editor stage: the runtime's perf log never has it). Both streams edit `Scene::Copy`: its context copiers run for full
  copies only, never for this stream's `SerializedOnly` snapshot (gpu-driven-rendering merges later and keeps that).
- **game-ui**: no shared files. Its canvases and widgets are components: saved, streamed and cooked generically.
- **authoring-and-playtest** (merges last):
  - Its compact `editor.status` keeps `assets.scanning` and `saving`; its MCP toolsets and skills say `scene.save` is
    deferred (`wait`).
  - Its `MayDefer` flag goes on every command whose handler this stream makes deferring: `scene.save`, `scene.saveAs`,
    `project.close`, `project.open`, `project.create`, `project.openSample`, `editor.quit`, and the commands that replace
    the scene while `CheckUnsavedChanges` defers (`scene.open`, `scene.new`, `scene.recover` and the others of that
    gate). List them in the report's merge notes. The registry's deferral of `Edit` commands and `WaitsForAssets`
    queries during a scan is not a handler's and needs no flag.
  - Its command splits (`entity.*`, `component.*`, `play.*` into `Editor/Commands/`) keep this stream's
    `WaitsForAssets` flags on the commands they move.
  - `McpServer.cpp`: this stream changes only `strata_launch_editor`'s wait (function-level).
  - `EditorSceneCommands.cpp`, `EditorStateCommands.cpp`, `EditorAssetCommands.cpp`: this stream changes only
    `scene.save`, `scene.saveAs`, `editor.quit`'s deferral and the `WaitsForAssets` flags (function-level).
  - This stream adds **no command**, so the command-kind lists in `CommandJournalTests.cpp` and the MCP tool count are
    unchanged by it.
  - Its contract changes to `play.step` and input do not touch saves; the integrator re-runs Smoke, Tetris, Feature and
    Scripts command scripts after its merge as planned.
- **Integrator**: `Budgets.json` (append), `Layers.json` (append), `AGENTS.md` and `Architecture.md` sections (union),
  the status pills (union: saving and scanning pills beside the others), skills (union).

## Questions to the planner (decided here as stated until answered)

1. **Entity references in the cooked format** are recomputed from the values on load instead of stored (the brief lists
   them among the stored data): a stored table would be a second source of truth that corruption could make disagree with
   the values, and recomputing costs one pass over entity-typed properties. Script fields are stored, typed.
2. **`StrataCLI launch` returns after the scan by default** (an agent gets a ready editor; `--no-wait-scan` returns at the
   session). The acceptance "answers `editor.status` within 5 s" is measured from launch to the first answer, with
   `--no-wait-scan`; the full launch must succeed whatever the scan takes, as long as it progresses.
3. **The import admission gate sets its budget to 1 GiB**: the default (25% of physical memory, at least 1 GiB) would not
   bind for 24 4k textures on the reference machine, so a gate at the default would prove nothing.
4. **`ComponentRegistry.h` gains three hooks** (binary extras, extra entity references), and the Script component's
   registration supplies them, so that no Scene-layer code names `ScriptComponent`. engine-modules, which may move that
   component this wave, should know.
5. **`GameRuntime.cpp` (world-partition's)**: this stream adds one log line ("Started '...' in N s") that its runtime gate
   reads. Alternatively world-partition adds the start time to `--perf-log` through `FramePerfLog`'s API.
6. **Commands defer while a background scan runs** (kind `Edit` and registry queries flagged `WaitsForAssets`) instead of
   answering from a partial registry. The flag is a new `EditorCommand` member; authoring-and-playtest's command work
   should keep it on the commands it touches.

## Answers from the planner (cross-check of 2026-10-13)

1. **Entity references recomputed in the cooked format:** accepted. Script fields stay stored, typed, through
   `ExtraBinary`.
2. **`StrataCLI launch` returns after the scan by default, `--no-wait-scan` at the session:** accepted, and the 5 s
   criterion (`PerfGPU.Editor.Launch30kStatus`) is measured from the launch to the first `editor.status` answer with
   `--no-wait-scan`.
3. **The import admission default:** 25% of physical memory, clamped to 1-4 GiB (item 5), so the default binds on large
   machines too. The gate `Perf.Editor.ImportFolder24x4kPeakPrivate` keeps its explicit 1 GiB and its 1,280 MB limit.
4. **`ComponentRegistry.h` hooks:** this stream adds only `ExtraBinary`. Entity and asset references in extra data are
   engine-modules' `ExtraReferences` (merging first); this stream's `ForEachExtraEntityReference` is dropped (item 4).
   No conflict with the component move: the Script registration's binary hooks are one block that moves with it.
5. **The start-time line in `GameRuntime.cpp`:** accepted ("Started '<scene>' in N s", one line, function-level);
   world-partition's spec keeps it.
6. **`WaitsForAssets`:** accepted; authoring-and-playtest's spec keeps the flags through its command splits. The scan
   deferral happens in the registry before handlers run, so it never meets authoring-and-playtest's `MayDefer` assert.
7. **AssetPipeline files in `Layers.json`:** append to AssetPipeline's `Paths` and Asset's `Exclude`. engine-modules keeps
   that path scheme (no folder restructure).
8. **No `scene.saveStatus` command:** accepted. Progress is in `editor.status` `saving` (kept in the compact form) and in
   `scene.save {wait: false}`'s `saveId`.
9. **Additions from the cross-check:** `SceneWriteOptions::Roots` are UUIDs and `WriteSettings` lets cells omit
   settings; `DocumentSaveItem::Document` writes ready bytes and `ReserveNativeAsset` takes a given handle (both for
   world-partition's cells and World document); `AssetPackCookOptions::AdjustSceneSettings` is the export's scene
   transform; components with JSON-only extras round-trip through cooked scenes as MessagePack; `SerializedOnly`
   snapshots run no context copiers; the over-cap refusal is the Release metric `Perf.Scene.RefuseOverCap1M`, and the
   Debug test uses a lowered limit.
