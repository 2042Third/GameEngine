#include <doctest/doctest.h>

#include "Network/NetworkTestHelpers.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/EditorSession.h"
#include "Strata/Network/JsonRpc.h"
#include "TestHelpers.h"

#include <algorithm>
#include <set>
#include <string>

using namespace Strata;

namespace
{
	EditorSessionInfo MakeSession(uint32_t processId, uint16_t port, std::string startedAt, std::string projectPath = {})
	{
		EditorSessionInfo info;
		info.ProcessId = processId;
		info.ProcessStartTime = Platform::GetProcessStartTime(processId).value_or(0);
		info.Port = port;
		info.Token = EditorSession::GenerateSessionToken();
		info.ProjectPath = std::move(projectPath);
		info.EditorVersion = "0.1.0";
		info.Headless = true;
		info.StartedAt = std::move(startedAt);
		return info;
	}

	bool WriteSession(const std::filesystem::path& sessionDirectory, const EditorSessionInfo& session, uint32_t fileProcessId = 0)
	{
		const uint32_t nameId = fileProcessId != 0 ? fileProcessId : session.ProcessId;
		return Platform::WritePrivateFile(EditorSession::GetSessionFilePath(sessionDirectory, nameId), session.ToJson().dump());
	}

	bool IsHexDigit(char character)
	{
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
	}

