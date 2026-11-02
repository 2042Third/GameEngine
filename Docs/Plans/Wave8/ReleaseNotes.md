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

**Slang (S0.10)**

- `Tools/LookDev.py compare` writes a signed difference image for every picture that differs (red where the new build
  is brighter, blue where it is darker) and a per-mode summary, so a change that shifts a region one way stands out from
  rounding; `selftest` runs its offline checks without an editor (`StrataTools.LookDevOffline`, on every CI job).
- Shaders are written in Slang with HLSL syntax and compiled by slangc 2026.20, still to SPIR-V for Vulkan and MoltenVK:
  the step toward other platforms' shader compilers. Pictures differ from 0.7.0 by rounding only (at most a few levels on
  edge pixels; with TAA, small differences spread through the history). Every shader also compiles to HLSL in a test on
  every CI job, CI validates every embedded module with spirv-val and converts it to Metal, and new shaders follow the
  rules of `Docs/DeveloperGuide.md` ("Shaders").

## Defaults that change

Stored values never change: a scene or project that saved a value keeps it. These change what new components and
projects, the template and scenes or projects without the value get.

| What | 0.7.0 | 0.8.0 | Where |
| --- | --- | --- | --- |
| Compiled shaders | glslang | slangc; pictures differ by rounding only | S0.10 |
| First configure of a clone | offline after submodules | fetches Slang's archive once, or `STRATA_SLANG_ROOT` | S0.10 |

## Upgrading from 0.7.0

| What | 0.7.0 | 0.8.0 | To keep the old |
| --- | --- | --- | --- |
| **Platform (S0.9)** | | | |
| `input.*` durations in `seconds` before play starts | Checked against 1/60 s | Checked against the edited scene's step (the project's rate when it sets one), the step play will run | Nothing to do: while playing they always counted in the scene's step |
| Code reading `SceneSettings::FixedTimestep` or `MaxFixedStepsPerFrame` for the step a scene runs | The scene's values | Still the scene's stored values, which a project rate overrides | Read `Scene::GetFixedTimestep` and `Scene::GetMaxFixedSteps` |
| **Slang (S0.10)** | | | |
| The glslang submodule | `Strata/vendor/glslang` | gone: after pulling, the folder (about 900 MB, which git leaves) may be deleted | Keep the folder while you build commits from before 0.8.0 |
| The shader compiler | glslang, built with the engine | Slang's release archive (25-64 MB), downloaded by the first configure into a per-user cache (`%LOCALAPPDATA%\Strata\Toolchains`, `~/.cache/strata/toolchains`, `~/Library/Caches/Strata/Toolchains`) and checked by its SHA-256 | Offline: download the archive `CMake/StrataSlang.cmake` names on another machine, extract it and configure with `-DSTRATA_SLANG_ROOT=<folder>` |

## Known issues

**Slang (S0.10)**

- Platforms: this release changes every shader. Its GPU tests ran on Windows with an NVIDIA GPU. On the Mac, CI converted
  every shader to Metal and compiled it (the 15 that read the bindless table only at run time, through MoltenVK's
  argument buffers, as before), but no GPU test ran through MoltenVK. On Mesa's lavapipe (a CPU driver, tried on CI), 529
  of 553 GPU tests pass with the new shaders as with the old, and the same 24 fail with both; AMD and Intel GPUs have not
  run them.
