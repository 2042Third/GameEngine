#include "stpch.h"
#include "Strata/Network/Socket.h"

// Platform-independent parts of the socket layer. The system calls (and SocketNotifier::Open) live in
// Platform/Windows/WindowsSocket.cpp and Platform/Posix/PosixSocket.cpp.

namespace Strata
{

	namespace
	{

		// Longest single wait while sending without a deadline, so a stalled peer is re-checked periodically.
		constexpr std::chrono::milliseconds c_UnboundedSendWaitSlice = std::chrono::milliseconds(1000);

	}

	////////////////////////////////////////////////////////////////////////////////
	// TcpSocket
	////////////////////////////////////////////////////////////////////////////////

	TcpSocket::~TcpSocket()
	{
		Close();
	}

	TcpSocket::TcpSocket(TcpSocket&& other) noexcept
		: m_Handle(std::exchange(other.m_Handle, c_InvalidSocketHandle))
	{
	}

	TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
	{
		if (this != &other)
		{
			Close();
			m_Handle = std::exchange(other.m_Handle, c_InvalidSocketHandle);
		}
		return *this;
	}

	bool TcpSocket::SendAll(std::span<const uint8_t> data, std::optional<std::chrono::milliseconds> timeout)
	{
		if (!IsValid())
			return false;

		const std::optional<std::chrono::milliseconds> limit = timeout ? std::optional(ClampSocketTimeout(*timeout)) : std::nullopt;
		const auto start = std::chrono::steady_clock::now();
		size_t offset = 0;
		while (offset < data.size())
		{
			const std::optional<size_t> sent = SendSome(data.subspan(offset));
			if (!sent)
				return false;

			offset += *sent;
			if (offset >= data.size())
				break;
			if (*sent > 0)
				continue;

			// The send buffer is full: wait until the peer drains it.
			std::chrono::milliseconds wait = c_UnboundedSendWaitSlice;
			if (limit)
			{
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
				if (elapsed >= *limit)
					return false;
				wait = std::min(wait, *limit - elapsed);
			}

			SocketPollEntry entry;
			entry.Handle = m_Handle;
			entry.WantWrite = true;
			if (!SocketPoller::Poll(std::span<SocketPollEntry>(&entry, 1), wait))
				return false;
		}
		return true;
	}

	bool TcpSocket::SendAll(std::string_view data, std::optional<std::chrono::milliseconds> timeout)
	{
		return SendAll(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()), timeout);
	}

	////////////////////////////////////////////////////////////////////////////////
	// TcpListener
	////////////////////////////////////////////////////////////////////////////////

	TcpListener::~TcpListener()
	{
		Close();
	}

	TcpListener::TcpListener(TcpListener&& other) noexcept
		: m_Handle(std::exchange(other.m_Handle, c_InvalidSocketHandle)), m_Port(std::exchange(other.m_Port, uint16_t(0))), m_LastError(std::move(other.m_LastError))
	{
	}

	TcpListener& TcpListener::operator=(TcpListener&& other) noexcept
	{
		if (this != &other)
		{
			Close();
			m_Handle = std::exchange(other.m_Handle, c_InvalidSocketHandle);
			m_Port = std::exchange(other.m_Port, uint16_t(0));
			m_LastError = std::move(other.m_LastError);
		}
		return *this;
	}

	////////////////////////////////////////////////////////////////////////////////
	// SocketNotifier
	////////////////////////////////////////////////////////////////////////////////

	void SocketNotifier::Close()
	{
		std::scoped_lock<std::mutex> lock(m_SenderMutex);
		m_Sender.Close();
		m_Receiver.Close();
	}

	void SocketNotifier::Notify()
	{
		std::scoped_lock<std::mutex> lock(m_SenderMutex);
		if (!m_Sender.IsValid())
			return;

		// A full send buffer means plenty of wake-ups are already pending, so a short write is harmless.
		const uint8_t signal = 1;
		m_Sender.SendSome(std::span<const uint8_t>(&signal, 1));
	}

	void SocketNotifier::Drain()
	{
		std::vector<uint8_t> discarded;
		while (m_Receiver.IsValid())
		{
			const SocketReceiveStatus status = m_Receiver.Receive(discarded, std::chrono::milliseconds(0));
			if (status == SocketReceiveStatus::Data)
			{
				discarded.clear();
				continue;
			}

			if (status != SocketReceiveStatus::Timeout)
			{
				// The pair broke. Close it, since a closed receiver would otherwise poll as readable forever.
				ST_CORE_WARN("SocketNotifier: the socket pair was closed unexpectedly");
				Close();
			}
			return;
		}
	}

}
