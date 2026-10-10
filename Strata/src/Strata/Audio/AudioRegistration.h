#pragma once

namespace Strata
{

	// Registers the audio module with the engine's registries: the audio clip loader and the "Audio" scene system.
	// Called by Engine::RegisterBuiltinModules only.
	void RegisterAudioModule();

}
