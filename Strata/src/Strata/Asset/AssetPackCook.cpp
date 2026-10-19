#include "stpch.h"
#include "Strata/Asset/AssetPackCook.h"

#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Core/DocumentOutput.h"
#include "Strata/Scene/CookedScene.h"
#include "Strata/Scene/EntitySink.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneDocumentReader.h"
#include "Strata/Scene/SceneDocumentWriter.h"
#include "Strata/Scene/SceneSerializer.h"

#include <optional>
#include <string_view>

namespace Strata
{

	namespace
	{

		std::optional<SceneDocumentKind> GetEntityDocumentKind(AssetType type)
		{
			switch (type)
			{
				case AssetType::Scene:  return SceneDocumentKind::Scene;
				case AssetType::Prefab: return SceneDocumentKind::Prefab;
				case AssetType::Model:  return SceneDocumentKind::Model;
				default:                return std::nullopt;
			}
		}

		// One read of a document for the pack: its entities into a prototype, their references as Scan finds them, and a
		// scene's name and settings.
		class CookSink final : public EntitySink
		{
		public:
			explicit CookSink(AssetDependencies::ReferenceList& references)
				: m_Builder(nullptr), m_References(references)
			{
			}

			void ReserveEntities(uint64_t count) override { m_Builder.ReserveEntities(count); }

			void ReadField(std::string_view key, const nlohmann::json& value) override
			{
				if (key == "Name")
					m_Name = value.is_string() ? value.get<std::string>() : std::string("Untitled");
				else if (key == "Settings")
					m_Settings = value;
			}

			bool ReadEntity(const nlohmann::json& entity, std::string* outError) override
			{
				AssetDependencies::ScanEntity(entity, m_References);
				return m_Builder.ReadEntity(entity, outError);
			}

			bool Finish(std::string* outError) override { return m_Builder.Finish(outError); }

			Ref<EntityPrototype> TakePrototype() { return m_Builder.TakePrototype(); }
			const std::string& GetName() const { return m_Name; }
			const nlohmann::json& GetSettings() const { return m_Settings; }
		private:
			EntityPrototypeBuilder m_Builder;
			AssetDependencies::ReferenceList& m_References;
			std::string m_Name = "Untitled";
			nlohmann::json m_Settings;
		};

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

	}

	bool CookForPack(const AssetMetadata& metadata, std::vector<uint8_t>& data, const AssetPackCookOptions& options, std::vector<AssetHandle>& outReferences,
		std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		const std::optional<SceneDocumentKind> kind = GetEntityDocumentKind(metadata.Type);
		if (!kind)
			return true;
		const bool adjust = *kind == SceneDocumentKind::Scene && options.AdjustSceneSettings;
		// Kept as it is: only its references are needed.
		if (!options.Cook && !adjust)
			return AssetDependencies::Scan(metadata.Type, data, outReferences, outError);
		if (CookedScene::IsCooked(data))
			return Fail(outError, "it is cooked already, which a project's documents never are");

		AssetDependencies::ReferenceList references(outReferences);
		CookSink sink(references);
		std::string error;
		if (!SceneDocumentReader::Read(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), *kind, sink, {}, nullptr, &error))
			return Fail(outError, error);
		const Ref<EntityPrototype> prototype = sink.TakePrototype();

		SceneSettings settings;
		nlohmann::json settingsJson;
		if (*kind == SceneDocumentKind::Scene)
		{
			if (sink.GetSettings().is_object())
				SceneSerializer::DeserializeSettings(sink.GetSettings(), settings);
			if (adjust)
				options.AdjustSceneSettings(metadata.Handle, settings);
			settingsJson = SceneSerializer::SerializeSettings(settings);
		}

		if (options.Cook)
		{
			std::vector<uint8_t> cooked;
			if (!CookedSceneWriter::Write(*kind, *prototype, sink.GetName(), settingsJson, cooked, &error))
				return Fail(outError, error);
			data = std::move(cooked);
			return true;
		}

		// JSON with the adjusted settings: the scene made again and written in the writer's layout.
		Scene scene(sink.GetName());
		scene.GetSettings() = settings;
		InstantiationOptions instantiation;
		instantiation.KeepIDs = true;
		scene.Instantiate(prototype, instantiation, &error);
		if (!error.empty())
			return Fail(outError, error);
		StringDocumentOutput output;
		SceneWriteOptions writeOptions;
		writeOptions.Priority = JobPriority::Normal;
		if (!SceneDocumentWriter::Write(scene, output, writeOptions, nullptr, &error))
			return Fail(outError, error);
		const std::string text = output.TakeText();
		data.assign(text.begin(), text.end());
		return true;
	}

}
