# Playing and testing in the editor

The editor plays your game without exporting it: **Play** (Ctrl+P) starts the edited scene, **Stop** ends it. What the
game changes while it plays is a copy: stopping brings the scene back as it was. This guide is for people working in the
editor; [Architecture.md](../Architecture.md) (Editor, "Viewport") explains how the engine does it.

## The Scene view and the Game view

The editor shows the world in two views, docked as tabs of one panel by default:

- the **Scene** view is the editor's camera: orbit (Alt + left drag), pan (middle drag), fly (right drag with WASD, Q/E),
  frame the selection (F). It shows the editor's overlays (grid, gizmos, outlines) and lets you pick and move entities,
  also while the game plays (you then change the running copy);
- the **Game** view is what a player sees: the scene's primary camera with the game's HUD, without editor overlays.

Pressing Play brings the Game view forward and gives it the keyboard, so the game gets your input; Stop brings the Scene
view back. Drag a tab beside the other to see both at once. To look through the game's camera in the Scene view (with the
overlays), press **F8** or the toolbar's **Game camera** chip.

A scene without a primary camera shows a notice in the Game view, as an exported game does: add a Camera component and
tick **Primary**.

While you edit, animated characters stand in their skeleton's rest pose, as the game shows them before their first
animation. The Scene view's **Camera** chip can show them at the first frame of what they play on start instead
(**First frame of their animation**); the choice is saved with the project and changes nothing in the scene.

## Resolution

The Game view's resolution menu (its toolbar) picks the shape of the image: **Free** fills the view, **16:9**, **16:10**,
**4:3** and **21:9** keep their aspect ratio with bars around the image, and fixed sizes (**1920 x 1080**, **1280 x 720**
or your own) render at exactly those pixels, scaled to fit. **Game's request** follows what the game asks for
(`Game::SetResolution` in scripts). Mouse positions the game reads are in the rendered image's pixels, so a HUD button is
hit where it is drawn at every size.

## Input and the mouse

The game reads the keyboard, the mouse and gamepads only while the Game view has the focus (click into it). A game that
locks the cursor (a first-person camera) takes the mouse; **Shift+F1** gives it back to the editor. The view says how for
a few seconds each time the game takes it.

## Pausing and stepping

**Pause** freezes the game; **Step** runs one fixed update and pauses again, to look at a moment frame by frame. The
status bar shows the play state, and the views' toolbars take its color while the game runs.

## Testing with an agent

Agents test games the same way, through commands: `play.start`, `play.step {steps}` (it answers once the steps ran),
`input.key`, `input.sequence` (a whole input timeline, counted in fixed steps, so a test repeats exactly),
`viewport.capture` (the Game view's image) and `play.stop`. The
[editor automation skill](../../.claude/skills/strata-editor-automation/SKILL.md) has the details.
