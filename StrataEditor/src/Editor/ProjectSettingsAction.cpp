#include "Editor/ProjectSettingsAction.h"

#include "Editor/EditorContext.h"

#include <utility>

namespace Strata
{

	ProjectSettingsFields ProjectSettingsFields::Of(const ProjectConfig& config)
	{
		ProjectSettingsFields fields;
		fields.StartScene = config.StartScene;
		fields.Export = config.Export;
		return fields;
	}

	void ProjectSettingsFields::ApplyTo(ProjectConfig& config) const
	{
		config.StartScene = StartScene;
		config.Export = Export;
	}

	bool ProjectSettingsFields::operator==(const ProjectSettingsFields& other) const
	{
		return StartScene == other.StartScene && Export.AlwaysInclude == other.Export.AlwaysInclude && Export.LoadingScene == other.Export.LoadingScene;
	}

	ProjectSettingsAction::ProjectSettingsAction(EditorContext& context, std::string name, ProjectSettingsFields before, ProjectSettingsFields after)
		: m_Context(context), m_Name(std::move(name)), m_ProjectGeneration(context.GetProjectGeneration()), m_Before(std::move(before)),
		  m_After(std::move(after))
	{
	}

	bool ProjectSettingsAction::Execute(std::string* outError)
	{
		return Apply(m_After, outError);
	}

	bool ProjectSettingsAction::Undo(std::string* outError)
	{
		return Apply(m_Before, outError);
	}

	bool ProjectSettingsAction::Apply(const ProjectSettingsFields& fields, std::string* outError)
	{
		// The history goes when the project does (a scene opens with the next one); this guards a step kept elsewhere.
		if (!m_Context.HasProject() || m_Context.GetProjectGeneration() != m_ProjectGeneration)
		{
			if (outError)
				*outError = fmt::format("'{}' changed a project that is no longer open", m_Name);
			return false;
		}
		fields.ApplyTo(m_Context.GetProject()->GetConfig());
		m_Context.SaveProjectFile();
		return true;
	}

	bool ChangeProjectSettings(EditorContext& context, std::string name, ProjectSettingsFields before, ProjectSettingsFields after, std::string* outError)
	{
		if (before == after)
			return true;
		return context.GetUndoStack().Execute(CreateScope<ProjectSettingsAction>(context, std::move(name), std::move(before), std::move(after)), outError);
	}

}
