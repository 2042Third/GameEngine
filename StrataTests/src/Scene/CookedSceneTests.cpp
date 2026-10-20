#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "TestHelpers.h"

#if ST_TESTS_HAVE_EVERY_MODULE
	#include "FeatureTest/FeatureTestUtils.h"
#endif

#include <Strata/Core/BinaryStream.h>
#include <Strata/Core/Hash.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scripting/ScriptingComponents.h>
#include <Strata/Scene/CookedScene.h>
#include <Strata/Scene/EntityPrototype.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/Scene/UnknownComponents.h>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::CookedSceneFormat;

namespace
{

#if ST_TESTS_HAVE_EVERY_MODULE
	Ref<Scene> LoadFeatureScene()
	{
		std::string error;
		const Ref<Scene> scene = SceneSerializer::LoadFromFile(GetFeatureProjectSourceDirectory() / "Assets" / "Scenes" / "Feature.stscene", &error);
		INFO(error);
		REQUIRE(scene);
		// Kept components of a module this build lacks.
		UnknownComponentsComponent unknown;
		unknown.Components = { { "Turret", { { "Barrels", 2 }, { "Name", "Twin" } } }, { "Zz.Last", nlohmann::json::object() } };
		scene->GetRegistry().emplace<UnknownComponentsComponent>(scene->GetEntityByUUID(scene->GetRootEntities().front()).GetHandle(), std::move(unknown));
		// Settings other than the defaults.
		scene->GetSettings().Gravity = glm::vec3(0.0f, -3.5f, 1.0f);
		scene->GetSettings().MaxFixedStepsPerFrame = 3;
		return scene;
	}
#endif

	std::vector<uint8_t> Cook(const Scene& scene)
	{
		const Ref<EntityPrototype> prototype = EntityPrototype::Compile(scene);
		std::vector<uint8_t> bytes;
		std::string error;
		INFO(error);
		REQUIRE(CookedSceneWriter::Write(SceneDocumentKind::Scene, *prototype, scene.GetName(), SceneSerializer::SerializeSettings(scene.GetSettings()), bytes, &error));
		return bytes;
	}

	Header ReadHeader(const std::vector<uint8_t>& bytes)
	{
		Header header {};
		std::memcpy(&header, bytes.data(), sizeof(header));
		return header;
	}

	std::vector<Section> ReadTable(const std::vector<uint8_t>& bytes)
	{
		const Header header = ReadHeader(bytes);
		std::vector<Section> sections(header.SectionCount);
		std::memcpy(sections.data(), bytes.data() + header.SectionTableOffset, sections.size() * sizeof(Section));
		return sections;
	}

	// Rewrites the checksums of a cooked document whose fields a test changed, so that validation reaches those fields:
	// each section's (where it lies inside the document), the table's and the header's.
	void RewriteCookedChecksums(std::vector<uint8_t>& bytes)
	{
		Header header = ReadHeader(bytes);
		if (header.SectionTableOffset <= bytes.size() && header.SectionCount <= (bytes.size() - header.SectionTableOffset) / sizeof(Section))
		{
			std::vector<Section> sections = ReadTable(bytes);
			for (Section& section : sections)
			{
				if (section.Offset <= bytes.size() && section.Size <= bytes.size() - section.Offset)
					section.Crc = Hash::CRC32C(std::span<const uint8_t>(bytes.data() + section.Offset, static_cast<size_t>(section.Size)));
			}
			std::memcpy(bytes.data() + header.SectionTableOffset, sections.data(), sections.size() * sizeof(Section));
			header.SectionTableCrc = Hash::CRC32C(std::span<const uint8_t>(bytes.data() + header.SectionTableOffset, sections.size() * sizeof(Section)));
		}
		header.HeaderCrc = 0;
		header.HeaderCrc = Hash::CRC32C(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&header), sizeof(header)));
		std::memcpy(bytes.data(), &header, sizeof(header));
	}

	void SetHeader(std::vector<uint8_t>& bytes, const Header& header)
	{
		std::memcpy(bytes.data(), &header, sizeof(header));
		RewriteCookedChecksums(bytes);
	}

	void SetTable(std::vector<uint8_t>& bytes, const std::vector<Section>& sections)
	{
		std::memcpy(bytes.data() + ReadHeader(bytes).SectionTableOffset, sections.data(), sections.size() * sizeof(Section));
		RewriteCookedChecksums(bytes);
	}

	const Section& FindSection(const std::vector<Section>& sections, SectionKind kind, uint32_t index = 0)
	{
		for (const Section& section : sections)
		{
			if (section.Kind == static_cast<uint32_t>(kind) && section.Index == index)
				return section;
		}
		FAIL("No such section");
		return sections.front();
	}

	// The read refuses the bytes with an error (and does not crash).
	void CheckRefused(const std::vector<uint8_t>& bytes, const std::string& expected = {})
	{
		CAPTURE(bytes.size());
		std::string error;
		CHECK_FALSE(CookedSceneReader::Read(bytes, {}, &error).has_value());
		CHECK_FALSE(error.empty());
		if (!expected.empty())
		{
			INFO(error);
			CHECK(error.find(expected) != std::string::npos);
		}
	}

	// A small scene of every kind of content: a hierarchy, a tag, a script, an unknown component.
	Ref<Scene> MakeSmallScene()
	{
		Ref<Scene> scene = CreateRef<Scene>("Small");
		Entity root = scene->CreateEntity("Root");
		root.AddComponent<TagComponent>().Tag = "Enemy";
		Entity child = scene->CreateChildEntity(root, "Child");
		child.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 2.0f, 3.0f);
		ScriptEntry& script = child.AddComponent<ScriptComponent>().Scripts.emplace_back();
		script.ClassName = "Mover";
		script.Fields.push_back(ScriptFieldValue { "Target", PropertyType::Entity, PropertyValue(root.GetUUID()) });
		scene->CreateEntity("Other").AddComponent<InactiveComponent>();
		UnknownComponentsComponent unknown;
		unknown.Components = { { "Future", { { "Value", 1 } } } };
		scene->GetRegistry().emplace<UnknownComponentsComponent>(child.GetHandle(), std::move(unknown));
		return scene;
	}

}

