# High Desert Outpost

A derelict fuel stop in a rocky high-desert valley the day after rain: a bus with flat tyres on a cracked, wet asphalt
apron, a weathered steel oil tank, a green fuel tank, a plaster-and-plank hut, drums, a fire pit and lanterns, boulders
and a cliff band, with the low sun's highlights on sheet steel and wet ground at three times of day. It is Strata's
graphical showcase, made by an AI agent through the editor alone, the way
[`strata-make-a-game`](https://github.com/2042Third/GameEngine/blob/main/.claude/skills/strata-make-a-game/SKILL.md)
describes.

![The sample as the launcher shows it](Picture.png)

## State of this version

The sample is being built in steps. This version holds the project and its scanned assets, imported and credited:

| Folder | What |
| --- | --- |
| `Assets/Scans/` | Twelve Poly Haven models at 1K: boulders and a cliff, dead brush, drums, a fire pit, a lantern, a tyre, a covered car, a ladder. The rocks keep six levels of detail, the lantern and the ladder five |
| `Assets/Textures/` | Ten Poly Haven texture sets (base color, OpenGL normal map, and AO, roughness and metalness packed as `arm`) for the ground, the apron, mud, plaster, planks and painted sheet metal |
| `Assets/Generated/Terrain/` | The ground, written by the engine repository's `Tools/GenerateShowcaseTerrain.py` and imported: a 300 m valley at 1 m resolution inside a 2.7 km ring of hills and mountains, the asphalt apron with its potholes and shallow puddles, their water and mud patches. The generator also checks the design the shots rest on (the low sun clears the hills from the apron, pads are flat) and computes where each shot's camera sees the sun reflected |
| `Assets/Generated/Props/` | The props, written by the engine repository's `Tools/GenerateShowcaseProps.py` and imported: a derelict bus of painted and bare sheet steel (its letterboard, where the low sun's highlight runs, is vertical), a weathered steel oil tank with its weld seams, pipes and a valve, a green fuel tank on saddles, a plaster-and-plank hut with a lean-to, embers and a lantern's wick; and the bare steel's color and metal/roughness maps, one tileable rust field over 4 m |
| `Assets/Materials/` | The materials: scans with their real sizes as tiling, the painted metal sets dielectric (their metalness maps are empty), bare steel metal by its own map |
| `Assets/Prefabs/` | Bus (on six scanned tyres), OilTank (with its pipes and a leaning ladder), FuelTank, Hut (with its lean-to), FirePit (stones, embers, the light `Fire`) and Lantern (wick and the light `Light`), each with convex colliders |
| `Assets/Scenes/Outpost.stscene` | The start scene, laid out by `GenerateShowcaseTerrain.py layout`: the ground with its materials and colliders (layer `Terrain`), every prop on the surface under it (layer `Props`), the cliff band, boulders and dead brush, six reflection probes (`Probe.<Dawn, Golden or Dusk>.<Yard or Hut>`), the tour's markers (`Shot<N>.From`, `.Key`, `.To`, `.Look`), the camera rig (`Player` and its `Eye`), the fog, the overlay and the director `Outpost`. It is saved at golden hour |
| `Assets/Lighting/Outpost/` | The six probes' captures, baked by `GenerateShowcaseTerrain.py bake`: the yard and the hut at dawn, golden hour and dusk |
| `Scripts/` | `Outpost` (the director: the three times of day and the camera tour), `Flicker` (the fire's and the lanterns' flames), and the tables they share with the generator (`Stations.h`, `TourPath.h`) |

Play it and the tour runs: six shots of about 80 seconds, dawn over the valley from the rim, the low sun on the bus's
side, on the oil tank and in the puddles, the hut's interior and dusk with the fire lit; it loops. `T` starts it again.
Each time of day switches its own baked probes on (the old ones stay on for a second, so a reflection never falls to
the sky). The walk, the free flight and the photo mode come next. The materials and prefabs were made by the generators'
command scripts (`GenerateShowcaseProps.py materials` and `prefabs`, `GenerateShowcaseTerrain.py layout` and `bake`),
run in the editor.

## Credits

Every scan is CC0 1.0 from [Poly Haven](https://polyhaven.com); `CREDITS.md` names each one's source and authors.
