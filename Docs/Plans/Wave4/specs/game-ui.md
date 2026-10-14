# Workstream `game-ui`: Game UI: canvases, layout, widgets, focus navigation, script events, editor and agent authoring (wave 4, size XL, new)

Read `common.md` first. A new workstream, merging **fifth**: after gpu-driven-rendering (rebase the UI pass onto its final
`SceneRenderer`), before authoring-and-playtest (which wires `input.sequence`, the Game view and the MCP toolsets to this
stream's work). Its host-function block and class callbacks come after world-partition's.

## Why

The architecture audit's critical finding, and the person and agent audits' major ones: a game made with Strata cannot have
a menu, a HUD that scales, a pause screen, a health bar or a button.

- **Screen-space text in raw pixels is the entire game UI.** `TextComponent` (`Scene/Components.h:229-239`) has `FontSize`
  in screen pixels per em, placed by `ScreenAnchor` and `ScreenOffset` in pixels. `TextRenderer::Prepare`
  (`TextRenderer.cpp:151-160`) builds `translate(anchor * viewport + offset) * scale(FontSize)`, with no reference
  resolution. Tetris' 40 px HUD (`Samples/Tetris/Assets/Scenes/Main.stscene`, `FontSize` 40, `ScreenAnchor` (0.05, 0.1))
  keeps its pixel size at 2560x1440, so it covers half its share of the screen
  (`audit/architecture/shots/tetris_1280x720.png` against `tetris_2560x1440.png`). None of the 27 components is a UI
  element; there are no images, panels, buttons, layout or focus.
- **Person M:** the Hierarchy's presets (`SceneHierarchyPanel.cpp` `GetPresets`) offer only Text.
- **Agent M F7:** FoxCoins (`audit/agent/game/FoxCoins`, `GameManager.cpp`) shows its pause menu and win screen as
  `Text` entities over the level, toggled with `SetActive`, with poor contrast (`audit/agent/shots/07_paused.png`,
  `06_win.png`, `12_final_win.png`). An agent cannot hit-test or click a UI element; the audit asked for `ui.hitTest` and
  `input.click {entity}`.
- **Agent m F14:** `StrataScript/Time.h` has only scaled time, so a pause menu at time scale 0 cannot animate; the SDK has
  no `LookAt` or `SmoothDamp`.
- **Architecture M "Input is raw devices only" (its UI part):** UI needs Submit, Cancel and Navigate on keyboard and
  gamepad, and scripts need typed text. `KeyTypedEvent` exists (`Events/KeyEvent.h:60`, raised by `GLFWWindow.cpp:217`),
  but only ImGui reads it; the SDK has no text input.

## Scope

### 1) The GameUI module

- **Folder and layer.** `Strata/src/Strata/GameUI/`, a new `GameUI` layer in `StrataTests/Architecture/Layers.json`:
  `Paths: ["Strata/GameUI/**"]`, `MayInclude: [Core, Input, Asset, Reflection, Scene, Renderer]`, listed after Animation
  and before Runtime, with `Notes` ("game UI: canvases, layout, widgets, navigation; the renderer reaches it only through
  `SceneOverlayRegistry`, scripts through `IUIControl`/`IUIEvents`"). `Engine`'s `MayInclude` gains `GameUI`. No other
  layer includes GameUI: `Runtime` does not need to (the overlay registry answers `GameRenderer`'s question, below), and
  Scripting must not. No allowlist line.
- **Namespace.** `Strata` with `GameUI`/`UI` prefixes on type names (`GameUIScene`, `UICanvasComponent`). Never a
  `Strata::UI` namespace in engine code: that is the editor's.
- **Registration.** `GameUI/GameUIRegistration.cpp`: `RegisterGameUIModule()` registers the components (Category `"UI"`),
  the scene system, the overlay pass and, after the rebase onto gpu-driven-rendering, the `GPU.UI` stage. One line in
  `Engine/BuiltinModules.cpp` at the end: after `RegisterAnimationModule()` on the base (the system's hard `Before`
  needs `Scripting` registered first), after world-partition's `RegisterWorldModule` row once engine-modules' table and
  world-partition have landed.
- **Files** (all new): `GameUIComponents.h/.cpp`, `GameUIRegistration.h/.cpp`, `GameUIScene.h/.cpp` (layout state per
  scene), `GameUILayout.cpp` (the layout algorithm), `GameUIText.cpp` (shaped-text cache), `GameUISystem.h/.cpp` (input,
  focus, navigation, events), `GameUIHitTest.h/.cpp`, `GameUIRenderer.h/.cpp` (the pass), `GameUIPresets.h/.cpp` (the
  preset hierarchies `ui.create` and tests build), `GameUIRenderData.h` (the GPU quad format shared with the shader).

### 2) Components (`GameUI/GameUIComponents.h`, registered in `RegisterGameUIModule`)

Never in `Scene/Components.h` (wave-4 rule). Coordinates are canvas units, +Y down, (0, 0) top-left, matching
`TextComponent::ScreenAnchor` and `Input::GetMousePosition`. Colors are display (sRGB) colors like `TextComponent::Color`.
An entity is a **UI element** when it has a `UIRect` and has a root canvas among its ancestors or is one. Defaults in
parentheses.

| Component | Properties |
| --- | --- |
| `UICanvas` ("UI Canvas") | `ScaleMode` enum `ConstantPixelSize`, `ScaleWithScreen`, `ConstantPhysicalSize` (ScaleWithScreen); `ReferenceResolution` vec2 (1920, 1080), 1 to 16384; `MatchWidthOrHeight` slider 0-1 (0.5); `ScaleFactor` (1), 0.01-100, multiplies the constant modes; `SafeAreaPadding` vec4 left, top, right, bottom in canvas units (0), insets the root rect; `SortOrder` int (0), higher draws over and gets input first; `PixelSnap` (true); `ReceivesInput` (true), false: drawn but never hit or navigated (a HUD that never blocks clicks); `FirstSelected` entity (none), focused when navigation starts |
| `UIRect` ("UI Rect") | `AnchorMin`, `AnchorMax` vec2 0-1 of the parent rect ((0.5, 0.5) both); `Pivot` vec2 0-1 ((0.5, 0.5)); `Position` vec2, the pivot's offset from the anchor point ((0, 0)); `Size` vec2, added to the anchored span ((100, 100)); `FitContent` enum `None`, `Width`, `Height`, `Both` (None): the rect takes its content's size (its `UIText`'s text, or its `UILayout`'s children); `IgnoreLayout` (false): a layout-group parent leaves it where its anchors put it |
| `UIImage` ("UI Image") | `Texture` asset (none: a solid color); `Color` (white); `Type` enum `Simple`, `Sliced`, `Filled` (Simple); `UVRect` vec4 u0, v0, u1, v1 ((0, 0, 1, 1)); `SliceBorders` vec4 texels left, top, right, bottom (0); `SliceScale` canvas units per border texel (1); `FillAmount` 0-1 (1); `FillDirection` enum `LeftToRight`, `RightToLeft`, `TopToBottom`, `BottomToTop`; `PreserveAspect` (false); `CornerRadius` canvas units (0); `BorderWidth` canvas units (0); `BorderColor` (black); `RaycastTarget` (true) |
| `UIText` ("UI Text") | `Text` multiline ("Text"); `Font` asset (none: the default font); `FontSize` canvas units per em (32), 1-1024; `Color` (white); `Alignment` `Left`, `Center`, `Right` (Center); `VerticalAlignment` `Top`, `Middle`, `Bottom` (Middle); `Wrap` (true); `Overflow` enum `Overflow`, `Truncate`, `Ellipsis` (Overflow); `LineSpacing` 0.5-3 (1); `OutlineWidth` em 0-0.1 (0); `OutlineColor` (black); `RaycastTarget` (false) |
| `UISelectable` ("UI Selectable") | `Interactable` (true); `Transition` enum `None`, `ColorTint`, `ImageSwap` (ColorTint); `TargetImage` entity (none: its own `UIImage`); `NormalColor`, `HoverColor`, `PressedColor`, `FocusedColor`, `DisabledColor`; `HoverImage`, `PressedImage`, `FocusedImage`, `DisabledImage` assets; `FadeDuration` seconds of unscaled time (0.1); `Navigation` enum `Automatic`, `Explicit`, `None` (Automatic); `NavigateUp`, `NavigateDown`, `NavigateLeft`, `NavigateRight` entities |
| `UIButton` ("UI Button") | `EventTarget` entity (none) |
| `UIToggle` ("UI Toggle") | `IsOn` (false); `Checkmark` entity (drawn only while on); `Group` entity (toggles naming the same group are exclusive); `EventTarget` |
| `UISlider` ("UI Slider") | `MinValue` (0); `MaxValue` (1); `Value` (0); `WholeNumbers` (false); `Direction` enum like `FillDirection`; `Fill` entity; `Handle` entity; `KeyboardStep` (0: a tenth of the range); `EventTarget` |
| `UILayout` ("UI Layout") | `Type` enum `Horizontal`, `Vertical`, `Grid` (Vertical); `Padding` vec4 (0); `Spacing` vec2 (0); `ChildAlignment` enum of the nine positions `TopLeft` ... `BottomRight` (TopLeft); `StretchChildren` (false): children fill the cross axis; `CellSize` vec2 (100, 100) and `Columns` uint (0: as many as fit), for grids |

Why `UISelectable` is a component of its own: the interaction states, transitions and navigation are the same for buttons,
toggles and sliders. Keeping them once keeps every widget's settings in one place in the Inspector and in `component.list`,
and the feature test covers them once. A widget preset is `UIRect + UIImage + UISelectable + UIButton` (or `UIToggle`,
`UISlider`). Entity properties use the reflection's `EntityProperty`, so paste and prefab instantiation remap them.

**`Requires`.** `ComponentInfoBuilder::Requires("UIRect")` (a new list in `ComponentInfo`, `Reflection/ComponentRegistry.h`)
on every UI component except `UIRect`: the paths that add components by name (`ComponentAccess`, used by `component.add`,
`entity.create`, the Inspector's Add Component and the scripts' `AddComponent`) add required components first, in the same
undo step. Without it, Add Component > UI Image on a plain entity shows nothing.

**Nested canvases and stray components.** Only root canvases (no canvas ancestor) define a screen. A `UICanvas` under
another canvas is laid out and drawn as a plain element of the outer one (its scale settings are ignored); a UI component
without a `UIRect`, or a `UIRect` outside any canvas, draws nothing. The Inspector says so (section 9).

### 3) Layout (`GameUIScene`, `GameUILayout.cpp`)

**Rect math** (exact; the layout tests check it to 1e-4). For a parent rect P (min, size), an element's anchor points are
`A0 = P.min + AnchorMin * P.size` and `A1 = P.min + AnchorMax * P.size`; its size is `max(A1 - A0 + Size, 0)`; its pivot
point is `A0 + (A1 - A0) * Pivot + Position`; its rect's min is `pivotPoint - size * Pivot`. Point anchors with `Size`
(200, 50) are a 200x50 rect; stretched anchors (0, 0)-(1, 1) with `Size` (-20, -20) inset the parent by 10.

**Scale modes.** A root canvas lays out for a **screen** (pixel size and content scale) at a scale s and a size in canvas
units `screen / s`:
- `ConstantPixelSize`: `s = ScaleFactor`;
- `ScaleWithScreen`: `s = 2^lerp(log2(W / Rw), log2(H / Rh), MatchWidthOrHeight)` (Rw, Rh: `ReferenceResolution`);
- `ConstantPhysicalSize`: `s = contentScale * ScaleFactor` (content scale 1 is 96 DPI).

The root rect is the canvas's unit size inset by `SafeAreaPadding`. Rects are cached **in canvas units**, and pixels are
`units * s + origin`: a window resized at the same aspect ratio lays out nothing again under `ScaleWithScreen`, and the
renderer's quads stay valid (section 5). `ui.layout` and hit testing report pixels.

**Layout groups.** A `UILayout` places its active children that have a `UIRect` and not `IgnoreLayout`, in sibling order
(`Scene::GetSiblingIndex`): Vertical stacks them from the padded top with `Spacing.y` between them, Horizontal from the
left with `Spacing.x`, Grid fills rows of `Columns` (or as many `CellSize` cells as fit) with `CellSize` cells. A child's
size along the main axis is its preferred size (its `Size`, or its content when it fits it); with `StretchChildren` it fills
the cross axis. `ChildAlignment` places the content block inside the padded rect. A group's anchors-and-position math does
not apply to the children it places.

**Fit content.** `UIRect::FitContent` makes the rect's size (on the chosen axes) its content's preferred size: a `UIText`'s
shaped text (unwrapped width when the width fits), a `UILayout`'s children block plus padding. Preferred sizes go bottom up,
rects top down.

**Widgets' parts.** A slider places its `Fill` (anchors along its direction from 0 to the normalized value) and `Handle`
(anchored at the value) in layout; a toggle draws its `Checkmark` only while `IsOn`; a selectable's tint applies to its
target image at render. None of these writes the scene: the layout and the pass read the widget's values, so they behave the
same in edit mode, play mode and under undo.

**The UI contract (dirty tracking).** Per-frame cost follows what changed, like the transform contract:
- `GameUIScene` (one per scene, in its registry's context, `GameUIScene::Get(scene)`, created when the scene first has a
  canvas) listens to `on_construct`, `on_update` and `on_destroy` of every UI component, reads
  `Scene::GetHierarchyMoves` (reparenting and sibling order) and `Scene::GetWorldTransformChanges` (activity changes;
  filtered by a membership bitset of UI element entity indices, so scenes with many moving bodies cost a bit test per
  change).
- **Code that writes UI component fields directly signals the change** (`MarkModified<T>()` or `registry.patch`;
  `ComponentAccess`, `SetPropertyWithUndo`, the script host and the UI system do). Values written right after
  `AddComponent` need no signal.
- A change marks the element; layout recomputes the dirty element's subtree, and walks up while the parent depends on its
  children (a layout group, or a fitted rect). A leaf moved under a plain parent lays out one element.
- Layout runs lazily, when the pass or the system asks for a screen (`GameUIScene::EnsureLayout(const UIScreen&)`), once per
  frame per canvas and canvas unit size. Each root canvas keeps up to `c_MaxLayoutsPerCanvas` (4) layouts keyed by its unit
  size (the game view, a capture of another aspect, the canvas mode), least recently used dropped, each with its own dirty
  marks.
- Debug builds compare the cached layout with a full recomputation after each `EnsureLayout`
  (`GameUIScene::Validate`); a mismatch asserts "The UI layout of scene ... differs from a full recomputation at entity
  ...", naming the entity whose write was not signaled. Tests call `Validate` (`CheckSceneCaches` gains it once a scene has
  canvases).
- Counters for tests: `GameUISceneStats { ElementsLaidOut, TextsShaped, QuadsBuilt, FullRebuilds }` since the last read.
- `GameUIScene` holds no `Ref`s to assets and no GPU objects (a scene may be released on a background thread): fonts and
  textures are held as handles and requested where used.

**Screens.** The UI's screen for input is the input viewport (`Input::GetViewportSize()`: the editor's game view, a game's
window), with the content scale from `IGameWindow` (`GameWindowState::ContentScale`, new, 1 without a window). When it is
empty (headless editors, `StrataRuntime --headless`, tests without a window) the screen is `GameUIScene::c_DefaultScreen`
(1920x1080, scale 1), so headless runs, `ui.layout` and `input.click` agree. The renderer draws with its viewport size; in
games and the editor's game view both are the same size, so one layout serves drawing and hit testing.

### 4) Text (`Renderer/FontFace.*`, `TextLayout.*`, `FontAtlas.*`, `TextRenderer.*`: owned by this stream)

- **Shaping without rasterizing.** Layout must measure text (fit content, wrapping) on the main thread without a GPU and
  without rasterizing distance fields. Split the glyph metrics (stb_truetype state, code point to glyph, advances, kerning,
  plane bounds, vertical metrics) out of `FontAtlas` into `FontFace` (new, created on first use by `Font::GetFace()`, main
  thread only). `FontAtlas` keeps rasterization and placement and uses the font's face. The validation rules for untrusted
  fonts (AGENTS.md "Rendering") apply unchanged: the face reads only what `FontValidation` covers.
- **Layout options.** `TextLayout` gains `ShapeText(const FontFace&, std::string_view, const TextShapeOptions&,
  ShapedText&)`: alignment, wrap width in em (word wrap at spaces and after hyphens, long words broken, `\n` kept),
  line spacing, a maximum height with `Truncate`/`Ellipsis` (U+2026, or "..." when the font lacks it), vertical alignment.
  `ShapedText` holds glyph indices and pen positions in em plus the block's bounds; `FontAtlas::Resolve` turns it into
  quads. The existing `LayoutText(FontAtlas&, ...)` becomes a wrapper with the same results (`TextLayoutTests` unchanged).
- **Cache.** `GameUIScene` shapes a `UIText` only when its text, font, size, wrap width, spacing or overflow changed, or its
  font's content changed (a font that loads replaces the default font it was shaped with).
- **Atlases shared with the text renderer.** `TextRenderer`'s per-font atlases become a `FontAtlasCache`
  (`Renderer/FontAtlasCache.h`, new) that the UI pass uses too: one atlas per font per `SceneRenderer`, one glyph raster
  budget per frame (`TextRenderer::c_FrameRasterBudget`, what the text renderer leaves). The cache drops an atlas only at
  the start of the next frame if no client used it in the previous one (today `TextRenderer::Prepare` trims at its end,
  which would drop atlases the UI uses after it).
- **Atlas placement.** `FontAtlas::GetPlacementVersion()` changes when a glyph is evicted; the UI rebuilds the quads of that
  font's texts then (no reshaping). The pass marks the distinct glyphs it shows as used each frame (`FontAtlas::MarkUsed`,
  cost per distinct glyph, not per quad), so the world text's churn never evicts glyphs on screen.
