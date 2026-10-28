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
  per view with a shadowed sun instead of 64 MiB (below 1440 rows High is Medium). Choose Medium to keep 0.6.0's costs.
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
  (Open World +0.25).
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
- Thumbnails are made again (their cache's version is 8: the dither, Filmic for scenes without a Post Process, the lens
  vignette, Filmic's shadows).
- Engine code: `RenderData::TonemapConstants` replaces the tone mapping's push constants (a constant buffer, 176 bytes);
  `RenderPostHDR` and `RenderPostDisplay` take the frame's post source (`SceneRenderer::PostSource`) and report degraded
  steps; `Renderer/PostProcessMath.h` holds the CPU twins of the post shaders' math.

**Smaller changes and fixes**

- A frame whose morphological anti-aliasing, lens flare or color lookup could not get its resources is drawn without it
  and logs the failure once instead of every frame; a frame whose tone mapping cannot run renders nothing instead of a
  stale image.
