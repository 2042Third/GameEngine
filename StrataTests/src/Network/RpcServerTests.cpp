#include <doctest/doctest.h>

#include "Network/NetworkTestHelpers.h"
#include "Strata/Network/RpcClient.h"
#include "Strata/Network/RpcServer.h"
#include "TestHelpers.h"

#include <algorithm>
#include <atomic>
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

	RpcClient& ConnectClient(RpcClient& client, uint16_t port, std::string_view token = {})
	{
		REQUIRE_MESSAGE(client.Connect("127.0.0.1", port, token, std::chrono::milliseconds(2000)), client.GetLastError());
		return client;
	}
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
		CHECK(names == std::vector<std::string> { "rpc.authenticate", "rpc.ping", "rpc.listMethods", "scene.save", "test.echo" });

		const std::vector<RpcMethodInfo> methods = server.GetServer().GetMethods();
		REQUIRE(methods.size() == 5);
		CHECK(methods[3].Name == "scene.save");
		CHECK(methods[3].ParamsSchema["type"] == "object"); // A null schema is normalized
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
		CHECK(server.GetMethods().size() == 4);
		server.UnregisterMethod("a.b");
		CHECK(server.GetMethods().size() == 3);
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

	TEST_CASE("Malformed and invalid messages get JSON-RPC errors")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));

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
	}

	TEST_CASE("Oversized messages are rejected and the connection is closed")
	{
		Tests::PumpedRpcServer server;
		RpcServerSpecification specification;
		specification.MaxMessageSize = 1024;
		REQUIRE(server.Start(specification));

		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
		REQUIRE(connection.GetSocket().SendAll(std::string(4096, 'x')));

		std::optional<nlohmann::json> error = connection.ReadMessage();
		REQUIRE(error.has_value());
		CHECK((*error)["error"]["code"] == JsonRpc::ErrorCode::InvalidRequest);
		CHECK_FALSE(connection.ReadMessage().has_value());
		CHECK(connection.WasClosedByPeer());
	}

	TEST_CASE("A client that half-closes still receives its responses")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		REQUIRE(server.Start());

		// Like `printf '<request>\n' | nc`: send everything, signal the end of input, then read the answers.
		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(server.GetPort()));
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
	}

	TEST_CASE("Authentication is required when the server has a token")
	{
		Tests::PumpedRpcServer server;
		RegisterEcho(server.GetServer());
		RpcServerSpecification specification;
		specification.AuthToken = "0123456789abcdef0123456789abcdef";
		REQUIRE(server.Start(specification));

		SUBCASE("Requests before authenticating are rejected")
		{
			RpcClient client;
			ConnectClient(client, server.GetPort());
			for (const char* method : { "test.echo", "rpc.ping", "rpc.listMethods" })
			{
				const RpcResult result = client.Call(method, nlohmann::json::object(), c_CallTimeout);
				REQUIRE(result.IsError());
				CHECK(result.GetError().Code == JsonRpc::ErrorCode::Unauthorized);
			}

			const RpcResult wrongToken = client.Call("rpc.authenticate", nlohmann::json { { "token", "wrong" } }, c_CallTimeout);
			CHECK(wrongToken.GetError().Code == JsonRpc::ErrorCode::Unauthorized);
			const RpcResult missingToken = client.Call("rpc.authenticate", nlohmann::json::object(), c_CallTimeout);
			CHECK(missingToken.GetError().Code == JsonRpc::ErrorCode::InvalidParams);

			const RpcResult authenticated = client.Call("rpc.authenticate", nlohmann::json { { "token", specification.AuthToken } }, c_CallTimeout);
			REQUIRE(authenticated.IsSuccess());
			CHECK(authenticated.GetValue()["authenticated"] == true);
			CHECK(client.Call("test.echo", nlohmann::json { { "value", "ok" } }, c_CallTimeout).IsSuccess());
		}

		SUBCASE("The client authenticates while connecting")
		{
			RpcClient client;
			CHECK_FALSE(client.Connect("127.0.0.1", server.GetPort(), "wrong-token", std::chrono::milliseconds(2000)));
			CHECK_FALSE(client.IsConnected());
			CHECK(client.GetLastError().find("Authentication failed") != std::string::npos);

			ConnectClient(client, server.GetPort(), specification.AuthToken);
			CHECK(client.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
		}

		SUBCASE("Notifications before authenticating are dropped silently")
		{
			std::atomic<int> calls = 0;
			REQUIRE(server.GetServer().RegisterMethod(MakeMethod("test.count"), [&calls](const nlohmann::json&)
			{
				calls++;
				return RpcResult::Success(nullptr);
			}));

			RpcClient client;
			ConnectClient(client, server.GetPort());
			REQUIRE(client.Notify("test.count"));
			CHECK(client.Call("rpc.authenticate", nlohmann::json { { "token", specification.AuthToken } }, c_CallTimeout).IsSuccess());
			REQUIRE(client.Notify("test.count"));
			CHECK(client.Call("test.count", nlohmann::json::object(), c_CallTimeout).IsSuccess());
			CHECK(calls.load() == 2); // The authenticated notification and the call
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
		REQUIRE(server.Start({}));

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
				if (!client.Connect("127.0.0.1", server.GetPort(), {}, std::chrono::milliseconds(2000)))
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

	TEST_CASE("Connections beyond MaxClients are rejected")
	{
		Tests::PumpedRpcServer server;
		RpcServerSpecification specification;
		specification.MaxClients = 1;
		REQUIRE(server.Start(specification));

		RpcClient first;
		ConnectClient(first, server.GetPort());
		CHECK(first.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());

		RpcClient second;
		ConnectClient(second, server.GetPort()); // The TCP connection itself succeeds
		const RpcResult rejected = second.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(rejected.IsError());
		CHECK((rejected.GetError().Code == JsonRpc::ErrorCode::ServerBusy || rejected.GetError().Code == JsonRpc::ErrorCode::ConnectionClosed));

		// Once the first client leaves, a new one is accepted.
		first.Close();
		CHECK(Tests::WaitUntil([&]() { return server.GetServer().GetClientCount() == 0; }));
		RpcClient third;
		ConnectClient(third, server.GetPort());
		CHECK(third.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
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
		REQUIRE(Tests::WaitUntil([&]() { return held.load(); }));

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
		heldResponder->Respond(RpcResult::Success(true));
		heldResponder.reset();

		// The server can be started again.
		REQUIRE(server.Start());
		RpcClient again;
		ConnectClient(again, server.GetPort());
		CHECK(again.Call("rpc.ping", nlohmann::json::object(), c_CallTimeout).IsSuccess());
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
