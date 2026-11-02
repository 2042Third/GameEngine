# Stream `reflections`: metal and wet ground (wave 8)

The owner (2026-11-01, item 4, verbatim): "Double check specular lighting, or show the specular light bouncing off of metal
sheets like the side of a bus or oil tank or wet ground. (pretty important for the showcase)". The plan reads that as
highlights and reflections on the surfaces themselves (plan 4.2, "What item 4 means here"; the other reading, sun thrown off a
sheet onto its neighbors, is not built: plan 9, and the owner's first sheet asks). The specular audit found the shading sound in
stills and the reflections at fault; the showcase audit found the owner's three shots working on blocks, with three defects
that land exactly on shot 4 (the comb along reflected edges, `p06`/`p10`; sky wedges at the screen's sides; a misplaced bright
probe patch, `p14`). This stream repairs them, in two parts: **R1 lands alone before the showcase tunes its materials**
(plan 4.3, 7.2; decisions.md ruling 6), **R2** lands with landing 1 or after it. **R2 is led by highlights** (decisions.md,
"Highlights first", the owner's answer of 2026-11-01: "highlights are extremely important just like in a drawing, because they
should be cheap"): step 7 (the sun's size, small lights' highlights, a source radius for highlights only) starts as soon as R1 is
in review and joins R1's landing if it is verified by then. Section 13 lists what the independent review changed.

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `E/` is `StrataEditor/src/Editor/`, `T/` is
`StrataTests/src/`, `DG` is `Docs/DeveloperGuide.md`. Line numbers are of 76f28a21 (release 0.7.0). Shaders are named as
`strata_add_shaders` registers them; after S0.10 the files are `.slang` in HLSL syntax (`Forward.frag` is `Forward.frag.slang`,
`Include/X.glsl` is `Include/X.slang`), and the code in this spec is written in that syntax (`float3`, `lerp`, `saturate`,
`Texture.Load`). If S0.10's box ended without Slang, write the same in GLSL. S0.11's names (`IndirectInputs`,
`GatherIndirectSpecular`, `SH/Include/IndirectLight`) are the plan's (plan S0.11); where S0.11 named them otherwise as built,
S0.11's names win. The audits' folders: `<spec>` is `.worktrees/wave8-plan/specular/`, `<show>` is `.worktrees/wave8-plan/showcase/`.

The machine's rules for this stream (plan 4.1, decisions.md):

- The Windows reference machine (i7-13700K, RTX 5090, 3840x2160 at 150%) on its temporary memory. Worktree
  `.worktrees/reflections`, branch `agent/reflections` from S0.12's tip, build `build/agent-reflections`
  (`cmake --preset windows -B build/agent-reflections`), never another agent's directory and never the owner's `build/windows`.
- Before each commit: Debug ctest and every `GPU*` suite under the validation layer, the feature test,
  `python Tools/CompatLint.py --changed-since <S0.12 tip>`. R1 lands through `ci/wave8-reflections` green on every job.
- No campaign, no `PerfStability.py`, no `-L perf` against budgets. The stream A/Bs its own stages, interleaved, under
  `Tools/PerfLock.py`, as information (section 6.3). New metrics enter `Budgets.json` as `Provisional` with their `Target`.
- Exact-pixel tests turn new defaults off; both draw paths stay pixel-equal; nothing touches how casters are drawn, so the
  shadow caches are untouched (checked in section 8).

## 1. Goal

At High and Ultra, in every anti-aliasing mode (TAA, FXAA, none), in this order of priority (the order the owner's eye finds it):

1. **A metal sheet at a grazing view shows a clean reflection of what stands beside it.** No fixed screen door, no ghost
   columns beside thin objects, no hatched band along reflected edges. Noise on the specular lab's sheet under 1% (today
   3.5-5.0%), frame-to-frame flicker in a camera strafe at most 1% over 32 px blocks and the per-pixel crawl SSR adds at least
   halved (never measured before: step 1).
2. **Reflections show the world where the world belongs.** Hits fade only where the reflected ray leaves the screen (today 10%
   of every side, 384 px at 4K, whatever the ray does); ambient occlusion no longer darkens what screen-space reflections found
   (dark smudges beside objects on wet ground); the misplaced bright probe patch of `p14` is explained and gone.
3. **Highlights, first in R2** (step 7): the sun's angular size in its highlight (Overcast's 10-degree sun is a pin dot
   today), lantern and fire highlights judged on metal and wet ground at dusk (the audit could not judge them), and a source
   radius for highlights only on point and spot lights (a property per light, energy-normalized so that a larger source widens
   the highlight without brightening it, one evaluation per light, no change to diffuse light or shadows). Every highlight is
   judged with no AA, FXAA and TAA, in motion as well as in stills: one that shimmers or aliases fails.
4. **Materials as measured** (R2, step 8): F0 from the IOR (water 0.02; 1.5 stays exactly 0.04) and a night sky without bands
   (shot 6).
5. **Sharper probes at High and Ultra** (R2, step 10): the `ReflectionProbeResolution` row, 256 per face, with its memory in the
   traits table, and conversions spread over frames when one frame would take more than 2 ms.

Rules held throughout (plan 1, 4.1): native resolution; nothing needs TAA to look right, with one named exception, Medium's
Checkerboard reflections (`Effects/ScreenSpaceReflections.comp`), which stay as they are, unchanged in cost, and which the open
items name; off costs nothing; the CPU and GPU-driven paths stay pixel-equal; Medium keeps 0.6.0's costs;
`SceneRendererSpecification{}` stays Medium.

Not the goal (plan 4.3's cut and plan 9): keyframed probe captures for a moving sun, sized point and spot lights' diffuse light
and shadows (their highlights are step 7c), hit-distance blur, a wet-surface model, normal-map-aware filtering, anisotropy, SSR on clear coat, SSR on transparent surfaces,
planar reflections, run-time probe capture, sun thrown off sheets onto neighbors.

## 2. Where we start

### 2.1 Files

| File | Lines | Role | This stream |
| --- | --- | --- | --- |
| `SH/Effects/ScreenSpaceReflectionsSpatial.comp` | 140 | Spatial trace (High, Ultra): march, bisection, color fetch | owns: pattern (D2), crossing test and validation (D4), back faces (D9) |
| `SH/Effects/ReflectionResolve.comp` | 105 | Spatial resolve: 3x3 ratio estimator, 10x10 tile | owns (D2) |
| `SH/Effects/ScreenSpaceReflections.comp` | 130 | Checkerboard trace (Medium) | one call site (D3); its march untouched |
| `SH/Include/ScreenReflections.glsl` | 93 | `ScreenRay`, `BeginScreenRay`, `GetHitConfidence` (both traces) | owns (D3) |
| `SH/Include/ReflectionProbes.glsl` | 125 | Probe list, box projection, two-probe blend | owns (D6 if the cause is here) |
| `SH/Include/PBR.glsl` | 179 | GGX, `SurfaceData`, `EvaluateLightWeighted`, clear coat | shares: F0 (D8a), the source lobe of the sun and of sized lights (D7a, D7c) |
| `SH/Scene/Forward.frag` | 452 | Forward shading; `LightSurface` 98-112, head 137-160, sun 164-179, loop 181-209, indirect 214-300, glass 333-350 | head (D8a), `LightSurface` and the sun term (D7a), the loop's `LightSurface` call (D7c), the specular gather after S0.11 (D5) |
| `SH/Include/Scene.glsl` | 396 | Frame constants, `MaterialData` (`IOR` :290, `Absorption` :295) | comments of `ReflectionParams.w` and `Absorption.w` |
| `SH/IBL/AtmosphereSkyView.comp`, `AtmosphereMultipleScattering.comp`, `AtmosphereAerial.comp`, `AtmosphereCube.comp` | 54, 83, 66, 25 | The atmosphere's per-lux tables (RGBA16F) | storage scale (D8b) |
| `SH/Include/AtmospherePass.glsl`, `Include/Atmosphere.glsl` | 88, 154 | Table bindings, `GetMultipleScattering` (:58-62), table mappings | storage scale (D8b) |
| `SH/Include/Lights.glsl`, `SH/Scene/LightCompact.comp` | 37, 108 | The light record's GPU mirror (13-20); the GPU-driven path's `LightData` (:93) | `Source` lane (D7c) |
| `SH/IBL/EquirectToCube.comp`, `Prefilter.comp`, `ProbeIrradiance.comp` | 34, 61, 93 | Probe conversion | unchanged (sizes come from parameters) |
| `SH/Scene/Skybox.frag` | 52 | Background; the atmosphere's sun disk (21-35) | unchanged (D6's flag zeroes the disk's constants) |
| `R/SceneRendererScreenReflections.cpp` | 345 | SSR resources, layouts, dispatches | owns |
| `R/SceneRendererReflectionProbes.cpp` | 394 | Probe gather, slots, conversions | owns (D10) |
| `R/SceneRendererAtmosphere.cpp` | 474 | Atmosphere tables and environment (`UpdateAtmosphere` 195-360) | shares (D8b) |
| `R/SceneRenderer.cpp` / `.h` | 4,125 / 2,771 | `ForBake` 228-246, `UpdateEnvironment` 866-986 (atmosphere intensity :921), `SetQuality` 1856-1907, `CollectLights` 2486-2527 (the CPU light path's `LightData` :2579), reflection flags 2904-2991, `ReflectionParams` :3019, reflections stage 3346-3356; `c_ReflectionProbeConversionsPerFrame` (`.h:925`), probe state (`.h:1330-1347`) | shares |
| `R/SceneRenderData.h` | 665 | `FrameConstants` (`ReflectionParams` :101), `MaterialData` (:191-221), `LightData` (:223-230), `LightRecord` (:235-248), probe constants (:269-299), `SpatialReflectionParameters` (:423-433) | shares |
| `R/RendererQuality.h` / `.cpp` | 267 / 895 | Rows (`.cpp:35-47`), traits (:121-141), `GetPreset` (:404-437), `Clamp` (:439-477), section (:645-733), read/write (:735-799); `ScreenReflectionSteps` (`.h:100-106`) | adds a row (D10), `MaxCandidates` (D4) |
| `R/MaterialData.cpp` / `.h` | 87 / 50 | `MaterialGPU::Build`: `IOR` :77, `Absorption` :83 | F0 lane (D8a) |
| `R/RendererComponents.cpp` | 707 | `ShadowSoftness` (:280-282), its inactive reason (:296-297); `PointLight` (299-314) and `SpotLight` (316-333) registrations | D7a; `SourceRadius` (D7c) |
| `R/RendererComponents.h` | 388 | `PointLightComponent` (50-61), `SpotLightComponent` (64-76) | `SourceRadius` (D7c) |
| `R/RenderLights.h` / `.cpp` | 126 / 379 | `MakeRecord` (`.h:42`, `.cpp:55-82`), `ComputeRecord` (`.cpp:149-186`, its calls 175-176 and 183) | the source radius into the record (D7c) |
| `R/Atmosphere.h` | 72 | CPU mirror of the atmosphere | storage scale constant (D8b) |
| `E/EditorLightingCommands.cpp` | 1,054 | `lighting.bakeProbes`: `RenderNextProbe` 184-220, renderer :941 | the bake's options (D6) |
| `E/ProbeBake.h` / `.cpp` | 51 / 147 | Faces 256, capture 1024x512 (`.h:25-27`) | comment only (D10: no re-bake) |
| `E/ThumbnailCache.h` | 245 | `c_Version` 10 (:108) | + 1 in R2 |
| `T/Renderer/GPUSceneSSRTests.cpp` | 681 | `GPU.SceneRenderer.SSR` | adds cases |
| `T/Renderer/GPUReflectionProbeTests.cpp` | 621 | `GPU.SceneRenderer.ReflectionProbes` (spread case 482-518) | adds and adapts |
| `T/Renderer/GPUSceneLightingTests.cpp` | 807 | Furnace (698-806) | adds cases |
| `T/Renderer/GPUSceneSpecularTests.cpp` | 400 | Specular anti-aliasing (167), the sky's 512 case (317) | highlight cases (step 7) |
| `T/Renderer/RenderSceneTests.cpp` | 1,362 | `Renderer.RenderScene`, light records (about 1140-1185) | the record's `Source` lane |
| `StrataTests/FeatureTest/Assets/Scenes/Feature.stscene`, `FeatureTest.stproj` | 5,602, - | `Lamp` (PointLight, 189-200), `Spot` (228-241); `Renderer.Quality` Custom (`.stproj` 150-173) | `SourceRadius` values; the new row's Custom value |
| `T/Renderer/GPUAtmosphereTests.cpp` | 560 | `GPU.Atmosphere` | adds a case |
| `T/Renderer/RendererQualityTests.cpp` | 775 | `Renderer.Quality` | the row's columns |
| `T/Renderer/GPUSceneRendererGPUDrivenTests.cpp` | 1,314 | Builders 529-540 | one builder |
| `T/Perf/RendererPerfTests.cpp`, `StrataTests/Perf/Budgets.json` | 1,098, 250 | `SSRSpatial4K` case (969-1018), entries 227-229 | new metrics |
| `T/Editor/GPUEditorLightingTests.cpp` | 795 | `GPU.Editor.Lighting` | one case (D6) |
| `Tools/LookDev.py`, `Tools/LookDev/LookDevScene.json`, `Shots.json` | 1,013, 90 steps, 70 | Look-dev harness | metrics, a scene, shots (appended) |

### 2.2 The Spatial trace and resolve today

- `ScreenSpaceReflectionsSpatial.comp:63-131`: one ray per pixel at full resolution, `BeginScreenRay`
  (`ScreenReflections.glsl:38-81`) cuts it where it leaves the screen, passes the far plane or nears the near plane. The march
  takes `Steps` (Spatial 32, SpatialHigh 48; `RendererQuality.cpp:185-187`) steps at `along = max(fraction^2, 1/pixels)`, each
  pixel offset by a fixed 4x4 ordered pattern (`c_PatternOrder`, :51, phase `pixel & 3`, :79-80).
- **The hit test** (:95-101): a hit where the ray is behind the depth by more than 0 and less than
  `thickness = |r_k - r_{k-1}| + max(5 cm, 2% of the ray's distance)`, the first such step wins and the march stops (:100). The
  bisection (:108-117) refines between the previous step and that one, and never checks that the ray reached the surface.
  So thin posts smear sideways as ghost columns (the step term accepts rays that passed far behind them), and whether a ray
  near a reflected edge hits depends on its offset, a hatch along every reflected edge (`<spec>/captures/crops/grazing_bands.png`,
  `crate_ao_on_wet.png`, `<show>/captures/p06_crop_ssr_edge.png`, `p10_crop_ssr_edge_ultra.png`).
- **The resolve** (`ReflectionResolve.comp`): groups of 8x8 with a 10x10 tile (:24-25), per pixel a ratio estimator over 3x3:
  the center's GGX lobe toward each neighbor's mirror direction over the neighbor's peak, at most 1 (:94-98), times
  `exp(-50 |z_i - z_c| / z_c)` (:92) and `pow(n_i . n_c, 8)` on the shading normals (:93). A 3-tap box leaves a third of a
  period-4 pattern (response 1/3 at a quarter and at half a cycle per pixel): the screen door (specular audit bug 1). On a
  grazing sheet the depth weight is not uniform either: neighbors along the receding axis differ in depth by about 0.25% per
  pixel at 9 degrees, `exp(-50 x 0.0025) = 0.88`, so no kernel would cancel the pattern exactly with today's weights.
- **Confidence** (`ScreenReflections.glsl:85-91`): `facing = saturate(-4 n_hit . d)` rejects surfaces met from behind (the
  depth buffer holds front faces: crescents, bug 4), and `edgeFade = saturate(min(edges) / 0.1)` fades within 10% of every
  side (bug 3), also for rays going up the screen that never leave it sideways.
- `SceneRendererScreenReflections.cpp:294-321`: one trace dispatch, one resolve dispatch, both 8x8 groups over the viewport.
  The resolve's layout (:177-186) binds depth (22), surface (23), traced (25); not the geometric normals (26).

### 2.3 Checkerboard (Medium)

`ScreenSpaceReflections.comp`: half resolution, a pixel of each 2x2 block per frame, 24 + 6 steps, noise turned each frame
(`TemporalParams.z`, :72), the TAA's history as color, only with TAA (`SceneRenderer.cpp:2987`). It shares `BeginScreenRay`
and `GetHitConfidence` (:116). This stream changes its confidence's edge fade (D3, a few instructions per hit pixel) and the
forward pass's use of every reflection (D5); its march, its cost and its need for TAA stay.

### 2.4 The forward pass's specular gather today (before S0.11)

`Forward.frag:227-300`: the reflected vector (:227), probes (`EvaluateReflectionProbes`, :235-236), the SSR texture at SRV 18
divided by the pre-exposure (:240-246), Lagarde's specular occlusion from `diffuseOcclusion` (AO) times the horizon fade
(:265-266), then three branches: with probes `radiance = radiance (1 - a) + rgb` (:273-274) and `radiance * specularColor *
specularOcclusion` (:280); without probes but with SSR `specular = env radiance (1 - a) + rgb` then `* specularColor *
specularOcclusion` (:287-288); without either, the sum times `EnvironmentParams.x` (:293). So AO darkens what SSR found (bug 5;
`<spec>/captures/crops/crate_ao_on_wet.png` against `crate_noao_on_wet.png`). S0.11 moves this into `GatherIndirectSpecular`
with `IndirectInputs` (`specularColor`, the reflected direction, occlusions, `probes`, `screenReflection`, `sky`,
`environment`) and a compose step that keeps each branch's scale, pixel-equal.

### 2.5 Probes today

- 128 texels per face (`SceneRenderData.h:273`), six levels (:274), RGBA16F, the array allocated whole for
  `min(MaxReflectionProbes, 16)` slots with the first probe in view (`SceneRendererReflectionProbes.cpp:114-170`) plus a scratch
  cube with its whole chain: 17.0 MiB per renderer that shows a probe.
- A conversion (`ConvertReflectionProbes`, :339-392): `EquirectToCube` into the scratch's level 0 (the capture's level by texel
  density, `EquirectToCube.comp:29`), `CubeDownsample` down the chain, `Prefilter` into the slot's six levels with 256 samples
  each (:37, :361-368), `ProbeIrradiance` from the 32-texel level (:35, :369-387). With `SpreadLightingUpdates`, at most
  `c_ReflectionProbeConversionsPerFrame` = 2 per frame (`SceneRenderer.h:925`, `.cpp` :236); a probe waiting lights nothing and
  its box falls to the sky (:288-298).
- Box projection (`ReflectionProbes.glsl:70-82`): the reflected ray meets the box's far faces, and the lookup goes from the box's
  center (the entity's position, where `lighting.bakeProbes` captured: `EditorLightingCommands.cpp:938`) to that point. Content
  inside the box is projected onto its faces.
- The bake (`E/EditorLightingCommands.cpp:184-220`) renders six 256-texel faces with `ForBake()` (`SceneRenderer.cpp:228-246`:
  Medium, no TAA) and `HDROutput`; the atmosphere's background draws the sun's disk (`Skybox.frag:21-35`, about 1e9 nits in the
  capture), which `EquirectToCube.comp:32` clamps to 65,000 nits at conversion. The atmosphere's environment cube has no disk
  (`AtmosphereCube.comp:8-9`).

### 2.6 F0, the sun and the night sky today

- F0 is 0.04 for every dielectric: `Forward.frag:150`, `:346` (glass), `PBR.glsl:111`. `MaterialProperties::IOR` reaches only
  refraction (`Forward.frag:335`). `MaterialGPU::Build` writes `IOR = max(IOR, 1)` (`MaterialData.cpp:77`) and
  `Absorption = (absorption, 0)` (:83): `Absorption.w` is unused (`Scene.glsl:295`). The glTF importer already reads
  `KHR_materials_ior` into `IOR` for every material (`GltfImporter.cpp:891-903`, 1 to 3).
- The sun's specular is a point light's: `PBR.glsl:109-114` with roughness clamped at `c_MinRoughness` 0.045 (:10). The light's
  angular diameter is `DirectionalLightComponent::ShadowSoftness` (`RendererComponents.h:27`, the sun 0.53, Overcast 10:
  `SkyPresets.cpp:46`), read into `m_ShadowLight.Softness` for every directional light (`SceneRenderer.cpp:2503`) but used only
  by the shadow filter (`SceneRendererExtraction.cpp:1189-1191`). Its property carries a "needs shadows" inactive reason
  (`RendererComponents.cpp:296-297`). A GGX lobe falls to half its peak at a reflected angle of about `1.287 alpha` (half-vector
  `tan^2 = (sqrt(2) - 1) alpha^2`, doubled): at `alpha = 0.045^2` that is 0.15 degrees, about half the real sun's 0.265-degree
  radius (the specular audit's "+/-0.23 degrees" used another width, unstated; the half maximum is the measure D7a and its test
  use); a 10-degree sun is drawn at the same pin dot (`<spec>/captures/crops/overcast_ladder.png`). The shadow filter's own
  `ShadowParams.x = tan(half angle)` (`SceneRendererExtraction.cpp:1189-1191`) is written only when the sun casts shadows, so the
  highlight cannot read it.
- Point and spot lights are points: their components have no size (`RendererComponents.h:50-76`), and every light reaches the
  forward pass through its record (`LightRecord`, 80 bytes, `SceneRenderData.h:235-248`), whose lanes are all used
  (`SpotAngles.z` drawn, `.w` shadowed: `RenderLights.cpp:78`, 164), and then through `LightData` (64 bytes), whose
  `SpotAngles.w` is 0 on both light paths (the CPU path `SceneRenderer.cpp:2579`, the GPU-driven path `LightCompact.comp:93`).
  The forward pass's light loop (`Forward.frag:186-208`) is the only reader of `u_Lights`.
- The night sky is posterized at -10 degrees: steps of 8-17 codes per band (`<spec>/captures/crops/night_sky_band.png`), wavy
  along the sky-view table's texels. The atmosphere's tables hold the sky "per lux of sun" in RGBA16F
  (`SceneRendererAtmosphere.cpp:24`), readers multiply by the sun's illuminance (`SceneRenderer.cpp:921`, `Skybox.frag:25`,
  `Fog.glsl:42`, `AerialPerspective.glsl:26`). At night per-lux values fall below half floats' smallest normal (2^-14, 6.1e-5)
  into subnormals, whose step is fixed at 2^-24: a value of 1e-7 has two levels. The hypothesis is the audit's, unverified.

### 2.7 What the audits measured (the numbers to beat)

| Measure | 0.7.0 | Source |
| --- | --- | --- |
| Noise, sheet roughness 0.18 at 9 degrees, over post and wall / over spheres | High: none 5.0/3.9%, FXAA 4.6/3.5%, TAA 4.2/3.9%; Ultra none 4.8/4.0%; Medium TAA 6.4/5.9%; probe alone 0.3% | specular audit 4 |
| `GPU.Reflections` at 4K (rough, loaded GPU) | High 0.30-0.92 ms (outliers 1.8), Ultra 0.44-1.56, Medium 0.06-0.15; a wet frame 2.03 ms (`p18`) | specular audit 7, showcase audit 3 |
| First probe conversion at 128 | `GPU.Reflections` about 3.6 ms in that frame (one reading, SSR included) | specular audit 5 |
| Flicker in motion | not measured | |

The audit's noise metric was computed by hand (no script was kept in `<spec>/scripts`); D1 defines it exactly so that the base
build's numbers are measured again by the same code.

### 2.8 What step 0 gives this stream

S0.10: every shader in Slang (or GLSL if the box ran out). S0.11: `IndirectInputs`, `GatherIndirectDiffuse`,
`GatherIndirectSpecular` and the compose step, pixel-equal; `ReflectionParams.y/.z` for GI, **`.w` left to this stream**; the
look-dev baseline taken by lane Slang's last act. S0.2: contact shadows off at every preset (the lab's numbers do not change:
contact shadows do not touch reflections). S0.12's tip is this stream's base.

## 3. Order, the two landings and the cut line

| Step | Part | What | Size | Lands |
| --- | --- | --- | --- | --- |
| 1 | **R1** | Look-dev shots, the noise, pattern and band metrics, the strafe flicker test | S | R1 |
| 2 | **R1** | The resolve: two candidates prototyped on the sheet, one chosen; plane-aware weights | M | R1 |
| 3 | **R1** | Direction-aware edge fade (both traces) | S | R1 |
| 4 | **R1** | Crossing test with hit validation, the march continuing on a false hit | M | R1 |
| 5 | **R1** | Specular occlusion off the SSR term | S | R1 |
| 6 | **R1** | The misplaced probe patch: cause found and fixed; bakes draw no sun disk | S | R1 |
| R1 landing | | Review, Debug and Release ctest, CI on `ci/wave8-reflections`, A/B, docs of steps 1-6 (and of step 7 if it joins), ThirdPerson's pictures | | **alone, before showcase step 5** |
| 7 | **R2, first** | Highlights: (a) the sun's size in specular; (b) lantern and fire highlights judged; (c) a source radius for highlights only on point and spot lights. Each judged with no AA, FXAA and TAA, in motion | S, S, S-M | R1's landing if verified by then, else landing 1 or 2 |
| 8 | R2 | (a) IOR to F0 and the furnace; (b) the night sky's bands | S, S | landing 1 or 2 |
| 9 | R2 | Back-facing hits: filled silhouette or crescents, by looking | S | landing 1 or 2 |
| 10 | R2 | `ReflectionProbeResolution` (128, 128, 256, 256), memory, conversion measured and spread | M | landing 1 or 2 |
| 11 | R2 | Adoption and docs | S | landing 1 or 2 |

**R1 is never cut, and lands whole if it can.** If step 4 runs more than one agent-day over, R1 lands steps 1-3, 5 and 6
(the screen door, the wedges, the smudges and the patch are the largest changes to the image the showcase tunes against) and
step 4 moves to R2, directly after step 7 (highlights keep the owner's first place); the showcase's shot 4 check (plan 4.2
graybox, row 4) then reads the comb as still open.

**Step 7 starts as soon as R1 is in review** (decisions.md), on its own branch `agent/reflections-highlights` from the commit
under review, in the same worktree and build directory (`git switch`; incremental rebuilds), while R1's findings are fixed on
`agent/reflections`. When R1 is verified: if step 7 is verified too (its tests, Debug ctest with every GPU suite under the
validation layer, the feature test, a review of its commits), it is rebased onto R1's tip, verified again and lands with R1;
otherwise R1 lands alone and step 7 rebases onto main afterwards and leads R2. Two branches keep R1's landing commit free of
half-done highlight work.

**R2's order of value** if landing 1 arrives before R2 is complete (each step lands working, tested and documented; what is not
verified joins landing 2): 7 (highlights: 7a, 7c, then 7b's judgment), 8b (shot 6 needs the sky), 8a (water's F0, shot 4), 10
(shot 3's tank), 9, with 11 always last.

**Cut line** (plan 4.3), below it in this order: 12 keyframed captures (L); 13 the rest of sized point and spot lights (their
diffuse light and shadows) stays in "Later", step 7c gives their highlights their size; 14 hit-distance blur, a wet-surface
model, normal-map-aware filtering, anisotropy, SSR on clear coat (M each).

## 4. Design

### D1 Shots and metrics (step 1, R1)

What nobody measured yet: the noise by a fixed formula on fixed shots, the screen-locked pattern apart from content, and
flicker in motion. All of it lands before any fix, and the base build is measured with it.

**The look-dev scene `Scenes/Specular.stscene`.** Appended to `Tools/LookDev/LookDevScene.json` before its
`project.setStartScene` step (existing scenes and shots unchanged). It rebuilds the specular lab (`<spec>/scripts/build_scene.py`)
with the look-dev's conventions (built-in meshes by handle: cube `...01`, sphere `...02`, plane `...03`, cylinder `...05`; every
look property at its component's default except where named):

- Materials (`material.create`): `SpecSheet` (base 0.91, 0.92, 0.92, metallic 1, roughness 0.18), `SpecPolished` (0.95, metal,
  0.03), `SpecTank` (0.75, 0.75, 0.76, metal, 0.12), `SpecTankRough` (same, 0.3), `SpecWet` (0.05, 0.05, 0.052, dielectric, 0.06),
  `SpecBrick` (0.45, 0.2, 0.12; 0.85), `SpecTeal` (0.05, 0.4, 0.4; 0.7), `SpecYellow` (0.7, 0.55, 0.08; 0.7), `SpecCrate` (0.4,
  0.28, 0.16; 0.75), `SpecPostWhite` (0.85; 0.5), `SpecPostRed` (0.8, 0.08, 0.05; 0.5), `SpecBulb` (black, emissive 1, 0.75, 0.45
  at 3000, unlit), `SpecLadder0`-`7` (0.95 metal, roughness 0, 0.05, 0.1, 0.2, 0.3, 0.5, 0.7, 1), `SpecIor0`-`3` (0.02, dielectric,
  roughness 0.1, IOR 1, 1.33, 1.5, 3).
- `scene.new {name: "Specular"}`; `Main Camera` (0, 7, 34), rotation (-6.5, 0, 0), fov 60, primary; `Sky` (SkyLight Atmosphere);
  `Sun` (DirectionalLight, CastShadows, ShadowDistance 200); `Post Process` (`ScreenSpaceReflections` true); then
  `lighting.applySkyPreset {preset: "Clear", sunElevation: 30, sunAzimuth: 75}`.
- `Ground` plane at the origin scale (400, 1, 400), `Materials/Ground.stmat`; `Wet` plane (-14, 0.01, 14) scale (24, 1, 16),
  `SpecWet`, `CastShadows` false; `Sheet` cube (8, 1.7, -6) scale (12, 3, 2.5) `SpecSheet`; `Box` cube (-8, 1.7, -6) scale (12, 3,
  2.5) `SpecPostWhite` (its top edge reflected in the wet plane is the comb's edge); `Polished` cube (0, 1.6, -14) scale (8, 3,
  0.1); `Tank` cylinder (-18, 6, -30) scale 12 `SpecTank`; `Tank Rough` cylinder (18, 6, -30) scale 12; `Ladder 0`-`7` spheres at
  (-7 + 2i, 0.5, 1); `Ior 0`-`3` spheres at (-3 + 2i, 0.5, 3); `Back Wall` cube (0, 4, 45) scale (120, 8, 1) `SpecBrick`; `East
  Block` (45, 7, 0) scale (2, 14, 60) `SpecTeal`; `West Block` (-45, 7, 0) scale (2, 14, 60) `SpecYellow`; six posts, cubes of
  scale (0.12, 3, 0.12) at y 1.5, z 9, x -20 (white), -14 (red), -8 (white), 8 (white), 14 (red), 20 (white); four crates, unit
  cubes at y 0.5 at (-17, 13), (-11, 11), (12, 11), (17, 14) `SpecCrate`; `Lantern` sphere (10, 1.2, 8) scale 0.12 `SpecBulb`
  without shadows, with a PointLight (color 1, 0.72, 0.42; 60 cd; range 18; CastShadows); `Lantern 2` (-10, 2.5, 8) alike without
  CastShadows.
- `Probe Yard` (0, 2, 14), ReflectionProbe (Extents 46, 20, 32; BlendDistance 2; BoxProjection), bound with `"bind"`, baked with
  `lighting.bakeProbes {entities: [{"$ref": ...}]}`, then `entity.setActive false` (shots activate it in `Setup`). The command
  script runs in a headless editor with a GPU (`LookDev.py:378-385`: `--headless` without `--no-gpu`); if the bake fails there
  (unverified), the probe's shots bake it in their `Setup` instead.
- `scene.saveAs {path: "Scenes/Specular.stscene"}`.

**Shots** appended to `Shots.json` (cameras from the lab's `shots.py`; never changed after they land):

| Shot | Camera (position, target, fov) | Setup | AA | Crops and metrics | Part |
| --- | --- | --- | --- | --- | --- |
| `lookdev.sheet` | (16, 1.6, -3.6), (0, 1.5, -6), 50 | | all | `post-wall` [0.61, 0.28, 0.73, 0.52], `spheres` [0.56, 0.52, 0.73, 0.65]: Noise | R1 |
| `lookdev.sheet.angle` | (-2, 1.6, 6), (8, 1.7, -4.75), 55 | | off, FXAA | `crescents` (spheres in front of the sheet): Noise | R1 |
| `lookdev.wet` | (-12, 0.5, 26), (-13, 0.5, 8), 45 | | all | `edge` (the box's reflected top edge): Noise | R1 |
| `lookdev.wet.sunward` | (-14, 1.6, 2), (-14, 0.3, 30), 55 | Clear, 12 degrees, azimuth 90 | off, FXAA | `left`, `right` (the sides, where wedges were) | R1 |
| `lookdev.tank` | (-4, 1.7, -18), (-14, 5, -30), 55 | | off | `streak`: Noise | R1 |
| `lookdev.patch` | set in step 6 when reproduced | probe active, GoldenHour 9 degrees, azimuth 45 | off | `patch` | R1 (step 6) |
| `lookdev.ior` | (0, 1, 6), (0, 0.5, 3), 40 | | off | `spheres` | R2 |
| `lookdev.overcast` | (0, 1.4, 7), (0, 0.6, 1), 45 | Overcast | off | `ladder` | R2 |
| `lookdev.night` | (0, 1.6, 6), (20, 6, 40), 60 | Clear, -10 degrees, azimuth 90 | off | `sky` [0.05, 0.05, 0.95, 0.6]: Bands | R2 |
| `lookdev.lantern.wet`, `.lantern.sheet` | (-6, 0.7, 1), (-13, 0.6, 16), 50; (0, 1.4, 7), (8, 1.7, -4.75), 55 | Clear, -3 degrees | off, FXAA | `highlight` | R2 |
| `lookdev.lantern.wet.sized`, `.lantern.sheet.sized` | as the two above | as above, then `component.set` of `PointLight.SourceRadius` 0.06 on `Lantern` and `Lantern 2` (step 7c; a `Setup` step, so that the scene still builds with the base's and 0.7.0's editors) | off, FXAA, TAA | `highlight` | R2 |
| `lookdev.polished` | (0, 1.6, -5), (0, 1.6, -14), 60 | probe active | off | `center` | R2 |

The crops' rectangles are first guesses from the audit's captures (`<spec>/captures/small/a30_sheet_grazing_off.png`): before
the shot lands the agent captures it on the base build and moves a rectangle so that it covers only the sheet's (or the wet
plane's) reflection, at least 200x200 px at 3840x2160; after landing nothing moves. "all" is TAA, FXAA and none. Every shot
of this table carries `"Project": "LookDev"` and `"Scene": "Scenes/Specular.stscene"` (as `lookdev.furnace` names its scene;
`Shots.json`), its camera as `Position`, `Target`, `Fov`, its setup as `Setup` steps (`lighting.applySkyPreset {preset,
sunElevation, sunAzimuth}`, `entity.setActive`, `component.set`), and the harness reopens the scene before each shot
(`LookDev.py:739`), so a shot's setup never leaks into the next.

**Metrics in the harness** (`Tools/LookDev.py`, standard library). A shot may carry `"Metrics": [{"Name", "Kind", "Rect"}]`
(normalized rectangles). `postprocess` (:563) computes them on the first repeat's PNG and writes them into
`stats/<shot>-<W>x<H>-<aa>.json` under `"metrics"` and a `metrics.csv` (shot, size, aa, name, kind, then the values); `compare`
reports both sides' values and their differences in `compare.json` and the page. Definitions, on the 8-bit sRGB-encoded output
(the PNG as stored):

- `Y(x, y) = (0.2126 R + 0.7152 G + 0.0722 B) / 255`, the encoded luma in [0, 1].
- `M(x, y)`: the mean of `Y` over the 9x9 window centered on the pixel (a summed-area table). Interior pixels `I`: those whose
  window lies inside the rectangle.
- **Noise** `N = 100 * sqrt(mean over I of (Y - M)^2)`, in percent of full scale (the audit's "RMS of luma minus its 9x9 mean").
- **Pattern** `P_k` for k = 3 and 4: `m_phi = mean of (Y - M)` over the interior pixels with `(x mod k, y mod k) = phi`, and
  `P_k = 100 * sqrt(mean over the k^2 phases of m_phi^2)`. Content averages out over phases; a screen-locked pattern does not, so
  `P_4` measures today's screen door and `P_3` candidate B's (D2).
- **Bands**: for every fourth column of the rectangle and every pair of adjacent rows, `step = max over R, G, B of |C(x, y+1) -
  C(x, y)|` in codes; `maxStep` the largest, `stepsOver3` how many exceed 3 codes. A smooth sky at 4K changes by at most 1-2 codes
  per row.
- Values: `{"noise": N, "pattern3": P_3, "pattern4": P_4, "pixels": |I|}` and `{"maxStep", "stepsOver3", "columns"}`.
- `selftest` gains two synthetic images: 64x64 gray 128 plus a 4x4 ordered pattern of amplitude 8 codes (expects `P_4` within 5%
  of `N` and `P_3` under 0.2 of it), and a vertical ramp with one 10-code step (expects `maxStep` 10 and `stepsOver3` equal to
  the columns).

**The same metrics in C++ for GPU tests** (`T/Renderer/ReflectionMetrics.h/.cpp`, in namespace `Strata::Tests`):

```cpp
// The look-dev harness's noise (Tools/LookDev.py "Noise") over a rectangle of an 8-bit image, in percent of full scale.
struct NoiseMeasure
{
	float Noise = 0.0f;    // RMS of the encoded luma minus its 9x9 mean
	float Pattern3 = 0.0f; // RMS over the 9 phases of (x mod 3, y mod 3) of the residual's mean
	float Pattern4 = 0.0f; // ...over the 16 phases of (x mod 4, y mod 4)
	uint32_t Pixels = 0;
};
NoiseMeasure MeasureNoise(const ReadbackImage& image, const glm::uvec4& rect); // rect: x0, y0, x1, y1 (pixels, exclusive end)
```

**Flicker: one meter for the wave, not two** (review correction). `radiance-cascades` designs the same measure in full
(`specs/radiance-cascades.md`, D11: `FlickerFrame`, `FlickerResult`, `MeasureFlicker`, `MeasureSequenceFlicker`, reprojection by
depth and normal, `Mean`, `P99`, `Blocks`, `BlocksP99`, `MeasuredShare`), and the plan's two acceptance rows ("over 32 px
blocks") must mean one thing. So this stream does not write a plane-only meter. The helper is
`T/Renderer/FlickerMetrics.h/.cpp` (namespace `Strata::Tests`), whose `FlickerFrame`, `FlickerResult`, `MeasureFlicker` and
`MeasureSequenceFlicker` are radiance-cascades' D11 definitions word for word (statistics as fractions; reports multiply by 100),
with its CPU cases as suite `Renderer.FlickerMetrics` ("A steady drift scores zero", "An alternating value scores its amplitude",
"Depth and normal edges are left out", "A block of half its pixels measured is left out", "The reprojection follows the
camera"). This stream adds one reader beside `ReadFlickerFrame` (the HDR target, which radiance-cascades measures):

```cpp
// A frame as the person sees it: Value is the Rec. 709 luminance of the 8-bit output (GetOutputTexture) decoded from sRGB to
// linear, after tone mapping and FXAA or TAA; depth and normals are the prepass's, as ReadFlickerFrame reads them.
FlickerFrame ReadDisplayFlickerFrame(SceneRenderer& renderer, const SceneCamera& camera);
```

Whichever of the two streams lands first creates the files with both readers; the other takes main's on merge and deletes
its copy (the orchestrator confirms the file name with both streams: openQuestions). The measured surface is the sheet's face or
the wet plane: reprojection by depth is exact there, so the plane form adds nothing.

**The strafe test** (`GPU.SceneRenderer.SSR`, "A strafe past a rough metal sheet and over a wet floor: the reflections do not
flicker"): a C++ builder `SpecularLab` in `ReflectionMetrics.cpp` with the sheet, the box, the three walls, the posts, the
crates, the ladder and the wet plane of the look-dev scene, an environment texture sky (`CreateEnvironmentTexture`, two tones)
and a sun (`AddSun`, test-scale illuminance, shadows), `AddNeutralPostProcess` with `ScreenSpaceReflections` true,
`ExactSpecification()` with the mode's `Reflections`. Viewport 1280x720. Two paths, each 3 still frames then 24 moving frames,
the camera's position and target moving together by a constant vector per frame: the sheet path from (16, 1.6, -3.6) / (0, 1.5,
-6) by (0, 0, -0.01) (toward the sheet, 1 cm a frame), the wet path from (-12, 0.5, 26) / (-13, 0.5, 8) by (0.01, 0, 0). Modes:
Spatial and SpatialHigh, each with no AA, FXAA and TAA (a fresh renderer per sequence; the TAA history continues within one:
1 cm is no camera cut). Every frame is read with `ReadDisplayFlickerFrame` and the sequence measured with
`MeasureSequenceFlicker`, restricted to the measured surface's pixels (the sheet's face, center (8, 1.7, -4.75), normal +Z, half
axes (6, 0, 0) and (0, 1.5, 0), or the wet plane: a mask from the CPU's ray-rectangle test, 8 px inside its outline, applied by
setting `ViewDepth` to 0 outside it, which the shared definition already leaves unmeasured, so its interface does not change). Both
`Blocks` and `Mean` are reported: blocks catch regions that pump, but a screen-locked pattern sliding under moving content
averages out within a 32 px block, so the per-pixel `Mean` is what shows SSR's crawl. Step 1 asserts what holds today: with
`ScreenSpaceReflections` false both are finite and `Blocks` is at most 0.3%; it reports every mode's four numbers with `MESSAGE`,
SSR on and off. Step 4 adds the bounds (section 5). Step 7 adds a third path for highlights: the camera at (-14, 0.6, 2) looking
toward (-14, 0.3, 30) over the wet plane into a sun at 12 degrees (the glint column), moving by (0.01, 0, 0) a frame, measured
on the wet plane, once with the sun's `ShadowSoftness` 0.53 and once at 0 (the point lobe); and the same over the wet plane at
dusk (the builder's sun off, its sky at 1% of its intensity) lit by the look-dev scene's two lanterns, which the builder gains
for step 7 (point lights at (10, 1.2, 8) and (-10, 2.5, 8), 60 cd, range 18, color (1, 0.72, 0.42)), with `SourceRadius` 0.06
against 0.

**Recorded in step 1's commit:** `N`, `P_3`, `P_4` of `lookdev.sheet` (both crops), `lookdev.wet` (`edge`) and `lookdev.tank`
at 3840x2160, High and Ultra, every AA mode, on the base build (S0.12's tip) and on 0.7.0 (`.worktrees/wave8-base`), against the
audit's 5.0/3.9% (High, none): if the formula reproduces them within a point, it is the audit's; if not, the commit says by how
much and the targets keep their meaning (under 1%). And the strafe's `Blocks` and `Mean` per mode on the base build, SSR on and
off (step 4's bounds are written from them).

### D2 The resolve (step 2, R1): two candidates, prototyped on the sheet

**Common to both candidates: weights that are uniform on a plane.** A kernel cancels a fixed pattern exactly only with uniform
weights over the window, and today's depth weight is not uniform on a grazing surface (2.2). So:

- **Plane distance instead of depth difference.** Each tile entry keeps its view-space position (from its pixel's UV and linear
  depth: `ViewPosition(uv, z)` with the inverse projection's diagonal and the jitter offset, as `BeginScreenRay` reconstructs it)
  and its **geometric** normal from the prepass (`u_Normals`, SRV 26, added to the resolve's layout and set:
  `SceneRendererScreenReflections.cpp:177-186`, :264-272). `planeWeight = exp(-c_PlaneSharpness * |dot(P_i - P_c, Ng_c)| / z_c)`,
  `c_PlaneSharpness = 50` (AO's sharpness, on the distance to the center's tangent plane). Coplanar neighbors weigh 1 whatever
  the viewing angle; a step in depth still weighs 0.
- **Normal weight on geometric normals**: `pow(max(dot(Ng_i, Ng_c), 0), 8)`, so normal maps do not make it vary across a sheet.
- **The lobe weight stays** (shading normals and mirror directions, :94-98): it is what keeps smooth surfaces sharp.
- Shared memory per tile entry, five arrays: traced (float4), shading normal and alpha^2 (float4), mirror direction and linear
  depth (float4, as today), view-space position (float4, w unused) and geometric normal (float4, w unused): 80 bytes, 8.0 KB
  for today's 10x10 tile and 11.5 KB for candidate A's 12x12 (review correction: the plane weight needs every tap's position,
  which the four arrays first listed here had no room for).

**Candidate A: a 5x5 phase-cancelling kernel, today's period-4 march.**

- Kernel `k = (0.5, 1, 1, 1, 0.5)` separable: tap (x, y) weighs `k[x+2] * k[y+2] * lobe * plane * normal`; the center's weight
  stays 1. Over x-2..x+2 the 4 phases of `x mod 4` each get weight 1 (the two ends share one), so with uniform weights every
  4-periodic pattern cancels exactly in both axes (its response is 0 at a quarter and at half a cycle).
- **A1, one pass:** groups 8x8, tile 12x12 (`c_Tile = c_Group + 4`, origin `- 2`), 144 entries, 11.5 KB of shared memory, 25 taps.
- **A2, separable, only if A1 misses the cost target:** two dispatches, horizontal (tile 12x8) then vertical (tile 8x12), 5
  taps each with the center's bilateral weights; the trace writes the texture the forward pass reads (`m_ScreenReflectionTexture`),
  the horizontal pass writes the other (`m_TracedReflectionTexture`) and the vertical pass writes back into the first. No new
  memory. It cancels exactly where the weights are uniform, as A1 does.
- It softens mirror reflections: the 5x5 tent averages about 2 px at 4K on smooth surfaces, where the lobe weight stays near 1
  for coplanar neighbors (0.97-0.99 at roughness 0.045-0.06 at 4K: their mirror directions differ by about a pixel's angle,
  0.00048 rad at 60 degrees over 2160 rows).

**Candidate B: a period-3 march, today's 3x3 kernel.**

- `c_PatternOrder3 = { 0, 7, 3, 6, 5, 2, 4, 1, 8 }` (the 3x3 ordered-dither matrix, row by row), phase `(x mod 3, y mod 3)`,
  `offset = (c_PatternOrder3[(y % 3) * 3 + x % 3] + 0.5) / 9`. The 3x3 window holds each phase once, so with uniform weights a
  3-periodic pattern cancels exactly (`1 + 2 cos(2 pi / 3) = 0`), at 9 offsets per step instead of 16.
- Groups 8x8, tile 10x10 as today, 9 taps: the resolve's cost grows only by the plane weight's few instructions and the two
  new shared arrays.
- The coarser offsets sample each step at 9 places instead of 16; the crossing test and validation (D4) find a hit wherever
  the ray crosses, so this matters less once D4 lands.

**How the choice is made.** Both are built in turn on the branch (a shader-only rebuild each), never landed together:

1. Capture `lookdev.sheet`, `lookdev.wet` and `lookdev.tank` at 3840x2160, High and Ultra, every AA mode, with
   `Tools/LookDev.py capture --shots "lookdev.*"`, once per candidate, and read `N`, `P_3`, `P_4`.
2. Run the strafe test for each (its `MESSAGE` values).
3. A/B `GPU.Reflections` per candidate against the base build, interleaved under PerfLock (section 6.3), on `lookdev.sheet` and
   `tp.scene` at 3840x2160 High.
4. **Rule:** a candidate with `N` above 1% on either sheet crop at High or Ultra in any AA mode is out. If both pass, **B**
   (sharper, no new taps), unless A's `N` is at least 0.3 points lower on the sheet at High without AA and its stage stays within
   the 1.2 ms target. If only one passes, that one. If neither passes, the lower `N`, and step 4 is expected to close the gap (the
   noise holds hit-or-miss toggles that D4 removes): measured again after step 4, and if it is still above 1% the commit says by
   how much and why.
5. The commit states the six readings per candidate, the choice and the reason; DG "Reflections" names the kernel and why.

### D3 Direction-aware edge fade (step 3, R1)

A hit near the screen's edge is doubtful only when the ray would have gone on beyond the edge: what it would have met there is
unknown. So the fade follows the ray's remaining length to its exit, not the hit's distance to the nearest side.

```hlsl
struct ScreenRay
{
	float3 Start;
	float3 Delta;
	float Pixels;
	float3 Direction;
	bool LeavesScreen; // Its end is where it leaves the screen (not the far plane, not the near plane)
};

// In BeginScreenRay (ScreenReflections.glsl:66-76): the screen's cut and the far plane's cut apart.
float screenExtent = 1.0;
for (int axis = 0; axis < 2; axis++) { /* today's lines, into screenExtent */ }
float depthExtent = delta.z < 0.0 ? -start.z / delta.z : 1.0;
float extent = min(screenExtent, depthExtent);
ray.LeavesScreen = screenExtent < 1.0 && screenExtent <= depthExtent;
delta *= max(extent, 0.0);

// How much a hit is believed: front faces only, and a fade over the last tenth of the screen's smaller side the ray has left
// before it would leave the screen.
float GetHitConfidence(float2 hitUV, int2 hitPixel, ScreenRay ray)
{
	float facing = saturate(-4.0 * dot(OctahedralDecode(u_Normals.Load(int3(hitPixel, 0)).rg), ray.Direction));
	float edgeFade = 1.0;
	if (ray.LeavesScreen)
	{
		float remaining = length((ray.Start.xy + ray.Delta.xy - hitUV) * u_Frame.ViewportSize.xy); // Pixels to the exit
		edgeFade = saturate(remaining / (c_EdgeFade * min(u_Frame.ViewportSize.x, u_Frame.ViewportSize.y)));
	}
	return facing * edgeFade;
}
```

`c_EdgeFade` stays 0.1 with this meaning (216 px at 2160 rows). Both traces pass `ray` (Spatial :129, Checkerboard :116): Medium's
fade improves at a cost of a `length` and a division per hit pixel. A ray going up the screen and hitting a wall near the left
side no longer fades; one leaving through the top fades over its last 216 px; the sky wedges of `n10b_lantern_wetfull_off.png`
and `a30_wetfull_sunward_off.png` go. The hard vertical cut in `a30_sheet_grazing_off.png` (about 0.73 of the width), where rays
begin to leave through the left side before reaching the wall, becomes a gradual fade into the probe's or the sky's reflection.

### D4 Crossing test with hit validation (step 4, R1)

The fixed steps stay: a purely hierarchical march ran out of steps on rays grazing a floor (DG "Reflections",
`GPU.SceneRenderer.SSR`'s wall case). What changes is what counts as a hit.

- **A candidate** is a step whose segment from the previous point crossed the depth of the surface at either end's pixel: the
  ray was in front of that depth at the previous point and behind it at this one. Checking both ends finds the crossing that
  happened between two samples on the previous pixel's surface (the top edge of a reflected wall: the ray reaches the wall's
  depth while still over the wall, and the next sample lands on the sky).
- **Refinement**: today's bisection between the two points, keeping each end's ray distance and scene distance.
- **Validation**: the refined "behind" end must be behind its own surface by no more than the thickness tolerance plus the last
  interval's change of the ray's distance, and the surface must be continuous across the crossing (the depths at the two ends
  differ by no more than the same). A ray that passed behind a thin post fails (the ends sit on the post and on the background
  20 m behind it); a ray that met a wall passes.
- **A false candidate does not end the march:** it continues from that step, up to `MaxCandidates` refined candidates per ray
  (Spatial 2, SpatialHigh 3); after that the ray finds nothing (the probe or the sky fills in). This bounds the cost in clutter.

```hlsl
static const float c_SkyDistance = 1.0e30; // The depth buffer's 0: nothing to cross

struct MarchPoint
{
	float Along;         // Position along the ray, 0 to 1
	float RayDistance;   // View distance of the ray's point
	float SceneDistance; // View distance of the depth at its pixel (c_SkyDistance for the sky)
};

float SceneDistanceAt(float3 point, int2 size)
{
	int2 pixel = clamp(int2(point.xy * u_Frame.ViewportSize.xy), int2(0, 0), size - 1);
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	return depth > 0.0 ? GetViewDistance(depth) : c_SkyDistance;
}

MarchPoint MarchPointAt(ScreenRay ray, float along, int2 size)
{
	float3 point = ray.Start + ray.Delta * along;
	MarchPoint result = { along, GetViewDistance(point.z), SceneDistanceAt(point, size) };
	return result;
}

// Bisects a candidate segment and validates the crossing: Along >= 0 for a hit, -1 for a false candidate.
float RefineCrossing(ScreenRay ray, MarchPoint low, MarchPoint high, int2 size)
{
	for (uint index = 0; index < u_Reflections.RefineSteps; index++)
	{
		MarchPoint middle = MarchPointAt(ray, 0.5 * (low.Along + high.Along), size);
		// Over the sky the ray is compared with the surface it was last in front of (review correction, below).
		if (middle.RayDistance > (middle.SceneDistance < c_SkyDistance ? middle.SceneDistance : low.SceneDistance))
			high = middle;
		else
			low = middle;
	}
	float slack = max(c_MinThickness, c_ThicknessScale * high.SceneDistance) + abs(high.RayDistance - low.RayDistance);
	bool valid = high.SceneDistance < c_SkyDistance
		&& high.RayDistance > high.SceneDistance                 // Behind its own surface (a crossing found)
		&& high.RayDistance - high.SceneDistance <= slack        // ...not far behind it
		&& abs(high.SceneDistance - low.SceneDistance) <= slack; // ...on one continuous surface
	return valid ? high.Along : -1.0;
}

// The march (replacing ScreenSpaceReflectionsSpatial.comp:76-117).
MarchPoint previous = { 0.0, GetViewDistance(ray.Start.z), c_SkyDistance }; // The start crosses no surface of its own
uint candidates = 0;
float hitAlong = -1.0;
for (uint index = 0; index < u_Reflections.Steps && hitAlong < 0.0; index++)
{
	float fraction = (float(index) + offset) / float(u_Reflections.Steps);
	MarchPoint current = MarchPointAt(ray, max(fraction * fraction, minStep), size);
	bool crossesCurrent = previous.RayDistance <= current.SceneDistance && current.RayDistance > current.SceneDistance;
	bool crossesPrevious = previous.RayDistance <= previous.SceneDistance && current.RayDistance > previous.SceneDistance;
	if (crossesCurrent || crossesPrevious)
	{
		hitAlong = RefineCrossing(ray, previous, current, size);
		if (hitAlong < 0.0 && ++candidates >= u_Reflections.MaxCandidates)
			break;
	}
	previous = current;
}
```

- **Why the bisection compares a sky pixel with the low end's surface** (review correction). The candidate this step exists for
  is the top edge of a reflected wall: `previous` lies on the wall in front of it, `current` beyond the edge on the sky. The
  crossing lies on the wall, between `previous` and the edge. If the sky counted as "in front" (its distance is
  `c_SkyDistance`), a first midpoint beyond the edge would move `low` onto the sky, the interval would lose the crossing and the
  validation (`high` on the sky) would reject the hit: the comb would stay exactly where it is worst. With the sky taking `low`'s
  surface, "behind" is monotonic along the segment and the bisection converges onto the wall. Validation still requires `high`'s
  own pixel to be a surface, so a ray that passed above the wall finds nothing there. When `low` is itself on the sky (a ray
  coming from over the sky), the comparison is with `c_SkyDistance`: never behind, as before.
- `c_MinThickness` (5 cm) and `c_ThicknessScale` (2%) keep their values and become the validation's tolerance; the step term of
  today's acceptance (`|r_k - r_{k-1}|`) goes. The color fetch and confidence follow at `hitAlong` as today (:118-130), with D3's
  `GetHitConfidence(hitUV, hitPixel, ray)`.
- `ScreenReflectionSteps` gains `uint32_t MaxCandidates` (`RendererQuality.h:102-106`; `c_ReflectionsNames`: Off and
  Checkerboard 1, unused; Spatial 2; SpatialHigh 3) and `SpatialReflectionParameters::Padding[0]` becomes `MaxCandidates`
  (`SceneRenderData.h:431`, the struct stays 32 bytes; `TraceSpatialReflections` sets it, `SceneRendererScreenReflections.cpp:305-307`).
- Checkerboard's march is not changed (Medium's 0.6.0 costs; its noise is the TAA's to average).
- One fetch per step as today (the scene distance of the previous point is carried); the bisection costs `RefineSteps` fetches
  per candidate, at most `MaxCandidates` times. Section 6.3 measures it.

### D5 Specular occlusion off the SSR term (step 5, R1)

What a screen-space ray found is visible light: ambient occlusion must not darken it again. In S0.11's `GatherIndirectSpecular`,
for each branch that has a screen reflection (the probes' branch and the sky's branch), the fallback (the probes' and the sky's
radiance, as S0.11 forms it, with the sky's scale where today's code applies it) is occluded and the screen's light is not:

```hlsl
if (inputs.ScreenReflection.a > 0.0)
	specular = (fallback * inputs.SpecularOcclusion * (1.0 - inputs.ScreenReflection.a) + inputs.ScreenReflection.rgb * inputs.ScreenReflectionOcclusion) * inputs.SpecularColor;
else
	specular = fallback * inputs.SpecularColor * inputs.SpecularOcclusion; // S0.11's expression and operand order: bit for bit
```

Pixels without a screen reflection keep S0.11's expression exactly, so they render bit for bit as before (the "rough surfaces
keep the sky's reflection" case depends on that).

**The material's own occlusion stays on the screen's light** (review correction). Screen-space ambient occlusion is what
double-counts SSR's hits: the trace already saw what blocks the ray at the screen's scale. A material's occlusion map
(`surface.Occlusion`, crevices below a pixel that no screen-space ray can see) is not: dropping it would make the dents and
seams of a scanned metal sheet glow wherever SSR's confidence is high and darken again where it fades into the probe (the edge
fade, the roughness fade), a pattern that moves with the camera. So `IndirectInputs` gains, appended (S0.11's rule),
`float ScreenReflectionOcclusion`, filled in `GetIndirectInputs` with Lagarde's formula on the material's occlusion alone,
`saturate(pow(NoV + surface.Occlusion, exp2(-16 * surface.Roughness - 1)) - 1 + surface.Occlusion)` (no screen occlusion, no
horizon fade: the trace refuses rays below the geometric surface). With no occlusion map `surface.Occlusion` is 1 and the
formula gives exactly 1, so every case below behaves as the code above without it; one `pow` per pixel. The horizon fade stays in `SpecularOcclusion` for the fallback; the trace
already refuses rays below the geometric surface (`ScreenReflections.glsl:51`). This applies to Medium's Checkerboard
reflections too (the forward pass does not know the mode), at no cost. `radiance-cascades` later multiplies the open share into
`SpecularOcclusion`: it then reaches the fallback only, as it should.

### D6 The misplaced probe patch (step 6, R1)

`<show>/captures/p14_barrel_high_probe.png` against `p06_barrel.png`: with a box-projected probe, a bright blurred patch on the
wet plane between the barrel and the lit wall, where SSR misses. Its setup was not kept (no script in `<show>/scripts` makes
it; unverified). The cause is found first, then fixed.

**Reproduce** in `Scenes/Specular.stscene`: the probe active, GoldenHour at 9 degrees and azimuth 45 (the showcase audit's sun:
`sunDirection` 0.698, 0.156, 0.698), the camera low over the wet plane looking toward the lit box and the back wall with a
crate in front (as `p14`). If no patch appears within half a day, rebuild `p14` from the audit's proof assets
(`<show>/proof_gen/ProofTank.glb`, `<show>/downloads/proof/barrel_03`) in a copy of the look-dev project. Append the shot as
`lookdev.patch` with a `patch` crop.

**Isolate** with four captures of that view (High, no AA): SSR on/off times `BoxProjection` on/off (`component.set`), plus the
probe off. Then, for the patch's center pixel, compute on the CPU (a scratch script, not committed) the box-projected lookup
direction (`ReflectionProbes.glsl:70-82` with the probe's center, axes and extents, the pixel's world position from the camera and
the plane, its mirror direction) and read the capture (`Assets/Lighting/Specular/<id>.hdr`, the equirect mapping of
`ProbeBake.cpp:94-99`) along it: what the patch shows is then named.

| Finding | Cause | Fix |
| --- | --- | --- |
| Patch with SSR off, gone with `BoxProjection` off, the lookup hits a bright surface inside the box (the lit box, a tank's highlight) | Box projection maps content inside the box onto its faces: the parallax of an open yard's box | Authoring, not code: DG and the Manual say a box-projected probe is for spaces whose walls are the box's faces; open yards use `BoxProjection` off or a box fitted to what bounds the yard (the hut's wall, the bus side). The showcase agent gets the rule for its Yard probes (plan 4.2 step 6) |
| The lookup lands on a baked highlight (a tank's or the sheet's sun streak seen from the probe's center) | View-dependent direct specular baked into the capture, looked up from elsewhere | `SceneRenderOptions::ProbeCapture` (below) also leaves the direct lights' specular out of the bake's frames (`MaterialParams.w` = 1 in the forward pass: `EvaluateLight`'s specular term times 0 behind a uniform branch) |
| The lookup lands on the sun's clamped disk | The bake draws the disk (`Skybox.frag:21-35`), clamped to 65,000 nits at conversion and unshadowed: a glint where the analytic sun is shadowed, displaced by box projection | `ProbeCapture` draws no disk (below) |
| Patch only with SSR on | The SSR confidence's partial fade over a fallback that is right | Steps 3-4 already changed this: re-capture; if it remains, the trace's confidence at that pixel is examined (the CPU predicts the ray) |

**The bake draws no sun disk, whatever the finding.** The clamped disk in a capture is a misplaced bright patch by construction:
it is unshadowed, box-projected and 65,000 nits in a mirror's level 0, while the directional light already lights everything with
its shadow. The atmosphere's environment cube excludes it for the same reason (`AtmosphereCube.comp:8-9`). So:

- `SceneRenderOptions::ProbeCapture` (bool, default false, `R/SceneRenderer.h:384-435`): "a reflection probe's view: no sun
  disk in the background (the directional light lights the scene; a disk in a capture would light surfaces again, unshadowed and
  in the wrong place)". With it, `RenderFrame` sets `frame.SunDiskDirection.w = 0` right after its `UpdateEnvironment` call
  (`SceneRenderer.cpp:3190`; `UpdateEnvironment`, :866, takes no options, and `UpdateAtmosphere` writes the disk at
  `SceneRendererAtmosphere.cpp:355`); `Skybox.frag:26-27` then draws no disk. The procedural sky's disk is part of its
  environment cube, as for every surface's image light, and HDR skies have `RemoveSun`: unchanged, stated in DG.
- `RenderNextProbe` sets it (`E/EditorLightingCommands.cpp:187-189`).
- If the isolation names baked highlights, the same flag also sets `MaterialParams.w` to 1 (`SceneRenderData.h:78`, w unused
  today: its comment names the claim) and `PBR.glsl`'s direct specular is skipped for it; otherwise `MaterialParams.w` stays free.

The commit states the finding with its four captures and the CPU lookup, and the fix. The comb of `p06` and `p10` is checked on
`lookdev.wet`'s `edge` crop after steps 2 and 4 (`P_3`, `P_4` at most 0.3%).

### R1's landing

When steps 1-6 pass: an independent review with its findings fixed or answered; Debug ctest with every GPU suite under the
validation layer; Release ctest without perf; `CompatLint.py`; the A/B of section 6.3; `ci/wave8-reflections` green on every job.
ThirdPerson uses SSR (`Course.stscene`, `MainMenu.stscene`) and a probe (`Course.stscene`): its `Picture.png` and screenshots
are retaken through its command script's capture steps and `ThirdPersonCommands.json.in` passes. R1's docs land with it
(section 7: DG "Reflections", the Manual's two sections, the AGENTS.md bullet's first form, the release-note lines of steps
1-6, the Roadmap's open items 877 and 887; step 7's docs and notes too if it joins, section 3). Then the orchestrator lands it in
slot 4 (plan 7.2) and the sample agent's editor is rebuilt; R2 continues on the branch after merging main.

### D7 Highlights (step 7, R2, first)

The owner (2026-11-01, decisions.md): "highlights are extremely important just like in a drawing, because they should be
cheap." Step 7 starts as soon as R1 is in review and joins R1's landing if it is verified by then (section 3). Every highlight is
judged with no AA, FXAA and TAA, in motion as well as in stills: one that shimmers or aliases fails (the strafe's highlight paths,
D1 and section 5, and a look in a windowed editor along the same paths in each mode). Specular anti-aliasing keeps its rules: the
lobes below add a constant per light to alpha^2, no derivative.

One mechanism serves the sun and the small lights: a source of angular radius theta spans the half maximum of a GGX lobe of
`alpha_s = tan(theta) / c_LobeHalfMaximum`, so it widens a highlight's distribution by `alpha_s^2` less what the smallest
roughness spreads already. The distribution stays normalized, so a larger source widens the highlight and lowers its peak
without brightening it, at one evaluation per light. (a) is the sun's disk, (c) a sphere's for point and spot lights, (b) judges
both on the lanterns and the fire.

**(a) The sun's size in specular.** A disk of angular radius theta spans the half maximum of a GGX lobe of
`alpha_s = tan(theta) / 1.287` (2.6), and the lobes of the surface and the disk combine about as variances add. The shading's
smallest roughness already spreads `alpha_min = 0.045^2`, so only the rest is added, and a mirror shows the disk at its size:

- CPU (`R/SceneRenderData.h`, beside `FrameConstants`):

```cpp
// The alpha^2 the forward pass adds to a directional light's specular lobe for its disk (ReflectionParams.w): a disk of angular
// diameter `degrees` spans the half maximum of a GGX lobe of alpha tan(radius) / c_LobeHalfMaximum (the reflected angle per
// alpha at which GGX falls to half its peak: 2 sqrt(sqrt(2) - 1)), less what the shading's smallest roughness spreads already
// (PBR.glsl's c_MinRoughness^2), so that a mirror shows the disk at its size. Clamped to the property's 0-10 degrees.
inline float GetSunLobeAlpha2(float degrees)
{
	constexpr float c_LobeHalfMaximum = 1.2871885f;
	constexpr float c_MinAlpha = 0.045f * 0.045f;
	const float radius = glm::radians(std::isfinite(degrees) ? std::clamp(degrees, 0.0f, 10.0f) : 0.0f) * 0.5f;
	const float alpha = std::tan(radius) / c_LobeHalfMaximum;
	return std::max(alpha * alpha - c_MinAlpha * c_MinAlpha, 0.0f);
}
```

  Values: 0.53 degrees 8.81e-6 (a mirror's lobe becomes 0.00359, 1.77 times today's; roughness 0.06 gains 30% in width; 0.18 0.4%),
  10 degrees 0.004616. `CollectLights` writes it: `frame.ReflectionParams.w = 0.0f` beside its other resets
  (`SceneRenderer.cpp:2491-2492`) and `frame.ReflectionParams.w = RenderData::GetSunLobeAlpha2(light.ShadowSoftness)` in
  `useDirectionalLight` (`:2495-2510`). Review correction: the first form wrote a member into the vector at `:3019`, but
  `RenderFrame` assigns `ReflectionParams` whole at `:3019` and calls `CollectLights` only at `:3050`, so the value would have
  been the previous frame's (a preview, a cut or the first frame shading with the wrong disk). Component writes in
  `CollectLights`, after `:3019`'s whole assignment, keep S0.11's and radiance-cascades' `.y` and `.z` as they are.
- `PBR.glsl`: `EvaluateLightWeighted(surface, lightDirection, radiance, diffuseWeight, sourceAlpha2)`,
  `EvaluateLight(surface, lightDirection, radiance, sourceAlpha2)` (which passes it on with `diffuseWeight` 1.0, as today) and
  `EvaluateClearcoat(coat, view, lightDirection, radiance, sourceAlpha2, out baseWeight)`. After `alpha = roughness * roughness`:
  `float lobeAlpha = sourceAlpha2 > 0.0 ? sqrt(alpha * alpha + sourceAlpha2) : alpha;`, which `DistributionGGX` takes, while
  `VisibilitySmithGGXCorrelated` keeps the surface's `alpha` (masking and shadowing belong to the microsurface, not to the
  source). D stays normalized, so the highlight keeps its light and lowers its peak; with `sourceAlpha2` 0 the arithmetic is
  today's, bit for bit. (Review correction: the first form had `EvaluateLight` pass 0, but `LightSurface`'s opaque variants call
  `EvaluateLight`, `Forward.frag:103`: every opaque surface's sun would have stayed a point. It also widened V, which darkens
  grazing highlights for no physical reason.)
- `Forward.frag`: `LightSurface` (98-112) gains `float sourceAlpha2` and passes it to both branches (`:101`, `:103`) and to
  `EvaluateClearcoat` (`:108`); the sun's call (`:178`) passes `u_Frame.ReflectionParams.w` (uniform), the light loop's (`:207`)
  passes (c)'s per-light value.
- `RendererComponents.cpp:280-282`: `ShadowSoftness` keeps its stored name (scenes, scripts, schemas), moves above
  `.Section("Shadows")`, takes `DisplayName = "Angular Diameter"` and the tooltip "Angular diameter of the light's disk in degrees
  (the sun: about 0.53; an overcast sky's soft sun up to 10): its shadows' penumbra widens with the distance from the caster, and
  its highlight on smooth surfaces takes the disk's size. 0: the hardest shadow the filter allows and a point-like highlight";
  it leaves the shadows' inactive reasons (:296-297): it matters without shadows now. `ComponentSchema.json` regenerated.

**(b) Lantern and fire highlights, judged.** Lanterns and the fire are punctual lights; their highlight on a smooth surface is
the lobe of `c_MinRoughness` (or the surface's), not the bulb's size, and the audit could not judge them under its exposure.

- Shots `lookdev.lantern.wet` and `.sheet` at dusk (the Specular scene's lanterns, 60 cd, the bulb's emissive 3000 nits) with
  automatic exposure, and their `.sized` forms (D1's table: both lanterns' `SourceRadius` 0.06, the bulb's radius); the
  showcase's shot 6 once it exists.
- **Measured** on the `highlight` crop: the peak and the area of the pixels above half the peak (its 8-bit luma), against the
  bulb's mirror image (a 12 cm sphere at the lantern's position mirrored in the plane, projected by the shot's camera: its area
  in pixels). **A pin dot** is a half-peak area under a quarter of the mirror image's on surfaces of roughness 0.1 or less, or a
  highlight that does not read above the surface's reflection of the sky (peak under twice the crop's median).
- **Judged** once (c) is in: the point and the sized forms side by side, no AA, FXAA and TAA, in stills and along the strafe's
  lantern path (D1); the sized form's half-peak area within 0.64 to 1.56 times the mirror image's, and its strafe `Mean` no
  higher than the point form's in the same mode. The commit records the numbers and gives the showcase its authoring rule:
  `SourceRadius` is the bulb's or the flame's radius (a lantern about 0.03-0.06 m, a fire pit's flames about 0.2-0.3 m). No
  other code in this part.

**(c) A source radius for highlights only.** A property per point and spot light: its highlight takes the source's angular size;
diffuse light, shadows, range and culling do not change (plan 4.3 step 7, decisions.md; review correction: the first form of
this spec left it below the cut).

- Components (`R/RendererComponents.h:50-76`): `float SourceRadius = 0.0f;` after `Range` in both, commented "Meters: the radius
  of the bulb or flame, which widens the light's highlights only (0: a point)", and `static constexpr float c_MaxSourceRadius =
  2.0f;` in `PointLightComponent` (the spot light's says "At most PointLightComponent::c_MaxSourceRadius"). Default 0: stored
  scenes and every existing light render bit for bit as before.
- Registration (`R/RendererComponents.cpp:299-333`), after `Range` in both: `.Property("SourceRadius", ...,
  WithTooltip(Range(0.0f, PointLightComponent::c_MaxSourceRadius, 0.005f), "Radius in meters of the light's bulb or flame: smooth
  and wet surfaces show its highlight at that size, the same light spread wider (a lantern about 0.05, a fire about 0.25). Its
  diffuse light and shadows do not change. 0: a point"))`. No inactive reason (it applies with and without shadows).
  `StrataTests/Architecture/ComponentSchema.json` regenerated.
- The record (`R/SceneRenderData.h:235-248`) has no free lane (2.6), so it gains, appended, `glm::vec4 Source = glm::vec4(0.0f);
  // x: the source radius in meters (highlights only), yzw: 0`, and `static_assert(sizeof(LightRecord) == 96)`;
  `SH/Include/Lights.glsl:13-20`'s mirror gains `vec4 Source;` after `Bounds`. Every user sizes by `sizeof`
  (`RenderLights.cpp:36`, `RenderSceneGPU.cpp:653`, 831, `GPURenderSceneGPUTests.cpp:97`, 866, 883). `RenderLights::MakeRecord`
  (`RenderLights.h:42`, `.cpp:55-82`) takes `float sourceRadius` after `outerDegrees` and writes `Source.x` for drawn lights
  (`std::isfinite ? std::clamp(sourceRadius, 0, c_MaxSourceRadius) : 0`); `ComputeRecord` passes `light->SourceRadius`
  (`.cpp:175-176`, 183). A changed radius changes the record, which the existing `on_update` connections (`RenderLights.cpp:91-95`)
  log and `RenderSceneGPU` uploads. Nothing that decides how casters are drawn changes: the shadow caches stay untouched.
- Both light paths carry it in `LightData.SpotAngles.w` (0 on both today; `LightData` stays 64 bytes; `SceneRenderData.h:228`'s
  and `Scene.glsl:308`'s comments gain "w: the source radius in meters (highlights only)"): the CPU path at
  `SceneRenderer.cpp:2579` (`glm::vec4(record.SpotAngles.x, record.SpotAngles.y, 0.0f, record.Source.x)`), the GPU-driven path at
  `LightCompact.comp:93` (`vec4(light.SpotAngles.xy, float(u_LightShadowSlots[record]), light.Source.x)`).
- `PBR.glsl`, beside `c_MinRoughness` (the sun's constant of (a) is the same number; `GetSunLobeAlpha2`'s comment names this
  function):

  ```hlsl
  // The alpha^2 a spherical source adds to a highlight's lobe: of `radius` at `distanceSquared`, it spans an angular radius theta
  // with sin(theta) = radius / distance, and widens the lobe by (tan(theta) / c_LobeHalfMaximum)^2, less what the smallest
  // roughness spreads already (c_MinRoughness^4), as GetSunLobeAlpha2 does for the sun's disk. 0 for a point.
  static const float c_LobeHalfMaximum = 1.2871885; // The reflected angle per alpha where GGX falls to half: 2 sqrt(sqrt(2) - 1)
  static const float c_MaxSourceSine2 = 0.5;        // At most 45 degrees of angular radius (a surface touching the bulb)
  float GetSourceLobeAlpha2(float radius, float distanceSquared)
  {
      if (radius <= 0.0)
          return 0.0;
      float sine2 = min(radius * radius / max(distanceSquared, 1e-8), c_MaxSourceSine2);
      float minAlpha2 = c_MinRoughness * c_MinRoughness * c_MinRoughness * c_MinRoughness;
      return max(sine2 / ((1.0 - sine2) * c_LobeHalfMaximum * c_LobeHalfMaximum) - minAlpha2, 0.0);
  }
  ```

  The light loop (`Forward.frag:186-208`) computes `float sourceAlpha2 = GetSourceLobeAlpha2(light.SpotAngles.w,
  distanceSquared);` once per light, after its attenuation test, and passes it to `LightSurface` (`:207`). A radius of 0 returns
  0 and (a)'s branch is skipped: bit for bit. Cost: one compare per light without a radius, about ten instructions with one; no
  buffer, pass or binding is added.
- `ThumbnailCache::c_Version` moves once in R2, for (a) and 8a (thumbnails of glossy materials change with the sun's lobe).

### D8 Materials (step 8, R2)

**(a) IOR to F0.**

- `R/MaterialData.h`/`.cpp`: `float MaterialGPU::GetDielectricF0(const MaterialProperties& properties)`: `0.04f` exactly when the
  IOR is 1.5 or not finite; otherwise `n = clamp(IOR, 1, 3)`, `r = (n - 1) / (n + 1)`, `r * r` (1.33: 0.02006, 1.45: 0.03374,
  2: 0.1111, 3: 0.25, 1: 0). Float arithmetic gives 0.040000003 for 1.5 against the literal's 0.039999999, so the exact case is
  written out: materials at the default render bit for bit as before (expected; a shader compiler that folded the literal
  `0.04` differently from a value read from the buffer may move a dielectric pixel by one code, which section 6.1 accepts and the
  commit states, and nothing more).
- `Build` writes it into the spare lane: `data.Absorption = glm::vec4(GetAbsorption(properties), GetDielectricF0(properties))`
  (`MaterialData.cpp:83`); `SceneRenderData.h:217-219` and `Scene.glsl:295` say "w: the dielectric's F0 from the IOR
  (MaterialGPU::GetDielectricF0)". `MaterialData` stays 144 bytes; both paths read the same buffer.
- Shaders: `SurfaceData` gains `float3 F0`; `ShadeLitSurface` sets `surface.F0 = lerp((float3)material.Absorption.w,
  surface.Albedo, surface.Metallic)` where `f0` is formed today (`Forward.frag:150`) and uses it; `EvaluateLightWeighted` reads
  `surface.F0` instead of forming its own (`PBR.glsl:111`); `TransmitLight` forms it alike (`Forward.frag:346`). The clear coat
  keeps `c_ClearcoatF0` (a coat is 1.5 by definition). S0.11's `IndirectInputs.SpecularColor` follows from the head.
  `SurfaceData` is used only by `Forward.frag` (checked at 76f28a21).
- No importer change: `KHR_materials_ior` already reaches `IOR` (`GltfImporter.cpp:891-903`), so imported values now count with
  no re-import. The IOR property's tooltip (`Material.cpp:69-71`) adds "and how much a dielectric reflects head-on: water 1.33
  reflects 2%, glass 1.5 4%, diamond 2.42 17%; 1 reflects nothing".
- `ThumbnailCache::c_Version` + 1 (once in R2, with 7a).

**(b) The night sky's bands.**

1. **Diagnose (first):** a GPU test helper renders the atmosphere at -10 degrees and reads back the transmittance, multiple
   scattering and sky-view tables (`GetAtmosphereTransmittanceTable`, `GetAtmosphereSkyViewTable`, `SceneRenderer.h:831-832`, and
   an accessor for the multiple scattering table added beside them) and the environment cube's level 0; it counts, per table,
   the non-zero texels below 2^-14. The hypothesis holds for a table where at least 1% of the texels that light the visible sky
   are subnormal. The commit records the counts.
2. **Fix (for the per-lux tables, expected: the sky view, the environment cube, and with them multiple scattering and aerial
   perspective's in-scattering):** a storage scale, `Atmosphere::c_StorageScale = 1024.0` (`R/Atmosphere.h`) and
   `ST_ATMOSPHERE_STORAGE_SCALE` (`Include/Atmosphere.glsl`). `AtmosphereMultipleScattering.comp:82` stores `multiple * scale` and
   `GetMultipleScattering` (`AtmospherePass.glsl:58-62`) returns its texel times `1 / scale`; `AtmosphereSkyView.comp:53` stores
   `clamp(light * SunColor * scale, 0, 65000)`; `AtmosphereAerial.comp:63` stores the in-scattering times the scale (not the
   transmittance); `AtmosphereCube.comp` passes the table's values through unchanged; `SceneRenderer.cpp:921` becomes
   `intensity *= m_AtmosphereSunIlluminance / Atmosphere::c_StorageScale`, so every reader (`Skybox.frag:25`, `Fog.glsl:42,49`,
   `AerialPerspective.glsl:26`, `Forward.frag`'s image light, `LightingViewStats.comp:87`) multiplies by the same product.
   Scaling by a power of two is exact in half floats while nothing overflows, so every value that was a normal half float before
   is bit for bit the same: daytime pictures do not change. Night values move up 10 stops, out of subnormals down to about 1e-7
   per lux. The clamp now applies at 63.5 per lux: the test below checks that no daytime table reaches it.
3. **Only if the diagnosis finds the transmittance table subnormal where it lights the sky** (sun far below the horizon, long
   grazing paths): store its optical depth (`-log T`, 0 to about 50 in half floats, a relative error of about 1e-3 per unit) and
   take `exp(-tau)` after the bilinear lookup (`GetSunTransmittance`, `AtmospherePass.glsl:50-56`; the CPU's
   `ComputeTransmittance` stays and the "within 1%" case compares `exp(-tau)`). Not expected; an extra commit if needed.
4. If neither table holds the bands, the cause is elsewhere: the commit records the counts and the step goes to the open items
   with what was found, and shot 6 holds at dusk (plan 4.2, graybox row 6).

### D9 Back-facing hits (step 9, R2)

Objects in front of a mirror show their back, which the depth buffer does not hold: today those hits are refused and the object
becomes a crescent (`<spec>/captures/crops/crescents.png`), the normal case for a bus side at 45 degrees.

- **Option F (filled silhouette):** a validated hit (D4) on a surface met from behind is kept at `c_BackFaceConfidence` = 0.5:
  `facing = max(saturate(-4 n . d), validated ? c_BackFaceConfidence : 0)`. The silhouette fills with the front's color at half
  confidence over the probe's or the sky's reflection. `GetHitConfidence` takes `bool validated`; Checkerboard passes false, so
  Medium keeps crescents (its hits are not validated).
- **Option C:** keep crescents, and say so in the Manual.
- **Decided by looking** at `lookdev.sheet.angle` (crescents crop) and the wet plane's reflections of the posts, at 3840x2160 High,
  no AA and FXAA, F and C side by side on the sheet the owner will see; the image where an object in front of a mirror reads as
  that object reflected, without a wrongly lit shape, wins. Default if unclear: C. A test is written for whichever wins
  (section 5). No case of `GPUSceneSSRTests.cpp` tests back faces today (checked at 76f28a21: no case names back faces, facing or
  crescents; the plan's "asserts today's behavior" does not hold), so the winner's case is new.

### D10 Probes at High (step 10, R2)

**The row.** `RendererQualitySettings::ReflectionProbeResolution` (`uint32_t`, default 128: Medium's; placed after
`MotionBlurQuality`, before the players' rows, `RendererQuality.h:146-148`):

- `constexpr PresetRow<uint32_t> c_ReflectionProbeResolutionRow = { 128, 128, 256, 256 };` (`RendererQuality.cpp:35-47`), its
  line in `GetPreset`, `FromSectionValue` (`ReadField` of a string, as `EnvironmentResolution`), `ToSectionValue`
  (`std::to_string`), `Clamp` (`== 256 ? 256 : 128`).
- The section's field: `EnumField("ReflectionProbeResolution", "Reflection probe resolution", "Texels each way of each face of
  the reflection probes' pictures: 256 keeps polished metal's reflections of nearby things sharp, for about 51 MiB more GPU
  memory per view that shows a probe (17 MiB at 128, 68 MiB at 256 for 16 probes) and more GPU time when a probe's picture is
  prepared.", { "128", "256" }, "256")`.
- Traits: `{ "ReflectionProbeResolution", true, "GPU.Reflections", &ReflectionProbeBytes }`, with

```cpp
uint64_t ReflectionProbeBytes(const RendererQualitySettings& settings, glm::uvec2)
{
	// The probes' cube array (SceneRendererReflectionProbes.cpp): RenderData::c_MaxReflectionProbes slots of six RGBA16F faces
	// with RenderData::c_ReflectionProbeMipCount levels, and the scratch cube with its whole chain. A renderer with a lower
	// MaxReflectionProbes allocates less; the row counts the default.
	const uint32_t size = RendererQuality::Clamp(settings).ReflectionProbeResolution;
	auto cubeTexels = [](uint32_t edge, uint32_t levels)
	{
		uint64_t texels = 0;
		for (uint32_t level = 0; level < levels; level++)
			texels += static_cast<uint64_t>(std::max(1u, edge >> level)) * std::max(1u, edge >> level);
		return texels * 6;
	};
	return (RenderData::c_MaxReflectionProbes * cubeTexels(size, RenderData::c_ReflectionProbeMipCount)
		+ cubeTexels(size, RenderData::GetReflectionProbeMipChain(size))) * 8;
}
```

  128: 17,821,680 bytes (17.0 MiB); 256: 71,286,768 (68.0 MiB). Allocated only by renderers that show a probe.
- `RenderData::c_ReflectionProbeSize` goes; `RenderData::GetReflectionProbeMipChain(size)` (the whole chain's level count, today's
  `ProbeMipCount`, :39-48, moved) stays beside `c_ReflectionProbeMipCount` (6 at both sizes: roughness 0 to 1 in steps of 0.2, as
  the sky's). The renderer reads `m_Specification.Quality.ReflectionProbeResolution` in `EnsureReflectionProbeResources`
  (recreate when the slots or the size differ, :117) and `ConvertReflectionProbes` (:345). The irradiance projection keeps its
  32-texel level: `level = GetReflectionProbeMipChain(size) - 6` (2 at 128, 3 at 256; :35, :372, :384). Prefilter samples:
  `GetProbePrefilterSamples(size, mip)` in `RendererQuality`: 256, except 128 for levels 1 and 2 at 256 (their narrow lobes read
  finer source levels, the sky's rule at 512, `RendererQuality.cpp:635-643`), so a conversion at 256 takes 2.2 times the samples
  of one at 128, not 4. `ProbeBake.h:25`'s comment drops "twice the renderer's".
- **No re-bake:** captures are 1024x512 from 256-texel faces (`ProbeBake.h:25-27`); `EquirectToCube` picks the capture's level 0
  for a 256 face (`EquirectToCube.comp:29`: `log2(1024 / (4 x 256)) = 0`).
- **`SetQuality`** (`SceneRenderer.cpp:1856-1907`): a new resolution releases the probes (`ReleaseReflectionProbes`) and sets
  `m_ReflectionProbesConvertAll`, so the next gather converts every loaded capture in view in that frame, as the atmosphere
  computes its environment at once after a new `EnvironmentResolution` (:1877-1880): a settings change never shows the boxes
  falling to the sky. It counts among the changes that return true.
- `RendererQualityTests.cpp`: the presets' column (`:42-62`), "High is Medium below 1440 rows but for the rows that differ at every
  size" (:222-254) lists the new row, the section's validation (:256), the traits (:530). `FeatureTest.stproj`'s
  `Renderer.Quality` (Custom, 150-173) takes `"ReflectionProbeResolution": "256"`: DG "Quality settings" asks a cost knob's
  Custom value off the struct's default (128), so the feature run converts at 256 (review correction: "128" is the default and
  exercises nothing new). Its `"Version"` follows `c_SectionVersion`, 4 to 5 at this landing (the integrator renumbers if another
  stream chose 5).
- DG "Quality settings" table: a row `ReflectionProbeResolution` 128, 128, 256, 256 and a memory row "Probes per view (16 slots,
  scenes with probes) 17, 17, 68, 68 MiB".

**Measure, then spread if needed.** A perf case (6.2) converts six captures in view at 256 with `SpreadLightingUpdates` and
reports the largest `GPU.Reflections` of those frames (SSR off, so the stage holds only conversions). If it is at most 2 ms on
this machine, two conversions a frame stay and nothing more is built. Above 2 ms, conversions are spread by work, not by count:

- A conversion is a list of steps: (1) the cube and its chain (`EquirectToCube`, `CubeDownsample`); (2) level 0 (the copy); (3-7)
  levels 1 to 5, each one `Prefilter` dispatch over the six faces; the irradiance projection rides with level 5. Each step's
  cost in units is texels times samples: (1) `2 x 6 x S^2`, (2) `6 x S^2`, level l `6 x (S >> l)^2 x samples(S, l)`.
- The budget per frame, `c_ProbeConversionBudget = 17,350,656` units, is exactly two whole conversions at 128 (8,675,328 each):
  Medium converts as today, two a frame. A conversion at 256 costs 18,972,672 units: its first frame takes the cube, levels 0, 1
  and 2 (16.9 million), its second frame the rest, so no frame does more than two 128 conversions did. The largest single step,
  level 1 at 256, is 12.6 million, within the budget: by units no step needs splitting by face; by time, see the last bullet.
- `m_ReflectionProbeConversions` becomes a queue that lives across frames (`ReflectionProbeConversion` gains `uint32_t NextStep`,
  `SceneRenderer.h:1343-1347`). The gather plans the frame: it walks the queue from its front, taking steps while the budget lasts
  (at least one step a frame), and a probe whose conversion finishes in the plan enters this frame's list; a probe whose
  conversion is unfinished stays pending (its box falls to the sky, as today while it waits). `ConvertReflectionProbes` records the
  planned steps. The scratch cube carries the front conversion's levels between frames; the next starts after it.
- A slot with a conversion in the queue is never chosen for another capture; a probe that leaves the view keeps its queued
  conversion while its slot is kept, and a slot given away drops its conversion; a capture whose texture object changed starts
  again. Without `SpreadLightingUpdates` (captures, thumbnails, the bake) the budget is unlimited, as today.
- `SceneRendererStats::ReflectionProbeConversions` counts conversions finished in the frame; `ReflectionProbeConversionSteps`
  (new) the steps recorded. `editor.perf`'s render stats gain `reflectionProbeConversionSteps`.
- If even the spread frame is above 2 ms, the budget constant goes down to what 2 ms holds on this machine (stated with the
  measurements) and Medium converts fewer than two a frame. If one step alone is above 2 ms (level 1 at 256 is 1.45 times a
  whole 128 conversion, so this is possible if the specular audit's single 3.6 ms reading was the conversion's), steps split by
  face: `RenderData::IBLParameters` (`SceneRenderData.h:329-335`) gains `uint32_t FirstFace` (padded to 32 bytes; every IBL
  dispatch passes 0 but these), `Prefilter.comp` adds it to `id.z` for the texel's direction, and `DispatchIBL`'s `layers` and
  `outputFirstLayer` (`SceneRenderer.cpp:629-649`) take 1 and `slot * 6 + face`; a step's units divide by six. Only if 256 cannot
  hold 2 ms a frame even so does the row stay 128 at High and Ultra (plan 4.3), with the reason in the open items.

The showcase's stations (plan 4.2 step 6) keep the old station active for about a second while the new one converts; with 256
that is two frames per probe.

### D11 Adoption and docs (step 11, R2)

R2 changes more samples than R1 did (review correction: the first form named ThirdPerson alone). `LookDev.py compare` of the
base against R2's tip over every shot names them; expected: ThirdPerson (its probe at 256, SSR), Open World (probes in
`Worlds/Island.stscene` and five landmark cells, so 256 at High; the sun's lobe on its smooth surfaces), Physics Playground
(the sun's lobe on its glass and plastics; its IORs are 1.5, so F0 does not move). Each changed sample's `Picture.png` and
screenshots are retaken through its command script's capture steps, and every `Editor/<Sample>Commands.json.in` passes.
Probes baked before D6 carry the sun's clamped disk: a sample whose capture's brightest texel is above 1e6 nits is baked again
through `lighting.bakeProbes` with its stations' sun (a step in its command script, so the bake is reproducible; ThirdPerson's
Goal probe keeps its open-air placement, commit 3b3fb193). The showcase's adoption pass (plan 4.2's table) gives its lantern and
fire their `SourceRadius` by 7b's rule. DG, Manual, Roadmap, release notes (section 7); `lookdev.mirror` captured again 5 times
at 3840x2160 with FXAA in one editor to record whether Spatial SSR's two images (`Roadmap.md:887-898`) survive the repair;
Checkerboard named in the open items as the one effect that needs TAA, kept for Medium's 0.6.0 costs (plan 9).

## 5. Tests per step

All GPU cases use `Tests::GPUContext` and end with `CHECK(gpu.GetNewErrorCount() == 0)`. "Fails on 0.7.0" was reasoned from the
code, and each such case is run once against the base build before its fix to confirm it fails.

**Step 1**

- `StrataTools.LookDev`'s selftest: the two synthetic images (D1).
- `GPU.SceneRenderer.SSR`, "A strafe past a rough metal sheet and over a wet floor: the reflections do not flicker": D1; asserts
  the SSR-off floor (`Blocks` at most 0.3%, both statistics finite, `MeasuredShare` of the surface's mask above 0.9) and reports
  `Blocks`, `BlocksP99`, `Mean` and `P99` for every mode, SSR on and off.
- `Renderer.ReflectionMetrics` (CPU, a new suite; `Renderer*` already has its shard in `StrataTests/CoreShards.json:21`):
  `MeasureNoise` on a synthetic 4x4 pattern (`Pattern4` within 5% of `Noise`, `Pattern3` under 0.2 of it), a synthetic 3x3 pattern
  (the reverse) and a flat image (0).
- `Renderer.FlickerMetrics` (CPU): radiance-cascades' five cases (D1), if this stream creates the file; plus "A display frame
  decodes sRGB": `ReadDisplayFlickerFrame`'s decoding of the codes 0, 128 and 255 against the sRGB curve within 1e-6.

**Step 2**

- `GPU.SceneRenderer.SSR`, "A rough metal floor's reflection of a picket fence shows no screen door": `MirrorScene` with
  roughness 0.18 (`SetAntiAliasingFor` for the mode, as the wall case does), the cube replaced by a fence of 40 unlit white posts
  4 cm wide and 1.5 m tall, 12 cm apart, centered at z = -2, in front of an unlit dark-red wall 40 m wide and 4 m high at z = -6;
  camera (0, 0.6, 6) toward (0, 0.3, 0), 512x512, `ExactSpecification()`, Spatial and SpatialHigh, FXAA and none; the region is
  the fence's CPU-predicted mirror image shrunk by 12 px and kept in the middle 80% of the view; `MeasureNoise` of the output:
  `Pattern3` and `Pattern4` at most 0.2% (the screen-locked part; `Noise` is reported, since the posts' edges are content). Review
  correction: the first form reflected a uniform wall, where every offset's ray hits the same color, so 0.7.0 shows no pattern
  there and the case could not fail. Here whether a ray hits a post depends on its offset, which is the screen door. The base
  build's `Pattern4` is recorded before the fix and must be at least three times the bound; if not, the posts move closer
  (8 cm apart) until it is. Fails on 0.7.0.
- The existing cases (mirror cube where the CPU predicts on both paths, edges blend, rough floors keep the sky, still frames
  identical from the third on) pass unchanged.

**Step 3**

- "Hits fade only where their rays leave the screen": mirror floor (roughness 0), an unlit wall 60 m wide spanning the view, camera
  low, 512x512, Spatial. The trace's own output is read through a new test accessor `SceneRenderer::GetTracedReflectionTexture()`
  (beside `GetScreenReflectionTexture`, `SceneRenderer.h:881`; review correction: no accessor reached the traced target). (a) Hit
  texels within 10% of the left and right sides whose CPU-computed ray (`BeginScreenRay`'s math) leaves through the top more than
  52 px (0.1 x 512) beyond the hit have confidence at least 0.99; at least 200 such texels exist, so the check cannot pass empty
  (rays on a floor spread outward with the perspective, so a texel's side alone does not say where its ray leaves). Fails on
  0.7.0: they fade to 0 at the sides. (b) For 64 sampled hit texels the confidence equals the CPU's `saturate(remaining /
  (0.1 x 512))` within 0.05. The same at Checkerboard with TAA on its half-resolution target (`GetScreenReflectionTexture`, the
  pixels traced that frame).

**Step 4**

- "Thin posts in front of a mirror leave no ghost columns": the audit's geometry (`<spec>/captures/crops/grazing_bands.png`),
  where reflected rays cross the screen sideways: a vertical mirror (white metal, roughness 0) 20 m long along x at z = 0, facing
  +z; five unlit white posts 5 cm wide and 2 m tall at z = 1, x = -6, -3, 0, 3, 6; an unlit blue wall at z = 12; camera at
  (14, 1.2, 1.4) looking toward (0, 1.2, 0) (about 6 degrees grazing), 512x512, Spatial and SpatialHigh, FXAA and none. On the
  CPU every mirror texel's reflected ray is intersected with the posts and the wall: the texels whose ray hits a post (expected
  white) and those whose ray passes within 30 cm behind a post and meets the wall (expected blue: today's ghosts). At least 98%
  of the second class read blue with confidence at least 0.9; at least 90% of the first read white (the erosion bound at
  silhouettes, risks). Fails on 0.7.0 (ghost columns). Review correction: the first form put the post over a mirror floor, where
  reflected rays run up the screen parallel to the post, so pixels beside it never cross it and 0.7.0 would have passed.
- "Spatial reflections of a wall across a grazing mirror floor are whole" (`GPUSceneSSRTests.cpp:572-631`): `raggedEdge` goes
  from 15% of the image's height (:608) to 2 pixels. Fails on 0.7.0. (It depends on the bisection's sky rule of D4.)
- The strafe case gains its bounds, written from step 1's numbers (D1): `Blocks` at most 1.0% for Spatial and SpatialHigh in
  each AA mode, on both surfaces (the plan's proposed target); and `Mean` at most the SSR-off run's `Mean` plus half of what the
  base build's SSR added to it (`Mean_off + 0.5 (Mean_base,on - Mean_off)`, the constants written into the case with the step 1
  commit's numbers), so the per-pixel crawl at least halves. The commit states which of the two bounds 0.7.0 fails; if 0.7.0
  passes both, the commit says so (the flicker was not there to fix in this geometry) and the case stays as a guard.
- `Renderer.Quality`, "Screen-space reflections' modes" (`RendererQualityTests.cpp:365`): `MaxCandidates` 2 and 3.

**Step 5**

- "Ambient occlusion leaves a mirror's reflection alone": `MirrorScene` (roughness 0, white metal: no diffuse), AO on and off
  (`AmbientOcclusion` on the neutral post process), Spatial with FXAA: the cube's reflection's texels next to its base (within 0.3
  m of the contact, where AO is below 0.7) differ by at most 1% between on and off; a floor texel beside the cube where the ray
  leaves the screen (no screen reflection) is still darker with AO on. Fails on 0.7.0. A second floor material with a constant
  occlusion map (a generated 4x4 texture of 0.5, `OcclusionStrength` 1) keeps its reflection's texels darker than the plain floor's
  by Lagarde's factor within 2% (D5's material occlusion).

**Step 6**

- `GPU.Editor.Lighting`, "A bake under the atmosphere draws no sun disk into its capture": an atmosphere scene with the sun at 30
  degrees, its `DirectionalLight.Intensity` 100,000 lux (the look-dev scene's; review correction: the component's default of
  3 lux puts the disk near 45,000 nits, under the bound, and the case would pass on 0.7.0), and a probe in the open;
  `lighting.bakeProbes`; the capture's brightest texel is under 1e6 nits (0.7.0: about 1e9; the brightest sky near a 30-degree
  sun is of the order of 1e5). Fails on 0.7.0.
- `GPU.SceneRenderer.ReflectionProbes`, "ProbeCapture draws no sun disk and changes nothing else": the atmosphere's background
  with and without the option: identical but within the disk.
- A case for the finding's fix if it is code (baked highlights: "ProbeCapture leaves the direct lights' specular out").

**Step 7** (highlights)

- `GPU.SceneRenderer.Lighting`, "A sized sun widens its highlight to its disk and keeps its light": a white metal sphere (roughness
  0) seen orthographically (a mirror sphere maps projected area to reflected solid angle uniformly: `d omega = 4 dA / R^2`), no sky
  light, a sun 30 degrees off the view axis, `SpecularAntiAliasing` false (at this size it would widen a mirror's lobe more than the
  sun does), `HDROutput` with `PreExposure` 1 and the sun at 1 lux, so the mirror's peak (about 20,000 today) stays below
  `SanitizeHDR`'s 65,000 clamp (review correction: a clamped peak has no half maximum), 512x512 with the whole sphere in view:
  (a) at `ShadowSoftness` 5 and 10 the sums of the HDR over the sphere's pixels (minus a render without the sun) agree within 5%;
  (b) at 10 degrees the half-peak area, converted to a cone through `4 dA / R^2`, has an angular radius within 0.8 to 1.25 times 5
  degrees; (c) at 0.53 degrees, with the orthographic view narrowed to 0.1 units around the highlight (its half-peak cone then
  covers about 110 pixels), within 0.8 to 1.25 times 0.265 degrees. (b) and (c) fail on 0.7.0 (a pin dot of 0.15 degrees whatever
  the size). A sun under about 2 degrees is not resolved at 512 pixels over the whole sphere, which is why (a) compares 5 and 10.
  (d) The same at (c) on a sphere whose material is opaque and dielectric (the opaque forward variant, `EvaluateLight`'s path):
  the half-peak radius moves with `ShadowSoftness` as on the metal one (guards the `EvaluateLight` defect corrected in D7a).
- `GPU.SceneRenderer.Lighting`, "A point light's source radius widens its highlight to the bulb's mirror image and keeps its
  light": a horizontal mirror plane (white metal, roughness 0), a point light 1 m above it with `SourceRadius` 0.05 and 0.1, the
  camera 3 m above looking straight down at the light's reflection with a vertical field of view of 10 degrees (the mirror image
  of a 0.05 m bulb then spans a radius of about 36 px; for a flat mirror the lobe's half maximum falls exactly on that image's
  edge, 0.75 times the radius from the specular point at these heights), `HDROutput`, `PreExposure` 1, the light's intensity chosen so
  the point form's peak stays under 65,000, `SpecularAntiAliasing` false, 512x512: (a) the half-peak area is 0.64 to 1.56 times
  the CPU-projected area of the bulb's mirror image (a sphere of that radius 1 m below the plane) at both radii; (b) the highlight's
  HDR sum at 0.05 and 0.1 agrees within 5%; (c) radius 0 is today's shading: the existing exact-pixel cases of
  `GPU.SceneRenderer.Lighting` and `.Materials`, whose lights all have radius 0, pass unchanged; (d) a
  matte dielectric (albedo 0.5, roughness 1, IOR 1) beside the mirror differs by under 0.5% between radius 0 and 0.1 (diffuse
  unchanged); (e) the local shadow atlas's contents are identical at both radii with `CastShadows` on (shadows unchanged).
  (a) fails without D7c (a pin dot).
- `Renderer.RenderScene` (CPU, `RenderSceneTests.cpp` beside the light-record cases near 1140): "A light's source radius reaches
  its record": `Source.x` equals the component's `SourceRadius` for a point and a spot light; NaN and negative give 0, 5 gives
  `c_MaxSourceRadius`; an inactive light's record stays the empty one; changing `SourceRadius` through `registry.patch` updates
  the record.
- `GPU.SceneRenderer.GPUDriven`, a builder "Sized highlights" (`GPUSceneRendererGPUDrivenTests.cpp:529-541`): a 10-degree sun, a
  point light with `SourceRadius` 0.1 and a spot light with 0.05 over a mirror floor and a coated sphere: colors within 1/255,
  identical IDs (both light paths carry `SpotAngles.w`).
- The strafe's highlight paths (D1): on the wet plane into the sun and at dusk with the lanterns, every AA mode, Spatial: the
  sized form's `Mean` (disk 0.53 degrees, `SourceRadius` 0.06) at most the point form's in the same mode. A wider lobe must not
  shimmer more; this is the measured half of "every highlight judged in motion", the windowed look the other.
- The feature test: `Feature.stscene`'s `Lamp` gets `"SourceRadius": 0.05` and `Spot` `"SourceRadius": 0.03` (non-default values,
  DG "Feature test"); the scene's directional light keeps its `ShadowSoftness`.

**Step 8** (materials)

- `Renderer.Material`, "The dielectric F0 comes from the IOR, 1.5 exactly 0.04": `GetDielectricF0` of 1.5 is `0.04f`, of 1.33
  0.02006 within 1e-5, of 1 0, of 3 0.25, of NaN and infinity 0.04, of 0.5 0 (clamped to 1); `Build` writes it into
  `Absorption.w`.
- `GPU.SceneRenderer.Lighting`, "The furnace keeps a dielectric's light at every IOR, and its reflectance is the IOR's": a white
  dielectric (albedo 1, roughness 1) at IOR 1, 1.33, 1.5, 2, 3 reads 1 within 3% at the center and rim (diffuse gets what specular
  leaves); a black one (albedo 0) at the same IORs reads `(f0 dfg.x + dfg.y)(1 + f0 (1 / (dfg.x + dfg.y) - 1))` with `f0 =
  GetDielectricF0` and the table read back (`ReadRoughBRDF`, `GPUSceneLightingTests.cpp:70-76`) within 3%. Fails on 0.7.0 at every
  IOR but 1.5. The existing furnace cases (:698-806) pass unchanged.
- `GPU.Atmosphere`, "The night sky at -10 degrees has no bands": the background (`AddNeutralPostProcess`, a manual exposure that
  brings the twilight sky to mid-gray, 512x512 looking toward the sun's azimuth at 20 degrees up): along each column above the
  horizon no step between adjacent rows exceeds 2% of the row's value in linear HDR; and the sky-view table holds no non-zero texel
  below 2^-14 above the horizon. Fails on 0.7.0 if the hypothesis holds.
- `GPU.Atmosphere`, "No daytime table reaches the storage clamp": at noon, at 6 degrees (GoldenHour) and at haze 4, every texel of
  the sky view and the multiple scattering tables is below 65,000 after the scale.
- `GPU.SceneRenderer.GPUDriven`, a builder "Reflections and dielectrics" (`GPUSceneRendererGPUDrivenTests.cpp:529-541`): IOR 1.33
  and 2 spheres, a 10-degree sun, a probe, a mirror floor: colors within 1/255, identical IDs.

**Step 9** (back faces)

- Option F: "A ball in front of a mirror is reflected whole, its back at half confidence": a vertical mirror at 45 degrees and a
  sphere in front of it, Spatial: the sphere's CPU-predicted mirror image is covered by hits, the back half's confidence 0.5 within
  0.05. Option C: "A ball's back is not reflected": the same image's back half has confidence 0.

**Step 10**

- `Renderer.Quality`: the column 128, 128, 256, 256; Clamp; the field round-trips through `FromSectionValue` and `ToSectionValue`;
  the trait's bytes are 17,821,680 and 71,286,768; High differs from Medium at every size in this row.
- `GPU.SceneRenderer.ReflectionProbes`, "ReflectionProbeResolution sizes the cube array, and a new resolution converts every probe
  in view in the next frame": a renderer at 128 with three probes; `SetQuality` to 256 returns true; the next frame's array is 256
  wide with six levels, `ReflectionProbes` 3 and `ReflectionProbesPending` 0 with `SpreadLightingUpdates` on.
- "A probe at 256 shows finer detail on a mirror ball": a checker capture, a mirror ball inside the box: the ball's gradient energy
  at 256 at least 1.5 times 128's (the sky's 512 case's method, `GPUSceneSpecularTests.cpp:317`).
- If spread: "Spread over frames, conversions take their steps within the budget and every probe converts": six captures at 256:
  each frame's planned units at most the budget (or a single step), every probe shaded by the fourth frame, and the image then equal
  to an unspread renderer's; at 128 the existing case's counts (`GPUReflectionProbeTests.cpp:496`: 2, 2, 3 / 2, 4, 1 / 1, 5, 0) hold
  unchanged.
- The feature test passes with the Custom value `"256"`.

## 6. Acceptance and measurement

### 6.1 The numbers

Measured on this machine, Release, alone under PerfLock where times are read, against the base build (S0.12's tip) and 0.7.0's
kept build (`.worktrees/wave8-base`) for the plan's 0.7.0 column.

| What | Target | How measured |
| --- | --- | --- |
| Noise on `lookdev.sheet` (both crops) and `lookdev.tank`, High and Ultra, TAA, FXAA, none | `N` under 1% (proposed in the plan, no prototype behind it) | `LookDev.py capture --shots "lookdev.*" --sizes 3840x2160 --quality high` and `ultra`, `metrics.csv` |
| Screen-locked pattern, same pictures, and `lookdev.wet`'s `edge` crop (the comb) | `P_3` and `P_4` at most 0.3% | same |
| Flicker, strafe over the sheet and the wet plane, Spatial and SpatialHigh, three AA modes | `Blocks` at most 1% (proposed, the plan's); `Mean` at most the SSR-off `Mean` plus half the base's excess (step 4) | the GPU case's values at 1280x720, the wave's one meter (`FlickerMetrics`, D1) |
| Highlights in motion (the owner's "highlights first"): the sun's glint column on the wet plane and the lanterns at dusk, strafed, no AA, FXAA and TAA | sized forms' `Mean` at most the point forms'; nothing shimmers or crawls by eye | the strafe's highlight paths (step 7); a look along the same paths in a windowed editor built from the branch, each AA mode, recorded in the commit |
| Sized small lights | `lookdev.lantern.*.sized`: half-peak area 0.64 to 1.56 times the bulb's mirror image; no pin dot | 7b's measure on the `highlight` crops |
| Sky wedges, `lookdev.wet.sunward` | no wedge: the `left` and `right` crops show the wall's reflection, not the sky's, where the wall is on screen above them | by eye on the page, with the base build's pictures beside |
| The misplaced patch | gone, or named as box projection's parallax with the authoring rule written | `lookdev.patch` before and after |
| `GPU.Reflections` at 3840x2160 High, 100k cubes | `PerfGPU.Renderer.SSRSpatial4KStageGPU` at most 1.2 ms | the perf case in Release, run alone under PerfLock (information on this memory) |
| `GPU.Reflections` on `lookdev.sheet` and `tp.scene`, High, 4K | at most 1.3 times the base's, as a ratio to `GPU.Opaque` in the same frames (proposed) | section 6.3 |
| Medium (Checkerboard) | `GPU.Reflections` and `GPU.Opaque` within the A/B's noise of the base at 1920x1080 | section 6.3 |
| Furnace | white dielectrics 1 within 3% at every IOR; metals as before | GPU cases; `lookdev.furnace` compare: 0 differing pixels expected, at most 1 code on dielectric pixels accepted (D8a) and stated |
| IOR 1.5 unchanged | the step 8a commit changes no look-dev picture but those with an IOR other than 1.5 (at most 1 code elsewhere, as above) | `LookDev.py compare` base against 8a, all shots, High |
| Sun size | Overcast's highlight reads as a soft glow; the default sun on `lookdev.wet.sunward` judged against the base, in stills and in motion (the row above) | `lookdev.overcast`, `.wet.sunward`, side by side |
| Night sky | `maxStep` at most 3 codes on `lookdev.night`'s `sky` | `metrics.csv` |
| Probe conversions at 256 | largest frame's `GPU.Reflections` at most 2 ms with `SpreadLightingUpdates` | `PerfGPU.Renderer.ProbeConversion256MaxStageGPU` |
| Probes at 256 | the polished sheet sharper: gradient energy of `lookdev.polished`'s crop at least 1.5 times 128's | `Custom` 128 against High, same capture |

### 6.2 Perf metrics

Appended to `StrataTests/Perf/Budgets.json`, all `Provisional`, measured in `T/Perf/RendererPerfTests.cpp` with
`MeasuredRendererSpecification()` and the stage helper `MedianGPUStageMs` (:64); a maximum over frames gets `MaxGPUStageMs`
beside it:

- `PerfGPU.Renderer.SSRSpatial4KStageGPU` (exists, Target 1.2): unchanged case, now running the chosen resolve and D4.
- `PerfGPU.Renderer.SSRSpatialHigh4K`, `...GPU`, `...StageGPU` (Target 1.8 ms: the Ultra mode's 48 + 8 steps and three
  candidates): the same case at SpatialHigh.
- `PerfGPU.Renderer.ProbeConversion256MaxStageGPU` (Target 2.0) and `...ProbeConversion128MaxStageGPU` (no target: the reference):
  six captures of 1024x512 converted in view with `SpreadLightingUpdates`, SSR off, the largest `GPU.Reflections` of the frames
  until all are converted, with `Quality.ReflectionProbeResolution` 256 and 128.

No existing entry changes: `MeasuredRendererSpecification()` is Medium (128 probes, Checkerboard), so
`PerfGPU.Renderer.ReflectionProbes16GPU` (`Budgets.json:219`) keeps its scene and cost.

### 6.3 How the stream A/Bs its own stages

Information, never a gate, when no other measuring agent holds the lock (`python Tools/PerfLock.py --status`):

- The base: S0.12's tip as a detached worktree `.worktrees/reflections-base` (`git worktree add --detach`, then `git submodule
  update --init --recursive --depth 1` in it) built Release into `build/agent-reflections-base`; both removed when the stream
  lands. (Review correction: `git archive` leaves the vendored submodules out, so that base would not build.) Its build is this
  stream's own slot's work, never beside another build of the stream.
- `python Tools/PerfLock.py --label reflections --limit 90 -- python -I Tools/LookDev.py capture --build <bin> --shots
  "<shots>" --sizes 3840x2160 --quality high --repeat 5 --output <dir>`, base and branch alternately, three pairs at least, for
  `lookdev.sheet`, `lookdev.wet`, `lookdev.mirror`, `tp.scene` (High and Ultra at 3840x2160), then `tp.scene` and `lookdev.mirror`
  at 1920x1080 `--quality medium`.
- Read from `stages.csv`: medians of `GPU.Reflections`, `GPU.Opaque` and GPU busy, and the ratio `GPU.Reflections / GPU.Opaque`
  per picture (it survives other agents' load: plan 4.1). Each step's commit that changes a shader states the ratios.
- R1's landing commit states R1's full A/B; step 7's commit the `GPU.Opaque` ratio with sized lights (`lookdev.lantern.wet.sized`
  against `lookdev.lantern.wet`, 3840x2160 High: within the A/B's noise expected, a few instructions per light); R2's step 10
  states the conversion metrics.

## 7. Documentation

- **AGENTS.md**, Rendering digest, after the "Light keeps its energy" bullet. R1 lands "- Screen-space reflections march fixed
  steps, keep only validated crossings and fade only where rays leave the screen ([Reflections](Docs/DeveloperGuide.md#reflections))."
  (170 characters; with step 7 in R1's landing, "...leave the screen; a light's size widens only its highlight ([Reflections](...))."
  212). R2's final form: "- Screen-space reflections march fixed steps, keep only validated crossings, fade only where rays leave
  the screen; F0 comes from the IOR (1.5 stays 0.04); a light's size widens only its highlight
  ([Reflections](Docs/DeveloperGuide.md#reflections))." (249, within plan 4.1's 250; counted). Stated in full in DG ("Reflections"
  and "Lighting"); `Docs.AgentGuide` passes after merging main.
- **DG "Reflections"** (`DeveloperGuide.md:4888-5030`): the Spatial paragraph rewritten (the chosen pattern and kernel and why,
  the plane-aware weights, the crossing test, validation, `MaxCandidates`, the direction-aware fade with its length, back faces as
  decided); the Checkerboard paragraph's fade and what it keeps; the forward pass's use (SSR not occluded); the probe cache's size
  by row, its memory, the conversion's steps and budget (if spread), the irradiance level; the bake's `ProbeCapture` (no sun disk,
  why; the procedural sky's disk stays in its environment); box projection's parallax and when to turn it off; the open item that
  Checkerboard needs TAA.
- **DG "Lighting"**: F0 from the IOR (the lane, the exact 1.5); highlights of sized sources (the formula, the constant, why
  `alpha_min` is subtracted, why only D widens, the sun's `ReflectionParams.w`, point and spot lights' `SourceRadius` through the
  record's `Source` lane and `LightData.SpotAngles.w` on both light paths, the rule that a source size changes highlights only,
  never diffuse light, shadows or culling); the furnace cases. **DG "Render scene"**: the light record is 96 bytes. **DG "Fog and atmosphere"**: the storage scale and the rule
  that a reader multiplies by `EnvironmentParams.x`. **DG "Quality settings"**: the row and its memory line. **DG "Look-dev
  harness"**: `Metrics`, the formulas, `metrics.csv`, the Specular scene. **DG "Perf tests"**: the new metrics.
- **`Docs/Manual/Lighting.md`**: "Reflection probes" (177-198): the resolution follows Graphics Quality (sharper at High and
  Ultra, memory), the picture is prepared over a frame or two, Box Projection for rooms and courtyards with walls at the box's
  faces, off for open yards; "Screen-space reflections" (200-222): "Reflections fade toward the screen's edges" becomes "fade
  only where the reflected rays leave the screen", ambient occlusion no longer darkens them, objects in front of a mirror (as
  decided in step 9); "Materials that keep their energy" (224-237): IOR sets reflectance (water 1.33 reflects 2%); "Lights and
  their units" (8-53): the sun's Angular Diameter sizes its highlight, and a point or spot light's Source Radius (the bulb's or
  the flame's radius) sizes its highlights only, with 7b's authoring rule. **`Docs/Manual/Graphics.md`**: the new row (35-60's
  list).
- **Roadmap**: open item 877's "probes are baked, so a moving sun does not relight them" stays, "screen-space reflections cover a
  clear coat's base only" stays, and its "sized local lights (PCSS)" becomes "sized local lights' diffuse light and shadows
  (their highlights take `SourceRadius` since wave 8)"; 887: the result of D11's repeat test; a new open item: Checkerboard
  (Medium) still needs TAA; "Later": keyframed captures, sized point and spot lights' diffuse and shadows, hit-distance blur, the
  wet model, normal-map-aware filtering, anisotropy, coat SSR, probe slots by demand.
- **Release notes** (`Docs/Releases/0.8.0.md`, created by the first lander if absent):
  - Screen-space reflections at High and Ultra no longer show a fixed screen-door pattern, ghost columns beside thin objects or
    hatched edges; they fade only where the reflected rays leave the screen; ambient occlusion no longer darkens them (Medium too).
  - Probe bakes draw no sun disk into their captures (the sun lights the scene; a disk in a probe lit surfaces again, unshadowed).
  - A material's IOR sets its reflectance: water (1.33) reflects 2%, glass (1.5) 4% as before; imported `KHR_materials_ior` values
    count, without re-importing.
  - The sun's highlight takes the light's size (Directional Light **Angular Diameter**, stored as `ShadowSoftness`): Overcast's
    10-degree sun gives a soft glow instead of a pin dot; on mirror-smooth surfaces the midday sun's glint is the disk's size.
  - Point and spot lights have a **Source Radius** (default 0, as before): a lantern's or a fire's highlight on metal and wet
    ground takes the bulb's or flame's size, the same light spread wider; their diffuse light and shadows do not change.
  - The sky after sunset no longer shows bands, nor do its reflections.
  - At High and Ultra reflection probes are 256 texels per face: sharper on polished metal, 68 MiB of GPU memory per view that
    shows probes instead of 17. Upgrade row: Custom with **Reflection probe resolution** 128 keeps 0.7.0's.
  - Asset thumbnails re-render once.
- **Skills**: `strata-make-a-game` (metal sheets: bare metal, roughness 0.15-0.45 for painted scans; water: IOR 1.33, opaque,
  roughness 0.02-0.06; probes: Box Projection only where walls are the faces; lanterns and fires: `SourceRadius` the bulb's or
  flame's radius), `strata-add-component` needs nothing (no new component), `strata-editor-automation` (the Specular look-dev
  shots are not commands: nothing new).

## 8. Shared with the other streams, and merges (plan 4)

`reflections` lands R1 alone, then R2 first at landing 1 (or at landing 2). It merges main before each landing, resolves,
reruns everything and retakes ThirdPerson's pictures (taking main's binaries on conflict, then retaking).

| File | Ours | Theirs | Resolution |
| --- | --- | --- | --- |
| `SH/Scene/Forward.frag` | head (F0, `SurfaceData.F0`), the sun term and `LightSurface`'s new argument, `TransmitLight`'s F0 | `local-lights`: the forward skip in the light loop (:181-209); `radiance-cascades`: the diffuse gather; `volumetric`: the fog at the end; `showcase-engine`: the layer blend upstream of the head | regions do not overlap but in the light loop: our `sourceAlpha2` line and `LightSurface`'s new argument (D7c, :207); `local-lights` branches after landing 1, so it rebases onto ours and keeps its skip before our line |
| `SH/Include/IndirectLight` (S0.11) | `GatherIndirectSpecular` (D5) | `radiance-cascades`: `GatherIndirectDiffuse`; the open share into `SpecularOcclusion` after we merged (plan 4) | we land first; they multiply the open share into the occlusion D5 applies to the fallback only |
| `SH/Include/PBR.glsl` | `SurfaceData.F0`, the source lobe in `EvaluateLight`, `EvaluateLightWeighted` and `EvaluateClearcoat`, `GetSourceLobeAlpha2` | `showcase-engine` (surface function, upstream) | additive |
| `R/RenderLights.h/.cpp` | `MakeRecord`'s `sourceRadius`, `ComputeRecord`'s two calls (175-176, 183) | `physics-timing` owns the file (plan 4): its `ComputeRecord` places lights by the drawn transform at the same two calls, and changes `Sync` and `Validate` | R2 lands first at landing 1: physics-timing keeps our argument and replaces `world->Matrix` with its drawn transform in the same calls; its `Validate` compares whole records, so it covers `Source` unchanged |
| `R/SceneRenderData.h` (`LightRecord`, `LightData`), `SH/Include/Lights.glsl`, `SH/Scene/LightCompact.comp`, `SceneRenderer.cpp:2579` | the record's `Source` lane (96 bytes), `LightData.SpotAngles.w` | `local-lights` (tranche 2: growing light buffers, the record-to-slot table of the CPU path, `SceneRendererGPUDriven.cpp`) | additive: the lane is appended; `local-lights` reads `SpotAngles.zw` as before and keeps `.w` of `LightData` for the radius |
| `R/SceneRenderData.h`, `SH/Include/Scene.glsl` | `ReflectionParams.w`, `Absorption.w`'s meaning, `SpatialReflectionParameters::MaxCandidates`, `GetSunLobeAlpha2`, the probe constants | S0.11's `ReflectionParams.y/z` (GI), `radiance-cascades`' and `volumetric`'s structs and slots (35, 34), `showcase-engine`'s `MaterialData` growth | additive; `MaterialData` grows only after `Absorption`, keeping `.w` |
| `R/SceneRenderer.cpp/.h` | `SetQuality`'s probe line, `CollectLights`' writes of `ReflectionParams.w`, the CPU light path's `SpotAngles.w` (:2579), `ProbeCapture` after `UpdateEnvironment` (:3190), `:921`'s scale, the conversion queue, `GetTracedReflectionTexture` | `physics-timing` (`RenderLights`, drawn transforms), `radiance-cascades` (passes between AO and Reflections, `:3346`), `local-lights` (stats) | function-level; re-read `RenderFrame`'s order after merging (the cascades' pass sits between AO and our stage) |
| `R/RendererQuality.h/.cpp`, `Renderer.Quality` | `ReflectionProbeResolution` (field, `PresetRow`, traits line, bytes), `MaxCandidates` | `local-lights` (`LocalShadowViewsPerFrame`, `MaxShadowedLocalLights`), `radiance-cascades` (`GlobalIllumination`), `volumetric` | one line each, field order = traits order; `c_SectionVersion` + 1 per landing, the integrator renumbers |
| `R/SceneRendererAtmosphere.cpp`, `SH/IBL/Atmosphere*.comp` | the storage scale | `volumetric` (fog lighting from the probe's L0 or the volume; reads the tables) | `volumetric` multiplies table reads by `EnvironmentParams.x` as every reader does, which includes the scale |
| `R/RendererComponents.h/.cpp` | `ShadowSoftness`'s place, display name, tooltip, inactive reason; `SourceRadius` of `PointLight` and `SpotLight` | `volumetric` (`HeightFog`), `radiance-cascades` (`PostProcess` GI fields), `local-lights` (rows only) | different components; regenerate `ComponentSchema.json` after merging; `Feature.stscene`'s `Lamp` and `Spot` keep our values |
| `E/ThumbnailCache.h` | `c_Version` + 1 | `showcase-engine` (layers) | the later lander bumps above main's |
| `T/Renderer/FlickerMetrics.*` (one meter, D1), `T/Renderer/ReflectionMetrics.*` | `ReadDisplayFlickerFrame`; `MeasureNoise`, `SpecularLab` | `radiance-cascades` stage 0: the same `FlickerFrame`/`MeasureFlicker` by its D11, `ReadFlickerFrame` | whoever lands first creates `FlickerMetrics` with both readers; the second takes main's file and deletes its own copy; `ReflectionMetrics` is ours alone |
| `Tools/LookDev.py`, `Shots.json`, `LookDevScene.json` | `Metrics`, the Specular scene, `lookdev.*` shots | `showcase` (E4: the sample, `showcase.*` shots), `radiance-cascades` (GI shots) | append-only; the harness's code merged function by function |
| `StrataTests/Perf/Budgets.json`, `RendererPerfTests.cpp` | three new metric families | every stream's | separate entries and cases |
| `Docs/`, AGENTS.md, `FeatureTest.stproj` | ours | theirs | additive; `Docs.AgentGuide`, `StrataTests.FeatureTest` rerun |

Not touched, although the plan lists them as shares: `SH/Scene/DepthPrepass.frag` (the surface target is used as it is; the
resolve reads the prepass's geometric normals at SRV 26) and `ProbeBake.*` beyond a comment. Shadow caches: no input of how
casters are drawn changes (F0, the sun's lobe and the source radius are shading; `MaterialData`'s table version changes as for
any material edit; a light's record changes as for any light edit, and the local shadow views compare what they compared
before). `physics-timing`: `RenderLights` only (above). `animation-graph`, `editor-ux`: no shared file.

The showcase: R1 lands before its step 5 (dressing), so materials are tuned against the repaired SSR; step 7 (with R1 if verified,
else landing 1) gives its shots 2-4 and 6 the sized sun and the lantern's and fire's `SourceRadius`, which its "judged on
highlights first" needs; R2's F0 and probes come with landing 1 or later and the showcase's adoption pass retunes water (IOR 1.33)
and retakes (plan 4.2's table, whose `reflections` R2 row should name "sized highlights" beside "F0 of water, shot 6's sky,
sharper probes": openQuestions).

## 9. Risks

| Risk | Mitigation |
| --- | --- |
| Neither resolve candidate reaches 1% before step 4 | The noise holds hit-or-miss toggles that D4 removes; measured again after D4; the plane-aware weights are the common part both need, and the rule picks by numbers |
| Candidate B's 9 offsets leave holes in thin objects | D4 finds crossings between samples; the "whole" and thin-post cases guard it |
| Plane-aware weights make curved or normal-mapped metal patterned (weights no longer uniform) | `lookdev.tank` (curved) is a measured crop; the lobe weight already varies with normal maps today; judged on the tank and the bus at graybox |
| Validation erodes reflections at silhouettes by a pixel or two | The CPU-predicted mirror cube cases (`GPUSceneSSRTests.cpp:165-218`, :498-570) bound it; erosion against ghosts is the trade |
| Bisection per candidate raises the cost in clutter | `MaxCandidates` 2 and 3; the 100k-cube perf case is clutter; the 1.2 ms target |
| The direction-aware fade shows a hard line where rays exit at grazing angles | The fade spans 216 px along the ray at 4K; judged in the strafe (flicker) and on `lookdev.sheet` |
| Spatial SSR's two images (`Roadmap.md:887`) persist | Recorded in D11; the repair may change or hide it |
| The sun's lobe changes every sunlit glossy surface | Within 0.4% at roughness 0.18; 30% wider at 0.06; mirrors take the real disk's size; release notes; look-dev compare |
| IOR 1 now reflects nothing head-on (Schlick still rises at grazing) | Physically right for an index of 1; the tooltip says so |
| The storage scale clamps a bright aureole | The daytime test reads every table below the clamp at haze 4 and GoldenHour |
| The bands come from the transmittance table, not the per-lux tables | D8b's diagnosis first; optical depth storage designed; shot 6 holds at dusk otherwise |
| A sized highlight on a curved or bumpy surface widens more than the eye expects (the lobes add as variances, an approximation) | 7b's measure against the mirror image on flat wet ground and the sheet; the tank and the bus judged at graybox; authors set the bulb's real radius, never more |
| The light record grows from 80 to 96 bytes | Every reader sizes by `sizeof` (D7c); uploads follow the change log as before; 16 bytes a light, 4 KiB at 256 lights |
| `physics-timing` and `local-lights` meet the record in `RenderLights` and the light paths | Section 8's rows; R2 lands first at landing 1, `local-lights` branches after it |
| The bisection's sky rule (D4) turns a ray that grazes over a wall's top edge into a hit on the edge | Validation requires the high end's own pixel to be a surface; the whole-wall case checks the edge within 2 px; the comb crop (`P_3`, `P_4`) and the strafe judge it |
| SSR keeping the material's occlusion (D5) darkens scanned metal's reflections in its crevices | That is the probe's darkening too: the two now agree where SSR fades; maps without occlusion are unchanged (the factor is exactly 1) |
| 68 MiB per renderer at High in the editor (Scene, Game and capture renderers that show probes) | Allocated only with probes in view; the row's bytes in the Graphics Quality page; slots by demand in "Later" |
| A station change at 256 hitches | Measured; the work budget caps a frame at two 128 conversions' work |
| The p14 patch cannot be reproduced | The showcase audit's assets are in its folder; if it still does not appear, the step records the four-way isolation on the closest view and the authoring rule, and graybox's capture (plan 4.2 row 4) checks the showcase's own puddles |
| R1 slips past showcase step 5 | The R1 fallback rule (section 3); the showcase's graybox fallback (plan 4.2) |
| A Slang conversion slip looks like a reflection bug | Plan 4.1: check the signed look-dev difference against 0.7.0 first; `mul` only with a matrix, vector comparisons through `all`/`any` |

## 10. Open questions and the decisions recommended

1. **Which resolve.** Decided by D2's rule on the sheet; *default B* (sharper, no new taps).
2. **Back faces.** Decided by looking (D9); *default C* (crescents), because a filled silhouette shows the front's lighting where
   the back's belongs, and only the picture can say which reads worse.
3. **Sun disk in bakes.** *Decision: removed* (D6), whatever p14's cause: an unshadowed, box-projected 65,000-nit spot cannot be
   right anywhere, and the directional light already lights everything.
4. **Baked highlights.** *Removed only if the isolation finds them* (D6): removing them darkens metals in other probes' views, a
   change that needs a reason.
5. **The sun's lobe: add or subtract the floor.** *Decision: subtract* (`alpha_s^2 - alpha_min^2`): a mirror then shows the disk at
   its size exactly, and rougher surfaces change as little as the physics allows.
6. **`ShadowSoftness`'s name.** *Decision: keep the stored name*, display "Angular Diameter": a rename breaks stored scenes and
   scripts for a word.
7. **F0's home.** *Decision: `MaterialData.Absorption.w`*, computed on the CPU: exact for 1.5, no per-pixel division, no growth
   of the 144-byte layout `showcase-engine` will extend.
8. **The flicker's resolution.** *Decision: 1280x720 in the GPU test* (a Debug run under the validation layer stays short);
   the 4K look is judged by the noise metric and by eye.
9. **Probe slots by demand** (an array sized to the scene's probes, rounded up to 4). *Not this wave*: the plan states the whole
   allocation with its bytes; "Later", with the editor's memory as the reason to revisit.
10. **Spread conversions.** *Only if the measurement asks* (D10): the work budget is designed so that Medium is unchanged if it is
    built.
11. **A reflections debug view** (SSR confidence, misses, back faces) for agents. *Not this stream*: captures with SSR on and
    off and the GPU tests' readbacks answer the same questions; the lighting views' plumbing is `editor-ux`'s and
    `radiance-cascades`' this wave.
12. **The source radius's default.** *Decision: 0* (a point, as today): stored values never change, and an assumed bulb size would
    move every existing light's highlight. Authors set it (7b's rule); the showcase and the look-dev lanterns do.
13. **One flicker meter for the wave** (D1). *Recommended to the orchestrator:* `T/Renderer/FlickerMetrics.h/.cpp` holds
    radiance-cascades' D11 definition and both readers; whichever stream lands first creates it (expected: this one, through R1).
    Radiance-cascades' spec names the file `IndirectFlicker`; the orchestrator aligns the name in both specs.

## 11. Deliver in this order

1. **Step 1:** `ReflectionMetrics.h/.cpp` with `Renderer.ReflectionMetrics`; `FlickerMetrics.h/.cpp` (D1's one meter, with
   `Renderer.FlickerMetrics`) unless radiance-cascades landed it first; the `SpecularLab` builder and the strafe case; the
   Specular scene in `LookDevScene.json`; the R1 shots; `Metrics` in `LookDev.py` with its selftest; the base and 0.7.0 numbers
   in the commit.
2. **Step 2:** the plane-aware weights and SRV 26 in the resolve (one commit, pictures stated); the two candidates measured on the
   branch; the chosen one with the "screen door" case; DG lines.
3. **Step 3:** `ScreenRay.LeavesScreen`, `GetHitConfidence(…, ray)`, both call sites, the fade cases.
4. **Step 4:** the march, `RefineCrossing` with the sky rule, `MaxCandidates` through the struct, the push constants and
   `GetScreenReflectionSteps`; the thin-posts (vertical mirror) and whole-wall cases; the strafe's bounds.
5. **Step 5:** `GatherIndirectSpecular`'s two lines and `ScreenReflectionOcclusion`; the AO case.
6. **Step 6:** reproduction, isolation, the finding; `ProbeCapture` and its cases; `lookdev.patch`.
7. **R1 in review:** docs of 1-6, AGENTS.md's bullet, release-note lines, the A/B, ThirdPerson's pictures; review, ctest,
   `ci/wave8-reflections`. **Step 7 starts now**, beside the review's fixes: 7a (the sun's lobe, the `EvaluateLight` path), 7c
   (the record's lane, both light paths, the property, its cases and the feature test), then 7b's judgment with the strafe's
   highlight paths and the windowed look. The orchestrator lands R1, with step 7 if it is verified by then (section 3).
8. **Steps 8b, 8a, 10, 9** in R2 (section 3's order of value; step 7 first if it did not join R1), each with its tests, docs and
   shots.
9. **Step 11:** adoption, the repeat test, Roadmap, the bullet's final form; merge main; landing 1 (or 2).

## 12. First hour

1. Read plan 4.1, 4.2 (shots 2-4, the graybox table), 4.3, decisions.md (its "Highlights first" ruling orders R2), this spec with
   section 13, `specs/radiance-cascades.md` D11 (the flicker meter both streams share), `<spec>/audit.md`, `<show>/audit.md`; look at
   `<spec>/captures/crops/` (`grazing_bands`, `grazing_high_taa`, `crescents`, `crate_ao_on_wet`, `night_sky_band`,
   `overcast_ladder`) and `<show>/captures/p06_crop_ssr_edge.png`, `p14_barrel_high_probe.png`.
2. Confirm on main: S0.10's shader file names and syntax, S0.11's `IndirectInputs` and `GatherIndirectSpecular` and the
   `ReflectionParams.w` comment, the Slang look-dev baseline's location; adjust this spec's names to them.
3. `git worktree add .worktrees/reflections -b agent/reflections main`; submodules; configure `build/agent-reflections`; build
   Debug; run `StrataTests.exe --test-suite=GPU.SceneRenderer.SSR,GPU.SceneRenderer.ReflectionProbes,GPU.SceneRenderer.Lighting,GPU.Atmosphere`
   to see the base green on this machine.
4. `git worktree add --detach .worktrees/reflections-base <S0.12 tip>` with its submodules, build Release into
   `build/agent-reflections-base` (6.3); `python Tools/PerfLock.py --status`. Ask the orchestrator which stream creates
   `FlickerMetrics` (section 10, question 13).
5. Start step 1 with `lookdev.sheet` alone: the Specular scene's steps, the shot, `Metrics` for `Noise`; capture it on the base
   build at 3840x2160 High without AA and compare `N` with the audit's 5.0% and 3.9% before writing anything else.

## 13. Review corrections

The independent review (2026-11-02) checked this spec against the code at 76f28a21, plan.md, decisions.md, the specular and
showcase audits and the specs beside it (step0's S0.11, step0-slang, physics-timing, radiance-cascades). Most file:line claims
held; these were changed, and they win over anything left in the body (decisions.md).

**Against the rulings**

1. **Highlights lead R2, above the cut** (decisions.md "Highlights first"; plan 4.3 step 7). The first form put the source
   radius for highlights below the cut (its step 13), made small lights' highlights a no-code judgment at step 9 and the sun's
   size a part of materials. Now step 7 is highlights: 7a the sun's size, 7b lantern and fire highlights judged, 7c a source
   radius for highlights only, designed in full (`SourceRadius` on point and spot lights, default 0; the record's appended
   `Source` lane, 96 bytes; `LightData.SpotAngles.w` on both light paths, `SceneRenderer.cpp:2579` and `LightCompact.comp:93`;
   `GetSourceLobeAlpha2` in the light loop; tests, feature test, docs, merges). Step 8 is IOR and the night sky, step 9 back
   faces. Step 7 starts when R1 is in review, on its own branch, and joins R1's landing if verified; R2's order of value starts
   with it; the cut line and "Not the goal" follow the plan; every highlight is judged with no AA, FXAA and TAA in motion
   (strafe paths, a windowed look, an acceptance row).

**Defects in the design**

2. **The sun's lobe never reached opaque surfaces** (7a). `EvaluateLight` was to pass 0, but `LightSurface`'s opaque variants call
   `EvaluateLight` (`Forward.frag:103`). It now takes and forwards `sourceAlpha2`; a test case on an opaque dielectric guards it.
   The widened alpha now feeds D only; V keeps the surface's alpha.
3. **`ReflectionParams.w` would have held the previous frame's disk** (7a). `RenderFrame` assigns the vector whole at `:3019` and
   calls `CollectLights` at `:3050`; `CollectLights` now writes the component.
4. **The crossing test's bisection lost the crossing it was built for** (D4). With the sky counted as "in front", a first midpoint
   past a reflected wall's top edge moved the low end onto the sky and validation rejected the hit, leaving the comb. A sky
   midpoint now compares with the low end's surface; validation still requires a surface at the high end.
5. **`ProbeCapture` was placed in `UpdateEnvironment`**, which takes no options (`:866`); `RenderFrame` applies it after the call
   (`:3190`) (D6).
6. **The resolve's shared memory had no room for positions** (D2): five arrays of 16 bytes, 8.0 KB at 10x10, 11.5 KB at 12x12.
7. **SSR lost the material's own occlusion** (D5): screen AO double-counts SSR's hits, a material's occlusion map does not; it
   stays on the screen's light through `ScreenReflectionOcclusion` (exactly 1 without a map), so crevices do not glow where SSR is
   confident and darken where it fades.

**Tests that could not fail without their change**

8. Step 2's screen-door case reflected a uniform wall, where every offset's ray returns the same color: a picket fence now, with
   the base's pattern recorded at three times the bound or more.
9. Step 4's ghost case put a post over a mirror floor, where rays run parallel to it: a vertical mirror at a grazing view with
   posts in front (the audit's geometry), texels classified by the CPU's rays.
10. Step 6's bake case used the light's default 3 lux, whose disk stays under the 1e6-nit bound: 100,000 lux.
11. The flicker bound on 32 px blocks alone averages a screen-locked crawl away: the per-pixel `Mean` is bounded too, from
    step 1's measurements, and the commit states which bound 0.7.0 fails.
12. Step 3's case read a target no accessor exposed (`GetTracedReflectionTexture` added) and chose texels by screen side, while a
    floor's rays spread outward: texels are chosen by where the CPU's ray leaves, with a minimum count.
13. The feature test's Custom `ReflectionProbeResolution` was the default ("128"): "256" (DG "Quality settings").
14. 7a's sun-size case now fixes the sun's intensity and pre-exposure so the mirror's peak stays under `SanitizeHDR`'s clamp.

**Conflicts with the specs beside it**

15. **One flicker meter.** Radiance-cascades designs the same measure (its D11) with depth and normal reprojection; this spec's
    plane-only `FlickerMeter` is replaced by that definition in a shared `FlickerMetrics` file with a display reader
    (openQuestions: the file's name and owner).
16. **`RenderLights` belongs to physics-timing** and the light paths to local-lights' later work: section 8 gains their rows.

**Smaller corrections**

17. Adoption (D11) names every sample R2 changes (Open World's probes, the sun's lobe everywhere) and re-bakes probes that hold
    the clamped disk; the showcase's lantern and fire get `SourceRadius`.
18. D10 designs splitting a conversion step by face (`IBLParameters.FirstFace`) before giving up on 256, since level 1 at 256 is
    1.45 times a whole 128 conversion.
19. IOR 1.5's pictures: bit for bit expected, one code from a compiler's folding accepted and stated (D8a, 6.1).
20. The A/B base is a detached worktree with submodules (`git archive` omits them).
21. Shots carry their `Project` and `Scene`; the R1 fallback moves step 4 after step 7; D9's options are F and C, and no back-face
    case exists today (checked); the AGENTS.md bullet's final form is counted at 249 characters; DG "Lighting" and "Render
    scene", the Manual's "Lights and their units", the Roadmap's "sized local lights" open item, the release notes and the
    skills gain the source radius.
