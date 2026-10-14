<!-- Replacement text for Docs/Roadmap.md: the "### Wave 4" section and the "### Later (wave 5 and beyond)" section,
from "### Wave 4" to the end of the file. The Pillars, Status and earlier waves stay as they are. -->

### Wave 4

Planned after five audits of wave 3's tree, by a person and by an AI agent each making a game, and by the rendering,
scale and streaming, and architecture lenses. The three streams planned here before stay; the scale audit split
world-partition in two, and two streams join for what people and agents meet first. Before they branch, main takes
wave 3's finish work, the release packaging (0.3.0) and a CI that is green on Windows, Linux and macOS. The streams
merge in this order, and the wave ends with release 0.4.0.

- **`engine-modules`: Engine modules: EngineModule lifecycle, per-layer libraries, module-owned components, a
  tooling-free runtime, faster builds, optional subsystems.** Turn the layer table into the build.
  - Subsystems (graphics, audio, ImGui, assets) start and stop through an EngineModule lifecycle, in dependency order,
    instead of code in Application (the allowlist from 8 exceptions to 1 or 0).
  - Each layer of `Layers.json` becomes its own static library, generated from the table, so a library links only what
    its layer may include.
  - StrataRuntime structurally excludes Dear ImGui, ImGuizmo, the asset pipeline and the network layer, with a CTest
    that checks it. Today the runtime links 425 KiB of ImGui, and every program starts 48 threads.
  - Every module declares and registers its own components. Ordering constraints that name a system a build lacks are
    soft, and Audio, Physics, Animation and Scripting can be left out of a build.
  - Precompiled headers for the editor and test libraries and a sharded Core CTest (1,672 cases in one serial process
    today) cut the clean build and the test run.
  - Per-scene contexts move to wave 5, after the renderer and scene loading this wave rewrites; new code reaches the
    asset manager through the scene.
  - Depends on `module-foundation`, `physics-at-scale`, `mip-streaming`, `scene-streaming`, `content-and-prefabs`,
    `animation`.
- **`world-io`: World I/O: saves off the frame, streamed and binary scenes, asset registration and imports at the scale
  of tens of thousands of assets.** The paths around the frame fail before a world exists:
  - saving a million entities freezes the editor for 22 s;
  - opening them peaks at about 4 GB;
  - registering a new asset costs 3-10 ms on the main thread (a 600-file import froze one frame for 3.6 s, and a fresh
    30,000-asset project timed out `StrataCLI launch`);
  - every import thread decodes a whole texture at once.

  What changes:
  - Saves snapshot the scene and write it in the background.
  - Scene documents stream in and out one entity at a time, and loads can be cancelled.
  - Exported scenes and prefabs are cooked into a binary form that loads straight into prototypes on workers (a
    million entities in under 1.5 s with under 300 MB of transient memory).
  - Registration's file work runs on I/O threads in batches, with automation answering while a first scan runs.
  - Imports run within a memory budget.
  - Pack version 3 bundles small entries (a 30,000-material pack is 10.7 times its content today).

  Depends on `scene-streaming`, `pack-v2`, `content-and-prefabs`, `agent-activity`.
- **`world-partition`: Layered world partition: data layers, grid cells, streaming sources, physics at world scale.**
  Deliver the signature feature.
  - A world is authored as one World asset organized into named data layers and cut into grid cells. The editor
    streams cells around its camera, edits them in place and saves the cells that changed.
  - Games stream them additively around streaming sources, within per-frame budgets for creation, destruction, bodies
    and proxies.
  - A cook writes each cell with its bounds, body count and asset closure.
  - Physics is sized for the most bodies any streaming radius holds, and grows between steps when something still
    overflows, so a streamed body never lacks collision. Today streamed bodies beyond the play-start capacity of 65,536
    get none.
  - Cells unload in slices, so no frame frees a whole cell (51-56 ms today for 25,000 entities).
  - A world can be much larger than memory and than one ECS registry (1,048,575 entities) while frames stay within
    budget.
  - HLOD and origin rebasing come later.
  - Depends on `scene-streaming`, `render-scene`, `physics-at-scale`, `pack-v2`, `streaming-observatory`, `world-io`.
