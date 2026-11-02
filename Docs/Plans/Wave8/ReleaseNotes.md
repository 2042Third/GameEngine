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

## Defaults that change

Stored values never change: a scene or project that saved a value keeps it. These change what new components and
projects, the template and scenes or projects without the value get.

| What | 0.7.0 | 0.8.0 | Where |
| --- | --- | --- | --- |

## Upgrading from 0.7.0

| What | 0.7.0 | 0.8.0 | To keep the old |
| --- | --- | --- | --- |
| **Platform (S0.9)** | | | |
| `input.*` durations in `seconds` before play starts | Checked against 1/60 s | Checked against the edited scene's step (the project's rate when it sets one), the step play will run | Nothing to do: while playing they always counted in the scene's step |
| Code reading `SceneSettings::FixedTimestep` or `MaxFixedStepsPerFrame` for the step a scene runs | The scene's values | Still the scene's stored values, which a project rate overrides | Read `Scene::GetFixedTimestep` and `Scene::GetMaxFixedSteps` |

## Known issues
