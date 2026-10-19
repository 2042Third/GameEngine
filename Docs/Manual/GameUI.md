# Game UI

Menus, HUDs, pause screens, health bars and settings in Strata are made of UI elements: entities with a **UI Rect**
under an entity with a **UI Canvas**. The game draws them over the scene at any screen size and handles the mouse, the
keyboard and gamepads for them. This guide is for people building UI in the editor; the
[strata-scripting skill](../../.claude/skills/strata-scripting/SKILL.md) ("Game UI") has the script recipes, and
[Architecture.md](../Architecture.md) ("Game UI") explains how the engine lays out, draws and hit tests it.

## Make a menu

**Hierarchy > + > UI** makes elements ready to use: a **Canvas**, a **Panel** (a rounded, framed background), an
**Image**, a **Text**, a **Button** with its label, a **Toggle** with its box, checkmark and label, a **Slider** with its
track, fill and handle, and **Horizontal**, **Vertical** and **Grid Layout** groups that place their children. An element
made outside a canvas gets a new canvas first, so whatever you make is drawn. (The world-space text you place in the scene
is **3D Text**.)

A typical main menu: a canvas, a panel in the middle, a vertical layout in the panel (anchored to stretch over it), and
a title text and Play, Settings and Quit buttons in the layout. The layout stacks them and sizes them for you.

## Place elements

Select an element: the Inspector's **UI Rect** section shows where it is.

- **Anchors** opens a grid of presets: corners, the middles of the edges, the center, and stretching along an edge or
  over the whole parent. An element keeps its place when you change its anchors; with **Position** on it moves onto them
  instead, and **Pivot** sets the point it is placed and scaled by to match (a top-left preset: the top-left corner).
- Anchors decide what happens when the screen changes: a score anchored to the top-right corner stays there on every
  screen; a bar stretched along the bottom edge keeps its margins and grows with the screen.
- Along a stretched axis the drawer edits the **Left** and **Right** (or **Top** and **Bottom**) margins, and along the
  other the position and width (or height), in place of **Position** and **Size**; **On Screen** shows the rect in
  pixels at the game's screen.
- The canvas's **Scale Mode** decides how the whole UI scales: **Scale With Screen** (the default) lays the UI out for its
  **Reference Resolution** and scales it to the screen (**Match Width Or Height** picks which of the two it follows), so a
  HUD keeps its proportions from 1280x720 to 4K; **Constant Pixel Size** keeps sizes in pixels; **Constant Physical Size**
  keeps them the same size on high-density displays.

Turn on **Game UI** in the viewport's toolbar to see the UI over the scene while you edit; a click on an element then
selects it.

The viewport's **2D** chip shows the canvas of the selection on its own, at its reference aspect ratio, as the game lays
it out: every element is outlined, the selected one has handles. Drag inside it to move it and drag a handle to resize
it (with **Snap** on, edges land on whole canvas units; Ctrl does the opposite of Snap). A click selects what is on top
there. Turn the mouse wheel to zoom in on the point under the pointer and drag with the middle button to move around;
**F** zooms in on the selected element and **Home** shows the whole canvas again. Zooming only magnifies: the UI keeps
the layout it has at its reference size.

## Make it look right

- **UI Image**: a texture or a solid **Color**, with **Corner Radius** and a **Border**; **Sliced** images keep their
  corners while they stretch (set **Slice Borders**); **Filled** images show a part (**Fill Amount**: health bars,
  cooldowns).
- **UI Text**: the text, **Font**, **Font Size** (canvas units), alignment, **Wrap**, **Overflow** (truncate, or end in an
  ellipsis) and an **Outline** that keeps text readable over any background.
- **UI Selectable** (on buttons, toggles and sliders): the colors of its states (normal, hover, pressed, focused,
  disabled), or images to swap, and the fade time between them.
- A canvas's **Sort Order** puts it in front of canvases with a lower one; a HUD that should never block clicks turns
  **Receives Input** off.
