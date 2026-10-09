#pragma once

// Strata script SDK. Game scripts include this header only; it builds on the C ABI in ScriptABI.h and never touches
// engine headers. See Script.h for the script lifecycle and AGENTS.md ("Scripting") for how modules are built.

#include "StrataScript/ScriptABI.h"

#include "StrataScript/Audio.h"
#include "StrataScript/Entity.h"
#include "StrataScript/Game.h"
#include "StrataScript/Gameplay.h"
#include "StrataScript/Host.h"
#include "StrataScript/Input.h"
#include "StrataScript/Log.h"
#include "StrataScript/Physics.h"
#include "StrataScript/Scene.h"
#include "StrataScript/Script.h"
#include "StrataScript/Time.h"
#include "StrataScript/Value.h"
