# Workstream `gpu-driven-rendering`: GPU instruments, shadows at scale, a shared geometry pool, GPU culling and indirect drawing, camera-relative rendering (wave 4, size XL)

Read `common.md` first, then AGENTS.md's Rendering rules and Docs/Architecture.md "Render scene", "Frame profiler" and
"Streaming and residency". This stream merges **fourth**: after engine-modules, world-io and world-partition, before
game-ui and authoring-and-playtest. The code described is main 685aef7's; locate it by symbol.

## Why

The owner asks for "high scaling". The rendering audit (`wave4\audit\rendering\`, `STATE.md`, `perf\runtime2` and
`runtime3\summary.json`, `shots\z*`) found that the renderer's CPU cost grows with instances and distinct meshes, and
that nothing measures the GPU.

- **C1: shadow casting is a serial, per-caster CPU cost that no gate exercises.**
  - The exported game at 1920x1080, shadow distance covering the view: 100k cubes cost 6.2-7.3 ms of CPU work (0.82-1.03 ms
    without shadows), 300k cost 18.6-20.4 ms, and 1M cost 67-98 ms. Nearly all of it is `Render.Extract`.
  - The editor's overlay at 100k (`shots\z09_stats_Cubes100k.png`): extract 5.78 ms, sort 0.74 ms, 93,192 instances.
  - `SceneRenderer::FindShadowCasters` (`SceneRendererExtraction.cpp`) queries each cascade and runs the exact
    light-space test on one thread. `GatherShadowDraws` radix-sorts every cascade's draws, and `DrawShadows` clears and
    redraws every cascade every frame.
  - `PerfGPU.Renderer.Render100kCubes` puts its camera 506 m up, with the scene's sun at the default `ShadowDistance` of
    80. It draws no caster (`RendererPerfTests.cpp` `GridView`, `SceneGenerators.cpp` `AddCameraAndLight`).
- **C2: CPU draw submission scales with distinct meshes.** `Scenes/Scale/Unique10k` (`scripts\gen_unique.py`, 10,000
  distinct boxes) records 9,797 opaque, 9,797 prepass and 17,327 shadow draws.
  - `Render.Submit` p50 is 16.6 ms and `Extract` 3.5 ms, for a 20.5 ms frame.
  - 9,799 assets are requested per frame (`shots\z13_stats_unique10k.png`).
  - Every mesh owns four buffers (`Mesh.h` `m_PositionBuffer`...`m_IndexBuffer`), and `DrawBatches`/`DrawShadows` call
    `setGraphicsState` per batch.
- **L1: the engine cannot measure GPU time.**
  - No timer query exists, and `RendererPerfTests.cpp` times with `steady_clock` only.
  - Frames are GPU-bound without anyone being able to tell: 100k cubes run at a 1.48 ms interval for 1.02 ms of CPU, and
    1M at 12.3 ms for 9.8 ms.
  - The person audit asks for GPU timings in the Performance panel.
- **L2: extraction and GPU work grow with everything in the frustum, and nothing culls small or hidden instances.**
  - Extraction without shadows: 100k 0.57-1.10 ms, 300k 3.36 ms, 1M 9.1-13.2 ms. The sort alone takes 2.54 ms for 927,948
    instances.
  - The 1M frame is GPU-bound at about 12 ms for 11.1M mostly sub-pixel triangles (moire, `shots\z10_cubes1m_far.png`).
  - `CullView`'s `QueryFrustum` is serial, and `RenderScene::Sync` updates transforms on one thread.
  - Skinned meshes are skinned again in every pass (`MeshSkinned.vert`, `ShadowMeshSkinned.vert`).
- **L11: no camera-relative rendering.** The sky breaks up 20 km out and geometry is quantized 100 km out
  (`shots\f_0.png`, `f_20000.png`, `f_100000.png`, `z14_sky_20km.png`).
  - `Mesh.vert` transforms to world space, then multiplies by the full view-projection.
  - `Skybox.frag` (and `Grid.frag`) builds its ray from the full `InverseViewProjection`.
- **Scale M: instance uploads bypass the upload budget.** When the instance buffer is recreated, `UploadSceneData` writes
  every instance with `commandList->writeBuffer`, outside the 16 MiB/frame budget. In `rt250k_window.csv`,
  `Render.Submit` took 25.1 ms in the swap frame.
- **Scale M: play start rebuilds the copy's render scene from scratch.** `Render.Extract` takes 360-418 ms at 1M and
  95.7 ms at 250k: the first `Sync` of `Scene::Copy`'s registry builds every proxy.
- **m4: render statistics are out of reach.**
  - `SceneRendererStats` reaches only `ViewportPanel::DrawStatsOverlay`.
  - `--perf-log` has no `Scene.Animation` column, because scene systems register their stages when a scene first plays
    (`Scene.cpp`, the system loop), after `FramePerfLog::Create` wrote the header.
- **If time:**
  - L3: 10,000 lights draw 4,097 and drop 5,904 (`CollectLights`, `MaxLights` 4096).
  - L9: generated LODs pop with torn UV and normal seams (`MeshUtils::GenerateLODs`: `meshopt_simplify`, `targetError`
    0.02, the result's error discarded; `LODSelection::SelectLOD` picks by halving).
  - m3: mip levels drop after 2 s with gigabytes of budget headroom (`TextureStreamingPolicy::c_DropDelayFrames` 120;
    `levelsDropped` 82, `levelsDroppedForBudget` 0).

## Scope

The CPU path that exists today stays as the **reference and the fallback**.
- `SceneRendererSpecification::GPUDriven` (`GPUDrivenMode::Auto | Off`) picks the path. `Auto` uses the GPU path where
  the device enables `drawIndirectFirstInstance` and `multiDrawIndirect`.
- Every GPU test compares the two paths' pixels.
- The CPU path gets fixes (2) and (3) and keeps them.

### 1) GPU instruments (first: every later gate needs them)

- **Timestamps in the device, not NVRHI's timer queries.** NVRHI's Vulkan timer queries are unsafe to build on:
  - `Device::pollTimerQuery` reads 32-bit results without `VK_QUERY_RESULT_64_BIT`, which the Vulkan spec lets drivers
    saturate;
  - `getTimerQueryTime` spins until the GPU is done;
  - both `begin` and `end` end the render pass.

  Instead, add a timestamp pool to `GraphicsDevice` (implemented in `Platform/Vulkan/VulkanGraphicsDevice.cpp`), as the
  engine already records raw Vulkan commands for `RecordBufferToTextureCopies`:
  - `Scope<GPUTimestampPool> CreateTimestampPool(uint32_t count)`, with `Reset(first, count)` (a host reset, since
    `hostQueryReset` is enabled where supported; otherwise a recorded reset at the head of the next command list);
    `Write(nvrhi::ICommandList*, index)` (`vkCmdWriteTimestamp2` at all-commands, allowed inside render passes); and
    `Read(first, count, std::span<uint64_t>)`, which returns false until every value is available and never waits
    (64-bit, `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`).
  - `GraphicsDeviceInfo` gains `SupportsTimestamps` (the graphics queue's `timestampValidBits` > 0), `TimestampPeriodNs`,
    `SupportsMultiDrawIndirect`, `SupportsDrawIndirectFirstInstance`, `SupportsDrawIndirectCount` and
    `SupportsSamplerMinMax`. They are filled from the features the device already enables.
- **`Renderer/GPUProfiler.{h,cpp}`** (`Renderer::GetGPUProfiler()`):
  - A ring of `MaxFramesInFlight + 2` frame slots of `c_MaxTimestampsPerFrame` (256) timestamps.
  - `GPUPassTimer` (RAII on a command list) writes one timestamp per pass boundary: `GPUPassTimer timer(commandList,
    stage)`, then `timer.Next(stage)`, and the last timestamp on destruction. Stages therefore tile a command list without
    gaps.
  - `GPUProfiler::BeginFrame` runs from `Renderer::BeginFrame`. It reads the oldest pending slot without waiting, then
    hands its per-stage sums (all renderers of that frame added up, like CPU stages) to the frame profiler, tagged with
    the profiler frame that recorded them.
  - A slot that is still pending when its turn to be reused comes is dropped (`GPUProfilerStats::DroppedFrames`), never
    waited for. Without timestamp support everything is a no-op.
- **GPU stages**, registered by `RegisterRendererModule` as `FrameStageKind::GPU`:
  - `GPU.Uploads`: the upload queue's graphics batch (`GPUUploadQueue::SubmitBatch`);
  - `GPU.Setup`: scene-data uploads, environment maps, light clusters;
  - `GPU.Cull`: culling dispatches and the HZB, from (4);
  - `GPU.Shadows`, `GPU.Prepass`, `GPU.AO`, `GPU.Opaque` (with the sky), `GPU.Transparent`;
  - `GPU.Post`: exposure, bloom, tone mapping, FXAA;
  - `GPU.Overlays`: grid, tints, outline, lines, text, the copy into a target;
  - `GPU.ImGui`: the editor's UI draw, in the ImGui layer's renderer. It registers there, on first use, not in
    `RegisterRendererModule`: after engine-modules games link no ImGui, so their perf logs get no empty ImGui column;
  - `GPU.UI` is registered by game-ui around its pass (see "Interfaces").
- **`FrameProfiler` (Core) learns GPU stages** without knowing the GPU:
  - `RegisterStage(name, inWorkTime, FrameStageKind kind = CPU)`. `BeginStage` refuses GPU stages, which are never in
    the work time, `StageSum` or `Untimed`.
  - `RecordGPUFrame(uint64_t frameIndex, std::span<const GPUStageTime>)` attaches GPU times to that frame's record while
    it is still in the ring. `FrameSample` gains `HasGPU`, `GPUBusyMs` and `GPUStageMs`.
  - Listeners choose `FrameListenerTiming::Immediate` (today's behavior) or `WithGPU`. A `WithGPU` listener is called in
    frame order once the frame's GPU times are attached, or after `c_MaxGPULatencyFrames` (8) frames without them.
  - `FrameProfileSummary` gains `GPU`: frames with data, `Busy` stats, the observed latency in frames, and `BoundBy`:
    - `GPU` when the median GPU busy time is at least 90% of the median interval and above the median work time;
    - `CPU` when the median work time is;
    - `Capped` when neither is: vsync, a frame-rate cap or idle throttling sets the pace;
    - `Unknown` without GPU data.
  - GPU stages appear in `Stages`, with their kind, and their statistics are taken over the frames that have data.
- **`--perf-log`** (`Runtime/FramePerfLog`):
  - `Create` first registers the `ProfileStage` of every registered scene system (`SceneSystemRegistry::GetAll`), so
    `Scene.Animation` and every later stream's system get columns. No `SceneSystem.cpp` change is needed.
  - GPU stages become columns like any stage. Rows are written by a `WithGPU` listener, so a row's CPU and GPU columns
    describe the same frame. New columns: `GPUFrame` (1 when GPU times arrived) and `GPUBusyMs`.
  - A stage registered after the header gets no column, and is warned about once by name.
  - **Column groups** (`FramePerfLogSpecification { std::vector<FramePerfColumnGroup> Columns; }`,
    `FramePerfColumnGroup { Names, Write }`, `FramePerfRow::Add`/`AddEmpty`, exactly as common.md section 12 declares
    them) are landed by **world-partition**, which merges before this stream and needs them for its world and physics
    columns; this stream does not write them a second time. Until its rebase onto world-partition, its new columns
    are built-in ones (GPU stages among the stage columns, `GPUFrame` and `GPUBusyMs` after `PeakPrivateMB`); group
    columns follow every built-in column.
  - In its rebase, `RuntimeApplication` adds a `Render` group from `GameRenderer::GetStats`: `DrawCalls`, `Instances`,
    `Triangles`, `VisibleProxies`, `OccludedProxies`, `InstanceUploadMB`.
- **Render statistics for agents.** `CommandUtils::DescribeRenderStats(const SceneRendererStats&, const
  SceneRendererGPUStats&)` returns a camelCase `renderStats` object:
  - `gpuDriven`, `drawCommands` (draws the CPU recorded), `draws` (draws the GPU made), `opaqueDraws`, `shadowDraws`;
  - `instances`, `shadowCasters`, `triangles`, `visibleProxies`, `occludedProxies`, `culledProxies`, `instancesPerLod`;
  - `lights`, `droppedLights`, `materials`, `pendingAssets`;
  - `extractMs`, `sortMs`, `proxyUpdates`, `instanceBytesUploaded`, `deferredInstanceBytes`;
  - `geometryPool` (`usedMB`, `capacityMB`);
  - `gpuMs` (per GPU stage of the frame the GPU counters belong to);
  - `gpuStatsFrame` (that frame).

  It appears in:
  - `viewport.capture`'s result, from the capture renderer's frame. The capture already waits for its readback, so its
    GPU counters and timestamps are resolved.
  - `editor.perf`'s result, from the viewport panel's renderer. `editor.perf` also gains `gpu: {frames, latencyFrames,
    busy, stages: {"GPU.Shadows": {avgMs, p50Ms, p95Ms, maxMs}...}, boundBy}`.
  - `stages` stays CPU-only, so existing readers keep their contract.

  Update both commands' descriptions, and the editor-automation skill.
- **Editor** (UI rules: theme tokens, font-relative sizes, the widget kit):
  - `PerformancePanel` gets a GPU timeline beside the CPU one: same frames, same scale, stacked GPU stages, frames
    without data left blank. Its stage table gets a "GPU" section with the busy total and the bound-by line, and Copy as
    JSON includes `gpu`.
  - The status bar's frame pill keeps its texts (`EditorLayer::StatusPillText`). Its tooltip (`UI::IsTooltipDue`) adds
    "GPU-bound: GPU 11.8 ms busy of 12.3 ms frames (CPU 9.8 ms)", or CPU-bound, capped or unknown.
  - `ViewportPanel::DrawStatsOverlay` adds a GPU line (the frame's busy time and its three largest stages) and an
    indirect line (draw commands, GPU draws, occluded).
  - Nothing new runs per frame while idle beyond reading a few timestamps (see the idle-frame criterion).
- **Perf lab:** `Perf::MeasureGPU`, beside `MeasureRender`, returns the median GPU busy time of measured frames from
  `GPUProfiler`. It is shared through a new `Perf/RendererPerfUtils.h` when the editor's perf tests need it;
  `PerfUtils.h` stays free of renderer includes. The measure loops already call `Renderer::BeginFrame`, which resolves
  the timestamps; a final `ResolveGPUStats(true)` collects the last frames' times.

### 2) Shadows at scale: a CPU fix now, kept as the CPU path

- In `FindShadowCasters`:
  - The cascades' `QueryPlanes` run as one job each (queries are const and may run concurrently) and merge in cascade
    order with the stamps.
  - The exact light-space test runs in `ForEachChunk` chunks of `c_ExtractionChunkSize`. Each chunk writes its own
    caster list and run-length mesh and material marks; they merge in chunk order, so results do not depend on threads.
  - The animator reports (`CanShadowView`) move into the chunks: the stamp reads are read-only, and the merge collects
    the reports.
- **Sub-texel casters:** a caster stays in cascade c only while its light-space extent (the larger of x and y) is at
  least `c_MinCasterTexels` (1.0) times `CascadeBox::WorldPerTexel`. `SceneRendererStats::SubTexelCasters` counts the
  casters dropped. The GPU path applies the same rule.
- `GatherShadowDraws` keeps its parallel chunks. Measure the sort; if it dominates at 300k, sort per cascade in
  parallel (the cascade is the key's top bits, so per-cascade sorts concatenate into the same order).
- **Optional, last in (2):** cache static casters' cascades. A cascade is redrawn only when its texel-snapped box moved
  or an instance inside it changed (`RenderScene` versions instance changes). This needs per-cascade depth copies; land
  it only with a test that moving one caster updates its cascade.
- **New gates (see Acceptance):** `PerfGPU.Renderer.Render100kCubesShadowed` and `Render300kCubesShadowed`.
  - They use `Perf::AddCameraAndLight(scene, shadowDistance)`, which takes the shadow distance as a new parameter, set to
    four times the grid's half extent, as in `gen_scale.py`, and `GridView`.
  - They check that casters are really drawn: `ShadowCasters` at least 0.9 times the cube count, read from the GPU
    counters on the GPU path.

### 3) The geometry pool

- **`Core/RangeAllocator.{h,cpp}`:** an offset allocator over a range of units.
  - It allocates first-fit from size-class free lists, with alignment, and coalesces on free.
  - It answers `GetLargestFree`, `GetFreeUnits` and `GetFragmentation`.
  - `PlanCompaction` returns moves to lower offsets, deterministic and bounded per call.
  - Unit tests: `Core.RangeAllocator`.
- **`Renderer/GeometryPool.{h,cpp}`** (`Renderer::GetGeometryPool()`). Its buffers:
  - two vertex arenas: `Static` (positions 12 B and attributes 36 B per vertex, in two buffers) and `Skinned` (the same
    plus the 16-byte skin stream);
  - one index arena (32-bit indices, relative to a submesh's base vertex as today).

  Allocations and handles:
  - Meshes get a stable `GeometryHandle`. The pool's table maps it to the current `GeometryAllocation`
    (`Arena`, `FirstVertex`, `VertexCount`, `FirstIndex`, `IndexCount`), so moving an allocation changes no `Mesh`.
  - Freed ranges become reusable only once the GPU finished the frames that could read them (fenced like
    `DeferredReleaseQueue`).

  Growth and compaction:
  - The pool grows by creating a larger buffer, copying on the GPU (`copyBuffer`) and releasing the old one through
    `Renderer::ReleaseDeferred`.
  - It compacts by GPU copies within `c_MaxDefragBytesPerFrame` (8 MiB) when fragmentation exceeds 25% of its capacity.
  - Growth and moves happen between upload batches (`GeometryPool::BeginFrame`), never under a mesh's upload in progress.

  GPU tables, uploaded with each mesh's geometry through `GPUUploadQueue` (small):
  - `GeometryEntry` (per handle: arena, first vertex, first index, first submesh, submesh count);
  - `GeometrySubmesh` (base vertex, first LOD, LOD count, local bounds, `UVDensity`);
  - `GeometryLOD` (absolute first index, index count; `Error` if (7c) lands).

  A `GeometryLOD` index is the draw record of (4). Bump `GeometryEntry`'s layout only with the shaders that read it.
- **`Mesh`:**
  - `FinalizeOnMainThread` allocates its ranges and uploads its streams into the pool's buffers at their offsets, in the
    same `PlanStep`/`HasStagingRoom` steps as today.
  - Pool uploads run on the graphics queue: a shared buffer cannot be handed between queues per mesh, and the transfer
    queue stays off by default anyway. `ChooseQueue` returns Graphics for them, and the transfer path for meshes goes.
  - `GetPositionBuffer`/`GetAttributeBuffer`/`GetSkinBuffer`/`GetIndexBuffer` are replaced by
    `GetGeometry() -> const GeometryAllocation*` (null until uploaded) and `GetGeometryHandle()`.
  - Destroying a mesh frees its allocation.
  - Positions and indices stay on the CPU for physics (`AssetMeshProvider`, until Mesh v2).
- **Memory accounting stays exact:**
  - `GetMemoryUsage` and `GetFinalizedMemoryUsage` report the allocation's bytes (vertex stride times count plus index
    bytes, rounded to the allocator's granularity) as `GpuBuffers`.
  - The pool's free space and its tables are the renderer's, reported in `GeometryPoolStats` (`renderStats.geometryPool`,
    and `asset.stats`' `geometryPool`).
  - The residency tests (`GPU.Assets.Residency`) and `StateCountsMatch` keep passing.
  - `GPUAssetTests` and `GPUAssetUploadTests` read the pool's ranges instead of per-mesh buffers.
- **Draws on the CPU path** are bucketed by pipeline state (`PipelineKey`: double-sided, mirrored, skinned; shadows also
  alpha-masked and cascade):
  - One `setGraphicsState` per bucket binds the pool's buffers.
  - The bucket's batches become `DrawIndexedIndirectArguments`, written by the CPU into a per-frame args buffer: one
    `drawIndexedIndirect(offset, count)` per bucket where `multiDrawIndirect` is enabled, else one `drawIndexed` per batch
    without state changes.
  - C2's submit cost then follows buckets, not distinct meshes.
- **Gate:** `PerfGPU.Renderer.Render10kUniqueMeshes` uses `Perf::CreateUniqueMeshGrid(scene, assets, 10'000)`, 10,000
  distinct 24-vertex boxes with slightly different extents (like `gen_unique.py`), each a memory asset of
  `SceneTestAssets`.

### 4) GPU culling and indirect draws

Land in sub-steps, each behind `GPUDriven` with the CPU path as reference.

**4a. The GPU scene: mirrored once per scene, uploaded where it changed.**

- **Ownership.** `Renderer/RenderSceneGPU.{h,cpp}` lives in a renderer-owned `RenderSceneGPUCache`, keyed by
  `RenderScene::GetId()`.
  - It is not in the registry context: scenes are destroyed on background jobs (`Scene::ReleaseInBackground`), and GPU
    resources go through `Renderer::ReleaseDeferred` on the main thread.
  - `RenderScene`'s destructor posts its id to a thread-safe queue that `Renderer::BeginFrame` drains. Mirrors unused for
    `c_UnusedFrames` (300) are released as well.
  - Every renderer of a scene (viewport, capture, Game view) shares one mirror. `RenderSceneGPU::Update` runs once per
    render scene version per frame, so a second renderer uploads nothing.
- **Buffers:**
  - `Instances`: today's `InstanceData`, still 64 bytes.
  - `InstanceDraw`: 8 bytes per instance, its `GeometrySubmesh` index (or "no geometry") and its proxy.
  - `ProxyCull`: 32 bytes per proxy, its world bounds (posed bounds for skinned proxies, as `GetWorldBounds` already
    gives) and flags (drawable, casts shadows, mirrored, skinned, active).
  - `MeshSlots`: the `GeometryHandle` of each mesh slot.
  - `Palette`: the joints.
  - `Materials`: per material slot. See 4f.
- **Change logs.** `RenderScene` gains:
  - `GetProxyChanges(sinceVersion)`, logged like `GetInstanceChanges` (the last 65,536 changes, else a full update);
  - mesh slot geometry changes. When `CheckMeshLayout` learns a layout from a finalized mesh it records its
    `GeometryHandle`, and evictions clear it (content changes).
- **Uploads go through staging, never `writeBuffer`, and are counted.** New:
  `GPUUploadQueue::RecordSceneUpload(nvrhi::ICommandList*, std::span<const GPUBufferRegion>)`.
  - The copies are recorded into the renderer's own command list, so they precede its culling.
  - They are counted in the queue's frame statistics (`SceneDataBytes`, `SceneDataMs`) and in `--perf-log`'s
    `UploadQueue*` columns.
  - Changes of proxies already on the GPU upload in full every frame: a moved object must never lag.
  - **New proxies** upload within `SceneRendererSpecification::NewInstanceBytesPerFrame` (16 MiB). A proxy becomes
    drawable when its instances and its cull record arrive in the same batch.
  - Until then it is counted in `deferredInstanceBytes` and not drawn. A destroyed proxy's record is cleared at once.
- **Growth.** Buffers grow by GPU copy into a larger buffer (the old one released deferred), so recreating the instance
  buffer uploads nothing again. This closes the `writeBuffer` finding (Scale M).
- **Scene loads.** `RenderScene::PrepareLoadedScene` also fills the loaded scene's mirror within its frame budget, so the
  frame that swaps a scene in uploads no instance. This covers world-partition's cell loads through the budget above.

**4b. Cull, compact, draw: frustum and screen size.**

- **`Scene/Cull.comp`**, one thread per proxy, per view. Views are the main view and each shadow cascade (variant
  `ST_CULL_SHADOW`), in camera-relative coordinates (5). It tests:
  - drawable and active;
  - the frustum (planes; the cascade box);
  - screen size: the bounding sphere's projected diameter of at least `SceneRendererSpecification::MinProjectedPixels`
    (0.5) for the view, and at least one texel for cascades, as in (2). The CPU path applies the same rule, so the paths
    agree; 0 turns it off.
- **Per instance of a passing proxy:**
  - the LOD, with `Include/LODSelection.glsl`, the same function as `LODSelection` (see below);
  - the material flags from the material table: transparent instances are skipped (the CPU draws them), alpha mask and
    double-sided pick the bucket;
  - the bucket: mirrored, double-sided and skinned; for cascades also alpha-masked;
  - an atomic count on the (bucket, `GeometryLOD`) record.
- **Compaction.**
  - `Scene/CullBuild.comp` turns the non-zero records into `DrawIndexedIndirectArguments` per bucket: `firstInstance` is
    the record's offset in the bucket's draw list, `baseVertex` and `firstIndex` come from the pool tables. It also
    writes a count per bucket.
  - `Scene/CullScatter.comp` writes each visible instance's index into its record's range of the draw list. The shaders'
    `u_DrawList[gl_InstanceIndex]` stays as it is.
  - The order of instances within one draw follows GPU scheduling. Opaque pixels do not depend on it (depth test);
    transparent draws stay CPU-sorted.
- **Draws.** One `drawIndexedIndirectCount` per bucket where `drawIndirectCount` is enabled (`indirectParams` and
  `indirectCountBuffer` in `GraphicsState`, with buffers made with `setIsDrawIndirectArgs`).
- **Fallbacks:**
  - Without `drawIndirectCount`: `drawIndexedIndirect(bucketOffset, bucketCapacity)` over args whose unused tail
    `CullBuild` zeroes.
  - Without `multiDrawIndirect`: one `drawIndexedIndirect` per record slot.
  - Without `drawIndirectFirstInstance`: the CPU path.
  - Tests force each mode with `SceneRendererSpecification::IndirectMode` (`Auto | NoCount | PerDraw`).
- **NVRHI pitfalls** (AGENTS.md):
  - dispatches through one binding set that read each other's writes need `setBufferState(..., UnorderedAccess)` plus
    `commitBarriers()`;
  - the args and count buffers need `IndirectArgument` states;
  - counters are cleared with `clearBufferUInt`.
- **`LODSelection`** gets one formulation the CPU and GLSL evaluate identically: the LOD is one plus the number of times
  the fraction can be doubled while staying below the threshold, as comparisons with the threshold scaled by powers of
  two, with no `log2`. `Renderer.LODSelection` checks that it matches the old function except at exact power-of-two
  boundaries. A GPU test compares 10,000 fractions between the CPU and the shader.
- **The CPU cost per frame becomes constant in instance count:** recording the dispatches and indirect draws (a fixed
  number per view and bucket), plus uploading what changed.
  - **Transparent surfaces stay CPU-sorted, without walking every proxy.** `RenderScene` keeps users per override material
    slot (as it does per mesh). The renderer marks the material slots it resolved as transparent
    (`RenderScene::SetMaterialTransparent`), and each frame walks only the users of transparent slots (by override, or by
    a mesh whose submesh uses the slot) against the frustum.
  - **Skin proxies keep their CPU visibility test** (`m_Skins`, thousands at most) for `ISkinningPoses::ReportVisible`
    and `CanShadowView`, so `WhenVisible` animators see no added latency.
- **Statistics with latency.** Counters such as instances, triangles, draws, occluded and per-LOD come from a small
  readback buffer, 2-3 frames late, through `SceneRenderer::GetGPUStats() -> SceneRendererGPUStats` (with the frame
  they belong to).
  - `SceneRenderer::ResolveGPUStats(bool wait)` is for tests and tools.
  - `GetOpaqueBatches`, `GetDrawList` and their shadow and transparent counterparts describe the CPU path only. The GPU
    path returns empty spans and documents why.

**4c. Two-phase HZB occlusion (view first, cascades last).**

- **Per-view state.** `ViewCullState` holds visibility bits per proxy, per renderer and view. It survives a
  render-scene clone (6), since proxy indices are equal.
- **Phase 1** draws, after the frustum and size tests, the proxies visible last frame, into the prepass.
- **The pyramid.** `Scene/HZBBuild.comp` builds a depth pyramid from that depth: reversed-Z, so each texel keeps the
  minimum (farthest) depth. It uses the min-reduction sampler (`SamplerReductionType::Minimum`) where
  `SupportsSamplerMinMax`, and explicit gathers otherwise.
- **Phase 2** tests every frustum-visible proxy against the pyramid. It draws those not drawn in phase 1 into the
  prepass, and sets each proxy's bit to its phase-2 result. A proxy that becomes visible is drawn in that same frame:
  no hole.
- **The forward pass** draws both phases' lists with the `EQUAL` depth test. AO, entity IDs, outlines and tints read the
  final depth and IDs.
- **Cameras.** Orthographic cameras are supported. A camera cut, or a renderer's first frame, has no history: phase 1
  draws nothing and phase 2 tests everything against an empty pyramid.
- **Cascades** get the same treatment last, with their own pyramids, only if (4) is otherwise done.

**4d. Visibility feedback drives requests and mip streaming. Only then does GPU culling replace the CPU marks.**

- **What `Cull.comp` writes.** For every proxy that passes the frustum and size tests, it writes feedback: visible or
  occluded, and whether or not its mesh is in the pool yet. Unloaded meshes are placed by their recorded bounds
  (`ResolveUnloadedBounds`, already in the proxies' bounds), so the GPU requests them where they are. This replaces the
  brief's coarse CPU pass over spatial-index cells, which is then needed only for meshes of unknown bounds (below).
  The feedback holds:
  - per mesh slot, the maximum screen pixels (`atomicMax` on float bits), with the use (view, high above
    `HighPriorityScreenFraction`, occluded, shadow);
  - per material slot, the maximum pixels and the maximum UV pixels. These are computed like
    `TextureStreamingPolicy::GetWorldPixels`/`GetUVPixels` from the submesh's `UVDensity` and the instance's axis scales,
    ported in `Include/TextureStreaming.glsl`.
  - a prefetch bit for proxies within `PrefetchDistance`, which removes the CPU's `QuerySphere` too.

  A pack pass compacts the touched slots into lists, so the readback is proportional to visible distinct assets.
- **The readback.** `Renderer/BufferReadback.{h,cpp}` (like `TextureReadback`: copy, poll `IsReady`, never wait)
  returns them 2-3 frames late. `SceneRenderer` then:
  - resolves the meshes at High, Normal or Low, and the materials;
  - builds `VisibleMaterialUse`/`VisibleTextureUse` for `TextureStreamer::ReportView` from the textures it recorded per
    material slot (4f), with no per-texture lookup.
- **The CPU marks remain only where the GPU cannot know yet:**
  - meshes of unknown bounds (`GetMeshesWithoutBounds`, per mesh slot);
  - the first frames of a render scene (until its first feedback arrives);
  - frames after a camera cut (`TextureStreamingPolicy::IsCameraCut`), so a cut requests everything at once and
    `PerfGPU.Streaming.FramesToSettle` keeps its value.

  On those frames `CullView`'s marks (parallel chunks) run as today.
- **Holding assets.** `AssetResolveCache` still holds what the frame resolved. Meshes the GPU draws are held by the pool's
  deferred frees, so an eviction never frees memory a frame in flight reads.

**4e. Skinned proxies** cull with their posed world bounds (already their proxy bounds) and draw in the skinned buckets,
from the `Skinned` arena with their palettes. **Optional:** skin once per frame in compute (`Scene/Skin.comp`) into a
per-frame posed-vertex range, so later passes draw plain buckets. Land it only with `PerfGPU.Renderer.Skinned1000`
measured better.

**4f. Material data that follows content changes, and the texture table.**

- **`Renderer/TextureTable.{h,cpp}`** (`Renderer::GetTextureTable()`):
  - every `Texture` gets a stable table index for its life. The entry holds the bindless slot, the sampler index and the
    grayscale bit, and is updated whenever the texture swaps its bindless slot (`Texture` level changes, finalization,
    eviction);
  - only changed entries upload, once per frame;
  - indices 0-2 stand for white, black and flat normal.
- **`MaterialData`'s map fields hold table indices.** `SampleMaterialTexture` (`Include/Scene.glsl`) reads `u_Textures`
  first.
- **The material table** is per mirror, per material slot. It is rebuilt only for material slots whose material or
  textures changed: content changes, plus a reverse index from texture to material slots for loads and evictions.
  - Mip changes no longer touch material data.
  - The per-frame `memcmp` of every drawn material in `ResolveAssets` goes, on both paths.
- **Debug views.** `RenderScene::Validate`, and `StreamingView`'s Streaming, LOD and Mip views, keep working.
  - The views keep their CPU builds, which run only while a view is on.
  - They count only entities the last frame's GPU visibility saw. `SceneRenderOptions::ReadVisibility` requests the
    per-proxy visibility bits, read back only while a debug view is on, so occluded entities are not counted.
  - The LOD view uses the same `LODSelection`.

### 5) Camera-relative rendering

- **`RenderData::FrameConstants`.**
  - `View`, `ViewProjection`, `InverseView` and `InverseViewProjection` become rotation-only (the view's translation is
    0); `Projection` is unchanged.
  - `CameraPosition.xyz` stays the camera's world position, for the few shaders that need absolute coordinates.
  - Update the `static_assert` and `Include/Scene.glsl` together.
- **Camera-relative world space in every scene shader.**
  - `Mesh.vert` and `ShadowMesh.vert` compute `(instance translation - camera) + rotation-scale * local` with
    `TransformPositionRelative` in `Include/Scene.glsl`. This subtracts before anything large is added, so vertices near
    the camera keep full precision.
  - `v_WorldPosition` becomes `v_RelativePosition`, and `GetViewVector` uses `-relative`.
  - **Lights:** `LightData.PositionRange.xyz` is uploaded camera-relative (`CollectLights` subtracts in double on the
    CPU). The cluster bounds are computed from those.
  - **Shadows:** the cascade light view is centered on the camera. `FindShadowCasters` computes the camera's light-space
    position in double and snaps the cascade centers in absolute light space, then makes them relative, so texel
    snapping stays stable. `CascadeViewProjection` takes camera-relative positions.
  - **Sky:** `Skybox.frag` builds its ray from the rotation-only `InverseViewProjection`.
  - **Overlays:** `Grid.frag` adds `CameraPosition` back for its lines. `SceneRenderer::DrawDebugLines` and the text pass
    upload world-space vertices minus the camera, a small CPU subtraction, for their world-space parts.
  - **Culling (4)** tests camera-relative bounds and planes.
- **`InstanceData` stays 64 bytes and is never rewritten per frame.**
  - Its translation is the scene's own float32 value, so the shader's subtraction is exact to half a unit in the last
    place of the result: about 0.06 mm 1 km from the camera.
  - The low halves of high/low pairs would be identically zero while scene transforms are float32, which they stay this
    wave (common.md, origin rebasing is w5). The spec therefore stores none (see "Questions").
  - If transforms become 64-bit, the low parts go into a parallel 12-byte buffer, as `InstanceDraw` does.
  - What remains is the documented float32 limit: entity origins quantize to 7.8 mm at 100 km, while their vertices no
    longer do.
- **Precision of CPU math.** Do the CPU-side math that mixes the camera and large positions in double: light positions,
  cascade snapping, debug-line offsets.

### 6) The render scene cloned on `Scene::Copy` (the play start's render part)

- **The hook.** `Scene` gains a copy hook registry for context data, and the Renderer module registers the render
  scene's.
  - `using SceneContextCopier = void (*)(const Scene& source, Scene& destination);` with
    `Scene::RegisterContextCopier(SceneContextCopier)`. It is only allowed while the composition root runs, like
    components.
  - `Scene::Copy` calls the copiers after `ConnectSignals`, for full copies only: world-io's `SerializedOnly` snapshot
    copies (background saves) run none, since a save needs no render scene.
  - `RegisterRendererModule` registers `RenderScene::CopyIntoScene`, which does nothing when the source has no render
    scene (`RenderScene::Find`).
  - The change in `Scene.cpp` stays function-level.
- **The clone keeps proxy indices.** Proxies are keyed by the preserved EnTT identifiers, so every proxy, instance, slot
  and palette index is identical:
  - per-proxy and per-instance arrays are shared by **copy-on-write pages** of 16,384 proxies (`RenderSceneArray<T>`:
    pages held by `Ref`, copied on the first write through `Mutable(index)`);
  - the spatial index shares its per-object array the same way, and copies its cells;
  - the small tables are copied: mesh and material slots, skins, pending entities, `m_ProxyByEntityIndex` pages;
  - the clone connects its listeners to the destination registry without queueing every entity.
- **Renderers.** The clone records `GetCloneSource()`. A renderer whose last render scene was the source keeps its
  per-slot state and its `ViewCullState`.
  - The clone's GPU mirror is created by GPU copies of the source's (`RenderSceneGPU::CloneFrom`), not by uploads.
  - VRAM for the mirror is doubled while playing: about 100 MB at 1M. Report it in `renderStats.geometryPool`'s
    neighbor `sceneMirrorMB`.
- **Why pages.** At about 250 bytes per proxy (`RenderScene`'s 15 per-proxy arrays, the instances and the spatial index),
  a deep copy of 1M proxies moves about 250 MB into fresh memory. That is estimated, not measured, at 15-25 ms on this
  machine, more than the 5 ms the acceptance asks.
  - The pages add an indirection to `RenderScene`'s hot loops.
  - Measure `PerfGPU.Renderer.Render100kCubes`, `Perf.Scene.*` and `RenderScene` `Sync` before and after. If the pages
    cost there, stop and report (see "Questions").
- **Validation.** `RenderScene::Validate` passes on the clone at once and after diverging writes.

### 7) If time, in this order

**a) m3: mip levels.**
- Drop levels only under GPU texture-budget pressure, or after `c_IdleDropSeconds` (30 s) unneeded, least recently
  needed first (`TextureStreamingPolicy`). The 2 s `c_DropDelayFrames` rule goes.
- Fade raised levels in over `c_FadeInSeconds` (0.25 s): `TextureTable` entries gain a `MinLod`, which
  `SampleMaterialTexture` applies by scaling the gradients (`textureGrad`), keeping anisotropy.
- `PerfGPU.Streaming.Mips.*` must hold. `Renderer.TextureStreamer` tests cover the new policy.

**b) L3: lights as `RenderScene` proxies.**
- Point and spot lights get proxies (SoA, change-logged and mirrored like meshes).
- `Scene/LightCull.comp` culls them into the cluster grid on the GPU without the 4,096 cap. They are ranked by relevance
  only when a cluster overflows `MaxLightsPerCluster`.
- The scene with 10,000 lights draws all 10,000 visible lights. `EditFramePointLights10k` must not regress.

**c) L9: LOD error.**
- `MeshUtils::GenerateLODs` uses `meshopt_simplifyWithAttributes` (normals, UVs, locked borders) and stores each LOD's
  object-space error in `MeshLOD::Error`. Bump `Mesh::c_CookedVersion` to 3 (version 2 still loads, with `Error` 0
  meaning "use the old halving") and `GltfImporter`'s version, and extend the cooked-mesh corruption tests.
- Selection picks the coarsest LOD whose projected error stays under `LODErrorPixels` (1.0), with a dithered cross-fade
  over `c_LODFadeFrames`. Both levels are drawn with complementary dither masks in the prepass and the forward pass (the
  same mask in both, so `EQUAL` holds).
- The LOD legend shows the error in pixels.

### 8) Editor and agents (with 1, completed by 4)

- `renderStats` as in (1); the editor-automation skill gets a "Render statistics and GPU time" section.
- The Performance panel's GPU bar, the pill tooltip, and the stats overlay's GPU and indirect lines.
- No new command, so `CommandJournalTests.cpp`'s lists do not change. `viewport.capture` and `editor.perf` keep their
  kinds.

### 9) Docs

- **Docs/Architecture.md:**
  - "Render scene": the GPU scene, culling and compaction, phases, feedback, buckets, fallbacks, the clone, camera-relative
    space;
  - "Frame profiler": GPU stages, latency, `WithGPU` listeners, `boundBy`;
  - "Streaming and residency": requests from feedback, CPU marks on cuts, geometry pool accounting, m3 if it lands;
  - "Export and the runtime": `--perf-log` columns.
- **AGENTS.md Rendering rules** (rules only):
  - read mesh geometry only through the pool;
  - wrap every new pass in a GPU stage;
  - shaders work in camera-relative space;
  - per-instance additions go to parallel buffers, and `InstanceData` stays 64 bytes;
  - render-scene changes are logged so the mirror can follow;
  - register stages when the module registers.
- **Skills:** `strata-editor-automation` (renderStats, gpu, boundBy); `strata-build-test` (the new perf metrics).

## Deliver in this order if time runs short

1. **(1)** Timestamps, `GPUProfiler`, GPU stages in the profiler, `editor.perf` `gpu` and `renderStats`, the
   `--perf-log` fixes (Scene.Animation, GPU columns; the `Render` group after the rebase onto world-partition's column
   groups). Then the Performance panel, the pill and the overlay.
2. **(2)** Shadow casters in parallel, sub-texel casters, the two shadowed gates. This closes C1 on the CPU path.
3. **(3)** The geometry pool, CPU-path buckets with multi-draw, `Render10kUniqueMeshes`. This closes C2 on the CPU path.
4. **(4a, 4b, 4f)** The GPU scene, frustum and size culling, indirect draws, fallbacks, the texture table. The CPU marks
   still request in this step.
5. **(4d)** Feedback-driven requests and mip streaming. The CPU marks go only once `PerfGPU.Streaming.*` and `Mips.*`
   hold. This step makes `Render1MCubes` constant.
6. **(4c)** HZB occlusion for the view, then (4e).
7. **(5)** Camera-relative rendering.
8. **(6)** The render-scene clone.
9. **(7a, 7b, 7c)**, then cascade HZB and cascade caching.

Docs and skills ship with each item, not at the end.

## Acceptance criteria

**Tests that fail on the base (confirm each, and say how in the report):**

- `GPU.Renderer.Timers`:
  - GPU stages resolve within `MaxFramesInFlight + 2` frames;
  - they are positive for a scene that draws;
  - their sum is at most the frames' interval;
  - nothing waits (a frame never blocks on a slot);
  - 0 validation errors.
- `Core.FrameProfiler`:
  - GPU stages stay out of work, `StageSum` and `Untimed`;
  - late GPU data attaches to its frame;
  - `WithGPU` listeners are called in order, also after 8 frames without data;
  - `BoundBy` follows the 90% rules.
- `Runtime.FramePerfLog`:
  - a log created after `RegisterBuiltinModules` has a `Scene.Animation` column and GPU columns;
  - rows carry their own frame's GPU times;
  - (world-partition's column-group test keeps passing with the GPU columns; the `Render` group's values follow it.)
- `Core.RangeAllocator`: alignment, coalescing, exhaustion, a compaction plan that leaves no gap, determinism.
- `GPU.Renderer.GeometryPool`:
  - allocating, freeing and reusing only after fencing;
  - growth by GPU copy, with contents read back equal;
  - compaction with contents equal at the new offsets, tables updated, and the frame's pixels identical before and after;
  - memory usage equal to the allocations;
  - 0 validation errors.
- **`GPU.SceneRenderer.GPUDriven`.** The indirect path's pixels equal the CPU path's within 1/255, and the entity-ID
  buffers are equal, for each case:
  - opaque instances of many meshes;
  - alpha-masked materials;
  - mirrored transforms;
  - posed skinned meshes;
  - shadowed scenes, every cascade;
  - transparent surfaces mixed in;
  - LODs at several distances;
  - new entities drawn in the frame they were created (within `NewInstanceBytesPerFrame`);
  - instance-buffer growth.

  Also: 0 validation errors.
- `GPU.SceneRenderer.IndirectFallback`: the same scenes with `IndirectMode::NoCount` and `PerDraw` equal the CPU path.
- `GPU.SceneRenderer.Occlusion`:
  - a box behind a wall is not drawn (`occludedProxies` at least 1, its instance absent from the GPU counters);
  - when the wall is removed, or the camera steps aside, the box's pixels are in the entity-ID buffer in that same frame
    (no one-frame hole);
  - an orthographic view works;
  - 0 validation errors.
- `GPU.SceneRenderer.CameraRelative`. The same scene translated by (20,000, 0, 20,000) and (100,000, 0, 100,000), with
  the camera translated alike, matches the origin's image: at least 99.9% of the pixels within 2/255 per channel. The
  scene is a finely tessellated mesh, a textured ground, the procedural sky, the sun's shadows and a point light.
  - The base fails both: a blocky sky at 20 km, quantized vertices at 100 km.
- `Renderer.LODSelection`: the exact formulation matches. `GPU.Renderer.LODSelection`: the CPU and GPU agree on 10,000
  fractions.
- `Renderer.RenderScene`:
  - the proxy change log;
  - the transparent-user walk;
  - `CopyIntoScene`: the clone `Validate`s, shares pages until written, diverges correctly on writes in both scenes, and
    keeps indices.
- `Scene.Copy`: a registered context copier runs for each copy.
- `Renderer.TextureTable` and `GPU.SceneRenderer.GPUDriven`: a texture's level change updates its table entry without
  rebuilding material data, and pixels stay right.
- `Editor.UI`:
  - the Performance panel with fake GPU frames draws the GPU bar and section with no fallback glyphs;
  - the pill tooltip names the bound.
- `Editor.Automation`: `editor.perf` returns `gpu` and `renderStats`. `GPU.Editor` (or the existing capture GPU tests):
  `viewport.capture` returns `renderStats` with `gpuMs`.
- `GPU.FeatureTest` renders the feature scene through the GPU path (the default) with its entity-ID checks unchanged,
  and checks that GPU stages resolved.

**Perf.** These are Release `Budgets.json` entries, measured with interleaved A/Bs under the perf lock (common.md
section 9), "to confirm in the integration campaign". Medians of 60 frames after 10 warm-up frames, as `MeasureRender`
takes them, unless noted.

- `PerfGPU.Renderer.Render100kCubesShadowed`: at most 1.5 ms of CPU. Today about 6 ms is extraction alone; acceptance
  limit.
- `PerfGPU.Renderer.Render300kCubesShadowed`: at most 3 ms (today 18.6-20.4 ms in the game); acceptance limit.
- `PerfGPU.Renderer.Render10kUniqueMeshes`: at most 2 ms for `Render` plus submit (today about 20 ms in the editor);
  acceptance limit.
  - `Render10kUniqueMeshes.DrawCommands` (count) is the CPU-recorded draws: at most the bucket count per pass.
  - The GPU's draw count goes in the report.
- `PerfGPU.Renderer.Render1MCubes`: at most 1.5 ms. Its `Target` is twice `Render100kCubes`' new measured value (when
  that lies below the budget), the "constant in instance count" goal.
