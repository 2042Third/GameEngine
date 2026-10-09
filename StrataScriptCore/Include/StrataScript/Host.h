#pragma once

#include "StrataScript/ScriptABI.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Internal plumbing of the script SDK: access to the engine's host API table and the context of the engine call that
// is currently executing. Game scripts use the public wrappers (Entity, Scene, Input, ...) instead.

namespace Strata
{

	namespace Detail
	{

		// Set by StrataScript_Load; null before the module is loaded.
		inline const StrataScriptHostAPI* s_Host = nullptr;
		// Context of the engine call currently executing (null outside engine calls).
		inline StrataScriptContext* s_Context = nullptr;
		// Entity of the script instance being constructed, so that Script's constructor can bind it.
		inline StrataScriptEntityID s_ConstructingEntity = 0;

		inline const StrataScriptHostAPI* GetHost()
		{
			return s_Host;
		}

		inline StrataScriptContext* GetContext()
		{
			return s_Context;
		}

		// Makes `context` current for the duration of an engine call (calls can nest).
		class ContextScope
		{
		public:
			explicit ContextScope(StrataScriptContext* context)
				: m_Previous(s_Context)
			{
				s_Context = context;
			}

			~ContextScope()
			{
				s_Context = m_Previous;
			}

			ContextScope(const ContextScope&) = delete;
			ContextScope& operator=(const ContextScope&) = delete;
		private:
			StrataScriptContext* m_Previous;
		};

		inline StrataScriptString ToABIString(std::string_view text)
		{
			return StrataScriptString { text.data(), static_cast<uint64_t>(text.size()) };
		}

		inline std::string_view FromABIString(const StrataScriptString& text)
		{
			if (!text.Data || text.Size == 0)
				return {};
			return std::string_view(text.Data, static_cast<size_t>(text.Size));
		}

		// Reads text from a host function following the buffer protocol: read(buffer, capacity) copies at most capacity
		// bytes and returns the full size.
		template<typename ReadFunction>
		std::string ReadHostString(ReadFunction&& read)
		{
			char stackBuffer[256];
			const uint64_t size = read(stackBuffer, static_cast<uint64_t>(sizeof(stackBuffer)));
			if (size <= sizeof(stackBuffer))
				return std::string(stackBuffer, static_cast<size_t>(size));

			// The text can only change between the two calls if the engine state changed in between, which it cannot
			// (calls are single-threaded); the size check keeps this safe regardless.
			std::string result(static_cast<size_t>(size), '\0');
			const uint64_t secondSize = read(result.data(), static_cast<uint64_t>(result.size()));
			result.resize(static_cast<size_t>(secondSize < size ? secondSize : size));
			return result;
		}

		// Reads an id array from a host function following the buffer protocol.
		template<typename ReadFunction>
		std::vector<uint64_t> ReadHostIDs(ReadFunction&& read)
		{
			uint64_t stackBuffer[64];
			const uint32_t count = read(stackBuffer, static_cast<uint32_t>(sizeof(stackBuffer) / sizeof(stackBuffer[0])));
			if (count <= sizeof(stackBuffer) / sizeof(stackBuffer[0]))
				return std::vector<uint64_t>(stackBuffer, stackBuffer + count);

			std::vector<uint64_t> result(count);
			const uint32_t secondCount = read(result.data(), count);
			result.resize(secondCount < count ? secondCount : count);
			return result;
		}

	}

}
