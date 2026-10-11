#include "Editor/AssetEdit.h"

#include "Editor/EditorContext.h"

#include <Strata/Animation/AnimationController.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>

#include <algorithm>

namespace Strata
{

	namespace
	{

		std::span<const uint8_t> AsBytes(const std::string& text)
		{
			return { reinterpret_cast<const uint8_t*>(text.data()), text.size() };
		}

		// The document read as the asset's type loader reads it: the object the manager would publish.
		Ref<Asset> LoadDocument(const EditorAssetManager& assets, AssetHandle handle, std::span<const uint8_t> document, std::string* outError)
		{
			const std::optional<AssetMetadata> metadata = assets.GetMetadata(handle);
			if (!metadata)
			{
				if (outError)
					*outError = fmt::format("Unknown asset {}", handle.ToString());
				return nullptr;
			}
			const AssetLoadFunction* loader = AssetLoaderRegistry::Find(metadata->Type);
			if (!loader)
			{
				if (outError)
					*outError = fmt::format("No loader for asset type {}", AssetTypeToString(metadata->Type));
				return nullptr;
			}
			AssetLoadData data(document);
			std::string error;
			Ref<Asset> asset = (*loader)(*metadata, data, &error);
			if (!asset && outError)
				*outError = fmt::format("Invalid {} data: {}", AssetTypeToString(metadata->Type), error.empty() ? std::string("the loader returned nothing") : error);
			return asset;
		}

		const PropertyInfo* FindMaterialProperty(std::string_view name)
		{
			const std::vector<PropertyInfo>& properties = Material::GetPropertyInfos();
			const auto it = std::find_if(properties.begin(), properties.end(), [name](const PropertyInfo& property) { return property.Name == name; });
			return it != properties.end() ? &*it : nullptr;
		}

	}

	namespace AssetEdit
	{

		bool ReadDocument(const EditorAssetManager& assets, AssetHandle handle, std::vector<uint8_t>& outDocument, std::string* outError)
		{
			auto fail = [outError](std::string message)
			{
				if (outError)
					*outError = std::move(message);
				return false;
			};

			const std::optional<AssetMetadata> metadata = assets.GetMetadata(handle);
			if (!metadata)
				return fail(fmt::format("Unknown asset {}", handle.ToString()));
			if (metadata->IsBuiltin() || metadata->IsSubAsset() || GetNativeAssetExtension(metadata->Type).empty())
				return fail(fmt::format("'{}' is not a document of the project (a material, prefab, scene or animation controller file)", metadata->Name));
			const std::filesystem::path path = assets.GetAbsolutePath(handle);
			if (path.empty())
				return fail(fmt::format("'{}' has no file in the project", metadata->Name));
			std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
			if (!bytes)
				return fail(fmt::format("Could not read '{}'", FileSystem::ToUTF8(path)));
			outDocument = std::move(*bytes);
			return true;
		}

		bool ApplyDocument(EditorAssetManager& assets, AssetHandle handle, std::span<const uint8_t> document, std::string* outError)
		{
			// The object first: a document the loader rejects changes neither the file nor what is loaded.
			Ref<Asset> asset = LoadDocument(assets, handle, document, outError);
			if (!asset)
				return false;
			if (!assets.SaveNativeAsset(handle, document, false, outError))
				return false;
			// An unloaded asset reads the new file when it is next requested.
			if (assets.GetAssetState(handle) == AssetState::Unloaded)
				return true;
			std::string error;
			if (!assets.ReplaceLoadedAsset(handle, asset, &error))
			{
				// The file holds the change: the loaded copy follows it through a reload.
				ST_WARN("Asset {} was saved, but could not be updated at once ({}); it is reloaded", handle.ToString(), error);
				assets.ReloadAsset(handle);
			}
			return true;
		}

		bool PreviewDocument(EditorAssetManager& assets, AssetHandle handle, std::span<const uint8_t> document, std::string* outError)
		{
			Ref<Asset> asset = LoadDocument(assets, handle, document, outError);
			return asset && assets.ReplaceLoadedAsset(handle, asset, outError);
		}

		std::string ListMaterialProperties()
		{
			std::string names;
			for (const PropertyInfo& property : Material::GetPropertyInfos())
				names += (names.empty() ? "" : ", ") + property.Name;
			return names;
		}

