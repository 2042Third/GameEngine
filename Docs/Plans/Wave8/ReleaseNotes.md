# Wave 8: release notes for 0.8.0

What wave 8 brings, as step 0's lanes and the streams land on main (`plan.md`, `specs/`). `Docs/Releases/0.8.0.md` is
written from this file when 0.8.0 is released. Each landing appends a block of its own under each heading it has lines
for, its lane or stream named first in bold, so that merges only add. In the defaults table a landing's rows name it in
the Where column, as plan section 6 does; in the upgrade table its block starts with a row that names it in bold.

## What's new

**Platform: the project's fixed rate (S0.9).**

- Project settings, Physics > Timing: **Fixed Rate** (Scene, 30, 60, 120, 240) sets every scene's fixed step, in the
  editor and in the exported game; with a rate an update runs at most 2/15 s of steps (8 at 60, 32 at 240), so 240 steps
  per second keeps every step of frames up to 133 ms. The default, Scene, keeps each scene's own. At 30 contacts are
  softer (box3d's stiffness ceiling is 15 Hz there), which the field's description says.
- `scene.info` and `scene.setSettings` report `settings.fixedRate` (the step the scene runs, its most steps per update
  and whether the project or the scene sets it); the Inspector shows the scene's step rows inactive while the project
  sets the rate, and links its page.
- Scripts: `Time::GetFixedDeltaTime` returns the step the scene runs: the project's rate when it sets one.
- Fixed: a project opened without a start scene, or whose start scene could not be opened, left the editor's blank
  scene with the default collision layers (since 0.6.0) until a project setting changed; it takes the project's layers
  and fixed rate as the project opens.

**Lane Editor (S0.7: placing assets).**

- Editor: drag a prefab, model or mesh from the Content Browser into the Scene view: a ghost box shows where it will land,
  dimmed where nothing is under the pointer (then it lands on the grid, or in front of the camera). A material shows the
  mesh it would go to. Snap applies, Ctrl inverts it, Escape cancels. Assets that cannot be dropped say so at the pointer.
- AI agents: `scene.raycast` (what a ray or a Scene view pixel hits) and `asset.place` (place a prefab, model or mesh
  where it lands, in one undo step), in the `scene` toolset.

**Lane Editor (S0.4, S0.5: the frame rate and the Performance pause).**

- Editor: File > Preferences > Frame Rate: how long until the editor idles (0: never), how fast it redraws while idle, in
  front and in the background, and an optional cap while it is busy (240, 165, 144, 120, 90 or 60). The defaults are
  0.7.0's.
- Editor: frame-rate caps hold on Windows: the frame pacer waits on a high-resolution timer, where it waited for the
  system's 15.6 ms tick (a 120 Hz cap ran 20% slow, a 60 Hz one alternated long and short frames).
- Editor: Performance panel: Pause (or Space) holds the frame breakdown still so its frames can be read; the profiler,
  `editor.perf` and the status bar keep measuring.
- AI agents: `editor.perf` reports frame intervals (`interval`), `editor.status` the frame rate preferences.

**Lane Render**

- Contact shadows are off in every Graphics Quality preset: they shimmered while the camera moved. The sun's cascades
  ground small objects near the camera.
- The sun's shadows no longer jump while something passes close to the camera (a ball thrown from it, a bird): see
  **Sun shadows (stable cascades)**, which replaced this lane's waits.

**Platform: GLFW sealed behind the window (S0.8).**

- GLFW is sealed behind the window: only `Platform/GLFW` includes it, which the layer table's `ExternalOwners` enforces
  with Vulkan's and the OS headers; the Vulkan device presents through `Window::GetVulkanPresentation`.
- A window that cannot be opened ends the program through the fatal-error path (a dialog for a person, exit code 1)
  instead of aborting.
- `-DSTRATA_WITH_GLFW=OFF` builds the engine without a window backend: its programs run headless only. CI builds it.
- Tests drive the frame loop through `Tests::StubWindow`, a window without a display; `ApplicationSpecification::WindowFactory`
  lets a program or a test give the application its window.

**Platform: what paces the editor (S0.6).**

- The swapchain's log line names the present mode chosen and the modes the surface offers (`Swapchain: 2400 x 1350,
  3 images, vsync (FIFO; the surface offers FIFO, MAILBOX, ...)`).
- Measured: with vsync on, FIFO holds a windowed editor at the display's rate (120 frames a second on the reference
  machine's 120 Hz display). It first seemed not to (1,200-1,500 frames a second, every image handed back at once):
  NVIDIA Control Panel's "Vertical sync" was forcing vsync off, which overrides what an application asks for. Set to
  "Use the 3D application setting", FIFO paces. Borderless and fullscreen windows were not measured. A lower cap is
  File > Preferences > Frame Rate > Active frame rate (Unlimited by default).

**Slang (S0.10)**

- `Tools/LookDev.py compare` writes a signed difference image for every picture that differs (red where the new build
  is brighter, blue where it is darker) and a per-mode summary, so a change that shifts a region one way stands out from
  rounding; `selftest` runs its offline checks without an editor (`StrataTools.LookDevOffline`, on every CI job).
- Shaders are written in Slang with HLSL syntax and compiled by slangc 2026.20, still to SPIR-V for Vulkan and MoltenVK:
  the step toward other platforms' shader compilers. Pictures differ from 0.7.0 by rounding only (at most a few levels on
  edge pixels; with TAA, small differences spread through the history). Every shader also compiles to HLSL in a test on
  every CI job, CI validates every embedded module with spirv-val and converts it to Metal, and new shaders follow the
  rules of `Docs/DeveloperGuide.md` ("Shaders").

**Reflections (R1)**

- Screen-space reflections at High and Ultra no longer show a fixed screen-door pattern on rough metal: neighboring
  pixels' rays are averaged over 5x5 with weights that are the same over a plane at any angle, which cancels the march's
  offsets exactly (about two pixels softer on mirrors at 3840x2160).
- Reflections fade only where the reflected ray would leave the screen, not within 10% of every side: no more sky
  wedges at the sides of wet ground or hard cuts across a metal sheet (Medium too).
- Screen-space reflections at High and Ultra keep only rays that really crossed a surface: no ghost columns beside thin
  objects in front of a grazing mirror or metal sheet, and the reflected top edges of walls are whole instead of stepping
  into the sky. A ray that passes behind something goes on to what lies beyond it, out to its far end: a far wall seen
  past thin posts in a rough metal sheet shows no sky-coloured columns where 0.7.0 drew ghost copies of the posts.
- Surfaces without a normal map are exactly flat: their shading normal was tilted by 0.3 degrees, which moved mirror
  images (a wall 10 m away lost the top 8 cm of its reflection in a polished floor).
- Ambient occlusion no longer darkens what screen-space reflections found: no dark smudges beside objects on wet ground
  (Medium too). A material's occlusion map still darkens them.
- Reflection probe bakes draw no sun disk into their captures: the sun lights the scene; a disk in a probe lit surfaces
  again, unshadowed and in the wrong place. Nor do they hold the lights' highlights: a polished tank's sun streak, seen
  from the probe's position, showed on wet ground as a bright patch in the wrong place.
- Look-dev: the specular lab (`Scenes/Specular.stscene`, shots `lookdev.sheet`, `.wet`, `.tank` and more) and shots'
  `Metrics` (noise, screen-locked pattern, bands) in `metrics.csv`; tests measure flicker in motion with one meter for the
  wave (`Tests::MeasureFlicker`).

**physics-timing: smooth at any frame rate**

- Games and the editor's Play and Simulate draw bodies, characters and what scripts move in fixed updates between their
  last two physics steps, so motion is smooth at any frame rate: the Third-Person robot running across the view moved by
  up to 29.4 px (peak to peak, on a 4K screen) at 700 fps; now by 0.25 px at 731 fps and at most 0.37 px at 30, 60, 120,
  144, 240 and 400-730 frames per second and at 30, 60, 120 and 240 steps per second. The simulation is unchanged. What
  you see is a constant step behind the game's clock, about half a step (8 ms at 60 steps per second) more on average
  than before and no more at worst; Project Settings > Physics > Timing > Interpolation turns it off. Its cost is that
  moving shadow casters redraw their shadows on every frame instead of every step: with about 2,000 balls rolling in the
  Physics Playground the editor's Game view ran at 356 instead of 501 fps (GPU 2.4 instead of 1.6 ms a frame, on an
  RTX 5090); with interpolation off it costs nothing measurable.
- New script API: `Time::GetStepAlpha`, `TransformComponent::GetDrawnPosition`, `GetDrawnRotation`, `ResetInterpolation`.
- The Third-Person camera follows the robot as it is drawn and lets its arm out smoothly after something passed between
  them.
- `play.state` and `physics.stats` report interpolation.

**Reflections (R2)**

- The sun's highlight takes the light's size (Directional Light **Angular Diameter**, stored as `ShadowSoftness`):
  Overcast's 10-degree sun gives a soft glow instead of a pin dot; on mirror-smooth surfaces the midday sun's glint is the
  disk's size. The property now sits with the light's own settings and applies without shadows too.
- Point and spot lights have a **Source Radius** (default 0, as before): a lantern's or a fire's highlight on metal and wet
  ground takes the bulb's or flame's size, the same light spread wider; their diffuse light and shadows do not change.
- A material's IOR sets its reflectance: water (1.33) reflects 2%, glass (1.5) 4% as before; imported `KHR_materials_ior`
  values count, without re-importing.
- The physical atmosphere's sky after sunset no longer shows bands, nor do its reflections: its tables keep the twilight's
  faint light at full precision (daytime skies are unchanged to the bit).
- At High and Ultra reflection probes are 256 texels per face: sharper on polished metal, 68 MiB of GPU memory per view that
  shows probes instead of 17 (Graphics Quality's **Reflection probe resolution**). No probe needs baking again.
- Asset thumbnails render again once (the sun's highlight and dielectrics' reflectance changed how materials look).

**showcase**

- A fifth sample, High Desert Outpost (`Samples/Showcase`, first in the launcher): a derelict fuel stop in a rocky
  desert valley the day after rain, built from 22 CC0 Poly Haven scans and generated props, made to show the low sun's
  highlights on sheet steel, a weathered oil tank and wet asphalt.
- Two generators keep its ground and props reproducible: `Tools/GenerateShowcaseTerrain.py` (the valley, the apron with
  its potholes and puddles, their water and mud; it checks that the golden sun clears the hills and computes where each
  shot sees the sun reflected) and `Tools/GenerateShowcaseProps.py` (the bus, the tanks, the pipes, the hut, the bare
  steel's maps, and the command scripts that make the sample's materials and prefabs). The CTests
  `StrataTools.ShowcaseTerrain` and `StrataTools.ShowcaseProps` keep the committed files equal to what they write.
- Play it and a camera tour of about 80 seconds shows six shots: dawn over the valley, the low sun sliding along the
  bus's side and down the oil tank, its glint in the puddles, the hut's interior and dusk with the fire and lanterns lit.
  Each time of day has its own baked reflection probes, switched on under the fade between shots.
- Any key takes the tour over into a first-person walk; Tab flies, P opens a photo mode (time of day, exposure, field of
  view, focus, f-number, depth of field, the quality preset and anti-aliasing, seven bookmarks), H hides the UI.
- Samples: a showcase may hold six screenshots in 8 MB (the others three in 3 MB).
- Manual, Lighting: "Times of day with baked probes".

**Global illumination (stage 1: the voxels).**

- Editor: the Scene view's debug view **Voxels** shows the scene as global illumination will see it: a voxel copy of
  everything around the camera (10 cm voxels near it, coarser further out), kept up to date as the camera moves and the
  scene changes, each surface in its albedo; walls thinner than two voxels show in the error color (they will get no bounce).
  A textured surface shows its texture's average color, so models colored from one shared palette texture show that
  palette's average. It costs nothing unless the view is on (about 58 MiB of GPU memory while it is).
- AI agents: `viewport.capture {debugView: "voxels"}` reports `debugView.globalIllumination` (levels, voxel sizes, whether
  each holds the scene, memory) and waits for the voxels to settle; every capture reports `globalIlluminationSettled`;
  `editor.perf` names the GPU stage `GPU.RadianceCascades`.

**Global illumination (stage 2: the light, behind debug views).**

- Editor: the Scene view's debug views **GI** (the bounce light on white surfaces), **Open** (the share of the sky each surface
  sees) and **Probes** (the light probes, dead ones red) show world-space radiance cascades: the sun lights the voxel copy
  of the scene every frame, and probes in the world gather it, so a sunlit floor lights the walls whether or not it is on
  screen, and moving the sun moves the bounce in the same frame. The lit image does not use it yet (stage 3). The
  **Voxels** view now shows the light each voxel holds: the sunlit patch bright, surfaces no light reaches dimmed.
- AI agents: `viewport.capture` and `viewport.setSettings` take `debugView` `"gi"`, `"open"` and `"probes"`; the capture's
  `debugView.globalIllumination` adds the probes' spacing, grid, cascades, directions and live and dead counts, and
  `"gi"` and `"open"` captures report `debugView.lighting` as the other lighting views do.

**Global illumination (stage 3: in the lit image; experimental).**

- Scenes: the Post Process's **Global Illumination** (off by default) puts the bounce light into the image: a sunlit floor
  lights the walls and the ceiling, a red floor turns them red (in the sun room test the walls' red-to-green ratio rises
  by 108% of a one-bounce reference's rise, none with it off), glowing surfaces light their surroundings and closed rooms
  stay dark, also for alpha-blended and glass surfaces. **Global Illumination Intensity** (1, 0-4) scales it. Scenes that
  do not turn it on, and every sample and the showcase, look exactly as before.
- Graphics Quality: a new row **Global illumination** (Off, Low, High; Off at Low and Medium, High at High and Ultra): 23
  MiB of GPU memory per view at Low, 153 MiB at High, only in scenes that turn it on; the Graphics Quality page's memory
  rows count it as they count the reflections' (High at 1920 x 1080: 702 MiB, 153 of them global illumination's). The
  `Renderer.Quality` section's version is 6.
- Captures and the project's picture wait for global illumination to settle.
- AI agents: `component.set` on `PostProcess` takes `GlobalIllumination` and `GlobalIlluminationIntensity`;
  `project.setSettings` and the players' graphics options take `Renderer.Quality.GlobalIllumination`.

**Global illumination: every bounce (stage 4; experimental).**

- Scenes with **Global Illumination** on at High: light now bounces on between surfaces instead of stopping after its
  first bounce. A room lit by a panel in its ceiling has a lit ceiling (in the owner's Cornell box, a 5,000-nit panel's
  room, the ceiling rises from 13-22 to 89-142 nits per unit of albedo at High), colored walls tint the floor and
  objects beside them (the floor by the red wall turns 9% redder than by the green one, the sphere's sides 26%), and a
  sunlit room's floor, which only the window's sill lit before, takes 40-230 nits of the walls' and ceiling's light.
  Each further bounce arrives a frame after the one before: after a cut or a change of the light, rooms fill over a
  number of frames set by their brightest surface (12 for the sun room's plaster, 17 for the Cornell box's white, at
  most 38), and captures and the project's picture wait for it. A sealed room stays as dark as with one bounce (its
  points' mean and every junction within 0.17% of the open room's light). Low bounces once. Look-dev: the Cornell box,
  `lookdev.cornell`.
- What the bounces cost: 0.06-0.10 ms of global illumination's 4 ms at 3840 x 2160 at High in the sun room (one bounce
  against every bounce in one run). Scenes without global illumination look exactly as before.

**Global illumination: smooth light between probes (experimental).**

- The bounce light no longer switches probes on and off: each probe keeps its distance to the first surface in every
  direction, and a surface weighs the probes around it by how surely they see it, so the light changes smoothly across
  walls, floors and ceilings. In the owner's Cornell box at High, the bands at probe-cell borders on the walls are gone
  (the largest step across a probe plane fell from 25% to 8% of the light), the black staircase around the sphere's foot
  and the blocky rectangle of its indirect shadow became a soft shadow, and the dark square at the tall box's foot is gone
  (batch 2 below darkens the floor along that box's left face again, softly: Known issues). The room's concave corners
  keep their light up to the junction. Closed rooms stay dark. Probes that sat in a wall move out of it into the room.
  Look-dev: `lookdev.cornell`, `lookdev.sunroom`.
- What it costs at 3840 x 2160 at High: about 0.3 ms more in global illumination's own stage (4.3 ms in the sun room and
  the yard) and 0.25-0.3 ms more in the opaque pass; 35 MiB more memory per view at High (4.4 MiB at Low).

**Global illumination: light at contacts (experimental).**

- A probe that falls inside an object or a wall moves into the open space of its cell, and a surface weighs the probes
  around it by whether the voxels between them leave it in sight, no longer by each probe's blurred distances. In the
  owner's Cornell box at High, the large black, ragged blotch under the sphere and the black slot at the tall box's foot are
  gone: the darkest floor point under the sphere against its neighbour 0.1 m away went from 0.021 to 0.083 of its light, at
  the tall box's foot from 0.10 to 0.23 (0.22 since batch 2), and the corners keep at least 0.58 (0.53 before, and 0.53
  again since batch 2's first item). A row of small hard-edged notches remains at the sphere's foot (Known issues). Closed
  rooms stay dark. Look-dev: `lookdev.cornell` (lit and GI view).
- A probe's move is a function of the world's voxels: as the camera walks, probes glide where the voxel levels hand over
  (a 2 cm step moves one at most 0.021 m) instead of jumping up to 0.35 m in one frame in a ring about 5.5 m away.
- What it costs at 3840 x 2160 at High: the probes' search takes 0.006 ms of global illumination's own stage, the opaque
  pass the same; no memory.
- Fixed: a camera strafe along a fence no longer makes its light crawl in the GI view: measured settled, 0.023% of the
  image's blocks change from frame to frame at 1280 x 720 and 0.022% at 3840 x 2160 (0.10% is the bound), their 99th
  percentile 0.44% and 0.34%, grass 0.025% and 0.024%; with TAA 0.023% and 0.022%, grass 0.023% and 0.024%. Before,
  from a fresh renderer, 0.247% and 7.2%: the probes' blurred distances, read toward a point moved toward the viewer,
  changed a thin post's light in steps as the view turned.

**Global illumination: batch 2 (experimental).**

- At High the finest voxels reach further down than up: their window lies 0.8 m below the camera, so floors and what
  rests on them keep 10 cm voxels down to about 3 m below the eye (ceilings more than about 1.5 m above it take 20 cm
  ones). In the owner's Cornell box seen from half its height, the outer ring of the floor the sphere overhangs (0.6 to
  0.9 m from its contact point) no longer falls to black against its neighbour: the darkest point there keeps 0.53 of
  its neighbour's light 0.1 m away (0.083 before), and in the lit picture at 3840 x 2160 none of that ring's 55,968
  pixels is left at a luma of 12 of 255 or under (298 before the batch). Nearer the contact nothing is gained: the ring
  0.3 to 0.6 m from it is darker than before the batch, smoothly (its mean green 24 of 255 against 34; 3,008 of its
  24,503 pixels at 12 or under against 1,435), and within 0.3 m the black notches are as they were (887 of 1,225 pixels
  at 6 or under, 892 before). There are two prices. That room's ceiling, 2.75 m above the camera, is in 20 cm voxels
  now: where the side walls meet it the darkest point keeps 0.53 and 0.64 of the light 0.1 m below (0.58 and 0.73
  before; the back wall there 0.92, 0.89 before). And floors beside what stands on them got darker (Known issues): in
  the Cornell box a soft dark patch lies on the floor along the tall box's left face, where the floor ran evenly up to
  the box before the batch (from the box's front corner 0.6 m back and 0.2 m wide the lit picture reads under half of
  what it read: 2,555 pixels, the darkest 0.26 of before, 92 to 24 of 255), and three points in one of ThirdPerson's
  ground folds read about half as bright (green 20-24 of 255 against 44-45). The review's captures of the batch's steps
  place both at this item; what in the finer voxels darkens them is not established. No memory; 0.12-0.15 ms more GPU
  time for a 3840 x 2160 frame in the sun room, 0.02 ms in the yard.
- Thin gaps that the voxels close no longer go black: where the probes around a surface cannot see it, it takes the
  light of the gap's opening as the camera sees it, darkened by how far in it lies. Under the yard's covered car (0.15 m
  off the ground) seen from 6 m, the ground 0.2-0.5 m under its edge keeps 7% of the open ground's light and 0.5-1 m in
  1.5% (it was black; seen from 4 m, where the finest voxels hold the gap, 6% and 0.5%; at Low the ground 0.5-1 m in
  keeps 1.6% from 4 m and 5% from 6 m). The gap's light is added to what the probes give and darkens no point: as first
  written it was blended in, which faded the probes' light twice where they see a surface in part (42 pixels of a 960 x
  540 view of the car from 6 m were darker with the gap's light than without it, the darkest at 0.18 of itself). It
  falls off smoothly under the object: as first written its depth was counted in steps of half a voxel and the light
  fell in bands, up to 1.5 times from one pixel to the next through a long lens (1.12 now). Closed rooms stay dark.
  In pictures the change is small. Captured before and after through a 25 degree lens at 3840 x 2160, the slot under
  the car stays nearly black in the lit picture from 6 m: 20-25 of 255 where it read 20 (the car's shadow on the open
  ground in front of it reads 44-66, sunlit ground 167); the GI view shows a faint gradient at the slot's near edge,
  1-13 of 255 where it read 0-2 (68-99 on the ground in front). From 4 m both pictures are what they were.
  0.06-0.08 ms of GPU time in the sun room, at most 0.03 ms in the yard.
- Round objects take their light more evenly: surfaces read each probe's light as spherical harmonics, which turn
  smoothly with the surface, where an ambient cube blended the light of the axes. A white sphere under a uniform sky now
  reads 3.5% dark at its rim (7% before) and 2% at its center. The bounces keep the cubes, so the light still settles to
  the same frame and a still camera's consecutive frames stay identical to the bit, with every bounce too (tested in the
  sun room and the Cornell box; two captures of one shot are another matter: Known issues). 6.3 MiB more memory per view
  at High (0.8 MiB at Low) and, as first written, 0.23-0.25 ms of GPU time, which the next item's two cuts make up for.
  The harmonics' first term is stored as the mean of the probe's light, not as the coefficient, which is 3.5 times the
  mean and left the texture's range in a bright closed room (a box glowing at 65,000 nits with every bounce: the test
  machine's driver wrote the format's largest value there, a first term 22% low, and a driver may write infinity). A
  surface never reads less than its probes' cubes hold for it: the harmonics weigh light from behind a surface's plane a
  little below zero, and alone they put dark to black blotches on a wall at and around a patch of sunlight on it (the
  sun room's north wall under a low sun read 0.48 to 0.74 of the cubes there, now 1.00 to 1.09; the back wall of the
  showcase's hut went black around the door's and the window's patches). Walls that stand at an angle to the world's
  axes glow softly around such a patch, as they did before this batch (Known issues).
- Summing the finest cascade's rays into the probes costs less: each ray's thread evaluates its harmonics once, where
  the nine threads that sum the coefficients evaluated them again for every ray, and the border of a probe's depth tile
  is copied from the tile's interior instead of being filtered a second time. 0.29-0.31 ms less GPU time for a 3840 x
  2160 frame in the sun room and 0.23-0.24 ms in the yard, every global illumination test reporting the numbers it
  reported before. In all, batch 2 costs that frame about 0.1 ms of GPU time: 0.05 ms more in the sun room and 0.09 ms
  in the yard than the build before it in three interleaved runs of each (8.96 against 8.92 ms and 8.38 against 8.29 ms;
  `GPU.RadianceCascades` 4.20 and 4.16 ms against 4.27 and 4.07, `GPU.Opaque` 1.07 and 0.84 ms against 1.00 and 0.75),
  0.12-0.15 and 0.04 ms as the sum of its separately measured steps. Without global illumination it costs nothing: a
  frame that does not read the field draws with forward shaders that hold nothing of it. (Compiled in behind a branch such
  a frame never takes, the field's gather had cost its opaque pass 6-11% at 3840 x 2160, 0.017-0.021 ms, in the review's
  interleaved runs.) In three interleaved pairs against the build before batch 2, the look-dev project's shots without
  global illumination at 3840 x 2160 spend 0.94-1.02 of that build's time in `GPU.Opaque` at High (eight shots without
  anti-aliasing, six with TAA; medians 0.99 and 0.98; -0.010 to +0.007 ms) and 0.96-1.01 at Medium (medians 0.98; -0.008
  to +0.003 ms). Without anti-aliasing seven of the eight pictures are that build's to the bit in all three captures, the
  eighth (polished metal) in two of three, with 3 pixels one level apart in the third; with TAA all six are, in all three.
- A camera that stands still gets the same probes every frame, to the bit. The pass that sums the finest cascade's rays
  now rounds every product, sum and quotient as written: left free, this machine's driver (NVIDIA 621.1) ran those sums
  in two ways from one frame to the next, a unit in the last place apart (a face of one probe, the harmonics of some
  180), which now and then failed the test that compares a strafe's field with a fresh renderer's when the machine was
  busy. That test, which now compares the harmonics too, and a new one over twelve frames of a still camera failed in 6
  of 24 runs of their suite without the rounding (the new one in all six, the strafe's in four) and in none of 24 with
  it, the two builds taking turns. Why the driver has two ways is not known. Look-dev's 15 global illumination pictures
  (the Cornell box and its GI view, the sun room, the yard and ThirdPerson's course, each without anti-aliasing, with
  FXAA and with TAA) change by one level in at most 245 pixels of a 3840 x 2160 frame without anti-aliasing or with
  FXAA. With TAA more pixels move: 4% of the yard's, all but 1,719 of them by one level, up or down, all over the
  picture (its automatic exposure ends 0.00001 EV apart), the rest by up to 41 on the fence and other thin edges; the
  pictures look the same, and a second capture of the new build equals the first in all 15 (captures of one shot do not
  always repeat: Known issues). No GPU time that six interleaved pairs of runs could measure: that frame's busy time
  read 8.76 against 8.78 ms in the sun room and 8.23 against 8.20 ms in the yard.

**Occlusion culling (cull fix)**

- Fixed: occlusion culling hid objects in plain view. Its depth pyramid was built a texel or more off where a level's
  size halves to an odd number (960 x 540, 1920 x 1080 and 3840 x 2160 among the sizes it happened at), so a box below a
  sealed window, seen straight on, vanished and showed the ground behind it. Each level now covers exactly the pixels the
  culling reads it for, its last row and column included.

**Sun shadows (stable cascades)**

- The sun's shadows are final in the first frame after the camera turns, moves or cuts: no more coarse shadows that
  sharpen over a second, and no shadows "going in and out of existence" while looking around. The cascades are the
  industry's stable cascaded shadow maps: fixed splits by distance (from 1.5 m to the Shadow Distance, shared by the
  light's Cascade Distribution: at 80 m the four end at 5.8, 14, 33 and 80 m), each the smallest sphere around its slice of
  the view, whose size a turn never changes, snapped to whole shadow texels as the camera moves, so shadows stay still on
  the ground. Nothing is read back from the GPU any more, so nothing in the scene (a thrown ball) moves them either.
- High and Ultra buy the detail back with larger shadow maps: High 4096 below 1440 rows and 8192 from them (was 2048 and
  4096), Ultra 8192 at every size (was 4096 below 1440 rows). Medium keeps 0.6.0's 2048 and costs.
- High and Ultra add a fifth cascade before the four, for what lies within 1.5 m of the camera (less with a wide lens:
  its sphere stays within 2 m, 0.49 mm texels at 8192), so close-ups keep fine shadows: the showcase's shot 5, which
  looks past a door post 0.3 m away, measures a median of 1.48 screen pixels per shadow texel (0.7.0 0.88) where four
  cascades gave 2.28, and none of its pixels above 4 (48.8% with four). The four keep their texels, so the grass
  blades' thin shadows 4-6 m away do not change. It costs 0.32 ms of GPU time where every cascade is drawn again (the
  100,000-cube 4K case: 2.81 ms against 2.49) and 256 MiB per view at 8192 texels.
- The soft tips of long shadows are the same width in every cascade: the penumbra's search distance follows the
  receiver's distance (5 m plus 0.3 of it) instead of each cascade's size, so no step shows where two cascades meet. The
  contact-hardening look (sharp at the caster, soft at the tip) is unchanged.
- `viewport.capture` and project thumbnails no longer wait for the cascades (no `shadowFitSettled`); `editor.perf` and
  `viewport.capture` drop `renderStats.shadowFit` and each cascade's `fitted`.

**CI (macOS 15)**

- Fixed: built on a Mac with Apple Clang 17 (clang-1700.0.13.5, the compiler of GitHub's macos-15 image), an Axis
  action bound to a whole gamepad axis (`Gamepad/LeftX`, for one) read 0 whenever the first gamepad held it to the
  negative side (left, down), while the positive side worked: the compiler dropped part of the comparison that picks
  the gamepad pushing an axis furthest. 0.7.0's packages, built with Apple Clang 15, are not affected, and Vector2
  actions on whole sticks never were.

## Defaults that change

Stored values never change: a scene or project that saved a value keeps it. These change what new components and
projects, the template and scenes or projects without the value get.

| What | 0.7.0 | 0.8.0 | Where |
| --- | --- | --- | --- |
| Contact shadow steps of the Graphics Quality presets | Low 0, Medium 0, High 12, Ultra 16 | 0 in every preset | lane Render |
| Compiled shaders | glslang | slangc; pictures differ by rounding only | S0.10 |
| First configure of a clone | offline after submodules | fetches Slang's archive once, or `STRATA_SLANG_ROOT` | S0.10 |
| What games, Play and Simulate draw | each body's last fixed pose | between its last two fixed poses, a step behind the clock | physics-timing |
| Reflection probe resolution of the High and Ultra presets | 128 | 256 | reflections R2 |
| A directional light's highlight | a point's | its Angular Diameter's disk (0.53 degrees by default) | reflections R2 |
| Sun shadow resolution of the High and Ultra presets | High 2048 / 4096 from 1440 rows, Ultra 4096 / 8192 | High 4096 / 8192 from 1440 rows, Ultra 8192 | stable cascades |
| Sun cascade splits | fitted to the receivers the GPU read back (0.6.0's from the near clip until then) | fixed: from 1.5 m to the shadow distance by Cascade Distribution | stable cascades |
| Sun shadow cascades of the High and Ultra presets | 4 | 5 (a near one within 1.5 m) | stable cascades |

## Upgrading from 0.7.0

| What | 0.7.0 | 0.8.0 | To keep the old |
| --- | --- | --- | --- |
| **Platform (S0.9)** | | | |
| `input.*` durations in `seconds` before play starts | Checked against 1/60 s | Checked against the edited scene's step (the project's rate when it sets one), the step play will run | Nothing to do: while playing they always counted in the scene's step |
| Code reading `SceneSettings::FixedTimestep` or `MaxFixedStepsPerFrame` for the step a scene runs | The scene's values | Still the scene's stored values, which a project rate overrides | Read `Scene::GetFixedTimestep` and `Scene::GetMaxFixedSteps` |
| **Lane Editor (S0.7)** | | | |
| A prefab dropped through Game camera | Under the editor camera's pivot | Under the pointer, as the scene's camera sees it | Turn Game camera off before dropping |
| A drop where nothing is under the pointer | Under the view's pivot | On the grid plane (y = 0, within 500 m), else in front of the camera | Use Add to Scene, which places under the pivot |
| A prefab with several top-level entities | Only the first moved to the drop | All move together, keeping their offsets | None needed: the template's layout is kept |
| Dropping an asset the view cannot take | A Console warning | A label at the pointer, nothing logged | None |
| **Lane Editor (S0.4)** | | | |
| The editor's idle throttling | Constants: idle after 0.5 s, 30 frames per second, 10 in the background | The `Editor.FrameRate` preferences, with those defaults | Nothing to do |
| **Lane Render** | | | |
| Contact shadows | drawn at High (12 steps) and Ultra (16) | off in every preset | Graphics Quality Custom, Contact Shadows 12 (or 16) |
| A Custom quality saved before contact shadows existed (a development build of wave 7) | read High's 12 steps | reads 0 steps | Graphics Quality Custom, Contact Shadows 12 |
| The cascades' near end | follows any near object above 1% of the view at once | waits 0.5 s (0.12 s moving) below 10% of the view; transparent surfaces and narrow views at once | nothing to keep: settled views are unchanged |
| **Platform (S0.8)** | | | |
| `Window::GetNativeWindow` | The `void*` the Vulkan device made its surface from | Gone | A `Window` of your own drops its override; code that needed the OS window uses `GetPlatformHandle`, code that presents through Vulkan `GetVulkanPresentation` |
| `Window::Create` when no window can be opened | Aborted (`ST_CORE_VERIFY`) | Returns null, with the reason logged | Check the result |
| **Slang (S0.10)** | | | |
| The glslang submodule | `Strata/vendor/glslang` | gone: after pulling, the folder (about 900 MB, which git leaves) may be deleted | Keep the folder while you build commits from before 0.8.0 |
| The shader compiler | glslang, built with the engine | Slang's release archive (25-64 MB), downloaded by the first configure into a per-user cache (`%LOCALAPPDATA%\Strata\Toolchains`, `~/.cache/strata/toolchains`, `~/Library/Caches/Strata/Toolchains`) and checked by its SHA-256 | Offline: download the archive `CMake/StrataSlang.cmake` names on another machine, extract it and configure with `-DSTRATA_SLANG_ROOT=<folder>` |
| **Reflections (R1)** | | | |
| Reflection probes baked under the atmosphere | Their capture holds the sun's disk (clamped to 65,000 nits) | Bakes draw no disk | Bake them again (Bake Reflection Probes, or `lighting.bakeProbes`) to lose the bright patch; nothing to keep |
| Reflection probes of lit shiny things | Their capture holds the lights' highlights as seen from the probe | Bakes keep the lights' diffuse light only | Bake them again to lose highlights in the wrong places; nothing to keep |
| Spatial reflections' resolve | 3x3 neighbors, depth-weighted | 5x5 neighbors, plane-weighted: no screen door, about two pixels softer on mirrors at 4K | None |
| **physics-timing** | | | |
| Scripts that place something from a moving body's position every frame (a follow camera, an effect) | the body's last fixed pose, which is what was drawn | the body is drawn up to a step behind its last fixed pose, so it seems to jitter against them | read `GetDrawnPosition()`/`GetDrawnRotation()` in `OnLateUpdate` instead of the world position |
| Scripts that move an entity far by setting its transform in `OnFixedUpdate` | drawn there at the step | drawn sliding there over a step if within 400 m/s | call `GetTransform().ResetInterpolation()` after the move |
| What games, Play and Simulate draw | each body's last fixed pose | between its last two fixed poses | Project Settings > Physics > Timing > Interpolation off |
| **Reflections (R2)** | | | |
| A directional light's highlight | A point's (the smallest roughness's pin dot) whatever its Shadow Softness | Its disk's size: mirrors 1.8 times wider at the default 0.53 degrees, roughness 0.06 30% wider, 0.18 0.4% | Angular Diameter 0 (also hardens its shadows) |
| Shaders reading the atmosphere's sky-view, multiple-scattering or aerial tables, or its environment cube | Light per lux of sun | Per lux times `Atmosphere::c_StorageScale` (16384) | Multiply by `EnvironmentParams.x`, which divides by it (or by `1.0 / ST_ATMOSPHERE_STORAGE_SCALE`) |
| Reflection probes' resolution at High and Ultra | 128 texels per face (17 MiB per view with probes) | 256 (68 MiB) | Graphics Quality Custom with **Reflection probe resolution** 128 |
| A non-metal's reflectance (F0) | 0.04 whatever its IOR | From its IOR: 1.5 (the default) exactly 0.04 as before, water 1.33 0.02, 2 0.11, 1 none | Set the IOR to 1.5 (it also bends what transmissive materials show) |
| Directional Light **Shadow Softness** | In the Shadows section, inactive without shadows | **Angular Diameter**, with the light's own settings, active with and without shadows; stored, scripted and set by commands as `ShadowSoftness` | Nothing to do |
| **Sun shadows (stable cascades)** | | | |
| `SceneRendererSpecification::FitShadowsToView`, `ShadowFitHolds`, `SceneRenderer::ResetShadowFit`, `ShadowFitTracker` (`Renderer/ShadowFit.h`) | The cascades' fit to the view | Removed: the cascades are the view's alone (`Renderer/ShadowCascades.h`) | Nothing to keep: code that turned the fit off for single pictures drops the line |
| `SceneRendererStats::ShadowFit*`, `ShadowCascadeStats::Fitted`; `renderStats.shadowFit`, `cascades[].fitted`, `viewport.capture`'s `shadowFitSettled` | The fit's state | Removed | Command scripts drop expectations of `shadowFitSettled` |
| Video memory of the sun's shadow maps at High | 256 MiB at 3840x2160 | 1 GiB at 3840x2160 (256 MiB at 1920x1080) | Graphics Quality Custom with Shadow Resolution 4096, or Medium |
| A light's Cascade Distribution | shared the receivers' range of distances once fitted | shares 1.5 m to the shadow distance (0.6.0 shared the near clip to it with 0.8) | 0.9 stays the default; scenes keep their value |

## Known issues

**Global illumination (experimental)**

The spec's gate for the light (Gate W) is not fully met; measured against a one-bounce reference in the sun room at High:

- The east wall high and the ceiling's east side get 0.63 and 0.49 of their bounce (8 of 10 wall and ceiling points within
  30%; mean error 20%), one floor-wall junction misses, and two points behind the camera 2 m up get 0.62-0.66.
- Low is too dark: 0.2-0.5 of the reference.
- Open ground under the sky reads 3.2% off at 6 m, and a white sphere under a uniform sky 2% dark at its center and 3.5% at
  its rim, or up to 10% where a probe lies within a voxel of it (3% is the goal: what is left lies in the probes' light
  beside the sphere, whose skimming rays the voxels absorb, not in how the probes store it).
- It costs about 4.2 ms of GPU time at 3840 x 2160 at High (`GPU.RadianceCascades`; 1-2 ms aimed at; the perf cases
  `PerfGPU.Renderer.GlobalIllumination4K*`: medians of 4.20 ms in the sun room and 4.16 ms in the yard), and the forward
  pass's gather adds 0.3-0.8 ms to `GPU.Opaque` (look-dev: the sun room, the yard and ThirdPerson's course). Of the
  stage, the march of the three coarser cascades takes 2.6 ms, summing the finest one's rays into the field about 1.1 ms
  (1.35 ms measured per pass, less the 0.23-0.31 ms batch 2's two cuts of it took off the frame; not measured per pass
  again), lighting the voxels 0.53 ms and their downsample 0.08 ms. What is left is the marches themselves, of which no
  cheap cut was found.
- In the lit image, a surface that the probes around it see little of gets little indirect light, darker than without
  global illumination: gaps, recesses and ledges thinner than about two voxels (20 cm at High, 40 cm at Low) and the bases
  of walls beside bright patches can go nearly black. A surface that none of its probes sees takes the thin gaps' light
  since batch 2 (the light at the gap's opening, darkened by its depth: a few percent of the open ground's), no longer
  none at all. Outdoors this shows on ThirdPerson's ground ledges and fence rails and in a strip along the base of the
  yard's pool box, sky-lit without it; indoors as a dark band about 0.3 m high where the floor meets the wall under the
  sun room's window. ThirdPerson's ground folds are darker since batch 2, not lighter (with the finest voxels' lower
  window, by the review's captures of the batch's steps): in the course's lit look-dev picture at 3840 x 2160, three
  points of one fold read (5,20,13), (5,23,16) and (6,24,17) of 255 where they read (15,45,33), (14,44,34) and
  (14,44,33) before the batch, and 2,451 pixels of the picture fell under half of their luma (none rose as far). With
  the sun high, the bounce on the window wall ends in sharp vertical edges either side of the patch's light.
- A wall that stands at an angle to the world's axes glows softly around a patch of sunlight on it, as it did before
  batch 2. A surface never reads less than its probes' ambient cubes hold for it, and between the axes a cube blends
  the light of the two or three axes the wall's normal lies between, some of which is the patch's own. On a test panel
  turned 30 degrees, the wall at the patch reads 2.2 to 2.7 times its shadowed part's indirect light, under sunlight
  hundreds of times as bright, and the shadow 0.75 m beside the patch 3% more than the harmonics alone give it (the
  harmonics alone read 0.79 to 1.17 at the patch and left the wall of a room that such patches alone light black around
  them: the showcase's hut). Walls along the axes read 1.00 to 1.09 of their cubes' faces. Six faces or nine
  coefficients a probe do not hold the light of every direction a wall can face: a read that is right at every angle
  needs more of them.
- Its light ends at the probe field's edge around the camera (High: full within 7.5 m horizontally, 5.5 m up and down;
  Low: 7 m and 2.8 m; faded to the light without it over 2 m): outdoors the edge of a bubble can show.
- The owner's Cornell box at High, with every bounce: the probes are 0.5 m apart (1 m at Low), and the light still
  falls faster than it should where they cannot resolve it. Where the sphere meets the floor a row of small hard-edged
  black notches remains: floor seen through a gap of 6-15 cm under the sphere, thinner than a voxel, which no probe sees;
  the floor at the sphere's left shows soft lighter streaks. The floor in the gap between the tall box and the red wall
  falls to 0.22 of its neighbour's light 0.1 m away (half is the goal), and that gap stays very dark: it lies in the box's
  full shadow, but the probes in front of it take the panel's light through the cascades' merge. Since batch 2 the dark
  reaches forward along the box's left face: a soft dark patch on the floor, from the box's front corner 0.6 m back and
  0.2 m wide, reads under half of what it read before the batch in the lit picture at 3840 x 2160 (2,555 pixels, the
  darkest 0.26 of before, 92 to 24 of 255; 2,543 pixels with TAA; under three quarters along 1.2 m of the face), with a
  small notch at the corner, and the floor 0.3 to 0.6 m from the sphere's contact is darker as well, smoothly (its mean
  green 24 of 255 against 34). Both came with the finest voxels' lower window (batch 2's first item, by the review's
  captures of the batch's steps). The cause is not established, and whether the patch is darker than the room's true
  light there was not measured: that floor lies in the box's shadow from the panel. At Low, cells of probes beside the
  box and the sphere lie inside them, and the walls and floor there still show steps and black patches, and the ceiling
  darkens along its edges.
  Low bounces once: with every bounce its Cornell box would be about 1.8 times as bright as High's (its ceiling 165-235
  nits per unit of albedo against 89-142). In the sun room, the window wall keeps its dark vertical bands either side of
  the window (a glow up to 1.8 times too bright near the window's edges, falling off a metre away) and the dark band
  along its foot (the probes 0.15 m above the floor see the sunlit patch only at grazing angles, which their directions do
  not resolve).
- The ground right at the edge of a thin gap beyond about 5.4 m from the camera at High (the first 0.2 m under the
  yard's covered car) reads about twice as bright as from nearer: there the probes beside the object light it without
  its overhang, since the 0.2 m voxels close the gap. That light ends in a hard line 0.25 m under the object's edge:
  seen from 6.5 m, the ground falls from 0.94 to 0.076 of the open ground's light from one pixel to the next (to black
  without the thin gaps' light, which falls off smoothly from there). At Low the probes beside the car light the first
  0.5 m under it as they light the open ground (0.97-0.99 of its light).
- The thin gaps' light takes the gap for three quarters of a voxel tall, of the voxels the surface lies in. Where the
  finer voxels' window ends the voxel doubles and the assumed gap with it, so the light of a gap that crosses the
  window's edge steps there: far inside the gap by up to four times, by the formula. Blending the two levels there is
  a change to the whole gather, whose probe visibility reads one level too, and is not made in this batch. Measured
  where the window's edge runs through the slot under the yard's car (seen from 5 m at 3840 x 2160), the GI view steps
  from 1 to 11 of 255 from one pixel row to the next and the lit picture from 20 to 25: that step is the probes' own
  light, there before batch 2 as well, and the thin gaps' light adds at most 1 of 255 on its dark side.
- The thin gaps' light can end in a crisp edge. In ThirdPerson's course one fold of the ground carries a lighter patch
  about 25 pixels across in the 3840 x 2160 look-dev picture, whose lower-left edge steps by 14-16 of 255 in the GI view
  (62 to 78) and by 3 in the lit picture (90 to 93). It came with the thin gaps' light: of the batch's captures of its
  steps, the pictures before the batch and after its first item have no edge there, and the one after its second item
  has it, by the same 3 levels. What in that light draws the edge is not established. By the code, the light looks for
  the first open voxel toward the camera, and that search is not continuous where the voxels close again on the way: a
  surface whose first sample is just open counts no depth and takes the opening's light whole, while its neighbour,
  whose first sample is just closed, marches on to an opening far away. A search that weighs every opening on the way
  would be continuous, but it is another design of the thin gaps' light, with its bands, its leaks and its cost to
  measure again, and is not made in this batch.
- Global illumination's own stage at 3840 x 2160 read about 4.4 ms in some runs and about 6.5 ms in others of the same
  build (the sun room and the yard, High): the difference lay in the pass that sums cascade 0's rays into the field (1.2
  or 3.6 ms), with the opaque pass's time unchanged. Every slow run was measured on one day before 15:00; afterwards
  neither that build (54 blocks of 60 frames in three processes) nor the one before it (16 runs) read slow once, with
  the same work in every frame, nor did any of the 61 later runs of five sessions (4.73 ms at most). The cause is not
  established: another program's work on the GPU falling into the frame's longest stage fits the readings but was not
  reproduced. This machine's readings do have states of their own: without global illumination, the 100,000 cubes' frame
  read 1.57, 1.77 or 2.31 ms by session, in every build measured, the one before batch 2 included. A lead, not a cause:
  on this driver that same pass's sums ran in two ways from frame to frame, a unit in the last place apart (batch 2's
  last item above), and two compiled forms of the pass would fit both findings; nothing ties them together yet, and
  neither the build with the sums rounded as written nor the one before it read slow in six runs each (4.53 ms at most).
  The stage's reading moves by itself, too: in those runs the yard's read 3.83 to 4.53 ms in one build and 4.08 to 4.51
  in the other, the frame's busy time following by 0.17 ms at most. Until the slow mode is explained, a single reading
  of the stage can be 2 ms high: compare builds only by interleaved runs, and by the frame's busy time first.
- Two captures of one still shot are not always equal, with or without global illumination, in this build and in the one
  before batch 2. Consecutive frames of a still camera are identical, and tested to be, with every bounce too; this is
  about pictures the editor's `viewport.capture` takes one after another. Look-dev captures each shot several times in a
  session and reports it (`repeats differ from the first by ... pixels`). At 3840 x 2160 with five captures a shot, this
  build's six global illumination pictures (the Cornell box, the sun room and the yard, without anti-aliasing and with
  TAA) repeated in each of three sessions and were equal across them; in the build before the batch the yard's TAA
  picture differed in 10 pixels in two of five captures of one session of three. Without global illumination both builds
  behave alike: the polished metal's shot without anti-aliasing differs from its first capture in 3,077 pixels in each
  of the four that follow, in five of six sessions at High (3,080 in the sixth) and in 2,378 pixels in six of six at
  Medium, and the steel sheet's shot with TAA at Medium in 400,058 or 439,206 pixels (5% of the frame) in every one of
  six sessions. The sun room's GI view with every bounce, captured eight times in a row without anti-aliasing, ended in
  one of two states in every build tried: in this build 2,874 pixels one level apart at the same exposure (the first
  capture against the other seven in one session, the first two against the other six in another); in the review's
  sessions 3,041 pixels in the batch's build before the review's fixes and 2,769 in the build before the batch. With TAA
  this build's eight were equal in both sessions, as the build before's were in the review's, where the batch's build
  differed in its last two captures, in 254,345 pixels (3% of the frame, all but three by one level, their automatic
  exposure 0.00002 EV from the others'). The cause is not established. A picture's change between two builds counts only
  beyond what its own repeats differ by.
- With every bounce, rooms take frames to fill after a cut, by their brightest surface: the sun room's gi view changes
  by less than 0.25% a frame from frame 11 (it is reported settled at 13), the Cornell box's from 15 (18), a closed
  white box's from 27 (39). Until then a game's cut shows the light rising.

**Reflections (R1)**

- In motion, screen-space reflections at High and Ultra still add a fine crawl to rough metal and wet ground, in the
  reflections of thin posts, shiny balls and edges: a third to a half less than 0.7.0's without TAA, up to a third less
  on metal with TAA and none less on wet ground with TAA, not yet the half less the wave aims at.
- Thin posts far behind a reflection (12 cm at 30 m) fall between the screen-space march's far steps: a rough metal
  sheet shows the wall behind them instead of the posts.
- At Ultra, shiny balls reflected in rough metal keep a fine grain in still pictures (`lookdev.sheet`'s spheres: noise
  1.05-1.08% at 3840x2160 against the 1% aimed at, High 0.95-0.96%): whether a ray grazing a ball's outline passes just
  behind it, which is not a hit, depends on its step offset.

**Reflections (R2)**

- An object in front of a mirror is reflected only where the camera also sees it (often a crescent), with the probes or
  the sky where its back would be: the screen holds only its front. Filling that silhouette with the front's colors was
  tried and drew front-lit rims instead.
- Where a mirror meets a ball's side at a slant, the screen-space march still misses part of it in fine stripes (in a
  test, 42% of those pixels hit at High, 65% at Ultra), which the resolve and the probes fill.
- With TAA, every highlight that slides over a surface as the camera moves pumps by 4.5-9.5% from frame to frame (the
  history follows the surface, not the highlight); a Source Radius roughly halves it on lanterns. Without TAA, lanterns'
  glints on wet ground pump by 1.4% as points and 0.3% sized, with FXAA by 4.2% and 1.3%.

**Lane Render**

- With contact shadows off, a thin object's shadow near the camera (a 5 cm plate on the floor) stays solid at High, but
  its edge shows the shadow map's texel steps, as Medium's did in 0.7.0. Graphics Quality Custom with Contact Shadows 12
  smooths it again, at the cost of shimmer in motion.

**Platform (S0.6).**

- If a windowed editor or game runs far above the display's rate with vsync on, the graphics driver is overriding vsync:
  set NVIDIA Control Panel's "Vertical sync" (Manage 3D settings) to "Use the 3D application setting". Borderless and
  fullscreen windows were not measured.

**Slang (S0.10)**

- Platforms: this release changes every shader. Its GPU tests ran on Windows with an NVIDIA GPU. On the Mac, CI converted
  every shader to Metal and compiled it (the 15 that read the bindless table only at run time, through MoltenVK's
  argument buffers, as before), but no GPU test ran through MoltenVK. On Mesa's lavapipe (a CPU driver, tried on CI), 529
  of 553 GPU tests pass with the new shaders as with the old, and the same 24 fail with both; AMD and Intel GPUs have not
  run them.

**physics-timing**

- While a game runs on the clock, world-space game UI, editor gizmos, picking, reflection probes, fog and post-process
  volumes, the sky and the sun, and attachments placed outside their animator's hierarchy use the simulated poses, up to
  one step off what is drawn (exact while paused or stepped).
- A character or body that rides something a script moves every frame in `OnUpdate` is drawn a step behind it (a
  constant 5 cm at 3 m/s and 60 steps per second; it flickered between 0 and 5 cm before): move carriers in
  `OnFixedUpdate` or as kinematic bodies.
- A frame that runs a fixed step costs more with interpolation, bound by memory: with 100,000 moving roots 5.5 ms
  against 2.6 ms without it on the development machine (`Perf.Scene.InterpolatedStepFrame100k.Ratio` 2.1 against the 1.5
  aimed at), while a real scene in the editor paid about 0.05 ms. Interpolation off removes the cost.

**Sun shadows (stable cascades)**

- Medium (2048 texels) draws the sun's shadows as 0.6.0 did: at 3840x2160 a texel spans about 2.2 screen pixels at each
  split and more nearer the camera, coarser than 0.7.0's Medium, whose cascades were fitted to what the view showed.
- High and Ultra are finer than 0.7.0's in most views (the shadows debug view over the look-dev shots at 3840x2160 and
  High: a median of 0.77 times 0.7.0's screen pixels per shadow texel), but coarser where 0.7.0 fitted its cascades to a
  few close or crowded surfaces. Medians of screen pixels per texel, 0.7.0 against 0.8.0 (under 1 is finer than a pixel):
  the showcase's shot 5, past a door post 0.3 m away, 0.88 against 1.48 (p90 1.24 against 2.09; the door post's pixels
  match 0.7.0's within 2 codes without anti-aliasing and with TAA); the showcase's macro shot, everything within 0.5 m
  at 30 degrees, 1.24 against 8.35 (p90 1.48 against 9.93; no shadow edge shows in it); the look-dev sun room seen from
  behind, 0.88 against 1.61; the Third-Person and Physics Playground editor views from high above, 0.31 and 0.57
  against 0.96, still under a pixel.

