#pragma once

#include "StrataScript/Entity.h"
#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>

namespace Strata
{

	namespace Detail
	{

		template<typename T>
		void AppendLogValue(std::ostream& stream, const T& value)
		{
			stream << value;
		}

		inline void AppendLogValue(std::ostream& stream, bool value)
		{
			stream << (value ? "true" : "false");
		}

		template<glm::length_t Length>
		void AppendLogValue(std::ostream& stream, const glm::vec<Length, float, glm::defaultp>& value)
		{
			stream << '(';
			for (glm::length_t index = 0; index < Length; index++)
				stream << (index > 0 ? ", " : "") << value[index];
			stream << ')';
		}

		inline void AppendLogValue(std::ostream& stream, const glm::quat& value)
		{
			stream << "quat(" << value.x << ", " << value.y << ", " << value.z << ", " << value.w << ')';
		}

		inline void AppendLogValue(std::ostream& stream, const Entity& value)
		{
			stream << "Entity(" << value.GetID() << ')';
		}

		inline void AppendLogValue(std::ostream& stream, const AssetHandle& value)
		{
			stream << "Asset(" << value.ID << ')';
		}

	}

	// Writes to the engine log (the editor console). Arguments are concatenated:
	//   Log::Info("Player ", GetEntity().GetName(), " has ", health, " health at ", position);
	class Log
	{
	public:
		template<typename... Args>
		static void Trace(const Args&... args) { Write(StrataScriptLogLevel_Trace, args...); }

		template<typename... Args>
		static void Info(const Args&... args) { Write(StrataScriptLogLevel_Info, args...); }

		template<typename... Args>
		static void Warn(const Args&... args) { Write(StrataScriptLogLevel_Warn, args...); }

		template<typename... Args>
		static void Error(const Args&... args) { Write(StrataScriptLogLevel_Error, args...); }
	private:
		template<typename... Args>
		static void Write(uint32_t level, const Args&... args)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			if (!host)
				return;

			std::ostringstream stream;
			(Detail::AppendLogValue(stream, args), ...);
			const std::string message = stream.str();
			host->Log(level, Detail::ToABIString(message));
		}
	};

}
