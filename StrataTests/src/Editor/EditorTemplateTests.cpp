#include <doctest/doctest.h>

#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectTemplates.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Math/Math.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <set>

using namespace Strata;

namespace
{

	struct TemplateHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters)
		{
			return Commands.Execute(Context, name, parameters);
		}
	};

	std::set<std::string> GetEntityNames(const Scene& scene)
	{
		std::set<std::string> names;
		for (UUID id : scene.GetRootEntities())
		{
			if (Entity entity = scene.GetEntityByUUID(id))
				names.insert(entity.GetName());
		}
		return names;
	}

	// The basic3d template's scene: exactly the five entities with their components and values.
	void CheckBasic3DScene(Scene& scene)
	{
		CHECK(scene.GetEntityCount() == 5);
		CHECK(GetEntityNames(scene) == std::set<std::string> { "Main Camera", "Sun", "Sky", "Ground", "Post Process" });

		Entity camera = scene.FindEntityByName("Main Camera");
		REQUIRE(camera);
		REQUIRE(camera.HasComponent<CameraComponent>());
		CHECK(camera.GetComponent<CameraComponent>().Primary);
		CHECK(camera.HasComponent<AudioListenerComponent>());
		const TransformComponent& cameraTransform = camera.GetComponent<TransformComponent>();
		CHECK(Math::IsNearlyEqual(cameraTransform.Translation, glm::vec3(0.0f, 2.0f, 6.0f), 1e-5f));
		// It looks at the origin.
		const glm::vec3 forward = cameraTransform.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
		CHECK(Math::IsNearlyEqual(forward, glm::normalize(-cameraTransform.Translation), 1e-4f));
		CHECK(scene.GetPrimaryCameraEntity() == camera);

		Entity sun = scene.FindEntityByName("Sun");
		REQUIRE(sun);
		REQUIRE(sun.HasComponent<DirectionalLightComponent>());
		CHECK(sun.GetComponent<DirectionalLightComponent>().Intensity == doctest::Approx(3.0f));
		CHECK(sun.GetComponent<DirectionalLightComponent>().CastShadows);
		CHECK(Math::IsNearlyEqual(Math::QuatToEulerDegrees(sun.GetComponent<TransformComponent>().Rotation), glm::vec3(-45.0f, 30.0f, 0.0f), 1e-3f));

		Entity sky = scene.FindEntityByName("Sky");
		REQUIRE(sky);
		REQUIRE(sky.HasComponent<SkyLightComponent>());
		CHECK(sky.GetComponent<SkyLightComponent>().Source == SkyLightSource::Procedural);

		Entity ground = scene.FindEntityByName("Ground");
		REQUIRE(ground);
		REQUIRE(ground.HasComponent<MeshRendererComponent>());
		CHECK(ground.GetComponent<MeshRendererComponent>().Mesh == BuiltinAssets::PlaneMesh);
		CHECK(ground.GetComponent<MeshRendererComponent>().Material == BuiltinAssets::DefaultMaterial);
		CHECK(Math::IsNearlyEqual(ground.GetComponent<TransformComponent>().Scale, glm::vec3(20.0f, 1.0f, 20.0f), 1e-6f));

		Entity postProcess = scene.FindEntityByName("Post Process");
		REQUIRE(postProcess);
		REQUIRE(postProcess.HasComponent<PostProcessComponent>());
		CHECK(postProcess.GetComponent<PostProcessComponent>().Tonemapper == TonemapOperator::ACES);
		CHECK(postProcess.GetComponent<PostProcessComponent>().AutoExposure);
		CHECK(postProcess.GetComponent<PostProcessComponent>().Exposure == doctest::Approx(1.0f));
	}

}

