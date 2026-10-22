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
		: m_Context(context), m_Name(std::move(name)), m_ProjectGeneration(context.GetProjectGeneration())
	{
		m_Before.Fields = std::move(before);
		m_After.Fields = std::move(after);
	}

	ProjectSettingsAction::ProjectSettingsAction(EditorContext& context, std::string name, std::string section, nlohmann::json before, nlohmann::json after)
		: m_Context(context), m_Name(std::move(name)), m_ProjectGeneration(context.GetProjectGeneration()), m_Section(std::move(section))
	{
		m_Before.SectionEntry = std::move(before);
		m_After.SectionEntry = std::move(after);
	}

	bool ProjectSettingsAction::Execute(std::string* outError)
	{
		return Apply(m_After, outError);
	}

	bool ProjectSettingsAction::Undo(std::string* outError)
	{
		return Apply(m_Before, outError);
	}

	bool ProjectSettingsAction::IsNoOp() const
	{
		return m_Before.Fields == m_After.Fields && m_Before.SectionEntry == m_After.SectionEntry;
	}

	bool ProjectSettingsAction::Apply(const State& state, std::string* outError)
	{
		// The history goes when the project does (a scene opens with the next one); this guards a step kept elsewhere.
		if (!m_Context.HasProject() || m_Context.GetProjectGeneration() != m_ProjectGeneration)
		{
			if (outError)
				*outError = fmt::format("'{}' changed a project that is no longer open", m_Name);
			return false;
		}
		Project& project = *m_Context.GetProject();
		if (state.Fields)
			state.Fields->ApplyTo(project.GetConfig());
		else if (!project.GetSettings().RestoreStoredEntry(m_Section, state.SectionEntry, m_Context.GetSettingsRegistry(), outError))
			return false;
		m_Context.SaveProjectFile();
		// The scenes take a changed collision layer table now, the running game's too (other sections are read where they apply).
		m_Context.ApplyProjectLayers();
		return true;
	}

	bool ChangeProjectSettings(EditorContext& context, std::string name, ProjectSettingsFields before, ProjectSettingsFields after, std::string* outError)
	{
		if (before == after)
			return true;
		// The project file is written later, on a job: a change it could not be written with (over its bound) is refused now.
		Project changed = *context.GetProject();
		after.ApplyTo(changed.GetConfig());
		if (!changed.MakeFileText(outError))
			return false;
		return context.GetUndoStack().Execute(CreateScope<ProjectSettingsAction>(context, std::move(name), std::move(before), std::move(after)), outError);
	}

	bool ChangeProjectSection(EditorContext& context, std::string name, std::string section, nlohmann::json after, std::string* outError)
	{
		const Project& project = *context.GetProject();
		nlohmann::json before = project.GetSettings().GetStoredEntry(section);
		if (before == after)
			return true;
		Project changed = project;
		if (!changed.GetSettings().RestoreStoredEntry(section, after, context.GetSettingsRegistry(), outError) || !changed.MakeFileText(outError))
			return false;
		return context.GetUndoStack().Execute(CreateScope<ProjectSettingsAction>(context, std::move(name), std::move(section), std::move(before), std::move(after)),
			outError);
	}

}
