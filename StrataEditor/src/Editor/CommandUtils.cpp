#include "Editor/CommandUtils.h"

#include "Editor/EditorContext.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

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

		nlohmann::json DescribeProject(const EditorContext& context)
		{
			const Ref<Project>& project = context.GetProject();
			if (!project)
				return { { "open", false } };
			const ProjectConfig& config = project->GetConfig();
			return {
				{ "open", true },
				{ "name", config.Name },
				{ "directory", FileSystem::ToUTF8(project->GetProjectDirectory()) },
				{ "assetDirectory", FileSystem::ToUTF8(project->GetAssetDirectory()) },
				{ "startScene", config.StartScene.IsValid() ? UUIDToJson(config.StartScene) : nlohmann::json(nullptr) } };
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

	}

}
