# Workstream `render-quality`: temporal anti-aliasing, local shadows, reflections, fog, sky and materials (wave 6)

Owner's request (2026-10-22): "Start working on render-quality and project-settings", the two streams the wave-4 audits
ranked first for wave 6. The owner wants "an actual production engine with its own identity: high scaling, modular,
assets streaming optimized", works hands-off (the decisions here are the agent's) and judges the engine by running the
editor, so every feature this stream lands is visible in the editor and in a sample. `project-settings` runs at the same
time in its own worktree; section 12 says what both touch.

AGENTS.md is the source of truth for rules. The machine's rules (from wave 5's specs, still true; the tools and flags
were checked at 6425b57b):

- The Windows perf reference machine is away. On the Mac (Apple M2 Max, MoltenVK 1.4.2), `-L perf` informs and is not
  the gate; existing `StrataTests/Perf/Budgets.json` entries (`Measured`, `Budget`) are never added, raised or lowered
  from Mac numbers; a new metric gets a provisional entry (`{ "Unit": ..., "Provisional": true }`, with its acceptance
  limit as `Target` when it has one in the reference machine's terms), which measures, prints and never fails.
- A/Bs: Release builds, interleaved, at least 3 pairs, every measurement under
  `python3 Tools/PerfLock.py --label render-quality -- <command>`, judged by the ratio of the medians. The base build is
  6425b57b's Release build in `build/baseline-6425b57b/` of this worktree (`/build/` is git-ignored): its sources
  exported there with `git archive 6425b57b`, the vendored sources from the same pins, deleted when the stream lands.
  A criterion in the reference machine's absolute numbers is reported with the Mac value and deferred to the reference
  machine.
- Perf tests this stream adds or changes: `python3 Tools/PerfStability.py --runs 5` runs on the Mac before they merge,
  as information (the Mac sets no budget), and again on the reference machine when it returns (AGENTS.md, Testing).
- Every commit: `python3 Tools/CompatLint.py --changed-since 6425b57b` after a Debug build (GCC 14 and LLVM 16 with its
  libc++, warnings as errors) comes back clean; CI's GCC 14.2 at `-O3` (the release workflow's Dist builds) must stay
  warning-free too. A changed header lints the files that include it (pass them, or `--all`).
- The stream builds in its own tree inside its worktree (`cmake --preset macos` there: `build/macos` of the worktree,
  which `CompatLint.py` uses by default). The worktree's submodules are initialized before the first build
  (`git submodule update --init --recursive --depth 1`); until then vendor sources are read from the main checkout's
  `Strata/vendor` (the same pins).
- CI never runs GPU tests (`.github/workflows/ci.yml` excludes `gpu|perf`): every `GPU*` suite of this stream is run
  locally on MoltenVK under the validation layer before each commit, and Windows and Linux GPUs see this code only on
  the reference machine later (risk, section 9).

## 1. Goal

At the end of the stream, in this order of priority:

- **Temporal anti-aliasing** with per-pixel motion vectors: a `PostProcessComponent` chooses FXAA or TAA; the jitter,
  the per-renderer history and the motion of cameras, moving entities and skinned poses are exact on both draw paths;
  the editor's selection outline, captures and thumbnails (the project's picture included) stay steady. GTAO and PCSS
  noise are accumulated over frames.
- **Cascade blending**: no visible seam between directional shadow cascades, and shadows fade out before
  `ShadowDistance` instead of stopping.
- **Height fog**.
- **Documented light units**: lux, candela and nits stated on every light, the sky and emission, in tooltips,
  descriptions, a Developer Guide section and a manual page.
- **Shadows of point and spot lights**: an atlas, a per-frame redraw budget and a cache like the cascades', decided on
  both draw paths.
- Below the cut line (sections 3 and 11), in this order: a **pre-exposed HDR target with a physical atmosphere** for
  the procedural sky (real values such as a 100,000 lux sun render without overflowing half floats), **reflection
  probes** baked in the editor, **screen-space reflections**, **extracting the sun from HDR skies**, and **clearcoat and
  transmission** in the material model and the glTF importer.
- The samples use them (section 6.13): the Open World gets TAA, fog, shadowed local lights (a lantern lit at all times
  near the start, and the campfires at night) and the atmosphere; the Third-Person Adventure TAA, shadowed lanterns in
  shade, reflection probes, SSR and its sun extracted from its sky; the Physics Playground TAA and clearcoat and glass
  materials.
- Every feature costs nothing when it is off (no target, no pass, no shader variant bound), and existing scenes render
  as in 0.5.0 unless they opt in. Three defaults change deliberately, each in its step and named in the release notes:
  cascade blending is on (D6, S3), because the seam it removes is a defect; from S7 the HDR target is pre-exposed by a
  power of two (D7), which changes no image unless a value would overflow or become subnormal; and from S7
  `PostProcessComponent::AutoExposureMaxEV` defaults to 16 instead of 12 (D7), which changes scenes without a
  `Post Process`, and what `component.add` and the template produce, only where the metered average luminance exceeds
  2^12 nits.
- The script ABI is unchanged (`ST_SCRIPT_ABI_VERSION` stays 1, no host function is added): scripts reach every new
  setting through `Entity::GetProperty`/`SetProperty` (`StrataScriptCore/Include/StrataScript/Entity.h:70-72`).

Not the goal: render scaling or upscaling, MSAA, volumetric fog, area lights, runtime probe capture, quality presets
(`game-shell` maps them to the renderer later), project-wide renderer defaults (section 12), sheen and the other glTF
material extensions (section 10).

## 2. Where we start (6425b57b, release 0.5.0)

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `DG` is `Docs/DeveloperGuide.md`.

### 2.1 Files

| File | Lines | Role |
| --- | --- | --- |
| `R/SceneRenderer.h` / `.cpp` | 1,334 / 2,670 | Specification, options, stats, private state; targets, post-processing, `RenderFrame` (1868-) |
| `R/SceneRendererExtraction.cpp` | 1,282 | CPU culling, `FindShadowCasters` (412-), `GatherShadowDraws` (1075-) |
| `R/SceneRendererGPUDriven.cpp` | 1,359 | GPU culling, depth pyramids, `PrepareShadowCache` (1126-), `CommitShadowCache` |
| `R/SceneRenderData.h` | 337 | `FrameConstants`, `InstanceData`, `MaterialData`, `LightData`, `LightRecord` |
| `R/GPUSceneData.h` | 224 | Culling layouts: buckets, `CullConstants`, `CullCounters`, `LODFadeState` |
| `R/RenderScene.h` / `.cpp` | 551 / 1,629 | Per-scene proxies and change logs (instances, proxies, palettes) |
| `R/RenderSceneGPU.cpp` | 863 | The per-scene GPU mirror, shared by the scene's renderers |
| `R/RendererComponents.h` / `.cpp` | 129 / 167 | Lights, `SkyLightComponent`, `PostProcessComponent`, `TextComponent` and their registration |
| `R/Material.h`, `R/MaterialData.h` / `.cpp` | 83, 39 / 47 | `MaterialProperties`; `MaterialGPU::Build`, `GetFlags`, `GetTextureMaps` |
| `R/GPUProfiler.h` / `.cpp` | 193 / 375 | `RendererGPUStages`, `GPUPassTimer` |
| `Strata/src/Strata/Asset/GltfImporter.cpp` | 2,202 | glTF import (`GetVersion()` 9) |
| `Strata/src/Strata/Asset/TextureImporter.cpp` | 647 | Texture import (`GetVersion()` 5), the HDR path at 517-547, settings at 348-372 |
| `SH/Scene/Forward.frag`, `Cull.comp` | 164, 535 | Forward shading; GPU culling |
| `SH/Include/Scene.glsl`, `Shadows.glsl`, `Culling.glsl` | 293, 123, 260 | Scene bindings and layouts; PCSS; culling bindings |

### 2.2 The frame (`SceneRenderer::RenderFrame`, `R/SceneRenderer.cpp:1868-2277`)

1. Extract on the CPU: `RenderScene::Sync`, the `RenderSceneGPU` upload, `FrameConstants` (1921-1933: `View` is
   rotation-only, `Projection = camera.Projection`, `ViewProjection` and the three inverses), `CollectLights` (1941),
   the camera-cut check (`IsCameraCut`, 1947: `m_CutRender`), culling, `FindShadowCasters` (1965), `EnsureShadowMap`
   (1972).
2. GPU stages (`GPUPassTimer` from 2021), registered in this order by `RendererGPUStages::Get`
   (`R/GPUProfiler.cpp:44-63`): `GPU.Uploads`, `Setup` (`UpdateEnvironment` 2022, the frame constants, lights,
   clusters), `Cull` (`PrepareShadowCache` 2052, GPU culling), `Shadows`, `Prepass` (clears at 2101-2105, the GPU path's
   two occlusion phases at 2110-2116), `AO` (2123-2129, only with `PostProcessComponent::AmbientOcclusion`), `Opaque`
   (EQUAL depth, then the sky as a fullscreen triangle at depth 0, 2142-2162), `Transparent` (2163-2166, CPU-sorted on
   both paths), `Post` (`UpdateExposure` 2170, `RenderBloom`, tonemap into `m_LDRFramebuffer` when `AntiAliasing`, then
   FXAA, 2236-2246), `Overlays` (grid, tints, outline, lines, text, the copy into a target). `CommitShadowCache` at
   2270.
3. Render targets (`CreateRenderTargets`, viewport-sized): D32 depth, RG16F octahedral **view-space geometric** normals
   (`SH/Scene/DepthPrepass.frag`: no normal map), R32_UINT entity IDs (only with
   `SceneRendererSpecification::EntityIDs`), RGBA16F HDR (cleared every frame to the scene's clear color, 2105: no
   history), RGBA8 LDR, AO, half-resolution bloom. Targets are released through `Renderer::ReleaseDeferred`; the
   exposure, bloom and tonemap binding sets bind `m_HDRTexture` directly.
4. The shadow map is a D32 2D array of `c_MaxCascades` = 4 layers of `ShadowMapSize` (2048), one framebuffer per slice
   (`EnsureShadowMap`, 1002-1036).
5. Overlays draw with the unjittered camera (`PrepareOverlayPasses`, 2408-2415: `frame.ViewProjection` from
   `camera.Projection`) and depth-test against the frame's depth; world-space text is "hidden behind geometry"
   (`R/RendererComponents.h:116`).

Gaps: no velocity target, no previous view-projection, no jitter, no per-renderer previous transforms or palettes, no
HDR history, no TAA. `PostProcessComponent::AntiAliasing` is a `bool` meaning FXAA (`R/RendererComponents.h:100`). The
exposure lives only on the GPU (`m_ExposureBuffer`, `R/SceneRenderer.h:1075`): nothing reads it back (the only buffer
readbacks are the cull counters', `CounterReadback`, 1233-1247).

### 2.3 Instances, palettes and the views that render them

- `InstanceData` is 64 bytes (`World` affine transform, `MaterialIndex`, `EntityID`, `Flags`, `SkinPalette`; static
  assert at `R/SceneRenderData.h:130`); additions go to parallel buffers (AGENTS.md, Rendering).
- `RenderSceneGPU` is shared by every renderer of a scene and overwrites its instance and palette buffers in place from
  `RenderScene`'s change logs (`GetInstanceChanges`, `GetPaletteChanges`, `GetProxyChanges`; each returns false when
  more than its cap changed since a version). Palette ranges are reallocated when skins change.
- The renderers that exist at once: the runtime's `GameRenderer` ("Game", `EntityIDs` off); the editor's three
  `ViewportRenderer`s "Viewport", "GameView" and "ViewportCapture"
  (`StrataEditor/src/Editor/EditorViewport.cpp:319-339`, all with entity IDs; the capture renderer has fades off and
  re-renders over several polls); `ThumbnailCache`'s "Thumbnails" (`ThumbnailCache.cpp:854-863`: 1024 shadow map, one
  cascade, fades off, shared across scenes and sizes). The capture renderer has two users: `viewport.capture` and
  `ProjectThumbnail` (`StrataEditor/src/Editor/ProjectThumbnail.cpp:111-146`), which renders the project's picture with
  its own completeness check (pending assets, glyphs, deferred proxies, textures below their wanted level) and nothing
  else. Both release the targets after each image (`EditorViewportCommands.cpp:341`, `ProjectThumbnail.cpp:148`), so
  every capture starts cold. Captures wait for text, assets, mips, poses and instances, each with a frame bound
  (`EditorViewportCommands.cpp:37-53, 326-339`). In Play the Viewport and the GameView render the same `RenderScene` at
  different times, and hidden views skip frames. The device has no frame number a renderer could compare with (only
  each renderer's `m_RenderCount`).
- "Instance history" today is the GPU path's visibility bits (alternating parity, `CullConstants::VisibilityParity`)
  and the per-instance `LODFadeState`, which keeps the `EntityID` it was drawn for because "an instance slot may change
  hands" (`R/GPUSceneData.h:71-77`).
- The only previous-frame camera state is `m_PreviousCameraPosition`/`Forward` (floats, `R/SceneRenderer.h:954-956`),
  for camera-cut detection and texture streaming; `SceneCamera::Position` is a float `vec3`.
- The shadow cache key holds the whole `SceneCamera`, `Projection` included (`ShadowCacheKey`,
  `R/SceneRenderer.h:1287-1297`): a jitter put into `SceneCamera::Projection` would redraw every cascade every frame.

### 2.4 Lights, shadows and units

- `DirectionalLightComponent` (color, `Intensity` 3, `CastShadows`, `ShadowDistance` 80, `ShadowSoftness` (the light's
  angular diameter in degrees, `R/RendererComponents.h:23`), `ShadowBias`, `ShadowNormalBias`); `PointLightComponent`
  (intensity 10, `Range` 10) and `SpotLightComponent` (intensity 20, range 15, half angles 20/30) have no shadow fields:
  "Point and spot lights cast no shadows" (`R/RendererComponents.h:28`, and their registered descriptions). Only the
  directional light's `Shadow*` properties lack an inactive reason today (`RendererComponents.cpp:121-123, 150-152` set
  them for the sky and post-processing).
- Units: DG "Scene renderer and lighting" calls them "relative but physically consistent". The shaders compute a
  directional light as `color * intensity * NoL * BRDF` (illuminance), a punctual one as `color * intensity` times
  windowed `1/d^2` (`SH/Include/PBR.glsl`: luminous intensity), spot lights without cone normalization, sky radiance
  times `SkyLightComponent::Intensity`; the glTF importer reads `KHR_lights_punctual` 1:1. So one scale links nits, lux
  and candela, but nothing states it. Manual exposure is a multiplier of `2^Exposure`, registered within [-16, 16] EV;
  `AutoExposureMinEV`/`MaxEV` are registered within [-12, 16] (`RendererComponents.cpp:131-136`; ranges are enforced in
  files, commands and scripts). Automatic exposure meters a log2-luminance histogram over [-12, 16] in 254 bins of
  0.11 EV and uses bin centers (`SH/Include/Exposure.glsl:10-36`); `SanitizeHDR` clamps at 65,000. A 100,000 lux sun
  on a white surface gives about 32,000 nits: one step from overflow in RGBA16F, and a 0.53 degree sun disk of real
  radiance cannot be stored at all (at the half-float maximum it carries about 4.4 lux).
- Point and spot lights: `RenderLights` records (80-byte `LightRecord`, `SpotAngles.w` unused), culled on the CPU path
  (`CollectLights`, capped at `MaxLights` 4096) or by `LightCull.comp`/`LightCompact.comp` on the GPU path, which writes
  zeros into `LightData::SpotAngles.zw` (`SH/Scene/LightCompact.comp:87`; `LightData` is 64 bytes); 16x9x24 clusters.
- Cascades: splits blend 0.8 logarithmic and 0.2 uniform; each square covers its slice's bounding sphere, snapped to
  texels in double; `frame.CascadeSplits[c]` is the slice's far distance; `CascadeFit.comp` fits depth on the GPU on
  both paths; `SelectCascade` (`SH/Include/Shadows.glsl:31-40`) switches hard, and a pixel beyond the last split is lit.
  `FrameConstants::ShadowParams.z` is unused (`SceneRendererExtraction.cpp:1104` writes 0). PCSS uses 16 + 16 taps
  rotated by interleaved gradient noise that does not change between frames.
- CPU culling queries `RenderScene`'s `SpatialIndex` (a loose two-level grid, `R/SpatialIndex.h`: `QueryFrustum`,
  `QuerySphere`, `QueryPlanes`); the cascades query it with their planes (`SceneRendererExtraction.cpp:545-560`).
- GPU culling buckets: `c_CullViewBuckets` 32, `c_CullShadowBucketsPerCascade` 64, `c_CullMaxCascades` 4
  (`R/GPUSceneData.h:119-122`); `Culling.glsl` declares 22 storage buffers (MoltenVK allows 31 per stage). The GPU
  path's counts arrive frames later: `SceneRendererStats` then counts only what the CPU drew
  (`R/SceneRenderer.h:285-288`), and tests compare through `Tests::GetDrawnCounts` (`DrawnCounts::ShadowCasters`,
  `ShadowDraws`, `SceneRendererTestUtils.h:186-199`).

### 2.5 Sky, environment and materials

- `SkyLightComponent::Source` is `EnvironmentMap` or `Procedural` (a `uint8_t` enum registered as names). The
  procedural cube is regenerated when `ProceduralSkyParameters` (96 bytes of push constants; NVRHI's
  `c_MaxPushConstantSize` is 128) differ exactly, and then the whole filter chain runs (512 cube, irradiance, 6
  prefiltered mips at 512 samples). The Open World's `DayNight` script turns the sun in steps
  (`Samples/OpenWorld/Scripts/DayNight.h:1-4`) and drives the sky's colors and the campfires' `PointLight` intensity
  through `SetProperty`.
- One environment is bound (`SH/Include/Scene.glsl:220-223`, SRV 3, 4, 6). `SceneRenderer::Render` into a target needs
  a non-sRGB UNORM format and returns the tone-mapped image; the HDR target is reachable as `GetHDRTexture()` only.
- The renderer resolves assets through the frame's `AssetResolveCache`, whose references end with the frame; two caches
  keep assets across frames (DG "Render scene", 2441-2446): the environment cache, which holds the sky's map whole
  (`TextureStreamer::Hold`) until its maps are computed, and the glyph atlases.
- `.hdr` files import as `TextureUsage::HDR` (RGBA16F, never compressed); `stbi_loadf_from_memory` decodes full floats,
  then `TextureCooker::CookHDRLevels` clamps to [0, 65504]. Cooked textures have no metadata field (`R/CookedTexture.h`,
  `c_CookedTextureVersion` 2); `ImportResult` has none either (`Asset/AssetImporter.h:47-56`). Textures are 2D only (no
  cube or array assets). Import settings are described one entry each in `GetSettingsProperties`, matching
  `GetDefaultSettings`, with `GetInactiveReason` for settings others can leave without effect (`Usage` is one of them);
  `asset.setImportSettings` validates against them (DG 853-858).
- `MaterialProperties`: metallic-roughness factors and five maps; `.stmat` JSON at format version 1, read and written
  through `GetPropertyInfos` (an unknown key only warns). `RenderData::MaterialData` is 96 bytes with one spare `uint`;
  the five-map count is hard-coded in `MaterialGPU::c_TextureMapCount`, `RenderSceneGPU.h:217-218`,
  `RenderSceneGPU.cpp:35, 255, 342, 391`, `StrataEditor/src/UI/MaterialEditor.h:50` / `.cpp:42` and
  `StrataEditor/src/Editor/AssetEdit.cpp:285-287` (`AssetEdit.h:31`). "Transparent" means `AlphaMode::Blend`, routed to
  the CPU-sorted pass by `c_CullMaterialTransparent`; alpha-blended surfaces are not in the prepass, so they have no
  entity IDs and no selection outline (DG 2666).
