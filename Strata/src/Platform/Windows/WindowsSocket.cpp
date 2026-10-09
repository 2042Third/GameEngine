#include "stpch.h"
#include "Strata/Network/Socket.h"

#include "Platform/Windows/WindowsUtils.h"

// WIN32_LEAN_AND_MEAN keeps <Windows.h> from pulling in the legacy <winsock.h>, so Winsock 2 can follow it.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <climits>

namespace Strata
{

	namespace
	{

		constexpr size_t c_ReceiveChunkSize = 64 * 1024;

		// Winsock must be started before any socket call. It is started on first use (WSAStartup is itself
		// reference counted, so this cooperates with other Winsock users in the process) and intentionally never
		// cleaned up: WSACleanup unloads the Winsock provider DLLs while the system may still have asynchronous
		// work queued for recently closed sockets and name lookups. Cleaning up whenever the last socket closed
		// crashed the process when it exited shortly afterwards under CPU load (reproduced by StrataTests), and
		// cleanup/startup cycles reload the providers on every reconnect. The OS releases everything at exit.
		bool EnsureWinsockStarted(std::string* error)
		{
			static const int s_StartupResult = []()
			{
				WSADATA data = {};
				return WSAStartup(MAKEWORD(2, 2), &data);
			}();

			if (s_StartupResult == 0)
				return true;
			if (error)
				*error = "WSAStartup failed: " + WindowsUtils::GetErrorMessage(static_cast<DWORD>(s_StartupResult));
			return false;
		}

		SOCKET ToNative(SocketHandle handle)
		{
			return static_cast<SOCKET>(handle);
		}

		SocketHandle FromNative(SOCKET socket)
		{
			return static_cast<SocketHandle>(socket);
		}

		std::string GetSocketErrorMessage(int errorCode)
		{
			return WindowsUtils::GetErrorMessage(static_cast<DWORD>(errorCode));
		}

		INT ToPollTimeout(std::chrono::milliseconds timeout)
		{
			if (timeout.count() <= 0)
				return 0;
			return static_cast<INT>(std::min<int64_t>(timeout.count(), INT_MAX));
		}

		std::chrono::milliseconds Remaining(std::chrono::steady_clock::time_point deadline)
		{
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			return std::max(remaining, std::chrono::milliseconds(0));
		}

		// Non-blocking mode, and no inheritance into child processes (accepted sockets inherit their
		// listener's flags, but setting them explicitly does not depend on that).
		bool ConfigureSocket(SOCKET socket)
		{
			u_long nonBlocking = 1;
			if (ioctlsocket(socket, FIONBIO, &nonBlocking) != 0)
				return false;
			SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
			return true;
		}

		// Creates a configured socket, or INVALID_SOCKET.
		SOCKET CreateSocket(int family, int type, int protocol, std::string* error)
		{
			const SOCKET socket = WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
			if (socket == INVALID_SOCKET)
			{
				if (error)
					*error = "socket() failed: " + GetSocketErrorMessage(WSAGetLastError());
				return INVALID_SOCKET;
			}

			if (!ConfigureSocket(socket))
			{
				if (error)
					*error = "Failed to configure socket: " + GetSocketErrorMessage(WSAGetLastError());
				closesocket(socket);
				return INVALID_SOCKET;
			}
			return socket;
		}