	size_t CountEntries(const std::filesystem::path& directory)
	{
		size_t count = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(directory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			count++;
		return count;
	}
}

TEST_SUITE("Network.EditorSession")
{
	TEST_CASE("Session info serializes with the documented keys")
	{
		const EditorSessionInfo info = MakeSession(4242, 50123, "2026-01-02T03:04:05Z", "C:/Projects/Game \xC3\xA9");
		const nlohmann::json json = info.ToJson();

		std::set<std::string> keys;
		for (const auto& [key, value] : json.items())
			keys.insert(key);
		CHECK(keys == std::set<std::string> { "ProcessId", "ProcessStartTime", "Address", "Port", "Token", "ProjectPath", "EditorVersion", "Headless", "StartedAt" });
		CHECK(json["ProcessId"] == 4242);
		CHECK(json["Port"] == 50123);
		CHECK(json["Headless"] == true);
		CHECK(json["Address"] == "127.0.0.1");

		const std::optional<EditorSessionInfo> parsed = EditorSessionInfo::FromJson(json);
		REQUIRE(parsed.has_value());
		CHECK(parsed->ProcessId == info.ProcessId);
		CHECK(parsed->ProcessStartTime == info.ProcessStartTime);
		CHECK(parsed->Address == info.Address);
		CHECK(parsed->Port == info.Port);
		CHECK(parsed->Token == info.Token);
		CHECK(parsed->ProjectPath == info.ProjectPath);
		CHECK(parsed->EditorVersion == info.EditorVersion);
		CHECK(parsed->Headless == info.Headless);
		CHECK(parsed->StartedAt == info.StartedAt);
	}

	TEST_CASE("Invalid session JSON is rejected")
	{
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json::array()).has_value());
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json { { "Port", 1234 } }).has_value());
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", 1 } }).has_value());
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", 1 }, { "Port", 70000 } }).has_value());
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", -5 }, { "Port", 80 } }).has_value());
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", "12" }, { "Port", 80 } }).has_value());

		// A session without a token could not be authenticated to.
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", 7 }, { "Port", 80 } }).has_value());
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", 7 }, { "Port", 80 }, { "Token", "" } }).has_value());
		CHECK_FALSE(EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", 7 }, { "Port", 80 }, { "Token", 1234 } }).has_value());

		// Only ProcessId, Port and Token are required; mistyped optional fields fall back to defaults.
		const std::optional<EditorSessionInfo> minimal = EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", 7 }, { "Port", 80 }, { "Token", "t" }, { "Headless", "yes" } });
		REQUIRE(minimal.has_value());
		CHECK(minimal->Token == "t");
		CHECK_FALSE(minimal->Headless);
		CHECK(minimal->ProcessStartTime == 0);
		CHECK(minimal->Address == "127.0.0.1");
	}

	TEST_CASE("The current process is described for its session")
	{
		const EditorSessionInfo info = EditorSession::DescribeCurrentProcess();
		CHECK(info.ProcessId == Platform::GetProcessID());
		CHECK(info.ProcessStartTime == Platform::GetProcessStartTime(Platform::GetProcessID()));
		CHECK(info.ProcessStartTime != 0);
		CHECK(info.Address == "127.0.0.1");
		CHECK_FALSE(info.EditorVersion.empty());
		CHECK(info.StartedAt.size() == 20);
		CHECK(EditorSession::IsSessionProcessRunning(info));
	}

	TEST_CASE("A session whose process id now belongs to another process is stale")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessionsReused");
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("EditorSessionsReusedProject");
		Tests::LiveProcess running;

		// The same id with another start time is what a reused id looks like.
		EditorSessionInfo reused = MakeSession(running.GetProcessId(), 46001, "2026-01-01T00:00:00Z", FileSystem::ToUTF8(project));
		reused.ProcessStartTime += 1;
		CHECK(EditorSession::GetSessionProcessState(reused) == SessionProcessState::Exited);
		CHECK_FALSE(EditorSession::IsSessionProcessRunning(reused));
		REQUIRE(WriteSession(sessionDirectory, reused));
		REQUIRE(FileSystem::WriteText(EditorSession::GetProjectSessionFilePath(project), nlohmann::json { { "ProcessId", running.GetProcessId() } }.dump()));
		CHECK_FALSE(EditorSession::ReadProjectSession(project, sessionDirectory).has_value());
		CHECK(EditorSession::FindSessions(sessionDirectory).empty());
		CHECK_FALSE(FileSystem::Exists(EditorSession::GetSessionFilePath(sessionDirectory, running.GetProcessId())));

		// A live process whose session has no start time (e.g. the editor could not read its own) cannot be verified:
		// it is neither used nor deleted, since it may well be a running editor.
		EditorSessionInfo unverifiable = MakeSession(running.GetProcessId(), 46002, "2026-01-01T00:00:00Z");
		unverifiable.ProcessStartTime = 0;
		CHECK(EditorSession::GetSessionProcessState(unverifiable) == SessionProcessState::Unverifiable);
		CHECK_FALSE(EditorSession::IsSessionProcessRunning(unverifiable));
		REQUIRE(WriteSession(sessionDirectory, unverifiable));
		CHECK(EditorSession::FindSessions(sessionDirectory).empty());
		CHECK(FileSystem::Exists(EditorSession::GetSessionFilePath(sessionDirectory, running.GetProcessId())));

		// With the right start time it is the running process's session.
		const EditorSessionInfo current = MakeSession(running.GetProcessId(), 46003, "2026-01-01T00:00:00Z", FileSystem::ToUTF8(project));
		CHECK(EditorSession::GetSessionProcessState(current) == SessionProcessState::Running);
		CHECK(EditorSession::IsSessionProcessRunning(current));
		REQUIRE(WriteSession(sessionDirectory, current));
		REQUIRE(EditorSession::FindSessions(sessionDirectory).size() == 1);
		CHECK(EditorSession::ReadProjectSession(project, sessionDirectory).has_value());

		// A session of a process that exited is stale, and FindSessions deletes it. (If a new process has taken the
		// id by now, its start time differs, so the verdict is the same.)
		Tests::ExitedProcess exited;
		EditorSessionInfo gone = MakeSession(exited.GetProcessId(), 46004, "2026-01-01T00:00:00Z");
		gone.ProcessStartTime = exited.GetStartTime();
		CHECK(EditorSession::GetSessionProcessState(gone) == SessionProcessState::Exited);
		CHECK_FALSE(EditorSession::IsSessionProcessRunning(gone));
		REQUIRE(WriteSession(sessionDirectory, gone));
		CHECK(EditorSession::FindSessions(sessionDirectory).size() == 1);
		CHECK_FALSE(FileSystem::Exists(EditorSession::GetSessionFilePath(sessionDirectory, gone.ProcessId)));

		// Without a start time, a session whose process is gone is stale too. Only a reused id (possible on POSIX,
		// where the exited process was reaped) would make it unverifiable instead.
		gone.ProcessStartTime = 0;
		const SessionProcessState withoutStartTime = EditorSession::GetSessionProcessState(gone);
		if (Platform::IsProcessAlive(gone.ProcessId))
			CHECK(withoutStartTime == SessionProcessState::Unverifiable);
		else
			CHECK(withoutStartTime == SessionProcessState::Exited);
		CHECK_FALSE(EditorSession::IsSessionProcessRunning(gone));
	}

	TEST_CASE("A stale session file is deleted only while it is still stale")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessionsRace");
		Tests::LiveProcess running;
		const std::filesystem::path path = EditorSession::GetSessionFilePath(sessionDirectory, running.GetProcessId());

		// FindSessions read a stale session (the id with another start time, as after the id was reused)...
		EditorSessionInfo stale = MakeSession(running.GetProcessId(), 47001, "2026-01-01T00:00:00Z");
		stale.ProcessStartTime += 1;
		REQUIRE(EditorSession::GetSessionProcessState(stale) == SessionProcessState::Exited);
		REQUIRE(WriteSession(sessionDirectory, stale));

		// ...but before deleting it, the process that now has the id wrote its own session at the same path.
		const EditorSessionInfo replacement = MakeSession(running.GetProcessId(), 47002, "2026-02-01T00:00:00Z");
		REQUIRE(EditorSession::GetSessionProcessState(replacement) == SessionProcessState::Running);
		REQUIRE(WriteSession(sessionDirectory, replacement));
		CHECK_FALSE(EditorSession::RemoveStaleSessionFile(path));
		const std::optional<EditorSessionInfo> kept = EditorSession::ReadSessionFile(path);
		REQUIRE(kept.has_value());
		CHECK(kept->Port == replacement.Port);
		CHECK(CountEntries(sessionDirectory) == 1); // Nothing is left behind

		// A file that is still stale is deleted, and a missing one is left alone.
		REQUIRE(WriteSession(sessionDirectory, stale));
		CHECK(EditorSession::RemoveStaleSessionFile(path));
		CHECK_FALSE(FileSystem::Exists(path));
		CHECK(CountEntries(sessionDirectory) == 0);
		CHECK_FALSE(EditorSession::RemoveStaleSessionFile(path));
	}

	TEST_CASE("Session files are written owner-only, found and removed")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("EditorSessions");
		const std::filesystem::path projectDirectory = Tests::CreateTemporaryDirectory("EditorSessionProject");
		const std::filesystem::path sessionDirectory = root / "Sessions";
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		std::string error;
		const std::optional<std::filesystem::path> resolved = EditorSession::GetSessionDirectory(&error);
		REQUIRE_MESSAGE(resolved.has_value(), error);
		CHECK(*resolved == sessionDirectory);
		CHECK(FileSystem::IsDirectory(sessionDirectory));

		Tests::LiveProcess olderProcess;
		Tests::LiveProcess newerProcess;
		const EditorSessionInfo older = MakeSession(olderProcess.GetProcessId(), 40001, "2026-01-01T10:00:00Z");
		const EditorSessionInfo newer = MakeSession(newerProcess.GetProcessId(), 40002, "2026-03-01T10:00:00Z", FileSystem::ToUTF8(projectDirectory));
		REQUIRE(EditorSession::WriteSessionFiles(older, &error));
		REQUIRE(EditorSession::WriteSessionFiles(newer, &error));

		const std::filesystem::path newerFile = EditorSession::GetSessionFilePath(sessionDirectory, newer.ProcessId);
		CHECK(newerFile.filename() == FileSystem::FromUTF8(std::to_string(newer.ProcessId) + ".json"));
		CHECK(Platform::IsTrustedFile(newerFile));
