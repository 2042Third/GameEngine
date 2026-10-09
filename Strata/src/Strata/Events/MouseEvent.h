#pragma once

#include "Strata/Events/Event.h"
#include "Strata/Input/MouseCodes.h"

namespace Strata
{

	class MouseMovedEvent : public Event
	{
	public:
		MouseMovedEvent(float x, float y)
			: m_MouseX(x), m_MouseY(y)
		{
		}

		float GetX() const { return m_MouseX; }
		float GetY() const { return m_MouseY; }

		std::string ToString() const override
		{
			return fmt::format("MouseMovedEvent: {}, {}", m_MouseX, m_MouseY);
		}

		ST_EVENT_CLASS_TYPE(MouseMoved)
		ST_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		float m_MouseX;
		float m_MouseY;
	};

	class MouseScrolledEvent : public Event
	{
	public:
		MouseScrolledEvent(float xOffset, float yOffset)
			: m_XOffset(xOffset), m_YOffset(yOffset)
		{
		}

		float GetXOffset() const { return m_XOffset; }
		float GetYOffset() const { return m_YOffset; }

		std::string ToString() const override
		{
			return fmt::format("MouseScrolledEvent: {}, {}", m_XOffset, m_YOffset);
		}

		ST_EVENT_CLASS_TYPE(MouseScrolled)
		ST_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		float m_XOffset;
		float m_YOffset;
	};

	class MouseButtonEvent : public Event
	{
	public:
		MouseCode GetMouseButton() const { return m_Button; }

		ST_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton)
	protected:
		MouseButtonEvent(MouseCode button)
			: m_Button(button)
		{
		}

		MouseCode m_Button;
	};

	class MouseButtonPressedEvent : public MouseButtonEvent
	{
	public:
		MouseButtonPressedEvent(MouseCode button)
			: MouseButtonEvent(button)
		{
		}

		std::string ToString() const override
		{
			return fmt::format("MouseButtonPressedEvent: {}", m_Button);
		}

		ST_EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	class MouseButtonReleasedEvent : public MouseButtonEvent
	{
	public:
		MouseButtonReleasedEvent(MouseCode button)
			: MouseButtonEvent(button)
		{
		}

		std::string ToString() const override
		{
			return fmt::format("MouseButtonReleasedEvent: {}", m_Button);
		}

		ST_EVENT_CLASS_TYPE(MouseButtonReleased)
	};

}