		bool IsLoopback(const sockaddr* address)
		{
			if (address->sa_family == AF_INET)
				return (ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr) >> 24) == 127;
			if (address->sa_family == AF_INET6)
				return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr) != 0;
			return false;
		}

		// Windows retries a refused connection attempt (SYN answered by RST) for about two seconds. For loopback
		// targets a refusal is definitive, so disable the retries to make probing stale editor sessions fast.
		void DisableSynRetransmissions(SOCKET socket)
		{
			TCP_INITIAL_RTO_PARAMETERS parameters = {};
			parameters.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT;
			parameters.MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS;
			DWORD bytesReturned = 0;
			WSAIoctl(socket, SIO_TCP_INITIAL_RTO, &parameters, sizeof(parameters), nullptr, 0, &bytesReturned, nullptr, nullptr);
		}

		// Waits for a non-blocking connect to finish. select() is used instead of WSAPoll because WSAPoll does
		// not report failed connection attempts on older Windows 10 builds.
		bool WaitForConnect(SOCKET socket, std::chrono::steady_clock::time_point deadline, std::string& error)
		{
			const std::chrono::milliseconds remaining = Remaining(deadline);
			timeval timeout = {};
			timeout.tv_sec = static_cast<long>(remaining.count() / 1000);
			timeout.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);

			// fd_set is filled directly: the FD_SET macro trips /W4 constant-condition warnings.
			fd_set writeSet = {};
			writeSet.fd_count = 1;
			writeSet.fd_array[0] = socket;
			fd_set exceptSet = writeSet;

			const int ready = select(0, nullptr, &writeSet, &exceptSet, &timeout);
			if (ready == SOCKET_ERROR)
			{
				error = "select() failed: " + GetSocketErrorMessage(WSAGetLastError());
				return false;
			}
			if (ready == 0)
			{
				error = "Connection timed out";
				return false;
			}

			int socketError = 0;
			int length = sizeof(socketError);
			if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &length) != 0)
				socketError = WSAGetLastError();
			if (socketError != 0 || exceptSet.fd_count > 0)
			{
				error = GetSocketErrorMessage(socketError != 0 ? socketError : WSAECONNREFUSED);
				return false;
			}
			return true;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// TcpSocket
	////////////////////////////////////////////////////////////////////////////////

	std::optional<TcpSocket> TcpSocket::Connect(std::string_view host, uint16_t port, std::chrono::milliseconds timeout, std::string* error)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
		std::string lastError;

		if (!EnsureWinsockStarted(&lastError))
		{
			if (error)
				*error = lastError;
			return std::nullopt;
		}

		ADDRINFOW hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_NUMERICSERV;

		const std::wstring wideHost = WindowsUtils::Utf8ToWide(host);
		const std::wstring widePort = std::to_wstring(port);
		ADDRINFOW* addresses = nullptr;
		const int lookupResult = GetAddrInfoW(wideHost.c_str(), widePort.c_str(), &hints, &addresses);
		if (lookupResult != 0)
		{
			if (error)
				*error = fmt::format("Failed to resolve '{}': {}", host, GetSocketErrorMessage(lookupResult));
			return std::nullopt;
		}

		std::optional<TcpSocket> connected;
		for (ADDRINFOW* address = addresses; address && !connected; address = address->ai_next)
		{
			const SOCKET socket = CreateSocket(address->ai_family, address->ai_socktype, address->ai_protocol, &lastError);
			if (socket == INVALID_SOCKET)
				continue;

			if (IsLoopback(address->ai_addr))
				DisableSynRetransmissions(socket);

			bool success = connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
			if (!success)
			{
				const int connectError = WSAGetLastError();
				if (connectError == WSAEWOULDBLOCK)
					success = WaitForConnect(socket, deadline, lastError);
				else
					lastError = GetSocketErrorMessage(connectError);
			}

			if (success)
				connected = TcpSocket(FromNative(socket));
			else
				closesocket(socket);
		}

		FreeAddrInfoW(addresses);

		if (!connected && error)
			*error = fmt::format("Failed to connect to {}:{}: {}", host, port, lastError.empty() ? std::string("no usable address") : lastError);
		return connected;
	}

	std::optional<size_t> TcpSocket::SendSome(std::span<const uint8_t> data)
	{
		if (!IsValid())
			return std::nullopt;
		if (data.empty())
			return size_t(0);

		const int length = static_cast<int>(std::min<size_t>(data.size(), INT_MAX));
		while (true)
		{
			const int sent = send(ToNative(m_Handle), reinterpret_cast<const char*>(data.data()), length, 0);
			if (sent >= 0)
				return static_cast<size_t>(sent);

			const int sendError = WSAGetLastError();
			if (sendError == WSAEINTR)
				continue;
			if (sendError == WSAEWOULDBLOCK)
				return size_t(0);
			return std::nullopt;
		}
	}

	SocketReceiveStatus TcpSocket::Receive(std::vector<uint8_t>& appendTo, std::chrono::milliseconds timeout)
	{
		if (!IsValid())
			return SocketReceiveStatus::Error;

		const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
		while (true)
		{
			SocketPollEntry entry;
			entry.Handle = m_Handle;
			entry.WantRead = true;
			if (!SocketPoller::Poll(std::span<SocketPollEntry>(&entry, 1), Remaining(deadline)))
				return SocketReceiveStatus::Error;
			if (!entry.Readable)
				return SocketReceiveStatus::Timeout;

			const size_t previousSize = appendTo.size();
			appendTo.resize(previousSize + c_ReceiveChunkSize);
			const int received = recv(ToNative(m_Handle), reinterpret_cast<char*>(appendTo.data() + previousSize), static_cast<int>(c_ReceiveChunkSize), 0);
			if (received > 0)
			{
				appendTo.resize(previousSize + static_cast<size_t>(received));
				return SocketReceiveStatus::Data;
			}

			appendTo.resize(previousSize);
			if (received == 0)
				return SocketReceiveStatus::Closed;

			const int receiveError = WSAGetLastError();
			if (receiveError == WSAEINTR || receiveError == WSAEWOULDBLOCK)
			{
				// Spurious readiness: wait again for whatever time is left.
				if (Remaining(deadline).count() == 0)
					return SocketReceiveStatus::Timeout;
				continue;
			}
			if (receiveError == WSAEDISCON || receiveError == WSAESHUTDOWN)
				return SocketReceiveStatus::Closed;
			return SocketReceiveStatus::Error;
		}
	}

	bool TcpSocket::ShutdownSend()
	{
		return IsValid() && shutdown(ToNative(m_Handle), SD_SEND) == 0;
	}

	void TcpSocket::Close()
	{
		if (!IsValid())
			return;
		closesocket(ToNative(m_Handle));
		m_Handle = c_InvalidSocketHandle;
	}

	bool TcpSocket::SetNoDelay(bool enabled)
	{
		if (!IsValid())
			return false;
		const BOOL value = enabled ? TRUE : FALSE;
		return setsockopt(ToNative(m_Handle), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// TcpListener
	////////////////////////////////////////////////////////////////////////////////

	bool TcpListener::Listen(std::string_view bindAddress, uint16_t port)
	{
		Close();
		m_LastError.clear();

		if (!EnsureWinsockStarted(&m_LastError))
			return false;

		ADDRINFOW hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

		const std::wstring wideAddress = WindowsUtils::Utf8ToWide(bindAddress);
		const std::wstring widePort = std::to_wstring(port);
		ADDRINFOW* addresses = nullptr;
		const int lookupResult = GetAddrInfoW(bindAddress.empty() ? nullptr : wideAddress.c_str(), widePort.c_str(), &hints, &addresses);
		if (lookupResult != 0)
		{
			m_LastError = fmt::format("Failed to resolve '{}': {}", bindAddress, GetSocketErrorMessage(lookupResult));
			return false;
		}

		for (ADDRINFOW* address = addresses; address && !IsListening(); address = address->ai_next)
		{
			const SOCKET socket = CreateSocket(address->ai_family, address->ai_socktype, address->ai_protocol, &m_LastError);
			if (socket == INVALID_SOCKET)
				continue;

			// Prevent other processes from binding the same port while we listen on it.
			const BOOL exclusive = TRUE;
			setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

			if (bind(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) != 0)
			{
				m_LastError = fmt::format("bind() failed: {}", GetSocketErrorMessage(WSAGetLastError()));
				closesocket(socket);
				continue;
			}
			if (listen(socket, SOMAXCONN) != 0)
			{
				m_LastError = fmt::format("listen() failed: {}", GetSocketErrorMessage(WSAGetLastError()));
				closesocket(socket);
				continue;
			}

			sockaddr_storage boundAddress = {};
			int boundLength = sizeof(boundAddress);
			if (getsockname(socket, reinterpret_cast<sockaddr*>(&boundAddress), &boundLength) != 0)
			{
				m_LastError = fmt::format("getsockname() failed: {}", GetSocketErrorMessage(WSAGetLastError()));
				closesocket(socket);
				continue;
			}

			if (boundAddress.ss_family == AF_INET6)
				m_Port = ntohs(reinterpret_cast<const sockaddr_in6*>(&boundAddress)->sin6_port);
			else
				m_Port = ntohs(reinterpret_cast<const sockaddr_in*>(&boundAddress)->sin_port);
			m_Handle = FromNative(socket);
		}

		FreeAddrInfoW(addresses);

		if (!IsListening())
		{
			if (m_LastError.empty())
				m_LastError = "No usable address";
			m_LastError = fmt::format("Failed to listen on {}:{}: {}", bindAddress, port, m_LastError);
			return false;
		}
		m_LastError.clear();
		return true;
	}

	std::optional<TcpSocket> TcpListener::Accept(std::chrono::milliseconds timeout)
	{
		if (!IsListening())
			return std::nullopt;

		const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
		while (true)
		{
			SocketPollEntry entry;
			entry.Handle = m_Handle;
			entry.WantRead = true;
			if (!SocketPoller::Poll(std::span<SocketPollEntry>(&entry, 1), Remaining(deadline)) || !entry.Readable)
				return std::nullopt;

			const SOCKET socket = accept(ToNative(m_Handle), nullptr, nullptr);
			if (socket != INVALID_SOCKET)
			{
				if (ConfigureSocket(socket))
					return TcpSocket(FromNative(socket));
				closesocket(socket);
				return std::nullopt;
			}

			const int acceptError = WSAGetLastError();
			// The pending connection may have been reset before we got to it; keep waiting for another one.
			const bool transient = acceptError == WSAEWOULDBLOCK || acceptError == WSAECONNRESET || acceptError == WSAEINTR;
			if (!transient)
			{
				m_LastError = "accept() failed: " + GetSocketErrorMessage(acceptError);
				return std::nullopt;
			}
			if (Remaining(deadline).count() == 0)
				return std::nullopt;
		}
	}

	void TcpListener::Close()
	{
		if (!IsListening())
			return;
		closesocket(ToNative(m_Handle));
		m_Handle = c_InvalidSocketHandle;
		m_Port = 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// SocketPoller
	////////////////////////////////////////////////////////////////////////////////

	bool SocketPoller::Poll(std::span<SocketPollEntry> entries, std::chrono::milliseconds timeout)
	{
		std::vector<WSAPOLLFD> descriptors;
		std::vector<size_t> entryIndices;
		descriptors.reserve(entries.size());
		entryIndices.reserve(entries.size());
		for (size_t index = 0; index < entries.size(); index++)
		{
			SocketPollEntry& entry = entries[index];
			entry.Readable = false;
			entry.Writable = false;
			if (entry.Handle == c_InvalidSocketHandle || (!entry.WantRead && !entry.WantWrite))
				continue;

			WSAPOLLFD descriptor = {};
			descriptor.fd = ToNative(entry.Handle);
			descriptor.events = static_cast<SHORT>((entry.WantRead ? POLLRDNORM : 0) | (entry.WantWrite ? POLLWRNORM : 0));
			descriptors.push_back(descriptor);
			entryIndices.push_back(index);
		}

		if (descriptors.empty())
		{
			// Nothing to wait for: honor the timeout so callers never spin.
			std::this_thread::sleep_for(std::max(timeout, std::chrono::milliseconds(0)));
			return true;
		}

		const int ready = WSAPoll(descriptors.data(), static_cast<ULONG>(descriptors.size()), ToPollTimeout(timeout));
		if (ready == SOCKET_ERROR)
			return false;

		for (size_t index = 0; index < descriptors.size(); index++)
		{
			const SHORT events = descriptors[index].revents;
			SocketPollEntry& entry = entries[entryIndices[index]];
			const bool failed = (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;
			entry.Readable = entry.WantRead && ((events & POLLRDNORM) != 0 || failed);
			entry.Writable = entry.WantWrite && ((events & POLLWRNORM) != 0 || failed);
		}
		return true;
	}

}
