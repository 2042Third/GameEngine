#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"

namespace Strata
{

	// The audio module's components (registered by RegisterAudioComponents).

	struct AudioSourceComponent
	{
		AssetHandle Clip = UUID::Null();
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		bool PlayOnStart = true;
		bool Spatial = true;
		float MinDistance = 1.0f;
		float MaxDistance = 50.0f;
		float Rolloff = 1.0f;
	};

	// The active listener; falls back to the primary camera when no listener exists.
	struct AudioListenerComponent
	{
		bool Active = true;
	};

}
