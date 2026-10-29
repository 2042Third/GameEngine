# Wave 7: release-note lines for 0.7.0

The lines each landing of wave 7 contributes to `Docs/Releases/0.7.0.md`, in the sections that file will have. The
integrator writes the release notes from them when the wave lands (and removes this folder with the specs).

## Step 0

**What's new**

- Graphics quality presets. Project Settings has a Graphics Quality page (Renderer.Quality) with Low, Medium (0.6.0's
  costs), High and Ultra, and Custom values for shadow resolution and cascades, point and spot light shadows and levels
  of detail. Games, the editor's Scene and Game views and captures follow it and change it while they run; the Scene
  view can use a preset of its own (`viewport.setSettings {quality}`), and `StrataRuntime --quality` plays a game at a
  preset for one run without saving anything.
- Captures match the views: `viewport.capture` draws point and spot lights' shadows as the view does (0.6.0 used half
  the atlas), and `quality` draws a picture at a preset.
- Exported games run for a span of game time with `--seconds S` (with `--screenshot`, the picture of that moment,
  however many frames it took); runs that end by themselves keep every fixed step.

**Upgrading**

- Projects use High unless they choose: at 1440 rows and above, sun shadows are drawn into 4096-texel cascades, 256 MiB
  per view with a shadowed sun instead of 64 MiB (below 1440 rows High keeps Medium's cascades, and at every size adds
  the sun shadows' 24-tap filter and contact shadows, and 12 motion blur samples where a scene uses motion blur). Choose
  Medium to keep 0.6.0's sample counts.
- Engine code: `SceneRendererSpecification`'s `ShadowMapSize`, `ShadowCascades`, `LocalShadowAtlasSize`,
  `MaxShadowedLocalLights`, `LODErrorPixels` and `MinProjectedPixels` moved into its `Quality`
  (`RendererQualitySettings`); `ShadowMapSize` became `ShadowResolution`. `ProbeBake::GetRendererSpecification` became
  `SceneRendererSpecification::ForBake()`, beside `ForThumbnails()`; `ViewportRenderer::Render` takes the quality to
  draw at.

**Smaller changes and fixes**

- The atmosphere keeps its reflections after the sky switches to another source and back (0.6.0 could leave mirrors
  black until the sun moved).
- A renderer drawing fewer sun shadow cascades keeps fewer layers (thumbnails: 4 MiB instead of 16), and a capture
  renderer frees its cascades between pictures.
- A validation layer the loader lists but cannot load no longer keeps a device from starting on Windows.

## graphics-options

**What's new**

- Graphics Quality: Project Settings' Graphics Quality page shows the presets Low to Ultra and Custom in one table with
  each row's memory and GPU cost, and the Scene view has a quality chip.
- Players' graphics options: games offer a Graphics menu through `StrataScript/Graphics.h` (`Graphics::SetPreset`,
  `Graphics::Set`, `Graphics::Save`); the choice is kept in `GraphicsOptions.json` beside the game's saves and loaded
  when the game starts. `StrataRuntime --quality` overrides it for one run. The Third-Person Adventure's settings have a
  Graphics row.
