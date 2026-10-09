#pragma once

#include "StrataScript/Entity.h"
#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>

namespace Strata
{

	// Playback of an entity's AudioSource component while the scene plays. The clip, volume, pitch, looping and 3D
	// settings are properties of the "AudioSource" component (Entity::GetProperty/SetProperty; they apply at once).
	// Calls fail on entities without an AudioSource component, on inactive entities and while the scene plays without
	// audio (the engine logs why).
	class AudioSource
	{
	public:
		explicit AudioSource(Entity entity)
			: m_Entity(entity)
		{
		}

		Entity GetEntity() const { return m_Entity; }

		// Starts the clip, or resumes it where Pause left it. A clip that is still loading starts once it is ready.
		bool Play()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioPlay);
			return host && Call(host->AudioPlay);
		}

		// Halts playback and keeps the position.
		bool Pause()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioPause);
			return host && Call(host->AudioPause);
		}

		// Halts playback and rewinds.
		bool Stop()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioStop);
			return host && Call(host->AudioStop);
		}

		// Playing, or about to play (the clip is still loading, or the scene is paused).
		bool IsPlaying() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioIsPlaying);
			return host && Call(host->AudioIsPlaying);
		}

		// Moves the playback position, in seconds from the start of the clip.
		bool Seek(float seconds)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioSeek);
			return host && m_Entity.GetID() != 0 && host->AudioSeek(Detail::GetContext(), m_Entity.GetID(), seconds);
		}

		// Seconds into the clip (0 when the source cannot be reached).
		float GetPlaybackPosition() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioGetPlaybackPosition);
			return host && m_Entity.GetID() != 0 ? host->AudioGetPlaybackPosition(Detail::GetContext(), m_Entity.GetID()) : 0.0f;
		}
	private:
		using SourceFunction = bool (*)(StrataScriptContext*, StrataScriptEntityID);

		bool Call(SourceFunction function) const
		{
			return m_Entity.GetID() != 0 && function(Detail::GetContext(), m_Entity.GetID());
		}
	private:
		Entity m_Entity;
	};

	// Sounds of the scene that belong to no entity, and the game's master volume.
	class Audio
	{
	public:
		// Plays a clip once, without spatialization (interface sounds, music stingers). The sound pauses and stops with the
		// scene. A clip that takes longer than a quarter second to load is dropped: request clips early
		// (Assets::RequestLoad). volume >= 0, pitch > 0 (1 is the clip's own pitch).
		static bool PlayOneShot(AssetHandle clip, float volume = 1.0f, float pitch = 1.0f)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioPlayOneShot);
			return host && host->AudioPlayOneShot(Detail::GetContext(), clip.ID, volume, pitch);
		}

		// Plays a clip once at a world position (3D sound heard from the listener).
		static bool PlayOneShotAt(AssetHandle clip, const glm::vec3& position, float volume = 1.0f, float pitch = 1.0f)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioPlayOneShotAt);
			if (!host)
				return false;
			float abiPosition[3];
			Detail::ToABIVector3(position, abiPosition);
			return host->AudioPlayOneShotAt(Detail::GetContext(), clip.ID, abiPosition, volume, pitch);
		}

		// The volume of everything the game plays (1 = unchanged). It outlives the scene, like a game's sound option; the
		// editor restores its own when play mode stops.
		static void SetMasterVolume(float volume)
		{
			if (const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioSetMasterVolume))
				host->AudioSetMasterVolume(Detail::GetContext(), volume);
		}

		static float GetMasterVolume()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AudioGetMasterVolume);
			return host ? host->AudioGetMasterVolume(Detail::GetContext()) : 1.0f;
		}
	};

	inline AudioSource Entity::GetAudioSource() const
	{
		return AudioSource(*this);
	}

}
