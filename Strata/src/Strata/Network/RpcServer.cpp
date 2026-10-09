#include "stpch.h"
#include "Strata/Network/RpcServer.h"

#include "Strata/Core/Crypto.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/RpcConnectionLimits.h"
#include "Strata/Network/Socket.h"

#include <condition_variable>
#include <deque>
#include <map>

namespace Strata
{

	namespace
	{

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

		// Limits before authentication: the only useful messages are the two small handshake requests, and the only
		// output their answers or an error.
		constexpr size_t c_PreAuthMaxMessageSize = 4 * 1024;
		constexpr size_t c_PreAuthMaxPendingOutput = 16 * 1024;
		// While a client has this much output waiting, no further requests of it are read (backpressure).
		constexpr size_t c_OutputBackpressureThreshold = 1024 * 1024;
		// Consumed output is discarded once it exceeds this size and half of the send buffer.
		constexpr size_t c_SendBufferCompactThreshold = 1024 * 1024;
		// Connections tracked beyond MaxClients + MaxPendingConnections (those being closed). Beyond it the oldest
		// closing connection is dropped, so a connection flood cannot exhaust file descriptors or memory.
		constexpr size_t c_MaxClosingConnections = 16;
		// Repeated warnings (e.g. from a port scanner or a hostile process) are logged at most this often.
		constexpr std::chrono::seconds c_WarningInterval = std::chrono::seconds(5);
		// Request ids are echoed in every reply; longer ones are refused rather than amplified.
		constexpr size_t c_MaxIdLength = 256;
		// Method names echoed in error messages are cut to this length.
		constexpr size_t c_MaxQuotedMethodLength = 64;

		// A method name for messages and logs: quoted, and shortened so a huge name cannot inflate a reply.
		std::string QuoteMethod(std::string_view method)
		{
			if (method.size() <= c_MaxQuotedMethodLength)
				return fmt::format("'{}'", method);
			return fmt::format("'{}...'", method.substr(0, c_MaxQuotedMethodLength));
		}

		bool IsIdTooLong(const nlohmann::json& id)
		{
			// A string is measured directly: serializing it first would copy a huge id just to measure it.
			if (id.is_string())
				return id.get_ref<const std::string&>().size() > c_MaxIdLength;
			return JsonRpc::Serialize(id).size() > c_MaxIdLength;
		}

		nlohmann::json MakeEmptyObjectSchema()
		{
			return nlohmann::json { { "type", "object" }, { "properties", nlohmann::json::object() } };
		}

		bool IsValidMethod(const RpcMethodInfo& info, const RpcHandler& handler)
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
			return true;
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

			RpcMethodInfo& handshake = methods.emplace_back();
			handshake.Name = c_RpcHandshakeMethod;
			handshake.Description = "First step of the authentication handshake, and the first request of every connection: sends a "
				"fresh client nonce and returns a server nonce with proof that the server knows the session token (see RpcAuthentication).";
			handshake.ParamsSchema = nlohmann::json {
				{ "type", "object" },
				{ "properties", {
					{ "clientNonce", { { "type", "string" }, { "description", "Fresh random value: 32 lower-case hexadecimal characters" } } } } },
				{ "required", nlohmann::json::array({ "clientNonce" }) }
			};

			RpcMethodInfo& authenticate = methods.emplace_back();
			authenticate.Name = c_RpcAuthenticateMethod;
			authenticate.Description = "Second step of the authentication handshake: proves that the client knows the session token, "
				"without sending it. Returns {\"authenticated\": true}.";
			authenticate.ParamsSchema = nlohmann::json {
				{ "type", "object" },
				{ "properties", {
					{ "clientProof", { { "type", "string" }, { "description", "HMAC-SHA256 of the nonces with the session token, in hexadecimal" } } } } },
				{ "required", nlohmann::json::array({ "clientProof" }) }
			};

			RpcMethodInfo& ping = methods.emplace_back();
			ping.Name = c_PingMethod;
			ping.Description = "Checks that the server is responsive. Returns {\"pong\": true}.";

			RpcMethodInfo& listMethods = methods.emplace_back();
			listMethods.Name = c_ListMethodsMethod;
			listMethods.Description = "Lists every available method with its description and JSON Schema of its parameters.";
			return methods;
		}

		// The labels keep a proof of one side from ever being valid for the other.
		constexpr std::string_view c_ServerProofLabel = "strata-server";
		constexpr std::string_view c_ClientProofLabel = "strata-client";
		constexpr size_t c_NonceBytes = 16;

