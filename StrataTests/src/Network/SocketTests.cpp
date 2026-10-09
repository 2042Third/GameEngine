#include <doctest/doctest.h>

#include "Strata/Network/Socket.h"
#include "TestHelpers.h"

#include <chrono>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{
	struct SocketPair
	{
		TcpSocket Client;
		TcpSocket Server;
	};

	SocketPair ConnectPair(TcpListener& listener)
	{
		SocketPair pair;
		std::optional<TcpSocket> client = TcpSocket::Connect("127.0.0.1", listener.GetPort(), std::chrono::milliseconds(2000));
		REQUIRE(client.has_value());
		std::optional<TcpSocket> server = listener.Accept(std::chrono::milliseconds(2000));
		REQUIRE(server.has_value());
		pair.Client = std::move(*client);
		pair.Server = std::move(*server);
		return pair;
	}

	// Receives until expectedSize bytes arrived, the peer closed, or the timeout elapsed.
	std::vector<uint8_t> ReceiveExactly(TcpSocket& socket, size_t expectedSize, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
	{
		std::vector<uint8_t> received;
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (received.size() < expectedSize && std::chrono::steady_clock::now() < deadline)
		{
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			const SocketReceiveStatus status = socket.Receive(received, remaining);
			if (status == SocketReceiveStatus::Closed || status == SocketReceiveStatus::Error)
				break;
		}
		return received;
	}

	std::string ToString(const std::vector<uint8_t>& bytes)
	{
		return std::string(bytes.begin(), bytes.end());
	}
}

