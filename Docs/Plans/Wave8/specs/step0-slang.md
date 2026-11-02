# Step 0, lane Slang (S0.10): every shader in HLSL-syntax Slang

Lane Slang runs in `G:/GameEngineAI/.worktrees/wave8-slang` on branch `agent/wave8-slang`, made with
`sh .worktrees/create_worktree.sh wave8-slang main` once S0.0d and S0.1 are on main (the names of step0.md 10.1). Its build
tree is the worktree's own `build/windows`; it pushes to `ci/slang` (plan 3: "Work on `ci/slang`, merge only through the
gates"). It is the only lane
that touches `Strata/shaders` (plan 3), and nothing in it waits for another lane until its last merge of main.

Paths: `R/` is `Strata/src/Strata/Renderer/`, `SH/` is `Strata/shaders/`, `T/` is `StrataTests/src/`, `DG` is
`Docs/DeveloperGuide.md`, `spike/` is `G:/GameEngineAI/.worktrees/wave8-plan/slang/spike/`, `plan` is
`.worktrees/wave8-plan/plan.md`, `decisions` is `.worktrees/wave8-plan/specs/decisions.md`. Line numbers are those of
76f28a21 (main, release 0.7.0) unless a line says otherwise. A shader is named as `strata_add_shaders` registers it
(`Scene/Forward.frag`); after the conversion its file is `SH/Scene/Forward.frag.slang`, an include `SH/Include/X.glsl` is
`SH/Include/X.slang`, and `SH/GameUI/UIQuad.glsl` is `SH/GameUI/UIQuad.slang`. Registered names never change, so the
about 90 shader names in C++ stay as they are.

Rules for the whole lane (AGENTS.md, plan 4.1, decisions, this machine):

- Every commit: Debug build with warnings as errors, `ctest --preset windows-debug -j 8` with every `GPU*` suite under the
  validation layer (`VK_ADD_LAYER_PATH=G:\GameEngineAI\build\_tools\Vulkan-ValidationLayers\build\layers\Release`; this
  machine has no Vulkan SDK), and `python Tools/CompatLint.py --changed-since main` clean. `ctest --preset windows-release
  -LE perf -j 8` before each push to `ci/slang`.
- Pictures and GPU times only under `python Tools/PerfLock.py --label lookdev-slang --limit 90 -- ...`, never while this
  lane builds; gate 4 asks the orchestrator for a quiet window (section 12).
- Never the owner's `build/windows`. At most four build-heavy agents on the machine (decisions, "Standing rules").
- No code from other folders on this computer. Slang is used as a pinned binary build tool (Apache-2.0 WITH
  LLVM-exception, permissive under D10); nothing of its source is copied.
- Python: `/c/msys64/mingw64/bin/python.exe -I <script file>`, never a script on stdin (it hangs).

## 1. The owner's request and the item's goal

The owner (2026-11-01, item 14, verbatim): "Change to slang for hlsl for shaders due to possible console port in the
future". The plan put it in step 0 (plan 1, priority 3) because it touches every shader, so it cannot overlap the
rendering streams, the tree is quiet now, and the wave's new shaders then get written once. Decision 4 accepted it there,
boxed at 2 agent-days with one extension, with the MoltenVK conversion and the HLSL test inside the box; decision 5
accepted D1, the pin.

The goal, in priority order:

1. **The same pictures and the same GPU time from a different compiler.** All 103 registered entries move to HLSL-syntax
   Slang in `.slang` files, compiled by a pinned `slangc` to SPIR-V 1.5, still run by Vulkan and by MoltenVK. Nothing a
   player sees changes beyond rounding (gate 3), nothing costs more (gate 4).
2. **No silent mistakes.** The spike's one silent slip (`mul` of two vectors, which collapsed the local shadows' filter and
   passed every GPU test) is the risk this item manages: three independent gates (reflection, validation, signed look-dev
   difference), a converted tree that equals the spike's validated tree, and rules that point later debugging at the
   conversion.
3. **The console route stays open and checked.** Plain HLSL syntax, every entry emitting HLSL in a CTest that runs on
   every CI job, and every entry converted to Metal on the macOS job. New shaders cannot quietly close the owner's reason
   for this item.
4. **A lighter, simpler build.** glslang's 926 MB submodule and its compile leave; one compile path; the first SPIR-V
   validation CI has ever run.

Not the goal: a console backend, a D3D or Metal target in the engine, Slang modules or generics, moving the converted
stage globals to entry parameters (below the box, section 3.3), or any change of a pass, slot, binding or quality row.

## 2. Where we start

### 2.1 Files

| File | Lines | Role in S0.10 |
| --- | --- | --- |
| `CMake/StrataShaders.cmake` | 123 | `strata_add_shaders`: glslang per entry (`:81-87`, `--vn` header), every shader depends on every `*.glsl` (`:28`, `:85`), the registry (`:99-118`). Rewritten |
| `CMake/StrataSlang.cmake` | new | The pin, the per-user cache, `STRATA_SLANG_ROOT` |
| `CMake/StrataShaderTool/` | new | The host tool: `SpirvModule.h/.cpp` (library `StrataShaderToolCore`), `StrataShaderTool.cpp` (the executable) |
| `Strata/CMakeLists.txt` | 195 | The `strata_add_shaders` call (`:73-180`): 78 SHADERS, 25 VARIANTS. Gains `INVARIANT_POSITION Scene/Mesh.vert` |
| `Strata/vendor/CMakeLists.txt` | 254 | glslang block (`:226-235`) leaves; header comment (`:1-6`) gains one line |
| `.gitmodules` | 80 | glslang (`:53-56`) leaves |
| `CMakeLists.txt` | 84 | Untouched: `include(StrataShaders)` (`:56`) brings the pin with it |
| `.github/workflows/release.yml` | n/a | Untouched: its configure fetches the pin, and its `ctest -LE "gpu\|perf"` (`:173`) runs the two unlabelled CTests below; the `-rc1` dry run proves both on all three platforms |
| `.github/workflows/ci.yml` | 244 | Linux deps (`:78-85`), macOS deps (`:87-89`), Build (`:135-140`), Annotate (`:197-202`): two new steps |
| `.github/scripts/annotate_failures.py` | n/a | `PATTERNS` (`:23-28`) gains Slang's two-line diagnostic |
| `.github/scripts/validate_shaders.sh`, `convert_shaders_to_metal.sh` | new | CI's spirv-val and Metal steps |
| `Strata/shaders/**` | 112 files, 9,980 lines | 46 `.comp`, 24 `.frag`, 8 `.vert`, 34 `.glsl`: all become `.slang` |
| `R/EmbeddedShaders.h` | 25 | `EmbeddedShader{Name, Code, Size}` (`:12-17`): unchanged |
| `R/ShaderLibrary.cpp` | 104 | Stage from the name's suffix (`:29-45`), entry `main` (`:77`): unchanged |
| `R/SceneRenderer.cpp` / `.h` | 4,125 / 2,771 | Vertex input layouts (`:311-329`), `GetScenePipeline` picks one (`:2371`); members (`.h:1967`, `:1976`). Shadow passes get their own (C2b) |
| `T/Architecture/ShaderSourceTests.cpp` | 277 | `Architecture.Shaders`: GLSL derivative builtins (`:28-31`), reads `.frag` and `.glsl` (`:254`). Ported and extended |
| `T/Renderer/GPURendererTests.cpp` | 441 | `GPU.Renderer` (`:35`): one case added |
| `T/Renderer/ShaderToolTests.cpp`, `ShaderModuleTests.cpp` | new | `Renderer.ShaderTool`, `Renderer.ShaderModules` |
| `StrataTests/CMakeLists.txt` | 985 | `StrataTests` target (`:103`); `StrataPackaging.Version` (`:332`) is the model of the two new CMake-script CTests |
| `StrataTests/Shaders/SlangFetchTests.cmake`, `TargetHLSL.cmake` | new | `StrataSlang.Fetch`, `StrataShaders.TargetHLSL` |
| `Tools/ConvertShadersToSlang.py` | new, then removed | The spike's converter (`spike/tools/glsl2slang.py`, 835 lines, plus `handfix.py`, `Prelude.slang`), for one commit |
| `Tools/FitFilmic.py` | 609 | Writes `FilmicConstants.glsl` (`:87`, `:496-498`, `:506-551`), reads it back (`:556-562`). Writes `.slang`, gains `--rewrite` |
| `Tools/FitAOMultiBounce.py` | 312 | Writes `AmbientOcclusionFit.glsl` (`:209-227`, `:275`). Writes `.slang` |
| `Tools/LookDev.py` | 1,013 | `difference` (`:254-277`), `compare_command` (`:833-894`), `selftest_command` (`:897-`). Gains signed differences and a per-mode summary |
| `ThirdPartyNotices.md` | 58 | Intro (`:3-5`), glslang row (`:24`) |
| `AGENTS.md` | 492 lines, 39,292 code points | Layout row (`:36`), Building (`:50`), Shaders bullet (`:203-207`), third-party section (`:483-488`) |
| `Docs/DeveloperGuide.md` | 5,427 | Contents (`:13-123`), Tools row (`:135`), requirements (`:155-156`), CI (`:187-199`), look-dev harness (`:628-676`), Rendering (`:3455`), 37 GLSL mentions; new section "Shaders" |
| `Docs/Architecture.md` | 4,149 | `ShaderLibrary` (`:296`) and 6 more mentions |
| `README.md` | n/a | Requirements (`:98-102`) |
| `.claude/skills/strata-build-test/SKILL.md` | 222 | Shaders line (`:20-21`) |

### 2.2 Facts the design depends on (checked at 76f28a21 unless marked)

- **Entries.** 78 SHADERS and 25 VARIANTS (`Strata/CMakeLists.txt:77-154`, `:156-180`); variants are `-D` defines, no
  specialization constants. Registered names keep their stage suffix, and `ShaderLibrary` takes the stage from it.
- **glslang's output.** `-V --target-env vulkan1.2 --quiet --vn g_StrataShader_<id>` (`CMake/StrataShaders.cmake:83-84`)
  writes SPIR-V 1.5 (header word `0x00010500`, read in `build/windows/Strata/EmbeddedShaders/ImGui.vert.h`), 8 words per
  line. No optimizer, no validation in the build. The registry includes `<cstdint>` before the headers (`:101-106`).
