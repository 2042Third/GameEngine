#include "stpch.h"
#include "Strata/Network/JsonRpc.h"

namespace Strata
{

	namespace
	{

		// Consumed bytes are discarded once they exceed this size and half of the buffer, keeping appends amortized O(1).
		constexpr size_t c_CompactThreshold = 64 * 1024;

	}

	////////////////////////////////////////////////////////////////////////////////
	// JsonLineReader
	////////////////////////////////////////////////////////////////////////////////

	JsonLineReader::JsonLineReader(size_t maxMessageSize)
		: m_MaxMessageSize(maxMessageSize > 0 ? maxMessageSize : 1)
	{
	}

	void JsonLineReader::Append(std::span<const uint8_t> data)
	{
		Append(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
	}

	void JsonLineReader::Append(std::string_view data)
	{
		if (HasError() || data.empty())
			return;

		Compact();
		m_Buffer.append(data);

		// Fail early on a line that can no longer fit, instead of buffering an unbounded amount of data.
		const size_t terminator = m_Buffer.find('\n', m_ScanOffset);
		const size_t pendingEnd = terminator == std::string::npos ? m_Buffer.size() : terminator;
		if (pendingEnd - m_ReadOffset > m_MaxMessageSize + 1) // +1 tolerates the '\r' of a "\r\n" terminator
			SetOversizeError(pendingEnd - m_ReadOffset);
	}

	std::optional<std::string> JsonLineReader::NextLine()
	{
		while (!HasError())
		{
			const size_t terminator = m_Buffer.find('\n', m_ScanOffset);
			if (terminator == std::string::npos)
			{
				m_ScanOffset = m_Buffer.size();
				if (m_Buffer.size() - m_ReadOffset > m_MaxMessageSize + 1)
					SetOversizeError(m_Buffer.size() - m_ReadOffset);
				return std::nullopt;
			}

			size_t lineEnd = terminator;
			if (lineEnd > m_ReadOffset && m_Buffer[lineEnd - 1] == '\r')
				lineEnd--;

			const size_t lineStart = m_ReadOffset;
			m_ReadOffset = terminator + 1;
			m_ScanOffset = m_ReadOffset;

			const size_t length = lineEnd - lineStart;
			if (length > m_MaxMessageSize)
			{
				SetOversizeError(length);
				return std::nullopt;
			}

			std::string line = m_Buffer.substr(lineStart, length);
			const bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
			if (!blank)
				return line;
		}
		return std::nullopt;
	}

	void JsonLineReader::Reset()
	{
		m_Buffer.clear();
		m_ReadOffset = 0;
		m_ScanOffset = 0;
		m_Error.clear();
	}

	void JsonLineReader::Compact()
	{
		if (m_ReadOffset == 0)
			return;

		if (m_ReadOffset == m_Buffer.size())
		{
			m_Buffer.clear();
			m_ReadOffset = 0;
			m_ScanOffset = 0;
			return;
		}

		if (m_ReadOffset >= c_CompactThreshold && m_ReadOffset >= m_Buffer.size() / 2)
		{
			m_Buffer.erase(0, m_ReadOffset);
			m_ScanOffset -= m_ReadOffset;
			m_ReadOffset = 0;
		}
	}

	void JsonLineReader::SetOversizeError(size_t size)
	{
		m_Error = fmt::format("Message exceeds the maximum size of {} bytes ({} bytes buffered)", m_MaxMessageSize, size);
		m_Buffer.clear();
		m_Buffer.shrink_to_fit();
		m_ReadOffset = 0;
		m_ScanOffset = 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// JsonRpc
	////////////////////////////////////////////////////////////////////////////////

	namespace JsonRpc
	{

		nlohmann::json MakeRequest(const nlohmann::json& id, std::string_view method, nlohmann::json params)
		{
			nlohmann::json message = MakeNotification(method, std::move(params));
			message["id"] = id;
			return message;
		}

		nlohmann::json MakeNotification(std::string_view method, nlohmann::json params)
		{
			nlohmann::json message = nlohmann::json::object();
			message["jsonrpc"] = c_Version;
			message["method"] = std::string(method);
			if (!params.is_null())
				message["params"] = std::move(params);
			return message;
		}

		nlohmann::json MakeResult(const nlohmann::json& id, nlohmann::json result)
		{
			nlohmann::json message = nlohmann::json::object();
			message["jsonrpc"] = c_Version;
			message["id"] = id;
			message["result"] = std::move(result);
			return message;
		}

		nlohmann::json MakeError(const nlohmann::json& id, int code, std::string_view message, nlohmann::json data)
		{
			nlohmann::json error = nlohmann::json::object();
			error["code"] = code;
			error["message"] = std::string(message);
			if (!data.is_null())
				error["data"] = std::move(data);

			nlohmann::json response = nlohmann::json::object();
			response["jsonrpc"] = c_Version;
			response["id"] = id;
			response["error"] = std::move(error);
			return response;
		}

		std::string Serialize(const nlohmann::json& message)
		{
			// The replace handler makes dump() total: it never throws for invalid UTF-8.
			return message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		}

		std::optional<nlohmann::json> Parse(std::string_view text)
		{
			// Non-throwing overload: invalid input yields a "discarded" value.
			nlohmann::json value = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
			if (value.is_discarded())
				return std::nullopt;
			return value;
		}

		bool IsValidId(const nlohmann::json& id)
		{
			return id.is_string() || id.is_number() || id.is_null();
		}

		bool IsResponse(const nlohmann::json& message)
		{
			return message.is_object() && !message.contains("method") && (message.contains("result") || message.contains("error"));
		}

		RequestValidation ValidateRequest(const nlohmann::json& message)
		{
			RequestValidation validation;
			if (!message.is_object())
			{
				validation.Error = "A request must be a JSON object";
				return validation;
			}

			const auto id = message.find("id");
			if (id != message.end())
			{
				if (!IsValidId(*id))
				{
					validation.Error = "\"id\" must be a string, a number or null";
					return validation;
				}
				validation.Id = *id;
			}
			validation.IsNotification = id == message.end();

			const auto version = message.find("jsonrpc");
			if (version == message.end() || !version->is_string() || version->get_ref<const std::string&>() != c_Version)
			{
				validation.Error = "\"jsonrpc\" must be \"2.0\"";
				return validation;
			}

			const auto method = message.find("method");
			if (method == message.end() || !method->is_string() || method->get_ref<const std::string&>().empty())
			{
				validation.Error = "\"method\" must be a non-empty string";
				return validation;
			}

			const auto params = message.find("params");
			if (params != message.end() && !params->is_object() && !params->is_array())
			{
				validation.Error = "\"params\" must be an object or an array";
				return validation;
			}

			validation.Valid = true;
			return validation;
		}

	}

}
