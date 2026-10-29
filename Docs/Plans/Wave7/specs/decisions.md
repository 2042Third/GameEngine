# Wave 7: the orchestrator's decisions on the specs' open questions (2026-10-27)

The specs in this directory were written and then corrected by independent reviewers (each ends with "Review
corrections"). Where a spec's text and a reviewer's correction disagree, the correction wins. Where specs disagree with
each other on a shared name, slot or stage position, `step0.md` wins (it lands first and its code is what the streams
build on); each stream records any name it found different in an "As built" note in its first commit's message and in
its spec's copy in `Docs/Plans/Wave7/specs/`.

## Step 0 (`step0.md`)

- The look-dev harness (S0.6) comes before S0.3 and S0.4, and its 0.6.0 baseline is taken from the owner's
  `G:\GameEngineAI\build\windows` Release build (bc6ee123) before anything rebuilds that tree. Accepted.
- S0.2: test first (renderer level, then the editor's reproduction), then the fixes the test proves. Accepted.
- `ForBake()` keeps 0.6.0's values. Accepted.
- Ultra's automatic shadow resolution is a second option, `AutoFine` (Auto: 2048 below 1440 rows, 4096 from 1440;
  AutoFine: 4096 / 8192). Accepted; graphics-options' `AutoUltra` proposal is withdrawn.
- `StrataTools.LookDev` CTest: labels `gpu;tools`, an AGENTS.md exception like the package tests (needs Python), and a
  clear failure naming the reason when Python is missing. Accepted.
- The editor's and runtime's existing perf tests measure Medium (their projects store `Renderer.Quality` Medium; runtime
  perf tests pass `--quality medium`); High gets new provisional `*High4K` metrics. Accepted.
- Budgets after the CPU replacement: the reviewer's rule (section 11) stands. An over-budget metric is measured alone,
  then A/B'd against the commit that set its budget; only a machine change shown that way raises a budget
  (`--rebaseline <metric>=<reason>`). A code regression is fixed in step 0 or becomes a named 0.7.0 release blocker in
  the open items. The implementer applies this rule case by case and lists every raised budget with its A/B evidence in
  its report; the orchestrator reviews each before step 0 lands. A library change the owner asked for (wave 5's move
  from Jolt to box3d) is not a machine change: physics metrics slower since that port are regressions to explain and,
  where Strata's own glue is the cause, to fix; where box3d itself is slower, say so with the measurement.
- `StrataRuntime --quality` with `--headless` is refused (exit 3). Accepted.
- Without a settings provider, the project quality is Medium (tests and tools); projects without the section get High.
  Accepted (reviewer's correction).

## `graphics-options`

- Choice values cross the ABI by name, never by position (reviewer's correction). Accepted.
- The info struct filled in module memory (capacity in, filled size out, StructSize) is an accepted ABI pattern; the
  DeveloperGuide's ABI rules are updated to say so.
- Supersampled stills (below the cut) re-quantize with the dither.

## `sun-shadows`

- Step 6 fits the cascades' squares to the receivers as well as their depth range (beyond the plan's "depth range"
  SDSM), with one reduction pass (`ShadowFit.comp`) on both paths. Accepted.
- Scene SRV 28 (mask), 32 (cascade depth ranges), 33 (blue noise). Accepted; step 0 writes them in Scene.glsl's slot
  comment (light-and-materials: 29-31 and 34; next free 35).
- `CascadeDistribution` default 0.9 for fitted layouts, 0.8 for fallback frames. Accepted.
- The mask is the red channel of an RG8 ping-pong pair, stored as the square root of visibility (reviewer); R16 only if
  contours show on the look-dev sheet.
- The Open World eye view may land up to 1.7 screen pixels per texel with a named remedy. Accepted.

## `camera-and-tone`

- Banding: the reviewer's metric stands.
- Center-weighted metering, the exposure key and faster light adaptation apply to stored scenes through the new
  properties' defaults, named in the release notes. Accepted.
- The players' AntiAliasing row includes Off. Accepted.
- The in-house morphological AA is `AntiAliasingMode::Morphological` (not SMAA: its design shares nothing with the
  published SMAA and a permanent enum value must not promise its behavior). Accepted.
- Motion blur: 180 and 90 degrees go on the before/after sheet; 90 is the template's fallback only if 90 holds and 180
  does not; otherwise the template and samples ship it off. The owner rejects smear: when in doubt, off.
- `BloomRadius` as the reviewer corrected it. Hue metric for the Filmic fit: ICtCp. Accepted.

## `light-and-materials`

- The template's sun is 120,000 lux above the atmosphere (about 95,000 at the ground at 40 degrees). Accepted.
- Spatial SSR (below the cut) reads the previous frame's pre-transparent color moved along the motion vectors, at full
  resolution (no accumulation). Accepted.
- Overcast is a procedural-sky preset. Accepted.
- The 5x5 blur after the 4x4 resolve stays unless the look-dev sheet shows lost crease detail. Accepted.
- The Open World's real sun and moon values go to "Later". Accepted.
- Alpha sharpening in the prepass is dropped (no alpha to coverage without MSAA); coverage-preserving mips stay below
  the cut. Accepted.

## The owner on physics (2026-10-27, verbatim)

"I don't think physics object creation performance is a big deal. But i care about runtime performance. For large
box3d physics be sure to use box3d's contact recycling. With a contact recycling of 0.7 or something will put more
entities in sleep and making something more stable"

So: physics metrics of body creation and loading (`Perf.Physics.Create10kBodies`, `Create100kStaticBodies`) are not
release blockers; record their box3d-port cost in the open items with the measurements, and do not spend wave 7's effort
on them. Runtime is what counts: the step, Strata's glue around it (`StrataToLibraryStep` ratios), frame hitches while
playing (`Grow65kMaxFrame`, cell streaming), sleeping and stability. Contact recycling: box3d has it on by default
(`b3BodyDef::enableContactRecycling`, world distance `B3_CONTACT_RECYCLE_DISTANCE` = 10 x the 5 mm linear slop = 5 cm,
angle 10 degrees); Strata uses the defaults and exposes nothing. A fifth stream, `physics-runtime`, measures and tunes it
(the owner's "0.7 or something" is to be found by measurement: box3d's parameter is a distance in meters, so the stream
sweeps it and reports what each value does to sleeping, stacking stability and step time), exposes it as a project
physics setting, turns it off for character controllers (box3d: recycling "may lead to ghost collision that should be
avoided on characters"), and brings the runtime costs above down.

## `physics-runtime` (`physics-runtime.md`)

- Q1: `Perf.Physics.Create10kBodies` is re-baselined once (`PerfStability.py --rebaseline`), citing wave 5's move to box3d
  and the owner's ruling of 2026-10-27; its budget becomes the box3d median (within the 1.5x rule) and Jolt's value stays
  as its `Target`. Whoever lands first does it (step 0 if it has not marked the metric a release blocker in a way that
  needs undoing; otherwise this stream's first commit), and the roadmap's open items say so. Not a release blocker.
- Q2: the sweep measures every reading of "0.7" and the default is chosen by the spec's stated rule; the report to the
  owner names each reading's numbers.
- Q5: `Grow65kMaxFrame` keeps its 16.7 ms target as a goal (creation is out of scope); the stream is judged on the new
  `Perf.Physics.Grow65k.GrowthMs` (target 4 ms).
- Q6: `Perf.World.CellUnloadMaxFrame` keeps its 4 ms target as an open item if box3d alone exceeds it; the stream cuts
  Strata's share to at most 1.25x box3d's own removal time.
- The stream's AGENTS.md bullet is shortened to its first clause if room runs out.

## Integration rulings (2026-10-28, after the six streams' verify reports)

- **sun-shadows, cost.** The screen-shadow pass costs 0.11-0.16 ms at 1080 Medium (busy 1.15-1.24x step 0 on the
  samples' small scenes, a fixed per-pixel overhead) and 0.54-0.83 ms at 4K High (targets 0.35/0.45). It lands: Medium
  keeps 0.6.0's sample counts and resolutions (plan.md's meaning of the AGENTS.md rule), and the image change is the
  wave's main point. The cost is then cut in a wave perf pass on the integrated tree (one budget for the whole wave, not
  per stream): per-pass GPU timers inside GPU.ScreenShadows, fewer depth-range reads per pixel in the evaluation,
  ContactShadows' step count at High, the fit at lower resolution. Targets for that pass: whole-wave Medium 1080 GPU
  busy at most 1.15x step 0 on the three samples' scene cameras; GPU.ScreenShadows at most 0.45 ms at 4K High. What is
  not reached is a named open item in the roadmap with its numbers.
- **sun-shadows, noise metric.** The 9-pixel moving-average metric reads the profile of a physically narrow penumbra
  (it gives 1.6% in every AA mode, so it is not grain). The grain metric is the RMS of the second difference across the
  penumbra row (0.95% at High): the 1% target is met by that metric. Record both numbers.
- **physics-runtime, recycle distance.** box3d's 5 cm stays the default (recycling on for every body but characters).
  The rule's gates had no valid outcome; 2.5 cm sleeps the 2,000-ball pit 8 s later and keeps a 47 mm bump on triangle
  meshes that 5 cm removes. Its ghost cost (seams +5.0 mm over distance 0, ledges +1.7 cm) is accepted and measured by
  Physics.Settling's bounds (1.25x the measured cost). Sleep threshold 0.08 m/s (box3d 0.05) by the spec's rule. Projects
  tune both in Physics.Simulation.
- **camera-and-tone, template exposure.** The template follows the samples' median rule (median display luminance in
  [0.15, 0.22]), not a fixed Exposure 0: re-measure it once light-and-materials' sun and sky are in, and set it then.
- **camera-and-tone, OpenWorldWindowedImage.** The green margin 40 -> 30 codes is accepted as a recalibration for an
  intended look change (Filmic keeps scene chroma); the check still fails a sky-only frame.
- **camera-and-tone, motion blur.** Ships off everywhere. The owner's sheet shows the full criteria table, failing edges
  included, before it is turned on anywhere. No Motion Blur row in the Third-Person menu while it ships off.
- **editor-perf.** SceneOpenOver1MMaxFrame stays an open 0.7.0 item (4.18 -> 1.23 ms median, ratio 0.295): re-measure on
  the reference memory; the stream's next hypotheses go into the roadmap. ThumbnailsMaxFrame is closed only after the
  reference-memory campaign.
- **History.** Commits on main build and pass on their own where it is cheap to make them so: the integrator reorders
  or folds the named broken commits (light-and-materials: fd5fc3f1 before a27f44ae, the DepthPyramidNearest deletion
  into df574d55, c38e5c53's GCC renames into the commits that introduced the members if the rebase stays mechanical;
  camera-and-tone: the motion blur shaders into 0776e91c). Every rewritten branch's tip tree must equal the verified
  tip's tree (`git diff <old> <new>` empty).
- **graphics-options.** The players' file cache outside play stays as designed (same as the bindings). S8 goes to the
  roadmap's Later.
