#include "stpch.h"
#include "Platform/Windows/WindowsFileSecurity.h"

#include "Platform/Windows/WindowsUtils.h"

#include <aclapi.h>

namespace Strata::WindowsFileSecurity
{

	namespace
	{

		// Rights that let a principal change a file or directory: its data or entries, its attributes, its name,
		// or its security.
		constexpr ACCESS_MASK c_ModifyRights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES
			| FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

		bool SetError(std::string* error, std::string message)
		{
			if (error)
				*error = std::move(message);
			return false;
		}

		bool IsSid(PSID sid, const std::vector<uint8_t>& wellKnownSid)
		{
			return !wellKnownSid.empty() && EqualSid(sid, const_cast<uint8_t*>(wellKnownSid.data())) != FALSE;
		}

	}

	std::vector<uint8_t> MakeWellKnownSid(WELL_KNOWN_SID_TYPE type)
	{
		DWORD size = SECURITY_MAX_SID_SIZE;
		std::vector<uint8_t> sid(size);
		if (!CreateWellKnownSid(type, nullptr, sid.data(), &size))
			return {};
		sid.resize(size);
		return sid;
	}

	std::vector<uint8_t> GetCurrentUserSid(std::string& error)
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		{
			error = "OpenProcessToken failed: " + WindowsUtils::GetErrorMessage(::GetLastError());
			return {};
		}

		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<uint8_t> tokenUser(size);
		const bool queried = size > 0 && GetTokenInformation(token, TokenUser, tokenUser.data(), size, &size);
		const DWORD queryError = ::GetLastError();
		CloseHandle(token);
		if (!queried)
		{
			error = "GetTokenInformation failed: " + WindowsUtils::GetErrorMessage(queryError);
			return {};
		}

		PSID user = reinterpret_cast<const TOKEN_USER*>(tokenUser.data())->User.Sid;
		std::vector<uint8_t> sid(GetLengthSid(user));
		if (!CopySid(static_cast<DWORD>(sid.size()), sid.data(), user))
		{
			error = "CopySid failed: " + WindowsUtils::GetErrorMessage(::GetLastError());
			return {};
		}
		return sid;
	}

	bool CheckOwnerAndAccess(PSID owner, PACL dacl, PSID currentUser, const std::string& name, std::string* error)
	{
		const std::vector<uint8_t> administrators = MakeWellKnownSid(WinBuiltinAdministratorsSid);
		const std::vector<uint8_t> system = MakeWellKnownSid(WinLocalSystemSid);
		const std::vector<uint8_t> ownerRights = MakeWellKnownSid(WinCreatorOwnerRightsSid);
		if (administrators.empty() || system.empty() || ownerRights.empty())
			return SetError(error, "Cannot create the well-known security identifiers: " + WindowsUtils::GetErrorMessage(::GetLastError()));

		if (!owner)
			return SetError(error, fmt::format("'{}' has no owner (its file system does not support permissions)", name));
		const bool trustedOwner = EqualSid(owner, currentUser) || IsSid(owner, administrators) || IsSid(owner, system);
		if (!trustedOwner)
			return SetError(error, fmt::format("'{}' is owned by another account", name));
		if (!dacl)
			return SetError(error, fmt::format("'{}' has no access control list, so any account may modify it", name));

		for (DWORD index = 0; index < dacl->AceCount; index++)
		{
			void* entry = nullptr;
			if (!GetAce(dacl, index, &entry))
				return SetError(error, fmt::format("Cannot read the permissions of '{}': {}", name, WindowsUtils::GetErrorMessage(::GetLastError())));

			const ACE_HEADER* header = static_cast<const ACE_HEADER*>(entry);
			if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
				continue; // Applies only to objects created inside it later
			if (header->AceType == ACCESS_DENIED_ACE_TYPE)
				continue; // Deny entries only take rights away
			if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
				return SetError(error, fmt::format("'{}' has an access control entry of an unexpected kind", name));

			const ACCESS_ALLOWED_ACE* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
			if ((allowed->Mask & c_ModifyRights) == 0)
				continue;

			PSID sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart));
			const bool trusted = EqualSid(sid, currentUser) || EqualSid(sid, owner) || IsSid(sid, system) || IsSid(sid, administrators) || IsSid(sid, ownerRights);
			if (!trusted)
				return SetError(error, fmt::format("'{}' can be modified by another account", name));
		}
		return true;
	}

	bool CheckHandleOwnerAndAccess(HANDLE handle, const std::string& name, std::string* error)
	{
		PSID owner = nullptr;
		PACL dacl = nullptr;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		const DWORD result = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor);
		if (result != ERROR_SUCCESS)
			return SetError(error, fmt::format("Cannot read the permissions of '{}': {}", name, WindowsUtils::GetErrorMessage(result)));

		// owner and dacl point into the descriptor.
		struct DescriptorGuard
		{
			PSECURITY_DESCRIPTOR Descriptor;
			~DescriptorGuard() { LocalFree(Descriptor); }
		} descriptorGuard { descriptor };

		std::string userError;
		std::vector<uint8_t> user = GetCurrentUserSid(userError);
		if (user.empty())
			return SetError(error, userError);
		return CheckOwnerAndAccess(owner, dacl, user.data(), name, error);
	}

	bool IsNameSurrogate(DWORD attributes, DWORD reparseTag)
	{
		return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(reparseTag);
	}

	std::optional<bool> IsNameSurrogate(HANDLE handle)
	{
		FILE_ATTRIBUTE_TAG_INFO information = {};
		if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &information, sizeof(information)))
			return std::nullopt;
		return IsNameSurrogate(information.FileAttributes, information.ReparseTag);
	}

}
