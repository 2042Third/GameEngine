# Workstream `camera-and-tone`: the image of a camera (wave 7)

Owner's request (2026-10-26, verbatim in `audit_brief.md`): "the current image quality is simply not great", no TAA
upscaling ever ("The blurry mess that current gen engine has (like unreal) is completely the opposite of what this engine
is planning to be"), "great realistic graphics, which doesn't mean sharp or clinical", with the Fox Engine (Metal Gear
Solid V) as the baseline look. The plan (`plan.md` 1) reads the camera half of that look as: a filmic curve with a soft
shoulder and natural hues, restrained grading, eye-like exposure, soft bloom, motion blur, a lens's vignette, and grain,
aberration and flares only with restraint; clean at native resolution, neither smeared nor over-sharpened. Two
orchestrator notes bind this stream: the perf campaign runs first on 0.6.0's build (no campaign here), and motion blur
must stay restrained ("if it does not hold up on the before/after sheet, the template and samples ship it off").

This spec is `plan.md` 4.4 made precise enough to implement without re-deciding. Paths: `R/` is
`Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `DG` is `Docs/DeveloperGuide.md`, `PP` is
`R/SceneRendererPost.cpp` (made by step 0's S0.3). Line numbers are as of bc6ee123 unless a step-0 file is named.

The machine and its rules (AGENTS.md, `plan.md` 4.1): the Windows reference machine (i7-13700K, RTX 5090, 3840x2160 at
150%), NVIDIA Debug ctest with every `GPU*` suite under the validation layer before each commit, the worktree
`.worktrees/camera-and-tone` with its own build directory `build/agent-camera-and-tone`, no perf campaign and no
`PerfStability.py` in the stream (A/Bs of its own stages under `Tools/PerfLock.py`, as information, section 8), CI green
on `ci/wave7-camera-and-tone` before landing, `Tools/CompatLint.py` before every push.

## 1. Goal, in priority order

1. **No banding** (step 1): the 8-bit output is dithered everywhere by default; skies, fog, dark gradients and the
   vignette lose their tinted contour bands (`clarity/captures/banding_pp_sky_x8.png`). FXAA stops blurring texture as
   much (subpixel blend 0.75 to 0.375).
2. **Filmic** (step 2), a tone curve designed here on AgX's principle (log-domain per-channel curve on an inset gamut,
   outset back), fitted by a committed script against stated targets: a metered 0.18 shows at display middle gray
   (sRGB 118) with `Exposure` 0, highlights roll to white without hue skews (red stays red), shadows are deep but not
   crushed, saturation is neither AgX's washout nor ACES's punch. It is the default of new Post Process components, of
   scenes without one and of the template; ACES, AgX, Khronos Neutral and Reinhard stay bit-identical.
3. **Color** (step 3): white balance (2,000-15,000 K and tint, a von Kries scale in cone space on scene-linear light),
   slope, offset and power per channel before the curve; saturation after it (today's).
4. **Exposure like an eye** (step 4): adaptation to brighter scenes faster than to darker ones, center-weighted metering,
   the metering window as properties, and an exposure key so interiors read darker than sunlight (as far as their light
   is darker: in 0.6.0 sky light is never occluded, so rooms are lit nearly like the street until light-and-materials
   occludes it).
5. **Bloom and vignette like a lens's** (step 5): the bloom halo covers the same share of the image at every resolution
   (today it halves at 4K), with a `BloomRadius`; the vignette becomes a field-angle (cos^4) fall-off on scene-linear
   light before the curve, default 0.12.
6. **Motion blur** (step 6), restrained: tile maximum, neighbor maximum and a depth-aware gather of 8, 12 or 16 samples
   from the existing velocity buffer, the length scaled by a shutter angle (default 180 degrees), dropped below a
   resolution-relative threshold (a slow pan's center stays bit-identical to no blur, its edges blur under 4 px at 4K)
   and clamped to 2% of the image height; never in editor views or captures; off on the component; on in the template
   and the 3D samples only if the before/after sheet holds.
   The quality row `MotionBlurQuality` and the players' rows `AntiAliasing` and `MotionBlur`.
7. **Adoption** (step 7): the template and the 3D samples on Filmic at Exposure 0, the new vignette and motion blur,
   through editor commands; `Docs/Manual/Camera.md`, `AntiAliasing.md`, DG "Post-processing", the AGENTS.md bullet.

Below the cut line (section 3), in order: depth of field with a physical lens, film grain and chromatic aberration,
morphological anti-aliasing (`AntiAliasingMode::Morphological`; the plan's `SMAA`), a 3D LUT asset, lens flares.

Rules the whole stream keeps (`plan.md` 1 and 4.1): native resolution only; nothing needs temporal accumulation (the
dither, the motion blur's sample jitter and the grain are spatial or per frame, judged with TAA, FXAA and no AA);
every feature costs nothing when off (no target, no pass, no bound resource beyond a dummy a layout needs); post passes
take parameters by push constants or their own constant buffer, never `FrameConstants`; the CPU and GPU-driven paths
stay pixel-equal (post-processing is path-independent; the motion blur reads the velocity both paths write);
`SceneRendererSpecification{}` stays Medium so tests keep their meaning.

Not the goal: HDR display output, render scales, temporal upscaling, local tone mapping, a curve editor UI, per-volume
grading (post volumes), physical exposure from aperture/shutter/ISO (the DoF lens below the cut does not drive exposure).

## 2. Where we start (bc6ee123, release 0.6.0)

### 2.1 Files

| File | Lines | Role |
| --- | --- | --- |
| `R/SceneRenderer.cpp` | 3,853 | Layouts (tonemap 319-329, exposure 492-502, bloom 523-533, TAA 537-551, FXAA 553-556), `UpdateExposure` 1351-1404, `ChoosePreExposure` 1479-1537, `RenderBloom` 1539-1563, `EnsureTemporalResources` 1565-1648, `CreateRenderTargets` 2036-2184, `EnsureOutputTarget` 2186-2217, `ValidateTarget` 2533-2556, `RenderFrame` (motion decision 2744, frame constants 2808-2851, TAA 3166-3181, post 3206-3293) |
| `R/SceneRenderer.h` | 2,035 | `SceneCamera` 55-73, `SceneRendererSpecification` 98-241, `SceneRenderOptions` 278-354, `SceneRendererStats` 356-, `GetExposureBuffer` 637, `ShadowCacheKey` 1926-1936 |
| `R/SceneRenderData.h` | 508 | `TonemapParameters` 379-394, exposure constants 397-401, `ExposureParameters` 403-413, bloom 460-471 (`c_MaxBloomLevels` 6 at 463) |
| `R/RendererComponents.h` / `.cpp` | 302 / 554 | `TonemapOperator` 151-158, `AntiAliasingMode` 161-167, `PostProcessComponent` 225-266; its registration 362-457 |
| `R/GPUProfiler.h` / `.cpp` | 195 / 377 | `RendererGPUStages` 20-36, registered in order at 44-61 |
| `SH/PostProcess/Tonemap.frag` | 197 | Exposure, TAA sharpening, bloom mix, contrast, curves, saturation, vignette, sRGB encode |
| `SH/PostProcess/FXAA.frag` | 129 | FXAA on the display-encoded image (`c_Subpixel` 0.75 at 17) |
| `SH/PostProcess/Bloom.comp` | 135 | Karis prefilter, 13-tap downsample, tent upsample |
| `SH/PostProcess/Exposure.comp`, `Histogram.comp` | 89, 48 | Metering and adaptation; luminance histogram |
| `SH/Include/Exposure.glsl`, `Common.glsl` | 38, 61 | Exposure buffer slots, histogram mapping; `LinearToSRGB` 28-34, interleaved gradient noise 45-48 |
| `SH/PostProcess/TAA.comp`, `SH/Include/Motion.glsl`, `SH/Overlay/MotionView.frag` | 278, 17, 36 | The resolve; `GetCameraMotion` 8-15; the motion debug view |
| `StrataEditor/src/Editor/ProjectTemplates.cpp` | 152 | Basic 3D: description 24-27, `c_Basic3DExposureCompensation` 1 EV at 39, Post Process 69-76 |
| `StrataEditor/src/Editor/ViewportRenderer.cpp` | 242 | `GetViewportRenderOptions` 68-75, the overlay-only scene's neutral post 128-137, `Render` 116-196 |
| `StrataTests/src/Renderer/SceneRendererTestUtils.h` | 406 | `AddNeutralPostProcess` 126-137 (used 200 times) |
| `StrataTests/src/Renderer/GPUScenePostProcessTests.cpp` | 731 | `GPU.SceneRenderer.PostProcess`: `ReadExposure` 57, exposure 98-167, bloom 188-260, FXAA 262 |
| `Docs/Manual/AntiAliasing.md`, `Lighting.md` | 8.8 KB, 13 KB | FXAA or TAA; Lighting's "Exposure" section 52-64 |

### 2.2 The post chain today

- **Order** (`RenderFrame`): TAA resolve (in `GPU.TAA`, 3166-3181), then `GPU.Post` from 3208: `UpdateExposure`
  (histogram, then `Exposure.comp`), `RenderBloom`, tone mapping into the output (or into `m_LDRTexture` when FXAA
  follows, 3282-3287), FXAA (3288-3293). Overlays come after (`GPU.Overlays`, 3295-3320). `HDROutput` frames stop before
  the post (3182-3205). The post reads the resolved image (the history's newest texture) on TAA frames and
  `m_HDRTexture` otherwise, through three sets each for exposure, bloom's prefilter and tone mapping (made in
  `CreateRenderTargets` 2112-2177 and `EnsureTemporalResources` 1606-1632).
- **Tone mapping** (`Tonemap.frag` 158-196): exposed color (54-58), TAA sharpening on resolved frames (163-164), bloom mixed
  energy-conservingly at 0.04 (165-176), `Contrast` as a per-channel power around 0.18 (178-179), the operator (181-188),
  `Saturation` as a luminance mix after the curve (190-191), the vignette after the curve in UV space with
  `smoothstep(0.2, 0.8, r)` (193-194), `LinearToSRGB(clamp)` into the target (196). Its 48 bytes of push constants
  (`RenderData::TonemapParameters`, `SceneRenderData.h:379-394`) have 3 floats of padding; NVRHI caps push constants at
  128 bytes.
- **Output formats**: `c_OutputFormat` RGBA8_UNORM (`SceneRenderer.cpp:46`) for the output texture and the FXAA
  intermediate (2055); external targets must be single-sampled non-sRGB UNORM (`ValidateTarget` 2544). Nothing is
  dithered: the clarity audit measured 62 codes in 64 strictly monotonic runs, the widest 123 px, in
  PhysicsPlayground's 4K sky column (F1).
- **Curves** (`clarity/tools/curves.py`, recomputed for this spec): ACES maps 0.18 to sRGB 91 (display 0.106) with a
  log-log slope of 1.48 at middle gray and reaches display 0.98 at +6.0 stops; AgX 128 (0.215), slope 0.80, +6.2 stops;
  Khronos Neutral 105 (0.140), slope 1.29, +4.3 stops. A scene value of 0.005 (5.17 stops below gray) shows at code 0.8
  (ACES), 5.6 (AgX), 0.5 (Neutral). Auto exposure meters to 0.18 (`Exposure.comp:88`), so the template
  (`ProjectTemplates.cpp:39, 74`) and the three 3D samples set `Exposure` 1.
- **Exposure** (`UpdateExposure`): one adaptation factor `1 - exp(-dt * AutoExposureSpeed)` both ways (1370), `dt` from
  the wall clock clamped to 0.25 s (1355), the mean log luminance between the 10th and 90th percentiles (1373-1374,
  `Exposure.comp:55-74`), every pixel weighted alike (`Histogram.comp:42`), bin 0 (black) ignored. An interior and a
  sunny exterior display at the same mean (0.121 and 0.112, lighting audit L3).
- **Bloom**: a half-resolution chain of at most `c_MaxBloomLevels` 6 levels (`CreateRenderTargets` 2122-2140); level 5's
  texel is 64 output pixels at every resolution, so the halo spans about 3.3% of the width at 1080p and 1.7% at 4K
  (clarity F3). `BloomScale` = 1 / levels (3223).
- **Motion vectors** exist without TAA: `SceneRenderOptions::MotionVectors` (`SceneRenderer.h:307`) or the motion debug
  view make the frame write RG16F UV-unit motion (`c_VelocityFormat`, `SceneRenderer.cpp:44`; current minus previous,
  unjittered; 0 where nothing is drawn, where `GetCameraMotion` derives the camera's) from the renderer's motion history;
  `m_MotionFrame = options.MotionVectors || options.ShowMotionVectors || temporalWanted` (2744). A frame without history
  (first frame, resize, another render scene) has none (`SceneRendererStats::MotionHistory`, 2758). Camera cuts
  (`IsCameraCut` 2602-2605: more than 10 m or 45 degrees, `TextureStreamer.h:95-96`) do not clear the motion.
- **Stages**: `GPU.Post` times exposure, bloom, tone mapping and FXAA (`GPUProfiler.h:32`); 0.18-0.21 ms at 4K and
  0.08 ms at 1080p on the samples, 0.23-0.24 ms at 4K with FXAA (`architecture/measurements.md`). Hard-coded stage lists:
  `EditorStateCommands.cpp:884` (the `editor.perf` description), `EditorProfilerTests.cpp:91-92`,
  `FramePerfLogTests.cpp:284`, `.claude/skills/strata-editor-automation/SKILL.md:488`, `Docs/Architecture.md:894`.

### 2.3 Tests, helpers, samples and the template

- `AddNeutralPostProcess` (`SceneRendererTestUtils.h:126-137`) turns off the curve, auto exposure, vignette, bloom, AO
  and AA; three files define a local `ExactSpecification()` (texture streaming off: `GPUGameUITests.cpp:104`,
  `GPUReflectionProbeTests.cpp:28`, `GPUSceneFogTests.cpp:21`); 136 `SceneRenderer` constructions in 32 test files, most
  with the default specification.
- `GPU.SceneRenderer.GPUDriven`'s scenes have no Post Process (they run the defaults on both paths and compare within
  1/255): default changes move both sides alike.
- `Editor.Templates` checks the template's ACES and `Exposure` 1 (`EditorTemplateTests.cpp:125-127`).
- The feature test fails when a property keeps its default on every entity (`FeatureTests.cpp:151-192`); its active Post
  Process runs TAA under Custom and an inactive "Alternate Grading" holds other values (DG 640-643).
- Sample Post Process entities and their command-script checks: ThirdPerson's MainMenu `13AB59872DE91DE7`
  (`ThirdPersonCommands.json.in:6`) and Course `4BC76BF041EAD086` (`:14`), OpenWorld's Island `F7486ABFA648EBD7`
  (`OpenWorldCommands.json.in:4`), PhysicsPlayground's Main `8B22F284D166ECC6` (`PhysicsPlaygroundCommands.json.in:2`).
  Stored values: ACES, `Exposure` 1 (0 in MainMenu), `Vignette` 0.2, `AutoExposureSpeed` 1.5, TAA Sharp; Tetris ACES with
  manual exposure 0 and `Vignette` 0.2. Every sample's sun is 3 to 4.4 lux (dim units; light-and-materials changes the
  template to 100,000 lux, not the samples).

### 2.4 What step 0 gives this stream (assumed API)

The step-0 spec is written beside this one; this stream assumes, and its first commit checks:

- S0.3: post-processing from the TAA resolve to FXAA lives in `PP` (`R/SceneRendererPost.cpp`), the same code as
  3206-3293 and its helpers (`UpdateExposure`, `RenderBloom`, the post layouts and sets), with no pixel change. Step 0's
  spec (`specs/step0.md` 6.1) names the plan's "RenderPost" as a pair: `RenderPostHDR(commandList, postProcess, resolved)`
  (exposure, bloom) and `RenderPostDisplay(...)` (tone mapping, FXAA), plus `CreatePostResources`, `CreatePostTargets`,
  `ReleasePostTargets` and `DrawFullscreen`; `gpuTimer.Next(gpuStages.Post)` stays in `RenderFrame` before
  `RenderPostHDR`. Where this spec says `RenderPost` it means that pair; where it cites `SceneRenderer.cpp` lines of that
  code, the code is in `PP`. `ChoosePreExposure` (1479-1537) stays in `SceneRenderer.cpp`.
- S0.4: `R/RendererQuality.h/.cpp` with `RendererQualitySettings`, a preset table laid out one row per line with its four
  preset values, `SceneRendererSpecification::Quality` (default Medium), `SceneRenderer::SetQuality`, the
  `Renderer.Quality` project section (version 1, `Preset` plus one field per row, validated and upgraded by the section's
  `Read`), followed by `GameRenderer` and `ViewportRenderer`; `SceneRendererSpecification::ForThumbnails()` and
  `ForBake()`. If a name differs, this stream follows step 0's names.
- S0.5: no scene binding slot for this stream (post passes have their own layouts).
- S0.6: `Tools/LookDev.py` with `Tools/LookDev/Shots.json`, live and still shots at 3840x2160 and 1920x1080 with TAA,
  FXAA and no AA, `renderStats.gpuMs` medians of 5, crops at fixed regions; this stream appends shots (section 5.6.7).
- graphics-options (lands first): property sections in the Reflection layer (a builder call naming a section; this spec
  names the section of each new property), `IGameGraphics` options by name (so new rows reach players and scripts
  without ABI changes), and the Third-Person Adventure's settings menu.

## 3. Order and the cut line

Ordered by what the owner notices per unit of risk, each step landing working, tested and documented:

1. Dither the output (and FXAA's subpixel; the tonemap's constant buffer).
2. Filmic and the template's Exposure 0.
3. White balance, slope, offset, power.
4. Exposure: speeds, metering, percentiles, key.
5. Bloom by screen fraction; the lens vignette.
6. Motion blur, its quality row and the players' rows.
7. Adoption: the 3D samples, the manual pages, the AGENTS.md bullet.

**Cut line.** Below it, in this order, each done only when everything above has landed; what is left moves to the
roadmap's "Later" when the wave lands:

8. Depth of field with a physical lens.
9. Film grain and chromatic aberration.
10. Morphological anti-aliasing (`AntiAliasingMode::Morphological`; the plan's `SMAA`, renamed in review: 5.10).
11. A 3D LUT asset.
12. Lens flares.

## 4. Where each setting lives

### 4.1 Post Process properties (new and changed)

All on `PostProcessComponent` (`R/RendererComponents.h`), registered in `RegisterRendererComponents` in the section
named (graphics-options' groups); new enum values are appended, never renumbered. "Default" is the struct default:
what new components, `component.add`, scenes without a Post Process and files without the property get.

| Property | Type, range | Default | Section | Inactive reason | Step |
| --- | --- | --- | --- | --- | --- |
| `Tonemapper` | enum, appends `Filmic` = 5 | **Filmic** (was ACES) | Tone and color | | 2 |
| `Toe` | float, Slider 0-1 | 0.5 | Tone and color | "Only the Filmic curve has a toe (Tonemapper is X)" | 2 |
| `Shoulder` | float, Slider 0-1 | 0.5 | Tone and color | as `Toe` | 2 |
| `WhiteBalanceTemperature` | float, Range 2000-15000, step 50 | 6500 | Tone and color | | 3 |
| `WhiteBalanceTint` | float, Slider -1-1 | 0 | Tone and color | | 3 |
| `ColorSlope` | vec3, Range 0-4 | (1, 1, 1) | Tone and color | | 3 |
| `ColorOffset` | vec3, Range -0.1-0.1 | (0, 0, 0) | Tone and color | | 3 |
| `ColorPower` | vec3, Range 0.2-5 | (1, 1, 1) | Tone and color | | 3 |
| `AutoExposureMetering` | enum `ExposureMetering` {`Average` 0, `CenterWeighted` 1} | CenterWeighted | Exposure | "Auto Exposure is off" | 4 |
| `AutoExposureLowPercentile` | float, Range 0-0.49 | 0.1 (0.6.0's) | Exposure | as above | 4 |
| `AutoExposureHighPercentile` | float, Range 0.51-1 | 0.9 (0.6.0's) | Exposure | as above | 4 |
| `AutoExposureSpeed` | unchanged field; now the rate toward darker scenes | 1.5 | Exposure | as above | 4 |
| `AutoExposureLightSpeed` | float, Range 0-20 | 3 | Exposure | as above | 4 |
| `AutoExposureKey` | float, Range 0-0.5 | 0.12 | Exposure | as above | 4 |
| `BloomRadius` | float, Range 0.01-0.25 | 0.0592593 (64/1080) | Bloom | "Bloom is off" | 5 |
| `Vignette` | unchanged field and range, new model | **0.12** (was 0.2) | Lens | | 5 |
| `MotionBlur` | bool | false | Motion | | 6 |
| `MotionBlurShutterAngle` | float, Range 0-360, step 1 | 180 | Motion | "Motion Blur is off" | 6 |
| `MotionBlurMaxLength` | float, Range 0.005-0.05 | 0.02 | Motion | as above | 6 |
| `FilmGrain` | float, Slider 0-1 | 0 | Lens | | 9 |
| `ChromaticAberration` | float, Slider 0-1 | 0 | Lens | | 9 |
| `AntiAliasingMethod` | enum, appends `Morphological` = 2 | FXAA (unchanged) | Anti-aliasing | | 10 |
| `ColorLookup` | asset, `AssetType::ColorLookup` | none | Tone and color | | 11 |
| `ColorLookupIntensity` | float, Slider 0-1 | 1 | Tone and color | "No Color Lookup is set" | 11 |
| `LensFlare` | float, Slider 0-1 | 0 | Lens | | 12 |

Depth of field lives on `CameraComponent` (section 5.8). Scripts reach every property through
`Entity::GetProperty`/`SetProperty`; no host function is added (the ABI keeps its version).

Registration (review): new properties are appended at the end of PostProcess's registration
(`R/RendererComponents.cpp:362-439`; registration order never changes, so files and the schema snapshot keep theirs),
each group after a `.Section("<section>")` call (graphics-options' `PropertyBuilderBase::Section`, its D11; sections need
not be contiguous), each with a description: graphics-options' description test fails for a PostProcess property without
one. Descriptions the steps below do not spell out:
- `AutoExposureMetering`: "How the image is metered: Average weighs every pixel alike; CenterWeighted counts the middle
  of the image most and its edges a quarter, as a camera's center-weighted meter".
- `AutoExposureLowPercentile` / `HighPercentile`: "The darkest (brightest) share of the metered pixels that is ignored, so
  deep shadows (light sources) do not move the exposure".
- `AutoExposureSpeed` (new text): "How fast the exposure follows a darker scene (the image brightens), per second; 0
  jumps at once"; `AutoExposureLightSpeed`: "How fast it follows a brighter scene (the image darkens), per second, as eyes
  leaving a tunnel; 0 jumps at once".
- `AutoExposureKey`: "How much a scene's brightness against a gray card in the sun shows: 0 meters every scene to
  middle gray; 0.12 shows an interior 5 stops below the sun's card 0.6 stops darker (at most 2 darker, 1 brighter). Needs
  a directional light".
- `MotionBlur`: "Blurs what moves on screen over the shutter's share of each frame, as a film camera does; editor views
  never blur, and players can turn it off".

Every new property gets the strata-add-component skill's tests: `Reflection` (exists, range), `Scene.InactiveProperties`
(its reason), `Scene.Serialization` (round trip; a 0.6.0 file without the key loads the default without a warning; out
of range values clamp), `Editor.Commands` (`component.set`, undo), and the component schema snapshot
(`StrataTests/Architecture/ComponentSchema.json`, `Reflection.ComponentSchema`) rewritten with
`--strata-test-helper=write-component-schema` in each commit that adds a property or changes a default, the commit
saying so.

### 4.2 Rows of `RendererQualitySettings` (and fields of `Renderer.Quality`)

One line per row in step 0's preset table, all four presets on it. The players' rows can only turn off what a scene
uses ("comfort"), or override the anti-aliasing method (preference and cost); every preset is `Scene` for them.

| Row | Values (file names) | Low | Medium | High | Ultra | Step |
| --- | --- | --- | --- | --- | --- | --- |
| `MotionBlurQuality` | `Off`, `Samples8`, `Samples12`, `Samples16` | Off | Samples8 | Samples12 | Samples16 | 6 |
| `AntiAliasing` | `Scene`, `Off`, `FXAA`, `TAA` (`Morphological` appended in step 10) | Scene | Scene | Scene | Scene | 6 |
| `MotionBlur` | `Scene`, `Off` (`SceneEffectChoice`) | Scene | Scene | Scene | Scene | 6 |
| `DepthOfField`, `FilmGrain`, `ChromaticAberration` | `Scene`, `Off` | Scene | Scene | Scene | Scene | 8, 9 |

Each landing bumps the section's version by one (`plan.md` 4.1; the integrator renumbers when two streams chose the
same). Values saved before a row existed: a simple section upgrades by its fields, so a missing field takes its default
(step 0's spec 7.6: the field defaults are High's values; here `Samples12`, `Scene`, `Scene`); under a named preset a row
that follows the preset reads the preset's value anyway. Medium's `Samples8` is new cost on Medium for scenes that turn
motion blur on (about 0.05 ms at 1080p, estimated), inside the wave's "Medium at most 1.15 times step 0's" acceptance
(`plan.md` 1.1).

**graphics-options' row contract (review).** graphics-options lands first and adds `RendererQuality::RowTraits`, one
line per field in `c_RowTraits` (its spec, "The traits table and functions"); its S1 coverage test fails for a row
without traits, and its test "Rows that do not follow the preset are read under every preset" fails for a comfort row
that `FromSectionValue` reads from the preset table. This stream's lines:
- `MotionBlurQuality`: `FollowsPreset` true, `GPUStage` "GPU.MotionBlur", `MemoryBytes`: 0 for `Off`, else `W * H * 8`
  (the RGBA16F motion blur target) plus the two tile targets (`2 * ceil(W / 8) * ceil(H / 8) * 8`), for a scene that
  blurs (the page's caption says "when a scene uses motion blur").
- `AntiAliasing`: `FollowsPreset` false, `GPUStage` empty (its cost is the chosen method's: `GPU.TAA` or `GPU.Post`),
  `MemoryBytes` null.
- `MotionBlur`: `FollowsPreset` false, `GPUStage` "GPU.MotionBlur", `MemoryBytes` null (counted by
  `MotionBlurQuality`).
- `FromSectionValue` reads `AntiAliasing` and `MotionBlur` from the value's fields under every preset (default `Scene`);
  `GetPreset` gives `Scene` for both in every preset. Below the cut, `DepthOfField`, `FilmGrain`, `ChromaticAberration`
  follow the same rule.

### 4.3 Renderer specification and render options

| Setting | Where | Default | Who changes it |
| --- | --- | --- | --- |
| `DitherOutput` | `SceneRendererSpecification` | true | exact-pixel tests (`Tests::ExactSpecification()`) |
| `MotionBlur` | `SceneRenderOptions` | true (the scene's motion blur applies) | `ViewportRenderer`: only the live views' scene camera while playing (never the capture renderer); `ThumbnailCache` and `ProbeBake`: false |
| `AdaptationSeconds` | `SceneRenderOptions`, `std::optional<float>` | unset: the wall clock, at most 0.25 s | tests and tools that step time |
| `Lens` | `SceneRenderOptions`, `std::optional<CameraLens>` | unset (no depth of field) | programs viewing through a scene camera (step 8) |

## 5. Steps

### 5.1 Step 1: dither the output, FXAA's subpixel, the tonemap's constant buffer

**Commit 1a, no pixel change: the tonemap's parameters move into a constant buffer.** Steps 2-5 and the cut-line items
need about 160 bytes (a 3x3 color matrix alone is 48), past NVRHI's 128-byte push constants.

- `R/PostProcessData.h` (new, owned by this stream) receives `TonemapParameters` (renamed `TonemapConstants`),
  `ExposureParameters`, `BloomParameters`, the exposure and histogram constants and `c_Bloom*` from
  `SceneRenderData.h:379-413, 460-471` (deleted there; the other streams append at that header's end, so the move does
  not meet their edits). `SceneRenderData.h` includes it, so existing includes keep compiling.
- `TonemapConstants` is std140 and grows by appending (each later step adds its block at the end):

  ```cpp
  // PostProcess/Tonemap.frag's constant buffer (binding ST_CBV(0)), written once per frame. std140: vec4-aligned blocks.
  struct TonemapConstants
  {
  	float BloomIntensity;      // 0: fraction of the light scattered into the glow; 0 = off
  	float BloomScale;          //    normalizes the bloom levels' sum (1 / BloomLevelSpan, step 5)
  	float Saturation;
  	float Contrast;
  	int32_t Operator;          // 16: TonemapOperator
  	uint32_t BloomAdditive;
  	float InversePreExposure;
  	float Sharpening;
  	float DitherLevels;        // 32: 2^bits - 1 of the target this pass writes; 0: no dither (step 1b)
  	float Vignette;            //     step 5 changes its model
  	glm::vec2 VignetteTangents; //    step 5: tan of the half field of view per axis; (0, 0) until then
  	float FilmicToeSlope;      // 48: step 2 (PostProcessMath::FilmicCurve)
  	float FilmicWhiteStops;
  	float FilmicShoulderPower;
  	uint32_t ColorFlags;       //     step 3: bit 0 = grading applies (else skipped: bit-identical to none);
  	                           //     step 6: bit 1 = the source's alpha is the motion blur mask (no sharpening there)
  	glm::vec4 ColorRows[3];    // 64: step 3: white balance times slope, as rows (w unused)
  	glm::vec4 ColorOffset;     // 112: step 3: xyz times middle gray, added before the power
  	glm::vec4 ColorPower;      // 128: step 3
  };                           // 144 bytes; static_assert on the size and on each block's offset
  ```

  In commit 1a the struct is its first 48 bytes: today's nine values in three rows, then `DitherLevels` 0 until 1b and
  `VignetteTangents` zero until step 5 (their comments name the step that fills them); steps 2 and 3 append their blocks.
- `m_TonemapConstantBuffer`: `nvrhi::BufferDesc().setByteSize(sizeof(TonemapConstants)).setIsConstantBuffer(true)
  .setIsVolatile(true).setMaxVersions(c_FrameConstantVersions)`, created with the other post resources; written with
  `commandList->writeBuffer` right before the tonemap draw (once per frame, `ST_CORE_ASSERT`ed like
  `WriteFrameConstants`, 1425-1430). Its versions need no reservation of their own: every frame that tone-maps also
  writes `FrameConstants` at least once, and `ReserveFrameConstantVersions` (1406-1423) starts a frame only while the
  submitted writes leave room for `c_MaxFrameConstantWrites` more, so at most `c_FrameConstantVersions -
  c_MaxFrameConstantWrites + 1` = 15 frames (`SceneRenderer.cpp:55-57`) hold a version of this buffer, within its 16; a
  comment and a `static_assert` beside the buffer state the dependency. The tonemap layout (`SceneRenderer.cpp:319-329`)
  replaces `PushConstants(0, ...)` with `VolatileConstantBuffer(0)`; every tonemap set (`CreateRenderTargets`
  2168-2177, `EnsureTemporalResources` 1623-1632) binds `ConstantBuffer(0, m_TonemapConstantBuffer)`. `Tonemap.frag`
  declares `layout(set = 0, binding = ST_CBV(0), std140) uniform TonemapConstants { ... } u_Tonemap;` and keeps its body.
- FXAA gets a layout of its own with `PushConstants(0, sizeof(FXAAParameters))` (`struct FXAAParameters { float
  DitherLevels; float Padding0, Padding1, Padding2; }`, 16 bytes); the copy pass (`EnsureOutputTarget` 2199-2205, the
  draw at 3317) gets `m_CopyBindingLayout` (SRV 0, sampler 0), which `m_FXAABindingLayout` was until now.
- Proof of no change: every `GPU*` suite passes unchanged (`GPU.SceneRenderer.PostProcess`, `.TAA`, `.TAAQuality`,
  `.GPUDriven`, `GPU.FeatureTest`, `GPU.Editor.Viewport`).

**Commit 1b: the dither.**

- `SceneRendererSpecification::DitherOutput` (bool, default true): "The 8-bit (or 10-bit) output is dithered: triangular
  noise of one code per channel in the encoded domain, the same every frame, so gradients show no bands. Tests that
  compare exact pixels turn it off (`Tests::ExactSpecification`)." Not a quality row and not a person's setting.
- **Domain and amplitude.** In the display-encoded (sRGB) domain, right where the value is quantized, with one code of
  the destination's precision. A value `v` in codes (`encoded * levels`, clamped to [0, levels]) becomes `q / levels`,
  with `u0`, `u1` the pixel's two independent uniform values of the channel in [0, 1) and `n = u0 + u1 - 1` (triangular,
  in (-1, 1)):
  - where `0.5 <= v <= levels - 0.5`: `q = floor(v + n + 0.5)`, a triangular dither of one code whose noise power does
    not depend on `v` (no noise modulation) and which never leaves [0, levels] there, so the mean of `q` is exactly `v`;
  - within half a code of black or white: `q = floor(v + u0)`, a rectangular dither whose mean is also exactly `v` and
    which keeps 0 at 0 and `levels` at `levels`.
  (Review: an amplitude narrowed toward the ends, `a = clamp(min(v, levels - v), 0, 1)`, biases the mean toward the end
  code: `v` = 0.3 averages 0.056 and every `v` below 0.25 becomes 0, leaving the darkest gradients undithered.) The shader
  quantizes itself: `q / levels` lies within half an ulp of code `q`, which implementations that round to nearest (every
  driver the GPU suites run on) store as `q`; the twin test below proves it on each device.
- **Noise.** `SH/Include/Dither.glsl` (new):

  ```glsl
  // A 32-bit integer finalizer of the xorshift-multiply kind (three xorshifts, two multiplications by odd constants
  // chosen when this was written and fixed since: Renderer.PostProcessMath's statistics accept them).
  uint MixBits(uint x);
  // Uniform value in [0, 1) for a pixel and a stream: MixBits(pixel.x ^ MixBits(pixel.y ^ MixBits(stream))), top 24 bits.
  float PixelRandom(uvec2 pixel, uint stream);
  // Triangular noise in (-1, 1) per channel: streams 0-5 (two uniform values per channel), independent channels.
  vec3 TriangularNoise(uvec2 pixel);
  // The dithered code values of an encoded color for a target of `levels` + 1 codes per channel (255, 1023), as
  // normalized values; levels 0 returns the color unchanged (no quantization in the shader: 0.6.0's path).
  vec3 DitherToCodes(vec3 encoded, uvec2 pixel, float levels);
  ```

  The noise is static (no frame index): at one code it is invisible as a pattern, and still frames, captures and the
  CPU-against-GPU comparisons stay deterministic. A C++ twin in `R/PostProcessMath.h` (`glm::vec3
  PostProcessMath::TriangularNoise(glm::uvec2 pixel)`, `glm::vec3 DitherToCodes(glm::vec3 encoded, glm::uvec2 pixel, float
  levels)`) uses the same integer arithmetic; the tests compare them. Film grain (step 9) uses other streams of the same
  hash with a frame salt.
- **Which pass dithers.** The last pass that writes the 8-bit (or 10-bit) image:
  - Without FXAA (TAA frames, AA off): `Tonemap.frag` writes the output with `DitherLevels` = the levels of the
    framebuffer it draws into.
  - With FXAA: the tonemap writes the intermediate `m_LDRTexture`, which becomes **`R10G10B10A2_UNORM`** (same 4 bytes
    per pixel as RGBA8; a mandatory color-attachment and linear-filter format in Vulkan, so MoltenVK has it), undithered
    (`DitherLevels` 0); `FXAA.frag` reads it and dithers its output (`FXAAParameters::DitherLevels`). Dithering the
    8-bit intermediate and again after FXAA would double the noise; an undithered 8-bit intermediate would bake the bands
    in before FXAA. FXAA's thresholds (`c_EdgeThresholdMin` 0.0312, about 8 codes) never see the noise as edges.
  - Copies (`Copy.frag`, output to target) and overlays are not dithered: overlays are exact display colors drawn over
    the dithered image.
  - `GetDitherLevels(nvrhi::Format)` in `PP`: 255 for 8-bit UNORM formats (RGBA8, BGRA8, the swapchain's), 1023 for
    `R10G10B10A2_UNORM`, 0 (no dither) for every other format: 16-bit UNORM and the packed 5-6-5, 5-5-5-1 and 4-4-4-4
    formats, which `ValidateTarget` (2544: normalized, unsigned, not sRGB) also accepts. `DitherOutput` false makes it 0
    everywhere. With overlays the tone mapping writes `m_OutputTexture` (RGBA8, 255) and the copy (3317) carries it
    into the target unchanged.
- **FXAA's subpixel blend** `c_Subpixel` 0.75 becomes 0.375 (`FXAA.frag:17`, its comment saying why: at 0.75 it cost
  19-22% of the texture detail of an image without AA, clarity F6).
- **Stats**: `SceneRendererStats::Dithered` (the frame's last 8-bit pass dithered).
- **Tests helpers**: `Tests::ExactSpecification()` in `SceneRendererTestUtils.h` (texture streaming off, `DitherOutput`
  false) replaces the three local copies; the sweep converts every test that compares exact codes or uses a tolerance
  below 2/255 against a value computed on the CPU (the GPU suites' failures after 1b list them; comparisons of two
  renderers of the same specification need nothing, the noise being static). `AddNeutralPostProcess`'s comment names the
  specification flag. The overlay-only empty scene (`ViewportRenderer.cpp:128-137`) stays black: black is never dithered.
- **Thumbnails (review)**: the dither changes every thumbnail's pixels, so this commit bumps `ThumbnailCache::c_Version`
  (`StrataEditor/src/Editor/ThumbnailCache.h:104`, 4 today) by one and extends its comment ("5: dithered output"), as
  AGENTS.md requires ("thumbnails that would look different bump `ThumbnailCache::c_Version`"). Steps 2 and 5 bump it
  again (Filmic for scenes without a Post Process: thumbnails render the model's scene with the default component and
  `PreviewEnvironment`, `ThumbnailCache.cpp:873-876`; the lens vignette and bloom). Other streams bump it too: the
  integrator takes the highest number plus one at each merge.

Tests (new suite `GPU.SceneRenderer.Camera`, `StrataTests/src/Renderer/GPUSceneCameraTests.cpp`; CPU suite
`Renderer.PostProcessMath`, `PostProcessMathTests.cpp`, in the existing `Renderer*` shard):

| Case | Checks | Fails without the change |
| --- | --- | --- |
| `Renderer.PostProcessMath` "The dither noise is triangular and uncorrelated" | Over 512x512 pixels: each channel's histogram within 0.03 (absolute; the density's peak is 1) of the triangular density per 0.1-wide bin; correlation between channels and between horizontal and vertical neighbors below 0.03 in magnitude | no twin |
| "Dithering keeps black, white and the mean" | `DitherToCodes(0)` = 0 and `(1)` = 1 exactly; over 512x512 pixels the mean code equals `v` within 0.01 for `v` = k + {0, 0.1, 0.25, 0.5, 0.75, 0.9} with k in {0, 1, 100, 254} (255 levels) and {0, 1022} (1023 levels) | no twin; with the narrowed amplitude, k = 0 fails (0.25 averages 0) |
| `GPU.SceneRenderer.Camera` "Dithering keeps a gradient's mean and breaks up its bands" | A ramp of 4 codes over 512 px (an emissive quad sampling a two-texel texture with linear filtering, Tonemapper None, manual exposure): every 32-px column band's mean within 0.1 code of the analytic ramp; with `DitherOutput` false the band means step by up to 0.5 | the bands stay |
| "The dither on the GPU is its CPU twin" | A flat emissive field at code 100.5 and one at 3.25 (and 0.3, the end rule): at least 99.9% of the pixels equal `PostProcessMath::DitherToCodes` fed the value the shader sees (the emissive's half-float HDR value, `glm::packHalf1x16`, through the shader's sRGB encode in float), the rest within one code | |
| "FXAA frames dither once, after FXAA" | AA FXAA, a flat field at code 100.5: the mean within 0.06 code (the 10-bit intermediate holds 100.46), codes only 99-101; the intermediate's format is `R10G10B10A2_UNORM` | the intermediate check |
| "A 10-bit target is dithered at 10-bit precision" | Rendering without overlays into an `R10G10B10A2_UNORM` framebuffer target: every pixel within one 10-bit code of the exact ramp (8-bit dithering errs by up to 4) and every 32-px band's mean within 0.1 10-bit code of it (an undithered 10-bit output steps by up to 0.5) | |
| "DitherOutput false quantizes without noise" | Flat fields at codes 100.5, 3.25 and 254.6, the flag off: one code everywhere in each field (on: several) across TAA, FXAA and no AA; black and white exact in every case, the flag on or off. (Not "0.6.0's output": FXAA frames differ from 0.6.0 by the 10-bit intermediate and the 0.375 subpixel blend whatever the flag.) | |
| `GPU.SceneRenderer.PostProcess` "FXAA smooths aliased edges and keeps flat areas" (existing, 262) | Still holds at 0.375 (its edge-softness expectation adjusted if it measured the subpixel term) | |

Docs: DG "Post-processing" (new, section 10) "Output and dither"; `AntiAliasing.md` "FXAA or TAA" gains FXAA's lower
subpixel blend and why; the tests' helper in DG "Readback, the game renderer and tests".

Acceptance (reference machine): PhysicsPlayground's sky at 3840x2160 (S0.6's `pp.scene` shot, its `sky` crop holding
the clarity audit's column x = 3000, y 0-500): the widest run of one 8-bit RGB value along the column is under 16 px,
measured as the audit measured 0.6.0's 123 px (`clarity/tools/bands.py`, which compares RGB triples). This is the plan's
number and it holds with margin (review; it replaces a "9x9 mean within 0.25 code of a cubic fit, runs under 32 px"
criterion: a sky column is no cubic, and the "three pixels in four keep the center code" argument is per channel, while
with independent noise per channel a pixel repeats its neighbor's triple with probability at most 0.75^3 = 0.42, so the
expected widest run in 500 px is about 7). As information, the commit also states the widest single-channel run and the
x8 contrast crop beside `clarity/captures/banding_pp_sky_x8.png`. PhysicsPlayground's tiled floor keeps at least 0.88 of
the no-AA image's texture detail with FXAA (clarity's `tools/detail.py`, both images dithered; 0.6.0: 0.81; unverified
that 0.375 reaches it: if it does not, 0.25, the low end of the clarity audit's 0.25-0.375, is the value, never lower, with
both measurements in the commit); dither cost below 0.01 ms at 4K (no new pass).

### 5.2 Step 2: Filmic

**The curve** (per channel, on the inset color; display-linear output `y` from an exposed scene value `x`; `t =
log2(x / 0.18)`, stops from middle gray):

- Shadows, `t < 0`: `y = 0.18 * 2^(Ct * t + (C0 - Ct) * tau * (exp(t / tau) - 1))`. The log-log slope is `C0` at middle
  gray and turns to `Ct` (the toe slope) below it over about `tau` stops: no black floor, no clipping, and deep
  shadows only a little deeper than a straight power law.
- Highlights, `0 <= t < Tw`: `y = 0.18 + 0.82 * (1 - (1 - t / Tw)^q)`, with `q = C0 * 0.18 * ln 2 * Tw / 0.82` so the
  slope matches `C0` at middle gray; `y = 1` from `Tw` on, reached with zero slope (a soft shoulder). `x <= 0` gives 0.
- Look parameters: `Ct = C0 * (0.85 + 0.5 * Toe)` and `Tw = 6.5 + 3 * (Shoulder - 0.5)` stops (5 to 8). `q >= 1` holds
  on the whole Shoulder range as long as `C0 >= 1.315` (at Shoulder 0, `q = 0.7607 C0`), so the shoulder never clips;
  T2's range [1.35, 1.45] keeps it.
- Base constants, the fit's starting values: `C0 = 1.40` (between ACES's 1.48 and Khronos Neutral's 1.29: punchy enough
  for a sunlit scene without ACES's crushed blacks), `tau = 1.5`. Middle gray is anchored 1:1: `y(0.18) = 0.18`, sRGB
  117.6, so a metered scene shows its average at display middle gray with `Exposure` 0, and the template's +1 EV goes.
- With `Toe` and `Shoulder` at 0.5 (a planning prototype, `C0` 1.40): -5.17 stops (0.005) shows at code 2.7 (ACES 0.8,
  AgX 5.6), -3 stops at 23, +2 stops at 190 (ACES 189), +4 at 230 (ACES 240), +6 at 252. Toe 0 lifts 0.005 to code 6.9
  (AgX-like), Toe 1 crushes it to 1.1 (ACES-like); Shoulder 0 reaches white at +5 stops, Shoulder 1 at +8.

**The gamut** (AgX's principle: a per-channel curve on an inset gamut desaturates bright colors toward white instead of
skewing their hue; the outset gives back the saturation the inset took):

- `M_in` row `i` = `(1 - s_i) e_i + s_i (1/3 (1, 1, 1) + rho_i (e_{i+1} - e_{i-1}))` (indices mod 3): each channel takes
  a share `s_i` of the mean, rotated by `rho_i` toward its next or previous neighbor. Every row sums to 1, so neutral
  colors pass unchanged and the gray ramp is exactly the curve. `M_out` is the inverse of a matrix of the same form with
  its own `s'_i`, `rho'_i`. Parameter bounds: `s` in [0, 0.45], `rho` in [-0.15, 0.15].
- Full operator: `v = M_in * max(color, 0)`, `y_c = Curve(v_c)` per channel, `o = clamp(M_out * y, 0, 1)` (display
  linear), then today's saturation and the sRGB encode.

**The fitting script `Tools/FitFilmic.py`** (standard library only; outputs committed; CI never runs it; header
docstring like `Tools/GenerateBrandAssets.py`'s):

- Fits `s_i, rho_i, s'_i, rho'_i` (12 values), `tau` in [0.75, 3] and `C0` in [1.35, 1.45] (T2) by Nelder-Mead (implemented in
  the script), restarted from 8 seeded starts (deterministic), minimizing the worst hue deviation + 0.1 times the sum of
  deviations + 40 times the saturation band's violations, subject to the targets below; it exits 1 and writes nothing
  when a target is not met.
- Targets, stated in the script's header and printed with the fitted values and ACES's and AgX's (transcribed from
  `Tonemap.frag` as `clarity/tools/curves.py` does) for comparison:
  - T1 middle gray: `y(0.18) = 0.18` (by construction; checked).
  - T2 contrast: the log-log slope at middle gray is `C0` within 0.01, `C0` in [1.35, 1.45].
  - T3 white: a neutral at `+Tw` is 1.0; at +6 stops at least 0.955 (sRGB code 250) with Shoulder 0.5 (the planning
    prototype: 0.976, code 252).
  - T4 toe: with Toe 0.5, 0.005 shows at sRGB code 2 to 4.
  - T5 hue: ten test colors (scene-linear sRGB, scaled to luminance 0.18): red (1, .05, .05), green (.05, 1, .05), blue
    (.05, .05, 1), yellow (1, 1, .05), cyan (.05, 1, 1), magenta (1, .05, 1), orange (1, .4, .05), skin (.8, .5, .35),
    foliage (.2, .4, .08), sky (.25, .45, .9), each ramped from -4 to +7 stops in quarter stops. Hue is measured in ICtCp
    (ITU-R BT.2100: BT.709 to BT.2020 by BT.2087's matrix, the LMS matrix, PQ at a display white of 100 nits; chosen for
    its hue linearity, where CIELAB bends blue toward purple). The displayed hue stays within 10 degrees (the seven
    saturated colors) and 6 degrees (skin, foliage, sky) of the scene color's own hue until the displayed chroma falls
    below 15% of the ramp's largest. With this metric ACES drifts 26 degrees on red and 26.6 on blue, AgX 11.3 and 10.4,
    a per-channel curve without inset 22.8 on orange (planning measurements); a crude random search of the inset/outset
    in planning reached 11.6 at worst (red 9.5, blue 10.4, orange 11.6, skin 6.4), so the targets are expected to be
    reachable by the real fit (unverified until it runs).
  - T6 path to white: every test color at `+Tw + 1` stops has every channel at least 0.95.
  - T7 saturation: at 0 stops, each color's displayed ICtCp chroma divided by that of the same color scaled linearly to
    the same display luminance lies in [0.90, 1.20] (AgX 0.54-0.96 washes out; ACES 0.93-1.22).
  - T8 shape: monotonic per channel and in luminance over [-12, +10] stops for Toe and Shoulder on a 0.25 grid; `q >= 1`.
- Outputs, the same numbers (9 significant digits) in two generated files, each headed "Generated by
  Tools/FitFilmic.py from its targets; do not edit":
  - `SH/Include/FilmicConstants.glsl`: `const mat3 c_FilmicInset`, `c_FilmicOutset` (column-major as GLSL builds them),
    `const float c_FilmicContrast` (`C0`), `c_FilmicToeWidth` (`tau`), `c_FilmicMiddleGray` (0.18).
  - `R/FilmicConstants.h`: `namespace Strata::FilmicConstants { inline constexpr glm::mat3 Inset ...; Outset; Contrast;
    ToeWidth; MiddleGray; }`.

**Code.**

- `R/PostProcessMath.h/.cpp` (new): `struct FilmicCurve { float ToeSlope; float WhiteStops; float ShoulderPower; };`
  `FilmicCurve MakeFilmicCurve(float toe, float shoulder)` (clamps both to [0, 1]); `float EvaluateFilmicCurve(float x,
  const FilmicCurve&)`; `glm::vec3 ApplyFilmic(const glm::vec3& exposed, const FilmicCurve&)` (inset, curve, outset,
  clamp: the CPU reference of the shader).
- `SH/Include/Filmic.glsl` (new): `float FilmicCurve(float x, float toeSlope, float whiteStops, float shoulderPower)`,
  `vec3 TonemapFilmic(vec3 color, ...)`, including `FilmicConstants.glsl`. `Tonemap.frag`: `#define ST_TONEMAP_FILMIC 5`
  and its case in the switch (181-188); `TonemapConstants` gains the Filmic block (offset 48), filled from
  `MakeFilmicCurve(postProcess.Toe, postProcess.Shoulder)`.
