// Components: generic access by component and property name for every value type, on the authored scene (whose
// values come from the scene file) and on an entity created at runtime.

#include "FeatureScript.h"

#include <cstdint>
#include <optional>
#include <string>

using namespace Strata;
using namespace FeatureTest;

class ComponentFeatures : public FeatureScript
{
public:
	Entity Scratch;
	int32_t HudUpdates = 0;

	void OnCreate() override
	{
		Journal(*this, "ComponentFeatures", "OnCreate");
		ReadAuthoredValues();
		WriteEveryValueType();
		UseComponentProxy();
	}

	void OnLateUpdate(float) override
	{
		// The HUD shows the frame; written every frame, read back the next one.
		Entity hud = Scene::FindEntityByName("HUD");
		const std::string expected = "Strata Feature Test " + std::to_string(HudUpdates);
		if (HudUpdates > 0)
			Expect(hud.GetProperty<std::string>("Text", "Text") == expected, "the HUD keeps the text written last frame");
		HudUpdates++;
		Expect(hud.SetProperty("Text", "Text", "Strata Feature Test " + std::to_string(HudUpdates)), "SetProperty on the HUD text");
		if (HudUpdates == 3)
			Completed = true;
	}
private:
	// Values written in Scenes/Feature.stscene.
	void ReadAuthoredValues()
	{
		Entity mainCamera = Scene::FindEntityByName("Main Camera");
		Expect(Scene::GetPrimaryCamera() == mainCamera, "Scene::GetPrimaryCamera");
		Expect(mainCamera.HasComponent("Camera") && mainCamera.HasComponent("camera"), "HasComponent is case-insensitive");
		Expect(Near(mainCamera.GetProperty<float>("Camera", "PerspectiveFOV").value_or(0.0f), 55.0f), "float property");
		Expect(mainCamera.GetProperty<bool>("Camera", "Primary") == true, "bool property");
		Expect(Near(mainCamera.GetProperty<glm::vec4>("Camera", "ClearColor").value_or(glm::vec4(0.0f)), glm::vec4(0.08f, 0.1f, 0.16f, 1.0f)), "color4 property");

		Entity overview = Scene::FindEntityByName("Overview Camera");
		Expect(overview.GetProperty<int32_t>("Camera", "Projection") == 1, "enum property (Orthographic)");
		Expect(overview.GetProperty<bool>("Camera", "Primary") == false, "a secondary camera");
		Expect(overview.GetProperty<bool>("AudioListener", "Active") == false, "an inactive audio listener");

		Entity lamp = Scene::FindEntityByName("Lamp");
		Expect(Near(lamp.GetProperty<glm::vec3>("PointLight", "Color").value_or(glm::vec3(0.0f)), glm::vec3(1.0f, 0.6f, 0.3f)), "color3 property");

		Entity hud = Scene::FindEntityByName("HUD");
		Expect(hud.GetProperty<std::string>("Text", "Text") == "Strata Feature Test", "string property");
		Expect(Near(hud.GetProperty<glm::vec2>("Text", "ScreenAnchor").value_or(glm::vec2(0.0f)), glm::vec2(0.03f, 0.05f)), "vec2 property");
		Expect(hud.GetProperty<AssetHandle>("Text", "Font") == Assets::Find("Fonts/FeatureBlocks.ttf"), "asset property (font)");

		Entity trigger = Scene::FindEntityByName("Trigger Zone");
		Expect(trigger.GetProperty<uint32_t>("RigidBody", "Layer") == 3u, "uint property");
		Expect(trigger.GetProperty<int64_t>("RigidBody", "Layer") == 3, "uint property read as int64");
		Expect(trigger.GetProperty<bool>("RigidBody", "IsTrigger") == true, "trigger body");
		Expect(Near(trigger.GetProperty<glm::vec3>("BoxCollider", "Offset").value_or(glm::vec3(0.0f)), glm::vec3(0.0f, 1.0f, 0.0f)), "vec3 property");

		Entity ground = Scene::FindEntityByName("Ground");
		Expect(ground.GetProperty<AssetHandle>("MeshRenderer", "Material") == Assets::Find("Materials/Feature.stmat"), "asset property (material)");
		Expect(ground.GetProperty<AssetHandle>("MeshRenderer", "Mesh") == AssetHandle(0x01), "built-in mesh handle");
		Expect(ground.GetProperty<int32_t>("RigidBody", "Type") == 0, "enum property (Static)");

		Entity transformParent = Scene::FindEntityByName("Transform Parent");
		const glm::quat quarterTurn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Expect(Near(transformParent.GetProperty<glm::quat>("Transform", "Rotation").value_or(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), quarterTurn), "quat property");

		Entity leaf = Scene::FindEntityByName("Leaf");
		Expect(leaf.GetProperty<Entity>("Relationship", "Parent") == Scene::FindEntityByName("Branch A"), "entity property");
		Expect(leaf.GetProperty<Entity>("ID", "ID") == leaf, "the ID property is the entity");

		Entity crate = Scene::FindEntityByName("Crate");
		Expect(crate.GetProperty<AssetHandle>("PrefabInstance", "Prefab") == Assets::Find("Prefabs/Crate.stprefab"), "prefab instance link");
		Expect(crate.GetProperty<std::string>("PrefabInstance", "PrefabEntityID") == "F7C0000000000001", "prefab entity id");

		Entity speaker = Scene::FindEntityByName("Speaker");
		Expect(speaker.GetProperty<AssetHandle>("AudioSource", "Clip") == Assets::Find("Audio/Blip.wav"), "asset property (audio clip)");
		Expect(Near(speaker.GetProperty<float>("AudioSource", "Pitch").value_or(0.0f), 1.5f), "audio source pitch");

		Entity environment = Scene::FindEntityByName("Environment");
		Expect(environment.GetProperty<AssetHandle>("SkyLight", "EnvironmentMap") == Assets::Find("Textures/Sky.hdr"), "asset property (environment map)");
		Expect(environment.GetProperty<int32_t>("PostProcess", "Tonemapper") == 3, "enum property (AgX)");
	}

