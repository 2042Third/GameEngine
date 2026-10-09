#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Network/JsonRpc.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
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

	struct RpcServerSpecification
	{
		std::string BindAddress = "127.0.0.1"; // Loopback only by default; automation must not be reachable remotely
		uint16_t Port = 0;                     // 0 picks a free ephemeral port (see RpcServer::GetPort)
		std::string AuthToken;                 // Non-empty: clients must call rpc.authenticate first
		uint32_t MaxClients = 8;
		size_t MaxMessageSize = c_DefaultMaxRpcMessageSize;
	};

	// JSON-RPC 2.0 server over TCP (newline-delimited messages, see JsonRpc.h).
	//
	// Threading: a background thread owns all network I/O (accepting, reading, writing) and answers the built-in
	// methods itself. Requests for registered methods are queued and their handlers run on the thread that calls
	// ProcessRequests() (the main thread, once per frame), so handlers may touch engine state directly. Responses
	// may be produced from any thread. Start, Stop and ProcessRequests belong to the owning thread;
	// RegisterMethod, UnregisterMethod, GetMethods and the getters are thread-safe.
	//
	// Built-in methods (always available, answered without waiting for ProcessRequests):
	//   rpc.authenticate {"token": "..."} -> {"authenticated": true}
	//   rpc.ping                          -> {"pong": true}
	//   rpc.listMethods                   -> {"methods": [{"name", "description", "paramsSchema"}, ...]}
	// When an auth token is configured, every request other than rpc.authenticate fails with Unauthorized until
	// the connection has authenticated.
	class RpcServer
	{
	public:
		RpcServer();
		~RpcServer();

		RpcServer(const RpcServer&) = delete;
		RpcServer& operator=(const RpcServer&) = delete;

		// Starts listening (stopping a previous session first). Returns false if the address/port is unavailable.
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