#if defined(ST_PLATFORM_POSIX)
		std::error_code statusError;
		CHECK((std::filesystem::status(newerFile, statusError).permissions() & std::filesystem::perms::all) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
#endif

		// The project pointer names the process, but never carries the port or the token.
		const std::filesystem::path pointerFile = EditorSession::GetProjectSessionFilePath(projectDirectory);
		CHECK(pointerFile == projectDirectory / ".strata" / "EditorSession.json");
		const std::optional<nlohmann::json> pointer = JsonRpc::Parse(FileSystem::ReadText(pointerFile).value_or(""));
		REQUIRE(pointer.has_value());
		CHECK((*pointer)["ProcessId"] == newer.ProcessId);
		CHECK_FALSE(pointer->contains("Token"));
		CHECK_FALSE(pointer->contains("Port"));
		CHECK(FileSystem::ReadText(pointerFile).value().find(newer.Token) == std::string::npos);

		const std::vector<EditorSessionInfo> sessions = EditorSession::FindSessions();
		REQUIRE(sessions.size() == 2);
		CHECK(sessions[0].ProcessId == newer.ProcessId); // Newest first
		CHECK(sessions[1].ProcessId == older.ProcessId);
		CHECK(sessions[0].Token == newer.Token);

		// The port and token come from the per-user file the pointer refers to.
		const std::optional<EditorSessionInfo> projectSession = EditorSession::ReadProjectSession(projectDirectory);
		REQUIRE(projectSession.has_value());
		CHECK(projectSession->ProcessId == newer.ProcessId);
		CHECK(projectSession->Port == newer.Port);
		CHECK(projectSession->Token == newer.Token);

		EditorSession::RemoveSessionFiles(newer);
		CHECK_FALSE(FileSystem::Exists(newerFile));
		CHECK_FALSE(FileSystem::Exists(pointerFile));
		CHECK_FALSE(EditorSession::ReadProjectSession(projectDirectory).has_value());
		REQUIRE(EditorSession::FindSessions().size() == 1);
		CHECK(EditorSession::FindSessions()[0].ProcessId == older.ProcessId);

		EditorSession::RemoveSessionFiles(older);
		CHECK(EditorSession::FindSessions().empty());
		CHECK(CountEntries(projectDirectory / ".strata") == 0); // No temporary files are left behind
	}

	TEST_CASE("Sessions of processes that exited are removed")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessionsStale");
		Tests::LiveProcess running;
		Tests::ExitedProcess exited;
		REQUIRE(exited.GetProcessId() != 0);
		REQUIRE(WriteSession(sessionDirectory, MakeSession(running.GetProcessId(), 41001, "2026-01-01T00:00:00Z")));
		EditorSessionInfo exitedSession = MakeSession(exited.GetProcessId(), 41002, "2026-02-01T00:00:00Z");
		exitedSession.ProcessStartTime = exited.GetStartTime();
		REQUIRE(WriteSession(sessionDirectory, exitedSession));

		const std::vector<EditorSessionInfo> sessions = EditorSession::FindSessions(sessionDirectory);
		REQUIRE(sessions.size() == 1);
		CHECK(sessions[0].ProcessId == running.GetProcessId());
		CHECK_FALSE(FileSystem::Exists(EditorSession::GetSessionFilePath(sessionDirectory, exited.GetProcessId())));
		CHECK(FileSystem::Exists(EditorSession::GetSessionFilePath(sessionDirectory, running.GetProcessId())));
	}

	TEST_CASE("Corrupt, misnamed and untrusted session files are ignored")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessionsUntrusted");
		Tests::LiveProcess running;
		Tests::LiveProcess other;
		REQUIRE(Platform::WritePrivateFile(sessionDirectory / "999.json", "{ corrupt"));
		REQUIRE(Platform::WritePrivateFile(sessionDirectory / "notes.txt", "not a session"));
		REQUIRE(Platform::WritePrivateFile(sessionDirectory / "huge.json", std::string(128 * 1024, ' ') + "{}"));
		// A file whose name does not match the process it describes (e.g. a copy) is not a session.
		REQUIRE(WriteSession(sessionDirectory, MakeSession(running.GetProcessId(), 42001, "2026-01-01T00:00:00Z"), other.GetProcessId()));
		CHECK(EditorSession::FindSessions(sessionDirectory).empty());

