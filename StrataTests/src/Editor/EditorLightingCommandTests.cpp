#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/UUID.h>
#include <Strata/Math/Math.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Renderer/SkyPresets.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <filesystem>
#include <optional>
#include <string>

using namespace Strata;

namespace
{

	struct LightingHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Execute(name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		Scene& GetScene() { return *Context.GetActiveScene(); }

		Entity Find(const nlohmann::json& id)
		{
			return GetScene().GetEntityByUUID(UUID(std::stoull(id.get<std::string>(), nullptr, 16)));
		}

		std::string Id(Entity entity)
		{
			return fmt::format("{:016X}", static_cast<uint64_t>(entity.GetUUID()));
		}
	};

	// The direction toward the sun a directional light shines from (it shines along its world -Z axis).
	glm::vec3 TowardSun(Scene& scene, Entity light)
	{
		scene.UpdateWorldTransforms();
		return -glm::normalize(glm::vec3(scene.GetWorldTransform(light) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
	}

	float Azimuth(const glm::vec3& towardSun)
	{
		return glm::degrees(std::atan2(towardSun.z, towardSun.x));
	}

	// A project whose materials are files (they load in the background, so lighting.makeAreaLight can be made to wait for
	// one): Materials/Panel.stmat emits 5,000 nits (the Cornell box's panel, GIScenes.h), Materials/Matte.stmat nothing.
	struct AreaLightProject
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		AssetHandle Emissive = UUID::Null();
		AssetHandle Matte = UUID::Null();

		explicit AreaLightProject(const std::filesystem::path& directory)
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Panels" } });
			Emissive = ToHandle(Run("material.create", { { "path", "Materials/Panel.stmat" }, { "properties", { { "EmissiveColor", { 1.0f, 1.0f, 1.0f } },
				{ "EmissiveIntensity", 5000.0f } } } })["asset"]);
			Matte = ToHandle(Run("material.create", { { "path", "Materials/Matte.stmat" } })["asset"]);
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
		}

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
		}

		Scene& GetScene() { return *Context.GetEditScene(); }

		// The built-in cube scaled to a panel, as the Cornell box and the look-dev scenes make theirs.
		Entity AddPanel(const std::string& name, AssetHandle material, const glm::vec3& center, const glm::vec3& size, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
		{
			const nlohmann::json created = Run("entity.create", { { "name", name }, { "components", { { "Transform", { { "Translation", { center.x, center.y, center.z } },
				{ "Rotation", { rotation.x, rotation.y, rotation.z, rotation.w } }, { "Scale", { size.x, size.y, size.z } } } },
				{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) }, { "Material", UUIDToJson(material) } } } } } });
			return GetScene().GetEntityByUUID(ToHandle(created["id"]));
		}

		size_t GetHistorySize() { return Context.GetUndoStack().GetHistory().size(); }
		size_t GetEntityCount() { return GetScene().GetRegistry().view<IDComponent>().size(); }

		static UUID ToHandle(const nlohmann::json& json)
		{
			const std::optional<UUID> uuid = UUIDFromJson(json);
			REQUIRE(uuid.has_value());
			return *uuid;
		}
	};

}

