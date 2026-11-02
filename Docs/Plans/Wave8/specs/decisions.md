# Wave 8: the orchestrator's rulings on the plan's decisions (2026-11-01)

The plan (`../plan.md`) was written from eleven audits and two critics. Its revision listed eleven decisions for the
orchestrator. These are the rulings; plan.md section 10 carries them as Q1 and D1-D9. Where a spec and this file disagree,
this file wins; a spec's "Review corrections" win over its body.

Refer to the owner as "the owner" (they/them), never by a gendered pronoun: the owner has not stated theirs.

| # | The planner's decision | Ruling |
| --- | --- | --- |
| 1 | Ask the owner Q1 (may `animation-graph` read Esoterica at c99a946 and port runtime semantics, with the MIT notice, through a scoped exception in golden rule 1) | **Answered yes, and wider (2026-11-01, D10 below)** |
| 2 | Ask Q2 (a one-pole temporal filter on the GI buffer alone) before `radiance-cascades`' stage 1 | **Not asked: D7.** The default stays no history. Only if the fence misses in every degrade form is that one filter tried, and it then passes the same numeric gate and the owner's live look in motion. AGENTS.md's Lighting rule changes only if it is adopted |
| 3 | Tranches 1-3 and the usage rule (60%, 75%, 90%, 25% reserved for a landing) | **Accepted. D8:** this session cannot read the owner's usage. The orchestrator asks for the figure before landing 1's integration, before tranche 2 and before landing 2, and otherwise goes by what the owner reports |
| 4 | Slang in step 0, boxed at 2 agent-days with one extension, MoltenVK conversion and the HLSL test inside the box | **Accepted.** Rendering streams branch from S0.12's tip; `showcase` and `physics-timing` do not wait |
| 5 | D1: Slang pinned as a release archive by SHA-256 in a per-user cache, `STRATA_SLANG_ROOT`, one sentence in AGENTS.md's third-party section | **Accepted.** The sentence is within the standing autonomy (AGENTS.md changes when workflows do, checklist item 5) |
| 6 | Three landings: R1 alone, landing 1 with the showcase, landing 2 | **Accepted** |
| 7 | G1 stays numeric; the owner's live look is the exit of hardening and the condition of adoption | **Accepted.** If the owner has not looked when the measure is due, GI ships off in the samples unless hardening is clearly stable in our own look in motion |
| 8 | D5: interpolation of drawn poses, on by default in games and Play, about half a step of mean latency | **Accepted.** Step 0's report to the owner gives the numbers |
| 9 | D2: showcase within 80 MB with the 72.2 MB A-list; a first-person walk; the bus side weathered steel without a clear coat; item 4's reading asked with the first sheet | **Accepted** |
| 10 | Platforms for 0.8.0 | **D9:** no Mac is reachable from this session (its Mac sessions are offline). S0.10 and the release run CI's conversion to Metal as the minimum, the Mesa job is tried, and the release notes say what did not run. If a Mac session comes online before a gate, the GPU suites run there |
| 11 | D4: perf on the temporary memory by interleaved A/Bs against a kept 76f28a21 Release build, no campaign, no budget changes, new metrics `Provisional` | **Confirmed.** Wave 7's unshown sheets and motion blur table go on this wave's sheet |

## The owner's answers (2026-11-01, verbatim)

"Yes you have permission to read and use esoterica engine and other repos if needed. The limitation I said only applies to
other folders on this computer. You can/should use open source resources as much as you need."

"I meant highlights and reflections, highlights are extremely important just like in a drawing, because they should be
cheap. And the sunlight thrown off the sheet onto other things is also something I was thinking to do, but it is not a
priority."