- `PerfGPU.Renderer.Render100kCubesGPU` and `Render1MCubesGPU` are median GPU busy times (`Perf::MeasureGPU`), budgeted
  at 1.5 times measured.
  - The report states the 1M image's change from screen-size culling, with before and after screenshots.
- `PerfGPU.Renderer.GPUTimerCost` is `GPUProfiler`'s own CPU time per frame (its writes, resolve and hand-off,
  accumulated by itself): at most 0.05 ms; acceptance limit.
- `PerfGPU.Renderer.PlayStartRender1MCubes`: at most 5 ms; acceptance limit. It is the copy hook's time in `Scene::Copy`
  of a rendered 1M cube grid, plus the copy's first `Render` minus the source's median `Render`. If copy-on-write pages
  cost `RenderScene`'s hot loops more than noise (see (6)), the clone is a deep parallel copy instead: then the budget is
  1.5 x its measured value, 5 ms becomes its target, and the report says so.
- **No regression:**
  - `PerfGPU.Renderer.Render100kCubes`, `Render300kCubes`, `Render10kMaterials` (and `.OpaqueDrawCalls`, now the GPU's
    draws: still 1), `Skinned1000`, `IdleFrame3kStreamedTextures`;
  - every `PerfGPU.Renderer.EditFrame*` (`EditFrameCubes100kStreamingView` included);
  - the `PerfGPU.Editor` idle frames (`{Edit,Play}FrameNested{100k,1M}`) and `PlayStart1MMaxFrame`;
  - `PerfGPU.Streaming.*` and `PerfGPU.Streaming.Mips.*`, with `FramesToSettle` and the exported stress game's peak
    memory;
  - `Perf.FrameProfiler.Frame16Stages`, `Perf.Scene.*`.

  A metric over budget on this branch is re-run against the base build before anything is concluded.
