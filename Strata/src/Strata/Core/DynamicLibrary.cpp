#include "stpch.h"
#include "Strata/Core/DynamicLibrary.h"

namespace Strata
{

	// Loading, unloading, symbol lookup and file names are implemented per platform (Platform/<OS>/*DynamicLibrary.cpp).

	DynamicLibrary::~DynamicLibrary()
	{
		Unload();
	}

	DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
		: m_Handle(std::exchange(other.m_Handle, nullptr)), m_Path(std::move(other.m_Path)), m_LastError(std::move(other.m_LastError))
	{
	}

	DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
	{
		if (this != &other)
		{
			Unload();
			m_Handle = std::exchange(other.m_Handle, nullptr);
			m_Path = std::move(other.m_Path);
			m_LastError = std::move(other.m_LastError);
		}
		return *this;
	}

	void DynamicLibrary::Release()
	{
		m_Handle = nullptr;
		m_Path.clear();
	}

}
