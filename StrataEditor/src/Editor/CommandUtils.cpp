#include "Editor/CommandUtils.h"

#include "Editor/EditorContext.h"
#include "Editor/SceneEdit.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Math/Math.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>
#include <system_error>

namespace Strata
{

	namespace CommandUtils
	{

		nlohmann::json ObjectSchema(std::initializer_list<std::pair<const char*, nlohmann::json>> properties, std::vector<std::string> required)
		{
			nlohmann::json schema = { { "type", "object" }, { "properties", nlohmann::json::object() }, { "additionalProperties", false } };
			for (const auto& [name, property] : properties)
				schema["properties"][name] = property;
			if (!required.empty())
				schema["required"] = required;
			return schema;
		}

		nlohmann::json StringSchema(std::string description)
		{
			return { { "type", "string" }, { "description", std::move(description) } };
		}

		nlohmann::json BoolSchema(std::string description)
		{
			return { { "type", "boolean" }, { "description", std::move(description) } };
		}

		nlohmann::json IntegerSchema(std::string description, int64_t minimum, int64_t maximum)
		{
			return { { "type", "integer" }, { "description", std::move(description) }, { "minimum", minimum }, { "maximum", maximum } };
		}

		nlohmann::json NumberSchema(std::string description)
		{
			return { { "type", "number" }, { "description", std::move(description) } };
		}

		nlohmann::json Vec3Schema(std::string description)
		{
			return { { "type", "array" }, { "items", { { "type", "number" } } }, { "minItems", 3 }, { "maxItems", 3 }, { "description", std::move(description) } };
		}

		nlohmann::json EntitySchema(std::string description)
		{
			return { { "type", "string" }, { "pattern", "^[0-9A-Fa-f]{1,16}$" }, { "description", std::move(description) + " (entity ID)" } };
		}

		nlohmann::json OptionalEntitySchema(std::string description)
		{
			return { { "type", { "string", "null" } }, { "description", std::move(description) + " (entity ID or null)" } };
		}

		nlohmann::json EntityArraySchema(std::string description)
		{
			return { { "type", "array" }, { "items", { { "type", "string" } } }, { "minItems", 1 }, { "description", std::move(description) + " (entity IDs)" } };
		}

		nlohmann::json AssetSchema(std::string description)
		{
			return { { "type", "string" }, { "description", std::move(description) + " (asset handle, or path relative to the asset directory)" } };
		}

		nlohmann::json AnyObjectSchema(std::string description)
		{
			return { { "type", "object" }, { "description", std::move(description) } };
		}

		std::optional<FoundAsset> FindAsset(std::string_view reference)
		{
			const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
			// A handle first; text that only looks like one (a file named "beef") falls through to the path lookup.
			if (std::optional<UUID> parsed = UUID::FromString(reference); parsed && parsed->IsValid())
			{
				if (manager && manager->IsHandleValid(*parsed))
					return FoundAsset { *parsed, manager->GetAssetType(*parsed) };
				for (const BuiltinAssetInfo& builtin : BuiltinAssets::GetAll())
				{
					if (builtin.Handle == *parsed)
						return FoundAsset { builtin.Handle, builtin.Type };
				}
			}
			if (manager)
			{
				const AssetHandle handle = manager->FindAssetByPath(reference);
				if (handle.IsValid())
					return FoundAsset { handle, manager->GetAssetType(handle) };
			}
			for (const BuiltinAssetInfo& builtin : BuiltinAssets::GetAll())
			{
				if (reference == fmt::format("Builtin/{}", builtin.Name))
					return FoundAsset { builtin.Handle, builtin.Type };
			}
			return std::nullopt;
		}

		AssetHandle ResolveAsset(EditorContext&, CommandArguments& arguments, std::string_view name, AssetType expectedType)
		{
			const std::string reference = arguments.GetString(name);
			if (!arguments.IsValid())
				return UUID::Null();

			const std::optional<FoundAsset> asset = FindAsset(reference);
			if (!asset)
			{
				arguments.SetError(fmt::format("Parameter '{}': no asset '{}'", name, reference));
				return UUID::Null();
			}
			if (expectedType != AssetType::None && asset->Type != expectedType)
			{
				arguments.SetError(fmt::format("Parameter '{}': '{}' is a {}, not a {}", name, reference, AssetTypeToString(asset->Type), AssetTypeToString(expectedType)));
				return UUID::Null();
			}
			return asset->Handle;
		}