- `GltfImporter`'s `c_SupportedExtensions` (`GltfImporter.cpp:48-51`) has no material extension beyond emissive
  strength and unlit; the vendored cgltf parses `has_clearcoat`, `has_transmission`, `has_volume`, `has_ior`
  (`cgltf.h:544-547`).

### 2.6 Device, bindings and stages

- Features: device selection rejects devices without a required feature, with the reason
  (`VulkanGraphicsDevice.cpp:626-650`: dynamic rendering, synchronization2, descriptor indexing, anisotropic filtering),
  then enables the optional ones that exist (766-790): `imageCubeArray` when present (782) without a
  `GraphicsDeviceInfo` flag; `shaderStorageImageExtendedFormats` as `SupportsExtendedStorageFormats` (half-resolution AO
  falls back without it, `SceneRenderer.cpp:394-397`); `shaderOutputLayer` and multiview are never enabled. `vulkaninfo`
  on this Mac: `imageCubeArray` and `shaderOutputLayer` true, no geometry shaders, no `drawIndirectCount` (so the GPU
  path draws through `IndirectDrawMode` `NoCount` and `PerDraw` here, never the count path), no `samplerFilterMinmax`;
  `maxPerStageDescriptorSamplers` 16, `maxPerStageDescriptorStorageBuffers` 31, `maxComputeSharedMemorySize` 32,768,
  `maxColorAttachments` 8; RG16F, RGBA16F, B10G11R11 and R32F are filterable color attachments and storage images.
- Every scene pipeline uses one scene binding layout (`SceneRenderer.cpp:255-277`). The scene binding set (851-869)
  uses SRV 0-7, 11, 12, 14-17 and samplers 0-5 (material samplers), 8, 9, 10: nine of MoltenVK's 16 samplers per stage,
  eight storage buffers (SRV 0, 1, 2, 12, 14-17). Free scene SRV slots: 8, 9, 10, 13 and 18 up (18 is the light
  records' slot only in the light-culling layout, `SH/Include/Lights.glsl:32`). The shadow pass binds the same layout
  with a dummy shadow map (`m_DummyShadowMap`, 847-848: "never the shadow map"), so it never samples what it renders
  into.
- The prepass and the forward pass draw with the same vertex module (`m_MeshVertexShader` or
  `m_SkinnedMeshVertexShader`, 1493-1495; `SH/Scene/Mesh.vert:27` declares `invariant gl_Position`), so the forward
  pass's EQUAL test meets the prepass's depth to the bit (DG "Skinned meshes", 2594-2596).
- Shader variants are `VARIANTS "src|name|DEFINE[,DEFINE...]"` in `strata_add_shaders`
  (`Strata/CMakeLists.txt:117-125`, `CMake/StrataShaders.cmake:6`); the prepass has a `NoEntityIDs` variant, the mesh
  vertex shader a `Skinned` one. `GetScenePipeline`'s `PipelineKey` {Pass, DoubleSided, Mirrored, Skinned} does not
  include the attachments.
- A new GPU stage also edits three hard-coded lists: `StrataEditor/src/Editor/EditorStateCommands.cpp:867` (the
  `editor.perf` description), `StrataTests/src/Editor/EditorProfilerTests.cpp:91` and
  `StrataTests/src/Runtime/FramePerfLogTests.cpp:284`.

### 2.7 Tests, feature test, samples and editor

- `GPU.SceneRenderer.GPUDriven` (`GPUSceneRendererGPUDrivenTests.cpp`, 964 lines) compares a CPU-path renderer with a
  GPU-path one over nine scene builders: colors within 1/255, identical entity IDs, equal instance, draw, triangle,
  skinned, caster and shadow-draw counts; one case runs the indirect fallbacks (582). Its scenes have no
  `PostProcessComponent`, so they run the defaults (ACES, auto exposure, bloom, GTAO, FXAA).
  `GPU.SceneRenderer.Feedback` checks that GPU feedback requests what the CPU walk requests. `AddNeutralPostProcess`
  (`SceneRendererTestUtils.h:126-137`) turns off every effect for exact-pixel tests. No suite holds reference images
  (none can render an older build).
- Other suites this stream extends: `GPU.SceneRenderer.Shadows` (cascade caching at 385), `.Lighting`, `.PostProcess`,
  `.CameraRelative`, `.Overlays`, `GPU.ProceduralSky`, `GPU.FeatureTest` (4 frames, `GPUFeatureTests.cpp:35`),
  `GPU.Editor.Viewport`, `GPU.Editor.Thumbnails` (the project thumbnail at 452), `Renderer.Material`, `Asset.Gltf`,
  `Asset.TextureImport`, `Editor.ImportSettings`. Components are tested as the strata-add-component skill says
  (section 4): `Reflection` and `Scene.InactiveProperties` (`StrataTests/src/Scene/ReflectionTests.cpp`),
  `Scene.Serialization` (round trips; out-of-range values in files are clamped, values of another type warn),
  `Editor.Commands` (`component.add` with values, `component.set`, undo); `component.get` reports inactive reasons
  (`inactive`).
  Perf: `PerfGPU.Renderer` (`RendererPerfTests.cpp`, CPU medians and `...GPU` busy medians;
  `MeasuredRendererSpecification` turns `CacheShadowCascades` off), the measured `EditFrame*`, `Render100kCubes`,
  `Render10kMaterials`, `Render300kCubes`, `Skinned1000` entries, and the provisional shadowed and Open World tour
  entries. Every measured scene has a shadowed sun (`AddCameraAndLight`,
  `StrataTests/src/Perf/SceneGenerators.cpp:127-138`).
- The feature test requires every registered component in `Feature.stscene` (5,384 lines) with every property at a
  non-default value on some owner (`FeatureTests.cpp`); `c_ExpectedLogMessages` is at `FeatureTestUtils.cpp:38`.
  Material properties are not enforced; golden rule 6 still covers them (`Feature.stmat`). `Feature.stscene` holds a
  `PostProcess` with `AntiAliasing` true and one with false (257, 314).
- Samples: each 3D scene has one shadowed sun, one sky and one `Post Process` with the defaults (FXAA,
  `AutoExposureMaxEV` 12 stored): the Third-Person's `MainMenu` (a 3D island behind the title) and `Course`, the Physics
  Playground's `Main`, the Open World's `Island`. Only the Open World has point lights: five campfires in
  `Island/Landmarks` cells, stored with `Intensity` 0 and lit only at dusk by the `DayNight` script in Play (the day
  starts at 9:00 and lasts 600 s, so they light about 225 s in). Pixel checks are `StrataRuntime.<Sample>WindowedImage`
  (`check-image`: the Open World requires `--dominant green 5`, `StrataTests/CMakeLists.txt:792-794`). Perf-log checks
  read the windowed runs' `Windowed.csv` (`check-perf-log`; `--nonzero` passes when any row is above zero):
  `StrataRuntime.PhysicsPlaygroundWindowedPerf` (748-750, `--nonzero Scene.Physics`), `OpenWorldWindowedPerf` (795-798:
  world cells, streaming, `GPUBusyMs`, physics; its run is 1,200 frames, about 20 s from 9:00, so no campfire is lit)
  and `ThirdPersonWindowedPerf` (848-851: animation, UI, `GPUBusyMs`; its run is 300 frames of the main menu). The
  runtime's own perf-log columns are world and physics counts (`GameRuntime::MakePerfLogColumns`). The regressions are
  `StrataTests/Editor/<Sample>Commands.json.in`; the Basic 3D template makes its `Post Process` in
  `StrataEditor/src/Editor/ProjectTemplates.cpp:69`.
- Editor: `ViewportDebugView` {None, Streaming, LOD, Mip} are entity tints (`StreamingView.h:26-36`), and the
  `debugView` of `viewport.capture` and `viewport.setSettings` is "none" | "streaming" | "lod" | "mip"
  (`DebugViewSchema`, `EditorViewportCommands.cpp:117`; DG "View commands", 2234). The Inspector's scene sections are
  `DrawSceneSettings` (1237), `DrawCollisionLayers` (1339) and `DrawSky` (1497,
  `StrataEditor/src/Panels/InspectorPanel.cpp`); `ThumbnailCache::c_Version` is 3. Commands that exist:
  `asset.setImportSettings`, `asset.reimport`, `material.*`, `viewport.capture`/`getSettings`/`setSettings`;
  `prefab.apply` refuses while playing because it writes from the edited scene (`EditorPrefabCommands.cpp:440`). Every
  command's kind and deferral are listed in `Editor.Journal` (`StrataTests/src/Editor/CommandJournalTests.cpp`:
  `c_EditCommands` 102, `c_DeferringCommands` 168; a command missing from them fails). MCP toolsets map prefixes in
  `StrataCLI/src/CLI/McpToolsets.cpp` (no `lighting.` prefix yet; `core` is full), tested by
  `EditorMcpToolsetsTests.cpp`.
- Files: new asset files reach a project through `EditorAssetManager::CreateNativeAsset` (native documents only:
  `material.create`) or `ImportExternal` (a copy on an I/O job and a background import, " (n)" appended when a name is
  taken, polled: `asset.import`); `AssetCreateAction` and `AssetTrashAction` (`Editor/AssetFileAction.h`) undo and redo
  them and check the files first; asset documents change through `Editor/AssetEdit` (`AssetDocumentAction`, the bytes
  before and after); asset files are never overwritten or lost (DG "Asset files are never lost", 1622-1634). Entity
  names are unique only among siblings, and only for copies, pastes and instances (DG 1753).
- `AGENTS.md` is 39,622 code points (39,638 bytes) of the 40,000 code points `Docs.AgentGuide` allows
  (`AgentGuideTests.cpp:25`): 378 are free.

## 3. Order and the cut line

Features are ordered by dependency first, then by what the owner sees, then by size:

1. Motion vectors, then TAA together with its editor integration: SSR, GTAO and PCSS accumulation reuse the jitter and
   the history; TAA is the most visible change in every sample, and no commit lets a scene choose it before the
   selection outline, captures and thumbnails handle it (S2 registers the setting in its last commits).
2. Cascade blending: small, independent, visible in every sample.
3. Height fog: small, visible in the Open World.
4. Light units, documented: tooltips, descriptions, the guide and a manual page; nothing is rescaled.
5. Point and spot shadows: the largest piece of the core, last in it, so that a short week still lands the small
   visible steps.
6. **Cut line.** Below it, each step may move to wave 7 if the usage limit runs out, in this order: pre-exposure with
   the atmosphere (the atmosphere is the first producer of real values, so the pre-exposure lands with what needs it;
   Open World), reflection probes (Third-Person), SSR (needs TAA and benefits from probes), sun extraction (needs the
   pre-exposure), clearcoat and transmission.

## 4. Where each setting lives

| Feature | Kind | Settings | Inspector | Script reach |
| --- | --- | --- | --- | --- |
| TAA | post-process | `PostProcessComponent::AntiAliasingMethod` {FXAA, TAA}; `TAAPreset` {Sharp, Original, Custom} and 10 tunables (D17) | generic; inactive while `AntiAliasing` is off; tunables inactive unless Custom; `TAAPreset` has a row drawer | `SetProperty("PostProcess", ...)` |
| Motion vectors | renderer | none (made when a consumer needs them); viewport debug view `motion` (`SceneRenderOptions::ShowMotionVectors`) | Viewport debug chip | none |
| Cascade blending | directional light | `DirectionalLightComponent::CascadeBlend` | generic; inactive without shadows | generic |
| Units | documentation, tooltips | none new | tooltips name the unit | none |
| Pre-exposure | renderer | none; `PostProcessComponent::AutoExposureMaxEV` default 16 | none | none |
| Local shadows | light | `CastShadows`, `ShadowBias`, `ShadowNormalBias`, `ShadowFilterRadius` on point and spot lights | generic; inactive reasons | generic |
| Height fog | new component `HeightFog` (first active one applies) | 8 properties (D9) | generic; "Add Height Fog" in the Sky section | generic |
| Atmosphere | sky source | `SkyLightSource::Atmosphere` + 3 properties | generic; inactive reasons | generic |
| Reflection probes | new component `ReflectionProbe` | 6 properties (D11); `lighting.bakeProbes` | generic + a Bake button | generic (not the bake) |
| SSR | post-process | `ScreenSpaceReflections`, `ScreenSpaceReflectionsIntensity`, `...MaxRoughness` | generic; inactive without TAA | generic |
| Sun extraction | command + import setting | `lighting.extractSun`; `TextureImportSettings::RemoveSun` | Sky section button; asset inspector | none |
| Clearcoat, transmission | material | 11 properties (D14) | material inspector | none (materials are assets) |
| Budgets per program | renderer specification | atlas size, shadowed lights, probe slots, `TemporalAntiAliasing`, `SpreadLightingUpdates` | none | none |

Scene settings (`SceneSettings`) get nothing: rendering settings stay on entities, as the sky and post-processing are,
so world cells, prefabs and the generic commands carry them. The per-program budgets are each program's choice (the
runtime's `GameRenderer`, the editor's renderers); no project-wide renderer setting is added in wave 6 (section 12).

## 5. Design decisions

**D1 One stream-wide rule: off costs nothing.** A feature whose setting is off allocates no target, runs no pass, binds
no new shader variant and adds no per-frame CPU work beyond reading its setting. No test can render 6425b57b, so each
step proves it inside one build:
- Resources: with the feature off its getters return null (`GetVelocityTexture()`, the history, the atlas, the probe
  array), its GPU stage records no time, and the frame's draw and dispatch counts (`SceneRendererStats`,
  `Tests::GetDrawnCounts`) equal a renderer's that never saw the feature.
- Images: a scene without the feature's component or setting renders bit-identical to the same scene with it present
  but off or neutral (a disabled `HeightFog` and one of `Density` 0; FXAA as `AntiAliasingMethod` and a `PostProcess`
  saved without the property; requesting motion vectors and not requesting them), as exact-pixel tests (texture
  streaming off, `AddNeutralPostProcess`). From S3 on, such comparisons of shadowed scenes set `CascadeBlend` 0; from S7
  on they force one pre-exposure on both sides.

The comparison with 6425b57b itself is the manual A/B of 7.1: the baseline and the branch's Release builds capture the
same scenes through `viewport.capture` (a headless editor driven by StrataCLI, which both builds have), a script in the
worktree's scratch area compares the images, and the step's commit states the result.

**D2 Motion vectors from per-renderer copies of the previous frame.**
- Each `SceneRenderer` that needs motion keeps `m_PreviousInstances` (the `InstanceData` layout, indexed by instance
  slot) and `m_PreviousPalette` (the palette buffer's layout): the shared data as this renderer last drew it. They are
  made with the first frame that has motion and released with the targets; they cost 64 bytes per instance slot (64 MiB
  for a million instances) per renderer with motion, which DG states. At the end of each frame a compute pass copies,
  from the shared buffers, the slots and palette ranges `RenderSceneGPU` wrote since the renderer's previous frame (as
  built in S1: its write log, `GetInstanceWrites`/`GetPaletteWrites` by a process-unique write version, bounded by the
  buffer's elements, rather than the render scene's change logs, which miss deferred and retried uploads); when the log
  does not reach back, for a new history or another render scene, it copies everything; a history that grows keeps its
  contents by a GPU copy. Steady state costs nothing. The copy is timed in `GPU.Post` in S1 and in `GPU.TAA` from S2
  (D15).
- `Mesh.vert` gains a velocity variant (`ST_VELOCITY`, with and without `ST_SKINNED`): it reads the previous record,
  and when its `EntityID` differs from the current one (a reused slot, a new entity) or its `SkinPalette` differs (a
  moved palette range) it uses the current transform or palette, so new and re-skinned instances move only with the
  camera. When the frame has motion, the prepass **and the forward pass** draw with the velocity module (the forward
  pass ignores its extra outputs): the two passes keep one vertex module, so the EQUAL test keeps its bit-exact depths
  (2.6). `PipelineKey` gains a `Velocity` bit for both passes. The transparent pass, which has no EQUAL test, keeps the
  plain module; the shadow passes keep theirs.
- The prepass's velocity fragment variant writes an RG16F target (unjittered current clip position minus previous clip
  position, in UV units) as one more color attachment; the prepass framebuffer gains the attachment. Half floats keep
  11 significant bits, so a motion is stored to about 2^-11 of its length (0.06 px of a 120 px motion at 1920 px).
  Both draw paths and both occlusion phases use the same shaders and framebuffer, so they write the same motion.
- The previous buffers are bound at scene-set SRV 20 and 21 (D16); a frame without motion binds the shared instance
  and palette buffers there (no allocation).
- The copy also takes the slots `RenderSceneGPU` uploaded in the frame without a logged change (new proxies whose
  upload `NewInstanceBytesPerFrame` deferred), so a deferred instance's previous record is its own from its first drawn
  frame (the write log covers them).
