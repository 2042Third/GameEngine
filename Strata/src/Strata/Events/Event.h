#pragma once

#include "Strata/Core/Base.h"

#include <spdlog/fmt/fmt.h>

#include <string>

namespace Strata
{

	// Events are dispatched synchronously: when one occurs it is handed to the application immediately,
	// which forwards it through the layer stack (top layer first) until a layer marks it handled.

	enum class EventType
	{
		None = 0,
		WindowClose, WindowResize, WindowFocus, WindowLostFocus, WindowMoved, WindowFileDrop, WindowContentScale,
		KeyPressed, KeyReleased, KeyTyped,
		MouseButtonPressed, MouseButtonReleased, MouseMoved, MouseScrolled
	};

	// Plain enum used as bit flags. Deliberately avoids the identifier `None`, which X11 headers define as a macro.
	enum EventCategory
	{
		EventCategoryNone        = 0,
		EventCategoryApplication = ST_BIT(0),
		EventCategoryInput       = ST_BIT(1),
		EventCategoryKeyboard    = ST_BIT(2),
		EventCategoryMouse       = ST_BIT(3),
		EventCategoryMouseButton = ST_BIT(4)
	};

#define ST_EVENT_CLASS_TYPE(type) \
	static EventType GetStaticType() { return EventType::type; } \
	virtual EventType GetEventType() const override { return GetStaticType(); } \
	virtual const char* GetName() const override { return #type; }

#define ST_EVENT_CLASS_CATEGORY(category) \
	virtual int GetCategoryFlags() const override { return category; }

	class Event
	{
	public:
		virtual ~Event() = default;

		bool Handled = false;

		virtual EventType GetEventType() const = 0;
		virtual const char* GetName() const = 0;
		virtual int GetCategoryFlags() const = 0;
		virtual std::string ToString() const { return GetName(); }

		bool IsInCategory(EventCategory category) const
		{
			return (GetCategoryFlags() & category) != 0;
		}
	};

	class EventDispatcher
	{
	public:
		EventDispatcher(Event& event)
			: m_Event(event)
		{
		}

		// F is deduced by the compiler: bool(T&)
		template<typename T, typename F>
		bool Dispatch(const F& function)
		{
			if (m_Event.GetEventType() == T::GetStaticType())
			{
				m_Event.Handled |= function(static_cast<T&>(m_Event));
				return true;
			}
			return false;
		}
	private:
		Event& m_Event;
	};

}
