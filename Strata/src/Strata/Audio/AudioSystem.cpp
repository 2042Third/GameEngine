#include "stpch.h"
#include "Strata/Audio/AudioSystem.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Audio/AudioClip.h"
#include "Strata/Audio/AudioClipAsset.h"
#include "Strata/Audio/AudioEngine.h"
#include "Strata/Audio/AudioSource.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Strata
{

	namespace
	{

		constexpr float c_MinDirectionLengthSquared = 1e-12f;
		// A listener whose right axis rises or falls less than this (the sine of its roll) counts as level.
		constexpr float c_MaxLevelRoll = 1e-3f;
		// Looking up or down more steeply than this (the sine of the pitch), forward x +Y no longer gives a usable right axis.
		constexpr float c_MaxLevelPitch = 0.99f;

		// Bitwise comparison, so that a value the source rejects (NaN) is applied, and reported, once instead of every frame.
		bool IsSameFloat(float a, float b)
		{
			return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// Whether a weak and a shared pointer refer to the same object (also when the weak one expired since: a new object
		// at the same address is not confused with it).
		bool IsSameOwner(const std::weak_ptr<AssetManagerBase>& a, const Ref<AssetManagerBase>& b)
		{
			return !a.owner_before(b) && !b.owner_before(a);
		}

		// Velocity from the distance moved during the last frame; implausibly fast moves are teleports (no Doppler shift).
		glm::vec3 ComputeVelocity(const glm::vec3& previous, const glm::vec3& current, float timestep)
		{
			if (!(timestep > 0.0f))
				return glm::vec3(0.0f);
			const glm::vec3 velocity = (current - previous) / timestep;
			if (!IsFinite(velocity) || glm::dot(velocity, velocity) > AudioSystem::c_MaxDopplerSpeed * AudioSystem::c_MaxDopplerSpeed)
				return glm::vec3(0.0f);
			return velocity;
		}

		// The up vector to give the AudioEngine for a listener. miniaudio derives the right axis from forward x up, so +Y gives
		// exactly the same orientation for a listener that does not roll, whatever its pitch and yaw; the up vector then
		// never changes, so it does not need to travel to the mixing thread every frame (see AudioEngine::SetListener).
		// Rolled listeners, and those looking (nearly) straight up or down, where forward x +Y degenerates, pass their own.
		glm::vec3 GetListenerUp(const glm::vec3& forward, const glm::vec3& up, const glm::vec3& right)
		{
			const bool level = std::abs(right.y) <= c_MaxLevelRoll * glm::length(right) && std::abs(forward.y) <= c_MaxLevelPitch * glm::length(forward);
			return level ? glm::vec3(0.0f, 1.0f, 0.0f) : up;
		}

		// The position of an entity in depth-first hierarchy order, as its sibling indices from its root down to it:
		// comparing two paths lexicographically orders entities like Scene::GetEntitiesInHierarchyOrder, without visiting
		// the whole scene.
		std::vector<size_t> GetHierarchyPath(const Scene& scene, Entity entity)
		{
			std::vector<size_t> path;
			for (Entity current = entity; current.IsValid();)
			{
				const UUID id = current.GetUUID();
				const Entity parent = current.GetParent();
				const std::vector<UUID>& siblings = parent ? parent.GetComponent<RelationshipComponent>().Children : scene.GetRootEntities();
				path.push_back(static_cast<size_t>(std::find(siblings.begin(), siblings.end(), id) - siblings.begin()));
				current = parent;
			}
			std::reverse(path.begin(), path.end());
			return path;
		}

		// The first of the candidates in hierarchy order (invalid if there are none).
		Entity FindFirstInHierarchyOrder(const Scene& scene, const std::vector<Entity>& candidates)
		{
			if (candidates.size() <= 1)
				return candidates.empty() ? Entity() : candidates.front();

			Entity first = candidates.front();
			std::vector<size_t> firstPath = GetHierarchyPath(scene, first);
			for (size_t index = 1; index < candidates.size(); index++)
			{
				std::vector<size_t> path = GetHierarchyPath(scene, candidates[index]);
				if (path < firstPath)
				{
					first = candidates[index];
					firstPath = std::move(path);
				}
			}
			return first;
		}

		// The clip of a loaded asset, or nullptr (with the reason) if the asset cannot be played.
		Ref<AudioClip> GetClip(const Ref<Asset>& asset, std::string& outReason)
		{
			if (asset->GetType() != AssetType::AudioClip)
			{
				outReason = "it is not an audio clip";
				return nullptr;
			}
			Ref<AudioClip> clip = static_cast<const AudioClipAsset&>(*asset).GetClip();
			if (!clip)
				outReason = "it has no audio data";
			return clip;
		}

	}

	AudioSystem::AudioSystem(Scene& scene)
		: m_Scene(scene)
	{
		// Clips resolve against the current assets when sources are created; only later changes need a second look.
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		m_AssetManager = manager;
		m_SeenContentVersion = manager ? manager->GetContentVersion() : 0;
	}

	AudioSystem::~AudioSystem()
	{
		OnRuntimeStop();
	}

	void AudioSystem::OnRuntimeStart()
	{
		ST_PROFILE_FUNCTION();

		// Scripts may have used the API in their OnCreate already (systems start in registration order).
		m_Paused = m_Scene.IsPaused();
		UpdateSources(0.0f);
		UpdateListener(0.0f);
	}

	void AudioSystem::OnRuntimeStop()
	{
		if (m_Stopped)
			return;
		m_Stopped = true;

		m_Sources.clear();
		m_OneShots.clear();
		m_ListenerEntity = {};
		// The next scene to play starts from the default listener.
		AudioEngine::SetListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	}

	void AudioSystem::OnLateUpdate(Timestep timestep)
	{
		ST_PROFILE_FUNCTION();

		if (m_Stopped)
			return;
		ProcessAssetChanges();
		UpdateSources(timestep);
		UpdateListener(timestep);
		UpdateOneShots(timestep);
	}

	void AudioSystem::OnPausedChanged(bool paused)
	{
		if (m_Stopped || paused == m_Paused)
			return;
		m_Paused = paused;

		for (auto& [handle, record] : m_Sources)
		{
			if (paused && record.Source->IsPlaying())
			{
				record.Source->Pause();
				record.PausedByScene = true;
			}
			else if (!paused)
			{
				if (record.PausedByScene)
					record.Source->Play();
				record.PausedByScene = false;
				StartIfRequested(record);
			}
		}

		for (OneShot& oneShot : m_OneShots)
		{
			if (paused && oneShot.Source->IsPlaying())
			{
				oneShot.Source->Pause();
				oneShot.PausedByScene = true;
			}
			else if (!paused && oneShot.PausedByScene)
			{
				oneShot.Source->Play();
				oneShot.PausedByScene = false;
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Gameplay API
	////////////////////////////////////////////////////////////////////////////////

	bool AudioSystem::Play(Entity entity)
	{
		SourceRecord* record = PrepareSource(entity);
		if (!record || !AudioEngine::IsInitialized())
			return false;

		switch (record->State)
		{
			case ClipState::NoClip:
			case ClipState::Unavailable:
				return false;
			case ClipState::Waiting:
				record->PlayRequested = true;
				return true;
			case ClipState::Ready:
				break;
		}

		if (m_Paused)
		{
			// A source paused by the scene resumes with it anyway; others start then.
			if (!record->PausedByScene)
				record->PlayRequested = true;
			return true;
		}
		record->Source->Play();
		return record->Source->IsPlaying();
	}

	bool AudioSystem::Pause(Entity entity)
	{
		SourceRecord* record = PrepareSource(entity);
		if (!record)
			return false;

		record->PlayRequested = false;
		// A source the scene paused is already halted at its position; it just does not resume.
		record->PausedByScene = false;
		record->Source->Pause();
		return true;
	}

	bool AudioSystem::Stop(Entity entity)
	{
		SourceRecord* record = PrepareSource(entity);
		if (!record)
			return false;

		record->PlayRequested = false;
		record->PausedByScene = false;
		record->PendingSeek.reset();
		record->Source->Stop();
		return true;
	}

	bool AudioSystem::IsPlaying(Entity entity)
	{
		const SourceRecord* record = PrepareSource(entity);
		return record && (record->Source->IsPlaying() || record->PlayRequested || record->PausedByScene);
	}

	bool AudioSystem::Seek(Entity entity, float seconds)
	{
		if (!std::isfinite(seconds))
		{
			ST_CORE_WARN("AudioSystem::Seek: ignoring non-finite position");
			return false;
		}
		SourceRecord* record = PrepareSource(entity);
		if (!record)
			return false;

		if (record->State == ClipState::Ready)
			record->Source->Seek(seconds);
		else
			record->PendingSeek = std::max(seconds, 0.0f);
		return true;
	}

	float AudioSystem::GetPlaybackPosition(Entity entity)
	{
		const SourceRecord* record = PrepareSource(entity);
		if (!record)
			return 0.0f;
		if (record->State != ClipState::Ready)
			return record->PendingSeek.value_or(0.0f);
		return record->Source->GetPlaybackPosition();
	}

	bool AudioSystem::PlayOneShot(AssetHandle clip, float volume, float pitch)
	{
		return StartOneShot(clip, false, glm::vec3(0.0f), volume, pitch);
	}

	bool AudioSystem::PlayOneShotAt(AssetHandle clip, const glm::vec3& position, float volume, float pitch)
	{
		if (!IsFinite(position))
		{
			ST_CORE_WARN("AudioSystem::PlayOneShotAt: ignoring non-finite position");
			return false;
		}
		return StartOneShot(clip, true, position, volume, pitch);
	}

	void AudioSystem::SetMasterVolume(float volume)
	{
		AudioEngine::SetMasterVolume(volume);
	}

	float AudioSystem::GetMasterVolume()
	{
		return AudioEngine::GetMasterVolume();
	}

	const AudioSource* AudioSystem::GetAudioSource(Entity entity) const
	{
		if (!entity.IsValid() || entity.GetScene() != &m_Scene)
			return nullptr;
		auto it = m_Sources.find(entity.GetHandle());
		return it != m_Sources.end() ? it->second.Source.get() : nullptr;
	}

	AudioSystemStats AudioSystem::GetStats() const
	{
		AudioSystemStats stats;
		for (const auto& [handle, record] : m_Sources)
		{
			stats.SourceCount++;
			if (record.Source->IsPlaying() || record.PlayRequested || record.PausedByScene)
				stats.PlayingSourceCount++;
			if (record.State == ClipState::Waiting)
				stats.WaitingSourceCount++;
		}
		stats.OneShotCount = static_cast<uint32_t>(m_OneShots.size());
		return stats;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Sources
	////////////////////////////////////////////////////////////////////////////////

	bool AudioSystem::IsAudible(Entity entity) const
	{
		for (Entity current = entity; current.IsValid(); current = current.GetParent())
		{
			if (current.HasComponent<InactiveComponent>() || m_Scene.IsPendingDestroy(current))
				return false;
		}
		return entity.IsValid();
	}

	AudioSystem::SourceRecord* AudioSystem::PrepareSource(Entity entity)
	{
		if (m_Stopped || !entity.IsValid() || entity.GetScene() != &m_Scene)
			return nullptr;
		const AudioSourceComponent* component = entity.TryGetComponent<AudioSourceComponent>();
		if (!component || !IsAudible(entity))
			return nullptr;

		auto it = m_Sources.find(entity.GetHandle());
		if (it == m_Sources.end())
			return &CreateSource(entity, *component);

		// Changes made since the last update apply first, e.g. a new clip followed by Play.
		ApplyComponent(it->second, *component);
		if (it->second.Applied.Clip != component->Clip)
			ResolveClip(it->second, entity);
		return &it->second;
	}

	AudioSystem::SourceRecord& AudioSystem::CreateSource(Entity entity, const AudioSourceComponent& component)
	{
		SourceRecord& record = m_Sources[entity.GetHandle()];
		record.Source = CreateScope<AudioSource>();
		record.SeenUpdate = m_UpdateIndex;

		AudioSourceSettings settings;
		settings.Volume = component.Volume;
		settings.Pitch = component.Pitch;
		settings.Looping = component.Loop;
		settings.Spatial = component.Spatial;
		settings.MinDistance = component.MinDistance;
		settings.MaxDistance = component.MaxDistance;
		settings.Rolloff = component.Rolloff;
		record.Source->SetSettings(settings);
		record.Applied = component;
		// Placed before it can start, so that its first samples come from the right position.
		UpdateSourcePosition(record, entity, 0.0f);

		record.State = component.Clip.IsValid() ? ClipState::Waiting : ClipState::NoClip;
		record.PlayRequested = component.PlayOnStart && record.State == ClipState::Waiting;
		if (record.State == ClipState::Waiting)
			ResolveClip(record, entity);
		return record;
	}

	void AudioSystem::ApplyComponent(SourceRecord& record, const AudioSourceComponent& component)
	{
		AudioSource& source = *record.Source;
		AudioSourceComponent& applied = record.Applied;
		if (!IsSameFloat(component.Volume, applied.Volume))
			source.SetVolume(component.Volume);
		if (!IsSameFloat(component.Pitch, applied.Pitch))
			source.SetPitch(component.Pitch);
		if (component.Loop != applied.Loop)
			source.SetLooping(component.Loop);
		if (component.Spatial != applied.Spatial)
		{
			source.SetSpatial(component.Spatial);
			record.HasLastPosition = false; // No velocity from a position that is no longer followed
		}
		if (!IsSameFloat(component.MinDistance, applied.MinDistance))
			source.SetMinDistance(component.MinDistance);
		if (!IsSameFloat(component.MaxDistance, applied.MaxDistance))
			source.SetMaxDistance(component.MaxDistance);
		if (!IsSameFloat(component.Rolloff, applied.Rolloff))
			source.SetRolloff(component.Rolloff);

		// The clip is left to ResolveClip, which notices a new handle, stops the source and loads the new clip.
		const AssetHandle previousClip = applied.Clip;
		applied = component;
		applied.Clip = previousClip;
	}

	void AudioSystem::ResolveClip(SourceRecord& record, Entity entity)
	{
		const AudioSourceComponent& component = entity.GetComponent<AudioSourceComponent>();
		if (record.Applied.Clip != component.Clip)
		{
			// A new clip stops the source; Play starts the new one.
			record.Applied.Clip = component.Clip;
			record.Source->SetClip(nullptr);
			record.PlayRequested = false;
			record.PausedByScene = false;
			record.PendingSeek.reset();
			record.State = component.Clip.IsValid() ? ClipState::Waiting : ClipState::NoClip;
		}
		if (record.State != ClipState::Waiting)
			return;

		const AssetHandle handle = record.Applied.Clip;
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		std::string reason;
		if (manager)
		{
			// Requests the load if needed; never blocks.
			if (Ref<Asset> asset = manager->GetAsset(handle))
			{
				if (Ref<AudioClip> clip = GetClip(asset, reason))
				{
					if (record.Source->SetClip(clip))
					{
						record.State = ClipState::Ready;
						if (record.PendingSeek)
							record.Source->Seek(*record.PendingSeek);
						record.PendingSeek.reset();
						StartIfRequested(record);
						return;
					}
					reason = "the mixer cannot play it";
				}
			}
			else if (manager->GetAssetState(handle) == AssetState::Loading)
			{
				return;
			}
			else if (reason.empty())
			{
				reason = manager->IsHandleValid(handle) ? manager->GetAssetError(handle) : std::string("it is not a known asset");
			}
		}
		else
		{
			reason = "no asset manager is active";
		}
		if (reason.empty())
			reason = "it did not load";

		record.State = ClipState::Unavailable;
		record.PlayRequested = false;
		ST_CORE_WARN("Audio: the clip {} of '{}' cannot be played: {}", handle.ToString(), entity.GetName(), reason);
	}

	void AudioSystem::RefreshClip(SourceRecord& record, Entity entity)
	{
		switch (record.State)
		{
			case ClipState::NoClip:
			case ClipState::Waiting: // Polled every update anyway
				return;
			case ClipState::Unavailable:
				// The asset changed (e.g. reimported): try again.
				record.State = ClipState::Waiting;
				ResolveClip(record, entity);
				return;
			case ClipState::Ready:
				break;
		}

		// A reloaded clip replaces the current one; a clip unloaded on purpose is kept (and not loaded again).
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		const AssetHandle handle = record.Applied.Clip;
		if (!manager || manager->GetAssetState(handle) != AssetState::Ready)
			return;
		const Ref<Asset> asset = manager->GetAsset(handle);
		std::string reason;
		const Ref<AudioClip> clip = asset ? GetClip(asset, reason) : nullptr;
		if (!clip || clip == record.Source->GetClip())
			return;

		const bool wasPlaying = record.Source->IsPlaying() || record.PausedByScene;
		if (!record.Source->SetClip(clip))
		{
			record.State = ClipState::Unavailable;
			ST_CORE_WARN("Audio: the reloaded clip {} of '{}' cannot be played: the mixer cannot play it", handle.ToString(), entity.GetName());
			return;
		}
		record.PausedByScene = false;
		record.PlayRequested = record.PlayRequested || wasPlaying;
		StartIfRequested(record);
	}

	void AudioSystem::StartIfRequested(SourceRecord& record)
	{
		if (!record.PlayRequested || m_Paused || record.State != ClipState::Ready)
			return;
		record.PlayRequested = false;
		record.Source->Play();
	}

	void AudioSystem::UpdateSourcePosition(SourceRecord& record, Entity entity, float timestep)
	{
		if (!record.Applied.Spatial)
			return;

		const glm::vec3 position = glm::vec3(m_Scene.GetWorldTransform(entity)[3]);
		if (!IsFinite(position))
			return; // A degenerate transform keeps the last position
		const glm::vec3 velocity = record.HasLastPosition ? ComputeVelocity(record.LastPosition, position, timestep) : glm::vec3(0.0f);
		record.Source->SetPosition(position);
		record.Source->SetVelocity(velocity);
		record.LastPosition = position;
		record.HasLastPosition = true;
	}

	void AudioSystem::UpdateSources(float timestep)
	{
		ST_PROFILE_FUNCTION();

		m_UpdateIndex++;
		entt::registry& registry = m_Scene.GetRegistry();
		for (const auto [handle, component] : registry.view<AudioSourceComponent>().each())
		{
			const Entity entity(handle, &m_Scene);
			if (!IsAudible(entity))
				continue;

			auto it = m_Sources.find(handle);
			if (it == m_Sources.end())
			{
				CreateSource(entity, component);
				continue;
			}

			SourceRecord& record = it->second;
			record.SeenUpdate = m_UpdateIndex;
			ApplyComponent(record, component);
			if (record.Applied.Clip != component.Clip || record.State == ClipState::Waiting)
				ResolveClip(record, entity);
			UpdateSourcePosition(record, entity, timestep);
		}

		// Components removed, entities deactivated or destroyed: their sources stop and are released.
		std::erase_if(m_Sources, [this](const auto& entry) { return entry.second.SeenUpdate != m_UpdateIndex; });
	}

	void AudioSystem::ProcessAssetChanges()
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		const uint64_t version = manager ? manager->GetContentVersion() : 0;
		bool checkAll = false;
		m_ChangedAssets.clear();
		if (!IsSameOwner(m_AssetManager, manager))
		{
			m_AssetManager = manager;
			checkAll = true;
		}
		else if (version == m_SeenContentVersion)
		{
			return;
		}
		else
		{
			checkAll = !manager->GetContentChanges(m_SeenContentVersion, m_ChangedAssets);
			std::sort(m_ChangedAssets.begin(), m_ChangedAssets.end());
		}
		m_SeenContentVersion = version;

		for (auto& [handle, record] : m_Sources)
		{
			if (!record.Applied.Clip.IsValid())
				continue;
			if (checkAll || std::binary_search(m_ChangedAssets.begin(), m_ChangedAssets.end(), record.Applied.Clip))
				RefreshClip(record, Entity(handle, &m_Scene));
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Listener
	////////////////////////////////////////////////////////////////////////////////

	void AudioSystem::UpdateListener(float timestep)
	{
		ST_PROFILE_FUNCTION();

		m_ListenerCandidates.clear();
		entt::registry& registry = m_Scene.GetRegistry();
		for (const auto [handle, listener] : registry.view<AudioListenerComponent>().each())
		{
			const Entity entity(handle, &m_Scene);
			if (listener.Active && IsAudible(entity))
				m_ListenerCandidates.push_back(entity);
		}
		if (m_ListenerCandidates.empty())
		{
			// The camera the player sees through (the first primary camera, see Scene::GetPrimaryCameraEntity).
			for (const auto [handle, camera] : registry.view<CameraComponent>().each())
			{
				const Entity entity(handle, &m_Scene);
				if (camera.Primary && IsAudible(entity))
					m_ListenerCandidates.push_back(entity);
			}
		}
		const Entity listener = FindFirstInHierarchyOrder(m_Scene, m_ListenerCandidates);

		if (!listener)
		{
			m_ListenerEntity = {};
			AudioEngine::SetListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			return;
		}

		const glm::mat4 transform = m_Scene.GetWorldTransform(listener);
		const glm::vec3 position = glm::vec3(transform[3]);
		const glm::vec3 forward = -glm::vec3(transform[2]);
		const glm::vec3 up = glm::vec3(transform[1]);
		const glm::vec3 right = glm::vec3(transform[0]);
		// A degenerate transform (zero scale) has no orientation: the listener stays where it was.
		if (!IsFinite(position) || !IsFinite(forward) || !IsFinite(up) || glm::dot(forward, forward) < c_MinDirectionLengthSquared || glm::dot(up, up) < c_MinDirectionLengthSquared)
			return;

		const glm::vec3 velocity = listener == m_ListenerEntity ? ComputeVelocity(m_ListenerPosition, position, timestep) : glm::vec3(0.0f);
		AudioEngine::SetListener(position, forward, GetListenerUp(forward, up, right), velocity);
		m_ListenerEntity = listener;
		m_ListenerPosition = position;
	}

	////////////////////////////////////////////////////////////////////////////////
	// One-shots
	////////////////////////////////////////////////////////////////////////////////

	bool AudioSystem::StartOneShot(AssetHandle clip, bool spatial, const glm::vec3& position, float volume, float pitch)
	{
		if (m_Stopped || !AudioEngine::IsInitialized())
			return false;
		if (!std::isfinite(volume) || volume < 0.0f || !std::isfinite(pitch) || pitch <= 0.0f)
		{
			ST_CORE_WARN("AudioSystem: invalid one-shot volume {} or pitch {}", volume, pitch);
			return false;
		}

		OneShot oneShot;
		oneShot.Clip = clip;
		oneShot.Source = CreateScope<AudioSource>();
		AudioSourceSettings settings;
		settings.Volume = volume;
		settings.Pitch = pitch;
		settings.Spatial = spatial;
		settings.Position = position;
		oneShot.Source->SetSettings(settings);
		if (!TryStartOneShot(oneShot))
			return false;

		// Voice stealing: cut off the oldest one-shot so the newest sound always plays.
		if (m_OneShots.size() >= c_MaxOneShots)
			m_OneShots.erase(m_OneShots.begin());
		m_OneShots.push_back(std::move(oneShot));
		return true;
	}

	bool AudioSystem::TryStartOneShot(OneShot& oneShot)
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		std::string reason;
		if (!manager)
		{
			reason = "no asset manager is active";
		}
		else if (Ref<Asset> asset = manager->GetAsset(oneShot.Clip))
		{
			if (Ref<AudioClip> clip = GetClip(asset, reason))
			{
				if (!oneShot.Source->SetClip(clip))
					return false; // Logged by the mixer
				oneShot.Started = true;
				if (m_Paused)
					oneShot.PausedByScene = true; // Starts when the scene resumes
				else
					oneShot.Source->Play();
				return true;
			}
		}
		else if (manager->GetAssetState(oneShot.Clip) == AssetState::Loading)
		{
			return true; // Waits for the clip
		}
		else
		{
			reason = manager->IsHandleValid(oneShot.Clip) ? manager->GetAssetError(oneShot.Clip) : std::string("it is not a known asset");
		}

		ST_CORE_WARN("Audio: cannot play the one-shot clip {}: {}", oneShot.Clip.ToString(), reason);
		return false;
	}

	void AudioSystem::UpdateOneShots(float timestep)
	{
		std::erase_if(m_OneShots, [&](OneShot& oneShot)
		{
			if (!oneShot.Started)
			{
				oneShot.WaitedTime += timestep;
				// Too late: a sound effect that comes long after its cause is worse than none.
				if (!TryStartOneShot(oneShot))
					return true;
				return !oneShot.Started && oneShot.WaitedTime > c_MaxOneShotClipWait;
			}
			// Finished (or never got a voice) one-shots are reclaimed.
			return !oneShot.PausedByScene && !oneShot.Source->IsPlaying();
		});
	}

}
