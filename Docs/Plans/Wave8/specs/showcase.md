# Workstream `showcase`: High Desert Outpost (wave 8)

The owner's words (2026-11-01, verbatim in `.worktrees/wave8-plan/owner_feedback.md`): "Make prioritization yourself, but
showcase needs to land this wave, you can update the showcase later if there are more things", and item 4: "Double check
specular lighting, or show the specular light bouncing off of metal sheets like the side of a bus or oil tank or wet
ground. (pretty important for the showcase)". The look stays wave 7's: realistic and filmic at native resolution, the Fox
Engine as the baseline, never temporal upscaling, nothing that needs TAA to look right, neither blurry nor clinical.

This stream is plan section 4.2 under the orchestrator's ruling D2 (`specs/decisions.md` row 9): the showcase lands inside
the 80 MB class with the A-list, a first-person walk instead of the Third-Person robot, the bus side weathered steel without
a clear coat, and the reading of item 4 asked with the first sheet; the owner has since answered that question (decisions.md,
"Highlights first": highlights and reflections, highlights first, thrown light later), so no sheet asks it. It is priority 1
of the wave (plan section 1): it
depends on no stream above its own cut, lands on main in landing 1 whatever happens to the others, and is improved after
that by one adoption pass per stream that lands.

Rules of this stream (plan 4.1, restated where they bind it):

