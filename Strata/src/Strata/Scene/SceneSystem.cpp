#include "stpch.h"
#include "Strata/Scene/SceneSystem.h"

#include "Strata/Scene/Scene.h"

#include <queue>

namespace Strata
{

	namespace
	{

		struct SystemRegistryData
		{
			bool Open = false;
			std::vector<SceneSystemDescriptor> Registered; // Registration order
			std::vector<SceneSystemDescriptor> Ordered;    // Update order
		};

		SystemRegistryData& GetDataUnchecked()
		{
			static SystemRegistryData s_Data;
			return s_Data;
		}

		SystemRegistryData& GetData()
		{
			SystemRegistryData& data = GetDataUnchecked();
			ST_CORE_VERIFY(data.Open, "The scene system registry is used before Engine::RegisterBuiltinModules() registered the engine's modules");
			return data;
		}

		// Names a cycle among the systems the ordering could not place: each of them still has a successor among them, so
		// walking successors from any of them must come back to a system it passed. Successors run later, so the walk reads
		// in update order.
		std::string DescribeCycle(const std::vector<SceneSystemDescriptor>& registered, const std::vector<std::vector<size_t>>& successors,
			const std::vector<uint32_t>& remainingSuccessors)
		{
			size_t current = 0;
			while (remainingSuccessors[current] == 0)
				current++;

			std::vector<size_t> path;
			std::vector<size_t> positionInPath(registered.size(), SIZE_MAX);
			while (positionInPath[current] == SIZE_MAX)
			{
				positionInPath[current] = path.size();
				path.push_back(current);
				for (size_t successor : successors[current])
				{
					if (remainingSuccessors[successor] > 0)
					{
						current = successor;
						break;
					}
				}
			}

			// The cycle is the path's tail from the repeated system.
			std::string cycle;
			for (size_t index = positionInPath[current]; index < path.size(); index++)
				cycle += registered[path[index]].Name + " -> ";
			return cycle + registered[current].Name;
		}

		// The update order of `registered`: a topological order of the After/Before constraints that follows registration
		// order as far as they allow. The first registered system runs as early as the constraints allow, then the second,
		// and so on: a system moves ahead of an earlier registered one only when it has to run before a system that runs
		// ahead of that one. Returns false with the reason for constraints that name an unknown system or the system
		// itself, and for cycles.
		//
		// The order is built from the back: each step puts the latest registered system whose successors are all placed in
		// front of the placed ones. (Taking the earliest registered ready system from the front instead lets an
		// unconstrained system overtake earlier registered ones that wait for a constraint: with Physics waiting for a
		// later "Wind" that runs before it, an unconstrained "CameraFollow" registered between them would run before
		// Physics.)
		bool ComputeUpdateOrder(const std::vector<SceneSystemDescriptor>& registered, std::vector<SceneSystemDescriptor>& outOrdered, std::string& outError)
		{
			std::unordered_map<std::string, size_t> indices;
			for (size_t index = 0; index < registered.size(); index++)
				indices.emplace(registered[index].Name, index);

			// An edge runs from the system that updates first to the one that updates after it.
			std::vector<std::vector<size_t>> successors(registered.size());
			std::vector<std::vector<size_t>> predecessors(registered.size());
			// Adds the constraints of system `index` against the systems in `names`; false (with the reason) for a bad name.
			auto addConstraints = [&](size_t index, const std::vector<std::string>& names, bool after)
			{
				const std::string& system = registered[index].Name;
				for (const std::string& name : names)
				{
					auto other = indices.find(name);
					if (other == indices.end())
					{
						outError = fmt::format("scene system '{}' is to run {} '{}', which is not registered", system, after ? "after" : "before", name);
						return false;
					}
					if (other->second == index)
					{
						outError = fmt::format("scene system '{}' names itself in its update order", system);
						return false;
					}
					const size_t first = after ? other->second : index;
					const size_t second = after ? index : other->second;
					successors[first].push_back(second);
					predecessors[second].push_back(first);
				}
				return true;
			};
			for (size_t index = 0; index < registered.size(); index++)
			{
				if (!addConstraints(index, registered[index].After, true) || !addConstraints(index, registered[index].Before, false))
					return false;
			}

			// Systems whose successors are all placed, the latest registered on top.
			std::vector<uint32_t> remainingSuccessors(registered.size());
			std::priority_queue<size_t> ready;
			for (size_t index = 0; index < registered.size(); index++)
			{
				remainingSuccessors[index] = static_cast<uint32_t>(successors[index].size());
				if (remainingSuccessors[index] == 0)
					ready.push(index);
			}

			std::vector<size_t> placedFromBack;
			placedFromBack.reserve(registered.size());
			while (!ready.empty())
			{
				const size_t index = ready.top();
				ready.pop();
				placedFromBack.push_back(index);
				for (size_t predecessor : predecessors[index])
				{
					if (--remainingSuccessors[predecessor] == 0)
						ready.push(predecessor);
				}
			}

			if (placedFromBack.size() != registered.size())
			{
				outError = fmt::format("the update order of the scene systems has a cycle: {}", DescribeCycle(registered, successors, remainingSuccessors));
				return false;
			}
			std::vector<SceneSystemDescriptor> ordered;
			ordered.reserve(registered.size());
			for (auto it = placedFromBack.rbegin(); it != placedFromBack.rend(); ++it)
				ordered.push_back(registered[*it]);
			outOrdered = std::move(ordered);
			return true;
		}

	}

