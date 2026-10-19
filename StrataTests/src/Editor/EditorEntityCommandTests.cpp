#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/PropertyEdit.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Math/Math.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/Scripting/ScriptTypes.h>
#include <Strata/Scripting/ScriptingComponents.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	struct EntityHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

		Scene& GetScene() { return *Context.GetActiveScene(); }

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		EditorCommandResult Fail(std::string_view name, const nlohmann::json& parameters)
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			CHECK_FALSE(result.Success);
			return result;
		}

		std::string Create(const std::string& name, const nlohmann::json& extra = nlohmann::json::object())
		{
			nlohmann::json parameters = extra;
			parameters["name"] = name;
			return Run("entity.create", parameters)["id"].get<std::string>();
		}

		Entity Get(const std::string& id) { return GetScene().GetEntityByUUID(*UUIDFromJson(nlohmann::json(id))); }

		// Names of the top-level entities, or of an entity's children, in order.
		std::vector<std::string> Names(const std::string& parent = {})
		{
			std::vector<std::string> names;
			if (parent.empty())
			{
				for (UUID id : GetScene().GetRootEntities())
					names.push_back(GetScene().GetEntityByUUID(id).GetName());
			}
			else
			{
				for (Entity child : Get(parent).GetChildren())
					names.push_back(child.GetName());
			}
			return names;
		}

		nlohmann::json Snapshot() { return SceneSerializer::Serialize(GetScene())["Scene"]["Entities"]; }
		// The number of applied undo steps.
		size_t Position() { return Context.GetUndoStack().GetPosition(); }

		void CheckCaches()
		{
			std::string error;
			CHECK_MESSAGE(GetScene().ValidateHierarchy(&error), error);
			GetScene().UpdateWorldTransforms();
			CHECK_MESSAGE(GetScene().ValidateWorldTransforms(&error), error);
		}
	};

	// Adds a script entry with entity fields (no module is needed to hold them).
	void AddEntityFields(Entity entity, UUID inner, UUID outer)
	{
		ScriptEntry& script = entity.AddComponent<ScriptComponent>().Scripts.emplace_back();
		script.ClassName = "Follower";
		script.Fields.push_back({ "Inner", PropertyType::Entity, inner });
		script.Fields.push_back({ "Outer", PropertyType::Entity, outer });
	}

	UUID GetField(Entity entity, const std::string& field)
	{
		const ScriptEntry* script = entity.GetComponent<ScriptComponent>().FindScript("Follower");
		REQUIRE(script);
		return std::get<UUID>(script->FindField(field)->Value);
	}

}

