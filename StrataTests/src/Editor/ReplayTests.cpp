#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorAutomation.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/PropertyEdit.h"
#include "Editor/SceneComparison.h"
#include "Network/NetworkTestHelpers.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Core/UUID.h>
#include <Strata/Network/RpcClient.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

// Command scripts that bind results of earlier steps, and sessions replayed from the activity journal
// (editor.activity.export).

using namespace Strata;

namespace
{

	constexpr std::chrono::milliseconds c_CallTimeout = std::chrono::milliseconds(20000);

	EditorContextSpecification MakeSpecification()
	{
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		return specification;
	}

	// An editor without UI whose frames the test runs: context, commands, runner and automation server.
	struct ReplayEditor
	{
		EditorContext Context { MakeSpecification() };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		// Declared last: stopped before the runner and the context go away, as in the editor.
		EditorAutomation Automation { Context, Commands, Runner };

		ReplayEditor() = default;
		ReplayEditor(const ReplayEditor&) = delete;
		ReplayEditor& operator=(const ReplayEditor&) = delete;

		~ReplayEditor()
		{
			Runner.CancelAll("The test is over");
			Automation.Stop();
		}

		void Frame()
		{
			Context.Update(Timestep(1.0f / 60.0f));
			Runner.Update(Context);
			Automation.Update();
		}

		bool FramesUntil(const std::function<bool()>& condition)
		{
			const auto deadline = std::chrono::steady_clock::now() + c_CallTimeout;
			while (!condition())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				Frame();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
		}

		// Runs a command script to its end, a step per frame like the editor; returns whether a step failed.
		bool RunScript(EditorCommandScript& script)
		{
			REQUIRE(FramesUntil([&]() { return script.Update(Runner, Context, Commands); }));
			return script.HasFailed();
		}

		// The edited scene as its file would hold it.
		nlohmann::json SceneDocument() const { return SceneSerializer::Serialize(*Context.GetEditScene()); }
	};

	// A tool driving an editor through its automation server, as an agent does.
	struct AgentClient
	{
		ReplayEditor& Editor;
		RpcClient Client;

		explicit AgentClient(ReplayEditor& editor)
			: Editor(editor)
		{
			EditorAutomationSpecification specification;
			specification.AuthToken = Tests::c_TestServerToken;
			specification.PublishSession = false;
			std::string error;
			REQUIRE_MESSAGE(Editor.Automation.Start(specification, &error), error);
			REQUIRE_MESSAGE(Client.Connect("127.0.0.1", Editor.Automation.GetPort(), Tests::c_TestServerToken, std::chrono::milliseconds(5000), "ReplayAgent"),
				Client.GetLastError());
		}