- `TonemapOperator::Filmic = 5` (appended; `{ "Filmic", 5 }` in the registration at 366-367, its tooltip naming every
  curve: "Filmic: the engine's camera-like curve, middle gray shown as middle gray, a soft shoulder that takes bright
  colors to white without changing their hue; ACES: punchier, darker mid-tones (needs about +0.8 EV); AgX: flatter and
  less saturated; Khronos Neutral: keeps base colors up to the highlights; Reinhard; None"). The struct default becomes
  `Filmic`: new components, `component.add` and scenes without a Post Process; scenes that stored a curve keep it (every
  saved Post Process stores `Tonemapper`).
- `Toe`, `Shoulder` properties (4.1), tooltips: "How deep the shadows go: higher crushes the darkest tones toward black,
  lower keeps more detail in them (Filmic only)"; "How softly highlights roll off to white: higher reaches white more
  stops above middle gray, keeping more highlight detail (Filmic only)".
- The template (`ProjectTemplates.cpp`): `c_Basic3DExposureCompensation` and line 74 go (Exposure 0), `Tonemapper` =
  Filmic, the description (24-27) says "Filmic tone mapping".
- DG "Light units and exposure" (its exposure paragraph, 3423-3433 today): the tonemapper's middle gray per curve
  (Filmic 0.18, ACES 0.106, Khronos Neutral 0.14, AgX 0.215) and that Filmic needs no compensation.
- `ThumbnailCache::c_Version` + 1 ("Filmic for scenes without a Post Process"; step 1's note).

**Why it stays a camera's look, not a clinical one.** Middle gray is where a light meter puts it, contrast at middle gray
sits between film-print punch and a flat neutral curve, the toe keeps shadow detail without lifting blacks to gray, the
shoulder rolls specular glints, skies and fire smoothly to white over 6.5 stops instead of clipping, and saturated lights
desaturate toward white as film does rather than skewing (red stays red, skin and foliage hold their hue). There is no
sharpening beyond TAA Sharp's measured, halo-free one, no local contrast, and the restrained defaults (bloom 0.04,
vignette 0.12, grain and aberration off) leave the scene's own light to make the image.

Tests:

| Case | Checks | Fails without |
| --- | --- | --- |
| `Renderer.PostProcessMath` "Filmic meets its targets" | T1-T8 on `ApplyFilmic` with the committed constants (the hue and saturation of the ten ramps recomputed in C++: the BT.2100 conversion lives in the test file) | no Filmic |
| "Filmic's look keeps the curve monotonic and soft" | Toe, Shoulder at 0, 0.5, 1: monotonic, `q >= 1`, white exactly at `Tw` | |
| `GPU.SceneRenderer.Camera` "Filmic on the GPU matches its CPU reference" | 64 emissive patches (8 colors by 8 exposures, manual exposure, no bloom, vignette or dither): every pixel within one code of `ApplyFilmic` | |
| "Automatic exposure under Filmic shows middle gray at sRGB 118 with Exposure 0" | `Tests::ExactSpecification()`, a uniform emissive wall whose luminance is a histogram bin's center (`2^HistogramBinToLog2(k)`: the metering returns bin centers, and half a bin, 0.055 stops, times Filmic's slope 1.4 would move the result by up to 2.7 codes), auto exposure, Filmic: the center pixel 117-119 | ACES gives 91 |
| "The earlier curves are unchanged" | ACES, AgX, Khronos Neutral and Reinhard at gray 0.005-11.5 give the codes of the transcription in the test (ACES 0.18 to 91, AgX to 128, Neutral to 105, Reinhard to 109) | |
| `Editor.Templates` (`EditorTemplateTests.cpp:125-127`) | Filmic, Exposure 0 | ACES, 1 |
| `Scene.InactiveProperties` (`ReflectionTests.cpp:515-523`) | `Toe`, `Shoulder` inactive unless Filmic, with the reason naming the curve; the motion and new exposure properties' reasons (steps 4, 6) | |

