#pragma once

#include "Renderer/SceneRendererTestUtils.h"

// The scene the ambient occlusion tests share.
namespace Strata::Tests
{

	// A box resting on a floor, lit by ambient light only, so ambient occlusion is all that differs between pixels.
	struct CreaseScene
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;
		SceneCamera Camera;

		explicit CreaseScene(AssetHandle floorMaterial = UUID::Null())
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			PostProcess->AmbientOcclusion = true;
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, floorMaterial, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
			AddMesh(SceneData, BuiltinAssets::CubeMesh, floorMaterial, glm::vec3(0.0f, 0.5f, 0.0f), "Box");
			SceneData.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.5f);
			Camera = LookAt(glm::vec3(0.0f, 2.5f, 3.0f), glm::vec3(0.0f, 0.3f, 0.0f));
		}

		glm::vec3 CreasePoint() const { return glm::vec3(0.0f, 0.0f, 0.56f); } // Floor at the box's front face
		glm::vec3 OpenFloorPoint() const { return glm::vec3(1.3f, 0.0f, 1.2f); } // More than the occlusion radius from the box
	};

}
