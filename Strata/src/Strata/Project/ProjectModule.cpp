#include "stpch.h"
#include "Strata/Project/ProjectModule.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Project/Project.h"

namespace Strata
{

	EngineModuleFactory ProjectModule::Factory()
	{
		return []() -> Scope<EngineModule> { return CreateScope<ProjectModule>(); };
	}

	bool ProjectModule::OnInit(EngineContext&)
	{
		return true;
	}

	void ProjectModule::OnRelease()
	{
		AssetManager::SetActive(nullptr);
		Project::SetActive(nullptr);
	}

}
