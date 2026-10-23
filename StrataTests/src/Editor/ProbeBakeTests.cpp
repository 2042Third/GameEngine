#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/ProbeBake.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Math/Math.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/constants.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

using namespace Strata;

TEST_SUITE("Editor.ProbeBake")
{
	TEST_CASE("The six face views put together give every direction of the equirectangular capture its own view")
	{
		// Each face's texels hold the direction its camera sees through them (unprojected with the face's own matrices, as the
		// renderer draws them); the assembled image's texels must then hold their own directions (Include/IBL.glsl's mapping).
		const glm::vec3 position(3.0f, -2.0f, 7.0f);
		std::array<std::vector<float>, 6> faces;
		for (uint32_t face = 0; face < 6; face++)
		{
			const SceneCamera camera = ProbeBake::GetFaceCamera(position, face);
			CHECK(camera.Position == position);
			CHECK(camera.VerticalFOV == doctest::Approx(glm::half_pi<float>()));
			const glm::mat4 inverse = glm::inverse(camera.Projection * camera.View);
			faces[face].resize(static_cast<size_t>(ProbeBake::c_FaceSize) * ProbeBake::c_FaceSize * 3);
			for (uint32_t y = 0; y < ProbeBake::c_FaceSize; y++)
			{
				for (uint32_t x = 0; x < ProbeBake::c_FaceSize; x++)
				{
					// Pixel centers; the framebuffer's origin is its top-left corner, NDC +Y up, reversed-Z (1 near).
					const float ndcX = (static_cast<float>(x) + 0.5f) / ProbeBake::c_FaceSize * 2.0f - 1.0f;
					const float ndcY = 1.0f - (static_cast<float>(y) + 0.5f) / ProbeBake::c_FaceSize * 2.0f;
					const glm::vec4 nearPoint = inverse * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
					const glm::vec3 direction = glm::normalize(glm::vec3(nearPoint) / nearPoint.w - position);
					const size_t index = (static_cast<size_t>(y) * ProbeBake::c_FaceSize + x) * 3;
					faces[face][index] = direction.x;
					faces[face][index + 1] = direction.y;
					faces[face][index + 2] = direction.z;
				}
			}
		}

		const std::vector<float> image = ProbeBake::AssembleEquirect(faces, 2.0f);
		REQUIRE(image.size() == static_cast<size_t>(ProbeBake::c_CaptureWidth) * ProbeBake::c_CaptureHeight * 3);
		float worst = 0.0f;
		for (uint32_t y = 0; y < ProbeBake::c_CaptureHeight; y += 7)
		{
			const float theta = (static_cast<float>(y) + 0.5f) / ProbeBake::c_CaptureHeight * glm::pi<float>();
			for (uint32_t x = 0; x < ProbeBake::c_CaptureWidth; x += 5)
			{
				const float phi = ((static_cast<float>(x) + 0.5f) / ProbeBake::c_CaptureWidth - 0.5f) * glm::two_pi<float>();
				const glm::vec3 expected(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
				const size_t index = (static_cast<size_t>(y) * ProbeBake::c_CaptureWidth + x) * 3;
				// Scaled by 2 (the inverse pre-exposure); bilinear between texels of a quarter degree.
				const glm::vec3 actual = glm::vec3(image[index], image[index + 1], image[index + 2]) * 0.5f;
				worst = std::max(worst, glm::length(actual - expected));
			}
		}
		CHECK(worst < 0.01f);
		// Along the axes: +X is the middle of the image, +Y its top row, +Z three quarters across.
		const auto at = [&](uint32_t x, uint32_t y)
		{
			const size_t index = (static_cast<size_t>(y) * ProbeBake::c_CaptureWidth + x) * 3;
			return glm::vec3(image[index], image[index + 1], image[index + 2]) * 0.5f;
		};
		CHECK(at(ProbeBake::c_CaptureWidth / 2, ProbeBake::c_CaptureHeight / 2).x > 0.99f);
		CHECK(at(ProbeBake::c_CaptureWidth / 2, 0).y > 0.99f);
		CHECK(at(ProbeBake::c_CaptureWidth * 3 / 4, ProbeBake::c_CaptureHeight / 2).z > 0.99f);
	}

	TEST_CASE("A bake's pre-exposure brings the brightest directional light to about 1 lux, a power of two")
	{
		Scene scene;
		CHECK(ProbeBake::ChoosePreExposure(scene) == 1.0f);
		Entity sun = scene.CreateEntity("Sun");
		DirectionalLightComponent& light = sun.AddComponent<DirectionalLightComponent>();
		light.Intensity = 100000.0f;
		sun.MarkModified<DirectionalLightComponent>();
		CHECK(ProbeBake::ChoosePreExposure(scene) == std::exp2(-16.0f));
		light.Intensity = 3.0f;
		light.Color = glm::vec3(1.0f, 0.5f, 0.25f);
		sun.MarkModified<DirectionalLightComponent>();
		CHECK(ProbeBake::ChoosePreExposure(scene) == 0.5f);
		light.Intensity = 0.25f;
		sun.MarkModified<DirectionalLightComponent>();
		CHECK(ProbeBake::ChoosePreExposure(scene) == 1.0f);
		// The brighter of two lights.
		Entity moon = scene.CreateEntity("Moon");
		moon.AddComponent<DirectionalLightComponent>().Intensity = 40.0f;
		CHECK(ProbeBake::ChoosePreExposure(scene) == std::exp2(-5.0f));
	}

	TEST_CASE("The bake's renderer draws single pictures at full resolution")
	{
		const SceneRendererSpecification specification = ProbeBake::GetRendererSpecification();
		CHECK_FALSE(specification.EntityIDs);
		CHECK_FALSE(specification.TemporalAntiAliasing);
		CHECK_FALSE(specification.TextureStreaming);
		CHECK_FALSE(specification.SpreadLightingUpdates);
		CHECK_FALSE(specification.FadeTextureLevels);
		CHECK_FALSE(specification.FadeLevelsOfDetail);
	}

	TEST_CASE("A bake's staging folder is removed only once the job writing it completed")
	{
		// A bake cancelled while its files are written: the writer is held until the removal had time to run beside it on the
		// other I/O thread. A removal that did not wait would find no folder yet and leave the one the writer then makes.
		Tests::ScopedJobSystem jobs(1, 2);
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProbeBakeStaging") / "Bake";
		struct Gate
		{
			std::atomic<bool> Started { false };
			std::atomic<bool> Released { false };
		};
		const auto gate = std::make_shared<Gate>(); // Shared with the writer, which may outlive a failed check
		const JobHandle writer = JobSystem::SubmitIO([gate, directory]()
		{
			gate->Started = true;
			while (!gate->Released)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (FileSystem::CreateDirectories(directory))
				FileSystem::WriteBytes(directory / "Probe.hdr", std::vector<uint8_t>(64, 1));
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!gate->Started && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		const bool started = gate->Started;
		const JobHandle removal = ProbeBake::RemoveStaging(directory, writer);
		const auto grace = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
		while (!removal.IsComplete() && std::chrono::steady_clock::now() < grace)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK(started);
		CHECK_FALSE(removal.IsComplete()); // It waits for the writer
		gate->Released = true;
		JobSystem::Wait(removal);
		CHECK(writer.IsComplete());
		CHECK_FALSE(FileSystem::Exists(directory));
	}
}
