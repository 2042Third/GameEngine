#pragma once

#include <cstddef>
#include <optional>
#include <span>

namespace Strata
{

	// What RpcServer knows about a connection that has not authenticated yet, when choosing one to evict.
	struct PendingConnectionState
	{
		bool Polled = false;           // It has been through a poll since it was accepted, so it had a chance to send
		bool HandshakeStarted = false; // It completed the first step of the handshake
	};

	// RpcServer's choice when a new connection arrives while every pending slot is taken. pending is ordered oldest
	// first. Returns the index of the connection to evict: the oldest one that had a chance to send, preferring one
	// that has not started the handshake. nullopt when none had a chance to send (they all arrived in the current
	// burst): the new connection is then turned away instead.
	std::optional<size_t> ChoosePendingConnectionToEvict(std::span<const PendingConnectionState> pending);

}
