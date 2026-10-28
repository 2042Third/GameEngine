# Stream `sun-shadows`: shadows of a real sun (wave 7)

Owner's request (2026-10-26, verbatim in `audit_brief.md`): "the current image quality is simply not great. The shadows are
very pixelated, maybe add more settings to improve that", never temporal upscaling ("The blurry mess that current gen engine
has (like unreal) is completely the opposite of what this engine is planning to be"), "great realistic graphics, which
doesn't mean sharp or clinical", with the Fox Engine (Metal Gear Solid V) as the baseline look. The plan (`plan.md` 4.3)
makes this stream the owner's first priority and the second to merge, after `graphics-options`.

This spec is written against main at bc6ee123 (release 0.6.0). Every file:line below is as of bc6ee123 unless it names step
0. Step 0 (`plan.md` section 3) lands on main before this stream branches; section 2.3 lists what this spec assumes it
provides, by the names the plan gives them (step 0's worktree had not reached S0.4 when this was written, so those names
are unverified: where step 0 chose other names, use step 0's and keep this design).

Rules of the machine (AGENTS.md and `plan.md` 4.1, restated where they bind this stream):

- Worktree `.worktrees/sun-shadows` from step 0's tip, build directory `build/agent-sun-shadows` (never `build/windows`,
  which the owner runs).
- Every commit: Debug build, Debug ctest, every `GPU*` suite under the validation layer on the NVIDIA reference machine,
  `Tools/CompatLint.py` on the changed files. New GPU suites use `Tests::GPUContext` and end with
  `CHECK(gpu.GetNewErrorCount() == 0)`.
- Perf: no campaign, no `PerfStability.py`. The stream A/Bs its own stages under `Tools/PerfLock.py` (section 7). New
  metrics enter `Budgets.json` as provisional entries with a target.
- Samples change only through editor commands; tests never open samples in place.

## 1. Goal, in priority order

At 3840x2160 on High, in every 3D sample:

1. **No pixelated shadows where receivers are.** About one shadow texel per screen pixel: the median of screen pixels per
   shadow texel over visible receivers at most 1.5 in each sample's scene camera and editor opening view, and no more
   than 1% of receiver pixels above 4 (0.6.0: 2.2 at splits, 5.5-8.7 at near ends; editor views all in the last cascade).
   Step 0's 4096-texel cascades halve the texels; this stream's fit (step 6) puts them where the receivers are.
2. **A sun's penumbra.** Sharp at contact, softening with distance by the sun's real angular size (0.53 degrees), within
   20% of the analytic width (0.6.0: about 3.8 times too wide at the default 1 degree and the full-width PCSS radius).
3. **Identical frame to frame without TAA, and no grain.** No per-frame noise in the shadow code; single-frame RMS noise
   across a penumbra at most 1% of the edge's contrast with no anti-aliasing (0.6.0: 4.6%).
4. **Grounded below the shadow map's resolution.** Contact shadows: a 5 cm plate on the ground at 2.6 m casts a solid
   contact shadow (0.6.0: a thin rim). Thin casters (wires, grass, poles) never lose their shadows to a sparse search.
5. **Visible as data.** A `shadows` debug view in the viewport and in `viewport.capture`, with per-cascade statistics for
   agents, used to tune and verify every other step.

Within the wave's rules: native resolution only; nothing needs TAA (judged with TAA, FXAA and no AA); off costs nothing;
the CPU and GPU-driven paths stay pixel-equal and cached and uncached cascades stay exact; Medium keeps 0.6.0's sample
counts and resolutions; `SceneRendererSpecification{}` stays Medium.

Not the goal (below the cut, section 4.7, or other streams): local lights' sized PCSS, more than 4 cascades, distant
shadows beyond `ShadowDistance`, 16-bit depth, per-cascade cache keys; GTAO's own noise (the hatching inside the robot's
shadow is GTAO's, `shadows/audit.md` F9: `light-and-materials`); tone, exposure and the sun-to-sky ratio (pale shadows in
two samples are a lighting question, F9).

## 2. Where we start

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `DG` is `Docs/DeveloperGuide.md`,
`ED/` is `StrataEditor/src/`, `T/` is `StrataTests/src/`.

### 2.1 Files

| File | Lines | Role for this stream |
| --- | --- | --- |
| `SH/Include/Shadows.glsl` | 154 | The sun's PCSS: bindings (SRV 7, samplers 9 and 10), Poisson disk, rotation, cascade selection, blend, receiver plane bias, `SampleCascadeShadow`, `SampleDirectionalShadow` (owned) |
| `SH/Include/LocalShadows.glsl` | 90 | Point and spot shadows; reuses `c_PoissonDisk` and `ShadowSampleRotation` from Shadows.glsl (owned) |
| `SH/Scene/CascadeFit.comp` | 73 | Fits each cascade's depth range to the casters on the GPU, writes `u_Cascades` (owned) |
| `SH/Scene/Forward.frag` | 374 | The sun term at 129-140 (shared: the sun term only) |
| `SH/Scene/DepthPrepass.frag` | 92 | Writes the geometric view-space normal (octahedral RG16F) the mask reads (unchanged) |
| `SH/Include/Scene.glsl` | 371 | Scene layout, `FrameConstants` mirror, `CascadeData` (shared: slot comment) |
| `SH/Scene/HZBBuild.comp` | 67 | Farthest-depth pyramids for occlusion culling, incl. the cascades' (unchanged; model for the range build) |
| `SH/Overlay/MotionView.frag` | 36 | The model for the debug view's overlay |
| `R/SceneRenderer.h` / `.cpp` | 2,035 / 3,853 | Specification, options, stats; `RenderFrame` (2676-3339), `EnsureShadowMap` (1681-1715), `DrawShadows` (1835-1856), `CreateSceneBindingSets` (957-1042), overlays (3341-3700) (shared) |
| `R/SceneRendererExtraction.cpp` | 1,354 | `FindShadowCasters` (415-633: the cascade layout at 448-509), `GetCascadeMask` (635-655), `GatherShadowDraws` (1147-1275: `ShadowParams` at 1175-1177), `FitCascades` (1277-1285) (owned: the cascade layout) |
| `R/SceneRendererGPUDriven.cpp` | 1,601 | Shadow pyramid (1036-1123), cache (`ShadowCacheKey` equality ~1230-1244, `GetCascadesReached` 1246, `PrepareShadowCache` 1266-1340, `CommitShadowCache` 1342-1353), `DrawGPUShadows` (1355-1383), `UsesFeedback` (1590-1599) (owned) |
| `R/SceneRendererLocalShadows.cpp` | 897 | Local shadows (owned; touched only below the cut) |
| `R/SceneRenderData.h` | 508 | `c_MaxCascades` (36), `FrameConstants` (38-87, assert 88), `CascadeData` (91-99), `CascadeFitConstants` (104-116) (shared) |
| `R/GPUSceneData.h` | 258 | `CullConstants::CascadeBoxes` (188) (unchanged above the cut) |
| `R/RendererComponents.h` / `.cpp` | 302 / 554 | `DirectionalLightComponent` (21-35), its registration (223-240) (shared) |
| `R/GPUProfiler.h` / `.cpp` | 195 / 377 | `RendererGPUStages` (20-36), registration (50-61) (shared: stage appended) |
| `R/Renderer.cpp` | 309 | Shared solid textures made in `Renderer::Init` (73-110, textures at 94-98): the blue-noise tile joins them |
| `ED/Editor/StreamingView.h` / `.cpp` | 144 / 317 | `ViewportDebugView` (26-37), names (33-55) (shared: the debug-view enum) |
| `ED/Editor/EditorViewportCommands.cpp` | 1,149 | `DebugViewSchema` (130-134), `DescribeDebugView` (136-147), capture waits (600-660), result (744-770), descriptions (877, 909, 1002-1004, 1026), error (1073) (shared) |
| `ED/Editor/ViewportRenderer.cpp` | 242 | The views' and capture's specifications (79-88), `ShowMotionVectors` (189) |
| `ED/Panels/ViewportPanel.cpp` | 958 | Debug view menu (584-590), legend (600-640) |
| `ED/Editor/EditorStateCommands.cpp` | 1,039 | `DescribeRenderStats` (206-235), the `editor.perf` stage list (882-883) |
| `ED/Editor/ThumbnailCache.cpp` / `.h` | | Thumbnails' renderer (854-858: 1024 x 1), `c_Version` 4 (`.h`:104) |
| `ED/Editor/ProbeBake.cpp` | | The bake's renderer specification (135-147) |
| `Strata/src/Strata/Runtime/GameRenderer.cpp` | | The game's renderer (25-29), resets on level loads (62-63, 74-75) |
| `T/Renderer/GPUSceneShadowTests.cpp` | 752 | `GPU.SceneRenderer.Shadows`: penumbra (226), acne (271), cache (496), blend (571, 652, 688) |
| `T/Renderer/GPUSceneRendererGPUDrivenTests.cpp` | 1,107 | `GPU.SceneRenderer.GPUDriven` (both paths) |
| `T/Perf/RendererPerfTests.cpp` | 716 | Shadowed cubes (602-636), `MedianGPUStageMs` (56-74) |
| `StrataTests/Perf/Budgets.json` | 213 | `Render100kCubesShadowed`/`300k` provisional (167-168) |

### 2.2 The sun's shadows today

- **Frame order** (`RenderFrame`, `R/SceneRenderer.cpp`): `CollectLights` (2859) fills `m_ShadowLight` from the brightest
  active `DirectionalLightComponent` (2350-2362; the preview sun is a default component, 2378); `FindShadowCasters` (2883)
  lays out the cascades and finds casters; `GatherShadowDraws` (2916) builds the fit's inputs and `ShadowParams`;
  `EnsureShadowMap` (2925-2929; failure renders without shadows); GPU stages `Setup`, `Cull` (`PrepareShadowCache` 3016 on
  the GPU path), `Shadows` (3048-3071: `FitCascades`, then `DrawGPUShadows` or, on the CPU path, `m_ShadowCacheValid = 0`
  and `DrawShadows`: the CPU path never caches), `Prepass` (3075-3101; the GPU path's two occlusion phases and counter
  readback 3088-3097), `AO` (3103-3109), `Reflections`, `Opaque` (3119-3125), sky, `Transparent`, `TAA`, `Post`,
  `Overlays`; `CommitShadowCache` after execution (3196, 3329).
- **Layout** (`SceneRendererExtraction.cpp:448-509`): `ShadowCascades` slices from `camera.Near` to
  `min(ShadowDistance, camera.Far)` (427), splits `0.8 log + 0.2 uniform` (456), each slice's corners around the camera,
  their bounding sphere rounded up to 1/16 m (483), the square snapped to whole texels of absolute light space in double
  (485-489), blend bands moving the next slice's near end (506). `frame.CascadeSplits[c]` is the slice's far end (504).
  The boxes `m_Cascades` feed the CPU caster test (`GetCascadeMask` 635-655, which also drops casters below
  `c_MinCasterTexels` = 1, line 72), the GPU's culling (`CullConstants::CascadeBoxes`, `SceneRendererGPUDriven.cpp:715`),
  the fit (`GatherShadowDraws` 1160-1172) and the cache keys (`PrepareShadowCache` 1287).
- **Fit** (`CascadeFit.comp:36-72`): depth from the sphere's far side to the nearest drawn caster toward the light
  (pancaking beyond, `ShadowMesh.vert:48-50`), `Parameters = (world per texel, depth range, search distance
  0.2 radius + 5 m, near z)`.
- **Filter** (`Shadows.glsl`): per pixel a 16-tap Poisson disk rotated by interleaved gradient noise plus
  `TemporalParams.z` (23-31), cascade by depth (33-42), blend band (48-56), receiver plane depth bias (65-75); 16 point taps
  of blocker search over `clamp(lightSizeUV * searchDistance, 1 texel, 32 texels)` (101-117, lit when none hits), then 16
  bilinear comparison taps over `clamp(separation * lightSizeUV, 1 texel, 32 texels)` (121-131). `ShadowParams.x` is
  `2 tan(halfAngle)` (`SceneRendererExtraction.cpp:1176`), so the radius is the full penumbra width: a transition twice
  the physical one. `Forward.frag:131-140` calls `SampleDirectionalShadow` with the interpolated position and the
  geometric normal flipped toward the viewer; the transparent and glass variants call the same.