		// Calls a method from another thread while this one runs the editor's frames; REQUIREs that it succeeded.
		nlohmann::json Call(const std::string& method, const nlohmann::json& params = nlohmann::json::object())
		{
			std::future<RpcResult> call = std::async(std::launch::async, [this, method, params]() { return Client.Call(method, params, c_CallTimeout); });
			REQUIRE(Editor.FramesUntil([&call]() { return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
			const RpcResult result = call.get();
			INFO(method, " ", params.dump(), ": ", result.GetError().Message);
			REQUIRE(result.IsSuccess());
			return result.GetValue();
		}

		std::string Create(const nlohmann::json& params) { return Call("entity.create", params)["id"].get<std::string>(); }
	};

	// What an agent builds in a basic3d project in 51 commands: materials, entities with components in a hierarchy, a prefab
	// and its instances, copies, an undone and redone deletion, a template entity looked up and changed, and a save.
	void BuildScene(AgentClient& agent)
	{
		const std::string red = agent.Call("material.create", { { "path", "Materials/Red.stmat" }, { "properties", { { "BaseColor", { 1, 0, 0, 1 } } } } })["asset"].get<std::string>();
		agent.Call("material.create", { { "path", "Materials/Metal.stmat" }, { "properties", { { "Metallic", 1 }, { "Roughness", 0.2 } } } });
		const std::string green = agent.Call("material.create", { { "path", "Materials/Green.stmat" }, { "properties", { { "BaseColor", { 0, 1, 0, 1 } } } } })["asset"].get<std::string>();
		// The template's sun, found by name: its ID is the project's, so the replay looks it up again.
		const std::string templateSun = agent.Call("entity.find", { { "name", "Sun" } })["entities"][0].get<std::string>();
		agent.Call("component.set", { { "entity", templateSun }, { "component", "DirectionalLight" }, { "values", { { "Intensity", 2.5 } } } });
		const std::string floor = agent.Create({ { "name", "Floor" }, { "components", { { "MeshRenderer", { { "Mesh", "Builtin/Plane" }, { "Material", red } } },
			{ "Transform", { { "Scale", { 10, 1, 10 } } } } } } });
		const std::string crate = agent.Create({ { "name", "Crate" }, { "components", { { "MeshRenderer", { { "Mesh", "Builtin/Cube" },
			{ "Material", "Materials/Metal.stmat" } } }, { "Transform", { { "Translation", { 0, 0.5, 0 } } } } } } });
		const std::string lamp = agent.Create({ { "name", "Lamp" }, { "parent", crate }, { "components", { { "PointLight", { { "Intensity", 5 }, { "Range", 8 } } } } } });
		const std::string pillar = agent.Create({ { "name", "Pillar" }, { "components", { { "MeshRenderer", { { "Mesh", "Builtin/Cylinder" }, { "Material", green } } } } } });
		const std::string group = agent.Create({ { "name", "Group" } });
		agent.Call("entity.setParent", { { "entities", { pillar } }, { "parent", group } });
		agent.Call("component.set", { { "entity", crate }, { "component", "Transform" }, { "values", { { "Rotation", { 0, 45, 0 } } } } });
		agent.Call("component.add", { { "entity", crate }, { "component", "RigidBody" }, { "values", { { "Mass", 4 } } } });
		agent.Call("component.add", { { "entity", crate }, { "component", "BoxCollider" } });
		const std::string copy = agent.Call("entity.duplicate", { { "entities", { crate } } })["entities"][0].get<std::string>();
		agent.Call("entity.rename", { { "entity", copy }, { "name", "Crate B" } });
		agent.Call("component.set", { { "entity", copy }, { "component", "Transform" }, { "values", { { "Translation", { 2, 0.5, 0 } } } } });
		agent.Call("prefab.create", { { "entities", { crate } }, { "path", "Prefabs/Crate.stprefab" } });
		agent.Call("prefab.instantiate", { { "prefab", "Prefabs/Crate.stprefab" }, { "components", { { "Transform", { { "Translation", { -2, 0.5, 0 } } } } } } });
		agent.Call("prefab.instantiate", { { "prefab", "Prefabs/Crate.stprefab" }, { "parent", group } });
		const std::string third = agent.Call("prefab.instantiate", { { "prefab", "Prefabs/Crate.stprefab" } })["entities"][0].get<std::string>();
		agent.Call("component.set", { { "entity", third }, { "component", "Transform" }, { "values", { { "Translation", { 4, 0.5, 2 } } } } });
		const std::string sun = agent.Create({ { "name", "Key Light" }, { "components", { { "DirectionalLight", { { "Intensity", 3 } } },
			{ "Transform", { { "Rotation", { -50, 30, 0 } } } } } } });
		const std::string camera = agent.Create({ { "name", "Camera" }, { "components", { { "Camera", { { "Primary", false } } },
			{ "Transform", { { "Translation", { 0, 2, 6 } } } } } } });
		agent.Call("entity.setActive", { { "entities", { lamp } }, { "active", false } });
		agent.Call("entity.setActive", { { "entities", { lamp } }, { "active", true } });
		const std::string temporary = agent.Create({ { "name", "Temporary" } });
		agent.Call("entity.delete", { { "entities", { temporary } } });
		agent.Call("edit.undo");
		agent.Call("edit.redo");
		agent.Call("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "BaseColor", { 0.8, 0.1, 0.1, 1 } } } } });
		agent.Call("component.set", { { "entity", floor }, { "component", "MeshRenderer" }, { "values", { { "Material", green } } } });
		agent.Create({ { "name", "Title" }, { "components", { { "Text", { { "Text", "Hello" }, { "FontSize", 24 } } } } } });
		agent.Call("component.remove", { { "entity", crate }, { "component", "BoxCollider" } });
		const nlohmann::json copied = agent.Call("entity.copy", { { "entities", { pillar } } });
		agent.Call("entity.paste", { { "data", copied["data"] }, { "parent", group } });
		agent.Call("scene.setSettings", { { "gravity", { 0, -5, 0 } } });
		const std::string first = agent.Create({ { "name", "First" }, { "parent", floor } });
		const std::string second = agent.Create({ { "name", "Second" }, { "parent", floor } });
		agent.Call("entity.setParent", { { "entities", { second } }, { "parent", floor }, { "index", 0 } });
		agent.Call("component.add", { { "entity", first }, { "component", "MeshRenderer" }, { "values", { { "Mesh", "Builtin/Sphere" } } } });
		agent.Call("entity.rename", { { "entity", first }, { "name", "Ball" } });
		agent.Call("component.set", { { "entity", first }, { "component", "Transform" }, { "values", { { "Scale", { 0.5, 0.5, 0.5 } } } } });
		agent.Call("entity.duplicate", { { "entities", { first, second } } });
		const std::string ball = agent.Call("entity.find", { { "name", "Ball" } })["entities"][0].get<std::string>();
		agent.Call("component.set", { { "entity", ball }, { "component", "Transform" }, { "values", { { "Translation", { 0, 3, 0 } } } } });
		agent.Call("entity.get", { { "entity", crate } });
		agent.Call("component.set", { { "entity", lamp }, { "component", "PointLight" }, { "values", { { "Color", { 1, 0.8, 0.6 } } } } });
		const std::string marker = agent.Create({ { "name", "Marker" }, { "parent", camera }, { "components", { { "Transform", { { "Translation", { 1, 1, 1 } } } } } } });
		agent.Call("entity.setParent", { { "entities", { marker } }, { "parent", nullptr } });
		agent.Call("entity.setActive", { { "entities", { pillar } }, { "active", false } });
		agent.Call("component.set", { { "entity", sun }, { "component", "DirectionalLight" }, { "values", { { "Intensity", 2 } } } });
		agent.Call("scene.save");
	}

	nlohmann::json ReadJsonFile(const std::filesystem::path& path)
	{
		const std::optional<std::string> text = FileSystem::ReadText(path);
		REQUIRE_MESSAGE(text.has_value(), FileSystem::ToUTF8(path));
		const nlohmann::json json = nlohmann::json::parse(*text, nullptr, false);
		REQUIRE_FALSE(json.is_discarded());
		return json;
	}

	std::unordered_map<std::string, std::string> ReadAssetNames(const std::filesystem::path& project)
	{
		std::optional<std::unordered_map<std::string, std::string>> assets = Tests::ReadAssetNames(project / "Assets");
		REQUIRE(assets.has_value());
		return std::move(*assets);
	}

	// Compares the documents of two projects (Tests::ProjectNormalizer): references to their assets by the assets' paths, the
	// other IDs numbered across the documents in the order they are compared.
	struct ProjectComparison
	{
		std::filesystem::path FirstProject;
		std::filesystem::path SecondProject;
		Tests::ProjectNormalizer First { ReadAssetNames(FirstProject) };
		Tests::ProjectNormalizer Second { ReadAssetNames(SecondProject) };

		void Check(const nlohmann::json& first, const nlohmann::json& second)
		{
			const nlohmann::json a = First.Normalize(first);
			const nlohmann::json b = Second.Normalize(second);
			INFO(Tests::DescribeFirstDifference(a, b));
			CHECK(a == b);
		}

		// The same document of both projects, by its path relative to their asset directories.
		void CheckFile(const std::string& document)
		{
			CAPTURE(document);
			Check(ReadJsonFile(FirstProject / "Assets" / document), ReadJsonFile(SecondProject / "Assets" / document));
		}
	};

	// A basic3d project, and a copy of it as a session finds it (without the editor's local data).
	void CreateProjectAndCopy(ReplayEditor& editor, const std::filesystem::path& original, const std::filesystem::path& copy)
	{
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(original) }, { "name", "Game" }, { "template", "basic3d" } });
		std::string error;
		REQUIRE_MESSAGE(FileSystem::CopyDirectory(original, copy, &error), error);
		REQUIRE(FileSystem::Remove(copy / ".strata"));
	}

