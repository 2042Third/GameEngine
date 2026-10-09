#include "stpch.h"
#include "Strata/Renderer/ShaderLibrary.h"

#include "Strata/Renderer/EmbeddedShaders.h"

namespace Strata
{

	namespace EmbeddedShaders
	{

		const EmbeddedShader* Find(std::string_view name)
		{
			for (const EmbeddedShader& shader : GetAll())
			{
				if (shader.Name == name)
					return &shader;
			}
			return nullptr;
		}

	}

	ShaderLibrary::ShaderLibrary(nvrhi::IDevice* device)
		: m_Device(device)
	{
	}

	nvrhi::ShaderType ShaderLibrary::GetStageFromName(std::string_view name)
	{
		auto endsWith = [name](std::string_view suffix)
		{
			return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
		};

		if (endsWith(".vert"))
			return nvrhi::ShaderType::Vertex;
		if (endsWith(".frag"))
			return nvrhi::ShaderType::Pixel;
		if (endsWith(".comp"))
			return nvrhi::ShaderType::Compute;
		if (endsWith(".geom"))
			return nvrhi::ShaderType::Geometry;
		return nvrhi::ShaderType::None;
	}

	nvrhi::ShaderHandle ShaderLibrary::Get(std::string_view name)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		const std::string key(name);
		auto it = m_Shaders.find(key);
		if (it != m_Shaders.end())
			return it->second;

		const EmbeddedShader* embedded = EmbeddedShaders::Find(name);
		if (!embedded)
		{
			ST_CORE_ERROR("Shader '{}' is not part of the engine's embedded shaders", name);
			return nullptr;
		}

		const nvrhi::ShaderType stage = GetStageFromName(name);
		if (stage == nvrhi::ShaderType::None)
		{
			ST_CORE_ERROR("Cannot determine the stage of shader '{}'", name);
			return nullptr;
		}

		nvrhi::ShaderDesc desc;
		desc.shaderType = stage;
		desc.debugName = key;
		desc.entryName = "main";
		nvrhi::ShaderHandle shader = m_Device->createShader(desc, embedded->Code, embedded->Size);
		if (!shader)
		{
			ST_CORE_ERROR("Failed to create shader '{}'", name);
			return nullptr;
		}

		m_Shaders.emplace(key, shader);
		return shader;
	}

	void ShaderLibrary::Clear()
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_Shaders.clear();
	}

}
