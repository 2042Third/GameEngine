#pragma once

#include "Strata/Events/Event.h"
#include "Strata/Input/KeyCodes.h"

namespace Strata
{

	class KeyEvent : public Event
	{
	public:
		KeyCode GetKeyCode() const { return m_KeyCode; }

		ST_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)
	protected:
		KeyEvent(KeyCode keycode)
			: m_KeyCode(keycode)
		{
		}

		KeyCode m_KeyCode;
	};

	class KeyPressedEvent : public KeyEvent
	{
	public:
		KeyPressedEvent(KeyCode keycode, bool isRepeat = false)
			: KeyEvent(keycode), m_IsRepeat(isRepeat)
		{
		}

		bool IsRepeat() const { return m_IsRepeat; }

		std::string ToString() const override
		{
			return fmt::format("KeyPressedEvent: {} (repeat = {})", m_KeyCode, m_IsRepeat);
		}

		ST_EVENT_CLASS_TYPE(KeyPressed)
	private:
		bool m_IsRepeat;
	};

	class KeyReleasedEvent : public KeyEvent
	{
	public:
		KeyReleasedEvent(KeyCode keycode)
			: KeyEvent(keycode)
		{
		}

		std::string ToString() const override
		{
			return fmt::format("KeyReleasedEvent: {}", m_KeyCode);
		}

		ST_EVENT_CLASS_TYPE(KeyReleased)
	};

	class KeyTypedEvent : public Event
	{
	public:
		KeyTypedEvent(uint32_t codepoint)
			: m_Codepoint(codepoint)
		{
		}

		uint32_t GetCodepoint() const { return m_Codepoint; }

		ST_EVENT_CLASS_TYPE(KeyTyped)
		ST_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)
	private:
		uint32_t m_Codepoint;
	};

}
