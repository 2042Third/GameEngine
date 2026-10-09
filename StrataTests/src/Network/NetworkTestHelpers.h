#pragma once

#include "Strata/Core/Platform.h"
#include "Strata/Network/JsonRpc.h"
#include "Strata/Network/RpcServer.h"
#include "Strata/Network/Socket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Strata::Tests
{

	// An RpcServer whose queued requests are processed by a helper thread standing in for the main loop.
	class PumpedRpcServer
	{
	public:
		PumpedRpcServer() = default;
		~PumpedRpcServer()
		{
			Stop();
		}

		PumpedRpcServer(const PumpedRpcServer&) = delete;
		PumpedRpcServer& operator=(const PumpedRpcServer&) = delete;

		bool Start(const RpcServerSpecification& specification = {})
		{
			if (!m_Server.Start(specification))
				return false;

			m_Pumping = true;
			m_Thread = std::thread([this]()
			{
				while (m_Pumping.load())
				{
					m_Server.ProcessRequests();
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});
			return true;
		}

		void Stop()
		{
			m_Pumping = false;
			if (m_Thread.joinable())
				m_Thread.join();
			m_Server.Stop();
		}

		RpcServer& GetServer() { return m_Server; }
		uint16_t GetPort() const { return m_Server.GetPort(); }
	private:
		RpcServer m_Server;
		std::thread m_Thread;
		std::atomic<bool> m_Pumping = false;
	};

	// Raw line-based connection for tests that send malformed or hand-crafted messages.
	class RawRpcConnection
	{
	public:
		bool Connect(uint16_t port)
		{
			std::optional<TcpSocket> socket = TcpSocket::Connect("127.0.0.1", port, std::chrono::milliseconds(2000));
			if (!socket)
				return false;
			m_Socket = std::move(*socket);
			return true;
		}

		bool SendLine(std::string_view text)
		{
			std::string line(text);
			line += '\n';
			return m_Socket.SendAll(line, std::chrono::milliseconds(5000));
		}

		// Reads the next message; nullopt on timeout, connection close or invalid JSON.
		std::optional<nlohmann::json> ReadMessage(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
		{
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			while (true)
			{
				if (std::optional<std::string> line = m_Reader.NextLine())
					return JsonRpc::Parse(*line);

				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
				if (remaining.count() <= 0)
					return std::nullopt;

				std::vector<uint8_t> buffer;
				const SocketReceiveStatus status = m_Socket.Receive(buffer, remaining);
				if (status == SocketReceiveStatus::Closed || status == SocketReceiveStatus::Error)
				{
					m_PeerClosed = true;
					return std::nullopt;
				}
				m_Reader.Append(buffer);
			}
		}

		bool WasClosedByPeer() const { return m_PeerClosed; }
		TcpSocket& GetSocket() { return m_Socket; }
	private:
		TcpSocket m_Socket;
		JsonLineReader m_Reader;
		bool m_PeerClosed = false;
	};

	// Sets an environment variable for the lifetime of the object. An empty value counts as unset for every
	// variable Strata reads, so restoring an absent variable sets it to empty.
	class ScopedEnvironmentVariable
	{
	public:
		ScopedEnvironmentVariable(std::string name, const std::string& value)
			: m_Name(std::move(name)), m_Previous(Platform::GetEnvVar(m_Name))
		{
			Platform::SetEnvVar(m_Name, value);
		}

		~ScopedEnvironmentVariable()
		{
			Platform::SetEnvVar(m_Name, m_Previous.value_or(std::string()));
		}

		ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
		ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;
	private:
		std::string m_Name;
		std::optional<std::string> m_Previous;
	};

}
