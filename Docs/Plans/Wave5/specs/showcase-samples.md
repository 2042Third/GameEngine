# Workstream `showcase-samples`: sample projects that show what Strata does (wave 5)

A new workstream of wave 5, run on the Apple M2 Max Mac (MoltenVK) while the Windows reference machine is away. It lands
**before** `physics-box3d`, which replaces Jolt with box3d at the same time and must keep these samples passing: the
samples use only the engine's API and scripts, never Jolt, and their physics checks are plausibility bounds, not Jolt's
numbers (section 6, "Physics robustness").

## 1. Goal

The owner, 2026-10-20: "include some more projects that you think could demonstrate this engine, you can also download
assets online, current sample project is too barebones, make projects you think that can demonstrate this engine."

`Samples/` holds only Tetris: a small scripted 2.5D game with built-in meshes and generated sounds. A person who opens the
launcher sees nothing of what waves 1-4 built: GPU-driven rendering at scale, world streaming, physics, skinned animation,
game UI, audio, scripting and export. This stream adds three sample projects, made through the editor the way Tetris was
(skills `strata-make-a-game` and `strata-editor-automation`: StrataCLI or MCP, command scripts, C++ game scripts), with
assets downloaded from sources with clear licenses, each played by a CTest that fails when the sample breaks, each exported
and run headless and in a window. Together they show:

| Engine feature | Physics Playground | Third-Person Adventure | Open World |
| --- | --- | --- | --- |
| GPU-driven rendering at scale (indirect draws, GPU culling, LODs, shadows) | ~600 bodies | | >= 20,000 instanced props, cascaded shadows |
| World streaming (World asset, layers, cells, streaming source, cooked cells) | | | yes |
| Physics (dynamic stacks, triggers, layers, raycasts, contacts, character controller) | yes | controller, pushing, triggers | controller on terrain, colliders in cells |
| Skinned animation (controllers, transitions, events, many instances) | | one character, 14 clips | 30 foxes in streamed cells |
| Game UI (canvases, buttons, toggles, sliders, layout, sliced images, fonts, menus) | tool panel, HUD | main menu, HUD, pause and settings | HUD, pause menu |
| Audio (one-shots by impact, 3D sources, streamed music, master volume) | impacts, UI | footsteps by animation event, music, UI | campfires, ambience |
| Scripting (fields, hot reload, save files, scene loads, window settings) | hot reload | save files, scene loads, window mode | World API, prefetch |
| Export (full and pruned, headless and windowed) | yes | yes | cooked world |
| PBR materials and lighting | ambientCG PBR textures | HDR sky (Poly Haven) | procedural sky, day and night |

## 2. Inventory: what exists today and what changes

What exists (release 0.4.0, main at 580eee9e):

- **Samples.** `Samples/Samples.json` (`{"Strata": {"Format": "Samples", "Version": 1}, "Samples": [{Directory, Name,
  Description}]}`) lists Tetris; `Editor/ProjectSamples` reads it (`List`, `Copy` without `.strata`); the
  `StrataSamples` target copies `Samples/` next to the editor at every build; `CMake/StrataPackaging.cmake` installs it
  (release archives ship it). The launcher (`Panels/WelcomePanel.cpp`, `DrawStarters`) shows each sample as a card with
  the `Gamepad2` icon, its name and description; recent projects show a picture (`Editor/ProjectThumbnail`:
  `<project>/.strata/Thumbnail.png`, 480x270, written after each scene save), samples do not. Tetris has no README, no
  credits file and no picture.
- **Sample regression.** `StrataEditor.Tetris` (`StrataTests/Editor/TetrisSample.cmake`, `TetrisCommands.json.in`, label
  `package`, `RESOURCE_LOCK toolchain`): copies the sample, runs the editor `--no-gpu --quit-after-commands` on the
  command script, checks printed lines, exports full and pruned, runs both games `--headless --frames 120` and checks their
  lines. The runner is written for Tetris only. `StrataEditor.Install` checks `project.samples` lists Tetris first.
  `ProjectSamplesTests.cpp` expects exactly one sample; `Scene.Serialization` loads the Tetris scene cleanly and checks it
  saves back the same.
- **Command scripts** (`Editor/EditorCommandRunner`): steps `{command, parameters, expect, bind}`; `expect` conditions are
  `equals`, `min`, `max` on JSON pointers. There is no way to wait for a condition: `editor.wait {frames}` counts frames,
  `play.step` counts fixed steps. A physics outcome that takes a different time under another engine can only be checked
  after a fixed wait.
- **Helpers for exported games:** `RunAndExpect.cmake` (`-DEXPECT` line), `StrataTests --strata-test-helper=check-image`
  (not black, not one color, `--dominant <color> <percent>`, `--size`) and `check-perf-log` (`--nonzero`, `--column`,
  `--max`), used by `StrataRuntime.World`, `StrataRuntime.RenderCheck` and others.
- **Importers:** glTF 2.0 (`.gltf`, `.glb`) with `KHR_lights_punctual`, `KHR_materials_emissive_strength`,
  `KHR_materials_unlit`, `KHR_texture_transform`, `KHR_mesh_quantization`, `EXT_meshopt_compression`; morph targets and
  vertex colors are skipped with a warning; settings `Scale`, `GenerateLODs`, `LODCount`, `OptimizeMeshes`,
  `ImportMaterials`, `ImportCameras`, `ImportLights`, `MaxTextureSize`, `TextureCompression`, `TextureQuality`. Textures:
  PNG, JPEG, TGA, BMP, PSD, GIF, HDR. Audio: WAV, MP3, FLAC, Ogg Vorbis (streamed above 1 MB). Fonts: TTF, OTF.
  `MeshRenderer.Material` overrides every submesh: a model whose materials import unlit (`KHR_materials_unlit`) cannot be
  made lit per submesh.
- **What the samples build on:** `RigidBody` (static, dynamic, kinematic, triggers, layers, CCD), box, sphere, capsule and
  mesh colliders, `CharacterController` (steps, slopes, pushing); scripts' raycasts, overlaps, contacts and triggers,
  `Scene::InstantiateMany`; `Animator` controllers with states, parameters, transitions and events
  (`animation.createController`), `OnAnimationEvent`; the World module (`world.create`, `edit`, `moveEntities`, `cook`,
  `status`, `info`, `StreamingSource`, the scripts' World API); `prefab.instantiate {instances}` (up to 100,000 in one
  step); GameUI (canvases, buttons, toggles, sliders, layouts, sliced images, fonts, `input.click`); `Audio::PlayOneShotAt`,
  `SetMasterVolume`, `AudioSource`; `Game::LoadScene`, `SetWindowMode`, `SetVSync`, save files; the procedural sky and HDR
  environment maps; `editor.perf`'s `renderStats` (`gpuDriven`, `instances`, `drawCommands`...).