- **Cache** (GPU path only): a cascade's layer is kept while its `ShadowCacheKey` (the whole `SceneCamera`, viewport, light
  view, box, count, map size, LOD criteria) and `ShadowCacheContent` (scene, tables, pool, texture uploads while alpha
  masks exist, the map's pointer) are equal and no changed proxy's old or new bounds reach it
  (`SceneRendererGPUDriven.cpp:1266-1340`).
- **Resources**: the map is a D32 2D array of `c_MaxCascades` layers of `ShadowMapSize` (`EnsureShadowMap` 1681-1715),
  kept for the renderer's life; the GPU path's occlusion pyramid (`EnsureShadowPyramid` 1036-1080) is farthest-only, a
  quarter of the map, built from the first phase's map only. Scene layout slots used: SRV 0-21 and 27, samplers 0-5, 8, 9,
  10 (`CreateSceneBindingSets` 976-1004); the shadow pass binds dummies at SRV 7 and 8 (1010).
- **Local shadows** reuse `c_PoissonDisk` and `ShadowSampleRotation` (`LocalShadows.glsl:77-84`).

### 2.3 What step 0 provides (assumed)

- `R/RendererQuality.h/.cpp`: `RendererQualityPreset { Low, Medium, High, Ultra, Custom }`, `RendererQualitySettings` with
  `ShadowResolution`, `ShadowCascades`, ..., a preset table laid out one knob per line with its four values on that line.
  `SceneRendererSpecification::Quality` (default Medium), `SceneRenderer::SetQuality` (rebuilds what changed, the cascade
  array with `ShadowCascades` layers, through `Renderer::ReleaseDeferred`), `SceneRendererSpecification::ForThumbnails()`
  and `ForBake()`.
- The `Renderer.Quality` settings section (version 1) and every program following it; `viewport.setSettings {quality}`,
  `viewport.capture {quality}`, `StrataRuntime --quality`.
- `SH/Include/Scene.glsl`'s slot comment assigning SRV 28 to this stream (S0.5).
- Post-processing in `R/SceneRendererPost.cpp` and ambient occlusion in `R/SceneRendererAmbientOcclusion.cpp` (S0.3).
- `Tools/LookDev.py` with `Tools/LookDev/Shots.json` and `LookDevScene.json` (S0.6) and its baselines of 0.6.0 and step 0.

Step 0's spec (`specs/step0.md`, written beside this one) confirms these names and adds what this stream builds on:
`RendererQualitySettings` and its `PresetRow` lines (7.1-7.2: a knob's touch points, which steps 2 and 4 follow),
`ShadowMapResolution`, `RendererQuality::ResolveShadowMapSize`, `SceneRenderer::GetShadowMapSize()` and the private
`m_ShadowMapSize`, `m_ShadowMapLayers`, `ReleaseShadowMap()` and `InvalidateShadowCache()` (7.5: the range texture is made
and released with the map there), `ForThumbnails()`/`ForBake()` (7.4), the slot comment and the GPU stage table (section 8:
`GPU.ScreenShadows` after `GPU.Prepass`), and the look-dev shots by name (9.5: `tp.scene`, `tp.editor`, `tp.feet`,
`pp.scene`, `pp.editor`, `ow.scene`, `ow.editor`, `lookdev.contact`, `lookdev.thin`). The code itself is unverified until
step 0 lands (its five commits on `agent/step0` at 4409b03b touch none of the shadow code); where it lands with other
names, use them and keep this design. `graphics-options` (merging first) adds `RendererQuality::RowTraits` /
`c_RowTraits` (`graphics-options.md` D1), a line of which each new row needs.

### 2.4 Measurements that set the targets

From `shadows/` (the audit's captures, `cascades_2048.txt`, `sweep.txt`, `tools/`) and `architecture/measurements.md`:

| Measured on the reference machine | Value |
| --- | --- |
| Screen pixels per texel at each split, 2048, 2160 rows (`cascades_2048.txt`) | 2.2-2.4 every cascade, every sample |
| At the near ends of cascades 1-3 | 5.5-8.7 |
| Slice footprint in its square / visible ground in its square | 22-28% / 0-15% (PhysicsPlayground C0 and C1: 0%) |
| RMS noise across the robot's penumbra, row 1450, x 1170-1290, ThirdPerson scene camera 3840x2160 (`tools/noise.py`) | 4.6% no AA, 4.3% FXAA, 1.6% after 8 still TAA frames |
| Contact blur from the 1-texel floor (ThirdPerson C0..C3) | 1.7, 3.7, 8.6, 29 cm |
| `GPU.Shadows`, Open World play tour 4K, all cascades redrawn | 0.125-0.184 ms (11,662-15,923 casters) |
| `GPU.Opaque`, PhysicsPlayground 4K FXAA: no shadow / softness 0.5 / softness 3 | 0.216 / 0.275 / 0.338 ms |
| `GPU.Opaque`, ThirdPerson play 4K | 0.320 ms |

The audit's cascade model (`shadows/tools/cascades.py`, a ground-plane model of receivers) was run again for this spec,
weighting every pixel of a 3840x2160 view by its receiver (sky and out-of-range pixels excluded), at 4096 texels and 4
cascades; "fit" lays the slices over the visible depth range (`sweep.txt`'s "depth-fit": ThirdPerson 2.3-80 m,
PhysicsPlayground 20.8-70 m, Open World eye view 3.0-160 m); "receivers" also shrinks each square to its slice's visible
receivers (the audit's "fitted side", quantized up to a quarter stop, the last cascade left on its sphere):

| Median screen pixels per texel (p95) | ThirdPerson | PhysicsPlayground | Open World eye |
| --- | --- | --- | --- |
| 0.6.0 layout (0.8 from 0.1 m), 2048 | 3.58 | 4.70 | 4.20 |
| 0.6.0 layout, 4096 (step 0's High) | 1.79 (2.5) | 2.35 (3.5) | 2.10 (3.2) |
| Depth fit only, 4096, split weight 0.8 | 2.28 (4.1) | 1.28 (1.5) | 2.87 (5.3) |
| Depth fit only, 4096, split weight 0.9 | 2.10 (3.3) | 1.28 (1.5) | 2.38 (4.1) |
| Depth fit and receivers, 4096, split weight 0.9 | 1.29 (2.1) | 0.98 (1.3) | 1.49 (2.5) |
| Depth fit and receivers, 2048 (Medium at 2160 rows), 0.9 | 2.58 | 1.95 | 2.97 |

Three conclusions shape step 6 (section 4.6, D11): the depth range alone does not help eye-level views (it lengthens the
first slice, where 70-75% of their receiver pixels are) and makes them worse at today's split weight; fitting the squares
to the receivers is what reaches 1.5; and the fitted layout wants a more logarithmic split (0.9) than the near-plane
layout (0.8, whose uniform share protects slice 0 from a 0.1 m near plane). A box around each slice's own light-space
footprint, without readback, gains only 0-10% over the sphere (ThirdPerson 1.71 against 1.79): the waste is between the
slice and its receivers, not between the sphere and the slice. These are model numbers; acceptance is measured in the
real scenes (section 6).

## 3. Decisions that hold for every step

**D1 Slots, constants and data.**
- Scene layout: SRV 28 the screen-space shadow mask (step 3; assigned by S0.5), SRV 32 the cascades' depth ranges
  (step 2), SRV 33 the blue-noise tile (step 2). S0.5 assigned this stream only SRV 28 (`light-and-materials` has 29-31);
  step 0's slot comment (`step0.md` section 8) says a new scene resource takes the next free slot (32).
  `light-and-materials` also names SRV 32, for its volumetric fog (its step 8, below its cut, `light-and-materials.md`
  285-287): this stream lands second and uses 32 and 33 above its cut, so this stream's first commit records 32 and 33 in
  `Scene.glsl`'s slot comment, and their step 8 takes the next free slot (34) when it lands (section 9). NVRHI's Vulkan
  mapping gives SRVs bindings 0-127 (`Common.glsl:6-9`), so 32 and 33 are valid. The three items join the scene binding
  *layout* (`m_SceneBindingLayout`, `SceneRenderer.cpp:275-308`, after SRV 27, each with a comment naming its shader) as
  well as the four sets (`CreateSceneBindingSets` 973-1014); the shadow pass's sets bind dummies at 28, 32 and 33 as they
  do at 7 and 8 (1010, 1014).
- The new passes' own layouts use SRV 22-24 for their private inputs (depth, normals, the blur's source), as the SSR
  compute layout does (`SceneRendererScreenReflections.cpp:44-52`): those slots are free in the scene layout, and these
  layouts include `Scene.glsl` only for `u_Frame` and `u_Cascades`.
- **No `FrameConstants` growth.** Reused fields, both declarations (`SceneRenderData.h:63-64`, `Scene.glsl:58-61`) and
  their comments changed together: `ShadowParams.x` becomes `tan(angular radius)` (the penumbra's half-width per unit of
  separation; was `2 tan`), `ShadowBias.z` the wide-penumbra tap count (the `ShadowFilterQuality` row); `ShadowBias.w`
  stays 0 and unused (0.6.0 writes 0 there, `SceneRendererExtraction.cpp:1177`): the last cascade always ends at the
  shadow distance and fades out as in 0.6.0, also when step 6 fits the other cascades (D12 rule 4). `TemporalParams.z`'s
  comment (both declarations, `SceneRenderData.h:67-69`, `Scene.glsl:64-66`) drops "PCSS" for "local shadows' PCF" in
  step 2. Forward.frag reads none of the new values except through `Shadows.glsl` in the transparent variants.
- `CascadeData.Parameters` keeps its meaning (`SceneRenderData.h:95-97`); `.z` (search distance) stays
  `0.2 radius + 5 m` (`CascadeFit.comp:70`; section 10, item 10), where the radius is always the slice's bounding sphere's:
  `CascadeBox::Radius` (`SceneRenderer.h:822`, written at `SceneRendererExtraction.cpp:503`, read only at 1171) keeps the
  sphere's radius after step 6 fits a square, and so do `FarZ` and `SphereNearZ`; a fitted square changes only
  `Min`/`Max`, `RelativeMin`/`RelativeMax` and `WorldPerTexel`.

**D2 Determinism.** Nothing in the sun's shadow reads `TemporalParams.z` or any frame counter. Per-pixel noise comes from
a fixed 64x64 blue-noise tile indexed by the screen pixel (`(pixel + shift) & 63`), so a still view renders the same
image every frame with and without TAA. Local shadows keep 0.6.0's rotation, which still turns with TAA, until step 7
(below the cut): `c_PoissonDisk` and the rotation move from Shadows.glsl into LocalShadows.glsl as
`c_LocalShadowPoissonDisk` and `LocalShadowSampleRotation`, unchanged, so local shadows render bit-identically.

**D3 Both paths and the caches.** Everything a frame decides about the cascades (slices, squares, depth ranges, which
cascade a caster is in) is computed once on the CPU in `FindShadowCasters` from values both paths have, before culling;
the CPU caster test, `CullConstants::CascadeBoxes`, `CascadeFitConstants` and `ShadowCacheKey::Box` keep reading
`m_Cascades`. Every new GPU pass (range build, mask, blur, fit reduction) is the same shader on both paths reading the same
inputs (the complete shadow map, the complete prepass depth), so `GPU.SceneRenderer.GPUDriven` stays pixel-equal. The
cascades' depth ranges are part of the cascade cache: built only for layers the frame drew, kept with kept layers
(D7). New inputs of how casters are drawn: none (the filter changes how shadows are read, not drawn).

**D4 Off costs nothing.** No shadowed sun: no range texture, no mask targets, no fit buffer, no pass, no stage time; the
scene layout binds dummies. The debug view's statistics buffer is made by the first frame that shows the view. Resources
follow the existing lifetimes: the range texture with the shadow map, the mask targets with the render targets
(`ReleaseRenderTargets` 1938), the readback rings with the renderer.

**D5 Where each setting lives.**

| Setting | Kind | Values | Step |
| --- | --- | --- | --- |
| `DirectionalLightComponent::ShadowSoftness` | light (existing) | default 0.53 (was 1.0); range 0-10 degrees kept | 2 |
| `DirectionalLightComponent::ContactShadowLength` | light (new) | meters, 0-2, default 0.3; 0 off for this light | 4 |
| `DirectionalLightComponent::CascadeDistribution` | light (new) | 0-1, default 0.9: the fitted layout's logarithmic share | 6 |
| `ShadowFilterQuality` | quality row | wide-penumbra taps: Low 8, Medium 16, High 24, Ultra 32 (Custom 8, 16, 24 or 32) | 2 (the row and field land with their first use; step 5 adopts) |
| `ContactShadows` | quality row | contact steps: Low 0, Medium 0, High 12, Ultra 16 (0 off; Custom 0 or 4-32) | 4 (likewise) |
| `SceneRendererSpecification::FitShadowsToView` | renderer (new) | bool, default true; off in `ForThumbnails()`, `ForBake()` and comparisons | 6 |
| `debugView: "shadows"` | viewport setting, capture parameter | `viewport.setSettings`, `viewport.capture` | 1 |

Scripts reach the light's properties through `Entity::GetProperty`/`SetProperty` and the rows through
`graphics-options`' named options; the script ABI is unchanged.

## 4. Steps

Each step lands working, tested and documented before the next starts (section 11 lists the commits).

### 4.1 Step 1: the shadows debug view

**What people and agents see.** `debugView: "shadows"` colors every receiver pixel by screen pixels per shadow texel and
outlines the cascades; the legend and the capture's result report per cascade its slice, texel size, share of the image
and the median, p10 and p90 of pixels per texel.

**The per-pixel quantity.** For a pixel with depth (`depth > 0`), its camera-relative position `p` (reconstructed through
`InverseViewProjection`, as `GetCameraMotion` does, `Include/Motion.glsl:10-13`), its view distance
`d = -(View * p).z` (as `Forward.frag:129`), its cascade `c` from the shading's own selection
(`SelectShadowCascade`, D13 of step 6; until then `SelectCascade(d)`), and:

```
pixelWorld = (CameraForward.w > 0.5 ? 2.0 : 2.0 * d) / (Projection[1][1] * ViewportSize.y)   // a pixel's size facing the camera
ratio      = u_Cascades[c].Parameters.x / pixelWorld                                      // screen pixels per shadow texel
```

This is the audit's metric (`cascades.py`: texel size over a screen pixel's size at the receiver's view distance),
independent of the surface's orientation, so the view's numbers compare with `cascades_2048.txt` and section 2.4.

**`SH/Include/ShadowView.glsl`** (new): `bool GetShadowViewSample(ivec2 pixel, float depth, out ShadowViewSample s)` with
`struct ShadowViewSample { int Cascade; float PixelsPerTexel; float BlendWeight; vec2 ShadowUV; }`, shared by the overlay
and the statistics so both see the same pixels.

**`SH/Overlay/ShadowView.frag`** (new, like `MotionView.frag`, drawn first among overlays, `RenderOverlays` 3654-3656):
- Layout: CBV 0 (frame constants), SRV 17 (`u_Cascades`), SRV 22 (depth), SRV 23 (the mask, step 3 on, its red decoded
  as `r * r`; a white dummy before), push constants (b1) `RenderData::ShadowViewParameters { vec4 Colors[3]; uint
  HasMask; uint pad[3]; }` (64 B). The mask is a render target of the frame; its read here comes after the forward pass,
  so NVRHI's barrier is the same as the forward pass's.
- Color: `t = clamp(log2(ratio), 0, 2) / 2`; the ramp is `Colors[0]` (at most 1 pixel per texel) to `Colors[1]` (2) to
  `Colors[2]` (4 and more), linear in `log2(ratio)`. A checker of the shadow map's texels modulates it by `1 +- 0.12 * f`,
  `f = clamp(log2(ratio) - 0.5, 0, 1)` (visible once a texel spans about 1.4 pixels, so the texels themselves show where
  they are coarse; no moire where they are fine). Cascade edges: a pixel whose right or lower neighbor has another cascade
  is white. Blend bands: diagonal hatching (`((x + y) & 7) < 2` darkened by 40%) where `BlendWeight > 0`. With the mask,
  the color is multiplied by `0.4 + 0.6 * visibility`, so the view also shows the shadows. The pass blends over the
  finished image as `MotionView.frag` does (`DrawMotionView`, `SceneRenderer.cpp:3475-3488`: SrcAlpha, InvSrcAlpha) and
  cannot read that image, so: pixels without a cascade (beyond the shadow distance, outside every square) get a neutral
  gray veil `vec4(vec3(0.25), 0.65)` (the image at 35% under gray); sky pixels `vec4(0, 0, 0, 0.65)` (the image at 35%).
  Opaque output (alpha 1) where a receiver is.
- Colors come from the editor's theme: `ViewportDebugColors::ShadowRatio = { Malachite, Sulfur, Cinnabar }` (the values of
  `LOD[0]`, `LOD[2]`, `LOD[3]`, `StreamingView.h:46-50`), passed through `SceneRenderOptions::ShadowViewColors`.

**The statistics reduction: `SH/Debug/ShadowViewStats.comp`** (new), in `GPU.Overlays`, only when the view is shown:
- Bins: per cascade 64 bins of 1/8 stop over [1/16, 16) pixels per texel:
  `bin = clamp(int(floor((log2(ratio) + 4.0) * 8.0)), 0, 63)`; bin b covers `[2^(b/8 - 4), 2^((b+1)/8 - 4))`.
- Buffer `RenderData::ShadowViewCounts { uint32_t Bins[c_MaxCascades][64]; uint32_t SurfacePixels, SkyPixels,
  UncoveredPixels, Reserved; }` (1,040 bytes; `static_assert`), cleared with `clearBufferUInt` each frame it is drawn.
- 16x16 threads per group, one pixel each; shared `uint s_Bins[c_MaxCascades * 64]` (256 entries: thread i clears and
  flushes entry i): clear, `barrier()`, shared atomic adds, `barrier()`, then each thread adds its non-zero entry to the
  buffer with `atomicAdd`; the three totals likewise (shared counters, one global add per group).
- Layout: CBV 0, SRV 17, SRV 22, UAV 0 (the buffer). Dispatch `ceil(W/16) x ceil(H/16)`. Its frame constants are the
  overlays' (in TAA frames the unjittered matrices, `SceneRenderer.cpp:3298-3308`, over the jittered depth: an error of
  under half a pixel in the reconstructed position, immaterial to a ratio binned in eighth stops).
- Readback: a ring of `c_GPURecordCount` (6, `SceneRenderer.h:1457`) `BufferReadback`s of 1,040 bytes
  (`R/BufferReadback.h`), recorded after the dispatch and submitted where `SubmitExposureReadback` is (3262, 3328; the
  `HDROutput` exit at 3182-3204 draws no overlays, so it records none). `SceneRenderer::ResolveShadowViewStats()` polls
  the ring without waiting (`IsReady`, as `ChoosePreExposure` does at 1508-1515) and keeps the newest completed entry; the
  renderer calls it at the start of each frame, and `ContinueCapture` calls it beside `ResolveGPUStats(false)`
  (`EditorViewportCommands.cpp:697`): the capture renderer renders no frame while a capture waits for its readback, so a
  poll only at frame starts would never see the capture's statistics. The ring lives with the renderer, not the targets,
  so a capture's statistics survive `ReleaseTargets` (672).
- CPU (`SceneRenderer::GetShadowViewStats()`, returning `const ShadowViewStats&`): per cascade, the pixels, the median,
  p10 and p90 (the bin where the cumulative count crosses the fraction, reported at its geometric center
  `2^((b + 0.5)/8 - 4)`), and `AboveFour` (bins 48-63); overall the same over every covered pixel; `Render` (the render
  count of the frame it describes) and the frame's cascade layout copied from `SceneRendererStats` (below).

**Renderer API.**
- `SceneRenderOptions::ShowShadowView` (bool) and `ShadowViewColors` (`std::array<glm::vec4, 3>`), documented beside
  `ShowMotionVectors` (`SceneRenderer.h:349-353`); `HasOverlays` (3404-3408) includes it.
- `SceneRendererStats` gains the frame's layout, filled by `FindShadowCasters` whether or not the view is shown:
  `struct ShadowCascadeStats { float Near = 0, Far = 0, WorldPerTexel = 0; bool Fitted = false; };`
  `std::array<ShadowCascadeStats, RenderData::c_MaxCascades> Cascades; uint32_t CascadeCount = 0;` (`Near` is the slice's
  near end including its blend band, `Far` its split; `Fitted` from step 6).

**Editor and commands.**
- `ViewportDebugView::Shadows` appended after `Motion` (`StreamingView.h:26-33`), named `"shadows"`
  (`StreamingView.cpp:33-55`), excluded from `StreamingView::Build` like `Motion` (69); `ViewportRenderer` sets
  `options.ShowShadowView` and the colors (189); `ViewportRenderer::GetShadowViewStats()` passes the renderer's through.
  The saved state's error (`EditorViewport.cpp:101`) and the debug chip's tooltip (`ViewportPanel.cpp:577-578`) name it.
- `DebugViewSchema` (130-134): `"enum": ["none", "streaming", "lod", "mip", "motion", "shadows"]`; the three
  descriptions (877, 909, 1026) and the capture's description (1002-1004) name it: `"shadows" colors receivers by screen
  pixels per shadow texel (green at most 1, yellow 2, red 4 and more), outlines the cascades and hatches their blend bands;
  the result's debugView.shadows reports each cascade's slice, texel size, share of the image and pixels per texel`; the
  error (1073) lists it.
- `DescribeDebugView` (136-147) gains, for `shadows`:

```json
"shadows": {
  "complete": true,
  "shadowMapSize": 4096,
  "surfaceShare": 0.79, "skyShare": 0.21, "uncoveredShare": 0.004,
  "medianPixelsPerTexel": 1.31, "p10PixelsPerTexel": 0.86, "p90PixelsPerTexel": 2.2, "aboveFourShare": 0.0,
  "cascades": [
    { "index": 0, "near": 2.30, "far": 8.80, "texelSize": 0.0028, "fitted": true, "screenShare": 0.42,
      "medianPixelsPerTexel": 1.4, "p10PixelsPerTexel": 0.9, "p90PixelsPerTexel": 2.3, "aboveFourShare": 0.0 }
  ],
  "fit": { "fitted": true, "settled": true, "near": 2.1, "far": 80.0, "squaresFitted": true }
}
```

  Shares are fractions of the image's pixels; the pixels-per-texel figures are over covered receiver pixels; `fit` from
  step 6 (absent before); without a shadowed sun `cascades` is `[]` and `"note": "no shadowed directional light"`.
  `complete` is false only when the statistics of the capture's last frame did not arrive within
  `c_MaxCaptureGPUStatsPolls` (then the figures are the newest ones available).
- The capture waits for its statistics as it waits for the GPU's times (`ContinueCapture`, 685-705): once the image's
  readback is ready, it calls `ResolveShadowViewStats()` and polls `GetShadowViewStats().Render >= progress.RenderCount`
  within the same `c_MaxCaptureGPUStatsPolls` (66) bound; `CaptureProgress` keeps the statistics and `DescribeDebugView`
  takes them as a fourth parameter (`const ShadowViewStats*`, null for the other views).
- Viewport panel: the menu entry `{ ViewportDebugView::Shadows, "Shadows: cascades and pixels per texel" }` (584-590); the
  legend (600-640) shows three ramp lines ("1 px per texel or finer", "2 px", "4 px or coarser", each with its share of
  receivers) and one line per cascade ("C0 2.3-8.8 m", "2.8 mm, median 1.4"); note "White: cascade edges. Hatched: blend
  bands."
- `.claude/skills/strata-editor-automation/SKILL.md` lists the view and its result.

**Tests.**
- `GPU.SceneRenderer.ShadowView` (new, `T/Renderer/GPUSceneShadowViewTests.cpp`):
  - "The shadows view reports each cascade's share of the image and its pixels per texel": a 400 m floor, a camera
    `PitchedCamera(glm::vec3(0, 3, 0), 20, 60)` (helper in `GPUSceneShadowTests.cpp:75-84`, moved to
    `SceneRendererTestUtils.h`) at 256x256, 4 cascades at 2048, `FitShadowsToView` off; the test computes for every row
    the floor's view distance (`FloorRowDepth`, 87-97, moved likewise), its cascade from `GetStats().Cascades` and the
    ratio, builds the same histogram on the CPU, and requires each cascade's share within 0.5% of the image and its
    median, p10 and p90 within one bin. Both paths. Must fail if the ratio used the texel's UV size or the jittered
    projection's other entries.
  - "The view changes nothing when it is off and allocates nothing": without `ShowShadowView` the output equals a renderer
    that never showed it and no statistics buffer exists (`HasShadowViewResources()`); with it, pixels without receivers
    are the image at 35%.
  - "No shadowed sun: the view reports no cascades and shows the image darkened".
- `GPU.Editor.Viewport`: "A capture with the shadows debug view reports its cascades": `viewport.capture {debugView:
  "shadows"}` returns `debugView.shadows` with `complete` true, as many cascades as the scene's, and shares that add up
  with sky and uncovered to 1 within 1e-3; `viewport.setSettings {debugView: "shadows"}` is returned by
  `viewport.getSettings`; `"shadow"` is refused with the message listing the names.
- `Editor.Viewport`: the saved viewport state round-trips `"shadows"` (`EditorViewport.cpp:50, 99`).

**Docs.** DG "View commands" (2756-): the view and its result; DG "Sun shadows" (new, step 2) gets the metric's
definition; the editor-automation skill.

**Acceptance.** The GPU test above. On the reference machine, with the look-dev harness at 3840x2160 Medium and High on
step 1's build (shots `tp.scene`, `tp.editor`, `pp.scene`, `pp.editor`, `ow.scene`, `ow.editor` of `Shots.json`,
`step0.md` 9.5), the ThirdPerson scene camera's per-cascade figures are recorded beside the audit's model (the p10 of
each cascade whose receivers reach its split within 15% of the model's split value, about 4400 / ShadowMapSize at 2160
rows: 2.2 at 2048, 1.1 at 4096; a cascade whose receivers end before its split, such as PhysicsPlayground's first two,
is reported, not checked); these figures are the baseline steps 2-6 compare with. Cost: nothing when off.

### 4.2 Step 2: a physical, deterministic filter

Forward.frag keeps calling `SampleDirectionalShadow` (step 3 moves the opaque call). Every change is in `Shadows.glsl`,
`CascadeFit.comp` (unchanged math), the range build, `SceneRendererExtraction.cpp:1175-1177` and the shared texture.

**D6 PCSS radius at the physical half-width.** A blocker at separation `s` (world units along the light) under a disk of
angular radius `a` (half of `ShadowSoftness`) lets a straight edge's light fall from full to none over a width
`w = 2 s tan(a)` on the plane perpendicular to the light. Percentage-closer filtering of a straight edge with a uniform disk
of radius `r` produces the same profile over `2r`, so `r = s tan(a)` (the shader's
`separation * ShadowParams.x`, with `ShadowParams.x = tan(a)`). Its 10%-90% width is `2 t10 s tan(a)` with
`A(t10) = 0.1`, `A(t) = (acos(t) - t sqrt(1 - t^2)) / pi` the share of a disk beyond a chord at `t` radii from its center:
`t10 = 0.6870`, so the 10-90% width is `1.374 s tan(a)` (2.54 cm for a 0.53 degree sun at 4 m). The tests compute `t10` by
bisection rather than trusting the constant.

`DirectionalLightComponent::ShadowSoftness` defaults to 0.53 (the sun's angular diameter): `RendererComponents.h:27` and
its comment ("the sun is about 0.5" becomes "the sun: 0.53, the default"). The tooltip (`RendererComponents.cpp:233`) is
`graphics-options`' description, which lands first and already says "the sun: about 0.53" (`graphics-options.md` D11):
this stream keeps it word for word, so the two never conflict. Stored values do not change (scenes store every property:
`Samples/*/Assets/**.stscene`).

**D7 The cascades' depth ranges: a conservative pyramid per cascade layer.**
- Texture `m_ShadowRanges`: `R32_UINT`, 2D array, layers = the cascade count (step 0's `ShadowCascades` layers), 3 mips:
  mip `l` covers `4 * 2^l` shadow texels each way (4, 8, 16), size `ceil(S / (4 * 2^l))`. Each texel packs
  `nearest = max depth` (reversed-Z: nearest to the light) and `farthest = min depth` of its texels as
  `(ceil(nearest * 65535) << 16) | floor(farthest * 65535)`: rounding outward keeps the range conservative.
  Memory: 21 MiB at 4096 x 4 (16 + 4 + 1), 5.3 MiB at 2048 x 4, 85 MiB at 8192 x 4 (Ultra, beside its 1 GiB map).
- Made in `EnsureShadowMap` with the map, at the size and layer count it made (step 0's `m_ShadowMapSize`, after its
  fallback to half the size, and `m_ShadowMapLayers`; `step0.md` 7.5); a failure fails the map: shadows off, reported, as
  at 2925-2929. Released with it in step 0's `ReleaseShadowMap()` (the texture, its per-level binding sets,
  `m_BoundShadowRanges = nullptr`); `m_DummyShadowRanges` (1x1x1, 0, made in `CreateResources` beside
  `m_DummyShadowMap`, 365) for the dummy bindings.
- Shaders read it as `layout(set = 0, binding = ST_SRV(32)) uniform utexture2DArray u_ShadowRanges;` (in `Shadows.glsl`)
  with `texelFetch`: every shader that includes `Shadows.glsl` enables `GL_EXT_samplerless_texture_functions`, as
  `Forward.frag:3` does (the new `ScreenShadows.frag` and `ShadowView.frag` too).
- Built by `SH/Scene/ShadowRanges.comp` (new) and its variant `Scene/ShadowRangesDownsample.comp`
  (`ST_SHADOW_RANGES_DOWNSAMPLE`): 8x8 threads, one destination texel each, `z` = layer; push constants
  `RenderData::ShadowRangeParameters { uvec2 SourceSize; uvec2 DestinationSize; uint32_t LayerMask; uint32_t pad[3]; }`
  (32 B); threads of layers outside `LayerMask` return. Mip 0 reads the D32 map's 16 texels (`texelFetch`, clamped at the
  edge) and writes the packed range; mips 1 and 2 read 2x2 packed texels of the level below and combine (max of nearest,
  min of farthest: exact on the packed integers). One binding set per level (SRV 0 the source over all layers, UAV 0 the
  destination mip over all layers), so NVRHI places the barriers between levels as it does for the occlusion pyramid
  (`RunShadowOcclusionPhase` 1082-1105).
- Built in `GPU.Shadows` right after the layers are drawn: the CPU path after `DrawShadows` (3060) with every layer; the
  GPU path at the end of `DrawGPUShadows` (after the second occlusion phase, 1376-1382) with
  `LayerMask = all & ~m_CachedCascadeMask`, so kept layers keep their ranges. A cached layer's ranges are valid exactly
  when its depth is: the ranges are part of the cascade cache (no key change needed: the texture is made and released with
  the map, whose pointer `ShadowCacheContent::ShadowMap` holds).
- The occlusion pyramid stays as it is (farthest-only, from the first phase: a different quantity at a different time).

**D8 The filter** (`Shadows.glsl`; `SampleCascadeShadow(int cascade, vec3 relativePosition, vec3 normal, ivec2 pixel)`
returns `struct ShadowSample { float Visibility; float PenumbraTexels; float DiskWeight; }`):

1. Normal offset, light clip position, `uv`, outside-the-square test, `receiverDepth` with the constant bias, the receiver
   plane gradient and `maxCorrection`: unchanged (`Shadows.glsl:81-95`). `size = 1 / ShadowParams.y`,
   `texelPos = uv * size`, `wpt = Parameters.x`, `gradTexel = depthGradient * ShadowParams.y` (depth per texel),
   `slope = |gradTexel.x| + |gradTexel.y|`.
2. **Search region.** `searchTexels = min(ShadowParams.x * Parameters.z / wpt, 31)` (the half-width of the penumbra of a
   blocker at the search distance); `regionTexels = max(searchTexels, 1.5) + 1` (the floor's support plus the bilinear
   footprint). Level `l = regionTexels <= 8 ? 0 : (regionTexels <= 16 ? 1 : 2)`, cell size `cs = 4 << l`; cells
   `floor((texelPos - regionTexels) / cs)` to `floor((texelPos + regionTexels) / cs)`, clamped: at most 5 x 5 by
   construction (the cap of 31 keeps `2 * regionTexels <= 64`).
3. **Classification.** Over those cells, `nearest = max`, `farthest = min` of the unpacked ranges. With
   `spread = min(slope * regionTexels, maxCorrection)` (how far the receiver plane's reference moves within the region):
   `nearest <= receiverDepth - spread` returns lit (`{1, 0, 0}`), `farthest > receiverDepth + spread` returns umbra
   (`{0, 0, 0}`). Both are exact: every comparison tap of the filter reads texels inside the cells and compares against a
   reference inside `receiverDepth +- spread` (the comparison is `stored > reference`, `Shadows.glsl:123`), and the ranges
   are rounded outward. A caster of any thinness anywhere in the region makes `nearest` exceed the reference, so the
   pixel is never classified lit and goes on to the filter: thin casters are never missed by the search.
4. **Blocker estimate.** In the same loop, a cell blocks when its `nearest` exceeds its own reference
   `cellRef = receiverDepth + clamp(dot(gradTexel, cellCenter - texelPos), -maxCorrection, maxCorrection)
   + min(slope * cs / 2, maxCorrection)`; its weight is its overlap with the search square,
   `w = ov.x * ov.y`, `ov = clamp(min(cellMax, texelPos + searchTexels) - max(cellMin, texelPos - searchTexels), 0, cs) / cs`,
   which changes continuously with `texelPos` (no jumps as cells enter the region). `separation = (sum w (nearest - cellRef)
   / sum w) * Parameters.y` (world units; the cell's nearest blocker, biased toward wider penumbrae by at most a cell's
   depth spread). `penumbraTexels = min(separation * ShadowParams.x / wpt, searchTexels)`; 0 when no cell blocks **or when
   `sum w` is 0** (tested before the division: with `ShadowSoftness` 0, `searchTexels` is 0 and every weight is 0, and
   `0 / 0` would put a NaN into `diskWeight` and the visibility, which `mix` and `clamp` do not remove).
5. **The floor: a fixed tent.** Four bilinear comparison taps at `(+-0.5, +-0.5)` texels, weight 1/4 each, each with its
   receiver plane reference: a separable trapezoid of support +-1.5 texels (flat over +-0.5), the same at every pixel and
   frame. The result is continuous as an edge moves under it (no dither, no stair-steps) and its 10-90% width is about
   1.6 texels.
6. **The disk.** For `penumbraTexels > 1`: N taps (`ShadowBias.z`: 8, 16, 24 or 32) on a Vogel disk:
   `offset_i = penumbraTexels * sqrt((i + 0.5) / N) * (cos(i g + phi), sin(i g + phi))`, `g = 2.39996323` (the golden
   angle), the direction stepped by a fixed rotation matrix per tap (no per-tap `sin`/`cos`), `phi = 2 pi *
   BlueNoise(pixel, ivec2(0))`; each tap a bilinear comparison at `uv + offset_i * ShadowParams.y` with its plane
   reference. `diskWeight = clamp(penumbraTexels - 1, 0, 1)`; `Visibility = mix(floor, disk, diskWeight)` (the floor is
   always taken: 4 taps), so the result is continuous where the penumbra crosses 1-2 texels.
7. Comparison taps use `textureGrad(sampler2DArrayShadow(...), vec4(uv, layer, reference), vec2(0), vec2(0))`: an
   explicit level for non-uniform control flow (the map has one level; LocalShadows.glsl:83 does the same for 2D).

`SampleDirectionalShadow(vec3, vec3, float viewDepth, vec2 pixel)` keeps its signature and returns the blended
visibility (`Shadows.glsl:137-152`, blend unchanged). `EvaluateDirectionalShadow(vec3, vec3, float, ivec2)` (for step 3)
returns the `ShadowSample` of the first cascade with `PenumbraTexels` and `DiskWeight` mixed by the blend weight, plus
the cascade index (`int Cascade` added to the struct).

Cost, per pixel in a cascade: 1-25 range fetches (typically 4-9), then nothing (lit or umbra), or 4 taps (floor), or
4 + N taps (wide penumbra). 0.6.0 takes 16 point taps everywhere and 16 more wherever a blocker is found. Which pixels
leave early: umbrae and receivers nearly perpendicular to the light. A receiver sloped in light space (ground under a sun
below about 70 degrees: its depth changes by about a depth bias per texel) is in the map itself, and its own texels span
more than the bias within a cell, so the conservative lit test cannot pass; no cell of its own counts as a blocker in
step 4 (each cell's reference carries the plane's half-cell spread), so it takes the 4-tap floor and gets exactly 1.
That is still cheaper than 0.6.0's 16 point taps, and it is the expected case for most lit ground, not a failure.

**D9 The blue-noise tile.**
- `Tools/GenerateBlueNoise.py` (new, standard library only, deterministic): the void-and-cluster method on a 64x64 torus
  with a Gaussian energy kernel (sigma 1.9, a precomputed 64x64 toroidal table): an initial pattern of 410 ones (10%) from
  `random.Random(7)`; relaxation (move the tightest cluster's one to the largest void until the move would return it);
  ranking (remove ones from the tightest clusters for ranks below 410, then fill the largest voids for ranks 410-4095);
  value `rank * 256 // 4096` (each byte value exactly 16 times). Writes `R/BlueNoise.h`:
  `namespace Strata::RenderData { inline constexpr uint32_t c_BlueNoiseSize = 64; inline constexpr uint8_t
  c_BlueNoise[64 * 64] = { ... }; }` with a header comment naming the tool and its arguments. Run time about 10-20 s.
- `Renderer::GetBlueNoiseTexture()` (`R/Renderer.h:99-100` beside `GetWhiteTexture`): R8_UNORM 64x64 made in
  `Renderer::Init` with the solid textures (94-98) by `writeTexture` (row pitch 64), part of its failure check (103-110).
- `Shadows.glsl`: `layout(set = 0, binding = ST_SRV(33)) uniform texture2D u_BlueNoise;` and
  `float BlueNoise(ivec2 pixel, ivec2 shift) { return texelFetch(u_BlueNoise, (pixel + shift) & 63, 0).r + 0.5 / 256.0; }`.

**D10 Biases at 4096.** The receiver plane bias, the 1-texel depth bias and 1.5-texel normal offset (texel-relative,
`RendererComponents.h:28-29`), the rasterizer's bias (-16, slope -1.5, `SceneRenderer.cpp:80-81`) and cascade blending
stay. "Retuned at 4096" means: the acne case (`GPUSceneShadowTests.cpp:271`) runs at 1024, 2048 and 4096 with the
component biases and with 0 (it must pass unchanged), and a new peter-panning case bounds the gap. Change a bias only if
one of them fails, with the measurement in the commit.

**Frame constants and CPU.** `GatherShadowDraws` (1175-1177): `ShadowParams.x = std::tan(halfAngle)`,
`ShadowBias = (Bias, NormalBias, taps, 0)` with `taps` from the quality row (`SceneRendererSpecification{}`: 16; `.w`
unused, D1).

**The `ShadowFilterQuality` row lands here** (its first use), with every touch point step 0 lists for a knob
(`step0.md` 7.2) so its static check (7.11, case 1) and `graphics-options`' row coverage test (S1) pass: the field
`uint32_t ShadowFilterQuality = 16;` of `RendererQualitySettings`, its `PresetRow` line `{ 8, 16, 24, 32 }`, a line in
`GetPreset`, `MakeSection` (an Int field, "ShadowFilterQuality", described as in section 4.5), `FromSectionValue` and
`ToSectionValue`, `Clamp`'s line (a value not in {8, 16, 24, 32} becomes the next larger one, above 32 becomes 32; the
section's validation refuses them), a `c_RowTraits` line of `graphics-options` (`FollowsPreset` true, `GPUStage`
`"GPU.ScreenShadows"`: from step 3 the filter runs in that pass, and in `GPU.Transparent` for transparent surfaces;
`MemoryBytes` null), `ForThumbnails()` and `ForBake()` 16, and the Medium test (`RendererQualitySettings{}` has 16). The
section's version goes up by one in this commit; step 4's row joins the same version (the stream lands once; the
integrator renumbers if another stream took the same number, `plan.md` 4.1).
`ShadowSampleRotation` and `c_PoissonDisk` leave Shadows.glsl (D2). The scene sets bind SRV 32 and 33
(`CreateSceneBindingSets` 976-1004; the shadow pass's sets the dummies); `m_BoundShadowRanges` joins the rebind test
(2938-2942).

**`ThumbnailCache::c_Version`** 4 -> 5 (the preview sun's softness and every penumbra change thumbnails; AGENTS.md).

**Tests** (`GPU.SceneRenderer.ShadowFilter`, new file `T/Renderer/GPUSceneShadowFilterTests.cpp`; scenes and helpers
shared with `GPUSceneShadowTests.cpp` through `SceneRendererTestUtils.h`):
- "A 0.53 degree sun's penumbra is as wide as the sun makes it": an orthographic camera 1 m above the floor (below the
  plate, so it sees the floor, not the plate) looking straight down at the edge of a 4 x 0.05 x 4 m plate's shadow,
  512x512 over 1 m; one cascade of 4096 texels, `ShadowDistance` 20, the sun straight down; the plate at 4 and 6 m. The
  test requires `GetStats().Cascades[0].WorldPerTexel` at most 8 mm (the edge spans at least 3 texels) and states it. The
  sun's share per pixel (`RenderSunlight`, `GPUSceneShadowTests.cpp:108-151`) across the edge; its 10-90% width within 20%
  of `1.374 h tan(0.265 deg)`; at 1.0 degree, 1.89 times the 0.53 degree width within 10%. Must fail with the full-width
  radius (twice as wide).
- "A still frame's shadow does not change with the TAA frame": TAA on, AO and SSR off, manual exposure
  (`AddNeutralPostProcess`); frames 1 and 9 of the history (the same jitter phase, `c_TemporalJitterPhases` 8,
  `SceneRenderer.h:716`, different `TemporalParams.z`) give bit-identical HDR targets. Must fail with the 0.6.0 rotation.
- "A thin caster's shadow is never lost to the search": a 1 x 1 cm wire along X, 4 m long, 3 m above the floor, a 2
  degree sun straight down, one cascade of 2048 texels with `ShadowDistance` 8; a perspective camera at (0, 1.2, 2.5)
  looking at the origin (the wire is above its view, so the floor under the wire is visible), 512x512. The test requires
  `GetStats().Cascades[0].WorldPerTexel` at most 6 mm, so the wire is at least 1.6 texels wide and its rasterization never
  leaves a gap (a wire narrower than a texel can miss every texel center and cast nothing at all, whatever the filter).
  Along the shadow's centerline (the floor's pixels at z = 0, x within +-1.5 m) the mean visibility is within 0.03 of the
  analytic strip value (`1 - (2/pi)(asin(u) + u sqrt(1 - u^2))`, `u = w / (2 h tan(a))`: 0.879), which the test
  computes. Must fail with the sparse 16-tap search (0.6.0 returns lit wherever no tap hits: its 16 taps over a 32-texel
  radius hit a 2-texel line about 0.6 times on average).
- When step 6 lands, these three cases and the narrow-penumbra and peter-panning cases below set
  `FitShadowsToView = false` with the comment "the quantity depends on the texel size and the cascade's slice, which the
  fit changes after the first frame" (each renders several frames with one renderer: `RenderSunlight` renders three).
- "Narrow penumbrae are smooth and the same at every pixel": softness 0, an edge across the image: the profile across it is
  monotonic (each pixel within its neighbors' range plus 1/255) and its 10-90% width is 1.2-2.2 texels. Must fail with
  the 0.6.0 rotated floor (non-monotonic).
- "The cascades' depth ranges hold every texel's depth": random caster heights over 4 cascades at 1024; read back the
  shadow map's layers and every mip of the ranges (`Renderer::ReadTexture`); every texel's depth lies within its cell's
  unpacked range at every level; both paths; on the GPU path a cached layer's ranges are byte-identical to the frame
  before.
- "Contacts recede at most 2.5 texels" (peter-panning): a cube resting on the floor, sun at 45 degrees, 1024/2048/4096;
  the shadow begins within `2.5 * wpt + 2 pixels` of the cube's base edge.
- Existing: penumbra (226), acne (271, now also at 1024 and 4096), caching (496), blend (571, 652, 688), alpha-masked
  holes (441, with softness 0: the floor keeps them), `GPU.SceneRenderer.GPUDriven`, `.Feedback`.
- `Renderer.BlueNoise` (CPU, `T/Renderer/BlueNoiseTests.cpp`): every byte value appears 16 times; every 8x8 block's mean
  is within 127.5 +- 4 (white noise: about +-9); the mean absolute difference of toroidal neighbors exceeds 90 (white
  noise: 85).
- Components: `Scene.Serialization` (a stored `ShadowSoftness` of 1 still loads as 1, and a component added without values
  has 0.53), `Reflection` (the default), `ComponentSchema.json` (the new tooltip; defaults are not in the schema).

**Docs.** DG new section "### Sun shadows" after "Lights, shadows and HDR" (3320): the filter (D6-D9), the debug view's
metric, the invariants (D2, D3), costs; the Shadows bullet at 3331-3339 shortened to point there. `Lighting.md`'s Shadows
section (104-115): Shadow Softness "0.53 for the sun". Release-note lines (section 8).

**Acceptance** (reference machine, look-dev harness, step 2's Release build against step 0's tip):
- The GPU tests above.
- Noise (per pixel, before step 3): the ThirdPerson scene camera at 3840x2160, no AA, `AmbientOcclusion` off (GTAO's
  pattern would count), High: RMS at most 2.5% of the contrast along row 1450, x 1170-1290, by `shadows/tools/noise.py`
  (the RMS of the luminance profile's residual against its 9-pixel moving average, relative to the edge's contrast; the
  audit did not record its `n`: use `x1 - x0 - 4`). With FXAA likewise. The same captures twice give identical PNGs.
- `GPU.Opaque` in the scene cameras of the three samples at 3840x2160 (5 captures, medians): reported against step 0;
  expected lower (lit and umbra pixels skip the 16-tap search). Not a gate: step 3 removes the filter from it.

### 4.3 Step 3: screen-space sun shadows

**The pass.** `R/SceneRendererScreenShadows.cpp` (new) owns the targets, layouts, pipelines and
`RenderScreenShadows(nvrhi::ICommandList*, const RenderData::FrameConstants&)`; shaders `SH/Effects/ScreenShadows.frag`
and `SH/Effects/ScreenShadowsBlur.frag` (new, fragment passes: comparison sampling in compute through MoltenVK is
unverified, `plan.md` 4.3). GPU stage `GPU.ScreenShadows`, a member of `RendererGPUStages` declared after `Prepass`
(`GPUProfiler.h:26`) and registered right after `GPU.Prepass` (`GPUProfiler.cpp:54`), the position step 0's stage table
assigns it (`step0.md` section 8: each stream registers at its own position so the lines never meet; the three
hard-coded lists below take the same position). It is timed from the end of the prepass (`gpuTimer.Next` after the
prepass's end marker, `SceneRenderer.cpp:3101`, before AO's at 3103): on the GPU path after both occlusion phases and
`RecordCullReadback` (3088-3097), on the CPU path after `DrawBatches(Prepass)` (3100). Recorded only when
`frame.ShadowParams.w > 0` (the cascade count).

**Targets.** Two `RG8_UNORM` render targets of the viewport's size, `m_ScreenShadowTextures[2]` (A and B), made by
`EnsureScreenShadowResources()` and released in `ReleaseRenderTargets` (1938): 33 MB at 3840x2160, 8.3 MB at 1920x1080.
`r` is the square root of the sun's visibility (`r = sqrt(v)`, decoded `v = r * r` by the blur and the forward pass), `g`
the pixel's blur radius in pixels over `c_ScreenShadowMaxBlurPixels` (8). The square root spends the 8 bits where a step
shows: near the umbra a linear code of 1/255 of the sun is 2-8% of the shadow's own light at the sun-to-sky ratios of 5-20
the wave aims for (a visible step at the penumbra's dark end), the encoded one 1/65,025; near full light a step is at
most 2/255 of the sun. Passes: evaluate into A, blur horizontally A -> B, blur vertically B -> A; the forward pass reads
A's `r` at SRV 28. The plan's "R8 mask" is A's red channel: the green carries each pixel's radius between the blur
passes, which saves a third target (decision; section 10, item 4).

`EnsureScreenShadowResources()` is called in extraction right after `EnsureShadowMap` (`SceneRenderer.cpp:2925-2930`)
when `frame.ShadowParams.w > 0`, before the frame constants are written (3001). When it fails it reports ("failed to
create the screen-space shadow targets; rendering without sun shadows") and sets `frame.ShadowParams.w = 0`, as the map's
failure does at 2928, so transparent surfaces (which still filter per pixel) and opaque ones agree.

**Evaluate (`ScreenShadows.frag`).** Layout: CBV 0 (frame constants), SRV 7 (map), SRV 17 (cascades), SRV 22 (D32
depth), SRV 23 (prepass normals, RG16F octahedral view space), SRV 32 (ranges), SRV 33 (blue noise), samplers 9 and 10,
push constants (b1) `RenderData::ScreenShadowParameters { float ContactLength; uint32_t ContactSteps; float
ContactMaxPixels; float ContactFadeStart; float ContactFadeEnd; float ContactThickness; float ContactThicknessScale;
uint32_t Reserved; }` (32 B; the contact fields from step 4, zero before). Per pixel:
1. `depth = texelFetch(depth)`; 0 (nothing drawn): write `(1, 0)`.
2. Position `p = (InverseViewProjection * (UVToNDC((pixel + 0.5) / size), depth, 1)).xyz / w`: the jittered matrices, the
   same the prepass rasterized with, so `p` is the surface point the forward pass shades at the pixel's center
   (camera-relative; `View` is rotation-only). View distance `d = -(View * p).z`.
3. Normal `n = mat3(InverseView) * OctahedralDecode(texelFetch(normals).xy)`: the prepass's geometric normal flipped toward
   the viewer (`DepthPrepass.frag:78-81`), what `Forward.frag:136` computes.
4. `ShadowSample s = EvaluateDirectionalShadow(p, n, d, pixel)`.
5. Blur radius: `pixelWorld` as in step 1; `r_px = s.PenumbraTexels * Parameters.x(c) / pixelWorld` (the penumbra's
   half-width in pixels); `blur = s.DiskWeight > 0 ? clamp(0.35 * r_px * s.DiskWeight, 1, 8) : 0`: zero where the
   floor alone filtered (lit, umbra, narrow penumbrae, contacts), 0.35 of the half-width where the disk did. The blue
   noise puts the disk's error at high spatial frequencies, so a small low-pass removes most of it; and the penumbra
   barely widens: the edge's profile across a disk of radius `r` has a variance of `r^2 / 4`, the blur's Gaussian
   (sigma `blur / 2 = 0.175 r`) adds `0.031 r^2`, so the 10-90% width grows by about 6% (the step 2 test keeps its 20%).
6. Write `(sqrt(visibility), blur / 8)`.

**Blur (`ScreenShadowsBlur.frag`, run twice).** Layout: CBV 0, SRV 22 (depth), SRV 23 (normals), SRV 24 (source), push
constants (b1) `RenderData::ScreenShadowBlurParameters { int32_t DirectionX, DirectionY; float PlaneTolerance; uint32_t
Reserved; }` (16 B). Per pixel: `(v0, g0) = source`; `R = g0 * 8`; `R < 0.5`: output `(v0, g0)`. Else with `p0, n0, d0`
reconstructed as above, taps `k = -K..K`, `K = min(ceil(R), 8)`, at `pixel + k * direction`:
- spatial `exp(-k^2 / (2 sigma^2))`, `sigma = max(R / 2, 0.5)`;
- plane `clamp(1 - |dot(n0, p_k - p0)| / (PlaneTolerance * d0), 0, 1)`, `PlaneTolerance = 0.01` (exact for points of one
  plane at any angle, so grazing floors keep their weight; another surface a percent of the distance away gets none);
- normal `pow(max(dot(n0, n_k), 0), 8)`; taps with depth 0 get 0;
- output `(sqrt(sum w v_k / sum w), g0)` with `v_k = r_k * r_k` (the blur averages visibility, not its encoding): the
  green keeps the center's radius for the second pass.

No light crosses a silhouette (the plane weight), pixels whose radius is 0 are untouched, so lit, umbra and contact-hard
pixels equal the per-pixel evaluation exactly.

**The forward pass.** `Forward.frag`: `layout(set = 0, binding = ST_SRV(28)) uniform texture2D u_ScreenShadows;` in the
`#ifndef ST_FORWARD_TRANSPARENT` block beside SRV 18 (60-66); at 133-138 the opaque variant takes
`float encoded = texelFetch(u_ScreenShadows, ivec2(gl_FragCoord.xy), 0).r; shadow = encoded * encoded;` and the
transparent and glass variants keep
`SampleDirectionalShadow` (the same filter, per pixel, without blur and without contact shadows: they are not in the
prepass). The opaque variant no longer contains the filter: the shader's largest unused path leaves the material shader
(`plan.md` 9, specialization constants). A frame whose mask resources cannot be made renders without the sun's shadow,
reported, with `ShadowParams.w` 0 (above). The scene sets bind A at SRV 28 (`Renderer::GetWhiteTexture()` without it:
its red 1 decodes to visibility 1; `m_BoundScreenShadows` joins the rebind test at 2938-2942). The prepass binds the
same scene set while A is not yet written this frame and the mask passes write A as a render target afterwards; NVRHI's
automatic barriers order the uses, as they do for the AO target (SRV 11) the forward pass reads after `RenderAmbientOcclusion`.

**Frames.** The mask passes run after the prepass and before `Opaque`, so `HDROutput` frames (the probe bake) have them
before their early exit (3182-3204) with nothing special; one-cascade renderers (thumbnails) need nothing special; TAA
frames evaluate the jittered frame (the forward pass reads the same pixel); a frame without a shadowed sun records nothing
(`GPU.ScreenShadows` absent, no targets).

**Stats.** `SceneRendererStats::ScreenShadows` (the frame drew the mask). `GetScreenShadowTexture()` (A) for tests, null
without one. The three hard-coded stage lists gain `GPU.ScreenShadows` right after `GPU.Prepass`:
`EditorStateCommands.cpp:882-883`, `EditorProfilerTests.cpp:91`, `FramePerfLogTests.cpp:283-284`.

**Tests** (`GPU.SceneRenderer.ScreenShadows`, new file):
- "Opaque surfaces get the per-pixel shadow where it is not blurred": a floor and a cube; renderer A draws the floor
  opaque, renderer B the same floor with an alpha-blended material at alpha 1 (the transparent variant's per-pixel
  evaluation; nothing opaque behind it); the sun's share per pixel (`RenderSunlight`) agrees within 0.01 where A's mask
  green is 0 and within 0.08 elsewhere; softness 0.53 and 4 degrees; both paths for A.
- "The blur never crosses a silhouette": a lit pole 1 m in front of a shadowed floor with a 4 degree sun: pole pixels next
  to the floor stay at their unblurred value (green 0 there, share at least 0.98), and floor pixels beside the pole equal
  a renderer's without the pole (same mask, the pole's casting off).
- "Wide penumbrae have no grain without anti-aliasing": a plate's edge under a 4 degree sun, no AA, High filter (24 taps):
  the audit's RMS residual across the penumbra at most 1% of the contrast; the transparent per-pixel version is
  reported (MESSAGE) for comparison.
- "The thin caster's shadow is a continuous line": step 2's wire, opaque floor: every centerline pixel within 0.04 of the
  analytic strip value.
- "The mask is drawn only for a shadowed sun": no sun, or `CastShadows` off: no `GPU.ScreenShadows` time,
  `GetScreenShadowTexture()` null, images identical to a renderer that never had a shadowed sun.
- "HDROutput frames and one cascade": an `HDROutput` frame's HDR target equals the normal frame's; a one-cascade
  renderer's mask equals its per-pixel evaluation outside blur bands.
- Existing: the acne case (271) must pass unchanged (the mask is exactly 1 on a lit plane); every `GPU.SceneRenderer.*`
  and `GPU.Editor.*` suite; `GPU.SceneRenderer.GPUDriven` and `.Feedback` (the mask on both paths).

**Docs.** DG "Sun shadows": the pass, its inputs and outputs, the transparent rule ("change both": AGENTS.md bullet,
section 8), the stage. DG's GPU-driven section notes that the mask is one pass on both paths.

**Acceptance** (reference machine):
- Noise: the ThirdPerson scene camera at 3840x2160, AO off: RMS at most 1.0% (no AA) and 1.0% (FXAA), High; at most 1.5%
  at Medium (16 taps); `noise.py` method as in step 2.
- Cost, 3840x2160 High, 5 captures each, medians: `GPU.ScreenShadows` at most 0.35 ms in each 3D sample's scene camera
  (contact shadows off); `GPU.Opaque` at most 0.02 ms above the same view's `GPU.Opaque` with the sun's `CastShadows` off
  (the forward pass now reads one texel instead of filtering; the audit measured 0.275 ms with shadows against 0.216 ms
  without, PhysicsPlayground FXAA, `architecture/measurements.md`); frame GPU busy at Medium 1920x1080 at most 1.08 times
  step 0's in the same views (edit mode, cached cascades).

### 4.4 Step 4: contact shadows

A march toward the sun inside `ScreenShadows.frag`, after the cascades' evaluation, for pixels whose cascade visibility is
above 0, whose normal and whose own face both face the sun (`dot(n, L) > 0` and `dot(FaceNormal(pixel), L) > 0`,
`L = -DirectionalLightDirection`; `FaceNormal` below) and whose view distance is below `ContactFadeEnd` (beyond it the
fade is 0: no march). The face test matters on smooth-shaded coarse meshes: near a terminator a face can turn away from
the sun while its interpolated normal still faces it, and a ray from such a face enters the object and finds the object
itself; a ray leaving a face that faces the sun never re-enters a convex object, so convex objects never darken
themselves with contact shadows (their terminators stay the cascades' and the shading's).

**Inputs.** `DirectionalLightComponent::ContactShadowLength` (meters, 0-2, default 0.3; 0 turns them off for the light;
`c_MaxContactShadowLength` 2); `ShadowLight::ContactLength`; the `ContactShadows` row (steps; 0 at Low and Medium, 12 High,
16 Ultra). `RenderScreenShadows` fills `ScreenShadowParameters`: `ContactLength`, `ContactSteps` (0 when either is 0),
`ContactMaxPixels = 0.05 * viewportHeight` (108 at 2160 rows, 9 pixels a step at 12 steps), `ContactFadeStart = 0.5 D`,
`ContactFadeEnd = D` (`D` the last split), `ContactThickness = 0.02` m, `ContactThicknessScale = 0.005` (per meter of view
distance). The cap is 5% of the height, not 4%: at 4% (86 pixels) the `lookdev.contact` shot (the plate at 2.6 m, a 40
degree field of view, `step0.md` 9.5) reaches about 7.7 cm along the ray, so only about 76% of the plate's 8.7 cm strip
could be shaded and step 4's acceptance (80%) would fail by construction; at 5% it reaches 9.6 cm.

**The `ContactShadows` row lands here** (its first use), with the touch points listed for `ShadowFilterQuality` in step 2:
the field `uint32_t ContactShadows = 0;`, its row `{ 0, 0, 12, 16 }`, the four functions, the section field (Int,
described as in section 4.5), `Clamp` (1-3 becomes 4, above 32 becomes 32; the section refuses them), a `c_RowTraits` line
(`FollowsPreset` true, `GPUStage` `"GPU.ScreenShadows"`, `MemoryBytes` null), `ForThumbnails()` and `ForBake()` 0, and the
Medium test (0).

**March.**
```
start  = p + n * pixelWorld                                 // one pixel's footprint off the surface
length = ContactLength, shortened so |screen(start + L*length) - screen(start)| <= ContactMaxPixels
stepPixels = |screen(start + L*length) - screen(start)| / ContactSteps
offset = BlueNoise(pixel, ivec2(32, 17))                    // fixed per pixel, decorrelated from the disk's rotation
for i in 0 .. ContactSteps-1:
    t = (i + offset) / ContactSteps
    q = start + L * length * t
    clip = ViewProjection * vec4(q, 1); stop if clip.w <= 0 or the UV leaves [0, 1]
    skip if |q - start| < 2 * pixelWorld                     // the start's own neighborhood (terminators of coarse meshes)
    s = the pixel under q; sceneDepth = depth(s); skip if 0
    ps = position(s, sceneDepth); nf = FaceNormal(s)         // the sampled pixel's surface and its plane
    behind = -dot(nf, q - ps)                                // how far q lies behind that surface along its normal
    if behind > 0.002 and behind < ContactThickness + ContactThicknessScale * d_q: hit at t; break
occlusion = hit ? fade(d) * edge(uv_hit) * (1 - smoothstep(0.6, 1.0, t)) : 0
fade(d) = 1 - smoothstep(ContactFadeStart, ContactFadeEnd, d);  edge(uv) = clamp(min(uv, 1 - uv) / 0.05, 0, 1) (both axes)
visibility *= 1 - occlusion;  if occlusion > 0: blur = max(blur, clamp(0.5 * stepPixels, 1, 4))
```
`FaceNormal(s)` is the plane of the surface the depth buffer holds at `s`, not the prepass's normal: positions are
reconstructed at `s`, `s +- (1, 0)` and `s +- (0, 1)` (four more depth fetches); on each axis the neighbor whose view depth
is nearer to `s`'s is taken (the other may lie across a silhouette), and `nf = normalize(cross(dx, dy))` turned toward the
camera. The prepass's normal is the interpolated vertex normal (`DepthPrepass.frag:78-81`), which on smooth-shaded coarse
meshes leans up to half the angle between faces off the surface: a point just above such a surface then lies "behind"
the leaning plane of a pixel a few centimeters away, and the march would darken lit curved surfaces near their
terminators (and at grazing views) with false contact shadows. Against the plane the depth buffer holds, the test is
exact for planes at any angle: a ray rising off a floor at a grazing sun never reads the floor's own neighbors as
occluders (the acne case), which a depth comparison with a pixel-quantized position would.

Without temporal accumulation, the per-pixel offsets dither where a hit depends on whether a step lands inside the
occluder (the far edge of a contact shadow, over about one step's length on screen). The blur therefore gets half a step's
screen length as its radius (1-4 pixels: the dithered band is about one step wide), and hits fade out over the last 40% of
the march, so the march's end leaves no hard line either. The thickness grows with distance as depth precision and pixel
footprints do; the fades hide what the screen cannot see (far away, off screen). Cost: five depth fetches per step for
sun-facing lit pixels nearer than `D` (at 3840x2160 an estimated 0.1-0.2 ms at 12 steps, mostly from cache; measured in
step 4's acceptance). The lever if the stage exceeds its 0.45 ms: High's row to 10 steps (Ultra keeps 16), with the
measurement in the commit.

**Tests** (`GPU.SceneRenderer.ContactShadows`, new file):
- "A 5 cm plate on the floor casts a contact shadow the cascades are too coarse for": a 0.3 x 0.05 x 0.3 m plate resting on
  the floor, the sun from +X at 30 degrees, one 1024-texel cascade with `ShadowDistance` 200 (the slice's sphere has a
  radius of about 190 m, so texels of about 0.37 m: the plate is a sub-texel caster, left out: `SubTexelCasters` 1, which
  the test checks), camera 2.6 m away with a 40 degree field of view, 256x256, High's 12 steps: the floor 4 cm beyond the
  plate's -X edge (inside the 8.7 cm the plate shades) has a sun share below 0.5; 40 cm away above 0.95. With
  `ContactShadowLength` 0 the first is above 0.95 (must fail without contact shadows). From step 6 on it sets
  `FitShadowsToView = false` ("the plate must stay below a texel; the fit makes the texels finer after the first frame").
- "Contact shadows never shadow an open plane": the acne case's loops with 12 and 16 steps: identical to no shadows within
  2/255.
- "Contact shadows never darken a convex lit surface": a smooth-shaded sphere of 16 x 8 segments (coarse: its vertex
  normals up to about 11 degrees off its faces) of radius 0.5 m with `CastShadows` off (the cascades leave it lit, so only
  contact shadows could darken it) and nothing else in view, the sun at 20, 45 and 70 degrees, High and Ultra: every pixel
  of the sphere equals the image with `ContactShadowLength` 0 within 2/255. Must fail without the start's face test
  (rays from faces turned away from the sun under normals turned toward it enter the sphere). The mesh is the test's own
  procedural sphere (`BuiltinAssets`' sphere only if its tessellation is that coarse, unverified).
- "Contact shadows have no grain without anti-aliasing": the plate scene at 512x512, the camera 1.5 m away, no AA: along
  the rows crossing the contact shadow's far edge (4-9 cm from the plate), the RMS residual against a 9-pixel moving
  average (`shadows/tools/noise.py`'s method) is at most 2% of the lit-to-shadow contrast. Must fail with a fixed blur
  radius of 1 pixel (the per-pixel offsets dither the edge over about one step's length).
- "Contact shadows fade with distance and at the screen's edges": the plate at 0.95 D shades at most 20% of what it shades
  near; a plate half off screen leaves no band at the edge.
- "Medium draws none": a `SceneRendererSpecification{}` renderer's image equals one with `ContactShadowLength` 0.
- Both paths (`GPU.SceneRenderer.GPUDriven` gains a contact case at High).
- Components: `Reflection` (range 0-2), `Scene.InactiveProperties` (inactive without `CastShadows`, like its siblings,
  `ReflectionTests.cpp:454`), `Scene.Serialization` (a 0.6.0 light loads with 0.3, no warning; 5 is clamped to 2),
  `Editor.Commands` (`component.set` and undo; the inactive list at `EditorCommandTests.cpp:924`),
  `ComponentSchema.json`, `Feature.stscene` (the Sun's `ContactShadowLength` 0.4).

**Docs.** DG "Sun shadows" (contact shadows, their fades and thickness, the face tests, the blur by step length, and the
limit every screen-space method has: they come from every opaque surface in the depth prepass, also from mesh renderers
whose `CastShadows` is off, and from nothing off screen); `Lighting.md` (step 5) says the same for people.

**Acceptance** (reference machine): the look-dev scene's 5 cm plate (S0.6, shot `lookdev.contact`) at 2.6 m, High,
3840x2160: the floor beside the plate on the side away from the sun reads at most 50% of the lit floor's sun contribution
over at least 80% of the strip `0.05 / tan(elevation)` long (the harness's crop; the sun's share from a capture with the
sun's intensity at 0, as `RenderSunlight` does); the ThirdPerson robot's foot plate (shot `tp.feet`, the audit's
`tp_foot_contact_x4.png` framing) shows a solid shadow; no AA and FXAA captures of both show no dither band at the
contact shadows' far edges (the grain test's bound, 2%, on the `lookdev.contact` crop). `GPU.ScreenShadows` at most
0.45 ms at 3840x2160 High (12 steps) in each 3D sample's scene camera.

### 4.5 Step 5: adoption

- **Quality rows** (added in steps 2 and 4 with their touch points; one line each in step 0's table, the section's
  version + 1 once for the stream): `ShadowFilterQuality` (wide-penumbra taps): 8 / 16 / 24 / 32, Custom one of 8, 16,
  24, 32; `ContactShadows` (steps): 0 / 0 / 12 / 16, Custom 0 or 4-32. Fields of `Renderer.Quality` described for agents:
  "ShadowFilterQuality: taps of the sun's shadow edges where they are wider than a shadow texel; more taps, less grain
  before the screen-space blur (8 Low, 16 Medium, 24 High, 32 Ultra)"; "ContactShadows: steps of the sun's contact
  shadows, which ground objects below the shadow map's detail (0 off: Low and Medium; 12 High; 16 Ultra)".
  `ForThumbnails()` and `ForBake()`: 16 and 0. `SetQuality` with only these changed rebuilds nothing (they are push
  constants and a frame constant). Step 5 checks that the Graphics Quality page (`graphics-options` step 1) shows both rows
  with their stage (`GPU.ScreenShadows`) and adds them to `Docs/Manual/Graphics.md`'s preset table (`graphics-options`'
  page; an additive line each).
- **Samples**, through editor commands on each sample opened in place for this change (AGENTS.md, Samples):
  `component.set {"entity": "<sun>", "component": "DirectionalLight", "values": {"ShadowSoftness": 0.53}}` for the
  ThirdPerson suns `0A16D1AE6976CD46` (Course) and `CB0854A118AE218E` (MainMenu), the Physics Playground's
  `8BC93C155B43B766` and the Open World's `BB31D33D75352EA5` (the IDs in the samples' scene files). `ContactShadowLength`
  stays 0.3 unless the look-dev close-ups (ThirdPerson feet, Playground dominoes, Open World forest floor) show
  over-darkening; a sample that changes it records the value and the reason in the commit. Each sample's
  `StrataTests/Editor/<Sample>Commands.json.in` gains `component.get` expects of `/values/ShadowSoftness` 0.53. Tetris
  keeps its stored 1.0 (it changes only through the shading defaults); its picture is retaken if it shows.
- **Feature test**: the feature project's `Renderer.Quality` sets `ShadowFilterQuality` 32 and `ContactShadows` 16 under
  Custom (non-default for High); `Feature.stscene`'s Sun keeps 0.6 (non-default against 0.53).
- **Docs**: DG "Sun shadows" complete; DG "Quality settings" (step 0's) lists the two rows; `Docs/Manual/Lighting.md`'s
  Shadows section rewritten (units: Shadow Softness in degrees, 0.53 for the sun, what it does to penumbrae; Contact Shadow
  Length in meters; Graphics Quality's two rows and what they cost; the `shadows` debug view for people); the AGENTS.md
  bullet (section 8); release-note lines.
- Pictures: retaken once at the stream's end (`plan.md` 4.1, Samples), not here.

### 4.6 Step 6: cascades where the receivers are (SDSM)

The last step above the cut. Section 2.4 shows the depth range alone does not move eye-level views; this step fits both
the depth range and the squares to the receivers, keeps the last cascade on its sphere for coverage, and selects cascades
by containment.

**D11 The reduction: `SH/Scene/ShadowFit.comp`** (new), in `GPU.ScreenShadows` after the mask, on both paths, reading the
complete prepass depth (the GPU path's occlusion pyramid holds only the farthest depth, the sky included, of the first
phase alone, `HZBBuild.comp:7-10`, `SceneRendererGPUDriven.cpp:994-1012`, so it cannot give the near bound or the
receivers' extent: the plan's "top levels of the depth pyramid" is replaced by this one pass on both paths, which also
keeps them equal).
- Bins by view distance, a quarter stop each: `bin = clamp(int(floor(4 * log2(d / 0.0625))), 0, 79)`
  (`RenderData::c_ShadowFitBins` 80, `c_ShadowFitBinBase` 0.0625 m: 6.25 cm to 65 km).
- `RenderData::ShadowFitResult { uint32_t Nearest, Farthest, SurfacePixels, Reserved; ShadowFitBin Bins[80]; }`,
  `ShadowFitBin { uint32_t MinX, MinY, MinD, MaxX, MaxY, MaxD; }` (24 bytes; 1,936 bytes in all, `static_assert`): per
  bin the box of its receivers in the camera's **view space** (x and y of `mat3(u_Frame.View) * p`, `View` being
  rotation-only, and the view distance `d`), and the nearest and farthest `d` overall, as `EncodeOrderedFloat`
  (`Include/OrderedFloat.glsl`); minima are stored complemented so every field reduces with `atomicMax` and the buffer
  clears to 0 (an empty bin has `MaxD == 0`: no float encodes to 0). View space, not light space: the CPU re-projects each
  bin's box through the readback camera's rotation into any later light direction and camera (D12 rule 5), so neither a
  turning sun nor a turning camera makes the readback unusable.
- 16x16 threads; shared arrays of 80 x 6 + 3 entries (a loop of two rounds clears and flushes them); one global
  `atomicMax` per non-empty shared entry per group. Layout: CBV 0, SRV 22 (depth), UAV 0, push constants
  `RenderData::ShadowFitParameters { uint32_t Width, Height, Reserved[2]; }` (16 B).
- Recorded only when the specification's `FitShadowsToView` is on and the frame has a shadowed sun (`ForThumbnails()`
  and `ForBake()` turn the flag off, so `HDROutput` frames record none and need no submission at their exit).
- Readback: a ring of 6 `BufferReadback`s of 1,936 bytes, each entry recording the frame's render count, fit epoch, the
  camera's world position and rotation (`glm::dvec3`, `glm::dmat3` of the inverse view) and whether the frame was
  fitted; submitted where the exposure's is (`SceneRenderer.cpp:3262, 3328`); polled without waiting in
  `UpdateShadowFit`.

**D12 The fit on the CPU: `R/ShadowFit.h/.cpp`** (new; `class ShadowFitTracker`, pure C++, unit-tested).
`SceneRenderer::UpdateShadowFit(const RenderScene&, const SceneCamera&)` runs at the start of extraction, before
`m_FeedbackFrame = UsesFeedback(...)` (2872): it takes the newest completed readback of the current epoch and lets the
tracker decide this frame's layout target. `FindShadowCasters` lays the cascades out from it. Rules, in this order:
1. **Epochs.** `ResetShadowFit()` (public), a camera cut (`IsCameraCut`, 2602), another render scene that is not a clone,
   `SetQuality` changing resolution or cascades, a spec with `FitShadowsToView` off, or a light that stopped casting
   shadows start a new epoch; until a readback of the new epoch arrives, the layout is 0.6.0's (step 6 changes nothing
   then: `0.8 log + 0.2 uniform` from `camera.Near` to `min(ShadowDistance, camera.Far)`, spheres).
   `GameRenderer` calls `ResetShadowFit` with `ResetExposureAdaptation` (62, 74); captures and the project thumbnail on
   their first frame (`EditorViewportCommands.cpp:611`, `ProjectThumbnail.cpp:137`). **The first readback of an epoch
   sets `N`, `F` and every side directly**, without hysteresis: the fallback layout is not a state to shrink from
   (otherwise the move from `[camera.Near, D]` to the fitted range would be a shrink waiting 8 readbacks, and a capture
   could not settle within its bound).
2. **No receivers** (`SurfacePixels` 0): 0.6.0's layout.
3. **Depth range.** `nearTarget = max(camera.Near, min(nearest, previous frame's nearest transparent draw) * 2^-0.5)`
   (the transparent draws' nearest view distance is recorded by `GatherDraws` each frame: transparent surfaces are not in
   the depth); `farTarget = min(D, farthest * 2^0.5)`, `D = min(ShadowDistance, camera.Far)`; if `farTarget >= 0.7 D`
   (every acceptance shot of section 2.4: their farthest receivers reach the shadow distance) the far bound `F` is `D`,
   else `F` is quantized below `D`. Quantization: `N = 2^(floor(4 log2 nearTarget) / 4)`,
   `F = 2^(ceil(4 log2 farTarget) / 4)`, `F >= N * 2^0.5`. Hysteresis: a bound that must grow (N down, F up) changes at
   once; one that could shrink changes only after 8 consecutive readbacks wanted at least one step less, then to the least
   shrunk of them.
4. **Slices.** With `F = D`: the `n` slices over `[N, D]`, `split_c = lambda N (D/N)^((c+1)/n) + (1 - lambda)(N + (D -
   N)(c+1)/n)`. With `F < D`: the first `n - 1` slices over `[N, F]` by the same formula with `n - 1` and `F`, and the
   last cascade from the band before `F` to `D`. The last cascade always ends at `D` and fades out there as in 0.6.0: it
   is the safety net for receivers no readback has seen yet. A fitted far bound that also ended the last cascade would
   leave ground revealed beyond it (the camera turning from a near wall to a vista, a door opening) outside every
   cascade, lit, for the 2-3 frames until a readback grows `F`: a flash of unshadowed ground. With one cascade, `F` is
   `D`. `lambda = DirectionalLightComponent::CascadeDistribution` (default 0.9, section 2.4; 0.6.0's layout keeps its 0.8
   on fallback frames); the blend bands as today (506).
5. **Squares** (perspective cameras only; orthographic views keep spheres, the depth range still applies). Each bin's
   view-space box is re-projected: its eight corners through the readback camera's rotation and position into world
   space relative to this frame's camera (in double), then through this frame's light rotation (`m_LightView`'s
   rotation rows) into light-space x and y, and their current view distance (along this frame's forward) gives the bin's
   distance range now. Each cascade but the last: the union of the re-projected boxes of the bins whose distance range
   overlaps its slice (blend band included), grown on each side by the light-space displacement of the slice's eight
   corners between the readback camera's rotation and this frame's (turning reveals receivers within that distance of
   the old view), then to `1.05 side / (1 - 66 / S)` (5%, and 33 of its own texels on each side: the filter's reach, `S`
   the map's size), as a square of side `2^(ceil(4 log2 side) / 4)` (quarter stops), at least 1/16 and at most the slice's
   sphere diameter, centered on the union's center snapped to its texels in absolute light space (as 485-489).
   Re-projecting makes a turned sun or camera cost no coarser frames: the Open World's `DayNight` script turns its sun in
   0.25 degree steps about every 0.4 s (`Samples/OpenWorld/Scripts/DayNight.h:19-21, 41-44`: a 600 s day), and a rule that
   fell back to spheres after any turn of the light would soften every near shadow for 2-3 frames at each step, a
   periodic pop. Hysteresis per cascade as for the bounds; a change of `N` or `F` starts the cascades' hysteresis afresh
   (their slices moved). The last cascade keeps its slice's sphere, the coverage guarantee of D13: the sphere of a slice
   `[a, D]` holds every point of the view frustum nearer than `D` when `a <= D k`, `k = tan^2(fovY/2) + tan^2(fovX/2)`
   (the squared distance to its center is convex in the view distance, so the frustum's end points bound it), which holds
   for any `a < D` at the samples' 55-60 degree fields of view at 16:9 (`k` 1.13 and 1.39). Where it does not (narrow
   fields of view: `k` is 0.13 at 20 degrees), the last cascade takes the sphere of `[N, D]` instead of `[a, D]` (coarser
   texels in that cascade only), which holds every frustum point from `N` to `D` by construction and every nearer one
   when `N <= D k`; only receivers nearer than `N` (closer than 0.7 times the nearest one seen 2-3 frames earlier) at
   fields of view where `N > D k` can then lack a cascade for those frames. Bins that are empty for a cascade give that
   cascade its sphere for the frame.
6. **Depth** stays the sphere's (`FarZ`, `SphereNearZ`): a fitted square only narrows x and y, so every point of the slice
   stays inside the cascade's depth range.

`ShadowFitTracker` state per renderer: the epoch, the current `N`, `F`, sides and their shrink counters, the newest
readback seen. Output: `struct ShadowFitLayout { bool Fitted; bool SquaresFitted; float Near, Far;
std::array<float, c_MaxCascades> Sides; std::array<glm::dvec2, c_MaxCascades> Centers; bool Changed; }` (`Changed`: a
slice or a square's side differs from the previous frame's, a texel-snapped shift of a center not counted; it adds to
`ShadowFitChanges` and makes `ShadowFitSettled` false until a readback of the new layout confirms it).

**Visibility feedback.** Layout changes keep the GPU path's feedback, as 0.6.0's spheres do, which move texel by texel
every frame the camera moves: on feedback frames every cascade gets the slots of all casters the feedback saw, a quarter
and some more (`SceneRendererGPUDriven.cpp:549-556`), not slots per layout, and every layout's last cascade, which keeps
its sphere and reaches `D`, holds every caster of every receiver in front of `D`, so a new layout's casters were casters
of the previous one. An overflow still falls back as today (1598). Turning feedback off for every changed layout, as
first drafted, would walk the view on the CPU every frame the camera moves in play (the squares snap texel by texel) and
fail the `.Feedback` cases at the first fitted frame.

**D13 Cascade selection by containment** (`Shadows.glsl`). `int SelectShadowCascade(float viewDepth, vec3 position)`
replaces `SelectCascade` (`Shadows.glsl:33-42`, removed, so code that samples the cascades at arbitrary positions, such as
`light-and-materials`' volumetric fog below its cut, cannot keep selecting by distance alone): from the first cascade
whose split exceeds `viewDepth`, the first whose square holds the position's light-space `uv` (of the normal-offset
position `SampleCascadeShadow` uses) inside `[m, 1 - m]` (`m` = 33 texels, the filter's reach, as rule 5 grows the
squares; the last cascade with `m` = 0, as 0.6.0 tests it) and whose depth is in `[0, 1]`; -1 when none. A receiver the
readback did not see (a fast turn, an object that just appeared, ground revealed beyond a fitted far bound) falls to a
coarser cascade, ultimately the last one's sphere, which reaches `D` (rule 4), for the frames until the next readback
grows the square or `F`: coarser for 2-3 frames, and within the last sphere's guarantee (rule 5) never unshadowed.
`CascadeBlendWeight` blends toward the next cascade only when that one holds the position, and fades the last cascade as
today. The debug view uses the same function.

**How culling, `CascadeFit.comp` and the cache keys agree.** The tracker's layout is applied once, in `FindShadowCasters`,
to `m_Cascades` (the squares and `WorldPerTexel`; `Radius`, `FarZ` and `SphereNearZ` stay the sphere's, D1) and
`frame.CascadeSplits`; the CPU caster test (`GetCascadeMask`),
`CullConstants::CascadeBoxes` (`SceneRendererGPUDriven.cpp:715`), `CascadeFitConstants` (`GatherShadowDraws`) and
`ShadowCacheKey::Box` read those boxes, so both paths cull, fit and draw the same cascades, and the cache redraws a cascade
exactly when its box or slice changed. A still camera over still receivers keeps its layout (hysteresis), hence its keys
and its cached layers. Sub-texel caster culling follows the finer texels (`GetCascadeMask` reads `WorldPerTexel`).
`GetCascadesReached` (1246-1264) reads the same boxes.

**Stats and captures.** `SceneRendererStats`: `ShadowFit` (fitted this frame), `ShadowFitSquares`, `ShadowFitSettled`
(no readback is pending that may change the layout: after any layout change or epoch, settled once a readback of a frame
drawn with the current layout arrived and left it unchanged; always true with `FitShadowsToView` off), `ShadowFitNear`,
`ShadowFitFar`, `ShadowFitChanges` (layout changes since the renderer was made), `Cascades[c].Fitted`.
`viewport.capture` waits while `!ShadowFitSettled`, bounded by `c_MaxCaptureShadowFitFrames` (12: a readback 2-3 frames
late sets the layout, a second one confirms it, and the Mac's latency is unmeasured), counted in
`waitedFrames`, and reports `"shadowFitSettled"` beside `"preExposureSettled"` (756); `ProjectThumbnail`'s
completeness check (`ProjectThumbnail.cpp:145-158`, beside `exposureWaiting`) adds the same condition.
`DescribeRenderStats` (206-235) gains `"shadowFit": {"fitted", "squaresFitted", "settled", "near", "far"}` and
`"cascades": [{"near", "far", "texelSize"}]`.

**Renderers.** `SceneRendererSpecification::FitShadowsToView` (default true); `ForThumbnails()` and `ForBake()` turn it
off (single images, deterministic); the capture renderer keeps it on and waits; tests that check 0.6.0's split positions
or count cached cascades frame by frame from the first frame turn it off: `GPUSceneShadowTests.cpp:496` (both renderers;
a new fitted-cache case below covers caching with it on), 571, 652, 688 and `RoofScene`'s users (its comment's splits are
0.6.0's), and this stream's own texel-size cases (step 2's penumbra, TAA-frame, wire, narrow-penumbra and peter-panning
cases; step 4's plate). Every other test keeps the default; a case that breaks gets the fit turned off only with a
comment saying why its quantity depends on the layout, never to hide a difference between paths. Tests render until
`ShadowFitSettled` with each frame waited for (`RenderToImage`), the pattern of `RenderUntilPreExposureSettled`
(`GPUScenePostProcessTests.cpp:83-92`), so readbacks arrive at the same frame on every machine.

**`CascadeDistribution`.** `DirectionalLightComponent::CascadeDistribution` (0-1, default 0.9, `Slider(0, 1)`, tooltip
"How the cascades share the distance from the nearest to the farthest surface the camera sees: 0 evenly, 1 each cascade
the same ratio farther than the previous. Higher puts more detail near the camera"), inactive without `CastShadows`;
`ShadowLight::Distribution`; `Feature.stscene`'s Sun 0.95.

**Tests.**
- `Renderer.ShadowFit` (CPU, `T/Renderer/ShadowFitTests.cpp`; the tracker alone with synthetic readbacks): quantization in
  quarter stops; growth at once, shrinking after 8 readbacks to the least shrunk; fallback to 0.6.0's layout for a new
  epoch, no receivers and `FitShadowsToView` off; the first readback of an epoch sets the layout at once (no shrink
  counting from the fallback); the far bound snaps to `D` from 0.7 D; below it `n - 1` slices end at `F` and the last
  cascade still ends at `D`; the transparent near bound; the camera's translation shifts the squares; a sun turned by 5
  degrees after the readback gives squares that hold the re-projected receivers (no spheres; must fail with light-space
  bins); the rotation margin grows a square by the corners' displacement; the last cascade is always its sphere, of
  `[N, D]` at a 20 degree field of view; orthographic cameras get spheres; sides within [1/16, 1] of the sphere; the same
  inputs give the same layout.
- `GPU.SceneRenderer.ShadowFit` (new file):
  - "The fit covers every receiver": `RoofScene` (its roof shadows the whole floor) with the fit on, rendered until
    `ShadowFitSettled` (at most 8 frames, each waited for): every floor pixel nearer than `0.9 D` has a sun share below
    0.05; after a turn of 30 degrees (not a cut: cuts start at 45, `TextureStreamer.h:93-96`) the first frame's pixels are
    still all shadowed.
  - "A revealed floor is shadowed in its first frame": `RoofScene` with a wall 3 m in front of the camera that hides the
    floor; once settled (receivers: the wall alone, so `F` is fitted near 4 m: three cascades end there, the last reaches
    `D`), the wall's `MeshRenderer` is removed: in the next frame, before any readback saw the floor, every floor pixel
    nearer than `0.9 D` has a sun share below 0.05 (the floor beyond `F` falls to the last cascade). Must fail without
    containment, and with all `n` slices ending at a fitted `F` (those pixels are lit).
  - "A high camera gets fine texels": the Physics Playground's framing (camera at (0, 22, 30) toward the origin over a
    floor), 4096 x 4: after settling, `GetStats().Cascades[0].WorldPerTexel` at most half of the unfitted renderer's, and
    the shadows view's median at most 0.6 times the unfitted one.
  - "Cuts and first frames use 0.6.0's layout": the first frame and the frame after a camera cut have `ShadowFit` false and
    the splits of 0.6.0's formula.
  - "A still camera keeps its fitted cascades cached": GPU path; after settling, 10 frames keep every cascade
    (`CachedCascadeMask` full) and `ShadowFitChanges` constant; a cube moving 10 cm in view changes no box.
  - "Both paths fit alike": the CPU and GPU-driven renderers of each `GPU.SceneRenderer.GPUDriven` shadowed scene, 6
    frames each, end with equal boxes and pixel-equal images.
  - "A moving camera keeps the visibility feedback": GPU path, a camera panning 0.5 m a frame for 10 frames over a settled
    view: `FeedbackRequests` stays true on every frame, and every frame's image equals the CPU path's (no overflow left a
    caster out). Must fail with feedback turned off for every changed layout.
  - "Captures wait for the fit": `GPU.Editor.Viewport`: a capture of the Playground framing reports `shadowFitSettled`
    true and equals a second capture byte for byte.
- Components: `CascadeDistribution` as `ContactShadowLength` in step 4 (range, inactive reason, serialization default 0.9,
  commands, schema, feature value).

**Docs.** DG "Sun shadows": the fit (D11-D13), its fallbacks, what captures wait for, the agreement rule; DG "View
commands" (`shadowFitSettled`); `Lighting.md` (Cascade Distribution).

**Acceptance** (reference machine, look-dev harness, High, 3840x2160, `debugView: "shadows"`): for each 3D sample's
scene camera and editor opening view, `medianPixelsPerTexel` at most 1.5 (the plan's target; the model predicts
ThirdPerson 1.29, Physics Playground 0.98, Open World eye view 1.49) and `aboveFourShare` at most 1%; the ThirdPerson
editor opening view no longer has every receiver in the last cascade. A shot above 1.5 first gets the cheap remedy: the
sample's `CascadeDistribution` set through `component.set` (0.85-0.97, the value with the lowest median, recorded with
its figures in the commit and the sample's command script). A shot still above 1.5 but at most 1.7 does not block
landing: the landing report names it with its per-cascade figures, and the integration's acceptance (`plan.md` 1.1)
lists it as unmet with its remedy (step 8's 6 cascades); the release notes then claim no figure for that view. Medium
at 1920x1080 likewise reports its medians (model: 1.29, 0.98, 1.49 at 1080 rows). `GPU.ScreenShadows` grows by at most
0.03 ms (the reduction) at 3840x2160. Live captures (`viewport.capture {live}`) of the Open World's play with its day
cycle running show no periodic change of near shadows' sharpness (the re-projection, rule 5).

### 4.7 Cut line, and what lies below it

Steps 1-6 are the stream. Below the cut, in this order, each only when everything above has landed (`plan.md` 4.3); what
is not done moves to the Roadmap's "Later" when the wave lands. They are sketched here so that the design above does not
block them:

7. **Local lights on par with the sun**: `SourceRadius` (meters, default 0.05) on point and spot lights; LocalShadows.glsl
   takes D8's classification (a range pyramid per atlas tile, built for redrawn tiles), half-width PCSS
   (`separation * SourceRadius / distanceToLight` in the tile's texels, `ShadowFilterRadius` the floor), the floor tent and
   the Vogel disk, and the blue noise (D2's temporal rotation then leaves the code entirely); the specular widening in
   `PBR.glsl` is agreed with `light-and-materials`; tiles sized by receivers' texel density.
8. **Up to 8 cascades** (6 High, 8 Ultra): `c_MaxCascades` 8; `CascadeSplits` as two vec4 (`FrameConstants` changes:
   coordinate with the later lander, `plan.md` 4.1); `CascadeFitConstants`, `CullConstants` (`c_CullMaxCascades`), three
   sort-key bits (`c_KeyCascadeShift`), `CascadeFit.comp`'s workgroup of 8, `ShadowViewCounts` 8 rows.
9. **Distant shadows** (`FarShadowDistance`, 0 off): a far cascade from static casters, cached until the sun moves a step
   or cells change, blended into the last cascade (D13's containment makes it a fallback layer).
10. **16-bit depth** for far cascades and Ultra, with the caster extension toward the light capped.
11. **Per-cascade cache keys** (the snapped square, depth range and casters' LOD criteria instead of the camera), so far
    cascades stay cached in play; D12's stable boxes make it effective.

## 5. Quality rows and defaults

| Row | Low | Medium (0.6.0's costs) | High (default) | Ultra |
| --- | --- | --- | --- | --- |
| `ShadowFilterQuality` (wide-penumbra taps) | 8 | 16 | 24 | 32 |
| `ContactShadows` (steps) | 0 (off) | 0 (off) | 12 | 16 |

Medium keeps 0.6.0's sample count (16 filter taps) and no contact shadows; its new fixed costs are the range build in
`GPU.Shadows` (0 with cached cascades) and the mask, blur and reduction in `GPU.ScreenShadows`, against the filter's
removal from `GPU.Opaque` (budget: section 7).

Defaults that change (release notes, section 8): `ShadowSoftness` 0.53 for new lights and the preview sun; every scene's
penumbrae (half the old width, no temporal rotation, a fixed tent where narrow, thin casters kept); contact shadows at
High and Ultra on every directional light (`ContactShadowLength` 0.3 for existing scenes, a new property); cascades fitted
to the receivers in every scene (`CascadeDistribution` 0.9, a new property); thumbnails (`c_Version` 5).

## 6. Tests: suites and must-fail checks

| Suite (file) | Step | Cases | Fails without the change |
| --- | --- | --- | --- |
| `GPU.SceneRenderer.ShadowView` (new) | 1 | statistics against the CPU histogram; off costs nothing; no sun | the view and stats |
| `GPU.Editor.Viewport`, `Editor.Viewport` | 1, 6 | capture's `debugView.shadows`; settings round trip; `shadowFitSettled` | the enum, the result, the wait |
| `GPU.SceneRenderer.ShadowFilter` (new) | 2 | analytic penumbra; TAA-frame invariance; thin caster mean; smooth narrow edge; conservative ranges; peter-panning | full-width radius; temporal rotation; sparse search; rotated floor |
| `Renderer.BlueNoise` (new) | 2 | value histogram, block means, neighbor differences | a white or broken table |
| `GPU.SceneRenderer.ScreenShadows` (new) | 3 | mask against per-pixel; no bleeding; grain at most 1%; continuous thin caster; nothing without a sun; HDROutput, one cascade | the blur's weights; the radius rule |
| `GPU.SceneRenderer.ContactShadows` (new) | 4 | the 5 cm plate; no self-shadowing on a plane; a convex coarse sphere stays lit; no grain at the far edge; fades; Medium off | contact shadows; the start's face test; the blur by step length |
| `Renderer.ShadowFit` (new) | 6 | the tracker's rules, incl. the first readback, the last cascade to `D`, re-projection after a sun turn, narrow and orthographic views | each rule; light-space bins |
| `GPU.SceneRenderer.ShadowFit` (new) | 6 | coverage incl. a turn; a revealed floor; density gain; cut fallback; still-camera cache; both paths; feedback while panning | containment; the last cascade to `D`; the fit; feedback kept |
| `GPU.SceneRenderer.Shadows` (existing) | 2-6 | all kept; acne at 1024/2048/4096 with contact on; cache case with the fit off | |
| `GPU.SceneRenderer.GPUDriven`, `.Feedback` | 2-6 | pixel-equal paths with the mask, contact and fit | |
| `Reflection`, `Scene.InactiveProperties`, `Scene.Serialization`, `Editor.Commands`, `Reflection.ComponentSchema`, `FeatureTest` | 2, 4, 6 | the two new properties and the new default | |
| `Editor.Perf` (`EditorProfilerTests.cpp`), `Runtime.FramePerfLog` | 3 | `GPU.ScreenShadows` in the stage lists | |

A regression test is checked to fail without its fix (AGENTS.md, Testing): each "must fail" above is run once against the
step's parent commit and the result stated in the commit message. The `GPU.SceneRenderer.GPUDriven` and `.Feedback`
suites and the cached-against-uncached case run before every commit of the stream.

## 7. Perf: metrics, targets and the stream's A/Bs

**New provisional entries** (`StrataTests/Perf/Budgets.json`, appended), measured by a new case in `RendererPerfTests.cpp`,
"Render cost of the sun's shadows at 3840x2160 on Medium and High": the existing shadowed scene (100,000 cubes,
`Perf::AddCameraAndLight(scene, 4 * halfExtent)`, 602-636) at 3840x2160, `MeasuredRendererSpecification()` (cascade
cache off), one renderer at Medium and one at High (step 0's presets with these rows), medians of the GPU stages over the
measured frames (`MedianGPUStageMs`):

```json
"PerfGPU.Renderer.ScreenShadows4KHigh": { "Unit": "ms", "Provisional": true, "Target": 0.45 },
"PerfGPU.Renderer.ScreenShadows4KMedium": { "Unit": "ms", "Provisional": true, "Target": 0.3 },
"PerfGPU.Renderer.ShadowRedraw4KHigh": { "Unit": "ms", "Provisional": true, "Target": 1.0 },
"PerfGPU.Renderer.ShadowRedraw4KMedium": { "Unit": "ms", "Provisional": true },
```

`ShadowRedraw*` is `GPU.Shadows` with every cascade drawn and its ranges built; the High target is an estimate (4 times the
fill of Medium, the same vertex work, plus the range build: about 0.15 ms reading 256 MiB at 4096 x 4), which the
integration campaign measures; the test prints the High-to-Medium ratio (expected at most 3.5). The Medium redraw has no
target until the campaign measures it. The existing measured entries (`Render100kCubesShadowed`, `Render300kCubesShadowed`
CPU, the `EditFrame*` metrics) must stay within their budgets; expected, since the fit lowers the CPU's caster work (fewer
casters per fitted cascade), the feedback stays on through layout changes (D12) and the tracker re-projects 80 boxes a
frame; the A/B below measures it, and the shadowed-cubes case keeps its checks
(`RendererPerfTests.cpp:626-630`: every cube of the view stays a caster or a sub-texel caster under the fit, since every
visible cube is a receiver inside the fitted squares).

**A/Bs** (information, never budgets): a Release build of the stream and step 0's tip's Release build (kept in
`build/agent-sun-shadows-base` from `git archive` of the tip, deleted when the stream lands), interleaved, 3 pairs, each
measurement under `python Tools/PerfLock.py --label sun-shadows -- <command>`, judged by the ratio of medians:
- the look-dev harness's shots (`Tools/LookDev.py`, step 0's options; unverified until S0.6 lands) of the three samples'
  scene cameras at 3840x2160 High and 1920x1080 Medium, with `renderStats.gpuMs` per stage (5 captures each);
- the new `PerfGPU.Renderer.*Shadow*` cases and `Render100kCubesShadowed` (`ctest --preset windows-release -R
  "PerfGPU.Renderer" -L perf` filtered to them; their numbers stated, not budgeted).
Each step's commit message states its stage medians against the base. A step whose Medium 1920x1080 GPU busy exceeds
1.08 times the base in the scene cameras finds the cause in its stage times before it lands. The first lever, which
changes none of Medium's sample counts: `ShadowFit.comp` on every second row and column (its bins only bound receivers,
within rule 5's margins). The ranges are never skipped for a drawn layer: the filter's lit and umbra tests are exact only
while every drawn layer's ranges are current (D7).

## 8. Docs, skills and release notes

- **DG**: new "### Sun shadows" (after "Lights, shadows and HDR", 3320): the filter, the ranges, the mask and blur, contact
  shadows, the fit and its fallbacks, the debug view's metric, what both paths and the cache share, costs; the Shadows
  bullet of "Lights, shadows and HDR" (3331-3339) shortened to a pointer; "View commands" (2756): the `shadows` view,
  `shadowFitSettled`; "Quality settings" (step 0): the two rows; "Local light shadows" (3361-3404): their rotation still
  turns with TAA until step 7.
- **Manual**: `Docs/Manual/Lighting.md` Shadows (104-127): Shadow Softness in degrees (the sun 0.53), Contact Shadow Length,
  Cascade Distribution, the two Graphics Quality rows, the `Shadows` view in the viewport's debug menu.
- **AGENTS.md** (step 0 freed the room), one bullet in "Rendering": "The sun's shadow reaches opaque surfaces through the
  screen-space mask (SRV 28) and transparent ones through `SampleDirectionalShadow`: change both; shadow filters use fixed
  noise, never `TemporalParams.z`, and cascade layouts come only from `ShadowFitTracker` on the CPU
  ([Sun shadows](Docs/DeveloperGuide.md#sun-shadows))." The guide states it in full.
- **Skills**: `strata-editor-automation` (the view, its result, `shadowFitSettled`).
- **Roadmap**: the wave 7 entry's `sun-shadows` lines (S0.7's text) gain "fitted to the depth range and the receivers'
  extent"; Status when the wave lands (integration).
- **Release-note lines** (0.7.0):
  - "Sun shadows as fine as the screen: cascades fitted to what the camera sees, with 4096-texel cascades at High, put
    about one shadow texel on each pixel at 3840x2160; the new `shadows` debug view shows it."
  - "Physical soft shadows: penumbrae at the sun's real width (Shadow Softness now defaults to 0.53 degrees; stored values
    are kept), sharp at contact, without grain and identical from frame to frame with or without TAA; thin casters such as
    wires and grass keep their shadows."
  - "Contact shadows ground small objects at High and Ultra (Contact Shadow Length on the Directional Light, 0.3 m)."
  - "Graphics Quality gains Shadow Filter Quality and Contact Shadows."
  - "Every scene's shadows change: half as wide penumbrae as before for the same softness, fitted cascades (Cascade
    Distribution, 0.9), contact shadows at High; transparent surfaces keep a per-pixel filter without contact shadows."

## 9. What the stream shares and how merges resolve

Merge order (`plan.md` 4): `graphics-options` first, this stream second, then `camera-and-tone`, then
`light-and-materials`. This stream merges main before landing and resolves:

| Shared | This stream's change | Resolution |
| --- | --- | --- |
| `SH/Scene/Forward.frag` | the sun term (129-140) and the SRV 28 declaration (beside 60-66) | `light-and-materials` owns the indirect block (172-266) and may declare SRV 29-31 beside SRV 18: keep both declarations in slot order |
| `R/SceneRenderer.cpp/.h` | the mask pass after the prepass; SRV 28/32/33 in the scene layout (275-308) and `CreateSceneBindingSets`; stats; options; readbacks | append-only hunks; slots in numeric order; stats fields appended at the end of their structs |
| `R/SceneRenderData.h` | new structs appended; `ShadowParams`/`ShadowBias`/`TemporalParams` comments | others append; no `FrameConstants` growth here |
| `SH/Include/Scene.glsl` | slot comment lines for 32, 33 | keep every stream's lines; `light-and-materials`' volumetric fog (its step 8, below its cut, which names 32) takes the next free slot, 34, when it lands (step 0's rule: the comment is the arbiter) |
| `SH/Include/Shadows.glsl` | `SelectCascade` replaced by `SelectShadowCascade(viewDepth, position)` (step 6) | `light-and-materials`' `VolumetricShadows.glsl` (below its cut) calls `SelectShadowCascade`: a position in the air may lie outside a fitted square, and containment falls back to a cascade that holds it |
| `R/RendererQuality.h/.cpp` | two rows with every touch point (`step0.md` 7.2), `graphics-options`' `c_RowTraits` lines | one line each; both rows' `GPUStage` is `"GPU.ScreenShadows"` (`graphics-options.md`'s merge table names `GPU.Shadows` for `ShadowFilterQuality`: superseded, the filter runs in the mask pass from step 3); the section's version: the later lander re-bumps above main's |
| `R/RendererComponents.h/.cpp` | `ShadowSoftness`' default; two properties | `graphics-options` writes the six descriptions and the `Shadows` section first: keep its `ShadowSoftness` description word for word, append this stream's properties after `CascadeBlend`, each with a description (its test S6 requires one), and extend the inactive list |
| `R/GPUProfiler.*` and the three stage lists | `GPU.ScreenShadows` registered right after `GPU.Prepass` | step 0's stage table gives each stream its own position (`step0.md` section 8); keep every stream's line where the table puts it |
| `Docs/Manual/Graphics.md` (`graphics-options`') | the two rows in its preset table | additive lines |
| `ViewportDebugView`, `DebugViewSchema`, descriptions | `Shadows` | `light-and-materials` adds five views: keep all, in landing order |
| `Feature.stscene`, feature `Renderer.Quality` | Sun values; two rows | keep both sets; rerun `StrataTests.FeatureTest` |
| `Budgets.json`, `Strata/CMakeLists.txt` shaders | entries; seven shaders | append |
| Samples' scene files (one line per entity) | the suns' `ShadowSoftness` | a conflicting line: take main's file and re-run this stream's `component.set` commands (section 4.5) through the editor; retake pictures at the end |
| `ThumbnailCache::c_Version` | 4 -> 5 | the later lander bumps above main's |
| DG, AGENTS.md, `Lighting.md` | sections named above | `Lighting.md`'s Shadows section is this stream's; the rest is `light-and-materials'` |

No new layer, include exception or `MayInclude` entry: everything stays in the Renderer layer and `StrataEditorCore`.

## 10. Risks and open questions, with the decisions recommended

1. **The plan says "depth range"; this spec fits the squares too** (step 6). The model (2.4) shows the depth range alone
   leaves eye-level views at 2.1-2.4 and the plan's 1.5 needs either receiver-fitted squares or 8 cascades.
   *Decision: fit both; keep the last cascade on its sphere and select by containment, so a stale or missed square costs
   sharpness for 2-3 frames, never a missing shadow.* If the look-dev's live captures show visible texel-density changes
   while turning, the rotation margin (D12 rule 5) grows before anything else.
2. **The GPU path's pyramid top levels are not used for the fit** (D11): farthest-only, sky included, first phase only.
   *Decision: one reduction pass on both paths.*
3. **SDSM latency** (2-3 frames): fast approaches or reveals. *Decision: margins (2^-0.5 near, 2^0.5 far), immediate
   growth, containment, the last cascade always reaching the shadow distance (so a reveal beyond a fitted far bound is
   coarser for 2-3 frames, never lit), bins re-projected through any later camera and sun (no fallback frames when either
   turns), 0.6.0's layout on cuts.*
4. **RG8 instead of an R8 mask**: 8-bit visibility is 256 levels across penumbrae of at most about 250 pixels (31 texels
   each side at up to 4 pixels per texel), about a level per pixel, and `camera-and-tone`'s dither covers the rest; the
   steps that would show are near the umbra, where a linear code is a large share of the shadow's own light.
   *Decision: RG8 ping-pong (33 MB at 4K) storing the square root of the visibility (step 3); move to R16G16 only if the
   look-dev sheet still shows contours.*
5. **Two more scene slots (32, 33)** beyond S0.5's assignment, and `light-and-materials` names 32 for its volumetric fog
   below its cut. *Decision: this stream takes 32 and 33 in its first commit and records them in `Scene.glsl`'s comment;
   the volumetric fog takes 34 when it lands (section 9); MoltenVK's per-stage sampled-image limit is far above the scene
   layout's count (unverified on the Mac until the integration's Mac gate).*
6. **Transparent surfaces** get the per-pixel filter (no blur: the disk's grain stays at High's 24 taps) and no contact
   shadows. *Decision: accept; stated in the Manual and release notes.*
7. **Local shadows still turn their noise with TAA** until step 7. *Decision: unchanged pixels now (D2), fixed below the
   cut.*
8. **Medium's cost at 1920x1080** (the wave allows 1.15 times step 0's for all streams together): the mask, blur, ranges
   and reduction add a few tenths of the frame where the forward pass loses the filter. *Decision: this stream's share is
   at most 1.08 times; lever in section 7.*
9. **8192 at Ultra**: the range texture is 85 MiB beside 1 GiB of cascades. *Decision: accept; step 10 halves both.*
10. **Penumbrae of casters farther than the search distance** (`0.2 radius + 5 m`, `CascadeFit.comp:70`) are cut short in
    the first cascades (a 15 m tree near the camera). *Open question; decision: keep 0.6.0's distance in this stream,
    measure with the look-dev forest shot, and raise it toward the 31-texel cap only if the cut shows.*
11. **The fitted layout's split weight** (0.9) differs from 0.6.0's 0.8, which stays for fallback frames, so tests that
    render one frame keep 0.6.0's layout. *Decision: as specified; per-light, so a scene can choose.*
12. **Contact shadows' thickness heuristic** can over-shadow behind thin occluders seen edge-on. *Decision: the plane test
    against the depth buffer's own face normal and the 2 cm + 0.5% thickness; tuned on the look-dev close-ups, values in
    the commit.*
13. **Unverified on MoltenVK and Linux**: `textureGrad` on `sampler2DArrayShadow`, R32_UINT array storage, the new
    fragment passes. *Decision: the integration's Mac gate (`plan.md` 7.4); nothing here needs an optional feature.*
14. **Step 0 names** (2.3) are the plan's; the implementer adopts step 0's actual names without changing this design.
15. **Contact shadows without TAA**: per-pixel march offsets dither the far edge of each contact shadow. *Decision: the
    blur radius follows the march's step length on screen (1-4 pixels), hits fade over the last 40% of the march, and a
    grain test bounds it (2%); if the look-dev's no-AA crops still show a pattern, the next lever is 16 steps at High.*
16. **Contact shadows from what casts no shadow**: the march sees every opaque surface of the prepass, also mesh renderers
    with `CastShadows` off, and nothing off screen. *Decision: accept, as every screen-space method must; stated in DG and
    `Lighting.md`.*
17. **Coarse smooth-shaded meshes' terminators** (the samples' low-poly models): a face turned from the sun under a normal
    turned toward it. *Decision: no march from such faces (step 4's face test); the convex-sphere test keeps it.*
18. **Most lit ground is not classified lit** by the range test (a receiver sloped in light space spans the cells' depth
    ranges itself): it takes the 4-tap floor, exactly 1. *Decision: accept (still under 0.6.0's 16 taps); the step 3
    acceptance measures the mask's cost.*

## 11. Deliver in this order

1. **Step 1** (commits): the slot comment (SRV 32, 33); `SceneRendererStats::Cascades`; `ShadowView.glsl`, the overlay,
   the statistics pass and its readback; `ViewportDebugView::Shadows`, the commands' schema, result and wait, the panel's
   menu and legend; tests; DG "View commands", the skill.
2. **Step 2**: `Tools/GenerateBlueNoise.py` and `R/BlueNoise.h` with `Renderer.BlueNoise`; `Renderer::GetBlueNoiseTexture`;
   the `ShadowFilterQuality` row with all its touch points and the section's version; the ranges (texture, shaders, both
   paths, cache, step 0's `ReleaseShadowMap`) with their test; the filter (D6-D8) with `ShadowParams`/`ShadowBias`, the
   local shadows' functions moved; `ShadowSoftness` 0.53 and `c_Version` 5; the filter tests and the bias checks; DG
   "Sun shadows" (first part).
3. **Step 3**: `GPU.ScreenShadows` (after `GPU.Prepass`) and the three lists; the targets (square-root encoded) and
   passes; Forward.frag's opaque read; tests; DG. Push `ci/wave7-sun-shadows` once for the compilers (CI runs no GPU tests).
4. **Step 4**: the `ContactShadows` row; the march (face tests, blur by step length); `ContactShadowLength` with its
   component tests and feature value; the contact tests; DG.
5. **Step 5**: the rows' descriptions on the Graphics Quality page and in `Graphics.md`; samples and their command
   scripts; the feature project's rows; Manual, AGENTS.md, release lines.
6. **Step 6**: `ShadowFit.comp` and its readback; `ShadowFitTracker` with `Renderer.ShadowFit`; the layout in
   `FindShadowCasters` (the last cascade to `D`, re-projected bins); `SelectShadowCascade` replacing `SelectCascade`;
   `CascadeDistribution`; stats, captures and thumbnails; the GPU fit tests; DG, Manual.
7. **Finish**: the perf case and `Budgets.json` entries; the A/Bs of section 7; the samples' pictures and screenshots
   retaken after merging main; independent review; NVIDIA Debug ctest with every GPU suite under validation, Release
   ctest without perf, `CompatLint.py`; CI green on `ci/wave7-sun-shadows`; land (`plan.md` 7.1).

If time runs short, stop after a complete step; every step leaves the branch mergeable. Step 6's square fit may land in a
second commit series after its depth range (rules 1-4 and D13 first), but the step's acceptance needs both.

## 12. The first hour

1. Read `plan.md` 3-4 and 4.3, this spec, AGENTS.md's Rendering digest, DG "Lights, shadows and HDR", "Local light
   shadows" and the GPU-driven path; skim `shadows/audit.md`.
2. Confirm step 0 is on main: `RendererQuality.h`, `SetQuality`, `ReleaseShadowMap`, `ForThumbnails`/`ForBake`, the
   `Renderer.Quality` section, `Scene.glsl`'s slot comment, the GPU stage table in DG "GPU timing", `Tools/LookDev.py` and
   its shot names; note any name that differs from section 2.3. If `graphics-options` has landed, read its `c_RowTraits`
   and the tests its rows (S1) and descriptions (S6) impose on this stream's rows and properties.
3. Create `.worktrees/sun-shadows` from step 0's tip, initialize submodules, configure `build/agent-sun-shadows`, build
   Debug and Release.
4. Run `StrataTests.exe --test-suite=GPU.SceneRenderer.Shadows,GPU.SceneRenderer.GPUDriven,GPU.SceneRenderer.Feedback`
   under validation on the tip and keep the output as the stream's baseline.
5. Export step 0's tip into `build/agent-sun-shadows-base` and build its Release; under `Tools/PerfLock.py`, take the
   look-dev baseline of the three samples' scene cameras (3840x2160 High and 1920x1080 Medium; no AA, FXAA, TAA) and the
   ThirdPerson feet close-up, and run `shadows/tools/noise.py` on the no-AA ThirdPerson capture to reproduce 4.6%.
6. Start step 1 with its test: move `PitchedCamera` and `FloorRowDepth` into `SceneRendererTestUtils.h`, write the CPU
   histogram the GPU test compares with, then `SceneRendererStats::Cascades` and `ShadowView.glsl`.

## 13. Review corrections

An independent review (2026-10-27) checked this spec against the code at bc6ee123, the five commits on `agent/step0`
(4409b03b; none touches the shadow code), `plan.md`, the audits and the other streams' specs beside it (`step0.md`,
`graphics-options.md`, `camera-and-tone.md`, `light-and-materials.md`). Its file:line claims held: every line count of
the 2.1 table and every function range checked (Shadows.glsl, CascadeFit.comp, Forward.frag, DepthPrepass.frag,
SceneRenderer.cpp/.h, SceneRendererExtraction.cpp, SceneRendererGPUDriven.cpp, SceneRenderData.h, Scene.glsl,
RendererComponents, GPUProfiler, Renderer.cpp, StreamingView, EditorViewportCommands, ViewportRenderer, ViewportPanel,
EditorStateCommands, ThumbnailCache, ProbeBake, GameRenderer, ProjectThumbnail, the tests and Budgets.json) was right, and
the containment bound `a <= F (tan^2(fovY/2) + tan^2(fovX/2))`, the 10-90% constant 1.374 and the wire's 0.879 were
re-derived and hold. What changed, and why:

**Design defects (the image or the frame would have been wrong):**

1. *A NaN in the filter.* With `ShadowSoftness` 0, `searchTexels` is 0, every blocker weight is 0 and the separation was
   `0 / 0`, which `clamp` and `mix` carry into the visibility. Step 2 now tests `sum w` before dividing (D8 step 4).
2. *Unshadowed reveals beyond a fitted far bound.* Ending the last cascade at a fitted `F` left ground revealed beyond it
   (a camera turning from a near wall to a vista) outside every cascade, lit, for 2-3 frames: the beyond-`F` region is
   generally outside the last slice's sphere too. The last cascade now always reaches the shadow distance and fades as in
   0.6.0; with `F < D` the other `n - 1` cascades are fitted to `[N, F]` (D12 rule 4). `ShadowBias.w` stays unused, and
   none of the acceptance shots changes (their receivers reach `D`, so `F = D` there). New test: "A revealed floor is
   shadowed in its first frame".
3. *A periodic sharpness pop in the Open World.* Rule 5 fell back to spheres for 2-3 frames whenever the sun had turned
   more than 0.1 degree since the readback; the sample's `DayNight` script turns it 0.25 degrees about every 0.4 s. The
   reduction now stores each bin's receivers as a view-space box (D11, 1,936 bytes), which the CPU re-projects through
   the readback camera into any later camera and sun: no fallback frames for a turned sun or camera. Tests updated.
4. *Feedback turned off while the camera moves.* `Changed` (any slice or square differs) made `UsesFeedback` false, but
   squares snap texel by texel every frame the camera moves, as 0.6.0's spheres do, so the GPU path would have walked
   the view on the CPU every frame in play, and the `.Feedback` cases would fail at the first fitted frame. Feedback
   slots on feedback frames are per caster set, not per layout (`SceneRendererGPUDriven.cpp:549-556`), and the last
   cascade reaching `D` holds every caster of every receiver: layout changes now keep the feedback (new test).
5. *First fitted layout delayed by hysteresis.* The move from the fallback `[camera.Near, D]` to the fitted range is a
   shrink, which rule 3 made wait 8 readbacks; the first readback of an epoch now sets the layout directly, and the
   capture's bound is 12 frames.
6. *False contact shadows on coarse smooth-shaded meshes.* The plane test used the prepass's normal, which is the
   interpolated vertex normal (`DepthPrepass.frag:78-81`), not the surface's plane; and rays from faces turned away from
   the sun (under normals turned toward it, near terminators) enter the object. The plane is now the depth buffer's own
   face normal (`FaceNormal`, four more fetches a step), and the march starts only on pixels whose own face faces the sun;
   a convex-sphere test (with `CastShadows` off, so only contact shadows could darken it) must fail without the face test.
7. *Contact shadows noisy without TAA.* Binary hits with per-pixel march offsets and a 1-pixel blur leave a dithered band
   about one step long at each contact shadow's far edge. The blur radius now follows the step's screen length (1-4
   pixels), hits fade over the last 40% of the march, and a grain test (2%) must fail with the 1-pixel blur.
8. *The contact acceptance could not pass.* At `ContactMaxPixels` 4% of the height the `lookdev.contact` march reaches
   about 7.7 cm, so only about 76% of the plate's 8.7 cm strip could be shaded against the 80% asked; the cap is 5%.
9. *Banding at the penumbra's dark end.* An 8-bit linear visibility step is 2-8% of a shadow's own light at the sun-to-sky
   ratios the wave aims for; the mask stores the square root (step 3).
10. *Transparent and opaque surfaces disagreeing on failure.* A failed mask allocation left `ShadowParams.w` at the
    cascade count, so transparent surfaces kept their shadows while opaque ones lost them; it is now zeroed as the map's
    failure does (2928), with the call placed before the frame constants are written.

**Conflicts with step 0 and the other streams:**

11. *GPU stage position.* Step 0's stage table registers `GPU.ScreenShadows` right after `GPU.Prepass`; the spec appended
    it after `Overlays` (step 3, section 9).
12. *Scene slot 32.* `light-and-materials` names SRV 32 for its volumetric fog (below its cut); this stream uses 32 and 33
    above its cut and lands second, so the fog takes 34 when it lands (D1, section 9). The items also join the scene
    binding layout (`SceneRenderer.cpp:275-308`), not only the sets.
13. *Quality rows.* The rows now land where they are first used (steps 2 and 4), each with every touch point step 0 lists
    and a `graphics-options` `c_RowTraits` line whose stage is `GPU.ScreenShadows` (that spec's table says `GPU.Shadows`
    for `ShadowFilterQuality`: superseded), `Clamp` rules for Custom values, and `Graphics.md`'s table lines.
14. *`ShadowSoftness`' description.* `graphics-options` writes it first; this stream keeps it word for word and changes
    only the default.
15. *`SelectCascade`.* Replaced by `SelectShadowCascade(viewDepth, position)` and removed, so `light-and-materials`'
    volumetric shadows (below its cut) select by containment instead of by distance into a fitted square.

**Steps an agent would have had to re-decide, and tests that would not have failed or run:**

16. The debug view's statistics: the capture renderer renders no frame while a capture waits, so a poll "at the start of
    each frame" never saw them; `ResolveShadowViewStats()` is now called by `ContinueCapture` beside `ResolveGPUStats`.
    The overlay blends over an image it cannot read, so "desaturated" became a gray veil; the reduction needs two
    barriers; `DescribeDebugView` gets the statistics as a parameter; the saved state's error text and the chip's tooltip
    name the view.
17. The range texture's GLSL declaration and extension, its release in step 0's `ReleaseShadowMap()`, and its size after
    step 0's allocation fallback; `CascadeBox::Radius` stays the sphere's (it sets the search distance); orthographic views
    keep spheres; narrow fields of view take the last sphere over `[N, D]`.
18. Step 2's tests: the penumbra camera now sits below the plate (looking down from above saw the plate, not its shadow);
    the wire test had no camera (straight down, it would have seen the wire) and no texel size (a wire narrower than a
    texel can rasterize to nothing); the TAA-frame test and every texel-size case now turn the fit off from step 6 on,
    since several frames of one renderer change the layout after the first readback.
19. Step 3's `GPU.Opaque` acceptance ("lower by at least the measured filter share") is now a number: within 0.02 ms of the
    same view with the sun's shadows off. Step 1's p10 check applies only to cascades whose receivers reach their split.
20. The A/B's first lever (building the ranges only for some drawn layers) would have broken the filter's exactness;
    replaced.
21. Step 6's 1.7 escape: a shot above 1.5 first gets the sample's `CascadeDistribution` through commands; one still above
    1.5 is reported unmet in the integration's acceptance rather than claimed.

**Checked and kept** (the author's open questions): fitting the squares as well as the depth range (the model's case is
sound, and the coverage guarantee now also covers reveals); one reduction pass on both paths instead of the occlusion
pyramid (which is farthest-only, sky included, first phase only: `HZBBuild.comp:7-10`); `CascadeDistribution` 0.9 for
fitted layouts with 0.8 kept for fallback frames; RG8 ping-pong (now square-root encoded); the search distance
`0.2 radius + 5 m`; the 1% form of the plan's "no visible receiver above 4" (measurable, where "none" is not). Also kept:
16-bit range packing (conservative by outward rounding; its quantum can exceed the depth bias in cascades with casters far
toward the light, which only makes the classification more conservative), the exactness of the lit and umbra tests, and
the half-width PCSS analysis. Noted rather than changed: most lit ground under a sun below about 70 degrees is not
classified lit (its own texels span each cell's range) and takes the 4-tap floor, so the early exit is mostly the
umbra's (section 10, item 18).

## 14. As built

Step 0 landed with the names section 2.3 assumes (`RendererQualitySettings` and its `PresetRow` lines,
`RendererQuality::ResolveShadowMapSize`, `SceneRenderer::GetShadowMapSize`, `m_ShadowMapLayers`, `ReleaseShadowMap()`,
`InvalidateShadowCache()`, `ForThumbnails()`/`ForBake()`, the slot comment with SRV 28, 32 and 33, the look-dev shots by
name). Differences found while building:

- Step 1: the shadows view's CPU types live in `Strata/Renderer/ShadowView.h` (`ShadowCascadeStats`, `ShadowViewRatioStats`,
  `ShadowViewStats`, and `ShadowView::GetBin`, `GetBinCenter`, `Summarize`, `SummarizeAll`, which the GPU test's CPU
  histogram shares); `RenderData::ShadowViewParameters` is the three colors (48 bytes) until step 3 adds the mask;
  `SceneRenderOptions::ShadowViewColors` defaults to the theme's Malachite, Sulfur and Cinnabar. The view's draw and count
  are `SceneRenderer::DrawShadowView` in `SceneRendererShadowView.cpp`. `Tools/LookDev.py capture --debug-view <view>`
  takes the acceptance's statistics.
- `graphics-options`' `c_RowTraits` is not on main yet: the rows' trait lines are added when that stream lands.
- The worktree builds in its own `build/windows` (the stream brief), not `build/agent-sun-shadows`.
- Step 2: the blue-noise tile uses a Gaussian of sigma 1.5, not 1.9 (at 1.9 an 8x8 block's mean strays up to 8.3 from
  127.5, at 1.5 up to 4.6, 1.9 on average; `Renderer.BlueNoise` bounds the worst block at 6 and the mean at 2.5).
  `ShadowFilterQuality` is an Enum field of "8", "16", "24" and "32" (the section refuses other values). The thin-caster
  test's camera has a 40 degree field of view (at 60 the cascade's texels are 7.5 mm). The cascade seam test's check is
  now the larger of the in-cascade step and a quarter pixel: the filter has no per-pixel noise any more.
- Step 3: `SceneRendererStats::ScreenShadows` and `SceneRenderer::GetScreenShadowTexture` as specified; the mask's blur
  constants are `SceneRenderer::c_ScreenShadowMaxBlurPixels` (8) and `c_ScreenShadowPlaneTolerance` (0.01). The test
  comparing the mask with the per-pixel filter compares blurred pixels with the per-pixel result's mean along the edge
  (17 pixels): a single per-pixel value carries the disk's grain (up to 0.12 at 16 taps), which the 0.08 bound cannot
  hold. The silhouette test's pole is lit on its visible face (a sun from +Z): under a sun straight down its faces are
  parallel to the light, whose receiver plane makes their visibility 0.5 (shaded by N.L = 0, so never seen). The TAA
  suite's smoothing cases now bound the converged roughness at 0.9 of a frame's: with the mask, a frame's penumbra is
  smooth already.