- **The spike's tree is the validated one.** `spike/converted/` (written 22:12) is the tree after the `mul` fix; its
  modules are what `StrataTestsSlang.exe` (22:14) and the 4K captures `spike/lookdev/slang3` (22:14-22:20) ran:
  553 of 554 `GPU*` cases under the validation layer (`spike/run/val2_Slang.log`; the one failure,
  `StandardStreamsProcessTests.cpp:297`, is the copied exe's and also fails on the base copy), 84 s in Release.
- **Warnings.** I compiled all 103 entries of `spike/converted/` with slangc 2026.20 and the flags of section 6.1 minus
  `-warnings-disable` (2026-11-01, output in my scratchpad only). Two warnings fire, nothing else: E39001
  "explicit binding overlap" in 12 entries (example: `ScreenSpaceReflectionsSpatial.comp`'s `u_PreviousColorLevels :
  register(t27)` against `Scene.slang:303`'s `u_SkyViewTable : register(t27)`), and E41012 "profile implicitly upgraded"
  in 16 entries (example: `DepthPrepass.frag`, "to include these capabilities: 'spvImageGatherExtended'"), which changes
  no declared capability (next fact). `slangc -v` prints `2026.20`.
- **Capabilities.** Over the spike's modules (my scan): glslang declares `Shader` 103, `ImageQuery` 34,
  `RuntimeDescriptorArray`, `SampledImageArrayNonUniformIndexing` and `ShaderNonUniform` 15 each,
  `StorageImageExtendedFormats` 4, `SampledCubeArray` 6 (the Forward variants); Slang the same minus `SampledCubeArray`,
  plus the extension `SPV_KHR_storage_buffer_storage_class` in 48. The device requires descriptor indexing, runtime arrays,
  non-uniform sampled-image indexing and `imageCubeArray` (`Platform/Vulkan/VulkanGraphicsDevice.cpp:669-685`, enabled at
  `:818`, `:830-834`); `shaderInt16`, `shaderFloat16`, the without-format storage images, `shaderImageGatherExtended` and
  `shaderStorageImageExtendedFormats` are enabled only where supported (`:822-827`, `:849`). (Reviewer, re-scanned: the
  same counts.) `StorageImageExtendedFormats` is declared by exactly `Effects/AODenoiseHalf.comp`, `Effects/GTAOHalf.comp`,
  `Effects/SceneColorCopy.comp` and `IBL/BRDFLut.comp`. The first three run only when the device has the feature
  (`R/SceneRendererAmbientOcclusion.cpp:56`, `R/SceneRendererGlass.cpp:43`); `BRDFLut.comp` writes its RG16F table always
  (`SH/IBL/BRDFLut.comp:12`, `R/SceneRenderer.cpp:605-616`), and device selection does not require the feature: a
  pre-existing gap the lane records, not one it adds.
- **`precise`.** glslang decorates `NoContraction` in `Debug/LODSelection.comp`, `Scene/Cull.comp` and `Scene/CullShadow.comp`
  (once each); Slang's modules keep it there (1, 2 and 3: inlined copies) (reviewer's scan of `spike/out/`).
- **Bindings.** In the converted modules no two entry-point interface variables share a (set, binding): 0 clashes over
  103 modules. glslang's modules list declared-but-unused variables too: 25 clashes (`LightingViewStats.comp` (0,0),
  `ScreenSpaceReflectionsSpatial.comp` (0,27), ...). So the overlap Slang warns about is declarations that never meet in
  one module.
- **Invariant.** Exactly `Scene/Mesh.vert`, `MeshSkinned.vert`, `MeshVelocity.vert` and `MeshSkinnedVelocity.vert` decorate
  their position `Invariant` (glslang, from `SH/Scene/Mesh.vert:33`; the spike's patched Slang modules likewise). Slang
  emits the position as its own variable (`OpDecorate %v BuiltIn Position`), which `spike/tools/invariant.py` patched.
- **Vertex input locations.** NVRHI numbers a layout's attributes 0, 1, 2... in order (`NVRHI/src/vulkan/vulkan-shader.cpp:176-186`).
  The shadow passes use the mesh layouts of four attributes (`R/SceneRenderer.cpp:311-329`, `:2371`) while
  `SH/Scene/ShadowMesh.vert` reads only position and UV (and the skin): Slang drops the unused inputs and the validation
  layer warns "attribute at location 1/2 not consumed" (the audit). The spike's log (`spike/run/val2_Slang.log`) shows the
  warning 10 times and then the layer's "reported 10 times, which is the duplicate_message_limit value": so the log cannot
  show that the shadow passes were the only pipelines with unconsumed attributes (gate 2 raises the limit).
- **The cull rows.** `Dispatch` (`R/SceneRendererGPUDriven.cpp:76-84`) dispatches `min(groups, 65535)` by
  `ceil(groups / x)` groups of `c_CullGroupSize` = 64 (`:31`), so whenever a second row exists the first is full: hand fix
  12 rests on that.
- **Per-user directories.** The engine's conventions: `%LOCALAPPDATA%` on Windows
  (`Platform/Windows/WindowsPlatform.cpp:224`), `~/Library/...` on macOS and XDG with absolute paths only on Linux
  (`Platform/Posix/PosixPlatform.cpp:351-372`, `:434-438`).
- **The release archives** (GitHub's API, saved in `.worktrees/wave8-plan/slang/latest.json`; v2026.20 published
  2026-10-17). I recomputed the Windows zip's digest locally: it matches.

| Host key | Archive | Bytes | SHA-256 |
| --- | --- | --- | --- |
| `WINDOWS_X86_64` | `slang-2026.20-windows-x86_64.zip` | 63,658,255 | `e6dd32bbddf1720ebb4e679d55e9635ce792865a5bd84f3d2cd9af28647c0174` (verified) |
| `LINUX_X86_64` | `slang-2026.20-linux-x86_64-glibc-2.27.tar.gz` | 29,547,372 | `b831477e994dee2fb36a3e83c54ad89cbfeefd47b4d973b2d8bed65b64780996` (API only) |
| `MACOS_AARCH64` | `slang-2026.20-macos-aarch64.tar.gz` | 62,779,895 | `07fa8833fdbb343c3674c84fdd885910f495f3daaaa0c732b32980151e8b58c4` (API only) |
| `MACOS_X86_64` | `slang-2026.20-macos-x86_64.tar.gz` | 26,896,481 | `b4f01f615dc625f60c593d7d0a4bbd38e05dfa2a34bfd3973017c309d866feaa` (API only) |

- **The Windows archive** has no top-level folder (`bin/`, `lib/`, `include/`, `share/`, `cmake/`, `README.md`, `LICENSE`,
  `LICENSES/`, `third-party-notices/`), 4,890 files whose longest path is 154 characters (all in `share/doc`). The run set the
  audit used is `bin/slangc.exe`, `slang.dll`, `slang-compiler.dll`, `slang-glslang.dll` and `slang-glsl-module.dll` (without
  the last, slangc writes a cache file beside itself). After my 103+ compiles nothing new appeared in `bin/`. **Reviewer,
  2026-11-02:** those five files copied alone into an empty `bin/` print `2026.20` for `-v`, compile `Scene/Forward.frag`,
  `Effects/GTAO.comp`, `Scene/ShadowMesh.vert` and `PostProcess/Tonemap.frag` of `spike/converted/` with section 6.1's flags
  into modules **byte-identical** to `spike/out/slang/` (with and without `-default-image-format-unknown`: every storage image
  already states its format), emit HLSL for `Scene/MeshSkinned.vert` and `Scene/CullBuild.comp`, and leave `bin/` unchanged.
  `bin/` also holds `slang-standard-module-2026.20/` (modules for `import slang.*`, unused by plain HLSL), `slang-rt.dll`,
  `gfx.dll` and `slang-llvm.dll`, none needed. The Linux and macOS layouts are unverified.
- **Homebrew's `molten-vk`** installs `MoltenVKShaderConverter` into `bin` (the formula's `bin.install` line, read
  2026-11-01). Its usage text: `-si <spv in>`, `-mo <msl out>`, `-mv <n[.n]>` MSL version, `-mp macos|ios`, `-mab` Metal
  argument buffers, `-q`; it writes `.metal` source only (it reports its own validation compile, but writes no library).
- **No test reads shader sources except `Architecture.Shaders`**, and no test compares shader text. The 17 files under
  `StrataTests/src` that mention `.glsl` do so in comments (reviewer's count; the author counted 22).
- **`Tools/LookDev.py compare`** requires `--output` (`:996`), names pictures `<shot>-<width>x<height>-<mode>.png` (`:476-477`),
  and returns 1 when one side has a picture the other lacks (`:891`). `selftest` requires `--build` (`:998`) and
  `StrataTools.LookDev` carries the label `gpu` (`StrataTests/CMakeLists.txt:496`), so CI never runs it.

### 2.3 What the spike did not do, so the lane must

A real build (it patched SPIR-V into copies of Release programs), Debug and Dist, Linux and macOS archives, MoltenVK,
non-NVIDIA drivers, pipeline creation time, and a merge with anything. Its look-dev shots were 4 projects at 4K only, and
point-light paths are thinly covered (only `tp.lantern` showed the slip).

## 3. Order, the box and the cut line

### 3.1 Commits, in order

Each commit builds, passes the lane's tests and is one logical change (AGENTS.md checklist 6).

| # | Commit | What it proves |
| --- | --- | --- |
| C0 | Show signed differences and per-mode statistics in look-dev compares (section 4) | The instrument of gate 3 and of plan 4.1's debugging rule |
| C1 | Pin Slang 2026.20 per platform in a per-user cache (section 5) | Every CI job fetches, verifies and runs `slangc -v` at configure, before any shader depends on it |
| C2 | Embed shaders through StrataShaderTool, two commands per entry (section 6), glslang still compiling | All 103 embedded arrays byte-identical to 76f28a21's; CI's spirv-val and Metal steps produce glslang's baseline |
| C2b | Give shadow passes their own vertex input layout (section 7.5), in GLSL | Pixel-equal under glslang: the trim is an engine change on its own |
| C3 | Convert every shader to HLSL-syntax Slang (section 7) | The mechanical conversion as one change (plan 3): converter, its output, the generators, slangc in the build, the ported and new tests |
| C4 | Remove the shader converter (section 8.1) | History keeps it for wave 9 |
| C5 | Remove the glslang submodule (section 8.2) | One compiler |
| C6 | Check that every shader emits HLSL (section 9) | The console route in CI |
| C7 | Document shaders in Slang (section 13) | Docs, AGENTS.md, comments, Roadmap open items, release-note lines |
| C8 | Below the box (3.3): the hand port of Forward, GTAO and Tonemap | Only if the box allows |

Pushes: C2 (with C0 and C1) goes to the separate ref `ci/slang-c2`, so that later pushes to `ci/slang` do not cancel it
(`ci.yml:9-11`, cancel-in-progress): its spirv-val and Metal steps are glslang's baseline. C7 goes to `ci/slang`. After the
gates, main is merged (section 16) and pushed to `ci/slang` again.

### 3.2 The box

The box is 2 agent-days from C0's first edit, the unit the audits and the plan use (plan 1: "a unit of size that gives
order, never a date"). It holds C0-C7, the six gates (section 12), the Metal conversion, the HLSL test, the CI steps
("All of this counts inside the box", plan 3; decision 4) and the Mesa (lavapipe) try of section 11.2, which the ruling D9
makes part of S0.10 ("the Mesa job is tried"): at most 2 hours of agent time, mostly CI wall time run beside the gates, its
result information, not a gate of its own (a failure only with Slang's modules is a conversion defect, fixed under gate 2,
11.2). Expected split: C0-C2b 0.6, C3 0.4, C4-C6 0.15, the gates and the lavapipe try 0.6,
C7 0.25. Only if an extension (section 17) is needed does the lavapipe try give way first, and the release notes then say it
was not tried.

### 3.3 The cut line

Below the box, done only if at least 0.25 agent-day of the box remains after every gate is green:

1. C8, the hand port (plan 3: "Below the box: moving the converter's `static` stage globals to entry parameters"): the
   spike's `spike/hand/tree/` versions of `Scene/Forward.frag` (and its five variants), `Effects/GTAO.comp` and
   `PostProcess/Tonemap.frag` (`spike/hand/port.py` lists the edits), which passed the same 553 cases
   (`spike/run/val3_Hand.log`). It needs gates 1-3 again for those 9 entries (Forward and its 5 variants, GTAO and
   GTAOHalf, Tonemap). If cut, wave 9 does all files.

Whatever the cut, **new shaders written after C3 take their inputs as entry-point parameters** (DG "Shaders"); only the
converted files keep the `st_*` stage globals.

## 4. C0: signed differences and a per-mode summary in `Tools/LookDev.py`

Today `compare` writes the absolute channel difference times 8 (`Tools/LookDev.py:254-277`) and per-picture numbers
(`:833-894`). The gate needs to see the sign (a slip darkens or brightens a region coherently) and per-mode statistics.

- `signed_difference(before, after, gain)` returns an RGB `Image` of the same size: per pixel v is the mean over the first
  three channels of (after - before); red = min(255, round(max(v, 0) * gain)), blue = min(255, round(max(-v, 0) * gain)),
  green 0. So after-brighter is red, after-darker is blue.
- `compare` gains `--signed-gain <float>` (default 32; 0 writes none). For every picture with differing pixels it writes
  `signed/<picture>.png`, adds `"signedImage"` to the picture's entry of `compare.json`, and `index.html` shows it beside
  the absolute image.
- `compare.json` gains `"summary"`: for each AA mode (the picture name's last `-<aa>.png`) `{"pictures", "differing",
  "medianDifferingPixels"` (over all pictures of the mode), `"medianLargest"` (over the differing ones, 0 when none),
  `"maxLargest", "maxMean"}`. `compare` prints one line per mode after its existing line. Exit codes unchanged.
- `selftest` checks `signed_difference` on a 2x1 pair with known values (v = +2 at gain 32 gives red 64, v = -1 gives
  blue 32) and the summary of a synthetic three-picture set. These checks need no editor, so `selftest`'s `--build` becomes
  optional: without it, `selftest` runs only the offline checks (the PNG round trip and filters that exist today, the signed
  difference, the summary) and says so. `StrataTools.LookDev` (labels `gpu;tools`) keeps running the whole selftest with
  `--build`; a new **`StrataTools.LookDevOffline`** (label `tools`, `TIMEOUT 120`, beside it in `StrataTests/CMakeLists.txt`
  `:484-497`, with the same `PythonMissing.cmake` fallback) runs `selftest --output <build>/LookDevOffline/$<CONFIG>` without
  `--build`, so every CI job checks C0's functions. It fails without C0 (`--build` required, no signed difference).
- DG "Look-dev harness" (`:661-664`): the `compare` bullet names the signed images and the summary.

The spike's numbers in this form (my recomputation of `spike/lookdev/compare_slang3/compare.json`):

| Mode | Pictures | Differing | Median differing pixels | Median largest (differing) | Max largest | Max mean |
| --- | --- | --- | --- | --- | --- | --- |
| off | 27 | 23 | 973 | 1 | 7 (`lookdev.mirror`) | 0.0002 |
| fxaa | 26 | 23 | 996 | 1 | 10 (`ow.scene`, `pp.scene`) | 0.0002 |
| taa | 31 | 27 | 330,288 | 16 | 63 (2 pixels of `pp.view.luminance`) | 0.0205 |

The control (glslang against glslang) differs in 0 of 84 pictures, TAA included: the capture is deterministic here.

C0 is independent of everything else in the lane. The orchestrator may land it on main ahead of the lane (as S0.7 may land
ahead of lane Editor); in the fallback (section 17) it lands anyway.

## 5. C1: the pin, `CMake/StrataSlang.cmake`

### 5.1 Contents

Functions only, so that the CTest can include it; nothing runs at include time. Constants:

```cmake
set(STRATA_SLANG_VERSION "2026.20")
set(STRATA_SLANG_URL "https://github.com/shader-slang/slang/releases/download/v2026.20")
# Release archives of shader-slang/slang v2026.20 (2026-10-17) and their SHA-256 digests. Bump all four together, in a
# commit of its own (Docs/DeveloperGuide.md, "Shaders").
set(STRATA_SLANG_ARCHIVE_WINDOWS_X86_64 "slang-2026.20-windows-x86_64.zip")
set(STRATA_SLANG_SHA256_WINDOWS_X86_64 "e6dd32bbddf1720ebb4e679d55e9635ce792865a5bd84f3d2cd9af28647c0174")
...the three others of section 2.2...
# What is extracted (the run set and the licenses), relative to the archive's root, and that root's folder in the archive.
set(STRATA_SLANG_FILES_WINDOWS_X86_64 bin/slangc.exe bin/slang.dll bin/slang-compiler.dll bin/slang-glslang.dll
	bin/slang-glsl-module.dll LICENSE LICENSES third-party-notices)
set(STRATA_SLANG_FILES_LINUX_X86_64 bin lib LICENSE LICENSES third-party-notices)   # confirmed in the first hour
set(STRATA_SLANG_ROOT_IN_ARCHIVE_LINUX_X86_64 "")                                   # likewise
```

`STRATA_SLANG_FILES_*` and `STRATA_SLANG_ROOT_IN_ARCHIVE_*` for Linux and macOS are set from the first hour's listing of
those archives (section 19); the values above are the default if their layout matches Windows'. Extracting only these
keeps paths short (the docs' 154-character paths would sit under a 75-character cache prefix) and skips `slang-llvm`
(84 MB on Windows). On Linux and macOS the first hour lists `lib/` and extracts only the libraries `bin/slangc` loads
(`ldd`/`otool -L` are not run on the downloads: the names follow Windows' run set, `libslang*.so`/`.dylib` without
`slang-llvm` and `slang-rt`), so the cache holds no 80 MB library nothing loads.

The cache folder of a pin (`strata_slang_fetch`'s `NAME`) is `slang-${STRATA_SLANG_VERSION}-<host key in lower case>`,
for example `slang-2026.20-windows_x86_64`: a version bump makes a new folder beside the old one, so two worktrees on
different pins never replace each other's compiler while one builds. Old folders are left for the user to delete (DG
"Shaders" says where they are).

Host detection works in script mode too, which the CTest needs: the system from `CMAKE_HOST_WIN32`, `CMAKE_HOST_APPLE` and
`CMAKE_HOST_LINUX` (CMake 3.25, the repository's minimum), the processor from
`cmake_host_system_information(RESULT <var> QUERY OS_PLATFORM)`. Whether `CMAKE_HOST_SYSTEM_NAME` and
`CMAKE_HOST_SYSTEM_PROCESSOR` are set under `cmake -P` is unverified, so the module does not read them.

| Function | Contract |
| --- | --- |
| `strata_slang_host_key(<out>)` | From the host variables above: Windows with `AMD64` or `x86_64` is `WINDOWS_X86_64`, Linux `x86_64` `LINUX_X86_64`, macOS `arm64` `MACOS_AARCH64`, macOS `x86_64` `MACOS_X86_64`; anything else empty |
| `strata_slang_cache_directory(<out> [SYSTEM <Windows\|Darwin\|Linux>])` | Windows `$ENV{LOCALAPPDATA}/Strata/Toolchains`; macOS `$ENV{HOME}/Library/Caches/Strata/Toolchains`; Linux `$ENV{XDG_CACHE_HOME}/strata/toolchains` when that is absolute, else `$ENV{HOME}/.cache/strata/toolchains`; empty when the variable it needs is unset. `SYSTEM` (tests only) picks the branch instead of the host, so the CTest checks all three on every CI OS |
| `strata_slang_fetch(<outRoot> <outError> URL <url> SHA256 <hex> CACHE_DIR <dir> NAME <folder> FILES <path>... [ROOT_IN_ARCHIVE <folder>])` | `<dir>/<folder>` holding a verified extraction of `<url>`, fetched when absent or stamped with another digest. Never fails the configure itself: on failure `<outRoot>` is empty and `<outError>` the reason |
| `strata_slang_check(<outError> <root>)` | `<root>/bin/slangc` (`.exe` on Windows hosts) exists, runs, and `slangc -v` (stdout and stderr, stripped) matches `^v?2026\.20$` |
| `strata_find_slang(<outSlangc>)` | `STRATA_SLANG_ROOT` when set, else the pin for this host in the per-user cache; `FATAL_ERROR` with what to do when neither works (5.3). Called once per configure by `strata_add_shaders` (6.1), which stores the path in the global property `STRATA_SLANGC` |

`STRATA_SLANG_ROOT` is a cache variable (`PATH`, "An extracted Slang 2026.20 release to use instead of the pinned download")
whose default is `$ENV{STRATA_SLANG_ROOT}`, so CI, agents and offline machines can set either.

### 5.2 `strata_slang_fetch`, step by step

1. `final = <dir>/<folder>`. If `final/.strata-slang-sha256` exists and holds `<hex>`: return `final` (no lock, no network).
2. Create `<dir>` if missing; on non-Windows hosts, when this call created it, `file(CHMOD <dir> DIRECTORY_PERMISSIONS
   OWNER_READ OWNER_WRITE OWNER_EXECUTE)`: the cache holds programs the build runs, so only its user may change it.
3. `file(LOCK "<dir>/<folder>.lock" GUARD FUNCTION TIMEOUT 900 RESULT_VARIABLE result)`. Non-zero: error "Another configure
   has held <lock> for 15 minutes. If none runs, delete the file and configure again."
4. Check step 1 again (another configure may have finished meanwhile).
5. Download to `<dir>/<folder>.part`, at most three attempts: `file(DOWNLOAD <url> <part> STATUS status
   INACTIVITY_TIMEOUT 60 TIMEOUT 1200 TLS_VERIFY ON)`, a `message(STATUS)` before the first ("Downloading Slang 2026.20
   (<MB> MB) into <dir>"). All failed: remove the part file, error (5.3, "offline").
6. `file(SHA256 <part> actual)`, compared lowercase with `<hex>`. Different: remove the part file, error "The download of
   <url> has SHA-256 <actual>, not the pinned <hex>: it was not used. ..." (5.3).
7. Remove a stale `final` and `<dir>/<folder>.staging`; `file(ARCHIVE_EXTRACT INPUT <part> DESTINATION
   <dir>/<folder>.staging PATTERNS <ROOT_IN_ARCHIVE/>FILES...)`.
8. The root is `staging/<ROOT_IN_ARCHIVE>`; it must hold `bin/slangc[.exe]`, else remove staging and the part file and
   error "<archive> holds no bin/slangc".
9. Write `<root>/.strata-slang-sha256` (the digest), `file(RENAME <root> <final>)`, remove staging (if `ROOT_IN_ARCHIVE`
   was set) and the part file. The rename makes a complete folder appear at once, so no build ever sees half an extraction.

### 5.3 `strata_find_slang` and its messages

Order: `STRATA_SLANG_ROOT` set -> `strata_slang_check` on it -> its `bin/slangc`. Else host key -> cache directory ->
`strata_slang_fetch` -> `strata_slang_check` -> `bin/slangc`. The configure prints one line:
`-- Slang 2026.20: <path> (STRATA_SLANG_ROOT | cached | downloaded)`.

Every failure is a `FATAL_ERROR` that names the archive, its digest and the way out. The texts:

- **Offline or unreachable:** "Strata compiles its shaders with Slang 2026.20 and could not download it from <url>:
  <status message>. Connect to the network and configure again, or download <archive> on another machine, check that its
  SHA-256 is <hex>, extract it and configure with -DSTRATA_SLANG_ROOT=<the extracted folder> (or set the environment
  variable STRATA_SLANG_ROOT)."
- **Digest mismatch:** step 6's text, then "Configure again to download it again; if it keeps differing, the release was
  replaced: report it, do not change the digest to match."
- **No pin for this host:** "No Slang 2026.20 archive is pinned for <system> <processor>: download the release for this
  machine from <url without file>, extract it and configure with -DSTRATA_SLANG_ROOT=<folder>."
- **No cache directory:** "<variable> is not set, so Strata has no per-user cache for Slang: set it, or configure with
  -DSTRATA_SLANG_ROOT=<folder>."
- **`STRATA_SLANG_ROOT` wrong:** "STRATA_SLANG_ROOT (<path>) holds no bin/slangc" / "...'s slangc did not run: <output>" /
  "...is Slang <version>; Strata's shaders are pinned to 2026.20 (CMake/StrataSlang.cmake)".
- **Fetched slangc does not run** (glibc, signing): "The pinned Slang (<path>) did not run on this machine: <output>.
  Configure with -DSTRATA_SLANG_ROOT=<a Slang 2026.20 build that runs here>."

CMake's download honours `https_proxy` (curl). The archive is fetched once per user and machine, not per build tree.

### 5.4 `ThirdPartyNotices.md`

S0.1 rewrites the intro for D10 first. C1 adds a sentence to it, "Slang, the shader compiler the build runs, is not a
submodule: `CMake/StrataSlang.cmake` fetches a pinned release archive at configure time; it is not shipped.", and a row
after the glslang row (C5 deletes that row):

`| [Slang](https://github.com/shader-slang/slang) | 2026.20 | Apache-2.0 WITH LLVM-exception (its components' licenses are in the archive's LICENSES/ and third-party-notices/) | No (build tool) | Shader compiler (HLSL-syntax Slang to SPIR-V) |`

### 5.5 C1's wiring and test

- `CMake/StrataShaders.cmake` starts with `include(StrataSlang)`. In C1, `strata_add_shaders` calls `strata_find_slang`
  and stores the result, nothing uses it yet: so every CI job proves the fetch, the digest and `slangc -v` on its runner
  before any shader depends on them.
- **`StrataSlang.Fetch`** (CTest, no label: it runs on every CI job): `${CMAKE_COMMAND} -DWORK=<build>/SlangFetchTest -P
  StrataTests/Shaders/SlangFetchTests.cmake`, written like `PackageVersionTests.cmake` (`SEND_ERROR` per failed
  expectation). It makes `source/bin/slangc` (`.exe` on Windows hosts) and `source/docs/readme.txt`, packs them with
  `cmake -E tar czf fake.tar.gz bin docs` and uses `file://` URLs:
  1. A fresh fetch with `FILES bin` returns `<cache>/fake`, which holds `bin/slangc`, not `docs`, and a stamp with the
     digest; no `.part` or `.staging` is left. (Also proves that a folder pattern extracts its contents on each CI OS.)
  2. With `fake.tar.gz` deleted, the same call returns the same folder: a cache hit needs no network.
  3. A stamp holding another digest makes the next call extract again and rewrite the stamp.
  4. A wrong digest (name `fake-bad`) returns an error containing "SHA-256", both digests, and leaves no folder or part file.
  5. A `file://` URL to a missing file returns an error containing the URL, "STRATA_SLANG_ROOT", the archive's name and digest.
  6. An archive of `docs` alone returns "holds no bin/slangc".
  7. `strata_slang_check` on a folder without `bin/slangc` names the folder.
  8. `strata_slang_cache_directory` with `HOME`, `XDG_CACHE_HOME` (absolute, then relative) and `LOCALAPPDATA` set to test
     values returns the paths of 5.1 for `SYSTEM Windows`, `Darwin` and `Linux` (the relative XDG value is ignored), and
     empty with the needed variable unset.
  9. `strata_slang_host_key` returns the key of the CI runner it runs on (`WINDOWS_X86_64`, `LINUX_X86_64` or
     `MACOS_AARCH64` on today's six jobs), which proves the script-mode detection.

  The `file://` URLs are formed from absolute paths with forward slashes, `file:///G:/...` on Windows (three slashes before
  the drive). The test's own folders live under `-DWORK`, never in the real cache.

## 6. C2: two commands per entry and the shader tool

### 6.1 `strata_add_shaders`

Signature (the comment at the head of `CMake/StrataShaders.cmake` is rewritten to match):

```
strata_add_shaders(<target>
    SOURCE_DIR <dir>
    SHADERS <registered name>...
    [VARIANTS "<registered name of the source>|<registered name>|<DEFINE>[,<DEFINE>...]" ...]
    [INVARIANT_POSITION <registered name of a source>...]
    [INCLUDE_DIRS <dir>...])
```

A registered name ends in its stage: `.vert` vertex, `.frag` fragment, `.comp` compute, `.geom` geometry (any other
suffix: `FATAL_ERROR "... has no stage suffix"`). In C2 the source file is `<SOURCE_DIR>/<name>` (GLSL); from C3 on it is
`<SOURCE_DIR>/<name>.slang`. Every `INVARIANT_POSITION` item must be a registered source of a vertex stage, else
`FATAL_ERROR`; every entry compiled from it (variants included) gets `--invariant-position`. `Strata/CMakeLists.txt` adds
`INVARIANT_POSITION Scene/Mesh.vert` after the VARIANTS, with the comment "The depth prepass's EQUAL test needs the
forward pass's positions bit-identical (Scene/Mesh.vert)".

Includes: from C3, `file(GLOB_RECURSE includeFiles CONFIGURE_DEPENDS "${ARG_SOURCE_DIR}/*.slang")` then
`list(FILTER includeFiles EXCLUDE REGEX "\\.(vert|frag|comp|geom)\\.slang$")`: every entry depends on every include and
its own file, as today (`:15-16`), so editing `Forward.frag.slang` recompiles its 6 entries, not 103.

Per entry, with `id` = `string(MAKE_C_IDENTIFIER <name>)` and `outputDir` = `${CMAKE_CURRENT_BINARY_DIR}/EmbeddedShaders`:

```cmake
add_custom_command(
	OUTPUT "${outputDir}/${name}.raw.spv"
	COMMAND "${CMAKE_COMMAND}" -E env SLANG_RUN_SPIRV_VALIDATION=1
		"${slangc}" "${source}" -entry main -stage ${stage} -target spirv -profile spirv_1_5 ${slangFlags}
		${includeArgs} ${defineArgs} -o "${outputDir}/${name}.raw.spv"
	DEPENDS "${source}" ${includeFiles} "${slangc}"
	COMMENT "Compiling shader ${name}"
	VERBATIM)
add_custom_command(
	OUTPUT "${outputDir}/${name}.spv" "${outputDir}/${name}.h"
	COMMAND StrataShaderTool --input "${outputDir}/${name}.raw.spv" --spirv "${outputDir}/${name}.spv"
		--header "${outputDir}/${name}.h" --variable g_StrataShader_${id} --name "${name}" ${invariantArgument}
	DEPENDS "${outputDir}/${name}.raw.spv" StrataShaderTool
	COMMENT "Embedding shader ${name}"
	VERBATIM)
```

In C2 the first command is today's glslang (`glslang-standalone -V --target-env vulkan1.2 --quiet ${includeArgs}
${defineArgs} -o <raw> <source>`, depending on `glslang-standalone`); C3 replaces it with the slangc form above. Using a
target in `COMMAND` and outputs without `$<CONFIG>` is today's pattern (`:83-85`), which works with the Visual Studio and
Ninja Multi-Config generators: shader bytes are the same in Debug, Release and Dist.

`slangFlags`, every flag and why (the why-comments go into the CMake file):

| Flag | Why |
| --- | --- |
| `-entry main`, `-stage <stage>` | `ShaderLibrary` creates every module with entry `main` (`R/ShaderLibrary.cpp:77`) and the stage of the name's suffix (`:29-45`) |
| `-target spirv -profile spirv_1_5` | SPIR-V 1.5, what glslang's `vulkan1.2` produced and Vulkan 1.2's highest (`VulkanGraphicsDevice.cpp:34`) |
| `-matrix-layout-column-major` | glm's column-major bytes in every buffer; the reflection gate shows the same offsets and matrix strides |
| `-fvk-t-shift 0 all -fvk-s-shift 128 all -fvk-b-shift 256 all -fvk-u-shift 384 all` | NVRHI's `VulkanBindingOffsets` (`nvrhi.h:2068-2073`): `register(tN)` becomes binding N, `sN` 128+N, `bN` 256+N, `uN` 384+N; `spaceS` is set S |
| `-default-image-format-unknown` | A storage image without `[vk::image_format(...)]` becomes Unknown instead of a format Slang guesses from its element type (the spike's uint images came out Rgba32ui, a silent mismatch with the view). Unknown needs a without-format capability that `Renderer.ShaderModules` rejects, so every storage image states its format |
| `-warnings-disable 39001,41012` | 39001 (explicit binding overlap): passes that include `Include/Scene.slang` for its helpers declare their own resources at slots the scene layout gives to resources they never read, in separate C++ binding layouts; `Renderer.ShaderModules` checks that no module binds two used variables to one slot. 41012 (profile implicitly upgraded): Slang widens `spirv_1_5` by the capability atoms of the intrinsics an entry calls, which are not what the module declares; `Renderer.ShaderModules` checks the declared capabilities against what the device enables |
| `-warnings-as-errors all` when `STRATA_WARNINGS_AS_ERRORS` | Shaders follow the C++ warning policy; the converted tree has no other warning (2.2) |
| `-I <SOURCE_DIR>`, `-I` per `INCLUDE_DIRS`, `-D<define>` | As today (`:31-34`, `:54-63`) |
| environment `SLANG_RUN_SPIRV_VALIDATION=1` | slangc validates its output with SPIRV-Tools and fails the compile on an error (the spike's `env_validate.py`); before the patch only, hence CI's step (6.5) |

Not passed, deliberately: `-fvk-invert-y` and `-fvk-use-dx-position-w` (NVRHI already flips the viewport; the engine works
in D3D conventions and no shader reads `gl_FragCoord.z/w`), `-fvk-use-dx-layout` and `-fvk-use-scalar-layout` (they would
change the std140/std430 offsets the C++ structs follow), `-g` (no debug information: reproducible output without source
paths, the same in every configuration), `-O<n>` (Slang's default, what the spike measured), `-source-embed-style` (it
would write the header before the patch), `-depfile` (every entry depends on every include, as today),
`-emit-spirv-via-glsl` (direct emission is what the spike validated).

The function also writes `${outputDir}/ShaderEntries.txt` with `file(CONFIGURE)` (rewritten only when it changes), one line
per entry: `<registered name>|<source file relative to the repository>|<stage>|<defines, comma-separated>`, and sets the
global properties `STRATA_SHADER_ENTRIES` (that file) and `STRATA_SLANGC`. CI's steps and the HLSL test read the manifest,
so none of them hard-codes 103. The registry source (`:99-118`) is unchanged.

### 6.2 `StrataShaderTool`

`strata_add_shaders` defines, once (`if(NOT TARGET StrataShaderTool)`), from
`${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StrataShaderTool/`:

- `add_library(StrataShaderToolCore STATIC SpirvModule.cpp)`, include directory that folder, `strata_configure_target`.
- `add_executable(StrataShaderTool StrataShaderTool.cpp)` linking it, `strata_configure_target`,
  `RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"` (multi-config generators add the configuration), so it is not
  among the programs in `bin/<Config>`. No install rule. It links nothing of the engine and none of
  `StrataEngineCodegen`'s instruction sets, so it runs on the host as glslang did. Standard library only, no exceptions
  thrown by its code, C++20, Hazel style. It is not engine code: no `stpch.h`, no layer of `Layers.json` (the layering rule
  covers `Strata/src` only), and it stays under `CMake/`, whose AGENTS.md row already names "shader compilation".

`SpirvModule.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata::ShaderTool
{

	struct SpirvInstruction
	{
		uint16_t Opcode = 0;
		size_t Offset = 0;                   // Word index of the instruction in the module
		std::span<const uint32_t> Operands;  // The words after the first
	};

	// The module's instructions after its five-word header, or nullopt with `error` naming the first problem: no SPIR-V
	// magic number, fewer than five header words, an instruction whose word count is 0 or runs past the module's end.
	std::optional<std::vector<SpirvInstruction>> ParseSpirv(std::span<const uint32_t> words, std::string& error);

	// The file's words, or nullopt with `error` (unreadable, empty, size not a multiple of four).
	std::optional<std::vector<uint32_t>> ReadSpirvFile(const std::filesystem::path& path, std::string& error);

	enum class InvariantResult { Added, AlreadyInvariant, NotVertexShader, NoPositionOutput };

	// Decorates the vertex shader's BuiltIn Position output Invariant: after each `OpDecorate %v BuiltIn Position` an
	// `OpDecorate %v Invariant`, after each `OpMemberDecorate %s m BuiltIn Position` an `OpMemberDecorate %s m Invariant`,
	// unless that decoration exists. Slang has no way to say `invariant`. `words` must parse.
	InvariantResult DecoratePositionInvariant(std::vector<uint32_t>& words);

	// The header the registry includes: `const uint32_t <variableName>[] = { ... };`, eight words per line.
	std::string FormatShaderHeader(std::span<const uint32_t> words, std::string_view variableName, std::string_view shaderName);

}
```

Opcodes and operands used: `OpEntryPoint` 15 (execution model operand 0: Vertex 0), `OpDecorate` 71, `OpMemberDecorate`
72, decoration `BuiltIn` 11 with built-in `Position` 0, decoration `Invariant` 18. The inserted instruction is
`{(3 << 16) | 71, id, 18}` or `{(4 << 16) | 72, struct, member, 18}`; the module's id bound is unchanged. A module with an
entry point that is not a vertex shader returns `NotVertexShader`.

`StrataShaderTool.cpp`: `StrataShaderTool --input <raw.spv> --spirv <out.spv> --header <out.h> --variable <identifier>
--name <registered name> [--invariant-position]`. It reads and parses the input; with `--invariant-position`, `Added` and
`AlreadyInvariant` succeed and the two others fail; it writes the `.spv` (the final module) and the header (always, as
glslang did). Exit 0 on success, 1 on a failure with `StrataShaderTool: <name>: <reason>` on stderr, 2 on bad arguments
with the usage.

Header text:

```
// Generated by StrataShaderTool (CMake/StrataShaders.cmake) from the shader <name> - do not edit.
#pragma once

#include <cstdint>

const uint32_t g_StrataShader_<id>[] = {
	0x07230203,0x00010500,0x0008000b,...   (eight words per line, tab-indented, comma after every word but the last)
};
```

### 6.3 Proof that C2 changes no byte

With glslang still compiling, every final `.spv` of C2's build equals 76f28a21's module: compare each
`EmbeddedShaders/<name>.spv` with `spike/out/glslang/<name with every non-alphanumeric character replaced by _>.spv` (the
audit confirmed those byte-identical to the embedded arrays of 76f28a21's Release build). 103 of 103 identical, the four
Mesh entries reporting `AlreadyInvariant` (glslang decorates them through `gl_PerVertex`).

### 6.4 C2's tests

- **`Renderer.ShaderTool`** (`T/Renderer/ShaderToolTests.cpp`; links `StrataShaderToolCore`; the Renderer shard). The test
  assembles small modules word by word (capability, memory model, entry point, decorations, types, output variable):
  1. "A module's framing is checked": a valid module parses; a wrong magic number, a header of four words, a word count of
     0 and a word count past the end each fail with an error naming the word.
  2. "Every truncation of a module is refused or parsed without reading past it": each length 0..N-1 of a 40-word module.
  3. "A vertex shader's position is decorated invariant once": `Added`, the decoration right after the BuiltIn one, bound
     unchanged; a second call returns `AlreadyInvariant` and leaves the words unchanged.
  4. "A position in a block is decorated through its member": the `gl_PerVertex` form gets `OpMemberDecorate ... Invariant`.
  5. "Only a vertex shader with a position output is decorated": a fragment module returns `NotVertexShader`, a vertex module
     without the decoration `NoPositionOutput`.
  6. "The header holds the words, eight per line": exact text for a 10-word module.
- **`Renderer.ShaderModules`** (`T/Renderer/ShaderModuleTests.cpp`), over `EmbeddedShaders::GetAll()`, parsing with
  `ParseSpirv`:
  1. "Every embedded shader is SPIR-V 1.5 with entry point main of its name's stage": version word `0x00010500`, one
     `OpEntryPoint` named `main` with model Vertex 0, Fragment 4 or GLCompute 5 matching the suffix.
  2. "Every embedded shader declares only capabilities and extensions every accepted device enables": the allow-list, each
     entry with its reason in a comment: `Shader`, `Matrix` (implied), `ImageQuery` and `DerivativeControl` (core Vulkan,
     no feature), `RuntimeDescriptorArray`, `SampledImageArrayNonUniformIndexing` and `ShaderNonUniform` (descriptor
     indexing, required at `VulkanGraphicsDevice.cpp:669-671`), `SampledCubeArray` and `ImageCubeArray` (`imageCubeArray`,
     required at `:683`); extensions `SPV_KHR_storage_buffer_storage_class` (core since SPIR-V 1.3) only.
     `StorageImageExtendedFormats` is allowed **only in a named list of entries**, each with its reason: `Effects/GTAOHalf.comp`
     and `Effects/AODenoiseHalf.comp` (run only when `SupportsExtendedStorageFormats`, `R/SceneRendererAmbientOcclusion.cpp:56`),
     `Effects/SceneColorCopy.comp` (likewise, `R/SceneRendererGlass.cpp:43`; `SceneColorCopyFallback.comp` is the other path)
     and `IBL/BRDFLut.comp` (RG16F always, `R/SceneRenderer.cpp:605-616`: a requirement device selection does not check,
     recorded in the open items, 13.5). A new entry that declares it fails with the hint "an extended storage format needs a
     fallback for devices without shaderStorageImageExtendedFormats (SupportsExtendedStorageFormats), then a line here".
     Anything else fails naming the entry and the capability, with a hint for the known traps: `Float16`/`Int16`
     ("f32tof16 lowers to 16-bit types: use PackHalf2x16 of Include/Common.slang"), `ComputeDerivativeGroupQuadsKHR` or
     `...LinearKHR` ("Sample in a compute shader: use SampleLevel"), `StorageImageReadWithoutFormat`/`WriteWithoutFormat`
     ("give the storage image [vk::image_format]"), `DrawParameters` ("SV_InstanceID excludes the base instance: use
     SV_VulkanInstanceID"; unverified that Slang lowers it that way, the hint costs nothing).
  3. "The depth prepass's vertex shaders keep their position invariant": exactly the entries named `Scene/Mesh*.vert`, and
     there are 4, carry `Invariant` on their Position, in either form: `OpDecorate %v Invariant` on the variable decorated
     `BuiltIn Position` (Slang's modules after the tool) or `OpMemberDecorate %s m Invariant` on the member decorated
     `BuiltIn Position` (glslang's `gl_PerVertex`, which C2's run sees). No other entry carries it.
  C3 adds a fourth case (7.6).
- **`GPU.Renderer`**, one case (`T/Renderer/GPURendererTests.cpp`): "Every embedded shader becomes a module the validation
  layer accepts": `Renderer::GetShaderLibrary().Get(name)` for every entry is non-null, then `CHECK(gpu.GetNewErrorCount()
  == 0)`. The layer runs SPIRV-Tools on every `vkCreateShaderModule` (NVRHI creates the module at once,
  `vulkan-shader.cpp:40`) and checks capabilities against the enabled features, so all 103 final modules are validated on
  this machine, the patched ones included; until now only the modules a test happened to use were. It `MESSAGE`s the total
  creation time (information).

These pass on glslang's modules in C2, which proves the tests themselves and the tool's plumbing before the conversion.

### 6.5 CI's two steps, in C2

`ci.yml`:

- "Install Linux dependencies" (`:82-83`): `spirv-tools` joins the `apt-get install` line.
- "Install macOS dependencies" (`:89`): `brew install ninja ccache molten-vk`.
- After "Build" (`:135-140`):

```yaml
      # Slang validates every module it writes, but StrataShaderTool then patches some (the depth prepass's vertex shaders'
      # Invariant decoration) and Slang's archive has no spirv-val: the modules the engine embeds are validated here, after
      # the patch, in the Vulkan 1.2 environment (Docs/DeveloperGuide.md, "Shaders").
      - name: Validate the embedded shaders
        if: matrix.key == 'linux-release'
        shell: bash
        run: |
          set -o pipefail
          bash .github/scripts/validate_shaders.sh build/${{ matrix.preset }}/Strata/EmbeddedShaders 2>&1 | tee shaders.log

      # No Mac with a GPU runs CI: every embedded module goes through MoltenVK's SPIR-V to Metal conversion and Apple's Metal
      # compiler, a lower bound of what MoltenVK does at run time (Docs/DeveloperGuide.md, "Shaders").
      - name: Convert the embedded shaders to Metal
        if: runner.os == 'macOS'
        shell: bash
        run: |
          set -o pipefail
          bash .github/scripts/convert_shaders_to_metal.sh build/${{ matrix.preset }}/Strata/EmbeddedShaders "${{ runner.temp }}/metal" 2>&1 | tee metal.log
```

- "Annotate failures" (`:202`): `shaders.log metal.log` join the arguments.

`validate_shaders.sh <dir>`: prints `spirv-val --version`; for each manifest line runs `spirv-val --target-env vulkan1.2
<dir>/<name>.spv`; a failure prints `<source file>:1: error: <name>: spirv-val: <first three lines joined>` (a form
`annotate_failures.py`'s first pattern turns into an annotation on the shader's file); ends with `<valid> of <total>
embedded shaders are valid SPIR-V for Vulkan 1.2` and fails unless total > 0 and every entry is valid.

`convert_shaders_to_metal.sh <dir> <out>`: uses `MoltenVKShaderConverter` from `PATH` (Homebrew's `molten-vk`, 2.2);
only if it is missing, `brew install spirv-cross`. Per entry, `<id>` its C identifier:

- `MoltenVKShaderConverter -si <dir>/<name>.spv -mo <out>/<id>.metal -mv 3.0 -mp macos -mab -q`, success meaning exit 0
  and a non-empty `.metal` file; else `spirv-cross --msl --msl-version 30000 --msl-argument-buffers
  --msl-argument-buffer-tier 1 --entry main --stage <vert|frag|comp> --output <out>/<id>.metal <dir>/<name>.spv`.
  Argument buffers because the bindless table (set 1, `u_Textures[]`) is a runtime array MoltenVK serves through them.
- `xcrun -sdk macosx metal -std=metal3.0 -c <out>/<id>.metal -o <out>/<id>.air`. Before the first entry the script runs
  `xcrun -sdk macosx metal --version`; if that fails (images with Xcode 26 install the Metal toolchain as a separate
  component), it runs `xcodebuild -downloadComponent MetalToolchain` once and checks again, else fails naming the problem.
  macos-14's default Xcode carries the compiler (unverified on the runner; the C2 push shows it).
- A failure prints `<source file>:1: error: <name>: <converter|metal>: <first three lines>`. The script ends with
  `<n> of <total> embedded shaders convert to Metal (<converter>) and compile (Metal 3.0)` and fails on any failure not in
  its `c_KnownFailures` list (registered names, each with the converter's first error line as its reason in a comment), and
  on any listed entry that now converts (so the list only shrinks).
- **The list is filled from glslang's modules, in C2.** The offline conversion may refuse some modules whatever compiled
  them (the bindless runtime array is configured by MoltenVK at run time, which the offline converter cannot know). C2's
  first push to `ci/slang-c2` therefore runs the step with `--report-only` (prints, never fails); the entries it refuses go
  into `c_KnownFailures` in C2 itself, with their reasons, and C2 is pushed again green. From then on the step fails on
  anything new. Gate 6 is Slang's run with the same list (target: empty).

`annotate_failures.py` gains Slang's diagnostic for build logs: a line `error[E<n>]: <message>` followed by
`--> <file>:<line>:<column>` becomes one located annotation.

The push of C2 to `ci/slang-c2` records glslang's results of both steps (expected 103 of 103 valid; the Metal count is the
baseline gate 6 compares with).

## 7. C2b and C3: the conversion

### 7.1 C2b: shadow passes get their own vertex input layout

In GLSL, under glslang, before the conversion, so the change is proved alone:

- `SH/Scene/ShadowMesh.vert:14-21`: `a_Normal` and `a_Tangent` go; `a_TexCoord` becomes location 1, `a_Joints` 2,
  `a_Weights` 3. The comment says why ("shadow passes read position, UV and the skin only: their own input layout,
  SceneRenderer").
- `R/SceneRenderer.cpp` after `:329`: `m_ShadowInputLayout` = {`attributes[0]` (POSITION), `attributes[3]` (TEXCOORD)}
  created with `m_ShadowVertexShader`; `m_SkinnedShadowInputLayout` = {POSITION, TEXCOORD, JOINTS, WEIGHTS} with
  `m_SkinnedShadowVertexShader`. `GetScenePipeline` (`:2371`): `desc.inputLayout = shadowPass ? (key.Skinned ?
  m_SkinnedShadowInputLayout : m_ShadowInputLayout) : (key.Skinned ? m_SkinnedInputLayout : m_InputLayout);`. Both join
  the `ST_CORE_VERIFY` at `:545`. Members in `R/SceneRenderer.h` beside `:1967` and `:1976`, commented.
- Draws keep binding buffers 0, 1 and 2; a bound buffer a layout does not read is allowed.
- Proof: `GPU.SceneRenderer.Shadow*`, `.LocalShadow*`, `.GPUDriven` (both paths), `.Feedback` and the cached-against-
  uncached cases pass; a look-dev capture of C2b's Release build (section 12's 84-set) differs in 0 of 84 pictures from
  `G:/GameEngineAI/.worktrees/wave8-plan/lookdev/wave8-base-84` (step0.md 4.4 step 5: the same 84-set from 76f28a21's kept
  build) when the lane's base has changed no image since 76f28a21 (true if only S0.0 and S0.1 are under it), else from a
  capture of C2. Not against `lookdev/0.7.0`: that set has 1080p, Basic3D and live shots the 84-set lacks, so `compare`
  would list them as one-sided and exit 1. No shadow cache version changes: the layouts change how vertices are fetched,
  not which casters are drawn or where.

### 7.2 `Tools/ConvertShadersToSlang.py`

The spike's `glsl2slang.py`, `handfix.py` and `Prelude.slang`, made a committed tool for one commit (standard library only,
`python -I Tools/ConvertShadersToSlang.py [--listing <file outside the tree>]`). Changes from the spike:

1. Paths from the script's location; it converts `Strata/shaders` in place and deletes each GLSL file it converted.
2. Every stage file starts with `#include "Include/Common.slang"` (the spike included `Include/Prelude.slang`); no blank
   lines are left where `#version` and `#extension` were.
3. `Include/Common.slang` is written as section 7.3 says: the binding comment replaces the four `ST_SRV/ST_SAMPLER/
   ST_CBV/ST_UAV` macros (which no converted declaration uses), and the prelude is its last section.
4. In comments, `ST_SRV(n)`, `ST_SAMPLER(n)`, `ST_CBV(n)`, `ST_UAV(n)` become `tn`, `sn`, `bn`, `un` (one site,
   `Include/DepthOfField.slang:9`).
5. Include guards `STRATA_<X>_GLSL` become `STRATA_<X>_SLANG`.
6. `invariant gl_Position;` becomes the comment "The position is invariant (the depth prepass's EQUAL test): Slang cannot
   say it, so the build decorates it (INVARIANT_POSITION in Strata/CMakeLists.txt)."
7. `Include/FilmicConstants.glsl` and `Include/AmbientOcclusionFit.glsl` are not converted but deleted, and the tool then
   runs `Tools/FitFilmic.py --rewrite` and `Tools/FitAOMultiBounce.py --write` (7.4) with `sys.executable -I`.
8. The twelve hand fixes (7.3) run last; each must match exactly once, else the tool fails without writing.
9. `--listing` writes every rewrite of the classes that can be silently wrong, as `file:line: before -> after`: each `mul`
   (with the GLSL operands' types), matrix index swap, `transpose(floatNxN(...))` constructor, `select` (whose argument order
   is `mix`'s reversed), `Interlocked*` with a hoisted return value, and every comparison it wrapped in `all()`/`any()`.
10. It prints the counts table (the spike's `converted.log`).
11. It drops preprocessor groups whose every branch is empty (the spike's tree has 14, in 3 files, left where output
    declarations moved into `ST_Output`: dead code under AGENTS.md checklist 3) and runs of more than one blank line it
    created. Neither changes a compiled byte (the module check below proves it).

**Before committing C3, the tree must equal the spike's.** Apply changes 2-7, 11 and hand fix 12's improved text to a copy
of `spike/converted/` (with `Prelude.slang` folded into `Common.slang`) and `diff -r` it with the new `Strata/shaders`:
empty, except `Scene/ShadowMesh.vert.slang` (C2b). The spike's tree is the one gates 1-4 already passed; any other
difference is explained in the commit message or fixed.

**And its modules must equal the spike's validated ones.** slangc 2026.20 is deterministic (the reviewer recompiled four
entries of `spike/converted/` into modules byte-identical to `spike/out/slang/`, the modules `StrataTestsSlang.exe` and the
4K captures ran). So every final `.spv` of C3's build is compared with `spike/out/slang/<flattened name>.spv` (Invariant
already patched there): byte-identical, or explained. Expected to differ: the four `Scene/*ShadowMesh*.vert` entries (C2b's
input locations), `PostProcess/Tonemap.frag` (the Filmic matrices written as rows) and any entry where folding the prelude
into `Common.slang` reorders declarations; for each, `spvreflect.py` between the two shows no difference except the shadow
entries' input locations, and `spirv-dis` of both differs only in ids and order. A difference outside these is a change the
spike never validated: fixed before the commit.

### 7.3 `Include/Common.slang` and the hand fixes

`Include/Common.slang`, in order: the guard; the binding comment ("Bindings are HLSL registers: `register(tN)` sampled
textures and read-only buffers, `sN` samplers, `bN` constant buffers, `uN` storage images and read-write buffers; `spaceS`
is descriptor set S, the binding set's position in the pipeline state. The build shifts each class to NVRHI's Vulkan
ranges, t +0, s +128, b +256, u +384 (`VulkanBindingOffsets`, CMake/StrataShaders.cmake)"); the converted helpers of
`Common.glsl` (`PI`, `NDCToUV`, ... `SanitizeHDR`); then the section "What the conversion from GLSL (wave 8) needs":

- The `static` stage globals `st_FragCoord`, `st_FrontFacing`, `st_FragDepth`, `st_Position`, `st_VertexIndex`,
  `st_InstanceIndex`, `st_GlobalInvocationID`, `st_LocalInvocationID`, `st_WorkGroupID`, `st_LocalInvocationIndex`, which
  each converted entry point fills before calling its `st_Main`, with the comment "New shaders take these as entry-point
  parameters (Docs/DeveloperGuide.md, "Shaders")".
- `TextureSize`, `TextureLevels`, `ImageSize`, `TexelFetch` overloads and `UnpackUnorm4x8` (`spike/tools/Prelude.slang:17-118`).
- `UnsignedMultiplyExtended(a, b, out high, out low)`: GLSL's `umulExtended` from 16-bit halves (HLSL has no 32x32 to 64
  multiplication without 64-bit integers).
- `PackHalf2x16` and `UnpackHalf2x16` with `__target_switch`: `spirv_asm` of `GLSL.std.450 PackHalf2x16`/`UnpackHalf2x16`
  on SPIR-V (Slang lowers `f32tof16` to `Float16` and `Int16` there, which the device does not require), `f32tof16`/
  `f16tof32` elsewhere. These are the tree's only `__target_switch` and `spirv_asm`.

The twelve hand fixes (`spike/tools/handfix.py`), in the tool's table:

| # | Where (GLSL line) | Class | Fix |
| --- | --- | --- | --- |
| 1-3 | `Scene/Mesh.vert:72`, `:77`, `:78` | A comparison of vectors used as one bool | `any(a != b)` per pair |
| 4 | `Effects/SceneColorCopy.comp:29` | Same | `all(size == last + 1)` |
| 5 | `PostProcess/MotionBlurNeighbors.comp:35` | Same | `any(neighbor != tile + int2(x, y))` |
| 6 | `PostProcess/TAA.comp:244` | Same | `all(motion == float2(0.0))` |
| 7 | `Scene/ShadowMesh.vert:44-45` | A `*` across two lines the converter missed | `mul(ViewProjection, float4(...))` |
| 8-9 | `PostProcess/DepthOfFieldComposite.comp:76`, `:79` | `texture()` in compute (glslang samples level 0; `Sample` would ask for compute derivatives) | `SampleLevel(..., 0.0)` |
| 10 | `Scene/Cull.comp:236` | A column write `world[3].xyz -= ...` | Three row writes `world[r][3] -= CameraPosition.<x,y,z>` |
| 11 | `Scene/CullBuild.comp:98` | `umulExtended` | `UnsignedMultiplyExtended` |
| 12 | `Scene/CullBuild.comp:54` | `gl_NumWorkGroups.x * gl_WorkGroupSize.x` (HLSL has no dispatch size) | `c_CullRowItems`, declared above `main` as `static const uint c_CullRowItems = 65535u * 64u;` with the comment "Dispatch (SceneRendererGPUDriven.cpp) fills rows of 65,535 groups of 64 whenever there is more than one row; with one row the y term is 0" (the spike wrote the bare product) |

### 7.4 The generators

- `Tools/FitFilmic.py`: `GLSL_OUTPUT` becomes `SLANG_OUTPUT` (`Include/FilmicConstants.slang`). The text: guard
  `STRATA_FILMIC_CONSTANTS_SLANG`, `static const float3x3 c_FilmicInset = float3x3(` with the matrix's three rows (the
  new `slang_matrix(m)` writes `m[row][column]` row by row; GLSL's `mat3` took columns, `:496-498`), the note "(rows:
  mul(c_FilmicInset, color))" instead of "(column-major)", `static const float` for the scalars. The C++ header's comment
  (`:537`) names `.slang`. New option `--rewrite`: writes both outputs from the committed fitted parameters
  (`read_committed`, `:556-562`, which reads the `.slang`), no fit. After the conversion, `FitFilmic.py --check` passes
  and `FilmicConstants.h` differs from 76f28a21's only in that comment.
- `Tools/FitAOMultiBounce.py`: the shader text (`:209-227`) becomes `static const float4 c_AOMultiBounce = float4(...)`
  and `float3 AOMultiBounce(float visibility, float3 albedo)` with `float3(...)` in place of `vec3(...)`, guard
  `STRATA_AMBIENT_OCCLUSION_FIT_SLANG`, path `.slang` (`:40`, `:230`, `:275`), help "write the Slang, C++ and test
  reference files" (`:287`). The fit is deterministic, so `--write` rewrites the C++ header and the test reference
  unchanged but for the comment naming the include.

Both are the converted form the spike validated, except that the Filmic matrices are written as rows instead of
`transpose(float3x3(...))` of columns: the same matrix.

### 7.5 `strata_add_shaders` on slangc

C3 switches the first command (6.1) to slangc and the includes to `.slang`. `glslang-standalone` is no longer referenced
(the target stays until C5).

### 7.6 Tests C3 changes or adds

- **`Architecture.Shaders`** (`T/Architecture/ShaderSourceTests.cpp`), ported:
  - `c_DerivativeBuiltins` (`:28-31`) becomes `ddx`, `ddy`, `ddx_fine`, `ddy_fine`, `ddx_coarse`, `ddy_coarse`, `fwidth`,
    `Sample`, `SampleBias`, `SampleCmp`, `CalculateLevelOfDetail`, `CalculateLevelOfDetailUnclamped` (method calls such as
    `u_Tex.Sample(` tokenize as a call of `Sample`). The first case's samples (`:204-238`) are rewritten in HLSL: `ddx` in
    a helper, `.Sample(` after a discard found, `.SampleLevel(` and `.Load(` not.
  - The second case reads `.slang` files (`:254`): the function set from the includes (no stage suffix) and the fragment
    files (`.frag.slang`), as today it reads `.glsl` and `.frag` only, so a compute or vertex file's `st_Main` never enters
    it; the fragments checked are those ending `.frag.slang`.
  - A call of `clip` counts as a discard (HLSL's conditional discard), beside the `discard` statement; the first case gains a
    crafted sample with `clip(alpha - 0.5);` before an `.Sample(` that is found. The comment at `:18-23` names the HLSL
    forms (`fwidth`, `ddx`/`ddy`, `Sample` without a level).
  - New: "Strata/shaders holds Slang sources only": every file ends in `.slang`; no `.glsl`, `.vert`, `.frag`, `.comp` is
    left. Fails on 76f28a21's tree.
  - New: "Shaders use HLSL syntax": over comment-stripped sources, the tokens `__target_switch`, `spirv_asm` and
    `__intrinsic_asm` appear only in `Include/Common.slang`; `import`, `__import`, `module`, `implementing`, `__include`,
    `interface`, `extension`, `__generic`, `associatedtype` and `__exported` nowhere; GLSL leftovers nowhere: the identifiers
    `layout`, `vec2-4`, `ivec2-4`, `uvec2-4`, `bvec2-4`, `mat2-4` and any `gl_*`, and calls of `texture`, `texelFetch`,
    `textureLod`, `mix`, `fract`, `dFdx`, `dFdy` (Slang accepts some of these silently). A crafted sample shows each class
    found. Generic functions (`T F<T>(...)`) are left to review: a textual check cannot tell them from comparisons.
- **`Renderer.ShaderModules`**, fourth case: "No embedded shader binds two used variables to one slot": no two variables of
  an entry point's interface share (set, binding). 0 clashes on Slang's modules; it would fail on glslang's (25), which
  is why it joins in C3. It is what makes disabling warning 39001 safe.
- **`Renderer.ShaderModules`'s third case** now fails if the tool's patch is removed (Slang's modules have no Invariant);
  its second fails if `PackHalf2x16` goes back to `f32tof16` (the spike saw `Float16` and `Int16`).
- **`GPU.Renderer`'s module case** validates all 103 Slang modules after the patch.

### 7.7 What C3 must show before it is committed

The tree equals the spike's and its modules equal the spike's validated modules or are explained (7.2); the build has no
shader warning; `--listing` reviewed line by line (every `mul` has a
matrix operand, every constructor of a matrix transposes columns or takes rows, every `select` has the condition first and
the values swapped from `mix`); the lane's tests pass in Debug; `Tools/FitFilmic.py --check` passes.

## 8. C4 and C5: what leaves

### 8.1 C4: the converter

`git rm Tools/ConvertShadersToSlang.py`. The commit message names C3's hash and how wave 9 would run it again:
`git show <C3>:Tools/ConvertShadersToSlang.py > <scratch>/Convert.py` on a tree of GLSL shaders.

### 8.2 C5: the glslang submodule

- `git rm Strata/vendor/glslang` (removes `.gitmodules:53-56` too), `Strata/vendor/CMakeLists.txt:226-235` deleted, its
  header comment (`:3-6`) gains "The shader compiler (Slang) is not here: CMake/StrataSlang.cmake fetches a pinned release
  archive.", `ThirdPartyNotices.md:24` deleted.
- `.git/modules/Strata/vendor/glslang` stays in the shared repository during wave 8: `.worktrees/wave8-base` (76f28a21, the
  A/B base) and older branches need it to rebuild. The main checkout's working copy keeps an untracked
  `Strata/vendor/glslang` folder after the landing, and it **stays until 0.8.0 ships** (deleted with `.worktrees/wave8-base`):
  `create_worktree.sh` points every submodule at the main checkout's working copy (`$repo/$path`, `:19-26`), so without that
  folder a worktree at a commit before C5 (a bisection, the wave 9 fallback's branch) could not get glslang offline. Nothing
  reads it: no CMake file, test or glob reaches `Strata/vendor/glslang` after C5.
- CI checks out 926 MB less and no longer compiles glslang (time not measured; information in gate 5's report).

## 9. C6: the console route stays checked, `StrataShaders.TargetHLSL`

A CTest with no label (so every CI job runs it): `${CMAKE_COMMAND} -DSLANGC=<STRATA_SLANGC> -DENTRIES=<STRATA_SHADER_ENTRIES>
-DREPOSITORY=${PROJECT_SOURCE_DIR} -DOUTPUT=<build>/ShaderTargets/hlsl -P StrataTests/Shaders/TargetHLSL.cmake`, `TIMEOUT
300`. For each manifest line it runs `slangc <repository>/<source file> -entry main -stage <stage> -target hlsl
-matrix-layout-column-major -warnings-disable 39001,41012 -I <repository>/Strata/shaders -D<defines> -o <output>/<id>.hlsl`
(the spike's `targets.py` form, which gave 103 of 103; no profile, no `-fvk-*` flags, no DXC). It fails listing every entry
that emits no HLSL with its first error line, else prints `<n> of <n> entries emit HLSL`. About 20-30 s of one core.

It fails when a shader uses `spirv_asm` outside a `__target_switch` with a default case, or Vulkan-only constructs with no
HLSL form. What a D3D-style target would still need (DG "Shaders", and the open items): `SV_VulkanInstanceID` and
`SV_VulkanVertexID` replaced, since `SV_InstanceID` excludes the base instance the draw list depends on; the 43 push
constant blocks given `register(bN)` from their C++ slots; NVRHI's `registerSpace` for set 1; HLSL's constant buffer
packing, which differs from std140; the six Forward variants' 3,600-byte `ReflectionProbeBuffer` element against DXC's
2,048 (the spike: 97 of 103 through DXC 1.8.2502 once the semantics were replaced).

## 10. The tests at a glance

| Test | Kind | Commit | Fails without the change because |
| --- | --- | --- | --- |
| `StrataTools.LookDev` selftest (signed difference, summary) | CTest, `gpu;tools` | C0 | The functions do not exist |
| `StrataTools.LookDevOffline` (the same checks without an editor) | CTest, `tools` (CI runs it) | C0 | `selftest` requires `--build`; the functions do not exist |
| `StrataSlang.Fetch` (9 expectations) | CTest, CMake script | C1 | The module does not exist |
| `Renderer.ShaderTool` (6 cases) | doctest | C2 | The tool does not exist |
| `Renderer.ShaderModules` (3 cases, a 4th in C3) | doctest | C2, C3 | Invariant: without the patch; capabilities: `f32tof16`; bindings: glslang's modules |
| `GPU.Renderer` "Every embedded shader becomes a module..." | doctest, `gpu` | C2 | An invalid module (a corrupting patch) |
| `Architecture.Shaders` (2 ported, 2 new cases) | doctest | C3 | "Slang only" and "HLSL syntax" fail on GLSL sources; the ported first case's `clip` sample fails on the GLSL-era check |
| `StrataShaders.TargetHLSL` | CTest, CMake script | C6 | A Vulkan-only construct |
| CI: "Validate the embedded shaders", "Convert the embedded shaders to Metal" | CI steps | C2 | An invalid or unconvertible module |
| Existing: every `GPU*` suite under validation, `StrataTests.FeatureTest` (unexpected warnings), the samples' CTests, `GPU.SceneRenderer.GPUDriven` and `.Feedback`, `GPU.Renderer.LODSelection` (CPU and GPU agree on 10,000 cases, the `precise` site) | | | They hold the image and both draw paths |

A shader change can make the validation layer warn where it did not (Slang drops unused inputs and outputs; the audit found
only the shadow inputs, fixed in C2b). The feature test counts unexpected warnings under the layer, so Debug ctest finds any
other; each is fixed at its cause (a trimmed layout or interface), never classified away.

## 11. MoltenVK and Mesa (D3, D9)

### 11.1 The Mac

No Mac is reachable from this session (D9). The minimum is CI's conversion of every embedded module to Metal (6.5) on the
macOS job, glslang's run of C2 as the baseline. It is a lower bound: MoltenVK configures its argument buffers and resource
indices from the pipeline layout at run time, which the offline conversion cannot know. If a Mac session comes online
before the lane lands, `ctest --preset macos-debug -L gpu` runs there on the lane's tip, with the validation layer set up as
the build-test skill says (`SKILL.md:63-65`), and a pass replaces the lower bound in gate 6. Otherwise the first real GPU
run through MoltenVK stays an open item, plan 7's Mac gate stays, and the release notes say so (section 13.8).

### 11.2 Mesa's lavapipe, tried (inside the box, D9; at most 2 hours of agent time, information only)

Two refs, never merged: `ci/slang-mesa-c2` = C2 plus one commit adding a job `gpu-lavapipe`, and `ci/slang-mesa` = the
lane's tip plus the same commit (cherry-picked). The job: `ubuntu-24.04`,
`timeout-minutes: 90`, the Linux dependencies plus `mesa-vulkan-drivers vulkan-validationlayers`, `cmake --preset linux`,
`cmake --build --preset linux-release --target StrataTests`, then `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
build/linux/bin/Release/StrataTests --test-suite='GPU*' --no-breaks`. Run it once on C2 (glslang) and once on the tip; the
two pushes run while the agent works on the gates, so the agent time is the job's commit and reading two logs. Whether
device selection accepts a CPU device and whether the GPU tests turn the layer on from the environment there are
unverified: the first run answers both.
Outcomes: the device is rejected (a required feature lavapipe lacks: logged by device selection) -> recorded; or it runs ->
the two failure lists are compared, a failure only with Slang's modules counts against gate 2 and is fixed, failures with
both are lavapipe's and recorded. Either way the result goes into the release notes and open items; a permanent lavapipe
job is a "Later" entry if it ran.

## 12. The six gates

Run in this order once C0-C7 are committed. A = the Release build of C2b (glslang, every C++ change of the lane), its
`bin/Release` copied to `G:/GameEngineAI/.worktrees/wave8-plan/slang/c2b-release/` (the spike ran copied programs the
same way); B = the Release build of C7.

| # | Gate | Number | How it is measured on this machine |
| --- | --- | --- | --- |
| 1 | Reflection | Every difference between A's and B's final modules is one of the audit's explained classes: resources and inputs Slang drops as unused (and `gl_PerVertex` members), multi-member blocks as one-element-indexed structs, scene aliases of unused bindings, `SampledCubeArray` not declared (the 6 Forward entries), `SPV_KHR_storage_buffer_storage_class` added. No difference of set, binding, type, offset, stride, matrix stride, location, flat, Invariant, execution mode or kill/demote, and every module whose A form decorates `NoContraction` (the `precise` product of `Include/LODSelection`: `Debug/LODSelection.comp`, `Scene/Cull.comp`, `Scene/CullShadow.comp`) keeps at least one in B (a few lines added to the copied script, which does not list decorations other than those) | The spike's `spike/tools/spvreflect.py`, copied to a scratch folder with `ROOT` replaced by two arguments (A's and B's `EmbeddedShaders` `.spv` files, flattened to the spike's names). Its report goes into the gate's note |
| 2 | Validation | `spirv-val --target-env vulkan1.2`: 103 of 103 final modules. Debug and Release: every `GPU*` case passes under the layer with 0 validation errors (the spike's one failure was its copied exe's), `GPU.Renderer`'s module case included; the feature test without unexpected warnings; the samples' CTests; `Renderer.*`, `Architecture.*`. And no "not consumed by vertex shader" (or any other validation warning that A's run does not show) in one Debug run of `StrataTests --test-suite=GPU*` with the layer's duplicate message limit raised, since the spike's log hit the limit of 10 (2.2) | `G:/GameEngineAI/build/_tools/Vulkan-ValidationLayers/external/Debug/64/SPIRV-Tools/build/install/bin/spirv-val.exe` over B's `EmbeddedShaders` (the manifest's names); `ctest --preset windows-debug -j 8` and `ctest --preset windows-release -LE perf -j 8` with `VK_ADD_LAYER_PATH` set. The warning run: a `vk_layer_settings.txt` holding `khronos_validation.duplicate_message_limit = 100000` in a scratch folder named by `VK_LAYER_SETTINGS_PATH` (the setting's name in this layer build is unverified: if the layer ignores it, `grep -c "duplicate_message_limit value"` on the log is not 0 and the agent finds the right name in the layer's `VkLayer_khronos_validation.json` before the gate counts), the log grepped for `[warning] Strata: Vulkan:` and compared with the same run on A |
| 3 | Look-dev | Per mode, A against B (C0's summary): off: median differing pixels at most 2,000, median largest 1, max largest at most 8, max mean at most 0.001; FXAA: at most 2,000, 1, 12, 0.001; TAA: median largest at most 20, max mean at most 0.03. Every differing picture's signed image inspected (gain 32 for off and FXAA, 8 for TAA): a region that follows geometry, a light's footprint, a shadow's edge or a material (a shape like `spike/lookdev/lantern_signed.png`) is a bug, whatever the numbers. The control A1 against A2 differs in 0 pictures | One script file, `<scratch>/gates34.sh`, run as `$PY -I Tools/PerfLock.py --label lookdev-slang --limit 90 -- bash <scratch>/gates34.sh`, holds the four captures in this order, each `$PY -I Tools/LookDev.py capture --build <A or B> --label <A1\|B1\|A2\|B2> --output $W/<label> --sizes 3840x2160 --aa taa,fxaa,off --repeat 2 --projects ThirdPerson,PhysicsPlayground,OpenWorld,LookDev --work $W/work` (84 pictures, the spike's `run3.sh` set; `$W` = `G:/GameEngineAI/.worktrees/wave8-plan/slang/gates`), then gate 4's two Medium captures; afterwards, outside the lock, `compare $W/A1 $W/B1 --output $W/compare-A1-B1 --signed-gain 32` (and `--signed-gain 8` into `compare-A1-B1-taa` for the TAA pictures) and `compare $W/A1 $W/A2 --output $W/compare-control` |
| 4 | GPU time | M = (sum of B1's and B2's `gpuBusyMs`) / (sum of A1's and A2's), over the 84 pictures; C = A2 / A1. Pass if abs(M - 1) <= abs(C - 1) + 0.03, and the same rule for the sums of `GPU.Opaque`, `GPU.Prepass`, `GPU.Shadows` and `GPU.Post` (other stages information). The spike: M 0.974, C 0.967. Information, for "Medium keeps 0.6.0's costs" (AGENTS.md, Rendering): one capture each of A and B at Medium, `--quality medium --sizes 1920x1080 --aa taa,fxaa,off --repeat 2` on the same four projects, and their `gpuBusyMs` ratio; a ratio above 1.03 is investigated before the lane lands | The four 4K captures' `stages.csv` and the two Medium ones, taken alone on the machine: the orchestrator holds other lanes' builds and GPU work for about 35 minutes (plan 4.1: readings beside running work are never a pass). If that window cannot be had, the gate waits for it |
| 5 | CI | `ci/slang` green on all six jobs, with "Validate the embedded shaders" (103 of 103), `StrataShaders.TargetHLSL` (103 of 103), `StrataSlang.Fetch` and `StrataTools.LookDevOffline` passing | `python Tools/CIStatus.py status <commit>` on the pushed commit |
| 6 | MoltenVK | The Mac's `-L gpu` passing if a Mac session is online (11.1). Otherwise CI's conversion with the `c_KnownFailures` list C2 filled from glslang's modules (6.5): the step passes on Slang's modules, so every entry glslang's modules converted and compiled also converts and compiles; the target is an empty list (103 of 103). Every listed entry is a limit of the offline conversion, named in the open items; plan 1.1's row "converts to Metal" then reads "except the listed entries, as glslang's modules did" | The macOS job's "Convert the embedded shaders to Metal" log of `ci/slang-c2` and `ci/slang` |

Gates 1-3 together are the defence against silent slips; gate 3's signed inspection is the one that caught the `mul`.

## 13. Docs

### 13.1 AGENTS.md (C7; 440 code points, within lane Slang's share of S0.1's room)

S0.1 leaves 3,456 code points free and gives lane Slang "about 450" of them (step0.md 5.4: the Shaders bullet, plan 4.1's
rules and D1's sentence), lane Platform about 200, and at least 2,800 to the streams. Lane Slang uses 440 (counted with the
test's rule, UTF-8 code points, against 76f28a21's lines; S0.1 moves the line numbers, so each edit is found by its text).
Wrapped at 120 columns.

- `:36`, the `Strata/` row: "`shaders/` the GLSL sources" becomes "`shaders/` the Slang sources" (+1).
- `:50` ("Dependencies are git submodules. After cloning:") stays: the third-party section below and DG "Building" say
  that the first configure fetches Slang.
- `:203-207`, the Shaders bullet, replaced whole (+258):

  > - **Shaders:** HLSL-syntax Slang in `Strata/shaders`, compiled to SPIR-V by the pinned slangc and embedded; every
  >   entry is listed in `strata_add_shaders` in `Strata/CMakeLists.txt` (re-run configure after adding one). Bindings are
  >   `register(tN|sN|bN|uN, spaceS)`; push constants share the `b` registers with constant buffers: give them a slot no
  >   constant buffer of the pipeline's other layouts uses. HLSL only: no generics, `__target_switch` or `spirv_asm` outside
  >   `Include/Common.slang`; `mul` only with a matrix; vector comparisons through `all()`/`any()`; matrix constructors take
  >   rows; `SampleLevel` in compute. A lighting bug in unchanged code may be a conversion slip
  >   ([Shaders](Docs/DeveloperGuide.md#shaders)).

- `:487-488`, appended to "...commit the submodule bump separately." (D1's sentence, +181): " Slang is no submodule:
  `CMake/StrataSlang.cmake` pins its release archives by SHA-256, fetched once into a per-user cache (offline:
  `STRATA_SLANG_ROOT`); bump it in its own commit."

The bullet states plan 4.1's shader rules in short (the dot product of `mul(v, w)`, componentwise `M * M`, `M[row][col]`
and the signed look-dev compare against 0.7.0 are in DG "Shaders", items 2 and 7); DG "Shaders" states them in full
(checklist item 5: change both). `Docs.AgentGuide` checks the size and the link's anchor (`#shaders`: no DG heading slugs
to it today). The later lander of lanes Slang and Platform measures after merging main (step0.md 10.3).

### 13.2 DG: the new section "Shaders"

The first subsection of "Rendering" (before `:3457`), with its line in Contents (after `:98`, "- [Rendering]"). In this
order:

1. **Language.** HLSL-syntax Slang; a stage file `<registered name>.slang`, includes `Include/<Name>.slang` with
   `STRATA_<NAME>_SLANG` guards; every stage file includes `Include/Common.slang` first.
2. **Rules in full.** `mul(M, v)` for `M * v`, `mul(A, B)` for `A * B`; `mul(v, w)` of two vectors is a dot product and
   `M * N` componentwise, both silently. Comparisons of vectors give bool vectors: `all()`/`any()`. `floatNxN(...)` takes
   rows, `M[row][col]`; a GLSL column is `transpose`d. `select(condition, ifTrue, ifFalse)` is `mix(ifFalse, ifTrue,
   condition)`. `Interlocked*` return through an out parameter. `SampleLevel`/`Load` in compute (`Sample` asks for compute
   derivatives). Every storage image declares `[vk::image_format("...")]`. Bindless reads use `NonUniformResourceIndex`.
   Vertex and instance ids are `SV_VulkanVertexID`/`SV_VulkanInstanceID` (the draw list is indexed by the instance index
   including the base instance). `precise` where fused or reordered math would break agreement (`Include/LODSelection.slang`).
   No generics, modules, interfaces or extensions; `__target_switch` and `spirv_asm` only in `Include/Common.slang`.
   `static const` for constants. New shaders take stage inputs as entry-point parameters (the converted files'
   `st_*` globals are the conversion's, until wave 9). Derivatives before the first `discard` or `clip` (`Architecture.Shaders`).
3. **Bindings.** Registers, spaces, the shifts, push constants (`[[vk::push_constant]] ConstantBuffer<T>`, their slot only
   in C++'s `BindingLayoutItem::PushConstants`), set 1 (the bindless table, Tonemap's second set), layouts std140 for
   constant buffers and std430 for structured buffers as before.
4. **The build.** The two commands, the flag table of 6.1 with every reason and the flags not passed, the Invariant patch
   and `INVARIANT_POSITION`, the outputs (`.raw.spv`, `.spv`, `.h`, `ShaderEntries.txt`), configuration-independent bytes,
   what recompiles when.
5. **The pin.** Version, archives, the per-user cache (its folders `slang-<version>-<host>`, which a bump leaves behind for
   the user to delete) and its lock, `STRATA_SLANG_ROOT`, the messages, and how to bump:
   new version and four digests (each archive downloaded and hashed, not taken from an API alone), the extracted file lists
   checked, then Debug and Release GPU suites under the layer, a look-dev compare against the previous build with
   median largest 1 without AA and the signed images inspected, `StrataShaders.TargetHLSL` and CI's two steps green: one
   commit of its own.
6. **The checks.** `Architecture.Shaders`, `Renderer.ShaderModules` (with the capability allow-list and how to extend it:
   only for a feature device selection requires, or, for `StorageImageExtendedFormats`, per entry with its fallback),
   `Renderer.ShaderTool`, `GPU.Renderer`'s module case, `StrataShaders.TargetHLSL`, `StrataSlang.Fetch`,
   `StrataTools.LookDevOffline`, CI's spirv-val and Metal steps (and the Metal step's `c_KnownFailures`, which only shrinks).
7. **A conversion slip.** The classes the spike found (section 7.3's table and the `mul` of two vectors that only the look-dev
   compare caught) and the procedure: 0.7.0's look-dev baseline (S0.0), a capture of the suspect build, `compare
   --signed-gain 32`, a coherent region in the signed image points at a converted line.
8. **Other targets.** Section 9's list of what a D3D-style target still needs; Metal through MoltenVK's conversion only.

### 13.3 The rest of DG

- `:135` (Tools row): `FilmicConstants.glsl` becomes `.slang`; `FitFilmic.py --rewrite` named.
- `:155-156`: "(headers and the shader compiler are vendored)" becomes "(the headers are vendored; the shader compiler,
  Slang, is downloaded once at the first configure into a per-user cache, or taken from `STRATA_SLANG_ROOT`: "Shaders")".
- `:187-199` (CI): Linux validates the embedded shaders with spirv-val, macOS converts them to Metal, every job runs
  `StrataShaders.TargetHLSL`.
- `:661-664` (look-dev compare): C0's signed images and summary; `:670-673` (the selftest): `selftest` without `--build`
  and `StrataTools.LookDevOffline`.
- `:4020-4025` (the derivative rule): "`fwidth`, `dFdx`, `texture` without an explicit level" becomes "`fwidth`,
  `ddx`/`ddy`, `Sample`/`SampleBias`/`SampleCmp` without an explicit level", and "before the discards" becomes "before the
  first `discard` or `clip`".
- Every other `.glsl` and `GLSL` mention (37 in DG) becomes `.slang` or "Slang"; `ST_SRV(16)` (`:5087`) becomes `t16`,
  `ST_CBV(0)` (`:4465`) `b0`.

### 13.4 Elsewhere

- `Docs/Architecture.md:296`: "SPIR-V compiled from `Strata/shaders` by glslang at build time" becomes "SPIR-V compiled
  from the Slang sources in `Strata/shaders` by the pinned slangc at build time"; `:2304` `ST_SRV(15)` becomes `t15`; the
  other `.glsl` mentions (`:2014`, `:2419`, `:2426`, `:2442`, `:2456`, `:2524`) become `.slang`.
- `README.md:98-102`: one sentence, "The first configure downloads the shader compiler (Slang, 25-64 MB by platform) into
  a per-user cache; offline, see `Docs/DeveloperGuide.md` ("Shaders")."
- `.claude/skills/strata-build-test/SKILL.md:20-21`: "Shaders are not globbed: add every new shader (`<name>.slang`) to
  `strata_add_shaders` ... The first configure downloads Slang; offline, set `STRATA_SLANG_ROOT`."
- Comments naming `Include/X.glsl` in C++, tests and tools (49 files besides the docs and the files C3 rewrites: 28 in
  `R/` and `Strata/src/Strata/Asset/HDRSun.h`, `StrataEditor/src/Editor/ProbeBake.*`, the test files, and
  `Tools/GenerateBlueNoise.py:131`, `StrataTests/FeatureTest/Tools/GenerateAssets.py:145`), and `R/PostProcessData.h:17`
  (`ST_CBV(0)` becomes `b0`): mechanically to `.slang` and register names. `R/FilmicConstants.h` and
  `R/AmbientOcclusionFit.h` changed already in C3, through their generators.
- Unchanged, as history: `Docs/Plans/**`, `Docs/Releases/0.*.md`, the Roadmap's entries of landed waves.
- The rule for all of them: `git grep -n -E '\.glsl\b|GLSL|glslang|ST_(SRV|SAMPLER|CBV|UAV)\('` outside `Strata/vendor`,
  `Docs/Plans` and `Docs/Releases` finds nothing after C7 except the Roadmap's history (174 occurrences in 58 files on
  76f28a21 for the first three patterns; the reviewer counts 178 in 58 for all four outside `Strata/shaders`). The same
  grep runs again after the lane merges main (section 16): S0.1's and lane Render's new DG text may name `.glsl` files.
- No Manual page changes: games have no shaders of their own.

### 13.5 `Docs/Roadmap.md` open items (the lane's; S0.12 writes the wave's entry)

The first GPU run of Slang's SPIR-V through MoltenVK (unless 11.1 ran it); AMD, Intel and Mesa drivers unseen (with 11.2's
result); the converted files' `st_*` stage globals and the hand port (C8 if cut); what a D3D-style target needs (section
9); a shader debug-information option (`-g`, configuration-dependent outputs) for RenderDoc; three findings to report to
Slang (no `invariant`, `spirv_asm OpDecorate` silently ignored, `f32tof16` needing `Float16` on SPIR-V), recorded, not
filed (18, Q3); `IBL/BRDFLut.comp` needs `shaderStorageImageExtendedFormats` (RG16F) while device selection does not
require it (pre-existing; every desktop GPU and MoltenVK have it); the Metal step's `c_KnownFailures`, if any, with their
reasons; the main checkout's untracked `Strata/vendor/glslang` and `.git/modules/Strata/vendor/glslang`, deleted with
`.worktrees/wave8-base` after 0.8.0 ships (8.2).

### 13.6 Release notes (0.8.0, lines this item contributes)

In C7, lane Slang's block (bold "**Slang (S0.10)**" first) is appended under each heading of
`Docs/Plans/Wave8/ReleaseNotes.md`, which S0.0d creates and every landing appends to (step0.md 4.5); `Docs/Releases/0.8.0.md`
is written from it at the release.

- What's new: "Shaders are written in Slang with HLSL syntax and compiled by slangc 2026.20, still to SPIR-V for Vulkan
  and MoltenVK: the step toward other platforms' shader compilers. Pictures differ from 0.7.0 by rounding only (at most a
  few levels on edge pixels; with TAA, small differences spread through the history)."
- Upgrading from 0.7.0, building from source: "The glslang submodule is gone: after pulling, `Strata/vendor/glslang` may be
  deleted (git leaves the folder, about 900 MB; keep it while you build commits from before 0.8.0). The first configure downloads Slang's release archive
  (25-64 MB) into a per-user cache (`%LOCALAPPDATA%\Strata\Toolchains`, `~/.cache/strata/toolchains`,
  `~/Library/Caches/Strata/Toolchains`) and checks its SHA-256; offline, download the archive `CMake/StrataSlang.cmake`
  names on another machine, extract it and configure with `-DSTRATA_SLANG_ROOT=<folder>`."
- Known issues, Platforms: "This release changes every shader. Its GPU tests ran on Windows with an NVIDIA GPU. On the
  Mac, CI converted every shader to Metal and compiled it, but no GPU test ran through MoltenVK" (or the Mac's result);
  and lavapipe's result.
- No rendering default changes. These lines are the plan's section 6 rows "Compiled shaders" and "First configure of a
  clone".

## 14. Acceptance, and how each number is measured here

| What | Target | Measured by |
| --- | --- | --- |
| Entries through slangc | 103 of 103 (and every later entry) | The build; the manifest's line count |
| Byte identity of C2 | 103 of 103 modules equal 76f28a21's | 6.3 |
| Gates 1-6 | Section 12's numbers | Section 12 |
| Invariant | On exactly the 4 `Scene/Mesh*.vert` entries | `Renderer.ShaderModules` |
| Declared capabilities | Within the allow-list; no `Float16`, `Int16` or derivative capability | `Renderer.ShaderModules` |
| HLSL | 103 of 103 emit | `StrataShaders.TargetHLSL` on every CI job |
| Metal | Slang's set contains glslang's (the step passes with C2's `c_KnownFailures`); target 103 of 103 | CI's macOS step, C2 against the tip |
| Modules against the spike's | Byte-identical to `spike/out/slang/`, or each difference explained (7.2) | C3's check before its commit |
| Clean checkout | 926 MB smaller | `du -sh Strata/vendor/glslang` before C5 |
| Cache | One fetch per user and machine; a second configure, and a configure of a second build tree, download nothing; a full build writes nothing into the cache | Configure output (`cached`); a file listing of `%LOCALAPPDATA%\Strata\Toolchains` before and after a clean build: identical |
| Offline | A configure with `STRATA_SLANG_ROOT` set needs no network | `StrataSlang.Fetch` case 2 and a configure of a fresh build tree with `-DSTRATA_SLANG_ROOT=<the cache folder>` |
| Shader compile time | Information: the spike measured 21 s against 11 s of one core for all entries (3 s against 1.5 s at 8 jobs) | Wall time of `cmake --build ... --target Strata_Renderer` after touching `Include/Common.slang` (B) and `Include/Common.glsl` (A), once each |

## 15. Perf

No `Budgets.json` entry: the lane adds no pass, stage or runtime work, and nothing is measured against budgets on the
temporary memory (decisions D4). Its A/B is gate 4, interleaved A1 B1 A2 B2 under `Tools/PerfLock.py --label
lookdev-slang --limit 90`, with a control, as plan 4.1 allows streams to A/B their own work. Information only: the module
creation time `GPU.Renderer`'s case prints, and the compile times of section 14. Pipeline creation (Slang's modules are
23% smaller: 1,553 against 2,008 KiB) is covered by plan 7's step 8 A/B against `.worktrees/wave8-base`, which times
whole frames and startups; this lane does not campaign.

## 16. What the item shares, and how merges resolve

The lane merges main before its last verification (plan 3). By then S0.0 and S0.1 are in, and lanes Render, Editor and
Platform may be.

| With | Shared | Resolution |
| --- | --- | --- |
| Lane Platform (S0.8, S0.9) | `Strata/CMakeLists.txt`: Platform edits the `glfw` link rows (`:61`, `:64`); Slang adds one line in the `strata_add_shaders` call (after `:180`) | Different hunks: keep both |
| | `Strata/vendor/CMakeLists.txt`: Platform makes the GLFW block (`:50-55`) conditional; Slang deletes the glslang block (`:226-235`) and adds a header line (`:3-6`) | Different hunks: keep both |
| | `ci.yml`: Platform adds `-DSTRATA_WITH_GLFW=OFF` to the `Linux (no Audio, no Physics)` row's `options` (`:59`); Slang edits the two install lines (`:82-83`, `:89`), adds two steps after Build and two log names to Annotate | Different hunks. Slang's Linux step is keyed on `matrix.key == 'linux-release'`, which Platform's row does not use; if Platform renames keys, the condition follows the `Linux` row's key. If both edit the apt line, the union |
| | `CMakeLists.txt`: Platform adds `option(STRATA_WITH_GLFW ...)` | Slang does not edit it |
| | `StrataTests/CMakeLists.txt`: Platform's test lists; Slang's three CTests (`StrataSlang.Fetch`, `StrataShaders.TargetHLSL`, `StrataTools.LookDevOffline`) and `StrataShaderToolCore` link | Additive |
| | DG "Building", the build-test skill (Platform `:36`, Slang `:20-21`) | Different paragraphs; additive |
| | `AGENTS.md`: Platform about 200 code points, Slang 440 | The later lander measures with `Docs.AgentGuide` after merging main (step0.md 10.3) |
| Lane Render (S0.2, S0.3) | No shader (S0.3 is all in `ShadowFitTracker`, `R/ShadowFit.cpp`); `R/SceneRenderer.cpp` only if S0.3 touches it | C2b's hunks are at `:311-329`, `:545`, `:2371`; resolve by hunk. Gates 2 and 5 run again after the merge |
| S0.1 | `AGENTS.md` (room, golden rule 1 for D10), `ThirdPartyNotices.md` intro (D10's section) | S0.1 lands first; C1 and C7 apply on top |
| Any lane that edits a shader before C3 | `Strata/shaders` | The orchestrator keeps shaders closed to other lanes (plan 3); if one lands anyway, merge main before C3 and convert that state; the hand-fix table must still match (else the tool fails, by design) |
| `showcase` | `Tools/LookDev.py` (its E4: the sample, shots appended), `ThirdPartyNotices.md` (sample rows) | C0 changes `compare` and `selftest` only; additive |
| S0.11 | Writes `Include/IndirectLight.slang` and the slot comment of `Include/Scene.slang`; compares against this lane's baseline | Branches from the lane's landing |
| Rendering streams (`reflections`, `radiance-cascades`, `local-lights`, `volumetric`, `editor-ux`, `showcase-engine`) | Every shader they write | They branch from S0.12's tip and write `.slang` under 13.2's rules; their specs' `Include/X.glsl` means `Include/X.slang`; registered names unchanged. A lighting bug in code nobody changed: the signed compare against 0.7.0 first |
| Roadmap, release notes | 13.5, 13.6 | Additive; S0.12 owns the wave's entry |

After merging main, 13.4's `git grep` runs again and every new `.glsl`, `GLSL` or `ST_*(n)` mention the merge brought (S0.1's
and lane Render's DG text) is renamed in the merge commit.

**The lane's last act** (plan 3), after the merge of main with lane Render in it and gates 2 and 5 green again: the
look-dev baseline in Slang, the **84-set** (`--sizes 3840x2160 --aa taa,fxaa,off --repeat 2 --projects
ThirdPerson,PhysicsPlayground,OpenWorld,LookDev`, the set S0.11's gate compares, step0.md 8.5 and 10.2), captured by the
merged tip's Release build under PerfLock into `G:/GameEngineAI/.worktrees/wave8-plan/lookdev/step0-slang/`, and its compare
against `lookdev/wave8-base-84` kept for the record (S0.2's and S0.3's differences are expected there). S0.11 captures its
own base again before any edit (step0.md 8.5) and compares against that; this set is the lane's record. Then the
orchestrator lands the lane (plan 3, "Landing step 0") and rebuilds `build/windows`.

## 17. The box's end: one extension, then wave 9

At the box's end the orchestrator reads the gate table. Green: the lane lands. Not green:

- **One extension of at most 1 agent-day**, only if every remaining failure is understood and small: each has a named cause
  and a fix of at most about 50 lines in at most three shaders or one CI script, or is a CI infrastructure failure with a
  known cause (an archive layout, a runner's tool). The extension's plan lists them.
- **Otherwise, or if the extension ends without green: wave 9's step 0.** Nothing of the lane lands except C0 (useful to
  every look-dev check; whether C2 and C2b land too is the orchestrator's open question, section 18). `agent/wave8-slang`,
  `ci/slang` and `ci/slang-c2` stay pushed with a note of each failing gate in the
  last commit's message; S0.12 records "Slang as step 0" in wave 9 with that list. The rendering streams branch from main
  with glslang; S0.11 lands in GLSL; the look-dev baseline is taken on main in GLSL. In wave 9 the converter runs again
  from history on the new GLSL (`git show <C3>:Tools/ConvertShadersToSlang.py`); new shaders need hand-fix entries only
  for the classes of 7.3.

## 18. Risks and open questions

| Risk | Mitigation |
| --- | --- |
| A silent slip in a path no test or shot covers (the audit's main risk) | The tree equals the spike's validated tree (7.2); `--listing` reviewed; gates 1-3; the AGENTS.md rule pointing lighting bugs at the conversion; 0.7.0's baseline kept |
| The Linux or macOS archive differs in layout, or its slangc does not run on the runner (glibc, signing, libraries) | Archives listed and hashed in the first hour; C1 runs `slangc -v` at every CI configure before anything depends on it; `STRATA_SLANG_ROOT` |
| GitHub is unreachable at configure: CI or a fresh clone fails | Three attempts, the per-user cache, a clear offline message; a GitHub Actions cache of the toolchain folder if it ever flakes (Q2) |
| MoltenVK at run time differs from the offline conversion | Gate 6's lower bound named as such; open item; plan 7's Mac gate |
| Non-NVIDIA drivers meet Slang's SPIR-V first in players' hands | The lavapipe try; the release notes' known issue |
| Slang changes how it emits the position | The tool fails the build (`NoPositionOutput`), `Renderer.ShaderModules` checks the four entries |
| New validation-layer warnings from interfaces Slang trims | Debug ctest's feature test finds them; each fixed at its cause (10); gate 2's run with the duplicate message limit raised, since the spike's log was cut at 10 reports |
| A shader with an extended storage format and no fallback (as `BRDFLut.comp` already is) | `Renderer.ShaderModules` allows the capability per named entry only; the existing case is an open item |
| The offline Metal conversion refuses some modules whatever compiled them | C2 lists them from glslang's run; gate 6 compares against that list; open item |
| `-warnings-as-errors all` makes a Slang bump fail on new warnings | Intended: the bump procedure (13.2.5) handles each |
| A later Slang version treats a disabled warning differently | The pin; the bump procedure |
| AGENTS.md's budget | 440 code points, within step0.md 5.4's "about 450" for the lane; the later lander of Slang and Platform measures |
| Windows path lengths in the cache | Only the run set is extracted |
| The main checkout keeps a 926 MB untracked folder and `.git/modules` copy | Both stay until 0.8.0 ships, because `create_worktree.sh` takes submodules from the working copy (8.2); then deleted with `.worktrees/wave8-base` |

Open questions, with the decision recommended:

- **Q1. Keep CI's spirv-val and Metal steps after the lane lands?** Yes: about 1-2 minutes each, and they keep the
  acceptance row "every entry ... converts to Metal" true for the wave's new shaders.
- **Q2. Cache the Slang folder in the GitHub Actions cache?** No for now: one 25-64 MB download per job is small beside the
  checkout. Add it only if downloads flake.
- **Q3. File the three upstream reports?** Record them in the open items; filing issues in public under the owner's
  account is the owner's call, not the lane's.
- **Q4. The hand port inside step 0?** Only under 3.3's condition. The rule that new shaders take entry parameters holds
  either way.
- **Q5. Pins for Linux ARM64 and Windows ARM64?** Not in wave 8: no preset or CI job builds there; `STRATA_SLANG_ROOT` serves
  anyone who does.
- **Q6. Shader debug information?** "Later" (13.5): it makes outputs configuration-dependent, a separate change.
- **Q7. Should the Metal step's converter be spirv-cross instead of MoltenVK's own?** No: MoltenVK's converter applies
  MoltenVK's own SPIRV-Cross options, the nearer bound (plan 3 prefers it); spirv-cross stays the fallback in the script.
- **Q8 (the orchestrator's). In the wave 9 fallback, do C2 and C2b land beside C0?** Recommended yes, if their own proofs
  held (C2: 103 of 103 modules byte-identical to 76f28a21's and `ci/slang-c2` green; C2b: 0 of 84 pictures differ): they
  give CI its first SPIR-V validation and the capability allow-list during wave 8, and make wave 9's step 0 smaller. Landed
  without C1, C2's call of `strata_find_slang` is left out, so nothing dead lands.
- **Q9 (the orchestrator's). Gate 4's quiet window** of about 35 minutes with no other lane building or using the GPU
  (section 12). Recommended: book it when C3 is committed; the gate waits rather than reading beside other work.

## 19. First hour

1. Check that S0.0 and S0.1 are on main and that `git diff 76f28a21 main -- Strata/shaders` is empty (else plan C3 on that
   state, section 16), and that `G:/GameEngineAI/.worktrees/wave8-plan/lookdev/wave8-base-84` exists (step0.md 4.4: C2b's
   comparison). Create the worktree: `sh .worktrees/create_worktree.sh wave8-slang main`; configure `cmake --preset
   windows` in it.
2. Download the Linux and macOS archives into a scratch folder (each in its own empty folder), compare `sha256sum` with
   2.2's table, list them (`tar tzf <archive> | head -60`): fill `STRATA_SLANG_FILES_*` and `STRATA_SLANG_ROOT_IN_ARCHIVE_*`
   for Linux and macOS with the run set: `bin/slangc` and the `lib/` libraries that are the counterparts of Windows' five
   files (not `slang-llvm`, not `slang-rt`). Do not run anything from them; CI's configure of C1 runs them. (Windows'
   five-file run set is already verified, 2.2.)
3. Read `spike/tools/glsl2slang.py`, `handfix.py`, `Prelude.slang`, `spvreflect.py`, `invariant.py`, `spike/converted.log`,
   and this spec's 7.2-7.3. Copy the spike's tools into a scratch folder as C3's starting material.
4. Note the tools this machine has: `spirv-val.exe` and `spirv-dis.exe` under
   `G:/GameEngineAI/build/_tools/Vulkan-ValidationLayers/external/Debug/64/SPIRV-Tools/build/install/bin/`, the layer at
   `...\build\_tools\Vulkan-ValidationLayers\build\layers\Release`, Python at `/c/msys64/mingw64/bin/python.exe`.
5. Start C0 (LookDev compare additions), then C1's `StrataSlang.cmake` with `StrataSlang.Fetch` first, and configure once
   to see the `-- Slang 2026.20: ... (downloaded)` line and the cache folder appear.
6. Ask the orchestrator now for gate 4's quiet window, so it is booked by the time the gates run.

## Review corrections (independent review, 2026-11-02)

Checked against 76f28a21's code (main checkout; `git show` where the object store reads), the plan, the rulings, the slang
audit and its spike, and the specs beside this one (step0, step0-platform, step0-editor, reflections, radiance-cascades,
physics-timing). Verified by the reviewer, read-only plus slangc runs into a private scratch folder: every file:line of
2.1-2.2 spot-checked (one wrong, below); the spike's capability, image-format, Invariant and NoContraction sets
re-scanned from `spike/out/`; the per-mode look-dev numbers of section 4 recomputed (they match); the five-file Windows
run set run alone; four entries recompiled byte-identically to the spike's validated modules; the converted tree scanned
for every token the new "HLSL syntax" case bans (none). What changed, and why:

1. **Worktree and branch names** (header, 17, 19): `.worktrees/wave8-slang`, `agent/wave8-slang`, `sh
   .worktrees/create_worktree.sh wave8-slang main`, as step0.md 10.1 names every lane; the spec had `.worktrees/slang` and
   `agent/slang`.
2. **AGENTS.md text cut from 743 to 440 code points** (13.1, 16, 18): step0.md 5.4 gives lane Slang "about 450" of S0.1's
   3,456 and keeps 2,800 for the streams. The `:50` edit is dropped (the third-party sentence and DG "Building" carry it);
   the bullet keeps every plan 4.1 rule in short, and the examples (dot product, componentwise `M * M`, `M[row][col]`, the
   signed compare) live in DG "Shaders" items 2 and 7. Counted with the test's rule.
3. **The lavapipe try is inside the box** (3.2, 3.3, 11.2): ruling D9 says "the Mesa job is tried"; the spec had put it
   below the cut, cut first. It stays information (CI wall time beside the gates), gives way only to an extension, and runs
   from two refs (`ci/slang-mesa-c2`, `ci/slang-mesa`) since C2 and the tip are different commits.
4. **The Metal step's baseline cannot fail C2** (6.5, 12 gate 6, 14, 18): with an empty `c_KnownFailures`, any module the
   offline conversion refuses whatever compiled it (the bindless runtime array is MoltenVK's run-time business) would make
   C2's push red. C2 now runs the step once with `--report-only`, records glslang's refusals in the list with reasons, and
   pushes green; gate 6 is Slang's run against that list, which only shrinks. Also: a Metal toolchain check with
   `xcodebuild -downloadComponent MetalToolchain` as the fallback for runner images that ship it separately.
5. **`StorageImageExtendedFormats`'s reason was wrong** (2.2, 6.4, 13.5, 18): `IBL/BRDFLut.comp` writes RG16F always
   (`SH/IBL/BRDFLut.comp:12`, `R/SceneRenderer.cpp:605-616`); only the other three users have fallbacks. The capability is
   now allowed per named entry, a new user needs a fallback, and BRDFLut's unchecked requirement is an open item.
6. **C2b compares against the right set** (7.1, 19): `lookdev/0.7.0` holds 1080p, Basic3D and live shots the 84-set lacks,
   so `compare` would exit 1 on one-sided pictures. C2b compares against `lookdev/wave8-base-84` (step0.md 4.4 step 5).
7. **The last act takes the 84-set** (16): step0.md 8.5 and 10.2 and plan S0.11 compare the 84-set; the spec said "the same
   set as 0.7.0's baseline" (every shot, both sizes, 5 repeats, live). Its record compare is against `wave8-base-84`. The
   merge of main re-runs 13.4's grep for `.glsl` text S0.1 and lane Render bring.
8. **C3 proves its modules, not only its tree** (2.2, 7.2, 7.7, 14): slangc is deterministic, so C3's modules are compared
   byte for byte with `spike/out/slang/` (what the GPU suites and captures validated), every difference explained by
   `spvreflect.py` and `spirv-dis`. Converter change 11 drops 14 empty preprocessor groups in 3 files (dead code) and extra
   blank lines; the module check proves it neutral.
9. **`Architecture.Shaders`'s port** (7.6, 10, 13.2, 13.3): the derivative-function set reads includes and fragments only
   (as today: `.glsl` and `.frag`), so compute and vertex files' `st_Main` cannot enter it; `clip(` counts as a discard (HLSL's
   conditional discard), with a crafted sample; DG `:4020-4025` and the test's header comment are rewritten in HLSL terms.
10. **`Renderer.ShaderModules`' Invariant case accepts both forms** (6.4): C2 runs it on glslang's modules, which decorate
    the `gl_PerVertex` member, not a variable; the spec said "carry Invariant on their Position" without saying how.
11. **The pin's details an agent would otherwise decide** (5.1, 5.5): the cache folder name
    (`slang-<version>-<host key>`, so worktrees on different pins never replace each other's compiler); host detection that
    works under `cmake -P` (`CMAKE_HOST_WIN32/APPLE/LINUX` and `cmake_host_system_information`; `CMAKE_HOST_SYSTEM_NAME` in
    script mode is unverified); `strata_slang_cache_directory`'s `SYSTEM` argument so every CI OS tests all three branches; a
    ninth expectation for the host key; `file:///G:/...` URLs on Windows; Linux and macOS extract only the libraries
    slangc loads.
12. **Gate 2 sees past the layer's duplicate limit** (2.2, 12, 18): the spike's log stopped reporting "not consumed by
    vertex shader" after 10, so it cannot show the shadow passes were the only source. One Debug run with the limit raised
    (setting name unverified, with how to find it) must show no warning A's run lacks.
13. **Gate 1 checks `precise`** (2.2, 12): `NoContraction` must survive in the three modules whose glslang form has it (the
    CPU/GPU LOD agreement); the reviewer's scan shows Slang keeps it.
14. **Gates 3 and 4 runnable as written** (12): `compare` requires `--output` and `capture` a `--label`; the four captures
    run from one script under one PerfLock; `CIStatus.py status <commit>`. Gate 4 adds a Medium 1080p A/B as information
    ("Medium keeps 0.6.0's costs"); the quiet window becomes about 35 minutes.
15. **C0 is tested on CI** (2.2, 4, 10, 12 gate 5): `StrataTools.LookDev` is labelled `gpu`, so CI never ran C0's new
    functions. `selftest` without `--build` runs the offline checks, and `StrataTools.LookDevOffline` (label `tools`) runs
    them on every job.
16. **The glslang working copy stays until 0.8.0 ships** (8.2, 13.5, 13.6, 18): `create_worktree.sh` points submodules at
    the main checkout's working copies (`:19-26`), so deleting `Strata/vendor/glslang` after the landing would stop
    offline worktrees at older commits (bisection, the fallback branch).
17. **Smaller facts**: root `include(StrataShaders)` is at `CMakeLists.txt:56`, not `:55`; 17 test files mention `.glsl`,
    not 22; the archive root also holds `cmake/` and `README.md`; `release.yml` needs no edit (its configure fetches, its
    ctest runs the new CTests); release-note lines go into `Docs/Plans/Wave8/ReleaseNotes.md` as lane Slang's block;
    `StrataShaderTool` is not engine code (no `stpch.h`, no layer); the fallback's open question on landing C2 and C2b (Q8)
    and gate 4's window (Q9) are the orchestrator's.

Checked and found sound, unchanged: the flag set (the reviewer's recompiles reproduce the spike's modules), the hand-fix
table against `spike/tools/handfix.py`, the cull-row reasoning of hand fix 12 (`R/SceneRendererGPUDriven.cpp:76-84`), the
input-layout plan of C2b (NVRHI numbers locations in order, `vulkan-shader.cpp:176-191`; `GetScenePipeline`'s `shadowPass`
covers the local shadow passes, `R/SceneRenderer.cpp:2366-2367`; no other site uses the mesh layouts), the box and
extension rule, the HLSL test, and the merge table with lane Platform.
