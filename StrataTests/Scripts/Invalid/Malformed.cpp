// A hand-written script module (no SDK) that describes two classes through the C ABI directly. The environment
// variable STRATA_TEST_MALFORMED_CASE, read when the module loads, selects a defect for the validation tests; without it
// the module is valid.

#include "StrataScript/ScriptABI.h"

#include <cstdlib>
#include <cstring>

namespace
{

	int s_InstanceStorage = 0;
	// "ChangesDescriptors": the first Create clears the functions of every class descriptor, as if the module had
	// overwritten its memory. The engine must keep calling the functions it read while loading.
	bool s_ChangeDescriptorsInCreate = false;

	void ClearDescriptorFunctions();

	uint32_t Create(StrataScriptContext*, StrataScriptEntityID, StrataScriptInstance* outInstance)
	{
		if (s_ChangeDescriptorsInCreate)
			ClearDescriptorFunctions();
		*outInstance = &s_InstanceStorage;
		return StrataScriptResult_Ok;
	}

	uint32_t Destroy(StrataScriptInstance)
	{
		return StrataScriptResult_Ok;
	}

	uint32_t GetField(StrataScriptInstance, uint32_t, StrataScriptValue* outValue)
	{
		*outValue = StrataScriptValue {};
		outValue->Type = StrataScriptValueType_Int;
		return StrataScriptResult_Ok;
	}

	uint32_t SetField(StrataScriptInstance, uint32_t, const StrataScriptValue*)
	{
		return StrataScriptResult_Ok;
	}

	StrataScriptString Text(const char* text)
	{
		return StrataScriptString { text, static_cast<uint64_t>(std::strlen(text)) };
	}

	bool Is(const char* testCase, const char* name)
	{
		return testCase && std::strcmp(testCase, name) == 0;
	}

	StrataScriptFieldDesc s_Fields[2];
	const StrataScriptFieldDesc* s_FieldPointers[2];
	const StrataScriptFieldDesc* s_DuplicateFieldPointers[2];
	StrataScriptClassDesc s_Classes[2];
	const StrataScriptClassDesc* s_ClassPointers[2];

	void ClearDescriptorFunctions()
	{
		for (StrataScriptClassDesc& descriptor : s_Classes)
		{
			descriptor.Create = nullptr;
			descriptor.Destroy = nullptr;
			descriptor.GetField = nullptr;
			descriptor.SetField = nullptr;
		}
	}

}

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_GetABIVersion(void)
{
	return ST_SCRIPT_ABI_VERSION;
}

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_Load(const StrataScriptHostAPI* host, uint32_t, StrataScriptModuleAPI* outModule)
{
	// The engine's struct has room for this module's description (StructSize is its size; see StrataScriptModuleAPI).
	if (outModule->StructSize < sizeof(StrataScriptModuleAPI))
		return StrataScriptResult_ABIMismatch;

	const char* testCase = std::getenv("STRATA_TEST_MALFORMED_CASE");
	if (Is(testCase, "Exception"))
	{
		host->ReportException(Text("Broken on purpose"));
		return StrataScriptResult_Exception;
	}
	if (Is(testCase, "Rejected"))
		return StrataScriptResult_InvalidArgument;

	// Two valid classes, "First" and "Second", with one Int field each.
	for (int index = 0; index < 2; index++)
	{
		StrataScriptFieldDesc& field = s_Fields[index];
		field = StrataScriptFieldDesc {};
		field.StructSize = sizeof(StrataScriptFieldDesc);
		field.Type = StrataScriptValueType_Int;
		field.Name = Text(index == 0 ? "A" : "B");
		field.DefaultValue.Type = StrataScriptValueType_Int;
		field.DefaultValue.As.Int = index;
		s_FieldPointers[index] = &field;

		StrataScriptClassDesc& descriptor = s_Classes[index];
		descriptor = StrataScriptClassDesc {};
		descriptor.StructSize = sizeof(StrataScriptClassDesc);
		descriptor.FieldCount = 1;
		descriptor.Name = Text(index == 0 ? "First" : "Second");
		descriptor.Fields = &s_FieldPointers[index];
		descriptor.Create = &Create;
		descriptor.Destroy = &Destroy;
		descriptor.GetField = &GetField;
		descriptor.SetField = &SetField;
		s_ClassPointers[index] = &descriptor;
	}

	*outModule = StrataScriptModuleAPI {};
	outModule->StructSize = sizeof(StrataScriptModuleAPI);
	outModule->ABIVersion = ST_SCRIPT_ABI_VERSION;
	outModule->Name = Text("Malformed");
	outModule->ClassCount = 2;
	outModule->Classes = s_ClassPointers;

	if (Is(testCase, "DuplicateClass"))
		s_Classes[1].Name = Text("First");
	else if (Is(testCase, "EmptyClassName"))
		s_Classes[0].Name = Text("");
	else if (Is(testCase, "MissingCreate"))
		s_Classes[1].Create = nullptr;
	else if (Is(testCase, "NullClass"))
		s_ClassPointers[1] = nullptr;
	else if (Is(testCase, "SmallClass"))
		s_Classes[0].StructSize = 8;
	else if (Is(testCase, "BadFieldType"))
		s_Fields[0].Type = 999;
	else if (Is(testCase, "BadDefault"))
		s_Fields[0].DefaultValue.Type = StrataScriptValueType_Float;
	else if (Is(testCase, "DuplicateField"))
	{
		s_DuplicateFieldPointers[0] = &s_Fields[0];
		s_DuplicateFieldPointers[1] = &s_Fields[0];
		s_Classes[0].FieldCount = 2;
		s_Classes[0].Fields = s_DuplicateFieldPointers;
	}
	else if (Is(testCase, "SmallModule"))
		outModule->StructSize = 8;
	else if (Is(testCase, "TooManyClasses"))
		outModule->ClassCount = 1u << 20;
	else if (Is(testCase, "NullClassList"))
		outModule->Classes = nullptr;
	s_ChangeDescriptorsInCreate = Is(testCase, "ChangesDescriptors");
	return StrataScriptResult_Ok;
}