		nlohmann::json DescribeEntity(Entity entity)
		{
			Entity parent = entity.GetParent();
			nlohmann::json children = nlohmann::json::array();
			for (Entity child : entity.GetChildren())
				children.push_back(UUIDToJson(child.GetUUID()));
			return {
				{ "id", UUIDToJson(entity.GetUUID()) },
				{ "name", entity.GetName() },
				{ "parent", parent ? UUIDToJson(parent.GetUUID()) : nlohmann::json(nullptr) },
				{ "active", entity.IsActive() },
				{ "children", std::move(children) },
				{ "components", ComponentAccess::SerializeEntityComponents(entity) }
			};
		}

		nlohmann::json DescribeWorldTransform(const glm::mat4& world)
		{
			glm::vec3 position(0.0f);
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 scale(1.0f);
			Math::DecomposeTransform(world, position, rotation, scale);
			const glm::vec3 euler = Math::QuatToEulerDegrees(rotation);
			return {
				{ "position", { position.x, position.y, position.z } },
				{ "rotation", { { "euler", { euler.x, euler.y, euler.z } }, { "quaternion", { rotation.x, rotation.y, rotation.z, rotation.w } } } },
				{ "scale", { scale.x, scale.y, scale.z } } };
		}

		nlohmann::json DescribeBounds(const AABB& bounds, bool pending)
		{
			if (!bounds.IsValid())
				return nullptr;
			const glm::vec3 size = bounds.Max - bounds.Min;
			const glm::vec3 center = bounds.GetCenter();
			return {
				{ "min", { bounds.Min.x, bounds.Min.y, bounds.Min.z } },
				{ "max", { bounds.Max.x, bounds.Max.y, bounds.Max.z } },
				{ "size", { size.x, size.y, size.z } },
				{ "center", { center.x, center.y, center.z } },
				{ "pending", pending } };
		}

		std::optional<glm::vec3> ReadVec3(const nlohmann::json& value)
		{
			if (!value.is_array() || value.size() != 3)
				return std::nullopt;
			glm::vec3 result(0.0f);
			for (int axis = 0; axis < 3; axis++)
			{
				if (!value[axis].is_number())
					return std::nullopt;
				result[axis] = value[axis].get<float>();
				if (!std::isfinite(result[axis]))
					return std::nullopt;
			}
			return result;
		}

		nlohmann::json DescribeProject(const EditorContext& context)
		{
			const Ref<Project>& project = context.GetProject();
			if (!project)
				return { { "open", false } };
			const ProjectConfig& config = project->GetConfig();
			return {
				{ "open", true },
				{ "name", config.Name },
				{ "path", FileSystem::ToUTF8(project->GetProjectFile()) },
				{ "directory", FileSystem::ToUTF8(project->GetProjectDirectory()) },
				{ "assetDirectory", FileSystem::ToUTF8(project->GetAssetDirectory()) },
				{ "startScene", config.StartScene.IsValid() ? UUIDToJson(config.StartScene) : nlohmann::json(nullptr) },
				{ "export", { { "alwaysInclude", config.Export.AlwaysInclude } } } };
		}

		nlohmann::json DescribeFrameTimes(std::vector<double> frameTimes)
		{
			std::sort(frameTimes.begin(), frameTimes.end());
			const size_t count = frameTimes.size();
			const double median = count % 2 == 1 ? frameTimes[count / 2] : (frameTimes[count / 2 - 1] + frameTimes[count / 2]) * 0.5;
			const size_t percentileRank = (count * 95 + 99) / 100; // ceil(0.95 * count), at least 1
			double sum = 0.0;
			for (const double frameTime : frameTimes)
				sum += frameTime;
			return {
				{ "count", count },
				{ "medianMs", median },
				{ "meanMs", sum / static_cast<double>(count) },
				{ "p95Ms", frameTimes[percentileRank - 1] },
				{ "maxMs", frameTimes.back() } };
		}

		FrameTimeRecorder::FrameTimeRecorder(uint64_t firstMeasured, size_t expectedFrames)
			: m_FrameTimes(std::make_shared<std::vector<double>>()), m_FirstMeasured(firstMeasured)
		{
			m_FrameTimes->reserve(expectedFrames);
		}

		void FrameTimeRecorder::Update(const EditorContext& context)
		{
			const uint64_t recorded = context.GetRecordedFrameCount();
			if (recorded < m_FirstMeasured || recorded == m_LastRecorded)
				return;
			m_FrameTimes->push_back(context.GetLastFrameTime());
			m_LastRecorded = recorded;
		}

