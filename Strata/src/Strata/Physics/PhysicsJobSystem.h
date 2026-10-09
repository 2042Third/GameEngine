#pragma once

#include "Strata/Core/Base.h"

namespace JPH
{
	class JobSystem;
}

namespace Strata
{

	// Creates the job system Jolt runs its simulation jobs on. Jobs are executed by Strata's JobSystem worker pool when
	// it is initialized and inline on the stepping thread otherwise (see PhysicsJobSystem.cpp for the rationale).
	// The returned object is only used by PhysicsWorld; it is declared here so that no engine header includes Jolt.
	Scope<JPH::JobSystem> CreatePhysicsJobSystem();

}
