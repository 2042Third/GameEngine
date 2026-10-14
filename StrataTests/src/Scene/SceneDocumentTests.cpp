#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Perf/WorldFixture.h"
#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Core/Crypto.h"
#include "Strata/Core/DocumentOutput.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/EntitySink.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneDocumentReader.h"
#include "Strata/Scene/SceneDocumentWriter.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The feature scene: every component, script fields of every type (asset and entity fields among them), prefab links.
	Ref<Scene> LoadFeatureScene()
	{
		std::string error;
		const Ref<Scene> scene = SceneSerializer::LoadFromFile(GetFeatureProjectSourceDirectory() / "Assets" / "Scenes" / "Feature.stscene", &error);
		INFO(error);
		REQUIRE(scene);
		return scene;
	}

	// Kept components of a module this build lacks, named to sort before, between and after the registered ones.
	void AddUnknownComponents(Scene& scene, Entity entity)
	{
		UnknownComponentsComponent unknown;
		unknown.Components = { { "AAFirst", { { "Value", 1 } } }, { "Middle", { { "Text", "between" }, { "List", { 1, 2.5, "x" } } } },
			{ "Zz.Last", nlohmann::json::object() } };
		scene.GetRegistry().emplace<UnknownComponentsComponent>(entity.GetHandle(), std::move(unknown));
	}

	struct WrittenText
	{
		std::string Text;
		SceneWriteResult Result;
	};

	WrittenText WriteDocument(const Scene& scene, const SceneWriteOptions& options = {})
	{
		StringDocumentOutput output;
		WrittenText document;
		std::string error;
		INFO(error);
		REQUIRE(SceneDocumentWriter::Write(scene, output, options, &document.Result, &error));
		document.Text = output.TakeText();
		return document;
	}

	// The document's lines: the header line, one line per entity (without its comma), the closing line.
	std::vector<std::string> SplitLines(const std::string& text)
	{
		std::vector<std::string> lines;
		size_t start = 0;
		while (start < text.size())
		{
			const size_t end = text.find('\n', start);
			REQUIRE(end != std::string::npos); // Every line ends with LF, the last one too
			lines.push_back(text.substr(start, end - start));
			start = end + 1;
		}
		return lines;
	}

	// The references of extra data as the editor finds them (AssetDependencies, which reads script fields).
	void ScanExtraReferences(const ComponentInfo& info, const nlohmann::json& component, std::vector<AssetHandle>& outReferences)
	{
		const nlohmann::json snapshot = { { "Entities", nlohmann::json::array({ { { "Components", { { info.Name, component } } } } }) } };
		AssetDependencies::ScanEntities(snapshot, outReferences);
	}

	// Checks the fast formatting of one float against FloatToJson's dump; counts mismatches (one CHECK per float would make
	// the run report hundreds of thousands of assertions).
	void CheckFloat(float value, size_t& mismatches, std::string& firstMismatch)
	{
		std::string fast;
		AppendFloatJson(fast, value);
		const std::string expected = JsonUtils::Dump(FloatToJson(value));
		if (fast != expected)
		{
			if (mismatches == 0)
			{
				uint32_t bits = 0;
				std::memcpy(&bits, &value, sizeof(bits));
				firstMismatch = fmt::format("bits {:08X}: '{}' instead of '{}'", bits, fast, expected);
			}
			mismatches++;
		}
	}

	float FromBits(uint32_t bits)
	{
		float value = 0.0f;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}

}

