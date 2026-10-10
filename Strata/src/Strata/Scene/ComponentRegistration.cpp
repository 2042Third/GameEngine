#include "stpch.h"
#include "Strata/Scene/SceneRegistration.h"

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/Components.h"

namespace Strata
{

	namespace
	{

		constexpr float c_Unbounded = 1.0e6f;

		PropertyOptions Range(float min, float max, float speed = 0.0f)
		{
			PropertyOptions options;
			options.Min = min;
			options.Max = max;
			options.Speed = speed;
			return options;
		}

		PropertyOptions Slider(float min, float max)
		{
			PropertyOptions options = Range(min, max);
			options.Flags = PropertyFlags::Slider;
			return options;
		}

		PropertyOptions Color()
		{
			PropertyOptions options;
			options.Color = true;
			return options;
		}

		PropertyOptions WithTooltip(PropertyOptions options, std::string tooltip)
		{
			options.Tooltip = std::move(tooltip);
			return options;
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
				{ .DisplayName = "Prefab Entity ID", .Flags = PropertyFlags::ReadOnly });

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
			.Description("Image-based lighting and sky background from an HDR environment map")
			.AssetProperty("EnvironmentMap", &SkyLightComponent::EnvironmentMap, AssetType::Texture, { .Tooltip = "Equirectangular HDR texture" })
			.Property("Intensity", &SkyLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.01f))
			.Property("Rotation", &SkyLightComponent::Rotation, WithTooltip(Range(-360.0f, 360.0f, 0.5f), "Degrees around +Y"))
			.Property("ShowBackground", &SkyLightComponent::ShowBackground)
			.Property("BackgroundBlur", &SkyLightComponent::BackgroundBlur, Slider(0.0f, 1.0f))
			.Property("AmbientColor", &SkyLightComponent::AmbientColor, WithTooltip(Color(), "Ambient light when no environment map is set"));

		ComponentRegistry::Register<PostProcessComponent>("PostProcess")
			.DisplayName("Post Process")
			.Category("Rendering")
			.Description("Tonemapping, exposure, bloom, ambient occlusion and anti-aliasing settings")
			.EnumProperty("Tonemapper", &PostProcessComponent::Tonemapper, { { "None", 0 }, { "Reinhard", 1 }, { "ACES", 2 }, { "AgX", 3 }, { "KhronosNeutral", 4 } })
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
			.Property("Layer", &RigidBodyComponent::Layer, Range(0.0f, 31.0f));

		ComponentRegistry::Register<BoxColliderComponent>("BoxCollider")
			.DisplayName("Box Collider")
			.Category("Physics")
			.Property("HalfExtents", &BoxColliderComponent::HalfExtents, Range(0.0f, 0.0f, 0.01f))
			.Property("Offset", &BoxColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f));

		ComponentRegistry::Register<SphereColliderComponent>("SphereCollider")
			.DisplayName("Sphere Collider")
			.Category("Physics")
			.Property("Radius", &SphereColliderComponent::Radius, Range(0.001f, c_Unbounded, 0.01f))
			.Property("Offset", &SphereColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f));

		ComponentRegistry::Register<CapsuleColliderComponent>("CapsuleCollider")
			.DisplayName("Capsule Collider")
			.Category("Physics")
			.Property("Radius", &CapsuleColliderComponent::Radius, Range(0.001f, c_Unbounded, 0.01f))
			.Property("HalfHeight", &CapsuleColliderComponent::HalfHeight, Range(0.0f, c_Unbounded, 0.01f))
			.Property("Offset", &CapsuleColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f));

		ComponentRegistry::Register<MeshColliderComponent>("MeshCollider")
			.DisplayName("Mesh Collider")
			.Category("Physics")
			.AssetProperty("Mesh", &MeshColliderComponent::Mesh, AssetType::Mesh, { .Tooltip = "Uses the Mesh Renderer's mesh when unset" })
			.Property("Convex", &MeshColliderComponent::Convex, WithTooltip({}, "Non-convex meshes are only supported on static bodies"));

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
		// Scripting
		////////////////////////////////////////////////////////////////////////////////

		ComponentRegistry::Register<ScriptComponent>("Script")
			.Category("Scripting")
			.Description("C++ behaviours from the project's script module")
			.Extra(SerializeScriptComponent, DeserializeScriptComponent);
	}

}