	// Runs an exported session on the copy; returns the editor that ran it.
	void Replay(ReplayEditor& replay, const std::filesystem::path& copy, const std::filesystem::path& session)
	{
		replay.Run("project.open", { { "path", FileSystem::ToUTF8(copy) } });
		std::string error;
		Scope<EditorCommandScript> script = EditorCommandScript::Load(session, &error);
		REQUIRE_MESSAGE(script, error);
		CHECK_FALSE(replay.RunScript(*script));
	}

}

TEST_SUITE("Editor.CommandScriptBindings")
{
	TEST_CASE("Steps bind results that later steps use, also inside objects and arrays")
	{
		ReplayEditor editor;
		std::string error;
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "entity.create", "parameters": { "name": "Crate" }, "bind": { "crate": "/id" } },
			{ "command": "entity.create", "parameters": { "name": "Child", "parent": { "$ref": "crate" } }, "bind": { "child": "/id" } },
			{ "command": "component.set", "parameters": { "entities": [ { "$ref": "crate" }, { "$ref": "child" } ], "component": "Transform",
				"values": { "Translation": [ 1, 2, 3 ] } } },
			{ "command": "entity.duplicate", "parameters": { "entities": [ { "$ref": "crate" } ] }, "bind": { "copies": "" } },
			{ "command": "entity.rename", "parameters": { "entity": { "$ref": "copies", "pointer": "/entities/0" }, "name": "Copy" } },
			{ "command": "entity.get", "parameters": { "entity": { "$ref": "child" } }, "expect": { "/name": { "equals": "Child" } } }
		])"), &error);
		REQUIRE_MESSAGE(script, error);
		CHECK_FALSE(editor.RunScript(*script));

		const std::optional<UUID> crate = UUID::FromString(script->GetBindings().at("crate").get<std::string>());
		const std::optional<UUID> child = UUID::FromString(script->GetBindings().at("child").get<std::string>());
		REQUIRE(crate.has_value());
		REQUIRE(child.has_value());
		const Scene& scene = *editor.Context.GetEditScene();
		const Entity childEntity = scene.GetEntityByUUID(*child);
		REQUIRE(childEntity);
		CHECK(childEntity.GetParent().GetUUID() == *crate);
		CHECK(editor.Run("component.get", { { "entity", UUIDToJson(*child) }, { "component", "Transform" } })["values"]["Translation"]
			== nlohmann::json::array({ 1, 2, 3 }));
		CHECK(scene.FindEntityByName("Copy"));
		CHECK(script->GetBindings().at("copies")["entities"].size() == 1);
	}

	TEST_CASE("References to names no earlier step binds fail the script before anything runs")
	{
		std::string error;
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "entity.create", "parameters": { "name": "A" } },
			{ "command": "entity.rename", "parameters": { "entity": { "$ref": "crate" }, "name": "B" } }
		])"), &error));
		CHECK(error.find("Step 2") != std::string::npos);
		CHECK(error.find("unknown binding 'crate'") != std::string::npos);

		// A name a later step binds is no better.
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "entity.rename", "parameters": { "entity": { "$ref": "crate" }, "name": "B" } },
			{ "command": "entity.create", "parameters": { "name": "A" }, "bind": { "crate": "/id" } }
		])"), &error));
		CHECK(error.find("unknown binding 'crate'") != std::string::npos);

		// Malformed references and bindings.
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "entity.create", "bind": { "a": "/id" } },
			{ "command": "entity.get", "parameters": { "entity": { "$ref": "a", "extra": 1 } } }
		])"), &error));
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "entity.create", "bind": { "a": "/id" } },
			{ "command": "entity.get", "parameters": { "entity": { "$ref": "a", "pointer": "id" } } }
		])"), &error));
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ { "command": "entity.create", "bind": { "a": "id" } } ])"), &error));
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ { "command": "entity.create", "bind": { "a b": "/id" } } ])"), &error));
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ { "command": "entity.create", "bind": [ "/id" ] } ])"), &error));
	}

	TEST_CASE("A step whose references have no value fails without running, and the script goes on")
	{
		ReplayEditor editor;
		std::string error;
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "entity.create", "parameters": { "name": 3 }, "bind": { "broken": "/id" } },
			{ "command": "entity.create", "parameters": { "name": "Fine" }, "bind": { "missing": "/nothing" } },
			{ "command": "entity.create", "parameters": { "name": "Child", "parent": { "$ref": "broken" } } },
			{ "command": "entity.create", "parameters": { "name": "Other", "parent": { "$ref": "missing" } } },
			{ "command": "entity.create", "parameters": { "name": "Last" } }
		])"), &error);
		REQUIRE_MESSAGE(script, error);
		CHECK(editor.RunScript(*script));
		const Scene& scene = *editor.Context.GetEditScene();
		CHECK(scene.FindEntityByName("Fine"));
		CHECK_FALSE(scene.FindEntityByName("Child"));
		CHECK_FALSE(scene.FindEntityByName("Other"));
		CHECK(scene.FindEntityByName("Last"));
		CHECK_FALSE(script->GetBindings().contains("broken"));
		CHECK_FALSE(script->GetBindings().contains("missing"));
	}
}