TEST_SUITE("Scene.Documents")
{
	TEST_CASE("Floats are written as FloatToJson's dump writes them, over every exponent and the edge values")
	{
		size_t mismatches = 0;
		size_t checked = 0;
		std::string firstMismatch;
		// Every exponent (subnormals and the non-finite values included), both signs, with the smallest, largest and
		// half-way mantissas and a pseudo-random sample of others.
		uint32_t state = 2463534242u;
		for (uint32_t exponent = 0; exponent < 256; exponent++)
		{
			for (uint32_t sign = 0; sign < 2; sign++)
			{
				const uint32_t high = (sign << 31) | (exponent << 23);
				for (uint32_t mantissa : { 0u, 1u, 2u, 0x400000u, 0x7FFFFFu, 0x7FFFFEu, 0x3FFFFFu, 0x555555u, 0x2AAAAAu })
				{
					CheckFloat(FromBits(high | mantissa), mismatches, firstMismatch);
					checked++;
				}
				for (int sample = 0; sample < 400; sample++)
				{
					state ^= state << 13;
					state ^= state >> 17;
					state ^= state << 5;
					CheckFloat(FromBits(high | (state & 0x7FFFFFu)), mismatches, firstMismatch);
					checked++;
				}
			}
		}
		// The edge values, and values scenes are full of.
		for (const float value : { 0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 0.5f, 1.0e7f, 1.0e8f, 1.2345678e7f, 16777216.0f, 16777217.0f, 1.0e-5f, 1.0e-4f,
			123456.79f, 3.14159265f, std::numeric_limits<float>::min(), std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(),
			std::numeric_limits<float>::lowest(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::quiet_NaN(), 1.0f / 60.0f, -9.81f, 0.05f, 1000.0f, 1.0e15f, 1.0e16f, 1.0e-13f, 1.0e-14f })
		{
			CheckFloat(value, mismatches, firstMismatch);
			checked++;
		}
		INFO(firstMismatch);
		CHECK(mismatches == 0);
		CHECK(checked > 200000);
		// Non-finite values become 0, as FloatToJson writes them.
		std::string nan;
		AppendFloatJson(nan, std::numeric_limits<float>::quiet_NaN());
		CHECK(nan == "0.0");
	}

	TEST_CASE("Property values are written as PropertyValueToJson's dump writes them")
	{
		auto check = [](PropertyType type, const PropertyValue& value, std::vector<EnumValue> options = {})
		{
			PropertyInfo property;
			property.Name = "Value";
			property.Type = type;
			property.EnumValues = std::move(options);
			std::string fast;
			AppendPropertyValueJson(fast, property, value);
			CAPTURE(PropertyTypeToString(type));
			CHECK(fast == JsonUtils::Dump(PropertyValueToJson(property, value)));
		};
		check(PropertyType::Bool, true);
		check(PropertyType::Bool, false);
		check(PropertyType::Int, std::numeric_limits<int32_t>::min());
		check(PropertyType::Int, int32_t(-7));
		check(PropertyType::UInt, std::numeric_limits<uint32_t>::max());
		check(PropertyType::Float, -0.25f);
		check(PropertyType::Vec2, glm::vec2(1.5f, -0.0f));
		check(PropertyType::Vec3, glm::vec3(0.1f, 1.0e20f, -3.0f));
		check(PropertyType::Vec4, glm::vec4(1.0f, 2.0f, 3.0f, 4.5f));
		check(PropertyType::Color3, glm::vec3(0.18f, 0.32f, 0.62f));
		check(PropertyType::Color4, glm::vec4(0.05f, 0.05f, 0.07f, 1.0f));
		check(PropertyType::Quat, glm::quat(0.7071f, 0.0f, 0.7071f, 0.0f));
		check(PropertyType::String, std::string("Hello \"world\"\n\xC3\xA9 \xFF"));
		check(PropertyType::String, std::string());
		check(PropertyType::Enum, int32_t(1), { { "First", 0 }, { "Second", 1 } });
		check(PropertyType::Enum, int32_t(9), { { "First", 0 } });
		check(PropertyType::Asset, UUID(0x0123456789ABCDEFull));
		check(PropertyType::Asset, UUID::Null());
		check(PropertyType::Entity, UUID(42));
	}

	TEST_CASE("Every entity line is the dump of the serializer's entity object, and the document reads as its document plus EntityCount")
	{
		ScopedJobSystem jobs(4, 1);
		// The feature scene with kept unknown components, and a generated world of three districts.
		const Ref<Scene> feature = LoadFeatureScene();
		AddUnknownComponents(*feature, feature->GetEntityByUUID(feature->GetRootEntities().front()));
		Scene world("Small");
		std::string error;
		REQUIRE(BuildWorldScene(world, { "Small", 2 * c_WorldDistrictCubes + 100, true }, std::vector<AssetHandle> { UUID(7), UUID(8) }, error));

		for (const Scene* scene : { feature.get(), &world })
		{
			CAPTURE(scene->GetName());
			SceneWriteOptions options;
			options.ChunkEntities = 97; // Many chunks, also for the feature scene
			const WrittenText document = WriteDocument(*scene, options);
			const nlohmann::json expected = SceneSerializer::Serialize(*scene);
			const nlohmann::json& entities = expected["Scene"]["Entities"];

			const std::vector<std::string> lines = SplitLines(document.Text);
			REQUIRE(lines.size() == entities.size() + 2);
			CHECK(document.Result.Entities == entities.size());
			CHECK(document.Result.Bytes == document.Text.size());
			for (size_t index = 0; index < entities.size(); index++)
			{
				std::string_view line = lines[index + 1];
				if (index + 1 < entities.size())
				{
					REQUIRE(line.ends_with(','));
					line.remove_suffix(1);
				}
				if (line != JsonUtils::Dump(entities[index]))
				{
					CHECK(line == JsonUtils::Dump(entities[index]));
					break;
				}
			}
			CHECK(lines.front().starts_with("{\"Strata\":{\"Format\":\"Scene\",\"Version\":1},\"Scene\":{\"Name\":"));
			CHECK(lines.front().ends_with("\"Entities\":["));
			CHECK(lines.back() == "]}}");

			// The whole document is today's document with the entity count.
			const std::optional<nlohmann::json> parsed = JsonUtils::Parse(document.Text);
			REQUIRE(parsed.has_value());
			nlohmann::json withCount = expected;
			withCount["Scene"]["EntityCount"] = entities.size();
			CHECK(*parsed == withCount);
		}
	}

	TEST_CASE("The references the writer collects are those a scan of the written bytes finds, in order")
	{
		const Ref<Scene> feature = LoadFeatureScene();
		AddUnknownComponents(*feature, feature->GetEntityByUUID(feature->GetRootEntities().back()));
		SceneWriteOptions options;
		options.ExtraReferences = ScanExtraReferences;
		options.ChunkEntities = 5;
		const WrittenText document = WriteDocument(*feature, options);
		std::vector<AssetHandle> scanned;
		std::string error;
		REQUIRE(AssetDependencies::Scan(AssetType::Scene, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(document.Text.data()),
			document.Text.size()), scanned, &error));
		CHECK(document.Result.References == scanned);
		// Script fields of asset type are among them.
		CHECK(std::find(scanned.begin(), scanned.end(), UUID(0xF7A0000000000005ull)) != scanned.end());

		// Without the extra-data scanner only asset properties count.
		const WrittenText plain = WriteDocument(*feature);
		CHECK(std::find(plain.Result.References.begin(), plain.Result.References.end(), UUID(0xF7A0000000000005ull)) == plain.Result.References.end());
	}

	TEST_CASE("Entity subsets write their roots with their descendants and parents only within the subset")
	{
		Scene scene("Subsets");
		const Entity a = scene.CreateEntity("A");
		const Entity a1 = scene.CreateChildEntity(a, "A1");
		const Entity a11 = scene.CreateChildEntity(a1, "A11");
		const Entity a2 = scene.CreateChildEntity(a, "A2");
		const Entity b = scene.CreateEntity("B");
		const Entity b1 = scene.CreateChildEntity(b, "B1");
		scene.CreateEntity("C");

		// A1 (with A11), B (with B1), and A11 again (nested in A1: written once, as part of it), and an unknown ID.
		SceneWriteOptions options;
		options.Roots = { a1.GetUUID(), b.GetUUID(), a11.GetUUID(), UUID(12345) };
		options.WriteSettings = false;
		options.Name = "Cell";
		const WrittenText document = WriteDocument(scene, options);
		const std::optional<nlohmann::json> parsed = JsonUtils::Parse(document.Text);
		REQUIRE(parsed.has_value());
		const nlohmann::json& content = (*parsed)["Scene"];
		CHECK(content["Name"] == "Cell");
		CHECK_FALSE(content.contains("Settings"));
		CHECK(content["EntityCount"] == 4);
		const nlohmann::json& entities = content["Entities"];
		REQUIRE(entities.size() == 4);
		const UUID expectedOrder[] = { a1.GetUUID(), a11.GetUUID(), b.GetUUID(), b1.GetUUID() };
		for (size_t index = 0; index < 4; index++)
			CHECK(UUIDFromJson(entities[index]["ID"]) == expectedOrder[index]);
		// A1's parent A is not written: A1 is top-level in the document. A11 keeps A1, B1 keeps B.
		CHECK_FALSE(entities[0].contains("Parent"));
		CHECK(UUIDFromJson(entities[1]["Parent"]) == a1.GetUUID());
		CHECK_FALSE(entities[2].contains("Parent"));
		CHECK(UUIDFromJson(entities[3]["Parent"]) == b.GetUUID());
		CHECK(document.Result.Entities == 4);
		(void)a2;

		// A subset that loads as a scene of its own.
		Scene loaded;
		std::string error;
		REQUIRE(SceneSerializer::Deserialize(loaded, *parsed, &error));
		CHECK(loaded.GetEntityCount() == 4);
		CHECK(loaded.GetRootEntities().size() == 2);

		// No root of the scene: an empty document.
		options.Roots = { UUID(999) };
		const WrittenText empty = WriteDocument(scene, options);
		CHECK(empty.Text.ends_with("\"EntityCount\":0,\"Entities\":[]}}\n"));
		CHECK(JsonUtils::Parse(empty.Text).has_value());
	}

	TEST_CASE("Prefab and model documents are written in their own formats and load")
	{
		Scene scene("Source");
		Entity root = scene.CreateEntity("Root");
		root.AddComponent<MeshRendererComponent>().Mesh = UUID(1);
		scene.CreateChildEntity(root, "Child");
		for (const SceneDocumentKind kind : { SceneDocumentKind::Prefab, SceneDocumentKind::Model })
		{
			SceneWriteOptions options;
			options.Kind = kind;
			options.Roots = { root.GetUUID() };
			const WrittenText document = WriteDocument(scene, options);
			const std::string format = SceneDocumentKindToString(kind);
			CHECK(document.Text.starts_with("{\"Strata\":{\"Format\":\"" + format + "\",\"Version\":1},\"" + format + "\":{\"EntityCount\":2,\"Entities\":["));
			const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(document.Text.data()), document.Text.size());
			std::string error;
			if (kind == SceneDocumentKind::Prefab)
			{
				const Ref<Prefab> prefab = Prefab::Deserialize(bytes, &error);
				INFO(error);
				REQUIRE(prefab);
				CHECK(prefab->GetEntityCount() == 2);
			}
			else
			{
				const Ref<Model> model = Model::Deserialize(bytes, &error);
				INFO(error);
				REQUIRE(model);
				CHECK(model->GetEntityCount() == 2);
			}
			CHECK(document.Result.References == std::vector<AssetHandle> { UUID(1) });
		}
	}

	TEST_CASE("The bytes do not depend on the thread count or the chunk size, and are hashed as written")
	{
		Scene world("Hashed");
		std::string error;
		REQUIRE(BuildWorldScene(world, { "Hashed", 3000, true }, std::vector<AssetHandle> { UUID(7) }, error));
		std::string serial;
		{
			SceneWriteOptions options;
			options.ChunkEntities = 1000;
			serial = WriteDocument(world, options).Text;
		}
		ScopedJobSystem jobs(4, 1);
		for (const uint32_t chunk : { 1u, 7u, 4096u })
		{
			SceneWriteOptions options;
			options.ChunkEntities = chunk;
			std::vector<float> progress;
			options.Progress = [&progress](float share) { progress.push_back(share); };
			const WrittenText document = WriteDocument(world, options);
			CAPTURE(chunk);
			CHECK(document.Text == serial);
			CHECK(document.Result.Sha256 == Sha256::Hash(document.Text));
			CHECK(document.Result.ContentHash == Hash::FNV1a(std::string_view(document.Text)));
			REQUIRE_FALSE(progress.empty());
			// One call at a time, each larger than the last: after every chunk serialized and every batch written.
			CHECK(std::adjacent_find(progress.begin(), progress.end(), std::greater_equal<float>()) == progress.end());
			CHECK(progress.size() >= (world.GetEntityCount() + chunk - 1) / chunk);
			CHECK(progress.back() == 1.0f);
		}
	}

	TEST_CASE("A cancelled write and a failing output end the write with an error")
	{
		Scene world("Cancelled");
		std::string error;
		REQUIRE(BuildWorldScene(world, { "Cancelled", 3000, false }, {}, error));

		// Cancelled once some chunks were written.
		int calls = 0;
		SceneWriteOptions options;
		options.ChunkEntities = 10;
		options.Cancel = [&calls]() { return ++calls > 20; };
		StringDocumentOutput output;
		SceneWriteResult result;
		CHECK_FALSE(SceneDocumentWriter::Write(world, output, options, &result, &error));
		CHECK(result.Cancelled);
		CHECK_FALSE(error.empty());
		CHECK(output.GetBytesWritten() < WriteDocument(world).Text.size());

		// An output that fails after a while.
		class FailingOutput final : public DocumentOutput
		{
		public:
			bool Write(std::string_view bytes) override
			{
				if (m_Bytes + bytes.size() > 1000)
					return false;
				m_Bytes += bytes.size();
				return true;
			}
			uint64_t GetBytesWritten() const override { return m_Bytes; }
		private:
			uint64_t m_Bytes = 0;
		};
		FailingOutput failing;
		error.clear();
		CHECK_FALSE(SceneDocumentWriter::Write(world, failing, {}, &result, &error));
		CHECK_FALSE(result.Cancelled);
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("Entities are counted on the thread that serializes them")
	{
		// Without the job system every chunk is serialized on this thread.
		Scene scene("Counted");
		for (int index = 0; index < 10; index++)
			scene.CreateEntity("Entity");
		const uint64_t before = SceneDocumentWriter::GetThreadEntityCount();
		WriteDocument(scene);
		CHECK(SceneDocumentWriter::GetThreadEntityCount() - before == 10);
	}

	TEST_CASE("SaveToFile writes the streamed layout, which loads back to the same scene")
	{
		const Ref<Scene> feature = LoadFeatureScene();
		const std::filesystem::path path = CreateTemporaryDirectory("SaveToFile") / "Feature.stscene";
		std::string error;
		REQUIRE(SceneSerializer::SaveToFile(*feature, path, &error));
		const std::optional<std::string> text = FileSystem::ReadText(path);
		REQUIRE(text.has_value());
		CHECK(*text == WriteDocument(*feature).Text);
		const Ref<Scene> loaded = SceneSerializer::LoadFromFile(path, &error);
		REQUIRE(loaded);
		CHECK(SceneSerializer::Serialize(*loaded) == SceneSerializer::Serialize(*feature));
	}

	TEST_CASE("A snapshot to save holds what the scene's document holds and writes the same bytes, without world transforms")
	{
		ScopedJobSystem jobs(4, 1);
		const Ref<Scene> feature = LoadFeatureScene();
		AddUnknownComponents(*feature, feature->GetEntityByUUID(feature->GetRootEntities().front()));
		feature->UpdateWorldTransforms();
		const Ref<Scene> snapshot = Scene::Copy(feature, SceneCopyOptions { true });
		CHECK(snapshot->GetEntityCount() == feature->GetEntityCount());
		CHECK(snapshot->GetRootEntities() == feature->GetRootEntities());
		CHECK(SceneSerializer::Serialize(*snapshot) == SceneSerializer::Serialize(*feature));
		CHECK(WriteDocument(*snapshot).Text == WriteDocument(*feature).Text);
		// The cached world transforms stay behind; a play copy has them.
		const auto* transforms = std::as_const(snapshot->GetRegistry()).storage<WorldTransformComponent>();
		CHECK((!transforms || transforms->empty()));
		const Ref<Scene> play = Scene::Copy(feature);
		const auto* playTransforms = std::as_const(play->GetRegistry()).storage<WorldTransformComponent>();
		REQUIRE(playTransforms);
		CHECK(playTransforms->size() == feature->GetEntityCount());
		// The snapshot keeps the scene's source and lineage, like every copy.
		CHECK(snapshot->GetLineageID() == feature->GetLineageID());
	}
}


