#include "stpch.h"
#include "Strata/Network/RpcServer.h"

#include "Strata/Core/Crypto.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/Socket.h"

#include <condition_variable>
#include <map>

namespace Strata
{

	namespace
	{

		constexpr const char* c_AuthenticateMethod = "rpc.authenticate";
		constexpr const char* c_PingMethod = "rpc.ping";
		constexpr const char* c_ListMethodsMethod = "rpc.listMethods";
		constexpr std::string_view c_ReservedMethodPrefix = "rpc.";

		// With a working notifier the network thread wakes up on events; this timeout is only a safety net.
		constexpr std::chrono::milliseconds c_IdlePollInterval = std::chrono::milliseconds(1000);
		// Without a notifier, responses produced on other threads are picked up at this interval.
		constexpr std::chrono::milliseconds c_FallbackPollInterval = std::chrono::milliseconds(5);
		// Time a connection that is being closed after an error gets to receive the error response.
		constexpr std::chrono::milliseconds c_ErrorCloseLinger = std::chrono::milliseconds(2000);
		// Time a client that half-closed its side (sent everything, still reading) gets to receive its responses.
		constexpr std::chrono::milliseconds c_HalfCloseLinger = std::chrono::milliseconds(30000);
		// After the server has sent everything and shut down its sending side, how long it waits for the client's
		// end of stream before closing anyway.
		constexpr std::chrono::milliseconds c_DrainTimeout = std::chrono::milliseconds(2000);

		// Limits before authentication: the only useful message is a small rpc.authenticate request, and the only
		// output an error or two.
		constexpr size_t c_PreAuthMaxMessageSize = 4 * 1024;
		constexpr size_t c_PreAuthMaxPendingOutput = 16 * 1024;
		// While a client has this much output waiting, no further requests of it are read (backpressure).
		constexpr size_t c_OutputBackpressureThreshold = 1024 * 1024;
		// Consumed output is discarded once it exceeds this size and half of the send buffer.
		constexpr size_t c_SendBufferCompactThreshold = 1024 * 1024;
		// Connections tracked beyond MaxClients + MaxPendingConnections (those being closed). Further connections
		// are dropped immediately, so a connection flood cannot exhaust file descriptors or memory.
		constexpr size_t c_MaxClosingConnections = 16;
		// Repeated warnings (e.g. from a port scanner or a hostile process) are logged at most this often.
		constexpr std::chrono::seconds c_WarningInterval = std::chrono::seconds(5);

		nlohmann::json MakeEmptyObjectSchema()
		{
			return nlohmann::json { { "type", "object" }, { "properties", nlohmann::json::object() } };
		}

		nlohmann::json NormalizeParamsSchema(nlohmann::json schema)
		{
			if (!schema.is_object())
				return MakeEmptyObjectSchema();
			if (!schema.contains("type"))
				schema["type"] = "object";
			return schema;
		}

		std::vector<RpcMethodInfo> GetBuiltInMethods()
		{
			std::vector<RpcMethodInfo> methods;

			RpcMethodInfo& authenticate = methods.emplace_back();
			authenticate.Name = c_AuthenticateMethod;
			authenticate.Description = "Authenticates this connection with the session token and returns proof that the server knows it too "
				"(HMAC-SHA256 of the nonce followed by \"strata-server\"). Must be the first request of every connection.";
			authenticate.ParamsSchema = nlohmann::json {
				{ "type", "object" },
				{ "properties", {
					{ "token", { { "type", "string" }, { "description", "Session token from the editor session file" } } },
					{ "nonce", { { "type", "string" }, { "description", "Fresh random value, 32 to 128 hexadecimal characters" } } } } },
				{ "required", nlohmann::json::array({ "token", "nonce" }) }
			};

			RpcMethodInfo& ping = methods.emplace_back();
			ping.Name = c_PingMethod;
			ping.Description = "Checks that the server is responsive. Returns {\"pong\": true}.";

			RpcMethodInfo& listMethods = methods.emplace_back();
			listMethods.Name = c_ListMethodsMethod;
			listMethods.Description = "Lists every available method with its description and JSON Schema of its parameters.";
			return methods;
		}

		constexpr std::string_view c_ServerProofLabel = "strata-server";
		constexpr size_t c_MinNonceLength = 32;
		constexpr size_t c_MaxNonceLength = 128;

		nlohmann::json MakeResponse(const nlohmann::json& id, const RpcResult& result)
		{
			if (result.IsSuccess())
				return JsonRpc::MakeResult(id, result.GetValue());

			const RpcError& error = result.GetError();
			return JsonRpc::MakeError(id, error.Code, error.Message, error.Data);
		}

		// Limits a repeated warning to one message per interval. Used by the network thread only.
		class WarningLimiter
		{
		public:
			// Returns true if a message may be logged now; suppressed then holds how many were dropped since the last.
			bool Allow(uint64_t& suppressed)
			{
				const auto now = std::chrono::steady_clock::now();
				if (m_HasLogged && now - m_LastLogged < c_WarningInterval)
				{
					m_Suppressed++;
					return false;
				}
				m_HasLogged = true;
				m_LastLogged = now;
				suppressed = std::exchange(m_Suppressed, uint64_t(0));
				return true;
			}
		private:
			std::chrono::steady_clock::time_point m_LastLogged;
			uint64_t m_Suppressed = 0;
			bool m_HasLogged = false;
		};

		std::string DescribeSuppressed(uint64_t suppressed)
		{
			return suppressed > 0 ? fmt::format(" ({} similar messages suppressed)", suppressed) : std::string();
		}

