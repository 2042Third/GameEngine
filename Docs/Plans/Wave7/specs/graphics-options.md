# Workstream `graphics-options`: graphics options for people, agents and players (wave 7)

Owner's request (2026-10-26, verbatim in `audit_brief.md`): "the current image quality is simply not great. The shadows
are very pixelated, maybe add more settings to improve that", no TAA upscaling ever, "great realistic graphics, which
doesn't mean sharp or clinical", the Fox Engine (MGS V) as the baseline look. What and when are ours to decide; only a
good end result counts. The plan (`plan.md`, section 1) turns "more settings" into deliverable 4: "Quality people,
agents and players can choose". Step 0 (`plan.md` S0.4) makes quality *work* (`RendererQualitySettings`, the
`Renderer.Quality` section, `SceneRenderer::SetQuality`, every program following it); this stream makes it a
*product*: a person finds it where they look, an agent sets it with one command, a player changes it in the game's
options menu, and the choice is kept for the next run.

This stream lands **first** of the four (`plan.md` section 4): it is the smallest, changes no image, and the other
streams' quality rows appear on its page and in its player options without UI or ABI work.

AGENTS.md is the source of truth for rules. The machine's rules for this wave (plan section 4.1, restated):

- The stream works in `.worktrees/graphics-options` on branch `agent/graphics-options` from step 0's tip, builds in
  `build/agent-graphics-options` (`cmake --preset windows -B build/agent-graphics-options`), never in another agent's
  directory.
- Every commit: Debug ctest and every `GPU*` suite under the validation layer on NVIDIA (`ctest --preset windows-debug`
  with this build directory). Before landing (plan 7.1): Release ctest without perf, `python Tools/CompatLint.py
  --changed-since <step 0 tip>` clean (its compiler paths default to the Mac's, `Tools/CompatLint.py:108-112`: pass
  `--build`, `--gcc` and `--clang` for this machine, or say in the landing message that CI's GCC and Clang jobs stand in),
  and CI green on `ci/wave7-graphics-options`.
- No perf campaign and no `PerfStability.py` in this stream. A/Bs of this stream's own effect are information, run
  under `python Tools/PerfLock.py --label graphics-options -- <command>` when no other build runs (section 8).
- Everything below this line that says "file:line" is as of bc6ee123 unless it says "step 0". Facts that could not be
  checked are marked *unverified*.

Paths: `R/` is `Strata/src/Strata/Renderer/`, `RT/` is `Strata/src/Strata/Runtime/`, `SC/` is
`Strata/src/Strata/Scripting/`, `SDK/` is `StrataScriptCore/Include/StrataScript/`, `ED/` is `StrataEditor/src/`,
`DG` is `Docs/DeveloperGuide.md`.

## 1. Goal, in priority order

1. **People find quality where they look for it.** Project Settings gets a *Graphics Quality* page: a preset picker
   (Low, Medium, High, Ultra, Custom) and one table, a row per knob and a column per preset, the chosen column
   highlighted, Custom's column editable, each row with its memory cost at 1920x1080 and 3840x2160 and the GPU time its
   stage takes now; every edit is one `project.setSettings` step (undoable). The Scene view gets a quality chip for its
   own `quality` setting. The sun's Inspector shows "Shadow maps: 4 x 4096 (Graphics Quality: High)" and links to the
   page. Post Process and the Directional Light are grouped under headings, and every property of both has a
   description with its unit.
2. **Players choose quality in the game's options menu, kept for the next run.** An engine service `IGameGraphics`
   (Core, beside `IGameWindow`) exposes the `Renderer.Quality` fields as named options with their effective value and
   source (Project, Player, CommandLine); `GameRuntime` and the editor's play mode provide it; the player's choices live
   in an engine-owned `GraphicsOptions.json` in the game's save directory; `StrataRuntime --quality` wins for the session
   without writing.
3. **Scripts reach it through a permanent, extensible ABI.** Seven host functions appended after
   `InputCancelBindingCapture` (ABI version stays 1), an info struct with `StructSize`, the SDK's
   `StrataScript/Graphics.h`. Options are named, so rows the other streams add reach scripts without ABI changes.
4. **Agents reach all of it through commands.** `project.setSettings` (exists) sets the project's quality,
   `viewport.setSettings {quality}` (step 0) the Scene view's, `graphics.resetPlayerOptions` (new) clears the players'
   file, `editor.status` reports the running game's effective options and their sources.
5. **A reference game uses it.** The Third-Person Adventure's settings menu gets a Graphics row (Low, Medium, High,
   Ultra), applied with `Graphics::SetPreset` and saved with `Graphics::Save`, covered by its command script.
6. Below the cut: supersampled stills for the showcase (`viewport.capture {supersample: 2}`), and the showcase budget
   class for wave 8.

Not the goal: any change of the image (no default of the renderer changes here), resolution or render scale options
(the owner's rule: native resolution only; window size and mode stay `IGameWindow`'s), texture-quality rows
(`plan.md` section 9: later).

## 2. Where we start

### 2.1 Files

| File | Lines | Role, and what this stream does there |
| --- | --- | --- |
| `Strata/src/Strata/Core/EngineServices.h` | 128 | `EngineServices::Register/Get/ScopedProvider` (17-65), `IGameWindow` (97-111), `IGameSaves` (119-126). **Adds `IGameGraphics`, `GameGraphicsSource`.** |
| `Strata/src/Strata/Core/SettingsRegistry.h` / `.cpp` | 136 / 385 | `SettingsField` (45-57), `SettingsSection::Validate` (90; body .cpp:179-239: absent fields default, unknown members refused at the current version, dropped at older ones). Read only. |
| `Strata/src/Strata/Core/ProjectSettings.h` / `.cpp` | 100 / 276 | `IProjectSettings` (24-35), `ProjectSettingsValues`; versions unique across the process (.cpp:20-26). Read only. |
| `R/RendererQuality.h` / `.cpp` | step 0 | Presets, `RendererQualitySettings`, the `Renderer.Quality` section, the typed preset rows (`specs/step0.md` 7.1-7.2). **Adds the row traits table (`RowTraits`, one line per field), `OptionModel`, `GetPresetValues`, `Normalize`, `MakeCustomStart`** (D1, D9). |
| `R/RendererComponents.cpp` | 554 | Registrations; DirectionalLight 222-238 (six properties without a description), PostProcess 341-456 (seven without). **Descriptions and sections** (D11). |
| `Strata/src/Strata/Reflection/Property.h`, `PropertyBuilder.h`, `PropertyJson.cpp` | 130, 128, 517 | `PropertyInfo` (72-110), `PropertyBuilderBase` (48-112), `DescribeProperty` (487-516). **Adds `PropertyInfo::Section`, `.Section()`, `"Section"` in descriptions** (D11). |
| `SC/ScriptHostAPI.cpp` | 3,314 | Host functions; window and saves 2140-2240; input actions 2850-3027; `ST_SCRIPT_HOST_FUNCTIONS` 3036-3191, its `static_assert` 3217. **Seven host functions** (D6). |
| `SDK/ScriptABI.h` | 765 | `StrataScriptHostAPI` 268-663, last member `InputCancelBindingCapture` 660, "New functions are appended here" 662. **Two enums, one struct, seven members** (D6). |
| `SDK/Host.h`, `SDK/StrataScript.h` | 111, 24 | `ST_SCRIPT_DETAIL_HOST_WITH` (Host.h:109-111), the umbrella header. **Graphics.h new, included by the umbrella** (D7). |
| `RT/GameRuntime.h` / `.cpp` | 174 / 368 | Settings provider (.cpp:41-52), saves provider and `InputActions::LoadBindings` (.cpp:98-103). **Owns the graphics service** (D5). |
| `RT/GameRenderer.h` / `.cpp` | 65 / 156 | The game's `SceneRenderer` (.cpp:23-29); step 0 makes it follow `RendererQuality::GetProjectQuality()` unless `SetQualityOverride` is set, and log "Graphics quality: ..." on changes (`specs/step0.md` 7.7). **Follows `IGameGraphics` when one is provided** (D5). |
| `RT/GameGraphicsService.h` / `.cpp` | new | **`GameGraphicsService` (IGameGraphics), `GraphicsOptionsFile`** (D3, D4). |
| `StrataRuntime/src/RuntimeApplication.cpp` | 481 | `RuntimeLayer::OnAttach` (69-100; `GameRuntimeOptions runtimeOptions` at 71, the renderer made at 96), `c_Arguments` (315-331); step 0 adds `--quality` (refused with `--headless`) and calls `GameRenderer::SetQualityOverride`. **Routes `--quality` to the service instead** (D5). |
| `ED/Editor/EditorContext.h` / `.cpp` | 993 / 2,852 | Game shell providers (.cpp:149-160, .h:836-880), `Play` loads bindings (.cpp:1381), `ResetGameShell` (.cpp:1616-1622), `ResetPlayerBindings` (.cpp:1656-1670). **Owns a `GameGraphicsService`** (D5). |
| `ED/Editor/EditorInputActionsCommands.cpp` | 482 | `inputActions.resetPlayerBindings` (461-479): the model of `graphics.resetPlayerOptions`. |
| `ED/Editor/EditorGraphicsCommands.cpp` | new | **`graphics.resetPlayerOptions`** (D8). |
| `ED/Editor/EditorCommands.h` / `.cpp` | - | `RegisterInputActionsCommands` declared (.h:202) and called (.cpp:49). **`RegisterGraphicsCommands` beside it** (D8). |
| `ED/Panels/GameViewPanel.cpp` | 327 | The Game view's `Render` call (122); step 0 passes the project's quality there. **Passes the game's quality** (D5). |
| `ED/Editor/EditorViewportCommands.cpp` | 1,149 | `viewport.capture`'s `StartCapture` and its render (613); step 0 resolves the capture's default quality there and writes the `quality` descriptions. **A scene-camera capture defaults to the game's quality; two descriptions** (D5). Shared with every stream (debug views). |
| `ED/Editor/EditorStateCommands.cpp` | 1,039 | `editor.status` (693-780). **Its description names `game.graphics`** (D8). |
| `ED/UI/SettingsPages.h` / `.cpp` | 120 / 653 | `SettingsPage`, `EditorSettingsPageRegistry` (44-61), `GenericSettingsPage` (.cpp:149-250), `CreatePage` (.cpp:485-492). Read only. |
| `ED/UI/GraphicsQualityPage.h` / `.cpp` | new | **The Graphics Quality page** (D9). |
| `ED/UI/InputActionsPage.cpp` | 763 | `DrawPlayerBindings` (646-670): the model of the page's player banner. |
| `ED/EditorLayer.cpp` | 2,447 | Page registrations (141-150). **One line: the page** (D9). |
| `ED/Panels/ViewportPanel.cpp` | 958 | `DrawChips(EditorContext&)` (326-415, called at 274 inside the chip strip), `DrawDebugViewChip` (561-596). **`DrawQualityChip`** (D10). The plan names `ViewportToolbar.cpp`; the chips are drawn here, and `ViewportToolbar.cpp` (137) is not changed. |
| `ED/Panels/InspectorPanel.cpp` | 1,434 | Component sections (690-775), `DrawProperties` (886-955), `DrawCollisionLayersLink` (1315-1329). **Section headings, the sun's line, the quality line with nothing selected** (D11). |
| `ED/UI/PropertyWidgets.h` | 97 | `BeginPropertyTable`/`EndPropertyTable` (73-74; tables of a window share one label width). **`PropertyGroupHeading`** (D11). |
| `StrataCLI/src/CLI/McpToolsets.cpp` | 185 | Prefix toolsets (42-53), the `settings` toolset (68). **`graphics.` prefix** (D8). |
| `Samples/ThirdPerson/Scripts/SettingsMenu.h` / `.cpp` | 42 / 122 | The settings panel's script. **The Graphics row** (D12). |
| `StrataTests/Editor/ThirdPersonCommands.json.in` | 79 | The sample's regression script; settings at 64-68. **Covers the row** (D12). |
| `StrataTests/src/Scripting/ScriptAPITests.cpp` | 710 | "Every host function has a call counter" checks the last name (680). **Becomes `GraphicsSaveOptions`.** |
| `StrataTests/src/FeatureTest/FeatureTests.cpp` | 276 | The headless runner provides window and saves (236-243) and checks every host function is called (265-269). **Provides the graphics service.** |
| `StrataTests/Architecture/ComponentSchema.json`, `StrataTests/src/Reflection/ComponentSchemaTests.cpp` | -, 53 | The registry's snapshot (descriptions included). **Regenerated** (D11). |

### 2.2 What this stream builds on: step 0's API (`specs/step0.md`, section 7)

The step-0 spec (`specs/step0.md`) is written; this stream builds on its API as specified there. If step 0 as built
renames something, the implementing agent maps the names in its first hour and records them in an "As built" note at the
end of this spec, without changing what is built.

| Step 0 gives (`specs/step0.md`) | What this stream relies on |
| --- | --- |
| `enum class RendererQualityPreset : uint8_t { Low = 0, Medium = 1, High = 2, Ultra = 3, Custom = 4 }`, `c_RendererQualityPresetCount = 4` (7.1) | File and script values, never renumbered |
| `enum class ShadowMapResolution { Auto, AutoFine, Size1024, Size2048, Size4096, Size8192 }`; `RendererQuality::ResolveShadowMapSize(resolution, outputRows)` (7.1, 7.3) | Auto (High's rule: 2048 below 1440 rows, 4096 from them) and AutoFine (Ultra's: 4096 / 8192) are two values, so every named preset can be written as Custom fields (the former open question Q1 is settled by step 0) |
| `struct RendererQualitySettings { Preset; ShadowResolution; ShadowCascades; LocalShadowAtlasSize; LocalShadowMaxTile; MaxShadowedLocalLights; LODErrorPixels; MinProjectedPixels; HasSameKnobs(); }` (7.1) | Unresolved settings: `ShadowResolution` may be Auto or AutoFine, which `SceneRenderer` resolves by its own viewport's rows (7.3). Nothing in this stream resolves by rows except the page's memory estimates. Compared with `HasSameKnobs` (there is no `operator==`) |
| `RendererQuality::c_SectionId`, `c_SectionVersion` (1), `MakeSection()`, `GetPreset(preset)`, `FromSectionValue(const nlohmann::json*)`, `ToSectionValue(settings)`, `GetProjectQuality()` (7.1, 7.6) | A *simple* section (title "Graphics Quality", category Rendering): `Preset` (Enum Low, Medium, High, Ultra, Custom; default High) first, then one field per knob of Bool, Int, Float or Enum type. `FromSectionValue` gives a preset's values under a named preset and the fields under Custom; `ToSectionValue(GetPreset(p))` is the section value of preset p with every field spelled out |
| The typed preset rows `PresetRow<T>` in `RendererQuality.cpp`, one line per knob (7.2) | There is **no common row type**: this stream adds its own traits table, one line per field (D1) |
| `SceneRenderer::SetQuality(const RendererQualitySettings&)`, `GetQuality()`, `GetShadowMapSize()`; `SceneRendererStats::Quality`, `ShadowMapSize`, `LocalShadowTileSizes` (7.5, 7.8) | Rebuilds what changed; the stats say what was drawn |
| `GameRenderer::SetQualityOverride(std::optional<RendererQualitySettings>)`, `GameRenderer::GetQuality()`; `Render` and `RenderEmpty` call `SetQuality(override or GetProjectQuality())` and log "Graphics quality: <preset> (<source>)" when the preset or its source changes (7.7) | D5 puts `IGameGraphics` between the override and the project's quality |
| `StrataRuntime --quality <low\|medium\|high\|ultra>` (refused with `--headless`, exit 3) through `SetQualityOverride`; CTest `StrataRuntime.Quality` (label `gpu`, windowed) expects "Graphics quality: Ultra (--quality)" (7.7, 7.11) | D5 routes `--quality` through `GameRuntimeOptions::Quality` instead; the log line and step 0's CTest stay as they are |
| `enum class ViewportQuality : uint8_t { Project = 0, Low, Medium, High, Ultra }`, `ResolveViewportQuality`, `ViewportSettings::Quality` (Scene view only), `ViewportRenderer::Render(..., const RendererQualitySettings& quality)` with its four callers, `CaptureRequest::Quality` resolved in `StartCapture` (7.7, 7.8) | D5 changes two callers (the Game view, a scene-camera capture's default); D10's chip edits `ViewportSettings::Quality` |
| DG "Quality settings", `Docs/Manual/Settings.md`'s Graphics Quality lines, the feature project's Custom `Renderer.Quality` (7.11, 7.13) | Exist; this stream extends them |

The feature project's value (step 0, 7.11) is Custom with `ShadowResolution` "1024", `ShadowCascades` 3,
`LocalShadowAtlasSize` "2048", `LocalShadowMaxTile` "512", `MaxShadowedLocalLights` 8, `LODErrorPixels` 1.5 and
`MinProjectedPixels` 0.75: the feature scripts below start from it.

### 2.3 Code paths each step changes

- **S1 page and chip:** `ED/EditorLayer.cpp:141-150` (one `Register` line), new `ED/UI/GraphicsQualityPage.*`,
  `ED/Panels/ViewportPanel.cpp:274,326,411` (`DrawChips` takes the panel context; a chip after the debug view chip),
  `R/RendererQuality.h/.cpp` (the traits table and four functions).
- **S2 service and programs:** `Core/EngineServices.h:113-128` (the new interface after `IGameSaves`), new
  `RT/GameGraphicsService.*`, `RT/GameRuntime.h:28-46,150-172` and `.cpp:98-103,132-147`, step 0's quality update in
  `RT/GameRenderer.cpp` (its `Render`/`RenderEmpty`), `StrataRuntime/src/RuntimeApplication.cpp:69-100` and step 0's
  `--quality` handling, `ED/Editor/EditorContext.h:378-395,836-880` and `.cpp:149-160,1381,1616-1622`,
  `ED/Panels/GameViewPanel.cpp:122` and `ED/Editor/EditorViewportCommands.cpp` (`StartCapture`, :613 at bc6ee123; step
  0 adds the quality resolution there).
- **S3 script API:** `SDK/ScriptABI.h:258-260` (types before the table) and `:660-662` (members), `SC/ScriptHostAPI.cpp`
  (implementations before the table comment at :3029, list :3191), new `SDK/Graphics.h`, `SDK/StrataScript.h:14` (after
  `Gameplay.h`: the includes are alphabetical), new `StrataTests/Scripts/API/GraphicsScripts.cpp`,
  `StrataTests/FeatureTest/Scripts/GameFeatures.cpp`, `StrataTests/src/FeatureTest/FeatureTests.cpp:236-243`,
  `StrataTests/src/Scripting/ScriptAPITests.cpp:680`.
- **S4 commands:** new `ED/Editor/EditorGraphicsCommands.cpp`, `RegisterGraphicsCommands` declared beside
  `RegisterInputActionsCommands` (`ED/Editor/EditorCommands.h:202`) and called after it (`EditorCommands.cpp:49`), the
  `"game"` status provider (`ED/Editor/EditorContext.cpp:153-160`), `editor.status`' description
  (`ED/Editor/EditorStateCommands.cpp:694-706`), `StrataCLI/src/CLI/McpToolsets.cpp:53,68`.
- **S5 sample:** `Samples/ThirdPerson/Scripts/SettingsMenu.*`, `MainMenu.stscene` and `Course.stscene` (through
  commands), `StrataTests/Editor/ThirdPersonCommands.json.in:64-68`.
- **S6 discoverability:** `Reflection/Property.h:72-110`, `Reflection/PropertyBuilder.h:48-112`,
  `Reflection/PropertyJson.cpp:487-516`, `R/RendererComponents.cpp:223-238,357-437`,
  `ED/Panels/InspectorPanel.cpp:763-771,886-955,1282`, `ED/UI/PropertyWidgets.h:73-88`,
  `StrataTests/src/Reflection/ComponentSchema.cpp` and `StrataTests/Architecture/ComponentSchema.json`.

## 3. Order and the cut line

| Step | What | Lands with |
| --- | --- | --- |
| S1 | Row traits, `Normalize`, `MakeCustomStart`, memory estimates; the Graphics Quality page; the Scene view's quality chip | Tests, DG "Settings pages" lines |
| S2 | `IGameGraphics`, `GameGraphicsService`, `GraphicsOptions.json`; the runtime, the editor's play mode, the Game view and captures follow it; `--quality` through it | Tests, DG "Rendering > Quality settings" |
| S3 | Seven host functions, `StrataScript/Graphics.h`, the API test module, `Scripting.Graphics`, the feature scripts | DG "Gameplay API", "ABI rules", "Adding a host function" |
| S4 | `graphics.resetPlayerOptions`, `editor.status` `game.graphics`, the page's players' banner | DG "Project settings", the automation skill |
| S5 | The Third-Person Adventure's Graphics row and its command script | Sample README |
| S6 | Property sections, descriptions with units, the sun's line, the nothing-selected line | DG "Inspectors" |
| S7 | `Docs/Manual/Graphics.md`, `Settings.md`, `Scripting.md`, the skills, the AGENTS.md bullet, the release-note lines, the Roadmap line | |
| **Cut line** | | |
| S8 | Supersampled stills: `viewport.capture {supersample: 2}` | |
| S9 | The showcase budget class (`Samples.json` `"Budget": "Showcase"`) | |

Steps run in order; each lands working, tested and documented before the next starts. Below the cut is done only when
everything above is; what is not done moves to the Roadmap's "Later" when the wave lands.

## 4. Design

### D1 The option model: project, player, command line

The options are the fields of `Renderer.Quality`, in field order, by name (case-insensitive lookup, canonical spelling
in every answer). Three layers make the **effective value**, the one value every renderer of a running game follows:

1. **Project**: the project's validated section value (`IProjectSettings::FindSection`, the section default when not
   set), read on the main thread and kept until `IProjectSettings::GetVersion()` changes (the documented pull pattern,
   DG "Project settings", "Reading").