- Scripts: seven host functions appended to ABI version 1 (`GraphicsGetOptionCount` to `GraphicsSaveOptions`); modules
  that use them run on 0.6.0 engines without the options (the SDK's calls do nothing there).
- Inspector: Post Process and Directional Light properties are grouped under headings, and every one of them has a
  description with its unit; the sun shows its shadow maps' resolution and links to Graphics Quality.
- Agents: `graphics.resetPlayerOptions`; `editor.status` reports the game's graphics options and their sources.
- Samples: `Samples.json` entries may declare `"Budget": "Showcase"` (80 MB of source assets, counted apart) for wave
  8's showcase.

**Upgrading**

- Nothing changes in images from this part. A project's Graphics Quality applies to games it exports; players' choices
  are saved per game. In the editor the Scene view's "Game camera" view and captures of the scene's camera now draw at
  the game's quality (the project's, with the players' options while playing).

## sun-shadows

**What's new**

- A `shadows` debug view (the viewport's debug menu, `viewport.setSettings {debugView: "shadows"}` and
  `viewport.capture {debugView: "shadows"}`) colors every surface by how many screen pixels one texel of the sun's shadow
  cascades spans there, outlines the cascades and hatches their blend bands; captures report each cascade's slice, texel
  size, share of the image and pixels per texel.
- Physical soft shadows: the sun's penumbrae are as wide as a sun of the light's Shadow Softness makes them (it now
  defaults to 0.53 degrees, the real sun's; stored values are kept), sharp at contact, without grain and identical from
  frame to frame with or without TAA; the filter no longer misses thin casters such as wires and grass that the shadow map
  holds.
- Graphics Quality gains Shadow Filter Quality (8, 16, 24 or 32 taps for wide penumbrae; Medium keeps 16).
- Opaque surfaces take the sun's shadow from a screen-space mask drawn after the depth prepass, whose edge-aware blur
  removes the soft penumbrae's grain in the frame (no TAA needed) without letting light cross a silhouette; transparent
  surfaces keep a per-pixel filter, without the blur. New GPU stage `GPU.ScreenShadows`.
- Contact shadows ground small objects at High and Ultra: the sun's shadow mask marches toward the sun over the depth
  buffer and shades what the cascades are too coarse for (Contact Shadow Length on the Directional Light, 0.3 m; 0 off).
  Graphics Quality gains Contact Shadows (off at Low and Medium, 12 steps at High, 16 at Ultra).
- The sun's cascades follow what the camera sees: a few frames after the view changes they cover only the distances from
  the nearest to the farthest surface in view, and each cascade but the farthest shrinks to the surfaces it shades, so the
  shadow's texels go where the surfaces are (several times finer under a high camera looking down over a level). The
  farthest cascade still reaches the Shadow Distance, so what comes into view is shadowed at once. Cascade Distribution
  on the Directional Light (0.9) shares the distances among the cascades; captures wait for the fit (`shadowFitSettled`),
  and the render statistics of `editor.perf` and `viewport.capture` report each cascade's slice and texel size.

**Upgrading**

- Every scene's sun shadows change: penumbrae half as wide as before for the same Shadow Softness, narrow edges filtered
  by a fixed tent instead of rotated noise. Set Shadow Softness to 2 times its old value to keep a scene's old width.
- Thumbnails are made again (their cache's version is 10, with camera-and-tone's and light-and-materials' changes).
- The Third-Person Adventure's, Physics Playground's and Open World's suns have the real sun's Shadow Softness, 0.53
  degrees (Tetris keeps its 1.0).
- Every directional light gets contact shadows at High and Ultra (Contact Shadow Length 0.3 m for scenes saved before it):
  set it to 0 on a light to keep its old look. They come from every opaque surface on screen, also from meshes whose
  Cast Shadows is off.
- Every scene's cascades are fitted to the view (Cascade Distribution 0.9 for scenes saved before it). Renderers made from
  `SceneRendererSpecification` fit them unless `FitShadowsToView` is off (thumbnails and probe bakes turn it off); a
  renderer's first frames, and frames after a camera cut, keep the earlier layout until the GPU's reduction of the view
  comes back. Shaders that sample the cascades select them with `SelectShadowCascade(viewDepth, position)`, by
  containment: `SelectCascade` is gone.
## camera-and-tone

**What's new**

- Filmic, a camera-like tone curve, is the default of new Post Process components, of scenes without one and of the
  Basic 3D template: a metered middle gray shows as middle gray (no exposure compensation needed), highlights roll off
  to white over six and a half stops without clipping, bright saturated lights go to white without changing their hue,
  and shadows keep the scene's own colors instead of deepening them. **Toe** and **Shoulder** set how deep the shadows
  go and how softly highlights reach white.
- Color on Post Process: white balance (**White Balance Temperature** and **Tint**; 6500 K and 0 change nothing) and
  per-channel **Color Slope**, **Offset** and **Power**.
- Automatic exposure adapts like an eye, to brighter scenes (**Auto Exposure Light Speed**, 3 per second) faster than to
  darker ones (1.5), meters the image's center more than its edges (**Auto Exposure Metering**: Center Weighted, the
  default, or Average), takes its metering window from properties, and darkens scenes that meter below a gray card in
  the directional light's light (**Auto Exposure Key**, 0.12; at most 2 stops; it never brightens), so interiors read
  darker than sunlight as far as their light is darker. `viewport.capture` and `editor.perf` report what it metered
  (`renderStats.exposure`: `ev100`, `keyStops`, `cardEv100`).
- Motion blur (**Motion Blur** on Post Process, a shutter angle and a length limit; off by default, and off in the
  template and the samples), never in the editor's views or captures; Graphics Quality's **Motion Blur Quality** sets
  its samples (Off at Low), and players can turn it off.
- Depth of field through a camera's physical lens (**Depth Of Field** on the Camera: focus distance or a focus entity,
  aperture, sensor height; off by default).
- Film grain, chromatic aberration and lens flares (each off by default; grain is the same strength under every
  anti-aliasing method), and color lookups: `.cube` files import as Color Lookup assets that a Post Process applies.
- Morphological anti-aliasing (**Anti Aliasing Method** Morphological), for scenes that want sharp edges without TAA.
- Players' rows in Graphics Quality: the anti-aliasing method (the scene's, Off, FXAA, TAA or Morphological), and Motion
  Blur, Depth Of Field, Film Grain and Chromatic Aberration (the scene's or Off).
- `Docs/Manual/Camera.md`, a guide to the camera's image.

**Upgrading**

- The output is dithered: gradients (skies, fog, dark falloffs, the vignette) no longer band. Tools that compare exact
  pixels turn `SceneRendererSpecification::DitherOutput` off (tests: `Tests::ExactSpecification()`).
- FXAA's subpixel smoothing goes from 0.75 to 0.25: it keeps more texture detail (0.87 of the image without
  anti-aliasing on Physics Playground's tiled floor, from 0.79).
- New Post Process components and scenes without one use Filmic at Exposure 0, the Basic 3D template's start scene
  Filmic at +0.25 (its bright sky over dark stone); scenes that stored a curve keep it. Filmic shows middle gray as
  middle gray, so ACES needs about +0.8 EV for the same brightness; the samples moved from ACES at +1 EV to Filmic at 0
  (the Physics Playground -0.25 under the Clear preset's sun).
- The vignette is a lens's fall-off applied before the tone curve (default 0.12, which darkens a 60 degree 16:9 view's
  corners by 9.9%); stored values keep their number, now as a share of a lens's full fall-off (0.2 darkens those corners
  by 16.5% where it darkened them 20%).