- **Idle cost.** `editor.perf` on an idle editor, before and after, shows no measurable new stage time. The timestamp
  reads are counted in `GPUTimerCost`.

**Everything else:**

- `Architecture.Layering` passes:
  - new files in `Core` (`RangeAllocator`) and `Renderer`; the copier registry in `Scene`;
  - `FramePerfLog` (Runtime) includes `Scene/SceneSystem.h`, which Runtime may include;
  - no allowlist line. The `Physics/AssetMeshProvider.cpp -> Renderer/Mesh.h` line stays as it is, unless engine-modules
    inverted it; then use its provider.
- No ABI change, no new component, no new command. The feature test's coverage is untouched, except `GPU.FeatureTest`
  as above.
- Debug `ctest --preset windows-debug` under the validation layer and Release `-LE perf` pass. The GCC lint, the brace
  check and the compat check come back clean.
- **Screenshots** (Release editor, `--frames N --screenshot`, 150%, `--ui-scale 1`, 1600x900; under
  `scratchpad\wave4\gpu-driven-rendering\shots`):
  - the Performance panel with GPU stages on the stress project;
  - the stats overlay with its GPU lines on 100k cubes;
  - the 1M grid far view (`gen_scale.py` `Cubes1MNoShadow`) before and after;
  - `Unique10k`;
  - the audit's large-coordinate views at 0, 20 km and 100 km (`scripts\far_coords.py`), and the sky at 20 km;
  - the shadowed 100k grid;
  - if (7c) lands, the vase at the audit's LOD switch distances (`z06`).

  Look at each, and fix what looks wrong.