2. **Player**: an object of the fields the player chose (`{"Preset": "Low"}`), at most one member per field, stored in
   the section's form. Session state of the program running the game; persisted only by `SaveOptions`.
3. **Command line**: a named preset (`--quality`), session only, never stored.

**Rows that follow the preset and rows that do not.** Each field but `Preset` gets a trait `FollowsPreset` (default
true). A row that follows the preset (every step-0 row; `ShadowFilterQuality`, `ContactShadows`,
`AmbientOcclusionQuality`, `EnvironmentResolution`, `MotionBlurQuality` later) is used only under `Custom`; under a
named preset its value is the preset's. A row that does not (`camera-and-tone`'s players' rows `AntiAliasing`,
`MotionBlur`, `DepthOfField`, `FilmGrain`, `ChromaticAberration`, `Scene` in every preset) applies under every preset:
changing it never switches the preset to Custom, and the command line's preset does not override it. **Step 0's
`FromSectionValue` must agree:** for a row that does not follow the preset it reads the field under every preset, not
the preset's table (step 0's rows all follow the preset, so nothing changes in step 0; `camera-and-tone` writes its
rows' `FromSectionValue` lines that way, and S1's test "Rows that do not follow the preset are read under every preset"
fails for a line that does not).

**The traits table and functions (graphics-options adds them to `R/RendererQuality.h/.cpp`; one line per field, so the
later streams' lines merge as separate lines):**

```cpp
namespace Strata::RendererQuality
{
	// What graphics options know of a field of Renderer.Quality besides the field itself. RendererQuality.cpp's
	// c_RowTraits holds one line per field but Preset, in field order, beside the knob's PresetRow line.
	struct RowTraits
	{
		std::string_view Field;    // The section field ("ShadowResolution")
		bool FollowsPreset = true; // False: a player's choice the presets leave alone (comfort rows)
		std::string_view GPUStage; // The GPU stage whose time this row changes ("GPU.Shadows"); empty: none of its own
		// Bytes of GPU memory this row's resources take in one renderer for these (unresolved) settings at an output size
		// (Auto and AutoFine resolved with ResolveShadowMapSize(resolution, output.y)); null when the row allocates nothing
		// of its own or another row counts the resource (ShadowCascades is counted with ShadowResolution).
		uint64_t (*MemoryBytes)(const RendererQualitySettings& settings, glm::uvec2 output) = nullptr;
	};
	std::span<const RowTraits> GetRowTraits();
	// The section values of Low, Medium, High and Ultra, every field spelled out: ToSectionValue(GetPreset(p)), made once.
	std::span<const nlohmann::json> GetPresetValues();

	// What the option logic works over: a simple quality section, its rows' traits and its four presets' values. The
	// engine's is GetOptionModel(section); the page's and the service's tests build one over a test section of their own
	// (for example with a row that does not follow the preset before camera-and-tone adds one).
	struct OptionModel
	{
		const SettingsSection* Section = nullptr;
		std::span<const RowTraits> Rows;           // One per field but Preset
		std::span<const nlohmann::json> PresetValues; // Low, Medium, High, Ultra: 4 entries
	};
	OptionModel GetOptionModel(const SettingsSection& section); // The engine's rows and presets over `section`

	// The row of a field, ignoring case (null for Preset and unknown names).
	const RowTraits* FindRow(std::string_view field, const OptionModel& model);

	// The value with every field present and, under a named preset, every row that follows the preset set to the preset's
	// value (PresetValues): what is in effect, spelled out. `value` must be valid at the section's version (Validate's
	// result). Idempotent; for the engine's model FromSectionValue(&Normalize(v)) has the same knobs as FromSectionValue(&v).
	nlohmann::json Normalize(const nlohmann::json& value, const OptionModel& model);

	// The Custom value a switch to Custom starts from (the TAA preset's rule, GetTemporalAntiAliasingCustomStart,
	// R/RendererComponents.cpp:528-537): the rows that follow the preset, taken together, are someone's tuning when they
	// equal neither the fields' defaults (High's values in step 0's section) nor any preset's values; a tuning is kept,
	// otherwise they become the values in effect (Normalize). Rows that do not follow the preset keep their values.
	// Preset becomes "Custom".
	nlohmann::json MakeCustomStart(const nlohmann::json& value, const OptionModel& model);
}
```

(`R/RendererQuality.h` includes `<nlohmann/json_fwd.hpp>` only, step 0's rule; these take and return JSON in
declarations only, and the spans of JSON are of a forward-declared type, which declarations allow. The header adds
`<span>`, `<string_view>` and glm's vector type, which step 0's `SceneRenderData.h` include already brings.)

Step 0's fields get these lines (this stream's first commit): `ShadowResolution` {`GPU.Shadows`, memory
`ShadowCascades * size^2 * 4` bytes with `size = ResolveShadowMapSize(ShadowResolution, output.y)`, D32 per cascade
layer}; `ShadowCascades` {`GPU.Shadows`, none}; `LocalShadowAtlasSize` {`GPU.Shadows`, `LocalShadowAtlasSize^2 * 4`
bytes, the atlas is D32 at `R/SceneRendererLocalShadows.cpp:800`}; `LocalShadowMaxTile`, `MaxShadowedLocalLights`
{`GPU.Shadows`, none}; `LODErrorPixels`, `MinProjectedPixels` {`GPU.Opaque`, none}. High at 2160 rows: 4 x 4096^2 x 4 =
256 MiB of cascades and 64 MiB of atlas per renderer with a shadowed sun and local shadows, as plan S0.4 and step 0's
table (7.2) state. The GPU-driven path's occlusion pyramid of the cascades (R32F, a quarter each way with its mips,
`R/SceneRendererGPUDriven.cpp:1040-1050`, about 8% more) is not counted, as step 0's table does not count it: the
page's caption says "shadow maps and atlas".

**Setting an option** (`GameGraphicsService::SetOption(name, value)`; `base` = `Normalize(Validate(project + player))`,
the command line left out):

1. Unknown name: fails ("there is no graphics option 'X' (options: Preset, ShadowResolution, ...)").
2. `Preset`: `Custom` makes the player's choices `MakeCustomStart(base)` restricted to `Preset` and the rows that follow
   the preset, plus the player's members of rows that do not; a named preset makes them `{"Preset": P}` plus the
   player's members of rows that do not (the rows that follow the preset carry no meaning under a named preset).
3. A row that follows the preset, while `base.Preset` is not `Custom`: first as in 2 with `Custom` (Custom starts from
   the look in effect), then the field; while it is `Custom`: the field alone.