- Bloom reaches the same share of the image at every resolution: 1080 rows keep 0.6.0's six levels, 2160 rows use seven
  (it was half as wide at 3840x2160).
- Automatic exposure in stored scenes changes too: they meter center-weighted, adapt to brighter scenes twice as fast,
  and the key darkens what meters below the directional light's gray card. For 0.6.0's exposure set
  **Auto Exposure Metering** to Average, **Auto Exposure Key** to 0 and **Auto Exposure Light Speed** to the scene's
  **Auto Exposure Speed**. Strata's interiors darken with the key only as far as their light is darker: sky light still
  reaches into closed rooms.
- Thumbnails are made again (their cache's version is 10: the sun's shadows, the dither, Filmic for scenes without a Post
  Process, the lens vignette, Filmic's shadows, and light-and-materials' light).
- Engine code: `RenderData::TonemapConstants` replaces the tone mapping's push constants (a constant buffer, 176 bytes);
  `RenderPostHDR` and `RenderPostDisplay` take the frame's post source (`SceneRenderer::PostSource`) and report degraded
  steps; `Renderer/PostProcessMath.h` holds the CPU twins of the post shaders' math.

**Smaller changes and fixes**

- A frame whose morphological anti-aliasing, lens flare or color lookup could not get its resources is drawn without it
  and logs the failure once instead of every frame; a frame whose tone mapping cannot run renders nothing instead of a
  stale image.

## light-and-materials

**What's new**

- Lighting debug views: `lighting` (every surface white, lit as the scene lights it), `ao`, `indirect`, `luminance`
  (EV100 bands) and `albedo` (base colors, those below 0.03 and above 0.85 marked), in the viewport's debug menu,
  `viewport.setSettings {debugView}` and `viewport.capture {debugView}`, whose `debugView.lighting` reports the captured
  frame's EV100 percentiles, albedo shares and sun to sky ratio on flat ground.