		// The string value of params[key], or nullptr if params is not an object or the value is not a string.
		const std::string* FindString(const nlohmann::json& params, const char* key)
		{
			if (!params.is_object())
				return nullptr;
			const auto it = params.find(key);
			return it != params.end() && it->is_string() ? &it->get_ref<const std::string&>() : nullptr;
		}

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
			bool IsNotification = false; // Only marks the end of a notification's handling; nothing is sent
		};

		struct QueuedRequest
		{
			uint64_t ConnectionId = 0;
			nlohmann::json Id;
			bool IsNotification = false;
			std::string Method;
			nlohmann::json Params;
			size_t Size = 0; // Size of the request line, for the queue's byte budget
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
				m_Deliver(RpcResult::Failure(JsonRpc::ErrorCode::InternalError, fmt::format("request dropped: {} finished without responding", QuoteMethod(m_Method))));
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
			ST_CORE_ERROR("RpcResponder: failed to deliver the response to {}: {}", QuoteMethod(m_Method), exception.what());
		}
		return true;
	}

	void RpcResponder::Respond(RpcResult result)
	{
		if (!TryRespond(std::move(result)))
			ST_CORE_WARN("RpcResponder: {} already responded; ignoring the second response", QuoteMethod(m_Method));
	}

	////////////////////////////////////////////////////////////////////////////////
	// RpcAuthentication
	////////////////////////////////////////////////////////////////////////////////

	std::string RpcAuthentication::GenerateNonce()
	{
		std::array<uint8_t, c_NonceBytes> bytes = {};
		if (!Platform::GenerateSecureRandom(bytes))
			return {};
		return Crypto::ToHex(bytes);
	}

	bool RpcAuthentication::IsValidNonce(std::string_view nonce)
	{
		// One canonical spelling: with fixed-length nonces, the concatenations in the proofs are unambiguous.
		if (nonce.size() != c_NonceBytes * 2)
			return false;
		return std::all_of(nonce.begin(), nonce.end(), [](char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
		});
	}

	std::string RpcAuthentication::ComputeServerProof(std::string_view token, std::string_view clientNonce, std::string_view serverNonce)
	{
		std::string message(c_ServerProofLabel);
		message += clientNonce;
		message += serverNonce;
		return Crypto::ToHex(Crypto::HmacSha256(token, message));
	}

	std::string RpcAuthentication::ComputeClientProof(std::string_view token, std::string_view serverNonce, std::string_view clientNonce)
	{
		std::string message(c_ClientProofLabel);
		message += serverNonce;
		message += clientNonce;
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
			// Set by a graceful Stop: the network thread delivers what is owed, then ends by itself (see Stop).
			bool Draining = false;
			std::chrono::steady_clock::time_point DrainDeadline;
			std::unordered_set<uint64_t> OpenConnections;
			std::vector<OutgoingResponse> Outgoing;
			// Wakes the network thread. Notify() is only called under Mutex while Running, so Stop() can close
			// the notifier safely once it has cleared Running.
			SocketNotifier Notifier;

			void Deliver(uint64_t connectionId, nlohmann::json id, RpcResult result, bool isNotification)
			{
				std::scoped_lock<std::mutex> lock(Mutex);
				if (!Running || !OpenConnections.contains(connectionId))
					return;

				Outgoing.push_back(OutgoingResponse { connectionId, std::move(id), std::move(result), isNotification });
				Notifier.Notify();
			}

			// Wakes the network thread, e.g. once the main thread has drained the queue it was waiting on.
			void Wake()
			{
				std::scoped_lock<std::mutex> lock(Mutex);
				if (Running)
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
			// Set by the first handshake step; the second step must prove the token against both.
			std::string ClientNonce;
			std::string ServerNonce;
			// Whether the connection has been through a poll since it was accepted, i.e. had a chance to send. Until
			// then it is never evicted to make room for newer connections.
			bool Polled = false;

			// Requests and notifications admitted for this client that are queued, being handled, or answered but
			// not yet moved into SendBuffer (backpressure counts all of them).
			uint32_t RequestsInFlight = 0;
			// Answers waiting for room in SendBuffer: they are serialized only when the socket keeps up, so the
			// memory a slow client can hold is bounded by its requests in flight.
			std::deque<OutgoingResponse> PendingResponses;
			// When the socket last accepted output (or output started waiting), to detect clients that stall.
			std::chrono::steady_clock::time_point LastSendProgress;

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
		std::string LastError; // Why Start failed (owning thread)
		std::atomic<uint32_t> ClientCount = 0;

		mutable std::mutex MethodsMutex;
		std::condition_variable MethodsCondition;
		std::map<std::string, RegisteredMethod> Methods;
		std::string RunningMethod;        // Handler executing in ProcessRequests (guarded by MethodsMutex)
		std::thread::id ProcessingThread; // The thread executing it

		std::mutex QueueMutex;
		std::vector<QueuedRequest> Queue;
		std::atomic<size_t> QueuedBytes = 0;
		// Set when the network thread held back requests because of the byte budget; ProcessRequests then wakes it.
		std::atomic<bool> QueueBackpressured = false;

		// Network thread state
		std::vector<Scope<Connection>> Connections;
		uint64_t NextConnectionId = 1;
		bool DrainStarted = false; // A graceful Stop is delivering the last answers (see BeginDrain)
		std::chrono::steady_clock::time_point DrainDeadline;
		WarningLimiter AuthenticationWarnings;
		WarningLimiter RejectionWarnings;
		WarningLimiter ProtocolWarnings;
		WarningLimiter ErrorWarnings;

		std::vector<RpcMethodInfo> GetMethods() const;

		void RunNetworkThread();
		bool RunIteration(std::vector<OutgoingResponse>& outgoing, std::vector<SocketPollEntry>& pollEntries);
		void BeginDrain(std::chrono::steady_clock::time_point deadline);
		void CancelQueuedRequests(std::vector<OutgoingResponse>& outgoing);
		uint32_t AcceptConnections();
		void ReadFrom(Connection& connection);
		void ProcessBufferedLines(Connection& connection);
		void CatchUpPendingConnections();
		void HandleMessage(Connection& connection, const std::string& line);
		void HandleHandshakeMessage(Connection& connection, const std::optional<nlohmann::json>& message);
		void TransferResponses(Connection& connection);
		std::string SerializeBounded(const nlohmann::json& message);
		void Send(Connection& connection, const nlohmann::json& message);
		void SendSerialized(Connection& connection, std::string serialized);
		void Flush(Connection& connection);
		void CheckDeadlines(Connection& connection, std::chrono::steady_clock::time_point now);
		void BeginClose(Connection& connection, std::chrono::milliseconds linger);
		void RemoveClosedConnections();
		void UpdateClientCount();
		bool CanProcessRequests(const Connection& connection);
		bool HasQueueRoom();
		bool WantsInput(const Connection& connection);
		size_t CountConnections(bool authenticated) const;
		Connection* FindConnection(uint64_t id);

		// The first open connection (in order of acceptance) that matches.
		template<typename Predicate>
		Connection* FindOldestConnection(Predicate&& predicate)
		{
			for (const Scope<Connection>& connection : Connections)
			{
				if (!connection->Closed && predicate(*connection))
					return connection.get();
			}
			return nullptr;
		}

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

	std::optional<size_t> ChoosePendingConnectionToEvict(std::span<const PendingConnectionState> pending)
	{
		std::optional<size_t> oldestStarted;
		for (size_t index = 0; index < pending.size(); index++)
		{
			if (!pending[index].Polled)
				continue; // Never evicted before it had a chance to send
			if (!pending[index].HandshakeStarted)
				return index;
			if (!oldestStarted)
				oldestStarted = index;
		}
		return oldestStarted;
	}

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
			std::optional<std::chrono::steady_clock::time_point> startDrain;
			{
				std::scoped_lock<std::mutex> lock(Shared->Mutex);
				if (!Shared->Running)
					break;
				if (Shared->Draining && !DrainStarted)
					startDrain = Shared->DrainDeadline;
				outgoing.swap(Shared->Outgoing);
			}

			// Per-connection work is guarded individually; this catches what remains (e.g. an allocation failure
			// while building the poll set) so the thread survives instead of calling std::terminate.
			bool succeeded = false;
			try
			{
				// Once the connections are closing, no further requests are read, so the queue cannot grow again.
				if (startDrain)
				{
					BeginDrain(*startDrain);
					CancelQueuedRequests(outgoing);
				}
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

			// A draining server ends once every client has its answers and has closed (connections that finish
			// closing are removed by RunIteration), or when the grace period is over.
			if (DrainStarted && (Connections.empty() || std::chrono::steady_clock::now() >= DrainDeadline))
				break;

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
		DrainStarted = false;
		{
			std::scoped_lock<std::mutex> lock(Shared->Mutex);
			Shared->OpenConnections.clear();
			Shared->Outgoing.clear();
		}
		ClientCount = 0;
	}

	bool RpcServer::Impl::RunIteration(std::vector<OutgoingResponse>& outgoing, std::vector<SocketPollEntry>& pollEntries)
	{
		for (OutgoingResponse& response : outgoing)
		{
			Connection* connection = FindConnection(response.ConnectionId);
			if (!connection)
				continue;
			Guard(*connection, [&]()
			{
				if (response.IsNotification)
				{
					if (connection->RequestsInFlight > 0)
						connection->RequestsInFlight--;
					return;
				}
				connection->PendingResponses.push_back(std::move(response));
			});
		}

		// Move answers into send buffers that have room and write eagerly (waiting for writability is only needed
		// once a socket's buffer is full), resume clients whose backpressure has cleared, and enforce deadlines.
		const auto now = std::chrono::steady_clock::now();
		for (const Scope<Connection>& connection : Connections)
		{
			Guard(*connection, [&]()
			{
				TransferResponses(*connection);
				if (!connection->Closed && connection->GetPendingOutput() > 0)
					Flush(*connection);
				TransferResponses(*connection);
				ProcessBufferedLines(*connection);
				CheckDeadlines(*connection, now);
			});
		}
		RemoveClosedConnections();
		// A draining server is done once its last connection is gone; waiting for socket events would only delay Stop.
		if (DrainStarted && Connections.empty())
			return true;

		// Poll set: [0] notifier, [1] listener, [2...] connections in order.
		const bool hasNotifier = Shared->Notifier.IsValid();
		std::chrono::milliseconds timeout = hasNotifier ? c_IdlePollInterval : c_FallbackPollInterval;
		if (DrainStarted)
		{
			const auto untilDrainDeadline = std::chrono::duration_cast<std::chrono::milliseconds>(DrainDeadline - now);
			timeout = std::clamp(untilDrainDeadline + std::chrono::milliseconds(1), std::chrono::milliseconds(0), timeout);
		}
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
			if (connection->GetPendingOutput() > 0)
			{
				const auto stallDeadline = connection->LastSendProgress + Specification.StalledClientTimeout;
				deadline = deadline ? std::min(*deadline, stallDeadline) : stallDeadline;
			}
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
			connection.Polled = true;
			Guard(connection, [&]()
			{
				if (entry.Readable && !connection.PeerFinished && !connection.Closed)
					ReadFrom(connection);
				if (entry.Writable && !connection.Closed)
				{
					Flush(connection);
					TransferResponses(connection);
				}
			});
		}

		// A listener that stays readable while accept() fails (e.g. out of file descriptors) would otherwise
		// turn this loop into a busy spin.
		if (pollEntries[1].Readable && AcceptConnections() == 0)
			std::this_thread::sleep_for(c_FallbackPollInterval);
		return true;
	}

	void RpcServer::Impl::BeginDrain(std::chrono::steady_clock::time_point deadline)
	{
		DrainStarted = true;
		DrainDeadline = deadline;
		// New clients are refused at once instead of waiting in the backlog of a server that is going away.
		Listener.Close();

		const auto now = std::chrono::steady_clock::now();
		const auto linger = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), std::chrono::milliseconds(0));
		for (const Scope<Connection>& connection : Connections)
		{
			if (connection->Closed)
				continue;
			// Nothing is owed to a connection that has not authenticated.
			if (!connection->Authenticated)
			{
				connection->Closed = true;
				continue;
			}
			// A closing connection stops reading requests and closes once its answers are sent (see CheckDeadlines).
			BeginClose(*connection, linger);
			connection->CloseDeadline = std::min(connection->CloseDeadline, deadline);
		}
		UpdateClientCount();
	}

	void RpcServer::Impl::CancelQueuedRequests(std::vector<OutgoingResponse>& outgoing)
	{
		// Queued requests wait for a ProcessRequests call that will not come any more; their clients get an answer.
		std::vector<QueuedRequest> abandoned;
		{
			std::scoped_lock<std::mutex> lock(QueueMutex);
			abandoned.swap(Queue);
			QueuedBytes = 0;
		}
		for (QueuedRequest& request : abandoned)
		{
			RpcResult result = request.IsNotification ? RpcResult::Success(nullptr)
				: RpcResult::Failure(JsonRpc::ErrorCode::Cancelled, fmt::format("The server is shutting down; {} was not handled", QuoteMethod(request.Method)));
			outgoing.push_back(OutgoingResponse { request.ConnectionId, std::move(request.Id), std::move(result), request.IsNotification });
		}
	}

	uint32_t RpcServer::Impl::AcceptConnections()
	{
		uint32_t accepted = 0;
		while (std::optional<TcpSocket> socket = Listener.Accept(std::chrono::milliseconds(0)))
		{
			accepted++;
			uint64_t suppressed = 0;

			// Connections that have not authenticated never lock out new ones: one of them makes room (see
			// ChoosePendingConnectionToEvict). A legitimate client authenticates within milliseconds, so it is the
			// hoarded slots that get recycled. Connections accepted since the last poll (this loop drains the whole
			// backlog at once) are never evicted before they had a chance to send, and whatever the others have sent
			// is handled first.
			bool refuse = false;
			if (CountConnections(false) >= Specification.MaxPendingConnections)
				CatchUpPendingConnections();
			if (CountConnections(false) >= Specification.MaxPendingConnections)
			{
				// Connections are kept in the order they were accepted, so the candidates are listed oldest first.
				std::vector<Connection*> candidates;
				std::vector<PendingConnectionState> states;
				for (const Scope<Connection>& existing : Connections)
				{
					if (existing->Authenticated || existing->Closing || existing->Closed)
						continue;
					candidates.push_back(existing.get());
					states.push_back(PendingConnectionState { existing->Polled, !existing->ServerNonce.empty() });
				}

				if (const std::optional<size_t> evicted = ChoosePendingConnectionToEvict(states))
				{
					Connection& victim = *candidates[*evicted];
					if (RejectionWarnings.Allow(suppressed))
						ST_CORE_WARN("RpcServer: closing client {}, which has not authenticated, to make room for a new connection{}", victim.Id, DescribeSuppressed(suppressed));
					Send(victim, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::ServerBusy, "Too many connections are waiting to authenticate; this one was chosen to make room"));
					BeginClose(victim, c_ErrorCloseLinger);
				}
				else
				{
					// No waiting connection has had a chance to send yet (they all arrived in this burst), so the new
					// one is turned away instead.
					refuse = true;
				}
			}

			// Connections still finishing a close are kept only up to a limit; beyond it the oldest one is dropped.
			const size_t trackedLimit = static_cast<size_t>(Specification.MaxClients) + Specification.MaxPendingConnections + c_MaxClosingConnections;
			const size_t tracked = static_cast<size_t>(std::count_if(Connections.begin(), Connections.end(), [](const Scope<Connection>& existing) { return !existing->Closed; }));
			if (tracked >= trackedLimit)
			{
				Connection* oldestClosing = FindOldestConnection([](const Connection& candidate) { return candidate.Closing; });
				if (!oldestClosing)
				{
					if (RejectionWarnings.Allow(suppressed))
						ST_CORE_WARN("RpcServer: dropping a connection, {} connections are open{}", tracked, DescribeSuppressed(suppressed));
					continue; // Closed when the socket goes out of scope
				}
				oldestClosing->Closed = true;
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

			if (refuse)
			{
				if (RejectionWarnings.Allow(suppressed))
					ST_CORE_WARN("RpcServer: rejecting client {}, {} connections that just arrived are waiting to authenticate{}", connection->Id, Specification.MaxPendingConnections, DescribeSuppressed(suppressed));
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

	void RpcServer::Impl::CatchUpPendingConnections()
	{
		// Input that arrived since the last poll is handled before choosing a connection to evict, so the choice
		// sees each connection's latest state (e.g. a handshake it just sent) and never discards a request that is
		// already here. A connection that authenticates this way frees its pending slot.
		for (const Scope<Connection>& connection : Connections)
		{
			if (connection->Authenticated || connection->Closing || connection->Closed || connection->PeerFinished)
				continue;
			Guard(*connection, [&]() { ReadFrom(*connection); });
		}
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
			HandleHandshakeMessage(connection, message);
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
		if (IsIdTooLong(validation.Id))
		{
			Send(connection, JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::InvalidRequest, fmt::format("The request id is longer than {} characters", c_MaxIdLength)));
			return;
		}
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

		if (method == c_RpcHandshakeMethod || method == c_RpcAuthenticateMethod)
		{
			reply(RpcResult::Failure(JsonRpc::ErrorCode::InvalidRequest, "This connection is already authenticated"));
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
			reply(RpcResult::Failure(JsonRpc::ErrorCode::MethodNotFound, fmt::format("Method {} not found", QuoteMethod(method))));
			return;
		}

		{
			std::scoped_lock<std::mutex> lock(QueueMutex);
			if (Queue.size() >= Specification.MaxQueuedRequests)
			{
				reply(RpcResult::Failure(JsonRpc::ErrorCode::ServerBusy, "Too many pending requests; the server is not keeping up"));
				return;
			}
			Queue.push_back(QueuedRequest { connection.Id, validation.Id, validation.IsNotification, method, std::move(params), line.size() });
			QueuedBytes += line.size();
		}
		// Notifications count as well: they produce no response, but their handling still costs the main thread.
		connection.RequestsInFlight++;
	}

	void RpcServer::Impl::HandleHandshakeMessage(Connection& connection, const std::optional<nlohmann::json>& message)
	{
		// Until the handshake completes, only its two requests are accepted, in order. Anything else ends the
		// connection: the peer is either a confused client or a hostile local process probing the port (e.g. a web
		// page sending HTTP), and gets nothing more.
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
		if (IsIdTooLong(validation.Id))
		{
			reject(nullptr, JsonRpc::ErrorCode::InvalidRequest, "The request id is too long", true);
			return;
		}
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

		const std::string& method = (*message)["method"].get_ref<const std::string&>();
		const auto paramsIt = message->find("params");
		const nlohmann::json params = paramsIt != message->end() ? *paramsIt : nlohmann::json::object();

		if (connection.ServerNonce.empty())
		{
			// Step 1: the client's nonce in, the server's nonce and its proof of the token out.
			if (method != c_RpcHandshakeMethod)
			{
				reject(validation.Id, JsonRpc::ErrorCode::Unauthorized, "Authentication required: the first request must be rpc.handshake", true);
				return;
			}
			const std::string* clientNonce = FindString(params, "clientNonce");
			if (!clientNonce || !RpcAuthentication::IsValidNonce(*clientNonce))
			{
				reject(validation.Id, JsonRpc::ErrorCode::InvalidParams, "Expected params {\"clientNonce\": 32 lower-case hexadecimal characters}", true);
				return;
			}
			std::string serverNonce = RpcAuthentication::GenerateNonce();
			if (serverNonce.empty())
			{
				reject(validation.Id, JsonRpc::ErrorCode::InternalError, "The server's random number generator failed", true);
				return;
			}
			// Each nonce is used once per connection; a client echoing the server's nonce back (or one that
			// happens to match) could make one side's proof stand in for a fresh one.
			if (serverNonce == *clientNonce)
			{
				reject(validation.Id, JsonRpc::ErrorCode::InvalidParams, "The nonces of a handshake must differ", true);
				return;
			}

			connection.ClientNonce = *clientNonce;
			connection.ServerNonce = std::move(serverNonce);
			const std::string serverProof = RpcAuthentication::ComputeServerProof(Specification.AuthToken, connection.ClientNonce, connection.ServerNonce);
			Send(connection, JsonRpc::MakeResult(validation.Id, nlohmann::json { { "serverNonce", connection.ServerNonce }, { "serverProof", serverProof } }));
			return;
		}

		// Step 2: the client's proof, bound to both nonces, so neither a recorded proof nor a proof from another
		// connection is accepted.
		if (method != c_RpcAuthenticateMethod)
		{
			const std::string_view reason = method == c_RpcHandshakeMethod
				? "The handshake was already started on this connection"
				: "Authentication required: the second request must be rpc.authenticate";
			reject(validation.Id, JsonRpc::ErrorCode::Unauthorized, reason, true);
			return;
		}
		const std::string* clientProof = FindString(params, "clientProof");
		if (!clientProof)
		{
			reject(validation.Id, JsonRpc::ErrorCode::InvalidParams, "Expected params {\"clientProof\": string}", true);
			return;
		}
		const std::string expectedProof = RpcAuthentication::ComputeClientProof(Specification.AuthToken, connection.ServerNonce, connection.ClientNonce);
		if (!Crypto::ConstantTimeEquals(*clientProof, expectedProof))
		{
			reject(validation.Id, JsonRpc::ErrorCode::Unauthorized, "Authentication failed: the proof does not match the session token", true);
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
		connection.ClientNonce.clear();
		connection.ServerNonce.clear();
		connection.Reader.SetMaxMessageSize(Specification.MaxMessageSize);
		UpdateClientCount();
		ST_CORE_TRACE("RpcServer: client {} authenticated", connection.Id);
		Send(connection, JsonRpc::MakeResult(validation.Id, nlohmann::json { { "authenticated", true } }));
	}

	void RpcServer::Impl::TransferResponses(Connection& connection)
	{
		// Answers are serialized only while the send buffer has room, so a client that reads slowly holds at most
		// its in-flight answers as values plus about one threshold of serialized output.
		while (!connection.Closed && !connection.PendingResponses.empty() && connection.GetPendingOutput() < c_OutputBackpressureThreshold)
		{
			const OutgoingResponse response = std::move(connection.PendingResponses.front());
			connection.PendingResponses.pop_front();
			if (connection.RequestsInFlight > 0)
				connection.RequestsInFlight--;
			SendSerialized(connection, SerializeBounded(MakeResponse(response.Id, response.Result)));
		}
	}

	std::string RpcServer::Impl::SerializeBounded(const nlohmann::json& message)
	{
		std::string serialized = JsonRpc::Serialize(message);
		if (serialized.size() <= Specification.MaxMessageSize)
			return serialized;

		// The client could not read it (its reader enforces the same limit), so send an error instead.
		uint64_t suppressed = 0;
		if (ProtocolWarnings.Allow(suppressed))
			ST_CORE_WARN("RpcServer: a {} byte reply exceeds the message size limit{}", serialized.size(), DescribeSuppressed(suppressed));

		const std::string reason = fmt::format("The response ({} bytes) exceeds the maximum message size of {} bytes", serialized.size(), Specification.MaxMessageSize);
		const auto id = message.is_object() ? message.find("id") : message.end();
		std::string error = JsonRpc::Serialize(JsonRpc::MakeError(id != message.end() ? *id : nlohmann::json(), JsonRpc::ErrorCode::InternalError, reason));
		if (error.size() <= Specification.MaxMessageSize)
			return error;
		// Only reachable with an id too long to echo (requests with such ids are refused before they get here).
		return JsonRpc::Serialize(JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::InternalError, reason));
	}

	void RpcServer::Impl::Send(Connection& connection, const nlohmann::json& message)
	{
		SendSerialized(connection, SerializeBounded(message));
	}

	void RpcServer::Impl::SendSerialized(Connection& connection, std::string serialized)
	{
		if (connection.Closed)
			return;

		// Output that starts waiting now has made no progress yet; the stall timeout counts from here.
		if (connection.GetPendingOutput() == 0)
			connection.LastSendProgress = std::chrono::steady_clock::now();
		connection.SendBuffer += serialized;
		connection.SendBuffer += '\n';

		// Authenticated connections are bounded by backpressure (no request is read and no answer is moved in
		// while about a threshold of output waits) and by the stall timeout. Before authentication the only
		// output is an error or two, so anything more means the peer is not reading at all.
		if (!connection.Authenticated && connection.GetPendingOutput() > c_PreAuthMaxPendingOutput)
		{
			uint64_t suppressed = 0;
			if (ProtocolWarnings.Allow(suppressed))
				ST_CORE_WARN("RpcServer: unauthenticated client {} is not reading its output; disconnecting{}", connection.Id, DescribeSuppressed(suppressed));
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
			connection.LastSendProgress = std::chrono::steady_clock::now();
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

		// A client that keeps its connection open but stops reading would otherwise hold its output and its
		// in-flight slots forever. Nothing more can be sent to it, so it is dropped without a goodbye.
		if (connection.GetPendingOutput() > 0 && now - connection.LastSendProgress >= Specification.StalledClientTimeout)
		{
			uint64_t suppressed = 0;
			if (ProtocolWarnings.Allow(suppressed))
				ST_CORE_WARN("RpcServer: client {} has not accepted any output for {} ms; disconnecting{}", connection.Id, Specification.StalledClientTimeout.count(), DescribeSuppressed(suppressed));
			connection.Closed = true;
			return;
		}

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
		if (connection.GetPendingOutput() > 0 || connection.RequestsInFlight > 0)
			return;

		// Everything is sent: end our stream once, then wait for the client's end so the close is orderly.
		if (!connection.SentShutdown)
		{
			connection.Socket.ShutdownSend();
			connection.SentShutdown = true;
			connection.CloseDeadline = std::min(connection.CloseDeadline, now + c_DrainTimeout);
		}
		// A stopping server does not wait for that: an idle client (e.g. an MCP server between tool calls) may only
		// notice the end of the stream when it next uses the connection. Its answers are sent, and a client that waits
		// for each answer before sending more has read them; anything it sends later finds the connection closed.
		if (connection.PeerFinished || DrainStarted)
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

	bool RpcServer::Impl::CanProcessRequests(const Connection& connection)
	{
		if (!connection.Authenticated)
			return true; // Only the first message is ever handled, and it is size-limited
		return connection.GetPendingOutput() < c_OutputBackpressureThreshold && connection.RequestsInFlight < Specification.MaxRequestsInFlightPerClient && HasQueueRoom();
	}

	bool RpcServer::Impl::HasQueueRoom()
	{
		if (QueuedBytes.load() < Specification.MaxQueuedBytes)
			return true;

		// ProcessRequests wakes this thread after draining the queue, but only if it sees the flag. Checking again
		// after raising it means a drain that happened in between is not missed (the atomics are sequentially
		// consistent: either ProcessRequests sees the flag, or this load sees the drained size).
		QueueBackpressured = true;
		return QueuedBytes.load() < Specification.MaxQueuedBytes;
	}

	bool RpcServer::Impl::WantsInput(const Connection& connection)
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
		m_Impl->LastError.clear();
		auto fail = [this](std::string error)
		{
			ST_CORE_ERROR("RpcServer: {}", error);
			m_Impl->LastError = std::move(error);
			return false;
		};

		// Automation grants full control over the editor, so it is never exposed beyond this machine.
		if (!IsLoopbackAddress(specification.BindAddress))
			return fail(fmt::format("refusing to listen on '{}': only loopback addresses (127.0.0.0/8 or ::1) are allowed", specification.BindAddress));
		if (specification.AuthToken.empty())
			return fail("refusing to start without an authentication token");
		if (!m_Impl->Listener.Listen(specification.BindAddress, specification.Port))
			return fail(m_Impl->Listener.GetLastError());

		RpcServerSpecification& active = m_Impl->Specification;
		active = specification;
		active.MaxClients = std::max(specification.MaxClients, 1u);
		active.MaxPendingConnections = std::max(specification.MaxPendingConnections, 1u);
		active.MaxQueuedRequests = std::max(specification.MaxQueuedRequests, 1u);
		active.MaxRequestsInFlightPerClient = std::max(specification.MaxRequestsInFlightPerClient, 1u);
		active.MaxMessageSize = std::max(specification.MaxMessageSize, c_PreAuthMaxMessageSize);
		active.MaxQueuedBytes = std::max(specification.MaxQueuedBytes, size_t(1));
		active.StalledClientTimeout = ClampSocketTimeout(specification.StalledClientTimeout);

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

	void RpcServer::Stop(std::chrono::milliseconds gracePeriod)
	{
		if (!m_Impl->NetworkThread.joinable())
			return;

		if (gracePeriod.count() > 0)
		{
			{
				std::scoped_lock<std::mutex> lock(m_Impl->Shared->Mutex);
				m_Impl->Shared->Draining = true;
				m_Impl->Shared->DrainDeadline = std::chrono::steady_clock::now() + ClampSocketTimeout(gracePeriod);
				m_Impl->Shared->Notifier.Notify();
			}
			// The network thread ends by itself once the clients have their answers or the grace period is over.
			m_Impl->NetworkThread.join();
		}

		{
			std::scoped_lock<std::mutex> lock(m_Impl->Shared->Mutex);
			m_Impl->Shared->Running = false;
			m_Impl->Shared->Draining = false;
			m_Impl->Shared->Notifier.Notify();
		}
		if (m_Impl->NetworkThread.joinable())
			m_Impl->NetworkThread.join();

		m_Impl->Shared->Notifier.Close();
		m_Impl->Listener.Close();
		{
			std::scoped_lock<std::mutex> lock(m_Impl->QueueMutex);
			m_Impl->Queue.clear();
			m_Impl->QueuedBytes = 0;
			m_Impl->QueueBackpressured = false;
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

	const std::string& RpcServer::GetLastError() const
	{
		return m_Impl->LastError;
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
		if (!IsValidMethod(info, handler))
			return false;
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

	bool RpcServer::ReplaceMethod(RpcMethodInfo info, RpcHandler handler)
	{
		if (!IsValidMethod(info, handler))
			return false;
		info.ParamsSchema = NormalizeParamsSchema(std::move(info.ParamsSchema));

		const std::string name = info.Name;
		std::unique_lock<std::mutex> lock(m_Impl->MethodsMutex);
		const bool replaced = m_Impl->Methods.contains(name);
		m_Impl->Methods.insert_or_assign(name, RegisteredMethod { std::move(info), std::move(handler) });

		// As in UnregisterMethod: the caller may destroy what the previous handler captured once this returns.
		if (replaced && m_Impl->ProcessingThread != std::this_thread::get_id())
			m_Impl->MethodsCondition.wait(lock, [&]() { return m_Impl->RunningMethod != name; });
		return true;
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
			size_t dequeuedBytes = 0;
			for (const QueuedRequest& request : requests)
				dequeuedBytes += request.Size;
			m_Impl->QueuedBytes -= dequeuedBytes;
		}
		// The network thread stopped reading because the queue was full; now that it is drained, let it resume.
		if (m_Impl->QueueBackpressured.exchange(false) && m_Impl->Shared)
			m_Impl->Shared->Wake();
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

			// A notification's "response" is never sent; delivering it only tells the network thread that the
			// notification no longer counts toward its client's in-flight budget.
			RpcResponder::DeliveryFunction deliver = [weakShared, connectionId = request.ConnectionId, id = request.Id, isNotification = request.IsNotification](RpcResult result)
			{
				if (Ref<RpcServerShared> shared = weakShared.lock())
					shared->Deliver(connectionId, id, isNotification ? RpcResult::Success(nullptr) : std::move(result), isNotification);
			};
			Ref<RpcResponder> responder = CreateRef<RpcResponder>(request.Method, std::move(deliver));

			if (!handler)
			{
				responder->Respond(RpcResult::Failure(JsonRpc::ErrorCode::MethodNotFound, fmt::format("Method {} not found", QuoteMethod(request.Method))));
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
				if (!responder->TryRespond(RpcResult::Failure(JsonRpc::ErrorCode::InternalError, fmt::format("Method {} failed: {}", QuoteMethod(request.Method), exception.what()))))
					ST_CORE_ERROR("RpcServer: method {} threw after responding: {}", QuoteMethod(request.Method), exception.what());
			}
			catch (...)
			{
				if (!responder->TryRespond(RpcResult::Failure(JsonRpc::ErrorCode::InternalError, fmt::format("Method {} failed with an unknown exception", QuoteMethod(request.Method)))))
					ST_CORE_ERROR("RpcServer: method {} threw after responding", QuoteMethod(request.Method));
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
