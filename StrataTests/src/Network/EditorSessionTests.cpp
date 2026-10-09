#include <doctest/doctest.h>

#include "Network/NetworkTestHelpers.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Network/EditorSession.h"
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
		info.Port = port;
		info.Token = EditorSession::GenerateSessionToken();
		info.ProjectPath = std::move(projectPath);
		info.EditorVersion = "0.1.0";
		info.Headless = true;
		info.StartedAt = std::move(startedAt);
		return info;
	}

	bool IsHexDigit(char character)
	{
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
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
		CHECK(keys == std::set<std::string> { "ProcessId", "Port", "Token", "ProjectPath", "EditorVersion", "Headless", "StartedAt" });
		CHECK(json["ProcessId"] == 4242);
		CHECK(json["Port"] == 50123);
		CHECK(json["Headless"] == true);

		const std::optional<EditorSessionInfo> parsed = EditorSessionInfo::FromJson(json);
		REQUIRE(parsed.has_value());
		CHECK(parsed->ProcessId == info.ProcessId);
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

		// Only ProcessId and Port are required; mistyped optional fields fall back to defaults.
		const std::optional<EditorSessionInfo> minimal = EditorSessionInfo::FromJson(nlohmann::json { { "ProcessId", 7 }, { "Port", 80 }, { "Headless", "yes" } });
		REQUIRE(minimal.has_value());
		CHECK(minimal->Token.empty());
		CHECK_FALSE(minimal->Headless);
	}

	TEST_CASE("Session files are written, found and removed")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessions");
		const std::filesystem::path projectDirectory = Tests::CreateTemporaryDirectory("EditorSessionProject");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory / "Nested"));
		REQUIRE(EditorSession::GetSessionDirectory() == sessionDirectory / "Nested");

		const EditorSessionInfo older = MakeSession(1001, 40001, "2026-01-01T10:00:00Z");
		const EditorSessionInfo newer = MakeSession(1002, 40002, "2026-03-01T10:00:00Z", FileSystem::ToUTF8(projectDirectory));
		REQUIRE(EditorSession::WriteSessionFiles(older));
		REQUIRE(EditorSession::WriteSessionFiles(newer));

		CHECK(FileSystem::IsRegularFile(EditorSession::GetSessionFilePath(1001)));
		CHECK(EditorSession::GetSessionFilePath(1001).filename() == "1001.json");
		CHECK(FileSystem::IsRegularFile(projectDirectory / ".strata" / "EditorSession.json"));

		// Unrelated and corrupt files in the directory are ignored.
		REQUIRE(FileSystem::WriteText(sessionDirectory / "Nested" / "notes.txt", "not a session"));
		REQUIRE(FileSystem::WriteText(sessionDirectory / "Nested" / "999.json", "{ corrupt"));

		const std::vector<EditorSessionInfo> sessions = EditorSession::FindSessions();
		REQUIRE(sessions.size() == 2);
		CHECK(sessions[0].ProcessId == 1002); // Newest first
		CHECK(sessions[1].ProcessId == 1001);
		CHECK(sessions[0].Token == newer.Token);

		const std::optional<EditorSessionInfo> projectSession = EditorSession::ReadProjectSession(projectDirectory);
		REQUIRE(projectSession.has_value());
		CHECK(projectSession->ProcessId == 1002);
		CHECK(projectSession->ProjectPath == FileSystem::ToUTF8(projectDirectory));

		EditorSession::RemoveSessionFiles(newer);
		CHECK_FALSE(FileSystem::Exists(EditorSession::GetSessionFilePath(1002)));
		CHECK_FALSE(EditorSession::ReadProjectSession(projectDirectory).has_value());
		REQUIRE(EditorSession::FindSessions().size() == 1);
		CHECK(EditorSession::FindSessions()[0].ProcessId == 1001);

		EditorSession::RemoveSessionFiles(older);
		CHECK(EditorSession::FindSessions().empty());
		CHECK(EditorSession::FindSessions(sessionDirectory / "DoesNotExist").empty());
	}

	TEST_CASE("Removing a session keeps a project file that another editor took over")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("EditorSessionsTakeover");
		const std::filesystem::path projectDirectory = Tests::CreateTemporaryDirectory("EditorSessionTakeoverProject");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		const EditorSessionInfo first = MakeSession(2001, 41001, "2026-01-01T10:00:00Z", FileSystem::ToUTF8(projectDirectory));
		const EditorSessionInfo second = MakeSession(2002, 41002, "2026-01-01T11:00:00Z", FileSystem::ToUTF8(projectDirectory));
		REQUIRE(EditorSession::WriteSessionFiles(first));
		REQUIRE(EditorSession::WriteSessionFiles(second));

		EditorSession::RemoveSessionFiles(first);
		const std::optional<EditorSessionInfo> projectSession = EditorSession::ReadProjectSession(projectDirectory);
		REQUIRE(projectSession.has_value());
		CHECK(projectSession->ProcessId == 2002);
		CHECK(EditorSession::FindSessions().size() == 1);
	}

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
