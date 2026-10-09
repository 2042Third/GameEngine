#pragma once

#include "Editor/EditorCommands.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Scene/Entity.h>

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	class EditorAssetManager;
	class EditorContext;
	class Scene;

	// Helpers shared by the built-in editor commands.
	namespace CommandUtils
	{

		// JSON Schema of a parameter object: {"type": "object", "properties": {...}, "required": [...]}.
		nlohmann::json ObjectSchema(std::initializer_list<std::pair<const char*, nlohmann::json>> properties, std::vector<std::string> required = {});
		nlohmann::json StringSchema(std::string description);
		nlohmann::json BoolSchema(std::string description);
		nlohmann::json IntegerSchema(std::string description, int64_t minimum, int64_t maximum);
		nlohmann::json NumberSchema(std::string description);
		nlohmann::json Vec3Schema(std::string description);
		nlohmann::json EntitySchema(std::string description);
		nlohmann::json OptionalEntitySchema(std::string description); // Also accepts null
		nlohmann::json EntityArraySchema(std::string description);
		nlohmann::json AssetSchema(std::string description); // Handle or path relative to the asset directory
		nlohmann::json AnyObjectSchema(std::string description);

		struct FoundAsset
		{
			AssetHandle Handle;
			AssetType Type;
		};
		// An asset by reference: a handle (registered or built-in), a path relative to the asset directory, or
		// "Builtin/<Name>". Works without a project for built-in assets.
		std::optional<FoundAsset> FindAsset(std::string_view reference);

		// Resolves a string parameter with FindAsset; it must be of the expected type (None: any type).
		AssetHandle ResolveAsset(EditorContext& context, CommandArguments& arguments, std::string_view name, AssetType expectedType);

		// {"id", "name", "parent", "children": [...], "components": {...}}
		nlohmann::json DescribeEntity(Entity entity);

		// Applies component values (as in scene files) to the entity, adding missing components. Validated like the
		// inspector: unknown components or properties, read-only properties, invalid values and references to missing
		// assets or entities fail (values before the failure may have been applied: roll back).
		bool ApplyComponents(Entity entity, const nlohmann::json& components, std::string* outError);

	}

}
