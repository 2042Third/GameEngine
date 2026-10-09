#include <doctest/doctest.h>

#include "Network/NetworkTestHelpers.h"
#include "Strata/Core/Crypto.h"
#include "Strata/Network/RpcClient.h"
#include "Strata/Network/RpcConnectionLimits.h"
#include "Strata/Network/RpcServer.h"
#include "TestHelpers.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{
	constexpr std::chrono::milliseconds c_CallTimeout = std::chrono::milliseconds(5000);

	RpcMethodInfo MakeMethod(std::string name, std::string description = {})
	{
		RpcMethodInfo info;
		info.Name = std::move(name);
		info.Description = std::move(description);
		return info;
	}

	void RegisterEcho(RpcServer& server)
	{
		RpcMethodInfo info = MakeMethod("test.echo", "Returns its params");
		info.ParamsSchema = nlohmann::json { { "type", "object" }, { "properties", { { "value", { { "type", "string" } } } } } };
		REQUIRE(server.RegisterMethod(info, [](const nlohmann::json& params) { return RpcResult::Success(params); }));
	}

	RpcClient& ConnectClient(RpcClient& client, uint16_t port, std::string_view token = Tests::c_TestServerToken)
	{
		REQUIRE_MESSAGE(client.Connect("127.0.0.1", port, token, std::chrono::milliseconds(2000)), client.GetLastError());
		return client;
	}

	std::string MakeRequestLine(const nlohmann::json& id, const std::string& method, nlohmann::json params = nlohmann::json::object())
	{
		return JsonRpc::Serialize(JsonRpc::MakeRequest(id, method, std::move(params)));
	}

	// Expects the connection's next message to be an error with the given code, followed by an orderly close.
	void CheckRejectedAndClosed(Tests::RawRpcConnection& connection, int code)
	{
		std::optional<nlohmann::json> response = connection.ReadMessage();
		REQUIRE(response.has_value());
		REQUIRE(response->contains("error"));
		CHECK((*response)["error"]["code"] == code);
		CHECK_FALSE(connection.ReadMessage().has_value());
		CHECK(connection.WasClosedByPeer());
		CHECK_FALSE(connection.WasResetByPeer());
	}

	std::string Nest(size_t depth)
	{
		return std::string(depth, '[') + std::string(depth, ']');
	}

	// Threads started by handlers, joined when the test ends however it ends. Declare it before the server, so the
	// server (and its handlers) are gone before the threads are joined.
	struct ThreadCollection
	{
		std::mutex Mutex;
		std::vector<std::thread> Threads;

		~ThreadCollection()
		{
			for (std::thread& thread : Threads)
			{
				if (thread.joinable())
					thread.join();
			}
		}
	};
}

