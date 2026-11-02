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
| `Assets/Scenes/Outpost.stscene` | The start scene: the golden-hour sky so far |

The terrain and the props come next, written by the generators `Tools/GenerateShowcaseTerrain.py` and
`Tools/GenerateShowcaseProps.py` of the engine's repository and imported, then the layout, the times of day, the camera
tour, the walk and the photo mode.

## Credits

Every scan is CC0 1.0 from [Poly Haven](https://polyhaven.com); `CREDITS.md` names each one's source and authors.
