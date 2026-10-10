#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Asset/BuiltinAssets.h>

#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Strata
{

	class Scene;

	// What a new project or scene starts with: project.create {template} and scene.new {template}, listed by
	// project.templates. The commands default to "empty" (what they always did); the editor's UI and the skills offer
	// "basic3d".
	struct ProjectTemplate
	{
		std::string_view Id;          // Stable identifier the commands take
		std::string_view Name;        // Display name
		std::string_view Description; // What a project made from it contains
	};

	// Assets of the project a template's scene uses (EditorContext::NewScene finds or makes them).
	struct TemplateAssets
	{
		AssetHandle GroundMaterial = BuiltinAssets::DefaultMaterial;
	};

	// The view of a template's scene the editor camera starts with.
	struct TemplateView
	{
		glm::vec3 Position;
		glm::vec3 Target;
	};

	namespace ProjectTemplates
	{

		constexpr std::string_view c_Empty = "empty";
		constexpr std::string_view c_Basic3D = "basic3d";
		// The start scene a project template with content saves (relative to the asset directory).
		constexpr std::string_view c_StartScenePath = "Scenes/Main.stscene";
		// basic3d's ground material, made in the project when it has none (relative to the asset directory).
		constexpr std::string_view c_GroundMaterialPath = "Materials/Ground.stmat";

		std::span<const ProjectTemplate> GetAll();
		// Null for an unknown id.
		const ProjectTemplate* Find(std::string_view id);
		// The ids, for messages and schemas: "empty, basic3d".
		std::string ListIds();
		// Whether the template comes with a start scene (everything but "empty").
		bool HasStartScene(std::string_view id);
		// Adds the template's entities to a scene. "basic3d": a primary "Main Camera" (with an audio listener) at (0, 2, 6)
		// looking at the origin, a "Sun" directional light (intensity 3, shadows) shining from GetBasic3DSunDirection, a "Sky"
		// with a procedural sky light, a 20 x 20 "Ground" plane with the assets' ground material, and a "Post Process"
		// entity (ACES, automatic exposure with +1 EV of compensation). "empty" adds nothing. False for an unknown id.
		bool Populate(std::string_view id, Scene& scene, const TemplateAssets& assets = {});
		// Whether the template's scene uses TemplateAssets::GroundMaterial.
		bool UsesGroundMaterial(std::string_view id);
		// The ground material (.stmat document): a dark, rough stone, so that what is made with the default (white)
		// material stands out against the ground, its top faces too.
		std::string GetGroundMaterialDocument();
		// Unit direction toward basic3d's sun: the preview sun's (SceneRenderer::GetPreviewSunDirection), 55 degrees up from
		// the +X+Z side, so the top, the side and the front of a box get clearly different light, from the template's camera
		// (top and front) as from the editor camera's default direction.
		glm::vec3 GetBasic3DSunDirection();
		// Where the editor camera looks at a new scene of the template from (nullopt: the template has no such view).
		std::optional<TemplateView> GetEditorView(std::string_view id);

	}

}