- **UI Mask** keeps an element's children inside its rect: a card whose content must not spill, a list that shows only
  what fits. **Padding** moves the clipping edges in (or out, negative), **Softness** fades the content out toward them.
  What a mask hides is not clicked either. Masks inside masks show only where both overlap.

## Make it do something

Buttons, toggles and sliders call the scripts on their own entity and on their **Event Target** (set it to the entity of
a menu script, so one script handles a whole menu):

```cpp
class MainMenu : public Script
{
public:
	AssetHandle Level;

	void OnUIClick(Entity widget) override
	{
		if (widget.GetName() == "Play")
			Game::LoadScene(Level);
		else if (widget.GetName() == "Quit")
			Game::Quit(0);
	}

	void OnUIValueChanged(Entity widget, float value) override
	{
		if (widget.GetName() == "Volume")
			Audio::SetMasterVolume(value / 10.0f);
	}
};
```

Scripts change what elements show with `entity.GetUI()`: `SetText`, `SetColor`, `SetImage`, `SetValue`,
`SetInteractable` and `Focus`. A HUD sets its texts every frame (setting the same text again costs nothing).

**Input Field** (Hierarchy > + > UI > Input Field) is a line the player types into: a name, a chat message, a code. A
click puts the caret there; typing, Backspace, Delete, the arrows, Home and End edit as in any text box (Shift selects,
Ctrl+A or Cmd+A selects everything), and Enter calls the scripts' `OnUISubmit` (on the field and its **Event Target**),
where `widget.GetUI().GetText()` reads what was typed. Escape stops editing. While the player types, the game's own key
input sees nothing, so typing a name never moves the character. **Character Limit** caps the length; the **Placeholder**
shows while the field is empty; text longer than the field scrolls to keep the caret in view.

- **Mouse**: a click on a button clicks it; the game's own mouse input never sees clicks that land on the UI
  (`UI::IsPointerOverUI` tells a script the pointer is over it).
- **Keyboard and gamepad**: the arrow keys, the d-pad and the left stick move the focus between selectables (the nearest
  one in that direction, or the neighbours you set under **Navigation**), Enter, Space or gamepad A clicks, and Escape or
  gamepad B is **Cancel** for scripts to close a menu (`UI::IsActionPressed(UIAction::Cancel)`). The canvas's **First
  Selected** is where the focus starts.
- **Pause menus** work at time scale 0: the UI animates on real time (`Time::GetUnscaledDeltaTime` for your own fades).

## UI in the world

A canvas can also be part of the scene: a shop sign, a computer screen, a name tag over a character. Set the canvas's
**Render Mode** to **World Space**: it becomes a flat panel at the entity's position and rotation, facing the entity's +Z
axis, **Reference Resolution** canvas units wide and high, each **World Units Per Unit** meters (0.01: a canvas 400
units wide is 4 m). The canvas's **Pivot** is the point at the entity's origin. Move, turn and parent it like any
entity; what is in front of it hides it, and the editor's viewport shows it with **Game UI** off too. Like 3D text it is
not lit and casts no shadow: its colors show as you set them.

The player clicks it through the game's camera, after any screen-space canvas over that point; sliders drag along it and
input fields work as on the screen. The keyboard and gamepad move the focus within one world-space canvas (set its
**First Selected**, or focus a widget from a script). The 2D chip edits screen-space canvases only: lay a world-space
canvas out in the viewport, or turn it to Screen Space while you build it.

## A main menu scene

A scene without a camera that has a canvas shows its UI over black: make the main menu a scene of its own with just the
canvas, set it as the project's start scene, and load the first level from the Play button.

## Test it

Play the game and use the menu as a player would. Tools and agents drive it with editor commands: `ui.create` builds
elements, `ui.layout` reports every element's rect at any screen size without rendering, `ui.hitTest` tells what a
click would hit, and while playing `input.click` clicks an element where the player sees it (and fails when something
covers it); see the [strata-editor-automation skill](../../.claude/skills/strata-editor-automation/SKILL.md).
