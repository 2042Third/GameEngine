#include "stpch.h"
#include "Strata/Scene/SceneRegistration.h"

#include "Strata/Core/Log.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyOptions.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/PrefabOverrides.h"

#include <charconv>
#include <string>
#include <vector>

namespace Strata
{

	namespace
	{

		// A 64-bit hash as 16 hex digits, and back; 0 (not known) is the empty string, and so is text that is no hash.
		std::string FormatHash(uint64_t hash)
		{
			return hash != 0 ? fmt::format("{:016X}", hash) : std::string();
		}

		uint64_t ParseHash(const std::string& text)
		{
			uint64_t hash = 0;
			const char* end = text.data() + text.size();
			const std::from_chars_result result = std::from_chars(text.data(), end, hash, 16);
			return result.ec == std::errc() && result.ptr == end ? hash : 0;
		}

		// The fields of the projection the camera does not use (CameraComponent::GetProjection), for the Inspector
		// (PropertyInfo::InactiveReason).
		std::string PerspectiveOnly(const CameraComponent& camera)
		{
			return camera.Projection == ProjectionType::Perspective ? std::string() : "The camera's projection is Orthographic";
		}

		std::string OrthographicOnly(const CameraComponent& camera)
		{
			return camera.Projection == ProjectionType::Orthographic ? std::string() : "The camera's projection is Perspective";
		}

	}

