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
  230 MiB per view at 3840x2160 instead of 79 MiB. At Low those reflections are not drawn. Choose Medium to keep 0.6.0's
  costs.
- `lighting.applySkyPreset` turns a manual exposure automatic (a real sun clips to white under a manual exposure of 0),
  in its undo step.
- Every glTF model re-imports once (importer version 11), and asset thumbnails are made again (their cache's version
  changed).
- The samples: the Physics Playground uses the Clear preset, the Open World's sun is white and its Kenney models' colors
  are read as sRGB (teal-green leaves instead of a pale cyan wash), and every 3D sample's ambient occlusion radius is 1 m.
- Engine code: `SceneRenderOptions` gains `LightingView` and `LightingColors`; `SceneRenderer::GetLightingViewStats`
  returns a view's statistics. `RendererQualitySettings` gains `AmbientOcclusionQuality` (`AmbientOcclusionLevel`),
  `EnvironmentResolution` and `Reflections`.