- **Guarantee:** `TextComponent` and `TextRenderer`'s pixels do not change (`GPUSceneTextTests`, `TextLayoutTests`,
  `FontAtlasTests`, `GPUFontAtlasTests` pass unmodified).

### 5) Rendering (`GameUI/GameUIRenderer.*`, shaders `Strata/shaders/GameUI/UIQuad.vert/.frag`)

- **Inversion.** The Renderer cannot include GameUI. `Renderer/SceneOverlay.h` (new): `ISceneOverlayPass` (`Prepare`,
  `Draw`, `HasScreenContent`) and `SceneOverlayRegistry` (factories registered by modules, here `RegisterGameUIModule`).
  `SceneRenderer` creates one pass instance per registered factory at construction. Its changes are this stream's "one
  call" and live in one place each:
  1. after `TextRenderer::Prepare`: `Prepare(SceneOverlayFrame)` of each pass (scene, viewport size, options, command list,
     the `FontAtlasCache` and raster budget, the frame's visible-texture list, stats);
  2. the `overlays` condition includes a pass that has something to draw;
  3. `RenderOverlays` calls `Draw` after the screen-space text (the UI goes over the legacy HUD);
  4. `SceneRendererStats::Overlays` (`SceneOverlayStats`: `Canvases`, `HiddenCanvases`, `Elements`, `Quads`, `DrawCalls`,
     `PendingGlyphs`, `PendingAssets`, `UploadedBytes`), with `PendingAssets` added to the frame's.
- **Editor views and captures.** Screen-space canvases follow `SceneRenderOptions::DrawScreenSpaceText` (documented as
  "the game's screen-space UI: the HUD text and screen-space canvases"): editor views hide them unless the viewport's Game
  UI setting is on (`GetViewportRenderOptions`), captures follow the viewport settings, thumbnails hide them. Hidden canvases
  are counted (`HiddenCanvases`) without being laid out. `viewport.capture` reports `ui {canvases, hiddenCanvases,
  elements, drawCalls, pendingGlyphs}` and waits for pending UI glyphs and textures like it waits for text.
- **Games.** `GameRenderer` draws through its `SceneRenderer`, so the UI appears with no change there except one: a scene
  without a primary camera whose passes have screen content (`SceneRenderer::HasOverlayContent(scene)`) renders its UI over
  black instead of the missing-camera message, so a main-menu scene needs no camera.
- **The pass.**
  - Quads in canvas units (`RenderData::UIQuad` in `GameUIRenderData.h`, at most 64 bytes: rect, UV rect, color, texture
    or atlas index and flags, corner radius, border width and color, a clip index for masks), built by `GameUIScene` in hierarchy order
    per root canvas (a nine-slice image is nine quads, a filled image one cut quad, a text one quad per glyph).
  - One storage buffer per `SceneRenderer`, uploaded only where quads changed since that renderer's last frame (a per-canvas
    journal of changed ranges; a full upload when it overflowed or another scene is drawn). An idle frame uploads 0 bytes.
  - Drawn instanced (six vertices per quad from `gl_VertexIndex`, no vertex buffer), canvases by `SortOrder` then hierarchy
    order, one draw per run of quads with the same bound font atlases. Up to `c_MaxBoundAtlases` (4) atlases are bound at
    once and images go through the bindless table, so a canvas with up to four fonts is one draw call.
  - Per-canvas push constants: scale, origin, viewport size and pixel snapping (rect edges and glyph baselines rounded to
    whole pixels in the vertex shader).
  - The fragment shader: textures sampled from the bindless table (honoring the grayscale bit, `c_TextureSlotGrayscale`)
    through a linear clamp sampler, converted to display values; solid colors; rounded corners and borders by a
    rectangle's signed distance; SDF glyphs with the coverage of `Overlay/Text.frag` (the shared function moves to
    `Include/SDFText.glsl`) and an optional outline. Blending in display space, like every overlay. List both shaders in
    `strata_add_shaders`.
