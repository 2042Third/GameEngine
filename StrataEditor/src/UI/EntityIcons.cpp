#include "UI/EntityIcons.h"

#include "UI/Icons.h"

#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/SceneHierarchy.h>

#include <string_view>

namespace Strata::UI
{

	namespace
	{

		struct ComponentIcon
		{
			std::string_view Name;
			const char* Icon;
		};

		// By registered component name; components of games and modules get the generic icon.
		constexpr ComponentIcon c_ComponentIcons[] = {
			{ "Transform", Icons::Move3d },
			{ "Tag", Icons::Tag },
			{ "PrefabInstance", Icons::Package },
			{ "Camera", Icons::Video },
			{ "MeshRenderer", Icons::Box },
			{ "DirectionalLight", Icons::Sun },
			{ "PointLight", Icons::Lightbulb },
			{ "SpotLight", Icons::Flashlight },
			{ "SkyLight", Icons::CloudSun },
			{ "PostProcess", Icons::Aperture },
			{ "Text", Icons::Type },
			{ "RigidBody", Icons::Weight },
			{ "BoxCollider", Icons::Cuboid },
			{ "SphereCollider", Icons::Globe },
			{ "CapsuleCollider", Icons::Pill },
			{ "MeshCollider", Icons::Shapes },
			{ "CharacterController", Icons::PersonStanding },
			{ "AudioSource", Icons::Volume2 },
			{ "AudioListener", Icons::Ear },
			{ "Script", Icons::FileCode }
		};

	}

	const char* GetComponentIcon(const ComponentInfo& component)
	{
		for (const ComponentIcon& entry : c_ComponentIcons)
		{
			if (entry.Name == component.Name)
				return entry.Icon;
		}
		return Icons::Puzzle;
	}

	const char* GetEntityIcon(const entt::registry& registry, entt::entity entity)
	{
		// What an entity is, most telling first: a camera or a light with a mesh is still a camera or a light.
		if (registry.all_of<CameraComponent>(entity))
			return Icons::Video;
		if (registry.all_of<DirectionalLightComponent>(entity))
			return Icons::Sun;
		if (registry.all_of<PointLightComponent>(entity))
			return Icons::Lightbulb;
		if (registry.all_of<SpotLightComponent>(entity))
			return Icons::Flashlight;
		if (registry.all_of<SkyLightComponent>(entity))
			return Icons::CloudSun;
		if (registry.all_of<PostProcessComponent>(entity))
			return Icons::Aperture;
		// A character with a mesh, a voice or a script is a character.
		if (registry.all_of<CharacterControllerComponent>(entity))
			return Icons::PersonStanding;
		if (registry.all_of<AudioSourceComponent>(entity))
			return Icons::Volume2;
		if (registry.all_of<AudioListenerComponent>(entity))
			return Icons::Ear;
		if (registry.all_of<TextComponent>(entity))
			return Icons::Type;
		if (registry.all_of<MeshRendererComponent>(entity))
			return Icons::Box;
		if (registry.all_of<PrefabInstanceComponent>(entity))
			return Icons::Package;
		if (registry.all_of<RigidBodyComponent>(entity))
			return Icons::Weight;
		if (registry.all_of<ScriptComponent>(entity))
			return Icons::FileCode;
		// Nothing but a transform: a group of its children, or an empty placeholder. Both get a full-size glyph, so the row
		// does not read as having no icon.
		const HierarchyComponent* hierarchy = registry.try_get<HierarchyComponent>(entity);
		return hierarchy && hierarchy->ChildCount > 0 ? Icons::Folder : Icons::SquareDashed;
	}

}
