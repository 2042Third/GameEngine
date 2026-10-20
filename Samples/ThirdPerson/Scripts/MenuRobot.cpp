// The robot of the main menu, on the model with the Animator: it dances, waves (holding the flag that follows its hand
// through an AttachToJoint component), and dances again, through its controller's Dance and Wave triggers.

#include "StrataScript/StrataScript.h"

using namespace Strata;

class MenuRobot : public Script
{
public:
	float DanceSeconds = 6.5f;
	float WaveSeconds = 2.4f;        // The wave clip, and the blend back to idle

	void OnCreate() override
	{
		m_Time = 0.0f;
		m_Waving = false;
		GetEntity().GetAnimator().SetTrigger("Dance");
	}

	void OnUpdate(float deltaTime) override
	{
		m_Time += deltaTime;
		Animator animator = GetEntity().GetAnimator();
		if (!m_Waving && m_Time >= DanceSeconds)
		{
			m_Waving = true;
			m_Time = 0.0f;
			animator.SetTrigger("Wave");
		}
		else if (m_Waving && m_Time >= WaveSeconds)
		{
			m_Waving = false;
			m_Time = 0.0f;
			animator.SetTrigger("Dance");
		}
	}
private:
	float m_Time = 0.0f;
	bool m_Waving = false;
};

ST_SCRIPT_CLASS(MenuRobot)
{
	ST_SCRIPT_FIELD(DanceSeconds);
	ST_SCRIPT_FIELD(WaveSeconds);
}
