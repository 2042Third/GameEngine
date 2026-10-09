#include "StrataScript/StrataScript.h"

namespace Game
{

	class Enemy : public Strata::Script
	{
	public:
		Strata::Entity Target;
		int32_t Health = 100;
	};

}

ST_SCRIPT_CLASS(Game::Enemy)
{
	ST_SCRIPT_FIELD(Target);
	ST_SCRIPT_FIELD(Health);
}