	void WriteEveryValueType()
	{
		Scratch = Scene::CreateEntity("Component Scratch");
		Expect(!Scratch.HasComponent("Camera") && Scratch.AddComponent("Camera") && Scratch.HasComponent("Camera"), "AddComponent");
		Expect(Scratch.AddComponent("Camera"), "adding a present component succeeds");
		Expect(Scratch.SetProperty("Camera", "Primary", false), "SetProperty keeps the scratch camera secondary");

		Expect(Scratch.SetProperty("Camera", "PerspectiveFOV", 70.0f) && Near(Scratch.GetProperty<float>("Camera", "PerspectiveFOV").value_or(0.0f), 70.0f), "write float");
		Expect(Scratch.SetProperty("Camera", "PerspectiveFOV", 500.0f) && Near(Scratch.GetProperty<float>("Camera", "PerspectiveFOV").value_or(0.0f), 179.0f), "ranges are enforced");
		Expect(Scratch.SetProperty("Camera", "Projection", 1) && Scratch.GetProperty<int32_t>("Camera", "Projection") == 1, "write enum");
		Expect(Scratch.SetProperty("Camera", "ClearColor", glm::vec4(0.5f, 0.25f, 0.125f, 1.0f))
			&& Near(Scratch.GetProperty<glm::vec4>("Camera", "ClearColor").value_or(glm::vec4(0.0f)), glm::vec4(0.5f, 0.25f, 0.125f, 1.0f)), "write color4");

		Expect(Scratch.AddComponent("Text"), "AddComponent Text");
		const std::string longText(400, 'x'); // Longer than the SDK's stack buffer
		Expect(Scratch.SetProperty("Text", "Text", longText) && Scratch.GetProperty<std::string>("Text", "Text") == longText, "write a long string");
		Expect(Scratch.SetProperty("Text", "ScreenOffset", glm::vec2(3.0f, -4.0f))
			&& Near(Scratch.GetProperty<glm::vec2>("Text", "ScreenOffset").value_or(glm::vec2(0.0f)), glm::vec2(3.0f, -4.0f)), "write vec2");

		Expect(Scratch.AddComponent("PointLight") && Scratch.SetProperty("PointLight", "Color", glm::vec3(0.25f, 0.5f, 1.0f))
			&& Near(Scratch.GetProperty<glm::vec3>("PointLight", "Color").value_or(glm::vec3(0.0f)), glm::vec3(0.25f, 0.5f, 1.0f)), "write color3");

		Expect(Scratch.AddComponent("RigidBody") && Scratch.SetProperty("RigidBody", "Type", 2), "a kinematic scratch body");
		Expect(Scratch.SetProperty("RigidBody", "Layer", 7u) && Scratch.GetProperty<uint32_t>("RigidBody", "Layer") == 7u, "write uint");
		Expect(Scratch.SetProperty("RigidBody", "Layer", int64_t(9)) && Scratch.GetProperty<int32_t>("RigidBody", "Layer") == 9, "write int64");
		Expect(Scratch.SetProperty("RigidBody", "ContinuousCollision", true) && Scratch.GetProperty<bool>("RigidBody", "ContinuousCollision") == true, "write bool");

		Expect(Scratch.AddComponent("BoxCollider") && Scratch.SetProperty("BoxCollider", "HalfExtents", glm::vec3(0.1f, 0.2f, 0.3f))
			&& Near(Scratch.GetProperty<glm::vec3>("BoxCollider", "HalfExtents").value_or(glm::vec3(0.0f)), glm::vec3(0.1f, 0.2f, 0.3f)), "write vec3");

		const glm::quat rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		Expect(Scratch.SetProperty("Transform", "Rotation", rotation)
			&& Near(Scratch.GetProperty<glm::quat>("Transform", "Rotation").value_or(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), rotation), "write quat");

		const AssetHandle cube(0x01);
		Expect(Scratch.AddComponent("MeshRenderer") && Scratch.SetProperty("MeshRenderer", "Mesh", cube) && Scratch.GetProperty<AssetHandle>("MeshRenderer", "Mesh") == cube,
			"write asset");
		Expect(Scratch.SetProperty("MeshRenderer", "Mesh", AssetHandle()) && !Scratch.GetProperty<AssetHandle>("MeshRenderer", "Mesh").value_or(cube).IsValid(), "clear an asset");

		// The SDK refuses to convert a value to another type (the host call itself succeeds, so nothing is reported).
		Expect(!Scratch.GetProperty<std::string>("Camera", "PerspectiveFOV").has_value(), "reading with the wrong value type fails");

		Expect(Scratch.RemoveComponent("PointLight") && !Scratch.HasComponent("PointLight"), "RemoveComponent");
		Expect(Scratch.RemoveComponent("RigidBody") && !Scratch.HasComponent("RigidBody"), "RemoveComponent of a physics component");
	}

	void UseComponentProxy()
	{
		Component text = Scratch.GetComponent("Text");
		Expect(text.Exists() && text.GetEntity() == Scratch && text.GetName() == "Text", "Component proxy");
		Expect(text.Set("FontSize", 48.0f) && Near(text.Get<float>("FontSize", 0.0f), 48.0f), "Component::Set and Get with a fallback");
		Expect(text.Set("Alignment", 2) && text.Get<int32_t>("Alignment") == 2, "Component::Get as optional");

		Component missing = Scratch.GetComponent("SpotLight");
		Expect(!missing.Exists(), "the proxy of a missing component does not exist");
	}
};

ST_SCRIPT_CLASS(ComponentFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Scratch);
	ST_SCRIPT_FIELD(HudUpdates);
}