Feature test (review: the entities named): the active "Environment" Post Process (`Feature.stscene:262-293`, manual
exposure, TAA under Custom) keeps AgX (`FeatureTest/Scripts/ComponentFeatures.cpp:92` keeps checking 3); the inactive
"Alternate Grading" (`:334-365`, automatic exposure) changes from Khronos Neutral to Filmic with `Toe` 0.6 and `Shoulder`
0.4, so the Filmic values sit where they apply. Khronos Neutral needs no entity (the feature test counts non-default
values, not enum members).

Acceptance: middle gray 117-119 on the GPU (plan: 112-124), white at +6 stops at least 250, the toe at 2-4, T5-T7 as
fitted and printed in the commit message beside ACES's and AgX's values; cost at 4K within `Post4K`'s target (section 8).

### 5.3 Step 3: white balance, slope, offset and power

- **Order in `Tonemap.frag`** after the bloom mix (and, from step 5, the vignette): `c1 = ColorRows * c` (white balance
  times slope), `c2 = max(c1 + ColorOffset.xyz, 0)`, `c3 = 0.18 * pow(c2 / 0.18, ColorPower.xyz)`, then today's
  `Contrast` (178-179), the curve, `Saturation`. `ColorFlags` bit 0 is set only when any of them differs from neutral,
  so a default component renders bit-identically to 0.6.0's path (the branch is uniform).
