# Workstream `editor-perf`: the editor regressions of waves 4-6 (wave 7)

Written by the orchestrator on 2026-10-27 from step 0's measurements (Docs/Roadmap.md, open items: "Blockers of 0.7.0";
`.worktrees/wave7-plan/step0_result.json`). A small stream: diagnose and fix, no new features. It runs beside the five
other wave-7 streams and touches none of the renderer's files they own unless a cause lies there (then the smallest fix,
named in the merge notes).

## 1. Goal

Four editor metrics regressed in waves 4-6 (3b87411c..bc6ee123), proven on the reference machine by interleaved A/Bs
against wave 3's Release build 3b87411c (3 pairs, medians), each over its budget in every attempt alone. AGENTS.md: "a
metric over budget is a regression to fix"; budgets only go down. Each is fixed before release 0.7.0:

| Metric | Budget | wave 3 (3b87411c) | bc6ee123 | Ratio |
| --- | --- | --- | --- | --- |
| `PerfGPU.Editor.SceneOpenOver1MMaxFrame` | 1.143 ms | 0.664 | 1.839 (step 0's run 4.97-9.76) | 2.77 |
| `PerfGPU.Editor.ThumbnailsMaxFrame` | 1.149 ms | 0.978 | 2.073 | 2.12 |
| `Perf.EditorPanels.Performance600FramesP95` | 0.0924 ms | 0.069 | 0.0993 | 1.44 |
| `PerfGPU.Editor.EditFrameNested100k` | 0.2808 ms | 0.2153 | 0.2708 (0.287-0.299 in step 0's -L perf run) | 1.26 |
| `PerfGPU.Editor.PlayFrameNested100k` (within budget, goes with the last) | 0.2928 ms | | 1.43 x wave 3 | 1.43 |

The owner uses the editor every day and cares about how it runs: these are frames a person feels (opening a big scene,
browsing thumbnails, the Performance panel open, editing a large hierarchy).

Note step 0's change: the editor's frame perf tests now measure at Medium quality (their projects store
`Renderer.Quality` Medium), so wave 7's High default does not move them.

## 2. Method

For each metric, in the order of the table (largest ratio first, since a shared cause may fix several):

1. Reproduce alone on main's tip (step 0 landed) under `Tools/PerfLock.py`, Release, the single test case
   (`StrataTests.exe --test-case=...` with the perf harness's attempts), and with wave 3's build for the same case.
2. Find the cause: bisect 3b87411c..bc6ee123 along main's first-parent history (wave merges, then within the merged
   stream), building only what the case needs in a scratch worktree (`git worktree add` under
   `G:/GameEngineAI/.worktrees/editor-perf-bisect`, its own build directory; MSVC 14.50 may reject old commits: apply
   step 0's `static constexpr` fix of `CoreTests.cpp` there if needed, never commit in the scratch tree), and/or compare
   Tracy or sampling profiles of the two builds' frames. Interleave measurements; judge medians; a 1.26x ratio needs
   more pairs than a 2.77x one.
3. Fix it in the code of today, keeping what the waves added (no feature removed to win time back). Typical causes to
   check first: work that became per-frame (a scan, a sort, a copy of a growing list), a cache invalidated every frame
   (a version bumped by something that does not change), work moved onto the main thread, a lock or a wait added to the
   frame, a per-entity path in the Inspector, Hierarchy or the Performance panel's history, thumbnails rendered or
   encoded on the frame instead of a job.
4. Prove it: the metric within its budget in every attempt alone (5 runs of its case under PerfLock), the A/B against
   bc6ee123 (ratio and medians in the commit message), and no other `Perf.Editor*`/`PerfGPU.Editor*` metric worse.
   Where a unit test can pin the cause (a cache that must not rebuild, a count of calls per frame), add it.

## 3. Rules

- No budget raised. If a metric cannot reach its budget without dropping a feature, stop and report the trade-off with
  numbers instead of committing (the orchestrator decides).
- No PerfStability campaign; measurements under `Tools/PerfLock.py` as information and proof. Five other streams build
  on the machine: interleave pairs and repeat when load shows (the canary `Perf.Harness.Sort200k` says how loaded).
- Each fix is a commit with the measurements; docs updated where behavior changed (DeveloperGuide sections of the
  editor, the panels, thumbnails or scene loading), and the roadmap's open items line for each blocker updated by the
  integrator (state the fix in your report).
- The full Debug ctest (validation layer on) and the Release ctest without perf pass before you finish.

## 4. First hour

1. `python Tools/PerfLock.py --status`; read Docs/Roadmap.md's open items and Docs/DeveloperGuide.md "Perf tests".
2. Find the four cases in `StrataTests/src/Editor/*PerfTests.cpp` and `StrataTests/src/Editor/EditorPanelPerfTests.cpp`
   (or wherever `--test-case` finds them) and read how each measures.
3. Build Release in the worktree; build 3b87411c's StrataTests in the scratch worktree; reproduce the largest ratio.