## What this stream guarantees the other streams meanwhile

- **`SceneRenderer::Render`'s contract does not change:** its signature, `SceneRenderOptions`, `GetStats`, the target
  rules and `RenderScene::PrepareLoadedScene` (apart from world-partition's one-line `SceneWorkBudget&` parameter,
  from which the mirror fill of (4a) also draws). Until (4) lands, `GPUDriven` behaves like today's path.
- **The overlay section stays a place for game-ui's single calls:** after the screen-space text in `RenderOverlays`,
  with the output texture composed when an overlay pass has content. game-ui re-applies its hooks there in its rebase.
- **New proxies are drawn in the frame they were created, up to `NewInstanceBytesPerFrame`** (about 250,000 instances).
  Beyond that they arrive over the next frames, counted in `deferredInstanceBytes`, which world-partition's cell drives
  can watch.
- **`ISkinningPoses`, `RenderScene`'s public queries and the debug views keep their meaning.** authoring-and-playtest's
  rest poses need no renderer change.
- **`--perf-log`'s existing columns keep their names and order.** New built-in columns come after `PeakPrivateMB` and
  the GPU stage columns; world-partition's column groups follow every built-in column.

## Interfaces with other streams

- **game-ui: the overlay hook is game-ui's** (`Renderer/SceneOverlay.h`: `ISceneOverlayPass` with `Prepare`, `Draw`,
  `HasScreenContent`, and `SceneOverlayRegistry`, filled by `RegisterGameUIModule`; see game-ui's spec, section 5). This
  stream defines no hook of its own and adds no `SceneOverlayPass.h`.
  - game-ui merges after this stream and re-applies its four single calls on the final `SceneRenderer` (pass
    construction, `Prepare` after the text renderer, the overlay condition, `Draw` after the screen-space text) and its
    stats, and adds one branch to `GameRenderer` (a camera-less scene with UI shows its UI over black).
  - game-ui registers the `GPU.UI` stage (`FrameStageKind::GPU`) in `RegisterGameUIModule` and wraps its `Draw` calls
    with this stream's `GPUPassTimer` (`timer.Next("GPU.UI")`, then back to `GPU.Overlays`). Keep `GPUPassTimer`
    usable from a pass that receives the frame's timer (the overlay frame context carries it).
  - The visible-texture list (`VisibleTextureUse`) stays the way game-ui's UI images reach the texture streamer, also
    once (4d)'s feedback drives the scene's textures: UI quads are not proxies, so the GPU never sees them.