#if defined(ST_PLATFORM_POSIX)
		// Files other users could have written, and links, are never trusted.
		const EditorSessionInfo session = MakeSession(running.GetProcessId(), 42002, "2026-01-01T00:00:00Z");
		const std::filesystem::path sessionFile = EditorSession::GetSessionFilePath(sessionDirectory, session.ProcessId);
		REQUIRE(WriteSession(sessionDirectory, session));
		REQUIRE(EditorSession::FindSessions(sessionDirectory).size() == 1);
		std::filesystem::permissions(sessionFile, std::filesystem::perms::others_write, std::filesystem::perm_options::add);
		CHECK(EditorSession::FindSessions(sessionDirectory).empty());

		const std::filesystem::path elsewhere = Tests::CreateTemporaryDirectory("EditorSessionsElsewhere") / "session.json";
		REQUIRE(Platform::WritePrivateFile(elsewhere, session.ToJson().dump()));
		std::filesystem::remove(sessionFile);
		std::error_code linkError;
		std::filesystem::create_symlink(elsewhere, sessionFile, linkError);
		REQUIRE_FALSE(linkError);
		CHECK(EditorSession::FindSessions(sessionDirectory).empty());
#endif
	}

	TEST_CASE("The project pointer is not trusted")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessionsPointer");
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("EditorSessionPointerProject");
		const std::filesystem::path otherProject = Tests::CreateTemporaryDirectory("EditorSessionPointerOther");
		const std::filesystem::path pointerFile = EditorSession::GetProjectSessionFilePath(project);
		Tests::LiveProcess editor;

		auto writePointer = [&](const nlohmann::json& pointer) { REQUIRE(FileSystem::WriteText(pointerFile, pointer.dump())); };

		SUBCASE("A valid pointer resolves to the per-user session, ignoring any port or token it carries")
		{
			const EditorSessionInfo session = MakeSession(editor.GetProcessId(), 43001, "2026-01-01T00:00:00Z", FileSystem::ToUTF8(project));
			REQUIRE(WriteSession(sessionDirectory, session));
			writePointer(nlohmann::json { { "ProcessId", editor.GetProcessId() }, { "Port", 1 }, { "Token", "planted" } });
			const std::optional<EditorSessionInfo> resolved = EditorSession::ReadProjectSession(project, sessionDirectory);
			REQUIRE(resolved.has_value());
			CHECK(resolved->Port == 43001);
			CHECK(resolved->Token == session.Token);
		}

		SUBCASE("The referenced session must be for the same project")
		{
			REQUIRE(WriteSession(sessionDirectory, MakeSession(editor.GetProcessId(), 43002, "2026-01-01T00:00:00Z", FileSystem::ToUTF8(otherProject))));
			writePointer(nlohmann::json { { "ProcessId", editor.GetProcessId() } });
			CHECK_FALSE(EditorSession::ReadProjectSession(project, sessionDirectory).has_value());
		}

		SUBCASE("The referenced process must be running")
		{
			Tests::ExitedProcess exited;
			EditorSessionInfo session = MakeSession(exited.GetProcessId(), 43003, "2026-01-01T00:00:00Z", FileSystem::ToUTF8(project));
			session.ProcessStartTime = exited.GetStartTime();
			REQUIRE(WriteSession(sessionDirectory, session));
			writePointer(nlohmann::json { { "ProcessId", exited.GetProcessId() } });
			CHECK_FALSE(EditorSession::ReadProjectSession(project, sessionDirectory).has_value());
		}

		SUBCASE("A pointer without a per-user session, or a malformed pointer, resolves to nothing")
		{
			writePointer(nlohmann::json { { "ProcessId", editor.GetProcessId() } });
			CHECK_FALSE(EditorSession::ReadProjectSession(project, sessionDirectory).has_value());
			REQUIRE(FileSystem::WriteText(pointerFile, "not json"));
			CHECK_FALSE(EditorSession::ReadProjectSession(project, sessionDirectory).has_value());
			REQUIRE(FileSystem::Remove(pointerFile));
			REQUIRE(FileSystem::CreateDirectories(pointerFile));
			CHECK_FALSE(EditorSession::ReadProjectSession(project, sessionDirectory).has_value());
		}
	}

	TEST_CASE("A session follows the editor to another project")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessionsMove") / "Sessions";
		const std::filesystem::path firstProject = Tests::CreateTemporaryDirectory("EditorSessionFirstProject");
		const std::filesystem::path secondProject = Tests::CreateTemporaryDirectory("EditorSessionSecondProject");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));
		Tests::LiveProcess editorProcess;

		const EditorSessionInfo first = MakeSession(editorProcess.GetProcessId(), 46001, "2026-01-01T10:00:00Z", FileSystem::ToUTF8(firstProject));
		REQUIRE(EditorSession::WriteSessionFiles(first));
		REQUIRE(EditorSession::ReadProjectSession(firstProject).has_value());

		// The editor opens another project: the session is rewritten in place and the old pointer goes away.
		EditorSessionInfo second = first;
		second.ProjectPath = FileSystem::ToUTF8(secondProject);
		REQUIRE(EditorSession::WriteSessionFiles(second));
		EditorSession::RemoveProjectPointer(first);

		CHECK_FALSE(FileSystem::Exists(EditorSession::GetProjectSessionFilePath(firstProject)));
		CHECK(CountEntries(firstProject / ".strata") == 0);
		const std::optional<EditorSessionInfo> moved = EditorSession::ReadProjectSession(secondProject);
		REQUIRE(moved.has_value());
		CHECK(moved->Port == first.Port);
		const std::vector<EditorSessionInfo> sessions = EditorSession::FindSessions();
		REQUIRE(sessions.size() == 1);
		CHECK(sessions[0].ProjectPath == second.ProjectPath);

		// Without a project there is no pointer to remove.
		EditorSessionInfo withoutProject = second;
		withoutProject.ProjectPath.clear();
		EditorSession::RemoveProjectPointer(withoutProject);
		CHECK(EditorSession::ReadProjectSession(secondProject).has_value());

		EditorSession::RemoveSessionFiles(second);
		CHECK(EditorSession::FindSessions().empty());
		CHECK_FALSE(FileSystem::Exists(EditorSession::GetProjectSessionFilePath(secondProject)));
	}

	TEST_CASE("Removing a session keeps a project pointer that another editor took over")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessionsTakeover") / "Sessions";
		const std::filesystem::path projectDirectory = Tests::CreateTemporaryDirectory("EditorSessionTakeoverProject");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));
		Tests::LiveProcess firstProcess;
		Tests::LiveProcess secondProcess;

		const EditorSessionInfo first = MakeSession(firstProcess.GetProcessId(), 44001, "2026-01-01T10:00:00Z", FileSystem::ToUTF8(projectDirectory));
		const EditorSessionInfo second = MakeSession(secondProcess.GetProcessId(), 44002, "2026-01-01T11:00:00Z", FileSystem::ToUTF8(projectDirectory));
		REQUIRE(EditorSession::WriteSessionFiles(first));
		REQUIRE(EditorSession::WriteSessionFiles(second));

		EditorSession::RemoveSessionFiles(first);
		const std::optional<EditorSessionInfo> projectSession = EditorSession::ReadProjectSession(projectDirectory);
		REQUIRE(projectSession.has_value());
		CHECK(projectSession->ProcessId == second.ProcessId);
		CHECK(EditorSession::FindSessions().size() == 1);
		CHECK(CountEntries(projectDirectory / ".strata") == 1);
	}

