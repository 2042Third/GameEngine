#pragma once

#include "Editor/UndoStack.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Project/Project.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Strata
{

	class EditorContext;

	// The project's own fields that undo steps set (ProjectSettingsAction): those the project settings commands change. The
	// others (the name, the directories, the script settings) are never part of a step.
	struct ProjectSettingsFields
	{
		AssetHandle StartScene = UUID::Null();
		ProjectExportSettings Export;
		ProjectGameInfo Game;

		static ProjectSettingsFields Of(const ProjectConfig& config);
		void ApplyTo(ProjectConfig& config) const;
		bool operator==(const ProjectSettingsFields& other) const;
	};

	// One undoable change of the open project's settings: its own fields (project.setStartScene, project.setExportSettings,
	// project.setGameInfo) or one settings section (project.setSettings). Execute and Undo set the project's values and queue
	// a write of the project file (EditorContext::SaveProjectFile, in the background). It changes no scene, so the edited
	// scene stays saved; it goes with the rest of the history when a scene opens.
	class ProjectSettingsAction final : public EditorAction
	{
	public:
		ProjectSettingsAction(EditorContext& context, std::string name, ProjectSettingsFields before, ProjectSettingsFields after);
		// A section's stored entries before and after (ProjectSettingsValues::GetStoredEntry; null: not stored), restored as
		// they were, also a stored value its section refuses.
		ProjectSettingsAction(EditorContext& context, std::string name, std::string section, nlohmann::json before, nlohmann::json after);

		const std::string& GetName() const override { return m_Name; }
		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
		bool IsNoOp() const override;
		bool ChangesScene() const override { return false; }
	private:
		struct State
		{
			std::optional<ProjectSettingsFields> Fields; // A change of the fields, or else of m_Section
			nlohmann::json SectionEntry;
		};

		bool Apply(const State& state, std::string* outError);
	private:
		EditorContext& m_Context;
		std::string m_Name;
		uint64_t m_ProjectGeneration; // The project the step changes (EditorContext::GetProjectGeneration)
		std::string m_Section;        // The section a section's step changes
		State m_Before;
		State m_After;
	};

	// A command's change of the open project's fields: executed and recorded as one undo step (ProjectSettingsAction). A change
	// that changes nothing records nothing. False, with the reason, when the project file could not be written with the change
	// (Project::MakeFileText: over its 1 MiB, checked on a copy before anything changes) or the step could not be executed
	// (nothing changed then).
	bool ChangeProjectSettings(EditorContext& context, std::string name, ProjectSettingsFields before, ProjectSettingsFields after, std::string* outError);
	// The same for one settings section, from its stored entry to `after` (null: reset to its default).
	bool ChangeProjectSection(EditorContext& context, std::string name, std::string section, nlohmann::json after, std::string* outError);

}
