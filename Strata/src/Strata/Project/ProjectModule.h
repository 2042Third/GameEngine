#pragma once

#include "Strata/Core/EngineModule.h"

#include <string_view>

namespace Strata
{

	// The engine module of the active project and asset manager (Project::SetActive, AssetManager::SetActive), which the
	// program's layers open: it lets them go once the layers are gone, while the job system (their manager waits for its
	// load jobs) and the graphics device (assets own GPU resources) still exist.
	class ProjectModule final : public EngineModule
	{
	public:
		static constexpr std::string_view c_Name = "Project";

		static EngineModuleFactory Factory();

		std::string_view GetName() const override { return c_Name; }
		[[nodiscard]] bool OnInit(EngineContext& context) override;
		void OnRelease() override;
	};

}
