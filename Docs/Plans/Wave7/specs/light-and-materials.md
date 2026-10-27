# Workstream `light-and-materials`: believable light (wave 7)

The owner (2026-10-26): "the current image quality is simply not great", no temporal upscaling ever ("The blurry mess that
current gen engine has (like unreal) is completely the opposite of what this engine is planning to be"), and "great
realistic graphics, which doesn't mean sharp or clinical", with the Fox Engine of Metal Gear Solid V as the baseline look.
Our reading of the part of that look this stream owns (plan.md section 1): real-world light units with a real sun to sky
ratio (deep, colored, never black shadows), measured-looking materials that keep their energy and do not sparkle, dense
grounded ambient occlusion that is clean without TAA, haze that gives distance and scale, and sky light that is blocked
where the sky is not seen. The plan (section 4.5) makes this the fourth stream to land: it tunes the samples' sky and sun
under the final tone curve and retakes their pictures. The decisions here are ours (the owner leaves what and when to us).

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `DG` is `Docs/DeveloperGuide.md`. Line numbers
are of bc6ee123 (release 0.6.0) unless a step-0 file is named; step 0 moves code (S0.3), so the implementing agent finds the
same code by its function names on step 0's tip.

The machine's rules for this stream (plan.md 4.1, 7):

- The Windows reference machine (i7-13700K, RTX 5090, 3840x2160 at 150%). The stream works in `.worktrees/light-and-materials`
  on `agent/light-and-materials`, branched from step 0's tip, built in `build/agent-light-and-materials`
  (`cmake --preset windows -B build/agent-light-and-materials`), never in another agent's directory.
- Before each commit: Debug ctest and every `GPU*` suite under the validation layer on NVIDIA (`StrataTests.GPU`), the
  feature test, `python Tools/CompatLint.py --changed-since <step 0 tip>` after a Debug build. CI green on
  `ci/wave7-light-and-materials` before the stream lands.
- No perf campaign and no `PerfStability.py` in the stream. Its own stages are A/B'd with step 0's look-dev harness
  (`Tools/LookDev.py`) under `Tools/PerfLock.py`, as information (section 7.1). New metrics enter `Budgets.json` as
  `Provisional` with their `Target`; the integration campaign measures them.
- Exact-pixel tests turn the new defaults off (`AddNeutralPostProcess`, which this stream extends; section 6).

## 1. Goal

At the end of the stream, in this order of priority (the order the owner notices it):

1. **A new project opens sunny with a real sun to sky ratio.** The Basic 3D template lights its scene with the physical
   atmosphere and a 120,000 lux sun at 40 degrees (about 95,000 lux on the ground after the air: the plan's "100,000 lux
   sun"), so flat ground in the sun gets at least 5 times the light of flat ground in shade (today 1.6); the procedural
   sky's default colors are scaled to a real ratio for new components; four sun and sky presets (Clear, Hazy, Overcast,
   Golden Hour) are one command (`lighting.applySkyPreset`, one undo step) and four buttons in the Sky section; the samples
   adopt balanced skies and suns, and the Open World's leaves get the color their artist meant.
2. **Rough metals keep their energy.** Specular light is compensated for GGX's missing multiple scattering from the BRDF
   table the renderer already has, for direct and image light, base and clear coat, and diffuse gets what specular leaves:
   a white-furnace sphere of roughness 1 reads its F0 times the sky (today 0.31 of it).
3. **Ambient occlusion that grounds every object without TAA.** GTAO by quality row (Low 1x4, Medium 2x6 as 0.6.0, High
   3x8, Ultra 4x12), a spatial denoiser that needs no history, a 1 m default radius kept within screen fractions, a thickness
   heuristic, and multi-bounce on albedo from a polynomial fitted by a committed `Tools/` script. No cross-hatch with FXAA
   or without anti-aliasing.
4. **Specular that holds at 4K.** Geometric specular anti-aliasing (roughness widened by the shading normal's variance over
   the pixel; derivatives only of the primitive's own interpolants and textures, so both draw paths stay pixel-equal), a
   specular horizon fade for normal-mapped reflections, and the prefiltered sky at 512 per face at High and Ultra.
5. **Distance fades into the atmosphere's haze.** Aerial perspective: a 32x32x32 froxel volume of in-scattered light and
   transmittance per frame, from the atmosphere's transmittance and multiple-scattering tables, applied by distance in the
   forward and transparent passes and combined with height fog; on by default for atmosphere skies.
6. **Lighting debug views** that let people and agents check all of the above: lighting only (white albedo), ambient
   occlusion, indirect light, luminance as EV100 false color, and albedo with values outside 0.03-0.85 marked, each with
   statistics in `viewport.capture`'s result (EV100 percentiles, albedo shares, the sun to sky ratio). They come first in
   the order of work (section 3) because every later step is tuned and verified with them.

Below the cut line, in this order, each specified to be implementable: reflections without TAA (spatial SSR and the
`Reflections` row), volumetric fog and light shafts without reprojection (the `Volumetric` row), the baked irradiance
volume (sky visibility and bounce light, `lighting.bakeVolume`), bent normals with cone specular occlusion and the probes'
"Fit to Room", and alpha cutouts that hold (coverage-preserving alpha mips).

Rules held throughout (plan.md 1, 4.1): native resolution only; nothing needs temporal accumulation to look right (every
screen-space effect here ships its own spatial filter and is judged with TAA, FXAA and no anti-aliasing); off costs nothing;
the CPU and GPU-driven paths stay pixel-equal and the shadow caches exact; Medium keeps 0.6.0's sample counts and
resolutions; `SceneRendererSpecification{}` stays Medium.

Not the goal: lightmaps, screen-space or ray-traced GI (plan.md 9), area lights, sized local lights (`sun-shadows` step 7
owns `SourceRadius`; its energy-normalized lobe widening is a function this stream reviews in `PBR.glsl`, section 9),
tone and exposure (`camera-and-tone`), shadows (`sun-shadows`), quality UI and the script API (`graphics-options`).

## 2. Where we start

### 2.1 Files

| File | Lines (bc6ee123) | Role | This stream |
| --- | --- | --- | --- |
| `SH/Scene/Forward.frag` | 374 | Forward shading, all variants | owns the indirect block (172-266), edits the surface setup (118-127), main (303-374) |
| `SH/Include/PBR.glsl` | 129 | GGX, Smith, Schlick, `SurfaceData`, `EvaluateLightWeighted`, clear coat | owns |
| `SH/Include/MaterialTextures.glsl` | 74 | Material sampling, `GetMappedNormal`, `GetShadingNormal` | owns |
| `SH/Scene/DepthPrepass.frag` | 92 | Prepass; `ST_PREPASS_SURFACE` writes SSR's surface target (59-69) | edits the surface roughness |
| `SH/IBL/BRDFLut.comp` | 51 | Split-sum table: x = scale, y = bias of F0; x + y is the lobe's albedo for F0 = 1 | reads (unchanged) |
| `SH/IBL/Prefilter.comp` | 61 | GGX prefiltered levels; roughness 0 copies (31-35) | sample counts per level |
| `SH/Effects/GTAO.comp` | 133 | Horizon-based AO, 2 slices x 6 steps, IGN noise turned by `TemporalParams.z` (66-67) | owns |
| `SH/Effects/AOBlur.comp` | 59 | 5x5 depth-aware blur | replaced by `AODenoise.comp` |
| `SH/Effects/ScreenSpaceReflections.comp` | 176 | Checkerboard trace into the TAA history | owns (below the cut) |
| `SH/Include/Atmosphere.glsl`, `AtmospherePass.glsl` | 154, 61 | Atmosphere constants, table mappings, pass bindings | owns |
| `SH/IBL/AtmosphereSkyView.comp`, `AtmosphereMultipleScattering.comp` | 66, 83 | Sky-view and multiple-scattering tables | factor the integrand (step 6) |
| `SH/Include/Fog.glsl` | 112 | Analytic height fog | owns |
| `SH/Include/Scene.glsl` | 371 | Frame constants (33-83), scene bindings | shared: SRV 29-31, claimed components (D1) |
| `R/SceneRenderer.cpp` / `.h` | 3,853 / 2,035 | Targets, environment, AO dispatch, `RenderFrame` (2676-3339) | shared |
| `R/SceneRendererAtmosphere.cpp` | 345 | Atmosphere tables and environment (`UpdateAtmosphere` 195-343) | owns (after S0.2) |
| `R/SceneRendererAmbientOcclusion.cpp` (step 0, S0.3) | n/a | `RenderAmbientOcclusion`, AO pipelines and targets | owns |
| `R/SceneRendererScreenReflections.cpp` | 157 | SSR resources and trace | owns |
| `R/SceneRenderData.h` | 508 | `FrameConstants` (38-88), `AOParameters` (361-367), `AtmosphereConstants` (339-350) | shared |
| `R/RendererComponents.h` / `.cpp` | 302 / 554 | `SkyLightComponent` (79-104), `HeightFogComponent` (110-131), `PostProcessComponent` (225-266) | shared (owns SkyLight, HeightFog, the Post Process AO fields) |
| `R/RendererQuality.h` / `.cpp` (step 0, S0.4) | n/a | `RendererQualitySettings`, presets one row per line, `Renderer.Quality` | adds rows |
| `R/Atmosphere.h` / `.cpp` | 68 / 88 | CPU mirror of the atmosphere (constants 18-42) | adds `ComputePathTransmittance` |
| `R/GPUProfiler.h` / `.cpp` | 195 / 377 | `RendererGPUStages` (20-36), registration (50-62) | adds `Aerial` after `Setup` (step 0's position) |
| `Strata/src/Strata/Asset/GltfImporter.cpp` / `.h` | 2,255 / 57 | glTF import; settings 76-121, base color factor 799, version 10 (`.h:39`) | one setting, version 11 |
| `StrataEditor/src/Editor/ProjectTemplates.cpp` | 152 | Basic 3D (`PopulateBasic3D` 41-76) | the sky and sun lines |
| `StrataEditor/src/Editor/EditorLightingCommands.cpp` | 802 | `lighting.bakeProbes`, `lighting.extractSun` (641-800) | adds `lighting.applySkyPreset` |
| `StrataEditor/src/Editor/EditorViewportCommands.cpp` | 1,149 | `debugView` schema (130-133), result (136-147), capture progress (236-259) | shared (debug views) |
| `StrataEditor/src/Editor/StreamingView.h` / `.cpp` | 144 / 317 | `ViewportDebugView` (26-33), names (`.cpp:33-55`), `Build`'s check (`.cpp:69`) | shared (appends) |
| `StrataEditor/src/Panels/InspectorPanel.cpp` | 1,434 | `DrawSky` (1344-1432) | the presets' buttons |
| `StrataEditor/src/Panels/ViewportPanel.cpp` | 958 | Debug view chip (560-596), legend (598-640) | shared (appends) |
| `Docs/Manual/Lighting.md` | 167 | Lighting manual | owns (but its Shadows section) |

### 2.2 Shading today

- `ShadeLitSurface` (`Forward.frag:115-270`) builds a `SurfaceData` (118-127; roughness `material.Roughness *
  metallicRoughness.g`, not clamped below until `PBR.glsl:62`), adds the sun with its shadow (131-140), the clustered
  punctual lights (143-170), then the indirect block: AO (172-178: `diffuseOcclusion = material AO * screen AO`), the
  reflected vector from the normal-mapped normal (179), probes (182-187), SSR (190-197), the split sum (201-246: one BRDF
  table fetch at 206, `specularColor = f0 * dfg.x + dfg.y` at 207, Lagarde's specular occlusion at 217, diffuse weighted
  by `1 - specularColor` at 226, 236, 243), the constant ambient without an environment (247-251), and the clear coat's
  image light (253-266, a second table fetch at 259).
- Direct light (`PBR.glsl:52-70`): `D * V * F` with `F = Schlick(VoH)` and diffuse `(1 - F) * (1 - metallic) * albedo / pi`;
  the clear coat (`99-112`) adds `D * V * coatFresnel` and passes `1 - coatFresnel` to the base. No multiple-scattering
  term anywhere: the lighting audit's furnace (L4, `lighting/cap/furnace_metal.png`) measured a silver sphere at 0.144
  against about 0.46 expected at roughness 1, and a numerical split sum with `BRDFLut.comp`'s formulas gives exactly that.
- The BRDF table is 128x128 RG16F with 512 samples (`SceneRenderer.cpp:65-66`), x indexed by NoV and y by perceptual
  roughness (`BRDFLut.comp:28-29`); `x + y` is the single-scattering lobe's directional albedo for F0 = 1 (the integral of
  `(1 - Fc) + Fc` times the visibility term, 45-48).
- Roughness is never widened by normal variation: `PBR.glsl:9` only clamps it to 0.045; `GetMappedNormal`
  (`MaterialTextures.glsl:47-65`) renormalizes; the cooker renormalizes every normal mip (clarity audit F6). The prepass's
  surface target stores the same roughness for SSR (`DepthPrepass.frag:59-69`).
- The reflected vector can dip below the geometric surface where normal maps tilt the normal (lighting audit L11): nothing
  fades it.

### 2.3 Ambient occlusion today

- `RenderAmbientOcclusion` (`SceneRenderer.cpp:1330-1349`; step 0 moves it into `R/SceneRendererAmbientOcclusion.cpp`)
  pushes `AOParameters { radius clamped to [0.01, 10], intensity [0, 8], 2, 6 }` (`SceneRenderData.h:361-367`) and runs
  GTAO then the blur at full resolution into `m_AORawTexture` and `m_AOTexture` (R16F where the device stores it, else R32F;
  `SceneRenderer.cpp:44-51, 2058-2089`), in `GPU.AO`, only when `PostProcessComponent::AmbientOcclusion` (3103-3109).
- `GTAO.comp`: the world radius projected to pixels, capped at 256 px, below 1 px no AO (55-64); a noise of interleaved
  gradient noise turned by `TemporalParams.z` (66-67: under TAA every frame turns, otherwise fixed); slices x steps per side
  with a quadratic step distribution snapped to pixel centers (96-119); a falloff from 0.6 of the radius (69, 116); the
  cosine-weighted arc integral (122-128); `pow(ao, Intensity)` (132). No thickness heuristic, no multi-bounce.
- `AOBlur.comp`: a 5x5 depth-aware box (44-58), no normal weight. With FXAA or no AA the slice noise survives as a
  cross-hatch (lighting audit L5, `lighting/cap/ao_fxaa_zoom_sphere.png`; the shadows audit found the hatching inside the
  robot's shadow is GTAO's, `shadows/captures/tp_zoom_charshadow_x4_noaa.png`).
- Cost (architecture audit, `architecture/measurements.md`): `GPU.AO` 0.35-0.79 ms at 3840x2160 (ThirdPerson's camera
  0.79 ms, the largest stage), 0.09-0.16 ms at 1920x1080.
- `Forward.frag:176` reads the AO texture for opaque surfaces only; it darkens indirect light only (correct).
- `PostProcessComponent::AmbientOcclusionRadius` defaults to 0.6 (`RendererComponents.h:240`); every 3D sample stores 0.6
  (`Samples/*/Assets/Scenes/*.stscene`, `Samples/OpenWorld/Assets/Worlds/Island.stscene`).

### 2.4 Sky, atmosphere, fog and the environment today

- `UpdateEnvironment` (`SceneRenderer.cpp:836-937`) picks the first active `SkyLightComponent`; the atmosphere path
  (`SceneRendererAtmosphere.cpp:195-343`) computes the transmittance (256x64) and multiple-scattering (32x32) tables when
  settings change, the sky-view table (192x108, per lux of sun) when the sun or the camera's 250 m band changes, and the
  environment cube double-buffered (264-316); it multiplies the sun's light by the CPU's transmittance (320-342).
  `EnvironmentParams.x` is the sky's `Intensity` times the sun's illuminance above the atmosphere (`SceneRenderer.cpp:886, 929`).
- S0.2 (step 0) fixes the black environment after a sky-source switch (lighting audit L6) in this file, with a GPU
  regression test. This stream depends on that fix and builds on the fixed file; it does not re-specify it.
- Environment maps (`CreateEnvironmentMaps`, 666-690): a 512 cube with mips, a 32 irradiance cube, a 256 prefiltered
  cube of 6 levels (roughness 0, 0.2 ... 1; `c_PrefilteredSize` at line 60). `FilterEnvironmentStep` (789-817) prefilters
  each level with 512 samples. Maps computed from an environment map texture are shared between renderers by texture
  (`FindSharedEnvironment`, 726-740).
- Fog (`Fog.glsl`): analytic height fog, applied in `Forward.frag:345-347` and `Skybox.frag`; under the atmosphere its sky
  light is the sky-view table at the horizon (`Fog.glsl:36-43`). No aerial perspective: distant haze comes only from the
  sky behind (lighting audit L9). The atmosphere's Mie scattering is Earth's clear-day value (`Atmosphere.h:28`,
  3.996e-3 per km), so at real scale a 1 km view barely hazes (unverified on screen; the Open World is 768 m across).
- Defaults: `SkyLightComponent` procedural colors zenith (0.18, 0.32, 0.62), horizon (0.66, 0.74, 0.84), ground
  (0.22, 0.20, 0.18) nits (`RendererComponents.h:91-93`) against a 3 lux default sun (`:24`): a 1.6 sun to sky ratio on a
  horizontal surface (lighting audit L2, `lighting/cap/meas_room_shadow_noAO.png`); the physical atmosphere gives 6.9
  (`meas_room_shadow_atmo.png`).

### 2.5 Debug views, captures and commands today

- `ViewportDebugView` {None, Streaming, LOD, Mip, Motion} (`StreamingView.h:26-37`): the first three are entity tints built
  on the CPU with statistics (`StreamingViewStats`); Motion sets `SceneRenderOptions::ShowMotionVectors`
  (`ViewportRenderer.cpp:189`), drawn by `DrawMotionView` (`SceneRenderer.cpp:3460-3515`, `SH/Overlay/MotionView.frag`)
  first among the overlays (`RenderOverlays`, 3595-).
- `viewport.capture` and `viewport.setSettings` take `debugView` "none" | "streaming" | "lod" | "mip" | "motion"
  (`DebugViewSchema`, `EditorViewportCommands.cpp:130-133`); the capture's result describes it (`DescribeDebugView`,
  136-147); captures re-render while things settle, each wait bounded (`CaptureProgress`, 236-259).
- `lighting.bakeProbes` and `lighting.extractSun` (`EditorLightingCommands.cpp:641-800`) are the lighting commands; the
  `lighting.` prefix is in the `scene` MCP toolset (`StrataCLI/src/CLI/McpToolsets.cpp:44`).
- The Sky section of the Inspector (`InspectorPanel.cpp:1344-1432`) shows the sky's properties and buttons (Add
  Procedural Sky, Extract Sun, Add Height Fog, Bake Reflection Probes).

### 2.6 The template and the samples today

- Basic 3D (`ProjectTemplates.cpp:41-76`): a 3 lux sun from `GetPreviewSunDirection` (55 degrees up, azimuth 20), a
  procedural sky with the defaults, a Post Process with ACES, automatic exposure and `Exposure` 1, TAA. `Editor.Templates`
  checks it (`StrataTests/src/Editor/EditorTemplateTests.cpp:74-130`, the sun at 92-99, the sky at 101-104).
- Physics Playground (`Samples/PhysicsPlayground/Assets/Scenes/Main.stscene`): a 3 lux sun, softness 1, the procedural
  defaults stored, `AutoExposureMaxEV` 12, `Exposure` 1, AO radius 0.6.
