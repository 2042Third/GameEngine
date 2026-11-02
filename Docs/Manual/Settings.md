# Project settings and preferences

Strata keeps two kinds of settings apart:

- **Project settings** belong to the game. They are saved in the project file (`<Project>.stproj`), shared with everyone
  who works on the project, and go into the games you export.
- **Preferences** belong to you. They hold for every project you open on this computer and never reach a game: the code
  editor that opens your scripts, for example.

This guide is for people working in the editor; how the engine keeps them is in the engine's source, in
`Docs/DeveloperGuide.md` ("Project settings", "Preferences").

## Project Settings

**File > Project Settings...** opens a panel beside the Scene view. A list of pages on the left (type in its search field
to find a page or a field), the selected page on the right:

- **Game**: the scene the game starts with, and the scene it plays while that one loads in the exported game (a loading
  screen). Pick each from the project's scenes. Below them, who the game is: its **Title** (shown in the exported game's
  window bar and in its messages; empty uses the project's name), its **Version** (such as `1.0.0`: up to four numbers
  separated by dots), the **Company** that makes it and a **Copyright** line, and its **Icon**: a texture of the project
  made from a PNG, JPEG, TGA or BMP image, at least 256 x 256 pixels and square (a narrower image is centered on
  transparent pixels). A texture inside a model cannot be the icon: import its image as a texture of its own. The
  project's name keeps naming the folders the game keeps its saves and logs in, so changing the title or the company never
  loses a player's saves. On Windows the export also writes the game's identity into its executable: Explorer shows the
  icon, and the executable's Properties the title, version, company and copyright. The **Export Game** dialog sums them
  up, with a link to this page.
- **Export**: what an export that packs only what the scenes use keeps anyway: assets and folders your scripts load by
  path. Type a path relative to the `Assets` folder (`Prefabs/Enemy.stprefab`, or a folder such as `Prefabs`) and press
  **Add**; the cross removes one.
- **Collision Layers** (under Physics): names for the 32 layers rigid bodies and character controllers are on, and which
  layers collide. **Name a Layer** names the next free one (type over the name, Enter applies it; the cross removes it),
  and the grid has a box for each pair of the layers shown: clear it and their bodies pass through each other. The
  layers are the project's, the same in every scene and prefab; the Inspector shows how many are named when nothing is
  selected, with a link to this page. Scripts can change which layers collide while the game plays, until the next scene
  starts.
- **Simulation** (under Physics): how bodies settle. **Contact recycle distance** is a distance in meters (5 cm by
  default): while two touching bodies have moved less than that relative to each other, the physics reuses their contact
  instead of computing it again, which costs less and keeps piles and stacks steady. A reused contact keeps its old
  points, so a body can seem held a little past an edge (up to about 2 cm at the default) and bumps a few millimeters more
  where floor tiles meet: lower it when bodies hang past edges. Bodies that visibly hop over the seams of a tiled floor hit
  the tiles' edges, with or without reuse: make such a floor one collider, or a mesh collider, instead. 0 turns recycling
  off, which costs more and lets tall stacks topple; the distance does not change how soon piles fall asleep. **Sleep
  threshold** is a speed in meters per second (0.08 by default): a body slower than that for half a second, with
  everything it touches, falls asleep and costs nothing until something wakes it; a larger value puts piles to sleep
  sooner but stops slow bodies (a ball rolling to a halt) early. **Contact stiffness**, **Contact damping** and **Overlap
  push speed** tune how contacts push touching bodies apart: the defaults suit most games; a softer stiffness lets towers
  sag, less damping lets piles jiggle longer, and a slower push speed separates overlapping spawns gently but lets piles
  settle longer. Stiffness cannot go above what the game's fixed step allows: 30 at 60 steps per second (the default), 60
  at 120, so higher values change nothing. Changes apply to the running game at once. Character controllers and
  kinematic bodies (moving platforms, doors) never reuse contacts, so what they carry or push always meets them where
  they are.
