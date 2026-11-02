# Workstream `radiance-cascades`: indirect light from the screen (wave 8)

The owner (2026-11-01, verbatim in `.worktrees/wave8-plan/owner_feedback.md`), item 5: "Implement radiance cascade (using path of
exile 2 technique)", and the same day: "Also radiance cascade, you will need to research as well." Item 2 is tied to it: "Add SH
irradiance probes. (have this if radiance cascade is preventatively hard to implement, or skip this)". The look stays wave 7's:
realistic and filmic, native resolution, the Fox Engine as the baseline, nothing that needs TAA to look right, neither blurry nor
clinical (decisions.md, "Standing rules").

This spec designs plan.md section 4.4 (the stream), decision D7 (no history unless every degrade form fails) and gate G1. Stages 0
and 1 run in tranche 1 and are specified to be built without re-deciding; stage 2 (the join), stage 3 (hardening) and the
irradiance volume below the cut are specified so that tranche 2 starts without re-deciding. Where this spec and `specs/decisions.md`
disagree, decisions.md wins; where it and plan.md disagree, this spec says so in section 12 with the reason.

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `E/` is `StrataEditor/src/Editor/`, `DG` is
`Docs/DeveloperGuide.md`, `RC/` is `.worktrees/wave8-plan/radiance-cascades/` (the audit: `audit.md`, the CPU model
`spike/rc_spike.cpp`, its sweeps `spike/sweep*.sh` and logs `spike/sweep*.log`, pictures `spike/out/`, baselines
`ld-samples-high-4k/`, `ld-room-indirect/`). Line numbers are of 76f28a21 (0.7.0). Step 0 moves code (S0.10 converts every shader
to Slang, S0.11 cuts `Forward.frag`'s indirect block into functions): the implementing agent finds the same code by its function
names on S0.12's tip, and step 0's names win where they differ from this spec's.

Rules for this stream (plan.md 4.1, decisions.md):

- The Windows reference machine (i7-13700K, RTX 5090, 3840x2160 at 150%) on its temporary memory. Worktree
  `.worktrees/radiance-cascades`, branch `agent/radiance-cascades` from S0.12's tip, build `build/agent-radiance-cascades`
  (`cmake --preset windows -B build/agent-radiance-cascades`). Never the owner's `build/windows`.
- Before each commit: Debug ctest with every `GPU*` suite under the validation layer, the feature test, `python Tools/CompatLint.py
  --changed-since <S0.12 tip>`. GPU suites use `Tests::GPUContext` and end with `CHECK(gpu.GetNewErrorCount() == 0)`.
- No perf campaign, no `PerfStability.py`, nothing measured against `Budgets.json`. The stream A/Bs its own stages, interleaved, under
  `Tools/PerfLock.py`, as information (7.1). Readings taken while other slots run are ratios to a stage of the same frames.
- Open source under the owner's D10 (decisions.md, which postdates this spec's brief): other folders on this computer are never
  read; published papers, talks and open-source projects are read as much as helps. Code is copied, ported or adapted only from
  permissive licenses (MIT, BSD, Apache-2.0, zlib, Boost, public domain, CC0), with the notice in `ThirdPartyNotices.md`'s section
  for derived code (S0.1 adds it) and the source named in the commit; copyleft code (GPL, LGPL, AGPL, MPL...) is read for ideas only.
  The radiance cascades paper (Sannikov), Osborne and Sannikov 2024, the Chalmers SPWI thesis and Therrien et al.'s visibility
  bitmask paper are the design's sources; every algorithm below is specified here, so nothing needs porting. The audit's CPU model
  (`RC/spike/rc_spike.cpp`) is this repository's evidence, read as the candidate's specification, not copied into engine code.
- Stages 0 and 1 do not land (plan.md 4, tranche 1 slot 2: "no landing"): the branch is kept, merges main at the start of tranche 2,
  and lands first at landing 2 after stage 3.

## 1. Goal

In priority order:

1. **An honest answer to "can Path of Exile 2's technique work for a free camera here", in numbers, early (G1, tranche 1).** The
   cascades run on the GPU behind two debug views, `gi` and `open`, on both draw paths, and three numbers decide the stream: the
   stage's cost as a ratio to `GPU.AO`, the indirect light's flicker on a fence strafe, and on grass cards. Nothing joins shading
   before the gate (stage 1).
2. **A measure of flicker that runs on 0.7.0 first (stage 0)**, so the gate compares against something real: the audit's
   second-difference metric in a GPU test, on a yard with a fence, a field of grass cards and the look-dev room.
3. **If G1 passes, bounce light that follows a moving sun within a few frames (stage 2, tranche 2):** diffuse indirect light becomes
   hit light plus the open share times today's light (probes or sky), with no history and no accumulation. Off costs nothing: a
   scene asks through `PostProcessComponent::GlobalIllumination`, the Graphics Quality row `GlobalIllumination` scales it (Off at Low
   and Medium, High at High and Ultra), captures and thumbnails wait for `GlobalIlluminationSettled`.
4. **Hardened on real scenes (stage 3)** and judged by the owner live, in motion, in the rebuilt editor with GI toggled, on the
   Third-Person Adventure and the showcase. No sample or template turns it on before that look (plan.md 7, step 7).
5. **The owner's item 2 kept honest:** the showcase's hut uses today's box probe (no code here). The SH irradiance volume is specified
   below the cut (D15.5) and moves above it if G1 fails, which is the owner's stated fallback.

Not the goal: world-space cascades or screen probes with world intervals (plan.md 9: voxelization or ray tracing), rough specular
from the cascades, lighting volumetrics from the cascades (the paper: cascades cannot light froxels), throwing sun off metal onto
neighbors beyond what the screen shows (plan.md 9), any temporal accumulation (D7 is the one guarded exception).

## 2. Where we start

### 2.1 Files

