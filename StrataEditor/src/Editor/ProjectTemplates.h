#pragma once

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

		std::span<const ProjectTemplate> GetAll();
		// Null for an unknown id.
		const ProjectTemplate* Find(std::string_view id);
		// The ids, for messages and schemas: "empty, basic3d".
		std::string ListIds();
		// Whether the template comes with a start scene (everything but "empty").
		bool HasStartScene(std::string_view id);
		// Adds the template's entities to a scene. "basic3d": a primary "Main Camera" (with an audio listener) at (0, 2, 6)
		// looking at the origin, a "Sun" directional light (intensity 3, rotation (-45, 30, 0), shadows), a "Sky" with a
		// procedural sky light, a 20 x 20 "Ground" plane with the default material, and a "Post Process" entity (ACES,
		// automatic exposure with +1 EV of compensation). "empty" adds nothing. False for an unknown id.
		bool Populate(std::string_view id, Scene& scene);
		// Where the editor camera looks at a new scene of the template from (nullopt: the template has no such view).
		std::optional<TemplateView> GetEditorView(std::string_view id);

	}

}