namespace
{

	// Counts what a reader hands it, refusing nothing; `OnEntity` hears of each entity's number (from 1). With
	// `takesPrepared` it takes prepared chunks too (the parallel read).
	class CountingSink final : public EntitySink
	{
	public:
		explicit CountingSink(bool takesPrepared = false)
			: m_TakesPrepared(takesPrepared)
		{
		}

		void ReserveEntities(uint64_t count) override { Reserved = count; }
		void ReadField(std::string_view key, const nlohmann::json&) override { Fields.emplace_back(key); }
		bool ReadEntity(const nlohmann::json&, std::string*) override
		{
			Entities++;
			if (OnEntity)
				OnEntity(Entities);
			return true;
		}
		bool TakesPreparedEntities() const override { return m_TakesPrepared; }
		bool ReadPrepared(PreparedEntities& entities, std::string*) override
		{
			for (size_t index = 0; index < entities.Columns.GetEntityCount(); index++)
			{
				Entities++;
				if (OnEntity)
					OnEntity(Entities);
			}
			Chunks++;
			return true;
		}
		bool Finish(std::string*) override
		{
			Finished = true;
			return true;
		}

		std::function<void(uint64_t)> OnEntity;
		uint64_t Reserved = 0;
		uint64_t Entities = 0;
		uint64_t Chunks = 0;
		std::vector<std::string> Fields;
		bool Finished = false;
	private:
		bool m_TakesPrepared = false;
	};