		// A response produced by a responder (on any thread), waiting for the network thread.
		struct OutgoingResponse
		{
			uint64_t ConnectionId = 0;
			nlohmann::json Id;
			RpcResult Result;
		};

		struct QueuedRequest
		{
			uint64_t ConnectionId = 0;
			nlohmann::json Id;
			bool IsNotification = false;
			std::string Method;
			nlohmann::json Params;
		};

		struct RegisteredMethod
		{
			RpcMethodInfo Info;
			RpcHandler Handler;
		};

	}

	////////////////////////////////////////////////////////////////////////////////
	// RpcResult / RpcResponder
	////////////////////////////////////////////////////////////////////////////////

	RpcResult RpcResult::Success(nlohmann::json value)
	{
		RpcResult result;
		result.m_Value = std::move(value);
		return result;
	}

	RpcResult RpcResult::Failure(int code, std::string message, nlohmann::json data)
	{
		RpcResult result;
		result.m_IsError = true;
		result.m_Error.Code = code;
		result.m_Error.Message = std::move(message);
		result.m_Error.Data = std::move(data);
		return result;
	}

	RpcResponder::RpcResponder(std::string method, DeliveryFunction deliver)
		: m_Method(std::move(method)), m_Deliver(std::move(deliver))
	{
	}

	RpcResponder::~RpcResponder()
	{
		if (m_Responded.exchange(true))
			return;

		// A destructor must not throw; delivery only allocates and enqueues, so a failure here can only be an
		// allocation failure, and the client then sees its connection close instead.
		try
		{
			if (m_Deliver)
				m_Deliver(RpcResult::Failure(JsonRpc::ErrorCode::InternalError, fmt::format("request dropped: '{}' finished without responding", m_Method)));
		}
		catch (...)
		{
		}
	}

	bool RpcResponder::TryRespond(RpcResult result)
	{
		if (m_Responded.exchange(true))
			return false;
		if (!m_Deliver)
			return true;

		try
		{
			m_Deliver(std::move(result));
		}
		catch (const std::exception& exception)
		{
			ST_CORE_ERROR("RpcResponder: failed to deliver the response to '{}': {}", m_Method, exception.what());
		}
		return true;
	}

	void RpcResponder::Respond(RpcResult result)
	{
		if (!TryRespond(std::move(result)))
			ST_CORE_WARN("RpcResponder: '{}' already responded; ignoring the second response", m_Method);
	}

	////////////////////////////////////////////////////////////////////////////////
	// RpcAuthentication
	////////////////////////////////////////////////////////////////////////////////

	std::string RpcAuthentication::GenerateNonce()
	{
		std::array<uint8_t, 16> bytes = {};
		if (!Platform::GenerateSecureRandom(bytes))
			return {};
		return Crypto::ToHex(bytes);
	}

	bool RpcAuthentication::IsValidNonce(std::string_view nonce)
	{
		if (nonce.size() < c_MinNonceLength || nonce.size() > c_MaxNonceLength)
			return false;
		return std::all_of(nonce.begin(), nonce.end(), [](char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
		});
	}

	std::string RpcAuthentication::ComputeServerProof(std::string_view token, std::string_view nonce)
	{
		std::string message(nonce);
		message += c_ServerProofLabel;
		return Crypto::ToHex(Crypto::HmacSha256(token, message));
	}

	////////////////////////////////////////////////////////////////////////////////
	// RpcServer internals
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		// State shared by the server, its network thread and outstanding responders. Responders hold weak
		// references, so a responder that outlives the server (or a stopped session) simply becomes inert.
		struct RpcServerShared
		{
			std::mutex Mutex;
			bool Running = false;
			std::unordered_set<uint64_t> OpenConnections;
			std::vector<OutgoingResponse> Outgoing;
			// Wakes the network thread. Notify() is only called under Mutex while Running, so Stop() can close
			// the notifier safely once it has cleared Running.
			SocketNotifier Notifier;

			void Deliver(uint64_t connectionId, nlohmann::json id, RpcResult result)
			{
				std::scoped_lock<std::mutex> lock(Mutex);
				if (!Running || !OpenConnections.contains(connectionId))
					return;

				Outgoing.push_back(OutgoingResponse { connectionId, std::move(id), std::move(result) });
				Notifier.Notify();
			}
		};

		// One client connection. Owned and accessed exclusively by the network thread.
		struct Connection
		{
			explicit Connection(size_t maxMessageSize)
				: Reader(maxMessageSize)
			{
			}

			uint64_t Id = 0;
			TcpSocket Socket;
			JsonLineReader Reader;
			std::vector<uint8_t> ReceiveBuffer;
			std::string SendBuffer;
			size_t SendOffset = 0;
			bool Authenticated = false;
			std::chrono::steady_clock::time_point AuthenticationDeadline;
			uint32_t OutstandingRequests = 0; // Queued requests whose response has not arrived yet

			// A closing connection processes no more requests. Once its responses are flushed it shuts down its
			// sending side and waits for the client's end of stream (input is read and discarded meanwhile), so the
			// final close is orderly instead of a reset that could destroy the last response in transit. It is
			// closed regardless when CloseDeadline passes. Closed connections are removed at the end of the iteration.
			bool PeerFinished = false; // The client closed its sending side
			bool Closing = false;
			bool SentShutdown = false;
			bool Closed = false;
			std::chrono::steady_clock::time_point CloseDeadline;

