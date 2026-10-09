#pragma once

#include "Strata/Events/Event.h"

#include <filesystem>
#include <vector>

namespace Strata
{

	class WindowResizeEvent : public Event
	{
	public:
		WindowResizeEvent(uint32_t width, uint32_t height)
			: m_Width(width), m_Height(height)
		{
		}

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		std::string ToString() const override
		{
			return fmt::format("WindowResizeEvent: {}, {}", m_Width, m_Height);
		}

		ST_EVENT_CLASS_TYPE(WindowResize)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	private:
		uint32_t m_Width;
		uint32_t m_Height;
	};

	class WindowCloseEvent : public Event
	{
	public:
		WindowCloseEvent() = default;

		ST_EVENT_CLASS_TYPE(WindowClose)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowFocusEvent : public Event
	{
	public:
		WindowFocusEvent() = default;

		ST_EVENT_CLASS_TYPE(WindowFocus)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowLostFocusEvent : public Event
	{
	public:
		WindowLostFocusEvent() = default;

		ST_EVENT_CLASS_TYPE(WindowLostFocus)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowMovedEvent : public Event
	{
	public:
		WindowMovedEvent(int32_t x, int32_t y)
			: m_X(x), m_Y(y)
		{
		}

		int32_t GetX() const { return m_X; }
		int32_t GetY() const { return m_Y; }

		ST_EVENT_CLASS_TYPE(WindowMoved)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	private:
		int32_t m_X;
		int32_t m_Y;
	};

	class WindowFileDropEvent : public Event
	{
	public:
		WindowFileDropEvent(std::vector<std::filesystem::path> paths)
			: m_Paths(std::move(paths))
		{
		}

		const std::vector<std::filesystem::path>& GetPaths() const { return m_Paths; }

		ST_EVENT_CLASS_TYPE(WindowFileDrop)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	private:
		std::vector<std::filesystem::path> m_Paths;
	};

}
