#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ScriptEdit.h"
#include "Editor/ScriptProject.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scripting/ScriptSystem.h>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		nlohmann::json DescribeFault(const std::optional<ScriptFault>& fault)
		{
			if (!fault)
				return nullptr;
			return {
				{ "module", fault->ModuleName },
				{ "class", fault->ClassName },
				{ "method", fault->Method },
				{ "entity", fault->Entity.IsValid() ? UUIDToJson(fault->Entity) : nlohmann::json(nullptr) },
				{ "entityName", fault->EntityName },
				{ "description", fault->Description } };
		}

		nlohmann::json DescribeDiagnostic(const ScriptDiagnostic& diagnostic)
		{
			return {
				{ "file", diagnostic.File },
				{ "line", diagnostic.Line },
				{ "column", diagnostic.Column },
				{ "severity", diagnostic.Severity },
				{ "code", diagnostic.Code },
				{ "message", diagnostic.Message } };
		}

		nlohmann::json DescribeBuild(const ScriptBuildResult& result, const ScriptBuildLoad& load)
		{
			nlohmann::json diagnostics = nlohmann::json::array();
			size_t errors = 0;
			size_t warnings = 0;
			for (const ScriptDiagnostic& diagnostic : result.Diagnostics)
			{
				diagnostics.push_back(DescribeDiagnostic(diagnostic));
				(diagnostic.Severity == "error" ? errors : warnings)++;
			}
			const bool loadKnown = load.BuildID == result.ID;
			return {
				{ "build", result.ID },
				{ "success", result.Success },
				{ "error", result.Error },
				{ "seconds", result.Seconds },
				{ "configured", result.Configured },
				{ "module", FileSystem::ToUTF8(result.Module) },
				{ "moduleChanged", result.ModuleChanged },
				{ "loaded", loadKnown && load.Loaded },
				{ "reloaded", loadKnown && load.Reloaded },
				{ "loadError", loadKnown ? load.Error : std::string() },
				{ "errorCount", errors },
				{ "warningCount", warnings },
				{ "diagnostics", std::move(diagnostics) },
				{ "log", result.LogTail } };
		}

		nlohmann::json DescribeScripts(EditorContext& context)
		{
			const Ref<Project>& project = context.GetProject();
			const Ref<ScriptEngine>& engine = context.GetScriptEngine();
			const ScriptBuilder& builder = context.GetScriptBuilder();

			nlohmann::json classes = nlohmann::json::array();
			if (engine)
			{
				for (const ScriptClassInfo& info : engine->GetClasses())
					classes.push_back(ScriptEdit::DescribeClass(info));
			}
			const ScriptBuildResult& last = builder.GetLastResult();
			nlohmann::json status = {
				{ "project", project != nullptr },
				{ "loaded", engine && engine->IsModuleLoaded() },
				{ "module", engine && engine->IsModuleLoaded() ? nlohmann::json(FileSystem::ToUTF8(engine->GetModulePath())) : nlohmann::json(nullptr) },
				{ "moduleName", engine ? engine->GetModuleName() : std::string() },
				{ "loadCount", engine ? engine->GetLoadCount() : 0 },
				{ "hotReload", engine && engine->IsHotReloadEnabled() },
				{ "classes", std::move(classes) },
				{ "fault", engine ? DescribeFault(engine->GetFault()) : nlohmann::json(nullptr) },
				{ "lastFault", DescribeFault(context.GetLastScriptFault()) },
				{ "build", {
					{ "running", builder.IsRunning() },
					{ "phase", ScriptBuildPhaseToString(builder.GetPhase()) },
					{ "current", builder.IsRunning() ? nlohmann::json(builder.GetCurrentID()) : nlohmann::json(nullptr) },
					{ "seconds", builder.GetElapsedSeconds() },
					{ "last", last.ID != 0 ? DescribeBuild(last, context.GetLastScriptBuildLoad()) : nlohmann::json(nullptr) } } } };
			if (project)
			{
				status["sourceDirectory"] = FileSystem::ToUTF8(project->GetScriptSourceDirectory());
				status["projectModule"] = FileSystem::ToUTF8(project->GetScriptModulePath());
				status["projectModuleBuilt"] = FileSystem::IsRegularFile(project->GetScriptModulePath());
			}
			return status;
		}

		// The class of the loaded module, or the failure telling the agent what to do: without a module the editor's state
		// is the problem (Failed), an unknown class is a mistake in the request (InvalidParameters).
		const ScriptClassInfo* RequireClass(EditorContext& context, const std::string& className, EditorCommandResult& outFailure)
		{
			const Ref<ScriptEngine>& engine = context.GetScriptEngine();
			if (!engine || !engine->IsModuleLoaded())
			{
				outFailure = EditorCommandResult::Fail(
					"No script module is loaded, so script classes and fields cannot be checked: build the project's scripts first (script.build)");
				return nullptr;
			}
			if (const ScriptClassInfo* info = engine->FindClass(className))
				return info;
			std::string classes;
			for (const ScriptClassInfo& info : engine->GetClasses())
				classes += (classes.empty() ? "" : ", ") + info.Name;
			outFailure = EditorCommandResult::InvalidParameters(
				fmt::format("The script module has no class '{}' (classes: {})", className, classes.empty() ? std::string("none") : classes));
			return nullptr;
		}

		// The fields of one attached script: the live instance's values while it runs, else the stored overrides over the class
		// defaults. Without a loaded module the class is unknown, so only the stored overrides can be reported.
		nlohmann::json DescribeScriptEntry(const EditorContext& context, Entity entity, const ScriptEntry& entry, const ScriptSystem* system)
		{
			const Ref<ScriptEngine>& engine = context.GetScriptEngine();
			const ScriptClassInfo* info = engine && engine->IsModuleLoaded() ? engine->FindClass(entry.ClassName) : nullptr;
			const bool live = system && system->HasInstance(entity, entry.ClassName);
			nlohmann::json fields = nlohmann::json::object();
			if (info)
			{
				for (const ScriptFieldInfo& field : info->Fields)
				{
					std::optional<PropertyValue> value = live ? system->GetFieldValue(entity, entry.ClassName, field.Name) : std::nullopt;
					if (!value)
					{
						const ScriptFieldValue* stored = entry.FindField(field.Name);
						value = stored && stored->Type == field.Type ? stored->Value : field.DefaultValue;
					}
					fields[field.Name] = ScriptEdit::FieldValueToJson(*value, field.Type);
				}
			}
			else
			{
				for (const ScriptFieldValue& stored : entry.Fields)
					fields[stored.Name] = ScriptEdit::FieldValueToJson(stored.Value, stored.Type);
			}
			return { { "class", entry.ClassName }, { "live", live }, { "known", info != nullptr }, { "fields", std::move(fields) } };
		}

		// Ends a script edit: while playing the change applied to the running copy, which play.stop discards.
		EditorCommandResult FinishScriptEdit(EditorContext& context, nlohmann::json value)
		{
			if (context.IsPlaying())
			{
				value["warning"] = fmt::format("The scene is running ({}): this change applies to the running copy and is discarded by play.stop",
					SceneStateToString(context.GetSceneState()));
			}
			return EditorCommandResult::Ok(std::move(value));
		}

		nlohmann::json DescribeEntry(const ScriptEntry& entry)
		{
			nlohmann::json fields = nlohmann::json::object();
			for (const ScriptFieldValue& field : entry.Fields)
				fields[field.Name] = ScriptEdit::FieldValueToJson(field.Value, field.Type);
			return { { "class", entry.ClassName }, { "fields", std::move(fields) } };
		}

	}

	void RegisterScriptCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "script.status",
			"The project's scripts: the loaded module (path, load count, hot reload), its classes with their fields (name, type, default) and "
			"implemented callbacks, the current crash fault and the last one that stopped play mode, and the running and last build (with "
			"compiler diagnostics).",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribeScripts(context));
			} });

		registry.Register({ "script.get",
			"The field values of the scripts attached to an entity. While the game plays they are the live instances' values (game state kept "
			"in fields, e.g. a score, as it is now; live: true); otherwise the values the scene stores (overrides, else the class defaults). "
			"known: false means the loaded module has no such class, so only stored overrides are listed. Optionally one class.",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "class", StringSchema("Only this script class (default: every script of the entity)") } },
				{ "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const std::string className = arguments.GetString("class", "");
				if (!arguments.IsValid())
					return arguments.Fail();
				const ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
				if (!className.empty() && (!component || !component->FindScript(className)))
					return EditorCommandResult::InvalidParameters(fmt::format("'{}' has no script {}", entity.GetName(), className));

				const ScriptSystem* system = scene.IsRunning() ? scene.GetSystem<ScriptSystem>() : nullptr;
				nlohmann::json scripts = nlohmann::json::array();
				if (component)
				{
					for (const ScriptEntry& entry : component->Scripts)
					{
						if (className.empty() || entry.ClassName == className)
							scripts.push_back(DescribeScriptEntry(context, entity, entry, system));
					}
				}
				return EditorCommandResult::Ok({ { "entity", UUIDToJson(entity.GetUUID()) }, { "scripts", std::move(scripts) } });
			} });

		registry.Register({ "script.build",
			"Builds the project's scripts (the CMakeLists.txt in its Scripts directory) with the engine's compiler and configuration, then loads "
			"the module, or hot-reloads it when it changed (playing scenes keep their script state and get OnReload). One build runs at a time; "
			"a second request fails while one runs. Fails with the first compiler errors; the result and script.status list all diagnostics "
			"(file, line, message) and the end of the build log.",
			ObjectSchema({ { "wait", BoolSchema("Return when the build finished, with its result (default true); false returns at once (poll script.status)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool wait = arguments.GetBool("wait", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				if (!context.BuildScripts(&error))
					return EditorCommandResult::Fail(error);
				const uint64_t id = context.GetScriptBuilder().GetCurrentID();
				if (!wait)
					return EditorCommandResult::Ok({ { "build", id }, { "running", true } });

				return EditorCommandResult::Defer([id](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					const ScriptBuilder& builder = context.GetScriptBuilder();
					if (builder.IsRunning() && builder.GetCurrentID() == id)
						return std::nullopt;
					const ScriptBuildResult& result = builder.GetLastResult();
					if (result.ID != id)
						return EditorCommandResult::Fail(fmt::format("The result of script build {} is no longer available (see script.status)", id));
					if (!result.Success)
						return EditorCommandResult::Fail(result.Error);
					const ScriptBuildLoad& load = context.GetLastScriptBuildLoad();
					if (load.BuildID == id && !load.Loaded)
						return EditorCommandResult::Fail(fmt::format("The scripts were built, but the module could not be loaded: {}", load.Error));
					return EditorCommandResult::Ok(DescribeBuild(result, load));
				});
			} });

		registry.Register({ "script.reload",
			"Loads the script module's file again (hot reload: playing scenes keep their script state and get OnReload), or the project's built "
			"module when none is loaded. Also clears a crash fault.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				if (!context.ReloadScripts(&error))
					return EditorCommandResult::Fail(error);
				const Ref<ScriptEngine>& engine = context.GetScriptEngine();
				return EditorCommandResult::Ok({ { "module", FileSystem::ToUTF8(engine->GetModulePath()) }, { "loadCount", engine->GetLoadCount() },
					{ "classes", engine->GetClasses().size() } });
			} });

		registry.Register({ "script.load",
			"Loads a script module file instead of the project's built one (e.g. a module built outside the editor). Playing scenes switch to it "
			"like on a hot reload. The project's own module is loaded again by script.build.",
			ObjectSchema({ { "path", StringSchema("Module file (.dll/.so/.dylib): absolute, or relative to the project directory") } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string pathText = arguments.GetString("path");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.HasProject())
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				std::filesystem::path path = FileSystem::FromUTF8(pathText);
				if (path.is_relative())
					path = context.GetProject()->GetProjectDirectory() / path;
				if (!FileSystem::IsRegularFile(path))
					return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'path': no file '{}'", FileSystem::ToUTF8(path)));
				std::string error;
				if (!context.LoadScriptModule(path, &error))
					return EditorCommandResult::Fail(error);
				const Ref<ScriptEngine>& engine = context.GetScriptEngine();
				return EditorCommandResult::Ok({ { "module", FileSystem::ToUTF8(engine->GetModulePath()) }, { "loadCount", engine->GetLoadCount() },
					{ "classes", engine->GetClasses().size() } });
			} });

		registry.Register({ "script.init",
			"Creates the project's script build if it is missing: the CMakeLists.txt in its Scripts directory (new projects have it already) and, "
			"with example, an example script (Spinner.cpp). Existing files are kept.",
			ObjectSchema({ { "example", BoolSchema("Also write the example script (default false)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool example = arguments.GetBool("example", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.HasProject())
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				std::vector<std::filesystem::path> created;
				std::string error;
				if (!CreateScriptProjectFiles(*context.GetProject(), example, &created, &error))
					return EditorCommandResult::Fail(error);
				nlohmann::json files = nlohmann::json::array();
				for (const std::filesystem::path& file : created)
					files.push_back(FileSystem::ToUTF8(file));
				return EditorCommandResult::Ok({ { "sourceDirectory", FileSystem::ToUTF8(context.GetProject()->GetScriptSourceDirectory()) }, { "created", std::move(files) } });
			} });

		registry.Register({ "script.add",
			"Attaches a script class of the loaded module to an entity, optionally overriding fields (undoable; while playing it changes the "
			"running copy only and the script starts at once). Needs a loaded module (script.build) to check the class and field types.",
			ObjectSchema({
				{ "entity", EntitySchema("Entity") },
				{ "class", StringSchema("Script class name as registered with ST_SCRIPT_CLASS (see script.status)") },
				{ "fields", AnyObjectSchema("Field overrides by name, e.g. {\"Speed\": 2.5, \"Target\": \"<entity id>\", \"Prefab\": \"Prefabs/Bullet.stprefab\"}") } },
				{ "entity", "class" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const std::string className = arguments.GetString("class");
				const nlohmann::json* fieldsJson = arguments.FindObject("fields");
				if (!arguments.IsValid())
					return arguments.Fail();

				EditorCommandResult failure;
				const ScriptClassInfo* info = RequireClass(context, className, failure);
				if (!info)
					return failure;
				std::string error;
				std::vector<ScriptFieldValue> fields;
				if (fieldsJson)
				{
					for (const auto& [name, valueJson] : fieldsJson->items())
					{
						const ScriptFieldInfo* field = info->FindField(name);
						if (!field)
							return EditorCommandResult::InvalidParameters(fmt::format("{} has no field '{}' (see script.status)", info->Name, name));
						std::optional<PropertyValue> value = ScriptEdit::FieldValueFromJson(valueJson, *field, scene, &error);
						if (!value)
							return EditorCommandResult::InvalidParameters(error);
						fields.push_back(ScriptFieldValue { field->Name, field->Type, std::move(*value) });
					}
				}
				if (!ScriptEdit::AddScript(context, entity, *info, fields, &error))
					return EditorCommandResult::Fail(error);
				return FinishScriptEdit(context, { { "entity", UUIDToJson(entity.GetUUID()) }, { "script", DescribeEntry(*entity.GetComponent<ScriptComponent>().FindScript(info->Name)) } });
			} });

		registry.Register({ "script.remove",
			"Detaches a script from an entity by class name (undoable; while playing it changes the running copy only and the script gets "
			"OnDestroy). Works without a loaded module.",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "class", StringSchema("Script class name") } }, { "entity", "class" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const std::string className = arguments.GetString("class");
				if (!arguments.IsValid())
					return arguments.Fail();
				// The entity has no such script: a mistake in the request.
				std::string error;
				if (!ScriptEdit::RemoveScript(context, entity, className, &error))
					return EditorCommandResult::InvalidParameters(error);
				return FinishScriptEdit(context, { { "entity", UUIDToJson(entity.GetUUID()) }, { "class", className } });
			} });

		registry.Register({ "script.setField",
			"Overrides one field of a script attached to an entity, or resets it to the class default with value null (undoable; while playing it "
			"changes the running copy and the live script at once). The value must fit the field's type (see script.status): bool, integer, "
			"number, [x, y] / [x, y, z] / [x, y, z, w] arrays, quaternions as [x, y, z, w] or Euler degrees [pitch, yaw, roll], text, an entity "
			"ID, or an asset handle or path. Needs a loaded module (script.build) to check the field.",
			ObjectSchema({
				{ "entity", EntitySchema("Entity") },
				{ "class", StringSchema("Script class name") },
				{ "field", StringSchema("Field name") },
				{ "value", { { "description", "New value, or null to use the class default" } } } },
				{ "entity", "class", "field", "value" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const std::string className = arguments.GetString("class");
				const std::string fieldName = arguments.GetString("field");
				// Null is a value here (it resets the field), so presence is checked directly.
				const auto valueIt = parameters.find("value");
				if (valueIt == parameters.end())
					arguments.SetError("Missing parameter 'value' (null resets the field to the class default)");
				if (!arguments.IsValid())
					return arguments.Fail();

				EditorCommandResult failure;
				const ScriptClassInfo* info = RequireClass(context, className, failure);
				if (!info)
					return failure;
				const ScriptFieldInfo* field = info->FindField(fieldName);
				if (!field)
					return EditorCommandResult::InvalidParameters(fmt::format("{} has no field '{}' (see script.status)", info->Name, fieldName));

				std::string error;
				const nlohmann::json& valueJson = *valueIt;
				std::optional<PropertyValue> value;
				if (!valueJson.is_null())
				{
					value = ScriptEdit::FieldValueFromJson(valueJson, *field, scene, &error);
					if (!value)
						return EditorCommandResult::InvalidParameters(error);
				}
				// SetField merges consecutive edits of a field (for inspector drags); every command is an undo step of its own.
				context.GetUndoStack().BreakMerge();
				const bool set = ScriptEdit::SetField(context, entity, info->Name, *field, value, &error);
				context.GetUndoStack().BreakMerge();
				// The entity has no such script: a mistake in the request.
				if (!set)
					return EditorCommandResult::InvalidParameters(error);
				return FinishScriptEdit(context, { { "entity", UUIDToJson(entity.GetUUID()) }, { "script", DescribeEntry(*entity.GetComponent<ScriptComponent>().FindScript(info->Name)) } });
			} });
	}

}