TEST_SUITE("Editor.Lighting")
{
	TEST_CASE("lighting.applySkyPreset writes the preset in one undo step")
	{
		LightingHarness harness;
		Scene& scene = harness.GetScene();
		Entity sky = scene.CreateEntity("Sky");
		sky.AddComponent<SkyLightComponent>();
		Entity sun = scene.CreateEntity("Sun");
		// The sun 30 degrees up at an azimuth of 120 degrees: the presets keep the azimuth.
		sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(-GetSkyPresetSunDirection(30.0f, 120.0f));
		sun.AddComponent<DirectionalLightComponent>();
		scene.UpdateWorldTransforms();

		for (const SkyPreset preset : { SkyPreset::Clear, SkyPreset::Hazy, SkyPreset::Overcast, SkyPreset::GoldenHour })
		{
			const std::string name = SkyPresetToString(preset);
			CAPTURE(name);
			const SkyPresetValues& values = GetSkyPresetValues(preset);
			const SkyLightComponent before = sky.GetComponent<SkyLightComponent>();
			const DirectionalLightComponent lightBefore = sun.GetComponent<DirectionalLightComponent>();
			const glm::vec3 towardBefore = TowardSun(scene, sun);
			const nlohmann::json result = harness.Run("lighting.applySkyPreset", { { "preset", name } });
			CHECK(result["preset"] == name);
			CHECK(result["changed"] == true);
			CHECK(result["createdSky"] == false);
			CHECK(result["createdLight"] == false);
			CHECK(result["sky"] == harness.Id(sky));
			CHECK(result["light"] == harness.Id(sun));
			CHECK(result["sunIntensity"].get<float>() == values.SunIlluminance);
			const SkyLightComponent& written = sky.GetComponent<SkyLightComponent>();
			CHECK(written.Source == values.Source);
			CHECK(written.SunSize == values.SunSize);
			if (values.Source == SkyLightSource::Atmosphere)
			{
				CHECK(written.AtmosphereHaze == values.AtmosphereHaze);
				CHECK(written.AtmosphereDensity == values.AtmosphereDensity);
				// The air dims the sun on its way to the ground.
				CHECK(result["groundIlluminance"].get<float>() < values.SunIlluminance);
				CHECK(result["groundIlluminance"].get<float>() > 0.0f);
			}
			else
			{
				CHECK(written.ZenithColor == values.ZenithColor);
				CHECK(written.SunIntensity == values.SunDiskIntensity);
				CHECK(result["groundIlluminance"].get<float>() == values.SunIlluminance);
			}
			const DirectionalLightComponent& light = sun.GetComponent<DirectionalLightComponent>();
			CHECK(light.Intensity == values.SunIlluminance);
			CHECK(light.ShadowSoftness == values.ShadowSoftness);
			const glm::vec3 toward = TowardSun(scene, sun);
			CHECK(glm::degrees(std::asin(toward.y)) == doctest::Approx(values.SunElevation).epsilon(0.001));
			CHECK(Azimuth(toward) == doctest::Approx(120.0f).epsilon(0.001));

			// One step: undo restores every value, redo applies it again.
			harness.Run("edit.undo");
			CHECK(sky.GetComponent<SkyLightComponent>().Source == before.Source);
			CHECK(sky.GetComponent<SkyLightComponent>().AtmosphereHaze == before.AtmosphereHaze);
			CHECK(sun.GetComponent<DirectionalLightComponent>().Intensity == lightBefore.Intensity);
			CHECK(glm::length(TowardSun(scene, sun) - towardBefore) < 1e-5f);
			harness.Run("edit.redo");
			CHECK(sun.GetComponent<DirectionalLightComponent>().Intensity == values.SunIlluminance);

			// The same preset again changes nothing and records nothing.
			const int64_t position = harness.Run("editor.status", { { "sections", { "undo" } } })["undo"]["position"].get<int64_t>();
			CHECK(harness.Run("lighting.applySkyPreset", { { "preset", name } })["changed"] == false);
			CHECK(harness.Run("editor.status", { { "sections", { "undo" } } })["undo"]["position"].get<int64_t>() == position);
		}

		// The elevation and azimuth on request.
		harness.Run("lighting.applySkyPreset", { { "preset", "Clear" }, { "sunElevation", 25.0 }, { "sunAzimuth", -90.0 } });
		const glm::vec3 toward = TowardSun(scene, sun);
		CHECK(glm::degrees(std::asin(toward.y)) == doctest::Approx(25.0f).epsilon(0.001));
		CHECK(Azimuth(toward) == doctest::Approx(-90.0f).epsilon(0.001));
	}

	TEST_CASE("lighting.applySkyPreset applied again changes nothing at any azimuth")
	{
		// The azimuth is read back from the light's rotation and the direction made again from it: rounded differently, the
		// rotation drifted by an ulp at some azimuths (the default 20 degrees among them), so a second click recorded a step.
		for (const float azimuth : { 20.0f, -60.0f, -33.0f, 13.0f, 41.0f, 120.0f, 157.0f })
		{
			CAPTURE(azimuth);
			LightingHarness harness;
			Scene& scene = harness.GetScene();
			scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
			Entity sun = scene.CreateEntity("Sun");
			sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(-GetSkyPresetSunDirection(30.0f, azimuth));
			sun.AddComponent<DirectionalLightComponent>();
			scene.UpdateWorldTransforms();
			for (const SkyPreset preset : { SkyPreset::Clear, SkyPreset::Hazy, SkyPreset::Overcast, SkyPreset::GoldenHour })
			{
				const std::string name = SkyPresetToString(preset);
				CAPTURE(name);
				CHECK(harness.Run("lighting.applySkyPreset", { { "preset", name } })["changed"] == true);
				const glm::quat rotation = sun.GetComponent<TransformComponent>().Rotation;
				const int64_t position = harness.Run("editor.status", { { "sections", { "undo" } } })["undo"]["position"].get<int64_t>();
				for (int again = 0; again < 3; again++)
				{
					CHECK(harness.Run("lighting.applySkyPreset", { { "preset", name } })["changed"] == false);
					CHECK(harness.Run("editor.status", { { "sections", { "undo" } } })["undo"]["position"].get<int64_t>() == position);
					const bool kept = sun.GetComponent<TransformComponent>().Rotation == rotation;
					CHECK(kept);
				}
				CHECK(Azimuth(TowardSun(scene, sun)) == doctest::Approx(azimuth).epsilon(0.001));
			}
		}
	}

	TEST_CASE("lighting.applySkyPreset creates the sky and the sun a scene lacks, and raises a clipping Post Process")
	{
		LightingHarness harness;
		Scene& scene = harness.GetScene();
		Entity post = scene.CreateEntity("Post");
		PostProcessComponent& postProcess = post.AddComponent<PostProcessComponent>();
		postProcess.AutoExposure = true;
		postProcess.AutoExposureMaxEV = 12.0f;
		const nlohmann::json result = harness.Run("lighting.applySkyPreset", { { "preset", "Clear" } });
		CHECK(result["createdSky"] == true);
		CHECK(result["createdLight"] == true);
		CHECK(harness.Find(result["sky"]).GetName() == "Sky");
		CHECK(harness.Find(result["light"]).GetName() == "Sun");
		CHECK(harness.Find(result["sky"]).GetComponent<SkyLightComponent>().Source == SkyLightSource::Atmosphere);
		// A new light faces the default azimuth (20 degrees) at the preset's elevation.
		const glm::vec3 toward = TowardSun(scene, harness.Find(result["light"]));
		CHECK(Azimuth(toward) == doctest::Approx(20.0f).epsilon(0.001));
		CHECK(glm::degrees(std::asin(toward.y)) == doctest::Approx(40.0f).epsilon(0.001));
		CHECK(post.GetComponent<PostProcessComponent>().AutoExposureMaxEV == 16.0f);
		CHECK(result["postProcess"] == harness.Id(post));
		harness.Run("edit.undo");
		CHECK_FALSE(harness.Find(result["sky"]));
		CHECK_FALSE(harness.Find(result["light"]));
		CHECK(post.GetComponent<PostProcessComponent>().AutoExposureMaxEV == 12.0f);
	}

	TEST_CASE("lighting.applySkyPreset turns a manual exposure automatic in its undo step")
	{
		// A manual exposure of 0 suits a 3 lux sun: under a real one the whole frame clips to white.
		LightingHarness harness;
		Scene& scene = harness.GetScene();
		Entity post = scene.CreateEntity("Post");
		PostProcessComponent& postProcess = post.AddComponent<PostProcessComponent>();
		postProcess.AutoExposure = false;
		postProcess.Exposure = 1.5f;
		postProcess.AutoExposureMaxEV = 12.0f;
		const nlohmann::json result = harness.Run("lighting.applySkyPreset", { { "preset", "Hazy" } });
		CHECK(result["autoExposureTurnedOn"] == true);
		CHECK(result["postProcess"] == harness.Id(post));
		CHECK(post.GetComponent<PostProcessComponent>().AutoExposure);
		CHECK(post.GetComponent<PostProcessComponent>().Exposure == 0.0f);
		CHECK(post.GetComponent<PostProcessComponent>().AutoExposureMaxEV == 16.0f);
		harness.Run("edit.undo");
		CHECK_FALSE(post.GetComponent<PostProcessComponent>().AutoExposure);
		CHECK(post.GetComponent<PostProcessComponent>().Exposure == 1.5f);
		CHECK(post.GetComponent<PostProcessComponent>().AutoExposureMaxEV == 12.0f);
		harness.Run("edit.redo");

		// An automatic exposure that reaches a sunlit scene's level is left as it is.
		const nlohmann::json again = harness.Run("lighting.applySkyPreset", { { "preset", "Clear" } });
		CHECK(again["autoExposureTurnedOn"] == false);
		CHECK(again["postProcess"].is_null());
		CHECK(post.GetComponent<PostProcessComponent>().AutoExposure);
		CHECK(post.GetComponent<PostProcessComponent>().AutoExposureMaxEV == 16.0f);
	}

	TEST_CASE("lighting.applySkyPreset refuses invalid parameters and changes nothing")
	{
		LightingHarness harness;
		Scene& scene = harness.GetScene();
		Entity plain = scene.CreateEntity("Plain");
		const auto refused = [&](const nlohmann::json& parameters)
		{
			CAPTURE(parameters.dump());
			const EditorCommandResult result = harness.Execute("lighting.applySkyPreset", parameters);
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		};
		refused({ { "preset", "Sunny" } });
		refused({ { "preset", "Clear" }, { "sky", harness.Id(plain) } });
		refused({ { "preset", "Clear" }, { "light", harness.Id(plain) } });
		refused({ { "preset", "Clear" }, { "sunElevation", 95.0 } });
		refused({ { "preset", "Clear" }, { "sunAzimuth", 400.0 } });
		refused(nlohmann::json::object());
		CHECK(scene.GetRegistry().view<SkyLightComponent>().size() == 0);
		CHECK(scene.GetRegistry().view<DirectionalLightComponent>().size() == 0);
	}

	TEST_CASE("lighting.makeAreaLight makes the Cornell panel a light under its face in one undo step, waiting for its material")
	{
		// L3: the panel's bottom face at y = 5.2, 1.3 x 1.05 m: the light 1 mm under it, facing down, with the panel's luminance.
		AreaLightProject project(Tests::CreateTemporaryDirectory("MakeAreaLight") / "Panels");
		Scene& scene = project.GetScene();
		const Entity panel = project.AddPanel("Cornell Panel", project.Emissive, glm::vec3(0.0f, 5.35f, 0.0f), glm::vec3(1.3f, 0.3f, 1.05f));
		const size_t history = project.GetHistorySize();
		const size_t entities = project.GetEntityCount();
		// The material is not loaded: the command waits for it (its first answer is pending), changing nothing meanwhile.
		project.Context.GetAssetManager()->UnloadAsset(project.Emissive);
		EditorCommandResult pending = project.Commands.Execute(project.Context, "lighting.makeAreaLight", { { "entity", UUIDToJson(panel.GetUUID()) } });
		REQUIRE(pending.Success);
		CHECK(pending.IsPending());
		CHECK(project.GetEntityCount() == entities);
		const EditorCommandResult result = Tests::CompleteCommand(project.Context, std::move(pending));
		INFO(result.Error);
		REQUIRE(result.Success);
		CHECK(result.Value["side"] == "-Y");
		CHECK(result.Value["size"][0].get<float>() == doctest::Approx(1.3f).epsilon(1e-5));
		CHECK(result.Value["size"][1].get<float>() == doctest::Approx(1.05f).epsilon(1e-5));
		CHECK(result.Value["luminance"].get<float>() == doctest::Approx(5000.0f));
		CHECK(result.Value["lumens"].get<float>() == doctest::Approx(glm::pi<float>() * 5000.0f * 1.3f * 1.05f).epsilon(1e-5));
		CHECK(result.Value["tagged"] == true);
		CHECK(project.GetHistorySize() == history + 1);

		const Entity light = scene.GetEntityByUUID(AreaLightProject::ToHandle(result.Value["light"]));
		REQUIRE(light);
		CHECK(light.GetParent() == panel);
		CHECK(light.GetName() == "Cornell Panel Light");
		CHECK(panel.HasComponent<LightSurfaceComponent>());
		scene.UpdateWorldTransforms();
		const glm::mat4 world = scene.GetWorldTransform(light);
		const glm::vec3 position(world[3]);
		CHECK(position.x == doctest::Approx(0.0f).epsilon(1e-5));
		CHECK(position.y == doctest::Approx(5.199f).epsilon(1e-6));
		CHECK(position.z == doctest::Approx(0.0f).epsilon(1e-5));
		const glm::vec3 facing = -glm::normalize(glm::vec3(world[2]));
		CHECK(facing.y == doctest::Approx(-1.0f).epsilon(1e-6));
		const RectLightComponent& component = light.GetComponent<RectLightComponent>();
		// Its face in the world: X along the panel's longer side (1.3 m), Y the other, from the panel's scale.
		CHECK(component.Width * glm::length(glm::vec3(world[0])) == doctest::Approx(1.3f).epsilon(1e-5));
		CHECK(component.Height * glm::length(glm::vec3(world[1])) == doctest::Approx(1.05f).epsilon(1e-5));
		CHECK(std::abs(glm::normalize(glm::vec3(world[0])).x) == doctest::Approx(1.0f).epsilon(1e-6));
		CHECK(component.Intensity == doctest::Approx(5000.0f));
		CHECK(component.Color == glm::vec3(1.0f));
		CHECK(component.Units == AreaLightUnits::Nits);
		CHECK(component.CastShadows);
		CHECK_FALSE(component.TwoSided);

		// A second call finds the light it made and changes nothing.
		const EditorCommandResult again = project.Execute("lighting.makeAreaLight", { { "entity", UUIDToJson(panel.GetUUID()) } });
		CHECK_FALSE(again.Success);
		CHECK(again.Error.find("already a light: Cornell Panel Light") != std::string::npos);
		CHECK(project.GetEntityCount() == entities + 1);
		CHECK(project.GetHistorySize() == history + 1);

		// One undo step takes the light and the tag away.
		const UUID lightId = light.GetUUID();
		project.Run("edit.undo");
		CHECK_FALSE(scene.GetEntityByUUID(lightId));
		CHECK_FALSE(panel.HasComponent<LightSurfaceComponent>());
		CHECK(panel.GetChildren().empty());
		CHECK(project.GetEntityCount() == entities);
	}

	TEST_CASE("lighting.makeAreaLight refuses what it cannot make a light and changes nothing")
	{
		AreaLightProject project(Tests::CreateTemporaryDirectory("MakeAreaLightRefused") / "Panels");
		Scene& scene = project.GetScene();
		const Entity plain = scene.CreateEntity("Plain");
		const Entity matte = project.AddPanel("Matte Panel", project.Matte, glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(1.0f, 0.05f, 1.0f));
		// A wall panel: the thin axis horizontal, so auto has no face pointing down to choose.
		const Entity wall = project.AddPanel("Wall Panel", project.Emissive, glm::vec3(2.0f, 1.5f, 0.0f), glm::vec3(1.0f, 0.05f, 1.0f),
			glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
		// A cube is no panel.
		const Entity cube = project.AddPanel("Cube", project.Emissive, glm::vec3(-2.0f, 1.0f, 0.0f), glm::vec3(1.0f));
		// A panel whose face is under a Rect Light's smallest size in the world (0.5 mm across).
		const Entity tiny = project.AddPanel("Tiny Panel", project.Emissive, glm::vec3(-4.0f, 3.0f, 0.0f), glm::vec3(0.0005f, 0.0001f, 0.0005f));
		const size_t history = project.GetHistorySize();
		const size_t entities = project.GetEntityCount();
		const auto refused = [&](const nlohmann::json& parameters, std::string_view reason)
		{
			CAPTURE(parameters.dump());
			const EditorCommandResult result = project.Execute("lighting.makeAreaLight", parameters);
			CHECK_FALSE(result.Success);
			CAPTURE(result.Error);
			CHECK(result.Error.find(reason) != std::string::npos);
			CHECK(project.GetEntityCount() == entities);
			CHECK(project.GetHistorySize() == history);
		};
		refused({ { "entity", UUIDToJson(plain.GetUUID()) } }, "has no Mesh Renderer");
		refused({ { "entity", UUIDToJson(matte.GetUUID()) } }, "is not emissive");
		refused({ { "entity", UUIDToJson(wall.GetUUID()) } }, "the panel is symmetric: pass side");
		refused({ { "entity", UUIDToJson(cube.GetUUID()) } }, "is not a thin panel");
		refused({ { "entity", UUIDToJson(tiny.GetUUID()) } }, "is outside a Rect Light's sizes");
		refused({ { "entity", UUIDToJson(wall.GetUUID()) }, { "side", "+W" } }, "Parameter 'side'");
		refused({ { "entity", UUIDToJson(wall.GetUUID()) }, { "offset", 0.5 } }, "Parameter 'offset'");
		refused(nlohmann::json::object(), "entity");
		CHECK_FALSE(wall.HasComponent<LightSurfaceComponent>());
		CHECK_FALSE(matte.HasComponent<LightSurfaceComponent>());
		CHECK_FALSE(tiny.HasComponent<LightSurfaceComponent>());

		// While a scene is being opened the edited scene is read-only: the panels' scene saved, an empty one edited, and the panels'
		// opened again.
		const UUID wallId = wall.GetUUID();
		const UUID panels = AreaLightProject::ToHandle(project.Run("scene.saveAs", { { "path", "Scenes/Panels.stscene" } })["scene"]);
		project.Run("scene.new");
		project.Run("scene.saveAs", { { "path", "Scenes/Other.stscene" } });
		EditorCommandResult opening = project.Commands.Execute(project.Context, "scene.open", { { "scene", UUIDToJson(panels) } });
		REQUIRE(opening.IsPending());
		REQUIRE(project.Context.IsSceneLoading());
		const EditorCommandResult loading = project.Commands.Execute(project.Context, "lighting.makeAreaLight", { { "entity", UUIDToJson(wallId) },
			{ "side", "-Z" } });
		CHECK_FALSE(loading.Success);
		CHECK(loading.Error.find("read-only") != std::string::npos);
		REQUIRE(Tests::CompleteCommand(project.Context, std::move(opening)).Success);
		const Entity reopened = project.GetScene().GetEntityByUUID(wallId);
		REQUIRE(reopened);
		CHECK_FALSE(reopened.HasComponent<LightSurfaceComponent>());
		CHECK(reopened.GetChildren().empty());
	}

	TEST_CASE("lighting.makeAreaLight on a wall panel with a side, two-sided, without shadows, and offset")
	{
		AreaLightProject project(Tests::CreateTemporaryDirectory("MakeAreaLightSide") / "Panels");
		Scene& scene = project.GetScene();
		// Rotated 90 degrees around X: the panel's local +Y face looks toward +Z in the world.
		const Entity wall = project.AddPanel("Sign", project.Emissive, glm::vec3(2.0f, 1.5f, 0.0f), glm::vec3(2.0f, 0.1f, 0.5f),
			glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
		const nlohmann::json result = project.Run("lighting.makeAreaLight", { { "entity", UUIDToJson(wall.GetUUID()) }, { "side", "+Y" }, { "twoSided", true },
			{ "castShadows", false }, { "offset", 0.01 } });
		CHECK(result["side"] == "+Y");
		CHECK(result["lumens"].get<float>() == doctest::Approx(2.0f * glm::pi<float>() * 5000.0f * 2.0f * 0.5f).epsilon(1e-5));
		const Entity light = scene.GetEntityByUUID(AreaLightProject::ToHandle(result["light"]));
		REQUIRE(light);
		scene.UpdateWorldTransforms();
		const glm::mat4 world = scene.GetWorldTransform(light);
		const glm::vec3 facing = -glm::normalize(glm::vec3(world[2]));
		CHECK(facing.z == doctest::Approx(1.0f).epsilon(1e-5));
		// The face at z = 0.05, the light 1 cm in front of it.
		CHECK(world[3].z == doctest::Approx(0.06f).epsilon(1e-5));
		const RectLightComponent& component = light.GetComponent<RectLightComponent>();
		CHECK(component.Width * glm::length(glm::vec3(world[0])) == doctest::Approx(2.0f).epsilon(1e-5));
		CHECK(component.Height * glm::length(glm::vec3(world[1])) == doctest::Approx(0.5f).epsilon(1e-5));
		CHECK(component.TwoSided);
		CHECK_FALSE(component.CastShadows);
	}

	TEST_CASE("lighting.applySkyPreset while playing changes the running copy")
	{
		LightingHarness harness;
		Entity sky = harness.GetScene().CreateEntity("Sky");
		sky.AddComponent<SkyLightComponent>();
		const UUID skyId = sky.GetUUID();
		harness.Run("play.start");
		const EditorCommandResult result = harness.Execute("lighting.applySkyPreset", { { "preset", "Overcast" } });
		REQUIRE(result.Success);
		CHECK(result.RunningCopy);
		CHECK(result.Value.contains("warning"));
		CHECK(harness.GetScene().GetEntityByUUID(skyId).GetComponent<SkyLightComponent>().Source == SkyLightSource::Procedural);
		harness.Run("play.stop");
		CHECK(harness.GetScene().GetEntityByUUID(skyId).GetComponent<SkyLightComponent>().Source == SkyLightSource::EnvironmentMap);
	}
}
