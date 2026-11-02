# Step 0 of wave 8: baselines, room in AGENTS.md, lane Render, the indirect light's seam, the roadmap, and how the lanes land

This spec covers S0.0 (first), S0.1 (first), lane Render (S0.2 and S0.3), S0.11 and S0.12 (last), and the landing of
every step 0 lane. The other lanes (Editor: S0.7, S0.4, S0.5; Platform: S0.8, S0.6, S0.9; Slang: S0.10) have specs of their
own; section 10 says only how they land.

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `E/` is `StrataEditor/src/Editor/`, `T/` is
`StrataTests/src/`, `DG` is `Docs/DeveloperGuide.md`, `plan` is `.worktrees/wave8-plan/plan.md`, `decisions` is
`.worktrees/wave8-plan/specs/decisions.md` (its rulings win over the plan and over this spec). Every file:line is as of
76f28a21 (main, release 0.7.0) unless it says otherwise. Shaders are named as `strata_add_shaders` registers them: after lane
Slang, `SH/Scene/Forward.frag` is the file `Forward.frag.slang` and `Include/X.glsl` is `Include/X.slang`.

Rules that hold for everything here (AGENTS.md, plan 4.1, decisions, this machine):

- Never the owner's `build/windows` except when the orchestrator rebuilds it after a landing (10.4), and nothing rebuilds it
  before S0.0's baseline is complete (4.3).
- At most four build-heavy agents at once. Step 0's four lanes are those four; landings and rebuilds run in the slot a
  landing lane frees.
- Every commit that changes code: Debug build with warnings as errors, `ctest --preset windows-debug -j 8` and every `GPU*`
  suite under the validation layer (`VK_ADD_LAYER_PATH=G:/GameEngineAI/build/_tools/Vulkan-ValidationLayers/build/layers/Release`),
  `python Tools/CompatLint.py --changed-since <the lane's base>` clean.
- Perf only under `python Tools/PerfLock.py --label <who> -- ...`, interleaved against `.worktrees/wave8-base` (D4), as
  information: no campaign, no budget change, new metrics `Provisional`.
- Python through `/c/msys64/mingw64/bin/python.exe <script file>` (never a script on stdin: it hangs). Editors driven in
  parallel use private session directories and `StrataCLI call --project` always.
- Refer to the owner as "the owner" (they/them).

## 1. The owner's request and these steps' goal

The owner (2026-11-01, `.worktrees/wave8-plan/owner_feedback.md`): "Make prioritization yourself, but showcase needs to land
this wave". Item 1: "Contact shadow is terrible; shimmering, and inconsistent. Fix or default to turn off. (don't prioritize
this at all, just default it to off, unless it is a easy fix)". Item 8: "In the physics playground, when shooting a ball a
bit away from the playground in the air, while the ball is in front of the camera the shadow behind jumps or changes ...
(pretty weird bug, investigate and work on it)".

In priority order:

1. **The two shadow items reach the owner in days (S0.2, S0.3).** Contact shadows off in every preset; the cascade fit stops
   re-cutting every slice for a ball passing the camera. Both land before the showcase's graybox (plan 4.2 step 4) and before
   `physics-timing` branches.
2. **The record every A/B and sheet starts from (S0.0).** The look-dev baseline of 0.7.0, a kept Release build of 76f28a21
   for interleaved A/Bs (D4), the plan and specs committed, and the repository's object store repaired (4.1: found while
   writing this spec).
3. **Parallel work that only adds (S0.1, S0.11).** Room in AGENTS.md for nine digest bullets, the three lanes' rules and
   golden rule 1 as the owner scoped it on 2026-11-01 (D10: other folders of this computer never; open-source code under
   permissive licenses, with `ThirdPartyNotices.md` gaining a section for derived code); the
   forward shader's indirect light split into shared inputs, a diffuse gather, a specular gather and a compose step, so that
   `reflections` and `radiance-cascades` change different functions; slots, uniform fields, GPU stages and debug views
   assigned.
4. **The roadmap (S0.12)** with the wave 8 entry and wave 9's animation entries, so the owner's "record and push into wave 9"
   holds whatever happens to `animation-graph`.
5. **Landings that keep main releasable (section 10).**

Not the goal: a contact shadow fix (plan section 9: M to L, "Later"); gap-aware cascade slices (not prototyped, "Later");
any change of pixels by S0.11; any new look.

## 2. Where we start

### 2.1 Files