TEST_SUITE("Editor.Replay")
{
	TEST_CASE("An agent's session replays on a fresh copy of the project into the same scene and assets")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("Replay");
		const std::filesystem::path original = root / "Original";
		const std::filesystem::path copy = root / "Copy";
		const std::filesystem::path session = root / "Session.json";

		nlohmann::json built;
		{
			ReplayEditor editor;
			CreateProjectAndCopy(editor, original, copy);
			const uint64_t since = editor.Context.GetJournal().GetLatestId();
			AgentClient agent(editor);
			BuildScene(agent);
			const nlohmann::json exported = agent.Call("editor.activity.export", { { "path", FileSystem::ToUTF8(session) }, { "since", since } });
			CHECK(exported["edits"].get<size_t>() >= 45);
			CHECK(exported["lookups"] == 1); // The template's sun, found by name
			CHECK(exported["bindings"].get<size_t>() > 10);
			CHECK(exported["skippedWhilePlaying"] == 0);
			built = editor.SceneDocument();
			for (const CommandJournalEntry& entry : editor.Context.GetJournal().GetEntries())
			{
				if (entry.Id > since && entry.Parent == 0)
				{
					CAPTURE(entry.Command);
					CHECK(entry.Source == CommandSource::Automation);
					CHECK(entry.Client == "ReplayAgent");
				}
			}
		}

		const nlohmann::json steps = ReadJsonFile(session);
		CHECK(steps.size() >= 46);
		CHECK(steps[0]["command"] == "material.create");

		ReplayEditor replay;
		Replay(replay, copy, session);
		// The same scene, referring to the same assets by path (Floor and Pillar to Green, the crates to Metal...), and the
		// assets the session made exist again with the same content, their own IDs aside.
		ProjectComparison comparison { original, copy };
		comparison.Check(built, replay.SceneDocument());
		CHECK_FALSE(replay.Context.IsSceneModified()); // The session saved at its end, and so did the replay
		for (const char* document : { "Scenes/Main.stscene", "Materials/Red.stmat", "Materials/Metal.stmat", "Materials/Green.stmat", "Prefabs/Crate.stprefab" })
			comparison.CheckFile(document);
	}

	TEST_CASE("Comparisons name assets by path, so a reference to another asset of the same kind differs")
	{
		// Two projects whose scenes use their two materials alike (two references to one of them), but not the same one: a
		// comparison that numbered IDs alone would find them equal.
		const std::string red = UUID().ToString();
		const std::string green = UUID().ToString();
		const std::string otherRed = UUID().ToString();
		const std::string otherGreen = UUID().ToString();
		const auto scene = [](const std::string& material)
		{
			return nlohmann::json { { "Entities", { { { "Material", material } }, { { "Material", material } } } } };
		};
		Tests::ProjectNormalizer first({ { Tests::ToUpperId(red), "asset:Materials/Red.stmat" }, { Tests::ToUpperId(green), "asset:Materials/Green.stmat" } });
		Tests::ProjectNormalizer second({ { Tests::ToUpperId(otherRed), "asset:Materials/Red.stmat" }, { Tests::ToUpperId(otherGreen), "asset:Materials/Green.stmat" } });
		CHECK(first.Normalize(scene(green)) != second.Normalize(scene(otherRed)));
		CHECK(first.Normalize(scene(green)) == second.Normalize(scene(otherGreen)));

		// Other IDs are numbered across the documents of a project: an entity the second document refers to is the first's.
		const std::string entity = UUID().ToString();
		const std::string otherEntity = UUID().ToString();
		Tests::ProjectNormalizer a;
		Tests::ProjectNormalizer b;
		CHECK(a.Normalize(nlohmann::json { { "ID", entity } }) == b.Normalize(nlohmann::json { { "ID", otherEntity } }));
		CHECK(a.Normalize(nlohmann::json { { "Source", entity } }) == b.Normalize(nlohmann::json { { "Source", otherEntity } }));
		CHECK(a.Normalize(nlohmann::json { { "Source", entity } }) != b.Normalize(nlohmann::json { { "Source", UUID().ToString() } }));

		// The names come from the projects' .meta files.
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("ReplayAssetNames") / "Assets";
		REQUIRE(FileSystem::CreateDirectories(assets / "Materials"));
		REQUIRE(FileSystem::WriteText(assets / "Materials/Red.stmat.meta", nlohmann::json { { "Handle", red } }.dump()));
		const std::optional<std::unordered_map<std::string, std::string>> names = Tests::ReadAssetNames(assets);
		REQUIRE(names.has_value());
		CHECK(names->at(Tests::ToUpperId(red)) == "asset:Materials/Red.stmat");
		REQUIRE(FileSystem::WriteText(assets / "Materials/Broken.stmat.meta", "{"));
		CHECK_FALSE(Tests::ReadAssetNames(assets).has_value());
	}

	TEST_CASE("IDs taken from the selection or editor.status are the project's, and the replay edits the same entity")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("ReplaySelection");
		const std::filesystem::path original = root / "Original";
		const std::filesystem::path copy = root / "Copy";
		const std::filesystem::path session = root / "Session.json";
		nlohmann::json built;
		{
			ReplayEditor editor;
			CreateProjectAndCopy(editor, original, copy);
			// The person selected the ground; the agent asks what is selected and edits that.
			const std::string ground = editor.Context.GetEditScene()->FindEntityByName("Ground").GetUUID().ToString();
			editor.Run("selection.set", { { "entities", { ground } } });
			const uint64_t since = editor.Context.GetJournal().GetLatestId();
			AgentClient agent(editor);
			const std::string primary = agent.Call("editor.status")["selection"]["primary"].get<std::string>();
			REQUIRE(primary == ground);
			agent.Call("component.set", { { "entity", primary }, { "component", "Transform" }, { "values", { { "Translation", { 0, 3, 0 } } } } });
			const std::string selected = agent.Call("selection.get")["entities"][0].get<std::string>();
			agent.Call("entity.rename", { { "entity", selected }, { "name", "Raised Ground" } });
			agent.Call("scene.save");
			const nlohmann::json exported = agent.Call("editor.activity.export", { { "path", FileSystem::ToUTF8(session) }, { "since", since } });
			CHECK(exported["edits"] == 3);
			CHECK(exported["lookups"] == 0);
			CHECK(exported["bindings"] == 0);
			built = editor.SceneDocument();
		}
		const nlohmann::json steps = ReadJsonFile(session);
		REQUIRE(steps.size() == 3);
		CHECK(steps[0]["parameters"]["entity"].is_string()); // The ground's own ID, which the copy has too

		// The copy has no selection: the script must not depend on one.
		ReplayEditor replay;
		Replay(replay, copy, session);
		const Entity raised = replay.Context.GetEditScene()->FindEntityByName("Raised Ground");
		REQUIRE(raised);
		CHECK(raised.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 3.0f, 0.0f));
		ProjectComparison comparison { original, copy };
		comparison.Check(built, replay.SceneDocument());
	}

	TEST_CASE("Changes made while playing that outlast play mode are replayed, edits of the running copy are not")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("ReplayWhilePlaying");
		const std::filesystem::path original = root / "Original";
		const std::filesystem::path copy = root / "Copy";
		const std::filesystem::path session = root / "Session.json";
		nlohmann::json built;
		{
			ReplayEditor editor;
			CreateProjectAndCopy(editor, original, copy);
			const uint64_t since = editor.Context.GetJournal().GetLatestId();
			editor.Run("entity.create", { { "name", "Edited" } });
			editor.Run("play.start");
			// Saved materials and the saved scene outlast play mode; the entity made in the running copy does not.
			CHECK_FALSE(editor.Run("material.set", { { "material", "Materials/Ground.stmat" }, { "properties", { { "Roughness", 0.123 } } }, { "save", true } })
				.contains("warning"));
			editor.Run("material.create", { { "path", "Materials/MadeWhilePlaying.stmat" }, { "properties", { { "Metallic", 1 } } } });
			CHECK(editor.Run("entity.create", { { "name", "Played" } }).contains("warning"));
			editor.Run("scene.save");
			editor.Run("play.stop");
			CHECK_FALSE(editor.Context.IsSceneModified());
			const nlohmann::json exported = editor.Run("editor.activity.export", { { "path", FileSystem::ToUTF8(session) }, { "since", since } });
			CHECK(exported["skippedWhilePlaying"] == 1);
			CHECK(exported["edits"] == 4);
			built = editor.SceneDocument();
		}
		const nlohmann::json steps = ReadJsonFile(session);
		REQUIRE(steps.size() == 4);
		CHECK(steps[1]["command"] == "material.set");
		CHECK(steps[2]["command"] == "material.create");
		CHECK(steps[3]["command"] == "scene.save");

		ReplayEditor replay;
		Replay(replay, copy, session);
		CHECK_FALSE(replay.Context.IsSceneModified());
		CHECK_FALSE(replay.Context.GetEditScene()->FindEntityByName("Played"));
		ProjectComparison comparison { original, copy };
		comparison.Check(built, replay.SceneDocument());
		for (const char* document : { "Scenes/Main.stscene", "Materials/Ground.stmat", "Materials/MadeWhilePlaying.stmat" })
			comparison.CheckFile(document);
	}

	TEST_CASE("An export after a recovery says that the script cannot repeat what the recovery applied")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("ReplayRecovered") / "Game";
		ReplayEditor editor;
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(project) }, { "name", "Game" }, { "template", "basic3d" } });
		editor.Run("entity.create", { { "name", "Recovered" } });
		editor.Run("project.close", { { "force", true } }); // Kept as recovery data
		editor.Run("project.open", { { "path", FileSystem::ToUTF8(project) } });

		const uint64_t since = editor.Context.GetJournal().GetLatestId();
		editor.Run("scene.recover", { { "scene", "Scenes/Main.stscene" }, { "action", "apply" } });
		const std::string recovered = editor.Context.GetEditScene()->FindEntityByName("Recovered").GetUUID().ToString();
		editor.Run("entity.rename", { { "entity", recovered }, { "name", "Renamed" } });
		const nlohmann::json exported = editor.Run("editor.activity.export", { { "path", "Session.json" }, { "since", since } });
		CHECK(exported["edits"] == 1);
		REQUIRE(exported.contains("unrepeatableEdits"));
		CHECK(exported["unrepeatableEdits"] == 1);
		REQUIRE(exported.contains("warning"));
		CHECK(exported["warning"].get<std::string>().find("scene.recover") != std::string::npos);

		// Without the recovery in it, the export says nothing of the kind.
		const nlohmann::json later = editor.Run("editor.activity.export", { { "path", "Later.json" }, { "since", editor.Context.GetJournal().GetLatestId() } });
		CHECK(later["unrepeatableEdits"] == 0);
		CHECK_FALSE(later.contains("warning"));
	}

	TEST_CASE("IDs a result holds at different pointers get different bindings")
	{
		ReplayEditor editor;
		const std::string first = UUID().ToString();
		const std::string second = UUID().ToString();
		// Pointers whose characters a binding's name cannot hold, and that would read alike without them.
		editor.Commands.Register({ "test.ids", "Test query returning IDs.", CommandUtils::ObjectSchema({}),
			[first, second](EditorContext&, const nlohmann::json&)
			{
				return EditorCommandResult::Ok({ { "a_b", first }, { "a", { { "b", second } } }, { "mesh", "0000000000000001" } });
			}, EditorCommandKind::Query });
		editor.Commands.Register({ "test.use", "Test edit taking IDs.", CommandUtils::ObjectSchema({ { "ids", { { "type", "array" } } } }),
			[](EditorContext&, const nlohmann::json&) { return EditorCommandResult::Ok(); } });
		editor.Run("test.ids");
		editor.Run("test.use", { { "ids", { first, second, "0000000000000001" } } });

		std::string error;
		const std::optional<ReplayScript> script = editor.Context.GetJournal().BuildReplayScript(0, &error);
		REQUIRE_MESSAGE(script.has_value(), error);
		REQUIRE(script->Steps.size() == 2);
		CHECK(script->Lookups == 1);
		CHECK(script->Edits == 1);
		const nlohmann::json& bind = script->Steps[0]["bind"];
		REQUIRE(bind.size() == 2);
		const nlohmann::json& ids = script->Steps[1]["parameters"]["ids"];
		REQUIRE(ids.size() == 3);
		CHECK(bind.at(ids[0]["$ref"].get<std::string>()) == "/a_b");
		CHECK(bind.at(ids[1]["$ref"].get<std::string>()) == "/a/b");
		CHECK(ids[2] == "0000000000000001"); // Built-in asset handles are the same in every project

		// The script runs as written.
		Scope<EditorCommandScript> replay = EditorCommandScript::FromJson(script->Steps, &error);
		REQUIRE_MESSAGE(replay, error);
		CHECK_FALSE(editor.RunScript(*replay));
	}

	TEST_CASE("Undo and redo from the UI run their commands, so the journal and replays have them")
	{
		Tests::HarnessEditor editor({}, Tests::WithFeatureProject("ReplayUndoUI"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("entity.create", { { "name", "Undone" } });
		const auto pressCtrl = [&editor](ImGuiKey key)
		{
			editor.Harness.SetKey(ImGuiMod_Ctrl, true);
			editor.Frames(1);
			editor.Harness.SetKey(key, true);
			editor.Frames(1);
			editor.Harness.SetKey(key, false);
			editor.Harness.SetKey(ImGuiMod_Ctrl, false);
			editor.Frames(1);
		};
		const CommandJournal& journal = editor.Context().GetJournal();

		pressCtrl(ImGuiKey_Z);
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Undone"));
		REQUIRE(journal.GetEntries().back().Command == "edit.undo");
		CHECK(journal.GetEntries().back().Source == CommandSource::UI);
		pressCtrl(ImGuiKey_Y);
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Undone"));
		REQUIRE(journal.GetEntries().back().Command == "edit.redo");
		CHECK(journal.GetEntries().back().Source == CommandSource::UI);

		// With nothing to redo the shortcut does nothing: no failed command, no error.
		const uint64_t latest = journal.GetLatestId();
		pressCtrl(ImGuiKey_Y);
		CHECK(journal.GetLatestId() == latest);
	}

	TEST_CASE("Exports leave out what a replay must not repeat and refuse what it cannot")
	{
		ReplayEditor editor;
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(Tests::CreateTemporaryDirectory("ReplayRefusals") / "Game") }, { "name", "Game" } });
		// Edits of a running copy, and play mode itself, are not repeated; nor are queries no edit took an ID from.
		editor.Run("play.start");
		editor.Run("entity.create", { { "name", "Played" } });
		editor.Run("play.stop");
		const std::string edited = editor.Run("entity.create", { { "name", "Edited" } })["id"].get<std::string>();
		editor.Run("entity.get", { { "entity", edited } });
		editor.Run("selection.set", { { "entities", { edited } } });
		const nlohmann::json exported = editor.Run("editor.activity.export", { { "path", "Session.json" } });
		CHECK(exported["skippedWhilePlaying"] == 1);
		CHECK(exported["edits"] == 1);
		CHECK(exported["lookups"] == 0);
		CHECK(exported["editsOutsideCommands"] == 0);
		CHECK_FALSE(exported.contains("warning"));
		const nlohmann::json steps = ReadJsonFile(editor.Context.GetProject()->GetProjectDirectory() / "Session.json");
		REQUIRE(steps.size() == 1);
		CHECK(steps[0]["command"] == "entity.create");
		CHECK(steps[0]["parameters"]["name"] == "Edited");

		// An existing file is replaced only with overwrite.
		CHECK(editor.Commands.Execute(editor.Context, "editor.activity.export", { { "path", "Session.json" } }).ErrorKind == EditorCommandError::InvalidParameters);
		editor.Run("editor.activity.export", { { "path", "Session.json" }, { "overwrite", true } });

		// An inspector edit records its undo step directly: the script cannot hold it, and the export says so. A drag is one
		// edit however many values it went through (they merge into one step). Undo through its command is an edit like any
		// other.
		const uint64_t beforeInspector = editor.Context.GetJournal().GetLatestId();
		const Entity entity = editor.Context.GetEditScene()->GetEntityByUUID(*UUID::FromString(edited));
		const ComponentInfo& transform = *ComponentRegistry::Find("Transform");
		const size_t stepsBeforeDrag = editor.Context.GetUndoStack().GetCount();
		for (int frame = 1; frame <= 5; frame++)
		{
			std::string error;
			REQUIRE_MESSAGE(SetPropertyWithUndo(editor.Context, entity, transform, *transform.FindProperty("Translation"),
				PropertyValue(glm::vec3(1.0f, 2.0f, static_cast<float>(frame))), &error), error);
		}
		REQUIRE(editor.Context.GetUndoStack().GetCount() == stepsBeforeDrag + 1);
		const nlohmann::json inspected = editor.Run("editor.activity.export", { { "path", "Inspected.json" }, { "since", beforeInspector } });
		CHECK(inspected["editsOutsideCommands"] == 1);
		CHECK(inspected["edits"] == 0);
		CHECK(inspected["warning"].get<std::string>().find("outside commands") != std::string::npos);
		editor.Run("edit.undo");
		const nlohmann::json undone = editor.Run("editor.activity.export", { { "path", "Undone.json" }, { "since", beforeInspector } });
		CHECK(undone["editsOutsideCommands"] == 1);
		CHECK(undone["edits"] == 1);

		// An edit whose parameters were too large to keep cannot be repeated: exports that include it fail.
		editor.Run("entity.create", { { "name", std::string(CommandJournal::c_MaxParameterBytes + 1024, 'n') } });
		const EditorCommandResult large = editor.Commands.Execute(editor.Context, "editor.activity.export", { { "path", "Large.json" } });
		CHECK(large.ErrorKind == EditorCommandError::Failed);
		CHECK(large.Error.find("too large") != std::string::npos);
		const uint64_t afterLarge = editor.Context.GetJournal().GetLatestId();
		editor.Run("entity.create", { { "name", "After" } });
		CHECK(editor.Run("editor.activity.export", { { "path", "Later.json" }, { "since", afterLarge } })["edits"] == 1);

		// Nor can entries the journal no longer holds.
		for (size_t index = 0; index < CommandJournal::c_MaxEntries; index++)
			editor.Commands.Execute(editor.Context, "scene.info");
		const EditorCommandResult gone = editor.Commands.Execute(editor.Context, "editor.activity.export", { { "path", "Gone.json" } });
		CHECK(gone.ErrorKind == EditorCommandError::Failed);
		CHECK(gone.Error.find("journal keeps at most") != std::string::npos);
	}
}
