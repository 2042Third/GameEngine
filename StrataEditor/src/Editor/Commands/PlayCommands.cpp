#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Core/Log.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

// The play.* commands (moved out of EditorStateCommands.cpp).

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// The most steps play.step runs in one call: about half an hour of a 60 Hz game.
		constexpr int64_t c_MaxSteps = 100'000;

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

		registry.Register({ "play.step",
			"While paused, runs fixed steps of the game, one per frame (frames run unthrottled meanwhile, so 600 steps take the frames' work, not "
			"ten seconds), and answers once its last step ran: {state, paused, stepsRun, frameIndex, time, fixedSteps (run since play started)}. "
			"Results are exact: the same steps give the same state whatever the frame rate. A play.step while another waits adds its steps after "
			"them and answers when its own last step ran. If the game is resumed (play.pause {paused: false} drops steps not run yet), stopped, "
			"crashes or quits first, it fails with stepsRun so far. wait: false answers at once with pendingSteps (the steps run over the next "
			"frames).",
			ObjectSchema({
				{ "steps", IntegerSchema("Fixed steps to run (default 1)", 1, c_MaxSteps) },
				{ "frames", IntegerSchema("Same as steps (an older name: each step runs in a frame of its own)", 1, c_MaxSteps) },
				{ "wait", BoolSchema("Answer once the steps ran (default true); false answers at once") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				if (arguments.Has("steps") && arguments.Has("frames"))
					arguments.SetError("Pass either 'steps' or 'frames' (an older name of steps), not both");
				const int64_t steps = arguments.Has("frames") ? arguments.GetInt("frames", 1, 1, c_MaxSteps) : arguments.GetInt("steps", 1, 1, c_MaxSteps);
				const bool wait = arguments.GetBool("wait", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.IsPaused())
					return EditorCommandResult::Fail("Stepping needs a paused running scene (play.pause)");
				// This request's last step runs after the steps already pending.
				const uint64_t first = context.GetFixedStepCount() + context.GetPendingSteps();
				const uint64_t target = first + static_cast<uint64_t>(steps);
				context.Step(static_cast<uint32_t>(steps));
				if (!wait)
				{
					nlohmann::json value = DescribePlayState(context);
					value["pendingSteps"] = context.GetPendingSteps();
					return EditorCommandResult::Ok(std::move(value));
				}
				return EditorCommandResult::Defer([first, target, steps, session = context.GetPlaySession(), lastSeen = context.GetFixedStepCount()](
					EditorContext& context) mutable -> std::optional<EditorCommandResult>
				{
					const auto ran = [&first, &steps](uint64_t count) { return std::clamp<int64_t>(static_cast<int64_t>(count) - static_cast<int64_t>(first), 0, steps); };
					if (!context.IsPlaying() || context.GetPlaySession() != session)
					{
						const uint64_t atStop = context.GetFixedStepCountAtStop(session).value_or(lastSeen);
						return EditorCommandResult::Fail(fmt::format("Play mode stopped (play.stop, or the game quit or crashed) after {} of {} steps", ran(atStop),
							steps), { { "stepsRun", ran(atStop) } });
					}
					// Resumed: the steps that ran before the resume count; the rest were dropped with it.
					const bool resumed = !context.IsPaused();
					lastSeen = resumed ? context.GetFixedStepCountAtResume() : context.GetFixedStepCount();
					if (lastSeen >= target)
					{
						const Scene& scene = *context.GetActiveScene();
						nlohmann::json value = DescribePlayState(context);
						value["stepsRun"] = steps;
						value["frameIndex"] = scene.GetFrameIndex();
						value["time"] = scene.GetTime();
						value["fixedSteps"] = lastSeen;
						return EditorCommandResult::Ok(std::move(value));
					}
					if (resumed)
					{
						return EditorCommandResult::Fail(fmt::format("Play mode was resumed after {} of {} steps; the rest were dropped", ran(lastSeen), steps),
							{ { "stepsRun", ran(lastSeen) } });
					}
					return std::nullopt;
				});
			}, EditorCommandKind::Session });

		registry.Register({ "play.state", "Whether the scene is being edited, played or simulated, and whether it is paused.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Query, EditorCommandResults::Session });
	}

}