- `FrameConstants` gains `UnjitteredViewProjection`, `PreviousViewProjection` (previous projection times previous
  rotation-only view times the translation from the previous to the current camera position, built in double: it maps a
  position relative to the current camera to the previous clip space), `JitterParams` (current and previous jitter in
  clip units) and `TemporalParams` (the frame index for noise, the pre-exposure of D7 from S7, flags). Both declarations
  and the `static_assert` change together. As built in S1: `TemporalParams.x` is the flag "the history holds the
  previous frame" (`SceneRendererStats::MotionHistory`), and `JitterParams` is added by S2 with the jitter itself (S2 adds
  the frame index to `TemporalParams` too). As built in S2: `JitterParams` holds this frame's jitter (xy) and the
  previous TAA frame's (zw) in clip units; `TemporalParams.y` stays free for S7's pre-exposure; `TemporalParams.z` is the
  per-pixel noise's turn, the golden ratio's fraction of a turn per frame since the history's reset (0 without TAA), which
  later per-frame noise (SSR's start offsets) reuses; the TAA resolve's own compute layout binds its images at SRV 22-26,
  above every scene-set slot of D16.
- The sky has no geometry: the resolve derives its motion from the camera alone (direction at depth 0).
- Rejected: a previous transform in `InstanceData` (the 64-byte rule); previous data in `RenderSceneGPU` (shared by
  renderers that render at different times, so "previous" differs per renderer); velocity written always (costs every
  scene); a separate velocity pass (a second geometry pass); a velocity module for the prepass only (two modules may
  round `gl_Position` differently despite `invariant`, leaving EQUAL-test holes); the velocity code in `Mesh.vert`
  behind a uniform branch (one module for all, but every scene would pay its outputs).

**D3 Jitter only in the frame's copy of the camera.** The jitter is an 8-sample Halton (2, 3) sequence indexed by the
frames since the renderer's last history reset (`TemporalFrames - 1` modulo 8; D4), added to `frame.Projection`,
`ViewProjection` and their inverses after 1924. Every renderer starts at phase 0 after a reset, so a capture and a fresh
renderer of the same view render the same phases. Culling, LOD selection, the shadow cache key, the cascades, the
overlays' `SceneOverlayFrame::ViewProjection` and the previous-frame matrices stay unjittered. A frame without TAA has
no jitter. The overlays keep their unjittered matrices (lines and glyphs stay steady) but depth-test against the
jittered depth: their depth tests get a slope-scaled bias of one jitter step, so overlays lying on surfaces (the grid on
a ground plane, text on a wall) do not flicker; where a silhouette hides an overlay, the hiding edge follows the
jittered depth and may move by one pixel between frames (accepted: a bias cannot change which surface covers a pixel,
and the scene under the overlay is resolved). Rejected: jittering `SceneCamera::Projection` (redraws every cascade every
frame: 2.3).

**D4 The TAA resolve.**
- A compute pass in a new stage `GPU.TAA`, after `Transparent` and before `UpdateExposure`, reads the jittered HDR, the
  depth, the velocity and the history, and writes the other texture of a ping-pong pair of RGBA16F history textures
  (made only with TAA, released through `ReleaseRenderTargets`). The exposure, bloom and tonemap binding sets exist per
  history index, so nothing is copied.
- The resolve: the velocity of the nearest depth in the 3x3 neighborhood, a filtered history fetch, the history clamped
  to the neighborhood's color range in a luma-chroma space, luminance weighting so highlights do not flicker,
  `SanitizeHDR` on the result. The current frame's weight is the largest of `1 / TemporalFrames`, `c_TemporalMinWeight`
  (1/8) and a motion weight that rises with the motion's length in pixels: the eight frames after a reset are averaged
  with equal weights (at `TemporalFrames` 8 the output is the mean of the eight jittered frames, where the clamp and the
  luminance weights leave them alone), and from then on the history is exponential; a still pixel then oscillates
  around that mean by at most 13% of the spread of its eight samples (the bound the weight 1/8 gives for any order of
  the samples).
- History resets (the output is the current frame and the jitter returns to phase 0): the first frame, camera cuts (the
  existing `IsCameraCut`), a different render scene that is not a clone of the previous one (the test at 1894: entering
  Play keeps the history), a resize, a renderer that did not render in the previous device frame (a new
  `Renderer::GetFrameNumber()`, advanced by `Renderer::BeginFrame`: hidden views and the GameView skip frames; counted
  in frames, so slow Debug frames, the validation layer and the editor's throttled idle frames never reset a history),
  and `SceneRenderer::ResetHistory()`, which `GameRenderer` calls where it calls `ResetExposureAdaptation` (level
  loads). `SceneRendererStats` gains `TemporalFrames` (the frames in the history, 1 on a reset frame) and
  `HistoryResets`.
- Transparent surfaces are resolved with the rest: they gain anti-aliasing and may ghost where they move over a still
  background; the clamp bounds it. Rejected: resolving before transparents (they would alias and shimmer with the
  jitter); a reset of renderers whose previous frame is older than a wall-clock limit (temporal state would depend on
  the machine's speed, so tests and captures would not settle on slow machines).
- `PostProcessComponent` keeps `AntiAliasing` (bool) and gains `AntiAliasingMethod` {`FXAA` = 0, `TAA` = 1}, default
  FXAA, inactive while `AntiAliasing` is off. Scenes of 0.5.0 load unchanged and keep FXAA; the Basic 3D template and
  the samples set TAA. Rejected: turning `AntiAliasing` into an enum (enum properties read names or integers, so every
  saved `"AntiAliasing": true` would fail: `Reflection/PropertyJson.cpp:464-476`); TAA as the component's default (it
  would change every scene without a `PostProcess`, every `GPU.SceneRenderer.GPUDriven` comparison and the scenes of the
  measured budgets). `AddNeutralPostProcess` already turns `AntiAliasing` off, so exact-pixel tests never jitter.
- From T1/T2 (D17): this resolve with S7's two pre-exposure terms is the preset Original (`c_TAAOriginalSettings`
  holds its constants), Sharp and Custom add variance clipping, a resampling term, a reactive mask, disocclusion
  rejection and sharpening in the tone mapping, and the history's alpha holds the signed view distance.

**D5 Renderers of single images and the editor's overlays.**
- `SceneRendererSpecification::TemporalAntiAliasing` (default true): `ThumbnailCache`'s renderer turns it off, and a
  TAA scene then renders with FXAA there. The capture renderer keeps TAA, and both of its users settle it:
  `viewport.capture` and `ProjectThumbnail` render again, with a still view, until `TemporalFrames` reaches
  `c_TemporalSettleFrames` (8), so the image is the mean of the eight phases (D4), as the converged Viewport shows
  within its oscillation. `viewport.capture` bounds the wait by `c_MaxCaptureTemporalFrames` (16) and reports
  `temporalFrames` in its result; `ProjectThumbnail` adds the condition to its completeness check
  (`ProjectThumbnail.cpp:139-146`, bounded by its `c_MaxFrames`). Their targets are released after each image, so each
  one starts from a reset. As built in S2: a frame still incomplete for any other reason (assets, mips, glyphs, poses,
  instances, the motion view) calls `ResetHistory`, so the eight frames are all complete ones (`c_TemporalSettleFrames` in
  `ViewportRenderer.h`); the waits S6 and S7 add (`LocalShadowViewsPending`, `ReflectionProbesPending`,
  `PreExposureSettled`) join those reasons. The TAA wait also applies without `waitForAssets`.
- Renderers of single images draw their lighting at once. `SceneRendererSpecification::SpreadLightingUpdates` (default
  true) spreads local shadow views (D8) and probe conversions (D11) over frames, with fades; the capture renderer (a
  `ViewportRenderer` made with fades off), the thumbnails' renderer and the probe bake's renderer (D11) turn it off, so
  their first complete frame draws every view of every shadowed light and converts every visible probe, without fades.
  `SceneRendererStats::LocalShadowViewsPending` and `ReflectionProbesPending` count what is not drawn or converted yet
  (0 for them; reported in the capture's result). From S7, `viewport.capture` and `ProjectThumbnail` also wait for
  `PreExposureSettled` (D7), bounded by `c_MaxCaptureExposureFrames` (8).
- Picking and entity tints read the jittered entity IDs: a sub-pixel difference at silhouettes, accepted (tints are
  debug views). The selection outline does not: it would crawl by a pixel with the jitter. With TAA and a selection, a
  selection-mask pass in `GPU.Overlays` draws the selected instances again with the unjittered matrices into an R8
  target (CPU-built draws on both paths, as transparent surfaces are; skinned and mirrored pipelines; depth-tested
  against the frame's depth with one jitter step of bias), and `Overlay/Outline.frag` reads the mask instead of the IDs
  in that case. Rejected: no jitter in editor views (the editor would show another image than the game); resolving the
  outline in the history (it lags behind a dragged object).
- With TAA, PCSS and GTAO rotate their per-pixel noise by the frame index (frames since the last reset, so captures are
  deterministic), and the history accumulates their samples; their sample counts do not change in this stream.
- From T1/T2 (D17): every preset settles after `c_TemporalSettleFrames` (Sharp keeps the least weight at 1/8), captures
  and the project picture show the default preset's sharpening, and T2 adds `viewport.capture {live}` for what a view
  presented in motion.

**D6 Cascade blending.** `DirectionalLightComponent::CascadeBlend` (fraction of a slice's depth range, 0 to 0.5,
default 0.1). The blend band of cascade c is the last `CascadeBlend` of its slice, before the split:
[split_c - b_c, split_c] with b_c = `CascadeBlend` x (split_c - split_(c-1)). Pixels in it sample cascades c and c+1 and
blend by depth; the last cascade's band fades to unshadowed, ending at `ShadowDistance`. Cascade c covers its band
already; `FindShadowCasters` moves the near end of slice c+1 toward the camera by b_c before fitting c+1's sphere, so
c+1's square covers the band too (the CPU caster test, `GetCascadeMask`, and the GPU's `CullConstants::CascadeBoxes`
read the same `m_Cascades`, so both paths follow). The near end is a slice's narrowest part, so the coarser cascade's
sphere grows little. `ShadowParams.z` carries the fraction. Cost: two PCSS evaluations in the band's pixels, and in
every shadowed scene a slightly larger square for cascades 1 to 3 (lower texel density, more casters); the measured perf
scenes all have a shadowed sun (2.7), so S3 A/Bs them (7.1). The cascade cache's keys change once. Default on, because
the seam is a visible defect. Rejected: a dithered choice of cascade (correct only with TAA); a band after the split
with slice c's far end widened (it would grow the finer cascade at its widest end and lower its density most).

**D7 Light units (S5) and pre-exposure (S7).**
- Units (S5), stated in the inspector's tooltips, the component descriptions and a new DG section "Light units and
  exposure": directional `Intensity` is illuminance in lux; point and spot `Intensity` is luminous intensity in candela
  (a point light of P lumens has P / 4pi cd; a spot's candela is along its axis, not spread over the cone); sky radiance
  and emission are luminance in nits (cd/m^2): an environment map's values times `SkyLightComponent::Intensity`, the
  procedural colors, `EmissiveColor * EmissiveIntensity`. The shading already computes this (2.4); nothing is rescaled,
  so 0.5.0 scenes render as before. With manual exposure EV, the tonemapper sees `L * 2^EV`; DG states the relation to a
  camera's EV100 and a table of reference values (sun, overcast sky, moon, lamp, candle). The registered ranges stay:
  manual `Exposure` [-16, 16] covers a 100,000 lux sunlit scene (about -15), and `AutoExposureMinEV`/`MaxEV` [-12, 16].
- Pre-exposure (S7): shading writes `L * P` into the HDR target, P a power of two. With automatic exposure, P follows
  the renderer's metered exposure, read back without a stall through a ring of `BufferReadback`s polled each frame on
  both paths (as the cull counters' `CounterReadback`), and changes only when it is off by more than one stop; with
  manual exposure P is the power of two nearest the multiplier (1 for EV 0). A renderer starts at P = 1 (again after
  `ResetExposureAdaptation`), and `SceneRendererStats::PreExposureSettled` turns true once a readback of its own
  exposure arrived and P kept its value; captures and the project thumbnail wait for it (D5).
  `SceneRenderOptions::PreExposure` forces a value: tests, the probe bake, and both renderers of every CPU/GPU
  comparison use it. Every writer of scene HDR multiplies by `TemporalParams.y`: forward, sky, transparent, fog and the
  HDR clear (its color is a luminance too); the histogram and bloom's threshold divide it out, the tonemap applies
  `M / P`, the TAA history is rescaled by `P / P_previous`. Multiplying by a power of two is exact unless a value
  overflows or becomes subnormal (below 2^-14 in half floats: at P = 2^-15 every value under about 2 nits loses bits),
  so two renderers with different P give the same image only where nothing is that bright or dark, and every exact
  comparison forces one P on both sides. `AutoExposureMaxEV`'s default becomes 16 (sunlit scenes in real units meter
  around 15); stored values do not change, so the samples that adopt real values (S7, S10) set 16 through the editor.
- Rejected: absolute values in the HDR target (a physical sun overflows); a pre-exposure read on the GPU from the
  exposure buffer (one more storage buffer in every scene shader, and two renderers' values could differ in the last
  bit); a first P guessed from the scene's lights (wrong for every scene lit by its sky; the readback settles in a few
  frames).
- As built in S7: automatic exposure's P is the power of two nearest a quarter of the metered multiplier
  (`SceneRenderer::c_PreExposureHeadroomStops` 2: a real sun's disk passes the half-float range at the hysteresis's
  stop otherwise), within 2^-24 to 2^24; the readback is copied right after the exposure pass (`GPU.Post`, the frame's
  own metering) rather than in `GPU.Setup`. `ResetExposureAdaptation` also starts P again at 1, and
  `JumpExposureAdaptation` only jumps the adaptation: captures and the project thumbnail reset on their first frame and
  jump on the later ones, so they settle. `SceneRendererStats::PreExposure` reports P; `GetExposureBuffer()` exposes the
  exposure for tests.

**D8 Point and spot shadows in an atlas.**
- Storage: one D32 2D atlas (`SceneRendererSpecification::LocalShadowAtlasSize`, 4096, 64 MiB; 2048 for the renderers
  of single images, D5; made when the first shadowed local light is drawn, released with the targets) with tiles of 128
  to 1024 texels from a quadtree allocator; when the wanted tiles do not fit, sizes step down in order of relevance
  until they do. A spot light takes one tile (perspective, the outer cone plus a one-texel guard band); a point light
  six tiles of one size (90-degree faces widened by the guard band so filtering never leaves a face). The atlas is one
  framebuffer; each view sets its viewport and scissor, and a redrawn tile is cleared by a quad at depth 0 (a texture
  clear would also clear the cached tiles). `ShadowMesh.vert` gains a local-view variant (a perspective view from the
  local view buffer, without the cascades' pancaking).
- Which lights: at most `MaxShadowedLocalLights` (16) per view, the lights with `CastShadows` ranked by the clusters'
  relevance (projected size of their range); a tile's size follows that projected size (power of two, at most 1024),
  with hysteresis so sizes do not toggle. Lights that drop out fade their shadows over the last rank's margin instead of
  popping.
- Cache on both paths: a view is redrawn only when its key changes (light position, direction, range and angles, tile
  and size, the near plane) or something that reaches it changed (a proxy whose old or new bounds reach the light's
  range, the scene's mesh and material tables, the geometry pool's content, the texture table while its casters are
  alpha-masked), as `PrepareShadowCache` does for cascades. With `SpreadLightingUpdates` (D5), at most
  `LocalShadowViewsPerFrame` (12) views are redrawn per frame, nearest lights first, and a light whose views are not
  drawn yet is unshadowed and fades in. With it off, or with `SceneRendererSpecification::CacheLocalShadows` off (the
  perf tests turn it off, as they turn `CacheShadowCascades` off), every view of every shadowed light is drawn in the
  frame and nothing fades: the budget only spreads the cache's redraws. The camera is not in the key: the matrices that
  sample a tile are rebuilt each frame relative to the camera in double.
- Deciding what each redrawn view draws, twice: the CPU path queries the render scene's `SpatialIndex` with the view's
  planes (`QueryPlanes`, as the cascades do at `SceneRendererExtraction.cpp:545-560`) and tests the candidates in
  parallel chunks merged in chunk order (as `FindShadowCasters` does); the GPU path's shadow culling (`Cull.comp`'s
  `ST_CULL_SHADOW` variant) tests the frame's redrawn local views after the cascades, read from one new storage buffer
  (`u_LocalShadowViews`: 23 of 31 buffers), into `c_CullMaxLocalShadowViews` (12) times 32 new buckets (mirrored,
  double-sided, geometry, alpha-masked; no occlusion phases). With every view drawn in a frame (above), the GPU path
  culls the views in batches of 12. Casters below a texel are left out on both paths; LOD is chosen with the tile's
  size as the viewport height on both. Casters mark `c_FeedbackShadow` (feedback) and the CPU's marks alike.
- Counts: which views are redrawn is the CPU's decision on both paths, so `SceneRendererStats` gains
  `LocalShadowLights`, `LocalShadowViewsDrawn` and `LocalShadowViewsPending`. Casters and draws of the GPU path are
  counted on the GPU: `CullCounters` and `Tests::DrawnCounts` gain `LocalShadowCasters` and `LocalShadowDraws` (the CPU
  path's stats fill them), compared through `Tests::GetDrawnCounts` as the cascades' counts are. This Mac draws the GPU
  path through `NoCount` and `PerDraw` only; the count path of the local views is untested until the reference machine
  returns.
- Shading: a per-renderer table maps light records to shadow slots (one more storage buffer in the light-culling
  layout, read by `LightCompact.comp`, D16); `LightCompact.comp` writes the slot into `LightData::SpotAngles.z` (and the
  CPU path in `CollectLights`); a `LocalShadowData` buffer per slot holds up to six camera-relative matrices, the atlas
  rectangles and the biases. `Forward.frag` picks a point light's face by the major axis, filters with 16 rotated taps
  through the existing comparison sampler (no new sampler: 9 of 16 stay), and applies `ShadowBias` and
  `ShadowNormalBias` in texels of the tile. Reversed-Z with the cascades' depth bias signs.
- Properties: `CastShadows` (default false: existing scenes do not change), `ShadowBias` (1), `ShadowNormalBias` (1.5),
  `ShadowFilterRadius` (the filter radius in texels of the light's tile, 1.5), inactive while `CastShadows` is off; the
  tooltips give the units. The name is not `ShadowSoftness`, which on the directional light is an angular diameter in
  degrees driving PCSS: one name with two units would mislead scripts and agents that set it by name. The same inactive
  reasons are added to the directional light's `Shadow*` properties (the gap of 2.4).
- Rejected: depth cube arrays (a face per framebuffer anyway without geometry shaders, fixed sizes, cube comparison
  filtering differs from the cascades', and one more sampler state); per-light textures (a binding each); layered
  rendering through `shaderOutputLayer` (not enabled by the engine; deferred); CPU-only culling of local views on the
  GPU path (would break the rule that both paths decide, and costs the CPU when many casters move); a physical source
  radius driving PCSS for local lights (a blocker search per tap; deferred with contact shadows).

**D9 Height fog per fragment.** A `HeightFogComponent` (first active one applies, like `PostProcess`): `Density`
(extinction at `BaseHeight`, 1/m, 0.02), `HeightFalloff` (1/m, 0.2), `BaseHeight` (0), `StartDistance` (0), `MaxOpacity`
(1), `Color` (linear albedo of the scattering, 0.7 0.8 0.9), `ColorFromSky` (true: the sky lights the fog: an
environment map's irradiance from above; the procedural sky's `HorizonColor`, because its irradiance from above is
mostly the zenith's color and made a dark blue band below the horizon in the Open World (S4); the atmosphere's when it
is on, which S7 reads at the horizon for the same reason) and `SunScattering` (forward-scattering strength toward the
directional light, 0.5). `Forward.frag` (opaque and transparent) applies the analytic integral of exponential height
fog along the camera ray before `SanitizeHDR`, from camera-relative positions and the camera's world height;
`Skybox.frag` applies it at infinite distance. Parameters go into `FrameConstants`; no fog component means a uniform
branch that skips it. Rejected: a fullscreen pass after the sky (transparent surfaces would still need the
per-fragment path, and it adds a full-screen read and write); volumetric froxels (deferred).

**D10 The atmosphere as a sky source (S7, with the pre-exposure its real values need).**
- `SkyLightSource::Atmosphere` (= 2, appended) with `AtmosphereDensity` (scales air and aerosol density, 1 = Earth),
  `AtmosphereHaze` (aerosol amount, 1) and `GroundAlbedo` (0.3); inactive reasons for the other sources' properties and
  the reverse. Earth's radii, scale heights and scattering coefficients are constants in one header shared by C++ and
  GLSL (`SH/Include/Atmosphere.glsl` mirrors `R/Atmosphere.h`).
- GPU: a transmittance LUT (256x64) and a multiple-scattering LUT (32x32), computed when the parameters change; a
  sky-view LUT (192x108) when the sun direction or the camera's altitude band changes. The background is drawn from the
  sky-view LUT (a `Skybox.frag` variant) and the sun disk analytically from the light's illuminance (it never enters the
  cube). The environment cube (mip 0 from the sky-view LUT, then `FilterEnvironment`) is double-buffered and refreshed
  in slices of one face per frame and then the filter steps, only when the sun moved more than 0.5 degrees or the
  parameters changed; the switch to the new set happens when it is complete. Atmosphere parameters go in a constant
  buffer (`ProceduralSkyParameters` has 32 bytes left). All of it in `GPU.Setup`.
- The sun's color at the ground: the CPU evaluates the same transmittance (double precision, once per change) and
  multiplies it into `frame.DirectionalLightColor` and `m_SunColor`, so lighting, shadows and the sky agree, without a
  readback.
- Rejected: a separate atmosphere component (two sky sources to arbitrate); regenerating the cube every frame (2.5);
  per-pixel atmosphere ray marching for the background (cost).
- As built in S7: the sky-view table is bound in the scene set at SRV 27 (a black texture without the atmosphere), so
  the background variant and the fog read it: `ColorFromSky` fog takes the atmosphere's horizon toward each ray's
  azimuth on the GPU (`FogLight.w` 2) instead of a CPU horizon value. `FrameConstants` gained `SunDiskDirection` and
  `SunDiskColor` (the disk's direction, angular radius and illuminance through the atmosphere). The tables and the
  environment hold the sky per lux of sun (`EnvironmentParams.x` is `Intensity` times the light's illuminance), so the
  light's intensity changes cost nothing; without a directional light a 1 lux sun at the default direction lights it.
  `BackgroundBlur` is inactive under the atmosphere (its background is the sharp table). The feature scene's "Atmosphere
  Sky" is inactive, like its "Procedural Sky". The Open World keeps its 3 lux sun (the atmosphere is linear in the
  sun's illuminance, and its lantern and campfires keep their look) and `DayNight` turns the same light into a dim
  moon at night, so the atmosphere shows a moonlit sky rather than black.

**D11 Reflection probes baked into texture assets.**
- `ReflectionProbeComponent`: `Capture` (texture asset), `Extents` (half extents of the influence box, 5 5 5),
  `BlendDistance` (1), `BoxProjection` (true), `Intensity` (1), `Priority` (0). Influence boxes follow the entity's
  rotation. Faces are baked at 256 (twice the run-time face size of 128, so the equirectangular image, 1024x512, keeps
  the detail the cube needs); the size is a renderer constant, not a property, because the cube array has one size.
- Bake: `lighting.bakeProbes` (deferred, `MayDefer`; refused while playing, as `prefab.apply` is, because it renders
  and writes from the edited scene) renders the six faces of each probe with a renderer of its own, made for the bake
  and released after it (entity IDs, TAA and `SpreadLightingUpdates` off, `TextureStreaming` off so it waits for full
  resolution, neutral post-processing, the pre-exposure forced to a power of two from the scene's directional light and
  divided out when the image is assembled), through a new `SceneRenderOptions::HDROutput` that skips post-processing
  and leaves the HDR target to read back (`TextureReadback`, polled). It assembles an equirectangular image on a job and
  writes it as a Radiance `.hdr` through `ImageWriter` (stb's `stbi_write_hdr_to_func`, on an I/O job) into a staging
  file under `<project>/.strata/Bake/`.
- Files are never overwritten. The staging file enters the project through `EditorAssetManager::ImportExternal` (the
  path `asset.import` takes: a copy and a background import that the command polls; the main thread never waits) as
  `Assets/Lighting/<scene>/<probe UUID>.hdr`, named after the entity's UUID (16 hex digits), since names are unique only
  among siblings (2.7); " (n)" is appended when the name is taken. The command then sets `Capture` to the new handle.
  When the probe's previous capture is a file an earlier bake made for this probe (in `Lighting/<scene>/`, named after
  its UUID) and nothing else uses it, it moves to the trash (`MoveToTrash`); any other texture `Capture` named, such as
  the sky's map, stays untouched. One undo step records it all in an `EditorActionGroup`: the scene change (`Capture`,
  tracked in a `SceneEditTransaction`), an `AssetCreateAction` per new file and an `AssetTrashAction` per replaced one.
  Undo restores the old handle and file, redo the new ones, and each action checks its files before it acts and fails
  with the reason when they changed (DG "Asset files are never lost"). The staging files are removed when the import
  ends, also on failure.
- The file is an ordinary texture asset: `.meta`, import, references through the component's asset property (pruned
  exports and delete checks follow it), packs. A probe bakes what is drawn at its position, other probes' current
  captures included (baking twice gives a second bounce).
- Runtime: the renderer gathers probes each frame (a registry view; worlds' cells are ordinary entities) and keeps up
  to `MaxReflectionProbes` (16) visible ones by priority and size. Their textures are requested through the frame's
  `AssetResolveCache` (`m_Assets`, High priority), never per entity from the asset manager, and a probe cache modelled
  on the environment cache keeps them across frames: it holds a capture whole (`TextureStreamer::Hold`) until it is
  converted (`EquirectToCube` at 128, `FilterEnvironment`) into a slot of a `TextureCubeArray` of 16 slots (about 1 MiB
  each), computes its diffuse irradiance as 9 RGB spherical-harmonic coefficients, then releases the hold; DG "Render
  scene" lists it as the third cache that keeps assets across frames. With `SpreadLightingUpdates`, at most two probes
  are converted per frame, in `GPU.Reflections`; without it every visible one. `Forward.frag` loops over the frame's
  probe list (at most 16 box tests per pixel), blends the two strongest by box distance and priority with box-projected
  lookups, and falls back to the sky. Transparent surfaces use them too.
- Cube arrays: `imageCubeArray` becomes a required device feature. Devices without it are rejected at start-up with
  the reason, as those without anisotropic filtering are (2.6), because the one scene binding layout puts the probe
  cube array into every scene shader, and SPIR-V's `SampledCubeArray` capability is valid only with the feature
  enabled. No `GraphicsDeviceInfo` flag is added. Without probes, SRV 9 holds a one-slot black cube array. Every desktop
  GPU with Vulkan 1.2 has the feature, MoltenVK included; DG "Building" lists it among the requirements.
- As built in S8: scene-set SRV 9 is the cube array (`Renderer::GetBlackCubeArrayTexture` without probes in view) and
  SRV 10 the probe buffer (`RenderData::ReflectionProbeBuffer`: the slots' 9 SH coefficients, written on the GPU, then the
  frame's list); 18 and 19 stay free for S9 and S11. The cache is per renderer (not shared like map environments): a cube
  array of `MaxReflectionProbes` slots of 128 with six roughness levels, kept across resizes, released with
  `SetViewportSize(0, 0)` or once the scene has no probe. Ranking: priority, then the smaller box (the more specific
  place), then the entity. Conversions (EquirectToCube, Prefilter at 256 samples, `IBL/ProbeIrradiance.comp`) are the
  only work of `GPU.Reflections` so far, so the stage records time only in frames that convert; S9's trace joins it.
  The bake renders through `SceneRenderOptions::HDROutput` (the frame ends with its HDR target) with a forced
  pre-exposure, one probe (six 256 views) per frame; the result lists `{directory, probes: [{entity, name, capture,
  path, complete, previous, trashed}]}`. Command scripts run the editor with `--no-gpu`, so
  `FeatureCommands.json.in` cannot bake: `GPU.Editor.Lighting` bakes a copy of the feature project instead. The
  Third-Person course has four probes (`Reflection Probes`), `BoxProjection` off (open islands).
- Rejected: run-time capture (six scene renders per probe with their own culling and shadows, and the Open World's
  moving sun would recapture constantly); cube texture assets (an asset pipeline change: cooked format, streaming,
  residency; deferred); clustered probe lists (16 box tests are cheap; deferred until scenes need more); a run-time
  flag turning probes off without cube arrays (the shaders would not load on such a device); a forward variant without
  cube arrays (a second set of scene pipelines for hardware Strata does not target); writing over the file `Capture`
  names (it may be any texture, and the previous bake would be lost to undo).

**D12 Screen-space reflections before the forward pass.** With `ScreenSpaceReflections` on (and TAA on: SSR reads the
previous frame's resolved image and relies on the history to remove its noise; without TAA it is inactive), a prepass
variant (`ST_PREPASS_SURFACE`) writes an RGBA16F surface target: the normal-mapped view-space normal (octahedral),
roughness and metallic (the normal and metallic-roughness maps are sampled before any discard). A compute pass in
`GPU.Reflections` after the prepass traces at half resolution (24 steps and a refinement, start jittered per frame)
against the current depth and fetches the previous resolved image at the hit's reprojected position (velocity at the hit
pixel), with fades for roughness above `ScreenSpaceReflectionsMaxRoughness` (0.5), screen edges and back-facing hits.
`Forward.frag` mixes it over the probe or sky specular by its confidence before the split-sum BRDF. Rejected: a
composite after the forward pass (it would need the IBL specular term separated out); tracing a depth pyramid (the
existing pyramid keeps the farthest depth for occlusion; deferred); full resolution.

**D13 Sun extraction at the source, written as a light.**
- `Asset/HDRSun.h` (a pure function over a float equirectangular image): the brightest texel, the connected region
  above a threshold around it (wrapping at the seam, at most 3 degrees across), its illuminance (sum of radiance times
  each texel's solid angle), direction (illuminance-weighted) and color, and whether the peak is flat (clipped in the
  file: the result is then a lower bound, and the command says so).
- `TextureImportSettings::RemoveSun` (default false): one `GetSettingsProperties` entry (bool, with a tooltip), the
  matching `GetDefaultSettings` key, and an inactive reason in `GetInactiveReason` unless `Usage` is HDR, so the asset
  inspector shows it and `asset.setImportSettings` accepts it (`NormalizeImportSettings`). The importer runs the
  function on stb's floats before the resize and the clamp, and fills the region from its border before cooking.
  Outputs for existing settings are byte-identical (tested in the build, and against 6425b57b by the A/B of 7.1), so
  `TextureImporter::GetVersion()` stays 5; the textures whose setting changes re-import.
- `lighting.extractSun` (deferred: it reads the source on a job; refused while playing, as the bake is): writes the
  scene's directional light (or creates "Sun") with the direction rotated by the sky's `Rotation`, the color, and
  `Intensity` = illuminance times `SkyLightComponent::Intensity`, in one undo step; the Sky section's "Extract Sun"
  button runs it and then `asset.setImportSettings` with `RemoveSun`, two steps with their own meanings.
- Rejected: a GPU reduction at render time (the stored map is clamped and the answer arrives frames late while the
  cascades need the direction on the CPU now); a light the renderer derives at run time (needs metadata in cooked
  textures, a format change, and hides an editable light from the user and scripts).

**D14 Clearcoat and transmission.**
- `MaterialProperties` gains `Clearcoat`, `ClearcoatRoughness`, `ClearcoatMap` (R), `ClearcoatRoughnessMap` (G),
  `ClearcoatNormalMap`, `Transmission`, `TransmissionMap` (R), `IOR` (1.5), `Thickness` and `AttenuationColor` with
  `AttenuationDistance` (volume, without a thickness map). `.stmat` stays at format version 1: new keys have defaults
  and 0.5.0's reader only warns on unknown keys. `MaterialData` grows from 96 to 144 bytes (multiple of 16); the map
  count becomes 9 in every place 2.5 lists; shader flags `c_MaterialClearcoat` (16) and `c_MaterialTransmissive` (32).
- Clearcoat: a second specular lobe in `EvaluateLight` and a second prefiltered lookup with the coat's normal and
  roughness, the base attenuated by the coat's Fresnel; a dynamic branch on the flag (coherent per draw).
- Transmission: transmissive materials are routed as transparent (`c_CullMaterialTransparent`), so both paths leave
  them to the CPU-sorted pass, which draws them first, without blending, reading a scene-color copy. Those draws also
  write depth, entity IDs (when the renderer has them) and velocity (when the frame has motion) as further attachments,
  so glass is picked, outlined and reprojected like an opaque surface, and transparent surfaces behind it, which the
  copy does not hold either, stay hidden behind it. Glass gets no ambient occlusion (GTAO runs on the prepass's depth),
  stated in DG. The scene-color copy: when the frame draws a transmissive surface, the HDR image after the sky is
  downsampled into a half-resolution texture with mips (roughness picks the level), in `GPU.Transparent`: B10G11R11
  where the device has extended storage formats (`SupportsExtendedStorageFormats`), RGBA16F otherwise, as
  half-resolution AO falls back. Refraction offsets the lookup by the view-space normal, `IOR` and `Thickness`;
  attenuation follows Beer's law. Transmissive draws are drawn without blending and write their own
  depth and velocity, so they write HDR alpha 1 and are not reactive to the TAA (D17).
- glTF: `KHR_materials_clearcoat`, `KHR_materials_transmission`, `KHR_materials_volume`, `KHR_materials_ior` join
  `c_SupportedExtensions`; their textures import with `TextureUsage::Data` or `NormalMap`; `GltfImporter::GetVersion()`
  becomes 10 (every glTF re-imports once).
- Rejected: a separate refraction pass after transparents (sorted transparency would be wrong behind glass); a
  full-resolution copy (bandwidth; rough glass blurs anyway); glass in the prepass (its depth would hide what lies
  behind it from the forward pass and from the copy).

**D15 GPU stages.** Two new stages, registered in pass order: `GPU.Reflections` between `GPU.AO` and `GPU.Opaque` (probe
conversion, SSR trace; registered by S8) and `GPU.TAA` between `GPU.Transparent` and `GPU.Post` (motion copy, resolve;
registered by S2). S1's motion copy is timed in `GPU.Post` until S2 moves it into `GPU.TAA`, so no commit runs a pass
outside a stage. Local shadows time in `GPU.Shadows`, the pre-exposure readback and the atmosphere in `GPU.Setup`, the
scene-color copy in `GPU.Transparent`, the selection mask and the motion view in `GPU.Overlays`; fog adds no pass. Each
stage boundary is outside a render pass (MoltenVK may write timestamps of a render pass at its end, DG "GPU timing").
The three lists of 2.6 change with each new stage.

**D16 Bindings.** New scene-set bindings, all within the slots free today: SRV 8 local shadow atlas, 13 local shadow
data (storage), 9 probe cube array (a one-slot black cube array without probes), 10 probe records (storage), 18 SSR
result, 19 scene-color copy, 20 previous instances and 21 previous palette (storage, D2: the shared buffers themselves
without motion). The shadow pass's binding set (cascades and local views) binds a dummy D32 texture at SRV 8, as it
binds `m_DummyShadowMap` at SRV 7, so no pass samples the atlas it renders into. The light-culling layout gains the
light-to-shadow-slot table (storage, D8). Samplers stay at 9 of 16 (the probes use the linear clamp sampler, the atlas
the comparison sampler); the scene set's storage buffers go from 8 to 12 of 31. As built in S7: SRV 27 is the
atmosphere's sky-view table (D10).

**D17 TAA quality presets and tuning (T1, T2).** The owner (2026-10-22) "really really hated the blurry mess in
Unreal Engine", asked how many frames the TAA keeps and whether it trails in motion, then asked for sharpening, tighter
clipping, a reactive mask for transparents, disocclusion rejection, and the TAA tunable with two defaults: the new look
and the original. T1 builds the settings and every technique, reachable through Custom, with Original (S2's look) as the
default, so stopping after T1 ships nothing untuned; T2 adds the step-synchronized live capture, takes the evidence,
tunes Sharp, makes it the default, measures and documents. Both come after S8 and before S9, whose SSR reads the history
and its alpha. These presets are one component's settings, not the renderer quality presets section 1 leaves out.

#### D17: Where T1 starts

| What | State before T1 |
| --- | --- |
| Resolve (`SH/PostProcess/TAA.comp`) | S2 as described in D4 |
| S7's additions to the resolve | The history multiplied by `HistoryScale` (P / P_prev, exact); the luminance weights' exposure times `InversePreExposure` |
| Push constants | `RenderData::TemporalParameters`, 32 B, two padding floats |
| History textures | Two RGBA16F; alpha always 1, read by nothing |
| HDR alpha | Clear, opaque surfaces and the sky write 1; the transparent pass blends alpha with `SrcBlendAlpha One`; read by nothing |
| Tone mapping | Reads the resolved image with a point sampler; its 32 B push constants are full |

"**Original**" is S2's resolve plus S7's two pre-exposure terms. Without those terms the image would flash at every
pre-exposure step.

#### D17: Settings

`PostProcessComponent` gains two enums and ten tunables, registered right after `AntiAliasingMethod` in the order below.

- `TAAPreset`: enum `TemporalAntiAliasingPreset : uint8_t`, `Sharp` = 0, `Original` = 1, `Custom` = 2. The default is
  Original in T1 and Sharp from T2 on.
- `TAAHistoryClamp`: enum `TemporalHistoryClamp : uint8_t`, `MinMax` = 0 (display "Range"), `Variance` = 1 (display
  "Tight (variance)").
- These integer values and names are script and file API from now on and never change.

| Preset | Meaning |
| --- | --- |
| Sharp | The new look: tighter history clipping, slow pans resampled less, uncovered surfaces drop their history, transparent surfaces take mostly the current frame, a faster response to motion, and a light sharpening of the displayed image only. |
| Original | The TAA as S2 built it, for comparison and for scenes that prefer its softer, steadier look. |
| Custom | Every tunable below. |

**The tunables** (inactive unless Custom, see "Inspector"):

| Property (display) | Plain words (tooltip and Manual) | Type, range | Original | Sharp: proposal (bounds) |
| --- | --- | --- | --- | --- |
| `TAAHistoryFrames` | About how many frames the image averages: the current frame weighs at least 1 / this. At 8, every jitter position counts once. More is smoother and steadier but lags behind lighting changes; fewer reacts faster but edges crawl. | uint, 2-32, step 1 | 8 | 8 (fixed: D5's settling and the noise accumulation rely on it) |
| `TAAMotionResponse` | How quickly moving surfaces let go of the history. At 0.0625, motion of 8 px per frame weighs the current frame 0.5. Higher keeps moving things crisper and their trails shorter, but they get less anti-aliasing while they move. 0: motion does not matter. | float, 0-0.25, speed 0.001 | 0.03125 | 0.0625 (0.03125-0.125) |
| `TAAMaxMotionWeight` | The most the current frame weighs, however fast things move. 1 lets very fast motion drop the history: sharp, but aliased. | float slider, 0-1 | 0.5 | 0.5 (0.5-0.75) |
| `TAAResampleWeight` | The current frame's least weight where the history is read between pixels, as in slow pans; it is largest halfway between them. Higher keeps slowly moving texture crisper, but its edges get less anti-aliasing while they move. 0: off. | float slider, 0-1 | 0 | 0.25 (0.125-0.5) |
| `TAAHistoryClamp` | How the history is kept consistent with what the pixel shows now. Range limits it to the range of the colors around the pixel. Tight limits it to a narrower box around their average, which removes more ghosting and trails, most where the surroundings are smooth and less beside glints. | enum | MinMax | Variance |
| `TAAClipWidth` | Tight only: the box's half width, in standard deviations of the colors around the pixel. Lower removes more trails, but lets edges flicker and lets noise through. | float, 0.5-4, speed 0.05 | not used | 1.25 (1.0-1.5) |
| `TAATransparentWeight` | The current frame's least weight where a blended transparent surface covers the pixel (half cover or more counts as full). Transparent surfaces have no motion vectors of their own. 0 treats them like everything else, so they ghost when they move; 1 drops their history, so they never ghost, but their edges shimmer. | float slider, 0-1 | 0 | 0.5 (0.3-0.75) |
| `TAADisocclusionRejection` | Drop the history where a surface was just uncovered (something nearer covered it in the previous frame), so moving objects leave no ghost on what they uncover. | bool | false | true |
| `TAADisocclusionThreshold` | How much nearer the previous frame's surface must have been, as a fraction of the distance, for the pixel to count as uncovered. Lower catches thin gaps, such as a hand in front of a wall, but may drop the history of surfaces moving fast away from the camera. | float, 0.01-0.5, speed 0.005 | not used | 0.05 (0.02-0.1) |
| `TAASharpening` | Sharpens the displayed image after TAA, never the history, to restore texture detail the averaging softens. Strong edges are sharpened much less and overshoot is bounded, so halos stay faint. 0 is off. | float slider, 0-1 | 0 | 0.5 (0.25-0.75) |

**Rules for these values**
- Every range contains Original's values exactly (1/32 is exact in binary) and an exact off value.
- Sharp keeps the least weight at 1/8, so captures and the project picture still settle after `c_TemporalSettleFrames`
  (8) frames: under Original the result is the mean of the eight phases (D4), under Sharp that mean wherever the clip
  leaves the samples alone.
- History Frames below 8 (Custom only): a capture at frame 8 holds the exponential mix of the last frames instead of the
  even mean. That is deterministic, and the Manual says so.

**One source of truth.** `R/RendererComponents.h` declares above `PostProcessComponent`, whose defaults use them:
- the two enums;
- `struct TemporalAntiAliasingSettings`: the ten fields, `operator==` defaulted;
- `inline constexpr TemporalAntiAliasingSettings c_TAAOriginalSettings` and `c_TAASharpSettings`.

`RendererComponents.cpp` defines:
- `GetTemporalAntiAliasingSettings(const PostProcessComponent&)`: a preset's table, or the stored fields under Custom.
  It normalizes its result by writing 0 to every field the chosen modes do not read (`ClipWidth` under MinMax,
  `DisocclusionThreshold` with rejection off), so Custom set to Original's values equals Original whatever its inactive
  fields hold.
- `GetTemporalAntiAliasingCustomStart(const PostProcessComponent&)`: the values Custom starts from (see "How Custom
  starts").
- `SetTemporalAntiAliasingSettings(PostProcessComponent&, const TemporalAntiAliasingSettings&)`: writes the tunables.

Only these read the tables: the tunables' C++ defaults (from `c_TAASharpSettings`; a test checks equality), the
renderer (through `GetTemporalAntiAliasingSettings`), the Custom start and the inactive reasons.
`c_TemporalMinWeight`, `c_TemporalMotionWeightScale` and `c_TemporalMaxMotionWeight` leave `SceneRenderer.cpp`.

**The preset and the tunables are independent stored fields.** No setter writes another property. JSON loads, Paste
Values, Reset, undo and prefab overrides apply keys through `ComponentAccess::Deserialize` (sorted key order); cooked
scenes set each value with `PropertyInfo::SetValue` in column order. Neither order matters.

#### D17: How Custom starts

The `TAAPreset` row gets its own drawer through a small generic slot: `UI::PropertyDrawers`, registered by component
type and property name, which `InspectorPanel::DrawProperties` calls in place of that property's generic row (same
label, prefab override marks and menu through `ComponentDrawOverrides`). A component drawer cannot do it: those draw
above all the rows. The drawer runs one `component.set` on the selection, so each choice is one undo step:
- **Sharp or Original** sets `TAAPreset` only. Tuned values survive a look at a preset, and undo returns to them.
- **Custom** also writes `GetTemporalAntiAliasingCustomStart`, through a copy of the component
  (`SetTemporalAntiAliasingSettings`) whose ten tunables the registry serializes. That is the table of the preset in
  effect while the stored tunables (normalized as under Custom) equal one of the two tables, and the stored values
  otherwise, because they are the person's tuning. So Original → Custom keeps Original's look, Sharp → Custom keeps
  Sharp's, and Custom → Original → Custom returns to the tuning.
- A selection whose entities would start from different values sets `TAAPreset` only.

Files, scripts and agents' `component.set` apply keys as given. The Manual and strata-make-a-game give the call that
starts Custom from Original:

```json
{"TAAPreset": "Custom", "TAAHistoryFrames": 8, "TAAMotionResponse": 0.03125, "TAAMaxMotionWeight": 0.5,
 "TAAResampleWeight": 0, "TAAHistoryClamp": "MinMax", "TAATransparentWeight": 0, "TAADisocclusionRejection": false,
 "TAASharpening": 0}
```

Rejected:
- **A preset setter that copies values into the tunables.** Keys load in sorted order, so it would overwrite stored
  tunables on load.
- **Writing a preset's table when Sharp or Original is chosen.** It would erase the person's tuning on every look at a
  preset.

#### D17: The resolve (`SH/PostProcess/TAA.comp`)

Every new term runs in a branch on a push constant, uniform across the dispatch. Original's settings take S2's path
through each step below (per-component clamp, no moments, no resampling, reactive or rejection term), plus S7's terms.
One thing is added for every preset: the history's alpha stores the pixel's view distance (see "History alpha"), which
nothing reads under Original.

The 3x3 loop skips the center, whose color and depth are fetched before it. Both color fetches become `.rgba`, and every
sum below (moments, reactive coverage) adds the center explicitly.

**Variance clipping (`ClampMode` 1).** This replaces the per-component clamp with a clip toward the neighborhood's mean.
1. **Compressed space.** The moments are taken in the space of the blend weights: each tap becomes
   `t = YCoCg(c / (1 + Luminance(c) * e))`, where `e = u_Exposure[MULT] * InversePreExposure`, the `e` the weights use.
   Without this, one bright texel (a glint, a sunlit leaf) would widen the box of a whole neighborhood.
2. **Moments.** `m1 += t`, `m2 += t * t` over the nine taps. `mu = m1 / 9`, `sigma = sqrt(max(m2 / 9 - mu * mu, 0))`.
3. **The box.** `B = [max(mu - ClipWidth * sigma, tmin), min(mu + ClipWidth * sigma, tmax)]`, where `tmin` and `tmax`
   are the compressed taps' per-component range. B always contains `mu`.
4. **The history in the same space.** `th`: the history, already times `HistoryScale`, compressed the same way.
5. **The clip.** `d = th - mu`. `f = min(1, min over k with d_k != 0 of (bound_k - mu_k) / d_k)`, where `bound_k` is
   B's side in the direction of `d_k`, and `th = mu + f * d`. The history moves along the line toward the mean, so its
   hue relative to the mean is kept; a history inside B is unchanged. The test is `!= 0`, not an absolute epsilon: `t`
   scales exactly with a power-of-two pre-exposure, so the same components are chosen at every pre-exposure (S7's
   no-flash case compares exactly).
6. **Back to linear.** `c = YCoCgToRGB(th)`, then `h = c / max(1 - e * Luminance(c), 1e-6)`, with the same Rec. 709
   `Luminance` as step 1. This is safe: `mu` and the history both have `e * L < 1`, and `th` lies on the segment between
   them.

It cannot produce NaN: `ClipWidth` and every tap are finite (`SanitizeHDR` writers, the existing `max(..., 0)`), and a
flat neighborhood gives `sigma = 0`, so B is `{mu}` and the history becomes `mu`, which is correct.

**Resampling term (`ResampleWeight` > 0).** Where the history is read through the Catmull-Rom filter (motion is not
zero), its fractional position `f` (`SampleHistory`'s `position - center`, in [0, 1)) gives
`weight = max(weight, ResampleWeight * 4 * max(f.x * (1 - f.x), f.y * (1 - f.y)))`.
- **Why.** Below 2 px per frame (Sharp; 4 under Original) the motion term stays under the 1/8 floor, so 7/8 of every
  frame is the history resampled again. Halfway between texels the filter passes 0% at Nyquist and 88% at a 4 px period,
  so a steady pan keeps `w / (1 - (1 - w) * a)` of the detail: 55% under Original, 74% with 0.25. Sharpening cannot
  restore it.
- A still pixel (motion exactly 0) still reads its own texel, and integer motion has `f = 0`: the term adds nothing.

**Reactive mask (`TransparentWeight` > 0).**
- **What transparents write.** The transparent pipeline's alpha blend changes from `SrcBlendAlpha One` to
  `SrcBlendAlpha Zero` (with `DestBlendAlpha InvSrcAlpha`). After the transparent pass the HDR alpha is the
  transmittance through blended surfaces, Π(1 - a_i), and 1 where there are none.
  - The clear, `Forward.frag`'s opaque variant and the sky keep writing 1.
  - A material without alpha blending drawn in the transparent pass outputs alpha 1, which the blend turns into a
    stored transmittance of 0: its pixel is fully reactive, rightly, since it has no depth or velocity in the prepass.
  - No target, attachment, clear, pipeline key or shader variant is added. FXAA scenes draw through the same pipelines;
    their colors are unchanged, because nothing but the TAA reads HDR alpha.
- **Glass (S11).** Transmissive draws are drawn without blending and write their own depth and velocity, so they write
  alpha 1 (not reactive). D14 says so.
- **Particles.** Strata has none; alpha-blended quads are its particles and are covered.
- **In the resolve.** `r = min(1, 2 * max over the 3x3 of (1 - a))`: the coverage, saturated so that a surface of alpha
  0.5 or more counts fully, and dilated over its aliased edges. After the motion and resampling terms:
  `weight = max(weight, r * TransparentWeight)`. It stays partial: D4 rejected bypassing the history, because
  transparent surfaces would then alias and shimmer.
- Where a transparent surface was and no longer is, `r` is 0, and the clip removes its trail like any moving object's.
  Transparent draws are CPU-built and CPU-sorted on both paths, so the alpha products match bit for bit.

**History alpha: the previous frame's distance.**
- **What is written, every preset**, also on reset frames and off-screen pixels: the pixel's view distance along the
  camera's forward axis, `dot(p.xyz / p.w, CameraForward.xyz)` with
  `p = InverseViewProjection * vec4(UVToNDC(uv), depth, 1)` (the jittered inverse, matching the jittered raster). It is
  signed: an orthographic camera with a negative near plane sees surfaces at negative distances.
- It is clamped to ±`c_SkyDistance` (65504, the largest half float). Depth 0 (the sky, or nothing drawn) stores
  `c_SkyDistance`.
- `HistoryScale` multiplies `.rgb` only: this distance is not a color.
- **Precision.** A half float holds the distance to 2^-11 of its value, far below the threshold's 0.01. Worlds are
  supported within 20 km of the origin in X and in Z (`WorldData::c_SupportedExtent`), so views reach about 57 km, and
  cells up to `c_MaxExtent` (1,000 km) are allowed with a warning: hence both sides are clamped to `c_SkyDistance`.
- **Zero memory.** The history pair keeps its size, and no copy of the depth target is made.
- **Readers.** Exposure metering, bloom and tone mapping read `.rgb`. S9's SSR may read the alpha as the previous
  frame's distance; DG says so.

**Disocclusion rejection (`DisocclusionThreshold` > 0).**
1. **The expected previous distance.** Take the surface whose motion the pixel uses, the nearest of the 3x3, and
   reconstruct its camera-relative position `p` as above.
   - `expected = min(dot(p, PreviousForward.xyz) + PreviousDepthOffset, c_SkyDistance)`; an all-sky neighborhood expects
     `c_SkyDistance`.
   - `PreviousForward` is the previous motion camera's forward, and `PreviousDepthOffset` is
     `dot(cameraPosition - previousCameraPosition, PreviousForward)`, subtracted in double on the CPU. Both are taken
     where `PreviousViewProjection` is built from `m_MotionCamera` (the `m_Stats.MotionHistory` block), which
     `RenderFrame` overwrites before `ResolveTemporal` runs. Without motion history: the current forward and 0.
2. **The stored distances.** `textureGather(sampler2D(u_History, u_LinearClampSampler), previousUV, 3)`: the alpha of
   the 2x2 texels around the previous position, one fetch through the bound sampler at slot 8, never through the
   Catmull-Rom filter. `stored` is the farthest of the four.
3. **The test.** The pixel is disoccluded when
   `stored < expected - DisocclusionThreshold * max(abs(expected), DepthScale)`. `DepthScale` (`PreviousForward.w`) is
   the near distance for perspective cameras (at least 0.001) and the view's height (`SceneCamera::VerticalFOV`) for
   orthographic ones, so the margin is positive for every camera, including an orthographic one whose near plane is
   negative.
   - The comparison is one-sided. Taking the nearest current surface against the farthest stored one absorbs the jitter
     at still edges and the depth gradient of grazing surfaces.
   - Uncovered sky counts as disoccluded; sky over sky does not.
4. **The result.** A disoccluded pixel skips the blend and takes the current frame exactly, as S2's off-screen path
   does.
5. **Known limits** (the Manual states both):
   - Velocity has no z, so a surface moving away from the camera faster than the threshold per frame looks disoccluded
     and loses its history meanwhile: about 0.5 m per frame at 10 m with 0.05.
   - The uncovered band holds one jittered frame, then blends at 1/8, so its edges stay less smooth than the rest for
     about 17 frames (90% of the way). A per-pixel "fresh" flag that would speed this up is left out until the evidence
     shows the band.

**Order of the weights**
1. `weight = max(max(CurrentWeight, MinWeight), min(motionPixels * MotionWeightScale, MaxMotionWeight))`: S2's weight,
   with `MinWeight = 1 / HistoryFrames`.
2. The resampling term raises it.
3. The reactive term raises it.
4. A disoccluded pixel takes the current frame and skips step 5.
5. The luminance-weighted blend as built (every preset), then `SanitizeHDR`.

#### D17: Sharpening (in `SH/PostProcess/Tonemap.frag`)

It runs in the tone-mapping pass, which already reads the resolved image at viewport size: no new pass, texture or
binding set. The history, the exposure metering, bloom, SSR and `GetResolvedHDRTexture` never see it.

**The parameter.** `TonemapParameters` grows from 32 to 48 B: `float Sharpening` plus three padding floats. The CPU
writes `Sharpening` only on frames the TAA resolved, and 0 otherwise, so FXAA frames and renderers without TAA skip a
uniform branch and stay bit-identical.

**The computation**, inside `if (Sharpening > 0)`, after the exposure and the NaN clamp and before bloom:
1. **Taps.** Four `texelFetch(sampler2D(u_SceneColor, u_PointSampler), ...)` taps (left, right, up, down) at
   `ivec2(gl_FragCoord.xy)`, clamped to `textureSize - 1` (combined-sampler `texelFetch` needs no extension). They are
   exposed exactly as the center is, including S7's inverse pre-exposure. The viewport starts at 0,0, so `gl_FragCoord`
   lines up with the texels.
2. **Compressed luminance.** For each of the five taps: `y = L / (1 + L)`, where `L` is the exposed luminance.
3. **Amount.** `R = ymax - ymin` over the five taps, `s = Sharpening * (1 - R)^n`. Low-contrast texture detail gets most
   of the amount and edges little, so the anti-aliasing and D4's still-edge oscillation are not undone.
4. **Sharpened luminance.** `y' = clamp(yc + s * (yc - mean(y of the four)), max(ymin - s * R, 0), min(ymax + s * R,
   (1 + ymax) / 2))`. The bounded overshoot lets local extrema (thin lines, glints, texture at 2-6 px periods) sharpen,
   which a clamp to `[ymin, ymax]` would forbid; the upper limit keeps `y' < 1`.
5. **Back to color.** `L' = y' / (1 - y')`. If `Lc > 0`, `color *= L' / Lc`: luminance only, so no color fringes, and a
   black pixel stays black.

**The exponent `n` (start: 4) and Sharp's bounds** come from a CPU prototype of steps 2-5 (a test helper), run before
the shader is written against two profiles:
- A black-to-white edge (L 0 to 1) as TAA resolves it: overshoot at most 2% of the edge's range at Sharp's value and 5%
  at 1. The five taps of a resolved edge see about half its contrast, so 2% at 1 would need `n` above 6 and would halve
  the texture gain.
- The texture pattern of 6.14: amplitude at least 8% above unsharpened at Sharp's value.
If both cannot hold, the prototype's numbers go in the report.

**Where it shows.** Captures and the project picture are sharpened under Sharp. Asset thumbnails render with FXAA (D5),
so `ThumbnailCache::c_Version` stays.

#### D17: Data, plumbing and cost

**`RenderData::TemporalParameters`** grows from 32 to 64 B (NVRHI's limit is 128 B):
- `CurrentWeight`, `MinWeight`, `MotionWeightScale`, `MaxMotionWeight`
- `HistoryScale`, `InversePreExposure`, `uint ClampMode`, `ClipWidth`
- `TransparentWeight`, `DisocclusionThreshold`, `PreviousDepthOffset`, `ResampleWeight`
- `glm::vec4 PreviousForward` at offset 48 (w: `DepthScale`)

The C++ struct and the GLSL block change together, with `static_assert`s on the size and that offset; the layout's
`PushConstants(1, sizeof)` follows. No new binding, slot, texture or sampler: HDR alpha comes through SRV 22, the
history's alpha through SRV 25 with sampler 8.

**`SceneRenderer`**
- `RenderFrame` computes `GetTemporalAntiAliasingSettings(postProcess)` once; `ResolveTemporal(commandList, settings)`
  packs it. Normalized fields stay 0, so equal settings give equal bytes.
- The tone-mapping parameters take `Sharpening` when the frame was resolved.
- **History resets** are unchanged. Settings changes never reset the history and take effect on the next frame: the
  history's colors stay valid under any settings, and every preset writes the distance into its alpha.
- **Draw paths.** The resolve and the tone mapping do not depend on the draw path. Of T1's two inputs, the reactive
  alpha comes from CPU-built transparent draws on both paths, and both paths rasterize the same depth (the both-paths
  case covers it).
- `SceneRendererStats` gains nothing: tests judge by images, and `GetTemporalAntiAliasingSettings` is public.

**Memory.** Zero new memory: no target, binding set or pipeline. `TAALocalShadowsPeakMemory` is reported unchanged.

**GPU cost**

| Pass | What it gains | Stage |
| --- | --- | --- |
| Resolve | The moments, the compression, the resampling term, one gather and two reconstructions | `GPU.TAA` |
| Tone mapping | Four cached fetches and about 25 ALU | `GPU.Post` |

Neither reaches a scene without TAA. **Target:** Sharp's `GPU.TAA` is at most 1.5 times Original's on the Mac; above
that, profile and reduce before T2 lands (for example, compute each tap's luminance once for the compression and the
reactive term).

#### D17: Editor, files and scripts

**Inspector.** The tunables' rows are generic. Their reasons are capturing lambdas through
`ComponentRegistry::SetInactiveReason` (a `std::function`), one per property or a template over the property, checked in
this order:

| Order | Condition | Reason |
| --- | --- | --- |
| 1 | `AntiAliasing` is off | "Anti Aliasing is off" |
| 2 | The method is FXAA | "Only TAA uses it (Anti Aliasing Method is FXAA)" |
| 3 | Tunables only: the preset is not Custom | "The Sharp preset sets it (0.0625): choose Custom to tune it", quoting the table |
| 4 | `TAAClipWidth` under Custom with Range | "Only Tight clipping uses it (TAA History Clamp is Range)" |
| 5 | `TAADisocclusionThreshold` under Custom with rejection off | "TAA Disocclusion Rejection is off" |

- Fields a preset normalizes away get their own words instead of "(0)": under Original, `ClipWidth` reads "The Original
  preset clamps to the colors' range: choose Custom to tune it", and `DisocclusionThreshold` "The Original preset does
  not reject uncovered surfaces: choose Custom to tune it".
- `AntiAliasingMethod`'s tooltip drops "a slight blur of what moves" and says the TAA's look is chosen by TAA Preset.

**Serialization**
- A `PostProcess` without `TAAPreset` loads as the default preset, without a warning. That covers every scene S2's
  branch saved (the samples, the template's output, `Feature.stscene`); no released scene has TAA.
- Unknown names or integers warn and keep the default. Out-of-range tunables are clamped (files) or refused (commands).
- Cooked scenes match properties by name, so they need no format bump.
- Scenes are saved with every property, so a file the editor saves during T1 holds `"TAAPreset": "Original"`. T2's
  flip checks `git grep '"TAAPreset"'` and moves to Sharp the files that meant the default.

**`StrataTests/Architecture/ComponentSchema.json`** is regenerated (`--strata-test-helper=write-component-schema`) on
top of S8's version.

**Scripts** reach the new properties through `Entity::GetProperty`/`SetProperty` (enums as int32, `TAAHistoryFrames` as
uint32). The ABI is unchanged.

**Feature test (`Feature.stscene`).** Every value lies outside Sharp's tuning bounds, so T2's tuning cannot turn a
default into the scene's value.

| Entity | Values |
| --- | --- |
| "Environment" (active, TAA) | Custom; History Frames 12, Motion Response 0.2, Max Motion Weight 0.9, Resample Weight 0.8, Clip Width 2.5, Transparent Weight 0.9, Threshold 0.25, Sharpening 0.9; clamp and rejection at their defaults. `GPU.FeatureTest` runs the Custom path with every technique on. |
| "Alternate Grading" (inactive, FXAA) | `TAAHistoryClamp` MinMax, `TAADisocclusionRejection` false; the preset at its default |

Together they give every new property a non-default value, before and after T2's flip. No new log message is added.

**Samples and template.** No scene file changes in T1 (the default is S2's look). From T2 the Basic 3D template and the
three TAA samples take Sharp by default; each sample's command script and `Editor.Templates` check `TAAPreset` == Sharp.
Tetris stays on FXAA.

**T2's command additions**, for the evidence:
- **`play.start {paused: true}`** (default false): the game starts paused before its first step (`fixedSteps` 0), also
  when it builds the scripts first.
- **`viewport.capture {live: true}`** (default false) returns what a view presented, with its history as it is. It
  renders nothing of its own and does not settle.
  - **`camera` is required:** `"editor"` is the Scene view (`EditorViewport::m_Renderer`), `"scene"` the Game view
    (`m_GameRenderer`). The size is the view's own.
  - It refuses `width`, `height`, `overlays`, `position`, `target`, `fov`, `debugView` and `waitForAssets`.
  - **`atFixedStep: N`** waits for the first frame the view renders after the game's `fixedSteps` reached N, and fails
    when step N already ran, play stops, or the view stops rendering.
  - **`frames: n`** (1-16, default 1) records that frame and the n - 1 following ones, each read back from the view's
    output before it renders again. Results: one image per frame (with `path`, numbered files), each with its
    `fixedSteps` and `temporalFrames`.
  - It fails with the reason when the view does not render (hidden, minimized or headless).
  - It is a query that may defer (`MayDefer`). The description and the strata-editor-automation skill say what it is
    for: seeing motion as it plays (trails, smearing, crawling) at exact game steps, armed before `input.sequence` or
    `play.step`.

## 6. Per feature: tests, coverage and the editor

Every new component and property is tested as the strata-add-component skill says (2.7): `Reflection` (the property
exists with its range or choices; invalid values are rejected), `Scene.InactiveProperties` (its inactive reason),
`Scene.Serialization` (a round trip keeps every value; a 0.5.0 file without the new keys loads with the defaults and no
warning; out-of-range values are clamped to the range, as every ranged property's are, and values of another type or
unknown names load with a warning), and `Editor.Commands` (`component.add` with values, `component.set`, undo restoring
the previous state; `component.get` reports the inactive reason). The steps below name the cases specific to each.

### 6.1 Motion vectors (S1)

- `SceneRenderer::GetVelocityTexture()`; `SceneRenderOptions::MotionVectors` asks for them in a frame (tests), TAA and
  SSR ask implicitly. The Viewport debug view `motion` is not an entity tint: `ViewportDebugView::Motion` makes the
  Viewport set `SceneRenderOptions::ShowMotionVectors`, which draws the velocity as colors over the image in
  `GPU.Overlays`; the `debugView` enum of `viewport.capture` and `viewport.setSettings` gains `"motion"`
  (`DebugViewSchema` and both descriptions).
- `GPU.SceneRenderer.MotionVectors`: a still camera over a still scene has zero motion everywhere (exact); a moving
  camera gives every pixel the reprojected motion computed on the CPU within 0.01 px plus 0.1% of the motion's length
  (RG16F, D2); a moved entity's pixels carry its motion and the others only the camera's; a skinned pose change, a
  mirrored instance and a LOD-fading instance carry theirs; a reused instance slot starts without motion; two renderers
  of one scene rendering at different times each see their own previous frame; a change log overflow (more than its cap
  between two frames) still gives exact motion; requesting motion vectors leaves the HDR and LDR images bit-identical,
  on both paths, with skinned and mirrored instances (one vertex module, D2); a renderer that never asks allocates no
  velocity target and no previous buffers. `GPU.SceneRenderer.CameraRelative`: motion 20 km from the origin is within
  0.05 px plus 0.1% of its length. `GPU.SceneRenderer.GPUDriven`: `Comparison` asks for motion vectors and requires
  bit-identical velocity textures on both paths. `Editor.Viewport`: `viewport.setSettings` and `viewport.capture` accept
  `"motion"` and still reject unknown names.
- Must-fail: drop the `EntityID` check (the reused-slot case fails); copy the previous data before the frame's draws
  instead of after (the moved-entity case fails); skip the full copy on overflow (the overflow case fails); draw the
  forward pass with the plain module while the prepass uses the velocity one (the identity case is expected to show
  EQUAL-test holes; should MoltenVK compile both alike, the commit says so, and the rule stands on 2.6).

### 6.2 TAA (S2)

- `GPU.SceneRenderer.TAA`: with a still camera the jitter changes the frame constants only (`CachedCascadeMask` stays
  full, LOD choices and culled counts equal a renderer without TAA); at `TemporalFrames` 8 a still edge between two
  colors of equal luminance (so D4's luminance weights are equal) equals the mean of its eight jittered frames rendered
  without history within 0.5% (HDR); over the next 32 frames every pixel stays within 15% of the spread of those eight
  samples around their mean (D4's bound is 13%); a moving object leaves no trail beyond 2 px after it stops; cuts, scene
  changes, resizes, a renderer that skipped a device frame and `ResetHistory` reset (`HistoryResets`, the output equals
  the current frame, the jitter is back at phase 0); frames slower than any wall-clock limit (the test sleeps between
  them) do not reset; FXAA scenes allocate no history; a renderer with `TemporalAntiAliasing` off renders a TAA scene
  with FXAA; both paths resolve the same image over 16 frames of a moving scene (within 1/255, identical IDs).
  `GPU.SceneRenderer.Overlays`: a grid line on a ground plane and world-space text on a wall cover the same pixels in
  all eight jitter phases; where a cube's silhouette hides a grid line, the hidden part differs by at most one pixel
  between phases.
- Components: `AntiAliasingMethod` is an enum {FXAA, TAA}, inactive while `AntiAliasing` is off; a 0.5.0 `PostProcess`
  without it loads as FXAA with no warning.
- Editor: `GPU.Editor.Viewport`: the selection outline covers the same pixels in all eight jitter phases;
  `viewport.capture` of a TAA scene waits for `TemporalFrames >= 8` (bounded, `temporalFrames` in its result) and
  equals the Viewport's eighth frame after `ResetHistory` within 1/255; with TAA, PCSS and GTAO noise changes per frame
  and the converged shadow edge is smoother than one frame's (lower gradient energy). `GPU.Editor.Thumbnails`: the
  project thumbnail of a TAA scene is taken at `TemporalFrames >= 8`, and an asset thumbnail of a TAA scene renders with
  FXAA. `Editor.Templates`: the Basic 3D template uses TAA.
- Must-fail: jitter `SceneCamera::Projection` (the cascade cache case fails); no reset on a cut (the reset case fails);
  no clamp (the trail case fails); the outline from jittered IDs (the outline case fails); the weight 1/8 from the first
  frame after a reset (the convergence case fails); no overlay depth bias (the ground-plane grid case fails).
- Feature test: the `Environment` entity's `PostProcess` uses TAA (the other keeps FXAA, `AntiAliasing` false);
  `GPU.FeatureTest` checks `TemporalFrames` grows.

### 6.3 Cascade blending (S3)

- `GPU.SceneRenderer.Shadows`: a plane receiving a shadow across a cascade split has no step larger than the
  in-cascade filter noise (the same scene with `CascadeBlend` 0 shows the seam: the test requires both); at
  `CascadeBlend` 0.5 every band pixel (before its split) lies inside cascade c+1's square; shadows fade to zero at
  `ShadowDistance`; the cascade caching case keeps passing. `GPU.SceneRenderer.GPUDriven`'s shadowed scene passes with
  blending on (the moved near ends reach both paths).
- Components: `CascadeBlend` in [0, 0.5], inactive without `CastShadows`; a 0.5.0 light without the key loads with the
  new default 0.1 (the deliberate change of D6).
- Must-fail: do not move slice c+1's near end (the band case fails at `CascadeBlend` 0.5).
- Feature test: `Sun.CascadeBlend` 0.2. Thumbnails render one cascade, whose band fades the last tenth of the shadow
  distance (72 to 80 m under the preview sun), which large models reach: `ThumbnailCache::c_Version` 4 (S3). Values out
  of range in files are clamped, as every ranged property's are (no warning); values of another type warn.

### 6.4 Height fog (S4)

- `GPU.SceneRenderer.Fog`: pixels at known distances and heights match the CPU's analytic integral within 1/255
  (neutral post); transparent surfaces are fogged like opaque ones at the same depth; the sky gets `MaxOpacity`; with
  two active fog components the first applies; a scene without the component renders bit-identical to the same scene
  with a disabled `HeightFog` and with one of `Density` 0 (the fog path runs and adds exactly nothing).
- Components: `HeightFog`'s ranges (`Density`, `StartDistance` at least 0, `HeightFalloff` above 0, `MaxOpacity` and
  `SunScattering` in [0, 1], `Color` a color); `Editor.Commands` also checks the Sky section's "Add Height Fog" path
  (one undo step).
- Must-fail: the transparent variant without fog.
- Feature test: a `Fog` entity with every property non-default. Open World: light fog that keeps the windowed image's
  `--dominant green 5` passing (the fog is tuned, the check is not touched).

### 6.5 Light units (S5)

- `Renderer.LightUnits` (new suite, the `Renderer` shard): every light's and the sky's intensity description and
  tooltip names its unit (lux, candela, nits), and so do the emissive properties of the material.
- `GPU.SceneRenderer.Lighting`: a closed scene of matte materials (geometry covers every pixel; the clear color and the
  sky black, since they do not scale with the light) lit by a 65,536 lux sun with manual exposure -16 renders
  bit-identical to the same scene at 1 lux with exposure 0 (powers of two, no value near the half-float limit). As
  landed, the scene also holds a point and a spot light scaled alike (4 and 8 cd at 1 lux), so candela shares the scale.
- Must-fail: a description without its unit (the suite fails).

### 6.6 Point and spot shadows (S6)

- `GPU.SceneRenderer.LocalShadows`: a point light shadows casters in all six directions with no seam at face edges; a
  spot light's shadow lies where the CPU's projection predicts; no acne at grazing angles; with more shadowed lights
  than `MaxShadowedLocalLights` the most relevant keep shadows; a still scene redraws no view after it settles
  (`LocalShadowViewsDrawn` 0), a moving caster redraws only the views it reaches, a moved light only its own; the redraw
  budget holds (at most 12 views per frame) with `SpreadLightingUpdates`; without it, and with `CacheLocalShadows` off,
  every view of every shadowed light is drawn in the frame and nothing fades; a 2048 atlas with 16 shadowed point lights
  shadows every light (tiles step down); skinned, mirrored and alpha-masked casters shadow; lights without `CastShadows`
  allocate no atlas and draw no view. `GPU.SceneRenderer.GPUDriven`: a `BuildLocalShadowed` scene (spots and points over
  the other builders' meshes) compares pixels and the local shadow counts (`LocalShadowCasters`, `LocalShadowDraws`
  through `Tests::GetDrawnCounts`), under `IndirectDrawMode::Auto`, `NoCount` and `PerDraw`.
  `GPU.SceneRenderer.Feedback`: local casters outside the view are requested like cascade casters.
  `GPU.Editor.Viewport`: a capture of a scene with 16 shadowed point lights shows every light's shadow in its first
  complete frame.
- Components: `CastShadows` (a 0.5.0 light loads without shadows), `ShadowBias`, `ShadowNormalBias` and
  `ShadowFilterRadius` (at least 0; the filter radius at most 8), inactive while `CastShadows` is off, on point, spot
  and directional lights.
- Must-fail: leave caster changes out of the key (the moving-caster case fails); skip the local views in `Cull.comp`
  (the comparison fails); swap two faces (the six-directions case fails).
- Feature test: `Lamp` and `Spot` cast shadows with non-default biases and filter radius.
- Samples (6.13): the Open World's lantern near the start and its campfires, the Third-Person's two lanterns. The
  runtime's perf log gains `LocalShadowLights` and `LocalShadowViewsDrawn` (`GameRuntime::MakePerfLogColumns`), and
  `StrataRuntime.OpenWorldWindowedPerf` requires `--nonzero LocalShadowViewsDrawn`.

### 6.7 Pre-exposure and the atmosphere (S7)

- `GPU.SceneRenderer.PostProcess`: a 100,000 lux sun over a glossy sphere, with the sun disk in view, leaves no pixel
  at the HDR clamp; the closed scene of 6.5 at 65,536 lux with exposure -16 now renders bit-identical to 1 lux with
  exposure 0 with glossy materials too (P = 2^-16 keeps the highlights in range); with automatic exposure, the closed
  scene at 65,536 lux and at 1 lux meters log2 exposures whose difference is within half a histogram bin (0.055 EV) of
  16, and the images agree within the brightness half a bin implies (4%, plus 1/255); P forced to 2^3 and 2^4 renders
  bit-identical images of a scene with a sky and of one without (the clear color is pre-exposed); P forced to 2^-8, 1
  and 2^3 renders a neutral-post scene bit-identically where no value overflows or becomes subnormal; automatic exposure
  adapts to the same value with pre-exposure; a fresh renderer reports `PreExposureSettled` within a few frames, and a
  capture of a 100,000 lux scene waits for it; the TAA history survives a pre-exposure step without a flash (no pixel
  changes by more than the scene's own change). The CPU/GPU comparisons force one P on both renderers.
- `GPU.Atmosphere`: the zenith is bluer than the horizon at noon and the horizon redder than the zenith at sunset; the
  GPU transmittance LUT matches the CPU evaluation within 1%; the light's color at sunset is the CPU transmittance times
  its color; the environment refreshes only after the sun moved 0.5 degrees, never more than one face per frame, and
  switches only when complete (`EnvironmentUpdates`); procedural and map skies are unchanged (existing
  `GPU.ProceduralSky` passes).
- Components: `Source` gains `Atmosphere`; `AtmosphereDensity`, `AtmosphereHaze` and `GroundAlbedo` have ranges and
  are inactive under the other sources, whose properties are inactive under it; a 0.5.0 `SkyLight` loads unchanged; a
  `PostProcess` without `AutoExposureMaxEV` gets 16, and a file storing 12 keeps 12.
- Must-fail: write HDR without pre-exposure (the 100,000 lux case saturates); round P to a non-power of two (the forced
  2^3 and 2^4 case fails); the clear color without P (the skyless case fails); skip the CPU transmittance (the sunset
  light case fails).
- Feature test: an "Atmosphere Sky" `SkyLight` (not the first active one) with `Source` Atmosphere and non-default
  atmosphere properties. Open World: the sky uses the atmosphere, `DayNight` drives the sun only (its sky-color fields
  go), `AutoExposureMaxEV` is 16; the command script checks them and the image check passes.

### 6.8 Reflection probes (S8)

- `GPU.SceneRenderer.ReflectionProbes`: a mirror sphere inside a probe's box reflects the probe's capture, not the sky;
  two overlapping boxes blend; box projection places a reflected wall correctly; a probe's diffuse coefficients light a
  matte sphere like its capture's irradiance (within 2%); more visible probes than slots keep the strongest; probe
  textures are requested through the frame's resolve cache, held by the probe cache until converted, then released;
  with `SpreadLightingUpdates` at most two probes convert per frame, without it every visible one in the first frame; a
  scene without probes binds the one-slot array and makes nothing else.
- `GPU.Editor.Lighting`: `lighting.bakeProbes` writes a new `.hdr` asset per probe, named after its UUID, and sets
  `Capture` in one undo step; two probes with the same name under different parents get different files; a rebake
  writes a new file and moves the previous bake's file to the trash, undo restores the previous file and handle, redo
  the new ones; a `Capture` set to another texture (the sky's map) leaves that file byte-identical; the command defers
  until the import completes, and the main thread never waits for it; it refuses while playing and rejects entities
  without the component without side effects. `Editor.Journal`: `lighting.bakeProbes` is in `c_EditCommands` and
  `c_DeferringCommands`. `Asset.Dependencies`: a scene with a baked probe references its texture.
- Components: `ReflectionProbe`'s ranges (`Extents` above 0, `BlendDistance` and `Intensity` at least 0) and its asset
  property.
- Must-fail: box projection off (the wall case fails); release the hold before conversion (the conversion never
  completes and the hold case fails); write over the file `Capture` names (the other-texture case fails).
- Feature test: a `Probe` entity with every property non-default and a committed `Assets/Lighting/Feature/Probe.hdr`
  (generated by `StrataTests/FeatureTest/Tools/GenerateAssets.py`); `StrataTests/Editor/FeatureCommands.json.in` runs
  `lighting.bakeProbes` on a copy. Third-Person: probes in the course, baked through the editor.

### 6.9 SSR (S9)

- `GPU.SceneRenderer.SSR`: a mirror floor reflects a red cube at the CPU-predicted pixels; rough surfaces fade to the
  probe or sky; off-screen hits fall back; without TAA nothing runs (no surface target); both paths give the same image.
- Components: the three properties' ranges, inactive without TAA; a 0.5.0 `PostProcess` loads with SSR off.
- Must-fail: reproject without the hit's velocity (a moving cube's reflection lags).
- Feature test: the TAA `PostProcess` turns SSR on with non-default intensity and roughness. Third-Person: SSR on in
  both scenes; `ThirdPersonWindowedPerf` requires `--nonzero GPU.Reflections` (the main menu's SSR).

### 6.10 Sun extraction (S10)

- `Asset.HDRSun`: a synthetic equirectangular sky with a disk of known radiance and direction gives the direction
  within 0.1 degree and the illuminance within 1%, also across the seam; a flat peak is reported as clipped.
  `Asset.TextureImport`: `RemoveSun`'s descriptor (bool, default false, tooltip), its default key, and its inactive
  reason for every usage but HDR; `RemoveSun` leaves no texel above the threshold; with `RemoveSun` false the cooked
  bytes equal those of settings without the key (the A/B of 7.1 compares the samples' and the feature test's cooked
  `.hdr` files with 6425b57b's). `Editor.ImportSettings`: `asset.setImportSettings` accepts `RemoveSun` on an `.hdr` and
  refuses a non-boolean. `GPU.Editor.Lighting`: `lighting.extractSun` writes a light whose direction matches where the
  sky shows the sun (render the background, find its brightest pixel), with the sky's `Rotation` applied, in one undo
  step, and refuses while playing. `Editor.Journal`: `lighting.extractSun` is in `c_EditCommands` and
  `c_DeferringCommands`.
- Must-fail: ignore `Rotation` (the direction case fails).
- Feature test: `Sky.hdr` gains a sun (the generator) and `RemoveSun` in its `.meta`; `FeatureCommands.json.in` runs
  `lighting.extractSun`. Third-Person: its hand-placed sun is replaced by the extracted one in both scenes, and
  `AutoExposureMaxEV` is set to 16; the command script checks them.

### 6.11 Clearcoat and transmission (S11)

- `Renderer.Material`: round trip of the new properties; 0.5.0 materials load with defaults; ranges (`IOR` at least 1,
  factors in [0, 1]). `Asset.Gltf`: the four extensions' factors and textures import (a test model per extension),
  unknown ones still warn. New suite `GPU.SceneRenderer.Materials`: clearcoat adds a second highlight and conserves
  energy (no pixel brighter than the coat's and base's sum); transmission shows the background offset by `IOR`; glass
  writes depth, entity IDs and velocity; no scene-color copy without a transmissive draw (`SceneColorCopies` 0); the
  copy's format is B10G11R11 or RGBA16F as the device allows (checked as the AO test checks its format), and
  `SceneRendererSpecification::ExtendedStorageFormats` false (test-only, as `IndirectDrawMode` forces the indirect
  fallbacks) runs the RGBA16F fallback; the transparent comparison of `GPU.SceneRenderer.GPUDriven` includes glass.
  `GPU.Editor.Viewport`: a click on a glass block selects it, and its outline is drawn.
- Must-fail: forget the coat's Fresnel on the base (the energy case fails); glass without the entity-ID attachment (the
  click case fails).
- Feature test: `Feature.stmat` gets clearcoat, a new `FeatureGlass.stmat` transmission, `Platform.gltf` the four
  extensions. Thumbnails: `ThumbnailCache::c_Version` 5 (S3 made it 4) if material previews change (checked by rendering
  one). Physics Playground: clearcoat balls and glass blocks.

### 6.12 Inspector, commands and MCP

- New components and properties get the generic Inspector; inactive reasons as in section 4. Component-specific UI: a
  Bake button on `ReflectionProbe` (runs `lighting.bakeProbes` for the selection); in `DrawSky`, "Extract Sun" (map
  skies), "Add Height Fog" (no fog yet) and "Bake Reflection Probes". Nothing is added near `DrawCollisionLayers`
  (section 12).
- Commands (`StrataEditor/src/Editor/EditorLightingCommands.cpp`): `lighting.bakeProbes` {entities?} and
  `lighting.extractSun` {sky?, light?}, both edits (`EditorCommandKind::Edit`) with complete schemas, descriptions for
  agents, `MayDefer`, `CheckSceneEditable`, one undo step, refused while playing (as `prefab.apply` is: they write from
  the edited scene). Both join `c_EditCommands` and `c_DeferringCommands` of `Editor.Journal`. The `lighting.` prefix
  maps to the `scene` toolset (`EditorMcpToolsetsTests.cpp` checks it); `core` is unchanged.
- `editor.perf`'s description and the profiler tests list the new stages; the strata-editor-automation skill names the
  commands, the `motion` debug view and what captures wait for.

### 6.13 Defaults and the samples

| Sample | Adopts | Proof |
| --- | --- | --- |
| Open World | TAA, cascade blending, height fog, local shadows (a lantern lit at all times in a shaded spot in view of the start, not driven by `DayNight`; the five campfires), pre-exposure and atmosphere (`AutoExposureMaxEV` 16) | command script; windowed image (`--dominant green 5` kept); the existing `StrataRuntime.OpenWorldWindowedPerf` extended with `--nonzero GPU.TAA --nonzero LocalShadowViewsDrawn` |
| Third-Person Adventure | TAA and SSR in both scenes (`MainMenu`, the windowed run's scene, and `Course`); two shadowed lanterns in shade in the course (an overhang or a passage, chosen by looking in the editor); reflection probes in the course; the extracted sun in both scenes (`AutoExposureMaxEV` 16) | command script (properties); windowed image; the existing `ThirdPersonWindowedPerf` extended with `--nonzero GPU.TAA` (S2) and `--nonzero GPU.Reflections` (S9) |
| Physics Playground | TAA (many moving bodies: motion vectors at scale), clearcoat and glass | command script; windowed image; the existing `PhysicsPlaygroundWindowedPerf` extended with `--nonzero GPU.TAA` |
| Tetris | unchanged (FXAA) | as today |

Sample scenes are changed through the editor (the strata-editor-automation skill), opened in place only to change them,
and each `StrataTests/Editor/<Sample>Commands.json.in` keeps passing (extended where a feature has a property to check).
A sample adopts a feature in the step that lands it. The campfires are lit only at dusk in Play; the final report tells
the owner how to see their shadows (Play and wait about 225 s, or set the Sun's `DayNight` `TimeOfDay` to 18.5 before
Play), while the lantern near the start shows a shadowed local light in the editor at once.

### 6.14 TAA quality (T1, T2)

**Where the cases live.** New cases go in `StrataTests/src/Renderer/GPUSceneTAAQualityTests.cpp`, in the existing suite
`GPU.SceneRenderer.TAA` (no new shard).
- S2's file-local helpers and scenes (`CreateRenderer`, `ReadHDR`, `ReadIDs`, `CheckReset`, `EdgeScene`,
  `TemporalScene`, `LoadAssets`, `FinishFrame`) move into `StrataTests/src/Renderer/TAATestUtils.h`.
- `AddTemporalPostProcess(scene, preset)` moves to `SceneRendererTestUtils.h`, and the two scenes take the preset.
- S2's cases pass Original, and their assertions are unchanged. Three S2 bodies run per preset: still edge, PCSS/GTAO
  noise, and resets. Original keeps S2's bounds; Sharp (T2) gets the bounds below.
- Every GPU case uses `AddNeutralPostProcess` and ends with `CHECK(gpu.GetNewErrorCount() == 0)`. Each technique's case
  sets Custom with only that technique differing from Original.
- **Glints:** a mid-gray plane with small emissive squares, 2 px across and 6 px apart, built from geometry: sparse
  outliers, as sunlit foliage gives.
- **Texture detail:** an in-memory texture of 3-6 px periods at low contrast (R about 0.1-0.2 in compressed luminance)
  on a fronto-parallel plane mapped 1:1 to pixels, with `TextureStreaming = false`.

**Original and Custom (T1)**
- **Original equals Custom with Original's values.** 16 frames of a moving scene with a moving transparent quad: the
  resolved history's RGB bytes and the output bytes are identical.
- **CPU (`Renderer.Components`).** `GetTemporalAntiAliasingSettings` returns equal settings for those two, also when
  Custom's inactive `ClipWidth` and `DisocclusionThreshold` hold other values. `GetTemporalAntiAliasingCustomStart`
  returns Original's table from Original with untouched tunables, Sharp's from Sharp, and tuned values unchanged.

**Variance clipping**
- **Trail.** A cube crosses the glints at 6 px per frame under a still camera. Six frames after it left, measure the
  crossed strip's mean absolute luminance difference from the converged background.
  - The gray, square and cube luminances are pinned from a CPU computation of both clips over the pattern. They are
    chosen where the predicted Variance/MinMax ratio is at most 0.35.
  - MinMax at least 0.02 (the trail exists); Variance (`ClipWidth` 1.25) at most half of MinMax.
- **Noise.** S2's PCSS/GTAO case under Variance: the converged roughness (squared second differences) stays below 0.6
  of one frame's (S2 asserts 0.5 under Original).

**Resampling**
- **Slow pan.** The camera translates parallel to the texture plane at 0.5 and at 1.5 px per frame for 32 frames. The
  measure is the resolved detail amplitude (standard deviation of luminance over the plane) relative to the still
  converged image's. With 0.25 it is at least 1.2 times Original's and at least 0.9 of the CPU model's prediction for
  the pattern (`w / (1 - (1 - w) * a)` per period: 0.74 with 0.25 and 0.55 under Original at a 4 px period).
- **Integer motion.** At 1.0 px per frame, `ResampleWeight` 0.25 equals 0 bit for bit.

**Reactive mask**
- **HDR alpha.** Two overlapping blended quads with alpha 0.5 and 0.25 leave 0.375 where they overlap and 1 elsewhere,
  read through `GetHDRTexture` within 1/1024, on both paths.
- **Moving quad.** A red quad with alpha 0.5 moves at 4 px per frame over the still texture plane. Pixels it has covered
  for exactly 2 frames: their error against the current composite with `TransparentWeight` 0.5 is at most half the
  error at 0 (the formula gives (0.5 / 0.875)^2 = 0.33 before the clip). The trail behind it does not grow.
- **Still quad.** A still transparent quad's edge pixels vary over frames 9-40 by less than the jittered frames
  themselves do (no history), so the mask is not a bypass.

**Disocclusion rejection**
- **True positive.** A near red box moves sideways at 20 px per frame in front of a wall textured red and white.
  - In the first frame after the uncovering, pixels at least 2 px from the box's new silhouette equal the current frame
    exactly with rejection on (0.05).
  - Without rejection, the mean luminance difference over the band's white texels is at least 0.05.
  - Uncovered sky is rejected too.
- **No false positives at the lowest bound, 0.02.** Rejection on equals rejection off bit for bit in each of these:
  - A camera rotating in place over boxes, a ground plane and the sky.
  - A dolly backward at 0.2 m per frame at 5 m over a grazing ground plane (twice the threshold per frame).
  - A still jittered view over 40 frames.
  - An orthographic camera with Near -10 panning over boxes that lie between its near plane and its position.
  - A perspective camera moving 20 km from the origin.
- **Pre-exposure.** Across forced pre-exposure steps up and down, the history alpha is unchanged and rejection on equals
  rejection off bit for bit.

**Sharpening**
- **Detail.** On the texture plane (still, at 8 frames), the output amplitude at Sharp's value is at least 8% above the
  amplitude at 0.
- **No feedback.** Over 32 frames of a moving scene, the resolved history is bit-identical with sharpening 1 and 0.
- **Halos.** On a resolved black-to-white edge, no output pixel exceeds its unsharpened five-tap range by more than 2%
  of the edge's range at Sharp's value, or 5% at 1.
- **Stability.** On a still view, the output's per-pixel temporal standard deviation over frames 9-40 is at most 1.5
  times the value without sharpening.
- **Off elsewhere.** An FXAA scene and a renderer without TAA render identically with `TAASharpening` 1 and 0.

**Sharp (T2, with the tuned values)**
- **Convergence.** On S2's still-edge scene, at frame 8 edge pixels are on average within 5% of their spread from the
  eight-phase mean, and every pixel within 25%. Over frames 9-40, every resolved pixel stays within 30% of its spread
  (Original: 15%), and the output's per-pixel temporal standard deviation is at most 1.5 times Original's. These bounds
  are fixed: if Sharp misses them, its values move within their bounds.
- **Trail regression.** On the trail case, Sharp's residual is at most half of Original's.
- **Resets.** `CheckReset` (cuts, resizes, `ResetHistory`) passes under Sharp.
- **Both paths.** Over 16 moving frames with a transparent quad, the two paths agree within 1/255, with identical IDs.
- **Pre-exposure.** S7's "follows a pre-exposure step without a flash" runs under the default preset, Sharp.
- **Other suites** keep passing under Sharp: overlays, `GPU.Editor.Viewport` (the capture still equals the 8th frame of
  the view), thumbnails, game UI, the game renderer, the feature test.

**Components and editor**
- **`Reflection`.** The enums' names, values and defaults. Ranges and tooltips. The tunables' defaults equal
  `c_TAASharpSettings`. `Scene.InactiveProperties` covers every reason above.
- **`Scene.Serialization`.** A TAA `PostProcess` without the key loads as the default preset without warnings.
  `"Original"` and 1 both load as Original. An unknown name warns and keeps the default. Out-of-range tunables are
  clamped.
- **`Editor.Commands`.** `component.set` to Custom with tunables is one undo step. Out-of-range values are refused
  without side effects. The exact `inactive` maps (`EditorCommandTests.cpp`) are extended, not weakened: the new rows
  are inactive under FXAA and with anti-aliasing off.
- **Inspector (`ImGuiHarness`).** Through the `TAAPreset` row:
  - Original → Custom writes Original's table in one undo step.
  - Sharp → Custom changes no tunable.
  - Tuned Custom → Original → Custom keeps the tuning.
- **`GPU.Editor.Viewport` (T2).**
  - `viewport.capture {live, camera: "scene", atFixedStep: N, frames: 2}` equals the Game view renderer's output read
    back in the frames that ran step N and the next.
  - A paused frame after them differs from the last one (the history moved on).
  - It refuses a missing `camera`, each refused parameter, a step already run, and a view that did not render.
  - After `play.start {paused: true}` and five frames, `fixedSteps` is 0.

**Must-fail checks.** Each change below must make the named case fail:

| Change | Case that fails |
| --- | --- |
| The MinMax clamp used under Variance | the trail case |
| No resampling term | the slow-pan case |
| `f` measured from texel corners instead of centers | the integer-motion case |
| The transparent alpha blend left at One | the HDR alpha and moving-quad cases |
| Coverage not saturated (`r = max(1 - a)`) | the moving-quad case |
| `TransparentWeight` treated as a full bypass | the still-quad case |
| The pixel's own depth against the per-pixel stored depth (no nearest or farthest) | the still jittered view |
| The current distance compared without the previous camera | the dolly case |
| The margin `Threshold * max(expected, Near)` | the orthographic case |
| The stored distance multiplied by `HistoryScale` | the pre-exposure rejection case |
| Settings not normalized | the Original/Custom CPU case |
| Sharpening written into the history | the no-feedback case |
| Overshoot unbounded (no limiter) | the halo case |
| Sharpening on FXAA frames | the off-elsewhere case |

**Visual evidence (T2).**

**Where the files go.** `/Users/mikeyang/Documents/gameengine/.worktrees/render-quality/build/taa-evidence/`
(`/build/` is git-ignored). Each sample is copied there (never opened in place) and opened in a windowed editor. The
Game view is shown at a fixed 1920x1080 (`viewport.setSettings {gameView}`), so crops are 1:1 and motion has a known
speed in pixels per frame. Hidden views skip frames and reset their history (D4).

**Each motion run, per mode** (FXAA, Original, Sharp):
1. Set the edited scene's `PostProcess` with `component.set`.
2. `play.start {paused: true}`.
3. Send `viewport.capture {live: true, camera: "scene", atFixedStep: N, frames: 8, path}`. It answers later.
4. Once it is pending (the automation status counts pending requests), run `input.sequence` with N + 8 steps.
5. Read both answers, then `play.stop`.

Every mode then shows the same game state at each captured step; only the jitter phase can differ.

| Run | Input, as the sample's own controls define it | N |
| --- | --- | --- |
| Third-Person | Forward held while the mouse turns the camera at a constant rate | 90 |
| Open World, slow pan | The camera turned by the mouse at about 0.5, then 1.5 px per frame at the screen's center, the player still | 90 |
| Physics Playground | Bodies falling from the start, or the sample's spawn input | 45 |
| Each sample's still view | A settled `viewport.capture`; then, after `editor.wait {frames: 40}`, `viewport.capture {live, camera: "scene", frames: 2}`, whose absolute difference times 8 is the flicker map | — |

**What to produce.** A standard-library Python script, kept in the evidence folder and not committed, writes:
- every image;
- side-by-side sheets in the order FXAA | Original | Sharp;
- 3-4x nearest-neighbor crops of the same rectangles in all three modes, as 8-frame strips for motion (crawling cannot
  be judged from one frame):
  - a high-contrast silhouette against the sky;
  - foliage or thin detail (grass, branches, fences, rails);
  - texture detail during the slow pan;
  - a moving object's trailing edge, including the band it uncovers;
  - a transparent surface. Where a sample shows none (glass arrives with S11), use the moving-quad case's frames from a
    run of the GPU test.

The implementer opens every image and writes one sentence per image on edges, texture detail, trails, ghosting, halos
and flicker. The report names the sheets and strips for the orchestrator to send to the owner. It also gives the
three-step A/B in the editor: open a sample, Play, and switch TAA Preset in the Inspector while moving (settings never
reset the history).

**Sharp's final values** are chosen within their bounds so that the captures show all of these:
- Texture detail visibly crisper than Original, in still views and slow pans, close to FXAA.
- Edges as smooth as Original's: no stair-steps, and no crawling in the flicker map or the strips.
- No trail behind the running character or the falling bodies beyond what FXAA shows plus 2 px.
- No smeared foliage while panning.
- No visible halo around sunlit silhouettes.

If no values within the bounds achieve all of these, the report says which goal is missed and why; the bounds are not
exceeded. The values and the reasons go in the tuning commit's message.

## 7. Deliverables and acceptance

Each step is one or more commits that build and pass every suite with the lint clean; `ctest` runs use the `macos-debug`
and `macos-release` presets of the worktree's tree; each step runs `StrataTests.GPU` under validation (every case ends
with `CHECK(gpu.GetNewErrorCount() == 0)`) and `StrataTests.FeatureTest`.

- **S1 Motion vectors**: D2, 6.1; the `motion` debug view; the copy timed in `GPU.Post` (D15). *Accept:* 6.1's suites,
  the identity of the image with and without motion vectors among them.
- **S2 TAA, in the engine and the editor**: D3, D4, D5 (without the lighting and pre-exposure waits, which come with
  S6 and S7), D15 (`GPU.TAA`), 6.2; the Basic 3D template and the three samples on TAA. `AntiAliasingMethod` is
  registered only in the commits after the editor integration, so no commit lets a scene choose TAA before the outline,
  captures and thumbnails handle it. *Accept:* 6.2's suites; `GPU.SceneRenderer.GPUDriven`, `.Feedback`, `.Shadows`,
  `.PostProcess`, `GPU.Editor.*`; the samples' command scripts, windowed images and perf logs; the A/Bs of 7.1.
- **S3 Cascade blending**: D6, 6.3. *Accept:* 6.3; the A/B of the measured shadowed scenes (7.1).
- **S4 Height fog**: D9, 6.4. *Accept:* 6.4; the Open World's image check.
- **S5 Light units**: D7's units, 6.5, the DG section, `Docs/Manual/Lighting.md`. *Accept:* 6.5.
- **S6 Point and spot shadows**: D8, D16's atlas bindings, `SpreadLightingUpdates` (D5), 6.6. *Accept:* 6.6's suites;
  the Open World's lantern and campfires and the Third-Person's lanterns; the perf-log check; provisional metrics of
  7.1. Push `ci/render-quality`: **the core is complete here.**
- *Cut line.*
- **S7 Pre-exposure and atmosphere**: D7's pre-exposure, D10, the captures' exposure wait (D5), 6.7.
  **S8 Reflection probes**: D11, D16, the required `imageCubeArray`, 6.8, `GPU.Reflections`.
- **T1 TAA quality presets and tuning**: D17's settings and every technique, reachable through Custom; the default
  stays Original (S2's look). *Accept:* 6.14's T1 cases and must-fail checks; `StrataTests.GPU` under validation; the
  FXAA-scene perf comparison of 7.1.
- **T2 Sharp tuned and made the default**: `play.start {paused}` and `viewport.capture {live, atFixedStep, frames}`;
  the evidence (6.14) and Sharp's tuning within its bounds; Sharp as the default in the template and the three TAA
  samples; the per-preset perf; the Manual page, DG and skills (section 8). *Accept:* 6.14's T2 cases; the evidence
  sheets and strips, each described, for the owner.
- **S9 SSR**: D12, 6.9. **S10 Sun extraction**: D13, 6.10. **S11 Clearcoat and transmission**: D14, 6.11.
- **S12 Finish**: A/Bs of 7.1 on the final branch, docs and skills (section 8), the Roadmap's Status, CI green on
  `ci/render-quality`.

### 7.1 Budgets and measurements

- Untouched: every existing entry's `Measured` and `Budget`. Every measured scene has a shadowed sun with the cascade
  cache off (2.7), so two always-on changes reach them: cascade blending from S3 and the pre-exposure from S7; every
  other feature defaults off, so otherwise the measured scenes run the same passes. Each step A/Bs the measured
  `PerfGPU.Renderer.*`, `PerfGPU.Renderer.EditFrame*` and `PerfGPU.Editor.EditFrame*`/`PlayFrame*` metrics against
  6425b57b's Release build (ratio of medians at most 1.02; above it the cause is found and fixed before the step lands).
  For S3 the ratio applies with blending on, its default; the step also reports `Render100kCubes`, `Render300kCubes`
  and the provisional `Render100kCubesShadowed` and `Render300kCubesShadowed` with blending on and at `CascadeBlend` 0.
  If blending cannot meet 1.02 once its cause is understood and what can be fixed is fixed, the step's commit states the
  ratio as the known cost of the default, and the reference machine re-measures it when it returns (budgets are never
  changed from Mac numbers). S7 A/Bs the pre-exposure the same way.
- Images: the same two builds capture fixed scenes (each sample's start view, the feature test's, a
  `GPU.SceneRenderer.GPUDriven` scene) through `viewport.capture`, compared by a script in the scratch area; each step
  states that scenes which do not opt in are unchanged (shadowed ones at `CascadeBlend` 0 from S3), and S10 that the
  cooked `.hdr` files are byte-identical.
- New provisional metrics (`ms`, no targets: the reference machine sets them), each with a CPU median and a GPU busy
  median (`...GPU`), at 1920x1080 in `RendererPerfTests.cpp`: `Render100kCubesTAA`, `LocalShadows16Lights` (cache off:
  every view of every light each frame, D8) and `LocalShadows16LightsCached`, `HeightFog`, `Atmosphere` and
  `AtmosphereSunMovingMaxFrame`, `ReflectionProbes16`, `SSR`, `Transmission`; and `TAALocalShadowsPeakMemory` (MiB,
  measured in a helper process: a TAA scene of 100,000 cubes with 16 shadowed point lights), which bounds the
  per-renderer cost of D2 and D8. The GPU stage times of each feature's pass on the Mac are reported in the step's
  commit message and the final report.
- `PerfGPU.Samples.OpenWorldTour.*` (provisional) runs before and after the Open World adopts its features; the report
  gives both.
- T1 and T2 (D17), on the Mac: a Release build under `Tools/PerfLock.py`, three or more interleaved pairs, ratio of the
  medians. No Mac number becomes a budget.
  - **T1: FXAA scenes.** `Render100kCubes` and the measured `PerfGPU.Renderer.*` and `EditFrame*`/`PlayFrame*` are
    compared against the branch before T1: ratio at most 1.02.
  - **T2: per preset.** Medians of `GPU.TAA` and `GPU.Post` for FXAA, Original, Sharp, and Custom with each technique
    alone, at 1920x1080 in the 100,000-cube TAA scene, stated in the tuning commit's message and the report.
  - **T2: new metrics.** `RendererPerfTests.cpp` gains `PerfGPU.Renderer.Render100kCubesTAAOriginal` and `...GPU` as
    provisional entries in `Budgets.json`. `Render100kCubesTAA` then measures Sharp, the default, and its entry is
    untouched.
  - **Memory.** `TAALocalShadowsPeakMemory` is reported unchanged.

## 8. Docs and skills

- `AGENTS.md` (378 code points below the limit, measured with `Docs.AgentGuide` after merging `main`): the Rendering
  digest gains at most one rule, in place of words, not beside them: "Temporal effects jitter only the frame's copy of
  the camera and keep their history per renderer; renderers of single images settle or turn TAA off." Local shadows fall
  under the existing "decided twice" and cache rules, whose wording names "shadow views" instead of "cascades" where it
  does. The file stays under 40,000 code points.
- `Docs/DeveloperGuide.md`, Rendering: new subsections "Motion vectors and TAA" (with the memory per renderer), "Light
  units and exposure", "Local light shadows" (with the atlas's memory), "Reflections" (probes, bake, SSR), "Fog and
  atmosphere" (S4 adds it as "Height fog", which S7 extends and renames); "Scene renderer and lighting" (the pass list,
  the units paragraph replaced), "Render scene" (the probe cache as a third cache that keeps assets across frames),
  "Lights, shadows and HDR" (cascade blending, the local shadows sentence), "GPU-driven path" (local shadow views and
  buckets), "GPU timing" (the stages), "Overlays" (the selection mask, the overlay depth bias, glass outlined),
  "Materials" (the new properties and maps), "Textures" (`RemoveSun`), "Feature test" (the new entities and assets),
  "macOS and MoltenVK" (binding counts), "Building" (the required `imageCubeArray`). Editor: "View commands"
  (`debugView` `"motion"`, what captures wait for and the result's new fields), "MCP toolsets" (the `scene` toolset's
  `lighting.` prefix), the `lighting.*` commands.
- `Docs/Architecture.md`: the frame's pass order, the per-renderer history, the probe bake's data flow.
- `Docs/Manual/Lighting.md` (new): units with reference values, exposure, the sky sources, fog, probes and how to bake
  them, local shadows and their cost. Linked from `Docs/README.md`.
- `Docs/Roadmap.md`: Status when the stream lands; "Later": what section 10 defers.
- Skills: `strata-make-a-game` (lighting advice: units, TAA, shadowed lights' cost, baking probes, fog),
  `strata-editor-automation` (`lighting.*`, the `motion` debug view, what captures wait for), `strata-scripting` (the
  new properties a script may drive, e.g. fog density or the sun for time of day).

**T1 and T2 (D17).**

**DG "Motion vectors and TAA"** (T1, completed in T2): the presets and their tables; each technique as built; the HDR
alpha convention (writers before the TAA write 1, blended transparent surfaces multiply by 1 - a, glass writes 1); the
history's alpha holds the signed view distance; sharpening lives in the tone mapping and never enters the history; the
resolve's constants paragraph points to `c_TAAOriginalSettings`; the tests.

**DG elsewhere:** "Feature test" (the new values), "Inspectors" (the reasons and `UI::PropertyDrawers`), "View commands"
and "Play" (`live`, `atFixedStep`, `frames`, `play.start {paused}`; T2).

**`Docs/Manual/AntiAliasing.md`** (T2; new, linked under "Manual" in `Docs/README.md`), in plain words:
- FXAA versus TAA.
- How many frames TAA keeps. Frames 1-8 after a reset are averaged evenly. After that, at 1/8 per frame, the newest 6
  frames hold more than half of the weight and the newest 18 more than 90%. Motion and Sharp's terms shorten this.
- When trails appear and what removes them; that Tight clipping gains most where the surroundings are smooth.
- Each preset (Original is S2's TAA, unchanged), each tunable, and how Custom starts, with the `component.set` that
  starts Custom from Original.
- The limits: fast receding surfaces, the uncovered band's few softer frames, and slow pans under Original.

**Skills** (T2)
- strata-make-a-game: TAA Sharp for 3D games, Original for a softer and steadier look, Custom with Transparent Weight
  when effects ghost, FXAA for 2D and pixel art.
- strata-editor-automation: `play.start {paused}`, `viewport.capture {live, atFixedStep, frames}`, and that captures
  show Sharp's sharpening.

**What stays as it is:** `AGENTS.md` (39,724 of 40,000 characters; the details belong to DG) and `Docs/Architecture.md`
(the pass order is unchanged).

## 9. Risks

| Risk | Mitigation |
| --- | --- |
| TAA blurs or ghosts, and the owner judges by eye | trail and convergence tests; the `motion` debug view; samples checked in the editor at each step; T1/T2 |
| Temporal state breaks the CPU/GPU comparisons (draw order, occlusion phases) | jitter and history are per renderer, order-independent and counted in frames; the comparisons run with TAA in S2; one forced pre-exposure on both renderers |
| The selection outline and overlays under TAA | D5's mask pass and D3's overlay bias, with their own stability tests |
| Captures and thumbnails show unsettled frames | settle waits with bounds (TAA, pre-exposure); lighting drawn at once without `SpreadLightingUpdates`; tests of each |
| Shader variants multiply (prepass: entity IDs x velocity x surface; mesh: skinned x velocity, used by the prepass and the forward pass alike) | variants only where a feature needs one; pipelines keyed by the framebuffer variant; build time watched |
| MoltenVK limits (16 samplers, 31 storage buffers) | D16's counts; no new sampler |
| Requiring `imageCubeArray` rejects a device that ran 0.5.0 | every desktop Vulkan 1.2 GPU has it; the rejection names it; DG "Building" lists it |
| Memory per renderer (atlas, previous instances) with four renderers in the editor | smaller atlas for single images; made only when used; `TAALocalShadowsPeakMemory` |
| No GPU tests on CI; Windows and Linux GPUs unseen; the count indirect path unseen on this Mac | only core Vulkan features and gated ones; validation clean on the Mac; the reference machine runs `StrataTests.GPU` when it returns (Roadmap) |
| Half-float overflow with real units | D7 pre-exposure; `SanitizeHDR` on every new writer |
| Local shadow redraws spike when many casters move | per-frame view budget, nearest first; `LocalShadows16Lights` metrics; GPU-side culling |
| Cascade blending costs the measured scenes | S3's explicit A/B; the near end widened (the cheaper side); the cost stated if it stays |
| Probe bake reads back HDR on MoltenVK | `TextureReadback` (polled) as captures do; a GPU editor test |
| The atmosphere's environment refresh hitches | double-buffered, one face per frame; `AtmosphereSunMovingMaxFrame` |
| `GltfImporter` version 10 re-imports every model once | stated in the release notes; the samples' first open re-imports in the background |
| Fog or the atmosphere fails the Open World's green check | tune the scene, never the check |
| `AGENTS.md` headroom shared with `project-settings` | one rule at most, replacing words |
| T1/T2: Variance clipping pulls still edges off the mean of their phases, so they flicker | The clip runs in the weights' compressed space, intersected with the min/max box, with `ClipWidth` at least 1.0 for Sharp. The convergence and flicker checks fix the bounds. Original keeps S2's semantics. |
| T1/T2: Slow pans stay soft (the owner's complaint) | The resampling term, the slow-pan case and the slow-pan evidence. The Manual says Original stays soft there. |
| T1/T2: Sharpening amplifies what TAA leaves (the still-edge oscillation, PCSS/GTAO noise, LOD dither) or makes halos | Luminance only, in compressed space. The amount falls steeply with local contrast, and overshoot is bounded by a CPU-prototyped formula. The halo and stability cases. |
| T1/T2: A future writer of the HDR target breaks the alpha convention (glass in S11, a new effect) | DG states it, D14 says it for glass, and the HDR-alpha case fails on a wrong writer. |
| T1/T2: False disocclusion (surfaces receding fast, orthographic cameras, far views) | One-sided test, nearest against farthest, a margin positive for every camera, both sides clamped to `c_SkyDistance`. Documented. The no-false-positive cases at the lowest bound. |
| T1/T2: A driver compiles Original's unchanged math differently once the shader grows | S2's bounds have tolerances. The exact comparisons (reset passthrough, Original against Custom) hold by construction. |
| T1/T2: Greyed rows show stored values, not the preset's | The reasons quote the preset's values, and choosing Custom in the Inspector starts from the preset in effect. |
| T1/T2: Evidence compares different moments | Paused start and step-armed captures: identical game states, only the jitter phase differs. |
| T1/T2: Untuned Sharp ships as the default | The default stays Original until T2's tuning. |
| T1/T2: The cost of Sharp on the reference machine is unknown | Provisional metrics for both presets and the 1.5x target on the Mac. Measured again when the reference machine returns. |
| T1/T2: The owner finds Sharp too soft or too sharp | The sheets and strips go to the owner, plus Custom and values within stated bounds, so they can ask for another point. |
| The usage limit | the cut line; small visible steps before the largest; each step lands alone |

## 10. Deferred

- Render scaling, dynamic resolution and TAA as an upscaler (with `game-shell`'s quality presets); MSAA.
- Project-wide renderer defaults (a `Renderer.Defaults` section of Project Settings), with `game-shell`'s presets.
- Layered shadow rendering through `shaderOutputLayer`, occlusion culling of local shadow views, more than 16 shadowed
  lights per view, physically sized local light sources (PCSS) and contact shadows.
- Run-time probe capture and relighting for moving suns; cube texture assets; clustered probe lists.
- SSR on a nearest-depth pyramid; SSR for transparent surfaces.
- Volumetric fog, light shafts, aerial perspective in froxels; the atmosphere seen from altitude or space.
- Thickness maps, sheen, specular, iridescence and anisotropy extensions; ambient occlusion on glass; EXR import; area
  lights.
- Fewer GTAO samples with temporal accumulation (a perf change once TAA is the samples' default).
- Shadows of a second directional light (the moon).

## 11. Deliver in this order

1. S1 motion vectors (commits: per-renderer previous data and the copy pass, timed in `GPU.Post`; the velocity
   variants, the target and the forward pass on the same module; the `motion` debug view and the `debugView` enum;
   tests and DG).
2. S2 TAA (commits: `GPU.TAA` stage and the three lists, the copy moved into it; jitter; resolve and history;
   the editor integration: selection mask, overlay depth bias, capture and project-thumbnail settling, the thumbnails'
   flag, noise rotation; `AntiAliasingMethod` registered, with its component tests and the feature test; the Basic 3D
   template and the three samples on TAA with their command scripts and perf-log checks; DG).
3. S3 cascade blending, with the A/B of the measured shadowed scenes.
4. S4 height fog, the Open World's fog.
5. S5 light units (DG section, manual page, tooltips and descriptions).
6. S6 point and spot shadows (commits: atlas and allocator; the CPU path's views and cache; the GPU path's views and
   counters; shading; `SpreadLightingUpdates` and the capture case; components and inactive reasons; the perf-log
   columns, the Open World's lantern and campfires and the Third-Person lanterns). Push `ci/render-quality`; **the core
   is complete here.**
7. *Cut line.* S7 pre-exposure and atmosphere (commits: the exposure readback, P and its writers, the captures' wait;
   `AutoExposureMaxEV` 16; the atmosphere; the Open World's sky).
8. S8 reflection probes (commits: the required `imageCubeArray` and the cube array; runtime probes and their cache;
   `lighting.bakeProbes`; Inspector; the Third-Person's probes).
9. T1 TAA quality presets and tuning (D17), commits:
   1. D17 and 6.14 added to this plan (with sections 4, 7, 7.1, 8, 9 and 11, D4, D5 and D14).
   2. **Settings and plumbing.**
      - The enums and fields (C++ only, not registered; default Original).
      - The settings struct, the tables and the three functions.
      - `ResolveTemporal(settings)`, the 64 B push constants and the distance in the history's alpha.
      - `TonemapParameters` at 48 B.
      - The test helpers moved to shared headers; `AddTemporalPostProcess(scene, preset)`.
      - The Original/Custom cases.
   3. Variance clipping (with the CPU computation of the trail's luminances), with its cases.
   4. The resampling term, with its cases.
   5. The reactive mask (the transparent alpha blend and the resolve term), with its cases.
   6. Disocclusion rejection (the previous camera's forward, offset and depth scale), with its cases.
   7. Sharpening: the CPU prototype first, then the tone mapping, with its cases.
   8. **Registration.**
      - The properties, tooltips, ranges and inactive reasons; `UI::PropertyDrawers` and the `TAAPreset` drawer.
      - The `Reflection`, `Scene.Serialization`, `Editor.Commands` and Inspector cases.
      - `ComponentSchema.json` and the feature test's values.
      - DG for what T1 adds.
      - The FXAA-scene perf comparison.
   If time runs short, stop after T1: every technique is usable through Custom, and the default is S2's look.
10. T2 Sharp tuned and made the default (D17), commits:
    1. `play.start {paused}` and `viewport.capture {live, atFixedStep, frames}`, with their cases, descriptions and skill
       lines.
    2. **Evidence and tuning.**
       - The captures. Sharp's final values go in `c_TAASharpSettings`, and the defaults follow.
       - Sharp becomes the default, and files that meant the default move to Sharp.
       - The template's and samples' command-script checks.
       - The Sharp cases.
    3. The perf entries and comparisons, with the numbers in the message.
    4. The Manual page, the rest of DG, the skills, and the plan's "As built in T1/T2" lines.
11. S9 SSR, the Third-Person's SSR.
12. S10 sun extraction, the Third-Person's sun.
13. S11 clearcoat and transmission (commits: material model; shading, glass's attachments and scene-color copy; glTF;
    the Playground).
14. S12 finish: A/Bs, docs and skills, Roadmap Status, full Debug and Release suites, lint, CI green on
    `ci/render-quality`, report.

If time runs short, stop after a complete step; every step from S1 on leaves the branch mergeable.

## 12. Merge notes

- `project-settings` (Project Settings and Preferences panels, input actions with rebinding, project-wide collision
  layers, the exported executable's name, version and icon) touches the same shared files:
  - `StrataEditor/src/Panels/InspectorPanel.cpp`: it moves `DrawCollisionLayers` (1339) out of the scene sections; this
    stream edits only `DrawSky` (1497) and adds the probe section. Function-level conflicts only.
  - The feature test: both add entities or properties to `Feature.stscene` and maybe `c_ExpectedLogMessages`; the
    second to land merges both sets and reruns `StrataTests.FeatureTest`.
  - The script ABI: `project-settings` may append host functions for input actions (`StrataScriptHostAPI`,
    `ST_SCRIPT_HOST_FUNCTIONS`, the API test module, the feature scripts); this stream appends none, so the tail is
    theirs.
  - `StrataTests/src/Editor/CommandJournalTests.cpp` (both add commands to its kind and deferral lists) and
    `StrataTests/src/Editor/EditorMcpToolsetsTests.cpp` with `StrataCLI/src/CLI/McpToolsets.cpp` (both may add
    prefixes); `ProjectTemplates.cpp` (the template's `Post Process` here; project files there), the samples' scenes and
    `StrataTests/Editor/<Sample>Commands.json.in` (input maps and collision layers there, rendering here),
    `StrataTests/Perf/Budgets.json` (appended entries), `StrataTests/CMakeLists.txt`, `EditorStateCommands.cpp`,
    `AGENTS.md` (shared headroom: 378 code points at 6425b57b), `Docs/DeveloperGuide.md` (different sections),
    `Docs/Roadmap.md` (Status).
- Renderer defaults: `project-settings` registers its sections in a `SettingsRegistry` and expects this stream either
  to register a `Renderer.Defaults` section or to record a follow-up. This stream adds no project-wide renderer
  setting: its budgets are per program (section 4), so nothing is registered, and the Roadmap's "Later" records the
  section as a follow-up with `game-shell`'s quality presets (section 10).
- The second stream to land merges `main` into its branch, reruns the feature test, the sample command scripts,
  `Docs.AgentGuide`, `Architecture.Layering`, `Editor.Journal` and the MCP toolset tests, and pushes `ci/<stream>`
  before `main` moves. `render-quality` is the larger stream and expects to land second; it then captures the samples'
  icons again (`project-settings`' 256-pixel `viewport.capture` of each sample's scene, set through
  `project.setGameInfo`), since TAA, fog and the atmosphere change their look. Should it land first, `project-settings`
  captures them with the new look.
- No new layer, include exception or `MayInclude` entry: the renderer's features stay in the Renderer layer, the
  commands in `StrataEditorCore`, `HDRSun` in Asset.