			size_t GetPendingOutput() const { return SendBuffer.size() - SendOffset; }
		};

	}

	struct RpcServer::Impl
	{
		RpcServerSpecification Specification;
		TcpListener Listener; // Used by the network thread while running
		Ref<RpcServerShared> Shared;
		std::thread NetworkThread;

		std::atomic<bool> Running = false;
		std::atomic<uint16_t> Port = 0;
		std::atomic<uint32_t> ClientCount = 0;

		mutable std::mutex MethodsMutex;
		std::condition_variable MethodsCondition;
		std::map<std::string, RegisteredMethod> Methods;
		std::string RunningMethod;        // Handler executing in ProcessRequests (guarded by MethodsMutex)
		std::thread::id ProcessingThread; // The thread executing it

		std::mutex QueueMutex;
		std::vector<QueuedRequest> Queue;

		// Network thread state
		std::vector<Scope<Connection>> Connections;
		uint64_t NextConnectionId = 1;
		WarningLimiter AuthenticationWarnings;
		WarningLimiter RejectionWarnings;
		WarningLimiter ProtocolWarnings;
		WarningLimiter ErrorWarnings;

		std::vector<RpcMethodInfo> GetMethods() const;

		void RunNetworkThread();
		bool RunIteration(std::vector<OutgoingResponse>& outgoing, std::vector<SocketPollEntry>& pollEntries);
		uint32_t AcceptConnections();
		void ReadFrom(Connection& connection);
		void ProcessBufferedLines(Connection& connection);
		void HandleMessage(Connection& connection, const std::string& line);
		void HandleFirstMessage(Connection& connection, const std::optional<nlohmann::json>& message);
		RpcResult CheckToken(const nlohmann::json& params) const;
		void DeliverResponse(Connection& connection, const OutgoingResponse& response);
		void Send(Connection& connection, const nlohmann::json& message);
		void SendSerialized(Connection& connection, std::string serialized);
		void Flush(Connection& connection);
		void CheckDeadlines(Connection& connection, std::chrono::steady_clock::time_point now);
		void BeginClose(Connection& connection, std::chrono::milliseconds linger);
		void RemoveClosedConnections();
		void UpdateClientCount();
		bool CanProcessRequests(const Connection& connection) const;
		bool WantsInput(const Connection& connection) const;
		size_t CountConnections(bool authenticated) const;
		Connection* FindConnection(uint64_t id);

		// Runs per-connection work so that a failure (in practice an allocation failure) closes that connection
		// instead of escaping the network thread and terminating the process.
		template<typename Function>
		void Guard(Connection& connection, Function&& function)
		{
			try
			{
				function();
			}
			catch (const std::exception& exception)
			{
				uint64_t suppressed = 0;
				if (ErrorWarnings.Allow(suppressed))
					ST_CORE_ERROR("RpcServer: closing client {} after an error: {}{}", connection.Id, exception.what(), DescribeSuppressed(suppressed));
				connection.Closed = true;
			}
		}
	};

	std::vector<RpcMethodInfo> RpcServer::Impl::GetMethods() const
	{
		std::vector<RpcMethodInfo> methods = GetBuiltInMethods();
		std::scoped_lock<std::mutex> lock(MethodsMutex);
		for (const auto& [name, method] : Methods)
			methods.push_back(method.Info);
		return methods;
	}

	void RpcServer::Impl::RunNetworkThread()
	{
		Platform::SetCurrentThreadName("RpcServer");

		std::vector<OutgoingResponse> outgoing;
		std::vector<SocketPollEntry> pollEntries;
		bool failureReported = false;
		while (true)
		{
			{
				std::scoped_lock<std::mutex> lock(Shared->Mutex);
				if (!Shared->Running)
					break;
				outgoing.swap(Shared->Outgoing);
			}

			// Per-connection work is guarded individually; this catches what remains (e.g. an allocation failure
			// while building the poll set) so the thread survives instead of calling std::terminate.
			bool succeeded = false;
			try
			{
				succeeded = RunIteration(outgoing, pollEntries);
				if (!succeeded && !failureReported)
					ST_CORE_ERROR("RpcServer: waiting for socket events failed; retrying");
			}
			catch (const std::exception& exception)
			{
				if (!failureReported)
					ST_CORE_ERROR("RpcServer: network thread error: {}; retrying", exception.what());
			}
			outgoing.clear();

			if (!succeeded)
			{
				failureReported = true;
				std::this_thread::sleep_for(c_FallbackPollInterval);
				continue;
			}
			failureReported = false;
		}

		// Shutting down: drop every connection without waiting for pending output.
		Connections.clear();
		{
			std::scoped_lock<std::mutex> lock(Shared->Mutex);
			Shared->OpenConnections.clear();
			Shared->Outgoing.clear();
		}
		ClientCount = 0;
	}

	bool RpcServer::Impl::RunIteration(std::vector<OutgoingResponse>& outgoing, std::vector<SocketPollEntry>& pollEntries)
	{
		for (const OutgoingResponse& response : outgoing)
		{
			if (Connection* connection = FindConnection(response.ConnectionId))
				Guard(*connection, [&]() { DeliverResponse(*connection, response); });
		}

		// Write eagerly (waiting for writability is only needed once a send buffer is full), resume clients whose
		// backpressure has cleared, and enforce deadlines.
		const auto now = std::chrono::steady_clock::now();
		for (const Scope<Connection>& connection : Connections)
		{
			Guard(*connection, [&]()
			{
				if (!connection->Closed && connection->GetPendingOutput() > 0)
					Flush(*connection);
				ProcessBufferedLines(*connection);
				CheckDeadlines(*connection, now);
			});
		}
		RemoveClosedConnections();

		// Poll set: [0] notifier, [1] listener, [2...] connections in order.
		const bool hasNotifier = Shared->Notifier.IsValid();
		std::chrono::milliseconds timeout = hasNotifier ? c_IdlePollInterval : c_FallbackPollInterval;
		pollEntries.clear();
		pollEntries.push_back(SocketPollEntry { hasNotifier ? Shared->Notifier.GetHandle() : c_InvalidSocketHandle, true, false });
		pollEntries.push_back(SocketPollEntry { Listener.GetHandle(), true, false });
		for (const Scope<Connection>& connection : Connections)
		{
			pollEntries.push_back(SocketPollEntry { connection->Socket.GetHandle(), WantsInput(*connection), connection->GetPendingOutput() > 0 });

			std::optional<std::chrono::steady_clock::time_point> deadline;
			if (connection->Closing)
				deadline = connection->CloseDeadline;
			else if (!connection->Authenticated)
				deadline = connection->AuthenticationDeadline;
			if (deadline)
			{
				const auto untilDeadline = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
				timeout = std::clamp(untilDeadline + std::chrono::milliseconds(1), std::chrono::milliseconds(0), timeout);
			}
		}

		if (!SocketPoller::Poll(pollEntries, timeout))
			return false;

		if (pollEntries[0].Readable)
			Shared->Notifier.Drain();

		// New connections are accepted after servicing the polled ones, so indices stay aligned.
		const size_t polledConnections = pollEntries.size() - 2;
		for (size_t index = 0; index < polledConnections; index++)
		{
			Connection& connection = *Connections[index];
			const SocketPollEntry& entry = pollEntries[index + 2];
			Guard(connection, [&]()
			{
				if (entry.Readable && !connection.PeerFinished && !connection.Closed)
					ReadFrom(connection);
				if (entry.Writable && !connection.Closed)
					Flush(connection);
			});
		}

		// A listener that stays readable while accept() fails (e.g. out of file descriptors) would otherwise
		// turn this loop into a busy spin.
		if (pollEntries[1].Readable && AcceptConnections() == 0)
			std::this_thread::sleep_for(c_FallbackPollInterval);
		return true;
	}

	uint32_t RpcServer::Impl::AcceptConnections()
	{
		uint32_t accepted = 0;
		while (std::optional<TcpSocket> socket = Listener.Accept(std::chrono::milliseconds(0)))
		{
			accepted++;
			uint64_t suppressed = 0;
			const size_t trackedLimit = static_cast<size_t>(Specification.MaxClients) + Specification.MaxPendingConnections + c_MaxClosingConnections;
			if (Connections.size() >= trackedLimit)
			{
				if (RejectionWarnings.Allow(suppressed))
					ST_CORE_WARN("RpcServer: dropping a connection, {} connections are open{}", Connections.size(), DescribeSuppressed(suppressed));
				continue; // Closed when the socket goes out of scope
			}

			socket->SetNoDelay(true);
			Scope<Connection> connection = CreateScope<Connection>(c_PreAuthMaxMessageSize);
			connection->Id = NextConnectionId++;
			connection->Socket = std::move(*socket);
			connection->AuthenticationDeadline = std::chrono::steady_clock::now() + ClampSocketTimeout(Specification.AuthenticationTimeout);
			{
				std::scoped_lock<std::mutex> lock(Shared->Mutex);
				Shared->OpenConnections.insert(connection->Id);
			}

			if (CountConnections(false) >= Specification.MaxPendingConnections)
			{
				if (RejectionWarnings.Allow(suppressed))
					ST_CORE_WARN("RpcServer: rejecting a connection, {} connections are already waiting to authenticate{}", Specification.MaxPendingConnections, DescribeSuppressed(suppressed));
				Send(*connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::ServerBusy, "Too many connections are waiting to authenticate"));
				BeginClose(*connection, c_ErrorCloseLinger);
			}
			else
			{
				ST_CORE_TRACE("RpcServer: client {} connected", connection->Id);
			}
			Connections.push_back(std::move(connection));
		}

		UpdateClientCount();
		return accepted;
	}

	void RpcServer::Impl::ReadFrom(Connection& connection)
	{
		connection.ReceiveBuffer.clear();
		const SocketReceiveStatus status = connection.Socket.Receive(connection.ReceiveBuffer, std::chrono::milliseconds(0));
		if (status == SocketReceiveStatus::Timeout)
			return;
		if (status == SocketReceiveStatus::Error)
		{
			ST_CORE_TRACE("RpcServer: client {} connection failed", connection.Id);
			connection.Closed = true;
			return;
		}
		if (status == SocketReceiveStatus::Closed)
		{
			// The client finished sending. It may still be reading, so answer what it already asked for.
			ST_CORE_TRACE("RpcServer: client {} closed its side of the connection", connection.Id);
			connection.PeerFinished = true;
			BeginClose(connection, connection.Authenticated ? c_HalfCloseLinger : c_ErrorCloseLinger);
			return;
		}
		if (connection.Closing)
			return; // Input arriving while the connection closes is discarded

		connection.Reader.Append(connection.ReceiveBuffer);
		ProcessBufferedLines(connection);
	}

	void RpcServer::Impl::ProcessBufferedLines(Connection& connection)
	{
		while (!connection.Closing && !connection.Closed && CanProcessRequests(connection))
		{
			std::optional<std::string> line = connection.Reader.NextLine();
			if (!line)
				break;
			HandleMessage(connection, *line);
		}

		if (connection.Reader.HasError() && !connection.Closing && !connection.Closed)
		{
			uint64_t suppressed = 0;
			if (ProtocolWarnings.Allow(suppressed))
				ST_CORE_WARN("RpcServer: client {}: {}{}", connection.Id, connection.Reader.GetError(), DescribeSuppressed(suppressed));
			Send(connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::InvalidRequest, connection.Reader.GetError()));
			BeginClose(connection, c_ErrorCloseLinger);
		}
	}

	void RpcServer::Impl::HandleMessage(Connection& connection, const std::string& line)
	{
		std::optional<nlohmann::json> message = JsonRpc::Parse(line);
		if (!connection.Authenticated)
		{
			HandleFirstMessage(connection, message);
			return;
		}

		if (!message)
		{
			Send(connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::ParseError, "Parse error: the message is not valid JSON (or nests too deeply)"));
			return;
		}
		if (message->is_array())
		{
			Send(connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::InvalidRequest, "Batch requests are not supported"));
			return;
		}
		if (JsonRpc::IsResponse(*message))
			return; // The server never sends requests, so there is nothing to match a response against

		const JsonRpc::RequestValidation validation = JsonRpc::ValidateRequest(*message);
		if (!validation.Valid)
		{
			Send(connection, JsonRpc::MakeError(validation.Id, JsonRpc::ErrorCode::InvalidRequest, validation.Error));
			return;
		}

		const std::string method = (*message)["method"].get<std::string>();
		nlohmann::json params = nlohmann::json::object();
		if (auto paramsIt = message->find("params"); paramsIt != message->end())
			params = std::move(*paramsIt);

		auto reply = [&](const RpcResult& result)
		{
			if (!validation.IsNotification)
				Send(connection, MakeResponse(validation.Id, result));
		};

		if (method == c_AuthenticateMethod)
		{
			reply(CheckToken(params));
			return;
		}
		if (method == c_PingMethod)
		{
			reply(RpcResult::Success(nlohmann::json { { "pong", true } }));
			return;
		}
		if (method == c_ListMethodsMethod)
		{
			nlohmann::json methods = nlohmann::json::array();
			for (const RpcMethodInfo& info : GetMethods())
				methods.push_back(nlohmann::json { { "name", info.Name }, { "description", info.Description }, { "paramsSchema", info.ParamsSchema } });
			reply(RpcResult::Success(nlohmann::json { { "methods", std::move(methods) } }));
			return;
		}

		bool registered = false;
		if (!method.starts_with(c_ReservedMethodPrefix))
		{
			std::scoped_lock<std::mutex> lock(MethodsMutex);
			registered = Methods.contains(method);
		}
		if (!registered)
		{
			reply(RpcResult::Failure(JsonRpc::ErrorCode::MethodNotFound, fmt::format("Method '{}' not found", method)));
			return;
		}

		{
			std::scoped_lock<std::mutex> lock(QueueMutex);
			if (Queue.size() >= Specification.MaxQueuedRequests)
			{
				reply(RpcResult::Failure(JsonRpc::ErrorCode::ServerBusy, "Too many pending requests; the server is not keeping up"));
				return;
			}
			Queue.push_back(QueuedRequest { connection.Id, validation.Id, validation.IsNotification, method, std::move(params) });
		}
		if (!validation.IsNotification)
			connection.OutstandingRequests++;
	}

	void RpcServer::Impl::HandleFirstMessage(Connection& connection, const std::optional<nlohmann::json>& message)
	{
		// The first message must authenticate. Anything else ends the connection: the peer is either a confused
		// client or a hostile local process probing the port (e.g. a web page sending HTTP), and gets nothing more.
		auto reject = [&](const nlohmann::json& id, int code, std::string_view reason, bool reply)
		{
			uint64_t suppressed = 0;
			if (AuthenticationWarnings.Allow(suppressed))
				ST_CORE_WARN("RpcServer: closing unauthenticated client {}: {}{}", connection.Id, reason, DescribeSuppressed(suppressed));
			if (reply)
				Send(connection, JsonRpc::MakeError(id, code, reason));
			BeginClose(connection, c_ErrorCloseLinger);
		};

		if (!message)
		{
			reject(nullptr, JsonRpc::ErrorCode::ParseError, "Parse error: the message is not valid JSON", true);
			return;
		}

		const JsonRpc::RequestValidation validation = JsonRpc::ValidateRequest(*message);
		if (!validation.Valid)
		{
			reject(validation.Id, JsonRpc::ErrorCode::InvalidRequest, validation.Error, true);
			return;
		}
		if (validation.IsNotification)
		{
			reject(nullptr, 0, "a notification arrived before authentication", false);
			return;
		}

		const nlohmann::json& method = (*message)["method"];
		if (method != c_AuthenticateMethod)
		{
			reject(validation.Id, JsonRpc::ErrorCode::Unauthorized, "Authentication required: the first request must be rpc.authenticate with the session token", true);
			return;
		}

		const auto paramsIt = message->find("params");
		const RpcResult result = CheckToken(paramsIt != message->end() ? *paramsIt : nlohmann::json::object());
		if (result.IsError())
		{
			reject(validation.Id, result.GetError().Code, result.GetError().Message, true);
			return;
		}

		if (CountConnections(true) >= Specification.MaxClients)
		{
			uint64_t suppressed = 0;
			if (RejectionWarnings.Allow(suppressed))
				ST_CORE_WARN("RpcServer: rejecting client {}, the limit of {} clients is reached{}", connection.Id, Specification.MaxClients, DescribeSuppressed(suppressed));
			Send(connection, JsonRpc::MakeError(validation.Id, JsonRpc::ErrorCode::ServerBusy, fmt::format("The server accepts at most {} clients", Specification.MaxClients)));
			BeginClose(connection, c_ErrorCloseLinger);
			return;
		}

		connection.Authenticated = true;
		connection.Reader.SetMaxMessageSize(Specification.MaxMessageSize);
		UpdateClientCount();
		ST_CORE_TRACE("RpcServer: client {} authenticated", connection.Id);
		Send(connection, MakeResponse(validation.Id, result));
	}

	RpcResult RpcServer::Impl::CheckToken(const nlohmann::json& params) const
	{
		const auto token = params.is_object() ? params.find("token") : params.end();
		const auto nonce = params.is_object() ? params.find("nonce") : params.end();
		if (token == params.end() || !token->is_string() || nonce == params.end() || !nonce->is_string())
			return RpcResult::Failure(JsonRpc::ErrorCode::InvalidParams, "Expected params {\"token\": string, \"nonce\": string}");
		const std::string& nonceText = nonce->get_ref<const std::string&>();
		if (!RpcAuthentication::IsValidNonce(nonceText))
			return RpcResult::Failure(JsonRpc::ErrorCode::InvalidParams, "The nonce must be 32 to 128 hexadecimal characters");
		if (!Crypto::ConstantTimeEquals(token->get_ref<const std::string&>(), Specification.AuthToken))
			return RpcResult::Failure(JsonRpc::ErrorCode::Unauthorized, "Invalid authentication token");

		// Prove knowledge of the token in return, bound to the client's nonce so the proof cannot be replayed.
		return RpcResult::Success(nlohmann::json { { "authenticated", true }, { "proof", RpcAuthentication::ComputeServerProof(Specification.AuthToken, nonceText) } });
	}

	void RpcServer::Impl::DeliverResponse(Connection& connection, const OutgoingResponse& response)
	{
		if (connection.OutstandingRequests > 0)
			connection.OutstandingRequests--;

		std::string serialized = JsonRpc::Serialize(MakeResponse(response.Id, response.Result));
		if (serialized.size() > Specification.MaxMessageSize)
		{
			// The client could not read it (its reader enforces the same limit), so send an error instead.
			uint64_t suppressed = 0;
			if (ProtocolWarnings.Allow(suppressed))
				ST_CORE_WARN("RpcServer: a {} byte response exceeds the message size limit{}", serialized.size(), DescribeSuppressed(suppressed));
			serialized = JsonRpc::Serialize(JsonRpc::MakeError(response.Id, JsonRpc::ErrorCode::InternalError,
				fmt::format("The response ({} bytes) exceeds the maximum message size of {} bytes", serialized.size(), Specification.MaxMessageSize)));
		}
		SendSerialized(connection, std::move(serialized));
	}

	void RpcServer::Impl::Send(Connection& connection, const nlohmann::json& message)
	{
		SendSerialized(connection, JsonRpc::Serialize(message));
	}

	void RpcServer::Impl::SendSerialized(Connection& connection, std::string serialized)
	{
		if (connection.Closed)
			return;

		connection.SendBuffer += serialized;
		connection.SendBuffer += '\n';

		// Backpressure keeps a well-behaved client far below this; a client that pipelines requests without reading
		// the responses is dropped before its output can exhaust memory.
		const size_t limit = connection.Authenticated ? Specification.MaxMessageSize + c_OutputBackpressureThreshold : c_PreAuthMaxPendingOutput;
		if (connection.GetPendingOutput() > limit)
		{
			uint64_t suppressed = 0;
			if (ProtocolWarnings.Allow(suppressed))
				ST_CORE_WARN("RpcServer: client {} is not reading its responses; disconnecting{}", connection.Id, DescribeSuppressed(suppressed));
			connection.Closed = true;
		}
	}

	void RpcServer::Impl::Flush(Connection& connection)
	{
		while (connection.GetPendingOutput() > 0)
		{
			const std::span<const uint8_t> pending(reinterpret_cast<const uint8_t*>(connection.SendBuffer.data()) + connection.SendOffset, connection.GetPendingOutput());
			const std::optional<size_t> sent = connection.Socket.SendSome(pending);
			if (!sent)
			{
				ST_CORE_TRACE("RpcServer: client {} connection failed while sending", connection.Id);
				connection.Closed = true;
				return;
			}
			if (*sent == 0)
				break;
			connection.SendOffset += *sent;
		}

		if (connection.SendOffset == connection.SendBuffer.size())
		{
			connection.SendBuffer.clear();
			connection.SendOffset = 0;
		}
		else if (connection.SendOffset >= c_SendBufferCompactThreshold && connection.SendOffset >= connection.SendBuffer.size() / 2)
		{
			connection.SendBuffer.erase(0, connection.SendOffset);
			connection.SendOffset = 0;
		}
	}

	void RpcServer::Impl::CheckDeadlines(Connection& connection, std::chrono::steady_clock::time_point now)
	{
		if (connection.Closed)
			return;

		if (!connection.Authenticated && !connection.Closing && now >= connection.AuthenticationDeadline)
		{
			uint64_t suppressed = 0;
			if (AuthenticationWarnings.Allow(suppressed))
				ST_CORE_WARN("RpcServer: client {} did not authenticate in time{}", connection.Id, DescribeSuppressed(suppressed));
			Send(connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::Unauthorized, "Authentication timed out"));
			BeginClose(connection, c_ErrorCloseLinger);
		}

		if (!connection.Closing)
			return;
		if (now >= connection.CloseDeadline)
		{
			connection.Closed = true;
			return;
		}
		if (connection.GetPendingOutput() > 0 || connection.OutstandingRequests > 0)
			return;

		// Everything is sent: end our stream once, then wait for the client's end so the close is orderly.
		if (!connection.SentShutdown)
		{
			connection.Socket.ShutdownSend();
			connection.SentShutdown = true;
			connection.CloseDeadline = std::min(connection.CloseDeadline, now + c_DrainTimeout);
		}
		if (connection.PeerFinished)
			connection.Closed = true;
	}

	void RpcServer::Impl::BeginClose(Connection& connection, std::chrono::milliseconds linger)
	{
		if (connection.Closing)
			return;
		connection.Closing = true;
		connection.CloseDeadline = std::chrono::steady_clock::now() + linger;
		UpdateClientCount();
	}

	void RpcServer::Impl::RemoveClosedConnections()
	{
		std::vector<uint64_t> removed;
		for (const Scope<Connection>& connection : Connections)
		{
			if (connection->Closed)
				removed.push_back(connection->Id);
		}

		if (!removed.empty())
		{
			{
				std::scoped_lock<std::mutex> lock(Shared->Mutex);
				for (uint64_t id : removed)
					Shared->OpenConnections.erase(id);
			}
			for (uint64_t id : removed)
				ST_CORE_TRACE("RpcServer: client {} disconnected", id);

			std::erase_if(Connections, [](const Scope<Connection>& connection) { return connection->Closed; });
		}
		UpdateClientCount();
	}

	void RpcServer::Impl::UpdateClientCount()
	{
		ClientCount = static_cast<uint32_t>(CountConnections(true));
	}

	bool RpcServer::Impl::CanProcessRequests(const Connection& connection) const
	{
		if (!connection.Authenticated)
			return true; // Only the first message is ever handled, and it is size-limited
		return connection.GetPendingOutput() < c_OutputBackpressureThreshold && connection.OutstandingRequests < Specification.MaxRequestsInFlightPerClient;
	}

	bool RpcServer::Impl::WantsInput(const Connection& connection) const
	{
		if (connection.Closed || connection.PeerFinished)
			return false; // A finished peer stays readable (end of stream) forever
		if (connection.Closing)
			return true; // Drained and discarded so the final close is orderly
		return CanProcessRequests(connection);
	}

	size_t RpcServer::Impl::CountConnections(bool authenticated) const
	{
		return static_cast<size_t>(std::count_if(Connections.begin(), Connections.end(), [authenticated](const Scope<Connection>& connection)
		{
			return connection->Authenticated == authenticated && !connection->Closing && !connection->Closed;
		}));
	}

	Connection* RpcServer::Impl::FindConnection(uint64_t id)
	{
		for (const Scope<Connection>& connection : Connections)
		{
			if (connection->Id == id)
				return connection.get();
		}
		return nullptr;
	}

	////////////////////////////////////////////////////////////////////////////////
	// RpcServer
	////////////////////////////////////////////////////////////////////////////////

	RpcServer::RpcServer()
		: m_Impl(CreateScope<Impl>())
	{
	}

	RpcServer::~RpcServer()
	{
		Stop();
	}

	bool RpcServer::Start(const RpcServerSpecification& specification)
	{
		Stop();

		// Automation grants full control over the editor, so it is never exposed beyond this machine.
		if (!IsLoopbackAddress(specification.BindAddress))
		{
			ST_CORE_ERROR("RpcServer: refusing to listen on '{}': only loopback addresses (127.0.0.0/8 or ::1) are allowed", specification.BindAddress);
			return false;
		}
		if (specification.AuthToken.empty())
		{
			ST_CORE_ERROR("RpcServer: refusing to start without an authentication token");
			return false;
		}

		if (!m_Impl->Listener.Listen(specification.BindAddress, specification.Port))
		{
			ST_CORE_ERROR("RpcServer: {}", m_Impl->Listener.GetLastError());
			return false;
		}

		RpcServerSpecification& active = m_Impl->Specification;
		active = specification;
		active.MaxClients = std::max(specification.MaxClients, 1u);
		active.MaxPendingConnections = std::max(specification.MaxPendingConnections, 1u);
		active.MaxQueuedRequests = std::max(specification.MaxQueuedRequests, 1u);
		active.MaxRequestsInFlightPerClient = std::max(specification.MaxRequestsInFlightPerClient, 1u);
		active.MaxMessageSize = std::max(specification.MaxMessageSize, c_PreAuthMaxMessageSize);

		m_Impl->Shared = CreateRef<RpcServerShared>();
		if (specification.UseWakeupNotifier && !m_Impl->Shared->Notifier.Open())
			ST_CORE_WARN("RpcServer: wake-up notifier unavailable; polling every {} ms instead", c_FallbackPollInterval.count());
		m_Impl->Shared->Running = true;

		m_Impl->Port = m_Impl->Listener.GetPort();
		m_Impl->ClientCount = 0;
		m_Impl->Running = true;
		m_Impl->NetworkThread = std::thread([impl = m_Impl.get()]() { impl->RunNetworkThread(); });

		ST_CORE_INFO("RpcServer: listening on {}:{}", specification.BindAddress, m_Impl->Port.load());
		return true;
	}

	void RpcServer::Stop()
	{
		if (!m_Impl->NetworkThread.joinable())
			return;

		{
			std::scoped_lock<std::mutex> lock(m_Impl->Shared->Mutex);
			m_Impl->Shared->Running = false;
			m_Impl->Shared->Notifier.Notify();
		}
		m_Impl->NetworkThread.join();

		m_Impl->Shared->Notifier.Close();
		m_Impl->Listener.Close();
		{
			std::scoped_lock<std::mutex> lock(m_Impl->QueueMutex);
			m_Impl->Queue.clear();
		}

		m_Impl->Running = false;
		m_Impl->Port = 0;
		m_Impl->ClientCount = 0;
		ST_CORE_INFO("RpcServer: stopped");
	}

	bool RpcServer::IsRunning() const
	{
		return m_Impl->Running.load();
	}

	uint16_t RpcServer::GetPort() const
	{
		return m_Impl->Port.load();
	}

	uint32_t RpcServer::GetClientCount() const
	{
		return m_Impl->ClientCount.load();
	}

	bool RpcServer::RegisterMethod(RpcMethodInfo info, RpcHandler handler)
	{
		if (info.Name.empty() || info.Name.starts_with(c_ReservedMethodPrefix))
		{
			ST_CORE_ERROR("RpcServer: invalid method name '{}' (names must be non-empty and not start with 'rpc.')", info.Name);
			return false;
		}
		if (!handler)
		{
			ST_CORE_ERROR("RpcServer: method '{}' has no handler", info.Name);
			return false;
		}

		info.ParamsSchema = NormalizeParamsSchema(std::move(info.ParamsSchema));

		std::scoped_lock<std::mutex> lock(m_Impl->MethodsMutex);
		if (m_Impl->Methods.contains(info.Name))
		{
			ST_CORE_ERROR("RpcServer: method '{}' is already registered", info.Name);
			return false;
		}

		const std::string name = info.Name;
		m_Impl->Methods.emplace(name, RegisteredMethod { std::move(info), std::move(handler) });
		return true;
	}

	bool RpcServer::RegisterMethod(RpcMethodInfo info, RpcSyncHandler handler)
	{
		if (!handler)
		{
			ST_CORE_ERROR("RpcServer: method '{}' has no handler", info.Name);
			return false;
		}

		return RegisterMethod(std::move(info), RpcHandler([handler = std::move(handler)](const nlohmann::json& params, const Ref<RpcResponder>& responder)
		{
			responder->Respond(handler(params));
		}));
	}

	void RpcServer::UnregisterMethod(const std::string& name)
	{
		std::unique_lock<std::mutex> lock(m_Impl->MethodsMutex);
		m_Impl->Methods.erase(name);

		// ProcessRequests runs a copy of the handler. Wait for it to return so the caller may destroy whatever the
		// handler captured. From inside a handler (on the processing thread) there is nothing to wait for.
		if (m_Impl->ProcessingThread != std::this_thread::get_id())
			m_Impl->MethodsCondition.wait(lock, [&]() { return m_Impl->RunningMethod != name; });
	}

	std::vector<RpcMethodInfo> RpcServer::GetMethods() const
	{
		return m_Impl->GetMethods();
	}

	uint32_t RpcServer::ProcessRequests()
	{
		ST_PROFILE_FUNCTION();

		std::vector<QueuedRequest> requests;
		{
			std::scoped_lock<std::mutex> lock(m_Impl->QueueMutex);
			requests.swap(m_Impl->Queue);
		}
		if (requests.empty() || !m_Impl->Shared)
			return 0;

		const WeakRef<RpcServerShared> weakShared = m_Impl->Shared;
		for (QueuedRequest& request : requests)
		{
			RpcHandler handler;
			{
				std::scoped_lock<std::mutex> lock(m_Impl->MethodsMutex);
				// Looked up again here: the method may have been unregistered since the request was queued, and its
				// handler's captures may no longer be valid.
				if (auto it = m_Impl->Methods.find(request.Method); it != m_Impl->Methods.end())
				{
					handler = it->second.Handler;
					m_Impl->RunningMethod = request.Method;
					m_Impl->ProcessingThread = std::this_thread::get_id();
				}
			}

			RpcResponder::DeliveryFunction deliver = [weakShared, connectionId = request.ConnectionId, id = request.Id, isNotification = request.IsNotification](RpcResult result)
			{
				if (isNotification)
					return;
				if (Ref<RpcServerShared> shared = weakShared.lock())
					shared->Deliver(connectionId, id, std::move(result));
			};
			Ref<RpcResponder> responder = CreateRef<RpcResponder>(request.Method, std::move(deliver));

			if (!handler)
			{
				responder->Respond(RpcResult::Failure(JsonRpc::ErrorCode::MethodNotFound, fmt::format("Method '{}' not found", request.Method)));
				continue;
			}

			// Handlers commonly read params with nlohmann::json accessors that throw on malformed input; contain
			// that here so one bad request cannot take down the main loop.
			try
			{
				handler(request.Params, responder);
			}
			catch (const std::exception& exception)
			{
				if (!responder->TryRespond(RpcResult::Failure(JsonRpc::ErrorCode::InternalError, fmt::format("Method '{}' failed: {}", request.Method, exception.what()))))
					ST_CORE_ERROR("RpcServer: method '{}' threw after responding: {}", request.Method, exception.what());
			}
			catch (...)
			{
				if (!responder->TryRespond(RpcResult::Failure(JsonRpc::ErrorCode::InternalError, fmt::format("Method '{}' failed with an unknown exception", request.Method))))
					ST_CORE_ERROR("RpcServer: method '{}' threw after responding", request.Method);
			}

			// Release the handler copy before signalling, so UnregisterMethod callers may destroy its captures.
			handler = nullptr;
			{
				std::scoped_lock<std::mutex> lock(m_Impl->MethodsMutex);
				m_Impl->RunningMethod.clear();
				m_Impl->ProcessingThread = std::thread::id();
			}
			m_Impl->MethodsCondition.notify_all();
		}
		return static_cast<uint32_t>(requests.size());
	}

}
