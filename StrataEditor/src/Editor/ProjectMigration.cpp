#include "Editor/ProjectMigration.h"

#include <Strata/Asset/AssetRegistration.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Scene/EntitySink.h>
#include <Strata/Scene/SceneDocumentReader.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace Strata::ProjectMigration
{

	namespace
	{

		// Keeps a scene document's settings and ends the read at its first entity once they came (the writer puts them before
		// the entities): only older documents, whose settings follow their entities, are read to the end.
		class SettingsSink final : public EntitySink
		{
		public:
			void ReadField(std::string_view key, const nlohmann::json& value) override
			{
				if (key == "Settings")
				{
					m_Settings = value;
					m_HasSettings = true;
				}
			}

			bool ReadEntity(const nlohmann::json&, std::string* outError) override
			{
				if (!m_HasSettings)
					return true;
				m_Stopped = true;
				if (outError)
					*outError = "stopped at the first entity";
				return false;
			}

			bool Finish(std::string*) override { return true; }

			bool HasSettings() const { return m_HasSettings; }
			bool IsStopped() const { return m_Stopped; }
			const nlohmann::json& GetSettings() const { return m_Settings; }
		private:
			nlohmann::json m_Settings;
			bool m_HasSettings = false;
			bool m_Stopped = false;
		};

		// A layer and its name for messages: "layer 1 'Ball'".
		std::string DescribeName(uint32_t layer, const std::string& name)
		{
			return fmt::format("layer {} '{}'", layer, name);
		}

	}

	SceneLayerScan ScanSceneLayers(const std::filesystem::path& assetDirectory, const std::atomic<bool>* cancel)
	{
		SceneLayerScan scan;
		const AssetDirectoryListing listing = WalkAssetDirectory(assetDirectory, cancel);
		if (!listing.Error.empty())
			scan.Problems.push_back(fmt::format("The asset directory was not read to the end: {}", listing.Error));
		for (const std::filesystem::path& file : listing.Files)
		{
			if (cancel && cancel->load(std::memory_order_relaxed))
				break;
			if (StringUtils::ToLower(FileSystem::ToUTF8(file.extension())) != ".stscene")
				continue;
			const std::string path = FileSystem::ToUTF8(FileSystem::GetRelativePath(file, assetDirectory));
			const std::optional<std::string> text = FileSystem::ReadText(file);
			if (!text)
			{
				scan.Problems.push_back(fmt::format("{}: could not be read", path));
				continue;
			}
			SettingsSink sink;
			SceneReadOptions options;
			options.Parallel = false;
			if (cancel)
				options.IsCancelled = [cancel]() { return cancel->load(std::memory_order_relaxed); };
			SceneReadResult result;
			std::string error;
			if (!SceneDocumentReader::Read(*text, SceneDocumentKind::Scene, sink, options, &result, &error) && !sink.IsStopped())
			{
				if (!result.Cancelled)
					scan.Problems.push_back(fmt::format("{}: {}", path, error));
				continue;
			}
			scan.ScenesRead++;
			if (!sink.HasSettings())
				continue;
			// What is not a valid name or pair was skipped with a warning whenever the scene loaded, and is skipped here too.
			std::optional<PhysicsLayerSettings> layers = SceneSerializer::ReadStoredPhysicsLayers(sink.GetSettings());
			if (!layers)
				continue;
			StoredSceneLayers& stored = scan.Scenes.emplace_back();
			stored.Path = path;
			stored.Layers = std::move(*layers);
			std::filesystem::path metaFile = file;
			metaFile += ".meta";
			if (const AssetMeta meta = ReadAssetMeta(metaFile); meta.Status == AssetMetaStatus::Valid)
				stored.Handle = meta.Handle;
		}
		std::sort(scan.Scenes.begin(), scan.Scenes.end(), [](const StoredSceneLayers& a, const StoredSceneLayers& b) { return a.Path < b.Path; });
		return scan;
	}

	LayerMerge MergeSceneLayers(const std::vector<StoredSceneLayers>& scenes, AssetHandle startScene)
	{
		LayerMerge merge;
		if (scenes.empty())
			return merge;

		// The start scene first, then the others in the order of their paths.
		std::vector<const StoredSceneLayers*> order;
		for (const StoredSceneLayers& scene : scenes)
		{
			if (startScene.IsValid() && scene.Handle == startScene)
				order.insert(order.begin(), &scene);
			else
				order.push_back(&scene);
		}

		PhysicsLayerSettings layers;
		std::array<std::string, c_PhysicsLayerCount> namedBy; // The scene each name came from
		const StoredSceneLayers* matrixFrom = nullptr;       // The first scene whose matrix is not the default
		for (const StoredSceneLayers* scene : order)
		{
			merge.Scenes.push_back(scene->Path);
			for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
			{
				const std::string& name = scene->Layers.Names[layer];
				if (name.empty())
					continue;
				if (!layers.Names[layer].empty())
				{
					if (layers.Names[layer] != name)
					{
						merge.Conflicts.push_back(fmt::format("{} names {}, {} names it '{}': '{}' is kept", namedBy[layer],
							DescribeName(layer, layers.Names[layer]), scene->Path, name, layers.Names[layer]));
					}
					continue;
				}
				if (std::optional<std::string> problem = layers.ValidateName(layer, name))
				{
					merge.Conflicts.push_back(fmt::format("{} names {}, which the project cannot take: {}", scene->Path, DescribeName(layer, name), *problem));
					continue;
				}
				layers.Names[layer] = name;
				namedBy[layer] = scene->Path;
			}
			if (scene->Layers.IsDefaultMatrix())
				continue;
			if (matrixFrom && scene->Layers.CollisionMasks != matrixFrom->Layers.CollisionMasks)
			{
				merge.Conflicts.push_back(fmt::format("{} and {} keep different pairs of layers apart: the project keeps apart every pair either does",
					matrixFrom->Path, scene->Path));
			}
			if (!matrixFrom)
				matrixFrom = scene;
			for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
				layers.CollisionMasks[layer] &= scene->Layers.CollisionMasks[layer];
		}
		merge.Layers = layers;
		return merge;
	}

}
