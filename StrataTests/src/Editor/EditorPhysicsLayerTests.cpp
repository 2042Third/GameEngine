#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "EditorLayer.h"
#include "UI/ItemProbe.h"

#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Runs commands against an editor context without UI.
	struct LayerCommands
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		// A request scene.setSettings refuses as invalid, changing nothing and recording no step.
		void Refuse(const nlohmann::json& parameters)
		{
			INFO(parameters.dump());
			const PhysicsLayerSettings before = Layers();
			const size_t position = Context.GetUndoStack().GetPosition();
			const EditorCommandResult result = Commands.Execute(Context, "scene.setSettings", parameters);
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
			CHECK(Layers() == before);
			CHECK(Context.GetUndoStack().GetPosition() == position);
		}

		const PhysicsLayerSettings& Layers() { return Context.GetActiveScene()->GetSettings().PhysicsLayers; }
	};

	// An editor without a project, past the launcher, with nothing selected (the Inspector shows the scene's settings).
	struct LayersEditor : HarnessEditor
	{
		LayersEditor()
		{
			Layer->DismissLauncher();
			Context().ClearSelection();
			Frames(2);
		}

		const PhysicsLayerSettings& Layers() { return Context().GetActiveScene()->GetSettings().PhysicsLayers; }
		size_t UndoPosition() { return Context().GetUndoStack().GetPosition(); }

		std::string ProbeText(std::string_view key)
		{
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
			REQUIRE_MESSAGE(item.has_value(), "No ", std::string(key), " in the last frame");
			return item->Text;
		}

		void PressEnter()
		{
			Harness.SetKey(ImGuiKey_Enter, true);
			Frames(1);
			Harness.SetKey(ImGuiKey_Enter, false);
			Frames(1);
		}
	};

}