TEST_SUITE("Editor.Templates")
{
	TEST_CASE("project.templates lists the templates with a schema")
	{
		TemplateHarness harness;
		const EditorCommand* info = harness.Commands.Find("project.templates");
		REQUIRE(info);
		CHECK(info->Parameters["type"] == "object");
		CHECK(info->Parameters["additionalProperties"] == false);
		CHECK_FALSE(info->Description.empty());

		const nlohmann::json result = harness.Run("project.templates");
		CHECK(result["default"] == "empty");
		REQUIRE(result["templates"].is_array());
		REQUIRE(result["templates"].size() == ProjectTemplates::GetAll().size());
		std::set<std::string> ids;
		for (const nlohmann::json& entry : result["templates"])
		{
			ids.insert(entry["id"].get<std::string>());
			CHECK_FALSE(entry["name"].get<std::string>().empty());
			CHECK_FALSE(entry["description"].get<std::string>().empty());
		}
		CHECK(ids == std::set<std::string> { "empty", "basic3d" });
		CHECK(result["templates"][1]["startScene"] == "Scenes/Main.stscene");
		CHECK(result["templates"][0]["startScene"].is_null());

		// The commands that take a template offer exactly these.
		for (const char* command : { "project.create", "scene.new" })
		{
			CAPTURE(command);
			const EditorCommand* withTemplate = harness.Commands.Find(command);
			REQUIRE(withTemplate);
			const nlohmann::json& options = withTemplate->Parameters["properties"]["template"]["enum"];
			CHECK(std::set<std::string>(options.begin(), options.end()) == ids);
		}
	}

	TEST_CASE("project.create with basic3d saves a lit start scene with exactly five entities")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("Basic3D") / "Lit";
		std::string startScene;
		{
			TemplateHarness harness;
			const nlohmann::json result = harness.Run("project.create",
				{ { "directory", FileSystem::ToUTF8(directory) }, { "name", "Lit" }, { "template", "basic3d" } });
			CHECK(result["template"] == "basic3d");
			REQUIRE(result["startScene"].is_string());
			startScene = result["startScene"].get<std::string>();

			// Saved, set as the project's start scene and open.
			CHECK(FileSystem::IsRegularFile(directory / "Assets" / "Scenes" / "Main.stscene"));
			CHECK(FileSystem::IsRegularFile(directory / "Assets" / "Scenes" / "Main.stscene.meta"));
			CHECK(harness.Context.GetProject()->GetConfig().StartScene == *UUIDFromJson(startScene));
			CHECK(harness.Context.GetSceneHandle() == *UUIDFromJson(startScene));
			CHECK_FALSE(harness.Context.IsSceneModified());
			CHECK(harness.Context.GetEditScene()->GetName() == "Main");
			CheckBasic3DScene(*harness.Context.GetEditScene());
			// Nothing to undo: the project starts like this.
			CHECK(harness.Context.GetUndoStack().GetHistory().empty());

			// The editor camera starts with the game's view.
			const EditorCamera& camera = harness.Context.GetViewport().GetCamera();
			CHECK(Math::IsNearlyEqual(camera.GetPosition(), glm::vec3(0.0f, 2.0f, 6.0f), 1e-4f));
			CHECK(Math::IsNearlyEqual(camera.GetTarget(), glm::vec3(0.0f), 1e-4f));

			// Playing looks through the scene's camera: no missing-camera message.
			harness.Run("play.start");
			const std::optional<ViewportView> view = ResolveViewportView(harness.Context, ViewportCameraSource::Automatic, 16.0f / 9.0f);
			REQUIRE(view);
			CHECK(view->FromScene);
			CHECK(view->Notice.empty());
			harness.Run("play.stop");
		}

		// Reopened, the start scene is the same five entities and keeps its camera.
		TemplateHarness harness;
		harness.Run("project.open", { { "path", FileSystem::ToUTF8(directory) } });
		CHECK(harness.Context.GetSceneHandle() == *UUIDFromJson(startScene));
		CheckBasic3DScene(*harness.Context.GetEditScene());
		CHECK(Math::IsNearlyEqual(harness.Context.GetViewport().GetCamera().GetPosition(), glm::vec3(0.0f, 2.0f, 6.0f), 1e-4f));
	}

	TEST_CASE("project.create and scene.new without a template start empty, as before")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EmptyTemplate") / "Empty";
		TemplateHarness harness;
		const nlohmann::json result = harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Empty" } });
		CHECK(result["template"] == "empty");
		CHECK(result["startScene"].is_null());
		CHECK_FALSE(harness.Context.GetProject()->GetConfig().StartScene.IsValid());
		CHECK_FALSE(harness.Context.GetSceneHandle().IsValid());
		CHECK(harness.Context.GetEditScene()->GetEntityCount() == 0);
		CHECK_FALSE(FileSystem::Exists(directory / "Assets" / "Scenes"));
		const nlohmann::json camera = harness.Run("camera.get");
		CHECK(camera["yaw"] == 45.0); // The default view

		harness.Run("entity.create", { { "name", "Old" } });
		harness.Run("scene.new", { { "name", "Fresh" } });
		CHECK(harness.Context.GetEditScene()->GetName() == "Fresh");
		CHECK(harness.Context.GetEditScene()->GetEntityCount() == 0);
		CHECK(harness.Run("camera.get") == camera); // An empty scene keeps the view

		// scene.new with a template fills the new, unsaved scene.
		harness.Run("scene.new", { { "name", "Level" }, { "template", "basic3d" } });
		CHECK(harness.Context.GetEditScene()->GetName() == "Level");
		CHECK_FALSE(harness.Context.GetSceneHandle().IsValid());
		CheckBasic3DScene(*harness.Context.GetEditScene());
		CHECK(Math::IsNearlyEqual(harness.Context.GetViewport().GetCamera().GetPosition(), glm::vec3(0.0f, 2.0f, 6.0f), 1e-4f));
		// It also works without a project: the template uses only built-in assets.
		harness.Context.CloseProject();
		harness.Run("scene.new", { { "template", "basic3d" } });
		CheckBasic3DScene(*harness.Context.GetEditScene());
	}

	TEST_CASE("Unknown templates are rejected before anything is created")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("UnknownTemplate") / "Nothing";
		TemplateHarness harness;
		EditorCommandResult result = harness.Execute("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Nothing" }, { "template", "racing" } });
		CHECK_FALSE(result.Success);
		CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(result.Error.find("racing") != std::string::npos);
		CHECK(result.Error.find("basic3d") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(directory));
		CHECK_FALSE(harness.Context.HasProject());

		harness.Run("entity.create", { { "name", "Kept" } });
		result = harness.Execute("scene.new", { { "template", "racing" } });
		CHECK_FALSE(result.Success);
		CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Kept"));
		result = harness.Execute("scene.new", { { "template", 3 } });
		CHECK_FALSE(result.Success);

		// The context refuses them too.
		std::string error;
		CHECK_FALSE(harness.Context.CreateProject(directory, "Nothing", "racing", &error));
		CHECK_FALSE(error.empty());
		CHECK_FALSE(FileSystem::Exists(directory));
		CHECK_FALSE(harness.Context.NewScene("Other", "racing"));
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Kept"));
	}
}
