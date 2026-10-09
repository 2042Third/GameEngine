#pragma once

#include "Editor/EditorCommands.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Scene/Entity.h>

#include <nlohmann/json.hpp>

#include <filesystem>
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

		// {"open": false} without a project, else {"open": true, "name", "directory", "assetDirectory", "startScene"}.
		nlohmann::json DescribeProject(const EditorContext& context);

		// Applies component values (as in scene files) to the entity, adding missing components. Validated like the
		// inspector: unknown components or properties, read-only properties, invalid values and references to missing
		// assets or entities fail (values before the failure may have been applied: roll back).
		bool ApplyComponents(Entity entity, const nlohmann::json& components, std::string* outError);

		// A file a command writes for its client (e.g. a capture). Relative paths resolve against the project directory
		// and fail without a project (the editor's working directory means nothing to a client); absolute paths stay.
		// Network (UNC) and device paths, drive-relative paths and reserved device names are refused, the extension must
		// match (".png"), and an existing file is replaced only with `overwrite`; anything but a regular file never is.
		std::optional<std::filesystem::path> ResolveOutputPath(const EditorContext& context, std::string_view path, std::string_view extension, bool overwrite,
			std::string* outError);

	}

}
