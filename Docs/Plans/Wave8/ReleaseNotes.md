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

- Contact shadows are off in every Graphics Quality preset: they shimmered while the camera moved. The fitted sun cascades
  ground small objects near the camera.
- The sun's shadows no longer jump while something passes close to the camera (a ball thrown from it, a bird): the
  cascades follow a near object that holds less than a tenth of the view only once it has stayed for half a second (an
  eighth while the camera moves), and shrink only after half a second. Something that stays close still moves them once.
  `editor.perf` and `viewport.capture` report the fit's layout `changes` and whether a growth is `holding`.

**Platform: GLFW sealed behind the window (S0.8).**

- GLFW is sealed behind the window: only `Platform/GLFW` includes it, which the layer table's `ExternalOwners` enforces
  with Vulkan's and the OS headers; the Vulkan device presents through `Window::GetVulkanPresentation`.
- A window that cannot be opened ends the program through the fatal-error path (a dialog for a person, exit code 1)
  instead of aborting.
- `-DSTRATA_WITH_GLFW=OFF` builds the engine without a window backend: its programs run headless only. CI builds it.
- Tests drive the frame loop through `Tests::StubWindow`, a window without a display; `ApplicationSpecification::WindowFactory`
  lets a program or a test give the application its window.

**Platform: why FIFO does not pace the editor (S0.6).**

- The swapchain's log line names the present mode chosen and the modes the surface offers (`Swapchain: 2400 x 1350,
  3 images, vsync (FIFO; the surface offers FIFO, MAILBOX, ...)`).
- Measured: on the reference machine vsync paces no window in windowed mode, focused or not: FIFO is chosen and the
  presentation engine hands every image back at once (1,200-1,500 frames a second). Not the engine's code, nor focus,
  nor the RTSS or NVIDIA present layers: a driver or system setting, unconfirmed (Roadmap, open items). Borderless and
  fullscreen windows were not measured. There only a frame-rate cap paces the editor: File > Preferences > Frame Rate >
  Active frame rate (Unlimited by default).

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
  into the sky. A ray that passes behind something goes on to what lies beyond it.
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

## Defaults that change

Stored values never change: a scene or project that saved a value keeps it. These change what new components and
projects, the template and scenes or projects without the value get.

| What | 0.7.0 | 0.8.0 | Where |
| --- | --- | --- | --- |
| Contact shadow steps of the Graphics Quality presets | Low 0, Medium 0, High 12, Ultra 16 | 0 in every preset | lane Render |
| Compiled shaders | glslang | slangc; pictures differ by rounding only | S0.10 |
| First configure of a clone | offline after submodules | fetches Slang's archive once, or `STRATA_SLANG_ROOT` | S0.10 |

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
| **Reflections (R2)** | | | |
| A directional light's highlight | A point's (the smallest roughness's pin dot) whatever its Shadow Softness | Its disk's size: mirrors 1.8 times wider at the default 0.53 degrees, roughness 0.06 30% wider, 0.18 0.4% | Angular Diameter 0 (also hardens its shadows) |
| Shaders reading the atmosphere's sky-view, multiple-scattering or aerial tables, or its environment cube | Light per lux of sun | Per lux times `Atmosphere::c_StorageScale` (16384) | Multiply by `EnvironmentParams.x`, which divides by it (or by `1.0 / ST_ATMOSPHERE_STORAGE_SCALE`) |
| A non-metal's reflectance (F0) | 0.04 whatever its IOR | From its IOR: 1.5 (the default) exactly 0.04 as before, water 1.33 0.02, 2 0.11, 1 none | Set the IOR to 1.5 (it also bends what transmissive materials show) |
| Directional Light **Shadow Softness** | In the Shadows section, inactive without shadows | **Angular Diameter**, with the light's own settings, active with and without shadows; stored, scripted and set by commands as `ShadowSoftness` | Nothing to do |

## Known issues

**Reflections (R1)**

- In motion, screen-space reflections at High and Ultra still add a fine crawl to rough metal and wet ground, in the
  reflections of thin posts, shiny balls and edges: a third to a half less than 0.7.0's without TAA, up to a third less
  on metal with TAA and none less on wet ground with TAA, not yet the half less the wave aims at.

**Lane Render**

- An object that stays close to the camera still re-fits the cascades once, coarsening far shadows; one that covers under
  a tenth of the view waits up to half a second in a coarser cascade.
- With contact shadows off, a thin object's shadow near the camera (a 5 cm plate on the floor) stays solid at High, but
  its edge shows the shadow map's texel steps, as Medium's did in 0.7.0. Graphics Quality Custom with Contact Shadows 12
  smooths it again, at the cost of shimmer in motion.

**Platform (S0.6).**

- On the reference machine (NVIDIA RTX 5090, driver 621.1), vsync does not pace windows in windowed mode: FIFO presents
  without waiting, so a windowed editor or game runs as fast as it can unless a frame-rate cap is set. The cause is a
  driver or system setting, not confirmed yet. Borderless and fullscreen windows were not measured.

**Slang (S0.10)**

- Platforms: this release changes every shader. Its GPU tests ran on Windows with an NVIDIA GPU. On the Mac, CI converted
  every shader to Metal and compiled it (the 15 that read the bindless table only at run time, through MoltenVK's
  argument buffers, as before), but no GPU test ran through MoltenVK. On Mesa's lavapipe (a CPU driver, tried on CI), 529
  of 553 GPU tests pass with the new shaders as with the old, and the same 24 fail with both; AMD and Intel GPUs have not
  run them.