| File | Lines | Role here |
| --- | --- | --- |
| `AGENTS.md` | 492 (39,292 code points) | `Docs.AgentGuide` allows 40,000 (`T/Docs/AgentGuideTests.cpp:25`): 708 free. The plan's "692" counted bytes (39,308) |
| `Docs/DeveloperGuide.md` | 5,427 | Full wording of what S0.1 shortens; "Sun shadows" (4027-4202: contact shadows 4081-4105, the fit 4106-4160, costs 4162-4170), "Quality settings" (3512-, table 3518-3536, 3539-3543), "Lighting" (4252-4317), "GPU timing" (5190-5217), "Look-dev harness" (628-676), "Working with third-party code" (5365-5387: intro 5367-5368) |
| `ThirdPartyNotices.md` | 58 | Submodules' table (7-28), fonts (30-42), "## Sample assets" (44-) whose section `Samples.Content` reads up to the next heading of its level (`T/Samples/SampleContent.cpp:177`); compiled into the editor's About dialog (`Editor.Launcher` compares it, `T/Editor/EditorLauncherTests.cpp:589-591`; `Editor.UI.Text` parses it, `T/Editor/EditorUITextTests.cpp:74-92`) |
| `StrataEditor/src/Editor/ViewportRenderer.cpp` | 304 | The single-image renderer (`ViewportRenderer(debugName, fades)`, 124-135: `fades` false for `EditorViewport::GetCaptureRenderer`, `E/EditorViewport.cpp:343-348`, which `viewport.capture` and `ProjectThumbnail.cpp:117` use) |
| `Docs/Roadmap.md` | 1,487 | Status (wave 7's block 574-608, open items 609-982), "### Wave 7" (1221-1416), "### Later (wave 8 and beyond)" (1417-1487) |
| `Docs/Manual/Lighting.md`, `Graphics.md`, `Settings.md` | 297, 110, 133 | Contact shadows at 129, 134-139, 148, 151-152; 24, 32-38; 73-74, 82. The fit at Lighting.md 113-119 |
| `Docs/Architecture.md` | 4,149 | Contact shadows at 308 and 922 (the `GPU.ScreenShadows` row) |
| `.claude/skills/strata-editor-automation/SKILL.md` | 760 | 515 (`GPU.ScreenShadows` lists contact shadows) |
| `R/RendererQuality.h` / `.cpp` | 267 / 895 | `ContactShadows` (h:136-137), `c_SectionVersion` 4 (h:168); the row (cpp:43), traits (130), clamp (147-149, 455-456), section field (695-697), read (758), write (788) |
| `R/RendererComponents.h` / `.cpp` | 388 / 707 | `ContactShadowLength` 0.3 (h:34-35); its tooltip (cpp:290-292) |
| `R/ShadowFit.h` / `.cpp` | 178 / 455 | `ShadowFitView` (h:16-37), constants (h:89-107), `Hysteresis` (h:135-139), `Follow` (h:158; cpp:146-168), `GetFittedNearest` (cpp:170-201), `GetBoundTargets` (203-226), `Reproject` (228-271), `GetSideTarget` (273-326), `Update` (328-453) |
| `R/SceneRendererShadowFit.cpp` | 204 | `UpdateShadowFit` (23-108): the view (44-57), the readback (82-99), stats (101-107) |
| `R/SceneRenderer.h` / `.cpp` | 2,771 / 4,125 | `SceneRendererSpecification::FitShadowsToView` (h:241), `SceneRenderOptions::AdaptationSeconds` (h:421-424), stats `ShadowFit*` (h:673-683), `GetSpecification` (h:972), `UpdateShadowFit` (h:1675), fit members (h:2116-2135), `m_LastExposureTime` (h:2366); `RenderFrame` (cpp:2825-, `m_Stats = {}` at 2829), its `UpdateShadowFit` call (3066), `ReflectionParams` written (3019), AO then reflections (3339-3352), the scene layout's SRVs (about 338-362) |
| `E/EditorViewportCommands.cpp` | 1,333 | `viewport.capture`'s settle loop (740-790): `!ShadowFitSettled` counts as pending content (761), which resets the TAA history (773-774), at most `c_MaxCaptureShadowFitFrames` (12, :72) |
| `R/SceneRendererPost.cpp` | 1,358 | `UpdateExposure`'s clock (909-918): wall clock capped at 0.25 s, `AdaptationSeconds` overrides |
| `R/SceneRendererScreenShadows.cpp` | 244 | The contact march only with steps and a length (192-193) |
| `R/GPUProfiler.cpp` | 393 | `RendererGPUStages::Get` (44-68), registration order |
| `R/SceneRenderData.h` | 665 | `FrameConstants::ReflectionParams` (101) |
| `E/EditorStateCommands.cpp` | 1,094 | `DescribeRenderStats` (207-): `shadowFit` (235-236); `editor.perf`'s description (about 919-957, shadowFit at 930-932), `gameRenderStats` (972-975) |
| `E/StreamingView.h` | 173 | `ViewportDebugView` (26-41) and its names (47-48) |
| `SH/Scene/Forward.frag` | 452 | `ScatterDiffuse` (85-94), `SampleSpecularLight` (114-126), `ShadeLitSurface` (134-318): head (137-160), indirect (214-300), coat (302-318) |
| `SH/Include/Scene.glsl` | 396 | The slot comment (7-12), `ReflectionParams` (107) |
| `SH/Include/ReflectionProbes.glsl` | 125 | `ProbeLight` and `EvaluateReflectionProbes` under `ST_REFLECTION_PROBE_SAMPLING` (33, 87-94) |
| `T/Renderer/RendererQualityTests.cpp` | 775 | The preset table (40), `CheckColumn` (58), High against Medium (225-240) |
| `T/Renderer/GPUSceneContactShadowTests.cpp` | 434 | "Medium draws no contact shadows" (415-433) |
| `T/Renderer/ShadowFitTests.cpp` | 651 | `Renderer.ShadowFit`: helpers (17-170), the cases that change (263-314, 360-375) |
| `T/Renderer/GPUSceneShadowFitTests.cpp` | 503 | `GPU.SceneRenderer.ShadowFit`: helpers (24-107), nine cases (179-503) |
| `T/Renderer/SceneRendererTestUtils.h` | 536 | `RenderToImage` (325-332) |
| `T/Editor/GPUEditorViewportTests.cpp` | 2,691 | "A capture waits until the sun's cascades fit the view..." (1931-), `renderStats.shadowFit` (1947-1948) |
| `T/Perf/RendererPerfTests.cpp` | 1,098 | "Render cost of 100,000 cubes whose shadows cover a 4K view, at Medium and at High" (785-), the comment (814-815) |
| `T/Editor/EditorGraphicsQualityPageTests.cpp` | 502 | The rows' comment (165-167) |
| `StrataTests/Architecture/ComponentSchema.json` | 4,433 | The tooltip (566), written by `StrataTests --strata-test-helper=write-component-schema <file>` (`T/TestMain.cpp:236-242`) |
| `StrataTests/FeatureTest/FeatureTest.stproj` | 199 | `"ContactShadows": 16` (166): stays non-default |
| `Tools/LookDev.py`, `Tools/LookDev/Shots.json`, `Tools/PerfLock.py` | 1,013, 70, 179 | The harness (options 978-999: sizes 3840x2160 and 1920x1080, `taa,fxaa,off`, 5 repeats by default) and the lock (limit 25 minutes by default) |
| `.worktrees/create_worktree.sh` | 31 | A worktree on `agent/<name>` from a base, submodules from the main checkout's local copies (no network) |

### 2.2 Facts each item depends on (checked at 76f28a21)

- **The object store is damaged (found while writing this spec).** The loose object
  `22eb972393ac3e8364955b6c93ae7b456ec0871b` (790 bytes, read-only, written 2026-10-28 17:36) fails to inflate, and the tree
  `c16b4ec31243c98bc971d05bf4727a694d41ce65` is missing. Both are reachable from main: `git log -- <path>` and `git log -S` stop
  with "loose object ... is corrupt" at commit f99f3a41 (2026-10-28 08:01, "Check environment filtering at both prefiltered
  sizes"), and `git log --diff-filter=A -- Docs/Plans/Wave7/plan.md` with "unable to read tree (c16b4ec3...)". The tip's
  tree reads, so worktrees at 76f28a21 and pushes of new commits work; history walks, `git blame` across those commits,
  bisection, `git gc` and `git repack` do not. The workflow memory recorded "fsck fine" on 2026-11-01; whether origin
  (GitHub) holds sound copies is **unverified** (it must: main was pushed with this history on 2026-10-29 and CI built it).
- **`build/windows/bin/Release` is 0.7.0.** Its `StrataEditor.exe` and `StrataTests.exe` embed `76f28a21d961`
  (`ST_VERSION_COMMIT`, `CMake/StrataVersionCommit.cmake`), built 2026-10-29 07:44-07:49 right after the 07:42 commit. Its
  Debug tree is older (2026-10-27).
- **The slang audit's look-dev captures.** `slang/spike/lookdev/base2/` holds 84 pictures from `build/windows/bin/Release`
  ("Strata Editor 0.7.0", `run.json`, 2026-11-01 21:50): `--sizes 3840x2160 --aa taa,fxaa,off --repeat 2 --projects
  ThirdPerson,PhysicsPlayground,OpenWorld,LookDev`. `base3/` no longer exists: only `base3.log` and
  `compare_control/compare.json` remain, the latter recording 0 of 84 pictures differing from `base2`. Section 4.2 says what
  `base2` can serve for.
- **A kept 76f28a21 build must be a build of its own worktree.** `StrataTests` reads `Budgets.json`, samples and documents
  through `STRATA_SOURCE_DIR`, compiled in as the source tree's path (`StrataTests/CMakeLists.txt:164`,
  `T/Perf/PerfUtils.cpp:471`, `T/Editor/ProjectSamplesTests.cpp:72`): a copy of `build/windows/bin/Release` would read main's
  moving budgets and samples. `Tools/LookDev.py` copies the samples of its own checkout.
- **The contact march costs nothing at 0 steps** (`R/SceneRendererScreenShadows.cpp:192-193`). Custom values are written in
  full (`R/RendererQuality.cpp:788`), so no stored project changes; the section field's default follows High (697).
- **The fit's clock is readbacks.** `Follow` grows at once and shrinks after `c_ShrinkReadbacks` (8) readbacks
  (`R/ShadowFit.cpp:146-168`); near receivers above `c_NearOutlierShare` (1%) of the receiver pixels set the near bound
  (`:170-201`), which re-lays every slice (`:360-374`), and moved slices reset every side (`:386-405`). At 880 fps eight
  readbacks last 9 ms (shadow-jump audit).
- **One clock already exists.** `UpdateExposure` takes the wall clock between a renderer's frames, at most 0.25 s, and
  `SceneRenderOptions::AdaptationSeconds` overrides it (`R/SceneRendererPost.cpp:912-918`); only one test sets it
  (`T/Renderer/GPUScenePostProcessTests.cpp:797`).
- **Captures start the fit afresh.** `ResetShadowFit` on a capture's first frame starts an epoch whose first readback sets
  the layout at once (`R/ShadowFit.cpp:345-354`; DG 4119-4123), so a hold can only start after that, from content that
  changes while the capture renders, or from TAA: each jitter phase's readback sees the receivers through another
  sub-pixel offset, and in 0.7.0 a phase that asks for a quarter stop more grows a square at once (the TAA block of "A
  still camera keeps its fitted cascades and their cached layers", `T/Renderer/GPUSceneShadowFitTests.cpp:440-455`). A
  capture counts `!ShadowFitSettled` as pending content and resets its TAA history for it
  (`E/EditorViewportCommands.cpp:761-774`), so anything that makes the settled flag or the layout depend on how long the
  capture's frames take makes captures depend on the machine (review: 7.3a).
- **Tests change their own environment.** `Tests::ScopedEnvironmentVariable` (`T/TestHelpers.h:203`) is used by 23 files under
  `StrataTests/src`; DG "Test rules" (706-708) forbids only changing the process's environment for one child. (Review: S0.1's test
  bullet keeps that wording.)
- **The forward pass's indirect light** (`SH/Scene/Forward.frag:214-300`) has four forms: probes (267-281), screen-space
  reflections without probes (282-289), the sky alone, whose sum is scaled by `EnvironmentParams.x` (290-294), and the
  constant ambient alone (296-300). `EvaluateReflectionProbes` returns irradiance and radiance in one call (236); `sky = 1 -
  probes.Weight` scales both (270-272); `(1.0 - specularColor)` enters the diffuse term (275, 285, 292); the specular
  occlusion derives from `diffuseOcclusion` (265-266).
- **Slots.** The scene layout binds SRV 0-21, 27, 28, 29, 32, 33 (`R/SceneRenderer.cpp:338-362`); the comment
  (`SH/Include/Scene.glsl:7-12`) reserves 30-31 and 34 from wave 7 and names 35 next free. `ReflectionParams.yzw` are
  written 0 (`R/SceneRenderer.cpp:3019`) and read nowhere.
- **GPU stages** register in frame order (`R/GPUProfiler.cpp:47-67`): `GPU.AO` then `GPU.Reflections`; no
  `GPU.Volumetric` exists (wave 7's was below its cut).

## 3. Order, lanes and the cut line

| Order | Item | Where | Blocks |
| --- | --- | --- | --- |
| 1 | S0.0a: the object store repaired (4.1) | main checkout's `.git`, by the orchestrator | Runs before worktrees are made, so no agent's git runs during it; if it cannot run, nothing waits (4.1, review) |
| 2 | S0.0b: the 0.7.0 baseline (4.3) | `build/windows/bin/Release`, alone on the machine | Any rebuild of `build/windows` |
| 3 | S0.0c: `.worktrees/wave8-base` built and checked (4.4) | its own worktree | Every A/B of the wave |
| 4 | S0.0d + S0.1: plan committed, room in AGENTS.md (4.5, section 5) | `.worktrees/wave8-first` | Every lane |
| 5 | The four lanes in parallel: Render (S0.2, S0.3), Editor, Platform, Slang | section 10 | |
| 6 | S0.11, then S0.12 (sections 8-9) | `.worktrees/wave8-last` after lane Slang lands (or its box ends) | `reflections`, `radiance-cascades` |

S0.0d and S0.1 can be written while S0.0b captures (writing loads nothing); their one build (`StrataTests`, 4.5) waits until
the capture ends. In lane Render, S0.2 goes first (one commit with its tests and docs), then S0.3 (section 7.9's commits).

**Cut line.** Everything above lands before the streams that need it start. Below the cut, in this order, may follow on main
while streams run: S0.0b's `--live` shots and its Medium set (both can be taken later from `.worktrees/wave8-base`, which 4.4
proves equal to `build/windows`), and S0.12's Status paragraph if lane Slang is still open (its text waits for the lanes'
results). Nothing of S0.1, S0.2, S0.3 or S0.11 is below the cut.

## 4. S0.0 Baselines, the kept base, the plan committed

### 4.1 The object store first (S0.0a)

The orchestrator does this before `create_worktree.sh` runs for anything (a worktree shares the object store). It changes no
tracked file and keeps the damaged object as a backup:

1. `git -C G:/GameEngineAI fsck --full --no-dangling > .worktrees/wave8-plan/fsck-before.log 2>&1` (read-only; slow on
   exFAT). The two objects of 2.2 are expected; anything else is listed in the step 0 report.
2. `git clone --mirror https://github.com/2042Third/GameEngine.git G:/GameEngineAI/.worktrees/wave8-plan/mirror.git` and, for
   every object fsck named, `git -C <mirror> cat-file -t <oid>` must succeed. If origin lacks one, stop and report: nothing
   below runs, and the wave's history work (bisection, `git log -S`) is limited until the owner decides.
3. Move (never delete) the damaged loose file: `mkdir -p .worktrees/wave8-plan/corrupt-objects && mv
   .git/objects/22/eb972393ac3e8364955b6c93ae7b456ec0871b .worktrees/wave8-plan/corrupt-objects/`.
4. Append the mirror's object directory to `.git/objects/info/alternates`
   (`G:/GameEngineAI/.worktrees/wave8-plan/mirror.git/objects`), run `git fsck --full --no-dangling` (clean), then `git repack
   -a -d` without `-l`, which copies the objects reachable through the alternate into a local pack; remove the alternates
   line; `git fsck --full --no-dangling > .worktrees/wave8-plan/fsck-after.log` is clean and `git log --oneline --
   Strata/src/Strata/Renderer/RendererQuality.cpp` lists its history again.
5. The step 0 report names the objects, the commit they belong to, and that the repair came from origin.

Review corrections to 4.1 (they win over the steps above):

- Before step 4, copy the packs (`cp -r .git/objects/pack .worktrees/wave8-plan/pack-backup`; 42.6 MB packed, 2,643 loose
  objects on 2026-11-02) so the repack can be undone.
- Step 4 runs `git repack -A -d` (not `-a -d`): with `-a -d`, objects that are unreachable but packed (abandoned agent
  commits no reflog holds) are dropped at once; `-A` leaves them loose for the usual expiry. Still without `-l`, so the
  objects reachable through the alternate are copied.
- No `git` process may run during steps 3-4 (no agent, no editor's script build, no IDE): the orchestrator checks
  `tasklist | grep -i git` first.
- "Nothing below runs" in step 2 means steps 3-5 only. If the mirror cannot be cloned (no network) or lacks an object, the
  wave proceeds unrepaired: the tip's tree reads, worktrees at 76f28a21 and later work, and commits and pushes send only
  new objects. Agents then avoid `git log -- <path>`, `git log -S`, `git blame` and `git gc` across f99f3a41 (they fail),
  and the step 0 report says so. The repair blocks nothing else.

### 4.2 What the slang audit's captures can serve for

`base2` is a sound record of 0.7.0's pixels at 3840x2160 for four of the five projects: the editor that made it is
`build/windows/bin/Release` (2.2), its work copies came from 76f28a21's samples, and the control `base3` matched it in all 84
pictures before `base3` was removed. It serves for:

- lane Slang's gate 3, as its glslang reference (it is what the audit compared against), and
- the check that `.worktrees/wave8-base` reproduces 0.7.0 (4.4, step 5).

It cannot be the 0.7.0 baseline: it has no 1920x1080 pictures, no `Basic3D` shots (`basic3d.scene`, `basic3d.editor`,
`basic3d.view.luminance`), no live shots (20 in `Shots.json`), 2 repeats instead of 5, and its `stages.csv` was timed while
other auditors and the slang spike's editors ran, so its GPU times are not a base. `base3` cannot serve for anything.

### 4.3 The 0.7.0 baseline (S0.0b)

Before anything rebuilds `build/windows`, with no other agent building or measuring (`python Tools/PerfLock.py --status`
names no holder), from the main checkout at 76f28a21 (whose `Tools/LookDev.py`, `Shots.json` and samples are 0.7.0's):

```sh
cd /g/GameEngineAI
PY=/c/msys64/mingw64/bin/python.exe
L=G:/GameEngineAI/.worktrees/wave8-plan/lookdev
$PY -I Tools/PerfLock.py --label lookdev-0.7.0 --limit 150 -- $PY -I Tools/LookDev.py capture \
  --build G:/GameEngineAI/build/windows/bin/Release --label 0.7.0 --output $L/0.7.0 --work $L/work --live
$PY -I Tools/PerfLock.py --label lookdev-0.7.0-medium --limit 90 -- $PY -I Tools/LookDev.py capture \
  --build G:/GameEngineAI/build/windows/bin/Release --label 0.7.0-medium --quality medium --repeat 2 \
  --output $L/0.7.0-medium --work $L/work
```

- `0.7.0`: every shot of every project, both sizes, all three modes, 5 repeats, live shots (windowed editors open on the
  desktop for them). Expected about 180 stills of 5 repeats and 20 live shots; at about 2 s a capture (base2's run took 6
  minutes for 168 captures) 45-70 minutes with the first import of the samples' copies (unmeasured). It is the "0.7.0" of the
  owner's sheets (plan 7 step 6) and the GPU busy column of plan 1.1's last two rows.
- `0.7.0-medium`: stills at Medium, 2 repeats: S0.3's "settled layouts unchanged" check (7.8) and the landings' Medium check.
- Each run's `stats/` must show self-differences of 0 except where the driver repeats are known to differ by one code value
  (wave 7's as-built: the Open World's 4K TAA shots); a shot that failed is captured again alone with `--shots`.
- Then `$PY -I Tools/LookDev.py compare <base2> $L/0.7.0 --output $L/compare-base2` (information): 0 differing pictures among
  those both have is expected; any difference is recorded in the step 0 report with its size.

DG "Look-dev harness"'s Baselines line (676) gains, in S0.0d's commit: "Strata 0.7.0's set (76f28a21's Release build,
every shot, both sizes, all modes, 5 repeats, live) lies in `.worktrees/wave8-plan/lookdev/0.7.0/`, its Medium stills in
`0.7.0-medium/`; `.worktrees/wave8-base` keeps that build for interleaved A/Bs ("Perf tests")."

### 4.4 The kept base: `.worktrees/wave8-base` (S0.0c)

The cheapest base that measures like 0.7.0 is a Release-only build of a 76f28a21 worktree (a copy of the owner's binaries
would read main's budgets and samples, 2.2):

1. After 4.1 and 4.3, and before lane Slang removes the glslang submodule (`create_worktree.sh` takes submodules from the
   main checkout's copies): `sh .worktrees/create_worktree.sh wave8-base 76f28a21` (branch `agent/wave8-base`, never committed
   to, never pushed).
2. `cd .worktrees/wave8-base && cmake --preset windows` (the preset's binary directory is the worktree's `build/windows`).
3. `cmd //c start "" //belownormal //wait cmake --build build/windows --config Release --parallel`: every target in Release,
   no Debug. The shadow-jump audit's private Release build of the same tree took about 5 minutes by its files' times
   (**unverified**); the tree's size is **unverified** (several GB; G: had 997 GB free on 2026-11-01).
4. Checks: `build/windows/bin/Release/StrataEditor.exe --version` reports 0.7.0, and the binary holds `76f28a21d961`
   (`grep -c 76f28a21d961`).
5. The 84-picture set with this build, from this worktree's tool: `$PY -I .worktrees/wave8-base/Tools/LookDev.py capture
   --build .worktrees/wave8-base/build/windows/bin/Release --label wave8-base --sizes 3840x2160 --aa taa,fxaa,off --repeat 2
   --projects ThirdPerson,PhysicsPlayground,OpenWorld,LookDev --output $L/wave8-base-84 --work $L/work-base` under
   `PerfLock.py`, then `compare` against `base2` and against `$L/0.7.0`'s 4K pictures: 0 differ, or only pictures whose own
   repeats differ, by one code value. Anything else means the base is not 0.7.0: it is investigated before any A/B uses it.

Rules for the whole wave: nobody runs cmake in `.worktrees/wave8-base` again or edits it; measuring agents use it under
`PerfLock.py` (which serializes them, so its `PerfResults/` never sees two runs); it is removed only after 0.8.0 ships. The
showcase's sample agent may run its editor for asset and generator work until main's code first differs from 76f28a21 (lane
Render's landing), which saves a build slot.

### 4.5 The plan committed (S0.0d)

In `.worktrees/wave8-first` (`sh .worktrees/create_worktree.sh wave8-first main`, after 4.1), one docs-only commit, "Commit
wave 8's plan, specs and owner's list":

- `Docs/Plans/Wave8/plan.md`: `.worktrees/wave8-plan/plan.md` as reviewed, with one line under its title: "Records cited as
  `.worktrees/wave8-plan/...` (audits, captures, prototypes) are on the reference machine, outside the repository."
- `Docs/Plans/Wave8/specs/decisions.md` and every spec of `.worktrees/wave8-plan/specs/` with its review corrections, this one
  included (specs whose reviews are not done yet follow in a later commit; nothing waits for them).
- `Docs/Plans/Wave8/owner_feedback.md`: the owner's list, verbatim (the plan cites it).
- `Docs/Plans/Wave8/ReleaseNotes.md`: "# Wave 8: release notes for 0.8.0", one paragraph saying `Docs/Releases/0.8.0.md` is
  written from it, and the headings "## What's new", "## Defaults that change" (a table: What, 0.7.0, 0.8.0, Where: plan
  section 6 names these as release-note rows, as wave 7's notes had them), "## Upgrading from 0.7.0" (a table: What, 0.7.0,
  0.8.0, To keep the old), "## Known issues". Each landing appends a block of its own under each heading (bold lane or
  stream name first), so merges only add.
- DG "Look-dev harness" (4.3's line).
- `Docs/Plans/Wave7/` stays: wave 8's plan cites its specs (plan 4.4 step 5).

S0.1's two commits (section 5 and 5.5) follow on the same branch; the three land together (section 10).

### 4.6 Acceptance

- `fsck-after.log` clean; history walks work (4.1 step 4).
- `$L/0.7.0` complete (`run.json` lists no failed shot; `index.html` opens), `$L/0.7.0-medium` likewise, captured alone
  under the lock.
- `.worktrees/wave8-base` reports 0.7.0 and reproduces `base2` and `$L/0.7.0` at 4K (4.4 step 5).
- `Docs/Plans/Wave8/` on main with the plan, the specs, the owner's list and the release notes' skeleton.

## 5. S0.1 Room in AGENTS.md

Checklist item 5: a rule AGENTS.md shortens is stated in full in the guide (change both), a rule kept whole is only in
AGENTS.md. Each bullet below keeps its musts and gains or keeps its link; what it drops is already in the guide section
named (5.3 gives the line) or moves there with the exact text of 5.2.

### 5.1 The count

Counted with the test's rule (UTF-8 code points, "\r" not counted, `T/Docs/AgentGuideTests.cpp:94-106`) on 76f28a21's
file: 39,292 code points, 708 free. Sixteen bullets shortened save 2,633 (review: two replacements below were corrected,
costing 115 of the author's 2,748), and golden rule 1's rewrite (5.5) adds 177: **36,836 after S0.1, 3,164 free** (the
plan asks for at least 3,000).

| AGENTS.md (76f28a21) | Before | After | Saves | Full wording in DG |
| --- | --- | --- | --- | --- |
| Testing, `GPU*` suites (105-109) | 527 | 329 | 198 | "GPU tests" (411-427) plus 5.2 (a) |
| Testing, test rules (117-121) | 524 | 397 | 127 | "Test rules and helper processes" (684-714) |
| Code style, headers (178-181) | 375 | 221 | 154 | "Precompiled headers" (209-240) |
| Code style, profiling (184-187) | 395 | 227 | 168 | "Code style", Frame stages (827-831) plus 5.2 (b) |
| Architecture, threading (194-198) | 504 | 275 | 229 | "Architecture rules", Threading (838-852) |
| Architecture, NVRHI pitfalls (208-212) | 429 | 225 | 204 | "Architecture rules", NVRHI pitfalls (853-879) |
| Architecture, assets (213-216) | 442 | 266 | 176 | "Architecture rules", Assets (880-885) |
| Scripting, save files (290-293) | 429 | 259 | 170 | "Gameplay API" (1672-1700) |
| Scripting, graphics options (295-297) | 340 | 256 | 84 | "Quality settings" (3572-3644) |
| Game UI, input (338-341) | 359 | 237 | 122 | "UI input" (2337-2385) |
| Editor, saves (360-363) | 421 | 271 | 150 | "No silent data loss" (2582-2595) plus 5.2 (c) |
| Editor, UI rules (379-382) | 384 | 301 | 83 | "Editor UI rules" (2394-2481), "Shortcuts and editor status" (2990-2995) |
| Rendering, post effects (406-410) | 490 | 235 | 255 | "Post-processing" (4445-4470) |
| Rendering, lighting (411-415) | 528 | 291 | 237 | "Lighting" (4252-4290), "Motion vectors and TAA" (3898) |
| Rendering, shadow caches and both paths (433-437) | 599 | 449 | 150 | "GPU-driven path" (3746-3820) |
| Automation, commands (445-448) | 402 | 276 | 126 | "Commands and requests" (5236-5248), "MCP toolsets", `editor.status` (2996-) |
| **Total** | | | **2,633** | |

The Architecture rules section's opening sentence ("states every architecture rule, seven of them in short", DG 835-836)
stays true: Threading, NVRHI pitfalls and Assets were already three of its seven.

The replacements, exactly, in file order (each replaces the whole bullet at the lines of the table):

```markdown
- `GPU*` suites (label `gpu`) share one device through `Tests::GPUContext` (never create devices in tests) and end with
  `CHECK(gpu.GetNewErrorCount() == 0)`; exact-pixel tests render with `Tests::ExactSpecification()` and
  `AddNeutralPostProcess` ([GPU tests](Docs/DeveloperGuide.md#gpu-tests): residency, text, drawn counts).
- Tests follow the transform contract, keep files in `Tests::CreateTemporaryDirectory()`, copy samples
  (`Tests::CopySampleProject`), never set their environment for a child, depend on no program outside the build and wait
  by the clock or a condition, never frames ([Test rules](Docs/DeveloperGuide.md#test-rules-and-helper-processes)).
  Check that a new regression test fails without its fix.
- Headers are self-contained and sources include what they use; precompiled headers hold only the standard library,
  vendor headers and the Core basics ([Precompiled headers](Docs/DeveloperGuide.md#precompiled-headers)).
- Profile hot paths with `ST_PROFILE_FUNCTION()` / `ST_PROFILE_SCOPE("Name")` (Tracy); a unit of the main thread's frame
  work runs in a `ST_PROFILE_STAGE("Stage.Name")` scope ([Code style](Docs/DeveloperGuide.md#code-style)).
- **Threading:** the main thread owns the scene, input, scripts and rendering submission; background work goes through
  `JobSystem`, and results return through `Application::SubmitToMainThread` or polled queues, never callbacks that touch
  engine state from worker threads.
- **NVRHI pitfalls:** never take the address of a handle (`&m_Pipeline`); dispatches through one binding set that read
  each other's UAV writes need explicit barriers; drop render targets through `Renderer::ReleaseDeferred`.
- **Assets:** referenced by `AssetHandle` (UUID), never by path at runtime; code handles "not loaded yet" every frame
  instead of blocking, and code that works for a scene reaches assets through `Scene::GetAssetManager()`, never
  `AssetManager`'s static functions.
- Save files go through `IGameSaves` and the players' bindings only through `InputActions`; gameplay and UI read
  rebindable controls as input actions; in-process game loops provide the game's services
  ([Gameplay API](Docs/DeveloperGuide.md#gameplay-api)).
- Players' graphics options go through `IGameGraphics` (in-process game loops provide it), only `GameGraphicsService`
  touches `GraphicsOptions.json`, and every quality row has a traits line
  ([Quality settings](Docs/DeveloperGuide.md#quality-settings)).
- UI input belongs to the "UI" scene system: widgets write with `registry.patch` and report through `IUIEvents`, and
  world-space elements are hit through the camera's ray (`GameUIPointer`) ([UI input](Docs/DeveloperGuide.md#ui-input)).
- Saves never serialize on the main thread (`Editor/DocumentSaver`, `SettingsFileWriter`); commands that replace the
  scene, close the project or quit wrap their handler in `CommandUtils::AfterSaves`
  ([No silent data loss](Docs/DeveloperGuide.md#no-silent-data-loss)).
- UI code takes colors only from theme tokens, sizes relative to the font, panels through `EditorPanelRegistry`, the
  editor's fonts and the widget kit ([Editor UI rules](Docs/DeveloperGuide.md#editor-ui-rules); selection shortcuts:
  [Shortcuts](Docs/DeveloperGuide.md#shortcuts-and-editor-status)).
- Post effects run in `Renderer/SceneRendererPost.cpp` between the TAA resolve and the tone mapping, add no
  `FrameConstants` fields and are off in `AddNeutralPostProcess`
  ([Post-processing](Docs/DeveloperGuide.md#post-processing)).
- Light keeps its energy and needs no history: specular is compensated for multiple scattering (`GetSpecularEnergy`)
  and screen-space lighting denoises within the frame; check lighting changes with the debug views and the white-furnace
  test ([Lighting](Docs/DeveloperGuide.md#lighting)).
- A new input of how casters are drawn changes a version the shadow caches compare (`PrepareShadowCache`,
  `FindLocalShadowCasters`). What is drawn and requested is decided by the CPU path and `Cull.comp`: change both, keep
  shading independent of draw order (derivatives only of the primitive's own interpolants and textures) and
  `GPU.SceneRenderer.GPUDriven` and `.Feedback` passing ([GPU-driven path](Docs/DeveloperGuide.md#gpu-driven-path)).
- Commands are the API: every registered command is a method and an MCP tool of its group's toolset (`core` stays
  within 32 tools and 30 KiB: [MCP toolsets](Docs/DeveloperGuide.md#mcp-toolsets)), so a feature reaches agents by
  registering a command; no RPC code is needed.
```

### 5.2 What moves into the guide

(a) DG "GPU tests" (after the bullet ending at 426, "...reads the Dock icon back by drawing it."), a new bullet:

```markdown
- Exact-pixel tests render with `Tests::ExactSpecification()` (no texture streaming, no dither) and `AddNeutralPostProcess`
  ([Readback, the game renderer and tests](#readback-the-game-renderer-and-tests)); tests of whole-asset residency call
  `AssetManagerBase::SetPartialLoads(false)` ([Mip streaming](#mip-streaming)); tests of text with many distinct glyphs render
  until no glyph is pending ([Text and fonts](#text-and-fonts)).
```

(b) DG "Code style", Frame stages bullet (827-831), appended: "Hot paths are profiled with `ST_PROFILE_FUNCTION()` and
`ST_PROFILE_SCOPE("Name")` (Tracy). Scene systems name their stage in `SceneSystemDescriptor::ProfileStage`, in whose scope
the scene runs their callbacks." And the section's opening sentence (819-820) becomes: "AGENTS.md, [Code style](...),
states the code style, the precompiled headers' rule in short ([Precompiled headers](#precompiled-headers) has it in full),
the profiling rule in short (Frame stages, below) and three conventions whose details are these:".

(c) DG "No silent data loss", Saves bullet (2582-2583): "the project file goes through `Editor/SettingsFileWriter`
([Project settings](#project-settings))" becomes "settings files (the project file, the preferences) go through
`Editor/SettingsFileWriter` (a snapshot taken in the frame, the writing on a background job; [Project settings](#project-settings))".
`E/EditorPreferences.cpp` writes through it (checked: it includes and uses `SettingsFileWriter`).

(d) DG "Precompiled headers", the bullet at 222: "Sources and headers still include what they use" gains "(a header
self-contained: `Strata/Core/Assert.h` for `ST_CORE_ASSERT`, though the precompiled header holds it)".

### 5.3 What each shortened bullet leaves to the guide, and where it is

The implementer checks each against the guide's text before the commit and moves any phrase the guide lacks into the section
named:

- GPU suites: "validation errors fail them" (DG 413-415), `GetDrawnCounts` and frames later (418-419), the rest by 5.2 (a).
- Test rules: "never the source tree" (697), the test helper's stand-ins (703-714), CI's slowest machines and job pools
  (688-690). The child's environment stays in AGENTS.md as the rule is (706-708 has its full wording); "never change their
  own environment" would contradict `Tests::ScopedEnvironmentVariable`'s 23 using files (2.2).
- Headers: `<nlohmann/json_fwd.hpp>` (225-229, "Headers stay light"); the example `Strata/Core/Assert.h` for
  `ST_CORE_ASSERT` moves by 5.2 (d).
- Profiling: "not around calls that take nanoseconds" (829-831), the rest by 5.2 (b).
- Threading: `Submit`, `SubmitIO`, `JobPriority::Background`, a waiting thread runs its job (838-855).
- NVRHI: `setBufferState`/`setTextureState(..., UnorderedAccess)` with `commitBarriers()` (857-858), clears and resolves
  not kept alive (859-863).
- Assets: "(scene systems, loading into it, rendering it)" and the active manager (884-889).
- Save files: `SaveFiles::ValidateName`, atomic writes, `IGameWindow`, `IProjectSettings`, `InputActions::Update()` after the
  frame's input (1690-1700); rebindable controls and the bindings file (1672-1677).
- Graphics options: the project's, then the player's, then `--quality` (3572-3576); `FollowsPreset` (3603-3612).
- UI input: `Input::GetPointerState` (2348), `GetKeyState` (2368), dividing pixels by a canvas's scale (2377).
- Editor saves: "UI code saves through the command runner" (2592-2593), the rest by 5.2 (c).
- UI rules: 16 pixels (2405), `ImGui::Begin` (2413), `UI/Widgets.h` (2426), `ImGuiInputFlags_RouteFocused` (2993-2995).
- Post effects: push constants or own buffers, reading `FrameConstants` (4463-4464), the post source (4457-4458), `DitherOutput`
  through `ExactSpecification` (4460-4461), generated constants only from their generators (4461-4462).
- Lighting: the BRDF table and diffuse taking what specular leaves (4254-4262), the debug views by name (4285-), the
  derivative rule (4274-4279; kept in AGENTS.md's shadow-cache bullet).
- Shadow caches and both paths: "Shadow views are kept while nothing reaching them changed", `SceneRendererExtraction.cpp`
  and visibility feedback (3746-3820). Draw order and the two named suites stay in AGENTS.md (review): most of this wave's
  streams change `Cull.comp`, the extraction or the shadow caches, and those suites are what catches a one-sided change.
- Commands: IDs as 16-digit hex, camelCase keys (5241-5245); `editor.status` compact (2996-).

### 5.4 The budget after S0.1

3,164 code points free. Step 0's own lanes take about 830 (review: the author left out lane Editor): lane Slang about 450
(unverified: its spec is not written; the Shaders bullet in Slang's words, its rules of plan 4.1 and D1's sentence in
"Working with third-party code"), lane Platform 201 (`step0-platform.md` section 9: `ExternalOwners` in the Layers bullet,
`Scene.Timing` beside physics tuning), lane Editor 179 (`step0-editor.md` section 7: `asset.place`). Lane Render, S0.11 and
S0.12 add nothing to AGENTS.md (their rules are details: DG). That leaves about 2,330 when the streams branch: nine streams
of at most 250 each (plan 4.1) take 2,250, a margin of about 80 if lane Slang takes 450. A lander whose bullet would pass
40,000 shortens a bullet of its own area into the guide in the same commit (checklist item 5), never another area's; the
later lander of two measures with `Docs.AgentGuide` after merging main.

Commits, in this order: "Make room in AGENTS.md: the guide states sixteen rules in full" (body: the table of 5.1, the
counts before and after, and 5.2's four moves), then 5.5's. **Acceptance:** 36,836 code points after both;
`Docs.AgentGuide` passes (its link check finds every anchor: `gpu-tests`, `test-rules-and-helper-processes`,
`precompiled-headers`, `code-style`, `gameplay-api`, `quality-settings`, `ui-input`, `no-silent-data-loss`,
`editor-ui-rules`, `shortcuts-and-editor-status`, `post-processing`, `lighting`, `gpu-driven-path`, `mcp-toolsets`,
`working-with-third-party-code`, and in the guide `readback-the-game-renderer-and-tests`, `mip-streaming`,
`text-and-fonts`).

### 5.5 Golden rule 1 as the owner scoped it (D10; added by the review)

The plan (S0.1) and the rulings (decisions.md, D10) give this to S0.1; the author's draft left it out. One commit after 5.1's,
"Let golden rule 1 forbid other folders of this computer, not open source" (body: the owner's words of 2026-11-01, verbatim
from decisions.md, and D10).

AGENTS.md lines 19-21 (214 code points) become (391):

```markdown
1. **Never read or use other folders of the machine you work on** (this repository, open-source projects fetched for
   the work and your own scratch directories are not "other"). Open-source code is used only under a permissive license,
   with its notice kept and its source named; copyleft code only for ideas
   ([Third-party code](Docs/DeveloperGuide.md#working-with-third-party-code)).
```

The rule is shortened, so DG states it in full (checklist item 5). DG "Working with third-party code": the intro (5367-5368)
becomes "AGENTS.md, [Working with third-party code](../AGENTS.md#working-with-third-party-code), says how submodules are
upgraded and states the rule for the editor's fonts in short, and its golden rule 1 the rule for code from other projects.
In full:", and a first paragraph follows it:

```markdown
**Code from other projects** (golden rule 1; the owner's ruling of 2026-11-01): nothing is read or used from other folders
of the machine an agent works on (other checkouts, other projects, personal files); this repository, the open-source
projects an agent fetches for the work and its own scratch directories are not other folders. Open-source projects, their
documentation, papers and talks are read as much as helps. Their code is copied, ported or adapted only under a permissive
license (MIT, BSD, Apache-2.0, zlib, Boost, public domain or CC0): `ThirdPartyNotices.md`'s "Code derived from other
projects" gains a row (the project with its link, the version or commit, the license, the Strata files that hold the
derived code, what was derived) and the license's full text below its table, since games ship that file and no submodule
carries the text, and the commit message names the project, its commit and its files. Copyleft code (GPL, LGPL, AGPL, MPL
and the like) is read for ideas, never copied or ported: Strata's license is the owner's choice, and copied copyleft code
would make it. Large pieces still come in as pinned submodules under `Strata/vendor/` by the rules of AGENTS.md's
[Working with third-party code](../AGENTS.md#working-with-third-party-code); engine code adapted from a submodule follows
the notice rule above.
```

`ThirdPartyNotices.md`, a new section after the Roboto paragraph (42) and before "## Sample assets" (44), so that
`Samples.Content`'s section still starts and ends where it did:

```markdown
## Code derived from other projects

Code that Strata copied, ported or adapted from open-source projects that are not submodules, each under its permissive
license (Docs/DeveloperGuide.md, "Working with third-party code"). Each project gets a row of a table here (project,
version, license, Strata's files, what was derived) and its license's full text below the table. There are no entries yet.
```

The editor embeds this file (About dialog), so the commit's gates add `Editor.UI.Text`, `Editor.Launcher` and
`Samples.Content` to `Docs*` (10.2). No release-note line (no engine behavior changes); `animation-graph`, the first stream
expected to port code (Esoterica, MIT), adds the first row.

## 6. S0.2 Contact shadows off (lane Render)

### 6.1 Code

- `R/RendererQuality.cpp:43`: `constexpr PresetRow<uint32_t> c_ContactShadowsRow = { 0, 0, 0, 0 };` with the comment above
  it: "// Off in every preset: binary hits in a thin shell under screen-locked noise shimmer in motion (DG "Sun shadows");
  Custom turns them on." The row stays (Custom 0 or 4-32), and so do its traits line (130), `Clamp` (455-456), the read (758)
  and write (788).
- The section field (695-697), player-facing, without versions: "Steps of the sun's contact shadows, which ground small
  objects below the shadow map's detail: more steps find thinner contacts for more GPU time, but they can shimmer in
  motion, so no preset turns them on (0: off; 1 to 3 count as 4)." Its default stays High's value, now 0.
- No `c_SectionVersion` bump (h:168): every section 0.7.0 wrote stores the field (788), so a stored Custom 12 stays 12. Only
  a Custom section written before the field existed (a development build between wave 7's step 0 and `sun-shadows`) now
  reads 0 instead of 12: accepted, said in the release notes.
- `R/RendererQuality.h:136`: "// Steps of the sun's screen-space contact shadows (0 off; else 4 to 32), which ground objects
  below the cascades' detail. 0 in every preset (they shimmer in motion); Custom turns them on."
- `DirectionalLightComponent::ContactShadowLength` stays 0.3 (`R/RendererComponents.h:35`), so a Custom row of steps works at
  once. The header comment (34-35): "(when the quality's RendererQualitySettings::ContactShadows has steps: Custom only)".
- The tooltip (`R/RendererComponents.cpp:291-292`): "Meters toward the light over which contact shadows ground objects
  smaller than the shadow cascades' texels, when Graphics Quality's Contact Shadows has steps (Custom; no preset draws them);
  0 off". `ComponentSchema.json:566` gets the same text: regenerate with `StrataTests.exe
  --strata-test-helper=write-component-schema StrataTests/Architecture/ComponentSchema.json` and check that the diff is that
  one line.
- No shader changes; `SH/Effects/ContactShadows.frag` stays for Custom.
- Samples: `ContactShadowLength` 0.3 is stored in `ThirdPerson/Assets/Scenes/Course.stscene` and `MainMenu.stscene`,
  `PhysicsPlayground/Assets/Scenes/Main.stscene` and `OpenWorld/Assets/Worlds/Island.stscene` and stays (inert without
  steps); no sample or template stores `Renderer.Quality`'s `ContactShadows`. Their pictures are retaken by the showcase's
  and the streams' adoption passes, not for this; the samples' command files (`Editor/<Sample>Commands.json.in`) do not
  name contact shadows and keep passing.

### 6.2 Tests

| Test | Change | Without the change |
| --- | --- | --- |
| `Renderer.Quality`, the preset table (`T/Renderer/RendererQualityTests.cpp:40`) | `c_ContactShadows = { 0, 0, 0, 0 }`; `CheckColumn` (58) unchanged | Fails at High and Ultra |
| `Renderer.Quality`, High against Medium (225-240) | `CHECK(high.ContactShadows == 0)` and `CHECK(medium.ContactShadows == 0)` replace 238-240 (no assignment needed); the comment (225-226) drops contact shadows from what differs | Fails |
| `GPU.SceneRenderer.ContactShadows`, "Medium draws no contact shadows" (`T/Renderer/GPUSceneContactShadowTests.cpp:415`) | Renamed "No preset draws contact shadows": the same plate and camera, for each of Low, Medium, High and Ultra a renderer with `Quality = RendererQuality::GetPreset(preset)`, `TextureStreaming` false, `FitShadowsToView` false; the picture with `ContactShadowLength` 0.3 equals the one with 0. Then a Custom quality with `ContactShadows = 12` differs (the row still works). Each preset gets its own renderer; if a preset's two frames differ with the length unchanged (a previous-frame input such as High's spatial reflections, if the plate scene asks for them: unverified), each frame is drawn by a fresh renderer of that preset instead | Fails at High and Ultra (12 and 16 steps draw them) |
| `Reflection.ComponentSchema` | The JSON's tooltip line | Fails until regenerated |
| Kept as they are | The other contact cases (they set steps), `GPUSceneRendererGPUDrivenTests.cpp:95` (explicit 12), `FeatureTest.stproj:166` (Custom 16, non-default), `Feature.stscene`, the Reflection, Serialization and EditorCommand cases | |
| Comments | `T/Perf/RendererPerfTests.cpp:814-815`: "(the mask, its blur and the cascades' fit to the view; no preset draws contact shadows)"; `T/Editor/EditorGraphicsQualityPageTests.cpp:165` stays (the row exists) | |

### 6.3 Docs

- DG 3527 (quality table): `| ContactShadows (steps of the sun's contact shadows; 0 off) | 0 | 0 | 0 | 0 |`; 3541: drop
  `ContactShadows` from the rows that cost at every size.
- DG 4084: "(0 in every preset since 0.8.0: whether a step lands in the thin shell is close to chance and the noise is
  locked to the screen, so they shimmer in motion; Custom 0 or 4-32)". 4081: "ground what lies below the cascades' texels
  when a Custom quality asks". 4165-4168: the costs are 0.7.0's High, "which drew them".
- `Docs/Manual/Graphics.md:24`: `| Contact shadow steps | Off | Off | Off | Off |`; 32-33: High "adds smoother wide sun
  penumbrae at every size, and sharper sun shadows from 1440 rows on"; 35-38: contact shadows "(Custom only: no preset
  turns them on, because they can shimmer in motion)".
- `Docs/Manual/Lighting.md:129`: "Near the camera, Contact Shadow Length grounds such small things when the Graphics
  Quality's Custom Contact Shadows has steps (no preset draws them: they can shimmer in motion)"; 134-139: "at the High and
  Ultra graphics quality" becomes "with Custom Contact Shadows"; 148: `| Contact Shadows (steps; off in every preset) | off |
  off | off | off |`; 151-152: "Contact shadows, when Custom turns them on, cost ...".
- `Docs/Manual/Settings.md:73-74`: Medium's "no contact shadows" and High's "contact shadows at every size" drop; 82 stays.
- `Docs/Architecture.md:308, 922` and the skill's `SKILL.md:515`: "contact shadows when a Custom quality draws them".
- `Docs/Roadmap.md`: S0.12 moves the open item at 1412 to "Later" and corrects 1278 (section 9).

**The 5 cm plate.** Wave 7's acceptance (`Roadmap.md:1276-1278`, "a solid contact shadow") now rests on the fitted cascades.
The implementer looks at `lookdev.contact`'s `plate` crop of `$L/0.7.0` against the lane's capture (both sizes, all modes,
the 84-set of 8.5 plus 1920x1080) and writes in the lane's report whether the strip stays solid; S0.12 states it in the
roadmap. The audit's own framing found off "a clean solid strip" and on "a wrong wedge" (`shadow-jump/captures/plate2/`).

### 6.4 Release notes (`Docs/Plans/Wave8/ReleaseNotes.md`, block **lane Render**)

- What's new: "Contact shadows are off in every Graphics Quality preset: they shimmered while the camera moved. The fitted
  sun cascades ground small objects near the camera."
- Upgrading: `| Contact shadows | drawn at High (12 steps) and Ultra (16) | off in every preset | Graphics Quality Custom,
  Contact Shadows 12 (or 16) |`; and "A Custom quality saved before contact shadows existed reads 0 steps."

### 6.5 Acceptance and perf

Plan 1.1's row: 0 in every preset, the row kept for Custom (the tests of 6.2). Perf, as information: three interleaved pairs
(A B, B A, A B) of `StrataTests.exe --test-suite=PerfGPU.Renderer --test-case="*whose shadows cover a 4K view*"` (doctest
splits filters at commas, so the case "Render cost of 100,000 cubes whose shadows cover a 4K view, at Medium and at High"
is named by a wildcard) under `PerfLock.py --label render-s02`, A `.worktrees/wave8-base`, B the lane's
Release build, both with `STRATA_TEST_GPU_VALIDATION=0`: `ScreenShadows4KHigh` is expected about 0.15-0.3 ms lower (the
audit's 0.63 to 0.35 ms on the Third-Person feet); `ScreenShadows4KMedium` within noise. The commit states the medians. The
metric's meaning changes (High draws no contact shadows); its entry stays `Provisional` with its target 0.45.

## 7. S0.3 The cascade fit stops jumping (lane Render)

### 7.1 What is wrong

The near bound follows any receiver above 1% of the pixels at once (2.2). A ball spawned 1.5 m from the camera holds 6.85%;
N drops from 11.31 to 0.84 m, every slice is re-cut, and about 52% of the receivers fall into the unfitted last cascade, 3.2
to 4.5 times coarser, for 116 ms (full power) to 354 ms (quarter power): 28 layout changes in three shots, 6,600-10,000 pixels
changing by 8/255 or more outside the ball (audit section 1). Hysteresis counts readbacks, 9 ms at 880 fps, so N walks back
in many small re-cuts. Only persistence tells a passing ball from a robot that stays, and persistence must be time.

### 7.2 The time base

- `R/SceneRenderer.h`, private, beside `UpdateExposure`:

  ```cpp
  // Seconds since `last`, which becomes now: the wall clock, at most c_MaxStepSeconds so that pauses (breakpoints, minimized
  // windows) never become one long step, or SceneRenderOptions::AdaptationSeconds when set (a non-finite value: 0). 0 the
  // first time.
  static float StepSeconds(std::optional<std::chrono::steady_clock::time_point>& last, const SceneRenderOptions& options);
  static constexpr float c_MaxStepSeconds = 0.25f;
  std::optional<std::chrono::steady_clock::time_point> m_LastShadowFitTime; // StepSeconds' clock of the cascades' fit
  ```

  defined in `R/SceneRendererShadowFit.cpp`. `UpdateExposure` (`R/SceneRendererPost.cpp:912-918`) calls
  `StepSeconds(m_LastExposureTime, options)` instead of its own lines: the same clamp and override, so exposure is unchanged.
- `UpdateShadowFit(const RenderScene&, const SceneCamera&, bool cameraCut, const SceneRenderOptions& options)` (h:1675, the
  call at cpp:3066 passes `RenderFrame`'s `options`); right before `m_ShadowFit.Update` (100): `view.Seconds =
  StepSeconds(m_LastShadowFitTime, options);`. Frames without a fit do not tick the clock; their gap returns capped, on a
  frame that starts an epoch anyway.
- `SceneRenderOptions::AdaptationSeconds` (h:421-423): "The time since this renderer's previous frame that automatic
  exposure adapts over and the sun cascades' fit counts its holds in (ShadowFitTracker); unset, the wall clock between its
  frames (at most 0.25 s ...). Tests and tools that step time, and tests that compare two renderers' frames, set it."
- `ShadowFitView` (`R/ShadowFit.h:36`), after `NearestTransparent`: `float Seconds = 0.0f; // Since the previous Update, from
  SceneRenderer::StepSeconds: what the tracker's holds count`, and (review, 7.3a) `bool Holds = true; // False: Strata
  0.7.0's rules (growths at once, shrinks after c_ShrinkReadbacks readbacks), for renderers of single images`.

### 7.3 The tracker

All in `ShadowFitTracker` (`R/ShadowFit.h/.cpp`), the prototype (`.worktrees/wave8-plan/shadow-jump/tools/patch_private.py`,
`patch_private2.py`) without its audit switches and logs, with the transparent rule and the narrow-view rule the prototype
lacked.

New public constants (after `c_NearOutlierShare`, h:100):

```cpp
// Receivers nearer than the near bound holding at least this share of the receiver pixels move it at once (a wall, a dive);
// fewer must keep asking for c_GrowSeconds (c_GrowSecondsMoving while the camera moves), so a ball passing the camera re-cuts
// no slice. 6.85% for the Physics Playground's ball at spawn; 15.6% for its dive (shadow-jump audit).
static constexpr double c_NearNowShare = 0.10;
// How long a growth must keep asking before it is made: the near bound's and a square's on a still camera (a square grows
// at once on a moving one), the near bound's on a moving camera. ShadowFitView::Seconds, never frames or readbacks.
static constexpr float c_GrowSeconds = 0.5f;
static constexpr float c_GrowSecondsMoving = 0.12f;
// A bound or side shrinks after c_ShrinkReadbacks readbacks in a row asked for less and at least this long.
static constexpr float c_ShrinkSeconds = 0.5f;
```

New public query: `bool IsHolding() const { return m_Holding; } // A growth of the near bound or of a square waits`. The
class comment (h:77-85) replaces "Bounds and sides grow at once and shrink only after c_ShrinkReadbacks readbacks in a row
asked for less" with "The far bound grows at once; the near bound grows at once for receivers holding c_NearNowShare of the
pixels, for a transparent surface and in narrow views, else only after asking for c_GrowSeconds (c_GrowSecondsMoving on a
moving camera); squares grow at once on a moving camera and after c_GrowSeconds on a still one, keeping the previous
square meanwhile; everything shrinks after c_ShrinkReadbacks readbacks and c_ShrinkSeconds."

Private changes:

```cpp
struct Hysteresis
{
	uint32_t Count = 0;    // Readbacks in a row that asked for less
	int32_t Wanted = 0;    // Shrinking: the least shrunk of what they asked for; growing: the least grown
	float Seconds = 0.0f;  // How long it has asked
	bool Growing = false;  // It asks for more, and waits
};
// Grows at once when `growNow`, else once it kept asking for `growSeconds`, to the least grown of what it asked for;
// shrinks after c_ShrinkReadbacks readbacks in a row and `shrinkSeconds` (c_ShrinkSeconds; 0 without holds, 7.3a), to the
// least shrunk; `larger` orders the targets.
static void Follow(int32_t target, int32_t& current, Hysteresis& state, bool newReadback, bool larger, float seconds, bool growNow,
	float growSeconds, float shrinkSeconds);
// The share of the readback's receiver pixels nearer than `nearBound` (whole bins: a bin straddling it counts).
double GetNearerShare(double nearBound) const;
bool m_Holding = false;
bool m_HasLastView = false;
glm::dvec3 m_LastCameraPosition = glm::dvec3(0.0);
glm::mat3 m_LastCameraRotation = glm::mat3(1.0f);
glm::dmat3 m_LastLightRotation = glm::dmat3(1.0);
```

`Follow` (replacing cpp:146-168):

```cpp
const bool grows = larger ? target > current : target < current;
if (grows)
{
	if (growNow)
	{
		current = target;
		state = {};
		return;
	}
	if (!state.Growing)
	{
		state = {};
		state.Growing = true;
		state.Wanted = target;
	}
	state.Seconds += seconds;
	state.Wanted = larger ? std::min(state.Wanted, target) : std::max(state.Wanted, target);
	if (state.Seconds >= growSeconds)
	{
		current = state.Wanted;
		state = {};
	}
	return;
}
if (state.Growing && newReadback)
	state = {}; // It stopped asking for more
if (!state.Growing && target != current)
	state.Seconds += seconds;
if (!newReadback)
	return; // Shrinks also count readbacks
if (target == current)
{
	state = {};
	return;
}
state.Wanted = state.Count == 0 ? target : (larger ? std::max(state.Wanted, target) : std::min(state.Wanted, target));
if (++state.Count >= c_ShrinkReadbacks && state.Seconds >= shrinkSeconds)
{
	current = state.Wanted;
	state = {};
}
```

`Update` (cpp:328-453), in order:

1. At its start: `const bool still = m_HasLastView && view.CameraPosition == m_LastCameraPosition && view.CameraRotation ==
   m_LastCameraRotation && view.LightRotation == m_LastLightRotation;` then store the three and `m_HasLastView = true`. A
   turning sun (the Open World's day) counts as moving. TAA's jitter is in the projection, not in these.
2. The direct path (345-354) is unchanged: an epoch's first readback, or one after unfitted frames, sets everything at once.
3. Else, replacing 357-358:

   ```cpp
   // A transparent surface nearer than the bound moves it at once: no readback measures it, and it does not pass by.
   if (std::isfinite(view.NearestTransparent))
   {
   	const int32_t transparent = QuartersBelow(std::max(static_cast<double>(view.Near), static_cast<double>(view.NearestTransparent) / c_Sqrt2));
   	if (transparent < m_NearQuarters)
   	{
   		m_NearQuarters = transparent;
   		m_NearState = {};
   	}
   }
   // Receivers nearer than the bound move it at once when they hold c_NearNowShare of the pixels, or when the last cascade's
   // sphere could not hold them meanwhile (the bound beyond D k: narrow and orthographic views, GetFittedNearest's rule);
   // otherwise the growth waits until it has asked long enough.
   const double nearBound = std::max(static_cast<double>(view.Near), FromQuarters(m_NearQuarters));
   const bool nearNow = !view.Holds || GetNearerShare(nearBound) >= c_NearNowShare || !(nearBound <= distance * GetSphereReach(view));
   Follow(nearTarget, m_NearQuarters, m_NearState, newReadback, false, view.Seconds, nearNow, still ? c_GrowSeconds : c_GrowSecondsMoving, shrinkSeconds);
   Follow(farTarget, m_FarQuarters, m_FarState, newReadback, true, view.Seconds, true, 0.0f, shrinkSeconds);
   ```

   with `const float shrinkSeconds = view.Holds ? c_ShrinkSeconds : 0.0f;` declared at `Update`'s start (step 1).

   `GetNearerShare(nearBound)`: the sum of `bin.Pixels` over bins with `bin.MaxD != 0` and `DecodeOrderedFloat(~bin.MinD) <
   nearBound`, over `max(SurfacePixels, 1)`. The transparent target is the one `GetBoundTargets` already folds into
   `nearTarget` (214), so after the jump to it only an opaque receiver nearer still can ask for more, and that part waits.
   The far bound keeps growing at once (the plan's "the far bound stays") and shrinks like every bound, after 8 readbacks
   and 0.5 s, as the prototype measured.
4. `Reproject` (236-239): `const double nearest = std::max(GetFittedNearest(view), std::max(static_cast<double>(view.Near),
   FromQuarters(m_NearQuarters)));` so bins wholly nearer than the layout's N (a held ball) stay out of the squares; they are
   shaded by containment, as outliers are. In a settled layout no bin lies there (N is the nearest over 2^0.5), so settled
   layouts do not change.
5. The squares (396-433): `Follow(target, m_SideQuarters[cascade], m_SideStates[cascade], newReadback, true, view.Seconds,
   !still || !view.Holds, c_GrowSeconds, shrinkSeconds)` (408), and, right after `quarters` is read (413) and before the sphere test (414), with
   `previous` (now at 425) moved up to there so both uses share it (as the prototype placed it: a waiting growth toward the
   sphere keeps the square too):

   ```cpp
   // A still camera whose square waits for a growth keeps the previous square whole (side and center): what lies outside it
   // takes the next cascade that holds it (containment) until the growth is made.
   if (!slicesMoved && still && target > m_SideQuarters[cascade] && m_Layout.Fitted && cascade < m_Layout.CascadeCount && previous.Fitted)
   {
   	slice.Fitted = true;
   	slice.Side = previous.Side;
   	slice.Center = previous.Center;
   	layout.SquaresFitted = true;
   	continue;
   }
   ```

6. After the layout: `m_Holding = fitted && (m_NearState.Growing || any m_SideStates[c].Growing for c + 1 < count)`. The
   settled rule (447-450) requires `!m_Holding`, and `if (m_Holding) m_Settled = false;` follows it: a layout a waiting
   growth may still change is not settled. `Reset()` (133-144) also clears `m_Holding`.

What it keeps: wave 7's density (settled layouts identical, same N and slices: audit, "Prototype results"), the 1% outlier
rule, the squares' texel snapping and their "holds" test (419-431), every epoch rule.

### 7.3a Renderers of single images keep 0.7.0's rules (review)

Wall-clock holds in the capture renderer would make pictures depend on how long frames take. Under TAA each jitter phase's
readback can ask a square for a quarter stop more (2.2); with holds, a still capture waits instead of growing, its
`m_Holding` (and so `ShadowFitSettled`, 7.3 step 6) turns false on those readbacks, `viewport.capture` then counts the fit as
pending and resets its TAA history (`E/EditorViewportCommands.cpp:761-774`), and a hold whose asks span 0.5 s (frames over
about 60 ms, or a skipped readback: only the newest ready one is read, `R/SceneRendererShadowFit.cpp:81-87`) completes at a
frame that depends on the machine. The look-dev harness, every stream's A/B, S0.11's 0-of-84 gate and the
"second capture equals it byte for byte" case all rest on captures being repeatable. Single images show no motion, so the
holds buy them nothing:

- `SceneRendererSpecification` (`R/SceneRenderer.h`, after `FitShadowsToView`, h:241): `bool ShadowFitHolds = true;` with
  the comment "The fitted cascades wait before they follow a near object or a wider square and shrink only after half a
  second (ShadowFitTracker's holds, counted in seconds). Off: Strata 0.7.0's rules, growths at once and shrinks after
  c_ShrinkReadbacks readbacks, so that pictures do not depend on how long frames take: renderers of single images
  (viewport.capture's renderer, project thumbnails)."
- `UpdateShadowFit`: `view.Holds = m_Specification.ShadowFitHolds;` with `view.Seconds`.
- `ViewportRenderer::ViewportRenderer` (`StrataEditor/src/Editor/ViewportRenderer.cpp:127-133`):
  `specification.ShadowFitHolds = fades;` beside `SpreadLightingUpdates`, with "single pictures: the cascades' fit follows
  0.7.0's rules (no holds), so a capture is the same on any machine". The capture renderer (`fades` false) is the one
  `viewport.capture` and `ProjectThumbnail` use; the Scene and Game views keep holds.
- `ShadowFitTracker::Update`: with `!view.Holds` every `Follow` call passes `growNow = true` and the shrink's time condition
  is dropped. `Follow` gains a last parameter `float shrinkSeconds` (`c_ShrinkSeconds` with holds, 0 without) used in place
  of `c_ShrinkSeconds`; the transparent rule and the held-square block are then never reached with a pending growth, and
  `m_Holding` stays false.
- Q8's coupling (settled false while holding) then concerns live views only, where `ShadowFitSettled` is a statistic.

### 7.4 Stats and commands

- `SceneRendererStats` (h:680): `bool ShadowFitHolding = false;` beside `ShadowFitSettled`, documented "a growth of the
  near bound or of a square waits (ShadowFitTracker::IsHolding)"; set in `UpdateShadowFit` with the others (101-107).
- `DescribeRenderStats` (`E/EditorStateCommands.cpp:235-236`): `shadowFit` gains `{ "changes", stats.ShadowFitChanges }`
  and `{ "holding", stats.ShadowFitHolding }`; `editor.perf`'s description (930-932) adds "changes: the view renderer's layout
  changes since it was made (count them over a run to see re-cuts); holding: a near object or a wider square waits before it
  re-cuts the cascades (half a second on a still camera, an eighth while the camera moves)". `viewport.capture`'s
  `renderStats` has them through the same function.

### 7.5 Tests

`Renderer.ShadowFit` (`T/Renderer/ShadowFitTests.cpp`). Helpers from `patch_tests.py`: `GetReceiversWithBall`,
`PlaygroundDefaultCamera` ((0, 22, 30) toward the origin), `SettleOnGround`. `MakeView` keeps `Seconds` 0 by default; every
case that relies on time sets it.

| Case | What it checks | Without the change |
| --- | --- | --- |
| New: "A ball shot from the camera leaves the layout alone at any frame rate" | At 60, 240 and 1000 fps (`Seconds` 1/fps) and 24 and 12 m/s, a 0.25 m ball from 1.5 m along the view for 1 s: `GetChanges()` and `Near` unchanged, `IsHolding()` false at the end | Fails all six (prototype) |
| New: "A receiver that stays near moves the near bound once it persisted, one that fills a tenth of the view at once" | Parked at 2 m (about 4%), 1/60 s frames: `Near` unchanged after 0.4 s, below 1.75 m after 0.6 s; a 2 m sphere at 3 m moves it in its first readback | Fails (moves at once) |
| New: "A moving camera waits an eighth of a second for the near bound, a still one half a second" | The parked ball with the camera moved 1 mm each frame: moved after 0.15 s, not after 0.10 s; still: not after 0.4 s | Fails (moves at once) |
| New: "The nearest transparent surface moves the near bound at once while an opaque growth waits" | Parked ball held (4%); `NearestTransparent` 1.2 m, nearer than N but farther than the ball: N at most 1.2 / 2^0.5 in that `Update` without a readback, above the ball's bound; after 0.5 s N reaches the ball | Fails (the prototype held the transparent surface) |
| New: "Narrow and orthographic views grow the near bound at once" | 20 degrees at 16:9 (D k = 10.3 m at D = 80) with N above D k, and an orthographic view: a 4% near receiver moves N in its first readback | Fails without the narrow-view rule |
| New: "A still camera keeps a square whole while its growth waits; a moving camera grows it at once" | Board view settled; receivers that widen cascade 0's box at under 10%: still, `Side` and `Center` of cascade 0 unchanged for 0.4 s, grown after 0.6 s; with the camera moved 1 cm, grown in the first readback | Fails |
| New: "Shrinks wait for eight readbacks and half a second at any frame rate" | Eight readbacks asking for less at 1 ms frames: no shrink; continued to 0.5 s: shrinks to the least shrunk | Fails (shrinks at the eighth readback) |
| New: "A waiting growth keeps the fit unsettled" | `IsSettled()` false while `IsHolding()`; true after the growth and a confirming readback | Fails (settled while a growth waits) |
| Changed: "Bounds grow at once and shrink after eight readbacks in a row, to the least shrunk of them" (263-314) | Renamed "...far bounds grow at once; bounds shrink after eight readbacks and half a second, to the least shrunk of them"; `view.Seconds = 0.05f` (the loop's 15 updates before the eighth readback are 0.75 s); the last block's 2% receiver (307-313) is now held: `Near` unchanged after one readback, lower after 0.5 s; a receiver of `near20.size() / 8` pixels (11.1% of the receivers: the share counts them in the total) lowers it at once | Its time-free form fails on the change |
| New (review, 7.3a): "Without holds the tracker keeps Strata 0.7.0's rules" | The old case's body (263-314) verbatim with `view.Holds = false` and `Seconds` 0: grows at once, shrinks at the eighth readback, the 2% receiver lowers `Near` at once | Fails if 7.3a's switch is missing |
| New (review): "Settled layouts are the same with and without holds" | `BoardCamera` over the board and the default camera over the ground, each settled by two trackers (`Holds` true with `Seconds` 1/60, false): `Near`, `Far`, every slice, side and center equal | A guard of "What it keeps", not a regression test (it passes on 0.7.0, which has no holds) |
| Kept: "The nearest transparent surface bounds the near end" (360-375) | Passes unchanged (the transparent rule) | The prototype failed it |
| Kept | Every other case: their readbacks are an epoch's first, or their cameras move, or nothing comes nearer | |

`GPU.SceneRenderer.ShadowFit` (`T/Renderer/GPUSceneShadowFitTests.cpp`). `T/Renderer/SceneRendererTestUtils.h` gains
`RenderToImage(SceneRenderer&, Scene&, const SceneCamera&, const SceneRenderOptions&)`; the file gets `SceneRenderOptions
StepOptions()` (`AdaptationSeconds` 1/60) and its helpers (`RenderUntilFitSettled`, `RenderHDR`) and every case render
through it, so no comparison depends on the machine's clock.

| Case | What it checks | Without the change |
| --- | --- | --- |
| New: "A ball shot from the camera leaves the cascades alone" (prototype) | `PlaygroundScene`, settled; a 0.5 m sphere from 1.5 m along the view, 0.4 m a frame for 40 frames: `ShadowFitChanges` and `Cascades` equal every frame, `ShadowFitHolding` false at the end; `gpu.GetNewErrorCount() == 0` | 55 assertions fail (audit) |
| New: "The fit's holds count the frame's AdaptationSeconds" | A box parked 2 m in front (about 4% of the receivers): with `AdaptationSeconds` 0.1, `ShadowFitHolding` and not `ShadowFitSettled` at first, `ShadowFitNear` below 1.75 m within 12 frames; with 0, unchanged after 30 frames | Fails (moves in the first readback) |
| Changed: every case | Renders through `StepOptions()`, `RenderShadowView` (163-173) included (its options gain `AdaptationSeconds`) | |
| Changed: "A still camera keeps its fitted cascades and their cached layers" (406-457) | Its TAA block (440-455) no longer expects a jitter phase's growth to be made: on a still camera a growth asked by some phases only waits and is dropped at the next readback that does not ask. The block keeps its checks (no layout change, every cascade cached across 2 x 8 frames) and adds `CHECK_FALSE(renderer.GetStats().ShadowFitHolding)` at its last frame; if that check fails (a phase that asks on every readback), the case records the holding frames in its message and the implementer reports it under 13, R3, before changing the rule | |

`GPU.Editor.Viewport`, "A capture waits until the sun's cascades fit the view..." (`T/Editor/GPUEditorViewportTests.cpp:1931`):
`renderStats.shadowFit.holding == false` and `changes` a number, beside 1947-1948, and (7.3a)
`CHECK_FALSE(harness.Context.GetViewport().GetCaptureRenderer()->GetSceneRenderer().GetSpecification().ShadowFitHolds)`.

**Regression check.** Each new case fails without the change: the implementer runs them once with a local edit that makes
every growth immediate (`growNow` forced true in the two near and side calls), sees them fail, and restores the code; the
commit says which failed. The suites `GPU.SceneRenderer.Shadows`, `.GPUDriven`, `.ScreenShadows`, `.ShadowFit`,
`GPU.Editor.Viewport` and the feature test pass in Debug under the validation layer (the prototype ran only in Release, without
it).

**The rule for other tests.** A test that compares two renderers' frames with the fit on while something nearer than N appears
or the camera moves passes `AdaptationSeconds` to both. The implementer checks the cases of `GPUSceneRendererGPUDrivenTests.cpp`
and `GPUSceneShadowTests.cpp` that keep the fit on (the 22 `FitShadowsToView = false` lines mark the others) and changes those
that fit that description; DG "Sun shadows" states the rule.

### 7.6 Docs

- DG 4133-4135 ("A bound or side grows in the frame it must and shrinks only after ... (8) readbacks in a row asked for less,
  to the least shrunk of them.") becomes:

  "**Persistence, in seconds** (`ShadowFitView::Seconds`: `SceneRenderer::StepSeconds`, the wall clock between the renderer's
  frames, at most 0.25 s, or `SceneRenderOptions::AdaptationSeconds`): the far bound grows in the frame it must; the near
  bound does so for receivers nearer than it that hold at least `c_NearNowShare` (10%) of the receiver pixels (a wall, a dive),
  for the previous frame's nearest transparent surface, and where the last cascade's sphere could not hold what it leaves out
  (the bound beyond `D k`); otherwise it grows only once it has asked for `c_GrowSeconds` (0.5 s; `c_GrowSecondsMoving`, 0.12
  s, while the camera or the sun moves), to the least it asked for, and the receivers nearer than it stay out of the squares
  meanwhile (containment shades them from the last cascade). So a ball passing the camera re-cuts nothing at any frame rate
  (the Physics Playground's ball holds 6.85% at spawn and is gone in 116-354 ms), while an object that stays near re-cuts once,
  after the wait. A square grows at once on a moving camera; on a still one it waits `c_GrowSeconds` and the previous square
  stays whole meanwhile. Every bound and side shrinks after `c_ShrinkReadbacks` (8) readbacks in a row and `c_ShrinkSeconds`
  (0.5 s), to the least shrunk of them. Tests that compare two renderers with the fit on and content or a camera that moves
  pass `AdaptationSeconds`, since wall-clock holds would make them timing dependent."
- DG "Settled" (4146-4148): "...arrived and kept it, and no growth waits (`ShadowFitHolding`; true without a fit)". The stats
  list (4161-4162) adds `ShadowFitHolding`. The persistence paragraph ends with 7.3a's rule: "Renderers of single images
  (`SceneRendererSpecification::ShadowFitHolds` off: the capture renderer behind `viewport.capture` and project thumbnails)
  keep Strata 0.7.0's rules, so a capture is the same on any machine."
- `Docs/Manual/Lighting.md:113-119`, after "follow.": "Something passing close to the camera, such as a thrown ball, leaves
  them alone; something that stays close moves them once, after half a second."
- The editor-automation skill: where it names `shadowFitSettled` and `renderStats`, add `shadowFit.changes` and `holding`.

### 7.7 Release notes (block **lane Render**)

- What's new: "The sun's shadows no longer jump while something passes close to the camera (a ball thrown from it, a bird):
  the cascades follow a near object that holds less than a tenth of the view only once it has stayed for half a second (an
  eighth while the camera moves), and shrink only after half a second. Something that stays close still moves them once."
- Upgrading: `| The cascades' near end | follows any near object above 1% of the view at once | waits 0.5 s (0.12 s moving)
  below 10% of the view; transparent surfaces and narrow views at once | nothing to keep: settled views are unchanged |`.
- Known issues: "An object that stays close to the camera still re-fits the cascades once, coarsening far shadows; one that
  covers under a tenth of the view waits up to half a second in a coarser cascade."

### 7.8 Acceptance, and how each is measured

Measured with the lane's Release build alone under `PerfLock.py --label render-s03`, scripts adapted from the audit's
(`shadow-jump/tools/exp3_variant.py`, `exp4_motion.py`, `exp5_sample.py`, `diff_masked.py`) that read
`editor.perf {frames: 1}`'s `gameRenderStats.shadowFit.changes` at each mark instead of the private log; kept in
`.worktrees/wave8-plan/step0/render/` (never committed). Projects are copies (the audit's `pp`, `tp`, `ow` folders are
0.7.0's copies).

| What | 0.7.0 (audit) | Target | How |
| --- | --- | --- | --- |
| Layout changes during three shots, Playground default camera, 1920x1080 Game view | 28 | 0 | `exp3_variant.py` (a full and a quarter-power shot in real time, then a 16-frame live burst one step a frame): `changes` before each shot and 3 s after; at the editor's uncapped rate, and at `ActiveFrameRate` 60 and 240 once lane Editor's S0.4 has landed (else at 60 and 240 through the CPU case of 7.5, and again when S0.4 lands). 1000 fps: the uncapped run where the editor reaches it, else the CPU case |
| Pixels changing by 8/255 or more outside the ball, frames 6-16 of the burst against frame 1 | 6,600-10,000 | at most 2,400 (fit off: 800-2,400) | `diff_masked.py` on the burst's frames |
| Layout changes in scripted motion: Playground (flights, dive, grab), Third-Person (turn and run, 6 s), Open World (tour, 20 s) | 202, 371, 427 | at most 100, 80, 65 (prototype 85, 66, 51) | `exp4_motion.py` and `exp5_sample.py`, `changes` at the first and last mark |
| Captures unchanged | | 0 of the Medium stills differ from 0.7.0's, and each picture's repeats equal each other as in 0.7.0 | The 84-set's options at `--quality medium` (8.5) against `$L/0.7.0-medium`: Medium draws no contact shadows either side, and captures keep 0.7.0's fit rules (7.3a), so a difference means the time plumbing or the tracker's refactor changed something. Live settled layouts are checked by "Settled layouts are the same with and without holds" (7.5) and the GPU fit cases |
| Debug, validation layer, feature test | not run (audit) | pass | 7.5 |

### 7.9 Commits and perf

S0.2: "Turn contact shadows off in every quality preset". S0.3, in order: "Count the sun cascades' fit in seconds"
(`StepSeconds`, `ShadowFitView::Seconds` and `Holds`, `SceneRendererSpecification::ShadowFitHolds` with the capture
renderer's off, `UpdateShadowFit`'s options, the exposure on the helper: no behavior change, every test passes as before),
"Let the cascades' near end wait for objects passing the camera" (7.3-7.6), "Report the cascades' fit changes and holds"
(7.4, if not folded into the previous). Perf, information only: three interleaved pairs of
`PerfGPU.Renderer` against `.worktrees/wave8-base`; the CPU metrics that include extraction (the `Render*` frames) within the
A/B's noise (the tracker adds a loop over 80 bins a frame); no new metric.

## 8. S0.11 The indirect light's seam, slots and stages (last)

### 8.1 Why

`reflections` (the specular gather: the resolve, F0, specular occlusion off screen-space hits) and `radiance-cascades` (the
diffuse gather: hit light plus open share times today's irradiance; the open share in specular occlusion) both change
`ShadeLitSurface`'s indirect block, whose terms share their inputs (2.2). S0.11 splits it into inputs filled once, two
gathers and a compose step, changing no pixel, so each stream changes its own function and both add to the struct.

### 8.2 `SH/Include/IndirectLight.glsl` (new; re-run configure: includes are globbed, `CMake/StrataShaders.cmake:28`)

```glsl
#ifndef STRATA_INDIRECT_LIGHT_GLSL
#define STRATA_INDIRECT_LIGHT_GLSL

// The forward pass's indirect light (Scene/Forward.frag's ShadeLitSurface; Docs/DeveloperGuide.md, "Lighting"): the inputs
// both gathers read, filled once per pixel (GetIndirectInputs in Forward.frag), the diffuse gather, the specular gather and
// the compose step that keeps Strata 0.7.0's four forms bit for bit. Included by Scene/Forward.frag only, after
// Include/PBR.glsl and Include/ReflectionProbes.glsl with ST_REFLECTION_PROBE_SAMPLING.

// The forms of the base's indirect light, by what lights the pixel.
#define ST_INDIRECT_PROBES  0u // Reflection probes; the sky's share (or the constant ambient's) fills what they leave
#define ST_INDIRECT_SCREEN  1u // Screen-space reflections without probes; the sky's intensity scales inside the gathers
#define ST_INDIRECT_SKY     2u // The sky alone; its intensity (EnvironmentParams.x) scales the composed sum
#define ST_INDIRECT_AMBIENT 3u // The constant ambient alone: diffuse only

// What the gathers share. Streams add fields at the end (plan 4: shared and additive).
struct IndirectInputs
{
	uint Form;                  // ST_INDIRECT_*
	bool Environment;           // EnvironmentParams.w > 0: the sky's image light; else the constant ambient
	vec3 SpecularColor;         // The split sum's reflectance, f0 dfg.x + dfg.y, compensated for multiple scattering
	vec3 Reflected;             // The view reflected about the shading normal
	vec3 GeometricNormal;       // The primitive's normal, facing the viewer
	float DiffuseOcclusion;     // Material occlusion x screen-space occlusion
	vec3 DiffuseOcclusionColor; // With the multi-bounce of AOParams.y
	float SpecularOcclusion;    // From DiffuseOcclusion (Lagarde), with the horizon fade; 1 in the ambient form
	ProbeLight Probes;          // The reflection probes at the surface (Weight 0 without probes)
	vec4 ScreenReflection;      // rgb the reflected luminance x confidence, unexposed; a the confidence (0: none)
	float Sky;                  // 1 - Probes.Weight: the sky's (or the constant ambient's) share
	vec3 SkyIrradiance;         // The sky's irradiance at the shading normal (0 without the environment)
	vec3 SkyRadiance;           // The sky's prefiltered radiance along Reflected at the surface's roughness (0 without)
	float DiffuseWeight;        // The share of its diffuse light the surface scatters (glass: 1 - transmission)
};

// The share of its diffuse light a surface scatters: transmissive surfaces pass the rest through (the glass variant). The other
// variants scatter all of it, with the arithmetic they had before transmission existed.
vec3 ScatterDiffuse(vec3 diffuse, float diffuseWeight)
{
#ifdef ST_FORWARD_GLASS
	return diffuse * diffuseWeight;
#else
	return diffuse;
#endif
}

// The base's diffuse indirect light, scattered and occluded (radiance-cascades' region).
vec3 GatherIndirectDiffuse(IndirectInputs inputs, SurfaceData surface)
{
	vec3 diffuse;
	if (inputs.Form == ST_INDIRECT_PROBES)
	{
		// The probes' captures hold the scene's luminance (their intensities applied); the sky's share is scaled here.
		vec3 irradiance = inputs.Probes.Irradiance + (inputs.Environment ? inputs.SkyIrradiance * u_Frame.EnvironmentParams.x * inputs.Sky : vec3(0.0));
		diffuse = irradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - inputs.SpecularColor);
		// The constant ambient's share as the constant ambient lights alone (diffuse only), so that a probe fading out meets it
		// without a step at its box's faces.
		if (!inputs.Environment)
			diffuse += u_Frame.AmbientColor.rgb * inputs.Sky * surface.Albedo * (1.0 - surface.Metallic);
	}
	else if (inputs.Form == ST_INDIRECT_SCREEN)
	{
		// Without the sky the constant ambient lights the diffuse part, as it does without reflections.
		diffuse = inputs.Environment ? inputs.SkyIrradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - inputs.SpecularColor) * u_Frame.EnvironmentParams.x
			: u_Frame.AmbientColor.rgb * surface.Albedo * (1.0 - surface.Metallic);
	}
	else if (inputs.Form == ST_INDIRECT_SKY)
	{
		diffuse = inputs.SkyIrradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - inputs.SpecularColor);
	}
	else
	{
		// Constant ambient without an environment map: diffuse only, modulated by material occlusion.
		diffuse = u_Frame.AmbientColor.rgb * surface.Albedo * (1.0 - surface.Metallic);
	}
	return ScatterDiffuse(diffuse, inputs.DiffuseWeight) * inputs.DiffuseOcclusionColor;
}

// The base's specular indirect light, by its reflectance and occlusion (reflections' region). Screen-space reflections replace
// the probes' and the sky's light by their confidence.
vec3 GatherIndirectSpecular(IndirectInputs inputs)
{
	vec3 radiance;
	if (inputs.Form == ST_INDIRECT_PROBES)
	{
		radiance = inputs.Probes.Radiance + inputs.SkyRadiance * (inputs.Environment ? u_Frame.EnvironmentParams.x * inputs.Sky : 0.0);
		if (inputs.ScreenReflection.a > 0.0)
			radiance = radiance * (1.0 - inputs.ScreenReflection.a) + inputs.ScreenReflection.rgb;
	}
	else if (inputs.Form == ST_INDIRECT_SCREEN)
	{
		radiance = inputs.SkyRadiance * (inputs.Environment ? u_Frame.EnvironmentParams.x : 0.0) * (1.0 - inputs.ScreenReflection.a) + inputs.ScreenReflection.rgb;
	}
	else if (inputs.Form == ST_INDIRECT_SKY)
	{
		radiance = inputs.SkyRadiance;
	}
	else
	{
		return vec3(0.0); // The constant ambient lights the diffuse part only
	}
	return radiance * inputs.SpecularColor * inputs.SpecularOcclusion;
}

// The base's indirect light from the two gathers, each form's scale where 0.7.0 applied it (the sky's intensity on the sum of
// the sky-only form; the clear coat follows in Forward.frag).
vec3 ComposeIndirect(IndirectInputs inputs, vec3 diffuse, vec3 specular)
{
	if (inputs.Form == ST_INDIRECT_AMBIENT)
		return diffuse;
	if (inputs.Form == ST_INDIRECT_SKY)
		return (diffuse + specular) * u_Frame.EnvironmentParams.x;
	return diffuse + specular;
}

#endif
```

Each expression is the statement it replaces (Forward.frag 270-299), operand for operand and in the same order: the probes
form's 271-275 and 279-280, the screen form's 285-288, the sky form's 292-293, the ambient form's 299.

### 8.3 `SH/Scene/Forward.frag` after S0.11

- `#include "Include/IndirectLight.glsl"` after line 17. `ScatterDiffuse` (85-94) moves into the include.
- Before `ShadeLitSurface`, the filler (it reads this file's `u_ScreenReflections`):

  ```glsl
  // The indirect light's inputs at the pixel (Include/IndirectLight.glsl), filled once.
  IndirectInputs GetIndirectInputs(SurfaceData surface, vec3 f0, vec2 dfg, float NoV, vec3 geometricNormal, vec2 fragCoord,
  	float diffuseOcclusion, vec3 diffuseOcclusionColor, float diffuseWeight)
  {
  	IndirectInputs inputs;
  	inputs.DiffuseOcclusion = diffuseOcclusion;
  	inputs.DiffuseOcclusionColor = diffuseOcclusionColor;
  	inputs.DiffuseWeight = diffuseWeight;
  	inputs.GeometricNormal = geometricNormal;
  	inputs.Reflected = reflect(-surface.View, surface.Normal);
  	inputs.SpecularColor = (f0 * dfg.x + dfg.y) * surface.SpecularEnergy;
  	inputs.Probes.Irradiance = vec3(0.0);
  	inputs.Probes.Radiance = vec3(0.0);
  	inputs.Probes.Weight = 0.0;
  	if (u_ProbeCounts.x > 0u)
  		inputs.Probes = EvaluateReflectionProbes(surface.Position, surface.Normal, inputs.Reflected, surface.Roughness);
  	inputs.Sky = 1.0 - inputs.Probes.Weight;
  	inputs.ScreenReflection = vec4(0.0);
  #ifndef ST_FORWARD_TRANSPARENT
  	if (u_Frame.ReflectionParams.x > 0.0)
  	{
  		inputs.ScreenReflection = textureLod(sampler2D(u_ScreenReflections, u_LinearClampSampler), fragCoord * u_Frame.ViewportSize.zw, 0.0);
  		inputs.ScreenReflection.rgb /= u_Frame.TemporalParams.y;
  	}
  #endif
  	inputs.Environment = u_Frame.EnvironmentParams.w > 0.0;
  	inputs.SkyIrradiance = vec3(0.0);
  	inputs.SkyRadiance = vec3(0.0);
  	if (inputs.Environment)
  	{
  		inputs.SkyIrradiance = textureLod(samplerCube(u_IrradianceMap, u_LinearClampSampler), RotateEnvironmentDirection(surface.Normal), 0.0).rgb;
  		inputs.SkyRadiance = textureLod(samplerCube(u_PrefilteredMap, u_LinearClampSampler), RotateEnvironmentDirection(inputs.Reflected),
  			surface.Roughness * u_Frame.EnvironmentParams.z).rgb;
  	}
  	inputs.Form = inputs.Probes.Weight > 0.0 ? ST_INDIRECT_PROBES : inputs.ScreenReflection.a > 0.0 ? ST_INDIRECT_SCREEN
  		: inputs.Environment ? ST_INDIRECT_SKY : ST_INDIRECT_AMBIENT;
  	// Specular occlusion from ambient occlusion (Lagarde): tighter for smooth surfaces and grazing views; reflections a normal
  	// map turns into the surface fade out (GetHorizonFade).
  	inputs.SpecularOcclusion = inputs.Form == ST_INDIRECT_AMBIENT ? 1.0
  		: clamp(pow(NoV + diffuseOcclusion, exp2(-16.0 * surface.Roughness - 1.0)) - 1.0 + diffuseOcclusion, 0.0, 1.0)
  		* GetHorizonFade(inputs.Reflected, geometricNormal);
  	return inputs;
  }
  ```

  The form test is the old branch order (250, 267, 282): probes first, then screen-space reflections, then the sky, else
  the ambient.
- In `ShadeLitSurface`, lines 227-300 become:

  ```glsl
  	vec3 surfaceGeometricNormal = normalize(v_Normal) * (gl_FrontFacing ? 1.0 : -1.0);
  	IndirectInputs indirectInputs = GetIndirectInputs(surface, f0, dfg, NoV, surfaceGeometricNormal, gl_FragCoord.xy, diffuseOcclusion,
  		diffuseOcclusionColor, diffuseWeight);
  	// The base's image-based light (or constant ambient), which the clear coat covers below.
  	vec3 indirect = ComposeIndirect(indirectInputs, GatherIndirectDiffuse(indirectInputs, surface), GatherIndirectSpecular(indirectInputs));
  ```

  and the coat (302-314) reads `indirectInputs.Environment` (309) and `indirectInputs.GeometricNormal` (311); its
  `diffuseOcclusion` stays the local. The head (137-160), the sun, the light loop and the lighting views are untouched.

### 8.4 The same in Slang's HLSL syntax (if lane Slang has landed)

`SH/Include/IndirectLight.slang` is 8.2 with the conversion lane Slang's converter applies and its hand port shows
(`.worktrees/wave8-plan/slang/spike/hand/tree/Scene/Forward.frag.slang:225-310`): `vecN` becomes `floatN`, `vec3(0.0)` stays
`float3(0.0)`, the guard is `STRATA_INDIRECT_LIGHT_SLANG`, the `#define`s, `struct`, `bool`, `uint` and the ternaries stay as
they are (scalar conditions), and no `mul` appears. In `Forward.frag.slang`'s filler the two cube reads are
`u_IrradianceMap.SampleLevel(u_LinearClampSampler, RotateEnvironmentDirection(surface.Normal), 0.0).rgb` and
`u_PrefilteredMap.SampleLevel(u_LinearClampSampler, RotateEnvironmentDirection(inputs.Reflected), surface.Roughness *
u_Frame.EnvironmentParams.z).rgb`, the screen read `u_ScreenReflections.SampleLevel(u_LinearClampSampler, fragCoord *
u_Frame.ViewportSize.zw, 0.0)`, and the caller passes the pixel position and the facing as the converted tree names them (the
converter's stage globals or the hand port's `pixel.Position.xy`, `pixel.Normal`, `pixel.FrontFacing`). The include is listed
by the glob lane Slang's `strata_add_shaders` uses; a configure follows its addition.

If lane Slang's box ended without Slang (plan S0.10), S0.11 lands in GLSL as 8.2-8.3.

### 8.5 Pixel-equal, and the fallback

- **The set.** The 84-set: `--sizes 3840x2160 --aa taa,fxaa,off --repeat 2 --projects ThirdPerson,PhysicsPlayground,OpenWorld,LookDev`.
  S0.11 captures it at its own base commit before any edit (lane Slang's last act took the same set; if main has not moved
  since, the two are the same), then at its tip. **Gate: 0 of 84 pictures differ**, beside every `GPU*` suite in Debug under
  the validation layer (the exact-pixel lighting, furnace, probe, reflection and glass cases among them) and the feature
  test. Pictures whose own repeats differ in the base capture are the only allowed exception, by one code value, as in 4.3.
  The gate relies on 7.3a: with holds in the capture renderer, TAA pictures could differ by how long frames took.
- **Why it should hold.** The four forms' expressions are the old statements; the gathers are inlined (glslang emits calls,
  the driver inlines; slangc inlines in spirv-opt), so the same operations reach the driver. **Unverified:** whether the
  driver contracts multiply-adds the same way when a product's single use crosses a function boundary. If any picture
  differs, the fallback shape is taken before anything else: the gathers return the unscattered `diffuse` and the unweighted
  `radiance`, and `ComposeIndirect` writes each form's final statement whole, as 0.7.0 did (`ScatterDiffuse(diffuse, w) *
  DiffuseOcclusionColor + radiance * SpecularColor * SpecularOcclusion`, the sky form's times `EnvironmentParams.x`, the
  ambient form's without the specular term). `reflections` then changes the compose step's specular term too, and the owner
  table of 8.7 says so.
- **Cost.** Information: the 84-set's `stages.csv`, `GPU.Opaque` and `GPU.Transparent` per picture against the base capture,
  median ratio within the control's run-to-run (the slang audit's 0.967-1.0 range); the filler adds a `pow` the ambient form
  skips and nothing else.

### 8.6 Slots, uniform fields, GPU stages, debug views

- `SH/Include/Scene.glsl` (`Include/Scene.slang`) 7-12 becomes:

  ```glsl
  // The scene layout's slots (SceneRenderer::CreateResources): SRV 0-21, 27, 28, 29 (the aerial perspective volume), 32 and 33,
  // samplers 0-5 (materials), 8, 9 and 10, CBV 0.
  // SRV 22-26 belong to the compute layouts of temporal anti-aliasing and screen-space reflections, never to this one.
  // Assigned in wave 7: SRV 28 the sun's screen-space shadow mask, 32 the cascades' depth ranges and 33 the shadows'
  // blue-noise tile; 29 the aerial perspective volume. Assigned for wave 8: 30 and 31 the irradiance volume (radiance-cascades,
  // below its cut), 34 volumetric fog (volumetric), 35 the radiance cascades' indirect light (radiance-cascades), 36 fog
  // volumes (volumetric, below its cut). A new scene resource takes the next free slot (37), and a pass that does not run
  // binds a dummy.
  ```

- `FrameConstants::ReflectionParams` (Scene.glsl:107, `R/SceneRenderData.h:101`): "x: 1 when the frame traced screen-space
  reflections (SRV 18); y: 1 when the frame's indirect light comes from the radiance cascades (SRV 35), z: their intensity
  (radiance-cascades); w: reflections'". Still written 0 for y, z, w (`R/SceneRenderer.cpp:3019`) until those streams.
- GPU stages, registered by their streams with the five lists (DG "GPU timing"): `GPU.Volumetric` after `GPU.Shadows` (as
  wave 7 assigned: injection needs the shadow maps, no depth), `GPU.RadianceCascades` after `GPU.AO` (its pass runs between
  AO and reflections, `R/SceneRenderer.cpp:3339-3352`).
- Debug views, appended by `radiance-cascades` after `Albedo` in `ViewportDebugView` (`E/StreamingView.h:26-41`) and its
  names (47-48, `ViewportDebugViewToString`), and in `Tools/LookDev.py`'s `DEBUG_VIEWS`: `gi` (`GlobalIllumination`, the gathered indirect light) and
  `open` (`OpenShare`, the share the cascades leave to the fallback).
- No dummy or binding is added by S0.11: each stream binds its slot with a dummy when its pass does not run.

### 8.7 Who owns what after S0.11 (plan 4's table, with the functions)

| Region | Owner |
| --- | --- |
| `IndirectInputs` and `GetIndirectInputs` | Shared and additive: new fields at the end, filled in `GetIndirectInputs` |
| `GatherIndirectSpecular` (and `SpecularOcclusion`'s formula) | `reflections`; `radiance-cascades` adds the open share to specular occlusion after `reflections` merged |
| `GatherIndirectDiffuse` | `radiance-cascades` |
| `ComposeIndirect` | S0.11's; changed only by the fallback of 8.5 or by agreement of both streams at merge |
| The form constants | S0.11's; a stream that needs a fifth form appends it |

### 8.8 Docs and commit

- DG "Lighting" gains a bullet after "The horizon fade": "**The indirect light's seam** (`Include/IndirectLight`): the
  forward pass fills `IndirectInputs` once per pixel (`GetIndirectInputs` in `Scene/Forward.frag`: the split sum's
  reflectance, the reflected direction, the geometric normal, the occlusions, the probes, the screen-space reflection, the
  sky's share and its two cube reads), and the base's indirect light is `ComposeIndirect(GatherIndirectDiffuse,
  GatherIndirectSpecular)`, keeping 0.7.0's four forms (`ST_INDIRECT_PROBES`, `SCREEN`, `SKY`, `AMBIENT`) bit for bit; the
  clear coat follows. Streams add fields at the end of the struct and change their own gather (wave 8's plan: the specular
  gather is `reflections`', the diffuse gather `radiance-cascades`')."
- DG "GPU timing" (5201-5203), after the slot sentence: "Wave 8's assignments are in `Include/Scene` (SRV 30-31, 34-36;
  `ReflectionParams.y` and `.z`) and its plan (`GPU.Volumetric` after `GPU.Shadows`, `GPU.RadianceCascades` after `GPU.AO`;
  debug views `gi` and `open` after `albedo`)."
- No release-note line (no behavior changes).
- Commit: "Split the forward pass's indirect light into two gathers over shared inputs" (body: the 84-set's result, the GPU
  suites, the A/B ratio), then "Assign wave 8's scene slots, stages and debug views" (comments and DG).

## 9. S0.12 The roadmap

Applied from `.worktrees/wave8-plan/roadmap.md`, corrected and brought up to what step 0 built, in one docs commit on
`agent/wave8-last` after S0.11 ("Record wave 8's plan and wave 9's animation work in the roadmap"):

1. **"### Wave 8"** after "### Wave 7" (before 1417): roadmap.md's lines 1-59, with the step 0 paragraph (16-24) rewritten
   as built: what each lane landed, with its numbers (S0.3's changes, S0.2's ScreenShadows A/B), and whether Slang landed
   or moved to wave 9's step 0. Two lines of the draft predate the owner's answers of 2026-11-01 and change (review):
   - `reflections` (33-36): "Below the cut: keyframed probe captures, a source radius for highlights, ..." becomes
     "Highlights lead R2 (the owner: \"highlights are extremely important just like in a drawing, because they should be
     cheap\"): the sun's size in specular, lantern and fire highlights on metal and wet ground, and a source radius for
     highlights only, each judged with no AA, FXAA and TAA. Below the cut: keyframed probe captures, hit-distance blur, a
     wet-surface model, normal-map-aware filtering, anisotropy, SSR on clear coat." (decisions.md, "Highlights first"; plan
     4.3 step 7).
   - `animation-graph` (46-48): "Strata's own module on Esoterica's architecture" gains ", reading Esoterica's source at
     c99a946 and porting its runtime semantics under its MIT notice (the owner allowed open-source code on 2026-11-01)"
     (decisions.md D10; plan 4.5).
2. **Status**, a block after wave 7's (before the open items at 609): "**Wave 8 in progress (step 0 landed, <date>).**" with
   the 0.7.0 baseline's location (`.worktrees/wave8-plan/lookdev/0.7.0`, `0.7.0-medium`), the kept base
   (`.worktrees/wave8-base`), the object store's repair (or that it could not run, 4.1), AGENTS.md's count after step 0,
   golden rule 1's new scope (open-source code under permissive licenses, D10), and what step 0 left open.
3. **"### Later (wave 8 and beyond)"** becomes **"### Later (wave 9 and beyond)"**:
   - Remove "Wave 8 starts with `showcase`..." (1425-1434); the wave 8 entry has it.
   - First, "**Wave 9, recorded in wave 8's step 0:**" (plan section 11): `animation-tools` (the node canvas through
     `EditorPanelRegistry` with every edit a command, live debugging and a task view, a clip timeline for events and sync
     markers, a mask editor, recording); `animation-runtime-2` (Blend2D with Strata's own triangulation, selectors, referenced
     and external graphs, variations, typed clip events, additive clips, float curves, two-bone and foot IK grounded through
     `IPhysicsQueries`, warps, forced transitions, skeleton LOD, compressed clips if measured to help, whether version 1
     controllers move onto the graph path); ragdoll nodes after a physics joints stream; skin and a character (pre-integrated
     skin, thin translucency if `volumetric` did not land it, screen-space scattering only for close-ups of faces, a distant
     figure in the showcase if a realistic CC0 or CC-BY body is found); what tranche 3 and the cut lines left; Slang as step 0
     if its box ran out. These stand whatever happens to `animation-graph`.
   - Replace "`animation-authoring`" (1464-1465) by a pointer to the wave 9 entries above.
   - "Rendering after wave 7" becomes "Rendering after wave 8": remove volumetric fog and light shafts, and the baked
     irradiance volume, unless they fall out of the wave again; add a replacement for contact shadows (a crossing test
     instead of a thin shell, noise not locked to the screen, look-dev in every sample), gap-aware cascade slices for a near
     object that stays (with hysteresis of their own), and the Physics Playground's empty first cascade (nearest receiver
     18.9 m, first split 18.66 m).
   - roadmap.md lines 68-69 as drafted: "Sunlight thrown off bright metal onto nearby surfaces (the owner wants it, \"not a
     priority\"), planned after `radiance-cascades` lands, its technique chosen then." (Review: the author's "correction"
     was wrong. The words are the owner's, in the answer of 2026-11-01 that decisions.md quotes verbatim: "And the sunlight
     thrown off the sheet onto other things is also something I was thinking to do, but it is not a priority." Item 4's
     reading is answered, so nothing is asked with the first sheet; decisions.md puts the item in "Later" after
     `radiance-cascades`.)
   - Console seams as an inventory in the guide (script modules without dynamic libraries, crash containment without SEH
     and signals, threads and paths behind the platform layer), for when an SDK exists.
4. **Open items:** the contact shadow fringe at 1412 moves to "Later" with the replacement march; the claim at 1276-1278
   gains "(since 0.8.0 contact shadows are off in every preset; the fitted cascades give the strip: <lane Render's finding,
   6.3>)"; Medium's Checkerboard reflections named as the one effect that needs TAA; "more than 16 shadowed lights" and the
   screen-space reflection items stay until `local-lights` and `reflections` land.
5. `Docs/Plans/Wave7/`: the Status sentence "its plan, reviewed specs and release notes are in `Docs/Plans/Wave7/` until it
   lands" (575-576) becomes "... are in `Docs/Plans/Wave7/`, kept while wave 8 builds on them".

Acceptance: `Docs.AgentGuide` (unchanged file sizes; the guide's links), and a read against plan sections 1.2 and 11 and
decisions.md that every owner item, every wave 9 entry and every ruling of 2026-11-01 (D10, highlights first, the thrown
sunlight in "Later") appears, and that every sentence put in quotation marks as the owner's is found verbatim in
`owner_feedback.md` or decisions.md's "The owner's answers".

## 10. How step 0's lanes land

### 10.1 Worktrees and branches

Every worktree by `sh .worktrees/create_worktree.sh <name> <base>` (branch `agent/<name>`): it initializes the submodules
from the main checkout's copies without network, which a plain `git worktree add` (as `step0-editor.md` 1260 and
`step0-platform.md` 1216 write) does not. Builds follow AGENTS.md's parallel-agents rule inside each worktree:
`cmake --preset windows -B build/agent-<lane>` (`build/agent-wave8-editor` and `build/agent-platform` as the lane specs name
them, `build/agent-wave8-render`, `build/agent-wave8-slang`, `build/agent-wave8-last`, `build/agent-wave8-first`); only
`.worktrees/wave8-base` uses the preset's own `build/windows` (4.4). No two builds share a directory.

| Worktree | Holds | Base | CI branch |
| --- | --- | --- | --- |
| `.worktrees/wave8-base` | 76f28a21, kept (4.4) | 76f28a21 | never pushed |
| `.worktrees/wave8-first` | S0.0d, S0.1 | main after 4.1 | `ci/wave8-first` |
| `.worktrees/wave8-render` | S0.2, S0.3 | main after S0.1 | `ci/wave8-render` |
| `.worktrees/wave8-editor` | S0.7, then S0.4, S0.5 | main after S0.1 | `ci/wave8-editor` (pushed at S0.7's tip first) |
| `.worktrees/wave8-platform` | S0.9 first (landed alone, `step0-platform.md` 167), then S0.8, S0.6 | main after S0.1 | `ci/wave8-platform-timing` (S0.9), then `ci/wave8-platform` |
| `.worktrees/wave8-slang` | S0.10 | main after S0.1 | `ci/slang` (plan) |
| `.worktrees/wave8-last` | S0.11, S0.12 | main after lane Slang lands (or its box ends) | `ci/wave8-last` |

### 10.2 What each landing verifies

Every landing: an independent review with each finding fixed or answered; then the gates below; then `ci/<name>` pushed and
`python Tools/CIStatus.py status <commit>` green on all six jobs (Windows, Windows Debug, Linux, macOS, Linux without Audio
and Physics, Windows Dist); then main fast-forwards to the lane's tip (`git -C G:/GameEngineAI merge --ff-only agent/<name>`,
`git push origin main`).

| Landing | Local gates |
| --- | --- |
| first (S0.0d, S0.1) | Markdown only: `StrataTests` built in Release in the worktree (`--target StrataTests`), `--test-suite=Docs*,Editor.UI.Text*,Editor.Launcher*,Samples.Content*` passes (the last three read `ThirdPartyNotices.md`, which 5.5 changes and the editor embeds); the code point count 36,836 |
| Render | Debug ctest and every `GPU*` suite under the validation layer; Release ctest `-LE perf`; `CompatLint.py --changed-since <S0.1 tip>`; feature test; 6.5's and 7.8's measurements; the 84-set at the project's quality and at Medium against 0.7.0 (contact shadows' change only at High and Ultra; none at Medium) |
| Editor (S0.7 alone, then the rest) | As lane Editor's spec; at least Debug ctest with `GPU.Editor*` under the validation layer, Release ctest `-LE perf`, CompatLint, MCP `core` within 32 tools |
| Platform | As its spec; at least Debug and Release ctest, `StrataEditor.Smoke` and `QuitScreenshot` (the surface path), the layering rule failing on 76f28a21's `VulkanGraphicsDevice.cpp:21` before its step 1 and passing after, the CI row without GLFW |
| Slang | Its six gates (plan S0.10), through `ci/slang` with the spirv-val and Metal conversion steps; its last act: the 84-set in Slang after merging main |
| last (S0.11, S0.12) | Debug `GPU*` suites under the validation layer, Release ctest `-LE perf`, CompatLint, 8.5's 0 of 84, `Docs.AgentGuide` |

### 10.3 Order and merges

- first lands alone, before any lane branches. Then Render, Editor, Platform and Slang branch from its tip and land in the
  order they are verified (S0.7 ahead of its lane if ready first: the showcase's graybox waits for it and for Render).
- A lane that lands after main moved merges main into itself (never rebases a pushed branch), reruns Release ctest `-LE
  perf` and the Debug suites of the files the merge touched, pushes `ci/<name>` again and waits for green before main moves.
  Lane Slang merges main before its last verification, with whatever has landed (plan S0.10).
- Files two lanes edit, and how they resolve: `E/EditorStateCommands.cpp` (Render's `shadowFit` fields and `editor.perf`
  text; Editor's `interval` and `editor.status` fields): keep both. `Docs/DeveloperGuide.md` (Render: "Sun shadows", "Quality
  settings"; Slang: 253 mentions of `.glsl`, among them Render's new text, which the Slang merge renames). `AGENTS.md` (Slang,
  Platform; the later lander measures). `Strata/CMakeLists.txt`, `Strata/vendor/CMakeLists.txt`, `ci.yml` (Platform, Slang).
  `Docs/Roadmap.md` (lane Platform's lines, `step0-platform.md` 455 and 995, and lane Editor's hand-over, then S0.12 after
  them; lane Render edits no roadmap line: S0.12 carries its finding, 6.3). `Docs/Plans/Wave8/ReleaseNotes.md` (every lane
  appends its own block). `R/SceneRenderer.h` (Render: `ShadowFitHolds`, `StepSeconds`, `ShadowFitHolding`; other lanes:
  none expected): keep both.
- last branches after lane Slang (rendering streams branch from its tip, plan S0.11); if Editor or Platform lands after it,
  last merges main and takes the 84-set again only if they changed a scene picture (neither should).

### 10.4 When `build/windows` is rebuilt

By the orchestrator, after S0.0b's baseline, never while `PerfLock.py --status` names a holder, in the slot the landing lane
just freed: `cmake --preset windows` (new files) then `cmake --build build/windows --config Release --parallel` on main's
checkout (Release: the build the owner runs; Debug only if the owner asks). After each landing the owner can see, with a line to
the owner naming what to look at:

| After | The owner sees |
| --- | --- |
| Render | Contact shadows gone at High; the Physics Playground's shots without the shadow jump |
| Editor's S0.7, then the rest | Drag and drop with a box ghost; then the frame rate preferences and the Performance pause |
| Platform's S0.9, then the rest | `FixedRate` in Project Settings; then the FIFO finding (GLFW's seal shows nothing) |
| Slang | Nothing should look different: the owner's editor runs the shipped compiler's shaders |
| last | Nothing visible: `build/windows` equals main when the streams start |

The first landing (S0.0d, S0.1) changes no binary and needs no rebuild.

### 10.5 Slots during step 0

The four lanes hold the four build-heavy slots. A landing's verification runs in the landing lane's slot; the rebuild of
`build/windows` follows in the same slot. The showcase's sample agent starts at once (plan 4.4) without a build: until lane
Render lands, `.worktrees/wave8-base`'s editor is main's code (4.4). `physics-timing` waits for lane Render's landing and
takes the first slot a lane frees.

## 11. Perf metrics

These items add no metric to `Budgets.json`, and change none. Their A/Bs are information, interleaved against
`.worktrees/wave8-base` under `PerfLock.py`, three pairs (A B, B A, A B), `STRATA_TEST_GPU_VALIDATION=0`, nothing else measuring:

| Item | A/B | Expected |
| --- | --- | --- |
| S0.2 | 6.5: `PerfGPU.Renderer` 100,000 shadowed cubes at Medium and High | `ScreenShadows4KHigh` 0.15-0.3 ms lower; Medium within noise |
| S0.3 | 7.9: `PerfGPU.Renderer`'s CPU metrics | Within the A/B's noise (wave 7's as-built: code layout alone moves single CPU metrics by up to 7%) |
| S0.11 | 8.5: the 84-set's `GPU.Opaque` and `GPU.Transparent` | Median ratio within the control's run-to-run |

The 0.7.0 baseline's `stages.csv` (4.3) is the "0.7.0 (measured)" GPU busy of plan 1.1's last two rows: the samples' scene
cameras at High 3840x2160 and Medium 1920x1080, captured alone.

## 12. What step 0 leaves the streams, and how their merges resolve

- **The baseline and the base:** every stream's look-dev compare starts from `$L/0.7.0` or main's capture; every A/B runs
  against `.worktrees/wave8-base` under the lock (D4).
- **AGENTS.md:** about 2,330 code points free when the streams branch (5.4); each stream one digest bullet of at most 250;
  the later lander measures after merging main, and a lander that would pass 40,000 shortens a bullet of its own area into
  the guide.
- **Golden rule 1 (5.5):** a stream that ports code (expected: `animation-graph`, Esoterica under MIT) adds the first row and
  license text to `ThirdPartyNotices.md`'s "Code derived from other projects" and names the source in its commit message.
- **The fit (S0.3):** `physics-timing` repeats 7.8's measurements with interpolation on (interpolated poses move proxies
  every frame, so holds now see per-frame motion; they count seconds, so the frame rate does not change them). The showcase's
  graybox checks `c_NearNowShare` and `c_GrowSeconds` against its traffic (plan 4.2 step 4); a change of either is a
  constant and a test's numbers, recorded in the roadmap.
- **The seam (S0.11):** 8.7's table. `reflections` merges before `radiance-cascades` (plan 4); `radiance-cascades` then adds
  the open share to `SpecularOcclusion` inside `GatherIndirectSpecular` or `GetIndirectInputs`, never in `ComposeIndirect`.
  `volumetric` applies fog after the indirect light, in `main`, outside the seam.
- **Slots and stages:** 8.6; a stream that needs another slot takes 37 and updates the comment.
- **Quality rows:** S0.2 bumps nothing; each stream's landing bumps `c_SectionVersion` from 4 (plan 4.1; the integrator
  renumbers).
- **Release notes:** `Docs/Plans/Wave8/ReleaseNotes.md`, one block per lane or stream.

## 13. Risks and open questions, with the decision recommended

| # | Question or risk | Decision |
| --- | --- | --- |
| Q1 | The object store is damaged (2.2), unknown to the plan | Repair first from origin with the damaged file and the packs kept (4.1); if it cannot run, report it and go on: only history walks across f99f3a41 are lost (review) |
| Q2 | Can `base2`/`base3` be the 0.7.0 baseline? | No: `base3` is gone and `base2` lacks 1080p, Basic3D, live shots and quiet GPU times. `base2` stays lane Slang's 4K reference and checks `.worktrees/wave8-base` (4.2) |
| Q3 | How to keep a 76f28a21 build cheaply | A Release-only build of its own worktree (4.4); a copy of binaries would read main's budgets and samples through `STRATA_SOURCE_DIR` |
| Q4 | The plan's 692 free characters | 708 code points (the plan counted bytes); S0.1's sixteen bullets free 2,633 and golden rule 1 takes 177, leaving 3,164 (review) |
| Q5 | The prototype lacked a transparent rule and failed a case | The transparent bound moves N at once, and only that far; opaque growth beyond it still waits (7.3 step 3) |
| Q6 | Held receivers in narrow or orthographic views fall outside every cascade (the prototype did not handle it) | Grow at once where the bound lies beyond `D k`, the rule `GetFittedNearest` already applies to outliers (7.3 step 3) |
| Q7 | "The far bound stays": does its shrink wait 0.5 s? | Its growth stays at once; its shrink takes 8 readbacks and 0.5 s like every bound, as the prototype measured |
| Q8 | Is a fit with a waiting growth settled? | No: `IsSettled` is false while holding. Review: this alone made TAA captures depend on frame times (7.3a); captures and thumbnails now keep 0.7.0's rules (`ShadowFitHolds` off), so the coupling concerns live views' statistics only |
| Q9 | Wall-clock holds make tests timing dependent | `AdaptationSeconds` drives the holds; new and fit tests pass it; the rule is in DG (7.5, 7.6) |
| Q10 | A persistent near object re-cuts once; a small one waits 0.5 s in a coarser cascade (audit risks) | Accepted for wave 8, in the release notes' known issues; gap-aware slices in "Later"; constants checked on the showcase's traffic |
| Q11 | S0.11's bit-equality depends on how the driver contracts multiply-adds across inlined functions (unverified) | The gate is 0 of 84 and the GPU suites; on any difference, the fallback shape of 8.5 first |
| Q12 | The roadmap draft quotes the owner ("not a priority") | Review: the words are the owner's (decisions.md, "The owner's answers", 2026-11-01); the draft stands, and S0.12 brings the two lines the answers changed up to date (section 9, item 1) |
| Q13 | `ScreenShadows4KHigh`'s meaning changes with S0.2 | Kept `Provisional` with its target; the commit states the change and the medians |
| Q14 | A Custom quality saved before the contact shadow field existed reads 0 steps | Accepted without a version bump; one release-note line (6.1, 6.4) |
| Q15 | S0.0b and S0.1's build compete with the lanes | S0.0b runs before any lane starts; S0.1's only build is `StrataTests` in Release after the capture |
| Q16 | Golden rule 1's scope (D10) was missing from S0.1 | 5.5: the rule shortened in AGENTS.md, in full in DG, a section in `ThirdPartyNotices.md` (review) |
| Q17 | Captures with wall-clock holds | Renderers of single images keep 0.7.0's rules (7.3a) |
| R3 | On a still camera with TAA, a jitter phase that asks for a wider square now waits and is dropped, so the live view's `ShadowFitHolding` (and `Settled`) can flicker on those readbacks | Nothing reads those flags in live views but statistics; the GPU case's TAA block checks it (7.5). If a scene shows it, the hold could ignore growths not asked by `c_TemporalJitterPhases` readbacks in a row: decided then, with the case's numbers |
| R1 | The 0.7.0 capture takes longer than the lock's limit | `--limit 150`; a run cut short is resumed per project with `--projects` into the same output, `--work` reused |
| R2 | The base build differs from the owner's 0.7.0 pixels (another toolchain update since 2026-10-29) | 4.4 step 5 catches it; if so, the owner's `build/windows` is not rebuilt until the cause is known and the base made again |

## 14. First hour

1. `python Tools/PerfLock.py --status` names no holder; no build runs (`tasklist | grep -i msbuild` empty).
2. Read `plan.md`, `decisions.md`, this spec, `shadow-jump/audit.md`, and `git -C G:/GameEngineAI show
   76f28a21:Docs/Plans/Wave7/specs/step0.md` for the form.
3. Orchestrator: 4.1's fsck and repair; then 4.3's capture (it runs about an hour: write S0.0d and S0.1 meanwhile in
   `.worktrees/wave8-first`, without building).
4. S0.1: apply the sixteen replacements of 5.1 and the four moves of 5.2, then golden rule 1 (5.5) as its own commit; count
   with a script in the scratchpad using the test's rule (target 36,836); check every dropped phrase against 5.3's lines.
5. After the capture: 4.4 (build `.worktrees/wave8-base`, check it), then build `StrataTests` in `wave8-first`, run
   `Docs*`, push `ci/wave8-first`.
6. Lane Render's agent, once first has landed: create `.worktrees/wave8-render`, apply S0.2 (6.1-6.4) and run
   `Renderer.Quality` and `GPU.SceneRenderer.ContactShadows`; then read `patch_private.py`, `patch_private2.py` and
   `patch_tests.py` beside `R/ShadowFit.cpp` and start 7.2's commit (with 7.3a's `ShadowFitHolds`, off in the capture
   renderer).

## 15. Review corrections

The independent review (2026-11-02) checked this spec against the code at 76f28a21, the plan, decisions.md, the owner's
list and the audits, and against `step0-editor.md` and `step0-platform.md` (the other step 0 specs present then; the
Slang, showcase, reflections, physics-timing and radiance-cascades specs did not exist yet). Confirmed as written: the
object store's damage (`git cat-file` fails on 22eb9723, `git log -- <path>` stops at it), AGENTS.md's 39,292 code points
and the sixteen bullets' before and after counts, every RendererQuality, ShadowFit, SceneRenderer, GPUProfiler and test
file:line checked, the prototype's rules (`patch_private.py`, `patch_private2.py`), and S0.11's four forms, whose
expressions match `Forward.frag:250-300` operand for operand. Changed:

1. **Golden rule 1 (D10) was missing from S0.1** (blocker). The plan's S0.1 and decisions.md D10 both give it to S0.1. Added
   5.5: the rule in AGENTS.md (391 code points, shortened), its full wording in DG "Working with third-party code", and a
   "Code derived from other projects" section in `ThirdPartyNotices.md`, as a commit of its own; the first landing's gates add
   the three suites that read that file (10.2).
2. **S0.12 "corrected" a quote the owner did write** (blocker). "Not a priority" is the owner's, in the answer decisions.md
   quotes verbatim; item 4's reading is answered. Section 9 keeps the draft's line, and updates the two draft lines the
   owner's answers changed (highlights lead `reflections`' R2; `animation-graph` ports Esoterica under MIT). Q12 rewritten;
   S0.12's acceptance now checks every quotation against the owner's words.
3. **Captures would have depended on frame times** (major). Wall-clock holds in the capture renderer, with settled coupled
   to holding and `viewport.capture` resetting TAA history while the fit is unsettled, could change TAA pictures by machine
   speed, undermining the look-dev baseline, every A/B and S0.11's 0-of-84 gate. Added 7.3a
   (`SceneRendererSpecification::ShadowFitHolds`, off for the single-image renderer), two CPU cases and a capture check
   (7.5), the DG sentence (7.6), and rewrote 7.8's "settled layouts unchanged" row as "captures unchanged". Q8, Q17, R3.
4. **S0.1's test bullet contradicted practice** (major). "Never change their own environment" is false for the 23 files
   that use `Tests::ScopedEnvironmentVariable`; the rule is "never for a child". Replacement fixed (5 code points).
5. **S0.1 dropped two musts most of this wave's streams need** (minor). The shadow-cache bullet keeps "draw order" and the
   `GPU.SceneRenderer.GPUDriven` and `.Feedback` suites (110 code points): `local-lights`, `radiance-cascades` and
   `showcase-engine` change `Cull.comp`, the extraction or the caches.
6. **AGENTS.md's budget omitted lane Editor** (major). With 1, 4 and 5: 36,836 code points after S0.1 (3,164 free, the
   plan's 3,000 met); lanes Slang (about 450, unverified), Platform (201) and Editor (179) leave about 2,330 for nine
   streams' 2,250. 5.1, 5.4, 10.2, 12, Q4 and the first hour updated; a lander that would pass 40,000 shortens a bullet of
   its own area.
7. **The object store repair** (minor). `repack -A -d` instead of `-a -d` (keeps unreachable packed objects), a pack backup
   first, no git process during it, and a repair that cannot run no longer blocks the wave (4.1, Q1, section 3's table).
8. **Release notes skeleton** (minor): "## Defaults that change" added, as plan section 6 and wave 7's notes have it.
9. **Landing details** (minor): builds in `build/agent-<lane>` inside each worktree as AGENTS.md and the lane specs say;
   worktrees only through `create_worktree.sh` (submodules); lane Platform's S0.9 lands alone through
   `ci/wave8-platform-timing`; Roadmap.md's merge order names lane Platform's lines, not lane Render's (10.1, 10.3, 10.4).
10. **Smaller facts**: DG's Threading, NVRHI and Assets line ranges (838-852, 853-879, 880-885); 5.2 (a) follows the bullet
    ending at 426; the changed CPU case's large receiver is 11.1% of the receivers, not 12.5%; the hold block's exact place
    (after `quarters`, 413); `RenderShadowView` steps time too; the TAA block of "A still camera keeps..." changes meaning;
    the contact-shadow GPU case may need a fresh renderer per frame; 2.1 gained `ThirdPartyNotices.md`,
    `ViewportRenderer.cpp` and `EditorViewportCommands.cpp`.