- Sun and sky presets: Clear, Hazy, Overcast and Golden Hour, buttons in the Sky Light's Inspector section and
  `lighting.applySkyPreset`, each a sky and a sun of real values in one undo step.
- Aerial perspective: under the atmosphere sky the air between the camera and distant surfaces dims them and adds its
  light, as the sky does (Sky Light **Aerial Perspective**, on, and **Aerial Perspective Scale**, 1; GPU stage
  `GPU.Aerial`, 512 KiB per view).
- Ambient occlusion denoises its fixed pattern within the frame: the same image with TAA, FXAA or no anti-aliasing, and
  still from frame to frame. **Ambient Occlusion Multi Bounce** (Post Process, on) keeps bright creases light. Graphics
  Quality gains Ambient Occlusion Quality (Low 1x4, Medium 2x6 as 0.6.0, High 3x8, Ultra 4x12 slices x steps).
- Highlights hold at 4K: specular anti-aliasing widens a highlight narrower than a pixel over the pixels it covers instead
  of sparkling, and reflections a normal map turns into the surface fade out.
- Graphics Quality gains Environment Resolution (the prefiltered sky: 256 per face at Low and Medium, 512 at High and
  Ultra) and Reflections (screen-space reflections: none at Low; Medium's half-resolution trace, which needs TAA; at High
  and Ultra every pixel traced at full resolution with a spatial resolve, with any anti-aliasing).
- glTF import setting **Base Color Factors Are sRGB**, for exporters that write sRGB colors into glTF's linear factors
  (Kenney's kits).

**Upgrading**

- Rough metals keep their light (multiple-scattering compensation): a metal of roughness 1 reflects up to three times
  what it did in 0.6.0, and dielectrics' specular grows about 9%.
- New projects (Basic 3D) open under the physical atmosphere with a 120,000 lux sun 40 degrees up and automatic exposure.
  New procedural skies are four times darker than 0.6.0's (0.25 times its colors), for a real sun to sky ratio (about 8
  with the default sun 45 degrees up); saved scenes keep their skies.
