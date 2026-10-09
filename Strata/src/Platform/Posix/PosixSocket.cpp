#include "stpch.h"
#include "Strata/Network/Socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace Strata
{

	namespace
	{

		constexpr size_t c_ReceiveChunkSize = 64 * 1024;

		// Writing to a connection the peer has closed must fail with EPIPE instead of killing the process with
		// SIGPIPE: Linux suppresses the signal per call (MSG_NOSIGNAL), macOS per socket (SO_NOSIGPIPE).
#if defined(MSG_NOSIGNAL)
		constexpr int c_SendFlags = MSG_NOSIGNAL;
#else
		constexpr int c_SendFlags = 0;
#endif

		int ToNative(SocketHandle handle)
		{
			return static_cast<int>(handle);
		}

		SocketHandle FromNative(int descriptor)
		{
			return static_cast<SocketHandle>(descriptor);
		}

		bool IsWouldBlock(int errorCode)
		{
#if EAGAIN == EWOULDBLOCK
			return errorCode == EAGAIN;
#else
			return errorCode == EAGAIN || errorCode == EWOULDBLOCK;
#endif
		}

		std::string GetErrorMessage(int errorCode)
		{
			return std::strerror(errorCode);
		}

		int ToPollTimeout(std::chrono::milliseconds timeout)
		{
			if (timeout.count() <= 0)
				return 0;
			return static_cast<int>(std::min<int64_t>(timeout.count(), INT_MAX));
		}

		std::chrono::milliseconds Remaining(std::chrono::steady_clock::time_point deadline)
		{
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			return std::max(remaining, std::chrono::milliseconds(0));
		}

		// Non-blocking, close-on-exec (child processes must not inherit connections) and no SIGPIPE.
		bool ConfigureSocket(int descriptor)
		{
			const int flags = fcntl(descriptor, F_GETFL, 0);
			if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
				return false;
			if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0)
				return false;
#if defined(SO_NOSIGPIPE)
			const int noSigPipe = 1;
			if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)) != 0)
				return false;
#endif
			return true;
		}

		int CreateSocket(int family, int type, int protocol, std::string* error)
		{
#if defined(SOCK_CLOEXEC)
			// Set close-on-exec atomically where supported, so a concurrent fork + exec cannot inherit the socket.
			type |= SOCK_CLOEXEC;
#endif
			const int descriptor = socket(family, type, protocol);
			if (descriptor < 0)
			{
				if (error)
					*error = "socket() failed: " + GetErrorMessage(errno);
				return -1;
			}

			if (!ConfigureSocket(descriptor))
			{
				if (error)
					*error = "Failed to configure socket: " + GetErrorMessage(errno);
				close(descriptor);
				return -1;
			}
			return descriptor;
		}

		// Waits for a non-blocking connect to finish and reports its outcome.
		bool WaitForConnect(int descriptor, std::chrono::steady_clock::time_point deadline, std::string& error)
		{
			while (true)
			{
				pollfd entry = {};
				entry.fd = descriptor;
				entry.events = POLLOUT;
				const int ready = poll(&entry, 1, ToPollTimeout(Remaining(deadline)));
				if (ready < 0)
				{
					if (errno == EINTR)
						continue;
					error = "poll() failed: " + GetErrorMessage(errno);
					return false;
				}
				if (ready == 0)
				{
					error = "Connection timed out";
					return false;
				}
				break;
			}

			int socketError = 0;
			socklen_t length = sizeof(socketError);
			if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0)
				socketError = errno;
			if (socketError != 0)
			{
				error = GetErrorMessage(socketError);
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

		addrinfo hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_NUMERICSERV;

		const std::string hostName(host);
		const std::string portText = std::to_string(port);
		addrinfo* addresses = nullptr;
		const int lookupResult = getaddrinfo(hostName.c_str(), portText.c_str(), &hints, &addresses);
		if (lookupResult != 0)
		{
			if (error)
				*error = fmt::format("Failed to resolve '{}': {}", host, gai_strerror(lookupResult));
			return std::nullopt;
		}

		std::optional<TcpSocket> connected;
		for (addrinfo* address = addresses; address && !connected; address = address->ai_next)
		{
			const int descriptor = CreateSocket(address->ai_family, address->ai_socktype, address->ai_protocol, &lastError);
			if (descriptor < 0)
				continue;

			bool success = false;
			while (true)
			{
				if (connect(descriptor, address->ai_addr, address->ai_addrlen) == 0)
				{
					success = true;
					break;
				}
				if (errno == EINTR)
					continue; // The connection attempt keeps going; retrying reports EALREADY or EISCONN
				if (errno == EINPROGRESS || errno == EALREADY)
					success = WaitForConnect(descriptor, deadline, lastError);
				else if (errno == EISCONN)
					success = true;
				else
					lastError = GetErrorMessage(errno);
				break;
			}

			if (success)
				connected = TcpSocket(FromNative(descriptor));
			else
				close(descriptor);
		}

		freeaddrinfo(addresses);

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

		while (true)
		{
			const ssize_t sent = send(ToNative(m_Handle), data.data(), data.size(), c_SendFlags);
			if (sent >= 0)
				return static_cast<size_t>(sent);
			if (errno == EINTR)
				continue;
			if (IsWouldBlock(errno))
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
			const ssize_t received = recv(ToNative(m_Handle), appendTo.data() + previousSize, c_ReceiveChunkSize, 0);
			if (received > 0)
			{
				appendTo.resize(previousSize + static_cast<size_t>(received));
				return SocketReceiveStatus::Data;
			}

			const int receiveError = errno;
			appendTo.resize(previousSize);
			if (received == 0)
				return SocketReceiveStatus::Closed;

			if (receiveError == EINTR || IsWouldBlock(receiveError))
			{
				// Spurious readiness: wait again for whatever time is left.
				if (Remaining(deadline).count() == 0)
					return SocketReceiveStatus::Timeout;
				continue;
			}
			return SocketReceiveStatus::Error;
		}
	}

	bool TcpSocket::ShutdownSend()
	{
		return IsValid() && shutdown(ToNative(m_Handle), SHUT_WR) == 0;
	}

	void TcpSocket::Close()
	{
		if (!IsValid())
			return;
		close(ToNative(m_Handle));
		m_Handle = c_InvalidSocketHandle;
	}

	bool TcpSocket::SetNoDelay(bool enabled)
	{
		if (!IsValid())
			return false;
		const int value = enabled ? 1 : 0;
		return setsockopt(ToNative(m_Handle), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) == 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// TcpListener
	////////////////////////////////////////////////////////////////////////////////

	bool TcpListener::Listen(std::string_view bindAddress, uint16_t port)
	{
		Close();
		m_LastError.clear();

		addrinfo hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

		const std::string addressText(bindAddress);
		const std::string portText = std::to_string(port);
		addrinfo* addresses = nullptr;
		const int lookupResult = getaddrinfo(addressText.empty() ? nullptr : addressText.c_str(), portText.c_str(), &hints, &addresses);
		if (lookupResult != 0)
		{
			m_LastError = fmt::format("Failed to resolve '{}': {}", bindAddress, gai_strerror(lookupResult));
			return false;
		}

		for (addrinfo* address = addresses; address && !IsListening(); address = address->ai_next)
		{
			const int descriptor = CreateSocket(address->ai_family, address->ai_socktype, address->ai_protocol, &m_LastError);
			if (descriptor < 0)
				continue;

			// Allow restarting a server on a fixed port while old connections linger in TIME_WAIT.
			const int reuse = 1;
			setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

			if (bind(descriptor, address->ai_addr, address->ai_addrlen) != 0)
			{
				m_LastError = "bind() failed: " + GetErrorMessage(errno);
				close(descriptor);
				continue;
			}
			if (listen(descriptor, SOMAXCONN) != 0)
			{
				m_LastError = "listen() failed: " + GetErrorMessage(errno);
				close(descriptor);
				continue;
			}

			sockaddr_storage boundAddress = {};
			socklen_t boundLength = sizeof(boundAddress);
			if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&boundAddress), &boundLength) != 0)
			{
				m_LastError = "getsockname() failed: " + GetErrorMessage(errno);
				close(descriptor);
				continue;
			}

			if (boundAddress.ss_family == AF_INET6)
				m_Port = ntohs(reinterpret_cast<const sockaddr_in6*>(&boundAddress)->sin6_port);
			else
				m_Port = ntohs(reinterpret_cast<const sockaddr_in*>(&boundAddress)->sin_port);
			m_Handle = FromNative(descriptor);
		}

		freeaddrinfo(addresses);

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

