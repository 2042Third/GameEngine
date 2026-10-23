#include "stpch.h"
#include "Strata/Input/InputRegistration.h"

#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Input/InputActionSet.h"

namespace Strata
{

	void RegisterInputModule()
	{
		SettingsRegistry::GetProcessRegistry().Register(InputActionSet::MakeSection());
	}

}
