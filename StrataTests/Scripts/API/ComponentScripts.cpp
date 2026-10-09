// Generic component access test scripts.

#include "TestScripts.h"

#include <limits>
#include <string>

using namespace Strata;
using namespace ScriptTests;

// Verifies generic component access (by component and property name) for every value type in OnCreate.
class ComponentAPI : public CheckingScript
{
public:
	void OnCreate() override
	{
		Entity self = GetEntity();
		Expect(self.HasComponent("Transform") && self.HasComponent("transform"), "HasComponent (case-insensitive)");
		Expect(!self.HasComponent("Camera"), "HasComponent of a missing component");
		Expect(!self.HasComponent("NoSuchComponent"), "HasComponent of an unknown component");
		Expect(self.AddComponent("Camera") && self.HasComponent("Camera"), "AddComponent");
		Expect(self.AddComponent("Camera"), "adding a present component is a no-op");
		Expect(!self.AddComponent("NoSuchComponent"), "AddComponent of an unknown component");
		Expect(!self.AddComponent("ID") && !self.AddComponent("Inactive"), "internal components cannot be added");
		Expect(!self.AddComponent("PrefabInstance") && !self.HasComponent("PrefabInstance"), "engine-added components cannot be added");

		// Float, with range clamping and integer conversion.
		Expect(self.SetProperty("Camera", "PerspectiveFOV", 75.0f), "SetProperty float");
		Expect(Near(self.GetProperty<float>("Camera", "PerspectiveFOV").value_or(0.0f), 75.0f), "GetProperty float");
		Expect(self.SetProperty("Camera", "PerspectiveFOV", 500.0f), "SetProperty clamps");
		Expect(Near(self.GetProperty<float>("Camera", "PerspectiveFOV").value_or(0.0f), 179.0f), "clamped value");
		Expect(self.SetProperty("Camera", "PerspectiveFOV", 60), "SetProperty float from an integer");
		Expect(Near(self.GetProperty<float>("Camera", "PerspectiveFOV").value_or(0.0f), 60.0f), "integer converted to float");
		Expect(!self.SetProperty("Camera", "PerspectiveFOV", std::numeric_limits<float>::infinity()), "non-finite floats are rejected");

		// Enum as integer.
		Expect(self.SetProperty("Camera", "Projection", 1), "SetProperty enum");
		Expect(self.GetProperty<int32_t>("Camera", "Projection").value_or(-1) == 1, "GetProperty enum");
		Expect(!self.SetProperty("Camera", "Projection", 7), "invalid enum values are rejected");

		// Bool and Color4.
		Expect(self.SetProperty("Camera", "Primary", false) && self.GetProperty<bool>("Camera", "Primary") == false, "bool property");
		const glm::vec4 clearColor(0.1f, 0.2f, 0.3f, 1.0f);
		Expect(self.SetProperty("Camera", "ClearColor", clearColor) && Near(self.GetProperty<glm::vec4>("Camera", "ClearColor").value_or(glm::vec4(0.0f)), clearColor),
			"color4 property");

		// Strings (including one longer than the SDK's stack buffer), Vec2 and enums of another component.
		Expect(self.AddComponent("Text"), "AddComponent Text");
		Expect(self.SetProperty("Text", "Text", std::string("Score: 10")), "SetProperty string");
		Expect(self.GetProperty<std::string>("Text", "Text").value_or("") == "Score: 10", "GetProperty string");
		const std::string longText(1000, 'x');
		Expect(self.SetProperty("Text", "Text", longText) && self.GetProperty<std::string>("Text", "Text").value_or("") == longText, "long string property");
		Expect(self.SetProperty("Text", "Text", std::string("Score: 10")), "restore string");
		Expect(self.SetProperty("Text", "ScreenAnchor", glm::vec2(0.25f, 0.75f)) && Near(self.GetProperty<glm::vec2>("Text", "ScreenAnchor").value_or(glm::vec2(0.0f)),
			glm::vec2(0.25f, 0.75f)), "vec2 property");
		Expect(self.SetProperty("Text", "Alignment", 2) && self.GetProperty<int32_t>("Text", "Alignment") == 2, "enum property");

		// Component proxy.
		Component text = self.GetComponent("Text");
		Expect(text.Exists() && text.Set("FontSize", 48.0f) && Near(text.Get<float>("FontSize", 0.0f), 48.0f), "Component proxy");

		// Vec3 and Color3.
		Expect(self.AddComponent("BoxCollider") && self.SetProperty("BoxCollider", "HalfExtents", glm::vec3(1.0f, 2.0f, 3.0f)), "vec3 property");
		Expect(Near(self.GetProperty<glm::vec3>("BoxCollider", "HalfExtents").value_or(glm::vec3(0.0f)), glm::vec3(1.0f, 2.0f, 3.0f)), "GetProperty vec3");
		Expect(self.AddComponent("PointLight") && self.SetProperty("PointLight", "Color", glm::vec3(1.0f, 0.5f, 0.25f)), "color3 property");
		Expect(Near(self.GetProperty<glm::vec3>("PointLight", "Color").value_or(glm::vec3(0.0f)), glm::vec3(1.0f, 0.5f, 0.25f)), "GetProperty color3");

		// UInt.
		Expect(self.AddComponent("RigidBody") && self.SetProperty("RigidBody", "Layer", 5u), "uint property");
		Expect(self.GetProperty<uint32_t>("RigidBody", "Layer").value_or(0) == 5u && self.GetProperty<int32_t>("RigidBody", "Layer").value_or(0) == 5, "GetProperty uint");
		Expect(!self.SetProperty("RigidBody", "Layer", -1), "negative values are rejected for unsigned properties");

		// Quaternion (also accepted as vec4).
		const glm::quat rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Expect(self.SetProperty("Transform", "Rotation", rotation) && Near(self.GetProperty<glm::quat>("Transform", "Rotation").value_or(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), rotation),
			"quat property");
		Expect(self.SetProperty("Transform", "Rotation", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)), "quat property from vec4");

