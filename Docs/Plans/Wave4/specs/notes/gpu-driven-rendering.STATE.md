# gpu-driven-rendering spec: state notes

Task: write `wave4/specs/gpu-driven-rendering.md` (format of wave3/specs/animation.md). Read-only on repo; code in
<repo>/.worktrees/verify (main 685aef7).

## Progress
- [x] Read plan.md, common.md, wave3 animation.md (format)
- [x] Audit evidence (rendering STATE.md, perf runtime2/3 summaries, gen_scale.py, gen_unique.py)
- [x] Code read: SceneRenderer.h/.cpp (RenderFrame, DrawBatches, DrawShadows, CollectLights), SceneRendererExtraction.cpp
      (whole), RenderScene.h + Sync, SceneRenderData.h, Mesh.h + Finalize/GetMemoryUsage, GPUUploadQueue.h, FrameProfiler.h,
      FramePerfLog, GameRenderer, RuntimeApplication perf log, editor.perf, PerformancePanel.h, ViewportPanel stats overlay,
      StreamingView.h, LODSelection, SpatialIndex.h, Scene::Copy, VulkanGraphicsDevice features, NVRHI timer/indirect API,
      shaders Mesh.vert/ShadowMesh.vert/Scene.glsl/DepthPrepass.frag/Skybox.frag, Budgets (PerfGPU.Renderer.*), RendererPerfTests
- [ ] Write spec
- [ ] StructuredOutput

## Facts gathered (code)
- NVRHI Vulkan timer queries read 32-bit results (vulkan-queries.cpp pollTimerQuery: uint32_t timestamps[2], no
  VK_QUERY_RESULT_64_BIT; spec lets drivers wrap or saturate) and getTimerQueryTime spins; begin/end call endRenderPass.
  -> design: own timestamp pool in GraphicsDevice (Platform/Vulkan), 64-bit, availability polled, hostQueryReset enabled.
  Precedent: RecordBufferToTextureCopies records raw Vulkan into NVRHI lists (getNativeObject VK_CommandBuffer).
- Device enables multiDrawIndirect, drawIndirectFirstInstance, drawIndirectCount, samplerFilterMinmax, hostQueryReset where
  supported; GraphicsDeviceInfo exposes none of them (add flags).
- NVRHI: drawIndexedIndirect(offset, count), drawIndexedIndirectCount(paramOffset, countOffset, max); GraphicsState
  indirectParams/indirectCountBuffer; BufferDesc isDrawIndirectArgs; SamplerReductionType::Minimum.
- Only SceneRenderer reads mesh GPU buffers (DrawBatches/DrawShadows); tests GPUAssetTests/GPUAssetUploadTests read them.
- Stage registration of scene systems happens at play start (Scene.cpp ~1829) -> perf log created earlier lacks
  Scene.Animation. Fix: register descriptor ProfileStage at SceneSystemRegistry registration.
- FrameProfiler in Core: CPU stages only; FrameSample has StageMs span; listeners at EndFrame.
- InstanceData 64 B: 3x4 rows (translation in .w) + MaterialIndex, EntityID, Flags, SkinPalette. Scene transforms float32:
  instance translation is exact scene value; hi/lo low part would be identically 0 -> keep 64 B, subtract camera in shader.
- RenderScene per-proxy arrays ~250 B/proxy (~250 MB at 1M) -> deep clone can't be <=5 ms; COW pages design.
- Per-renderer GPU copies today (each SceneRenderer uploads instance buffer) -> shared RenderSceneGPU per render scene.
- Budgets: Render100kCubes 0.69/1.04, Render300kCubes 2.26/3.39, Render10kMaterials 1.36/2.03, Skinned1000 0.64/0.96,
  IdleFrame3kStreamedTextures 1.02/1.53, EditFrameCubes100kStreamingView 1.13/1.69, PlayStart1MMaxFrame 48.6/69.8.
- Material data embeds bindless slots that change with mip levels -> GPU path needs texture slot table indirection
  (stable per-texture index, updated from a change log), else stale materials for frames not in feedback.

## Design decisions (so far)
- GPU timers: GPUProfiler (Renderer) + GraphicsDevice timestamp API; chained timestamps per pass boundary; FrameProfiler
  gets GPU stage kind, reported late (latency 2-3 frames), stats over frames with data.
- GPU path flag SceneRendererSpecification::GPUDriven (Auto/Off), IndirectDrawMode for forcing fallbacks in tests.
- Feedback-driven requests replace CPU marks in 4b; CPU coarse pass for meshes without bounds; camera cut fallback.
- GPU mirror per render scene (RenderSceneGPU in a renderer-owned cache keyed by render scene id, NOT registry ctx:
  scenes die on background jobs). Proxy cull data from a new proxy change log; per-instance draw data (geometry submesh,
  proxy) in a parallel 8-byte buffer; InstanceData stays 64 B.
- Texture table indirection (stable index per Texture, entry updated when its bindless slot changes) so material data
  follows content changes only; reverse index texture->materials for loads.
- Transparent proxies: RenderScene users per override material slot + mesh users; renderer marks transparent slots;
  CPU walks only those. Skin proxies stay CPU-tested for ReportVisible.
- Geometry pool: Core/RangeAllocator; arenas Static, Skinned (+skin stream), Index; GeometryHandle stable ids; table
  buffers GeometryEntry/Submesh/LOD; graphics-queue uploads only; defrag by GPU copy.
- Overlay hook for game-ui: SceneOverlayPassRegistry in Renderer (question to planner).
- FramePerfLog::Create registers scene systems' ProfileStage from SceneSystemRegistry::GetAll (no SceneSystem.cpp edit);
  rows written with FrameListenerTiming::WithGPU; FramePerfColumnGroup API for world-partition.
- Screen-size culling default 0.5 px view / 1 texel cascades (question to planner: image change of far fields).
- Clone: COW pages (16,384 proxies) for proxy/instance arrays; GPU mirror cloned by GPU copy.
- Questions: hi/lo instances (deviation), own timestamps vs NVRHI timers, 5 ms clone, screen-size default, overlay hook.

## Next step
DONE: spec written (wave4/specs/gpu-driven-rendering.md, ~60 KB), proofread once. Remaining: StructuredOutput only.
