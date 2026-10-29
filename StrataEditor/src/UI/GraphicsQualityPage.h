#pragma once

#include "UI/SettingsPages.h"

#include <Strata/Core/Base.h>
#include <Strata/Renderer/RendererQuality.h>

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace Strata
{

	class EditorContext;

	// The Graphics Quality page of the Project Settings window (the project section Renderer.Quality): the preset as five
	// chips, then a table with a row per quality row of the renderer that follows the preset and a column per preset, the
	// chosen column highlighted and Custom's editable (under a named preset it shows the values a switch to Custom starts
	// from, RendererQuality::MakeCustomStart), each row's memory at 1920 x 1080 and 3840 x 2160 and the time its GPU stage
	// takes now in the Scene view; the rows the presets leave alone (players' choices, RowTraits::FollowsPreset false) below
	// it. Rows come from the option model, so the rows other changes add appear without UI work. Every edit is one
	// project.setSettings (one undo step) with CommandOrigin { CommandSource::UI }. While the players' graphics options
	// replace the project's in play (EditorContext::GetPlayerGraphicsOptions: the running game's, or the players' file the
	// next session starts with) a banner says what they set, the rows they set carry a "Player" pill, and a button resets
	// them (graphics.resetPlayerOptions). Probe keys, under "ProjectSettings.<section id>.": Preset.<Low|Medium|High|Ultra|
	// Custom>, Header.<Preset>, Row.<Field>, Row.<Field>.Player (Row.Preset.Player beside the chips), Cell.<Field>.<Preset>
	// (the chosen column's record AccentMuted), Custom.<Field>, CustomTooltip (while a Custom cell's tooltip shows: where an
	// edit starts), Choice.<Field>, Memory1080.<Preset>, Memory2160.<Preset>, GPU, PlayerOptions, PlayerOptions.Reset,
	// Problem, Reset.
	class GraphicsQualityPage final : public SettingsPage
	{
	public:
		// The engine's rows and presets over the registered section (RendererQuality::GetOptionModel(section)).
		explicit GraphicsQualityPage(const SettingsSection& section);
		// Tests: a test section with its rows and presets' values (they outlive the page). A model whose section is not the
		// engine's shows no memory rows (its rows' settings are not RendererQualitySettings).
		explicit GraphicsQualityPage(const RendererQuality::OptionModel& model);
		~GraphicsQualityPage() override;

		GraphicsQualityPage(const GraphicsQualityPage&) = delete;
		GraphicsQualityPage& operator=(const GraphicsQualityPage&) = delete;

		void Draw(EditorPanelContext& context, float width) override;
	private:
		// Reads the section again when the project's settings version changed, and formats what the page shows then.
		void Refresh(const EditorContext& context);
		void DrawPresets(EditorPanelContext& context);
		void DrawTable(EditorPanelContext& context, float width);
		void DrawChoices(EditorPanelContext& context, float width); // Rows with FollowsPreset false; nothing when there are none
		void DrawGPU(EditorPanelContext& context, float width);
		// The banner while the players' options replace the project's, read again when the editor's read of them changes.
		void DrawPlayerOptions(EditorPanelContext& context);
		// The "Player" pill of a field the players' options set, after its label.
		void DrawPlayerPill(std::string_view field);
		void Apply(EditorPanelContext& context, const nlohmann::json& parameters); // project.setSettings
	private:
		struct Data;
		Scope<Data> m_Data; // Never null
	};

}