| File | Lines | Role | This stream |
| --- | --- | --- | --- |
| `R/SceneRenderer.cpp` | 4,125 | `RenderFrame` (2825-3608): motion (2913-2928), SSR's copy gating (2971-2991), frame constants (2996-3022, 3140-3142), rebind check (3199-3202), prepass (3298-3324), AO (3339-3345), Reflections (3346-3356), copy (3392-3405), transparent (3406-3408), lighting view overlay (3566-3573); scene layout (337-362), `CreateSceneBindingSets` (1006-1105), `ReleaseRenderTargets` (2180-2275), `ForThumbnails`/`ForBake` (200-246) | shared: gating, the GI stage, the copy flag, SRV 35, the view pass |
| `R/SceneRenderer.h` | 2,771 | `SceneRendererSpecification` (124-286), `SceneLightingView` (322-330), `LightingViewStats` (359-380), `SceneRenderOptions::LightingView` (483-491), stats (494-716, SSR's pending flag 643-652), SSR copy members (2676-2691) | shared: a settings struct, two views, stats, members |
| `R/SceneRendererRadianceCascades.cpp` | new | resources, the six passes, the view pass, layout tracking | owns |
| `R/RadianceCascadeLayout.h/.cpp` | new | the layout's math (CPU, unit-tested) | owns |
| `R/SceneRendererGlass.cpp` | 131 | `EnsureSceneColorResources` (39-105), `CopySceneColor` (107-129): the half-resolution copy with 6 mips | shared (the copy GI reads) |
| `R/SceneRendererScreenReflections.cpp` | 345 | the spatial trace reads the previous copy along velocity (157-321; history scale 304) | read as the model |
| `R/SceneRendererAmbientOcclusion.cpp` | 145 | GTAO + denoise in `GPU.AO`; the push-constant pattern (42-61, 116-143) | read as the model; below the cut: AO at 2x6 with GI |
| `R/SceneRendererLightingView.cpp` | 333 | `IsLightingValueView` (73-76), `BeginLightingViewFrame` (78-95), `RecordLightingView` (97-246), `ResolveLightingViewStats` (257-312) | shared: two views |
| `R/SceneRenderData.h` | 665 | `FrameConstants` (41-103, `static_assert` 103), `AOParameters` (393-405), lighting view data (620-661) | shared: push-constant structs, constants |
| `R/RendererQuality.h/.cpp` | 267 / 895 | rows (`.cpp` 33-47), traits (121-141), `GetPreset` (404-437), `Clamp` (439-477), section 4 (`.h` 168) | stage 2: the row |
| `R/RendererComponents.h/.cpp` | 388 / 707 | `PostProcessComponent` (257-352; AO fields 283-288), its properties (`.cpp` 429-589; AO 451-460, reflections 510-520), inactive reasons (589-610) | stage 2: three properties |
| `R/GPUProfiler.h/.cpp` | 204 / 393 | `RendererGPUStages` (`.h` 20-37), registration (`.cpp` 50-65) | `GPU.RadianceCascades` (S0.11 names it; this stream registers it: step0.md 8.6) |
| `SH/Include/Scene.glsl` | 396 | slot comment (7-12), lighting view constants (31-37), `AOParams` (80-81), `ReflectionParams` (107) | shared: SRV 35, views 6 and 7, claimed components |
| `SH/Scene/Forward.frag` | 452 | indirect block (210-300): views (210-212), occlusion and multi-bounce (214-226), probes (231-236), the three branches (247-300) | stage 2: the diffuse gather (S0.11's), the open share in specular occlusion |
| `SH/Include/IndirectLight.glsl` | S0.11 | `IndirectInputs`, `GatherIndirectDiffuse`, `GatherIndirectSpecular`, compose | stage 1 adds `GetDiffuseFallback`; stage 2 owns the diffuse gather |
| `SH/GI/*` | new | `RadianceCascadesPyramid.comp` (and its base variant), `RadianceCascadesProbes.comp`, `RadianceCascadesMarch.comp`, `RadianceCascadesGather.comp`, `RadianceCascadesFilter.comp`, `RadianceCascadesUpsample.comp`, `GlobalIlluminationView.frag`: eight entries; `SH/Include/RadianceCascades.glsl` | owns |
| `SH/Effects/LightingViewStats.comp`, `SH/Overlay/LightingView.frag`, `SH/Include/LightingView.glsl` | 94 / 51 / 60 | value views (stats 27-30, 46-64; overlay 26-48) | shared: two views |
| `Strata/CMakeLists.txt` | 195 | `strata_add_shaders` (74-), variants (156-180; AO's at 177 and 179) | eight entries |
| `StrataEditor/src/Editor/EditorStateCommands.cpp`, `StrataTests/src/Editor/EditorProfilerTests.cpp`, `StrataTests/src/Runtime/FramePerfLogTests.cpp`, `Docs/Architecture.md`, `.claude/skills/strata-editor-automation/SKILL.md` | | DG "GPU timing" (5195-5199): the five lists that name the GPU stages (`EditorStateCommands.cpp:924`, `EditorProfilerTests.cpp:91`, `FramePerfLogTests.cpp:284`, Architecture.md 921-923, the skill) | `GPU.RadianceCascades` after `GPU.AO` in each |
| `StrataCLI/src/CLI/McpToolsets.cpp`, `StrataTests/src/Editor/EditorMcpToolsetsTests.cpp` | | `viewport.capture` is a `core` command (`McpToolsets.cpp:29`); the core list stays within 30,720 bytes (`EditorMcpToolsetsTests.cpp:69`) | read: the two new debug view names and their description must fit |
| `E/StreamingView.h/.cpp` | 173 / 368 | `ViewportDebugView` (`.h` 27-42), names (`.cpp` 33-57), lighting mapping (`.cpp` 65-75) | `gi`, `open` (S0.11 may have added the names) |
| `E/EditorViewportCommands.cpp` | 1,333 | capture waits (55-72, 754-787), `DebugViewSchema` (144-148), `DescribeDebugView` (194-234), result (905-918) | shared: the GI wait, two views, their description |
| `E/ProjectThumbnail.cpp` | 227 | the picture's waits (162-183) | stage 2: the GI wait |
| `StrataEditor/src/Panels/ViewportPanel.cpp` | 1,117 | debug chip (639-666), legend (771-790) | two entries, the open legend |
| `E/EditorStateCommands.cpp` | 1,094 | `DescribeRenderStats` (207-), its stage list text (924) | GI stats, the stage name |
| `Tools/LookDev.py`, `Tools/LookDev/LookDevScene.json`, `Shots.json` | 1,013 / 90 / 70 | debug views (56), project build (440-470), captures (519-537), stages table (590-617), argparse (974-999); the look-dev scene (21-50: room, poles, fence, leaves); shots | the yard and grass scenes, shots, `ratio`, grass macro |
| `StrataTests/src/Renderer/SceneRendererTestUtils.h` | 536 | `ExactSpecification` (240-246), `AddNeutralPostProcess` (251-267) | read |
| `StrataTests/src/Perf/RendererPerfTests.cpp`, `StrataTests/Perf/Budgets.json` | 1,098 / | `MedianGPUStageMs` (64-83); AO and SSR entries (`Budgets.json` 176-182, 227-229) | `PerfGPU.RadianceCascades.*`, stage 2 `PerfGPU.Renderer.GI4K*` |
| `StrataTests/FeatureTest/FeatureTest.stproj`, `Assets/Scenes/Feature.stscene` | | `Renderer.Quality` (150-172), Post Process (269-) | stage 2 values |

### 2.2 What the frame does today, and what the cascades need from it

- **Depth and normals.** The prepass writes D32 reversed-Z depth and geometric view-space normals, octahedral RG16F
  (`SceneRenderer.cpp:41-42`, `DepthPrepass.frag:82-85`), on both paths: the GPU-driven path draws its first phase, the occlusion
  phase and the second phase before the prepass marker ends (3310-3324). Everything this stream reads is written by then.
- **No usable depth pyramid.** The GPU path's HZB is the farthest depth only, half resolution, R32F, made only with occlusion
  culling (`SceneRendererGPUDriven.cpp:792-817`, `HZBBuild.comp:7-10`), and the CPU path has none. The cascades build their own
  (D4), after the complete prepass, on both paths.
- **The previous frame's light.** `CopySceneColor` writes the HDR target after the opaque pass and the sky into a half-resolution
  copy with up to 6 mips, B10G11R11 where stored, else RGBA16F (`SceneRendererGlass.cpp:24-30, 39-105`), in `GPU.Transparent`, when
  glass draws or spatial reflections ask (`SceneRenderer.cpp:3393-3405`). Spatial SSR reads the previous frame's copy where the hit
  surface was, along the motion vector, scaled by `m_PreExposure / m_ReflectionColorPreExposure`, gated by `m_ReflectionColorRender
  == m_RenderCount`, no camera cut, no reset and the same render scene (2976-2991, `SceneRendererScreenReflections.cpp:294-321`,
  `ScreenSpaceReflectionsSpatial.comp:121-128`). The cascades copy that discipline exactly (D9).
- **Motion vectors** are written on motion frames only (2913-2914): the cascades make their frames motion frames.
- **Today's indirect light** (`Forward.frag:214-300`): material AO times screen AO, multi-bounce (`AOMultiBounce`), probes whose boxes
  hold the surface (`EvaluateReflectionProbes`, `ReflectionProbes.glsl:94-122`, irradiance from 9 coefficients), the sky's irradiance
  map times `EnvironmentParams.x` where probes leave room, or `AmbientColor` without an environment; three branches with different
  scales (the third multiplies the sum by `EnvironmentParams.x`, 293). S0.11 turns this into `IndirectInputs`,
  `GatherIndirectDiffuse`, `GatherIndirectSpecular` and a compose step, pixel-equal.
- **Lighting debug views** (`SceneLightingView`, `SceneRenderer.h:322-330`; DG "Lighting"): the forward pass shades with
  `MaterialParams.y`, value views (AO, Albedo) write unexposed values that `Overlay/LightingView.frag` shows, and
  `Effects/LightingViewStats.comp` histograms every frame of a view into readbacks. The `gi` and `open` views join this machinery
  without touching the forward pass (D10).
- **Captures** re-render while things settle, each wait bounded (`EditorViewportCommands.cpp:754-787`); SSR's pending flag keeps the
  history (770). `ProjectThumbnail` does the same (162-183). GI joins both.
- **Slots and constants.** S0.11 assigns SRV 35 to radiance cascades, SRV 30-31 to the irradiance volume, `ReflectionParams.y` and
  `.z` to GI on and its intensity (`.w` to `reflections`), stage name `GPU.RadianceCascades`, debug view names `gi` and `open`.
  `AOParams.z` and `.w` are written 0 and read by nobody (`SceneRenderer.cpp:3140`, `Scene.glsl:80-81`; no shader under `SH/` reads
  `AOParams.z/w` or `ReflectionParams.y/z/w`, checked by search): this stream claims `AOParams.z`.
- **Baseline cost** (`RC/ld-samples-high-4k/stages.csv`, 3840x2160 High TAA, medians of 5): `GPU.AO` 1.23 ms on `tp.scene`, 0.62 on
  `pp.scene`, 0.59 on `ow.scene`; GPU busy 3.02, 2.02, 1.95 ms. The look-dev room at 1920x1080: `GPU.AO` 0.34 ms
  (`RC/ld-room-indirect/stages.csv`), and its picture is lit like the street.

### 2.3 What the audit's CPU model established (RC/spike)

`rc_spike.cpp` is a CPU model written for the audit (not engine code; read it as the specification of the candidate, never copy it).
Its candidate (`sweep6.sh`: `probemode=3 fractional=1 nearshare=1 interval0=16 thick=0.15 blur=4`, at 720p; `interval0=24` at 1080p):

| Case (indirect light, flicker = second difference over three frames) | Per pixel mean / p99 | 32 px blocks mean / p99 | Log |
| --- | --- | --- | --- |
| Paper's form (nearest depth, no filter, 4 px first interval) | 6.86 / 46.6 | 3.45 / 21.7 | `sweep7.log`, court naive |
| Candidate, 1280x720, court strafe 2 cm/frame | 0.75 / 7.4 | 0.52 / 3.0 | `sweep7.log`, court r0 |
| Candidate, 1920x1080 | 0.61 / 6.1 | 0.45 / 3.1 | `sweep7.log`, court hd |
| Candidate, still camera with TAA's jitter | 0.69 / 6.6 | 0.51 / 3.2 | `sweep6.log`, jitter |
| Candidate, 500 grass blades (thin boxes) | 1.78 / 17.1 | 1.17 / 6.2 | `sweep8.log`, grass |
| Candidate, only two cascades (fence, older filter) | 0.69 / 8.6 | 0.35 / 3.1 | `sweep3.log`, c2 |
| Candidate + one-pole history 0.5 | 0.29 / 2.9 | 0.20 / 1.2 | `sweep9.log`, h50 |

Accuracy: in the room, cascades with a sky fallback read 3.7 times the reference; with one box probe within 13% of its mean
(`sweep7.log`, interior r0/r1). Counts at 1080p: 28.4 M march steps and 40.2 M merge fetches per frame. What the model lacks: GTAO,
normal maps, alpha cards, skinning, transparents, real materials; its grass is boxes. Nothing ran on a GPU.

## 3. Order, tranches and the cut line

Each stage is commits that build and pass everything; each stage's DG text is written with its code on the branch.

| Stage | Tranche | Size | What | Exit |
| --- | --- | --- | --- | --- |
| 0. Measure | 1 | M (1 day) | the flicker metric (test helpers), the yard, grass and room scenes in C++ and in the look-dev project, `GPU.SceneRenderer.IndirectFlicker` on 0.7.0's indirect view, look-dev shots, the ExileCon 2023 talk tried again | the metric runs on today's build; its numbers are in the commit |
| 1. Cascades behind views | 1 | L (3 days) | layout, resources, six passes, `GPU.RadianceCascades`, views `gi` and `open` with statistics, settling stat and the capture's wait, both paths, the gate's perf cases, `LookDev.py ratio` | **G1 measured and reported** (D12), one extra tuning day, then the degrade path if needed |
| 2. Join shading | 2 | L + S-M (2.5 days) | the diffuse gather, three Post Process properties with inactive reasons, the row (Off, Low, High), `SetQuality`, off path, specular occlusion after `reflections` merged, thumbnails wait, feature test, `PerfGPU.Renderer.GI4K*` | 6.3's tests pass |
| 3. Harden | 2 | M-L (1.5 days) | tuning on the samples and the showcase's yard: thickness, cap, edge fade; docs (DG final, Manual, AGENTS.md bullet, release notes) | **the owner's live look in motion** on Third-Person and the showcase, GI toggled, in the rebuilt editor |

Turning GI on in the template and samples is not a stream step: plan.md 7, step 7, after that look.

**Cut line.** Below it, cut first to last (plan.md 4.4, with one change: the players' Low value moves above the cut, section 12):

1. The Ultra layout (D15.1).
2. The bent direction (D15.2).
3. AO at 2x6 when cascades run (D15.3).
4. Time keys of the irradiance volume (D15.4).
5. The irradiance volume (D15.5).

If G1 fails in every degrade form and D7's filter (D12.4): the stream stops at its debug views (branch kept for wave 9) and slot 2 goes
to the irradiance volume, which moves above the cut (plan.md 4.4, "If G1 fails").

## 4. Where each setting lives

| Feature | Kind | Setting | Inspector | Agents and scripts |
| --- | --- | --- | --- | --- |
| The cascades' debug views | viewport setting, renderer option | `debugView` `"gi"`, `"open"`; `SceneLightingView::GlobalIllumination`, `OpenShare` | the Viewport's debug chip and legend | `viewport.setSettings`, `viewport.capture {debugView}` with statistics |
| Tuning for tests and the gate | renderer specification | `SceneRendererSpecification::RadianceCascades` (`RadianceCascadeSettings`): filter passes, cascade limit, view occlusion, history (D12.4 only) | none | none (tests and perf cases) |
| A scene asks for GI (stage 2) | Post Process | `GlobalIllumination` (off), `GlobalIlluminationIntensity` (1), `GlobalIlluminationOcclusion` (1) | generic, section "Global illumination", inactive reasons | `component.set`, `SetProperty` |
| How much GI a machine affords (stage 2) | quality row | `Renderer.Quality.GlobalIllumination`: Off, Low, High (Ultra below the cut) | Graphics Quality page (generic) | `project.setSettings`; players through `graphics-options`' named options |
| Pictures wait for it | renderer stat | `SceneRendererStats::GlobalIlluminationSettled` | none | `viewport.capture`'s `globalIlluminationSettled` |

No script host function: properties through `Entity::GetProperty`/`SetProperty`, the row through the players' graphics options. The
script ABI does not change.

## 5. Design

### D1 Rules this stream keeps, and what it claims

- **No history.** Every frame's cascades are complete in themselves. The one input from the past is the previous frame's lit color
  (as spatial SSR's), so bounce builds over a few frames (a one-frame feedback loop through the image, never a blend of GI buffers).
  D7's one-pole filter exists only as D12.4's last resort.
- **Off costs nothing.** No pipeline, texture, pass, motion vectors or color copy is made for GI until a frame wants it; SRV 35 binds
  `Renderer::GetBlackTexture()`; the forward pass (stage 2) reads it behind a uniform branch on `ReflectionParams.y`. Every stage
  proves it (6.2, 6.3).
- **Both paths.** All GI work is compute after the complete prepass plus a fullscreen view pass: both paths feed it the same depth,
  normals, velocity and color, so `GPU.SceneRenderer.GPUDriven` and `.Feedback` stay pixel-equal (a GI case is added there, 6.2).
- **Shadow caches.** Nothing changes how casters are drawn: no cache input, no version.
- **Derivatives.** None: compute passes and a fullscreen pass that fetches by pixel.
- **Frame constants.** None appended. Claimed components, commented in both declarations (`SceneRenderData.h`, `Scene.glsl`):

  | Component | Meaning | Stage |
  | --- | --- | --- |
  | `ReflectionParams.y` | 1 when the forward pass joins GI: SRV 35 holds this frame's result (or the pending frame's zeros) and the scene asked | 2 (S0.11 assigned it) |
  | `ReflectionParams.z` | `GlobalIlluminationIntensity`, clamped to [0, 4] | 2 (S0.11) |
  | `AOParams.z` | `GlobalIlluminationOcclusion`, clamped to [0, 1]: the open share's weight on the fallback | 2 |

- **Slots.** Scene SRV 35: the GI texture (full resolution, RGBA16F: rgb the hit light pre-exposed, a the closed share), added to the
  scene layout in stage 1 (the view pass reads it through the scene layout) with a black dummy. The compute passes have layouts of
  their own (D3). `Scene.glsl`'s slot comment says "35 radiance cascades (bound from wave 8's stage 1)".
- **Stage.** `GPU.RadianceCascades` between `GPU.AO` and `GPU.Reflections` (`GPUProfiler.cpp:57-58`). S0.11 only assigns the name
  (step0.md 8.6: "registered by their streams"), so this stream registers it there and adds it at the same position to the five
  lists DG "GPU timing" names (5197-5199): `editor.perf`'s description (`EditorStateCommands.cpp:924`), `EditorProfilerTests.cpp:91`,
  `FramePerfLogTests.cpp:284`, Architecture.md's stage table (921-923) and the `strata-editor-automation` skill.
- **Perspective cameras only.** An orthographic camera's rays are parallel and its "hemisphere through the view vector" degenerates:
  orthographic frames run no cascades (`SceneRendererStats::GlobalIllumination` false; the views show the fallback). Editor
  orthographic views are flat drafting views; documented.
- **Shader language.** New shaders in what S0.10 left: in Slang, HLSL syntax only, no generics, `mul` only with a matrix, `all()`/
  `any()` on vector comparisons, `SampleLevel` in compute (plan.md 4.1). The listings below use GLSL-like pseudocode for brevity;
  register classes and slots are NVRHI's (t SRV, u UAV, b CBV and push constants, s sampler).

### D2 The layout

Terms: the output is W x H pixels. A **probe** sits at the center of a block of pixels; a **wedge** is an image-plane direction of a
probe; a **sector** is one of 32 elevation bits within a wedge; **interval** i is the range of screen distances cascade i marches.

**Formulas** (`R/RadianceCascadeLayout.cpp`, `RadianceCascades::ComputeLayout`):

| Quantity | Rule |
| --- | --- |
| Probe spacing of cascade 0, `s0` (pixels) | `max(2, round(PixelsAt1080 * H / 1080))`, `PixelsAt1080` = 2 (High), 4 (Low), 1.5 (Ultra, below the cut) |
| Spacing of cascade i | `s_i = s0 * 2^i` |
| Wedges of cascade i | `W_i = 8 * 2^i` (`c_GIWedges0 = 8`); wedge k's image direction at angle `phi = (k + 0.5) * 2 pi / W_i`, `d = (cos phi, sin phi)` in pixels (+x right, +y down) |
| Interval of cascade 0, `I0` (pixels) | `max(16, round(24 * H / 1080))`: 16 at 720 rows, 24 at 1080, 32 at 1440, 48 at 2160 |
| Interval of cascade i | `[I0 (2^i - 1), I0 (2^(i+1) - 1))`: lengths double |
| Cascades, `C` | the least count with `I0 (2^C - 1) >= sqrt(W^2 + H^2)`, clamped to [2, 8] (`c_GIMaxCascades = 8`); at most `RadianceCascadeSettings::MaxCascades` when that is set (D12.3) |
| Probe grid of cascade i | `G_i = (ceil(W / s_i), ceil(H / s_i))`; then `G_{i+1} = ceil(G_i / 2)` exactly, so grids nest without padding |
| Pyramid texel, `r` (pixels per level-0 texel) | `ceil(s0 / 2)`: 1 at 1080 rows, 2 at 1440 and 2160 |
| Pyramid level 0 size | `(ceil(W / r), ceil(H / r))`; level l: `max(1, (size_{l-1} + 1) / 2)` each way; `L = max(1, C - 1)` levels |
| Level cascade i marches | `m_i = min(max(0, i - 1), L - 1)`; its texel is `r * 2^m_i` pixels (the audit's "level max(0, i-1), plus one at 4K") |
| Steps per interval | `n_i = min(6, ceil(I0 2^i / (r 2^m_i)))` (`c_GIStepsPerInterval = 6`) |
| Atlas tiles of cascade i | `Tx_i = 2^ceil((3 + i) / 2)`, `Ty_i = W_i / Tx_i`: 4x2, 4x4, 8x4, 8x8, 16x8, 16x16, 32x16, 32x32 |
| Texel of (probe g, wedge k) in cascade i | `((k mod Tx_i) * G_i.x + g.x, (k div Tx_i) * G_i.y + g.y)`: direction-major, so the four parents of a child wedge are a 2x2 block |
| Atlases | even cascades in atlas 0, odd in atlas 1; each the largest of its cascades' `(Tx_i G_i.x, Ty_i G_i.y)` |

The layout is invalid (no GI that frame, reported once) when an atlas exceeds `GraphicsDeviceInfo::MaxTextureDimension2D`
(`GraphicsDevice.h:42`) or the output is smaller than 16x16.

The signature (stage 1; stage 2 maps the quality row's level to `pixelsAt1080` through `GetProbeSpacing`, D13):

```cpp
namespace RadianceCascades
{
	// The layout for an output size: cascade 0's probe spacing from pixelsAt1080 (High 2), at most maxCascades cascades (0: as
	// many as cover the diagonal), none when an atlas would exceed maxTextureDimension (the device's; the quality row's bytes
	// function passes RenderData::c_GIUnboundedTextureDimension = 65536, so estimates exist for every size) or the output is
	// smaller than 16x16.
	std::optional<RadianceCascadeLayout> ComputeLayout(glm::uvec2 output, float pixelsAt1080, uint32_t maxCascades, uint32_t maxTextureDimension);
}
```

**The layouts this produces** (`Renderer.RadianceCascades` checks these numbers exactly):

| Output, level | s0 | I0 | C | Grid 0 | r | Pyramid 0 | Atlas 0 / 1 | Texels | Last interval ends |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1280x720 High | 2 | 16 | 7 | 640x360 | 1 | 1280x720 | 2560x720 / 1280x720 | 3.67 M | 2,032 (diagonal 1,469) |
| 1920x1080 High | 2 | 24 | 7 | 960x540 | 1 | 1920x1080 | 3840x1080 / 1920x1080 | 8.24 M | 3,048 (2,203) |
| 2560x1440 High | 3 | 32 | 7 | 854x480 | 2 | 1280x720 | 3416x960 / 1708x960 | 6.52 M | 4,064 (2,937) |
| 3840x2160 High | 4 | 48 | 7 | 960x540 | 2 | 1920x1080 | 3840x1080 / 1920x1080 | 8.24 M | 6,096 (4,406) |
| 1920x1080 Low | 4 | 24 | 7 | 480x270 | 2 | 960x540 | 1920x540 / 960x540 | 2.07 M | 3,048 |
| 3840x2160 Low | 8 | 48 | 7 | 480x270 | 4 | 960x540 | 1920x540 / 960x540 | 2.07 M | 6,096 |
| 3840x2160 Ultra (cut) | 3 | 48 | 7 | 1280x720 | 2 | 1920x1080 | 5120x1440 / 2560x1440 | 14.64 M | 6,096 |

4K High has exactly 1080p High's texels: cost barely follows resolution. Cascade 0 at 4K High is 4 px, 8 wedges, 0-48 px; intervals
0-48, 48-144, 144-336, 336-720, 720-1488, 1488-3024, 3024-6096 (the audit's table). The long first interval deliberately breaks the
paper's penumbra condition in cascade 0 (the arc at its end is about 19 px at 1080p against 2 px spacing): merges are the lossy step,
and the model found the long first interval buys stability; faint fans around small emitters are the price.

**Memory** (`RadianceCascadeLayout::GetMemoryBytes`, which the row's traits report; MiB):

| Resource | Format | 1920x1080 High | 3840x2160 High | 3840x2160 Low |
| --- | --- | --- | --- | --- |
| Atlases (light + cover, mask) | RGBA16F + R32_UINT, 12 B/texel | 71.2 | 71.2 | 17.8 |
| Probe records, one texture per cascade | RGBA32F | 10.5 | 10.5 | 2.6 |
| Depth and radiance pyramids | RGBA16F each, L levels | 42.2 | 42.2 | 10.5 |
| Near depth, current and previous | R32F x 2, pyramid level 0's size | 15.8 | 15.8 | 4.0 |
| Gathered and filtered probes | RGBA16F x 2, grid 0 | 7.9 | 7.9 | 2.0 |
| The GI texture (SRV 35) | RGBA16F, output size | 15.8 | 63.3 | 63.3 |
| The half-resolution color copy, if nothing else made it | B10G11R11 (RGBA16F fallback), 6 levels | 2.6 | 10.5 | 10.5 |
| **Total** | | **166** | **221** | **111** |

This is above the audit's 150-170 MiB at 4K because the full-resolution output is RGBA16F (63 MiB) rather than packed and the
disocclusion check keeps two near-depth images; section 11, question 3, says what is cut if memory matters. Not counted (the
reflections row does not count them either): the velocity target and the motion history GI forces when nothing else asks for
motion (no TAA, no Spatial reflections): RG16F, 7.9 MiB at 1080p, 31.6 MiB at 4K, plus 64 bytes per instance slot. The DG section
and the release note say so. (Script check of every row above: the reviewer recomputed the layouts and the memory with the rules
of this section; they hold, including 1440p Ultra's 295 MiB.)

### D3 Resources, pipelines and bindings

All made by `EnsureRadianceCascadeResources(const RadianceCascadeLayout&)` on the first frame that wants GI, re-made when the layout
changes (resize, quality, `MaxCascades`; the old ones go through `Renderer::ReleaseDeferred`), released in `ReleaseRenderTargets`
(with `m_GIColorRender = 0`). Textures: `isUAV`, initial state `ShaderResource`, `keepInitialState`, debug names
`<renderer>.GI.<name>`. Pipelines stay once made.

| Texture | Size | Format | Written by | Read by |
| --- | --- | --- | --- | --- |
| `DepthPyramid` | pyramid level 0, L mips | RGBA16F: x nearest view depth (m, 0: none), y farthest, z near-layer share, w coverage | Pyramid | March |
| `RadiancePyramid` | the same | RGBA16F: rgb radiance (pre-exposed, capped), a validity | Pyramid | March |
| `Near[2]` | pyramid level 0 | R32F: nearest view depth | Pyramid (current) | Pyramid (previous) |
| `Probes[i]`, i < C | `G_i` | RGBA32F: x view depth (0: no probe), yz octahedral view-space normal, w coverage | Probes | March, Gather, Filter, Upsample |
| `Atlas[2]` light | atlas sizes | RGBA16F: rgb light (pre-exposed, irradiance over pi), a cover | March | March (parent), Gather |
| `Atlas[2]` mask | atlas sizes | R32_UINT: sectors covered | March | March (parent), Gather |
| `Gathered`, `Filtered` | `G_0` | RGBA16F: rgb hit light (pre-exposed), a closed share | Gather, Filter | Filter, Upsample |
| `GlobalIllumination` (SRV 35) | output | RGBA16F: rgb hit light (pre-exposed, irradiance over pi), a closed share | Upsample | view pass, Forward (stage 2) |

R32F, R32_UINT, RGBA16F and RGBA32F are core storage formats (no `ExtendedStorageFormats` variant, no B10G11R11 storage: the
MoltenVK concern of the audit).

Compute layouts (one per pipeline; `VolatileConstantBuffer(0)` is the frame constant buffer; push constants at b1, which no constant
buffer of these layouts uses; `Sampler(8)` linear clamp):

| Pipeline | Shader | Bindings | Groups | Dispatch |
| --- | --- | --- | --- | --- |
| Pyramid base | `GI/RadianceCascadesPyramidBase.comp`: the variant of `GI/RadianceCascadesPyramid.comp` with `ST_GI_PYRAMID_BASE` (registered as `"GI/RadianceCascadesPyramid.comp\|GI/RadianceCascadesPyramidBase.comp\|ST_GI_PYRAMID_BASE"`, the AO variants' form, `Strata/CMakeLists.txt:177-179`) | b0; t0 depth; t1 velocity; t2 previous color copy; t3 previous `Near`; s8; u0 `DepthPyramid` level 0; u1 `RadiancePyramid` level 0; u2 current `Near`; b1 `RadianceCascadePyramidParameters` | 8x8 | pyramid level 0 texels |
| Pyramid reduce | `GI/RadianceCascadesPyramid.comp` | b0; t4 `DepthPyramid` level l-1; t5 `RadiancePyramid` level l-1; u0 level l; u1 level l; b1 | 8x8 | level l, l = 1 .. L-1 |
| Probes | `GI/RadianceCascadesProbes.comp` | b0; t0 depth; t1 normals; t2 `Probes[i-1]`; u0 `Probes[i]`; b1 `RadianceCascadeProbeParameters` | 8x8 | `G_i`, i = 0 .. C-1 (i = 0 reads pixels, the rest children) |
| March and merge | `GI/RadianceCascadesMarch.comp` | b0; t0 `DepthPyramid`; t1 `RadiancePyramid`; t2 `Probes[i]`; t3 `Probes[i+1]` (a 1x1 dummy for the top); t4, t5 parent atlas light, mask; u0, u1 this atlas light, mask; b1 `RadianceCascadeMarchParameters` | 8x8x1 | `(ceil(G_i.x / 8), ceil(G_i.y / 8), W_i)`, i = C-1 down to 0 |
| Gather | `GI/RadianceCascadesGather.comp` | b0; t2 `Probes[0]`; t4, t5 atlas 0 light, mask; u0 `Gathered`; b1 `RadianceCascadeProbePassParameters` | 8x8 | `G_0` |
| Filter | `GI/RadianceCascadesFilter.comp` | b0; t2 `Probes[0]`; t6 input; u0 output; b1 `RadianceCascadeProbePassParameters` | 8x8 | `G_0`, `FilterPasses` times (ping-pong between two sets; D8) |
| Upsample | `GI/RadianceCascadesUpsample.comp` | b0; t0 depth; t1 normals; t2 `Probes[0]`; t6 the last filter pass's output (a set per source); u0 `GlobalIllumination`; b1 `RadianceCascadeProbePassParameters` | 8x8 | output pixels |

Binding sets: the march has one set per cascade (it alternates atlases, so NVRHI's automatic state tracking places the barriers: each
dispatch reads one atlas and writes the other); the pyramid one per level; the probes one per cascade; the filter two (ping-pong). No
dispatch reads its own UAV writes through the same set, so no explicit `setTextureState` is needed (AGENTS.md's NVRHI pitfall does
not arise).

Push-constant structs (`SceneRenderData.h`, `static_assert` on each size, mirrored in `Include/RadianceCascades.glsl`):

```cpp
// GI/RadianceCascadesPyramid.comp (b1).
struct RadianceCascadePyramidParameters
{
	glm::uvec2 SourceSize;      // The base pass: the viewport's pixels; a reduction: level l - 1's texels
	glm::uvec2 DestinationSize; // Level l's texels
	uint32_t BlockPixels;       // r: pixels per level-0 texel each way
	uint32_t SourceValid;       // 1 when the previous color copy and Near[previous] hold the renderer's previous frame
	float HistoryScale;         // This frame's pre-exposure over the copy's
	float RadianceCap;          // Pre-exposed asymptote of the soft cap (D4), its knee at half of it
};
static_assert(sizeof(RadianceCascadePyramidParameters) == 32);

// GI/RadianceCascadesProbes.comp (b1).
struct RadianceCascadeProbeParameters
{
	glm::uvec2 GridSize;   // This cascade's probes
	glm::uvec2 ChildSize;  // Cascade 0: the viewport's pixels; cascade i: cascade i - 1's grid
	uint32_t Spacing;      // s_i in pixels
	uint32_t Cascade;
	uint32_t Padding[2];
};
static_assert(sizeof(RadianceCascadeProbeParameters) == 32);

// GI/RadianceCascadesMarch.comp (b1).
struct RadianceCascadeMarchParameters
{
	glm::uvec2 GridSize;       // Cascade i's probes
	glm::uvec2 ParentGridSize; // Cascade i + 1's ((0, 0) for the top cascade: nothing to merge)
	uint32_t Wedges;           // W_i
	uint32_t TilesX;           // Tx_i
	uint32_t ParentTilesX;     // Tx_{i+1}
	uint32_t Steps;            // n_i
	float Spacing;             // s_i in pixels
	float ParentSpacing;       // s_{i+1}
	float IntervalStart;       // Pixels
	float IntervalEnd;
	uint32_t MarchLevel;       // m_i
	uint32_t ColorLevel;       // min(m_i + c_GIColorLevelBias, L - 1)
	float TexelPixels;         // r * 2^m_i
	float EdgeFadePixels;      // I0 * c_GIEdgeFadeIntervals
	uint32_t FillLevel;        // min(ColorLevel + c_GIColorFillLevels, L - 1): the level whose valid mean fills unknown radiance (D6)
	float PixelsPerMeterAt1m;  // 0.5 H |Projection[1][1]|: pixels a meter spans at 1 m of view depth (mpp(z) = z / this)
	float PlaneSlackPixels;    // c_GIPlaneSlack * (0.5 Spacing + TexelPixels): the self-plane test's slack (D6)
	uint32_t Padding;
};
static_assert(sizeof(RadianceCascadeMarchParameters) == 80);

// Gather, Filter, Upsample (b1): one struct, each pass reads what it needs.
struct RadianceCascadeProbePassParameters
{
	glm::uvec2 GridSize; // G_0
	uint32_t TilesX;     // Gather: Tx_0
	uint32_t Stride;     // Filter: the a-trous stride (1, 2, 4, 8); Upsample: s0
};
static_assert(sizeof(RadianceCascadeProbePassParameters) == 16);
```

Push constants are at most 128 bytes on every target (Vulkan's guaranteed minimum, MoltenVK's too); the largest struct here is 80.

Constants (`R/RadianceCascadeLayout.h`, mirrored as `ST_GI_*` defines in `Include/RadianceCascades.glsl` with a comment naming the C++
constant, as `Scene.glsl:152-154` mirrors `RenderData`'s):

| Constant | Value | Meaning |
| --- | --- | --- |
| `c_GISectors` | 32 | elevation bits per wedge |
| `c_GIWedges0` | 8 | cascade 0's wedges |
| `c_GIStepsPerInterval` | 6 | the most steps per interval |
| `c_GIThicknessMin`, `c_GIThicknessScale`, `c_GIThicknessReach` | 0.15 m, 0.03, 4 | a texel's thickness guess (D6) |
| `c_GISelfBias` | 0.02 | self-plane rejection per meter of distance |
| `c_GIPlaneSlack` | 1.0 | the self-plane test's slack in footprints: `c_GIPlaneSlack * (0.5 s_i + TexelPixels)` pixels at the sample's depth (D6) |
| `c_GIColorFillLevels` | 2 | where a radiance texel's validity is below 1, the levels above it whose valid mean fills the rest (D6; 0 is the unfilled form) |
| `c_GIUnboundedTextureDimension` | 65536 | the texture limit the quality row's bytes function passes to `ComputeLayout` |
| `c_GIEdgeFadeIntervals` | 1.0 | hits fade over `I0` pixels at the screen's edges |
| `c_GIColorLevelBias` | 0 | the radiance level above the march level (the candidate's) |
| `c_GIRadianceCap` | 16 | the soft cap's asymptote in exposed units (D4) |
| `c_GIFilterPasses` | 4 | the a-trous passes (default of `RadianceCascadeSettings::FilterPasses`) |
| `c_GISettleFrames` | 6 | frames with a valid source before `GlobalIlluminationSettled` (stage 1 measures and lowers it, 6.2) |
| `c_GIMaxViewDepth` | 60,000 m | farther depths count as no surface (half floats hold 65,504) |

`SceneRendererSpecification` gains (stage 1):

```cpp
// The radiance cascades' tuning (DG "Radiance cascades"): tests and gate G1's degrade forms set it; programs keep the defaults.
struct RadianceCascadeSettings
{
	uint32_t FilterPasses = RenderData::c_GIFilterPasses; // A-trous passes over cascade 0's probes, 0 to 6 (strides 1, 2, 4, ...)
	uint32_t MaxCascades = 0;    // 0: as many as cover the screen's diagonal; else at most this many (2 to 8): rays end sooner
	// Stage 1 only: the open share's weight on the fallback in the gi view (stage 2 replaces it with
	// PostProcessComponent::GlobalIlluminationOcclusion and removes the field). 1: the cascades' occlusion; 0: bounce only.
	float ViewOcclusion = 1.0f;
	bool operator==(const RadianceCascadeSettings&) const = default;
};
RadianceCascadeSettings RadianceCascades;
```

### D4 The pyramids: depth, radiance and the previous frame's light

**Base pass** (one thread per level-0 texel, i.e. a block of `r x r` pixels; pixels past the viewport's edge left out):

```
surfaces = 0; near = +inf; far = 0; valid = 0; sum = 0
for each pixel p of the block with device depth > 0:
    z = view depth of p (InverseProjection; > c_GIMaxViewDepth counts as none)
    surfaces++; near = min(near, z); far = max(far, z)
    P = camera-relative world position of p (InverseViewProjection)
    if SourceValid:
        prevUV = uv(p) - velocity(p)                       // velocity: current minus previous UV (Scene.glsl:146-150)
        if prevUV inside [0, 1]^2:
            zExpected = (PreviousViewProjection * vec4(P, 1)).w      // the point's view depth in the previous frame
            zPrevious = Near[previous] at floor(prevUV * levelZeroSize)
            if zPrevious > 0 and |zPrevious - zExpected| <= 0.02 zExpected + 0.03:      // not a disocclusion (the model's test)
                c = SampleLevel(previousCopy, prevUV, 0).rgb * HistoryScale
                sum += SoftCap(c, RadianceCap); valid++
share = (count of p with |z - near| <= 0.02 near + 0.05) / r^2      // r = 1: 1 for a surface
DepthPyramid[0] = (surfaces ? near : 0, far, surfaces ? share : 0, surfaces / r^2)
RadiancePyramid[0] = (valid ? sum / valid : 0, surfaces ? valid / surfaces : 0)
Near[current] = surfaces ? near : 0
```

- The copy is the previous frame's HDR image after the opaque pass and the sky, pre-exposed (B10G11R11 or RGBA16F); it holds direct
  light, emission, today's indirect light and, from stage 2 on, the previous frame's GI: one more bounce per frame. A disoccluded or
  off-screen point gets no color (validity drops) instead of the wrong surface's. Validity is about the **color** only: the march
  fills a texel's unknown share from the valid mean of a coarser level and lets cover fall only where no level nearby knows the
  color (D6). The model never dropped cover on a disocclusion (its source substituted the current frame's direct light,
  `rc_spike.cpp:1232-1260`, which the engine cannot: the prepass has no albedo), so an unfilled coupling of cover to validity
  would open the sky in a strip behind every moving silhouette, on every frame of the fence strafe.
- It also holds what the forward pass applies on top of surfaces: height fog and aerial perspective (`Forward.frag`'s end). Distant
  hits re-emit haze, and dense fog brightens bounce. Section 10 names the check.
- **The soft cap** (`SoftCap` in `Include/RadianceCascades.glsl`), so sun glints on metal and bulbs do not sparkle in the bounce:
  `y = Luminance(c); knee = cap / 2; if y <= knee return c; capped = knee + (cap - knee)(1 - exp(-(y - knee) / (cap - knee)));
  return c * capped / y` (continuous, slope 1 at the knee, asymptote `cap`). `RadianceCap = c_GIRadianceCap * f` pre-exposed, with
  `f = 0.25` when the pre-exposure follows automatic exposure (it is then about a quarter of the exposure's multiplier, `SceneRenderer.h`
  stats comment 657-664) and `f = 1` with manual or forced pre-exposure (it is then the multiplier). So the cap sits 4 stops above an
  exposed white surface whatever the exposure. Tuned in stage 3. The pre-exposure moves in whole stops, so under automatic exposure
  the cap's absolute level steps by 2x when the pre-exposure does (walking into the hut): only light already above the knee changes,
  and stage 3 checks it (section 10).

**Reduction** (level l from l - 1; the 2x2 children `min(2x + dx, w - 1)`, as the model clamps):

```
near = min over children with x > 0; far = max of y
share = sum over children with |x_c - near| <= 0.02 near + 0.05 of z_c / 4
coverage = mean of w_c
radiance = sum(rgb_c a_c w_c) / sum(a_c w_c); validity = sum(a_c w_c) / sum(w_c)      (0 when the sums are 0)
```

The share is what keeps thin things continuous as they cross texels (the model's `nearshare=1`, `sweep4.log`).

### D5 Probe records

- **Cascade 0** (one thread per probe, its block `[g s0, (g + 1) s0)^2` of pixels): the view depths of its surface pixels (at most
  16 at s0 = 4; `s0 <= 4` at High, larger blocks at Low read a strided 4x4 subset of their pixels, documented); the probe's depth is
  the upper median (sorted, index n / 2: the model's `nth_element(size / 2)`); its normal the normalized sum of the view-space normals
  of pixels with `z in [0.9 zp - 0.1, 1.1 zp + 0.1]`; coverage `n / s0^2` over the whole block (pixels outside the viewport count as
  empty). Valid when `2 n >= s0^2`, or `n > 0` when `s0 <= 2`, and the normal sum is at least 1e-3 long; invalid probes store 0 depth.
- **Cascade i > 0** from its four children `2g + (0|1, 0|1)` in cascade i - 1 (missing children count as empty): coverage the mean of
  the children's; depth the upper median of the valid children's depths; normal the normalized sum of valid children's normals within
  `[0.9 z - 0.1, 1.1 z + 0.1]`; valid when coverage >= 0.5 and the sum is long enough. This replaces the model's exact block median for
  large blocks (a median of medians); `Renderer.RadianceCascades` cannot check it on the CPU, so 6.2's "Probe depths follow their
  blocks" checks it on the GPU against an exact median.
- A probe's view position: `P = ViewRay(center) * (zp / -ViewRay(center).z)`, center `((g + 0.5) s_i, (g.y + 0.5) s_i)` pixels,
  `ViewRay(pixel) = (InverseProjection * vec4(UVToNDC(pixel / size), 1, 1)).xyz` (the jittered projection the frame rendered with).
  All GI geometry is in view space; `V = normalize(-P)`.

### D6 The march per interval

One thread per (probe g, wedge k) of cascade i. Invalid probe: write light 0, cover 0, mask 0 and stop.

**The slice.** `P_off = ViewPositionAt(center + 4 d, zp)` (same view depth), `D = P_off - P`, `T = normalize(D - V dot(D, V))`.
Sector j covers `theta in [j pi / 32, (j + 1) pi / 32)` from V toward T. With `theta_m` its middle,
`c_j = cos(theta_m) dot(N, V) + sin(theta_m) dot(N, T)`; **visible** bit j iff `c_j > 0` (each probe limits the sectors to its own
hemisphere); weight `w_j = 2 c_j (cos theta_a - cos theta_b) / W_i` (cosine-weighted solid angle over pi, this wedge's share:
`delta phi / pi = 2 / W_i`). The 32 `cos`/`sin`/`cos a - cos b` values are a constant table.

**The steps.**

```
mask = 0; light = 0; cover = 0
for t in 0 .. Steps - 1:
    s = IntervalStart + (t + 0.5) / Steps * (IntervalEnd - IntervalStart)
    sample = center + d * s
    if sample outside [0, W) x [0, H): break                       // rays leaving the screen count as open
    (zN, zF, share) = DepthPyramid[MarchLevel] at floor(sample / TexelPixels)
    if zN == 0: continue
    thick = max(0.15, 0.03 zN); zB = zN + thick
    if zF > 0: zB = max(zB, min(zF, zN + 4 thick))                 // raised up to 4 times toward the texel's far depth
    Qf = ViewPositionAt(sample, zN); Qb = ViewPositionAt(sample, zB)
    slack = PlaneSlackPixels * zN / PixelsPerMeterAt1m             // a footprint's height at the sample's depth (below)
    if dot(N, Qf - P) <= c_GISelfBias |Qf - P| + slack: continue    // the probe's own plane covers nothing of its hemisphere
    thF = acos(clamp(dot(normalize(Qf - P), V), -1, 1)); thB = likewise for Qb; order them
    first = clamp(floor(thF 32 / pi), 0, 31); last = clamp(ceil(thB 32 / pi), first + 1, 32)
    bits = (last - first >= 32 ? 0xFFFFFFFF : (1 << (last - first)) - 1) << first
    fresh = bits & visible & ~mask
    if fresh != 0:
        (R, v) = RadiancePyramid[ColorLevel] at floor(sample / (r 2^ColorLevel))
        if v < 1 and FillLevel > ColorLevel:                       // fill the unknown share from a coarser level's valid mean
            (Rc, vc) = RadiancePyramid[FillLevel] at floor(sample / (r 2^FillLevel))
            known = v + (1 - v) vc
            R = known > 0 ? (R v + Rc vc (1 - v)) / known : 0; v = known
        weight = sum over j in fresh of w_j * clamp((min(thetaB_j, thB) - max(thetaA_j, thF)) / (pi / 32), 0, 1)   // fractional
        weight *= clamp(min(sample.x, sample.y, W - sample.x, H - sample.y) / EdgeFadePixels, 0, 1) * share * v
        light += R * weight; cover += weight
    if share >= 0.5: mask |= bits & visible
```

**The self-plane slack.** Without it the test `dot(N, Qf - P) <= 0.02 |Qf - P|` (the model's, `rc_spike.cpp:917-919`) rejects a
plane's own samples only to within 2% of their distance, and three offsets of a footprint's size are larger at the first steps:
the probe's depth is a block's upper median, behind or in front of the plane at the block's center by up to half the block's
depth gradient (offset over distance `0.25 s_i sin 2a / k` for a sample `k` pixels away on a plane seen at angle `a`: 0.125 at
the first step of cascade 0 at 1080p, so a ground plane seen at more than 5 degrees fails); the march reads the nearest depth of
an `r x r` texel at 1440 rows and above; and the pyramid's half floats round depth to 2^-11 of itself. Each makes the plane's own
samples cover its horizon sectors by a fraction of a percent, changing with the median's pixel as the camera moves, and makes
6.2's "Flat ground under an open sky is all open" and 6.3's pixel equality fail. The slack, `(0.5 s_i + TexelPixels)` pixels of
height at the sample's depth (2 pixels at cascade 0 at 1080p, 4 at 4K: 0.19% of the depth either way, four times the half-float
rounding), removes all three; what it ignores is an occluder less than a footprint above the plane right beside the probe, which
GTAO keeps for contact. `c_GIPlaneSlack` is on the tuning day's list (D12.1).

**The fill.** `FillLevel = min(ColorLevel + c_GIColorFillLevels, L - 1)`. A strip disoccluded behind a moving post (one to a few
pixels a frame) has no color in the base level, but the level two above it, 4 x 4 texels, does: the occluder keeps its cover and
takes the neighborhood's color. Where both levels know nothing (the first frame, a cut, a wide disocclusion after a fast turn) the
cover falls with `v` and the fallback shows there, as on pending frames. The extra fetch happens only for fresh bits with `v < 1`.
`c_GIColorFillLevels` 0 is the unfilled form, measured against 2 on the tuning day.

What each choice is for (the model's sweeps): the median probe depth and the fractional cover halve the flicker of the paper's form
(`sweep1.log`, `sweep2.log`); the near-layer share and the 0.15 m thickness tame thin geometry (`sweep4.log`: 1.62% to 1.02%); the long
first interval helps most (`sweep5.log`: 0.60% to 0.52%); thicker occluders hurt (`sweep3.log`, thick1). Sub-rays and 16 wedges barely
help and are not built.

### D7 The merge (in the same dispatch, top down)

The top cascade only marches. Cascade i < C - 1 then takes, for its wedge k, what the two child wedges `2k` and `2k + 1` of cascade
i + 1 (already merged) hold behind its own mask, from the four parent probes around it:

```
u = center / ParentSpacing - 0.5; a = floor(u); f = u - a
mpp = zp / PixelsPerMeterAt1m                                 // meters per pixel at the probe's depth (0.5 H |Projection[1][1]|)
tolerance = max(0.05, 0.04 zp) + 0.5 ParentSpacing mpp
for each q in a + {0,1}^2 inside the parent grid with a valid record:
    weight_q = bilinear(f, q) / (1 + (|dot(Pq - P, N)| / tolerance)^2) + 1e-5
normalize the weights (no parent: the wedge keeps its own light; beyond its interval counts as open)
votes[32] = 0
for child in {2k, 2k + 1}, for each q:
    farMask = mask(q, child); all = popcount(farMask)
    if all > 0:
        open = popcount(farMask & ~nearMask)
        light += lightRGB(q, child) * weight_q * open / all
        cover += cover(q, child) * weight_q * open / all
    for each bit j of farMask: votes[j] += weight_q / 2
mask = nearMask | (bits j with votes[j] >= 0.5)               // the parents' masks combined by weighted vote
```

The sectors of different probes are measured in their own slices; at the merge they are taken as the same angles (the model's
approximation; merges are where the lossy step lives, which is why the first interval is long). Merge occlusion matters: without it
the model's open share flickered at 14.5% (`sweep3.log`, nomergeocc).

### D8 Gather, filter and upsample

**Gather** (one thread per cascade-0 probe): over its 8 wedges, recomputing each slice's visible bits and weights,
`all = sum of w_j over visible sectors`; `hit = sum of light_k / max(all, 0.25)`; `closed = clamp(sum of cover_k / max(all, 1e-4),
0, 1)`. Write `(hit, closed)`; invalid probes write zeros (the filter and upsample skip them by their record). The output stores the
**closed** share (1 - open) so that a surface nothing on screen covers holds exactly 0 through every average and the forward pass
gets an open share of exactly 1 (the pixel-equality test of stage 2 depends on it).

**Filter** (`FilterPasses` a-trous passes, strides 1, 2, 4, 8): per valid probe p, over q = p + stride (dx, dy), dx, dy in [-2, 2],
valid, weight `k(dx) k(dy) / (1 + (|dot(Pq - Pp, Np)| / tol)^2) * max(dot(Nq, Np), 0)^4`, `k = (1/16, 1/4, 3/8, 1/4, 1/16)`,
`tol = max(0.02, 0.015 zp)`; hit and closed averaged by weight (no weight: unchanged). Deterministic: stills repeat with TAA off.
The passes alternate two binding sets (Gathered to Filtered, Filtered to Gathered); the upsample has one set per source and binds
the one that holds the last pass's result (no copy for an odd count). `FilterPasses = 0` upsamples `Gathered`.

**Upsample** (one thread per pixel): pixels without depth write `(0, 0, 0, 0)`. Otherwise the four cascade-0 probes around the pixel,
`u = (x + 0.5) / s0 - 0.5`, weight `(bilinear + 1e-3) / (1 + (|dot(Pq - Ppx, Npx)| / tol)^2) * max(dot(Nq, Npx), 0)^4`,
`tol = max(0.03, 0.02 zpx)`, the pixel's position from its depth and normal from the prepass. Total below 1e-6: `(0, 0, 0, 0)` (the
fallback alone). Write `(hit, closed)` into `GlobalIllumination` (SRV 35).

### D9 Frame integration

**Gating** (`RenderFrame`, after the reflections' decisions at 2971-2991):

```cpp
// Stage 1: only while a gi or open view shows. Stage 2 adds the scene's ask (D13).
const bool giViewShown = options.LightingView == SceneLightingView::GlobalIllumination || options.LightingView == SceneLightingView::OpenShare;
const bool giWanted = giViewShown && !camera.Orthographic;
```

- `m_MotionFrame` (2914) also when `giWanted`: the base pass needs velocity.
- After the motion and temporal setup: `layout = ComputeLayout(...)`; `m_GIFrame = giWanted && m_MotionFrame && layout &&
  EnsureSceneColorResources() && EnsureRadianceCascadeResources(*layout)`. `giSourceValid = m_GIFrame && m_GIColorRender != 0 &&
  m_GIColorRender == m_RenderCount && !cameraCut && !historyReset && (m_GIColorSceneId == renderScene.GetId() ||
  renderScene.GetCloneSource() == m_GIColorSceneId)`, as SSR's (2980-2981); without TAA, `m_ResetHistory = false` (as 2982-2983).
- `giWanted && !m_GIFrame` (a failed allocation, an invalid layout) sets `degraded` and reports once.
- **Pending frames** (no valid source: the first, a cut, a reset, another scene) run the whole pipeline with `SourceValid = 0`: every
  radiance texel has validity 0, so hits weigh nothing, the output is `(0, 0, 0, 0)` and the frame shows the fallback alone, the near
  depth is written for the next frame, and `GPU.RadianceCascades` costs the same every frame.
- **Settling:** `m_GISettledFrames = giSourceValid ? m_GISettledFrames + 1 : 0`. Stats (`SceneRendererStats`, beside SSR's at 643-652):

  ```cpp
  // Global illumination (radiance cascades; DG "Radiance cascades"): whether the frame ran them (stage 1: a gi or open view
  // showed; stage 2: also the scene asked and the quality allows; perspective cameras only), whether it had no previous frame's
  // color to light them with (the first frame, a camera cut, a reset: the fallback alone), and whether the light settled:
  // c_GISettleFrames frames with that color in a row (true when the frame ran none). Bounce builds over frames: pictures wait.
  bool GlobalIllumination = false;
  bool GlobalIlluminationPending = false;
  bool GlobalIlluminationSettled = true;
  ```

- **Pass order:** at 3346, after `GPU.AO` and before `GPU.Reflections`:

  ```cpp
  if (m_GIFrame)
  {
  	gpuTimer.Next(gpuStages.RadianceCascades);
  	commandList->beginMarker("RadianceCascades");
  	RenderRadianceCascades(commandList, giSourceValid); // pyramid, probes, march C-1..0, gather, filter, upsample
  	commandList->endMarker();
  }
  ```

  It reads the previous frame's copy before this frame overwrites it in `GPU.Transparent`; SSR's spatial trace (same stage order)
  reads the same copy's blurred levels afterwards, unchanged.
- **The copy for the next frame:** 3393 becomes `(m_GlassBatchCount > 0 && m_SceneColorTexture) || m_ReflectionCopyFrame || m_GIFrame`,
  and after the copy, when `m_GIFrame`: `m_GIColorRender = m_RenderCount + 1; m_GIColorSceneId = renderScene.GetId();
  m_GIColorPreExposure = m_PreExposure`. `HistoryScale = m_PreExposure / m_GIColorPreExposure`. `Near` flips index each GI frame.
- **Rebinding:** `m_BoundGlobalIllumination` joins the rebind check (3199-3202) and `CreateSceneBindingSets` binds SRV 35 to
  `GlobalIllumination` or `Renderer::GetBlackTexture()` in all four sets (1036-1044 pattern).
- **Members** (beside SSR's, `SceneRenderer.h` 2676-2691): `m_GIFrame`, `m_GISettledFrames`, `m_GIColorRender`, `m_GIColorSceneId`,
  `m_GIColorPreExposure`, `m_GINearIndex`, the layout (`std::optional<RadianceCascadeLayout> m_GILayout`), textures, sets, pipelines.
- **Accessors:** `nvrhi::ITexture* GetGlobalIlluminationTexture() const` (null without), `const std::optional<RadianceCascadeLayout>&
  GetRadianceCascadeLayout() const`, `uint64_t GetRadianceCascadeMemoryBytes() const` (0 without).

### D10 The debug views `gi` and `open`, and their statistics (stage 1)

They must not touch the forward pass: the cascades light themselves from the previous frame's **lit** image, which a view shaded by
the forward pass would replace. So the forward pass shades the lit image as usual (its `MaterialParams.y` is 0 for these two views)
and a fullscreen **view pass** writes the view into the HDR target after the frame's color copy:

- `SceneLightingView` gains `GlobalIllumination = 6` (bounce plus the open share of today's light, white albedo, diffuse only) and
  `OpenShare = 7` (the share of each surface's hemisphere nothing on screen covers, as gray); `Scene.glsl` gains
  `ST_LIGHTING_VIEW_GLOBAL_ILLUMINATION 6u` and `ST_LIGHTING_VIEW_OPEN_SHARE 7u` (`Include/LightingView.glsl` mirrors what the stats
  pass needs). `SceneRenderer::GetForwardLightingView(view)` returns `None` for the two; 3141 writes it into `MaterialParams.y`.
  `IsLightingValueView` (C++ 73-76, used by `ChoosePreExposure`'s hold at 2891, and `LightingViewStats.comp:27-30`) adds `OpenShare`;
  `LightingView.frag` shows `OpenShare` as gray like AO (its branch at 37), the stats pass reads its value from `hdr.r` like AO's (54),
  and `RecordLightingView` draws its overlay (the condition at 234).
- **View pass** (`GI/GlobalIlluminationView.frag`, fullscreen vertex shader, layouts {scene (set 0), bindless (set 1), own (set 2:
  t0 depth, t1 normals, b1 `{ uint View; float Intensity; float Occlusion; uint Valid; }`)}), into `m_GIViewFramebuffer` (the HDR
  target as its only attachment, no depth, no blending), in `GPU.Transparent` right after the copy, on frames with `giViewShown`
  (also when `m_GIFrame` is false: then `Valid = 0` and the views show the fallback). Per pixel with depth: camera-relative position
  and world normal (the prepass's, through `InverseView`); `gi = Valid ? texelFetch(SRV 35) : 0`; `open = 1 - gi.a`;
  `fallback = GetDiffuseFallback(position, normal)` (below); `OpenShare` writes `vec3(open)` unexposed; `GlobalIllumination` writes
  `(gi.rgb / TemporalParams.y * Intensity + fallback * mix(1, open, Occlusion)) * TemporalParams.y`. Stage 1: `Intensity = 1`,
  `Occlusion = RadianceCascadeSettings::ViewOcclusion`; stage 2: the Post Process's.
- `GetDiffuseFallback(position, normal)` (stage 1 adds it to S0.11's `Include/IndirectLight.glsl`; stage 2's diffuse gather uses it):
  today's diffuse irradiance over pi as `Forward.frag:257-299` computes it: `probes = EvaluateReflectionProbes(position, normal,
  normal, 1)` when `u_ProbeCounts.x > 0`; `sky = environment ? irradianceMap(RotateEnvironmentDirection(normal)) *
  EnvironmentParams.x : AmbientColor` (the rotation as `Forward.frag:259` and S0.11's `GetIndirectInputs` apply it); result
  `probes.Irradiance + sky * (1 - probes.Weight)`. (The `(1 - specularColor)` factor belongs to shading, not to the light.) The
  stage-2 forward pass does not call it (it keeps S0.11's four forms, D13); a GPU case compares the two (6.3).
- In these two views the transparent pass is not drawn (their GI is the fallback alone; the view shows opaque surfaces), and the
  temporal history resets on the change of view (`BeginLightingViewFrame`, 78-95); `OpenShare`, a value view, holds the pre-exposure.
- **Statistics.** `LightingViewStats.comp` gains t2 = SRV 35's texture (or black) in its layout (`SceneRendererLightingView.cpp:104-113`:
  t0, t1, t3, s8 and u0 are taken, t2 is free), and `RenderData::LightingViewParameters::Padding0` (`SceneRenderData.h`, the struct at
  the file's end) becomes `float GIIntensity` (the view pass's `Intensity`; the struct stays 80 bytes). `GlobalIllumination` counts EV100 of
  surfaces and background as the light views do **and** the per-pixel hit share `Luminance(gi.rgb * Intensity) /
  max(Luminance(hdr), 1e-6)` (both pre-exposed) into the value bins; `OpenShare` counts the open share (`hdr.r`) into the value bins.
  `ResolveLightingViewStats` (257-312) fills `ValueMedian`/`ValueP05` for both and `SurfaceEV100` for `GlobalIllumination`.
- **Editor.** `ViewportDebugView` gains `GlobalIllumination` and `OpenShare`, names `"gi"` and `"open"` (S0.11 may have added the
  names), `GetLightingView` maps them; `DebugViewSchema` (144-148) and both commands' descriptions (1066, 1196) list them ("gi" shows
  bounce light and the open share of the probes' and sky's light on white albedo; "open" the share of each surface's hemisphere the
  screen leaves open); `LookDev.py`'s `DEBUG_VIEWS` (56) too. The debug chip (`ViewportPanel.cpp:639-666`): labels "GI" and "Open"
  beside "AO", popup entries "GI: bounce and open share on white" and "Open share: what the screen does not cover", and the chip's
  tooltip gains ", bounce light, or the share the screen leaves open"; the legend for `open`: three gray swatches "Open 1", "Half
  0.5", "Covered 0" (theme tokens, `Widgets.h`).
- **The core toolset's size.** `viewport.capture` is in `core` (`McpToolsets.cpp:29`), whose `tools/list` must stay within 30,720
  bytes (`EditorMcpToolsetsTests.cpp:69`). Measure its size on the branch point first (the test's `defaultBytes`, printed with a
  `MESSAGE` while working, not committed). The capture's `debugView` description gains at most 120 bytes (`"gi" bounce light and
  the open share, "open" the share the screen leaves open, each with debugView.globalIllumination`); the full wording goes into
  `viewport.setSettings`' description (not core) and DG "View commands". If the margin is smaller, only the enum grows.
- **The capture's description** (`DescribeDebugView`, 214-234): for these views `lighting` as for every lighting view (for `gi` the
  `value` fields are the hit share, for `open` the open share), and a new `globalIllumination` object from the renderer:
  `{ "ran", "pending", "settled", "settledFrames", "cascades", "probeSpacing", "wedges", "intervalPixels", "texels", "memoryMiB" }`
  (`null` layout fields when it ran none). The capture's result gains `globalIlluminationSettled` beside `preExposureSettled` (910).
  `DescribeRenderStats` (`EditorStateCommands.cpp:207`) gains `globalIllumination: { ran, pending, settled }`.
- **The capture waits** (stage 1, because its own look-dev pictures need it; section 12): `c_MaxCaptureGIFrames = 12` beside 61; after
  `reflectionsPending` (770): `giPending = !contentPending && progress.Stats.GlobalIllumination && !stats.GlobalIlluminationSettled &&
  progress.GIFrames < c_MaxCaptureGIFrames`, keeping the history (it must not reset GI), counted in `progress.GIFrames`.

### D11 The measuring step (stage 0)

**The metric** (`StrataTests/src/Renderer/FlickerMetrics.h/.cpp`, test helpers, CPU), the audit's definition made exact:

```cpp
namespace Strata::Tests
{
	// One frame of a measured sequence, read back from a renderer that rendered it with HDROutput and a forced pre-exposure.
	struct FlickerFrame
	{
		uint32_t Width = 0, Height = 0;
		std::vector<float> Value;      // Rec. 709 luminance of the measured image (the HDR target)
		std::vector<float> ViewDepth;  // Meters along the camera's forward axis; 0 where nothing was drawn
		std::vector<glm::vec3> Normal; // World space, unit (the prepass's)
		glm::mat4 ViewProjection;      // The frame's, as rendered (jittered with TAA), camera-relative
		glm::dvec3 CameraPosition;
		glm::vec3 CameraForward;
	};
	struct FlickerResult
	{
		double Mean = 0, P99 = 0;          // Per pixel |d|
		double Blocks = 0, BlocksP99 = 0;  // Per 32x32 block |mean d|
		double MeasuredShare = 0;          // Pixels measured over all pixels
		uint32_t Frames = 0;               // Triples averaged
	};
	FlickerFrame ReadFlickerFrame(SceneRenderer& renderer, const SceneCamera& camera); // HDR, depth, normals; Renderer::ReadTexture
	FlickerResult MeasureFlicker(const FlickerFrame& a, const FlickerFrame& b, const FlickerFrame& c, uint32_t blockPixels = 32); // a oldest
	// A strafe measured as it renders: holds the last three frames only (a 3840x2160 frame is 166 MB here; fourteen would be 2.3 GB)
	// and averages the triples ending at frames firstFrame and later (frames count from 0, the sequence's first Add).
	class FlickerSequence
	{
	public:
		explicit FlickerSequence(uint32_t firstFrame = 3, uint32_t blockPixels = 32);
		void Add(FlickerFrame frame);
		FlickerResult GetResult() const;
	};
}
```

`ReadFlickerFrame` reads `GetHDRTexture()`, `GetDepthTexture()` and `GetNormalTexture()` (`SceneRenderer.h:824-826`) after the frame,
and rebuilds the matrix the frame rendered with as `RenderFrame` does (2963-3004): the camera's relative view, its projection
translated by the jitter in clip units `(2 Jitter.x / W, -2 Jitter.y / H)` from `GetStats().Jitter` (0 without TAA). View depth from
the reversed-Z depth through that projection (the jitter does not change depth), the normal decoded from the octahedral view-space
target and rotated by the inverse view. With `HDROutput` the frame ends before post-processing (`SceneRenderer.cpp:3426-3450`), so
with TAA the HDR target holds the jittered, **unresolved** image: the measure with TAA is the cascades' response to the jitter,
never TAA's history averaging it away (the rule that nothing needs TAA).

For the newest frame c, every pixel i with `2 <= x < W - 2`, `2 <= y < H - 2` and depth: `X` its world position (from its view
depth and c's inverse view-projection, plus c's camera), `N` its normal. For each earlier frame f in {a, b}: project X with f's
view-projection (relative to f's camera) to pixel coordinates `(px, py)`; `px -= 0.5`, `py -= 0.5`; `q = floor`; require `1 <= q.x <
W - 2`, `1 <= q.y < H - 2`; for each of the four texels q + {0,1}^2 require `|depth_f - depth_X_in_f| <= 0.01 depth_X_in_f + 0.01 m`
(`depth_X_in_f = dot(X - camera_f, forward_f)`) and `dot(N_f, N) >= 0.98`, else the pixel is not measured; `value_f` = bilinear of
f's values. Then `d_i = (c_i - 2 b_i + a_i) / max((a_i + b_i + c_i) / 3, 1e-3)`. Statistics over measured pixels: `Mean` of |d|,
`P99` the |d| at index `floor(0.99 (n - 1))` of the sorted values. Blocks: the image tiled in 32x32 blocks from (0, 0) (partial
blocks at the right and bottom left out); a block counts when more than half of its pixels (512 of 32x32) were measured; its value
is `|mean of its d|`; `Blocks` is their mean, `BlocksP99` the 99th percentile as above. A sequence reports the mean of each statistic
over the triples ending at frames `firstFrame .. last` (frame 0 is a cut, frames 1-2 settle the first triple). A steady drift (the
view's real change) scores zero; a value that alternates scores its amplitude. The audit's percentages are these values times 100.
This is the definition `reflections`' `FlickerMeter` uses (`specs/reflections.md` D1: the same `d`, 32x32 blocks with more than 512
valid pixels; its statistics in percent, these in ratio); section 9 says how the two share one statistics function.

**Blocks at 4K.** The gate measures 32 px blocks at 3840x2160 (plan 1.1). The model's 0.45% was at 1080p, where a 32 px block covers
four times the angle (256 cascade-0 probes against 64 at 4K). Every 4K measure therefore also prints (`MESSAGE`, information) the
64 px block values, the 1080p-equivalent, so the gate's number and the model's can be compared; the gate itself is the 32 px value.

**The scenes** (`StrataTests/src/Renderer/GIScenes.h/.cpp`, test helpers; the look-dev project mirrors them, a comment in each names
the other: change both). Units and layout transplant the model's `BuildScene` (`rc_spike.cpp:71-129`): y up, meters, MeshFactory
primitives (`MeshFactory.h:13-21`), materials through `AddMaterial`, roughness 0.8, metallic 0.

- **Yard** (`BuildYard(Scene&, AssetFixture&, const YardOptions&)`, `YardOptions { bool Fence = true; uint32_t GrassCards = 0; float
  LanternPower = 1.0f; }`): ground plane 60 x 60 at y = 0, albedo (0.42, 0.35, 0.27); a hut of plaster (0.70, 0.64, 0.54), walls
  0.2 m thick, 6 x 5 m, 2.8 m high, a door (x 2.5-3.5, up to 2.1 m) in the south wall, a window (z 1.5-3.0, y 1-2) in the west wall, a
  roof (0.25, 0.22, 0.20) with a 0.6 m overhang; two red barrels (0.62, 0.09, 0.07), cylinders r 0.35, h 1, at (1.0, -0.8) and
  (1.8, -0.9); crates (0.30, 0.20, 0.12) (4.1..5.1, 0..1, -1.3..-0.3) and (4.3..5.0, 1..1.7, -1.1..-0.4); a blue covered car (0.18, 0.32,
  0.50) of two boxes (-5..-1, 0.15..1.2, -4..-2.2) and (-4.2..-1.8, 1.2..1.7, -3.9..-2.3); boulders (0.33, 0.30, 0.27), spheres r 2.2 at
  (9, 0.6, -2), r 3.0 at (11, 0.4, 3), r 1.6 at (-8, 0.2, 2); with `Fence`, 34 posts 0.06 x 1.2 x 0.06 m every 0.5 m from x = -8 at
  z = -7..-6.94 and two rails 17 m long at y = 1.0-1.06 and 0.5-0.56; a lantern post (3.8..3.86, 0..1.6, -1.1..-1.04) with an
  emissive sphere r 0.09 at (3.83, 1.68, -1.07), emission (40, 24, 9) times `LanternPower`, and one inside r 0.12 at (5.3, 0.5, 4.3),
  (30, 18, 7) times it (unlit materials in the form of the `Emitting` helper, `SceneRendererTestUtils.h:433`, which takes one
  luminance: here `EmissiveColor` is the color divided by its largest component and `EmissiveIntensity` that component). Sun: a shadowed
  `DirectionalLight`, the sun lying toward normalize(-1, 0.75, 0.28) (the light travels along its negation), illuminance 16, color
  (1, 0.93, 0.82).
  Sky: procedural, zenith (0.30, 0.50, 1.0), horizon (0.95, 1.0, 1.08), ground (0.20, 0.18, 0.15), `SunIntensity` 0, intensity 1.
  `AddNeutralPostProcess` (no AO, no multi-bounce: the measured light is the cascades'), forced pre-exposure 1.
- **Grass** (`YardOptions::GrassCards = 500`): 500 alpha-masked cards (`MeshFactory::CreateQuad`, double-sided, `AlphaCutoff` 0.5),
  each 0.30 m wide and 0.25-0.60 m tall, base on the ground, random yaw, positions x in [-6, 6], z in [-9, -1], `std::mt19937(42)` drawn
  in the model's order (x, z, height, color); the texture a generated 64x128 RGBA8 of 9 vertical blades (alpha 1 inside, 0 outside,
  blades 3 texels wide, tapering), color (0.45, 0.40, 0.22) times 0.7-1.2. The model's grass was thin boxes; cards are the real case
  (Open World's foliage), so the gate's foliage number is this scene's.
- **Room** (`BuildRoom`): the look-dev room (`LookDevScene.json:22-33`): floor, ceiling and walls of plaster (0.58) at x -13.1..-6.9,
  z -5.1..1.1, a 2 x 1.3 m window in the south wall, a box and a ball, the point lamp (intensity 150, range 6, shadowed); sun and atmosphere as
  the look-dev scene (100,000 lux at -40, 30); `WithProbe` adds a `ReflectionProbeComponent` box filling the room, baked in the test
  through `ProbeBake` as `GPU.ReflectionProbes` tests do.
- **Camera paths** (`GIScenes.h`): `YardStrafe(frame, speed)` from (-3.5, 1.7, -9.5) looking at (2.5, 1.0, -0.5), both moving +x by
  0.02 m times `speed` per frame, vertical field of view 55 degrees (the model's "court", which has the fence in the foreground: the
  gate's "fence strafe"); `RoomStrafe(frame)` from (-9, 1.5, -4.5) toward (-10, 1.2, 1.1) moving +x 0.01 m a frame, 70 degrees.

**The GPU tests of stage 0** (`GPU.SceneRenderer.IndirectFlicker`, 1280x720, `ExactSpecification`, `options.HDROutput`,
`PreExposure = 1`, `LightingView = Indirect`, 12 frames each, no AA unless named):

- "The measure runs on the yard's fence strafe": finite statistics, `MeasuredShare > 0.5`, `Frames == 9`; `MESSAGE` prints the five
  numbers (0.7.0's indirect light: GTAO-free here, so mostly the probes'/sky's, near 0).
- "A still camera's indirect light does not flicker": still camera, no AA: `Blocks == 0` and `Mean == 0` exactly (today's indirect
  light is deterministic).
- "The grass and room scenes are measured": both finite, measured share above 0.3 (grass) and 0.5 (room).
- "The fence strafe with TAA is measured": TAA on (`AddTemporalPostProcess(Sharp)`): finite; the jitter enters through each frame's
  view-projection.
- "Today's ambient occlusion on the fence strafe is measured" (information, the real reference the gate is read against): the yard
  with `AmbientOcclusion` on at `AmbientOcclusionLevel::High` (3 x 8, GTAO and its denoise) and multi-bounce off, the `Indirect`
  view, no AA and TAA: finite statistics, `MESSAGE` the numbers. Today's indirect light without AO is deterministic and near 0, so
  GTAO's own flicker in motion is what the owner already sees; the stage-0 commit states it beside the gate's 0.5%.
- Plus `Renderer.FlickerMetrics` (CPU): "A steady drift scores zero" (values `v + 2 t delta` on a plane seen by a strafing camera:
  `Blocks < 1e-6`); "An alternating value scores its amplitude" (1, 1.1, 1: `Mean = 0.2 / 1.0333...` within 1e-6); "Depth and normal
  edges are left out" (a step in depth beside the pixel: not measured); "A block of half its pixels measured is left out"; "The
  reprojection follows the camera" (a textured plane whose value is a function of world position, a camera moving 3 px a frame:
  `Mean < 1e-4`); "A sequence keeps three frames" (`FlickerSequence` over 6 synthetic frames equals the mean of `MeasureFlicker`
  over the four triples ending at 2..5 with `firstFrame` 2, exactly).

**Look-dev** (`Tools/LookDev/LookDevScene.json`, `Shots.json`, `LookDev.py`):

- Scenes `Scenes/Yard.stscene` and `Scenes/Grass.stscene` appended to the script (materials `Materials/Yard*.stmat`, the blade texture
  generated by `LookDev.py` beside the leaf one, `@GRASS_TEXTURE@`). The 500 cards come from a harness macro: a step
  `{"command": "lookdev.grassCards", "parameters": {"count": 500, "seed": 42, "min": [-6, -9], "max": [6, -1], "material":
  "Materials/YardGrass.stmat"}}` that `prepare_project` expands into `entity.create` steps before running the script (Python's
  `random.Random(42)`; the same distribution as the test, not the same numbers: documented). DG "Look-dev harness" documents the macro.
- Shots appended: `lookdev.yard` (Scene Yard, the strafe's first camera), `lookdev.yard.view.indirect`, `lookdev.grass`,
  `lookdev.grass.view.indirect`, `lookdev.room.view.indirect` (the existing room camera). Stage 1 appends the `.view.gi` and
  `.view.open` forms of these three and `tp.scene.view.gi`, `ow.scene.view.gi`, `pp.scene.view.gi`.
- Look-dev pictures are stills for eyes and crops. Flicker numbers come only from the GPU tests: a PNG is tone-mapped and 8-bit and
  a capture has no depth to reproject with.

**The ExileCon 2023 talk** (`https://www.youtube.com/watch?v=TrHHTQqmAaM`) is tried again, published material only: auto-generated
captions or a transcript, slides the speaker published, write-ups that quote it. What it says about the bitmask's mapping to the
hemisphere, the probe placement or a filter goes into the stage-0 commit and corrects the plan's table "How it departs from the
published form" (plan.md 4.4) in this spec's "As built". If nothing readable exists, the commit says so.

### D12 Gate G1 (end of stage 1)

**The three numbers**, each on the GPU at 3840x2160 High, with no AA and with TAA, in a Release build:

| # | Number | Pass | How it is measured |
| --- | --- | --- | --- |
| 1 | Cost: `GPU.RadianceCascades / GPU.AO` in the same frames | at most 1.2 on `tp.scene`, at most 2.5 on `ow.scene` (1.5 ms over 0.7.0's 1.23 and 0.59 ms) | look-dev captures with the `gi` view; per capture the ratio of the two stages of that frame (`renderStats.gpuMs`); the median of 5 captures per shot and mode |
| 2 | Fence: indirect flicker of the yard's fence strafe | `Blocks` at most 0.5%, `BlocksP99` at most 3.5% (the plan's "near 3%"), both modes | `PerfGPU.RadianceCascades`, the `gi` view, 14 frames, triples from frame 3 |
| 3 | Foliage: the grass cards' strafe | `Blocks` at most 1.0%, both modes | the same, the grass scene |

The ratio survives other slots' GPU suites (both stages run in the same frames); milliseconds are read alone at integration (plan.md
7, step 8). The flicker numbers are deterministic (no timing), but they run in the perf suite because they are heavy at 4K and belong
to Release.

**Commands** (the stream's worktree; every run under the lock):

```sh
# 1. Cost (5 captures of tp.scene and ow.scene, no AA and TAA, the gi view)
python Tools/PerfLock.py --label radiance-cascades -- python Tools/LookDev.py capture --build build/agent-radiance-cascades/bin/Release \
    --projects ThirdPerson,OpenWorld --shots "??.scene" --sizes 3840x2160 --aa off,taa --quality high --debug-view gi --repeat 5 \
    --output .worktrees/radiance-cascades-gate/cost
python Tools/LookDev.py ratio .worktrees/radiance-cascades-gate/cost --numerator GPU.RadianceCascades --denominator GPU.AO
# 2 and 3. Flicker
python Tools/PerfLock.py --label radiance-cascades -- build/agent-radiance-cascades/bin/Release/StrataTests.exe --test-suite=PerfGPU.RadianceCascades
```

`LookDev.py ratio <capture dir> --numerator <stage> --denominator <stage>` (new, stage 1): for every `stats/<shot>-<size>-<mode>.json`,
the ratio of the two stages in each repeat's `renderStats.gpuMs` (as `write_stages` reads them, 590-617; the path `repeats[*].
renderStats.gpuMs` checked on the audit's `RC/ld-samples-high-4k/stats/tp.scene-3840x2160-taa.json`), and per shot and mode the
median, the minimum and the maximum, printed as a table and written to `ratio.csv`; repeats missing either stage are reported and left
out. `StrataTools.LookDev`'s selftest checks it on a synthetic stats directory.

`PerfGPU.RadianceCascades` (`StrataTests/src/Perf/RadianceCascadesPerfTests.cpp`, Release, label `perf`): "Gate G1: the fence strafe at
3840x2160" and "Gate G1: the grass strafe at 3840x2160" render 14 frames of the strafe (`YardStrafe(frame, 1)`) with the `gi` view
through `HDROutput`, forced pre-exposure 1, no AA then TAA (a fresh renderer each), read every frame back into a `FlickerSequence`
(`ReadFlickerFrame`; with TAA the unresolved, jittered image, D11), and `Perf::Record` (`PerfUtils.h:150`) the metrics below (unit
`ratio`: the flicker is a relative change, a ratio of two measurements per pixel), with the 64 px block values as `MESSAGE`s; "Gate
G1: the yard's stage at 3840x2160" turns the yard's `AmbientOcclusion` on (High's 3 x 8; `AddNeutralPostProcess` turns it off, and
without `GPU.AO` there is no ratio), renders 30 still frames with the `gi` view and records the medians of `GPU.RadianceCascades` and
`GPU.AO` over frames 10-29 (`ResolveGPUStats(true)`, `MedianGPUStageMs`'s pattern, `RendererPerfTests.cpp:64-83`) and their ratio
(information).

**The decision.** The stage-1 commit that completes the views states the three numbers in both modes, the 720p regression numbers and
the stage's milliseconds alone (if the machine was quiet) and in ratio. Then:

- All pass: stage 2 may start in tranche 2.
- The fence misses: one extra day of tuning (D12.1), then the degrade path in order (D12.2-D12.4), each step measured with the same
  commands and recorded.
- **What ships** (plan.md 4.4): full cascades on solid geometry; the fence row decides the join. A foliage miss alone does not stop
  it: GI is off unless a scene asks, the showcase has no grass, and a scene with dense foliage takes `GlobalIlluminationOcclusion` 0.
  The join stops only if the fence misses in the bounce-only form too (and in D12.3's and D12.4's).

**D12.1 The tuning day** (in this order, each measured on the 720p regression case first, then the gate's 4K): thickness
(`c_GIThicknessMin` 0.10-0.25 m, scale 0.02-0.04), the first interval (`24 * H / 1080` to `32 * H / 1080`), the edge fade (0.5-2
intervals), `c_GIColorLevelBias` (0, 1, 2: the model found 2-3 helped emitters and hurt the fence, `sweep3.log`, `sweep8.log`), the
filter's plane tolerance (1.5 x), `c_GIColorFillLevels` (0, 1, 2, 3; 0 is the unfilled coupling of cover to validity) and
`c_GIPlaneSlack` (0.5, 1, 2; below 1 the flat-ground case of 6.2 must still pass). Every change keeps D6's form; the winning
constants are committed with their numbers.

**D12.2 More filter**: `FilterPasses = 5` (stride 16), then 6 (32).

**D12.3 Bounce only, then upper cascades off**: `ViewOcclusion = 0` (the gi view becomes hit light plus the whole fallback: the open
share, which carries most flicker, stops scaling it; GTAO occludes in the joined image), measured; then `MaxCascades = 4`, 3, 2 with
occlusion 1 and then 0 (rays end at `I0 (2^N - 1)`: 360, 168, 72 px at 1080p; beyond counts as open; the model: two cascades 0.35%,
`sweep3.log`). Each of these is a setting the joined renderer can ship with (stage 2 maps the chosen one to the property's default or
the layout).

**D12.4 D7's filter, the last resort** (decisions.md row 2): only if the fence misses in every form above. A one-pole filter on the GI
buffer alone, in probe space: the filtered cascade-0 probes of the previous frame (`Filtered` kept, plus the previous `Probes[0]`,
about 13 MiB at 4K High), each current probe reprojected with `PreviousViewProjection` to the previous grid, bilinear over the
previous probes whose record depth is within `0.01 z + 0.02 m` of the expected and whose normals agree (`dot >= 0.98`), blended
`result = mix(current, previous, History)` with `History = 0.5` where all four agree, else no blend. It then passes the same numeric
gate (the model: 0.20% blocks at 0.5, `sweep9.log`) **and the owner's live look in motion**; AGENTS.md's Lighting rule ("needs no
history") changes only if this form is adopted. If it fails or the owner rejects it: the stream stops at its debug views and the
irradiance volume moves above the cut.

### D13 Stage 2: the join

**The scene's ask.** `PostProcessComponent` (after the AO fields, `RendererComponents.h:283-288`):

```cpp
// Global illumination (radiance cascades): light bounced from what the screen shows, and the share of each surface's hemisphere it
// covers darkening the probes' and the sky's light; the quality's GlobalIllumination row scales it (off at Low and Medium).
bool GlobalIllumination = false;
float GlobalIlluminationIntensity = 1.0f; // The bounce light's scale, 0 to 4; 1 as measured
float GlobalIlluminationOcclusion = 1.0f; // The open share's weight on the probes' and sky's light: 1 fully, 0 bounce only (foliage)
```

Properties in a new section "Global illumination" after "Ambient occlusion" (`RendererComponents.cpp:451-460`):
`GlobalIllumination` ("Light bounced from what the screen shows (radiance cascades): sunlit walls light the ground beside them, colored
surfaces tint their neighbors, rooms darken where the screen shows their walls. What is off screen comes from the reflection probes and
the sky as before. Graphics Quality High and Ultra; off at Low and Medium"), `GlobalIlluminationIntensity` (`Range(0, 4, 0.01)`, "How
much bounce light the cascades add: 1 as measured, 0 none"), `GlobalIlluminationOcclusion` (`Slider(0, 1)`, "How much the share of the
view the screen covers darkens the probes' and the sky's light: 1 fully; 0 bounce only, the ambient occlusion alone darkening (scenes
with dense foliage, whose thin leaves flicker in it)"). Inactive reasons (589-610 pattern): the two tunables `&WithGlobalIllumination`
("Global Illumination is off"). `ComponentSchema.json` regenerated.

**The row.** `RendererQuality.h`:

```cpp
// Global illumination's layout (radiance cascades; PostProcessComponent::GlobalIllumination asks for it): Off, or the cascades with
// cascade 0's probes every 4 (Low), 2 (High) pixels per 1080 rows (RadianceCascades::GetProbeSpacing). The values never change
// meaning (the section stores their names); Ultra (1.5 pixels per 1080 rows) takes 3 when it lands (D15.1).
enum class GlobalIlluminationLevel : uint8_t
{
	Off = 0,
	Low = 1,
	High = 2
};
```

(Low and High land in one commit, so they take the order of the other level enums, `AmbientOcclusionLevel`'s: `RendererQuality.h:74-80`.)

- **The field** `GlobalIlluminationLevel GlobalIllumination = GlobalIlluminationLevel::Off;` (Medium's) goes after `MotionBlurQuality`
  (`RendererQuality.h:148`) and before the players' rows (149-156: they stay last), with a comment naming the memory at 1080p
  and 4K and that it scales what a scene asks for. Its traits line goes at the same position in `c_RowTraits` (after
  `MotionBlurQuality`'s, `RendererQuality.cpp:134`: "field order", 121): `{ "GlobalIllumination", true, "GPU.RadianceCascades",
  &GlobalIlluminationBytes }`, where `GlobalIlluminationBytes(settings, output)` returns, for Low and High,
  `ComputeLayout(output, GetProbeSpacing(level), 0, c_GIUnboundedTextureDimension)->GetMemoryBytes()` (which counts the color copy
  when nothing else made it: D2's table), and 0 for Off. `reflections`, `local-lights` and `volumetric` add their own rows at the same
  boundary: one line each, in merge order (section 9).
- `c_GlobalIlluminationRow = { Off, Off, High, High }` (Ultra's column becomes `Ultra` when D15.1 lands); a line in `GetPreset`,
  `Clamp` (unknown values to Medium's), `MakeSection` (Enum "GlobalIllumination", title "Global illumination", options "Off", "Low",
  "High", default High's value, description naming `GPU.RadianceCascades` and the memory at 1920x1080 and 3840x2160),
  `FromSectionValue`, `ToSectionValue`. `DescribeRenderStats`' quality object comes from `ToSectionValue`
  (`EditorStateCommands.cpp:230`): no extra key. `c_SectionVersion` + 1 at landing 2 (the integrator renumbers). `ForThumbnails` and
  `ForBake` take Medium's rows: GI off with no change there.
- **High differs from Medium at every size.** DG "Quality settings" (3539-3542) lists the rows in which High is not Medium below 1440
  rows, and `Renderer.Quality`'s "High is Medium below 1440 rows but for the rows that differ at every size"
  (`RendererQualityTests.cpp:222-253`) checks it: `GlobalIllumination` joins both (the test sets `resolved.GlobalIllumination =
  medium.GlobalIllumination` after checking it is High), or the existing case fails.
- **`SetQuality`** (`SceneRenderer.cpp:1856-1907`): a changed `GlobalIllumination` releases the cascades' resources at once
  (`ReleaseRadianceCascades(released)`, beside `ReleaseScreenReflections` at 1885, through `Renderer::ReleaseDeferred`), sets
  `m_GIColorRender = 0` and returns that the image changed (the temporal history restarts, as for reflections); the next frame that
  wants GI makes the new layout. So the row's memory follows `SetQuality` the frame it is called, as the motion blur's rows do.
- `RadianceCascades::GetProbeSpacing(GlobalIlluminationLevel)` maps Low 4, High 2 (Ultra 1.5) pixels at 1080 rows into D2's rule
  (`pixelsAt1080` of `ComputeLayout`; Off has none). **Low is above the cut** (section 12): the feature test needs a running value
  other than the default.

**Gating, stage 2 form:**

```cpp
const GlobalIlluminationLevel giLevel = m_Specification.Quality.GlobalIllumination;
const bool giAsked = postProcess.GlobalIllumination && giLevel != GlobalIlluminationLevel::Off;
const bool giWanted = (giAsked || giViewShown) && !camera.Orthographic;
// The views at a quality without GI show what High would give.
const GlobalIlluminationLevel layoutLevel = giLevel != GlobalIlluminationLevel::Off ? giLevel : GlobalIlluminationLevel::High;
frame.ReflectionParams.y = m_GIFrame && giAsked ? 1.0f : 0.0f; // a view alone never changes the lit image
frame.ReflectionParams.z = Clamp(postProcess.GlobalIlluminationIntensity, 0, 4);
frame.AOParams.z = Clamp(postProcess.GlobalIlluminationOcclusion, 0, 1);
```

Both vectors are assigned whole today (`ReflectionParams` at 3019, `AOParams` at 3140): those two constructions gain the components
(non-finite values read as the defaults, as `RenderAmbientOcclusion` does at 121-122), never a second assignment that a later merge
could reorder.

`RadianceCascadeSettings::ViewOcclusion` is removed; the view pass takes `Intensity` and `Occlusion` from the Post Process.

**The diffuse gather, in S0.11's terms** (step0.md 8.2-8.3; opaque variants only: everything below sits under `#ifndef
ST_FORWARD_TRANSPARENT`, which the glass variants define too, `Strata/CMakeLists.txt:156-160`). S0.11's `ComposeIndirect` multiplies
the sky form's sum, diffuse gather included, by `EnvironmentParams.x` (step0.md 8.2, 933-934), and `ComposeIndirect` is S0.11's
(8.7). Hit light is absolute luminance that no sky intensity scales, so it cannot go inside `GatherIndirectDiffuse`; it is a function
of its own, added after the compose step. Three places change:

1. **`ShadeLitSurface`, where occlusion is formed** (`Forward.frag:214-226` today; S0.11 keeps these lines and passes
   `diffuseOcclusion` and `diffuseOcclusionColor` to `GetIndirectInputs`). After the `ao` view's early return (so the `ao` view keeps
   showing material times screen occlusion; the `open` view shows the cascades'):

   ```glsl
   vec4 globalIllumination = vec4(0.0);                // rgb hit light (pre-exposed), a the closed share; SRV 35 (Include/Scene)
   bool giOn = false;
   #ifndef ST_FORWARD_TRANSPARENT
   if (u_Frame.ReflectionParams.y > 0.0)
   {
   	globalIllumination = texelFetch(u_GlobalIllumination, ivec2(gl_FragCoord.xy), 0);
   	giOn = true;
   }
   #endif
   // The probes' and the sky's light reaches a surface through what the screen leaves open (AOParams.z: how much the open share
   // counts) and what screen-space occlusion leaves, whichever is less; written so that a closed share of exactly 0 gives exactly
   // diffuseOcclusion (min(x, 1) is x for occlusion in [0, 1]).
   float fallbackOcclusion = giOn ? surface.Occlusion * min(screenOcclusion, 1.0 - u_Frame.AOParams.z * globalIllumination.a) : diffuseOcclusion;
   vec3 diffuseOcclusionColor = giOn ? vec3(fallbackOcclusion)  // real bounce replaces the multi-bounce fit
   	: (u_Frame.AOParams.y > 0.0 ? AOMultiBounce(diffuseOcclusion, surface.Albedo * (1.0 - surface.Metallic)) : vec3(diffuseOcclusion));
   ```

   `GetIndirectInputs` is called as S0.11 calls it, with this `diffuseOcclusionColor`; its `diffuseOcclusion` argument (which forms
   `SpecularOcclusion`) stays `diffuseOcclusion` until the specular line below.
2. **`IndirectInputs` gains, appended** (S0.11: shared and additive): `bool GI; vec3 GIHit; float GIOpen;`. `GetIndirectInputs`
   initializes them to `false`, `vec3(0.0)`, `1.0` (its signature does not change); `ShadeLitSurface` sets them right after the call:
   `GI = giOn`, `GIHit = globalIllumination.rgb / u_Frame.TemporalParams.y * u_Frame.ReflectionParams.z`, `GIOpen = 1.0 -
   globalIllumination.a`.
3. **`GatherIndirectBounce`** (new, `Include/IndirectLight`, owned here), and S0.11's compose line in `ShadeLitSurface` becomes
   `vec3 indirect = ComposeIndirect(indirectInputs, GatherIndirectDiffuse(indirectInputs, surface), GatherIndirectSpecular(indirectInputs))
   + GatherIndirectBounce(indirectInputs, surface);` (before the clear coat, which then covers it as it covers the rest):

   ```glsl
   // The light the radiance cascades found on screen (absolute luminance: no form's sky intensity scales it), diffuse only, by
   // material occlusion; diffuse gets what specular leaves where image light has a specular part (every form but the ambient).
   vec3 GatherIndirectBounce(IndirectInputs inputs, SurfaceData surface)
   {
   	if (!inputs.GI)
   		return vec3(0.0);
   	vec3 diffuseShare = inputs.Form == ST_INDIRECT_AMBIENT ? vec3(1.0) : 1.0 - inputs.SpecularColor;
   	return ScatterDiffuse(inputs.GIHit * surface.Albedo * (1.0 - surface.Metallic) * diffuseShare, inputs.DiffuseWeight) * surface.Occlusion;
   }
   ```

`GatherIndirectDiffuse` itself is unchanged: the fallback's forms keep their expressions and scales, and only the occlusion they take
changes. Without GI, S0.11's code exactly. With nothing on screen (closed share exactly 0, hit 0: D8) the result equals the frame
without GI but with multi-bounce off, bit for bit: the regression test (6.3, "Nothing on screen equals off").

**Specular occlusion** (the specular gather is `reflections`'; this stream changes one argument after `reflections` merged, step0.md
1237-1238 and `specs/reflections.md` D5): the call passes `fallbackOcclusion` instead of `diffuseOcclusion` as `GetIndirectInputs`'
`diffuseOcclusion`, so Lagarde's visibility takes the open share; after `reflections`' D5 it reaches the probes' and the sky's
specular light only, never a screen-space reflection. The clear coat's occlusion (`Forward.frag:310`) keeps the local
`diffuseOcclusion` (S0.11 8.3), stated in DG.

**`GetDiffuseFallback` against the forward pass.** The view pass calls `GetDiffuseFallback`; the forward pass keeps S0.11's forms. 6.3
checks the two on flat open ground (open share 1, no hits): under a constant ambient (no environment, no probes) the `gi` view equals
the `Indirect` view of the same ground made white (albedo 1, roughness 1, metallic 0) within 0.5%; under the sky within 6% (the
`Indirect` view keeps the split sum's specular share and `1 - specularColor`).

**Settling for pictures.** `ProjectThumbnail` (162-183) waits like SSR's pending (176-180): `if (stats.GlobalIllumination &&
!stats.GlobalIlluminationSettled && m_GIFrames < c_MaxGIFrames) { m_GIFrames++; return; }` with `c_MaxGIFrames = 12` (`.h:36-42`
pattern). Look-dev A/Bs go through `viewport.capture`, which waits since stage 1. On a cut the first frame shows the fallback and bounce
rises over the next K frames (`c_GISettleFrames` as measured); the showcase tour's fades cover it, a game's cut shows it (DG says so).

**Flicker with feedback.** From stage 2 the lit image holds the previous frame's GI, so the light converges over K frames after a cut,
and a geometric approach has a second difference (it is not a steady drift). Every flicker measure of the joined image (6.3, the
stage-2 and stage-3 re-runs of the gate's commands) starts its triples at `firstFrame = c_GISettleFrames + 2` and renders that many
more frames (14 + `c_GISettleFrames` - 1), so it measures motion, not convergence.

**The AGENTS.md lighting check** ("check lighting changes with the `lighting`, `ao`, `indirect`, `luminance` and `albedo` debug views
and the white-furnace test"): stage 2's commit looks at the five views of `lookdev.room`, `tp.scene` and the yard with GI asked
against not asked, and runs the GPU furnace case (GI does not change it: a sphere in a uniform sky has nothing on screen to hit).

**Feature test.** `Feature.stscene`'s first Post Process: `GlobalIllumination: true`, `GlobalIlluminationIntensity: 1.25`,
`GlobalIlluminationOcclusion: 0.5`; `FeatureTest.stproj`'s `Renderer.Quality` (Custom, 150-172): `"GlobalIllumination": "Low"` (a cost
knob off its default that runs, DG "Feature test"); `GPU.FeatureTest` then renders with GI: its stats check `GlobalIllumination` true
from the second frame.

**Perf** (`RendererPerfTests.cpp`, AO's pattern, 853-901): `PerfGPU.Renderer.GI4K` (CPU frame), `GI4KGPU` (busy), `GI4KStageGPU`
(`GPU.RadianceCascades` median, Target 1.5): the 100k cube grid at 3840x2160, High, TAA, a shadowed sun, the Post Process asking for GI.

### D14 Stage 3: hardening

- Tune on the samples' scene cameras and the showcase's yard (once landed on main): thickness, `c_GIRadianceCap`, the edge fade, the
  filter's tolerances; each change measured on the fence and grass cases (no regression above the gate) and judged in the look-dev
  `.view.gi` shots and the lit shots side by side with GI off.
- Lanterns and fire stay punctual lights; their emissive meshes are under 19 px at 1080p and are not resolved (they add faint fans at
  most, capped). Check shot 6's flicker (dusk) with the gate's commands on a showcase camera if the sample has landed.
- **Exit: the owner's live look in motion** on Third-Person and the showcase, GI toggled through the Post Process in the rebuilt
  editor (plan.md 7, step 6), **with no AA, FXAA and TAA** (plan 4.1: every screen-space effect is judged in the three; TAA's history
  hides flicker, so a look under TAA alone proves nothing). A sheet of pictures cannot show flicker, which is what the owner rejected in
  contact shadows. Before it, our own look in motion in the three modes (decisions.md row 7 needs it if the owner has not looked).
- Checks named by section 10's risks: walking into the hut under automatic exposure (the cap steps with the pre-exposure), and
  `ow.scene` and a scene with dense Height Fog (the copy holds fog and aerial perspective: distant hits re-emit haze). If either shows,
  the remedy is a stage-3 change measured on the gate's cases: the cap from the metered exposure rather than the pre-exposure, and
  hits faded by their distance from the probe beyond about 30 m.
- The docs land here (section 8).

### D15 Below the cut

Each is specified to be built in tranche 2 only when everything above landed and the usage rule allows.

**D15.1 The Ultra layout.** `GlobalIlluminationLevel::Ultra = 3`, `PixelsAt1080 = 1.5` (3 px at 2160 rows: 14.64 M texels, about
291 MiB at 4K; 2 px at 1440 rows, 295 MiB; at 1080 rows the same as High), the row's Ultra column; measured with the gate's cost
command (target: at most 2.0 x `GPU.AO` on `tp.scene`).

**D15.2 The bent direction.** The gather also sums `w_j * dir_j` over visible, uncovered sectors (dir in view space), normalizes it
(the normal when empty) and stores it octahedral in a second probe texture (R32_UINT, the two components packed with `packSnorm2x16`:
RG16F storage needs the extended formats, `GraphicsDevice.h:39-40`, which MoltenVK may lack; filtered with the same weights,
renormalized); the upsample writes a second full-resolution R32_UINT target the same way (4 bytes a pixel: 31.6 MiB at 3840x2160,
7.9 at 1920x1080); the forward pass evaluates `GetDiffuseFallback` at the bent
direction instead of the normal (one extra probe/irradiance evaluation is avoided: the same call with another direction). Scene SRV:
the next free slot at that time (37 if fog volumes took 36).

**D15.3 AO at 2x6 with cascades.** When the frame runs GI at High, `RenderAmbientOcclusion` uses Medium's samples (2x6) instead of
3x8. Adopted only if the A/B of `GPU.AO` saves at least 0.3 ms at 4K on `tp.scene` and the `ao` view shows no loss of contact detail on
`tp.view.ao` and `tp.scene.view.ao`.

**D15.4 Time keys of the irradiance volume** (with D15.5): 1 to 4 keys per volume, each baked with its sun.

**D15.5 The irradiance volume** (wave 7's D10, `Docs/Plans/Wave7/specs/light-and-materials.md:1088-1124`, kept except where stated):

- `IrradianceVolumeComponent` (renderer module): `AssetHandle Data`, `glm::vec3 Extents = {16, 8, 16}` (half extents along the entity's
  axes), `float Spacing = 2.0f` (0.5-8 m; rooms 1, yards 4), `float Intensity = 1`, `float NormalBias = 0.3f`, `float ViewBias = 0.2f`,
  `float BlendDistance = 2.0f`, `int32_t Priority = 0`; at most 128 x 64 x 128 probes; on an entity of a scene or a world cell.
- `AssetType::IrradianceVolume`, `.stvolume`: magic "STIV", format version 1, counts (x, y, z) u32, spacing f32, key count u32 (1-4),
  reserved u32 x 2 (32-byte header); a key table of 16 bytes per key (the sun's direction toward it, xyz f32; its elevation in
  degrees f32); validity bytes, one per probe (share of front faces seen, 0-255), padded to 4; then per key, per probe, 12 half floats
  (L1 of RGB irradiance over pi: L0 and three L1 per channel). Size = 32 + 16 keys + align4(n) + 24 n keys. Importer validates counts
  and size; the loader rejects truncated and garbage bytes; an `Asset.IrradianceVolume.Sweep` suite. References recorded
  (`AssetDependencies::Scan` through the property).
- `lighting.bakeVolume {entities?, times?: [{"elevation": deg, "azimuth": deg}], force?}` (Edit, deferred `MayDefer`, refused while
  playing, one undo step, `Assets/Lighting/<scene>/<entity id>.stvolume` through staging and `ImportExternal` as `lighting.bakeProbes`):
  per key the directional light of a working copy of the scene is set to the key (never the document); per probe six 16x16 `HDROutput`
  faces with `ProbeBake`'s renderer (`ForBake()`, GI off), back faces writing alpha 0 (`MaterialParams.w` in bake frames), projected on
  the GPU (`SH/IBL/VolumeProjection.comp`) into L1 and validity, read back with `BufferReadback`, 16 probes per frame (progress in
  `editor.status`); probes with validity below 0.5 take the mean of valid neighbors (up to 8 passes); a second run sees the first
  (second bounce). Without `times`, one key with the scene's sun.
- GPU: SRV 30 a 3D atlas RGBA16F, three planes per volume holding the **current** blend of its two keys (plane 0 (L0.rgb, L1x.r),
  plane 1 (L1y.r, L1z.r, L1x.g, L1y.g), plane 2 (L1z.g, L1x.b, L1y.b, L1z.b)), shelf-allocated with a one-texel gutter; the keys
  themselves in a structured buffer; `SH/GI/VolumeBlend.comp` rewrites a volume's planes when the sun's elevation moved by more than
  0.5 degrees since its last blend, one volume per frame (the keys bracketing the elevation, blended linearly in elevation, clamped
  outside). SRV 31 the records (up to 16: camera-relative world-to-grid rows, atlas offset, counts, intensity, blend distance, biases,
  priority) with a count header (no frame constant).
- Shading: the two strongest volumes whose boxes hold `position + N NormalBias + V ViewBias` blend as probes do; three trilinear
  fetches per volume; `E(n) = pi (L0 Y00 c0 + (2/3) sum L1m Y1m(n))`. It replaces the probes' and the sky's diffuse light by its
  weight inside `GetDiffuseFallback`, so with cascades it is simply the fallback (hits replace it direction by direction: a sun patch on
  screen and its baked bounce off screen stay consistent). Specular image light takes D10's ratio clamp ([0, 1] for the sky, [0.25,
  4] for probes).
- Never updated at run time this wave. Tests as D10's plus "Keys blend by the sun's elevation" and "A volume baked at noon darkens the
  hut at dusk with its dusk key".

## 6. Tests per stage

Every GPU case ends with `CHECK(gpu.GetNewErrorCount() == 0)`; regression tests are checked to fail without their change (named
below where it is not obvious). Suites: `Renderer.*` CPU suites run in the Core shard `Renderer` (`CoreShards.json`), `GPU*` in
`StrataTests.GPU`, `PerfGPU.*` in `StrataTests.PerfGPU`: no shard change.

### 6.1 Stage 0

`Renderer.FlickerMetrics` and `GPU.SceneRenderer.IndirectFlicker` as in D11. `StrataTools.LookDev` passes with the new scenes (the
selftest builds the look-dev project).

### 6.2 Stage 1

`Renderer.RadianceCascades` (CPU, `RadianceCascadeLayout`):

- "The layouts at 1280x720, 1920x1080, 2560x1440 and 3840x2160 High and at 1920x1080 and 3840x2160 Low": every field of D2's table
  exactly (spacing, interval, cascades, grids of every cascade, r, pyramid size and levels, atlas sizes, texels).
- "Intervals double and the last covers the diagonal": for every table entry, the last interval's end at least the diagonal and the one
  before below it; `MaxCascades` 3 gives 3 cascades.
- "Grids nest": `G_{i+1} == ceil(G_i / 2)` for every cascade of every entry and odd sizes (1x1 fails, 17x9, 1366x768, 3440x1440).
- "Tiles cover their wedges": `Tx_i * Ty_i == W_i` and the atlas holds every cascade of its parity.
- "Memory adds up": `GetMemoryBytes` equals D2's table to the byte for 1920x1080 and 3840x2160 High.
- "A layout too wide for the device is refused": `maxTextureSize` 2048 at 3840x2160 returns none.

`GPU.SceneRenderer.RadianceCascades` (1280x720 unless named, `ExactSpecification`, the `gi` view through `HDROutput` and forced
pre-exposure 1 unless named):

- "Without a gi or open view nothing is made": a renderer that never showed them: `GetRadianceCascadeMemoryBytes() == 0`,
  `GetGlobalIlluminationTexture() == nullptr` (SRV 35 then binds `Renderer::GetBlackTexture()`, which the rebind check compares), no
  `GPU.RadianceCascades` stage in its GPU stats, no motion frame forced (with a neutral Post Process and no motion options,
  `GetVelocityTexture()` stays null) and no color copy made (`SceneColorCopies == 0`).
- "Memory matches the layout": after a gi frame, `GetRadianceCascadeMemoryBytes()` (the renderer's sum over the textures it made,
  from their descriptions) equals `GetMemoryBytes()` minus the copy's bytes.
- "Flat ground under an open sky is all open": the yard's ground alone, at 1280x720 (r = 1) and at 2560x1440 (r = 2, the nearest of
  2 x 2 pixels), each from two cameras 1.7 m above it: the strafe's (pitched about 4 degrees down) and one pitched 45 degrees down
  (where the probe's median offset is largest, D6): every ground pixel of SRV 35 is `(0, 0, 0, 0)` exactly (read back through
  `GetGlobalIlluminationTexture()`), and, rendered once more without `HDROutput` (which measures nothing), the `open` view's
  `ValueMedian` is 1. Fails without the self-plane rejection, and with `c_GIPlaneSlack` 0 (checked once and reported in the commit).
- "A box over the floor covers it": a 1 m cube 0.5 m above a floor: the open share under its center at most 0.6, 3 m away at least 0.95.
- "An emissive wall lights the floor within 15% of its form factor": a 2 x 2 m unlit wall of luminance 4 (below the cap's knee of 8
  at a forced pre-exposure of 1) standing on a gray (0.5) floor in the dark (no sun, black sky): after 3 frames, the hit light at floor
  points 0.5, 1 and 2 m from the wall's foot (on its axis) within 15% of `L * F` with F the analytic form factor of a rectangle to a
  point (`GIScenes.cpp` implements it), read from SRV 35 divided by the pre-exposure.
- "Hits outside the screen take the fallback": the same wall and floor, two cameras at the same height and distance looking along the
  wall's axis: one sees the whole wall, the other is turned so that the wall's left half lies beyond the screen's left edge. At the
  floor point 1 m in front of the wall's right quarter-line (on screen in both views, at least `I0` pixels inside the edge), the
  turned view's hit light is at most 0.85 of the whole view's and its open share larger by at least 0.02. Fails if samples beyond the
  screen are clamped to its edge instead of ending the ray (D6's `break`).
- "The first frame and a cut show the fallback": frame 1: `GlobalIlluminationPending` true, SRV 35 all zeros; frame 2: hit light on the
  floor; a camera cut then: pending again. Fails if the source validity is not gated.
- "The radiance cap compresses a bright emitter": the wall at luminance 4 and at 400: the hit light ratio below 5 (uncapped: 100).
- "Settling is reported after c_GISettleFrames frames": the yard, still camera: `GlobalIlluminationPending` on frame 1 only,
  `GlobalIlluminationSettled` false until exactly `c_GISettleFrames` frames with a valid source, true from then on; in stage 1 the lit
  image holds no GI, so the `gi` view is identical from the second valid frame on (checked). The value of `c_GISettleFrames` is
  measured with feedback in stage 2 (6.3) and lowered there if the light settles sooner.
- "Two still frames are identical without TAA": the yard, frames 8 and 9 of a still camera, the `gi` view's HDR target bit for bit.
- "Paths are equal": `GPU.SceneRenderer.GPUDriven` gains "Radiance cascades' gi view": the CPU and GPU paths with occlusion culling on,
  the yard, the `gi` view's HDR target identical after 4 frames.
- "Probe depths follow their blocks": a scene of three planes facing the camera at 2, 5 and 20 m covering thirds of the view, and a
  fourth tilted 30 degrees across the bottom: cascade 4's records read back (a test accessor `ReadProbeRecords(cascade)`, the
  renderer's `Probes[cascade]` through `Renderer::ReadTexture`) compared on the CPU with each block's exact pixel depths (read from the
  depth target): a block within one facing plane has that plane's depth (relative 1e-4); a block on the tilted plane is within 2% of
  the exact upper median; a block straddling planes has a depth that one of its pixels has (relative 1e-4: a median of medians picks a
  real depth, never a mean of two layers) and coverage equal to the block's surface share exactly.
- "Orthographic cameras run no cascades": `GlobalIllumination` false, the `gi` view shows the fallback (`open` view median 1).
- "Resizing remakes the layout": 1280x720 then 1920x1080: the layout changes, the memory follows, no validation error.
- "A thin disocclusion keeps its cover": the yard, a 6 cm post 1 m in front of the hut's wall moving 2 cm a frame, still camera:
  over the floor pixels within 1 m of the wall's foot, the closed share of frame 6 against a second renderer that drew frame 6's scene
  for 3 frames (no disocclusion) differs by at most 0.02 at the 99th percentile; with `c_GIColorFillLevels` 0 the commit states the
  same number (information: the fill's effect, D6).
- "The fence strafe's flicker stays within its bound" (the regression case of the gate): 1280x720, 14 frames, no AA: `Blocks <= 0.8%`,
  `BlocksP99 <= 5%`; with TAA the same bounds. The commit that lands stage 1 replaces 0.8% and 5% by 1.5 times the measured values
  when those are lower (never higher), stating them. Fails with `FilterPasses = 0` (the check is done once and reported).
- "The gi and open views report their statistics": `LightingViewStats` for both views arrive; `gi` has `SurfaceEV100` and a hit-share
  median in (0, 1) on the yard; `open` a value median in (0, 1).

`GPU.SceneRenderer.LightingViews` (existing, 97-): "Without a lighting view nothing is made and the image is unchanged" keeps
passing; a new case "The gi and open views leave the forward pass alone": a still yard with Spatial reflections asked
(`ScreenSpaceReflections` on, quality `Reflections = Spatial`, so that both renderers copy the scene color every frame: without a GI
view nothing else would make the copy, `SceneRenderer.cpp:3393`) rendered without a view and with the `gi` view gives the same
scene-color copy after frame 4 (level 0 of `GetSceneColorTexture()`, `SceneRenderer.h:885`, read back: the copy holds the lit image
the next frame's cascades read, not the view). Fails if the view is shaded by the forward pass. (That accessor's comment, "Null
until a frame draws a transmissive surface", gains "or asks for spatial reflections or radiance cascades".)

Editor (`GPU.Editor.Viewport`): "A capture of the gi view waits for its light and reports the cascades": `viewport.capture {debugView:
"gi"}` on a small yard: `globalIlluminationSettled` true, `debugView.globalIllumination.settledFrames >= c_GISettleFrames`,
`cascades` equal to `ComputeLayout` for the capture's size (C is 7 only for 16:9 outputs of 720 rows and more: 5 at 320x180),
`memoryMiB` > 0. `Editor.Commands`: the schema lists `gi` and `open`. `EditorProfilerTests.cpp:91` and `FramePerfLogTests.cpp:284`
list `GPU.RadianceCascades` after `GPU.AO`. `EditorMcpToolsets` (its core size check, `EditorMcpToolsetsTests.cpp:69`) passes.

`PerfGPU.RadianceCascades` (7.1): the gate's cases.

### 6.3 Stage 2

`GPU.SceneRenderer.RadianceCascades` gains:

- "Nothing on screen equals off, pixel for pixel": the yard's ground alone, `AddNeutralPostProcess`, GI asked at High against not asked:
  the 8-bit output identical (multi-bounce is off in the neutral Post Process). Fails if the closed share is not exactly 0.
- "Off costs nothing": GI asked with the row Off, and not asked at High: no GI memory, no stage, no velocity target and no color copy
  made for it.
- "The row follows SetQuality": Off, High, Low, Off: memory 0, High's, Low's (about a quarter of High's cascades), 0.
- "Bounce colors the floor": a red wall (0.62, 0.09, 0.07) beside a white floor in the sun: the floor's indirect light within 1 m of
  the wall (the `Indirect` view through `HDROutput`, after settling) has R/G at least 1.15, with GI off below 1.05.
- "The views' fallback is the forward pass's": D13's `GetDiffuseFallback` check (open ground: constant ambient within 0.5%, sky within
  6%).
- "Specular occlusion takes the open share" (after `reflections` merged): a smooth (roughness 0.2) floor under the box of "A box over
  the floor covers it", probes and sky only (no SSR): its specular indirect light under the box's center with GI is at most 0.7 of
  the same without GI; with `GlobalIlluminationOcclusion` 0 equal to without (within 1%).
- "A closed room with a probe reads darker than without": the room with GI, mean luminance of its floor and walls with the box probe at
  most 0.6 of the same without it (the fallback decides the room: the audit's 3.7 times).
- "Intensity scales only the hit light": intensity 0 equals bounce-free shading with the open share; 2 doubles the hit term (within 1%).
- "Occlusion 0 leaves the fallback unscaled": `GlobalIlluminationOcclusion` 0 gives the fallback times screen and material AO only.
- "Transparent surfaces keep the fallback": a glass pane and a blended quad: their pixels equal GI off.
- "Bounce settles within c_GISettleFrames frames" (with feedback now): the yard and the room, a still camera after a cut, the `gi`
  view through `HDROutput`; K is the first valid frame from which the relative change of every 32 px block's mean luminance stays below
  0.25% for four frames; the case asserts `K <= c_GISettleFrames`, and the stage-2 commit sets `c_GISettleFrames` to the larger of the
  two scenes' K (at most 8, else the filter or the cap is wrong) with the measurement.
- "Two still frames are identical without TAA": frames 16 and 17 of a still camera, the lit 8-bit output identical (section 11,
  question 5, says what happens if a rounding cycle shows).
- "Paths are equal" with the join: `GPU.SceneRenderer.GPUDriven`'s case on the lit image.

`Renderer.Quality` (`RendererQualityTests.cpp`): the row's presets, `Clamp`, the section's round trip of Off, Low and High, the traits
line (memory for High at 1920x1080 equals the layout's, copy included), and the existing "High is Medium below 1440 rows but for the
rows that differ at every size" (222-253) extended by `GlobalIllumination` (D13). `GPU.FeatureTest` and `StrataTests.FeatureTest`
with the new values. `GPU.Editor.Thumbnails` (`GPUThumbnailTests.cpp`, beside the project picture's case at 455): "A project picture
waits for GI to settle" (a project whose scene asks for GI at High: the picture is taken with `GlobalIlluminationSettled` true, and
with the wait removed it is taken on the pending frame).

The gate's re-run on the joined image (information, stage 2's commit; the gate itself stays the stage-1 `gi` view): the fence and
grass cases at 4K on the lit image and the `Indirect` view, `firstFrame = c_GISettleFrames + 2` (D13, "Flicker with feedback").

## 7. Deliverables and acceptance

- **Stage 0:** D11, 6.1. *Accept:* the metric runs on 0.7.0's build; the commit states the indirect view's numbers on the three
  scenes, both modes, at 1280x720, today's GTAO High numbers on the fence strafe (the reference the gate is read against), and the
  talk's outcome.
- **Stage 1:** D2-D10, D12's tooling, 6.2. *Accept:* every 6.2 test on both paths in Debug under the validation layer; the views in the
  editor on the yard, the room and `tp.scene`; **G1 measured** and reported (D12), with the decision the numbers give.
- **Stage 2:** D13, 6.3. *Accept:* 6.3 passes; `GPU.Opaque` with GI off within noise of the branch point (A/B, 7.1); the feature test.
- **Stage 3:** D14, section 8. *Accept:* the owner's live look; the gate's numbers re-measured on the merged branch (no regression);
  plan.md 1.1's GPU busy row (at most 5 ms at High 3840x2160 on the samples' scene cameras with GI on for that measure) measured at
  landing 2.

### 7.1 Perf metrics and the stream's own A/Bs

New `Budgets.json` entries, all `Provisional` (no measurement against them on the temporary memory):

| Metric | Unit | Target | Stage |
| --- | --- | --- | --- |
| `PerfGPU.RadianceCascades.FenceFlicker4K` | ratio | 0.005 | 1 |
| `PerfGPU.RadianceCascades.FenceFlicker4KP99` | ratio | 0.035 | 1 |
| `PerfGPU.RadianceCascades.FenceFlicker4KTAA` | ratio | 0.005 | 1 |
| `PerfGPU.RadianceCascades.FenceFlicker4KTAAP99` | ratio | 0.035 | 1 |
| `PerfGPU.RadianceCascades.GrassFlicker4K` | ratio | 0.01 | 1 |
| `PerfGPU.RadianceCascades.GrassFlicker4KTAA` | ratio | 0.01 | 1 |
| `PerfGPU.RadianceCascades.Yard4KStageGPU` | ms | 1.5 | 1 |
| `PerfGPU.RadianceCascades.Yard4KStageOverAO` | ratio | (none) | 1 |
| `PerfGPU.Renderer.GI4K` | ms | (none) | 2 |
| `PerfGPU.Renderer.GI4KGPU` | ms | (none) | 2 |
| `PerfGPU.Renderer.GI4KStageGPU` | ms | 1.5 | 2 |

**A/Bs (information, never gates; no campaign).** Base: the branch point's (S0.12's tip, or the merge base after tranche 2's merge
of main) Release build: a detached worktree `git worktree add --detach .worktrees/radiance-cascades-base <commit>` (submodules
initialized there), configured from inside it with `cmake --preset windows -B G:/GameEngineAI/build/agent-radiance-cascades-base` (an
absolute directory, so the binaries are in `build/agent-radiance-cascades-base/bin/Release` as the commands below expect) and built
Release only. It counts as a build-heavy run (plan 1, at most four at once). Under the lock, interleaved base, branch, base, branch,
base, branch, each a `LookDev.py capture --repeat 5` of `tp.scene`, `pp.scene`, `ow.scene` at 1920x1080 and 3840x2160,
`--quality high`, TAA:

```sh
python Tools/PerfLock.py --label radiance-cascades -- python Tools/LookDev.py capture --build build/agent-radiance-cascades-base/bin/Release \
    --projects ThirdPerson,PhysicsPlayground,OpenWorld --shots "??.scene" --sizes 1920x1080,3840x2160 --aa taa --quality high --repeat 5 \
    --output .worktrees/radiance-cascades-ab/base-1
# then the branch's build into branch-1, and so on; LookDev.py compare base-N branch-N for the pictures
```

- Stage 1: GPU busy with no view: equal within the pairs' spread (off costs nothing); with the `gi` view: `GPU.RadianceCascades`
  and its ratio to `GPU.AO`.
- Stage 2: GI off: `GPU.Opaque` and busy within the pairs' spread; GI asked: shots `tp.scene.gi`, `pp.scene.gi`, `ow.scene.gi`
  appended to `Shots.json` (the scene cameras, a `Setup` step `component.set` of the sample's Post Process entity, by id as the
  `tp.live.*` shots name it, with `GlobalIllumination` true; existing shots never change, `Shots.json`'s rule), captured on the branch
  only (the base has no such property), their `GPU.RadianceCascades`, `GPU.Opaque` growth (the forward pass's one fetch and branch)
  and busy against the base's `??.scene` captures of the same pairs. The commit states the medians and ratios.

## 8. Documentation

- **DG, Rendering, new section "Radiance cascades"** (after "Ambient occlusion", anchor `#radiance-cascades`; written with stage 1,
  completed in stage 3): what it is and is not (screen-space, no history, the previous frame's color, bounce over frames); the layout's
  rules and the table; the six passes in order with their formats; the march (slices, sectors, thickness, share, self-plane, edge fade,
  fractional cover), the merge, gather, filter, upsample; the soft cap; pending and settling, captures and thumbnails waiting; the
  `gi` and `open` views and their statistics; `RadianceCascadeSettings`; memory and cost; the flicker metric and how to run the gate
  (D12's commands); how it departs from the published form (the plan's table, corrected by stage 0); what it cannot do (off-screen
  light, hidden occluders, emitters under about 19 px at 1080p, orthographic cameras, transparents). "Lighting": the two views in the
  debug views list; the open share in specular occlusion and the clear coat that keeps the screen's occlusion. "View commands" and
  "Viewport panel": the names and legend. "GPU timing": the stage. "Look-dev harness": the yard and grass scenes, the macro, `ratio`.
  "Quality settings" (stage 2): the row in the table, its memory row, and `GlobalIllumination` in the list of rows where High is not
  Medium below 1440 rows (3539-3542). "Feature test": the values. "Perf tests": the `PerfGPU.RadianceCascades` suite's nature
  (deterministic metrics recorded as ratios; the 4K flicker measures the unresolved image under TAA).
- **Architecture.md**: the frame's pass order (`GPU.RadianceCascades` between `GPU.AO` and `GPU.Reflections`; the color copy for the
  next frame; the view pass in `GPU.Transparent`); the stage table (921-923).
- **Manual** (stage 3): `Docs/Manual/Lighting.md` a section "Global illumination" before "Ambient occlusion" (266): what the property
  does, when to use occlusion 0, rooms need a probe (or the volume) because the screen does not show what is behind the camera, the
  first frames after a cut, the two debug views; its "Debug views" section (280) lists `gi` and `open`; `Docs/Manual/Graphics.md` the
  row in "The presets" (its memory at 1080p and 4K).
- **AGENTS.md** (Rendering digest, stage 3; S0.1 freed the room; 246 characters): "- Radiance cascades keep no history: bounce comes
  from the previous frame's color, pictures wait for `GlobalIlluminationSettled`; check changes with the `gi` and `open` views and the
  fence flicker ([GI](Docs/DeveloperGuide.md#radiance-cascades))." Stated in full in the DG section (checklist item 5); `Docs.AgentGuide`
  passes after merging main.
- **Skills**: `strata-editor-automation` (the two debug views, their statistics, `globalIlluminationSettled`, and
  `GPU.RadianceCascades` in its stage list: DG "GPU timing"'s fifth list); `strata-make-a-game` (when to ask for GI, occlusion 0 for
  foliage, a probe per room).
- **Roadmap.md**: Status when the stream lands; "Later": what fell below the cut, and if G1 failed, the branch and the reason.
- **Release-note lines** (0.8.0, the stream's, only what landed):
  - Global illumination from the screen (radiance cascades): Post Process **Global Illumination**, **Intensity** and **Occlusion**;
    Graphics Quality's new **Global illumination** row (Off at Low and Medium, High at High and Ultra; Low is a players' choice). Off
    unless a scene asks; scenes saved before look the same.
  - It costs about 1.2-1.5 ms at 3840x2160 High on an RTX 5090 (the measured value goes here) and about 166 MiB at 1920x1080, 221 MiB
    at 3840x2160 for a view that uses it.
  - New debug views `gi` and `open` with statistics in `viewport.capture`; pictures and project thumbnails wait for indirect light to
    settle.
  - (Section 7 decides the samples': GI on in the template and samples only after the owner's live look.)

## 9. Shared with the other streams, and merges (plan.md 4)

The stream merges main at the start of tranche 2 (with step 0, `reflections`, `physics-timing`, `showcase`), lands first at landing 2,
and `volumetric` lands after it (plan.md 4, merge constraints). `reflections` merges before this stream's stage 2 specular line.

| File | Ours | Theirs | Resolution |
| --- | --- | --- | --- |
| `SH/Scene/Forward.frag`, `SH/Include/IndirectLight.glsl` | `GatherIndirectBounce` (new) and its term after `ComposeIndirect`; the occlusion lines before `GetIndirectInputs` (D13, place 1); `IndirectInputs`' GI fields (appended, initialized in `GetIndirectInputs`, set in `ShadeLitSurface`); `GetDiffuseFallback`; one argument of the `GetIndirectInputs` call (the specular occlusion). `GatherIndirectDiffuse` and `ComposeIndirect` do not change | `reflections`: head (IOR), sun term, `GatherIndirectSpecular` (its D5); `local-lights`: the light loop; `volumetric`: the fog at the end; `showcase-engine`: the surface function | S0.11's regions (plan.md 4's table, step0.md 8.7); our argument change lands after `reflections` merged; re-run S0.11's pixel-equality check (GI off) after each merge |
| `StrataTests/src/Renderer/IndirectFlicker.*` (ours) and `ReflectionMetrics.*` (`reflections`' R1, `specs/reflections.md` D1) | the depth-based `FlickerFrame`/`FlickerSequence` | the plane-based `FlickerMeter`, the same `d` and 32x32 blocks in percent | `reflections` lands first (R1 alone, before landing 1); this stream, landing second, moves the per-pixel `d` and the block statistics into one function in `ReflectionMetrics.*` (`ComputeFlickerStatistics(values, measured mask, size, blockPixels)`) that both meters call; units stay each struct's (percent there, ratio here: the factor 100 documented); `Renderer.ReflectionMetrics`' plane cases and `Renderer.FlickerMetrics` both pass unchanged |
| `SH/Include/Scene.glsl` | SRV 35's declaration, `ST_LIGHTING_VIEW_*` 6 and 7, comments of `AOParams.z`, `ReflectionParams.y/z` | `volumetric` SRV 34/36, `reflections` `ReflectionParams.w` | additive; the slot comment is the arbiter |
| `R/SceneRenderData.h` | push-constant structs, `c_GI*` mirrors, claimed components' comments | others' structs | additive |
| `R/SceneRenderer.cpp/.h` | gating, the stage, the copy flag, SRV 35, the view pass, stats, members, accessors | `local-lights` (stats), `editor-ux` (`RenderOverlays`), `volumetric` (its stage), `physics-timing` (none expected) | different functions; re-read `RenderFrame`'s order after each merge |
| `R/SceneRendererGlass.cpp` | none (the copy's condition is in `SceneRenderer.cpp`) | none | n/a |
| `R/SceneRendererAmbientOcclusion.cpp` | below the cut only (D15.3) | none | n/a |
| `R/RendererQuality.h/.cpp`, `RendererQualityTests.cpp` | the `GlobalIllumination` row, field and traits line after `MotionBlurQuality`'s, the 1440-rows case | `reflections` `ReflectionProbeResolution`, `local-lights` two rows, `volumetric` | line-level, all before the players' rows, field order = traits order; the section version renumbered by the integrator |
| `R/RendererComponents.h/.cpp` | three Post Process properties, their section and reasons | `volumetric` (`HeightFogComponent`, `FogVolumeComponent`) | additive; regenerate `ComponentSchema.json` |
| `R/GPUProfiler.*` and DG "GPU timing"'s five stage lists (`EditorStateCommands.cpp:924`, `EditorProfilerTests.cpp:91`, `FramePerfLogTests.cpp:284`, Architecture.md, the automation skill) | `GPU.RadianceCascades` after `GPU.AO` | `GPU.Volumetric` after `GPU.Shadows` | each at its own position; each stream registers its own stage (step0.md 8.6) |
| `E/EditorViewportCommands.cpp` | the GI wait, two debug views, their description | `showcase-engine` (supersampled stills) | additive lines |
| `E/ProjectThumbnail.cpp` | the GI wait | none | n/a |
| `E/StreamingView.*`, `Panels/ViewportPanel.cpp` | two views, legend | none this wave | additive |
| `Tools/LookDev.py`, `Shots.json`, `LookDevScene.json` | scenes, shots, `ratio`, the grass macro, `DEBUG_VIEWS` | `showcase` (`showcase.*` shots, the sample), `reflections` (`lookdev.sheet/.wet/.tank`) | appended lines; the selftest after merging |
| `Feature.stscene`, `FeatureTest.stproj`, `Budgets.json`, `Strata/CMakeLists.txt` | values, entries, eight shader entries | everyone's | additive; re-run `StrataTests.FeatureTest`, `Docs.AgentGuide` |
| Samples | none (section 7 adopts) | `showcase` | n/a |

Merge order within tranche 2: `animation-graph` touches no renderer file; `local-lights` and this stream meet only in
`SceneRenderer.h` stats and `Forward.frag`'s separate regions. A stream ready before one ahead of it may land first; the later merges
main (plan.md 4).

## 10. Risks

| Risk | Mitigation |
| --- | --- |
| Shimmer on real content (grass, branches, alpha cards, normal-mapped surfaces' geometric normals) | G1 before any join; the degrade path is the plan, not the exception; occlusion 0 for foliage; off until the owner's live look; the irradiance volume as the owner's stated fallback; the showcase's landing does not depend on it |
| The median of medians (D5) differs from the model's exact block median on mixed blocks | "Probe depths follow their blocks" (2% bound); if it fails on real samples, the probes pass of cascade i reads a strided 4x4 sample of its pixels (16 depths) instead |
| Stage 1's single bounce (no feedback) flickers differently from stage 2's multi-bounce | stage 2 re-runs the fence and grass cases on the joined image; the feedback attenuates by albedo times hit share (about 0.1-0.3 a frame) |
| Disocclusion trails at silhouettes while strafing | the near-depth check of D4 (the model's), validity instead of a wrong color, and D6's fill so that a thin strip keeps its cover; "A thin disocclusion keeps its cover" and the fence case catch regressions |
| Half-float pyramid depths and a footprint's offsets break the self-plane test | D6's slack, `(0.5 s_i + TexelPixels)` pixels of height (0.19% of depth at High), four times the half-float rounding; "Flat ground under an open sky is all open" at r = 1 and r = 2 and 45 degrees. The merge, filter and upsample use the probes' RGBA32F records. If contours still show at distance on `ow.scene`, the depth pyramid's x and y become RG32F beside an RG16F for share and coverage (+10.5 MiB at 1080p and 4K High) |
| The disocclusion check at 4K compares a point's depth with the nearest of a 2 x 2 texel | tolerance `0.02 z + 0.03 m` holds to about 30 m on ground seen from 1.7 m; beyond, grazing ground loses its radiance (validity 0, then D6's fill): distant ground's bounce is small. Measured on `ow.scene` in stage 3 |
| Objects moving along the view axis fail the disocclusion check (it reprojects the current position with the previous camera, not the object's motion) | a 0.4 m-a-frame ball loses its own radiance for the frames it moves (validity, then D6's fill); characters at walking speed stay within the tolerance. Documented |
| The copy holds height fog and aerial perspective (applied in `Forward.frag`): distant hits re-emit haze, dense fog brightens bounce | stage 3 checks `ow.scene` and a dense Height Fog (D14); the remedy, if seen, fades hits beyond about 30 m from the probe |
| The radiance cap follows the pre-exposure, which steps in whole stops | only light above the knee changes, by up to 2x at a step; stage 3 walks into the hut under automatic exposure (D14); the remedy, if seen, takes the cap from the metered exposure |
| The `viewport.capture` description pushes `core` past 30,720 bytes | measured first; at most 120 bytes added, the rest in `viewport.setSettings` and DG (D10) |
| Sun glints and bright emissive panels sparkle in the bounce | the soft cap (D4), tuned in stage 3 against the showcase's metal |
| Cost above the estimate | measured as a ratio in stage 1; Low is a quarter of the texels; `MaxCascades`; D15.3 |
| Memory (221 MiB at 4K High) on 8 GiB cards | the row's bytes are shown to players; Low takes 111 MiB at 4K; section 11, question 3 |
| The view pass and the forward pass disagree on the fallback | the view calls `GetDiffuseFallback`, the forward pass keeps S0.11's forms (D13); 6.3's "The views' fallback is the forward pass's" compares them, and "Nothing on screen equals off" checks the forward pass bit for bit |
| A camera cut shows a bright first frame in games | documented; a game's cut is rare in the samples; the showcase's tour fades cover it |
| The ExileCon talk contradicts the design | stage 0 reads it before stage 1's code; its facts correct the table, not the gate |
| Mac (MoltenVK) | compute with core storage formats only, `bitCount`/`findLSB` exist in Metal, no subgroups; the S0.10 conversion check (every registered entry) covers the eight new ones; unverified on a Mac (decisions.md D9) |
| Conversion slips in new Slang shaders (`mul` of vectors, matrix indexing) | the plan's 4.1 rules; the GPU tests' exact values (open share 1, the form factor) catch a wrong sign or product |

## 11. Open questions and the decisions recommended

1. **Should the gate's flicker be measured on the lit image too?** The audit measured the indirect buffer because the lit image has a
   0.4% floor from hard shadow edges reprojected. *Decision:* the gate stays on the `gi` view; stage 2 records the lit image's numbers
   beside it as information.
2. **The plan places settling in stage 2.** *Decision:* the stat and the capture's wait come in stage 1 (its own look-dev pictures and
   G1's cost captures need settled frames; thumbnails and the K measurement with feedback stay in stage 2). Section 12.
3. **Memory above the audit's estimate (221 against 150-170 MiB at 4K).** *Decision:* keep RGBA16F output and the near-depth check for
   quality; if the owner or a memory limit asks, the first cut is moving the upsample into the forward pass (SRV 35 becomes cascade 0's
   filtered probes plus records, 9 MiB; -63 MiB and -1 pass, +4 fetches in the forward pass), measured with the A/B before adoption.
4. **The players' Low value was third in the plan's cut order.** *Decision:* above the cut: one constant, and the feature test needs a
   running non-default value (section 12).
5. **"Two still frames identical" with feedback (stage 2).** The feedback converges geometrically, and half-float rounding could leave
   a one-code cycle on a few pixels. *Decision:* the test asserts identity of frames 16 and 17; if the reference machine shows a cycle,
   the source pass rounds the reprojected color to a 2^-10 relative grid (deterministic, below visibility) so that the fixed point is
   exact, and the test stays as written.
6. **Orthographic views.** *Decision:* no cascades (D1); the property's tooltip and the DG say so.
7. **Where D7's history would live.** *Decision:* probe space (13 MiB), not the full-resolution output (63 MiB); same reprojection
   discipline as SSR. Only if D12.4 is reached.
8. **Measuring the cost on samples in stage 1, when no scene property exists.** *Decision:* through the `gi` view (the cascades run at
   High's layout); `GPU.RadianceCascades` is the same work as with the property.

## 12. Departures from the plan and the audit

| Plan or audit says | Here | Why |
| --- | --- | --- |
| Audit: the cascades' debug views come from the GI buffer, no mechanism named | a view pass after the color copy writes the views into the HDR target; the forward pass is untouched in stage 1 | a view shaded by the forward pass would replace the lit image the next frame's cascades read |
| Plan: settling is part of stage 2 | the stat and the capture's wait in stage 1; thumbnails in stage 2 | stage 1's pictures and G1's captures need it |
| Plan: the players' Low value below the cut (third) | above the cut | the feature test's rule (a running non-default cost knob); one constant |
| Audit: 7 cascades at 1080p and 4K, 8 at 4K Ultra | the count follows the diagonal: 7 everywhere at 16:9, also Ultra | Ultra halves spacing, not reach |
| Audit: 4 px at 4K, "2 px" at 1080p, the rest unspecified | `s0 = max(2, round(2 H / 1080))`: 3 px at 1440p | a power-of-two rule gave 1440p more texels than 4K |
| Audit: memory 150-170 MiB at 4K | 221 MiB | RGBA16F output and the disocclusion check (11, question 3) |
| Model: exact block medians per cascade | median of medians for cascades above 0 | a block of 256x256 pixels is too costly to sort per probe; checked by a test |
| Plan: G1's flicker "p99 near 3%" | `BlocksP99` at most 3.5% | a number the gate can apply |
| Plan: the measure "in new look-dev shots" | the scenes exist in both, the numbers come from GPU tests | look-dev captures are 8-bit, tone-mapped and without depth |
| Model: cover never depends on the color's validity (its source substituted direct light) | validity scales cover only where neither the color's level nor the level two above it knows the color (D6's fill) | the prepass has no albedo, so the engine cannot substitute direct light; an unfilled coupling opens the sky behind every moving silhouette (review) |
| Model: self-plane rejection at 2% of the distance alone | plus a slack of a footprint's height (D6) | the median probe depth, the nearest-of-block texels at 1440 rows and above and the half-float pyramid each put a plane's own samples above it (review) |
| Plan 4.4: "the diffuse gather" changes | `GatherIndirectDiffuse` keeps S0.11's forms; the hit light is `GatherIndirectBounce`, added after `ComposeIndirect` | `ComposeIndirect` scales the sky form's diffuse by `EnvironmentParams.x`, and hit light is absolute (review) |

## 13. Deliver in this order

1. **Stage 0:** `FlickerMetrics.h/.cpp` and `Renderer.FlickerMetrics`; `GIScenes.h/.cpp` (yard, grass, room, paths);
   `GPU.SceneRenderer.IndirectFlicker`; the look-dev scenes, the grass macro and shots; the talk; the commit with the numbers.
2. **Stage 1, in commits:** (a) `RadianceCascadeLayout` and `Renderer.RadianceCascades`; (b) the views' plumbing (enum values,
   forward mapping, editor names, schema, chip, legend, `DEBUG_VIEWS`) with the view pass showing the fallback only (`Valid = 0`) and
   its tests; (c) resources, the pyramid and probes passes, SRV 35 with its dummy; (d) march and merge; (e) gather, filter, upsample;
   (f) gating, copy, pending and settling, the capture's wait, stats, description; (g) the GPU tests of 6.2, the GPU-driven case;
   (h) `PerfGPU.RadianceCascades`, `LookDev.py ratio`, `Budgets.json` entries; (i) the DG section's first form, Architecture.md, the
   stage lists; then G1 (D12) and its report.
3. **Stage 2** (tranche 2, after merging main; first the shared flicker statistics with `reflections`' `ReflectionMetrics`, section
   9): properties and reasons; the row, traits, `SetQuality`'s release and the 1440-rows case; gating's stage 2 form; the occlusion
   lines and `GatherIndirectBounce`; the specular argument after `reflections`; thumbnails; feature test; `PerfGPU.Renderer.GI4K*`;
   6.3; the A/B with the `.gi` shots.
4. **Stage 3:** tuning, the owner's look, docs, release notes, Roadmap; merge main, re-run everything, retake nothing (no sample
   changes until section 7).
5. *Cut line:* D15.1-D15.5 in their order, each with tests, docs and an A/B.

If time runs short, stop after a complete stage; every stage leaves the branch buildable and tested.

## 14. First hour

1. Read plan.md 1, 4.1 and 4.4, decisions.md (D7, D10), this spec with its "Review corrections", `RC/audit.md` and
   `RC/spike/rc_spike.cpp` lines 626-1310 (the candidate's options, pyramids, march, merge, gather, filter, upsample, source) and
   1504-1570 (the flicker measure); `specs/step0.md` section 8 (S0.11's functions) and `specs/reflections.md` D1 (its `FlickerMeter`)
   and D5 (specular occlusion); AGENTS.md's Rendering and Testing; DG "Lighting", "Ambient occlusion", "Reflections", "Look-dev
   harness", "GPU timing", "Quality settings".
2. Confirm S0.10, S0.11 and S0.12 are on main (`git log main --oneline -20`): read `Include/IndirectLight.slang` (or `.glsl`), the
   slot comment in `Scene.*`, whether `GPU.RadianceCascades`, `gi` and `open` are already registered; adjust this spec's names to
   step 0's.
3. `git worktree add .worktrees/radiance-cascades -b agent/radiance-cascades main`; submodules; `cmake --preset windows -B
   build/agent-radiance-cascades`; build Debug; run `StrataTests.exe --test-suite=GPU.SceneRenderer.LightingViews,GPU.SceneRenderer.SSR,
   GPU.SceneRenderer.GPUDriven,GPU.SceneRenderer.AmbientOcclusion` so the baseline is green here.
4. Make the base as 7.1 says (a detached worktree of the branch point, its Release in `build/agent-radiance-cascades-base`);
   `python Tools/PerfLock.py --status`.
5. Start stage 0 with `Renderer.FlickerMetrics`'s "A steady drift scores zero" (the metric's core), then the yard builder and the
   first GPU case on 0.7.0's indirect view.

## Review corrections

An independent review on 2026-11-02 checked this spec against the code at 76f28a21, plan.md, decisions.md, the audit with its CPU
model, and the specs beside it (step0, step0-editor, step0-platform, physics-timing, reflections; showcase and step0-slang did not
exist yet). Every file:line in 2.1 and 2.2 was checked and holds, as do the layout and memory tables (recomputed by script), the
246-character AGENTS.md bullet, the unused `AOParams.z/w` and `ReflectionParams.y/z/w`, and the stats path of `LookDev.py`. Per
decisions.md, these corrections win over the body where they differ; they are already applied above.

**Design (would have made the picture flicker or the tests fail):**

1. **Cover no longer falls with an unknown color where a coarser level knows it (D4, D6, constants, D12.1, 6.2, 10, 12).** The body
   scaled both light and cover by the radiance's validity, so every disocclusion strip behind a moving post opened the sky's light
   through the wall behind it, on every frame of the very strafe the gate measures. The model never did this (its source substitutes
   the current frame's direct light, `rc_spike.cpp:1232-1260`), so its 0.45% was not measured with it. Now the march fills the
   unknown share from the level `c_GIColorFillLevels` (2) above; cover falls only where neither knows (pending frames keep their
   "fallback alone"). Tuned on the tuning day against 0 (the body's form); new test "A thin disocclusion keeps its cover".
2. **The self-plane test gained a slack of a footprint's height (D6, constants, push constants, D12.1, 6.2, 10, 12).** With 2% of
   the distance alone, the probe's median depth (half a block's gradient off its plane), the nearest-of-2x2 texels at 1440 rows and
   above and the half-float pyramid put a plane's own samples above it at the first steps (0.125 of the distance at cascade 0 at
   1080p for planes seen at 5 degrees and more), covering horizon sectors by fractions of a percent that change as the camera moves,
   and failing "Flat ground under an open sky is all open" and stage 2's bit-equality. That test now also runs at 2560x1440 (r = 2)
   and from 45 degrees, and fails with the slack at 0. The risk row's "+21 MiB" for 32-bit depths was wrong: +10.5 MiB.
3. **The join is written against S0.11's real functions (D13).** S0.11's `ComposeIndirect` multiplies the sky form's diffuse gather by
   `EnvironmentParams.x` (step0.md 8.2), so hit light inside `GatherIndirectDiffuse` would have been scaled by the sky's intensity.
   Hit light is now `GatherIndirectBounce`, added after `ComposeIndirect` and before the clear coat; the fallback's occlusion is
   formed in `ShadeLitSurface` before `GetIndirectInputs` (as `1 - z * closed` so that a closed share of 0 is bit-exact for any
   `GlobalIlluminationOcclusion`); the GI fields are initialized in `GetIndirectInputs` and set after it; the specular change is one
   argument of that call after `reflections` merged; the `ao` view stays material times screen; the clear coat keeps the local
   occlusion. `GatherIndirectDiffuse` and `ComposeIndirect` are untouched.
4. **`GetDiffuseFallback` rotates the environment direction** (`RotateEnvironmentDirection`, as `Forward.frag:259`), and a 6.3 case
   compares the view's fallback with the forward pass's forms.

**Tests that could not be implemented or would pass without the change:**

5. "Gate G1: the yard's stage" measured `GPU.AO` in a scene whose neutral Post Process turns AO off: it now turns AO on (D12).
6. "The gi and open views leave the forward pass alone" compared scene-color copies that a frame without a GI view never makes: both
   renderers now ask for Spatial reflections (6.2).
7. "Hits outside the screen" and "Probe depths follow their blocks" had unmeasurable or wrong bounds (a median of medians is not
   within 2% of the exact median on a block straddling two depths): rewritten with exact checks (6.2).
8. "Without a gi or open view": the black binding is not observable; the accessor is (6.2). The capture test's "cascades 7" holds only
   for 16:9 outputs of 720 rows and more (6.2).
9. Stage 2's flicker re-runs started at frame 3, inside the convergence the feedback causes (a geometric approach is not a steady
   drift): they now start at `c_GISettleFrames + 2` (D13, 6.3). New 6.3 cases: specular occlusion takes the open share; the views'
   fallback equals the forward pass's; the project picture's test is in `GPU.Editor.Thumbnails` (`GPUThumbnailTests.cpp:455`).
10. `MeasureSequenceFlicker` held every frame: 14 frames at 4K are 2.3 GB. `FlickerSequence` keeps three (D11).

**Rules and integration:**

11. The quality row: `GlobalIlluminationLevel` in natural order (Off 0, Low 1, High 2; Ultra 3); the field and traits line placed
    after `MotionBlurQuality`, before the players' rows; `SetQuality` releases the cascades on a change of the row (else "The row
    follows SetQuality" fails); the existing "High is Medium below 1440 rows" case and DG's list gain the row (else that case fails);
    the bytes function's `ComputeLayout` signature with a texture limit (D2, D13).
12. The GPU stage goes into the five lists DG "GPU timing" names, the automation skill included; this stream registers it (S0.11 only
    names it, step0.md 8.6) (D1, 2.1, 9).
13. `viewport.capture` is a `core` command and `core` must stay within 30,720 bytes: the description grows by at most 120 bytes,
    measured first (D10, 6.2, 10).
14. The flicker metric is `reflections`' `FlickerMeter`'s definition; landing second, this stream merges the two into one statistics
    function (D11, 9).
15. D10's open-source ruling (the owner, 2026-11-01) replaces the stream rule written before it.
16. The gate's TAA mode measures the unresolved, jittered image (`HDROutput` ends before the resolve): stated, so that nobody passes
    the gate on TAA's history; 64 px block values are printed beside the 32 px gate at 4K to compare with the model's 1080p (D11, D12).
17. Stage 0 also measures today's GTAO on the fence strafe: today's indirect light without AO is deterministic and near 0, so it was
    no reference for 0.5% (D11, 7).
18. The owner's live look (stage 3's exit) is in no AA, FXAA and TAA, as plan 4.1 rules; TAA alone hides flicker (D14).
19. Smaller fixes: push-constant struct names in D3's table (`RadianceCascadeProbePassParameters` for gather, filter and upsample;
    the march struct grew to 80 bytes); the filter's odd count needs no copy; D15.2's bent direction is R32_UINT (RG16F storage
    needs the extended formats MoltenVK may lack, and its 4K size is 31.6 MiB, not 16); the memory table's uncounted velocity target;
    colored emission in the yard; the stage-2 A/B's `.gi` shots instead of changing existing shots (Shots.json's rule); the base
    build's directories; new risks (fog and aerial perspective in the copy, the cap stepping with the pre-exposure, the
    disocclusion check at distance and for objects moving along the view axis); Manual "Debug views" and the chip's labels.

Orchestrator's alignment (2026-11-02): the wave has one flicker meter, `StrataTests/src/Renderer/FlickerMetrics.h/.cpp`
(CPU suite `Renderer.FlickerMetrics`), holding D11's definitions word for word plus `reflections`' `ReadDisplayFlickerFrame`.
Whichever of `reflections` and this stream lands first creates it; the other takes main's. The GPU suite keeps the name
`GPU.SceneRenderer.IndirectFlicker`.