- **world-partition** (merges before this stream; what follows is this stream's scope, done in its rebase):
  - its `FramePerfLog` column groups, which this stream builds on;
  - `RenderScene::PrepareLoadedScene`'s `SceneWorkBudget&` parameter;
  - cell loads within `NewInstanceBytesPerFrame`, and `deferredInstanceBytes` draining during a cell drive;
  - the copy hook for a world's play start (`PerfGPU.Editor.PlayStartWorldMaxFrame`);
  - camera-relative instances for its 20 km test (`GPU.World`).
- **authoring-and-playtest:**
  - the Game view's renderer adds into the same GPU stages (like CPU stages);
  - `renderStats` comes from `CommandUtils::DescribeRenderStats` for any view it adds;
  - rest-pose palettes draw through the skinned buckets.
- **engine-modules:** new files land in the per-layer libraries through `Layers.json` patterns. The shader list
  additions go into `strata_add_shaders` in whatever shape engine-modules gives it.
- **Importer versions.** If (7c) lands, bump `GltfImporter`'s version once. authoring-and-playtest bumps it too, and the
  integrator leaves one bump beyond main's.

## Open items this stream owns

- **Rendering audit:** C1, C2, L1, L2, L11, m4; the person audit's GPU timings.
- **Scale audit:** the instance-upload budget, the play start's render part.
- **If time:** m3, L3, L9.
- **Remove from Roadmap's open items:** "culling and drawing skinned proxies on the GPU belongs to `gpu-driven-rendering`".
- **Out of scope, wave 5's render-quality:**
  - TAA and motion vectors;
  - local-light shadows;
  - reflection probes and SSR;
  - fog and atmosphere;
  - sun extraction from HDR skies;
  - cascade blending;
  - transparency and the material model.

  Previous instance transforms stay cheap to add: changed instance ranges are known per upload, so a
  `PreviousInstances` copy of exactly those ranges is one GPU copy. Nothing in this stream may make that harder.
- **Also out of scope:**
  - HLOD and impostors for far fields (w5; screen-size culling is the stopgap);
  - Mesh v2's quantized vertices and per-LOD streaming, which build on this pool.

## Files expected to be touched

- **Renderer:**
  - `Strata/src/Strata/Renderer/`: `SceneRenderer.{h,cpp}`, `SceneRendererExtraction.cpp`, `SceneRenderData.h`,
    `RenderScene.{h,cpp}`, `SpatialIndex.{h,cpp}` (shared pages), `Mesh.{h,cpp}`, `LODSelection.{h,cpp}`,
    `GPUUploadQueue.{h,cpp}`, `Renderer.{h,cpp}`, `RendererRegistration.cpp`, `Texture.{h,cpp}`, `Material.cpp`,
    `TextureStreamer.{h,cpp}` (4d; 7a), `AssetResolveCache.{h,cpp}`, `GraphicsDevice.h`;
  - new: `GPUProfiler`, `GeometryPool`, `RenderSceneGPU`, `TextureTable`, `BufferReadback` (the overlay hook,
    `SceneOverlay.h`, is game-ui's).
- **Platform:** `Strata/src/Platform/Vulkan/VulkanGraphicsDevice.cpp` (timestamp pool, info flags).
- **Core, Scene, Runtime, ImGui:**
  - `Strata/src/Strata/Core/FrameProfiler.{h,cpp}`, `Core/RangeAllocator.{h,cpp}` (new);
  - `Strata/src/Strata/Scene/Scene.{h,cpp}` (the copier registry and its call, function-level);
  - `Strata/src/Strata/Runtime/FramePerfLog.{h,cpp}`, `Runtime/GameRenderer.{h,cpp}` (stats access only);
  - `StrataRuntime/src/RuntimeApplication.cpp` (perf log groups);
  - `Strata/src/Strata/ImGui/ImGuiRenderer.cpp` (the `GPU.ImGui` timer around the UI draw, function-level; engine-modules
    may move the ImGui layer, so follow it).
- **Shaders:**
  - `Strata/shaders/Scene/{Mesh.vert, ShadowMesh.vert, DepthPrepass.frag, Forward.frag, Skybox.frag, LightClusters.comp}`;
  - `Overlay/{Grid.frag, DebugLine.vert, Text.vert}`;
  - `Include/{Scene.glsl, Shadows.glsl, Clusters.glsl}`;
  - new: `Scene/Cull.comp` (+ `ST_CULL_SHADOW`), `CullBuild.comp`, `CullScatter.comp`, `CullPack.comp`, `HZBBuild.comp`,
    `Include/LODSelection.glsl`, `Include/TextureStreaming.glsl`, `Include/Culling.glsl`; optional `Scene/Skin.comp` and
    `Scene/LightCull.comp`;
  - `Strata/CMakeLists.txt` (`strata_add_shaders`).
- **Editor:**
  - `StrataEditor/src/Panels/PerformancePanel.{h,cpp}`;
  - `EditorLayer.cpp` (`DrawFramePill` tooltip only);
  - `Panels/ViewportPanel.cpp` (`DrawStatsOverlay` only);
  - `Editor/EditorStateCommands.cpp` (`editor.perf`, `DescribeFrameProfile`);
  - `Editor/EditorViewportCommands.cpp` (capture result);
  - `Editor/CommandUtils.{h,cpp}` (`DescribeRenderStats`);
  - `Editor/ViewportRenderer.{h,cpp}` (stats accessors);
  - `Editor/StreamingView.cpp` (GPU visibility counts);
  - `Editor/EditorStreamingCommands.cpp` (`asset.stats` `geometryPool`, function-level).
- **Tests:**
  - `StrataTests/src/Renderer/` (new GPU suites above; `GPUAssetTests`, `GPUAssetUploadTests`, `GPUSceneShadowTests`,
    `LODSelectionTests`, `RenderSceneTests`, `SceneRendererTestUtils.h`);
  - `StrataTests/src/Core/{FrameProfilerTests, RangeAllocatorTests}.cpp`;
  - `StrataTests/src/Runtime/FramePerfLogTests.cpp`;
  - `StrataTests/src/Perf/{RendererPerfTests.cpp, SceneGenerators.{h,cpp}}`, `Perf/RendererPerfUtils.h` (new);
  - `StrataTests/src/Editor/` (Performance panel, `editor.perf`);
  - `StrataTests/src/FeatureTest/` (`GPU.FeatureTest` checks);
  - `StrataTests/Perf/Budgets.json`;
  - `StrataTests/Architecture/Layers.json` (patterns for new files, if needed).
- **Docs and skills:** `Docs/Architecture.md`, `AGENTS.md`, `.claude/skills/strata-editor-automation`,
  `.claude/skills/strata-build-test`.
- **If (7c) lands:** `Asset/GltfImporter.cpp` (version) and the mesh corruption tests.

## Merge notes and conflict risks

Merges **fourth**.

- **engine-modules** (first) moves components into their modules, turns layers into libraries and reshapes
  `Application`.
  - Rebase includes mechanically.
  - `MeshRendererComponent` and `SkinComponent` keep their types in `Scene/Components.h` (Renderer and Animation register
    them); lights, sky, post-process and text move to `Renderer/RendererComponents.h`.
  - If engine-modules inverted `AssetMeshProvider -> Mesh.h` through a mesh-geometry provider, the pool keeps CPU
    positions and indices for it.
  - `Application::RunFrame`'s profiler and renderer calls may move into module lifecycle hooks: `GPUProfiler::BeginFrame`
    follows `Renderer::BeginFrame` wherever it lands.
  - New suites (`Core.RangeAllocator`, `GPU.Renderer.Timers`, ...) must fall into a shard; the shard coverage test says
    which.
- **world-io** (second) touches no renderer file.
  - Its `Perf/WorldFixture.h` may host the 1M play-start gate. Use it if it lands first and builds rendered scenes;
    else `CreateCubeGrid`.
  - Its `Editor.Commands` profiler stage is an editor stage (registered on first use); the runtime's perf log never has
    it.
  - It adds a `SceneCopyOptions` overload of `Scene::Copy` for snapshots (`SerializedOnly`): the copier call skips it.
- **world-partition** (third) changes `SceneSubScenes`, `PhysicsWorld` and `GameRuntime`, and lands
  `FramePerfLog`'s column groups.
  - In this stream's rebase (its scope, not the integrator's): check that a cell drive stays within
    `NewInstanceBytesPerFrame` and that `deferredInstanceBytes` drains, run `Perf.World.CellDriveMaxFrame` with GPU
    culling, run `GPU.World`'s 20 km pick camera-relative, and run `PerfGPU.Editor.PlayStartWorldMaxFrame` with
    `CopyIntoScene`.
  - **Conflict risk:** both edit `Scene.cpp`, in different functions. world-partition's play start of a world goes
    through the ordinary `Scene::Copy`, so the copier runs there; any other full-copy path must call the copiers too.
- **game-ui** (fifth) re-applies its own overlay hook (`Renderer/SceneOverlay.h`) on this stream's `SceneRenderer`,
  registers `GPU.UI`, and puts no draw of its own into `SceneRenderer` beyond the hook.
  - `TextRenderer`, `TextLayout` and `Font*` are game-ui's. This stream only wraps the text pass in `GPU.Overlays` and
    subtracts the camera from world-space text vertices: one call site in `SceneRenderer::RenderOverlays`.
- **authoring-and-playtest** (last) owns `ViewportPanel`, `EditorViewport`, `ViewportRenderer` and
  `EditorStateCommands`. This stream's edits there stay function-level: `DrawStatsOverlay`, the `editor.perf` block and
  stats accessors.
  - Its Game view must call `DescribeRenderStats` and add its renderer to the GPU stages (automatic).
  - Its command-script contract changes (`play.step`) do not touch this stream.
  - Its `GltfImporter` bump unions with (7c)'s.
- **Shared files with unions:** `Budgets.json` (new entries in one block), `AGENTS.md` and `Docs/Architecture.md` (own
  sections), the skills, `Layers.json` patterns, and `Strata/CMakeLists.txt`'s shader list.
- **Risks to watch at integration:**
  - **Coplanar surfaces** may flicker between frames on the GPU path, since the instance order within a draw is not
    fixed. The tests avoid coplanar geometry; the report says so.
  - **Platforms:** drivers without `drawIndirectCount` (MoltenVK may be one) run the fallbacks. Linux and macOS are only
    compiled by CI, and the report lists them as untested POSIX paths.
  - **The 2-3 frame feedback latency** could cost `PerfGPU.Streaming.FramesToSettle` (measured 4, budget 6) if camera-cut
    CPU marks miss a case.
  - **Screen-size culling changes far fields:** a dense field of sub-half-pixel cubes thins out instead of aliasing.
    The before and after screenshots decide whether 0.5 px stays the default.

## Questions for the planner (also in the report)

- **High/low instance translations.** The brief asks for high/low pairs. With float32 scene transforms the low parts are
  identically zero, and the exact float subtraction in the shader already gives full precision near the camera. The spec
  keeps `InstanceData` at 64 bytes without them, and says where they would go if transforms become 64-bit.
- **Timestamps.** The brief names NVRHI's timer queries. The spec uses a device timestamp pool instead: NVRHI reads 32-bit
  results that drivers may saturate, spins in `getTimerQueryTime`, and ends render passes.
- **The 5 ms play start.** At 1M it needs the copy-on-write pages of (6); a deep copy is estimated at 15-25 ms. If the pages cost
  `RenderScene`'s hot paths more than their noise, should the budget follow a deep parallel copy instead?
- **Screen-size culling.** Should 0.5 px be the default, which changes the image of dense far fields, or should it be
  opt-in until HLOD (w5)?
- **The overlay hook.** Who defines it: this stream's `SceneOverlayPassRegistry`, or game-ui's?

## Answers from the planner (cross-check of 2026-10-13)

1. **High/low instance translations:** accepted. `InstanceData` stays 64 bytes without low parts while scene transforms
   are float32; the parallel 12-byte buffer is the documented path for 64-bit transforms.
2. **Timestamps:** accepted: the device's own 64-bit timestamp pool, not NVRHI's timer queries, for the reasons given.
3. **The 5 ms play start:** copy-on-write pages only if `PerfGPU.Renderer.Render100kCubes`, `Perf.Scene.*` and
   `RenderScene::Sync` stay within noise with them; a hot-loop regression is not traded for the play start. Otherwise ship
   the deep parallel copy, budget it at 1.5 x measured with 5 ms as its target, and report it.
4. **Screen-size culling:** on by default (0.5 px for views, one texel for cascades), with the before and after
   screenshots of the 1M grid in the report; `MinProjectedPixels` 0 turns it off. Sub-pixel instances add moire and GPU
   time, not information, until HLOD (wave 5).
5. **The overlay hook:** game-ui's `Renderer/SceneOverlay.h`. This stream defines none (see "Interfaces").
6. **`GPU.UI`:** registered by game-ui, which wraps its `Draw` with this stream's `GPUPassTimer` in its rebase.
7. **Stage registration for `--perf-log`:** engine-modules keeps `SceneSystemRegistry::GetAll` and
   `SceneSystemDescriptor::ProfileStage` as they are (it only adds soft constraints), so `FramePerfLog::Create` can
   register every system's stage.
8. **Clone hook placement:** at the end of full `Scene::Copy`s only. world-partition's play start of a world uses the
   ordinary `Scene::Copy`; world-io's `SerializedOnly` snapshot copies run no copiers.
9. **Column groups:** world-partition lands `FramePerfLog`'s column groups in the shape this spec gave (common.md
   section 12); this stream builds on them in its rebase and does not write them again.
