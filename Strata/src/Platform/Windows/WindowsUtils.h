#pragma once

#include <Windows.h>

#include <string>
#include <string_view>
#include <utility>

namespace Strata::WindowsUtils
{

	// Owns a kernel handle and closes it when it goes out of scope, so every early return closes what was opened. Null
	// and INVALID_HANDLE_VALUE both mean "no handle".
	class ScopedHandle
	{
	public:
		ScopedHandle() = default;
		explicit ScopedHandle(HANDLE handle)
			: m_Handle(handle)
		{
		}

		~ScopedHandle() { Reset(); }

		ScopedHandle(const ScopedHandle&) = delete;
		ScopedHandle& operator=(const ScopedHandle&) = delete;

		HANDLE Get() const { return m_Handle; }
		// For functions that return a handle through a pointer; closes the current handle first.
		HANDLE* Receive()
		{
			Reset();
			return &m_Handle;
		}
		HANDLE Release() { return std::exchange(m_Handle, nullptr); }
		void Reset(HANDLE handle = nullptr)
		{
			if (IsValid())
				CloseHandle(m_Handle);
			m_Handle = handle;
		}
		bool IsValid() const { return m_Handle && m_Handle != INVALID_HANDLE_VALUE; }
	private:
		HANDLE m_Handle = nullptr;
	};

	inline std::wstring Utf8ToWide(std::string_view text)
	{
		if (text.empty())
			return {};

		const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		std::wstring result(static_cast<size_t>(length), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
		return result;
	}

	inline std::string WideToUtf8(std::wstring_view text)
	{
		if (text.empty())
			return {};

		const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string result(static_cast<size_t>(length), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
		return result;
	}

	inline std::string GetErrorMessage(DWORD errorCode)
	{
		LPWSTR buffer = nullptr;
		const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
		std::string message = length > 0 ? WideToUtf8(std::wstring_view(buffer, length)) : std::string("Unknown error");
		if (buffer)
			LocalFree(buffer);
		while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' || message.back() == '.'))
			message.pop_back();
		return message;
	}

}
