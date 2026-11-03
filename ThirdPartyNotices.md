# Third-Party Notices

Strata includes the following third-party software as pinned git submodules under `Strata/vendor/`.
Each project's full license text is in its submodule directory. Exported games ship this file
alongside the executable. Slang, the shader compiler the build runs, is not a submodule:
`CMake/StrataSlang.cmake` fetches a pinned release archive at configure time; it is not shipped.

| Library | Version | License | Shipped in games | Purpose |
| --- | --- | --- | --- | --- |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | zlib/libpng | Yes | Windowing and input |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | 6b96fb0 | MIT | Yes | Rendering hardware interface |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.4.352 | Apache-2.0 OR MIT | Yes | Vulkan API headers |
| [glm](https://github.com/g-truc/glm) | 1.0.3 | MIT | Yes | Math |
| [EnTT](https://github.com/skypjack/entt) | 3.16.0 | MIT | Yes | Entity-component system |
| [box3d](https://github.com/erincatto/box3d) | efca0c1 | MIT | Yes | Physics |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | Public domain (Unlicense) or MIT-0 | Yes | Audio |
| [spdlog](https://github.com/gabime/spdlog) (bundles [fmt](https://github.com/fmtlib/fmt)) | 1.17.0 | MIT | Yes | Logging |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | Yes | JSON serialization |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | 1.92.9 | MIT | Yes | Immediate-mode UI |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | 18cef5e | MIT | Editor only | Transform gizmos |
| [stb](https://github.com/nothings/stb) | 2c980bb | MIT or public domain | Yes | Image decoding/encoding, mip filtering, block compression (editor), font rasterization |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | Editor only | glTF import |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.3 | MIT | Editor only | Mesh optimization and LOD generation |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) | 3e895b4 | zlib | Editor only | Tangent generation |
| [Slang](https://github.com/shader-slang/slang) | 2026.20 | Apache-2.0 WITH LLVM-exception (its components' licenses are in the archive's LICENSES/ and third-party-notices/) | No (build tool) | Shader compiler (HLSL-syntax Slang to SPIR-V) |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | 1.4.1 | zlib | Editor only | Native file dialogs |
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | No (tests) | Unit testing |
| [Tracy](https://github.com/wolfpld/tracy) | 0.14.1 | BSD-3-Clause | Optional | Profiler instrumentation |
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | BSD-3-Clause (dual-licensed with GPL-2.0; used under BSD) | Yes (decompression; compression editor only) | Asset pack compression |

The editor (not exported games) embeds these fonts, committed as release files with their license texts in
`StrataEditor/Resources/Fonts/`:

| Font | Version | License | Files | Purpose |
| --- | --- | --- | --- | --- |
| [Inter](https://github.com/rsms/inter) by The Inter Project Authors | 4.1 | SIL Open Font License 1.1 (`Inter-OFL.txt`) | `Inter-Regular.ttf`, `Inter-SemiBold.ttf` (from `Inter-4.1.zip`, `extras/ttf/`) | UI text and headers |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) by The JetBrains Mono Project Authors | 2.304 | SIL Open Font License 1.1 (`JetBrainsMono-OFL.txt`) | `JetBrainsMono-Regular.ttf` (from `JetBrainsMono-2.304.zip`, `fonts/ttf/`) | Logs, IDs and numbers |
| [Lucide](https://github.com/lucide-icons/lucide) by Lucide Icons and Contributors | 1.47.0 | ISC; icons derived from [Feather](https://github.com/feathericons/feather) also MIT (Cole Bemis), see `Lucide-LICENSE.txt` | `Lucide.ttf`, `Lucide-codepoints.json` (from `lucide-font-1.47.0.zip`) | Icons |

The engine embeds the font [Roboto Medium](https://fonts.google.com/specimen/Roboto) by Christian Robertson
(Apache License 2.0, https://www.apache.org/licenses/LICENSE-2.0) as the default font of text rendering. It is
compiled in from the copy that Dear ImGui distributes (`Strata/vendor/imgui/misc/fonts/Roboto-Medium.ttf`) and ships in
every game.

## Code derived from other projects

Code that Strata copied, ported or adapted from open-source projects that are not submodules, each under its permissive
license (Docs/DeveloperGuide.md, "Working with third-party code"). Each project gets a row of a table here (project,
version, license, Strata's files, what was derived) and its license's full text below the table.

| Project | Version | License | Strata's files | What was derived |
| --- | --- | --- | --- | --- |
| [radiance-cascades-lab](https://github.com/dequevedo/radiance-cascades-lab) | as of 2026-11-03 | MIT | `Strata/shaders/Include/Voxelize.slang`, `Strata/shaders/GI/VoxelTriangles.comp.slang`, `VoxelFill.comp.slang`, `VoxelFinalize.comp.slang` | Akenine-Moller's triangle-box test in its form, the winding steps along columns and their fill, the emission's coverage (from `src/rc/shaders/voxelize_mesh.comp`, `fill_solid.comp`) |

radiance-cascades-lab:

```
MIT License

Copyright (c) 2026 Daniel Quevedo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Sample assets

The sample projects under `Samples/` (and the games exported from them) use the asset packs below. Each sample's
`CREDITS.md` lists which of its files come from which pack, what was changed, and the files made for Strata. CC0 packs
are in the public domain; CC-BY packs ask for the attribution given here, which every game made from them must show.

| Sample | Pack | Source | Author | License | Attribution |
| --- | --- | --- | --- | --- | --- |
| Physics Playground | ambientCG Tiles101, Bricks076C, Planks037B, Concrete034, Plastic010, Metal032 | https://ambientcg.com/view?id=Tiles101, https://ambientcg.com/view?id=Bricks076C, https://ambientcg.com/view?id=Planks037B, https://ambientcg.com/view?id=Concrete034, https://ambientcg.com/view?id=Plastic010, https://ambientcg.com/view?id=Metal032 | ambientCG | CC0 1.0 | |
| Physics Playground | Kenney Impact Sounds, Interface Sounds, UI Pack, Kenney Fonts | https://kenney.nl/assets/impact-sounds, https://kenney.nl/assets/interface-sounds, https://kenney.nl/assets/ui-pack, https://kenney.nl/assets/kenney-fonts | Kenney | CC0 1.0 | |
| Open World | Kenney Nature Kit, Kenney Fonts | https://kenney.nl/assets/nature-kit, https://kenney.nl/assets/kenney-fonts | Kenney | CC0 1.0 | |
| Open World | Khronos glTF Sample Assets: Fox | https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/Fox | PixelMannen; tomkranis; @AsoboStudio and @scurest | CC0 1.0 (model); CC BY 4.0 (rigging, animation, glTF conversion) | Fox: model by PixelMannen (CC0 1.0); rigging and animation by tomkranis (CC BY 4.0); glTF conversion by @AsoboStudio and @scurest (CC BY 4.0) |
| Third-Person Adventure | three.js example models: RobotExpressive | https://github.com/mrdoob/three.js/blob/f67dcda032208b367f5965bbd835c16f839909c8/examples/models/gltf/RobotExpressive/RobotExpressive.glb | Tomás Laulhé (Quaternius); modifications by Don McCurdy | CC0 1.0 | |
| Third-Person Adventure | Kenney Platformer Kit, Impact Sounds, Interface Sounds, UI Pack, Kenney Fonts | https://kenney.nl/assets/platformer-kit, https://kenney.nl/assets/impact-sounds, https://kenney.nl/assets/interface-sounds, https://kenney.nl/assets/ui-pack, https://kenney.nl/assets/kenney-fonts | Kenney | CC0 1.0 | |
| Third-Person Adventure | Poly Haven: Kloofendal 48d Partly Cloudy (Pure Sky) | https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky | Greg Zaal; sky edits by Jarod Guest | CC0 1.0 | |
| High Desert Outpost | Poly Haven: 22 scans (rocky_terrain_02, dry_ground_01, asphalt_02, brown_mud_03, beige_wall_001, weathered_brown_planks, rusty_painted_metal, green_metal_rust, rusty_corrugated_iron, rusty_metal_03, namaqualand_boulder_02, namaqualand_boulder_03, namaqualand_cliff_01, dry_branches_medium_01, barrel_03, Barrel_01, Barrel_02, stone_fire_pit, Lantern_01, old_tyre, covered_car, ladder_sectioned_01) | https://polyhaven.com/a/rocky_terrain_02, https://polyhaven.com/a/dry_ground_01, https://polyhaven.com/a/asphalt_02, https://polyhaven.com/a/brown_mud_03, https://polyhaven.com/a/beige_wall_001, https://polyhaven.com/a/weathered_brown_planks, https://polyhaven.com/a/rusty_painted_metal, https://polyhaven.com/a/green_metal_rust, https://polyhaven.com/a/rusty_corrugated_iron, https://polyhaven.com/a/rusty_metal_03, https://polyhaven.com/a/namaqualand_boulder_02, https://polyhaven.com/a/namaqualand_boulder_03, https://polyhaven.com/a/namaqualand_cliff_01, https://polyhaven.com/a/dry_branches_medium_01, https://polyhaven.com/a/barrel_03, https://polyhaven.com/a/Barrel_01, https://polyhaven.com/a/Barrel_02, https://polyhaven.com/a/stone_fire_pit, https://polyhaven.com/a/Lantern_01, https://polyhaven.com/a/old_tyre, https://polyhaven.com/a/covered_car, https://polyhaven.com/a/ladder_sectioned_01 | Amal Kumar, Rob Tuytel, Dimitrios Savva, Rico Cilliers, Charlotte Baglioni, Greg Zaal, Jenelle van Heerden, Dario Barresi, Serhii Khromov, Jorge Camacho, Sebastian Platen, Rajil Jose Macatangay, MP | CC0 1.0 | |