- Open World (`Samples/OpenWorld/Assets/Worlds/Island.stscene`): the atmosphere, a sun of `Color` [0.85, 1, 1] (cyan) at
  3 lux above the atmosphere driven by `DayNight` (`Scripts/DayNight.h`: it sets only `Intensity`), AO radius 0.6. Its 35
  Kenney Nature models (`Assets/Models/Nature/*.glb`) carry sRGB bytes in their glTF base color factors: `tree_default.glb`'s
  leaves are [0.1608, 0.7882, 0.6706], #29C9AB written as linear (lighting audit L7, `lighting/cap/leaves_cubes.png`);
  `GltfImporter.cpp:799` reads factors as linear, as glTF specifies.
- Third-Person Adventure: an HDR sky with its extracted sun (a content choice, kept), AO radius 0.6 in `MainMenu` and `Course`.
- `StrataRuntime.OpenWorldWindowedImage` requires `--dominant green 5` (`StrataTests/CMakeLists.txt:821`).

### 2.7 What step 0 gives this stream (the API assumed; adapt names to step 0 as built)

The step-0 spec is written beside this one; where its names differ, the step-0 names win and this spec's meaning holds.

- **S0.2**: the atmosphere's environment is refreshed after a source switch, a sun activation and an intensity change from
  0, with a GPU regression test. Dependency only.
- **S0.3**: `R/SceneRendererPost.cpp` (`RenderPost`: exposure, bloom, tone mapping, FXAA; owned by `camera-and-tone`) and
  `R/SceneRendererAmbientOcclusion.cpp` (`RenderAmbientOcclusion`, the AO pipelines, targets and binding sets; owned here).
