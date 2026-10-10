#include "stpch.h"
#include "Strata/Scene/SceneSystem.h"

namespace Strata
{

	namespace
	{

		struct SystemRegistryData
		{
			bool Open = false;
			std::vector<SceneSystemDescriptor> Descriptors;
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

	}

	void SceneSystemRegistry::BeginRegistration()
	{
		GetDataUnchecked().Open = true;
	}

	void SceneSystemRegistry::Register(SceneSystemDescriptor descriptor)
	{
		Unregister(descriptor.Name);
		GetData().Descriptors.push_back(std::move(descriptor));
	}

	void SceneSystemRegistry::Unregister(const std::string& name)
	{
		std::vector<SceneSystemDescriptor>& descriptors = GetData().Descriptors;
		descriptors.erase(std::remove_if(descriptors.begin(), descriptors.end(), [&](const SceneSystemDescriptor& descriptor) { return descriptor.Name == name; }), descriptors.end());
	}

	const std::vector<SceneSystemDescriptor>& SceneSystemRegistry::GetAll()
	{
		return GetData().Descriptors;
	}

}
