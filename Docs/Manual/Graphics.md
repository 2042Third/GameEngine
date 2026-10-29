# Graphics quality

Graphics quality is how much the renderer spends on drawing what a scene asks for: how fine the sun's and the lamps'
shadows are, how many lights cast them, how far meshes keep their detail. It never changes the look a scene asks for
(its lights, its Post Process and its sky stay as they are), and no setting renders below the screen's resolution: the
picture is always drawn at the size it is shown.

## The presets

Choose one in **Project Settings > Graphics Quality**. The page shows the presets side by side, a row per setting, with
what each one costs:

| Setting | Low | Medium | High (default) | Ultra |
| --- | --- | --- | --- | --- |
| Sun shadow resolution (each cascade) | 1024 | 2048 | Auto: 2048 below 1440 rows, 4096 from 1440 | Auto fine: 4096 below 1440 rows, 8192 from 1440 |
| Sun shadow cascades | 3 | 4 | 4 | 4 |
| Point and spot shadow atlas | 2048 | 4096 | 4096 | 8192 |
| Largest point and spot shadow tile | 512 | 1024 | 1024 | 2048 |
| Point and spot lights with shadows | 4 | 16 | 16 | 16 |
| Level of detail error (pixels) | 2 | 1 | 1 | 0.5 |
| Smallest drawn size (pixels) | 1 | 0.5 | 0.5 | 0.25 |
| Sun shadow filter taps (wide soft edges) | 8 | 16 | 24 | 32 |
| Contact shadow steps | Off | Off | 12 | 16 |
| Motion blur samples (scenes with motion blur) | Off | 8 | 12 | 16 |
| Shadow maps, atlas and motion blur targets of one view, 1920 x 1080 | 28 MiB | 152 MiB | 152 MiB | 536 MiB |
| Shadow maps, atlas and motion blur targets of one view, 3840 x 2160 | 28 MiB | 224 MiB | 416 MiB | 1.34 GiB |

- **Medium** keeps Strata 0.6.0's shadow map sizes and sample counts, without contact shadows. **High**, the default,
  adds smoother wide sun penumbrae and contact shadows at every size, and sharper sun shadows from 1440 rows on.
  **Ultra** is for large GPUs; **Low** for older and integrated ones.
- **Sun shadow filter taps** smooth the sun's soft shadow edges where they are wider than a shadow texel, and
  **contact shadows** ground small objects the shadow maps are too coarse for (each sun's Contact Shadow Length sets
  how far they reach: [Lighting.md](Lighting.md#shadows)). Neither takes memory of its own; both cost GPU time in the
  `GPU.ScreenShadows` stage.
- **Motion blur samples** apply only to scenes whose Post Process turns motion blur on (it is off by default, and in the
  template and the samples): more samples show less streak noise where things move fast, for GPU time in the
  `GPU.MotionBlur` stage. Its targets take 12 bytes a pixel (24 MiB at 1920 x 1080, 97 MiB at 3840 x 2160), which the
  memory rows count; Low turns motion blur off. [Camera.md](Camera.md) says what motion blur does.
- **Players' choices**, below the table, are the same under every preset: comfort and taste rather than cost.
  **Anti-aliasing** chooses the method whatever the scene uses (Scene: the Post Process's; or Off, FXAA, TAA or
  Morphological: [AntiAliasing.md](AntiAliasing.md)). **Motion blur**, **Depth of field** (as the camera's lens says),
  **Film grain** and **Chromatic aberration** are Scene or Off: players may turn off what the scene uses, never turn on
  what it does not.
- **Custom** sets each value yourself: the table's Custom column is editable. Choosing a preset keeps your Custom values
  for later, and the column keeps showing them. Editing it while a preset is chosen switches to Custom, starting from
  the values the column shows: your earlier Custom values, or that preset's when you have none (its tooltip says which).
- The memory is that of one view: each renderer holds its own (the editor's Scene and Game views and its captures, the
  exported game one); shadow maps exist only while a light casts shadows, motion blur targets only while a scene uses
  motion blur.
- The page also shows how long the Scene view's shadows and meshes take on the GPU now, so you can see a change's cost.

## Where quality is chosen

- **The project** (the page above, or `project.setSettings {"section": "Renderer.Quality", "value": {"Preset": "Low"}}`):
  what the exported game, the Game view and pictures of the scene's camera draw with. Each change is an undo step.
- **The Scene view** has a quality chip in its toolbar: **Project** follows the page, or choose a preset for this view
  alone, for your computer (`viewport.setSettings {"quality": "ultra"}` does the same). The Game view always draws as the
  game will, and so does the Scene view while it looks through the scene's camera (**Game camera**): its chip then names
  the game's preset, a player's choice while you play.
- **Pictures** (`viewport.capture`) draw at the quality of the view they show unless they ask for one
  (`{"quality": "ultra"}`).
- **One run of a game**: `<Game> --quality low` (or `medium`, `high`, `ultra`) plays at that preset without saving
  anything.

## Players' options

Players change quality in your game's options menu. A row of toggles is a few lines of script
(`StrataScript/Graphics.h`):

```cpp
void OnUIValueChanged(Entity widget, float value) override
{
	if (widget == UltraToggle && value != 0.0f)
	{
		Graphics::SetPreset(QualityPreset::Ultra); // Applies from the next frame
		Graphics::Save();                          // Kept for the next run
	}
}
```

`Graphics::GetPreset()` shows what is in effect, `Graphics::Options()` lists every setting with its title, description
and choices, `Graphics::Set("ShadowCascades", 3)` changes one (the preset becomes Custom, starting from what was in
effect), and `Graphics::Reset()` goes back to the project's quality. The choices last for the session until
`Graphics::Save()` writes them to `GraphicsOptions.json` beside the game's save files (in the editor: the project's
`.strata/Saves` folder); the game reads them when it starts. A player's preset replaces the project's: while a saved
choice exists, changing the project's preset does not show when you play in the editor. The Graphics Quality page says so
and offers **Reset Player Options** (`graphics.resetPlayerOptions`), which deletes the file. The Third-Person Adventure's
settings menu has a Graphics row made this way.

## Why there is no render scale

Strata draws every image at the resolution it is shown and never upscales it from a smaller one, temporally or
otherwise: an upscaled image is softer and smears in motion, which no quality setting should trade for speed. To spend
less, lower the presets' costs (shadows, lights, detail); to spend more, raise them.