TEST_SUITE("Network.Socket")
{
	TEST_CASE("Listener binds an ephemeral loopback port")
	{
		TcpListener listener;
		CHECK_FALSE(listener.IsListening());
		REQUIRE_MESSAGE(listener.Listen(), listener.GetLastError());
		CHECK(listener.IsListening());
		CHECK(listener.GetPort() != 0);

		TcpListener second;
		REQUIRE(second.Listen("127.0.0.1", 0));
		CHECK(second.GetPort() != listener.GetPort());

		// A port that is already listening cannot be taken by another listener.
		TcpListener conflicting;
		CHECK_FALSE(conflicting.Listen("127.0.0.1", listener.GetPort()));
		CHECK_FALSE(conflicting.GetLastError().empty());

		listener.Close();
		CHECK_FALSE(listener.IsListening());
		CHECK(listener.GetPort() == 0);
		CHECK_FALSE(listener.Accept(std::chrono::milliseconds(0)).has_value());
	}

	TEST_CASE("Loopback connection sends and receives in both directions")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());
		SocketPair pair = ConnectPair(listener);
		CHECK(pair.Client.IsValid());
		CHECK(pair.Server.IsValid());
		CHECK(pair.Client.SetNoDelay(true));

		REQUIRE(pair.Client.SendAll(std::string_view("hello server")));
		CHECK(ToString(ReceiveExactly(pair.Server, 12)) == "hello server");

		const std::vector<uint8_t> reply = { 0, 1, 2, 255, 10, 13 };
		REQUIRE(pair.Server.SendAll(std::span<const uint8_t>(reply)));
		CHECK(ReceiveExactly(pair.Client, reply.size()) == reply);

		CHECK(pair.Client.SendSome(std::span<const uint8_t>()).value() == 0);
	}

	TEST_CASE("Large transfers arrive intact")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());
		SocketPair pair = ConnectPair(listener);

		std::vector<uint8_t> payload(4 * 1024 * 1024);
		for (size_t index = 0; index < payload.size(); index++)
			payload[index] = static_cast<uint8_t>((index * 31) ^ (index >> 9));

		// The payload exceeds the socket buffers, so the sender must wait for the receiver to drain them.
		bool sent = false;
		std::thread sender([&]() { sent = pair.Client.SendAll(std::span<const uint8_t>(payload), std::chrono::milliseconds(10000)); });
		const std::vector<uint8_t> received = ReceiveExactly(pair.Server, payload.size(), std::chrono::milliseconds(10000));
		sender.join();

		CHECK(sent);
		CHECK(received.size() == payload.size());
		CHECK(received == payload);
	}

	TEST_CASE("Receive times out when no data arrives")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());
		SocketPair pair = ConnectPair(listener);

		std::vector<uint8_t> buffer;
		const auto start = std::chrono::steady_clock::now();
		CHECK(pair.Server.Receive(buffer, std::chrono::milliseconds(50)) == SocketReceiveStatus::Timeout);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		CHECK(elapsed >= std::chrono::milliseconds(40));
		CHECK(elapsed < std::chrono::milliseconds(2000));
		CHECK(buffer.empty());

		CHECK(pair.Server.Receive(buffer, std::chrono::milliseconds(0)) == SocketReceiveStatus::Timeout);
	}

	TEST_CASE("A closed peer is detected")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());
		SocketPair pair = ConnectPair(listener);

		REQUIRE(pair.Client.SendAll(std::string_view("last words")));
		pair.Client.Close();
		CHECK_FALSE(pair.Client.IsValid());

		// Data sent before the close is still delivered, then the orderly shutdown is reported.
		CHECK(ToString(ReceiveExactly(pair.Server, 10)) == "last words");
		std::vector<uint8_t> buffer;
		CHECK(pair.Server.Receive(buffer, std::chrono::milliseconds(2000)) == SocketReceiveStatus::Closed);

		// Operations on a closed socket fail without crashing (and never raise SIGPIPE).
		CHECK(pair.Client.Receive(buffer, std::chrono::milliseconds(0)) == SocketReceiveStatus::Error);
		CHECK_FALSE(pair.Client.SendAll(std::string_view("x")));
		CHECK_FALSE(pair.Client.SetNoDelay(true));
	}

	TEST_CASE("Half-closing ends the stream while the other direction stays open")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());
		SocketPair pair = ConnectPair(listener);

		REQUIRE(pair.Client.SendAll(std::string_view("request")));
		REQUIRE(pair.Client.ShutdownSend());
		CHECK(ToString(ReceiveExactly(pair.Server, 7)) == "request");
		std::vector<uint8_t> buffer;
		CHECK(pair.Server.Receive(buffer, std::chrono::milliseconds(2000)) == SocketReceiveStatus::Closed);

		REQUIRE(pair.Server.SendAll(std::string_view("response")));
		CHECK(ToString(ReceiveExactly(pair.Client, 8)) == "response");

		TcpSocket invalid;
		CHECK_FALSE(invalid.ShutdownSend());
	}

	TEST_CASE("Writing to a peer that went away fails instead of crashing")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());
		SocketPair pair = ConnectPair(listener);
		pair.Client.Close();

		// The first writes may still be buffered locally; keep writing until the reset is reported.
		const std::vector<uint8_t> chunk(64 * 1024, 7);
		bool failed = Tests::WaitUntil([&]() { return !pair.Server.SendAll(std::span<const uint8_t>(chunk), std::chrono::milliseconds(100)); }, std::chrono::milliseconds(5000));
		CHECK(failed);
	}

	TEST_CASE("Connecting to a closed port fails promptly")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());
		const uint16_t port = listener.GetPort();
		listener.Close();

		std::string error;
		const auto start = std::chrono::steady_clock::now();
		std::optional<TcpSocket> socket = TcpSocket::Connect("127.0.0.1", port, std::chrono::milliseconds(5000), &error);
		CHECK_FALSE(socket.has_value());
		CHECK_FALSE(error.empty());
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(3000));
	}

	TEST_CASE("Connecting by name tries every resolved address")
	{
		// "localhost" may resolve to ::1 first while the listener only accepts IPv4; the IPv4 address must be tried next.
		TcpListener listener;
		REQUIRE(listener.Listen("127.0.0.1", 0));
		std::string error;
		std::optional<TcpSocket> socket = TcpSocket::Connect("localhost", listener.GetPort(), std::chrono::milliseconds(3000), &error);
		REQUIRE_MESSAGE(socket.has_value(), error);
		CHECK(listener.Accept(std::chrono::milliseconds(2000)).has_value());
	}

	TEST_CASE("Sockets are move-only owners of their handle")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());
		SocketPair pair = ConnectPair(listener);

		const SocketHandle handle = pair.Client.GetHandle();
		TcpSocket moved = std::move(pair.Client);
		CHECK_FALSE(pair.Client.IsValid());
		CHECK(moved.GetHandle() == handle);

		TcpSocket assigned;
		assigned = std::move(moved);
		CHECK_FALSE(moved.IsValid());
		CHECK(assigned.SendAll(std::string_view("ok")));
		CHECK(ToString(ReceiveExactly(pair.Server, 2)) == "ok");

		TcpListener movedListener = std::move(listener);
		CHECK_FALSE(listener.IsListening());
		CHECK(movedListener.IsListening());
		CHECK(movedListener.GetPort() != 0);
	}

	TEST_CASE("Poller reports readiness and the notifier wakes it up")
	{
		SocketNotifier notifier;
		REQUIRE(notifier.Open());
		REQUIRE(notifier.IsValid());

		SocketPollEntry entry;
		entry.Handle = notifier.GetHandle();
		entry.WantRead = true;
		REQUIRE(SocketPoller::Poll(std::span<SocketPollEntry>(&entry, 1), std::chrono::milliseconds(0)));
		CHECK_FALSE(entry.Readable);

		std::thread notifierThread([&]()
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			notifier.Notify();
			notifier.Notify();
		});
		const auto start = std::chrono::steady_clock::now();
		REQUIRE(SocketPoller::Poll(std::span<SocketPollEntry>(&entry, 1), std::chrono::milliseconds(5000)));
		CHECK(entry.Readable);
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2000));
		notifierThread.join();

		// Draining consumes every pending wake-up.
		notifier.Drain();
		REQUIRE(SocketPoller::Poll(std::span<SocketPollEntry>(&entry, 1), std::chrono::milliseconds(0)));
		CHECK_FALSE(entry.Readable);

		// Entries without a valid handle are ignored, and an empty poll honors its timeout.
		SocketPollEntry invalid;
		invalid.WantRead = true;
		CHECK(SocketPoller::Poll(std::span<SocketPollEntry>(&invalid, 1), std::chrono::milliseconds(1)));
		CHECK_FALSE(invalid.Readable);

		notifier.Close();
		CHECK_FALSE(notifier.IsValid());
		notifier.Notify(); // Harmless after closing
	}

	TEST_CASE("Poller reports writability and listener readiness")
	{
		TcpListener listener;
		REQUIRE(listener.Listen());

		std::optional<TcpSocket> client = TcpSocket::Connect("127.0.0.1", listener.GetPort(), std::chrono::milliseconds(2000));
		REQUIRE(client.has_value());

		std::vector<SocketPollEntry> entries(2);
		entries[0].Handle = listener.GetHandle();
		entries[0].WantRead = true;
		entries[1].Handle = client->GetHandle();
		entries[1].WantWrite = true;
		REQUIRE(SocketPoller::Poll(entries, std::chrono::milliseconds(2000)));
		CHECK(entries[1].Writable);
		CHECK_FALSE(entries[1].Readable);

		CHECK(Tests::WaitUntil([&]()
		{
			SocketPollEntry entry;
			entry.Handle = listener.GetHandle();
			entry.WantRead = true;
			return SocketPoller::Poll(std::span<SocketPollEntry>(&entry, 1), std::chrono::milliseconds(100)) && entry.Readable;
		}, std::chrono::milliseconds(2000)));
		CHECK(listener.Accept(std::chrono::milliseconds(0)).has_value());
	}
}
