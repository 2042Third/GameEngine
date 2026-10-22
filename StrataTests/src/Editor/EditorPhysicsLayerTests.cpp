#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/SaveTestUtils.h"
#include "EditorLayer.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"
#include "UI/CollisionLayersPage.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Physics/PhysicsComponents.h>
#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/ProjectCollisionLayers.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	EditorContextSpecification MakeSpecification()
	{
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		return specification;
	}

	// Runs commands against an editor context without UI, with a project of its own (collision layers are the project's).
	struct LayerCommands
	{
		EditorContext Context { MakeSpecification() };
		EditorCommandRegistry Commands;

		explicit LayerCommands(const std::string& name)
		{
			const std::filesystem::path directory = CreateTemporaryDirectory(name) / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		// A request a command refuses as invalid, changing nothing and recording no step.
		void Refuse(std::string_view command, const nlohmann::json& parameters)
		{
			INFO(parameters.dump());
			const PhysicsLayerSettings before = Layers();
			const nlohmann::json section = Context.GetProject()->GetSettings().GetStoredEntry(ProjectCollisionLayers::c_SectionId);
			const size_t position = Context.GetUndoStack().GetPosition();
			const EditorCommandResult result = Commands.Execute(Context, command, parameters);
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
			CHECK(Layers() == before);
			CHECK(Context.GetProject()->GetSettings().GetStoredEntry(ProjectCollisionLayers::c_SectionId) == section);
			CHECK(Context.GetUndoStack().GetPosition() == position);
		}

		// The layers the active scene runs with: its copy of the project's.
		const PhysicsLayerSettings& Layers() { return Context.GetActiveScene()->GetPhysicsLayers(); }
		// The project's table as its settings hold it.
		PhysicsLayerSettings ProjectLayers()
		{
			const nlohmann::json* value = Context.GetProject()->GetSettings().FindSection(ProjectCollisionLayers::c_SectionId);
			return value ? PhysicsLayerSettings::FromJson(*value) : PhysicsLayerSettings();
		}
	};

	// An editor with a project of its own and nothing selected (the Inspector shows the scene's settings).
	struct LayersEditor : HarnessEditor
	{
		explicit LayersEditor(const std::string& name)
		{
			const std::filesystem::path directory = CreateTemporaryDirectory(name) / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
			Context().ClearSelection();
			Frames(2);
		}

		const PhysicsLayerSettings& Layers() { return Context().GetActiveScene()->GetPhysicsLayers(); }
		size_t UndoPosition() { return Context().GetUndoStack().GetPosition(); }

		std::string ProbeText(std::string_view key)
		{
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
			REQUIRE_MESSAGE(item.has_value(), "No ", std::string(key), " in the last frame");
			return item->Text;
		}

		// The Collision Layers page in the Project Settings panel, once it shows.
		void OpenPage()
		{
			Layer->ShowProjectSettings(ProjectCollisionLayers::c_SectionId);
			REQUIRE(FramesUntil([]() { return UI::ItemProbe::Find(PageKey("Summary")).has_value(); }));
			Frames(1);
		}

		static std::string PageKey(std::string_view item)
		{
			return std::string(CollisionLayersPage::c_ProbePrefix) + std::string(item);
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
	TEST_CASE("setCollisionLayers names, resets and sets pairs with one undo step")
	{
		LayerCommands harness("SetCollisionLayers");
		const size_t position = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json result = harness.Run("project.setCollisionLayers", {
			{ "layerNames", { { "1", "Player" }, { "2", "Debris" } } },
			{ "layerCollisions", nlohmann::json::array({
				{ { "layers", { "Player", "debris" } }, { "collide", false } },
				{ { "layers", { 2, 2 } }, { "collide", false } },
				{ { "layers", { "Layer 9", 0 } }, { "collide", false } } }) } });
		CHECK(harness.Context.GetUndoStack().GetPosition() == position + 1);
		// A project step: the scene stays saved, and the project's settings hold the table.
		CHECK_FALSE(harness.Context.IsSceneModified());
		const nlohmann::json expected = {
			{ "names", { { "1", "Player" }, { "2", "Debris" } } },
			{ "noCollisions", nlohmann::json::array({ { "Layer 0", "Layer 9" }, { "Player", "Debris" }, { "Debris", "Debris" } }) } };
		CHECK(result["physicsLayers"] == expected);
		CHECK(harness.Run("scene.info")["settings"]["physicsLayers"] == expected);
		CHECK_FALSE(harness.Layers().DoCollide(2, 1));
		CHECK(harness.ProjectLayers() == harness.Layers());
		const nlohmann::json stored = harness.Run("project.settings", { { "section", ProjectCollisionLayers::c_SectionId } });
		CHECK(stored["sections"][0]["value"]["Names"]["1"] == "Player");

		harness.Run("edit.undo");
		CHECK(harness.Layers() == PhysicsLayerSettings());
		CHECK_FALSE(harness.Context.GetProject()->GetSettings().IsStored(ProjectCollisionLayers::c_SectionId));
		harness.Run("edit.redo");
		CHECK(harness.Layers().Names[1] == "Player");
		CHECK_FALSE(harness.Layers().DoCollide(1, 2));

		// Layers swap names in one call; an empty name or null clears one.
		harness.Run("project.setCollisionLayers", { { "layerNames", { { "1", "Debris" }, { "2", "Player" } } } });
		CHECK(harness.Layers().Names[1] == "Debris");
		CHECK(harness.Layers().Names[2] == "Player");
		harness.Run("project.setCollisionLayers", { { "layerNames", { { "1", "" }, { "2", nullptr } } } });
		CHECK(harness.Layers().Names[1].empty());
		CHECK(harness.Layers().Names[2].empty());
		CHECK_FALSE(harness.Layers().DoCollide(1, 2)); // Names and the matrix are independent

		// Requests that are not valid change nothing, even the valid parts of them.
		harness.Run("project.setCollisionLayers", { { "layerNames", { { "3", "Enemy" } } } });
		const auto refuse = [&harness](const nlohmann::json& parameters) { harness.Refuse("project.setCollisionLayers", parameters); };
		refuse({ { "layerNames", { { "4", "enemy" } } } });
		refuse({ { "layerNames", { { "4", "Layer 5" } } } });
		refuse({ { "layerNames", { { "32", "Far" } } } });
		refuse({ { "layerNames", { { "04", "Leading Zero" } } } });
		refuse({ { "layerNames", { { "x", "Letter" } } } });
		refuse({ { "layerNames", { { "4", 5 } } } });
		refuse({ { "layerNames", { { "4", "Line\nBreak" } } } });
		refuse({ { "layerNames", { "Enemy" } } });
		refuse({ { "layerNames", { { "4", "Fine" } } }, { "layerCollisions", { { { "layers", { "Nobody", 0 } }, { "collide", false } } } } });
		refuse({ { "layerCollisions", { { { "layers", { 0 } }, { "collide", false } } } } });
		refuse({ { "layerCollisions", { { { "layers", { 0, 32 } }, { "collide", false } } } } });
		refuse({ { "layerCollisions", { { { "layers", { 0, -1 } }, { "collide", false } } } } });
		refuse({ { "layerCollisions", { { { "layers", { 0, 1 } } } } } });
		refuse({ { "layerCollisions", { { { "layers", { 0, 1 } }, { "collide", "no" } } } } });
		refuse({ { "layerCollisions", { { { "layers", { 0, 1 } }, { "collide", false }, { "extra", 1 } } } } });
		refuse({ { "layerCollisions", { { "layers", { 0, 1 } }, { "collide", false } } } });
		refuse({ { "resetLayerCollisions", "yes" } });
		CHECK(harness.Layers().Names[4].empty());

		// A reset lets every pair collide again (names stay), before the request's own pairs apply.
		harness.Run("project.setCollisionLayers", { { "resetLayerCollisions", true }, { "layerCollisions", { { { "layers", { "Enemy", "Enemy" } }, { "collide", false } } } } });
		CHECK(harness.Layers().DoCollide(1, 2));
		CHECK(harness.Layers().DoCollide(0, 9));
		CHECK_FALSE(harness.Layers().DoCollide(3, 3));
		CHECK(harness.Layers().Names[3] == "Enemy");

		// A request that changes nothing records no step.
		const size_t unchanged = harness.Context.GetUndoStack().GetPosition();
		harness.Run("project.setCollisionLayers", { { "layerNames", { { "3", "Enemy" } } } });
		CHECK(harness.Context.GetUndoStack().GetPosition() == unchanged);

		// The default table is no section: a project without layers stores none.
		harness.Run("project.setCollisionLayers", { { "layerNames", { { "3", "" } } }, { "resetLayerCollisions", true } });
		CHECK(harness.Layers() == PhysicsLayerSettings());
		CHECK_FALSE(harness.Context.GetProject()->GetSettings().IsStored(ProjectCollisionLayers::c_SectionId));
	}

	TEST_CASE("scene.setSettings refuses layer parameters naming the new command")
	{
		LayerCommands harness("SceneSettingsRefusesLayers");
		harness.Run("project.setCollisionLayers", { { "layerNames", { { "1", "Player" } } } });
		for (const nlohmann::json& parameters : { nlohmann::json { { "layerNames", { { "2", "Debris" } } } },
			nlohmann::json { { "resetLayerCollisions", true } },
			nlohmann::json { { "gravity", { 0, -5, 0 } }, { "layerCollisions", { { { "layers", { 0, 1 } }, { "collide", false } } } } } })
		{
			harness.Refuse("scene.setSettings", parameters);
			const EditorCommandResult result = harness.Commands.Execute(harness.Context, "scene.setSettings", parameters);
			CHECK(result.Error.find("project.setCollisionLayers") != std::string::npos);
		}
		CHECK(harness.Context.GetActiveScene()->GetSettings().Gravity.y == doctest::Approx(-9.81f));
		CHECK_FALSE(harness.Context.IsSceneModified());
	}

	TEST_CASE("scene.setSettings sets sub-scene creation, saved with the scene, and the saved scene stores no layers")
	{
		LayerCommands harness("SceneSettingsSaved");
		Scene& scene = *harness.Context.GetActiveScene();
		const uint32_t defaultEntities = scene.GetSettings().MaxEntitiesInstantiatedPerFrame;
		harness.Run("project.setCollisionLayers", { { "layerNames", { { "1", "Player" } } },
			{ "layerCollisions", nlohmann::json::array({ { { "layers", { "Player", "Player" } }, { "collide", false } } }) } });
		const size_t position = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json result = harness.Run("scene.setSettings", { { "maxEntitiesInstantiatedPerFrame", 1500 } });
		CHECK(harness.Context.GetUndoStack().GetPosition() == position + 1);
		CHECK(result["settings"]["maxEntitiesInstantiatedPerFrame"] == 1500);
		// The result shows the layers the scene runs with: the project's.
		CHECK(result["settings"]["physicsLayers"]["names"]["1"] == "Player");
		CHECK(scene.GetSettings().MaxEntitiesInstantiatedPerFrame == 1500u);
		harness.Refuse("scene.setSettings", { { "maxEntitiesInstantiatedPerFrame", 0 } });
		CHECK(scene.GetSettings().MaxEntitiesInstantiatedPerFrame == 1500u);

		// Saved: the setting is in the file, the layers are not (they are the project's).
		harness.Run("scene.saveAs", { { "path", "Scenes/Level.stscene" } });
		const std::optional<std::string> text = FileSystem::ReadText(harness.Context.GetProject()->GetAssetDirectory() / "Scenes" / "Level.stscene");
		REQUIRE(text.has_value());
		CHECK(text->find("\"MaxEntitiesInstantiatedPerFrame\":1500") != std::string::npos);
		CHECK(text->find("PhysicsLayers") == std::string::npos);

		harness.Run("edit.undo");
		CHECK(harness.Context.GetActiveScene()->GetSettings().MaxEntitiesInstantiatedPerFrame == defaultEntities);
		CHECK(harness.Layers().Names[1] == "Player"); // A scene step leaves the project's layers alone
		harness.Run("edit.redo");
		CHECK(harness.Context.GetActiveScene()->GetSettings().MaxEntitiesInstantiatedPerFrame == 1500u);
	}

	TEST_CASE("Bodies and character controllers take their layer by the project's layer names")
	{
		LayerCommands harness("LayersByName");
		harness.Run("project.setCollisionLayers", { { "layerNames", { { "5", "Pickups" } } } });
		const nlohmann::json created = harness.Run("entity.create", { { "name", "Coin" }, { "components", {
			{ "RigidBody", { { "Layer", "Pickups" } } }, { "BoxCollider", nlohmann::json::object() } } } });
		const Entity coin = harness.Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(created["id"]));
		REQUIRE(coin);
		CHECK(coin.GetComponent<RigidBodyComponent>().Layer == 5u);

		SUBCASE("component.set accepts a project layer name")
		{
			harness.Run("project.setCollisionLayers", { { "layerNames", { { "6", "Water" } } } });
			harness.Run("component.set", { { "entity", created["id"] }, { "component", "RigidBody" }, { "values", { { "Layer", "water" } } } });
			CHECK(coin.GetComponent<RigidBodyComponent>().Layer == 6u);
		}
		harness.Run("component.set", { { "entity", created["id"] }, { "component", "RigidBody" }, { "values", { { "Layer", "Layer 7" } } } });
		CHECK(coin.GetComponent<RigidBodyComponent>().Layer == 7u);
		harness.Run("component.set", { { "entity", created["id"] }, { "component", "RigidBody" }, { "values", { { "Layer", 2 } } } });
		CHECK(coin.GetComponent<RigidBodyComponent>().Layer == 2u);
		const EditorCommandResult unknown = harness.Commands.Execute(harness.Context, "component.set",
			{ { "entity", created["id"] }, { "component", "RigidBody" }, { "values", { { "Layer", "Ghosts" } } } });
		CHECK(unknown.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(unknown.Error.find("Ghosts") != std::string::npos);
		CHECK(unknown.Error.find("project.setCollisionLayers") != std::string::npos);
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

	TEST_CASE("A change while playing reaches the running copy and persists after play mode")
	{
		LayerCommands harness("LayersWhilePlaying");
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
		// A script's change of the running copy (Physics::SetLayersCollide), which a change of the table replaces.
		running.GetPhysicsLayers().SetCollide(5, 6, false);

		const size_t position = harness.Context.GetUndoStack().GetPosition();
		harness.Run("project.setCollisionLayers", { { "layerCollisions", { { { "layers", { 0, 3 } }, { "collide", false } } } } });
		CHECK(harness.Context.GetUndoStack().GetPosition() == position + 1);
		CHECK_FALSE(physics->DoLayersCollide(0, 3));
		CHECK(running.GetPhysicsLayers().DoCollide(5, 6));
		for (int frame = 0; frame < 30; frame++)
			running.OnUpdateRuntime(running.GetSettings().FixedTimestep);
		CHECK(glm::vec3(running.GetWorldTransform(crate)[3]).y < 0.0f);

		// The project's table stays after play mode, in the edited scene too.
		harness.Run("play.stop");
		CHECK_FALSE(harness.Layers().DoCollide(0, 3));
		CHECK_FALSE(harness.ProjectLayers().DoCollide(0, 3));
	}

	TEST_CASE("The Collision Layers page names layers and edits the matrix through setCollisionLayers, one undo step per edit")
	{
		LayersEditor editor("CollisionLayersPage");
		editor.OpenPage();
		CHECK(editor.ProbeText(LayersEditor::PageKey("Summary")) == "No names \xC2\xB7 all collide");
		// Layer 0 is in the matrix from the start.
		REQUIRE(UI::ItemProbe::Find(LayersEditor::PageKey("Cell.0.0")).has_value());
		CHECK(editor.ProbeText(LayersEditor::PageKey("Cell.0.0")) == "on");

		size_t position = editor.UndoPosition();
		REQUIRE(editor.Click(LayersEditor::PageKey("Add")));
		editor.Frames(1);
		CHECK(editor.Layers().Names[1] == "New Layer");
		CHECK(editor.UndoPosition() == position + 1);
		CHECK_FALSE(editor.Context().IsSceneModified());
		REQUIRE(editor.Click(LayersEditor::PageKey("Add")));
		editor.Frames(1);
		CHECK(editor.Layers().Names[2] == "New Layer 2");

		// A name applies when Enter is pressed.
		REQUIRE(editor.ReplaceText(LayersEditor::PageKey("Name.1"), "Player"));
		CHECK(editor.Layers().Names[1] == "New Layer");
		editor.PressEnter();
		CHECK(editor.Layers().Names[1] == "Player");
		// A name another layer has is refused (the page says why) and the field shows the name again.
		REQUIRE(editor.ReplaceText(LayersEditor::PageKey("Name.2"), "player"));
		editor.PressEnter();
		CHECK(editor.Layers().Names[2] == "New Layer 2");
		editor.Frames(1);
		CHECK(editor.ProbeText(LayersEditor::PageKey("Name.2")) == "New Layer 2");

		// The matrix: a box per pair of shown layers.
		REQUIRE(UI::ItemProbe::Find(LayersEditor::PageKey("Cell.1.2")).has_value());
		CHECK_FALSE(UI::ItemProbe::Find(LayersEditor::PageKey("Cell.2.1")).has_value()); // Each pair once
		position = editor.UndoPosition();
		REQUIRE(editor.Click(LayersEditor::PageKey("Cell.1.2")));
		editor.Frames(1);
		CHECK_FALSE(editor.Layers().DoCollide(1, 2));
		CHECK_FALSE(editor.Layers().DoCollide(2, 1));
		CHECK(editor.UndoPosition() == position + 1);
		CHECK(editor.ProbeText(LayersEditor::PageKey("Cell.1.2")) == "off");
		CHECK(editor.ProbeText(LayersEditor::PageKey("Summary")) == "2 named \xC2\xB7 1 pair passes through");
		REQUIRE(editor.Click(LayersEditor::PageKey("Cell.0.0")));
		editor.Frames(1);
		CHECK_FALSE(editor.Layers().DoCollide(0, 0));
		CHECK(editor.ProbeText(LayersEditor::PageKey("Summary")) == "2 named \xC2\xB7 2 pairs pass through");

		// Removing a name keeps the layer in the matrix while one of its pairs passes through.
		REQUIRE(editor.Click(LayersEditor::PageKey("Clear.2")));
		editor.Frames(1);
		CHECK(editor.Layers().Names[2].empty());
		CHECK(UI::ItemProbe::Find(LayersEditor::PageKey("Cell.1.2")).has_value());
		CHECK_FALSE(UI::ItemProbe::Find(LayersEditor::PageKey("Name.2")).has_value());

		editor.Run("edit.undo");
		editor.Run("edit.undo");
		editor.Frames(1);
		CHECK(editor.Layers().Names[2] == "New Layer 2");
		CHECK(editor.Layers().DoCollide(0, 0));
		CHECK_FALSE(editor.Layers().DoCollide(1, 2));
		CHECK(editor.ProbeText(LayersEditor::PageKey("Summary")) == "2 named \xC2\xB7 1 pair passes through");
	}

	TEST_CASE("The Collision Layers page refuses edits until the project is ready, saying why")
	{
		// The project opens with its assets scanned in the background, held back while the gate is closed: commands that edit
		// wait for the scan, which nothing on a page polls.
		GatedJobSystem jobs;
		std::string error;
		const std::optional<ManyAssetsProject> project = WriteManyAssetsProject(CreateTemporaryDirectory("CollisionLayersPageWaits") / "Many", 4, error);
		REQUIRE_MESSAGE(project.has_value(), error);
		EditorOptions options;
		options.ProjectPath = project->Directory;
		std::optional<HarnessEditor> editor;
		const std::string summary = LayersEditor::PageKey("Summary");
		const std::string refusal = LayersEditor::PageKey("Error");
		{
			BackgroundGate gate;
			editor.emplace(ImGuiHarness::Specification {}, options, std::string(), false, true);
			editor->Frames(3);
			REQUIRE(editor->Context().IsWaitingForAssets());
			editor->Layer->ShowProjectSettings(ProjectCollisionLayers::c_SectionId);
			REQUIRE(editor->FramesUntil([&]() { return UI::ItemProbe::Find(summary).has_value(); }));
			editor->Frames(1);
			REQUIRE(editor->Click(LayersEditor::PageKey("Add")));
			editor->Frames(1);
			// Refused at once with the reason, rather than left waiting where nothing would ever run it.
			const std::optional<UI::ItemProbe::Item> said = UI::ItemProbe::Find(refusal);
			REQUIRE(said.has_value());
			CHECK(said->Text.find("Not yet") != std::string::npos);
			CHECK(said->Text.find("still being scanned") != std::string::npos);
		}
		// The scan ends: the refused edit never runs, and the same click names the layer now.
		REQUIRE(editor->FramesUntil([&]() { return !editor->Context().IsWaitingForAssets(); }, std::chrono::minutes(2)));
		editor->Frames(2);
		CHECK(editor->Context().GetActiveScene()->GetPhysicsLayers().Names[1].empty());
		REQUIRE(editor->Click(LayersEditor::PageKey("Add")));
		editor->Frames(1);
		CHECK(editor->Context().GetActiveScene()->GetPhysicsLayers().Names[1] == "New Layer");
		CHECK_FALSE(UI::ItemProbe::Find(refusal).has_value());
	}

	TEST_CASE("The Inspector links to the page")
	{
		LayersEditor editor("InspectorLinksLayers");
		editor.Run("project.setCollisionLayers", { { "layerNames", { { "1", "Player" }, { "4", "Enemies" } } } });
		editor.Frames(1);
		CHECK(editor.ProbeText("Inspector.Layers") == "Collision layers: 2 named");
		CHECK_FALSE(UI::ItemProbe::Find(LayersEditor::PageKey("Summary")).has_value());
		REQUIRE(editor.Click("Inspector.Layers.Open"));
		REQUIRE(editor.FramesUntil([]() { return UI::ItemProbe::Find(LayersEditor::PageKey("Summary")).has_value(); }));
		editor.Frames(1);
		CHECK(editor.ProbeText(LayersEditor::PageKey("Summary")) == "2 named \xC2\xB7 all collide");
		CHECK(editor.ProbeText(LayersEditor::PageKey("Name.4")) == "Enemies");
	}

	TEST_CASE("The Inspector offers a body's layer by the project's layer names")
	{
		LayersEditor editor("InspectorLayerChoice");
		editor.Run("project.setCollisionLayers", { { "layerNames", { { "4", "Enemies" } } } });
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
		LayersEditor editor("CompactLayerMatrix");
		editor.Run("project.setCollisionLayers", { { "layerNames", { { "1", "Player" }, { "2", "Enemies" }, { "3", "Pickups" }, { "4", "Interactable Triggers" },
			{ "5", "EnemyProjectilesAndHazards" }, { "6", "Water" } } } });
		editor.OpenPage();
		// Short names show whole; long ones end in an ellipsis, as wide as the longest label may be (ten text heights).
		CHECK(editor.ProbeText(LayersEditor::PageKey("Label.1")) == "1 \xC2\xB7 Player");
		const std::string longName = editor.ProbeText(LayersEditor::PageKey("Label.5"));
		CHECK(longName.starts_with("5 \xC2\xB7 EnemyProjectil"));
		CHECK(longName.ends_with("\xE2\x80\xA6"));
		const std::optional<UI::ItemProbe::Item> label = UI::ItemProbe::Find(LayersEditor::PageKey("Label.5"));
		REQUIRE(label);
		CHECK(label->Max.x - label->Min.x <= ImGui::GetFontSize() * 10.0f + 1.0f);
		// Seven layers in a grid of boxes of the text's height: the last box sits within the panel, no scrolling needed.
		const std::optional<UI::ItemProbe::Item> last = UI::ItemProbe::Find(LayersEditor::PageKey("Cell.6.6"));
		REQUIRE(last);
		CHECK(last->Max.y - last->Min.y <= ImGui::GetFontSize() + 1.0f);
		ImGuiWindow* panel = editor.FindPanelWindow(EditorPanels::c_ProjectSettings);
		REQUIRE(panel);
		CHECK(last->Max.x <= panel->InnerRect.Max.x);
	}
}
