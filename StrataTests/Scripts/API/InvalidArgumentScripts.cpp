// Calls host functions directly, with invalid buffers, pointers and strings: every call fails or degrades harmlessly.

#include "TestScripts.h"

#include <cstdint>
#include <limits>

using namespace Strata;
using namespace ScriptTests;

// Runs in OnCreate on an entity named "Tester" (six characters).
class InvalidArguments : public CheckingScript
{
public:
	void OnCreate() override
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptContext* context = Detail::GetContext();
		Entity self = GetEntity();
		const uint64_t id = self.GetID();
		Scene::CreateEntity("Child", self);
		self.SetTag("Probe");
		self.AddComponent("Text");
		self.SetProperty("Text", "Text", std::string("Hello"));
		const StrataScriptString textComponent = Detail::ToABIString("Text");

		// Buffers: without one (or without capacity) only the size is reported; at most the capacity is written.
		Expect(host->GetEntityName(context, id, nullptr, 64) == 6, "GetEntityName without a buffer reports the length");
		char buffer[4] = { '#', '#', '#', '#' };
		Expect(host->GetEntityName(context, id, buffer, 0) == 6 && buffer[0] == '#', "GetEntityName without capacity writes nothing");
		Expect(host->GetEntityName(context, id, buffer, 2) == 6 && buffer[0] == 'T' && buffer[1] == 'e' && buffer[2] == '#',
			"GetEntityName writes at most the capacity");
		Expect(host->GetEntityTag(context, id, nullptr, 16) == 5, "GetEntityTag without a buffer");
		Expect(host->GetChildren(context, id, nullptr, 16) == 1, "GetChildren without a buffer reports the count");
		Expect(host->FindEntitiesByTag(context, Detail::ToABIString("Probe"), nullptr, 16) == 1, "FindEntitiesByTag without a buffer");
		Expect(host->GetRootEntities(context, nullptr, 16) >= 1, "GetRootEntities without a buffer");
		uint64_t ids[1] = { 0 };
		Expect(host->GetChildren(context, id, ids, 0) == 1 && ids[0] == 0, "GetChildren without capacity writes nothing");

		// A string property read without a buffer reports the length and points nowhere.
		StrataScriptValue value = {};
		Expect(host->GetProperty(context, id, textComponent, Detail::ToABIString("Text"), &value, nullptr, 0)
			&& value.Type == StrataScriptValueType_String && value.As.String.Size == 5, "GetProperty of a string without a buffer");

		// Missing output or input structs are refused.
		Expect(!host->GetProperty(context, id, textComponent, Detail::ToABIString("Text"), nullptr, buffer, sizeof(buffer)), "GetProperty without output");
		Expect(!host->SetProperty(context, id, textComponent, Detail::ToABIString("Text"), nullptr), "SetProperty without a value");
		Expect(!host->GetTransform(context, id, nullptr), "GetTransform without output");
		Expect(!host->SetTransform(context, id, nullptr, StrataScriptTransformPart_All), "SetTransform without a transform");
		Expect(!host->GetWorldTransform(context, id, nullptr), "GetWorldTransform without output");
		Expect(!host->SetWorldTransform(context, id, nullptr, StrataScriptTransformPart_All), "SetWorldTransform without a transform");
		host->GetMousePosition(context, nullptr);
		host->GetMouseDelta(context, nullptr);
		host->GetScrollDelta(context, nullptr);

		// Strings: a null pointer with a size reads as empty; a size beyond the engine's limit is refused unread.
		const StrataScriptString nullWithSize { nullptr, 10 };
		const StrataScriptString impossibleSize { "Tester", std::numeric_limits<uint64_t>::max() };
		Expect(host->FindEntityByName(context, nullWithSize) == 0, "FindEntityByName with a null string");
		Expect(host->FindEntityByName(context, impossibleSize) == 0, "FindEntityByName with an impossible size");
		Expect(!host->HasComponent(context, id, nullWithSize), "HasComponent with a null string");
		Expect(!host->AddComponent(context, id, impossibleSize), "AddComponent with an impossible size");
		Expect(!host->HasScript(context, id, impossibleSize), "HasScript with an impossible size");
		StrataScriptValue invalidString = {};
		invalidString.Type = StrataScriptValueType_String;
		invalidString.As.String = nullWithSize;
		Expect(!host->SetProperty(context, id, textComponent, Detail::ToABIString("Text"), &invalidString), "SetProperty with an invalid string");
		invalidString.As.String = impossibleSize;
		Expect(!host->SetProperty(context, id, textComponent, Detail::ToABIString("Text"), &invalidString), "SetProperty with an impossible size");
		Expect(self.GetProperty<std::string>("Text", "Text").value_or("") == "Hello", "refused strings change nothing");
		host->Log(StrataScriptLogLevel_Trace, nullWithSize);
		host->Log(StrataScriptLogLevel_Trace, impossibleSize);

		// Values that do not fit: wrong types, out-of-range integers, non-finite transforms, unknown codes and entities.
		StrataScriptValue wrongType = {};
		wrongType.Type = StrataScriptValueType_Vec3;
		Expect(!host->SetProperty(context, id, textComponent, Detail::ToABIString("Text"), &wrongType), "SetProperty with a value of the wrong type");
		StrataScriptValue unknownType = {};
		unknownType.Type = 999;
		Expect(!host->SetProperty(context, id, textComponent, Detail::ToABIString("Alignment"), &unknownType), "SetProperty with an unknown value type");
		StrataScriptValue tooLarge = {};
		tooLarge.Type = StrataScriptValueType_Int;
		tooLarge.As.Int = std::numeric_limits<int64_t>::max();
		Expect(!host->SetProperty(context, id, textComponent, Detail::ToABIString("Alignment"), &tooLarge), "SetProperty with an out-of-range integer");
		StrataScriptTransform notFinite = { { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
		Expect(!host->SetTransform(context, id, &notFinite, StrataScriptTransformPart_Translation), "SetTransform with a non-finite translation");
		StrataScriptTransform zeroRotation = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f } };
		Expect(!host->SetTransform(context, id, &zeroRotation, StrataScriptTransformPart_Rotation), "SetTransform with a zero quaternion");
		Expect(host->Instantiate(context, 0, 0, &notFinite) == 0, "Instantiate of a null asset");
		Expect(!host->IsKeyDown(context, std::numeric_limits<uint32_t>::max()), "IsKeyDown with an unknown key");
		Expect(!host->IsMouseButtonDown(context, std::numeric_limits<uint32_t>::max()), "IsMouseButtonDown with an unknown button");
		Expect(host->GetEntityName(context, 0, buffer, sizeof(buffer)) == 0, "GetEntityName of the null entity");
		Expect(host->GetScriptInstance(context, id, nullWithSize) == nullptr, "GetScriptInstance with a null class name");
		Expect(self.IsValid(), "the entity survives");
	}
};

ST_SCRIPT_CLASS(InvalidArguments)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
}
