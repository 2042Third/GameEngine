#pragma once

#include "Strata/Core/Base.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// JSON-RPC 2.0 over newline-delimited streams: every message is one compact JSON document followed by '\n'.
// Compact serialization escapes control characters inside strings, so a message never contains a raw newline.
// Batch requests are not supported.

namespace Strata
{

	constexpr size_t c_DefaultMaxRpcMessageSize = 64ull * 1024 * 1024;

	// Splits a byte stream into lines. Accepts "\n" and "\r\n" terminators and skips blank lines.
	// A line longer than the maximum message size puts the reader into a permanent error state, since the
	// stream can no longer be resynchronized safely.
	class JsonLineReader
	{
	public:
		explicit JsonLineReader(size_t maxMessageSize = c_DefaultMaxRpcMessageSize);

		void Append(std::span<const uint8_t> data);
		void Append(std::string_view data);

		// Extracts the next complete line without its terminator, or nullopt if no complete line is buffered.
		std::optional<std::string> NextLine();

		bool HasError() const { return !m_Error.empty(); }
		const std::string& GetError() const { return m_Error; }
		size_t GetBufferedSize() const { return m_Buffer.size() - m_ReadOffset; }
		size_t GetMaxMessageSize() const { return m_MaxMessageSize; }
		// Changes the limit for lines not extracted yet, e.g. raising it once a connection has authenticated.
		void SetMaxMessageSize(size_t maxMessageSize);
		void Reset();
	private:
		void Compact();
		void SetOversizeError(size_t size);
	private:
		std::string m_Buffer;
		size_t m_ReadOffset = 0; // Start of the first unconsumed line
		size_t m_ScanOffset = 0; // Bytes before this offset are known to contain no terminator
		size_t m_MaxMessageSize;
		std::string m_Error;
	};

	namespace JsonRpc
	{

		// Standard JSON-RPC 2.0 error codes plus Strata's implementation-defined codes (-32000 to -32099).
		// Plain enumerators so they convert to the integer codes used on the wire.
		struct ErrorCode
		{
			enum : int
			{
				ParseError = -32700,
				InvalidRequest = -32600,
				MethodNotFound = -32601,
				InvalidParams = -32602,
				InternalError = -32603,

				Unauthorized = -32001,     // The connection has not authenticated (or used a wrong token)
				ConnectionClosed = -32002, // Client side: not connected, or the connection was lost mid-call
				Timeout = -32003,          // Client side: no response arrived in time
				ServerBusy = -32004,       // The server rejected the request or connection due to load limits
				OperationFailed = -32005,  // The request was valid, but the method could not carry it out (the message says why)
				Cancelled = -32006         // The request was abandoned before it completed (e.g. the server is shutting down)
			};
		};

		constexpr const char* c_Version = "2.0";
		// Deepest nesting of arrays and objects Parse accepts. The parser itself is iterative, but copying, comparing
		// and serializing values recurse, so untrusted documents must not be able to exhaust the stack.
		constexpr size_t c_MaxJsonDepth = 256;

		// Message builders. A null params value omits "params" (allowed by JSON-RPC 2.0).
		nlohmann::json MakeRequest(const nlohmann::json& id, std::string_view method, nlohmann::json params = nlohmann::json::object());
		nlohmann::json MakeNotification(std::string_view method, nlohmann::json params = nlohmann::json::object());
		nlohmann::json MakeResult(const nlohmann::json& id, nlohmann::json result);
		nlohmann::json MakeError(const nlohmann::json& id, int code, std::string_view message, nlohmann::json data = nullptr);

		// Compact single-line serialization. Invalid UTF-8 in strings is replaced (U+FFFD) instead of failing.
		std::string Serialize(const nlohmann::json& message);
		// Returns nullopt if text is not valid JSON or nests arrays/objects deeper than c_MaxJsonDepth.
		std::optional<nlohmann::json> Parse(std::string_view text);

		// Request ids must be strings, numbers or null.
		bool IsValidId(const nlohmann::json& id);
		// True for a JSON-RPC response object (has "result" or "error", and no "method").
		bool IsResponse(const nlohmann::json& message);

		struct RequestValidation
		{
			bool Valid = false;
			bool IsNotification = false; // A request without "id" expects no response
			nlohmann::json Id;           // The request id when it could be recovered, else null
			std::string Error;           // Why the request is invalid
		};

		// Checks the JSON-RPC 2.0 request/notification structure: an object with "jsonrpc": "2.0", a string
		// "method", an optional valid "id", and optional "params" that is an object or an array.
		RequestValidation ValidateRequest(const nlohmann::json& message);

	}

}