- Ambient occlusion's default radius is 1 m (0.6 before) for new Post Process components and files without the key; every
  Post Process gains multi-bounce (turn **Ambient Occlusion Multi Bounce** off to keep 0.6.0's darker creases).
- Every scene with the atmosphere sky gains aerial perspective: turn **Aerial Perspective** off on its Sky Light to keep
  0.6.0's look.
- At High and Ultra (the default is High) the prefiltered sky is 512 per face, 12 MiB more video memory per view (24 MiB
  with the atmosphere), and scenes whose Post Process turns on Screen Space Reflections trace them on every pixel: about
  232 MiB per view at 3840x2160 instead of 79 MiB. At Low those reflections are not drawn. Choose Medium to keep 0.6.0's
  costs.
- `lighting.applySkyPreset` turns a manual exposure automatic (a real sun clips to white under a manual exposure of 0),
  in its undo step.
- Every glTF model re-imports once (importer version 11), and asset thumbnails are made again (their cache's version is
  10).
- The samples: the Physics Playground uses the Clear preset, the Open World's sun is white and its Kenney models' colors
  are read as sRGB (teal-green leaves instead of a pale cyan wash), and every 3D sample's ambient occlusion radius is 1 m.
- Engine code: `SceneRenderOptions` gains `LightingView` and `LightingColors`; `SceneRenderer::GetLightingViewStats`
  returns a view's statistics. `RendererQualitySettings` gains `AmbientOcclusionQuality` (`AmbientOcclusionLevel`),
  `EnvironmentResolution` and `Reflections`.

## physics-runtime

**What's new**

- Physics steps cost less: Strata's own part of a step of 70,000 falling boxes went from 10.4 to 6.0 ms, of 30,000 with
  tracked contacts from 4.3 to 3.1 ms, of 10,000 from 0.61 to 0.44 ms (interleaved runs on one machine; 1,000 walking
  characters unchanged at 0.92 ms). The bodies' poses are written back and synchronized on the job threads, and contacts
  nobody listens to are no longer looked up.
- The project's Physics.Simulation settings (Project Settings > Physics > Simulation) tune how bodies settle: the contact
  recycle distance (box3d's reuse of resting contacts, 5 cm by default, kept by measurement), the sleep threshold, and the
  contacts' stiffness, damping and push speed. A running game takes a change at its next fixed step.
- `physics.stats` reports the step's parts and box3d's stages, sleeping bodies and islands, contacts touching, awake and
  recycled, how bodies settle, the tuning in effect and the stiffness box3d uses; `physics.setDetailedStats` (and the
  Streaming panel's Count contacts check box) has every step count contacts; `editor.status` shows the running game's
  bodies, awake bodies and step time, and the runtime's perf log has physics step columns.
- A streamed cell unloads without removing its static bodies from box3d in that frame: they collide with nothing at once
  and leave box3d a second later, in slices, unless they come back first.

**Upgrading**

- Bodies fall asleep below 0.08 m/s instead of box3d's 0.05: piles of balls sleep a quarter to a third sooner, and slow
  bodies (a ball rolling to a halt) stop a little sooner. Set Physics.Simulation's Sleep threshold to 0.05 for the old
  behavior.
- Character controllers and kinematic bodies (moving platforms, doors) no longer reuse contacts, so what they carry or
  push meets them where they are (boxes on a turning platform crept 0.70 m in 4 s before). Contacts with them are computed
  every step.
- A static body that leaves the simulation without contacts stays in box3d's trees, colliding with nothing, for 60 fixed
  steps before box3d disables it (`PhysicsStats::ParkedBodyCount`, `physics.stats` `bodies.parked`).

**Smaller changes and fixes**

- Creating and loading many bodies still costs what box3d's one-at-a-time insertion costs (10,000 bodies about 19 ms, 10
  of them box3d's); recorded, not changed in this release.

## editor-perf

**What's new**

- The editor's Performance panel costs a frame less than half what it did in 0.6.0 (and less than in 0.5.0): its
  statistics, table text and timeline no longer grow with every registered GPU or module stage.
- The Content Browser's first thumbnails no longer hitch the editor (the longest frame while rendering them went from
  about 2.4 ms to about 1 ms on the reference machine). Small scenes hand their GPU buffers to the next one, and
  thumbnails and Asset previews draw through the CPU path, with the same pictures.
- Every frame saves a queue submission: the GPU profiler resets a frame's timestamps on the host when it opens the
  frame's slot.
- Editing and playing scenes without mesh renderers (empty entities, lights, cameras) costs about a sixth less CPU per
  editor frame than in 0.6.0 (100,000 nested entities: 0.24 ms against 0.29 ms): the GPU-driven renderer no longer
  records culling, occlusion or feedback passes for a scene that has nothing to draw.
- Opening a large scene in the editor rarely freezes frames for 4-11 ms any more: the parallel read of a scene's file
  runs below the editor's frame, so the graphics driver gets a processor, and the scene's GPU copy is made a frame
  before it is shown (the longest frame while opening 100,000 entities over a million went from about 4.2 ms to about
  1.2 ms on the reference machine, median of opens). Opening takes as long as before.

**Upgrading**

- `renderStats.gpuDriven` (`editor.perf`, `gameRenderStats`, `viewport.capture`) means the frame took the GPU-driven
  path; the new `gpuCulled` says the GPU culled the scene's mesh renderers (false in a scene without any, whose frames
  take their asset requests from the CPU, `feedbackRequests` false).
- Engine code: `SceneRendererSpecification::ForThumbnails()` sets `GPUDriven` Off; `SceneRendererStats::GPUCulled`;
  `JobSystem::SubmitBackgroundPart` queues parts of background work in progress ahead of work not started; a scene
  asset's load reads at `SceneReadOptions::Priority` `JobPriority::Background`; `RenderSceneGPUCache` hands the buffers
  of a released mirror of at most `c_RecycleBytes` (1 MiB) to the next mirror, under their buffer version (binding sets
  compare buffer versions, never mirrors or scene ids).

**Smaller changes and fixes**

- A released scene's GPU copy releases its light records' buffer with its other buffers, after the frames that read it.
