#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/World/WorldGrid.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// A layer of a world: a grid of cells of one size that stream within one distance of their sources (terrain, props,
	// gameplay volumes...). Cells and WorldCellKey name layers by ID, so that renaming one is cheap.
	struct WorldLayer
	{
		uint8_t ID = 1;   // 1-255, unique in the world
		std::string Name; // Unique, non-empty, without control characters, commas or surrounding spaces (sources list layers by name)
		float CellSize = 256.0f;          // Side of the cells' squares, world units
		float StreamingDistance = 1024.0f; // Cells within this distance of a source load (WorldComponent::DistanceScale scales it)
		bool EnabledByDefault = true;     // Whether the layer streams until a game disables it
		bool LoadBeforeStart = false;     // Whether a game's loads wait for its cells around the sources before the world starts
	};

	// A cell that has content: a scene asset holding its entities.
	struct WorldCell
	{
		uint8_t Layer = 1;
		int32_t X = 0;
		int32_t Z = 0;
		AssetHandle Scene = UUID::Null();

		WorldCellKey GetKey() const { return WorldCellKey { Layer, X, Z }; }
	};

	// What the cook measured of a cell (world.cook): the streamer reserves physics room from it, the cell map and world.info
	// show it.
	struct WorldCellSummary
	{
		uint8_t Layer = 1;
		int32_t X = 0;
		int32_t Z = 0;
		std::string Stamp;      // The cell document's content stamp when it was measured (stale once it differs)
		uint64_t Entities = 0;
		uint64_t Bodies = 0;    // Physics bodies the cell's entities make (PhysicsWorld::CountBodyCandidates)
		glm::vec3 BoundsMin { 0.0f };
		glm::vec3 BoundsMax { 0.0f };
		uint64_t ClosureBytes = 0; // Stored bytes of what loading the cell loads (its dependency closure without other scenes)

		WorldCellKey GetKey() const { return WorldCellKey { Layer, X, Z }; }
	};

	struct WorldIndex
	{
		static constexpr uint32_t c_Version = 1;

		uint32_t Version = c_Version;
		uint64_t MaxBodiesInRadius = 0; // The most bodies the sources can keep loaded anywhere, plus the persistent scene's
		uint64_t PersistentBodies = 0;
		std::vector<WorldCellSummary> Cells;
	};

	// The content of a World document (".stworld"): the persistent scene that holds the world's always-loaded entities, its
	// layers and the cells that have content. Everything a document holds is validated by FromJson; data built in code is
	// validated by Validate (WorldAsset::Create does).
	struct WorldData
	{
		static constexpr uint32_t c_FormatVersion = 1;
		static constexpr uint32_t c_MaxLayers = 32;
		static constexpr uint32_t c_MaxCells = 1u << 20;
		static constexpr size_t c_MaxNameLength = 64;
		static constexpr float c_MinCellSize = 1.0f;
		static constexpr float c_MaxCellSize = 65536.0f;
		static constexpr float c_MaxStreamingDistance = 1'000'000.0f;
		// Every cell lies within this distance of the origin, in X and in Z (1,000 km).
		static constexpr double c_MaxExtent = 1'000'000.0;
		// What float32 positions support at 2-4 mm precision: worlds are supported within 20 km of the origin (world.cook
		// warns about cells beyond it). Origin rebasing is later work.
		static constexpr double c_SupportedExtent = 20'000.0;

		std::string Name;
		AssetHandle PersistentScene = UUID::Null(); // Null until the world has one
		glm::dvec2 GridOrigin { 0.0 };              // X and Z of the corner of every layer's cell (0, 0)
		std::vector<WorldLayer> Layers;
		std::vector<WorldCell> Cells;
		std::optional<WorldIndex> Index;

		// Reads the "World" object of a document; nullopt with the reason when anything is invalid. Unknown keys are ignored.
		static std::optional<WorldData> FromJson(const nlohmann::json& content, std::string* outError = nullptr);
		nlohmann::json ToJson() const;
		// Checks every rule FromJson applies to data built in code: true, or false with the reason.
		bool Validate(std::string* outError = nullptr) const;

		const WorldLayer* FindLayer(uint8_t id) const;
		const WorldLayer* FindLayer(std::string_view name) const;
	};

	class WorldAsset : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::World; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr std::string_view c_DocumentKey = "World";

		static Ref<WorldAsset> Create(WorldData data, std::string* outError = nullptr);
		// A whole document: the header ({"Strata": {"Format": "World", "Version": 1}}) and the "World" object.
		static Ref<WorldAsset> FromJson(const nlohmann::json& document, std::string* outError = nullptr);
		static Ref<WorldAsset> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
		static nlohmann::json ToDocument(const WorldData& data);
		nlohmann::json Serialize() const { return ToDocument(m_Data); }

		AssetMemoryUsage GetMemoryUsage() const override;

		const WorldData& GetData() const { return m_Data; }
		// The index into GetData().Cells of the cell with that key; -1 when the cell has no content.
		int32_t FindCell(const WorldCellKey& key) const;
		// The cook's summary of a cell; null without an index entry for it.
		const WorldCellSummary* FindSummary(const WorldCellKey& key) const;
	private:
		WorldData m_Data;
		std::unordered_map<uint64_t, int32_t> m_CellIndex;    // WorldCellKey::Pack -> index into m_Data.Cells
		std::unordered_map<uint64_t, int32_t> m_SummaryIndex; // WorldCellKey::Pack -> index into m_Data.Index->Cells
	};

}
