#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Network/JsonRpc.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	struct RpcMethodInfo
	{
		std::string Name;
		std::string Description;
		// JSON Schema of the params object: {"type": "object", "properties": {...}, "required": [...]}.
		nlohmann::json ParamsSchema = nlohmann::json { { "type", "object" }, { "properties", nlohmann::json::object() } };
	};

	struct RpcError
	{
		int Code = 0;
		std::string Message;
		nlohmann::json Data; // Null when absent
	};

	// Outcome of an RPC method: a JSON result value or an error.
	class RpcResult
	{
	public:
		static RpcResult Success(nlohmann::json value);
		static RpcResult Failure(int code, std::string message, nlohmann::json data = nullptr);

		bool IsSuccess() const { return !m_IsError; }
		bool IsError() const { return m_IsError; }

		// The result value (null for failures).
		const nlohmann::json& GetValue() const { return m_Value; }
		// The error (code 0 and an empty message for successes).
		const RpcError& GetError() const { return m_Error; }
	private:
		RpcResult() = default;
	private:
		nlohmann::json m_Value;
		RpcError m_Error;
		bool m_IsError = false;
	};

	// Completes one request. Handlers may respond immediately or keep the responder and respond later from any
	// thread (e.g. after advancing a few frames). Only the first response counts; later ones are ignored with a
	// warning. A responder destroyed without responding answers with an InternalError ("request dropped"), so a
	// client never waits forever. Responses for notifications or closed connections are discarded.
	class RpcResponder
	{
	public:
		using DeliveryFunction = std::function<void(RpcResult result)>;

		RpcResponder(std::string method, DeliveryFunction deliver);
		~RpcResponder();

		RpcResponder(const RpcResponder&) = delete;
		RpcResponder& operator=(const RpcResponder&) = delete;

		void Respond(RpcResult result); // Thread-safe
		// Responds unless a response was already sent (silently). Returns whether this call responded.
		bool TryRespond(RpcResult result);
		bool HasResponded() const { return m_Responded.load(); }
		const std::string& GetMethod() const { return m_Method; }
	private:
		std::string m_Method;
		DeliveryFunction m_Deliver;
		std::atomic<bool> m_Responded = false;
	};

	// Handler of an asynchronous method. params is an object (empty when the request had none) or an array.
	using RpcHandler = std::function<void(const nlohmann::json& params, const Ref<RpcResponder>& responder)>;
	// Handler of a synchronous method: the returned result is the response.
	using RpcSyncHandler = std::function<RpcResult(const nlohmann::json& params)>;

	// The rpc.authenticate handshake authenticates both sides. The client sends the session token together with a
	// fresh random nonce; the server checks the token and answers with a proof that it knows the token as well,
	// HMAC-SHA256(token, nonce + "strata-server") in lower-case hexadecimal. The client verifies the proof before it
	// sends anything else, so it never talks to a process that merely took over a dead editor's port.
	class RpcAuthentication
	{
	public:
		// 32 lower-case hexadecimal characters from the system's secure random generator (empty if it fails).
		static std::string GenerateNonce();
		// 32 to 128 hexadecimal characters.
		static bool IsValidNonce(std::string_view nonce);
		static std::string ComputeServerProof(std::string_view token, std::string_view nonce);
	};

	struct RpcServerSpecification
	{
		std::string BindAddress = "127.0.0.1"; // Must be a numeric loopback address (127.0.0.0/8 or ::1)
		uint16_t Port = 0;                     // 0 picks a free ephemeral port (see RpcServer::GetPort)
		std::string AuthToken;                 // Required; see EditorSession::GenerateSessionToken
		uint32_t MaxClients = 8;               // Authenticated connections
		uint32_t MaxPendingConnections = 8;    // Connections that have not authenticated yet
		std::chrono::milliseconds AuthenticationTimeout = std::chrono::milliseconds(5000); // From accept to rpc.authenticate
		size_t MaxMessageSize = c_DefaultMaxRpcMessageSize;  // Per message, in both directions
		uint32_t MaxQueuedRequests = 1024;                   // Waiting for ProcessRequests, across all clients
		size_t MaxQueuedBytes = 256ull * 1024 * 1024;        // Their total size; beyond it, no further requests are read
		// Requests and notifications of one client that are queued, being handled, or answered but not yet sent;
		// further ones of that client wait in its connection (backpressure).
		uint32_t MaxRequestsInFlightPerClient = 64;
		// A client that has output waiting but has not accepted a byte of it for this long is disconnected.
		std::chrono::milliseconds StalledClientTimeout = std::chrono::milliseconds(30000);
		bool UseWakeupNotifier = true; // Diagnostics: false makes the network thread poll at a short interval instead
	};

	// JSON-RPC 2.0 server over TCP (newline-delimited messages, see JsonRpc.h) for editor automation.
	//
	// Threading: a background thread owns all network I/O (accepting, reading, writing) and answers the built-in
	// methods itself. Requests for registered methods are queued and their handlers run on the thread that calls
	// ProcessRequests() (the main thread, once per frame), so handlers may touch engine state directly. Responses
	// may be produced from any thread. Start, Stop and ProcessRequests belong to the owning thread;
	// RegisterMethod, UnregisterMethod, GetMethods and the getters are thread-safe.
	//
	// Security model: every local process (and any web page in a local browser) may reach the port, so only
	// holders of the session token are trusted. The server binds loopback addresses only and requires a token.
	// A new connection must send rpc.authenticate with the token as its first message, within
	// AuthenticationTimeout; any other first message, a wrong token, or malformed input closes the connection.
	// Until then it is limited to tiny messages and output, and only MaxPendingConnections such connections are
	// kept (a new one evicts the oldest). Authenticated connections count toward MaxClients (rejected with
	// ServerBusy beyond it). A client's requests are only read while it has few enough in flight and little
	// output waiting; its answered responses wait in its connection until the socket takes them, and a client
	// that stops accepting output for StalledClientTimeout is dropped. Replies larger than MaxMessageSize are
	// replaced by an error, request ids longer than 256 characters are refused rather than echoed, and method
	// names quoted in error messages are shortened.
	//
	// Built-in methods (answered without waiting for ProcessRequests):
	//   rpc.authenticate {"token", "nonce"} -> {"authenticated": true, "proof"} (see RpcAuthentication)
	//   rpc.ping                            -> {"pong": true}
	//   rpc.listMethods                     -> {"methods": [{"name", "description", "paramsSchema"}, ...]}
	class RpcServer
	{
	public:
		RpcServer();
		~RpcServer();

		RpcServer(const RpcServer&) = delete;
		RpcServer& operator=(const RpcServer&) = delete;

		// Starts listening (stopping a previous session first). Returns false for a non-loopback bind address, an
		// empty token, or an unavailable address/port.
		bool Start(const RpcServerSpecification& specification);
		// Closes every connection and joins the network thread. Queued requests are discarded; pending responders
		// become inert.
		void Stop();
		bool IsRunning() const;
		uint16_t GetPort() const;
		uint32_t GetClientCount() const;

		// Registers a method (names starting with "rpc." are reserved). Returns false for an invalid or
		// duplicate name. A missing or non-object ParamsSchema is replaced by an empty object schema.
		bool RegisterMethod(RpcMethodInfo info, RpcHandler handler);
		bool RegisterMethod(RpcMethodInfo info, RpcSyncHandler handler);
		// Removes a method. If its handler is running on the owning thread, waits for it to return, so the caller
		// may destroy whatever the handler captured afterwards (calling it from inside a handler does not wait).
		void UnregisterMethod(const std::string& name);
		// Every callable method, built-ins first, then registered methods sorted by name.
		std::vector<RpcMethodInfo> GetMethods() const;

		// Runs the handlers of queued requests on the calling thread. Returns the number of requests processed.
		uint32_t ProcessRequests();
	private:
		struct Impl;
		Scope<Impl> m_Impl;
	};

}