	void RegisterSceneComponents()
	{
		////////////////////////////////////////////////////////////////////////////////
		// Core
		////////////////////////////////////////////////////////////////////////////////

		ComponentRegistry::Register<IDComponent>("ID")
			.Category("Core")
			.Flags(ComponentFlags::Hidden | ComponentFlags::NotRemovable | ComponentFlags::NoSerialize)
			.EntityProperty("ID", &IDComponent::ID, { .Flags = PropertyFlags::ReadOnly });

		ComponentRegistry::Register<NameComponent>("Name")
			.Category("Core")
			.Flags(ComponentFlags::NotRemovable)
			.Property("Name", &NameComponent::Name);

		ComponentRegistry::Register<TransformComponent>("Transform")
			.Category("Core")
			.Flags(ComponentFlags::NotRemovable)
			.Description("Position, rotation and scale relative to the parent entity")
			.Property("Translation", &TransformComponent::Translation, Range(0.0f, 0.0f, 0.05f))
			.Property("Rotation", &TransformComponent::Rotation, WithTooltip({}, "Edited as Euler angles in degrees"))
			.Property("Scale", &TransformComponent::Scale, Range(0.0f, 0.0f, 0.01f));

		ComponentRegistry::Register<RelationshipComponent>("Relationship")
			.Category("Core")
			.Flags(ComponentFlags::Hidden | ComponentFlags::NotRemovable | ComponentFlags::NoSerialize)
			.EntityProperty("Parent", &RelationshipComponent::Parent, { .Flags = PropertyFlags::ReadOnly });

		ComponentRegistry::Register<TagComponent>("Tag")
			.Category("Core")
			.Description("Gameplay label used to find groups of entities")
			.Property("Tag", &TagComponent::Tag);

		ComponentRegistry::Register<InactiveComponent>("Inactive")
			.Category("Core")
			.Flags(ComponentFlags::Hidden)
			.Description("Deactivates the entity and its descendants");

		ComponentRegistry::Register<PrefabInstanceComponent>("PrefabInstance")
			.DisplayName("Prefab Instance")
			.Category("Core")
			.Flags(ComponentFlags::EngineAdded)
			.Description("Links this entity to the prefab it was created from")
			.AssetProperty("Prefab", &PrefabInstanceComponent::Prefab, AssetType::Prefab, { .Flags = PropertyFlags::ReadOnly })
			// The prefab-internal entity id is not a scene entity reference, so it is exposed as text and is never remapped.
			.CustomProperty("PrefabEntityID", PropertyType::String,
				[](const PrefabInstanceComponent& component) -> PropertyValue { return component.PrefabEntityID.ToString(); },
				[](PrefabInstanceComponent& component, const PropertyValue& value)
				{
					component.PrefabEntityID = UUID::FromString(std::get<std::string>(value)).value_or(UUID::Null());
				},
				{ .DisplayName = "Prefab Entity ID", .Flags = PropertyFlags::ReadOnly })
			// Hashes, as 16 hex digits (empty when not known), never edited by hand: the catch-up's bookkeeping (MarkSynced), which
			// a person cannot act on, so the inspector leaves them out (they are serialized and reported like the others).
			.CustomProperty("PrefabVersion", PropertyType::String,
				[](const PrefabInstanceComponent& component) -> PropertyValue { return FormatHash(component.PrefabVersion); },
				[](PrefabInstanceComponent& component, const PropertyValue& value) { component.PrefabVersion = ParseHash(std::get<std::string>(value)); },
				{ .DisplayName = "Prefab Version", .Flags = PropertyFlags::ReadOnly | PropertyFlags::Hidden })
			.CustomProperty("SyncedHash", PropertyType::String,
				[](const PrefabInstanceComponent& component) -> PropertyValue { return FormatHash(component.SyncedHash); },
				[](PrefabInstanceComponent& component, const PropertyValue& value) { component.SyncedHash = ParseHash(std::get<std::string>(value)); },
				{ .DisplayName = "Synced Hash", .Flags = PropertyFlags::ReadOnly | PropertyFlags::Hidden })
			// The instance's overrides (PrefabOverrides), written only when there are any: scenes without them stay as they were.
			// The component's data is whole (undo states, copies), so a document without the list clears it.
			.Extra([](const PrefabInstanceComponent& component, nlohmann::json& out)
				{
					if (!component.Overrides.empty())
						out["Overrides"] = PrefabOverrides::ToJson(component.Overrides);
				},
				[](PrefabInstanceComponent& component, const nlohmann::json& in, std::string*)
				{
					const auto overrides = in.find("Overrides");
					if (overrides == in.end())
					{
						component.Overrides.clear();
						return true;
					}
					std::vector<std::string> warnings;
					component.Overrides = PrefabOverrides::FromJson(*overrides, &warnings);
					for (const std::string& warning : warnings)
						ST_CORE_WARN("Prefab instance: {}", warning);
					return true;
				});

		////////////////////////////////////////////////////////////////////////////////
		// Rendering
		////////////////////////////////////////////////////////////////////////////////

		ComponentRegistry::Register<CameraComponent>("Camera")
			.Category("Rendering")
			.EnumProperty("Projection", &CameraComponent::Projection, { { "Perspective", 0 }, { "Orthographic", 1 } })
			.Property("PerspectiveFOV", &CameraComponent::PerspectiveFOV, WithTooltip(Range(1.0f, 179.0f, 0.1f), "Vertical field of view in degrees"))
			.Property("PerspectiveNear", &CameraComponent::PerspectiveNear, Range(0.001f, c_Unbounded, 0.01f))
			.Property("PerspectiveFar", &CameraComponent::PerspectiveFar, Range(0.01f, c_Unbounded, 1.0f))
			.Property("OrthographicSize", &CameraComponent::OrthographicSize, WithTooltip(Range(0.001f, c_Unbounded, 0.1f), "Vertical extent in world units"))
			.Property("OrthographicNear", &CameraComponent::OrthographicNear, Range(-c_Unbounded, c_Unbounded, 0.01f))
			.Property("OrthographicFar", &CameraComponent::OrthographicFar, Range(-c_Unbounded, c_Unbounded, 1.0f))
			.Property("Primary", &CameraComponent::Primary, WithTooltip({}, "The first active primary camera renders the game view"))
			.Property("ClearColor", &CameraComponent::ClearColor, Color());
		for (const char* property : { "PerspectiveFOV", "PerspectiveNear", "PerspectiveFar" })
			ComponentRegistry::SetInactiveReason<CameraComponent>(property, &PerspectiveOnly);
		for (const char* property : { "OrthographicSize", "OrthographicNear", "OrthographicFar" })
			ComponentRegistry::SetInactiveReason<CameraComponent>(property, &OrthographicOnly);

	}

}
