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
