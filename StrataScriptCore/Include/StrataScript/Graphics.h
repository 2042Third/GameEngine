#pragma once

#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// The presets of the "Preset" option, in the order of its choices: what Project Settings > Graphics Quality offers.
	enum class QualityPreset : uint32_t
	{
		Low = 0,
		Medium = 1, // The costs of Strata 0.6.0
		High = 2,   // The default
		Ultra = 3,
		Custom = 4  // Every other option as set
	};

	enum class GraphicsOptionType : uint32_t
	{
		Bool = StrataScriptGraphicsOptionType_Bool,
		Int = StrataScriptGraphicsOptionType_Int,
		Float = StrataScriptGraphicsOptionType_Float,
		Choice = StrataScriptGraphicsOptionType_Choice
	};

	enum class GraphicsOptionSource : uint32_t
	{
		Project = StrataScriptGraphicsOptionSource_Project,
		Player = StrataScriptGraphicsOptionSource_Player,
		CommandLine = StrataScriptGraphicsOptionSource_CommandLine
	};

	// A graphics option as an options menu shows it.
	struct GraphicsOption
	{
		std::string Name;        // What Get and Set take
		std::string Title;       // For the menu (English: a game that translates its menu writes its own)
		std::string Description; // What it changes and what it costs, for the menu's tooltip or help line
		GraphicsOptionType Type = GraphicsOptionType::Bool;
		double Min = 0.0;        // Int and Float
		double Max = 0.0;
		std::vector<std::string> Choices; // Choice: the names Set takes, in order
		GraphicsOptionSource Source = GraphicsOptionSource::Project; // Where the value in effect comes from
	};

	namespace Detail
	{
		constexpr std::array<std::string_view, 5> c_QualityPresetNames = { "Low", "Medium", "High", "Ultra", "Custom" };

		inline bool EqualsIgnoringASCIICase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t index = 0; index < a.size(); index++)
			{
				if (std::tolower(static_cast<unsigned char>(a[index])) != std::tolower(static_cast<unsigned char>(b[index])))
					return false;
			}
			return true;
		}
	}

	// The players' graphics options (a game's options menu): the rows of Project Settings > Graphics Quality by name. The
	// value in effect is the project's, the player's choices over it, and the command line's preset over both
	// (StrataRuntime --quality); renderers follow it from the next frame (a change of shadow resolution rebuilds the shadow
	// maps once). Setting a row that follows the preset (a cost, such as "ShadowCascades") while the preset is not Custom
	// switches it to Custom, starting from the look in effect; the players' own rows ("AntiAliasing", "MotionBlur",
	// "DepthOfField", "FilmGrain", "ChromaticAberration"), which every preset leaves alone, keep the preset. Choices last for
	// the session; Save keeps them for the next run (the engine's file in the game's save directory). Engines without graphics
	// options (before 0.7.0) return nothing and false.
	class Graphics
	{
	public:
		// Every option, in the engine's order ("Preset" first).
		static std::vector<GraphicsOption> Options();
		// The option of that name (ignoring case), or nothing.
		static std::optional<GraphicsOption> FindOption(std::string_view name);

		// The value in effect: bool, int32_t, int64_t, uint32_t or float for Bool, Int and Float options, std::string for a
		// Choice (the choice's name); nothing for an unknown option or another type.
		template<typename T>
		static std::optional<T> Get(std::string_view option);
		// The player's choice: false (logged by the engine), changing nothing, for an unknown option, another type or a value
		// out of range. A Choice option takes the choice's name (the string overloads), never a number.
		template<typename T>
		static bool Set(std::string_view option, const T& value);
		static bool Set(std::string_view option, std::string_view choice);
		static bool Set(std::string_view option, const char* choice) { return Set(option, std::string_view(choice ? choice : "")); }

		static std::optional<QualityPreset> GetPreset();
		static bool SetPreset(QualityPreset preset);
		// "Low" ... "Custom" (the Preset option's choice names); "" for a value out of range.
		static std::string_view GetPresetName(QualityPreset preset);

		// Removes the player's choice of an option, or of every option (empty): the project's values apply again.
		static bool Reset(std::string_view option = {});
		// Writes the player's choices for the next run; false when the game has nowhere to keep them.
		static bool Save();
	};

	inline std::vector<GraphicsOption> Graphics::Options()
	{
		std::vector<GraphicsOption> options;
		const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GraphicsGetOptionChoice);
		if (!host || !host->GraphicsGetOptionCount || !host->GraphicsDescribeOption)
			return options;
		const uint32_t count = host->GraphicsGetOptionCount(Detail::GetContext());
		for (uint32_t index = 0; index < count; index++)
		{
			StrataScriptGraphicsOptionInfo info = {};
			info.StructSize = static_cast<uint32_t>(sizeof(info));
			if (!host->GraphicsDescribeOption(Detail::GetContext(), index, &info))
				continue;
			GraphicsOption option;
			option.Name = std::string(Detail::FromABIString(info.Name));
			option.Title = std::string(Detail::FromABIString(info.Title));
			option.Description = std::string(Detail::FromABIString(info.Description));
			option.Type = static_cast<GraphicsOptionType>(info.Type);
			option.Min = info.Min;
			option.Max = info.Max;
			if (ST_SCRIPT_HAS_MEMBER(StrataScriptGraphicsOptionInfo, &info, Source))
				option.Source = static_cast<GraphicsOptionSource>(info.Source);
			for (uint32_t choice = 0; choice < info.ChoiceCount; choice++)
			{
				option.Choices.push_back(Detail::ReadHostString([host, index, choice](char* buffer, uint64_t capacity)
				{
					return host->GraphicsGetOptionChoice(Detail::GetContext(), index, choice, buffer, capacity);
				}));
			}
			options.push_back(std::move(option));
		}
		return options;
	}

	inline std::optional<GraphicsOption> Graphics::FindOption(std::string_view name)
	{
		for (GraphicsOption& option : Options())
		{
			if (Detail::EqualsIgnoringASCIICase(option.Name, name))
				return std::move(option);
		}
		return std::nullopt;
	}

	template<typename T>
	std::optional<T> Graphics::Get(std::string_view option)
	{
		const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GraphicsGetOption);
		if (!host)
			return std::nullopt;
		StrataScriptValue value = {};
		char stackBuffer[256];
		if (!host->GraphicsGetOption(Detail::GetContext(), Detail::ToABIString(option), &value, stackBuffer, sizeof(stackBuffer)))
			return std::nullopt;
		std::string largeBuffer;
		if (value.Type == StrataScriptValueType_String && value.As.String.Size > sizeof(stackBuffer))
		{
			// Longer than the stack buffer: read again into a buffer of the reported size.
			largeBuffer.resize(static_cast<size_t>(value.As.String.Size));
			if (!host->GraphicsGetOption(Detail::GetContext(), Detail::ToABIString(option), &value, largeBuffer.data(), largeBuffer.size()))
				return std::nullopt;
			if (value.As.String.Size > largeBuffer.size())
				value.As.String.Size = largeBuffer.size();
		}
		T result {};
		if (!Detail::ValueTraits<T>::FromValue(value, result))
			return std::nullopt;
		return result;
	}

	template<typename T>
	bool Graphics::Set(std::string_view option, const T& value)
	{
		const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GraphicsSetOption);
		if (!host)
			return false;
		const StrataScriptValue abiValue = Detail::ValueTraits<T>::ToValue(value);
		return host->GraphicsSetOption(Detail::GetContext(), Detail::ToABIString(option), &abiValue);
	}

	inline bool Graphics::Set(std::string_view option, std::string_view choice)
	{
		const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GraphicsSetOption);
		if (!host)
			return false;
		StrataScriptValue value = {};
		value.Type = StrataScriptValueType_String;
		value.As.String = Detail::ToABIString(choice);
		return host->GraphicsSetOption(Detail::GetContext(), Detail::ToABIString(option), &value);
	}

	inline std::optional<QualityPreset> Graphics::GetPreset()
	{
		const std::optional<std::string> name = Get<std::string>("Preset");
		if (!name)
			return std::nullopt;
		for (size_t index = 0; index < Detail::c_QualityPresetNames.size(); index++)
		{
			if (Detail::EqualsIgnoringASCIICase(*name, Detail::c_QualityPresetNames[index]))
				return static_cast<QualityPreset>(index);
		}
		return std::nullopt;
	}

	inline bool Graphics::SetPreset(QualityPreset preset)
	{
		const std::string_view name = GetPresetName(preset);
		return !name.empty() && Set("Preset", name);
	}

	inline std::string_view Graphics::GetPresetName(QualityPreset preset)
	{
		const size_t index = static_cast<size_t>(preset);
		return index < Detail::c_QualityPresetNames.size() ? Detail::c_QualityPresetNames[index] : std::string_view();
	}

	inline bool Graphics::Reset(std::string_view option)
	{
		const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GraphicsResetOptions);
		return host && host->GraphicsResetOptions(Detail::GetContext(), Detail::ToABIString(option));
	}

	inline bool Graphics::Save()
	{
		const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GraphicsSaveOptions);
		return host && host->GraphicsSaveOptions(Detail::GetContext());
	}

}
