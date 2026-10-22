# Project settings and preferences

Strata keeps two kinds of settings apart:

- **Project settings** belong to the game. They are saved in the project file (`<Project>.stproj`), shared with everyone
  who works on the project, and go into the games you export.
- **Preferences** belong to you. They hold for every project you open on this computer and never reach a game: the code
  editor that opens your scripts, for example.

This guide is for people working in the editor; the [Developer Guide](../DeveloperGuide.md) ("Project settings",
"Preferences") explains how the engine keeps them.

## Project Settings

**File > Project Settings...** opens a panel beside the Scene view. A list of pages on the left (type in its search field
to find a page or a field), the selected page on the right:

- **Game**: the scene the game starts with, and the scene it plays while that one loads in the exported game (a loading
  screen). Pick each from the project's scenes.
- **Export**: what an export that packs only what the scenes use keeps anyway: assets and folders your scripts load by
  path. Type a path relative to the `Assets` folder (`Prefabs/Enemy.stprefab`, or a folder such as `Prefabs`) and press
  **Add**; the cross removes one.
- A page for each kind of setting the engine and your game define, grouped by category. **Reset to Defaults** sets a page
  back to the values it starts with.

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

Strata 0.5.0 kept the external editor in `EditorSettings.json` in the same folder. The first time a later editor starts,
it copies that setting into its preferences; from then on each version keeps its own, so an older editor installed beside
it keeps working as it did.
