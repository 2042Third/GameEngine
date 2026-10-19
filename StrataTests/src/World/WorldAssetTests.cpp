#include <doctest/doctest.h>

#include "FeatureTest/FeatureTestUtils.h"
#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/World/WorldAsset.h"
#include "Strata/World/WorldGrid.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr AssetHandle c_Persistent = UUID(0xA11CE);

	// Two layers, three cells and an index entry: every kind of field set to a value other than its default.
	WorldData MakeWorld()
	{
		WorldData data;
		data.Name = "Island";
		data.PersistentScene = c_Persistent;
		data.GridOrigin = glm::dvec2(-128.0, 64.0);
		data.Layers.push_back(WorldLayer { 1, "Terrain", 256.0f, 1024.0f, true, true });
		data.Layers.push_back(WorldLayer { 7, "Props", 64.0f, 200.0f, false, false });
		data.Cells.push_back(WorldCell { 1, 0, -1, UUID(0xC0) });
		data.Cells.push_back(WorldCell { 1, -3, 2, UUID(0xC1) });
		data.Cells.push_back(WorldCell { 7, 5, 5, UUID(0xC2) });
		WorldIndex index;
		index.MaxBodiesInRadius = 41210;
		index.PersistentBodies = 12;
		WorldCellSummary summary;
		summary.Layer = 1;
		summary.X = 0;
		summary.Z = -1;
		summary.Stamp = "1234-5678";
		summary.Entities = 2502;
		summary.Bodies = 501;
		summary.BoundsMin = glm::vec3(-128.0f, -1.0f, -192.0f);
		summary.BoundsMax = glm::vec3(128.0f, 30.0f, 64.0f);
		summary.ClosureBytes = 1048576;
		index.Cells.push_back(summary);
		data.Index = index;
		return data;
	}

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json, 1, '\t');
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// The reason a document changed by `edit` is rejected (empty when it loads).
	std::string Reject(const std::function<void(nlohmann::json& world)>& edit)
	{
		nlohmann::json document = WorldAsset::ToDocument(MakeWorld());
		edit(document["World"]);
		std::string error;
		const Ref<WorldAsset> world = WorldAsset::FromJson(document, &error);
		return world ? std::string() : error;
	}

}