	struct SceneRead
	{
		bool Success = false;
		std::string Error;
		std::vector<std::string> Warnings;
		Ref<Scene> Loaded = CreateRef<Scene>();
		SceneReadResult Result;
	};

	// The DOM reader's load of a document's text (SceneSerializer::Deserialize), as scenes were read before the streamed
	// reader.
	SceneRead ReadWithDom(const std::string& text)
	{
		SceneRead read;
		const std::optional<nlohmann::json> document = JsonUtils::Parse(text, &read.Error);
		if (document)
			read.Success = SceneSerializer::Deserialize(*read.Loaded, *document, &read.Error, &read.Warnings);
		return read;
	}

	SceneRead ReadStreamed(const std::string& text, const SceneReadOptions& options = {})
	{
		SceneRead read;
		read.Success = SceneSerializer::DeserializeText(*read.Loaded, text, options, &read.Error, &read.Warnings, {}, &read.Result);
		return read;
	}

	// Both readers load the text to the same scene, with the same warnings in the same order.
	void CheckSameRead(const std::string& text)
	{
		const SceneRead dom = ReadWithDom(text);
		const SceneRead streamed = ReadStreamed(text);
		INFO(dom.Error);
		INFO(streamed.Error);
		REQUIRE(dom.Success);
		REQUIRE(streamed.Success);
		CHECK(SceneSerializer::Serialize(*streamed.Loaded) == SceneSerializer::Serialize(*dom.Loaded));
		CHECK(streamed.Loaded->GetName() == dom.Loaded->GetName());
		CHECK(streamed.Warnings == dom.Warnings);
		CHECK(streamed.Loaded->GetRootEntities() == dom.Loaded->GetRootEntities());
	}

	std::string Header(std::string_view format = "Scene", int version = 1)
	{
		return fmt::format("\"Strata\":{{\"Format\":\"{}\",\"Version\":{}}}", format, version);
	}

	// A document of `count` entities (IDs 1 to count, each the child of the one before every tenth), written as the writer
	// lays them out: the header first, EntityCount when `declare`.
	std::string MakeEntityListText(size_t count, bool declare)
	{
		std::string text = "{" + Header() + ",\"Scene\":{\"Name\":\"Many\",";
		if (declare)
			text += fmt::format("\"EntityCount\":{},", count);
		text += "\"Entities\":[\n";
		for (size_t index = 0; index < count; index++)
		{
			nlohmann::json entity = { { "ID", UUIDToJson(UUID(index + 1)) }, { "Components", { { "Name", { { "Name", fmt::format("E{}", index) } } } } } };
			if (index % 10 != 0)
				entity["Parent"] = UUIDToJson(UUID(index));
			text += JsonUtils::Dump(entity);
			text += index + 1 < count ? ",\n" : "\n";
		}
		text += "]}}";
		return text;
	}

	// Entities and settings with everything a load reports: invalid and unknown properties, components this build does not
	// register (on two entities), a component that is not read from entity data, a 'Components' that is no object, an
	// invalid parent, a missing one and a cycle.
	nlohmann::json MakeProblemScene()
	{
		nlohmann::json entities = nlohmann::json::array();
		entities.push_back({ { "ID", UUIDToJson(UUID(1)) },
			{ "Components", { { "Transform", { { "Translation", "up" }, { "Wobble", 1 } } }, { "Turret", { { "Barrels", 2 } } } } } });
		entities.push_back({ { "ID", UUIDToJson(UUID(2)) }, { "Parent", UUIDToJson(UUID(1)) },
			{ "Components", { { "Turret", nlohmann::json::object() }, { "ID", { { "ID", UUIDToJson(UUID(77)) } } } } } });
		entities.push_back({ { "ID", UUIDToJson(UUID(3)) }, { "Parent", "not an ID" } });
		entities.push_back({ { "ID", UUIDToJson(UUID(4)) }, { "Parent", UUIDToJson(UUID(999)) } });
		entities.push_back({ { "ID", UUIDToJson(UUID(5)) }, { "Parent", UUIDToJson(UUID(6)) } });
		entities.push_back({ { "ID", UUIDToJson(UUID(6)) }, { "Parent", UUIDToJson(UUID(5)) } });
		entities.push_back({ { "ID", UUIDToJson(UUID(7)) }, { "Components", nlohmann::json::array() } });
		const nlohmann::json settings = { { "Gravity", "down" }, { "PhysicsCapacity", { { "MaxBodies", -1 } } }, { "FixedTimestep", 0.02 } };
		return { { "Strata", { { "Format", "Scene" }, { "Version", 1 } } },
			{ "Scene", { { "Name", "Problems" }, { "Settings", settings }, { "Entities", entities } } } };
	}

