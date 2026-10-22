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
  `python3 Tools/PerfLock.py --label render-quality -- <command>`, judged by the ratio of the medians. A criterion in
  the reference machine's absolute numbers is reported with the Mac value and deferred to the reference machine.
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
  the editor's selection outline, captures and thumbnails stay steady. GTAO and PCSS noise are accumulated over frames.
- **Cascade blending**: no visible seam between directional shadow cascades, and shadows fade out before
  `ShadowDistance` instead of stopping.
- **Documented light units**: lux, candela and nits stated on every light, the sky and emission, with a pre-exposed HDR
  target so real values (a 100,000 lux sun) render without overflowing half floats.
- **Shadows of point and spot lights**: an atlas, a per-frame redraw budget and a cache like the cascades', decided on
  both draw paths.
- **Height fog**.
- Below the cut line (sections 3 and 11), in this order: a **physical atmosphere** for the procedural sky, **reflection
  probes** baked in the editor, **screen-space reflections**, **extracting the sun from HDR skies**, and **clearcoat and
  transmission** in the material model and the glTF importer.
- The samples use them (section 6.13): the Open World gets TAA, fog, campfire shadows and the atmosphere; the
  Third-Person Adventure TAA, shadowed lanterns, reflection probes, SSR and its sun extracted from its sky; the Physics
  Playground TAA and clearcoat and glass materials.
- Every feature costs nothing when it is off (no target, no pass, no shader variant bound), and existing scenes render
  as in 0.5.0 unless they opt in. The one exception is cascade blending (D6), on by default because the seam it removes
  is a defect; the pre-exposure of D7 is a power of two and changes no stored value.
- The script ABI is unchanged (`ST_SCRIPT_ABI_VERSION` stays 1, no host function is added): scripts reach every new
  setting through `Entity::GetProperty`/`SetProperty` (`StrataScriptCore/Include/StrataScript/Entity.h:70-72`).

Not the goal: render scaling or upscaling, MSAA, volumetric fog, area lights, runtime probe capture, quality presets
(`game-shell` maps them to the renderer later), sheen and the other glTF material extensions (section 10).

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
| `Strata/src/Strata/Asset/TextureImporter.cpp` | 647 | Texture import (`GetVersion()` 5), the HDR path at 517-547 |
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
   `SceneRendererSpecification::EntityIDs`), RGBA16F HDR (cleared every frame: no history), RGBA8 LDR, AO,
   half-resolution bloom. Targets are released through `Renderer::ReleaseDeferred`; the exposure, bloom and tonemap
   binding sets bind `m_HDRTexture` directly.
4. The shadow map is a D32 2D array of `c_MaxCascades` = 4 layers of `ShadowMapSize` (2048), one framebuffer per slice
   (`EnsureShadowMap`, 1002-1036).

Gaps: no velocity target, no previous view-projection, no jitter, no per-renderer previous transforms or palettes, no
HDR history, no TAA. `PostProcessComponent::AntiAliasing` is a `bool` meaning FXAA (`R/RendererComponents.h:100`).

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
  cascade, fades off, shared across scenes and sizes). In Play the Viewport and the GameView render the same
  `RenderScene` at different times, and hidden views skip frames.
- "Instance history" today is the GPU path's visibility bits (alternating parity, `CullConstants::VisibilityParity`)
  and the per-instance `LODFadeState`, which keeps the `EntityID` it was drawn for because "an instance slot may change
  hands" (`R/GPUSceneData.h:71-77`).
- The only previous-frame camera state is `m_PreviousCameraPosition`/`Forward` (floats, `R/SceneRenderer.h:954-956`),
  for camera-cut detection and texture streaming; `SceneCamera::Position` is a float `vec3`.
- The shadow cache key holds the whole `SceneCamera`, `Projection` included (`ShadowCacheKey`,
  `R/SceneRenderer.h:1287-1297`): a jitter put into `SceneCamera::Projection` would redraw every cascade every frame.

### 2.4 Lights, shadows and units

- `DirectionalLightComponent` (color, `Intensity` 3, `CastShadows`, `ShadowDistance` 80, `ShadowSoftness`,
  `ShadowBias`, `ShadowNormalBias`); `PointLightComponent` (intensity 10, `Range` 10) and `SpotLightComponent`
  (intensity 20, range 15, half angles 20/30) have no shadow fields: "Point and spot lights cast no shadows"
  (`R/RendererComponents.h:28`, and their registered descriptions). Only the directional light's `Shadow*` properties
  lack an inactive reason today (`RendererComponents.cpp:121-123, 150-152` set them for the sky and post-processing).
- Units: DG "Scene renderer and lighting" calls them "relative but physically consistent". The shaders compute a
  directional light as `color * intensity * NoL * BRDF` (illuminance), a punctual one as `color * intensity` times
  windowed `1/d^2` (`SH/Include/PBR.glsl`: luminous intensity), spot lights without cone normalization, sky radiance
  times `SkyLightComponent::Intensity`; the glTF importer reads `KHR_lights_punctual` 1:1. So one scale links nits, lux
  and candela, but nothing states it. Manual exposure is a multiplier of `2^Exposure`; automatic exposure meters a
  log2-luminance histogram over [-12, 16] (`SH/Include/Exposure.glsl`); `SanitizeHDR` clamps at 65,000. A 100,000 lux
  sun on a white surface gives about 32,000 nits: one step from overflow in RGBA16F, and a 0.53 degree sun disk of real
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
  (`R/GPUSceneData.h:119-122`); `Culling.glsl` declares 22 storage buffers (MoltenVK allows 31 per stage).

### 2.5 Sky, environment and materials

- `SkyLightComponent::Source` is `EnvironmentMap` or `Procedural` (a `uint8_t` enum registered as names). The
  procedural cube is regenerated when `ProceduralSkyParameters` (96 bytes of push constants; NVRHI's
  `c_MaxPushConstantSize` is 128) differ exactly, and then the whole filter chain runs (512 cube, irradiance, 6
  prefiltered mips at 512 samples). The Open World's `DayNight` script turns the sun in steps
  (`Samples/OpenWorld/Scripts/DayNight.h:1-4`) and drives the sky's colors and the campfires' `PointLight` intensity
  through `SetProperty`.
- One environment is bound (`SH/Include/Scene.glsl:220-223`, SRV 3, 4, 6). `SceneRenderer::Render` into a target needs
  a non-sRGB UNORM format and returns the tone-mapped image; the HDR target is reachable as `GetHDRTexture()` only.
- `.hdr` files import as `TextureUsage::HDR` (RGBA16F, never compressed); `stbi_loadf_from_memory` decodes full floats,
  then `TextureCooker::CookHDRLevels` clamps to [0, 65504]. Cooked textures have no metadata field (`R/CookedTexture.h`,
  `c_CookedTextureVersion` 2); `ImportResult` has none either (`Asset/AssetImporter.h:47-56`). Textures are 2D only (no
  cube or array assets).
