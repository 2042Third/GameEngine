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
