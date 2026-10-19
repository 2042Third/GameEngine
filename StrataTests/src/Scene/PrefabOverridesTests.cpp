#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"

#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/PrefabOverrides.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/Scripting/ScriptingComponents.h>

#include <glm/gtc/epsilon.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	const AssetHandle c_Prefab = AssetHandle(0x4321);

	// A crate with a lid (a point light) and a handle on the lid, linked to c_Prefab as its own template (ids = UUIDs), and
	// the crate's part of the prefab.
	struct Crate
	{
		Ref<Scene> World = CreateRef<Scene>();
		Entity Root, Lid, Handle;
		nlohmann::json Part;

		Crate()
		{
			Root = World->CreateEntity("Crate");
			Lid = World->CreateChildEntity(Root, "Lid");
			Handle = World->CreateChildEntity(Lid, "Handle");
			Lid.AddComponent<PointLightComponent>().Intensity = 2.0f;
			const nlohmann::json snapshot = SceneSerializer::SerializeEntities(*World, { Root });
			PrefabInstances::Link(*World, { Root }, c_Prefab);
			Part = PrefabInstances::ExtractTemplate(snapshot, Root.GetUUID());
			World->UpdateWorldTransforms();
		}

		std::vector<PrefabOverride> Compute() const { return PrefabOverrides::ComputeOverrides(*World, Root, Part); }

		// The part with one value of an entity changed (what the prefab's file would hold after an edit).
		nlohmann::json PartWith(Entity entity, const std::string& component, const std::string& property, const nlohmann::json& value) const
		{
			nlohmann::json part = Part;
			for (nlohmann::json& candidate : part["Entities"])
			{
				if (candidate["ID"] == UUIDToJson(entity.GetUUID()))
					candidate["Components"][component][property] = value;
			}
			return part;
		}
	};

	void SetIntensity(Entity entity, float value)
	{
		const ComponentInfo* info = ComponentRegistry::Find("PointLight");
		REQUIRE(info);
		REQUIRE(ComponentAccess::SetProperty(entity, *info, *info->FindProperty("Intensity"), PropertyValue(value)));
	}

}

