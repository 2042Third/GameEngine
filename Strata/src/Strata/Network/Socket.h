#pragma once

#include "Strata/Core/Base.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// Native socket handle (a SOCKET on Windows, a file descriptor elsewhere) widened to a pointer-sized
	// integer so this header stays free of platform headers.
	using SocketHandle = intptr_t;
	constexpr SocketHandle c_InvalidSocketHandle = -1;

	// Longest wait any socket operation accepts. Longer (or negative) timeouts are clamped, which also keeps
	// deadline arithmetic (now + timeout) from overflowing.
	constexpr std::chrono::milliseconds c_MaxSocketTimeout = std::chrono::hours(24);

	inline std::chrono::milliseconds ClampSocketTimeout(std::chrono::milliseconds timeout)
	{
		return std::clamp(timeout, std::chrono::milliseconds(0), c_MaxSocketTimeout);
	}

	// Whether address is a numeric loopback address: IPv4 127.0.0.0/8 (dotted decimal) or IPv6 ::1. Host names
	// (including "localhost") are not accepted, since they may resolve elsewhere.
	bool IsLoopbackAddress(std::string_view address);

	enum class SocketReceiveStatus : uint8_t
	{
		Data,    // At least one byte was appended
		Timeout, // Nothing arrived before the timeout elapsed
		Closed,  // The peer closed the connection (orderly shutdown)
		Error    // The connection failed (reset, aborted) or the socket is invalid
	};

	// Connected TCP stream socket (move-only; the destructor closes it).
	//
	// Sockets are kept in non-blocking mode internally; every blocking operation is implemented by waiting
	// for readiness with a timeout (clamped to c_MaxSocketTimeout), so no call can hang forever unless explicitly
	// asked to. Writing to a connection the peer has closed never raises SIGPIPE. A socket may be used by one
	// thread at a time.
	class TcpSocket
	{
	public:
		TcpSocket() = default;
		~TcpSocket();

		TcpSocket(TcpSocket&& other) noexcept;
		TcpSocket& operator=(TcpSocket&& other) noexcept;
		TcpSocket(const TcpSocket&) = delete;
		TcpSocket& operator=(const TcpSocket&) = delete;

		// Resolves host (a name or numeric IPv4/IPv6 address) and connects to the first address that accepts
		// within timeout (shared by all addresses). On failure, error (if given) receives a description.
		static std::optional<TcpSocket> Connect(std::string_view host, uint16_t port, std::chrono::milliseconds timeout, std::string* error = nullptr);

		// Sends every byte, waiting for buffer space as needed. Returns false if the connection failed or the
		// timeout (no limit when nullopt) elapsed first; the amount of data sent is then unspecified.
		bool SendAll(std::span<const uint8_t> data, std::optional<std::chrono::milliseconds> timeout = std::nullopt);
		bool SendAll(std::string_view data, std::optional<std::chrono::milliseconds> timeout = std::nullopt);

		// Sends as much of data as the socket accepts without blocking. Returns the number of bytes sent
		// (0 when the send buffer is full), or nullopt if the connection failed.
		std::optional<size_t> SendSome(std::span<const uint8_t> data);

		// Waits up to timeout for incoming data and appends whatever is available (one read) to appendTo.
		SocketReceiveStatus Receive(std::vector<uint8_t>& appendTo, std::chrono::milliseconds timeout);

		// Half-closes the connection: the peer sees the end of the stream, while this side can still receive.
		bool ShutdownSend();
		void Close();
		bool IsValid() const { return m_Handle != c_InvalidSocketHandle; }
		// Disables (true) or enables (false) Nagle's algorithm. Request/response protocols want it disabled.
		bool SetNoDelay(bool enabled);

		// The local port this socket is bound to (0 if unknown).
		uint16_t GetLocalPort() const;
		SocketHandle GetHandle() const { return m_Handle; }
	private:
		explicit TcpSocket(SocketHandle handle)
			: m_Handle(handle)
		{
		}
	private:
		SocketHandle m_Handle = c_InvalidSocketHandle;

		friend class TcpListener;
		friend class SocketNotifier;
	};

	// Listening TCP socket (move-only; the destructor closes it).
	class TcpListener
	{
	public:
		TcpListener() = default;
		~TcpListener();

		TcpListener(TcpListener&& other) noexcept;
		TcpListener& operator=(TcpListener&& other) noexcept;
		TcpListener(const TcpListener&) = delete;
		TcpListener& operator=(const TcpListener&) = delete;

		// Binds and starts listening. Port 0 picks a free ephemeral port (see GetPort). The default bind
		// address only accepts connections from this machine. On failure, see GetLastError.
		bool Listen(std::string_view bindAddress = "127.0.0.1", uint16_t port = 0);
		uint16_t GetPort() const { return m_Port; }

		// Waits up to timeout for an incoming connection.
		std::optional<TcpSocket> Accept(std::chrono::milliseconds timeout);

		void Close();
		bool IsListening() const { return m_Handle != c_InvalidSocketHandle; }
		const std::string& GetLastError() const { return m_LastError; }

		SocketHandle GetHandle() const { return m_Handle; }
	private:
		SocketHandle m_Handle = c_InvalidSocketHandle;
		uint16_t m_Port = 0;
		std::string m_LastError;
	};

	// One socket of a SocketPoller::Poll call.
	struct SocketPollEntry
	{
		SocketHandle Handle = c_InvalidSocketHandle; // Invalid entries are ignored
		bool WantRead = false;
		bool WantWrite = false;

		// Results. Readable is also set when the connection failed or was closed, so the next read reports it.
		bool Readable = false;
		bool Writable = false;
	};

	class SocketPoller
	{
	public:
		// Waits until at least one entry is ready or the timeout elapses (which leaves every result false).
		// Returns false if waiting failed.
		static bool Poll(std::span<SocketPollEntry> entries, std::chrono::milliseconds timeout);
	};

	// Wakes a thread blocked in SocketPoller::Poll from any other thread: include GetHandle() in the poll set
	// (WantRead), call Drain() once it becomes readable, and Notify() from other threads. Built on a socket pair:
	// socketpair() on POSIX, a verified loopback TCP connection on Windows (which can only poll sockets, not pipes
	// or events). If the pair breaks, Drain closes it and IsValid() turns false; callers then fall back to polling
	// with a short timeout. Open, Close, Drain and GetHandle belong to the polling thread.
	class SocketNotifier
	{
	public:
		SocketNotifier() = default;
		~SocketNotifier() = default;

		SocketNotifier(const SocketNotifier&) = delete;
		SocketNotifier& operator=(const SocketNotifier&) = delete;

		bool Open();
		void Close();
		bool IsValid() const { return m_Receiver.IsValid(); }

		void Notify(); // Thread-safe
		void Drain();  // Polling thread only

		SocketHandle GetHandle() const { return m_Receiver.GetHandle(); }
	private:
		std::mutex m_SenderMutex;
		TcpSocket m_Sender;
		TcpSocket m_Receiver;
	};

}
