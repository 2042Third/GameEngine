#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"

#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <string>
#include <unordered_map>
#include <vector>

using namespace Strata;

namespace
{

	const AssetHandle c_Prefab = AssetHandle(0x1234);

	// A crate with a lid and a handle on the lid, linked to c_Prefab as its own template (ids = UUIDs), with the snapshot.
	struct LinkedCrate
	{
		Ref<Scene> World = CreateRef<Scene>();
		Entity Crate, Lid, Handle;
		nlohmann::json Snapshot;

		LinkedCrate()
		{
			Entity holder = World->CreateEntity("Holder");
			Crate = World->CreateChildEntity(holder, "Crate");
			Lid = World->CreateChildEntity(Crate, "Lid");
			Handle = World->CreateChildEntity(Lid, "Handle");
			Lid.AddComponent<PointLightComponent>().Intensity = 2.0f;
			Snapshot = SceneSerializer::SerializeEntities(*World, { Crate });
			PrefabInstances::Link(*World, { Crate }, c_Prefab);
			World->UpdateWorldTransforms();
		}
	};

}

TEST_SUITE("Scene.PrefabInstances")
{
	TEST_CASE("Instance roots are linked entities whose parent is not of the same prefab")
	{
		LinkedCrate crate;
		CHECK(PrefabInstances::IsInstanceRoot(crate.Crate));
		CHECK_FALSE(PrefabInstances::IsInstanceRoot(crate.Lid));
		CHECK(PrefabInstances::FindInstanceRoot(crate.Handle) == crate.Crate);
		CHECK_FALSE(PrefabInstances::FindInstanceRoot(crate.Crate.GetParent()));
		CHECK(PrefabInstances::GetPrefab(crate.Lid) == c_Prefab);
		CHECK(PrefabInstances::FindInstanceRoots(*crate.World, c_Prefab) == std::vector<Entity> { crate.Crate });
		// A nested instance of another prefab is a root of its own.
		crate.Handle.AddOrReplaceComponent<PrefabInstanceComponent>(PrefabInstanceComponent { AssetHandle(0x99), UUID(5) });
		CHECK(PrefabInstances::IsInstanceRoot(crate.Handle));
	}

	TEST_CASE("Compare leaves the root's transform and name out and reports changed, added and removed entities")
	{
		LinkedCrate crate;
		CHECK(PrefabInstances::Compare(*crate.World, crate.Crate, crate.Snapshot).IsEmpty());
		crate.Crate.GetTransform().Translation = glm::vec3(3.0f, 0.0f, 1.0f);
		crate.Crate.MarkModified<TransformComponent>();
		crate.Crate.GetComponent<NameComponent>().Name = "Big Crate";
		crate.Crate.MarkModified<NameComponent>();
		CHECK(PrefabInstances::Compare(*crate.World, crate.Crate, crate.Snapshot).IsEmpty());

		crate.Lid.GetComponent<PointLightComponent>().Intensity = 4.0f;
		const Entity added = crate.World->CreateChildEntity(crate.Crate, "Sticker");
		const UUID handleId = crate.Handle.GetUUID();
		crate.World->DestroyEntities(std::vector<Entity> { crate.Handle });
		const PrefabInstanceDifference difference = PrefabInstances::Compare(*crate.World, crate.Crate, crate.Snapshot);
		CHECK(difference.Changed == std::vector<UUID> { crate.Lid.GetUUID() });
		CHECK(difference.Added == std::vector<UUID> { added.GetUUID() });
		CHECK(difference.Removed == std::vector<UUID> { handleId });
	}

	TEST_CASE("Replace gives the instance the template's values, keeping UUIDs and the root's place, transform and name")
	{
		LinkedCrate crate;
		Entity holder = crate.Crate.GetParent();
		Entity sibling = crate.World->CreateChildEntity(holder, "Sibling");
		crate.World->SetSiblingIndex(sibling, 0);
		crate.Crate.GetTransform().Translation = glm::vec3(1.0f, 2.0f, 3.0f);
		crate.Crate.MarkModified<TransformComponent>();
		crate.Lid.GetComponent<PointLightComponent>().Intensity = 9.0f;
		crate.World->CreateChildEntity(crate.Lid, "Extra");
		const UUID crateId = crate.Crate.GetUUID();
		const UUID lidId = crate.Lid.GetUUID();
		const UUID handleId = crate.Handle.GetUUID();
		crate.World->DestroyEntities(std::vector<Entity> { crate.Handle });

		std::vector<UUID> created;
		std::string error;
		REQUIRE(PrefabInstances::Replace(*crate.World, crate.World->GetEntityByUUID(crateId), crate.Snapshot, &created, &error));
		const Entity root = crate.World->GetEntityByUUID(crateId);
		REQUIRE(root);
		CHECK(root.GetParent() == holder);
		CHECK(crate.World->GetSiblingIndex(root) == 1);
		CHECK(root.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
		const Entity lid = crate.World->GetEntityByUUID(lidId);
		REQUIRE(lid);
		CHECK(lid.GetComponent<PointLightComponent>().Intensity == 2.0f);
		CHECK(lid.GetChildren().size() == 1); // Extra went, the handle came back with a new UUID
		REQUIRE(created.size() == 1);
		CHECK(created[0] != handleId);
		CHECK(lid.GetChildren()[0].GetUUID() == created[0]);
		CHECK(lid.GetChildren()[0].GetComponent<PrefabInstanceComponent>().PrefabEntityID == handleId);
		CHECK(PrefabInstances::Compare(*crate.World, root, crate.Snapshot).IsEmpty());
		crate.World->UpdateWorldTransforms();
		Tests::CheckSceneCaches(*crate.World);
	}

	TEST_CASE("Replace changes nothing when the template is missing or broken")
	{
		LinkedCrate crate;
		const nlohmann::json before = SceneSerializer::SerializeEntities(*crate.World, { crate.Crate.GetParent() });
		std::string error;
		CHECK_FALSE(PrefabInstances::Replace(*crate.World, crate.Crate, nlohmann::json { { "Entities", nlohmann::json::array() } }, nullptr, &error));
		CHECK(error.find("no longer has") != std::string::npos);

		// Two entities with one id.
		nlohmann::json broken = crate.Snapshot;
		broken["Entities"][2]["ID"] = broken["Entities"][1]["ID"];
		error.clear();
		CHECK_FALSE(PrefabInstances::Replace(*crate.World, crate.Crate, broken, nullptr, &error));
		CHECK(error.find("repeated ids") != std::string::npos);
		CHECK(SceneSerializer::SerializeEntities(*crate.World, { crate.Crate.GetParent() }) == before);
	}

	TEST_CASE("MakeTemplate and ReplaceTemplate put an instance's part in its place in the prefab")
	{
		LinkedCrate crate;
		const Entity sticker = crate.World->CreateChildEntity(crate.Lid, "Sticker");
		crate.Crate.GetTransform().Translation = glm::vec3(7.0f, 0.0f, 0.0f);
		crate.Crate.MarkModified<TransformComponent>();
		const nlohmann::json& templateRoot = crate.Snapshot["Entities"][0];
		std::unordered_map<UUID, UUID> ids;
		const nlohmann::json part = PrefabInstances::MakeTemplate(*crate.World, crate.Crate, &templateRoot["Components"]["Transform"],
			&templateRoot["Components"]["Name"], ids);
		REQUIRE(part["Entities"].size() == 4);
		CHECK(part["Entities"][0]["Components"]["Transform"] == templateRoot["Components"]["Transform"]);
		CHECK_FALSE(part["Entities"][0].contains("Parent"));
		CHECK_FALSE(part["Entities"][1]["Components"].contains("PrefabInstance"));
		CHECK(ids.at(crate.Lid.GetUUID()) == crate.Lid.GetUUID()); // Linked entities keep the prefab's ids
		const UUID stickerId = ids.at(sticker.GetUUID());
		CHECK(stickerId != sticker.GetUUID()); // A new id for what was added

		// A prefab with a second top-level entity after the crate: the crate's part is replaced where it was.
		nlohmann::json snapshot = crate.Snapshot;
		snapshot["Entities"].push_back({ { "ID", UUIDToJson(UUID(77)) }, { "Components", { { "Name", { { "Name", "Other" } } } } } });
		snapshot["Extra"] = true;
		const nlohmann::json replaced = PrefabInstances::ReplaceTemplate(snapshot, crate.Crate.GetUUID(), part);
		REQUIRE(replaced["Entities"].size() == 5);
		CHECK(replaced["Entities"][4]["ID"] == UUIDToJson(UUID(77)));
		CHECK(replaced["Extra"] == true);
		CHECK(PrefabInstances::ExtractTemplate(replaced, crate.Crate.GetUUID())["Entities"].size() == 4);
		CHECK(PrefabInstances::ExtractTemplate(replaced, UUID(77))["Entities"].size() == 1);
	}

	TEST_CASE("A snapshot that lists children before their parents works like one in hierarchy order")
	{
		// The crate's snapshot written by hand with the handle first and the lid last: Crate, Lid, Handle -> Handle, Crate, Lid.
		LinkedCrate crate;
		nlohmann::json shuffled = crate.Snapshot;
		nlohmann::json& entities = shuffled["Entities"];
		REQUIRE(entities.size() == 3);
		entities = nlohmann::json::array({ entities[2], entities[0], entities[1] });

		// The whole part, the root first and parents before children.
		const nlohmann::json part = PrefabInstances::ExtractTemplate(shuffled, crate.Crate.GetUUID());
		REQUIRE(part["Entities"].size() == 3);
		CHECK(part["Entities"][0]["ID"] == UUIDToJson(crate.Crate.GetUUID()));
		CHECK(part["Entities"][1]["ID"] == UUIDToJson(crate.Lid.GetUUID()));
		CHECK(part["Entities"][2]["ID"] == UUIDToJson(crate.Handle.GetUUID()));
		CHECK(PrefabInstances::Compare(*crate.World, crate.Crate, shuffled).IsEmpty());

		// Revert brings back a handle the instance lost.
		const UUID crateId = crate.Crate.GetUUID();
		crate.World->DestroyEntities(std::vector<Entity> { crate.Handle });
		std::vector<UUID> created;
		std::string error;
		REQUIRE(PrefabInstances::Replace(*crate.World, crate.World->GetEntityByUUID(crateId), shuffled, &created, &error));
		CHECK(created.size() == 1);
		const Entity root = crate.World->GetEntityByUUID(crateId);
		CHECK(PrefabInstances::Compare(*crate.World, root, shuffled).IsEmpty());

		// Applying replaces the whole old part: no entity stays behind or comes twice.
		std::unordered_map<UUID, UUID> ids;
		const nlohmann::json instance = PrefabInstances::MakeTemplate(*crate.World, root, nullptr, nullptr, ids);
		const nlohmann::json applied = PrefabInstances::ReplaceTemplate(shuffled, crateId, instance);
		CHECK(applied["Entities"].size() == 3);
		CHECK(PrefabInstances::ExtractTemplate(applied, crateId)["Entities"].size() == 3);
		crate.World->UpdateWorldTransforms();
		Tests::CheckSceneCaches(*crate.World);
	}

	TEST_CASE("An instance records the version of its part it took and its values then, which tell it outdated or modified later")
	{
		LinkedCrate crate;
		CHECK_FALSE(PrefabInstances::IsOutdated(crate.Crate, crate.Snapshot)); // Nothing recorded: not known to be outdated
		CHECK_FALSE(PrefabInstances::IsUnchangedSinceSync(*crate.World, crate.Crate));
		PrefabInstances::MarkSynced(*crate.World, crate.Crate, crate.Snapshot);
		const PrefabInstanceComponent& link = crate.Crate.GetComponent<PrefabInstanceComponent>();
		CHECK(link.PrefabVersion == PrefabInstances::GetPartVersion(crate.Snapshot, crate.Crate.GetUUID()));
		CHECK(link.PrefabVersion != 0);
		CHECK(link.SyncedHash == PrefabInstances::GetContentHash(*crate.World, crate.Crate));
		CHECK_FALSE(PrefabInstances::IsOutdated(crate.Crate, crate.Snapshot));
		CHECK(PrefabInstances::IsUnchangedSinceSync(*crate.World, crate.Crate));

		// The root's place and name are the instance's own: moving and renaming it change neither.
		crate.Crate.GetTransform().Translation = glm::vec3(3.0f, 0.0f, 0.0f);
		crate.Crate.MarkModified<TransformComponent>();
		crate.Crate.GetComponent<NameComponent>().Name = "Moved Crate";
		crate.Crate.MarkModified<NameComponent>();
		CHECK(PrefabInstances::IsUnchangedSinceSync(*crate.World, crate.Crate));
		nlohmann::json moved = crate.Snapshot;
		moved["Entities"][0]["Components"]["Transform"]["Translation"] = { 9.0, 9.0, 9.0 };
		CHECK(PrefabInstances::GetPartVersion(moved, crate.Crate.GetUUID()) == link.PrefabVersion);
		// Nor does the order the snapshot lists its entities in.
		nlohmann::json shuffled = crate.Snapshot;
		shuffled["Entities"] = nlohmann::json::array({ shuffled["Entities"][2], shuffled["Entities"][0], shuffled["Entities"][1] });
		CHECK(PrefabInstances::GetPartVersion(shuffled, crate.Crate.GetUUID()) == link.PrefabVersion);

		// Another version of the prefab: the instance is outdated (it did not take it), and still unchanged since it synced.
		nlohmann::json brighter = crate.Snapshot;
		brighter["Entities"][1]["Components"]["PointLight"]["Intensity"] = 7.0;
		CHECK(PrefabInstances::GetPartVersion(brighter, crate.Crate.GetUUID()) != link.PrefabVersion);
		CHECK(PrefabInstances::IsOutdated(crate.Crate, brighter));
		// Taking it (Replace) records the new version.
		const UUID crateId = crate.Crate.GetUUID();
		const UUID lidId = crate.Lid.GetUUID();
		REQUIRE(PrefabInstances::Replace(*crate.World, crate.Crate, brighter));
		const Entity root = crate.World->GetEntityByUUID(crateId);
		REQUIRE(root);
		CHECK_FALSE(PrefabInstances::IsOutdated(root, brighter));
		CHECK(PrefabInstances::IsOutdated(root, crate.Snapshot));
		CHECK(PrefabInstances::IsUnchangedSinceSync(*crate.World, root));

		// An edit of the instance: no longer unchanged since it synced.
		crate.World->GetEntityByUUID(lidId).GetComponent<PointLightComponent>().Intensity = 1.0f;
		CHECK_FALSE(PrefabInstances::IsUnchangedSinceSync(*crate.World, root));
		// The stamps serialize as 16 hex digits and come back.
		const nlohmann::json saved = SceneSerializer::SerializeEntities(*crate.World, { root });
		const nlohmann::json& stamps = saved["Entities"][0]["Components"]["PrefabInstance"];
		CHECK(stamps["PrefabVersion"].get<std::string>().size() == 16);
		CHECK(stamps["SyncedHash"].get<std::string>().size() == 16);
	}

	TEST_CASE("Unpack and StripLinks remove the links of one prefab")
	{
		LinkedCrate crate;
		crate.Handle.AddOrReplaceComponent<PrefabInstanceComponent>(PrefabInstanceComponent { AssetHandle(0x99), UUID(5) });
		PrefabInstances::Unpack(*crate.World, crate.Crate);
		CHECK_FALSE(crate.Crate.HasComponent<PrefabInstanceComponent>());
		CHECK_FALSE(crate.Lid.HasComponent<PrefabInstanceComponent>());
		CHECK(crate.Handle.HasComponent<PrefabInstanceComponent>()); // Another prefab's

		nlohmann::json snapshot = SceneSerializer::SerializeEntities(*crate.World, { crate.Crate });
		PrefabInstances::StripLinks(snapshot);
		for (const nlohmann::json& entity : snapshot["Entities"])
			CHECK_FALSE(entity["Components"].contains("PrefabInstance"));
	}
}
