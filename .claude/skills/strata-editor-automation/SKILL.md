---
name: strata-editor-automation
description: Drive the Strata editor from outside, as an AI agent, through StrataCLI or its MCP server - start or find an editor (headless), discover its commands, create projects, import assets, build scenes from entities, components and prefabs, play and inspect (with simulated keyboard and mouse input), undo, save, capture the viewport and export a playable game. Use whenever you need the editor to do something (build or change a game, check a scene, export) rather than change the engine's code.
---

# Driving the Strata editor (StrataCLI and MCP)

Every editor feature is a **command** (`entity.create`, `scene.saveAs`, `project.export`, ...). The editor serves
all of them over a local, token-authenticated JSON-RPC connection (`EditorAutomation`), and two clients reach it:

- **StrataCLI** (`StrataCLI[.exe]`, next to `StrataEditor` and `StrataRuntime`: in the Strata folder of a downloaded
  release, or in `build/<preset>/bin/<Config>/` of a source checkout): one command per call, JSON result on stdout.
- **MCP** (`StrataCLI mcp`): every command becomes a tool, `.` replaced by `_` (`entity.create` -> `entity_create`).

Both find the editor through its **session file** (`<user data>/Sessions/<pid>.json`, written by the editor,
holding the port and the secret token; `<user data>` is `%LOCALAPPDATA%\Strata` on Windows,
`~/.local/share/Strata` on Linux and `~/Library/Application Support/Strata` on macOS). You never handle ports or
tokens yourself.

## 1. Get an editor

```sh
StrataCLI status                      # is one running? lists known editor sessions (never the token)
StrataCLI launch --no-gpu             # start one without window and GPU, no project; waits until it answers
StrataCLI launch --headless --project <dir>   # existing project; no window, but rendering works (captures)
```

- `--no-gpu`: no graphics device at all (any machine, CI). `--headless`: no window but rendering works.
  Without either, the editor opens its window (a human can watch).
- Started by hand: `StrataEditor --no-gpu [--project <dir>]` (keep it running in the background). It serves
  automation by default; `--automation-port <n>` fixes the port (default: a free one), `--no-automation`
  turns it off. A headless editor runs until `editor.quit` (or a signal), or with `--idle-timeout <seconds>` until
  no client has been connected for that long; it paces itself at 60 frames/s. `launch` passes
  `--idle-timeout <s>` when given; editors started through MCP close after 10 minutes without a client.
- Several editors: pick one with `--project <dir>` on every call; otherwise the newest is used.
- `STRATA_SESSION_DIR` moves the session directory; editor and clients must agree on it (tests use it).

## 2. Call commands

```sh
StrataCLI list                              # every command with its one-line description
StrataCLI list --json                       # ... with the JSON Schema of each command's parameters
StrataCLI call editor.status                # start here: project, scene, play state, selection, undo, automation,
                                            # window, UI scale and frame rate (an idle windowed editor draws 30 per second)
StrataCLI call entity.create '{"name": "Player"}'
StrataCLI call entity.create @params.json   # params from a file (no shell quoting problems)
echo '{"frames": 60}' | StrataCLI call editor.wait -     # params from stdin
StrataCLI call viewport.capture --save-image shot.png    # image results are written to a file (needs rendering)
```

- On Windows, quoting JSON on the command line is fragile (PowerShell strips inner quotes): prefer `@file` or `-`.
- Exit codes: `0` success (result on stdout); `1` the editor answered with an error (`{"code", "message",
  "data"}` on stderr); `2` no editor reachable or the connection was lost; `3` invalid command line; `4` no
  answer within `--timeout` (the editor is still there and the command may still finish: wait or call
  `editor.status`, do not start another editor); `5` `--save-image` could not write the image.
- `--timeout <ms>` (default 30000) bounds the wait for one call. Commands that take frames (`editor.wait`,
  captures, builds) answer only when they finish.
- The person at the editor sees you: the status bar reads "Agent connected · <your MCP client's name>" (StrataCLI calls
  name themselves "StrataCLI"), the viewport names your latest command ("Agent: entity.create · 3 s ago") and outlines
  what it changed for a moment, and the Activity panel (`editor.showPanel {"panel": "Activity"}`) lists every command
  you, the person and scripts ran, with Re-run and Undo to here.

### MCP

Register the server with your MCP client, e.g. `{"command": "<bin>/StrataCLI", "args": ["mcp"]}` (Claude Code:
`claude mcp add strata -- <bin>/StrataCLI mcp`; `<bin>` is the folder that holds StrataCLI, see above). Tools:

- `strata_status`, `strata_launch_editor` (`project`, `headless`, `noGpu`; without `project` it starts an empty
  editor, or reconnects to the empty one it started before), `strata_list_methods`, `strata_call` (`method`,
  `params`) are always there.
- While an editor is connected, each command is a tool with the command's schema as `inputSchema`. The tool list
  changes (`notifications/tools/list_changed`) when an editor connects or goes away.
- Results come as text plus `structuredContent`; `{"Image": {"MimeType", "Data"}}` results come as image content.
- Once connected, the server stays with that editor (also when it opens another project, or restarts with the same
  project) and never silently switches to another one.

## 3. Discover before you act