		nlohmann::json FrameTimeRecorder::Describe() const
		{
			return DescribeFrameTimes(*m_FrameTimes);
		}

		nlohmann::json ForceSchema()
		{
			return BoolSchema("Go on although the edited scene has unsaved changes, discarding them (default false: the command fails instead). "
				"Discarded changes are kept as recovery data of the project (scene.recoveryStatus), and the person at the editor is told who "
				"discarded them");
		}

		std::optional<EditorCommandResult> CheckUnsavedChanges(const EditorContext& context, bool force, std::string_view action)
		{
			if (force || !context.IsSceneModified())
				return std::nullopt;
			const std::string& scene = context.GetEditScene()->GetName();
			if (!context.HasProject())
			{
				return EditorCommandResult::Fail(fmt::format("The scene '{}' has unsaved changes, which {} would discard; without a project they cannot be "
					"saved: pass force: true to discard them", scene, action));
			}
			const char* save = context.GetSceneHandle().IsValid() ? "scene.save" : "scene.saveAs, as the scene was never saved";
			return EditorCommandResult::Fail(fmt::format("The scene '{}' has unsaved changes, which {} would discard: save them first ({}) or pass "
				"force: true to discard them (they are kept as recovery data, scene.recoveryStatus)", scene, action, save));
		}

		EditorCommandHandler AfterSaves(EditorCommandHandler handler)
		{
			return [handler = std::move(handler)](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				if (!context.HasSavesInFlight())
					return handler(context, parameters);
				return EditorCommandResult::Defer([handler, parameters = nlohmann::json(parameters)](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					if (context.HasSavesInFlight())
						return std::nullopt;
					return handler(context, parameters);
				});
			};
		}

		nlohmann::json DescribePlayState(const EditorContext& context)
		{
			return { { "state", SceneStateToString(context.GetSceneState()) }, { "paused", context.IsPaused() } };
		}

		nlohmann::json DescribeRunningScene(const EditorContext& context)
		{
			if (!context.IsPlaying())
				return nullptr;
			const EditorAssetManager* assets = context.GetAssetManager();
			auto metadataOf = [assets](AssetHandle asset)
			{
				return asset.IsValid() && assets ? assets->GetMetadata(asset) : std::nullopt;
			};
			Scene& scene = *context.GetActiveScene();
			const AssetHandle asset = context.GetRuntimeSceneAsset();
			const std::optional<AssetMetadata> metadata = metadataOf(asset);
			nlohmann::json subScenes = nlohmann::json::array();
			for (const SubSceneStatus& subScene : scene.GetSubScenes())
			{
				const std::optional<AssetMetadata> subSceneMetadata = metadataOf(subScene.Scene);
				subScenes.push_back({
					{ "id", subScene.ID },
					{ "scene", subScene.Scene.IsValid() ? UUIDToJson(subScene.Scene) : nlohmann::json(nullptr) },
					{ "name", subSceneMetadata ? nlohmann::json(subSceneMetadata->Name) : nlohmann::json(nullptr) },
					{ "state", SubSceneStateToString(subScene.State) },
					{ "entities", subScene.CreatedEntities } });
			}
			nlohmann::json switching = nullptr;
			if (context.IsRuntimeSceneLoading())
				switching = { { "scene", UUIDToJson(context.GetRuntimeSceneLoadTarget()) }, { "progress", context.GetRuntimeSceneLoadProgress() } };
			return {
				{ "scene", asset.IsValid() ? UUIDToJson(asset) : nlohmann::json(nullptr) },
				{ "name", scene.GetName() },
				{ "path", metadata ? nlohmann::json(metadata->Path) : nlohmann::json(nullptr) },
				{ "entityCount", scene.GetEntityCount() },
				{ "subScenes", std::move(subScenes) },
				{ "switching", std::move(switching) } };
		}

		std::string GetRunningCopyWarning(const EditorContext& context)
		{
			return fmt::format("The scene is running ({}): this change applies to the running copy and is discarded by play.stop",
				SceneStateToString(context.GetSceneState()));
		}

		EditorCommandResult FinishEdit(EditorContext& context, SceneEditTransaction& transaction, nlohmann::json value)
		{
			if (std::string error; !context.CheckSceneEditable(&error))
			{
				transaction.Rollback();
				return EditorCommandResult::Fail(std::move(error));
			}
			context.CommitEdit(transaction);
			if (!context.IsPlaying())
				return EditorCommandResult::Ok(std::move(value));
			if (!value.is_object())
				value = nlohmann::json::object();
			value["warning"] = GetRunningCopyWarning(context);
			EditorCommandResult result = EditorCommandResult::Ok(std::move(value));
			result.RunningCopy = true;
			return result;
		}