- **D10, open source.** Golden rule 1 now forbids only other folders on this computer (S0.1 rewrites it, and
  `ThirdPartyNotices.md` gains a section for code derived from projects that are not submodules). Open-source projects,
  papers and talks are read as much as helps. Code is copied, ported or adapted only from permissive licenses (MIT, BSD,
  Apache-2.0, zlib, Boost, public domain or CC0), with the notice kept and the source named in the commit message. Copyleft
  code (GPL, LGPL, AGPL, MPL and the like) is read for ideas, never copied or ported: Strata's license is the owner's choice,
  and copying copyleft code would make it. `animation-graph` reads Esoterica at c99a946 and ports runtime semantics under
  its MIT notice; vendoring it stays out for the plan's technical reasons.
- **Highlights first.** Highlights lead `reflections`' R2 as step 7 (the sun's size, small lights' highlights, a source
  radius for highlights only), starting as soon as R1 is in review and joining R1's landing if verified by then. Every
  highlight is judged with no AA, FXAA and TAA; one that shimmers fails. The showcase judges shots 2-4 on highlights first.
- **Sun thrown off a sheet** goes to the roadmap's "Later", after `radiance-cascades` lands.
- The specs workflow of 2026-11-01 was briefed before these answers. The orchestrator folds them into `reflections`,
  `showcase` and every later brief after the reviews return.

## Standing rules carried from wave 7

- Native resolution only, never temporal upscaling; nothing needs TAA to look right (Medium's Checkerboard reflections are the
  named exception); realistic, filmic, the Fox Engine baseline, neither blurry nor clinical.
- Budgets only go down; nothing is measured against `Budgets.json` on the temporary memory.
- At most four build-heavy agents at once; each agent its own worktree and build directory; never the owner's `build/windows`
  except when the orchestrator rebuilds it after a landing.
- Never read or use anything from other folders on this computer. Open source under D10.

## Rulings on the specs' open questions (2026-11-02)

The specs workflow returned eight specs, each corrected in place by an independent reviewer. Where a reviewer recommended a
decision, it is accepted unless a line below says otherwise. Where a spec still says "no code from outside the repository",
D10 governs.

**Usage (D8).** The owner reported 15% of the weekly usage spent on 2026-11-02, after the audits and the specs (about 14 M
subagent tokens). So every lane and stream runs lean: one implementer for the whole lane, one independent reviewer, and a
fixer only for findings. Reviews do not fan out into several lenses and adversarial votes. The usage rule of plan section 10
stands.

**The object store (S0.0a): done by the orchestrator, 2026-11-02 00:35 PST.** `git fsck --full` found 7 corrupt loose objects
and 12 missing ones: everything that had been in `.git/objects/c1/`, and one more hidden under a missing tree. They were left by
the crash of 2026-10-28; one corrupt file held another file's text, the MIT license. All 19 were written again from a mirror of
origin with `git cat-file` and `git hash-object -w`, and the hashes match. No repack ran, and the packs are untouched. `fsck`
is clean. All 2,283 tracked files and every submodule's files hash equal to their index, so no working file was damaged.
`git log -- <path>` walks past f99f3a41 again. Logs: `fsck-before.log`, `fsck-after.log`; the damaged files are in
`corrupt-objects/`. step0.md 4.1 is therefore done, and agents may use history walks freely.

