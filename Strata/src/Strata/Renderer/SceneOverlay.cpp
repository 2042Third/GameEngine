#include "stpch.h"
#include "Strata/Renderer/SceneOverlay.h"

#include <algorithm>

namespace Strata
{

	namespace
	{

		std::vector<SceneOverlayRegistry::Entry>& GetEntries()
		{
			static std::vector<SceneOverlayRegistry::Entry> s_Entries;
			return s_Entries;
		}

	}

	bool SceneOverlayRegistry::Register(std::string name, SceneOverlayFactory factory)
	{
		if (name.empty() || !factory)
			return false;
		std::vector<Entry>& entries = GetEntries();
		auto existing = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.Name == name; });
		if (existing != entries.end())
			existing->Factory = std::move(factory);
		else
			entries.push_back(Entry { std::move(name), std::move(factory) });
		return true;
	}

	const std::vector<SceneOverlayRegistry::Entry>& SceneOverlayRegistry::GetAll()
	{
		return GetEntries();
	}

}