- **S0.4**: `R/RendererQuality.h` with `RendererQualityPreset { Low = 0, Medium = 1, High = 2, Ultra = 3, Custom = 4 }`,
  `RendererQualitySettings` (one field per knob), the preset table written one knob per line with its four preset values on
  that line, `SceneRendererSpecification::Quality` (default Medium), `SceneRenderer::SetQuality`, the named profiles
  `SceneRendererSpecification::ForThumbnails()` and `ForBake()`, and the `Renderer.Quality` settings section (version 1,
  one field per knob, read by its section's upgrade). This stream adds rows the same way (D4, D6, D8, D9).
  Step 0's spec (`specs/step0.md` 7.1-7.8, 13) names the touch points of a row: a field of `RendererQualitySettings`, a
  typed line `constexpr PresetRow<T> c_<Knob>Row = { Low, Medium, High, Ultra };` in `RendererQuality.cpp`, one line in
  each of `GetPreset`, `MakeSection`, `FromSectionValue`, `ToSectionValue`, `Clamp` and `HasSameKnobs`, a field of the
  section (default: High's value), the knob's key in `DescribeRenderStats`' `"quality"` object
  (`E/EditorStateCommands.cpp:206-250`), a non-default Custom value in the feature project's `Renderer.Quality`, and
  `c_SectionVersion` + 1 at landing. `graphics-options` (lands first) adds a `RowTraits` line per field
  (`specs/graphics-options.md` D1: `FollowsPreset`, `GPUStage`, `MemoryBytes`); this stream's rows set theirs (D4, D6).
  `ForBake()` keeps 0.6.0's values on purpose (step 0's decision Q3: probe faces are 256 pixels and `GPU.Editor.Lighting`'s
  baked values stay), so this stream's rows take their Medium values there.
- **S0.5 stages**: step 0's table registers `GPU.Aerial` after `GPU.Setup` and `GPU.Volumetric` after `GPU.Shadows`
  (`specs/step0.md` 8), with the three hard-coded lists taking the same positions.
- **S0.5**: scene SRV slots assigned in a comment of `SH/Include/Scene.glsl`: 28 to `sun-shadows`, 29-31 to this stream.
- **S0.6**: `Tools/LookDev.py` with `Tools/LookDev/LookDevScene.json` (a closed room with one window, a white-furnace row of
  spheres over roughness and metalness, thin casters, a 5 cm plate, an alpha-masked foliage card, a sky gradient) and
  `Shots.json`, its CTest, and the 0.6.0 and step-0 baselines.

## 3. Order and the cut line

Steps run in order; each lands working, tested and documented before the next starts (plan.md 4.1).

1. **Lighting debug views** (D2): every later step is tuned and checked with them; they cost nothing when off.
2. **Energy compensation** (D3): small, every scene benefits, and the furnace test needs only step 1's numbers.
3. **Ambient occlusion without TAA** (D4): the largest visible change after the sky, and a quality row.
4. **Balanced light by default** (D5): the template, the procedural defaults, the presets command and buttons, the glTF
   setting and the samples' adoption. Last of the "look" steps on purpose: steps 2 and 3 change what the presets are judged
   against.
5. **Specular that holds at 4K** (D6): specular anti-aliasing, the horizon fade, the `EnvironmentResolution` row.
6. **Aerial perspective** (D7), then the stream's DG "Lighting" pass, `Lighting.md`, the AGENTS.md bullet and the samples'
   pictures. **The core is complete here.**

**Cut line.** Below it, each step is done only when everything above landed; what is not done moves to the Roadmap's
"Later" when the wave lands:

7. Reflections without TAA (D8).
8. Volumetric fog and light shafts (D9).
9. The baked irradiance volume (D10).
10. Bent normals and cone specular occlusion; reflection probes' Fit to Room (D11).
11. Alpha cutouts that hold (D12).

## 4. Where each setting lives

| Feature | Kind | Settings | Inspector | Agents and scripts |
| --- | --- | --- | --- | --- |
| Lighting debug views | renderer option, viewport setting | `SceneRenderOptions::LightingView`; `debugView` "lighting", "ao", "indirect", "luminance", "albedo" | the Viewport's debug chip and legend | `viewport.setSettings`, `viewport.capture {debugView}` with statistics |
| Energy compensation | shading | none (always on) | none | none |
| AO quality | quality row | `Renderer.Quality.AmbientOcclusionQuality` (Low, Medium, High, Ultra) | Graphics Quality page (generic) | `project.setSettings`; players through `graphics-options`' named options |
| AO look | Post Process | `AmbientOcclusionRadius` (default 1.0), `AmbientOcclusionMultiBounce` (new, default on) | generic | `component.set`, `SetProperty` |
| Sky and sun presets | command | `lighting.applySkyPreset {preset, sky, light, sunElevation, sunAzimuth}` | four buttons in the Sky section | the command (no script API) |
| Procedural sky defaults | component defaults | `SkyLightComponent` colors (new components only) | generic | n/a |
| glTF sRGB factors | import setting | `BaseColorFactorsAreSRGB` (default off) | asset inspector (generic import settings) | `asset.setImportSettings` |
| Specular anti-aliasing | renderer specification | `SceneRendererSpecification::SpecularAntiAliasing` (default on; tests and comparisons only) | none | none |
| Prefiltered sky size | quality row | `Renderer.Quality.EnvironmentResolution` (256, 512) | Graphics Quality page | as above |
| Aerial perspective | SkyLight (atmosphere) | `AerialPerspective` (default on), `AerialPerspectiveScale` (default 1) | generic, inactive without the atmosphere | `component.set`, `SetProperty` |
| Below the cut | | `Reflections` row; `Volumetric` row and HeightFog's volumetric properties; `IrradianceVolume` component and `lighting.bakeVolume`; `lighting.fitProbe`; `AlphaCoverageCutoff` import setting | | |

The look stays on components; quality rows scale what a scene asks for (plan.md 5). No new script host function: scripts
reach every new property through `Entity::GetProperty`/`SetProperty`, and the quality rows through `graphics-options`'
named options without ABI changes.

## 5. Design

### D1 Rules this stream keeps, and the frame constants it claims

**Off costs nothing.** A feature that is off allocates no target, records no pass, binds a dummy where a layout needs one,
and in `Forward.frag` sits behind a uniform branch on a frame constant. Each step proves it with an exact-pixel comparison
of "off" against a scene that never had the feature (texture streaming off, `AddNeutralPostProcess`), on both paths.

**Frame constants.** Above the cut, this stream adds no `vec4` to `FrameConstants`: it uses components that are free at
bc6ee123 and belong to its features, so no other stream's appended block moves. It claims them in comments of both
declarations (`SceneRenderData.h:65-66, 81`; `Scene.glsl:62-63, 78`):

| Component | Meaning | Step |
| --- | --- | --- |
| `AOParams.y` | 1 when ambient occlusion's multi-bounce applies (`PostProcessComponent::AmbientOcclusionMultiBounce`) | 3 |
| `MaterialParams.y` | the lighting debug view (`SceneLightingView` as a float; 0 = none) | 1 |
| `MaterialParams.z` | specular anti-aliasing's strength (1 with `SceneRendererSpecification::SpecularAntiAliasing`, else 0) | 5 |
| `FogParams.z` | aerial perspective's far distance in meters (0: off) | 6 |
| `FogParams.w` | aerial perspective's distance scale (`SkyLightComponent::AerialPerspectiveScale`) | 6 |
| `MaterialParams.w` | 1 in irradiance-volume bake frames (back faces write alpha 0; below the cut) | 9 |

**Write order of `FogParams`.** `SetHeightFog` (`SceneRenderer.cpp:1281-1318`) assigns the whole vector twice
(`frame.FogParams = glm::vec4(0.0f)` at 1286, `glm::vec4(1.0f, sunScattering * ..., 0.0f, 0.0f)` at 1316), and
`RenderFrame` calls it right after `UpdateEnvironment` (2980-2981). So aerial perspective's z and w are **not** written in
`UpdateEnvironment`: `SetHeightFog`'s two assignments become writes of `.x` and `.y` only, and a new
`void SetAerialPerspective(const SceneCamera& camera, RenderData::FrameConstants& frame)` called right after `SetHeightFog`
writes `.z` and `.w` (both 0 when the frame has no aerial perspective). `UpdateEnvironment` only decides `m_AerialFrame`.

Below the cut, the first of steps 7-9 that lands appends one `vec4 VolumeParams` at the end of `FrameConstants` (after
whatever the other streams appended; the `static_assert` at `SceneRenderData.h:88` and `Scene.glsl` change together); the
later ones fill their component: x volumetric fog's far distance (0: off), y its depth slices, z irradiance volumes in the
frame's list, w 1 when the frame's reflections are spatial.

**Slots.** Scene SRV 29 is the aerial perspective volume (step 6), 30 and 31 the irradiance volume's atlas and records
(step 9). `sun-shadows` takes SRV 32 (its cascades' depth ranges) and 33 (its blue-noise tile) above its assignment
(`specs/sun-shadows.md` D1), and lands before this stream; volumetric fog (step 8) therefore takes the next free slot when
it lands (34 if those two landed), recorded in `Scene.glsl`'s slot comment, which is the arbiter. New passes take
parameters by push constants (slot `b1` where the layout has a constant buffer at `b0`) or buffers of their own.

**Both paths.** Everything in this stream is shading, a screen-space pass after the prepass, or a per-frame volume: both
draw paths run the same shaders on the same targets. Derivatives are taken only of values computed from the primitive's own
interpolants and textures, in control flow that is uniform across a quad (branches only on flat, per-primitive values such
as material flags), before any `discard` (D6). Each step adds a scene to `GPU.SceneRenderer.GPUDriven` (section 6).

**Shadow caches.** Nothing here changes how casters are drawn into the cascades or the local atlas: no new cache input.
Coverage-preserving alpha mips (step 11) change textures, which the caches already compare through the texture table's
entries while materials are alpha-masked (`CacheShadowCascades` comment, `SceneRenderer.h:196-202`).

### D2 Lighting debug views and their statistics (step 1)

**Renderer API** (`R/SceneRenderer.h`, beside `SceneRenderOptions`):

```cpp
// What a lighting debug view shows instead of the lit image (SceneRenderOptions::LightingView). The values are the order of
// ViewportDebugView's names; nothing stores them.
enum class SceneLightingView : uint8_t
{
	None = 0,
	Lighting,         // Every lit surface shaded white (albedo 1, metallic and roughness kept, no emission), through the scene's exposure
	AmbientOcclusion, // The occlusion the forward pass applies to indirect light (material x screen), as gray
	Indirect,         // Only the indirect light (sky, probes, screen-space reflections), through the scene's exposure
	Luminance,        // The scene's luminance as EV100 bands (log2 nits + 3)
	Albedo            // The base color, with values outside [0.03, 0.85] marked
};

// Display colors of the value views (sRGB; alpha unused). Not saved: the editor sets them from its theme.
struct LightingViewColors
{
	std::array<glm::vec4, 12> Luminance; // EV100 bands from LuminanceFirstEV100, one EV wide
	float LuminanceFirstEV100 = 0.0f;    // The first band's lower edge; below it the first color, above the last the last
	glm::vec4 AlbedoLow;                 // Over albedo below 0.03 (blended at 0.7)
	glm::vec4 AlbedoHigh;                // Over albedo above 0.85 (diagonal stripes, 0.7)
	glm::vec4 Background;                // Pixels without a surface in the occlusion and albedo views
};
```

`SceneRenderOptions` gains `SceneLightingView LightingView = SceneLightingView::None;` and `LightingViewColors
LightingViewColors;` (defaults: an even ramp from dark blue through green and yellow to white, a blue low mark, a red high
mark, mid gray).

**Statistics** (`R/SceneRenderer.h`):

```cpp
// What a lighting view measured in one frame, frames later (a readback, never waited for).
struct LightingViewStats
{
	SceneLightingView View = SceneLightingView::None;
	uint64_t Render = 0;          // The renderer's frame (its render count) the statistics belong to
	float SurfaceShare = 0.0f;    // Share of the image's pixels that show a surface (depth > 0)
	// Lighting, Indirect, Luminance: EV100 of the surfaces' pixels at the 5th, 50th and 95th percentile, and of the
	// background's (the sky or the clear color); NaN where there are no such pixels.
	glm::vec3 SurfaceEV100 = glm::vec3(std::numeric_limits<float>::quiet_NaN());
	float BackgroundEV100 = std::numeric_limits<float>::quiet_NaN();
	// AmbientOcclusion and Albedo (the albedo's luminance): the surfaces' median and 5th percentile; Albedo: the shares of
	// surface pixels below 0.03 and above 0.85.
	float ValueMedian = std::numeric_limits<float>::quiet_NaN();
	float ValueP05 = std::numeric_limits<float>::quiet_NaN();
	float BelowRangeShare = 0.0f;
	float AboveRangeShare = 0.0f;
	// Every view: illuminance in lux on a horizontal surface from the directional light at the ground (after the atmosphere,
	// before shadows) and from the sky (pi times the irradiance map at +Y times the sky's intensity, or pi times the constant
	// ambient without an environment), and their ratio (0 without either).
	float SunIlluminance = 0.0f;
	float SkyIlluminance = 0.0f;
	float SunToSkyRatio = 0.0f;
};
std::optional<LightingViewStats> GetLightingViewStats() const; // The newest that arrived; empty without a view
```

`SceneRendererStats` gains `bool LightingViewPending` (a view is on and no statistics of it, measured since it was chosen,
arrived yet).

**GPU data** (`R/SceneRenderData.h`):

```cpp
constexpr uint32_t c_LightingViewEVBins = 128;     // Quarter-EV bins of EV100 ...
constexpr float c_LightingViewMinEV100 = -8.0f;    // ... from -8 to 24
constexpr float c_LightingViewBinsPerEV = 4.0f;
constexpr uint32_t c_LightingViewValueBins = 64;   // Bins of 1/64 over [0, 1] (occlusion, albedo luminance)
constexpr float c_AlbedoLow = 0.03f;
constexpr float c_AlbedoHigh = 0.85f;
// Effects/LightingViewStats.comp's output (cleared before each dispatch).
struct LightingViewStatsData
{
	uint32_t SurfaceEV[c_LightingViewEVBins];
	uint32_t BackgroundEV[c_LightingViewEVBins];
	uint32_t Value[c_LightingViewValueBins];
	uint32_t SurfacePixels;
	uint32_t BackgroundPixels;
	uint32_t BelowRange;
	uint32_t AboveRange;
	glm::vec4 SkyIrradiance; // rgb: the irradiance map at +Y times EnvironmentParams.x (irradiance over pi), or the ambient color
};
static_assert(sizeof(LightingViewStatsData) == (2 * c_LightingViewEVBins + c_LightingViewValueBins + 4) * 4 + 16);
// Push constants of Overlay/LightingView.frag and Effects/LightingViewStats.comp (b1).
struct LightingViewParameters
{
	uint32_t View;               // SceneLightingView
	float InversePreExposure;    // Of the frame (TemporalParams.y): the HDR target holds luminance times it
	float LuminanceFirstEV100;
	uint32_t Padding;
	uint32_t LuminanceColors[12]; // RGBA8 (sRGB), packed
	uint32_t AlbedoLow;
	uint32_t AlbedoHigh;
	uint32_t Background;
	uint32_t Padding1;
};
static_assert(sizeof(LightingViewParameters) == 80);
```

**Forward.frag.** Constants `ST_LIGHTING_VIEW_LIGHTING` 1u ... `ST_LIGHTING_VIEW_ALBEDO` 5u in `Scene.glsl` (mirroring the
enum). `main()` reads `uint lightingView = uint(u_Frame.MaterialParams.y + 0.5)` once (uniform):

- Lighting: the albedo passed to `ShadeLitSurface` is `vec3(1.0)` and the emission `vec3(0.0)`; unlit materials draw as
  they are. Transparent and glass variants likewise.
- Indirect: in `ShadeLitSurface`, after the punctual loop (after line 170) and before the indirect block,
  `if (lightingView == ST_LIGHTING_VIEW_INDIRECT) color = vec3(0.0);` and the emission is left out. This leaves the sun
  term's lines (131-140, `sun-shadows`') untouched. Unlit materials draw black.
- AmbientOcclusion: `ShadeLitSurface` returns `vec3(diffuseOcclusion)` (material times screen occlusion, without
  multi-bounce) right after line 178; unlit materials 1.
- Albedo: `main()` writes `baseColor.rgb` (base color factor times map, linear) for lit and unlit materials alike.
- The value views (AmbientOcclusion, Albedo) skip aerial perspective and fog and write their value without the
  pre-exposure (`o_Color = vec4(SanitizeHDR(value), alpha)`): a pre-exposure of 2^-19 (a sunlit scene) would make an
  albedo of 0.03 subnormal in half floats. The glass variant adds no transmitted light in them. Lighting and Indirect write
  as always (pre-exposed, fogged), so exposure and tone mapping show them.
- The LOD fade's discard and the masked discard stay where they are (after shading), so the prepass's EQUAL depth holds.

**The overlay** `SH/Overlay/LightingView.frag` (bindings: CBV 0 frame constants, SRV 0 the frame's HDR target
`m_HDRTexture`, SRV 1 depth; push constants `LightingViewParameters` at b1), drawn by `SceneRenderer::DrawLightingView`
into the output framebuffer without blending, without depth, first among the overlays (with the motion view, which the
editor never shows at the same time; if both are asked, the lighting view wins and the motion view is skipped with no
error). It runs only for AmbientOcclusion, Albedo and Luminance:

- AmbientOcclusion: `LinearToSRGB(vec3(hdr.r))`; background (depth 0) the background color.
- Albedo: `LinearToSRGB(albedo)`, then where the albedo's luminance (Rec. 709 weights) is below `c_AlbedoLow` mixed 0.7
  toward `AlbedoLow`, above `c_AlbedoHigh` mixed 0.7 toward `AlbedoHigh` on pixels where `((x + y) / 4) % 2 == 0`
  (stripes, so a marked bright surface is not mistaken for a bright one); background the background color.
- Luminance: `EV100 = log2(max(luminance(hdr.rgb) * InversePreExposure, 2^-10)) + 3`, band `clamp(floor(EV100 -
  LuminanceFirstEV100), 0, 11)`, its color; the background is included (the sky's EV is information).

It reads the frame's own HDR target, not the TAA's resolved image, so a value view never mixes with history.

**The statistics pass** `SH/Effects/LightingViewStats.comp` (16x16 groups; layout: CBV 0 frame constants, SRV 0 HDR,
SRV 1 depth, SRV 2 the irradiance cube (black cube without an environment), sampler 8 linear clamp, UAV 0 the stats buffer
(`StructuredBuffer_UAV`, uint words), push constants `LightingViewParameters` at b1): each thread classifies its pixel
(surface if depth > 0), adds it to a group-shared histogram (EV bins for Lighting, Indirect and Luminance; value bins,
below and above counts for AmbientOcclusion and Albedo), and the group adds its histogram to the buffer with `atomicAdd`.
Thread (0, 0) of group (0, 0) writes `SkyIrradiance`: `textureLod(irradiance, RotateEnvironmentDirection(+Y), 0) *
EnvironmentParams.x` when `EnvironmentParams.w > 0`, else `AmbientColor.rgb`. Before the dispatch the buffer is cleared
(`clearBufferUInt`), after it copied into the next of a ring of three `BufferReadback`s (as `m_ExposureReadbacks`), each
remembering its render count and view. `ResolveGPUStats` (`SceneRenderer.cpp:2644`) polls them each frame and turns the
newest into `LightingViewStats` on the CPU: percentiles by walking the cumulative histogram (linear within a bin), shares
by counts, `SunIlluminance` from the frame's `DirectionalLightColor` (after the atmosphere) luminance times
`max(0, -DirectionalLightDirection.y)` (kept from the frame that recorded it), `SkyIlluminance = pi * luminance(SkyIrradiance)`.

**Where it runs.** `HasOverlays(options)` is true for any `LightingView` other than None, so the output texture exists.
In `RenderFrame`'s overlay section (`SceneRenderer.cpp:3295-3320`), before `RenderOverlays`: the statistics dispatch and,
for the three value views, the overlay draw, in `GPU.Overlays`. Nothing is created until the first frame with a view; the
two pipelines then live as long as the renderer (the capture renderer releases its targets after every picture, and
pipelines would otherwise be compiled again for each capture), while the stats buffer and its readbacks (about 4 KB) are
released with the targets by `SetViewportSize(0, 0)`.

**History and exposure.** A change of `LightingView` between frames calls `ResetHistory()` (TAA must not blend a view's
image with the lit one). Value views write unexposed values that the exposure meters, so while a value view is on the
automatic exposure is held, exactly so:
- `ChoosePreExposure` (`SceneRenderer.cpp:1479-1537`) gains a parameter `bool hold` (true in value-view frames): when set,
  after the reset and manual branches it returns before looking at readbacks, with `m_PreExposure` unchanged and
  `m_PreExposureSettled = true` (captures then never wait on exposure in a value view: `EditorViewportCommands.cpp:650`).
- Value-view frames record no exposure readback (`SubmitExposureReadback` is not called for them), and both entering and
  leaving a value view increment `m_PreExposureEpoch` (`SceneRenderer.h:1652`), so no readback recorded around the switch is
  followed.
- Leaving one calls `JumpExposureAdaptation()` (`SceneRenderer.h:704`), so the lit image returns at once without a flash.
`RenderPost` (camera-and-tone's file) is not edited: it runs as always and the overlay replaces its result.

**Editor.**

- `ViewportDebugView` (`StreamingView.h:26-37`) appends `Lighting`, `AmbientOcclusion`, `Indirect`, `Luminance`, `Albedo`
  with names "lighting", "ao", "indirect", "luminance", "albedo" (`StreamingView.cpp:40-49`); `StreamingView::Build`
  builds nothing for them (its check at `.cpp:69` becomes "not a tint view").
- `ViewportRenderer` (`ViewportRenderer.cpp:186-189`) maps them to `options.LightingView` and passes the theme's colors
  (`ViewportDebugColors` gains `Luminance` (12), `AlbedoLow`, `AlbedoHigh`, `Background`, set by the UI from theme tokens as
  its other colors are); it exposes `GetLightingViewStats()`.
- `DebugViewSchema` (`EditorViewportCommands.cpp:130-133`) lists the five names; the descriptions of `viewport.setSettings`
  and `viewport.capture` (877, 909, 1002-1004, 1026) say what each shows; unknown names stay rejected (1069-1074).
  `viewport.capture` is in the MCP `core` toolset (`StrataCLI/src/CLI/McpToolsets.cpp:20-29`), which must stay within 30 KiB
  (`EditorMcpToolsetsTests`): its description gains one sentence naming the five views and `debugView.lighting`; the
  per-view explanations go into the `debugView` parameter's description and the skill.
- `viewport.capture` with a lighting view waits for statistics of a frame it rendered (`CaptureProgress::LightingViewFrames`,
  bounded by `c_MaxCaptureLightingViewFrames` = 8 re-renders; the existing `LightingFrames` and
  `c_MaxCaptureLightingFrames`, `EditorViewportCommands.cpp:646-655`, already count the local shadows' and probes' waits and
  keep that meaning), and `DescribeDebugView` adds
  `"lighting": { "view", "surfaceShare", "surfaceEv100": {"p05", "p50", "p95"}, "backgroundEv100", "value": {"median",
  "p05"}, "belowRangeShare", "aboveRangeShare", "sunIlluminance", "skyIlluminance", "sunToSkyRatio" }` (NaN as null; the
  fields a view does not measure are null), or `"lighting": null` with `"lightingPending": true` when the bound ran out.
- The Viewport's chip (`ViewportPanel.cpp:560-596`) names the views "Lighting", "AO", "Indirect", "Luminance", "Albedo"
  with one-line menu descriptions; the legend (598-640) shows the luminance bands with their EV100 ranges, the albedo marks
  with their shares, the AO median, and a note "Sun : sky 6.9 on flat ground" for every lighting view.
- The editor-automation skill lists the views and the statistics.

### D3 Energy compensation (step 2)

The technique is multiple-scattering compensation of a microfacet lobe from its directional albedo: the single-scattering
lobe of F0 = 1 reflects `E(NoV, roughness) = A + B` of the light (the BRDF table's x + y); the light it loses is
reflected again by the surface's microfacets, which a factor `1 + F0 (1 / E - 1)` restores (exact for F0 = 1, where the
lobe then reflects all the light; scaled by F0, the light each further bounce keeps). It needs only the existing table.

`PBR.glsl`:

```glsl
// The smallest directional albedo the compensation divides by: the table's x + y stays above 0.2 for every NoV and
// roughness (unverified for the extreme corners of the 128 x 128 table), this only keeps a broken table finite.
const float c_MinSpecularAlbedo = 0.05;

// Multiple-scattering compensation of the specular lobe: the factor its light is multiplied by. `dfg` is the BRDF table at
// (NoV, roughness): its x + y is the single-scattering lobe's directional albedo for F0 = 1.
vec3 GetSpecularEnergy(vec3 f0, vec2 dfg)
{
	return 1.0 + f0 * (1.0 / max(dfg.x + dfg.y, c_MinSpecularAlbedo) - 1.0);
}
```

- `SurfaceData` gains `vec3 SpecularEnergy;` and `EvaluateLightWeighted` multiplies its specular term (line 67) by it. The
  diffuse term keeps `(1 - F) * (1 - metallic)`: with the compensation a dielectric's specular grows by about 9% at normal
  incidence (F0 0.04, roughness 1, A + B about 0.30 per the audit's numbers: `1 + 0.04 (1 / 0.30 - 1)`), under 0.5% of the
  light it reflects (unverified numerically; the furnace test bounds it). Only `Forward.frag` builds `SurfaceData` and
  `ClearcoatData` (no other shader includes `PBR.glsl` but `IBL/BRDFLut.comp` and `IBL/Prefilter.comp`), so these are the only
  sites to set.
- `ClearcoatData` gains `float SpecularEnergy;` and `EvaluateClearcoat` multiplies the coat's light by it (`baseWeight`
  unchanged). `main()` initializes it to 1 beside `coat.Strength = 0.0` (`Forward.frag:323-325`), so a surface without a coat
  never reads an undefined value.
- `Forward.frag`, `ShadeLitSurface`: the table fetch moves from the indirect block (204-207) to the surface setup, right
  after line 127: `NoV`, `f0`, `dfg` computed once, `surface.SpecularEnergy = GetSpecularEnergy(f0, dfg)`; the indirect
  block reuses them, with `specularColor = (f0 * dfg.x + dfg.y) * surface.SpecularEnergy` (207). The diffuse image light
  keeps `* (1.0 - specularColor)` (226, 236, 243), now the compensated reflectance: diffuse gets what specular leaves.
  For F0 = 1 the reflectance is exactly 1 (`E * (1 / E)`), so a white metal in a white furnace reflects the whole sky.
- The clear coat's table fetch (258-259) moves up beside the base's: `coat.SpecularEnergy = 1 + c_ClearcoatF0 * (1 /
  (coatDFG.x + coatDFG.y) - 1)`, and `coatReflectance` (260) is multiplied by it.
- The glass variant's `TransmitLight` (`Forward.frag:285-300`) multiplies `reflectance` by `GetSpecularEnergy(f0, dfg)`,
  so transmitted light leaves what the surface reflects.
- `ThumbnailCache::c_Version` (`StrataEditor/src/Editor/ThumbnailCache.h:104`, 4 at bc6ee123) goes up by one in this
  step: thumbnails of models with metals look different. `sun-shadows` also bumps it (4 to 5, `specs/sun-shadows.md` 4) and
  lands earlier, so at this stream's landing the value is main's plus one (the later lander bumps above main's). Later
  steps of this stream change thumbnails too (AO's radius, the procedural defaults of the preview sky) and land with it in
  one merge: no further bump.

### D4 Ambient occlusion without TAA (step 3)

**Quality row** (`R/RendererQuality.h`, one line in the preset table):

```cpp
// Ambient occlusion's slices and steps per side (GTAO.comp): the file and script values never change.
enum class AmbientOcclusionQuality : uint8_t { Low = 0, Medium = 1, High = 2, Ultra = 3 };
struct AmbientOcclusionSamples { uint32_t Slices = 2; uint32_t StepsPerSide = 6; };
AmbientOcclusionSamples GetAmbientOcclusionSamples(AmbientOcclusionQuality quality); // {1,4} {2,6} {3,8} {4,12}
// RendererQuality.cpp, one line beside step 0's typed rows (specs/step0.md 7.2):
constexpr PresetRow<AmbientOcclusionQuality> c_AmbientOcclusionQualityRow = { AmbientOcclusionQuality::Low, AmbientOcclusionQuality::Medium, AmbientOcclusionQuality::High, AmbientOcclusionQuality::Ultra };
// graphics-options' traits table, one line: { "AmbientOcclusionQuality", true, "GPU.AO", nullptr } (no memory of its own)
```

`RendererQualitySettings::AmbientOcclusionQuality = AmbientOcclusionQuality::Medium` (so `SceneRendererSpecification{}`
keeps 0.6.0's 2 x 6); the `Renderer.Quality` field "AmbientOcclusionQuality" (Enum "Low", "Medium", "High", "Ultra",
default "High" as every field's default is High's value; described for agents: "Custom only: ambient occlusion's samples,
Low 1x4, Medium 2x6 (Strata 0.6.0's), High 3x8, Ultra 4x12 slices x steps per side; GPU time at 3840x2160 grows about with
their product"). The other touch points of 2.7 each get their line (`DescribeRenderStats`' quality key
`"ambientOcclusionQuality"`). `ForThumbnails()` Medium, `ForBake()` Medium (0.6.0's values, step 0's Q3). `SetQuality`
changes only the push constants: nothing to rebuild. `SceneRendererStats::AmbientOcclusionSamples` reports slices x steps
x 2 (0 without AO).

**Push constants** (`SceneRenderData.h:361-367` grows; AO layout's `PushConstants(1, ...)` follows):

```cpp
struct AOParameters
{
	float Radius;            // Meters (PostProcessComponent::AmbientOcclusionRadius, clamped to [0.01, 10])
	float Intensity;         // Exponent of the visibility ([0, 8])
	uint32_t SliceCount;     // The quality row's
	uint32_t StepsPerSide;
	float MinRadiusPixels;   // max(c_AOMinRadiusFraction * viewport height, 1.5)
	float MaxRadiusPixels;   // c_AOMaxRadiusFraction * viewport height
	float ThinOccluderDecay; // 1 - 0.9^(6 / StepsPerSide): how far a horizon relaxes toward a lower sample per step
	uint32_t Padding;
};
static_assert(sizeof(AOParameters) == 32);
constexpr float c_AOMinRadiusFraction = 0.003f; // 6.5 px at 2160 rows, 3.2 at 1080
constexpr float c_AOMaxRadiusFraction = 0.12f;  // 259 px at 2160 rows (0.6.0's cap was 256 px at every size), 130 at 1080
```

**GTAO.comp** (changes by line):

- Radius (55-64): `radiusPixels = clamp(u_AO.Radius * pixelsPerUnit, u_AO.MinRadiusPixels, u_AO.MaxRadiusPixels)`; the
  world radius used for the falloff (69, 116) and the minimum step (71) becomes `radiusPixels / pixelsPerUnit`, so a close
  surface keeps a radius of at most 12% of the view's height (cost and look independent of resolution) and a far one at
  least 0.3% of it (distant creases keep a line instead of vanishing). The "below 1 pixel, no occlusion" early-out goes
  (the minimum is at least 1.5 px).
- Noise (66-68): no longer turned by `TemporalParams.z`. A fixed 4x4 interleaved pattern: with `c_Order[16] = {0, 8, 2,
  10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5}` (the 4x4 ordered-dither order `IsLODFadeDrawn` uses, `Scene.glsl:138`) and
  `cell = pixel & 3`, the slice rotation is `(c_Order[cell.y * 4 + cell.x] + 0.5) / 16` and the step offset
  `(c_Order[cell.x * 4 + cell.y] + 0.5) / 16`. Every 4x4 block holds the 16 rotations once, so the resolve below integrates
  16 times the slices: 32 directions at Medium.
- Thickness heuristic (116-118): a thin occluder is seen by few samples, after which the samples fall back below the horizon
  it raised. Per side, a sample above the horizon raises it as today; a sample below it relaxes the horizon toward it:
  `horizonCos[side] = sampleCos > horizonCos[side] ? sampleCos : max(mix(horizonCos[side], sampleCos, u_AO.ThinOccluderDecay),
  lowHorizonCos[side])`. Poles, fences and thin plates in front of walls stop drawing dark halos on the walls; thick
  occluders, whose later samples stay high, keep their occlusion. `ThinOccluderDecay` is 0.1 at 6 steps and scaled with the
  step count so the relaxation per meter stays the same at every quality.

**AODenoise.comp** (replaces `AOBlur.comp`; registered as `Effects/AODenoise.comp` and `Effects/AODenoiseHalf.comp`
(`ST_AO_HALF`), the blur's two entries leaving `strata_add_shaders`). Same binding layout as today's AO passes
(`SceneRenderer.cpp:423-433`: SRV 8 depth, SRV 9 normals, SRV 10 input, UAV 0 output, push constants), one dispatch, the
same two targets (no new memory):

1. 8x8 groups; each loads a 16x16 tile (its 8x8 pixels and 4 around) of raw occlusion, linear view depth and decoded
   geometric normal into group-shared arrays (256 x 20 bytes, plus the 12x12 resolved values and depths: about 6 KB;
   MoltenVK allows 32 KB).
2. **The resolve** of the 4x4 pattern, for the 12x12 pixels around the group: a 5x5 bilateral average with the tent weights
   (0.5, 1, 1, 1, 0.5) in x and y, so every one of the 16 pattern phases weighs exactly 1 and the kernel stays centered,
   times `exp(-50 |z_s - z_c| / z_c)` and `max(dot(n_s, n_c), 0)^8`. Background samples (depth 0) weigh 0.
3. **Today's edge-aware blur**: the 5x5 depth-weighted average of the resolved values (`AOBlur.comp`'s weights,
   `exp(-50 x relative depth difference)`), written to the output. Background pixels write 1.

**Multi-bounce** (Forward.frag's indirect block). The technique: occluded light is not lost but reflected by the occluders,
which are lit like the point and have its albedo; the GTAO paper names such a fit "multi-bounce". Ours is fitted here:

- `Tools/FitAOMultiBounce.py` (standard library; its outputs committed; CI never runs it; header docstring with its
  command line, like `Tools/GenerateIslandTerrain.py`). Reference: 2D radiosity in cavities whose walls see each other
  without obstruction, so view factors are exact by the crossed-strings rule: V-grooves of opening angles 20, 30, ... 160
  degrees and rectangular trenches of width-to-depth ratios 0.25, 0.5, 1, 2 and 4, each surface split into 64 strips. Under
  a uniform sky of irradiance E0 on an open plane, strip i receives `E0 F_i,sky` from the sky (`F_i,sky = 1 - sum_j F_ij`),
  which is exactly GTAO's cosine-weighted visibility `x_i`; with walls of albedo rho the radiosity `B_i = rho (E0 F_i,sky +
  sum_j F_ij B_j)` is solved by Gauss-Seidel to 1e-9, and the reference multi-bounce visibility is `y_i = (E0 F_i,sky +
  sum_j F_ij B_j) / E0`, for rho in 0.05, 0.10, ... 0.90.
- Model: `y = x + rho x (1 - x) (p0 + p1 x + p2 rho + p3 rho x)`: 1 at x = 1, x at rho = 0, above x for the fitted
  coefficients. Least squares over every (strip, rho) sample, normal equations solved in pure Python; the script prints the
  maximum and RMS error, and for comparison the uniform-enclosure closed form `x / (1 - rho (1 - x))`. If the polynomial's
  maximum error exceeds 0.03, the script fails and says so (the model then needs another term; the decision is the
  implementing agent's, recorded in the script).
- `--write` writes `SH/Include/AmbientOcclusionFit.glsl` (`const vec4 c_AOMultiBounce`; `vec3 AOMultiBounce(float
  visibility, vec3 albedo)`, per channel), `R/AmbientOcclusionFit.h` (the same constants and `glm::vec3 AOMultiBounce(float,
  const glm::vec3&)`, used by the tests' expected values) and `StrataTests/src/Renderer/AmbientOcclusionFitReference.h`
  (`c_AOMultiBounceReference`: 24 (x, rho, y) samples of the reference spread over its range; test data stays out of
  engine headers), each marked "generated by Tools/FitAOMultiBounce.py; do not edit".
- `Forward.frag`: `vec3 diffuseOcclusionColor = u_Frame.AOParams.y > 0.0 ? AOMultiBounce(diffuseOcclusion, surface.Albedo *
  (1.0 - surface.Metallic)) : vec3(diffuseOcclusion);` replaces `diffuseOcclusion` where it multiplies diffuse light (231,
  239, 244, 250); specular occlusion keeps the scalar.
- `PostProcessComponent::AmbientOcclusionMultiBounce` (bool, default true; tooltip "Occluded corners of bright surfaces
  keep the light their walls reflect into them: white plaster stays light in its creases, dark soil darkens fully"; no
  inactive reason: it applies to materials' occlusion maps too). `AOParams.y` is 1 when it is on, whether or not screen AO is.
  Registered right after `AmbientOcclusionIntensity` (inside `graphics-options`' "Ambient occlusion" section, so it needs
  no `.Section` call of its own; if it lands before `graphics-options`, `graphics-options` places it), with the tooltip as
  its description (`graphics-options`' S6 test requires one on every Post Process property).

**Radius default.** `PostProcessComponent::AmbientOcclusionRadius` defaults to 1.0 (`RendererComponents.h:240`; tooltip:
"Meters: how far around a surface occluders darken it; on screen kept between 0.3% and 12% of the view's height").
Stored values stay; scenes without a Post Process get 1.0 (release notes).

### D5 Balanced light by default (step 4)

**Sky presets** (`R/SkyPresets.h/.cpp`, renderer module: the command and the Inspector use one table; layers: Renderer
already includes nothing new):

```cpp
// The sun and sky presets (lighting.applySkyPreset). Values are command API: they never change meaning.
enum class SkyPreset : uint8_t { Clear = 0, Hazy = 1, Overcast = 2, GoldenHour = 3 };
const char* SkyPresetToString(SkyPreset preset);                       // "Clear", "Hazy", "Overcast", "GoldenHour"
std::optional<SkyPreset> SkyPresetFromString(std::string_view name);
struct SkyPresetValues
{
	SkyLightSource Source;
	float SkyIntensity;
	float AtmosphereDensity, AtmosphereHaze, GroundAlbedo;  // Atmosphere
	glm::vec3 ZenithColor, HorizonColor, GroundColor;      // Procedural (nits)
	float SunDiskIntensity;                                // SkyLightComponent::SunIntensity (procedural disk)
	float SunSize;                                         // Degrees
	float SunElevation;                                    // Degrees above the horizon
	float SunIlluminance;                                  // DirectionalLightComponent::Intensity (lux; above the air for the atmosphere)
	glm::vec3 SunColor;
	float ShadowSoftness;                                  // Degrees
};
const SkyPresetValues& GetSkyPresetValues(SkyPreset preset);
// The direction toward the sun at the preset's elevation (or `elevationDegrees`) and `azimuthDegrees` (from +X toward +Z).
glm::vec3 GetSkyPresetSunDirection(float elevationDegrees, float azimuthDegrees);
```

| Preset | Source | Air: density, haze, ground albedo | Procedural colors (nits) | Sun: elevation, lux, color, softness | Sky intensity, SunSize |
| --- | --- | --- | --- | --- | --- |
| Clear | Atmosphere | 1.0, 1.0, 0.3 (the component's defaults) | n/a | 40, 120,000, (1, 1, 1), 0.53 | 1, 0.53 |
| Hazy | Atmosphere | 1.2, 4.0, 0.3 | n/a | 35, 120,000, (1, 1, 1), 0.53 | 1, 0.53 |
| Overcast | Procedural | n/a | zenith (2600, 2650, 2750), horizon (2200, 2250, 2300), ground (400, 380, 360); disk `SunIntensity` 0 | 60, 1,500, (1, 1, 1), 10 (the property's maximum, `RendererComponents.cpp:233`) | 1, 0.53 |
| GoldenHour | Atmosphere | 1.0, 1.5, 0.3 | n/a | 6, 120,000, (1, 1, 1), 0.53 | 1, 0.53 |

Clear is a default Atmosphere sky (`SkyLightComponent`'s `AtmosphereDensity` 1, `AtmosphereHaze` 1, `GroundAlbedo` 0.3,
`RendererComponents.h:99-101`; the haze tooltip says "1: a clear day", `RendererComponents.cpp:309-310`), so the template,
the preset and a new Atmosphere sky agree, and the audit's 6.9 ratio was measured on these values.

Why: 120,000 lux is the sun above the air (the manual's value, `Docs/Manual/Lighting.md:84, 88`); the atmosphere dims it to
about 95,000 lux at 40 degrees (unverified; `SceneRendererStats::SunTransmittance` reports it, and the command returns
the ground value). The atmosphere cannot make an overcast sky (its haze is Earth's clear aerosol, `Atmosphere.h:28`, and
even 10 times that is a light haze), so Overcast is a near-uniform procedural sky of about 7,700 lux on the ground with a
weak, very soft sun (a sun to sky ratio near 0.17). The values are judged on the look-dev sheet; the step may move them by
at most 25% (elevations by 5 degrees) with the reason in its commit, never their kind.

**`lighting.applySkyPreset`** (`EditorLightingCommands.cpp`, kind Edit, not deferred; `c_EditCommands` of
`Editor.Journal`, `StrataTests/src/Editor/CommandJournalTests.cpp`):

- Description (for agents): "Lights the scene with a sun and sky preset: Clear (the physical atmosphere, a 120,000 lux sun
  40 degrees up: sunny, deep blue shadows), Hazy (thicker air, a paler sky and softer contrast), Overcast (an even gray sky
  of about 7,700 lux and a faint, very soft sun) or GoldenHour (the sun 6 degrees up, warm light and long shadows). Writes
  the sky light's source and settings and the directional light's rotation, intensity, color and shadow softness; keeps the
  light's azimuth unless sunAzimuth is given. Raises the active Post Process's AutoExposureMaxEV to 16 when automatic
  exposure would otherwise clip a real sun. Creates 'Sky' or 'Sun' when the scene has none. One undo step; while playing it
  changes the running copy (warning). Returns {preset, changed, sky, light, createdSky, createdLight, sunDirection (toward the sun),
  sunIntensity (lux above the air), groundIlluminance (lux on a surface facing the sun at the ground), postProcess (the
  entity whose AutoExposureMaxEV was raised, or null)}."
- Schema: `{ "preset": {"type": "string", "enum": ["Clear", "Hazy", "Overcast", "GoldenHour"]}, "sky": entity, "light":
  entity, "sunElevation": {"type": "number", "minimum": -10, "maximum": 90}, "sunAzimuth": {"type": "number", "minimum":
  -360, "maximum": 360} }`, required `preset`. Invalid parameters (unknown preset, a `sky` without a SkyLight, a `light`
  without a DirectionalLight, numbers out of range) fail with `InvalidParameters` before anything changes.
- Defaults: `sky` the first active SkyLight (else a new entity "Sky"); `light` the brightest active directional light
  (`FindSceneSun`, `EditorLightingCommands.cpp:507`; else a new "Sun"); the azimuth the light's current one, measured on the
  direction **toward** the sun, which is the negated world -Z axis (the light shines along -Z, away from the sun:
  `towardSun = -normalize(world * (0, 0, -1, 0))`, azimuth `degrees(atan2(towardSun.z, towardSun.x))`, as
  `DirectionFromAngles` defines it, `SceneRenderer.cpp:91-95`; using the -Z axis itself would turn the sun 180 degrees);
  20 degrees for a new light or one whose horizontal part is under 1e-4 (pointing straight down).
- Effects, in one `SceneEditTransaction` ("Apply Sky Preset"; `Track` each changed entity, `TrackCreated` the new ones):
  the SkyLight's `Source`, `Intensity`, `AtmosphereDensity`, `AtmosphereHaze`, `GroundAlbedo` (atmosphere presets) or the
  three colors and `SunIntensity` (Overcast), and `SunSize`; the light's rotation (as `ApplyExtractedSun` does, 546-568:
  through the parent's inverse linear part, failing on a zero Z scale), `Intensity`, `Color`, `ShadowSoftness`; then the
  active `PostProcessComponent` with `AutoExposure` on and `AutoExposureMaxEV` below 15: set to 16. Each written component
  is marked modified. `FinishEdit` (`CommandUtils.cpp:418-434`) records the step and adds the running-copy warning while
  playing. A preset that changes nothing records no step (`changed: false`).
- `groundIlluminance`: `SunIlluminance * Atmosphere::ComputeSunTransmittance(...)` luminance for atmosphere presets (the
  CPU function the renderer uses, `SceneRendererAtmosphere.cpp:327`), the light's intensity otherwise.

**Inspector** (`DrawSky`, `InspectorPanel.cpp:1344-1432`): a row "Presets" with four `UI::ToolbarButton`s, `Icons::Sun`
"Clear", `Icons::Haze` "Hazy", `Icons::Cloud` "Overcast", `Icons::Sunset` "Golden Hour" (tooltips quote the table's line),
each running `lighting.applySkyPreset {preset, sky}` through `RunEditorCommand` with `CommandOrigin { CommandSource::UI }`.
Shown with and without a sky (without one the command creates it). Enabled while playing (the running copy, as every
component edit). `graphics-options` owns the Inspector's property sections: this is one row in `DrawSky`, merged additively.

**The template** (`ProjectTemplates.cpp`): `PopulateBasic3D` takes Clear's values: the sun's rotation toward
`GetSkyPresetSunDirection(40, 20)` (elevation 40, the preview's azimuth 20: the light from the camera's right and slightly
behind it, so objects placed at the origin show a lit and a shaded side and a shadow falling left and back), intensity
120,000, color white, `ShadowSoftness` 0.53, `CastShadows`; the sky `Source = Atmosphere` with Clear's density, haze and
ground albedo. `c_Basic3DSunIntensity` (line 31) becomes 120,000; `GetBasic3DSunDirection()` returns
`GetSkyPresetSunDirection(40, 20)` and no longer equals the preview sun; the template's description (24-27) says
"a sun and the physical atmosphere (the Clear preset)", and the comments of `Populate` and `GetBasic3DSunDirection`
(`StrataEditor/src/Editor/ProjectTemplates.h:57-71`: "intensity 3", "a procedural sky light", "the preview sun's ... 55
degrees up") say the same. The Post Process lines (69-76: tone curve, exposure) are `camera-and-tone`'s and stay as they
are in this stream. If the template's measured ratio (6.4) is below 5 at 40 degrees, the step raises the elevation to 45
(within the 5 degrees the presets may move), never the threshold.

How it looks: auto exposure meters the frame (the ground of albedo 0.16 at about 3,100 nits in the sun and 450 in shade,
the sky at 3,000-8,000; unverified, read from the luminance view's statistics at landing) to middle gray. With
`camera-and-tone`'s Filmic at Exposure 0 (calibrated so metered 0.18 displays at about sRGB 118) it opens as a bright,
sunny day with blue-tinted shade at about 1/6 of the sunlit ground. **If `camera-and-tone` lands later** (plan.md 4: a
stream ready earlier may land first), the template keeps ACES at Exposure +1 over this sky: about the same brightness
(ACES maps metered gray to sRGB 91, +1 EV brings it near 118) with ACES's hue skews and harder highlights; nothing in this
stream depends on the curve, and `camera-and-tone` retunes the template's exposure line on its scene camera after merging
main, as plan.md 4.4 step 7 says. `AutoExposureMaxEV` is 16 by the component's default (enough for a real sun).

**Procedural sky defaults** (`RendererComponents.h:91-93`): the three colors times 0.2: zenith (0.036, 0.064, 0.124),
horizon (0.132, 0.148, 0.168), ground (0.044, 0.04, 0.036). The factor comes from the audit's measurement: lit 0.286 and
shadow 0.109 under the 3 lux sun at 55 degrees (`lighting/audit.md` L2) give 1.52 lux of sky on flat ground against 2.46
of sun; at 0.2 of the sky that is a ratio of about 7.0 with the default sun at 45 degrees (`c_DefaultSunElevation`,
`SceneRenderer.cpp:84`, the direction without a light) and 8.1 at the preview sun's 55 degrees: the atmosphere's 6.9 at
the first, a clear day's ratio at both, with room above the tests' threshold of 5 (0.25 would give 5.6 at 45 degrees,
too close to it for an estimate). The GPU test measures it. `SunIntensity` (20) and `AmbientColor` stay. Saved skies keep
their colors; new components, the editor's preview sky for unlit scenes (`SceneRenderer.cpp:854-860`, with the preview
sun of 3 lux at 55 degrees, 2378) and scenes made by `component.add` get them.

**The glTF setting** (`GltfImporter.cpp`): `ImportSettings::BaseColorFactorsAreSRGB` (bool, default false), read in
`ReadSettings` (90-121), in `GetDefaultSettings` (2022-2034) and `GetSettingsProperties` (2052-2077: `.Bool(
"BaseColorFactorsAreSRGB", false).DisplayName("Base Color Factors Are sRGB").Tooltip("The file's base color factors hold
sRGB-encoded values (some exporters write them so, e.g. Kenney's kits): convert them to linear, as glTF expects. Off
reads them as glTF specifies")`), inactive while Import Materials is off (`GetInactiveReason`, 2037-2050). In
`ImportMaterial` (799): when set, `properties.BaseColor.rgb` takes the sRGB-to-linear transfer per channel (alpha
unchanged; emissive factors unchanged, the exporter's problem is the base color). `GetVersion()` (`GltfImporter.h:39`)
becomes 11 with the comment "11: BaseColorFactorsAreSRGB". Adding a default key already changes every glTF model's settings
hash (`Strata/src/Strata/Asset/EditorAssetManager.cpp:1403-1404`, `1971-1973`: defaults are merged before hashing), so every model re-imports once
whether or not the version moves; the bump records why (release notes).

**The samples' adoption** (through editor commands on the samples, never by editing files; plan.md 4.1):

- Physics Playground: `lighting.applySkyPreset {"preset": "Clear"}` (keeps its sun's azimuth, raises its Post Process's
  `AutoExposureMaxEV` from 12 to 16); `component.set` `AmbientOcclusionRadius` 1.0.
- Open World: `component.set` the Sun's `DirectionalLight.Color` to [1, 1, 1] (the `DayNight` script sets only its
  intensity); `asset.setImportSettings {"settings": {"BaseColorFactorsAreSRGB": true}}` on each of the 35
  `Assets/Models/Nature/*.glb` (`tree_default.glb`'s leaves become linear (0.022, 0.584, 0.408), albedo luminance about
  0.45 instead of 0.65); `AmbientOcclusionRadius` 1.0; `AerialPerspectiveScale` (step 6) chosen on the look-dev sheet (start
  at 6: the island is 768 m across, at real scale it would show no haze). `README.md` says why the factors are sRGB; the
  `StrataRuntime.OpenWorldWindowedImage` check (`--dominant green 5`) must keep passing (tune the scene, never the check).
- Third-Person Adventure: `AmbientOcclusionRadius` 1.0 in `MainMenu` and `Course`; its HDR sky and extracted sun stay.
- Their command scripts (`StrataTests/Editor/<Sample>Commands.json.in`) check the new values (section 6); pictures are
  retaken at the end of the stream (step 6), after merging main.

### D6 Specular that holds at 4K (step 5)

**Geometric specular anti-aliasing.** The technique (Kaplanyan's and Tokuyoshi's filtering of normal distributions, named
here, designed below): the shading normal's change across the pixel widens the specular lobe by its variance, so a highlight
that would flicker between pixels spreads over them with its energy. `PBR.glsl`:

```glsl
// Screen-space variance of the normal over a pixel's filter footprint, per unit of its squared derivatives.
const float c_SpecularAAVariance = 0.25;
// The most the lobe's alpha^2 is widened: silhouettes and seams, where the normal jumps, would otherwise turn matte.
const float c_SpecularAAMaxKernel = 0.18;

// The perceptual roughness widened by the variance of the normal over the pixel: `normalDx` and `normalDy` are the screen
// derivatives of the unit shading normal; `strength` 0 returns the roughness unchanged.
float WidenRoughness(float perceptualRoughness, vec3 normalDx, vec3 normalDy, float strength)
{
	float variance = c_SpecularAAVariance * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
	float kernel = min(2.0 * variance, c_SpecularAAMaxKernel) * strength;
	float alpha = perceptualRoughness * perceptualRoughness;
	return sqrt(sqrt(clamp(alpha * alpha + kernel, 0.0, 1.0)));
}
```

- `Forward.frag` `main()`: right after `shadingNormal` (320), in the branch on the material's unlit flag (flat per
  primitive, so uniform across each quad on both paths), `vec3 normalDx = dFdx(shadingNormal), normalDy =
  dFdy(shadingNormal);` and `roughness = WidenRoughness(clamp(material.Roughness * metallicRoughness.g, 0.0, 1.0),
  normalDx, normalDy, u_Frame.MaterialParams.z)`. `ShadeLitSurface` takes the roughness as a parameter instead of computing
  it (124-125). The coat: `dFdx`/`dFdy` of `coat.Normal` (330) widen `coat.Roughness` the same way. The glass variant's
  `roughness` (337) is the widened one. Derivatives are taken before any discard (the comment at 309-310 holds).
- `DepthPrepass.frag` (`ST_PREPASS_SURFACE`, 59-69) computes the same derivatives of the same normal and writes the widened
  roughness into the surface target, so SSR traces what the forward pass shades.
- Why both paths agree: the shading normal is a function of the primitive's interpolants (`v_Normal`, `v_Tangent`,
  `v_TexCoord`) and its material's textures only; a quad belongs to one primitive; the GPU path draws the same triangles with
  the same vertex module and the same depth. `GPU.SceneRenderer.GPUDriven` gains a normal-mapped, clear-coated scene to
  prove it (section 6).
- `SceneRendererSpecification::SpecularAntiAliasing` (default true) sets `MaterialParams.z`; tests use it to compare.

**Horizon fade.** A reflection the normal map turns below the geometric surface would see the surface itself:

```glsl
const float c_HorizonFadeWidth = 0.25; // The fade reaches 0 about 15 degrees below the geometric surface
float GetHorizonFade(vec3 reflected, vec3 geometricNormal)
{
	float fade = clamp(1.0 + dot(reflected, geometricNormal) / c_HorizonFadeWidth, 0.0, 1.0);
	return fade * fade;
}
```

`Forward.frag` multiplies `specularOcclusion` (217) by `GetHorizonFade(reflected, geometricNormal)` and `coatOcclusion`
(263) by the coat's, with `geometricNormal = normalize(v_Normal) * (gl_FrontFacing ? 1.0 : -1.0)` (as at 136). Only image
light (probes, sky, SSR); direct light has its own `NoL`.

**The `EnvironmentResolution` row** (`R/RendererQuality.h`):

```cpp
// RendererQualitySettings: the prefiltered sky's size per face, 256 or 512 (the environment cube it is filtered from stays
// 512); Clamp takes any other value to 256.
uint32_t EnvironmentResolution = 256;
// RendererQuality.cpp, one line beside step 0's typed rows:
constexpr PresetRow<uint32_t> c_EnvironmentResolutionRow = { 256, 256, 512, 512 };
// graphics-options' traits table, one line: { "EnvironmentResolution", true, "", &EnvironmentMemoryBytes }, where
// EnvironmentMemoryBytes(settings, output) is the prefiltered cube's bytes with its six levels (RGBA16F: 4.0 MiB at 256,
// 16.0 MiB at 512) times 2 (the atmosphere's spare set; the page shows the larger case).
```

- `CreateEnvironmentMaps` (`SceneRenderer.cpp:666-690`) uses the renderer's value instead of `c_PrefilteredSize` (60), and
  `EnvironmentMaps` records it (`PrefilteredSize`). Six levels at both sizes (roughness 0 ... 1; mip count unchanged, so
  `EnvironmentParams.z` stays 5).
- `FilterEnvironmentStep` (812-815): samples per level `GetPrefilterSamples(size, mip)`: 512 at every level of 256 (Medium
  is bit-identical to 0.6.0); at 512, levels 1 and 2 take 128 (narrow lobes read finer source mips through the filtered
  importance sampling, `Prefilter.comp:43-56`) and levels 3-5 take 512, so the largest step costs what 0.6.0's largest
  does (level 1: 256^2 x 6 x 128 samples = 0.6.0's level 1, 128^2 x 6 x 512) and the atmosphere's spread refresh never
  hitches more than today.
- Sharing (`FindSharedEnvironment`, 726-740; `ShareEnvironment`, 742-750) keys by the texture and the size: two renderers of
  different rows compute their own maps.
- `SetQuality` with a new `EnvironmentResolution`: `m_Environment` is dropped, its textures released through
  `Renderer::ReleaseDeferred` as step 0's `SetQuality` releases the cascades (the map path recomputes or takes a shared set
  of its size, the procedural sky regenerates), the atmosphere's `Refresh` and `Spare` are released the same way and reset
  (`takeMaps`, `SceneRendererAtmosphere.cpp:267-276`, would otherwise reuse a spare of the old size), so its
  first-environment path (277-285) computes the new size at once; `SetQuality` then returns true (history restarts).
  Memory of the prefiltered cube (RGBA16F, `c_EnvironmentFormat`, six levels): 4.0 MiB at 256, 16.0 MiB at 512 per set; the
  atmosphere holds two sets (DG "Fog and atmosphere" restated). The `Renderer.Quality` field "EnvironmentResolution" (Enum
  "256", "512", default "512"): "Custom only: the prefiltered sky's size per face: 512 keeps mirror reflections sharp at 4K;
  12 MiB more per view, 24 MiB with the atmosphere". `DescribeRenderStats`' quality key `"environmentResolution"`.
  `ForThumbnails()` 256, `ForBake()` 256 (0.6.0's: the bake's faces are 256 pixels, step 0's Q3).

### D7 Aerial perspective (step 6)

The air between the camera and a surface dims what lies behind it and scatters the sun's and the sky's light toward the
camera, as the atmosphere's sky does along the whole ray. A froxel volume holds both per frame, computed from the tables the
atmosphere already has.

**Component** (`SkyLightComponent`, `RendererComponents.h:79-104`):

```cpp
// Atmosphere: the air between the camera and every surface dims it and adds the light it scatters toward the camera, as the
// sky does over its whole depth (aerial perspective), distances multiplied by AerialPerspectiveScale: worlds smaller than
// real ones (an island of 1 km) show the haze of larger ones above 1.
bool AerialPerspective = true;
float AerialPerspectiveScale = 1.0f;
static constexpr float c_MaxAerialPerspectiveScale = 64.0f;
```

Registered with tooltips ("Aerial Perspective: distant surfaces fade into the air's haze, as the sky does";
"Aerial Perspective Scale: distances multiplied by this for the haze: 1 is Earth's air, 4 makes a 1 km view look like
4 km") and `Range(0, 64)`; inactive reasons: `AtmosphereOnly` for both, and "Aerial Perspective is off" for the scale.
Files without the keys (every 0.6.0 scene) get the defaults: atmosphere scenes gain aerial perspective (a default that
changes, release notes).

**When** (`UpdateEnvironment`): `m_AerialFrame = m_AtmosphereFrame && sky.AerialPerspective && scale > 0 &&
!camera.Orthographic` (orthographic views keep no distance; the scale clamped to [0, `c_MaxAerialPerspectiveScale`] and
non-finite values taken as 0, as `SetHeightFog` sanitizes its inputs). `SetAerialPerspective` (D1, called after
`SetHeightFog` because that function assigns the whole `FogParams` vector) writes `FogParams.z` = the volume's far distance
`D = clamp(camera.Far, 1, c_AerialMaxDistance)` meters and `FogParams.w` = the scale; both 0 when off.
`SceneRendererStats::AerialPerspective`.

**Data** (`R/SceneRenderData.h`):

```cpp
constexpr uint32_t c_AerialSize = 32;          // Froxels across the screen, each way
constexpr uint32_t c_AerialSlices = 32;        // Depth slices; the texture holds two slabs of them (scattering, transmittance)
constexpr float c_AerialMaxDistance = 32000.0f; // Meters: the volume reaches at most this far (or the camera's far plane)
// IBL/AtmosphereAerial.comp's push constants: the view rays through the screen's corners, unjittered, camera-relative world
// space (not normalized: they interpolate linearly across the screen), and what the march needs.
struct AerialPerspectiveParameters
{
	glm::vec4 RayTopLeft;     // w: the volume's far distance D (meters)
	glm::vec4 RayTopRight;    // w: the distance scale
	glm::vec4 RayBottomLeft;  // w: the viewer's distance from the planet's center (km): bottom radius + clamp(camera y, 0, 20 km)
	glm::vec4 RayBottomRight; // w: unused
};
static_assert(sizeof(AerialPerspectiveParameters) == 64);
```

The volume: a `Texture3D` RGBA16F of 32 x 32 x 64, UAV, made with the atmosphere's resources (`AtmosphereState`) on the
first aerial frame and released with them (`m_Atmosphere.reset()`): slices 0-31 hold the in-scattered light per lux of sun
(rgb, the sun's normalized color applied as the sky-view table's is), slices 32-63 the transmittance (rgb), 512 KiB. A
1 x 1 x 2 black RGBA16F 3D texture (`m_DummyAerialVolume`, made in `CreateResources`) is bound at SRV 29 otherwise.

**The pass** `SH/IBL/AtmosphereAerial.comp` (layout of its own: CBV 0 the atmosphere constants (`AtmospherePass.glsl:10-20`),
SRV 0 transmittance table, SRV 1 multiple-scattering table, sampler 0 the atmosphere's, UAV 0 the volume (3D), push constants
`AerialPerspectiveParameters` at b1). 8x8 groups over 32 x 32 columns; each thread: the column's direction (bilinear in the
corner rays at the froxel's center UV, normalized), the cosine to the zenith `mu = direction.y` and to the sun; then for
each slice k the target distance `d_k = D ((k + 0.5) / 32)^2` (slices crowd near the camera, where the change is fastest),
scaled by the distance scale and converted to km; it integrates from the previous slice's distance to `d_k` in two midpoint
sub-steps the same integrand as the sky-view table (`AtmosphereSkyView.comp:44-56`: Rayleigh and Mie scattering with their
phase functions of the sun's angle, the sun's transmittance from the table (zero in the planet's shadow), multiple scattering
from its table, the step's own transmittance), accumulating `S` and `T`, and stores `S * SunColor` at (x, y, k) and `T` at
(x, y, k + 32). To keep one integrand, `AtmospherePass.glsl` gains `void AccumulateAtmosphereSegment(AtmosphereMedium medium,
float viewerRadius, float mu, vec3 direction, vec3 toSun, float rayleighPhase, float miePhase, float t, float length, inout vec3
transmittance, inout vec3 light)` and the sky-view pass calls it in its loop with the same arithmetic (its output stays
within 1e-6 relative: `GPU.Atmosphere`'s existing cases keep passing). `AtmospherePass.glsl` also declares the passes'
push-constant block `u_Pass` (`AtmospherePassParameters`, 22-29), and a shader may have only one push-constant block: the
block is wrapped in `#ifndef ST_ATMOSPHERE_CUSTOM_PUSH_CONSTANTS`, which `AtmosphereAerial.comp` defines before its include
and then declares `AerialPerspectiveParameters` as its own block (the other passes compile unchanged).

Recorded in `RenderFrame` after the light clusters (`SceneRenderer.cpp:3041-3046`) and before the shadows (3048): `if
(m_AerialFrame) { gpuTimer.Next(gpuStages.Aerial); RenderAerialPerspective(commandList, camera, frame); }`, so the frame's
constants are written (3001) and the atmosphere's tables are up to date (2980), and the next stage's `Next` (Shadows at
3050 or Prepass at 3075) closes the interval, so `GPU.Aerial` times the volume alone (the light clusters stay in
`GPU.Setup` or `GPU.Cull`). New stage `GPU.Aerial`: a member of `RendererGPUStages` after `Setup` (`GPUProfiler.h:22-23`),
registered right after `GPU.Setup` in `GPUProfiler.cpp:52` (step 0's table, `specs/step0.md` 8: each stream registers at its
own position so the lines never meet), and at the same position in the three lists (`EditorStateCommands.cpp:882`,
`EditorProfilerTests.cpp:91`, `FramePerfLogTests.cpp:283`) and Architecture.md's stage table (888-891). The corner rays come
from the unjittered camera (`camera.Projection * frame.View` inverted in double), so TAA's jitter never moves the volume.

**Applying it** (`SH/Include/AerialPerspective.glsl`, owned; included by `Forward.frag`):

```glsl
layout(set = 0, binding = ST_SRV(29)) uniform texture3D u_AerialPerspective;

// A surface's color seen through the air between it and the camera (only when FogParams.z > 0): dimmed by the transmittance
// and lit by the in-scattered light (per lux of sun, times EnvironmentParams.x as the sky is).
vec3 ApplyAerialPerspective(vec3 color, vec3 relativePosition, vec2 fragCoord)
{
	vec2 uv = fragCoord * u_Frame.ViewportSize.zw;
	float slice = clamp(sqrt(length(relativePosition) / u_Frame.FogParams.z) * 32.0 - 0.5, 0.0, 31.0);
	vec3 scattering = textureLod(sampler3D(u_AerialPerspective, u_LinearClampSampler), vec3(uv, (slice + 0.5) / 64.0), 0.0).rgb;
	vec3 transmittance = textureLod(sampler3D(u_AerialPerspective, u_LinearClampSampler), vec3(uv, (slice + 32.5) / 64.0), 0.0).rgb;
	return color * transmittance + scattering * u_Frame.EnvironmentParams.x;
}
```

`Forward.frag` (main, before the fog at 345-347): `if (u_Frame.FogParams.z > 0.0 && !valueView) color =
ApplyAerialPerspective(color, v_RelativePosition, gl_FragCoord.xy);`. Opaque, transparent and glass variants alike (glass
adds its transmitted light after, as it does after the fog: what lies behind it holds its own haze). The sky is not hazed
again: its table integrates the whole ray. Height fog, nearer to the camera, applies after: the result is fog over air,
which is the order along the ray. Under the atmosphere `ColorFromSky` fog keeps lighting itself from the sky at the horizon
(`Fog.glsl:36-43`), which includes the air's haze, so fog and haze meet at the horizon.

The scene binding layout (`SceneRenderer.cpp:275-307`) gains `Texture_SRV(29)` and the binding set (977-1003) the volume or
the dummy; `rebind` (2986-2988) compares `m_BoundAerialVolume` with `GetBoundAerialVolume()`. Probe bakes (`HDROutput`
frames) apply it too (it is light the probe sees).

`R/Atmosphere.h` gains `glm::dvec3 ComputePathTransmittance(const Medium& medium, double viewerRadius, double mu, double
distance)` (kilometers; the same midpoint quadrature as `ComputeTransmittance` with 40 steps over the given distance) for
the tests.

Cost: the pass is 1,024 threads of 64 segments (estimate 0.01-0.03 ms); the forward pass two trilinear 3D fetches per pixel
(estimate under 0.05 ms at 4K). Memory 512 KiB per renderer with the atmosphere.

### D8 Reflections without TAA (step 7, below the cut)

Spatial screen-space reflections: full-resolution rays with a spatial resolve, at High and Ultra, without TAA's history.

- **Row** (`R/RendererQuality.h`): `enum class ReflectionsQuality : uint8_t { Off = 0, Checkerboard = 1, Spatial = 2,
  SpatialHigh = 3 };`, preset line `Reflections: Low = Off, Medium = Checkerboard, High = Spatial, Ultra = SpatialHigh`.
  Checkerboard is 0.6.0's trace (TAA only, 24 + 6 steps, half resolution); Spatial traces 32 + 6 steps and SpatialHigh 48 + 8,
  both at full resolution, every pixel, with or without TAA. The scene still asks with `ScreenSpaceReflections`; the row only
  scales or turns off what it asks. Its inactive reason (`WithTemporalHistory`, `RendererComponents.cpp:125-130`) becomes
  "Needs TAA at this quality (Reflections Checkerboard)" only when the renderer's row is Checkerboard; the component cannot
  know the renderer, so the reason is removed and the Inspector shows a note from the Graphics Quality row instead
  (`graphics-options`' line, agreed at merge).
- **Source of the reflected light.** The current frame's color does not exist before the opaque pass that reads the
  reflections (`Forward.frag:192-196`), and compositing after it would evaluate the probes and the sky twice. So the
  spatial modes fetch the **previous frame's pre-transparent scene color**: when the frame's reflections are spatial, the
  scene-color copy glass already makes (`CopySceneColor`, half resolution with blurred mips) is made every frame after the
  sky (before transparents, `SceneRenderer.cpp:3155-3161`), and the next frame's trace, recorded earlier in that frame
  (`GPU.Reflections`, 3110-3118), reads it before the frame's own copy overwrites it: one texture (the state tracking
  orders the write after the read), no ping-pong. The trace reads it at the hit's previous position along its motion vector (motion vectors are requested whenever the frame's reflections are
  spatial). That is reprojection of last frame's radiance, not accumulation: each frame's reflection is complete in itself.
  Camera cuts and the first frame have none (confidence 0). The copy's mips give rough reflections their blur: the fetch
  level is `roughness * (mips - 1)` as glass's. The glass copy is half resolution (`SceneRendererGlass.cpp:64`, R11G11B10F
  where stored, 23-26), which would soften mirror reflections at 4K against today's full-resolution TAA-history fetch
  (`SceneRendererScreenReflections.cpp:105-114`): in spatial modes a full-resolution texture beside the copy (the
  pre-transparent HDR target copied as is, in the same step) is read for roughness 0 to 0.1 and the half-resolution blurred
  levels above it (32 MiB at 3840x2160 in R11G11B10F, 63 MiB in the RGBA16F fallback; counted in the `Reflections` row's
  `MemoryBytes`, and only at High and Ultra).
- **Trace** on a nearest-depth pyramid: `SH/Effects/DepthPyramidNearest.comp` builds max-depth (nearest with reversed-Z)
  mips of the prepass depth after the prepass (R32F, half resolution and below, 8 levels); `ScreenSpaceReflections.comp`
  gains a hierarchical march (start at level 0, step up a level while the ray stays in front of the cell's nearest depth,
  down when it passes behind) with a fixed per-pixel offset from the 4x4 order of D4 (no frame turn), refined by today's
  bisection. Output: full-resolution RGBA16F (rgb pre-exposed reflected light times confidence, a confidence).
- **Resolve** `SH/Effects/ReflectionResolve.comp`: per pixel, a 3x3 neighborhood of hits weighted by the center pixel's GGX
  lobe evaluated toward each neighbor's hit direction over that neighbor's own sampling weight (a ratio estimator), times
  depth and normal weights (as D4's), so smooth surfaces keep sharp reflections and rough ones borrow their neighbors' rays.
  Writes SRV 18's texture (which becomes full resolution in spatial modes).
- `VolumeParams.w` = 1 tells `Forward.frag` the texture is full resolution (the lookup at 194 samples it unchanged either
  way: it uses screen UVs). Stage `GPU.Reflections` as today.
- Tests: the existing `GPU.SceneRenderer.SSR` cases also run at Spatial without TAA (the mirror floor's red cube where the CPU
  predicts it, both paths; edges blend; rough surfaces keep the sky); "Without TAA spatial reflections trace and need no
  history: two frames of a still view are identical"; "Checkerboard without TAA traces nothing" (today's case 341 kept).
  Perf `PerfGPU.Renderer.SSRSpatial4K` (+GPU, provisional, Target 1.2 ms GPU stage).

### D9 Volumetric fog and light shafts (step 8, below the cut)

- **Component** (`HeightFogComponent`): `bool Volumetric = false;` `float VolumetricDistance = 128.0f;` (meters, 8 to
  1,024) `float NoiseStrength = 0.3f;` (0 to 1) `float NoiseScale = 8.0f;` (meters) `float LocalLightScattering = 1.0f;`
  (0 to 4), inactive while `Volumetric` is off. Off by default: stored scenes do not change, and Medium keeps 0.6.0's costs.
- **Row**: `enum class VolumetricQuality : uint8_t { Off = 0, Low = 1, Medium = 2, High = 3 };`, line `Volumetric: Low = Off,
  Medium = Low, High = Medium, Ultra = High`; grids Low 96 x 54 x 48, Medium 128 x 72 x 64, High 160 x 90 x 96 froxels
  (screen x, y; depth slices exponentially spaced from 0.5 m to `VolumetricDistance`: slice k ends at `0.5 (D / 0.5)^((k +
  1) / N)`), independent of the screen's resolution. Off: the analytic fog as today.
- **Passes** (stage `GPU.Volumetric`, registered after `GPU.Shadows` as step 0's table says, and recorded there: right after
  the shadows block, `SceneRenderer.cpp:3048-3073`, before the prepass; it needs the cascades, the local atlas and the
  light clusters, all made by then, and no depth):
  1. `SH/Effects/VolumetricInject.comp` (bound with the scene binding set, visibility All, plus a set with its UAV): per
     froxel two samples at fixed depth offsets (0.25 and 0.75 of the froxel) and the 4x4 order's xy offset of its cell (no
     frame turn); density = the height fog's `Density * exp(-falloff h)` times `1 + NoiseStrength (2 n - 1)` with `n` a
     procedural 3D value noise of the world position over `NoiseScale` (no time: static); scattered light = `Color` (albedo)
     times: the sun (`DirectionalLightColor`, the fog's Henyey-Greenstein phase, visibility from the cascades through
     `SH/Include/VolumetricShadows.glsl`: `SelectCascade` and one bilinear comparison tap of `u_Cascades`' map, read-only use
     of `sun-shadows`' functions and bindings, 1 beyond `ShadowDistance`), the sky (the fog's `FogLight` as
     `GetFogInscattering` without the sun), and the clustered point and spot lights of the froxel's cluster (attenuation,
     cone, one tap of their local shadow, isotropic phase, times `LocalLightScattering`). Writes RGBA16F (rgb scattered
     light per meter, a extinction).
  2. `SH/Effects/VolumetricFilter.comp`: a 3x3 tent (1, 2, 1) in x and y within each slice, weighted by extinction so thin
     fog does not bleed into dense.
  3. `SH/Effects/VolumetricIntegrate.comp`: per column front to back, `T_k = exp(-sigma_t dz)`, `S += T_acc sigma_s L (1 -
     T_k) / sigma_t`, `T_acc *= T_k`; writes RGBA16F (rgb in-scattered light, a transmittance; the fog's extinction is gray).
- **Apply** (`Forward.frag`, `Skybox.frag`): `VolumeParams.x` > 0 samples the volume at its scene slot (the next free one
  when it lands, D1: 34 after `sun-shadows`' 32 and 33) (trilinear at the pixel's UV and slice):
  `color = color * T + S` after aerial perspective; the analytic height fog continues beyond `VolumetricDistance` only
  (its start distance becomes `max(StartDistance, VolumetricDistance)` in that frame). The sky takes the last slice.
- Memory: High 160 x 90 x 96 x 8 B = 11 MiB per volume, three volumes (inject, filter, integrated) 33 MiB.
- Tests: `GPU.SceneRenderer.Fog` gains "Volumetric fog matches the analytic fog where nothing shadows it" (uniform density,
  no noise: the integrated transmittance and in-scatter equal `Fog.glsl`'s integral within 3% at slice centers); "A shaft:
  a lit window in a dark room puts light in the fog only where the sun passes" (froxels inside the beam brighter than 5x
  those beside it); "Two frames of a still view are identical"; "Off costs nothing" (no volume, no stage time); both paths.
  Perf `PerfGPU.Renderer.Volumetric4K` (+GPU; Target 1.0 ms GPU stage at Medium quality).

### D10 The baked irradiance volume (step 9, below the cut)

Sky visibility and bounce light for every surface, baked in the editor, streaming with world cells.

- **Component** `IrradianceVolumeComponent` (renderer module, `R/RendererComponents.h`): `AssetHandle Data`, `glm::vec3
  Extents = {16, 8, 16}` (half extents in meters along the entity's axes), `float Spacing = 2.0f` (meters, 0.5 to 8),
  `float Intensity = 1.0f`, `float NormalBias = 0.3f`, `float ViewBias = 0.2f`, `float BlendDistance = 2.0f`,
  `int32_t Priority = 0`. On an entity of a scene or a world cell (it streams with its cell; its asset is a reference the
  dependency scan follows through the asset property).
- **Asset** `AssetType::IrradianceVolume`, file `.stvolume` (binary): magic "STIV", format version 1, probe counts (x, y, z),
  spacing, then per probe 12 half floats (L1 spherical harmonics of RGB irradiance over pi: L0 and the three L1 terms per
  channel) and one validity byte (share of front faces it saw, 0-255). Its importer validates sizes (at most 128 x 64 x 128
  probes, counts times 25 bytes plus the header equal the file's size) and stores it; its loader rejects truncated and
  garbage bytes (`Asset.Pack` style corruption sweep in an `Asset.IrradianceVolume.Sweep` suite).
- **Bake** `lighting.bakeVolume {entities?}` (deferred, refused while playing, one undo step, `Assets/Lighting/<scene>/<id>
  .stvolume` through staging and `ImportExternal` as `lighting.bakeProbes` does): per probe, six 16 x 16 `HDROutput` faces
  with the bake renderer (`ProbeBake::GetRendererSpecification`, `ForBake()`), the forward pass writing alpha 0 on back faces
  in bake frames (`MaterialParams.w` = 1 there), projected on the GPU (`SH/IBL/VolumeProjection.comp`) into the probe's L1
  coefficients and validity, read back with `BufferReadback`, a few probes per frame (the editor keeps running; progress in
  `editor.status`); then probes with validity below 0.5 take the average of their valid neighbors (dilation, repeated until
  none is left or 8 passes). A second bake sees the first (a second bounce), as probes do.
- **GPU** (`VolumeParams.z`; appended by whichever of steps 7-9 lands first, D1): SRV 30 a 3D atlas RGBA16F (three planes along x per volume: plane 0 (L0.rgb, L1x.r),
  plane 1 (L1y.r, L1z.r, L1x.g, L1y.g), plane 2 (L1z.g, L1x.b, L1y.b, L1z.b)), volumes placed by a shelf allocator with a
  one-texel gutter; SRV 31 a structured buffer of up to `c_MaxIrradianceVolumes` 16 records (camera-relative world to grid
  transform as three rows, atlas offset and plane stride, counts, intensity, blend distance in probes, normal and view bias,
  priority), the frame's strongest in view as probes are chosen. `VolumeParams.z` their count.
- **Shading** (`Forward.frag` indirect block, `SH/Include/IrradianceVolume.glsl`): the two strongest volumes whose boxes hold
  `position + N NormalBias + V ViewBias` blend as probes do; three trilinear fetches per volume (hardware filtering inside the
  volume's box, coordinates clamped to probe centers), irradiance `E(n) = pi (L0 Y00 c0 + (2/3) sum L1m Y1m(n))` with the
  real SH constants and the cosine lobe's band factors (pi, 2pi/3). The volume's irradiance replaces the probes' and the sky's
  diffuse light by its weight; specular image light is multiplied by the ratio of the volume's irradiance luminance to the
  irradiance of the light it reflects (the probe's spherical harmonics, or the sky's irradiance map) at the same normal,
  clamped to [0, 1] for the sky (a room only darkens the sky's reflection) and [0.25, 4] for probes.
- Tests: "A closed room's walls get the window's light, not the open sky's" (the look-dev room as a GPU scene: wall
  irradiance with the volume at most 0.4 of without, the sun patch's surroundings brighter than the far corner); "Volumes of
  neighboring cells blend without a seam"; "Off costs nothing"; the bake's command tests in `GPU.Editor.Lighting`; the
  feature project's volume.

### D11 Bent normals, cone specular occlusion and Fit to Room (step 10, below the cut)

- GTAO at High and Ultra also accumulates the bent normal (each slice's visible arc bisector, weighted by its visibility,
  summed over slices, normalized, view space). The AO targets then use RGBA8_UNORM (R occlusion, GB the octahedral bent
  normal, A unused; storage `rgba8`, a core format), shader variants `Effects/GTAOBent.comp`, `AODenoiseBent.comp`; the
  denoiser filters the bent normal with the same weights. Memory at 4K: 2 x 33 MiB instead of 2 x 16.6 MiB, only at High and
  Ultra. `AOParams.z` = 1 tells `Forward.frag`.
- `Forward.frag`: the irradiance lookups (sky, probes, irradiance volume) use `normalize(N + (bent - Ng))` (the bend carried
  onto the shading normal); specular occlusion is the share of the specular cone (axis R, half angle `atan(2 alpha)`) inside
  the visibility cone (axis the bent normal, `cos(theta_v) = sqrt(1 - ao)`), approximated by the angular overlap
  `clamp((theta_v + theta_s - beta) / (2 min(theta_v, theta_s)), 0, 1)` with beta the angle between the axes; it replaces
  Lagarde's where bent normals exist.
- `lighting.fitProbe {entity, padding = 0.05}` (Edit, one undo step): `SceneRaycast::Cast` (`Editor/SceneRaycast.h`) from the
  probe's center along its six axes; `Extents` = the hit distances minus the padding, clamped to [`c_MinExtent`, 100];
  `BlendDistance` = min(1, 0.15 x the smallest extent); fails without a hit along an axis (open spaces) naming the axis. A
  "Fit to Room" button in the probe's Inspector section.
- Tests: "Bent normals tilt toward the open side of a crease"; "Cone occlusion darkens a crease's reflection more than
  Lagarde's at grazing angles"; `lighting.fitProbe` on a closed box room.

### D12 Alpha cutouts that hold (step 11, below the cut)

- `CookSettings::AlphaCoverageCutoff` (`Asset/TextureCooker.h:34-46`; 0 = off): each level after the first is rescaled by the
  factor (bisection over [0, 4], 16 steps) that keeps the share of texels with alpha at or above the cutoff equal to level
  0's, so foliage keeps its coverage at a distance instead of thinning out.
- `TextureImportSettings::AlphaCoverageCutoff` (float 0 to 1, default 0; inactive unless Usage is Color); `TextureImporter`
  version 6 ("6: AlphaCoverageCutoff"). The glTF importer cooks a masked material's base color texture with its
  `alphaCutoff` (the first masked material using it; a warning when two cutoffs differ), version 11 again if the stream's
  bump has not reached main yet, else 12.
- The prepass's test stays `alpha >= cutoff`: an alpha sharpened at the cutoff, `(a - c) / fwidth(a) + 0.5` compared with
  0.5, is the same test; it changes anything only with alpha-to-coverage, which needs MSAA (left out, plan.md 9). Coverage is
  kept by the mips alone.
- Tests: `Asset.TextureImport` "Coverage-preserving mips keep the share of texels above the cutoff in every level" (a
  noise-alpha 256 texture: each level within 1/texels of level 0's share; without it, level 4 loses more than 20%); the look-
  dev foliage card at 50 m keeps its silhouette.

## 6. Tests per step

Every new property is tested as the strata-add-component skill says: `Reflection` (exists, range), `Scene.InactiveProperties`
(its reason), `Scene.Serialization` (round trip; a 0.6.0 file without the key loads the default without a warning; out of
range values clamp), `Editor.Commands` (`component.add` with values, `component.set`, undo). The component schema snapshot
(`StrataTests/Architecture/ComponentSchema.json`, `Reflection.ComponentSchema`) is rewritten with
`--strata-test-helper=write-component-schema` in each commit that adds a property or changes a default, the commit saying so.
`AddNeutralPostProcess` (`SceneRendererTestUtils.h:126-137`) also sets `AmbientOcclusionMultiBounce = false`. Each "must
fail" is run once against the unfixed code before the commit and its result stated in the commit message.

### 6.1 Lighting debug views (step 1)

- `GPU.SceneRenderer.LightingViews` (new suite; `StrataTests/src/Renderer/GPUSceneLightingViewTests.cpp`), every case on the
  CPU and the GPU-driven path:
  - "Without a lighting view nothing is made and the image is unchanged": a frame with `LightingView` None is bit-identical
    to a renderer that never had one; `GetLightingViewStats()` empty; `LightingViewPending` false; no stats buffer.
  - "The albedo view shows the base color and marks values outside 0.03 to 0.85": three quads of albedo 0.02, 0.5 and 0.9
    (neutral post): the 0.5 quad's pixels equal `LinearToSRGB(0.5)` within 1/255; the others carry their mark colors (the
    0.9 quad's stripes); statistics: `BelowRangeShare` and `AboveRangeShare` equal the quads' screen shares within 1%.
  - "The luminance view bands EV100 and reports its percentiles": an emissive quad of 100 nits filling the view
    (`Emitting(100)`): its pixels show the band of EV100 9.64; `SurfaceEV100` p50 within 0.25 EV of 9.64 at pre-exposures
    1 and 2^-10 (forced).
  - "The AO view shows the occlusion the forward pass applies": the crease scene: the view's crease pixel equals
    `LinearToSRGB(ao)` of the AO texture there within 2/255.
  - "The indirect view leaves out direct light and emission": a lit plane under a sun and a constant ambient (no
    environment): HDR equals `albedo * ambient` within 1%; an emissive quad is black.
  - "The lighting view shades with white albedo": a plane of base color 0.2 (metallic 0, roughness 0.5) under a sun, forced
    pre-exposure 1: its HDR in the lighting view equals the normal view of the same plane with base color 1 within 1e-3
    relative (a ratio of 5 to the normal view would hold for the diffuse term only: the specular term is the same in both).
    Must fail: without the albedo override the lighting view equals the 0.2 plane's normal view.
  - "Changing the view resets TAA and leaves the exposure where it was": a TAA scene with automatic exposure settled; switch
    to the albedo view for 5 frames and back: `HistoryResets` grew by 2; the first lit frame after has the pre-exposure it
    had before (no readback of a value-view frame followed).
- `GPU.Editor.Lighting` (exists, `StrataTests/src/Editor/GPUEditorLightingTests.cpp`): "The sun to sky ratio is measured on
  flat ground" with 0.6.0's template values (procedural default colors 0.18/0.66, a 3 lux sun at 55 degrees): `SunToSkyRatio`
  1.6 within 0.3 (it documents the measure against the audit's number).
- `Editor.Viewport` (existing suite): `viewport.setSettings {debugView}` accepts the five names, saves them, rejects others;
  the description lists them. `GPU.Editor.Viewport`: `viewport.capture {debugView: "luminance"}` returns `debugView.lighting`
  with every field (nulls where the view does not measure), within the bound.
- Must fail: draw the albedo overlay from the lit HDR (no base-color write in the forward pass): the albedo case's 0.5 quad
  fails; leave out the exposure hold (`ChoosePreExposure`'s `hold` and the skipped readbacks, D2): the exposure case fails.
  (Reading the TAA-resolved image instead of the frame's own would not fail a case of flat quads, whose interiors are equal
  in both: the overlay reads `m_HDRTexture` by design, which code review checks.)

### 6.2 Energy compensation (step 2)

`GPU.SceneRenderer.Lighting` (`GPUSceneLightingTests.cpp`), a uniform environment of 1 nit (`CreateEnvironmentTexture(vec3(1),
vec3(1))`), neutral post, a forced pre-exposure of 1, spheres read at their centers (NoV = 1) and at 0.8 of their radius:

- "A white furnace keeps a rough metal's light at every roughness": metallic 1, base color white (F0 = 1), roughness 0,
  0.25, 0.5, 0.75 and 1: every value within [0.97, 1.03]; a silver-like metal (base color 0.95, the look-dev furnace's): at
  least 0.9 of its F0 at roughness 1 (the plan's acceptance, met by metals of F0 0.9 and above); copper (0.95, 0.64, 0.54):
  each channel within 3% of `(F0 x + y) (1 + F0 (1 / (x + y) - 1))` with (x, y) the BRDF table read back at the pixel's
  NoV and roughness 1 (`Renderer::ReadTexture(renderer.GetBRDFLut())`, `SceneRenderer.h:632`). A colored metal keeps less
  than its F0 at roughness 1, and correctly so (each further bounce absorbs 1 - F0): with the audit's A + B of about 0.30,
  about 0.97, 0.77 and 0.71 of F0 for copper's three channels, so "0.9 of F0 per channel" would fail a correct
  implementation. A white dielectric (metallic 0): within [0.97, 1.03] (a guard: it holds before the change too). Must fail:
  without the compensation the white metal at roughness 1 reads below 0.5 (the audit measured 0.31 of F0 for silver).
- "Direct light's specular is compensated as image light's": a white metal plane (F0 = 1, roughness 1) facing an
  orthographic camera and a sun along its normal (NoL = NoV = NoH = VoH = 1), no environment: the HDR value equals the
  CPU's `D(1, 1) * V(1, 1, 1) * 1 * Illuminance / (x + y)` (GGX D = 1/pi at alpha 1, `PBR.glsl:11-24`'s formulas evaluated
  on the CPU) with x, y read back from the BRDF table at (NoV 1, roughness 1) within 2%. Must fail: without the compensation
  it reads about x + y (0.3) of that.
- "A clear coat over a white metal keeps the furnace's light": coat strength 1, roughness 1: within [0.97, 1.03]. Must fail:
  without the base's compensation it reads below 0.6.
- `GPU.SceneRenderer.Materials`: "Glass leaves what it reflects": a glass pane over an emissive backdrop: transmitted plus
  reflected light within 1.03 of the backdrop's.
- The existing "Environment filtering conserves energy ... BRDF lookup table is plausible" (423) stays as is.

### 6.3 Ambient occlusion (step 3)

- `Renderer.AmbientOcclusion` (new, `StrataTests/src/Renderer/AmbientOcclusionFitTests.cpp`, no GPU): "The multi-bounce fit
  matches its radiosity reference and keeps its bounds": every `c_AOMultiBounceReference` sample within 0.03; `AOMultiBounce(1,
  rho) == 1`, `AOMultiBounce(x, 0) == x`, at least x, and non-decreasing in x and rho on a 21 x 10 grid.
- `GPU.SceneRenderer.AmbientOcclusion` (new; the crease scene of `GPUScenePostProcessTests.cpp:17-35` moved into a shared
  header), each case on both paths:
  - "Ambient occlusion has no pattern without TAA at any quality": 256 x 256, AO on, FXAA and no anti-aliasing, each row value:
    over the floor pixels 0.1-0.5 m in front of the box, the RMS of `ao - box7x7(ao)` is at most 0.01; two consecutive frames'
    AO textures are bit-identical. Must fail: the same case run once on step 0's tip (IGN noise, today's blur) exceeds 0.01;
    its RMS is stated in the step's commit.
  - "The quality row sets the samples": `AmbientOcclusionSamples` is 8, 24, 48 and 96 for Low to Ultra; Medium's AO texture
    under `SceneRendererSpecification{}` keeps 2 x 6.
  - "The radius stays within its screen fractions": at 1024 x 1024 (minimum 3.1 px, maximum 123 px): a 1 m box on a plane
    seen from 1 m: the darkened band (AO below 0.95) on the plane at most 123 px wide; a 50 m box at 2,000 m (camera far plane
    5,000 m, where the 1 m radius projects to under half a pixel): the plane's AO below 0.95 within 3 px of the box's base.
    Must fail: 0.6.0's radius (no minimum, the 1-pixel early-out at `GTAO.comp:60-64`) leaves the far crease at 1. (At 256
    rows the minimum would be the 1.5 px floor, too small to measure a band.)
  - "Thin occluders darken less than thick ones": a 2 cm pole 0.3 m in front of a wall: the wall's AO beside the pole's
    silhouette at least 0.9; a 1 m thick block in its place keeps the wall below 0.85 there. Must fail: decay 0.
  - "Multi-bounce lifts occlusion by the fit on bright albedo": the crease's HDR with AO over without, for albedo 0.8 and
    0.1, equals `AOMultiBounce(ao, rho)` with ao read from the AO texture, within 2%; with multi-bounce off it equals ao
    within 1%.
- The existing AO cases (`GPUScenePostProcessTests.cpp:299-365`) keep passing; if the 1 m default moves their open-floor
  point into the radius, they set `AmbientOcclusionRadius = 0.6` with a comment.
- `GPU.SceneRenderer.GPUDriven`: the default scenes (which run GTAO) pass with the new shaders.
- `Renderer.Quality` (step 0's suite): case 1's Medium values and round trip include `AmbientOcclusionQuality` (and, in
  step 5, `EnvironmentResolution`); the section accepts "Low" ... "Ultra" and refuses "Extreme". Feature test: the
  feature project's `Renderer.Quality` Custom value (step 0, `specs/step0.md` 7.11) gains `"AmbientOcclusionQuality": "Low"`
  (High is the field's default), and in step 5 `"EnvironmentResolution": "256"`, set through `project.setSettings` in a copy
  opened in the editor and copied back; `GPU.FeatureTest`'s check of `GetProjectQuality()` follows.
- `GPU.Editor.Lighting`'s baked values stay as they are: `ForBake()` takes the rows' Medium values (2.7).

### 6.4 Balanced light (step 4)

- `Editor.Templates` (`EditorTemplateTests.cpp:74-130`): the sun's intensity is 120,000, softness 0.53, direction
  `GetSkyPresetSunDirection(40, 20)` (40 degrees up) and no longer the preview's; the sky's source is Atmosphere with Clear's
  values. `GPU.Editor.Lighting`: "The Basic 3D template's sun lights flat ground at least five times as much as its sky":
  the template scene (`ProjectTemplates::Populate` into a scene with test assets) rendered with the luminance view:
  `SunToSkyRatio >= 5`. Must fail: with 0.6.0's template lines the same case reads 1.6 (6.1).
- `GPU.ProceduralSky`: "New procedural skies against the default sun keep a real ratio": a default `SkyLightComponent` and a
  default directional light at 45 degrees: `SunToSkyRatio` at least 5 (about 7 expected, D5). Must fail: with 0.6.0's
  colors it reads about 1.4.
- `Editor.Lighting` (new suite in `StrataTests/src/Editor/EditorLightingCommandTests.cpp`, shard Editor):
  - "lighting.applySkyPreset writes the preset in one undo step": a scene with a sky and a sun: each preset sets the table's
    values, keeps the light's azimuth, records one step; undo restores every value; redo applies again; a second apply of the
    same preset records nothing.
  - "It creates the sky and the sun a scene lacks, and raises a clipping Post Process": an empty scene gets "Sky" and "Sun"
    (`createdSky`, `createdLight`); a Post Process with `AutoExposureMaxEV` 12 gets 16 and is named in the result.
  - "Invalid parameters change nothing": unknown preset, a `sky` without a SkyLight, an elevation of 95.
  - "While playing it changes the running copy": the result carries the warning, `RunningCopy`, and play.stop restores.
  - `Editor.Journal` lists it among the edit commands; `EditorMcpToolsetsTests` keeps `lighting.` in `scene`.
- `Editor.UI.Inspector` (`EditorAuthoringUITests.cpp:1022`): the Sky section's four preset buttons run the command (UI origin)
  once their tab shows.
- `Asset.Gltf`: "BaseColorFactorsAreSRGB converts the base color factors from sRGB": a generated glTF with factor [0.1608,
  0.7882, 0.6706, 0.5]: on, the material's base color is (0.0222, 0.584, 0.408, 0.5) within 1e-3; off, unchanged.
  `Editor.ImportSettings`: the setting is described, accepts booleans, rejects strings, is inactive without Import Materials.
  `Asset.ImportSettings`' "Model, audio and font import settings are described" (`StrataTests/src/Asset/ImportSettingsTests.cpp:124`)
  lists the new setting in its order.
- Feature test: `Arm.gltf.meta` and `Platform.gltf.meta` gain the key (the feature test requires every default key in the
  committed `.meta`, `FeatureTests.cpp:86-99`), `Platform.gltf` with true; the Post Process's
  `AmbientOcclusionMultiBounce` false on one of the two Post Process entities (`Feature.stscene:262`, `334`).
- Samples' command scripts: Physics Playground `component.get SkyLight` Source "Atmosphere", the Sun's Intensity 120000,
  the Post Process's `AutoExposureMaxEV` 16, `AmbientOcclusionRadius` 1; Open World the Sun's Color [1, 1, 1], `asset.info
  Models/Nature/tree_default.glb` `importSettings.BaseColorFactorsAreSRGB` true, `AmbientOcclusionRadius` 1; Third-Person
  `AmbientOcclusionRadius` 1 in both scenes. The package tests and `StrataRuntime.<Sample>WindowedImage` pass.

### 6.5 Specular at 4K (step 5)

- `GPU.SceneRenderer.Materials`:
  - "Specular anti-aliasing lowers a bumpy sphere's highlight variance and keeps its light": a sphere of roughness 0.1 with a
    generated normal map of 2-texel bumps at 256 x 256, a sun, 256 x 256: over the highlight's pixels (the off image above
    half its maximum) the variance of luminance with `SpecularAntiAliasing` is at most 0.5 of without, the mean within 15%.
    Must fail: strength 0 in the shader (the case compares to itself and the variance ratio is 1).
  - "The surface target holds the roughness the forward pass shades": with TAA and SSR, a flat plane's surface target
    roughness equals its material's within half-float precision (the target is RGBA16F, `SceneRendererScreenReflections.cpp:27`;
    compare with the value rounded to half); a bumpy one's is larger by at least 0.05.
  - "Normal-mapped reflections fade below the geometric surface": a plane with normals tilted 60 degrees, a camera at a
    grazing angle, an environment bright below the horizon: the pixel's specular is at most 0.3 of its value without the
    fade (the test computes both from the CPU's expected terms).
- `GPU.SceneRenderer.Lighting`:
  - "At High the prefiltered sky is 512 per face and a mirror shows finer detail": `SetQuality` with `EnvironmentResolution`
    512: the map is 512 with 6 levels; a mirror sphere reflecting a checker environment has at least 1.5 times the gradient
    energy of 256's. The filtering case (423) runs at both sizes.
  - "Renderers of different environment resolutions do not share maps": two renderers of one map.
  - "A new EnvironmentResolution recomputes the environment once": `EnvironmentUpdates` +1, also for the atmosphere.
- `GPU.SceneRenderer.GPUDriven`: a scene builder "Lit materials" (normal-mapped bumpy spheres, metals of roughness 0.2, 0.6
  and 1, one clear-coated, a sun and a procedural sky, AO on) in the builders' list (`GPUSceneRendererGPUDrivenTests.cpp:468-
  477`): colors within 1/255, identical IDs.
- `GPU.Atmosphere`: "The largest refresh step at 512 costs no more samples than at 256" (a CPU check of
  `GetPrefilterSamples` products, and the step count unchanged).

### 6.6 Aerial perspective (step 6)

- `GPU.Atmosphere` (`GPUAtmosphereTests.cpp:77`):
  - "Aerial perspective is clear at the camera and follows the CPU's transmittance": the volume read back
    (`GetAerialVolume()`): slice 0 transmittance at least 0.9999 and in-scattering at most 1e-4 of the sky's horizon value; at
    the center column, slice 31's transmittance equals `Atmosphere::ComputePathTransmittance` over `d_31` within 1% per channel.
  - "At the end of a horizontal ray aerial perspective matches the sky at the horizon": camera 1 m up looking along +X with a
    far plane of 25,000 m, an unlit black quad at 20,000 m filling the center, `AerialPerspectiveScale` 64 (its maximum,
    `c_MaxAerialPerspectiveScale`: 1,280 km of air, beyond the 1,132 km a horizontal ray from the ground crosses to the air's
    top at 100 km, `Atmosphere.h:18-19`; the scale cannot be 2,000): its HDR equals the sky's at the same pixel without the
    quad within 5%.
  - "Off costs nothing": `AerialPerspective` false and a scale of 0 are bit-identical to each other and allocate no volume; no
    `GPU.Aerial` time; orthographic cameras have none.
  - "Transparent surfaces are hazed like opaque ones at the same distance": an unlit gray opaque quad and an alpha-blended
    one of alpha 1 and the same color side by side at 500 m, scale 8: equal within 1/255.
  - The existing atmosphere cases keep passing (the sky-view table within 1e-6 after the integrand is factored).
- `GPU.SceneRenderer.GPUDriven`: a builder "Atmosphere and aerial perspective" (scale 8).
- Feature test: `Feature.stscene`'s atmosphere sky (541-555) gets `AerialPerspectiveScale` 4; its other sky (484-505)
  `AerialPerspective` false.

## 7. Deliverables and acceptance

Each step is one or more commits that build and pass every suite (Debug, every `GPU*` suite under validation on NVIDIA, the
feature test), with the lint clean.

- **Step 1, lighting debug views**: D2, 6.1. *Accept:* 6.1's suites; the five views shown in the editor on the look-dev
  scene; `viewport.capture` returns their statistics within its bound.
- **Step 2, energy compensation**: D3, 6.2, `ThumbnailCache::c_Version` main's + 1. *Accept:* the furnace: a metal of
  roughness 1 at least 0.9 F0 L (plan 1.1, for the furnace's 0.95 metals; F0 = 1 within 3%), on the look-dev furnace row
  (`Scenes/Furnace.stscene`, Tonemapper None, so the PNG's sRGB-decoded values at the sphere centers are the linear ones).
- **Step 3, ambient occlusion**: D4, 6.3, the fit script and its outputs, the row. *Accept:* no pattern with FXAA or
  without anti-aliasing: RMS at most 1% (plan 1.1) in the GPU case and on the look-dev ThirdPerson feet and robot shadow
  shots (AO view, 3840x2160); `GPU.AO` at Medium within 1.10 of step 0's on the three samples' scene cameras at 1920x1080
  and 3840x2160 (the samples' stored radius 0.6); `GPU.AO` at High at most 1.8 ms at 3840x2160 on the Third-Person camera
  (0.79 ms at Medium in the architecture audit).
- **Step 4, balanced light**: D5, 6.4. *Accept:* the template's sun to sky ratio at least 5 (plan 1.1, measured by the
  luminance view); the Physics Playground's at least 5; the Open World's forest shot: the leaves' albedo luminance
  0.40-0.50 (0.65 before) on the albedo view's crop, and its `aboveRangeShare` at most 0.01. Six of the 35 models hold a white
  `_defaultMat` (factor 1, which stays 1 after the conversion: `mushroom_red`, `rock_largeB`, `rock_tallA`, `sign`,
  `tent_smallClosed`, `tree_detailed`); if those materials are drawn, the commit lists them as content left as it is;
  `woodInner` (0.72) and `stone` (0.70) stay under 0.85. The samples' command scripts and windowed image checks pass.
- **Step 5, specular**: D6, 6.5, the row. *Accept:* the bumpy sphere's highlight variance at most half; the look-dev
  ThirdPerson coins and PhysicsPlayground balls without single-pixel glints with FXAA at 4K; `GPU.SceneRenderer.GPUDriven`
  and `.Feedback` pass.
- **Step 6, aerial perspective, then the core's documents and pictures**: D7, 6.6, section 8, the samples' pictures after
  merging main. *Accept:* transmittance 1 at the camera, the horizon within 5%; `GPU.Aerial` at most 0.05 ms and `GPU.Opaque`
  at most 0.05 ms more at 3840x2160 on the Open World tour (look-dev); push `ci/wave7-light-and-materials`: **the core is
  complete here.**
- *Cut line.* Steps 7-11 as D8-D12 with their tests.
- **Finish**: A/Bs of 7.1, the Roadmap's "Later" for what did not land, CI green.

### 7.1 Budgets and measurements

- **Provisional metrics** (`RendererPerfTests.cpp`, entries appended to `Budgets.json` as `{ "Unit": "ms", "Provisional":
  true, "Target": ... }`). A stage's median over the measured frames is read with the helper that already exists,
  `MedianGPUStageMs(name)` (`StrataTests/src/Perf/RendererPerfTests.cpp:53-72`, which `sun-shadows` uses too): no new
  measurement API. Each scene is the 100,000-cube grid (`Perf::CreateCubeGrid`) at 3840x2160 with a shadowed sun, rendered
  with `MeasuredRendererSpecification()` (`RendererPerfUtils.h:36-41`) and its quality set to the metric's preset:
  - `PerfGPU.Renderer.AOHigh4K` (CPU), `...AOHigh4KGPU` (busy), `...AOHigh4KStageGPU` (`GPU.AO`, Target 1.8);
  - `PerfGPU.Renderer.AOMedium4KStageGPU` (`GPU.AO` at Medium, no Target: the campaign compares it with step 0's);
  - `PerfGPU.Renderer.Aerial4K`, `...Aerial4KGPU`, `...Aerial4KStageGPU` (`GPU.Aerial`, Target 0.05), under the atmosphere;
  - `PerfGPU.Renderer.AtmosphereSunMovingMaxFrameHigh` (+`GPU`): the existing case at `EnvironmentResolution` 512, Target
    1.1 times `AtmosphereSunMovingMaxFrame`'s measurement (stated when measured);
  - below the cut: `SSRSpatial4K` (+`GPU`, `...StageGPU` Target 1.2), `Volumetric4K` (+`GPU`, `...StageGPU` Target 1.0).
  No existing entry changes: the CPU metrics do not see shading changes; the GPU variants of the measured scenes are
  provisional, and the default AO radius (1.0, scenes without a Post Process) changes their `GPU.AO`, which the campaign
  measures at integration.
- **A/Bs of the stream's own stages** (information, not gates; when no campaign or other measurement holds the machine):
  the base is step 0's tip's Release build in `build/agent-light-and-materials-base` (sources exported with `git archive`,
  deleted when the stream lands; named like `sun-shadows`' base so every agent's directories stay its own). `python Tools/PerfLock.py --label light-and-materials -- python Tools/LookDev.py ...` runs
  the three samples' scene cameras and the Open World tour at 1920x1080 and 3840x2160, interleaved base and branch, three or
  more pairs, Medium and High, and the step's commit states the medians of `GPU.AO`, `GPU.Opaque`, `GPU.Aerial` and GPU busy
  with their ratios. Step 3's Medium ratio of `GPU.AO` above 1.10, or step 6's `GPU.Opaque` growth above 0.05 ms at 4K,
  is investigated and fixed before the step lands.

## 8. Documentation and skills

- **AGENTS.md** (Rendering digest; step 0 freed the room; at most one bullet, about 480 characters): "- Light keeps its energy
  and needs no history: specular is compensated for multiple scattering from the BRDF table (`GetSpecularEnergy`) and
  diffuse gets what specular leaves; ambient occlusion and other screen-space lighting denoise within the frame; specular
  anti-aliasing takes derivatives only of the primitive's own interpolants and textures. Check lighting changes with the
  `lighting`, `ao`, `indirect`, `luminance` and `albedo` debug views and the white-furnace test
  ([Lighting](Docs/DeveloperGuide.md#lighting))." Stated in full in DG, as the checklist's item 5 asks. `Docs.AgentGuide`
  must pass after merging main.
- **DG**, Rendering: a new subsection "Lighting" (the anchor the bullet links): energy compensation (the formula, where it
  applies, why diffuse keeps `1 - F` for direct light), specular anti-aliasing (the function, the derivative rule, the
  prepass's surface target), the horizon fade, the lighting debug views and their statistics, the white-furnace test as the
  way to check a shading change. "Ambient occlusion" (new: the row, the 4x4 pattern and its resolve, the radius fractions, the
  thickness heuristic, multi-bounce and its fit script, the targets and their memory). "Light units and exposure": the
  template's real values, the ratio, the presets and their table, the procedural defaults. "Fog and atmosphere": aerial
  perspective (the volume, its slices, the scale, the order with height fog, its memory and stage), the factored integrand.
  "Reflections": the `EnvironmentResolution` row and its memory, sharing by size. "Quality settings" (step 0's): the two
  rows. "GPU timing": `GPU.Aerial`. "View commands": the five debug views and `debugView.lighting`. "Importers": the glTF
  setting and version 11. "Feature test": the new values and `.meta` keys. "Scene renderer and lighting" (2970-3001): the
  pass list gains aerial perspective.
- **`Docs/Architecture.md`**: the frame's pass order (`GPU.Aerial` after the light clusters; the lighting view's statistics
  in the overlays).
- **`Docs/Manual/Lighting.md`** (owned but its Shadows section): rewrite the "small values" paragraph (40-44: new projects use
  real values now); a section "Sun and sky presets" (the four, the buttons, the command); a table of sun to sky ratios
  (clear noon 6-10, hazy 3-5, golden hour about 1-2 on flat ground, overcast below 0.3) and how to read it (the luminance
  view's legend); "Aerial perspective" in the sky section (what the scale means for small worlds); "Ambient occlusion" (radius,
  multi-bounce, quality); "Debug views" (what each shows, how to read EV100 and the albedo marks); "Materials that keep their
  energy" (rough metals look brighter than in 0.6.0, on purpose); the glTF setting in a note for Kenney-style kits.
- **Skills**: `strata-editor-automation` (the five debug views and their statistics; `lighting.applySkyPreset`),
  `strata-make-a-game` (start from a preset; real units; check the ratio and the albedo view; AO radius; aerial scale for
  small worlds), `strata-scripting` (the SkyLight's aerial properties through `SetProperty` for time of day).
- **Roadmap**: Status when the stream lands; "Later" for what fell below the cut.
- **Release-note lines** (for `Docs/Releases/0.7.0.md`, defaults that change):
  - Rough metals keep their light (multiple-scattering compensation): metal surfaces of roughness above 0.5 look up to three
    times brighter than in 0.6.0, as they should.
  - Highlights hold at 4K without sparkle (specular anti-aliasing): distant normal-mapped and curved surfaces look slightly
    rougher.
  - Ambient occlusion: denoised within the frame (no TAA needed), a 1 m default radius for new Post Process components and
    scenes without one, multi-bounce on bright surfaces (Post Process **Ambient Occlusion Multi Bounce**), quality by
    Graphics Quality (Low 1x4 to Ultra 4x12; Medium as 0.6.0).
  - New projects (Basic 3D) open under the physical atmosphere with a 120,000 lux sun at 40 degrees; new procedural skies are
    five times darker, for a real sun to sky ratio (about 7) with the default 3 lux sun; saved scenes keep their skies.
  - Sun and sky presets: Clear, Hazy, Overcast and Golden Hour, in the Sky section and as `lighting.applySkyPreset`.
  - Aerial perspective is on for atmosphere skies (Sky Light **Aerial Perspective**, **Aerial Perspective Scale**): scenes
    with the atmosphere gain distance haze; turn it off on the Sky Light to keep 0.6.0's look.
  - At High and Ultra the prefiltered sky is 512 per face: 12 MiB more per view (24 MiB with the atmosphere).
  - glTF: **Base Color Factors Are sRGB** for exporters that write sRGB colors; every glTF model re-imports once (importer
    version 11). The Open World's trees are teal-green again.
  - Lighting debug views: lighting only, ambient occlusion, indirect, luminance (EV100) and albedo, with statistics in
    `viewport.capture`.
  - Asset thumbnails re-render once (their cache's version changed).

## 9. Shared with the other streams, and merges (plan.md 4)

This stream lands fourth (last). It merges main (with `graphics-options`, `sun-shadows` and `camera-and-tone`) before it
lands, resolves the shared files, reruns everything, and retakes every sample's `Picture.png` and `Screenshots/` (taking
main's binaries on conflict, then retaking). If it is ready before a stream ahead of it, it may land first (plan.md 4); the
later one then merges main and resolves.

| File | Ours | Theirs | Resolution |
| --- | --- | --- | --- |
| `SH/Scene/Forward.frag` | the surface setup (118-127), after the punctual loop (a line after 170), the indirect block (172-266), main's roughness and debug views (303-365), aerial before the fog (345) | `sun-shadows`: the sun term (131-140, the opaque variant reads its mask at SRV 28) | regions do not overlap; keep both |
| `SH/Include/Scene.glsl` | SRV 29 (and 30, 31 and the volumetric slot below the cut); comments of `AOParams.y`, `MaterialParams.y/z/w`, `FogParams.z/w`; `ST_LIGHTING_VIEW_*` | `sun-shadows`: SRV 28, 32, 33 and `ShadowBias.z/w`; any appended frame constants | additive; no component claimed twice (checked at bc6ee123: only `AOParams.x`, `MaterialParams.x`, `FogParams.x/y` are read); volumetric fog takes the next free slot (34); `VolumeParams` is appended after theirs |
| `R/SceneRenderData.h` | `AOParameters` (32 B), lighting view and aerial structs, the claimed components' comments | their structs and appended constants | additive; the `static_assert` sums both |
| `R/SceneRenderer.cpp/.h` | SRV 29 in the scene layout and set, `UpdateEnvironment`'s aerial flag, `SetHeightFog`'s `.x/.y` writes and the new `SetAerialPerspective`, the aerial dispatch after the light clusters, the overlay section's lighting view, `CreateEnvironmentMaps` and sharing by size, `ChoosePreExposure`'s `hold` | screen shadows after the prepass, motion blur and post targets; `camera-and-tone` may change exposure adaptation near `ChoosePreExposure` | different functions; if both touched `ChoosePreExposure`, keep the hold's early return first after the reset and manual branches; re-read `RenderFrame`'s order after merging |
| `R/SceneRendererPost.cpp` | not edited | `camera-and-tone` | n/a |
| `R/RendererQuality.h/.cpp`, `Renderer.Quality` | rows `AmbientOcclusionQuality`, `EnvironmentResolution` (`Reflections`, `Volumetric` below the cut): a `PresetRow` line each, their lines in the functions of 2.7 and a `RowTraits` line each (`graphics-options`) | their rows and traits | line-level; the section's version is bumped by one at our landing (the integrator renumbers when two chose the same) |
| `E/EditorStateCommands.cpp` (`DescribeRenderStats`' `"quality"`) | `ambientOcclusionQuality`, `environmentResolution` | the other streams' keys | one key per line |
| `R/RendererComponents.h/.cpp` | `SkyLight` aerial properties and procedural defaults, the Post Process AO fields (`AmbientOcclusionMultiBounce` inside the "Ambient occlusion" section) | descriptions and `.Section` calls (`graphics-options`), the sun's properties (`sun-shadows`), the Post Process's camera fields (`camera-and-tone`) | additive; regenerate `ComponentSchema.json` after merging |
| `R/GPUProfiler.h/.cpp`, Architecture.md's stage table and the three stage lists | `GPU.Aerial` after `GPU.Setup` (and `GPU.Volumetric` after `GPU.Shadows`), step 0's positions | `GPU.ScreenShadows`, `GPU.MotionBlur` at theirs | each at its own position (step 0, `specs/step0.md` 8), so the lines do not meet |
| `StrataEditor/src/Editor/ThumbnailCache.h` (`c_Version`) | + 1 | `sun-shadows`: 4 to 5 | the later lander bumps above main's |
| `StrataTests/src/Renderer/SceneRendererTestUtils.h` (`AddNeutralPostProcess`) | `AmbientOcclusionMultiBounce = false` | `camera-and-tone`'s new effects off | one line each |
| `StrataTests/src/Perf/RendererPerfTests.cpp` | the AO, aerial and environment cases, through `MedianGPUStageMs` | `sun-shadows`' screen-shadow case, the same helper | separate cases |
| `StreamingView.h/.cpp`, `EditorViewportCommands.cpp`, `ViewportPanel.cpp` | five debug views | `shadows` | append both; the schema's enum holds all |
| `StrataEditor/src/Editor/ProjectTemplates.cpp` | the sky and sun lines | `camera-and-tone`: the Post Process lines | different lines |
| `StrataEditor/src/Panels/InspectorPanel.cpp` | the presets row in `DrawSky` | `graphics-options`: property sections, the sun's line | function-level |
| `Strata/src/Strata/Asset/GltfImporter.cpp/.h` | one setting, version 11 | none | n/a |
| `Feature.stscene`, `Budgets.json`, `Strata/CMakeLists.txt` shader list, DG, AGENTS.md, Roadmap | ours | theirs | additive; rerun `StrataTests.FeatureTest`, `Docs.AgentGuide`, `Editor.Journal`, `EditorMcpToolsetsTests` |
| Samples (scenes, command scripts, pictures) | AO radius, the Playground's sky, the Open World's sun, leaves and aerial scale | softness (`sun-shadows`), tone and motion blur (`camera-and-tone`), the Graphics row (`graphics-options`) | different properties; command scripts merged line by line; pictures retaken last |

Coordination points: `sun-shadows` step 7 (sized local lights) adds an energy-normalized lobe widening in `PBR.glsl`, which
this stream owns: it lands as their function in our file, reviewed by this stream's spec (the widening must divide the lobe's
normalization by the same factor, so energy stays; it composes with `WidenRoughness` by adding alpha^2 terms). Volumetric fog
(step 8) reads `sun-shadows`' cascade bindings and `SelectCascade` without changing them; if their SDSM fit changes the
cascade data layout, step 8 follows it.

## 10. Risks

| Risk | Mitigation |
| --- | --- |
| Energy compensation brightens rough metals the samples tuned dark | It is correct (furnace); samples are judged on the look-dev sheet; the release notes say so |
| Specular AA makes distant normal-mapped surfaces look matte or clinical-soft | The kernel's cap (0.18) and variance factor (0.25) are judged on the look-dev's ThirdPerson coins and PhysicsPlayground balls; the test bounds the highlight's energy; `SpecularAntiAliasing` compares |
| A derivative breaks pixel equality between the paths | Derivatives only of the shading normal, in quad-uniform flow before discards; the "Lit materials" GPU-driven scene proves it before landing |
| AO's 1 m radius and High's 3x8 cost too much at 4K on slower GPUs | The radius fraction caps the footprint at 0.6.0's 256 px at 4K; High measured against its 1.8 ms target; players choose Medium (one row); if High misses, it drops to 3x6 with the reason in the commit |
| The fixed 4x4 pattern shows as a grid at depth edges, where the bilateral weights reject neighbors | The 5x5 tent covers each phase once only on flat areas; at edges the following blur hides it; checked on the AO view at 4K; if visible, edges fall back to the blurred value (a weight floor) |
| The template's sky looks wrong under ACES if `camera-and-tone` lands later | Auto exposure meters the scene; the image stays sunny; `camera-and-tone` retunes its lines after merging |
| The Open World's green check fails once leaves darken and haze turns blue | Tune the aerial scale and the sun on the sheet; never the check |
| Re-importing every glTF model on first open | Stated in the release notes; it happens in the background |
| Aerial perspective's 32 slices band on long, flat ground | Quadratic slices crowd near the camera; trilinear lookup; judged on the Open World's low-sun shot; 64 slices (1 MiB) is the fallback |
| The debug views' statistics arrive late or never in captures | Bounded wait with `lightingPending`; readbacks polled, never waited for |
| The procedural defaults' change alters editor previews and thumbnails people know | Darker previews read as sunlight, intended; thumbnails' cache version bumped |
| Below-the-cut work (irradiance volume) is large | Ordered last-but-two; each step lands alone; what does not land moves to "Later" |
| AO's screen-space minimum draws dark outlines around distant objects (a drawn, clinical look) | 0.3% of the height is 6.5 px at 2160 rows; judged on the Open World's forest and mountain crops; if it outlines, the fraction goes down (0 turns it off) with the evidence in the commit |
| The template's ratio at 40 degrees misses 5 under the default haze | The audit's 6.9 was measured on the same defaults; the step may raise the elevation to 45 (D5) |
| The procedural defaults at 0.2 make editor previews of unlit scenes too contrasty | 8.1 at the preview sun's 55 degrees is a clear day's ratio; judged on the `basic3d.editor` and `lookdev` shots; the step may move the factor between 0.2 and 0.25 within the tests' bound |

## 11. Open questions and the decisions recommended

1. **"100,000 lux sun" for the template.** The atmosphere's light intensity is above the air. *Decision:* 120,000 above the
   air (about 95,000 at the ground at 40 degrees), the real sun the manual names, so presets, template and manual agree.
2. **Spatial SSR's color source.** The plan says "the frame's own pre-transparent color", which does not exist before the
   opaque pass that reads the reflections. *Decision:* the previous frame's pre-transparent scene-color copy, reprojected
   along motion (D8): no accumulation, no second evaluation of probes and sky. Revisit only if one-frame lag shows on the
   sheet in fast pans.
3. **Overcast in the atmosphere.** The physical atmosphere has no clouds. *Decision:* Overcast is a procedural preset (D5).
4. **The final blur after the 4x4 resolve** (plan: "then today's edge-aware blur"). *Decision:* keep it (5x5); if the AO view
   at 4K shows lost contact detail in creases of 1-2 px, shrink it to 3x3 (a constant) with the evidence in the commit.
5. **Should the Open World move to real sun and moon values** (DayNight's 3 lux sun, 0.05 lux moon)? Auto exposure hides the
   difference by day, and nights are only 1.6 stops darker than days within `AutoExposureMinEV` -4. *Decision:* not in this
   stream (its look is the neutral sun and the leaves); recorded in "Later" for the showcase's time of day.
6. **Multi-bounce on material AO when screen AO is off.** *Decision:* yes (`AOParams.y` independent of `AOParams.x`): a
   material's occlusion map is occlusion too.
7. **Alpha sharpening in the prepass** (plan step 11). *Decision:* not implemented, because it equals the binary test
   without alpha-to-coverage (D12); coverage-preserving mips carry the step.
8. **Step 0's names.** This spec uses the names of step 0's spec (2.7); if step 0 named them otherwise as built, step 0's
   win.
9. **The probe bake's quality rows.** *Decision (review):* `ForBake()` takes this stream's rows at their Medium values (AO
   2x6, a 256 prefiltered sky), as step 0 keeps the bake at 0.6.0's values (its Q3): the faces are 256 pixels, and
   `GPU.Editor.Lighting`'s baked values stay.
10. **The Clear preset's haze.** *Decision (review):* 1.0, the component's default (the tooltip's "clear day"), so the
    template, the preset and a new Atmosphere sky agree; the author's 0.5 would have made the template differ from a default
    atmosphere for no measured reason.
11. **The procedural sky's factor.** *Decision (review):* 0.2 instead of 0.25: it gives the atmosphere's measured ratio
    (6.9) with the default sun and keeps the tests' threshold of 5 out of the estimate's error.

## 12. Deliver in this order

1. **Step 1** (commits): the renderer enum, options and `MaterialParams.y`, the forward variants' views; the overlay and its
   pipeline; the statistics pass, buffer, readbacks and `LightingViewStats`; the history and exposure rules; the editor's
   enum, names, renderer mapping, capture wait and result, schema and descriptions; the chip and legend; tests (6.1); DG and
   skill lines.
2. **Step 2**: `GetSpecularEnergy` and the data fields; the forward pass's surface setup and indirect block; the coat; glass;
   the thumbnail version; tests (6.2).
3. **Step 3**: the row (with `GetAmbientOcclusionSamples`, the section field, the profiles); `AOParameters`; GTAO's radius,
   pattern and heuristic; `AODenoise.comp` and its registration; `Tools/FitAOMultiBounce.py` with its outputs; multi-bounce in
   the forward pass and its property; the radius default; tests (6.3); the A/B.
4. **Step 4**: `SkyPresets`; `lighting.applySkyPreset` and its tests; the Inspector row; the template and its tests; the
   procedural defaults; the glTF setting, version and tests; the feature project's metas and values; each sample's commands
   and command script, one commit per sample.
5. **Step 5**: `WidenRoughness` in the forward and prepass shaders with the specification flag; the horizon fade; the
   `EnvironmentResolution` row, sizes, samples per level, sharing, `SetQuality`; tests (6.5).
6. **Step 6**: the component's properties; `AccumulateAtmosphereSegment` factored (tests unchanged); the volume, the pass and
   the stage; SRV 29 and `AerialPerspective.glsl`; `ComputePathTransmittance`; tests (6.6); the Open World's scale; DG
   "Lighting" and the other DG sections, `Lighting.md`, Architecture, the AGENTS.md bullet, the skills; merge main, rerun
   everything, retake the samples' pictures and screenshots; push `ci/wave7-light-and-materials`. **The core is complete.**
7. *Cut line.* Steps 7-11 (D8-D12), each with its tests, docs and A/B.
8. **Finish**: A/Bs (7.1) in the final commit's message, Roadmap Status and "Later", CI green.

If time runs short, stop after a complete step; every step from 1 on leaves the branch mergeable.

## 13. First hour

1. Read plan.md 4.1 and 4.5, this spec, AGENTS.md (Rendering, Testing) and DG "Light units and exposure", "Fog and
   atmosphere", "Reflections"; confirm step 0 is on main (`git log main`), and read `R/RendererQuality.h`,
   `R/SceneRendererAmbientOcclusion.cpp` and `Scene.glsl`'s slot comment there: adjust this spec's names to step 0's.
2. `git worktree add .worktrees/light-and-materials -b agent/light-and-materials main`; submodules; `cmake --preset windows
   -B build/agent-light-and-materials`; build Debug; run `StrataTests.exe --test-suite=GPU.SceneRenderer.Lighting,
   GPU.Atmosphere,GPU.SceneRenderer.PostProcess,GPU.SceneRenderer.GPUDriven` so the baseline is green on this machine.
3. Export step 0's tip into `build/agent-light-and-materials-base` and build its Release; check
   `python Tools/PerfLock.py --status` is free before any measurement (the integration campaign may hold it).
4. Run `Tools/LookDev.py` on step 0's tip for this stream's shots (the furnace row, the room, the template, the Open World
   forest, the ThirdPerson feet) to have its own before images, or reuse step 0's tip baselines (`--quality project` and
   `medium`, kept beside the 0.6.0 baseline in `.worktrees/wave7-plan/lookdev/`, `specs/step0.md` 9.8).
5. Start step 1: add `SceneLightingView` and `MaterialParams.y`, and the albedo view's case of 6.1 first: it is the
   smallest end-to-end path (forward pass, overlay, statistics, capture).

## Review corrections

An independent review on 2026-10-27 checked this spec against the code at bc6ee123, the five commits on `agent/step0`
(NVIDIA fixes only: none touches this stream's files), `plan.md`, the audits, and the specs written beside it (`step0.md`,
`sun-shadows.md`, `graphics-options.md`; `camera-and-tone.md` did not exist yet). Most file:line claims held (Forward.frag's
regions, PBR.glsl, BRDFLut.comp, GTAO.comp, AOBlur.comp, the frame constants and their free components, the scene layout,
the environment maps and their sharing, the atmosphere's file, the template, the glTF importer, the editor's debug-view
code, the tests' files and suites). What was changed, and why:

1. **`FogParams.z/w` were zeroed every frame** (blocker). The spec wrote aerial perspective's values in `UpdateEnvironment`,
   but `SetHeightFog` runs right after it (`SceneRenderer.cpp:2980-2981`) and assigns the whole vector (1286, 1316), so
   aerial perspective would never have run. D1 and D7 now write z and w in a new `SetAerialPerspective` after
   `SetHeightFog`, whose assignments become `.x/.y` writes.
2. **Two push-constant blocks in `AtmosphereAerial.comp`** (blocker). `AtmospherePass.glsl` declares `u_Pass` (22-29); the
   aerial pass declared its own block too, which GLSL refuses. D7 guards `u_Pass` with
   `ST_ATMOSPHERE_CUSTOM_PUSH_CONSTANTS`.
3. **The copper furnace check would fail a correct implementation** (major). `1 + F0 (1/E - 1)` gives a colored metal
   `F0 (E + F0 (1 - E))`, about 0.71-0.77 of F0 for copper's green and blue at roughness 1 (E about 0.30, audit L4), not
   0.9. 6.2 now checks copper against the formula with the table read back, and the plan's 0.9 acceptance against metals
   of F0 0.9 and above (the look-dev furnace's 0.95). The direct-light case's expected value is spelled out.
4. **The capture's wait collided with an existing name** (major). `CaptureProgress::LightingFrames` and
   `c_MaxCaptureLightingFrames` already exist for local shadows and probes (`EditorViewportCommands.cpp:646-655`); D2 uses
   `LightingViewFrames` and `c_MaxCaptureLightingViewFrames`.
5. **The exposure hold was not implementable as written** (major). Raising the epoch alone does not stop readbacks recorded
   during a value view from being followed, and captures would wait on `PreExposureSettled` every time. D2 now adds
   `ChoosePreExposure`'s `hold` (early return, settled), skips the exposure readback in value-view frames, and raises the
   epoch on entering and leaving. The must-fail cases of 6.1 were replaced: reading the TAA-resolved image would not fail a
   test of flat quads.
6. **"Lighting view = 5 times the normal view" ignored specular** (major). 6.1 now compares the lighting view of a 0.2
   plane with the normal view of the same plane at base color 1.
7. **`ForBake()` contradicted step 0** (major). Step 0 keeps the bake at 0.6.0's values (its Q3; AO runs in bake frames,
   `SceneRenderer.cpp:3103`), so `ForBake()` takes this stream's rows at Medium (AO 2x6, a 256 sky), not High and 512.
8. **Quality rows in step 0's format** (major). Rows are typed `PresetRow` lines plus their lines in `GetPreset`,
   `MakeSection`, `FromSectionValue`, `ToSectionValue`, `Clamp`, `HasSameKnobs` and `DescribeRenderStats`, a `RowTraits`
   line (`graphics-options`: `GPU.AO`; the prefiltered cube's memory), the section fields as Enums with High as default, and
   the feature project's non-default Custom values (`AmbientOcclusionQuality` "Low", `EnvironmentResolution` "256"), which
   the spec had left out (2.7, D4, D6, 6.3).
9. **`GPU.Aerial`'s position** (minor). Step 0's table registers it after `GPU.Setup`, not after `Overlays`, and
   `GPU.Volumetric` after `GPU.Shadows`; D9 records volumetric fog right after the shadows block accordingly.
10. **Slot 32 is `sun-shadows`'** (minor, below the cut). `sun-shadows` takes SRV 32 and 33; volumetric fog takes the next
    free slot when it lands (34). `MaterialParams.w` (bake frames, step 9) was claimed in D10 without a line in D1's table;
    the `VolumeParams` vector is appended by whichever of steps 7-9 lands first (D1 and D10 disagreed).
11. **The sky preset's azimuth was turned 180 degrees** (major). The light's world -Z axis points away from the sun; the
    azimuth is now measured on the negated axis, as `DirectionFromAngles` defines it (`SceneRenderer.cpp:91-95`). The
    result gains `changed`.
12. **The Clear preset's haze** (minor). 1.0, the component's default and the tooltip's "clear day"
    (`RendererComponents.h:101`, `.cpp:309-310`), on which the audit measured the 6.9 ratio; a `SunSize` column was added.
13. **The procedural sky's factor** (major). 0.25 gave an estimated 5.6 against the tests' threshold of 5 at the default
    sun's 45 degrees; 0.2 gives about 7.0 (the atmosphere's measured 6.9) and 8.1 at the preview sun's 55 degrees, derived
    in D5 from the audit's numbers. Release notes say "five times darker".
14. **Thumbnails' version** (minor). `sun-shadows` bumps 4 to 5 first; this stream bumps main's value by one at landing.
15. **The AO radius test could not measure its band** (minor). At 256 rows the minimum radius is the 1.5 px floor; 6.3 runs
    at 1024 x 1024 with a 50 m box at 2,000 m, where 0.6.0's early-out leaves the crease at 1.
16. **The aerial horizon test used an impossible scale** (minor). The scale's maximum is 64; the test now places the quad at
    20 km with scale 64, beyond the 1,132 km a horizontal ray crosses to the air's top (`Atmosphere.h:18-19`). The
    transparent case got numbers.
17. **A perf helper that exists** (minor). `MedianGPUStageMs` (`RendererPerfTests.cpp:53-72`) already reads a stage's
    median; 7.1 uses it instead of a new `Perf::MeasureGPUStage`, as `sun-shadows` does.
18. **Spatial SSR would have softened mirrors** (minor, below the cut). The glass copy is half resolution
    (`SceneRendererGlass.cpp:64`); spatial modes add a full-resolution level for roughness 0-0.1 (32 MiB at 4K). The
    ping-pong was unnecessary: the trace runs before the frame's copy overwrites the texture.
19. **Smaller precision fixes**: memory of the prefiltered cube in MiB (4.0 / 16.0; 12 MiB more per view, 24 with the
    atmosphere); a dielectric's specular grows about 9%, not 5%; group-shared memory about 6 KB; the SSR surface target is
    RGBA16F (half-float precision in 6.5); `coat.SpecularEnergy` initialized for uncoated surfaces; the template's comments
    in `ProjectTemplates.h:57-71`; `EditorAssetManager.cpp`'s path; the multi-bounce reference samples moved to a test
    header; `viewport.capture`'s description growth kept small (it is in the 30 KiB `core` toolset); `Asset.ImportSettings`'
    model case lists the new glTF setting; `AmbientOcclusionMultiBounce` registered inside the "Ambient occlusion" section
    with a description (`graphics-options`' S6); line counts filled in; the base build directory renamed
    `build/agent-light-and-materials-base`.
20. **Acceptance made measurable** (minor). Step 4's "no surface above 0.85" would fail on content: six Open World models
    hold a white `_defaultMat` that stays 1 after the conversion; the acceptance is now the albedo view's
    `aboveRangeShare` at most 0.01, with those materials listed if drawn.

Not changed, but worth the implementer's eye: the final 5x5 blur after the 4x4 resolve (open question 4) and the AO
minimum radius (a new risk row) are the two places where this stream could make the image softer or outlined; both are
judged on the look-dev sheet at 3840x2160 with FXAA and without anti-aliasing before the step lands.
