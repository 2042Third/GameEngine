#include "Editor/InputSequence.h"

#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ScriptEdit.h"

#include <Strata/Input/Input.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scripting/ScriptEngine.h>
#include <Strata/Scripting/ScriptSystem.h>

#include <utility>

namespace Strata
{

	InputSequence::InputSequence(std::vector<Event> events, uint32_t steps, std::vector<Observation> observations, bool keepHeld,
		const EditorCommandRegistry& commands)
		: m_Events(std::move(events)), m_Observations(std::move(observations)), m_KeepHeld(keepHeld), m_Commands(commands)
	{
		m_Result.Steps = steps;
	}

	bool InputSequence::Start(EditorContext& context, std::string* outError)
	{
		m_WasRunning = !context.IsPaused();
		m_Session = context.GetPlaySession();
		if (m_WasRunning)
			context.SetPaused(true);
		if (!ApplyEvents(context, outError))
		{
			Fail(context, outError ? *outError : std::string("an event failed"));
			return false;
		}
		m_ExpectedSteps = context.GetFixedStepCount() + 1;
		context.Step(1);
		return true;
	}

	bool InputSequence::ApplyEvents(EditorContext& context, std::string* outError)
	{
		SimulatedInput& input = context.GetSimulatedInput();
		const uint64_t fixedSteps = context.GetFixedStepCount();
		for (; m_NextEvent < m_Events.size() && m_Events[m_NextEvent].Step == m_Result.StepsRun; m_NextEvent++)
		{
			const Event& event = m_Events[m_NextEvent];
			switch (event.Type)
			{
				case EventType::Button:
					if (event.Action == ButtonAction::Release)
						input.Release(event.Control, event.Code);
					else
						m_Holds.push_back(input.Press(event.Control, event.Code, event.Action == ButtonAction::Tap ? event.TapSteps : 0, fixedSteps));
					break;
				case EventType::Axis:
					if (event.AxisValue)
						m_Holds.push_back(input.HoldAxis(event.Gamepad, event.Axis, *event.AxisValue, 0, fixedSteps));
					else
						input.Release(SimulatedControlType::GamepadAxis, SimulatedInput::MakeGamepadCode(event.Gamepad, static_cast<uint32_t>(event.Axis)));
					break;
				case EventType::MouseMove:
					Input::SimulateMouseMove(event.Vector);
					break;
				case EventType::Scroll:
					Input::SimulateScroll(event.Vector);
					break;
				case EventType::Command:
				{
					const EditorCommandResult result = m_Commands.Execute(context, event.Command, event.Parameters);
					if (!result.Success || result.IsPending())
					{
						if (outError)
						{
							*outError = result.IsPending() ? fmt::format("Event {} ({}) did not answer at once", event.Index, event.Command)
								: fmt::format("Event {} ({}) failed: {}", event.Index, event.Command, result.Error);
						}
						return false;
					}
					break;
				}
			}
			m_Result.Seen.push_back({ { "step", event.Step }, { "event", event.Index } });
		}
		return true;
	}

	void InputSequence::Update(EditorContext& context)
	{
		if (m_Result.Finished)
			return;
		if (!context.IsPlaying() || context.GetPlaySession() != m_Session)
		{
			Fail(context, fmt::format("Play mode stopped after {} of {} steps of the input sequence", m_Result.StepsRun, m_Result.Steps));
			return;
		}
		if (!context.IsPaused())
		{
			Fail(context, fmt::format("The game was resumed after {} of {} steps of the input sequence; the rest did not run", m_Result.StepsRun,
				m_Result.Steps));
			return;
		}
		// The step given its events has not run yet (a frame without a game update).
		if (context.GetFixedStepCount() < m_ExpectedSteps)
			return;
		m_Result.StepsRun++;
		if (m_Result.StepsRun == m_Result.Steps)
		{
			Finish(context);
			return;
		}
		Sample(context, false);
		std::string error;
		if (!ApplyEvents(context, &error))
		{
			Fail(context, std::move(error));
			return;
		}
		m_ExpectedSteps = context.GetFixedStepCount() + 1;
		context.Step(1);
	}

	void InputSequence::Sample(EditorContext& context, bool final)
	{
		nlohmann::json values = nlohmann::json::array();
		for (size_t index = 0; index < m_Observations.size(); index++)
		{
			const Observation& observation = m_Observations[index];
			if (final || (observation.Every > 0 && m_Result.StepsRun % observation.Every == 0))
				values.push_back({ { "observe", index }, { "value", Observe(context, observation) } });
		}
		if (!values.empty())
			m_Result.Samples.push_back({ { "step", m_Result.StepsRun }, { "values", std::move(values) } });
	}

	nlohmann::json InputSequence::Observe(EditorContext& context, const Observation& observation) const
	{
		Scene& scene = *context.GetActiveScene();
		const Entity entity = scene.GetEntityByUUID(observation.Entity);
		if (!entity)
			return nullptr;
		switch (observation.Type)
		{
			case ObservationType::Component:
			{
				const void* component = observation.Info->Get(scene.GetRegistry(), entity.GetHandle());
				if (!component)
					return nullptr;
				nlohmann::json values = ComponentAccess::Serialize(*observation.Info, component);
				if (observation.Property.empty())
					return values;
				const auto value = values.find(observation.Property);
				return value != values.end() ? *value : nlohmann::json(nullptr);
			}
			case ObservationType::ScriptField:
			{
				const ScriptSystem* system = scene.GetSystem<ScriptSystem>();
				const Ref<ScriptEngine>& engine = context.GetScriptEngine();
				const ScriptClassInfo* info = engine && engine->IsModuleLoaded() ? engine->FindClass(observation.ClassName) : nullptr;
				const ScriptFieldInfo* field = info ? info->FindField(observation.Field) : nullptr;
				const std::optional<PropertyValue> value = system && field ? system->GetFieldValue(entity, observation.ClassName, observation.Field)
					: std::nullopt;
				return value ? ScriptEdit::FieldValueToJson(*value, field->Type) : nlohmann::json(nullptr);
			}
			case ObservationType::World:
				return CommandUtils::DescribeWorldTransform(scene.GetWorldTransform(entity));
		}
		return nullptr;
	}

	void InputSequence::Finish(EditorContext& context)
	{
		Sample(context, true);
		for (const Ref<const SimulatedHold>& hold : m_Holds)
			m_Result.Interrupted = m_Result.Interrupted || (hold->Ended && hold->Interrupted);
		if (!m_KeepHeld)
		{
			for (const Ref<const SimulatedHold>& hold : m_Holds)
				context.GetSimulatedInput().EndHold(hold);
		}
		const Scene& scene = *context.GetActiveScene();
		m_Result.FrameIndex = scene.GetFrameIndex();
		m_Result.Time = scene.GetTime();
		m_Result.Finished = true;
		if (m_WasRunning)
			context.SetPaused(false);
	}

	void InputSequence::Fail(EditorContext& context, std::string error)
	{
		if (m_Result.Finished)
			return;
		m_Result.Finished = true;
		m_Result.Failed = true;
		m_Result.Error = std::move(error);
		if (context.IsPlaying() && context.GetPlaySession() == m_Session)
		{
			for (const Ref<const SimulatedHold>& hold : m_Holds)
				context.GetSimulatedInput().EndHold(hold);
			// A game that ran before the sequence paused it runs again, as after a finished sequence.
			if (m_WasRunning && context.IsPaused())
				context.SetPaused(false);
		}
	}

}
