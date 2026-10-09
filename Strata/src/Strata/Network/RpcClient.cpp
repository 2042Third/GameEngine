#include "stpch.h"
#include "Strata/Network/RpcClient.h"

#include "Strata/Core/Crypto.h"

namespace Strata
{

	namespace
	{

		constexpr std::chrono::milliseconds c_NotificationSendTimeout = std::chrono::milliseconds(5000);

		// Moves the result out of response to avoid copying large payloads.
		RpcResult ToResult(nlohmann::json& response)
		{
			if (auto result = response.find("result"); result != response.end())
				return RpcResult::Success(std::move(*result));

			const auto error = response.find("error");
			if (error == response.end() || !error->is_object())
				return RpcResult::Failure(JsonRpc::ErrorCode::InternalError, "Malformed response: neither \"result\" nor an \"error\" object");

			const auto code = error->find("code");
			const auto message = error->find("message");
			const auto data = error->find("data");
			return RpcResult::Failure(
				code != error->end() && code->is_number_integer() ? code->get<int>() : static_cast<int>(JsonRpc::ErrorCode::InternalError),
				message != error->end() && message->is_string() ? message->get<std::string>() : std::string("Unknown error"),
				data != error->end() ? *data : nlohmann::json());
		}

	}

	RpcClient::~RpcClient()
	{
		Close();
	}

	bool RpcClient::Connect(std::string_view host, uint16_t port, std::string_view token, std::chrono::milliseconds timeout)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_Socket.Close();
		m_Reader.Reset();
		m_LastError.clear();

		std::string error;
		std::optional<TcpSocket> socket = TcpSocket::Connect(host, port, timeout, &error);
		if (!socket)
		{
			m_LastError = error;
			return false;
		}
		socket->SetNoDelay(true);
		m_Socket = std::move(*socket);

		if (!token.empty())
		{
			const std::string nonce = RpcAuthentication::GenerateNonce();
			if (nonce.empty())
			{
				FailLocked(JsonRpc::ErrorCode::InternalError, "Authentication failed: the system random number generator failed", true);
				return false;
			}

			const RpcResult result = CallLocked("rpc.authenticate", nlohmann::json { { "token", std::string(token) }, { "nonce", nonce } }, timeout);
			if (result.IsError())
			{
				FailLocked(result.GetError().Code, fmt::format("Authentication failed: {}", result.GetError().Message), true);
				return false;
			}

			// The server must prove that it knows the token before anything else is sent to it: a process that took
			// over the port of an editor that exited cannot.
			const nlohmann::json& value = result.GetValue();
			const auto proof = value.is_object() ? value.find("proof") : value.end();
			const bool proven = proof != value.end() && proof->is_string()
				&& Crypto::ConstantTimeEquals(proof->get_ref<const std::string&>(), RpcAuthentication::ComputeServerProof(token, nonce));
			if (!proven)
			{
				FailLocked(JsonRpc::ErrorCode::Unauthorized, "Authentication failed: the server could not prove that it knows the session token, so it may not be the editor", true);
				return false;
			}
		}
		return true;
	}

	RpcResult RpcClient::Call(const std::string& method, const nlohmann::json& params, std::chrono::milliseconds timeout)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return CallLocked(method, params, timeout);
	}

	RpcResult RpcClient::CallLocked(const std::string& method, const nlohmann::json& params, std::chrono::milliseconds requestedTimeout)
	{
		if (!m_Socket.IsValid())
			return FailLocked(JsonRpc::ErrorCode::ConnectionClosed, "Not connected", false);

		const std::chrono::milliseconds timeout = ClampSocketTimeout(requestedTimeout);
		const int64_t id = m_NextId++;
		std::string request = JsonRpc::Serialize(JsonRpc::MakeRequest(id, method, params));
		request += '\n';

		const auto deadline = std::chrono::steady_clock::now() + timeout;
		// A partially sent request leaves the stream unusable, so a failed send always drops the connection.
		if (!m_Socket.SendAll(request, timeout))
			return FailLocked(JsonRpc::ErrorCode::ConnectionClosed, fmt::format("Failed to send '{}': the connection was lost", method), true);

		while (true)
		{
			while (std::optional<std::string> line = m_Reader.NextLine())
			{
				std::optional<nlohmann::json> message = JsonRpc::Parse(*line);
				if (!message || !JsonRpc::IsResponse(*message))
					continue; // Notifications and unparsable lines are not ours to answer

				const auto responseId = message->find("id");
				if (responseId == message->end())
					continue;

				// A null id means the server could not attribute its error to a request (e.g. it could not parse
				// it); with one request in flight at a time, that error belongs to this call.
				const bool matches = responseId->is_number_integer() && responseId->get<int64_t>() == id;
				if (!matches && !responseId->is_null())
					continue; // A late response to an earlier, timed out call

				RpcResult result = ToResult(*message);
				if (result.IsError())
					m_LastError = result.GetError().Message;
				return result;
			}

			if (m_Reader.HasError())
				return FailLocked(JsonRpc::ErrorCode::ConnectionClosed, fmt::format("Invalid response stream: {}", m_Reader.GetError()), true);

			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			if (remaining.count() <= 0)
				return FailLocked(JsonRpc::ErrorCode::Timeout, fmt::format("No response to '{}' within {} ms", method, timeout.count()), false);

			m_ReceiveBuffer.clear();
			const SocketReceiveStatus status = m_Socket.Receive(m_ReceiveBuffer, remaining);
			if (status == SocketReceiveStatus::Data)
				m_Reader.Append(m_ReceiveBuffer);
			else if (status == SocketReceiveStatus::Closed)
				return FailLocked(JsonRpc::ErrorCode::ConnectionClosed, fmt::format("The server closed the connection during '{}'", method), true);
			else if (status == SocketReceiveStatus::Error)
				return FailLocked(JsonRpc::ErrorCode::ConnectionClosed, fmt::format("The connection failed during '{}'", method), true);
		}
	}

	bool RpcClient::Notify(const std::string& method, const nlohmann::json& params)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (!m_Socket.IsValid())
		{
			m_LastError = "Not connected";
			return false;
		}

		std::string notification = JsonRpc::Serialize(JsonRpc::MakeNotification(method, params));
		notification += '\n';
		if (!m_Socket.SendAll(notification, c_NotificationSendTimeout))
		{
			FailLocked(JsonRpc::ErrorCode::ConnectionClosed, fmt::format("Failed to send '{}': the connection was lost", method), true);
			return false;
		}
		return true;
	}

	bool RpcClient::IsConnected() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_Socket.IsValid();
	}

	bool RpcClient::CheckConnection()
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (!m_Socket.IsValid())
			return false;

		m_ReceiveBuffer.clear();
		const SocketReceiveStatus status = m_Socket.Receive(m_ReceiveBuffer, std::chrono::milliseconds(0));
		if (status == SocketReceiveStatus::Data)
			m_Reader.Append(m_ReceiveBuffer);
		else if (status == SocketReceiveStatus::Closed || status == SocketReceiveStatus::Error)
			FailLocked(JsonRpc::ErrorCode::ConnectionClosed, "The server closed the connection", true);
		return m_Socket.IsValid();
	}

	void RpcClient::Close()
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_Socket.Close();
		m_Reader.Reset();
	}

	std::string RpcClient::GetLastError() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_LastError;
	}

	RpcResult RpcClient::FailLocked(int code, std::string message, bool closeConnection)
	{
		m_LastError = message;
		if (closeConnection)
		{
			m_Socket.Close();
			m_Reader.Reset();
		}
		return RpcResult::Failure(code, std::move(message));
	}

}
