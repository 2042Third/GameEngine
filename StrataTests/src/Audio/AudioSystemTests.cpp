#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Physics/PhysicsTestUtils.h"
#include "Strata/Audio/AudioClip.h"
#include "Strata/Audio/AudioEngine.h"
#include "Strata/Audio/AudioSource.h"
#include "Strata/Audio/AudioSystem.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A non-spatial source that plays on start.
	Entity CreateSource(Scene& scene, const std::string& name, AssetHandle clip, bool loop = true)
	{
		Entity entity = scene.CreateEntity(name);
		AudioSourceComponent& source = entity.AddComponent<AudioSourceComponent>();
		source.Clip = clip;
		source.Loop = loop;
		source.Spatial = false;
		return entity;
	}

	AudioSystem& GetAudio(Scene& scene)
	{
		AudioSystem* audio = scene.GetSystem<AudioSystem>();
		REQUIRE(audio != nullptr);
		return *audio;
	}

	// Registers a scene system for the lifetime of the object (also when a test fails early).
	struct ScopedSceneSystemRegistration
	{
		explicit ScopedSceneSystemRegistration(SceneSystemDescriptor descriptor)
			: Name(descriptor.Name)
		{
			SceneSystemRegistry::Register(std::move(descriptor));
		}

		~ScopedSceneSystemRegistration()
		{
			SceneSystemRegistry::Unregister(Name);
		}

		ScopedSceneSystemRegistration(const ScopedSceneSystemRegistration&) = delete;
		ScopedSceneSystemRegistration& operator=(const ScopedSceneSystemRegistration&) = delete;

		std::string Name;
	};

	// Moves "Mover" along +Z at Speed on every fixed step, like a script's OnFixedUpdate.
	struct FixedMoverSystem : public SceneSystem
	{
		explicit FixedMoverSystem(Scene& scene)
			: TargetScene(scene)
		{
		}

		void OnFixedUpdate(float timestep) override
		{
			if (Entity mover = TargetScene.FindEntityByName("Mover"))
				mover.GetTransform().Translation.z += Speed * timestep;
		}

		Scene& TargetScene;
		static inline float Speed = 0.0f;
	};

	// Destroys "Doomed" during its update, so the entity is pending destruction for the rest of the frame.
	struct DoomingSystem : public SceneSystem
	{
		explicit DoomingSystem(Scene& scene)
			: TargetScene(scene)
		{
		}

		void OnUpdate(Timestep) override
		{
			if (Entity doomed = TargetScene.FindEntityByName("Doomed"))
				TargetScene.DestroyEntity(doomed);
		}

		Scene& TargetScene;
	};

}

