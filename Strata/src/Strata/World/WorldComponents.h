#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <cstdint>
#include <string>

namespace Strata
{

	// Makes a scene a world's persistent scene: while it runs, the World system streams the world's cells around the
	// scene's streaming sources. The first active one in hierarchy order counts.
	struct WorldComponent
	{
		AssetHandle World = UUID::Null(); // A World asset
		// Multiplies every layer's streaming distance (0.1-10): a quality knob, lower on devices with less memory.
		float DistanceScale = 1.0f;
	};

	// A point the world's cells stream around (a player, a vehicle). With no active source, the primary camera is one.
	struct StreamingSourceComponent
	{
		bool Enabled = true;
		float Radius = 0.0f;     // 0: each layer's streaming distance; else this radius on every layer (0 to 1,000,000)
		std::string Layers;      // Comma-separated layer names this source streams; empty: every enabled layer
		int32_t Priority = 0;    // -100 to 100: higher-priority sources' cells load first
		float ViewBias = 0.25f;  // 0-1: cells ahead of its forward axis (-Z) rank as if this share of the radius nearer
	};

}