- **White balance** (`PostProcessMath::GetWhiteBalanceMatrix(float kelvin, float tint)`, double precision, cached per
  value pair): the light to show as white has the chromaticity of the white locus (below) at `kelvin`, moved along the
  locus's normal in CIE 1960 (u, v) by `0.02 * tint` (positive tint assumes a greener light, so the image turns magenta;
  negative, green: as cameras and photo editors label it). Its XYZ (Y = 1) and the reference white's (sRGB's D65, x
  0.3127, y 0.3290: the white the sRGB matrices are built on) go into cone space through the CAT02 matrix (CIE
  159:2004); the gains are the reference's cone responses over the light's (von Kries); `M = M_XYZ_to_sRGB * CAT02^-1 *
  diag(gains) * CAT02 * M_sRGB_to_XYZ`. The locus's 6500 K entry is D65 itself, so 6500 K and tint 0 give gains of
  exactly 1 (the function returns the identity there without arithmetic), and a light the setting names shows as r = g =
  b. Lower temperatures (a 3200 K tungsten light) cool the image, higher ones warm it, as a camera's white balance.
  (Review: with the Planckian locus at 6500 K as the reference, which lies about 0.003 in (u, v) below D65, a light
  the setting names would show as sRGB (1.04, 0.98, 1.03), a magenta cast on every neutral it was meant to neutralize,
  and the test "it neutralizes the light it names" could not pass.)
- **The white locus** comes from `Tools/GenerateWhiteBalance.py` (standard library; output committed;
  `R/WhiteBalanceLocus.inl`) at every 5 mireds from 60 to 510 (91 entries: 16,667 K to 1,961 K), each entry's (u, v) and
  unit normal; linear interpolation in mireds. An entry is the CIE daylight locus at or above 5000 K (200 mireds; the
  CIE's D-illuminant chromaticity polynomial in `T * 1.4388 / 1.4380`, the correction that makes nominal 6500 K exactly
  D65, as CIE 15 defines D65 at 6504 K), the Planckian locus at or below 4000 K (250 mireds; Planck's law integrated
  against the CIE 1931 2-degree color matching functions, the CIE's published 5 nm table from 380 to 780 nm embedded in
  the script as data), and between them the two blended linearly in mireds, so the locus has no jump. Normals are the
  normalized perpendicular of the blended curve's tangent (central differences of the table).
- Slope folds into the matrix (`diag(ColorSlope) * M`); `ColorOffset` is in units of middle gray (0.1 adds a tenth of
  middle gray's light: it tints the shadows most); `ColorPower` is a per-channel power around middle gray (1 keeps it).
- Tooltips: "Temperature in kelvin of the light that shows as white: lower values cool the image (3200 for tungsten
  light), higher warm it; 6500 is neutral"; "Tint: positive adds magenta, negative green"; "Slope: per-channel gain
  before the tone curve"; "Offset: light added per channel, in fractions of middle gray (tints the shadows most)";
  "Power: per-channel contrast around middle gray".

Tests: `Renderer.PostProcessMath` "White balance at 6500 K and tint 0 is the identity" (exact), "it neutralizes the
light it names" (the locus color at 3200 K, 4500 K and 9000 K, as sRGB-linear, maps to r = g = b within 1e-3; fails with a
Planckian reference), "lower temperatures cool the image and tint moves between green and magenta" (signs), "the locus
table matches Planck's law below 4000 K and the daylight formula above 5000 K" (5 entries each recomputed in C++, the
Planckian ones with the 10 nm CMF subset in the test, within 5e-4 in u and v; the 6500 K entry equals D65 within 1e-5),
"the locus has no jump" (consecutive entries closer than 0.004 in (u, v)); `GPU.SceneRenderer.Camera` "Grading at its
defaults renders bit-identically to no grading; white balance, slope, offset and power follow the CPU math" (patches
within one code). Feature test: `WhiteBalanceTemperature` 5600, `WhiteBalanceTint` 0.1, `ColorSlope` (1.02, 1, 0.98),
`ColorOffset` (0, 0, 0.002), `ColorPower` (1, 1, 1.05) on the active Post Process. DG and `Camera.md` "Color".

Acceptance: identity exact at defaults; cost within `Post4K`.

### 5.4 Step 4: exposure like an eye

- **Adaptation speeds**: the existing `AutoExposureSpeed` (1.5 per second) becomes the rate toward darker scenes (dark
  adaptation: the image brightens slowly); `AutoExposureLightSpeed` (3 per second) the rate toward brighter ones (light
  adaptation: faster, as eyes leaving a tunnel). `ExposureParameters` replaces `AdaptFactor` by `AdaptLight` and
  `AdaptDark` (each `1 - exp(-dt * speed)`, 1 on a reset or at speed 0); `Exposure.comp` uses `AdaptLight` when the
  target is above the previous adapted value and `AdaptDark` otherwise (79-88).
- **Time**: `SceneRenderOptions::AdaptationSeconds` (`std::optional<float>`): "the time the automatic exposure adapts
  over since this renderer's previous frame; unset, the wall clock between its frames (at most 0.25 s)" (1355).
- **Metering** (`Histogram.comp:42`): `atomicAdd(s_Bins[bin], weight)` with weight 1 for `Average` (0.6.0's histogram
  bit for bit) and, for `CenterWeighted`, `uint(round(16 * (0.25 + 0.75 * exp(-r^2 / (2 * 0.25^2)))))` (4 to 16), `r` the
  distance from the image center in units of the image height (`(uv - 0.5) * vec2(aspect, 1)`): the middle third of the
  frame decides, the edges count a quarter. At 3840x2160 the sum stays below 2^32 (8.3 M pixels times 16).
- **Window**: `LowPercentile` and `HighPercentile` from the properties (clamped so high - low >= 0.02).
- **Key** ("so interiors read darker than sunlight"), scene-relative so it works in the samples' dim units and in real
  ones: the reference is the luminance of a gray card (albedo 0.18) facing the scene's directional light, `L_ref =
  Luminance(frame.DirectionalLightColor.rgb) * 0.18 / pi` (that constant is the light as `Forward.frag` applies it,
  atmosphere transmittance included: `SceneRenderer.cpp:2353`, `SceneRendererAtmosphere.cpp:335`). `Exposure.comp`
  computes `keyStops = clamp(AutoExposureKey * (adapted - log2(L_ref)), -2, +1)` and the multiplier `Compensation *
  0.18 / 2^adapted * 2^keyStops`; without a directional light (or a black one) the key is 0. The CPU passes
  `KeyReferenceLog2 = log2(L_ref)` from the frame's constants as `CollectLights` and the atmosphere left them (the
  editor's preview sun included, `SceneRenderOptions::PreviewEnvironment`). A sunlit exterior meters
  within about a stop of the card (key near 0); an interior five stops below it shows 0.6 stops darker; a night under
  the moon (the moon is the light) is not darkened by the key (the scene's `Exposure` does that); a snow field above the
  card brightens slightly, as a photographer compensates. `ExposureParameters` (64 bytes now):

  ```cpp
  struct ExposureParameters // PostProcess/Histogram.comp and Exposure.comp (push constants)
  {
  	float MinLog2, MaxLog2, AdaptLight, AdaptDark;                          // 0
  	float Compensation, LowPercentile, HighPercentile, InversePreExposure;  // 16
  	float KeyStrength, KeyReferenceLog2, KeyMinStops, KeyMaxStops;           // 32: key 0 without a light
  	float MeteringAspect; uint32_t Metering; float Padding0, Padding1;      // 48: width / height; ExposureMetering
  };
  ```
- `RenderPostHDR` gains a parameter `float keyReferenceLog2` (`RenderFrame` computes it from `frame`; with no light,
  `KeyStrength` is 0 and the value unused). The exposure buffer (4 floats, `SceneRenderer.cpp:510-516`) holds `keyStops`
  in slot 2 (`ST_EXPOSURE_KEY` in `Exposure.glsl`, `c_ExposureKeyStops` beside `SceneRenderData.h:397-398`; the manual
  path's write at 1361 already puts 0 there); the readback copies 3 floats (its creation at 519, `Record` at 1399 and
  `ChoosePreExposure`'s `Read` at 1519-1520, which still follows only the multiplier);
  `SceneRendererStats::ExposureLog2Luminance` and `ExposureKeyStops` (from the newest readback, frames late, as the
  pre-exposure's). `DescribeRenderStats` (`EditorStateCommands.cpp:206`) adds `exposure: { ev100 (log2 L + 3), keyStops,
  cardEv100 (log2 L_ref + 3, the sun's gray card; null without a light) }` to `renderStats` (`exposure` null with manual
  exposure, whose slot 0 holds no metering; `SceneRendererStats::ExposureKeyReferenceLog2` carries the card), so agents
  tuning a scene read what it metered and what the key compared it with.
- **Tests keep 0.6.0's metering (review):** `AddNeutralPostProcess` (`SceneRendererTestUtils.h:126-137`) also sets
  `AutoExposureMetering = Average`, `AutoExposureKey = 0` and `AutoExposureLightSpeed = AutoExposureSpeed`, so the
  existing tests that turn automatic exposure on after it (98, 143, 580, ...) keep 0.6.0's exposure exactly; the new
  tests below set the new values explicitly. (light-and-materials adds `AmbientOcclusionMultiBounce = false` to the same
  helper: separate lines.)

Tests (`GPU.SceneRenderer.PostProcess`, with `ReadExposure` at 57, reading 3 floats where the key is checked):

| Case | Checks | Fails without |
| --- | --- | --- |
| "Automatic exposure adapts to brighter scenes faster than to darker ones" | `AdaptationSeconds` 0.1: after a dim-to-bright switch one frame moves the adapted value 0.259 of the way (speed 3), after bright-to-dim 0.139 (speed 1.5), within 0.01 | one speed |
| "Center-weighted metering favors the middle of the image" | A square viewport, a patch 8 stops brighter than its surround over the central ninth, window 0.1-0.9: each mode's metered log2 (slot 0) equals the CPU's weighted window mean of the HDR image read back (`ReadHDRImage`, the same weights and bins) within 0.06 stops (half a bin); CenterWeighted at least 0.5 stops above Average (planning estimate 1.0: the patch holds 11% of the pixels, 21% of the weight, so the window keeps 1% of it under Average and 11% under CenterWeighted) | Average's weights: CenterWeighted equals Average |
| "The metering window is a property" | Low 0.5 meters the brighter half: higher than 0.1's | |
| "The exposure key darkens what meters below the sun's gray card" | No sky light (ambient 0), a floor of albedo 0.18 under a sun 60 degrees high: keyStops equals `clamp(0.12 * (slot 0 - log2(L_ref)), -2, 1)` computed on the CPU from the readback within 0.01, and lies in [-0.05, 0] (the floor meters 0.2 stops below the card); the same floor, the camera looking at it in the shade of a wide roof that hides the sun and the sky's `AmbientColor` set so the shaded floor's luminance is 1/32 of the card's: keyStops -0.6 within 0.02 and the CPU formula within 0.01 (not "a box lit through an opening": without bounce light only the sun's patch inside is lit, everything else is black and left out of the metering, so such a box meters its patch, above the card); no light, or key 0: exactly 0; a 65,536 lux and a 1 lux sun give the same keyStops within 0.01 | no key |
| "Automatic exposure meters a 65,536 lux scene 16 stops below a 1 lux one" (existing, 580) | Still exact (the helper keeps the key 0) | |

Acceptance (review: the plan's "interiors read darker" needs light the room does not get yet). In 0.6.0 sky light is
never occluded: the look-dev room's walls receive about 23% of the sun's irradiance (lighting audit L1, its 6x6x3 m room),
so the room meters only about half a stop below the sun's gray card and the key (0.12) darkens it by about 0.05 EV; the
occlusion that makes interiors dark is `light-and-materials`' irradiance volume, below its cut. So this step's acceptance
is the mechanism, measured where it can be: on the look-dev `lookdev.room` and `lookdev.overview` shots, each shot's
`renderStats.exposure.keyStops` equals `clamp(0.12 * (ev100 - cardEv100), -2, 1)` within 0.05 (the displayed means are
not compared: their tone-curve and histogram differences reach 0.1 EV without any key, 0.121 against 0.112 in 0.6.0);
the GPU test's shaded floor (no sky light, an ambient 5 stops below the card) shows the full effect. The commit states the room's and the exterior's displayed means (0.6.0: 0.121 and
0.112) as information, and the release note says interiors darken with the key as far as their light is darker
(`plan.md` 1.1's look depends on `light-and-materials`' sky occlusion). Adaptation half-lives as specified; no cost
change beyond noise.

### 5.5 Step 5: bloom by screen fraction, the lens vignette

**Bloom.**

- The chain keeps its half-resolution start and is allocated with every level down to 4 px, at most `c_MaxBloomLevels` =
  **8** (`CreateRenderTargets` 2122-2140), so a change of `BloomRadius` never reallocates. Per frame, `s = clamp(log2(
  BloomRadius * height), 1, levelsAllocated)`, `L = ceil(s)` levels are used and the deepest is weighted `phi = s - (L -
  1)` (in (0, 1]); `TonemapConstants::BloomScale = 1 / s`. `BloomParameters` gains `float UpsampleWeight` (32 bytes):
  1 for every upsample except the one adding level `L - 1`, which uses `phi` (`Bloom.comp:118-121`: `color =
  imageLoad(...) + UpsampleWeight * Upsample(...)`). `RenderBloom` (1539-1563) dispatches `L` levels.
- The deepest level's texel spans `2^s` output pixels, `BloomRadius * height`. Review: the default is 64/1080
  (0.0592593, shown 0.059) instead of 0.06, and `s` within 1e-4 of an integer is snapped to it, so at 1080 rows `s` is 6:
  6 levels with `phi` 1 and `BloomScale` 1/6, 0.6.0's chain and bloom bit for bit (no seventh level of weight 0.02 at
  Medium's usual resolution); at 2160 rows `s` is 7 (7 levels, 128 px; 0.6.0: 6 levels, 64 px). Every level weighs the
  same (one octave each), so 4K gains one finer octave (a seventh of the energy) and keeps the same reach; the mix stays
  energy-conserving.
- Small viewports get a small halo, as the goal says (at the tests' 64 px, `s` = 1.9: 2 levels where 0.6.0 used 4). The
  range reaches 0.25 so tests can ask for 0.6.0's chain: the bloom tests at `c_SceneTestSize` ("Bloom spreads bright light
  around its source" 188, which checks the glow 6 px from the source; "with a threshold" 222; "Bloom and ambient
  occlusion keep working after a resize" 364) set `BloomRadius = 0.25` (s = 4, 0.6.0's four levels at 64 px) in this
  commit, with the reason in a comment.
- Tooltip: "How far the glow reaches, as a fraction of the image height: the same share of the image at every
  resolution".

**Vignette.**

- Moves before the curve, after the bloom mix, on scene-linear light: `color *= mix(1, cos^4(theta), Vignette)`, with
  `tan(theta) = length(ndc * VignetteTangents)` and `cos^4 = 1 / (1 + tan^2)^2`. `VignetteTangents` = `(1 / P[0][0], 1
  / P[1][1])` of the camera's unjittered projection (the tangents of the half field of view per axis; aspect-correct by
  construction); orthographic views use `(aspect, 1) * tan(30 degrees)`. The old ring (193-194) goes.