TEST_SUITE("World.Asset")
{
	TEST_CASE("A world round trips through JSON and bytes, and finds its cells by key")
	{
		const WorldData data = MakeWorld();
		std::string error;
		const Ref<WorldAsset> world = WorldAsset::Create(data, &error);
		REQUIRE_MESSAGE(world, error);
		CHECK(world->GetType() == AssetType::World);

		const nlohmann::json document = world->Serialize();
		CHECK(document["Strata"]["Format"] == "World");
		CHECK(document["Strata"]["Version"] == 1);
		const std::vector<uint8_t> bytes = ToBytes(document);
		const Ref<WorldAsset> loaded = WorldAsset::Deserialize(bytes, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(ToBytes(loaded->Serialize()) == bytes);

		const WorldData& read = loaded->GetData();
		CHECK(read.Name == "Island");
		CHECK(read.PersistentScene == c_Persistent);
		CHECK(read.GridOrigin == glm::dvec2(-128.0, 64.0));
		REQUIRE(read.Layers.size() == 2);
		CHECK(read.Layers[1].ID == 7);
		CHECK(read.Layers[1].Name == "Props");
		CHECK(read.Layers[1].CellSize == 64.0f);
		CHECK(read.Layers[1].StreamingDistance == 200.0f);
		CHECK_FALSE(read.Layers[1].EnabledByDefault);
		CHECK(read.Layers[0].LoadBeforeStart);
		REQUIRE(read.Cells.size() == 3);
		CHECK(read.Cells[1].X == -3);
		CHECK(read.Cells[1].Z == 2);
		CHECK(read.Cells[1].Scene == UUID(0xC1));
		REQUIRE(read.Index.has_value());
		CHECK(read.Index->MaxBodiesInRadius == 41210);
		CHECK(read.Index->PersistentBodies == 12);
		REQUIRE(read.Index->Cells.size() == 1);
		CHECK(read.Index->Cells[0].Stamp == "1234-5678");
		CHECK(read.Index->Cells[0].Entities == 2502);
		CHECK(read.Index->Cells[0].Bodies == 501);
		CHECK(read.Index->Cells[0].BoundsMax == glm::vec3(128.0f, 30.0f, 64.0f));
		CHECK(read.Index->Cells[0].ClosureBytes == 1048576);

		CHECK(loaded->FindCell(WorldCellKey { 7, 5, 5 }) == 2);
		CHECK(loaded->FindCell(WorldCellKey { 1, 5, 5 }) == -1);
		CHECK(loaded->FindCell(WorldCellKey { 1, std::numeric_limits<int32_t>::max(), 0 }) == -1);
		REQUIRE(loaded->FindSummary(WorldCellKey { 1, 0, -1 }));
		CHECK(loaded->FindSummary(WorldCellKey { 1, 0, -1 })->Bodies == 501);
		CHECK(loaded->FindSummary(WorldCellKey { 1, -3, 2 }) == nullptr);
		CHECK(read.FindLayer("Props")->ID == 7);
		CHECK(read.FindLayer(uint8_t(1))->Name == "Terrain");
		CHECK(loaded->GetMemoryUsage().Cpu > sizeof(WorldAsset));

		// Without an index, a persistent scene or a name; unknown keys are ignored.
		nlohmann::json bare = WorldAsset::ToDocument(MakeWorld());
		bare["World"].erase("Index");
		bare["World"]["PersistentScene"] = nullptr;
		bare["World"].erase("Name");
		bare["World"]["FutureKey"] = { 1, 2, 3 };
		bare["World"]["Layers"][0]["FutureKey"] = "kept out";
		const Ref<WorldAsset> minimal = WorldAsset::FromJson(bare, &error);
		REQUIRE_MESSAGE(minimal, error);
		CHECK_FALSE(minimal->GetData().Index.has_value());
		CHECK_FALSE(minimal->GetData().PersistentScene.IsValid());
		CHECK(minimal->FindSummary(WorldCellKey { 1, 0, -1 }) == nullptr);
	}

	TEST_CASE("Every validation rule rejects a world with its reason")
	{
		CHECK(Reject([](nlohmann::json&) {}).empty());

		struct Case
		{
			const char* Rule;
			std::function<void(nlohmann::json&)> Edit;
			const char* Reason;
		};
		const Case cases[] = {
			{ "Layers missing", [](nlohmann::json& world) { world.erase("Layers"); }, "\"Layers\" is missing" },
			{ "No layer", [](nlohmann::json& world) { world["Layers"] = nlohmann::json::array(); }, "1 to 32 layers" },
			{ "33 layers", [](nlohmann::json& world)
				{
					nlohmann::json layers = nlohmann::json::array();
					for (int id = 1; id <= 33; id++)
						layers.push_back({ { "ID", id }, { "Name", fmt::format("L{}", id) }, { "CellSize", 16 }, { "StreamingDistance", 32 } });
					world["Layers"] = layers;
					world["Cells"] = nlohmann::json::array();
					world.erase("Index");
				}, "more than 32 items" },
			{ "Layer ID 0", [](nlohmann::json& world) { world["Layers"][0]["ID"] = 0; }, "layer ID from 1 to 255" },
			{ "Layer ID 256", [](nlohmann::json& world) { world["Layers"][0]["ID"] = 256; }, "layer ID from 1 to 255" },
			{ "Layer ID not a number", [](nlohmann::json& world) { world["Layers"][0]["ID"] = "one"; }, "must be an integer" },
			{ "Duplicate layer ID", [](nlohmann::json& world) { world["Layers"][1]["ID"] = 1; }, "is used by layer 'Terrain' too" },
			{ "Empty layer name", [](nlohmann::json& world) { world["Layers"][1]["Name"] = ""; }, "1 to 64 characters" },
			{ "Control character in a name", [](nlohmann::json& world) { world["Layers"][1]["Name"] = "Pro\tps"; }, "no control characters" },
			{ "Comma in a name", [](nlohmann::json& world) { world["Layers"][1]["Name"] = "Props,Trees"; }, "must not contain commas" },
			{ "Space around a name", [](nlohmann::json& world) { world["Layers"][1]["Name"] = " Props"; }, "start or end with a space" },
			{ "Duplicate layer name", [](nlohmann::json& world) { world["Layers"][1]["Name"] = "Terrain"; }, "another layer has the same name" },
			{ "Cell size below 1", [](nlohmann::json& world) { world["Layers"][0]["CellSize"] = 0.5; }, "CellSize must be from 1 to 65536" },
			{ "Cell size above 65536", [](nlohmann::json& world) { world["Layers"][0]["CellSize"] = 70000; }, "CellSize must be from 1 to 65536" },
			{ "Negative streaming distance", [](nlohmann::json& world) { world["Layers"][0]["StreamingDistance"] = -1; }, "StreamingDistance must be from 0" },
			{ "Streaming distance above 1,000 km", [](nlohmann::json& world) { world["Layers"][0]["StreamingDistance"] = 2e6; }, "StreamingDistance must be from 0" },
			{ "Boolean flag of the wrong type", [](nlohmann::json& world) { world["Layers"][0]["LoadBeforeStart"] = 1; }, "must be true or false" },
			{ "Unknown layer", [](nlohmann::json& world) { world["Cells"][2]["Layer"] = 9; }, "no layer with that ID" },
			{ "Duplicate cell", [](nlohmann::json& world) { world["Cells"][1]["X"] = 0; world["Cells"][1]["Z"] = -1; }, "lists it twice" },
			{ "Cell beyond 1,000 km", [](nlohmann::json& world) { world["Cells"][1]["X"] = 4000; }, "beyond the 1000 km a world may reach" },
			{ "Cell coordinate not an integer", [](nlohmann::json& world) { world["Cells"][1]["X"] = 1.5; }, "must be an integer" },
			{ "Cell without a scene", [](nlohmann::json& world) { world["Cells"][0]["Scene"] = nullptr; }, "it has no scene" },
			{ "Cell scene not a handle", [](nlohmann::json& world) { world["Cells"][0]["Scene"] = "not a handle"; }, "must be an asset handle" },
			{ "Two cells share a scene", [](nlohmann::json& world) { world["Cells"][2]["Scene"] = world["Cells"][0]["Scene"]; }, "belongs to another cell too" },
			{ "A cell is the persistent scene", [](nlohmann::json& world) { world["Cells"][0]["Scene"] = world["PersistentScene"]; }, "is the world's persistent scene" },
			{ "Grid origin beyond 1,000 km", [](nlohmann::json& world) { world["GridOrigin"] = { 2e6, 0 }; }, "GridOrigin must lie within 1000 km" },
			{ "Grid origin of three numbers", [](nlohmann::json& world) { world["GridOrigin"] = { 0, 0, 0 }; }, "array of 2 numbers" },
			{ "Index of another version", [](nlohmann::json& world) { world["Index"]["Version"] = 2; }, "unsupported version 2" },
			{ "Negative count", [](nlohmann::json& world) { world["Index"]["Cells"][0]["Bodies"] = -5; }, "must not be negative" },
			{ "Negative radius bodies", [](nlohmann::json& world) { world["Index"]["MaxBodiesInRadius"] = -1; }, "must not be negative" },
			{ "Bounds with Min above Max", [](nlohmann::json& world) { world["Index"]["Cells"][0]["Bounds"]["Min"][1] = 100; }, "Min at most Max" },
			{ "Bounds not finite", [](nlohmann::json& world) { world["Index"]["Cells"][0]["Bounds"]["Max"] = { 1, "x", 2 }; }, "finite numbers" },
			{ "Bounds missing", [](nlohmann::json& world) { world["Index"]["Cells"][0].erase("Bounds"); }, "\"Bounds\" must be an object" },
			{ "Index of an unknown cell", [](nlohmann::json& world) { world["Index"]["Cells"][0]["X"] = 3; }, "the world has no such cell" },
			{ "Index lists a cell twice", [](nlohmann::json& world) { world["Index"]["Cells"].push_back(world["Index"]["Cells"][0]); }, "lists it twice" },
			{ "Layers not an array", [](nlohmann::json& world) { world["Layers"] = { { "ID", 1 } }; }, "must be an array" },
			{ "Cell not an object", [](nlohmann::json& world) { world["Cells"][0] = 3; }, "must be an object" },
			{ "Name with a control character", [](nlohmann::json& world) { world["Name"] = "Is\nland"; }, "no control characters" },
		};
		for (const Case& item : cases)
		{
			INFO(item.Rule);
			const std::string reason = Reject(item.Edit);
			CHECK(reason.find(item.Reason) != std::string::npos);
			CAPTURE(reason);
		}

		std::string error;
		CHECK_FALSE(WorldAsset::FromJson({ { "Strata", { { "Format", "Scene" }, { "Version", 1 } } }, { "World", nlohmann::json::object() } }, &error));
		CHECK(error == "Not a Strata world document");
		CHECK_FALSE(WorldAsset::FromJson({ { "Strata", { { "Format", "World" }, { "Version", 2 } } }, { "World", nlohmann::json::object() } }, &error));
		CHECK(error == "Unsupported world version 2");
		CHECK_FALSE(WorldAsset::FromJson({ { "Strata", { { "Format", "World" }, { "Version", 1 } } } }, &error));
		CHECK(error.find("no \"World\" object") != std::string::npos);

		// Data built in code is held to the same rules.
		WorldData data = MakeWorld();
		data.Layers[0].CellSize = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(WorldAsset::Create(data, &error));
		CHECK(error.find("CellSize") != std::string::npos);
	}

	TEST_CASE("Truncated and garbage bytes are rejected without crashing")
	{
		const std::vector<uint8_t> bytes = ToBytes(WorldAsset::ToDocument(MakeWorld()));
		for (size_t size = 0; size < bytes.size(); size += 7)
		{
			CAPTURE(size);
			std::string error;
			CHECK(WorldAsset::Deserialize(std::span<const uint8_t>(bytes.data(), size), &error) == nullptr);
			CHECK_FALSE(error.empty());
		}
		uint32_t state = 12345;
		for (int round = 0; round < 64; round++)
		{
			std::vector<uint8_t> garbled = bytes;
			for (int flip = 0; flip < 8; flip++)
			{
				state = state * 1664525u + 1013904223u;
				garbled[state % garbled.size()] = static_cast<uint8_t>(state >> 24);
			}
			std::string error;
			const Ref<WorldAsset> world = WorldAsset::Deserialize(garbled, &error);
			// Whatever loads is valid: its own document loads again.
			if (world)
				CHECK(WorldAsset::FromJson(world->Serialize()) != nullptr);
			else
				CHECK_FALSE(error.empty());
		}
	}

	TEST_CASE("Grid math: cells, packed keys and distances")
	{
		const WorldLayer layer { 3, "Ground", 32.0f, 100.0f, true, false };
		const glm::dvec2 origin(200.0, -100.0);
		CHECK(WorldGrid::CellAt(layer, origin, glm::dvec3(200.0, 5.0, -100.0)) == WorldCellKey { 3, 0, 0 });
		CHECK(WorldGrid::CellAt(layer, origin, glm::dvec3(199.99, 0.0, -100.01)) == WorldCellKey { 3, -1, -1 });
		CHECK(WorldGrid::CellAt(layer, origin, glm::dvec3(264.0, 0.0, -36.0)) == WorldCellKey { 3, 2, 2 });
		CHECK(WorldGrid::CellAt(layer, origin, glm::dvec3(std::numeric_limits<double>::quiet_NaN(), 0.0, 1e30)).Z == WorldCellKey::c_MaxCoordinate);

		const WorldCellSquare square = WorldGrid::CellBounds(layer, origin, 2, -1);
		CHECK(square.Min == glm::dvec2(264.0, -132.0));
		CHECK(square.Max == glm::dvec2(296.0, -100.0));
		CHECK(WorldGrid::Distance(square, glm::dvec3(270.0, 1000.0, -110.0)) == 0.0);
		CHECK(WorldGrid::Distance(square, glm::dvec3(300.0, 0.0, -100.0)) == doctest::Approx(4.0));
		CHECK(WorldGrid::Distance(square, glm::dvec3(299.0, 0.0, -96.0)) == doctest::Approx(5.0));

		for (const WorldCellKey key : { WorldCellKey { 0, 0, 0 }, WorldCellKey { 255, -WorldCellKey::c_MaxCoordinate, WorldCellKey::c_MaxCoordinate }, WorldCellKey { 9, -1, 1 } })
			CHECK(WorldCellKey::Unpack(key.Pack()) == key);
		// Packed keys order like (Layer, X, Z).
		CHECK(WorldCellKey { 1, -5, 3 }.Pack() < WorldCellKey { 1, -4, -9 }.Pack());
		CHECK(WorldCellKey { 1, 5, 3 }.Pack() < WorldCellKey { 2, -9, -9 }.Pack());
		CHECK(WorldCellKey { 1, 0, -2 }.Pack() < WorldCellKey { 1, 0, -1 }.Pack());
	}

	TEST_CASE("The feature world lists its persistent scene and cells as references, and survives a pack round trip")
	{
		FeatureProject project;
		const Ref<EditorAssetManager>& editor = project.GetAssetManager();
		const AssetHandle handle = editor->FindAssetByPath("Worlds/FeatureWorld.stworld");
		REQUIRE(handle.IsValid());
		REQUIRE(editor->GetAssetType(handle) == AssetType::World);
		const Ref<WorldAsset> world = std::dynamic_pointer_cast<WorldAsset>(editor->LoadAssetSync(handle));
		REQUIRE(world);
		const WorldData& data = world->GetData();
		CHECK(data.PersistentScene == editor->FindAssetByPath("Scenes/Feature.stscene"));
		REQUIRE(data.Layers.size() == 2);
		REQUIRE(data.Cells.size() == 8);

		// The scan lists the persistent scene, then every cell, once each; the cells' handles are derived from the world's,
		// and their documents lie in <world folder>/<world name>/<layer>/<X>_<Z>.stscene.
		std::vector<AssetHandle> expected { data.PersistentScene };
		for (const WorldCell& cell : data.Cells)
		{
			const std::string layer = data.FindLayer(cell.Layer)->Name;
			CAPTURE(layer);
			CHECK(cell.Scene == DeriveSubAssetHandle(handle, fmt::format("Cell/{}/{}/{}", cell.Layer, cell.X, cell.Z)));
			CHECK(editor->FindAssetByPath(fmt::format("Worlds/FeatureWorld/{}/{}_{}.stscene", layer, cell.X, cell.Z)) == cell.Scene);
			expected.push_back(cell.Scene);
		}
		std::vector<AssetHandle> scanned;
		std::string error;
		REQUIRE(AssetDependencies::Scan(AssetType::World, ToBytes(world->Serialize()), scanned, &error));
		CHECK(scanned == expected);
		CHECK(editor->GetDependencies(handle) == expected);
		// Loading the world loads none of its scenes: they load as levels and cells of their own.
		CHECK(editor->GetDependencyClosure(handle, AssetClosureScope::Load) == std::vector<AssetHandle> { handle });
		CHECK(editor->GetDependencyClosure(handle, AssetClosureScope::All).size() > expected.size());

		const std::filesystem::path packPath = project.GetDirectory() / "Build" / "World.stpak";
		REQUIRE_MESSAGE(editor->BuildAssetPack(packPath, &error), error);
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(packPath, &error);
		REQUIRE_MESSAGE(runtime, error);
		CHECK(runtime->GetAssetType(handle) == AssetType::World);
		CHECK(runtime->GetDependencies(handle) == expected);
		const Ref<WorldAsset> unpacked = std::dynamic_pointer_cast<WorldAsset>(runtime->LoadAssetSync(handle));
		REQUIRE(unpacked);
		CHECK(ToBytes(unpacked->Serialize()) == ToBytes(world->Serialize()));
	}
}