	// The document with the header and the settings first and an entity per line, as the writer orders them.
	std::string WriteInWriterOrder(const nlohmann::json& document)
	{
		const nlohmann::json& content = document["Scene"];
		std::string text = "{\"Strata\":" + JsonUtils::Dump(document["Strata"]) + ",\"Scene\":{\"Name\":" + JsonUtils::Dump(content["Name"])
			+ ",\"Settings\":" + JsonUtils::Dump(content["Settings"]) + fmt::format(",\"EntityCount\":{},\"Entities\":[\n", content["Entities"].size());
		for (size_t index = 0; index < content["Entities"].size(); index++)
			text += JsonUtils::Dump(content["Entities"][index]) + (index + 1 < content["Entities"].size() ? ",\n" : "\n");
		return text + "]}}";
	}

}

TEST_SUITE("Scene.Documents")
{
	TEST_CASE("Old layouts and the streamed one load to the scene the DOM reader makes, children before parents too")
	{
		const Ref<Scene> feature = LoadFeatureScene();
		AddUnknownComponents(*feature, feature->GetEntityByUUID(feature->GetRootEntities().front()));
		const nlohmann::json document = SceneSerializer::Serialize(*feature);
		// Tab-indented and compact: nlohmann sorts the keys, so the header comes after the entities, the settings after them too.
		CheckSameRead(JsonUtils::Dump(document, 1, '\t'));
		CheckSameRead(JsonUtils::Dump(document));
		CheckSameRead(WriteDocument(*feature).Text);
		// Children listed before their parents.
		nlohmann::json reversed = document;
		nlohmann::json& entities = reversed["Scene"]["Entities"];
		std::reverse(entities.begin(), entities.end());
		CheckSameRead(JsonUtils::Dump(reversed));
		// Keys a reader does not know, anywhere, with nested values.
		nlohmann::json extra = document;
		extra["Editor"] = { { "Camera", { 1, 2, { { "x", nullptr } } } } };
		extra["Scene"]["Notes"] = { { "List", nlohmann::json::array({ nlohmann::json::object(), 2.5, "text" }) } };
		CheckSameRead(JsonUtils::Dump(extra));
		// A scene without entities, and one without an entity list.
		CheckSameRead("{" + Header() + ",\"Scene\":{\"Name\":\"Empty\",\"Entities\":[]}}");
		CheckSameRead("{\"Scene\":{}," + Header() + "}");
	}

	TEST_CASE("The streamed reader reports what the DOM reader reports, in its order, also when the settings follow the entities")
	{
		const nlohmann::json document = MakeProblemScene();
		const SceneRead dom = ReadWithDom(JsonUtils::Dump(document));
		REQUIRE(dom.Success);
		// Every kind of report is there.
		const auto has = [&dom](std::string_view text)
		{
			return std::any_of(dom.Warnings.begin(), dom.Warnings.end(), [text](const std::string& warning) { return warning.find(text) != std::string::npos; });
		};
		CHECK(has("invalid 'Gravity'"));
		CHECK(has("Wobble"));
		CHECK(has("Unknown component 'Turret' (2 entities)"));
		CHECK(has("component 'ID' is not read from entity data"));
		CHECK(has("'Components' must be an object"));
		CHECK(has("invalid 'Parent'"));
		CHECK(has("not found"));
		CHECK(has("would create a cycle"));
		// Settings after the entities (sorted keys), and before them (the writer's order): the same reports in the same order.
		CheckSameRead(JsonUtils::Dump(document));
		CheckSameRead(WriteInWriterOrder(document));
	}

	TEST_CASE("Invalid documents fail with the DOM reader's errors")
	{
		const std::string header = Header();
		const std::vector<std::string> documents = {
			"[]",
			"{\"Scene\":{}}",
			"{" + header + "}",
			"{" + Header("Prefab") + ",\"Scene\":{}}",
			"{" + Header("Scene", 7) + ",\"Scene\":{}}",
			"{\"Strata\":\"Scene\",\"Scene\":{}}",
			"{" + header + ",\"Scene\":[]}",
			"{" + header + ",\"Scene\":{\"Entities\":{}}}",
			"{" + header + ",\"Scene\":{\"Entities\":null}}",
			"{" + header + ",\"Scene\":{\"Entities\":[1]}}",
			"{" + header + ",\"Scene\":{\"Entities\":[{\"ID\":\"zz\"}]}}",
			"{" + header + ",\"Scene\":{\"Entities\":[{\"ID\":\"0000000000000001\"},{\"ID\":\"0000000000000001\"}]}}",
			// The header after the entities: they are read before it fails.
			"{\"Scene\":{\"Entities\":[{\"ID\":\"0000000000000001\"}]}," + Header("Model") + "}",
		};
		for (const std::string& text : documents)
		{
			INFO(text);
			const SceneRead dom = ReadWithDom(text);
			const SceneRead streamed = ReadStreamed(text);
			REQUIRE_FALSE(dom.Success);
			CHECK_FALSE(streamed.Success);
			CHECK(streamed.Error == dom.Error);
			CHECK_FALSE(streamed.Result.SyntaxError);
		}

		const SceneRead broken = ReadStreamed("{" + header + ",\"Scene\":{\"Entities\":[{\"ID\":");
		CHECK_FALSE(broken.Success);
		CHECK(broken.Result.SyntaxError);
		CHECK(broken.Error.rfind("Invalid JSON", 0) == 0);
		CHECK_FALSE(ReadStreamed("{" + header + ",\"Scene\":{}} trailing").Success);
	}

	TEST_CASE("A document over the entity limit is refused before its first entity when it declares its count, else at the first entity beyond it")
	{
		SceneReadOptions options;
		options.MaxEntities = 100;
		std::string error;
		SceneReadResult result;

		CountingSink declared;
		CHECK_FALSE(SceneDocumentReader::Read(MakeEntityListText(1000, true), SceneDocumentKind::Scene, declared, options, &result, &error));
		CHECK(declared.Entities == 0);
		CHECK(result.Entities == 0);
		CHECK(error.find("1000 entities") != std::string::npos);
		CHECK_FALSE(declared.Finished);

		CountingSink undeclared;
		CHECK_FALSE(SceneDocumentReader::Read(MakeEntityListText(1000, false), SceneDocumentKind::Scene, undeclared, options, &result, &error));
		CHECK(undeclared.Entities == 100);
		CHECK(error.find("more than the 100") != std::string::npos);

		// At the limit the document loads, its declared count reserved.
		CountingSink fits;
		CHECK(SceneDocumentReader::Read(MakeEntityListText(100, true), SceneDocumentKind::Scene, fits, options, &result, &error));
		CHECK(fits.Entities == 100);
		CHECK(fits.Reserved == 100);
		CHECK(fits.Finished);
		CHECK(fits.Fields == std::vector<std::string> { "Name" });
		// A count the text cannot hold reserves only what it could.
		CountingSink claims;
		const std::string text = "{" + Header() + ",\"Scene\":{\"EntityCount\":1000000,\"Entities\":[{}]}}";
		CHECK(SceneDocumentReader::Read(text, SceneDocumentKind::Scene, claims, {}, &result, &error));
		CHECK(claims.Reserved <= text.size() / 3);

		// Scene loads take the limit from the scene.
		options.MaxEntities = 99;
		CHECK_FALSE(ReadStreamed(MakeEntityListText(100, true), options).Success);
		options.MaxEntities = 100;
		const SceneRead loaded = ReadStreamed(MakeEntityListText(100, true), options);
		REQUIRE(loaded.Success);
		CHECK(loaded.Loaded->GetEntityCount() == 100);
	}

	TEST_CASE("A cancelled read stops within one check of entities after the cancel, as cancelled, and finishes nothing")
	{
		constexpr size_t c_Count = 5000;
		const std::string text = MakeEntityListText(c_Count, true);
		for (const size_t percent : { 10, 50, 90 })
		{
			const uint64_t cancelAt = c_Count * percent / 100;
			CAPTURE(cancelAt);
			bool cancelled = false;
			CountingSink sink;
			sink.OnEntity = [&](uint64_t entity) { cancelled = cancelled || entity == cancelAt; };
			SceneReadOptions options;
			options.IsCancelled = [&cancelled]() { return cancelled; };
			SceneReadResult result;
			std::string error;
			CHECK_FALSE(SceneDocumentReader::Read(text, SceneDocumentKind::Scene, sink, options, &result, &error));
			CHECK(result.Cancelled);
			CHECK_FALSE(result.EntityError);
			// The reader asks every CancelCheckEntities entities: it stops at the first check after the cancel.
			const uint64_t interval = options.CancelCheckEntities;
			CHECK(sink.Entities == (cancelAt + interval - 1) / interval * interval);
			CHECK(sink.Entities - cancelAt < interval);
			CHECK_FALSE(sink.Finished);
		}

		// A read cancelled before it starts reads nothing.
		SceneReadOptions options;
		options.IsCancelled = []() { return true; };
		const SceneRead read = ReadStreamed(text, options);
		CHECK_FALSE(read.Success);
		CHECK(read.Result.Cancelled);
		CHECK(read.Loaded->GetEntityCount() == 0);
	}
}

