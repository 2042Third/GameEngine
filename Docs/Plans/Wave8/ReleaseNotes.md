# Wave 8: release notes for 0.8.0

What wave 8 brings, as step 0's lanes and the streams land on main (`plan.md`, `specs/`). `Docs/Releases/0.8.0.md` is
written from this file when 0.8.0 is released. Each landing appends a block of its own under each heading it has lines
for, its lane or stream named first in bold, so that merges only add. In the defaults table a landing's rows name it in
the Where column, as plan section 6 does; in the upgrade table its block starts with a row that names it in bold.

## What's new

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

## Defaults that change

Stored values never change: a scene or project that saved a value keeps it. These change what new components and
projects, the template and scenes or projects without the value get.

| What | 0.7.0 | 0.8.0 | Where |
| --- | --- | --- | --- |

## Upgrading from 0.7.0

| What | 0.7.0 | 0.8.0 | To keep the old |
| --- | --- | --- | --- |
| **Lane Editor (S0.7)** | | | |
| A prefab dropped through Game camera | Under the editor camera's pivot | Under the pointer, as the scene's camera sees it | Turn Game camera off before dropping |
| A drop where nothing is under the pointer | Under the view's pivot | On the grid plane (y = 0, within 500 m), else in front of the camera | Use Add to Scene, which places under the pivot |
| A prefab with several top-level entities | Only the first moved to the drop | All move together, keeping their offsets | None needed: the template's layout is kept |
| Dropping an asset the view cannot take | A Console warning | A label at the pointer, nothing logged | None |
| **Lane Editor (S0.4)** | | | |
| The editor's idle throttling | Constants: idle after 0.5 s, 30 frames per second, 10 in the background | The `Editor.FrameRate` preferences, with those defaults | Nothing to do |

## Known issues
