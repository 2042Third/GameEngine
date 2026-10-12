#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <atomic>
#include <optional>

namespace Strata
{

	// How far the reads of scene documents under way on the loading workers got, by scene asset (SceneAsset::Deserialize:
	// parsing the document, creating its entities, preparing the scene). Reading a large scene takes seconds (a million
	// entities: about 7), so SceneLoadRequest's progress counts the read's share of it instead of standing still until
	// the scene is prepared. Thread-safe.
	class SceneReadProgress
	{
	public:
		// One read of a scene asset's document, registered for the lifetime of the object (on the thread that reads), with
		// the share of the read done so far.
		class Tracker
		{
		public:
			explicit Tracker(AssetHandle scene);
			~Tracker();

			Tracker(const Tracker&) = delete;
			Tracker& operator=(const Tracker&) = delete;

			// The share done, in [0, 1]; a smaller share than one reported before changes nothing.
			void Report(float share);
			float GetShare() const { return m_Share.load(std::memory_order_relaxed); }
		private:
			AssetHandle m_Scene;
			std::atomic<float> m_Share { 0.0f };
		};

		// The share of the scene asset's document read so far by the furthest of the reads of it under way, or nullopt while
		// none is (not started yet, or finished).
		static std::optional<float> Get(AssetHandle scene);
	};

}
