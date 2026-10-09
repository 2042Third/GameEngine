#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSystem.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class AudioClip;
	class AudioSource;
	class Scene;

	struct AudioSystemStats
	{
		uint32_t SourceCount = 0;        // AudioSourceComponents with an AudioSource (those of active entities)
		uint32_t PlayingSourceCount = 0; // Sources playing, or about to (clip still loading, or the scene paused)
		uint32_t WaitingSourceCount = 0; // Sources whose clip is still loading
		uint32_t OneShotCount = 0;       // One-shots playing or waiting for their clip
	};

	// Plays a running scene's audio: the built-in "Audio" scene system. It runs in Play mode only (like scripts, it is
	// not created in Simulate mode) and updates after scripts and physics, in OnLateUpdate, so it hears the transforms
	// of this frame.
	//
	// Sources: every entity with an AudioSourceComponent that is active in the hierarchy (and not about to be destroyed)
	// owns an AudioSource. It is created when the scene starts, when the component is added or when the entity becomes
	// active, and is stopped and released when the component is removed, the entity is deactivated or destroyed, or the
	// scene stops. A new source plays at once if PlayOnStart is set (so a reactivated entity plays again). Clips are
	// assets loaded asynchronously: a source whose clip is still loading starts playing once it is ready, if PlayOnStart
	// or Play asked for it; a clip that cannot be loaded (unknown, failed, not an audio clip) is reported once. Reloaded
	// clips replace the playing ones.
	//
	// Changes: component values are compared every frame (and before each gameplay call on the entity) with the values
	// last applied, so field writes, ComponentAccess and the inspector all apply without a signal. Changing the clip stops
	// the source (Play starts the new clip). Spatial sources take their position from the entity's world transform every
	// frame and their velocity (for the Doppler effect) from the distance moved since the last frame; moves faster than
	// c_MaxDopplerSpeed count as teleports and leave the velocity at zero.
	//
	// Listener: the first active AudioListenerComponent (Active set, entity active) in hierarchy order, or else the
	// primary camera; without either, the listener stays at the origin facing -Z. Its position, orientation (forward -Z,
	// up +Y of the entity) and velocity follow the entity every frame. The AudioEngine has one listener, so with several
	// running scenes the last one updated places it.
	//
	// Pause: pausing the scene (Scene::SetPaused, e.g. the editor's pause button) pauses its sources and one-shots and
	// resumes them where they were; IsPlaying does not change. Sounds requested while paused start on resume, and steps
	// of a paused scene (Scene::Step) stay silent.
	//
	// Works whether or not the AudioEngine is initialized (sources are then silent and never playing) and with the null
	// device. Main thread only.
	class AudioSystem : public SceneSystem
	{
	public:
		// Faster than this (world units per second), a move counts as a teleport and gives no Doppler shift. Far below the
		// speed of sound, where the Doppler pitch shift becomes infinite.
		static constexpr float c_MaxDopplerSpeed = 150.0f;
		// One-shots playing at once per scene; beyond, the oldest one is cut off.
		static constexpr uint32_t c_MaxOneShots = 64;
		// Longest wait, in seconds of scene time, of a one-shot for its clip to load; a sound that would come later is
		// dropped.
		static constexpr float c_MaxOneShotClipWait = 0.25f;

		explicit AudioSystem(Scene& scene);
		~AudioSystem() override;

		AudioSystem(const AudioSystem&) = delete;
		AudioSystem& operator=(const AudioSystem&) = delete;

		void OnRuntimeStart() override;
		void OnRuntimeStop() override;
		void OnLateUpdate(Timestep timestep) override;
		void OnPausedChanged(bool paused) override;
		void OnEntityDestroying(const Entity& entity) override;

		//////////////////////////////////////////////////////////////////////////
		// Gameplay API (entities with an AudioSourceComponent)
		//////////////////////////////////////////////////////////////////////////

		// Starts or resumes the entity's source (see AudioSource::Play). A clip that is still loading plays once it is
		// ready; while the scene is paused, the source starts when it resumes. Returns false without an active
		// AudioSourceComponent, without a playable clip, or without audio output (AudioEngine not initialized).
		bool Play(Entity entity);
		// Halts playback and keeps the position (Play resumes). Returns false without an active AudioSourceComponent.
		bool Pause(Entity entity);
		// Halts playback and rewinds (also cancels a start that waits for the clip). Returns false without an active
		// AudioSourceComponent.
		bool Stop(Entity entity);
		// Playing, or about to play (clip still loading, scene paused).
		bool IsPlaying(Entity entity);
		// Moves the playback position (see AudioSource::Seek); before the clip has loaded, the position applies once it
		// has. Returns false without an active AudioSourceComponent or for a non-finite position.
		bool Seek(Entity entity, float seconds);
		// Seconds into the clip (0 without a source).
		float GetPlaybackPosition(Entity entity);

		// Fire-and-forget sounds owned by the scene: they pause with it and stop when it stops. PlayOneShot plays the clip
		// without spatialization (UI, stingers), PlayOneShotAt at a world position. A clip that is still loading plays once
		// it is ready, unless that takes longer than c_MaxOneShotClipWait. Returns false for an unknown or unusable clip,
		// invalid parameters (volume >= 0 and pitch > 0, finite), without audio output or while the scene is not running.
		bool PlayOneShot(AssetHandle clip, float volume = 1.0f, float pitch = 1.0f);
		bool PlayOneShotAt(AssetHandle clip, const glm::vec3& position, float volume = 1.0f, float pitch = 1.0f);

		// The engine-wide master volume (AudioEngine::SetMasterVolume): it outlives the scene, like a game's sound option.
		static void SetMasterVolume(float volume);
		static float GetMasterVolume();

		//////////////////////////////////////////////////////////////////////////
		// Inspection (tools, tests)
		//////////////////////////////////////////////////////////////////////////

		// The entity the listener followed at the last update (an AudioListener or the primary camera); invalid if none.
		Entity GetListenerEntity() const { return m_ListenerEntity; }
		// The AudioSource playing an entity's AudioSourceComponent; nullptr if it has none.
		const AudioSource* GetAudioSource(Entity entity) const;
		AudioSystemStats GetStats() const;
	private:
		enum class ClipState : uint8_t
		{
			NoClip = 0,  // No clip handle
			Waiting,     // The clip is loading
			Ready,       // The source holds the clip
			Unavailable  // The clip cannot be loaded (reported once); retried when the asset changes
		};

		struct SourceRecord
		{
			Scope<AudioSource> Source;
			AudioSourceComponent Applied;        // Component values last applied to the source
			ClipState State = ClipState::NoClip;
			bool PlayRequested = false;          // Start once the clip is ready and the scene runs
			bool PausedByScene = false;          // Was playing when the scene paused; resumes with it
			std::optional<float> PendingSeek;    // Seek requested before the clip was ready
			glm::vec3 LastPosition = glm::vec3(0.0f);
			bool HasLastPosition = false;
			uint64_t SeenUpdate = 0;             // Last update that found the component (others are released)
		};

		struct OneShot
		{
			Scope<AudioSource> Source;
			AssetHandle Clip = UUID::Null();
			bool Started = false;       // The clip was set and playback started (or held by the paused scene)
			bool PausedByScene = false; // Was playing when the scene paused; resumes with it
			float WaitedTime = 0.0f;    // Scene time spent waiting for the clip
		};

		bool IsAudible(Entity entity) const;
		// The record of an entity's active source (created if needed, with the component's current values applied), or
		// nullptr without an active AudioSourceComponent.
		SourceRecord* PrepareSource(Entity entity);
		SourceRecord& CreateSource(Entity entity, const AudioSourceComponent& component);
		void ApplyComponent(SourceRecord& record, const AudioSourceComponent& component);
		void ResolveClip(SourceRecord& record, Entity entity, const AudioSourceComponent& component);
		void RefreshClip(SourceRecord& record, Entity entity, const AudioSourceComponent& component);
		void StartIfRequested(SourceRecord& record);
		void UpdateSourcePosition(SourceRecord& record, Entity entity, float timestep);
		void UpdateSources(float timestep);
		void ProcessAssetChanges();
		void UpdateListener(float timestep);
		void UpdateOneShots(float timestep);
		bool StartOneShot(AssetHandle clip, bool spatial, const glm::vec3& position, float volume, float pitch);
		// Starts a waiting one-shot whose clip is ready; returns false if it should be dropped.
		bool TryStartOneShot(OneShot& oneShot);
	private:
		Scene& m_Scene;
		bool m_Stopped = false;
		bool m_Paused = false;
		uint64_t m_UpdateIndex = 0;
		std::unordered_map<entt::entity, SourceRecord> m_Sources;
		std::vector<OneShot> m_OneShots; // Oldest first

		// Asset changes (loads, reloads) of the active asset manager seen so far.
		std::weak_ptr<AssetManagerBase> m_AssetManager;
		uint64_t m_SeenContentVersion = 0;
		std::vector<AssetHandle> m_ChangedAssets; // Scratch

		Entity m_ListenerEntity;
		glm::vec3 m_ListenerPosition = glm::vec3(0.0f);
		std::vector<Entity> m_ListenerCandidates; // Scratch
	};

}