#if defined(ST_PLATFORM_LINUX)
			const int descriptor = accept4(ToNative(m_Handle), nullptr, nullptr, SOCK_CLOEXEC);
#else
			const int descriptor = accept(ToNative(m_Handle), nullptr, nullptr);
#endif
			if (descriptor >= 0)
			{
				// Accepted sockets do not inherit O_NONBLOCK (Linux) or SO_NOSIGPIPE reliably; configure explicitly.
				if (ConfigureSocket(descriptor))
					return TcpSocket(FromNative(descriptor));
				close(descriptor);
				return std::nullopt;
			}

			// The pending connection may have been reset before we got to it; keep waiting for another one.
			const int acceptError = errno;
			const bool transient = acceptError == EINTR || acceptError == ECONNABORTED || acceptError == EPROTO || IsWouldBlock(acceptError);
			if (!transient)
			{
				m_LastError = "accept() failed: " + GetErrorMessage(acceptError);
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
		close(ToNative(m_Handle));
		m_Handle = c_InvalidSocketHandle;
		m_Port = 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// SocketPoller
	////////////////////////////////////////////////////////////////////////////////

	bool SocketPoller::Poll(std::span<SocketPollEntry> entries, std::chrono::milliseconds timeout)
	{
		std::vector<pollfd> descriptors;
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

			pollfd descriptor = {};
			descriptor.fd = ToNative(entry.Handle);
			descriptor.events = static_cast<short>((entry.WantRead ? POLLIN : 0) | (entry.WantWrite ? POLLOUT : 0));
			descriptors.push_back(descriptor);
			entryIndices.push_back(index);
		}

		const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
		int ready = 0;
		while (true)
		{
			// poll() with no descriptors simply sleeps for the timeout, which is the behavior we want.
			ready = poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), ToPollTimeout(Remaining(deadline)));
			if (ready >= 0)
				break;
			if (errno != EINTR)
				return false;
		}

		for (size_t index = 0; index < descriptors.size(); index++)
		{
			const short events = descriptors[index].revents;
			SocketPollEntry& entry = entries[entryIndices[index]];
			const bool failed = (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;
			entry.Readable = entry.WantRead && ((events & POLLIN) != 0 || failed);
			entry.Writable = entry.WantWrite && ((events & POLLOUT) != 0 || failed);
		}
		return true;
	}

}