TEST_SUITE("Network.RpcServer")
{
	TEST_CASE("Requests round-trip between client and server")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());
		CHECK(server.GetServer().IsRunning());
		CHECK(server.GetPort() != 0);

		RpcClient client;
		ConnectClient(client, server.GetPort());
		CHECK(client.IsConnected());

		const RpcResult echo = client.Call("test.echo", nlohmann::json { { "value", "hello" }, { "number", 3 } }, c_CallTimeout);
		REQUIRE_MESSAGE(echo.IsSuccess(), echo.GetError().Message);
		CHECK(echo.GetValue()["value"] == "hello");
		CHECK(echo.GetValue()["number"] == 3);

		const RpcResult ping = client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(ping.IsSuccess());
		CHECK(ping.GetValue()["pong"] == true);

		// Requests without params receive an empty object.
		const RpcResult empty = client.Call("test.echo", nullptr, c_CallTimeout);
		REQUIRE(empty.IsSuccess());
		CHECK(empty.GetValue() == nlohmann::json::object());

		CHECK(Tests::WaitUntil([&]() { return server.GetServer().GetClientCount() == 1; }));
		client.Close();
		CHECK_FALSE(client.IsConnected());
		CHECK(Tests::WaitUntil([&]() { return server.GetServer().GetClientCount() == 0; }));
	}

	TEST_CASE("The server refuses non-loopback addresses and missing tokens")
	{
		RpcServer server;
		for (const char* address : { "0.0.0.0", "::", "", "localhost", "192.168.1.10", "10.0.0.1" })
		{
			INFO(address);
			RpcServerSpecification specification = Tests::MakeTestServerSpecification();
			specification.BindAddress = address;
			CHECK_FALSE(server.Start(specification));
			CHECK_FALSE(server.IsRunning());
		}

		RpcServerSpecification withoutToken = Tests::MakeTestServerSpecification();
		withoutToken.AuthToken.clear();
		CHECK_FALSE(server.Start(withoutToken));

		CHECK(server.Start(Tests::MakeTestServerSpecification()));
		CHECK(server.IsRunning());
	}

	TEST_CASE("rpc.listMethods describes built-in and registered methods")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		RpcMethodInfo withoutSchema = MakeMethod("scene.save", "Saves the scene");
		withoutSchema.ParamsSchema = nullptr;
		REQUIRE(server.GetServer().RegisterMethod(withoutSchema, [](const nlohmann::json&) { return RpcResult::Success(true); }));
		REQUIRE(server.Start());

		RpcClient client;
		ConnectClient(client, server.GetPort());
		const RpcResult result = client.Call("rpc.listMethods", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(result.IsSuccess());

		std::vector<std::string> names;
		for (const nlohmann::json& method : result.GetValue()["methods"])
		{
			names.push_back(method["name"].get<std::string>());
			CHECK(method["description"].is_string());
			CHECK(method["paramsSchema"].is_object());
			CHECK(method["paramsSchema"]["type"] == "object");
			if (method["name"] == "test.echo")
			{
				CHECK(method["description"] == "Returns its params");
				CHECK(method["paramsSchema"]["properties"]["value"]["type"] == "string");
			}
		}
		CHECK(names == std::vector<std::string> { "rpc.handshake", "rpc.authenticate", "rpc.ping", "rpc.listMethods", "scene.save", "test.echo" });

		const std::vector<RpcMethodInfo> methods = server.GetServer().GetMethods();
		REQUIRE(methods.size() == 6);
		CHECK(methods[4].Name == "scene.save");
		CHECK(methods[4].ParamsSchema["type"] == "object"); // A null schema is normalized
	}

	TEST_CASE("Method registration rules")
	{
		RpcServer server;
		auto handler = [](const nlohmann::json&) { return RpcResult::Success(nullptr); };
		CHECK(server.RegisterMethod(MakeMethod("a.b"), handler));
		CHECK_FALSE(server.RegisterMethod(MakeMethod("a.b"), handler));
		CHECK_FALSE(server.RegisterMethod(MakeMethod(""), handler));
		CHECK_FALSE(server.RegisterMethod(MakeMethod("rpc.custom"), handler));
		CHECK_FALSE(server.RegisterMethod(MakeMethod("no.handler"), RpcHandler()));
		CHECK(server.GetMethods().size() == 5); // 4 built-ins + a.b
		server.UnregisterMethod("a.b");
		CHECK(server.GetMethods().size() == 4);
		CHECK(server.RegisterMethod(MakeMethod("a.b"), handler));
	}

	TEST_CASE("Unknown and unregistered methods report MethodNotFound")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());

		RpcClient client;
		ConnectClient(client, server.GetPort());

		const RpcResult unknown = client.Call("does.notExist", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(unknown.IsError());
		CHECK(unknown.GetError().Code == JsonRpc::ErrorCode::MethodNotFound);
		CHECK(unknown.GetError().Message.find("does.notExist") != std::string::npos);

		const RpcResult reserved = client.Call("rpc.unknown", nlohmann::json::object(), c_CallTimeout);
		CHECK(reserved.GetError().Code == JsonRpc::ErrorCode::MethodNotFound);

		server.GetServer().UnregisterMethod("test.echo");
		const RpcResult removed = client.Call("test.echo", nlohmann::json::object(), c_CallTimeout);
		CHECK(removed.GetError().Code == JsonRpc::ErrorCode::MethodNotFound);
		CHECK(client.IsConnected());
	}

	TEST_CASE("Handler errors reach the client")
	{
		Tests::PumpedRpcServer server;
		RpcServer& rpc = server.GetServer();
		REQUIRE(rpc.RegisterMethod(MakeMethod("entity.get"), [](const nlohmann::json& params)
		{
			if (!params.contains("id"))
				return RpcResult::Failure(JsonRpc::ErrorCode::InvalidParams, "Missing 'id'", nlohmann::json { { "Required", "id" } });
			return RpcResult::Success(params["id"]);
		}));
		REQUIRE(rpc.RegisterMethod(MakeMethod("entity.throws"), [](const nlohmann::json& params)
		{
			// at() throws for a missing key; the server must contain it.
			return RpcResult::Success(params.at("missing"));
		}));
		REQUIRE(server.Start());

		RpcClient client;
		ConnectClient(client, server.GetPort());

		const RpcResult invalid = client.Call("entity.get", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(invalid.IsError());
		CHECK(invalid.GetError().Code == JsonRpc::ErrorCode::InvalidParams);
		CHECK(invalid.GetError().Message == "Missing 'id'");
		CHECK(invalid.GetError().Data["Required"] == "id");
		CHECK(client.GetLastError() == "Missing 'id'");

		const RpcResult valid = client.Call("entity.get", nlohmann::json { { "id", 9 } }, c_CallTimeout);
		REQUIRE(valid.IsSuccess());
		CHECK(valid.GetValue() == 9);

		const RpcResult thrown = client.Call("entity.throws", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(thrown.IsError());
		CHECK(thrown.GetError().Code == JsonRpc::ErrorCode::InternalError);

		// The server keeps serving after handler failures.
		CHECK(client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
	}

	TEST_CASE("Malformed and invalid messages from an authenticated client get JSON-RPC errors")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());

		REQUIRE(connection.SendLine("{this is not json"));
		std::optional<nlohmann::json> parseError = connection.ReadMessage();
		REQUIRE(parseError.has_value());
		CHECK((*parseError)["id"].is_null());
		CHECK((*parseError)["error"]["code"] == JsonRpc::ErrorCode::ParseError);

		REQUIRE(connection.SendLine(R"({"jsonrpc":"1.0","id":4,"method":"test.echo"})"));
		std::optional<nlohmann::json> invalid = connection.ReadMessage();
		REQUIRE(invalid.has_value());
		CHECK((*invalid)["id"] == 4);
		CHECK((*invalid)["error"]["code"] == JsonRpc::ErrorCode::InvalidRequest);

		REQUIRE(connection.SendLine(R"([{"jsonrpc":"2.0","id":5,"method":"rpc.ping"}])"));
		std::optional<nlohmann::json> batch = connection.ReadMessage();
		REQUIRE(batch.has_value());
		CHECK((*batch)["error"]["code"] == JsonRpc::ErrorCode::InvalidRequest);

		// Notifications (no id) and stray responses get no response: the next message answers the ping.
		REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","method":"test.echo","params":{}})"));
		REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","method":"does.notExist"})"));
		REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","id":99,"result":true})"));
		REQUIRE(connection.SendLine("\r\n   \r\n"));
		REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","id":"ping-1","method":"rpc.ping"})"));
		std::optional<nlohmann::json> pong = connection.ReadMessage();
		REQUIRE(pong.has_value());
		CHECK((*pong)["id"] == "ping-1");
		CHECK((*pong)["result"]["pong"] == true);

		// CRLF-terminated requests are accepted.
		REQUIRE(connection.GetSocket().SendAll(std::string_view("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"test.echo\",\"params\":{\"value\":\"crlf\"}}\r\n")));
		std::optional<nlohmann::json> echo = connection.ReadMessage();
		REQUIRE(echo.has_value());
		CHECK((*echo)["result"]["value"] == "crlf");

		// Deep nesting is refused before any value is built; moderate nesting is fine.
		REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","id":8,"method":"test.echo","params":{"value":)" + Nest(JsonRpc::c_MaxJsonDepth) + "}}"));
		std::optional<nlohmann::json> deep = connection.ReadMessage();
		REQUIRE(deep.has_value());
		CHECK((*deep)["error"]["code"] == JsonRpc::ErrorCode::ParseError);
		REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","id":9,"method":"test.echo","params":{"value":)" + Nest(100) + "}}"));
		std::optional<nlohmann::json> moderate = connection.ReadMessage();
		REQUIRE(moderate.has_value());
		CHECK((*moderate)["id"] == 9);
		CHECK((*moderate)["result"]["value"].is_array());
	}

	TEST_CASE("Oversized messages are rejected and the connection is closed cleanly")
	{
		Tests::PumpedRpcServer server;
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxMessageSize = 8 * 1024;
		REQUIRE(server.Start(specification));

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());
		REQUIRE(connection.GetSocket().SendAll(std::string(32 * 1024, 'x')));
		CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::InvalidRequest);
	}

	TEST_CASE("Responses larger than the message size limit become errors")
	{
		Tests::PumpedRpcServer server;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.huge"), [](const nlohmann::json&)
		{
			return RpcResult::Success(std::string(64 * 1024, 'y'));
		}));
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxMessageSize = 16 * 1024;
		REQUIRE(server.Start(specification));

		RpcClient client;
		ConnectClient(client, server.GetPort());
		const RpcResult result = client.Call("test.huge", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(result.IsError());
		CHECK(result.GetError().Code == JsonRpc::ErrorCode::InternalError);
		CHECK(result.GetError().Message.find("exceeds the maximum message size") != std::string::npos);
		CHECK(client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
	}

	TEST_CASE("A client that half-closes still receives its responses")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());

		// Like `printf '<requests>\n' | nc`: send everything, signal the end of input, then read the answers.
		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());
		REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","id":1,"method":"test.echo","params":{"value":"queued"}})"));
		REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","id":2,"method":"rpc.ping"})"));
		REQUIRE(connection.GetSocket().ShutdownSend());

		std::vector<nlohmann::json> responses;
		for (int index = 0; index < 2; index++)
		{
			std::optional<nlohmann::json> response = connection.ReadMessage();
			REQUIRE(response.has_value());
			responses.push_back(std::move(*response));
		}
		std::sort(responses.begin(), responses.end(), [](const nlohmann::json& left, const nlohmann::json& right) { return left["id"].get<int>() < right["id"].get<int>(); });
		CHECK(responses[0]["result"]["value"] == "queued");
		CHECK(responses[1]["result"]["pong"] == true);

		// Once everything is answered, the server closes its side too.
		CHECK_FALSE(connection.ReadMessage().has_value());
		CHECK(connection.WasClosedByPeer());
		CHECK_FALSE(connection.WasResetByPeer());
	}

	TEST_CASE("The connection must complete the handshake first")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		const std::string clientNonce = RpcAuthentication::GenerateNonce();

		SUBCASE("Any other request is rejected and the connection closed")
		{
			REQUIRE(connection.SendLine(MakeRequestLine(1, "rpc.ping")));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::Unauthorized);
		}

		SUBCASE("A proof before the handshake is rejected")
		{
			REQUIRE(connection.SendLine(MakeRequestLine(1, "rpc.authenticate", nlohmann::json { { "clientProof", std::string(64, '0') } })));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::Unauthorized);
		}

		SUBCASE("A missing client nonce closes the connection")
		{
			REQUIRE(connection.SendLine(MakeRequestLine(1, "rpc.handshake")));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::InvalidParams);
		}

		SUBCASE("A malformed client nonce closes the connection")
		{
			REQUIRE(connection.SendLine(MakeRequestLine(1, "rpc.handshake", nlohmann::json { { "clientNonce", "not-hex-and-too-short" } })));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::InvalidParams);
		}

		SUBCASE("A nonce in another spelling closes the connection")
		{
			REQUIRE(connection.SendLine(MakeRequestLine(1, "rpc.handshake", nlohmann::json { { "clientNonce", std::string(32, 'A') } })));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::InvalidParams);
		}

		SUBCASE("A proof made with the wrong token closes the connection")
		{
			std::optional<nlohmann::json> handshake = connection.Handshake(clientNonce);
			REQUIRE(handshake.has_value());
			const std::string serverNonce = (*handshake)["result"]["serverNonce"].get<std::string>();
			REQUIRE(connection.SendLine(MakeRequestLine(2, "rpc.authenticate", nlohmann::json { { "clientProof", RpcAuthentication::ComputeClientProof("wrong", serverNonce, clientNonce) } })));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::Unauthorized);
			CHECK(server.GetServer().GetClientCount() == 0);
		}

		SUBCASE("A second handshake on the same connection closes it")
		{
			REQUIRE(connection.Handshake(clientNonce).has_value());
			REQUIRE(connection.SendLine(MakeRequestLine(2, "rpc.handshake", nlohmann::json { { "clientNonce", RpcAuthentication::GenerateNonce() } })));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::Unauthorized);
		}

		SUBCASE("Any other request between the two steps closes the connection")
		{
			REQUIRE(connection.Handshake(clientNonce).has_value());
			REQUIRE(connection.SendLine(MakeRequestLine(2, "rpc.ping")));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::Unauthorized);
		}

		SUBCASE("Non-JSON-RPC input, such as an HTTP request from a web page, closes the connection")
		{
			REQUIRE(connection.SendLine("POST / HTTP/1.1"));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::ParseError);
		}

		SUBCASE("An invalid request closes the connection")
		{
			REQUIRE(connection.SendLine(R"({"jsonrpc":"1.0","id":1,"method":"rpc.handshake"})"));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::InvalidRequest);
		}

		SUBCASE("A notification closes the connection without a reply")
		{
			REQUIRE(connection.SendLine(R"({"jsonrpc":"2.0","method":"rpc.handshake","params":{"clientNonce":"x"}})"));
			CHECK_FALSE(connection.ReadMessage().has_value());
			CHECK(connection.WasClosedByPeer());
		}

		SUBCASE("A long first line is cut off at the pre-authentication limit")
		{
			REQUIRE(connection.GetSocket().SendAll(std::string(64 * 1024, 'x')));
			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::InvalidRequest);
		}

		SUBCASE("The handshake unlocks the connection and the full message size")
		{
			REQUIRE(connection.Authenticate());
			CHECK(Tests::WaitUntil([&]() { return server.GetServer().GetClientCount() == 1; }));
			REQUIRE(connection.SendLine(MakeRequestLine(2, "test.echo", nlohmann::json { { "value", std::string(256 * 1024, 'v') } })));
			std::optional<nlohmann::json> echo = connection.ReadMessage();
			REQUIRE(echo.has_value());
			CHECK((*echo)["result"]["value"].get_ref<const std::string&>().size() == 256 * 1024);

			// Handshake requests on an authenticated connection are answered with an error but do not end it.
			REQUIRE(connection.SendLine(MakeRequestLine(3, "rpc.handshake", nlohmann::json { { "clientNonce", RpcAuthentication::GenerateNonce() } })));
			std::optional<nlohmann::json> again = connection.ReadMessage();
			REQUIRE(again.has_value());
			CHECK((*again)["error"]["code"] == JsonRpc::ErrorCode::InvalidRequest);
			REQUIRE(connection.SendLine(MakeRequestLine(4, "rpc.authenticate", nlohmann::json { { "clientProof", std::string(64, '0') } })));
			again = connection.ReadMessage();
			REQUIRE(again.has_value());
			CHECK((*again)["error"]["code"] == JsonRpc::ErrorCode::InvalidRequest);
			REQUIRE(connection.SendLine(MakeRequestLine(5, "rpc.ping")));
			std::optional<nlohmann::json> pong = connection.ReadMessage();
			REQUIRE(pong.has_value());
			CHECK((*pong)["result"]["pong"] == true);
		}
	}

	TEST_CASE("The handshake proves the token both ways without sending it")
	{
		Tests::PumpedRpcServer server;
		REQUIRE(server.Start());

		const std::string token = Tests::c_TestServerToken;
		const std::string clientNonce = "00112233445566778899aabbccddeeff";
		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		std::optional<nlohmann::json> handshake = connection.Handshake(clientNonce);
		REQUIRE(handshake.has_value());
		REQUIRE(handshake->contains("result"));
		const std::string serverNonce = (*handshake)["result"]["serverNonce"].get<std::string>();
		CHECK(RpcAuthentication::IsValidNonce(serverNonce));
		CHECK(serverNonce != clientNonce);

		// HMAC-SHA256 over the label and both nonces, computed independently of RpcAuthentication.
		const std::string expectedServerProof = Crypto::ToHex(Crypto::HmacSha256(token, "strata-server" + clientNonce + serverNonce));
		CHECK((*handshake)["result"]["serverProof"] == expectedServerProof);
		CHECK(RpcAuthentication::ComputeServerProof(token, clientNonce, serverNonce) == expectedServerProof);

		const std::string clientProof = Crypto::ToHex(Crypto::HmacSha256(token, "strata-client" + serverNonce + clientNonce));
		CHECK(RpcAuthentication::ComputeClientProof(token, serverNonce, clientNonce) == clientProof);
		CHECK(clientProof != expectedServerProof);
		std::optional<nlohmann::json> authenticated = connection.SendClientProof(clientProof);
		REQUIRE(authenticated.has_value());
		CHECK((*authenticated)["result"]["authenticated"] == true);

		// Every connection gets a fresh server nonce, even for the same client nonce.
		Tests::RawRpcConnection other;
		REQUIRE(other.Connect(server.GetPort()));
		std::optional<nlohmann::json> otherHandshake = other.Handshake(clientNonce);
		REQUIRE(otherHandshake.has_value());
		CHECK((*otherHandshake)["result"]["serverNonce"] != serverNonce);

		CHECK(RpcAuthentication::IsValidNonce(RpcAuthentication::GenerateNonce()));
		CHECK(RpcAuthentication::GenerateNonce() != RpcAuthentication::GenerateNonce());
		CHECK_FALSE(RpcAuthentication::IsValidNonce("0123"));
		CHECK_FALSE(RpcAuthentication::IsValidNonce(std::string(32, 'g')));
		CHECK_FALSE(RpcAuthentication::IsValidNonce(std::string(32, 'A')));
		CHECK_FALSE(RpcAuthentication::IsValidNonce(std::string(31, 'a')));
		CHECK_FALSE(RpcAuthentication::IsValidNonce(std::string(33, 'a')));
	}

	TEST_CASE("A recorded client proof is refused on another connection")
	{
		Tests::PumpedRpcServer server;
		REQUIRE(server.Start());

		// An eavesdropper records a complete handshake...
		const std::string clientNonce = RpcAuthentication::GenerateNonce();
		Tests::RawRpcConnection recorded;
		REQUIRE(recorded.Connect(server.GetPort()));
		std::optional<nlohmann::json> handshake = recorded.Handshake(clientNonce);
		REQUIRE(handshake.has_value());
		const std::string clientProof = RpcAuthentication::ComputeClientProof(Tests::c_TestServerToken, (*handshake)["result"]["serverNonce"].get<std::string>(), clientNonce);
		std::optional<nlohmann::json> accepted = recorded.SendClientProof(clientProof);
		REQUIRE(accepted.has_value());
		CHECK((*accepted)["result"]["authenticated"] == true);

		// ...and replays it with the same client nonce: the server's fresh nonce makes the old proof worthless.
		Tests::RawRpcConnection replay;
		REQUIRE(replay.Connect(server.GetPort()));
		REQUIRE(replay.Handshake(clientNonce).has_value());
		REQUIRE(replay.SendLine(MakeRequestLine(2, "rpc.authenticate", nlohmann::json { { "clientProof", clientProof } })));
		CheckRejectedAndClosed(replay, JsonRpc::ErrorCode::Unauthorized);
	}

	TEST_CASE("The client refuses a server that cannot prove it knows the token")
	{
		// A process that took over the port of an editor that exited answers the handshake, but cannot compute the
		// proof. The client must give up before sending anything beyond its nonce, and never sends the token.
		auto runImpostor = [](const std::function<nlohmann::json(const std::string& clientNonce)>& makeResult)
		{
			TcpListener listener;
			REQUIRE(listener.Listen());
			std::atomic<size_t> linesReceived = 0;
			std::atomic<bool> closedByClient = false;
			std::string received;
			std::thread impostor([&]()
			{
				std::optional<TcpSocket> socket = listener.Accept(std::chrono::milliseconds(5000));
				if (!socket)
					return;
				JsonLineReader reader;
				std::vector<uint8_t> buffer;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
				while (std::chrono::steady_clock::now() < deadline)
				{
					buffer.clear();
					const SocketReceiveStatus status = socket->Receive(buffer, std::chrono::milliseconds(100));
					if (status == SocketReceiveStatus::Closed || status == SocketReceiveStatus::Error)
					{
						closedByClient = true;
						return;
					}
					received.append(buffer.begin(), buffer.end());
					reader.Append(buffer);
					while (std::optional<std::string> line = reader.NextLine())
					{
						if (linesReceived++ > 0)
							continue;
						const std::optional<nlohmann::json> request = JsonRpc::Parse(*line);
						if (!request || !request->contains("id"))
							continue;
						const auto params = request->find("params");
						const std::string clientNonce = params != request->end() && params->is_object() ? params->value("clientNonce", std::string()) : std::string();
						socket->SendAll(JsonRpc::Serialize(JsonRpc::MakeResult((*request)["id"], makeResult(clientNonce))) + "\n");
					}
				}
			});

			RpcClient client;
			const bool connected = client.Connect("127.0.0.1", listener.GetPort(), Tests::c_TestServerToken, std::chrono::milliseconds(3000));
			const std::string error = client.GetLastError();
			impostor.join();
			CHECK_FALSE(connected);
			CHECK_FALSE(client.IsConnected());
			CHECK(error.find("could not prove") != std::string::npos);
			CHECK(linesReceived.load() == 1);
			CHECK(closedByClient.load());
			CHECK(received.find("rpc.handshake") != std::string::npos);
			CHECK(received.find(Tests::c_TestServerToken) == std::string::npos);
		};

		SUBCASE("No proof")
		{
			runImpostor([](const std::string&) { return nlohmann::json { { "serverNonce", RpcAuthentication::GenerateNonce() } }; });
		}

		SUBCASE("A wrong proof")
		{
			runImpostor([](const std::string&) { return nlohmann::json { { "serverNonce", RpcAuthentication::GenerateNonce() }, { "serverProof", std::string(64, '0') } }; });
		}

		SUBCASE("A proof recorded from the real editor for another client nonce")
		{
			Tests::PumpedRpcServer editor;
			REQUIRE(editor.Start());
			Tests::RawRpcConnection recorder;
			REQUIRE(recorder.Connect(editor.GetPort()));
			std::optional<nlohmann::json> recorded = recorder.Handshake(RpcAuthentication::GenerateNonce());
			REQUIRE(recorded.has_value());
			const nlohmann::json replayed = (*recorded)["result"];
			runImpostor([&replayed](const std::string&) { return replayed; });
		}

		SUBCASE("The client's own nonce reflected as the server nonce")
		{
			// Even with a correct proof for that pair, equal nonces are refused.
			runImpostor([](const std::string& clientNonce)
			{
				return nlohmann::json { { "serverNonce", clientNonce }, { "serverProof", RpcAuthentication::ComputeServerProof(Tests::c_TestServerToken, clientNonce, clientNonce) } };
			});
		}
	}

	TEST_CASE("The client authenticates while connecting")
	{
		Tests::PumpedRpcServer server;
		REQUIRE(server.Start());

		RpcClient client;
		CHECK_FALSE(client.Connect("127.0.0.1", server.GetPort(), "wrong-token", std::chrono::milliseconds(2000)));
		CHECK_FALSE(client.IsConnected());
		CHECK(client.GetLastError().find("Authentication failed") != std::string::npos);
		CHECK(server.GetServer().GetClientCount() == 0);

		// Without a token there is nothing to authenticate with, so no connection is even attempted.
		CHECK_FALSE(client.Connect("127.0.0.1", server.GetPort(), "", std::chrono::milliseconds(2000)));
		CHECK_FALSE(client.IsConnected());
		CHECK(client.GetLastError().find("token is required") != std::string::npos);

		ConnectClient(client, server.GetPort());
		CHECK(client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
	}

	TEST_CASE("Connections that do not authenticate in time are closed")
	{
		// The deadline counts from the accept and covers the whole handshake. Its default is 5 s, so a close well
		// before that shows the configured timeout is what closed the connection.
		auto checkClosedAtDeadline = [](std::chrono::milliseconds timeout, bool completeFirstStep)
		{
			Tests::PumpedRpcServer server;
			RpcServerSpecification specification = Tests::MakeTestServerSpecification();
			specification.AuthenticationTimeout = timeout;
			REQUIRE(server.Start(specification));

			const auto start = std::chrono::steady_clock::now();
			Tests::RawRpcConnection connection;
			REQUIRE(connection.Connect(server.GetPort()));
			if (completeFirstStep)
			{
				std::optional<nlohmann::json> handshake = connection.Handshake(RpcAuthentication::GenerateNonce());
				REQUIRE(handshake.has_value());
				CHECK(handshake->contains("result"));
			}

			CheckRejectedAndClosed(connection, JsonRpc::ErrorCode::Unauthorized);
			const auto elapsed = std::chrono::steady_clock::now() - start;
			CHECK(elapsed >= timeout);
			CHECK(elapsed < timeout + std::chrono::milliseconds(2500));
		};

		SUBCASE("A connection that sends nothing")
		{
			checkClosedAtDeadline(std::chrono::milliseconds(100), false);
		}

		SUBCASE("A connection that stops after the first step")
		{
			// Long enough for the handshake round trip even on a loaded machine.
			checkClosedAtDeadline(std::chrono::milliseconds(1000), true);
		}
	}

	TEST_CASE("Only authenticated connections hold client slots")
	{
		Tests::PumpedRpcServer server;
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxClients = 1;
		specification.MaxPendingConnections = 2;
		REQUIRE(server.Start(specification));

		// A connection that never authenticates does not take the only client slot.
		Tests::RawRpcConnection idleFirst;
		REQUIRE(idleFirst.Connect(server.GetPort()));
		RpcClient first;
		ConnectClient(first, server.GetPort());
		CHECK(first.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
		CHECK(server.GetServer().GetClientCount() == 1);

		// With two connections waiting to authenticate, a third one makes room by closing the oldest of them.
		Tests::RawRpcConnection idleSecond;
		REQUIRE(idleSecond.Connect(server.GetPort()));
		Tests::RawRpcConnection idleThird;
		REQUIRE(idleThird.Connect(server.GetPort()));
		CheckRejectedAndClosed(idleFirst, JsonRpc::ErrorCode::ServerBusy);

		// The other idle connections give up (the server notices the end of their streams and closes them).
		for (Tests::RawRpcConnection* idle : { &idleSecond, &idleThird })
		{
			REQUIRE(idle->GetSocket().ShutdownSend());
			CHECK_FALSE(idle->ReadMessage().has_value());
			CHECK(idle->WasClosedByPeer());
		}

		// A second client authenticates correctly but exceeds MaxClients: it gets exactly ServerBusy, and the clean
		// close guarantees the reply arrives before the connection ends.
		Tests::RawRpcConnection second;
		REQUIRE(second.Connect(server.GetPort()));
		const std::string clientNonce = RpcAuthentication::GenerateNonce();
		std::optional<nlohmann::json> handshake = second.Handshake(clientNonce);
		REQUIRE(handshake.has_value());
		const std::string serverNonce = (*handshake)["result"]["serverNonce"].get<std::string>();
		REQUIRE(second.SendLine(MakeRequestLine(2, "rpc.authenticate", nlohmann::json { { "clientProof", RpcAuthentication::ComputeClientProof(Tests::c_TestServerToken, serverNonce, clientNonce) } })));
		std::optional<nlohmann::json> rejected = second.ReadMessage();
		REQUIRE(rejected.has_value());
		CHECK((*rejected)["error"]["code"] == JsonRpc::ErrorCode::ServerBusy);
		CHECK((*rejected)["error"]["message"].get<std::string>().find("at most 1 client") != std::string::npos);
		CHECK_FALSE(second.ReadMessage().has_value());
		CHECK(second.WasClosedByPeer());

		// RpcClient reports the same.
		RpcClient refused;
		CHECK_FALSE(refused.Connect("127.0.0.1", server.GetPort(), Tests::c_TestServerToken, std::chrono::milliseconds(2000)));
		CHECK(refused.GetLastError().find("at most 1 client") != std::string::npos);

		// Once the first client leaves, a new one is accepted.
		first.Close();
		CHECK(Tests::WaitUntil([&]() { return server.GetServer().GetClientCount() == 0; }));
		RpcClient third;
		ConnectClient(third, server.GetPort());
		CHECK(third.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
	}

	TEST_CASE("Connections that hoard pending slots cannot lock out a client")
	{
		Tests::PumpedRpcServer server;
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxPendingConnections = 2;
		specification.AuthenticationTimeout = std::chrono::milliseconds(30000); // The hoarders never time out here
		REQUIRE(server.Start(specification));

		// A hostile process opens many connections and never authenticates.
		std::vector<Tests::RawRpcConnection> hoarders(6);
		for (Tests::RawRpcConnection& hoarder : hoarders)
			REQUIRE(hoarder.Connect(server.GetPort()));

		// Each connection beyond the two pending slots turns one connection away with ServerBusy: an older one
		// once it had a chance to send, else (within one burst) the new one itself. Two hoarders remain.
		std::vector<bool> turnedAway(hoarders.size(), false);
		std::vector<std::string> reasons(hoarders.size());
		auto countTurnedAway = [&]()
		{
			for (size_t index = 0; index < hoarders.size(); index++)
			{
				if (turnedAway[index])
					continue;
				std::optional<nlohmann::json> message = hoarders[index].ReadMessage(std::chrono::milliseconds(1));
				if (message && (*message)["error"]["code"] == JsonRpc::ErrorCode::ServerBusy)
				{
					turnedAway[index] = true;
					reasons[index] = (*message)["error"]["message"].get<std::string>();
				}
			}
			return static_cast<size_t>(std::count(turnedAway.begin(), turnedAway.end(), true));
		};
		REQUIRE(Tests::WaitUntil([&]() { return countTurnedAway() == hoarders.size() - 2; }));
		const std::vector<bool> turnedAwayBefore = turnedAway;

		// A legitimate client still gets in: its connection evicts the oldest hoarder, and it authenticates at once.
		RpcClient client;
		ConnectClient(client, server.GetPort());
		CHECK(client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
		CHECK(server.GetServer().GetClientCount() == 1);
		CHECK(Tests::WaitUntil([&]() { return countTurnedAway() == hoarders.size() - 1; }));

		// The evicted hoarder is told why; it was not necessarily the oldest connection, only the one chosen.
		for (size_t index = 0; index < hoarders.size(); index++)
		{
			if (turnedAway[index] && !turnedAwayBefore[index])
			{
				CHECK(reasons[index].find("chosen to make room") != std::string::npos);
				CHECK(reasons[index].find("oldest") == std::string::npos);
			}
		}

		// The newest remaining hoarder keeps its slot.
		const size_t survivor = static_cast<size_t>(std::find(turnedAway.begin(), turnedAway.end(), false) - turnedAway.begin());
		REQUIRE(survivor < hoarders.size());
		CHECK_FALSE(hoarders[survivor].ReadMessage(std::chrono::milliseconds(100)).has_value());
		CHECK_FALSE(hoarders[survivor].WasClosedByPeer());
	}

	TEST_CASE("The pending connection to evict is the oldest that had a chance to send")
	{
		using State = PendingConnectionState;
		auto choose = [](std::initializer_list<State> states) { return ChoosePendingConnectionToEvict(std::vector<State>(states)); };
		const State fresh { false, false };
		const State idle { true, false };
		const State started { true, true };
		const State freshStarted { false, true }; // Its request was read before its first poll

		// Connections that have not been through a poll yet are never evicted: the newcomer is turned away.
		CHECK_FALSE(choose({}).has_value());
		CHECK_FALSE(choose({ fresh, fresh }).has_value());
		CHECK_FALSE(choose({ freshStarted }).has_value());

		CHECK(choose({ idle, fresh }) == size_t(0));
		CHECK(choose({ fresh, idle }) == size_t(1));
		CHECK(choose({ idle, idle }) == size_t(0));

		// Connections in the middle of the handshake are evicted only when no idle one is left.
		CHECK(choose({ started, idle }) == size_t(1));
		CHECK(choose({ started, fresh, idle }) == size_t(2));
		CHECK(choose({ started, started, fresh }) == size_t(0));
		CHECK(choose({ fresh, started }) == size_t(1));
	}

	TEST_CASE("A request sent before eviction is still answered")
	{
		Tests::PumpedRpcServer server;
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxPendingConnections = 1;
		REQUIRE(server.Start(specification));

		// With one pending slot, every second connection evicts the first or is turned away. A connection that sent
		// its first request before the next one arrived is always answered: either it is kept (both were accepted
		// in one go), or what it sent is handled before it is evicted.
		for (int round = 0; round < 100; round++)
		{
			CAPTURE(round);
			Tests::RawRpcConnection first;
			REQUIRE(first.Connect(server.GetPort()));
			REQUIRE(first.SendLine(MakeRequestLine(1, "rpc.handshake", nlohmann::json { { "clientNonce", RpcAuthentication::GenerateNonce() } })));
			Tests::RawRpcConnection second;
			REQUIRE(second.Connect(server.GetPort()));
			REQUIRE(second.SendLine(MakeRequestLine(1, "rpc.handshake", nlohmann::json { { "clientNonce", RpcAuthentication::GenerateNonce() } })));

			std::optional<nlohmann::json> firstAnswer = first.ReadMessage();
			REQUIRE(firstAnswer.has_value());
			const std::string firstText = firstAnswer->dump();
			CAPTURE(firstText);
			CHECK(firstAnswer->contains("result"));

			// The second one is either answered too (the first was evicted afterwards) or turned away itself.
			std::optional<nlohmann::json> secondAnswer = second.ReadMessage();
			REQUIRE(secondAnswer.has_value());
			const bool answered = secondAnswer->contains("result");
			const bool turnedAway = secondAnswer->contains("error") && (*secondAnswer)["error"]["code"] == JsonRpc::ErrorCode::ServerBusy;
			CHECK((answered || turnedAway));
		}
	}

	TEST_CASE("Deferred responders can answer from another thread")
	{
		Tests::PumpedRpcServer server;
		std::mutex respondersMutex;
		std::vector<Ref<RpcResponder>> pending;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("frames.advance"), [&](const nlohmann::json& params, const Ref<RpcResponder>& responder)
		{
			std::scoped_lock<std::mutex> lock(respondersMutex);
			CHECK(responder->GetMethod() == "frames.advance");
			CHECK(params["count"] == 3);
			pending.push_back(responder);
		}));
		REQUIRE(server.Start());

		std::thread worker([&]()
		{
			Ref<RpcResponder> responder;
			Tests::WaitUntil([&]()
			{
				std::scoped_lock<std::mutex> lock(respondersMutex);
				if (pending.empty())
					return false;
				responder = pending.front();
				pending.clear();
				return true;
			});
			CHECK(responder != nullptr);
			if (!responder)
				return; // REQUIRE would throw outside the test thread
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			responder->Respond(RpcResult::Success(nlohmann::json { { "advanced", 3 } }));
			CHECK(responder->HasResponded());
			responder->Respond(RpcResult::Success(nlohmann::json { { "advanced", 99 } })); // Ignored with a warning
			CHECK_FALSE(responder->TryRespond(RpcResult::Success(nullptr)));
		});

		RpcClient client;
		ConnectClient(client, server.GetPort());
		const RpcResult result = client.Call("frames.advance", nlohmann::json { { "count", 3 } }, c_CallTimeout);
		worker.join();
		REQUIRE(result.IsSuccess());
		CHECK(result.GetValue()["advanced"] == 3);

		// Only one response was sent: the next call gets its own answer.
		const RpcResult ping = client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout);
		CHECK(ping.IsSuccess());
	}

	TEST_CASE("Responses produced on other threads arrive without the wake-up notifier")
	{
		ThreadCollection workers;
		Tests::PumpedRpcServer server;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.later"), [&](const nlohmann::json&, const Ref<RpcResponder>& responder)
		{
			std::scoped_lock<std::mutex> lock(workers.Mutex);
			workers.Threads.emplace_back([responder]()
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				responder->Respond(RpcResult::Success("later"));
			});
		}));
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.UseWakeupNotifier = false;
		REQUIRE(server.Start(specification));

		RpcClient client;
		ConnectClient(client, server.GetPort());
		for (int index = 0; index < 5; index++)
		{
			const auto start = std::chrono::steady_clock::now();
			const RpcResult result = client.Call("test.later", nlohmann::json::object(), c_CallTimeout);
			REQUIRE(result.IsSuccess());
			CHECK(result.GetValue() == "later");
			CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2000));
		}
	}

	TEST_CASE("A dropped responder answers with an InternalError")
	{
		Tests::PumpedRpcServer server;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.forget"), [](const nlohmann::json&, const Ref<RpcResponder>&)
		{
			// Neither responds nor keeps the responder.
		}));
		REQUIRE(server.Start());

		RpcClient client;
		ConnectClient(client, server.GetPort());
		const RpcResult result = client.Call("test.forget", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(result.IsError());
		CHECK(result.GetError().Code == JsonRpc::ErrorCode::InternalError);
		CHECK(result.GetError().Message.find("request dropped") != std::string::npos);
	}

	TEST_CASE("Requests wait for ProcessRequests on the owning thread")
	{
		RpcServer server;
		std::thread::id handlerThread;
		REQUIRE(server.RegisterMethod(MakeMethod("test.thread"), [&](const nlohmann::json&)
		{
			handlerThread = std::this_thread::get_id();
			return RpcResult::Success(true);
		}));
		REQUIRE(server.Start(Tests::MakeTestServerSpecification()));

		RpcClient client;
		ConnectClient(client, server.GetPort());
		RpcResult result = RpcResult::Failure(0, "not called");
		std::thread caller([&]() { result = client.Call("test.thread", nlohmann::json::object(), c_CallTimeout); });

		// Built-ins are answered by the network thread, registered methods only when the owner processes them.
		uint32_t processed = 0;
		Tests::WaitUntil([&]()
		{
			processed += server.ProcessRequests();
			return processed > 0;
		});
		caller.join();
		CHECK(processed == 1);
		REQUIRE(result.IsSuccess());
		CHECK(handlerThread == std::this_thread::get_id());
		CHECK(server.ProcessRequests() == 0);
	}

	TEST_CASE("A client's requests in flight are limited")
	{
		RpcServer server;
		RegisterEcho(server);
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxRequestsInFlightPerClient = 2;
		REQUIRE(server.Start(specification));

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());
		for (int id = 1; id <= 5; id++)
			REQUIRE(connection.SendLine(MakeRequestLine(id, "test.echo", nlohmann::json { { "value", std::to_string(id) } })));

		// The network thread queues at most two of the client's requests at a time; the rest wait in its buffer.
		uint32_t processed = 0;
		uint32_t largestBatch = 0;
		std::vector<int> answered;
		CHECK(Tests::WaitUntil([&]()
		{
			const uint32_t batch = server.ProcessRequests();
			processed += batch;
			largestBatch = std::max(largestBatch, batch);
			while (std::optional<nlohmann::json> response = connection.ReadMessage(std::chrono::milliseconds(1)))
				answered.push_back((*response)["id"].get<int>());
			return answered.size() == 5;
		}));
		CHECK(processed == 5);
		CHECK(largestBatch <= 2);
		std::sort(answered.begin(), answered.end());
		CHECK(answered == std::vector<int> { 1, 2, 3, 4, 5 });
	}

	TEST_CASE("The request queue is bounded")
	{
		RpcServer server;
		RegisterEcho(server);
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxQueuedRequests = 3;
		REQUIRE(server.Start(specification));

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());
		for (int id = 1; id <= 5; id++)
			REQUIRE(connection.SendLine(MakeRequestLine(id, "test.echo")));

		// Nothing processes the queue yet, so the requests beyond its capacity are rejected right away.
		std::vector<int> busy;
		for (int index = 0; index < 2; index++)
		{
			std::optional<nlohmann::json> response = connection.ReadMessage();
			REQUIRE(response.has_value());
			CHECK((*response)["error"]["code"] == JsonRpc::ErrorCode::ServerBusy);
			busy.push_back((*response)["id"].get<int>());
		}
		std::sort(busy.begin(), busy.end());
		CHECK(busy == std::vector<int> { 4, 5 });

		CHECK(server.ProcessRequests() == 3);
		for (int index = 0; index < 3; index++)
		{
			std::optional<nlohmann::json> response = connection.ReadMessage();
			REQUIRE(response.has_value());
			CHECK(response->contains("result"));
		}
	}

	TEST_CASE("A client that stops reading is throttled, not dropped")
	{
		Tests::PumpedRpcServer server;
		std::atomic<int> calls = 0;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.large"), [&calls](const nlohmann::json&)
		{
			calls++;
			return RpcResult::Success(std::string(512 * 1024, 'z'));
		}));
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxMessageSize = 1024 * 1024;
		specification.MaxRequestsInFlightPerClient = 1; // One response at a time, so only output backpressure applies
		REQUIRE(server.Start(specification));

		constexpr int c_RequestCount = 200; // 100 MiB of responses, far more than socket buffers can absorb
		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());
		std::string requests;
		for (int id = 1; id <= c_RequestCount; id++)
			requests += MakeRequestLine(id, "test.large") + "\n";
		REQUIRE(connection.GetSocket().SendAll(requests, std::chrono::milliseconds(5000)));

		// Without reading, the server stops processing once the client's output backs up.
		int stableCalls = -1;
		CHECK(Tests::WaitUntil([&]()
		{
			const int before = calls.load();
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			stableCalls = calls.load();
			return before == stableCalls && stableCalls > 0;
		}, std::chrono::milliseconds(15000)));
		CHECK(stableCalls < c_RequestCount);

		// The connection survived: reading resumes the stream in order.
		for (int id = 1; id <= 3; id++)
		{
			std::optional<nlohmann::json> response = connection.ReadMessage();
			REQUIRE(response.has_value());
			CHECK((*response)["id"] == id);
			CHECK((*response)["result"].get_ref<const std::string&>().size() == 512 * 1024);
		}
	}

	TEST_CASE("Pipelined large responses reach a client that reads them late")
	{
		Tests::PumpedRpcServer server;
		constexpr size_t c_ResponseSize = 900 * 1024;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.large"), [](const nlohmann::json&)
		{
			return RpcResult::Success(std::string(c_ResponseSize, 'p'));
		}));
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxMessageSize = 1024 * 1024;
		specification.MaxRequestsInFlightPerClient = 32;
		REQUIRE(server.Start(specification));

		// All requests are admitted at once and answered long before the client reads: about 28 MiB of responses,
		// far more than socket buffers hold. They wait in the server until the client catches up.
		constexpr int c_RequestCount = 32;
		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());
		std::string requests;
		for (int id = 1; id <= c_RequestCount; id++)
			requests += MakeRequestLine(id, "test.large") + "\n";
		REQUIRE(connection.GetSocket().SendAll(requests, std::chrono::milliseconds(5000)));
		std::this_thread::sleep_for(std::chrono::milliseconds(500));

		std::vector<int> answered;
		for (int index = 0; index < c_RequestCount; index++)
		{
			std::optional<nlohmann::json> response = connection.ReadMessage();
			REQUIRE(response.has_value());
			REQUIRE(response->contains("result"));
			CHECK((*response)["result"].get_ref<const std::string&>().size() == c_ResponseSize);
			answered.push_back((*response)["id"].get<int>());
		}
		std::vector<int> expected(c_RequestCount);
		for (int index = 0; index < c_RequestCount; index++)
			expected[index] = index + 1;
		CHECK(answered == expected);

		REQUIRE(connection.SendLine(MakeRequestLine("after", "rpc.ping")));
		std::optional<nlohmann::json> pong = connection.ReadMessage();
		REQUIRE(pong.has_value());
		CHECK((*pong)["id"] == "after");
	}

	TEST_CASE("A client that stops accepting output is disconnected after the stall timeout")
	{
		Tests::PumpedRpcServer server;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.large"), [](const nlohmann::json&)
		{
			return RpcResult::Success(std::string(512 * 1024, 's'));
		}));
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxMessageSize = 1024 * 1024;
		specification.StalledClientTimeout = std::chrono::milliseconds(300);
		REQUIRE(server.Start(specification));

		Tests::RawRpcConnection stalled;
		REQUIRE(stalled.Connect(server.GetPort()));
		REQUIRE(stalled.Authenticate());
		std::string requests;
		for (int id = 1; id <= 64; id++)
			requests += MakeRequestLine(id, "test.large") + "\n";
		REQUIRE(stalled.GetSocket().SendAll(requests, std::chrono::milliseconds(5000)));
		CHECK(server.GetServer().GetClientCount() == 1);

		// Once the socket buffers are full, nothing moves; the server gives up on the client and frees its slot.
		CHECK(Tests::WaitUntil([&]() { return server.GetServer().GetClientCount() == 0; }, std::chrono::milliseconds(15000)));

		RpcClient client;
		ConnectClient(client, server.GetPort());
		CHECK(client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
	}

	TEST_CASE("Notifications count toward a client's requests in flight")
	{
		RpcServer server;
		std::atomic<int> notified = 0;
		REQUIRE(server.RegisterMethod(MakeMethod("test.notify"), [&notified](const nlohmann::json&)
		{
			notified++;
			return RpcResult::Success(nullptr);
		}));
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxRequestsInFlightPerClient = 2;
		REQUIRE(server.Start(specification));

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());
		for (int index = 0; index < 6; index++)
			REQUIRE(connection.SendLine(JsonRpc::Serialize(JsonRpc::MakeNotification("test.notify"))));
		REQUIRE(connection.SendLine(MakeRequestLine("last", "test.notify")));

		// The network thread admits at most two of them at a time, notifications included.
		uint32_t processed = 0;
		uint32_t largestBatch = 0;
		std::optional<nlohmann::json> response;
		CHECK(Tests::WaitUntil([&]()
		{
			const uint32_t batch = server.ProcessRequests();
			processed += batch;
			largestBatch = std::max(largestBatch, batch);
			if (!response)
				response = connection.ReadMessage(std::chrono::milliseconds(1));
			return response.has_value() && processed == 7;
		}));
		CHECK(largestBatch <= 2);
		CHECK(notified == 7);
		REQUIRE(response.has_value());
		CHECK((*response)["id"] == "last");
	}

	TEST_CASE("The request queue's size in bytes is bounded")
	{
		RpcServer server;
		RegisterEcho(server);
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxQueuedBytes = 64 * 1024;
		REQUIRE(server.Start(specification));

		// Twenty requests of about 16 KiB: only about four fit in the queue's budget at a time. The rest wait in the
		// connection, and the server resumes reading as soon as ProcessRequests drains the queue.
		constexpr int c_RequestCount = 20;
		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());
		std::string requests;
		for (int id = 1; id <= c_RequestCount; id++)
			requests += MakeRequestLine(id, "test.echo", nlohmann::json { { "value", std::string(16000, 'q') } }) + "\n";

		// The server stops reading once the queue is full, so with small socket buffers the requests only go through
		// as ProcessRequests drains the queue: a helper thread sends them while this thread plays the main loop. A
		// socket is used by one thread at a time, so the responses are read only once the sender has finished.
		// They fit in the server's output allowance meanwhile, so it keeps reading.
		std::atomic<bool> sent = false;
		std::atomic<bool> sending = true;
		std::thread sender([&]()
		{
			sent = connection.GetSocket().SendAll(requests, std::chrono::milliseconds(15000));
			sending = false;
		});

		uint32_t processed = 0;
		uint32_t largestBatch = 0;
		CHECK(Tests::WaitUntil([&]()
		{
			const uint32_t batch = server.ProcessRequests();
			processed += batch;
			largestBatch = std::max(largestBatch, batch);
			return !sending.load() && processed == c_RequestCount;
		}, std::chrono::milliseconds(15000)));
		sender.join();
		CHECK(sent.load());

		std::vector<int> answered;
		while (answered.size() < c_RequestCount)
		{
			std::optional<nlohmann::json> response = connection.ReadMessage();
			if (!response)
				break;
			answered.push_back((*response)["id"].get<int>());
		}
		REQUIRE(answered.size() == c_RequestCount);
		CHECK(processed == c_RequestCount);
		CHECK(largestBatch >= 1);
		CHECK(largestBatch <= 5);
		std::sort(answered.begin(), answered.end());
		CHECK(answered.front() == 1);
		CHECK(answered.back() == c_RequestCount);
	}

	TEST_CASE("Replies stay bounded whatever the request contains")
	{
		Tests::PumpedRpcServer server;
		for (int index = 0; index < 20; index++)
			REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.method" + std::to_string(index), std::string(1024, 'd')), [](const nlohmann::json&) { return RpcResult::Success(true); }));
		RpcServerSpecification specification = Tests::MakeTestServerSpecification();
		specification.MaxMessageSize = 8 * 1024;
		REQUIRE(server.Start(specification));

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.Authenticate());

		SUBCASE("A built-in reply larger than the message size limit becomes an error")
		{
			REQUIRE(connection.SendLine(MakeRequestLine(1, "rpc.listMethods")));
			std::optional<nlohmann::json> response = connection.ReadMessage();
			REQUIRE(response.has_value());
			CHECK((*response)["id"] == 1);
			CHECK((*response)["error"]["code"] == JsonRpc::ErrorCode::InternalError);
			CHECK((*response)["error"]["message"].get<std::string>().find("exceeds the maximum message size") != std::string::npos);
		}

		SUBCASE("A long method name is shortened in the error message")
		{
			REQUIRE(connection.SendLine(MakeRequestLine(2, std::string(6000, 'm'))));
			std::optional<nlohmann::json> response = connection.ReadMessage();
			REQUIRE(response.has_value());
			CHECK((*response)["error"]["code"] == JsonRpc::ErrorCode::MethodNotFound);
			const std::string message = (*response)["error"]["message"].get<std::string>();
			CHECK(message.size() < 128);
			CHECK(message.find("...") != std::string::npos);
		}

		SUBCASE("A long request id is refused instead of echoed")
		{
			REQUIRE(connection.SendLine(MakeRequestLine(std::string(1000, 'i'), "rpc.ping")));
			std::optional<nlohmann::json> response = connection.ReadMessage();
			REQUIRE(response.has_value());
			CHECK((*response)["id"].is_null());
			CHECK((*response)["error"]["code"] == JsonRpc::ErrorCode::InvalidRequest);
		}

		// The connection stays usable.
		REQUIRE(connection.SendLine(MakeRequestLine("ping", "rpc.ping")));
		std::optional<nlohmann::json> pong = connection.ReadMessage();
		REQUIRE(pong.has_value());
		CHECK((*pong)["result"]["pong"] == true);
	}

	TEST_CASE("Unregistering a method waits for its running handler")
	{
		RpcServer server;
		std::atomic<bool> started = false;
		std::atomic<bool> finished = false;
		REQUIRE(server.RegisterMethod(MakeMethod("test.slow"), [&](const nlohmann::json&)
		{
			started = true;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			finished = true;
			return RpcResult::Success(true);
		}));
		REQUIRE(server.Start(Tests::MakeTestServerSpecification()));

		RpcClient client;
		ConnectClient(client, server.GetPort());
		std::thread caller([&]() { client.Call("test.slow", nlohmann::json::object(), c_CallTimeout); });

		// The main thread runs the handler while another thread unregisters the method.
		bool finishedWhenUnregistered = false;
		std::thread unregisterer([&]()
		{
			Tests::WaitUntil([&]() { return started.load(); });
			server.UnregisterMethod("test.slow");
			finishedWhenUnregistered = finished.load();
		});
		CHECK(Tests::WaitUntil([&]() { return server.ProcessRequests() > 0; }));
		unregisterer.join();
		caller.join();
		CHECK(finishedWhenUnregistered);
	}

	TEST_CASE("Several clients are served concurrently")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());

		constexpr int c_ClientCount = 6;
		constexpr int c_CallsPerClient = 25;
		std::atomic<int> successes = 0;
		std::vector<std::thread> threads;
		for (int clientIndex = 0; clientIndex < c_ClientCount; clientIndex++)
		{
			threads.emplace_back([&, clientIndex]()
			{
				RpcClient client;
				if (!client.Connect("127.0.0.1", server.GetPort(), Tests::c_TestServerToken, std::chrono::milliseconds(2000)))
					return;
				for (int call = 0; call < c_CallsPerClient; call++)
				{
					const std::string value = std::to_string(clientIndex) + "-" + std::to_string(call);
					const RpcResult result = client.Call("test.echo", nlohmann::json { { "value", value } }, c_CallTimeout);
					if (result.IsSuccess() && result.GetValue()["value"] == value)
						successes++;
				}
			});
		}
		for (std::thread& thread : threads)
			thread.join();
		CHECK(successes.load() == c_ClientCount * c_CallsPerClient);
	}

	TEST_CASE("Stopping the server disconnects its clients")
	{
		Tests::PumpedRpcServer server;
		Ref<RpcResponder> heldResponder;
		std::atomic<bool> held = false;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.hold"), [&](const nlohmann::json&, const Ref<RpcResponder>& responder)
		{
			heldResponder = responder;
			held = true;
		}));
		REQUIRE(server.Start());
		const uint16_t port = server.GetPort();

		RpcClient idle;
		ConnectClient(idle, port);
		RpcClient waiting;
		ConnectClient(waiting, port);

		RpcResult waitingResult = RpcResult::Failure(0, "not called");
		std::thread caller([&]() { waitingResult = waiting.Call("test.hold", nlohmann::json::object(), c_CallTimeout); });
		CHECK(Tests::WaitUntil([&]() { return held.load(); }));

		const auto start = std::chrono::steady_clock::now();
		server.Stop();
		caller.join();
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(3000));
		CHECK_FALSE(server.GetServer().IsRunning());
		CHECK(server.GetPort() == 0);

		REQUIRE(waitingResult.IsError());
		CHECK(waitingResult.GetError().Code == JsonRpc::ErrorCode::ConnectionClosed);
		CHECK_FALSE(waiting.IsConnected());

		const RpcResult idleResult = idle.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout);
		CHECK(idleResult.GetError().Code == JsonRpc::ErrorCode::ConnectionClosed);

		// A responder that outlives the session is inert.
		REQUIRE(heldResponder != nullptr);
		heldResponder->Respond(RpcResult::Success(true));
		heldResponder.reset();

		// The server can be started again.
		REQUIRE(server.Start());
		RpcClient again;
		ConnectClient(again, server.GetPort());
		CHECK(again.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
	}

	TEST_CASE("Destroying the server while another thread holds a responder is safe")
	{
		auto server = CreateScope<RpcServer>();
		std::mutex responderMutex;
		Ref<RpcResponder> heldResponder;
		REQUIRE(server->RegisterMethod(MakeMethod("test.hold"), [&](const nlohmann::json&, const Ref<RpcResponder>& responder)
		{
			std::scoped_lock<std::mutex> lock(responderMutex);
			heldResponder = responder;
		}));
		REQUIRE(server->Start(Tests::MakeTestServerSpecification()));

		RpcClient client;
		ConnectClient(client, server->GetPort());
		RpcResult result = RpcResult::Failure(0, "not called");
		std::thread caller([&]() { result = client.Call("test.hold", nlohmann::json::object(), c_CallTimeout); });
		CHECK(Tests::WaitUntil([&]()
		{
			server->ProcessRequests();
			std::scoped_lock<std::mutex> lock(responderMutex);
			return heldResponder != nullptr;
		}));

		// Another thread answers (and finally drops) the responder while the server is being destroyed.
		std::atomic<bool> destroyed = false;
		std::thread responderThread([&]()
		{
			Ref<RpcResponder> responder;
			{
				std::scoped_lock<std::mutex> lock(responderMutex);
				responder = std::move(heldResponder);
			}
			while (!destroyed.load())
				std::this_thread::yield();
			if (responder)
				responder->Respond(RpcResult::Success("too late"));
		});

		server.reset();
		destroyed = true;
		responderThread.join();
		caller.join();
		REQUIRE(result.IsError());
		CHECK(result.GetError().Code == JsonRpc::ErrorCode::ConnectionClosed);
	}

	TEST_CASE("Calls time out without losing the connection")
	{
		Tests::PumpedRpcServer server;
		Ref<RpcResponder> heldResponder;
		std::atomic<bool> held = false;
		REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.slow"), [&](const nlohmann::json&, const Ref<RpcResponder>& responder)
		{
			heldResponder = responder;
			held = true;
		}));
		REQUIRE(server.Start());

		RpcClient client;
		ConnectClient(client, server.GetPort());
		const RpcResult timedOut = client.Call("test.slow", nlohmann::json::object(), std::chrono::milliseconds(100));
		REQUIRE(timedOut.IsError());
		CHECK(timedOut.GetError().Code == JsonRpc::ErrorCode::Timeout);
		CHECK(client.IsConnected());

		// The late response is skipped; the next call gets its own result.
		REQUIRE(Tests::WaitUntil([&]() { return held.load(); }));
		heldResponder->Respond(RpcResult::Success("late"));
		const RpcResult ping = client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(ping.IsSuccess());
		CHECK(ping.GetValue()["pong"] == true);
		heldResponder.reset();

		CHECK(client.CheckConnection());
		RpcClient disconnected;
		CHECK(disconnected.Call("rpc.ping").GetError().Code == JsonRpc::ErrorCode::ConnectionClosed);
		CHECK_FALSE(disconnected.CheckConnection());
	}

	TEST_CASE("Large payloads round-trip")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());

		RpcClient client;
		ConnectClient(client, server.GetPort());

		std::string payload(8 * 1024 * 1024, '\0');
		for (size_t index = 0; index < payload.size(); index++)
			payload[index] = static_cast<char>('a' + index % 26);

		const RpcResult result = client.Call("test.echo", nlohmann::json { { "value", payload } }, std::chrono::milliseconds(30000));
		REQUIRE_MESSAGE(result.IsSuccess(), result.GetError().Message);
		REQUIRE(result.GetValue()["value"].is_string());
		CHECK(result.GetValue()["value"].get_ref<const std::string&>().size() == payload.size());
		CHECK(result.GetValue()["value"].get_ref<const std::string&>() == payload);
	}
}
