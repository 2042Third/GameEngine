// Interpolation: what the fixed steps move is drawn between its poses before and after the latest stepping frame's steps
// (Time::GetStepAlpha, TransformComponent::GetDrawnPosition, GetDrawnRotation, ResetInterpolation).

#include "FeatureScript.h"

#include <cmath>

using namespace Strata;
using namespace FeatureTest;

// Moves itself 0.1 m and 5 degrees each fixed step. The in-process runners interpolate (the project's Scene.Timing keeps
// Interpolation on), the executables' scripted runs do not: either way the drawn pose is the blend of the last stepping
// frame's poses at the frame's step alpha, which is 1 (the world pose) while nothing interpolates.
class InterpolationFeatures : public FeatureScript
{
public:
	static constexpr int32_t c_ResetFrame = 120; // After the hot reload at frame 100

	int32_t BetweenSteps = 0;                       // Frames drawn between two steps' poses (alpha below 1)
	bool Stepped = false;                           // A frame ran fixed steps
	glm::vec3 BeforeSteps = glm::vec3(0.0f);        // The world position in this frame's OnUpdate, before its steps
	glm::quat BeforeStepsRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::vec3 From = glm::vec3(0.0f);               // The latest stepping frame's poses before and after its steps
	glm::quat FromRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::vec3 To = glm::vec3(0.0f);
	glm::quat ToRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

	void OnCreate() override
	{
		Journal(*this, "InterpolationFeatures", "OnCreate");
	}

	void OnUpdate(float) override
	{
		m_Steps = 0;
		BeforeSteps = GetTransform().GetWorldPosition();
		BeforeStepsRotation = GetTransform().GetWorldRotation();
	}

	void OnFixedUpdate(float) override
	{
		TransformComponent transform = GetTransform();
		transform.SetTranslation(transform.GetTranslation() + glm::vec3(0.1f, 0.0f, 0.0f));
		transform.SetRotation(glm::angleAxis(glm::radians(5.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * transform.GetRotation());
		m_Steps++;
	}

	void OnLateUpdate(float) override
	{
		TransformComponent transform = GetTransform();
		if (m_Steps > 0)
		{
			Stepped = true;
			From = BeforeSteps;
			FromRotation = BeforeStepsRotation;
			To = transform.GetWorldPosition();
			ToRotation = transform.GetWorldRotation();
		}
		const float alpha = Time::GetStepAlpha();
		Expect(alpha >= 0.0f && alpha <= 1.0f, "the step alpha is a fraction");
		const glm::vec3 drawn = transform.GetDrawnPosition();
		const glm::quat drawnRotation = transform.GetDrawnRotation();
		if (!Stepped || alpha >= 1.0f)
		{
			Expect(Near(drawn, transform.GetWorldPosition()) && Near(drawnRotation, transform.GetWorldRotation(), 1e-3f),
				"without a blend the drawn pose is the world pose");
		}
		else
		{
			BetweenSteps++;
			// The engine's blend: the translation along its segment, the rotation by a normalized lerp along the shorter arc.
			const glm::vec3 expected = From + (To - From) * alpha;
			const glm::quat target = glm::dot(FromRotation, ToRotation) < 0.0f ? -ToRotation : ToRotation;
			const glm::quat expectedRotation = glm::normalize(FromRotation * (1.0f - alpha) + target * alpha);
			Expect(Near(drawn, expected) && Near(drawnRotation, expectedRotation, 1e-3f), "the drawn pose lies between the last two steps' poses");
		}

		if (GetFrame() == c_ResetFrame)
		{
			Expect(transform.ResetInterpolation(), "ResetInterpolation");
			Expect(Near(transform.GetDrawnPosition(), transform.GetWorldPosition()), "a reset draws the world pose");
			Completed = Stepped;
		}
	}
private:
	int32_t m_Steps = 0; // Fixed steps of this frame so far
};

ST_SCRIPT_CLASS(InterpolationFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(BetweenSteps);
	ST_SCRIPT_FIELD(Stepped);
	ST_SCRIPT_FIELD(BeforeSteps);
	ST_SCRIPT_FIELD(BeforeStepsRotation);
	ST_SCRIPT_FIELD(From);
	ST_SCRIPT_FIELD(FromRotation);
	ST_SCRIPT_FIELD(To);
	ST_SCRIPT_FIELD(ToRotation);
}
