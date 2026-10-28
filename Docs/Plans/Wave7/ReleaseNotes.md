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
  the sun shadows' 24-tap filter and contact shadows). Choose Medium to keep 0.6.0's sample counts.
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
- Thumbnails are made again (their cache's version is 5).
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