		EditorCommandResult RollBackEdit(SceneEditTransaction& transaction, std::string error, EditorCommandError kind)
		{
			transaction.Rollback();
			return EditorCommandResult::Fail(std::move(error), kind);
		}

		std::optional<std::filesystem::path> ResolveOutputPath(const EditorContext& context, std::string_view path, std::string_view extension, bool overwrite,
			std::string* outError)
		{
			auto fail = [outError](std::string message) -> std::optional<std::filesystem::path>
			{
				if (outError)
					*outError = std::move(message);
				return std::nullopt;
			};

			if (path.empty())
				return fail("the path is empty");
			// "\\server\share", "//server" and the "\\?\" and "\\.\" device namespaces all start with two separators.
			auto isSeparator = [](char character) { return character == '/' || character == '\\'; };
			if (path.size() >= 2 && isSeparator(path[0]) && isSeparator(path[1]))
				return fail("network and device paths are not allowed");
			const std::filesystem::path file = FileSystem::FromUTF8(path);
			// Windows "C:file" (relative to the drive's current folder) and "\file" (on the current drive) are neither.
			if (!file.is_absolute() && (file.has_root_name() || file.has_root_directory()))
				return fail("the path must be relative or fully absolute (with its drive on Windows)");
			if (!StringUtils::EqualsIgnoreCase(FileSystem::ToUTF8(file.extension()), extension))
				return fail(fmt::format("the path must name a {} file", extension));
			// Windows opens devices for these names in any folder and with any extension.
			for (const std::filesystem::path& part : file.relative_path())
			{
				std::string name = StringUtils::ToLower(FileSystem::ToUTF8(part));
				name = name.substr(0, name.find('.'));
				const bool numbered = name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '0' && name[3] <= '9';
				if (name == "con" || name == "prn" || name == "aux" || name == "nul" || name == "conin$" || name == "conout$" || numbered)
					return fail(fmt::format("'{}' is a reserved device name", FileSystem::ToUTF8(part)));
			}

			std::filesystem::path resolved = file;
			if (file.is_relative())
			{
				if (!context.HasProject())
					return fail("relative paths are relative to the project directory, and no project is open (pass an absolute path)");
				resolved = context.GetProject()->GetProjectDirectory() / file;
			}
			resolved = resolved.lexically_normal();

			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(resolved, error);
			if (!error && std::filesystem::exists(status))
			{
				if (!std::filesystem::is_regular_file(status))
					return fail(fmt::format("'{}' exists and is not a regular file", FileSystem::ToUTF8(resolved)));
				if (!overwrite)
					return fail(fmt::format("'{}' already exists (pass overwrite: true to replace it)", FileSystem::ToUTF8(resolved)));
			}
			return resolved;
		}

