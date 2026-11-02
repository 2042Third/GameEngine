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
| `Assets/Scenes/Outpost.stscene` | The start scene: the golden-hour sky, the ground with its colliders (layer `Terrain`) and the post-processing |

The layout comes next (every prop at its place, the rocks, the times of day and their reflection probes), then the camera
tour, the walk and the photo mode. The materials and prefabs were made by the generator's command scripts
(`GenerateShowcaseProps.py materials` and `prefabs`), run in the editor.

## Credits

Every scan is CC0 1.0 from [Poly Haven](https://polyhaven.com); `CREDITS.md` names each one's source and authors.