| Question | Call |
| --- | --- |
| What is open, is it playing, unsaved changes? | `editor.status` |
| Is a scene still opening, how far is it? | `scene.loadStatus` (`open` with `progress` and whether it is the project's `startScene`, `lastOpen` with how it ended, `play` for a switch a running game asked for, `subScenes` of the running game; `{"wait": true}` answers once nothing is opening); also `editor.status`'s `sceneLoad` |
| What did the person, other agents or I just do? | `editor.activity {since, limit, source, command}` (every command run: source UI / Automation with the client's name / CommandScript / Internal, parameters, result or error, undo step, entities touched); page with `since` = the returned `latest` |
| Are there unsaved changes a crashed (or discarding) editor left? | `scene.recoveryStatus` (entries with `id`, `sceneName`, `applicable`; the edited scene's `journal`), then `scene.recover {scene: <id or scene>, action: "apply" \| "discard"}` |
| Which projects did the user (or an agent) open lately? | `editor.recentProjects` (name, project file path for `project.open`, `lastOpened`; editors run with `--commands` or `--frames` list what they opened without saving it); `editor.removeRecentProject {path}` takes one off |
| What can a new project or scene start from? | `project.templates` (`empty`, `basic3d`) |
| Is there a finished game to learn from? | `project.samples` (e.g. `Tetris`); `project.openSample {sample, directory}` opens a copy in a new directory (the sample stays as shipped; `play.start` builds its scripts first) |
| Does the person at the editor see the launcher or the editor? | `editor.status` (`editor.launcher`; also `editor.gpu` and `editor.startupSeconds`) |
| Did the game ask for fullscreen or a resolution, lock the cursor, where do its saves go? | `editor.status` (`game`: `windowRequests`, recorded but not applied in play mode; `cursor`; `saveDirectory`, the project's `.strata/Saves`) |
| Which commands and parameters exist? | `editor.commands` (or `list --json`) |
| Which components and properties (types, ranges, enum values)? | `component.list` |
| Which assets exist? | `asset.list` (`type`, `path` filters), `asset.info` |
| What uses an asset (before deleting it)? | `asset.findReferences` (`asset.delete` refuses while anything does) |
| What does a material hold, which properties exist? | `material.get` (values, and each property's type, range, options) |
| What state do the scripts hold (a score, a timer)? | `script.get` (live field values while playing) |
| What is in the scene? | `scene.hierarchy`, `entity.find` (`name`, `tag`, `component`), `entity.get` |
| What does the person have selected, what does the inspector show? | `selection.get` (`entities`, the last is the primary one; `inspectedAsset` while the inspector shows an asset); `selection.set {entities?, asset?}` changes them |
| What happened (errors, script output)? | `log.read` (`after` = the previous `latest` to page; `minLevel`) |
| How much memory do assets use, what is loaded or streaming? | `asset.stats` (`assets: true` lists them; `sort`, `limit`); `asset.setBudget` changes the open project's budgets (MB of memory, staging and uploads per frame; ms of finalization and of upload steps per frame) |
| Where does the frame time go (scripts, physics, uploads, rendering, UI)? | `editor.perf {frames?}` (per stage `avgMs`/`p50Ms`/`p95Ms`/`maxMs`, the frame, FPS, job threads' busy %) |

Commands added at run time (for example `script.*`, `viewport.*`, `camera.*` when those features are present)
appear in `editor.commands` and as tools automatically. The viewport commands need rendering: they fail in an
editor started with `--no-gpu` (use `--headless`):

- `viewport.capture {width?, height?, camera?: "editor" | "scene", position?, target?, fov?, overlays?, debugView?,
  waitForAssets?, path?}` returns `{"Image": {"MimeType": "image/png", "Data"}, width, height, camera, view, waitedFrames,
  overlays, pendingAssets, texturesBelowWanted, stalePoses, pendingTextGlyphs, previewLighting, screenSpaceTexts,
  hiddenScreenSpaceTexts, debugView, notice?, path?}`. Text is complete in the picture, and so are the view's meshes,
  materials and textures: the capture renders again for up to about five seconds of frames while they load, up to about
  two more while textures still lack the finer mip levels the view wants, and a few while animated characters that
  animate only while seen (`CullingMode: WhenVisible`) still show an older pose, also while paused (`waitForAssets: false`
  takes it at once; `pendingAssets` says what was still loading, `texturesBelowWanted` what was still blurrier than
  wanted, e.g. under a texture budget, `stalePoses` the animators drawn in an older pose). `position` +
  `target` (and `fov`) look from your own camera: the
  person's view (`camera.get`) does not move, and the result's `camera` is `"custom"`.
- **Debug views** show what streaming and level-of-detail selection do: `debugView: "streaming"` tints entities whose
  mesh, material or textures are still loading (yellow, Sulfur) or failed (red, Cinnabar) and draws a box where a mesh
  is not loaded yet; `"lod"` tints entities by the level of detail they are drawn at (green 0, blue 1, yellow 2, red 3+);
  `"mip"` tints entities drawn with textures by the mip levels their worst texture lacks against what the view needs
  (green: as needed, blue 1 level short, yellow 2, red 3+ or still loading; textures stream their mip levels to what
  the screen shows, so a picture taken right after the camera moved may be blurry where it is red); `"none"` is the
  plain image. Pass it to `viewport.capture` for one picture, or set it for the viewport with
  `viewport.setSettings {"debugView": ...}` (it then colors the person's viewport too, also the game view). The result's
  `debugView` counts what it found among the entities in view: `{view, tintedEntities, pendingEntities, failedEntities,
  placeholderBoxes, entitiesPerLOD: [LOD0, LOD1, LOD2, LOD3+], entitiesPerMipShortfall: [as needed, 1, 2, 3+]}`, so
  you need not read colors: e.g. capture with `"streaming"` until `pendingEntities` and `placeholderBoxes` are 0, or with
  `"mip"` until only the first entry of `entitiesPerMipShortfall` counts entities (every texture as sharp as the view
  needs).
- **Texture memory** follows the screen: `asset.stats {"assets": true}` lists each streaming texture with `residentMip`
  (the finest mip level resident, 0 the full resolution), `wantedMip` (the finest its views need), `mipCount` and
  `residentBytes`, and its `mipStreaming` totals say what the streamer did (`levelsRaised`, `levelsDropped`,
  `levelsDroppedForBudget`: of those, what a texture budget took, `bytesStreamed`, `assetsBelowWanted`: textures still
  coarser than needed, 0 once the view is complete).
- **Editor views are not the game's view.** With the editor camera outside play mode, a scene without any light (no
  directional, point, spot or sky light) is lit by *preview lighting* (a sun and a procedural sky that the game will not
  have: the result says `"previewLighting": true`), and the game's screen-space text (its HUD) is hidden
  (`hiddenScreenSpaceTexts` counts it). To see what a player sees, capture with `{"camera": "scene"}` (or while playing).
  `viewport.getSettings` / `viewport.setSettings {previewLighting?, gameUI?, grid?, selectionOutline?, sceneGizmos?,
  allColliders?, stats?, gizmo?, space?, debugView?, ...}` change these view settings (no undo step; saved with the
  project's editor state). Collider shapes show for the selection only unless `allColliders` is on; far away lights and
  cameras show as small icons.
- **Opening a scene takes frames.** `scene.open` reads the scene and loads everything it uses in the background while the
  editor goes on showing the edited scene; it answers once the opened scene replaced it (`entities`, `seconds`, and the
  `frameTimes` of the frames meanwhile), so give large scenes a long `--timeout`. Meanwhile the edited scene is read-only:
  edits and `play.start` fail with "is being opened" (wait for the answer, or watch `scene.loadStatus`). A second
  `scene.open`, `scene.new` or `project.close` cancels it (the first `scene.open` then fails as cancelled, `-32006`).
  Opening a project opens its start scene the same way: `project.open` and `project.openSample` answer once it is the
  edited scene (`startSceneOpened`; `startSceneError` says why not, the project then stays open with an empty scene). An
  editor started with a window and `--project` shows itself at once and opens the start scene meanwhile: call
  `scene.loadStatus {"wait": true}` before editing it (headless editors and `--commands`/`--frames` runs start with it open).
- **Sub-scenes (play mode).** `scene.loadAdditive {scene}` loads a scene asset into the running game next to what it holds
  (as scripts' `Scene::LoadAdditive` do) and answers once its entities exist: `{subScene, roots, totalEntities, slices,
  seconds, ...}`; they appear over several frames, at most the scene's `maxEntitiesInstantiatedPerFrame` per frame
  (`scene.setSettings`). Deleting one of its entities before its children appeared leaves those out (`skippedEntities`).
  Its entities keep their IDs, so loading the same scene twice fails; `scene.unloadAdditive
  {subScene}` removes one (also a failed one), and `play.stop` discards them all with the running copy.
- `camera.get`; `camera.set {position?, target?, yaw?, pitch?, distance?, fov?, near?, far?, flySpeed?}`;
  `camera.focus {entities?}` frames the given (or selected) entities. Every scene remembers its own editor camera; a
  scene opened for the first time is framed on what it renders, seen from the direction of its primary camera.
- A windowed editor without a project shows its launcher (recent projects, templates, samples, how to connect an
  agent) instead of the editor; opening or creating a project (`project.open`, `project.create`, `project.openSample`)
  shows the editor, and `project.close` returns to the launcher. Changes to the untitled scene are not visible to the
  person while the launcher shows: create or open a project first.
- A project opens at once even when its assets need importing (a fresh copy, a new engine version): the imports run in the
  background and their assets load once imported (a model's meshes and materials appear then). `editor.status` says how
  many are still importing (`assets.importing`), `asset.info` per asset (`importing`); wait for 0 (e.g. `editor.wait`
  between checks) before relying on sub-assets or exporting, which waits for them itself. Editors run with `--commands`
  or `--frames` import before the project opens.
- Game logic: write C++ scripts into the project's `Scripts/` folder (`script.create {"class", "fields"}` starts one) and
  use `script.build`, `script.status`, `script.list` (the source files with the classes each registers), `script.add`,
  `script.setField`, `script.reload` (see the `strata-scripting` skill, "The workflow in a game project"). Builds need
  CMake and a C++ compiler on the machine (`script.status` -> `toolchain`: the engine's in a source checkout, one found
  on the machine in a download); they work with `--no-gpu`. `script.open {"file", "line"}` shows a source line to the
  person in their code editor (`script.setExternalEditor` sets which: its program must be a path or a name in PATH, and
  the answer's `program` says which file it starts).

## 4. Conventions

- **IDs** are strings of 16 hexadecimal digits (`"00000000000000AB"`), for entities and assets alike.
- **Assets** are referenced by handle, by path relative to the project's `Assets/` directory
  (`"Materials/Red.stmat"`), or as built-ins: `Builtin/Cube`, `Sphere`, `Plane`, `Quad`, `Cylinder`, `Capsule`,
  `Cone`, `Torus`, `Builtin/DefaultMaterial`.
- **Component values** use the scene file format: `{"Transform": {"Translation": [0, 1, 0], "Rotation": [0, 90, 0],
  "Scale": [1, 1, 1]}}`. Rotations accept Euler degrees `[pitch, yaw, roll]` or a quaternion `[x, y, z, w]` (read
  back as a quaternion). Property names are case-insensitive. `component.get` shows the accepted format of every value.
  Values outside a property's range (`component.list`, `material.get` describe the ranges) are refused with `-32602`
  naming the range (`Text.FontSize must be between 1 and 1024, not 0.5`); nothing is clamped or changed.
- **Undo**: every successful mutating command is one undo step (`edit.undo`, `edit.redo`, `edit.history`, whose steps say
  what they changed: "Create 'Wall'", "Set Intensity of 3 entities"). Copies, pastes and prefab instances are named
  uniquely among their siblings ("Crate (1)"): read the names back from the results' IDs rather than assuming them. Material
  edits are steps of the same history, but they save their file themselves, so they never mark the scene unsaved.
- **Unsaved changes are never discarded silently**: `scene.open`, `scene.new`, `project.open`, `project.create`,
  `project.close`, `project.openSample` and `editor.quit` fail (`-32005`, naming the scene) while the edited scene has
  unsaved changes (`editor.status` `scene.modified`), also while a copy of it plays. Save first (`scene.save`, or
  `scene.saveAs` for a scene never saved), or pass `"force": true` when discarding them is what you want; their results
  report `discardedChanges`. A person may be editing beside you: never force away work you did not make. The person sees
  a toast naming your client ("Claude Code discarded unsaved changes") with a Recover button.
- **Recovery**: the editor journals the edited scene's unsaved changes in `.strata/Recovery` (written about 2 s after
  edits stop; `editor.status` `recovery.journal` has `lastWrite` and `pending`), and even forced discards are kept
  there and listed at once (the newest 8 discards stay on offer, older ones move to `Recovery/Discarded`; what a crash
  left always stays). When a project opens with changes an editor left behind, `scene.recoveryStatus` lists them;
  `scene.recover {"scene": "<id>", "action": "apply"}` reopens the scene (in the background, like `scene.open`; it fails
  while another scene is being opened) with them as one undo step (the scene stays unsaved: save
  it), `"discard"` drops them. Entries follow their scene by its handle, so moving or renaming a scene (`asset.move`)
  keeps its changes recoverable (`scenePath` is where the scene is now). Ask the person before recovering or discarding their changes, or show them the choice:
  `editor.showDialog {"dialog": "recovery"}`.
- **Play mode**: `play.start` runs a copy of the scene, after building the scripts when their sources changed or they
  are not built while scenes use them (it answers once the game plays, with `built`; a failed build fails it with the
  errors; `{"build": false}` plays what is loaded; while a scene is being opened it fails at once, so wait for
  `scene.open` first). Changes made while playing apply to that copy, carry a
  `warning` in the result, and are discarded by `play.stop`. Undo and redo are not available while playing;
  `scene.save` saves the edited scene, never the running copy.
- **Errors** (JSON-RPC codes): `-32602` invalid parameters (the error's `data.parameters` is the command's schema:
  fix the request and retry), `-32005` the request was valid but cannot be done now (message says why, e.g.
  "Nothing to undo", unsaved changes), `-32601` unknown command, `-32006` cancelled (editor closing), `-32603`
  internal error. Unknown or missing parameters are always errors, never ignored.

## 5. Workflows

**New game from scratch**: start from the `basic3d` template. It saves a lit start scene `Scenes/Main.stscene` (set as
the start scene) with "Main Camera" (primary, with an audio listener, at (0, 2, 6) looking at the origin), "Sun" (a
directional light with shadows), "Sky" (a procedural sky light), "Ground" (a 20 x 20 plane with the dark stone material
`Materials/Ground.stmat`, which the template adds to the project, and a static `BoxCollider` whose top is the plane at y = 0,
so dynamic bodies and character controllers stand on it) and "Post Process"; the result names it in `startScene`. Without
`template` the project is `empty` (no scene; build and save one yourself).

```text
project.create      {"directory": "<absolute dir>", "name": "Tetris", "template": "basic3d"}
material.create     {"path": "Materials/Red.stmat", "properties": {"BaseColor": [1, 0, 0, 1]}}
entity.create       {"name": "Block", "components": {"MeshRenderer": {"Mesh": "Builtin/Cube", "Material": "Materials/Red.stmat"},
                     "Transform": {"Translation": [0, 0.5, 0]}}}
scene.save          {}
```

`scene.new {"template": "basic3d"}` starts another scene the same way (save it with `scene.saveAs`). Move the camera or
sun with `component.set` (find their IDs with `entity.find {"name": "Main Camera"}`). The sky is a `SkyLight` with
`"Source": "Procedural"`: `ZenithColor`, `HorizonColor`, `GroundColor` (linear colors), `SunSize` (degrees) and
`SunIntensity`; its sun follows the scene's directional light.

**Assets**: `asset.import {"file": "<absolute path>", "directory": "Models"}` copies a file into the project and
imports it: models `.gltf`/`.glb` (also meshopt-compressed), textures `.png`/`.jpg`/`.tga`/`.bmp`/`.psd`/`.gif`/`.hdr`,
audio `.wav`/`.mp3`/`.flac`/`.ogg`, fonts `.ttf`/`.otf`. It runs in the background (copy on an I/O thread, import like
re-imports) and answers when the imports are done; take several with `"files": [...]` and whole folders (their tree,
`.meta` files and model files kept) with `"directories": [...]` rather than many calls (calls that overlap copy one after
the other). A `.gltf` that names separate `.bin` and image files
brings them along into a folder named after the model (`Models/Quad/Quad.gltf`, `Models/Quad/Quad.bin`, ...; files up
to two folders above it, such as `../Textures`, keep their layout), so it imports textured. A failed import **fails the
command** (the message names the file; the error's data has `items` with each `error`) and its copied files are removed
again, unless `"keepOnFailure": true`; the result lists `items` with their `assets`. `asset.info` reports import errors and warnings, the import
settings and what each accepts (`importSettingsProperties`: textures `Usage`, `Compression`, `Quality`, `MaxSize`,
`Filter`, `Wrap`, `GenerateMips`, `FlipVertically`; models `Scale`, `GenerateLODs`, `LODCount`, `MaxTextureSize`,
`TextureCompression`, `TextureQuality`, `OptimizeMeshes`, `Import*`; audio `LoadMode`; fonts and the engine's own files
have none). `asset.setImportSettings {"asset", "settings":
{"MaxSize": 1024}}` changes them and `asset.reimport` imports again; both import in the background (the editor keeps
running, the asset keeps its previous version until the new one is loaded) and answer when the import is done, failing
when it failed (`"wait": false` answers at once; `asset.info` says `importing`). Unknown settings, invalid values and
numbers outside a setting's range (e.g. `MaxSize` above 16384) are rejected with what is accepted, never clamped.
Textures are block-compressed by default (`"Compression": "Auto"`; the format follows `"Usage"`: `Color`, `NormalMap`,
`Data` or `HDR`; color and multi-channel data are BC7); `"Quality": "Fast"` imports large color textures several times
faster (opaque color as BC1, half the bytes, a little less exact; normal maps and data are stored the same at either
quality); `"Compression": "Uncompressed"` keeps exact texels, e.g. for pixel art or textures a test compares texel for
texel. Models expose sub-assets (meshes, materials) and are instantiated whole with `prefab.instantiate`.

**Moving and deleting assets** never loses work: `asset.findReferences {"asset"}` lists what uses an asset
(`referencedBy`: scenes, prefabs, models and materials by `path`, and the open scene from memory with `openScene: true`
and the `entities` that use it, also when that scene is not saved). `asset.delete {"asset"}` **fails while anything uses
the asset**, naming the users in the message and in the error's data (`referencedBy`); change those references first,
or pass `"force": true` to delete anyway (the references become missing: meshes fall back to the default material).
Deleting moves the file, its `.meta` and its cached import to `<project>/.strata/Trash` (kept 30 days) and is one undo
step: `edit.undo` brings the asset back with the same handle, so its references work again. Folders:
`asset.createFolder {"path"}`, `asset.moveFolder {"path", "to"}` (everything in it moves, every handle and reference
stays valid) and `asset.deleteFolder {"path", "force"?}` (refused while assets outside it or the open scene use its
assets); `asset.move {"asset", "path"}` renames or moves one asset (a name that differs only in case renames it in
place; its own path changes nothing). Each is one undo step (`edit.undo`, `edit.history`); the open scene cannot be
deleted. Undo and redo check the files first: `edit.undo` of a delete fails (the step stays, nothing changes) while
another file is at the deleted path, and `edit.redo` of one fails (dropping the step) when the path holds other assets
now or the delete would be refused now; the error says why.

**Hierarchy and prefabs**: `entity.create {"parent": "<id>"}`;
`entity.setParent {"entities": ["<id>", ...], "parent": "<id>" | null, "index": 0}` moves entities (in the order given;
`index` is the final position of the first one, also to reorder top-level entities or siblings; without it entities that
change parent go last) in one undo step; `entity.duplicate {"entities": [...]}` puts each copy right after its original;
`entity.copy {"entities": [...]}` returns `data` that `entity.paste {"data", "parent"?, "index"?}` turns into new
entities (fresh IDs, references among them remapped, also in another scene);
`prefab.create {"entities": ["<id>"], "path": "Prefabs/Block.stprefab"}` (the entities become an instance of it; `"link":
false` leaves them alone), then
`prefab.instantiate {"prefab": "Prefabs/Block.stprefab", "components": {"Transform": {"Translation": [2, 0, 0]}}}`
(returns `id`, the instance's root). Instances stay linked: change one instance (any of its entities) and
`prefab.apply {"entity": "<id>"}` writes it into the prefab, and every instance that still had the prefab's values takes
the new ones (one undo step for the file and the scene). Instances in other scenes follow when their scene opens next
(also after the prefab changed on disk; done by the time `scene.open` answers): those still as they were take the new
values (an undo step per prefab, "Update Instances of ..."; save the scene), modified ones keep theirs and are
`outdated`; `prefab.apply` refuses an
outdated instance (it would write its older values over the newer ones) unless `"force": true`: `prefab.revert` it
and make its change again instead. `prefab.status {"entity"}` says whether an instance differs
(`modified`, `outdated`, `changed`, `added`, `removed`), `prefab.revert {"entity"}` gives it the prefab's values again (IDs kept),
`prefab.unpack {"entity"}` makes its entities plain ones, `prefab.select {"entity", "asset"?}` selects the root or shows
the prefab. While playing, revert and unpack change the running copy (their answers carry a `warning`) and apply is
refused; while a scene is being opened, apply, revert, unpack and a linking `prefab.create` are refused like every edit. The root's transform and name always stay the instance's own. Imported models are instantiated the same way
but cannot be applied to (unpack them, or make a prefab of the instance).

**Placing**: `entity.create` and `prefab.instantiate` put an entity where its `Transform` says (the origin, or the
prefab's own place, without one); the editor's own menus put new entities on the surface under the view's pivot (the
camera's target) and send that translation, and prefabs dragged into the viewport rest on the surface under the cursor.
To rest entities on what is below them,
`entity.dropToGround {"entities": [...]}` moves each straight down until its bounds touch a surface (one undo step;
`unchanged` lists those with nothing below).

**Edit**: `component.add` (adds if missing, then sets values), `component.set` (component must exist),
`component.remove`, `entity.setActive`: each takes `"entities": [...]` instead of `"entity"` to change all of them in one
undo step (as the inspector does for a multi-selection); `entity.rename`, `entity.delete`, `scene.setSettings` (gravity,
timestep, and `physicsCapacity {maxBodies, maxBodyPairs, maxContactConstraints}` for scenes that spawn many bodies while
they play; 0 sizes a capacity from the bodies the scene starts with; set capacities in edit mode: they apply when play
starts; `maxEntitiesInstantiatedPerFrame` bounds how many entities sub-scenes create per frame, 5000 by default).
Collision layers: `scene.setSettings {"layerNames": {"1": "Player", "2": "Debris"}, "layerCollisions":
[{"layers": ["Player", "Debris"], "collide": false}]}` names layers and keeps pairs apart (`resetLayerCollisions: true`
lets everything collide again first; `scene.info` lists `physicsLayers`), and `RigidBody.Layer` and
`CharacterController.Layer` take a layer's name as well as its index (`component.set {"values": {"Layer": "Debris"}}`).

**Materials**: `material.set {"material": "Materials/Red.stmat", "properties": {"Roughness": 0.3}}` saves the material
file and is one undo step (`edit.undo` restores the file and what entities show); entities using it show the change in
the same frame. `"save": false` only previews the values in the loaded material (no file, no undo step) until a saving
`material.set` saves them too; `material.get` says `"saved": false` meanwhile. Built-in materials and those imported
with a model cannot be changed: make one with `material.create` and assign it.

**Animation**: an imported model with skins or animations comes with a skeleton and clips as sub-assets; instantiated,
its root entity is the animator (`Animator` component: `Skeleton`, `Controller`, `Clip`, `Speed`, `Loop`, `PlayOnStart`,
`ApplyRootMotion`, `CullingMode`), its skinned meshes follow it (`Skin`), and joints that carry rigid parts follow their
joint (`AttachToJoint`). `animation.info {"asset": "Models/Hero.glb"}` lists the model's skeleton (joint names) and
clips (handles, names, durations); `{"asset": <skeleton|clip|controller>}` describes one; `{"entity": "<id>"}` an
animator, and while playing its current state, normalized time, blending and parameter values. A controller is made
and changed without writing JSON files:

```text
animation.createController {"path": "Animations/Hero.stanimator",
  "parameters": [{"Name": "Speed", "Type": "Float", "Default": 0}, {"Name": "Jump", "Type": "Trigger"}],
  "states": [{"Name": "Idle", "Clip": "<clip handle>"},
             {"Name": "Run", "Clip": "<clip handle>", "Events": [{"Name": "Step", "Time": 0.3}]},
             {"Name": "Jump", "Clip": "<clip handle>", "Loop": false}],
  "transitions": [
    {"From": "Idle", "To": "Run", "Duration": 0.2,
     "Conditions": [{"Parameter": "Speed", "Mode": "Greater", "Threshold": 0.1}]},
    {"From": "Run", "To": "Idle", "Duration": 0.2,
     "Conditions": [{"Parameter": "Speed", "Mode": "Less", "Threshold": 0.1}]},
    {"AnyState": true, "To": "Jump", "Duration": 0.1, "Conditions": [{"Parameter": "Jump", "Mode": "Set"}]},
    {"From": "Jump", "To": "Idle", "ExitTime": 1, "Duration": 0.2}]}
component.set {"entity": "<root id>", "component": "Animator",
               "values": {"Controller": "Animations/Hero.stanimator"}}
animation.setController {"controller": "Animations/Hero.stanimator", "states": [...]}
                        (replaces the sections given; one undo step)
animation.preview {"entity": "<root id>", "state": "Run", "time": 0.25}
                  (edit mode: the viewport and captures show the pose, attached parts included;
                   answers once this pose shows)
animation.preview {"stop": true}
```

Modes: `Greater`/`Less` (floats, with `Threshold`), `True`/`False` (bools), `Set` (triggers, consumed when the
transition fires). `ExitTime` is the source state's normalized time (1: its end; below 1 on a looping state: that point
of every loop). Previews change nothing in the scene and need no undo; play mode poses the running copy instead. The
Asset tab edits controllers as tables (one undo step per edit) and shows skeletons and clips.

**Play and inspect**: `play.start` -> `editor.wait {"frames": 120}` (about two seconds) -> `entity.get` /
`component.get` / `log.read` -> `play.stop`. `play.pause {"paused": true}` and `play.step {"frames": n}` advance
a paused scene by fixed steps. `play.simulate` runs physics only.

**Frame cost**: every `editor.wait` result of a running editor has `frameTimes` (`count`, `medianMs`, `meanMs`, `p95Ms`,
`maxMs`): the CPU time of the frames that ran entirely during the wait, without the waits for the GPU and vsync (measure
with a window, not `--no-gpu`, to include rendering). A command script can gate it:
`{"command": "editor.wait", "parameters": {"frames": 120}, "expect": {"/frameTimes/medianMs": {"max": 5}}}`.
`editor.perf {"frames": 120}` then says where the time went, per stage of the main thread (self times in ms:
`Input`, `Assets.Update`, `Assets.Finalize` (GPU uploads of arriving assets), `Scene.Load` (swapping in a scene that
finished loading, creating sub-scenes' entities), `Scene.Scripts`, `Scene.Animation` (animators: poses, root motion,
attachments, events), `Scene.Physics`, `Scene.Audio`, `Scene.Transforms`, `Render.Extract`, `Render.Submit`, `Editor.UI`, and `Present` outside the frame
time), with `stageSum` (at most the frame), `untimed` (the rest of each frame) and `jobs` (`workerBusyPercent`,
`ioBusyPercent`, `backgroundBusyPercent` (imports, exports), `running`, `queued`): `{"expect": {"/stages/Scene.Scripts/p95Ms": {"max": 2}}}` gates one stage. The
person at the editor sees the same in the Performance panel (click the frame pill) and the streaming numbers of
`asset.stats` in the Streaming panel (click the assets pill); `editor.showPanel {"panel": "Streaming"}` (or
`"Performance"`, `"Console"`, `"Activity"`, ...) brings one to the front for them, also as the first command of a script
(it fails while no project is open: the editor then shows its launcher instead of panels); `{"panel": "ContentBrowser",
"scripts": true}` shows the browser's Scripts root, the script files you wrote with their classes. `editor.showDialog {"dialog":
"export" | "newProject" | "openSample" | "recovery" | "newScript" | "externalEditor" | "about"}` opens a dialog of the
window the same way ("recovery" lets the person choose what to recover or discard, instead of you choosing for them;
"newScript" and "externalEditor" let them name a script class or pick the program that opens scripts).

**Play it (simulated input)**: while the game runs (`play.start`), the `input.*` commands press keys and buttons the way a
player would; scripts see them through `Input::IsKeyDown/IsKeyPressed/IsKeyReleased` (also while the viewport has no focus).

```text
input.key         {"key": "Left"}                         # tap: down for 1 frame, answers after the game saw the release
input.key         {"key": "Space", "frames": 30}          # held for half a second
input.key         {"key": "Right", "action": "press"}     # held across commands ... until
input.key         {"key": "Right", "action": "release"}
input.mouseMove   {"position": [640, 360]}                # pixels from the game view's top-left corner
input.mouseButton {"button": "Left"}                      # Left, Right, Middle, Button3..7; same actions as keys
input.scroll      {"delta": [0, 1]}
input.gamepadButton {"button": "A"}                       # gamepad 0 unless "gamepad": 0..3; same actions as keys
input.gamepadAxis {"axis": "LeftY", "value": 1}           # stick up, held until replaced, "value": null or play.stop
input.gamepadAxis {"axis": "RightTrigger", "value": 1, "frames": 30} # half a second, then back to rest
input.state       {}                                      # what simulated input holds; input.releaseAll lets go of it
```

Keys are named like the SDK's `Key::` constants (`Left`, `Up`, `Space`, `Enter`, `Escape`, `A`, `D1`, `F5`, `LeftShift`;
case does not matter), gamepad buttons and axes like `GamepadButton::`/`GamepadAxis::` (`A`, `B`, `X`, `Y`, `LeftBumper`,
`Start`, `DPadUp`; `LeftX`, `LeftY` (+1 is up), `RightX`, `RightY`, `LeftTrigger`, `RightTrigger`); a gamepad the
commands used is connected for the game until play stops. Each command answers once the game has seen its input (`"seen": true`), so issue them one after another
and read the effect (`component.get`, `log.read`, `viewport.capture`). Notes:

- Input only reaches play mode (not edit or simulate); `play.stop` drops held keys.
- Frames are game frames. While the game is paused (`play.pause`), input waits for the next frame `play.step` runs (or for
  `play.pause {"paused": false}`) and arrives with its press: `input.key {"key": "Space"}` then `play.step {"frames": 1}` makes the
  game see Space pressed in that step, and released in the following one. Commands answer at once while paused
  (`"seen": false`), because waiting would block you from stepping; pass `"wait": true` to wait anyway.
- Each command ends only its own hold: overlapping taps of a key keep it down until the last ends; `"action": "release"` ends
  every hold of the key and `input.releaseAll` every hold of every button. Holds outlive your connection; the editor's status bar
  lists them in a pill that releases them when a person clicks it.
- A tap of 1 frame is one key press for games that act on `IsKeyPressed` or `KeyRepeat`; use `frames` (or press/release) to
  hold a key for repeats.

**Look at it** (needs rendering, not `--no-gpu`): `camera.focus` or `camera.set`, then
`StrataCLI call viewport.capture '{"camera": "scene"}' --save-image shot.png` and open `shot.png`; with MCP the
capture arrives as an image. To look from elsewhere without moving the person's camera, pass `position` and `target`
(`{"position": [6, 4, 6], "target": [0, 0, 0]}`). The capture waits for what it shows to load; `pendingAssets` above
zero means something was still loading after that: wait and capture again. Editor-camera captures can be lit by preview lighting and leave out the HUD (see section 3): judge the
game's look and its HUD from `"camera": "scene"` captures.

**Export**: `project.export {"directory": "<absolute dir outside the project>"}` writes `<Game>.stpak`,
`<Game>.stgame` and the runtime renamed `<Game>[.exe]` (`"includeRuntime": false` skips it). It runs in the background
and answers when it finished (with `assetCount`, `packBytes`, `seconds`, `excludedAssets` and the editor's `frameTimes`
from the frame that started it); a large project takes a while, so give the call a long `--timeout`, and watch it with
`project.exportStatus` (progress 0 to 1 and the stage) from another call; `project.cancelExport` stops it. Mistakes
(a relative path, a directory inside the project, a file, unsaved changes) fail at once with what to do instead.
`"prune": true` ships only what the scenes use: assets the scripts load by path must then be named with
`project.setExportSettings {"alwaysInclude": ["Prefabs", "Sounds/Win.wav"]}` (folders or assets, saved in the project).
`project.setExportSettings {"loadingScene": "Scenes/Loading.stscene"}` names a scene the game plays while its start scene
loads (null: none; the game shows black until its start scene is in; never the start scene itself, which is refused, and
`project.setStartScene` of the loading scene clears that setting); exports keep it, also pruned ones.
Check the result headless: `<dir>/<Game> --headless --frames 120` (exit code 0). Games load scenes in the background
too: the first frames run before the start scene is in, so give short checks enough `--frames`.

**Repeat a session** (a regression script without writing one): note `journal.latest` of `editor.activity {"limit": 1}` (the newest
entry) before you start, work, then `editor.activity.export {"path": "Session.json", "since": <that latest>}`. It writes your successful
edits (and the person's, if they edited meanwhile) as a command script that rebuilds the result on a copy of the project
as it was when you noted the entry: `StrataEditor --no-gpu --no-automation --quit-after-commands --project <copy>
--commands Session.json`. Created entities and assets get new IDs in every run, so the script binds them where a result
gave them (`"bind": {"e12.id": "/id"}`) and refers to them later (`{"$ref": "e12.id"}`); queries of the scene and assets
you took IDs from (`entity.find`, `scene.hierarchy`, `entity.get`) are part of it. IDs you took from the selection or
`editor.status` stay as they are (the script does not repeat the person's selection): they work for what the project
already had. Play mode itself and edits of the running copy are not part of it (`skippedWhilePlaying`), but what you
changed while playing that outlasts play mode is (materials and other assets, `scene.save`). Changes the person made in
the inspector, with gizmos or in the material editor have no command, so no script holds them: `editsOutsideCommands`
(and a `warning`) counts them, and `unrepeatableEdits` counts what `scene.recover` applied (its recovery file is not in
a copy). Write your own scripts the same way: `"bind": {"crate":
"/id"}` on a step, `{"$ref": "crate"}` (or `{"$ref": "copy", "pointer": "/entities/0"}`) in any later parameter; a name
no earlier step binds fails the script before it starts.

**Finish**: save (`scene.save`), then `editor.quit`. Like every command that would replace the scene, it refuses while
the scene has unsaved changes; `{"force": true}` discards them. The editor answers, closes and removes its session file.

## 6. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `No running Strata editor was found` (exit 2) | Start one (`StrataCLI launch --no-gpu`). The editor and the client must use the same `STRATA_SESSION_DIR` (and user). |
| Several editors, the wrong one answers | Pass `--project <dir>` to every call (MCP: `strata_launch_editor` with `project`). |
| `-32602 Unknown parameter 'x'` | Misspelled or unsupported parameter; read `data.parameters` or `editor.commands`. |
| A call times out (exit 4) | The command takes many frames (or the editor is busy): raise `--timeout`. The command still finishes in the editor; do not launch another one. |
| Changes vanished after `play.stop` | They were made while playing; make them in edit mode. |
| `scene.open`, `scene.new`, `project.*` or `editor.quit` fail with unsaved changes | `scene.save` / `scene.saveAs` first, or `{"force": true}` to discard them. |
| Headless editor exits at once with code 1 | Its automation could not start (session directory not private, port in use); read its output or `<user data>/Logs/StrataEditor.log`. |
| The editor went away on its own | It was started with `--idle-timeout` (MCP: 10 minutes) and no client stayed connected; start it again. |
| `viewport.*` / `camera.*` fail or are missing | The editor runs with `--no-gpu` (restart it with `--headless`), or this build has no viewport commands (check `editor.commands`). |
| The capture is lit, but the game is dark | The capture says `"previewLighting": true`: the scene has no light of its own. Add a directional light and a sky light (or start from the `basic3d` template); check with `{"camera": "scene"}`. |
| The HUD is missing from a capture | Editor views hide screen-space text (`hiddenScreenSpaceTexts`); capture with `{"camera": "scene"}`, while playing, or turn on `viewport.setSettings {"gameUI": true}`. |
| Script or import errors | `log.read {"minLevel": "Warn"}`, `asset.info`. |

Session files of editors that exited are removed by the clients automatically; never edit or copy them (they
hold the token).