namespace
{

	// The scene's document with each entity's ID replaced by its place in hierarchy order: scenes instantiated from equal
	// prototypes with fresh IDs compare equal, references between their entities (properties and script fields) included.
	std::string DescribeByPlace(const Scene& scene)
	{
		std::string text = JsonUtils::Dump(SceneSerializer::Serialize(scene));
		size_t place = 0;
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
		{
			const std::string id = entity.GetUUID().ToString();
			const std::string name = fmt::format("Entity#{}", place++);
			for (size_t found = text.find(id); found != std::string::npos; found = text.find(id, found + name.size()))
				text.replace(found, id.size(), name);
		}
		return text;
	}

	std::string InstantiateAndDescribe(const Ref<EntityPrototype>& prototype)
	{
		Scene scene("Instances");
		std::string error;
		INFO(error);
		REQUIRE_FALSE(scene.Instantiate(prototype, InstantiationOptions {}, &error).empty());
		return DescribeByPlace(scene);
	}

	// Two prototypes hold the same entities in the same order: IDs, parents, components and their values, unknown components,
	// and the references between their entities (which instances point at the copies).
	void CheckSamePrototype(const Ref<EntityPrototype>& built, const Ref<EntityPrototype>& compiled)
	{
		REQUIRE(built);
		REQUIRE(compiled);
		REQUIRE(built->GetEntityCount() == compiled->GetEntityCount());
		CHECK(std::ranges::equal(built->GetParentIndices(), compiled->GetParentIndices()));
		CHECK(std::ranges::equal(built->GetTopLevelIndices(), compiled->GetTopLevelIndices()));
		for (size_t index = 0; index < built->GetEntityCount(); index++)
		{
			CAPTURE(index);
			CHECK(built->GetEntityID(index) == compiled->GetEntityID(index));
			CHECK(built->FindEntity(built->GetEntityID(index)) == index);
			const std::span<const ComponentInfo* const> builtInfos = built->GetComponents(index);
			REQUIRE(std::ranges::equal(builtInfos, compiled->GetComponents(index)));
			for (size_t slot = 0; slot < builtInfos.size(); slot++)
			{
				CAPTURE(builtInfos[slot]->Name);
				CHECK(ComponentAccess::Serialize(*builtInfos[slot], built->GetComponentValues(index)[slot])
					== ComponentAccess::Serialize(*builtInfos[slot], compiled->GetComponentValues(index)[slot]));
			}
			const nlohmann::json* builtUnknown = built->GetUnknownComponents(index);
			const nlohmann::json* compiledUnknown = compiled->GetUnknownComponents(index);
			REQUIRE((builtUnknown == nullptr) == (compiledUnknown == nullptr));
			if (builtUnknown)
				CHECK(*builtUnknown == *compiledUnknown);
		}
		CHECK(built->Serialize() == compiled->Serialize());
		CHECK(InstantiateAndDescribe(built) == InstantiateAndDescribe(compiled));
	}

