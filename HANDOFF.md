# Handoff: work moves from the Windows machine to a Mac (2026-10-13)

The Windows development machine (i7-13700K, RTX 5090: the perf reference machine) is going out of service: its CPU is
being returned for replacement after repeated crashes. Work continues on a Mac (Apple Silicon). This file is the state
of the work and what comes next. CLAUDE.md imports it so that every session sees it; **when steps 1 to 3 below are done,
delete this file and the `@HANDOFF.md` line of CLAUDE.md** (Docs/Handoff/ goes with it; Docs/Plans/Wave4/ stays until
wave 4 lands).

## The owner and how they want the work done

- The goal, in the owner's words: "an actual production engine with its own identity: high scaling, modular, assets
  streaming optimized". What to build and when is the agent's call; "all I care is the end result is good".
- Work autonomously. Ask only when absolutely necessary, and do not interrupt the owner when it is not needed. Never
  record a question you asked the owner as a decision of theirs.
- Golden rule 1 is the owner's: never look outside the working directory or use other code on the machine as reference.
- Review before every commit; production-grade quality; code style as AGENTS.md says; tests written and passing.
- Commit and push to `origin` (github.com/2042Third/GameEngine), with the trailer lines the session gives for commits.
- **The owner judges the engine by running the editor.** It was `build/windows/bin/Release/StrataEditor.exe`; on the
  Mac it is `build/macos/bin/Release/StrataEditor`. Rebuild it after every change that lands on main, and smoke-run it.
