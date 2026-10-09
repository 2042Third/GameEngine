#include "stpch.h"
#include "Strata/Network/RpcServer.h"

#include "Strata/Core/Platform.h"
#include "Strata/Network/Socket.h"

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
		// A client that stops reading while responses pile up is disconnected beyond this much pending output.
		constexpr size_t c_MaxPendingOutputBytes = 256ull * 1024 * 1024;
		// Requests waiting for ProcessRequests beyond this count are rejected (the main thread is not keeping up).
		constexpr size_t c_MaxQueuedRequests = 4096;
		// Consumed output is discarded once it exceeds this size and half of the send buffer.
		constexpr size_t c_SendBufferCompactThreshold = 1024 * 1024;

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
			authenticate.Description = "Authenticates this connection with the session token. Required before any other request when the server has a token.";
			authenticate.ParamsSchema = nlohmann::json {
				{ "type", "object" },
				{ "properties", { { "token", { { "type", "string" }, { "description", "Session token from the editor session file" } } } } },
				{ "required", nlohmann::json::array({ "token" }) }
			};

			RpcMethodInfo& ping = methods.emplace_back();
			ping.Name = c_PingMethod;
			ping.Description = "Checks that the server is responsive. Returns {\"pong\": true}.";

			RpcMethodInfo& listMethods = methods.emplace_back();
			listMethods.Name = c_ListMethodsMethod;
			listMethods.Description = "Lists every available method with its description and JSON Schema of its parameters.";
			return methods;
		}

		// Compares secrets without an early exit, so response timing does not reveal how much of a guess matched.
		bool ConstantTimeEquals(std::string_view left, std::string_view right)
		{
			if (left.size() != right.size())
				return false;

			uint8_t difference = 0;
			for (size_t index = 0; index < left.size(); index++)
				difference = static_cast<uint8_t>(difference | static_cast<uint8_t>(left[index] ^ right[index]));
			return difference == 0;
		}

		nlohmann::json MakeResponse(const nlohmann::json& id, const RpcResult& result)
		{
			if (result.IsSuccess())
				return JsonRpc::MakeResult(id, result.GetValue());

			const RpcError& error = result.GetError();
			return JsonRpc::MakeError(id, error.Code, error.Message, error.Data);
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
		// allocation failure, and there is nothing better to do than drop the response.
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

		if (m_Deliver)
			m_Deliver(std::move(result));
		return true;
	}

	void RpcResponder::Respond(RpcResult result)
	{
		if (!TryRespond(std::move(result)))
			ST_CORE_WARN("RpcResponder: '{}' already responded; ignoring the second response", m_Method);
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
			uint32_t OutstandingRequests = 0; // Queued requests whose response has not arrived yet

			// A closing connection processes no more requests and is closed once its responses are flushed (or the
			// deadline passes). Its input is still read and discarded, so the final close is orderly instead of a
			// reset that could destroy the last responses before the client reads them. Closed connections are
			// removed at the end of the loop iteration.
			bool PeerFinished = false; // The client closed its sending side
			bool Closing = false;
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
		std::map<std::string, RegisteredMethod> Methods;

		std::mutex QueueMutex;
		std::vector<QueuedRequest> Queue;

		// Network thread state
		std::vector<Scope<Connection>> Connections;
		uint64_t NextConnectionId = 1;

		std::vector<RpcMethodInfo> GetMethods() const;

		void RunNetworkThread();
		uint32_t AcceptConnections();
		void ReadFrom(Connection& connection);
		void HandleMessage(Connection& connection, const std::string& line);
		RpcResult Authenticate(Connection& connection, const nlohmann::json& params);
		void Send(Connection& connection, const nlohmann::json& message);
		void Flush(Connection& connection);
		void BeginClose(Connection& connection, std::chrono::milliseconds linger);
		void RemoveClosedConnections();
		void UpdateClientCount();
		Connection* FindConnection(uint64_t id);
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
		bool pollFailureReported = false;
		while (true)
		{
			{
				std::scoped_lock<std::mutex> lock(Shared->Mutex);
				if (!Shared->Running)
					break;
				outgoing.swap(Shared->Outgoing);
			}

			for (OutgoingResponse& response : outgoing)
			{
				Connection* connection = FindConnection(response.ConnectionId);
				if (!connection)
					continue;
				if (connection->OutstandingRequests > 0)
					connection->OutstandingRequests--;
				Send(*connection, MakeResponse(response.Id, response.Result));
			}
			outgoing.clear();

			// Write eagerly; waiting for writability is only needed once a socket's send buffer is full.
			for (const Scope<Connection>& connection : Connections)
			{
				if (!connection->Closed && connection->GetPendingOutput() > 0)
					Flush(*connection);
			}
			RemoveClosedConnections();

			// Poll set: [0] notifier, [1] listener, [2...] connections in order.
			const bool hasNotifier = Shared->Notifier.IsValid();
			std::chrono::milliseconds timeout = hasNotifier ? c_IdlePollInterval : c_FallbackPollInterval;
			pollEntries.clear();
			pollEntries.push_back(SocketPollEntry { hasNotifier ? Shared->Notifier.GetHandle() : c_InvalidSocketHandle, true, false });
			pollEntries.push_back(SocketPollEntry { Listener.GetHandle(), true, false });
			const auto now = std::chrono::steady_clock::now();
			for (const Scope<Connection>& connection : Connections)
			{
				// A finished peer stays readable (end of stream) forever, so it is only polled for writing.
				pollEntries.push_back(SocketPollEntry { connection->Socket.GetHandle(), !connection->PeerFinished, connection->GetPendingOutput() > 0 });
				if (connection->Closing)
				{
					const auto untilDeadline = std::chrono::duration_cast<std::chrono::milliseconds>(connection->CloseDeadline - now);
					timeout = std::clamp(untilDeadline + std::chrono::milliseconds(1), std::chrono::milliseconds(0), timeout);
				}
			}

			if (!SocketPoller::Poll(pollEntries, timeout))
			{
				if (!pollFailureReported)
					ST_CORE_ERROR("RpcServer: waiting for socket events failed");
				pollFailureReported = true;
				std::this_thread::sleep_for(c_FallbackPollInterval);
				continue;
			}
			pollFailureReported = false;

			if (pollEntries[0].Readable)
				Shared->Notifier.Drain();

			// New connections are accepted after servicing the polled ones, so indices stay aligned.
			const size_t polledConnections = pollEntries.size() - 2;
			for (size_t index = 0; index < polledConnections; index++)
			{
				Connection& connection = *Connections[index];
				const SocketPollEntry& entry = pollEntries[index + 2];
				if (entry.Readable && !connection.PeerFinished && !connection.Closed)
					ReadFrom(connection);
				if (entry.Writable && !connection.Closed)
					Flush(connection);
			}

			// A listener that stays readable while accept() fails (e.g. out of file descriptors) would otherwise
			// turn this loop into a busy spin.
			if (pollEntries[1].Readable && AcceptConnections() == 0)
				std::this_thread::sleep_for(c_FallbackPollInterval);
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

	uint32_t RpcServer::Impl::AcceptConnections()
	{
		uint32_t accepted = 0;
		while (std::optional<TcpSocket> socket = Listener.Accept(std::chrono::milliseconds(0)))
		{
			accepted++;
			socket->SetNoDelay(true);

			Scope<Connection> connection = CreateScope<Connection>(Specification.MaxMessageSize);
			connection->Id = NextConnectionId++;
			connection->Socket = std::move(*socket);
			connection->Authenticated = Specification.AuthToken.empty();
			{
				std::scoped_lock<std::mutex> lock(Shared->Mutex);
				Shared->OpenConnections.insert(connection->Id);
			}

			const uint32_t activeConnections = static_cast<uint32_t>(std::count_if(Connections.begin(), Connections.end(), [](const Scope<Connection>& existing) { return !existing->Closing; }));
			if (activeConnections >= Specification.MaxClients)
			{
				ST_CORE_WARN("RpcServer: rejecting a connection, the limit of {} clients is reached", Specification.MaxClients);
				Send(*connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::ServerBusy, fmt::format("The server accepts at most {} clients", Specification.MaxClients)));
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

	void RpcServer::Impl::UpdateClientCount()
	{
		ClientCount = static_cast<uint32_t>(std::count_if(Connections.begin(), Connections.end(), [](const Scope<Connection>& connection) { return !connection->Closing && !connection->Closed; }));
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
			BeginClose(connection, c_HalfCloseLinger);
			return;
		}
		if (connection.Closing)
			return; // Input arriving while the connection closes is discarded

		connection.Reader.Append(connection.ReceiveBuffer);
		while (!connection.Closing && !connection.Closed)
		{
			std::optional<std::string> line = connection.Reader.NextLine();
			if (!line)
				break;
			HandleMessage(connection, *line);
		}

		if (connection.Reader.HasError() && !connection.Closing)
		{
			ST_CORE_WARN("RpcServer: client {}: {}", connection.Id, connection.Reader.GetError());
			Send(connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::InvalidRequest, connection.Reader.GetError()));
			BeginClose(connection, c_ErrorCloseLinger);
		}
	}

	void RpcServer::Impl::HandleMessage(Connection& connection, const std::string& line)
	{
		std::optional<nlohmann::json> message = JsonRpc::Parse(line);
		if (!message)
		{
			Send(connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::ParseError, "Parse error: the message is not valid JSON"));
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
			reply(Authenticate(connection, params));
			return;
		}
		if (!connection.Authenticated)
		{
			reply(RpcResult::Failure(JsonRpc::ErrorCode::Unauthorized, "Authentication required: call rpc.authenticate with the session token first"));
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
			if (Queue.size() >= c_MaxQueuedRequests)
			{
				reply(RpcResult::Failure(JsonRpc::ErrorCode::ServerBusy, "Too many pending requests; the server is not keeping up"));
				return;
			}
			Queue.push_back(QueuedRequest { connection.Id, validation.Id, validation.IsNotification, method, std::move(params) });
		}
		if (!validation.IsNotification)
			connection.OutstandingRequests++;
	}

	RpcResult RpcServer::Impl::Authenticate(Connection& connection, const nlohmann::json& params)
	{
		if (Specification.AuthToken.empty())
		{
			connection.Authenticated = true;
			return RpcResult::Success(nlohmann::json { { "authenticated", true } });
		}

		const auto token = params.is_object() ? params.find("token") : params.end();
		if (token == params.end() || !token->is_string())
			return RpcResult::Failure(JsonRpc::ErrorCode::InvalidParams, "Expected params {\"token\": string}");

		if (!ConstantTimeEquals(token->get_ref<const std::string&>(), Specification.AuthToken))
		{
			ST_CORE_WARN("RpcServer: client {} sent an invalid authentication token", connection.Id);
			return RpcResult::Failure(JsonRpc::ErrorCode::Unauthorized, "Invalid authentication token");
		}

		connection.Authenticated = true;
		return RpcResult::Success(nlohmann::json { { "authenticated", true } });
	}

	void RpcServer::Impl::Send(Connection& connection, const nlohmann::json& message)
	{
		if (connection.Closed)
			return;

		connection.SendBuffer += JsonRpc::Serialize(message);
		connection.SendBuffer += '\n';
		if (connection.GetPendingOutput() > c_MaxPendingOutputBytes)
		{
			ST_CORE_WARN("RpcServer: client {} is not reading its responses; disconnecting", connection.Id);
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

	void RpcServer::Impl::BeginClose(Connection& connection, std::chrono::milliseconds linger)
	{
		if (connection.Closing)
			return;
		connection.Closing = true;
		connection.CloseDeadline = std::chrono::steady_clock::now() + linger;
	}

	void RpcServer::Impl::RemoveClosedConnections()
	{
		const auto now = std::chrono::steady_clock::now();
		std::vector<uint64_t> removed;
		for (const Scope<Connection>& connection : Connections)
		{
			if (connection->Closing && !connection->Closed)
			{
				const bool finished = connection->GetPendingOutput() == 0 && connection->OutstandingRequests == 0;
				if (finished || now >= connection->CloseDeadline)
					connection->Closed = true;
			}
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

		if (!m_Impl->Listener.Listen(specification.BindAddress, specification.Port))
		{
			ST_CORE_ERROR("RpcServer: {}", m_Impl->Listener.GetLastError());
			return false;
		}

		m_Impl->Specification = specification;
		m_Impl->Specification.MaxClients = std::max(specification.MaxClients, 1u);
		m_Impl->Shared = CreateRef<RpcServerShared>();
		if (!m_Impl->Shared->Notifier.Open())
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
		std::scoped_lock<std::mutex> lock(m_Impl->MethodsMutex);
		m_Impl->Methods.erase(name);
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
					handler = it->second.Handler;
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
		}
		return static_cast<uint32_t>(requests.size());
	}

}
