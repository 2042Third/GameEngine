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
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneDocumentWriter.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
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

	struct WrittenDocument
	{
		std::string Text;
		SceneWriteResult Result;
	};

	WrittenDocument WriteDocument(const Scene& scene, const SceneWriteOptions& options = {})
	{
		StringDocumentOutput output;
		WrittenDocument document;
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
			const WrittenDocument document = WriteDocument(*scene, options);
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
		const WrittenDocument document = WriteDocument(*feature, options);
		std::vector<AssetHandle> scanned;
		std::string error;
		REQUIRE(AssetDependencies::Scan(AssetType::Scene, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(document.Text.data()),
			document.Text.size()), scanned, &error));
		CHECK(document.Result.References == scanned);
		// Script fields of asset type are among them.
		CHECK(std::find(scanned.begin(), scanned.end(), UUID(0xF7A0000000000005ull)) != scanned.end());

		// Without the extra-data scanner only asset properties count.
		const WrittenDocument plain = WriteDocument(*feature);
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
		const WrittenDocument document = WriteDocument(scene, options);
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
		const WrittenDocument empty = WriteDocument(scene, options);
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
			const WrittenDocument document = WriteDocument(scene, options);
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
			const WrittenDocument document = WriteDocument(world, options);
			CAPTURE(chunk);
			CHECK(document.Text == serial);
			CHECK(document.Result.Sha256 == Sha256::Hash(document.Text));
			CHECK(document.Result.ContentHash == Hash::FNV1a(std::string_view(document.Text)));
			REQUIRE_FALSE(progress.empty());
			CHECK(std::is_sorted(progress.begin(), progress.end()));
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
}
