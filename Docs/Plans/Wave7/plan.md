# Wave 7 plan: realistic, filmic images at native resolution, shadows first

Planned on 2026-10-26 on the Windows reference machine (i7-13700K, 64 GB DDR4-3600, RTX 5090, 3840x2160 at 150%),
from five audits of main at bc6ee123 (release 0.6.0): shadows, the image pipeline (clarity), lighting and materials,
quality settings, and architecture and cost. The harness refused the auditors' report files, so their structured results
are the reports; their captures, scripts and data are beside this file in `shadows/`, `clarity/`, `lighting/`,
`settings/` and `architecture/`. Paths below: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `DG` is
`Docs/DeveloperGuide.md`.

## 1. Goal

The owner (2026-10-26): "the current image quality is simply not great. The shadows are very pixelated, maybe add more
settings to improve that", no TAA upscaling ever ("The blurry mess that current gen engine has (like unreal) is
completely the opposite of what this engine is planning to be"), "great realistic graphics, which doesn't mean sharp or
clinical", with the Fox Engine (Metal Gear Solid V, 2014-2015) as the baseline look, and a graphical showcase after the
wave. What and when are ours to decide; only a good end result counts.

Our reading of that baseline, refined by the audits (an interpretation of a look, never of anyone's code): a strong
natural sun whose shadows are sharp at contact and soften with distance, never pixelated, noisy or floating; dense,
grounded ambient occlusion; sky light that is blocked where the sky is not seen; real-world light units with a real sun
to sky ratio (deep but not black shadows); measured-looking materials that keep their energy and do not sparkle; haze
that gives distance and scale; and the image of a camera: a filmic curve with a soft shoulder and natural hues, restrained
grading, eye-like exposure, soft bloom, motion blur, a lens's vignette, effects such as grain, aberration and flares only
with restraint. Clean at native resolution, neither smeared nor over-sharpened. TAA Sharp, measured at 0.91-0.92 of the
no-AA image's texture detail with no halos, is not what makes images clinical and stays the 3D default.

What the wave delivers, in the order the owner will notice it:

1. **Shadows of a real sun** (`sun-shadows`, with step 0's 4096-texel cascades): about one shadow texel per screen
   pixel where receivers are at 3840x2160, a penumbra sized by the sun (0.53 degrees), no noise without TAA, contact
   shadows that ground feet and props, and a debug view that shows all of it.
2. **The image of a camera** (`camera-and-tone`): a calibrated filmic curve that needs no +1 EV, white balance and
   grading, no banding, eye-like exposure, bloom of the same size at every resolution, a physical vignette and motion blur.
3. **Believable light** (`light-and-materials`): physically balanced sun and sky in new projects, energy-conserving
   rough metals, ambient occlusion that is dense and clean without TAA, specular that holds at 4K, and aerial perspective.
4. **Quality people, agents and players can choose** (step 0 and `graphics-options`): a `Renderer.Quality` project
   section with Low, Medium, High and Ultra presets that every renderer follows and changes at run time, a Graphics
   Quality page, and graphics options in shipped games.

Rules that hold for everything in the wave:

- Native resolution only: no render scale below 1, no dynamic resolution, no temporal upscaling, ever.
- Nothing needs temporal accumulation to look right. Every new screen-space effect ships its own spatial filter and is
  judged with TAA, with FXAA and without anti-aliasing. Noise that only TAA's history hides counts as a defect.
- Every feature costs nothing when off (no target, no pass, no bound resource; at most a uniform branch in shared shaders).
- The CPU and GPU-driven paths stay pixel-equal, and cached and uncached shadows stay exact
  (`GPU.SceneRenderer.GPUDriven`, `.Feedback`, the shadow cache's exact case).
- Medium keeps 0.6.0's costs (sample counts and resolutions), High is the default for programs, and
  `SceneRendererSpecification{}` stays Medium so tests keep their meaning.

### 1.1 Acceptance at the wave's end

Checked at integration on the reference machine with step 0's look-dev harness (section 3, S0.6), against its 0.6.0
baseline and the auditors' measurements:

| What | 0.6.0 (measured) | Wave 7 target |
| --- | --- | --- |
| Screen pixels per shadow texel over visible receivers, High, 3840x2160 (debug view median, scene cameras and the editor's opening views of the 3D samples) | 2.2 at splits, 5.5-8.7 at near ends; editor views all in the last cascade | median at most 1.5, no visible receiver above 4 |
| Single-frame RMS noise across a sun penumbra, no AA (the shadows audit's `noise.py` method, ThirdPerson robot) | 4.6% of the edge's contrast | at most 1% |
| Penumbra width of a 0.53 degree sun (GPU test, analytic caster) | about 3.8 times too wide with the default 1 degree | within 20% of 2 d tan(0.265 deg) |
| A 5 cm plate on the ground at 2.6 m (look-dev scene) | thin rim only | a solid contact shadow |
| Middle gray (0.18, metered) on the display, template at Exposure 0 | sRGB 91 (ACES), so +1 EV everywhere | sRGB 112-124 with Filmic at Exposure 0 |
| Widest run of one 8-bit code in PhysicsPlayground's sky column | 123 px | under 16 px (dithered) |
| White furnace, metal of roughness 1 | 0.31 of F0 L | at least 0.9 |
| Template's sun to sky illuminance on a horizontal surface | 1.6 | at least 5 |
| GTAO residue with FXAA or no AA (AO-only ratio, flat areas) | cross-hatch visible | no pattern; RMS at most 1% |
| GPU busy, High, 3840x2160, each 3D sample's scene camera and the Open World tour | 1.2-1.9 ms | at most 5 ms |
| GPU busy, Medium, 1920x1080, same views | 0.6-0.7 ms | at most 1.15 times step 0's |
| Renderer CPU (`Perf.Renderer.*`, `PerfGPU.Renderer.*` CPU metrics) | budgets | within budgets (they only go down) |

## 2. Where we start

The audits' numbers that shaped the plan (details and captures in their directories):

- **Shadow density.** One 2048-texel cascade per slice, fitted with rotation-stable bounding spheres, gives 2.2 screen
  pixels per texel at every split at 2160 rows (about 4400 / `ShadowMapSize`) and 5.5-8.7 at near ends; 72-78% of each
  square holds nothing of its slice, and PhysicsPlayground's first two cascades see nothing at all
  (`shadows/cascades_2048.txt`, `sweep.txt`). Nothing outside C++ can set `ShadowMapSize` (`R/SceneRenderer.h:142`;
  `GameRenderer.cpp:25-29`, `ViewportRenderer.cpp:79-88` never set it). The capture renderer uses half the views' local
  atlas (`ViewportRenderer.cpp:86-87`), so `viewport.capture` differs from what players see.
- **Shadow filter.** 16 Poisson taps rotated per pixel by interleaved gradient noise (`SH/Include/Shadows.glsl:23-31`)
  leave 4.6% RMS noise without AA and 4.3% with FXAA; a sparse 16-tap blocker search dots thin casters' shadows; the
  PCSS radius is the full penumbra width (twice the physical one) on a 1 degree default light (the sun is 0.53); a
  1-texel floor of rotated taps blurs contacts over about 3 texels (29 cm in ThirdPerson's last cascade). No contact
  shadows; shadows end at `ShadowDistance`. Cost is small here: `GPU.Shadows` 0.13-0.18 ms with every cascade redrawn.
- **Tone and exposure.** The ACES fit shows metered middle gray at sRGB 91, so the template and all three samples set
  `Exposure 1`; bright reds skew to orange; there is no white balance, grading or LUT; the 8-bit output is not dithered
  (tinted contour bands, `clarity/captures/banding_pp_sky_x8.png`); bloom's radius halves at 4K; exposure weights every
  pixel alike and adapts at one speed; the vignette is a ring applied after the curve; the camera has no motion blur or
  depth of field although motion vectors exist without TAA.
- **Light and materials.** Sky light is never occluded (a closed room is lit like the street,
  `lighting/cap/room_atmo_default.png`); the template's procedural sky with its 3 lux sun gives a 1.6 sun to sky ratio
  (`basic3d_scene.png`) where the physical atmosphere gives 6.9 (`test_overview_atmo.png`); rough metals lose up to 69%
  (no multiple-scattering compensation); GTAO is narrow (0.6 m, 2x6) and leaves a cross-hatch without TAA (0.35-0.87 ms
  at 4K); SSR exists only with TAA; no aerial perspective; the atmosphere's reflections go black after a sky-source
  switch until the sun moves (bug). The Open World's cyan trees are content (sRGB bytes written as linear factors by
  the exporter) plus a cyan-tinted sun.
- **Settings.** No person, project, agent or player can change rendering quality; project settings hold only input
  actions and collision layers; scripts have no graphics API; `SceneRenderer` cannot change its specification while it
  lives; six of the sun's properties have no description.
- **Cost and architecture.** The three 3D samples cost 1.2-1.9 ms of GPU time at 3840x2160 and 0.6-1.0 ms at 1920x1080
  here; a mid-range GPU is about 6 times slower (`architecture/measurements.md`). Every image feature meets in
  `R/SceneRenderer.cpp`'s `RenderFrame` (28 wave-6 commits), `SH/Scene/Forward.frag`, `FrameConstants`,
  `RendererComponents` and `Feature.stscene`. CI runs no GPU tests, and waves 4-6 never ran on NVIDIA. AGENTS.md has
  22 characters of room.

## 3. Step 0 (on main, before the streams branch)

Step 0 is already running in `.worktrees/step0`: the GPU and Release ctest on NVIDIA for the first time since wave 3,
with fixes for what fails, and a perf campaign that measures the budgets again after the CPU replacement. The audits
require these items in it as well, in this order, after the NVIDIA fixes and before the perf campaign (so the campaign
measures the tree the streams branch from):

- **S0.1 Room in AGENTS.md.** Move details into the guide (rule kept whole here, stated in full there) until at least
  2,500 characters are free: each of the four streams adds one digest bullet.
- **S0.2 The atmosphere's black environment** after a sky-source switch (lighting audit L6: Atmosphere, switch to
  Procedural and deactivate the sun, reactivate and switch back: a mirror sphere reads (0, 0, 0)). Find the cause in
  `R/SceneRendererAtmosphere.cpp:264-290` (first-environment path, spare maps, the sky-view table's validity), make a
  source switch, a sun activation and an intensity change from 0 start a full refresh, and add a GPU regression test of
  the sequence. A bug in shipped code, and the file `light-and-materials` extends.
- **S0.3 Seams that make the streams additive** (no pixel changes; the GPU suites prove it): post-processing from the
  TAA resolve to FXAA (`R/SceneRenderer.cpp:3201-3296`: exposure, bloom, tone mapping, FXAA) moves into `RenderPost` in
  `R/SceneRendererPost.cpp`, and ambient occlusion (`RenderAmbientOcclusion`, its pipelines and targets) into
  `R/SceneRendererAmbientOcclusion.cpp`, as wave 6 did for glass, SSR, the atmosphere and probes.
- **S0.4 The quality foundation: `RendererQualitySettings` and the `Renderer.Quality` section.**
  - `R/RendererQuality.h/.cpp`: `RendererQualityPreset { Low = 0, Medium = 1, High = 2, Ultra = 3, Custom = 4 }` (file
    and script values, never renumbered) and `RendererQualitySettings` with today's costly knobs:
    `ShadowResolution` (Auto, 1024, 2048, 4096, 8192), `ShadowCascades` (1-4), `LocalShadowAtlasSize`,
    `LocalShadowMaxTile` (new; replaces the constant 1024 of `R/SceneRendererLocalShadows.cpp:36-37`),
    `MaxShadowedLocalLights`, `LODErrorPixels`, `MinProjectedPixels`. The preset table is laid out one knob per line
    with its four preset values on that line, so the streams' rows merge as separate lines.
  - Presets (section 5 has the table after the wave): Medium is exactly 0.6.0; High resolves `ShadowResolution` Auto to
    2048 below 1440 output rows and 4096 from 1440 rows (1.1 pixels per texel at every split at 2160 rows), so at
    1920x1080 High equals Medium; Ultra 4096 below and 8192 from 1440 rows, an 8192 local atlas and 2048-texel tiles.
  - `SceneRendererSpecification::Quality` holds them (the four existing fields move in, about 80 test sites renamed in
    this one step, before anyone branches); its default is Medium, so tests keep their pixels.
  - `SceneRenderer::SetQuality` changes them while the renderer lives: only what changed is rebuilt (the cascade array
    with `ShadowCascades` layers instead of always 4, its framebuffers, pyramids and cache; the local atlas and its
    cache) through `Renderer::ReleaseDeferred`. Renderers of single images release the cascades with their targets
    (today every renderer keeps its four layers for its whole life, 64 MiB for the capture renderer).
  - A simple settings section `Renderer.Quality` (title "Graphics Quality", category Rendering, version 1; `Preset`
    default High, one field per knob used under Custom, each described for agents) registered in
    `R/RendererRegistration.cpp`, shown by wave 6's generic page until `graphics-options` gives it its own.
  - Every program follows it: `GameRenderer` (by the section's version, each frame) and the editor's Scene view, Game
    view and capture renderer (`ViewportRenderer`); a viewport setting `quality` ("project" or a preset;
    `viewport.setSettings {quality}`), `viewport.capture {quality}` defaulting to the quality of the view it captures
    (the Game view's for `camera: "scene"`); thumbnails and the probe bake use named profiles
    (`SceneRendererSpecification::ForThumbnails()`, today's 1024 x 1; `ForBake()`, fixed, never the person's);
    `StrataRuntime --quality <preset>` for the session without writing anything; `renderStats` and `editor.perf` report
    the effective quality and each shadowed local light's tile size.
  - Tests: a GPU test that a capture equals the Game view for a scene with a shadowed point light near the camera (fails
    today), `SetQuality` round trips on both draw paths with the shadow cache's exact case, the section's validation and
    upgrade, `--quality`, the feature test project's non-default `Renderer.Quality`; DG "Rendering > Quality settings",
    `Docs/Manual/Settings.md`, one AGENTS.md rule ("costly knobs are rows of `RendererQualitySettings`; Medium keeps
    0.6.0's costs").
  - The default changes: every project at 1440 rows and above draws 4096-texel cascades (256 MiB per view renderer
    with a shadowed sun, from 64). Release notes name it.
- **S0.5 Scene binding slots and stage names assigned** in a comment of `SH/Include/Scene.glsl` and in this plan, so
  parallel streams never take the same slot: `sun-shadows` SRV 28, `light-and-materials` SRV 29-31; `camera-and-tone`
  needs none (post passes have their own layouts). (Today the scene layout uses SRV 0-21 and 27; 22-26 belong to the
  TAA's layout.)
- **S0.6 The look-dev harness** `Tools/LookDev.py` (standard library only), the tool every stream proves its change with
  and the owner's before/after sheet at the end:
  - Copies the three 3D samples and the `basic3d` template into a temporary directory, builds a look-dev project there
    from a committed command script (`Tools/LookDev/LookDevScene.json`: a closed room with one window, a white-furnace
    row of spheres over roughness and metalness, thin casters such as poles, a fence and a 5 cm plate, an alpha-masked
    foliage card with a texture the script generates, a sky gradient), and launches headless Release editors on them.
  - Captures the shots of `Tools/LookDev/Shots.json` (each sample's scene camera and editor opening view, the auditors'
    close-ups: ThirdPerson's feet and far coins, PhysicsPlayground's dominoes and sky, the Open World's forest, low sun
    and water) at 3840x2160 and 1920x1080, with TAA, FXAA and no AA, still and live (`viewport.capture {live}`), each
    with `renderStats.gpuMs` medians of 5, into a dated folder outside the repository, with crops at fixed regions and a
    table of stage times.
  - A CTest (label `gpu`) runs one shot at 320x180 so the tool keeps working.
  - The baseline is taken twice: with 0.6.0's build (`build/windows`, bc6ee123) and at step 0's tip.
- **S0.7 The roadmap text** of this planning (`roadmap_wave7.md`): the Wave 7 entry and the "Later" edits, including
  the owner's ruling against rendering below native resolution.
- **S0.8 The perf campaign** (orchestrator's change, 2026-10-26): it runs first, on 0.6.0's Release build
  (`build/windows`, bc6ee123), while the specs are written (no builds then), so it measures the code the budgets were set
  for on the replaced CPU: `.worktrees/wave7-plan/perf-campaign-bc6ee123/`. Step 0 applies its results to
  `Budgets.json` (re-baselined entries stated with their measured values, the Mac's provisional metrics measured here),
  and proves S0.3 and S0.4 cost nothing at Medium with interleaved A/Bs of the renderer metrics (`Perf.Renderer.*`,
  `PerfGPU.Renderer.*`) against bc6ee123 under `Tools/PerfLock.py`, when no other build runs.

Main gets step 0 after Debug and Release ctest pass on NVIDIA and CI is green on `ci/step0`; the streams branch from
its tip.

## 4. The streams

| Key | Title | Merges | Owns (exclusive) | Shares (additive edits, resolved at merge) |
| --- | --- | --- | --- | --- |
| `graphics-options` | Graphics options for people, agents and players | 1st | `Core/EngineServices.h` (IGameGraphics), `Scripting/ScriptHostAPI.cpp`, `ScriptABI.h`, `StrataScript/Graphics.h` (new), `Runtime/GameRuntime.*`, `Runtime/GameGraphicsService.*` (new), `StrataEditor/src/UI/GraphicsQualityPage.*` (new), `Panels/ViewportToolbar.cpp`, `Panels/InspectorPanel.cpp` (its property sections; light adds two buttons), `Reflection/PropertyBuilder.*` (sections), ThirdPerson's settings menu, `Docs/Manual/Graphics.md` (new) | `RendererComponents.cpp` (descriptions), `Feature*` scripts, `EditorLayer.cpp` |
| `sun-shadows` | Shadows of a real sun | 2nd | `SH/Include/Shadows.glsl`, `LocalShadows.glsl`, `SH/Scene/CascadeFit.comp`, `SH/Effects/ScreenShadows*.frag` (new), `SH/Overlay/ShadowView.frag` (new), `R/SceneRendererExtraction.cpp` (cascade layout), `R/SceneRendererGPUDriven.cpp`, `R/SceneRendererLocalShadows.cpp`, `R/SceneRendererScreenShadows.cpp` (new), the light components | `Forward.frag` (the sun term only), `SceneRenderer.cpp/.h`, `SceneRenderData.h`, `Scene.glsl` (SRV 28) |
| `camera-and-tone` | The image of a camera | 3rd | `SH/PostProcess/*` (Tonemap, FXAA, Bloom, Exposure, Histogram, new MotionBlur), `R/SceneRendererPost.cpp`, `PostProcessComponent`'s exposure, tone, color, bloom, vignette and motion fields, `Docs/Manual/Camera.md` (new), `AntiAliasing.md` | `SceneRenderer.cpp/.h` (targets, motion vectors), `ProjectTemplates.cpp` (Post Process lines) |
| `light-and-materials` | Believable light | 4th | `SH/Include/PBR.glsl`, `MaterialTextures.glsl`, `Forward.frag`'s indirect block, `SH/IBL/*`, `SH/Effects/GTAO.comp`, `AOBlur.comp`, `ScreenSpaceReflections.comp`, `Atmosphere*.glsl`, `Fog.glsl`, `R/SceneRendererAmbientOcclusion.cpp`, `R/SceneRendererAtmosphere.cpp`, `R/SceneRendererScreenReflections.cpp`, `SkyLightComponent`, `HeightFogComponent`, the Post Process AO fields, `Docs/Manual/Lighting.md` (but its Shadows section) | `SceneRenderer.cpp/.h`, `SceneRenderData.h`, `Scene.glsl` (SRV 29-31), `ProjectTemplates.cpp` (sky and sun lines), `GltfImporter.cpp` (one setting), `InspectorPanel.cpp` (the sky presets' buttons) |

All four branch from step 0's tip and run at the same time in `.worktrees/<key>` with build directories
`build/agent-<key>`. None depends on another above its cut line. Every stream shares, additively: `RendererQuality.h`
rows, `R/RendererComponents.h/.cpp`, `R/GPUProfiler.h/.cpp` (stages appended), the debug-view enum and
`EditorViewportCommands.cpp`, `Feature.stscene`, `Budgets.json`, `Strata/CMakeLists.txt`'s shader list, DG, AGENTS.md.
The merge order puts the smallest and least conflicting stream first, then shadows (the owner's first priority), then
the camera (its calibration decides the samples' exposure), and light last (it tunes the samples' sky and sun under
the final curve and retakes their pictures). A stream ready before one ahead of it may land first, as in wave 4; the
later one then merges main.

### 4.1 Rules every stream follows

- **Testing.** Debug ctest and every `GPU*` suite under the validation layer on NVIDIA before each commit; new GPU suites
  use `Tests::GPUContext` and end with `CHECK(gpu.GetNewErrorCount() == 0)`; exact-pixel tests turn the new defaults
  off (`AddNeutralPostProcess`, `DitherOutput = false`). A regression test fails without its fix.
- **Perf.** No perf campaign and no `PerfStability.py` in a stream (the one campaign is at integration). A stream may
  A/B its own stages with the look-dev harness, under `Tools/PerfLock.py`, as information. New metrics enter
  `Budgets.json` as `Provisional` with their target; the integration campaign measures them.
- **Quality rows.** A costly knob is a row of `RendererQualitySettings` and a field of `Renderer.Quality` (one line per
  row, all four presets on it); Medium keeps 0.6.0's costs. Each landing bumps the section's version by one (the
  integrator renumbers when two streams chose the same), so values saved between landings upgrade.
- **Both paths and the caches.** New inputs of how casters are drawn change a version the shadow caches compare;
  `GPU.SceneRenderer.GPUDriven`, `.Feedback` and the cached-against-uncached case pass; shading stays independent of
  draw order (derivatives only of per-primitive interpolants and textures, proven by those tests).
- **Off costs nothing.** A disabled feature allocates no target, records no pass and binds a dummy where a layout needs
  one; Forward.frag's new work sits behind uniform branches on frame constants. New GPU work is wrapped in a
  `GPUPassTimer` stage registered with the renderer module.
- **Frame constants** grow only for values Forward.frag reads, appended at the end (the later lander re-appends after
  the earlier); new passes take parameters by push constants or buffers of their own.
- **Agents reach everything through commands:** properties through `component.set`, presets through
  `project.setSettings`, debug views through `viewport.setSettings`/`viewport.capture {debugView}` with statistics in
  the result, and any action through a described command with a complete schema. The editor-automation skill lists new
  debug views.
- **Feature test:** every new property gets a non-default value in `Feature.stscene` (or the feature project's
  `Renderer.Quality`), every new host function a call in the feature scripts.
- **Documentation:** a DG section per feature (Rendering), the Manual page for people, one AGENTS.md digest bullet per
  stream (step 0 freed the room), release-note lines for every default it changes.
- **Samples:** changed through editor commands on the samples (never by hand-editing scene files), their regression
  command scripts updated (`Editor/<Sample>Commands.json.in`, the package tests), and after merging main before landing,
  each stream that changed a sample's look retakes its `Picture.png` and `Screenshots/` at the end (binary conflicts
  resolve by taking main's and retaking).
- **Scope.** Steps run in order; each lands working, tested and documented before the next starts. Steps below the
  cut line are done only when everything above is; what is not done moves to "Later" when the wave lands. No code from
  outside the repository: techniques are named (PCSS, SDSM, GTAO, contact shadows, Vogel disks, AgX's principle) and
  designed here, curves and tables fitted by committed `Tools/` scripts.

### 4.2 `graphics-options`: graphics options for people, agents and players

Goal: quality is something a person finds where they look for it, an agent sets with one command and a player changes in
the game's options menu, saved for the next run. Step 0 made it work; this stream makes it a product.

Steps:

1. **The Graphics Quality page** (`UI/GraphicsQualityPage`, registered in `EditorLayer`): a preset picker and one table,
   a row per knob and a column per preset, the chosen column highlighted, Custom's column editable, a cost hint per row
   (memory and GPU at the project's resolutions), each edit one `project.setSettings` step. A quality chip in the Scene
   view's toolbar for its `quality` setting (Project, Low ... Ultra). The page and the table read the rows, so the
   other streams' rows appear without UI work.
2. **`IGameGraphics`**, an engine service in `Core/EngineServices.h` beside `IGameWindow` (Scripting may not include
   Renderer): the options are the `Renderer.Quality` fields, by name; it gives the effective value and its source
   (Project, Player, CommandLine), sets a value validated by the section, resets and saves. `GameRuntime` provides it
   over `GameRenderer::SetQuality`, the editor's play mode for the Game view. The player's choices live in an
   engine-owned `GraphicsOptions.json` in the game's save directory (through `IGameSaves`: a format header, the section's
   version, 64 KiB at most, upgraded by the section's `Read`, one warning and the project's values when it is bad),
   loaded before the first frame; in the editor `.strata/Saves`. `--quality` wins for the session without writing.
3. **Script API:** host functions appended after `InputCancelBindingCapture` (`GraphicsGetOptionCount`,
   `GraphicsDescribeOption` with an info struct carrying `StructSize`, `GraphicsGetOption`, `GraphicsSetOption`,
   `GraphicsGetOptionChoice`, `GraphicsResetOptions`, `GraphicsSaveOptions`; the ABI keeps its version), the SDK's
   `StrataScript/Graphics.h` (`QualityPreset`, `Graphics::Get`, `Set`, `Options`, `Reset`, `Save`), the API test module,
   a `Scripting.*` test and the feature scripts. Named options keep later rows reaching scripts without ABI changes.
4. **Commands:** `graphics.resetPlayerOptions` (Session, `settings` toolset) and `editor.status` section
   `game.graphics` (effective values and their sources while playing).
5. **The Third-Person Adventure's Graphics row** (Low, Medium, High, Ultra) in its settings menu, made through editor
   commands, applied with `Graphics::Set` and saved with `Graphics::Save`; its command script covers it.
6. **Discoverability:** property sections in the Reflection layer (a heading per property group in the Inspector,
   `component.list` and schemas); Post Process grouped (Exposure, Tone and color, Bloom, Ambient occlusion,
   Anti-aliasing, Reflections, Motion, Lens); descriptions with units for the six undocumented Directional Light and
   seven Post Process properties; the sun's Inspector line "Resolution and cascades: Graphics Quality (High, 4 x 4096)"
   linking the page, as collision layers link today.
7. **Documentation:** `Docs/Manual/Graphics.md` (presets and their costs, players' options, why there is no render
   scale), `Settings.md`, `Scripting.md`, DG (Gameplay API, Project settings), the skills (scripting, make-a-game,
   editor-automation).

**Cut line.**

8. **Supersampled stills:** `viewport.capture {supersample: 2}` renders at twice the size in the capture renderer and
   filters down (an inverse-luminance-weighted separable filter), offline only and above native, for showcase stills.
9. **A showcase budget class** for wave 8: `Samples.json` `"Budget": "Showcase"` read by `Samples.Content`: 80 MB of
   source assets for that sample and about 140 MB of binaries over all samples; every other sample keeps 25 MB.

Defaults it changes: none in images. The Third-Person Adventure's menu gains a row.

### 4.3 `sun-shadows`: shadows of a real sun

Goal: at 3840x2160 the sun's shadows are as fine as the screen where receivers are, sharp at contact and softening by
the sun's real angular size, identical frame to frame without TAA, grounded below the shadow map's resolution by contact
shadows, and visible as data in a debug view. Step 0's 4096-texel cascades halve the texels; this stream makes the
filter and the fit deserve them.

Steps:

1. **The shadows debug view** (`debugView: "shadows"` in `viewport.setSettings` and `viewport.capture`): an overlay
   pass (`SH/Overlay/ShadowView.frag`, like `MotionView.frag`) reading depth and the cascade constants, a hue per
   cascade and a shade by screen pixels per shadow texel (green at most 1, yellow 2, red 4 and more); the capture's
   result reports per cascade its slice, texel size, share of screen pixels and median pixels per texel (a small
   reduction, as the lod and mip views report theirs). Every later step is tuned and verified with it.
2. **A physical, deterministic filter** in `Shadows.glsl` (Forward.frag still calls it; step 3 moves the call):
   - PCSS's radius is half the penumbra's width (`separation * tan(halfAngle)`, today the full width);
     `DirectionalLightComponent::ShadowSoftness` defaults to 0.53 degrees (today 1.0).
   - The least filter is a fixed tent of bilinear comparison taps (about 1.5 texels), unrotated: no dither and no
     stair-steps where the penumbra is narrower than a texel, instead of the 1-texel floor of rotated noise.
   - The blocker search reads a conservative depth range pyramid of each cascade layer (two levels, nearest and
     farthest depth per 4x4 and 16x16 texels, built after each layer is drawn on both paths and kept with the cascade
     cache): it classifies the search region as lit, umbra or penumbra, never misses a thin caster, and lit and umbra
     pixels leave early.
   - Wide penumbrae take 16, 24 or 32 taps (the `ShadowFilterQuality` row) on a golden-angle (Vogel) disk turned by a
     64x64 blue-noise tile generated by a committed `Tools/GenerateBlueNoise.py`. Nothing turns with the frame any more
     (`TemporalParams.z` leaves the shadow code), so a still image is identical frame to frame.
   - The receiver plane depth bias, normal offset and cascade blending stay; biases are retuned at 4096.
3. **Screen-space sun shadows** (`R/SceneRendererScreenShadows.cpp`, `SH/Effects/ScreenShadows.frag` and its blur): a
   fullscreen fragment pass after the depth prepass (after the GPU path's second occlusion phase) evaluates the sun's
   cascades per pixel from the prepass's D32 depth (camera-relative position through the frame's inverse projection) and
   its geometric normals, then blurs the result with an edge-aware separable filter (depth and normal weights) whose
   radius follows each pixel's penumbra (none in lit, umbra and contact-hard areas), into an R8 mask (SRV 28). The
   opaque variant of Forward.frag reads one texel instead of `SampleDirectionalShadow`; transparent and glass surfaces
   keep the per-pixel call with the same functions. It works in `HDROutput` frames (probe bakes), with one cascade
   (thumbnails), with and without TAA, and records nothing without a shadowed sun. GPU stage `GPU.ScreenShadows`.
   Fragment form because comparison sampling in compute through MoltenVK is unverified.
4. **Contact shadows** inside that pass: a march toward the sun against the prepass depth over
   `DirectionalLightComponent::ContactShadowLength` (meters, default 0.3, also capped in pixels), 12 steps at High and
   16 at Ultra (`ContactShadows` row: Off at Low and Medium), with a thickness that grows with distance, fades with
   distance and at the screen's edges, and a fixed per-pixel offset (no temporal noise; the mask's blur cleans it).
5. **Adoption:** the rows `ShadowFilterQuality` and `ContactShadows`; the samples' explicit `ShadowSoftness` 1.0 becomes
   0.53 and their scenes' contact lengths are checked on their close-ups; their command scripts and pictures follow; DG
   "Shadows", `Docs/Manual/Lighting.md`'s Shadows section (units, softness, contact, quality), the AGENTS.md bullet.
6. **Cascades where the receivers are** (SDSM, depth range): the prepass depth is reduced to the nearest and farthest
   receivers (the GPU path from its depth pyramid's top levels, the CPU path by a small reduction pass), read back two or
   three frames late like the visibility feedback, quantized (quarter-stop steps with hysteresis and a margin) and the
   slices laid over that range up to `ShadowDistance`, split by `DirectionalLightComponent::CascadeDistribution`
   (0-1, default 0.8: today's hard-coded log/uniform blend, `R/SceneRendererExtraction.cpp:456`). First frames, camera
   cuts and views without receivers keep today's layout. The CPU computes the boxes from the read values on both paths,
   so culling, the cache keys and `CascadeFit.comp` agree; a still camera keeps its boxes, so the cache keeps working.
   A spec flag `FitShadowsToView` (on) for comparisons; not a person's knob (it costs about nothing).

**Cut line.**

7. **Local lights on par with the sun:** `SourceRadius` on point and spot lights (meters, default 0.05) drives PCSS with
   step 2's filter (`ShadowFilterRadius` becomes the floor), an energy-normalized widening of their specular lobe (a
   function in `PBR.glsl`, agreed with `light-and-materials`), and tiles sized by the texel density receivers need.
8. **Up to 8 cascades** (6 at High, 8 at Ultra): `RenderData::c_MaxCascades` 8 on both paths (two vec4 of splits,
   `CascadeFitConstants`, `CullConstants`, three sort-key bits, `CascadeFit.comp`'s workgroup).
9. **Distant shadows for open worlds:** a far cascade beyond `ShadowDistance` (`FarShadowDistance`, 0 = off) from static
   casters, cached until the sun moves a step or cells change, blended into the last cascade.
10. **16-bit depth** for far cascades and Ultra, the caster extension toward the light capped (Ultra's 1 GiB halves).
11. **Cascades kept while the camera moves:** per-cascade cache keys on the snapped square, depth range and casters'
    LOD criteria instead of the whole camera, so the far cascades stay cached in play.

Tests: penumbra width against the analytic value (a 0.53 degree sun, a caster at known heights); two frames with
different TAA phases are equal; a thin caster's shadow is found (the sparse search's failure case); the mask equals the
per-pixel evaluation within a tolerance outside blur bands and on both paths; contact shadow under the 5 cm plate; the
fit covers the visible range and falls back on cuts; the debug view's statistics; acne and peter-panning at grazing suns
(`GPUSceneShadowTests` :271 kept); cached against uncached exact. Perf (provisional): `PerfGPU.Renderer.ScreenShadows4K`
and High's shadow redraw at 4096.

Defaults it changes: ShadowSoftness 0.53 for new lights; every scene's penumbrae (half-width PCSS, deterministic filter,
fitted cascades); contact shadows on at High and Ultra.

### 4.4 `camera-and-tone`: the image of a camera

Goal: what the renderer shows reads like a photograph from a good camera: exposure that lands middle gray where a
camera would without +1 EV, a filmic curve that rolls highlights to white without hue skews, white balance and grading
tools, no banding, eyes' adaptation, bloom and vignette like a lens's, and motion blur, all restrained by default.

Steps:

1. **Dither the 8-bit output:** triangular noise of about one code per channel in the encoded domain, in the last pass
   that writes 8 bits (tone mapping without FXAA, FXAA with it); `SceneRendererSpecification::DitherOutput` (on) is
   turned off by exact-pixel tests' helper. FXAA's subpixel blend from 0.75 to 0.375 (it costs 19-22% of texture detail).
2. **Filmic**, the new tone curve, designed here on AgX's principle: log encoding on an inset gamut, a per-channel
   sigmoid, outset back, so bright saturated colors go to white without skewing; a look of Contrast, Toe, Shoulder and
   Saturation; calibrated so a metered 0.18 shows at display middle gray (sRGB about 118) with `Exposure` 0. Its
   matrices and curve come from a committed fitting script against stated targets. `TonemapOperator::Filmic` is the
   default of new Post Process components and the template; ACES, AgX, Khronos Neutral and Reinhard stay unchanged, so
   scenes that stored them look the same. The samples move to Filmic with their `Exposure 1` removed.
3. **Color:** white balance (temperature 2,000-15,000 K, tint; a von Kries scale in LMS on scene-linear light) and
   slope, offset and power per channel before the curve, saturation after it.
4. **Exposure like an eye:** adaptation toward brighter and toward darker at separate speeds (brighter faster), metering
   Average or CenterWeighted (`Histogram.comp`), an exposure key (compensation over the metered EV, so interiors read
   darker than sunlight; today an interior and a sunny exterior display at the same mean, 0.121 and 0.112), and the
   metering percentiles as properties.
5. **Bloom and vignette like a lens's:** the bloom chain's levels chosen by screen fraction (7 at 4K, so the halo spans
   the same share of the image at every resolution) with a `BloomRadius`; the vignette a field-angle fall-off (cos^4
   like, aspect-correct) on scene-linear light before the curve, default 0.12.
6. **Motion blur** of the camera and of moving objects from the existing velocity buffer: tile maximum and neighbor
   maximum, then a depth-aware gather of 8 to 16 samples scaled by `MotionBlurShutterAngle` (degrees, default 180) and
   clamped in pixels; motion vectors requested whenever it is on; never in the Scene view in edit mode; `MotionBlur` on
   Post Process (component default off, so stored scenes do not change; on in the template and the 3D samples), the
   `MotionBlurQuality` row (Off at Low), and the players' rows `AntiAliasing` (Scene, FXAA, TAA) and `MotionBlur`
   (Scene, Off: players may turn off, never on, what a scene uses). GPU stage `GPU.MotionBlur`.
   Restraint (orchestrator's note): the owner rejects blur ("the blurry mess"), so motion blur must read as a camera's,
   never as smear: the blur length is clamped in pixels (a small share of the screen), a slow pan or a turning camera at
   gameplay speeds stays clean at 3840x2160 (judged on live captures), and it is judged on the before/after sheet; if it
   does not hold up there, the template and samples ship it off.
7. **Adoption:** the template and the 3D samples get Filmic at Exposure 0 tuned on their scene cameras, motion blur at
   180 degrees and the new vignette; `AddNeutralPostProcess` turns the new effects off; new manual page
   `Docs/Manual/Camera.md` (exposure, tone, color, bloom, vignette, motion blur; what each costs), `AntiAliasing.md`
   (which method suits which look; FXAA's change), DG "Post-processing", the AGENTS.md bullet.

**Cut line.**

8. **Depth of field** with a physical lens on `CameraComponent` (focal length or field of view, aperture, focus distance
   or a focus entity): circle of confusion from depth, a half-resolution scatter-as-gather bokeh with near and far
   fields, after the TAA resolve and before exposure; off by default; kept out of the shadow cache's camera key; the
   players' `DepthOfField` row (Scene, Off).
9. **Film grain and chromatic aberration** with restraint (grain luminance-weighted and sharing the dither's noise;
   radial aberration of at most half a pixel at a 4K corner); both off by default, players' rows (Scene, Off).
10. **Morphological anti-aliasing** designed here (edge detection on luma and depth, pattern-aware blend weights from an
    area table a `Tools/` script generates, neighborhood blending) as `AntiAliasingMode::SMAA`, the non-temporal method.
11. **A 3D LUT asset** (`.cube` import, 33^3) with an intensity, after the curve (a new asset type: its importer,
    references, the feature project's asset).
12. **Lens flares** from the bloom chain (ghosts and halo) and a sun starburst, off by default.

Tests: dither keeps the mean and is off by the flag; Filmic's middle gray, white point and hue preservation of a
saturated ramp; white balance neutral at 6,500 K and tint 0; bloom's radius as a fraction of the width equal at 1080 and
2160 rows; vignette symmetric and 1 at the center; exposure speeds both ways; motion blur zero for a still camera and
equal with a shutter of 0, a moving camera's blur length; both paths equal. Perf (provisional): `PerfGPU.Renderer.MotionBlur4K`,
`Post4K`.

Defaults it changes: dithered output everywhere; FXAA's subpixel; Filmic and Exposure 0 for new components and the
template; the vignette's model; bloom's size at 4K; adaptation speeds; motion blur in the template and samples.

### 4.5 `light-and-materials`: believable light

Goal: light arrives and leaves surfaces as it does in the world: a new project opens sunny with a real sun to sky ratio,
rough metals keep their energy, occlusion grounds every object without TAA, highlights hold at 4K without sparkle, and
distance fades into the atmosphere's haze.

Steps:

1. **Lighting debug views** (`debugView` "lighting" (white albedo), "ao", "indirect", "luminance" (EV100 false color),
   "albedo" (outside 0.03-0.85 marked)), with statistics in the capture's result. The audit had to fake each of them.
2. **Energy compensation:** specular times `1 + F0 (1 / (A + B) - 1)` from the existing BRDF table, for image light
   and direct light, base and clear coat, and diffuse scaled by what specular takes; the white-furnace regression test
   (a metal sphere of roughness 1 reads at least 0.9 F0 L; 0.31 today).
3. **Ambient occlusion without TAA:** GTAO quality rows (Low 1x4, Medium 2x6 as 0.6.0, High 3x8, Ultra 4x12), a
   spatial denoiser that needs no history (interleaved 4x4 noise, a 4x4 bilateral resolve, then today's edge-aware
   blur), slices that no longer turn with the TAA frame, `AmbientOcclusionRadius` default 1.0 m with screen-space
   minimum and maximum, a thickness heuristic, and multi-bounce on albedo (our own fitted polynomial) in Forward.frag.
4. **Balanced light by default:** the `basic3d` template uses the physical atmosphere with a 100,000 lux sun at about 40
   degrees (with `camera-and-tone`'s Filmic at Exposure 0 it opens sunny with deep, colored shadows); the procedural
   sky's default colors scaled to a real ratio for new components; sun and sky presets (Clear, Hazy, Overcast, Golden
   hour) as `lighting.applySkyPreset` (one undo step) and SkyLight Inspector buttons; the samples adopt: the Physics
   Playground's flat procedural defaults become a sunny sky, the Open World's sun loses its cyan tint and its Kenney
   leaves their sRGB-as-linear factors (a glTF import setting `BaseColorFactorsAreSRGB`, off by default, so the
   importer's output only changes where set), the Third-Person Adventure keeps its HDR sky. `Lighting.md` gains the
   ratio table.
5. **Specular that holds at 4K:** geometric specular anti-aliasing (roughness widened by the normal's variance over the
   pixel, base and coat, the same in what SSR reads), the prefiltered sky at 512 per face at High (`EnvironmentResolution`
   row, 256 at Low and Medium, about 17 MB), and a specular horizon fade for normal-mapped reflections.
6. **Aerial perspective** from the atmosphere: a 32x32x32 froxel volume of in-scattered light and transmittance per
   frame from the transmittance and multiple-scattering tables (SRV 29), applied by depth in the forward and transparent
   passes and combined with height fog; on by default for atmosphere skies (`SkyLightComponent::AerialPerspective`,
   `AerialPerspectiveScale`). Then DG "Lighting", `Lighting.md`, the AGENTS.md bullet, the samples' pictures.

**Cut line.**

7. **Reflections without TAA:** a spatial SSR mode (trace on a nearest-depth pyramid, a roughness-aware bilateral
   resolve, the frame's own pre-transparent color) and the `Reflections` row (Low Off, Medium 0.6.0's checkerboard with
   TAA, High spatial at full resolution, Ultra more steps).
8. **Volumetric fog and light shafts:** a froxel grid independent of the screen's resolution with density from height
   fog plus noise, the sun through the cascades (`Shadows.glsl`'s cascade functions) and shadowed local lights,
   integrated front to back with fixed stratified samples and a 3x3 spatial filter (no reprojection); the `Volumetric`
   row.
9. **Sky visibility and bounce light:** a baked irradiance volume (L1 spherical harmonics with validity, about 2 m
   spacing, per scene or world cell, `lighting.bakeVolume` reusing the probe bake's capture; SRV 30-31) that every
   surface samples, with reflection probes normalized by it, so interiors, alleys and forests stop glowing.
10. **Bent normals and cone specular occlusion**, and reflection probes' "Fit to room".
11. **Alpha cutouts that hold:** coverage-preserving alpha mips (`TextureImporter` version bump) and alpha sharpened at
    the cutoff in the prepass, for foliage.

Tests: the furnace (energy), AO under FXAA and without AA has no pattern (a flat area's variance bound) on both paths,
AO quality rows, specular AA lowers a bumpy sphere's highlight variance, aerial perspective's transmittance is 1 at the
camera and matches the sky at the horizon, `lighting.applySkyPreset` one undo step, the glTF import setting, both paths
equal. Perf (provisional): `PerfGPU.Renderer.AOHigh4K`, `Aerial4K`.

Defaults it changes: energy compensation and specular anti-aliasing everywhere; AO's radius, multi-bounce and
denoising; aerial perspective for atmosphere skies; the template's sky and sun; the procedural sky's colors for new
components; the prefiltered sky's size at High.

## 5. Quality presets after the wave

| Row (field of `Renderer.Quality`) | Low | Medium (0.6.0's costs) | High (default) | Ultra | Added by |
| --- | --- | --- | --- | --- | --- |
| `ShadowResolution` | 1024 | 2048 | Auto: 2048 below 1440 rows, 4096 from 1440 | Auto: 4096 / 8192 | step 0 |
| `ShadowCascades` | 3 | 4 | 4 (6 if step 8 lands) | 4 (8 if step 8 lands) | step 0 |
| `LocalShadowAtlasSize` / `LocalShadowMaxTile` | 2048 / 512 | 4096 / 1024 | 4096 / 1024 | 8192 / 2048 | step 0 |
| `MaxShadowedLocalLights` | 4 | 16 | 16 | 16 | step 0 |
| `LODErrorPixels` / `MinProjectedPixels` | 2 / 1 | 1 / 0.5 | 1 / 0.5 | 0.5 / 0.25 | step 0 |
| `ShadowFilterQuality` (wide-penumbra taps) | 8 | 16 | 24 | 32 | `sun-shadows` |
| `ContactShadows` | Off | Off | 12 steps | 16 steps | `sun-shadows` |
| `AmbientOcclusionQuality` | 1x4 | 2x6 | 3x8 | 4x12 | `light-and-materials` |
| `EnvironmentResolution` | 256 | 256 | 512 | 512 | `light-and-materials` |
| `Reflections` (below the cut) | Off | Checkerboard (TAA) | Spatial | Spatial, more steps | `light-and-materials` |
| `Volumetric` (below the cut) | Off | Low | Medium | High | `light-and-materials` |
| `MotionBlurQuality` | Off | 8 samples | 12 samples | 16 samples | `camera-and-tone` |
| `AntiAliasing` (player) | Scene | Scene | Scene | Scene | `camera-and-tone` |
| `MotionBlur`, `DepthOfField`, `FilmGrain`, `ChromaticAberration` (player comfort) | Scene | Scene | Scene | Scene | `camera-and-tone` |

Scene-look settings stay on the components (lights, Post Process, Sky Light, Height Fog, the camera): quality rows scale
what a scene asks for, comfort rows can only turn it off. No row lowers the rendering resolution.

## 6. Defaults that change

Stored values never change: a scene that saved a value keeps it. These change what new components, the template, scenes
without the component and every scene's shading get; each is named in the 0.7.0 release notes.

| What | 0.6.0 | Wave 7 | Where |
| --- | --- | --- | --- |
| Programs' rendering quality (editor views, Game view, captures, games) | fixed in code (Medium) | `Renderer.Quality`, default High: 4096-texel cascades from 1440 rows | step 0 |
| `SceneRendererSpecification{}` (tests, code) | Medium | Medium (unchanged) | step 0 |
| `DirectionalLightComponent::ShadowSoftness` | 1.0 degree | 0.53 (the sun) | `sun-shadows` |
| PCSS radius, least filter, temporal rotation | full width, rotated 1-texel noise, turned per frame | half width, fixed tent, still | `sun-shadows` (all scenes) |
| Contact shadows | none | 0.3 m on the sun, on at High and Ultra | `sun-shadows` |
| Cascade layout | from the camera's near plane | fitted to the visible receivers | `sun-shadows` |
| 8-bit output | not dithered | dithered | `camera-and-tone` (all scenes) |
| FXAA subpixel blend | 0.75 | 0.375 | `camera-and-tone` |
| Tone curve of new Post Process components and the template | ACES, Exposure +1 | Filmic, Exposure 0 | `camera-and-tone` |
| Vignette | ring after the curve, 0.2 | lens fall-off before the curve, 0.12 | `camera-and-tone` (model for all scenes) |
| Bloom at 3840x2160 | half the 1080p halo | the same share of the image | `camera-and-tone` |
| Exposure adaptation | one speed | brighter faster than darker | `camera-and-tone` |
| Motion blur | none | component off; template and 3D samples on, 180 degrees | `camera-and-tone` |
| Specular energy, specular anti-aliasing | none | on | `light-and-materials` (all scenes) |
| Ambient occlusion | 0.6 m, 2x6, noisy without TAA | 1.0 m, 3x8 at High, denoised, multi-bounce | `light-and-materials` |
| Template sky and sun | procedural, 3 lux sun | physical atmosphere, 100,000 lux sun at about 40 degrees | `light-and-materials` |
| Procedural sky colors (new components) | zenith 0.31, horizon 0.73 nits | scaled to a real ratio | `light-and-materials` |
| Aerial perspective | none | on for atmosphere skies | `light-and-materials` |
| Prefiltered sky at High | 256 per face | 512 | `light-and-materials` |

The samples adopt the look through commands: the Third-Person Adventure (softness 0.53, Filmic at Exposure 0, motion
blur, AO radius, its HDR sky kept, a Graphics row in its menu), the Open World (softness, Filmic, aerial perspective, a
neutral sun, corrected leaves, motion blur), the Physics Playground (a sunny sky instead of the flat defaults, softness,
Filmic, motion blur). Tetris changes only through the shading defaults; its picture is retaken if they show. The last
stream to land retakes every sample's `Picture.png` and screenshots after merging main; the integration compares them
with the look-dev baseline.

## 7. Integration and release

1. Each stream, when its steps above the cut are done: an independent review (findings fixed or answered), NVIDIA
   Debug ctest with every GPU suite under the validation layer, Release ctest without perf, `CompatLint.py`, then CI
   green on `ci/wave7-<key>`; merge main, resolve the shared files (append rows, frame constants and stages; take main's
   pictures and retake), and land in the order of section 4.
2. After the last landing, alone on the machine: the one perf campaign (`PerfStability.py --runs 5 --wait-quiet`),
   which measures the streams' provisional metrics and new High metrics at 3840x2160 (the Open World tour's GPU busy
   and fixed renders of the other two samples at High), and interleaved A/Bs of Medium at 1920x1080 against step 0's
   tip; acceptance per section 1.1. A regression is fixed before the release, not budgeted.
3. The look-dev harness's before/after sheet (0.6.0, step 0, wave 7) for the owner; `build/windows` rebuilt so the owner
   judges the editor and the samples.
4. The GPU suites on the Mac (MoltenVK) before the release: waves 4-6 were never run on NVIDIA, and wave 7 is built on
   NVIDIA only; if the Mac cannot run them, the release notes and the open items say so.
5. Release 0.7.0 (`ctest --preset windows-dist -LE "gpu|perf"`, the `-rc1` dry run), Status "Wave 7 landed", and what
   fell below the cut lines moved into "Later".

## 8. Risks

| Risk | Mitigation |
| --- | --- |
| Four streams edit the renderer's hotspots (`RenderFrame`, Forward.frag, `RendererComponents`, `Feature.stscene`, frame constants, the preset table, debug views) | Step 0's seams (post and AO in their own files, preset rows one per line, assigned slots), ownership of Forward.frag by region (sun term to shadows, indirect block to light), the merge order, and every stream merging main before it lands |
| The shadow mask's reconstructed positions break the acne test or the cache's exact case; its blur bleeds across edges; transparent surfaces differ from opaque ones beside them | D32 reversed-Z depth and the prepass's geometric normals are what Forward.frag uses today; tests compare the mask with per-pixel evaluation; the blur's radius is zero where the penumbra is narrow; transparent surfaces use the same deterministic filter |
| SDSM: two-frame latency on fast camera moves, shimmer when boxes change | margins, quantized sizes with hysteresis, today's layout on cuts; the cut line puts it last above the cut |
| A shading change breaks the paths' pixel equality (derivatives in specular anti-aliasing) | only per-primitive derivatives, proven by `GPU.SceneRenderer.GPUDriven` before landing |
| Defaults change the samples and new projects the owner judges | the look-dev before/after sheet, every change named, stored values kept, each default reversible on its component |
| GPU cost on machines slower than the 5090 (about 6 times for a mid-range card) and memory (High's cascades 256 MiB per view renderer; the editor has three) | Medium keeps 0.6.0's costs; High's 5 ms at 4K here scales to roughly 13 ms at 1440p on a mid-range card, where players choose Medium (one row of their options); captures release cascades; Ultra is opt-in |
| MoltenVK and Linux drivers are unverified (CI runs no GPU test) | fragment passes where comparison sampling is needed, no sparse textures or mesh shaders, the Mac gate before the release |
| ABI additions are permanent | `graphics-options`' signatures reviewed in its spec; named options instead of one function per knob |
| A taste risk in Filmic and the template's look | calibrated against stated targets, ACES kept, the owner sees the sheet; one property switches back |
| Machine load and stability with four builds and GPU suites at once | perf only at integration, `PerfLock.py`, retries of one-off compiler crashes, as in waves 2-6 |
| An agent's day ends before its steps | ordered steps, each landing working; cut lines; what is left moves to "Later" |

## 9. Left out, and why

- **Render scaling below 1, dynamic resolution, TAA or other temporal upscaling:** the owner's rule (2026-10-26); struck
  from the roadmap, also from the open items, with "fewer GTAO samples accumulated by TAA".
- **Virtual shadow maps** (XL): 4096 cascades with the fitted depth range and the deterministic filter reach about one
  texel per pixel at a fraction of the effort; sparse depth through MoltenVK is unverified and the CPU path would need
  per-page culling. Revisit only if the shadows debug view shows cascades falling short.
- **MSAA** (XL): every pass and both paths' pipelines change; TAA Sharp, morphological AA and specular anti-aliasing
  cover the cases without it.
- **Forward.frag specialization constants** (frame-level variants): the screen-space shadow mask takes the largest unused
  path out of the material shader; measured again at integration, and done later only if `GPU.Opaque` shows the need
  (pipeline-compile hitches on MoltenVK are the risk).
- **Exposure calibration of ACES, AgX and the others:** it would shift every stored scene by about +0.8 EV and
  wash out the samples' +1 EV; the new Filmic is calibrated instead and becomes the default.
- **Lightmaps, screen-space GI and ray-traced GI:** UV2 unwrapping and a baker (XL, static only), temporal accumulation,
  and no ray tracing through MoltenVK. The baked irradiance volume (below the cut) is the chosen path.
- **A renderer fix for the Open World's cyan trees:** the importer reads glTF as specified; the exporter's sRGB factors
  get an import setting and the sample's sun its neutral color.
- **Render scale above 1 for live views:** every pass that assumes the viewport is the output changes; supersampled
  captures (offline, below `graphics-options`' cut) serve stills.
- **Texture quality (mip bias) rows and per-light shadow resolution scales:** later, with game-shell's options.
- **The showcase in this wave:** the owner asked for it after the wave; it is wave 8's first stream (below), and needs
  this wave's features to show.

## 10. Wave 8: the graphical showcase (outline)

Wave 8 starts with `showcase`: a sample made through the editor, like the others, that shows wave 7's look and becomes
the reference scene for the presets' budgets.

- **The place:** "High Desert Outpost", an original scene in the kind of climate the baseline look is known for: a
  rocky valley of about 300 m with a plaster-and-plank hut, crates, barrels, a covered car, a lantern and a fire pit,
  framed by boulders and cliffs, under the physical atmosphere through a time of day.
- **Assets:** Poly Haven CC0 scans (all sources answered the settings audit's requests): `rocky_terrain_02` at 2K with
  `brown_mud_dry` or `coast_sand_rocks_02` for the ground, `namaqualand_boulder_02` at 2K as the hero rock,
  `boulder_03` and `cliff_02` at 1K, `othonna_cerarioides`, `dry_branches_medium_01`, `grass_medium_01` and
  `dead_tree_trunk_02`, `wooden_crate_01`, `Barrel_01` and `_02`, `cardboard_box_01`, `concrete_road_barrier`,
  `Lantern_01`, `stone_fire_pit`, `covered_car`, `beige_wall_001`, `weathered_brown_planks`, `rust_coarse_01`; terrain
  from a committed generator. About 60 MB of source assets, in the showcase budget class (80 MB; about 140 MB of
  binaries over all samples). No people: no realistic CC0 or CC-BY animated humans were found.
- **Five shots, a camera tour between them:** a dawn vista (aerial perspective, long soft shadows), the outpost's
  contact (ambient occlusion, contact shadows, material detail at 4K, reflections in a puddle), the hut's interior
  (light shafts, sky occlusion), dusk to night (the shadowed lantern and fire), and a macro of the hero rock.
- **A photo mode:** free camera, time of day, exposure, focus (depth of field) and the Graphics options menu
  (`graphics-options`' API). Defaults to High; its screenshots at Ultra with supersampling.
- **Checks:** `PerfGPU.Samples.Showcase.{High,Ultra}` at 3840x2160, image checks at the fixed shots through the look-dev
  harness, the package tests, `Samples.Content`.
- **Prerequisites:** wave 7's streams above their cut lines; from below them, if they did not land, the showcase stream
  takes or waits for depth of field, volumetric light shafts, the irradiance volume (the hut), coverage-preserving alpha
  (grass and branches), distant shadows (the vista) and supersampled stills; plus what no wave has yet: terrain material
  blending or detail normal maps (materials have nine maps and no layers) and one baked time of day for the probes (or
  run-time probe capture for a moving sun).
