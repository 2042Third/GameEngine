#pragma once

#include "Editor/UndoStack.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Project/Project.h>

#include <cstdint>
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

		static ProjectSettingsFields Of(const ProjectConfig& config);
		void ApplyTo(ProjectConfig& config) const;
		bool operator==(const ProjectSettingsFields& other) const;
	};

	// One undoable change of the open project's settings (project.setStartScene, project.setExportSettings): Execute and Undo
	// set the project's values and queue a write of the project file (EditorContext::SaveProjectFile, in the background). It
	// changes no scene, so the edited scene stays saved; it goes with the rest of the history when a scene opens.
	class ProjectSettingsAction final : public EditorAction
	{
	public:
		ProjectSettingsAction(EditorContext& context, std::string name, ProjectSettingsFields before, ProjectSettingsFields after);

		const std::string& GetName() const override { return m_Name; }
		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
		bool IsNoOp() const override { return m_Before == m_After; }
		bool ChangesScene() const override { return false; }
	private:
		bool Apply(const ProjectSettingsFields& fields, std::string* outError);
	private:
		EditorContext& m_Context;
		std::string m_Name;
		uint64_t m_ProjectGeneration; // The project the step changes (EditorContext::GetProjectGeneration)
		ProjectSettingsFields m_Before;
		ProjectSettingsFields m_After;
	};

	// A command's change of the open project's fields: executed and recorded as one undo step (ProjectSettingsAction). A change
	// that changes nothing records nothing. False, with the reason, when the step could not be executed (nothing changed then).
	bool ChangeProjectSettings(EditorContext& context, std::string name, ProjectSettingsFields before, ProjectSettingsFields after, std::string* outError);

}
