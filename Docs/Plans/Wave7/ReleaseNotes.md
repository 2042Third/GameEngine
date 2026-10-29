# Wave 7: release notes for 0.7.0

What wave 7 brings, as integrated on 2026-10-28 (step 0 and the six streams: `graphics-options`, `sun-shadows`,
`camera-and-tone`, `light-and-materials`, `physics-runtime`, `editor-perf`), in one list: the defaults are those that
were built, which differ from the plan's section 6 where the streams' measurements and the decisions
(`specs/decisions.md`) changed them. `Docs/Releases/0.7.0.md` is written from this file; the folder goes when the wave
lands.

## What's new

**Graphics quality for people, agents and players** (step 0, `graphics-options`)

- Graphics quality presets: the project section `Renderer.Quality` with Low, Medium (0.6.0's costs), High (the default)
  and Ultra, or Custom values. Project Settings' **Graphics Quality** page shows the presets side by side in one table,
  a row per setting with its memory at 1920 x 1080 and 3840 x 2160 and the GPU time of its stage in the Scene view, the
  chosen column highlighted and Custom's column editable. Games, the editor's Scene and Game views and captures follow
  it and change it while they run.
- The Scene view has a quality chip (Project, or a preset for this view on this computer: `viewport.setSettings
  {quality}`); `viewport.capture {quality}` draws one picture at a preset, and captures otherwise draw at their view's
  quality and match it (0.6.0's captures used half the point and spot lights' shadow atlas). The Scene view's "Game
  camera" mode and captures of the scene's camera draw at the game's quality.
- Players' graphics options: games offer a Graphics menu through `StrataScript/Graphics.h` (`Graphics::SetPreset`,
  `Graphics::Set` by a row's name, `Graphics::Reset`, `Graphics::Save`, `Graphics::Options()` with each row's title,
  description and choices); the choice is kept in `GraphicsOptions.json` beside the game's saves and loaded when the
  game starts. `<Game> --quality low|medium|high|ultra` plays one run at a preset without saving anything. The
  Third-Person Adventure's settings menu has a Graphics row (Low to Ultra).
- Players' rows, the same under every preset: **Anti-aliasing** (the scene's method, Off, FXAA, TAA or Morphological),
  and **Motion blur**, **Depth of field**, **Film grain** and **Chromatic aberration** (the scene's, or Off: players can
  turn off what a scene uses, never turn on what it does not).
