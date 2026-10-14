# game-ui spec: state notes (successor continues from here)

## Progress
- [x] Read common.md, plan.md
- [x] Read wave3 animation.md format
- [x] Read audits/evidence (architecture, person, agent shots; FoxCoins)
- [x] Read code: TextComponent, TextRenderer, TextLayout, Font, Input, SceneServices, ScriptABI, SDK, ScriptHostAPI, SceneRenderer overlays, GameRenderer, ViewportPanel, SceneHierarchyPanel presets, EditorInputCommands, CommandJournalTests, Layers.json, BuiltinModules, Feature test, Tetris
- [x] Write spec game-ui.md (written 2026-10-13; edits applied: quad<=64B, dirty counts, ui.layout edit-mode screen, feature per-frame Validate)

## Notes (facts found)
- Evidence read: tetris 720 vs 1440 shots (HUD 64px offset at 720, ~128 at 1440 but font ~1.3x: content-scale?), agent 07_paused/12_final_win (text over level, no panel), person 08 tetris play. FoxCoins GameManager.cpp: SetProperty("Text","Text"), SetActive on PauseText, Time::SetTimeScale(0) for pause.
- TextRenderer::Prepare lays out EVERY text EVERY frame (LayoutText per TextComponent), uses AssetManager::GetAsset (global); atlases per TextRenderer in unordered_map<Font*,CachedAtlas>, orphan trim at end of Prepare (bug risk if a 2nd client touches atlas after) -> design FontAtlasCache with trim at next frame begin.
- FontAtlas: BeginFrame protects glyphs of current frame; LRU eviction only when full (8 pages x 144 cells). Need placement version for cached UI quads.
- SceneRenderer::RenderFrame: TextRenderer::Prepare before tonemap; overlays = HasOverlays||text -> output texture + copy into target; RenderOverlays draws screen text last (line ~2248). Renderer can't include GameUI -> need Renderer-layer overlay registry (ISceneOverlayPass) filled by RegisterGameUIModule.
- GameRenderer: no primary camera -> message scene. A menu-only scene (canvas, no camera) must render UI over clear color -> the GameRenderer call.
- SceneRenderOptions::DrawScreenSpaceText = !EditorView || ShowGameUI (ViewportRenderer.cpp GetViewportRenderOptions); ThumbnailCache sets false.
- Input: Input::GetViewportSize (input viewport = game view / window), SimulateMouseMove/Button; GLFWWindow.cpp:217 KeyTypedEvent (add Input::ProcessText); no capture concept.
- Scene: GetHierarchyVersion/GetHierarchyMoves (c_MaxHierarchyMoves 4096), GetWorldTransformChanges (activity too). RenderScene pattern: registry on_construct/update/destroy + journals.
- Scene::OnUpdateRuntime: deltaTime = stepping ? fixed : timestep*m_TimeScale -> add m_UnscaledDeltaTime/m_UnscaledTime.
- ScriptSystem listens IAnimationEventSource (ScriptSystem.cpp:97) -> same pattern IUIEvents. ScriptModule InvokeAnimationEventCallback; SDK Script.h flags CallbackFlag_OnAnimationEvent = 1<<10.
- Host API last = PhysicsGetLayerIndex (119). Time host fns: GetDeltaTime etc via ScriptSystem.
- Hierarchy presets: SceneHierarchyPanel.cpp GetPresets() {Icon, Label, Name, Components json} -> entity.create. UI presets need hierarchy -> run ui.create.
- Bindless: Texture::GetBindlessSlot changes with mip streaming; UI textures must be requested per frame and reported to TextureStreamer (else stuck at 32px tail).
- Tetris HUD: Text entities (FontSize 40, ScreenAnchor 0.05/0.1); TetrisBoard.cpp SetText via entity.SetProperty("Text","Text"); TetrisCommands.json.in uses component.get Text on ids 893AE784EA121440 (Score), 25892A97AD67432E (Level), 479A9674867A5955 (Lines), 3AB0E777DCA23EC4 (Message). Decision: HUD moves onto a canvas (UIText on same entity ids), commands switch "Text"->"UIText".
- Layers.json: layers list bottom-up; Engine MayInclude explicit. GameUI after Renderer, before Runtime. Runtime need NOT include GameUI if overlay registry answers HasContent.
- BuiltinModules: RegisterGameUIModule after RegisterAnimationModule (system Before Scripting needs Scripting registered).
- No component "Requires" in ComponentRegistry -> propose Requires("UIRect") honored in ComponentAccess add path.
- Inspector has no custom drawers -> add a component drawer registry (UI/ComponentDrawers.*), one call in InspectorPanel::DrawComponent.
- ViewportPanel::UpdateGameInput sets Input::SetViewport only when enabled -> must set every frame while playing so UI layout/hit test == drawn.
- IGameWindow GameWindowState has Width/Height/Mode/VSync, no content scale (Window::GetContentScale exists) -> add ContentScale.
- CommandJournalTests lists: Edit(47) Query(37) Session(31). ui.create=Edit, ui.layout/ui.hitTest=Query, input.click/input.type=Session.
- GPU.FeatureTest checks stats.Texts==2; add UI checks.
- Test dirs: src/Renderer (TextLayoutTests, FontAtlasTests, GPUSceneTextTests, GPUSceneOverlayTests), src/Core/InputTests.cpp, Scripts/API/*Scripts.cpp.
- Design decisions so far: GameUIScene (registry ctx, any mode: layout+render data, per canvas-unit-size layout slots<=4), GameUISystem (play only, Before Scripting: hit test/focus/nav/events, IUIControl+IUIEvents), quads in canvas units + per-canvas scale/offset in shader; FontFace split from FontAtlas (shaping w/o raster); FontAtlasCache shared with TextRenderer (trim at next frame begin); Renderer/SceneOverlay.h registry ISceneOverlayPass; UISelectable component; FitContent on UIRect; slider/toggle visuals by layout/render override (no registry writes).
- NEXT: write spec game-ui.md (sections: Why, Scope 1-8, Acceptance, Deliver order, Open items, Files, Merge notes).
- NEXT: final skim, then StructuredOutput (summary, risks, questionsForPlanner).
