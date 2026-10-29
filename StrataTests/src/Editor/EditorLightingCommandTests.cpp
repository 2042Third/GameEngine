#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Core/UUID.h>
#include <Strata/Math/Math.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Renderer/SkyPresets.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
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
