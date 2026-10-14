# world-io spec STATE (successor continues from here)
- 2026-10-13 started. No spec written yet.
- Steps: [ ] read plan/common/animation format [ ] read scale audit [ ] read code [ ] write spec [ ] StructuredOutput
- Read plan.md, common.md, wave3 animation.md format, scale audit STATE.md + generators. Next: code in .worktrees/verify.
- 2026-10-13 code read so far (verify worktree = 685aef7):
  * SceneSerializer.cpp: Serialize builds DOM; SaveToFile Dump(1,'\t'); DeserializeEntities 4 passes over DOM; entity cap check up front on array size.
  * Prefab.cpp: SceneAsset::Deserialize = JsonUtils::Parse(progress) + SceneSerializer::Deserialize; EntityTemplate::Compile via FromSnapshot (temp Scene then EntityPrototype::Compile).
  * Loaders in Scene/SceneRegistration.cpp (AssetLoadFunction(metadata, AssetLoadData&, err)); no cancel token; RunLoad in AssetManager.cpp:548 checks IsLoadCurrent(handle, generation) only before/after read.
  * EditorContext::SaveScene (cpp ~663): Serialize DOM + ScanDocument + Dump(1,'\t') + SaveNativeAsset, which RUNS THE LOADER on the bytes to validate (= full parse + Scene build again!) -> explains 22 s / 8.76 GB.
  * SceneRecovery::OnSaved only hashes the doc (SHA-256 BaseHash) and starts a NEW journal, clearing pending steps (edits during an async save would be lost -> spec must carry steps after save point).
  * UndoStack::MarkSaved = position; need SavePoint{position, serial} + BreakMerge at snapshot.
  * EditorAssetManager: Scan serial RegisterSourceFile loop (.meta read/write, stats, RegisterAsset), ParallelFor LoadCurrentImport, ParallelFor FindNativeReferences (per-asset .import record file via atomic WriteText), RemoveUnusedCacheFiles, SaveAssetIndex (whole JSON) also every Update when dirty. RegisterAddedFile per file in UpdateExternalImports (main thread). SubmitImport: no admission; RunImport has static s_LargeImportMutex for >64MB sources. HasPendingImports counts Jobs incl. reference searches (QueueNativeReferences on SubmitIO, never cancels previous search).
  * Records: <cache>/<handle>.import JSON per asset, c_ImportRecordVersion; AssetIndex.json handle->path.
  * EditorLayer::OnAttach: opens project (blocking scan) THEN StartAutomation -> session published after scan. StrataCLI EditorLauncher WaitTimeout 60 s; McpServer LaunchTimeout 60 s.
  * GetResidencyInfo/Samples walk all m_Entries (AssetResidency.cpp:187).
  * AssetPack v2 header 64B, EntryRecord 88B; AssetPackWriter DataProvider; EditorAssetManager::WriteAssetPack provider re-scans docs vs plan; GameExport waits HasPendingImports then PlanAssetPack, writes on I/O job.
  * FrameProfiler: ST_PROFILE_STAGE registers dynamically (gpu-driven owns FrameProfiler.*).
  * Budgets: Perf.Scene.Copy1M 43 ms; PerfGPU.Editor.SceneOpen1M* in StrataTests/src/Editor/EditorFramePerfTests.cpp (real editor + command scripts).
  * Tests call harness.Run("scene.save") via Execute directly (HarnessEditor::Run) -> deferred save needs harness Run to complete pending.
- Design decisions (writing spec now):
  * DocumentSaver (Editor/DocumentSaver.*) multi-doc background saves; snapshot = Scene::Copy(SceneCopyOptions{SerializedOnly}); UndoStack::SavePoint{position,serial}+BreakMerge; publish via EditorAssetManager::ReserveNativeAsset/ClaimDocumentWrite/PublishWrittenDocument (no loader re-parse); recovery OnSaved(hash, savePoint) carries steps after save point.
  * SceneDocumentWriter (one entity per line, lines byte-equal to compact Dump of old SerializeEntity; header first; EntityCount hint) built in step 1; SceneDocumentReader (sax_parse, EntitySink: SceneBuilder / EntityPrototypeBuilder / ReferenceSink) in step 3.
  * CookedScene "STSC" v1 SoA columns (dense flag), sections 16-aligned w/ CRC, settings as compact JSON, unknown comps msgpack, script fields via ComponentInfo ExtraBinary hooks (engine-modules may move ScriptComponent); entity refs recomputed (deviation, flag to planner).
  * ImportRecordStore: <cache>/ImportRecords.log, lines "<crc32c hex> <json>", latest wins, torn tail ignored, compaction on I/O job, migrates old .import files; also holds asset index paths.
  * Registration pipeline: prepare on background/IO jobs in batches of 256, insert on main thread in ~1 ms slices; BeginScan/IsScanning/GetScanProgress; Edit-kind commands defer during scan (EditorCommand::WaitsForAssets); automation starts before scan completes; launch waits on session+progress (returns after scan by default; --no-wait-scan).
  * Import admission via BackgroundWork queue (no blocked threads): AssetImporter::EstimateImportMemory(head, size, settings); default 25% RAM min 1 GiB; gate sets 1 GiB.
  * No new commands (MCP count): scene.save {wait}, editor.status saving/assets.scanning, asset.stats importMemory, asset.setBudget importMB.
- Spec written to specs/world-io.md (sections: Why, Scope 0-6, Deliver order, Acceptance, Open items, Files, Merge notes, Questions). Edits applied after review (float formatting test, MarkSaved semantics, deterministic cancel test, Perf.Runtime naming, import gate 1,280 MB). Next: final read-through, then StructuredOutput.
- Final read-through done (fixes: saving dialog Cancel semantics, FinishAll only in destructor, ScriptComponent hook replaces today's two named uses, save gate issued through runner). Spec COMPLETE. Remaining: StructuredOutput only.