	void SceneSystemRegistry::BeginRegistration()
	{
		GetDataUnchecked().Open = true;
	}

	bool SceneSystemRegistry::Register(SceneSystemDescriptor descriptor)
	{
		SystemRegistryData& data = GetData();
		if (descriptor.Name.empty() || !descriptor.Create)
		{
			ST_CORE_ERROR("Scene system '{}' is not registered: a system needs a name and a factory", descriptor.Name);
			return false;
		}
		if (const uint32_t running = Scene::GetRunningSceneCount(); running > 0)
		{
			ST_CORE_ERROR("Scene system '{}' is not registered: {} scene(s) are running, whose systems were created from the registry", descriptor.Name, running);
			return false;
		}
		for (const SceneSystemDescriptor& existing : data.Registered)
		{
			if (descriptor.Type != 0 && existing.Type == descriptor.Type && existing.Name != descriptor.Name)
			{
				ST_CORE_ERROR("Scene system '{}' is not registered: its class is registered already, as scene system '{}'", descriptor.Name, existing.Name);
				return false;
			}
		}

		std::vector<SceneSystemDescriptor> registered;
		registered.reserve(data.Registered.size() + 1);
		for (const SceneSystemDescriptor& existing : data.Registered)
		{
			if (existing.Name != descriptor.Name)
				registered.push_back(existing);
		}
		const std::string name = descriptor.Name;
		registered.push_back(std::move(descriptor));

		std::vector<SceneSystemDescriptor> ordered;
		std::string error;
		if (!ComputeUpdateOrder(registered, ordered, error))
		{
			ST_CORE_ERROR("Scene system '{}' is not registered: {}", name, error);
			return false;
		}
		data.Registered = std::move(registered);
		data.Ordered = std::move(ordered);
		return true;
	}

	bool SceneSystemRegistry::Unregister(const std::string& name)
	{
		SystemRegistryData& data = GetData();
		auto it = std::find_if(data.Registered.begin(), data.Registered.end(), [&](const SceneSystemDescriptor& descriptor) { return descriptor.Name == name; });
		if (it == data.Registered.end())
			return false;
		if (const uint32_t running = Scene::GetRunningSceneCount(); running > 0)
		{
			ST_CORE_ERROR("Scene system '{}' is not unregistered: {} scene(s) are running, whose systems were created from the registry", name, running);
			return false;
		}

		std::string dependents;
		for (const SceneSystemDescriptor& descriptor : data.Registered)
		{
			const bool names = std::find(descriptor.After.begin(), descriptor.After.end(), name) != descriptor.After.end()
				|| std::find(descriptor.Before.begin(), descriptor.Before.end(), name) != descriptor.Before.end();
			if (names)
				dependents += fmt::format("{}'{}'", dependents.empty() ? "" : ", ", descriptor.Name);
		}
		if (!dependents.empty())
		{
			ST_CORE_ERROR("Scene system '{}' is not unregistered: the update order of {} names it", name, dependents);
			return false;
		}

		data.Registered.erase(it);
		// Removing a system no other system names cannot invalidate the order of the rest.
		std::string error;
		const bool ordered = ComputeUpdateOrder(data.Registered, data.Ordered, error);
		ST_CORE_VERIFY(ordered, "The scene systems cannot be ordered after unregistering '{}': {}", name, error);
		return true;
	}

	const std::vector<SceneSystemDescriptor>& SceneSystemRegistry::GetAll()
	{
		return GetData().Ordered;
	}

}
