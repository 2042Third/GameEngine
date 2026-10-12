#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <cstdint>

// How the physics module maps collision layers to Jolt's object layers (internal to the Physics module, which alone includes
// Jolt). Object layers encode the user layer (0-31, see c_PhysicsLayerCount) and whether the body can move:
// objectLayer = userLayer * 2 + moving. Static bodies never collide with each other, so the broad phase keeps them in a tree
// of their own that only moving bodies are tested against.
namespace Strata::PhysicsLayers
{

	inline JPH::ObjectLayer MakeObjectLayer(uint32_t userLayer, bool moving)
	{
		return static_cast<JPH::ObjectLayer>(userLayer * 2u + (moving ? 1u : 0u));
	}

	inline uint32_t GetUserLayer(JPH::ObjectLayer layer)
	{
		return static_cast<uint32_t>(layer) >> 1u;
	}

	inline bool IsMovingLayer(JPH::ObjectLayer layer)
	{
		return (static_cast<uint32_t>(layer) & 1u) != 0;
	}

}