TEST_SUITE("Audio.System")
{
	TEST_CASE("Audio plays in Play mode only, after scripts and physics")
	{
		const std::vector<SceneSystemDescriptor>& descriptors = SceneSystemRegistry::GetAll();
		const auto indexOf = [&](const std::string& name)
		{
			auto it = std::find_if(descriptors.begin(), descriptors.end(), [&](const SceneSystemDescriptor& descriptor) { return descriptor.Name == name; });
			REQUIRE(it != descriptors.end());
			return it - descriptors.begin();
		};
		CHECK(indexOf("Audio") > indexOf("Physics"));
		CHECK(indexOf("Physics") > indexOf("Scripting"));
		CHECK_FALSE(descriptors[static_cast<size_t>(indexOf("Audio"))].RunsInSimulateMode);

		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		Scene scene;
		CreateSource(scene, "Music", project.AddClip(1.0f));
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
		CHECK(scene.GetSystem<AudioSystem>() == nullptr);
		StepScene(scene, 2);
		CHECK(AudioEngine::GetStats().SourceCount == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Sources play when the scene starts and are released when it stops")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClip(1.0f);

		Scene scene;
		Entity music = CreateSource(scene, "Music", clip);
		Entity idle = CreateSource(scene, "Idle", clip);
		idle.GetComponent<AudioSourceComponent>().PlayOnStart = false;
		Entity empty = CreateSource(scene, "Empty", UUID::Null());
		CHECK(AudioEngine::GetStats().SourceCount == 0);

		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		CHECK(audio.IsPlaying(music));
		CHECK_FALSE(audio.IsPlaying(idle));
		CHECK_FALSE(audio.IsPlaying(empty));
		const AudioSystemStats stats = audio.GetStats();
		CHECK(stats.SourceCount == 3);
		CHECK(stats.PlayingSourceCount == 1);
		CHECK(stats.WaitingSourceCount == 0);
		AudioStats engineStats = AudioEngine::GetStats();
		CHECK(engineStats.SourceCount == 3);
		CHECK(engineStats.AllocatedVoices == 2); // The sources with a clip
		CHECK(engineStats.ActiveVoices == 1);
		CHECK(MeasureRms() == doctest::Approx(c_SineRms).epsilon(0.05));

		StepScene(scene, 3);
		CHECK(audio.IsPlaying(music));
		CHECK(audio.GetAudioSource(music)->GetClip() != nullptr);

		scene.OnRuntimeStop();
		engineStats = AudioEngine::GetStats();
		CHECK(engineStats.SourceCount == 0);
		CHECK(engineStats.AllocatedVoices == 0);
		CHECK(ComputeRms(Render(4800)) == 0.0f);
	}

	TEST_CASE("Clips that are still loading play once they are ready")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClipFile("Late", 1.0f);

		Scene scene;
		Entity music = CreateSource(scene, "Music", clip);
		Entity later = CreateSource(scene, "Later", clip);
		later.GetComponent<AudioSourceComponent>().PlayOnStart = false;

		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		CHECK(project.Manager->GetAssetState(clip) == AssetState::Loading); // Requested, never waited for
		CHECK(audio.GetStats().WaitingSourceCount == 2);
		CHECK(audio.IsPlaying(music)); // About to
		CHECK_FALSE(audio.IsPlaying(later));
		CHECK(AudioEngine::GetStats().ActiveVoices == 0);

		// Requests made meanwhile apply once the clip is there.
		CHECK(audio.Seek(later, 0.5f));
		CHECK(audio.GetPlaybackPosition(later) == 0.5f);
		CHECK(audio.Play(later));
		CHECK(audio.IsPlaying(later));
		StepScene(scene, 2);
		CHECK(audio.GetStats().WaitingSourceCount == 2);

		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 1);
		CHECK(audio.GetStats().WaitingSourceCount == 0);
		CHECK(AudioEngine::GetStats().ActiveVoices == 2);
		CHECK(audio.IsPlaying(music));
		CHECK(audio.IsPlaying(later));
		CHECK(std::abs(audio.GetPlaybackPosition(later) - 0.5f) < c_PositionTolerance);
		CHECK(MeasureRms() > c_SineRms);
	}

	TEST_CASE("Reloaded clips replace the playing ones; unloaded clips keep playing")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClipFile("Loop", 1.0f);
		REQUIRE(project.Manager->LoadAssetSync(clip));

		Scene scene;
		Entity music = CreateSource(scene, "Music", clip);
		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		const AudioSource* source = audio.GetAudioSource(music);
		REQUIRE(source);
		const Ref<AudioClip> original = source->GetClip();
		REQUIRE(original);
		CHECK(MeasureRms() == doctest::Approx(c_SineRms).epsilon(0.05));

		project.WriteClipFile("Loop", 2.0f, c_SineAmplitude * 0.5f);
		REQUIRE(project.Manager->ReimportAsset(clip));
		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 1);
		REQUIRE(source->GetClip());
		CHECK(source->GetClip() != original);
		CHECK(source->GetClip()->GetLength() == doctest::Approx(2.0f));
		CHECK(audio.IsPlaying(music));
		CHECK(MeasureRms() == doctest::Approx(c_SineRms * 0.5f).epsilon(0.05));

		// A clip unloaded on purpose keeps playing, and is not loaded again.
		const Ref<AudioClip> reloaded = source->GetClip();
		project.Manager->UnloadAsset(clip);
		StepScene(scene, 2);
		CHECK(project.Manager->GetAssetState(clip) == AssetState::Unloaded);
		CHECK(source->GetClip() == reloaded);
		CHECK(audio.IsPlaying(music));
	}

	TEST_CASE("Clips that cannot be played are reported once")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		AssetMetadata metadata;
		metadata.Name = "NotAClip";
		const AssetHandle material = project.Manager->AddMemoryAsset(Material::Create(), metadata);

		Scene scene;
		Entity unknown = CreateSource(scene, "Unknown", UUID(0x1234));
		Entity wrong = CreateSource(scene, "Wrong", material);
		const uint64_t logSequence = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		StepScene(scene, 3);
		CHECK(CountLogMessages(logSequence, "cannot be played") == 2);
		CHECK(CountLogMessages(logSequence, "it is not an audio clip") == 1);
		CHECK_FALSE(audio.IsPlaying(unknown));
		CHECK_FALSE(audio.Play(unknown));
		CHECK_FALSE(audio.Play(wrong));
		CHECK(audio.GetStats().WaitingSourceCount == 0);
		CHECK(audio.GetStats().SourceCount == 2);

		// Another clip plays.
		unknown.GetComponent<AudioSourceComponent>().Clip = project.AddClip(1.0f);
		CHECK(audio.Play(unknown));
		CHECK(audio.IsPlaying(unknown));
		CHECK(CountLogMessages(logSequence, "cannot be played") == 2);
	}

	TEST_CASE("Sources whose component or entity goes away are not looked at again")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const std::vector<uint8_t> garbage(256, 0x5A);
		REQUIRE(FileSystem::WriteBytes(project.Sounds / "Broken.wav", garbage));
		project.Manager->Scan();
		const AssetHandle broken = project.Manager->FindAssetByPath("Sounds/Broken.wav");
		REQUIRE(broken.IsValid());

		Scene scene;
		Entity removed = CreateSource(scene, "Removed", broken);
		Entity destroyed = CreateSource(scene, "Destroyed", broken);
		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 1);
		REQUIRE(project.Manager->GetAssetState(broken) == AssetState::Failed);
		CHECK_FALSE(audio.Play(removed)); // Unavailable: retried when the clip changes

		// The clip is repaired, and its change lands in the frame in which the sources go away.
		project.WriteClipFile("Broken", 1.0f, c_SineAmplitude);
		REQUIRE(project.Manager->ReimportAsset(broken));
		REQUIRE(project.Manager->WaitForPendingLoads());
		removed.RemoveComponent<AudioSourceComponent>();
		scene.DestroyEntity(destroyed);
		CHECK(audio.GetStats().SourceCount == 1); // A destroyed entity's source goes at once
		StepScene(scene, 1);
		CHECK(audio.GetStats().SourceCount == 0);
		CHECK(AudioEngine::GetStats().SourceCount == 0);

		// A source added again plays the repaired clip.
		AudioSourceComponent& component = removed.AddComponent<AudioSourceComponent>();
		component.Clip = broken;
		component.Spatial = false;
		StepScene(scene, 1);
		CHECK(audio.IsPlaying(removed));
		CHECK(AudioEngine::GetStats().ActiveVoices == 1);
	}

	TEST_CASE("Component changes apply while playing, with or without a signal")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle shortClip = project.AddClip(0.5f);
		const AssetHandle longClip = project.AddClip(1.0f);

		Scene scene;
		Entity entity = CreateSource(scene, "Source", shortClip);
		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		const AudioSource* source = audio.GetAudioSource(entity);
		REQUIRE(source);
		CHECK(MeasureRms() == doctest::Approx(c_SineRms).epsilon(0.05));

		// Plain field writes.
		AudioSourceComponent& component = entity.GetComponent<AudioSourceComponent>();
		component.Volume = 0.5f;
		component.Pitch = 2.0f;
		component.Spatial = true;
		component.MinDistance = 2.0f;
		component.MaxDistance = 30.0f;
		component.Rolloff = 0.5f;
		StepScene(scene, 1);
		CHECK(source->GetVolume() == 0.5f);
		CHECK(source->GetPitch() == 2.0f);
		CHECK(source->IsSpatial());
		CHECK(source->GetMinDistance() == 2.0f);
		CHECK(source->GetMaxDistance() == 30.0f);
		CHECK(source->GetRolloff() == 0.5f);
		component.Pitch = 1.0f;
		component.Spatial = false;
		StepScene(scene, 1);
		CHECK(MeasureRms() == doctest::Approx(c_SineRms * 0.5f).epsilon(0.05));

		// Through ComponentAccess, like the inspector and the automation API.
		const ComponentInfo* info = ComponentRegistry::Find<AudioSourceComponent>();
		REQUIRE(info);
		const PropertyInfo* volume = info->FindProperty("Volume");
		REQUIRE(volume);
		REQUIRE(ComponentAccess::SetProperty(entity, *info, *volume, PropertyValue(0.25f)));
		StepScene(scene, 1);
		CHECK(source->GetVolume() == 0.25f);

		// A value the source rejects is reported once, not every frame, and the previous one stays.
		const uint64_t logSequence = Log::GetBuffer().GetLatestSequence();
		component.Pitch = std::numeric_limits<float>::quiet_NaN();
		StepScene(scene, 5);
		CHECK(CountLogMessages(logSequence, "SetPitch") == 1);
		CHECK(source->GetPitch() == 1.0f);
		component.Pitch = 1.0f;

		// Without looping the clip ends.
		component.Loop = false;
		StepScene(scene, 1);
		Render(c_SampleRate);
		CHECK_FALSE(audio.IsPlaying(entity));

		// A new clip stops the source; a Play in the same frame starts the new clip.
		REQUIRE(audio.Play(entity));
		component.Clip = longClip;
		REQUIRE(audio.Play(entity));
		REQUIRE(source->GetClip());
		CHECK(source->GetClip()->GetLength() == doctest::Approx(1.0f));
		CHECK(audio.IsPlaying(entity));
		component.Clip = shortClip;
		StepScene(scene, 1);
		CHECK(source->GetClip()->GetLength() == doctest::Approx(0.5f));
		CHECK_FALSE(audio.IsPlaying(entity));
		// Clearing the clip leaves nothing to play.
		component.Clip = UUID::Null();
		StepScene(scene, 1);
		CHECK_FALSE(source->GetClip());
		CHECK_FALSE(audio.Play(entity));
	}

	TEST_CASE("Spatial sources and the listener follow their entities")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;

		Scene scene;
		Entity listener = scene.CreateEntity("Listener");
		listener.AddComponent<AudioListenerComponent>();
		Entity entity = CreateSource(scene, "Source", project.AddClip(1.0f));
		AudioSourceComponent& component = entity.GetComponent<AudioSourceComponent>();
		component.Spatial = true;
		component.MinDistance = 1.0f;
		component.MaxDistance = 100.0f;
		component.Rolloff = 1.0f;
		entity.GetTransform().Translation = glm::vec3(0.0f, 0.0f, -2.0f);

		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		const AudioSource* source = audio.GetAudioSource(entity);
		REQUIRE(source);
		CHECK(audio.GetListenerEntity() == listener);
		const float nearRms = MeasureRms();

		// Moved by a plain field write: inverse distance attenuation, 1/2 against 1/20.
		entity.GetTransform().Translation = glm::vec3(0.0f, 0.0f, -20.0f);
		StepScene(scene, 1);
		CHECK(nearRms / MeasureRms() == doctest::Approx(10.0f).epsilon(0.1));

		// The listener moving closer brings the level back.
		listener.GetTransform().Translation = glm::vec3(0.0f, 0.0f, -18.0f);
		StepScene(scene, 1);
		CHECK(MeasureRms() / nearRms == doctest::Approx(1.0f).epsilon(0.05));

		// The listener's orientation pans: a source to its right is louder on the right, until it turns around.
		entity.GetTransform().Translation = glm::vec3(5.0f, 0.0f, -18.0f);
		StepScene(scene, 1);
		CHECK(MeasureRms(1) > MeasureRms(0) * 1.5f);
		listener.GetTransform().Rotation = glm::angleAxis(std::numbers::pi_v<float>, glm::vec3(0.0f, 1.0f, 0.0f));
		StepScene(scene, 1);
		CHECK(MeasureRms(0) > MeasureRms(1) * 1.5f);

		// Velocities come from the moves (smoothed); an entity that stops has none, and teleports have none.
		const float timestep = scene.GetSettings().FixedTimestep;
		for (int frame = 0; frame < 60; frame++)
		{
			entity.GetTransform().Translation.z += 1.0f;
			StepScene(scene, 1);
		}
		CHECK(source->GetVelocity().z == doctest::Approx(1.0f / timestep).epsilon(0.01));
		CHECK(source->GetVelocity().x == 0.0f);
		StepScene(scene, 3); // Longer than two fixed steps without a move
		CHECK(source->GetVelocity() == glm::vec3(0.0f));
		listener.GetTransform().Translation = glm::vec3(0.0f, 1.0f, -18.0f);
		entity.GetTransform().Translation = glm::vec3(1000.0f, 0.0f, 0.0f);
		StepScene(scene, 1);
		CHECK(source->GetVelocity() == glm::vec3(0.0f));
		CHECK(source->GetPosition() == glm::vec3(1000.0f, 0.0f, 0.0f));

		// Children follow their parents.
		Entity parent = scene.CreateEntity("Parent");
		parent.GetTransform().Translation = glm::vec3(10.0f, 0.0f, 0.0f);
		REQUIRE(scene.SetParent(entity, parent, false));
		StepScene(scene, 1);
		CHECK(source->GetPosition() == glm::vec3(1010.0f, 0.0f, 0.0f));
		// Non-spatial sources are not positioned.
		component.Spatial = false;
		parent.GetTransform().Translation = glm::vec3(20.0f, 0.0f, 0.0f);
		StepScene(scene, 1);
		CHECK(source->GetPosition() == glm::vec3(1010.0f, 0.0f, 0.0f));
	}

	TEST_CASE("Doppler velocities stay steady when frames and fixed steps differ")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClip(1.0f);
		ScopedSceneSystemRegistration moverSystem({ "TestFixedMover", false, [](Scene& scene) { return CreateScope<FixedMoverSystem>(scene); } });

		Scene scene;
		REQUIRE(scene.GetSettings().FixedTimestep == doctest::Approx(1.0f / 60.0f));
		Entity mover = CreateSource(scene, "Mover", clip);
		mover.GetComponent<AudioSourceComponent>().Spatial = true;
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(10.0f, 100.0f, 0.0f));
		box.AddComponent<AudioSourceComponent>().Clip = clip;
		Entity attached = scene.CreateChildEntity(box, "Attached");
		attached.GetTransform().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
		attached.AddComponent<AudioSourceComponent>().Clip = clip;

		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.SetAngularVelocity(box, glm::vec3(0.0f, 2.0f, 0.0f)));
		const AudioSource* moverSource = audio.GetAudioSource(mover);
		REQUIRE(moverSource);

		// At 144 frames per second the mover moves on two or three frames out of five; its velocity still holds steady.
		const float frameTime = 1.0f / 144.0f;
		for (const float speed : { 30.0f, 70.0f })
		{
			FixedMoverSystem::Speed = speed;
			for (int frame = 0; frame < 144; frame++)
			{
				scene.OnUpdateRuntime(frameTime);
				if (frame >= 72)
					CHECK(std::abs(moverSource->GetVelocity().z - speed) < speed * 0.05f);
			}
		}

		// Rigid bodies, and entities below them, use the simulated velocity.
		const glm::vec3 boxVelocity = physics.GetLinearVelocity(box);
		CHECK(boxVelocity.y < -10.0f);
		CHECK(glm::length(audio.GetAudioSource(box)->GetVelocity() - boxVelocity) < 1e-4f);
		const glm::vec3 attachedVelocity = boxVelocity + glm::cross(physics.GetAngularVelocity(box), GetWorldPosition(scene, attached) - GetWorldPosition(scene, box));
		CHECK(glm::length(audio.GetAudioSource(attached)->GetVelocity() - attachedVelocity) < 1e-3f);
		CHECK(glm::length(attachedVelocity - boxVelocity) > 1.0f);

		// A mover that stops has no velocity within two fixed steps.
		FixedMoverSystem::Speed = 0.0f;
		for (int frame = 0; frame < 8; frame++)
			scene.OnUpdateRuntime(frameTime);
		CHECK(moverSource->GetVelocity() == glm::vec3(0.0f));
	}

	TEST_CASE("Level listeners keep +Y as their up vector; rolled and vertical ones pass their own")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;

		Scene scene;
		Entity camera = scene.CreateEntity("Camera");
		camera.AddComponent<CameraComponent>();
		Entity source = CreateSource(scene, "Source", project.AddClip(1.0f));
		source.GetComponent<AudioSourceComponent>().Spatial = true;
		scene.OnRuntimeStart();
		const auto rotate = [](float angle, const glm::vec3& axis) { return glm::angleAxis(angle, axis); };
		const glm::vec3 xAxis(1.0f, 0.0f, 0.0f);
		const glm::vec3 yAxis(0.0f, 1.0f, 0.0f);
		const glm::vec3 zAxis(0.0f, 0.0f, 1.0f);

		// Turning and pitching without rolling: the up vector stays +Y, so it never has to reach the mixing thread anew.
		for (int frame = 0; frame < 10; frame++)
		{
			camera.GetTransform().Rotation = rotate(0.3f * static_cast<float>(frame), yAxis) * rotate(-0.6f, xAxis);
			StepScene(scene, 1);
			const glm::vec3 forward = camera.GetTransform().Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
			const AudioListenerState listener = AudioEngine::GetListener();
			CHECK(listener.Up == yAxis);
			CHECK(glm::length(listener.Forward - forward) < 1e-5f);
		}

		// Rolled: its own up vector.
		camera.GetTransform().Rotation = rotate(0.5f, zAxis);
		StepScene(scene, 1);
		CHECK(glm::length(AudioEngine::GetListener().Up - camera.GetTransform().Rotation * yAxis) < 1e-5f);

		// Looking straight down and turned by 90 degrees, the camera's right is -Z: forward x +Y would give no right axis, so
		// its own up vector is used, and a source on its right is louder on the right.
		camera.GetTransform().Rotation = rotate(std::numbers::pi_v<float> * 0.5f, yAxis) * rotate(-std::numbers::pi_v<float> * 0.5f, xAxis);
		source.GetTransform().Translation = glm::vec3(0.0f, 0.0f, -5.0f);
		StepScene(scene, 1);
		CHECK(glm::length(AudioEngine::GetListener().Up - glm::vec3(-1.0f, 0.0f, 0.0f)) < 1e-5f);
		CHECK(MeasureRms(1) > MeasureRms(0) * 1.5f);
	}

	TEST_CASE("Spatial sources follow entities moved by physics")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;

		Scene scene;
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 10.0f, 0.0f));
		AudioSourceComponent& component = box.AddComponent<AudioSourceComponent>();
		component.Clip = project.AddClip(1.0f);
		component.Loop = true;

		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		StepScene(scene, 30);
		const AudioSource* source = audio.GetAudioSource(box);
		REQUIRE(source);
		const glm::vec3 position = GetWorldPosition(scene, box);
		CHECK(position.y < 9.0f);
		CHECK(source->GetPosition().x == doctest::Approx(position.x));
		CHECK(source->GetPosition().y == doctest::Approx(position.y));
		CHECK(source->GetPosition().z == doctest::Approx(position.z));
		const glm::vec3 velocity = GetPhysics(scene).GetLinearVelocity(box);
		CHECK(velocity.y < -3.0f);
		CHECK(source->GetVelocity().y == doctest::Approx(velocity.y).epsilon(0.02));
	}

	TEST_CASE("The listener is the first active listener in hierarchy order, then the primary camera")
	{
		// Listeners and cameras are created in an order that differs from the hierarchy order both forwards and backwards.
		Scene scene;
		Entity camera = scene.CreateEntity("Camera");
		camera.AddComponent<CameraComponent>();
		Entity holder = scene.CreateEntity("Holder");
		Entity first = scene.CreateChildEntity(holder, "First");
		first.AddComponent<AudioListenerComponent>();
		Entity second = scene.CreateEntity("Second");
		second.AddComponent<AudioListenerComponent>();
		// Earlier in the hierarchy than all of them, but inactive.
		Entity hidden = scene.CreateEntity("Hidden");
		scene.CreateChildEntity(hidden, "HiddenListener").AddComponent<AudioListenerComponent>();
		REQUIRE(scene.SetSiblingIndex(hidden, 0));
		hidden.SetActive(false);

		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		CHECK(audio.GetListenerEntity() == first);

		// Created last, but first in the hierarchy.
		Entity third = scene.CreateEntity("Third");
		third.AddComponent<AudioListenerComponent>();
		REQUIRE(scene.SetSiblingIndex(third, 0));
		StepScene(scene, 1);
		CHECK(audio.GetListenerEntity() == third);
		scene.DestroyEntity(third);
		StepScene(scene, 1);
		CHECK(audio.GetListenerEntity() == first);

		first.GetComponent<AudioListenerComponent>().Active = false;
		StepScene(scene, 1);
		CHECK(audio.GetListenerEntity() == second);

		second.SetActive(false);
		StepScene(scene, 1);
		CHECK(audio.GetListenerEntity() == camera);

		// Several primary cameras: the same one as Scene::GetPrimaryCameraEntity.
		Entity otherCamera = scene.CreateEntity("OtherCamera");
		otherCamera.AddComponent<CameraComponent>();
		REQUIRE(scene.SetSiblingIndex(otherCamera, 0));
		Entity lastCamera = scene.CreateEntity("LastCamera");
		lastCamera.AddComponent<CameraComponent>();
		StepScene(scene, 1);
		CHECK(audio.GetListenerEntity() == otherCamera);
		CHECK(audio.GetListenerEntity() == scene.GetPrimaryCameraEntity());
		otherCamera.GetComponent<CameraComponent>().Primary = false;
		StepScene(scene, 1);
		CHECK(audio.GetListenerEntity() == camera);
		CHECK(audio.GetListenerEntity() == scene.GetPrimaryCameraEntity());

		camera.GetComponent<CameraComponent>().Primary = false;
		lastCamera.GetComponent<CameraComponent>().Primary = false;
		StepScene(scene, 1);
		CHECK_FALSE(audio.GetListenerEntity());

		second.SetActive(true);
		StepScene(scene, 1);
		CHECK(audio.GetListenerEntity() == second);
	}

	TEST_CASE("Sources come and go with their components and entities during play")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClip(1.0f);
		SceneSystemRegistry::Register({ "TestDooming", false, [](Scene& scene) { return CreateScope<DoomingSystem>(scene); } });

		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity entity = scene.CreateChildEntity(parent, "Source");
		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		CHECK(audio.GetStats().SourceCount == 0);
		const auto addSource = [&](Entity target)
		{
			AudioSourceComponent& component = target.AddComponent<AudioSourceComponent>();
			component.Clip = clip;
			component.Loop = true;
			component.Spatial = false;
		};
		const auto checkReleased = [&]()
		{
			CHECK(audio.GetStats().SourceCount == 0);
			CHECK(AudioEngine::GetStats().SourceCount == 0);
			CHECK(ComputeRms(Render(4800)) == 0.0f);
		};

		// Added during play: plays at once.
		addSource(entity);
		StepScene(scene, 1);
		CHECK(audio.GetStats().SourceCount == 1);
		CHECK(audio.IsPlaying(entity));
		CHECK(AudioEngine::GetStats().ActiveVoices == 1);

		// Deactivated (itself or an ancestor): released; reactivated: plays again.
		entity.SetActive(false);
		StepScene(scene, 1);
		checkReleased();
		CHECK_FALSE(audio.IsPlaying(entity));
		CHECK_FALSE(audio.Play(entity));
		entity.SetActive(true);
		StepScene(scene, 1);
		CHECK(audio.IsPlaying(entity));
		parent.SetActive(false);
		StepScene(scene, 1);
		checkReleased();
		parent.SetActive(true);
		StepScene(scene, 1);
		CHECK(audio.IsPlaying(entity));

		// Removed.
		entity.RemoveComponent<AudioSourceComponent>();
		StepScene(scene, 1);
		checkReleased();

		// Destroyed outside an update.
		addSource(entity);
		StepScene(scene, 1);
		CHECK(audio.IsPlaying(entity));
		scene.DestroyEntity(parent);
		StepScene(scene, 1);
		checkReleased();

		// Destroyed during an update (the test system destroys "Doomed"): silent from that update on.
		Entity survivor = scene.CreateEntity("Survivor");
		addSource(survivor);
		StepScene(scene, 1);
		CHECK(audio.GetStats().SourceCount == 1);
		Entity doomed = scene.CreateEntity("Doomed");
		addSource(doomed);
		CHECK(audio.Play(doomed)); // Created on demand
		CHECK(audio.GetStats().SourceCount == 2);
		StepScene(scene, 1);
		CHECK_FALSE(doomed.IsValid());
		CHECK(audio.GetStats().SourceCount == 1);
		CHECK(audio.IsPlaying(survivor));

		scene.OnRuntimeStop();
		SceneSystemRegistry::Unregister("TestDooming");
		CHECK(AudioEngine::GetStats().SourceCount == 0);
	}

	TEST_CASE("Pausing the scene pauses its audio")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClip(2.0f);

		Scene scene;
		Entity music = CreateSource(scene, "Music", clip);
		Entity idle = CreateSource(scene, "Idle", clip);
		idle.GetComponent<AudioSourceComponent>().PlayOnStart = false;
		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		REQUIRE(audio.PlayOneShot(clip));
		Render(4800);

		scene.SetPaused(true);
		const float position = audio.GetPlaybackPosition(music);
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		CHECK(audio.GetPlaybackPosition(music) == position);
		CHECK(audio.IsPlaying(music)); // Paused by the scene, not by the game

		// Sounds requested meanwhile, and steps of the paused scene, stay silent until it resumes.
		CHECK(audio.Play(idle));
		CHECK(audio.PlayOneShot(clip));
		scene.Step(1);
		StepScene(scene, 1);
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		CHECK(audio.GetStats().OneShotCount == 2);
		CHECK(AudioEngine::GetStats().ActiveVoices == 0);

		scene.SetPaused(false);
		CHECK(AudioEngine::GetStats().ActiveVoices == 4); // Music, idle and both one-shots
		Render(4800);
		CHECK(std::abs(audio.GetPlaybackPosition(music) - (position + 0.1f)) < c_PositionTolerance);

		// A source the game paused stays paused when the scene resumes.
		CHECK(audio.Pause(music));
		scene.SetPaused(true);
		scene.SetPaused(false);
		CHECK_FALSE(audio.IsPlaying(music));
		CHECK(audio.IsPlaying(idle));
	}

	TEST_CASE("Gameplay calls control the sources of entities")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClip(1.0f);

		Scene scene;
		Entity entity = CreateSource(scene, "Source", clip, false);
		entity.GetComponent<AudioSourceComponent>().PlayOnStart = false;
		Entity plain = scene.CreateEntity("Plain");
		Entity inactive = CreateSource(scene, "Inactive", clip);
		inactive.SetActive(false);
		Scene other;
		Entity foreign = CreateSource(other, "Foreign", clip);

		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		CHECK_FALSE(audio.IsPlaying(entity));
		CHECK(audio.Play(entity));
		CHECK(audio.IsPlaying(entity));
		Render(4800);
		CHECK(std::abs(audio.GetPlaybackPosition(entity) - 0.1f) < c_PositionTolerance);

		CHECK(audio.Pause(entity));
		CHECK_FALSE(audio.IsPlaying(entity));
		const float paused = audio.GetPlaybackPosition(entity);
		Render(4800);
		CHECK(audio.GetPlaybackPosition(entity) == paused);
		CHECK(audio.Play(entity));
		Render(4800);
		CHECK(std::abs(audio.GetPlaybackPosition(entity) - (paused + 0.1f)) < c_PositionTolerance);

		CHECK(audio.Seek(entity, 0.75f));
		CHECK(std::abs(audio.GetPlaybackPosition(entity) - 0.75f) < c_PositionTolerance);
		CHECK_FALSE(audio.Seek(entity, std::numeric_limits<float>::quiet_NaN()));
		CHECK(audio.Stop(entity));
		CHECK_FALSE(audio.IsPlaying(entity));
		CHECK(audio.GetPlaybackPosition(entity) == 0.0f);
		CHECK(audio.Seek(entity, 0.5f));
		CHECK(audio.Play(entity));
		CHECK(std::abs(audio.GetPlaybackPosition(entity) - 0.5f) < c_PositionTolerance);
		// The non-looping clip ends.
		Render(c_SampleRate);
		CHECK_FALSE(audio.IsPlaying(entity));

		// Entities without an active AudioSourceComponent of this scene.
		for (Entity invalid : { Entity(), plain, inactive, foreign })
		{
			CHECK_FALSE(audio.Play(invalid));
			CHECK_FALSE(audio.Pause(invalid));
			CHECK_FALSE(audio.Stop(invalid));
			CHECK_FALSE(audio.IsPlaying(invalid));
			CHECK_FALSE(audio.Seek(invalid, 0.5f));
			CHECK(audio.GetPlaybackPosition(invalid) == 0.0f);
			CHECK(audio.GetAudioSource(invalid) == nullptr);
		}

		// The master volume is the engine's.
		AudioSystem::SetMasterVolume(0.5f);
		CHECK(AudioEngine::GetMasterVolume() == 0.5f);
		CHECK(AudioSystem::GetMasterVolume() == 0.5f);
	}

	TEST_CASE("One-shots belong to the scene")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle shortClip = project.AddClip(0.1f);
		const AssetHandle mediumClip = project.AddClip(0.25f);
		const AssetHandle longClip = project.AddClip(2.0f);
		const AssetHandle lateClip = project.AddClipFile("Late", 0.5f);
		const AssetHandle slowClip = project.AddClipFile("Slow", 0.5f);

		Scene scene;
		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);

		REQUIRE(audio.PlayOneShot(shortClip));
		CHECK(audio.GetStats().OneShotCount == 1);
		CHECK(AudioEngine::GetStats().ActiveVoices == 1);
		CHECK(ComputeRms(Render(2400)) == doctest::Approx(c_SineRms).epsilon(0.05));
		Render(c_SampleRate / 10);
		StepScene(scene, 1);
		CHECK(audio.GetStats().OneShotCount == 0);
		CHECK(AudioEngine::GetStats().AllocatedVoices == 0);

		// Positional one-shots are attenuated (the listener is at the origin without a listener or camera).
		REQUIRE(audio.PlayOneShotAt(mediumClip, glm::vec3(0.0f, 0.0f, -2.0f)));
		const float nearRms = MeasureRms();
		Render(c_SampleRate / 4);
		StepScene(scene, 1);
		REQUIRE(audio.GetStats().OneShotCount == 0);
		REQUIRE(audio.PlayOneShotAt(mediumClip, glm::vec3(0.0f, 0.0f, -20.0f)));
		CHECK(nearRms / MeasureRms() == doctest::Approx(10.0f).epsilon(0.1));
		Render(c_SampleRate / 4);
		StepScene(scene, 1);

		// A clip that is still loading plays once it is ready...
		REQUIRE(audio.PlayOneShot(lateClip));
		CHECK(audio.GetStats().OneShotCount == 1);
		CHECK(AudioEngine::GetStats().ActiveVoices == 0);
		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 1);
		CHECK(AudioEngine::GetStats().ActiveVoices == 1);
		// ...unless that takes too long: then it is dropped.
		REQUIRE(audio.PlayOneShot(slowClip));
		CHECK(audio.GetStats().OneShotCount == 2);
		StepScene(scene, static_cast<uint32_t>(AudioSystem::c_MaxOneShotClipWait / scene.GetSettings().FixedTimestep) + 2);
		CHECK(audio.GetStats().OneShotCount == 1);
		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 1);
		CHECK(AudioEngine::GetStats().ActiveVoices == 1);

		// Unknown clips and invalid parameters are rejected.
		CHECK_FALSE(audio.PlayOneShot(UUID(0x1234)));
		CHECK_FALSE(audio.PlayOneShot(shortClip, -1.0f));
		CHECK_FALSE(audio.PlayOneShot(shortClip, std::numeric_limits<float>::quiet_NaN()));
		CHECK_FALSE(audio.PlayOneShot(shortClip, 1.0f, 0.0f));
		CHECK_FALSE(audio.PlayOneShotAt(shortClip, glm::vec3(std::numeric_limits<float>::infinity())));
		CHECK(audio.GetStats().OneShotCount == 1);

		// Beyond the limit, the oldest ones are cut off.
		for (uint32_t index = 0; index < AudioSystem::c_MaxOneShots + 10; index++)
			CHECK(audio.PlayOneShot(longClip, 0.01f));
		CHECK(audio.GetStats().OneShotCount == AudioSystem::c_MaxOneShots);
		CHECK(AudioEngine::GetStats().ActiveVoices == AudioSystem::c_MaxOneShots);

		// Stopping the scene releases them.
		scene.OnRuntimeStop();
		CHECK(AudioEngine::GetStats().AllocatedVoices == 0);
	}

	TEST_CASE("Repeated runs of a scene release every voice")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClip(1.0f);
		Ref<Scene> edited = CreateRef<Scene>();
		CreateSource(*edited, "Music", clip);
		Entity spatial = CreateSource(*edited, "Spatial", clip);
		spatial.GetComponent<AudioSourceComponent>().Spatial = true;
		edited->CreateEntity("Listener").AddComponent<AudioListenerComponent>();

		for (int run = 0; run < 25; run++)
		{
			Ref<Scene> running = Scene::Copy(edited);
			running->OnRuntimeStart();
			AudioSystem& audio = GetAudio(*running);
			REQUIRE(audio.PlayOneShot(clip));
			StepScene(*running, 2);
			Render(480);
			CHECK(AudioEngine::GetStats().ActiveVoices == 3);
			running->OnRuntimeStop();
			const AudioStats stats = AudioEngine::GetStats();
			CHECK(stats.SourceCount == 0);
			CHECK(stats.AllocatedVoices == 0);
		}
	}

	TEST_CASE("Scenes run without audio output")
	{
		REQUIRE_FALSE(AudioEngine::IsInitialized());
		AudioProject project;
		const AssetHandle clip = project.AddClip(1.0f);
		Scene scene;
		Entity music = CreateSource(scene, "Music", clip);
		scene.OnRuntimeStart();
		AudioSystem& audio = GetAudio(scene);
		StepScene(scene, 3);
		CHECK(audio.GetStats().SourceCount == 1);
		CHECK_FALSE(audio.IsPlaying(music));
		CHECK_FALSE(audio.Play(music));
		CHECK_FALSE(audio.PlayOneShot(clip));
		CHECK(audio.Stop(music));
		scene.OnRuntimeStop();
		CHECK(AudioEngine::GetStats().SourceCount == 0);
	}
}
