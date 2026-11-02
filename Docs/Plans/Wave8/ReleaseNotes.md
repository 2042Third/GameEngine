# Wave 8: release notes for 0.8.0

What wave 8 brings, as step 0's lanes and the streams land on main (`plan.md`, `specs/`). `Docs/Releases/0.8.0.md` is
written from this file when 0.8.0 is released. Each landing appends a block of its own under each heading it has lines
for, its lane or stream named first in bold, so that merges only add. In the defaults table a landing's rows name it in
the Where column, as plan section 6 does; in the upgrade table its block starts with a row that names it in bold.

## What's new

**showcase**

- A fifth sample, High Desert Outpost (`Samples/Showcase`, first in the launcher): a derelict fuel stop in a rocky
  desert valley the day after rain, built from 22 CC0 Poly Haven scans and generated props, made to show the low sun's
  highlights on sheet steel, a weathered oil tank and wet asphalt.
- Two generators keep its ground and props reproducible: `Tools/GenerateShowcaseTerrain.py` (the valley, the apron with
  its potholes and puddles, their water and mud; it checks that the golden sun clears the hills and computes where each
  shot sees the sun reflected) and `Tools/GenerateShowcaseProps.py` (the bus, the tanks, the pipes, the hut, the bare
  steel's maps, and the command scripts that make the sample's materials and prefabs). The CTests
  `StrataTools.ShowcaseTerrain` and `StrataTools.ShowcaseProps` keep the committed files equal to what they write.

## Defaults that change

Stored values never change: a scene or project that saved a value keeps it. These change what new components and
projects, the template and scenes or projects without the value get.

| What | 0.7.0 | 0.8.0 | Where |
| --- | --- | --- | --- |

## Upgrading from 0.7.0

| What | 0.7.0 | 0.8.0 | To keep the old |
| --- | --- | --- | --- |

## Known issues
