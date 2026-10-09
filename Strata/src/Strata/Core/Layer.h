#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Events/Event.h"

#include <string>

namespace Strata
{

	// A slice of application behaviour (game, editor UI, debug overlay). Layers update in stack order and
	// receive events in reverse order, so overlays on top can consume input first.
	class Layer
	{
	public:
		Layer(const std::string& name = "Layer");
		virtual ~Layer() = default;

		virtual void OnAttach() {}
		virtual void OnDetach() {}
		virtual void OnUpdate(Timestep) {}
		virtual void OnImGuiRender() {}
		virtual void OnEvent(Event&) {}

		const std::string& GetName() const { return m_DebugName; }
	protected:
		std::string m_DebugName;
	};

}
