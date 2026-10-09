#pragma once

#include <cstdint>

namespace Strata
{

	// Process-wide lifetime of the Jolt physics library (allocator hooks, type factory, logging and assert hooks).
	// Reference counted: the first Acquire initializes Jolt and the last Release shuts it down, so independent users
	// (one PhysicsWorld per running scene, tools) need no coordination. Thread-safe.
	class PhysicsRuntime
	{
	public:
		static void Acquire();
		static void Release();
		static bool IsInitialized();
		static uint32_t GetReferenceCount();
	};

	// Holds a PhysicsRuntime reference for its lifetime.
	class PhysicsRuntimeReference
	{
	public:
		PhysicsRuntimeReference() { PhysicsRuntime::Acquire(); }
		~PhysicsRuntimeReference() { PhysicsRuntime::Release(); }

		PhysicsRuntimeReference(const PhysicsRuntimeReference&) = delete;
		PhysicsRuntimeReference& operator=(const PhysicsRuntimeReference&) = delete;
	};

}
