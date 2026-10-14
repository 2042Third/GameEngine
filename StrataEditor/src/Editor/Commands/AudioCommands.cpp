#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Audio/AudioSystem.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Scene.h>

#include <optional>
#include <utility>

// The audio.* commands: what a running game sounds like, for tools that cannot hear it.

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		nlohmann::json DescribeClip(const EditorContext& context, AssetHandle clip)
		{
			if (!clip.IsValid())
				return nullptr;
			const EditorAssetManager* assets = context.GetAssetManager();
			const std::optional<AssetMetadata> metadata = assets ? assets->GetMetadata(clip) : std::nullopt;
			return { { "handle", UUIDToJson(clip) }, { "path", metadata ? nlohmann::json(metadata->Path) : nlohmann::json(nullptr) } };
		}

	}

	void RegisterAudioCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "audio.state",
			"What the running game plays (play mode only): voices: [{entity (the source's entity; null for one-shots), clip: {handle, path}, "
			"volume, pitch, playing, paused, waiting (its clip loads), looping, spatial, position, seconds (into the clip)}], oneShots: {playing, "
			"played (since play started), dropped (more than 64 at once, a clip too slow to load or unplayable: each logs a warning)}, listener: "
			"{entity, position} (null without one) and masterVolume. Works without a sound device.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				if (!context.IsPlaying())
					return EditorCommandResult::Fail("audio.state reports a running game: start play mode first (play.start)");
				Scene& scene = *context.GetActiveScene();
				const AudioSystem* audio = scene.GetSystem<AudioSystem>();
				if (!audio)
					return EditorCommandResult::Fail("The running scene has no audio (play.simulate runs physics only)");
				nlohmann::json voices = nlohmann::json::array();
				for (const AudioVoiceInfo& voice : audio->GetVoices())
				{
					voices.push_back({
						{ "entity", voice.Entity.IsValid() ? UUIDToJson(voice.Entity) : nlohmann::json(nullptr) },
						{ "clip", DescribeClip(context, voice.Clip) },
						{ "volume", voice.Volume },
						{ "pitch", voice.Pitch },
						{ "playing", voice.Playing },
						{ "paused", voice.Paused },
						{ "waiting", voice.Waiting },
						{ "looping", voice.Looping },
						{ "spatial", voice.Spatial },
						{ "position", { voice.Position.x, voice.Position.y, voice.Position.z } },
						{ "seconds", voice.Seconds } });
				}
				const AudioSystemStats stats = audio->GetStats();
				nlohmann::json listener = nullptr;
				if (const Entity entity = audio->GetListenerEntity())
				{
					const glm::vec3 position(scene.GetWorldTransform(entity)[3]);
					listener = { { "entity", UUIDToJson(entity.GetUUID()) }, { "position", { position.x, position.y, position.z } } };
				}
				return EditorCommandResult::Ok({
					{ "voices", std::move(voices) },
					{ "oneShots", { { "playing", stats.OneShotCount }, { "played", stats.OneShotsPlayed }, { "dropped", stats.OneShotsDropped } } },
					{ "listener", std::move(listener) },
					{ "masterVolume", AudioSystem::GetMasterVolume() } });
			}, EditorCommandKind::Query, EditorCommandResults::Session });
	}

}
