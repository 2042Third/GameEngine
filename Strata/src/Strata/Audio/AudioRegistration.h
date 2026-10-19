#pragma once

namespace Strata
{

	// Registers the audio module with the engine's registries: its components, the audio clip loader and the "Audio" scene
	// system. Called by Engine::RegisterBuiltinModules only.
	void RegisterAudioModule();

	// Registers the audio components (AudioSource, AudioListener: AudioComponents.cpp); part of RegisterAudioModule.
	void RegisterAudioComponents();

}
