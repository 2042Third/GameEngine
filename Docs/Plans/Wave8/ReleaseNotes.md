# Wave 8: release notes for 0.8.0

What wave 8 brings, as step 0's lanes and the streams land on main (`plan.md`, `specs/`). `Docs/Releases/0.8.0.md` is
written from this file when 0.8.0 is released. Each landing appends a block of its own under each heading it has lines
for, its lane or stream named first in bold, so that merges only add. In the defaults table a landing's rows name it in
the Where column, as plan section 6 does; in the upgrade table its block starts with a row that names it in bold.

## What's new

**Lane Render**

- Contact shadows are off in every Graphics Quality preset: they shimmered while the camera moved. The fitted sun cascades
  ground small objects near the camera.
- The sun's shadows no longer jump while something passes close to the camera (a ball thrown from it, a bird): the
  cascades follow a near object that holds less than a tenth of the view only once it has stayed for half a second (an
  eighth while the camera moves), and shrink only after half a second. Something that stays close still moves them once.
  `editor.perf` and `viewport.capture` report the fit's layout `changes` and whether a growth is `holding`.

## Defaults that change

Stored values never change: a scene or project that saved a value keeps it. These change what new components and
projects, the template and scenes or projects without the value get.

| What | 0.7.0 | 0.8.0 | Where |
| --- | --- | --- | --- |
| Contact shadow steps of the Graphics Quality presets | Low 0, Medium 0, High 12, Ultra 16 | 0 in every preset | lane Render |

## Upgrading from 0.7.0

| What | 0.7.0 | 0.8.0 | To keep the old |
| --- | --- | --- | --- |
| **Lane Render** | | | |
| Contact shadows | drawn at High (12 steps) and Ultra (16) | off in every preset | Graphics Quality Custom, Contact Shadows 12 (or 16) |
| A Custom quality saved before contact shadows existed (a development build of wave 7) | read High's 12 steps | reads 0 steps | Graphics Quality Custom, Contact Shadows 12 |
| The cascades' near end | follows any near object above 1% of the view at once | waits 0.5 s (0.12 s moving) below 10% of the view; transparent surfaces and narrow views at once | nothing to keep: settled views are unchanged |

## Known issues

**Lane Render**

- An object that stays close to the camera still re-fits the cascades once, coarsening far shadows; one that covers under
  a tenth of the view waits up to half a second in a coarser cascade.
- With contact shadows off, a thin object's shadow near the camera (a 5 cm plate on the floor) stays solid at High, but
  its edge shows the shadow map's texel steps, as Medium's did in 0.7.0. Graphics Quality Custom with Contact Shadows 12
  smooths it again, at the cost of shimmer in motion.
