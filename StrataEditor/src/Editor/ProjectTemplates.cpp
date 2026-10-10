#include "Editor/ProjectTemplates.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Math/Math.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <array>

namespace Strata
{

	namespace
	{

		constexpr std::array<ProjectTemplate, 2> c_Templates = { {
			{ ProjectTemplates::c_Empty, "Empty", "A project with no scene: start from nothing." },
			{ ProjectTemplates::c_Basic3D, "Basic 3D",
				"A lit 3D start scene: a main camera with an audio listener, a sun with shadows, a procedural sky, a 20 x 20 ground plane and "
				"post-processing (ACES tone mapping, automatic exposure)." }
		} };

		constexpr glm::vec3 c_Basic3DCameraPosition = { 0.0f, 2.0f, 6.0f };
		constexpr glm::vec3 c_Basic3DSunRotation = { -45.0f, 30.0f, 0.0f }; // Euler degrees
		constexpr float c_Basic3DSunIntensity = 3.0f;
		constexpr float c_Basic3DGroundSize = 20.0f;
		constexpr float c_Basic3DExposureCompensation = 1.0f; // EV

		void PopulateBasic3D(Scene& scene)
		{
			Entity camera = scene.CreateEntity("Main Camera");
			TransformComponent& cameraTransform = camera.GetComponent<TransformComponent>();
			cameraTransform.Translation = c_Basic3DCameraPosition;
			cameraTransform.Rotation = Math::LookRotation(glm::normalize(-c_Basic3DCameraPosition));
			camera.AddComponent<CameraComponent>().Primary = true;
			camera.AddComponent<AudioListenerComponent>();

			Entity sun = scene.CreateEntity("Sun");
			sun.GetComponent<TransformComponent>().Rotation = Math::EulerDegreesToQuat(c_Basic3DSunRotation);
			DirectionalLightComponent& light = sun.AddComponent<DirectionalLightComponent>();
			light.Intensity = c_Basic3DSunIntensity;
			light.CastShadows = true;

			scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().Source = SkyLightSource::Procedural;

			Entity ground = scene.CreateEntity("Ground");
			ground.GetComponent<TransformComponent>().Scale = glm::vec3(c_Basic3DGroundSize, 1.0f, c_Basic3DGroundSize);
			MeshRendererComponent& renderer = ground.AddComponent<MeshRendererComponent>();
			renderer.Mesh = BuiltinAssets::PlaneMesh;
			renderer.Material = BuiltinAssets::DefaultMaterial;

			PostProcessComponent& postProcess = scene.CreateEntity("Post Process").AddComponent<PostProcessComponent>();
			postProcess.Tonemapper = TonemapOperator::ACES;
			postProcess.AutoExposure = true;
			// Automatic exposure brings the image's average to middle gray; with sunlit white surfaces (the default
			// material) filling most of the view that leaves the sky dim. One stop brighter reads as a sunny day.
			postProcess.Exposure = c_Basic3DExposureCompensation;
		}

	}

	namespace ProjectTemplates
	{

		std::span<const ProjectTemplate> GetAll()
		{
			return c_Templates;
		}

		const ProjectTemplate* Find(std::string_view id)
		{
			for (const ProjectTemplate& projectTemplate : c_Templates)
			{
				if (projectTemplate.Id == id)
					return &projectTemplate;
			}
			return nullptr;
		}

		std::string ListIds()
		{
			std::string ids;
			for (const ProjectTemplate& projectTemplate : c_Templates)
				ids += (ids.empty() ? "" : ", ") + std::string(projectTemplate.Id);
			return ids;
		}

		bool HasStartScene(std::string_view id)
		{
			return Find(id) && id != c_Empty;
		}

		bool Populate(std::string_view id, Scene& scene)
		{
			if (id == c_Empty)
				return true;
			if (id == c_Basic3D)
			{
				PopulateBasic3D(scene);
				return true;
			}
			return false;
		}

		std::optional<TemplateView> GetEditorView(std::string_view id)
		{
			// The game's view: the scene opens the way its camera shows it.
			if (id == c_Basic3D)
				return TemplateView { c_Basic3DCameraPosition, glm::vec3(0.0f) };
			return std::nullopt;
		}

	}

}
