// Hot reload test module, built twice: ST_TEST_RELOAD_VERSION 1 and 2 stand for the module before and after a code
// change. Version 2 changes a default, changes one field's type, removes a field, adds a field, removes a class and
// adds a class.

#include "StrataScript/StrataScript.h"

#include <cstdint>
#include <string>

#if !defined(ST_TEST_RELOAD_VERSION)
	#error "ST_TEST_RELOAD_VERSION must be 1 or 2"
#endif

using namespace Strata;

// The version is visible in behavior: version 1 counts in ones, version 2 in tens.
constexpr int32_t c_CountIncrement = ST_TEST_RELOAD_VERSION == 1 ? 1 : 10;

class Counter : public Script
{
public:
	int32_t Count = 0;
#if ST_TEST_RELOAD_VERSION == 1
	float Speed = 1.0f;
	std::string Label = "v1";
	int32_t ChangesType = 5;
	int32_t RemovedInV2 = 7;
#else
	float Speed = 2.0f;
	std::string Label = "v2";
	float ChangesType = 0.5f;
	int32_t AddedInV2 = 99;
#endif
	int32_t Creates = 0;
	int32_t Reloads = 0;
	Entity Target;

	void OnCreate() override
	{
		Creates++;
	}

	void OnUpdate(float) override
	{
		Count += c_CountIncrement;
	}

	void OnReload() override
	{
		Reloads++;
	}
};

ST_SCRIPT_CLASS(Counter)
{
	ST_SCRIPT_FIELD(Count);
	ST_SCRIPT_FIELD(Speed);
	ST_SCRIPT_FIELD(Label);
	ST_SCRIPT_FIELD(ChangesType);
#if ST_TEST_RELOAD_VERSION == 1
	ST_SCRIPT_FIELD(RemovedInV2);
#else
	ST_SCRIPT_FIELD(AddedInV2);
#endif
	ST_SCRIPT_FIELD(Creates);
	ST_SCRIPT_FIELD(Reloads);
	ST_SCRIPT_FIELD(Target);
}

#if ST_TEST_RELOAD_VERSION == 1

class OnlyInV1 : public Script
{
public:
	int32_t Value = 1;
};

ST_SCRIPT_CLASS(OnlyInV1)
{
	ST_SCRIPT_FIELD(Value);
}

#else

class OnlyInV2 : public Script
{
public:
	int32_t Value = 2;
	int32_t Creates = 0;

	void OnCreate() override
	{
		Creates++;
	}
};

ST_SCRIPT_CLASS(OnlyInV2)
{
	ST_SCRIPT_FIELD(Value);
	ST_SCRIPT_FIELD(Creates);
}

#endif