- The Inspector groups Post Process and Directional Light properties under headings (Tone and color, Exposure, Bloom,
  Ambient occlusion, Anti-aliasing, Reflections, Lens, Motion; Light, Shadows; a Camera's lens under Depth of field),
  every one with a description and its unit; the sun shows its shadow maps' resolution and a Post Process the
  screen-space reflections the quality draws, each with a link to the page.
- Agents: `project.setSettings {"section": "Renderer.Quality", ...}`, `graphics.resetPlayerOptions`, and
  `editor.status`'s `game.graphics` (preset, what is in effect and where each value comes from).
- `Docs/Manual/Graphics.md` explains the presets, their costs and the players' options.

**Shadows of a real sun** (`sun-shadows`)

- Physical soft shadows: the sun's penumbrae are as wide as a sun of the light's **Shadow Softness** makes them (0.53
  degrees, the real sun's, by default), sharp at contact, without grain and the same in every frame with or without
  TAA; a blocker search over a conservative depth pyramid keeps thin casters such as wires and grass.
- Opaque surfaces take the sun's shadow from a screen-space mask drawn after the depth prepass, whose edge-aware blur
  removes the soft penumbrae's grain within the frame without letting light cross a silhouette; transparent surfaces
  keep a per-pixel filter. New GPU stage `GPU.ScreenShadows`.
- Contact shadows ground small objects the cascades are too coarse for: the mask marches toward the sun over the depth
  buffer (**Contact Shadow Length** on the Directional Light, 0.3 m; 0 off), at High and Ultra.
- The cascades follow what the camera sees: a few frames after the view changes they cover only the distances from the
  nearest to the farthest surface in view, and each but the farthest shrinks to the surfaces it shades (**Cascade
  Distribution**, 0.9, shares the distances). At 3840 x 2160 on High the samples' views now have about one shadow texel
  per screen pixel or finer (median 0.3-1.35 screen pixels per texel, none above 4; 0.6.0: 2.2 to 8.7).
- Graphics Quality gains **Shadow Filter Quality** (8, 16, 24 or 32 taps for wide penumbrae) and **Contact Shadows**
  (off, off, 12 and 16 steps).
- A `shadows` debug view (the viewport's debug menu, `viewport.setSettings {debugView}`, `viewport.capture
  {debugView}`) colors every surface by the screen pixels one shadow texel spans, outlines the cascades and hatches
  their blend bands; captures report each cascade's slice, texel size, share of the image and pixels per texel, and
  `editor.perf`'s render statistics each cascade's slice.

**The image of a camera** (`camera-and-tone`)

- Filmic, a tone curve of Strata's own, shows a metered middle gray as middle gray (sRGB 117-119 at Exposure 0; ACES
  needs about +0.8 EV for the same), rolls highlights to white over six and a half stops without clipping or skewing
  hues, and keeps the scene's colors in the shadows. **Toe** and **Shoulder** shape it.
- Color on Post Process: white balance (**White Balance Temperature** and **Tint**; 6500 K and 0 change nothing) and
  per-channel **Color Slope**, **Offset** and **Power**; color lookups (`.cube` files import as Color Lookup assets that
  **Color Lookup** applies after the tone curve, with **Color Lookup Intensity**).
- Automatic exposure adapts like an eye, to brighter scenes (**Auto Exposure Light Speed**, 3 per second) faster than
  to darker ones (**Auto Exposure Speed**, 1.5), meters the image's center more than its edges (**Auto Exposure
  Metering**: Center Weighted or Average) between percentiles of its own, and darkens scenes that meter below a gray
  card in the directional light's light (**Auto Exposure Key**, 0.12; at most 2 stops; it never brightens).
  `renderStats.exposure` reports what it metered.
- The output is dithered: skies, fog, dark falloffs and the vignette no longer band (the widest run of one 8-bit code in
  the Physics Playground's sky went from 113 to 4-5 pixels).
- Bloom reaches the same share of the image at every resolution (**Bloom Radius**), and the vignette is a lens's
  fall-off applied before the tone curve.
- Motion blur of the camera and of moving objects (**Motion Blur** on Post Process, a shutter angle and a length limit),
  never in the editor's views or captures; Graphics Quality's **Motion Blur Quality** sets its samples. It is off by
  default, in the template and in every sample.
- Depth of field through a camera's physical lens (**Depth Of Field** on the Camera: a focus distance or a focus entity,
  an aperture as an f-number and a sensor height), shown in the Game view and in captures of the scene's camera.
- **Film Grain**, **Chromatic Aberration** and **Lens Flare** on Post Process, each off by default.
- Morphological anti-aliasing (**Anti Aliasing Method** Morphological): no history, the lines that jagged edges stand
  for reconstructed, texture detail left alone (0.88 of the image without anti-aliasing on a tiled floor; FXAA 0.87).
- `Docs/Manual/Camera.md`: exposure, tone, color, bloom, vignette, motion blur and depth of field, their costs and
  recipes for a sunny day, an interior and a night.

**Believable light** (`light-and-materials`)

- Rough metals keep their light: specular is compensated for multiple scattering (a 0.95 metal of roughness 1 reflects
  0.955 of its color in a white furnace; 0.6.0: 0.31), and diffuse gets what specular leaves.
- Ambient occlusion is denoised within the frame: the same image with TAA, FXAA or no anti-aliasing, still from frame
  to frame, with a 1 m radius kept between 0.3% and 12% of the view's height on screen, no halos around thin occluders
  and multi-bounce for bright surfaces (**Ambient Occlusion Multi Bounce**). Graphics Quality gains **Ambient Occlusion
  Quality** (1 x 4, 2 x 6 as 0.6.0, 3 x 8 and 4 x 12 slices x steps).
- Highlights hold at 4K without sparkle (specular anti-aliasing widens highlights narrower than a pixel), and
  reflections a normal map turns into its surface fade out.
- Aerial perspective: under the atmosphere sky the air between the camera and distant surfaces dims them and adds its
  light (Sky Light **Aerial Perspective** and **Aerial Perspective Scale**; GPU stage `GPU.Aerial`, 512 KiB per view).
- Sun and sky presets: Clear, Hazy, Overcast and Golden Hour, buttons in the Sky Light's Inspector section and
  `lighting.applySkyPreset`, each a sky and a sun of real values in one undo step.
- Screen-space reflections at High and Ultra trace every pixel at full resolution with a spatial resolve and need no
  TAA; Graphics Quality gains **Reflections** (Low off, Medium 0.6.0's half-resolution trace with TAA, High Spatial,
  Ultra SpatialHigh) and **Environment Resolution** (the prefiltered sky: 256 per face at Low and Medium, 512 at High
  and Ultra).
- Lighting debug views: `lighting` (every surface white, lit as the scene lights it), `ao`, `indirect`, `luminance`
  (EV100 bands) and `albedo` (base colors below 0.03 and above 0.85 marked); `viewport.capture`'s `debugView.lighting`
  reports the frame's EV100 percentiles, albedo shares and the sun to sky ratio on flat ground.
- glTF import setting **Base Color Factors Are sRGB**, for exporters that write sRGB colors into glTF's linear factors
  (Kenney's kits).

**Physics that runs fast and settles** (`physics-runtime`)

- Physics steps cost less in busy scenes: the job threads write simulated poses back to entities and check the bodies
  before each step, and contacts nobody listens to are no longer looked up. Strata's part of a 70,000-box step fell from
  0.78 to 0.47 of box3d's own step (9.84 to 5.93 ms on the loaded test machine), of 30,000 boxes with tracked contacts
  from 0.94 to 0.66 and of 10,000 boxes from 0.59 to 0.39; scenes that listen to some contacts look up only the bodies
  that touch something tracked.
- **Physics > Simulation** in Project Settings (`Physics.Simulation`), changeable while the game plays: Contact Recycle
  Distance (box3d's reuse of resting contacts, 5 cm, kept by measurement), Sleep Threshold (0.08 m/s), Contact
  stiffness, Contact damping and Overlap push speed.
- A streamed cell that unloads parks its static bodies: they collide with nothing at once and leave box3d about a second
  later in slices (the frame that unloads a 25,000-body cell went from 7.8 to 3.4 ms on the loaded test machine).
- `physics.stats` reports the step's parts and box3d's stages, sleeping bodies and islands, contacts touching, awake and
  recycled, how piles settle, parked bodies and the tuning in effect; `physics.setDetailedStats` (and the Streaming
  panel's Count contacts) counts contacts every step; `editor.status` shows the running game's bodies, awake bodies and
  step time, the runtime's perf log seven physics columns, and the Streaming panel a Step row.

**A faster editor** (`editor-perf`)

- The Performance panel costs a frame less than half what it did in 0.6.0: its statistics, table text and timeline no
  longer grow with every registered stage.
- The Content Browser's first thumbnails no longer hitch the editor (the longest frame while rendering them went from
  about 2.4 ms to about 1 ms): small scenes hand their GPU buffers to the next one, thumbnails and Asset previews draw
  through the CPU path with the same pictures, and GPU timestamps reset on the host instead of in a submission per
  frame.
- Editing and playing scenes without mesh renderers (empty entities, lights, cameras) costs about a sixth less CPU per
  frame (100,000 nested entities: 0.24 against 0.29 ms): the GPU-driven renderer records no culling, occlusion or
  feedback for a scene with nothing to draw.
- Opening a large scene rarely freezes frames for 4-11 ms any more: the parallel read of its file runs below the
  editor's frame, and the scene's GPU copy is made a frame before it is shown (the longest frame while opening 100,000
  entities over a million: about 1.2 ms instead of 4.2, median of opens).

**Tools**

- `Tools/LookDev.py` captures fixed shots of the samples, a new project and a look-dev scene through headless editors
  (stills at sizes, anti-aliasing modes, presets and debug views, and live shots of the playing game) and compares two
  sets of them.
- Exported games run for a span of game time with `--seconds S` (with `--screenshot`, the picture of that moment,
  however many frames it took); runs that end by themselves keep every fixed step.
- `Tools/FitFilmic.py` (with `--check`), `GenerateWhiteBalance.py`, `GenerateBlueNoise.py`, `GenerateAreaTable.py` and
  `FitAOMultiBounce.py` write the curves and tables the wave's shaders use; `PhysicsSweep.py` measures how physics
  settles under each tuning.

## Defaults that change

Stored values never change: a scene that saved a value keeps it. These change what new components and projects, the
template, scenes without the property or component and every scene's shading get.

| What | 0.6.0 | 0.7.0 | Applies to |
| --- | --- | --- | --- |
| Programs' rendering quality (editor views, Game view, captures, games) | fixed (Medium's values) | `Renderer.Quality`, High by default: 4096-texel sun cascades from 1440 output rows, the rows below at every size | projects without the section |
| `SceneRendererSpecification{}` (engine code, tests) | Medium | Medium | code |
| Directional Light **Shadow Softness** | 1.0 degree | 0.53 (the sun) | new lights, files without the key |
| Penumbra width, narrow edges, temporal rotation | full width, rotated noise turned per frame | half the width for the same softness, a fixed tent, still | every scene |
| Contact shadows | none | **Contact Shadow Length** 0.3 m, drawn at High and Ultra | every directional light |
| Cascade layout | from the camera's near plane | fitted to the visible receivers (**Cascade Distribution** 0.9) | every scene |
| 8-bit output | not dithered | dithered | every scene |
| FXAA subpixel blend | 0.75 | 0.25 | every scene with FXAA |
| Tone curve | ACES, the template at Exposure +1 | Filmic at Exposure 0; the template at +0.25 | new Post Process components, scenes without one, the template |
| Vignette | a ring after the curve, 0.2 | a lens's fall-off before the curve, 0.12 (stored values keep their number) | every scene |
| Bloom at 3840 x 2160 | half the 1080p halo | the same share of the image | every scene |
| Exposure adaptation and metering | one speed, the average | brighter twice as fast (3 per second), center-weighted, a key of 0.12 | every Post Process without the keys |
| Motion blur | none | available, off | the component, the template and every sample |
| Specular energy, specular anti-aliasing, horizon fade | none | on | every scene |
| Ambient occlusion | 0.6 m, 2 x 6, noisy without TAA | 1.0 m, 3 x 8 at High, denoised, multi-bounce | radius: new components and files without the key; the rest every scene |
| Template sky and sun | procedural sky, 3 lux sun | physical atmosphere, a 120,000 lux sun 40 degrees up (sun to sky 8.1 on the ground) | the Basic 3D template |
| Procedural sky colors | zenith 0.31, horizon 0.73 nits | a quarter of those (sun to sky about 8 with the default sun) | new Sky Lights |
| Aerial perspective | none | on, scale 1 | every Sky Light without the keys (shows under the atmosphere) |
| Prefiltered sky | 256 per face | 512 at High and Ultra | every scene with a sky |
| Screen-space reflections | half resolution, TAA only | every pixel at High and Ultra, off at Low | scenes that turn them on |
| Sleep threshold | 0.05 m/s | 0.08 m/s | projects without `Physics.Simulation` |
| Contact recycling of character controllers and kinematic bodies | on | off | every scene |

The template's +0.25 and the samples' exposures follow the rule of the camera spec's section 5.7: the median display
luminance of the start scene's camera image (1920 x 1080, TAA) within [0.15, 0.22], in steps of 0.25 EV. Measured on
the integrated Release build: the template 0.188 at +0.25 (0.148 at 0), the Third-Person course 0.178 and its main menu
0.171 at 0, the Physics Playground 0.187 at -0.25 (0.227 at 0) and the Open World 0.161 at 0.

## Upgrading

- **How 0.6.0's scenes look.** Every scene's sun shadows change: penumbrae are half as wide for the same Shadow
  Softness (set it to twice its old value to keep a scene's width), narrow edges are filtered by a fixed tent, contact
  shadows appear at High and Ultra (set **Contact Shadow Length** to 0 to keep a light's old look; they come from every
  opaque surface on screen, also meshes whose Cast Shadows is off) and the cascades are fitted to the view. The output
  is dithered, FXAA blends less, the vignette follows a lens (0.2 now darkens a 60 degree view's corners by 16.5%
  instead of 20%), bloom keeps its size at 4K, metals keep their energy (a metal of roughness 1 reflects up to three
  times what it did; dielectrics' specular grows about 9%), ambient occlusion is denoised and gains multi-bounce (turn
  **Ambient Occlusion Multi Bounce** off for 0.6.0's darker creases), and scenes under the atmosphere gain aerial
  perspective (turn **Aerial Perspective** off on the Sky Light to keep 0.6.0's look). Scenes that stored ACES keep it.
- **Exposure.** Stored scenes meter center-weighted, adapt to brighter scenes twice as fast and darken what meters below
  the directional light's gray card: for 0.6.0's exposure set **Auto Exposure Metering** to Average, **Auto Exposure
  Key** to 0 and **Auto Exposure Light Speed** to the scene's **Auto Exposure Speed**. A Post Process moved from ACES to
  Filmic needs about 0.8 EV less exposure compensation. `lighting.applySkyPreset` turns a manual exposure automatic.
- **Quality and memory.** Projects use High unless they choose: at 1440 output rows and more, sun shadows are drawn into
  4096-texel cascades (256 MiB per view with a shadowed sun instead of 64), and at every size High adds the sun's
  24-tap filter, contact shadows, 3 x 8 ambient occlusion, a 512 prefiltered sky (12 MiB more per view, 24 with the
  atmosphere) and, in scenes that turn screen-space reflections on, a full-resolution trace (about 232 MiB per view at
  3840 x 2160 instead of 79). Choose Medium to keep 0.6.0's costs. Graphics.md lists every preset's memory.
- **Physics.** Bodies fall asleep below 0.08 m/s instead of 0.05: piles sleep a quarter to a third sooner and slow
  bodies stop a little sooner (Physics.Simulation's Sleep Threshold 0.05 for the old behavior). Character controllers
  and kinematic bodies no longer reuse contacts, so what they carry or push meets them where they are. A static body
  that leaves the simulation without contacts stays in box3d's trees, colliding with nothing, for 60 fixed steps.
- **One more import.** Every glTF model imports once more (importer version 11), and asset thumbnails are made again
  (the cache's version is 10).
- **Scripts.** The script ABI keeps version 1: seven host functions for graphics options are appended (162 in all) and
  none removed, so modules built for 0.6.0 load unchanged, and modules that use the options run on 0.6.0 engines (the
  SDK's calls do nothing there).
- **Exported games** keep the runtime they were exported with: export again to get 0.7.0's runtime, the project's
  Graphics Quality and the players' options. Players' choices are saved per game in `GraphicsOptions.json`.
- **The samples** adopted the look through editor commands: Shadow Softness 0.53 (Tetris keeps 1.0), Filmic with the
  exposures above and the lens vignette 0.12, ambient occlusion of 1 m, motion blur off; the Physics Playground is lit
  by the Clear preset; the Open World's sun is white, its Kenney models' colors are read as sRGB (teal-green leaves
  instead of a pale cyan wash), its meadow is greener and its aerial perspective scale is 6. Copies made with 0.6.0 stay
  as they are: open a new copy to get 0.7.0's look.
- **Engine code.**
  - `SceneRendererSpecification`'s `ShadowMapSize`, `ShadowCascades`, `LocalShadowAtlasSize`,
    `MaxShadowedLocalLights`, `LODErrorPixels` and `MinProjectedPixels` moved into its `Quality`
    (`RendererQualitySettings`, which also holds the wave's other rows); `ShadowMapSize` became `ShadowResolution`.
    `ProbeBake::GetRendererSpecification` became `SceneRendererSpecification::ForBake()`, beside `ForThumbnails()`
    (which sets `GPUDriven` Off); `ViewportRenderer::Render` takes the quality to draw at.
  - Renderers fit the cascades to the view unless `SceneRendererSpecification::FitShadowsToView` is off (thumbnails and
    probe bakes); shaders select cascades with `SelectShadowCascade(viewDepth, position)`, by containment
    (`SelectCascade` is gone).
  - Tools that compare exact pixels turn `SceneRendererSpecification::DitherOutput` off (tests:
    `Tests::ExactSpecification()`). `RenderData::TonemapConstants` replaces the tone mapping's push constants;
    `RenderPostHDR` and `RenderPostDisplay` take the frame's post source and report degraded steps;
    `Renderer/PostProcessMath.h` holds the CPU twins of the post shaders' math.
  - `SceneRenderOptions` gains `LightingView` and `LightingColors`, `SceneRenderer::GetLightingViewStats` returns a
    view's statistics, and `SceneRendererStats` gains `GPUCulled`.
  - `renderStats.gpuDriven` means the frame took the GPU-driven path; the new `gpuCulled` says the GPU culled the
    scene's mesh renderers (false in a scene without any, whose frames take their asset requests from the CPU).
  - `JobSystem::SubmitBackgroundPart` queues parts of background work in progress ahead of work not started; a scene
    asset's load reads at `JobPriority::Background`; `RenderSceneGPUCache` hands the buffers of a released mirror of at
    most 1 MiB to the next mirror (binding sets compare buffer versions, never mirrors or scene ids).

## Smaller changes and fixes

- The atmosphere keeps its reflections after the sky switches to another source and back (0.6.0 could leave mirrors
  black until the sun moved).
- A renderer drawing fewer sun shadow cascades keeps fewer layers (thumbnails: 4 MiB instead of 16), and a capture
  renderer frees its cascades between pictures.
- A validation layer the loader lists but cannot load no longer keeps a device from starting on Windows.
- A frame whose morphological anti-aliasing, lens flare or color lookup could not get its resources is drawn without it
  and logs the failure once; a frame whose tone mapping cannot run renders nothing instead of a stale image.
- A released scene's GPU copy releases its light records' buffer with its other buffers, after the frames that read it.
- Creating and loading many bodies still costs what box3d's one-at-a-time insertion costs (10,000 bodies about 19 ms,
  10 of them box3d's own); recorded, not changed (the owner's ruling).

## Known issues at integration

- Motion blur ships off everywhere: in the live shots' criteria table only 3 of 8 measured edges were within 15% of the
  blur model (the others 0.53-2.51 times it) and a fast fence post's noise 0.047 against 0.028. The owner's
  before/after sheet decides before it is turned on anywhere, and the Third-Person menu has no Motion Blur row
  meanwhile.
- `GPU.ScreenShadows` costs 0.11-0.16 ms at 1920 x 1080 Medium (the samples' small scenes' GPU busy 1.15-1.24 times step
  0's) and 0.54-0.83 ms at 3840 x 2160 High (targets 0.35 and 0.45 ms): a wave perf pass cuts it before the release.
- The sun's penumbra noise reads 1.6% by the shadows audit's 9-pixel moving average in every anti-aliasing mode (the
  profile of a physically narrow penumbra, not grain) and 0.95% by the RMS of its second difference, the grain metric.
- Contact shadows can leave 1-pixel fringes beside thin foliage; a face exactly parallel to the sun gets half its
  shadow (unseen, since such faces receive no direct sun).
- FXAA keeps 0.87 of the texture detail of an image without anti-aliasing (target 0.88), morphological anti-aliasing
  0.88 (target 0.95); the dither costs 0.021 ms at 3840 x 2160 (target 0.01).
- The Open World's opaque pass grows 0.053-0.058 ms at 3840 x 2160 on High with the always-on shading (target 0.05).
- 1,000 walking characters take 0.89 ms a step (target 0.70); 1,000 falling boxes beside 70,000 sleeping ones make
  box3d's own step 4 to 7 times slower (box3d's cost, an upstream request).
- Opening a scene of a million entities still has frames of 1.2-6.5 ms in about half the opens on the integration
  machine (`PerfGPU.Editor.SceneOpenOver1MMaxFrame`, median 1.23 ms against its 1.143 ms budget).
- Wave 7 was built and tested on Windows with an NVIDIA GPU; its GPU suites have not run on the Mac (MoltenVK) yet.
