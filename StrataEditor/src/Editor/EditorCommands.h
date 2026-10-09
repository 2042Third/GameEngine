#pragma once

#include <Strata/Core/UUID.h>
#include <Strata/Scene/Entity.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Scene;

	struct EditorCommandResult;

	// Polled once per frame, on the main thread, while a command is pending: returns the final result once the command
	// has finished (which may itself be pending again), nullopt while it is still running.
	using EditorCommandPoll = std::function<std::optional<EditorCommandResult>(EditorContext& context)>;

	// Why a command failed. Callers that report failures to tools (automation) map these to error codes, so a client can
	// tell a mistake in its request from a command that could not be carried out.
	enum class EditorCommandError : uint8_t
	{
		None = 0,
		UnknownCommand,    // No command has this name
		InvalidParameters, // A parameter is unknown, missing, mistyped, out of range or refers to nothing that exists
		Failed,            // The request was valid, but the command could not be carried out (e.g. nothing to undo)
		Cancelled,         // A pending command was abandoned before it finished (e.g. the editor is closing)
		Internal           // An unexpected failure inside the command (e.g. an exception from third-party code)
	};

	const char* EditorCommandErrorToString(EditorCommandError error);

	struct EditorCommandResult
	{
		bool Success = true;
		nlohmann::json Value; // Result data (null when the command returns nothing)
		std::string Error;
		EditorCommandError ErrorKind = EditorCommandError::None; // Set for failures
		// Set for commands that finish over the next frames (waiting for frames, a build or a GPU readback) instead of
		// blocking the frame. Success, Value and Error are meaningless while it is set. Run commands that may defer through
		// EditorCommandRunner, which polls them.
		EditorCommandPoll Pending;

		bool IsPending() const { return static_cast<bool>(Pending); }

		static EditorCommandResult Ok(nlohmann::json value = nullptr) { return { true, std::move(value), {}, EditorCommandError::None, {} }; }
		static EditorCommandResult Fail(std::string error, EditorCommandError kind = EditorCommandError::Failed) { return { false, nullptr, std::move(error), kind, {} }; }
		// A request the command cannot accept: report the parameter and what is wrong with it.
		static EditorCommandResult InvalidParameters(std::string error) { return Fail(std::move(error), EditorCommandError::InvalidParameters); }
		// The poll function must own everything it uses: copy the parameters, never capture them by reference.
		static EditorCommandResult Defer(EditorCommandPoll poll) { return { true, nullptr, {}, EditorCommandError::None, std::move(poll) }; }
	};

	using EditorCommandHandler = std::function<EditorCommandResult(EditorContext& context, const nlohmann::json& parameters)>;

	// A named editor operation taking a JSON object of parameters. Commands are the single way to change the editor
	// state from outside the UI: the automation server exposes every registered command to tools and AI agents, and
	// tests drive the editor through them. Mutating commands record undo steps like the equivalent UI actions.
	struct EditorCommand
	{
		std::string Name;          // "<group>.<action>", e.g. "entity.create"
		std::string Description;   // One or two sentences for tool listings
		nlohmann::json Parameters; // JSON Schema of the parameter object
		EditorCommandHandler Handler;
	};

	class EditorCommandRegistry
	{
	public:
		// Registers the built-in commands.
		EditorCommandRegistry();

		// Commands may refer to the registry (editor.commands lists it): it stays where it was created.
		EditorCommandRegistry(const EditorCommandRegistry&) = delete;
		EditorCommandRegistry& operator=(const EditorCommandRegistry&) = delete;

		// Adds a command, replacing one with the same name.
		void Register(EditorCommand command);
		const EditorCommand* Find(std::string_view name) const;
		std::vector<const EditorCommand*> GetAll() const; // Sorted by name
		// Changes whenever a command is registered, so users that mirror the commands (automation) notice additions.
		uint64_t GetRevision() const { return m_Revision; }

		// Runs a command. Unknown commands, parameters that are not an object and failures inside the handler
		// (including exceptions from third-party code) become error results. The result may be pending (see
		// EditorCommandResult::Pending); EditorCommandRunner handles that.
		EditorCommandResult Execute(EditorContext& context, std::string_view name, const nlohmann::json& parameters = nlohmann::json::object()) const;
	private:
		std::map<std::string, EditorCommand, std::less<>> m_Commands;
		uint64_t m_Revision = 0;
	};

	// Reads command parameters. Getters record the first problem (missing or mistyped parameter) and return a
	// fallback; check IsValid after reading all of them and return Fail() (an InvalidParameters error) if not.
	class CommandArguments
	{
	public:
		explicit CommandArguments(const nlohmann::json& parameters);

		bool Has(std::string_view name) const;
		bool IsValid() const { return m_Error.empty(); }
		EditorCommandResult Fail() const { return EditorCommandResult::InvalidParameters(m_Error); }
		void SetError(std::string error);

		std::string GetString(std::string_view name);
		std::string GetString(std::string_view name, std::string fallback);
		bool GetBool(std::string_view name, bool fallback);
		int64_t GetInt(std::string_view name, int64_t fallback, int64_t min, int64_t max);
		const nlohmann::json& GetObject(std::string_view name);   // Required
		const nlohmann::json* FindObject(std::string_view name);  // Optional; null when absent
		UUID GetUUID(std::string_view name);
		// An entity of the scene by UUID (required parameter).
		Entity GetEntity(Scene& scene, std::string_view name);
		// An optional entity reference: absent or null gives an invalid entity; an unknown UUID is an error.
		Entity FindEntity(Scene& scene, std::string_view name);
		std::vector<Entity> GetEntities(Scene& scene, std::string_view name); // Array of UUIDs, at least one
	private:
		const nlohmann::json* Get(std::string_view name);
		void TypeError(std::string_view name, std::string_view expected);
	private:
		const nlohmann::json& m_Parameters;
		std::string m_Error;
	};

	// Built-in command groups (EditorCommands*.cpp).
	void RegisterSceneCommands(EditorCommandRegistry& registry);
	void RegisterAssetCommands(EditorCommandRegistry& registry);
	void RegisterEditorStateCommands(EditorCommandRegistry& registry);
	void RegisterViewportCommands(EditorCommandRegistry& registry);
	void RegisterScriptCommands(EditorCommandRegistry& registry);

}
