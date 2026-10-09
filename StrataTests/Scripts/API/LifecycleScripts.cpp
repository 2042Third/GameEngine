// Lifecycle, ordering, field and exception test scripts.

#include "TestScripts.h"

#include <stdexcept>

using namespace Strata;
using namespace ScriptTests;

ST_SCRIPT_CLASS(Lifecycle)
{
	ST_SCRIPT_FIELD(Creates);
	ST_SCRIPT_FIELD(Updates);
	ST_SCRIPT_FIELD(FixedUpdates);
	ST_SCRIPT_FIELD(LateUpdates);
	ST_SCRIPT_FIELD(RecordUpdates);
}

// A second class to verify the order of several scripts on one entity.
class LifecycleSecond : public Lifecycle
{
protected:
	const char* GetClassName() const override { return "LifecycleSecond"; }
};

ST_SCRIPT_CLASS(LifecycleSecond)
{
	ST_SCRIPT_FIELD(Creates);
	ST_SCRIPT_FIELD(Updates);
}

// Implements no callbacks at all: the engine never calls it.
class Idle : public Script
{
};

ST_SCRIPT_CLASS(Idle) {}

// Overrides callbacks privately and protectedly; they run like public ones.
class HiddenCallbacks : public Script
{
public:
	int32_t Creates = 0;
	int32_t Updates = 0;
protected:
	void OnCreate() override { Creates++; }
private:
	void OnUpdate(float) override { Updates++; }
};

ST_SCRIPT_CLASS(HiddenCallbacks)
{
	ST_SCRIPT_FIELD(Creates);
	ST_SCRIPT_FIELD(Updates);
}

// One field of every supported type, with distinctive defaults.
class FieldTypes : public Script
{
public:
	bool BoolField = true;
	int32_t IntField = 42;
	float FloatField = 1.5f;
	glm::vec2 Vec2Field = { 1.0f, 2.0f };
	glm::vec3 Vec3Field = { 1.0f, 2.0f, 3.0f };
	glm::vec4 Vec4Field = { 1.0f, 2.0f, 3.0f, 4.0f };
	glm::quat QuatField = glm::quat(0.70710678f, 0.0f, 0.70710678f, 0.0f);
	std::string StringField = "Hello";
	Entity EntityField;
	AssetHandle AssetField = AssetHandle(0x1234);

	// What OnCreate saw: field overrides are applied before it runs.
	int32_t IntSeenInCreate = 0;
	std::string StringSeenInCreate;

	void OnCreate() override
	{
		IntSeenInCreate = IntField;
		StringSeenInCreate = StringField;
	}
};

ST_SCRIPT_CLASS(FieldTypes)
{
	ST_SCRIPT_FIELD(BoolField);
	ST_SCRIPT_FIELD(IntField);
	ST_SCRIPT_FIELD(FloatField);
	ST_SCRIPT_FIELD(Vec2Field);
	ST_SCRIPT_FIELD(Vec3Field);
	ST_SCRIPT_FIELD(Vec4Field);
	ST_SCRIPT_FIELD(QuatField);
	ST_SCRIPT_FIELD(StringField);
	ST_SCRIPT_FIELD(EntityField);
	ST_SCRIPT_FIELD(AssetField);
	ST_SCRIPT_FIELD(IntSeenInCreate);
	ST_SCRIPT_FIELD(StringSeenInCreate);
}

// Throws from the callback named by ThrowIn ("OnCreate", "OnUpdate", "OnDestroy" or "NonStandard" for a non-std
// exception in OnUpdate).
class Thrower : public Script
{
public:
	std::string ThrowIn;
	int32_t Updates = 0;

	void OnCreate() override
	{
		if (ThrowIn == "OnCreate")
			throw std::runtime_error("OnCreate failed on purpose");
	}

	void OnUpdate(float) override
	{
		Updates++;
		if (ThrowIn == "OnUpdate")
			throw std::runtime_error("OnUpdate failed on purpose");
		if (ThrowIn == "NonStandard")
			throw 42;
	}

	void OnDestroy() override
	{
		Record(*this, "Thrower", "Destroy");
		if (ThrowIn == "OnDestroy")
			throw std::logic_error("OnDestroy failed on purpose");
	}
};

ST_SCRIPT_CLASS(Thrower)
{
	ST_SCRIPT_FIELD(ThrowIn);
	ST_SCRIPT_FIELD(Updates);
}

// Its constructor throws for real instances (the module also constructs one without an entity to read defaults).
class ThrowingConstructor : public Script
{
public:
	ThrowingConstructor()
	{
		if (GetEntity().GetID() != 0)
			throw std::runtime_error("Constructor failed on purpose");
	}
};

ST_SCRIPT_CLASS(ThrowingConstructor) {}
