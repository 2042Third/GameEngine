#pragma once

// Script classes that other feature scripts use directly (GetScript<T>, AddScript<T>). Each is registered with
// ST_SCRIPT_CLASS in exactly one .cpp file.

#include "FeatureScript.h"

#include <cstdint>
#include <string>

namespace FeatureTest
{

	// On the "Toggled" entity: counts updates while EntityFeatures deactivates and reactivates the entity.
	class ActivationProbe : public Strata::Script
	{
	public:
		int32_t Updates = 0;

		void OnCreate() override;
		void OnUpdate(float deltaTime) override;
	};

	// On the root of the Crate prefab: remembers the Value it had when OnCreate ran (spawners configure it before).
	class CrateScript : public FeatureScript
	{
	public:
		int32_t Value = 0;
		int32_t ValueSeenInCreate = -1;

		void OnCreate() override;
	};

	// Receives messages from other scripts (Messenger).
	class Receiver : public Strata::Script
	{
	public:
		int32_t Received = 0;
		std::string LastMessage;

		void OnCreate() override;
		void Receive(const std::string& message);
	};

	// Throws from its first update: the engine disables the instance until the module is reloaded.
	class ExceptionProbe : public FeatureScript
	{
	public:
		bool Thrown = false;
		bool Reloaded = false;
		int32_t UpdatesAfterReload = 0;

		void OnCreate() override;
		void OnUpdate(float deltaTime) override;
		void OnReload() override;
	};

}