- `MaterialProperties`: metallic-roughness factors and five maps; `.stmat` JSON at format version 1, read and written
  through `GetPropertyInfos` (an unknown key only warns). `RenderData::MaterialData` is 96 bytes with one spare `uint`;
  the five-map count is hard-coded in `MaterialGPU::c_TextureMapCount`, `RenderSceneGPU.h:217-218`,
  `RenderSceneGPU.cpp:35, 255, 342, 391`, `StrataEditor/src/UI/MaterialEditor.h:50` / `.cpp:42` and
  `StrataEditor/src/Editor/AssetEdit.cpp:285-287` (`AssetEdit.h:31`). "Transparent" means `AlphaMode::Blend`, routed to
  the CPU-sorted pass by `c_CullMaterialTransparent`.
- `GltfImporter`'s `c_SupportedExtensions` (`GltfImporter.cpp:48-51`) has no material extension beyond emissive
  strength and unlit; the vendored cgltf parses `has_clearcoat`, `has_transmission`, `has_volume`, `has_ior`
  (`cgltf.h:544-547`).

### 2.6 Device, bindings and stages

- Required and optional features: `VulkanGraphicsDevice.cpp` enables `imageCubeArray` when present (782) without a
  `GraphicsDeviceInfo` flag (`R/GraphicsDevice.h:29-62`); `shaderOutputLayer` and multiview are never enabled.
  `vulkaninfo` on this Mac: `imageCubeArray` and `shaderOutputLayer` true, no geometry shaders, no `drawIndirectCount`,
  no `samplerFilterMinmax`; `maxPerStageDescriptorSamplers` 16, `maxPerStageDescriptorStorageBuffers` 31,
  `maxComputeSharedMemorySize` 32,768, `maxColorAttachments` 8; RG16F, RGBA16F, B10G11R11 and R32F are filterable color
  attachments and storage images.
