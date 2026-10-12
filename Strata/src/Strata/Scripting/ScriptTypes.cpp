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
			case ScriptCallback::OnAnimationEvent: return "OnAnimationEvent";
		}
		return "Unknown";
	}

	std::string DescribeScriptCodeLocation(const ScriptCodeLocation& location)
	{
		// "File(Line)" first, the form compilers print and the editor opens: IDE consoles and the editor's Console link it.
		std::string text;
		if (!location.File.empty())
			text = location.Line > 0 ? fmt::format("{}({})", location.File, location.Line) : location.File;
		else if (!location.Module.empty())
			text = fmt::format("{}+0x{:X}", location.Module, location.ModuleOffset);
		else
			text = fmt::format("0x{:016X}", location.Address);
		if (!location.Function.empty())
			text += " in " + location.Function;
		return text;
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