- **Every wave ends with a release**, version 0.<wave>.0: 0.3.0 is published
  (https://github.com/2042Third/GameEngine/releases/tag/v0.3.0); wave 4 ends with 0.4.0. The procedure is AGENTS.md's
  "Releases" (an `-rc1` tag first, then the release tag).
- Do not add a LICENSE file: choosing one is the owner's call.
- The owner approved the thorough perf approach: measure, compare interleaved A/B runs, keep budgets only going down.
- The AGENTS.md split (step 2) is the owner's request: "i really like the information there, try find a way still put
  all the in a file". AGENTS.md (loaded into every session) keeps the rules under 40,000 characters, and
  Docs/DeveloperGuide.md keeps every detail.

## State

| Ref | Commit | What |
| --- | --- | --- |
| `main`, tag `v0.3.0` | 63c454a | Release 0.3.0: wave 3 and the release pipeline (`release.yml`, packages for three platforms). CI green. |
| `agent/integration` (also pushed as `ci/integration`) | bf8bc13 | main + wave 3's finishing round (merge 649d357) + 9 fixes from its review. CI on `ci/integration` was started at the handoff: check it. |
| `agent/agents-guide` | 0a549db | The AGENTS.md split, on 649d357. Not yet merged into `agent/integration`; it merged cleanly there before the crash. |
| `agent/wave3-finish` | 3b87411 | Wave 3's finishing round (already contained in `agent/integration`), kept for reference. |

- Wave 3's finishing round (on `agent/integration`):
  - A background log writer (`Core/BackgroundLogSink`).
  - Background job threads leave four hardware threads to the frame.
  - Deferred GPU releases are destroyed within a frame budget.
  - Frames that use the same materials take the material data over.
  - Command frame times are recorded without allocating.
  - The Activity panel's Result column is sized to its results.
  - The roadmap records of what it did about the perf items wave 3 left open.
- The Windows build of the merge, before the review's fixes, passed:
  - Debug under the validation layer: 48/48;
  - Release `-LE perf`: 47/48 (a flaky case, fixed in bf8bc13);
  - Dist `-LE "gpu|perf"`: 35/35.
- Each of the 9 fixes was built and its tests run on Windows (each new test checked to fail without its fix). The full
  suites were not run again after them.
- Review results: `Docs/Handoff/wave3-landing.md`. Of the 23 findings, 18 are real; 10 are fixed (the commit of each is
  named there) and 8 are open:
  - Docs: D1 to D4 and S1 (portability-docs), small.
  - Perf budgets: F2, F7 and F9 (tests-perf). These need the reference machine (see "Perf on the Mac"): leave them open
    and listed in the roadmap until it is back.
- The AGENTS.md split (`agent/agents-guide`):
  - AGENTS.md has 34,965 characters, holding the rules and a digest of each area that links into
    Docs/DeveloperGuide.md (174,858 characters, the moved text word for word).
  - A script found every one of the old file's 1,664 lines in one of the two files, and two independent checks passed
    (lossless and links; rules completeness).
  - Its doc test `Docs.AgentGuide` (`StrataTests/src/Docs/AgentGuideTests.cpp`: AGENTS.md under 40,000 characters,
    links and anchors) has **never been compiled**.
- Wave 4 is planned. It has five audits of main, a plan with six streams, a roadmap section and one spec per stream,
  all in `Docs/Plans/Wave4/` (its README says how to read them).

## Next steps, in order

1. **Set up the Mac** (AGENTS.md, "Building"):
   - Install the Xcode command line tools, CMake 3.25+, Ninja and Python 3.
   - Install the LunarG Vulkan SDK for macOS: it brings MoltenVK, the loader and the validation layers. The editor, the
     runtime with a window and the `gpu` tests need it.
   - Run `git submodule update --init --recursive --depth 1`, then `cmake --preset macos` and
     `cmake --build build/macos --config Debug --parallel` (also Release and Dist).
   - Run the tests with `ctest --preset macos-debug -LE perf`.
   - CI has only ever run the macOS tests without `gpu` and `perf`. **Nothing has rendered on macOS before.** Expect
     MoltenVK issues in the GPU paths, among them bindless descriptor limits, storage image formats, GTAO's compute
     shaders and block compression. The fixes must be portable: no macOS-only hacks, and Vulkan behavior gated on
     device features, never on the OS.
   - The deferred release test added in 6336981 counts on one frame's destroy budget not holding 512 texture
     destructions. A driver that destroys much faster could fail it; if so, make the test independent of driver speed.
2. **Land `agent/integration`**:
   - Merge `agent/agents-guide` into it with `--no-ff`.
   - Fix the open doc findings D1 to D4 and S1 in the file the split put their text in: the details in
     Docs/DeveloperGuide.md, the rules in AGENTS.md too.
   - Re-run the configure step (new test file), build Debug, Release and Dist, and run every suite (the `gpu` ones
     too, as far as the Mac allows). `Docs.AgentGuide` must pass.
   - Push as `ci/integration`, wait for green CI on all three platforms, then fast-forward main to it and push.
   - CI's annotations carry the failures (job logs need admin rights); `Docs/Handoff/tools/` has standard-library
     scripts that read them through GitHub's public API, or use `gh` if it is installed.
3. **The owner's editor on the Mac**:
   - Build Release and run `build/macos/bin/Release/StrataEditor`.
   - Check that the launcher works; that a Basic 3D project opens and renders (lit, shadows, sky); that the Tetris
     sample plays, builds its scripts with the installed toolchain, and exports; and that the exported game runs.
   - Whatever fails here comes before new features: the owner will judge the engine on this machine.
4. **Wave 4**, after Docs/Plans/Wave4/README.md:
   - Put `roadmap.md`'s section into Docs/Roadmap.md ("### Wave 4") and commit it.
   - Do step 0 of `plan.md`. The branches in flight are landed by step 2 above; then comes the small step-0 branch: the
     New Script configure-stamp fix and the rest it lists.
   - Then the six streams, each from its spec in `specs/` with `specs/common.md`.
   - Give each stream its own worktree (`git worktree add .worktrees/<key> -b agent/<key>`) and build directory, then
     an independent review, then integration, then CI on `ci/<name>`, then main.
   - The plan and specs were written on the Windows machine. Read `windows-*` presets as `macos-*`, and `<scratch>` and
     `<repo>` paths as notes of that machine.
   - Their AGENTS.md references predate the split: rules go into AGENTS.md (keep it under 40,000 characters, with
     `Docs.AgentGuide` as the gate), details into Docs/DeveloperGuide.md.
5. **Release 0.4.0** at the end of wave 4, as AGENTS.md's "Releases" says.

## Perf on the Mac

- StrataTests/Perf/Budgets.json holds measurements of the reference machine (i7-13700K, RTX 5090, Windows 11), which is
  away. On the Mac `-L perf` informs; it is not the gate.
- Never raise, lower or add a `Measured` value from Mac numbers.
- For perf work, compare A/B on the Mac (the same build tree for both sides, interleaved runs, a quiet machine), and
  state in the commit that the reference machine still has to confirm the budgets.
- Keep a list of the budget changes that wait for it in Docs/Roadmap.md's open items.
- If the Mac is to become the reference machine for good, all budgets must be measured again on it in one go. That is
  a decision to propose to the owner, not to drift into.

## Lessons from waves 1 to 3 (not all of them are in AGENTS.md)

- Integrate many branches on an integration branch, push it as `ci/<name>` and move main only after green CI.
- Every branch gets a read-only review, with its findings checked by a skeptic, before it merges.
- Older compilers fail on what newer ones accept: Apple Clang 15 (CI's macOS) and GCC 14 (CI's Linux), see "CI before
  main". The Mac's own Apple Clang is newer than CI's.
- Waits in tests go by the clock, never by a count of frames or updates: a busy CI runner breaks counts.
- On POSIX a copied file gets the current modification time.
- CI builds and tests Release only. Before a release, run the full Dist suite locally (`ctest --preset macos-dist -LE
  "gpu|perf"`), because Dist-only failures have slipped through before.
- Release packages are checked with the size notice `release.yml` writes on each platform's job. On Linux, `cpack`
  strips the executables.
- Follow-up: `actions/checkout@v4` and `actions/upload-artifact@v4` / `download-artifact@v4` warn that Node.js 20 is
  deprecated. Move ci.yml and release.yml to their Node 24 major versions (check the actions' tags first).
