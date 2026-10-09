#pragma once

#include <string>

namespace Strata
{

	// Logs recurring errors once: a message is logged only when it differs from the previous one, until Clear() (call it
	// once the failing operation works again). Keeps failures that repeat every frame from flooding the log.
	// Not thread-safe.
	class ErrorThrottle
	{
	public:
		explicit ErrorThrottle(std::string source);

		// Logs "<source>: <message>" as an error unless it repeats the previous message. Returns whether it was logged.
		bool Report(const std::string& message);
		void Clear() { m_LastMessage.clear(); }
		const std::string& GetLastMessage() const { return m_LastMessage; }
	private:
		std::string m_Source;
		std::string m_LastMessage;
	};

}
