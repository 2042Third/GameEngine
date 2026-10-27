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
