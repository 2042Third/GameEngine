# Workstream `radiance-cascades`, world-space form: global illumination traced through a voxel clipmap (wave 8)

The owner, 2026-11-03: "Radiance cascade is the priority", "don't use ssgi. I don't like screen space global illumination",
"Irradiance prob should be used when radiance cascade absolutely doesn't happen". The look stays wave 7's: realistic and filmic,
the Fox Engine (MGS V) baseline, never blurry (no temporal upscaling, nothing that needs TAA to look right), never clinical, no
shimmer; highlights matter most.

This spec replaces `specs/radiance-cascades.md` (the screen-space form) for everything but the parts it names as reused. It
implements the research's R1-R5 (`research/radiance-cascades-field.md`, read it first) under the orchestrator's rulings of
2026-11-03 (`specs/decisions.md`, the last three entries). Where this spec and decisions.md disagree, decisions.md wins; where it
departs from the research or plan.md 4.4, section 12 says so with the reason.

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `E/` is `StrataEditor/src/Editor/`, `T/` is
`StrataTests/src/`, `DG` is `Docs/DeveloperGuide.md`, `LAB/` is
`.worktrees/wave8-plan/downloads/rc-research/godot-rc-lab/src/rc/shaders/` (radiance-cascades-lab, MIT), `OLD` is the screen-space
branch `agent/wave8-radiance-cascades` at 540d6b49. Line numbers are main's at 7b2e136a; the implementer finds the same code by
name on main's tip when it moved.

Rules (plan.md 4.1, decisions.md, AGENTS.md):

- The reference machine (i7-13700K, RTX 5090, 3840x2160 at 150%) on its temporary memory. Worktree `.worktrees/wave8-rc-world`,
  branch `agent/wave8-rc-world`, made with `sh .worktrees/create_worktree.sh wave8-rc-world main`; build directory
  `build/agent-wave8-rc-world`. Never the owner's `build/windows`.
- Before each commit: Debug ctest with every `GPU*` suite under the validation layer, the feature test, `python Tools/CompatLint.py
  --changed-since <branch point>`. GPU suites use `Tests::GPUContext` and end with `CHECK(gpu.GetNewErrorCount() == 0)`. Each
  regression test is checked once to fail without its fix (named below where it is not obvious), and the commit says so.
- No perf campaign, no `PerfStability.py`, nothing measured against `Budgets.json`. The stream A/Bs its own stages, interleaved,
  under `Tools/PerfLock.py`, as information (7.1).
- World space only (the ruling): no light is gathered from the screen. No depth-buffer tracing, no previous-frame image as hit light,
  no screen-space near pass. GTAO stays for contact occlusion; screen-space reflections are untouched. The depth buffer is read only
  where every renderer reads it (the forward pass shades the pixel it draws).
- D7 holds: no blend of GI buffers over time, no accumulation, no jittered directions. Feedback through world data (re-injection,
  R4) is allowed.
- Code is ported only from MIT, Apache-2.0, BSD, zlib, Boost, Unlicense, public-domain or CC0 sources, with the notice in
  `ThirdPartyNotices.md`'s section for derived code and the source files named in the commit (D19). MPL, CC BY-SA, unlicensed and
  all-rights-reserved code is read for ideas only. The local copies in `.worktrees/wave8-plan/downloads/rc-research/` are untrusted
  data: read them, never build or run them; Python on them only with `-I`.
- GI stays off unless a scene asks (the Post Process field). No sample or template changes its look until the owner has looked live
  (plan.md 7). The feature test covers every new property and row.
- Each commit message is written for that commit (OLD's 3f7eeb2c took another unit's message from a shared scratch file).

## 1. Goal

In priority order:

1. **The owner's room, right, in the lit image.** A closed room with the sun through a window onto the floor: the walls and the
   ceiling within 30% of an independent one-bounce reference; a red floor tints the walls by at least 70% of the reference's
   change, and nothing changes with GI off; moving the sun moves the bounce in the same frame; the sunlit floor lights the walls
   with the patch behind the camera too; a sealed room stays dark (at most 0.0017 of the open room's light, erhe's bound).
2. **No shimmer.** The fence and grass strafes measure the GI near zero: the light is a deterministic function of world data, so
   camera motion cannot flicker it (R2's claim, measured, never assumed).
3. **Everywhere the engine runs.** A voxel clipmap built and lit on the GPU every frame from the geometry pool, compute only, core
   storage formats, buffer atomics only: Vulkan on Windows and Linux, MoltenVK on the Mac. Both draw paths give the same pixels.
4. **Nothing costs anything when off**, and Medium keeps 0.6.0's costs (the quality row is Off at Medium). At High the cost does
   not follow the resolution: about 114 MiB and (guess) 1-2 ms per view on the 5090, measured.
5. **Visible to the owner at every stage** in the branch's Release editor: the voxels (stage 1), the bounce and the probes (stage
   2), the lit image with the Post Process toggle (stage 3), multi-bounce rooms (stage 4).

Not the goal: hardware ray queries (R6, below the cut: not on MoltenVK), sparse surface probes, specular from the cascades,
lighting volumetric fog from the field, the irradiance volume of plan item 2 (only if the cascades absolutely fail: section 11,
question 11), any history.

## 2. Where we start

### 2.1 Files on main

