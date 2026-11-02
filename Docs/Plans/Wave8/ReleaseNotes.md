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

## Defaults that change

Stored values never change: a scene or project that saved a value keeps it. These change what new components and
projects, the template and scenes or projects without the value get.

| What | 0.7.0 | 0.8.0 | Where |
| --- | --- | --- | --- |
| Contact shadow steps of the Graphics Quality presets | Low 0, Medium 0, High 12, Ultra 16 | 0 in every preset | lane Render |
| What games, Play and Simulate draw | each body's last fixed pose | between its last two fixed poses, a step behind the clock | physics-timing |

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
| **physics-timing** | | | |
| Scripts that place something from a moving body's position every frame (a follow camera, an effect) | the body's last fixed pose, which is what was drawn | the body is drawn up to a step behind its last fixed pose, so it seems to jitter against them | read `GetDrawnPosition()`/`GetDrawnRotation()` in `OnLateUpdate` instead of the world position |
| Scripts that move an entity far by setting its transform in `OnFixedUpdate` | drawn there at the step | drawn sliding there over a step if within 400 m/s | call `GetTransform().ResetInterpolation()` after the move |
| What games, Play and Simulate draw | each body's last fixed pose | between its last two fixed poses | Project Settings > Physics > Timing > Interpolation off |

## Known issues

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

**physics-timing**

- While a game runs on the clock, world-space game UI, editor gizmos, picking, reflection probes, fog and post-process
  volumes, the sky and the sun, and attachments placed outside their animator's hierarchy use the simulated poses, up to
  one step off what is drawn (exact while paused or stepped).
- A character or body that rides something a script moves every frame in `OnUpdate` is drawn a step behind it (a
  constant 5 cm at 3 m/s and 60 steps per second; it flickered between 0 and 5 cm before): move carriers in
  `OnFixedUpdate` or as kinematic bodies.
