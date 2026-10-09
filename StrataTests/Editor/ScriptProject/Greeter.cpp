// The game script of the editor's end-to-end script test (StrataTests/Editor/ScriptsEndToEnd.cmake): the test copies it
// into a project the editor created, builds it with script.build, attaches it with script.add, plays and exports.

#include "StrataScript/StrataScript.h"

#include <string>

using namespace Strata;

// Renames its entity when it starts and says so in the log.
class Greeter : public Script
{
public:
	std::string Greeting = "Hello";

	void OnCreate() override
	{
		GetEntity().SetName(Greeting + " from Greeter");
		Log::Info("Greeter ran on '", GetEntity().GetName(), "'");
	}
};

ST_SCRIPT_CLASS(Greeter)
{
	ST_SCRIPT_FIELD(Greeting);
}
