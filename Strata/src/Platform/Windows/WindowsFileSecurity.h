#pragma once

// Windows-only (like WindowsUtils.h): included by Windows translation units, never by engine headers.
#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Strata::WindowsFileSecurity
{

	// A well-known SID such as WinBuiltinAdministratorsSid (empty if it cannot be created).
	std::vector<uint8_t> MakeWellKnownSid(WELL_KNOWN_SID_TYPE type);
	// The SID of the account this process runs as (empty, with the reason in error, on failure).
	std::vector<uint8_t> GetCurrentUserSid(std::string& error);

	// Whether only currentUser, SYSTEM and the Administrators group (who can take ownership of anything anyway)
	// can modify an object with this owner and DACL. The owner must be one of them, whatever the elevation of
	// this process: an elevated run creates objects owned by Administrators, and they stay usable afterwards.
	// No access-allowed entry may grant modify rights to anyone else. name only appears in error.
	bool CheckOwnerAndAccess(PSID owner, PACL dacl, PSID currentUser, const std::string& name, std::string* error);
	// Reads the owner and DACL of an open handle (opened with READ_CONTROL) and checks them for the current user.
	bool CheckHandleOwnerAndAccess(HANDLE handle, const std::string& name, std::string* error);

	// Whether a reparse point redirects name resolution to another object (symbolic links, junctions and other
	// name surrogates). Other reparse points, such as cloud placeholders or deduplicated files, hold the file's
	// own data and are fine.
	bool IsNameSurrogate(DWORD attributes, DWORD reparseTag);
	// IsNameSurrogate for an open handle; nullopt if its attributes cannot be read.
	std::optional<bool> IsNameSurrogate(HANDLE handle);

}
