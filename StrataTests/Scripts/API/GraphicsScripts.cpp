// Scripts of the Scripting.Graphics tests (StrataTests/src/Scripting/ScriptGraphicsTests.cpp): the players' graphics options
// through the SDK's Graphics and, for the info struct's size protocol, the raw host function.

#include "TestScripts.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace ScriptTests;

namespace
{

	std::string Join(const std::vector<std::string>& texts)
	{
		std::string joined;
		for (const std::string& text : texts)
			joined += (joined.empty() ? "" : "\n") + text;
		return joined;
	}

	// A module struct of a later SDK: the first version's members and 16 bytes the engine does not know.
	struct LargerOptionInfo
	{
		StrataScriptGraphicsOptionInfo Info;
		uint8_t Later[16];
	};

}

// Does what the test asks through Request (then cleared), then reads Option every update. Requests: 1 lists the options
// (Count, Names, PresetChoices), 2 sets Option to the choice Value, 3 to the Int IntValue, 4 to the Float FloatValue, 5
// resets Option (empty: every option), 6 saves, 7 describes option 0 through the raw host function with a struct 16 bytes
// larger than the first version's (InfoStructSize: the size the engine says it filled) and one of 40 bytes
// (SmallInfoRefused), 8 sets Option to the Bool IntValue != 0, 9 sets the preset IntValue (QualityPreset). Accepted is the
// result of a set or SetPreset. Effective is Option's value in effect as text, Source its source (-1: none), Preset the
// preset in effect (-1: none).
class GraphicsProbe : public CheckingScript
{
public:
	int32_t Request = 0;
	std::string Option = "Preset";
	std::string Value;
	int32_t IntValue = 0;
	float FloatValue = 0.0f;

	int32_t Count = -1;
	std::string Names;
	std::string PresetChoices;
	std::string Effective;
	int32_t Source = -1;
	int32_t Preset = -1;
	bool Accepted = false;
	bool Saved = false;
	bool ResetDone = false;
	int32_t InfoStructSize = 0;
	bool SmallInfoRefused = false;

	void OnUpdate(float) override
	{
		switch (Request)
		{
			case 1: List(); break;
			case 2: Accepted = Graphics::Set(Option, std::string_view(Value)); break;
			case 3: Accepted = Graphics::Set<int32_t>(Option, IntValue); break;
			case 4: Accepted = Graphics::Set<float>(Option, FloatValue); break;
			case 5: ResetDone = Graphics::Reset(Option); break;
			case 6: Saved = Graphics::Save(); break;
			case 7: DescribeRaw(); break;
			case 8: Accepted = Graphics::Set<bool>(Option, IntValue != 0); break;
			case 9: Accepted = Graphics::SetPreset(static_cast<QualityPreset>(static_cast<uint32_t>(IntValue))); break;
			default: break;
		}
		Request = 0;
		Read();
	}
private:
	void List()
	{
		const std::vector<GraphicsOption> options = Graphics::Options();
		Count = static_cast<int32_t>(options.size());
		std::vector<std::string> names;
		for (const GraphicsOption& option : options)
			names.push_back(option.Name);
		Names = Join(names);
		const std::optional<GraphicsOption> preset = Graphics::FindOption("preset");
		PresetChoices = preset ? Join(preset->Choices) : std::string();
		Expect(!preset || (preset->Type == GraphicsOptionType::Choice && !preset->Title.empty() && !preset->Description.empty()), "Preset is described");
		// The SDK's preset names are the Preset option's choices, in order.
		for (uint32_t index = 0; preset && index < preset->Choices.size(); index++)
			Expect(Graphics::GetPresetName(static_cast<QualityPreset>(index)) == preset->Choices[index], "QualityPreset follows Preset's choices");
	}

	void DescribeRaw()
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		LargerOptionInfo larger;
		std::memset(&larger, 0xAB, sizeof(larger));
		larger.Info.StructSize = static_cast<uint32_t>(sizeof(larger));
		InfoStructSize = host->GraphicsDescribeOption(Detail::GetContext(), 0, &larger.Info) ? static_cast<int32_t>(larger.Info.StructSize) : -1;
		// The bytes past the engine's struct are the module's: left alone.
		Expect(larger.Later[0] == 0xAB && larger.Later[15] == 0xAB, "the engine writes no byte past its own struct");
		StrataScriptGraphicsOptionInfo small = {};
		small.StructSize = 40;
		SmallInfoRefused = !host->GraphicsDescribeOption(Detail::GetContext(), 0, &small);
	}

	void Read()
	{
		Effective.clear();
		if (const std::optional<std::string> choice = Graphics::Get<std::string>(Option))
			Effective = *choice;
		else if (const std::optional<int64_t> integer = Graphics::Get<int64_t>(Option))
			Effective = std::to_string(*integer);
		else if (const std::optional<float> number = Graphics::Get<float>(Option))
			Effective = std::to_string(*number);
		else if (const std::optional<bool> flag = Graphics::Get<bool>(Option))
			Effective = *flag ? "true" : "false";
		const std::optional<GraphicsOption> option = Graphics::FindOption(Option);
		Source = option ? static_cast<int32_t>(option->Source) : -1;
		const std::optional<QualityPreset> preset = Graphics::GetPreset();
		Preset = preset ? static_cast<int32_t>(*preset) : -1;
	}
};

ST_SCRIPT_CLASS(GraphicsProbe)
{
	ST_SCRIPT_FIELD(Request);
	ST_SCRIPT_FIELD(Option);
	ST_SCRIPT_FIELD(Value);
	ST_SCRIPT_FIELD(IntValue);
	ST_SCRIPT_FIELD(FloatValue);
	ST_SCRIPT_FIELD(Count);
	ST_SCRIPT_FIELD(Names);
	ST_SCRIPT_FIELD(PresetChoices);
	ST_SCRIPT_FIELD(Effective);
	ST_SCRIPT_FIELD(Source);
	ST_SCRIPT_FIELD(Preset);
	ST_SCRIPT_FIELD(Accepted);
	ST_SCRIPT_FIELD(Saved);
	ST_SCRIPT_FIELD(ResetDone);
	ST_SCRIPT_FIELD(InfoStructSize);
	ST_SCRIPT_FIELD(SmallInfoRefused);
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
}
