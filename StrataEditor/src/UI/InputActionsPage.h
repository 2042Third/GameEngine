#pragma once

#include "UI/SettingsPages.h"

#include <Strata/Input/InputActionSet.h>

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace Strata
{

	class EditorContext;

	// The Input Actions page of the Project Settings window (the project section Input.Actions): the maps as a list (the
	// selected one's name, whether it is enabled at start, removing it), the selected map's actions with their name, kind, dead
	// zone and bindings as chips, and a binding added from lists of controls, composites part by part. The UI map's actions are
	// marked as the game UI's navigation and keep their names and kind. Every edit is one inputActions.setMap or
	// inputActions.setAction (one undo step); while the project is not ready for edits (EditorContext::IsWaitingForAssets) an
	// edit is refused with the reason. Probe keys, under "ProjectSettings.Input.Actions.": Summary, Map.<i>, AddMap, MapName,
	// MapEnabled, RemoveMap, AddAction, and per action of the selected map Action.<a>.Name, .Reserved, .Remove, .Kind (and
	// .Kind.<kind> while its list is open), .DeadZone, .Binding.<b>, .RemoveBinding.<b>, .AddBinding; in the binding popup
	// AddBinding.Shape (.Shape.<Control|1D|2D>), AddBinding.Part.<p> (.Part.<p>.<binding text>) and AddBinding.Add; Error (why
	// the last edit was refused).
	class InputActionsPage final : public SettingsPage
	{
	public:
		static constexpr const char* c_ProbePrefix = "ProjectSettings.Input.Actions.";

		void Draw(EditorPanelContext& context, float width) override;
	private:
		// The project's actions as its settings hold them, read again when they change.
		void Refresh(const EditorContext& context);
		void DrawMaps(EditorPanelContext& context);
		void DrawActions(EditorPanelContext& context);
		void DrawBindings(EditorPanelContext& context, uint32_t action, float width);
		void DrawAddBinding(EditorPanelContext& context, uint32_t action);
		// A name's text field: edits a copy while it is typed in and returns the text when the edit ends with a change.
		std::optional<std::string> NameField(const std::string& key, const std::string& name, bool readOnly);
		bool Run(EditorPanelContext& context, const char* command, const nlohmann::json& parameters);
		const InputActionMap* FindSelectedMap() const;
	private:
		InputActionSet m_Set;
		uint64_t m_SettingsVersion = 0; // Of the project's settings m_Set was read from (ProjectSettingsValues::GetVersion)
		bool m_Read = false;
		std::string m_SelectedMap;
		// The field being edited (its probe key), with the text typed or the dead zone dragged so far.
		std::string m_EditKey;
		std::string m_EditText;
		float m_EditDeadZone = 0.0f;
		// The binding being put together in the popup: for which action ("<Map>/<Action>"), its shape and parts (binding texts).
		std::string m_AddAction;
		InputBinding::Shape m_AddShape = InputBinding::Shape::Control;
		std::array<std::string, 4> m_AddParts;
		std::string m_Error; // Why the last edit was refused
	};

}
