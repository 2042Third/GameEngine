#pragma once

#include "UI/SettingsPages.h"

#include <Strata/Core/Base.h>
#include <Strata/Renderer/RendererQuality.h>

#include <nlohmann/json_fwd.hpp>

namespace Strata
{

	class EditorContext;

	// The Graphics Quality page of the Project Settings window (the project section Renderer.Quality): the preset as five
	// chips, then a table with a row per quality row of the renderer that follows the preset and a column per preset, the
	// chosen column highlighted and Custom's editable (under a named preset it shows the values a switch to Custom starts
	// from, RendererQuality::MakeCustomStart), each row's memory at 1920 x 1080 and 3840 x 2160 and the time its GPU stage
	// takes now in the Scene view; the rows the presets leave alone (players' choices, RowTraits::FollowsPreset false) below
	// it. Rows come from the option model, so the rows other changes add appear without UI work. Every edit is one
	// project.setSettings (one undo step) with CommandOrigin { CommandSource::UI }. Probe keys, under
	// "ProjectSettings.<section id>.": Preset.<Low|Medium|High|Ultra|Custom>, Header.<Preset>, Row.<Field>,
	// Cell.<Field>.<Preset> (the chosen column's record AccentMuted), Custom.<Field>, Choice.<Field>, Memory1080.<Preset>,
	// Memory2160.<Preset>, GPU, Problem, Reset.
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
		void Apply(EditorPanelContext& context, const nlohmann::json& parameters); // project.setSettings
	private:
		struct Data;
		Scope<Data> m_Data; // Never null
	};

}