- The scene binding set (`SceneRenderer.cpp:851-869`) uses SRV 0-7, 11, 12, 14-17 and samplers 0-5 (material samplers),
  8, 9, 10: nine of MoltenVK's 16 samplers per stage, nine storage buffers. Free scene SRV slots: 8, 9, 10, 13 and 18 up
  (18 is the light records' slot only in the light-culling layout, `SH/Include/Lights.glsl:32`).
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
  skinned, caster and shadow-draw counts. Its scenes have no `PostProcessComponent`, so they run the defaults (ACES,
  auto exposure, bloom, GTAO, FXAA). `GPU.SceneRenderer.Feedback` checks that GPU feedback requests what the CPU walk
  requests. `AddNeutralPostProcess` (`SceneRendererTestUtils.h:126-137`) turns off every effect for exact-pixel tests.
- Other suites this stream extends: `GPU.SceneRenderer.Shadows` (cascade caching at 385), `.Lighting`, `.PostProcess`,
  `.CameraRelative`, `GPU.ProceduralSky`, `GPU.FeatureTest` (4 frames, `GPUFeatureTests.cpp:35`), `Renderer.Material`,
  `Asset.Gltf`, `Asset.TextureImport`. Perf: `PerfGPU.Renderer` (`RendererPerfTests.cpp`, CPU medians and `...GPU` busy
  medians; `MeasuredRendererSpecification` turns `CacheShadowCascades` off), the measured `EditFrame*`,
  `Render100kCubes`, `Render10kMaterials`, `Render300kCubes`, `Skinned1000` entries, and the provisional shadowed and
  Open World tour entries.
- The feature test requires every registered component in `Feature.stscene` (5,384 lines) with every property at a
  non-default value on some owner (`FeatureTests.cpp`); `c_ExpectedLogMessages` is at `FeatureTestUtils.cpp:38`.
  Material properties are not enforced; golden rule 6 still covers them (`Feature.stmat`). `Feature.stscene` holds a
  `PostProcess` with `AntiAliasing` true and one with false (257, 314).
- Samples: every 3D sample has one shadowed sun, one sky and one `Post Process` with the defaults (FXAA). Only the Open
  World has point lights (five campfires in `Island/Landmarks` cells). Pixel checks are
  `StrataRuntime.<Sample>WindowedImage` (`check-image`: the Open World requires `--dominant green 5`,
  `StrataTests/CMakeLists.txt:792-794`) and `StrataRuntime.ThirdPersonWindowedPerf`
  (`check-perf-log ... --nonzero GPUBusyMs`, 848-850). The regressions are
  `StrataTests/Editor/<Sample>Commands.json.in`; the Basic 3D template makes its `Post Process` in
  `StrataEditor/src/Editor/ProjectTemplates.cpp:69`.
- Editor: `ViewportDebugView` {None, Streaming, LOD, Mip} are entity tints (`StreamingView.h:26-36`); the Inspector's
  scene sections are `DrawSceneSettings` (1237), `DrawCollisionLayers` (1339) and `DrawSky` (1497,
  `StrataEditor/src/Panels/InspectorPanel.cpp`); `ThumbnailCache::c_Version` is 3. Commands that exist:
  `asset.setImportSettings`, `asset.reimport`, `material.*`, `viewport.capture`/`getSettings`/`setSettings`; MCP
  toolsets map prefixes in `StrataCLI/src/CLI/McpToolsets.cpp` (no `lighting.` prefix yet; `core` is full).
- `AGENTS.md` is 39,638 characters of the 40,000 `Docs.AgentGuide` allows.

## 3. Order and the cut line

Features are ordered by dependency first, then by what the owner sees:

1. Motion vectors, then TAA and its editor integration: SSR, GTAO and PCSS accumulation reuse the jitter and the
   history; TAA is the most visible change in every sample.
2. Cascade blending: small, independent, visible in every sample.
3. Light units and pre-exposure: the atmosphere and the sun extraction produce real values and need them; TAA's history
   scaling follows the pre-exposure.
4. Point and spot shadows: the largest piece of the core, visible at night in the Open World.
5. Height fog: small, visible in the Open World.
6. **Cut line.** Below it, each step may move to wave 7 if the usage limit runs out, in this order: atmosphere (Open
   World), reflection probes (Third-Person), SSR (needs TAA and benefits from probes), sun extraction (needs the units),
   clearcoat and transmission.

## 4. Where each setting lives

| Feature | Kind | Settings | Inspector | Script reach |
| --- | --- | --- | --- | --- |
| TAA | post-process | `PostProcessComponent::AntiAliasingMethod` {FXAA, TAA} | generic; inactive while `AntiAliasing` is off | `SetProperty("PostProcess", ...)` |
| Motion vectors | renderer | none (made when a consumer needs them); viewport debug view `motion` | Viewport debug chip | none |
| Cascade blending | directional light | `DirectionalLightComponent::CascadeBlend` | generic; inactive without shadows | generic |
| Units | documentation, tooltips | none new; `PostProcessComponent::AutoExposureMaxEV` default 16 | tooltips name the unit | none |
| Local shadows | light | `CastShadows`, `ShadowBias`, `ShadowNormalBias`, `ShadowSoftness` on point and spot lights | generic; inactive reasons | generic |
| Height fog | new component `HeightFog` (first active one applies) | 8 properties (D9) | generic; "Add Height Fog" in the Sky section | generic |
| Atmosphere | sky source | `SkyLightSource::Atmosphere` + 3 properties | generic; inactive reasons | generic |
| Reflection probes | new component `ReflectionProbe` | 6 properties (D11); `lighting.bakeProbes` | generic + a Bake button | generic (not the bake) |
| SSR | post-process | `ScreenSpaceReflections`, `ScreenSpaceReflectionsIntensity`, `...MaxRoughness` | generic; inactive without TAA | generic |
| Sun extraction | command + import setting | `lighting.extractSun`; `TextureImportSettings::RemoveSun` | Sky section button | none |
| Clearcoat, transmission | material | 11 properties (D14) | material inspector | none (materials are assets) |
| Budgets per program | renderer specification | atlas size, shadowed lights, probe slots, `TemporalAntiAliasing` | none | none |

Scene settings (`SceneSettings`) get nothing: rendering settings stay on entities, as the sky and post-processing are,
so world cells, prefabs and the generic commands carry them.

## 5. Design decisions

**D1 One stream-wide rule: off costs nothing.** A feature whose setting is off allocates no target, runs no pass, binds
no new shader variant and adds no per-frame CPU work beyond reading its setting. Each step proves it with an exact
comparison (`AddNeutralPostProcess` scenes render bit-identical to 6425b57b's output) and with the A/Bs of 7.1.

**D2 Motion vectors from per-renderer copies of the previous frame.**
- Each `SceneRenderer` that needs motion keeps `m_PreviousInstances` (the `InstanceData` layout, indexed by instance
  slot) and `m_PreviousPalette` (the palette buffer's layout): the shared data as this renderer last drew it. At the end
  of each frame a compute pass copies, from the shared buffers, the slots and palette ranges the change logs report
  since the renderer's previous frame; when a log overflowed (`GetInstanceChanges` returns false) or the buffers grew,
  it copies everything. Steady state costs nothing.
- `Mesh.vert` gains a velocity variant (`ST_VELOCITY`, with and without `ST_SKINNED`): it reads the previous record,
  and when its `EntityID` differs from the current one (a reused slot, a new entity) or its `SkinPalette` differs (a
  moved palette range) it uses the current transform or palette, so new and re-skinned instances move only with the
  camera.
- The prepass gets a velocity variant writing an RG16F target (unjittered current clip position minus previous clip
  position, in UV units), as one more color attachment; the prepass framebuffer and `PipelineKey` gain a `Velocity` bit.
  Both draw paths and both occlusion phases use the same shaders and framebuffer, so they write the same motion.
- The copy also takes the slots `RenderSceneGPU` uploaded in the frame without a logged change (new proxies whose
  upload `NewInstanceBytesPerFrame` deferred), so a deferred instance's previous record is its own from its first drawn
  frame.
- `FrameConstants` gains `UnjitteredViewProjection`, `PreviousViewProjection` (previous projection times previous
  rotation-only view times the translation from the previous to the current camera position, built in double: it maps a
  position relative to the current camera to the previous clip space), `JitterParams` (current and previous jitter in
  clip units) and `TemporalParams` (the frame index for noise, the pre-exposure of D7, flags). Both declarations and the
  `static_assert` change together.
- The sky has no geometry: the resolve derives its motion from the camera alone (direction at depth 0).
- Rejected: a previous transform in `InstanceData` (the 64-byte rule); previous data in `RenderSceneGPU` (shared by
  renderers that render at different times, so "previous" differs per renderer); velocity written always (costs every
  scene); a separate velocity pass (a second geometry pass).

**D3 Jitter only in the frame's copy of the camera.** The jitter is an 8-sample Halton (2, 3) sequence indexed by
`m_RenderCount` (scene time stops while paused; rendering does not), added to `frame.Projection`, `ViewProjection` and
their inverses after 1924. Culling, LOD selection, the shadow cache key, the cascades, the overlays'
`SceneOverlayFrame::ViewProjection` and the previous-frame matrices stay unjittered. A frame without TAA has no jitter.
Rejected: jittering `SceneCamera::Projection` (redraws every cascade every frame: 2.3).

**D4 The TAA resolve.**
- A compute pass in a new stage `GPU.TAA`, after `Transparent` and before `UpdateExposure`, reads the jittered HDR, the
  depth, the velocity and the history, and writes the other texture of a ping-pong pair of RGBA16F history textures
  (made only with TAA, released through `ReleaseRenderTargets`). The exposure, bloom and tonemap binding sets exist per
  history index, so nothing is copied.
- The resolve: the velocity of the nearest depth in the 3x3 neighborhood, a filtered history fetch, the history clamped
  to the neighborhood's color range in a luma-chroma space, luminance weighting so highlights do not flicker, a history
  weight lowered with the motion's length, `SanitizeHDR` on the result.
- History resets (the output is the current frame): the first frame, camera cuts (the existing `IsCameraCut`), a
  different render scene that is not a clone of the previous one (the test at 1894: entering Play keeps the history), a
  resize, a renderer whose previous frame is more than 0.25 s old, and `SceneRenderer::ResetHistory()`, which
  `GameRenderer` calls where it calls `ResetExposureAdaptation` (level loads). `SceneRendererStats` gains
  `TemporalFrames` (frames since the last reset) and `HistoryResets`.
- Transparent surfaces are resolved with the rest: they gain anti-aliasing and may ghost where they move over a still
  background; the clamp bounds it. Rejected: resolving before transparents (they would alias and shimmer with the
  jitter).
- `PostProcessComponent` keeps `AntiAliasing` (bool) and gains `AntiAliasingMethod` {`FXAA` = 0, `TAA` = 1}, default
  FXAA, inactive while `AntiAliasing` is off. Scenes of 0.5.0 load unchanged and keep FXAA; the Basic 3D template and
  the samples set TAA. Rejected: turning `AntiAliasing` into an enum (enum properties read names or integers, so every
  saved `"AntiAliasing": true` would fail: `Reflection/PropertyJson.cpp:464-476`); TAA as the component's default (it
  would change every scene without a `PostProcess`, every `GPU.SceneRenderer.GPUDriven` comparison and the scenes of the
  measured budgets). `AddNeutralPostProcess` already turns `AntiAliasing` off, so exact-pixel tests never jitter.

**D5 Renderers of single images and the editor's overlays.**
- `SceneRendererSpecification::TemporalAntiAliasing` (default true): `ThumbnailCache` turns it off, and a TAA scene
  then renders with FXAA there. `viewport.capture` (the capture renderer) keeps TAA and waits, as it waits for textures,
  until `TemporalFrames >= c_TemporalSettleFrames` (8) with a still camera, so a capture shows what the Viewport shows.
- Picking and entity tints read the jittered entity IDs: a sub-pixel difference at silhouettes, accepted (tints are
  debug views). The selection outline does not: it would crawl by a pixel with the jitter. With TAA and a selection, a
  selection-mask pass in `GPU.Overlays` draws the selected instances again with the unjittered matrices into an R8
  target (CPU-built draws on both paths, as transparent surfaces are; skinned and mirrored pipelines; depth-tested
  against the frame's depth with one jitter step of bias), and `Overlay/Outline.frag` reads the mask instead of the IDs
  in that case. Rejected: no jitter in editor views (the editor would show another image than the game); resolving the
  outline in the history (it lags behind a dragged object).
- With TAA, PCSS and GTAO rotate their per-pixel noise by the frame index, so the history accumulates their samples;
  their sample counts do not change in this stream.

**D6 Cascade blending.** `DirectionalLightComponent::CascadeBlend` (fraction of a cascade's depth range, 0 to 0.5,
default 0.1). `FindShadowCasters` widens each slice but the last by the band before fitting its sphere, so the next
cascade's square covers the band (the CPU caster test, `GetCascadeMask`, and the GPU's `CullConstants::CascadeBoxes`
read the same `m_Cascades`, so both paths follow). `ShadowParams.z` carries the fraction; `SampleDirectionalShadow`
evaluates both cascades in the band and blends them by depth, and the last cascade fades to unshadowed over its band
before `ShadowDistance`. The cascade cache's keys change once (the boxes are wider). Default on, because the seam is a
visible defect and the cost is limited to the band's pixels (two PCSS evaluations there); 7.1's A/B checks it. Rejected:
a dithered choice of cascade (correct only with TAA).

**D7 Light units and pre-exposure.**
- Units, stated in the inspector's tooltips, the component descriptions and a new DG section "Light units and
  exposure": directional `Intensity` is illuminance in lux; point and spot `Intensity` is luminous intensity in candela
  (a point light of P lumens has P / 4pi cd; a spot's candela is along its axis, not spread over the cone); sky radiance
  and emission are luminance in nits (cd/m^2): an environment map's values times `SkyLightComponent::Intensity`, the
  procedural colors, `EmissiveColor * EmissiveIntensity`. The shading already computes this (2.4); nothing is rescaled,
  so 0.5.0 scenes render as before. With manual exposure EV, the tonemapper sees `L * 2^EV`; DG states the relation to a
  camera's EV100 and a table of reference values (sun, overcast sky, moon, lamp, candle).
- Pre-exposure: shading writes `L * P` into the HDR target, where P is a power of two chosen on the CPU from the latest
  read-back exposure (2-3 frames late, changed only when it is off by more than one stop), or the power of two nearest
  the manual multiplier (1 for manual exposure 0). Every writer of scene HDR (forward, sky, transparent, fog) multiplies
  by `TemporalParams.y`; the histogram and bloom's threshold divide it out, the tonemap applies `M / P`, the TAA history
  is rescaled by `P / P_previous`. A power of two changes no half-float mantissa, so two renderers that pick different P
  produce the same image unless a value overflows, which keeps the CPU/GPU comparisons exact. `AutoExposureMaxEV`'s
  default becomes 16 (sunlit scenes in real units meter around 15); stored values are unchanged.
- Rejected: absolute values in the HDR target (a physical sun overflows); a pre-exposure read on the GPU from the
  exposure buffer (one more storage buffer in every scene shader, and two renderers' values could differ in the last
  bit).

**D8 Point and spot shadows in an atlas.**
- Storage: one D32 2D atlas (`SceneRendererSpecification::LocalShadowAtlasSize`, 4096; made when the first shadowed
  local light is drawn, released with the targets) with tiles of 128 to 1024 texels from a quadtree allocator. A spot
  light takes one tile (perspective, the outer cone plus a one-texel guard band); a point light six tiles of one size
  (90-degree faces widened by the guard band so filtering never leaves a face). The atlas is one framebuffer; each view
  sets its viewport and scissor, and a redrawn tile is cleared by a quad at depth 0 (a texture clear would also clear
  the cached tiles). `ShadowMesh.vert` gains a local-view variant (a perspective view from the local view buffer,
  without the cascades' pancaking).
- Which lights: at most `MaxShadowedLocalLights` (16) per view, the lights with `CastShadows` ranked by the clusters'
  relevance (projected size of their range); a tile's size follows that projected size (power of two, at most 1024),
  with hysteresis so sizes do not toggle. Lights that drop out fade their shadows over the last rank's margin instead of
  popping.
- Cache on both paths: a view is redrawn only when its key changes (light position, direction, range and angles, tile
  and size, the near plane) or something that reaches it changed (a proxy whose old or new bounds reach the light's
  range, the scene's mesh and material tables, the geometry pool's content, the texture table while its casters are
  alpha-masked), as `PrepareShadowCache` does for cascades; at most `LocalShadowViewsPerFrame` (12) views are redrawn
  per frame, nearest lights first; a light whose views are not drawn yet is unshadowed and fades in. The camera is not
  in the key: the matrices that sample a tile are rebuilt each frame relative to the camera in double.
  `SceneRendererSpecification::CacheLocalShadows` (default true) is turned off by the perf tests, as
  `CacheShadowCascades` is.
- Deciding what each redrawn view draws, twice: the CPU path queries the render scene's `SpatialIndex` with the view's
  planes (`QueryPlanes`, as the cascades do at `SceneRendererExtraction.cpp:545-560`) and tests the candidates in
  parallel chunks merged in chunk order (as `FindShadowCasters` does); the GPU path's shadow culling (`Cull.comp`'s
  `ST_CULL_SHADOW` variant) tests the frame's redrawn local views after the cascades, read from one new storage buffer
  (`u_LocalShadowViews`: 23 of 31 buffers), into `c_CullMaxLocalShadowViews` (12) times 32 new buckets (mirrored,
  double-sided, geometry, alpha-masked; no occlusion phases). Casters below a texel are left out on both paths; LOD is
  chosen with the tile's size as the viewport height on both. Casters mark `c_FeedbackShadow` (feedback) and the CPU's
  marks alike. `SceneRendererStats`: `LocalShadowLights`, `LocalShadowViewsDrawn`, `LocalShadowCasters`,
  `LocalShadowDraws` (compared by the GPU-driven tests).
- Shading: a per-renderer table maps light records to shadow slots; `LightCompact.comp` writes the slot into
  `LightData::SpotAngles.z` (and the CPU path in `CollectLights`); a `LocalShadowData` buffer per slot holds up to six
  camera-relative matrices, the atlas rectangles and the biases. `Forward.frag` picks a point light's face by the major
  axis, filters with 16 rotated taps through the existing comparison sampler (no new sampler: 9 of 16 stay), and applies
  `ShadowBias` and `ShadowNormalBias` in texels of the tile. Reversed-Z with the cascades' depth bias signs.
- Properties: `CastShadows` (default false: existing scenes do not change), `ShadowBias` (1), `ShadowNormalBias` (1.5),
  `ShadowSoftness` (the filter radius in texels, 1.5), inactive while `CastShadows` is off. The same inactive reasons
  are added to the directional light's `Shadow*` properties (the gap of 2.4).
- Rejected: depth cube arrays (a face per framebuffer anyway without geometry shaders, fixed sizes, cube comparison
  filtering differs from the cascades', and one more sampler state); per-light textures (a binding each); layered
  rendering through `shaderOutputLayer` (not enabled by the engine; deferred); CPU-only culling of local views on the
  GPU path (would break the rule that both paths decide, and costs the CPU when many casters move).

**D9 Height fog per fragment.** A `HeightFogComponent` (first active one applies, like `PostProcess`): `Density`
(extinction at `BaseHeight`, 1/m, 0.02), `HeightFalloff` (1/m, 0.2), `BaseHeight` (0), `StartDistance` (0), `MaxOpacity`
(1), `Color` (linear albedo of the scattering, 0.7 0.8 0.9), `ColorFromSky` (true: the sky's irradiance lights the fog,
from the environment's irradiance map, or the atmosphere's when it is on) and `SunScattering` (forward-scattering
strength toward the directional light, 0.5). `Forward.frag` (opaque and transparent) applies the analytic integral of
exponential height fog along the camera ray before `SanitizeHDR`, from camera-relative positions and the camera's world
height; `Skybox.frag` applies it at infinite distance. Parameters go into `FrameConstants`; no fog component means a
uniform branch that skips it. Rejected: a fullscreen pass after the sky (transparent surfaces would still need the
per-fragment path, and it adds a full-screen read and write); volumetric froxels (deferred).

**D10 The atmosphere as a sky source.**
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

**D11 Reflection probes baked into texture assets.**
- `ReflectionProbeComponent`: `Capture` (texture asset), `Extents` (half extents of the influence box, 5 5 5),
  `BlendDistance` (1), `BoxProjection` (true), `Intensity` (1), `Priority` (0). Influence boxes follow the entity's
  rotation. Faces are baked at 256 (twice the run-time face size of 128, so the equirectangular image, 1024x512, keeps
  the detail the cube needs); the size is a renderer constant, not a property, because the cube array has one size.
- Bake: `lighting.bakeProbes` (deferred command, `MayDefer`; one undo step for all handles) renders the six faces of
  each probe with a capture renderer (entity IDs, fades and TAA off, `TextureStreaming` off so it waits for full
  resolution, neutral post-processing and pre-exposure 1) through a new `SceneRenderOptions::HDROutput` that skips
  post-processing and leaves the HDR target to read back (`TextureReadback`, polled), assembles an equirectangular image
  on a job, writes it as a Radiance `.hdr` through `ImageWriter` (stb's `stbi_write_hdr_to_func`; a write on an I/O job)
  into `Assets/Lighting/<scene>/<probe>.hdr`, or over the file its `Capture` already names, and sets `Capture`. The file
  is an ordinary texture asset: `.meta`, import, references through the component's asset property (pruned exports and
  delete checks follow it), packs. A probe bakes what is drawn at its position, other probes' current captures included
  (baking twice gives a second bounce). Refused while playing.
- Runtime: the renderer gathers probes each frame (a registry view; worlds' cells are ordinary entities), keeps up to
  `MaxReflectionProbes` (16) visible ones by priority and size, requests their textures at High priority through the
  scene's asset manager (`Scene::GetAssetManager()`, never `AssetManager::GetActive()`), holds each whole until it is
  converted (`EquirectToCube` at 128, `FilterEnvironment`) into a slot of a `TextureCubeArray` of 16 slots (about 1 MiB
  each), computes its diffuse irradiance as 9 RGB spherical-harmonic coefficients, then releases the hold. At most two
  probes are converted per frame, in `GPU.Reflections`. `Forward.frag` loops over the frame's probe list (at most 16 box
  tests per pixel), blends the two strongest by box distance and priority with box-projected lookups, and falls back to
  the sky. Transparent surfaces use them too.
- Without `imageCubeArray` (`GraphicsDeviceInfo::SupportsImageCubeArray`, new) probes are off and
  `SceneRendererStats::ReflectionProbesUnsupported` says so; no log per frame.
- Rejected: run-time capture (six scene renders per probe with their own culling and shadows, and the Open World's
  moving sun would recapture constantly); cube texture assets (an asset pipeline change: cooked format, streaming,
  residency; deferred); clustered probe lists (16 box tests are cheap; deferred until scenes need more).

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
- `TextureImportSettings::RemoveSun` (HDR only, default false): the importer runs the function on stb's floats before
  the resize and the clamp, and fills the region from its border before cooking. Outputs for existing settings are
  byte-identical (tested), so `TextureImporter::GetVersion()` stays 5; the textures whose setting changes re-import.
- `lighting.extractSun` (deferred: it reads the source on a job): writes the scene's directional light (or creates
  "Sun") with the direction rotated by the sky's `Rotation`, the color, and `Intensity` = illuminance times
  `SkyLightComponent::Intensity`, in one undo step; the Sky section's "Extract Sun" button runs it and then
  `asset.setImportSettings` with `RemoveSun`, two steps with their own meanings.
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
  them to the CPU-sorted pass, which draws them first, without blending, reading a scene-color copy: when the frame
  draws a transmissive surface, the HDR image after the sky is downsampled into a half-resolution B10G11R11 texture with
  mips (roughness picks the level), in `GPU.Transparent`. Refraction offsets the lookup by the view-space normal, `IOR`
  and `Thickness`; attenuation follows Beer's law.
- glTF: `KHR_materials_clearcoat`, `KHR_materials_transmission`, `KHR_materials_volume`, `KHR_materials_ior` join
  `c_SupportedExtensions`; their textures import with `TextureUsage::Data` or `NormalMap`; `GltfImporter::GetVersion()`
  becomes 10 (every glTF re-imports once).
- Rejected: a separate refraction pass after transparents (sorted transparency would be wrong behind glass); a
  full-resolution copy (bandwidth; rough glass blurs anyway).

**D15 GPU stages.** Two new stages, registered in pass order: `GPU.Reflections` between `GPU.AO` and `GPU.Opaque` (probe
conversion, SSR trace) and `GPU.TAA` between `GPU.Transparent` and `GPU.Post` (motion copy, resolve). Local shadows time
in `GPU.Shadows`, the atmosphere in `GPU.Setup`, the scene-color copy in `GPU.Transparent`, the selection mask in
`GPU.Overlays`; fog adds no pass. Each stage boundary is outside a render pass (MoltenVK may write timestamps of a
render pass at its end, DG "GPU timing"). The three lists of 2.6 change with each new stage.

**D16 Bindings.** New scene-set bindings, all within the slots free today: SRV 8 local shadow atlas, 13 local shadow
data (storage), 9 probe cube array, 10 probe records (storage), 18 SSR result, 19 scene-color copy. Samplers stay at 9
of 16 (the probes use the linear clamp sampler, the atlas the comparison sampler); storage buffers go from 9 to 11 of
31.

## 6. Per feature: tests, coverage and the editor

### 6.1 Motion vectors (S1)

- `SceneRenderer::GetVelocityTexture()`; `SceneRenderOptions::MotionVectors` asks for them in a frame (the Viewport's
  debug view and tests), TAA and SSR ask implicitly. The Viewport debug view `motion` (`ViewportDebugView::Motion`, name
  `"motion"` in `viewport.setSettings`) draws the velocity as colors over the image in `GPU.Overlays`.
- `GPU.SceneRenderer.MotionVectors`: a still camera over a still scene has zero motion everywhere (exact); a moving
  camera gives every pixel the reprojected motion computed on the CPU (within 0.01 px); a moved entity's pixels carry
  its motion and the others only the camera's; a skinned pose change, a mirrored instance and a LOD-fading instance
  carry theirs; a reused instance slot starts without motion; two renderers of one scene rendering at different times
  each see their own previous frame; a change log overflow (more than its cap between two frames) still gives exact
  motion. `GPU.SceneRenderer.CameraRelative`: motion 20 km from the origin is within 0.05 px.
  `GPU.SceneRenderer.GPUDriven`: `Comparison` asks for motion vectors and requires bit-identical velocity textures on
  both paths.
- Must-fail: drop the `EntityID` check (the reused-slot case fails); copy the previous data before the frame's draws
  instead of after (the moved-entity case fails); skip the full copy on overflow (the overflow case fails).

### 6.2 TAA (S2) and its editor integration (S3)

- `GPU.SceneRenderer.TAA`: with a still camera the jitter changes the frame constants only (`CachedCascadeMask` stays
  full, LOD choices and culled counts equal a renderer without TAA); a still edge converges within 1% (HDR) of the
  average of its eight jittered frames rendered without history; a moving object leaves no trail beyond 2 px after it
  stops; cuts, scene changes, resizes, stale renderers and `ResetHistory` reset (`HistoryResets`, output equals the
  current frame); FXAA scenes allocate no history; thumbnails' renderer (`TemporalAntiAliasing` off) renders a TAA scene
  with FXAA; both paths resolve the same image over 16 frames of a moving scene (within 1/255, identical IDs).
  `Scene.Serialization`: a 0.5.0 `PostProcess` without `AntiAliasingMethod` loads as FXAA with no warning.
- S3: `GPU.Editor.Viewport`: the selection outline covers the same pixels in all eight jitter phases;
  `viewport.capture` of a TAA scene waits for `TemporalFrames >= 8` and equals the Viewport's image within 1/255; with
  TAA, PCSS and GTAO noise changes per frame and the converged shadow edge is smoother than one frame's (lower gradient
  energy). `Editor.Templates`: the Basic 3D template uses TAA.
- Must-fail: jitter `SceneCamera::Projection` (the cascade cache case fails); no reset on a cut (the reset case fails);
  no clamp (the trail case fails); the outline from jittered IDs (the outline case fails).
- Feature test: the `Environment` entity's `PostProcess` uses TAA (the other keeps FXAA, `AntiAliasing` false);
  `GPU.FeatureTest` checks `TemporalFrames` grows.

### 6.3 Cascade blending (S4)

- `GPU.SceneRenderer.Shadows`: a plane receiving a shadow across a cascade split has no step larger than the in-cascade
  filter noise (the same scene with `CascadeBlend` 0 shows the seam: the test requires both); every band pixel is inside
  the next cascade's square; shadows fade to zero at `ShadowDistance`; the cascade caching case keeps passing.
  `GPU.SceneRenderer.GPUDriven`'s shadowed scene passes with blending on (the widened boxes reach both paths).
- Must-fail: do not widen the slices (the band case fails where the next square ends).
- Feature test: `Sun.CascadeBlend` 0.2. Thumbnails render one cascade: no thumbnail change (checked).

### 6.4 Light units and pre-exposure (S5)

- `GPU.SceneRenderer.PostProcess`: a scene lit by a 100,000 lux sun with automatic exposure renders like the same scene
  at 1 lux (within 1/255 after adaptation), and a 100,000 lux sun with manual exposure -16.6 like 1 lux with exposure 0;
  `AddNeutralPostProcess` scenes are bit-identical to 6425b57b's; automatic exposure adapts to the same value with
  pre-exposure; TAA history survives a pre-exposure step without a flash (no pixel changes by more than the scene's own
  change). The CPU/GPU comparisons keep passing (D7's power of two). Two renderers forced to pre-exposures 2^3 and 2^4
  render bit-identical images of one scene.
- Must-fail: write HDR without pre-exposure (the 100,000 lux case saturates); round the pre-exposure to a non-power of
  two (the bit-identical case fails).
- Editor: tooltips name the unit; a new `Renderer.LightUnits` suite (the `Renderer` shard) checks that every light's
  and the sky's intensity description names its unit.

### 6.5 Point and spot shadows (S6)

- `GPU.SceneRenderer.LocalShadows`: a point light shadows casters in all six directions with no seam at face edges; a
  spot light's shadow lies where the CPU's projection predicts; no acne at grazing angles; with more shadowed lights
  than `MaxShadowedLocalLights` the most relevant keep shadows; a still scene redraws no view after it settles
  (`LocalShadowViewsDrawn` 0), a moving caster redraws only the views it reaches, a moved light only its own; the redraw
  budget holds (at most 12 views per frame); skinned, mirrored and alpha-masked casters shadow; lights without
  `CastShadows` allocate no atlas and draw no view. `GPU.SceneRenderer.GPUDriven`: a `BuildLocalShadowed` scene (spots
  and points over the other builders' meshes) compares pixels and the local shadow counts. `GPU.SceneRenderer.Feedback`:
  local casters outside the view are requested like cascade casters.
- Must-fail: leave caster changes out of the key (the moving-caster case fails); skip the local views in `Cull.comp`
  (the comparison fails); swap two faces (the six-directions case fails).
- Feature test: `Lamp` and `Spot` cast shadows with non-default bias and softness. Open World: the five campfires cast
  shadows; `StrataTests/Editor/OpenWorldCommands.json.in` checks the property. Third-Person: two shadowed lanterns.

### 6.6 Height fog (S7)

- `GPU.SceneRenderer.Fog`: pixels at known distances and heights match the CPU's analytic integral within 1/255
  (neutral post); transparent surfaces are fogged like opaque ones at the same depth; the sky gets `MaxOpacity`; a scene
  without the component renders bit-identical to 6425b57b's. Must-fail: the transparent variant without fog.
- Feature test: a `Fog` entity with every property non-default. Open World: light fog that keeps the windowed image's
  `--dominant green 5` passing (the fog is tuned, the check is not touched).

### 6.7 Atmosphere (S8)

- `GPU.Atmosphere`: the zenith is bluer than the horizon at noon and the horizon redder than the zenith at sunset; the
  GPU transmittance LUT matches the CPU evaluation within 1%; the light's color at sunset is the CPU transmittance times
  its color; the environment refreshes only after the sun moved 0.5 degrees, never more than one face per frame, and
  switches only when complete (`EnvironmentUpdates`); procedural and map skies are unchanged (existing
  `GPU.ProceduralSky` passes). Must-fail: skip the CPU transmittance (the sunset light case fails).
- Feature test: an "Atmosphere Sky" `SkyLight` (not the first active one) with `Source` Atmosphere and non-default
  atmosphere properties. Open World: the sky uses the atmosphere; `DayNight` drives the sun only (its sky-color fields
  go); the command script and the image check pass.

### 6.8 Reflection probes (S9)

- `GPU.SceneRenderer.ReflectionProbes`: a mirror sphere inside a probe's box reflects the probe's capture, not the sky;
  two overlapping boxes blend; box projection places a reflected wall correctly; a probe's diffuse coefficients light a
  matte sphere like its capture's irradiance (within 2%); more visible probes than slots keep the strongest; probe
  textures are requested, held until converted, then released; a device reported without cube arrays draws no probe.
  `GPU.Editor.Lighting`: `lighting.bakeProbes` writes an `.hdr` asset per probe, sets `Capture` in one undo step,
  rebakes over the same file, refuses while playing and rejects entities without the component without side effects.
  `Asset.Dependencies`: a scene with a baked probe references its texture.
- Must-fail: box projection off (the wall case fails); release the hold before conversion (the conversion never
  completes and the hold case fails).
- Feature test: a `Probe` entity with every property non-default and a committed `Assets/Lighting/Feature/Probe.hdr`
  (generated by `StrataTests/FeatureTest/Tools/GenerateAssets.py`); `StrataTests/Editor/FeatureCommands.json.in` runs
  `lighting.bakeProbes` on a copy. Third-Person: probes in the course, baked through the editor.

### 6.9 SSR (S10)

- `GPU.SceneRenderer.SSR`: a mirror floor reflects a red cube at the CPU-predicted pixels; rough surfaces fade to the
  probe or sky; off-screen hits fall back; without TAA nothing runs (no surface target); both paths give the same image.
  Must-fail: reproject without the hit's velocity (a moving cube's reflection lags).
- Feature test: the TAA `PostProcess` turns SSR on with non-default intensity and roughness. Third-Person: SSR on, its
  perf log check requires `--nonzero GPU.Reflections`.

### 6.10 Sun extraction (S11)

- `Asset.HDRSun`: a synthetic equirectangular sky with a disk of known radiance and direction gives the direction
  within 0.1 degree and the illuminance within 1%, also across the seam; a flat peak is reported as clipped; `RemoveSun`
  leaves no texel above the threshold; without the setting the cooked bytes equal 6425b57b's. `GPU.Editor.Lighting`:
  `lighting.extractSun` writes a light whose direction matches where the sky shows the sun (render the background, find
  its brightest pixel), with the sky's `Rotation` applied, in one undo step.
- Must-fail: ignore `Rotation` (the direction case fails).
- Feature test: `Sky.hdr` gains a sun (the generator) and `RemoveSun` in its `.meta`; `FeatureCommands.json.in` runs
  `lighting.extractSun`. Third-Person: its hand-placed sun is replaced by the extracted one.

### 6.11 Clearcoat and transmission (S12)

- `Renderer.Material`: round trip of the new properties; 0.5.0 materials load with defaults. `Asset.Gltf`: the four
  extensions' factors and textures import (a test model per extension), unknown ones still warn. New suite
  `GPU.SceneRenderer.Materials`: clearcoat adds a second highlight and conserves energy (no pixel brighter than the
  coat's and base's sum); transmission shows the background offset by `IOR`; no scene-color copy without a transmissive
  draw (`SceneColorCopies` 0); the transparent comparison of `GPU.SceneRenderer.GPUDriven` includes glass.
- Must-fail: forget the coat's Fresnel on the base (the energy case fails).
- Feature test: `Feature.stmat` gets clearcoat, a new `FeatureGlass.stmat` transmission, `Platform.gltf` the four
  extensions. Thumbnails: `ThumbnailCache::c_Version` 4 if material previews change (checked by rendering one). Physics
  Playground: clearcoat balls and glass blocks.

### 6.12 Inspector, commands and MCP

- New components and properties get the generic Inspector; inactive reasons as in section 4. Component-specific UI: a
  Bake button on `ReflectionProbe` (runs `lighting.bakeProbes` for the selection); in `DrawSky`, "Extract Sun" (map
  skies), "Add Height Fog" (no fog yet) and "Bake Reflection Probes". Nothing is added near `DrawCollisionLayers`
  (section 12).
- Commands (`StrataEditor/src/Editor/EditorLightingCommands.cpp`): `lighting.bakeProbes` {entities?} and
  `lighting.extractSun` {sky?, light?}, both mutations with complete schemas, descriptions for agents, `MayDefer`,
  `CheckSceneEditable`, one undo step. The `lighting.` prefix maps to the `scene` toolset; `core` is unchanged.
- `editor.perf`'s description and the profiler tests list the new stages; the strata-editor-automation skill names the
  commands and the `motion` debug view.

### 6.13 Defaults and the samples

| Sample | Adopts | Proof |
| --- | --- | --- |
| Open World | TAA, cascade blending, campfire shadows, height fog, atmosphere | command script; windowed image (`--dominant green 5` kept); a new `StrataRuntime.OpenWorldWindowedPerf` reading the windowed run's existing perf log, with `--nonzero GPU.TAA` |
| Third-Person Adventure | TAA, two shadowed lanterns, reflection probes, SSR, extracted sun | command script; windowed image; perf log `--nonzero GPU.TAA --nonzero GPU.Reflections` |
| Physics Playground | TAA (many moving bodies: motion vectors at scale), clearcoat and glass | command script; windowed image |
| Tetris | unchanged (FXAA) | as today |

Sample scenes are changed through the editor (the strata-editor-automation skill), opened in place only to change them,
and each `StrataTests/Editor/<Sample>Commands.json.in` keeps passing (extended where a feature has a property to check).
A sample adopts a feature in the step that lands it.

## 7. Deliverables and acceptance

Each step is one or more commits that build and pass every suite with the lint clean; `ctest` runs use the `macos-debug`
and `macos-release` presets of the worktree's tree; each step runs `StrataTests.GPU` under validation (every case ends
with `CHECK(gpu.GetNewErrorCount() == 0)`) and `StrataTests.FeatureTest`.

- **S1 Motion vectors**: D2, 6.1; the `motion` debug view. *Accept:* 6.1's suites; no-motion scenes bit-identical.
- **S2 TAA**: D3, D4, D15 (`GPU.TAA`), 6.2 without the editor part. *Accept:* `GPU.SceneRenderer.TAA`,
  `GPU.SceneRenderer.GPUDriven`, `.Feedback`, `.Shadows`, `.PostProcess` pass; the A/Bs of 7.1.
- **S3 TAA in the editor and the samples**: D5, the noise rotation, the template, the samples' TAA. *Accept:* 6.2's
  editor cases; the three samples' command scripts, windowed images and perf logs.
- **S4 Cascade blending**: D6, 6.3. *Accept:* `GPU.SceneRenderer.Shadows`, `.GPUDriven`; A/B of the shadowed perf
  scenes.
- **S5 Light units and pre-exposure**: D7, 6.4, the DG section, `Docs/Manual/Lighting.md`. *Accept:* 6.4.
- **S6 Point and spot shadows**: D8, 6.5, `GraphicsDeviceInfo` untouched (core features only). *Accept:* 6.5's suites;
  the Open World's campfires and the Third-Person lanterns; provisional metrics of 7.1.
- **S7 Height fog**: D9, 6.6. *Accept:* 6.6; the Open World's image check.
- *Cut line.*
- **S8 Atmosphere**: D10, 6.7. **S9 Reflection probes**: D11, D16, `SupportsImageCubeArray`, 6.8, `GPU.Reflections`.
  **S10 SSR**: D12, 6.9. **S11 Sun extraction**: D13, 6.10. **S12 Clearcoat and transmission**: D14, 6.11.
- **S13 Finish**: A/Bs of 7.1 on the final branch, docs and skills (section 8), the Roadmap's Status, CI green on
  `ci/render-quality`.

### 7.1 Budgets and measurements

- Untouched: every existing entry's `Measured` and `Budget`. Every feature defaults off for existing scenes (cascade
  blending excepted), so the measured scenes run the same passes; each step A/Bs the measured `PerfGPU.Renderer.*`,
  `PerfGPU.Renderer.EditFrame*` and `PerfGPU.Editor.EditFrame*`/`PlayFrame*` metrics against 6425b57b's Release build
  (ratio of medians at most 1.02; above it the cause is found and fixed before the step lands). S4 A/Bs
  `Render100kCubesShadowed` and `Render300kCubesShadowed` with and without blending and reports the ratio.
- New provisional metrics (`ms`, no targets: the reference machine sets them), each with a CPU median and a GPU busy
  median (`...GPU`), at 1920x1080 in `RendererPerfTests.cpp`: `Render100kCubesTAA`, `LocalShadows16Lights` (cache off)
  and `LocalShadows16LightsCached`, `HeightFog`, `Atmosphere` and `AtmosphereSunMovingMaxFrame`, `ReflectionProbes16`,
  `SSR`, `Transmission`. The GPU stage times of each feature's pass on the Mac are reported in the step's commit message
  and the final report.
- `PerfGPU.Samples.OpenWorldTour.*` (provisional) runs before and after the Open World adopts its features; the report
  gives both.

## 8. Docs and skills

- `AGENTS.md` (362 characters below the limit): the Rendering digest gains at most one rule, in place of words, not
  beside them: "Temporal effects jitter only the frame's copy of the camera and keep their history per renderer;
  renderers of single images settle or turn TAA off." Local shadows fall under the existing "decided twice" and cache
  rules, whose wording names "shadow views" instead of "cascades" where it does. The file stays under 40,000 characters.
- `Docs/DeveloperGuide.md`, Rendering: new subsections "Motion vectors and TAA", "Light units and exposure", "Local
  light shadows", "Reflections" (probes, bake, SSR), "Fog and atmosphere"; "Scene renderer and lighting" (the pass list,
  the units paragraph replaced), "Lights, shadows and HDR" (cascade blending, the local shadows sentence), "GPU-driven
  path" (local shadow views and buckets), "GPU timing" (the stages), "Materials" (the new properties and maps),
  "Textures" (`RemoveSun`), "Feature test" (the new entities and assets), "macOS and MoltenVK" (cube arrays, binding
  counts).
