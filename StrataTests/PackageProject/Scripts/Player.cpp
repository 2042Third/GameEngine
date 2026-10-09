#include "StrataScript/StrataScript.h"

class Player : public Strata::Script
{
public:
	float Speed = 5.0f;

	void OnUpdate(float deltaTime) override
	{
		Strata::TransformComponent transform = GetTransform();
		if (Strata::Input::IsKeyDown(Strata::Key::W))
			transform.SetTranslation(transform.GetTranslation() + transform.GetForward() * Speed * deltaTime);
	}
};

ST_SCRIPT_CLASS(Player)
{
	ST_SCRIPT_FIELD(Speed);
}