TEST_SUITE("Scene.PrefabOverrides")
{
	TEST_CASE("An unchanged instance has no overrides; the root's transform and name never count")
	{
		Crate crate;
		CHECK(crate.Compute().empty());
		crate.Root.GetTransform().Translation = glm::vec3(5.0f, 0.0f, 0.0f);
		crate.Root.MarkModified<TransformComponent>();
		crate.Root.GetComponent<NameComponent>().Name = "Crate (1)";
		crate.Root.MarkModified<NameComponent>();
		CHECK(crate.Compute().empty());
		// Below the root they do.
		crate.Lid.GetTransform().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		crate.Lid.MarkModified<TransformComponent>();
		const std::vector<PrefabOverride> overrides = crate.Compute();
		REQUIRE(overrides.size() == 1);
		CHECK(overrides[0] == PrefabOverride { crate.Lid.GetUUID(), "Transform", "Translation", PrefabOverrideKind::Property });
	}

	TEST_CASE("Overrides name changed properties, added and removed components and removed entities")
	{
		Crate crate;
		SetIntensity(crate.Lid, 4.0f);
		crate.Handle.AddComponent<SpotLightComponent>();
		crate.Lid.RemoveComponent<PointLightComponent>();
		std::vector<PrefabOverride> overrides = crate.Compute();
		CHECK(std::find(overrides.begin(), overrides.end(), PrefabOverride { crate.Lid.GetUUID(), "PointLight", "", PrefabOverrideKind::RemovedComponent }) != overrides.end());
		CHECK(std::find(overrides.begin(), overrides.end(), PrefabOverride { crate.Handle.GetUUID(), "SpotLight", "", PrefabOverrideKind::AddedComponent }) != overrides.end());
		CHECK(overrides.size() == 2);

		// An entity the instance added is its own: no override. One it removed is.
		crate.World->CreateChildEntity(crate.Root, "Sticker");
		const UUID handle = crate.Handle.GetUUID();
		crate.World->DestroyEntities(std::vector<Entity> { crate.Handle });
		overrides = crate.Compute();
		CHECK(std::find(overrides.begin(), overrides.end(), PrefabOverride { handle, "", "", PrefabOverrideKind::RemovedEntity }) != overrides.end());
		CHECK(overrides.size() == 2);
		CHECK(PrefabOverrides::IsOverridden(overrides, handle, "Transform", "Translation"));
		CHECK(PrefabOverrides::IsOverridden(overrides, crate.Lid.GetUUID(), "PointLight", "Intensity"));
		CHECK_FALSE(PrefabOverrides::IsOverridden(overrides, crate.Lid.GetUUID(), "Transform", "Translation"));
	}

	TEST_CASE("Scripts are overridden field by field, and added or removed as classes")
	{
		Crate crate;
		ScriptComponent& scripts = crate.Lid.AddComponent<ScriptComponent>();
		scripts.Scripts.push_back(ScriptEntry { "Spinner", { ScriptFieldValue { "Speed", PropertyType::Float, PropertyValue(1.0f) } } });
		scripts.Scripts.push_back(ScriptEntry { "Blinker", {} });
		nlohmann::json snapshot = SceneSerializer::SerializeEntities(*crate.World, { crate.Root });
		PrefabInstances::StripLinks(snapshot);
		crate.Part = PrefabInstances::ExtractTemplate(snapshot, crate.Root.GetUUID());
		CHECK(crate.Compute().empty());

		ScriptComponent& changed = crate.Lid.GetComponent<ScriptComponent>();
		std::get<float>(changed.Scripts[0].Fields[0].Value) = 3.0f;
		changed.Scripts.erase(changed.Scripts.begin() + 1);
		changed.Scripts.push_back(ScriptEntry { "Wobbler", {} });
		std::vector<PrefabOverride> overrides = crate.Compute();
		const UUID lid = crate.Lid.GetUUID();
		CHECK(std::find(overrides.begin(), overrides.end(), PrefabOverride { lid, "Script", "Spinner.Speed", PrefabOverrideKind::Property }) != overrides.end());
		CHECK(std::find(overrides.begin(), overrides.end(), PrefabOverride { lid, "Script", "Blinker", PrefabOverrideKind::RemovedComponent }) != overrides.end());
		CHECK(std::find(overrides.begin(), overrides.end(), PrefabOverride { lid, "Script", "Wobbler", PrefabOverrideKind::AddedComponent }) != overrides.end());
		CHECK(overrides.size() == 3);
		CHECK(PrefabOverrides::IsOverridden(overrides, lid, "Script", "Blinker.Rate"));
		CHECK_FALSE(PrefabOverrides::IsOverridden(overrides, lid, "Script", "Spinner.Range"));

		// A field the prefab stores no value for (the class default) reverts by dropping the instance's value.
		crate.Lid.GetComponent<ScriptComponent>().Scripts[0].Fields.push_back(ScriptFieldValue { "Range", PropertyType::Float, PropertyValue(5.0f) });
		crate.Lid.MarkModified<ScriptComponent>();
		REQUIRE(PrefabOverrides::IsOverridden(crate.Compute(), lid, "Script", "Spinner.Range"));
		std::string error;
		REQUIRE_MESSAGE(PrefabOverrides::Revert(*crate.World, crate.Lid, crate.Part, "Script", "Spinner.Range", &error), error);
		CHECK(crate.Lid.GetComponent<ScriptComponent>().Scripts[0].FindField("Range") == nullptr);
		CHECK_FALSE(PrefabOverrides::IsOverridden(crate.Compute(), lid, "Script", "Spinner.Range"));
		REQUIRE_MESSAGE(PrefabOverrides::Revert(*crate.World, crate.Lid, crate.Part, "Script", "Spinner.Speed", &error), error);
		CHECK(std::get<float>(crate.Lid.GetComponent<ScriptComponent>().Scripts[0].FindField("Speed")->Value) == 1.0f);
		// A field of a script the instance added has nothing to revert to.
		CHECK_FALSE(PrefabOverrides::Revert(*crate.World, crate.Lid, crate.Part, "Script", "Wobbler.Speed", &error));
	}

	TEST_CASE("UpdateOverrides works out the entries of the entities it is given as ComputeOverrides does, and keeps the others")
	{
		Crate crate;
		// The lid's script aims at the handle, the root's at the lid: references between the instance's entities.
		crate.Lid.AddComponent<ScriptComponent>().Scripts.push_back(
			ScriptEntry { "Aim", { ScriptFieldValue { "Target", PropertyType::Entity, PropertyValue(crate.Handle.GetUUID()) } } });
		crate.Root.AddComponent<ScriptComponent>().Scripts.push_back(
			ScriptEntry { "Aim", { ScriptFieldValue { "Target", PropertyType::Entity, PropertyValue(crate.Lid.GetUUID()) } } });
		const nlohmann::json linked = SceneSerializer::SerializeEntities(*crate.World, { crate.Root });
		nlohmann::json snapshot = linked;
		PrefabInstances::StripLinks(snapshot);
		// An instance with UUIDs of its own, its references remapped to its entities.
		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = true;
		const std::vector<Entity> made = SceneSerializer::DeserializeEntities(*crate.World, linked, options);
		REQUIRE(made.size() == 1);
		Entity root = made[0];
		REQUIRE(root.GetChildren().size() == 1);
		Entity lid = root.GetChildren()[0];
		REQUIRE(lid.GetChildren().size() == 1);
		Entity handle = lid.GetChildren()[0];
		REQUIRE(root.GetUUID() != crate.Root.GetUUID());

		PrefabOverrides::IndexedPartCache parts;
		const PrefabOverrides::IndexedPart& part = parts.Get(snapshot, crate.Root.GetUUID());
		CHECK(part.Positions.size() == 3);
		CHECK(&parts.Get(snapshot, crate.Root.GetUUID()) == &part);
		std::vector<PrefabOverride> list;
		const auto update = [&](std::vector<Entity> entities)
		{
			REQUIRE(PrefabOverrides::UpdateOverrides(root, part, entities, list));
			CHECK(list == PrefabOverrides::ComputeOverrides(*crate.World, root, part.Part));
		};
		// The reference compares as the prefab's handle.
		update({ lid });
		CHECK(list.empty());
		// The handle's entry, then the lid's before it (the part's order).
		handle.AddComponent<SpotLightComponent>();
		update({ handle });
		SetIntensity(lid, 5.0f);
		update({ lid });
		REQUIRE(list.size() == 2);
		CHECK(list[0] == PrefabOverride { crate.Lid.GetUUID(), "PointLight", "Intensity", PrefabOverrideKind::Property });
		// The root's transform is its own, and its reference to the lid compares as the prefab's lid.
		root.GetComponent<TransformComponent>().Translation = glm::vec3(3.0f, 0.0f, 0.0f);
		root.MarkModified<TransformComponent>();
		update({ root, root });
		CHECK(list.size() == 2);
		// Aimed at the handle instead, it overrides the field.
		root.GetComponent<ScriptComponent>().Scripts[0].Fields[0].Value = PropertyValue(handle.GetUUID());
		root.MarkModified<ScriptComponent>();
		update({ root });
		CHECK(list.size() == 3);
		CHECK(list[0] == PrefabOverride { crate.Root.GetUUID(), "Script", "Aim.Target", PrefabOverrideKind::Property });
		root.GetComponent<ScriptComponent>().Scripts[0].Fields[0].Value = PropertyValue(lid.GetUUID());
		root.MarkModified<ScriptComponent>();
		update({ root });
		CHECK(list.size() == 2);
		// Back to the prefab's value clears the entry; aiming at the root overrides the field.
		SetIntensity(lid, 2.0f);
		lid.GetComponent<ScriptComponent>().Scripts[0].Fields[0].Value = PropertyValue(root.GetUUID());
		lid.MarkModified<ScriptComponent>();
		update({ lid });
		CHECK(list == std::vector<PrefabOverride> { { crate.Lid.GetUUID(), "Script", "Aim.Target", PrefabOverrideKind::Property },
			{ crate.Handle.GetUUID(), "SpotLight", "", PrefabOverrideKind::AddedComponent } });

		// A copy of the lid made within the instance keeps its link: it counts as added there, as in ComputeOverrides.
		EntityInstantiationOptions copyOptions;
		copyOptions.GenerateNewUUIDs = true;
		copyOptions.Parent = root;
		const std::vector<Entity> copies = SceneSerializer::DeserializeEntities(*crate.World, SceneSerializer::SerializeEntities(*crate.World, { lid }), copyOptions);
		REQUIRE(copies.size() == 1);
		REQUIRE(copies[0].HasComponent<PrefabInstanceComponent>());
		SetIntensity(copies[0], 7.0f);
		const std::vector<PrefabOverride> beforeCopy = list;
		update({ copies[0], crate.Lid });
		CHECK(list == beforeCopy);

		// A list naming an entity the part lacks was worked out against another version: it needs ComputeOverrides.
		std::vector<PrefabOverride> stale = { { UUID(0x99), "PointLight", "Intensity", PrefabOverrideKind::Property } };
		CHECK_FALSE(PrefabOverrides::UpdateOverrides(root, part, std::vector<Entity> { lid }, stale));
		CHECK(stale.size() == 1);
	}

	TEST_CASE("The list is stored on the root only when it has entries, and malformed entries are dropped on load")
	{
		Crate crate;
		const std::string without = SceneSerializer::SerializeEntities(*crate.World, { crate.Root }).dump();
		CHECK(without.find("Overrides") == std::string::npos);

		const std::vector<PrefabOverride> overrides = { { crate.Lid.GetUUID(), "PointLight", "Intensity", PrefabOverrideKind::Property },
			{ crate.Handle.GetUUID(), "", "", PrefabOverrideKind::RemovedEntity } };
		crate.Root.GetComponent<PrefabInstanceComponent>().Overrides = overrides;
		const nlohmann::json written = SceneSerializer::SerializeEntities(*crate.World, { crate.Root });
		const nlohmann::json& stored = written["Entities"][0]["Components"]["PrefabInstance"]["Overrides"];
		REQUIRE(stored.size() == 2);
		CHECK(stored[1]["Kind"] == "RemovedEntity");
		// Read back into another scene, the list is the same; templates and content hashes leave it out.
		Ref<Scene> copy = CreateRef<Scene>();
		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = false;
		const std::vector<Entity> roots = SceneSerializer::DeserializeEntities(*copy, written, options);
		REQUIRE(roots.size() == 1);
		CHECK(roots[0].GetComponent<PrefabInstanceComponent>().Overrides == overrides);
		std::unordered_map<UUID, UUID> ids;
		CHECK(PrefabInstances::MakeTemplate(*crate.World, crate.Root, nullptr, nullptr, ids).dump().find("Overrides") == std::string::npos);
		const uint64_t hash = PrefabInstances::GetContentHash(*crate.World, crate.Root);
		crate.Root.GetComponent<PrefabInstanceComponent>().Overrides.clear();
		CHECK(PrefabInstances::GetContentHash(*crate.World, crate.Root) == hash);

		std::vector<std::string> warnings;
		const nlohmann::json list = { { { "Entity", "0000000000000005" }, { "Component", "Light" }, { "Property", "Color" }, { "Kind", "Property" } },
			{ { "Entity", "zz" }, { "Component", "Light" }, { "Property", "Color" }, { "Kind", "Property" } },
			{ { "Entity", "0000000000000005" }, { "Component", "Light" }, { "Property", "Color" }, { "Kind", "Renamed" } },
			"not an entry" };
		const std::vector<PrefabOverride> read = PrefabOverrides::FromJson(list, &warnings);
		CHECK(read.size() == 1);
		CHECK(warnings.size() == 3);
		CHECK(PrefabOverrides::FromJson(PrefabOverrides::ToJson(overrides)) == overrides);
	}

	TEST_CASE("Propagate gives every instance the prefab's changes except what it overrides")
	{
		Crate crate;
		// The instance overrides the lid's color; the prefab changes the lid's intensity and color, and the handle's name.
		const ComponentInfo* light = ComponentRegistry::Find("PointLight");
		REQUIRE(ComponentAccess::SetProperty(crate.Lid, *light, *light->FindProperty("Color"), PropertyValue(glm::vec3(1.0f, 0.0f, 0.0f))));
		crate.Root.GetComponent<PrefabInstanceComponent>().Overrides = crate.Compute();
		REQUIRE(crate.Root.GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);
		nlohmann::json next = crate.PartWith(crate.Lid, "PointLight", "Intensity", 7.0);
		for (nlohmann::json& entity : next["Entities"])
		{
			if (entity["ID"] == UUIDToJson(crate.Lid.GetUUID()))
				entity["Components"]["PointLight"]["Color"] = { 0.0, 0.0, 1.0 };
			if (entity["ID"] == UUIDToJson(crate.Handle.GetUUID()))
				entity["Components"]["Name"]["Name"] = "Grip";
		}
		PrefabOverrides::PropagateResult result;
		std::vector<UUID> touched;
		std::string error;
		REQUIRE_MESSAGE(PrefabOverrides::Propagate(*crate.World, crate.Root, crate.Part, next, result, [&](UUID id) { touched.push_back(id); }, &error), error);
		const std::vector<UUID>& changed = result.Changed;
		// The hash MarkSynced records, from the template Propagate took instead of serializing the instance again.
		CHECK(result.ContentHash == PrefabInstances::GetContentHash(*crate.World, crate.Root));
		CHECK(crate.Lid.GetComponent<PointLightComponent>().Intensity == 7.0f);
		CHECK(crate.Lid.GetComponent<PointLightComponent>().Color == glm::vec3(1.0f, 0.0f, 0.0f)); // Overridden
		CHECK(crate.Handle.GetName() == "Grip");
		CHECK(std::find(changed.begin(), changed.end(), crate.Lid.GetUUID()) != changed.end());
		CHECK(std::find(changed.begin(), changed.end(), crate.Handle.GetUUID()) != changed.end());
		CHECK(std::find(changed.begin(), changed.end(), crate.Root.GetUUID()) == changed.end()); // Its list stayed the same
		CHECK(touched.size() == changed.size());
		// Still one override, against the new part.
		CHECK(crate.Root.GetComponent<PrefabInstanceComponent>().Overrides
			== std::vector<PrefabOverride> { { crate.Lid.GetUUID(), "PointLight", "Color", PrefabOverrideKind::Property } });
	}

	TEST_CASE("Propagate adds and removes what the prefab added and removed, except where the instance overrides it")
	{
		Crate crate;
		// The instance removed the handle; the prefab adds a sticker below the lid, a spot light on the lid, and removes the
		// lid's point light.
		const UUID handle = crate.Handle.GetUUID();
		crate.World->DestroyEntities(std::vector<Entity> { crate.Handle });
		crate.Root.GetComponent<PrefabInstanceComponent>().Overrides = crate.Compute();
		nlohmann::json next = crate.Part;
		const UUID sticker = UUID(0x5717);
		SpotLightComponent spot;
		spot.Intensity = 3.0f;
		const nlohmann::json spotValues = ComponentAccess::Serialize(*ComponentRegistry::Find("SpotLight"), &spot);
		for (nlohmann::json& entity : next["Entities"])
		{
			if (entity["ID"] == UUIDToJson(crate.Lid.GetUUID()))
			{
				entity["Components"].erase("PointLight");
				entity["Components"]["SpotLight"] = spotValues;
			}
			if (entity["ID"] == UUIDToJson(handle))
				entity["Components"]["Name"]["Name"] = "Grip";
		}
		// The sticker as the prefab's file would hold it: written by the serializer.
		Scene scratch;
		nlohmann::json stickerEntity = SceneSerializer::SerializeEntities(scratch, { scratch.CreateEntity("Sticker") })["Entities"][0];
		stickerEntity["ID"] = UUIDToJson(sticker);
		stickerEntity["Parent"] = UUIDToJson(crate.Lid.GetUUID());
		next["Entities"].push_back(stickerEntity);
		PrefabOverrides::PropagateResult result;
		std::string error;
		REQUIRE_MESSAGE(PrefabOverrides::Propagate(*crate.World, crate.Root, crate.Part, next, result, {}, &error), error);
		const std::vector<UUID>& created = result.Created;
		CHECK(result.ContentHash == PrefabInstances::GetContentHash(*crate.World, crate.Root));
		CHECK_FALSE(crate.Lid.HasComponent<PointLightComponent>());
		REQUIRE(crate.Lid.HasComponent<SpotLightComponent>());
		CHECK(crate.Lid.GetComponent<SpotLightComponent>().Intensity == 3.0f);
		// The removed handle stays removed; the sticker is made, linked to its prefab entity.
		CHECK_FALSE(crate.World->GetEntityByUUID(handle));
		REQUIRE(created.size() == 1);
		const Entity made = crate.World->GetEntityByUUID(created[0]);
		REQUIRE(made);
		CHECK(made.GetName() == "Sticker");
		CHECK(made.GetParent() == crate.Lid);
		CHECK(made.GetComponent<PrefabInstanceComponent>().PrefabEntityID == sticker);
		CHECK(crate.Root.GetComponent<PrefabInstanceComponent>().Overrides
			== std::vector<PrefabOverride> { { handle, "", "", PrefabOverrideKind::RemovedEntity } });
		// A part without the instance's root is refused.
		CHECK_FALSE(PrefabOverrides::Propagate(*crate.World, crate.Root, crate.Part, nlohmann::json { { "Entities", nlohmann::json::array() } }, result, {}, &error));
	}

	TEST_CASE("Propagate keeps what the instance added below an entity the prefab removed, in its place in the world")
	{
		Crate crate;
		// The instance put a sticker on the handle (an entity of its own); the prefab then removes the lid and the handle.
		crate.Lid.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 2.0f, 0.0f);
		crate.Lid.MarkModified<TransformComponent>();
		Entity sticker = crate.World->CreateChildEntity(crate.Handle, "Sticker");
		sticker.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
		sticker.MarkModified<TransformComponent>();
		crate.World->UpdateWorldTransforms();
		const glm::vec3 stickerWorld = glm::vec3(crate.World->GetWorldTransform(sticker)[3]);
		REQUIRE(stickerWorld == glm::vec3(1.0f, 2.0f, 0.0f));
		crate.Root.GetComponent<PrefabInstanceComponent>().Overrides = crate.Compute();
		REQUIRE(crate.Root.GetComponent<PrefabInstanceComponent>().Overrides
			== std::vector<PrefabOverride> { { crate.Lid.GetUUID(), "Transform", "Translation", PrefabOverrideKind::Property } });

		// The prefab's part is its root alone now.
		nlohmann::json next = { { "Entities", nlohmann::json::array({ crate.Part["Entities"][0] }) } };
		REQUIRE(next["Entities"][0]["ID"] == UUIDToJson(crate.Root.GetUUID()));
		const UUID lid = crate.Lid.GetUUID();
		const UUID handle = crate.Handle.GetUUID();
		const UUID stickerId = sticker.GetUUID();
		std::vector<UUID> tracked;
		PrefabOverrides::PropagateResult result;
		std::string error;
		REQUIRE_MESSAGE(PrefabOverrides::Propagate(*crate.World, crate.Root, crate.Part, next, result, [&](UUID id) { tracked.push_back(id); }, &error),
			error);
		CHECK_FALSE(crate.World->GetEntityByUUID(lid));
		CHECK_FALSE(crate.World->GetEntityByUUID(handle));
		// The sticker stays in the instance, below its nearest entity that stays, where it was in the world; undo can put it
		// back (it was tracked before it moved).
		REQUIRE(crate.World->GetEntityByUUID(stickerId));
		CHECK(sticker.GetParent() == crate.Root);
		crate.World->UpdateWorldTransforms();
		const glm::vec3 after = glm::vec3(crate.World->GetWorldTransform(sticker)[3]);
		CHECK(glm::all(glm::epsilonEqual(after, stickerWorld, 1e-5f)));
		CHECK(std::ranges::find(tracked, stickerId) != tracked.end());
		CHECK(std::ranges::find(result.Changed, stickerId) != result.Changed.end());
		// The lid's override went with the lid.
		CHECK(crate.Root.GetComponent<PrefabInstanceComponent>().Overrides.empty());
	}

	TEST_CASE("Propagate works out an unrecorded list from the instance, and an instance that overrides every change keeps its values")
	{
		Crate crate;
		// The instance changed the lid's intensity, but nothing recorded it (a scene written before overrides were kept).
		SetIntensity(crate.Lid, 9.0f);
		REQUIRE(crate.Root.GetComponent<PrefabInstanceComponent>().Overrides.empty());
		const uint64_t before = PrefabInstances::GetContentHash(*crate.World, crate.Root);
		PrefabOverrides::PropagateResult result;
		std::string error;
		REQUIRE_MESSAGE(PrefabOverrides::Propagate(*crate.World, crate.Root, crate.Part, crate.PartWith(crate.Lid, "PointLight", "Intensity", 4.0), result,
			{}, &error), error);
		CHECK(crate.Lid.GetComponent<PointLightComponent>().Intensity == 9.0f);
		// Only the root changed (it records its list now); the instance's values, and so its hash, are the ones it had.
		CHECK(result.Changed == std::vector<UUID> { crate.Root.GetUUID() });
		CHECK(result.Created.empty());
		CHECK(result.ContentHash == before);
		CHECK(crate.Root.GetComponent<PrefabInstanceComponent>().Overrides
			== std::vector<PrefabOverride> { { crate.Lid.GetUUID(), "PointLight", "Intensity", PrefabOverrideKind::Property } });
	}

	TEST_CASE("Revert and ApplyToPart work one component or property at a time")
	{
		Crate crate;
		SetIntensity(crate.Lid, 9.0f);
		crate.Lid.GetTransform().Translation = glm::vec3(0.0f, 2.0f, 0.0f);
		crate.Lid.MarkModified<TransformComponent>();
		REQUIRE(crate.Compute().size() == 2);
		std::string error;
		REQUIRE_MESSAGE(PrefabOverrides::Revert(*crate.World, crate.Lid, crate.Part, "PointLight", "Intensity", &error), error);
		CHECK(crate.Lid.GetComponent<PointLightComponent>().Intensity == 2.0f);
		CHECK(crate.Compute() == std::vector<PrefabOverride> { { crate.Lid.GetUUID(), "Transform", "Translation", PrefabOverrideKind::Property } });

		// Applying the translation writes only it into the part.
		std::unordered_map<UUID, UUID> ids;
		const nlohmann::json instance = PrefabInstances::MakeTemplate(*crate.World, crate.Root, nullptr, nullptr, ids);
		const nlohmann::json applied = PrefabOverrides::ApplyToPart(crate.Part, instance, crate.Lid.GetUUID(), "Transform", "Translation");
		crate.Part = applied;
		CHECK(crate.Compute().empty());

		// Reverting a whole component it lacks adds it back; one the prefab lacks goes.
		crate.Lid.RemoveComponent<PointLightComponent>();
		REQUIRE(PrefabOverrides::Revert(*crate.World, crate.Lid, crate.Part, "PointLight", "", &error));
		CHECK(crate.Lid.GetComponent<PointLightComponent>().Intensity == 2.0f);
		crate.Handle.AddComponent<SpotLightComponent>();
		REQUIRE(PrefabOverrides::Revert(*crate.World, crate.Handle, crate.Part, "SpotLight", "", &error));
		CHECK_FALSE(crate.Handle.HasComponent<SpotLightComponent>());
		CHECK(crate.Compute().empty());
		CHECK_FALSE(PrefabOverrides::Revert(*crate.World, crate.Handle, crate.Part, "PointLight", "Intensity", &error));
	}
}