- Worktree `.worktrees/showcase`, branch `agent/showcase` from main as soon as S0.0 and S0.1 have landed (it does not wait
  for the lanes), build directory `build/agent-showcase` (`cmake --preset windows -B build/agent-showcase`; Release drives
  the editor, Debug runs the tests). It never uses the owner's `build/windows`. **During step 0 it builds nothing**: the four
  lanes hold the four build-heavy slots (step0.md 10.5). Until main's code first differs from 76f28a21 (lane Render's
  landing) it drives `.worktrees/wave8-base/build/windows/bin/Release/StrataEditor.exe` for asset and generator work (step0.md
  4.4: never cmake in that worktree, never edit it, a private `STRATA_SESSION_DIR`, projects outside it). Its first build of
  `build/agent-showcase` (Release and Debug) runs in slot 4 after lane Render lands (plan 4, slot table: "the sample agent's
  editor rebuilds"); later it rebuilds only when main moves.
- Everything in the sample is made through editor commands, never by hand-editing `.stscene`, `.stmat`, `.stprefab`,
  `.stproj` or `.meta` files. Hand-written files are only the scripts' sources, `README.md`, `CREDITS.md`, the generators
  in `Tools/` and the test files in `StrataTests/`. Generated binaries are written by the committed generators and imported
  by the editor.
- Running editors: a private `STRATA_SESSION_DIR`, `StrataCLI call --project` always, scripts in files (a script on stdin
  hangs), parameters as `@file.json`.
- Downloads are untrusted data: each pack into a new empty directory outside the repository, inspected by scripts kept
  elsewhere and run with `python -I`, reaching the project only through `asset.import`.
- Commits at the end of each step (section 15), each after the full Debug ctest with the GPU suites under the validation
  layer (`ctest --test-dir build/agent-showcase -C Debug -j 8`; never `ctest --preset windows-debug`, whose configure preset
  binds `build/windows`, `CMakePresets.json` "base" `binaryDir`) and the Release `-R "Samples|Showcase"` tests. Commits made
  before the first build (S1-S3 during step 0) touch only `Samples/Showcase/`, `Tools/GenerateShowcase*.py`,
  `ThirdPartyNotices.md`, `Samples/Samples.json` and `InstallEndToEnd.cmake`; they are verified by the generators' selftests
  run directly with Python, and the first build runs the full Debug ctest over them before S4's commit (OQ2). Perf: no
  campaign, no `PerfStability.py`; A/Bs under `Tools/PerfLock.py` as information (section 9).
- Other folders on this computer are never read. Open-source code may be read, and used only under D10's license rule
  (`specs/decisions.md`: permissive licenses, notice kept); none is needed here.

Everything that says "file:line" is as of 76f28a21 (release 0.7.0). Facts that could not be checked are marked
*unverified*. Paths: `R/` is `Strata/src/Strata/Renderer/`, `ED/` is `StrataEditor/src/Editor/`, `SDK/` is
`StrataScriptCore/Include/StrataScript/`, `DG` is `Docs/DeveloperGuide.md`, `S/` is `Samples/Showcase/`, `A/` is
`.worktrees/wave8-plan/showcase/` (the audit's folder: `audit.md`, `captures/`, `scripts/`, `downloads/`, `proof_gen/`).

## 1. Goal, in priority order

1. **It lands in landing 1.** A complete sample, `Samples/Showcase` ("High Desert Outpost"), listed in `Samples.json`,
   within its budgets, with its regression, its windowed run and its perf metrics, on main at the end of tranche 1 even if
   no rendering stream has landed by then. Every dependency has a fallback that works on 0.7.0 (section 11).
2. **The owner's item 4, in three shots that are never cut.** Shot 2: the sun's highlight sliding along the side of a bus
   of painted and bare sheet steel with no clear coat. Shot 3: the vertical streak on a weathered steel oil tank at low sun,
   with the ground in its reflection. Shot 4: a wet apron seen from knee height into the sun, its glint column between the
   bus and the tank, both reflected in the puddles. The geometry of each is computed, not guessed (D5), and checked by
   numbers at the graybox (S4). **Highlights first** (`specs/decisions.md`, the owner's answer of 2026-11-01): shots 2-4,
   and shot 6's lantern glint on metal, are judged first on their highlights, then on their reflections, in motion, with no
   AA, FXAA and TAA; a highlight that shimmers fails (D15's shimmer row, D14's live shots).
3. **A place to look at, three ways.** A camera tour of about 80 seconds over six shots at three baked times of day, any
   input hands over to a first-person walk, Tab to free flight, P to a photo mode (time, exposure, field of view, focus,
   f-number, the Graphics options through `Graphics::Options()`, seven bookmarks). High by default.
4. **The reference scene for the presets' budgets.** `PerfGPU.Samples.ShowcaseHigh.*` and `ShowcaseUltra.*` at 3840x2160,
   `Provisional` with the plan's targets (8.3 ms and 14 ms), and `showcase.*` shots in the look-dev harness, so every later
   stream's adoption is judged on it.
5. **Reproducible.** Two committed standard-library generators (`Tools/GenerateShowcaseTerrain.py`,
   `Tools/GenerateShowcaseProps.py`) write the terrain, the props and the editor command scripts that place them; a CTest
   keeps their committed outputs equal to what they write.
6. **Improvable.** One adoption pass per stream that lands (section 11); the owner's first sheet at the graybox and the full
   sheet at landing 1.

**The reading of item 4** (plan 4.2; answered by the owner on 2026-11-01, `specs/decisions.md`): "I meant highlights and
reflections, highlights are extremely important just like in a drawing, because they should be cheap." Highlights and
reflections on the surfaces themselves: the sun's streak on a sheet, the curved streak on the tank, the glint column on the
apron, a lantern's glint on metal at dusk, then the world in the tank's side and the bus in a puddle. Nothing here throws sun
off a sheet onto nearby surfaces: the owner wants that later, "not a priority", and it goes to the roadmap's "Later" after
`radiance-cascades` lands (decisions.md). No sheet asks about the reading again.

Not the goal: grass (masked grass vanishes by 30 m at 4K, audit p16-p17; plan section 9), people or animals (none realistic
in the sources; the robot breaks the look, D2), sound, run-time probe capture, any engine change (E1-E6 are
`showcase-engine`'s or left out).

## 2. Where we start

### 2.1 Files

| File | Lines | Role, and what this stream does there |
| --- | --- | --- |
| `Samples/Samples.json` | 32 | The launcher's index (four entries, read by `ED/ProjectSamples.cpp:127-171`, which ignores unknown keys). **Adds the showcase entry first, with `"Budget": "Showcase"`.** |
| `StrataTests/src/Samples/SampleContent.h` / `.cpp` | 62 / 390 | `SampleBudgetClass` (`.h:19-23`), `SampleBudgets` (`.h:29-37`: 80 MB showcase sources, 85 MB showcase binaries, 3 MB and 3 screenshots for every sample), `CheckSampleContent` screenshots (`.cpp:296-303`) and limits (`.cpp:348-356`). **Adds the showcase's screenshot allowance (E3).** |
| `StrataTests/src/Samples/SampleContentTests.cpp` | 257 | `Samples.Content`; the showcase class case (`:123-153`). **One new case.** |
| `StrataTests/src/Perf/SamplePerfTests.cpp` | 114 | `PerfGPU.Samples`, the Open World's tour (`:23-25` metric names, `:56-113` the case). **Two showcase cases.** |
| `StrataTests/Perf/Budgets.json` | 250 | `Provisional` entries (`:200-202` the Open World tour's). **Five `Provisional` entries.** |
| `StrataTests/CMakeLists.txt` | 985 | Python and `StrataTools.LookDev` (`:487-496`), the sample CTests (`:787-956`), `StrataTests.PerfGPU.Samples` (`:905-913`). **The showcase's CTests; PerfGPU.Samples' fixtures and environment.** |
| `StrataTests/Editor/SampleProject.cmake` | 170 | Copies a sample, runs its command script in `--no-gpu` (`:110`), exports full and pruned, runs both headless. Read only. |
| `StrataTests/Editor/ShowcaseCommands.json.in` | new | **The regression (D13).** Owned. |
| `StrataTests/Editor/ShowcaseRenderCommands.json` | new | **The GPU render checks of the six shots (D14).** Owned. |
| `StrataTests/Editor/InstallEndToEnd.cmake` | 219 | The installed layout's sample projects (`:66-67`) and `project.samples`' order (`:132-133`). **Adds the showcase's project and shifts the order.** |
| `Tools/LookDev.py` | 1013 | Takes any `Samples/` project named by `Shots.json` (`prepare_project`, `:440-470`), reopens the scene per shot with `force` (`:739`) and runs its `Setup` steps (`:740-741`, `entityName` resolved at `:491-507`). **No change** (E4 needs only shots). |
| `Tools/LookDev/Shots.json` | 70 | Projects and shots, append only. **The `Showcase` project and `showcase.*` shots.** |
| `Tools/GenerateIslandTerrain.py` | 376 | The model of a committed generator (standard library, seeded, writes GLB and a command script). Read only. |
| `Tools/GenerateShowcaseTerrain.py`, `Tools/GenerateShowcaseProps.py` | new | **The generators (D6, D7).** Owned. |
| `ThirdPartyNotices.md` | 58 | "Sample assets" table (`:44-58`), which `Samples.Content` matches every CC0 source against (`SampleContent.cpp:327-346`). **One row.** |
| `ED/EditorLightingCommands.cpp` | 1054 | `lighting.bakeProbes` (`:871-948`: active probes by default, `entities` to choose, writes `Assets/Lighting/<scene>/<probe id>.hdr`, trashes the earlier capture of that probe once unused, `:365-388`), `lighting.applySkyPreset` (`:950-972`, body `:576-749`). Used, not changed. |
| `R/SkyPresets.cpp` | 100 | GoldenHour is the atmosphere at density 1.0, haze 1.5, a 120,000 lux sun (`:20-31`, `:56`); the direction toward the sun is `(cos e cos a, sin e, cos e sin a)` with the azimuth from +X toward +Z (`:93-98`). The scripts use the same formula. |
| `R/SceneRendererReflectionProbes.cpp` | 394 | An inactive probe is skipped (`:185`); a capture not yet resident with every level, or waiting for a conversion, is left out of the frame's list (`:288-297`), so its box falls to the sky ("the sky lights the box", `:290`) unless another active probe covers it; two conversions a frame (`R/SceneRenderer.h:925`). The stations keep the old probe active during the switch for exactly that reason (D10). |
| `R/RendererComponents.h` | - | `DirectionalLightComponent` (`:21-46`: `ShadowDistance` 80 by default), `PointLightComponent` (`:50-61`), `HeightFogComponent` (`:127-148`), `ReflectionProbeComponent` (`:150-166`: the two strongest overlapping boxes blend, higher `Priority` first, then the smaller box; the box is centered on the entity, which is also where the capture is taken). |
| `Strata/src/Strata/Physics/PhysicsComponents.h` | - | `MeshColliderComponent::Mesh` "uses the MeshRenderer's mesh when unset"; `CharacterControllerComponent::Enabled` (a disabled controller neither moves nor collides). The prefab script sets `Mesh` before it removes a hull's `MeshRenderer` (D9). |
| `Strata/src/Strata/Renderer/RenderLights.cpp` | - | A point or spot light at intensity 0 gets an undrawn record (`MakeRecord`, `:59-61`) and no shadow (`setShadows` returns for undrawn records, `:159-163`), so a station turns its lights off by intensity at no cost (D4). |
| `ED/ProbeBake.h` | 51 | Captures are 1024 x 512 from 256-texel faces (`:25-27`); 0.9-1.07 MB each in the Third-Person's `Assets/Lighting/Course/`. |
| `Strata/src/Strata/Asset/AssetDependencies.cpp` | 235 | Every asset property is a reference, active entity or not (`:36`): inactive stations' captures stay in pruned exports. |
| `SDK/Scene.h` | 232 | `Assets::RequestLoad` (`:36-42`), `Assets::GetLoadProgress` (`:53-61`: loads all of an asset and keeps it, negative for unknown handles). |
| `SDK/Graphics.h` | 238 | `Graphics::Options`, `FindOption`, `Set`, `SetPreset` (`:80-108`). |
| `SDK/Entity.h` | - | `SetActive` (`:51`), `GetScript<T>` (`:88`), `SetProperty` (`:72`). |
| `Samples/OpenWorld/Scripts/Explorer.cpp`, `DayNight.h` | 348, 73 | The models: Catmull-Rom tour (`Explorer.cpp:37-42`, `:257-307`), fades (`:249-255`), the body's yaw and the eye's pitch (`:196-201`); the sun clamped 2 degrees up and faded below (`DayNight.h:43-60`). Read only. |
| `Strata/src/Strata/Asset/GltfImporter.cpp` | 2273 | Defaults `LODCount` 3 (`:2034-2048`), supported extensions (`:49-52`, no `KHR_materials_specular`), MikkTSpace tangents when a mesh has none (`:1485-1492`). Read only. |
| `Strata/src/Strata/Asset/TextureImporter.cpp` | - | `nor` names a normal map, `arm` linear data (`:332-338`): Poly Haven's file names import right. |
| `Docs/DeveloperGuide.md` | 5427 | "Samples" (`:5389-5427`; "showcase scans 2K" at `:5427` changes), "Look-dev harness" (`:628`), "Perf tests" (`:428`). |
| `AGENTS.md` | 492 | The `Samples/` row (`:42`). **The row and one digest bullet.** |
| `README.md` | 172 | The samples (`:79-83`, `:131-134`, `:162`). **Five samples.** |
| `Docs/Roadmap.md` | 1487 | The wave 8 showcase entry (`:1425-1434`), clear coats get no SSR (`:877`). |
| `.claude/skills/strata-make-a-game/SKILL.md` | 514 | "Worked examples" (`:465-495`). **A showcase bullet.** |
| `CMake/StrataPackaging.cmake` | 107 | Installs all of `Samples/` (`:68`). No change. |
| `StrataEditor/CMakeLists.txt` | - | `StrataSamples` copies `Samples/` next to the editor on every build (`:98-103`). No change. |

### 2.2 What 0.7.0 already gives this sample (proved in the audit)

| Need | State | Evidence |
| --- | --- | --- |
| Sun glint on wet ground | Works with authored maps | `A/captures/p05_wet_into_sun.png`, `p18_wet_4k_high.png` |
| A highlight on a painted sheet | Works at roughness factor 0.45, not at 1 | `p08_container_mirror.png` against `p11_container_rough045.png` |
| A metal streak with the world in it | Needs a baked probe | `p04_tank_lowsun.png` against `p07_tank_probe.png` |
| One baked time of day | Works | `lighting.bakeProbes`, `p07` |
| Sky presets, aerial perspective, Filmic | Works | `lighting.applySkyPreset`, `R/SkyPresets.cpp` |
| Depth of field | Works through the scene's camera | `Scene/Components.h:142-150`, `GPU.DepthOfField` |
| The players' graphics options | Works | `SDK/Graphics.h`, the Third-Person's Graphics row |
| The showcase budget class | Landed, unused | `SampleContent.h:19-37` |
| Supersampled stills, blending, light shafts, irradiance volume, coverage-preserving alpha, distant shadows | Not landed | audit section 2 |

Defects the audit found that touch the shots: SSR's comb along a reflected straight edge (`p06_crop_ssr_edge.png`,
`p10_crop_ssr_edge_ultra.png`), the misplaced probe patch on wet ground where SSR misses (`p14_barrel_high_probe.png`), a
scan whose metal channel is zero (`p12_container_metal.png`). `reflections` R1 owns the first two; the third is authoring.

### 2.3 What it builds on, and when

| From | Used for | Needed by | If it is not on main by then |
| --- | --- | --- | --- |
| S0.0, S0.1 | The branch point | Step 1 | Branch from 76f28a21; nothing in this stream conflicts with them |
| S0.2 contact shadows off, S0.3 cascade fit | Every shot's shadows | The graybox exit capture (S4) | Capture on main as it is and again after lane Render lands; both are small |
| S0.7 `scene.raycast`, `asset.place` | Hand dressing | S6 | `prefab.instantiate` with the generators' heights, `entity.dropToGround`. **The graybox does not wait for S0.7**: the layout script places everything at heights computed from the height function and the apron (D6), so the plan's "graybox waits for S0.7" (plan 4, start conditions) is relaxed; the orchestrator confirms (OQ1) |
| `reflections` R1 | Tuning metals and water against the repaired SSR | S6 (dressing) | Steps 5 and 7 go first; if R1 is still out when they are done, dressing uses the fallback written from the graybox capture (S4, shot 4) |
| `physics-timing` | The walk on a character controller | S5 | The per-frame ground-following walk (D11), which needs no controller |
| Everything else | Adoption passes after landing 1 | Section 11 | The landing form |

### 2.4 Code paths and files each step changes

- **S1 assets:** `S/` created through `project.create`; `S/Assets/Scans/<id>/`, `S/Assets/Textures/<id>/` through
  `asset.import`; `S/CREDITS.md`, `S/README.md` (the state of that commit, rewritten in S7), `S/Picture.png`;
  `ThirdPartyNotices.md:58` (a row after it); `Samples/Samples.json` (entry first); `InstallEndToEnd.cmake:66-67,132-133`.
- **S2 terrain:** `Tools/GenerateShowcaseTerrain.py` (new), `S/Assets/Generated/Terrain/*` (generated, imported),
  `StrataTests/CMakeLists.txt:496` (after it: `StrataTools.ShowcaseTerrain`).
- **S3 props:** `Tools/GenerateShowcaseProps.py` (new), `S/Assets/Generated/Props/*`, `S/Assets/Materials/*.stmat`,
  `S/Assets/Prefabs/*.stprefab` (commands), `StrataTests/CMakeLists.txt` (`StrataTools.ShowcaseProps`).
- **S4 graybox:** `S/Assets/Scenes/Outpost.stscene`, `S/Assets/Lighting/Outpost/*.hdr` (commands), `S/Scripts/Stations.h`,
  `S/Scripts/Outpost.h`, `Outpost.cpp` (stations only), `StrataTests/Editor/ShowcaseCommands.json.in` (first form),
  `StrataTests/CMakeLists.txt` (`StrataEditor.Showcase` before `StrataTests.PerfGPU.Samples` at `:905`).
- **S5 scripts:** `S/Scripts/*` (D11, D12), `S/Assets/Scenes/Loading.stscene`, the photo mode's canvas (commands).
- **S6 dressing:** `S/Assets/Materials/*` (`material.set`), the scene (commands).
- **S7 wiring:** `SampleContent.h:29-37`, `SampleContent.cpp:296-303,353-356`, `SampleContentTests.cpp` (after `:153`),
  `SamplePerfTests.cpp` (after `:113`), `Budgets.json` (after `:202`), `StrataTests/CMakeLists.txt:905-913` and the new
  tests, `Tools/LookDev/Shots.json`, `StrataTests/Editor/ShowcaseRenderCommands.json`, `S/Screenshots/*`, `S/Assets/Icon.png`,
  docs (section 7).

## 3. Order, timing and the cut line

| Step | What | Size | Starts when | Ends with |
| --- | --- | --- | --- | --- |
| S1 | Freeze, fetch, verify, measure the import, credit | S | At once (S0.0, S0.1 landed) | Scans imported in `S/`, credits, notices, the import times |
| S2 | Terrain generator | M | S1 | `Terrain.glb`, `Apron.glb`, `Water.glb`, the checks green, the terrain in the scene |
| S3 | Props generator, materials, prefabs | M-L | S2 (may overlap) | Seven props as prefabs with materials and colliders |
| S4 | Graybox: layout, stations, bakes, six shots, checks, first perf reading, the owner's first sheet | M | S3 | **Exit capture on main with every check of D15 passed or its fallback written** |
| S5 | Scripts: tour, walk, flight, photo mode, loading screen | M | S4 | The regression's full form passes |
| S6 | Dress and tune materials | L | R1 on main and the editor rebuilt, or S5 and S7 done (whichever first) | The tuned look, shots retaken |
| S7 | Wiring: budgets, CTests, perf, LookDev shots, render checks, pictures, docs | M | S5 | Every test green; ready to land |
| S8 | Land (plan section 7.3), then adoption passes | S each | Landing 1 | Section 11 |

Steps run in order, except that S7 may run before S6 while R1 is not on main (plan: "Tuning against the old SSR and F0
would be done twice"). Each step lands on the branch working, tested and documented.

**Step numbers against the plan.** Plan 4.2's step 5 (dressing) is S6 here and its step 6 (scripts) is S5. Other specs that
say "showcase step 5" (`reflections.md`: "R1 lands alone ... before showcase step 5") mean dressing, S6.

**What runs on which editor.** S1-S3 run on `.worktrees/wave8-base`'s Release editor while step 0's lanes hold the slots
(Rules above); S1's Debug import time (step 3 there) and every test wait for the first build of `build/agent-showcase`, after
lane Render lands. The graybox's exit capture (S4) needs lane Render on main (S0.2, S0.3) in any case.

**Cut line.** Below it, cut first to last: (1) the B-list (it is out unless the sheet shows softness, D2); (2) bookmark 7,
the rock macro; (3) the walk (keep the tour and the flight); (4) Dusk's Hut probe (the Golden Hut probe then stays active
at dusk; shot 6 does not look into the hut). **Never cut:** shots 2, 3 and 4; the tour; the flight; the photo mode's time,
exposure and Graphics rows; the regression; the budgets; the perf cases.

## 4. Design

### D1 The frozen A-list

Source: Poly Haven, every asset CC0 1.0 (license page https://polyhaven.com/license). Sizes are the real file sizes from
`https://api.polyhaven.com/files/<id>` as the audit saved them (`A/downloads/polyhaven/files/<id>.json`), MB = 1,000,000
bytes, recomputed here by a scratch script over those files. Authors are from `A/downloads/polyhaven/{models,textures}.json`
(the API's `/info/<id>` gives the same). Textures are the three maps the engine reads: `diff` (sRGB base color), `nor_gl`
(OpenGL normal map), `arm` (AO in R, roughness in G, metalness in B: the glTF packing the engine reads, `Renderer/Material.h:41-44`).
Models are Poly Haven's glTF package at the given resolution: the `.gltf`, its `.bin` and its `textures/` JPEGs.

**One change from the plan's list, recommended (Q1):** `rusty_painted_metal` at 1K instead of 2K. Its source page describes it
as "rusty painted corrugated metal" and its normal map is corrugated, so it cannot be the bus side the plan had in mind; it
roofs the hut, seen from 8 m and more, where 1K at 2.2 m per tile is 2.1 mm per texel. The bus takes `rusty_metal_03`
("yellow painted metal with heavy brown rust... metal sheet", flat), whose 1K at 1.35 m per tile is 1.3 mm per texel, finer
than a 3840x2160 pixel at shot 2's 8.5 m (2.7 mm at 40 degrees). This saves 4.45 MB and adds nothing from the B-list.

| # | Asset (https://polyhaven.com/a/<id>) | Kind | Res. | Files | MB | Authors | Real size | Role |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `rocky_terrain_02` | Texture | 1K | diff 0.843, nor_gl 0.994, arm 0.525 | 2.36 | Amal Kumar | 90 m (aerial) | The outer ring |
| 2 | `dry_ground_01` | Texture | 2K | 2.542, 1.081, 0.792 | 4.42 | Rob Tuytel | 4 m | The valley floor, slopes |
| 3 | `asphalt_02` | Texture | 2K | 3.076, 4.944, 1.299 | 9.32 | Rob Tuytel | 3 m | The apron (hero: shot 4) |
| 4 | `brown_mud_03` | Texture | 1K | 0.526, 1.229, 0.278 | 2.03 | Rob Tuytel | 1.3 m | Mud at the apron's edges |
| 5 | `beige_wall_001` | Texture | 2K | 0.236, 0.816, 0.438 | 1.49 | Dimitrios Savva, Rico Cilliers | 3 m | The hut's plaster (shot 5), the tank pad |
| 6 | `weathered_brown_planks` | Texture | 1K | 0.226, 0.223, 0.204 | 0.65 | Dimitrios Savva, Rico Cilliers | 1.8 m | Floor, door, shutters, rafters, the bus's boards |
| 7 | `rusty_painted_metal` | Texture | **1K** | 0.472, 0.484, 0.373 | **1.33** | Amal Kumar | 2.2 m | The hut's corrugated roof |
| 8 | `green_metal_rust` | Texture | 2K | 0.787, 0.357, 0.478 | 1.62 | Rob Tuytel | 1.0 m | The horizontal fuel tank |
| 9 | `rusty_corrugated_iron` | Texture | 1K | 0.620, 0.631, 0.428 | 1.68 | Charlotte Baglioni | 2.0 m | The lean-to's roof |
| 10 | `rusty_metal_03` | Texture | 1K | 0.781, 0.582, 0.555 | 1.92 | Amal Kumar | 1.35 m | The bus's paint (shot 2); its normal on bare steel |
| 11 | `namaqualand_boulder_02` | Model | 1K | gltf 0.003, bin 2.298, 3 JPEGs 2.839 | 5.14 | Greg Zaal, Rico Cilliers | 2.5 x 1.2 x 0.9 m, 184k tris | Boulders; the rock macro |
| 12 | `namaqualand_boulder_03` | Model | 1K | 0.003, 1.450, 2.326 | 3.78 | Jenelle van Heerden, Dario Barresi | 2.4 x 3.1 x 1.5 m, 122k | Boulders; shot 1's hero boulder |
| 13 | `namaqualand_cliff_01` | Model | 1K | 0.003, 2.180, 2.362 | 4.54 | Jenelle van Heerden, Rico Cilliers | 8.3 x 5.0 x 4.4 m, 177k | The cliff band, scaled 1.8-3 |
| 14 | `dry_branches_medium_01` | Model | 1K | 0.006, 0.465, 2.781 | 3.25 | Rico Cilliers | 1.1 x 0.3 x 1.3 m, 17k | Dead brush (opaque) |
| 15 | `barrel_03` | Model | 1K | 0.003, 0.045, 0.527 | 0.57 | Serhii Khromov | 0.63 x 0.93 m | Oil drums |
| 16 | `Barrel_01` | Model | 1K | 0.003, 0.082, 0.608 | 0.69 | Jorge Camacho | 0.56 x 0.88 m | Drums |
| 17 | `Barrel_02` | Model | 1K | 0.003, 0.077, 0.341 | 0.42 | Jorge Camacho | 0.49 x 0.88 m | Drums by the hut |
| 18 | `stone_fire_pit` | Model | 1K | 0.003, 0.096, 2.437 | 2.54 | Sebastian Platen | 1.45 x 0.39 m | The fire (shot 6) |
| 19 | `Lantern_01` | Model | 1K | 0.005, 0.865, 0.551 | 1.42 | Rajil Jose Macatangay | 0.29 m, 34k | Two lanterns (shot 6) |
| 20 | `old_tyre` | Model | 1K | 0.003, 0.084, 2.150 | 2.24 | MP | 0.60 m | The bus's wheels (scale 1.65), a stack |
| 21 | `covered_car` | Model | 1K | 0.010, 0.394, 1.945 | 2.35 | MP | 4.4 x 1.8 x 1.4 m | Under the lean-to |
| 22 | `ladder_sectioned_01` | Model | 1K | 0.008, 0.914, 1.650 | 2.57 | MP | 2.1 m, 29k | Against the oil tank (shot 3) |
| | **Scans** | | | | **56.33** | | | |

Cut from the plan's list as the audit did (section 4, "Dropped"); the B-list (audit section 4, 48.9 MB) stays out (D2).

**Fetch (S1).** A scratch script outside the repository (not committed: Q2) reads the list above, downloads each file from
the URL the API's `/files/<id>` names (`https://dl.polyhaven.org/file/ph-assets/...`) into a new, empty directory with a
`User-Agent` naming Strata, checks every file's MD5 against the API's `md5`, and writes a `manifest.json` (id, resolution,
file, bytes, md5, authors). A mismatch stops it. Nothing is renamed.

**Import (S1).** Textures: `asset.import {"file": "<dl>/<id>/<file>", "directory": "Textures/<id>"}` for the three maps.
Models: `asset.import {"file": "<dl>/<id>/<id>_1k.gltf", "directory": "Scans/<id>"}` (brings the `.bin` and the images it
names). Import settings: the three Namaqualand rocks `LODCount` 6 (`asset.setImportSettings {"asset", "settings":
{"LODCount": 6}}`; the default 3 leaves a 180k scan near 23k triangles, audit section 5), `Lantern_01` and
`ladder_sectioned_01` 5, the rest the default. After the import, `material.get` on every scan's material answers
`"warnings": []`; a texture imported for another use is fixed with the warning's `fix`.

**Verify (S1), the metal channels.** For each metal texture set (rows 7-10) make a 1 m quad with its material twice, Metallic
factor 1 and 0, and capture both at 512 x 512 under GoldenHour (p11 against p12's method). Identical pictures mean the
`arm`'s B is zero: that set is paint, and its material keeps Metallic 0 (painted surfaces are dielectric; their highlight
comes from the roughness factor). Record the result per set in the S1 commit message. The design below assumes all four are
paint (the safe reading of `p12`, and the likely one: none of the four publishes a metalness map in the API's file list, whose
keys are `Diffuse`, `nor_gl`, `nor_dx`, `arm`, `AO`, `Rough`, `Displacement` and the like, `A/downloads/polyhaven/files/<id>.json`);
bare metal comes from the `WeatheredSteel` material (D8), which never relies on a scan's B.

**Credit (S1).**

- `S/CREDITS.md`: the table `| Files | Source | Author | License | Attribution | Changes |` (`SampleContent.cpp:28`), one row
  per directory: `Assets/Scans/<id>/` with Source `https://polyhaven.com/a/<id> (1K glTF)` and Changes "none: the glTF with
  its .bin and textures as downloaded"; `Assets/Textures/<id>/` with Source `https://polyhaven.com/a/<id> (<res> JPG)` and
  Changes "3 of the set's maps (diff, nor_gl, arm)"; License `CC0-1.0`; Author from the manifest. Then `Strata` rows:
  `Assets/Generated/Terrain/` ("`Tools/GenerateShowcaseTerrain.py` (terrain)", none: the generator's output as it writes it),
  `Assets/Generated/Props/` (`Tools/GenerateShowcaseProps.py`), `Assets/Lighting/Outpost/` ("Baked in the editor from
  `Scenes/Outpost.stscene` (`lighting.bakeProbes`), six probes at three times of day"), `Assets/Icon.png` ("A render of the
  sample: a 256 x 256 `viewport.capture` of shot 2"). Rows for files that S2-S7 add are added in the commit that adds them.
- `ThirdPartyNotices.md`, after `:58`: `| High Desert Outpost | Poly Haven: 22 scans (<the ids, comma separated>) |
  <the 22 page URLs, comma separated> | <the 13 authors> | CC0 1.0 | |`. `Samples.Content` finds each row's first URL in the
  section (`SampleContent.cpp:341-345`).
- The thirteen authors: Amal Kumar, Rob Tuytel, Dimitrios Savva, Rico Cilliers, Charlotte Baglioni, Greg Zaal, Jenelle van
  Heerden, Dario Barresi, Serhii Khromov, Jorge Camacho, Sebastian Platen, Rajil Jose Macatangay, MP.

### D2 The budget

| Item | MB | Source |
| --- | --- | --- |
| Scans (D1) | 56.33 | Measured (API sizes) |
| `Generated/Terrain/` (`Terrain.glb` 3.7, `Apron.glb` 0.7, `Water.glb` 0.05, `Mud.glb` 0.1) | 4.55 | Estimate: 154k vertices at 24 bytes (D6); the audit's 3.0 assumed coarser tiles |
| `Generated/Props/` (six GLBs 0.6, `WeatheredSteel_arm.png` about 1.3, `WeatheredSteel_diff.png` about 2.0) | 3.9 | Estimate (D7: 1024 x 1024 smooth noise, PNG filter Sub) |
| `Lighting/Outpost/` (six captures) | 5.70 | Estimate: 0.95 each (the Third-Person's are 0.91-1.07) |
| `Icon.png` | 0.15 | The other samples' 0.03-0.13 |
| **Source assets** (`SampleBudgets::ShowcaseSourceAssetBytes`, 80 MB) | **70.6** | 9.4 MB margin |
| `Picture.png` | 0.25 | The other samples' 0.19-0.25 |
| `Screenshots/` (six at 1280 x 720) | at most 8.0 | E3 below |
| **Binaries of the showcases** (`ShowcaseTotalBinaryBytes`, 85 MB) | **at most 78.9** | |
| `showcase-engine`'s masks, if it lands (adoption) | +2.0 | Audit section 4 |

Measured sizes replace the estimates in the S2, S3, S4 and S7 commit messages. If a generated file exceeds its estimate by
more than 50%, the generator's resolution is cut before anything else (Q5; the two steel maps go to 768 x 768 covering 3 m).
With `showcase-engine`'s masks (+2.0 MB) the sources stay at about 72.6 MB.

**E3, the screenshot allowance.** `SampleBudgets` gains `ShowcaseScreenshotBytes = 8'000'000` and `ShowcaseMaxScreenshots = 6`;
`CheckSampleContent` reads them for `SampleBudgetClass::Showcase` instead of `ScreenshotBytes` and `MaxScreenshots`
(`SampleContent.cpp:353-356`). Six shots need six pictures; 1280 x 720 PNGs of scanned detail are about 1.0-1.4 MB each
(the audit's 1080p proofs were 3.0-3.4 MB, the samples' 960 x 540 and 1280 x 720 ones 0.65-0.84 MB).

### D3 The place and its layout

A derelict fuel stop in a rocky high-desert valley the day after rain: a bus with flat tyres on a cracked, wet asphalt
apron; a weathered vertical oil tank on a concrete pad; a green horizontal fuel tank on saddles; a plaster-and-plank hut with
a corrugated roof and a lean-to over a covered car; drums, tyres, a fire pit and two lanterns; dead brush; boulders and a cliff
band behind; the valley opening toward the golden-hour sun. An original place (audit section 5), made through the editor.

**Frame.** Meters, +Y up, the yard's ground at y = 0, azimuths in degrees from +X toward +Z (as `lighting.applySkyPreset`'s
`sunAzimuth`, `R/SkyPresets.cpp:93-98`). The valley's axes: `u = (x + z) / sqrt(2)` points down the valley toward the golden
sun (azimuth 45), `v = (z - x) / sqrt(2)` across it (+v is azimuth 135, the dawn side).

| Element | Position (x, y, z) | Yaw | Size | Notes |
| --- | --- | --- | --- | --- |
| Apron (asphalt) | x -14..26, z -16..24 | | corner radius 6, outline noise +-0.8 m | Its own mesh (D6), 4 cm above the ground at its +X edge and 12 cm at its -X edge (drains toward +X); its outline plus 1 m is a pad of the terrain. Props on it stand on its top |
| Pothole field | wedge from (-10, 0, -10) at azimuth 18-72, 3-22 m | | 14 potholes r 0.7-2.6 m, 5-12 cm deep; 3 shallow puddles r 2-3.5 m on azimuth 45 at 5-14 m | Water pieces in each (D6) |
| Bus | (11.8, 0, 0.1) | 0 (front toward +Z) | 10.8 x 2.44 x 2.95 m | Hero side +X (normal azimuth 0): the golden sun rakes it at 45 degrees |
| Oil tank (vertical) | (2.7, 0, 17.2) | 0 | r 4.0, shell 7.2 m, cone roof +0.33 | Pad r 4.35, 0.3 m |
| Fuel tank (horizontal) | (-10.0, 0, -20.0) | -45 (axis along azimuth 135) | r 1.3, 6.4 m + heads, axis 1.75 m up | Off the apron, on dirt |
| Hut | (-22.0, 0, -18.0) | 45 (door wall faces azimuth 45) | outside 5.2 x 4.4 m, 2.6 / 2.3 m high | Door center (-21.01, -15.88), window center (-19.60, -17.29) |
| Lean-to, covered car | hut-local (3.9, 0, 0): (-19.24, 0, -20.76) | 45 | 5.2 x 3.0 m | `covered_car` under it, yaw 45 |
| Fire pit | (-15.0, 0, -14.0) | 0 | | 6.3 m from the door, 3.3 m from the fuel tank's head |
| Lantern A | (-20.76, 2.0, -15.63) | | | Hung 0.35 m out from the door's lintel |
| Lantern B | on a drum at (9.6, 0.88, 7.4) | | | No shadows |
| Ladder | leaning 15 degrees on the oil tank at tank azimuth 30 (shell point (6.16, 19.20)): foot (6.64, 0, 19.48) | | 2.1 m | Its shadow falls near shot 3's streak |
| Drums | three clusters: oil tank pad (-2, 20), 5 drums; hut (-17.5, -21.5), 3; the bus's front left (9.5, 7.5), 3 (not at its front right: shot 3's camera stands there) | seeded | | |
| Tyres | 6 on the bus (scale 1.65, flat: scale Y 0.85 on the bottom pair), a stack of 4 at (9.5, 0, -7.0), 3 scattered | | | |
| Cliff band | u -60..-36 across v -80..60 | seeded | 8 `namaqualand_cliff_01` at scale 1.8-3.0 | Embedded in the rim's face |
| Boulders | talus at the cliff foot, the valley walls; hero boulder at shot 1 | seeded | ~25 `_boulder_02`, ~20 `_boulder_03` | |
| Brush | 60 seeded on dirt, denser at the apron's edge and slope feet | seeded | `dry_branches_medium_01`, scale 0.8-1.4 | |

**Design rules, checked by the terrain generator (D6 `check`):**

1. From every apron point (a 2 m grid inside the outline, at 0.3 m above the ground), the terrain's highest elevation angle
   toward azimuths 30-60 stays at or below 4.0 degrees (the golden sun is 6; audit section 5's "below 5"), so shots 2-4 keep
   their sun.
2. From shot 1's camera, toward azimuths 135-145, at or below 3.5 degrees (the dawn sun is 5): the rim is lit at dawn.
3. Every pad (the apron's outline grown by 1 m, bus, both tanks, hut, lean-to, fire pit, the drum clusters' centers) is
   flat: under its footprint the terrain varies by at most 3 cm and slopes by at most 2 degrees.
4. The inner terrain and the ring meet within 2 cm along their whole boundary.
5. Within the walk's 140 m radius, slopes above 45 degrees only inside the cliff band (reported, not fatal).
6. The apron's top stays at least 2 cm above the terrain at every vertex of its outline and of its skirt's top edge, and its
   skirt reaches at least 3 cm below the terrain there (so the terrain never covers the asphalt's edge and no gap shows under
   it). The first draft's formula, `0.04 + 0.004 * (x - 6)`, is -4 cm at the -X edge and fails this rule.
7. *As built (the reflections' adoption):* the dusk sun reaches shot 6's subjects. Seen from the fire, lantern A, the fuel
   tank and the hut's door wall, the sun's azimuth stands at least 0.75 degrees beside the bus's footprint, and the light's
   drawn elevation at least 0.15 degrees above the terrain's horizon (`dusk_sun_reach`). A probe holds no sun since the
   adoption (a bake draws neither its disk nor the highlights it makes), so what the dusk sun does not reach has the sky's
   light and the fire's alone: at D4's azimuth 32 the bus stood before the sun (from the fire its footprint spans azimuths
   17.3 to 37.3) and its shadow lay over the hut's front, the fire's ground and the fuel tank's near end, which came out
   near black. On the sun's course (its azimuth falls through the day: 140, 92, 45) the rule passes at 16 alone among
   whole degrees: 1.00 degree beside the bus from lantern A, 0.21 above the terrain from the fire; at 17 the bus is 0.004
   degrees beside the sun from lantern A, at 15 the sun 0.11 degrees above the terrain from the fire. It passes again
   from 52 to 61, on the bus's other side, where the sun would turn back after Golden's 45.

The layout's heights (`layout`, `stations`) come from the surface a point stands on: the apron's top inside its outline (the
bus, the oil tank's pad, the drums and tyres on the apron, Lantern B's drum), else the terrain. D5's numbers below are
computed by `stations` from those heights; the table's values are the generator's first output and its output wins.

### D4 The stations (three baked times of day)

A station is the sun's direction and the values that go with it. The scene is saved at Golden.

| Station | Photo hour | Sun elevation | Light drawn at | Azimuth | Light scale | Fog density | Exposure | Shadow distance | Local lights |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Dawn (0) | 6.5 | 5.0 | 5.0 | 140 | 1.0 | 0.006 | 0.0 | 600 m | off |
| Golden (1) | 17.75 | 6.0 | 6.0 | 45 | 1.0 | 0.0006 | 0.0 | 200 m | off |
| Dusk (2) | 19.0 | 1.0 | 2.0 | 32 | 0.5 | 0.0015 | -0.7 | 150 m | on |

- **Toward the sun:** `TowardSun(e, a) = (cos e cos a, sin e, cos e sin a)`, `R/SkyPresets.cpp:93-98`; Dawn
  (-0.763, 0.087, 0.640), Golden (0.703, 0.105, 0.703), Dusk drawn (0.848, 0.035, 0.530).
  *As built (the reflections' adoption):* Dusk's azimuth is 16, not 32, its drawn direction (0.961, 0.035, 0.275): D3's
  rule 7 keeps the last sun beside the bus, whose shadow now lies over the asphalt before the camera and the slope's foot
  left of the hut, clear of the fire, the lantern, the hut's front and the fuel tank. The photo mode's day turns the sun
  through 29 degrees between Golden and Dusk (13 before).
- **The light below 2 degrees** follows `DayNight.h:43-60`: it is drawn at `max(e, 2)` (shadows stay drawable) and its
  intensity is `120000 * clamp(sin(e) / 0.035, 0, 1)`; at Dusk, `sin(1) / 0.035 = 0.4986`, the light scale 0.5.
- **The sun's rotation** in scripts: `glm::quatLookAt(-TowardSun(drawnElevation, a), glm::vec3(0, 1, 0))` (lights shine along
  -Z; quatLookAt points -Z along its direction, as the make-a-game skill's `FollowCamera` uses). In edit mode the same
  direction comes from `lighting.applySkyPreset {"preset": "GoldenHour", "sunElevation": <drawn>, "sunAzimuth": <a>}`
  (`EditorLightingCommands.cpp:638`), then `component.set` of the light's `Intensity` (`120000 * scale`) and `ShadowDistance`.
- **What else a station sets:** `HeightFog.Density` (the fog's other fields fixed: `BaseHeight` -8, `HeightFalloff` 0.10,
  `StartDistance` 80, `MaxOpacity` 0.7, `ColorFromSky` true, `SunScattering` 0.6; `StartDistance` keeps every shot that looks
  into the hut fog-free, plan 4.2's volumetric fallback); the Post Process's `Exposure` (compensation, EV); local lights: the
  lights `Fire`, `LanternA.Light` and `LanternB.Light` (D9's names) through their `Flicker.Intensity`: 250, 40 and 25 cd at
  Dusk, 0 otherwise (a light at 0 cd is neither drawn nor shadowed, `RenderLights.cpp:59-61`, `:159-163`), the emissive
  `FirePit.Embers`, `LanternA.Wick` and `LanternB.Wick` active only at Dusk; the six probes (D10).
- **Proposed values:** fog densities, Dusk's exposure and the local lights' intensities are first values for the graybox,
  tuned by look in S6; the elevations and azimuths are fixed by the geometry (D5) and change only with it.
- **The photo mode's day** (`Stations.h`, `SampleDay(hour)`): piecewise linear in hour between knots Dawn (6.5 h), Noon (12.0
  h: elevation 55, azimuth 92, light scale 1, fog 0.0006, exposure 0), Golden (17.75 h), Dusk (19.0 h); fog density
  interpolates in log; the slider spans 6.5-19.0 h until `reflections` R2 fixes the night sky's bands (then 19.5 h, below 0
  degrees: the light fades to 0, adoption pass). The probes snap to the nearest station by hour: Dawn below 12.125 h, Golden
  below 18.375 h, Dusk above.

### D5 The shots and their computed cameras

The mirror condition: a camera at C sees the sun's reflection at a surface point P with normal n when the reflected view
direction `R(d) = d - 2 (d . n) n`, with `d = normalize(P - C)`, equals `TowardSun`. For a vertical sheet, the vertical
component of d is kept, so the highlight centers at `P.y = C.y + t * tan(e)` for a horizontal distance t (audit section 5).

| # | Shot | Station | Key camera (position) | Look at | Vertical FOV | Proves | Computed check |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Dawn vista from the rim, hero boulder in front | Dawn | (-29.7, rim + 1.7, -58.0); rim at u -62, about 24.4 m | (40, 0, 40) | 50 | Aerial perspective, long side-lit shadows (the sun 91 degrees to the right of the view), the ridge's shadow across the valley | Rule 2 of D3 |
| 2 | Side of the bus | Golden | (19.03, 1.60, -5.91) | P = (13.02, 2.49, 0.10) | 40 | The highlight on the letterboard (the upper side panel), sliding along the bus as the camera dollies across bare and painted sections (D7) | d = (-0.703, 0.105, 0.703) (azimuth 135); t = 8.5 m; P.y = 1.60 + 8.5 tan 6 = 2.49, on the letterboard between 2.25 and 2.62 m, which is **vertical** (D7: with a 2 degree tumblehome the view ray that mirrors the sun drops from 6 to 3.2 degrees, putting the highlight at about 2.07 m, in the boarded window band); the bus's front at image x 0.26 |
| 3 | Oil tank at low sun | Golden | (14.96, 1.70, 6.92) (16 m from the tank at its azimuth -40) | (2.70, 3.60, 17.20) | 45 | The vertical streak, weld seams, the ladder's shadow, the ground in the reflection | Streak normal azimuth 2.5 (the bisector of the sun and the camera); its point (6.70, y, 17.37), 13.3 m away, centered at 3.10 m; image x 0.36, from y 0.17 to 0.78 |
| 4 | Wet apron into the sun | Golden | (-10.0, 0.75, -10.0) | (12.98, 2.0, 9.28) | 55 | The glint column; the bus (azimuth 11.5-37, 24 m) and the tank (65 +- 7.7, 30 m) reflected | The sun at image (0.55, 0.44), the glint's mirror point 7.1 m out at (-4.95, 0, -4.95), image (0.55, 0.64); bus base x 0.37, tank base x 0.80; puddles reflect the bus from 4.9 m and the tank from 2.4 m out |
| 5 | Hut interior from the doorway | Golden | (-20.66, 1.50, -15.53) (0.5 m out of the door) | (-24.12, 0.90, -18.99) | 70 | The door's and window's sun patches on the back wall and floor, a dark interior | The door faces the sun; the sun's line from the door passes the tank 6.6 m and the bus 11.9 m off (unblocked) |
| 6 | Dusk | Dusk | (-2.0, 1.6, -2.0) | (-16.0, 1.2, -16.0) | 50 | Shadowed lantern and fire, the fuel tank catching firelight, the dusk sky behind the camera, exposure | Fire at image (0.48, 0.55), lantern A (0.40, 0.46), fuel tank (0.73, 0.47) |
| 7 | Rock macro (photo-mode bookmark) | Golden | 0.45 m from the hero boulder's sunward face | its surface | 30, f/2.8, depth of field on | Scan detail | |

Each tour shot dollies through its key: markers `Shot<N>.From`, `Shot<N>.Key`, `Shot<N>.To` and `Shot<N>.Look` (empty
entities). From and To lie 1.5 m on either side of the key along the shot's dolly direction (shot 2: along +Z, so the
highlight slides along the bus; shot 3: around the tank at constant distance, 6 degrees each way; shot 4: 1.0 m along azimuth
45; shot 5: 0.4 m inward; shot 6: 2 m sideways; shot 1: 3 m along the rim). The generator's `stations` subcommand computes
every key, From, To, look target and the image positions above from the layout, prints them and writes them into the layout
script; its numbers win over this table if the layout moves. It solves the mirror condition with the normals of the
generated meshes (`Bus.Body`'s letterboard faces, `OilTank.Shell`, the puddles), never with an assumed vertical sheet, so a
change of the props keeps the check honest. Over shot 2's dolly (camera z -7.41 to -4.41) the mirror point runs along the
letterboard from z -1.40 to 1.60.

### D6 `Tools/GenerateShowcaseTerrain.py`

Standard library only (`argparse`, `json`, `math`, `random`, `struct`, `zlib`), modeled on `GenerateIslandTerrain.py`.

```
python -I Tools/GenerateShowcaseTerrain.py terrain  --output Samples/Showcase/Assets/Generated/Terrain
python -I Tools/GenerateShowcaseTerrain.py check                      # the design rules of D3; exit 1 on a failure
python -I Tools/GenerateShowcaseTerrain.py stations [--json <file>]   # D4, D5: stations, shot cameras, image positions
python -I Tools/GenerateShowcaseTerrain.py layout   --output <Layout.json>        # the scene's command script
python -I Tools/GenerateShowcaseTerrain.py setup    --station Dawn|Golden|Dusk --format lookdev|script
python -I Tools/GenerateShowcaseTerrain.py checkshots --capture <LookDev output dir>
python -I Tools/GenerateShowcaseTerrain.py selftest --committed Samples/Showcase/Assets/Generated/Terrain --stations Samples/Showcase/Scripts/Stations.h --work <dir>
```

Exit codes: 0 done, 1 a check failed, 2 usage.

**Determinism.** `SEED = 20261101`. The height function uses only `+ - * /`, `floor`, `sqrt` and integer hashing (value noise
with smoothstep polynomials, as `GenerateIslandTerrain.py:61-100`), never `sin`, `cos`, `exp` or `atan`, so it is bit-exact
on every platform's Python. Every vertex attribute is quantized before it is packed: positions to 1/4096 m, normals to 8-bit
SNORM, texture coordinates to 1/65536. JSON with `separators=(",", ":")` and sorted nothing else. So two runs, and runs on
Windows, Linux and macOS, write the same bytes (practically: a value would have to land within an ulp of a rounding boundary).

**The height function** (meters; `S(a, b, x)` is smoothstep from a to b, reversed when a > b):

```
floor(u)    = -0.02 * max(u - 30, 0)                         # flat yard, then 2% down the valley
rim(u)      = 26 * S(-28, -68, u)                            # the rim behind the yard (the cliff band is its face)
right(v)    = 18 * S(55, 110, v)                             # the dawn side's wall
left(v)     = 12 * S(-70, -130, v)
far(u)      = 45 * S(700, 1150, u)                           # hills that close the valley's end on the skyline
mountains   = max(140 * S(300, 1100, -u), 70 * S(260, 900, |v|))
detail      = 1.6 * (fbm(x / 40, z / 40) - 0.5) * W(x, z)    # 4 octaves; W 0 on the pads + 8 m, 1 beyond 30 m,
                                                             # faded to 0 from 120 to 145 m (max(|x|, |z|)) for the seam
ringdetail  = 8 * (fbm(x / 300, z / 300) - 0.5) * S(170, 260, max(|x|, |z|))
h(x, z)     = floor(u) + max(rim, right, left, far, mountains) + detail + ringdetail
pads        : h = 0 inside each pad polygon (the apron's outline grown by 1 m is one), blended over 6 m to the outside
              (smoothstep of the distance)
```

The walls combine by `max`, not a sum, so the rim seen from its own plateau does not gain the dawn wall's height (a sum puts
the rim camera in shadow at dawn: 7.8 degrees at 130 m). The constants are the S2 starting point; `check` binds them.

**Outputs.**

- `Terrain.glb`: one glTF 2.0 model. Inner terrain: 36 tiles of 50 m (6 x 6 over -150..150) at 1 m spacing (51 x 51
  vertices, one shared 16-bit index buffer as the island's), nodes `Ground_<i>_<j>` (i, j 0-5), triangles fully inside the
  apron's outline shrunk by 1 m left out (the apron covers them), a 1.5 m skirt down the outer edges of the 20 edge tiles.
  The ring: 63 tiles of 300 m over -1200..1200 at 10 m spacing (31 x 31), nodes `Ring_<k>` (k 0-62), heights from the same
  function. Attributes: POSITION float32, NORMAL 8-bit (KHR_mesh_quantization, as `GenerateIslandTerrain.py:241-248`),
  TEXCOORD_0 float32 in meters (u = x, v = z); no TANGENT (the importer computes MikkTSpace tangents,
  `GltfImporter.cpp:1485-1492`). One glTF material per node group (`Ground`, `Ring`) with factors only, overridden by `.stmat`.
  About 154k vertices, 3.7 MB.
- `Apron.glb`: nodes `Apron` (0.5 m grid) and `ApronFine` (0.25 m grid over the pothole wedge's bounding box, its boundary on
  the coarse grid's vertices and midpoints and kept flat, so the T-junctions do not crack), top at `0.04 + 0.002 * (26 - x)`
  (drainage toward +X: 4 cm above the ground at x 26, 12 cm at x -14; the terrain under the outline grown by 1 m is a pad
  at 0), potholes as smooth bowls (depth 5-12 cm below the local top, smoothstep profile), a 6 cm bevel and a skirt from the
  outline down to 5 cm below the terrain (rule 6 of D3). About 0.7 MB. Terrain tiles that lose triangles under the apron
  carry their own index buffers; the others share one.
- `Water.glb`: one flat node `Puddle_<n>` per pothole and shallow puddle at its water level (the lowest point of its rim minus
  1.5 cm), a 32-gon disc 10% wider than the pothole: the apron above the water hides the rest, and the intersection is the
  waterline (audit section 5). Opaque (SSR traces no transparent surface, `Roadmap.md:877`).
- `Mud.glb`: 8 patches `Mud_<n>` (r 2-5 m, noisy outlines) along the apron's outline and under the bus's front, draped 1.5
  cm above whatever surface is under them (the terrain, or the apron's top) along its normal.

**`check`** evaluates D3's rules (seven with the adoption's rule 7) on a sampled grid (1 m inner, 10 m ring, bilinear
between), prints each rule's worst value and where, and exits 1 if one fails. `terrain` runs it first and writes nothing on
a failure.

**`stations`** prints the D4 table and, per shot, the key, From, To, look, FOV and the image positions of D5's subjects
(pinhole, 16:9), as text or JSON.

**`layout`** writes the editor command script (a JSON array of steps, as `GenerateIslandTerrain.py:346-354`) that, in an
editor with `Scenes/Outpost.stscene` open and the materials and prefabs of D7-D9 present, creates the scene's entities of D9:
the terrain instance and its tiles' materials and colliders, the apron, puddles and mud, every prop at the generator's height
under it (the triangle's surface, `GenerateIslandTerrain.py:143-158`), the seeded rocks and brush (`prefab.instantiate
{instances}`), the six probes (D10), the lights, the camera rig and the tour's markers. Names are unique, so its own steps find
children with `entity.find {"name"}` and `bind`. It is run once into a fresh scene; later edits are commands of their own.
The script is not committed (the generator is).

**`setup`** prints a station's edit-mode steps (D4: `lighting.applySkyPreset`, `component.set` of the sun, fog, Post
Process and lights, `entity.setActive` of the probes and emissive entities), as LookDev `Setup` entries (`entityName`) or as
command steps with `entity.find` and `bind`. Shots.json and `ShowcaseRenderCommands.json` take their station steps from it.

**`checkshots`** reads `shots/showcase.shot2-1920x1080-taa.png` and `shots/showcase.shot3-1920x1080-taa.png` of a LookDev
capture directory (its own PNG reader for 8-bit RGB and RGBA with the five filters, as `LookDev.py:123-210` reads them; that
code is this repository's), finds the centroid of the brightest 0.1% of pixels in the bus-side crop (shot 2) and the brightest
column in the tank crop (shot 3), and compares them with the projected highlight: at most 40 px (shot 2) and 30 px (shot 3)
off at 1920 x 1080. With `shots/showcase.shot3.noprobe-1920x1080-taa.png` present it also reports the difference of the
`streak` crop's mean code between `showcase.shot3` and `.noprobe` (D15 shot 3). Prints PASS or FAIL per check; exit 1 on a
failure.

**`checkshots --live`** measures highlight shimmer on D14's live shots: for each `shots/showcase.live.shot<N>[.fxaa|.off]-
1920x1080-live-<k>.png`, k = 1-4, the shot's highlight crop (shot 2 `highlight`, 3 `streak`, 4 `glint`, 6 `lantern`), the
encoded luma `Y = (0.2126 R + 0.7152 G + 0.0722 B) / 255` (as `reflections`' metrics define it), the mean `B_k` of each 32 x
32 block wholly inside the crop, and the second differences `D = |B_{k+1} - 2 B_k + B_{k-1}|` for k = 2, 3 (a steady dolly
moves the image at a constant rate, which the second difference cancels). It prints the mean and the 99th percentile of `D`
over blocks and k, in percent of full scale, per shot and AA mode: PASS at a mean of at most 1.0 and a p99 of at most 3.0
(plan 1.1's reflection flicker bound over 32 px blocks, proposed here for highlights).

**`selftest`** (CTest `StrataTools.ShowcaseTerrain`): `check`; `terrain` twice into `--work`, byte-equal; each GLB byte-equal
to the committed one in `--committed`; the station rows parsed from `Stations.h`'s `c_Stations` (one line per station,
`{ <hour>, <elevation>, <drawn>, <azimuth>, <scale>, <fog>, <exposure>, <shadow distance>, <local lights> }`) equal to its
own table; D5's mirror relations of shots 2-4 hold for the layout within 1 cm and 0.1 degree. Under 60 s (the horizon march
uses the sampled grid, not the noise). *As built (the reflections' adoption):* it also asks rule 7 about the azimuth the
dusk sun had (32) and fails if the rule would pass it: the sun behind the bus is what the rule is there to refuse.

### D7 `Tools/GenerateShowcaseProps.py`

```
python -I Tools/GenerateShowcaseProps.py props     --output Samples/Showcase/Assets/Generated/Props
python -I Tools/GenerateShowcaseProps.py materials --output <Materials.json>    # material.create steps (D8)
python -I Tools/GenerateShowcaseProps.py prefabs   --output <Prefabs.json>      # prefab-building steps (D9)
python -I Tools/GenerateShowcaseProps.py selftest  --committed Samples/Showcase/Assets/Generated/Props --work <dir>
```

Same rules as D6: `SEED = 20261102`, quantized attributes (circles use `math.cos`/`math.sin`, whose last-bit differences
quantization absorbs), meters, +Y up, each model's origin at its footprint's center on the ground, normals, texture
coordinates in meters (a material's `UVTiling` is 1 / the texture's real size), no tangents. **One node per material slot**
(MeshRenderer's `Material` overrides every submesh, `Scene/Components.h:163`), named `<Prop>.<Slot>` so command scripts find
them, each with a factor-only glTF material of the slot's name (so an unassigned model still reads right). Each prop also has
`<Prop>.Collision` nodes: convex hulls of at most 32 vertices, which the prefab script turns into colliders (D9).

| Model | Parts (meters) | Nodes (slot: material, D8) | Triangles |
| --- | --- | --- | --- |
| `Bus.glb` | Body 10.8 long (+Z front), 2.44 wide, floor 0.62, roof crown 2.99. Profile: lower panel vertical 0.62-1.52; window band 1.52-2.25 inset 3 cm, its windows boarded (planks 2 cm behind the frame); letterboard 2.25-2.62 **vertical** and flush with the lower panel (the tumblehome starts at 2.62 in the roof arc: shot 2's mirror point lies on the letterboard, D5); roof arc r 0.38 to 2.95, crowned. Rub rails: half-rounds 6 cm tall, 1.8 cm proud at 0.95, 1.30, 1.52, broken at arches and the door. Panel seams: V-grooves 8 mm wide, 3 mm deep every 1.15 m below 2.25 and at the rails, and on the letterboard at the bare sections' ends. Wheel arches: r 0.56 at z +3.2 and -2.6, 4 cm lips. Ends: vertical corners r 0.18, boarded windshield 1.5-2.4, an outlined rear door. Bumpers 0.20 x 0.12 at 0.55-0.75. Chassis box 0.35-0.62 inset 0.15. On the +X side, two **letterboard** sections (z 0.85-2.0 and -1.45 to -0.30, where shot 2's highlight runs, so it crosses bare and painted steel), the lower panel under the second (z -1.45 to -0.30) and one roof panel are bare. Side UVs: u = z plus a seeded offset per panel (a multiple of 0.37 m), v = y; roof u = z, v = arc length; ends u = x, v = y | `Bus.Body`: BusPaint; `Bus.BarePanels`, `Bus.Rails`, `Bus.Bumpers`: WeatheredSteel; `Bus.Boards`: Planks; `Bus.Chassis`: Grime; `Bus.Collision` (one box) | 25-30k |
| `OilTank.glb` | Shell r 4.0, six 1.2 m courses (7.2 m), 160 segments; horizontal weld beads (half-round r 1.2 cm) at the course joints; vertical beads per course at 0, 120, 240 degrees plus 30 degrees per course; cone roof rising 0.33 m, 48 radial segments, 16 radial roof seams; top angle ring 8 x 8 cm; a manway (disc r 0.3, 24 bolts) at 0.8 m facing tank azimuth -90; a nozzle stub r 0.08 at 0.5 m, azimuth 200. UVs: u = arc length plus a seeded offset per course, v = y | `OilTank.Shell`, `OilTank.Roof`, `OilTank.Fittings`: WeatheredSteel; `OilTank.Pad` (r 4.35, 0.3 m): Concrete; `OilTank.Collision` (16-gon prism) | 35-40k |
| `FuelTank.glb` | Shell r 1.3, 6.4 m along Z plus 2:1 ellipsoidal heads (0.65 deep), axis at 1.75 m; circumferential welds every 1.6 m; two saddles (0.25 m plates, curved to the shell) at z +-2.0; a bolted manhole r 0.3 on top; two bottom nozzles | `FuelTank.Shell`: TankPaint; `FuelTank.Saddles`, `FuelTank.Fittings`: WeatheredSteel; `FuelTank.Collision` (12-gon prism) | 12k |
| `Pipes.glb` | From the oil tank's nozzle to a valve stand at (-6.0, 0, 12.0): r 0.08, two elbows (bend r 0.3), flanges every 4 m (r 0.14, 3 cm), H-frame supports every 3 m (0.5 m), a valve wheel (torus r 0.18); a 3 m stub to a standpipe. Origin at the oil tank's center (placed with it) | `Pipes.Pipe`: WeatheredSteel; `Pipes.Supports`: BusPaint | 6k |
| `Hut.glb` | Walls 0.2 thick, inside 4.8 x 4.0, 2.6 m at the door wall (local +Z) and 2.3 at the back; door opening 0.9 x 2.05 at local x -0.8, window 0.8 x 0.6 (sill 1.0) at x +1.2, both on the door wall; the door leaf open 70 degrees, the shutters open; floor planks 0.1 m up; rafters 5 x 15 cm every 0.6 m; roof sheets overhanging 0.3 m; inside a shelf on the back wall and a table 1.2 x 0.7 x 0.75 by the right wall; lintels | `Hut.Walls`: Plaster; `Hut.Floor`, `Hut.Door`, `Hut.Shutters`, `Hut.Rafters`, `Hut.Furniture`, `Hut.Lintels`: Planks; `Hut.Roof`: CorrugatedRed; `Hut.Collision.1-6`: three walls as boxes, and the door wall as three boxes (left of the door, from the door's right jamb to the corner with the window's opening filled, above the door's lintel), so the walk enters through the door (a convex hull of the whole door wall would close it) | 8k |
| `LeanTo.glb` | 5.2 deep x 3.0 wide on the hut's +X side, four posts 0.1 m square, roof sheets sloping 2.4 to 2.0 m | `LeanTo.Posts`: Planks; `LeanTo.Roof`: CorrugatedRust; `LeanTo.Collision.1-4` (posts) | 1k |
| `Embers.glb` | 25 seeded irregular chunks (subdivided octahedra, r 3-8 cm) within r 0.35 m; a wick: a 1.5 x 4 cm capsule | `Embers.Embers`, `Embers.Wick`: Embers | 3k |

**The bare steel's two maps**, from one tileable field (periodic lattice noise), 1024 x 1024, 8-bit RGB, covering 4 m
(`UVTiling` 0.25: 3.9 mm a texel, about the footprint of one 3840 x 2160 pixel at shot 2's 8.5 m; a 2 m tile would repeat
four times across shot 3's tank and read as a pattern):

- `f`: vertical rain streaks (1D value noise along x at three frequencies, stretched 8 times along y) and blotches (2D fbm),
  in [0, 1]; `m`: a rust mask, `S(0.62, 0.78, 0.6 f + 0.4 g)` with `g` a second fbm, about 15-25% of the area (the
  generator prints the share; the field is tileable, so rust does not follow the props' seams).
- `WeatheredSteel_arm.png` (the file name's `arm` token imports it as linear data, `TextureImporter.cpp:338-341`; a plain
  `Weathering.png` would import as sRGB color and distort the roughness): R 255; G the roughness, `0.22 + 0.28 f` on steel and
  0.85 on rust (`mix` by `m`); B the metalness, `255 * (1 - m)`.
- `WeatheredSteel_diff.png` (sRGB color): steel `(0.56, 0.56, 0.57)` linear (codes 196, 196, 197) mixed by `m` toward rust
  `(0.20, 0.09, 0.04)` linear. Rust is dielectric, rough and brown, so the steel varies in color and metalness, not only in
  roughness: a uniform steel F0 over an 8 m tank is the "looks like CG" of `p04`.
- Written with filter 1 (Sub) on every row and zlib level 9 (smooth noise compresses about twice as well as with filter 0):
  about 1.3 and 2.0 MB (D2). Bare steel never depends on a scan's B channel.

`selftest` (CTest `StrataTools.ShowcaseProps`): `props` twice, byte-equal; each GLB byte-equal to the committed one;
the two `WeatheredSteel_*.png` compared by decoded pixels (another zlib may compress differently); `Bus.Body`'s letterboard
faces vertical within 0.01 degree; every node of the table present with its
triangle count within the table's range; each collision hull convex with at most 32 vertices.

### D8 Materials

`.stmat` files under `Assets/Materials/`, made by the `materials` script's `material.create` steps from the generator's table,
then tuned with `material.set` (S6). After creation the `.stmat` files are the truth; the regression checks the values that
the shots rest on (D13). For dielectric scans Metallic is 0. For the painted metal sets Metallic is 1 so that the map's B
decides if S1 finds metal in it, and is 0 if S1 finds the B channel zero. Scan maps: BaseColorMap = `diff`, NormalMap =
`nor_gl`, MetallicRoughnessMap and OcclusionMap = `arm` (one texture, both slots, `Material.h:42-44`).

| Material | Maps | UVTiling | BaseColor | Metallic | Roughness | Other | On |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Asphalt | `asphalt_02` 2K | 0.3333 | (0.72, 0.72, 0.72) | 0 | 0.55 (damp) | | Apron |
| Mud | `brown_mud_03` 1K | 0.7692 | (0.85, 0.85, 0.85) | 0 | 0.80 | | Mud patches |
| Ground | `dry_ground_01` 2K | 0.25 | (1, 1, 1) | 0 | 1.0 | | Inner terrain |
| RingGround | `rocky_terrain_02` 1K | 0.01111 | (1, 1, 1) | 0 | 1.0 | | The ring |
| Water | none | | (0.018, 0.020, 0.022) | 0 | 0.03 | Opaque; IOR 1.5 (R2: 1.33) | Puddles |
| BusPaint | `rusty_metal_03` 1K | 0.7407 | (1, 1, 1) | S1's finding | 0.45 (p11) | Clearcoat 0 (D2) | Bus body, pipe supports |
| WeatheredSteel | BaseColor `WeatheredSteel_diff.png`; MR `WeatheredSteel_arm.png`; Normal `rusty_metal_03` | 0.25 (both steel maps; the normal map takes the same tiling, 4 m a tile) | (1, 1, 1) (the map carries steel and rust) | 1 (the map's B: 0 on rust) | 1.0 (the map's 0.22-0.50 on steel, 0.85 on rust) | NormalScale 0.6 | Bus bare panels, rails, bumpers; oil tank; pipes; saddles; fittings |
| TankPaint | `green_metal_rust` 2K | 1.0 | (1, 1, 1) | S1's finding | 0.70 | | Fuel tank shell |
| Plaster | `beige_wall_001` 2K | 0.3333 | (0.85, 0.85, 0.85) | 0 | 1.0 | | Hut walls |
| Concrete | `beige_wall_001` 2K | 0.25 | (0.55, 0.55, 0.55) | 0 | 1.0 | | Oil tank pad |
| Planks | `weathered_brown_planks` 1K | 0.5556 | (1, 1, 1) | 0 | 1.0 | | Hut wood, bus boards, lean-to posts |
| CorrugatedRed | `rusty_painted_metal` 1K | 0.4545 | (1, 1, 1) | S1's finding | 1.0 | | Hut roof |
| CorrugatedRust | `rusty_corrugated_iron` 1K | 0.5 | (1, 1, 1) | S1's finding | 1.0 | | Lean-to roof |
| Grime | none | | (0.035, 0.032, 0.030) | 0 | 0.85 | | Bus chassis |
| Embers | none | | (0.05, 0.04, 0.035) | 0 | 0.9 | EmissiveColor (1.0, 0.35, 0.08), EmissiveIntensity 4000 nits | Embers, wicks |

`UVTiling` is a two-component property (`Material.h`, `MaterialProperties::UVTiling`): the `materials` script writes `[t, t]`.

*As built (the reflections' adoption):* Water has IOR 1.33 and the base (0.035, 0.037, 0.040), the wet asphalt under it: the
row's base was darkened for an F0 of 0.04, and at 0.02 the water mirrors half as much, so the adoption made it lighter, not
darker as proposed below. `Water.glb`'s stand-in carries the same base and roughness: both generators take them from
`WATER_COLOR` and `WATER_ROUGHNESS` of `GenerateShowcaseTerrain.py` (the stand-in had kept the row's base).

Proposed values to tune in S6 (each with the reason): the bus paint's roughness factor (no highlight at 1, glints at 0.45,
`p08`/`p11`); the apron's damp roughness 0.45-0.6 and darker base (audit: "uniformly damp through factors"); water 0.02-0.06
(audit section 5), darker base after R2's F0 of 0.02. The scans' own materials (rocks, drums, car, lantern, tyre, ladder, fire
pit, brush) stay as imported.

### D9 The project and the scene

**Project** (S1): `project.create {"directory": "<worktree>/Samples/Showcase", "name": "Showcase"}` (no template: the
scene is built from nothing), deleting the example `Scripts/Spinner.cpp`. `project.setCollisionLayers {"layerNames": {"1":
"Terrain", "2": "Props", "3": "Player"}}`. `project.setGameInfo {"title": "High Desert Outpost", "version": "1.0.0", "icon":
"Icon.png"}` (S7). `project.setExportSettings {"loadingScene": "Scenes/Loading.stscene"}` (S5). `Renderer.Quality` left at
its default, High. The script module is `ShowcaseScripts`; the exported game `Showcase`.

**Input actions** (S5): map `Gameplay`: `Move` (Vector2: `2D:Key/W,Key/S,Key/A,Key/D`, `Gamepad/LeftStick`), `Look`
(Vector2: `Gamepad/RightStick`), `Jump` (`Key/Space`, `Gamepad/A`), `Run` (`Key/LeftShift`, `Gamepad/RightBumper`), `Up`
(`Key/E`, `Gamepad/RightTrigger+`), `Down` (`Key/Q`, `Gamepad/LeftTrigger+`: the triggers are axes, `InputNames.cpp:76`, bound
to a button as a half axis like the Third-Person's `Gamepad/LeftY+`, `ThirdPerson.stproj:52`); map `Showcase`: `Fly`
(`Key/Tab`, `Gamepad/Y`), `Photo` (`Key/P`, `Gamepad/Start`), `HideUI` (`Key/H`, `Gamepad/Back`), `Tour` (`Key/T`), `Bookmark1`-`Bookmark7`
(`Key/D1`-`Key/D7`). Binding names checked against `inputActions.setAction`'s schema in the first hour (*unverified* beyond
those the make-a-game skill shows). The mouse is read directly (no action binds motion).

**Scene `Scenes/Outpost.stscene`** (the start scene), its entities:

```
Outpost                     Outpost script (D11); widgets' EventTarget
Sun                         DirectionalLight (GoldenHour: 120,000 lux; ShadowDistance 200; CascadeBlend 0.1), rotation of Golden
Sky                         SkyLight: Atmosphere, GoldenHour's density 1.0, haze 1.5, GroundAlbedo 0.3; AerialPerspectiveScale 4
Fog                         HeightFog (D4)
Post Process                PostProcess: TAA (Sharp), ScreenSpaceReflections on, Filmic, Exposure 0, AutoExposure, MaxEV 16,
                            Vignette 0.12, AmbientOcclusionRadius 1.0, MotionBlur off (as the other samples)
Player                      CharacterController (Height 1.8, Radius 0.3, Layer Player, Enabled false); at Shot2.Key minus 1.6 m
  Eye                       Camera: Primary, FOV 40, Near 0.1, Far 3000, DepthOfField false, Aperture 2.8; local (0, 1.6, 0)
Terrain                     RigidBody Static, Layer Terrain
  Island (Terrain.glb)      Ground_<i>_<j>: Material Ground, MeshCollider (Convex false); Ring_<k>: Material RingGround, no collider
  Apron, ApronFine          Material Asphalt, MeshCollider (Convex false)
  Puddle_<n>                Material Water, no collider
  Mud_<n>                   Material Mud
Props                       RigidBody Static, Layer Props
  Bus, OilTank (+ Pipes, Ladder), FuelTank, Hut, LeanTo, CoveredCar, FirePit, LanternA, LanternB, drums, tyres
Rocks                       RigidBody Static, Layer Props: cliffs, boulders (BoxCollider at 80% of asset.info's bounds)
Brush                       dry branches (no colliders)
Stations                    Probe.<Dawn|Golden|Dusk>.<Yard|Hut> (D10)
Tour                        Shot<1-6>.<From|Key|To|Look>, Macro.Key, Macro.Look, Measure.Yard, Measure.YardLook, Measure.Hut, Measure.HutLook
UI                          Overlay canvas (Fade, Caption), PhotoMode canvas (D12)
```

A collider entity takes the layer of its nearest RigidBody ancestor (`Physics/PhysicsComponents.h:14-17`). The Measure
markers are the graybox's measurement views for shot 5 (D15).

**Prefabs** (S3, the `prefabs` script): for each model, `prefab.instantiate` at the origin; `component.set` of each
`<Prop>.<Slot>` child's `MeshRenderer.Material`; on each `<Prop>.Collision*` child, in this order, `component.get MeshRenderer`
(bind `/values/Mesh`), `component.add MeshCollider {"Mesh": <that mesh>, "Convex": true}`, then `component.remove
MeshRenderer` (`MeshColliderComponent::Mesh` "uses the MeshRenderer's mesh when unset": removed first, the collider would have
no mesh); extras (the bus's six tyres, `Embers` and a `PointLight` "Fire" (`CastShadows` true, `Range` 12) with a `Flicker`
script on the fire pit, a `Wick` and a `PointLight` "Light" (`CastShadows` true, `Range` 8) with `Flicker` in the lantern);
`prefab.create` to `Prefabs/<Prop>.stprefab`; the instance deleted. Prefabs: Bus, OilTank (with Pipes and Ladder), FuelTank,
Hut (with LeanTo), FirePit, Lantern. The Open World's prefabs made from GLB instances are the precedent.

**Unique names.** The `layout` script renames, with `entity.rename`, what instances repeat and scripts or regressions find
by name: the fire pit's light `Fire` (one instance) and embers `FirePit.Embers`; the two lantern instances `LanternA`,
`LanternB`, their lights `LanternA.Light`, `LanternB.Light` (`LanternB.Light`'s `CastShadows` false, D3) and wicks
`LanternA.Wick`, `LanternB.Wick`. `Outpost`'s entity fields (D11) name these.

**Loading scene** `Scenes/Loading.stscene` (S5): a camera with a black clear color, a canvas with the title and a progress bar
driven by `LoadingScreen` (`Game::GetSceneLoadProgress()`), as the Open World's (`Samples/OpenWorld/Scripts/LoadingScreen.cpp`).

### D10 The six probes: placement, bake, switching

Stations are **six probe entities**, `Yard` and `Hut` for each of Dawn, Golden and Dusk (plan 4.2 step 6), never two probes
baked three times: a bake trashes the capture an earlier bake made for that entity once nothing uses it
(`EditorLightingCommands.cpp:365-388`).

| Probe | Position | Rotation | Extents (half) | BlendDistance | BoxProjection | Priority | Intensity |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `Probe.<S>.Yard` | (2.0, 1.6, 4.0) (10.5 m from the bus, 13.2 from the tank, 18.4 from shot 4's camera) | 0 | (34, 14, 34) | 8 | true (Q6) | 0 | 1 |
| `Probe.<S>.Hut` | (-22.0, 1.2, -18.0) | yaw 45 (the hut's) | (2.45, 1.35, 2.05) | 0.3 | true | 2 | 1 |

**Bake procedure (S4, again after any change the probes see; the S4 commit message records it):** for each station in the
order Dawn, Dusk, Golden (Golden last, so the saved scene is Golden):

1. The station's edit-mode steps (`GenerateShowcaseTerrain.py setup --station <S> --format script`): sun, fog, exposure,
   lights, emissive entities.
2. `entity.setActive {"entities": [the station's two probes], "active": true}`, the other four `false` (a bake's renderer
   reads active probes).
3. `lighting.bakeProbes {"entities": [the two]}`, twice (the second pass adds the bounce the first capture of the other probe
   gives: "baking twice adds a second bounce", `:872-873`; it trashes the first pass's files).

   *As built (the reflections' adoption):* eight times (`BAKE_PASSES`), not twice. A pass draws the probes' present
   captures, the baked probe's own too, so two passes left the hut's captures depending on the ones the project held
   (begun from the sample's earlier captures, the dusk hut's came out 18% above where it settles, and 17% above the one
   two passes gave from the adoption's first captures). A pass leaves about 0.16 of the difference; after eight, bakes
   begun from different captures agree to 0.01% in every 64 x 64 block's mean, so one run reproduces what is committed.
   `selftest` holds the script to the probes' table (`check_bake`). The same work found that the first pass after the
   hut probe's move had been taken from the place it had left (`lighting.bakeProbes` read the world matrix the frame had
   not yet updated; fixed in the command).

Then `scene.save`. Six `.hdr` files in `Assets/Lighting/Outpost/`.

**Switching in play** (`Outpost::SetStation`):

1. In `OnCreate`, each probe's `Capture` is read (`GetProperty<AssetHandle>("ReflectionProbe", "Capture")`) and
   `Assets::GetLoadProgress` called on it, which loads all of it and keeps it (`SDK/Scene.h:53-61`); `CapturesFound` counts the
   valid, known ones (progress at least 0) and `CapturesResident` those at 1, polled each frame. The tour starts once all found
   captures are resident or 5 s of unscaled time passed, logging `Showcase: stations ready (found <n>, resident <m>)`.
2. `SetStation(new)`: the new station's probes `SetActive(true)` with their priority raised by one (Yard 1, Hut 3), the sun,
   fog, exposure and local lights set (D4), `Station` and `SunElevation`/`SunAzimuth` fields updated, `Showcase: station
   <name>` logged.
3. After `ProbeHoldSeconds` (1.0 s, unscaled), the old station's probes `SetActive(false)` and the new ones' priorities back
   (Yard 0, Hut 2).
4. At most two stations are active: a switch while a hold runs (the photo mode's slider scrubbed across a boundary) first
   ends that hold at once (step 3 for the station it was holding), then starts its own. Switching to the station already
   active does nothing.

Why: until a new capture is converted it is not in the list and the old probe lights its box, so the box never falls to the
sky (`SceneRendererReflectionProbes.cpp:289-297`); once converted (two a frame, `SceneRenderer.h:925`), the raised priority
makes the new one win at once while both are active; the hut's priorities stay above the yard's. Converted captures keep their
slots (16, `SceneRenderData.h:272`), so returning to a station converts nothing. In the tour every switch happens under a fade;
in the photo mode a switch pops once, the rule of plan 4.2.

### D11 The scripts

Written against `StrataScriptCore` only (skills `strata-scripting`, `strata-make-a-game`), compiled warning-free with
`-Wall -Wextra` once with GCC or Clang (the skill's line 260). Files in `S/Scripts/`:

| File | What |
| --- | --- |
| `Stations.h` | No engine calls: `enum class StationId { Dawn, Golden, Dusk }`, `struct StationValues { float Hour, Elevation, DrawnElevation, Azimuth, LightScale, FogDensity, Exposure, ShadowDistance; bool LocalLights; }`, `constexpr std::array<StationValues, 3> c_Stations` (one line per station, parsed by D6's selftest), `TowardSun`, `SampleDay(float hour)`, `NearestStation(float hour)`, the photo knots |
| `TourPath.h` | No engine calls: `CatmullRom` (as `Explorer.cpp:37-42`), `EaseInOut`, `struct TourShot { int Number; float Seconds; float Fov; StationId Station; const char* Caption; }`, `constexpr c_Shots` (14, 14, 12, 14, 10, 14 s: 78 s), the timeline (fades of `FadeSeconds` at both ends of each shot) |
| `Outpost.h`, `Outpost.cpp` | The director, `ST_SCRIPT_CLASS(Outpost)`: fields, lifecycle, input, modes, stations, logs |
| `OutpostTour.cpp`, `OutpostWalk.cpp`, `OutpostPhoto.cpp` | Member functions of `Outpost` by concern (tour; walk and flight; photo mode and UI callbacks) |
| `Flicker.h`, `Flicker.cpp` | `Flicker`: on a light; `Intensity` (target, set by `Outpost`), `Amount` 0.15, `Speed` 9; writes `PointLight.Intensity = Intensity * (1 + Amount * n(t))`, n a smooth deterministic noise in [-1, 1]; 0 when `Intensity` is 0 |
| `LoadingScreen.cpp` | The loading scene's progress bar |

**`Outpost`'s fields** (registered with `ST_SCRIPT_FIELD`; the regression reads the state ones):

- Entities: `Sun`, `Sky`, `Fog`, `PostProcess`, `Player`, `Eye`, `FadeImage`, `Caption`, `PhotoPanel`; `TimeSlider`,
  `TimeLabel`, `ExposureSlider`, `ExposureLabel`, `FovSlider`, `FovLabel`, `FocusSlider`, `FocusLabel`, `ApertureSlider`,
  `ApertureLabel`, `DofToggle`, `AutoFocusToggle`, `AntiAliasingLabel`; `DawnYard`, `DawnHut`, `GoldenYard`, `GoldenHut`,
  `DuskYard`, `DuskHut`; `FireLight` (`Fire`), `LanternALight`, `LanternBLight` (`LanternA.Light`, `LanternB.Light`);
  `Embers` (`FirePit.Embers`), `WickA`, `WickB` (`LanternA.Wick`, `LanternB.Wick`).
- Tuning: `TourOnStart` true, `StartShot` 1 (the shot a tour starts with, 1-6: LookDev's live shots start at their shot,
  D14), `TourSpeed` 1, `LoopTour` true, `FadeSeconds` 0.75, `WalkSpeed` 1.5, `RunSpeed` 3.2,
  `FlySpeed` 6, `FastFactor` 4, `LookSpeed` 0.12 (degrees per pixel), `StickTurnSpeed` 120, `UseController` false (true
  after `physics-timing`'s adoption), `WalkRadius` 140, `ProbeHoldSeconds` 1, `NightAfterDusk` false, `PhotoHourMax` 19.0,
  `SunIlluminance` 120000. *As built:* `UseController` and `NightAfterDusk` are not fields of the script: each comes
  with its adoption (section 11), and the night was not adopted with R2.
- State: `Mode` (0 tour, 1 walk, 2 fly), `Shot` (0-6), `Station` (0-2), `CapturesFound`, `CapturesResident`, `PhotoOpen`,
  `PhotoHour`, `SunElevation`, `SunAzimuth`, `Bookmark` (last one taken, 0 none), `DistanceWalked`, `Grounded`,
  `HeightAboveGround` (the eye's height above the ground under it), `ToursCompleted`.

**Lifecycle.** `OnCreate`: find the tour markers by name (`Scene::FindEntityByName`), request the captures, apply Golden, set
the fade black, then `StartTour()` if `TourOnStart`, else `Mode` 1 (the walk, from the `Player` entity's place) with `Shot`
0. `StartTour()` (also the `Tour` action): `Mode` 0, the fade black, the station of `StartShot`'s shot applied under it, then
the first shot once the stations are ready (D10 step 1: resident, or 5 s of unscaled time after `OnCreate`), so a tour
started late does not wait again. `OnReload`: markers again; the tour resumes at its `Shot`. `OnUpdate(dt)` uses
`Time::GetUnscaledDeltaTime()` (the photo mode may pause time; while the editor steps a paused game it is the fixed step,
`Scene.cpp:2155-2157`, so a regression's step counts are exact).

**Input** (actions of D9): during the tour any of `Gameplay/Move`, `Look`, `Jump`, `Showcase/Fly`, `Photo` pressed, or a
mouse button, ends it into the walk (`Showcase: tour taken over`); `Showcase/Fly` toggles walk and fly (`Showcase: walking`,
`Showcase: flying`); `Showcase/Photo` opens and closes the photo panel (`Showcase: photo mode on|off`); `HideUI` hides the
canvases; `Tour` restarts the tour (`StartTour()`); `Bookmark<n>` moves the camera to shot n's key (7: the macro), sets the
photo hour to the shot's station hour, and logs `Showcase: bookmark <n>`. Bookmark 7 also turns `Autofocus` off and sets
`FocusDistance` to the key's computed distance to the boulder's surface (0.45 m) with `DepthOfField` on at f/2.8: the
rocks' colliders are boxes at 80% of their bounds (D9), so a ray from the view's center would focus behind the surface at a
macro distance.

**Tour** (`OutpostTour.cpp`): logs `Showcase: tour started`; at each shot's start `Showcase: shot <n> (<caption>)`
(captions: "dawn vista", "the bus", "the oil tank", "the wet apron", "the hut", "dusk"); the eye follows the Catmull-Rom of
From, Key, To (ends repeated), eased in time, looking at `Look` (yaw on `Player`, pitch on `Eye`, as `Explorer.cpp:196-201`); FOV per shot;
at the black of a fade the station changes (shots 1-2 and 5-6) and the camera cuts; after shot 6, `ToursCompleted++` and, with
`LoopTour`, `Showcase: tour looped` and shot 1 again; `Caption` shows the shot's caption during the tour. Time runs at
`TourSpeed`.

**Walk** (`OutpostWalk.cpp`), two forms:

- `UseController` false (the landing form; plan 4.2's fallback): no controller (`Enabled` false); each frame the move
  (`Gameplay/Move` times `WalkSpeed` or `RunSpeed`, along the body's flattened forward and right) is tried: a ray from 0.5 m up
  along the move, `|step| + 0.35` long, against layers Terrain and Props, slides along a wall's normal; a ray down from the
  target 1.0 m up, 3 m long, puts the body on the ground if the rise is at most 0.45 m, else it stays; the body stays within
  `WalkRadius` of the origin. Moved per frame, so it is smooth at every frame rate (a 60 Hz controller would show the jitter the
  owner reported, audit section 5). No jump.
- `UseController` true (after `physics-timing`): the controller enabled, `Move(velocity)`, `Jump(4.5)` on `Gameplay/Jump`, as
  the skill's `PlayerController` (lines 156-187), drawn between fixed poses by `physics-timing`'s interpolation.

Mouse look with a locked cursor (`Input::SetCursorMode(CursorMode::Locked)`), the right stick through `Gameplay/Look`.
Handing over from the tour: the camera's position dropped to the ground below it (a ray from 300 m up). `DistanceWalked` adds
the horizontal distance moved per frame while walking; `HeightAboveGround` is measured by a ray down every frame in every mode.

**Flight**: per-frame translation (`FlySpeed`, `FastFactor` with `Run`, `Up`/`Down`), mouse look while the right button is
held (cursor free), the right stick; no collisions; at least 0.3 m above the ground.

**Logging**: one `Log::Info` line per event above, prefixed `Showcase: `; warnings only for a missing marker or probe
(`Showcase: no entity '<name>'`), which the regressions treat as failures.

### D12 The photo mode

A `UICanvas` "PhotoMode" (`ScaleWithScreen`, reference 1280 x 720, match height), made with `ui.create` in S5: a panel
300 x 600 on the right (a rounded translucent `UIImage`), inactive until P. Rows (each a `UIText` label and a widget; the
script writes the label's value text):

| Row | Widget | Range | Writes |
| --- | --- | --- | --- |
| Time | slider `TimeSlider` | 6.5 to `PhotoHourMax` hours | `PhotoHour`; `SampleDay` to the sun, fog, exposure and lights; the probes to the nearest station (D4) |
| Exposure | slider | -3 to +3 EV | `PostProcess.Exposure` = station's + the slider's |
| Field of view | slider | 20 to 90 degrees | `Camera.PerspectiveFOV` |
| Focus distance | slider (0-1, shown in meters) | 0.5 * 400^s m (0.5 to 200 m) | `Camera.FocusDistance` |
| f-number | slider, whole numbers 0-8 | 1.4, 2, 2.8, 4, 5.6, 8, 11, 16, 22 | `Camera.Aperture` |
| Depth of field | toggle | | `Camera.DepthOfField` |
| Autofocus | toggle (on) | | each frame a ray from the view's center: `FocusDistance` = its distance (colliders only: bookmark 7 turns it off, D11) |
| Quality | four toggles in a group `PresetLow`, `PresetMedium`, `PresetHigh`, `PresetUltra` | | `Graphics::SetPreset`; logs `Showcase: graphics preset <name>` |
| Anti-aliasing | a button cycling the option's choices | `Graphics::FindOption("AntiAliasing")->Choices` | `Graphics::Set("AntiAliasing", choice)` |
| Bookmarks | buttons `Bookmark1`-`Bookmark7` | | as the actions |

The rows' titles come from `Graphics::FindOption(name)->Title` (plan: "the Graphics menu through `Graphics::Options()`");
the toggles show the preset in effect (`Graphics::GetPreset`), including one set by `--quality`. Choices last for the session
(no `Graphics::Save`: a showcase is not a game with settings). Clicks reach `Outpost::OnUIClick` and `OnUIValueChanged`
(widgets' `EventTarget` is `Outpost`). Group toggles' 0 events are ignored (the Third-Person's lesson, its spec's D12).
In the photo mode the walk or flight goes on; the cursor is free; H hides both canvases for a clean picture (stills are taken
with the platform's tools or, in the editor, `viewport.capture {"camera": "scene"}`: scripts have no screenshot function, E5).

### D13 `StrataTests/Editor/ShowcaseCommands.json.in`

Run by `SampleProject.cmake` in a `--no-gpu` editor on a fresh copy (`:110`). The tour runs four times faster, so shot 6 starts
at 64 / 4 = 16 s, 960 steps, after the tour starts. **The tour is started by the script, not by the scene:** an `until` that
misses in a paused game runs a fixed step before its next attempt (`EditorCommandRunner.h:91-95`), so `physics.stats`'s wait
for the colliders (seconds on CI in Debug) would play the tour on and the `Shot 1` check could find shot 3. `TourOnStart`
false keeps the game in the walk, standing still, until `input.key T` starts the tour after that wait; both fields are undone
after play (two `edit.undo`).

```json
[
	{"command": "script.build", "expect": {"/success": {"equals": true}}},
	{"command": "editor.status", "parameters": {"sections": ["assets"]}, "expect": {"/assets/importing": {"equals": 0}, "/assets/scanning": {"equals": null}}, "until": {"seconds": 720}},
	{"command": "scene.info", "expect": {"/name": {"equals": "Outpost"}}},
	{"command": "entity.find", "parameters": {"name": "Outpost"}, "bind": {"outpost": "/entities/0"}},
	{"command": "entity.find", "parameters": {"name": "Sun"}, "bind": {"sun": "/entities/0"}},
	{"command": "entity.find", "parameters": {"name": "Post Process"}, "bind": {"post": "/entities/0"}},
	{"command": "component.get", "parameters": {"entity": {"$ref": "post"}, "component": "PostProcess"}, "expect": {"/values/AntiAliasingMethod": {"equals": "TAA"}, "/values/ScreenSpaceReflections": {"equals": true}, "/values/Tonemapper": {"equals": "Filmic"}, "/values/MotionBlur": {"equals": false}, "/values/AutoExposureMaxEV": {"equals": 16}, "/values/Exposure": {"equals": 0}}},
	{"command": "component.get", "parameters": {"entity": {"$ref": "sun"}, "component": "DirectionalLight"}, "expect": {"/values/Intensity": {"min": 119999, "max": 120001}, "/values/CastShadows": {"equals": true}}},
	{"command": "material.get", "parameters": {"material": "Materials/BusPaint.stmat"}, "expect": {"/values/Clearcoat": {"equals": 0}, "/values/Roughness": {"min": 0.3, "max": 0.6}, "/warnings": {"equals": []}}},
	{"command": "material.get", "parameters": {"material": "Materials/WeatheredSteel.stmat"}, "expect": {"/values/Metallic": {"equals": 1}, "/warnings": {"equals": []}}},
	{"command": "material.get", "parameters": {"material": "Materials/Water.stmat"}, "expect": {"/values/AlphaMode": {"equals": "Opaque"}, "/values/Roughness": {"min": 0.02, "max": 0.06}}},
	{"command": "material.get", "parameters": {"material": "Materials/Asphalt.stmat"}, "expect": {"/warnings": {"equals": []}}},
	{"command": "script.setField", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost", "field": "TourSpeed", "value": 4}},
	{"command": "script.setField", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost", "field": "TourOnStart", "value": false}},
	{"command": "play.start", "expect": {"/state": {"equals": "Play"}}},
	{"command": "play.pause", "parameters": {"paused": true}},
	{"command": "physics.stats", "expect": {"/waitingForMeshes": {"equals": false}, "/pendingOther": {"equals": 0}}, "until": {"seconds": 300}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/CapturesFound": {"equals": 6}, "/scripts/0/fields/Mode": {"equals": 1}, "/scripts/0/fields/Shot": {"equals": 0}}, "until": {"steps": 60}},
	{"command": "entity.find", "parameters": {"name": "Probe.Dawn.Yard"}, "bind": {"dawnYard": "/entities/0"}},
	{"command": "entity.find", "parameters": {"name": "Probe.Golden.Yard"}, "bind": {"goldenYard": "/entities/0"}},
	{"command": "entity.find", "parameters": {"name": "Probe.Dusk.Yard"}, "bind": {"duskYard": "/entities/0"}},
	{"command": "entity.find", "parameters": {"name": "Fire"}, "bind": {"fire": "/entities/0"}},
	{"command": "input.key", "parameters": {"key": "T"}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/Mode": {"equals": 0}, "/scripts/0/fields/Shot": {"equals": 1}, "/scripts/0/fields/Station": {"equals": 0}, "/scripts/0/fields/SunAzimuth": {"min": 139.9, "max": 140.1}}, "until": {"steps": 400}},
	{"command": "entity.get", "parameters": {"entity": {"$ref": "dawnYard"}}, "expect": {"/active": {"equals": true}}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/Shot": {"equals": 2}, "/scripts/0/fields/Station": {"equals": 1}}, "until": {"steps": 600}},
	{"command": "entity.get", "parameters": {"entity": {"$ref": "dawnYard"}}, "expect": {"/active": {"equals": false}}, "until": {"steps": 120}},
	{"command": "entity.get", "parameters": {"entity": {"$ref": "goldenYard"}}, "expect": {"/active": {"equals": true}}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/Shot": {"equals": 6}, "/scripts/0/fields/Station": {"equals": 2}, "/scripts/0/fields/SunAzimuth": {"min": 15.9, "max": 16.1}}, "until": {"steps": 1500}},
	{"command": "entity.get", "parameters": {"entity": {"$ref": "duskYard"}}, "expect": {"/active": {"equals": true}}},
	{"command": "component.get", "parameters": {"entity": {"$ref": "fire"}, "component": "PointLight"}, "expect": {"/values/Intensity": {"min": 150}, "/values/CastShadows": {"equals": true}}, "until": {"steps": 60}},
	{"command": "input.key", "parameters": {"key": "W", "steps": 10}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/Mode": {"equals": 1}}, "until": {"steps": 20}},
	{"command": "script.setField", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost", "field": "DistanceWalked", "value": 0}},
	{"command": "input.key", "parameters": {"key": "W", "steps": 120}},
	{"command": "play.step", "parameters": {"steps": 125}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/DistanceWalked": {"min": 2.5}, "/scripts/0/fields/HeightAboveGround": {"min": 1.5, "max": 1.7}}},
	{"command": "input.key", "parameters": {"key": "Tab"}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/Mode": {"equals": 2}}, "until": {"steps": 10}},
	{"command": "input.key", "parameters": {"key": "E", "steps": 60}},
	{"command": "play.step", "parameters": {"steps": 65}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/HeightAboveGround": {"min": 5}}},
	{"command": "input.key", "parameters": {"key": "P"}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/PhotoOpen": {"equals": true}}, "until": {"steps": 10}},
	{"command": "entity.find", "parameters": {"name": "TimeSlider"}, "bind": {"timeSlider": "/entities/0"}},
	{"command": "input.click", "parameters": {"entity": {"$ref": "timeSlider"}}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/PhotoHour": {"min": 12.7, "max": 12.8}, "/scripts/0/fields/Station": {"equals": 1}}, "until": {"steps": 10}},
	{"command": "entity.get", "parameters": {"entity": {"$ref": "duskYard"}}, "expect": {"/active": {"equals": false}}, "until": {"steps": 120}},
	{"command": "entity.find", "parameters": {"name": "PresetMedium"}, "bind": {"presetMedium": "/entities/0"}},
	{"command": "input.click", "parameters": {"entity": {"$ref": "presetMedium"}}},
	{"command": "editor.status", "parameters": {"sections": ["game"]}, "expect": {"/game/graphics/effective/Preset": {"equals": "Medium"}, "/game/graphics/sources/Preset": {"equals": "player"}}, "until": {"steps": 10}},
	{"command": "graphics.resetPlayerOptions", "expect": {"/reset": {"equals": ["Preset"]}}},
	{"command": "input.key", "parameters": {"key": "D3"}},
	{"command": "script.get", "parameters": {"entity": {"$ref": "outpost"}, "class": "Outpost"}, "expect": {"/scripts/0/fields/Bookmark": {"equals": 3}, "/scripts/0/fields/Station": {"equals": 1}}, "until": {"steps": 10}},
	{"command": "play.stop", "expect": {"/state": {"equals": "Edit"}}},
	{"command": "edit.undo"},
	{"command": "edit.undo"},
	{"command": "scene.info", "expect": {"/modified": {"equals": false}}},
	{"command": "project.export", "parameters": {"directory": "@EXPORT_DIR_JSON@"}, "expect": {"/excludedAssets": {"equals": []}}},
	{"command": "project.export", "parameters": {"directory": "@PRUNED_EXPORT_DIR_JSON@", "prune": true}, "expect": {"/assetCount": {"min": 1}}}
]
```

The walk is measured from shot 6's key at (-2, -2) toward the fire pit (13 m of open apron along the view); the Dusk lights'
check reads `Fire`, which D9 names uniquely and D4 lights only at Dusk.

The steps follow `ThirdPersonCommands.json.in` (graphics row at `:71-76`, export at `:85-86`). The first `input.key W` only
hands the tour over; the walk is measured afterwards. The click on the time slider's center sets 12.75 h (Golden). With the
walk cut (section 3), the steps from the second `input.key W` to the `HeightAboveGround` check go, and Tab starts the flight
from the tour. Each `expect` path is checked against the real results in S4 (`material.get`'s `values`,
`EditorAssetCommands.cpp:1110`).

### D14 Wiring

**`Samples/Samples.json`**, first entry (the flagship; the launcher shows the index's order):

```json
{
	"Directory": "Showcase",
	"Name": "High Desert Outpost",
	"Description": "A derelict fuel stop in a desert valley the day after rain, built from Poly Haven scans: a camera tour of six shots across dawn, golden hour and dusk, then walk, fly or open the photo mode.",
	"Picture": "Picture.png",
	"Budget": "Showcase"
}
```

`InstallEndToEnd.cmake`: `"Samples/Showcase/Showcase.stproj"` in the layout list (`:66-67`); `project.samples` expects
`/samples/0/id` "Showcase" and the others one index later (`:132-133`).

**CTests** (`StrataTests/CMakeLists.txt`), inside `if(TARGET StrataEditor AND TARGET StrataRuntime)`, after
`StrataEditor.OpenWorldRender` (`:904`) and before `StrataTests.PerfGPU.Samples` (`:905`), because that test's environment
names the showcase's directory:

```cmake
	# High Desert Outpost: its six probe stations are found, the tour (four times faster) reaches its sixth shot at dusk with
	# the fire lit, the walk covers 2.5 m at eye height over the ground, Tab flies, the photo mode's time slider snaps the
	# stations to Golden and its Quality row sets Medium (then reset); the exported games start the tour from the cooked pack,
	# the pruned one with every capture the inactive stations name.
	add_test(NAME StrataEditor.Showcase
		COMMAND "${CMAKE_COMMAND}"
			"-DEDITOR=$<TARGET_FILE:StrataEditor>"
			"-DSAMPLE_DIR=${PROJECT_SOURCE_DIR}/Samples/Showcase"
			"-DPROJECT_FILE=Showcase.stproj"
			"-DGAME=Showcase"
			"-DGAME_RUNS=$<IF:$<CONFIG:Debug>,pruned,full|pruned>"
			"-DCOMMANDS=${CMAKE_CURRENT_SOURCE_DIR}/Editor/ShowcaseCommands.json.in"
			"-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/Samples/Showcase/$<CONFIG>"
			"-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
			"-DEDITOR_EXPECT=Showcase: stations ready (found 6|Showcase: tour started|Showcase: shot 1|Showcase: shot 6|Showcase: walking|Showcase: flying|Showcase: photo mode on|Showcase: graphics preset Medium"
			"-DGAME_EXPECT=Loaded script module 'ShowcaseScripts'|Showcase: stations ready (found 6|Showcase: tour started|Showcase: shot 2"
			"-DGAME_FRAMES=<at least 1800: 1.5 times the frames the Debug pruned game takes to print 'Showcase: shot 2', measured in S7>"
			# A fresh copy has never built its scripts: opening its scene says so, and play.start builds them. <Each import warning
			# the S1 measurement shows, with its reason, e.g. covered_car's KHR_materials_specular (GltfImporter.cpp:49-52).>
			"-DALLOWED_MESSAGES=\\[warning\\] App: The scripts of 'Showcase' are not built yet"
			# <Only if the S4-S7 runs show it, as the Open World's line 874 does, with the same reason: the play copy cooks the 36
			# terrain tiles' and the props' colliders while the paused game's waits step it.>
			# "-DEDITOR_ALLOWED_MESSAGES=\\[warning\\] Strata: Physics: scene 'Outpost' starts simulating after waiting 10 s for meshes"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/Editor/SampleProject.cmake")
	set_tests_properties(StrataEditor.Showcase PROPERTIES LABELS "package" TIMEOUT 1800 RESOURCE_LOCK toolchain FIXTURES_SETUP StrataShowcaseExport)
	set(STRATA_SHOWCASE_DIR "${CMAKE_CURRENT_BINARY_DIR}/Samples/Showcase/$<CONFIG>")
	# In a window at 1280 x 720 for <S> s of game time: shot 2, the bus's side in the golden light, 7 s into its 14 (never in a
	# fade); the perf log shows the GPU at work in the passes the shots rest on.
	add_test(NAME StrataRuntime.ShowcaseWindowed
		COMMAND "${STRATA_SHOWCASE_DIR}/Build/Showcase${CMAKE_EXECUTABLE_SUFFIX}" --resolution 1280x720 --seconds <S>
			--screenshot "${STRATA_SHOWCASE_DIR}/Windowed.png" --perf-log "${STRATA_SHOWCASE_DIR}/Windowed.csv"
			--save-directory "${STRATA_SHOWCASE_DIR}/WindowedSaves")
	set_tests_properties(StrataRuntime.ShowcaseWindowed PROPERTIES LABELS "gpu" TIMEOUT 300 FIXTURES_REQUIRED StrataShowcaseExport
		FIXTURES_SETUP StrataShowcaseWindowed)
	add_test(NAME StrataRuntime.ShowcaseWindowedImage COMMAND StrataTests --strata-test-helper=check-image "${STRATA_SHOWCASE_DIR}/Windowed.png"
		--size 1280x720 --dominant red <half the share measured in S7> --margin 30)
	set_tests_properties(StrataRuntime.ShowcaseWindowedImage PROPERTIES LABELS "gpu" FIXTURES_REQUIRED StrataShowcaseWindowed)
	add_test(NAME StrataRuntime.ShowcaseWindowedPerf
		COMMAND StrataTests --strata-test-helper=check-perf-log "${STRATA_SHOWCASE_DIR}/Windowed.csv" any --nonzero GPUBusyMs
			--nonzero GPU.TAA --nonzero GPU.Reflections --nonzero GPU.Shadows --nonzero GPU.Aerial)
	set_tests_properties(StrataRuntime.ShowcaseWindowedPerf PROPERTIES LABELS "gpu" FIXTURES_REQUIRED StrataShowcaseWindowed)
	# The six shots rendered by a headless editor on the exported project: nothing pending, the dawn cascades reach 550 m, the
	# hut's interior a third or less of the yard's light (StrataTests/Editor/ShowcaseRenderCommands.json).
	add_test(NAME StrataEditor.ShowcaseRender
		COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:StrataEditor>"
			"-DARGUMENTS=--headless|--no-automation|--quit-after-commands|--frames|36000|--project|${STRATA_SHOWCASE_DIR}/Project|--commands|${CMAKE_CURRENT_SOURCE_DIR}/Editor/ShowcaseRenderCommands.json"
			"-DEXPECT=Command script finished (" -P "${CMAKE_CURRENT_SOURCE_DIR}/Editor/RunAndExpect.cmake")
	set_tests_properties(StrataEditor.ShowcaseRender PROPERTIES LABELS "gpu" FIXTURES_REQUIRED StrataShowcaseExport)
```

`<S>` is the time from the game's first log line to `Showcase: shot 2` in the Release windowed run (the log lines' timestamps:
a run that ends by itself keeps every fixed step, so its game time follows the clock), plus 7, rounded up, measured in S7 (the
median of three runs, recorded in the comment). `--seconds` counts game time from the game's start
(`RuntimeApplication.cpp:343-344`), which includes the loading scene and the stations' wait (at most 5 s), so a fixed 20 could
fall in the black of the fade between shots 1 and 2 (14 s after the tour starts) and fail the image check; the scene's own
`Time::GetElapsedTime()` would leave the loading scene out. `ShowcaseRender`'s script (below) starts with `{"command": "scene.loadStatus", "parameters":
{"wait": true}}`, as `OpenWorldRenderCommands.json:2` does, so the first capture does not spend its five seconds on a cold
project's loads.

`StrataTests.PerfGPU.Samples` (`:911-913`): `FIXTURES_REQUIRED "StrataOpenWorldExport;StrataShowcaseExport;StrataPerfResults"`,
`ENVIRONMENT "STRATA_OPEN_WORLD_GAME=...;STRATA_SHOWCASE_GAME=${STRATA_SHOWCASE_DIR}/Build/Showcase${CMAKE_EXECUTABLE_SUFFIX}"`.

After `StrataTools.LookDev` (`:489-496`), in the same `find_package(Python3)` branch and with the same `PythonMissing.cmake`
fallback:

```cmake
		add_test(NAME StrataTools.ShowcaseTerrain
			COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/Tools/GenerateShowcaseTerrain.py" selftest
				--committed "${PROJECT_SOURCE_DIR}/Samples/Showcase/Assets/Generated/Terrain"
				--stations "${PROJECT_SOURCE_DIR}/Samples/Showcase/Scripts/Stations.h"
				--work "${CMAKE_CURRENT_BINARY_DIR}/ShowcaseGenerators/$<CONFIG>/Terrain")
		add_test(NAME StrataTools.ShowcaseProps
			COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/Tools/GenerateShowcaseProps.py" selftest
				--committed "${PROJECT_SOURCE_DIR}/Samples/Showcase/Assets/Generated/Props"
				--work "${CMAKE_CURRENT_BINARY_DIR}/ShowcaseGenerators/$<CONFIG>/Props")
	set_tests_properties(StrataTools.ShowcaseTerrain StrataTools.ShowcaseProps PROPERTIES LABELS "tools" TIMEOUT 300)
```

Label `tools` only (no GPU), so CI's `-LE "gpu|perf"` runs them on every platform (`ci.yml:191-193`).

**`StrataTests/Editor/ShowcaseRenderCommands.json`** (gpu): for each shot, the station's steps (`setup --format script`),
then `viewport.capture {"position", "target", "fov", "width": 960, "height": 540, "quality": "high", "path": <a file under the
work directory>, "overwrite": true}` expecting `/pendingAssets` 0, `/reflectionProbesPending` 0, `/shadowFitSettled` true;
shot 1 again with `"debugView": "shadows"` expecting `/debugView/shadows/cascades/3/far` at least 550; at Golden the
`Measure.Yard` view with `"debugView": "lighting"` expecting `/debugView/lighting/surfaceEv100/p50` within +-0.5 of the S4
measurement E_yard, and the `Measure.Hut` view expecting at most E_yard - 1.58 (a third of the light, D15). Numbers set from
the S4 measurement, the measurement in the commit message. The runner's `expect` compares a result with constants only, so
E_yard is pinned: every adoption pass that changes the yard's light (R2's F0 and sun size, GI, `local-lights`, `volumetric`;
section 11) measures it again and updates both numbers in its commit.

**LookDev** (`Tools/LookDev/Shots.json`, appended): project `"Showcase": {"Sample": "Showcase", "Scene":
"Scenes/Outpost.stscene"}`; shots `showcase.scene` (`Camera: scene`), `showcase.shot1` to `showcase.shot6` and `showcase.macro`
with D5's `Position`, `Target`, `Fov`, the station's `Setup` (`setup --format lookdev`; none for Golden, the saved station) and
crops around D5's image positions (shot 2 `highlight` [0.42, 0.40, 0.58, 0.60] and `front` [0.18, 0.55, 0.34, 0.75]; shot 3
`streak` [0.30, 0.15, 0.42, 0.80]; shot 4 `glint` [0.48, 0.55, 0.62, 0.75], `bus` [0.25, 0.48, 0.48, 0.70], `tank`
[0.70, 0.45, 0.90, 0.70]; shot 5 `patch` [0.40, 0.40, 0.60, 0.85], `corner` [0.0, 0.2, 0.25, 0.6]; shot 6 `fire`
[0.40, 0.45, 0.56, 0.65], `lantern` [0.34, 0.38, 0.46, 0.54], `tank` [0.64, 0.38, 0.82, 0.58]; shot 1 `boulder`
[0.0, 0.6, 0.35, 1.0], `valley` [0.3, 0.25, 0.6, 0.45]); debug-view shots `showcase.shot1.view.shadows`,
`showcase.shot5.view.lighting`, `showcase.shot2.view.luminance`, `showcase.shot4.view.luminance` (`"AntiAliasing": ["taa"]`);
`showcase.shot3.noprobe` (shot 3's camera and crops, Setup `entity.setActive {"entityName": "Probe.Golden.Yard", "active":
false}`, `"AntiAliasing": ["taa"]`: D15's probe check).

**Live shots, for the highlights in motion** (decisions.md: every highlight judged with no AA, FXAA and TAA): `showcase.live.
shot2`, `.shot3`, `.shot4` and `.shot6`, each also as `.fxaa` and `.off` (12 shots, as the Third-Person's `tp.live.turn`,
`.fxaa`, `.off` are made), `"Camera": "scene"`, the shot's highlight crop, `"Live": {"AtFixedStep": 600, "Frames": 4}`
(LookDev starts play paused and steps, `LookDev.py:540-560`: the stations' wait takes at most 300 steps, so the frames fall
300-595 steps, 5-10 s, into the shot, clear of the 0.75 s fades of shot 3's 12 s and the others' 14 s), Setup
`script.setField {"entityName": "Outpost", "class": "Outpost", "field": "StartShot", "value": N}` and `component.set
{"entityName": "Post Process", "component": "PostProcess", "values": {"AutoExposureSpeed": 0, "AutoExposureLightSpeed": 0,
"MotionBlur": false, ...}}` with `AntiAliasing` / `AntiAliasingMethod` per mode, as those shots do (exposure held, so
adaptation does not count as shimmer). Captured with `--live --shots "showcase.live.*" --sizes 1920x1080`; `checkshots
--live` judges them (D6). The `Outpost` entity carries the script; `StartShot` is set in edit mode, before LookDev's
`play.start`, so it needs no undo (each shot reopens the scene with `force`).
`LookDev.py` needs no change: a `Sample` project is copied (`:449-451`), each shot reopens the scene with `force` (`:739`), so
a Setup never leaks into the next shot. The shots are appended, never changed later (the file's rule).

**Perf cases** (`SamplePerfTests.cpp`, suite `PerfGPU.Samples`), two `TEST_CASE`s modeled on `:56-113`: "The showcase's tour
at High" and "at Ultra": `STRATA_SHOWCASE_GAME` started once per attempt through `Tests::Perf::CheckBudgetsConfirmed` with
`--resolution 3840x2160 --seconds 90 --perf-log <file> --save-directory <dir> --quality high|ultra`; the frames measured
start at the first row with `LoadingAssets` 0 and `GPUBusyMs` above 0 and cover 70 s of `IntervalMs` (the 78 s tour's six shots;
the case fails, as the Open World's does at `:104-105`, if the run ends before the window does); metrics: the medians of
`GPUBusyMs` and `WorkMs` and the 95th percentile of `GPUBusyMs`. Names in section 9.

**Pictures.** `Picture.png`: the project's `.strata/Thumbnail.png` after saving the start scene, whose camera is at shot 2's
key (DG `:5399`). `Assets/Icon.png`: a 256 x 256 `viewport.capture` of shot 2 imported and set by `project.setGameInfo`.
`Screenshots/Shot1.png`-`Shot6.png`: 1280 x 720 captures of the six keys at High with TAA, each at its station.

### D15 The graybox and its exit

The graybox (S4) is the layout, the stations, the bakes and the six shots with first materials (the D8 values, no tuning).
**Exit: a LookDev capture of all six shots on main** (`python -I Tools/LookDev.py capture --build build/agent-showcase/bin/Release
--projects Showcase --shots "showcase.*" --sizes 3840x2160,1920x1080 --aa taa,fxaa,off`, the branch merged with main, then
the same with `--live --shots "showcase.live.*" --sizes 1920x1080` in a windowed editor), with these checks, before the L of
dressing starts:

| Shot | Check, and how it is measured | If it fails |
| --- | --- | --- |
| 1 | The reach of shadows: `showcase.shot1.view.shadows`' `cascades[3].far` at least 550 m and the ridge's shadow edge visible across the valley floor beyond 200 m; `uncoveredShare` reported | Raise `ShadowDistance` within the cascades' limit; else reframe so that the shadows that matter fall within reach and aerial perspective carries the rest (plan 4.2) |
| 2 | `checkshots`: the highlight within 40 px of the computed point at 1920 x 1080; it reads as metal by eye | Turn the bus or move the station (both through the generator); check S1's metal channel finding; lower BusPaint's roughness factor |
| 3 | `checkshots`: the streak's column within 30 px; the ground and its neighbors show in the tank: the `streak` crop's mean differs by at least 8 codes between `showcase.shot3` and `showcase.shot3.noprobe` (p07's method; proposed threshold) | Move or resize the probe box; check Q6 (a box whose floor lies 12 m under the ground projects the ground's reflection wrongly) |
| 4 | The bus and tank in puddles, with no comb along straight edges, no sky wedge, no misplaced patch (crops at 4x) | R1 if it has landed. If not, write the fallback from this capture: framing or geometry with no long straight edge across a puddle (a drum row before the bus's sill, puddles shaped away from the bus's bottom edge), roughness of large wet areas not raised (water stays 0.02-0.06) |
| 5 | A dark hut from one box probe: the `Measure.Hut` view's `lighting` p50 at least 1.58 EV below the `Measure.Yard` view's (a third of the light, plan 4.2, proposed; the CPU model's reference is about a sixth) | A smaller box, a narrower door, a bake at the right station; then unshadowed fill lights |
| 6 | Lantern and fire read at dusk; the `fire` and `lantern` crops of the 4K TAA still clip (code 255 in any channel) on at most 0.5% of their pixels; metal catches firelight (the `tank` crop brighter in `showcase.shot6` than in a scratch capture with `Fire` inactive) | Hold the shot at dusk, not night, until `reflections` R2 fixes the sky's bands; lower the Dusk exposure compensation |
| 2-4, 6 | **Highlights in motion** (decisions.md): `checkshots --live` on the 12 live shots; at the graybox the numbers are recorded (first materials, SSR before R1); at S6's exit every one passes (mean at most 1.0, p99 at most 3.0) in all three modes | In this order: WeatheredSteel's `NormalScale` 0.6 to 0.3, its roughness floor 0.22 to 0.30 (the generator's constant, maps regenerated), BusPaint's roughness factor up by 0.05 steps while the highlight still reads (`p11`); what still fails is named on the owner's sheet with its numbers and goes to `reflections` |

Also at the graybox: **S0.3 on the showcase's traffic** (plan 4.2 step 4; step0.md 12: "the showcase's graybox checks
`c_NearNowShare` and `c_GrowSeconds` against its traffic"): with lane Render on main, in a windowed Release editor, the tour in
the Game view at real time and a 60 s walk; `editor.perf {"frames": 1}`'s `gameRenderStats.shadowFit.changes` (S0.3 adds it,
step0.md 7.4) read once a second with the `Outpost` script's `Shot`. Pass: no change in the middle 10 s of any dolly (changes at
the cuts are allowed), and at most 2 in the walk while it passes the bus and the tank. A change in a dolly goes to S0.3's owner
with the shot and the numbers; a change of either constant is theirs (step0.md 12). **The first perf reading** (information:
the machine is loaded): `renderStats.gpuMs` of the six keys at 3840 x 2160 High and Ultra from the LookDev run's `stages.csv`,
triangles and draws. If a key exceeds 8.3 ms at High: `LODCount` 8 on the rocks, fewer cliff instances, then a shorter Dawn
`ShadowDistance`. **The owner's first sheet**: the LookDev index page, the six 3840 x 2160 TAA pictures and the live shots'
numbers go to the orchestrator (item 4's reading is answered: no question on it).

## 5. Steps: design, tests, docs, exit

### S1 Freeze, fetch, verify, measure, credit (S)

1. The fetch and import of D1 into `Samples/Showcase`, made by `project.create` (D9); `.gitignore` as written.
2. The metal channel check (D1), recorded.
3. **The import time** (plan step 2's measurement, done first because it decides the CTest's form): copy the project with its
   scans and no `.strata`, open it in a `--no-gpu` editor, poll `editor.status {"sections": ["assets"]}` until `importing` is 0;
   Release and Debug, three times each, median, under `Tools/PerfLock.py --label showcase-import` (a measurement: other agents'
   builds would lengthen it). Release runs on `.worktrees/wave8-base`'s editor during step 0; Debug waits for the first build.
   Two limits bind the CTest: its 1800 s, and, tighter, the sample runner's cap of 54,000 editor frames for the whole command
   script (15 minutes at its 60 frames per second, `SampleProject.cmake:43-45`), which the import, the play and both exports
   share. And tests hold on CI's slowest machines (AGENTS.md, Testing): CI's Windows (Debug) job runs package tests on a
   4-way runner inside a 120-minute job (`ci.yml:32-37`, `:62`, `:193`), far slower than this i7-13700K at BC7 (unmeasured). So the import
   decides the first form, and the **whole run** decides the last: the CTest's own time in Debug here (S4, S5), and the
   Windows (Debug) job's time for `StrataEditor.Showcase` on `ci/wave8-showcase`, pushed right after S4's commit to read it
   early. Decision (Q4):

   | Measured | Do |
   | --- | --- |
   | Debug fresh import here at most 300 s, the whole run here at most 600 s and on CI's Windows (Debug) job at most 750 s | Nothing |
   | Above any of them | Every scan texture outside the hero sets (asphalt, rusty_metal_03, green_metal_rust, beige_wall) gets `{"Quality": "Fast"}` in its import settings (`TextureImporter.cpp:389`: BC1 instead of BC7 for opaque color; committed in its `.meta` through `asset.setImportSettings`); measure again |
   | Still above | `StrataEditor.Showcase` and its fixtures take `CONFIGURATIONS Release Dist` with the measured reason in a comment; the Release runs cover the same scripts. If Release here exceeds 600 s or CI's Release jobs 750 s, `SampleProject.cmake` gains an optional `MAX_FRAMES` (default 54,000, the others unchanged), a shared-file change named in the landing |

4. `CREDITS.md`, the notices row, `Samples.json`, `InstallEndToEnd.cmake`, a README stating what the commit holds, and
   `Picture.png` from the project's thumbnail (an empty scene's thumbnail is replaced in S4). `Samples.Content` passes.

Tests: `Samples.Content` (every listed sample; the budgets). Exit: the import times, the metal findings, the scans' sizes in
the commit message.

### S2 Terrain generator (M)

D6 entire: the generator, `check`, `stations`, `selftest`; `terrain` into `Assets/Generated/Terrain/` (the editor imports it;
`.meta` files are the editor's); `StrataTools.ShowcaseTerrain`. The terrain placed: `Scenes/Outpost.stscene` made with
`scene.new` and `scene.saveAs`, `project.setStartScene`, `prefab.instantiate` of `Terrain.glb`, `Apron.glb`, `Water.glb`,
`Mud.glb` with factor materials, the tiles' colliders. `Terrain.glb` imported with `GenerateLODs` false (simplified tiles would
open seams, `GenerateIslandTerrain.py:15`).

Tests: `StrataTools.ShowcaseTerrain` fails on a hand-edited GLB, a station row that differs, a rule that fails. Exit: `check`
green, the sizes, a capture of the valley from the rim at Golden.

### S3 Props generator, materials, prefabs (M to L)

D7 and D8 and the prefab half of D9: `props` into `Assets/Generated/Props/`; the `materials` script run; the `prefabs` script
run; `StrataTools.ShowcaseProps`. Fallback if the bus reads as a box at the graybox (risk table): containers built from
`container_side` (+1.59 MB, B-list) with bare-metal factors keep the sheet shot; the generator's bus stays for a later update.

Tests: `StrataTools.ShowcaseProps`. Exit: each prefab captured alone at 1024 x 1024 under GoldenHour in a scratch scene.

### S4 Graybox (M)

The `layout` script into the scene; the six probes (D10) baked; `Stations.h`, `Outpost.h/.cpp` with the station switch,
`CapturesFound`/`CapturesResident`, the first tour (shots and fades, no walk); the first `ShowcaseCommands.json.in` (through
the `Shot 6` check) and `StrataEditor.Showcase`; the LookDev shots of D14 appended (stills and live); `checkshots` and
`checkshots --live`. Then D15's exit, the first perf reading, the S0.3 check and the owner's first sheet. Push
`ci/wave8-showcase` after this commit to read CI's times for `StrataEditor.Showcase` early (S1 step 3's table).

Tests: `StrataEditor.Showcase` (first form), `StrataTools.*`. Exit: D15.

### S5 Scripts (M)

D11 and D12 in full: the walk (landing form), flight, photo mode, bookmarks, captions, loop, `Flicker`, the loading scene,
input actions; `ShowcaseCommands.json.in` complete (D13).

Tests: `StrataEditor.Showcase` (full form) in Debug and Release. Exit: the regression green; a 60 s look in the editor's Game
view at the monitor's frame rate (the walk smooth, no pops outside the photo mode's station snaps).

### S6 Dress and tune (L)

After R1 is on main and the editor is rebuilt (or with the S4 fallback if R1 is not on main when S5 and S7 are done):

- Materials (D8's proposed values) tuned by `material.set` with captures at the keys, **highlights first** (decisions.md): the
  sun's streak on the letterboard's painted and bare sections (the bus paint's roughness factor until the highlight reads on
  the dents, p11), the tank's curved streak, the apron's glint column, the lantern's glint at dusk; then the reflections (the
  water's roughness within 0.02-0.06 and the apron's damp factor, the world in the tank, the tank paint). Each tuning step
  is judged on the live shots (`checkshots --live`) in all three AA modes, not on stills alone.
- Dressing: drums, tyres, brush, rocks at the cliff foot and on the rim, the hero boulder's framing, the covered car, the ladder's
  lean; joins between materials hidden by rocks, tyres and drums (the fallback without blending, plan 4.2); hand placements with
  `asset.place` if S0.7 is on main, else `prefab.instantiate` and `entity.dropToGround`.
- The station values of D4 by look (fog, Dusk's exposure, local lights).
- Rebake after the last change the probes see; LookDev capture; retake the exit checks.

Tests: the regression's material expectations updated to the tuned values. Exit: D15's checks still green, the highlights'
shimmer row passing in every AA mode (or its residue named on the sheet, D15), the sheet retaken.

### S7 Wiring (M)

E3 (D2) with its `Samples.Content` case; the CTests of D14; `ShowcaseRenderCommands.json`; the perf cases and `Budgets.json`
entries; the Icon, Picture and Screenshots; `project.setGameInfo`; README and CREDITS final; docs (section 7). Full Debug ctest
with the GPU suites under the validation layer, Release ctest without perf, the perf cases once in Release (information), and
`ci/wave8-showcase` green on every job.

### S8 Land, then adopt (S each)

Plan section 7.3: landing 1 merges `reflections` R2 and `physics-timing` if verified, then the showcase. Then one adoption pass per
stream (section 11), each a commit with its retaken pictures and the regression green.

## 6. Tests that must fail without the change

| Test | Fails without |
| --- | --- |
| `Samples.Content` "Every sample of the repository..." | Any credit, notice, link or budget of the sample (the existing case covers the new directory) |
| `Samples.Content` "A showcase sample may hold six screenshots in 8 MB and others three in 3 MB" (new: six 1.2 MB PNGs in a showcase pass, the same in a standard sample fail with "Screenshots/ holds 6 files (at most 3)", a showcase with seven or 8.1 MB fails) | E3 |
| `StrataTools.ShowcaseTerrain`, `StrataTools.ShowcaseProps` | The generators; a hand edit of a generated file; drift between `Stations.h` and the generator; a layout that breaks a design rule (rule 6: the first draft's apron formula fails it); a tilted letterboard |
| `StrataEditor.Showcase` | The scripts, the stations, the walk, the photo mode, the pruned export keeping the inactive stations' captures (`CapturesFound` 6 in the pruned game, `AssetDependencies.cpp:36`) |
| `StrataEditor.ShowcaseRender` | A probe or asset left pending, shot 1's reach, the hut's darkness |
| `StrataRuntime.ShowcaseWindowed*` | A game that renders nothing or not the passes listed |
| `StrataEditor.Install` | The showcase's project in the installed layout and its place in `project.samples` |
| `PerfGPU.Samples` showcase cases | Record only (`Provisional`) |

No new doctest suite prefix: `Samples.*` and `PerfGPU.Samples` exist, so `StrataTests/CoreShards.json` does not change.

## 7. Documentation

- **`S/README.md`**: title and paragraph; the hero screenshot; "What it shows" (a table as the Third-Person's: the physical
  atmosphere at three baked times of day; metal and wet ground: painted and bare sheet steel, the weathered tank, opaque water at
  roughness 0.03 reflected by SSR and probes; six probe stations switched by `SetActive`; 22 scans with LODs; generated terrain and
  props; cascaded sun shadows to 600 m at dawn; shadowed point lights for the fire and the lantern; height fog and aerial
  perspective; depth of field in the photo mode; the players' graphics options through `Graphics::Options()`; input actions; the
  export's loading scene), each with its files and https links to the manual; Controls (keyboard, gamepad); Open it; How it was
  made (the generators' commands, the bake procedure of D10, the regression); Credits (pointer to CREDITS.md); Known limits (no
  grass: masked grass vanishes by 30 m at 4K; no people; reflections snap to the nearest of three baked times in the photo mode;
  at Medium, screen-space reflections need TAA (Checkerboard traces nothing with FXAA or no anti-aliasing: plan 1, the named
  exception); no sound).
- **DG "Samples"** (`:5389-5427`): the showcase class's screenshot allowance (six, 8 MB); `:5427` "showcase scans 2K" becomes
  "showcase scans 1K, 2K only for the surfaces its key shots frame within 10 m"; a subsection "The showcase" (about 40 lines): the
  generators and what they commit, `selftest` and determinism, stations as probe entities and the bake procedure, regenerating
  the terrain or props (write in place, the editor reimports and keeps handles; then the `layout` script only on a fresh scene),
  the regressions, the LookDev shots, the perf cases, and the rule that a change to the showcase's look retakes its six shots.
- **DG "Look-dev harness"** (`:628`): the Showcase project and its `showcase.*` shots, the live highlight shots and
  `checkshots` / `checkshots --live` (what they measure, their bounds).
- **DG "Perf tests"** (`:428`): the showcase's metrics.
- **Manual** `Docs/Manual/Lighting.md`: a short section "Times of day with baked probes" (a probe entity per station, switched with
  `SetActive`, priorities raised during the switch, captures kept with `Assets::GetLoadProgress`), pointing at the showcase. A new
  section, so it merges beside `reflections`' changes to the reflections section.
- **`README.md`** (root): five samples (`:79-83`, `:131-134`, `:162`).
- **`AGENTS.md`**: the `Samples/` row (`:42`) lists `Showcase` first; one digest bullet in "Asset pipeline", after the sample-assets
  bullet: "- The showcase (`Samples/Showcase`) is the presets' reference scene: its terrain, props and stations change only through
  `Tools/GenerateShowcase*.py`; a change to its look retakes its shots ([Samples](Docs/DeveloperGuide.md#samples))." (234
  characters, 246 with the row's `` `Showcase`, ``; within plan 4.1's 250).
- **`Docs/Roadmap.md`**: the showcase's line of the wave 8 entry S0.12 writes (S0.12 removes today's "Wave 8 starts with
  `showcase`" paragraph, `:1425-1434`, step0.md section 9) rewritten as landed: what it shows, what fell below the cut; "Later":
  grass with coverage-preserving alpha, people, the B-list, sound, supersampled stills if `showcase-engine` did not land. The
  sunlight thrown off a sheet onto nearby surfaces is the roadmap's "Later" item after `radiance-cascades` (decisions.md),
  not a question to the owner.
- **Skill** `strata-make-a-game` "Worked examples": a High Desert Outpost bullet (generated terrain and props, stations as probe
  entities, a photo mode on `Graphics::Options()`).
- **Release notes** (0.8.0): "New sample: High Desert Outpost (`Samples/Showcase`), a derelict fuel stop in a desert valley the day
  after rain, made through the editor from 22 Poly Haven CC0 scans and generated terrain and props: an 80 s tour of six shots at
  three baked times of day (the sun on a bus's sheet steel, an oil tank at low sun, a wet apron), a first-person walk, free flight
  and a photo mode." and "Samples: a showcase may hold six screenshots in 8 MB." No default changes.

## 8. Acceptance numbers, and how each is measured here

| What | Target | How |
| --- | --- | --- |
| Source assets | at most 80 MB (expected 70.6) | `Samples.Content` |
| The showcases' binaries | at most 85 MB (expected at most 78.9) | `Samples.Content` |
| The six shots' checks | D15, all passed or their fallback written | The graybox's LookDev run, `checkshots`, `StrataEditor.ShowcaseRender` |
| Highlights in motion, shots 2-4 and 6, no AA, FXAA and TAA | second difference of 32 px block means: mean at most 1.0%, p99 at most 3.0% (proposed) at S6's exit | `checkshots --live` on D14's live shots, 1920 x 1080, this machine's windowed Release editor |
| The owner's three shots | read as real on the owner's sheet (plan 1.1) | The owner's answer to the sheet |
| Regression | `StrataEditor.Showcase` green in Debug and Release, every platform of CI | ctest, `ci/wave8-showcase` |
| GPU busy, tour median, 3840 x 2160 | at most 8.3 ms at High, 14 ms at Ultra (`Provisional` targets) | `PerfGPU.Samples` showcase cases, Release, alone under PerfLock; information until the reference memory |
| Fresh import and the whole regression | import at most 300 s in Debug here; the whole `StrataEditor.Showcase` at most 600 s here and 750 s on CI's Windows (Debug) job, so it fits the runner's 54,000 frames (900 s) and the CTest's 1800 s on CI's slowest machine | S1 step 3 (three runs, median, under PerfLock); the CTest's time here; CI's log on `ci/wave8-showcase` from S4 on |
| Walk smoothness | no visible stepping at 60-1000 fps in the Game view | By eye in S5 (landing form); `physics-timing`'s own metric after adoption |
| Generators | deterministic, committed outputs equal | `StrataTools.Showcase*` on every CI platform |

## 9. Perf

**New metrics** (`Budgets.json`, after `:202`, `Provisional`, plan D4):

```json
"PerfGPU.Samples.ShowcaseHigh.GPUBusyMedianMs": { "Unit": "ms", "Provisional": true, "Target": 8.3 },
"PerfGPU.Samples.ShowcaseHigh.GPUBusyP95Ms": { "Unit": "ms", "Provisional": true },
"PerfGPU.Samples.ShowcaseHigh.FrameWorkMedianMs": { "Unit": "ms", "Provisional": true },
"PerfGPU.Samples.ShowcaseUltra.GPUBusyMedianMs": { "Unit": "ms", "Provisional": true, "Target": 14.0 },
"PerfGPU.Samples.ShowcaseUltra.GPUBusyP95Ms": { "Unit": "ms", "Provisional": true }
```

Measured and recorded, never checked, until the reference memory measures them. The 3840 x 2160 window on this machine's
3840 x 2160 display at 150% is *unverified* (`--resolution` "opens a window of exactly this many pixels",
`StrataRuntime/src/RuntimeApplication.cpp:341`); the first hour checks it, and if the window cannot have that size the cases
fail saying so and the orchestrator decides (Q7).

**How the stream A/Bs.** The showcase adds no stage of its own; it is measured whole and its adoption passes are A/B'd: the
exported game before and after a pass, `python Tools/PerfLock.py --label showcase -- <game> --resolution 3840x2160 --seconds 60
--perf-log <file> --quality high`, interleaved A, B five times each, per-stage medians from the logs reported as ratios to
`GPU.Opaque` in the same frames (other slots' GPU suites can triple stage times, plan 4.1), information in the pass's commit.
No campaign, no `PerfStability.py`, nothing measured against `Budgets.json` on the temporary memory.

## 10. What the stream shares, and how merges resolve

| Shared file | This stream | Others | Resolution |
| --- | --- | --- | --- |
| `Samples/Samples.json` | The first entry | None planned | Additive |
| `SampleContent.h/.cpp`, `SampleContentTests.cpp` | E3 | None planned | - |
| `Tools/LookDev/Shots.json` | The `Showcase` project and `showcase.*` shots | `reflections` (`lookdev.sheet`, `.wet`, `.tank`), `radiance-cascades` (its `gi` shots) append | Append-only: keep every block, in landing order |
| `Tools/LookDev.py` | No change | `radiance-cascades` may add settling | - |
| `ThirdPartyNotices.md` | One row | None planned | Additive |
| `StrataTests/CMakeLists.txt` | The showcase block before `:905`, `PerfGPU.Samples`' fixtures and environment, two `StrataTools` tests | Streams add GPU and runtime tests elsewhere | Line-level |
| `StrataTests/Perf/Budgets.json` | Five `Provisional` lines after `:202` | Every stream appends its own | Line-level |
| `StrataTests/Editor/InstallEndToEnd.cmake` | The project line and the order | None planned | - |
| `StrataTests/Editor/SampleProject.cmake` | Only if Q4's last remedy is needed: an optional `MAX_FRAMES` (default 54,000) | None planned | Additive; named in the landing |
| `AGENTS.md`, `README.md`, DG, Roadmap, the skill | Section 7 | Their own sections and bullets | Section-level; AGENTS.md's room is S0.1's |
| `Docs/Manual/Lighting.md` | A new section | `reflections` owns its reflections section | Different sections |
| `Samples/Showcase/*` after landing 1 | The sample | Every adoption pass (section 11) and section 7's GI adoption | Through commands; binary conflicts (pictures, captures) take main's and retake (plan 4.1) |

This stream changes no engine file, shader or ABI, so it never conflicts with step 0's lanes or the rendering streams in code.

## 11. Adoption: every stream's fallback, and its pass

| From | Used for | The landing's form (fallback) | The adoption pass when it lands |
| --- | --- | --- | --- |
| S0.2 contact shadows off | Every shot | Graybox on main as it is | Shots retaken; AO carries the grounding (audit section 6) |
| S0.3 cascade fit | Shot 1's reach, the dollies | As it is | D15's S0.3 check repeated; shots retaken |
| S0.7 ray and placement | Dressing | `prefab.instantiate`, the generator's heights, `entity.dropToGround` | Later hand dressing with `asset.place` |
| S0.10 Slang | Nothing (no shaders here) | - | The LookDev A/B of the showcase's shots is one of its gates' inputs if it lands after the showcase |
| `reflections` R1 | Shots 2-4 | If R1 is not on main by S6: sheets at roughness 0.18 or more under `ScreenSpaceReflectionsMaxRoughness` 0.15 for shots 2-3; shot 4 as D15's fallback (framing without long straight edges across puddles; water stays 0.02-0.06, so SSR stays on it with every defect, plan 4.2) | `ScreenSpaceReflectionsMaxRoughness` back to 0.5, materials retuned, rebake, retake |
| `reflections` R2 | **Highlights** (its step 7, first in R2 by decisions.md: the sun's size in specular, small lights' highlights, a highlight-only source radius for point and spot lights), water's F0, the night sky, sharper probes | Punctual lantern and fire highlights; IOR 1.5 water with a darker base; dusk, not night; 128 probes | The highlight-only source radius, once R2 delivers it (OQ5), on `Fire` (about 0.25 m) and `LanternA.Light`, `LanternB.Light` (about 0.04 m; the property's name is `reflections`' choice), shots 2-4 and 6 judged again with `checkshots --live` in all three AA modes (a highlight that shimmers fails); water IOR 1.33 with a lighter base; `PhotoHourMax` 19.5 and `NightAfterDusk` true; the station switch's worst frame measured at 256 (A/B); E_yard of `ShowcaseRenderCommands.json` measured again (D14); retake |
| `radiance-cascades` | Shots 5-6, shadow sides | The Hut probe's 9 coefficients; unshadowed fill lights only if D15's shot 5 check asks | Only after the owner's live look (plan 7.6-7.7): `GlobalIllumination` on in the Post Process, check flicker in shot 6 and the hut, the hut's darkness check and E_yard measured again (bounce brightens the hut on purpose: the bound moves with the owner's look, recorded), retake |
| `local-lights` | Shot 6 | Two shadowed lights (works today: 16 at High), at 0 cd outside Dusk (D4) | Two yard work lights (spot lights with shadows on poles at the bus and tank) at Dusk, 0 cd at the other stations; shot 6 judged with no AA and FXAA (fixed noise) on its live shots |
| `physics-timing` | The walk | The per-frame ground-following walk | `UseController` true, the regression's walk on the controller, judged at 60-1000 fps |
| `volumetric` | Shots 1 and 5 | `HeightFog` haze, `StartDistance` 80 (no fog into the hut), sun patches instead of shafts, no fake cones | Shafts through the hut's door at Golden, valley mist with shadows at Dawn |
| `showcase-engine` E2 | Ground at the feet | Separate meshes per material (apron, mud, ground) with hard edges hidden by props | The generator writes the two masks (G wetness, B puddles; +2.0 MB), the apron and ground take the blended material, the mud meshes go |
| `showcase-engine` E1 | Stills | Native 4K stills | Screenshots and the owner's sheet with `supersample: 2` |
| `editor-ux`, `animation-graph`, subsurface | Nothing | No people | A later update if a realistic body is found (wave 9) |

*As built (the reflections' adoption), R2's row:*

- Highlights: `SourceRadius` 0.25 m on `Fire` and 0.04 m on `LanternA.Light` and `LanternB.Light`, as proposed (the
  stations' steps write them; `StrataEditor.Showcase` holds the fire's to 0.2-0.3 m and both lanterns' to 0.03-0.06 m).
- `checkshots`, on stills at 1920 x 1080 with TAA and on four live frames per shot in the three AA modes, all captured
  after the last bake: shot 2's highlight lies 14.3 px from its computed place (at most 40), shot 3's streak 12.0 px (at
  most 30), the shell beside it differs by 11.3 codes with and without the yard's probe (at least 8), and shot 6's fuel
  tank is 5.7 codes brighter with the fire (at least 2). Live, the twelve crops (shots 2, 3, 4 and 6 with TAA, FXAA and
  no anti-aliasing) differ between frames by at most 0.08% in the mean and 0.73% at p99 (limits 1.0 and 3.0): no
  highlight shimmers.
- Water has IOR 1.33 and a lighter base, not a darker one (D8's note).
- The night after dusk is not adopted. `PhotoHourMax` stays 19.0, and `NightAfterDusk`, which landing 1 listed and did
  not build, stays unbuilt. R2 took the bands out of the night sky, which the row waited for, but the day's course
  (`SampleDay`) has no values below the horizon, and dusk's probes hold dusk's sky: a night needs a station with probes
  of its own and a look to judge (the release notes' known issues, the roadmap's "Later").
- The station switch with the probes' cubes at 256 against 128 texels: the exported Release game's tour at 2560 x 1440,
  High with the one row set by the players' options (`Preset` Custom), three runs of each in turns and one of High
  alone, under `PerfLock.py` on a machine other agents worked on. Device memory differs by 49-51 MB from the start (2198
  against 2147-2149 MB), so the row took effect; at each switch it grows by about 70 MB at either size. The main
  thread's worst frame at the switch to Golden took 5.37, 6.16 and 6.91 ms at 256 and 6.23, 5.65 and 5.41 ms at 128
  (High alone 4.72): five or six frames of `Assets.Finalize`, 1.2-4.6 ms each, with loads queued at the cut to shot 2.
  To Dusk it took 5.53, 9.02 and 4.46 ms at 256 and 5.45, 4.50 and 5.36 ms at 128 (High alone 11.16): `Render.Submit`
  of the switch's own frame (3.8-10.6 ms), in which the two lights' twelve shadow views are drawn, then two frames of
  finalizing. A frame's median is 0.65-1.05 ms. The GPU's longest busy time in the 1.5 s after the switch: Golden
  6.2-7.2 ms at 256 and 6.4-7.8 ms at 128 (the second before: 5.4-6.0), Dusk 7.6-11.1 and 7.8-10.3 ms (before:
  6.9-8.0). The runs differ more than the sizes do (away from the switches every run has frames of 5.1-9.5 ms of
  work), and neither long frame changes with the row: it costs memory and no hitch at the switch that this measurement
  can show. `GPU.Reflections`, the conversions' stage, steps to the new station's level at the switch (0.27 to 0.37 ms
  at Golden, 0.26 to 0.59 ms at Dusk). Of the sixteen frames after it a few stand above that level, at most by 0.2-1.9
  ms in the runs at 256 and by 0.3-1.2 ms in those at 128, and frames up to 0.7 ms above the level stand before the
  switches too: the conversions do not stand out from the stage's scatter.
- E_yard, measured again: the yard's surface EV100 is 14.03 at 960 x 540, the render test's size (13.98 at 1920 x 1080),
  so `ShowcaseRenderCommands.json` keeps 14.0 +-0.5; the hut's dark corner reads 8.74, 5.29 EV below (at least 1.58).
- Dusk's sun stands at azimuth 16, not 32 (D3's rule 7, D4's note), and a bake takes eight passes, not two (D10's
  note). What the sun does not reach at dusk is darker than in landing 1's pictures: the release notes' known issues
  give the numbers.

## 12. Risks

| Risk | Mitigation |
| --- | --- |
| The landing slips with no stream to blame | Nothing above the cut waits for a stream (section 2.3); S6 has a start-by rule; the cut line |
| The generated bus looks like a box | Close shots show the panel, not the silhouette; rails, seams, arches and boards; the containers fallback (S3); the owner's first sheet early |
| A painted scan has no metal and the "metal sheet" reads as paint | Bare panels and the tank use `WeatheredSteel`, metal by its own map; S1's check per set |
| Shot 4 on SSR's defects if R1 is late | D15's fallback written from the graybox capture; S6's start-by rule; plan 4.2's table |
| A fresh import exceeds the package test's time or the runner's 54,000-frame cap (`SampleProject.cmake:43-45`), on CI's slowest runners above all (the Windows Debug job's 120 minutes, `ci.yml:62`) | S1's measurement and its decision table (Q4), judged on the whole run and on CI's own time, read from S4 on |
| Highlights shimmer without TAA (scan normal maps and the steel's roughness noise near a texel a pixel; normal-map-aware filtering is below `reflections`' cut) | The live shots in three AA modes and `checkshots --live` from the graybox on; D15's fallback order (normal scale, roughness floor) |
| A tilted or curved panel moves a computed highlight off its check | `stations` solves the mirror condition with the generated meshes' normals; the letterboard is vertical (D7); `selftest` checks it |
| Steel reads as CG (one F0 over an 8 m tank, `p04`) | Rust varies color, metalness and roughness from one mask (D7's two maps); the probe brings the world into it (`p07`) |
| Step 0's lanes hold every build slot | The sample agent works on `.worktrees/wave8-base`'s editor and builds first in slot 4 after lane Render (Rules) |
| The hut is not dark with one box probe | D15 shot 5's numeric check and its fallbacks |
| The vista's shadows do not reach 600 m | D15 shot 1's check and reframe rule |
| Generated files differ across platforms | No transcendental functions in the height function; quantized attributes; PNG compared by pixels; `selftest` on every CI platform |
| The station switch pops visibly in the photo mode | Priorities raised during the switch (D10); a pop at a snap is accepted (plan 4.2) |
| `Assets::GetLoadProgress` never reaches 1 headless | `CapturesFound` drives the regressions; residency waits at most 5 s (D10); first hour checks it |
| Import warnings fail the CTest (e.g. `covered_car`'s `KHR_materials_specular`, not in `GltfImporter.cpp:49-52`) | Each warning S1 sees is listed in `ALLOWED_MESSAGES` with its reason, as the Third-Person's morph target line |
| The 4K perf window is refused on this display | First hour; Q7 |
| A later stream changes the look and nobody retakes | The AGENTS.md bullet; section 7 step 5 of the plan retakes in landing 2 |

## 13. Open questions and the decisions recommended

- **Q1 `rusty_painted_metal` at 1K.** Recommended yes (D1): it is corrugated and roofs the hut; the bus takes `rusty_metal_03`.
  Saves 4.45 MB, adds nothing from the B-list. Alternative: keep 2K (+4.45 MB, within the class).
- **Q2 A committed fetch script.** Recommended no: the A-list table here and `CREDITS.md` name every source; the B-list's later
  fetch reuses a scratch script. A committed tool would be a downloader in `Tools/`, which holds generators and helpers only.
- **Q3 The order in `Samples.json`.** Recommended first (the flagship), with the install test's indices shifted. Alternative: last,
  touching only the layout line.
- **Q4 The package test's time.** Decided by S1's measurement and the whole run's times here and on CI (table there).
  Recommended order of remedies: `{"Quality": "Fast"}` outside the hero sets, then `CONFIGURATIONS Release Dist` for the CTest,
  then `MAX_FRAMES`. Not recommended: a smaller copy for the test (the test would not play the real sample).
- **Q5 The terrain's size.** 3.7 MB at float positions; if the measured `Terrain.glb` exceeds 5.5 MB, positions become 16-bit with a
  node scale per tile (KHR_mesh_quantization, supported, `GltfImporter.cpp:49-52`), saving about 0.9 MB.
- **Q6 The Yard probe's box projection.** Recommended on (the yard's faces approximate its surroundings for the tank's reflection);
  if the graybox shows the misplaced patch of `p14` in shot 4 and R1 has not fixed it, off, decided by look and recorded. Note
  the box is centered on the capture point (`RendererComponents.h:150-166`): with the capture at 1.6 m and half-height 14 m its
  floor lies 12.4 m under the ground, so box-projected ground in the tank's lower half is placed too far; shot 3's probe check
  (D15) looks at exactly that.
- **Q7 The perf cases' 4K window.** If this display refuses it: the cases measure at 3840 x 2160 in exclusive fullscreen if the runtime
  offers it (*unverified*), else at 2560 x 1440 with the metric names saying so; the orchestrator decides.
- **Q8 The walk's form at landing.** The per-frame form unless `physics-timing` is on main before S7; then the controller form ships
  and the regression covers it.
- **OQ1-OQ5** (the reviewer's, for the orchestrator) are in section 17.

## 14. Left out

- Grass and other cutouts (coverage-preserving alpha is `showcase-engine`'s E6, out; plan section 9).
- People and animals (wave 9 if a realistic CC0 or CC-BY body is found).
- Sound: no ambience in the budget; a generated wind loop could come in a later update.
- Script screenshots (E5) and supersampled stills (E1: `showcase-engine`, below its cut).
- Run-time probe capture and a fully free time of day (three baked stations; keyframed captures are below `reflections`' cut).
- Light thrown from a sheet onto nearby surfaces: the owner wants it later, "not a priority" (decisions.md); the roadmap's
  "Later", planned after `radiance-cascades` lands (plan section 9).
- A world (cells, streaming): a 300 m scene with a ring loads whole; the Open World shows streaming.

## 15. Deliver in this order (commits)

1. S1: the project, the scans, credits, notices, the index entry, the install test lines (the import times and metal findings in
   the message).
2. S2: `GenerateShowcaseTerrain.py` with `check`, `stations`, `selftest`; the terrain in the scene; `StrataTools.ShowcaseTerrain`.
3. S3: `GenerateShowcaseProps.py`; materials; prefabs; `StrataTools.ShowcaseProps`.
4. S4 (after the first build, which runs the full Debug ctest over S1-S3 first): the layout, the six probes baked, the stations'
   script, the first regression and `StrataEditor.Showcase`, the LookDev shots (stills and live), `checkshots`; the exit
   capture's numbers, the live shots' numbers and the perf reading in the message; then push `ci/wave8-showcase` for CI's times.
5. S5: the scripts in full, the loading scene, input actions, the full regression.
6. S7 (before S6 if R1 is late): E3, the CTests, `ShowcaseRenderCommands.json`, the perf cases, `Budgets.json`, pictures, game info, docs.
7. S6: materials and dressing, rebake, retakes.
8. Push `ci/wave8-showcase`; land through landing 1 (plan 7.3).
9. One commit per adoption pass, each with its A/B numbers and retaken pictures.

## 16. First hour

1. `python Tools/PerfLock.py --status`; `git -C G:/GameEngineAI log -1 --format=%H main` (S0.0 and S0.1 landed?); create the
   worktree and branch. **Do not build** while step 0's lanes run (Rules): check that
   `.worktrees/wave8-base/build/windows/bin/Release/StrataEditor.exe --version` reports 0.7.0 and drive that editor with a
   private `STRATA_SESSION_DIR` and `StrataCLI call --project` (its `StrataCLI.exe` beside it); the first build of
   `build/agent-showcase` waits for lane Render's landing and slot 4.
2. Read `.claude/skills/strata-make-a-game/SKILL.md`, `strata-scripting` and `strata-editor-automation`'s sections on
   `inputActions.*`, `ui.create` and `prefab.*`, and check this spec's command names and parameters against `editor.commands`
   (binding names, the triggers' `+` half axes, `prefab.create`'s parameters, `asset.setImportSettings` on a scan texture,
   `entity.rename`, `component.get`'s `/values/Mesh` of a `MeshRenderer`).
3. Start the fetch of D1 (scratch script, MD5 checks) in the background; meanwhile create the project and confirm in a scratch
   copy with the base editor: `Assets::GetLoadProgress` of a texture reaches 1 in a `--headless` exported game (a three-line
   test script, built by the editor's `script.build`); a `--resolution 3840x2160` window opens on this display (`StrataRuntime`
   of any exported sample with `--seconds 2 --screenshot`).
4. Import the A-list; list every warning the import logs (for `ALLOWED_MESSAGES`); start the Release import-time measurement
   under PerfLock (S1 step 3; Debug's waits for the first build).
5. Run the metal channel check on the four metal sets; write the D8 Metallic column from it.
6. Draft `GenerateShowcaseTerrain.py`'s height function and `check`; run `check` against D3's rules (all six) before writing any
   mesh.

## 17. Review corrections

The independent review (2026-11-02) checked this spec against main at 76f28a21, plan.md, `specs/decisions.md`, the showcase
audit and its data, and the specs beside it (`step0`, `step0-editor`, `step0-platform`, `step0-slang`, `reflections`,
`physics-timing`, `radiance-cascades`). The line counts of section 2.1 and nearly every file:line held; the A-list's sizes
recompute to 56.34 MB from `A/downloads/polyhaven/files/*.json`; shots 2-4's mirror arithmetic and image positions recompute
as written. These corrections win over the body (decisions.md: "a spec's Review corrections win over its body"); the body is
already edited to match.

1. **Item 4's reading is answered** (decisions.md, the owner on 2026-11-01). The body asked it again on the first sheet and
   made the other reading a wave 9 item. Now: highlights and reflections, **highlights first**; shots 2-4 and shot 6's lantern
   glint judged first on highlights, in motion, with no AA, FXAA and TAA, and a highlight that shimmers fails; the thrown light
   goes to the roadmap's "Later" after `radiance-cascades` (sections 1, 7, 14, D15).
2. **Highlights in motion were never measured.** Added 12 LookDev live shots (`showcase.live.shot2/3/4/6` with `.fxaa` and
   `.off`, exposure held, `StartShot` to begin the tour at the shot), `checkshots --live` (second difference of 32 px block
   means over 4 frames, mean at most 1.0%, p99 at most 3.0%, proposed from plan 1.1's reflection bound), a D15 row with a
   fallback order, S6's exit and an acceptance row. Stills cannot show shimmer, which is what the owner rejected in contact
   shadows.
3. **Step 0's slots** (step0.md 4.4, 10.5): the body configured and built `build/agent-showcase` in the first hour, beside four
   build-heavy lanes. Now: no build during step 0; `.worktrees/wave8-base`'s Release editor serves S1-S3; the first build runs
   in slot 4 after lane Render; commits before it are verified by the generators' selftests, and the first build runs the
   full Debug ctest before S4 (OQ2). The Debug import time waits for that build.
4. **`ctest --preset windows-debug`** runs the configure preset's `build/windows` (`CMakePresets.json`, "base" `binaryDir`), not
   `build/agent-showcase`: now `ctest --test-dir build/agent-showcase -C Debug -j 8`.
5. **The apron was below the ground on its -X side.** `0.04 + 0.004 * (x - 6)` is -4 cm at x -14 and 6 cm above y 0 under the
   bus, whose wheels stood at y 0; the terrain under the apron was not a pad (detail noise up to +-0.8 m against a 0.1 m skirt),
   and the text said "drainage toward +X" while the formula drains toward -X. Now `0.04 + 0.002 * (26 - x)`, the outline grown
   by 1 m is a pad, the skirt reaches 5 cm under the terrain, `check` gains rule 6, and props on the apron stand on its top
   (D3, D6).
6. **Shot 2's highlight fell off its panel.** The letterboard had a 2 degree tumblehome; tilting the mirror up turns the
   view ray that sees the sun down from 6 to 3.2 degrees here, so for a 1.6 m camera at 8.5 m the highlight lands near 2.07 m
   (the boarded window band), not at 2.49 m, and `checkshots`' 40 px would fail. Now the letterboard is vertical, `stations` solves the mirror condition with
   the generated normals, and `selftest` checks the letterboard (D5, D7).
7. **The bare steel was where the sun's highlight cannot be.** Vertical panels below the 1.6 m camera never mirror a sun above
   the horizon. The bare sections move to the letterboard along shot 2's dolly (z -1.45 to -0.30 and 0.85 to 2.0), so the
   highlight crosses painted and bare sheet steel, as ruling D2 ("the bus side weathered steel") and item 4 ask (D7).
8. **`Weathering.png` would import as sRGB color** (`TextureImporter.cpp:332-341` sets data only for names with `arm`, `rough`
   and the like), bending its roughness. Renamed `WeatheredSteel_arm.png`; with it `WeatheredSteel_diff.png` from the same rust
   mask, so bare steel varies in color, metalness and roughness instead of one F0 over an 8 m tank (`p04`'s "looks like CG");
   1024 x 1024 over 4 m instead of 512 over 2 m (four repeats across shot 3's tank would read as a pattern); PNG filter Sub. The
   budget becomes 70.6 MB of sources (9.4 MB margin) and at most 78.9 MB of binaries (D2, D7, D8).
9. **Collision hulls lost their mesh.** `MeshColliderComponent::Mesh` "uses the MeshRenderer's mesh when unset"
   (`PhysicsComponents.h`); removing the `MeshRenderer` first left the collider without one. The prefab script now binds the
   mesh and sets it before removing the renderer (D9).
10. **The hut's door was closed to the walk.** Four convex hulls cannot be four walls with a door opening; the door wall is
    three boxes (`Hut.Collision.1-6`, D7).
11. **The regression could miss shot 1.** An `until` that misses in a paused game steps it (`EditorCommandRunner.h:91-95`), so
    the colliders' wait played the tour on. The tour now starts with `input.key T` after that wait (`TourOnStart` false, two
    undos; D11 defines `StartTour()` and `Mode` 1 without a tour) (D13).
12. **The local lights had no unique names.** Two lantern instances repeated "Lantern" and "Wick", and the regression finds
    `Fire` by name: the `layout` script now renames them with `entity.rename`, and the prefab sets `CastShadows` and `Range`
    (D9). Turning them off by intensity 0 outside Dusk stays: a light at 0 cd is neither drawn nor shadowed
    (`RenderLights.cpp:59-61`, `:159-163`; checked, D4 now cites it).
13. **Windowed and headless timing.** `--seconds 20` could land in the black between shots 1 and 2 (the loading scene and the
    stations' wait shift the tour); `<S>` is now measured from the log's timestamps of `Showcase: shot 2` plus 7 s (D14).
    `GAME_FRAMES` likewise comes from the Debug pruned run's measurement (at least 1800).
14. **Q4 judged the import alone, on this machine alone.** The 54,000-frame cap (900 s) holds the whole script (import, play,
    two exports), and tests hold on CI's slowest runners: the decision now reads the whole run here (at most 600 s) and CI's
    Windows (Debug) time (at most 750 s), with `ci/wave8-showcase` pushed after S4 to read it early; the import measurement
    runs under PerfLock; the import setting is `{"Quality": "Fast"}` (`TextureImporter.cpp:389`), not `TextureQuality`.
15. **S0.3's check is measurable.** S0.3 adds `renderStats.shadowFit.changes` to `editor.perf` (step0.md 7.4), which the body
    called unverified; D15 now reads it once a second through the tour and a walk, with a pass rule (step0.md 12).
16. **Smaller fixes.** The probe row of 2.1 claimed a pending capture "never" flashes to the sky; `:290` says the sky lights its
    box unless another probe covers it (why D10 keeps the old station active). `ReflectionProbeComponent` is at
    `RendererComponents.h:150-166`. Rapid photo-mode scrubbing could leave three stations active (D10 step 4). Bookmark 7's
    autofocus would focus behind the boulder's surface on its 80% box collider (D11, D12). Trigger bindings take the half-axis
    form `Gamepad/RightTrigger+` (`InputNames.cpp:76`, `ThirdPerson.stproj:52`). `UVTiling` is two-component. The render
    script waits for `scene.loadStatus` first, and E_yard is re-measured by every adoption pass that changes the yard's light.
    D15's shot 3 probe check and shot 6 clipping check are now concrete captures (`showcase.shot3.noprobe`). The roadmap edit
    targets S0.12's wave 8 entry (step0.md section 9 removes `Roadmap.md:1425-1434`). `reflections`' "showcase step 5" means S6
    here. The README names Medium's TAA-only reflections. Q6 notes the box's floor 12.4 m under the ground. D10's open-source
    rule replaces "never copy code from outside this repository".

**Questions for the orchestrator** (also in the review's result):

- **OQ1** The graybox no longer waits for S0.7 (plan 4: "Its graybox waits for lane Render and S0.7"): the layout script places
  everything at computed heights. Recommended: accept; S0.7 serves S6's hand dressing.
- **OQ2** Commits S1-S3 during step 0 cannot run the Debug ctest (no build slot): recommended: allowed on the branch, verified by
  the generators' selftests, with the full Debug ctest at the first build before S4's commit; nothing reaches main before S8.
- **OQ3** `step0.md` section 9 still writes the roadmap line "Sunlight thrown off bright metal ... asked with the showcase's first
  sheet; a wave 9 item": recommended: fold decisions.md's answer into S0.12 ("Later", after `radiance-cascades`).
- **OQ4** The steel's color map (+about 3 MB of the 80 MB class, D7): recommended yes; without it the tank of shot 3 is one F0.
- **OQ5** `reflections.md` (its D9) still puts the highlight-only source radius at its step 13, first below its cut, while
  decisions.md moved it into R2's step 7 above the cut; this spec's R2 adoption row follows decisions.md. Recommended: the
  reflections review folds decisions.md in, and this row applies only to what R2 delivers.
