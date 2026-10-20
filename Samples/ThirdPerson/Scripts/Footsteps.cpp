// The robot's footsteps, on the model with the Animator: the walk and run states of its controller raise a "Footstep"
// event where a foot touches the ground, and each plays one of the step sounds at the robot's position, at a slightly
// different pitch every time.

#include "StrataScript/StrataScript.h"

#include <array>
#include <cstdint>

using namespace Strata;

class Footsteps : public Script
{
public:
	AssetHandle Step1;
	AssetHandle Step2;
	AssetHandle Step3;
	AssetHandle Step4;
	float Volume = 0.45f;
	int32_t Steps = 0; // Footsteps played

	void OnCreate() override
	{
		for (const AssetHandle clip : GetClips())
			Assets::RequestLoad(clip);
	}

	void OnAnimationEvent(std::string_view name) override
	{
		if (name != "Footstep")
			return;
		const std::array<AssetHandle, 4> clips = GetClips();
		const AssetHandle clip = clips[static_cast<size_t>(m_Random.Range(0, 3))];
		if (clip.IsValid())
			Audio::PlayOneShotAt(clip, GetTransform().GetWorldPosition(), Volume, m_Random.Range(0.9f, 1.1f));
		Steps++;
	}
private:
	std::array<AssetHandle, 4> GetClips() const { return { Step1, Step2, Step3, Step4 }; }
private:
	Random m_Random { 20261020 };
};

ST_SCRIPT_CLASS(Footsteps)
{
	ST_SCRIPT_FIELD(Step1);
	ST_SCRIPT_FIELD(Step2);
	ST_SCRIPT_FIELD(Step3);
	ST_SCRIPT_FIELD(Step4);
	ST_SCRIPT_FIELD(Volume);
	ST_SCRIPT_FIELD(Steps);
}
