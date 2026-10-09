// Script fields of every type, with values overridden in the scene file.

#include "FeatureScript.h"

#include <string>

using namespace Strata;
using namespace FeatureTest;

// Every field but Untouched is overridden in Scenes/Feature.stscene with the value OnCreate expects; the defaults
// below differ from those values, so a field that lost its override fails a check.
class FieldFeatures : public FeatureScript
{
public:
	bool Flag = false;
	int32_t Count = 1;
	float Speed = 1.0f;
	glm::vec2 Size = glm::vec2(1.0f);
	glm::vec3 Offset = glm::vec3(0.0f);
	glm::vec4 Tint = glm::vec4(1.0f);
	glm::quat Orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	std::string Label = "default";
	Entity Target;
	AssetHandle Texture;
	int32_t Untouched = 42; // Not overridden: keeps its default

	void OnCreate() override
	{
		Journal(*this, "FieldFeatures", "OnCreate");
		Expect(Flag, "bool field override");
		Expect(Count == -7, "int field override");
		Expect(Near(Speed, 2.5f), "float field override");
		Expect(Near(Size, glm::vec2(3.0f, 4.0f)), "vec2 field override");
		Expect(Near(Offset, glm::vec3(1.0f, -2.0f, 3.0f)), "vec3 field override");
		Expect(Near(Tint, glm::vec4(0.1f, 0.2f, 0.3f, 0.4f)), "vec4 field override");
		Expect(Near(Orientation, glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f))), "quat field override");
		Expect(Label == "overridden", "string field override");
		Expect(Target.IsValid() && Target.GetName() == "Leaf", "entity field override");
		Expect(Texture.IsValid() && Texture == Assets::Find("Textures/Checker.png"), "asset field override");
		Expect(Untouched == 42, "fields without an override keep their default");
	}

	void OnUpdate(float) override
	{
		// Fields changed by the script are what the engine reads back (the runner checks Count after play).
		Count++;
		if (Count == -7 + 5)
			Completed = true;
	}
};

ST_SCRIPT_CLASS(FieldFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Flag);
	ST_SCRIPT_FIELD(Count);
	ST_SCRIPT_FIELD(Speed);
	ST_SCRIPT_FIELD(Size);
	ST_SCRIPT_FIELD(Offset);
	ST_SCRIPT_FIELD(Tint);
	ST_SCRIPT_FIELD(Orientation);
	ST_SCRIPT_FIELD(Label);
	ST_SCRIPT_FIELD(Target);
	ST_SCRIPT_FIELD(Texture);
	ST_SCRIPT_FIELD(Untouched);
}
