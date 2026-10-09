#pragma once

#include <Windows.h>

#include <string>
#include <string_view>

namespace Strata::WindowsUtils
{

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
