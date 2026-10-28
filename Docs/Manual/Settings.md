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
  instead of computing it again, which costs less and keeps piles and stacks steady. A large value can let a body seem
  held a little past an edge, or bump where floor tiles meet: lower it when bodies hop over seams or hang past edges, and
  0 turns recycling off. **Sleep threshold** is a speed in meters per second (0.05 by default): a body slower than that
  for half a second, with everything it touches, falls asleep and costs nothing until something wakes it; a larger value
  puts piles to sleep sooner but stops slow bodies (a ball rolling to a halt) early. Changes apply to the running game at
  once. Character controllers never reuse contacts, so what they carry or push always meets them where they are.
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
- **Graphics Quality** (under Rendering): how much the game's and the editor's views spend on shadows and detail. Pick a
  **Preset**: **Low** (smaller shadow maps, three sun shadow cascades, four point and spot lights with shadows, coarser
  levels of detail: for older or integrated GPUs), **Medium** (what Strata 0.6.0 drew), **High** (the default: as Medium,
  but sharper sun shadows on displays of 1440 rows and more, 256 MiB of shadow maps per view instead of 64) or **Ultra**
  (sharper shadows everywhere, larger point and spot light shadows, finer levels of detail: for large GPUs), or
  **Custom** to set each value yourself (the shadow resolution, the number of cascades, the point and spot lights' shadow
  atlas and largest tile, how many of those lights cast shadows, the level-of-detail error and the smallest size drawn).
  No setting draws below the screen's resolution, and a scene's look (its lights, Post Process and sky) stays the same.
  Exported games use it, and `<Game> --quality low` (or `medium`, `high`, `ultra`) plays one run at a preset without
  saving anything. The Scene view can draw at a preset of your own, for your computer alone, with
  `viewport.setSettings {quality}` ("project" follows this page).
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

Strata 0.5.0 kept the external editor in `EditorSettings.json` in the same folder. The first time a later editor starts,
it copies that setting into its preferences; from then on each version keeps its own, so an older editor installed beside
it keeps working as it did. An editor older than the one that last saved your preferences, if it cannot read them, leaves
them as they are: it uses the defaults and lets you change nothing, so the newer editor still has them.
