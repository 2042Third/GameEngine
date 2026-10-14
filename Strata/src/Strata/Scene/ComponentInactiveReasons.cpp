#include "stpch.h"
#include "Strata/Scene/SceneRegistration.h"

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Components.h"

#include <string>

// Which properties of the built-in components have no effect in a component's state, and why (PropertyInfo::InactiveReason):
// the Inspector draws them disabled with the reason, component.get and entity.get list them. Each row follows the code that
// reads the property: SceneRenderer (the sky and its environment, the ambient light, automatic exposure, bloom, ambient
// occlusion), the camera's projection (CameraComponent::GetProjection) and the Animation system (the clip without a
// controller).

namespace Strata
{

	namespace
	{

		template<typename T>
		void SetReasons(std::initializer_list<const char*> properties, std::string (*reason)(const T&))
		{
			for (const char* property : properties)
				ComponentRegistry::SetInactiveReason<T>(property, reason);
		}

		std::string ProceduralSkyOnly(const SkyLightComponent& sky)
		{
			return sky.Source == SkyLightSource::Procedural ? std::string() : "Only the procedural sky uses it (Source is Environment Map)";
		}

		std::string EnvironmentMapOnly(const SkyLightComponent& sky)
		{
			return sky.Source == SkyLightSource::EnvironmentMap ? std::string() : "Only an environment map uses it (Source is Procedural)";
		}

		// The renderer lights with the environment once there is one: the procedural sky, or a map that is set.
		std::string WithoutEnvironmentOnly(const SkyLightComponent& sky)
		{
			if (sky.Source == SkyLightSource::Procedural)
				return "The procedural sky lights the scene: the ambient color applies only without an environment";
			if (sky.EnvironmentMap.IsValid())
				return "The environment map lights the scene: the ambient color applies only without one";
			return {};
		}

		std::string WithAutoExposure(const PostProcessComponent& post)
		{
			return post.AutoExposure ? std::string() : "Auto Exposure is off";
		}

		std::string WithBloom(const PostProcessComponent& post)
		{
			return post.Bloom ? std::string() : "Bloom is off";
		}

		std::string WithAmbientOcclusion(const PostProcessComponent& post)
		{
			return post.AmbientOcclusion ? std::string() : "Ambient Occlusion is off";
		}

		std::string PerspectiveOnly(const CameraComponent& camera)
		{
			return camera.Projection == ProjectionType::Perspective ? std::string() : "The camera's projection is Orthographic";
		}

		std::string OrthographicOnly(const CameraComponent& camera)
		{
			return camera.Projection == ProjectionType::Orthographic ? std::string() : "The camera's projection is Perspective";
		}

		std::string WithoutController(const AnimatorComponent& animator)
		{
			return animator.Controller.IsValid() ? "The controller's states choose the clips (Controller is set)" : std::string();
		}

	}

	void RegisterComponentInactiveReasons()
	{
		SetReasons<SkyLightComponent>({ "EnvironmentMap", "Rotation" }, &EnvironmentMapOnly);
		SetReasons<SkyLightComponent>({ "ZenithColor", "HorizonColor", "GroundColor", "SunSize", "SunIntensity" }, &ProceduralSkyOnly);
		SetReasons<SkyLightComponent>({ "AmbientColor" }, &WithoutEnvironmentOnly);
		// Exposure is the compensation SceneRenderer applies with automatic exposure too: always active.
		SetReasons<PostProcessComponent>({ "AutoExposureMinEV", "AutoExposureMaxEV", "AutoExposureSpeed" }, &WithAutoExposure);
		SetReasons<PostProcessComponent>({ "BloomIntensity", "BloomThreshold" }, &WithBloom);
		SetReasons<PostProcessComponent>({ "AmbientOcclusionRadius", "AmbientOcclusionIntensity" }, &WithAmbientOcclusion);
		SetReasons<CameraComponent>({ "PerspectiveFOV", "PerspectiveNear", "PerspectiveFar" }, &PerspectiveOnly);
		SetReasons<CameraComponent>({ "OrthographicSize", "OrthographicNear", "OrthographicFar" }, &OrthographicOnly);
		SetReasons<AnimatorComponent>({ "Clip" }, &WithoutController);
	}

}
