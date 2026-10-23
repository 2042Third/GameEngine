#include "Editor/ProbeBake.h"

#include <Strata/Core/Assert.h>
#include <Strata/Math/Math.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Strata::ProbeBake
{

	namespace
	{

		struct FaceBasis
		{
			glm::vec3 Forward;
			glm::vec3 Up;
		};

		// +X, -X, +Y, -Y, +Z, -Z, each looking along its axis; the vertical faces keep -Z and +Z up.
		constexpr FaceBasis c_Faces[6] = {
			{ { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
			{ { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
			{ { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } },
			{ { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
			{ { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f } },
			{ { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f } }
		};

		uint32_t SelectFace(const glm::vec3& direction)
		{
			const glm::vec3 magnitude = glm::abs(direction);
			if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
				return direction.x >= 0.0f ? 0u : 1u;
			if (magnitude.y >= magnitude.z)
				return direction.y >= 0.0f ? 2u : 3u;
			return direction.z >= 0.0f ? 4u : 5u;
		}

		// A face's RGB at a continuous pixel position (texel centers at +0.5), bilinear, clamped to the face.
		glm::vec3 SampleFace(const std::vector<float>& face, float x, float y)
		{
			const float maximum = static_cast<float>(c_FaceSize - 1);
			const float sx = std::clamp(x - 0.5f, 0.0f, maximum);
			const float sy = std::clamp(y - 0.5f, 0.0f, maximum);
			const uint32_t x0 = static_cast<uint32_t>(sx);
			const uint32_t y0 = static_cast<uint32_t>(sy);
			const uint32_t x1 = std::min(x0 + 1, c_FaceSize - 1);
			const uint32_t y1 = std::min(y0 + 1, c_FaceSize - 1);
			const float fx = sx - static_cast<float>(x0);
			const float fy = sy - static_cast<float>(y0);
			auto texel = [&](uint32_t tx, uint32_t ty)
			{
				const size_t index = (static_cast<size_t>(ty) * c_FaceSize + tx) * 3;
				return glm::vec3(face[index], face[index + 1], face[index + 2]);
			};
			const glm::vec3 top = glm::mix(texel(x0, y0), texel(x1, y0), fx);
			const glm::vec3 bottom = glm::mix(texel(x0, y1), texel(x1, y1), fx);
			return glm::mix(top, bottom, fy);
		}

	}

	SceneCamera GetFaceCamera(const glm::vec3& position, uint32_t face)
	{
		ST_CORE_ASSERT(face < 6, "Cube face {} out of range", face);
		SceneCamera camera;
		camera.View = glm::lookAt(position, position + c_Faces[face].Forward, c_Faces[face].Up);
		camera.Projection = Math::PerspectiveReverseZ(glm::half_pi<float>(), 1.0f, c_Near, c_Far);
		camera.Position = position;
		camera.Near = c_Near;
		camera.Far = c_Far;
		camera.VerticalFOV = glm::half_pi<float>();
		return camera;
	}

	std::vector<float> AssembleEquirect(const std::array<std::vector<float>, 6>& faces, float scale)
	{
		for (const std::vector<float>& face : faces)
			ST_CORE_ASSERT(face.size() == static_cast<size_t>(c_FaceSize) * c_FaceSize * 3, "A face has {} values", face.size());
		std::vector<float> image(static_cast<size_t>(c_CaptureWidth) * c_CaptureHeight * 3);
		const float size = static_cast<float>(c_FaceSize);
		for (uint32_t y = 0; y < c_CaptureHeight; y++)
		{
			// Include/IBL.glsl's mapping: u = atan(z, x) / 2 pi + 0.5 around +Y from -X, v from +Y (row 0) down to -Y.
			const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(c_CaptureHeight) * glm::pi<float>();
			for (uint32_t x = 0; x < c_CaptureWidth; x++)
			{
				const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(c_CaptureWidth) - 0.5f) * glm::two_pi<float>();
				const glm::vec3 direction(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
				const uint32_t face = SelectFace(direction);
				// Where the face's view sees the direction: its right and up over its depth, the view's 90 degrees making them NDC.
				const glm::vec3 forward = c_Faces[face].Forward;
				const glm::vec3 right = glm::normalize(glm::cross(forward, c_Faces[face].Up));
				const glm::vec3 up = glm::cross(right, forward);
				const float depth = glm::dot(direction, forward);
				const float ndcX = glm::dot(direction, right) / depth;
				const float ndcY = glm::dot(direction, up) / depth;
				// The framebuffer's origin is its top-left corner, NDC +Y up.
				const glm::vec3 color = SampleFace(faces[face], (ndcX * 0.5f + 0.5f) * size, (0.5f - ndcY * 0.5f) * size) * scale;
				const size_t index = (static_cast<size_t>(y) * c_CaptureWidth + x) * 3;
				image[index] = color.r;
				image[index + 1] = color.g;
				image[index + 2] = color.b;
			}
		}
		return image;
	}

	float ChoosePreExposure(Scene& scene)
	{
		float brightest = 0.0f;
		for (auto [entity, light, world] : scene.GetRegistry().view<DirectionalLightComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy)
				continue;
			const float illuminance = light.Intensity * std::max({ light.Color.r, light.Color.g, light.Color.b });
			if (std::isfinite(illuminance))
				brightest = std::max(brightest, illuminance);
		}
		// A power of two keeps the scaling exact; at most 2^-30 (a billion lux is no sun).
		const float stops = std::min(std::floor(std::log2(std::max(brightest, 1.0f))), 30.0f);
		return std::exp2(-stops);
	}

	SceneRendererSpecification GetRendererSpecification()
	{
		SceneRendererSpecification specification;
		specification.DebugName = "ProbeBake";
		specification.EntityIDs = false;
		specification.TemporalAntiAliasing = false;
		specification.TextureStreaming = false;
		specification.SpreadLightingUpdates = false;
		specification.FadeTextureLevels = false;
		specification.FadeLevelsOfDetail = false;
		specification.LocalShadowAtlasSize = 2048;
		return specification;
	}

}