- **Timing** (under Physics): **Fixed Rate** is how many fixed steps the game runs per second, whatever its frame rate:
  physics, character controllers, scripts' fixed updates and root motion advance at it. **Scene** (the default) keeps
  each scene's own **Fixed Timestep** (60 steps per second unless you changed it in the Inspector's scene settings); 30,
  60, 120 or 240 sets the rate of every scene and of the exported game. With a rate, a frame catches up at most 2/15 of a
  second of steps (8 at 60, 32 at 240) and a longer delay is dropped. The physics costs grow with the rate: 240 runs four
  times the steps of 60. At 30 contacts are soft: stiffness can reach only 15 instead of 30, so stacks sag and settle more
  slowly. A higher rate halves the small step-to-step stutter a fast character shows on a high refresh rate display, but
  does not remove it. While a rate is set, the Inspector shows the scene's Fixed Timestep and Max Steps per Frame greyed
  out, and "Fixed rate: the project's" with a link to this page.
- **Input Actions** (under Input): the game's named controls, which scripts read by name and players can rebind. Actions
  sit in maps (a set the game turns on and off together, such as Gameplay or Vehicle): **Add Map** adds one, and the
  selected map's name, whether it works when the game starts, and the bin to remove it are below the list. **Add Action**
  adds an action to the selected map; name it in place, choose its **Kind** (a Button is down or up, an Axis is one value
  from -1 to 1 such as steering, a Vector2 two values such as walking) and, for an Axis or a Vector2, its **Dead zone**
  (small stick movements that read as none). Its bindings are the controls that drive it: **+** adds one from the list of
  keys, mouse buttons and gamepad controls, or a composite made of two keys (an Axis: the negative and the positive one)
  or four (a Vector2: up, down, left, right); the cross removes one. The button beside **+** binds the next control you
  press (a key, mouse button or gamepad control), and so does the one beside each part of a composite; **Cancel**, or ten
  seconds without a press, stops listening, and the keys you press meanwhile trigger no editor shortcut. The map **UI**
  holds the actions the game's menus navigate with (Submit, Cancel and the four directions): you can rebind them, but not
  rename or remove them. When your game saved a player's rebinding while you played it, that player's bindings replace
  the project's in every play session after: those actions are marked **In play**, and **Reset Player Bindings** deletes
  the saved bindings so that your edits apply again.
- **Graphics Quality** (under Rendering, [Graphics.md](Graphics.md)): how much the game's and the editor's views spend on
  shadows, light and detail. The page shows the presets side by side in one table, a row per setting with what it costs
  in memory at 1920 x 1080 and 3840 x 2160 and the time its part of the frame takes now in the Scene view; the chosen
  column is highlighted. Pick a **Preset** with the chips above it: **Low** (smaller shadow maps, three sun shadow
  cascades, four point and spot lights with shadows, coarser levels of detail, lighter ambient occlusion, no
  screen-space reflections and no motion blur: for older or integrated GPUs), **Medium** (Strata 0.6.0's shadow map
  sizes and sample counts, no contact shadows, screen-space reflections at half resolution, only with TAA), **High**
  (the default: smoother wide sun penumbrae and contact shadows at every size, sharper sun shadows on displays of 1440
  rows and more, 256 MiB of shadow maps per view instead of 64, denser ambient occlusion, sharper reflections of the
  sky, 512 texels per face instead of 256, and screen-space reflections traced on every pixel with any anti-aliasing,
  about 232 MiB of video memory per view at 3840 x 2160 instead of 79) or **Ultra** (sharper shadows everywhere, larger
  point and spot light shadows, finer levels of detail, the densest ambient occlusion and longer screen-space reflection
  rays: for large GPUs), or **Custom** to set each value yourself in the table's Custom column (the shadow resolution,
  the number of cascades, the point and spot lights' shadow atlas and largest tile, how many of those lights cast
  shadows, the level-of-detail error, the smallest size drawn, the shadow filter quality: taps of the sun's wide
  penumbrae, the contact shadows' steps, the ambient occlusion's quality, the sky reflections' resolution, the
  screen-space reflections, and the motion blur's samples: Low has none, Medium 8, High 12, Ultra 16, for scenes whose
  Post Process turns motion blur on): choosing a preset keeps your Custom values for when you come back, and editing
  the column under a preset switches to Custom from the values it shows (your earlier Custom values, or that preset's
  when you have none). Five rows are the same under every preset, for players' comfort and preference, listed under
  **Players' choices**: **Anti-aliasing** (Scene: the Post Process's method; or Off, FXAA, TAA, Morphological),
  **Motion blur** (Scene: as the Post Process says; Off: never), **Depth of field** (Scene: as the camera's lens says;
  Off: everything in focus), **Film grain** and **Chromatic aberration** (Scene: as the Post Process says; Off: never).
  No setting draws below the screen's resolution, and a scene's own settings (its lights, Post Process and sky) are never
  changed: a preset only decides how finely they are drawn, and what costs too much at Low (screen-space reflections,
  motion blur) is left out.
  Exported games use it, and `<Game> --quality low` (or `medium`, `high`, `ultra`) plays one run at a preset without
  saving anything. Players can choose a preset in your game's options menu (`Graphics::SetPreset`, kept with
  `Graphics::Save`): when such a choice was saved while you played, a banner says what it sets, the rows it sets are
  marked **Player**, and **Reset Player Options** deletes it so that this page applies in play again. The Scene view can
  draw at a preset of your own, for your computer alone, with its quality chip (or `viewport.setSettings {quality}`;
  "Project" follows this page). With nothing selected, the Inspector shows the project's preset with a link here, and a
  sun's Inspector says how large its shadow maps are.