- **Textures and residency.** The pass requests each distinct UI texture and font once per frame through
  `scene.GetAssetManager()` and its `GetAsset<T>` (never per element, never `AssetManager`'s static functions in new
  code), at High priority; a texture still loading draws as its fallback, a font as the default font. Its texture slots
  go into a small slot table (UI texture index to bindless slot), rewritten when a slot changes (mip streaming replaces
  slots), so quads never change for it. Each drawn texture is reported to the texture streamer through the frame's
  visible-texture list (`VisibleTextureUse`, with the pixels one UV unit spans on screen), so UI images stream to the
  level they are drawn at.
- **Frame stages.** `Render.UI` (`ST_PROFILE_STAGE`, the pass's CPU time, nested in `Render.Submit`) and `Scene.UI` (the
  system). `GPU.UI` is this stream's, added in its rebase onto gpu-driven-rendering (which merges before it): register
  the stage (`FrameStageKind::GPU`) in `RegisterGameUIModule`, give `SceneOverlayFrame` the frame's `GPUPassTimer`, and
  have the hook's `Draw` call switch it to `GPU.UI` and back to `GPU.Overlays` around the passes.
- **Cost without UI:** a scene without a `UICanvas` costs one storage-size check per frame; with Game UI hidden, a count of
  root canvases cached per hierarchy version.

### 6) Input (`Input/*`, `GameUI/GameUISystem.*`, `GameUIHitTest.*`)

- **The "UI" scene system** (`MakeSceneSystemDescriptor<GameUISystem>("UI", false, ...)`, `ProfileStage` `Scene.UI`,
  `Before: {"Scripting"}` on the base, `BeforeIfPresent: {"Scripting"}` after the rebase onto engine-modules): play mode
  only. Its `OnUpdate` runs before the scripts, so a click the UI takes is hidden from
  the scripts in the same frame and UI callbacks arrive before the scripts' `OnUpdate`. It provides the scene services
  `IUIControl` and `IUIEvents` (section 7). Times use the scene's unscaled delta, so menus work at time scale 0.
