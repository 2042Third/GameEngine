# Credits

Every file the importers read in this sample, where it comes from and what was changed. CC0 files are in the public
domain; the CC BY 4.0 parts of the fox ask for the attribution below, which the game's credits (Esc > Credits) and
`ThirdPartyNotices.md` show.

| Files | Source | Author | License | Attribution | Changes |
| --- | --- | --- | --- | --- | --- |
| `Assets/Terrain/` | `Tools/GenerateIslandTerrain.py` (terrain) | Strata | Strata | | none: the generator's output as it writes it |
| `Assets/Models/Nature/` | https://kenney.nl/assets/nature-kit (version 2.1) | Kenney | CC0-1.0 | | 35 of 329 glTF models; the scene of each lists its root node (the exporter listed a child node, which glTF forbids) and the colors are dielectric (metallic 0, where the exporter wrote 1 for colors it meant unlit) |
| `Assets/Models/Fox/Fox.glb` | https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/Fox | PixelMannen (model); tomkranis (rigging and animation); @AsoboStudio and @scurest (glTF conversion) | CC-BY-4.0 | Fox: model by PixelMannen (CC0 1.0); rigging and animation by tomkranis (CC BY 4.0); glTF conversion by @AsoboStudio and @scurest (CC BY 4.0); from the Khronos glTF Sample Assets | none |
| `Assets/Fonts/KenneyFuture.ttf` | https://kenney.nl/assets/kenney-fonts | Kenney | CC0-1.0 | | `Kenney Future.ttf`, renamed |
| `Assets/Icon.png` | A render of the sample: a 256 x 256 `viewport.capture` of the island (its trees and mountain), the exported game's icon (`project.setGameInfo`) | Strata | Strata | | none |