- Semantics: 1 is an ideal lens's natural fall-off (cos^4 of the field angle), 0 none; 0.12 darkens the corners of a
  60-degree, 16:9 view by 9.9% (about 0.15 EV), of a 45-degree view by 7.9%, and the side edges' centers much less than
  the corners (the ring darkened them 18.7% against 20%, clarity F4). The default becomes 0.12; stored values keep
  theirs under the new model (a stored 0.2 darkens a 60-degree view's corners 16.5% instead of 20%).
- Tooltip: "Natural darkening toward the image's corners, as a lens's (cos^4 of the field angle): 0 none, 1 a lens's
  full fall-off; 0.12 darkens the corners of a 60 degree view by about 10%".

Tests (`GPU.SceneRenderer.Camera`):

| Case | Checks | Fails without |
| --- | --- | --- |
| "Bloom covers the same share of the image at 1080 and 2160 rows" | A small emissive square (1% of the height) on black, the same camera at 1920x1080 and 3840x2160: the radius where the added glow falls to 1% of its value at 2% of the height from the source, as a fraction of the width, equal within 10%; the glow's total energy (relative to the source's) equal within 10% | 4K's halo is half |
| "Bloom radius scales the halo" | 0.03 and 0.12 reach about half and twice the default's radius (within 25%) | |
| "At 1080 rows the default bloom is 0.6.0's" | 1920x1080, `BloomRadius` default: 6 levels used (stats), `BloomScale` 1/6, and the glow equals a renderer with `BloomRadius` 0.0592593 + 1e-5 (snapped) bit for bit | without the snap, 7 levels |
| "The vignette leaves the center, is symmetric and falls like cos^4 of the field angle" | `Tests::ExactSpecification()`, Vignette 0 against 0.12 on a flat emissive wall (Tonemapper None): the center pixel bit-identical; mirrored pixels within one code; the corner and the side edge's center match the CPU's `1 - 0.12 (1 - cos^4)` within one code | ring model |
| "Orthographic views vignette as a 60 degree view" | An orthographic camera (`OrthographicLookAt`, `SceneRendererTestUtils.h:114-122`) in a 16:9 viewport (the tangents use the viewport's aspect): its corner and side-edge centers match a 60-degree perspective view's CPU values within one code | no orthographic branch (tangents 0: no vignette) |

`ThumbnailCache::c_Version` + 1 ("the lens vignette and bloom by screen fraction"). Acceptance: equal halo share within
10%; vignette center exact; cost within `Post4K` (one more bloom level at 4K, about 0.01 ms, estimated; none at 1080).

### 5.6 Step 6: motion blur

#### 5.6.1 When it runs, and the motion vectors' request rules

- `motionBlurWanted = options.MotionBlur && !options.HDROutput && postProcess.MotionBlur && postProcess.
  MotionBlurShutterAngle > 0 && quality.MotionBlurQuality != Off && quality.MotionBlur != Off` (after the quality
  overrides of 5.6.6).
- `m_MotionFrame = options.MotionVectors || options.ShowMotionVectors || temporalWanted || motionBlurWanted`
  (`SceneRenderer.cpp:2744`): motion vectors are requested on every frame motion blur is wanted, so the motion history
  continues.
- The passes run when `motionBlurWanted && m_Stats.MotionHistory && !cameraCut` (2758, 2764): a frame without a history
  has no motion, and a cut's motion spans the cut. Such frames, and frames where nothing in view moves faster than the
  threshold, show the unblurred image.
- **Editor views never blur**: `ViewportRenderer::Render` (after 148) sets `options.MotionBlur = !m_SingleImages &&
  view.FromScene && context.IsPlaying()`, with `m_SingleImages = !fades` set by the constructor (`ViewportRenderer.cpp:77-89`:
  the capture renderer is the one made without fades). The Game view and the Scene view piloting the game camera blur
  while the game plays (`IsPlaying` is Play or Simulate, `EditorContext.h:325`); the editor camera never blurs (in edit
  mode or while playing), and nothing blurs in edit mode (a still scene; camera moves made by editing are not the
  game's). Still captures never blur (review: they do have a history, since a capture renders its view again for
  `c_TemporalSettleFrames` frames, `ViewportRenderer.h:67-70`, and a running game moves between them, so the capture
  renderer turns the blur off instead of relying on "no history"); `viewport.capture {live}` returns what the view
  presented, blur included. `GameRenderer` keeps the default (true). `ThumbnailCache` (`ThumbnailCache.cpp:873`) and
  `ProbeBake` set `options.MotionBlur = false`, so a model or scene with motion blur on allocates no motion resources
  for a single image.

#### 5.6.2 Passes (`GPU.MotionBlur`, between `GPU.TAA` and `GPU.Post`)

Where it is recorded (review): step 0 keeps `gpuTimer.Next(gpuStages.Post)` in `RenderFrame` before `RenderPostHDR`
(`specs/step0.md` 6.1), so passes "at the top of `RenderPostHDR`" would be timed as `GPU.Post`. `RenderFrame` instead
calls `bool SceneRenderer::RenderMotionBlur(nvrhi::ICommandList*, const PostProcessComponent&, bool resolved)` (defined
in `PP`) after the HDR-output return (3182-3205) and before `gpuTimer.Next(gpuStages.Post)`, behind `if
(motionBlurFrame)` with its own `gpuTimer.Next(gpuStages.MotionBlur)`; it returns whether the post source is now the
motion blur texture, which `RenderPostHDR` and `RenderPostDisplay` take as a `PostSource` parameter.

The source is the post source (the resolved image on TAA frames, `m_HDRTexture` otherwise); the result,
`m_MotionBlurTexture` (RGBA16F, viewport-sized, UAV; made by the first frame that blurs, released with the render
targets: 66 MB at 3840x2160), becomes the post source of exposure, bloom and tone mapping (5.6.4). Its alpha is the
gather's mask: 1 where the pixel was blurred (`rN >= 0.5`), 0 where the center color was copied. Velocities are
processed identically in the three passes (a function in `SH/Include/MotionBlur.glsl`):

- `v_px = velocityUV * ViewportSize.xy * ShutterFraction` (`ShutterFraction` = angle / 360); where depth is 0 (the sky)
  the velocity is `GetCameraMotion(uv, 0)` (`Motion.glsl:8-15`);
- clamped to `MaxLength = MotionBlurMaxLength * height` pixels (direction kept);
- **the threshold**: `L = |v_px|`, `L' = L * smoothstep(MinLength, 2 * MinLength, L)` with `MinLength = height / 1080`
  px (1 px at 1080 rows, 2 px at 2160): blurs shorter than that are dropped, fading in up to twice it. Rationale: a
  sample-and-hold display already smears eye-tracked motion by its whole per-frame length; a blur of one or two pixels
  only softens texture during slow pans, which is what the owner rejects.

Passes (compute; one layout `m_MotionBlurLayout`: `VolatileConstantBuffer(0)` (frame constants, for `GetCameraMotion`
and depth), `Texture_SRV(0..3)` (0 velocity, 1 depth, 2 the tile or neighbor maxima, 3 color), `Texture_UAV(0)`,
`PushConstants(1, 32)`; register b0 is the frame constants, so the push constants take slot 1 as the AO pass does). No
sampler (review): every fetch is a point fetch, so the shaders use `texelFetch` (`GL_EXT_samplerless_texture_functions`)
as `Overlay/MotionView.frag` does, and declare nothing at `ST_SAMPLER(0)`, where `Include/Scene.glsl:292` (pulled in by
`Motion.glsl`) declares the material sampler array; SRV 0-3 alias the scene layout's unused declarations exactly as
`MotionView.frag`'s SRV 0-1 do. A set binds only what its pass reads (the tile pass leaves SRV 2-3 unbound; nothing
reads them).

```cpp
struct MotionBlurParameters // PostProcess/MotionBlur*.comp
{
	float ShutterFraction; // MotionBlurShutterAngle / 360
	float MaxLength;       // Pixels: MotionBlurMaxLength * viewport height
	float MinLength;       // Pixels: viewport height / 1080
	uint32_t TileSize;     // Pixels: max(8, ceil(MaxLength / 2))
	uint32_t SampleCount;  // 8, 12 or 16 (MotionBlurQuality)
	uint32_t Padding0, Padding1, Padding2;
};
```

1. `SH/PostProcess/MotionBlurTiles.comp`: one 8x8 group per tile of `TileSize` px (22 px at 2160 rows, 11 at 1080 with
   the defaults); each thread scans its share of the tile; the tile's processed velocity of largest length goes to the
   top-left `ceil(W / T) x ceil(H / T)` texels of the tile target (SRV 0 velocity, SRV 1 depth, UAV 0). Tile targets
   (review) are **RGBA16F** (xy used): rgba16f is a storage format every Vulkan device supports, while rg16f storage
   needs the optional `shaderStorageImageExtendedFormats` (the reason the AO target has a fallback,
   `SceneRenderer.cpp:48-51`); they are sized for the smallest tile, `ceil(W / 8) x ceil(H / 8)` (480x270 at 4K, 1 MB
   each), so a change of `MotionBlurMaxLength` never reallocates.
2. `SH/PostProcess/MotionBlurNeighbors.comp`: per tile, the largest of its 3x3 neighborhood's tile maxima; a diagonal
   neighbor counts only when its velocity points toward the center tile (it can blur into it). The second tile target,
   same size.
3. `SH/PostProcess/MotionBlur.comp` (8x8 groups, full resolution): the gather, a reconstruction filter on the principle
   of McGuire et al.'s tile-max/neighbor-max filter (2012), designed here:
   - `rN` = half the neighbor maximum's length; if `rN < 0.5` px the output is the center color, unchanged (a still or
     slowly moving neighborhood is bit-identical to no blur; this is also the slow pan's path).
   - The center: color `C`, view distance `zC` (from depth through `InverseProjection`; the sky infinitely far), radius
     `rC` (half its own processed length, at least 0.5).
   - `N` = `SampleCount` samples at `s_i = -1 + (2i + 1 + (j - 0.5)) / N` along the direction (stratified over the whole
     span, `j` in [0, 1) a static per-pixel value from `PixelRandom(pixel, 6)`: noise only where things move, no ghost
     copies, nothing temporal); even samples follow the neighbor maximum's direction, odd ones the pixel's own when it
     moves at least 1.5 px (else the neighbor maximum's). Offset `s_i * rN`, distance `d = |s_i| * rN`; point fetches.
   - Weights, with `InFront = clamp(1 - (zS - zC) / (0.05 zC), 0, 1)`, `Behind = clamp(1 - (zC - zS) / (0.05 zC), 0, 1)`,
     `Cover(d, r) = clamp(1 - d / r, 0, 1)` and `Span(d, r) = 1 - smoothstep(0.95 r, 1.05 r, d)`: `w = InFront *
     Cover(d, rS) + Behind * Cover(d, rC) + 2 * Span(d, rS) * Span(d, rC)`: a nearer sample whose blur reaches the
     center spreads over it (a moving foreground over the background), a farther sample counts when the center's own
     blur reaches it, and parts of one surface blur together. The center counts as a sample at distance 0. The output
     is the weighted mean. A still foreground (the player, who moves with a third-person camera) over a moving
     background takes no background samples (`Cover(d, rC)` is 0 at `rC` 0.5), so it stays sharp.
   - NaN cannot enter (the source is sanitized HDR); the output is clamped to `c_MaxHDRValue`.
   - Noise (review): with one static `j` per pixel, the stratified estimate of an edge crossing the span is quantized to
     multiples of 1/N, so neighboring rows differ by up to 1/N of the edge's contrast (about +-10 codes on a black-white
     edge at 12 samples, sigma about 6): visible as fine streak noise without TAA, nothing that averages away. It is
     bounded by a test (5.6.7), judged on the sheet with FXAA and no AA (5.6.5), and not amplified: on frames that blur,
     `TonemapConstants::ColorFlags` bit 1 says the source's alpha is the gather's mask, and the tone mapping's TAA
     sharpening (`Tonemap.frag:163-164`) is multiplied by `1 - mask`, so it never sharpens a blur (sharpening there would
     raise the streak noise by up to half again and cut the streaks back into steps).
- Pre-exposure: the blur is linear, so it works on pre-exposed values unchanged.
- Limits, documented: blended transparent surfaces write no velocity (glass does) and blur with what lies behind them;
  overlays, text and the game UI are drawn after the post and never blur.

#### 5.6.3 Properties, quality and stats

- `MotionBlur` (off on the component, so stored scenes do not change), `MotionBlurShutterAngle` ("How long the shutter
  stays open, in degrees of the frame: 180 blurs over half a frame's motion, as film cameras do; 0 none"),
  `MotionBlurMaxLength` ("The longest blur, as a fraction of the image height: 0.02 is 43 pixels at 2160 rows").
- The row `MotionBlurQuality` sets `SampleCount` (`Off` disables the pass); the players' row `MotionBlur` (`Scene`,
  `Off`) can only turn it off.
- `SceneRendererStats::MotionBlurred` (the passes ran), `MotionBlurSamples`, `MotionBlurTileSize`; `renderStats`
  reports `motionBlur: { samples, tileSize }` or null.
- GPU stage `GPU.MotionBlur` (`RendererGPUStages::MotionBlur`), registered right after `GPU.TAA` (`GPUProfiler.cpp:59`),
  as step 0's S0.5 table assigns (`specs/step0.md` 8: each stream registers at its own position, so the lines never meet;
  `GPU.DepthOfField`, below the cut, right after `GPU.MotionBlur`), and timed between `GPU.TAA` and `GPU.Post`. The five
  hard-coded lists of 2.2 gain it at the same position (`EditorProfilerTests.cpp:91-92` and `FramePerfLogTests.cpp:284`
  check the registration order). (Review: "registered last, after `GPU.Overlays`" contradicted that table.)

#### 5.6.4 Post sources

