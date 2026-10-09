#pragma once

#include "Network/NetworkTestHelpers.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/EditorSession.h"
#include "Strata/Network/RpcServer.h"
#include "Strata/Network/Socket.h"
#include "TestHelpers.h"

#include <filesystem>
#include <string>

namespace Strata::Tests
{

	constexpr const char* c_FakeEditorToken = "fedcba9876543210fedcba9876543210";

	// Methods mimicking the editor's automation API, covering each kind of result the MCP mapping handles.
	inline void RegisterFakeEditorMethods(RpcServer& server)
	{
		RpcMethodInfo create;
		create.Name = "entity.create";
		create.Description = "Creates an entity";
		create.ParamsSchema = nlohmann::json {
			{ "type", "object" },
			{ "properties", { { "Name", { { "type", "string" } } } } },
			{ "required", nlohmann::json::array({ "Name" }) }
		};
		server.RegisterMethod(create, [](const nlohmann::json& params)
		{
			const auto name = params.find("Name");
			if (name == params.end() || !name->is_string())
				return RpcResult::Failure(JsonRpc::ErrorCode::InvalidParams, "Missing 'Name'");
			return RpcResult::Success(nlohmann::json { { "Entity", 42 }, { "Name", *name } });
		});

		RpcMethodInfo fail;
		fail.Name = "scene.fail";
		fail.Description = "Always fails";
		server.RegisterMethod(fail, [](const nlohmann::json&)
		{
			return RpcResult::Failure(JsonRpc::ErrorCode::InvalidParams, "Scene 'x' not found", nlohmann::json { { "Scene", "x" } });
		});

		RpcMethodInfo capture;
		capture.Name = "viewport.capture";
		capture.Description = "Captures the viewport";
		server.RegisterMethod(capture, [](const nlohmann::json&)
		{
			return RpcResult::Success(nlohmann::json {
				{ "Width", 2 },
				{ "Height", 1 },
				{ "Image", { { "MimeType", "image/png" }, { "Data", "iVBORw0KGgo=" } } } });
		});

		RpcMethodInfo add;
		add.Name = "math.add";
		add.Description = "Adds two numbers";
		server.RegisterMethod(add, [](const nlohmann::json& params)
		{
			const auto a = params.find("a");
			const auto b = params.find("b");
			if (a == params.end() || b == params.end() || !a->is_number() || !b->is_number())
				return RpcResult::Failure(JsonRpc::ErrorCode::InvalidParams, "Expected numbers 'a' and 'b'");
			return RpcResult::Success(a->get<double>() + b->get<double>());
		});

		RpcMethodInfo arrayParams;
		arrayParams.Name = "list.sum";
		arrayParams.Description = "Takes positional params";
		arrayParams.ParamsSchema = nlohmann::json { { "type", "array" } };
		server.RegisterMethod(arrayParams, [](const nlohmann::json&) { return RpcResult::Success(0); });
	}

	inline bool StartFakeEditor(PumpedRpcServer& editor, const std::string& token = c_FakeEditorToken)
	{
		RegisterFakeEditorMethods(editor.GetServer());
		RpcServerSpecification specification;
		specification.AuthToken = token;
		return editor.Start(specification);
	}

	inline EditorSessionInfo MakeFakeSession(uint32_t processId, uint16_t port, std::string startedAt, std::string projectPath = {})
	{
		EditorSessionInfo session;
		session.ProcessId = processId;
		session.Port = port;
		session.Token = c_FakeEditorToken;
		session.ProjectPath = std::move(projectPath);
		session.EditorVersion = "0.1.0";
		session.StartedAt = std::move(startedAt);
		return session;
	}

	// Writes a per-user session file the way the editor does (owner-only). Discovery only accepts it while
	// session.ProcessId is a running process (see LiveProcess).
	inline bool WriteFakeSessionFile(const std::filesystem::path& sessionDirectory, const EditorSessionInfo& session)
	{
		return Platform::WritePrivateFile(EditorSession::GetSessionFilePath(sessionDirectory, session.ProcessId), session.ToJson().dump());
	}

}