TEST_SUITE("Editor.PhysicsLayers")
{
	TEST_CASE("scene.setSettings names layers and changes the matrix in one undo step")
	{
		LayerCommands harness;
		const size_t position = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json result = harness.Run("scene.setSettings", {
			{ "layerNames", { { "1", "Player" }, { "2", "Debris" } } },
			{ "layerCollisions", nlohmann::json::array({
				{ { "layers", { "Player", "debris" } }, { "collide", false } },
				{ { "layers", { 2, 2 } }, { "collide", false } },
				{ { "layers", { "Layer 9", 0 } }, { "collide", false } } }) } });
		CHECK(harness.Context.GetUndoStack().GetPosition() == position + 1);
		const nlohmann::json expected = {
			{ "names", { { "1", "Player" }, { "2", "Debris" } } },
			{ "noCollisions", nlohmann::json::array({ { "Layer 0", "Layer 9" }, { "Player", "Debris" }, { "Debris", "Debris" } }) } };
		CHECK(result["settings"]["physicsLayers"] == expected);
		CHECK(harness.Run("scene.info")["settings"]["physicsLayers"] == expected);
		CHECK_FALSE(harness.Layers().DoCollide(2, 1));

		harness.Run("edit.undo");
		CHECK(harness.Layers() == PhysicsLayerSettings());
		harness.Run("edit.redo");
		CHECK(harness.Layers().Names[1] == "Player");
		CHECK_FALSE(harness.Layers().DoCollide(1, 2));

		// Layers swap names in one call; an empty name or null clears one.
		harness.Run("scene.setSettings", { { "layerNames", { { "1", "Debris" }, { "2", "Player" } } } });
		CHECK(harness.Layers().Names[1] == "Debris");
		CHECK(harness.Layers().Names[2] == "Player");
		harness.Run("scene.setSettings", { { "layerNames", { { "1", "" }, { "2", nullptr } } } });
		CHECK(harness.Layers().Names[1].empty());
		CHECK(harness.Layers().Names[2].empty());
		CHECK_FALSE(harness.Layers().DoCollide(1, 2)); // Names and the matrix are independent

		// Requests that are not valid change nothing, even the valid parts of them.
		harness.Run("scene.setSettings", { { "layerNames", { { "3", "Enemy" } } } });
		harness.Refuse({ { "layerNames", { { "4", "enemy" } } } });
		harness.Refuse({ { "layerNames", { { "4", "Layer 5" } } } });
		harness.Refuse({ { "layerNames", { { "32", "Far" } } } });
		harness.Refuse({ { "layerNames", { { "04", "Leading Zero" } } } });
		harness.Refuse({ { "layerNames", { { "x", "Letter" } } } });
		harness.Refuse({ { "layerNames", { { "4", 5 } } } });
		harness.Refuse({ { "layerNames", { { "4", "Line\nBreak" } } } });
		harness.Refuse({ { "layerNames", { "Enemy" } } });
		harness.Refuse({ { "layerNames", { { "4", "Fine" } } }, { "layerCollisions", { { { "layers", { "Nobody", 0 } }, { "collide", false } } } } });
		harness.Refuse({ { "layerCollisions", { { { "layers", { 0 } }, { "collide", false } } } } });
		harness.Refuse({ { "layerCollisions", { { { "layers", { 0, 32 } }, { "collide", false } } } } });
		harness.Refuse({ { "layerCollisions", { { { "layers", { 0, -1 } }, { "collide", false } } } } });
		harness.Refuse({ { "layerCollisions", { { { "layers", { 0, 1 } } } } } });
		harness.Refuse({ { "layerCollisions", { { { "layers", { 0, 1 } }, { "collide", "no" } } } } });
		harness.Refuse({ { "layerCollisions", { { { "layers", { 0, 1 } }, { "collide", false }, { "extra", 1 } } } } });
		harness.Refuse({ { "layerCollisions", { { "layers", { 0, 1 } }, { "collide", false } } } });
		harness.Refuse({ { "resetLayerCollisions", "yes" } });
		CHECK(harness.Layers().Names[4].empty());

		// A reset lets every pair collide again (names stay), before the request's own pairs apply.
		harness.Run("scene.setSettings", { { "resetLayerCollisions", true }, { "layerCollisions", { { { "layers", { "Enemy", "Enemy" } }, { "collide", false } } } } });
		CHECK(harness.Layers().DoCollide(1, 2));
		CHECK(harness.Layers().DoCollide(0, 9));
		CHECK_FALSE(harness.Layers().DoCollide(3, 3));
		CHECK(harness.Layers().Names[3] == "Enemy");

		// A request that changes nothing records no step.
		const size_t unchanged = harness.Context.GetUndoStack().GetPosition();
		harness.Run("scene.setSettings", { { "layerNames", { { "3", "Enemy" } } } });
		CHECK(harness.Context.GetUndoStack().GetPosition() == unchanged);
	}

	TEST_CASE("scene.setSettings sets sub-scene creation and collision layers together, saved with the scene")
	{
		// SceneSettings gained a field from each of two workstreams (scene streaming's MaxEntitiesInstantiatedPerFrame, the
		// physics layers): one request sets both in one step, an invalid value of either changes neither, and both are saved.
		LayerCommands harness;
		Scene& scene = *harness.Context.GetActiveScene();
		const uint32_t defaultEntities = scene.GetSettings().MaxEntitiesInstantiatedPerFrame;
		const size_t position = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json result = harness.Run("scene.setSettings", {
			{ "maxEntitiesInstantiatedPerFrame", 1500 },
			{ "layerNames", { { "1", "Player" } } },
			{ "layerCollisions", nlohmann::json::array({ { { "layers", { "Player", "Player" } }, { "collide", false } } }) } });
		CHECK(harness.Context.GetUndoStack().GetPosition() == position + 1);
		CHECK(result["settings"]["maxEntitiesInstantiatedPerFrame"] == 1500);
		CHECK(result["settings"]["physicsLayers"]["names"]["1"] == "Player");
		CHECK(scene.GetSettings().MaxEntitiesInstantiatedPerFrame == 1500u);
		CHECK_FALSE(harness.Layers().DoCollide(1, 1));

		std::string error;
		Scene reloaded;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(reloaded, SceneSerializer::Serialize(scene), &error), error);
		CHECK(reloaded.GetSettings().MaxEntitiesInstantiatedPerFrame == 1500u);
		CHECK(reloaded.GetSettings().PhysicsLayers == harness.Layers());

		const auto refuse = [&](const nlohmann::json& parameters)
		{
			harness.Refuse(parameters);
			CHECK(scene.GetSettings().MaxEntitiesInstantiatedPerFrame == 1500u);
		};
		refuse({ { "maxEntitiesInstantiatedPerFrame", 0 }, { "layerNames", { { "2", "Debris" } } } });
		refuse({ { "maxEntitiesInstantiatedPerFrame", 2000 }, { "layerNames", { { "2", 7 } } } });
		refuse({ { "maxEntitiesInstantiatedPerFrame", 2000 }, { "layerCollisions", { { { "layers", { "Nobody", 0 } }, { "collide", false } } } } });

		harness.Run("edit.undo");
		CHECK(scene.GetSettings().MaxEntitiesInstantiatedPerFrame == defaultEntities);
		CHECK(harness.Layers() == PhysicsLayerSettings());
		harness.Run("edit.redo");
		CHECK(scene.GetSettings().MaxEntitiesInstantiatedPerFrame == 1500u);
		CHECK(harness.Layers().Names[1] == "Player");
	}

	TEST_CASE("Bodies and character controllers take their layer by name")
	{
		LayerCommands harness;
		harness.Run("scene.setSettings", { { "layerNames", { { "5", "Pickups" } } } });
		const nlohmann::json created = harness.Run("entity.create", { { "name", "Coin" }, { "components", {
			{ "RigidBody", { { "Layer", "Pickups" } } }, { "BoxCollider", nlohmann::json::object() } } } });
		const Entity coin = harness.Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(created["id"]));
		REQUIRE(coin);
		CHECK(coin.GetComponent<RigidBodyComponent>().Layer == 5u);
		harness.Run("component.set", { { "entity", created["id"] }, { "component", "RigidBody" }, { "values", { { "Layer", "Layer 7" } } } });
		CHECK(coin.GetComponent<RigidBodyComponent>().Layer == 7u);
		harness.Run("component.set", { { "entity", created["id"] }, { "component", "RigidBody" }, { "values", { { "Layer", 2 } } } });
		CHECK(coin.GetComponent<RigidBodyComponent>().Layer == 2u);
		const EditorCommandResult unknown = harness.Commands.Execute(harness.Context, "component.set",
			{ { "entity", created["id"] }, { "component", "RigidBody" }, { "values", { { "Layer", "Ghosts" } } } });
		CHECK(unknown.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(unknown.Error.find("Ghosts") != std::string::npos);
		CHECK(coin.GetComponent<RigidBodyComponent>().Layer == 2u);

		const nlohmann::json walker = harness.Run("entity.create", { { "name", "Walker" }, { "components", {
			{ "CharacterController", { { "Layer", "pickups" } } } } } });
		const Entity controller = harness.Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(walker["id"]));
		REQUIRE(controller);
		CHECK(controller.GetComponent<CharacterControllerComponent>().Layer == 5u);

		// component.list tells agents which properties are layers.
		size_t layerProperties = 0;
		const nlohmann::json list = harness.Run("component.list");
		for (const nlohmann::json& component : list["components"])
		{
			for (const nlohmann::json& property : component["properties"])
			{
				if (property.value("PhysicsLayer", false))
				{
					CHECK(property["Name"] == "Layer");
					layerProperties++;
				}
			}
		}
		CHECK(layerProperties == 2);
	}

	TEST_CASE("Layer changes while playing reach the running physics and end with play mode")
	{
		LayerCommands harness;
		harness.Run("entity.create", { { "name", "Ground" }, { "components", {
			{ "Transform", { { "Translation", { 0, -0.5, 0 } } } }, { "BoxCollider", { { "HalfExtents", { 20, 0.5, 20 } } } } } } });
		harness.Run("entity.create", { { "name", "Crate" }, { "components", {
			{ "Transform", { { "Translation", { 0, 0.5, 0 } } } }, { "RigidBody", { { "Layer", 3 } } }, { "BoxCollider", nlohmann::json::object() } } } });
		harness.Run("play.start", { { "build", false } });
		Scene& running = *harness.Context.GetActiveScene();
		PhysicsSystem* physics = running.GetSystem<PhysicsSystem>();
		REQUIRE(physics != nullptr);
		for (int frame = 0; frame < 30; frame++)
			running.OnUpdateRuntime(running.GetSettings().FixedTimestep);
		const Entity crate = running.FindEntityByName("Crate");
		REQUIRE(glm::vec3(running.GetWorldTransform(crate)[3]).y == doctest::Approx(0.5f).epsilon(0.03));

		const nlohmann::json changed = harness.Run("scene.setSettings", { { "layerCollisions", { { { "layers", { 0, 3 } }, { "collide", false } } } } });
		CHECK(changed.contains("warning"));
		CHECK_FALSE(physics->DoLayersCollide(0, 3));
		for (int frame = 0; frame < 30; frame++)
			running.OnUpdateRuntime(running.GetSettings().FixedTimestep);
		CHECK(glm::vec3(running.GetWorldTransform(crate)[3]).y < 0.0f);

		harness.Run("play.stop");
		CHECK(harness.Layers() == PhysicsLayerSettings());
	}

	TEST_CASE("The Inspector names collision layers and edits the matrix, one undo step per edit")
	{
		LayersEditor editor;
		// The section starts closed, its header summing it up.
		CHECK(editor.ProbeText("Inspector.Layers") == "No names \xC2\xB7 all collide");
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.Layers.Add").has_value());
		REQUIRE(editor.Click("Inspector.Layers"));
		editor.Frames(1);
		// Layer 0 is in the matrix from the start.
		REQUIRE(UI::ItemProbe::Find("Inspector.Layers.Cell.0.0").has_value());
		CHECK(editor.ProbeText("Inspector.Layers.Cell.0.0") == "on");

		size_t position = editor.UndoPosition();
		REQUIRE(editor.Click("Inspector.Layers.Add"));
		editor.Frames(1);
		CHECK(editor.Layers().Names[1] == "New Layer");
		CHECK(editor.UndoPosition() == position + 1);
		REQUIRE(editor.Click("Inspector.Layers.Add"));
		editor.Frames(1);
		CHECK(editor.Layers().Names[2] == "New Layer 2");

		// A name applies when Enter is pressed.
		REQUIRE(editor.ReplaceText("Inspector.Layers.Name.1", "Player"));
		CHECK(editor.Layers().Names[1] == "New Layer");
		editor.PressEnter();
		CHECK(editor.Layers().Names[1] == "Player");
		// A name another layer has is refused (the command logs why) and the field shows the name again.
		REQUIRE(editor.ReplaceText("Inspector.Layers.Name.2", "player"));
		editor.PressEnter();
		CHECK(editor.Layers().Names[2] == "New Layer 2");
		editor.Frames(1);
		CHECK(editor.ProbeText("Inspector.Layers.Name.2") == "New Layer 2");

		// The matrix: a box per pair of shown layers.
		REQUIRE(UI::ItemProbe::Find("Inspector.Layers.Cell.1.2").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.Layers.Cell.2.1").has_value()); // Each pair once
		position = editor.UndoPosition();
		REQUIRE(editor.Click("Inspector.Layers.Cell.1.2"));
		editor.Frames(1);
		CHECK_FALSE(editor.Layers().DoCollide(1, 2));
		CHECK_FALSE(editor.Layers().DoCollide(2, 1));
		CHECK(editor.UndoPosition() == position + 1);
		CHECK(editor.ProbeText("Inspector.Layers.Cell.1.2") == "off");
		CHECK(editor.ProbeText("Inspector.Layers") == "2 named \xC2\xB7 1 pair passes through");
		REQUIRE(editor.Click("Inspector.Layers.Cell.0.0"));
		editor.Frames(1);
		CHECK_FALSE(editor.Layers().DoCollide(0, 0));
		CHECK(editor.ProbeText("Inspector.Layers") == "2 named \xC2\xB7 2 pairs pass through");

		// Removing a name keeps the layer in the matrix while one of its pairs passes through.
		REQUIRE(editor.Click("Inspector.Layers.Clear.2"));
		editor.Frames(1);
		CHECK(editor.Layers().Names[2].empty());
		CHECK(UI::ItemProbe::Find("Inspector.Layers.Cell.1.2").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.Layers.Name.2").has_value());

		editor.Run("edit.undo");
		editor.Run("edit.undo");
		editor.Frames(1);
		CHECK(editor.Layers().Names[2] == "New Layer 2");
		CHECK(editor.Layers().DoCollide(0, 0));
		CHECK_FALSE(editor.Layers().DoCollide(1, 2));
	}

	TEST_CASE("The Inspector offers a body's layer by the scene's layer names")
	{
		LayersEditor editor;
		editor.Run("scene.setSettings", { { "layerNames", { { "4", "Enemies" } } } });
		const UUID id = *UUIDFromJson(editor.Run("entity.create", { { "name", "Goblin" }, { "components", {
			{ "RigidBody", nlohmann::json::object() }, { "BoxCollider", nlohmann::json::object() } } } })["id"]);
		editor.Context().Select(id);
		editor.Frames(2);
		CHECK(editor.ProbeText("Inspector.RigidBody.Layer") == "Layer 0");
		const size_t position = editor.UndoPosition();
		REQUIRE(editor.Click("Inspector.RigidBody.Layer"));
		editor.Frames(1);
		CHECK(editor.ProbeText("Inspector.RigidBody.Layer.4") == "Enemies");
		// Only the default layer and the named ones are offered, then where more are named.
		CHECK(editor.ProbeText("Inspector.RigidBody.Layer.0") == "Layer 0");
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.RigidBody.Layer.7").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.RigidBody.Layer.31").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.RigidBody.Layer.Hint").has_value());
		REQUIRE(editor.Click("Inspector.RigidBody.Layer.4"));
		editor.Frames(1);
		const Entity goblin = editor.Context().GetActiveScene()->GetEntityByUUID(id);
		CHECK(goblin.GetComponent<RigidBodyComponent>().Layer == 4u);
		CHECK(editor.UndoPosition() == position + 1);
		CHECK(editor.ProbeText("Inspector.RigidBody.Layer") == "Enemies");

		// A body on a layer without a name (a script put it there) still finds its layer among the choices.
		editor.Run("component.set", { { "entity", id.ToString() }, { "component", "RigidBody" }, { "values", { { "Layer", 7 } } } });
		editor.Frames(1);
		CHECK(editor.ProbeText("Inspector.RigidBody.Layer") == "Layer 7");
		REQUIRE(editor.Click("Inspector.RigidBody.Layer"));
		editor.Frames(1);
		CHECK(editor.ProbeText("Inspector.RigidBody.Layer.7") == "Layer 7");
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.RigidBody.Layer.8").has_value());
	}

	TEST_CASE("The layer matrix fits long names into a compact grid, elided where too long")
	{
		LayersEditor editor;
		editor.Run("scene.setSettings", { { "layerNames", { { "1", "Player" }, { "2", "Enemies" }, { "3", "Pickups" }, { "4", "Interactable Triggers" },
			{ "5", "EnemyProjectilesAndHazards" }, { "6", "Water" } } } });
		editor.Frames(2);
		REQUIRE(editor.Click("Inspector.Layers"));
		editor.Frames(2);
		// Short names show whole; long ones end in an ellipsis, as wide as the longest label may be (ten text heights).
		CHECK(editor.ProbeText("Inspector.Layers.Label.1") == "1 \xC2\xB7 Player");
		const std::string longName = editor.ProbeText("Inspector.Layers.Label.5");
		CHECK(longName.starts_with("5 \xC2\xB7 EnemyProjectil"));
		CHECK(longName.ends_with("\xE2\x80\xA6"));
		const std::optional<UI::ItemProbe::Item> label = UI::ItemProbe::Find("Inspector.Layers.Label.5");
		REQUIRE(label);
		CHECK(label->Max.x - label->Min.x <= ImGui::GetFontSize() * 10.0f + 1.0f);
		// Seven layers in a grid of boxes of the text's height: the last box sits within the Inspector, no scrolling needed.
		const std::optional<UI::ItemProbe::Item> last = UI::ItemProbe::Find("Inspector.Layers.Cell.6.6");
		REQUIRE(last);
		CHECK(last->Max.y - last->Min.y <= ImGui::GetFontSize() + 1.0f);
		ImGuiWindow* inspector = editor.FindPanelWindow(EditorPanels::c_Inspector);
		REQUIRE(inspector);
		CHECK(last->Max.x <= inspector->InnerRect.Max.x);
	}
}