		std::optional<std::string> BuildMaterialDocument(const nlohmann::json& base, const nlohmann::json* properties, std::string* outError,
			EditorCommandError* outErrorKind)
		{
			auto fail = [outError, outErrorKind](std::string message) -> std::optional<std::string>
			{
				if (outError)
					*outError = std::move(message);
				if (outErrorKind)
					*outErrorKind = EditorCommandError::InvalidParameters;
				return std::nullopt;
			};

			nlohmann::json document = base;
			if (properties)
			{
				if (!properties->is_object())
					return fail("Material properties must be an object of the form {\"PropertyName\": value}");
				for (const auto& [name, value] : properties->items())
				{
					if (!FindMaterialProperty(name))
						return fail(fmt::format("Unknown material property '{}' (properties: {}; material.get describes them)", name, ListMaterialProperties()));
					document["Material"][name] = value;
				}
			}
			std::string error;
			std::vector<std::string> warnings;
			Ref<Material> material = Material::FromJson(document, &error, &warnings);
			if (!material)
				return fail(error);
			// The loader skips invalid values with a warning; an edit that would lose a value is refused instead.
			if (!warnings.empty())
				return fail(warnings.front());
			return JsonUtils::Dump(material->Serialize(), 1, '\t') + "\n";
		}

		bool SetMaterialProperties(EditorContext& context, AssetHandle material, const nlohmann::json& properties, const MaterialEdit& edit,
			std::string* outError, EditorCommandError* outErrorKind)
		{
			auto fail = [outError, outErrorKind](std::string message, EditorCommandError kind)
			{
				if (outError)
					*outError = std::move(message);
				if (outErrorKind)
					*outErrorKind = kind;
				return false;
			};

			EditorAssetManager* assets = context.GetAssetManager();
			if (!assets)
				return fail("No project is open (project.open or project.create)", EditorCommandError::Failed);
			const std::optional<AssetMetadata> metadata = assets->GetMetadata(material);
			if (!metadata || metadata->Type != AssetType::Material)
				return fail(fmt::format("{} is not a material of the project", material.ToString()), EditorCommandError::InvalidParameters);
			if (metadata->IsBuiltin())
			{
				return fail(fmt::format("'{}' is built in and cannot be changed; make a material of the project (material.create) instead", metadata->Name),
					EditorCommandError::Failed);
			}
			if (metadata->IsSubAsset())
			{
				return fail(fmt::format("'{}' is imported with its model and cannot be changed; make a material of the project (material.create) instead",
					metadata->Name), EditorCommandError::Failed);
			}

			// The loaded material, with what earlier previews changed.
			Ref<Asset> loaded = assets->LoadAssetSync(material);
			if (!loaded || loaded->GetType() != AssetType::Material)
				return fail(fmt::format("Loading the material failed: {}", assets->GetAssetError(material)), EditorCommandError::Failed);
			std::string error;
			EditorCommandError kind = EditorCommandError::Failed;
			const std::optional<std::string> document = BuildMaterialDocument(std::static_pointer_cast<Material>(loaded)->Serialize(), &properties, &error, &kind);
			if (!document)
				return fail(error, kind);

			if (edit.Preview)
			{
				if (!PreviewDocument(*assets, material, AsBytes(*document), &error))
					return fail(error, EditorCommandError::Failed);
				return true;
			}

			std::vector<uint8_t> before;
			if (!ReadDocument(*assets, material, before, &error))
				return fail(error, EditorCommandError::Failed);
			std::vector<uint8_t> after(document->begin(), document->end());
			// Also when the document is unchanged: a preview that returned to the saved values is replaced by the saved material.
			if (!ApplyDocument(*assets, material, after, &error))
				return fail(error, EditorCommandError::Failed);
			if (before == after)
				return true;

			std::string name = fmt::format("Edit Material {}", metadata->Name);
			if (properties.size() == 1)
			{
				if (const PropertyInfo* property = FindMaterialProperty(properties.begin().key()))
					name = fmt::format("Edit {} {}", metadata->Name, property->DisplayName);
			}
			context.GetUndoStack().Record(CreateScope<AssetDocumentAction>(*assets, material, std::move(name), std::move(before), std::move(after), edit.MergeKey));
			return true;
		}

		bool SetMaterialProperty(EditorContext& context, AssetHandle material, const PropertyInfo& property, const PropertyValue& value, bool preview,
			std::string* outError)
		{
			MaterialEdit edit;
			edit.Preview = preview;
			if (!preview)
				edit.MergeKey = fmt::format("Material/{}/{}", material.ToString(), property.Name);
			const nlohmann::json properties = { { property.Name, PropertyValueToJson(property, value) } };
			return SetMaterialProperties(context, material, properties, edit, outError);
		}

