#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Core/Log.h>

#include <optional>
#include <string>
#include <utility>

// The play.* commands (moved out of EditorStateCommands.cpp).

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// Where play.start is while the scripts build first.
		struct PlayBuildState
		{
			uint64_t WaitingFor = 0;   // The build that has to finish before anything else happens (0: none)
			uint64_t OwnBuild = 0;     // The build play.start started itself (it starts at most one)
			std::string Reason;        // Why it built
		};

		// Plays once the build play.start waited for finished: a failed build (or a module that cannot load) fails play.start
		// with its first diagnostics and asks the UI for the Problems panel; a cancelled one only ends play.start (Cancelled).
		std::optional<EditorCommandResult> FinishPlayBuild(EditorContext& context, PlayBuildState& state)
		{
			const ScriptBuildResult& result = context.GetScriptBuilder().GetLastResult();
			const ScriptBuildLoad& load = context.GetLastScriptBuildLoad();
			if (context.GetPlayBuildID() == state.OwnBuild)
				context.ClearPlayBuild();
			if (result.ID != state.WaitingFor)
				return EditorCommandResult::Fail(fmt::format("The result of script build {} is no longer available (see script.status)", state.WaitingFor));
			if (result.Cancelled)
				return EditorCommandResult::Fail("The script build play.start waited for was cancelled, so the game does not play", EditorCommandError::Cancelled);
			// The build's own notification told the person how it ended (EditorContext::OnScriptBuildFinished): no second toast.
			if (!result.Success)
			{
				context.RequestProblemsPanel();
				EditorCommandResult failed = EditorCommandResult::Fail(fmt::format("The scripts must be built before playing, and the build failed: {}", result.Error));
				failed.Announced = true;
				return failed;
			}
			if (load.BuildID == result.ID && !load.Loaded)
			{
				context.RequestProblemsPanel();
				EditorCommandResult failed = EditorCommandResult::Fail(fmt::format("The scripts were built, but their module could not be loaded: {}", load.Error));
				failed.Announced = true;
				return failed;
			}

			// A build someone else started may have left the scripts in need of another (sources changed meanwhile): play.start
			// builds once itself.
			if (state.OwnBuild == 0)
			{
				if (std::optional<std::string> reason = context.GetPlayBuildReason())
				{
					std::string error;
					if (!context.StartPlayBuild(&error))
						return EditorCommandResult::Fail(error);
					state.Reason = std::move(*reason);
					state.OwnBuild = state.WaitingFor = context.GetPlayBuildID();
					ST_INFO("Building the scripts before playing: {}", state.Reason);
					return std::nullopt;
				}
			}

			std::string error;
			if (!context.Play(&error))
				return EditorCommandResult::Fail(error);
			nlohmann::json value = DescribePlayState(context);
			if (state.OwnBuild != 0)
				value["built"] = { { "build", state.OwnBuild }, { "seconds", result.Seconds }, { "reason", state.Reason } };
			return EditorCommandResult::Ok(std::move(value));
		}

		// play.start when the scripts need a build (or one runs): builds them (or waits for the running build), then plays.
		EditorCommandResult StartPlayAfterBuild(EditorContext& context)
		{
			Ref<PlayBuildState> state = CreateRef<PlayBuildState>();
			const ScriptBuilder& builder = context.GetScriptBuilder();
			if (builder.IsRunning())
			{
				state->WaitingFor = builder.GetCurrentID();
			}
			else
			{
				state->Reason = context.GetPlayBuildReason().value_or(std::string());
				std::string error;
				if (!context.StartPlayBuild(&error))
					return EditorCommandResult::Fail(fmt::format("The scripts must be built before playing ({}), and the build could not start: {}",
						state->Reason, error));
				state->OwnBuild = state->WaitingFor = context.GetPlayBuildID();
				ST_INFO("Building the scripts before playing: {}", state->Reason);
			}
			return EditorCommandResult::Defer([state](EditorContext& pollContext) -> std::optional<EditorCommandResult>
			{
				const ScriptBuilder& pollBuilder = pollContext.GetScriptBuilder();
				if (pollBuilder.IsRunning() && pollBuilder.GetCurrentID() == state->WaitingFor)
					return std::nullopt;
				return FinishPlayBuild(pollContext, *state);
			});
		}

	}

	void RegisterPlayCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "play.start",
			"Runs a copy of the scene with scripts, physics and audio. Changes made while playing are discarded by play.stop. When the project's "
			"scripts need building first (their sources changed since the module was built, or scenes use scripts that are not built or "
			"loaded), it builds them, then plays the new code: the answer comes once the build finished (a failed build fails play.start with "
			"its first errors; script.status lists every diagnostic). A build already running is waited for. While a scene is being opened "
			"(scene.open) it fails at once, building nothing. Returns the play state and, when it built, built: {build, seconds, reason}.",
			ObjectSchema({ { "build", BoolSchema("Build the scripts first when they need it (default true); false plays what is loaded") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool build = arguments.GetBool("build", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				// A running scene, and a scene being opened, refuse (Play says why) before anything is built: a build would take
				// its time only for the play to be refused after it.
				if (build && !context.IsPlaying() && !context.IsSceneLoading()
					&& (context.GetScriptBuilder().IsRunning() || context.GetPlayBuildReason()))
					return StartPlayAfterBuild(context);
				std::string error;
				if (!context.Play(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Session });

		registry.Register({ "play.simulate", "Runs a copy of the scene with physics only (no scripts or audio).", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				if (!context.Simulate(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Session });

		registry.Register({ "play.stop", "Stops play or simulate mode and returns to the edited scene.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				context.Stop();
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Session });

		registry.Register({ "play.pause", "Pauses or resumes the running scene.",
			ObjectSchema({ { "paused", BoolSchema("Whether the simulation is paused") } }, { "paused" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				if (!arguments.Has("paused"))
					arguments.SetError("Missing parameter 'paused'");
				const bool paused = arguments.GetBool("paused", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.IsPlaying())
					return EditorCommandResult::Fail("The scene is not running");
				context.SetPaused(paused);
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Session });

		registry.Register({ "play.step", "While paused, advances the simulation by a number of fixed steps over the next frames.",
			ObjectSchema({ { "frames", IntegerSchema("Fixed steps (default 1)", 1, 10000) } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t frames = arguments.GetInt("frames", 1, 1, 10000);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.IsPaused())
					return EditorCommandResult::Fail("Stepping needs a paused running scene (play.pause)");
				context.Step(static_cast<uint32_t>(frames));
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Session });

		registry.Register({ "play.state", "Whether the scene is being edited, played or simulated, and whether it is paused.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Query, EditorCommandResults::Session });
	}

}