#if defined(ST_PLATFORM_POSIX)
	TEST_CASE("Without a per-user directory, sessions are refused rather than shared")
	{
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", "");
		Tests::ScopedEnvironmentVariable home("HOME", "");
		Tests::ScopedEnvironmentVariable dataHome("XDG_DATA_HOME", "");

		std::string error;
		CHECK_FALSE(EditorSession::GetSessionDirectory(&error).has_value());
		CHECK(error.find("STRATA_SESSION_DIR") != std::string::npos);
		CHECK_FALSE(EditorSession::WriteSessionFiles(MakeSession(Platform::GetProcessID(), 45001, "2026-01-01T00:00:00Z")));
	}

	TEST_CASE("A session directory reached through a symbolic link is refused")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("EditorSessionsLink");
		REQUIRE(FileSystem::CreateDirectories(root / "Real"));
		std::error_code linkError;
		std::filesystem::create_directory_symlink(root / "Real", root / "Link", linkError);
		REQUIRE_FALSE(linkError);
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(root / "Link"));

		std::string error;
		CHECK_FALSE(EditorSession::GetSessionDirectory(&error).has_value());
		CHECK_FALSE(error.empty());
	}
#endif

	TEST_CASE("Session tokens are random hexadecimal strings")
	{
		const std::string token = EditorSession::GenerateSessionToken();
		CHECK(token.size() == 32);
		CHECK(std::all_of(token.begin(), token.end(), IsHexDigit));
		CHECK(EditorSession::GenerateSessionToken() != token);
	}

	TEST_CASE("Timestamps are ISO-8601 UTC")
	{
		const std::string timestamp = EditorSession::GetCurrentTimestamp();
		REQUIRE(timestamp.size() == 20);
		CHECK(timestamp[4] == '-');
		CHECK(timestamp[7] == '-');
		CHECK(timestamp[10] == 'T');
		CHECK(timestamp[13] == ':');
		CHECK(timestamp[16] == ':');
		CHECK(timestamp[19] == 'Z');
		CHECK(timestamp.substr(0, 2) == "20");
	}
}