4. A row that does not follow the preset: the field alone.
5. The result, `project + player`, is validated by `SettingsSection::Validate` at the section's version; a refused value
   changes nothing and fails naming the field (the section's own message).
6. The effective value and the sources are recomputed; the version changes when they or the player's choices changed
   (D2).

**Resetting** (`ResetOptions(name)`): `""` removes every choice; `Preset` removes `Preset` and every row that follows the
preset (the project's quality applies again); a row that follows the preset, while the player's `Preset` is `Custom`,
takes `Normalize(project)[field]` (what the project would use for it); otherwise the member is removed. Unknown names
fail.

**The effective value**: `Normalize(Validate(project + player))`, then with a command-line preset C: `Preset` := C and
every row that follows the preset := C's value. Sources: a field is `CommandLine` when a command-line preset is set and
the field is `Preset` or follows the preset; else `Player` when the player has a member for it, or for a row that
follows the preset when the player's `Preset` is set (the player's preset decides it); else `Project`. A value set while
the command line fixes the field is kept as the player's choice (saved by `SaveOptions`, applied in later sessions) and
reported with source `CommandLine` until the session ends; `SetOption` returns true.

**Project changes while playing** (the editor's `project.setSettings`): the project layer is pulled again by version,
the player's choices stay on top. A project switching from High to Low under a player's `{"Preset": "Ultra"}` stays
Ultra; under a player's Custom the player's rows stay.

### D2 `IGameGraphics` (Core)

Appended to `Strata/src/Strata/Core/EngineServices.h` after `IGameSaves` (:119-126). The header adds
`#include <nlohmann/json_fwd.hpp>`, `<string>` and `<string_view>`, and declares `struct SettingsSection;` (it stays out
of `HeaderWeightTests`' `c_FullJsonHeaders`). Scripting may include Core, Input, Asset, Reflection and Scene, but neither
Renderer nor Runtime (Layers.json:76-78), which is why the interface is here and speaks JSON and field names, never
Renderer types.

```cpp
	//////////////////////////////////////////////////////////////////////////
	// The game's graphics options
	//////////////////////////////////////////////////////////////////////////

	// Where the value in effect of a graphics option comes from (the ABI's StrataScriptGraphicsOptionSource, same values).
	enum class GameGraphicsSource : uint8_t
	{
		Project = 0,    // The project's settings (Project Settings > Graphics Quality, the section Renderer.Quality)
		Player = 1,     // The player's choice (SetOption, or GraphicsOptions.json of an earlier session)
		CommandLine = 2 // The program's command line fixes it for the session (StrataRuntime --quality)
	};

	// The players' graphics options of the game a program runs (scripts' Graphics::Set, Get, Reset and Save): the fields of
	// the project settings section the renderer registers (Renderer.Quality), by name, ignoring case. The value every
	// renderer of the game follows is the project's, the player's choices over it and the command line's preset over both
	// (DeveloperGuide "Rendering > Quality settings", "Players' options"). The player's choices last for the session;
	// SaveOptions keeps them in the game's save directory, from which the program loads them when the game starts. Provided
	// by GameRuntime (exported games) and EditorContext (the editor's play mode, for the Game view). Main thread only.
	class IGameGraphics
	{
	public:
		virtual ~IGameGraphics() = default;

		// The section whose fields are the options, in field order; null when the program has none (a registry without the
		// renderer's section): every other function then reports nothing and refuses every change.
		virtual const SettingsSection* GetSection() const = 0;
		// The value in effect: an object with every field of the section at its version (a named preset's rows spelled
		// out). Valid until GetVersion changes.
		virtual const nlohmann::json& GetEffectiveValue() = 0;
		// Changes whenever the value in effect, a source or the player's choices change (the project's settings, a SetOption,
		// a reset, a load, the session's end); unique among the process's providers, so a renderer notices a provider
		// replaced by another.
		virtual uint64_t GetVersion() = 0;
		// Where the value in effect of an option comes from; nullopt for an unknown option.
		virtual std::optional<GameGraphicsSource> GetSource(std::string_view option) = 0;
		// The player's choices: an object of field names and values as the section stores them (what SaveOptions writes).
		// Valid until GetVersion changes.
		virtual const nlohmann::json& GetPlayerOptions() = 0;

		// Sets the player's choice of an option (its stored form: a choice's name, a number, a boolean): a row that follows
		// the preset switches the preset to Custom first, starting from the look in effect. False, changing nothing, with
		// the reason: an unknown option or a value the section refuses.
		virtual bool SetOption(std::string_view option, const nlohmann::json& value, std::string* outError = nullptr) = 0;
		// Removes the player's choice of an option ("" : of every option); see DeveloperGuide for Preset and Custom's rows.
		// False for an unknown option.
		virtual bool ResetOptions(std::string_view option, std::string* outError = nullptr) = 0;
		// Writes the player's choices to <save directory>/GraphicsOptions.json, atomically (the disk is written before it
		// returns). False, with the reason, without a save directory or when the write fails; the choices stay.
		virtual bool SaveOptions(std::string* outError = nullptr) = 0;
	};
```

`GetEffectiveValue`, `GetVersion`, `GetSource` and `GetPlayerOptions` are not const: they pull the project layer
first (an integer compare when nothing changed).

### D3 `GameGraphicsService` (Runtime)

`RT/GameGraphicsService.h/.cpp` (new; the Runtime layer may include Renderer and Core, Layers.json:93-96). The header
includes `Strata/Core/EngineServices.h`, `Strata/Renderer/RendererQuality.h` (JSON forward-declared there) and
`<nlohmann/json_fwd.hpp>`, never `SettingsRegistry.h` (it needs the complete JSON type): the registry is passed by pointer.
JSON state lives in a private `Data` (the `ProjectSettingsValues` pattern, ProjectSettings.h:96-97). `GameRuntime.h` and
`EditorContext.h` forward-declare the class (they hold it in a `Scope`).

```cpp
namespace Strata
{

	class SettingsRegistry;

	// The players' graphics options file, <IGameSaves directory>/GraphicsOptions.json (D4):
	// {"Strata": {"Format": "GraphicsOptions", "Version": 1}, "Section": {"Id": "Renderer.Quality", "Version": n},
	//  "Options": {"Preset": "Low"}}. Only GameGraphicsService reads, writes and deletes it.
	namespace GraphicsOptionsFile
	{
		constexpr const char* c_FileName = "GraphicsOptions.json";
		constexpr uint64_t c_MaxFileBytes = 64 * 1024;
		constexpr uint32_t c_FormatVersion = 1;

		// Reads a file's text against the section and the project's value (valid at the section's version): the player's
		// choices at the section's version in outOptions, members the section no longer has or refuses dropped and
		// described in outDropped. False, with the reason, for text that is no options file at all (too large, not JSON,
		// another format, a newer format or section version, another section, no Options object).
		bool Read(std::string_view text, const SettingsSection& section, const nlohmann::json& projectValue, nlohmann::json& outOptions,
			std::vector<std::string>* outDropped = nullptr, std::string* outError = nullptr);
		std::string Write(const nlohmann::json& options, const SettingsSection& section);
	}

	// IGameGraphics for a program that runs a game: GameRuntime and EditorContext own one. `project` is the program's
	// project settings (null: none, the section's default applies) and `saves` where its game keeps save files (null:
	// nowhere, SaveOptions fails); both must outlive the service.
	class GameGraphicsService final : public IGameGraphics
	{
	public:
		// The options are `registry`'s Renderer.Quality section (null: the process's registry) with the engine's rows and
		// presets (RendererQuality::GetOptionModel); without that section the service has no options (GetSection null).
		GameGraphicsService(const IProjectSettings* project, IGameSaves* saves, const SettingsRegistry* registry = nullptr);
		// Tests: any simple quality section with its rows and presets' values (D1's OptionModel; its section and spans must
		// outlive the service).
		GameGraphicsService(const IProjectSettings* project, IGameSaves* saves, const RendererQuality::OptionModel& model);
		~GameGraphicsService() override;

		GameGraphicsService(const GameGraphicsService&) = delete;
		GameGraphicsService& operator=(const GameGraphicsService&) = delete;

		// The preset StrataRuntime --quality fixes for the session (nullopt: none). Never saved.
		void SetCommandLinePreset(std::optional<RendererQualityPreset> preset);
		std::optional<RendererQualityPreset> GetCommandLinePreset() const;
		// Replaces the player's choices with those of GraphicsOptions.json in the save directory: a file that is no options
		// file is ignored with one warning (the project's quality applies), members it can no longer use are dropped with one
		// warning. Without a save directory or a file the player has no choices.
		void LoadPlayerOptions();
		// The session ends (play stops in the editor): the player's choices are forgotten; the file stays.
		void ResetSession();
		// The file's choices without applying them (the editor while not playing: what the next session starts with); empty
		// without a file. outProblem: why a file is ignored, or what it drops.
		nlohmann::json ReadSavedOptions(std::string* outProblem = nullptr) const;
		// Deletes the file; false, with the reason, when it stays. True when there was none.
		bool DeleteSavedOptions(std::string* outError = nullptr);
		// The save directory's options file (nullopt without a save directory), for editor.status.
		std::optional<std::filesystem::path> GetOptionsFile() const;

		// What renderers draw with now: RendererQuality::FromSectionValue(&GetEffectiveValue()), made again only when the
		// version changed (Auto and AutoFine stay unresolved: each SceneRenderer resolves them by its own rows). The engine's
		// model only (a test model's service returns GetPreset(High)).
		const RendererQualitySettings& GetQuality();

		// IGameGraphics
		const SettingsSection* GetSection() const override;
		const nlohmann::json& GetEffectiveValue() override;
		uint64_t GetVersion() override;
		std::optional<GameGraphicsSource> GetSource(std::string_view option) override;
		const nlohmann::json& GetPlayerOptions() override;
		bool SetOption(std::string_view option, const nlohmann::json& value, std::string* outError = nullptr) override;
		bool ResetOptions(std::string_view option, std::string* outError = nullptr) override;
		bool SaveOptions(std::string* outError = nullptr) override;
	private:
		struct Data;
		Scope<Data> m_Data; // Never null
	};

}
```

- `Data` holds the option model (`GetOptionModel(*section)` with `section = (registry ? *registry :
  SettingsRegistry::GetProcessRegistry()).Find(RendererQuality::c_SectionId)`, in the .cpp; no options without it), the project and saves pointers and the project value's version, the project value (validated, at the section's
  version), the player's choices, the command-line preset, the effective value, the sources, `GetQuality`'s cache and the
  version. Versions come from a process-wide `std::atomic<uint64_t>` in `GameGraphicsService.cpp` starting at 1 (unique
  among `IGameGraphics` providers).
- `Refresh()` (private, called first by every public function): when the project's version changed, read the section
  again (`FindSection`, or the default), then re-validate `project + player` (a project change may make a player's
  member invalid, e.g. a later engine's narrower range: such members are dropped with one warning, as a load drops them)
  and recompute the effective value and the sources; bump the version when the effective value, a source or the
  player's choices changed (D2).
- The file is reached only through the `saves` the service was given (never `EngineServices::Get<IGameSaves>()`: an
  in-process `GameRuntime` in an editor test replaces that provider, and the editor's page must keep reading the
  project's file); the path is `*saves->GetSaveDirectory() / FileSystem::FromUTF8(GraphicsOptionsFile::c_FileName)`.
  `SaveOptions` writes with `FileSystem::WriteText` (atomic, the `InputActions::SaveBindings` path,
  `Input/InputActions.cpp:935-946`); `DeleteSavedOptions` with `FileSystem::Remove` and checks it is gone
  (`InputActions::DeleteSavedBindings`, :977-988). Reads are `FileSystem::ReadBytesPrefix(file, c_MaxFileBytes + 1)`
  (refused above the bound without reading it whole).
- Logging: the service logs only D4's warnings. The info line "Graphics quality: <preset> (<source>)" is step 0's
  `GameRenderer` line (one owner, so no run logs it twice); it now takes its source from the service (D5). No per-frame
  work besides `Refresh`'s integer compare.

### D4 `GraphicsOptions.json`

- **Location.** `<IGameSaves directory>/GraphicsOptions.json`: a shipped game's `<user data>/<Game>/Saves`
  (`SaveDirectory::ForGame`), StrataRuntime's `--save-directory`, the editor's `<project>/.strata/Saves`
  (`EditorContext::GetGameSaveDirectory`, `.cpp:1624-1630`). Script save files are stored as `<name>.save`
  (`Core/SaveFiles.h:14-19`), so no script save can name it and `Game::ListSaveFiles` never lists it, like
  `InputBindings.json` (`Input/InputActionSet.h:174-180`).
- **Format** (UTF-8, tab-indented, final newline; written by `GraphicsOptionsFile::Write`):

  ```json
  {
  	"Options": {
  		"Preset": "Custom",
  		"ShadowCascades": 3,
  		"ShadowResolution": "4096"
  	},
  	"Section": {
  		"Id": "Renderer.Quality",
  		"Version": 1
  	},
  	"Strata": {
  		"Format": "GraphicsOptions",
  		"Version": 1
  	}
  }
  ```

  `Strata.Format` is `"GraphicsOptions"`; `Strata.Version` the file format (`c_FormatVersion` 1: a change of this
  envelope bumps it); `Section.Version` the section's version the options were written at; `Options` the player's
  choices, at most one member per field.
- **Reading** (`GraphicsOptionsFile::Read`), in order, each failure refusing the whole file with its reason: larger than
  64 KiB; not JSON (`JsonUtils::Parse`); no `Strata` object or `Format` other than `"GraphicsOptions"`; `Strata.Version`
  not a positive integer, or above `c_FormatVersion` ("written by a later version of the engine"); `Section` not an
  object, `Section.Id` not `"Renderer.Quality"`; `Section.Version` not a positive integer, or above the section's
  version ("written by a later version of the game"); `Options` not an object; two members of `Options` naming the same
  field ignoring case ("two members name 'Preset'"). Then each member naming a current field ignoring case is renamed to
  the field's canonical spelling first (`SettingsSection::Validate` matches names exactly, `SettingsRegistry.cpp:210-213`,
  so "preset" would otherwise sit beside the project's "Preset"), and the members are upgraded and checked through the
  section's single validation: `candidate = projectValue` with the `Options` members put over it,
  `section.Validate(candidate, storedSectionVersion)`. If it succeeds, the choices are the members that name a current
  field (names matched ignoring case, kept in canonical spelling), with the validated values; members naming no current
  field are dropped and described ("'OldRow' is no graphics option of this game"). If it fails, each member is checked
  alone (`projectValue` with that member, same validation) and those refused are dropped with the section's reason.
  Strings quoted from the file go through `StringUtils::MakeValidUTF8` (AGENTS.md, importers' rule, applied here too:
  the file is player-editable input).
- **Writing** (`GraphicsOptionsFile::Write`): the envelope with the section's current id and version and the choices as
  held (an empty `Options` object when there are none). The service writes even when empty, so a game's "reset to the
  project's" followed by `Save` persists the reset.
- **Bad files.** `LoadPlayerOptions` logs one warning, `"The players' graphics options '<path>' are ignored (<reason>):
  the project's quality applies"`, and leaves the player with no choices; the file is never rewritten by a load (the next
  `SaveOptions` replaces it). Dropped members: one warning, `"The players' graphics options '<path>' set what the game no
  longer offers (<n> dropped; the project's values apply there): <first reason>"`.
- **Versions.** The section's version goes up whenever a stream changes its fields (plan 4.1); an older file upgrades
  through `Validate` as above; a file of a newer section version is ignored (the warning names both versions), never
  half-read. Bounds: the section's value is about 1 KB compact today (about 15 fields after the wave), so 64 KiB leaves
  room for any field set the section can hold without approaching `SettingsSection::c_MaxValueBytes`.

### D5 Programs

**Exported games (`GameRuntime`, StrataRuntime).**

- `GameRuntimeOptions` (`RT/GameRuntime.h:28-46`) gains
  `std::optional<RendererQualityPreset> Quality; // StrataRuntime --quality: the session's preset over the project's and the player's`.
- `GameRuntime` gains, declared after `m_SavesProvider` (:171) so they end first:
  `Scope<GameGraphicsService> m_Graphics;` and `Scope<EngineServices::ScopedProvider<IGameGraphics>> m_GraphicsProvider;`,
  and `GameGraphicsService& GetGraphics()`. `Create` builds them right after `InputActions::LoadBindings()`
  (.cpp:103): `m_Graphics = CreateScope<GameGraphicsService>(m_Manifest.Settings ? &*m_Manifest.Settings : nullptr,
  m_Saves.get(), options.Settings)`, `SetCommandLinePreset(options.Quality)`, `LoadPlayerOptions()`, then the provider;
  the first script call (a scene's `OnCreate`) and the first frame see the player's choices. `~GameRuntime` resets them
  after the scene stopped (with `InputActions::ResetSession`, .cpp:146). `m_Manifest` and `m_Saves` are declared before
  them, so the pointers outlive the service. A version-4 manifest passes null (the section's default applies; no
  provider of the editor's leaks into an in-process game).
- `RuntimeLayer::OnAttach` (`RuntimeApplication.cpp:69-100`) passes step 0's parsed `--quality` as
  `runtimeOptions.Quality` (`GameRuntimeOptions runtimeOptions` at :71), and step 0's
  `m_Renderer->SetQualityOverride(RendererQuality::GetPreset(*m_Options.Quality))` is removed in the same commit:
  the override stays in `GameRenderer` for tests and tools, the command line now reaches the renderer through the
  service. Step 0's checks stay (an unknown value or `--headless` with `--quality`: exit 3), and so does its help line.
- `GameRenderer` (step 0's `Render`/`RenderEmpty`, `specs/step0.md` 7.7): the quality is `m_Override` when set, else
  the game's when an `IGameGraphics` with a section is provided, else `RendererQuality::GetProjectQuality()` (programs
  and tests that render a game without the service):

  ```cpp
  	// The game's quality (private; new members uint64_t m_GraphicsVersion = 0, RendererQualitySettings m_GraphicsQuality):
  	// the provided IGameGraphics' value in effect, made again only when its version changed (versions are unique across
  	// providers, so a replaced provider is noticed too), else the project's.
  	const RendererQualitySettings& GameRenderer::FollowQuality()
  	{
  		IGameGraphics* graphics = EngineServices::Get<IGameGraphics>();
  		if (!graphics || !graphics->GetSection())
  			return RendererQuality::GetProjectQuality();
  		if (graphics->GetVersion() != m_GraphicsVersion)
  		{
  			m_GraphicsQuality = RendererQuality::FromSectionValue(&graphics->GetEffectiveValue());
  			m_GraphicsVersion = graphics->GetVersion();
  		}
  		return m_GraphicsQuality;
  	}
  ```

  `Render` and `RenderEmpty` call `m_Renderer->SetQuality(m_Override ? *m_Override : FollowQuality())`.

  `SetQuality` keeps step 0's "equal knobs: one comparison". Step 0's log line keeps its words, with the source from
  `graphics->GetSource("Preset")`: `Project` "the project's", `Player` "the player's", `CommandLine` "--quality",
  the override "override" (so step 0's CTest `StrataRuntime.Quality`, windowed, still finds "Graphics quality: Ultra
  (--quality)"). A headless run has no renderer and logs no quality line (step 0 refuses `--quality` there).

**The editor's play mode.**

- `EditorContext` owns `Scope<GameGraphicsService> m_GameGraphics` (forward-declared in `EditorContext.h`, built in the
  constructor with `&m_OpenProjectSettings`, `&m_GameSaves` and `&GetSettingsRegistry()`) and its
  `ScopedProvider<IGameGraphics>`. The service is declared after `m_OpenProjectSettings` (.h:877) and the provider after
  `m_ProjectSettingsProvider` (.h:880), and the constructor creates the provider after the other three (.cpp:149-151),
  so it ends first and the providers end in the reverse order of their start.
- `Play` (`.cpp:1381`): after `InputActions::LoadBindings()`, `m_GameGraphics->LoadPlayerOptions()`. `ResetGameShell`
  (`.cpp:1616-1622`, run when play starts, :1379, and stops, :1414): `m_GameGraphics->ResetSession()`. Outside play
  the service has no player choices, so the Game view shows the project's quality.
- New accessors, as the bindings have them (`.h:384-393`):
  `GameGraphicsService& GetGameGraphics()`;
  `const RendererQualitySettings& GetGameQuality()` (`m_GameGraphics->GetQuality()`);
  `const nlohmann::json& GetPlayerGraphicsOptions()` — while playing the running game's choices
  (`GetPlayerOptions`), otherwise those of the file the next session starts with (`ReadSavedOptions`), read again when
  play starts or stops, the project changes or the options are reset: the cache keys of `GetPlayerBindings`
  (`.cpp:1631-1654`: playing, project generation, play session, reset count) with `m_GameGraphics->GetVersion()` in place
  of `InputActions::GetVersion()`, so a script's `Graphics::Set` during play shows at once;
  `const std::string& GetPlayerGraphicsProblem()` (the file's problem, from the same read);
  `bool ResetPlayerGraphicsOptions(std::string* outError)` — no project: fails; deletes the file
  (`DeleteSavedOptions`, failing when it stays); while playing `m_GameGraphics->ResetOptions("")`; bumps the reset count.
- **The Game view and captures of the scene camera** draw with the game's quality, `context.GetGameQuality()`: the Game
  view's `Render` call (`ED/Panels/GameViewPanel.cpp:122`, where step 0 passes the project's) and `StartCapture`'s
  default for a scene-camera view (`EditorViewportCommands.cpp`, step 0's `request.Quality.value_or(view->FromScene ?
  ViewportQuality::Project : ...)` becomes: an explicit `quality` resolves with `ResolveViewportQuality`; without one a
  `FromScene` view takes `context.GetGameQuality()`, any other view `ResolveViewportQuality(settings.Quality)`). Outside
  play both equal the project's, so step 0's tests keep their meaning. Step 0's descriptions change by a clause:
  `viewport.setSettings`' `quality` "...the Game view and the scene's camera always draw at the game's: the project's,
  and while playing the players' options over it"; `viewport.capture`'s `quality` "(default: ... the game's for the
  scene's camera: the project's, with the players' options while playing)". The project's picture
  (`ProjectThumbnail.cpp:139`) keeps the project's (the game's look, never a player's). The Scene view keeps its own
  `quality` setting (step 0; Project = the project's value, never the player's): the person's machine, not the player's.

### D6 Script ABI

Appended to `SDK/ScriptABI.h`. Types go before `StrataScriptHostAPI` (after `StrataScriptInputDevices`, :252-258);
members after `InputCancelBindingCapture` (:660), before the "New functions are appended here" comment, in the plan's
order. `ST_SCRIPT_ABI_VERSION` stays `1u`.

```c
	/* The kind of value a graphics option takes (StrataScriptGraphicsOptionInfo::Type). */
	typedef enum StrataScriptGraphicsOptionType
	{
		StrataScriptGraphicsOptionType_Bool = 0,
		StrataScriptGraphicsOptionType_Int = 1,   /* A whole number from Min to Max */
		StrataScriptGraphicsOptionType_Float = 2, /* A number from Min to Max */
		StrataScriptGraphicsOptionType_Choice = 3 /* One of ChoiceCount names (GraphicsGetOptionChoice) */
	} StrataScriptGraphicsOptionType;

	/* Where the value in effect of a graphics option comes from (StrataScriptGraphicsOptionInfo::Source). */
	typedef enum StrataScriptGraphicsOptionSource
	{
		StrataScriptGraphicsOptionSource_Project = 0,    /* The project's Graphics Quality settings */
		StrataScriptGraphicsOptionSource_Player = 1,     /* The player's choice: GraphicsSetOption, or saved by an earlier session */
		StrataScriptGraphicsOptionSource_CommandLine = 2 /* The program's command line fixes it for this session */
	} StrataScriptGraphicsOptionSource;

	/*
	 * A graphics option (GraphicsDescribeOption). The module sets StructSize to the size of the struct it passes; the engine
	 * fills at most that many bytes, at most its own struct's, and sets StructSize to the bytes it filled: read a member
	 * only when ST_SCRIPT_HAS_MEMBER covers it, so modules of a newer SDK (members appended) work with older engines and the
	 * other way round. The engine refuses a StructSize smaller than this first version's. The strings are engine memory,
	 * valid until the engine call into the module that asked returns: copy them.
	 */
	typedef struct StrataScriptGraphicsOptionInfo
	{
		uint32_t StructSize;
		uint32_t Type;                  /* StrataScriptGraphicsOptionType */
		StrataScriptString Name;        /* What GraphicsGetOption and GraphicsSetOption take ("ShadowResolution") */
		StrataScriptString Title;       /* For menus ("Shadow resolution") */
		StrataScriptString Description; /* What it changes and what it costs */
		double Min;                     /* Int and Float: the inclusive range; 0 for the others */
		double Max;
		uint32_t ChoiceCount;           /* Choice: how many choices; 0 for the others */
		uint32_t Source;                /* StrataScriptGraphicsOptionSource of the value in effect when it was described */
	} StrataScriptGraphicsOptionInfo;

	ST_SCRIPT_DETAIL_STATIC_ASSERT(sizeof(void*) != 8 || (sizeof(StrataScriptGraphicsOptionInfo) == 80
		&& offsetof(StrataScriptGraphicsOptionInfo, Type) == 4 && offsetof(StrataScriptGraphicsOptionInfo, Name) == 8
		&& offsetof(StrataScriptGraphicsOptionInfo, Title) == 24 && offsetof(StrataScriptGraphicsOptionInfo, Description) == 40
		&& offsetof(StrataScriptGraphicsOptionInfo, Min) == 56 && offsetof(StrataScriptGraphicsOptionInfo, Max) == 64
		&& offsetof(StrataScriptGraphicsOptionInfo, ChoiceCount) == 72 && offsetof(StrataScriptGraphicsOptionInfo, Source) == 76),
		"StrataScriptGraphicsOptionInfo's first version is part of the ABI");
```

The members (after `InputCancelBindingCapture`):

```c
		/*
		 * Graphics options (added after InputCancelBindingCapture, ABI version 1: check ST_SCRIPT_HAS_MEMBER before use): the
		 * players' choice of rendering quality, a game's options menu. The options are the fields of the project's Graphics
		 * Quality settings (the section Renderer.Quality) by name, ignoring case: "Preset" (Low, Medium, High, Ultra,
		 * Custom) first, then a row per costly setting, so options a later engine adds reach scripts without new
		 * functions. The value in effect is the project's, the player's choices over it, and over both the preset the
		 * program's command line fixes for the session (StrataRuntime --quality); renderers follow it from the next frame
		 * (a change of shadow resolution rebuilds the shadow maps once). Setting a row other than Preset while Preset is
		 * not Custom switches it to Custom, starting from the look in effect. The player's choices last for the session:
		 * GraphicsSaveOptions writes them into the game's save directory (the engine's file, outside the save files), from
		 * which the program running the game reads them when the game starts. Options are numbered 0 to
		 * GraphicsGetOptionCount - 1 in a fixed order for the engine's run (not across engine versions: use names).
		 * GraphicsDescribeOption fills an info struct (see StrataScriptGraphicsOptionInfo); GraphicsGetOptionChoice copies
		 * the name of a Choice option's choice (the text buffer protocol; 0 for anything else). GraphicsGetOption reads
		 * the value in effect: Bool, Int, Float, or for a Choice a String with the choice's name, copied into stringBuffer
		 * (at most stringCapacity bytes) with outValue->As.String set to { stringBuffer, full size }. GraphicsSetOption
		 * takes Bool for Bool, Int for Int, Float or Int for Float, and String (a choice's name, ignoring case) for a
		 * Choice, never a number (choices are named; their order may change between engine versions, and "2048" is a
		 * name); it refuses, changing nothing, other types and values out of range. GraphicsResetOptions
		 * removes the player's choice of an option, or of every option for an empty name (Preset: the project's quality
		 * applies again). Every function fails (0, false) when the program running the game has no graphics options.
		 */
		uint32_t (*GraphicsGetOptionCount)(StrataScriptContext* context);
		bool (*GraphicsDescribeOption)(StrataScriptContext* context, uint32_t index, StrataScriptGraphicsOptionInfo* outInfo);
		bool (*GraphicsGetOption)(StrataScriptContext* context, StrataScriptString option, StrataScriptValue* outValue, char* stringBuffer,
			uint64_t stringCapacity);
		bool (*GraphicsSetOption)(StrataScriptContext* context, StrataScriptString option, const StrataScriptValue* value);
		uint64_t (*GraphicsGetOptionChoice)(StrataScriptContext* context, uint32_t index, uint32_t choice, char* buffer, uint64_t capacity);
		bool (*GraphicsResetOptions)(StrataScriptContext* context, StrataScriptString option);
		bool (*GraphicsSaveOptions)(StrataScriptContext* context);
```

**Implementation (`SC/ScriptHostAPI.cpp`)**, a section "The game's graphics options" after the input actions'
functions (before the table comment, :3029):

- `IGameGraphics* RequireGameGraphics(ScriptSystem& system, const char* function)`: `EngineServices::Get<IGameGraphics>()`
  with a section, else `ReportProblem(function, "the program running the game has no graphics options")` and null (the
  `RequireGameWindow` pattern, :2144-2150).
- The options are the section's fields of type Bool, Int, Float or Enum, in field order (Renderer.Quality has no String
  or Asset field; `Renderer.Quality.Fields are graphics options` in S1 checks it). A helper
  `std::optional<size_t> FindOptionField(const SettingsSection&, std::string_view name)` matches ignoring case.
- Each function: `HostCall(name, failure, [&] { ... })`, `ResolveContext(context, name)` first, misuse reported once
  through `ScriptSystem::ReportProblem` (SC/ScriptSystem.h:144: each distinct message once, at warning level), failure
  values, never an assert:
  - `HostGraphicsGetOptionCount`: the count (0 without options, reported).
  - `HostGraphicsDescribeOption`: null `outInfo`, `outInfo->StructSize < 80` (`c_GraphicsOptionInfoFirstSize =
    offsetof(StrataScriptGraphicsOptionInfo, Source) + sizeof(uint32_t)`), or an index out of range: reported, false.
    Fills a local struct (strings pointing into the frozen registry's `SettingsField` strings; Min/Max from the field for
    Int and Float; `ChoiceCount` = `Options.size()` for Enum; `Source` from `GetSource`), sets its `StructSize` to
    `filled = min(outInfo->StructSize, sizeof(StrataScriptGraphicsOptionInfo))`, copies the first `filled` bytes.
  - `HostGraphicsGetOption`: null `outValue`, an unreadable or unknown name: reported, false. Converts the effective
    member: Bool → `Bool`; Int → `Int` (int64); Float → `Float`; Enum → `String` into the caller's buffer (the
    `HostGetProperty` pattern, :659-684).
  - `HostGraphicsSetOption`: null `value`, unknown name, a type the option does not take (the rules above: an Int for a
    Choice is refused with "'ShadowResolution' takes the name of a choice (Auto, AutoFine, 1024, ...)", so
    `Set<int32_t>("ShadowResolution", 2048)` cannot silently pick a choice by position), non-finite floats: reported,
    false. Otherwise
    `IGameGraphics::SetOption(canonical, json, &error)`; its error is reported as `"<Option>: <error>"`.
  - `HostGraphicsGetOptionChoice`: an option out of range, not a Choice, or a choice out of range: reported, 0. Else
    `CopyText(choice, buffer, capacity)` (:139-144).
  - `HostGraphicsResetOptions`: `option.Size > 0` but unreadable is reported and false (an unreadable name must not
    become "every option", as `HostInputResetActionBindings` guards, :2919-2940); else `ResetOptions`.
  - `HostGraphicsSaveOptions`: `SaveOptions(&error)`, the error reported.
- `static_assert`s beside the window's (:2152-2154): `StrataScriptGraphicsOptionSource_*` equal
  `static_cast<uint32_t>(GameGraphicsSource::*)`.
- `ST_SCRIPT_HOST_FUNCTIONS` (:3036-3191) gains, after `X(InputCancelBindingCapture)`: `X(GraphicsGetOptionCount)`,
  `X(GraphicsDescribeOption)`, `X(GraphicsGetOption)`, `X(GraphicsSetOption)`, `X(GraphicsGetOptionChoice)`,
  `X(GraphicsResetOptions)`, `X(GraphicsSaveOptions)`. The `static_assert` at :3217 fails the build until the list
  matches the struct; `CreateHostAPI` gives each a call counter (non-Dist).

**Why these signatures (permanent):** names, not indices, for reading and writing, so a later engine's added or
reordered rows never break a menu; indices only to enumerate (options and choices alike: step 0 already put `AutoFine`
before the sizes, so a choice's position is no stable value). One extensible info struct instead of a function per
attribute (seven functions in all, the plan's set). Choice values cross as their names (no ABI enum per row), so
`QualityPreset` is the SDK's mapping of `Preset`'s choice names, checked by a test (S3), not an ABI type. No resolution,
window or render-scale function: those are `IGameWindow`'s, and no render scale exists.

### D7 The SDK: `SDK/Graphics.h`

New header, added to `SDK/StrataScript.h` after `Gameplay.h` (:14; the includes are alphabetical). Every public name is
exercised by the feature scripts
(`SDKCoverageTests`); every call checks its host function with `ST_SCRIPT_DETAIL_HOST_WITH` (`SDK/Host.h:109-111`) and
degrades on older engines (empty lists, nullopt, false).

```cpp
#pragma once

#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// The presets of the "Preset" option, in the order of its choices: what Project Settings > Graphics Quality offers.
	enum class QualityPreset : uint32_t
	{
		Low = 0,
		Medium = 1, // The costs of Strata 0.6.0
		High = 2,   // The default
		Ultra = 3,
		Custom = 4  // Every other option as set
	};

	enum class GraphicsOptionType : uint32_t
	{
		Bool = StrataScriptGraphicsOptionType_Bool,
		Int = StrataScriptGraphicsOptionType_Int,
		Float = StrataScriptGraphicsOptionType_Float,
		Choice = StrataScriptGraphicsOptionType_Choice
	};

	enum class GraphicsOptionSource : uint32_t
	{
		Project = StrataScriptGraphicsOptionSource_Project,
		Player = StrataScriptGraphicsOptionSource_Player,
		CommandLine = StrataScriptGraphicsOptionSource_CommandLine
	};

	// A graphics option as an options menu shows it.
	struct GraphicsOption
	{
		std::string Name;        // What Get and Set take
		std::string Title;       // For the menu
		std::string Description;
		GraphicsOptionType Type = GraphicsOptionType::Bool;
		double Min = 0.0;        // Int and Float
		double Max = 0.0;
		std::vector<std::string> Choices; // Choice: the names Set takes, in order
		GraphicsOptionSource Source = GraphicsOptionSource::Project; // Where the value in effect comes from
	};

	namespace Detail
	{
		constexpr std::array<std::string_view, 5> c_QualityPresetNames = { "Low", "Medium", "High", "Ultra", "Custom" };
	}

	// The players' graphics options (a game's options menu): the rows of Project Settings > Graphics Quality by name. The
	// value in effect is the project's, the player's choices over it, and the command line's preset over both
	// (StrataRuntime --quality); renderers follow it from the next frame. Setting a row other than "Preset" while the
	// preset is not Custom switches it to Custom, starting from the look in effect. Choices last for the session; Save
	// keeps them for the next run (the engine's file in the game's save directory).
	class Graphics
	{
	public:
		// Every option, in the engine's order ("Preset" first).
		static std::vector<GraphicsOption> Options();
		// The option of that name (ignoring case), or nothing.
		static std::optional<GraphicsOption> FindOption(std::string_view name);

		// The value in effect: bool, int32_t, int64_t, uint32_t or float for Bool, Int and Float options, std::string for a
		// Choice (the choice's name); nothing for an unknown option or another type.
		template<typename T>
		static std::optional<T> Get(std::string_view option);
		// The player's choice: false (logged by the engine), changing nothing, for an unknown option, another type or a value
		// out of range. A Choice option takes the choice's name (the string overloads), never a number.
		template<typename T>
		static bool Set(std::string_view option, const T& value);
		static bool Set(std::string_view option, std::string_view choice);
		static bool Set(std::string_view option, const char* choice) { return Set(option, std::string_view(choice)); }

		static std::optional<QualityPreset> GetPreset();
		static bool SetPreset(QualityPreset preset);
		// "Low" ... "Custom" (the Preset option's choice names).
		static std::string_view GetPresetName(QualityPreset preset);

		// Removes the player's choice of an option, or of every option (empty): the project's values apply again.
		static bool Reset(std::string_view option = {});
		// Writes the player's choices for the next run; false when the game has nowhere to keep them.
		static bool Save();
	};

}
```

Bodies, inline in the header (sketch of the non-obvious ones):

- `Options()`: needs `GraphicsGetOptionCount`, `GraphicsDescribeOption` and `GraphicsGetOptionChoice`; for each index
  a value-initialized `StrataScriptGraphicsOptionInfo` with `StructSize = sizeof(info)`; skipped if the call fails;
  strings copied with `Detail::FromABIString`; `Source` read only when `ST_SCRIPT_HAS_MEMBER(StrataScriptGraphicsOptionInfo,
  &info, Source)`; choices read with `Detail::ReadHostString` (`SDK/Host.h:72-87`).
- `Get<T>`: the `Entity::GetProperty<T>` pattern (`SDK/Entity.h:349-374`: a 256-byte stack buffer, a second call with
  the reported size, `Detail::ValueTraits<T>::FromValue`).
- `Set<T>`: `Detail::ValueTraits<T>::ToValue(value)` into `GraphicsSetOption`. `Set(option, std::string_view)` builds a
  `StrataScriptValueType_String` value over the view.
- `GetPreset()`: `Get<std::string>("Preset")`, matched against `c_QualityPresetNames` ignoring ASCII case.
  `SetPreset(p)`: `Set("Preset", GetPresetName(p))`; an out-of-range enum value returns false without a call.
  `GetPresetName` returns `""` out of range.
- `Reset(option)`, `Save()`: forward.

`Architecture.ScriptSDK` stays true: no engine, editor or test translation unit includes `Graphics.h` (it is
exercised only inside script modules).

### D8 Commands, status and the MCP toolset

**`graphics.resetPlayerOptions`** (new file `ED/Editor/EditorGraphicsCommands.cpp`, `RegisterGraphicsCommands`,
registered where `inputActions.*` are; kind `EditorCommandKind::Session`):

- Description: "Deletes the players' graphics options of the project's game: the file
  <project>/.strata/Saves/GraphicsOptions.json that a game's options menu wrote with Graphics::Save while the game played
  here, and that every play session starts with, so that the project's Graphics Quality (the settings section
  Renderer.Quality, project.setSettings) applies in play again (while the file sets the preset, changes of the project's
  preset do not show in play; the Graphics Quality page says so). While playing, the running game also drops the
  players' choices from its next frame. Not undoable, like the game's saves. Returns reset: the options the players'
  choices had set."
- Schema: `ObjectSchema({})`. Result: `{ "reset": ["Preset", ...] }` (the keys of `GetPlayerGraphicsOptions()` before
  the reset, sorted).
- Fails without a project ("No project is open: the players' graphics options belong to a project's game (project.open
  or project.create first)") and when the file stays (`ResetPlayerGraphicsOptions`' reason). No undo step, no
  `RunningCopy` (it changes neither the scene nor the project).

**`editor.status`**: the `"game"` provider (`ED/Editor/EditorContext.cpp:153-160`) gains a member `graphics`:

```json
"graphics": {
	"preset": "Low",
	"effective": { "Preset": "Low", "ShadowResolution": "1024", "ShadowCascades": 3, "...": "..." },
	"sources": { "Preset": "player", "ShadowResolution": "player", "...": "project" },
	"playerOptions": { "Preset": "Low" },
	"playerFile": "C:/.../.strata/Saves/GraphicsOptions.json",
	"playerFileProblem": null
}
```

`effective` and `sources` are the service's (outside play: the project's, every source `"project"`); `playerOptions`
is `GetPlayerGraphicsOptions()` (outside play: the file's, what the next session starts with); `playerFile` is
`GetGameGraphics().GetOptionsFile()` when that file exists, else null; `playerFileProblem` the warning text of a refused
or partly dropped file (`GetPlayerGraphicsProblem()`), else null.
Sources are lower-case strings (`"project"`, `"player"`, `"commandLine"`). The status description
(`EditorStateCommands.cpp:694-706`) changes "game (the running game's window requests, cursor and save directory)" to
"game (the running game's window requests, cursor, save directory and graphics: the Graphics Quality options in effect,
where each comes from, and the players' saved options)". Agents read it as `/game/graphics/...` (plan: "section
`game.graphics`").

**MCP.** `c_PrefixToolsets` (`McpToolsets.cpp:42-53`) gains `{ "graphics.", "settings" }`; the `settings` toolset's text
(:68) becomes "Project settings, graphics quality, collision layers, input actions, the game's identity and the editor's
preferences". `core` is unchanged (32 of 32 tools).

**Lists that name commands:** `StrataTests/src/Editor/CommandJournalTests.cpp` `c_SessionCommands` (~:144) gains
`graphics.resetPlayerOptions`; `EditorMcpToolsetsTests.cpp:103` the settings set.

### D9 The Graphics Quality page

`ED/UI/GraphicsQualityPage.h/.cpp`, a `SettingsPage` registered in `EditorLayer`'s constructor beside the others
(`ED/EditorLayer.cpp:141-150`):

```cpp
m_SettingsPages.Register(std::string(RendererQuality::c_SectionId), [](EditorContext&, const SettingsSection& section, SettingsWindowKind)
	{ return CreateScope<GraphicsQualityPage>(section); });
```

A registered page wins over the generic page for a simple section (`EditorSettingsPageRegistry::CreatePage`,
`SettingsPages.cpp:485-492`), so step 0's generic page stops showing for it. No new panel: the page is drawn inside
the Project Settings panel (`EditorPanelRegistry` id `ProjectSettings`, DG "Settings pages"), never with `ImGui::Begin`;
`EditorShell::ShowProjectSettings("Renderer.Quality")` (`ED/UI/EditorShell.h:80`) opens it.

```cpp
	// The Graphics Quality page (the project section Renderer.Quality): the preset as five chips, then a table with a row per
	// quality row of the renderer and a column per preset, the chosen column highlighted and Custom's editable, each row's
	// memory at 1920 x 1080 and 3840 x 2160 and the time its GPU stage takes now; the rows the presets leave alone (players'
	// choices) below it. Every edit is one project.setSettings (one undo step) with CommandOrigin { CommandSource::UI }.
	// Probe keys under "ProjectSettings.Renderer.Quality.": see D9.
	class GraphicsQualityPage final : public SettingsPage
	{
	public:
		// The engine's rows and presets over the registered section (RendererQuality::GetOptionModel(section)).
		explicit GraphicsQualityPage(const SettingsSection& section);
		// Tests: a test section with its rows and presets' values (D1's OptionModel; they outlive the page).
		explicit GraphicsQualityPage(const RendererQuality::OptionModel& model);
		void Draw(EditorPanelContext& context, float width) override;
	private:
		void Refresh(const EditorContext& context); // Reads the section again when the project's settings version changed
		void DrawPresets(EditorPanelContext& context);
		void DrawTable(EditorPanelContext& context, float width);
		void DrawChoices(EditorPanelContext& context);       // Rows with FollowsPreset false; nothing when there are none
		void DrawMemory(float width);
		void DrawGPU(EditorPanelContext& context);
		void DrawPlayerOptions(EditorPanelContext& context); // The banner while a players' file or session overrides the project
		void Apply(EditorPanelContext& context, nlohmann::json value); // project.setSettings { section, value }
	};
```

(The memory row and the "Auto" cells need `RendererQualitySettings` for a column: `GetPreset(P)` for the engine's model,
`FromSectionValue(&value)` for Custom; a test model, whose section is not the engine's, shows no memory row.)

**Layout** (sizes in multiples of `ImGui::GetFontSize()`, no fixed pixels; colors only from `UI::GetThemeColors()`):

1. **Caption** (`TextSecondary`, wrapped to `width`): "Every renderer of the project follows this: the editor's views
   (unless a view chooses its own quality), captures and the exported game. Players can change it in the game's options
   (Graphics). Medium has Strata 0.6.0's costs; High is the default. Nothing here lowers the rendering resolution."
2. **Preset chips:** five `UI::ToggleChip` (`ED/UI/Widgets.h:55`), icon `Icons::SlidersHorizontal` on the first, labels
   Low, Medium, High, Ultra, Custom, the chosen one on. A click on a named preset runs
   `project.setSettings {section, value: stored-or-default value with "Preset": P}` (the rows' stored values are kept,
   so Custom's tuning comes back when Custom is chosen again); a click on Custom runs it with
   `RendererQuality::MakeCustomStart(value, model)`. Probe keys `Preset.<Name>`.
3. **The table** (`ImGui::BeginTable("Rows", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
   ImGuiTableFlags_BordersInnerV)`): columns Setting (stretch), Low, Medium, High, Ultra, Custom (each `7 *
   GetFontSize()` wide). The header row shows the presets; the chosen preset's header text in `Accent` and every cell of
   its column on `AccentMuted` (`ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ...)`). Rows are the table's rows
   that follow the preset, in table order. Per row:
   - the label: the field's `Title`, its tooltip the field's `Description` plus the memory line and the GPU line of
     that row (below), probe `Row.<Field>`;
   - Low to Ultra: the preset's value as text (`model.PresetValues[P][field]`; Enum: the option name, `Auto` shown as
     "Auto (2048 / 4096)" and `AutoFine` as "Auto fine (4096 / 8192)" from `ResolveShadowMapSize(r, 1439)` and
     `ResolveShadowMapSize(r, 1440)`, so the texts follow step 0's rule rather than repeat it; Int: the
     number; Float: up to two decimals; Bool: On/Off), `TextSecondary` unless it is the chosen column (`Text`); a cell's
     tooltip names its memory at both resolutions. Probe `Cell.<Field>.<Preset>`;
   - Custom: `DrawPropertyWidget` over a `PropertyInfo` made from the field (the generic page's `MakeProperty`,
     `SettingsPages.cpp:83`, and its value conversions beside it; move them to a shared header
     `UI/SettingsFieldProperty.h/.cpp` so both pages use them), probe `Custom.<Field>`. Under Custom it shows the stored value; under a named preset it shows
     `MakeCustomStart(value)`'s, drawn with `TextSecondary` and a tooltip "Editing a value switches to Custom, starting
     from <Preset>'s values". A finished edit runs `project.setSettings` with `MakeCustomStart(value)` (or the stored
     value under Custom) and that field set: one step.
4. **Players' choices** (only when a row with `FollowsPreset == false` exists; none until `camera-and-tone` lands):
   `UI::Heading("Players' choices", TextSize::Caption)` and a property table (`UI::BeginPropertyTable("Choices")`): one
   row per such field, `DrawPropertyWidget` editing it, each finished edit one `project.setSettings` with the field set
   and the preset unchanged. Probe `Choice.<Field>`. Caption: "The presets leave these alone: the game's look keeps
   them, and players may turn them off."
5. **Memory** (two rows under the table, same columns): "Memory at 1920 x 1080" and "at 3840 x 2160": per column the
   sum of the rows' `MemoryBytes` for that column's settings (`GetPreset(P)`; Custom: `FromSectionValue(&value)` of the
   Custom value the column shows) with `output = {1920, 1080}` / `{3840, 2160}`, formatted "64 MiB" / "1.0 GiB". Probes
   `Memory1080.<Column>`, `Memory2160.<Column>`. Caption: "Shadow maps and atlas of one view. Each renderer holds its
   own: the editor's Scene and Game views and its captures, the exported game one. Shadow maps exist only while a light
   casts shadows."
6. **GPU now** (one line, `TextSecondary`, monospace numbers): from the Scene view's renderer
   (`context.Context.GetViewport().FindRenderer()`, `EditorViewport.h:203`): "Now in the Scene view (High, 2560 x 1440):
   GPU.Shadows 0.18 ms, GPU.Opaque 0.42 ms, all stages 1.31 ms" — the stages the rows name, from
   `GetGPUStats().StageMs` and `FrameProfiler::Get().GetStageName` (as `ViewportToolbar::DrawStatsOverlay` does,
   `ViewportToolbar.cpp:91-114`); "GPU times appear while the Scene view renders" without times. The line is formatted
   again at most twice a second (times change every frame; the rest of the page only on version changes). Probe `GPU`.
   A row's tooltip repeats its own stage's time.
7. **Players' options banner** (S4): when `GetPlayerGraphicsOptions()` is not empty, a `UI::Banner(probe, Icons::Gamepad2,
   text, colors.Info)` and a `UI::ToolbarButton(..., Icons::RotateCcw, "Reset Player Options", ...)` running
   `graphics.resetPlayerOptions` (the Input Actions page's pattern, `InputActionsPage.cpp:646-670`). Text while playing:
   "The running game's player chose: Preset Low (marked)"; otherwise "The players' saved options replace the project's
   in play: Preset Low. Changes here reach play once they are reset." The rows the players' options set show a `UI::Pill`
   "Player" in their label cell (`colors.Info`). Probes `PlayerOptions`, `PlayerOptions.Reset`, `Row.<Field>.Player`.
8. **Problem and reset:** a stored value the section refuses shows the generic page's warning caption (`colors.Warning`,
   probe `Problem`); "Reset to Defaults" (`UI::DialogButton`, probe `Reset`) runs `{reset: true}`, enabled while stored.

The page reads the section's value only when `ProjectSettingsValues::GetVersion()` changes (`Refresh`), the player's
options only when `EditorContext`'s cached read changes, and formats the table's texts then, never per frame. Memory
estimates are computed in `Refresh`. Every edit goes through `RunEditorCommand` with `CommandOrigin { CommandSource::UI }`
(DG "Settings pages", "Rules").

### D10 The Scene view's quality chip

`ViewportPanel::DrawQualityChip(EditorPanelContext&, ViewportSettings&)`, drawn in `DrawChips` after the debug view
chip (`ViewportPanel.cpp:411`). `DrawChips` takes `EditorContext&` today (:326) and has no shell to open Project
Settings with: it takes `EditorPanelContext&` instead (its one caller, :274, has `panelContext`; the body reads
`panelContext.Context` where it read `context`).

- `UI::ToggleChip("Viewport.Quality", Icons::MonitorCog, label, &lit, tooltip)`; lit while the view chooses its own
  preset; label the preset in effect ("High", or "Ultra" while overriding). A click opens the popup `Quality` (the
  debug view chip's pattern, :561-596).
- Items: "Project (High)" (the project's preset, from `RendererQuality::GetProjectQuality().Preset`; selects step 0's
  `ViewportQuality::Project`), Low, Medium, High, Ultra (`ViewportQuality::Low` ... `Ultra`), a separator and "Graphics
  Quality..." (`panelContext.Shell->ShowProjectSettings(std::string(RendererQuality::c_SectionId))` when `Shell` is set).
  Probes `Viewport.Quality.<Item>` (`Project`, `Low` ... `Ultra`, `Open`).
- Tooltip: "The Scene view's rendering quality. Project follows Project Settings > Graphics Quality (High); a preset here
  changes only this view. The Game view, captures through the scene camera and the game follow the project and the
  players' options."
- The chip writes `settings.Quality` directly, as every other chip of the strip writes its viewport setting (viewport
  settings are the person's view, not the scene or the project; `viewport.setSettings {quality}` is the agents' way).

### D11 Property sections, descriptions and the sun's line

**Reflection** (`Strata/src/Strata/Reflection/`):

- `PropertyInfo` gains `std::string Section; // The group editors show it under ("Shadows"); empty: none`.
- `PropertyBuilderBase` gains `Derived& Section(std::string name)` which sets the section of every property added after
  it, until the next call (a member `std::string m_Section`, copied into each property in `Property`, `EnumProperty`,
  `AssetProperty`, `EntityProperty`, `CustomProperty`). Registration order never changes: sections need not be
  contiguous (Saturation and Contrast join Tone and color, registered last), so cooked scenes, files and the snapshot's
  order stay.
- `DescribeProperty` (`PropertyJson.cpp:487-516`) adds `"Section"` when not empty, so `component.list` shows it.
- `StrataTests/src/Reflection/ComponentSchema.cpp`'s `DescribeComponentSchema` adds `"Section"` the same way;
  `StrataTests/Architecture/ComponentSchema.json` is regenerated with `--strata-test-helper=write-component-schema`, and
  the commit's diff is reviewed to change only DirectionalLight's and PostProcess' tooltips and sections.

**Registrations** (`R/RendererComponents.cpp`):

- DirectionalLight: `.Section("Light")` before Color, `.Section("Shadows")` before CastShadows. Descriptions (the six
  without one):
  - Color: "Linear color of the light, times Intensity: (1, 1, 1) for the midday sun; the physical atmosphere dims and
    reddens it toward the horizon itself, so tint it only for a look"
  - CastShadows: "Whether the light casts shadows: cascaded shadow maps out to Shadow Distance, their resolution and
    number set by Project Settings > Graphics Quality"
  - ShadowDistance: "Meters from the camera the shadows reach; the last cascade fades out before it. Shorter spends the
    same shadow-map texels on less ground: sharper near shadows"
  - ShadowSoftness: "Angular diameter of the light's disk in degrees (the sun: about 0.53): the penumbra widens with the
    distance from the caster. 0: the hardest shadow the filter allows"
  - ShadowBias: "Depth offset toward the light in shadow-map texels: raise it against shadow acne (stripes on lit
    surfaces), lower it when shadows detach from their casters"
  - ShadowNormalBias: "Offset of the lookup along the surface normal in shadow-map texels: raise it against acne on
    surfaces at grazing angles to the light"
- PostProcess sections, in order of first appearance: "Tone and color" (Tonemapper; later Saturation, Contrast),
  "Exposure" (Exposure, AutoExposure and its three), "Bloom" (3), "Ambient occlusion" (3), "Anti-aliasing"
  (AntiAliasing, AntiAliasingMethod, TAAPreset, the ten TAA tunables), "Reflections" (the three SSR), "Lens"
  (Vignette); "Motion" is `camera-and-tone`'s. Descriptions (the seven without one):
  - Tonemapper: "The curve that maps the scene's light to the display: ACES (filmic, contrasty), AgX (filmic, keeps bright
    colors' hue), Khronos Neutral (keeps colors close to the materials'), Reinhard (soft), None (clipped: for UI and
    tests)"
  - AutoExposure: "Adapts the exposure to the scene's brightness over time, as an eye or a camera's meter does, within
    Auto Exposure Min and Max EV; off, Exposure alone sets it"
  - Bloom: "A soft glow around bright light: the light a camera's lens scatters"
  - AmbientOcclusion: "Darkens creases, corners and contacts that ambient light reaches less (screen-space ambient
    occlusion)"
  - AmbientOcclusionIntensity: "How strongly the occlusion darkens ambient light: 1 as measured, 0 none, above 1 stronger"
  - Vignette: "Darkens the image toward its corners, as a lens does: 0 off, 1 strongest"
  - Saturation: "Color saturation after the tone curve: 1 unchanged, 0 gray, 2 twice as vivid"
- A test (S6) requires a description on every property of DirectionalLight and PostProcess, so the other streams' new
  properties there come with one.
- **How the later streams add properties.** `.Section(name)` applies to every property registered after it, so a new
  property appended at the end of a registration is preceded by its own `.Section(...)` call (sections need not be
  contiguous: the Inspector draws them in order of first appearance); one inserted inside its group needs none. Either
  way the snapshot is regenerated. Descriptions this stream writes that another stream's change makes wrong are that
  stream's to rewrite when it merges main: `sun-shadows` rewrites ShadowSoftness' (its own text, `specs/sun-shadows.md`
  4.2), `camera-and-tone` adds Filmic to Tonemapper's and the lens model to Vignette's.

**Inspector** (`ED/Panels/InspectorPanel.cpp`):

- `UI::PropertyGroupHeading(std::string_view text, std::string_view probeKey)` (new, `ED/UI/PropertyWidgets.h`, inside
  namespace `UI`): called between `EndPropertyTable` and the next `BeginPropertyTable` (tables of a window share their
  label width, :71-73): spacing of `0.35 * GetFontSize()`, the text in SemiBold at `TextSize::Caption` in
  `TextSecondary`, then a separator line in `Border`. Probe records the text.
- `DrawProperties` (:886-955) draws the properties grouped: a cached plan per `ComponentInfo` (a
  `std::unordered_map<const ComponentInfo*, PropertyPlan>` filled on first use: the registry is frozen) lists the
  sections in order of first appearance and their property indices; properties without a section first, without a
  heading. A heading is drawn only when a property of its section is drawn (not hidden, transient or replaced by a
  drawer). Table ids `"Properties"` then `"Properties.<n>"`. Probe `Inspector.<Component>.Section.<Section>`. Prefab
  override menus, inactive reasons and multi-entity editing are unchanged (they are per row).
- **The sun's line**, after the DirectionalLight's properties (beside `DrawReflectionProbeBake`, :770):
  `DrawSunShadowQuality(panelContext)`: "Shadow maps: 4 x 4096 (Graphics Quality: High)" from the Scene view
  renderer's stats (`context.GetViewport().FindRenderer()->GetStats()`, const: step 0's `SceneRendererStats::Quality`
  and `ShadowMapSize`, what its latest frame drew; `ViewportRenderer::GetSceneRenderer()` is not const,
  `ViewportRenderer.h:114`) and the project's preset; while the Scene view chooses its own: "Shadow maps: 4 x 8192 in the
  Scene view (Ultra; the project: High)"; without a Scene view renderer, or before its first shadowed frame
  (`ShadowMapSize` 0), the project's quality with `ResolveShadowMapSize(quality.ShadowResolution, 1080)` and "at
  1080 rows". Then `UI::LinkButton("Inspector.DirectionalLight.ShadowQuality.Open",
  "Graphics Quality", Icons::ExternalLink, "Shadow-map resolution and cascades are the project's: choose them in Project
  Settings")` → `Shell->ShowProjectSettings(std::string(RendererQuality::c_SectionId))` (step 0 declares the id as a `std::string_view`) (the collision layers' link, :1315-1329).
  Probe `Inspector.DirectionalLight.ShadowQuality`.
- **With nothing selected**, after the collision layers' link (:1282): `DrawGraphicsQualityLink`: "Graphics quality: High"
  and the same link (probe `Inspector.GraphicsQuality`), so the overview the settings audit found empty
  (`settings/captures/ui_inspector_nothing_crop.png`) leads to the page.

### D12 The Third-Person Adventure's Graphics row

- **Script** (`Samples/ThirdPerson/Scripts/SettingsMenu.h/.cpp`): fields `Strata::Entity GraphicsLow, GraphicsMedium,
  GraphicsHigh, GraphicsUltra;` and `int32_t GraphicsSaves = 0;`, each with its `ST_SCRIPT_FIELD` line (:110-122).
  `OnUIValueChanged`: for one of the four with `value != 0`: `Graphics::SetPreset(<its preset>)`, `if (Graphics::Save())
  GraphicsSaves++`, the toggle sound; **a value of 0 is ignored**. The four share one toggle group: the player cannot
  switch the one that is on off (`GameUISystem::SetToggle` returns, `GameUISystem.cpp:402-404`), and switching another
  on queues the 0 events of the others *before* the 1 of the new one (:405-413). Acting on a 0 (for instance setting the
  previous preset's toggle back on while it is still the preset in effect) would turn the clicked toggle off again
  (`SetValue(1)` enforces the group, :1134-1137) and leave the menu showing the old preset over the new one.
  `ShowValues`: each toggle's value 1 when `Graphics::GetPreset()` is its preset, else 0 (a Custom preset, or a session
  `--quality` the menu did not choose, shows the preset in effect; Custom turns all four off: a script's `SetValue(0)`
  sets the toggle directly, :1129-1133). The header's comment names the
  row. The README's "Settings" lines name it.
- **Scenes** (`MainMenu.stscene` and `Course.stscene`, both hold the panel): through editor commands only, on a copy, in
  each scene: `ui.create {type: "horizontalLayout", parent: <the panel's list>, name: "GraphicsRow"}`, a `text`
  "Graphics", four `toggle`s named `GraphicsLow` ... `GraphicsUltra` with texts Low, Medium, High, Ultra;
  `component.set UIToggle {Group: <GraphicsRow>, EventTarget: <Game>}` on each; `script.setField SettingsMenu
  Graphics<Preset>` on `Game`; the panel's `UIRect.Size` grown by one row if `ui.layout` shows the Back button pushed
  out; `viewport.capture` of the open panel in both scenes checked by eye. The commands are kept as the stream's record
  in the commit message.
- **Command script** (`StrataTests/Editor/ThirdPersonCommands.json.in`, after the invert-Y check at :68):

  ```json
  {"command": "entity.find", "parameters": {"name": "GraphicsLow"}, "bind": {"graphicsLow": "/entities/0"}},
  {"command": "input.click", "parameters": {"entity": {"$ref": "graphicsLow"}}},
  {"command": "script.get", "parameters": {"entity": {"$ref": "game"}, "class": "SettingsMenu"}, "expect": {"/scripts/0/fields/GraphicsSaves": {"equals": 1}}, "until": {"steps": 10}},
  {"command": "component.get", "parameters": {"entity": {"$ref": "graphicsLow"}, "component": "UIToggle"}, "expect": {"/values/IsOn": {"equals": true}}},
  {"command": "editor.status", "parameters": {"sections": ["game"]}, "expect": {"/game/graphics/effective/Preset": {"equals": "Low"}, "/game/graphics/sources/Preset": {"equals": "player"}}},
  {"command": "graphics.resetPlayerOptions", "expect": {"/reset": {"equals": ["Preset"]}}},
  {"command": "editor.status", "parameters": {"sections": ["game"]}, "expect": {"/game/graphics/sources/Preset": {"equals": "project"}}},
  ```

  (the `entity.find` joins the binds at :26-29). The package test runs it. The `UIToggle` check fails with a handler that
  acts on the group's 0 events (the project's High toggle would be set on again and Low switched off).

