#include "Editor/ProjectTemplates.h"

#include <Strata/Audio/AudioComponents.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Math/Math.h>
#include <Strata/Physics/PhysicsComponents.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Renderer/SkyPresets.h>
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
				"A lit 3D start scene: a main camera with an audio listener, a sun and the physical atmosphere, a 20 x 20 ground plane of dark "
				"stone (Materials/Ground.stmat) with a box collider, so bodies and characters stand on it, and post-processing (Filmic tone mapping, "
				"automatic exposure)." }
		} };

		constexpr glm::vec3 c_Basic3DCameraPosition = { 0.0f, 2.0f, 6.0f };
		// The Clear sky preset's sun (SkyPresets.h): 40 degrees up, from the camera's right and slightly behind it, so objects at the
		// origin show a lit and a shaded side and a shadow falling left and back.
		constexpr SkyPreset c_Basic3DSkyPreset = SkyPreset::Clear;
		constexpr float c_Basic3DSunAzimuth = 20.0f;
		// The start scene's exposure compensation, by the rule the 3D samples follow (the median display luminance of its
		// camera's 1920x1080 TAA image within [0.15, 0.22], in steps of 0.25 EV): its bright sky over the dark stone ground
		// meters the ground below middle gray, 0.148 at Exposure 0 under the Clear preset's atmosphere and sun; +0.25 brings it
		// to 0.188.
		constexpr float c_Basic3DExposure = 0.25f;
		// The ground: a dark, warm stone (linear color), rough enough that the sky's reflection does not wash it out.
		constexpr glm::vec4 c_GroundColor = { 0.16f, 0.15f, 0.14f, 1.0f };
		constexpr float c_GroundRoughness = 0.9f;
		constexpr float c_Basic3DGroundSize = 20.0f;
		// The ground's collider: a box under the plane (the plane's own space, scaled with it to 20 x 20), its top flush with
		// the surface at y = 0, a meter thick so that fast bodies do not tunnel through.
		constexpr float c_Basic3DGroundColliderHalfThickness = 0.5f;

		void PopulateBasic3D(Scene& scene, const TemplateAssets& assets)
		{
			Entity camera = scene.CreateEntity("Main Camera");
			TransformComponent& cameraTransform = camera.GetComponent<TransformComponent>();
			cameraTransform.Translation = c_Basic3DCameraPosition;
			cameraTransform.Rotation = Math::LookRotation(glm::normalize(-c_Basic3DCameraPosition));
			camera.AddComponent<CameraComponent>().Primary = true;
			camera.AddComponent<AudioListenerComponent>();

			// Real light: the sun above the air and the physical atmosphere, as lighting.applySkyPreset's Clear writes them.
			const SkyPresetValues& preset = GetSkyPresetValues(c_Basic3DSkyPreset);
			Entity sun = scene.CreateEntity("Sun");
			// A directional light shines along its forward (-Z) axis: away from the sun.
			sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(-ProjectTemplates::GetBasic3DSunDirection());
			DirectionalLightComponent& light = sun.AddComponent<DirectionalLightComponent>();
			light.Intensity = preset.SunIlluminance;
			light.Color = preset.SunColor;
			light.ShadowSoftness = preset.ShadowSoftness;
			light.CastShadows = true;

			SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
			sky.Source = preset.Source;
			sky.Intensity = preset.SkyIntensity;
			sky.AtmosphereDensity = preset.AtmosphereDensity;
			sky.AtmosphereHaze = preset.AtmosphereHaze;
			sky.GroundAlbedo = preset.GroundAlbedo;
			sky.SunSize = preset.SunSize;

			Entity ground = scene.CreateEntity("Ground");
			ground.GetComponent<TransformComponent>().Scale = glm::vec3(c_Basic3DGroundSize, 1.0f, c_Basic3DGroundSize);
			MeshRendererComponent& renderer = ground.AddComponent<MeshRendererComponent>();
			renderer.Mesh = BuiltinAssets::PlaneMesh;
			renderer.Material = assets.GroundMaterial;
			// A collider without a rigid body is a static body of its own: what falls on the ground stays on it.
			BoxColliderComponent& collider = ground.AddComponent<BoxColliderComponent>();
			collider.HalfExtents = glm::vec3(0.5f, c_Basic3DGroundColliderHalfThickness, 0.5f);
			collider.Offset = glm::vec3(0.0f, -c_Basic3DGroundColliderHalfThickness, 0.0f);

			PostProcessComponent& postProcess = scene.CreateEntity("Post Process").AddComponent<PostProcessComponent>();
			// Filmic shows the metered average at display middle gray; the compensation is the scene's, for its composition.
			postProcess.Tonemapper = TonemapOperator::Filmic;
			postProcess.AutoExposure = true;
			postProcess.Exposure = c_Basic3DExposure;
			// Temporal anti-aliasing: steady edges and thin details (the sun's soft shadow needs none: it is the same in every frame
			// with any method; point and spot lights' shadows a game adds get their grain averaged away).
			postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
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

		bool Populate(std::string_view id, Scene& scene, const TemplateAssets& assets)
		{
			if (id == c_Empty)
				return true;
			if (id == c_Basic3D)
			{
				PopulateBasic3D(scene, assets);
				return true;
			}
			return false;
		}

		bool UsesGroundMaterial(std::string_view id)
		{
			return id == c_Basic3D;
		}

		std::string GetGroundMaterialDocument()
		{
			MaterialProperties properties;
			properties.BaseColor = c_GroundColor;
			properties.Roughness = c_GroundRoughness;
			return JsonUtils::Dump(Material::Create(properties)->Serialize(), 1, '\t') + "\n";
		}

		glm::vec3 GetBasic3DSunDirection()
		{
			return GetSkyPresetSunDirection(GetSkyPresetValues(c_Basic3DSkyPreset).SunElevation, c_Basic3DSunAzimuth);
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
