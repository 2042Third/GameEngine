#pragma once

#include "Editor/StreamingView.h"

#include <Strata/Renderer/DebugDraw.h>
#include <Strata/World/WorldStreamer.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Strata
{

	class WorldAsset;

	// The viewport's cell bounds (ViewportSettings::CellBounds): the squares of the cells a world's streamer holds (loading,
	// loaded, unloading) as debug lines over the scene, colored by state (ViewportDebugColors), at the bottom of their
	// cooked bounds (y = 0 without an index), for the cells within the streaming reach of the camera: the largest layer's
	// streaming distance times the world's distance scale and the editor's radius scale, plus its hysteresis. The squares
	// are worked out again only when the streamer's StateVersion or its World asset changed; a frame adds four lines per
	// cell in reach. The viewport calls it only while the setting is on. Main thread.
	class CellBoundsOverlay
	{
	public:
		void Draw(const WorldStreamer& streamer, const glm::vec3& camera, float radiusScale, const ViewportDebugColors& colors, DebugDraw& debugDraw);

		// How often the squares were worked out (tests: a still world works them out once).
		uint64_t GetBuildCount() const { return m_Builds; }
		// The cells the last Draw drew.
		size_t GetDrawnCellCount() const { return m_DrawnCells; }
	private:
		struct Outline
		{
			glm::dvec2 Min { 0.0 };
			glm::dvec2 Max { 0.0 };
			float Y = 0.0f;
			WorldCellStatus Status = WorldCellStatus::Loaded;
		};

		std::vector<Outline> m_Outlines;
		std::vector<WorldActiveCell> m_Cells; // Scratch
		const WorldStreamer* m_Streamer = nullptr;
		std::weak_ptr<WorldAsset> m_World;
		uint64_t m_StateVersion = 0;
		double m_Reach = 0.0; // Before the radius scale
		bool m_Built = false;
		uint64_t m_Builds = 0;
		size_t m_DrawnCells = 0;
	};

}
