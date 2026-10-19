#include "stpch.h"
#include "Strata/Scene/Components.h"

namespace Strata
{

	glm::mat4 CameraComponent::GetProjection(float aspectRatio) const
	{
		if (!(aspectRatio > 0.0f) || !std::isfinite(aspectRatio))
			aspectRatio = 1.0f;

		if (Projection == ProjectionType::Perspective)
		{
			const float nearClip = std::max(PerspectiveNear, 1e-4f);
			const float farClip = std::max(PerspectiveFar, nearClip + 1e-3f);
			const float fov = glm::radians(std::clamp(PerspectiveFOV, 1.0f, 179.0f));
			return Math::PerspectiveReverseZ(fov, aspectRatio, nearClip, farClip);
		}

		const float halfHeight = std::max(OrthographicSize, 1e-4f) * 0.5f;
		const float halfWidth = halfHeight * aspectRatio;
		const float farClip = std::max(OrthographicFar, OrthographicNear + 1e-3f);
		return Math::OrthographicReverseZ(-halfWidth, halfWidth, -halfHeight, halfHeight, OrthographicNear, farClip);
	}

}
