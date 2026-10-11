#include "Editor/AssetEdit.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Animation/AnimationClip.h>
#include <Strata/Animation/AnimationController.h>
#include <Strata/Animation/AnimationPreview.h>
#include <Strata/Animation/AnimationSystem.h>
#include <Strata/Animation/Skeleton.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		nlohmann::json ControllerSchemaProperties()
		{
			return {
				{ "parameters", { { "type", "array" }, { "description", "Parameters: [{\"Name\", \"Type\": \"Float\" | \"Bool\" | \"Trigger\", \"Default\": number or "
					"bool}]" } } },
				{ "states", { { "type", "array" }, { "description", "States: [{\"Name\", \"Clip\": animation clip handle (animation.info lists a model's clips) "
					"or null for the rest pose, \"Speed\" (default 1, negative plays backwards), \"Loop\" (default true), \"Events\": [{\"Name\", \"Time\": "
					"seconds into the clip}]}]" } } },
				{ "defaultState", StringSchema("The state the animator starts in (default: the first state)") },
				{ "transitions", { { "type", "array" }, { "description", "Transitions, tested after every update in order (from any state first): "
					"[{\"From\": state name, or \"AnyState\": true (with \"CanTransitionToSelf\"), \"To\", \"Conditions\": [{\"Parameter\", \"Mode\": "
					"\"Greater\" | \"Less\" (floats, with \"Threshold\") | \"True\" | \"False\" (bools) | \"Set\" (triggers, consumed when it fires)}], "
					"\"ExitTime\": the source state's normalized time to wait for (null: none), \"Duration\": cross-fade seconds}]" } } } };
		}

		// The content of a controller from command parameters over `base` (the sections the parameters leave out stay).
		std::optional<AnimationControllerData> ReadControllerParameters(const nlohmann::json& parameters, nlohmann::json base, std::string& outError)
		{
			const std::pair<const char*, const char*> names[] = { { "parameters", "Parameters" }, { "states", "States" }, { "defaultState", "DefaultState" },
				{ "transitions", "Transitions" } };
			for (const auto& [parameter, section] : names)
			{
				if (parameters.contains(parameter))
					base[section] = parameters[parameter];
			}
			if (!base.contains("Parameters"))
				base["Parameters"] = nlohmann::json::array();
			if (!base.contains("Transitions"))
				base["Transitions"] = nlohmann::json::array();
			if (!base.contains("DefaultState") && base.contains("States") && base["States"].is_array() && !base["States"].empty()
				&& base["States"][0].is_object() && base["States"][0].contains("Name"))
				base["DefaultState"] = base["States"][0]["Name"];
			return AnimationController::DataFromJson(base, &outError);
		}

		// Every state's clip must be an animation clip the asset manager knows.
		bool CheckStateClips(const AnimationControllerData& data, std::string& outError)
		{
			for (const AnimationState& state : data.States)
			{
				if (state.Clip.IsValid() && AssetManager::GetAssetType(state.Clip) != AssetType::AnimationClip)
				{
					outError = fmt::format("State '{}': {} is not an animation clip (animation.info lists a model's clips)", state.Name, state.Clip.ToString());
					return false;
				}
			}
			return true;
		}

		nlohmann::json DescribeSkeleton(AssetHandle handle, const Skeleton& skeleton)
		{
			nlohmann::json joints = nlohmann::json::array();
			for (uint32_t joint = 0; joint < skeleton.GetJointCount(); joint++)
			{
				const JointIndex parent = skeleton.GetParents()[joint];
				joints.push_back({ { "name", skeleton.GetJointNames()[joint] },
					{ "parent", parent == c_NoJoint ? nlohmann::json(nullptr) : nlohmann::json(skeleton.GetJointNames()[parent]) } });
			}
			return { { "asset", UUIDToJson(handle) }, { "type", "Skeleton" }, { "jointCount", skeleton.GetJointCount() },
				{ "rootMotionJoint", skeleton.GetJointNames()[skeleton.GetRootMotionJoint()] }, { "joints", std::move(joints) } };
		}

		nlohmann::json DescribeClip(AssetHandle handle, const AnimationClip& clip, bool channels)
		{
			nlohmann::json result = { { "asset", UUIDToJson(handle) }, { "type", "AnimationClip" }, { "name", clip.GetName() }, { "duration", clip.GetDuration() },
				{ "channelCount", clip.GetChannels().size() }, { "keyCount", clip.GetKeyCount() }, { "joints", clip.GetTargetNames() } };
			if (channels)
			{
				nlohmann::json list = nlohmann::json::array();
				for (const AnimationChannel& channel : clip.GetChannels())
				{
					list.push_back({ { "joint", clip.GetTargetNames()[channel.Target] }, { "path", AnimationPathToString(channel.Path) },
						{ "interpolation", AnimationInterpolationToString(channel.Interpolation) }, { "keys", channel.KeyCount } });
				}
				result["channels"] = std::move(list);
			}
			return result;
		}

		nlohmann::json DescribeController(AssetHandle handle, const AnimationController& controller)
		{
			nlohmann::json clips = nlohmann::json::array();
			for (const AnimationState& state : controller.GetStates())
			{
				nlohmann::json clip = { { "state", state.Name }, { "clip", state.Clip.IsValid() ? UUIDToJson(state.Clip) : nlohmann::json(nullptr) } };
				if (Ref<AnimationClip> loaded = state.Clip.IsValid() ? AssetManager::LoadAssetSync<AnimationClip>(state.Clip) : nullptr)
				{
					clip["name"] = loaded->GetName();
					clip["duration"] = loaded->GetDuration();
				}
				clips.push_back(std::move(clip));
			}
			return { { "asset", UUIDToJson(handle) }, { "type", "AnimationController" }, { "controller", AnimationController::DataToJson(controller.GetData()) },
				{ "clips", std::move(clips) } };
		}

		// An entity's Animator: its assets and, while the scene plays, what it plays.
		nlohmann::json DescribeAnimator(EditorContext& context, Entity entity)
		{
			const AnimatorComponent& component = entity.GetComponent<AnimatorComponent>();
			auto handleOrNull = [](AssetHandle handle) { return handle.IsValid() ? UUIDToJson(handle) : nlohmann::json(nullptr); };
			nlohmann::json result = { { "entity", UUIDToJson(entity.GetUUID()) }, { "skeleton", handleOrNull(component.Skeleton) },
				{ "controller", handleOrNull(component.Controller) }, { "clip", handleOrNull(component.Clip) }, { "playing", false } };
			AnimationSystem* system = context.GetActiveScene()->GetSystem<AnimationSystem>();
			if (!system || !system->FindInstance(entity))
				return result;
			const std::optional<std::string> state = system->GetCurrentState(entity);
			nlohmann::json parameters = nlohmann::json::object();
			for (const AnimatorParameterValue& parameter : system->GetParameters(entity))
				parameters[parameter.Name] = parameter.Type == AnimationParameterType::Float ? nlohmann::json(parameter.Value) : nlohmann::json(parameter.Value != 0.0f);
			result["playing"] = true;
			result["state"] = state.value_or(std::string());
			result["normalizedTime"] = system->GetNormalizedTime(entity);
			result["inTransition"] = system->IsInTransition(entity);
			result["parameters"] = std::move(parameters);
			return result;
		}

	}

	void RegisterAnimationCommands(EditorCommandRegistry& registry)
	{
		nlohmann::json createProperties = ControllerSchemaProperties();
		createProperties["path"] = StringSchema("Path relative to the asset directory, e.g. \"Animations/Hero.stanimator\"");
		registry.Register({ "animation.createController",
			"Creates an animation controller asset (.stanimator): parameters, states (each plays an animation clip), transitions between them (conditions "
			"on parameters, exit times, cross-fades) and events, which an Animator component plays (its Controller property). Returns the new asset and "
			"its content as animation.info describes it.",
			{ { "type", "object" }, { "properties", std::move(createProperties) }, { "additionalProperties", false }, { "required", { "path", "states" } } },
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorAssetManager* assets = context.GetAssetManager();
				if (!assets)
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				const std::optional<AnimationControllerData> data = ReadControllerParameters(parameters, nlohmann::json::object(), error);
				if (!data)
					return EditorCommandResult::InvalidParameters(fmt::format("Invalid animation controller: {}", error));
				if (!CheckStateClips(*data, error))
					return EditorCommandResult::InvalidParameters(error);
				const std::optional<std::string> document = AssetEdit::BuildControllerDocument(*data, &error);
				if (!document)
					return EditorCommandResult::InvalidParameters(fmt::format("Invalid animation controller: {}", error));
				const AssetHandle handle = assets->CreateNativeAsset(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(document->data()), document->size()), &error);
				if (!handle.IsValid())
					return EditorCommandResult::Fail(error);
				Ref<AnimationController> controller = AssetManager::LoadAssetSync<AnimationController>(handle);
				if (!controller)
					return EditorCommandResult::Fail(fmt::format("Loading the controller failed: {}", assets->GetAssetError(handle)));
				return EditorCommandResult::Ok(DescribeController(handle, *controller));
			} });

		nlohmann::json setProperties = ControllerSchemaProperties();
		setProperties["controller"] = AssetSchema("The animation controller: handle or path");
		registry.Register({ "animation.setController",
			"Replaces parts of an animation controller asset of the project: the given sections (parameters, states, defaultState, transitions; each "
			"replaces the whole list) while the others stay. One undo step; animators and previews use the new controller at once. Returns its content.",
			{ { "type", "object" }, { "properties", std::move(setProperties) }, { "additionalProperties", false }, { "required", { "controller" } } },
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				if (!context.GetAssetManager())
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "controller", AssetType::AnimationController);
				if (!arguments.IsValid())
					return arguments.Fail();
				Ref<AnimationController> current = AssetManager::LoadAssetSync<AnimationController>(handle);
				if (!current)
					return EditorCommandResult::Fail(fmt::format("Loading the controller failed: {}", context.GetAssetManager()->GetAssetError(handle)));
				std::string error;
				const std::optional<AnimationControllerData> data = ReadControllerParameters(parameters, AnimationController::DataToJson(current->GetData()), error);
				if (!data)
					return EditorCommandResult::InvalidParameters(fmt::format("Invalid animation controller: {}", error));
				if (!CheckStateClips(*data, error))
					return EditorCommandResult::InvalidParameters(error);
				EditorCommandError kind = EditorCommandError::Failed;
				const std::string name = fmt::format("Edit {}", context.GetAssetManager()->GetMetadata(handle)->Name);
				if (!AssetEdit::SetControllerData(context, handle, *data, name, {}, &error, &kind))
					return EditorCommandResult::Fail(error, kind);
				Ref<AnimationController> controller = AssetManager::LoadAssetSync<AnimationController>(handle);
				if (!controller)
					return EditorCommandResult::Fail(fmt::format("Loading the controller failed: {}", context.GetAssetManager()->GetAssetError(handle)));
				return EditorCommandResult::Ok(DescribeController(handle, *controller));
			} });

		registry.Register({ "animation.info",
			"Describes animation data. asset: a model (its skeleton and animation clips with names, durations and joints), a skeleton (its joints and "
			"parents), an animation clip (duration and channels) or an animation controller (parameters, states with their clips, transitions). "
			"entity: an entity's Animator (its assets and, while the scene plays, the current state, normalized time, whether it blends and the "
			"parameter values).",
			ObjectSchema({ { "asset", AssetSchema("A model, skeleton, animation clip or animation controller: handle or path") },
				{ "entity", EntitySchema("An entity with an Animator component (instead of asset)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				if (arguments.Has("entity"))
				{
					const Entity entity = arguments.GetEntity(*context.GetActiveScene(), "entity");
					if (!arguments.IsValid())
						return arguments.Fail();
					if (!entity.HasComponent<AnimatorComponent>())
						return EditorCommandResult::InvalidParameters(fmt::format("'{}' has no Animator component", entity.GetName()));
					return EditorCommandResult::Ok(DescribeAnimator(context, entity));
				}
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				if (!arguments.IsValid())
					return arguments.Fail();
				const AssetType type = AssetManager::GetAssetType(handle);
				const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
				switch (type)
				{
					case AssetType::Skeleton:
						if (Ref<Skeleton> skeleton = AssetManager::LoadAssetSync<Skeleton>(handle))
							return EditorCommandResult::Ok(DescribeSkeleton(handle, *skeleton));
						break;
					case AssetType::AnimationClip:
						if (Ref<AnimationClip> clip = AssetManager::LoadAssetSync<AnimationClip>(handle))
							return EditorCommandResult::Ok(DescribeClip(handle, *clip, true));
						break;
					case AssetType::AnimationController:
						if (Ref<AnimationController> controller = AssetManager::LoadAssetSync<AnimationController>(handle))
							return EditorCommandResult::Ok(DescribeController(handle, *controller));
						break;
					case AssetType::Model:
					{
						// The model's own animation data: its sub-assets.
						nlohmann::json skeletons = nlohmann::json::array();
						nlohmann::json clips = nlohmann::json::array();
						for (const AssetMetadata& metadata : manager->GetAllMetadata())
						{
							if (metadata.Parent != handle)
								continue;
							if (metadata.Type == AssetType::Skeleton)
							{
								if (Ref<Skeleton> skeleton = AssetManager::LoadAssetSync<Skeleton>(metadata.Handle))
									skeletons.push_back(DescribeSkeleton(metadata.Handle, *skeleton));
							}
							else if (metadata.Type == AssetType::AnimationClip)
							{
								if (Ref<AnimationClip> clip = AssetManager::LoadAssetSync<AnimationClip>(metadata.Handle))
									clips.push_back(DescribeClip(metadata.Handle, *clip, false));
							}
						}
						return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) }, { "type", "Model" }, { "skeletons", std::move(skeletons) },
							{ "clips", std::move(clips) } });
					}
					default:
						return EditorCommandResult::InvalidParameters(fmt::format("{} is a {}, not a model, skeleton, animation clip or animation controller",
							handle.ToString(), AssetTypeToString(type)));
				}
				return EditorCommandResult::Fail(fmt::format("Loading {} failed: {}", handle.ToString(), manager ? manager->GetAssetError(handle) : "no asset manager"));
			} });

		registry.Register({ "animation.preview",
			"Shows an animator of the edited scene posed while the scene does not play (the viewport and viewport.capture show it), without changing "
			"the scene: a state of its controller, a clip, or what it plays on start, at a time in seconds. Answers once the pose is shown (its assets "
			"loaded). stop: true ends the entity's preview (or every preview without an entity).",
			ObjectSchema({ { "entity", EntitySchema("An entity with an Animator component") },
				{ "state", StringSchema("A state of the animator's controller (default: what it plays on start)") },
				{ "clip", AssetSchema("An animation clip to show instead of a state") },
				{ "time", NumberSchema("Seconds into the state or clip (default 0)") },
				{ "stop", BoolSchema("End the preview instead") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool stop = arguments.GetBool("stop", false);
				if (context.IsPlaying())
					return EditorCommandResult::Fail("The scene plays: previews pose the edited scene (play.stop first)");
				if (stop && !arguments.Has("entity"))
				{
					if (!arguments.IsValid())
						return arguments.Fail();
					if (AnimationPreview* preview = context.FindAnimationPreview())
						preview->ClearAll();
					return EditorCommandResult::Ok({ { "stopped", true } });
				}
				const Entity entity = arguments.GetEntity(*context.GetEditScene(), "entity");
				AnimationPreview::Request request;
				request.State = arguments.GetString("state", std::string());
				if (arguments.Has("clip"))
					request.Clip = ResolveAsset(context, arguments, "clip", AssetType::AnimationClip);
				double time = 0.0;
				if (const auto value = parameters.find("time"); value != parameters.end())
				{
					if (!value->is_number())
						arguments.SetError("Parameter 'time' must be a number");
					else
						time = value->get<double>();
				}
				if (!arguments.IsValid())
					return arguments.Fail();
				if (stop)
				{
					if (AnimationPreview* preview = context.FindAnimationPreview())
						preview->Clear(entity);
					return EditorCommandResult::Ok({ { "entity", UUIDToJson(entity.GetUUID()) }, { "stopped", true } });
				}
				if (!entity.HasComponent<AnimatorComponent>())
					return EditorCommandResult::InvalidParameters(fmt::format("'{}' has no Animator component", entity.GetName()));
				if (!std::isfinite(time) || time < 0.0)
					return EditorCommandResult::InvalidParameters("time must be finite and not negative");
				request.Time = static_cast<float>(time);
				std::string error;
				AnimationPreview& preview = context.GetAnimationPreview();
				if (!preview.Set(entity, request, &error))
					return EditorCommandResult::InvalidParameters(error);
				const UUID id = entity.GetUUID();
				const std::string state = request.State;
				return EditorCommandResult::Defer([id, state, time](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					AnimationPreview* preview = context.FindAnimationPreview();
					const Entity target = context.GetEditScene()->GetEntityByUUID(id);
					if (!preview || !target)
						return EditorCommandResult::Fail("The preview ended (the edited scene changed)");
					std::string reason;
					switch (preview->GetStatus(target, &reason))
					{
						case AnimationPreview::Status::Posed:
							return EditorCommandResult::Ok({ { "entity", UUIDToJson(id) }, { "state", state }, { "time", time }, { "posed", true } });
						case AnimationPreview::Status::Failed:
							return EditorCommandResult::Fail(fmt::format("The preview cannot be shown: {}", reason));
						case AnimationPreview::Status::None:
							return EditorCommandResult::Fail("The preview was stopped");
						case AnimationPreview::Status::Waiting:
							break;
					}
					return std::nullopt;
				});
			} });
	}

}
