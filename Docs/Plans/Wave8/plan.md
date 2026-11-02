# Wave 8 plan: the showcase lands, and the owner's list behind it

Records cited as `.worktrees/wave8-plan/...` (audits, captures, prototypes) are on the reference machine, outside the repository.

Planned on 2026-11-01 on the Windows reference machine with its temporary memory (i7-13700K, 32 GB at 3200 MT/s, RTX 5090,
3840x2160 at 150%), from eleven audits of main at 76f28a21 (release 0.7.0): `shadow-jump`, `local-lights`, `specular`,
`radiance-cascades`, `volumetrics-sss`, `physics-timing`, `editor-ux`, `platform-seams`, `slang`, `esoterica` and `showcase`.
Their structured results are the reports; their captures, spikes, scripts and data are in `.worktrees/wave8-plan/<topic>/`.
Paths below: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/` (never spherical harmonics, which this plan
spells out), `DG` is `Docs/DeveloperGuide.md`. Shaders are named as `strata_add_shaders` registers them: after step 0's move
to Slang, `SH/Scene/Forward.frag` is the file `Forward.frag.slang` and `Include/X.glsl` is `Include/X.slang`. Days are the
audits' agent-days, a unit of size that gives order, never a date (section 4).

## 1. Goal

The owner (2026-11-01, verbatim in `.worktrees/wave8-plan/owner_feedback.md`): "Make prioritization yourself, but showcase
needs to land this wave, you can update the showcase later if there are more things." Fifteen items follow. The same day the
owner added: "This is the esoterica engine that the animation system should be based on" and "radiance cascade, you will need
to research as well." The look stays wave 7's: realistic, filmic images at native resolution with the Fox Engine as the
baseline, never temporal upscaling, nothing that needs TAA to look right, neither blurry nor clinical.

Our priorities, and why. Rank says what gives way last. Section 4's tranches say what runs first; the two differ where an answer
is cheap and decides much (gate G1) or a fix is small and the showcase uses it (`physics-timing`).

1. **The showcase lands.** It is the one fixed promise, so it depends on no stream above its own cut (section 4.2), lands on
   main in landing 1 and is improved afterwards, as the owner allows.
2. **The bugs and the easy items come first** (step 0): contact shadows off, the shadow jump fixed, the idle limit, the
   Performance pause, the physics rate option, a first drag and drop with a ghost, GLFW sealed. The owner sees them in days.
3. **Slang goes in step 0.** It touches every shader, so it cannot overlap the rendering streams; the tree is quiet now, and
   the wave's new shaders then get written once.
4. **`reflections`**: "pretty important for the showcase". The audit found the shading sound in stills and the reflections at
   fault.
5. **`animation-graph`**: "really really important". The whole system does not fit in the wave; a useful first slice does,
   beside the showcase, sharing no file with it. It has the first claim on tranche 2: if usage runs short, `radiance-cascades`'
   hardening gives way first, then `local-lights`.
6. **`radiance-cascades`**: the largest visual step and the largest risk. Its gate G1 runs in tranche 1 because that answer
   decides the stream and costs little; the join follows in tranche 2.
7. **`physics-timing` and `local-lights`**: both marked "easy", and both need more than the owner expected. The option and
   the cap are easy; the jitter's fix and shadows that do not blink are the real work. `physics-timing` runs in tranche 1
   because the showcase's walk uses it.
8. **`editor-ux`, `volumetric`, `showcase-engine`**, in that order: wave 9 unless usage remains. Subsurface scattering goes to
   wave 9 with a character to show it.

Rules that hold for everything in the wave:

- Wave 7's rules stand: native resolution only; nothing needs temporal accumulation (every screen-space effect ships its own
  spatial filter and is judged with TAA, with FXAA and without anti-aliasing); a feature that is off costs nothing; the CPU and
  GPU-driven paths stay pixel-equal and cached and uncached shadows exact; Medium keeps 0.6.0's costs, High is the programs'
  default, `SceneRendererSpecification{}` stays Medium. One effect still breaks the temporal rule and is named: Medium's
  Checkerboard reflections trace nothing without TAA (DG "Reflections"). They stay, because Medium holds 0.6.0's costs, and they
  go in the open items.
- One wave 7 ruling is overturned: it rejected screen-space global illumination because it needed temporal accumulation
  (wave 7 plan, section 9). The owner's item 5 asks for it. The design keeps no history buffer and accumulates nothing; its one
  input from the past is the previous frame's lit color, as screen-space reflections' is, so bounce light builds over a few
  frames (section 4.4).
- Budgets only go down. On the temporary memory nothing is measured against `Budgets.json`: interleaved A/Bs against 0.7.0
  under `Tools/PerfLock.py`, new metrics `Provisional`. Times in this plan are rough figures unless they say A/B.
- At most four build-heavy agents at once on this machine.

### 1.1 Acceptance at the wave's end

Checked at integration with the look-dev harness and each audit's own method (kept in its folder), against 0.7.0's Release
build. Targets marked (proposed) have no measurement of the real thing behind them yet. A row holds only if its tranche landed;
what did not land says so in the release notes.

| What | 0.7.0 (measured) | Wave 8 target | Tranche |
| --- | --- | --- | --- |
| Shadows while a ball leaves the camera (Physics Playground, default camera, three shots) | 28 layout changes; 6,600-10,000 pixels change by 8/255 or more outside the ball | 0 layout changes during the shots at 60, 240 and 1000 fps; at most 2,400 pixels (fit off: 800-2,400) | Step 0 |
| Cascade layout changes in scripted motion (Playground, Third-Person, Open World) | 202, 371, 427 | at most 100, 80, 65 (prototype 85, 66, 51); settled layouts unchanged | Step 0 |
| Contact shadows | on at High (12) and Ultra (16) | 0 in every preset; the row stays for Custom | Step 0 |
| Shaders | 103 entries through glslang | 103 through slangc; reflection diff explained; 0 validation errors; GPU time within control noise; every entry emits HLSL and converts to Metal | Step 0 |
| GLFW | 3 functions leak into `VulkanGraphicsDevice`, nothing enforces it | 0 `glfw*` symbols outside `Platform/GLFW`, a table rule, a CI build without GLFW | Step 0 |
| Noise in a metal sheet's reflection, roughness 0.18, 9 degrees grazing, High and Ultra (RMS of luma minus its 9x9 mean) | 3.5-5.0% in every AA mode (Medium 5.9-6.4%); probe alone 0.3% | under 1% in every AA mode (proposed: no fix was prototyped) | 1 |
| Reflection flicker in motion, camera strafe over `lookdev.sheet` and `.wet`, no AA, FXAA and TAA | not measured: the audit took stills | at most 1% over 32 px blocks (proposed; measured first in `reflections`' step 1) | 1 |
| The owner's three shots in the showcase: side of a bus, oil tank at low sun, wet ground | proven on blocks in the audit (`p05`, `p07`, `p11`) | read as real on the owner's sheet | 1 |
| Character jitter at 6.5 m/s, 3840x2160 equivalent, 60 Hz steps | 29.4 px at 702 fps, 16.7 px at 120 fps | at most 1 px from 30 to 1000 fps | 1 |
| Simulation with interpolation on against off | n/a | the same poses at the same step counts, bit for bit | 1 |
| Radiance cascades, gate G1, 3840x2160 High, no AA and TAA | no engine code; CPU model 0.45-0.52% block flicker, 1.17% on grass | stage cost at most 1.2 times `GPU.AO` on `tp.scene`, 2.5 times on `ow.scene`; at most 0.5% flicker over 32 px blocks on the fence strafe, p99 near 3%; at most 1.0% on foliage (proposed; the degrade path is expected, section 4.4) | 1 |
| Showcase GPU busy, 3840x2160 | n/a | at most 8.3 ms at High, 14 ms at Ultra (proposed, `Provisional`) | 1 |
| Showcase content | n/a | source assets at most 80 MB, binaries at most 85 MB, `Samples.Content` and the package test pass | 1 |
| Indirect light in motion | n/a | the owner's live look in motion, in the rebuilt editor with GI toggled, on Third-Person and the showcase, before any sample turns it on | 2 |
| Shadowed point and spot lights, High and Ultra | 16, 16 | 64, 256 | 2 |
| Frames where a ranked light has lost its shadow, 35 s flight through 256 lanterns at 60 fps | 12.3% at 16 lights; a plain unlock 27.8% at 64 and 53-57% at 256 | Medium at most 1.5%, High at most 2%, Ultra at most 1%; no full atlas layout | 2 |
| Local shadows' filter noise | turned every frame under TAA, so that its history averages it away | fixed noise; no grain in shot 6 or the lantern town with FXAA or no AA | 2 |
| `FindLocalShadowCasters` p50, 1000 moving bodies | 6.08 ms at 234 lights | at most 0.5 ms at 256 (proposed; if missed, Ultra takes 64) | 2 |
| Walk to run in the Third-Person Adventure | cycles 0.26 s (27%) apart in the cross-fade | phase locked within 1% of a cycle over 10 simulated minutes | 2 |
| `Perf.Animation.Evaluate1000` | 0.37 ms | unchanged (simple animators keep today's path) | 2 |
| GPU busy, High, 3840x2160, the 3D samples' scene cameras, everything adopted (GI only once the owner has looked, else on for this measure alone) | 1.95-3.02 ms | at most 5 ms | Each landing |
| GPU busy, Medium, 1920x1080, same views, interleaved A/B | wave 7's 1.10-1.20 times step 0's | no worse than 0.7.0 within the A/B's noise | Each landing |

### 1.2 The owner's list, item by item

| # | Item | Where it went | Size | Note |
| --- | --- | --- | --- | --- |
| 1 | Contact shadows | S0.2, default off | S | Not an easy fix (M to L). The replacement march goes to "Later" |
| 2 | Spherical-harmonic irradiance probes | `radiance-cascades`, below its cut; above it if gate G1 fails | L | The hut uses today's box probe, which already carries 9 coefficients |
| 3 | Unlock shadowed lights | `local-lights`, tranche 2 | L | Not step 0: the cap alone makes shadows blink |
| 4 | Specular | `reflections`, and shots 2-4 of `showcase`, tranche 1 | L | Sound in stills, motion still to check; reflections repaired. Reading: section 4.2 |
| 5 | Radiance cascades | `radiance-cascades`: to G1 in tranche 1, the join in tranche 2 | XL | Time-boxed; the owner's live look before any sample adopts it |
| 6 | Volumetrics and light cones | `volumetric`, tranche 3 | L | Needs dark interiors first. Joins only with its cones (steps 1-5) |
| 7 | Subsurface scattering | Wave 9 | M-L | Nothing to show without people. Thin translucency is the last item below `volumetric`'s cut |
| 8 | Shadow jump | S0.3 | M | Root cause proved, prototype passes |
| 9 | Esoterica's animation system | `animation-graph` (first slice), tranche 2; wave 9 (tools and the rest) | XL now, 2 XL later | Evaluated: the whole does not fit. No graph canvas in wave 8. Wave 9's entries are recorded in S0.12 whatever happens |
| 10 | Drag and drop with a ghost | S0.7 (ray, placement, box ghost), `editor-ux`, tranche 3 (mesh ghost, live material, sky) | M+S, then L | Not all "easy", but the first form lands in step 0 |
| 11 | Physics rate | S0.9 (the option), `physics-timing`, tranche 1 (the fix) | S, then L | Display sampling, not a physics bug |
| 12 | Idle frame limit | S0.4 | S+S | |
| 13 | Performance pause | S0.5 | S | The scrolling breakdown pauses; the viewport does not |
| 14 | Slang | S0.10 | L | Time-boxed at 2 agent-days, or wave 9's step 0 |
| 15 | GLFW wrapped | S0.8 | M | Inventory of other console seams in the docs only |

## 2. Where we start

The audits' numbers that shaped the plan:

- **Capacity.** Wave 7 is the only measured capacity. Six streams of 5.5-17.9k insertions took about 18 hours on four slots
  with 210 agents, and integration about 15 hours more. Usage stood at 88% of the owner's week when the streams ended, and
  integration was stopped at 99%: the whole perf A/B, the sheets, the `-rc` dry run and CI before main were skipped. Cut lines
  absorbed nothing; what fell below them was simply not started.
- **Shadow jump.** Wave 7's fit moves its near bound at once for any receiver above 1% of the pixels. A ball at 1.5 m holds
  6.85%, every slice is re-cut, and about 52% of the receivers fall into the unfitted last cascade, 3.2 to 4.5 times coarser,
  for 116 to 354 ms. Hysteresis counts readbacks, which last 9 ms at 880 fps. Main also re-cuts up to 76 times a second in the
  Third-Person run, which may be part of the instability the owner sees (unverified).
- **Contact shadows.** Binary hits in a thin shell, steps of 2.5 cm, screen-locked noise. Worst 16x16 tile per frame on against
  off: 11.5 against 0.4 codes without AA, 3.1 against 0.1 with TAA. Off loses nothing visible on the feet, the dominoes or the
  5 cm plate in the audit's views, and saves about 0.15 to 0.3 ms at 4K.
- **Reflections.** The sun's highlight, energy, Fresnel and sky reflection hold up in 4K stills, and wet ground already looks
  right with authored maps. Metal sheets do not: SSR leaves a fixed 4x4 screen door, ghost columns and crescents at every
  quality and AA mode (4-6% noise against 0.3% without SSR), fades 384 px at each screen side whatever the ray does, and
  off-screen reflections are the sky unless a baked probe stands in. IOR never reaches F0. The night sky is posterized.
- **Indirect light.** A room is lit like the street. A CPU model of Path of Exile 2's layout flickers at 3.45% over 32 px blocks
  as published and 0.45-0.52% with the fixes found, but 1.2-1.5% over dense grass and around small emitters. Cascades with a sky
  fallback leave a room 3.7 times too bright, because the depth buffer covers little of the hemisphere there; one box probe
  lands within 13% of the reference. Estimated cost 1.2 ms and 150 MiB at 4K; nothing ran on a GPU.
- **Local lights.** Removing the cap is S. A plain unlock leaves some light without a shadow in 28% of frames at 64 lights and
  53-57% at 256; 0.7.0's 16 lights already do it in 12%. About 100 lines of tile lifetime logic bring that to 1-5%, the proposed
  rows to 0.2-1.7%. Change tracking costs 26 ns per light per moved proxy.
- **Physics timing.** The character moves on 60 Hz steps while the camera slides every frame: a 29 px sawtooth. A prototype that
  draws between the last two fixed poses gives 0.25 px with the simulation unchanged. A higher rate only halves it per doubling.
  Stepping with the frame's delta costs 10-14 times more at 700 fps and loses repeatability and every step-counting tool.
- **Editor.** With FIFO requested the editor ran at 190-720 FPS on a 120 Hz display, so a 600-frame ring is under a second of
  history. Nobody knows why FIFO does not pace it. A ray costs about 6 ns per triangle; scene size barely matters.
- **Shaders.** A spike took all 103 entries through an 835-line converter and 12 hand fixes. They pass spirv-val, match glslang's
  reflection, and pass 553 of 554 GPU test cases under the validation layer (the same failure as baseline). One silent mistake
  (`mul` of two vectors) passed every GPU test and only the look-dev A/B caught it.
- **Windowing.** Two objects reference GLFW: the backend (51 symbols) and `VulkanGraphicsDevice` (3). Nothing enforces the seam.
- **Animation.** Esoterica is MIT, Windows, MSVC and x86 only, and its animation code cannot be separated from its Base,
  reflection generator, resource and entity systems (core closure 87k lines). It has no motion matching, retargeting or tests.
- **Showcase.** The owner's three shots already work on 0.7.0 as highlights on blocks. No bus, van, truck or oil tank exists in
  either CC0 source, so they are generated. Wave 7's list is 82.2 MB before terrain and probes, not 60; a re-cut list is 72.2 MB
  with everything.
- **Volumetrics.** Height fog ignores shadow: a sealed room glows at dusk. A froxel grid needs no history if visibility is read
  coarsely. Most of the hut's gain comes from a dark interior, so it follows GI.

What the audits did not do, so the streams must:

- The shadow prototype ran in Release only (no Debug, validation layer or feature test).
- The Slang spike ran on patched Release copies: no real build, Debug, Dist, Linux, macOS, MoltenVK or other GPUs.
- The local-lights prototype ran no tests and only cube scenes.
- Radiance cascades, volumetrics and everything in `editor-ux` are designs from reading and models.
- Specular was judged from stills: SSR crawl and highlight shimmer in motion were not checked, lantern highlights were not judged
  properly, and the furnace and Fresnel were not re-measured. No SSR fix was prototyped.
- The ExileCon 2023 talk was never read: Path of Exile 2's facts come only from the radiance cascades paper, and how it maps its
  bitmask to the hemisphere is not published.
- Esoterica was never built. The README's talk was not watched and `esotericaengine.com/docs` returned only navigation.
- The showcase's vista, dusk and interior were not tried: shadows at 600 m in a 300 m scene, and a real dark hut.

## 3. Step 0 (on main, before the streams branch)

Step 0 runs as four **lanes**, parallel worktrees, each landing by itself when its gates pass. Only S0.10 touches shaders, so the
lanes cannot collide there. They do meet in three files: S0.8 and S0.10 both edit `Strata/CMakeLists.txt`,
`Strata/vendor/CMakeLists.txt` and `ci.yml`, so lane Slang merges main before its last verification. About 7.5 agent-days of
building, about 4 days from the first commit to S0.12's tip with four lanes, the landings' reviews and CI included. The streams'
days count from there.

| Step | Lane | What | Item | Size |
| --- | --- | --- | --- | --- |
| S0.0 | first | Baselines, kept A/B base, plan committed | | S |
| S0.1 | first | Room in AGENTS.md | | S |
| S0.2 | Render | Contact shadows off | 1 | S |
| S0.3 | Render | The cascade fit stops jumping | 8 | M |
| S0.7 | Editor, first | `scene.raycast`, `asset.place`, box ghost | 10 | M+S |
| S0.4 | Editor | Editor frame rate preferences | 12 | S+S |
| S0.5 | Editor | Performance pause | 13 | S |
| S0.8 | Platform | GLFW sealed | 15 | M |
| S0.6 | Platform, after S0.8's step 1 | Why FIFO does not pace the editor | 11, 13 | S, boxed |
| S0.9 | Platform | `Scene.Timing` and `FixedRate` | 11 | S |
| S0.10 | Slang | Every shader in Slang | 14 | L, boxed |
| S0.11 | last | The indirect block's seam, slots and stages | | S-M |
| S0.12 | last | Roadmap, wave 9's entries | | S |

- **S0.0 Baselines (first).** Before anything rebuilds `build/windows`: the look-dev baseline at 0.7.0 from its Release build,
  and a kept Release build of 76f28a21 in `.worktrees/wave8-base` as every A/B's base (no older base build exists). This plan
  goes to `Docs/Plans/Wave8/plan.md`.
- **S0.1 Room in AGENTS.md (first, before the lanes branch).** 692 characters are free. Move details into the guide (rule kept
  whole here, stated in full there) until at least 3,000 are free: nine digest bullets and Slang's rules need them. It also rewrites golden rule 1 as the owner scoped it
  (D10): other folders on this computer are never read; open-source projects are, and their code is used under D10's license rule, with
  `ThirdPartyNotices.md` gaining a section for code derived from projects that are not submodules.

### Lane Render

- **S0.2 Contact shadows off (S).** `R/RendererQuality.cpp:43` becomes `{0, 0, 0, 0}`. The row stays (Custom 0 or 4-32), and
  `ContactShadowLength` stays 0.3 so turning the row on works at once. No section version bump: Custom values are stored
  explicitly. The audit lists every test, tooltip, schema and doc line (`shadow-jump` report, section 2). Wave 7's "solid contact
  shadow" under the 5 cm plate now rests on the fitted cascades: re-check with `lookdev.contact` and correct `Roadmap.md:1277`.
  Release notes get an upgrade row (Custom, Contact Shadows 12 keeps them).
- **S0.3 The cascade fit stops jumping (M).** All in `ShadowFitTracker`:
  - `ShadowFitView::Seconds`, from the clock exposure uses (wall clock capped at 0.25 s, `SceneRenderOptions::AdaptationSeconds`
    overrides). Tests pass `AdaptationSeconds`: wall-clock holds make comparisons of two renderers timing dependent.
  - The near bound grows at once only when receivers nearer than N hold at least 10% of the receiver pixels, or when the
    transparent bound asks (not in the prototype; "The nearest transparent surface bounds the near end" needs it). Otherwise it
    must keep asking for 0.5 s on a still camera or 0.12 s on a moving one, then grows to the least grown it asked for.
  - Squares: on a still camera a growth waits the same 0.5 s and keeps the previous square whole; a moving camera grows at once.
  - Reproject leaves out bins nearer than the layout's N. Shrinks need 8 readbacks and at least 0.5 s. The far bound stays.
  - Regression tests from the prototype: a ball at 60, 240 and 1000 fps at 24 and 12 m/s; a receiver that stays; the GPU case.
    DG "Sun shadows". Debug, the validation layer and the feature test run on the real change.
  - An object that stays near still re-cuts once. The optional gap-aware slices are not built (section 9).

### Lane Editor

S0.7 goes first and lands by itself, because the showcase's graybox waits for it and for nothing else in this lane.

- **S0.7 Ray, placement, box ghost (M+S).** Before the showcase scene is built, because an agent makes it.
  - `scene.raycast {origin, direction | viewport, maxDistance?, ignore?}` (Query) and `asset.place {asset, point | ray | viewport,
    parent?, snap?}` (Edit, one undo step, `MayDefer` until the template is resident, no `LoadAssetSync`), both in a non-core
    toolset (`core` is at its 32 tools). The UI drop calls `asset.place`, so people and agents get identical results.
  - The owner asked for a ghost "when the ray hit an entity that make sense". A hit on a surface that takes the asset draws the full
    ghost: prefabs, models and mesh sub-assets on any surface, a material's tint only on a mesh renderer. When a prefab, model or mesh
    misses, placement falls back to the ray's meeting with y = 0 within about 500 m, else to the ray at the camera's distance to its
    target, and the ghost draws in a quieter style, so a guess never looks like a hit. `FindPivotSurface` stays for Add to Scene. The
    view's own camera is used, so Game camera works.
  - Other asset types show "Cannot place a <type> here" at the pointer instead of a Console warning. Snapping as the gizmo does.
    `AssetBounds::Measure` throttled, `SceneRaycast` through `Scene::GetAssetManager()`.
  - The box ghost through `DebugDraw`, cast only when the pointer or camera changed; cancel with Escape; `AcceptNoDrawDefaultRect`.
    Hover changes no version: `edit.history`, `UndoStack::GetVersion`, `IsSceneModified` and `GetHierarchyVersion` stay still until
    the drop. Logic in `Editor/PlacementPreview`, the panel only feeds the pointer.
- **S0.4 Editor frame rate preferences (S+S).** New section `Editor.FrameRate` (`Editor/EditorFrameRate.{h,cpp}`, drawn by the
  generic page, `preferences.set`, kind Session). Defaults keep today's behavior.

  | Field | Choices | Default |
  | --- | --- | --- |
  | `IdleAfterSeconds` | 0-3600; 0 never counts as idle, even behind other windows | 0.5 |
  | `IdleFrameRate` | 60, 30, 20, 15, 10, 5 | 30 |
  | `UnfocusedIdleFrameRate` | 60, 30, 15, 10, 5 | 10 |
  | `ActiveFrameRate` | Unlimited, 240, 165, 144, 120, 90, 60 | Unlimited |

  Not labelled "idle timeout" (`--idle-timeout` means something else). Floor of 5, because `FramePacer` does not wake on input.
  Two traps in `UpdateFrameRate`: its early return when throttling is off would leave an idle editor stuck at 30, so 0 is a
  separate branch; and an active cap must not overwrite the lift of `UpdateSteppingFrameRate`, so cap 0 stays while steps or
  commands are pending. `editor.status` gains the four values, `editor.perf` gains `interval` (how a cap gets verified). Headless,
  `--frames` and script runs ignore the section. Measure `FramePacer` at 120 and above; if `sleep_until` cannot hold them, the
  precise wait goes to `editor-ux`.
- **S0.5 Performance pause (S).** A Pause toggle beside Copy as JSON (Space with the panel focused). Pause copies the ring through
  `FrameProfiler::TakeSnapshot()` `c_MaxGPULatencyFrames` frames after the click, so the frames shown have GPU times. Timeline,
  tooltip, table and summary read the snapshot, and scale and spike threshold freeze with it. The profiler never stops:
  `editor.perf`, listeners, `--perf-log` and the status pill stay live. Copy as JSON copies the frozen statistics and says so.

### Lane Platform

- **S0.8 GLFW sealed (M).** In this order; steps 1 and 2 are never cut.
  1. `Platform/Vulkan/VulkanPresentation.h` (App layer): instance extensions, presentation support, create surface. `GLFWWindow`
     implements it, `Window` gains a defaulted accessor (null: a headless device), the device reads it through a `Window*` in
     `GraphicsDeviceSpecification`, `GetNativeWindow()` is deleted, `glfw` leaves `strata_layer_link(Renderer ...)`.
  2. `ExternalOwners` in `Layers.json`, checked by `LayeringAnalyzer`: `GLFW/` only from `Platform/GLFW/**`, also `vulkan/` and OS
     headers. It must fail on today's tree at `VulkanGraphicsDevice.cpp:21` before step 1 lands.
  3. `Window::Create` returns null on failure, `Application` reports through `ReportFatalError` and exits 1;
     `ApplicationSpecification::WindowFactory` lets a stand-in in.
  4. `Tests::StubWindow` replaces the two fake `Window`s; a `Core.WindowBackend` suite drives the frame loop through it.
  5. `STRATA_WITH_GLFW=OFF` on the existing `Linux (no Audio, no Physics)` CI row, with `Platform/NoWindow/NoWindow.cpp`, a link
     closure check and `IsOtherPlatform` kept in agreement. Cut first if the lane runs long.
  6. DG "Platform seams" with the audit's inventory of other seams (documents only: threads, files, script modules), Architecture,
     `KeyCodes.h`'s comments (the values are the script ABI's, backends translate).

  The surface path runs only in `StrataEditor.Smoke` and `QuitScreenshot`, so run those here and rebuild the owner's editor to look.
- **S0.6 Why FIFO does not pace the editor (S, box of half a day).** In this lane, after S0.8's step 1, because both edit
  `VulkanGraphicsDevice.cpp` (`:1125` and `:1035`). That line asks for FIFO and logs "vsync", yet play runs at 190-900 fps with Present
  near 0.1 ms. Confirm the present mode in use, test a focused window, check for a driver override. The result is a fix or a named
  cause in the open items; `ActiveFrameRate` works either way. Both the Performance panel and `physics-timing` assume the frame rates
  the owner sees until this is answered.
- **S0.9 `Scene.Timing` and `FixedRate` (S).** A project section following `Scene.CollisionLayers` (`ApplyProjectLayers`), not
  `Physics.Simulation`, because the step also drives scripts. `FixedRate`: Scene (default, so 60 stays), 30, 60, 120, 240; 30 warns
  about soft contacts.
  - Max steps becomes about 0.133 s of time (at most 64) with a project rate: 8 steps lose time at 240 Hz. A new
    `Scene::GetMaxFixedSteps()` carries it, and both readers go through it: `Scene.cpp:2174` and `LimitToFixedSteps` at
    `GameRuntime.cpp:38`, which otherwise goes on dropping time at 240 Hz.
  - The three direct readers of `FixedTimestep` (`ScriptHostAPI.cpp:325`, `EditorInputCommands.cpp:98`, `EditorWorldCommands.cpp:421`)
    go through `GetFixedTimestep()`. So does the Physics summary's steps per second (`DescribePhysics`, `InspectorPanel.cpp:260`).
    The Inspector's row shows as overridden.
  - Feature test, `Editor.Commands`, and a runtime test at 240 Hz with long frames that loses no step.

### Lane Slang

- **S0.10 Every shader in Slang (L, box of 2 agent-days).** HLSL-syntax Slang in `.slang` files, compiled by slangc, still SPIR-V on
  Vulkan and MoltenVK. Work on `ci/slang`, merge only through the gates. The mechanical conversion is one change.
  - Pin: `CMake/StrataSlang.cmake` fetches v2026.20 at configure time, verified by SHA-256, into a per-user cache under
    `file(LOCK)`, with a `STRATA_SLANG_ROOT` override (D1). ThirdPartyNotices. The glslang submodule leaves (about 900 MB).
  - `strata_add_shaders` becomes two commands for every entry, so there is one path. slangc writes a `.spv`, with
    `-fvk-{t,s,b,u}-shift` 0, 128, 256, 384, `-matrix-layout-column-major`, `-default-image-format-unknown`, the two warnings disabled
    with a why-comment, and `SLANG_RUN_SPIRV_VALIDATION=1`, which gives CI real SPIR-V validation for the first time. Then a small
    C++ host tool built by CMake (Python is not a build dependency) adds the decoration below and writes the header as `--vn` did
    (`g_StrataShader_<id>`). slangc's `-source-embed-style` is not used: it would write the header before the patch.
  - The `Invariant` decoration: Slang has no way to say it, and the depth prepass's EQUAL test depends on it. A unit test checks that
    `Scene/Mesh*.vert` carry it. slangc's validation runs before the patch, and Slang's archive has no spirv-val. So after the patch a
    CI step on the Linux job runs apt's `spirv-val` over all 103 modules, and the validation layer checks every module the GPU suites
    create. All of this counts inside the box.
  - The converter (`.worktrees/wave8-plan/slang/spike/tools/`) enters as a Tools script for the conversion commit and leaves in the
    next one (history keeps it). Hand fixes and the prelude go into `Include/Common`. `FitFilmic.py` and `FitAOMultiBounce.py`
    write `.slang`. `Architecture.Shaders` is ported and a scan rejects Float16, Int16 and derivative capabilities. Shadow input
    layouts are trimmed (a validation warning otherwise). Docs: 253 mentions, the AGENTS.md Shaders bullet, the new rules (4.1).
  - The console route stays checked (S): a test runs slangc `-target hlsl` over every entry, as the spike did for 103 of 103, with no
    DXC. New shaders cannot quietly close the owner's reason for this item.
  - **Gates, all of them:** (1) reflection diff shows only the audit's explained differences; (2) `spirv-val` 103 of 103 and the
    validated GPU suites with 0 errors, Debug and Release, feature test, samples; (3) look-dev compare within the audit's numbers
    (median largest difference 1 level without AA, nothing structured in a signed amplified difference; a shape like
    `tp.lantern`'s is a bug); (4) GPU time within control noise; (5) `ci/slang` green on every job, the two checks above included;
    (6) MoltenVK, below.
  - **MoltenVK is a gate here, not at the release.** Slang's SPIR-V has never been through MoltenVK's conversion, `ci/slang` proves
    compile only, and every later stream builds on this. If the Mac can be reached, the GPU suites run there before the lane lands.
    If it cannot, the minimum is a step on the macOS CI job that converts all 103 entries to Metal, with MoltenVK's own converter if
    Homebrew's `molten-vk` carries it, else `spirv-cross --msl`, and compiles them with `xcrun metal`. That is a lower bound: the
    engine's bindless argument buffers are not configured the same way. The first real GPU run is then an open item and section 7's
    Mac gate stays.
  - If the gates are not green at the box's end, one extension is allowed if every remaining failure is understood and small. After
    that the rendering streams branch from main with glslang and this becomes wave 9's step 0. The converter re-runs on the new GLSL,
    so nothing is lost. S0.11 then lands in GLSL.
  - The lane's last act, after merging main with lane Render in it: the look-dev baseline in Slang, which S0.11 compares against. If
    the box ended without Slang, the same baseline on main in GLSL.
  - Below the box: moving the converter's `static` stage globals to entry parameters.

### Last

- **S0.11 The indirect block's seam, slots and stages (S-M).** `Forward.frag`'s indirect block has three branches, and both `reflections`
  and `radiance-cascades` must change it. The two terms share inputs: `EvaluateReflectionProbes` returns irradiance and radiance in one
  call, `sky = 1 - probes.Weight` scales both, `(1.0 - specularColor)` enters the diffuse term, `diffuseOcclusion` feeds specular
  occlusion, and one branch multiplies the summed result by `EnvironmentParams.x`.
  - It becomes, in `SH/Include/IndirectLight.glsl`, a struct `IndirectInputs` that S0.11 owns (filled once per pixel: `specularColor`,
    the reflected direction, occlusions, `probes`, `screenReflection`, `sky`, `environment`), `GatherIndirectDiffuse`,
    `GatherIndirectSpecular`, and a small compose step that keeps each branch's scale on the sum. Pixel-equal: GPU suites and 0 of 84
    look-dev pictures differ from lane Slang's baseline.
  - Slots in `SH/Include/Scene.glsl`'s comment: SRV 30-31 the irradiance volume, 34 volumetric fog, 35 radiance cascades, 36 fog
    volumes, next free 37. `ReflectionParams.y` and `.z` for GI on and intensity, `.w` left to `reflections`.
  - Stage names `GPU.RadianceCascades` and `GPU.Volumetric`, debug views `gi` and `open`.
- **S0.12 Roadmap (S).** The wave 8 entry and the "Later" edits, with wave 9's entries for animation (`animation-tools`,
  `animation-runtime-2`, ragdoll nodes). They go in here, not in `animation-graph`'s A0, so the owner's "record and push into wave 9"
  holds whatever happens to that stream.

### Landing step 0

S0.0 and S0.1 land first. Then each lane lands by itself when it is verified: review, Debug ctest with GPU suites under the validation
layer, Release ctest without perf, `CompatLint.py`, its `ci/wave8-<lane>` green. S0.7 lands ahead of its lane if it is ready first,
because graybox needs it; S0.2 and S0.3 likewise. Lane Slang lands through `ci/slang`, then S0.11 and S0.12. `build/windows` is rebuilt
after each lane that the owner can see, so the early items reach the owner early.

## 4. The streams

**Capacity.** This wave's nine streams come to about 50 audit agent-days of building, on a bigger step 0, with two landings. Audit days
are a unit of size, not a calendar: `animation-graph`'s 10 days are 10-15k lines, the size of one wave 7 stream. By what was built, the
whole wave is about two wave 7s, so about two weeks of the owner's usage, if tranche 3 stays out. That is an estimate from one data
point, and cut lines will not absorb an overrun, so the work is ordered by what stops first.

| Tranche | Holds | Starts when | Ends with |
| --- | --- | --- | --- |
| 1, committed | Step 0, `showcase`, `reflections`, `physics-timing`, `radiance-cascades` through G1 | Now | Landing 1: main is releasable as 0.8.0 |
| 2, intended | `animation-graph` (first claim), `radiance-cascades`' join and hardening, `local-lights` | Landing 1 is on main and the usage rule allows | Landing 2 |
| 3, wave 9 unless usage remains | `editor-ux`, `volumetric`, `showcase-engine`, in that order | A tranche 2 slot frees and the usage rule allows | Joins landing 2 if verified; otherwise wave 9, branch kept |

**The usage rule.** The orchestrator cannot read the owner's weekly usage (D8): it goes by what the owner reports. Wave 7's
trimmed integration took more than 11% of a week without its perf A/B, sheets, dry run or CI, so a whole landing is reserved at 25%.

| Weekly usage | What may happen |
| --- | --- |
| Below 60% | A stream of the open tranche may start |
| 60-75% | No new stream. Running streams finish, review, fix and verify. An integration may start |
| Above 75% | No integration starts; it waits for the reset |
| 90% | No new agent. Running agents commit to their branches and stop at a clean point; what they were verifying runs again after the reset |

A tranche may cross a weekly reset: the owner's list has no date, and the wave 7 plan's "after the reset" worked. What this rule prevents is
a landing cut short, as wave 7's was. Only the owner's order ships one short. If the wave has to end early, tranche 2 moves to wave 9 and
0.8.0 is cut from landing 1 through section 7's steps.

| Key | Title | Size | Tranche, merge order | Owns (exclusive) | Shares (additive, resolved at merge) |
| --- | --- | --- | --- | --- | --- |
| `showcase` | High Desert Outpost | XL | 1, landing 1 | `Samples/Showcase/`, `Tools/GenerateShowcaseTerrain.py`, `Tools/GenerateShowcaseProps.py`, `StrataTests/Editor/ShowcaseCommands.json.in` | `Samples.json`, `SampleContent.h`, `Tools/LookDev.py`, `Shots.json`, `ThirdPartyNotices.md`, `StrataTests/CMakeLists.txt`, `Budgets.json` |
| `reflections` | Metal and wet ground | L | 1, part R1 alone, the rest 1st at landing 1 | `SH/Effects/ScreenSpaceReflectionsSpatial.comp`, `ReflectionResolve.comp`, `SH/Include/ScreenReflections.glsl`, `ReflectionProbes.glsl`, the specular gather, `R/SceneRendererScreenReflections.cpp`, `R/SceneRendererReflectionProbes.cpp`, `ProbeBake.*`, `Docs/Manual/Lighting.md` reflections section | `Forward.frag` (head, sun term), `R/SceneRenderData.h`, `PBR.glsl`, `DepthPrepass.frag`, `AtmosphereSkyView.comp`, `R/SceneRendererAtmosphere.cpp`, `LookDevScene.json`, `Shots.json` |
| `physics-timing` | Smooth at any frame rate | L | 1, 2nd at landing 1 | `Scene/Scene.cpp`'s fixed loop and world matrix update, `Scene.Timing`, `RenderLights` | `RenderScene.cpp`, `PhysicsWorld.cpp` (teleports only), `ScriptABI.h`, `ScriptHostAPI.cpp`, `StrataScript/*`, `GameRuntime.cpp`, `EditorContext.cpp`, ThirdPerson's `FollowCamera` |
| `radiance-cascades` | Indirect light from the screen | XL | 1 to G1, 2 for the rest, 1st at landing 2 | `R/SceneRendererRadianceCascades.cpp`, `SH/GI/*`, the diffuse gather, `PostProcessComponent`'s GI fields, below the cut `IrradianceVolumeComponent`, `.stvolume`, `lighting.bakeVolume` | `Forward.frag` (the open share in specular occlusion), `R/SceneRenderer.cpp/.h`, `R/SceneRendererGlass.cpp` (the color copy), `R/SceneRendererAmbientOcclusion.cpp`, `Scene.glsl`, `SceneRenderData.h`, `EditorViewportCommands.cpp` and `ProjectThumbnail.cpp` (settling), `Shots.json` |
| `animation-graph` | The animation graph, first slice | XL | 2, any time, touches no renderer file | `Strata/src/Strata/Animation/*`, `Editor` animation commands and views, `Docs/Manual/Animation.md`, ThirdPerson's `Robot.stanimator` | `AssetDependencies`, `Inspector` tables, `ThirdPersonCommands.json.in`, `FeatureTest` |
| `local-lights` | Many shadowed lights | L | 2, 2nd at landing 2 | `R/SceneRendererLocalShadows.cpp`, `R/ShadowAtlasAllocator.*`, `SH/Include/LocalShadows.glsl` | `R/SceneRendererGPUDriven.cpp`, `SH/Scene/Cull.comp`, `Forward.frag` (its light loop), `R/SceneRenderer.h` (stats), `EditorStateCommands.cpp` |
| `editor-ux` | The rest of drag and drop | L | 3, 3rd at landing 2 | `R/SceneRendererPreview.cpp`, `SH/Overlay/PreviewMesh.*`, `Editor/PlacementPreview.*`, `Panels/ViewportPanel.cpp`, `Panels/PerformancePanel.cpp` | `RenderScene.cpp`, `SceneRenderer.cpp` (`RenderOverlays`), `ViewportRenderer.cpp`, `EditorLayer.cpp` |
| `volumetric` | Fog with shafts and cones | L | 3, 4th at landing 2 | `R/SceneRendererVolumetric.cpp`, `SH/Volumetric/*`, `Include/VolumetricShadows.glsl`, `Fog.glsl`, `HeightFogComponent`, `FogVolumeComponent` | `Forward.frag`, `Skybox.frag`, `Scene.glsl`, `SceneRenderData.h` |
| `showcase-engine` | Layers on materials, stills | L | 3, 5th at landing 2 | `Asset/Material*`, `SH/Include/MaterialTextures.glsl`, `R/SceneRendererExtraction.cpp`'s material UV scales | `Forward.frag`, `DepthPrepass.frag` (the shared surface function), `Cull.comp`, `MaterialData`, `ViewportRenderer`, `EditorViewportCommands.cpp` |

**Who owns what in `Forward.frag`:**

| Region | Owner |
| --- | --- |
| Head of `ShadeLitSurface`: surface, F0, DFG, `SpecularEnergy` | `reflections` (IOR to F0). `showcase-engine`'s layer blends upstream of it, in the surface function |
| Sun term and `LightSurface` | `reflections` (the sun's size) |
| The light loop | `local-lights` (the forward skip) |
| `IndirectInputs` | Shared and additive, as S0.11 left it |
| The specular gather | `reflections`. `radiance-cascades` adds the open share to specular occlusion after `reflections` merged |
| The diffuse gather | `radiance-cascades` |
| The fog applied at the end | `volumetric` |

**Order and slots.** Stream worktrees are `.worktrees/<key>`, builds `build/agent-<key>`. Days count from S0.12's tip, about day 4 of
the wave. The right-hand column is the audit's days times about 1.5, because each stream also needs a review, a fix round and
verification, in its own slot. Slot 4 stays with the landings: they are build-heavy, and they do not stall a stream or break the
limit of four.

Tranche 1:

| Slot | Work | Build days | With review, fix, verify |
| --- | --- | --- | --- |
| 1 | `reflections`: R1, its landing, then R2 | 2, then 1.5 | 3, then 2.5 |
| 2 | `radiance-cascades`: stages 0 and 1, to G1 | 4 | 4-5, no landing |
| 3 | `physics-timing` | 3 | 4.5 |
| 4 | R1's landing, the sample agent's editor rebuilds, landing 1 | | |
| None | `showcase`'s sample agent: it drives a Release editor built from main and rebuilds only when main moves | About 9 | Starts at once |

So landing 1 can expect `physics-timing` and `reflections`, no more. If they are not verified it goes without them.

Tranche 2:

| Slot | Work | Build days | With review, fix, verify |
| --- | --- | --- | --- |
| 1 | `animation-graph` (protected: nothing takes this slot) | 10 | 15 |
| 2 | `radiance-cascades`: join, settle and hardening | 3-4 | 5-6 |
| 3 | `local-lights` | 4 | 6 |
| 4 | Landing 2 | | Measure: section 7, with every slot empty |

Tranche 3 takes slots 2 and 3 as they free, in the order `editor-ux` (1.5 above its cut, 2.5 with review), `volumetric` (4 for steps 1-5,
6 with review), `showcase-engine` (2-3, 4 with review).

Start conditions:

- `showcase` starts at once, beside step 0: its assets and generators touch nothing else. Its graybox waits for lane Render and S0.7.
- `physics-timing` starts when lane Render has landed. It edits no shader, so it does not wait for lane Slang, but it repeats S0.3's
  checks with interpolation on.
- `reflections` and `radiance-cascades` start from S0.12's tip: nothing that edits a shader starts before S0.11's seam, slots and baseline.
- `animation-graph` touches no shader. It may start before landing 1 only if `physics-timing` is verified, G1 is decided, a slot is free
  and usage is below 50%, since landing 1's reserve comes first.
- `local-lights` branches after landing 1, so its perf step runs with `physics-timing` in it, because the two costs multiply (risks).
- `volumetric` starts only when G1 is decided and `local-lights` is verified. If G1 fails, slot 2 goes to the irradiance volume first.

Merge constraints behind the order: `reflections` before `radiance-cascades` (the specular occlusion both read), `physics-timing` before
`editor-ux` (`RenderScene.cpp`), `local-lights` and `radiance-cascades` before `volumetric`. A stream ready before one ahead of it may land
first, as in earlier waves; the later one then merges main.

### 4.1 Rules every stream follows

- **Testing.** Debug ctest and every `GPU*` suite under the validation layer before each commit; GPU suites use `Tests::GPUContext`
  and end with `CHECK(gpu.GetNewErrorCount() == 0)`; exact-pixel tests turn new defaults off. A regression test fails without its fix.
- **Perf.** No campaign, no `PerfStability.py`, no `-L perf` run against budgets in a stream. A stream may A/B its own stages,
  interleaved, under `Tools/PerfLock.py`, as information. New metrics enter `Budgets.json` as `Provisional` with their target. PerfLock
  serializes only measuring agents, and wave 7's stage times jumped up to 3 times under another stream's GPU suites: readings beside
  running streams are ratios to a stage in the same frames, or information, never a pass.
- **Quality rows.** A costly knob is a row of `RendererQualitySettings` and a field of `Renderer.Quality`, one line with its four
  presets, with its line in the traits table (a row that allocates names its bytes function there). Each landing bumps the section's
  version by one from 4 (the integrator renumbers).
- **Both paths and the caches.** New inputs of how casters are drawn change a version the shadow caches compare;
  `GPU.SceneRenderer.GPUDriven`, `.Feedback` and the cached-against-uncached case pass; derivatives only of the primitive's own
  interpolants and textures.
- **Off costs nothing.** No target, no pass, a dummy where a layout needs one, new work in `Forward.frag` behind uniform branches,
  every new pass in a `GPUPassTimer` stage. `FrameConstants` grows only for what `Forward.frag` reads, appended at the end.
- **Shader language.** New shaders in whatever S0.10 left. In Slang, HLSL syntax only, no generics, no `__target_switch` or
  `spirv_asm` outside the prelude. `mul` only with a matrix (`mul` of two vectors is a dot product and `M * M` is componentwise, both
  silently). Vector comparisons need `all()` or `any()`. Matrix constructors take rows, indexing is `M[row][col]`. `SampleLevel` in
  compute. A lighting bug in code nobody changed may be a conversion slip: check the signed look-dev difference against 0.7.0.
- **Agents reach everything through commands:** properties by `component.set`, sections by `project.setSettings`, debug views with
  statistics in the capture's result, actions through described commands with complete schemas. New commands stay out of `core`.
- **Feature test:** every new property a non-default value, every host function a call. Host functions append; the ABI keeps its
  version.
- **Documentation:** a DG section, the Manual page, one AGENTS.md digest bullet of at most 250 characters, release-note lines for
  every default it changes, a `Docs/Roadmap.md` note of what moved below its cut.
- **Samples:** changed through editor commands on the samples, never by hand-editing documents; their `Editor/<Sample>Commands.json.in`
  stays passing; pictures retaken after merging, binary conflicts resolved by taking main's and retaking.
- **Running editors in parallel:** private session directories, `StrataCLI call --project` always (without it, it reaches the newest
  editor), scripts in files (a script on stdin hangs), and never the owner's `build/windows`.
- **Scope.** Steps run in order; each lands working, tested and documented before the next starts. Below a cut line only when
  everything above is done, and only if the usage rule allows. Open-source projects and their code may be read and used as much as helps (D10: permissive
  licenses only, notices kept); other folders on this computer never.

### 4.2 `showcase`: High Desert Outpost

Goal: a sample made through the editor that shows the wave's look on the metal and wet ground the owner asked for, runs as a tour,
a walk and a photo mode, and becomes the reference scene for the presets' budgets. It lands in landing 1 and is improved as streams
arrive. Size XL, about 9 agent-days, one agent for most of the wave.

**What item 4 means here.** The owner confirmed it on 2026-11-01: highlights and reflections on the surfaces themselves, and "highlights
are extremely important, just like in a drawing, because they should be cheap". So shots 2-4 are judged first on their highlights (the sun's
streak on a sheet, the curved streak on the tank, the glint column on the apron, a lantern's glint on metal at dusk), then on their
reflections (the world in a tank's side, the bus in a puddle). Sun thrown off a sheet onto nearby surfaces is wanted later, not a priority
(section 9).

**The place.** A derelict fuel stop in a rocky high-desert valley, the morning after rain, which gives a desert its puddles. An
original place, made through the editor. A generated bus stands on an apron with a vertical oil tank and a horizontal one, a
plaster-and-plank hut with a lean-to and `covered_car`, barrels, a fire pit and a lantern, with cliffs and boulders behind. Layout
in the audit (section 5). Inner terrain 300 m at 1 m spacing, a coarse ring to about 1.2 km.

**Design rules, checked by the generators:** terrain toward the golden sun stays below 5 degrees (8.7 m per 100 m), or the low sun
is blocked and shots 2-4 fail. The golden station sits at 5-7 degrees: the sheet's highlight centers at camera height plus
`t * tan(elevation)`, so a 2.9 m bus and a 1.6 m camera allow 8 m at 9 degrees and 15 m at 5.

**Six shots:**

| # | Shot | Station | Proves |
| --- | --- | --- | --- |
| 1 | Dawn vista from the rim, hero boulder in front | Dawn | Aerial perspective, long soft shadows |
| 2 | Side of the bus, camera within 8-15 m | Golden | Sun highlight on weathered sheet steel with bare panels, probe reflections |
| 3 | Oil tank at low sun | Golden | Curved metal streak, weld seams, ground in the reflection |
| 4 | Wet apron, camera at 0.6-1 m into the sun | Golden | Glint column, reflections of the bus and tank in puddles |
| 5 | Hut interior from the doorway | Golden | Sun patch, occlusion, indirect light |
| 6 | Dusk to night | Dusk | Shadowed lantern and fire, metal catching firelight, exposure |

The rock macro is a photo-mode bookmark. Contact shadows leave the shot list. **Shots 2, 3 and 4 are never cut.**

**Steps:**

1. **Freeze the assets (S).** The A-list, 72.2 MB with everything: ground and walls 20.27 MB (`rocky_terrain_02`@1K, `dry_ground_01`@2K,
   `asphalt_02`@2K, `brown_mud_03`@1K, `beige_wall_001`@2K, `weathered_brown_planks`@1K); metal 11.00 (`rusty_painted_metal`@2K,
   `green_metal_rust`@2K, `rusty_corrugated_iron`@1K, `rusty_metal_03`@1K); rock 13.46 (`namaqualand_boulder_02`, `_boulder_03`,
   `namaqualand_cliff_01`, all 1K); `dry_branches_medium_01` 3.25; props at 1K 12.80 (`barrel_03`, `Barrel_01`, `_02`, `stone_fire_pit`,
   `Lantern_01`, `old_tyre`, `covered_car`, `ladder_sectioned_01`); generated 11.45 (terrain, props, masks, icon, six probes). Credits
   and notices rows from the API. DG's "showcase scans 2K" changes. The B-list stays out unless the sheet shows softness (D2).
2. **Terrain generator (M).** Time a fresh import of the real set here: the package test has 1800 s, and this is unmeasured.
3. **Props generator (M to L).** Bus (rounded body, rub rails, panel seams, wheel arches, boarded windows, UVs in meters), tanks
   (lathe, weld seams, saddles), pipes, the hut shell. The owner named metal sheets, so the bus side is weathered steel with no clear
   coat. Coats get no SSR (`Roadmap.md:877`), and a scan's metal channel can be zero (`container_side`, `p12`). Check the metal channel of
   `rusty_painted_metal` before relying on it; worn panels show bare metal through factors or `rusty_metal_03`. Fallback if the bus reads
   as a box: containers with `container_side` (+1.59 MB, with bare-metal factors) keep the sheet shot.
4. **Graybox (M), after lane Render and S0.7 land.** Layout, three stations with all six probes baked and switched in play, six shots,
   a first perf reading (information: the machine is loaded), and the owner's first sheet. Scans import with a higher `LODCount`. Check
   S0.3's 10% and 0.5 s against the sample's traffic. **Exit: a capture of all six shots on main, with these checks, before the L of
   dressing starts:**

   | Shot | Check | If it fails |
   | --- | --- | --- |
   | 1 | The reach of shadows: cliffs' and boulders' shadows at about 600 m, read in the `shadows` debug view (the last cascade is an unfitted sphere, and distant shadows stay in "Later") | Raise `ShadowDistance` within the cascades' limit; else reframe so that shadows that matter fall within reach and aerial perspective carries the rest |
   | 2 | The highlight lands on the panel at the computed height and reads as metal | Turn the bus or move the station; check the metal channel |
   | 3 | The ground and neighbors show in the tank from the baked probe | Move or resize the probe box |
   | 4 | The bus and tank in puddles, with no comb along straight edges, no sky wedge, no misplaced patch | R1 if it has landed. If not, write the fallback from this capture: framing or geometry with no long straight edge across a puddle |
   | 5 | A dark hut from one box probe: interior luminance at most a third of the yard's on the same albedo (proposed; the CPU model's reference is about a sixth) | A smaller box, a narrower door, a bake at the right station; then unshadowed fill lights |
   | 6 | Lantern and fire read at dusk, exposure holds, metal catches firelight | Hold the shot at dusk, not night, until `reflections` fixes the sky's bands |

5. **Dress and tune materials (L), after R1 has landed and the sample agent's editor is rebuilt.** Metal with bare-metal scans or factors;
   a roughness tuned on painted sheets (factor 1 gives no highlight, 0.45 does); opaque water pieces at roughness 0.02-0.06 dipped into
   the apron. Tuning against the old SSR and F0 would be done twice.
6. **Scripts (M).** A tour of about 80 seconds, Catmull-Rom over markers with fades between stations, logging `Showcase: shot N`. Any key
   hands over to a first-person walk, Tab to free flight and the photo mode: free camera, sliders for time, exposure, focus, f-number and
   field of view through `SetProperty`, the Graphics menu through `Graphics::Options()`. Default High.

   **Stations are six probe entities**, `Yard` and `Hut` for each of Dawn, Golden and Dusk, not two probes baked three times. A bake
   trashes the capture an earlier bake made for that entity once nothing uses it (`EditorLightingCommands.cpp:366-386`). Each station
   bakes through `lighting.bakeProbes {entities}` with its sun set and only its own probes active, since a bake's renderer reads active
   probes. Scripts switch stations with `SetActive`: an inactive probe is skipped (`SceneRendererReflectionProbes.cpp:185`). So no swap
   of `Capture` is needed, and each capture stays in the scene's references, so pruned exports keep it. `Assets::RequestLoad` of all six in
   `OnCreate` keeps them resident. A switch activates the new station and leaves the old one active for about a second: until the
   new capture is converted it is not in the list and the old probe lights the box, so the box never falls to the sky (`:291`), which is the
   bright flash a photo mode has no fade to hide. Converted captures stay in their slots, so going back converts nothing.
7. **Wiring (M).** E3: a showcase screenshot allowance in `SampleBudgets` (today 3 MB for 3 files; about 10 MB fits in the 85 MB
   total). E4: `Tools/LookDev.py` takes the sample, shots `showcase.*` appended. `Samples.json` with `"Budget": "Showcase"`, `README.md`,
   `CREDITS.md`, `Picture.png`, `Assets/Icon.png`, `ThirdPartyNotices.md`. CTest `StrataEditor.Showcase` (build, play, reach `Showcase:
   shot 6`, photo mode, change a graphics option, export full and pruned, run both, no unexpected warnings), the windowed run, image
   and perf tests, `PerfGPU.Samples.Showcase.{High,Ultra}` `Provisional`.
8. **Land (section 7).** Then one adoption pass per stream that landed, and the owner's sheet.

**Cut line.** Grass is already out (it needs coverage-preserving alpha). Below the cut, first to last: the B-list; walking (keep tour and
flight); Dusk's Hut probe.

**What it depends on, and how it lands without it:**

| From | Used for | If it falls short, the landing uses | Adoption pass when it lands |
| --- | --- | --- | --- |
| S0.2, S0.3 | Every shot | Graybox on main and recheck: both are small and in step 0 | |
| S0.7 | Placing props | `prefab.instantiate` and `entity.dropToGround`, slower | |
| `reflections` R1 | Shots 2-4 | The zero-code list does not cover shot 4: water at roughness 0.02-0.06 stays under any SSR roughness limit, with every defect. Shots 2-3 can use `ScreenSpaceReflectionsMaxRoughness` 0.15 with sheets at 0.18 or more. Shot 4 uses the fallback written from graybox's capture (above) | Raise the limit, retune, retake |
| `reflections` R2 | F0 of water, shot 6's sky, sharper probes | 0.04 water with a darker base, a dusk sky without the night's bands, 128 probes | Retune, retake |
| `radiance-cascades` | Shots 5-6, shadow sides | A box probe in the hut carries 9 coefficients; unshadowed fill lights where needed | Turn on after the owner's live look; check flicker in shot 6 |
| `local-lights` | Shot 6 | At most 4 shadowed lights, works today | Yard work lights |
| `physics-timing` | The walk | A per-frame ground-following walk (S), because a 60 Hz walk shows the jitter the owner complained about | Character controller |
| `showcase-engine` | Ground at the feet, stills | Separate terrain meshes per material, joins hidden by rocks and barriers; stills at native 4K | Blend; supersampled stills |
| `volumetric` | Shots 1 and 5 | `HeightFog` haze in shot 1, no fog in any shot that looks into the hut, sun patches instead of shafts, no fake cones | Shafts, mist |
| `animation-graph`, subsurface | Nothing | No people: the blockers are a realistic body and skin, not the animator | A later update if a body is found |

Defaults it changes: none. A new sample.

### 4.3 `reflections`: metal and wet ground

Goal: at High and Ultra, in every AA mode, a metal sheet at a grazing view shows a clean reflection of what stands beside it, and
reflections stop showing the sky where the world belongs. Medium's Checkerboard path (`ScreenSpaceReflections.comp`) stays: it needs TAA
and holds 0.6.0's costs, and the open items name it. Size L, about 3.5 agent-days above the cut, in two parts because the showcase tunes
its materials on the first.

**R1, the first deliverable. It lands alone through `ci/wave8-reflections`, before showcase step 5.**

1. **The shots and metrics (S).** The specular lab's sheet, wet plane and tank in `LookDevScene.json`, shots appended (`lookdev.sheet`,
   `.wet`, `.tank`). The audit's noise metric in the harness, a flat-area variance bound as a GPU test, and a camera strafe on `.sheet` and
   `.wet` with a frame-to-frame flicker metric (the second difference over three frames, over 32 px blocks) under no AA, FXAA and TAA.
   The audit took stills only, so this measures what nobody has yet.
2. **The resolve (M), prototyped first on the sheet shot.** It is not the S the audit thought. `ReflectionResolve.comp` is a bilateral with
   lobe, depth and normal weights over a groupshared tile with a 1-pixel border (`c_Tile = c_Group + 2`). A 5-tap kernel (0.5, 1, 1, 1, 0.5)
   needs a larger tile and 2.8 times the taps against a 1.2 ms target, and it cancels the pattern exactly only with uniform weights. Two
   candidates go on the sheet shot, chosen by noise and cost: that 5x5, or a march whose offsets repeat every 3 pixels (`c_PatternOrder`
   today repeats every 4), which today's 3x3 cancels as it stands, at 9 offsets instead of 16.
3. **Edge fade (S).** Fade only along the axis the ray travels toward. `ScreenReflections.glsl` is shared, so Medium's fade improves too,
   at no cost.
4. **Crossing test (M).** A conservative crossing test with hit validation after the bisection, continuing the march on a false hit;
   the fixed steps stay, because a purely hierarchical march failed on floors in wave 7.
5. **Specular occlusion off the SSR term (S).** Dark smudges beside objects on wet ground.
6. **The misplaced probe patch (S).** On wet ground where SSR misses, the box-projected probe puts a bright blurred patch in the wrong
   place (`showcase` audit, `p14`). Find the cause; the comb at `p06` and `p10` should be gone with steps 2 and 4.

**R2, the rest above the cut. Highlights first:** the owner calls them extremely important, like in a drawing, and cheap. Step 7 starts as soon
as R1 is in review, and it may join R1's landing if it is verified by then.

7. **Highlights (S, S, S-M).** The sun's size in specular (Overcast's 10 degree sun is a pin dot today); lantern and fire highlights on metal
   and wet ground, judged on `lookdev` and shot 6, which the audit could not judge under its exposure; a source radius for highlights only (a
   property per point and spot light, energy-normalized so a larger source widens the highlight without brightening it, one evaluation per
   light, no change to diffuse or shadows). Every highlight is judged with no AA, FXAA and TAA: one that shimmers or aliases fails, and specular
   anti-aliasing keeps its rules (derivatives only of the primitive's own interpolants and textures).
8. **Materials (S, S).** IOR to F0, with 1.5 at exactly 0.04 (water 0.02, imported `KHR_materials_ior` values count), and the white
   furnace run again after it; the night sky's bands, hypothesis half-float subnormals in the sky-view table, needed by shot 6.
9. **Back-facing hits (S).** A filled silhouette at lower confidence, or accept crescents, decided by looking; `GPU.SceneRenderer.SSR`
   asserts today's behavior.
10. **Probes at High (M).** The `ReflectionProbeResolution` row, 128, 128, 256, 256. No re-bake: captures are already 1024x512 from
    256-texel faces (`ProbeBake.h:25-27`). It is not free. The cube array is allocated whole for up to 16 slots in RGBA16F, so a renderer
    that shows a probe holds about 67 MiB instead of 17, and the row gets its bytes function in the traits table. Conversion quadruples,
    and `c_ReflectionProbeConversionsPerFrame` is 2, so a station change could take about 28 ms in one frame (scaled from one rough 3.6 ms
    reading). Measure it. Above a limit of 2 ms a frame (proposed) spread a conversion over frames; captures and bakes keep converting at
    once (`SpreadLightingUpdates`). If that does not hold, the row stays 128.
11. **Adoption and docs.** Samples that use SSR checked and pictures retaken; DG "Reflections", the Roadmap's open items 877 and 887
    updated; whether Spatial SSR's two images survive the repair is recorded; Checkerboard named as the one effect that needs TAA.

**Cut line.** Below it, in this order:

12. Keyframed captures (L): N sun positions per probe, the two nearest blended into the slot and irradiance on the GPU, one face a
    frame. Only if a moving sun is worth it beyond the showcase's stations.
13. The rest of sized point and spot lights (their diffuse and shadows) stays in "Later"; step 7 gives highlights their size.
14. Hit-distance blur (M), a wet-surface model (M), normal-map-aware filtering (M, importer bump), anisotropy (M), SSR on clear coat (M):
    no shot needs it, because the bus has no coat.

Tests: edge fade on a floor with rays up the screen; the variance bound; the strafe's flicker bound; a validated hit on thin posts; AO
leaves SSR alone; F0 from IOR with 1.5 unchanged; the furnace; the row round-trips through `SetQuality`. Perf:
`PerfGPU.Renderer.SSRSpatial4KStageGPU` holds its 1.2 ms target (`Provisional`).

Defaults it changes: SSR's image in every scene that uses it; F0 of materials with an IOR other than 1.5; probe slots and their memory at
High and Ultra.

### 4.4 `radiance-cascades`: indirect light from the screen

Goal: the Path of Exile 2 technique, adapted for a free camera: bounce light, color bleeding and large-scale occlusion that follow a
moving sun within a few frames, with no history buffer and no accumulation, at about 1.2 ms at 4K High. Size XL, 6-8 agent-days. Stages 0
and 1 run in tranche 1, the rest in tranche 2. It must not hold anything else up.

**Design** (the audit's section 3 has it in full; a planar summary here). Image-plane wedges with an elevation bitmask of 32 sectors;
7 cascades at High, spacing, wedges and interval doubling, cascade 0 at 4 px, 8 wedges, 48 px at 4K (8.3 M texels, the same as
1080p). Rays that leave the screen count as open and take today's light, so with nothing on screen the image equals today's exactly.
Light comes from the previous frame's lit color, reprojected by velocity, with a soft radiance cap. A pyramid of nearest depth,
farthest depth and near-layer share, on both paths, after the complete prepass (the HZB is not usable). Spatial filter of 4 à-trous
passes in probe space, stopped by plane and normal, then per-pixel upsampling. Between AO and Reflections (`R/SceneRenderer.cpp:3346`).
Diffuse becomes hit light plus open share times today's irradiance; screen AO stays for contact; `AOMultiBounce` turns off with it.

**What it is not.** It is a one-frame feedback loop. The previous frame's color already holds the previous frame's GI, so bounce converges
over several frames. Stills, captures and cuts have to treat it that way, as stage 2 does.

**How it departs from the published form.** The owner asked for "path of exile 2 technique", so the owner sees what that became:

| Published (the radiance cascades paper on Path of Exile 2) | Here | Why |
| --- | --- | --- |
| Fixed isometric camera, flatland-like hierarchy | Free camera; sectors run through the view vector, each probe limits them to its hemisphere | Strata's camera is free |
| 128-bit occlusion mask per cascade texel, its mapping to the hemisphere unpublished | 32-bit sector mask, wedges as image-plane directions, elevation as sectors. Our own design | Memory; the mapping is not published |
| 4 rays marched at once | Up to 6 steps per interval, one dispatch per cascade, a fractional cover beside the mask | Thin geometry's stability in the model |
| Intervals that keep the penumbra condition | A long first interval, 48 px at 4K, which breaks it in cascade 0 | Merges are the lossy step; faint fans around small emitters are the price |
| No temporal reuse, no denoiser | No temporal reuse; 4 à-trous passes | The published form flickers at 3.45% in the model |
| Emitters light dark scenes | About 70% of the indirect light passes through the open share, with today's light as the fallback | Daylight scenes, off-screen light |

Steps:

0. **Measure (M), tranche 1.** A GPU test that reads back the GI buffer and depth, the model's flicker measure on a fence, a grass card
   scene and the look-dev room, in new look-dev shots. The ExileCon 2023 talk is tried again, published material only; what it shows
   corrects the table above. Exit: the metric runs on today's build.
1. **The cascades behind a debug view (L), tranche 1.** `debugView: "gi"` and `"open"` with statistics, High only, nothing joined to
   shading, both paths, then gate G1.
2. **Join shading (L plus S-M), tranche 2.** The diffuse gather, `PostProcessComponent::GlobalIllumination`, intensity and
   `GlobalIlluminationOcclusion` (1: the open share scales the fallback; 0: bounce only) with `InactiveReason`s, the row, `SetQuality`, off
   path with a dummy at SRV 35, feature test, `PerfGPU.Renderer.GI4K` `Provisional`. The open share enters specular occlusion, in the specular
   gather, after `reflections` merged. Settling is part of this step: a `GlobalIlluminationSettled` stat beside
   `ScreenSpaceReflectionsPending`, waited for by `viewport.capture` and `ProjectThumbnail`, which today wait one frame for this class
   of input and would catch GI mid-convergence, in look-dev A/Bs too. On a cut the first frame shows the fallback and bounce rises
   over the next K frames, measured here; the tour's fades cover that, a game's cut will show it.
3. **Harden on real scenes (M to L), tranche 2.** Tuning on the samples and the showcase's yard: thickness, emission cap, edge fade.
   Lanterns and fire stay punctual lights (emitters under about 19 px at 1080p are not resolved). **Exit: the owner's live look in motion
   on Third-Person and the showcase, in the rebuilt editor with GI toggled.** A sheet of pictures cannot show flicker, which is what the owner
   rejected in contact shadows. The stream's docs land here: DG, Manual, the digest bullet.

Turning it on in the template and samples is not a stream step: section 7 does it after that look.

**Gate G1**, numeric, so the stream waits for nobody. On the GPU, no AA and TAA:

- Cost: `GPU.RadianceCascades` over `GPU.AO` in the same frames, medians of 5 interleaved runs at 3840x2160: at most 1.2 on `tp.scene`
  and 2.5 on `ow.scene`, which is 1.5 ms over 0.7.0's 1.23 and 0.59 ms under PerfLock. A ratio survives other slots' GPU suites;
  milliseconds are read alone at integration.
- Indirect flicker: at most 0.5% over 32 px blocks on the fence strafe, p99 near 3%.
- Foliage: at most 1.0% on the grass card scene.

**We expect the degrade path.** The model's best candidate reads 0.45-0.52% on the fence and 1.17% on grass, before normal maps, alpha cards
and GTAO. So the plan is one extra day of tuning, then in order: more filter; bounce only, with GTAO for occlusion; upper cascades off.

**What ships.** Full cascades on solid geometry. The fence row decides the join. A foliage miss alone does not stop it: the showcase has no
grass, GI is off unless a scene asks, and a scene with dense foliage takes `GlobalIlluminationOcclusion` 0, where the open share, which
carries most of the flicker, stops scaling the fallback. The join stops only if the fence misses in the bounce-only form too.

**If G1 fails** that way, the stream stops at its debug view, its branch is kept for wave 9, and the irradiance volume below moves above the
cut, which is the owner's item 2 as written ("have this if radiance cascade is preventatively hard"). A temporal filter on the GI buffer
would bring every case under 0.5%, but it breaks the rule against history, so it is not assumed: D7 says when it may be tried.

**Cut line.** Below it, cut first to last: the Ultra layout; bent direction; the player's Low value; AO at 2x6 when cascades run;
time keys; the irradiance volume.

5. **Irradiance volume (L).** Wave 7's design (`Docs/Plans/Wave7/specs/light-and-materials.md:1088-1124`) with 1 to 4 time keys:
   `IrradianceVolumeComponent`, `.stvolume` with L1 coefficients and validity per probe, `lighting.bakeVolume {times?}` reusing
   `ProbeBake`, two strongest volumes blended, SRV 30-31. Never updated at run time. With cascades it is simply the fallback.

Tests: an empty scene equals off, pixel for pixel; an emissive wall lights a floor within 15% of its form factor; a closed room with a
probe reads darker than without; two still frames identical in the 8-bit output after K settle frames, without TAA; paths equal; off
allocates nothing; a flicker bound.

Defaults it changes: none until section 7's adoption. The property is off; the row scales what a scene asks for.

### 4.5 `animation-graph`: the animation graph, first slice

Goal: Strata's own module on Esoterica's architecture: graph update separated from deferred pose tasks, sync tracks, layers with bone
masks, state machines as graph nodes, authored by agents through commands and proved on the Third-Person robot. About 10-15k lines
with tests. Tell the owner up front: no graph canvas in wave 8, and no people in the showcase.

The owner's words: "Add animation system from esoterica" and "This is the esoterica engine that the animation system should be based on".
They settle that Esoterica is the base: its architecture, concepts and evaluation are the owner's ask. On 2026-11-01 the owner also allowed
reading and using Esoterica and other open-source projects as much as needed (D10). So the stream reads Esoterica's source at commit c99a946,
with its talks and docs, and ports runtime semantics that are hard to re-derive (sync time math, transition behavior, event sampling, layer
sync) into Strata's own module, under its MIT notice. Vendoring is still out, for the technical reasons in section 9.

Steps:

- **A0 (S).** Esoterica's commit pinned (c99a946) in the stream's spec, and a section for derived code in `ThirdPartyNotices.md` (its header
  covers only submodules under `Strata/vendor/`) with the full MIT notice and every copyright line, added with the first ported code. Every
  commit that ports code names the source files it came from. Golden rule 1's new text is S0.1's.
- **A1 Pose tasks (M).** Sample, Blend, MaskedBlend, Reference; a per-update task arena and per-worker scratch poses (per-task heap
  allocation is wrong for 1,000 to 10,000 animators). Test: equals `AnimatorInstance::Evaluate` bit for bit on a two-clip fade.
- **A2 Sync tracks (M).** Markers, sync time and ranges, the blended track. Test: phase stays locked over long runs with different
  lengths and marker counts.
- **A3 Graph definition and instance (L).** Validated and sized at load (no asserts on data). Nodes: Clip, Blend1D, Layer, BoneMask,
  StateMachine, State, Transition (sync, time match, easing, root motion modes), parameters, compare, and, or, not, state completed,
  time, speed scale. Sampled events with weights.
- **A4 Into `AnimationSystem` (L).** Jobs run nodes and tasks; `WhenVisible` runs nodes and skips tasks; reach, root motion, events, hot
  reload, previews; the bit-exact thread-count test on graphs.
- **A5 `.stanimator` version 2 (L).** Same asset type, version 1 stays valid; corruption sweep; `AssetDependencies`; complete command
  schemas; `animation.info` with live node weights and sync; Inspector and Asset tab tables extended. Check which MCP toolset carries them.
- **A6 The proof (M).** Idle, Walk, Run in a Blend1D synced on `Footstep`; Wave as an upper-body layer while moving; its commands file;
  feature test; `Perf.Animation.Graph1000` `Provisional` (proposed target: at most 3 times `Evaluate1000`).
- **A7 Docs (S-M).** DG, Architecture, a Manual page, the digest bullet.

**Cut line.** Cut first, in order: additive layers, speed scale, time-matched transitions, Asset tab extensions (keep `animation.info`).
Never cut sync, Blend1D, the masked layer, agent commands or the Third-Person proof.

Rules it keeps: poses are not entities; jobs write only their animator's record; scene changes on the main thread afterwards; root
motion only in `AnimationSystem::ApplyRootMotion`, and through the fixed step; Animation includes only Core, Asset, Reflection and
Scene. No ABI change (`SetFloat`, `SetBool`, `SetTrigger` suffice). Simple animators keep today's path, so `Evaluate1000` must not move;
moving version 1 controllers onto the graph path is a wave 9 choice once its costs are measured. It merges main with `physics-timing` in it:
rerun the root motion tests and the attachment test.

Defaults it changes: the Third-Person robot's look in motion, on purpose. Stored version 1 controllers behave as before.

### 4.6 `physics-timing`: smooth at any frame rate

Goal: a character reads smooth from 30 to 1000 fps with the simulation untouched. S0.9 gave the option the owner asked for; this is
the fix. Size L, about 3 agent-days, no change to physics. The owner's words allow "an option or something and/or make it tick by tick
following the frame refresh"; drawing between fixed poses gives the frame-by-frame motion without stepping by the frame's delta, which
is rejected on its numbers (section 9).

Steps:

1. **Capture and blend (M).** An `UpdateWorldTransforms()` before the fixed loop flushes per-frame writes; after the loop, if a step ran,
   it runs again in a capture mode that stores the old matrix beside the entity as it overwrites it. So only what changed during fixed
   steps enters the set: bodies, characters, scripts moving things in `OnFixedUpdate`, and what hangs under them, lights and cameras
   included. `Scene::GetDrawnTransform`, `BreakInterpolation`, suite `Scene.Timing`.
   - **Roots blend, descendants compose.** An entity whose own transform changed in the steps is a root: drawn = blend(captured, current
     world, a), with a = 1 - (1 - accumulator/h) / n, n the last stepping frame's steps. A descendant in the set never blends on its own:
     drawn = its parent's drawn matrix times its current local. The set is walked parents first. So a joint attachment, whose local
     `PlaceAttachments` writes every frame under a moving animator (`AnimationSystem.cpp:829-885`), stays on its joint on every frame, and
     neither lags nor runs a step ahead of the body (10.8 cm at 6.5 m/s).
   - **Per-frame writes.** A script's per-frame translation of a root cancels that root's interpolation: it asked to be there. A rotation-only
     write does not, since the blend already uses the current world (the player's per-frame turn). A write to a descendant cancels nothing.
2. **To the renderer (M).** `RenderScene::Sync` walks the set after the log through `UpdateProxyTransform`, whose `SameBytes` early-out
   makes a frozen alpha free; `RenderLights::Sync` and the camera read go through `GetDrawnTransform`. A GPU test with motion vectors on
   a non-step frame, and agreement with `GPU.SceneRenderer.GPUDriven`.
3. **Teleports (S).** `PhysicsWorld::Teleport`, `TeleportCharacter` and scripts break interpolation; a step longer than box3d's maximum
   speed times h counts as one. New entities are excluded. Paused or stepping, a = 1, so `atFixedStep` captures stay exact.
4. **The switch (S).** `Scene.Timing.Interpolation`, default true, read by the game runtime and editor Play; off in a bare `Scene`, as
   `SetTimeDilation` is, so tests and `Validate` stay exact. `stepAlpha` and the interpolated count in `play.state` and `physics.stats`.
5. **Script API (S).** Appended: `Time::GetStepAlpha()`, drawn world position and rotation, `ResetInterpolation()`, through the host
   function checklist. ThirdPerson's camera uses the drawn pose. The `FollowCamera` arm's one-frame jump (131-141 px, `FollowCamera.cpp:85-86`) is fixed there.
6. **Perf and docs (S).** `Perf.Scene` capture at 100k, `PerfGPU.Renderer` interpolated frame at 32k (`Provisional`), DG, Manual. S0.3's
   checks run again with interpolation on.

**Cut line.** World-space UI and gizmos on drawn poses; a cap on interpolated bodies and parallel placement, if a later measurement asks
(100k awake bodies cost about 11 ms a frame in the serial proxy path, paid per step today). Keep capture, render, lights, camera, teleports.

Regression tests for the owner's case: a character moved by a controller at 1 ms frames, with per-frame drawn increments uniform within
a tolerance; it fails without the fix. An attachment stays on its joint on non-step frames, run again after `animation-graph` lands.

Defaults it changes: games and Play draw a constant one step behind the wall clock (mean latency up about half a step, worst case
unchanged). Every frame pays what a step frame pays now, plus the blend; sleeping bodies pay nothing.

### 4.7 `local-lights`: many shadowed lights

Goal: as many shadowed point and spot lights as the atlas holds, and shadows that stay on while the camera moves. Size L, about 4
agent-days. Never split steps 1-3 and 6: the cap without tile lifetime is worse than today.

Steps:

1. **Limits out (S).** Growing light and view buffers, a dynamic `drawn`, first view in 16 bits and count in 8 (`R/SceneRendererGPUDriven.cpp:1448`,
   `Cull.comp:358`), stats as first entries by rank plus a size histogram, a record-to-slot table for the CPU path.
2. **Tile lifetime (M).** A resized light samples its old tiles until the new ones are drawn (`DrawnKey.Tile` holds them, freed in
   `CommitLocalShadows`). No global layout: on failure keep the size, or take a smaller tile for a new light. Shrinks (largest first), then
   new lights, then grows, at most two resizes a frame. `FitTileSizes` targets 7/8 of the atlas. Capacity counted before ranking, so atlas
   drops fade.
3. **Scheduler (S).** Views without content first, then by age and distance, so nothing starves; the `LocalShadowViewsPerFrame` row.
4. **Change tracking (M).** A grid of lights for `dirtyReached` and `inRange`, a hash lookup instead of `find_if`, an allocator bitset per
   level. The cached rotation-projection per view goes first if this runs long.
5. **Forward skip (S).** `coat.Strength <= 0 && dot(surface.Normal, toLight) <= 0` continues before the shadow, removing about half of what
   shadows add in the A/B. Check glass. Can land first.
6. **Fixed noise (S).** `LocalShadowSampleRotation` (`LocalShadows.glsl:39-44`) turns its noise with `TemporalParams.z` "so the history averages
   the noise away", which AGENTS.md forbids for the sun. This wave raises the count to 64 and 256, and shot 6 rests on these shadows, so
   the filter moves to fixed noise. Judged with no AA and FXAA in shot 6 and the lantern town.
7. **Presets and docs (S).** Rows, traits, graphics page, `FeatureTest.stproj`, DG, Manual. The setting's maximum is 4096; the real ceiling is
   the atlas (about 37, 149 and 597 point lights at 128-texel tiles in 2048, 4096 and 8192).
8. **Tests and perf (M).** A camera sequence over a full atlas where no light with content loses it (fails on 0.7.0); a resized light keeps
   its shadow; lights the atlas cannot hold fade; a capture draws more than 255 views; a far dirty view is drawn within a bound while nearer
   lights move every frame; more than 16 lights on both paths; allocator determinism. Metrics `PerfGPU.Renderer.LocalShadows64Lights*`, the
   256-light forms and a main-thread metric for 1000 moving proxies under 256 lights, measured with `physics-timing` merged.

**Cut line.** Time governor (a `GPU.LocalShadows` stage scaling views per frame), tap counts by rank, retained tiles for lights that left the
view. If step 4 slips, Ultra takes 64 as well.

Defaults it changes: High and Ultra shadow more lights at the same atlas sizes; High 24 and Ultra 48 views a frame; the local filter's noise
stops turning under TAA.

### 4.8 `editor-ux`: the rest of drag and drop

Goal: the ghost looks like what will land. Size L, of which about 1.5 agent-days above the cut, after S0.7. It leads tranche 3 because the
owner's item 10 said "should be easy to do" and a box ghost is not yet what the owner asked for.

1. **Mesh ghost (M).** `SceneRenderOptions::PreviewMeshes`, drawn in `RenderOverlays` after the tested debug lines, `R/SceneRendererPreview.cpp`,
   shaders `Overlay/PreviewMesh.*`: translucent theme color with a rim, depth-tested, no writes, unlit, after TAA, no entity ID, Scene view's
   renderer only (captures never show it). Parts from the template in a private scene, cached by content stamp; above about 512 parts or
   while meshes load, the box. Skinned meshes in bind pose. Inside `GPU.Overlays`. The quieter style of a miss carries over from S0.7.
2. **Live material preview (S).** `RenderScene::SetMaterialPreview(entity, material)`, one entry, applied in `UpdateProxyFromEntity`; no component
   written, so nothing reaches undo or saves. Cleared on cancel and on scene change, tested.

**Cut line.** Cut first to last: per-mesh BVH (M, above about 50,000 triangles, built by a Background job; without it a 2M mesh costs 12.3 ms per
ray during drags only, and `entity.dropToGround` stays slow on terrain); pause on spike; HDRI to the Sky Light (no live preview); hover and step
aids in the paused timeline; the precise frame wait if S0.4's measurement asks for it.

Tests: GPU pixels change and captures omit the ghost; hover changes nothing; the snapshot rules.

Defaults it changes: none.

### 4.9 `volumetric`: fog with shafts and cones

Goal: visible sun shafts and light cones in fog, stable without history. Size L, 4-5 agent-days, sun-only core 2-3. Wave 7's design
(`light-and-materials.md`, D9) with these changes: read shadows coarsely, from the finest cascade whose texel is near the froxel's width, or 4
taps across the froxel, which cuts shimmer 4-7 times in the simulation and keeps fog independent of the fit; write volumes pre-exposed (a
100,000 lux sun overflows half floats); light fog indoors from the probe's L0 at the froxel or the irradiance volume, not the open sky; drop
96x54x48, which fails on a 30 cm slit.

Steps:

1. Stage, row, slot 34 with a dummy, `VolumeParams`, `HeightFog` properties with `InactiveReason`s (S).
2. Sun-only inject, filter, integrate, applied in `Forward.frag` and `Skybox.frag` (M). Tests: matches analytic fog, off costs nothing, still
   frames identical, both paths.
3. `Include/VolumetricShadows.glsl` with the coarse rule and a fade at the shadow distance (S). Test: a shaft in a dark room, unchanged when the
   cascade fit changes.
4. Indoor light for fog (S).
5. Point and spot lights in fog, one tap each, at most about 8 per froxel (M): the owner's light cones.
6. `PerfGPU.Renderer.Volumetric4K`, docs, the showcase's adoption (S). Hut openings stay 10 cm or wider; 3 cm plank gaps do not hold at High.

It joins landing 2 only with steps 1-5 verified, cones included, because the owner's item is "Volumetric and light cones". If only the core is
verified, it waits for wave 9 with its branch kept, and if a release must take the core alone, 1.2 and the release notes say the cones moved.

**Cut line.**

7. `FogVolumeComponent` (M), a box of density for hut dust without fogging the valley (SRV 36). Without it shafts in the hut need a thin height
   layer at dawn only.
8. Density noise; Ultra's 4 samples.
9. Thin translucency for leaves and grass (M), if time remains: little gain in a dry valley, otherwise wave 9.

Costs are scaled from other passes, not measured: about 0.25 ms at High, at most 0.7, 32 MiB per renderer.

Defaults it changes: none; a Height Fog asks for it.

### 4.10 `showcase-engine`: layers on materials, stills

Goal: what the showcase wants from the engine and can land without. Size L. Not on the owner's list, so last in tranche 3.

1. **One blended layer and wetness (E2, L).** Two sets of maps (a layer's base color, roughness and normal) blended by a mask, G for wetness
   and B for puddles, up to four more maps per material. One surface function, so `Forward.frag` and `DepthPrepass.frag` agree (SSR reads the
   prepass's normal and roughness). Streaming learns the layer's tiling on both paths (`m_MaterialUVScales`, `Cull.comp:151`). `GetTextureMaps`
   users, `MaterialData` (144 bytes), the feature test, `GPU.SceneRenderer.GPUDriven` and `.Feedback`. The showcase's ground adopts it.

**Cut line.**

2. **Supersampled stills (E1, M).** `viewport.capture {supersample: 2}` in the capture renderer, filtered down with the output's dither, offline only.

Left out with reasons in section 9: the script screenshot function (E5) and coverage-preserving alpha (E6).

Defaults it changes: none; materials without a layer stay as they are.

## 5. Quality presets after the wave

Rows not listed stay as 0.7.0 (`R/RendererQuality.cpp:35-47`). A row of a stream that did not land stays as 0.7.0.

| Row (field of `Renderer.Quality`) | Low | Medium (0.6.0's costs) | High (default) | Ultra | Added or changed by |
| --- | --- | --- | --- | --- | --- |
| `ContactShadows` | 0 | 0 | 0 (was 12) | 0 (was 16) | S0.2 |
| `ReflectionProbeResolution` (new) | 128 | 128 | 256 | 256 | `reflections` |
| `MaxShadowedLocalLights` | 4 | 16 | 64 (was 16) | 256 (was 16) | `local-lights` |
| `LocalShadowViewsPerFrame` (new) | 12 | 12 | 24 | 48 | `local-lights` |
| `GlobalIllumination` (new) | Off | Off | High | Ultra (below the cut: High until then) | `radiance-cascades` |
| `Volumetric` (new, tranche 3) | Off | 128x72x64, 2 samples | 160x90x96, 2 | 240x135x128, 4 | `volumetric` |

Memory: atlases stay 2048, 4096, 4096, 8192. `ReflectionProbeResolution` is the one memory row that moves: about 67 MiB instead of 17 per
renderer that shows a probe, at High and Ultra, with its bytes in the traits table. Quality rows scale what a scene asks for: a scene that
does not ask for GI or fog pays nothing at any preset. Medium's row for fog is only paid by a scene with a Height Fog that asks.

## 6. Defaults that change

Stored values never change. These are named in the 0.8.0 release notes, those of streams that landed.

| What | 0.7.0 | Wave 8 | Where |
| --- | --- | --- | --- |
| Contact shadows at High and Ultra | on | off; Custom turns them on | S0.2 |
| Cascade near bound | moves at once above 1% of pixels | persistence in seconds, at once above 10% or for transparent surfaces | S0.3 |
| Compiled shaders | glslang | slangc; pictures differ by rounding only | S0.10 |
| First configure of a clone | offline after submodules | fetches Slang's archive once, or `STRATA_SLANG_ROOT` | S0.10 |
| Fixed rate | per scene | per scene, unless the project's `FixedRate` overrides it | S0.9 |
| Max fixed steps with a project rate | 8 | about 0.133 s of time | S0.9 |
| Editor frame rate | constants | preferences with the same defaults | S0.4 |
| What games and Play draw | last fixed pose | between the last two fixed poses | `physics-timing` |
| SSR in every scene that uses it | screen door, wide edge fade, AO on the hit | repaired at High and Ultra; Medium's edge fade improves | `reflections` |
| F0 of materials with IOR other than 1.5 | 0.04 | from the IOR | `reflections` |
| Probe slots at High and Ultra | 128, 17 MiB | 256, 67 MiB | `reflections` |
| Shadowed lights at High and Ultra | 16 | 64, 256 | `local-lights` |
| Local shadows' filter noise under TAA | turns every frame | fixed | `local-lights` |
| Template and samples | no GI | on only after the owner's live look; off otherwise | Section 7 |
| Third-Person robot's locomotion | cross-fade | synced Blend1D and a layer | `animation-graph` |

Motion blur stays off everywhere until the owner has seen wave 7's table.

## 7. Integration and release

Main is safe at every landing. The showcase lands on no stream's schedule.

1. **Each stream, when its steps above the cut are done:** an independent review with findings fixed or answered, Debug ctest with every GPU
   suite under the validation layer, Release ctest without perf, `CompatLint.py`.
2. **R1 lands alone** through `ci/wave8-reflections`, in slot 4, and the sample agent's editor is rebuilt from main.
3. **Landing 1, the showcase (end of tranche 1).** When `showcase` reaches step 8, an integration on `agent/wave8-mid` merges, in order, what
   is verified by then (expected: `reflections` R2, `physics-timing`), then `showcase`. If nothing is verified, the showcase lands alone: no
   stream's lateness moves this landing. Debug, Release and Dist ctest, CI green on `ci/wave8-mid`, then main. `build/windows` is rebuilt; the
   owner gets the first full sheet of the six shots. Slot 4 runs it, and the usage rule
   applies. Later streams merge main, and `radiance-cascades` hardens on the yard.
4. **Landing 2, the rest.** When tranche 2 is done, an integration in `.worktrees/wave8` merges the remaining streams in order, with whatever of
   tranche 3 is verified, wires what meets, then: Debug, Release and Dist verified; a review through lenses (fidelity to this plan and the
   specs, interactions between streams, image); fixes; verified again.
5. **Adopt everything but GI.** Each landed feature in the showcase, its table in 4.2. Pictures retaken.
6. **The first sheet and the owner's live look.** Before and after for 0.7.0, step 0 and the wave, the six shots, and wave 7's unshown sheets with
   its motion blur table. `build/windows` rebuilt, so that the owner can look at the editor and the samples, and at GI toggled in motion on Third-Person
   and the showcase.
7. **GI adoption.** After that look, GI goes on in the template and samples, per sample. If the owner has not looked by the time step 8 can start, it
   ships off in samples and template, with the property documented, or we turn it on where our own look in motion finds it holds, since the
   order is ours. The release does not wait for a reply.
8. **Measure, every slot empty, alone on the machine.** Interleaved A/Bs against `.worktrees/wave8-base` under `Tools/PerfLock.py`:
   `PerfGPU.Renderer.*`, `Perf.Renderer.*`, `Perf.Scene.*`, `Perf.Animation.*`, physics, GI's milliseconds, and GPU busy at the samples' scene
   cameras at High 3840x2160 and Medium 1920x1080. Budgets are not re-baselined on this memory; a regression is fixed before the release, or
   named in the open items with its cause. New metrics stay `Provisional` until the reference memory returns, with the wave 7 leftovers
   (`SceneOpenOver1M`, `ThumbnailsMaxFrame`, the wave's campaign).
9. **The combined cases no single stream measured:** interpolation at unlocked frame rates with 64 and 256 shadowed lights and 1000 moving
   bodies; `viewport.capture` and `play.step` with interpolation on; the showcase on Ultra with everything adopted.
10. **The final sheet**, with every sample's pictures retaken by the last change that shows in them.
11. **CI and main.** `ci/wave8` green on every job before `main` moves.
12. **The Mac gate.** The GPU suites through MoltenVK before the release, the second check after S0.10's. If it cannot run, the release notes
    and the open items say so (D3).
13. **Release 0.8.0.** `ctest --preset windows-dist -LE "gpu|perf"`, the `-rc1` dry run, Status "Wave 8 landed", release notes with their upgrade
    rows, and what fell below the cut lines or out of the tranches moved to "Later" or wave 9.

If the wave ends at landing 1, steps 8 to 13 run on that tip, for the rows of section 1.1 it holds.

## 8. Risks

| Risk | Mitigation |
| --- | --- |
| The wave outruns the owner's usage, as wave 7 did | Tranches, the usage rule, a reserve of 25% for a landing; main is releasable at landing 1; the wave may cross a reset |
| The estimate of two usage weeks is one data point | Read usage before each start; tranche 3 is out by default; `radiance-cascades`' hardening and then `local-lights` give way first |
| A silent slip from the Slang conversion surfaces weeks later as a "lighting bug" | Three independent gates, every `mul` and matrix index audited by listing, the rule in 4.1 that points debugging at the conversion, look-dev compare against 0.7.0 kept |
| Slang's SPIR-V meets drivers it has not met (AMD, Intel, Mesa, MoltenVK) | MoltenVK is a gate of S0.10 at least as a conversion; the Mac gate again at release; the old compiler is in history |
| Radiance cascades shimmer on real foliage, the audit's main risk | G1 before anything joins shading; the degrade path is the plan, not the exception; `GlobalIlluminationOcclusion` 0 for scenes with foliage; off until the owner's live look; the irradiance volume as the owner's stated fallback; the showcase's landing does not depend on it |
| The rule against history meets G1 | D7: no history unless the fence misses in every degrade form, and then only with the owner's live look |
| Pictures catch GI mid-convergence and A/Bs lie | `GlobalIlluminationSettled` in captures and thumbnails, K frames in the still test |
| Shot 4 on untested parameters | R1 lands before dressing; graybox's capture decides the fallback |
| The vista's shadows do not reach 600 m (shot 1), or a box probe does not darken the hut (shot 5) | Graybox's named checks before dressing; a reframe rule for each |
| Interpolation and local lights multiply: proxies move every frame instead of every step, so change tracking (26 ns per light per moved proxy) and shadow redraws are paid at the frame rate. Neither audit measured the two together | `local-lights` step 4 is above its cut and its perf step runs with `physics-timing` merged; section 7's combined case; the fallback row (Ultra 64); a cap on interpolated bodies below `physics-timing`'s cut |
| The sun's cascade cache and moving casters at high frame rates | S0.3 lands first and its checks repeat with interpolation on |
| Two streams meet in `Forward.frag` | S0.11's struct and functions, the region table in section 4, merge order, a merge of main before landing |
| A probe at 256 hitches a station change | Measured in `reflections`' step 10, spread over frames, or the row stays 128 |
| A generated bus looks like a box | Close shots show the panel, not the silhouette; containers as the fallback; graybox on the owner's sheet early |
| An agent's composition looks flat | The six shots grayboxed first, station geometry computed, not guessed |
| A fresh showcase import exceeds the package test's 1800 s | Timed at step 2 with the real set; `Quality: Fast` is available |
| S0.3's constants come from one sample | Checked on the showcase's traffic at graybox; `Roadmap` records the limit that a persistent near object still re-cuts once |
| Step 0 grows and holds the streams | Lanes land separately; only lane Slang holds rendering streams, and its box is hard; `showcase` and `physics-timing` do not wait for it |
| Nine streams against four slots: the wave's end drifts | Tranches, the slot tables, cut lines; what is unfinished moves to wave 9 with its branch kept |
| AGENTS.md's 40,000 characters | S0.1 frees 3,000; 250 characters a stream; `Docs.AgentGuide` checks |
| GPU cost on slower machines (a mid-range card about 6 times slower: cascades about 7 ms) | Medium keeps its costs and GI off; a player's option turns High's features down |
| Upstream Slang moves every 2-3 weeks with breaking changes | One pinned version, bumped deliberately |

## 9. Left out, and why

- **Stepping physics with the frame's delta** (item 11's "tick by tick"): box3d stayed stable, but it costs 230-336 ms per simulated second at 700
  fps against 22-31 at 60 Hz, a different frame-time sequence put a ball 14.6 cm elsewhere, and `play.step`, input durations, `atFixedStep` and the
  samples' regressions all count steps. Interpolation gives the same motion on screen. Substeps as a setting: not this wave (pits slept later).
- **Sun thrown off a sheet onto nearby surfaces:** the owner wants it eventually, "not a priority" (2026-11-01). Not built in wave 8. Cascades
  re-emit only what the camera sees, so a bright sheet lights its neighbors only while it is on screen, within the radiance cap. It goes to
  "Later" as its own item, planned after `radiance-cascades` lands, with its technique chosen then.
- **A fix for contact shadows** (item 1): a crossing test and noise that is not screen-locked, M to L with look-dev in every sample, for little
  visible gain now that the fit reaches millimeters near the camera. "Later".
- **Subsurface scattering** (item 7): pre-integrated skin (M-L) and any screen-space form (L; the forward pass has one target) go to wave 9 and
  later, with a character to show them. The showcase has no people. Thin translucency is the last item of `volumetric`, else wave 9.
- **The irradiance volume above the cut** (item 2): the owner made it conditional on cascades failing. For the hut, today's probe is enough at each
  station, to be proved at graybox.
- **Vendoring or wholly porting Esoterica** (item 9): no Linux or macOS layer, x86 SSE math (CI builds on Apple Silicon), a libclang generator in the
  build, a second asset, memory and type system, no tag to pin, a permanent fork, 4 to 5 streams. No graph editor, IK, warps, 2D blends or ragdoll
  nodes in wave 8: wave 9. Motion matching and retargeting would be new work; Esoterica has neither.
- **Pausing the viewport** (item 13 offered "either"): it stops the `Render.*` and most `GPU.*` stages, removing the costs being studied, and lets
  mips drop. The breakdown's pause does what the owner needs. S if still wanted.
- **The cap alone as an early win** (item 3): worse than today (section 2).
- **Medium's Checkerboard reflections without TAA:** a spatial trace at Medium would break Medium's 0.6.0 costs. Open item.
- **Gap-aware cascade slices:** the remedy for a near object that stays, with hysteresis of its own, not prototyped. "Later", if the walk or a
  game shows the case.
- **Virtual and planar alternatives:** planar reflections (L; puddles already hit in view, probes cover the rest). World-space cascades (voxelization
  XL, the paper calls storage a dealbreaker) and screen probes with world intervals (ray tracing MoltenVK lacks, 30-50 ms).
- **Run-time probe capture and fully free time of day:** three baked stations serve the showcase; keyframes are below `reflections`' cut.
- **Script screenshots (E5) and coverage-preserving alpha (E6):** nothing in the A-list needs them; masked grass vanishes by 30 m at 4K, so grass
  is out of the showcase. "Later".
- **Terrain with height-field colliders, sculpting and painting:** `world-environment` stays in "Later"; the showcase's terrain is generated.
- **Slang modules, generics, Metal or D3D targets:** plain HLSL syntax keeps the path open, and S0.10's `-target hlsl` test keeps it checked (HLSL
  output took 97 of 103 through DXC once the stage semantics were replaced); nothing for consoles can be verified without an SDK.
- **Abstractions for threads, paths, processes and static script modules:** with no SDK they would be guesses. An inventory in the docs.
- **Keeping glslang beside Slang:** two compilers would keep every rule twice.
- **Raising the showcase's 80 MB:** one constant if the sheet shows softness (D2).
- **Wave 7's other leftovers:** the reference-memory campaign waits for the memory; up to 8 cascades, distant shadows, 16-bit depth, sized point and
  spot lights stay in "Later", the last except for its highlight part, which is `reflections`' step 7. The Playground's empty C0 (nearest receiver
  18.9 m, first split 18.66 m) is recorded there.

## 10. Questions and decisions

The owner said "Make prioritization yourself". The one question that touched the owner's own rules is answered:

- **Q1, answered 2026-11-01.** "Yes you have permission to read and use esoterica engine and other repos if needed. The limitation I said only
  applies to other folders on this computer. You can/should use open source resources as much as you need." D10 says how.
- **Item 4's reading, answered the same day.** "I meant highlights and reflections, highlights are extremely important just like in a drawing,
  because they should be cheap. And the sunlight thrown off the sheet onto other things is also something I was thinking to do, but it is not a
  priority." Highlights lead `reflections`' R2 (step 7); the thrown light goes to "Later" (section 9).

Shown to the owner without holding anything: the live look at GI, with step 6 of section 7.

**Decided here:**

- **D1, how Slang is pinned.** A release archive fetched at configure time, verified by SHA-256, into a per-user cache, with a `STRATA_SLANG_ROOT`
  override, plus one sentence in AGENTS.md's third-party section, which today says all dependencies are pinned shallow submodules. A source build
  took Slang's own CI 29-47 minutes per job; a binary submodule needs a new repository. Cost: a clone no longer configures offline. Lane Slang verifies
  the Linux and macOS digests first: only the Windows archive was downloaded.
- **D2, showcase content.** Land inside the 80 MB class with the 72.2 MB A-list, mostly 1K scans, and raise the limit (one constant each for source
  and total) only if the owner's sheet shows softness. First-person walk, not the Third-Person robot, which is stylized and breaks the look.
- **D3, platforms.** MoltenVK is S0.10's gate: the GPU suites if the Mac can be reached, else CI's conversion to Metal as the minimum. A small
  time-boxed `GPU*` job on Mesa's software Vulkan is tried on `ci/slang` as a first second driver (untried, may not meet the engine's feature needs).
  If neither runs the suites, the release notes and open items say so, as 0.7.0's did.
- **D4, perf on the temporary memory.** Integration uses interleaved A/Bs against a kept 76f28a21 Release build, no campaign, no budget changes,
  all new metrics `Provisional`. The release does not wait for the reference memory.
- **D5, physics.** Interpolation on by default in games and Play, adding about half a step of mean latency, with `FixedRate` as the option. This departs
  from the owner's "tick by tick" and delivers its result, so step 0's report gives the numbers: 0.25 px against 29 px with the simulation unchanged,
  10 to 14 times the cost at 700 fps and lost repeatability for per-frame stepping.
- **D6, the order against "easy to do".** `local-lights` and `physics-timing`'s fix are streams, because the cap alone is worse than today and the option
  does not cure the jitter. Step 0's report says why.
- **D7, a temporal filter on the GI buffer.** Not asked in advance: G1 may never need it, and the owner's words rule out TAA upscaling and blur, not
  a filter on low-frequency light. The default stays no history. Only if the fence misses in every degrade form does one one-pole filter on the GI
  buffer alone get tried before the stream stops, and it then passes the same numeric gate and the owner's live look in motion. AGENTS.md's Lighting
  rule changes only if that form is adopted. If it fails, or the owner rejects it, the irradiance volume moves above the cut as section 4.4 says.
- **D8, reading usage.** This session cannot read the owner's weekly usage. The orchestrator asks for the figure before landing 1's integration,
  before tranche 2 and before landing 2, and otherwise goes by what the owner reports. The thresholds are the usage rule's, from wave 7 alone.
- **D9, the Mac.** No Mac is reachable from this session: its Mac sessions are offline. So S0.10 and section 7 run CI's conversion to Metal as the
  minimum, the Mesa job is tried, and the release notes say what did not run, naming that this release changes every shader. If a Mac session
  comes online before a gate, the GPU suites run there.
- **D10, open source (the owner, 2026-11-01).** Golden rule 1 now forbids only other folders on this computer. Open-source projects, papers and
  talks are read as much as helps, and their code may be used: copied, ported or adapted only from permissive licenses (MIT, BSD, Apache-2.0,
  zlib, Boost, public domain or CC0), with the notice kept in `ThirdPartyNotices.md` and the source named in the commit message. Copyleft
  code (GPL, LGPL, AGPL, MPL and the like) is read for ideas but never copied or ported, because Strata's license is the owner's choice and
  copying copyleft code would make it. Large pieces still come in as pinned submodules under `Strata/vendor/` when they fit the rules of
  "Working with third-party code".

## 11. Wave 9 (outline)

- **`animation-tools` (XL):** the node canvas (panels through `EditorPanelRegistry`, every edit a command), live debugging and a task view, a clip
  timeline for events and sync markers, a mask editor, recording.
- **`animation-runtime-2` (XL):** Blend2D with our own triangulation, selectors, referenced and external graphs, variations, typed clip events,
  additive clips, float curves, two-bone and foot IK (ground through `IPhysicsQueries`, post-physics in `OnLateUpdate`), warps, forced transitions,
  skeleton LOD, compressed clips if measured to help; whether version 1 controllers move onto the graph path.
- **Ragdoll nodes (L):** after a physics joints stream.
- **Skin and a character:** pre-integrated skin, thin translucency if not landed, and a distant figure in the showcase if a realistic CC0 or CC-BY
  body is found (Quaternius' library could supply motion, not a realistic body).
- **Whatever tranche 3 and the cut lines left,** tranche 2 if the wave ended at landing 1, and Slang as step 0 if its box ran out.

## 12. Review of the draft: what was rejected, and why

Two critics reviewed the draft. Their problems were checked against the code, and most held and are fixed above. These were rejected or changed:

- **"Roughly five times wave 7."** The tranches stand, the figure does not. It turns wave 7's 72 slot-hours into 6-12 agent-days at an assumed rate.
  By what was built, wave 7's streams ran 5.5-17.9k insertions each, and `animation-graph`'s 10 audit days are 10-15k lines, one such stream. So
  about two wave 7s, not five, which is why tranche 2 is intended and not dropped.
- **`animation-graph` only after landing 1, and ranked directly after `reflections`.** The two critics pull opposite ways. Neither is taken whole. It has
  the first claim on tranche 2 and a protected slot, and may start before landing 1 only on section 4's conditions. It does not go ahead of
  `physics-timing`, which the walk uses, or G1, whose answer decides a stream: the audit says this is the stream that can start late.
- **`editor-ux` directly behind `reflections` in slot 1.** Kept the swap with `showcase-engine`, rejected the place: it would put a tranche 3 stream ahead
  of tranche 2. It now leads tranche 3, and S0.7's box ghost lands early.
- **"Reserve landing 2's cost before starting any tranche 2 stream."** Applied in the week the landing runs, not before each stream, because a tranche may
  cross a reset. The rule's numbers (25%, 60%, 75%, 90%) are ours, from one data point.
- **"Keep any true owner question to the Esoterica rule change."** Rejected by the planner and then accepted by the orchestrator: a temporal filter is
  asked about only if G1 needs it (D7), so one question stands.
- **A rule for attachments that tells engine placement from a script's moves.** The problem holds: `PlaceAttachments` does write every frame, so the audit's
  rule would drop attachments out of interpolation, and its "hierarchy needs no special case" is wrong too. But no write says who made it. The rule goes
  by role instead: roots blend, descendants compose.
- **"Bring sized point and spot lights back from Later."** Only a highlight-only source radius, now above the cut as step 7 after the owner's word on highlights. Shot 6
  needs a check, not the shadow filter work.
- **Adding Checkerboard's march to `reflections` (the alternative offered).** Rejected for Medium's 0.6.0 costs; the goal is narrowed and the open items
  name it.
- **The audit's own G1 "with the owner looking live".** One critic asked to restore it as written. G1 stays numeric, so that the stream does not wait, and the owner's
  live look moves to the exit of hardening and to adoption, as the other critic proposed.
- **"Conversion, one per frame."** The code says 2 (`c_ReflectionProbeConversionsPerFrame`), so the problem is worse than stated, up to about 28 ms in a
  frame at 256. The change stands with the right number. Keeping stations' captures resident needs no new API: `Assets::RequestLoad` does it.