- `Docs/Architecture.md`: the frame's pass order, the per-renderer history, the probe bake's data flow.
- `Docs/Manual/Lighting.md` (new): units with reference values, exposure, the sky sources, fog, probes and how to bake
  them, local shadows and their cost. Linked from `Docs/README.md`.
- `Docs/Roadmap.md`: Status when the stream lands; "Later": what section 10 defers.
- Skills: `strata-make-a-game` (lighting advice: units, TAA, shadowed lights' cost, baking probes, fog),
  `strata-editor-automation` (`lighting.*`, the `motion` debug view, captures waiting for TAA), `strata-scripting` (the
  new properties a script may drive, e.g. fog density or the sun for time of day).

## 9. Risks

| Risk | Mitigation |
| --- | --- |
| TAA blurs or ghosts, and the owner judges by eye | trail and convergence tests; the `motion` debug view; samples checked in the editor at each step; sharpening deferred but cheap to add |
| Temporal state breaks the CPU/GPU comparisons (draw order, occlusion phases) | jitter and history are per renderer and order-independent; the comparisons run with TAA in S2; pre-exposure in powers of two |
| The selection outline under TAA | D5's mask pass, with its own stability test |
| Shader variants multiply (prepass: entity IDs x velocity x surface; mesh: skinned x velocity) | variants only where a feature needs one; pipelines keyed by the framebuffer variant; build time watched |
| MoltenVK limits (16 samplers, 31 storage buffers) | D16's counts; no new sampler; cube arrays gated by a device flag |
| No GPU tests on CI; Windows and Linux GPUs unseen | only core Vulkan features and gated ones; validation clean on the Mac; the reference machine runs `StrataTests.GPU` when it returns (Roadmap) |
| Half-float overflow with real units | D7 pre-exposure; `SanitizeHDR` on every new writer |
| Local shadow redraws spike when many casters move | per-frame view budget, nearest first; `LocalShadows16Lights` metrics; GPU-side culling |
| Probe bake reads back HDR on MoltenVK | `TextureReadback` (polled) as captures do; a GPU editor test |
| The atmosphere's environment refresh hitches | double-buffered, one face per frame; `AtmosphereSunMovingMaxFrame` |
| `GltfImporter` version 10 re-imports every model once | stated in the release notes; the samples' first open re-imports in the background |
| Fog or the atmosphere fails the Open World's green check | tune the scene, never the check |
| `AGENTS.md` headroom shared with `project-settings` | one rule at most, replacing words |
| The usage limit | the cut line; each step lands alone |

## 10. Deferred

- Render scaling, dynamic resolution and TAA as an upscaler (with `game-shell`'s quality presets); MSAA.
- Layered shadow rendering through `shaderOutputLayer`, occlusion culling of local shadow views, more than 16 shadowed
  lights per view.
- Run-time probe capture and relighting for moving suns; cube texture assets; clustered probe lists.
- SSR on a nearest-depth pyramid; SSR for transparent surfaces.
- Volumetric fog, light shafts, aerial perspective in froxels; the atmosphere seen from altitude or space.
- Thickness maps, sheen, specular, iridescence and anisotropy extensions; EXR import; area lights; contact shadows.
- Fewer GTAO samples with temporal accumulation (a perf change once TAA is the samples' default).
- Shadows of a second directional light (the moon).

## 11. Deliver in this order

1. S1 motion vectors (commits: per-renderer previous data and the copy pass; the velocity variants and target; the
   `motion` debug view; tests and DG).
2. S2 TAA (commits: `GPU.TAA` stage and the three lists; jitter; resolve and history; `AntiAliasingMethod`; tests, DG).
3. S3 TAA in the editor (selection mask, capture settling, thumbnails flag, noise rotation), the Basic 3D template, the
   three samples on TAA with their command scripts and perf-log checks.
4. S4 cascade blending.
5. S5 light units and pre-exposure (DG section, manual page, tooltips).
6. S6 point and spot shadows (commits: atlas and allocator; the CPU path's views and cache; the GPU path's views;
   shading; components and inactive reasons; the Open World's campfires and the Third-Person lanterns).
7. S7 height fog, the Open World's fog. Push `ci/render-quality`; **the core is complete here.**
8. *Cut line.* S8 atmosphere, the Open World's sky.
9. S9 reflection probes (commits: device flag and cube array; runtime probes; `lighting.bakeProbes`; Inspector; the
   Third-Person's probes).
10. S10 SSR, the Third-Person's SSR.
11. S11 sun extraction, the Third-Person's sun.
12. S12 clearcoat and transmission (commits: material model; shading and scene-color copy; glTF; the Playground).
13. S13 finish: A/Bs, docs and skills, Roadmap Status, full Debug and Release suites, lint, CI green on
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
  - `StrataCLI/src/CLI/McpToolsets.cpp` (both may add prefixes), `ProjectTemplates.cpp` (the template's `Post Process`
    here; project files there), the samples' scenes and `StrataTests/Editor/<Sample>Commands.json.in` (input maps and
    collision layers there, rendering here), `StrataTests/Perf/Budgets.json` (appended entries),
    `StrataTests/CMakeLists.txt`, `EditorStateCommands.cpp`, `AGENTS.md` (shared headroom), `Docs/DeveloperGuide.md`
    (different sections), `Docs/Roadmap.md` (Status).
- The second stream to land merges `main` into its branch, reruns the feature test, the sample command scripts,
  `Docs.AgentGuide`, `Architecture.Layering` and the MCP toolset tests, and pushes `ci/<stream>` before `main` moves.
  `render-quality` is the larger stream and expects to land second.
- No new layer, include exception or `MayInclude` entry: the renderer's features stay in the Renderer layer, the
  commands in `StrataEditorCore`, `HDRSun` in Asset.