- **Also there, and shown by the samples:** the export's loading scene (`project.setExportSettings {loadingScene}`) with
  `Game::GetSceneLoadProgress` (the Open World's start and the course load); texture mip streaming and residency budgets
  (`--asset-budget-mb`, `asset.setBudget`: one exported Open World run under a small budget); `AttachToJointComponent` (a
  prop in the robot's hand); `Animator`'s `GetCurrentState` (the checks' state fields); `entity.dropToGround` (landmarks
  placed on the terrain mesh); `UIMask` (the Third-Person's scrolling credits panel). The procedural sky's colors are
  constant (`SkyLightComponent`): the Open World's `DayNight` script drives its colors and intensity itself.
- **What the checks can express:** `expect` takes `equals`, `min` and `max` on absolute JSON pointers; `$ref` bindings
  substitute into parameters, not into conditions. A check of a difference, an "or", or a comparison with an earlier value
  is therefore a script field that computes it (section 4.2-4.4 name them), never an extension of `expect`.
- **What physics guarantees:** the simulation waits before its first step only for mesh colliders that exist then; one that
  arrives later (with a streamed cell) is pending until its mesh loads and its shape is cooked (`PhysicsWorld.h`).
  `MeshColliderComponent::Convex` defaults to true; non-convex mesh colliders exist only on static bodies. `physics-box3d`
  defers triggers on triangle meshes, so every trigger of the samples uses a primitive collider.
- **What is not there** (and the samples work around, section 9): no terrain system (height fields are wave 5's
  `world-environment`), no fog (height fog is `render-quality`'s: distant streamed cells are hidden by streaming distances,
  the camera's far plane and the tour's height, not by fog), no particles (`vfx`), no joints, no input actions, no morph
  targets, no exported-game automation. Simulated taps (`input.key`, `input.click`...) end only after the game's fixed steps,
  and none run at time scale 0 (I7 fixes that before the Third-Person's pause menu).

What changes:

| Area | Change |
| --- | --- |
| `Samples/` | Three new projects, each with `README.md`, `CREDITS.md`, `Picture.png` and `Screenshots/`; Tetris gets the same files; `Samples.json` lists four samples with pictures. |
| Launcher | Sample cards show the sample's picture; a copy starts with it as its thumbnail. |
| Command scripts | `until`: a query step repeated until its `expect` holds, bounded in game steps and seconds; in a paused game each attempt runs one fixed step (exact, independent of the frame rate). |
| Simulated input | Taps end while the game runs no fixed steps (time scale 0): I7. |
| Tests | One generic sample runner (`SampleProject.cmake`, Tetris moved onto it; unexpected warnings fail it), three new package tests, windowed GPU runs of every exported sample, a `Samples.Content` suite (licenses, credits, sizes, required files); the tests that copied all of `Samples/` copy only what they need. |
| glTF import | A setting that imports `KHR_materials_unlit` materials lit (Kenney's Nature Kit is authored unlit by its exporter). |
| Tools | `Tools/GenerateIslandTerrain.py`: the Open World's terrain tiles, palette and prop scatter, deterministic, outputs committed. |
| Docs, skills, notices | Section 7. |

## 3. Assets: sources, licenses and budgets

**Licenses.** CC0 first; CC-BY 4.0 only with the attribution it asks for, shown in the game's credits and in
`ThirdPartyNotices.md`. Anything whose license is unclear is left out. Checked at selection time (the probe on 2026-10-20
found each of these reachable by plain HTTPS and licensed as stated):

| Source | License | Used for |
| --- | --- | --- |
| Kenney (kenney.nl): Nature Kit, Platformer Kit, Impact Sounds, Interface Sounds, UI Pack, Kenney Fonts | CC0 (each pack's `License.txt`) | Open World props; Third-Person course; impacts, footsteps, UI sounds; menu panels; display fonts |
| ambientCG (ambientcg.com), 1K-JPG sets | CC0 | Physics Playground PBR materials (color, normal (GL), roughness) |
| Poly Haven (polyhaven.com), one 1K HDRI | CC0 | Third-Person sky and image-based light |
| `RobotExpressive.glb` by Tomás Laulhé (Quaternius), modified by Don McCurdy, from the three.js repository's example models, pinned to a commit | CC0 1.0 (its README) | Third-Person character: skinned, 86 joints, 14 clips (Idle, Walking, Running, Jump, WalkJump, Wave, Dance, Punch, Death...) |
| Khronos glTF-Sample-Assets `Fox`, pinned to a commit | model CC0 (PixelMannen); rigging and animation CC-BY 4.0 (tomkranis); glTF conversion CC-BY 4.0 (@AsoboStudio, @scurest) | Open World wildlife: skinned, Survey, Walk, Run |
| OpenGameArt, only entries marked CC0 on their page | CC0 | Optional music and ambience loops (left out if no clear CC0 loop fits) |
| Generated: `Tools/GenerateIslandTerrain.py` | made for Strata | Island terrain tiles and palette |

Quaternius' larger packs (Universal Animation Library, Stylized Nature MegaKit) are CC0 but download through itch.io or
Google Drive, whose free-download flows refused a scripted client in the probe: not used (open question 1).

**Downloads are untrusted data.** Each download goes into its own new, empty directory under the worktree's
`build/scratch`; scripts that inspect or trim them live elsewhere (`build/scratch/tools`) and run with `python3 -I`. Files
reach a project only through the engine's importers (`asset.import`); a crash or hang of an importer on a downloaded file is
an engine bug, fixed with a crafted-file regression test. Preparing a file before import (keeping the 1K maps of a set,
dropping maps the engine does not use) is recorded in the credits' Changes column.

**Budgets** (MB = 1,000,000 bytes; checked by `Samples.Content`, section 6):

| What | Limit |
| --- | --- |
| Source assets of one sample (files an importer reads: models, buffers, images, sounds, fonts) | 25 MB |
| Documents of one sample (scenes, world cells, prefabs, materials, controllers, `.meta`, `.stworld`) | 20 MB |
| Binary files of all samples, Tetris included (every file that is not a text document: source assets, `Picture.png`, `Screenshots/`); they barely compress | 55 MB |
| Text documents of all samples (they compress about tenfold in the archives) | 50 MB |
| `Picture.png` | PNG, exactly 480 x 270 (`ProjectThumbnail::c_Width` x `c_Height`) |
| `Screenshots/` | at most 3 PNGs, 3 MB together |

Textures 1K (2K only where a 1K map visibly fails), JPEG for color, PNG where alpha or exact palettes need it;
`EXT_meshopt_compression` or `KHR_mesh_quantization` for generated meshes. With the two totals, the release archives grow
by at most 60 MB (55 MB of binaries plus about a tenth of the documents); the report states the growth measured with
`cmake -E tar cfz` of `Samples/` before and after (and the release dry run's package notice when the integrator runs it).

**PBR maps.** The engine's `MetallicRoughnessMap` is glTF-packed (G roughness, B metallic, multiplied by the factors,
`Material.h`). A grayscale ambientCG roughness JPEG is used as that map only with `Metallic` 0 (non-metals); a metal set's
metalness and roughness maps are packed into one map by a scratch script before import, recorded in the credits' Changes
column.

**Credits format** (machine-read). Each sample's `CREDITS.md` has one table:

```markdown
| Files | Source | Author | License | Attribution | Changes |
| --- | --- | --- | --- | --- | --- |
| `Assets/Models/Robot/RobotExpressive.glb` | https://github.com/mrdoob/three.js/blob/<commit>/examples/models/gltf/RobotExpressive/RobotExpressive.glb | Tomás Laulhé (Quaternius); modifications by Don McCurdy | CC0-1.0 | | none |
| `Assets/Sounds/Impacts/` | https://kenney.nl/assets/impact-sounds (version 1.0) | Kenney | CC0-1.0 | | 14 of 130 files |
```

`Files` is a path relative to the sample, or a directory ending in `/`; `License` is `CC0-1.0`, `CC-BY-4.0` (with
`Attribution` text) or `Strata` (made for this repository: generated by a committed tool, named in Source, or by the agent
that made the sample, like Tetris' sounds; no license is claimed, the repository's terms are the owner's call).
`ThirdPartyNotices.md` gains a "Sample assets" table with one row per source pack (sample, pack, URL, author, license,
attribution), which replaces its sentence about Poly Haven environment maps in sample projects; exported games ship that
file, which carries the Fox's CC-BY attribution with every exported Open World.

## 4. Deliverables

Every sample is a project under `Samples/<Directory>/`:

```
<Name>.stproj  Assets/...  Scripts/ (CMakeLists.txt, C++ sources)  README.md  CREDITS.md  Picture.png  Screenshots/
```

- Made through the editor: projects, imports, materials, prefabs, scenes, worlds, controllers, UI and export settings are
  editor commands; the only hand-written files are script sources, the README and credits, and files generated outside
  the project and then imported (as Tetris' WAVs were). `.stscene`, `.stmat`, `.stproj` and `.meta` files are never edited
  by hand.
- Scripts follow the code style (Hazel conventions, as Tetris' do), keep tuning in fields, log one `Log::Info` line per
  meaningful event with the sample's prefix ("Playground: ..."), survive hot reload (`OnReload` finds its entities again),
  and compile without warnings with GCC 14 and LLVM 16 at `-Wall -Wextra -Werror` (checked once by hand, as the
  make-a-game skill says) and in every CI package job.
- `README.md`: what it shows (mapped to the engine features of section 1, with links to the manual pages), controls
  (keyboard and mouse, gamepad where supported), how to open it (launcher > Open Sample, or `project.openSample`), how it was
  made (the commands and generators used, in a few lines), credits (a link to `CREDITS.md`, and the CC-BY attribution in
  full where one applies), and at least two screenshots from `Screenshots/` that the agent looked at.
- `Picture.png`: the project's own thumbnail (`.strata/Thumbnail.png`, written after a scene save in an editor with a GPU),
  copied once the start scene looks right; the launcher shows it.
- A launcher entry in `Samples.json` with a description of one or two sentences written for people.
- A regression command script `StrataTests/Editor/<Directory>Commands.json.in` run by CTest (label `package`: it builds the
  scripts), exported full and pruned, both games run headless; a windowed run of the exported game (label `gpu`) whose
  screenshot is checked. Every exported-game run passes `--save-directory <work directory>/Saves`, so no run reads or
  writes the player's own save files (a windowed check never runs fullscreen because of an owner's setting).
- The sample's runs log no warning or error but the ones its CTest lists as expected (`ALLOWED_MESSAGES`, regular
  expressions, each with the reason: the robot's skipped morph targets, the regression's deliberate misuse checks):
  `SampleProject.cmake` fails on any other `[warning]`, `[error]` or `[critical]` line of the editor's and the games'
  output. Every scene and cell document loads cleanly and saves back the same (`Scene.Serialization`'s committed-scenes
  test, extended to every sample).
- Colliders: every trigger (goal basket, launch pad, coins, jump pads, goal flag) uses a primitive collider (box, sphere,
  capsule); non-convex mesh colliders (`Convex` false) only on static bodies; dynamic bodies use primitives.

### 4.1 Infrastructure (engine and tests), before the samples

**I1. Command-script `until`.** A step may carry `"until": {"steps": n, "seconds": s}` (at least one; `seconds` defaults to
300, `steps` counts the running game's fixed steps since the step began, `Scene::GetFixedStepCount`). The step's command
must be a query (`EditorCommandKind::Query`); the runner issues it once per frame until its `expect` holds, and fails the
step with the last result and the attempts made when `steps` fixed steps ran or `s` seconds passed first. **In a paused
game** each attempt that fails runs exactly one fixed step before the next (as `play.step` does), so a paused wait is exact
and independent of the frame rate, bounded by `steps`; `seconds` stays the wall-clock guard. The sample regressions run
their physics checks paused (unpaused editor play drops steps when a frame falls behind, which a Debug build with 600
bodies on 3 threads does, so outcomes would depend on the machine). Only the final attempt is recorded in the activity
journal (with `attempts`), so a wait does not flood it. Parsing refuses `until` without `expect`, on a non-query command,
with non-positive bounds or unknown keys, and inside `editor.batch` (atomic, one frame). `bind` applies to the final
result.

- Acceptance: unit tests in `Editor.CommandScript` (and `Editor.CommandScriptBindings` for `bind`) for: a field that
  reaches its value after some steps passes, unpaused and paused (paused: the step count advanced by exactly the attempts
  minus one); one that never does fails after `steps`; one whose game neither plays nor steps (play mode off) fails after
  `seconds`; a non-query or an `until` without `expect` is refused before anything runs; the journal holds one entry;
  `bind` reads the final result. Must-fail: with the runner's `until` handling removed the first test fails (the condition
  is false on the first attempt); without the paused stepping the paused test fails.

**I2. Sample pictures in the launcher.** A `Samples.json` entry takes an optional `"Picture": "Picture.png"` (a relative path
inside the sample's directory: no absolute paths, no `..`, PNG). `ProjectSample::Picture` holds it (empty when absent);
an entry whose picture is missing or invalid is still offered, without its picture, and `List` reports why in its log
(not an invalid index: a sample stays usable). `project.samples` returns `picture` (absolute path or null). The launcher's
sample cards show the picture (decoded on an I/O job as recent-project cards are, the icon until it is ready or when it
fails); `ProjectSamples::Copy` writes the picture as the copy's `.strata/Thumbnail.png`, so its recent-project card shows it
before its first save.

- Acceptance: `Editor.Samples` (`ProjectSamplesTests.cpp`): a picture is listed; absolute, escaping, missing and non-PNG
  pictures leave the sample listed without one; a copy's thumbnail equals the picture byte for byte; `project.samples`
  reports it. A headless UI test (`Editor.UI`, `ImGuiHarness`) shows the sample card with an image. Must-fail: without
  the WelcomePanel change the UI test sees no image on the card; without `Copy`'s change the thumbnail check fails.
- `ProjectSamplesTests` stops expecting exactly one sample: it reads the repository's `Samples.json` and expects what it
  lists (Tetris among them), so adding a sample breaks no test. The tests that copy the whole `Samples/` directory
  (`ProjectSamplesTests`' `CopySamples`, `DataSafetyTests`) copy only `Samples.json` and `Tetris/`, or write a synthetic
  index, so that about 100 MB of new samples are not copied per test on 3-thread CI runners and a nearly full disk.

**I3. One sample runner.** `StrataTests/Editor/SampleProject.cmake` generalizes `TetrisSample.cmake`: copy the sample without
`.strata` (refusing a directory that is not the named sample), configure the command script (export directories as JSON
strings), run the editor `--no-gpu --no-automation --quit-after-commands --frames <cap>` on it, check the editor's output for
each `EDITOR_EXPECT` line, check both exports exist (game, `.stgame`, `.stpak`, the script module), run each `--headless
--frames <n> --save-directory <work>/Saves` (with `--perf-log` when asked) and check each `GAME_EXPECT` line, check the
pruned pack is not larger, and fail on any warning, error or critical line of any output that no `ALLOWED_MESSAGES`
expression matches. Parameters come from `StrataTests/CMakeLists.txt`. `StrataEditor.Tetris` runs on it with its current
commands and lines unchanged; `TetrisSample.cmake` is removed.

- Acceptance: `StrataEditor.Tetris` passes on the new runner; each failure branch (missing expected line, a failing game,
  an unexpected warning) is checked once by hand and reported.

**I4. `Samples.Content`** (doctest suite `StrataTests/src/Samples/SampleContentTests.cpp`, a shard in `CoreShards.json`).
For every sample of `Samples.json`, and every directory of `Samples/` (none unlisted): `README.md`, `CREDITS.md` and
`Picture.png` (480 x 270 PNG) exist; every source-asset file under `Assets/` (the extensions the importers read) is covered
by exactly one credits row; every row covers at least one file; licenses are in the allowed set, CC-BY rows have an
attribution; every CC0 and CC-BY source URL appears in `ThirdPartyNotices.md`'s "Sample assets" table; the budgets of
section 3 hold, per sample and the two totals over all samples. The checks are functions (`CheckSampleContent(directory,
notices) -> errors` per sample, `CheckSamplesTotals(samples) -> errors` for the totals) so the suite also runs them on
crafted directories.

- Acceptance: the suite passes on the repository. Must-fail (permanent test cases on crafted temporary directories): an
  uncredited `.png`, a stale row, a `CC-BY-4.0` row without attribution, an unknown license, a 26 MB source asset, binary
  files over the total, a missing picture and a picture of the wrong size each produce the expected error.

**I5. Lit import of unlit glTF materials.** `GltfImporter` setting `UnlitMaterials`: `AsAuthored` (default: today's output,
so `GetVersion` stays) or `Lit` (the `KHR_materials_unlit` flag is ignored; base color, factors and textures are kept),
described in `GetSettingsProperties`. Needed for Kenney's Nature Kit, whose 329 models all carry the extension (the
Platformer Kit's GLBs are checked in the first step: if they carry it too, the Third-Person imports them `Lit`). The key is
left out of the importer's default settings (an absent key means `AsAuthored`), because `EditorAssetManager` hashes the
merged default and `.meta` settings into the cache key: a new default key would re-import every existing glTF of every
project once.

- Acceptance: an `Asset.Gltf` test imports a model with `KHR_materials_unlit` both ways (`Unlit` true, then false with the
  same base color); `asset.setImportSettings` re-imports with it; an existing model's cache stays valid (an import of a
  project made before the setting is not redone). Must-fail: without the setting's handling the `Lit` import keeps
  `Unlit` true; with the key in the defaults the cache test fails.

**I7. Taps while time stands still.** At `Time::SetTimeScale(0)` (a pause menu) the game runs frames but no fixed steps
(`Scene` scales the frame's time into the step accumulator), so a simulated tap, which ends only after its last fixed step,
never ends, and `input.key`, `input.mouseButton`, `input.click` and `input.gamepadButton` never answer. Taps count, in
place of fixed steps, the input frames the running game updates without a fixed step when its time scale is 0 (a counter
next to the step count that the editor advances by one for each such frame), so a 1-step tap at scale 0 lasts one input
frame and a paused game (`Input::SetSuspended`) still waits for its next step.

- Acceptance: an `Editor.Commands` test plays a scene whose script sets the time scale to 0 when a key is tapped and back
  to 1 when a UI button is clicked: the tap and the `input.click` answer, the script saw both, and fixed steps run again.
  Must-fail: without the counter the click never answers (the test's bounded wait fails).

**I6. Small engine bugs found while building** get a fix and a regression test that fails without it, in their own commits;
large gaps go into the report (section 9 lists the expected ones).

### 4.2 Physics Playground (`Samples/PhysicsPlayground`)

An arena (40 x 40 m, PBR floor and walls from ambientCG) with stations a free camera flies between:

1. **Tower and pyramid:** a 12-crate column and a 21-crate pyramid (wood PBR).
2. **Domino run:** 64 dominoes along an S-curve, started by a button (or the demo).
3. **Ball pit:** 500 spheres of several materials (restitution and friction vary) in a walled pit; "Pour" adds 100 with
   `Scene::InstantiateMany`.
4. **Goal:** a funnel over a trigger basket that counts balls (`OnTriggerEnter`, score on the HUD, a sound); "Feed Goal"
   drops 5 balls into the funnel.
5. **Ghosts:** 8 boxes on a `Ghost` collision layer that does not collide with `Ball` (the scene's layer matrix): dropped
   onto the pit, they sink through the balls to its floor.
6. **Ramp and pins:** balls launched by a trigger pad roll down a ramp into 10 pins.

Interaction: WASD and mouse fly the camera (right mouse looks), number keys jump to stations; left click shoots a ball from
the camera (raycast aim, an impulse along the view); holding E grabs the body under the cursor (`Physics::Raycast`, then a
velocity toward a point in front of the camera each fixed step); R resets everything. A tool panel (UI canvas: `UILayout`
of `UIButton`s, a `UIToggle` for slow motion (`Time::SetTimeScale(0.25)`), a `UISlider` for shot power; Kenney UI Pack
sliced images, a Kenney font) and a HUD (bodies, score, frame time). Impacts play a wood, metal or soft sound by material
(`OnCollisionEnter`, volume from the relative speed, at most 8 a frame). An `AutoDemo` field (default true; the regression
turns it off) pushes the dominoes after 2 s, pours after 6 s and feeds the goal after 10 s, so the exported game shows
something without input.

Colliders: crates, dominoes, pins and ghosts are boxes, balls spheres, all dynamic; the arena, pit walls, funnel and ramp
are static boxes (the funnel built of boxes); the goal basket and the launch pad are box triggers.

Script fields the checks read: `Bodies`, `TowerTopDrift` (metres the top crate moved), `DominoesDown` (tipped beyond 45
degrees), `BallsInPit`, `BallsEscaped` (outside the arena or below the floor), `GhostsOnPitFloor`, `Score`, `Shots`,
`LastShotForwardSpeed` (the newest shot ball's velocity along the camera's forward, sampled in the step after the shot),
`ShotSpeed` (the tuning field it is compared with), `Reloads` (`OnReload` calls).

Acceptance (`StrataEditor.PhysicsPlayground`, `--no-gpu`; the physics checks run with the game paused, so `until` steps it
exactly):
- `script.build` succeeds; `play.start`; `physics.stats` reports at least 600 dynamic bodies; `Bodies` equals the scene's
  count (a number written into the script once the scene is built).
- After `play.step` of 180 steps (3 s of game time): `TowerTopDrift` at most 0.25; `BallsEscaped` 0.
- `input.click` on "Start Dominoes": `until` `DominoesDown` at least 60 within 1,200 steps.
- `input.click` on "Feed Goal": `until` `Score` at least 4 within 900 steps.
- Ghosts: `until` `GhostsOnPitFloor` equals 8 within 600 steps.
- A left click at the viewport's centre (`input.mouseButton`): `Shots` 1 and `LastShotForwardSpeed` at least half of
  `ShotSpeed` (the expected value is the field's default, written into the check).
- `script.reload` while playing: `Reloads` 1, `Shots` still 1 and `Bodies` the scene's count plus the poured and shot balls
  (known numbers), and the playground goes on (a domino reset and rerun reaches 60 again).
- R: `DominoesDown` 0 and `TowerTopDrift` below 0.05 after 60 steps; the log has "Playground: reset".
- Export full and pruned. `alwaysInclude` names only what scripts load by path and no scene references (pruned packs keep
  every scene with its closure anyway): the ball prefab the shots and pours spawn and the impact sounds played by path.
  Both games headless for 900 frames print "Playground: 6xx bodies" (the count from the scene), "Playground: domino run
  complete" and "Playground: goal scored".
- `gpu`: the exported game windowed for 600 frames, `--screenshot` passes `check-image`; `--perf-log` has a nonzero
  `Scene.Physics` column. The report gives the Mac's Release median of `Scene.Physics` with every body awake (information,
  not a gate).

### 4.3 Third-Person Adventure (`Samples/ThirdPerson`)

- **Scenes.** `Scenes/MainMenu.stscene` (start scene): a camera on the robot dancing and waving on a platform with a flag
  in its hand (`AttachToJointComponent` on its hand joint), a canvas with the title (Kenney font), Play, Settings, Credits
  (a scrolling panel inside a `UIMask`) and Quit. `Scenes/Course.stscene`: a course of Platformer Kit pieces (about 40
  models: islands, paths, ramps and stairs within the controller's step height and slope, trees, fences, flags; static
  bodies, non-convex mesh colliders), 12 spinning coins (sphere triggers), 2 jump pads (box triggers: a high `Jump`), a
  crate (dynamic box) to push off a ledge into a gap so the player can climb across, a goal flag (box trigger); the Poly
  Haven HDRI as the sky light's environment, a sun with shadows, post-processing. `Scenes/Loading.stscene`: the export's
  loading scene, a progress bar from `Game::GetSceneLoadProgress`.
- **Character.** `RobotExpressive.glb` imported (scale from `asset.info` bounds: about 1.8 m tall; morph targets are skipped
  with the importer's warning, listed in the README and the CTest's `ALLOWED_MESSAGES`), a controller
  `Animations/Robot.stanimator`: parameters `Speed` (float), `Grounded` (bool), `Jump`, `Wave` and `Dance` (triggers;
  animators have only float, bool and trigger parameters); states Idle, Walk, Run, Jump, Wave, Dance with cross-fades;
  `Footstep` events at the foot contacts of Walking and Running.
- **Scripts.** `ThirdPersonPlayer` (camera-relative movement, Shift sprints, Space jumps with a short coyote time, 1 waves
  and 2 dances while idle, footsteps from `OnAnimationEvent`, a land sound), `FollowCamera` (mouse or right stick orbit,
  scroll zoom, a spring arm shortened by `Physics::Raycast` so walls never come between camera and player), `Coin`,
  `JumpPad`, `Goal`, `CourseState` (HUD: coins, time; a completion panel with the best time kept in a save file),
  `PauseMenu` (Esc: `Time::SetTimeScale(0)`, Resume, Settings, Restart, Main Menu, Quit), `SettingsMenu` (master volume and
  mouse sensitivity sliders, invert-Y, fullscreen and vsync toggles through `Game::SetWindowMode` and `SetVSync`; written to
  `settings.json` with `Game::WriteSaveFile` and read at start), `MainMenu` (`Game::LoadScene`). **Attract mode:** after
  `AttractDelaySeconds` (default 10) in the main menu without input, the menu sets the module's attract flag (game-wide
  state in the script module, which stays loaded across scene loads) and loads the course; in attract mode the player walks
  itself along `AttractPath` waypoints to the first coins, and any key, click or button returns to the main menu (the arcade
  convention). The editor regression sets `AttractDelaySeconds` to 0 (off). Gamepad: left stick moves, right stick orbits,
  A jumps, Start pauses; UI navigation works by itself.
- **Audio.** Footsteps (Kenney Impact Sounds), jump and land, coin and completion (Interface Sounds), UI hover and click,
  a streamed music loop when a CC0 one fits (else none: the README says so).

Script fields the checks read (expect compares a value with constants only, section 2): `ThirdPersonPlayer.AnimatorState`
(the index of `Animator::GetCurrentState` in Idle 0, Walk 1, Run 2, Jump 3, Wave 4, Dance 5; -1 for another),
`DistanceWalkedForward` (metres along the camera's forward since the field was last reset by `script.setField`),
`MaxJumpHeight`, `Grounded`; the crate's `Pushable.Displacement` (metres from its start); `CourseState.Coins`;
`PauseMenu.Paused` and `TimeScale`.

Acceptance (`StrataEditor.ThirdPerson`, `--no-gpu`):
- `play.start` in the main menu; `input.click` Play: `until` `play.state`'s `/running/name` is "Course" within 10 s.
- `input.sequence`: W held 90 steps: `DistanceWalkedForward` at least 3; `AnimatorState` between 1 and 2 (Walk or Run)
  while held (`input.key` held, then `script.getField`), 0 (Idle) within 60 steps after release (`until`).
- Space: `MaxJumpHeight` at least 0.5 m, `Grounded` again within 120 steps.
- The player placed behind the crate (`component.set` on the running copy), W held 120 steps: `Displacement` at least 0.5.
- The player placed on a coin: `until` the coins `UIText` equals "1 / 12" within 120 steps; the log has
  "ThirdPerson: coin 1 of 12".
- Esc: `Paused` true and `TimeScale` 0; `input.click` Settings, the invert-Y toggle: the log has "ThirdPerson: settings
  saved"; `input.click` Resume: `TimeScale` 1 (these taps at time scale 0 need I7).
- The player placed in the goal: the completion panel is active; the log has "ThirdPerson: course complete in".
- Export full and pruned with the loading scene. `alwaysInclude` names what scripts load by path and no scene references
  (the sounds played by path, if any). Both games headless for 1,800 frames (attract mode on, as saved) print
  "ThirdPerson: main menu ready", "ThirdPerson: attract mode", "ThirdPerson: course loaded" (the course loaded through
  `Game::LoadScene` from the cooked pack, behind the loading scene) and "ThirdPerson: coin 1 of 12".
- `gpu`: the exported game windowed, its menu screenshot passes `check-image`.

### 4.4 Open World (`Samples/OpenWorld`)

- **Terrain.** `Tools/GenerateIslandTerrain.py` (standard library only, deterministic from a seed, outputs committed like
  `GenerateBrandAssets.py`'s; CI never runs it): an island about 768 m across in a 12 x 12 grid of 64 m tiles, written as
  one glTF model `Terrain/Island.glb` with one mesh per land tile (2 m spacing, shared edge vertices identical so tiles
  meet without cracks, `KHR_mesh_quantization`), its UVs into a small palette `Terrain/Palette.png` (sand, grass, forest
  floor, rock, snow by height and slope). Imported with `GenerateLODs` off (simplified tiles would open seams). **The
  terrain stays in the persistent scene** (the unpacked instance's tiles, each a static body with a `MeshCollider`,
  `Convex` false): it is visible from everywhere (no fog hides missing tiles, section 2), and the simulation waits for its
  colliders before the first step, so nothing falls through after a teleport. The streamed layers are Vegetation, Wildlife
  and Landmarks.
- **World.** `world.create {"path": "Worlds/Island", "cellSize": 64}`; layers Vegetation (streaming distance 3 cells),
  Wildlife (2) and Landmarks (5). The persistent scene holds the terrain, the sun and procedural sky, a 2 km water plane
  (blended), post-processing, the player, the game manager and the UI. The camera's far plane and the tour's height are
  chosen so that the edge of the loaded vegetation is far and low in the view (pop-in at the horizon, not in front).
- **Props at scale.** About 25 Nature Kit models (imported with `UnlitMaterials: Lit`) as one-entity prefabs (a
  `MeshRenderer` on the model's mesh; trees with a capsule and large rocks with a box collider, static), placed by the
  generator's `scatter` command: a command script of `prefab.instantiate {instances}` steps (seeded Poisson-disc sampling
  on the same height function: palms on the beach, oaks and pines inland, rocks on slopes, flowers and grass tufts in
  clearings) that the editor runs into the Vegetation layer: the script starts with `world.edit {"editLayer":
  "Vegetation"}` and pins every cell of the island with `world.loadCells`, so no instance lands in the persistent scene
  (an entity created in an unloaded cell's area would stay there, `EditorWorld::Recell`). At least 20,000 instances.
  Before scattering, the bytes a prop instance takes in its cell document are measured and the count set so that the
  documents stay within 20 MB.
- **Landmarks.** Four or five places (camps with tents, campfires with a point light, a crackling 3D `AudioSource` and an
  emissive ember material; a canoe on a beach; bridges and fences), placed on the terrain mesh with `entity.dropToGround`,
  so walking has destinations.
- **Wildlife.** 30 foxes (Khronos Fox, skinned) in the Wildlife layer's cells, each with a `Wander` script (survey, walk,
  run; ground height by raycast against the Terrain collision layer, and a fox stands still until its ground ray hits; a
  fox stays inside its cell's area so it never walks into an unloaded cell).
- **Player.** `Explorer`: flies (WASD, mouse, Shift fast, Q and E down and up) or walks (Tab: a `CharacterController`,
  the make-a-game skill's first-person walker); a `StreamingSource`; teleporting (the map's landmarks on 1-5) prefetches
  with `World::RequestCellsAround`, and the fade ends and walking resumes only once `World::IsLoadedAround` holds and a
  downward `Physics::Raycast` at the destination hits the Terrain layer.
- **Day and night.** `DayNight` turns the sun (a full day in `DayLengthSeconds`, default 600) in steps of at most 0.25
  degrees, so the shadow cascades are not redrawn every frame, and drives the procedural sky's colors and intensity itself
  (they are constants of `SkyLightComponent`); campfire lights come on at dusk.
- **Tour.** The game starts behind the export's loading scene (progress from `Game::GetSceneLoadProgress`), then with a
  camera tour over 8 waypoints (Catmull-Rom, low over the forest so cells stream in and out); any key or click hands over
  control (`TourOnStart` field, which the editor regression turns off).
- **UI.** HUD (mode, time of day, cells loaded, the controls), pause menu (Resume, day speed slider, restart the tour,
  Credits with the Fox's attribution, Quit).
- **Audio.** Campfires, waves near the shore (a 3D source on the water's edge of each beach cell) and an ambience loop
  when a CC0 one fits.

Script fields the checks read: `Explorer.Grounded`, `HeightAboveGround`, `DistanceWalked` (metres since reset),
`StartAreaLoaded` (`World::IsLoadedAround` the start position, each frame), `TeleportsDone`; a fox's `Wander.Distance`
(metres since reset) and `Wander.StateIndex` (Survey 0, Walk 1, Run 2).

Acceptance (`StrataEditor.OpenWorld`, `--no-gpu`):
- `world.info`: at least 80 cells with content; the Vegetation layer's cells hold at least 20,000 entities; layers as
  listed; the persistent scene holds no props (its entity count at most the terrain tiles plus 50).
- `play.start` with `TourOnStart` false: `until` `world.status`'s `/playing` shows at least 9 loaded cells and none loading
  within 900 steps.
- Tab (walk): `until` `Grounded` is true within 300 steps; W held 120 steps: `DistanceWalked` at least 4 and
  `HeightAboveGround` at least -0.1 (never below the terrain).
- Teleport to landmark 3 (500 m away): `until` `TeleportsDone` 1 and `Grounded` within 1,800 steps; `StartAreaLoaded` false
  (the start's Vegetation cells are gone).
- A fox near landmark 3 (`entity.find`): `StateIndex` at least 0 (a known state); `Distance` reset, over 300 steps at least
  1.
- `DayLengthSeconds` set to 20 (`script.setField`): within 900 steps a campfire's `PointLight` intensity is above 0.
- Export full and pruned (cooked cells in the pack, the loading scene); both games headless for 1,800 frames with
  `--perf-log`: they print "OpenWorld: tour reached waypoint 8"; `check-perf-log` sees nonzero `WorldCellsLoaded` and
  `World.Streaming` and no row with `PhysicsBodiesWaiting` above 0. Debug runs a shorter tour (a field) if CI's Debug job
  cannot finish 1,800 frames in its timeout.
- `gpu`: the exported game windowed for 1,200 frames at 1920 x 1080 (`--resolution`) and `--asset-budget-mb 128` (mip
  streaming under a budget): `check-image` with green dominant on at least 10% of the screenshot, and no error line; and
  an editor run with a GPU (`--headless`) at the start position: `editor.perf`'s `renderStats.gpuDriven` true, `instances`
  at least 5,000 and `drawCommands` at most 256 (thresholds confirmed at the first measurement and stated in the report: at
  most half the instances and twice the draw commands measured).
- Perf, information on the Mac (Release, quiet, under the perf lock): the tour's median and 95th-percentile frame time and
  GPU busy time at 1920 x 1080, as provisional metrics `PerfGPU.Samples.OpenWorldTour.FrameMedianMs`, `.FrameP95Ms` and
  `.GPUBusyMedianMs` (`{"Unit": "ms", "Provisional": true}` in `Budgets.json`, unit only: no Mac number goes there; target
  16.7 for the median). They are measured by `StrataRuntime.OpenWorldTourPerf` (labels `perf` and `gpu`, Release and Dist),
  a CTest with `FIXTURES_REQUIRED` on `StrataEditor.OpenWorld`'s export that runs the exported tour windowed with
  `--perf-log` and hands the log to a test helper mode (`--strata-test-helper=record-perf-log`) that computes the metrics
  and writes them to the perf results as every perf test does; never gated here.

### 4.5 Tetris

`Samples/Tetris` gains `README.md` (what it shows, controls, how it was made), `CREDITS.md` (its sounds: `Strata`) and
`Picture.png`; its scene, scripts and commands do not change.

### 4.6 The launcher and the index

Each sample's directory and its `Samples.json` entry land in the same commit, the entry appended after Tetris, so that
`StrataEditor.Install`'s check of `/samples/0` stays valid until the last step. Then `Samples.json` lists Third-Person
Adventure, Open World, Physics Playground, Tetris in that order (the richest first), each with its picture, and
`StrataEditor.Install` checks that `project.samples` lists the four ids and that the installed copies carry no `.strata`.
`ProjectSamplesTests` reads `Samples.json` (I2), so it needs no change per sample.

## 5. Making each sample (the method)

1. Pick and download the assets (section 3) into their own scratch directories; inspect them with scratch scripts
   (glTF JSON chunk: nodes, skins, clips, extensions, triangles; sizes); trim to what the sample uses.
2. Start a headless editor with a GPU on a private session directory (`STRATA_SESSION_DIR`); note `journal.latest`.
3. `project.create` (basic3d template), imports, materials, prefabs, scenes, UI (`ui.create`), controllers, worlds, export
   settings: commands only. Scripts are written in `Scripts/` and built with `script.build`.
4. Play with simulated input and `input.sequence`; look at every capture (`viewport.capture {"camera": "scene"}`, in play
   mode); fix what looks wrong. Save; copy `.strata/Thumbnail.png` to `Picture.png`; take the screenshots.
5. Write the regression command script against the saved scene's IDs; run it through `SampleProject.cmake`; check each
   expectation fails when the behavior it checks is broken (section 6).
6. Export, run the games headless and windowed and look at the windowed screenshot.

## 6. Tests

| Test | Label | What |
| --- | --- | --- |
| `Editor.CommandScript` (`until` cases) | | I1 |
| `Editor.Commands` (a tap and a click at time scale 0) | | I7 |
| `Editor.Samples`, the launcher's `Editor.UI` test | | I2 |
| `Samples.Content` | | I4, including its crafted-directory must-fail cases |
| `Asset.Gltf` (`UnlitMaterials`) | | I5 |
| `Scene.Serialization` committed scenes | | every sample's scenes and cells load cleanly and save back the same |
| `StrataEditor.Tetris` | `package` | I3, unchanged expectations |
| `StrataEditor.PhysicsPlayground`, `.ThirdPerson`, `.OpenWorld` | `package` | sections 4.2-4.4 (`RESOURCE_LOCK toolchain`, `TIMEOUT 1800`) |
| `StrataRuntime.PhysicsPlaygroundWindowed`, `.ThirdPersonWindowed`, `.OpenWorldWindowed` (+ their image and perf-log checks) | `gpu` | the exported games in a window (fixtures on the package tests' exports) |
| `StrataEditor.OpenWorldRender` | `gpu` | render statistics at the start position |
| `StrataRuntime.OpenWorldTourPerf` | `perf`, `gpu` | provisional tour metrics (Release), on the package test's export |

**Must-fail checks** (each done once by hand, the result in the report; the permanent ones are I1, I2, I4, I5 and I7's tests):
for every sample, break what each expectation checks and see the regression fail: a script that never counts dominoes or
coins; the goal trigger made solid; the ghost layer's collision turned back on; the controller's `Speed` parameter never
set (animator stays Idle); the crate made static; the Playground's ball prefab removed from `alwaysInclude` (the pruned
game logs a failed spawn: an unexpected error, and no "goal scored"); the attract mode's course load broken (the exported
Third-Person prints no "course loaded"); the Vegetation layer emptied (`world.info` fails); `StreamingSource` removed (no
cells load); a warning added to a script's start (the unexpected-message check fails). Restore each.

**Physics robustness** (for `physics-box3d`): checks wait by condition or in game time (`until`, `play.step`), never by frame
counts, and run with the game paused, so each step is a fixed step whatever the machine; the samples rely only on primitive
triggers, dynamic primitives and static (convex or non-convex) mesh colliders; thresholds are what any sound rigid-body engine reaches with margin (60 of 64 dominoes, tower drift 0.25 m, 4 of 5
balls through a wide funnel, 0 balls escaped over walls three times a ball's bounce height); scenes avoid edge cases
engines treat differently (stacks start separated by 1 mm and settle, no bodies start interpenetrating, no thin walls
against fast balls without CCD). The checks name the physical outcome in their `expect` message so the box3d stream can tell
a tuning difference from a bug. A threshold is never loosened to pass; a scene's tuning fields may change with a stated
reason.

**CI.** The package tests run on every CI job but "Linux (no Audio, no Physics)" (which excludes `package` and builds no
editor) and in the Windows Dist job. Each must finish within its 1,800 s timeout on CI's slowest runner (Windows Debug,
3 hardware threads); the report states their CI durations. `gpu` and `perf` tests run on the Mac only.

## 7. Docs and skills to update

- `AGENTS.md` (39,451 of 40,000 code points; the `physics-box3d` stream edits it too, so this stream adds at most 200 net):
  the `Samples/` row names the four samples; the testing bullet becomes "a change to a sample must keep its
  `Editor/<Sample>Commands.json.in` passing"; one asset-pipeline bullet: sample assets are CC0 or CC-BY, credited in the
  sample's `CREDITS.md` and in `ThirdPartyNotices.md` (`Samples.Content`).
- `Docs/DeveloperGuide.md`: Repository layout (the samples, their files and tests, `GenerateIslandTerrain.py` under
  `Tools/`); Testing (the sample runner, `Samples.Content`, the windowed and render checks); Deferred commands and command
  scripts (`until`, paused stepping); Simulated input (taps at time scale 0); Launcher and Templates and recent projects (sample pictures, the copy's thumbnail); Importers
  (`UnlitMaterials`); Packages (what samples add to the archives); a "Samples" paragraph: the rules of section 3 and 4
  (licenses, credits format, budgets, made through the editor, regression, picture).
- `Docs/Architecture.md`: the `ProjectSamples` paragraph (pictures, seeded thumbnails).
- `README.md` and `Docs/README.md`: the samples listed with one line each.
- `Docs/Roadmap.md`: the `manual-and-samples` item loses "a 3D sample made through the editor with a CTest that plays it"
  (done here; the people's manual and templates stay); open items gain what section 9 defers. The wave-5 Status entry is
  the integrator's.
- `ThirdPartyNotices.md`: the "Sample assets" table.
- Skills: `strata-make-a-game` (the new samples as worked examples next to Tetris; finding and crediting downloaded assets;
  the picture; `until` in regression scripts; recipes the samples proved: a physics sandbox's grab and shoot, impact sounds
  with rate limits, scattering thousands of props into a world with `prefab.instantiate {instances}`, terrain tiles in
  cells, a third-person character with a controller and footstep events, a main menu scene and a settings menu kept in a
  save file, a camera tour); `strata-editor-automation` (`until`; `project.samples`' `picture`); `strata-build-test` (the
  new CTests and labels).

## 8. Risks

- **AGENTS.md headroom** (549 code points) is shared with `physics-box3d`: this stream keeps to 200 net and moves details to
  the guide; the integrator trims if both land near the limit.
- **Physics tuning under box3d** (section 6): thresholds with margins; the samples' physics checks are the box3d stream's
  acceptance targets.
- **CI time:** three more package tests under the `toolchain` lock add an estimated 15-30 minutes to the Windows Debug job.
  Mitigations: one script build per test, no repeated exports beyond full and pruned, Debug-only shorter runs where a run
  is long (as `StrataRuntime.World` does).
- **Document size:** thousands of prefab instances in pretty-printed JSON cells (estimated 400-700 bytes each). Measured
  before scattering; the count is set to fit 20 MB; JSON compresses about tenfold in archives and in git.
- **Binary assets in git** (no LFS): up to 60 MB forever in history (open question 2).
- **Terrain without a terrain system:** mesh tiles with mesh colliders in the persistent scene; seams avoided by identical
  edge vertices and no LODs; collision gaps at tile edges are checked by the walk test.
- **Pop-in without fog:** distant vegetation is not loaded; streaming distances, the far plane and the tour's height keep the
  loaded edge far and low in the view, and the README says so.
- **Scattering into a world:** instances created where no cell is loaded stay in the persistent scene; the scatter script
  pins every island cell first and the acceptance bounds the persistent scene's entities.
- **Importers on untrusted files:** a crash is an engine bug to fix with a crafted-file test; the Kenney, Khronos and
  three.js files are widely used and parse with cgltf, stb_image and stb_vorbis today.
- **RobotExpressive's morph targets** (facial expressions) are skipped with a warning: the face stays neutral.
- **Day and night vs the shadow cache:** a turning sun invalidates cascades; stepped rotation keeps most frames cached; the
  report states the GPU cost.
- **MoltenVK:** each indirect draw becomes a Metal draw; about 30 unique meshes keep that small. Validation errors fail the
  `gpu` tests.
- **The Mac is not the reference machine:** perf numbers are information and provisional entries only.

## 9. Deferred (and why)

- Budgets of the provisional metrics: the Windows reference machine (away).
- Windowed runs and render checks on Windows and NVIDIA: CI has no GPU; the reference machine.
- Exported-game automation (command scripts and simulated input in StrataRuntime): a later wave-5 item; exported runs are
  checked by their logs, perf logs and screenshots.
- Quaternius' itch.io and Google Drive packs: not scriptable to download (open question 1).
- Terrain as height fields with sculpting and painting: `world-environment`; the sample uses generated mesh tiles.
- Particles (sparks, splashes, smoke): `vfx`; campfires use emissive embers and a light.
- Joints, ragdolls and vehicles in the playground: later physics work, after box3d.
- Key rebinding in the settings menu: `project-settings` (input actions).
- Facial morph targets: morph-target support (later).
- A macOS app bundle for exported samples: `game-shell`.

## 10. Deliver in this order

Every commit keeps every test passing (golden rule 2): a sample's directory and its `Samples.json` entry land together,
appended after Tetris.

1. I1 `until`, with its paused stepping (with docs and skill lines).
2. I3 the generic runner, Tetris moved onto it (the unexpected-message check, `--save-directory`).
3. I4 `Samples.Content`, with Tetris' README, CREDITS and `Picture.png` (the project's thumbnail; it needs no launcher
   change) and the "Sample assets" table.
4. I2 sample pictures in the launcher; `ProjectSamplesTests` reads `Samples.json`; the copying tests copy only Tetris.
5. Open World de-risk spike, before any sample: `Tools/GenerateIslandTerrain.py` (terrain tiles), a 20,000-prop scatter
   into a world, cook, export and a headless streaming run, in a scratch project; what it finds (document sizes, the
   scatter's pins, collider readiness, pop-in) goes into this spec, and the generator is committed if it works.
6. Physics Playground (assets, project, scripts, scene, UI, audio, regression, exports, windowed check, README, credits,
   picture, screenshots, notices).
7. **Batch 1:** CI green on `ci/showcase-samples` at the Playground's tip, which the report names: the integrator can land
   the infrastructure, Tetris' files and the Playground on their own, so `physics-box3d`'s sample acceptance (its D13) can
   start on them.
8. I5 `UnlitMaterials` (the Nature Kit; the Platformer Kit too if its GLBs carry the extension).
9. The Open World (the same as the Playground, plus the render check).
10. I7 taps at time scale 0.
11. Third-Person Adventure (the same).
12. `StrataRuntime.OpenWorldTourPerf` and the provisional metrics; the Mac measurements.
13. **Batch 2:** docs and skills completed, `Samples.json` order and the install test, the archive growth measured, the full
    Debug and Release suites, the lint, CI on `ci/showcase-samples`.

## 11. Merge notes

- Lands before `physics-box3d`, in two batches (section 10): batch 1 (infrastructure, Tetris' files, the Playground) can
  land alone, so `physics-box3d` need not wait for the Open World. That stream's acceptance includes
  `StrataEditor.PhysicsPlayground`, `.ThirdPerson` and `.OpenWorld`; their thresholds stay; scene tuning fields may change
  with a reason.
- Files shared with other streams: `AGENTS.md` (headroom), `Docs/DeveloperGuide.md`, `ThirdPartyNotices.md` (box3d replaces
  the Jolt row; this stream adds a separate table), `StrataTests/CMakeLists.txt` (new tests near `StrataEditor.Tetris`),
  `StrataTests/CoreShards.json` (one shard), `Samples/Samples.json`.
- No new host functions or components are planned: the script ABI stays at 142 host functions unless a sample cannot be
  built without one (then appended, and the feature test extended).
- The release archive grows by the samples' size (report): the integrator checks the release dry run's package notice.
