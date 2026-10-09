#include "stpch.h"
#include "Strata/Scripting/ScriptTypes.h"

namespace Strata
{

	const char* ScriptCallbackToString(ScriptCallback callback)
	{
		switch (callback)
		{
			case ScriptCallback::OnCreate:         return "OnCreate";
			case ScriptCallback::OnUpdate:         return "OnUpdate";
			case ScriptCallback::OnFixedUpdate:    return "OnFixedUpdate";
			case ScriptCallback::OnLateUpdate:     return "OnLateUpdate";
			case ScriptCallback::OnDestroy:        return "OnDestroy";
			case ScriptCallback::OnReload:         return "OnReload";
			case ScriptCallback::OnCollisionEnter: return "OnCollisionEnter";
			case ScriptCallback::OnCollisionExit:  return "OnCollisionExit";
			case ScriptCallback::OnTriggerEnter:   return "OnTriggerEnter";
			case ScriptCallback::OnTriggerExit:    return "OnTriggerExit";
		}
		return "Unknown";
	}

	const ScriptFieldInfo* ScriptClassInfo::FindField(std::string_view name) const
	{
		const uint32_t index = FindFieldIndex(name);
		return index != UINT32_MAX ? &Fields[index] : nullptr;
	}

	uint32_t ScriptClassInfo::FindFieldIndex(std::string_view name) const
	{
		for (size_t index = 0; index < Fields.size(); index++)
		{
			if (Fields[index].Name == name)
				return static_cast<uint32_t>(index);
		}
		return UINT32_MAX;
	}

}