- **Hit testing** on the main thread against the layout of the input screen (the one the player saw): the topmost element in
  draw order (higher `SortOrder` canvases first, then reverse hierarchy order) whose rect contains the point (half-open) and
  that is a raycast target (`UIImage::RaycastTarget`, `UIText::RaycastTarget`, or a `UISelectable`), in a canvas with
  `ReceivesInput`. A uniform grid of raycast-target rects per canvas (rebuilt when its rects change) keeps a query to its
  cell's candidates. Hover is recomputed only when the pointer moved or the layout changed.
- **Capture.** `Input::SetPointerCapture(bool)` and `Input::IsPointerCaptured()`: while set, the game's
  `IsMouseButtonDown/Pressed/Released` and `GetScrollDelta` report nothing; `Input::GetPointerState()` gives the UI the
  unfiltered merged pointer. A press over a raycast target is the UI's until its release (also when the pointer leaves); a
  press elsewhere is the game's until its release (also when it moves over UI). Scroll over UI is captured. The system clears
  capture when play stops; `Input::Reset` clears it. Games without UI under the pointer see exactly today's input.
- **The fixed UI action set** (`Input/UIActions.h/.cpp`): `UIAction { Submit, Cancel, Up, Down, Left, Right }` with one
  binding table `c_UIActionMap`, written as data in the shape of wave 5's input-actions asset (`{"Action": "Submit",
  "Bindings": ["Key/Enter", "Key/KeypadEnter", "Key/Space", "Gamepad/South"]}`): Submit Enter, keypad Enter, Space, gamepad
  South; Cancel Escape, gamepad East; Navigate arrows, d-pad, left stick beyond 0.5 (released below 0.35), on any gamepad.
  `UIActionState::Update(unscaledDelta)` gives pressed, down and repeated (after 0.4 s, every 0.1 s), deterministic for given
  deltas. Rebinding is wave 5.
- **Focus and navigation.**
  - Navigate with a focused selectable: `Explicit` uses its neighbour for the direction (when active and interactable);
    `Automatic` picks, among the interactable selectables of input-receiving canvases with `Navigation` not `None`, the one
    ahead (projection on the direction above 1e-3 canvas units) with the lowest `along + 2 * |across|` from the focused
    rect's center (ties: draw order); nothing ahead keeps the focus.
  - Navigate or Submit with no focus focuses the topmost canvas's `FirstSelected`, else its top-left selectable.
  - Submit clicks the focused button or flips the focused toggle; Left/Right (Up/Down for vertical sliders) step the focused
    slider by `KeyboardStep` instead of navigating. Cancel raises no event: scripts read it (`UI::IsActionPressed`).
  - A click on a selectable focuses it. Disabled selectables (not `Interactable`) never hover, press, focus or raise events.
- **Widgets.** A button clicks on release over the same button it was pressed on. A toggle flips on click (exclusive within
  its `Group`). A slider follows the pointer while dragged and snaps with `WholeNumbers`. Input writes `UIToggle::IsOn` and
  `UISlider::Value` through `registry.patch` (the running scene, no undo), so `component.get` shows live values. Visual
  states: Disabled, else Pressed, Hover, Focused, Normal; tints fade over `FadeDuration` of unscaled time, and only fading
  elements rebuild quads (colors only).
- **Typed text.** `Input::ProcessText(uint32_t codepoint)` (called next to the `KeyTypedEvent` in `GLFWWindow.cpp`),
  `Input::SimulateText(std::string_view utf8)` (queued for the next input frame like other simulated input), and
  `Input::GetTypedText()`: the UTF-8 typed during the current input frame (device text only while input is enabled), at most
  `c_MaxTypedTextBytes` (1024) per frame.
- **Simulated input reaches UI exactly like device input** because the UI reads `Input`. The editor sets the input viewport
  every frame while the game view shows play (`ViewportPanel::UpdateGameInput` today sets it only while the view has the
  input), so hit testing always uses the size that is drawn.

### 7) Scripts (append block after world-partition's; common.md section 7)

- **Scene services** (`Scene/SceneServices.h`): `IUIControl` (text, image, color, interactable, value, focus,
  `IsPointerOverUI`, `IsActionPressed/IsActionDown` by action index; every function fails for entities without the needed
  component) and `IUIEvents` (`AddUIEventListener`/`RemoveUIEventListener`; `UIEventInfo { Type: Click, ValueChanged,
  FocusChanged; Widget; Target; Value; Focused }`). Scripting never includes GameUI.
- **Host functions** (`Host<Name>`, `HostCall` + `ResolveContext`, `X(Name)` in `ST_SCRIPT_HOST_FUNCTIONS`, misuse through
  `ScriptSystem::ReportProblem`): `UIGetText`, `UISetText`, `UIGetImage`, `UISetImage`, `UIGetColor`, `UISetColor`
  (the element's `UIImage` tint, else its `UIText` color), `UIIsInteractable`, `UISetInteractable`, `UIGetValue`,
  `UISetValue` (slider value, toggle 0/1; does not raise `OnUIValueChanged`), `UIGetFocus`, `UISetFocus` (0 clears),
  `UIIsPointerOverUI`, `UIIsActionPressed`, `UIIsActionDown`, `GetUnscaledDeltaTime`, `GetRealTime`, `GetTypedText`
  (text-buffer protocol). 18 functions. Unscaled time comes from `Scene::GetUnscaledDeltaTime()` and
  `Scene::GetUnscaledTime()` (new, set in `OnUpdateRuntime`: the frame's timestep before the time scale, the fixed step
  while stepping; the sum since play started).
- **Class callbacks** appended to `StrataScriptClassDesc` after `OnAnimationEvent`, read only when `StructSize` covers them:
  `OnUIClick(instance, StrataScriptEntityID widget)`, `OnUIValueChanged(instance, widget, float value)`,
  `OnUIFocusChanged(instance, widget, bool focused)`. `ScriptCallback` gains the three, and `ScriptSystem` registers an
  `IUIEvents` listener while running (like `IAnimationEventSource`): an event reaches every script on the widget's entity,
  then every script on its `EventTarget` (when set and different), once per instance, skipping removed, disabled and not
  yet created instances, in the order the inputs happened (pointer, then navigation, then submit).
- **SDK.**
  - `StrataScript/UI.h` (new, in `StrataScript.h`): `enum class UIAction`; `UIElement` (wraps an entity: `GetText`,
    `SetText`, `GetImage`, `SetImage`, `GetColor`, `SetColor`, `IsInteractable`, `SetInteractable`, `GetValue`,
    `SetValue`, `Focus`, `IsFocused`, `GetEntity`); `UI` (static: `GetFocused`, `ClearFocus`, `IsPointerOverUI`,
    `IsActionPressed`, `IsActionDown`). `Entity::GetUI()` returns a `UIElement`. The class `Strata::UI` exists only in
    script modules; no engine, editor or test translation unit includes the SDK's C++ headers (checked), so it never meets
    the editor's `Strata::UI` namespace.
  - `Script.h`: `OnUIClick(Entity widget)`, `OnUIValueChanged(Entity widget, float value)`,
    `OnUIFocusChanged(Entity widget, bool focused)` with their flags and thunks.
  - `Time.h`: `GetUnscaledDeltaTime()`, `GetRealTime()`. `Input.h`: `GetTypedText()` returning `std::string`.
  - `Gameplay.h`: `Motion::LookAt(from, to, up)` (the rotation that points -Z at the target, the engine's convention) and
    `Motion::SmoothDamp` (float, `glm::vec2`, `glm::vec3`; critically damped, with `maxSpeed`), static so the SDK reader
    counts them, tested inside the API module (`HelperScripts.cpp`), never in an engine translation unit.
  - Every wrapper uses `ST_SCRIPT_DETAIL_HOST_WITH(Name)` and degrades (empty, false, 0) on older engines.
- **API module and tests:** `StrataTests/Scripts/API/UIScripts.cpp` calls every host function with valid and invalid
  arguments; suite `Scripting.UI`. A module built against the SDK before this block (the `NewerSDK`/older-module pattern)
  loads and receives no UI callbacks.

### 8) Commands and agents

- **`Editor/EditorUICommands.cpp`** (new, `RegisterUICommands`):
  - `ui.create {type, parent?, name?, text?, anchor?, position?, size?}` (Edit): `type` one of `canvas`, `panel`, `image`,
    `text`, `button`, `toggle`, `slider`, `horizontalLayout`, `verticalLayout`, `gridLayout`. A parent outside any canvas
    (or none) gets a new top-level "Canvas" first. `anchor` names one of the 16 presets (`topLeft` ... `stretch`, shared
    with the Inspector, `Editor/UIRectEdit.h`), which also sets the pivot. Builds the preset hierarchy
    (`GameUIPresets::Create`: rounded panels, a button with a label, a toggle with box, checkmark and label, a slider with
    track, fill and handle, no texture assets needed) in one undo step. Returns `{entity, canvas, created, parts: {label,
    background, fill, handle, checkmark}, rect}` (rect in pixels at the game screen). Mutations of the running game warn.
  - `ui.layout {entity?, screen?: [w, h]}` (Query): the canvases (scale, unit size, sort order) and the elements of the
    entity's subtree (every root canvas without one): `rect` [x, y, w, h] in screen pixels, `visible`, `interactable`,
    `state` (normal, hover, pressed, focused, disabled), `focused`, `text`, `value`; at most 1,000 elements with `truncated`.
    `screen` defaults to the game's screen: while playing the UI's input screen (section 3); in edit mode the viewport
    image's size, which `EditorViewport` records each frame it draws; headless 1920x1080. Agents check proportions at
    several resolutions without rendering.
  - `ui.hitTest {position, screen?, all?}` (Query): the topmost raycast target at a pixel position (`entity`, `path` from its
    canvas, `interactable`), or every element there with `all`. Edit and play mode.
- **`input.click {entity, button?, frames?, wait?}` and `input.type {text, wait?}`** (Session), registered by
  `RegisterUIInputCommands` in `EditorInputCommands.cpp` (a function of its own, reusing that file's hold helpers). Play
  mode only. `input.click` hit-tests the center of the element's visible rect: another raycast target there fails the
  command naming it (the menu is covered); else it moves the simulated pointer there and taps the button through
  `SimulatedInput`, answers when the game has seen the release, and returns `{entity, position, clicked, events}` (the UI
  events of that click). `input.type` queues typed text (`Input::SimulateText`). `input.type` is the one command beyond the
  brief: without it no tool can test `GetTypedText` (or text fields).
- **Journal lists:** `ui.create` in `c_EditCommands`; `ui.layout`, `ui.hitTest` in `c_QueryCommands`; `input.click`,
  `input.type` in `c_SessionCommands` (`CommandJournalTests.cpp`).
- **Skills** in the same commits: `strata-make-a-game` (recipes: a main menu scene without a camera, a HUD canvas, a pause
  screen at time scale 0 with unscaled fades; replace the `Text`-HUD advice), `strata-editor-automation` (`ui.*`,
  `input.click`, `input.type`, `viewport.setSettings {canvasMode}`), `strata-scripting` (the UI API, callbacks, unscaled
  time, typed text, `Motion`), `strata-add-component` (components live in their module, `Requires`).

### 9) The editor (people)

- **Presets.** The Hierarchy's preset table (shared with authoring-and-playtest: a union) gets a UI submenu: Canvas, Panel,
  Image, Text, Button, Toggle, Slider, Horizontal Layout, Vertical Layout, Grid Layout. UI presets run `ui.create {type,
  parent}` instead of `entity.create` (a `Command` field on the preset). The old "Text" preset becomes "3D Text" (a
  world-space `TextComponent`), so "Text" means UI text.
- **Inspector.** A component drawer registry (`UI/ComponentDrawers.h/.cpp`, new): `InspectorPanel::DrawComponent` calls it
  once before the generic properties. The `UIRect` drawer (`UI/UIRectDrawer.cpp`):
  - an anchor-preset button opening a 4x4 grid (rows top, middle, bottom, stretch; columns left, center, right, stretch)
    drawn with theme tokens, with "Pivot" and "Position" toggles;
  - changing anchors keeps the rect where it is on screen (`Editor/UIRectEdit.*` converts Position and Size);
  - along a stretched axis it edits Left/Right (Top/Bottom) margins instead of Position/Size;
  - the rect in pixels at the game screen (read only, monospace).

  Every edit goes through `SetPropertyWithUndo` (multi-selection, one undo step). Drawers also show the notices of section
  2 (no `UIRect`, outside a canvas, nested canvas).
- **2D canvas mode.** `ViewportSettings::CanvasMode` (saved per project, `viewport.setSettings {canvasMode}`, a "2D" chip in
  edit mode). The viewport shows the selection's canvas (else the first root canvas) at its reference aspect ratio,
  letterboxed, through the scene's primary camera (else the editor camera) with the game UI forced on, so a
  `ScaleWithScreen` canvas lays out exactly as at its reference resolution.
  - It draws every element's outline and the selection's rect with eight handles and its pivot, through ImGui draw lists
    with theme tokens and font-relative sizes.
  - Dragging moves or resizes (`Editor/CanvasEdit.*`: `CanvasRectDrag`, one undo step per drag, snapping to whole canvas
    units while Snap is on, Ctrl inverts). A click selects the topmost element (an editor hit test over all elements);
    the wheel zooms about the pointer and middle drag pans, F and Home reset. The gizmo is hidden.
  - Code in `Panels/CanvasModeView.*` (new), called from `ViewportPanel` in one place. Probes record handles for harness
    tests.
- **Picking with Game UI shown** (normal viewport, edit mode): a click on a UI element selects it before the ID-buffer pick.
- **Icons** for the UI components and presets come from `UI/Icons.h` (missing Lucide icons are added with
  `StrataEditor/Tools/GenerateIconHeader.py`).
- No new docked panel; `c_LayoutVersion` is untouched.

### 10) The Tetris sample

The HUD moves onto a canvas (the brief's second option; `TextComponent` keeps its pixel semantics, and the skills steer
HUDs to canvases).
- `Main.stscene`: a `HUD` canvas (`ScaleWithScreen`, reference 1280x720, match height 1, `ReceivesInput` false): Score,
  Level and Lines keep their entity IDs and get `UIRect` + `UIText` in a vertical layout on a translucent rounded panel; the
  help text bottom-left; the message (same ID) on a centered panel that shows while the message is not empty (game over,
  paused). "NEXT" stays world-space text.
- `TetrisBoard.cpp` sets text with `GetUI().SetText` and toggles the message panel.
- `TetrisCommands.json.in`: `component.get` of the four HUD entities reads `UIText` instead of `Text`, and new steps
  `expect` the Score rect from `ui.layout` at screens 1280x720 and 2560x1440 to be the same fraction of the screen.
- The export, `StrataEditor.Tetris` and the person's view at 1280x720 and 2560x1440 keep the proportions of
  `tetris_1280x720.png`.

### 11) If time (in this order)

1. **Masks:** `UIMask` (clips descendants to its rect: the per-quad clip index, a clip-rect buffer, no batch breaks);
   hit testing respects it.
2. **Text fields:** `UIInputField` (`Text`, `Placeholder` entity, `MaxLength`, `CharacterLimit`, caret, selection by
   keyboard; typed text, Backspace, Delete, arrows, Home, End, Enter submits); keyboard capture while focused
   (`Input::SetKeyboardCapture`), `OnUISubmit` callback.
3. **World-space canvases:** `UICanvas::RenderMode` `WorldSpace` and `WorldUnitsPerUnit`: the canvas plane at the
   entity's world transform, depth-tested like world text, hit-tested by a ray from the camera.

Each lands with feature-scene coverage, tests and docs, or not at all (no placeholder enum values).

### 12) Docs

- `Docs/Architecture.md`: a new "Game UI" section (components, layout math, screens and scale modes, the UI contract,
  text shaping and atlases, the pass, input, capture and navigation, events, commands); updates to "Layers", "Composition
  root and registries" (`SceneOverlayRegistry`), "Frame profiler" (`Scene.UI`, `Render.UI`), "Scripting" (callbacks,
  unscaled time), "Editor" (presets, canvas mode, drawers), "Export and the runtime" (camera-less UI scenes).
- `AGENTS.md` (rules only): the UI contract (signal writes to UI components; Debug validates the layout); UI components
  live in GameUI; the renderer reaches UI only through `SceneOverlayRegistry`; scripts only through `IUIControl` and
  `IUIEvents`; the pass requests each distinct UI asset once per frame through the scene; text is measured through
  `FontFace`, never by rasterizing; UI input reads `Input`, so simulated input reaches it.
- `Docs/Manual/GameUI.md` (new, for people): canvases and scaling, anchors and layout groups, images (nine-slice, filled,
  rounded), text, widgets, navigation with keyboard and gamepad, scripting UI events, a pause menu recipe, the canvas mode.

### Guarantees to other streams in the meantime

- Scenes without a `UICanvas` render, cost and behave exactly as before: the overlay registry is empty until
  `RegisterGameUIModule` runs, and a pass without canvases returns at one storage-size check.
- `TextComponent` and `TextRenderer` output are unchanged (section 4).
- Input without UI under the pointer is unchanged; typed text is additive.
- The ABI changes are append-only; older modules load unchanged.
- `Scene` gains only unscaled-time accessors.

## Acceptance criteria

Tests (each suite named after the module; "must fail" ones are confirmed on the base or with the fix reverted, and named in
the report):
- **`GameUI.Layout`:** anchors, pivots, positions and sizes exact (1e-4 canvas units) for canvas sizes 1920x1080,
  1280x720, 2560x1440 and 1080x1920; stretched anchors; nested rects; negative sizes clamp to 0; vertical, horizontal and
  grid groups with padding, spacing, all nine alignments, `StretchChildren`, `FitContent` and `IgnoreLayout`, inactive
  children skipped; slider fill and handle; toggle checkmark; `SafeAreaPadding`.
- **`GameUI.Scale`:** the three scale modes at 16:9, 21:9, 4:3 and portrait screens, `MatchWidthOrHeight` 0, 0.5 and 1,
  content scales 1, 1.5 and 2: scale and unit size to 1e-5.
- **`GameUI.Dirty`** (deterministic counters):
  - an idle frame lays out 0 elements, shapes 0 texts and builds 0 quads;
  - in the 10,000-element perf scene, one label's text change shapes 1 text and lays out the label alone (1 element); when
    the label and its button fit their content, it lays out its panel's subtree (25 elements) and nothing else;
  - moving a leaf under a plain parent lays out 1 element;
  - a reparent lays out the moved subtree and both groups;
  - an activity change; a same-aspect resize under `ScaleWithScreen` lays out 0;
  - a write without a signal makes `Validate` fail naming the entity;
  - an overflowed journal falls back to one full layout.
- **`GameUI.Text`:** shaping equals today's `LayoutText` advances; word wrap, long words, `\n`, the alignments, `Truncate`,
  `Ellipsis`, `LineSpacing`, fitted sizes; shaping rasterizes nothing (`FontAtlasStats::RasterizedGlyphs` unchanged);
  `TextLayoutTests`, `FontAtlasTests` and `GPUSceneTextTests` pass unmodified.
- **`GameUI.HitTest`:** topmost by sort order and draw order; non-raycast elements pass through; `ReceivesInput` false and
  inactive elements ignored; rect edges half-open; at most 64 candidates per query among 10,000 elements.
- **Input capture** (in `GameUI.HitTest` and the Input tests): a press over UI hides `IsMouseButtonPressed/Down/Released`
  and scroll from the game until its release; a press elsewhere stays the game's when the pointer moves over UI;
  `IsPointerOverUI`.
- **`GameUI.Navigation`:** expected focus sequences on a 3x3 grid and an irregular menu; explicit neighbours, `None`,
  disabled skipped; start at `FirstSelected` or top-left; key repeat at 0.4 s then every 0.1 s with fixed deltas;
  d-pad and stick with hysteresis; slider steps; the same input sequence gives the same focus sequence in every run.
- **`GameUI.Events`:** one `OnUIClick` per click per script instance (widget first, then `EventTarget`; once when they are
  the same); press, drag out, release clicks nothing; value changes once per change; toggle groups exclusive; disabled
  widgets raise nothing; events arrive before the scripts' `OnUpdate` of that frame.
- **Input tests** (`InputTests.cpp`): typed text per frame, UTF-8 multi-byte, simulated text, the 1,024-byte cap, text
  ignored while input is disabled; `UIActionState` bindings and repeat.
- **`Scripting.UI`:** every new host function with valid and invalid arguments; the callbacks' `StructSize` gating;
  `GetUnscaledDeltaTime` non-zero at time scale 0 and `GetRealTime` increasing; `Motion` helpers checked inside the module.
- **`GameUI.Serialization`:** UI components round-trip; entity references remap through `entity.paste`, prefabs and
  `Scene::Copy`; a build without GameUI keeps them as unknown components.
- **`GPU.GameUI`** (validation layer, 0 errors):
  - the same canvas at 1280x720 and 2560x1440 places a panel at the same fraction of the screen (edges within 1 pixel of
    the expected positions);
  - nine-slice corners keep their size while edges and center stretch; a `Filled` image at 0.25; rounded corners and
    border; SDF text with an outline in exact display colors;
  - a 512x512 UI texture drawn at 512 pixels streams to level 0; drawn at 64 pixels it stays at a coarser level;
  - an idle frame uploads 0 bytes; one draw call per canvas with up to four fonts;
  - hidden with `DrawScreenSpaceText` off; a camera-less canvas scene through `GameRenderer` shows its UI and no message;
  - **must fail on the base:** Tetris' HUD (a copy of the sample's `Main.stscene`, rendered at both sizes) covers the same
    fraction of the screen within 1% (on the base the 1440p HUD covers half the fraction).
- **Feature test:**
  - `Feature.stscene` holds a canvas with every UI component and every property non-default somewhere;
  - a feature script (`Scripts/UIFeatures.cpp`) navigates with simulated keys, clicks a button with the simulated
    pointer, changes and reads a slider, toggles, focuses, reads typed text the runner simulates, uses unscaled time at
    time scale 0 and `Motion`, and journals `OnUIClick`, `OnUIValueChanged` and `OnUIFocusChanged`;
  - `InputFeatures` expects no game press in the frame the UI captures and `UI::IsPointerOverUI()` true then (the runner's
    UI phase follows `InputFeatures`' press frames);
  - after every frame of `PlayFeatureScene`, `GameUIScene::Validate` passes next to the scene's cache validators;
  - all runners pass (`StrataTests.FeatureTest` three ways, `StrataEditor.FeatureTest` with new `ui.layout`, `ui.hitTest`
    and `input.click` steps in `FeatureCommands.json.in`, `StrataRuntime.FeatureTest`);
  - `GPU.FeatureTest` sees UI pixels (the feature panel's color at its position) and `Overlays.Canvases >= 1`.
- **Editor:**
  - `Editor.UICommands`: `ui.create` of every type in one undo step (undo removes all, redo restores the IDs), parent and
    anchor rules, invalid input changes nothing; `ui.layout` rects at given screens; `ui.hitTest`; `input.click` clicks
    (`clicked: true`), fails when covered (naming the cover) and outside play; `input.type`; `CommandJournalTests`.
  - UI-harness tests: `Editor.UI.Hierarchy` (the UI submenu creates a button under a new canvas, one undo step),
    `Editor.UI.Inspector` (the anchor preset keeps the rect in place; margins along stretched axes), `Editor.UI.CanvasMode`
    (handles shown; dragging a handle resizes by the dragged canvas units in one undo step; a click selects the topmost
    element; at UI scales 1 and 1.5), `Editor.UI.Source` with its empty allowlist.
- **Samples and scripts:** `StrataEditor.Tetris` passes with the migrated HUD and its new `ui.layout` steps; Smoke, Export,
  Scripts and Install pass.
- **`Architecture.Layering`** passes with the GameUI layer, no new allowlist line.

Perf (Release, confirmed checks, `Budgets.json` entries set from locked A/Bs, "to confirm in the integration campaign"):
- `Perf.GameUI.Layout10k`: a full relayout of 10,000 elements: one canvas of 400 vertical-layout panels with 12 buttons each
  (button image and label; 8-12 character labels), the screen alternating between 1920x1080 and 1280x1024 so every
  element is laid out and no text is reshaped. Median of 30 relayouts (a mean would let one page fault decide). Expected
  1-2 ms; budget 1.5 times the measured value.
- `Perf.GameUI.IdleFrame10k`: the same scene playing with nothing changing and the pointer still: the UI system's update and
  `EnsureLayout` without rendering, timed in batches of frames (the clock's resolution). Median of 300 frames; about 0
  (expected below 0.01 ms); budget 1.5 times the measured value. The counters test (`GameUI.Dirty`) proves it does no
  work.
- `PerfGPU.GameUI.Render10k`: the UI pass's CPU time (`Prepare` and `Draw` recording) at 1920x1080 while 100 labels change
  text and 100 buttons change color each frame. Median of 300 frames. Acceptance limit 1 ms, so the budget is the lower of
  1 ms and 1.5 times the measured value.
- `PerfGPU.GameUI.RenderIdle10k`: the same with nothing changing (what an idle game view pays). Median of 300 frames;
  expected below 0.05 ms; budget 1.5 times the measured value.
- No regression, by interleaved A/B against the base (3 or more pairs, ratio of medians):
  - `PerfGPU.Editor.{Edit,Play}FrameNested{100k,1M}` idle frames, also measured with `editor.perf` stages before and after;
  - `PerfGPU.Renderer.*`;
  - the exported stress game's frames in `PerfGPU.Streaming`'s runtime runs (the runtime's first frame).

Screenshots (Release, `--frames N --screenshot`, private session, recent-projects and layout files; put them under
`scratchpad\wave4\game-ui\shots`, look at them and fix what looks unfinished):
- Tetris playing with its HUD, and its pause screen, in the editor's game view at 150%, at `--ui-scale 1` and in a
  1600x900 window.
- Exported Tetris (`StrataRuntime --resolution 1280x720` and `2560x1440`), HUD and pause screen: the same proportions.
- A basic3d project built by a command script with `ui.create` only (a main menu with a title, Play/Settings/Quit buttons
  in a vertical layout, a slider and a toggle; a HUD with a health bar), in the game view and exported at both
  resolutions.
- The 2D canvas mode with a selected button and its handles at 150% and at `--ui-scale 1`; the Inspector's `UIRect`
  drawer with the anchor grid open.

Docs: the "Game UI" section and the other Architecture.md updates, the AGENTS.md rules, `Docs/Manual/GameUI.md` and the
four skills, each in the commit of the behavior it describes.

## Deliver in this order if time runs short

1. **The module and what draws (critical: architecture C).** The layer and registration; the components with their
   feature-scene entries and `Requires`; layout with the UI contract, validation and counters; the `FontFace` split,
   shaping and the shared atlas cache; the overlay registry and `GameUIRenderer` in `SceneRenderer`, editor views,
   captures and `GameRenderer`; Tetris on a canvas. Perf `Layout10k` and `Render10k`/`RenderIdle10k`.
2. **Input:** hit testing, capture, the UI action set, focus and navigation, widget behavior, typed text in `Input`. Perf
   `IdleFrame10k`.
3. **Scripts:** the services, the host block, the callbacks, `UI.h`, unscaled time, typed text, `Motion`; the API module
   and the feature scripts.
4. **Commands:** `ui.create`, `ui.layout`, `ui.hitTest`, `input.click`, `input.type`; skills. (Agents can author UI from
   here even if item 5 slips.)
5. **Editor:** presets, the Inspector drawer and notices, the 2D canvas mode, picking with Game UI shown.
6. **If time:** masks, text fields, world-space canvases.

Docs, skills, tests and feature coverage go with each item (golden rules 2 and 6): a component or API never lands without
its feature-scene or feature-script coverage.

## Open items this stream owns

- Architecture C "No in-game UI system"; Person M "No in-game UI beyond text"; Agent M F7 "Game UI is Text only"
  (`ui.hitTest`, `input.click`); Agent m F14 (unscaled time, `LookAt`, `SmoothDamp`); the UI part of Architecture M
  "Input is raw devices only" (the fixed UI action set, typed text).
- Out of scope (wave 5 or later): input actions and rebinding (the action set is shaped as their first map); a
  settings-screen framework; localization and fallback fonts for CJK text in games; rich text; scroll views, dropdowns and
  lists; UI animation beyond transition tints; and whichever of masks, text fields and world-space canvases do not land
  (report them).

## Files expected to be touched

- New: `Strata/src/Strata/GameUI/*` (section 1); `Strata/src/Strata/Renderer/SceneOverlay.h/.cpp`, `FontFace.h/.cpp`,
  `FontAtlasCache.h/.cpp`; `Strata/src/Strata/Input/UIActions.h/.cpp`; `Strata/shaders/GameUI/UIQuad.vert`,
  `UIQuad.frag`, `Strata/shaders/Include/SDFText.glsl`.
- Engine: `Engine/BuiltinModules.cpp` (one line); `Renderer/SceneRenderer.h/.cpp` (overlay hooks, stats);
  `Renderer/TextRenderer.*`, `TextLayout.*`, `FontAtlas.*`, `Font.*`; `Strata/shaders/Overlay/Text.frag` (shared include);
  `Strata/CMakeLists.txt` (`strata_add_shaders` entries); `Runtime/GameRenderer.cpp` (camera-less UI);
  `Runtime/GameWindowService.cpp` and `Core/EngineServices.h` (`ContentScale`); `Input/Input.h/.cpp` (capture, typed text);
  `Platform/GLFW/GLFWWindow.cpp` (`ProcessText`); `Scene/SceneServices.h` (`IUIControl`, `IUIEvents`); `Scene/Scene.h/.cpp`
  (unscaled time); `Scene/ComponentAccess.cpp` and `Reflection/ComponentRegistry.h` (`Requires`);
  `Scripting/ScriptHostAPI.cpp`, `ScriptSystem.*`, `ScriptModule.*`, `ScriptTypes.*`.
- SDK: `StrataScriptCore/Include/StrataScript/ScriptABI.h`, `UI.h` (new), `Script.h`, `Entity.h`, `Time.h`, `Input.h`,
  `Gameplay.h`, `StrataScript.h`.
- Editor: `Editor/EditorUICommands.cpp` (new), `Editor/UIRectEdit.*`, `Editor/CanvasEdit.*` (new),
  `Editor/EditorInputCommands.cpp` (`RegisterUIInputCommands`), `Editor/EditorCommands.cpp` (registration),
  `Editor/EditorViewport.*` (`CanvasMode`), `Editor/ViewportRenderer.cpp`, `Editor/EditorViewportCommands.cpp`
  (`canvasMode`, capture `ui`), `Editor/EditorGameWindow.cpp` (`ContentScale`), `Panels/CanvasModeView.*` (new),
  `Panels/ViewportPanel.cpp` (one call, input viewport while playing, UI picking), `Panels/SceneHierarchyPanel.cpp`
  (presets), `Panels/InspectorPanel.cpp` (one drawer call), `UI/ComponentDrawers.*`, `UI/UIRectDrawer.cpp` (new),
  `UI/EntityIcons.cpp`, `UI/Icons.h` if icons are added.
- Tests: `StrataTests/src/GameUI/*` (new), `src/Renderer/GPUGameUITests.cpp` (new), `src/Core/InputTests.cpp`,
  `src/Scripting/ScriptUITests.cpp` (new), `src/Editor/EditorUICommandTests.cpp`, `EditorUICanvasModeTests.cpp` (new),
  `CommandJournalTests.cpp`, `src/FeatureTest/*` (runner UI phase, GPU checks), `src/Perf/GameUIPerfTests.cpp` (new),
  `src/Perf/SceneGenerators.h` (`CreateUIWidgets`), `StrataTests/Perf/Budgets.json`, `StrataTests/Scripts/API/UIScripts.cpp`,
  `HelperScripts.cpp`, `StrataTests/FeatureTest/Assets/Scenes/Feature.stscene`, `FeatureTest/Scripts/UIFeatures.cpp`,
  `InputFeatures.cpp`, `StrataTests/Editor/FeatureCommands.json.in`, `TetrisCommands.json.in`,
  `StrataTests/Architecture/Layers.json`, `StrataTests/CMakeLists.txt` (sources inside its blocks).
- Sample: `Samples/Tetris/Assets/Scenes/Main.stscene`, `Samples/Tetris/Scripts/TetrisBoard.cpp`.
- Docs and skills: `Docs/Architecture.md`, `AGENTS.md`, `Docs/Manual/GameUI.md` (new), `.claude/skills/strata-make-a-game`,
  `strata-editor-automation`, `strata-scripting`, `strata-add-component`.

## Merge notes and conflict risks

Merges **fifth**: engine-modules, world-io, world-partition, gpu-driven-rendering, then game-ui, then
authoring-and-playtest.

- **engine-modules (first).**
  - The GameUI layer becomes a library through `Layers.json` (append `GameUI` to `Engine`'s `MayInclude`).
    `RegisterGameUIModule` is one row at the end of its module table, after Scripting and world-partition's World row.
  - The system's `Before: {"Scripting"}` becomes `BeforeIfPresent` in this stream's rebase.
  - The `GameUI.*` suites fall into a Core shard by prefix (its shard-coverage test fails otherwise).
  - `Requires` lands next to wherever it moves the component registration helpers.
  - Rebase includes of moved headers (`TextComponent`/`TextAlignment`, used by `TextLayout.h`). The `ComponentRegistration`
    move does not touch GameUI's components, which live in their module from day 0.
- **world-io.** UI components serialize through reflection, so its streamed and binary scene formats carry them (entity
  and vec4 properties included); nothing else shared.
- **world-partition (third).**
  - Its host block and any callbacks come first. This stream re-appends its 18 functions and 3 callbacks after them, and
    the integrator checks the `static_assert`, the API module and feature coverage.
  - In an edited world, new top-level entities go into the persistent scene unless an edit layer is picked, so
    `ui.create`'s new canvases never stream out with a cell. `Feature.stscene` becomes the feature world's persistent
    scene: this stream's canvas lives there.
  - `Feature.stscene` and `FeatureTest/Assets` are a union (its tiny world, this canvas).
  - `Scene.cpp`: the unscaled-time lines in `OnUpdateRuntime` are function-level.
- **gpu-driven-rendering (fourth, rewrites `SceneRenderer`).**
  - Re-apply the four hooks of section 5 on the final `SceneRenderer`: pass construction, `Prepare` after the text
    renderer, the overlay condition and `Draw` after the screen-space text, the stats. The hook is this stream's alone:
    gpu-driven-rendering defines none (its spec says so).
  - The visible-texture list stays the way UI images reach the texture streamer: gpu-driven-rendering's spec guarantees
    it also once visibility feedback drives the scene's textures (UI quads are not proxies).
  - This stream adds the `GPU.UI` stage in its rebase (section 5, "Frame stages"); gpu-driven-rendering wraps only the
    text pass (`TextRenderer` is this stream's) and subtracts the camera from world-space text vertices at one call
    site.
  - New shaders and `Include/SDFText.glsl` go into its shader tree. The bindless table layout is read, not changed.
  - `Renderer/SceneOverlay.h` and `FontFace`/`FontAtlasCache` are new files in its folder.
  - `GameRenderer`: one branch for camera-less UI scenes.
- **authoring-and-playtest (sixth, wires this stream's work).**
  - The Hierarchy's preset table is a union: its primitives with colliders and this stream's UI submenu; "Text" becomes
    "3D Text".
  - `InspectorPanel::DrawComponent`'s drawer call and `UI/UIRectDrawer.cpp`.
  - Its `InactiveReason` hooks should cover the UI properties that other settings leave without effect (`SliceBorders`,
    `SliceScale` unless Sliced; `FillAmount`, `FillDirection` unless Filled; `CellSize`, `Columns` unless Grid;
    `ReferenceResolution`, `MatchWidthOrHeight` unless ScaleWithScreen; the selectable's images unless ImageSwap).
  - Its Game view:
    - sets the input viewport in game pixels at its resolution, so UI hit testing matches (this stream sets it every play
      frame in `ViewportPanel::UpdateGameInput`; the Game view takes that over);
    - renders camera-less canvas scenes like `GameRenderer`;
    - keeps presence chips off `GameUIScene::GetVisibleRects(screen)`.
  - `input.sequence` clicks UI entities through `input.click`'s target computation and `ui.hitTest`.
  - Its input timing in fixed steps covers `input.click` too: in its rebase `input.click {frames}` becomes `steps`
    (`frames` an alias) like the other input commands.
  - Its `MayDefer` flag goes on `input.click` and `input.type` (they wait for the game to see their input unless
    `wait: false`); list them in the report's merge notes.
  - Its MCP toolsets include `ui.create`, `ui.layout`, `ui.hitTest`, `input.click` and `input.type`, and the
    `CommandJournalTests` lists are a union.
  - `script.status` (its fix in `ScriptEdit.cpp`) lists the three UI callbacks.
  - `TetrisCommands.json.in` is a union: this stream changes `Text` to `UIText` and adds `ui.layout` steps; it changes
    `play.step` and input timing.
- **Unions with everyone:** `Budgets.json`, `Layers.json`, `BuiltinModules.cpp`, the skills, `AGENTS.md` and
  `Docs/Architecture.md` sections, `StrataTests/CMakeLists.txt`.
- **Conflict risks.**
  - `SceneRenderer.cpp` (gpu-driven-rendering's rewrite) is the largest. Keep each hook a single call into
    `SceneOverlay.h`.
  - `TextRenderer`/`FontAtlas` refactors could change text pixels: the unmodified text tests are the gate.
  - `ViewportPanel.cpp` is shared with three streams: canvas mode stays in `CanvasModeView`.
  - Idle editor frames: measure with `editor.perf` before and after; a regression is fixed, never budgeted.

## Answers from the planner (cross-check of 2026-10-13)

1. **The UI system runs before Scripting** (`Before`, `BeforeIfPresent` after engine-modules): accepted. `plan.md`,
   common.md and engine-modules' spec now say so.
2. **Rects cached in canvas units, reported in pixels:** accepted.
3. **`input.type`:** accepted. It maps to the MCP `play` toolset by its `input.` prefix; authoring-and-playtest's kind
   lists and toolset table include it.
4. **Three callbacks and 18 host functions** (124 -> 142 after world-partition's five): accepted.
5. **`UISelectable`:** accepted, with every property non-default somewhere in `Feature.stscene`.
6. **Only Engine includes GameUI:** accepted (append `GameUI` to `Engine`'s `MayInclude`; Runtime does not need it).
7. **Small changes in other streams' files:** accepted as function-level unions, now listed in common.md section 12:
   `Requires` in `ComponentRegistry.h` beside engine-modules' hooks, world-io's `ExtraBinary` and authoring-and-playtest's
   `InactiveReason`; `GameWindowState::ContentScale` beside authoring-and-playtest's Game view resolution in
   `EditorGameWindow`; the input viewport set every play frame in `ViewportPanel::UpdateGameInput`, which
   authoring-and-playtest's Game view takes over; `SceneOverlay.h`, `FontFace` and `FontAtlasCache` as this stream's
   files in the Renderer folder.
8. **"3D Text":** confirmed; authoring-and-playtest's spec uses the same labels in the preset table.
9. **The SDK class `UI`:** keep it as the brief asked. No engine, editor or test translation unit includes the SDK's C++
   headers; keep it that way (the API module and feature scripts are separate modules).
10. **Tetris on a canvas, `TextComponent` unchanged:** accepted. `TextComponent` gains no `ReferenceHeight` this wave.
11. **Additions from the cross-check:** the overlay hook is this stream's alone (gpu-driven-rendering defines none), and
    `GPU.UI` is added by this stream in its rebase; the composition-root row goes last; `input.click`'s `frames`
    becomes `steps` in authoring-and-playtest's rebase.