Exposure, bloom's prefilter and tone mapping read one post source; their sets exist per source: `PostSourceSets {
nvrhi::BindingSetHandle Exposure, BloomPrefilter, Tonemap; }` for `PostSource::HDR` (today's `m_ExposureBindingSet`,
`m_BloomDownsampleSets[0]`, `m_TonemapBindingSet`), `History0`/`History1` (today's `m_Temporal*Sets`) and `MotionBlur`
(new, made with `m_MotionBlurTexture`); the gather has a set per input (HDR, History0, History1; the history ones made by
`EnsureMotionBlurResources` when the TAA's history textures exist, and dropped with them). Below the cut, depth of field
adds `DepthOfField` to both lists. All are released with the render targets (`ReleaseRenderTargets` ~1950-2034; with
S0.3, `ReleasePostTargets`).

#### 5.6.5 How "restrained" is judged, and the fallback

The owner rejects blur; motion blur here must read as a camera's, never as smear. It is judged on live captures at
3840x2160 (the S0.6 harness; ThirdPerson's course, its fence and its robot, playing paused and stepped with
`input.sequence` so each frame is one fixed step of 1/60 s, as `clarity/tools/livepan.sh` does), at three yaw rates of
the third-person camera (the shot sets the `Gameplay/Look` value that gives them, measured once from the camera entity's
rotation between two steps): a **slow pan** 6 degrees per second (about 3.3 px per frame at the image center), a
**gameplay turn** 30 degrees per second (about 16 px), a **fast turn** 120 degrees per second (about 65 px); each with
TAA Sharp, with FXAA and without AA, motion blur on (180 degrees) and off. The shots go into `Tools/LookDev/Shots.json`
as live shots beside step 0's `tp.live.pan`, each twice, with `Setup` commands (`component.set` on the Course's Post
Process `4BC76BF041EAD086`: `MotionBlur` true or false, `MotionBlurShutterAngle` 180 or 90), so the harness takes them
without new options; the blur-off variant captures at least two consecutive frames (`Frames` 4, as `tp.live.pan`), so
the motion of each measured edge is its displacement between consecutive blur-off frames (stepping is deterministic, so
the blur-on run shows the same motion at the same steps). The motion debug view is no measure (it saturates at 16 px a
frame, `MotionView.frag:17`, and live captures take no render parameters).

Why the criteria use the measured motion (review): the camera orbits the player (`Samples/ThirdPerson/Scripts/
FollowCamera.cpp`, 60 degrees vertical), so image motion is not one number: a rotation moves pixels by `f dtheta (1 +
(x / f)^2)` (twice the center's at a 16:9 view's side edges), the orbit's translation scales it by `1 - D / z` (the player
at the pivot does not move, near posts less than the far hills), and one rendered frame per fixed step is assumed, not
known. So each blur is judged against the CPU model of the gather (`PostProcessMath::MotionBlurProfile(lengthPx,
sampleCount)`: the row-averaged profile the 5.6.2 weights give an ideal edge on one surface, whose 10-90% width is about
0.735 of the blur length, not the length itself) fed the motion measured at that edge times the shutter fraction.

1. Slow pan: pixels whose motion times the shutter is below `MinLength` (2 px at 2160 rows) equal the motion-blur-off
   frame bit for bit. At 6 degrees per second that is a central band about 46% of the width wide for the far background
   (less than 4 px of motion per frame) and all of the robot, so a crop `[0.35, 0.2, 0.65, 0.8]` and the robot's crop are
   compared bit for bit (the neighbor maximum reaches about one tile, 22 px, beyond the band: the crop stays inside); the
   rest of the frame is blurred by less than 2 `MinLength`. (Review: "every frame bit for bit" cannot hold:
   the side edges move about 6.7 px per frame, 3.35 px at 180 degrees, above the threshold.)
2. Gameplay turn: on the fence posts, the row-averaged 10-90% edge width within 15% of the model's for the motion
   measured at the post; the robot's crop keeps at least 0.95 of the blur-off frame's texture detail (clarity's `tools/detail.py`);
   no doubled edges (each post's row-averaged profile is monotonic); the streak noise in the post crops (the RMS of each
   row's profile about the rows' mean) within 1.5 times the model's (`MotionBlurProfile` gives its spread too).
3. Fast turn: the same against the model (about 32 px of blur at the center); where the shutter's length exceeds
   `MaxLength` (43 px at 2160 rows), the 0-100% width is at most `MaxLength + 1`.
4. The before/after sheet (S0.6, `plan.md` 7.3) shows still, slow, gameplay and fast frames with blur off, 180 and 90
   degrees, with TAA, FXAA and no AA, for the owner.

**Fallback**: if a criterion fails after the stream's fixes, or the integration review of the sheet finds the gameplay
turn smeared, the template and the samples ship `MotionBlur` off (their command scripts expect false) and the release
notes say motion blur is available and off by default; the feature, its rows and its tests stay. A shutter of 90 degrees
in the template is the integration's alternative only if the sheet shows 90 holding and 180 not.

#### 5.6.6 The players' anti-aliasing row, and where overrides apply

`SceneRenderer::GetPostProcessSettings` (`SceneRenderer.cpp:1320`) is static (`SceneRenderer.h:1191`) and cannot see the
renderer's quality (review). A new `static void SceneRenderer::ApplyQualityOverrides(PostProcessComponent& settings,
const RendererQualitySettings& quality)` in `PP` applies the overrides to the copy `RenderFrame` takes at 2735, once,
right after it and before `ChoosePreExposure` (2736) or anything else reads it: `AntiAliasing` `Off` sets
`AntiAliasing` false, `FXAA` sets it true with `FXAA`, `TAA` true with `TAA` (still subject to
`SceneRendererSpecification::TemporalAntiAliasing`); `Scene` leaves the component's. `MotionBlur` `Off` sets
`MotionBlur` false. Both paths read the same copy. Screen-space reflections follow the effective method (they need TAA,
as today). Callers outside the renderer that report a scene's post settings (none read the anti-aliasing method today)
keep the component's values.

#### 5.6.7 Tests, docs, acceptance

Tests (new suite `GPU.SceneRenderer.MotionBlur`, `GPUSceneMotionBlurTests.cpp`; `AdaptationSeconds` set, manual
exposure, `Tests::ExactSpecification()`):

| Case | Checks | Fails without |
| --- | --- | --- |
| "A still camera over a still scene renders bit-identically with motion blur on and off" | Frames 2-5 | |
| "A shutter of 0 renders bit-identically to motion blur off" | A moving camera | |
| "Motion below the threshold leaves the image unchanged" | The camera translating parallel to a flat wall (uniform image motion) by `MinLength` px per frame | |
| "A moving camera blurs an edge over the shutter's share of its motion" | A 1024x576 viewport, `MotionBlurMaxLength` 0.05 (28.8 px): the camera translating parallel to a black and white wall by 24 px per frame at 180 degrees gives the row-averaged edge a 0-100% width of 12 px within 1 px and the profile of `PostProcessMath::MotionBlurProfile(12, N)` within 2 codes; 90 degrees, 6 px | no blur |
| "The gather's noise stays within the stratified estimate's" | The same edge at 8, 12 and 16 samples, no AA: each row's profile differs from the row average by at most `255 / N + 2` codes and the rows' RMS spread is within 1.5 times `MotionBlurProfile`'s | samples clumped or one `j` for the whole image (bands) |
| "Blurred pixels are not sharpened" | TAA Sharp (`Sharpening` 0.5), a moving camera: the tone-mapped output where the mask is 1 equals that of the same frame with `TAASharpening` 0 under Custom, bit for bit; still pixels keep their sharpening | sharpening of the blur |
| "The blur is clamped" | The default 0.02 (11.5 px at 576 rows), 400 px per frame: width at most `MaxLength + 1` | |
| "A foreground moving with the camera stays sharp over a blurred background" | A box parented to the camera: its edges within 1 px; the background blurred | |
| "Camera cuts and frames without history do not blur" | A 20 m jump; the first frame | |
| "Both draw paths blur equally" | CPU and GPU-driven renderers of a moving camera and a moving entity, within 1/255 | |
| "Off costs nothing" | Component off, row Off, player Off, `options.MotionBlur` false: no motion blur texture, no `GPU.MotionBlur` time, and no velocity target when nothing else asks | |
| "The quality row sets the samples" | `SampleCount` 8/12/16 in stats | |
| `GPU.SceneRenderer.TAA` "The players' anti-aliasing row overrides the scene's method" | Scene, Off, FXAA, TAA | |
| `GPU.Editor.Viewport` "Editor views never blur; the Game view blurs while playing; captures never blur" | `MotionBlurred` per view, and false for `viewport.capture {camera: "scene"}` of a playing game with a moving camera | the capture renderer blurring (it has a history over its settle frames) |
| `Editor.Perf` (`EditorProfilerTests.cpp:91-92`), `Runtime.FramePerfLog` (`FramePerfLogTests.cpp:284`): the stage lists | `GPU.MotionBlur` right after `GPU.TAA` | |

Feature test (review: values named): the active "Environment" Post Process gets `MotionBlur` true,
`MotionBlurShutterAngle` 150, `MotionBlurMaxLength` 0.03 (the feature runs exercise the passes whenever the camera
moves), `BloomRadius` 0.08, and step 3's grading values; the inactive "Alternate Grading" (automatic exposure on) gets
step 4's: `AutoExposureMetering` Average, `AutoExposureLowPercentile` 0.05, `AutoExposureHighPercentile` 0.95,
`AutoExposureLightSpeed` 4, `AutoExposureKey` 0.2. `FeatureTest.stproj`'s `Renderer.Quality` (step 0's Custom value)
gains `"MotionBlurQuality": "Samples16"` and `"AntiAliasing": "TAA"` (the scene's own method, so nothing renders
differently) and keeps `"MotionBlur": "Scene"` (Off would keep the passes out of every feature run); the section's value
differs from its default as a whole, which is what the feature test checks (DG "Feature test", 623-626).

Docs: DG "Post-processing" "Motion blur" (rules of 5.6.1, the passes, the threshold and the clamp, the limits); DG
"Motion vectors and TAA" names motion blur among the requesters; `Camera.md`; the editor-automation skill (the stage,
`renderStats.motionBlur`, that editor views never blur).

Acceptance: 5.6.5's criteria; `PerfGPU.Renderer.MotionBlur4K` at most 0.30 ms at High (12 samples) for a full-screen
gameplay turn, `MotionBlur4KStill` at most 0.12 ms (section 8; targets, provisional; review: a still frame still reads
velocity and depth in the tile pass and copies the full-resolution RGBA16F image through the gather, 66 MB each way at
4K, about 0.1 ms on this GPU's bandwidth, so the earlier 0.03 could not be met).

### 5.7 Step 7: adoption

Through editor commands on each sample opened in place (`StrataEditor --project Samples/<Game>` headless, StrataCLI),
never by editing scene files; each change one `component.set` step, then `scene.save`:

| Scene (Post Process entity) | Changes |
| --- | --- |
| ThirdPerson Course (`4BC76BF041EAD086`) | `Tonemapper` Filmic, `Exposure` 0, `Vignette` 0.12, `MotionBlur` true (180 degrees) |
| ThirdPerson MainMenu (`13AB59872DE91DE7`) | Filmic, `Exposure` 0, `Vignette` 0.12 (a menu backdrop: no blur) |
| OpenWorld Island (`F7486ABFA648EBD7`) | Filmic, `Exposure` 0, `Vignette` 0.12, `MotionBlur` true |
| PhysicsPlayground Main (`8B22F284D166ECC6`) | Filmic, `Exposure` 0, `Vignette` 0.12, `MotionBlur` true |
| Tetris | nothing: it changes only through the defaults (dither; its stored 0.2 vignette under the new model); its picture is retaken if that shows |

- `Exposure` stays 0 unless a sample's scene camera image (S0.6's 1920x1080 shot) has its median display luminance
  outside [0.15, 0.22]; then a compensation of at most 0.5 EV in 0.25 steps, stated with the medians in the commit.
- The template already took Filmic and Exposure 0 in step 2; here it gains `MotionBlur` true (the new vignette and
  center-weighted metering are its defaults). Its description says "Filmic tone mapping, automatic exposure, motion
  blur".
- The Third-Person Adventure's settings menu gains "Motion Blur: On / Off" (`Graphics::Set("MotionBlur", "Scene" or
  "Off")`, saved with `Graphics::Save`), made through editor commands on its UI (graphics-options lands first and owns
  the menu; this is one more row in its form).
- The command scripts (`StrataTests/Editor/<Sample>Commands.json.in`) expect the new values (`/values/Tonemapper`
  "Filmic", `/values/Exposure` 0, `/values/MotionBlur`, `/values/Vignette`) and the menu row.
- Pictures: after merging main, `Picture.png` and `Screenshots/` are retaken (binary conflicts take main's and retake);
  light-and-materials, landing after, retakes them again under its sky changes.
- Docs: `Docs/Manual/Camera.md` (new; listed in `Docs/README.md`'s Manual list): exposure (auto and manual, metering,
  adaptation, the key, a table of what each scene type meters), tone (the curves, Toe, Shoulder, Contrast, Saturation),
  color (white balance, slope, offset, power), bloom, vignette, motion blur (what it is, when it shows, shutter angle,
  max length, the players' option), what each costs at 3840x2160 on the reference machine, and three recipes (a sunny
  day, an interior, a night). `Lighting.md`'s "Exposure" section (52-64) shortens to the units and links `Camera.md`.
  `AntiAliasing.md`: which method suits which look (TAA Sharp the 3D default, FXAA's lower subpixel blend, no AA with the
  dither), the players' row. DG "Post-processing" complete (section 10). The AGENTS.md bullet. Release-note lines
  (section 10).

Acceptance: a metered 0.18 under the template's Post Process displays at sRGB 112-124 (`plan.md` 1.1's row; 0.6.0: 91 at
Exposure 0), checked by `Editor.Templates` reading the template's component and step 2's GPU test run with those values
(117-119). (Review: "a gray card in sun" is not what the plan measures: a sunlit card shows above middle gray whenever the
metered average includes shade, and the look-dev scene has no card.) The samples' package tests pass; the before/after
sheet.

### 5.8 Depth of field (below the cut)

- **Lens**: `CameraComponent` (`Scene/Components.h:130-147`) gains `DepthOfField` (false), `FocusDistance` (meters,
  10), `FocusEntity` (entity reference; when set and present, the distance along the camera's forward to its world
  position, at least the near plane), `Aperture` (f-number, 1-32, 2.8), `SensorHeight` (mm, 4-100, 24: full frame); the
  focal length follows from the field of view, `f = 0.5 * SensorHeight / tan(fov / 2)`. Inactive reasons: the lens
  fields without `DepthOfField`, `FocusDistance` while a focus entity applies, all of them on orthographic cameras.
- **Out of the shadow cache's key**: the lens does not enter `SceneCamera` (whose whole value is the cascades' cache key,
  `SceneRenderer.h:1926-1936`); programs pass `SceneRenderOptions::Lens` (`struct CameraLens { float FocusDistance,
  FocalLength, FNumber, SensorHeight; }`) built by `CameraLens::FromEntity(scene, entity)` when they view through a scene
  camera with depth of field (`GameRenderer`; `ViewportRenderer` for `view.FromScene`).
- **Circle of confusion**: `c = (f^2 / (N (zf - f))) * |z - zf| / z` on the sensor, signed (negative in front of the focus),
  in pixels `c_px = c / SensorHeight * height`, clamped to 2% of the height.
- **Passes** (`GPU.DepthOfField`, between `GPU.TAA` and `GPU.MotionBlur`; half resolution, RGBA16F: about 50 MB at 4K
  when on): (1) CoC and downsample (color weighted by CoC, the signed CoC in alpha; near field taking the nearest, far
  field the farthest of each 2x2); (2) near-field tile maximum (16 px tiles) dilated over 3x3 tiles, so a blurred
  foreground spreads over sharp background; (3) gather, scatter-as-gather: 48 samples on a golden-angle disk rotated by a
  static per-pixel angle (the dither hash), radius the pixel's CoC (far) or the tile's dilated CoC (near); a sample
  counts where its own CoC covers the distance to the pixel, far samples never over nearer in-focus pixels; the near
  field's coverage becomes its alpha; (4) a 3x3 median of both fields; (5) composite at full resolution: pixels with
  `|c_px| < 1` keep the sharp image exactly, the far field fades in over 1-2 px of CoC, the near field over everything.
- The players' row `DepthOfField` (`Scene`, `Off`); off costs nothing. Feature test: the camera's lens fields non-default
  with `DepthOfField` on.
- Tests (`GPU.SceneRenderer.DepthOfField`): the focus plane bit-identical to off; a point at known distance blurs to the
  CoC's diameter within 15%; a near object's blur spreads over the in-focus background; a focus entity follows; off costs
  nothing; both paths equal. Perf (provisional): `PerfGPU.Renderer.DepthOfField4K`, target 0.8 ms.

### 5.9 Film grain and chromatic aberration (below the cut)

- `FilmGrain` (0-1, default 0): in `Tonemap.frag` after the curve and saturation, in the encoded domain, before the
  dither: `encoded += FilmGrain * 0.04 * sqrt(4 Y (1 - Y)) * g`, `Y` the encoded luminance (most visible in the
  mid-tones, as film's), `g` a near-Gaussian value (the sum of four `PixelRandom` streams minus 2, scaled to unit
  variance) salted with the frame's index (grain changes every frame, as film's; it averages nothing over time).
- `ChromaticAberration` (0-1, default 0): lateral, radial: the red channel is fetched at `0.5 + (uv - 0.5) * (1 + k)`,
  blue at `(1 - k)`, green at the center, bilinear (sampler 1), `k = ChromaticAberration * 0.5 / (0.5 * 3840)` so 1 shifts
  a 3840x2160 corner by half a pixel (a fixed fraction of the image at every resolution).
- `TonemapConstants` appends `{ float FilmGrain, ChromaticAberration; uint32_t GrainFrame; float Padding; }` at 144.
- The players' rows `FilmGrain` and `ChromaticAberration` (`Scene`, `Off`). Tests: off bit-identical; grain keeps the
  mean within 0.1 code and differs between frames; aberration 0 at the center and at most 0.5 px at a 4K corner.

### 5.10 Morphological anti-aliasing (below the cut)

`AntiAliasingMode::Morphological` (appended, 2; file and Inspector name "Morphological", tooltip "Morphological:
non-temporal, finds edges and blends along them"; the players' row gains `Morphological`; review: the plan's shorthand
was `SMAA`, but enum names are file and script API forever, and this design is not the published SMAA, whose diagonal
patterns, corner handling and search tables it does not reproduce, so the name would promise its behavior): the
non-temporal method, designed here on the morphological principle (edge detection, pattern-aware blend weights from an
area table, neighborhood blending), reading the tonemap's 10-bit intermediate (step 1):

1. Edges (fragment pass, RG8): left and top edges where the luma difference of the display-encoded image exceeds 0.05,
   kept only if at least half the largest difference among the neighboring edges (local contrast adaptation), or-ed
   with depth edges (view distance differing by more than 5%).
2. Blend weights (fragment pass, RGBA8): for each edgel, search along the edge for its ends (up to 32 px each way), read
   the crossing edges at both ends (none, up, down, both: 4 patterns each, 16 combinations), and look up the area the
   reconstructed line covers on each side of the pixel boundary from the area table at the two distances.
3. Blending (fragment pass, into the output, with the dither): each pixel mixes with its neighbors by those weights.

The area table: `Tools/GenerateAreaTable.py` computes, for each of the 16 crossing combinations and distances 0-32 to
each end, the coverage of the pixel by the straight line the pattern implies (L, Z and U shapes), analytically, as an
RG8 table of 16 x 33 x 33 entries, written to `R/MorphologicalAreaTable.inl` (committed) and uploaded once per renderer
as a texture. Targets: RG8 edges and RGBA8 weights (about 50 MB at 4K, only with Morphological). Tests: an edge tilted at 1:7 gets
coverage-like values (mean error against the analytic coverage below 0.1); flat areas bit-identical; texture detail at
least 0.95 of no AA on the tiled floor (FXAA 0.81 in 0.6.0); both paths equal. Perf (provisional):
`PerfGPU.Renderer.Morphological4K`, target 0.35 ms. Timed in `GPU.Post`.

### 5.11 A 3D LUT asset (below the cut)

- `AssetType::ColorLookup` (appended after `World`, value 13; append only). Imported from `.cube` files by
  `ColorLookupImporter` (registered in `RegisterAssetPipeline`; `GetVersion()` 1; reads its source through
  `ReadImportSource`; checks `ImportContext::IsCancelled` per slice; messages quoting the file through `MakeValidUTF8`):
  `TITLE`, `LUT_3D_SIZE N` (2-65), `DOMAIN_MIN`/`DOMAIN_MAX`, `#` comments, then `N^3` lines of three floats, red fastest;
  `LUT_1D_SIZE` is refused with the reason. Setting `Size` (17, 33, 65; default 33, described in
  `GetSettingsProperties`): the importer resamples to it trilinearly.
- Cooked form: magic `STLU`, version 1, size, domain (2 x vec3), `size^3` RGBA16F texels; the loader rejects truncated or
  garbage bytes (round-trip and corruption tests). The `ColorLookup` asset (Renderer layer) holds an nvrhi 3D texture,
  uploaded in one step (33^3 x 8 bytes = 287 KB, under `c_AssetUploadStepBytes`), and reports its memory.
- `PostProcessComponent::ColorLookup` and `ColorLookupIntensity`: after the saturation, in the encoded domain, `encoded =
  mix(encoded, texture(lut, encoded * (N - 1) / N + 0.5 / N).rgb, intensity)`; resolved through the frame's
  `AssetResolveCache` and requested each frame; while it loads, nothing is applied (never blocks). The tonemap layout
  gains `Texture_SRV(3)` (a 1x1x1 dummy when absent). The component's asset reference is recorded like every asset
  property (pruned exports, delete checks).
- Feature project: `Assets/Lighting/Feature/Warm.cube` (17^3, written by `StrataTests/FeatureTest/Tools/GenerateAssets.py`)
  on the active Post Process at intensity 0.5. Tests: identity LUT bit-identical to none (within the 16-bit texels'
  rounding: one code); a known LUT maps colors as the CPU's trilinear lookup within one code; import of malformed files
  fails with the line.

### 5.12 Lens flares (below the cut)

`LensFlare` (0-1, default 0): ghosts and a halo from the bloom chain, and a sun starburst.

- Ghosts: a pass over bloom level 1 (quarter resolution) with a high soft threshold (exposed luminance above 4) writes
  four ghosts at `uv' = 0.5 + (0.5 - uv) * k`, `k` in {-1.6, -0.7, 0.45, 1.2}, each tinted (a fixed spectral tint per
  ghost) and faded radially, plus a halo (a ring at 0.45 of the height toward the center); the result is blurred by one
  tent upsample and added in the tonemap at `LensFlare * 0.02` (restraint: a sun in view leaves faint ghosts).
- Starburst: when the directional light's disk projects inside the view and 16 depth taps around it see the sky, an
  additive pattern of six rays (`pow(abs(cos(3 phi)), 40)` falling with distance) centered on it, scaled by the
  visible fraction, in the tonemap.
- Timed in `GPU.Post`; off costs nothing (no pass, no target). Tests: off bit-identical; ghosts mirror a bright source
  through the center.

## 6. The post chain after the wave

| Order | Pass | Stage | Reads | Writes | When |
| --- | --- | --- | --- | --- | --- |
| 1 | TAA resolve | `GPU.TAA` | HDR, depth, velocity, history | history | TAA |
| 2 | Depth of field (5.8) | `GPU.DepthOfField` | post source, depth | DoF texture | lens and `DepthOfField` |
| 3 | Motion blur tiles, neighbors, gather | `GPU.MotionBlur` | post source, depth, velocity | motion blur texture | 5.6.1 |
| 4 | Histogram, exposure | `GPU.Post` | post source | exposure buffer | auto exposure |
| 5 | Bloom (and flares) | `GPU.Post` | post source | bloom chain | bloom |
| 6 | Tonemap: CA, expose, sharpen (not where the motion blur mask is 1), bloom, vignette, grade, contrast, curve, saturation, LUT, grain, dither | `GPU.Post` | post source, bloom, exposure, CB | output or 10-bit intermediate | always |
| 7 | FXAA or Morphological (with the dither) | `GPU.Post` | intermediate | output | FXAA, Morphological |
| 8 | Overlays, copy | `GPU.Overlays` | | output, target | |