		bool ApplyComponents(Entity entity, const nlohmann::json& components, std::string* outError)
		{
			auto fail = [outError](std::string message)
			{
				if (outError)
					*outError = std::move(message);
				return false;
			};

			if (!components.is_object())
				return fail("Components must be an object of the form {\"ComponentName\": {\"Property\": value}}");
			entt::registry& registry = entity.GetScene()->GetRegistry();
			for (const auto& [name, values] : components.items())
			{
				const ComponentInfo* info = ComponentRegistry::Find(name);
				if (!info || info->IsHidden() || HasFlag(info->Flags, ComponentFlags::NoSerialize))
					return fail(fmt::format("Unknown component '{}' (see component.list)", name));
				if (!values.is_object())
					return fail(fmt::format("{}: the values must be an object of property values", name));

				// Validated like the inspector: no read-only or runtime properties; references must point at existing
				// assets of the right type (handles or paths) and at entities of the scene. Null clears a reference.
				nlohmann::json resolved = nlohmann::json::object();
				for (const auto& [key, value] : values.items())
				{
					const PropertyInfo* property = info->FindProperty(key);
					if (!property)
					{
						// Data that is not a property (e.g. script fields) is checked by the component's own deserializer.
						if (!info->DeserializeExtra)
							return fail(fmt::format("{}: unknown property '{}' (see component.list)", name, key));
						resolved[key] = value;
						continue;
					}
					if (property->IsReadOnly() || property->IsTransient())
						return fail(fmt::format("{}.{} cannot be set: it is {}", name, property->Name, property->IsReadOnly() ? "read-only" : "runtime state"));

					nlohmann::json converted = value;
					const std::optional<UUID> reference = value.is_null() ? std::optional<UUID>(UUID::Null()) : UUIDFromJson(value);
					const bool clearsReference = reference && !reference->IsValid();
					if (property->Type == PropertyType::Asset && !clearsReference)
					{
						const std::optional<FoundAsset> asset = value.is_string() ? FindAsset(value.get<std::string>())
							: (reference ? FindAsset(reference->ToString()) : std::nullopt);
						if (!asset)
							return fail(fmt::format("{}.{}: no asset {}", name, property->Name, value.dump()));
						if (property->AssetFilter != AssetType::None && asset->Type != property->AssetFilter)
						{
							return fail(fmt::format("{}.{}: {} is a {}, not a {}", name, property->Name, value.dump(), AssetTypeToString(asset->Type),
								AssetTypeToString(property->AssetFilter)));
						}
						converted = UUIDToJson(asset->Handle);
					}
					else if (property->Type == PropertyType::Entity && !clearsReference)
					{
						if (!reference || !entity.GetScene()->GetEntityByUUID(*reference))
							return fail(fmt::format("{}.{}: no entity {} in the scene", name, property->Name, value.dump()));
					}
					else if (HasFlag(property->Flags, PropertyFlags::PhysicsLayer) && value.is_string())
					{
						// Collision layers can be given by the names the scene's settings give them.
						const PhysicsLayerSettings& layers = entity.GetScene()->GetSettings().PhysicsLayers;
						const std::optional<uint32_t> layer = layers.FindLayer(value.get<std::string>());
						if (!layer)
							return fail(fmt::format("{}.{}: the scene has no layer named {} (scene.info lists them)", name, property->Name, value.dump()));
						converted = *layer;
					}
					// Commands refuse values out of a property's range, where the inspector clamps as it drags: an agent learns its
					// value was not taken instead of finding another one set.
					if (property->HasRange())
					{
						std::string rangeError;
						if (const std::optional<PropertyValue> parsed = PropertyValueFromJson(*property, converted); parsed && !property->IsInRange(*parsed, &rangeError))
							return fail(fmt::format("{}.{} {}", name, property->Name, rangeError));
					}
					resolved[property->Name] = std::move(converted);
				}

				std::string error;
				if (!ComponentAccess::AddComponent(entity, *info, &error))
					return fail(fmt::format("{}: {}", name, error));
				if (!ComponentAccess::Deserialize(*info, info->Get(registry, entity.GetHandle()), resolved, true, &error))
					return fail(error);
				info->MarkModified(registry, entity.GetHandle());
			}
			return true;
		}

		std::string MakeUniqueAssetPath(const EditorAssetManager& assets, std::string_view folder, std::string_view name, std::string_view extension,
			std::string_view fallback)
		{
			// What file names cannot hold becomes '_'; spaces around the name and dots at its end go.
			std::string fileName;
			for (char character : name)
			{
				const auto value = static_cast<unsigned char>(character);
				fileName.push_back(value < 0x20 || value == 0x7F || std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos ? '_' : character);
			}
			while (!fileName.empty() && fileName.front() == ' ')
				fileName.erase(fileName.begin());
			while (!fileName.empty() && (fileName.back() == ' ' || fileName.back() == '.'))
				fileName.pop_back();
			// Long names are cut at a character boundary, leaving room for the extension and a number.
			constexpr size_t c_MaxNameBytes = 96;
			if (fileName.size() > c_MaxNameBytes)
			{
				size_t size = c_MaxNameBytes;
				while (size > 0 && (static_cast<unsigned char>(fileName[size]) & 0xC0) == 0x80)
					size--;
				fileName.resize(size);
				while (!fileName.empty() && (fileName.back() == ' ' || fileName.back() == '.'))
					fileName.pop_back();
			}
			if (!Project::IsValidName(fileName))
				fileName = std::string(fallback);
			const std::string prefix = folder.empty() ? std::string() : std::string(folder) + "/";
			const std::filesystem::path unique = FileSystem::GetUniquePath(assets.GetAssetDirectory() / FileSystem::FromUTF8(prefix + fileName + std::string(extension)));
			return prefix + FileSystem::ToUTF8(unique.filename());
		}

	}

}
