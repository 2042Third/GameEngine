#include "stpch.h"
#include "Strata/Scene/SceneRegistration.h"

#include "Strata/Math/Math.h"
#include "Strata/Reflection/ComponentGizmo.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Reflection/PropertyOptions.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/PhysicsLayerSettings.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>

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

		// A collision layer: an index the inspector offers by the scene's layer names.
		PropertyOptions PhysicsLayer(std::string tooltip)
		{
			PropertyOptions options = WithTooltip(Range(0.0f, static_cast<float>(c_PhysicsLayerCount - 1)), std::move(tooltip));
			options.Flags = PropertyFlags::PhysicsLayer;
			return options;
		}

		// Collider gizmos follow the entity's scale like rigid bodies do (box extents per axis, sphere radii by the largest
		// axis, capsule radii by X and Z and heights by Y) and never its shear: the shape's frame is the entity's rotation and
		// translation, moved by the collider's offset (scaled like the entity). False for a transform that does not decompose.
		bool GetColliderFrame(const glm::mat4& world, const glm::vec3& offset, glm::mat4& outFrame, glm::vec3& outScale)
		{
			glm::vec3 translation(0.0f);
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 scale(1.0f);
			if (!Math::DecomposeTransform(world, translation, rotation, scale))
				return false;
			const glm::vec3 center = translation + rotation * (offset * scale);
			outFrame = glm::translate(glm::mat4(1.0f), center) * glm::mat4_cast(rotation);
			outScale = glm::abs(scale);
			return true;
		}

		bool BoxColliderGizmo(const entt::registry&, entt::entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const BoxColliderComponent& collider = *static_cast<const BoxColliderComponent*>(component);
			glm::vec3 scale(1.0f);
			if (!GetColliderFrame(world, collider.Offset, outShape.Transform, scale))
				return false;
			outShape.Type = ComponentGizmoShape::Kind::Box;
			outShape.HalfExtents = collider.HalfExtents * scale;
			return true;
		}

		bool SphereColliderGizmo(const entt::registry&, entt::entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const SphereColliderComponent& collider = *static_cast<const SphereColliderComponent*>(component);
			glm::vec3 scale(1.0f);
			if (!GetColliderFrame(world, collider.Offset, outShape.Transform, scale))
				return false;
			outShape.Type = ComponentGizmoShape::Kind::Sphere;
			outShape.Radius = collider.Radius * std::max({ scale.x, scale.y, scale.z });
			return true;
		}

		bool CapsuleColliderGizmo(const entt::registry&, entt::entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const CapsuleColliderComponent& collider = *static_cast<const CapsuleColliderComponent*>(component);
			glm::vec3 scale(1.0f);
			if (!GetColliderFrame(world, collider.Offset, outShape.Transform, scale))
				return false;
			outShape.Type = ComponentGizmoShape::Kind::Capsule;
			outShape.Radius = collider.Radius * std::max(scale.x, scale.z);
			outShape.HalfHeight = collider.HalfHeight * scale.y;
			return true;
		}

		// The bounds of the collider's mesh, or of the Mesh Renderer's when it names none.
		bool MeshColliderGizmo(const entt::registry& registry, entt::entity entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const MeshColliderComponent& collider = *static_cast<const MeshColliderComponent*>(component);
			AssetHandle mesh = collider.Mesh;
			if (!mesh.IsValid())
			{
				const MeshRendererComponent* renderer = registry.try_get<MeshRendererComponent>(entity);
				mesh = renderer ? renderer->Mesh : AssetHandle::Null();
			}
			if (!mesh.IsValid())
				return false;
			outShape.Type = ComponentGizmoShape::Kind::MeshBounds;
			outShape.Transform = world;
			outShape.Mesh = mesh;
			return true;
		}

		// A character controller's capsule stands upright on the entity's origin, whatever the entity's rotation and scale.
		bool CharacterControllerGizmo(const entt::registry&, entt::entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const CharacterControllerComponent& controller = *static_cast<const CharacterControllerComponent*>(component);
			const float height = std::abs(controller.Height);
			const float radius = std::min(std::abs(controller.Radius), height * 0.5f);
			if (!std::isfinite(height) || !std::isfinite(radius))
				return false;
			const glm::vec3 center = glm::vec3(world[3]) + glm::vec3(0.0f, height * 0.5f, 0.0f);
			outShape.Type = ComponentGizmoShape::Kind::Capsule;
			outShape.Transform = glm::translate(glm::mat4(1.0f), center);
			outShape.Radius = radius;
			outShape.HalfHeight = height * 0.5f - radius;
			return true;
		}

		void SerializeScriptComponent(const ScriptComponent& component, nlohmann::json& out)
		{
			nlohmann::json scripts = nlohmann::json::array();
			for (const ScriptEntry& script : component.Scripts)
			{
				nlohmann::json fields = nlohmann::json::object();
				for (const ScriptFieldValue& field : script.Fields)
				{
					// Script fields have no named enum options, so enum values are stored as plain integers.
					const PropertyType type = field.Type == PropertyType::Enum ? PropertyType::Int : field.Type;
					PropertyInfo info;
					info.Name = field.Name;
					info.Type = type;
					if (field.Value.index() != GetPropertyValueIndex(type))
						continue;
					fields[field.Name] = { { "Type", PropertyTypeToString(type) }, { "Value", PropertyValueToJson(info, field.Value) } };
				}
				scripts.push_back({ { "Class", script.ClassName }, { "Fields", fields } });
			}
			out["Scripts"] = scripts;
		}

		bool DeserializeScriptComponent(ScriptComponent& component, const nlohmann::json& in, std::string* outError)
		{
			if (!in.contains("Scripts"))
				return true;
			if (!in["Scripts"].is_array())
			{
				if (outError)
					*outError = "'Scripts' must be an array";
				return false;
			}

			component.Scripts.clear();
			for (const nlohmann::json& scriptJson : in["Scripts"])
			{
				if (!scriptJson.is_object() || !scriptJson.contains("Class") || !scriptJson["Class"].is_string())
				{
					if (outError)
						*outError = "Each script needs a 'Class' name";
					return false;
				}

				ScriptEntry& script = component.Scripts.emplace_back();
				script.ClassName = scriptJson["Class"].get<std::string>();
				if (!scriptJson.contains("Fields") || !scriptJson["Fields"].is_object())
					continue;

				for (const auto& [fieldName, fieldJson] : scriptJson["Fields"].items())
				{
					// A malformed field is skipped (keeping every other script and field) rather than failing the component.
					if (!fieldJson.is_object() || !fieldJson.contains("Type") || !fieldJson["Type"].is_string() || !fieldJson.contains("Value"))
					{
						ST_CORE_WARN("Script field '{}.{}' needs 'Type' and 'Value'; skipped", script.ClassName, fieldName);
						continue;
					}

					std::optional<PropertyType> type = PropertyTypeFromString(fieldJson["Type"].get<std::string>());
					if (type == PropertyType::Enum)
						type = PropertyType::Int;
					if (!type)
					{
						ST_CORE_WARN("Script field '{}.{}' has an unsupported type; skipped", script.ClassName, fieldName);
						continue;
					}

					PropertyInfo info;
					info.Name = fieldName;
					info.Type = *type;
					std::string error;
					std::optional<PropertyValue> value = PropertyValueFromJson(info, fieldJson["Value"], &error);
					if (!value)
					{
						ST_CORE_WARN("{}; skipped", error);
						continue;
					}
					script.Fields.push_back(ScriptFieldValue { fieldName, *type, *value });
				}
			}
			return true;
		}

		// The entity and asset references of script fields: slot is the script, subslot the field.
		void VisitScriptReferences(const ScriptComponent& component, const ComponentExtraReferences::Visitor& visitor)
		{
			for (uint32_t script = 0; script < component.Scripts.size(); script++)
			{
				const std::vector<ScriptFieldValue>& fields = component.Scripts[script].Fields;
				for (uint32_t field = 0; field < fields.size(); field++)
				{
					const ScriptFieldValue& value = fields[field];
					if ((value.Type == PropertyType::Entity || value.Type == PropertyType::Asset) && std::holds_alternative<UUID>(value.Value))
						visitor(script, field, value.Type, std::get<UUID>(value.Value));
				}
			}
		}

		void SetScriptReference(ScriptComponent& component, uint32_t script, uint32_t field, UUID value)
		{
			if (script < component.Scripts.size() && field < component.Scripts[script].Fields.size())
				component.Scripts[script].Fields[field].Value = value;
		}

		// The same scripts with the same field overrides, entity fields of `a` mapped through `remap`.
		bool ScriptsEqual(const ScriptComponent& a, const ScriptComponent& b, const ComponentExtraReferences::Remap& remap)
		{
			if (a.Scripts.size() != b.Scripts.size())
				return false;
			for (size_t script = 0; script < a.Scripts.size(); script++)
			{
				const ScriptEntry& x = a.Scripts[script];
				const ScriptEntry& y = b.Scripts[script];
				if (x.ClassName != y.ClassName || x.Fields.size() != y.Fields.size())
					return false;
				for (size_t field = 0; field < x.Fields.size(); field++)
				{
					const ScriptFieldValue& first = x.Fields[field];
					const ScriptFieldValue& second = y.Fields[field];
					if (first.Name != second.Name || first.Type != second.Type)
						return false;
					PropertyValue value = first.Value;
					if (const UUID* entity = std::get_if<UUID>(&value); entity && first.Type == PropertyType::Entity)
						value = remap(*entity);
					if (value != second.Value)
						return false;
				}
			}
			return true;
		}

		void VisitScriptReferencesJson(const nlohmann::json& extra, const ComponentExtraReferences::JsonVisitor& visitor)
		{
			ScriptComponent scripts;
			if (!DeserializeScriptComponent(scripts, extra, nullptr))
				return;
			VisitScriptReferences(scripts, [&visitor](uint32_t, uint32_t, PropertyType type, UUID value) { visitor(type, value); });
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
				{ .DisplayName = "Synced Hash", .Flags = PropertyFlags::ReadOnly | PropertyFlags::Hidden });

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

		ComponentRegistry::Register<MeshRendererComponent>("MeshRenderer")
			.DisplayName("Mesh Renderer")
			.Category("Rendering")
			.AssetProperty("Mesh", &MeshRendererComponent::Mesh, AssetType::Mesh)
			.AssetProperty("Material", &MeshRendererComponent::Material, AssetType::Material, { .Tooltip = "Overrides every submesh material when set" })
			.Property("CastShadows", &MeshRendererComponent::CastShadows);

		ComponentRegistry::Register<DirectionalLightComponent>("DirectionalLight")
			.DisplayName("Directional Light")
			.Category("Lighting")
			.Description("Sun-like light shining along the entity's forward (-Z) direction")
			.Property("Color", &DirectionalLightComponent::Color, Color())
			.Property("Intensity", &DirectionalLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.05f))
			.Property("CastShadows", &DirectionalLightComponent::CastShadows)
			.Property("ShadowDistance", &DirectionalLightComponent::ShadowDistance, Range(1.0f, 10000.0f, 1.0f))
			.Property("ShadowSoftness", &DirectionalLightComponent::ShadowSoftness, Range(0.0f, 10.0f, 0.01f))
			.Property("ShadowBias", &DirectionalLightComponent::ShadowBias, Range(0.0f, 10.0f, 0.01f))
			.Property("ShadowNormalBias", &DirectionalLightComponent::ShadowNormalBias, Range(0.0f, 10.0f, 0.01f));

		ComponentRegistry::Register<PointLightComponent>("PointLight")
			.DisplayName("Point Light")
			.Category("Lighting")
			.Description("Light shining in every direction from the entity's position (casts no shadows)")
			.Property("Color", &PointLightComponent::Color, Color())
			.Property("Intensity", &PointLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.1f))
			.Property("Range", &PointLightComponent::Range, WithTooltip(Range(0.01f, c_Unbounded, 0.1f), "Light fades to zero at this distance"));

		ComponentRegistry::Register<SpotLightComponent>("SpotLight")
			.DisplayName("Spot Light")
			.Category("Lighting")
			.Description("Cone light shining along the entity's forward (-Z) direction (casts no shadows)")
			.Property("Color", &SpotLightComponent::Color, Color())
			.Property("Intensity", &SpotLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.1f))
			.Property("Range", &SpotLightComponent::Range, Range(0.01f, c_Unbounded, 0.1f))
			.Property("InnerConeAngle", &SpotLightComponent::InnerConeAngle, WithTooltip(Range(0.0f, 89.0f, 0.1f), "Half angle in degrees"))
			.Property("OuterConeAngle", &SpotLightComponent::OuterConeAngle, WithTooltip(Range(0.1f, 89.9f, 0.1f), "Half angle in degrees"));

		ComponentRegistry::Register<SkyLightComponent>("SkyLight")
			.DisplayName("Sky Light")
			.Category("Lighting")
			.Description("Image-based lighting and sky background from an HDR environment map or a procedural sky")
			.EnumProperty("Source", &SkyLightComponent::Source, { { "EnvironmentMap", 0, "Environment Map" }, { "Procedural", 1 } },
				{ .Tooltip = "EnvironmentMap: an HDR texture; Procedural: a sky computed from the colors below and the directional light" })
			.AssetProperty("EnvironmentMap", &SkyLightComponent::EnvironmentMap, AssetType::Texture, { .Tooltip = "Equirectangular HDR texture" })
			.Property("Intensity", &SkyLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.01f))
			.Property("Rotation", &SkyLightComponent::Rotation, WithTooltip(Range(-360.0f, 360.0f, 0.5f), "Degrees around +Y (environment maps only)"))
			.Property("ShowBackground", &SkyLightComponent::ShowBackground)
			.Property("BackgroundBlur", &SkyLightComponent::BackgroundBlur, Slider(0.0f, 1.0f))
			.Property("AmbientColor", &SkyLightComponent::AmbientColor, WithTooltip(Color(), "Ambient light while there is no environment (no map set)"))
			.Property("ZenithColor", &SkyLightComponent::ZenithColor, WithTooltip(Color(), "Procedural sky: color straight up (linear radiance)"))
			.Property("HorizonColor", &SkyLightComponent::HorizonColor, WithTooltip(Color(), "Procedural sky: color at the horizon (linear radiance)"))
			.Property("GroundColor", &SkyLightComponent::GroundColor, WithTooltip(Color(), "Procedural sky: color below the horizon (linear radiance)"))
			.Property("SunSize", &SkyLightComponent::SunSize, WithTooltip(Range(0.0f, 20.0f, 0.01f), "Procedural sky: angular diameter of the sun's disk in degrees (0 hides it)"))
			.Property("SunIntensity", &SkyLightComponent::SunIntensity,
				WithTooltip(Range(0.0f, 60000.0f, 0.1f), "Procedural sky: radiance of the sun's disk, times the directional light's color"));

		ComponentRegistry::Register<PostProcessComponent>("PostProcess")
			.DisplayName("Post Process")
			.Category("Rendering")
			.Description("Tonemapping, exposure, bloom, ambient occlusion and anti-aliasing settings")
			.EnumProperty("Tonemapper", &PostProcessComponent::Tonemapper, { { "None", 0 }, { "Reinhard", 1 }, { "ACES", 2 }, { "AgX", 3 },
				{ "KhronosNeutral", 4, "Khronos Neutral" } })
			.Property("Exposure", &PostProcessComponent::Exposure, WithTooltip(Range(-16.0f, 16.0f, 0.05f), "Exposure compensation in EV"))
			.Property("AutoExposure", &PostProcessComponent::AutoExposure)
			.Property("AutoExposureMinEV", &PostProcessComponent::AutoExposureMinEV,
				WithTooltip(Range(-12.0f, 16.0f, 0.1f), "Darkest average scene luminance (log2) automatic exposure brightens up to"))
			.Property("AutoExposureMaxEV", &PostProcessComponent::AutoExposureMaxEV,
				WithTooltip(Range(-12.0f, 16.0f, 0.1f), "Brightest average scene luminance (log2) automatic exposure darkens down to"))
			.Property("AutoExposureSpeed", &PostProcessComponent::AutoExposureSpeed, WithTooltip(Range(0.0f, 20.0f, 0.05f), "Adaptation rate per second; 0 = instant"))
			.Property("Bloom", &PostProcessComponent::Bloom)
			.Property("BloomIntensity", &PostProcessComponent::BloomIntensity, WithTooltip(Slider(0.0f, 1.0f), "Fraction of the light scattered into the glow"))
			.Property("BloomThreshold", &PostProcessComponent::BloomThreshold,
				WithTooltip(Range(0.0f, 20.0f, 0.05f), "Only light brighter than this (after exposure) blooms; 0 = all light, physically based"))
			.Property("AmbientOcclusion", &PostProcessComponent::AmbientOcclusion)
			.Property("AmbientOcclusionRadius", &PostProcessComponent::AmbientOcclusionRadius, WithTooltip(Range(0.01f, 10.0f, 0.01f), "World units"))
			.Property("AmbientOcclusionIntensity", &PostProcessComponent::AmbientOcclusionIntensity, Slider(0.0f, 4.0f))
			.Property("AntiAliasing", &PostProcessComponent::AntiAliasing, WithTooltip({}, "Fast approximate anti-aliasing (FXAA)"))
			.Property("Vignette", &PostProcessComponent::Vignette, Slider(0.0f, 1.0f))
			.Property("Saturation", &PostProcessComponent::Saturation, Slider(0.0f, 2.0f))
			.Property("Contrast", &PostProcessComponent::Contrast, WithTooltip(Slider(0.5f, 2.0f), "Around middle gray; 1 = unchanged"));

		ComponentRegistry::Register<TextComponent>("Text")
			.Category("UI")
			.Description("Text drawn in screen space (HUD) or in the world")
			.Property("Text", &TextComponent::Text, { .Flags = PropertyFlags::MultiLine })
			.AssetProperty("Font", &TextComponent::Font, AssetType::Font)
			.Property("Color", &TextComponent::Color, Color())
			.Property("FontSize", &TextComponent::FontSize, Range(1.0f, 1024.0f, 0.5f))
			.Property("ScreenSpace", &TextComponent::ScreenSpace)
			.Property("ScreenAnchor", &TextComponent::ScreenAnchor, WithTooltip(Range(0.0f, 1.0f, 0.005f), "Normalized viewport position; (0,0) is the top-left corner"))
			.Property("ScreenOffset", &TextComponent::ScreenOffset, WithTooltip({}, "Pixel offset from the anchor (+Y down)"))
			.EnumProperty("Alignment", &TextComponent::Alignment, { { "Left", 0 }, { "Center", 1 }, { "Right", 2 } });

		////////////////////////////////////////////////////////////////////////////////
		// Physics
		////////////////////////////////////////////////////////////////////////////////

		ComponentRegistry::Register<RigidBodyComponent>("RigidBody")
			.DisplayName("Rigid Body")
			.Category("Physics")
			.EnumProperty("Type", &RigidBodyComponent::Type, { { "Static", 0 }, { "Dynamic", 1 }, { "Kinematic", 2 } })
			.Property("Mass", &RigidBodyComponent::Mass, Range(0.001f, c_Unbounded, 0.1f))
			.Property("LinearDamping", &RigidBodyComponent::LinearDamping, Range(0.0f, 100.0f, 0.01f))
			.Property("AngularDamping", &RigidBodyComponent::AngularDamping, Range(0.0f, 100.0f, 0.01f))
			.Property("GravityScale", &RigidBodyComponent::GravityScale, Range(-100.0f, 100.0f, 0.05f))
			.Property("Friction", &RigidBodyComponent::Friction, Range(0.0f, 10.0f, 0.01f))
			.Property("Restitution", &RigidBodyComponent::Restitution, Slider(0.0f, 1.0f))
			.Property("IsTrigger", &RigidBodyComponent::IsTrigger, WithTooltip({}, "Reports overlaps without colliding"))
			.Property("ContinuousCollision", &RigidBodyComponent::ContinuousCollision)
			.Property("LockRotationX", &RigidBodyComponent::LockRotationX)
			.Property("LockRotationY", &RigidBodyComponent::LockRotationY)
			.Property("LockRotationZ", &RigidBodyComponent::LockRotationZ)
			.Property("Layer", &RigidBodyComponent::Layer, PhysicsLayer("Collision layer: it collides with the layers the scene's layer matrix lets it"));

		ComponentRegistry::Register<BoxColliderComponent>("BoxCollider")
			.DisplayName("Box Collider")
			.Category("Physics")
			.Property("HalfExtents", &BoxColliderComponent::HalfExtents, Range(0.0f, 0.0f, 0.01f))
			.Property("Offset", &BoxColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f))
			.Gizmo(ComponentGizmoGroup::Collider, BoxColliderGizmo);

		ComponentRegistry::Register<SphereColliderComponent>("SphereCollider")
			.DisplayName("Sphere Collider")
			.Category("Physics")
			.Property("Radius", &SphereColliderComponent::Radius, Range(0.001f, c_Unbounded, 0.01f))
			.Property("Offset", &SphereColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f))
			.Gizmo(ComponentGizmoGroup::Collider, SphereColliderGizmo);

		ComponentRegistry::Register<CapsuleColliderComponent>("CapsuleCollider")
			.DisplayName("Capsule Collider")
			.Category("Physics")
			.Property("Radius", &CapsuleColliderComponent::Radius, Range(0.001f, c_Unbounded, 0.01f))
			.Property("HalfHeight", &CapsuleColliderComponent::HalfHeight, Range(0.0f, c_Unbounded, 0.01f))
			.Property("Offset", &CapsuleColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f))
			.Gizmo(ComponentGizmoGroup::Collider, CapsuleColliderGizmo);

		ComponentRegistry::Register<MeshColliderComponent>("MeshCollider")
			.DisplayName("Mesh Collider")
			.Category("Physics")
			.AssetProperty("Mesh", &MeshColliderComponent::Mesh, AssetType::Mesh, { .Tooltip = "Uses the Mesh Renderer's mesh when unset" })
			.Property("Convex", &MeshColliderComponent::Convex, WithTooltip({}, "Non-convex meshes are only supported on static bodies"))
			.Gizmo(ComponentGizmoGroup::Collider, MeshColliderGizmo);

		ComponentRegistry::Register<CharacterControllerComponent>("CharacterController")
			.DisplayName("Character Controller")
			.Category("Physics")
			.Description("An upright capsule that scripts walk, jump and teleport: it slides along walls, walks up steps and walkable slopes and "
				"pushes dynamic bodies; its bottom is the entity's origin")
			.Property("Height", &CharacterControllerComponent::Height,
				WithTooltip(Range(0.01f, 1000.0f, 0.01f), "Total height of the capsule in world units; its bottom is the entity's origin"))
			.Property("Radius", &CharacterControllerComponent::Radius, WithTooltip(Range(0.005f, 500.0f, 0.01f), "World units"))
			.Property("StepHeight", &CharacterControllerComponent::StepHeight,
				WithTooltip(Range(0.0f, 100.0f, 0.01f), "Highest step it walks up, and how far it follows the ground down (world units)"))
			.Property("MaxSlopeDegrees", &CharacterControllerComponent::MaxSlopeDegrees,
				WithTooltip(Range(0.0f, 89.0f, 0.5f), "Steepest slope it walks up, in degrees from the horizontal"))
			.Property("Layer", &CharacterControllerComponent::Layer, PhysicsLayer("Collision layer: it collides with the layers the scene's layer matrix lets it"))
			.Property("GravityScale", &CharacterControllerComponent::GravityScale, WithTooltip(Range(-100.0f, 100.0f, 0.05f), "Times the scene's gravity"))
			.Property("PushStrength", &CharacterControllerComponent::PushStrength,
				WithTooltip(Range(0.0f, c_Unbounded, 1.0f), "Largest force with which it pushes dynamic bodies, in newtons"))
			.Property("Enabled", &CharacterControllerComponent::Enabled, WithTooltip({}, "A disabled controller neither moves nor collides"))
			.Gizmo(ComponentGizmoGroup::Collider, CharacterControllerGizmo);

		////////////////////////////////////////////////////////////////////////////////
		// Audio
		////////////////////////////////////////////////////////////////////////////////

		ComponentRegistry::Register<AudioSourceComponent>("AudioSource")
			.DisplayName("Audio Source")
			.Category("Audio")
			.AssetProperty("Clip", &AudioSourceComponent::Clip, AssetType::AudioClip)
			.Property("Volume", &AudioSourceComponent::Volume, Slider(0.0f, 4.0f))
			.Property("Pitch", &AudioSourceComponent::Pitch, Range(0.01f, 8.0f, 0.01f))
			.Property("Loop", &AudioSourceComponent::Loop)
			.Property("PlayOnStart", &AudioSourceComponent::PlayOnStart)
			.Property("Spatial", &AudioSourceComponent::Spatial)
			.Property("MinDistance", &AudioSourceComponent::MinDistance, Range(0.0f, c_Unbounded, 0.1f))
			.Property("MaxDistance", &AudioSourceComponent::MaxDistance, Range(0.0f, c_Unbounded, 0.5f))
			.Property("Rolloff", &AudioSourceComponent::Rolloff, Range(0.0f, 100.0f, 0.01f));

		ComponentRegistry::Register<AudioListenerComponent>("AudioListener")
			.DisplayName("Audio Listener")
			.Category("Audio")
			.Property("Active", &AudioListenerComponent::Active);

		////////////////////////////////////////////////////////////////////////////////
		// Animation
		////////////////////////////////////////////////////////////////////////////////

		ComponentRegistry::Register<SkinComponent>("Skin")
			.Category("Animation")
			.Description("Poses the entity's skinned mesh with an animator's joints")
			.EntityProperty("Animator", &SkinComponent::Animator, { .Tooltip = "The entity with the Animator whose pose moves this mesh" });

		// Speeds are bounded like a controller state's (AnimationController::c_MaxSpeed, which the Scene layer cannot include).
		ComponentRegistry::Register<AnimatorComponent>("Animator")
			.Category("Animation")
			.Description("Plays a skeleton's animations: an animation controller's states, or one clip")
			.AssetProperty("Skeleton", &AnimatorComponent::Skeleton, AssetType::Skeleton, { .Tooltip = "The joints the animations move (a model's Skeleton)" })
			.AssetProperty("Controller", &AnimatorComponent::Controller, AssetType::AnimationController, { .Tooltip = "States, transitions and parameters; without one the animator plays Clip" })
			.AssetProperty("Clip", &AnimatorComponent::Clip, AssetType::AnimationClip, { .Tooltip = "The clip played without a controller" })
			.Property("Speed", &AnimatorComponent::Speed, WithTooltip(Range(-100.0f, 100.0f, 0.01f), "Playback rate; negative plays backwards"))
			.Property("Loop", &AnimatorComponent::Loop, WithTooltip({}, "Whether the clip (without a controller) repeats"))
			.Property("PlayOnStart", &AnimatorComponent::PlayOnStart, WithTooltip({}, "Starts playing when the scene starts"))
			.Property("ApplyRootMotion", &AnimatorComponent::ApplyRootMotion, WithTooltip({}, "Moves the entity by the root joint's horizontal motion and turn"))
			.EnumProperty("CullingMode", &AnimatorComponent::CullingMode, { { "AlwaysAnimate", 0, "Always Animate" }, { "WhenVisible", 1, "When Visible" } },
				{ .Tooltip = "When Visible poses the skeleton only while a renderer draws a mesh it skins or its shadow; one that skins no mesh but moves attached parts poses always" });

		ComponentRegistry::Register<AttachToJointComponent>("AttachToJoint")
			.DisplayName("Attach To Joint")
			.Category("Animation")
			.Description("Places the entity at a joint of an animator's pose (a sword in a hand)")
			.EntityProperty("Animator", &AttachToJointComponent::Animator, { .Tooltip = "The entity with the Animator" })
			.Property("Joint", &AttachToJointComponent::Joint, WithTooltip({}, "The joint's name in the animator's skeleton"));

		////////////////////////////////////////////////////////////////////////////////
		// Scripting
		////////////////////////////////////////////////////////////////////////////////

		ComponentRegistry::Register<ScriptComponent>("Script")
			.Category("Scripting")
			.Description("C++ behaviours from the project's script module")
			.Extra(SerializeScriptComponent, DeserializeScriptComponent)
			.ExtraReferences(VisitScriptReferences, SetScriptReference, ScriptsEqual, VisitScriptReferencesJson);
	}

}
