// Scripts of the Scripting.Audio tests (StrataTests/src/Scripting/ScriptAudioTests.cpp). They run on "Speaker", whose
// AudioSource component plays the clip in the field Clip (looping, not on start); "Mute" has no AudioSource and "Sleeper"
// is an inactive entity with one.

#include "TestScripts.h"

#include <cstddef>
#include <cstdint>
#include <limits>

using namespace Strata;
using namespace ScriptTests;

namespace
{

	constexpr float c_NaN = std::numeric_limits<float>::quiet_NaN();
	constexpr float c_Infinity = std::numeric_limits<float>::infinity();

}

// Plays the speaker's source and one-shots and changes the master volume, on the first update (the audio system starts
// after the scripts, so the sources do not exist during OnCreate yet).
class AudioAPI : public CheckingScript
{
public:
	AssetHandle Clip;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		AudioSource source = GetEntity().GetAudioSource();
		Expect(source.GetEntity() == GetEntity(), "AudioSource::GetEntity");
		Expect(!source.IsPlaying(), "the source waits for Play");
		Expect(source.Play() && source.IsPlaying(), "Play");
		Expect(source.Seek(0.5f) && Near(source.GetPlaybackPosition(), 0.5f, 0.03f), "Seek");
		Expect(source.Pause() && !source.IsPlaying() && Near(source.GetPlaybackPosition(), 0.5f, 0.03f), "Pause keeps the position");
		Expect(source.Play() && source.IsPlaying(), "Play resumes");
		Expect(source.Stop() && !source.IsPlaying() && source.GetPlaybackPosition() == 0.0f, "Stop rewinds");
		Expect(source.Play(), "play on");

		Expect(Audio::PlayOneShot(Clip), "PlayOneShot");
		Expect(Audio::PlayOneShot(Clip, 0.5f, 2.0f), "PlayOneShot with a volume and pitch");
		Expect(Audio::PlayOneShotAt(Clip, { 1.0f, 2.0f, 3.0f }, 0.25f), "PlayOneShotAt");

		Audio::SetMasterVolume(0.5f);
		Expect(Near(Audio::GetMasterVolume(), 0.5f), "SetMasterVolume");
		Audio::SetMasterVolume(-1.0f);
		Expect(Audio::GetMasterVolume() == 0.0f, "negative master volumes silence the game");
		Audio::SetMasterVolume(1.0f);
	}
};

ST_SCRIPT_CLASS(AudioAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Clip);
	ST_SCRIPT_FIELD(Done);
}

// Calls the audio functions with what they refuse: every call fails harmlessly.
class AudioMisuse : public CheckingScript
{
public:
	AssetHandle Clip;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptContext* context = Detail::GetContext();
		AudioSource mute = Scene::FindEntityByName("Mute").GetAudioSource();
		Expect(!mute.Play() && !mute.IsPlaying() && !mute.Stop() && mute.GetPlaybackPosition() == 0.0f, "an entity without an AudioSource");
		AudioSource sleeper = Scene::FindEntityByName("Sleeper").GetAudioSource();
		Expect(!sleeper.Play() && !sleeper.Pause(), "an inactive entity");
		Expect(!host->AudioStop(context, 0), "the null entity");
		Expect(!host->AudioPause(context, 0x51DE5u), "an entity that does not exist");
		Expect(!AudioSource(Entity()).Play(), "the SDK's null entity");

		AudioSource source = GetEntity().GetAudioSource();
		Expect(!source.Seek(c_NaN) && !source.Seek(c_Infinity), "non-finite positions");
		Expect(source.GetPlaybackPosition() == 0.0f, "refused seeks move nothing");

		Expect(!Audio::PlayOneShot(AssetHandle()), "a null clip");
		Expect(!Audio::PlayOneShot(AssetHandle(0x51DE5u)), "an asset that does not exist");
		Expect(!Audio::PlayOneShot(Clip, -1.0f), "a negative volume");
		Expect(!Audio::PlayOneShot(Clip, c_NaN), "a NaN volume");
		Expect(!Audio::PlayOneShot(Clip, 1.0f, 0.0f), "a zero pitch");
		Expect(!Audio::PlayOneShot(Clip, 1.0f, c_Infinity), "an infinite pitch");
		Expect(!Audio::PlayOneShotAt(Clip, { c_NaN, 0.0f, 0.0f }), "a non-finite position");
		Expect(!host->AudioPlayOneShotAt(context, Clip.ID, nullptr, 1.0f, 1.0f), "no position");

		const float volume = Audio::GetMasterVolume();
		Audio::SetMasterVolume(c_NaN);
		Expect(Audio::GetMasterVolume() == volume, "a NaN master volume is ignored");
	}
};

ST_SCRIPT_CLASS(AudioMisuse)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Clip);
	ST_SCRIPT_FIELD(Done);
}

// Where audio cannot play, every call that would make a sound fails harmlessly. Mode 0: the engine has no audio output
// (sources exist, silent: they can be paused, stopped and moved, but not played); 1: the scene plays without its audio
// system; 2: the engine predates the audio functions (the script uses a host table that ends before them).
class AudioUnavailable : public CheckingScript
{
public:
	AssetHandle Clip;
	int32_t Mode = 0;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		StrataScriptHostAPI older = *Detail::GetHost();
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, AudioPlay));
		const StrataScriptHostAPI* engine = Detail::s_Host;
		if (Mode == 2)
			Detail::s_Host = &older;

		AudioSource source = GetEntity().GetAudioSource();
		const bool played = source.Play() || source.IsPlaying() || Audio::PlayOneShot(Clip) || Audio::PlayOneShotAt(Clip, glm::vec3(0.0f));
		const bool controlled = source.Pause() && source.Stop() && source.Seek(0.05f);
		const float position = source.GetPlaybackPosition();
		Audio::SetMasterVolume(0.25f);
		const float masterVolume = Audio::GetMasterVolume();
		Detail::s_Host = engine;

		Expect(!played, "nothing plays");
		// Silent sources still exist without audio output.
		Expect(Mode == 0 ? controlled && Near(position, 0.05f) : !controlled && position == 0.0f, "source control");
		// The master volume belongs to the engine, not to the scene: only an engine without the functions ignores it.
		Expect(Mode == 2 ? masterVolume == 1.0f && Audio::GetMasterVolume() == 1.0f : masterVolume == 0.25f, "the master volume");
		Audio::SetMasterVolume(1.0f);
	}
};

ST_SCRIPT_CLASS(AudioUnavailable)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Clip);
	ST_SCRIPT_FIELD(Mode);
	ST_SCRIPT_FIELD(Done);
}