		bool IsMaterialSaved(EditorAssetManager& assets, AssetHandle material)
		{
			std::vector<uint8_t> document;
			if (assets.GetAssetState(material) != AssetState::Ready || !ReadDocument(assets, material, document))
				return true; // Nothing loaded differs from the file, or there is no file to differ from
			const std::optional<nlohmann::json> json = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(document.data()), document.size()));
			Ref<Material> saved = json ? Material::FromJson(*json) : nullptr;
			Ref<Asset> loaded = assets.GetAsset(material);
			if (!saved || !loaded || loaded->GetType() != AssetType::Material)
				return true;
			return std::static_pointer_cast<Material>(loaded)->Serialize() == saved->Serialize();
		}

		std::optional<std::string> BuildControllerDocument(const AnimationControllerData& data, std::string* outError)
		{
			Ref<AnimationController> controller = AnimationController::Create(data, outError);
			if (!controller)
				return std::nullopt;
			return JsonUtils::Dump(controller->Serialize(), 1, '\t') + "\n";
		}

		bool SetControllerData(EditorContext& context, AssetHandle controller, const AnimationControllerData& data, const std::string& name,
			const std::string& mergeKey, std::string* outError, EditorCommandError* outErrorKind)
		{
			auto fail = [outError, outErrorKind](std::string message, EditorCommandError kind)
			{
				if (outError)
					*outError = std::move(message);
				if (outErrorKind)
					*outErrorKind = kind;
				return false;
			};

			EditorAssetManager* assets = context.GetAssetManager();
			if (!assets)
				return fail("No project is open (project.open or project.create)", EditorCommandError::Failed);
			const std::optional<AssetMetadata> metadata = assets->GetMetadata(controller);
			if (!metadata || metadata->Type != AssetType::AnimationController)
				return fail(fmt::format("{} is not an animation controller of the project", controller.ToString()), EditorCommandError::InvalidParameters);

			std::string error;
			const std::optional<std::string> document = BuildControllerDocument(data, &error);
			if (!document)
				return fail(fmt::format("Invalid animation controller: {}", error), EditorCommandError::InvalidParameters);
			std::vector<uint8_t> before;
			if (!ReadDocument(*assets, controller, before, &error))
				return fail(error, EditorCommandError::Failed);
			std::vector<uint8_t> after(document->begin(), document->end());
			if (before == after)
				return true;
			if (!ApplyDocument(*assets, controller, after, &error))
				return fail(error, EditorCommandError::Failed);
			context.GetUndoStack().Record(CreateScope<AssetDocumentAction>(*assets, controller, name, std::move(before), std::move(after), mergeKey));
			return true;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetDocumentAction
	////////////////////////////////////////////////////////////////////////////////

	AssetDocumentAction::AssetDocumentAction(EditorAssetManager& assets, AssetHandle handle, std::string name, std::vector<uint8_t> before,
		std::vector<uint8_t> after, std::string mergeKey)
		: m_Assets(assets.weak_from_this()), m_Handle(handle), m_Name(std::move(name)), m_Before(std::move(before)), m_After(std::move(after)),
		  m_MergeKey(std::move(mergeKey))
	{
	}

	bool AssetDocumentAction::Apply(const std::vector<uint8_t>& document, std::string* outError)
	{
		const Ref<AssetManagerBase> manager = m_Assets.lock();
		if (!manager)
		{
			if (outError)
				*outError = "the project this edit belongs to is closed";
			return false;
		}
		// Made from an EditorAssetManager (the constructor's argument).
		return AssetEdit::ApplyDocument(static_cast<EditorAssetManager&>(*manager), m_Handle, document, outError);
	}

	bool AssetDocumentAction::Execute(std::string* outError)
	{
		return Apply(m_After, outError);
	}

	void AssetDocumentAction::Undo()
	{
		std::string error;
		if (!Apply(m_Before, &error))
			ST_ERROR("Undo of '{}' failed: {}", m_Name, error);
	}

	bool AssetDocumentAction::MergeWith(const EditorAction& next)
	{
		const auto* edit = dynamic_cast<const AssetDocumentAction*>(&next);
		if (!edit || m_MergeKey.empty() || edit->m_MergeKey != m_MergeKey || edit->m_Handle != m_Handle)
			return false;
		// Steps of another project's manager (one that replaced it at the same address) never merge.
		if (m_Assets.owner_before(edit->m_Assets) || edit->m_Assets.owner_before(m_Assets))
			return false;
		m_After = edit->m_After;
		return true;
	}

}
