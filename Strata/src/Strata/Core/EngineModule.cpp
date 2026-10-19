#include "stpch.h"
#include "Strata/Core/EngineModule.h"

#include <algorithm>

namespace Strata
{

	namespace
	{

		// "'A', 'B' and 'C'"
		std::string QuoteNames(const std::vector<std::string_view>& names)
		{
			std::string text;
			for (size_t index = 0; index < names.size(); index++)
			{
				if (index > 0)
					text += index + 1 == names.size() ? " and " : ", ";
				text += fmt::format("'{}'", names[index]);
			}
			return text;
		}

	}

	EngineModuleRegistry::~EngineModuleRegistry()
	{
		Clear();
	}

	void EngineModuleRegistry::Clear()
	{
		m_Modules.clear();
		while (!m_Owned.empty())
			m_Owned.pop_back();
	}

	EngineModule* EngineModuleRegistry::Find(std::string_view name) const
	{
		for (EngineModule* module : m_Modules)
		{
			if (module->GetName() == name)
				return module;
		}
		return nullptr;
	}

	bool EngineModuleRegistry::Create(std::span<const EngineModuleFactory> factories, std::string* outError)
	{
		ST_CORE_ASSERT(m_Modules.empty(), "EngineModuleRegistry::Create is called twice");
		std::vector<Scope<EngineModule>> modules;
		modules.reserve(factories.size());
		std::vector<std::string> problems;
		for (size_t index = 0; index < factories.size(); index++)
		{
			Scope<EngineModule> module = factories[index] ? factories[index]() : nullptr;
			if (!module)
			{
				problems.push_back(fmt::format("the engine module factory at position {} of the program's list makes no module", index + 1));
				continue;
			}
			modules.push_back(std::move(module));
		}

		const auto findModule = [&](std::string_view name) -> size_t
		{
			for (size_t index = 0; index < modules.size(); index++)
			{
				if (modules[index]->GetName() == name)
					return index;
			}
			return modules.size();
		};

		std::vector<std::string_view> duplicates;
		for (size_t index = 0; index < modules.size(); index++)
		{
			const std::string_view name = modules[index]->GetName();
			if (findModule(name) != index && std::find(duplicates.begin(), duplicates.end(), name) == duplicates.end())
			{
				duplicates.push_back(name);
				problems.push_back(fmt::format("two engine modules are named '{}'", name));
			}
			for (const EngineModuleDependency& dependency : modules[index]->GetDependencies())
			{
				if (dependency.Required && findModule(dependency.Name) == modules.size())
					problems.push_back(fmt::format("engine module '{}' requires '{}', which the program does not run", name, dependency.Name));
			}
		}

		std::vector<size_t> order;
		if (problems.empty())
		{
			// Kahn's algorithm, always taking the first ready module in the program's listing order: dependencies first,
			// the listing order wherever they leave a choice.
			const size_t count = modules.size();
			std::vector<std::vector<size_t>> dependents(count);
			std::vector<size_t> pending(count, 0);
			for (size_t index = 0; index < count; index++)
			{
				for (const EngineModuleDependency& dependency : modules[index]->GetDependencies())
				{
					const size_t target = findModule(dependency.Name);
					if (target == count)
						continue; // An optional module the program does not run
					dependents[target].push_back(index);
					pending[index]++;
				}
			}
			std::vector<bool> placed(count, false);
			order.reserve(count);
			while (order.size() < count)
			{
				size_t next = count;
				for (size_t index = 0; index < count; index++)
				{
					if (!placed[index] && pending[index] == 0)
					{
						next = index;
						break;
					}
				}
				if (next == count)
					break; // The rest wait for each other
				placed[next] = true;
				order.push_back(next);
				for (const size_t dependent : dependents[next])
					pending[dependent]--;
			}
			if (order.size() < count)
			{
				// What is left waits for a cycle. Modules that only wait for it (no module left waits for them) are not part
				// of it: leave them out of the message, until only the cycles remain.
				std::vector<bool> inCycle(count, false);
				for (size_t index = 0; index < count; index++)
					inCycle[index] = !placed[index];
				for (bool removed = true; removed;)
				{
					removed = false;
					for (size_t index = 0; index < count; index++)
					{
						if (!inCycle[index])
							continue;
						const bool awaited = std::any_of(dependents[index].begin(), dependents[index].end(), [&](size_t dependent) { return inCycle[dependent]; });
						if (!awaited)
						{
							inCycle[index] = false;
							removed = true;
						}
					}
				}
				std::vector<std::string_view> cycle;
				for (size_t index = 0; index < count; index++)
				{
					if (inCycle[index])
						cycle.push_back(modules[index]->GetName());
				}
				problems.push_back(fmt::format("engine modules {} depend on each other in a cycle", QuoteNames(cycle)));
			}
		}

		if (!problems.empty())
		{
			if (outError)
			{
				outError->clear();
				for (const std::string& problem : problems)
				{
					if (!outError->empty())
						*outError += "; ";
					*outError += problem;
				}
			}
			return false;
		}

		m_Owned.reserve(order.size());
		m_Modules.reserve(order.size());
		for (const size_t index : order)
		{
			m_Modules.push_back(modules[index].get());
			m_Owned.push_back(std::move(modules[index]));
		}
		return true;
	}

}