| File | Lines | Role | This stream |
| --- | --- | --- | --- |
| `R/SceneRenderer.cpp` | 4,156 | scene layout 360-391 (SRV list ends with 29, 32, 33 at 384-386); `CreateSceneBindingSets` 1032- (SRV 29/32/33 at 1068-1070); `SetQuality` 1882; `ReleaseRenderTargets` 2206; `RenderFrame` 2856: render scene and its GPU copy 2876-2913, Post Process 2917-2918, cut 2965 and 3087-3092, reflections' gating 2935-3022, frame constants 3027-3051, `AOParams` 3171, rebind checks 3175-3180 and 3230-3233, prepass 3329-3355, `GPU.ScreenShadows` 3358-3368, `GPU.AO` 3370-3376, `GPU.Reflections` 3377-3387, `GPU.Opaque` 3388-3394 | gating, the GI stage, SRVs 35 and 37-40, stats |
| `R/SceneRenderer.h` | 2,787 | `SceneRendererSpecification` 125-, `SceneLightingView` 328-336, `ResetHistory` 975, `IsCameraCut` 1676 | settings struct, views 6-9, stats, members |
| `R/SceneRenderData.h` | 668 | `FrameConstants` 41-106 (`ReflectionParams` 101-104, `static_assert` 106), `InstanceData` 165-175 | appended frame constants, GI structs, `c_GI*` |
| `R/GPUSceneData.h` | 258 | `GPUProxy` 15-29, `GeometryEntry` 34-41, `GeometrySubmesh` 43-53 (bit 0 of `Flags` taken), `GeometryLOD` 56-67, `c_CullMeshSkinnedArena` 84, material flags 85-91 | `c_GeometrySubmeshClosed` |
| `R/GeometryPool.h/.cpp` | 285 / 631 | strides 76-79, `SetLayout` 131, tables 135-146, buffers 148-153 | read only (the voxelizer reads its buffers and tables every frame) |
| `R/Mesh.h/.cpp` | 224 / | positions and indices stay on the CPU (`Mesh.h` 92-96); layout upload `Mesh.cpp` 361-376 | closedness of LOD 0 into the submesh flags |
| `R/RenderSceneGPU.h` | 369 | `IsResident` 111, buffers 121-129, tables 133-140 | read only |
| `R/RenderScene.h` | 564 | `RenderProxyFlags` 35-49, proxy accessors 195-232, `GetSpatialIndex` 212, `GetProxyVersion`/`GetProxyChanges` 272-280 | read only (a proxy-log reader) |
| `R/SpatialIndex.h` | | `QueryPlanes` 65-68 | read only (box queries as six planes) |
| `R/RendererQuality.h/.cpp` | 268 / 898 | settings 111-164 (`MotionBlurQuality` 149, players' rows after it), `c_SectionVersion` 169, `RowTraits` 224-233; `.cpp` rows 49, traits 123-136, `GetPreset` 406, `Clamp` 441, `MakeSection` 647, `FromSectionValue` 738 | the `GlobalIllumination` row |
| `R/RendererComponents.h/.cpp` | | `PostProcessComponent`, its properties and inactive reasons | two properties |
| `R/GPUProfiler.h/.cpp` | 204 / 393 | `RendererGPUStages` 20-38; registration 50-65 (`GPU.AO` 57, `GPU.Reflections` 58) | `GPU.RadianceCascades` after `GPU.AO` |
| `R/Renderer.h` | | `GetBlack3DTexture` 110, `ReadTexture` 115 (2D slices only) | dummies; a 3D slice read for tests |
| `SH/Include/Scene.slang` | 368 | slot comment 7-14 (next free SRV 37), lighting views 31-37, `FrameConstants` 48-110, `InstanceData` 114-121, `MaterialData` 254-280, `OctahedralEncode/Decode` 351-366 | SRVs, views, appended constants |
| `SH/Include/IndirectLight.slang` | 115 | `IndirectInputs` 16-32, `GatherIndirectDiffuse` 46-75, `GatherIndirectSpecular` 80-102, `ComposeIndirect` 106-113 | the field's gather, the field's fields appended |
| `SH/Scene/Forward.frag.slang` | 480 | `GetIndirectInputs` 115-162; `ShadeLitSurface` 170-285 (views 246-258, multi-bounce 259-262, compose 264-267, clear coat 270-281); view outputs 334-381 | the field in shading and in the `gi`/`open` views |
| `Strata/CMakeLists.txt` | | `strata_add_shaders` | twelve entries |
| `E/StreamingView.h/.cpp`, `StrataEditor/src/Panels/ViewportPanel.cpp`, `E/EditorViewportCommands.cpp`, `E/EditorStateCommands.cpp`, `E/ProjectThumbnail.cpp` | | debug views, chip, legend, capture waits and description, `editor.perf` | four views, waits, the GI object |
| `Tools/LookDev.py`, `Tools/LookDev/LookDevScene.json`, `Shots.json` | | the look-dev room at `LookDevScene.json` 23-34 | sun room scenes, shots |
| DG | 5,783 | Rendering 3665-; Quality settings 3821-3957; Render scene 3959-4010; Batching (the geometry pool) 4012-4053; GPU-driven path 4055-4128; Camera-relative space 4130-4143; Skinned meshes 4275-4288; Sun shadows 4336-4527; Lighting 4578-4650 (the seam 4612-4618); Ambient occlusion 4652-; GPU timing 5524-5553 | a new section and lines in these |

### 2.2 What the frame gives the GI

- **The scene on the GPU, on both paths.** Every renderer updates the render scene's GPU copy at the start of its frame
  (`SceneRenderer.cpp:2899-2913`): instances (`InstanceData`, 64 bytes, world rows), a record per proxy (`GPUProxy`: world bounds,
  mesh slot, flags), the mesh table (per mesh slot the loaded mesh's `GeometryHandle`, with `c_CullMeshSkinnedArena`), material data
  per slot (`MaterialData`: base color, emission, flags, texture table indices). The geometry pool holds positions (12 B), attributes
  (36 B), 32-bit indices and the tables `GeometryEntry`, `GeometrySubmesh`, `GeometryLOD` (DG "Batching": read through the pool every
  frame, compaction moves it). Nothing the GI needs depends on the draw path or on culling: GI is the same on both paths by
  construction.
- **The scene on the CPU.** `RenderScene` keeps world bounds, mesh slots, flags and instance ranges per proxy, a proxy change log
  (`GetProxyChanges`), the mesh layouts (submeshes, LOD index counts and errors) and a spatial index of indexed proxies
  (`QueryPlanes`). Positions and indices of meshes stay on the CPU (`Mesh.h` 92-96), so closedness is computed there.
- **The sun and the sky.** `FrameConstants::DirectionalLightDirection/Color` (lux, after the atmosphere), the environment cube
  `u_EnvironmentMap` with mips (512 per face), `EnvironmentParams` (intensity, rotation, `w` 1 with a sky), `AmbientColor` without one.
- **The indirect light's seam** (S0.11, `IndirectLight.slang`): `GatherIndirectDiffuse` owns the probes' and sky's diffuse with
  0.7.0's four forms; `ComposeIndirect` scales the sky form's sum by `EnvironmentParams.x`. The field's diffuse is absolute light
  (the sky's intensity already applied), so it enters after `ComposeIndirect`, as OLD's `GatherIndirectBounce` did (section 12).
- **Slots and names step 0 assigned:** SRV 35 "radiance cascades" (this stream; now the field), 30-31 the irradiance volume
  (reserved, not built), 34 and 36 volumetric's, next free 37; `ReflectionParams.y` (GI joined) and `.z` (its intensity); stage
  `GPU.RadianceCascades` after `GPU.AO` (registered by this stream); debug views `gi` and `open` after `albedo`.

### 2.3 What stages 0 and 1 built, and the branch decision

OLD holds 11 commits (`git log --oneline 7b2e136a..540d6b49`) and two stashes: `stash@{0}` "room-reference-wip" (the sun room and
its CPU one-bounce reference) and `stash@{1}` "join-wip" (the quality row, the gate's degrade forms, the `.gi` shots).

**Decision: a new branch from main's tip, taking the reusable parts by file.** Reasons:

1. What is reusable is separable by file (test helpers, look-dev, editor names) or by small hunks (the row); what is not is most of
   the code (eight screen-space shaders, `RadianceCascadeLayout`, `SceneRendererRadianceCascades.cpp`, the view pass, the
   full-resolution SRV 35 texture and its semantics, `GetDiffuseFallback`, `RadianceCascadeSettings`' screen fields). Removing them
   on OLD would land about 2,000 lines into main's history and out again in the same landing, plus 3f7eeb2c's wrong message.
2. "Nothing screen-space ships, and no dead code lands": a branch whose history never held the screen-space passes is the plain way
   to keep both. The screen-space form stays on OLD's history only (decisions.md, 2026-11-03).
3. OLD's branch point (7b2e136a) is behind main; a fresh branch takes step 0's later lanes and whatever landed since without a merge.

OLD and its stashes stay untouched as the record (section 11, question 8).

| Reused | From | How |
| --- | --- | --- |
| The flicker meter: `T/Renderer/FlickerMetrics.h/.cpp`, `FlickerMetricsTests.cpp` (`Renderer.FlickerMetrics`) | 2228a8b2 | whole files (`git checkout 540d6b49 -- <paths>`); if `reflections` landed it on main first (decisions.md: "the first lander creates it"), main's wins and ours is dropped |
| The GI scenes: `T/Renderer/GIScenes.h/.cpp` (yard, grass, room, strafes) | 2228a8b2, 540d6b49 | whole files; the doc comment's "Radiance cascades" link becomes "Global illumination" |
| The sun room and its reference: `SunRoomOptions`, `SunRoomDirection`, `BuildSunRoom`, `SunRoomReference`, `SunRoomRadiance` | `stash@{0}` (`GIScenes.*` hunks) | applied by hand; `ReadGlobalIllumination` (read the full-resolution SRV 35) is not taken; options gain `SealWindow`, `Offset` and `PairedRoom` (6.2), each reflected in the reference's boxes |
| `GPU.SceneRenderer.IndirectFlicker` (0.7.0's indirect light measured) | 2228a8b2 | whole file (the baseline the gate is read against) |
| Look-dev: scenes Yard and Grass, the grass texture, the `lookdev.grassCards` macro, `LookDev.py ratio`, `DEBUG_VIEWS` | 2228a8b2, 3f7eeb2c | the `LookDev.py`, `LookDevScene.json` hunks; `DEBUG_VIEWS` gains `voxels` and `probes` |
| Shots: `lookdev.yard*`, `lookdev.grass*`, `lookdev.room.view.*`, `tp/ow/pp.scene.view.gi`; `lookdev.room.gi`, `.redfloor`, `.redfloor.gi`, `lookdev.yard.gi`, `tp/pp/ow.scene.gi` | 2228a8b2, 3f7eeb2c, `stash@{1}` | `Shots.json` hunks (the `.view.gi` shots now show the world-space view) |
| Editor: `ViewportDebugView::GlobalIllumination`/`OpenShare`, names `gi`/`open`, the chip's labels, the open legend (`GetOpenShareLegendColor`), the legend note, the capture's `globalIllumination` object and wait, `editor.perf`'s key; the five stage lists' `GPU.RadianceCascades` | 3f7eeb2c, 1409f8df, 0b9a215b | hunks of `StreamingView.*`, `ViewportPanel.cpp`, `EditorViewportCommands.cpp`, `EditorStateCommands.cpp`, their tests; descriptions rewritten for world space ("the share the screen leaves open" becomes "the share of the sky a surface sees") |
| The row: `GlobalIlluminationLevel` (Off 0, Low 1, High 2), the field after `MotionBlurQuality`, `c_GlobalIlluminationRow = { Off, Off, High, High }`, names | `stash@{1}` | hunks; `GlobalIlluminationBytes` rewritten over the world layout (D2) |
| The perf suite's strafe harness (`MeasureStrafe`, `Report`, the 4K constants) | 540d6b49 `T/Perf/RadianceCascadesPerfTests.cpp`, `stash@{1}` | adapted into `T/Perf/GlobalIlluminationPerfTests.cpp` |
| Lessons | 4ba9dbe3, e7927fdd | a pipeline's layouts never hold one slot twice (NVRHI refuses it whatever the sets): the overlay passes take their own layouts, never the scene layout beside them; a frame that does not want GI releases it, so the SRVs bind dummies again |

Not reused: `R/RadianceCascadeLayout.*`, `R/SceneRendererRadianceCascades.cpp`, `SH/GI/RadianceCascades*.slang`,
`SH/GI/GlobalIlluminationView.frag.slang`, `SH/Include/RadianceCascades.slang`, `GetDiffuseFallback`, OLD's GPU and layout tests,
its `Budgets.json` entries, its DG section.

## 3. Order, stages and the cut line

Each stage is commits that build and pass everything, and ends with the branch's Release editor built for the owner and a short
note of what to look at. The stream lands on main after stage 3 at the earliest (GI off by default), normally after stage 4.

| Stage | Size | What | Visible to the owner | Exit |
| --- | --- | --- | --- | --- |
| 1. The clipmap | XL, 3 days (2.5-4) | the branch and the reused parts (2.3); the layout (D2); the clipmap's windows, scrolling and regions (D3); resources (D4); voxelization from the geometry pool with large-triangle tiles and budgets (D5); solid fill and closedness (D6); finalize (D7); moving objects re-voxelized, skinned ones left out (D8); the `voxels` view (D15); `GPU.RadianceCascades` registered; 6.1 | Debug view **Voxels**: the scene as the GI sees it, following the camera, moving objects moving | 6.1 passes; voxels equal a fresh renderer's after any camera path |
| 2. Light and probes behind views | XL, 2.5 days (2-3.5) | injection: the sun with a voxel shadow march, emission (D9); probe cascades, validity, weights (D10); the march with the miss rule and the occlusion lead-in (D11); the merge (D12); the field (D13's reduce) and the forward gather for the `gi` and `open` views; the `probes` view; statistics (D15); the owner's room cases on the views; the fence and grass gate (6.2) | Debug views **GI**, **Open**, **Probes**: bounce from the sun patch, following the sun; a sealed room dark | **Gate W** (D14.7) |
| 3. The join | L, 1.5 days (1-2) | `PostProcessComponent::GlobalIllumination` and `GlobalIlluminationIntensity`; the row (Off, Low, High; Off at Medium); the field in `ShadeLitSurface` and the transparent variants (D16); off path; captures and thumbnails wait; feature test; perf metrics; look-dev before and after (6.3) | The Post Process toggle in the lit image: the owner's red floor experiment live | 6.3 passes |
| 4. Multi-bounce | M, 0.75 day (0.5-1) | re-injection of last frame's field (D17); settling measured; the owner's live look in motion; docs final (section 8) | Rooms fill, corners light | 6.4 passes; the owner's live look |
| **Cut line** (7.75 days nominal, 6-10.5 as ranged) | | | | |
| 5a. Local lights | M, 0.5 day | point and spot lights injected with a voxel shadow march (D18.1) | | |
| 5b. Moving objects without pops (R5) | M, 0.75 day | fractional occupancy of moving proxies, a soft shadow march near them (D18.2) | | only after 6.2's moving-prop case fails its trigger |
| 5c. Posed skinned meshes | M, 0.5 day | characters voxelized from their palettes (D18.3) | | |
| 5d. The open share in specular occlusion | S, 0.25 day | after `reflections` merged (D18.4) | | |
| 5e. Ultra | S, 0.25 day | a larger probe field (D18.5) | | |
| 5f. Textured albedo per voxel | S, 0.5 day | (D18.6) | | |
| 5g. Probe relocation | M, 0.75 day | (D18.7) | | |
| 5h. Hardware ray queries (R6) | XL | a second tracer for the same probes where `rt::AccelStruct` exists (D18.8) | | |

The sizes are honest guesses: OLD's stage 1 was sized L (3 days) and ran over through its tuning day and degrade path. If stages 1
and 2 together pass 6 days, stage 4 moves below the cut and the stream lands after stage 3; the orchestrator decides with the
owner's usage figure (decisions.md D8). If Gate W fails after its one tuning day, the stream stops at its views (branch kept) and the
orchestrator decides between more tuning and DDGI-style irradiance probes over the same voxels (the research's fallback: nothing of
stage 1 is lost).

## 4. Where each setting lives

| Feature | Kind | Setting | Inspector | Agents and scripts |
| --- | --- | --- | --- | --- |
| The GI's debug views | viewport setting, renderer option | `debugView` `"gi"`, `"open"`, `"voxels"`, `"probes"`; `SceneLightingView::GlobalIllumination` (6), `OpenShare` (7), `Voxels` (8), `Probes` (9) | the Viewport's debug chip and its legend | `viewport.setSettings`, `viewport.capture {debugView}` with `debugView.globalIllumination` |
| A scene asks for GI (stage 3) | Post Process | `GlobalIllumination` (off), `GlobalIlluminationIntensity` (1, 0-4) | section "Global illumination" after "Ambient occlusion", the intensity's inactive reason | `component.set`, `Entity::SetProperty` |
| What a machine affords (stage 3) | quality row | `Renderer.Quality.GlobalIllumination`: `Off`, `Low`, `High` (Off at Low and Medium; High at High and Ultra) | Graphics Quality page (generic) | `project.setSettings`; players through `IGameGraphics` |
| Tests' tuning | renderer specification | `SceneRendererSpecification::GlobalIllumination` (`GlobalIlluminationSettings`: `BounceGain`, `VoxelizeTrianglesPerFrame`, `VoxelizeTrianglesOnCut`) | none | none |
| Pictures wait | renderer stat | `SceneRendererStats::GlobalIlluminationSettled` | none | `viewport.capture`'s `globalIlluminationSettled` |

No script host function: the properties go through `Entity::GetProperty`/`SetProperty`, the row through the players' graphics
options. The script ABI does not change.

## 5. Design

### D1 Rules this stream keeps

- **World space only.** Every input of the light is world data: the voxel clipmap built from the geometry pool, the sun, the sky,
  emission and (stage 4) last frame's field. The forward pass reads the field at the point it shades.
- **No history (D7).** Every frame's probes are complete in themselves: all cascades are traced, merged and reduced every frame from
  the voxels; directions are fixed; nothing is jittered or blended over frames. The voxels persist (they are the scene, rebuilt where
  it changed); re-injection feeds last frame's field into this frame's voxel light (stage 4), as decisions.md allows.
- **Off costs nothing.** No texture, buffer, pipeline, pass or stage until a frame wants GI; a frame that does not want it releases
  everything at once (e7927fdd's lesson); the scene layout's new SRVs bind `Renderer::GetBlack3DTexture()`; the forward pass reads
  them only behind a uniform branch (`GIFieldSize.w` for the views, `ReflectionParams.y` for the join). Without GI the forward pass
  runs S0.11's code exactly.
- **Both paths.** The GI reads the render scene's GPU copy, the geometry pool and the CPU's render scene, never culling results:
  `GPU.SceneRenderer.GPUDriven` gains cases that compare the voxels, the views and the lit image bit for bit.
- **Shadow caches.** Nothing changes how casters are drawn: no cache input, no version.
- **Derivatives.** None: compute, `Load` and `SampleLevel`; the forward pass's gather takes no derivative.
- **Frame constants** grow only for what `Forward.frag` reads, appended at the end (D14.4). Everything else goes into the GI's own
  constant buffer.
- **Camera-relative.** Every position uploaded to shaders is relative to the camera, subtracted in double on the CPU (D3.2, D5.4).
- **Formats and features** (MoltenVK): storage images only in Vulkan's mandatory storage formats (`RGBA8_UNORM`, `RGBA8_SNORM`,
  `R32_UINT`, `RGBA16_FLOAT`), every one declared with `[vk::image_format(...)]`; atomics only on buffers (`RWByteAddressBuffer`
  `InterlockedAdd`/`InterlockedMax`: Metal has device buffer atomics everywhere, 3D texture atomics only on recent Apple GPUs);
  3D textures at most 256 texels a side (Vulkan's guaranteed `maxImageDimension3D`; D2's largest is 192); one indirect dispatch
  (`dispatchIndirect`). Shaders in Slang, HLSL syntax only (DG "Shaders": `mul` only with a matrix, `all()`/`any()`, `SampleLevel`
  in compute, `NonUniformResourceIndex` for bindless reads).
- **Determinism.** No floating-point atomics: albedo and emission resolve with `InterlockedMax` on packed words, normals and winding
  steps add integers (order-free). Two renderers that voxelize the same regions write the same bits.

### D2 The layout

`R/GlobalIlluminationLayout.h/.cpp`, namespace `GlobalIllumination` (CPU, unit-tested by `Renderer.GlobalIlluminationLayout`):

```cpp
struct VoxelLevelLayout
{
	float VoxelSize = 0.0f;        // Meters: VoxelSize0 * 2^level
	glm::uvec3 Dimensions{ 0 };    // Voxels per axis (every level the same)
};
struct ProbeCascadeLayout
{
	float Spacing = 0.0f;          // Meters: s0 * 2^k
	glm::uvec3 Grid{ 0 };          // Probes per axis: cascade 0's grid / 2^k
	uint32_t DirectionsPerAxis = 0; // Octahedral tile side a_k = 4 * 2^k (a_k^2 directions)
	float IntervalStart = 0.0f;    // Meters along the ray
	float IntervalEnd = 0.0f;      // Infinity for the top cascade: it marches until it leaves the clipmap
	float LeadIn = 0.0f;           // Meters of occlusion-only march before IntervalStart (D11): 1.3 * Spacing for k >= 1, 0 for k = 0
	uint32_t MinimumLevel = 0;     // The finest voxel level its samples read: max(0, k - 1)
};
struct GlobalIlluminationLayout
{
	std::vector<VoxelLevelLayout> Levels;
	std::vector<ProbeCascadeLayout> Cascades;
	uint64_t GetMemoryBytes() const; // Every resource of D4, as the renderer makes them
	uint64_t GetRaysPerFrame() const;
};
namespace GlobalIllumination
{
	// The layout of a quality level (Off: none).
	std::optional<GlobalIlluminationLayout> ComputeLayout(GlobalIlluminationLevel level);
	// Octahedral direction of texel (u, v) of an a x a tile: the engine's OctahedralDecode (Include/Scene.slang) of the texel center
	// mapped to [-1, 1]^2; and its solid angle, the octahedron's Jacobian 1 / |p|^3 at the texel center (p the point on the
	// octahedron, |p.x| + |p.y| + |p.z| = 1) normalized so a tile's add up to 4 pi (LAB/gather3d.comp's weight).
	glm::vec3 GetDirection(uint32_t u, uint32_t v, uint32_t a);
	float GetSolidAngle(uint32_t u, uint32_t v, uint32_t a);
}
```

**The two layouts** (`Renderer.GlobalIlluminationLayout` checks every number):

| | High | Low |
| --- | --- | --- |
| Voxel levels | 4, each 128 x 64 x 128 voxels | 3, each 64 x 32 x 64 voxels |
| Voxel sizes | 0.10, 0.20, 0.40, 0.80 m | 0.20, 0.40, 0.80 m |
| Level extents (x y z) | 12.8 x 6.4 x 12.8; 25.6 x 12.8 x 25.6; 51.2 x 25.6 x 51.2; 102.4 x 51.2 x 102.4 m | 12.8 x 6.4 x 12.8; 25.6 x 12.8 x 25.6; 51.2 x 25.6 x 51.2 m |
| Probe spacing s0 | 0.5 m | 1.0 m |
| Cascade grids | 48 x 40 x 48, 24 x 20 x 24, 12 x 10 x 12, 6 x 5 x 6 (all 24 x 20 x 24 m) | 24 x 20 x 24, 12 x 10 x 12, 6 x 5 x 6 (24 x 20 x 24 m) |
| Directions | 16, 64, 256, 1,024 | 16, 64, 256 |
| Intervals (m) | [0, 0.5], [0.5, 2], [2, 8], [8, out of the clipmap] | [0, 1], [1, 4], [4, out] |
| Lead-in (m) | 0, 1.3, 2.6, 5.2 | 0, 2.6, 5.2 |
| Minimum level per cascade | 0, 0, 1, 2 | 0, 0, 1 |
| Top cascade spacing (the field's snap) | 4 m | 4 m |
| Rays per frame | 2,764,800 (1,474,560 + 737,280 + 368,640 + 184,320) | 322,560 |
| Full field around the camera (D13.5) | 7.5 m horizontally, 5.5 m vertically; fades to none over 2 m | 7 m, 5 m; 2 m fade |

Why these numbers:

- **Level 0 at 10 cm** (High): the look-dev room's walls are 0.2 m, two voxels: a wall at least two voxels thick is opaque to every
  march at 0.75-voxel steps and keeps its inner and outer faces' light apart (D7). Low's 20 cm makes them one voxel: they stay opaque
  (a trilinear sample within 0.6 voxel of a one-voxel wall's center reads at least 0.4, times the gain 2.5 is 1) but go two-sided
  (dark, D7). The research's 10-12.5 cm in rooms, 25 cm at Low.
- **Half-height levels** (128 x 64 x 128, not 128^3): half the memory; what is above or below a level's height is in the next level,
  and the forward pass's visibility reads levels 0-2 (D13.4), whose level 2 holds the whole probe field at both qualities.
- **0.5 m probes, intervals x4, spacing x2, directions x4** (the research: the lab and Split RC agree; Split RC found 2x direction
  growth measurably worse in 3D). Cascade 0's interval ends at s0, so its rays reach the next probes.
- **The field's extent, 24 x 20 x 24 m**, is the first knob the stage-2 cost decides (section 11, question 1). It is snapped to the
  top cascade's 4 m so every cascade's probes stay on their world lattice (LAB step 20: probes that move with the camera change the
  light at a fixed point); the vertical 20 m keeps the ground under a third-person camera (3-5 m up) inside the full field.
- **The top cascade marches until it leaves the outermost level** (High: up to about 77 m from the camera), the research's miss rule:
  only rays that leave the world read the sky.

**Memory** (`GlobalIlluminationLayout::GetMemoryBytes`, the row's traits report it; identical at every output size):

| Resource (D4) | Format | High | Low |
| --- | --- | --- | --- |
| Voxel levels: material, emission, normal, radiance | 20 B per voxel | 83,886,080 (80.0 MiB) | 7,864,320 (7.5 MiB) |
| Voxelization scratch (one 64^3 batch, or one Low level) | 24 B per voxel | 6,291,456 (6.0 MiB) | 3,145,728 (3.0 MiB) |
| Cascade atlases | RGBA16F | 22,118,400 (21.1 MiB) | 2,580,480 (2.5 MiB) |
| Probe weights (cascades but the top) | 2 x RGBA8 per probe | 840,960 | 103,680 |
| Probe validity (every cascade) | RGBA8 per probe | 421,200 | 52,560 |
| The field (cascade 0's ambient cubes) | RGBA16F, 6 per probe | 4,423,680 (4.2 MiB) | 552,960 |
| Tile list (32,768 tiles) | 8 B per tile | 262,144 | 262,144 |
| Work items and batches (65,536 each) | 16 B and 8 B | 1,572,864 | 1,572,864 |
| **Total per view that runs GI** | | **119,816,784 (114.3 MiB)** | **16,134,736 (15.4 MiB)** |

The editor's Scene and Game views each hold their own (D14.1): 228.6 MiB with both at High. For comparison, 0.7.0's High
reflections take 232 MiB at 3840x2160.

**Constants** (`R/SceneRenderData.h` `RenderData::c_GI*`, mirrored as `ST_GI_*` defines in `SH/Include/GlobalIllumination.slang`
with a comment naming the C++ constant). Values marked *guess* are measured where named and changed with their numbers in the
commit:

| Constant | Value | Meaning, reason |
| --- | --- | --- |
| `c_GIScrollVoxels` | 4 | a level's window moves in steps of 4 voxels: fewer, larger slabs |
| `c_GILevelGuardVoxels` | 2 | a level is never sampled within 2 voxels of its window's faces (trilinear reads one beyond; the snap moves the camera up to 4 off-center) |
| `c_GILevelBlendVoxels` | 8 | samples blend into the next level over 8 voxels, by distance from the camera (D3.4) |
| `c_GIRegionBrick` | 4 | dirty boxes snap outward to 4-voxel bricks |
| `c_GIBatchVoxels` | 262,144 | a batch's regions together hold at most 64^3 voxels (the scratch) |
| `c_GISmallTriangleVoxels` | 64 | a triangle whose bounds within its region hold more voxels goes to tiles (D5.3) |
| `c_GITileVoxels` | 8 | a tile is 8^3 voxels, a group of 64 threads with 8 voxels each |
| `c_GIMaxTiles` | 32,768 | tiles per batch; more: the batch's regions are split and queued again (counted) |
| `c_GIVoxelizeTrianglesPerFrame` | 500,000 | *guess*, 6.1: the voxelization's share of the stage at most 0.5 ms on the 5090 |
| `c_GIVoxelizeTrianglesOnCut` | 4,000,000 | *guess*, 6.1: a cut frame's voxelization of `ow.scene` |
| `c_GIMaskedOccupancy` | 0.15 | *guess*, 6.2: alpha-masked surfaces' occupancy (the grass's open view 0.6-0.9) |
| `c_GIOccupancyGain` | 2.5 | LAB's `OCC_GAIN` (its step 3f): a sample's absorption is `saturate(occupancy * 2.5)`, so a wall two voxels thick is exactly opaque |
| `c_GITwoSidedSum` | 0.5 | a voxel whose triangles' unit normals sum to less is two-sided (D7) |
| `c_GINormalFixedPoint` | 256 | normals add as integers times 256 |
| `c_GIShadowStartVoxels` | 2.4 | LAB `inject.comp`: shadow marches start 2.4 voxels off the surface (acne on curved walls) |
| `c_GIStepVoxels` | 0.75 | march steps in voxels of the level sampled (LAB) |
| `c_GITransmittanceCutoff` | 1e-4 | below it a ray is opaque (LAB's 0.02 would let 2% through a sealed wall's last sample) |
| `c_GILeadInSpacings` | 1.3 | the lead-in in the cascade's spacings: a child's trilinear parents sit at most 0.75 sqrt(3) = 1.3 parent spacings away (D11) |
| `c_GIRadianceScale` | 1/16 | stored radiance is absolute luminance times this, clamped to 60,000 (half floats; 100,000 lux on white is 31,831 nits) |
| `c_GISkyMip` | 4 | the environment cube's level top-cascade rays read (32 texels a face, about a 1,024-direction texel) |
| `c_GIGatherNormalBias` | 0.25 | *guess*, 6.2: the forward pass's lookup point moves 0.25 s0 along the geometric normal |
| `c_GIGatherStartVoxels` | 1.5 | *guess*, 6.2: visibility marches start 1.5 voxels off the surface (a 45-degree plane's voxels stick out 0.87 voxel) |
| `c_GIFieldFadeMeters` | 2.0 | the field fades to the fallback over 2 m at its edge (D13.5) |
| `c_GIMaxBounceAlbedo` | 0.9 | re-injection's albedo is clamped below it (the research: "clamp albedo below 1") |
| `c_GISettleFrames` | 6 | *guess*, 6.4: frames of re-injection before `GlobalIlluminationSettled` |
| `c_MaxCaptureGIFrames` | 16 | `viewport.capture` and the project picture wait at most this long |

### D3 The voxel clipmap

#### D3.1 Windows and snapping

Level `l` covers a window of `Dimensions` voxels of `v_l` meters, in world voxel indices (`int64` on the CPU, from double):
`c = floor(cameraPosition / v_l)`; the window's minimum index `W_l = 4 * floor((c - Dimensions / 2) / 4)` per axis
(`c_GIScrollVoxels`). The camera is then between `Dimensions / 2` and `Dimensions / 2 + 4` voxels from the window's minimum face:
centered within 4 voxels (the research: centered on the camera; EggyStudio's ahead-of-eye volume leaked the sky behind the camera).

#### D3.2 Toroidal addressing and anchors

A world voxel index `w` is stored at texel `w mod Dimensions` (non-negative modulo): moving the window rewrites only the cells that
entered. Textures sample with the linear repeat sampler (`u_Samplers[0]`), so trilinear filtering wraps with the storage.

Coordinates uploaded per level (D14.4 and the GI constants): the window's minimum corner relative to the camera,
`float(double(W_l) * v_l - cameraPosition)` per axis, and its texel `W_l mod Dimensions` (`int`). A point `p` (camera-relative)
reads texel coordinate `(p - originRelative) / v_l + texel`, divided by `Dimensions`. Voxelization works relative to a fixed
**anchor** per level, `A_l = 1024 * v_l * floor(W_l / 1024)` (world meters, a float of the CPU's double): the instance's float
translation minus the anchor's float is exact (they are within a factor of two: Sterbenz), so a region voxelizes to the same voxels
whatever the camera's position when it does (`Renderer.GlobalIlluminationLayout`: "voxels are independent of the camera").

#### D3.3 Regions, scrolling and validity

`R/VoxelClipmap.h/.cpp` (`class VoxelClipmap`, CPU, main thread; `Renderer.VoxelClipmap` tests it with a render scene and no GPU)
keeps per level the window, a queue of **regions** (boxes of world voxel indices) and the **valid box**:

- **Scroll:** a new `W_l` queues the slabs that entered (per axis, the difference of the two windows), split at texel wraps so each
  piece is contiguous in the textures, cut into pieces of at most 64^3 voxels. A move of at least `Dimensions` on any axis queues the
  whole window. The valid box shrinks on the faces with queued slabs until they are done (slabs always lie at faces).
- **Changes** (D5.1) queue the boxes that changed, snapped outward to 4-voxel bricks, clipped to the window.
- **Rebuilds** (the first frame, a cut, `ResetHistory`, a new layout, another render scene that is not a clone of the last) empty the
  valid box and queue the whole window of every level.
- **Order:** changes (level 0 first), then slabs (level 0 first), then rebuilds (level 0 first). The frame takes regions in that
  order while their triangles (D5.1) fit `VoxelizeTrianglesPerFrame` (`VoxelizeTrianglesOnCut` on a rebuild's frame; 0: no limit, for
  single images), at least one region a frame.
- **Pending:** a frame is `GlobalIlluminationPending` while levels 0-2's valid boxes do not hold their windows (the field then has
  weight 0: the frame shows today's fallback), or proxies wait for residency (D5.1).

#### D3.4 Which level a sample reads

`SampleVoxels(p, minimumLevel)` (`Include/GlobalIllumination.slang`) reads the finest level at or above `minimumLevel` whose valid
box holds `p` with `c_GILevelGuardVoxels` to spare, blended with the next level over `c_GILevelBlendVoxels` by the distance from the
camera (the origin of camera-relative space), not from the snapped window: per axis the margin
`m = halfExtent - (c_GIScrollVoxels + c_GILevelGuardVoxels) v_l - |p|`, the level's weight `saturate(min(m) / (8 v_l))`. As the camera
moves the weights change continuously, so no sample switches level in a step when a window scrolls (6.2's "no steps" case). Samples
outside every level read nothing (occupancy 0).

### D4 Resources and formats

Made by `EnsureGlobalIlluminationResources(const GlobalIlluminationLayout&)` on the first frame that wants GI, released when a frame
does not want it, on a new layout (`SetQuality`) and with the render targets (`Renderer::ReleaseDeferred`). Textures: `isUAV`,
initial state `ShaderResource`, `keepInitialState`, debug names `<renderer>.GI.<name>`.

| Resource | Per | Format | Written by | Read by |
| --- | --- | --- | --- | --- |
| `Material[l]` | voxel, `Dimensions` | `RGBA8_UNORM` 3D: rgb albedo (linear, diffuse: `BaseColor * (1 - Metallic)` times its map's mean), a occupancy | `VoxelFinalize` | marches (trilinear), `VoxelInject`, forward visibility (levels 0-2), the `voxels` view |
| `Emission[l]` | voxel | `R32_UINT` 3D: RGB9E5 of emission times coverage (nits) | `VoxelFinalize` | `VoxelInject` |
| `Normal[l]` | voxel | `RGBA8_SNORM` 3D: xyz the surface's normal, w 1 one-sided, 0.5 double-sided material, -1 two-sided, 0 no surface (interior or empty) | `VoxelFinalize` | `VoxelInject`, marches (facing), the `voxels` view |
| `Radiance[l]` | voxel | `RGBA16_FLOAT` 3D: rgb outgoing radiance times `c_GIRadianceScale` times a, a the surface weight (1 for a lit surface voxel) | `VoxelInject` | marches (trilinear: `rgb / a`, LAB `cascade3d.comp`) |
| `ScratchKeys`, `ScratchWinding`, `ScratchEmission`, `ScratchNormal` | batch voxel | raw buffers of `uint`: packed albedo and class; signed winding steps; packed emission; three normal sums | `VoxelClear`, `VoxelTriangles`, `VoxelTiles`, `VoxelFill` | `VoxelFill`, `VoxelFinalize` |
| `WorkItems`, `Batches`, `Regions` | frame | structured buffers (D5.2) | the CPU (`writeBuffer`, at most 65,536 items and batches) | `VoxelTriangles` |
| `Tiles`, `TileArguments` | batch | `uint2` per tile; `DispatchIndirectArguments` | `VoxelTriangles` | `VoxelTiles` |
| `Validity[k]` | probe of cascade k | `RGBA8_UNORM` 3D: r validity | `ProbeValidity` | `ProbeWeights`, `CascadeMarch`, `FieldReduce`, forward (k = 0) |
| `Weights[k]`, k < C - 1 | probe | 2 x `RGBA8_UNORM` 3D: the 8 parents' weights | `ProbeWeights` | `CascadeMarch` |
| `Atlas[k]` | ray | `RGBA16_FLOAT` 3D, `(Grid.x a_k, Grid.y a_k, Grid.z)`: probe-major `a_k x a_k` tiles (LAB's layout); rgb merged radiance (scaled), a merged transmittance to the sky | `CascadeMarch` (k) | `CascadeMarch` (k - 1), `FieldReduce` (k = 0) |
| `Field` | cascade-0 probe | `RGBA16_FLOAT` 3D, `(3 Grid.x, 2 Grid.y, Grid.z)`: face f at `(3x + f % 3, 2y + f / 3, z)`, faces +X, -X, +Y, -Y, +Z, -Z; rgb irradiance over pi (scaled), a the face's sky visibility | `FieldReduce` | forward, `VoxelInject` (stage 4), the views |

### D5 Voxelization from the geometry pool

#### D5.1 What is voxelized, and when (CPU)

`VoxelClipmap::BuildFrame(const RenderScene&, const RenderSceneGPU&, ...)` runs in the extraction (`Render.Extract`, as one
`JobSystem` job beside the view's culling, merged before `GatherDraws`):

- **Which proxies:** indexed and active proxies (`RenderProxyFlags::Indexed`, `Active`) that are resident (`RenderSceneGPU::IsResident`)
  with a loaded mesh (mesh table entry not 0), in the Static arena (posed skinned proxies, `Skinned`, are left out above the cut: D8),
  whose material is neither transparent nor transmissive (`c_CullMaterialTransparent`). `CastShadows` does not matter: a mesh that
  casts no sun shadow still bounces and occludes light.
- **Regions to proxies:** `GetSpatialIndex().QueryPlanes` with the region's six planes (world box from the anchor). For the solid fill
  (D6) a second query takes closed proxies below the region in its column prism (D6).
- **Work items:** one per (proxy instance, submesh) that intersects a region: the instance, the mesh slot, the submesh, the level of
  detail (for open meshes the coarsest whose `MeshLOD::Error` times the instance's largest scale is at most half a voxel, else LOD 0;
  closed meshes take LOD 0, D6), the material slot, the region, flags (closed, alpha-masked, double-sided, mirrored, fill only). Its
  triangle count is the level's index count over 3 from the CPU's mesh layout; the frame's batches cut items into 64 triangles.
- **Changes** (each frame, before the queue is taken): the proxy log (`GetProxyChanges` since the clipmap's version) gives changed
  proxies; each queues the box it was voxelized with (kept per proxy, `m_VoxelizedBounds`) and its new world bounds, in every level
  whose window they touch. The render scene GPU copy's `GetTablesVersion` changing compares the clipmap's copy of the mesh table
  entries and material data (`GetMeshTableEntry`, `GetMaterialData`) slot by slot; the proxies of changed slots (`CollectMaterialUsers`;
  for meshes a walk of the proxies' mesh slots, rare) queue their bounds. Proxies left out for not being resident or loaded are kept
  in a waiting list and queue their bounds when they become so (`GlobalIlluminationPending` meanwhile). Geometry pool compaction moves
  no voxel (offsets are read every frame) and queues nothing.

#### D5.2 GPU records

```cpp
// RenderData (SceneRenderData.h): the voxelizer's inputs, uploaded per frame.
struct VoxelRegion           // 48 bytes
{
	glm::ivec4 Minimum;        // xyz: the region's first voxel, from the level's anchor; w: the level
	glm::ivec4 Maximum;        // xyz: one past its last voxel; w: its first voxel in the batch's scratch
	glm::ivec4 Texel;          // xyz: the texel of Minimum (contiguous: regions are split at wraps); w: unused
};
struct VoxelWorkItem         // 16 bytes
{
	uint32_t Instance;         // Into the render scene GPU copy's instances (world rows, material slot)
	uint32_t MeshSubmeshLOD;   // Mesh slot (bits 0-19), submesh (20-27), level of detail (28-31)
	uint32_t Region;           // Into the batch's regions
	uint32_t Flags;            // c_VoxelItem*: Closed, AlphaMasked, DoubleSided, Mirrored, FillOnly
};
struct VoxelBatch            // 8 bytes: a group of 64 triangles
{
	uint32_t Item;
	uint32_t FirstTriangle;
};
```

The GPU resolves vertices through the tables every frame: `handle = meshTable[slot]`, `entry = entries[handle]`,
`submesh = submeshes[entry.FirstSubmesh + s]`, `lod = lods[submesh.FirstLOD + min(level, submesh.LODCount - 1)]`, vertex
`positions[entry.FirstVertex + lod.BaseVertex + indices[entry.FirstIndex + lod.IndexOffset + i]]` (the pool's position buffer, 12-byte
stride, read as a raw buffer). Before the first dispatch the stage calls `GeometryPool::RecordTableUploads` when the frame's
GPU-driven culling has not (it writes only changed entries).

#### D5.3 Passes

Per batch (regions holding at most `c_GIBatchVoxels`; a frame may run several batches one after another, with barriers):

1. **`GI/VoxelClear.comp`** (64 threads, one voxel each): zeros the batch's scratch.
2. **`GI/VoxelTriangles.comp`** (64 threads, one triangle each; a direct dispatch over the batch's `VoxelBatch` records): the three
   vertices through the instance's world rows into voxel coordinates relative to the level's anchor (D3.2), divided by `v_l`. For
   `Closed` items, the winding steps first (D6). `FillOnly` items stop there. A triangle whose bounds miss the region stops. The rest:
   - **Small** (its bounds within the region hold at most `c_GISmallTriangleVoxels` voxels): every voxel of those bounds that the
     triangle overlaps (Akenine-Moller's separating-axis test, LAB `voxelize_mesh.comp`'s `tri_box_overlap`) gets the triangle's
     writes (below).
   - **Large:** every 8^3 tile of its bounds within the region whose box the triangle overlaps (the same test, half size 4) is appended
     to `Tiles` (`InterlockedAdd` on the count, which is `TileArguments.x`): `(batch triangle index, tile origin packed 10:10:10)`.
     LAB step 18: one thread walking a 220 m road triangle's 65,000 voxels cost 11.6 ms; tiles spread the same work over groups.
3. **`GI/VoxelTiles.comp`** (`dispatchIndirect`, 64 threads a tile, 8 voxels each): the same test and writes per voxel.

**The writes** of a triangle to a voxel (`Include/Voxelize.slang`, all into the scratch with buffer atomics):

- **Key** (`InterlockedMax`): bits 30-31 the class (3 opaque, 1 alpha-masked; 2 is the fill's), bit 29 double-sided, bits 0-23 the
  albedo as linear RGB8. The largest key wins whatever the order: opaque over masked, then the larger color code (creikey's
  voxel-radiance-cascades resolved overlapping objects with `atomicMax` on a packed word for the same reason: flicker). Albedo is the
  material's `BaseColor.rgb * (1 - Metallic)` (a metal bounces no diffuse light, LAB) times its base color map's mean: the map sampled
  at its coarsest level (`SampleLevel(u_Textures[slot], 0.5, 16)`: the 1x1 tail, always resident, so streaming never changes a voxel).
- **Emission** (`InterlockedMax` of RGB9E5): `Emissive.rgb` times the emissive map's mean times the triangle's **coverage**: its area in
  voxel faces over the voxels it touches in one layer, times 1.5 for the ring the conservative test adds, at most 1 (LAB step 19: an
  18 cm lens in 25 cm voxels glowed like one twice its size).
- **Normal** (`InterlockedAdd` x3 of `int(n * 256)`): the triangle's unit normal, counter-clockwise front faces (glTF), negated for
  mirrored instances; double-sided triangles add nothing.

The tri-box test, the tile walk and the packings are this stream's; the tri-box form and the winding (D6) are ported from LAB (MIT,
D19). Instances are world rows of floats as every pass reads them (`InstanceData`, 64 bytes, unchanged).

#### D5.4 Determinism and precision

Integer adds and packed maxima make the scratch independent of thread order; the anchor makes it independent of the camera (D3.2);
positions 20 km out keep 2 mm (`Renderer.GlobalIlluminationLayout` and `GPU.SceneRenderer.GlobalIllumination`'s 20 km case).

### D6 Solid fill and closedness

Probes inside walls must die (the research, R1: "Fill closed meshes solid so that probes in walls die"; EggyStudio drops probes deep in
walls "so a sealed room stays dark"). Interiors are found by the winding number along vertical columns (LAB `voxelize_mesh.comp`'s
`winding_steps` and `fill_solid.comp`, MIT, turned to columns along +Y):

- **Closedness** (`MeshUtils::IsClosed(positions, indices, submeshes)`, computed once in `Mesh::Create` and `CreateSkinned` over every
  submesh's LOD 0 together): positions welded by exact equality (glTF splits vertices at seams with equal positions), then every
  directed edge (a, b) must meet exactly one (b, a) and no edge may repeat. A closed mesh sets `RenderData::c_GeometrySubmeshClosed`
  (`1u << 1`) on all its submeshes in `Mesh.cpp`'s layout upload (361-376), beside `c_GeometrySubmeshLODErrors`. The CPU's clipmap
  reads the same flag from the mesh (`Mesh::IsClosed()`), and closed meshes are voxelized at LOD 0 (a simplified level may open).
- **Steps:** for each closed triangle, every column (voxel center line along +Y, the center moved off the half-voxel grid by
  (0.00137, 0.00291) so no edge lies on a line, LAB) whose (x, z) the triangle covers under the top-left rule gets +1 where the line
  enters the mesh and -1 where it leaves, at the first voxel above the crossing, `InterlockedAdd` into `ScratchWinding`. Mirrored
  instances negate the step (their winding is reversed). A crossing below the region adds at the region's first layer; one above
  adds nothing. So the fill needs every closed triangle **below** the region in its column: the CPU queries closed proxies whose bounds
  meet the region's x and z ranges and lie below its top (`FillOnly` items when they miss the region itself).
- **`GI/VoxelFill.comp`** (one thread per column of the region): sums the steps upward; a voxel whose sum is not 0 and whose key has
  no class gets class 2 (interior: occupancy 1, albedo 0, no normal). Its color never shows: an interior voxel has no empty neighbor,
  so no ray reads its light; it only blocks rays and kills probes.

### D7 Finalize, normals and sides

**`GI/VoxelFinalize.comp`** (one thread per region voxel) writes the level's textures at the region's texels:

- `Material`: albedo from the key; occupancy 1 (class 3 or 2), `c_GIMaskedOccupancy` (class 1), 0 (none).
- `Emission`: the packed word as it is.
- `Normal`: with triangles (class 3 or 1), `s = sum / 256`; `|s| >= c_GITwoSidedSum`: xyz `normalize(s)`, w 1; below it, xyz 0 and
  w -1 (**two-sided**: a voxel holding both faces of a wall thinner than it, or a box seen from two sides); a double-sided key: xyz
  `normalize(s)` or 0, w 0.5. Interior and empty voxels: 0.

Why normals from triangles, not from empty neighbors (LAB `inject.comp`): a ground plane's voxels have empty neighbors above and
below, so the neighbors' normal cancels and the most important bounce source goes dark; the triangles' normals say up. A two-sided
voxel gets no light (D9): a thin wall goes dark from both sides rather than letting its sunlit face's light into the room. Walls at
least two voxels thick keep their faces' voxels apart. Double-sided materials (foliage, thin sheets) are lit from either side: a leaf
lit from above bounces from below as a translucent leaf does; a double-sided sheet used as a wall between a sunlit and a dark room
leaks (DG says so, section 10).

### D8 Moving and skinned objects

- **Moving rigid objects** are proxies like any other: a move queues their old and new bounds in every level they touch (D5.1), at
  the front of the queue, so the voxels follow in the frame they moved. Occupancy is whole voxels: a ball rolling past a wall changes
  its voxels 10 cm at a time. The soft march (trilinear occupancy) and the soft probe weights (D10) smooth that over a voxel of
  movement; 6.2's moving-prop case measures what is left and triggers 5b (R5's fixes, in the research's order) if it pops.
- **Posed skinned proxies are left out** above the cut: characters neither occlude nor bounce light in the GI; they receive it from
  the field like every surface, and GTAO darkens their contacts. 5c voxelizes them from their palettes.

### D9 Light injection

**`GI/VoxelInject.comp`** (4x4x4 threads, over every voxel of every level, every frame; voxels without a surface write zero and stop
after one load). For a surface voxel (`Normal.w` 1 or 0.5; two-sided -1 writes zero light but keeps its occupancy):

- `n` its normal; for a double-sided voxel `n` is turned toward the light (`dot(n, l) < 0`: `-n`; zero: `l`).
- **The sun:** `E = DirectionalLightColor * max(0, dot(n, l)) * T`, `l = -DirectionalLightDirection`, `T` the transmittance of a march
  toward the sun from `center + n * c_GIShadowStartVoxels * v_l` through `SampleVoxels` from the voxel's own level up (one voxel of the level
  sampled per step, `T *= 1 - saturate(occupancy * c_GIOccupancyGain)`, stop under `c_GITransmittanceCutoff` or out of the clipmap). The cascade
  shadow maps are not used: they cover the view, and hits behind the camera need the sun too (the research, R1).
- **Emission:** `Emission` unpacked.
- **Bounce** (stage 4, D17): `albedo * Field(p, n) * BounceGain`.
- **Radiance** = `albedo * E / pi + emission + bounce` (luminance; the forward pass's units before pre-exposure), times
  `c_GIRadianceScale`, clamped to 60,000; alpha 1.

It runs whole every frame: moving the sun re-lights every level in the frame it moved (the owner's "same frame"), and the research's
first number to measure ("a whole-clipmap re-light when the sun moves") is simply the stage's steady cost. Local lights: 5a (D18.1).

### D10 Probe cascades: placement, validity, weights

- **Placement:** the field's minimum corner `F` is snapped to the top cascade's spacing (4 m) in world space, centered on the camera:
  `F = 4 * floor((cameraPosition - extent / 2) / 4)` per axis (double). Cascade k's probe (i, j, l) sits at
  `F + ((i, j, l) + 0.5) s_k`. All cascades cover the same 24 x 20 x 24 m; a parent's center is the center of its 2x2x2 children, so a child's
  trilinear parents carry weights 0.25 and 0.75 per axis (erhe's layout). The probes are recomputed every frame; the snap keeps every
  probe at a fixed world position while the field covers it, so the light at a fixed point does not depend on the camera (LAB step 20).
- **Validity** (`GI/ProbeValidity.comp`, every cascade): `v = 1 - saturate(occupancy * c_GIOccupancyGain)` of `SampleVoxels` at the
  probe (cascade k from level `MinimumLevel`). `v = 0`: the probe is **dead** (inside a wall or a filled solid); its rays are not
  marched and it writes zero (radiance and transmittance). Continuous, so a probe a moving object covers fades rather than switching.
- **Weights** (`GI/ProbeWeights.comp`, every cascade but the top; LAB `probe_weights.comp`): for each of the 8 trilinear parents in
  cascade k + 1, `trilinear * v_parent * T(child, parent)`, `T` the soft march between the two probes (one voxel steps, the gain).
  Stored once per probe; every ray of the probe shares them. A wall between a child and a parent zeroes that parent.

### D11 The march, the lead-in and the miss rule

**`GI/CascadeMarch.comp`** (64 threads a group, one ray each; cascades top down, C - 1 to 0). For probe `p`, direction
`d = GetDirection(u, v, a_k)` (fixed, never jittered):

```hlsl
if (validity(p) == 0) { write(0, 0); return; }                 // a dead probe
float T = 1;
// The occlusion lead-in (cascades k >= 1): what lies just before this interval, in front of a child probe whose own interval
// ended short of it, may hold a wall; light is not gathered here (the child's interval holds that light), only occlusion.
for (t over [max(0, t0 - LeadIn), t0) in steps of 0.75 voxel of the level sampled)
	T *= 1 - saturate(SampleVoxels(p + d t, MinimumLevel).Occupancy * 2.5);
float3 L = 0;
for (t over [t0, t1) in steps of 0.75 voxel; the top cascade until the sample leaves every level)
{
	VoxelSample s = SampleVoxels(p + d t, MinimumLevel);       // trilinear occupancy, radiance rgb / a, normal
	float a = saturate(s.Occupancy * 2.5);
	if (a > 0)
	{
		float facing = s.NormalW == 0.5 ? 1 : smoothstep(-0.15, 0.25, dot(-d, s.Normal)); // LAB facing_weight: back faces send nothing
		L += T * a * s.Radiance * facing;
		T *= 1 - a;
		if (T < 1e-4) { T = 0; break; }
	}
}
float4 rest = (T == 0) ? 0
	: k == C - 1 ? (leftTheClipmap ? float4(Sky(d), 1) : 0)
	: Merge(p, u, v, k);                                         // D12
write(L + T * rest.rgb, T * rest.a);
```

- **The miss rule** (the research, R2): a ray that ends its interval without being absorbed is transparent: it continues into the
  parents (D12), never into the sky. Only the top cascade, marching until it leaves the outermost level, reads the sky.
- **The sky:** `u_EnvironmentMap.SampleLevel(linear clamp, RotateEnvironmentDirection(d), c_GISkyMip) * EnvironmentParams.x` with an
  environment, `AmbientColor` without. It holds what today's irradiance map is filtered from, so open ground under the sky reads
  today's sky light (6.2's first case), the procedural sky's disk included as today.
- **The lead-in** (this stream's, *inference*, section 12): a child probe's interval ends at `C + t_k d`, its parent's begins at
  `P + t_k d`, up to 1.3 parent spacings further along `d` (the trilinear parents' offsets): a wall in that gap blocks neither, and
  the parent's sunlit outside leaks into the child. The lead-in marches occlusion over that gap. It double-counts partial occluders
  (foliage) a little: a darkening, never a leak. 6.2's sealed room fails without it (checked once).
- **Level per sample:** `SampleVoxels` from the cascade's `MinimumLevel` (0, 0, 1, 2): steps grow with the level sampled (the research's
  pointer to creikey's level by distance; walls never thin at coarser levels, the conservative test thickens them).
- **Steps** (High, guesses to measure): cascade 0 about 7 a ray, 1 about 20 + 17 lead-in, 2 about 40 + 17, 3 about 100 + 17: about 80 M
  samples a frame.

### D12 The merge

`Merge(p, u, v, k)` = the parents' merged light, weighted: `sum_n w_n * (1/4) sum_{children c of (u, v)} Atlas[k+1](parent n, c) /
sum_n w_n`, with `w_n` from `Weights[k]` (D10); the children of texel (u, v) are `(2u..2u+1, 2v..2v+1)` of the parent's tile (LAB
`sample_upper`, erhe's layout). Merged values carry transmittance in alpha: `merged.rgb = L + T upper.rgb`, `merged.a = T upper.a`
(erhe's merge), so `Atlas[0].a` is the share of each cascade-0 direction that reaches the sky. No visible parent (`sum w < 1e-4`):
`(0, 0)`, dark, never sky. erhe's per-neighbour ray and the bilinear fix (Osborne and Sannikov) are not built: the lead-in and the
weights cover their cases at a fraction of the cost, and 6.2's offset sweep says if they do not.

### D13 The irradiance field and the forward pass

#### D13.1 The reduce

**`GI/FieldReduce.comp`** (one thread per cascade-0 probe): per face axis `a_f` (+X ... -Z),
`E_f / pi = (1 / pi) sum_d Atlas[0](p, d).rgb max(0, dot(d, a_f)) Omega(d)` and the sky visibility
`V_f = (1 / pi) sum_d Atlas[0](p, d).a max(0, dot(d, a_f)) Omega(d)`, `Omega` from `GetSolidAngle` (D2, a table of 16 in the GI
constants). A dead probe writes zeros. This is an ambient cube (Valve's; LAB `ambient.comp` computes it for its bounce), not
Split RC's and erhe's 6x6 octahedral map: 16 directions carry no more than six cosine lobes, and it is 48 bytes a probe instead of
512 (section 11, question 2).

#### D13.2 `SampleIrradianceField`

`Include/GlobalIllumination.slang`, called by the forward pass (opaque, glass and blended variants alike: no screen data), by
`VoxelInject` (stage 4, with last frame's field origin) and by the views:

```hlsl
// The field's irradiance over pi at a surface point (camera-relative), for a shading normal; .a the field's weight there (0
// outside it or while pending). openShare receives the cosine-weighted share of the sky the point sees.
float4 SampleIrradianceField(float3 position, float3 geometricNormal, float3 shadingNormal, out float openShare)
{
	float f = FieldWeight(position);                                    // D13.5, times GIFieldSize.w (0 while pending)
	if (f <= 0) { openShare = 1; return 0; }
	float3 q = (position + geometricNormal * c_GIGatherNormalBias * s0 - GIFieldOrigin.xyz) / s0 - 0.5;
	int3 base = floor(q); float3 t = q - base;
	for pass in 0, 1:                                                    // pass 1 only if pass 0 found no weight (LAB gather3d)
		for the 8 corners c (skip those outside the grid):
			float v = Validity[0](base + c);                             if (v == 0) continue;
			float3 toProbe = probe(base + c) - position;
			float h = smoothstep(-0.05, 0.25, dot(normalize(toProbe), geometricNormal)); // behind the surface's plane: none
			float vis = pass == 0 ? VisibilityMarch(position + geometricNormal * c_GIGatherStartVoxels * v_L, probe) : 1;
			float w = trilinear(c, t) * v * h * vis;
			E += w * AmbientCube(base + c, shadingNormal); V += w * SkyVisibility(base + c, shadingNormal); W += w;
	openShare = W > 0 ? V / W : 0;
	return float4(W > 0 ? E / W : 0, f);                                 // no weight at all: dark, never the sky
}
```

- `AmbientCube(p, n) = n.x^2 E(+-X) + n.y^2 E(+-Y) + n.z^2 E(+-Z)`, the faces picked by the signs: three `Load`s.
- `VisibilityMarch` (D13.4) returns the transmittance from the start to the probe.
- Back-facing probes get no weight (`h`): a probe behind the pixel's plane is on the other side of the wall the pixel lies on.

#### D13.3 Units and the seam

The field holds irradiance over pi (the radiance of a white Lambertian surface), scaled by `c_GIRadianceScale`; the forward pass
multiplies by `1 / c_GIRadianceScale` and works with it in absolute luminance, as with every other light, until its final write
multiplies by the pre-exposure (`Forward.frag.slang:410`). Diffuse inside the field (stage 3, D16):
`fieldDiffuse = E/pi * albedo * (1 - metallic) * (1 - SpecularColor in every form but the ambient) * diffuseOcclusion * intensity`,
scattered by `ScatterDiffuse`.

#### D13.4 Visibility in the forward pass

`VisibilityMarch(start, probe)`: from the start (1.5 voxels off the surface along the geometric normal) to the probe, one voxel steps
of the finest valid level among 0-2 holding the start (SRVs 38-40), `T *= 1 - saturate(occupancy * 2.5)`; under 0.05 it is 0. A cell
diagonal is at most 0.87 m at High: at most 9 samples, about 5 on average, for the probes in front (about half). LAB's gather skipped
the first three voxels (a voxel staircase sticking out of the pixel's own surface); that would leave 0.3-0.45 m of wall unchecked at
every floor-wall junction of a sealed room, so this starts 1.5 voxels out instead (a 45-degree surface's voxels stick out 0.87 voxel,
trilinear blur adds 0.5). 6.2 checks a 45-degree slope keeps at least 6 of 8 probes.

#### D13.5 The field's edge and the fallback

`FieldWeight(p) = saturate(min over axes (H - |p|) / c_GIFieldFadeMeters) * GIFieldSize.w`, `H` per axis the half extent the snapped
field always holds around the camera, minus a probe: High `12 - 2 - 0.5 = 9.5` m horizontally, `10 - 2 - 0.5 = 7.5` m vertically
(`GIFieldFade.xyz`). It is measured from the camera (the origin), not the snapped field, so it moves continuously (a 4 m jump of the
field changes nothing at a point inside `H`). Outside the field and in its fade, today's forms supply the rest: reflection probes whose
boxes hold the point, then the sky (S0.11's `GatherIndirectDiffuse`), so beyond the field the image is today's.

### D14 Frame integration

#### D14.1 Gating

```cpp
// RenderFrame, after the reflections' decisions (2935-3022).
const GlobalIlluminationLevel giLevel = m_Specification.Quality.GlobalIllumination;
const bool giAsked = postProcess.GlobalIllumination && giLevel != GlobalIlluminationLevel::Off;   // stage 3
const bool giViewShown = IsGlobalIlluminationView(options.LightingView);                           // 6, 7, 8, 9
const bool giWanted = giAsked || giViewShown;
// A view at a quality without GI shows what High gives.
const GlobalIlluminationLevel layoutLevel = giLevel != GlobalIlluminationLevel::Off ? giLevel : GlobalIlluminationLevel::High;
```

`m_GIFrame = giWanted && EnsureGlobalIlluminationResources(layout)`; a failed allocation reports once and the frame runs without GI
(degraded). A frame with `!giWanted` releases the GI (`ReleaseGlobalIllumination`). Each renderer keeps its own clipmap and field
(its camera's); the editor's two views hold two. Orthographic cameras work as perspective ones (the field centers on the camera's
position: an editor top view far above the scene sees the fallback; DG says so).

#### D14.2 Pass order

In `GPU.RadianceCascades`, after `GPU.AO` and before `GPU.Reflections` (3376), on frames with `m_GIFrame`:

1. per batch of the frame's regions: `VoxelClear`, `VoxelTriangles`, `VoxelTiles` (indirect), `VoxelFill`, `VoxelFinalize`;
2. `VoxelInject` (every level; reads last frame's field in stage 4, before step 5 overwrites it);
3. `ProbeValidity` (every cascade), `ProbeWeights` (every cascade but the top);
4. `CascadeMarch`, cascade C - 1 down to 0;
5. `FieldReduce`.

The voxels and probes need neither the prepass nor the frame's culling; the stage sits where step 0 assigned it. The `voxels` and
`probes` views draw in `GPU.Overlays` (D15).

**The GPU stage:** `GPU.RadianceCascades`, one stage for voxelization, injection, probes and reduce (the quality row names one stage;
step 0 assigned the name), registered in `RendererGPUStages` after `GPU.AO` and added at the same position to the five lists DG "GPU
timing" names (OLD's hunks). The forward gather's cost shows in `GPU.Opaque` (and `GPU.Transparent`). Sub-pass times for the commits
come from temporary timers, not committed (as the sun shadows' costs were measured).

#### D14.3 Barriers and bindings

- Between consecutive dispatches that touch the same buffer or texture, explicitly `setBufferState`/`setTextureState(...,
  UnorderedAccess)` (or `ShaderResource` for a texture read next) and `commitBarriers()` (AGENTS.md's NVRHI pitfall, which a batch's
  passes over one scratch meet): Clear, Triangles, Tiles, Fill and Finalize on the scratch; Tiles' arguments to `IndirectArgument`
  before `dispatchIndirect`; each `Atlas[k]` to `ShaderResource` before cascade k - 1 reads it; `Field` before the forward pass.
- Compute layouts (one per pipeline): `VolatileConstantBuffer(0)` the frame constants, `VolatileConstantBuffer(1)`
  `RenderData::GlobalIlluminationConstants` (levels: origins relative to the camera, texels, valid boxes, anchors; the field: origin,
  previous origin, grids; the sun; sky flags; `BounceGain`; the solid-angle table), push constants at b2 (at most 64 bytes:
  cascade, directions, spacing, interval, lead-in, minimum level, batch), samplers s8 (linear clamp) and s0 (linear repeat). The
  voxelizer's layouts add the render scene GPU copy's instances, mesh table and materials, the pool's position, index and table buffers,
  the texture table, and the bindless table as set 1 (as the scene pipelines take it). `CascadeMarch` binds each level's `Material`,
  `Normal`, `Radiance` (12 SRVs at High), `u_EnvironmentMap`.
- **The scene layout** gains five SRVs, in all four scene binding sets (the pattern of 29, 32, 33 at 1068-1070), each a dummy
  (`Renderer::GetBlack3DTexture()`) without GI: **35** `Field`, **37** `Validity[0]`, **38-40** `Material` of levels 0-2. `Scene.slang`'s
  slot comment names them ("35 the GI field, 37 its probes' validity, 38-40 its voxel levels 0-2"; next free 41). If another stream
  took 37-40 first, the integrator renumbers (the slot comment is the arbiter). The rebind check gains `m_BoundGIField` and the four
  others (3175-3180).
- The overlay passes (`voxels`, `probes`) take a layout of their own with the frame constants at b0 and their SRVs at slots no layout
  beside them holds (4ba9dbe3).

#### D14.4 Frame constants appended

```cpp
// Global illumination (SceneRendererGlobalIllumination.cpp; Include/GlobalIllumination.slang): what the forward pass reads.
glm::vec4 GIFieldOrigin;     // xyz: the field's minimum corner, camera-relative (cascade 0's probe (i, j, l) at + (i, j, l) + 0.5 spacings); w: s0
glm::vec4 GIFieldSize;       // xyz: cascade 0's probes per axis; w: 1 when the field is used this frame (ran, not pending), else 0
glm::vec4 GIFieldFade;       // xyz: the half extents the field always holds around the camera (m); w: the fade's width (m)
glm::vec4 GIVoxelOrigin[3];  // levels 0-2: xyz the window's minimum corner, camera-relative (m); w: voxel size (m), 0 when not valid
glm::vec4 GIVoxelTexel[3];   // levels 0-2: xyz the minimum corner's texel; w: unused
glm::vec4 GIVoxelDimensions; // xyz: voxels per axis; w: unused
```

`static_assert(sizeof(FrameConstants) == 8 * 64 + 36 * 16)`; `Scene.slang` mirrors them. Zero without GI. `ReflectionParams.y`
(stage 3): 1 when the scene asked, the frame ran GI and the field is used; `.z` the intensity, clamped to [0, 4] (non-finite: 1).
`AOParams` is untouched.

#### D14.5 Pending, settling, stats

```cpp
// SceneRendererStats, beside SSR's: global illumination (world-space radiance cascades; DG "Global illumination").
bool GlobalIllumination = false;        // The frame ran it: a scene asked at a row other than Off, or a GI view showed
bool GlobalIlluminationPending = false; // Its voxel levels 0-2 were incomplete or proxies waited: the frame showed today's light
bool GlobalIlluminationSettled = true;  // c_GISettleFrames frames in a row with no voxel work, no change of sun, sky or field (true without GI)
uint32_t GIVoxelizedTriangles = 0;      // Triangles of the frame's work items
uint32_t GIQueuedRegions = 0;           // Regions left for later frames
```

`SceneRendererGPUStats` (2-3 frames late, `ResolveGPUStats`) adds `GIDeadProbes`, `GILiveProbes` (cascade 0), `GISurfaceVoxels`,
`GITiles`, `GITileOverflow`, counted by `ProbeValidity`, `VoxelInject` and `VoxelTriangles` into a small counter buffer read back in
the GPU stats' ring. Accessors: `GetGlobalIlluminationLayout()`, `GetGlobalIlluminationMemoryBytes()` (0 without), and for tests and
tools only (blocking): `ReadGlobalIlluminationVoxels(level, ...)`, `ReadGlobalIlluminationField(...)`, `ReadGlobalIlluminationValidity`
(through `Renderer::ReadTexture`, which gains a depth-slice argument for 3D textures).

#### D14.6 Quality, releases and single images

- `SetQuality`: a changed `GlobalIllumination` releases the GI at once (`ReleaseGlobalIllumination`, beside
  `ReleaseScreenReflections`); the next frame that wants it rebuilds (D3.3).
- `ForThumbnails()` and `ForBake()` take Medium's rows: GI off, nothing changes there.
- The capture renderer (`viewport.capture`) and the project picture set `VoxelizeTrianglesOnCut = 0` (no limit: their first frame is
  complete) and wait for `GlobalIlluminationSettled` (at most `c_MaxCaptureGIFrames`).

#### D14.7 Gate W (end of stage 2)

Numeric, on the views, so the join waits for nobody's look: every 6.2 room case (the owner's five experiments, the paired rooms, the
offset sweep), the fence (`Blocks` at most 0.10%, `BlocksP99` at most 1.0%, no AA and TAA, 3840x2160) and grass (`Blocks` at most
0.2%) strafes on the `gi` view, cost as information (`GPU.RadianceCascades` ms alone and over `GPU.AO`, the room, the yard,
`tp/ow/pp.scene`). The flicker bounds are *guesses* (0.7.0's GTAO High reads 0.074% / 0.71% on the fence, 2228a8b2): the commit
replaces each by 1.5 times the measured value when that is lower, never higher. On a miss: one tuning day (`c_GIGatherNormalBias`,
`c_GIGatherStartVoxels`, `c_GILeadInSpacings`, `c_GIMaskedOccupancy`, `c_GILevelBlendVoxels`), each change measured on the failing case
and the others; then the orchestrator decides (section 3).

### D15 Debug views and statistics

- **`gi`** (`SceneLightingView::GlobalIllumination = 6`, `ST_LIGHTING_VIEW_GLOBAL_ILLUMINATION 6u`): the forward pass shades white
  (albedo 1, no emission, as `Lighting`) and returns only diffuse indirect light: `field * f + today's diffuse * (1 - f)`, through
  the scene's exposure. It shows exactly what the join adds on a white surface. Statistics: EV100 of surfaces, as the light views.
- **`open`** (`OpenShare = 7`, a value view: `IsLightingValueView` and `LightingView.frag`'s gray, like `ao`): the cosine-weighted
  share of the sky the surface sees through the world (`openShare` times f plus 1 - f: today's light counts everything open).
  Statistics: `ValueMedian`, `ValueP05`. A sealed room reads 0, open ground 1.
- **`voxels`** (`Voxels = 8`): `GI/VoxelView.frag` in `GPU.Overlays`, replacing the finished image (as `LightingView.frag`): from the
  camera, a march through `SampleVoxels` (0.75 voxel steps, at most 512) to the first sample of occupancy at least 0.5; stage 1 shows
  its albedo shaded `0.25 + 0.75 max(0, dot(n, -view))` (sRGB-encoded for display); from stage 2 its radiance (`rgb / a`) through the
  frame's exposure and a display curve `x / (1 + x)`, with albedo times 0.03 where no light arrives (dark geometry stays legible);
  two-sided voxels magenta; nothing hit: dark gray. Legend: "Two-sided: no bounce" (magenta), and a note "levels 4, 10-80 cm, valid 4 of
  4". The forward pass shades the lit image for it (`GetForwardLightingView` returns `None`).
- **`probes`** (`Probes = 9`): `GI/ProbeView.frag` in `GPU.Overlays`, over the finished image: per pixel a 3D DDA through cascade 0's
  grid (at most 96 cells) testing a sphere of 0.12 s0 at each probe, nearer than the pixel's depth: a live probe shaded with its
  ambient cube at the sphere's normal through the exposure and the display curve, a dead one dark red (0.6, 0.05, 0.05). Legend:
  "Live probe" (white), "Dead: inside a solid" (red), and a note from the GPU stats, for example "probes 92,160, dead 4,210; field 24 x
  20 x 24 m".
- **Editor:** `ViewportDebugView` gains `GlobalIllumination`, `OpenShare`, `Voxels`, `Probes`, names `gi`, `open`, `voxels`, `probes`
  (after `albedo`); chip labels "GI", "Open", "Voxels", "Probes"; popup entries "GI: indirect light on white", "Open: the share of
  sky each surface sees", "Voxels: the scene as the GI sees it", "Probes: the GI's probes, dead ones red"; the chip's tooltip gains ",
  or the GI's light, sky share, voxels and probes". `DebugViewSchema` and both commands' descriptions list them; the core toolset's
  `viewport.capture` description gains at most 120 bytes (measure the margin first, `EditorMcpToolsetsTests.cpp:69`; the full wording
  goes to `viewport.setSettings` and DG "View commands").
- **The capture's result** (`DescribeDebugView`): for the four views a `globalIllumination` object from the renderer: `{ "ran",
  "pending", "settled", "settledFrames", "level", "voxelLevels": [{ "voxelSize", "dimensions", "valid" }], "probeSpacing",
  "probeGrid", "cascades", "directions", "liveProbes", "deadProbes", "voxelizedTriangles", "queuedRegions", "memoryMiB" }`; every
  capture's result gains `globalIlluminationSettled`; `editor.perf`'s `renderStats` `globalIllumination: { ran, pending, settled }`.

### D16 Stage 3: the join

**The scene's ask** (`PostProcessComponent`, after the ambient occlusion fields):

```cpp
// Global illumination (world-space radiance cascades; DG "Global illumination"): bounce light and sky occlusion from a voxel copy
// of the scene around the camera, lit by the sun and glowing surfaces every frame, also from what is off screen; the quality's
// GlobalIllumination row scales it (off at Low and Medium).
bool GlobalIllumination = false;
float GlobalIlluminationIntensity = 1.0f; // The field's diffuse light, 0 to 4; 1 as traced
```

Properties in a section "Global illumination" after "Ambient occlusion": `GlobalIllumination` ("Bounce light and sky occlusion
traced in the world around the camera: sunlit floors light the walls, colored surfaces tint their neighbors, closed rooms stay dark,
whether the source is on screen or not. Graphics Quality High and Ultra; off at Low and Medium"), `GlobalIlluminationIntensity`
(`Range(0, 4, 0.01)`, "How much of the traced indirect light shades surfaces: 1 as traced"), its inactive reason "Global Illumination
is off". `ComponentSchema.json` regenerated.

**The row** (`stash@{1}`'s hunks): `GlobalIlluminationLevel { Off = 0, Low = 1, High = 2 }` (Ultra takes 3 when D18.5 lands); the
field after `MotionBlurQuality` with the comment "the world-space GI's layout for scenes that ask (Off at Medium: none runs); 15 MiB at
Low, 114 MiB at High per view, at any resolution"; `c_GlobalIlluminationRow = { Off, Off, High, High }`; lines in `GetPreset`, `Clamp`
(unknown to Medium's), `MakeSection` (Enum "GlobalIllumination", "Global illumination", options Off, Low, High, default High, a
description naming the stage and the memory), `FromSectionValue`, `ToSectionValue`; traits `{ "GlobalIllumination", true,
"GPU.RadianceCascades", &GlobalIlluminationBytes }` with `GlobalIlluminationBytes` = `ComputeLayout(level)->GetMemoryBytes()` (0 for
Off). DG "Quality settings": the row, its memory row, and `GlobalIllumination` in the list of rows where High is not Medium below 1440
rows (`RendererQualityTests.cpp`'s case extended). `c_SectionVersion` + 1 at landing (the integrator renumbers).

**The forward pass** (`ShadeLitSurface`, inside S0.11's regions; everything behind `if (u_Frame.ReflectionParams.y > 0.0)`):

1. `IndirectInputs` gains, appended: `float FieldWeight; float3 FieldIrradiance; float FieldOpen;`, `GetIndirectInputs` initializes them
   to 0, 0, 1 (its signature does not change).
2. When joined: `field = SampleIrradianceField(surface.Position, geometricNormal, surface.Normal, open)`; `FieldWeight = field.a`,
   `FieldIrradiance = field.rgb / c_GIRadianceScale * ReflectionParams.z` (absolute, D13.3). The field's share takes
   `inputs.DiffuseOcclusion` (material times GTAO, no multi-bounce: real bounce replaces the fit); today's share keeps
   `DiffuseOcclusionColor` as it is, so outside the field nothing changes.
3. `GatherIndirectField(inputs, surface)` (new, `IndirectLight.slang`) returns D13.3's `fieldDiffuse`; the compose line becomes
   `indirect = ComposeIndirect(inputs, GatherIndirectDiffuse(inputs, surface) * (1 - FieldWeight), GatherIndirectSpecular(inputs)) +
   GatherIndirectField(inputs, surface) * FieldWeight;` before the clear coat (which then covers it). `ComposeIndirect`'s sky scale
   applies to today's share only; the field's light already holds the sky's intensity.
4. The transparent and glass variants call the same (no screen data is needed): glass scatters it by its diffuse weight.
5. Specular is untouched (`reflections`' region): the field's open share enters specular occlusion below the cut (D18.4).

Without the branch the code is S0.11's exactly; with GI on and the field's weight 0 (pending, or far away) the light is today's
(`x * 1.0` and `+ 0.0` are exact: 6.3's "pending equals off" case).

**Settling for pictures:** `viewport.capture` and `ProjectThumbnail` wait while `GlobalIllumination && !GlobalIlluminationSettled`, at
most `c_MaxCaptureGIFrames` (beside SSR's pending waits); look-dev A/Bs go through the capture.

**Feature test:** `Feature.stscene`'s first Post Process `GlobalIllumination: true`, `GlobalIlluminationIntensity: 1.25`;
`FeatureTest.stproj`'s `Renderer.Quality` (Custom) `"GlobalIllumination": "Low"`; `GPU.FeatureTest`'s stats check
`GlobalIllumination` true and `GlobalIlluminationPending` false from the first frame.

### D17 Stage 4: multi-bounce by re-injection

`VoxelInject` adds `bounce = min(albedo, c_GIMaxBounceAlbedo) * Field(center + n * 0.5 v_l, n) * BounceGain` to every surface voxel
inside last frame's field, read with last frame's origin (the field may have moved 4 m) through `SampleIrradianceField`'s weights
with its visibility march (a wall's inner voxel at a floor junction would otherwise take the outside's probes: the sealed room again).
The first bounce arrives in the frame the light changes; each further bounce one frame later (the research, R4). It is world data fed
back, not a blend of GI buffers (D7). `BounceGain` defaults to 1; the reference cases set 0 (one bounce, like the reference).
`GlobalIlluminationSettled` counts `c_GISettleFrames` frames since the last change (voxel work, sun, sky, the field's move), the value
measured by 6.4 and set at most 8.

### D18 Below the cut

1. **Local lights** (5a): `VoxelInject` loops over the render scene's light records (`RenderSceneGPU::GetLightRecordBuffer`) whose
   range reaches the level's window (a list built per frame, at most 64, nearest the camera first, LAB step 18), with the forward
   pass's attenuation and cone, shadowed by a voxel march to the light (stop `0.5 v` short of it). Test: the look-dev room's lamp
   lights the ceiling's bounce within 30% of a CPU one-bounce reference of a point light (`SunRoomReference`'s integrator with the
   lamp's irradiance).
2. **R5 for moving objects** (5b, only after 6.2's trigger): moving proxies (changed in the last 30 frames) go into a separate dynamic
   occupancy with fractional coverage per voxel (4x4x4 point samples per voxel against the triangles' closed winding, or the
   oriented box for boxes: LAB `voxelize_boxes.comp`), the shadow and probe marches read `max(static, dynamic)`; each fix in the
   research's order with its failing-first case.
3. **Posed skinned meshes** (5c): the voxelizer blends the palette (`BlendJoints`) for `Skinned` proxies (the skin stream from the
   Skinned arena), queued every frame they pose; their fractional occupancy as 5b.
4. **The open share in specular occlusion** (5d, after `reflections` merged): the sky's share of the specular light times the field's
   sky visibility along the reflected vector, weighted by f; probes and SSR untouched.
5. **Ultra** (5e): `GlobalIlluminationLevel::Ultra = 3`, the field 96 x 40 x 96 at 0.5 m (48 m wide), memory and cost measured.
6. **Textured albedo per voxel** (5f): the base color map sampled at the voxel center's barycentric UV at the voxel's footprint level
   (`UVDensity`), with the streaming entry's finest resident level; voxels change when textures load (a table version).
7. **Probe relocation** (5g, erhe's and DDGI's lesson): dead or near-wall probes move within their cell to open space.
8. **Hardware ray queries** (5h, R6): a second tracer for the same probes with `rt::AccelStruct` where the device has it; exact on thin
   walls; BLASes, skinned refits and material fetch at hits.

### D19 What is ported, from where

| Source | Licence | What | Where it goes |
| --- | --- | --- | --- |
| radiance-cascades-lab, Daniel Quevedo (2026), `LAB/voxelize_mesh.comp`, `fill_solid.comp`, `inject.comp`, `cascade3d.comp`, `probe_weights.comp`, `gather3d.comp`, `ambient.comp` | MIT (checked) | the triangle-box test, the winding steps and the column fill, the emission coverage, the shadow march's start, the soft density march with its gain and facing weight, the 4-children average of the merge, the probe weights, the gather's 8 probes with visibility and its second pass, the ambient cube | `Include/Voxelize.slang`, `GI/VoxelTriangles`, `VoxelTiles`, `VoxelFill`, `VoxelInject`, `CascadeMarch`, `ProbeWeights`, `FieldReduce`, `Include/GlobalIllumination.slang` |
| erhe, tksuoran, `doc/plans/radiance_cascades.md` and its shaders | MIT | the merge's transmittance (`merged.a = raw.a * upper.a`), the parents' 0.25 / 0.75 layout, the leak stations (paired rooms, the probe-offset sweep) and the 0.0017 bound | `CascadeMarch`, 6.2's cases |
| creikey/voxel-radiance-cascades | MIT | levels coarser with distance along a ray; overlaps resolved by a maximum on packed words | `SampleVoxels`, `Include/Voxelize.slang` (ideas; the code is ours unless the implementer ports a function, then noticed) |
| tmpvar/radiance-cascades-3d-grid (2024, Elijah Insua) | MIT (checked) | octahedral probe atlases (read; the engine's own `OctahedralDecode` is used) | none unless ported |
| Akenine-Moller, "Fast 3D triangle-box overlap testing" (2001) | a paper | the separating-axis test | through LAB's form |
| Split RC (Freeman, Sannikov, Pitt-Francis), chandra | CC BY-SA / none | ideas only: K = 4, l = 4, the miss rule | no code |
| EggyStudio 3DEngine | MPL-2.0 | ideas only: sealed rooms, dead probes, centering | no code |
| Sannikov's paper and videos | CC-BY-ND badge | ideas only | no code |
| three-rc, kurushimee, UERadianceCascades | all rights reserved / none | not read for code | none |

`ThirdPartyNotices.md`'s section for derived code gains the LAB and erhe notices (and creikey's or tmpvar's if a function is ported);
each commit that ports names the source files.

## 6. Tests per stage

Every GPU case ends with `CHECK(gpu.GetNewErrorCount() == 0)`; renders at 1280x720 with `ExactSpecification()` and
`AddNeutralPostProcess` unless named (the GI's work does not follow resolution; the 4K cases are the gate's). The room cases render
the `gi` view through `HDROutput` with a forced pre-exposure (`stash@{0}`'s `c_RoomPreExposure = 1/8192`) and read 3x3 pixels at each
point's projection (the stash's `Average`). Suites: `Renderer.*` in the Core shard `Renderer`, `GPU.*` in `StrataTests.GPU`,
`PerfGPU.*` in `StrataTests.PerfGPU`: no new shard.

### 6.1 Stage 1

`Renderer.GlobalIlluminationLayout` (CPU):

- "The High and Low layouts": every number of D2's table (levels, dimensions, sizes, extents, grids, directions, intervals, lead-ins,
  minimum levels, rays).
- "Memory adds up": `GetMemoryBytes` is 119,816,784 at High and 16,134,736 at Low, and the parts as D2's table.
- "Directions and solid angles": `GetDirection` gives unit vectors, a tile's 4 a^2 directions cover the sphere symmetrically, solid
  angles add to 4 pi within 1e-5, the four children of a texel lie within its parent's texel.
- "Windows snap and follow": for cameras at the origin, 1 km and 20 km out and at negative coordinates, `W_l` is a multiple of 4 and
  the camera lies 64-68 voxels (32-36 vertically) from its minimum face.
- "Scrolling queues exactly what entered": moves of 1, 3, 4, 5, 64, 127 and 300 voxels on each axis and diagonally: the slabs are
  disjoint, their union is the new window minus the old one exactly, each is contiguous in texels and at most 64^3; 128 or more is a
  rebuild.
- "Voxels are independent of the camera": a triangle's voxel coordinates from two cameras 37.3 m apart are the same bits (the anchor).
- "Level weights are continuous": a camera moving 1 mm changes no weight by more than 1e-3; the weight is 1 at the center and 0 past
  the band.

`Renderer.Mesh` (CPU): "Closed meshes are found": `MeshFactory`'s cube, sphere and cylinder are closed (with split normals: the cube's
24 vertices weld); the plane and quad are open; a cube without a face is open; two disjoint cubes in one mesh are closed; a cube with
one triangle doubled is open.

`Renderer.VoxelClipmap` (CPU, a `RenderScene` built from a scene, residency given by a test function):

- "A moved proxy queues its old and new bounds" in every level they touch, snapped to bricks, ahead of slabs; "a removed one its old".
- "A cut queues every window, level 0 first, within the cut's budget"; "the frame's triangles stay within the budget but for its first
  region".
- "Levels of detail follow the voxel size": an open mesh with levels of error 0, 0.04 and 0.2 m (scale 1) takes LOD 1 at levels 0
  and 1 (half voxels 0.05 and 0.1 m), LOD 2 at levels 2 and 3; a closed mesh LOD 0 everywhere.
- "What is left out": posed skinned, transparent, transmissive and non-resident proxies make no item; the non-resident one queues its
  bounds when it becomes resident.
- "Material and mesh changes requeue their users": a material slot's data changed in the GPU copy's tables queues its proxies' bounds.

`GPU.SceneRenderer.GlobalIllumination` (the `voxels` view; voxels read with `ReadGlobalIlluminationVoxels`):

- "A box voxelizes to its cells": a 1 m red cube (0.62, 0.09, 0.07) at a voxel-aligned position and at an offset of 0.37 voxel: the
  occupied voxels of levels 0 and 1 are exactly the CPU reference's (the same tri-box test in `T/Renderer/GIScenes.cpp` over its 12
  triangles) plus its interior; surface albedo within 1/255; interior occupancy 1, albedo 0, normal w 0.
- "A plane is open and faces up": a ground plane: one layer (two where it lies on a voxel face), normals +Y within 1 degree, nothing
  below it filled.
- "Normals face out, thin walls are two-sided": the cube's face voxels within 10 degrees of their faces (edges excepted); a 0.05 m
  closed slab at level 0 is two-sided (w -1).
- "A textured material's albedo is its map's mean": a 2x2 texture of known colors on a white factor.
- "Glow covers its area": a 0.18 m emissive square in High's level 1 (0.2 m voxels): the sum of its voxels' emission over one layer is
  within 20% of its emission times its area over a voxel face (LAB step 19).
- "A 220 m triangle costs its area": a 220 x 220 m ground quad (two triangles): its voxels are the plane's layers in every level, the
  tile count equals the CPU's count of tiles the plane crosses within the windows, no overflow, and (`MESSAGE`) the stage's
  milliseconds. Fails as an 11 ms frame without tiles (checked once with the threshold raised).
- "Scrolling keeps the voxels": the yard, a camera moving +x 0.05 m a frame for 100 frames, then +y and -z: after every frame every
  level's textures equal a fresh renderer's that rendered only that camera (bit for bit).
- "A moved object leaves no trace": a cube moved 1 m: its old voxels empty and its new ones filled in the same frame; equal to a fresh
  renderer.
- "A cut rebuilds in its frame": the camera jumps 500 m: not pending on that frame (the yard is small), voxels equal a fresh renderer's.
- "The budget spreads a rebuild": `VoxelizeTrianglesOnCut` 2,000 on the yard: pending for N frames (`GIQueuedRegions` falls), then
  equal to a fresh renderer's; the field's weight 0 while pending.
- "Paths are equal": `GPU.SceneRenderer.GPUDriven` gains "The GI's voxels": the CPU and GPU paths (occlusion culling on) give
  identical voxel textures after 4 frames.
- "20 km out": the yard 20 km from the origin gives the voxels of the origin's yard (same texels relative to the anchor).
- "Nothing is made without GI": a renderer that never shows a GI view: no GI memory, no `GPU.RadianceCascades` stage, the five SRVs
  bound to the dummy, `ReflectionParams.yz` and the appended constants zero; its 8-bit image equals that of a renderer that showed
  `voxels` for 4 frames and then none (released). The look-dev A/B (7.1) shows the default image unchanged against the branch point.
- "Turning the views off releases it": `voxels` shown, then none: memory 0 again; shown again: rebuilt (e7927fdd's case).

Editor: `GPU.Editor.Viewport` "A capture of the voxels view reports the GI" (`debugView.globalIllumination.voxelLevels` with four
levels, `memoryMiB` 114.3 at High); `Editor.Viewport` names and the open legend; `Editor.Commands` schema; `EditorProfilerTests` and
`FramePerfLogTests` list `GPU.RadianceCascades` after `GPU.AO`; `EditorMcpToolsets`' core size passes.

### 6.2 Stage 2

`GPU.SceneRenderer.GlobalIlluminationRoom`, the owner's experiments first (`BuildSunRoom` from `stash@{0}`: the look-dev room's shell
on its ground, sun (-40, 30) at 100,000 lux, no sky; `BounceGain = 0`, so one bounce like the reference; the stash's ten points on the
ceiling and walls, and nine floor points on a 3 x 3 grid inset 0.6 m):

1. "Walls and ceiling within 30% of the reference": the room camera of the stash (-9, 1.5, -4.5) toward (-10, 1.2, 1.1), 70 degrees:
   each of the ten points' luminance within +-30% of `SunRoomReference` (128 x 128 strata) at High (the bound); at Low the same table
   as information; `MESSAGE` the tables and the mean absolute errors. Fails without the sun's injection (zero). Cases 2-4 run at High.
2. "A red floor tints the walls": `FloorAlbedo` (0.62, 0.09, 0.07) against 0.58: the mean over the seven wall points of the change of
   R/G is at least 0.7 of the reference's change of R/G (stage 3 adds "none when GI is off" on the lit image).
3. "Moving the sun moves the bounce in the same frame": four frames at yaw 30, then one at yaw 60: the ten points within 1% of a fresh
   renderer's at yaw 60, and at least three points changed by more than 10% from yaw 30.
4. "The patch lights the walls from behind the camera": camera A at (-10, 1.6, 0.8) toward (-10, 1.6, -5.1) sees the floor patch and the
   north wall; camera B at (-10, 1.6, -3.0) toward the same point, the patch behind it: six north-wall points (x -12, -10, -8 at y 1.0
   and 2.0) within 2% between A and B, and within 30% of the reference.
5. "A sealed room stays dark": `SealWindow` fills the window with a 0.2 m plaster box: the nineteen points' mean luminance at most
   0.0017 of the open room's same mean, at High and Low, and with the room moved by 0.25 and 0.5 s0 along x and z (`Offset`: the probe
   planes cut the walls at three places, erhe's sweep). Fails without the occlusion lead-in (checked once, numbers in the commit).
6. "Paired rooms": room A (the sun room) and room B sealed beside it, sharing its 0.2 m east wall: B's mean at most 0.0017 of A's (erhe's
   `leak_pair`).
7. "No sky through misses": the sealed room under a uniform bright sky (constant ambient 1,000 nits, no sun): the interior at most 0.0017
   of the open ground outside.

Look-dev mirrors them so the owner can open the experiment in the editor: `LookDevScene.json` gains scenes `Scenes/SunRoom.stscene` and
`Scenes/SealedRoom.stscene` (the stash's boxes, the sun at (-40, 30), a comment naming `BuildSunRoom`: change both); `Shots.json` gains
`lookdev.sunroom`, `.view.gi`, `.view.open`, `.view.voxels`, `.view.probes`, `lookdev.sunroom.redfloor.view.gi` (a `Setup` step setting
the floor's material to `Materials/YardRed.stmat`) and `lookdev.sealedroom.view.gi`, at the stash's camera.

`GPU.SceneRenderer.GlobalIllumination` gains:

- "Open ground reads today's sky": the yard's ground alone, constant ambient, no environment: the `gi` view equals `AmbientColor` within
  3%; with the procedural sky: within 5% of today's `Indirect` view of the same ground made white; the `open` view's median 1 +- 0.02.
- "An emissive wall lights the floor": a 2 x 2 m wall of luminance 4 standing on a gray floor in the dark (OLD's case): the floor 0.5, 1
  and 2 m from its foot within 20% of `L F` (the rectangle's form factor, OLD's `GIScenes.cpp` function).
- "Dead probes": probes inside a 2 m closed block have validity 0, those 0.05 m from its face are in (0, 1), `GIDeadProbes` equals the
  count of probes whose centers lie inside it.
- "A slope keeps its probes": a 45-degree plane's pixels: pass 0 of the gather finds at least 6 of 8 probes (a test variant of the view
  that writes the count, `MESSAGE`).
- "Still frames are identical": frames 8 and 9 of a still camera, the `gi` view's HDR target bit for bit (no TAA).
- "The field does not step as the camera moves": a floor point in the yard, the camera moving +x 5 cm a frame for 200 frames (several 4
  m field jumps and level scrolls): the point's value's second difference over its value at most 0.5% at every frame.
- "The fence and grass strafes" (the regression form of Gate W, 1280x720, 14 frames, triples from frame 3, `FlickerSequence` on the `gi`
  view): fence `Blocks <= 0.10%`, `BlocksP99 <= 1.0%`, no AA and TAA; grass `Blocks <= 0.2%`. *Guesses*: the stage's commit lowers them to
  1.5 times the measured values when those are lower.
- "A moving prop" (information and 5b's trigger): a 0.5 m ball rolling 2 cm a frame 0.3 m beside 15 floor points (LAB's `--pop`): the
  mean and largest second difference over the value, `MESSAGE`; 5b becomes due when the largest exceeds 2%.
- "Paths are equal": `GPU.SceneRenderer.GPUDriven`'s "The GI views": the `gi` view identical on both paths after 4 frames.
- "The views report their statistics": `gi` EV100; `open` a value median in (0, 1) on the yard, below 0.01 in the sealed room.
- "Orthographic cameras": an orthographic camera 5 m above the yard runs the GI (`GlobalIllumination` true) and its `gi` view equals a
  perspective camera's at the same point within 2% at the image center.

`PerfGPU.GlobalIllumination` (Release, label `perf`; 7.1): the gate's fence and grass strafes at 3840x2160 (no AA, TAA), and the
stage's milliseconds in the sun room and the yard at High and Low (frames 10-29 of a still camera).

### 6.3 Stage 3

`GPU.SceneRenderer.GlobalIllumination` gains:

- "Off costs nothing": GI asked with the row Off, and not asked at High: no GI memory, stage or SRV other than the dummies, and the
  8-bit output of the two equal.
- "Pending equals off": GI asked, the first frame of a rebuild held pending (`VoxelizeTrianglesOnCut` 1): the 8-bit output equals the
  same frame not asked.
- "Beyond the field the light is today's, bit for bit": the yard's ground with `AddNeutralPostProcess`, GI asked against not asked: every
  pixel whose point lies outside the field's fade (more than 9.5 m from the camera horizontally) identical in the 8-bit output (`x * 1`
  and `+ 0` are exact). Fails if today's share is scaled or its occlusion changed outside the field.
- "The owner's experiment in the lit image": the sun room, GI asked: the `Indirect` view's R/G change on the seven wall points with the
  red floor at least 0.7 of the reference's change; GI not asked: below 0.01.
- "Inside the field the diffuse is the field's": white walls in the room: the `Indirect` view (diffuse part: roughness 1, metallic 0) equals
  the `gi` view times the albedo within 1%.
- "The field meets today's light at its edge": a flat ground 20 m long under the sky: along a line from 5 to 12 m from the camera, no step
  between neighboring pixels above 2% of the value (the fade).
- "Transparent surfaces take the field": a blended quad and a glass pane inside the sealed room read at most 0.0017 of the same in the open
  room.
- "Intensity scales the field": 2 doubles the field's diffuse within 1%; 0 leaves none inside the full field.
- "The field's share takes no multi-bounce": inside the full field, a corner's pixels are the same with `AmbientOcclusionMultiBounce` on
  and off when GI is joined (real bounce replaces the fit there; today's share keeps it).
- "The row follows SetQuality": Off, High, Low, Off: memory 0, 119,816,784, 16,134,736, 0.
- "Paths are equal" on the lit image (`GPUDriven`).
- "The white furnace": `GPU.SceneRenderer.Lighting`'s furnace cases with GI asked under a uniform sky: within 3% of without (a sphere in an
  open sky sees the sky).

`Renderer.Quality`: the row's presets, `Clamp`, the round trip of Off, Low, High, the traits line (bytes as D2), the "High is not Medium
below 1440 rows" case extended. `Renderer.Components`: the two properties, the inactive reason. `GPU.FeatureTest` and
`StrataTests.FeatureTest` with the values. `GPU.Editor.Thumbnails` "A project picture waits for GI to settle". `GPU.Editor.Viewport`
"A capture with GI waits for it" (`globalIlluminationSettled` true).

Look-dev (7.1's commands): the `.gi` shots against their GI-off shots at 3840x2160, no AA, FXAA and TAA, and the A/B base's `??.scene`.

### 6.4 Stage 4

- "Bounces fill the room": the sun room with `BounceGain` 1: every point at least its one-bounce value; the ceiling over the patch rises by
  at least 10% (*guess*); none above 2.5 times the reference (`MESSAGE` the table).
- "Bounce settles": the room and the yard after a cut, still camera: the first frame K from which every 32 px block's mean changes by less
  than 0.25% for four frames; K at most `c_GISettleFrames`, which the commit sets from the larger K (at most 8).
- "A sealed room stays dark with bounces": 6.2's case 5 with `BounceGain` 1, the same bound.
- "Still frames are identical": frames 30 and 31 bit for bit (section 11, question 6, if a rounding cycle shows).
- "An emissive panel lights a dark room": a closed room lit only by a 1 x 0.6 m emissive panel: the far wall brighter with bounces than
  without, the room's open share 0.
- The fence and grass strafes again with bounces (the same bounds); `PerfGPU.GlobalIllumination` re-run (information).

## 7. Deliverables and acceptance

- **Stage 1:** D2-D8, D15's `voxels` view, 6.1. *Accept:* every 6.1 case on both paths in Debug under the validation layer; the
  `voxels` view live in the Release editor on the yard, the room and `tp.scene` (window pictures in `lookdev/rc-world/stage1`).
- **Stage 2:** D9-D13, D15, 6.2. *Accept:* **Gate W** (D14.7) with its numbers in the commit; the views live on the room, the sun room
  scenes, the yard and `ow.scene` with its day cycle.
- **Stage 3:** D16, 6.3. *Accept:* 6.3 passes; GI off: `GPU.Opaque` and GPU busy within the pairs' spread of the base (7.1); look-dev
  before and after at 3840x2160 in no AA, FXAA and TAA; the owner's red floor live in the editor.
- **Stage 4:** D17, 6.4, section 8. *Accept:* 6.4 passes; **the owner's live look in motion** on Third-Person, the sun room and the
  showcase's yard (if landed), GI toggled through the Post Process in the rebuilt Release editor, with no AA, FXAA and TAA (a sheet of
  pictures cannot show flicker); our own look in motion first (decisions.md row 7).

### 7.1 Perf metrics and A/Bs

New `Budgets.json` entries, all `Provisional`:

| Metric | Unit | Target | Stage |
| --- | --- | --- | --- |
| `PerfGPU.GlobalIllumination.FenceFlicker4K`, `...4KP99`, `...4KTAA`, `...4KTAAP99` | ratio | 0.001, 0.01, 0.001, 0.01 | 2 |
| `PerfGPU.GlobalIllumination.GrassFlicker4K`, `...4KTAA` | ratio | 0.002 | 2 |
| `PerfGPU.GlobalIllumination.RoomStageGPU`, `YardStageGPU` (High) | ms | 2.0 (*guess*) | 2 |
| `PerfGPU.GlobalIllumination.RoomStageGPULow` | ms | 0.5 (*guess*) | 2 |
| `PerfGPU.GlobalIllumination.CutStageGPU` (a rebuild's frame, the yard) | ms | (none) | 2 |
| `PerfGPU.Renderer.GI4K`, `GI4KGPU`, `GI4KStageGPU` (the 100k cube grid, High, TAA, the Post Process asking) | ms | (none), (none), 2.0 | 3 |

**A/Bs** (information; no campaign): the base is main's Release at the branch point in a detached worktree
(`.worktrees/wave8-rc-world-base`, built Release in `build/agent-wave8-rc-world-base`, a build-heavy run). Under the lock, interleaved
base, branch three times, `LookDev.py capture --repeat 5` of `tp.scene`, `pp.scene`, `ow.scene` at 1920x1080 and 3840x2160, High, TAA:

```sh
python Tools/PerfLock.py --label rc-world -- python Tools/LookDev.py capture --build build/agent-wave8-rc-world-base/bin/Release \
    --projects ThirdPerson,PhysicsPlayground,OpenWorld --shots "??.scene" --sizes 1920x1080,3840x2160 --aa taa --quality high --repeat 5 \
    --output .worktrees/rc-world-ab/base-1
# then the branch's build into branch-1, and so on; LookDev.py compare base-N branch-N; LookDev.py ratio for stage ratios
```

- Stage 1 and 2: GPU busy without a view equals the base within the pairs' spread (off costs nothing); with the `gi` view (shots
  `??.scene.view.gi`): `GPU.RadianceCascades` alone and over `GPU.AO`.
- Stage 3: GI off: `GPU.Opaque` and busy within the spread; GI asked (`??.scene.gi`): `GPU.RadianceCascades`, `GPU.Opaque`'s growth (the
  gather), busy. Plan 1.1's busy row (at most 5 ms at High 3840x2160 on the samples' cameras with GI on) is read at integration, alone.

## 8. Documentation

- **DG, Rendering, new section "Global illumination"** (after "Ambient occlusion", anchor `#global-illumination`; first form with stage
  1, completed in stage 4): what it is (world-space radiance cascades over a voxel clipmap; no light from the screen; no history; the
  sources it was built from) and what it is not; the layout and memory tables; the clipmap (windows, toroidal addressing, anchors,
  regions, budgets, pending); voxelization (what enters, levels of detail, tiles, packing, closedness and fill, normals and sides,
  emission coverage); moving and skinned objects; injection; probes, validity, weights; the march, the lead-in, the miss rule, the sky;
  the merge; the field, the gather, visibility, the edge; the join and the seam; re-injection and settling; the four views and their
  statistics; `GlobalIlluminationSettings`; costs; the gate's commands; what it cannot do (walls thinner than two voxels go dark,
  double-sided walls leak, characters are not in it, emission maps count by their mean, textured albedo by its mean, the field reaches
  7.5 m around the camera at full weight, editor top views far above a scene show today's light). "Lighting": the seam's
  `GatherIndirectField`, the four views in the list. "View commands", "Viewport panel": names and legends. "GPU timing": the stage.
  "Quality settings": the row, memory, the 1440-rows list. "Batching": the closed flag of `GeometrySubmesh`. "Feature test": the values.
  "Perf tests": the suite. "Look-dev harness": the sun room scenes. "GPU tests": the flicker meter, the GI scenes, the sun room reference.
- **Architecture.md:** the frame's pass order (`GPU.RadianceCascades` after `GPU.AO`), the stage table, a paragraph on the GI's data flow
  (render scene GPU copy and geometry pool, clipmap, probes, field, forward pass).
- **Manual:** `Docs/Manual/Lighting.md` "Global illumination" before "Ambient occlusion" (what the property does, its cost, rooms need walls
  at least 20 cm thick at High and 40 cm at Low to stay lit from one side only, the four debug views, the first frames after a cut);
  `Docs/Manual/Graphics.md` the row and its memory.
- **AGENTS.md** (Rendering digest, stage 4; 248 characters): "- GI is world-space radiance cascades over a voxel clipmap of the geometry
  pool, never light from the screen or a GI history; check changes with the `gi`, `open`, `voxels` views and sun room cases
  ([GI](Docs/DeveloperGuide.md#global-illumination))." Stated in full in the DG (checklist item 5); `Docs.AgentGuide` passes.
- **Skills:** `strata-editor-automation` (the four views, `debugView.globalIllumination`, `globalIlluminationSettled`, the stage);
  `strata-make-a-game` (when to ask for GI, wall thickness, the cost).
- **Roadmap.md:** Status when it lands; "Later": 5a-5h, and that the screen-space form stays on `agent/wave8-radiance-cascades`.
- **ThirdPartyNotices.md:** D19.
- **Release notes** (0.8.0, what landed): "Global illumination (Post Process **Global Illumination**, **Intensity**): bounce light and sky
  occlusion traced through a voxel copy of the scene around the camera, lit by the sun and glowing surfaces every frame, including what
  is off screen; Graphics Quality's new **Global illumination** row (Off at Low and Medium, High at High and Ultra; Low is a players'
  choice); about X ms (measured) and 114 MiB per view at High, 15 MiB at Low, at any resolution; new debug views `gi`, `open`, `voxels`,
  `probes`; pictures and project thumbnails wait for it to settle. Off unless a scene asks: scenes saved before look the same."

## 9. Shared with other streams, and merges

| File | Ours | Theirs | Resolution |
| --- | --- | --- | --- |
| `SH/Scene/Forward.frag.slang`, `SH/Include/IndirectLight.slang` | the field's branch in `ShadeLitSurface`, `GatherIndirectField`, `IndirectInputs`' appended fields, the compose line, the two views | `reflections` (specular gather, head), `local-lights` (the light loop), `volumetric` (fog at the end), `showcase-engine` | S0.11's regions; re-run the pixel-equality check (GI off) after each merge |
| `SH/Include/Scene.slang`, `R/SceneRenderData.h` | SRVs 35, 37-40, views 6-9, the appended constants | `volumetric` 34, 36; `reflections` `ReflectionParams.w` | additive; the slot comment and the `static_assert` arbitrate; the integrator renumbers slots or appended order |
| `R/SceneRenderer.cpp/.h` | gating, the stage, SRVs, stats, members (grouped under "SceneRendererGlobalIllumination.cpp") | `local-lights`, `volumetric`, `editor-ux` | different functions; re-read `RenderFrame`'s order after each merge |
| `R/RendererQuality.*`, `RendererQualityTests.cpp` | the row after `MotionBlurQuality` | `reflections`, `local-lights`, `volumetric` rows | one line each in merge order, before the players' rows; the integrator renumbers `c_SectionVersion` |
| `R/GPUSceneData.h`, `R/Mesh.cpp`, `MeshUtils` | `c_GeometrySubmeshClosed`, `IsClosed` | none known | additive |
| `R/GPUProfiler.*` and the five stage lists | `GPU.RadianceCascades` after `GPU.AO` | `GPU.Volumetric` after `GPU.Shadows` | each at its position |
| `T/Renderer/FlickerMetrics.*` | OLD's | `reflections`' (if landed first) | main's wins (decisions.md) |
| `E/*`, `Panels/ViewportPanel.cpp`, `Tools/LookDev*` | views, legends, waits, shots, sun room scenes | `showcase`, `reflections` shots | appended lines; the selftest after merging |
| `Feature.stscene`, `FeatureTest.stproj`, `Budgets.json`, `Strata/CMakeLists.txt`, `ThirdPartyNotices.md` | values, entries, twelve shaders, notices | everyone's | additive |

## 10. Risks

| Risk | Mitigation |
| --- | --- |
| Cost above the guess (the march's 80 M samples, injection every frame, the forward gather's visibility) | measured in stage 2 alone and as ratios; the field's extent is the first knob (section 11, question 1); Low is an eighth of the rays; visibility steps of two voxels where walls are known thicker; far cascades every other frame (LAB step 6) only with the owner's consent (a frame of lag, no blend) |
| Voxelization spikes (cuts, fast cameras, large meshes, a 100k-triangle terrain under every slab) | budgets by triangles, pending shows today's light, the large-triangle tiles, `GIVoxelizedTriangles` and `CutStageGPU` measured; coarse levels' slabs are rare |
| Leaks through the interval gap and at junctions (the sealed room's 0.0017) | the lead-in, the weights' transmittance, the gather's visibility with no skipped voxels, dead probes, `c_GITransmittanceCutoff`; the offset sweep and paired rooms; erhe's per-neighbour rays as the next fix if a case fails |
| Walls thinner than two voxels (Low's 20 cm voxels, thin props) | two-sided voxels go dark rather than leak; documented with the wall thickness per quality; Low's sealed case checks occlusion |
| Double-sided walls leak | documented; rare in practice (foliage is double-sided by design) |
| Moving objects pop (whole-voxel occupancy) | soft march and weights now, the moving-prop case measures, 5b by its trigger |
| Characters absent from the GI | GTAO grounds them; 5c |
| The ambient cube is too coarse for the 30% bound near the patch | diagnosed by integrating the same probe's 16 radiances on the CPU; then a 3x3 octahedral irradiance per probe (memory x1.5 of the field) |
| Textured albedo is a material's mean | consistent and streaming-free; 5f |
| Specular sky reflections inside closed rooms (the specular gather is unchanged) | 5d after `reflections`; the owner may notice it first in the sealed room: it is the first item below the cut to raise |
| GTAO and the field both darken corners | the reference cases run without AO; stage 4's look checks corners with AO on; the AO's intensity is the scene's |
| Two views in the editor double the memory and cost | documented; a shared clipmap per scene is a later change |
| MoltenVK (3D storage images, buffer atomics, indirect dispatch, bindless in a compute layout) | only mandatory formats and buffer atomics; the S0.10 conversion check covers the twelve entries (the voxelizer's bindless read joins `c_KnownFailures` like every bindless entry); unverified on a Mac (decisions.md D9) |
| Half-float range | `c_GIRadianceScale` and the clamp; a 1,000,000-nit emitter is clamped (DG says so) |
| The sky's sun disk counted as sky light | the same as today's irradiance map; checked on `ow.scene` |
| Conversion slips in new Slang (vector `mul`, comparisons) | plan 4.1's rules; the room cases' exact numbers catch a wrong product |
| Usage: stages over their sizes | the cut line moves stage 4 below it; every stage leaves the branch buildable and tested |

## 11. Open questions and the decisions recommended

1. **The field's extent at High** (24 x 20 x 24 m, full weight within 7.5 m). *Decision:* keep; if stage 2 measures the probe passes
   under 1 ms, raise to 64 x 40 x 64 (32 m, full within 11.5 m, +7 MiB) in that stage; Ultra (5e) takes 96.
2. **Ambient cube or octahedral irradiance per probe.** *Decision:* ambient cube; switch only on 6.2's evidence (section 10).
3. **Skinned meshes** above or below the cut. *Decision:* below (5c); characters receive GI but do not occlude it.
4. **One GPU stage or two** (voxelization apart). *Decision:* one, `GPU.RadianceCascades` (the row names one stage; step 0 assigned it);
   `GIVoxelizedTriangles` explains spikes.
5. **A clipmap per renderer or per scene.** *Decision:* per renderer (its camera's); sharing is later.
6. **A rounding cycle under re-injection** ("still frames identical"). *Decision:* if one shows, `VoxelInject` rounds the bounce to a
   2^-10 relative grid before adding (deterministic, below visibility); the test stays.
7. **Local lights above the cut?** *Decision:* first below the cut (5a): the owner's experiments are the sun's; scenes lit by lamps keep
   today's indirect light until it lands, and the look-dev room's lamp does not bounce until then.
8. **OLD and its stashes.** *Decision:* keep untouched until 0.8.0 ships; then the orchestrator deletes or archives them.
9. **When the stream lands.** *Decision:* after stage 4 (or after stage 3 if stage 4 drops below the cut), GI off by default; samples
   adopt it only after the owner's live look (plan.md 7).
10. **The cut-frame budget** (4 M triangles may be a 20-50 ms hitch on large scenes). *Decision:* keep; games cut at loads and
    teleports, where a hitch is expected; the measured `CutStageGPU` decides whether to lower it.
11. **The irradiance volume** (plan item 2, slots 30-31). *Decision:* not built. If Gate W fails after its tuning day, the fallback is
    DDGI-style probes over the same voxels (the research), not baked SH volumes.

## 12. Departures from the research, the plan and OLD's spec

| Source says | Here | Why |
| --- | --- | --- |
| Research R1: 3-4 levels of about 128^3 | 128 x 64 x 128 | half the memory; height comes from coarser levels |
| Research R2: probes fade over the last ~6 spacings | a 2 m fade measured from the camera, not the snapped field | continuous as the field jumps 4 m |
| Research R2: erhe's per-neighbour ray or the bilinear fix later, if leaks show | an occlusion lead-in now | the sealed room's 0.0017 is an acceptance bound; the interval gap leaks by construction (*inference*, checked by 6.2) |
| Research R3: a 6x6 octahedral irradiance map per probe | an ambient cube per probe | 16 directions carry no more; 48 B instead of 512 B a probe |
| Research R3: per pixel from 8 probes with visibility | in the forward pass, not a separate full-resolution pass | normal maps and transparent surfaces get it; no 63 MiB target at 4K |
| LAB `gather3d.comp`: skip the first three voxels of a visibility path | start 1.5 voxels off the surface, skip none | three skipped voxels leave junction strips in a sealed room |
| LAB: normals from empty neighbors | normals from the triangles | a ground plane would go dark |
| LAB: rebuild the following volume whole on a move (11.6 ms) | toroidal levels, slabs, budgets | per-frame cost |
| LAB: temporal blend in the final pass | none | D7 |
| Plan 4.4 (screen-space design, G1's ratio limits) | world space (decisions.md 2026-11-03); Gate W | the owner's ruling |
| OLD's spec: SRV 35 a full-resolution texture; a view pass after the color copy | SRV 35 the field; the forward pass draws `gi` and `open` | the GI no longer reads the lit image, so the forward pass can show it |
| OLD's spec: `GlobalIlluminationOcclusion` | dropped | there is no screen open share scaling a fallback |
| OLD's spec: transparent surfaces keep the fallback | they take the field | it needs no screen data |
| OLD's spec: orthographic cameras run none | they run it | world space has no hemisphere through the view |

## 13. Deliver in this order

1. **Stage 1, in commits:** (a) the branch and the reused parts (2.3), with the 0.7.0 numbers of `IndirectFlicker` re-run on main's tip;
   (b) `GlobalIlluminationLayout` and its tests; (c) `MeshUtils::IsClosed`, the flag, `Renderer.Mesh`; (d) `VoxelClipmap` and its CPU
   tests; (e) resources, the stage, the SRVs with dummies, the appended constants (zero); (f) the voxelizer's passes and finalize; (g)
   the `voxels` view and the editor's names, chip and legend; (h) 6.1's GPU cases, the GPU-driven case; (i) the DG section's first form,
   Architecture.md, the five stage lists. Then the owner's note.
2. **Stage 2:** (a) injection; (b) validity and weights; (c) the march and merge; (d) the reduce and `SampleIrradianceField`, the `gi` and
   `open` views; (e) the `probes` view, statistics, the capture's object; (f) the room cases and the sun room look-dev scenes; (g) the
   strafes, `PerfGPU.GlobalIllumination`; Gate W and its report.
3. **Stage 3:** properties; the row (`stash@{1}`); the forward join and transparents; waits; feature test; perf entries; 6.3; the A/B and
   look-dev before and after.
4. **Stage 4:** re-injection; 6.4; the owner's live look; docs final; release notes; Roadmap.
5. *Cut line:* 5a-5h in order, each with tests, docs and an A/B.

If time runs short, stop after a complete stage; every stage leaves the branch buildable and tested.

## 14. First hour

1. Read `research/radiance-cascades-field.md` whole, decisions.md's 2026-11-02 and 2026-11-03 entries, plan.md 4.1, this spec, AGENTS.md's
   Rendering and Testing, DG "Batching", "Render scene", "GPU-driven path", "Camera-relative space", "Lighting", "Quality settings",
   "GPU timing", "Shaders". Then `LAB/voxelize_mesh.comp`, `fill_solid.comp`, `inject.comp`, `cascade3d.comp`, `probe_weights.comp`,
   `gather3d.comp`, `ambient.comp` and the lab's DEVLOG steps 2, 3f, 6, 18-21 (`.../godot-rc-lab/docs/DEVLOG.md`): read, never run.
2. `git -C .worktrees/wave8-radiance-cascades stash list` (stash@{0} is room-reference-wip, stash@{1} join-wip: check), then
   `stash show -p` both, and `git show 540d6b49 --stat` for the files 2.3 takes.
3. `sh .worktrees/create_worktree.sh wave8-rc-world main`; submodules; `cmake --preset windows -B build/agent-wave8-rc-world`; build
   Debug; run `StrataTests.exe --test-suite=GPU.SceneRenderer.LightingViews,GPU.SceneRenderer.GPUDriven,GPU.SceneRenderer.Lighting` so
   the baseline is green here. Check whether main already holds `FlickerMetrics` (reflections' R1) and which SRV slots are free (the
   slot comment).
4. Measure the core toolset's margin (`EditorMcpToolsetsTests.cpp:69`'s `defaultBytes`, a `MESSAGE` while working, not committed).
5. Start with commit (a), then `Renderer.GlobalIlluminationLayout`'s "The High and Low layouts" and "Memory adds up": the numbers every
   later pass and the row depend on.

## Review corrections

Independent review, 2026-11-03, against main at 7b2e136a, AGENTS.md, the DG, the research and the local copies of its sources
(read, never run). Each correction replaces the body's text it names and wins over the body and section 12.

1. **The field's snap leaves its fade without probes (D10, D13.5).** `F = 4 * floor((cameraPosition - extent / 2) / 4)` puts the
   camera 12 to 16 m above F, so on the + side of each axis the field reaches only 8 m past the camera (6 m vertically), while
   D13.5's `H` (High 9.5 and 7.5 m) assumes at most 2 m off center. Points 8-9.5 m out on a + side get `FieldWeight > 0` and no
   corner in the grid, and D13.2 returns black: a dark band that moves with the camera. Corrected: `F = 4 * floor((cameraPosition -
   extent / 2) / 4 + 0.5)` per axis (the nearest step: the camera within 2 m of the center), which makes D2's and D13.5's extents
   true. 6.3's edge case adds cameras every 0.5 m across one 4 m step on x and on y: every pixel with `FieldWeight > 0` has its eight
   corners in the grid (the slope case's count variant); it fails with the `floor` form.

2. **High's top-cascade lead-in misses grazing walls (D2, D11, section 12).** The gap runs from the child's end `C + t0 d` to the
   parent's start `P + t0 d`, but the lead-in marches along the parent's ray. A wall between those two points crosses that ray at
   `t*` with `t0 - t* < n.(P - C) / n.d`: more than 1.3 parent spacings for grazing rays (`n.d < 0.58` for a wall along the probes'
   axes, any `n.d < 1` for an oblique one). Where `LeadIn >= t0` the march starts at the parent and misses nothing; that holds for
   every cascade but High's top one (t0 8 m, lead-in 5.2 m: `[2.8, 8)`). A cascade-2 probe in a hall whose 8 m rays end inside it
   then merges the sunlit outside through the wall. The sun room cannot show it (no 8 m ray from a cascade-2 probe stays inside 6 x
   6 x 2.9 m), so case 5 passes either way. Corrected: `LeadIn = IntervalStart` for k >= 1 (D2's row: High 0, 0.5, 2, 8 m; Low 0, 1,
   4 m; `c_GILeadInSpacings` goes). Why it holds: the child's interval, the lead-in and the weight's `T(child, parent)` march three
   sides of the quadrilateral C, `C + t0 d`, `P + t0 d`, P, so a plane crossing the fourth side (the gap) crosses a marched one. The
   lead-in absorbs only where occupancy is at least 0.3, so alpha-masked voxels (0.15) are not attenuated twice. D11's cascade-3
   estimate becomes about 100 + 27 steps. 6.2 gains case 8, "A sealed hall stays dark": a 16 x 3 x 16 m plaster hall with 0.2 m
   walls on the sunlit ground at High, its interior mean at most 0.0017 of the same hall's with a 2 x 1.3 m window, with the
   `Offset` sweep; checked once to fail with the 5.2 m lead-in.

3. **Trilinear radiance leaks through walls one to two voxels thick (D2, D7, D11).** The march reads `Radiance` and `Normal` where it
   reads occupancy. Where a wall's inner-face and outer-face voxels are adjacent (walls between one and two voxels thick: 0.1-0.2 m
   at High, Low's 0.2 m walls off its 0.2 m grid; D2's "two-sided, dark" holds only when both faces share a voxel), a ray whose first
   occupied sample reads 0.25-0.4 takes its next sample up to 0.15 voxel past the inner voxel's center: absorption 1, 15% of the
   radiance from the sunlit outer voxel (`rgb / a` renormalizes), and a blended normal that still faces the ray. Up to 1.4% of the
   outer face's radiance enters (`(1 - 2.5 f)(f - 0.25)` at f = 0.33), far above the 0.0017 bound. Corrected: within a run of
   samples with occupancy above 0, every sample uses the `Radiance` and `Normal` read at the run's first sample; that sample lies
   less than one step (0.75 voxel) past the last empty voxel center, so its footprint holds the layer the ray entered, never the one
   behind it. The `voxels` view does the same. Case 5's Low run at `Offset` 0.25 m (two adjacent one-sided layers) is the
   failing-first case. D2's sentence on Low becomes "Low's 20 cm voxels hold a 20 cm wall in one two-sided layer (dark) or in two
   adjacent one-sided layers (each lit from its own side)".

4. **The gather's fallback pass and its visibility level (D13.2, D13.4).** (a) Pass 1 drops visibility when pass 0 found no visible
   probe. At a floor-wall junction the start (1.5 voxels off the floor) can lie in the wall's touching voxel; every march then reads
   0, pass 1 runs, and corners beyond the wall that lie in front of the floor's plane (`h > 0`) bring in the outside's light: bright
   strips along a sealed room's junctions. 6.2's points (wall centers, floor points inset 0.6 m) never sample one. Corrected: no pass
   drops visibility. Pass 1 moves the start off the solid (down the occupancy gradient, one voxel at a time, at most two, while the
   start's occupancy is at least 0.3) and marches again; a pixel with no visible probe after it takes no field light (`E = 0`, its
   weight kept). Cases 5, 6 and 8 add junction points (the floor 0.05 and 0.15 m from each wall, a three-wall corner, the ceiling
   0.1 m from a wall) held to the same 0.0017; case 1 adds the open room's junction points, each at least 0.5 of its reference (no
   black strip). (b) `VisibilityMarch` uses one level, chosen by the start; the probe can lie 0.87 m away outside that level's valid
   box, where the toroidal textures wrap to the window's far side. Corrected: the finest of levels 0-2 whose valid box, less
   `c_GILevelGuardVoxels`, holds both the start and the probe.

5. **Pending switches the whole field off in ordinary motion and streaming (D3.3, D5.1, D14.5).** As written, an unfinished scroll
   slab of levels 0-2 or any proxy waiting for residency makes the frame pending, and the field's weight drops to 0 everywhere: a
   camera that outruns the budget, or a world streaming its meshes in, flips the image between the field and today's light. A proxy
   outside the view and beyond `PrefetchDistance` (prefetch off, or evicted) is requested by nobody and can hold it pending for good.
   D3.3 also rebuilds on "a cut, `ResetHistory`": `IsCameraCut` is true for a 45-degree turn between frames, and `ResetHistory` runs
   on every lighting-view switch (`BeginLightingViewFrame`) and quality change, so a fast turn or going from `voxels` to `gi` would
   throw the voxels away. Corrected rules: (1) Rebuilds are the first frame, a new layout and another render scene that is not a
   clone of the last. Camera motion of any size goes through scrolling (a move of a window or more requeues that window, with
   `VoxelizeTrianglesOnCut` as that frame's budget); camera cuts and `ResetHistory` leave the clipmap alone. (2) A frame is pending
   while a valid box of levels 0-2 misses its window shrunk by `2 * c_GIScrollVoxels` on any face (a rebuild or a jump), never for a
   slab or a change region left for a later frame; there `SampleVoxels` reads the next level. (3) Proxies waiting for residency or
   their mesh are left out and queued when they arrive; they never make a frame pending; `GIWaitingProxies` counts them (stats and
   the capture's object). (4) `GetProxyChanges` returning false (more than 65,536 changes) requeues every window and keeps the valid
   boxes. 6.1 gains "A fast turn keeps the voxels" (90 degrees in one frame: no voxel work, not pending), "A view switch keeps the
   voxels" and "Waiting proxies do not switch the field off" (a proxy held non-resident by the test: not pending; its voxels appear
   in the frame it becomes resident).

6. **Moving objects (D8, 5b, 6.2's moving prop).** "The soft march and the soft probe weights smooth that over a voxel of movement"
   is wrong: they smooth in space, not in time; a whole voxel that turns on changes every sample around it in one frame. The lab
   measured it (DEVLOG step 3f, without its temporal blend: largest second difference 112.8 of 760 before its fixes, 8.2 after all
   three, the largest drop from fractional occupancy), so the 2% trigger will fire. Re-voxelizing a mover's old and new bricks each
   frame in every level also re-dispatches every triangle of every static proxy overlapping them (a terrain's whole submesh per
   region) against `VoxelizeTrianglesPerFrame`: several movers on a large mesh overrun it every frame. Corrected D8 above the cut: a
   proxy whose placement changed within the last 30 frames is moving and is left out of the voxels; its bounds are queued once when
   it starts moving (removing it) and once when it has been still for 30 frames (adding it back). Movers then cost no voxel work and
   cannot pop the GI while they move; they receive the field like characters. 6.2's moving-prop case (ambient occlusion off, 9)
   checks that the floor points do not change while the ball rolls and reports the one step when it settles. 5b (a dynamic
   fractional layer, LAB `voxelize_boxes`) is the fix that gives movers their occlusion back. DG "what it cannot do" adds "moving
   objects neither occlude nor bounce light until they rest".

7. **Cosine normalization and the ambient cube (D13.1, D13.2, section 11 question 2, 6.2, 6.3).** (a) For the 16 directions of a
   4 x 4 tile with `GetSolidAngle`'s weights, `sum_d max(0, d.a_f) Omega(d)` is 0.959 pi for +-X and +-Y and 1.115 pi for +-Z (the
   engine's `OctahedralDecode` folds along z; computed for this review). Dividing by pi makes open ground under a uniform sky 4% dark
   and walls facing +-Z 11.5% bright, so "Open ground reads today's sky within 3%", the `open` median 1 +- 0.02 and the room's
   per-wall ratios fail for a reason no tuning knob reaches. Corrected: each face divides by `C_f = sum_d max(0, d.a_f) Omega(d)`
   instead of pi, for `E_f` and `V_f` (six constants beside the solid angles); `Renderer.GlobalIlluminationLayout` checks that a
   uniform radiance gives every face exactly that radiance and visibility 1. (b) The ambient cube is exact for axis normals only: an
   oblique normal blends axis lobes that include directions behind its tangent plane, which on a convex surface hit the surface
   itself (a 45-degree lune of each axis hemisphere). A sphere's 45-degree points read several percent dark (up to about 15%) and a
   sunlit slope lights itself. The room's surfaces are all axis-aligned, so question 2's "switch only on 6.2's evidence" has no case
   that can show it, while 6.3's furnace case (3%) will. Corrected: Gate W adds the furnace sphere on the `gi` view (no sun, one
   bounce: every visible point reads the sky's radiance within 3%). On a miss the reduce stores an 8 x 8 octahedral irradiance per
   probe (6 x 6 with a one-texel border, DDGI's form; RGBA16F, 512 B, in a `Texture2DArray` since 8 x 48 texels pass the 3D limit;
   +40.8 MiB at High, +5.1 MiB at Low), read with one bilinear fetch per probe; 0.5 day inside stage 2.

8. **Faces on voxel boundaries close openings (D5.3, 6.2 case 1).** Akenine-Moller's test counts touching as overlap, so a face on
   a voxel boundary marks a voxel on each side. That keeps one-voxel slabs lit (the sun room's 0.1 m floor keeps a lit layer at
   High), but the sun room's window lies on the 0.1 m grid too (x -11 to -9, sill 0.9 m, lintel 2.2 m): each reveal marks a voxel
   into the opening, and with the gain's extra 0.1 voxel the voxel world's opening is about 1.78 x 1.08 m against 2.0 x 1.3 m. Its
   sun patch has about 74% of the reference's area before any probe error, inside a 30% bound. Corrected: case 1 prints beside its
   table the patch's power (the floor's surface voxels' injected radiance times their face area, against `SunRoomRadiance`
   integrated over the floor), so a miss splits into geometry and probes before the tuning day; the tuning day's first knob for a
   geometry miss is `c_GITouchOccupancy`, the occupancy of voxels marked only because a face lies on their boundary (their light and
   normal kept), measured on case 1 and the sealed cases together. The room stays on the grid: authored rooms are.

9. **Gate W measures GTAO, not the GI (D14.7, 6.2's strafes, 7.1).** The `gi` view shows what the join adds, which carries
   `DiffuseOcclusion` (material times GTAO); GTAO's own fence flicker (0.074% / 0.71%) sits just under the bounds, so the gate passes
   or fails on GTAO and cannot see the GI's flicker. Corrected: the strafes (regression and 4K) run with the scene's ambient
   occlusion off, so the meter reads the field alone (R2 predicts near 0), and once with it on as information; the commit sets each
   bound to 1.5 times its AO-off measurement, never above the guesses.

10. **Closed meshes with see-through or masked parts (D5.1, D6).** Closedness is over all submeshes, but items are per submesh and
    transparent or transmissive materials are left out. A closed mesh with a glass or alpha-masked submesh (a house with windows, a
    crate with a cut-out) then loses those triangles' winding steps, and every column through the hole fills solid up to the top of
    every region above it, blocking the sky and killing probes; counting their steps instead fills a closed glass box solid.
    Corrected: an item is `Closed` only when the mesh is closed and every submesh's material as drawn (the proxy's override
    included) is opaque: not alpha-masked, transparent or transmissive; otherwise the mesh is voxelized as open. For a closed mesh
    every submesh is an item (`FillOnly` where it misses the region). `c_GeometrySubmeshClosed` is not added: no shader reads it
    (the item's flag carries closedness), and no dead code lands. `Renderer.VoxelClipmap` adds "A closed mesh with a glass submesh
    fills nothing".

11. **The CPU's mesh layouts are not in `RenderScene` (2.2, D5.1).** `RenderScene::MeshEntry` keeps bounds and submesh materials
    only; LOD index counts, LOD errors, submesh bounds and closedness are the `Mesh`'s, and the pool's CPU tables are private.
    Corrected: `VoxelClipmap` keeps a cache per mesh slot (per submesh its bounds and per level of detail its index count and
    `MeshLOD::Error`; `Mesh::IsClosed()`), filled on the main thread from the meshes the frame resolves (the renderer's
    `AssetResolveCache`, the scene's asset manager) when the slot's mesh table entry changes, before the `BuildFrame` job; the job
    reads only that cache, `RenderScene` and `RenderSceneGPU`. Never per proxy.

12. **Bindless reads in compute would grow `c_KnownFailures` (D5.3, section 10's MoltenVK row).** The Metal conversion's list "only
    shrinks" (DG "Shaders", `.github/scripts/convert_shaders_to_metal.sh`), and no compute shader binds the bindless table today.
    Corrected: the albedo and emission factors are the CPU's. `Texture` keeps the linear mean of its smallest level, computed before
    its CPU copy is released (block formats through `DecodeLevel`), and the GI keeps a buffer per material slot (diffuse albedo
    `BaseColor.rgb * (1 - Metallic)` times the base map's mean, emission times the emissive map's mean, flags), rebuilt on the main
    thread for the slots whose data changed (`GetTablesVersion`, as D5.1 already compares) from the materials and textures the
    frame resolves. No GI layout binds the bindless or texture tables; "A textured material's albedo is its map's mean" stays.

13. **Anchors break the bit-exact claims (D3.2, D5.4, 6.1).** `A_l = 1024 v_l floor(W_l / 1024)` changes whenever a window crosses
    a 1,024-voxel boundary (every 102.4 m at level 0; level 0's vertical anchor changes at y = 3.2 m, which the scroll case's "+y"
    can cross), and `R x + (t - A)` rounds differently under another anchor, so regions voxelized before and after a change can
    differ from a fresh renderer's by a voxel at boundaries. At 20 km the instances' float translations round differently from the
    origin's, so "the voxels of the origin's yard" cannot hold bit for bit. Corrected: a level's anchor is set at a rebuild and kept
    until the camera is more than 1 km from it (then that level requeues, without pending); "Voxels are independent of the camera"
    holds within an anchor. Comparisons with a fresh renderer render the path's first camera first and then the compared one (a
    jump within 1 km keeps the anchors). The 20 km case compares occupancy with a tolerance: at most 1% of occupied voxels differ,
    each next to one occupied in the other.

14. **The owner's room cases (6.2 cases 2 and 4, 6.3, D10).** (a) Case 4's 2% between cameras A and B is not what the world-space
    form guarantees: from A (5.8 m from the north wall) the wall's probes sample level 1 or a blend, from B (2 m) level 0, and the
    two levels' voxels differ; D10's "the light at a fixed point does not depend on the camera" holds per level only. The failure
    case 4 guards (screen-space GI losing most of the bounce with the patch off screen) is far larger. Corrected: within 10% between
    A and B, both within 30% of the reference; D10 reads "does not depend on the camera but through `SampleVoxels`' level blend,
    which changes continuously with the camera's distance". (b) The sun room has no sky, so with GI off its walls' indirect light is
    0 and 6.3's "R/G change below 0.01" divides 0 by 0. Corrected: with GI not asked, the seven wall points' `Indirect` values with
    the red floor equal those with the white floor bit for bit.

15. **Sizes and the cut line (section 3).** The sizes are not credible. OLD's stage 1 was sized 3 days and overran with less novel
    GPU work; stage 1 here adds a toroidal clipmap with bit-exact tests, five compute passes and a view, stage 2 seven passes, a
    forward gather, three views and about 20 GPU cases. Honest guesses, with these corrections: stage 1 4.5 days (4-5.5), stage 2 4
    days (3.5-5) plus Gate W's tuning day on a miss, stage 3 1.75 days (1.5-2.5), stage 4 1 day (0.75-1.25). Stages 1-3 come to
    about 10 days; 6-8 days hold stages 1 and 2 (the views and Gate W), not the lit image. Corrected order and cut: stage 2 ends at
    its (d) (the field, the `gi` and `open` views) with the room cases, the sealed hall, the furnace and the strafes; then the join's
    core: the two properties, the row, the forward branch for opaque surfaces, the feature test, 6.3's off, pending, edge,
    owner's-experiment and furnace cases, look-dev before and after. **Cut line** there (about 8.5 days, 7.5-10.5): the owner's red
    floor live in the lit image. Below it, in order: stage 4 (multi-bounce), 5b (the movers' dynamic layer), the transparent and
    glass variants (D16.4: until then they keep today's light), the `probes` view, the capture object's full fields and
    `editor.perf`'s key, then 5a and the rest. If the orchestrator holds 8 days, its levers are Low (the row Off and High first, Low
    a later commit), the 20 km and orthographic cases, and the 4K suite's stage milliseconds (the 4K strafes stay).

16. **Numbers (D2, section 11, D18.5, 2.3).** (a) Question 1's 64 x 40 x 64 field adds 20.6 MiB (21,625,520 bytes: atlases +17.2
    MB, field +3.4 MB, weights and validity +1.0 MB) and 1.78 times the rays, not 7 MiB. (b) 5e's 96 x 40 x 96 field makes cascade
    0's atlas 384 texels wide, past D1's 256: Ultra needs `Texture2DArray` atlases. (c) `c_GIBatchVoxels` is per layout, the
    scratch's voxels: 262,144 at High, 131,072 at Low (Low's scratch is one level; a 64^3 batch overflows it). (d) `c_GISkyMip` 4
    has 6,144 texels of 0.0020 sr, a sixth of a 1,024-direction texel (0.0123 sr), so a procedural sky's disk falls into some fixed
    directions and not others as it moves. Corrected: LOD 5.3 (`SampleLevel` between mips 5 and 6), a top-cascade direction's solid
    angle. (e) `VoxelWorkItem`'s submesh has 8 bits: a submesh at index 256 or above is left out and logged once
    (`GIUnvoxelizedSubmeshes`). (f) 2.3's reason 3 is false today: main is at 7b2e136a, OLD's branch point; the decision stands on
    reasons 1 and 2.

17. **Implementation details (D4, D5.3, D14.3).** (a) The GPU cannot split and requeue an overflowing batch (the CPU learns of it
    2-3 frames later), and a region marked valid with triangles missing leaks until something requeues it. Corrected: when `Tiles`
    is full, `VoxelTriangles` walks that triangle's remaining tiles itself (slow, complete) and counts `GITileOverflow`;
    `VoxelTiles` dispatches at most `c_GIMaxTiles` groups. (b) `TileArguments` is written (count 0, y and z 1) before each batch's
    `VoxelTriangles`, with a barrier. (c) Every release of GI resources (no frame wants it, `SetQuality`, the render targets) goes
    through `Renderer::ReleaseDeferred`, and the scene binding sets that held them are made again in that frame. (d) The
    voxelizer's binding sets are made again when `GeometryPool::GetBufferVersion`, `GeometryPool::GetTableVersion` or
    `RenderSceneGPU::GetBufferVersion` change.

18. **Licences (D19).** The research marks erhe and creikey's voxel-radiance-cascades "MIT" without "(checked)", and neither is
    among the local copies. Corrected: both are ideas only until the implementer reads their LICENSE at the commit used and names it
    in the porting commit. Nothing above the cut needs their code: what D19 takes from erhe (the transmittance product, the 0.25 /
    0.75 layout, the leak stations, the 0.0017 bound) and from creikey (levels by distance, the packed maximum) is ideas and test
    design, written out here.

19. **Cost when off, and the Mac (D1, D16, section 10).** (a) The gather (eight probes with marches of up to nine samples) sits in
    `Forward.frag` behind a uniform branch, and the driver allocates registers for it on every frame, GI or not, so "GI off costs
    nothing" is a measurement, not a given. Corrected: the corner loop and the march carry `[loop]` (never unrolled); stage 3's A/B
    at Medium (GI off) holds `GPU.Opaque` within the pairs' spread of the base, and a miss goes to the orchestrator; a define variant
    is no fallback (new `Forward` entries would grow `c_KnownFailures`). (b) CI's Mac has no GPU (ci.yml: hosted runners convert the
    shaders to Metal and skip `gpu`), so nothing here is measured on MoltenVK, and a Mac project at the default High runs High's
    layout when a scene asks. The DG and the Manual say so: High's cost is measured on the reference machine only, and Low is the
    level for integrated and Apple GPUs until measured there.