- A page for each kind of setting the engine and your game define, grouped by category. **Reset to Defaults** sets a page
  back to the values it starts with.

Strata 0.5.0 kept collision layers in each scene. The first time a later editor opens a project saved by 0.5.0, it makes
the layers its scenes name the project's (the start scene's first; where scenes disagree, the log says how, and a pair
any scene kept apart stays apart) and saves the project file in the new format, which 0.5.0 cannot open. Your scene
files stay as they are until you save them.

Every change takes effect at once and is saved in the project file right away, in the background. It is also a step of
the undo history: **Edit > Undo** (Ctrl+Z) brings the previous value back. Changing project settings never marks the
scene as unsaved. The panel can stay open while the game plays: the running game picks up the change, and it stays after
you stop (undo it once the game stopped).

If the project file cannot be written (a full disk, a file another program holds), the editor says so, keeps your changes
and tries again with your next change. Closing the project or the editor then asks whether to try again, discard the
changes or stay.

## Preferences

**File > Preferences...** opens them, from the launcher too. Changes apply at once and are saved in your user data
folder (`Preferences.json` in `%LOCALAPPDATA%\Strata` on Windows, `~/Library/Application Support/Strata` on macOS,
`~/.local/share/Strata` on Linux), which only your account may change. They are not part of the undo history: change
them back on the same page. **Close** or Escape closes the window.

- **External Editor** (also **Scripts > External Editor...**): the program that opens your script files, from the
  Problems panel, crash reports and New Script. See [Your code editor](Scripting.md#your-code-editor).
- **Interface**: the size of the editor's interface, as a percentage of the size Strata picks for your display
  (**Automatic**). It changes at once and holds when you move the window to another display. Starting the editor with
  `--ui-scale` overrides it for that session. It sizes the editor only, not the game you play in it.
- **Frame Rate**: how fast the editor redraws. **Idle after** is how long after your last input the editor counts as idle
  and redraws slowly (0: never idle; it then redraws at full speed all the time, also behind other windows, which uses
  more power). **Idle frame rate** and **Idle frame rate in the background** are how fast it redraws while idle, with its
  window in front or behind another. **Active frame rate** caps it while anything happens (Unlimited: as fast as the
  display's vertical sync allows); a cap above your display's refresh rate shows nothing more. Stepping a paused game
  and commands that wait for frames always run at full speed. The defaults are 0.7.0's: idle after half a second, 30
  frames per second idle, 10 in the background, no cap while active.

Strata 0.5.0 kept the external editor in `EditorSettings.json` in the same folder. The first time a later editor starts,
it copies that setting into its preferences; from then on each version keeps its own, so an older editor installed beside
it keeps working as it did. An editor older than the one that last saved your preferences, if it cannot read them, leaves
them as they are: it uses the defaults and lets you change nothing, so the newer editor still has them.