	// What EntityPrototype::Compile makes of a snapshot read into a scene (the way prototypes were made before
	// EntityPrototypeBuilder), with the warnings of that read.
	Ref<EntityPrototype> CompileThroughScene(const nlohmann::json& snapshot, std::vector<std::string>& outWarnings, std::string& outError)
	{
		Scene scene("Prototype");
		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = false;
		SceneSerializer::DeserializeEntities(scene, snapshot, options, &outError, &outWarnings);
		return outError.empty() ? EntityPrototype::Compile(scene) : nullptr;
	}

	std::span<const uint8_t> AsBytes(const std::string& text)
	{
		return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
	}

}

TEST_SUITE("Scene.Documents")
{
	TEST_CASE("EntityPrototypeBuilder makes what EntityPrototype::Compile makes of the document read into a scene, with the same reports")
	{
		const Ref<Scene> feature = LoadFeatureScene();
		AddUnknownComponents(*feature, feature->GetEntityByUUID(feature->GetRootEntities().front()));
		std::vector<Entity> roots;
		for (const UUID root : feature->GetRootEntities())
			roots.push_back(feature->GetEntityByUUID(root));
		nlohmann::json snapshot = SceneSerializer::SerializeEntities(*feature, roots);
		REQUIRE(snapshot["Entities"].size() == feature->GetEntityCount());

		std::vector<std::string> builtWarnings;
		std::vector<std::string> compiledWarnings;
		std::string builtError;
		std::string compiledError;
		CheckSamePrototype(EntityPrototype::FromSnapshot(snapshot, &builtError, &builtWarnings), CompileThroughScene(snapshot, compiledWarnings, compiledError));
		CHECK(builtWarnings == compiledWarnings);

		// Children before their parents: the prototype lists them in hierarchy order all the same.
		std::reverse(snapshot["Entities"].begin(), snapshot["Entities"].end());
		builtWarnings.clear();
		compiledWarnings.clear();
		CheckSamePrototype(EntityPrototype::FromSnapshot(snapshot, &builtError, &builtWarnings), CompileThroughScene(snapshot, compiledWarnings, compiledError));
		CHECK(builtWarnings == compiledWarnings);

		// Everything a read reports, in the same order.
		const nlohmann::json problems = { { "Entities", MakeProblemScene()["Scene"]["Entities"] } };
		builtWarnings.clear();
		compiledWarnings.clear();
		CheckSamePrototype(EntityPrototype::FromSnapshot(problems, &builtError, &builtWarnings), CompileThroughScene(problems, compiledWarnings, compiledError));
		CHECK_FALSE(builtWarnings.empty());
		CHECK(builtWarnings == compiledWarnings);

		// Invalid entity data fails with the same errors.
		for (const char* text : { R"({"Entities": {}})", R"({"Entities": [3]})", R"({"Entities": [{"ID": "zz"}]})",
			R"({"Entities": [{"ID": "0000000000000001"}, {"ID": "0000000000000001"}]})" })
		{
			INFO(text);
			const nlohmann::json invalid = nlohmann::json::parse(text);
			builtError.clear();
			compiledError.clear();
			CHECK_FALSE(EntityPrototype::FromSnapshot(invalid, &builtError));
			CHECK_FALSE(CompileThroughScene(invalid, compiledWarnings, compiledError));
			CHECK(builtError == compiledError);
		}
	}

	TEST_CASE("Prefab and model documents round trip through the streamed writer and reader, in old layouts too")
	{
		const Ref<Scene> feature = LoadFeatureScene();
		std::vector<UUID> roots(feature->GetRootEntities().begin(), feature->GetRootEntities().end());
		std::vector<Entity> rootEntities;
		for (const UUID root : roots)
			rootEntities.push_back(feature->GetEntityByUUID(root));
		const Ref<Prefab> source = Prefab::CreateFromEntities(*feature, rootEntities);
		REQUIRE(source);

		for (const SceneDocumentKind kind : { SceneDocumentKind::Prefab, SceneDocumentKind::Model })
		{
			const std::string format = SceneDocumentKindToString(kind);
			CAPTURE(format);
			SceneWriteOptions options;
			options.Kind = kind;
			options.Roots = roots;
			// The writer's layout, and the DOM's (keys sorted: the header after the entities), read to the same prototype.
			const std::string streamed = WriteDocument(*feature, options).Text;
			const nlohmann::json document = { { "Strata", { { "Format", format }, { "Version", 1 } } }, { format, source->GetSnapshot() } };
			for (const std::string& text : { streamed, JsonUtils::Dump(document, 1, '\t') })
			{
				std::string error;
				const Ref<EntityTemplate> read = kind == SceneDocumentKind::Prefab ? Ref<EntityTemplate>(Prefab::Deserialize(AsBytes(text), &error))
					: Ref<EntityTemplate>(Model::Deserialize(AsBytes(text), &error));
				INFO(error);
				REQUIRE(read);
				CheckSamePrototype(read->GetPrototype(), source->GetPrototype());
			}

			// Documents that are not of the kind, or whose entities are invalid, fail with the DOM readers' errors.
			const std::string header = Header(format);
			const std::vector<std::pair<std::string, std::string>> invalid = {
				{ "{\"" + format + "\":{\"Entities\":[]}}", "Not a Strata " + format + " document" },
				{ "{" + Header("Scene") + ",\"" + format + "\":{\"Entities\":[]}}", "Not a Strata " + format + " document" },
				{ "{" + Header(format, 9) + ",\"" + format + "\":{\"Entities\":[]}}", "Unsupported " + format + " version 9" },
				{ "{" + header + "}", format + " document has no entity list" },
				{ "{" + header + ",\"" + format + "\":{}}", format + " document has no entity list" },
				{ "{" + header + ",\"" + format + "\":{\"Entities\":[{\"ID\":\"0000000000000001\"},{\"ID\":\"0000000000000001\"}]}}",
					fmt::format("Invalid {} entities: Duplicate entity ID 0000000000000001 in entity data", kind == SceneDocumentKind::Prefab ? "prefab" : "model") },
			};
			for (const auto& [text, expected] : invalid)
			{
				INFO(text);
				std::string error;
				const bool loaded = kind == SceneDocumentKind::Prefab ? Prefab::Deserialize(AsBytes(text), &error) != nullptr
					: Model::Deserialize(AsBytes(text), &error) != nullptr;
				CHECK_FALSE(loaded);
				CHECK(error == expected);
			}
		}
	}
}

namespace
{

	// Options that read any document in the writer's layout in parallel, in chunks of `chunk` entities.
	SceneReadOptions ParallelOptions(uint32_t chunk)
	{
		SceneReadOptions options;
		options.ParallelMinEntities = 0;
		options.ParallelChunkEntities = chunk;
		return options;
	}

