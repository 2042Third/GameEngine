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
- Along a stretched axis the drawer edits the **Left** and **Right** (or **Top** and **Bottom**) margins; **On Screen**
  shows the rect in pixels at the game's screen.
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

- **Mouse**: a click on a button clicks it; the game's own mouse input never sees clicks that land on the UI
  (`UI::IsPointerOverUI` tells a script the pointer is over it).
- **Keyboard and gamepad**: the arrow keys, the d-pad and the left stick move the focus between selectables (the nearest
  one in that direction, or the neighbours you set under **Navigation**), Enter, Space or gamepad A clicks, and Escape or
  gamepad B is **Cancel** for scripts to close a menu (`UI::IsActionPressed(UIAction::Cancel)`). The canvas's **First
  Selected** is where the focus starts.
- **Pause menus** work at time scale 0: the UI animates on real time (`Time::GetUnscaledDeltaTime` for your own fades).

## A main menu scene

A scene without a camera that has a canvas shows its UI over black: make the main menu a scene of its own with just the
canvas, set it as the project's start scene, and load the first level from the Play button.

## Test it

Play the game and use the menu as a player would. Tools and agents drive it with editor commands: `ui.create` builds
elements, `ui.layout` reports every element's rect at any screen size without rendering, `ui.hitTest` tells what a
click would hit, and while playing `input.click` clicks an element where the player sees it (and fails when something
covers it); see the [strata-editor-automation skill](../../.claude/skills/strata-editor-automation/SKILL.md).
