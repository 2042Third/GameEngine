# Third-Party Notices

Strata includes the following third-party software as pinned git submodules under `Strata/vendor/`.
Each project's full license text is in its submodule directory. Exported games ship this file
alongside the executable.

| Library | Version | License | Shipped in games | Purpose |
| --- | --- | --- | --- | --- |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | zlib/libpng | Yes | Windowing and input |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | 6b96fb0 | MIT | Yes | Rendering hardware interface |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.4.352 | Apache-2.0 OR MIT | Yes | Vulkan API headers |
| [glm](https://github.com/g-truc/glm) | 1.0.3 | MIT | Yes | Math |
| [EnTT](https://github.com/skypjack/entt) | 3.16.0 | MIT | Yes | Entity-component system |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | 5.6.0 | MIT | Yes | Physics |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | Public domain (Unlicense) or MIT-0 | Yes | Audio |
| [spdlog](https://github.com/gabime/spdlog) (bundles [fmt](https://github.com/fmtlib/fmt)) | 1.17.0 | MIT | Yes | Logging |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | Yes | JSON serialization |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking) | 1.92.9 | MIT | Yes | Immediate-mode UI |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | 18cef5e | MIT | Editor only | Transform gizmos |
| [stb](https://github.com/nothings/stb) | 2c980bb | MIT or public domain | Yes | Image decoding/encoding, font rasterization |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | Editor only | glTF import |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.3 | MIT | Editor only | Mesh optimization and LOD generation |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) | 3e895b4 | zlib | Editor only | Tangent generation |
| [glslang](https://github.com/KhronosGroup/glslang) | 16.6.0 | BSD-3-Clause and others (see `LICENSE.txt`) | No (build tool) | GLSL to SPIR-V compiler |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | 1.4.1 | zlib | Editor only | Native file dialogs |
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | No (tests) | Unit testing |
| [Tracy](https://github.com/wolfpld/tracy) | 0.14.1 | BSD-3-Clause | Optional | Profiler instrumentation |

HDR environment maps in sample projects come from [Poly Haven](https://polyhaven.com/hdris) and are
licensed CC0.