- **`gpu-driven-rendering`: GPU instruments, shadows at scale, shared geometry, GPU culling and indirect drawing,
  camera-relative rendering.** Scale rendering to millions of instances and thousands of distinct meshes.
  - GPU timer queries come first: frame stages, the Performance panel, `editor.perf`, `--perf-log` and GPU-time gates.
  - Shadow casters are found in parallel and culled below a texel. 100,000 shadowed cubes cost 6-7 ms of CPU today.
  - Meshes live in a shared geometry pool, so 10,000 distinct meshes no longer cost 37,000 draw calls and 20 ms.
  - Proxies mirrored on the GPU are frustum-, size- and occlusion-culled and LOD-selected per view and per shadow
    cascade, then drawn with indirect draws, skinned proxies included, so the CPU cost of drawing stays constant in
    instance count.
  - Visibility feedback keeps asset requests and mip streaming following what is seen.
  - Rendering is relative to the camera, so worlds stay precise tens of kilometers out.
  - Render statistics and GPU times reach agents through `viewport.capture` and `editor.perf`.
  - Depends on `render-scene`, `mip-streaming`, `animation`, `streaming-observatory`.
- **`game-ui`: Game UI: canvases, layout, widgets, focus navigation, script events, editor and agent authoring.** Give
  games menus, HUDs and settings screens. Today the only UI is screen-space text sized in raw pixels.
  - Canvases have a reference resolution and safe areas. Rects have anchors and pivots.
  - Images (with nine-slice), text, buttons, toggles, sliders and layout groups are drawn in one batched pass.
  - Mouse, keyboard and gamepad focus navigation; script callbacks; unscaled time, so pause menus move.
  - A 2D canvas view in the editor and `ui.*` commands for agents.
  - Depends on `render-scene`, `gameplay-and-scripting`, `editor-authoring`.
- **`authoring-and-playtest`: Authoring fidelity and exact play-testing.** What people and agents make should behave
  as the editor shows it.
  - Skinned characters stand in their rest pose in edit mode, thumbnails and previews; whole classes of characters lie
    on their backs today.
  - A model's import scale reaches the instances already placed, and normal maps are recognized and checked.
  - Agents play-test exactly: `play.step` answers once its steps ran, input lasts game updates rather than render
    frames, and `input.sequence` drives whole input timelines.
  - Agents can query world transforms and bounds, and the running scene names itself.
  - The MCP server offers compact toolsets: today 121 tools cost about 27,000 tokens, close to clients' 128-tool limit.
  - A Game view with fixed resolutions sits beside the Scene view while playing.
  - Prefab instances keep per-property overrides while every prefab change reaches them.
  - Small fixes go to what a person meets every day.
  - Depends on `animation`, `content-and-prefabs`, `gameplay-and-scripting`, `agent-activity`, `editor-authoring`.

### Later (wave 5 and beyond)

The wave-4 audits rank these first for wave 5:
- **`render-quality`:** TAA with motion vectors, shadows of point and spot lights, reflection probes and screen-space
  reflections, height fog and a physical atmosphere, extracting the sun from HDR skies with documented light units,
  cascade blending, transmission and clearcoat. It builds on `gpu-driven-rendering`'s per-view culling and instance
  history.
- **`project-settings`:**
  - Project Settings and Preferences panels.
  - Input actions with rebinding; `game-ui`'s navigation becomes their first map.
  - Collision layers for the whole project.
  - The game's name, version, company and icon in the exported executable.
- **`game-shell`:**
  - Crash reports in shipped games.
  - Game settings with quality presets mapped to the renderer.
  - Exports per platform and configuration, with a macOS app bundle.
- **`manual-and-samples`:** a people's manual reachable from the Help menu, playable templates, and a 3D sample made
  through the editor with a CTest that plays it.
- **`scene-contexts`:** per-scene asset managers and script engines instead of process-wide active ones.
- **`vfx`:** particles, trails and decals on the GPU-driven path.
- **`mesh-streaming`:** Mesh v2 with quantized vertices, per-LOD streaming on the geometry pool, and a collision
  sub-asset, so physics stops depending on the renderer's Mesh.
- **HLOD:** merged, simplified cells with baked atlases for distant parts of a world.
- **`world-environment`:** terrain cooked into world cells, with height-field colliders and sculpt and paint tools.
- **`animation-authoring`:** previews and scrubbing of animators, clips and transitions in the Inspector and the Asset
  tab.
- **Exported-game automation:** command scripts, simulated input and expectations in StrataRuntime.

Further:
- JobSystem v2 with work stealing and a frame task graph, once `Perf.Jobs` shows a need.
- 64-bit entity handles and origin rebasing.
- Per-world audio listeners and input contexts.
- Native game modules and an exported engine package.
- Audio streaming from packs, and an audio mixer with buses and effects.
- Localization with string tables and font fallback chains.
- BC6H.
- ImGui multi-viewports.
- Physics joints, ragdolls and vehicles.
- Level-design tools beyond placement: orthographic views, surface and vertex snapping, hiding and locking in the editor
  only.
- OBJ and FBX import.
- Morph targets.
- A prefab edit mode.
- A timestep display and other editor polish.