		// Asset and entity references.
		Expect(self.AddComponent("MeshRenderer") && self.SetProperty("MeshRenderer", "Mesh", AssetHandle(0x42)), "asset property");
		Expect(self.GetProperty<AssetHandle>("MeshRenderer", "Mesh").value_or(AssetHandle()) == AssetHandle(0x42), "GetProperty asset");
		Expect(self.GetProperty<Entity>("Relationship", "Parent").has_value(), "entity property");
		Expect(!self.SetProperty("Relationship", "Parent", self), "read-only properties are rejected");
		Expect(self.GetProperty<Entity>("ID", "ID").value_or(Entity()) == self, "the ID property is the entity");

		// Mismatches and missing pieces.
		Expect(!self.SetProperty("Camera", "PerspectiveFOV", std::string("wide")), "wrong value types are rejected");
		Expect(!self.GetProperty<std::string>("Camera", "PerspectiveFOV").has_value(), "GetProperty of the wrong type");
		Expect(!self.GetProperty<float>("Camera", "NoSuchProperty").has_value(), "unknown property");
		Expect(!self.GetProperty<float>("AudioSource", "Volume").has_value(), "property of a missing component");
		Expect(!self.SetProperty("NoSuchComponent", "Value", 1.0f), "SetProperty of an unknown component");

		// Removal.
		Expect(self.RemoveComponent("Camera") && !self.HasComponent("Camera"), "RemoveComponent");
		Expect(!self.RemoveComponent("Camera"), "removing a missing component fails");
		Expect(!self.RemoveComponent("Transform") && self.HasComponent("Transform"), "required components cannot be removed");
	}
};

ST_SCRIPT_CLASS(ComponentAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
}

// Reads the property named by the fields with whichever value type works and writes the value back. The tests run it
// over every property of every registered component.
class PropertyProbe : public Script
{
public:
	std::string Component;
	std::string Property;
	std::string ReadType; // The value type the read succeeded with (empty if none)
	bool WroteBack = false;
	int32_t Probes = 0;

	void OnUpdate(float) override
	{
		if (Component.empty())
			return;

		GetEntity().AddComponent(Component);
		ReadType.clear();
		WroteBack = false;
		const bool read = Probe<bool>("Bool") || Probe<int64_t>("Int") || Probe<float>("Float") || Probe<glm::vec2>("Vec2") || Probe<glm::vec3>("Vec3")
			|| Probe<glm::vec4>("Vec4") || Probe<glm::quat>("Quat") || Probe<std::string>("String") || Probe<Entity>("Entity") || Probe<AssetHandle>("Asset");
		(void)read;
		Component.clear();
		Probes++;
	}
private:
	template<typename T>
	bool Probe(const char* typeName)
	{
		Entity self = GetEntity();
		const std::optional<T> value = self.GetProperty<T>(Component, Property);
		if (!value)
			return false;
		ReadType = typeName;
		WroteBack = self.SetProperty(Component, Property, *value);
		return true;
	}
};

ST_SCRIPT_CLASS(PropertyProbe)
{
	ST_SCRIPT_FIELD(Component);
	ST_SCRIPT_FIELD(Property);
	ST_SCRIPT_FIELD(ReadType);
	ST_SCRIPT_FIELD(WroteBack);
	ST_SCRIPT_FIELD(Probes);
}