**Worktrees.** Every worktree is made with `sh .worktrees/create_worktree.sh <name> <base>`, which makes the branch
`agent/<name>`. Where a spec writes `git worktree add`, read that script. A stream's A/B base other than
`.worktrees/wave8-base` (for example `reflections-base` at S0.12's tip) is made the same way: its branch is never committed
to or pushed, it is built in Release only in the stream's own slot, and it is removed when the stream lands.

**step0.md:** golden rule 1's text in 5.5 is accepted. Renderers of single images keep 0.7.0's fit rules (`ShadowFitHolds`
off, 7.3a). `IsSettled` is false while a growth waits. The far bound grows at once and shrinks like every other bound. The
owner's list and this file are committed to `Docs/Plans/Wave8/`, as wave 7 did. AGENTS.md: S0.1 frees at least 3,000 code
points (3,164 with the review). Lane caps: Slang at most 440, Platform 201 (or the Layers clause alone), Editor 179. Each stream
gets at most 250. A lander that would pass 40,000 shortens a bullet of its own area into the guide.

**step0-editor.md:**
- The precise wait (a Windows high-resolution waitable timer under `Platform/Windows`, no `timeBeginPeriod`) comes into S0.4
  whenever FramePacer fails at 120 Hz.
- S0.7a (placement core and commands) may land ahead of S0.7b.
- A multi-root prefab's roots move rigidly together, with a line in the upgrade notes.
- Q3-Q6 are as the spec says.

**step0-platform.md:**
- S0.9 goes first and lands alone through `ci/wave8-platform-timing`. `physics-timing` starts once both S0.9 and lane Render
  are on main.
- The CI row is renamed, and every reference in the repository (CIStatus.py, docs) changes in the same commit.
- Q-P3 to Q-P10 are as recommended.
- S0.6's case A may take the desktop's focus once, for about two minutes; the lane logs when.
- The question about NVIDIA's driver settings goes to the owner, through the orchestrator, only if S0.6 shows that FIFO does
  not pace a focused window.

**step0-slang.md:**
- No quiet window is booked for gate 4. Its readings are interleaved pairs (A B, B A, A B) under `PerfLock.py`. A reading
  counts when the two A runs agree within 3% on the gated stages. Otherwise it is retried up to three times, 15 minutes apart,
  and then the lane reports the noise instead of a pass, and the orchestrator decides.
- The Mesa try stays inside the box (D9).
- C0 lands ahead of the lane. If Slang moves to wave 9, C2 and C2b land beside C0 when their own proofs hold.
- C8 is done only with at least 0.25 agent-day left.
- The spirv-val and Metal steps stay on CI.
- The three Slang findings are recorded as open items. Filing them upstream is the owner's call, because it is outward-facing.

**showcase.md:**
- OQ1-OQ5 accepted. The graybox does not wait for S0.7.
- S1-S3 are verified by the generators' selftests until the first build, which runs the full Debug ctest before S4.
- The weathered steel gets its color map.
- Q1 and Q3 accepted.
- Q7: exclusive fullscreen if the runtime offers it, else 2560x1440 with renamed Provisional metrics.
- The showcase uses `.worktrees/wave8-base`'s editor until lane Render lands. That base is built after the 0.7.0 capture,
  so S1's downloads and generators come first.

**reflections.md:**
- The wave has one flicker meter, `StrataTests/src/Renderer/FlickerMetrics.h/.cpp` (suite `Renderer.FlickerMetrics`), with
  radiance-cascades' D11 definitions word for word plus `ReadDisplayFlickerFrame`. The first lander creates it, and
  radiance-cascades.md has been aligned.
- Plan 1.1's flicker row gains "and the per-pixel Mean that SSR adds at least halved".
- Step 7 joins R1's landing only if it is verified by then. R1 never waits for it.
- Probes brighter than 1e6 nits are re-baked through command steps.
- SourceRadius defaults to 0.
- The resolve candidate is chosen by D2's rule, and back faces by looking (crescents if unclear).
- At 256, if conversion cannot hold 2 ms even with per-face steps, the row stays at 128.

**physics-timing.md:**
- The arm comes in at once and goes out smoothly.
- Automation sessions interpolate, as documented.
- No owner question is asked about the cut items; the release notes' Known issues name them.

**radiance-cascades.md:**
- The reviewer's fill from two levels up and the self-plane slack are accepted, each measured against the original form on the
  tuning day.
- 221 MiB at 4K High is accepted for stages 1-2 and stated in the release notes.
- Settling moves into stage 1, and Low moves above the cut.
- G1's BlocksP99 is at most 3.5%. G1 is not relaxed if today's GTAO flickers; both numbers are shown to the owner.
- Quantization happens only if a rounding cycle appears.
- The ExileCon talk corrects the table of departures. Permissively licensed radiance cascades implementations may be read
  and ported under D10.