TEST_SUITE("Scene.CookedScene")
{
#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("The feature scene through its cooked form equals its JSON load: components, script fields, unknown components, settings, IDs, hierarchy")
	{
		const Ref<Scene> feature = LoadFeatureScene();
		const std::vector<uint8_t> bytes = Cook(*feature);
		CHECK(CookedScene::IsCooked(bytes));
		std::vector<std::string> warnings;
		std::string error;
		const Ref<Scene> read = CookedSceneReader::ReadScene(bytes, {}, &warnings, &error);
		INFO(error);
		REQUIRE(read);
		CHECK(SceneSerializer::Serialize(*read) == SceneSerializer::Serialize(*feature));
		CHECK(read->GetRootEntities() == feature->GetRootEntities());
		CHECK(read->GetName() == feature->GetName());
		CHECK(warnings.empty());

		// The comparison covers what other streams added to the feature scene. The prefab instance's override list is extra
		// data with no binary form, kept as MessagePack; game-ui's components are registered ones like any other.
		const ComponentInfo* prefabInstance = ComponentRegistry::Find<PrefabInstanceComponent>();
		REQUIRE(prefabInstance);
		CHECK(prefabInstance->SerializeExtra);
		CHECK_FALSE(prefabInstance->SerializeExtraBinary);
		auto countOverrides = [](const Scene& scene)
		{
			size_t overrides = 0;
			for (const auto [handle, instance] : scene.GetRegistry().view<PrefabInstanceComponent>().each())
				overrides += instance.Overrides.size();
			return overrides;
		};
		CHECK(countOverrides(*feature) > 0);
		CHECK(countOverrides(*read) == countOverrides(*feature));
		const nlohmann::json readDocument = SceneSerializer::Serialize(*read);
		for (const char* component : { "UICanvas", "UIRect", "UIImage", "UIText", "UIButton", "UIToggle", "UISlider", "UIInputField", "UIMask", "UILayout" })
		{
			CAPTURE(component);
			size_t entities = 0;
			for (const nlohmann::json& entity : readDocument["Scene"]["Entities"])
				entities += entity["Components"].contains(component) ? 1 : 0;
			CHECK(entities > 0);
		}

		// Through the scene asset's loader, as packs load it.
		const Ref<SceneAsset> asset = SceneAsset::Deserialize(bytes, &error);
		REQUIRE(asset);
		CHECK(SceneSerializer::Serialize(*asset->CreateScene()) == SceneSerializer::Serialize(*feature));
	}
#endif

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("Prefabs and models through their cooked form equal their JSON load")
	{
		const Ref<Scene> feature = LoadFeatureScene();
		std::vector<Entity> roots;
		for (const UUID root : feature->GetRootEntities())
			roots.push_back(feature->GetEntityByUUID(root));
		const Ref<Prefab> prefab = Prefab::CreateFromEntities(*feature, roots);
		REQUIRE(prefab);
		for (const SceneDocumentKind kind : { SceneDocumentKind::Prefab, SceneDocumentKind::Model })
		{
			std::vector<uint8_t> bytes;
			std::string error;
			REQUIRE(CookedSceneWriter::Write(kind, *prefab->GetPrototype(), {}, nullptr, bytes, &error));
			const Ref<EntityTemplate> read = kind == SceneDocumentKind::Prefab ? Ref<EntityTemplate>(Prefab::Deserialize(bytes, &error))
				: Ref<EntityTemplate>(Model::Deserialize(bytes, &error));
			INFO(error);
			REQUIRE(read);
			CHECK(read->GetPrototype()->Serialize() == prefab->GetPrototype()->Serialize());
			CHECK(std::ranges::equal(read->GetPrototype()->GetParentIndices(), prefab->GetPrototype()->GetParentIndices()));
			// The kind is checked.
			const bool wrongKind = kind == SceneDocumentKind::Prefab ? Model::Deserialize(bytes, &error) != nullptr : Prefab::Deserialize(bytes, &error) != nullptr;
			CHECK_FALSE(wrongKind);
		}
	}
#endif

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("Cooked bytes depend only on the input, and reads give the same scene on any number of threads")
	{
		const Ref<Scene> feature = LoadFeatureScene();
		const std::vector<uint8_t> first = Cook(*feature);
		CHECK(Cook(*feature) == first);
		const Ref<Scene> copy = Scene::Copy(feature);
		CHECK(Cook(*copy) == first);

		nlohmann::json single;
		{
			const Ref<Scene> read = CookedSceneReader::ReadScene(first);
			REQUIRE(read);
			single = SceneSerializer::Serialize(*read);
		}
		ScopedJobSystem jobs(6, 1);
		const Ref<Scene> parallel = CookedSceneReader::ReadScene(first);
		REQUIRE(parallel);
		CHECK(SceneSerializer::Serialize(*parallel) == single);
	}
#endif

	TEST_CASE("Truncated, damaged and inconsistent cooked documents are refused with an error")
	{
		const Ref<Scene> smallScene = MakeSmallScene();
		const std::vector<uint8_t> bytes = Cook(*smallScene);
		REQUIRE(CookedSceneReader::Read(bytes).has_value());

		// Truncated at every byte.
		for (size_t size = 0; size < bytes.size(); size++)
			CheckRefused(std::vector<uint8_t>(bytes.begin(), bytes.begin() + size));

		// Every single bit of the header and of the section table flipped.
		const Header header = ReadHeader(bytes);
		std::vector<size_t> checked;
		for (size_t offset = 0; offset < sizeof(Header); offset++)
			checked.push_back(offset);
		for (size_t offset = header.SectionTableOffset; offset < bytes.size(); offset++)
			checked.push_back(offset);
		for (const size_t offset : checked)
		{
			for (int bit = 0; bit < 8; bit++)
			{
				std::vector<uint8_t> flipped = bytes;
				flipped[offset] ^= static_cast<uint8_t>(1u << bit);
				CheckRefused(flipped);
			}
		}

		// Huge counts and offsets, the checksums rewritten so that validation reaches them.
		const uint64_t huge = std::numeric_limits<uint64_t>::max() / 2;
		for (int field = 0; field < 7; field++)
		{
			Header changed = header;
			switch (field)
			{
				case 0: changed.EntityCount = std::numeric_limits<uint32_t>::max(); break;
				case 1: changed.ComponentTypeCount = std::numeric_limits<uint32_t>::max(); break;
				case 2: changed.StringCount = std::numeric_limits<uint32_t>::max(); break;
				case 3: changed.StringBytes = huge; break;
				case 4: changed.SectionTableOffset = huge; break;
				case 5: changed.SectionCount = std::numeric_limits<uint32_t>::max(); break;
				case 6: changed.Flags = 1; break;
			}
			std::vector<uint8_t> damaged = bytes;
			SetHeader(damaged, changed);
			CheckRefused(damaged);
		}
		const std::vector<Section> sections = ReadTable(bytes);
		for (size_t index = 0; index < sections.size(); index++)
		{
			for (int field = 0; field < 4; field++)
			{
				std::vector<Section> changed = sections;
				switch (field)
				{
					case 0: changed[index].Offset = huge; break;
					case 1: changed[index].Size = huge; break;
					case 2: changed[index].Count = std::numeric_limits<uint32_t>::max(); break;
					case 3: changed[index].Offset += 8; break; // Unaligned
				}
				std::vector<uint8_t> damaged = bytes;
				SetTable(damaged, changed);
				CheckRefused(damaged);
			}
		}
		// Overlapping sections.
		{
			std::vector<Section> changed = sections;
			changed[1].Offset = changed[0].Offset;
			std::vector<uint8_t> damaged = bytes;
			SetTable(damaged, changed);
			CheckRefused(damaged);
		}

		// The contents: IDs null or twice, parents not before their entity, column indices unsorted or out of range, a property's
		// type changed, an oversized MessagePack.
		auto patch = [&](SectionKind kind, uint32_t index, size_t offset, const void* value, size_t size)
		{
			std::vector<uint8_t> damaged = bytes;
			const std::vector<Section> table = ReadTable(damaged);
			const Section& section = FindSection(table, kind, index);
			REQUIRE(offset + size <= section.Size);
			std::memcpy(damaged.data() + section.Offset + offset, value, size);
			RewriteCookedChecksums(damaged);
			return damaged;
		};
		const uint64_t nullID = 0;
		CheckRefused(patch(SectionKind::EntityIDs, 0, 8, &nullID, sizeof(nullID)), "null entity ID");
		uint64_t firstID = 0;
		std::memcpy(&firstID, bytes.data() + FindSection(sections, SectionKind::EntityIDs).Offset, sizeof(firstID));
		CheckRefused(patch(SectionKind::EntityIDs, 0, 8, &firstID, sizeof(firstID)), "twice");
		const uint32_t ownIndex = 1;
		CheckRefused(patch(SectionKind::Parents, 0, 4, &ownIndex, sizeof(ownIndex)), "does not come before");
		// The Tag column is sparse (the root only): its index list follows its flags and count.
		uint32_t tagColumn = std::numeric_limits<uint32_t>::max();
		{
			BinaryReader reader(std::span<const uint8_t>(bytes.data() + FindSection(sections, SectionKind::ComponentTypes).Offset,
				static_cast<size_t>(FindSection(sections, SectionKind::ComponentTypes).Size)));
			const Section& stringsSection = FindSection(sections, SectionKind::Strings);
			BinaryReader strings(std::span<const uint8_t>(bytes.data() + stringsSection.Offset, static_cast<size_t>(stringsSection.Size)));
			std::vector<uint32_t> offsets(stringsSection.Count + 1);
			for (uint32_t& offset : offsets)
				offset = strings.Read<uint32_t>();
			const char* text = reinterpret_cast<const char*>(bytes.data() + stringsSection.Offset + offsets.size() * sizeof(uint32_t));
			for (uint32_t type = 0; type < header.ComponentTypeCount; type++)
			{
				const uint32_t name = reader.Read<uint32_t>();
				const uint32_t properties = reader.Read<uint32_t>();
				reader.Read<uint32_t>();
				for (uint32_t property = 0; property < properties; property++)
				{
					reader.Read<uint32_t>();
					reader.Read<uint32_t>();
				}
				if (std::string_view(text + offsets[name], offsets[name + 1] - offsets[name]) == "Tag")
					tagColumn = type;
			}
		}
		REQUIRE(tagColumn != std::numeric_limits<uint32_t>::max());
		const uint32_t outOfRange = 99;
		CheckRefused(patch(SectionKind::Column, tagColumn, 8, &outOfRange, sizeof(outOfRange)), "strictly increasing");
		const uint32_t dense = c_ColumnDense;
		CheckRefused(patch(SectionKind::Column, tagColumn, 0, &dense, sizeof(dense)), "dense");
		// The first type's first property (Name.Name, a String) becomes a Float.
		const uint32_t floatType = static_cast<uint32_t>(PropertyType::Float);
		CheckRefused(patch(SectionKind::ComponentTypes, 0, 16, &floatType, sizeof(floatType)), "in this build");
		// The unknown components' MessagePack claims more bytes than a cooked document holds.
		const uint32_t oversized = c_MaxBlobBytes + 1;
		CheckRefused(patch(SectionKind::UnknownComponents, 0, 4, &oversized, sizeof(oversized)), "unknown components");
	}
}