## 7. Shared files and merges

`camera-and-tone` lands third (`plan.md` 4), after graphics-options and sun-shadows, before light-and-materials; it
merges main before landing. It owns `SH/PostProcess/*`, `SH/Include/Dither.glsl`, `Filmic.glsl`, `FilmicConstants.glsl`,
`MotionBlur.glsl`, `Exposure.glsl`, `PP`, `R/PostProcessData.h`, `R/PostProcessMath.*`, `R/FilmicConstants.h`,
`R/WhiteBalanceLocus.inl`, `Tools/FitFilmic.py`, `Tools/GenerateWhiteBalance.py`, `Docs/Manual/Camera.md`,
`AntiAliasing.md`, and Post Process's exposure, tone, color, bloom, vignette, motion and lens fields.

| Shared file | This stream's edit | Resolution |
| --- | --- | --- |
| `R/SceneRenderer.h/.cpp` | Spec flag, options, stats, the motion decision (2744), the `ApplyQualityOverrides` call after 2735, the `RenderMotionBlur` call before `gpuTimer.Next(gpuStages.Post)`, target release lines | Additive lines; sun-shadows and light-and-materials touch other regions |
| `SceneRenderer.cpp`'s `ChoosePreExposure` (1479-1537) and the exposure readback (519, 1399) | The readback grows to 3 floats (5.4) | light-and-materials adds a `bool hold` parameter and its early return (its spec, "History and exposure"): different lines; keep both |
| `R/SceneRenderData.h` | Deletes the post structs (moved; 379-414, 460-472) | Others append at the end; `ScreenReflectionParameters` (370-377), which light-and-materials may extend below its cut, ends one blank line before the deletion: merge by hand if git calls them adjacent |
| `R/RendererComponents.h/.cpp` | Post Process fields and registrations | graphics-options lands first with sections and descriptions (it rewrites the Tonemapper and Vignette descriptions this stream rewrites again: take this stream's text, which names Filmic and the lens model); this stream appends its lines after `.Section` calls; light-and-materials changes only the AO lines (`AmbientOcclusionRadius`) |
| `R/RendererQuality.h/.cpp`, `Renderer.Quality` | Rows of 4.2, one line each, with their `c_RowTraits` lines (4.2) and `FromSectionValue` lines; version +1 | Append; the integrator renumbers versions |
| `R/GPUProfiler.h/.cpp` and the five stage lists | `GPU.MotionBlur` right after `GPU.TAA` (and `GPU.DepthOfField` after it), step 0's S0.5 positions | Line-level: each stream's stage sits at its own position (`GPU.Aerial` after Setup, `GPU.ScreenShadows` after Prepass) |
| `StrataEditor/src/Editor/ThumbnailCache.h` (`c_Version`, 104) | +1 per image-changing step (1b, 2, 5) | sun-shadows and light-and-materials bump it too: the integrator takes the highest number plus one and joins the comments |
| `StrataTests/src/Renderer/SceneRendererTestUtils.h` | `Tests::ExactSpecification()`; `AddNeutralPostProcess`'s metering lines (5.4) | light-and-materials adds `AmbientOcclusionMultiBounce = false` to the same helper: separate lines |
| `StrataEditor/src/Editor/ThumbnailCache.cpp`, `ProbeBake.cpp` | `options.MotionBlur = false` | step 0 changed their specifications (`ForThumbnails`, `ForBake`); different lines |
| `Strata/CMakeLists.txt` (shader list 108-115) | `MotionBlurTiles.comp`, `MotionBlurNeighbors.comp`, `MotionBlur.comp` (and the cut-line passes) | Append |
| `StrataEditor/src/Editor/ProjectTemplates.cpp` | Post Process lines (69-76), description | light-and-materials owns the sky and sun lines; it may set nothing on the Post Process |
| `StrataEditor/src/Editor/ViewportRenderer.cpp`, `EditorStateCommands.cpp` | `options.MotionBlur`; `renderStats.exposure`, `.motionBlur`; the stage list | Additive |
| `Feature.stscene`, `FeatureTest.stproj` | Non-default values of new properties and rows | Additive (text merge of JSON values) |
| `Docs/Manual/Lighting.md` | The "Exposure" section shortened to a link | light-and-materials owns the page (but Shadows): it keeps this edit when merging |
| `Tools/LookDev/Shots.json` (step 0) | The pan shots of 5.6.5 (three rates, blur on and off through `Setup`) | Append (step 0's 9.8: new shots appended, existing ones unchanged) |
| DG, AGENTS.md, `Budgets.json`, sample command scripts, sample pictures | Sections, one bullet, provisional entries, expectations; pictures retaken after merging main | As `plan.md` 4.1 |

## 8. Perf

Provisional entries in `StrataTests/Perf/Budgets.json` (`{ "Unit": "ms", "Provisional": true, "Target": ... }`), measured
by the integration campaign, never by this stream:

| Metric | What | Target |
| --- | --- | --- |
| `PerfGPU.Renderer.Post4K` | Median of `GPU.Post` at 3840x2160: 100k-cube grid, TAA Sharp, Filmic, auto exposure (center-weighted, key), bloom (7 levels at the default radius), vignette, grading on, dither | 0.25 (0.6.0: about 0.20) |
| `PerfGPU.Renderer.MotionBlur4K` | Median of `GPU.MotionBlur`, the camera turning at 16 px per frame, High (12 samples) | 0.30 |
| `PerfGPU.Renderer.MotionBlur4KStill` | The same, still camera (the tile passes and the gather's copy of every pixel) | 0.12 (review: a full-resolution RGBA16F read and write is about 0.1 ms here; 0.03 was below that floor) |
| below the cut: `DepthOfField4K`, `Morphological4K` | | 0.8, 0.35 |

The tests go in `RendererPerfTests.cpp` (suite `PerfGPU.Renderer`, label `perf`, Release and Dist), modeled on
`MeasureTemporalCubes` (98-133) with a 3840x2160 viewport and `MedianGPUStageMs`.

A/Bs of the stream's own stages, as information in commit messages: Release builds of the branch and of step 0's tip
(built once in `build/baseline-step0` of the worktree), interleaved, at least 3 pairs, each run as `python
Tools/PerfLock.py --label camera-and-tone --limit 60 -- python -I Tools/LookDev.py capture --build <bin dir> --label <a
or b> --shots <glob, e.g. "tp.*"> --sizes 3840x2160 --aa taa,fxaa --repeat 5 [--live]` (step 0's command line,
`specs/step0.md` 9.2; PerfLock ends a command after 25 minutes by default, `Tools/PerfLock.py:16`), then `LookDev.py
compare`, comparing the medians of `GPU.Post` and `GPU.MotionBlur` in `stages.csv` on the samples' scene cameras and the
pan shots, never while another build or campaign runs. The Open World tour (`PerfGPU.Samples.OpenWorldTour.*`,
provisional at bc6ee123 unless step 0's campaign gave it a budget) pays motion blur once the sample turns it on; the A/B
of `ow.live.tour` states that cost, and if step 0 gave the tour a budget the stream checks the A/B against it before
step 7 lands (a budget is never raised: the sample would ship with motion blur off instead).

## 9. Acceptance, summarized

| What | 0.6.0 | Target | Step |
| --- | --- | --- | --- |
| Banding in PhysicsPlayground's 4K sky | widest run of one RGB value 123 px (`bands.py`) | under 16 px (the plan's; about 7 expected) | 1 |
| FXAA texture detail (tiled floor, 4K) | 0.81 | at least 0.88 (else subpixel 0.25, measured) | 1 |
| Metered middle gray on the display, Exposure 0 | sRGB 91 (ACES) | 117-119 (GPU test); the template's Post Process 112-124 | 2, 7 |
| Hue drift of saturated ramps (ICtCp) | ACES 26 degrees red, 26.6 blue | at most 10 (skin, foliage, sky 6) | 2 |
| White at +6 stops (Shoulder 0.5) | | at least code 250 | 2 |
| White balance | none | a light the setting names shows r = g = b within 1e-3; 6500 K, tint 0 exact identity | 3 |
| Exposure key | none (interior 0.121, exterior 0.112) | keyStops = 0.12 x the metered EV below the sun's card, within 0.05, on the look-dev room and overview; the full "interiors darker" look waits for light-and-materials' sky occlusion | 4 |
| Bloom halo share, 1080 against 2160 rows | 2:1 | within 10%; 1080 rows bit-identical to 0.6.0 | 5 |
| Vignette | ring after the curve | center exact, cos^4, corners 9.9% at 60 degrees | 5 |
| Slow pan at 4K with motion blur (6 degrees per second) | | the central band and the robot bit-identical to off; elsewhere under 4 px of blur | 6 |
| Gameplay and fast turns | | blur widths within 15% of the gather's model for the measured motion; streak noise within 1.5 times the model's | 6 |
| `Post4K`, `MotionBlur4K`, `MotionBlur4KStill` | 0.20 | 0.25, 0.30, 0.12 | 1-6 |

## 10. Docs and release notes

- **DG "Post-processing"** (new, under "Rendering" after "Light units and exposure"): the chain and its stages (section
  6), post sources, the constant buffer and push-constant rules, output and dither (`DitherOutput`, levels per target,
  the 10-bit intermediate), the curves (Filmic's form, constants and fit; the other curves unchanged), color, exposure
  (speeds, metering, window, key, `AdaptationSeconds`), bloom's levels, the vignette, motion blur (5.6), the quality and
  players' rows, the tests' helpers (`Tests::ExactSpecification`, `AddNeutralPostProcess`). "Light units and exposure"
  gains the curves' middle grays; "Motion vectors and TAA" names motion blur; "GPU timing" the new stages; "Readback,
  the game renderer and tests" the helper.
- **AGENTS.md** (one bullet under "Rendering", about 380 characters): "Post effects run in `R/SceneRendererPost.cpp`
  between the TAA resolve and the tone mapping, take parameters by push constants or the post's constant buffer (never
  new `FrameConstants` fields; reading them is fine), read the frame's post source, are off in `AddNeutralPostProcess`,
  and exact-pixel tests also turn
  `DitherOutput` off; Filmic's and white balance's constants come only from their `Tools/` generators
  ([Post-processing](Docs/DeveloperGuide.md#post-processing))." The rule is stated in full in the guide.
- **Manual**: `Camera.md` (new), `AntiAliasing.md`, `Lighting.md`'s link (5.7); `Settings.md` lists the new rows of
  Graphics Quality.
- **Skills**: editor-automation (the stage, `renderStats.exposure` and `.motionBlur`, editor views never blur);
  make-a-game (Filmic at Exposure 0 for new scenes, when to turn motion blur on).
- **Release notes** (`Docs/Releases/0.7.0.md`, "Upgrading from 0.6.0", lines this stream adds):
  - "The output is dithered: gradients no longer band. Tools that compare exact pixels turn
    `SceneRendererSpecification::DitherOutput` off."
  - "FXAA's subpixel smoothing is halved (0.75 to 0.375): it keeps more texture detail."
  - "New Post Process components, scenes without one and the Basic 3D template use the new Filmic curve at Exposure 0;
    scenes that stored a curve keep it. Filmic shows middle gray as middle gray: ACES needs about +0.8 EV for the same
    brightness."
  - "The vignette is a lens's fall-off applied before the tone curve (default 0.12); stored values keep their number,
    now as a share of a lens's full fall-off (0.2 darkens a 60 degree view's corners by 16.5% instead of 20%)."
  - "Bloom reaches the same share of the image at every resolution (it was half as wide at 3840x2160)."
  - "Automatic exposure adapts to brighter scenes twice as fast as to darker ones, meters the image's center more than
    its edges, and darkens scenes that meter below a gray card in the sun (up to 2 stops) and brightens those above it
    (up to 1 stop; `AutoExposureKey`), so interiors read darker than sunlight as far as their light is darker. Set
    `AutoExposureMetering` to Average, `AutoExposureKey` to 0 and `AutoExposureLightSpeed` to `AutoExposureSpeed` for
    0.6.0's exposure."
  - "White balance (temperature and tint), slope, offset and power on Post Process; 6500 K and tint 0 change nothing."
  - "Motion blur (off on the component; on in the template and the 3D samples), never in editor views; players can turn
    it off (Graphics Quality's Motion Blur row)." (Or, after the fallback: "available, off by default".)

## 11. Risks

| Risk | Mitigation |
| --- | --- |
| The owner finds motion blur smeary | Threshold (slow pans exact in the image's center, under 4 px of blur at its edges), 2% clamp, editor views never blur, the players' Off row, 5.6.5's criteria and sheet, the fallback (ship it off) |
| Filmic is a taste risk (too bright, too saturated, not "MGS V") | Stated targets between ACES and AgX, the sheet with ACES +1 EV beside it, `Toe`/`Shoulder`/`Contrast`/`Saturation` per scene, one property switches back; ACES unchanged |
| The hue targets cannot all be met by 12 matrix parameters | The planning prototype reached 11.6 degrees with a crude search; if the fit misses by a little, the script reports it and the spec's targets are relaxed in the commit only with the measured values and ACES's beside them (never silently) |
| The dither breaks exact-pixel tests | One helper, the sweep in 1b, and the "DitherOutput false quantizes without noise" test |
| Exposure changes in stored scenes (speeds, center weighting, key) | Bounded (key in [-2, +1] EV, needs a sun), scene-relative, named in the release notes with how to get 0.6.0's back; the existing exposure tests keep passing |
| More constant data than push constants allow | The tonemap's constant buffer (1a) |
| The FXAA intermediate's format change on some driver | `R10G10B10A2_UNORM` is a mandatory color-attachment format; the Mac gate (`plan.md` 7.4) runs the GPU suites |
| Motion blur artifacts at silhouettes of moving objects over moving backgrounds, thin objects, transparent surfaces | Depth-classified weights, neighbor max with direction-aware diagonals, documented limits (blended surfaces take the velocity behind them) |
| Memory: the motion blur target (66 MB at 4K per renderer that blurs) | Made only when blurring, released with the targets; editor views and captures never blur (so the Scene view and the capture renderer never allocate it) |
| Merge conflicts in the Post Process registration (graphics-options' sections, light-and-materials' AO lines) | Land order; re-base onto sections; AO lines untouched |
| Motion blur's streak noise without TAA (static per-pixel sample offsets quantize an edge's blur to 1/N steps) | It exists only where things move (still pixels copy their color); bounded by the noise test (5.6.7) and the sheet's FXAA and no-AA frames; never sharpened (the mask); if the sheet shows it at Medium's 8 samples, Medium becomes 12 (the row's line), stated with the A/B's cost |
| The exposure key does little in this wave's scenes (sky light is never occluded, so rooms meter near the sun's card) | Acceptance measures the mechanism (5.4); the look follows when light-and-materials' sky occlusion lands; the key's strength is not raised to compensate (it would darken real interiors too much once they are dark) |
| The white locus blends two curves between 4000 and 5000 K | Continuous by construction (the no-jump test); the 6500 K entry is D65 exactly, so the default stays the identity |
| A thumbnail, sample or picture that changed through a default is missed | `ThumbnailCache::c_Version` bumped per image-changing step; the samples' pictures retaken at the end (5.7); Tetris checked on the sheet |

## 12. Open questions and the decisions recommended

1. **The banding acceptance.** Answered in review: keep the plan's "widest run under 16 px". It is measured as the
   clarity audit measured 0.6.0's 123 px, on RGB triples (`clarity/tools/bands.py`); with independent noise per channel a
   pixel repeats its neighbor's triple with probability at most 0.42, so the expected widest run in 500 px is about 7.
   The "three pixels in four keep the center code" argument holds per channel only (a single channel's widest run would
   be about 20 px, reported as information).
2. **Defaults of the new exposure properties for stored scenes.** Recommended: center weighting, the key (0.12) and the
   faster light adaptation apply to every scene (named in the release notes), since "exposure like an eye" is the goal
   and the key is scene-relative and bounded. Alternative: Average and key 0 by default with the template and samples
   opting in. Review: agreed, with `AddNeutralPostProcess` keeping 0.6.0's metering for the existing tests (5.4).
3. **Is `Off` a choice of the players' anti-aliasing row?** The plan lists Scene, FXAA, TAA. Recommended: add `Off` (a
   player's preference; nothing needs AA to look right, and the dither keeps gradients clean).
4. **The name `SMAA`** for a morphological AA designed here. Review: `AntiAliasingMode::Morphological` (5.10), a
   deviation from the plan's shorthand for the integrator to record: enum names are file and script API forever, and a
   design that is not the published SMAA should not carry its name.
5. **Motion blur's camera rotation.** A real camera blurs pans fully; some players dislike it. Recommended: as specified
   (physical within the threshold and clamp); the 90-degree variant on the sheet lets the owner choose a softer default.
6. **`BloomRadius` default 0.06 against an exact 64/1080.** Review: 64/1080 with `s` snapped to integers (5.5), so
   1080p keeps 0.6.0's six levels and bloom bit for bit (Medium's usual resolution, its tests and A/Bs) instead of adding a
   seventh level of weight 0.02.
7. **Where the lens lives** (below the cut). Recommended: fields on `CameraComponent` (the plan's) and the renderer's
   copy in `SceneRenderOptions::Lens`, so the cascades' cache key never sees it.
8. **Hue metric.** Recommended: ICtCp (BT.2100) over CIELAB, whose blue bends toward purple and would steer the fit wrong.
9. **The white locus** (review). Planckian throughout makes 6500 K land 0.003 below D65, leaving every named light
   magenta; daylight throughout is undefined below 4000 K. Recommended: daylight from 5000 K, Planckian to 4000 K, blended
   in mireds between (5.3).

## 13. First hour

1. Read `plan.md` 4.4 and this spec; merge step 0's tip; check 2.4's assumptions against step 0's code
   (`RenderPostHDR`/`RenderPostDisplay`, `RendererQualitySettings`'s table form, `Renderer.Quality`'s version, the
   GPU stages' positions of S0.5) and, if graphics-options has landed, its `RowTraits` and `.Section` API (4.1, 4.2);
   note any name differences in the first commit message.
2. Configure and build `build/agent-camera-and-tone` (Debug and Release); run `ctest --preset windows-debug -R GPU`
   once to have the baseline of the GPU suites on this tree.
3. Build step 0's tip in `build/baseline-step0` (Release) for the A/Bs.
4. Start commit 1a: `R/PostProcessData.h`, the tonemap's constant buffer, FXAA's and the copy's layouts; prove no pixel
   change with the GPU suites.
5. Write `Tests::ExactSpecification()` and list (from a first run with 1b's default on) the tests the sweep converts.
6. Draft `Tools/FitFilmic.py`'s target checks (T1-T8 with ACES's and AgX's transcriptions) before its optimizer, so the
   baseline numbers print on day one.

## 14. Review corrections

An independent review (2026-10-27) checked this spec against bc6ee123 (and the five commits on `agent/step0`, none of
which touches the post chain), `plan.md`, the audits and the specs beside it (`step0.md`, `graphics-options.md`,
`sun-shadows.md`, `light-and-materials.md`), and corrected it in place. Each change, with why:

1. **Dither formula (5.1, major).** The narrowed amplitude `a = clamp(min(v, levels - v), 0, 1)` does not keep the mean
   near the ends: `v` = 0.3 averaged 0.056 and every `v` below 0.25 became black, so the darkest gradients stayed
   banded and the spec's own "mean within 0.01" test failed at k = 0. Replaced by a triangular dither where it cannot
   clip and a rectangular one within half a code of black or white: exact mean everywhere, black and white exact. The
   claim that shader quantization makes the result independent of float-to-UNORM rounding was narrowed to what holds.
2. **Banding acceptance (5.1, 9, Q1, major).** The author's argument against the plan's "under 16 px" is per channel, but
   the metric (`clarity/tools/bands.py`, which measured 0.6.0's 123 px) compares RGB triples, which per-channel noise
   breaks up far faster (expected widest run about 7 px). The plan's number is restored; the cubic-fit criterion, which a
   sky column would fail for reasons other than banding, is gone.
3. **White balance reference (5.3, Q9, major).** Relative to the Planckian locus at 6500 K, which lies about 0.003 in
   (u, v) below sRGB's D65 white, a light the setting names would show as about (1.04, 0.98, 1.03): magenta, and the test
   "it neutralizes the light it names" could not pass. The reference is now D65 and the white locus is daylight from
   5000 K (6500 K nominal is D65 exactly), Planckian to 4000 K, blended between; tests added for the split and continuity.
4. **GPU stage position (5.6.3, 7, major).** "Registered last, after `GPU.Overlays`" contradicted step 0's S0.5 table
   (`GPU.MotionBlur` after `GPU.TAA`, `GPU.DepthOfField` after it), which the stage-list tests check.
5. **Where motion blur is recorded (5.6.2, major).** Step 0 calls `gpuTimer.Next(gpuStages.Post)` before `RenderPostHDR`,
   so passes "at its top" would be timed as `GPU.Post`; `RenderMotionBlur` is now called from `RenderFrame` before it.
   Step 0's actual names (`RenderPostHDR`/`RenderPostDisplay`) replace "RenderPost" (2.4).
6. **Restraint criteria made true and measurable (5.6.5, 9, major).** "Every slow-pan frame bit for bit" is false: a
   rotation moves a 16:9 view's side edges about twice as fast as its center (6.7 px a frame at 6 degrees per second,
   3.35 px of blur, above the 2 px threshold). The 10-90% width of the gather's kernel is about 0.735 of the blur length,
   not the length, and the orbit camera's image motion depends on depth. The criteria now compare a central band bit for
   bit and judge widths and noise against a CPU model of the gather fed the motion measured between consecutive
   blur-off frames (the motion debug view saturates at 16 px a frame, and live captures take no render parameters).
7. **Noise without TAA (5.6.2, 5.6.7, 6, 11, major).** The gather's static per-pixel offsets make streak noise of up to
   1/N of an edge's contrast, which TAA Sharp's sharpening, applied after the blur, would amplify. Sharpening is now
   masked where the gather blurred (alpha mask, `ColorFlags` bit 1), and two tests bound the noise and the mask.
8. **Exposure key acceptance and tests (5.4, 1, 9, major).** "Interior at least 0.4 EV darker" cannot be met in this
   stream: 0.6.0's sky light reaches into closed rooms (lighting L1: walls at about 23% of the sun's irradiance), so a
   room meters near the sun's card; occlusion is light-and-materials' irradiance volume, below its cut. Acceptance now
   measures the key's mechanism. The test's "closed box lit through an opening" would meter its sun patch (black pixels
   are left out and there is no bounce light), above the card; replaced by a shaded floor with a known ambient. The
   center-weighted test's "at least one stop" was not guaranteed by its stimulus; it now compares with the CPU's
   weighted mean.
9. **Quality overrides (5.6.6, major).** `GetPostProcessSettings` is static (`SceneRenderer.h:1191`) and cannot see the
   renderer's quality; a static `ApplyQualityOverrides` is applied to the copy taken at 2735.
10. **graphics-options' row contract (4.2, 7, major).** The `RowTraits` lines (`FollowsPreset` false for the comfort
    rows, `GPUStage`, `MemoryBytes`) and `FromSectionValue`'s reading of comfort rows under every preset were missing,
    and the upgrade sentence contradicted step 0 (missing fields take their defaults, High's values).
11. **Thumbnails (5.1, 5.2, 5.5, 7, major).** AGENTS.md requires bumping `ThumbnailCache::c_Version` when thumbnails would
    look different; the dither, Filmic for scenes without a Post Process and the lens vignette all change them.
12. **Captures and single images (5.6.1, 4.3, major).** Still captures do have a history (a capture renders its view for
    `c_TemporalSettleFrames` frames) and a running game moves between them; the capture renderer, thumbnails and the
    probe bake now turn motion blur off explicitly.
13. **Bloom at small sizes and at 1080 (5.5, 4.1, Q6, major).** At the tests' 64 px the default radius uses 2 levels
    where 0.6.0 used 4, so "Bloom spreads bright light around its source" (its glow checked 6 px out) would likely fail,
    and the range (at most 0.15) could not restore 0.6.0's chain. The range reaches 0.25 and those tests set it. The
    default became 64/1080 with snapping, so 1080p bloom stays bit-identical (no seventh level of weight 0.02).
14. **Middle-gray test (5.2, 5.7, major).** Metering returns histogram bin centers (0.11 stops wide); half a bin times
    Filmic's slope moves the display by up to 2.7 codes, beyond 117-119. The wall's luminance is now a bin center, with
    the dither off. The template's acceptance measures a metered 0.18 under its Post Process, as the plan's row does,
    not "a gray card in sun".
15. **Component obligations (4.1, 5.2, 5.6.7, major).** Descriptions for the properties without one (graphics-options'
    description test), registration at the end of PostProcess's list after `.Section` calls, the add-component tests
    (`Scene.Serialization`, `Editor.Commands`, the component schema snapshot) and the feature test's values for steps 2
    to 6 (with `FeatureTest.stproj`'s rows), now named entity by entity.
16. **The existing tests' meaning (5.4).** `AddNeutralPostProcess` keeps 0.6.0's metering (Average, key 0, one speed),
    so the existing exposure tests that turn automatic exposure on keep their meaning.
17. **Minor fixes.** Tile targets RGBA16F (rg16f storage is an optional Vulkan feature) sized for 8 px tiles; no sampler
    in the motion blur layout (point fetches by `texelFetch`, as `MotionView.frag`, avoiding `Scene.glsl`'s sampler
    array); `GetDitherLevels` covers the packed UNORM formats `ValidateTarget` accepts; the tone-mapping constant buffer's
    versions are bounded by `ReserveFrameConstantVersions`; the readback's three sites named (519, 1399, 1519); the
    `ChoosePreExposure` merge with light-and-materials' `hold`; `renderStats.exposure` null with manual exposure; T3's
    code 250 is 0.955, not 0.97; `q >= 1` needs `C0 >= 1.315`; the DG cite is 3423-3433; `OrthographicLookAt`'s name and
    checks for the orthographic vignette test; the 10-bit and twin dither tests made able to fail; "DitherOutput false
    keeps 0.6.0's output" restated (FXAA frames differ whatever the flag); the noise histogram's tolerance made absolute;
    the FXAA detail target given a fallback (0.25); `MotionBlur4KStill`'s target raised to 0.12 ms (a full-resolution
    RGBA16F copy is about 0.1 ms here); the A/B command line made step 0's, and PerfLock's 25-minute limit lifted; the
    AGENTS.md bullet says "never new `FrameConstants` fields" (motion blur reads them); the release notes name the key's
    brightening and the grading tools.
18. **Below the cut.** `AntiAliasingMode::SMAA` renamed `Morphological` (Q4): an enum name is permanent API and this
    design is not the published SMAA.

Checked and kept: the Filmic curve's form and numbers (middle gray 117.6, the toe at code 2.7, +6 stops at code 252,
recomputed), the vignette's numbers (9.9% and 16.5% at 60 degrees, recomputed), the bloom level arithmetic, the exposure
parameter layout (64 bytes) and the tone-mapping buffer's std140 offsets, the file:line references of 2.1-2.3
(spot-checked against bc6ee123), the ABI (no host function), both draw paths (post-processing reads what both write),
the shadow caches (the lens stays out of their key), the cut line, and open questions 2, 3, 5, 7 and 8.

## 15. As built

What the branch built differently from the sections above, the names it found or chose, and what the integration adds.
Each item is also in the commit that made it.

### 15.1 What the integrator adds after graphics-options

graphics-options had not landed on this branch's base (cc9524a1): there is no `PropertyBuilderBase::Section`, no
`RendererQuality::RowTraits` and no description test, so this stream added none of them. At the merge:

- `RowTraits` lines for the rows this stream added to `RendererQualitySettings`: `MotionBlurQuality` (FollowsPreset
  true, GPU stage `GPU.MotionBlur`, memory `W * H * 12` bytes plus the tile targets: the RGBA16F output and the R32_UINT
  pixel target), and the players' rows `AntiAliasing`, `MotionBlur`, `DepthOfField` (GPU stage `GPU.DepthOfField`),
  `FilmGrain` and `ChromaticAberration` (FollowsPreset false: comfort, not cost).
- The `.Section` calls of the Post Process and Camera properties this stream appended, in the sections of table 4.1.
- The Third-Person settings menu's "Motion Blur: On / Off" row (4.2), absent here.
- `Renderer.Quality`'s `c_SectionVersion` is 2 here: renumber it if another stream took 2.
- `ThumbnailCache::c_Version` went to 5, 6, 7 (dither, Filmic for scenes without a Post Process, the lens vignette) and
  8 (Filmic's shadows, 15.3): renumber if another stream bumped it.

### 15.2 Names

- `RendererQualitySettings::MotionBlurQuality`'s enum is `MotionBlurSamples` (`Off`, `Samples8`, `Samples12`,
  `Samples16`): a member named like its type trips GCC's `-Wchanges-meaning`. `AntiAliasingChoice` is `{Scene, Off,
  FXAA, TAA, Morphological}`, `SceneEffectChoice` `{Scene, Off}`.
- Post sources are `SceneRenderer::PostSource` `{HDR, History, DepthOfField, MotionBlur}` with `SelectPostSet`;
  `RenderPostHDR` and `RenderPostDisplay` take one instead of the spec's flags, and the frame's `degraded`.
- The tone mapping's blur mask is `RenderData::c_TonemapBlurMask` (`ST_BLUR_MASK`; the spec's
  `c_TonemapMotionBlurMask`), set for the depth of field's and the motion blur's outputs;
  `MotionBlurParameters::Padding0` became `SourceMask`. `TonemapConstants` is 176 bytes.
- `Renderer/FilmicConstants.h` names its constants `c_Inset`, `c_Outset`, `c_Contrast`, `c_ToeWidth`, `c_MiddleGray`
  (AGENTS.md's naming table; the spec's 5.2 named them without the prefix).
- New public statics: `SceneRenderer::GetBloomSpan`, `GetVignetteTangents`, `ApplyQualityOverrides`;
  `PostProcessMath::GetMotionBlurProfile` (the motion blur's CPU model); `Tests::ExactSpecification()`.
- `GPU.DepthOfField` is registered after `GPU.MotionBlur` (step 0's table) and timed before it.

### 15.3 Deviations and relaxed targets

- **FXAA's subpixel blend is 0.25** (5.1's fallback): 0.375 kept 0.85 of the texture detail, under the 0.88 target; 0.25
  keeps 0.87.
- **The dither costs 0.021 ms at 3840x2160** (5.1's target: below 0.01): `GPU.Post` of `Post4K`'s TAA frame, 0.2446 ms
  with it and 0.2235 ms without (three interleaved runs each). Six `PixelRandom` values a pixel; a cheaper hash of the
  same statistics would have to replace its CPU twin and `Renderer.PostProcessMath`'s statistics with it.
- **Filmic's toe works on the color as a whole** (5.2 specified the curve per channel everywhere). Per channel, the
  toe's log-log slope (about 1.5) raised the channels' ratios to its power, and the outset's negative lobes pushed the
  weak channel to 0: the sky test color showed 1.71 to 1.88 times its chroma from -1 to -4 stops (ACES at the samples'
  old +1 EV: 1.25 to 1.50), a dark sky (0.02, 0.06, 0.15) at sRGB (0, 39, 95), and the look-dev skies and blue flowers
  turned ultramarine. T7 checked saturation only at 0 stops, and no refit of the per-channel form brought the sky under
  1.35 below middle gray (1.53 at best, with T5 failing). Now the operator scales the inset color by the curve of its
  largest channel over that channel below middle gray and blends to the per-channel curve over 0 to 4 stops of that
  channel (`c_FilmicChannelStart`, `c_FilmicChannelEnd`, the form's constants, written by the generator); T9 (new)
  bounds every test color's saturation ratio below middle gray at 1.30. Refitted (C0 1.398, tau 3): sky 1.16, skin 1.05,
  foliage 0.98, blue 1.29 below middle gray; the dark sky at (11, 60, 99). The hue limits of T5 went to 12.5 and 7.5
  degrees for the per-channel form (it reached 12.28 on blue and 7.13 on sky); this form meets 5.2's 10 and 6 (7.95 and
  4.77), so they are back.
- **The exposure key never brightens** (5.4 gave it [-2, +1] and "a snow field above the card brightens slightly"): a
  scene above the directional light's card is in practice lit by more than the sun (an overcast sky, dusk, lamps), so
  the positive side brightened overcast days and sunsets (up to +1 stop) and jumped at 0 lux; the range is [-2, 0].
- Test tolerances: the 10-bit dither test allows 1.5 codes with 8-pixel bands; the bloom snap test checks 0.0592593
  within 1e-6; the key tests use a dark SkyLight (ambient 0) and a 16,384 lux sun (bins line up).
- **Motion blur ships off** in the template and the samples (the decisions' "when in doubt, off"): the release note
  takes the fallback wording. Its colors-only uniform path is new (a neighborhood that moves as one surface reads only
  colors, with the depth-aware path's one-surface weights); occluding edges over slower backgrounds blur at 0.56 of the
  one-surface model (the spec's depth weights, the restrained side).
- OpenWorld's Exposure is +0.25 EV by 5.7's median rule (0.147 at 0, 0.188 after). The Basic 3D template's start scene
  follows the same rule (5.2 set it at Exposure 0): its bright sky over the dark stone ground meters the ground below
  middle gray, a median of 0.136 at 0, so it takes +0.25 (0.174 after). light-and-materials replaces its sky and
  sun; measure it again after that lands.
- **Depth of field (5.8):** no 3x3 median pass (the static 48-sample rotation and the bilinear upsampling leave no
  outliers); the near field uses an area-share weight clamped at 16. Under TAA the composite fades the blur in over two
  pixels from in-focus depth (the depth is jittered, the color is not: a hard decision flickered at in-focus
  silhouettes).
- **Grain (5.9)** is seeded for single images (captures, thumbnails) and added by the pass that writes the display image
  (FXAA or the morphological blend when one follows the tone mapping), not by the tone mapping before them.
- **Morphological anti-aliasing (5.10)** keeps 0.88 of the texture detail, short of 0.95: U-shaped runs blend nothing
  (with them 0.81), and depth edges need a jump above 5% and twice the neighboring steps.
- Post failures (the morphological targets, the lens flare, the tone mapping's second set) mark the frame degraded, and
  a tone mapping that cannot run fails the frame.

### 15.4 5.11 and 5.12

1. **The color lookup's cooked form holds no domain (5.11).** `ColorLookupImporter` folds `DOMAIN_MIN`/`DOMAIN_MAX` (and
   Resolve's `LUT_3D_INPUT_RANGE`) into its resampling, so every cooked lattice spans [0, 1] and the shader needs no
   domain constants; `STLU` version 1 is a 16-byte header (magic, version, size, zero) and the RGBA16F points. The `Size`
   setting is a choice of "17", "33" (default), "65" or "File" (the file's own size). The upload queue gained 3D regions
   (`GPUTextureRegion::Depth`). The tone mapping reads the lookup through a second binding set (set 1, slots t3 and s2:
   NVRHI wants a pipeline's layouts to use different slots), so the five tone-mapping sets of the post sources stay as
   they are.
2. **Lens flares read level 1 before the bloom's upsamples (5.12).** After the upsamples, the deeper levels' tent filters
   leave diamond-shaped contours that the flare's threshold cut out as diamond ghosts (look-dev `13-flare`); the flare
   runs between `RenderBloomDownsamples` and `RenderBloomUpsamples` and reads through a cubic B-spline. The bright part's
   excess over 4 is compressed toward 16 so the sun's ghosts stay faint. The starburst is drawn by the tone mapping, as
   specified, but its 16 depth taps run in the flare pass, which hands their share over in its target's alpha.