## 5. Steps: design, tests, docs, acceptance

Each step's tests are written first and run red against the step's base where they test new behavior (AGENTS.md:
"Check that a new regression test fails without its fix").

### S1 The page, the chip and the row traits

Design: D1 (functions and traits), D9 (without the players' banner, which needs S2/S4), D10.

Tests:

- `Renderer.Quality` (step 0's suite, `StrataTests/src/Renderer/RendererQualityTests.cpp`):
  - "Presets round-trip through Custom": for each named preset P, with `v = section.Validate({"Preset": P}, version)`:
    `FromSectionValue(&MakeCustomStart(*v, model))` has the same knobs as `GetPreset(P)` (`HasSameKnobs`), and
    `ResolveShadowMapSize` of both gives the same size at 720, 1439, 1440, 2160 and 4320 rows. Fails without
    `MakeCustomStart`, and fails if a preset's value cannot be written as fields (what `AutoFine` exists for).
  - "Normalize spells out a named preset and changes nothing it decides": `FromSectionValue(&Normalize(v))` has the same
    knobs as `FromSectionValue(&v)` over every preset and a Custom value; `Normalize(Normalize(v)) == Normalize(v)`;
    under a named preset every following row equals `GetPresetValues()[P][field]`.
  - "Custom keeps a tuning and otherwise starts from the look in effect": stored rows equal to the defaults or to a
    preset's values → the look in effect; one tuned row → all stored rows kept.
  - "Every field but Preset has a row, and every row a field": `GetRowTraits()` covers `MakeSection()`'s fields exactly,
    in field order (a later stream's field without a traits line fails here).
  - "Rows that do not follow the preset are read under every preset": for every row with `FollowsPreset` false and every
    named P, `ToSectionValue(FromSectionValue(&{Preset: P, field: x}))[field] == x` for a non-default x; for every row
    that follows, it equals `GetPresetValues()[P][field]` whatever x is. No step-0 row is of the first kind, so today it
    checks the second half; it fails for a comfort row whose `FromSectionValue` line reads the preset's table.
  - "Renderer.Quality's fields are graphics options": every field is Bool, Int, Float or Enum.
  - "Memory estimates": High at 3840x2160 = 256 MiB of cascades + 64 MiB of atlas; Ultra at 2160 rows = 1 GiB + 256 MiB;
    Medium at 1920x1080 = 64 MiB + 64 MiB (with the formulas of D1).
- `Editor.UI.GraphicsQuality` (new, `StrataTests/src/Editor/EditorGraphicsQualityPageTests.cpp`, the `ImGuiHarness`;
  clicks only once the page shows; shard EditorUI):
  - "The page replaces the generic page and shows a column per preset with the chosen one highlighted" (probe colors:
    the chosen column's cells record `AccentMuted`).
  - "Choosing a preset is one project.setSettings step and keeps the rows' stored values" (undo restores; the scene
    stays saved).
  - "Editing Custom's cell under High switches to Custom from High's values in one step" (`edit.history` gains one
    step; the value equals `MakeCustomStart` with the field).
  - "The memory rows match the estimates" and "The GPU line names the Scene view's stages" (the harness renders no GPU:
    the line says times appear while the Scene view renders).
  - "Rows the presets leave alone appear under Players' choices" (a page made from a test `OptionModel`: a local
    registry with a simple section `Test.Quality`, two rows, one with `FollowsPreset = false`, and four preset values).
  - "Reset to Defaults resets the section".
- `Editor.UI.ProjectSettings` "Every registered section has a page" (`EditorSettingsUITests.cpp:482`) keeps passing (the
  page is registered).
- `Editor.UI` (Scene view): "The quality chip shows the preset in effect and its popup sets the Scene view's quality"
  (probes `Viewport.Quality.*`; `viewport.getSettings` reports the choice; "Graphics Quality..." selects the page in
  Project Settings).

Docs: DG "Settings pages" (the page, its probes, "a custom page for a simple section"), DG "Rendering > Quality
settings" (row traits: `FollowsPreset`, `GPUStage`, `MemoryBytes`; `Normalize`, `MakeCustomStart`).

Acceptance: the page lists every step-0 row (7) with five columns; choosing Ultra is one `edit.history` step, which one
`edit.undo` reverts; the memory row reads 256 MiB + 64 MiB (cascades, atlas) for High at 3840 x 2160 and 64 MiB + 64 MiB
for Medium at 1920 x 1080; zero new validation errors in `GPU*` (no renderer change).

### S2 `IGameGraphics`, the service and the programs

Design: D2, D3, D4, D5.

Tests:

- `Runtime.GameGraphics` (new, `StrataTests/src/Runtime/GameGraphicsTests.cpp`; a local registry with step 0's
  section, a `ProjectSettingsValues` as the project, a `SaveDirectory` in `Tests::CreateTemporaryDirectory()` passed as
  the service's `saves`; cases 2 and 9 build the service over a test `OptionModel`):
  1. "Without choices the value in effect is the project's, spelled out" (High project → `effective == Normalize`).
  2. "A preset choice keeps only the preset and the choices the presets leave alone" (test rows with one
     `FollowsPreset = false` row through a test section).
  3. "Setting a row under a named preset switches to Custom from the look in effect" (Low → set `ShadowCascades` 2:
     `ShadowResolution` is Low's `1024`, sources all `Player`).
  4. "A value the section refuses changes nothing and names the field" (`ShadowCascades` 9; `Preset` "Epic"; a bool
     for an Int).
  5. "Resetting the preset brings the project's quality back; resetting a row under the player's Custom takes the
     project's".
  6. "The command line's preset wins for the session, is its rows' source, and is never written" (a Set under it
     returns true, is reported `CommandLine`, and `SaveOptions` writes the player's choice, not the command line's).
  7. "Saving writes GraphicsOptions.json and loading reads it back" (byte form matches D4; atomic: no temp file left).
  8. "A file that is no options file is ignored with one warning and the project's quality applies" (cases: 64 KiB + 1,
     not JSON, wrong format, newer format, newer section version, another section, no Options, two members naming one
     field; `LogCapture` sees one warning each). A member spelled "preset" is read as `Preset`.
  9. "An older section version's file is upgraded and what the section no longer has is dropped with one warning" (a
     version-1 file with a member no current field has, written against a test section at version 2).
  10. "The version changes exactly when the value in effect, a source or the player's choices change, and is unique
      across services" (a Set under `--quality` changes the player's choices only: the version changes).
  11. "A project change reaches the value in effect while the player's choices stay on top".
  12. "Without a save directory Save fails with the reason and the choices stay".
- `Runtime.GameGraphics.Sweep`: "Every truncation and every single-byte change of a valid options file is read or
  refused without a crash" (`*.Sweep`, runs sampled in the Renderer shard and whole in `StrataTests.Fuzz`).
- `Runtime.Game` (`GameRuntimeTests.cpp`, beside "An exported game loads the players' input bindings ..." at :580):
  - "An exported game loads the players' graphics options before its first script call, and they end with it" (a
    manifest with `Renderer.Quality` High, a saves directory holding `{"Preset": "Low"}`: after `Create`,
    `EngineServices::Get<IGameGraphics>()->GetEffectiveValue()["Preset"] == "Low"`; after destruction the provider is
    gone).
  - "GameRuntimeOptions::Quality fixes the preset for the session without writing the file" (bytes unchanged).
- `GPU.Runtime.GameRenderer` (`GPUGameRendererTests.cpp`): "The game renderer follows the player's options and the
  command line" (a scene with a shadowed sun at 640 x 360, a `GameGraphicsService` provider: player `Low` →
  `GetQuality().HasSameKnobs(GetPreset(Low))` and `GetSceneRenderer().GetShadowMapSize() == 1024` after one `Render`;
  `SetOption("Preset", "Medium")` → 2048 in the next; `SetCommandLinePreset(Low)` → 1024 again; a
  `SetQualityOverride` still wins over all; no provider → the project's (step 0's case). Below 1440 rows High and Ultra
  would allocate 64 and 256 MiB, so the test stays with Low and Medium; ends with `CHECK(gpu.GetNewErrorCount() ==
  0)`). Fails without `FollowQuality`.
- `Editor.GameGraphics` (new, `StrataTests/src/Editor/EditorGameGraphicsTests.cpp`, the editor harness, a project in a
  temporary directory):
  - "Play loads the players' graphics options from .strata/Saves and stop forgets them" (before play the effective
    value is the project's; during play the file's; after stop the project's again).
  - "Project changes while playing reach the game under the player's choices".
- `GPU.Editor.GameGraphics`: "The Game view and captures of the scene camera follow the player's options while playing;
  the Scene view keeps its own" (step 0's `renderStats` quality in `viewport.capture`'s result).
- Step 0's CTest `StrataRuntime.Quality` (label `gpu`, windowed, "Graphics quality: Ultra (--quality)") keeps passing
  with `--quality` routed through the service: no new CTest (a headless one would exit 3 under step 0's rule and take
  the same name). That the players' file stays untouched is `Runtime.Game`'s second case, in-process, where its bytes
  can be compared.

Docs: DG "Rendering > Quality settings" gains "Players' options" (the three layers, `FollowsPreset`, Custom's start,
sources, the file, who provides the service); DG "Project settings" names the players' file beside the bindings file;
Architecture.md: Core (`IGameGraphics`) and Runtime (`GameGraphicsService`) paragraphs.

Acceptance: a player's preset reaches the game's renderer in the next frame it renders (GPU test, one `Render`);
`GraphicsOptions.json` round-trips byte-exactly; a bad file costs one warning and nothing else; no per-frame work but one
version compare (section 8).

### S3 The script API

Design: D6, D7.

Tests:

- API test module: `StrataTests/Scripts/API/GraphicsScripts.cpp` (new): `GraphicsProbe : CheckingScript` with fields
  `Request` (int), `Option` (string), `Value` (string), `IntValue` (int), and results `Count`, `Names` (string, joined
  by `\n`), `PresetChoices`, `Effective` (string), `Source` (int), `Accepted`, `Saved`, `ResetDone`, `InfoStructSize`
  (int: what the engine reported for a struct of the module's size plus 16 bytes, through a raw host call), and
  `SmallInfoRefused` (bool: a struct with `StructSize` 40 is refused). It calls the SDK and, for the struct-size cases,
  the raw `GraphicsDescribeOption`.
- `Scripting.Graphics` (new, `StrataTests/src/Scripting/ScriptGraphicsTests.cpp`, the `Scripting.InputActions`
  pattern: a local registry with the renderer's section, a `ProjectSettingsValues` provider, a `GameGraphicsService`
  provider, a `SaveDirectory` provider):
  1. "Scripts list, describe and read the options by name" (count = the section's field count; names in field order;
     `Preset`'s choices Low, Medium, High, Ultra, Custom in that order: the SDK's `QualityPreset` order).
  2. "Scripts set a preset and a row, and the service follows" (effective value and sources after one update).
  3. "Invalid names, types and values fail, are reported once and change nothing" (`LogCapture`: one warning per
     distinct message, the second identical call silent; among the cases an Int 2048 and an Int 2 for
     `ShadowResolution`, both refused, and a float for `ShadowCascades`).
  4. "The info struct is filled up to the smaller size and says so; a struct smaller than the first version is
     refused" (`InfoStructSize == 80` for a module struct of 96 bytes).
  5. "Reset and Save reach the service; Save without a save directory fails and is reported".
  6. "Without a graphics service every function fails and the count is 0".
- `Scripting.API` "Every host function has a call counter" (`ScriptAPITests.cpp:680`): the last name is
  `GraphicsSaveOptions`. Fails until the list and the struct agree.
- Feature test: `GameFeatures::CheckGraphics()` (in `StrataTests/FeatureTest/Scripts/GameFeatures.cpp`, called from
  `OnCreate` beside `CheckWindow` and `CheckSaves`), every SDK name once: `Graphics::Options` (not empty, `Preset` first
  with five choices), `FindOption`, `Get<std::string>`, `Get<int32_t>`, `Get<float>`, `GetPreset`, `GetPresetName`,
  `SetPreset(QualityPreset::Low)`, `Set<int32_t>("ShadowCascades", 2)` (the preset becomes Custom unless the command
  line fixes it: then the check reads the source and expects `CommandLine`), `Set("ShadowResolution", "2048")` (the
  `const char*` overload), `Set(option, std::string_view)`, `Reset("ShadowCascades")`, `Reset()`, `Save()`; then
  `Reset()` and `Save()` again so the file holds no choice (runners' save directories). No misuse in the feature scripts
  (problems are warnings, which the feature test refuses outside `c_ExpectedLogMessages`).
- `FeatureTests.cpp` "The feature scene plays headless with every script check passing" (:229-275): a
  `GameGraphicsService graphics(EngineServices::Get<IProjectSettings>(), &saves)` and its
  `ScopedProvider<IGameGraphics>` after the saves provider (:243). The editor's and the exported game's runs
  (`Editor.FeatureTest`, `EditorFeatureTests.cpp:69`) provide it themselves (D5).
- `SDKCoverageTests` (existing) fails until every new public name is used.

Docs: DG "Gameplay API" (a bullet "Graphics options"; the host function order line, :1517-1521: "then wave 7's
graphics options, `GraphicsGetOptionCount` to `GraphicsSaveOptions`"; and the in-process sentence, :1515-1516, "Code
that runs a game in-process ... provides both" gains `IGameGraphics`: without it scripts' graphics calls fail and are
reported), DG "ABI rules" (:1541-1543 says `StrataScriptModuleAPI` is the only extensible struct a module writes into
engine memory; add the opposite direction: `StrataScriptGraphicsOptionInfo` is module memory the engine fills, the module
giving its capacity in `StructSize`, the engine writing at most that much and its own filled size back, refusing a
capacity below the first version's 80 bytes; the frozen structs `StrataScriptRaycastHit` and `StrataScriptCharacterState`
stay frozen because they carry no size), Architecture.md's Scripting paragraph (:230-242 lists the input action
functions; the graphics functions follow) and its engine services table (:1170-1171: an `IGameGraphics` row, providers
`GameRuntime` and `EditorContext`, user the script host and `GameRenderer`).

Acceptance: `sizeof(StrataScriptHostAPI)` grows by 7 pointers (56 bytes); `ST_SCRIPT_ABI_VERSION` 1; the feature test,
`Scripting.Graphics` and the API module pass; the NewerSDK module (`StrataTests/Scripts/NewerSDK`) still loads.

### S4 Commands, status and the page's players' banner

Design: D8, D9 item 7.

Tests:

- `Editor.GameGraphics`:
  - "graphics.resetPlayerOptions deletes the players' file and, while playing, the game's choices" (returns `reset`
    sorted; outside play the file is gone; while playing the effective value is the project's from the next frame).
  - "graphics.resetPlayerOptions refuses without a project and changes nothing".
  - "editor.status reports the game's graphics: in play the session's, outside play the file's" (sources lower-case;
    `playerFileProblem` names a bad file; a `SetOption` during play shows in `playerOptions` in the next answer: the
    cache keys on the service's version, D5).
- `Editor.Commands` (the conventions checks: description, complete schema, kind) pass for the new command.
- `Editor.Journal` (`CommandJournalTests.cpp`): the command in `c_SessionCommands`.
- `CLI.McpToolsets` (`StrataTests/src/Editor/EditorMcpToolsetsTests.cpp:26`, shard Network; the settings set at :103)
  gains `graphics.resetPlayerOptions`; core stays 32 tools.
- `Editor.UI.GraphicsQuality`: "The players' banner shows what their options set and its button resets them" (probes
  `PlayerOptions`, `PlayerOptions.Reset`, `Row.Preset.Player`).

Docs: DG "Project settings" (the command beside `inputActions.resetPlayerBindings`, :1133), the editor-automation skill
(the command, `editor.status` `game.graphics`, the Scene view chip's `viewport.setSettings {quality}`).

Acceptance: an agent sets the project's quality with one `project.setSettings`, reads the game's effective options
with one `editor.status {sections: ["game"]}`, and clears the players' choices with one `graphics.resetPlayerOptions`.

### S5 The Third-Person Adventure's Graphics row

Design: D12. Tests: the sample's package test `StrataEditor.ThirdPerson` (label `package`, `StrataTests/CMakeLists.txt:850`)
runs the updated command script, and fails on any warning not in its `ALLOWED_MESSAGES` (:724), so the row may log
none; `Samples.Content` passes (no assets added). The sample's `Picture.png` is unchanged (the menu is not in it).

Acceptance: clicking Low in the pause menu's settings saves once (`GraphicsSaves == 1`) and `editor.status` reports
`Preset: Low` from the player; `graphics.resetPlayerOptions` returns `["Preset"]`.

### S6 Discoverability

Design: D11.

Tests:

- `Reflection` (`StrataTests/src/Scene/ReflectionTests.cpp`): "Section applies to the properties registered after it
  until the next" (a `PropertyListBuilder` list over a test struct: no registry needed; a property before the first
  `Section` has none, and a section may come back after another).
- `Reflection.ComponentSchema`: passes with the regenerated snapshot (the diff limited to two components).
- `Renderer.Components` (`StrataTests/src/Renderer/RendererComponentsTests.cpp:25`): "Every property of DirectionalLight and PostProcess has a description" and "Post
  Process' sections are Tone and color, Exposure, Bloom, Ambient occlusion, Anti-aliasing, Reflections, Lens, in that
  order of first appearance".
- `Editor.Commands`: `component.list` reports `Section` for PostProcess' properties.
- `Editor.UI.Inspector`: "Post Process draws a heading per section" (probes `Inspector.PostProcess.Section.*` in order);
  "The sun shows its shadow maps and links to Graphics Quality" (the link opens Project Settings at
  `Renderer.Quality`: the browser's selection); "With nothing selected the Inspector links to Graphics Quality".
- `Perf.Editor.SelectionFrame100kSelected` (existing, Release): A/B under PerfLock (section 8).

Docs: DG "Inspectors" (sections: `PropertyBuilderBase::Section`, order of first appearance, headings, probes), the
add-component skill (group new properties with `.Section`, write a description with units).

Acceptance: `component.list` shows no DirectionalLight or PostProcess property without a description (6 + 7 fixed);
the Inspector shows 7 headings for Post Process; the sun's line reads "Shadow maps: 4 x 4096 (Graphics Quality: High)" in
a Scene view of 1440 rows or more at High.

### S7 Documentation

- `Docs/Manual/Graphics.md` (new, linked from `Docs/README.md` and `Docs/Manual/Settings.md`): what quality is and is
  not (it never changes the look a scene asks for, only what it costs to draw it; no row lowers the resolution); the
  presets table (plan section 5's rows as they exist when the stream lands, with Medium = 0.6.0 and High the default) and
  the memory at 1080 and 2160 rows; where to set it (the page, the Scene view chip, captures' `quality`, `--quality`);
  players' options (a menu in a few lines of script, `Graphics::Save`, where the file lives, how to reset it in the
  editor); "Why there is no render scale" (the owner's rule: native resolution only, no temporal upscaling; supersampled
  stills are offline and above native).
- `Docs/Manual/Settings.md`: the Graphics Quality page.
- `Docs/Manual/Scripting.md`: a section "Graphics options" after "Input actions and rebinding" (:109-135): the SDK, a
  menu row example, saving.
- DG sections listed per step; `Docs/Architecture.md` (Core, Runtime, Scripting paragraphs).
- Skills: `strata-scripting` (the Graphics API and an options-menu recipe), `strata-make-a-game` (offer a Graphics row in
  settings menus, as the Third-Person sample does), `strata-editor-automation` (D8, D10).
- **AGENTS.md** (one digest bullet, in "Scripting", about 330 characters; step 0 leaves 2,833 code points free,
  `specs/step0.md` section 13):
  "- Players' graphics options go through `IGameGraphics` (`Renderer.Quality`'s fields by name; the project's value,
  then the player's, then `--quality`); in-process game loops provide it too; only `GameGraphicsService` touches
  `GraphicsOptions.json`, and every quality row has a line in `RendererQuality`'s traits table (`FollowsPreset`)."
  Stated in full in DG "Rendering > Quality settings" and "Gameplay API" (AGENTS.md checklist item 5). Measured with
  `Docs.AgentGuide` after merging main (under 40,000 characters).
- **Release notes 0.7.0** (lines for the integrator):
  - "Graphics Quality: Project Settings has a Graphics Quality page (presets Low to Ultra and Custom in one table with
    each row's memory and GPU cost), and the Scene view a quality chip."
  - "Players' graphics options: games offer a Graphics menu through `StrataScript/Graphics.h` (`Graphics::SetPreset`,
    `Graphics::Set`, `Graphics::Save`); the choice is kept in `GraphicsOptions.json` beside the game's saves and loaded
    when the game starts. `StrataRuntime --quality` overrides it for one run. The Third-Person Adventure's settings have
    a Graphics row."
  - "Scripts: seven host functions appended to ABI version 1 (`GraphicsGetOptionCount` to `GraphicsSaveOptions`);
    modules that use them run on 0.6.0 engines without the options (the SDK's calls do nothing there)."
  - "Inspector: Post Process and Directional Light properties are grouped under headings, and every one of them has a
    description with its unit; the sun shows its shadow maps' resolution and links to Graphics Quality."
  - "Agents: `graphics.resetPlayerOptions`; `editor.status` reports the game's graphics options and their sources."
  - Upgrading: "Nothing changes in images from this part. A project's Graphics Quality applies to games it exports;
    players' choices are saved per game."
- `Docs/Roadmap.md`: nothing here (the integrator writes Status); below-the-cut leftovers go to "Later".

### Cut line

### S8 Supersampled stills

`viewport.capture {supersample: 1 | 2}` (default 1):

- The capture renderer renders the view at `2 * width x 2 * height` (refused with `InvalidParameters` when that exceeds
  8192 per side or the device's limit, `Renderer::GetGraphicsDevice().GetInfo().MaxTextureDimension2D`
  (`Renderer/GraphicsDevice.h:42`, filled from `maxImageDimension2D` at `VulkanGraphicsDevice.cpp:709`, read the same way
  at `SceneRenderer.cpp:1928`) with every other parameter as asked
  (quality, camera, debug view, TAA settling: the temporal frames are counted at the larger size), then reads it back
  as now.
- **The filter** runs on a job (`JobSystem::Submit`), the command's deferred poll waiting for it (commands never block a
  frame; the capture command is already deferred, `MayDefer`): new `ED/Editor/CaptureSupersample.h/.cpp`,
  `ReadbackImage DownsampleTwice(const ReadbackImage& image)`: the 8-bit sRGB values decoded to linear; per axis a
  separable 4-tap filter over source pixels `2x-1, 2x, 2x+1, 2x+2` with tent weights 1/8, 3/8, 3/8, 1/8 (clamped at the
  edges), each sample's weight multiplied by `1 / (1 + L)` (L its linear luminance, Rec. 709 weights) and the weights
  renormalized, so a single bright sub-pixel does not ring; re-encoded to 8-bit sRGB with the same triangular dither of
  about one code per channel that `camera-and-tone` puts on the renderer's 8-bit output (a fixed per-pixel hash of the
  output coordinates, so a still is reproducible), then rounded. Averaging 16 weighted samples of a dithered image leaves
  about a quarter of its noise, and rounding without a new dither would bring back the bands that dither removed
  (`clarity/captures/banding_pp_sky_x8.png`); while `camera-and-tone` has not landed, the filter dithers all the same.
- Screen-space UI: the overlay pass draws at the doubled size with the UI's own scaling; constant-pixel text is half
  size in the still (*unverified*; documented as a limit, "stills are for the scene").
- Result: `supersample` echoed; `renderStats` of the large render.
- Tests: `GPU.Editor.Viewport` (existing suite) "A supersampled capture is the filtered large render" (a scene of 1-pixel stripes:
  the 2x capture's mean equals the 1x capture's within 1 code and its edges are smoother: lower max column-to-column
  delta); `Editor.Viewport` (existing, no GPU) the refusals; the filter's unit test, new suite `Editor.CaptureSupersample` (shard Editor) (a constant image
  stays constant within one code and keeps its mean within 0.05 of a code over 64 x 64 pixels; a slow gradient's widest run of one
  code stays under 16 pixels; one white sub-pixel spreads with the weights; edges clamp).
- Docs: the automation skill, `Docs/Manual/Graphics.md`'s "stills" line.

### S9 The showcase budget class

`Samples/Samples.json` entries accept `"Budget": "Showcase"`. `StrataTests/src/Samples/SampleContent.h:15-22`
`SampleBudgets` gains `ShowcaseSourceAssetBytes = 80'000'000` and `ShowcaseTotalBinaryBytes = 85'000'000` (the binary
files of showcase samples, counted apart); every other sample keeps `SourceAssetBytes` 25 MB and the others together keep
`TotalBinaryBytes` 55 MB (the existing check is not loosened: the showcase's bytes are counted against their own limit,
about 140 MB over all samples). `Samples.Content` reads the field (unknown values refused). Tests: `Samples.Content`
"A showcase sample may hold 80 MB of source assets and others 25 MB" (temporary sample directories). DG "Samples".

## 6. Tests that must fail without the change

| Test | Fails without |
| --- | --- |
| `Scripting.API` "Every host function has a call counter" | the seven members and list entries (last name) |
| `Scripting.Graphics` (all) | the host functions and the service |
| `FeatureTest` (host function and SDK coverage) | `CheckGraphics` and the headless provider |
| `Renderer.Quality` "Presets round-trip through Custom", "Every field but Preset has a row", "Rows that do not follow the preset are read under every preset" | `MakeCustomStart`, the traits table; a later comfort row whose `FromSectionValue` line reads the preset's table |
| `Runtime.GameGraphics` 1-12, `.Sweep` | the service and the file |
| `Runtime.Game` graphics cases | `GameRuntime`'s service and `GameRuntimeOptions::Quality` |
| `GPU.Runtime.GameRenderer` graphics case | `GameRenderer::FollowQuality` |
| `Editor.GameGraphics`, `GPU.Editor.GameGraphics` | the editor's service, play hooks, command, status |
| `Editor.UI.GraphicsQuality` | the page |
| `CLI.McpToolsets`, `Editor.Journal` | the toolset prefix, the session list |
| `Reflection` sections, `Renderer.Components` descriptions, `Editor.UI.Inspector` headings and links | D11 |
| The Third-Person command script | the row and the script fields (and a handler acting on the group's 0 events: the Low toggle would end off) |

Every new suite falls under an existing prefix of `StrataTests/CoreShards.json` (`Runtime*` in the Renderer shard,
`Scripting*` in Systems, `Editor.UI*` in EditorUI, `Editor*` in Editor, `GPU*` in `StrataTests.GPU`), so the shards do
not change; `Runtime.GameGraphics.Sweep` also runs whole in `StrataTests.Fuzz` (`FuzzSuites`).

## 7. Documentation and skills (summary)

DG: "Rendering > Quality settings" (row traits, players' options), "Settings pages" (the page), "Project settings"
(the players' file and command), "Gameplay API", "ABI rules", "Adding a host function" (unchanged steps; the
graphics functions named in the order line), "Inspectors" (sections), "Samples" (S9). Manual: `Graphics.md` (new),
`Settings.md`, `Scripting.md`. Architecture.md: Core, Runtime, Scripting. Skills: scripting, make-a-game,
editor-automation, add-component. AGENTS.md: one bullet. Release notes: S7's lines.

## 8. Perf

This stream adds no per-frame rendering work and changes no pass: `GameRenderer` and the Game view compare one
version per frame (`IGameGraphics::GetVersion`, which compares the project's version), and the Inspector draws Post
Process in seven tables instead of one with a cached plan. Medium's and High's costs are step 0's.

- **New metrics:** none. A provisional metric would measure nothing this stream can regress at frame level; the
  costs a player's choice brings are step 0's (`SetQuality`'s rebuild) and are measured by its metrics. The one frame
  path this stream changes, the Inspector's `DrawProperties`, is already measured for its per-component cost:
  `Perf.Editor.SelectionFrame100kSelected` (`StrataTests/src/Editor/EditorPerfTests.cpp:277-300`, budget 11.71 ms,
  `Budgets.json:65`) selects 100,000 bare roots (`Perf::CreateFlatRoots`, `StrataTests/src/Perf/SceneGenerators.cpp`:
  Transform rows, no sections), so it measures the plan lookup every drawn component now pays. The seven tables of one
  Post Process are a single component's rows, with nothing to scale: no metric.
- **A/Bs (information, under `python Tools/PerfLock.py --label graphics-options -- <command>`, Release, interleaved, at
  least 3 pairs against step 0's tip in `build/agent-graphics-options-base`, judged by the ratio of medians):**
  `Perf.Editor.SelectionFrame100kSelected` (expect a ratio within 1.02) and `Perf.Renderer.*` (expect 1.00 within
  noise: no renderer change). Numbers go into the S6 commit message. No campaign, no `PerfStability.py`; the base build
  directory is deleted when the stream lands.

## 9. What the stream shares, and how merges resolve

This stream lands first (plan section 4); the later streams merge main and resolve.

| Shared file | This stream | Later streams | Resolution |
| --- | --- | --- | --- |
| `R/RendererQuality.h/.cpp` | The traits table `c_RowTraits` (a line per step-0 field), `OptionModel`, `GetPresetValues` and four functions | Each stream's knob adds, beside step 0's touch points (`specs/step0.md` 7.2), **one `c_RowTraits` line** (`sun-shadows`: `ShadowFilterQuality` {`GPU.Shadows`}, `ContactShadows` {`GPU.ScreenShadows`}; `light-and-materials`: `AmbientOcclusionQuality` {`GPU.AO`}, `EnvironmentResolution` {`GPU.Environment` or none, memory of the prefiltered cube}; `camera-and-tone`: `MotionBlurQuality` {`GPU.MotionBlur`}, and `AntiAliasing`, `MotionBlur`, `DepthOfField`, `FilmGrain`, `ChromaticAberration` with `FollowsPreset = false`, whose `FromSectionValue` lines read the field under every preset, D1) | Line-level; S1's tests fail for a field without a traits line and for a comfort row read from the preset's table |
| `Renderer.Quality` section version | Unchanged (this stream adds no field) | Each bumps by one | The integrator renumbers when two chose the same (plan 4.1) |
| `R/RendererComponents.cpp` | Descriptions and `.Section` calls in DirectionalLight and PostProcess | New properties go inside their group or are appended after a `.Section` call of their own (D11; `sun-shadows`: Shadows; `camera-and-tone`: Tone and color, Exposure, Bloom, Lens, Motion, Anti-aliasing; `light-and-materials`: Ambient occlusion, Reflections), each with a description. `sun-shadows` rewrites ShadowSoftness' description, `camera-and-tone` Tonemapper's (Filmic) and Vignette's (the lens model) | Line-level; the S6 description test enforces |
| `StrataTests/Architecture/ComponentSchema.json` | Regenerated | Regenerate after merging main | Take main's, regenerate, review the diff |
| `ED/Panels/ViewportPanel.cpp` | `DrawQualityChip`; `DrawChips` takes `EditorPanelContext&` | `sun-shadows` and `light-and-materials` add debug view items to `DrawDebugViewChip` | Different functions (a later stream's chip in `DrawChips` uses `panelContext.Context`) |
| `ED/Editor/EditorViewportCommands.cpp`, `ED/Panels/GameViewPanel.cpp` | A scene-camera capture's default quality and the Game view's quality (D5); two description clauses | Debug views (schema, descriptions, capture results) | Different lines; descriptions merged clause by clause |
| `ED/Editor/EditorCommands.h/.cpp` | `RegisterGraphicsCommands` (declared after .h:202, called after .cpp:49) | none planned (`lighting.applySkyPreset` goes into `EditorLightingCommands.cpp`, registered at .cpp:58) | - |
| `ED/UI/GraphicsQualityPage.*` | The page | Their rows appear without UI work (they follow the traits table); `camera-and-tone`'s comfort rows land under "Players' choices" | None |
| `ED/Panels/InspectorPanel.cpp` | `DrawProperties` grouping, the sun's line, the nothing-selected link | `light-and-materials` adds sky preset buttons in `DrawSky` | Different functions |
| `ED/EditorLayer.cpp` | One `Register` line | none planned | - |
| Script ABI tail | Seven members, the last `GraphicsSaveOptions` | none planned | If another stream appends, it goes after `GraphicsSaveOptions` and updates `ScriptAPITests.cpp:680` |
| Feature test (`Feature.stscene`, feature scripts, `FeatureTest.stproj`) | `GameFeatures.cpp`'s `CheckGraphics`; no scene change | New properties' non-default values; new rows in `FeatureTest.stproj`'s `Renderer.Quality` | Additive; the second lander reruns `StrataTests.FeatureTest` |
| `CommandJournalTests.cpp`, `EditorMcpToolsetsTests.cpp`, `McpToolsets.cpp` | One command, one prefix | `lighting.applySkyPreset` (scene toolset by its prefix) | Additive lists |
| `Docs/DeveloperGuide.md`, `AGENTS.md`, `Docs/Manual/Settings.md` | Sections named above; one bullet | Their own sections and bullets | Different sections; AGENTS.md's room is step 0's |
| `Samples/ThirdPerson` | Settings menu script, two scenes' panels, command script | `sun-shadows`, `camera-and-tone`, `light-and-materials` change its look (other entities); `camera-and-tone` adds a "Motion Blur: On / Off" row to this menu (`Graphics::Set("MotionBlur", ...)`, `specs/camera-and-tone.md` 5.7) | Scene files: commands replayed on main's version if a conflict appears; the menu's rows are separate entities and script fields; pictures are theirs |

## 10. Risks

| Risk | Mitigation |
| --- | --- |
| The ABI additions are permanent | Names for reading and writing, indices only to enumerate; one extensible info struct (capacity in, filled size out, first version's size enforced); no per-row functions; Choice values as names; reviewed here and by the spec's reviewer; layout static asserts on 64-bit |
| Step 0 as built differs from `specs/step0.md` (section 2.2) | The first hour maps names; the "As built" note records them; the semantics stay |
| A preset cannot be written as Custom fields | Settled by step 0's `Auto`/`AutoFine` (former Q1); the round-trip test guards later rows |
| A comfort row is ignored under named presets (its `FromSectionValue` line reads the preset's table) | D1's rule and S1's test "Rows that do not follow the preset are read under every preset", which fails for such a line when `camera-and-tone` merges main |
| A player's choice silently overrides later project changes in the editor (a play session saved Low) | The page's banner and row pills, `editor.status` `playerOptions`, `graphics.resetPlayerOptions`, the Manual |
| A player picks Ultra at 4K on a small GPU (1 GiB of cascades per renderer) | The menu shows presets, not texels; Graphics.md states the memory; Medium stays 0.6.0's; `sun-shadows`' 16-bit cascades (below its cut) halve it; an allocation failure halves the size with one error (step 0's `EnsureShadowMap`, `specs/step0.md` 7.5) |
| A preset change stutters a frame (cascades rebuilt) | Expected in an options menu; documented in the ABI comment and the SDK |
| String lifetime in the info struct misread by modules | Documented ("copy them"); the SDK copies at once; engine strings live in the frozen registry anyway |
| The Inspector's grouping costs frames with 100k selected | Cached plan per component; the A/B of section 8 |
| The feature scripts' saves leave a file in the editor project's `.strata/Saves` | They save an empty choice set last; runners use temporary directories |
| Third-Person's menu shows the wrong preset after a click (group toggles' 0 events come first) | D12 ignores 0 events; the command script checks the Low toggle stays on |
| Comfort rows' meaning (Scene / Off) drifts between streams | `FollowsPreset = false` and the page's "Players' choices" table are the contract; `camera-and-tone` sets it on its rows |

## 11. Open questions and the decisions recommended

- **Q1 Ultra's shadow resolution under Custom.** Settled by step 0 (`specs/step0.md` 7.1, its Q4): `ShadowResolution`
  has `Auto` (High's rule) and `AutoFine` (Ultra's), so every preset is expressible as fields; this stream adds no
  option and no section version. Alternative rejected: starting Custom from the resolution in effect at the current
  output (it loses the switch at 1440 rows when the window changes).
- **Q2 Does a script's `Set` succeed while `--quality` fixes the option?** Yes: kept as the player's choice, saved, and
  reported with source CommandLine for the session; a menu shows what applies.
- **Q3 Automatic saving?** No: `Graphics::Save` is explicit, as `Input::SaveBindings` is (the game decides when a choice
  is final).
- **Q4 Does the Scene view follow the players' options while playing?** No: the Scene view is the person's (its chip);
  the Game view and captures through the scene camera are the game's.
- **Q5 `--quality custom`?** No: the command line names presets only (step 0's values).
- **Q6 Resolution and window options in `IGameGraphics`?** No: they are `IGameWindow`'s (`Game::SetResolution`), and no
  render scale exists (owner's rule).
- **Q7 Choice titles for menus** ("Auto (2048 / 4096)")? Not in the ABI now: choice names are what a menu shows. The
  info struct describes an option, not its choices, so titles per choice would come as a host function appended later
  (`GraphicsGetOptionChoiceTitle`, the text buffer protocol), which the SDK checks like the others; nothing now
  prevents it.
- **Q8 Should the Post Process properties be reordered so sections are contiguous?** No, though files would allow it
  (cooked scenes map properties by name, `Scene/CookedScene.h:35-36`): the snapshot's diff stays limited to tooltips and
  sections, `component.list` keeps its order, and the Inspector draws sections in order of first appearance anyway.

## 12. Left out

- Texture quality (mip bias) and per-light shadow scales: plan section 9, later with game-shell's options.
- A per-option "title of each choice" ABI member (Q7): appendable later.
- An options UI widget in the engine (a ready-made menu): games build menus with the game UI; the Third-Person sample
  is the reference.
- Render scale, dynamic resolution, temporal upscaling: never (owner's rule).

## 13. Deliver in this order (commits)

1. The traits table, `OptionModel`, `GetPresetValues` and the four functions with the `Renderer.Quality` tests.
2. `UI/SettingsFieldProperty.h` (the generic page's field-to-property helper shared), the Graphics Quality page, its
   registration and tests.
3. The Scene view's quality chip.
4. `IGameGraphics`, `GameGraphicsService`, `GraphicsOptionsFile` with `Runtime.GameGraphics` and `.Sweep`.
5. `GameRuntime`, `GameRenderer`, StrataRuntime's `--quality` through the service, with `Runtime.Game` and the GPU case.
6. The editor's play mode, the Game view and captures, with `Editor.GameGraphics` and `GPU.Editor.GameGraphics`.
7. The ABI, host functions, the SDK, the API module, `Scripting.Graphics`, the feature scripts and runners.
8. `graphics.resetPlayerOptions`, `editor.status` `game.graphics`, the MCP prefix, the page's banner.
9. The Third-Person Adventure's row (script, scenes through commands, command script).
10. Property sections (Reflection, Inspector, `component.list`, snapshot), descriptions, the sun's line, the
    nothing-selected link.
11. Manual pages, DG, Architecture.md, skills, AGENTS.md bullet; A/B numbers in the message. Push `ci/wave7-graphics-options`.
12. *Cut line.* Supersampled stills. 13. The showcase budget class.

## 14. First hour

1. `python Tools/PerfLock.py --status`: no build while another agent measures under the lock.
2. Read step 0's `R/RendererQuality.h/.cpp`, `SceneRenderer::SetQuality`, `GameRenderer::SetQualityOverride` and its
   `Render`, `ViewportQuality` and the four `ViewportRenderer::Render` callers, `StartCapture`'s quality, and
   `--quality` in `RuntimeApplication.cpp`; check them against section 2.2 and write the "As built" note at the end of
   this spec (names only; the design does not change).
3. `git worktree add .worktrees/graphics-options -b agent/graphics-options <step 0 tip>`;
   `cmake --preset windows -B build/agent-graphics-options`; a Debug build; run `StrataTests
   --test-suite=Renderer.Quality,Scripting.InputActions,Runtime.Game` as the baseline.
4. Write `Renderer.Quality` "Presets round-trip through Custom", "Every field but Preset has a row" and "Rows that do
   not follow the preset are read under every preset", and `Runtime.GameGraphics` 1-3, red; then the traits table and
   the functions.
5. Draft the ABI block of D6 in `ScriptABI.h` and build once with MSVC to see the static asserts hold. CompatLint runs
   before landing (its compiler paths need this machine's, see the rules at the top).

## Review corrections

The independent review (2026-10-27) checked this spec against bc6ee123, step 0's five commits on `agent/step0`
(`e65353b5..4409b03b`: none touches quality), `specs/step0.md` (written beside this spec, now the API this stream builds
on), `specs/sun-shadows.md`, `specs/camera-and-tone.md`, `specs/light-and-materials.md`, the plan and the settings audit.
Line counts and most file:line claims held. Changed, and why:

1. **Blocker: step 0's API (section 2.2, D1, D3, D5, D9, S1, S2).** The spec assumed a common row type
   (`RendererQualityRow`) to append traits to, `GetPreset(preset, outputRows)` and `Resolve(json, outputRows)`.
   `specs/step0.md` 7.1-7.2 has typed `PresetRow<T>` constants, `GetPreset(preset)`, `FromSectionValue`/`ToSectionValue`,
   unresolved `ShadowMapResolution` values that `SceneRenderer` resolves by its own rows, `HasSameKnobs`,
   `GameRenderer::SetQualityOverride`, and `ViewportQuality` (Project, Low ... Ultra) instead of an optional preset.
   Section 2.2 now states step 0's API; D1 adds its own traits table (`RowTraits`, one line per field), `OptionModel`
   and `GetPresetValues`, and every function, constructor and test uses step 0's names. `ResolveQuality(rows)` became
   `GetQuality()` (renderers resolve Auto themselves).
2. **Major: Q1 is settled by step 0.** Step 0 has `Auto` and `AutoFine` (its Q4). `AutoUltra`, the conditional version
   bump and the "fails until Q1 is settled" test wording were removed; the round-trip test now guards later rows.
3. **Major: comfort rows under named presets (D1, S1, sections 9 and 10).** Step 0's `FromSectionValue` takes every knob
   from the preset's table under a named preset, so `camera-and-tone`'s `MotionBlur`/`AntiAliasing` choices
   (`specs/camera-and-tone.md` 4.2) would be ignored except under Custom. D1 now requires those rows' `FromSectionValue`
   lines to read the field under every preset, and S1 adds a test that fails for a line that does not.
4. **Major: `--quality` routing and the CTest (D5, S2).** Step 0 refuses `--quality` with `--headless` (exit 3) and
   defines a windowed CTest of the same name `StrataRuntime.Quality`; the spec's headless CTest would have failed and
   clashed. It is removed; `--quality` goes through `GameRuntimeOptions::Quality` and step 0's `SetQualityOverride`
   call is removed, the override staying for tests. The "Graphics quality" info line has one owner (step 0's
   `GameRenderer`), with its source from the service; the service no longer logs it, and its `DescribePreset`, which
   nothing else used, is gone.
5. **Major: the Third-Person menu's toggles (D12, S5).** Group toggles queue the others' 0 events before the new one's 1
   (`GameUISystem.cpp:399-414`) and cannot be switched off by a click (:402-404). The spec's "a 0 while it is the preset
   in effect sets it on again" would turn the clicked toggle off (`SetValue(1)` enforces the group, :1134-1137) and show
   the old preset. 0 events are now ignored, and the command script checks the Low toggle stays on.
6. **Major: choices by number in the ABI (D6, D7, S3).** `GraphicsSetOption` took an Int as a choice's position. Choice
   lists change between versions (step 0 put `AutoFine` before the sizes) and `Set<int32_t>("ShadowResolution", 2048)`
   reads as a position. A Choice now takes only its name; indices stay for enumeration. This is permanent ABI meaning,
   so it is fixed before anything ships.
7. **Minor: the service's dependencies (D3, D5).** The constructor's default argument
   `SettingsRegistry::GetProcessRegistry()` needs the complete class, which the header only forward-declared (and
   `SettingsRegistry.h` brings the complete JSON). The registry is now a nullable pointer. The save directory is passed in
   (`IGameSaves*`) rather than looked up, so an in-process `GameRuntime`'s provider cannot redirect the editor's reads.
8. **Minor: versions and caches (D2, D3, D5, S2, S4).** `GetPlayerOptions` is "valid until GetVersion changes", but the
   version moved only with the effective value and sources. A `Set` under `--quality` changes neither, so the version now
   also moves with the player's choices. The editor's cache keys on the service's version, so play-time choices reach
   `editor.status` and the banner.
9. **Minor: the editor details (D5, D10, D11).** `DrawChips` takes `EditorContext&` (`ViewportPanel.cpp:326`) and cannot
   open Project Settings, so it now takes the panel context. The sun's line read `GetSceneRenderer()` through a const
   `FindRenderer()`, but it is non-const (`ViewportRenderer.h:114`), so it now reads step 0's stats. `c_SectionId` is a
   `string_view`, so the calls convert it. The provider order is tied to `m_ProjectSettingsProvider`. The Game view call
   (`GameViewPanel.cpp:122`) and `StartCapture` are named, with the description clauses step 0 wrote.
10. **Minor: file reading (D4).** `Validate` matches names exactly (`SettingsRegistry.cpp:210-213`), so members are
    renamed to the canonical spelling before merging, and two members naming one field refuse the file.
11. **Minor: wrong names and lines.** Scripting may include Core, Input, Asset, Reflection and Scene, not "Core only".
    The MCP suite is `CLI.McpToolsets`. The registration site is `EditorCommands.cpp:49` / `.h:202` (was "unverified").
    `Graphics.h` goes after `Gameplay.h` (alphabetical). Capture tests use the existing `GPU.Editor.Viewport` and
    `Editor.Viewport`. The package test is `StrataEditor.ThirdPerson`. The device limit is
    `GraphicsDeviceInfo::MaxTextureDimension2D` (was "unverified accessor"). RendererComponents' ranges are tightened.
12. **Minor: claims corrected.** `Perf.Editor.SelectionFrame100kSelected` selects bare roots, so it measures the plan
    lookup, not Post Process's tables. Q8's reason changed: cooked scenes map properties by name
    (`CookedScene.h:35-36`). Q7's titles per choice would need an appended function, not a struct member. Ultra's
    allocation failure is handled by step 0 (halving with one error). CompatLint's defaults are the Mac's paths
    (`CompatLint.py:108-112`), and plan 7.1 puts it before landing, not at every commit.
13. **Minor: images (S8).** The supersampled still was re-quantized without dither. Averaging 16 samples of a dithered
    image and rounding would bring back the sky bands `camera-and-tone` removes, so it now dithers like the renderer's
    output. Its tests check a gradient's widest single-code run.
14. **Minor: docs (S3, S7).** DG "Gameplay API"'s in-process sentence and the AGENTS.md bullet now name `IGameGraphics`
    for in-process loops. DG "ABI rules" states the module-memory direction. Architecture.md's engine services table
    gets a row. Later streams append properties after their own `.Section` call and rewrite this stream's descriptions
    that their changes make wrong (ShadowSoftness, Tonemapper, Vignette). The merge table names the files this stream
    now shares (`GameViewPanel.cpp`, `EditorViewportCommands.cpp`, `EditorCommands.*`) and `camera-and-tone`'s
    Motion Blur menu row.

Answers to the author's open questions: Q1, settled by step 0 (`AutoFine`). Step 0's names, mapped from
`specs/step0.md`; the "As built" note records any later rename. Allocation failure, handled by step 0 (7.5). The info
struct pattern (module memory, capacity in, filled size out, refused below 80 bytes) is acceptable: it is the mirror of
`StrataScriptModuleAPI`'s rule, and DG "ABI rules" gets the sentence (S3). The device-limit accessor is resolved above.
The constant-pixel UI in 2x stills stays *unverified* and documented as a limit.

## As built

Step 0 at cc9524a1 against section 2.2 (names only; the design is unchanged):

- `GameRenderer::SetQualityOverride(quality, source)` takes the label its log line names ("--quality", "override"), and
  the quality is applied in a private `ApplyQuality()`, which now calls `FollowQuality(IGameGraphics&)`. The log line's
  sources: "the project's", "the player's", "--quality", or the override's label.
- `RendererQuality::PresetFromString(name, ignoreCase)` has the case flag; `c_HighResolutionRows` (1440) and
  `c_MaxShadowMapSize` exist.
- `ViewportQuality`, `ResolveViewportQuality` and `GetSceneViewQuality` live in `Editor/ViewportRenderer.h`. Step 0 drew
  the scene's camera in the Scene view at the project's quality; `GetSceneViewQuality(view, setting, game)` now returns
  settings and takes the game's quality (`EditorContext::GetGameQuality`) for the scene's camera, so the Scene view's
  "Game camera", the Game view and captures of the scene's camera agree (D5's description clause says so).
- The stream builds in its worktree's own `build/windows` (the wave brief), not `build/agent-graphics-options`.
- The page's preset chips carry signal icons (Low to Ultra) and the sliders icon (Custom), not one icon on the first chip:
  a chip without an icon leaves a gap.
- `GraphicsQualityPage` also records `Header.<Preset>` probes; the players' banner is `UI::Banner` with a
  `UI::ToolbarButton` under it, and "Player" pills mark the fields the players' options set (the preset beside the chips).
- The Scene view's tenth chip needs about 400 units of view as icons: `Editor.UI`'s narrow-strip test uses a 1000-unit
  window (was 900) so that its view still holds every chip.
- The Third-Person Adventure's Graphics row is a two-by-two grid (`gridLayout`, cells of 198 x 40), not one line of four
  (`horizontalLayout`): the sample's font is wide, and "Medium" wrapped in a quarter of the row. The toggles are copies
  of the panel's VSync toggle (`entity.duplicate`), so they keep its look.
