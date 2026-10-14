#pragma once

#include "Strata/Core/JsonUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace Strata
{

	// The text as nlohmann's lexer reads it (one character per get_character call), with a limit that is the next report
	// point instead of the end: reaching the limit costs the comparison reaching the end costs anyway, so the parse runs as
	// fast as nlohmann's own input from pointers, and once per interval the limit moves on and the position is reported.
	// Without a progress (or without its callback) the limit is the end.
	class JsonProgressInput
	{
	public:
		using char_type = char;

		JsonProgressInput(std::string_view text, const JsonParseProgress* progress)
			: m_Begin(text.data()), m_Current(text.data()), m_End(text.data() + text.size()),
			m_Progress(progress && progress->Callback ? progress : nullptr)
		{
			m_Limit = NextLimit();
		}

		// The lexer's interface names it (nlohmann::detail::lexer calls get_character).
		std::char_traits<char>::int_type get_character()
		{
			if (m_Current != m_Limit) [[likely]]
				return std::char_traits<char>::to_int_type(*m_Current++);
			return AtLimit();
		}
	private:
		// The next report point: an interval further, or the end.
		const char* NextLimit() const
		{
			if (!m_Progress)
				return m_End;
			const size_t interval = std::max<size_t>(m_Progress->Interval, 1);
			return static_cast<size_t>(m_End - m_Current) > interval ? m_Current + interval : m_End;
		}

		std::char_traits<char>::int_type AtLimit()
		{
			if (m_Current == m_End)
			{
				// The lexer may ask again at the end; it is reported once.
				if (m_Progress && !m_ReportedEnd)
				{
					m_ReportedEnd = true;
					m_Progress->Callback(static_cast<size_t>(m_End - m_Begin));
				}
				return std::char_traits<char>::eof();
			}
			m_Progress->Callback(static_cast<size_t>(m_Current - m_Begin));
			m_Limit = NextLimit();
			return std::char_traits<char>::to_int_type(*m_Current++);
		}
	private:
		const char* m_Begin = nullptr;
		const char* m_Current = nullptr;
		const char* m_Limit = nullptr;
		const char* m_End = nullptr;
		const JsonParseProgress* m_Progress = nullptr;
		bool m_ReportedEnd = false;
	};

	// Runs nlohmann's event (SAX) parser over the text as nlohmann::json::parse runs its parser: strict (the text holds one
	// value and nothing after it), without exceptions. The handler has nlohmann's SAX interface (nlohmann::json_sax's
	// functions, called directly rather than virtually: documents of a million entities produce a hundred million events);
	// one of them returning false ends the parse, which then returns false. `progress` (optional) hears of the bytes parsed.
	template<typename Handler>
	bool ParseJsonEvents(std::string_view text, Handler& handler, const JsonParseProgress* progress = nullptr)
	{
		nlohmann::detail::parser<nlohmann::json, JsonProgressInput> parser(JsonProgressInput(text, progress), nullptr, false, false);
		return parser.sax_parse(&handler, true);
	}

}
