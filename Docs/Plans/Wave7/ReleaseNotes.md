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

## sun-shadows

**What's new**

- A `shadows` debug view (the viewport's debug menu, `viewport.setSettings {debugView: "shadows"}` and
  `viewport.capture {debugView: "shadows"}`) colors every surface by how many screen pixels one texel of the sun's shadow
  cascades spans there, outlines the cascades and hatches their blend bands; captures report each cascade's slice, texel
  size, share of the image and pixels per texel.
- Physical soft shadows: the sun's penumbrae are as wide as a sun of the light's Shadow Softness makes them (it now
  defaults to 0.53 degrees, the real sun's; stored values are kept), sharp at contact, without grain and identical from
  frame to frame with or without TAA; thin casters such as wires and grass keep their shadows.
- Graphics Quality gains Shadow Filter Quality (8, 16, 24 or 32 taps for wide penumbrae; Medium keeps 16).
- Opaque surfaces take the sun's shadow from a screen-space mask drawn after the depth prepass, whose edge-aware blur
  removes the soft penumbrae's grain in the frame (no TAA needed) without letting light cross a silhouette; transparent
  surfaces keep a per-pixel filter, without the blur. New GPU stage `GPU.ScreenShadows`.

**Upgrading**

- Every scene's sun shadows change: penumbrae half as wide as before for the same Shadow Softness, narrow edges filtered
  by a fixed tent instead of rotated noise. Set Shadow Softness to 2 times its old value to keep a scene's old width.
- Thumbnails are made again (their cache's version is 5).