TEST_SUITE("Editor.Commands")
{
	TEST_CASE("entity.get {world} reports world transforms and bounds, and entity.find {box} the entities in a box")
	{
		EntityHarness harness;
		const std::string parent = harness.Create("Parent", { { "components", { { "Transform", { { "Translation", { 1, 0, 0 } },
			{ "Rotation", { 0, 90, 0 } }, { "Scale", { 2, 2, 2 } } } } } } });
		const std::string child = harness.Create("Child", { { "parent", parent }, { "components", { { "Transform", { { "Translation", { 0, 0, 1 } },
			{ "Rotation", { 30, 0, 0 } } } } } } });
		const nlohmann::json got = harness.Run("entity.get", { { "entity", child }, { "world", true } });
		const glm::mat4 world = harness.GetScene().GetWorldTransform(harness.Get(child));
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		REQUIRE(Math::DecomposeTransform(world, position, rotation, scale));
		for (int axis = 0; axis < 3; axis++)
		{
			CHECK(got["world"]["position"][axis].get<float>() == doctest::Approx(position[axis]));
			CHECK(got["world"]["scale"][axis].get<float>() == doctest::Approx(scale[axis]));
		}
		CHECK(got["world"]["position"][0].get<float>() == doctest::Approx(3.0f)); // 1 + 2 along the parent's turned +Z
		const nlohmann::json& quaternion = got["world"]["rotation"]["quaternion"];
		CHECK(std::abs(glm::dot(glm::quat(quaternion[3].get<float>(), quaternion[0].get<float>(), quaternion[1].get<float>(), quaternion[2].get<float>()),
			rotation)) == doctest::Approx(1.0f));
		const glm::vec3 euler = Math::QuatToEulerDegrees(rotation);
		CHECK(got["world"]["rotation"]["euler"][1].get<float>() == doctest::Approx(euler.y));
		// Without world, nothing of it.
		CHECK_FALSE(harness.Run("entity.get", { { "entity", child } }).contains("world"));

		// A cube twice its size at (1, 2, 3).
		const std::string cube = harness.Create("Cube", { { "components", {
			{ "Transform", { { "Translation", { 1, 2, 3 } }, { "Scale", { 2, 2, 2 } } } },
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } });
		const nlohmann::json bounds = harness.Run("entity.get", { { "entity", cube }, { "world", true } })["bounds"];
		CHECK(bounds["min"] == nlohmann::json { 0.0, 1.0, 2.0 });
		CHECK(bounds["max"] == nlohmann::json { 2.0, 3.0, 4.0 });
		CHECK(bounds["size"] == nlohmann::json { 2.0, 2.0, 2.0 });
		CHECK(bounds["center"] == nlohmann::json { 1.0, 2.0, 3.0 });
		CHECK(bounds["pending"] == false);

		// The box finds exactly the entities whose own bounds reach into it, in hierarchy order: the cube, and the empty
		// entities by their origins.
		const std::string far = harness.Create("Far", { { "components", { { "Transform", { { "Translation", { 50, 0, 0 } } } } } } });
		CHECK(harness.Run("entity.find", { { "box", { { "min", { 1.9, 2.9, 3.9 } }, { "max", { 5, 5, 5 } } } } })["entities"] == nlohmann::json { cube });
		CHECK(harness.Run("entity.find", { { "box", { { "min", { 49, -1, -1 } }, { "max", { 51, 1, 1 } } } } })["entities"] == nlohmann::json { far });
		CHECK(harness.Run("entity.find", { { "box", { { "min", { -100, -100, -100 } }, { "max", { 100, 100, 100 } } } }, { "limit", 2 } })["entities"]
			== nlohmann::json { parent, child });
		CHECK(harness.Run("entity.find", { { "box", { { "min", { 2.1, 3.1, 4.1 } }, { "max", { 5, 5, 5 } } } } })["entities"].empty());
		CHECK(harness.Fail("entity.find", { { "box", { { "min", { 1, 1, 1 } }, { "max", { 0, 0, 0 } } } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("entity.find", { { "box", { { "min", { 1, 1 } } } } }).ErrorKind == EditorCommandError::InvalidParameters);
	}

	TEST_CASE("entity.setParent moves and reorders many entities in one undo step and refuses cycles")
	{
		EntityHarness harness;
		const std::string a = harness.Create("A", { { "components", { { "Transform", { { "Translation", { 1, 0, 0 } } } } } } });
		const std::string b = harness.Create("B");
		const std::string c = harness.Create("C");
		const std::string d = harness.Create("D");
		const std::string parent = harness.Create("P", { { "components", { { "Transform", { { "Translation", { 10, 0, 0 } } } } } } });
		const std::string x = harness.Create("X", { { "parent", parent } });
		const std::string y = harness.Create("Y", { { "parent", parent } });
		const nlohmann::json original = harness.Snapshot();
		const size_t start = harness.Position();

		// Top-level entities reorder: the given order lands at the index.
		harness.Run("entity.setParent", { { "entities", { d, b } }, { "index", 0 } });
		CHECK(harness.Names() == std::vector<std::string> { "D", "B", "A", "C", "P" });
		CHECK(harness.Position() == start + 1);
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);
		harness.Run("edit.redo");
		CHECK(harness.Names() == std::vector<std::string> { "D", "B", "A", "C", "P" });
		harness.Run("edit.undo");

		// Into a parent at a position, keeping their place in the world.
		harness.Run("entity.setParent", { { "entities", { a, c } }, { "parent", parent }, { "index", 1 } });
		CHECK(harness.Names(parent) == std::vector<std::string> { "X", "A", "C", "Y" });
		CHECK(harness.Names() == std::vector<std::string> { "B", "D", "P" });
		CHECK(harness.Get(a).GetComponent<TransformComponent>().Translation.x == doctest::Approx(-9.0f));
		harness.CheckCaches();
		CHECK(glm::vec3(harness.GetScene().GetWorldTransform(harness.Get(a))[3]).x == doctest::Approx(1.0f));
		CHECK(harness.Position() == start + 1);

		// Reordering among the current siblings (the index is the final position).
		harness.Run("entity.setParent", { { "entities", { y } }, { "parent", parent }, { "index", 0 } });
		CHECK(harness.Names(parent) == std::vector<std::string> { "Y", "X", "A", "C" });
		harness.Run("entity.setParent", { { "entity", x }, { "parent", parent }, { "index", 3 } });
		CHECK(harness.Names(parent) == std::vector<std::string> { "Y", "A", "C", "X" });
		// Without an index, entities that change parent go last and the others stay.
		harness.Run("entity.setParent", { { "entities", { b, a } }, { "parent", parent } });
		CHECK(harness.Names(parent) == std::vector<std::string> { "Y", "A", "C", "X", "B" });
		harness.CheckCaches();
		const size_t position = harness.Position();
		CHECK(position == start + 4);

		// Cycles and malformed requests change nothing.
		const nlohmann::json before = harness.Snapshot();
		CHECK(harness.Fail("entity.setParent", { { "entities", { parent } }, { "parent", x } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("entity.setParent", { { "entities", { d, parent } }, { "parent", parent } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("entity.setParent", { { "entity", d }, { "entities", { d } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("entity.setParent", { { "parent", parent } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("entity.setParent", { { "entities", nlohmann::json::array() } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Snapshot() == before);
		CHECK(harness.Position() == position);

		// An entity listed with its parent moves with it.
		harness.Run("entity.setParent", { { "entities", { x, parent, x } }, { "index", 0 } });
		CHECK(harness.Names() == std::vector<std::string> { "P", "D" });
		CHECK(harness.Get(x).GetParent() == harness.Get(parent));

		// Everything back, one step per command.
		while (harness.Position() > start)
			harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);
		harness.CheckCaches();
	}

	TEST_CASE("entity.copy and entity.paste make new entities with remapped references in one undo step")
	{
		EntityHarness harness;
		const std::string outside = harness.Create("Outside");
		const std::string root = harness.Create("Root", { { "components", { { "PointLight", { { "Intensity", 3.0 } } } } } });
		const std::string child = harness.Create("Child", { { "parent", root } });
		AddEntityFields(harness.Get(root), harness.Get(child).GetUUID(), harness.Get(outside).GetUUID());
		const std::string target = harness.Create("Target");
		const nlohmann::json before = harness.Snapshot();
		const size_t position = harness.Position();

		// The child goes with its parent; the data names its format. Copying changes nothing.
		const nlohmann::json copied = harness.Run("entity.copy", { { "entities", { child, root } } });
		CHECK(copied["count"] == 2);
		CHECK(copied["roots"] == nlohmann::json::array({ root }));
		CHECK(copied["data"]["Strata"]["Format"] == "Entities");
		CHECK(harness.Snapshot() == before);
		CHECK(harness.Position() == position);

		const nlohmann::json pasted = harness.Run("entity.paste", { { "data", copied["data"] }, { "parent", target } });
		CHECK(harness.Position() == position + 1);
		REQUIRE(pasted["entities"].size() == 1);
		CHECK(pasted["warnings"].empty());
		Entity copy = harness.Get(pasted["entities"][0].get<std::string>());
		REQUIRE(copy);
		CHECK(copy.GetUUID() != harness.Get(root).GetUUID());
		CHECK(copy.GetName() == "Root");
		CHECK(copy.GetParent() == harness.Get(target));
		CHECK(copy.GetComponent<PointLightComponent>().Intensity == 3.0f);
		const std::vector<Entity> copyChildren = copy.GetChildren();
		REQUIRE(copyChildren.size() == 1);
		CHECK(copyChildren[0].GetUUID() != harness.Get(child).GetUUID());
		// References inside the copy point at the copies; references to other entities stay.
		CHECK(GetField(copy, "Inner") == copyChildren[0].GetUUID());
		CHECK(GetField(copy, "Outer") == harness.Get(outside).GetUUID());
		// Pasting links nothing to a prefab.
		CHECK_FALSE(copy.HasComponent<PrefabInstanceComponent>());

		// At a position; again with fresh IDs.
		const nlohmann::json second = harness.Run("entity.paste", { { "data", copied["data"] }, { "index", 0 } });
		CHECK(harness.Names().front() == "Root (1)"); // Among the top-level entities, beside the original
		CHECK(second["entities"][0] != pasted["entities"][0]);
		harness.CheckCaches();

		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == before);
		harness.Run("edit.redo");
		CHECK(harness.Get(pasted["entities"][0].get<std::string>()).GetParent() == harness.Get(target));

		// Data that is not entity data is refused without changes.
		const nlohmann::json unchanged = harness.Snapshot();
		CHECK(harness.Fail("entity.paste", { { "data", nlohmann::json::object() } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("entity.paste", { { "data", { { "Strata", { { "Format", "Scene" } } }, { "Entities", copied["data"]["Entities"] } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("entity.paste", { { "data", { { "Entities", { { { "ID", "not an id" } } } } } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Snapshot() == unchanged);

		// Into another scene: what the clipboard is for.
		harness.Run("scene.new", { { "force", true } });
		const nlohmann::json other = harness.Run("entity.paste", { { "data", copied["data"] } });
		CHECK(harness.GetScene().GetEntityCount() == 2);
		CHECK(GetField(harness.Get(other["entities"][0].get<std::string>()), "Outer") == *UUIDFromJson(nlohmann::json(outside))); // Missing in this scene
	}

	TEST_CASE("entity.duplicate copies many entities, each right after its original, in one undo step")
	{
		EntityHarness harness;
		const std::string a = harness.Create("A");
		const std::string b = harness.Create("B");
		const std::string c = harness.Create("C");
		const std::string parent = harness.Create("P");
		const std::string x = harness.Create("X", { { "parent", parent } });
		const std::string y = harness.Create("Y", { { "parent", parent } });
		AddEntityFields(harness.Get(parent), harness.Get(y).GetUUID(), harness.Get(a).GetUUID());
		const nlohmann::json original = harness.Snapshot();
		const size_t position = harness.Position();

		const nlohmann::json duplicated = harness.Run("entity.duplicate", { { "entities", { c, a, y, x } } });
		CHECK(harness.Position() == position + 1);
		CHECK(harness.Names() == std::vector<std::string> { "A", "A (1)", "B", "C", "C (1)", "P" });
		CHECK(harness.Names(parent) == std::vector<std::string> { "X", "X (1)", "Y", "Y (1)" });
		REQUIRE(duplicated["entities"].size() == 4);
		CHECK(duplicated["id"] == duplicated["entities"][0]);
		// In the order given: the copies of C, A, Y, X.
		CHECK(harness.Get(duplicated["entities"][0].get<std::string>()) == harness.GetScene().GetEntityByUUID(harness.GetScene().GetRootEntities()[4]));
		CHECK(harness.Get(duplicated["entities"][1].get<std::string>()) == harness.GetScene().GetEntityByUUID(harness.GetScene().GetRootEntities()[1]));
		harness.CheckCaches();

		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);
		harness.Run("edit.redo");
		CHECK(harness.Names() == std::vector<std::string> { "A", "A (1)", "B", "C", "C (1)", "P" });

		// A parent listed with its children is copied whole, once; references inside it point at the copies.
		const nlohmann::json whole = harness.Run("entity.duplicate", { { "entities", { x, parent } } });
		REQUIRE(whole["entities"].size() == 1);
		Entity copy = harness.Get(whole["id"].get<std::string>());
		CHECK(copy.GetChildren().size() == 4);
		CHECK(GetField(copy, "Inner") == copy.GetChildren()[2].GetUUID());
		CHECK(GetField(copy, "Outer") == harness.Get(a).GetUUID());
		CHECK(harness.Names().back() == "P (1)");

		// The single form keeps its result.
		const nlohmann::json single = harness.Run("entity.duplicate", { { "entity", b } });
		CHECK(single["entities"] == nlohmann::json::array({ single["id"] }));
		CHECK(harness.Fail("entity.duplicate", nlohmann::json::object()).ErrorKind == EditorCommandError::InvalidParameters);
		harness.CheckCaches();
	}

	TEST_CASE("entity.duplicate remaps references between copies in different sibling lists")
	{
		EntityHarness harness;
		const std::string outside = harness.Create("Outside");
		const std::string a = harness.Create("A");
		const std::string parent = harness.Create("P", { { "components", { { "Transform", { { "Translation", { 10, 0, 0 } } } } } } });
		const std::string x = harness.Create("X", { { "parent", parent } });
		const std::string y = harness.Create("Y", { { "parent", parent }, { "components", { { "Transform", { { "Translation", { 0, 2, 0 } } } } } } });
		// A (top level) refers to Y (a child of P), and Y to A: across the two sibling lists.
		AddEntityFields(harness.Get(a), harness.Get(y).GetUUID(), harness.Get(outside).GetUUID());
		AddEntityFields(harness.Get(y), harness.Get(a).GetUUID(), harness.Get(x).GetUUID());
		const nlohmann::json original = harness.Snapshot();
		const size_t position = harness.Position();

		const nlohmann::json duplicated = harness.Run("entity.duplicate", { { "entities", { a, y } } });
		CHECK(harness.Position() == position + 1);
		REQUIRE(duplicated["entities"].size() == 2);
		const Entity copyA = harness.Get(duplicated["entities"][0].get<std::string>());
		const Entity copyY = harness.Get(duplicated["entities"][1].get<std::string>());
		REQUIRE(copyA);
		REQUIRE(copyY);
		// Each copy right after its original, in its original's sibling list.
		CHECK(harness.Names() == std::vector<std::string> { "Outside", "A", "A (1)", "P" });
		CHECK(harness.Names(parent) == std::vector<std::string> { "X", "Y", "Y (1)" });
		CHECK_FALSE(copyA.GetParent());
		CHECK(copyY.GetParent() == harness.Get(parent));
		// The copy of Y keeps its place in the world (its local transform, under the same parent).
		CHECK(copyY.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 2.0f, 0.0f));
		harness.CheckCaches();
		CHECK(glm::vec3(harness.GetScene().GetWorldTransform(copyY)[3]) == glm::vec3(10.0f, 2.0f, 0.0f));
		// References between the copies point at the copies, also across the sibling lists; others stay.
		CHECK(GetField(copyA, "Inner") == copyY.GetUUID());
		CHECK(GetField(copyA, "Outer") == harness.Get(outside).GetUUID());
		CHECK(GetField(copyY, "Inner") == copyA.GetUUID());
		CHECK(GetField(copyY, "Outer") == harness.Get(x).GetUUID());

		// Undo removes both copies; redo brings them back with the same IDs and references.
		const UUID copyYId = copyY.GetUUID();
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);
		harness.Run("edit.redo");
		CHECK(GetField(harness.Get(duplicated["entities"][0].get<std::string>()), "Inner") == copyYId);
		CHECK(harness.Names(parent) == std::vector<std::string> { "X", "Y", "Y (1)" });
		harness.CheckCaches();
	}

	TEST_CASE("component.set {entities} sets the same values on every entity in one undo step")
	{
		EntityHarness harness;
		std::vector<std::string> lights;
		for (int index = 0; index < 3; index++)
			lights.push_back(harness.Create("Light", { { "components", { { "PointLight", { { "Intensity", 1.0 + index } } } } } }));
		const std::string plain = harness.Create("Plain");
		const nlohmann::json original = harness.Snapshot();
		const size_t position = harness.Position();

		const nlohmann::json result = harness.Run("component.set", { { "entities", lights }, { "component", "PointLight" },
			{ "values", { { "Intensity", 7.0 }, { "Range", 4.0 } } } });
		CHECK(result["count"] == 3);
		CHECK(result["values"]["Intensity"] == 7.0);
		CHECK(harness.Position() == position + 1);
		for (const std::string& light : lights)
			CHECK(harness.Get(light).GetComponent<PointLightComponent>().Intensity == 7.0f);
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);

		// One entity without the component, or an invalid value, and nothing changes.
		std::vector<std::string> withPlain = lights;
		withPlain.push_back(plain);
		CHECK(harness.Fail("component.set", { { "entities", withPlain }, { "component", "PointLight" }, { "values", { { "Intensity", 2.0 } } } }).ErrorKind
			== EditorCommandError::Failed);
		CHECK(harness.Fail("component.set", { { "entities", lights }, { "component", "PointLight" }, { "values", { { "Intensity", "bright" } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("component.set", { { "entity", lights[0] }, { "entities", lights }, { "component", "PointLight" }, { "values", nlohmann::json::object() } })
			.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Snapshot() == original);
		CHECK(harness.Position() == position);
		CHECK(harness.Context.GetUndoStack().CanRedo()); // Failed edits record nothing (the undone step is still there to redo)
	}

	TEST_CASE("entity.setActive, component.add and component.remove take entities and record one step")
	{
		EntityHarness harness;
		std::vector<std::string> entities;
		for (int index = 0; index < 3; index++)
			entities.push_back(harness.Create(fmt::format("Entity {}", index)));
		const nlohmann::json original = harness.Snapshot();
		const size_t position = harness.Position();

		harness.Run("entity.setActive", { { "entities", entities }, { "active", false } });
		CHECK(harness.Position() == position + 1);
		CHECK(harness.Context.GetUndoStack().GetUndoName() == "Deactivate 3 entities");
		for (const std::string& id : entities)
			CHECK_FALSE(harness.Get(id).IsActive());

		// Added where it is missing, with the values given: one step for all.
		harness.Run("component.add", { { "entity", entities[0] }, { "component", "PointLight" } });
		harness.Run("component.add", { { "entities", entities }, { "component", "PointLight" }, { "values", { { "Range", 5.0 } } } });
		CHECK(harness.Position() == position + 3);
		for (const std::string& id : entities)
			CHECK(harness.Get(id).GetComponent<PointLightComponent>().Range == 5.0f);

		// Removed from all, or (when one lacks it) from none.
		const std::string plain = harness.Create("Plain");
		std::vector<std::string> withPlain = entities;
		withPlain.push_back(plain);
		const nlohmann::json before = harness.Snapshot();
		CHECK(harness.Fail("component.remove", { { "entities", withPlain }, { "component", "PointLight" } }).ErrorKind == EditorCommandError::Failed);
		CHECK(harness.Snapshot() == before);
		// An entity listed twice counts once.
		std::vector<std::string> twice = entities;
		twice.push_back(entities[0]);
		harness.Run("component.remove", { { "entities", twice }, { "component", "PointLight" } });
		for (const std::string& id : entities)
			CHECK_FALSE(harness.Get(id).HasComponent<PointLightComponent>());

		// Exactly one of entity and entities.
		CHECK(harness.Fail("entity.setActive", { { "active", true } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("component.add", { { "entity", entities[0] }, { "entities", entities }, { "component", "PointLight" } }).ErrorKind
			== EditorCommandError::InvalidParameters);

		for (int step = 0; step < 5; step++)
			harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);
	}

	TEST_CASE("A yaw dragged through 90 degrees on several entities keeps each one's pitch and roll")
	{
		// Rolls of 0 (where reading the angles back recovers them exactly) and others; the yaw passes 90 degrees exactly, where
		// a rotation keeps only the difference of pitch and roll, and goes past it, where reading folds it back (yaw 100 reads
		// as pitch + 180, yaw 80, roll + 180).
		const std::vector<std::array<glm::vec3, 2>> cases = {
			{ glm::vec3(10.0f, 80.0f, 0.0f), glm::vec3(20.0f, 80.0f, 0.0f) },
			{ glm::vec3(10.0f, 80.0f, 30.0f), glm::vec3(20.0f, 80.0f, 40.0f) },
		};
		for (const std::array<glm::vec3, 2>& starts : cases)
		{
			CAPTURE(starts[1]);
			EntityHarness harness;
			std::vector<Entity> entities;
			for (const glm::vec3& start : starts)
				entities.push_back(harness.Get(harness.Create("Turned", { { "components", { { "Transform", { { "Rotation", { start.x, start.y, start.z } } } } } } })));
			const ComponentInfo& transform = *ComponentRegistry::Find("Transform");
			const PropertyInfo& rotation = *transform.FindProperty("Rotation");
			const nlohmann::json original = harness.Snapshot();
			const size_t position = harness.Position();

			// The inspector's drag of the yaw field: the angles it shows (the first entity's) with the yaw raised 2 degrees a
			// frame.
			std::string error;
			std::optional<RotationAnglesEdit> edit = RotationAnglesEdit::Begin(entities, transform, rotation, starts[0], &error);
			REQUIRE_MESSAGE(edit, error);
			for (float yaw = 82.0f; yaw <= 120.0f; yaw += 2.0f)
			{
				REQUIRE(edit->Continues(entities, transform, rotation));
				REQUIRE_MESSAGE(edit->Update(harness.Context, entities, transform, rotation, glm::vec3(starts[0].x, yaw, starts[0].z), { false, true, false, false },
					&error), error);
				// The first entity's rotation is exactly the one the field's angles make, so the widget keeps showing them.
				CHECK(entities[0].GetComponent<TransformComponent>().Rotation == Math::EulerDegreesToQuat(glm::vec3(starts[0].x, yaw, starts[0].z)));
			}
			CHECK(harness.Position() == position + 1);
			// Each turned to a yaw of 120 degrees, keeping its own pitch and roll.
			for (size_t index = 0; index < entities.size(); index++)
			{
				const glm::quat expected = Math::EulerDegreesToQuat(glm::vec3(starts[index].x, 120.0f, starts[index].z));
				CHECK(std::abs(glm::dot(expected, entities[index].GetComponent<TransformComponent>().Rotation)) == doctest::Approx(1.0f).epsilon(0.0001));
			}
			// And shown as differing in pitch and roll (roll only where it differs).
			const std::optional<SharedPropertyValue> shared = GetSharedProperty(entities, transform, rotation);
			REQUIRE(shared);
			CHECK(shared->MixedComponents == std::array<bool, 4> { true, false, starts[0].z != starts[1].z, false });

			// One step, which undo reverts whole.
			REQUIRE(harness.Context.Undo());
			CHECK(harness.Snapshot() == original);
		}
	}

	TEST_CASE("A rotation edit of several entities continues only while they keep the rotations it gave them")
	{
		EntityHarness harness;
		std::vector<Entity> entities;
		const glm::vec3 starts[] = { glm::vec3(10.0f, 20.0f, 30.0f), glm::vec3(40.0f, 50.0f, 60.0f) };
		for (const glm::vec3& start : starts)
			entities.push_back(harness.Get(harness.Create("Turned", { { "components", { { "Transform", { { "Rotation", { start.x, start.y, start.z } } } } } } })));
		const ComponentInfo& transform = *ComponentRegistry::Find("Transform");
		const PropertyInfo& rotation = *transform.FindProperty("Rotation");
		const PropertyInfo& translation = *transform.FindProperty("Translation");
		const PropertyComponentMask yaw = { false, true, false, false };

		std::string error;
		std::optional<RotationAnglesEdit> edit = RotationAnglesEdit::Begin(entities, transform, rotation, starts[0], &error);
		REQUIRE_MESSAGE(edit, error);
		REQUIRE_MESSAGE(edit->Update(harness.Context, entities, transform, rotation, glm::vec3(10.0f, 25.0f, 30.0f), yaw, &error), error);
		CHECK(edit->Continues(entities, transform, rotation));
		// Other entities, another order, another property: not this edit.
		const std::vector<Entity> reversed = { entities[1], entities[0] };
		CHECK_FALSE(edit->Continues(reversed, transform, rotation));
		CHECK_FALSE(edit->Continues(std::span<const Entity>(entities).first(1), transform, rotation));
		CHECK_FALSE(edit->Continues(entities, transform, translation));

		// Undone (or changed by anything else): the edit's angles no longer describe the entities, and it refuses to write them.
		harness.Context.GetUndoStack().BreakMerge();
		REQUIRE(harness.Context.Undo());
		CHECK_FALSE(edit->Continues(entities, transform, rotation));
		const nlohmann::json undone = harness.Snapshot();
		CHECK_FALSE(edit->Update(harness.Context, entities, transform, rotation, glm::vec3(10.0f, 30.0f, 30.0f), yaw, &error));
		CHECK_FALSE(error.empty());
		CHECK(harness.Snapshot() == undone);

		// Invalid edits begin nothing: not a rotation, no entities, an entity that does not exist.
		CHECK_FALSE(RotationAnglesEdit::Begin(entities, transform, translation, starts[0], &error));
		CHECK_FALSE(RotationAnglesEdit::Begin({}, transform, rotation, starts[0], &error));
		const std::vector<Entity> withMissing = { entities[0], Entity() };
		CHECK_FALSE(RotationAnglesEdit::Begin(withMissing, transform, rotation, starts[0], &error));
	}

	TEST_CASE("A rotation edit gives entities with the rotation shown the angles shown")
	{
		// Two entities with the same rotation at a yaw of 90 degrees (a duplicate), where reading the angles back gives another
		// split of pitch and roll (any with the same difference is the same rotation): the inspector shows no angle as
		// differing, so an edit of the pitch gives both the angles shown, and they stay the same rotation.
		EntityHarness harness;
		const glm::vec3 shown(10.0f, 90.0f, 30.0f);
		std::vector<Entity> entities;
		for (int index = 0; index < 2; index++)
			entities.push_back(harness.Get(harness.Create("Turned", { { "components", { { "Transform", { { "Rotation", { shown.x, shown.y, shown.z } } } } } } })));
		const ComponentInfo& transform = *ComponentRegistry::Find("Transform");
		const PropertyInfo& rotation = *transform.FindProperty("Rotation");
		const std::optional<SharedPropertyValue> shared = GetSharedProperty(entities, transform, rotation);
		REQUIRE(shared);
		REQUIRE_FALSE(shared->Mixed);

		std::string error;
		std::optional<RotationAnglesEdit> edit = RotationAnglesEdit::Begin(entities, transform, rotation, shown, &error);
		REQUIRE_MESSAGE(edit, error);
		const glm::vec3 edited(15.0f, 90.0f, 30.0f);
		REQUIRE_MESSAGE(edit->Update(harness.Context, entities, transform, rotation, edited, { true, false, false, false }, &error), error);
		for (const Entity entity : entities)
			CHECK(entity.GetComponent<TransformComponent>().Rotation == Math::EulerDegreesToQuat(edited));
	}

	TEST_CASE("Rotation edits of many entities change only the Euler angles the edit changed")
	{
		EntityHarness harness;
		std::vector<Entity> entities;
		for (int index = 0; index < 3; index++)
		{
			const std::string id = harness.Create("Turned", { { "components", { { "Transform", { { "Rotation", { 10.0 * index, 20.0, 30.0 } } } } } } });
			entities.push_back(harness.Get(id));
		}
		const ComponentInfo& transform = *ComponentRegistry::Find("Transform");
		const PropertyInfo& rotation = *transform.FindProperty("Rotation");
		const std::optional<SharedPropertyValue> shared = GetSharedProperty(entities, transform, rotation);
		REQUIRE(shared);
		CHECK(shared->Mixed);
		CHECK(shared->MixedComponents == std::array<bool, 4> { true, false, false, false });

		// Yaw (Y) set on all three: each keeps its own pitch (X).
		const glm::vec3 shown = Math::QuatToEulerDegrees(std::get<glm::quat>(shared->Value));
		const PropertyValue edited = Math::EulerDegreesToQuat(glm::vec3(shown.x, 45.0f, shown.z));
		std::string error;
		REQUIRE_MESSAGE(SetPropertyWithUndo(harness.Context, entities, transform, rotation, shared->Value, edited, &error), error);
		for (size_t index = 0; index < entities.size(); index++)
		{
			const glm::vec3 angles = Math::QuatToEulerDegrees(entities[index].GetComponent<TransformComponent>().Rotation);
			CHECK(angles.x == doctest::Approx(10.0f * static_cast<float>(index)).epsilon(0.001));
			CHECK(angles.y == doctest::Approx(45.0f).epsilon(0.001));
			CHECK(angles.z == doctest::Approx(30.0f).epsilon(0.001));
		}

		// Components given explicitly: pitch reset to 0 on all, although the first entity's is 0 already (the value shown
		// equals the new one, so comparing them could not tell what to write). A new step: the edit before has ended.
		harness.Context.GetUndoStack().BreakMerge();
		const size_t position = harness.Position();
		const PropertyValue reset = Math::EulerDegreesToQuat(glm::vec3(0.0f, 45.0f, 30.0f));
		REQUIRE_MESSAGE(SetPropertyComponentsWithUndo(harness.Context, entities, transform, rotation, reset, { true, false, false, false }, &error), error);
		CHECK(harness.Position() == position + 1);
		for (const Entity entity : entities)
		{
			const glm::vec3 angles = Math::QuatToEulerDegrees(entity.GetComponent<TransformComponent>().Rotation);
			CHECK(angles.x == doctest::Approx(0.0f).epsilon(0.001));
			CHECK(angles.y == doctest::Approx(45.0f).epsilon(0.001));
		}
		// And for vectors: only X, each entity keeps its own Y and Z.
		const PropertyInfo& translation = *transform.FindProperty("Translation");
		for (size_t index = 0; index < entities.size(); index++)
		{
			entities[index].GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index), 1.0f + static_cast<float>(index), 2.0f);
			entities[index].MarkModified<TransformComponent>();
		}
		REQUIRE(SetPropertyComponentsWithUndo(harness.Context, entities, transform, translation, PropertyValue(glm::vec3(0.0f)), { true, false, false, false }, &error));
		for (size_t index = 0; index < entities.size(); index++)
			CHECK(entities[index].GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 1.0f + static_cast<float>(index), 2.0f));
	}

	TEST_CASE("Property edits of many entities are one undo step and keep what the edit did not touch")
	{
		EntityHarness harness;
		std::vector<Entity> lights;
		for (int index = 0; index < 30; index++)
		{
			const std::string id = harness.Create("Light", { { "components", { { "PointLight", { { "Intensity", 1.0 + index } } },
				{ "Transform", { { "Translation", { 1, index, 2 } } } } } } });
			lights.push_back(harness.Get(id));
		}
		const ComponentInfo& light = *ComponentRegistry::Find("PointLight");
		const PropertyInfo& intensity = *light.FindProperty("Intensity");
		const ComponentInfo& transform = *ComponentRegistry::Find("Transform");
		const PropertyInfo& translation = *transform.FindProperty("Translation");
		const nlohmann::json original = harness.Snapshot();
		const size_t position = harness.Position();

		std::optional<SharedPropertyValue> shared = GetSharedProperty(lights, light, intensity);
		REQUIRE(shared);
		CHECK(shared->Mixed);
		CHECK(std::get<float>(shared->Value) == 1.0f);
		std::optional<SharedPropertyValue> positions = GetSharedProperty(lights, transform, translation);
		REQUIRE(positions);
		CHECK(positions->MixedComponents == std::array<bool, 4> { false, true, false, false });

		// A drag over the intensity of 30 lights: one step.
		std::string error;
		PropertyValue shown = shared->Value;
		for (int step = 1; step <= 40; step++)
		{
			const PropertyValue value(1.0f + step * 0.25f);
			REQUIRE_MESSAGE(SetPropertyWithUndo(harness.Context, lights, light, intensity, shown, value, &error), error);
			shown = value;
		}
		harness.Context.GetUndoStack().BreakMerge();
		CHECK(harness.Position() == position + 1);
		CHECK(harness.Context.GetUndoStack().GetUndoName() == "Edit Point Light Intensity of 30 Entities");
		for (const Entity entity : lights)
			CHECK(entity.GetComponent<PointLightComponent>().Intensity == 11.0f);
		CHECK_FALSE(GetSharedProperty(lights, light, intensity)->Mixed);

		// Resetting X (the label's click) keeps every entity's own Y and Z.
		PropertyValue resetX = positions->Value;
		std::get<glm::vec3>(resetX).x = 0.0f;
		REQUIRE(SetPropertyWithUndo(harness.Context, lights, transform, translation, positions->Value, resetX, &error));
		for (size_t index = 0; index < lights.size(); index++)
			CHECK(lights[index].GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, static_cast<float>(index), 2.0f));
		CHECK(harness.Position() == position + 2);

		// Invalid values change nothing.
		const nlohmann::json edited = harness.Snapshot();
		CHECK_FALSE(SetPropertyWithUndo(harness.Context, lights, light, intensity, PropertyValue(std::nanf("")), PropertyValue(std::nanf("")), &error));
		CHECK_FALSE(SetPropertyWithUndo(harness.Context, lights, light, intensity, shown, PropertyValue(glm::vec3(1.0f)), &error));
		CHECK(harness.Position() == position + 2);
		CHECK(harness.Snapshot() == edited);

		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);

		// The defaults Reset restores, as component.set takes them: one step for all entities.
		const nlohmann::json defaults = GetDefaultComponentValues(light);
		CHECK(defaults["Intensity"] == PropertyValueToJson(intensity, PropertyValue(PointLightComponent().Intensity)));
		nlohmann::json ids = nlohmann::json::array();
		for (const Entity entity : lights)
			ids.push_back(UUIDToJson(entity.GetUUID()));
		harness.Run("component.set", { { "entities", ids }, { "component", "PointLight" }, { "values", defaults } });
		for (const Entity entity : lights)
			CHECK(entity.GetComponent<PointLightComponent>().Intensity == PointLightComponent().Intensity);
		CHECK(GetDefaultComponentValues(transform)["Scale"] == nlohmann::json { 1.0, 1.0, 1.0 });
	}
}