	SceneReadOptions StreamOptions()
	{
		SceneReadOptions options;
		options.Parallel = false;
		return options;
	}

	// The parallel read and the stream read load the text to the same scene, with the same reports; the parallel one took
	// the parallel path.
	void CheckSameParallelRead(const std::string& text, uint32_t chunk)
	{
		const SceneRead streamed = ReadStreamed(text, StreamOptions());
		const SceneRead parallel = ReadStreamed(text, ParallelOptions(chunk));
		INFO(streamed.Error);
		INFO(parallel.Error);
		REQUIRE(streamed.Success);
		REQUIRE(parallel.Success);
		CHECK_FALSE(streamed.Result.Parallel);
		CHECK(parallel.Result.Parallel);
		CHECK(parallel.Result.Entities == streamed.Result.Entities);
		CHECK(SceneSerializer::Serialize(*parallel.Loaded) == SceneSerializer::Serialize(*streamed.Loaded));
		CHECK(parallel.Loaded->GetName() == streamed.Loaded->GetName());
		CHECK(parallel.Loaded->GetRootEntities() == streamed.Loaded->GetRootEntities());
		CHECK(parallel.Warnings == streamed.Warnings);
	}

}

TEST_SUITE("Scene.Documents")
{
	TEST_CASE("Documents in the writer's layout read in parallel to the scene a stream read makes, with the same reports")
	{
		ScopedJobSystem jobs(4, 1);
		const Ref<Scene> feature = LoadFeatureScene();
		AddUnknownComponents(*feature, feature->GetEntityByUUID(feature->GetRootEntities().front()));
		const std::string written = WriteDocument(*feature).Text;
		for (const uint32_t chunk : { 1u, 3u, 1000u })
			CheckSameParallelRead(written, chunk);
		// Everything a read reports, over chunks; and a file checked out with Windows line ends.
		const std::string problems = WriteInWriterOrder(MakeProblemScene());
		CheckSameParallelRead(problems, 2);
		std::string crlf;
		for (const char character : written)
			crlf += character == '\n' ? std::string("\r\n") : std::string(1, character);
		CheckSameParallelRead(crlf, 4);
		CheckSameParallelRead(MakeEntityListText(5000, true), 256);

		// Large documents take the parallel path by default; small ones are read as a stream.
		CHECK(ReadStreamed(MakeEntityListText(SceneReadOptions().ParallelMinEntities, true)).Result.Parallel);
		CHECK_FALSE(ReadStreamed(written).Result.Parallel);
	}

	TEST_CASE("A parallel read fails as the stream read does, falls back to it for lines that do not parse alone, and is refused or cancelled by the chunk")
	{
		ScopedJobSystem jobs(4, 1);
		// Invalid entities in a later chunk: the stream read's errors.
		std::string text = MakeEntityListText(40, false);
		for (const auto& [from, to] : { std::pair<std::string, std::string> { "{\"Components\":{\"Name\":{\"Name\":\"E30\"}},\"ID\":\"000000000000001F\"", "{\"Components\":{\"Name\":{\"Name\":\"E30\"}},\"ID\":\"0000000000000003\"" },
			std::pair<std::string, std::string> { "{\"Components\":{\"Name\":{\"Name\":\"E30\"}},\"ID\":\"000000000000001F\"", "{\"Components\":{\"Name\":{\"Name\":\"E30\"}},\"ID\":\"zz\"" } })
		{
			std::string broken = text;
			const size_t at = broken.find(from);
			REQUIRE(at != std::string::npos);
			broken.replace(at, from.size(), to);
			const SceneRead streamed = ReadStreamed(broken, StreamOptions());
			const SceneRead parallel = ReadStreamed(broken, ParallelOptions(8));
			CHECK_FALSE(streamed.Success);
			CHECK_FALSE(parallel.Success);
			CHECK(parallel.Result.Parallel);
			CHECK(parallel.Error == streamed.Error);
			CHECK(parallel.Result.EntityError);
		}

		// Two entities on one line: valid JSON, read as a stream.
		std::string shared = MakeEntityListText(40, false);
		const size_t lineEnd = shared.find(",\n{", shared.find("\"Entities\":[\n") + 14);
		REQUIRE(lineEnd != std::string::npos);
		shared.replace(lineEnd, 2, ",");
		const SceneRead fallback = ReadStreamed(shared, ParallelOptions(8));
		REQUIRE(fallback.Success);
		CHECK_FALSE(fallback.Result.Parallel);
		CHECK(fallback.Loaded->GetEntityCount() == 40);
		// Not even valid JSON: the stream read's error.
		std::string invalid = MakeEntityListText(40, false);
		invalid.insert(invalid.find("\"E20\""), "\"");
		const SceneRead broken = ReadStreamed(invalid, ParallelOptions(8));
		CHECK_FALSE(broken.Success);
		CHECK_FALSE(broken.Result.Parallel);
		CHECK(broken.Result.SyntaxError);

		// Over the limit: refused before any entity, declared or not.
		for (const bool declare : { true, false })
		{
			SceneReadOptions options = ParallelOptions(8);
			options.MaxEntities = 30;
			CountingSink sink(true);
			std::string error;
			SceneReadResult result;
			CHECK_FALSE(SceneDocumentReader::Read(MakeEntityListText(40, declare), SceneDocumentKind::Scene, sink, options, &result, &error));
			CHECK(sink.Entities == 0);
			CHECK(error.find(declare ? "40 entities" : "more than the 30") != std::string::npos);
		}

		// Cancelled: it stops after the chunk in which the cancel came.
		const std::string many = MakeEntityListText(5000, true);
		for (const uint64_t cancelAt : { 500u, 2500u, 4500u })
		{
			CAPTURE(cancelAt);
			bool cancelled = false;
			CountingSink sink(true);
			sink.OnEntity = [&](uint64_t entity) { cancelled = cancelled || entity == cancelAt; };
			SceneReadOptions options = ParallelOptions(256);
			options.IsCancelled = [&cancelled]() { return cancelled; };
			SceneReadResult result;
			std::string error;
			CHECK_FALSE(SceneDocumentReader::Read(many, SceneDocumentKind::Scene, sink, options, &result, &error));
			CHECK(result.Parallel);
			CHECK(result.Cancelled);
			CHECK(sink.Entities == (cancelAt + 255) / 256 * 256);
			CHECK_FALSE(sink.Finished);
		}
	}
}
