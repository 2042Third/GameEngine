// The audio API of scripts (AudioSource, Audio) on "Speaker": an AudioSource with the Blip clip (0.1 s, looping, not
// played on start).

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

class AudioFeatures : public FeatureScript
{
public:
	static constexpr int32_t c_CheckFrame = 30;

	int32_t Frames = 0;

	void OnCreate() override
	{
		Journal(*this, "AudioFeatures", "OnCreate");
		// One-shots of clips that are not loaded soon enough are dropped: games request their clips early.
		Expect(Assets::RequestLoad(GetClip()), "request the clip");
	}

	void OnUpdate(float) override
	{
		if (Completed)
			return;

		// The audio system starts after the scripts: the sources exist from the first update on.
		AudioSource source = GetEntity().GetAudioSource();
		if (Frames++ == 0)
		{
			Expect(source.GetEntity() == GetEntity(), "AudioSource::GetEntity");
			Expect(!source.IsPlaying(), "the speaker does not play on start");
			Expect(source.Play() && source.IsPlaying(), "AudioSource::Play");
			Expect(source.Seek(0.05f) && Near(source.GetPlaybackPosition(), 0.05f, 0.01f), "AudioSource::Seek");
			Expect(source.Pause() && !source.IsPlaying() && Near(source.GetPlaybackPosition(), 0.05f, 0.01f), "AudioSource::Pause keeps the position");
			Expect(source.Stop() && !source.IsPlaying() && source.GetPlaybackPosition() == 0.0f, "AudioSource::Stop rewinds");
			Expect(source.Play(), "the speaker plays on");

			Expect(Audio::PlayOneShot(GetClip()) && Audio::PlayOneShot(GetClip(), 0.5f, 2.0f), "Audio::PlayOneShot");
			Expect(Audio::PlayOneShotAt(GetClip(), GetTransform().GetWorldPosition(), 0.75f), "Audio::PlayOneShotAt");

			const float volume = Audio::GetMasterVolume();
			Audio::SetMasterVolume(0.5f);
			Expect(Near(Audio::GetMasterVolume(), 0.5f), "Audio::SetMasterVolume");
			Audio::SetMasterVolume(volume);
		}
		else if (Frames == c_CheckFrame)
		{
			Expect(source.IsPlaying(), "the looping speaker keeps playing");
			Completed = true;
		}
	}
private:
	AssetHandle GetClip() const
	{
		return GetEntity().GetProperty<AssetHandle>("AudioSource", "Clip").value_or(AssetHandle());
	}
};

ST_SCRIPT_CLASS(AudioFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Frames);
}
