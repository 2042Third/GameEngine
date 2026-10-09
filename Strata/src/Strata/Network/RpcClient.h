#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Network/JsonRpc.h"
#include "Strata/Network/RpcServer.h"
#include "Strata/Network/Socket.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace Strata
{

	// Synchronous JSON-RPC 2.0 client for RpcServer (newline-delimited messages over TCP).
	// Thread-safe: concurrent calls are serialized, one request in flight at a time.
	class RpcClient
	{
	public:
		RpcClient() = default;
		~RpcClient();

		RpcClient(const RpcClient&) = delete;
		RpcClient& operator=(const RpcClient&) = delete;

		// Connects (closing any previous connection) and runs the authentication handshake with the session token
		// (see RpcAuthentication): the connection fails unless the server proves that it knows the token, before
		// the client proves it in return. The token itself is never sent, and an empty token fails at once.
		// timeout bounds the connection and the whole handshake separately.
		bool Connect(std::string_view host, uint16_t port, std::string_view token, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

		// Sends a request and waits for its response. Notifications and responses to other (e.g. timed out)
		// requests are skipped. Transport failures are reported as ConnectionClosed (the connection is closed)
		// or Timeout (the connection stays usable; a late response is ignored) errors.
		RpcResult Call(const std::string& method, const nlohmann::json& params = nlohmann::json::object(), std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
		// Sends a notification (no response).
		bool Notify(const std::string& method, const nlohmann::json& params = nlohmann::json::object());

		bool IsConnected() const;
		// Detects a connection the server has closed without sending anything. Received data is kept for the
		// next call. Returns IsConnected() afterwards.
		bool CheckConnection();
		void Close();

		std::string GetLastError() const;
	private:
		bool AuthenticateLocked(std::string_view token, std::chrono::milliseconds timeout);
		RpcResult CallLocked(const std::string& method, const nlohmann::json& params, std::chrono::milliseconds timeout);
		RpcResult FailLocked(int code, std::string message, bool closeConnection);
	private:
		mutable std::mutex m_Mutex;
		TcpSocket m_Socket;
		JsonLineReader m_Reader;
		std::vector<uint8_t> m_ReceiveBuffer;
		int64_t m_NextId = 1;
		std::string m_LastError;
	};

}
