#include "Editor/CellBoundsOverlay.h"

#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldGrid.h>

#include <algorithm>

namespace Strata
{

	void CellBoundsOverlay::Draw(const WorldStreamer& streamer, const glm::vec3& camera, float radiusScale, const ViewportDebugColors& colors, DebugDraw& debugDraw)
	{
		m_DrawnCells = 0;
		const Ref<WorldAsset> world = streamer.GetWorldAsset();
		if (!world)
			return;
		const uint64_t stateVersion = streamer.GetStats().StateVersion;
		if (!m_Built || &streamer != m_Streamer || m_World.lock() != world || stateVersion != m_StateVersion)
		{
			m_Streamer = &streamer;
			m_World = world;
			m_StateVersion = stateVersion;
			m_Built = true;
			m_Builds++;

			const WorldData& data = world->GetData();
			m_Reach = 0.0;
			for (const WorldLayer& layer : data.Layers)
			{
				const double radius = static_cast<double>(layer.StreamingDistance) * static_cast<double>(streamer.GetDistanceScale());
				m_Reach = std::max(m_Reach, radius + std::max(static_cast<double>(layer.CellSize) * 0.5, 0.1 * radius));
			}
			m_Outlines.clear();
			streamer.GetActiveCells(m_Cells);
			for (const WorldActiveCell& cell : m_Cells)
			{
				const WorldLayer* layer = data.FindLayer(cell.Key.Layer);
				if (!layer)
					continue;
				const WorldCellSquare square = WorldGrid::CellBounds(*layer, data.GridOrigin, cell.Key.X, cell.Key.Z);
				const WorldCellSummary* summary = world->FindSummary(cell.Key);
				m_Outlines.push_back({ square.Min, square.Max, summary ? summary->BoundsMin.y : 0.0f, cell.Status });
			}
		}

		// The squares within reach of the camera on the ground plane (pinned cells far away stay off the view).
		const double reach = m_Reach * static_cast<double>(std::max(radiusScale, 0.0f));
		const glm::dvec2 eye(camera.x, camera.z);
		for (const Outline& outline : m_Outlines)
		{
			const glm::dvec2 nearest = glm::clamp(eye, outline.Min, outline.Max);
			if (glm::length(nearest - eye) > reach)
				continue;
			const glm::vec4& color = outline.Status == WorldCellStatus::Loading ? colors.CellLoading
				: outline.Status == WorldCellStatus::Unloading ? colors.CellUnloading : colors.CellLoaded;
			const glm::vec3 a(static_cast<float>(outline.Min.x), outline.Y, static_cast<float>(outline.Min.y));
			const glm::vec3 b(static_cast<float>(outline.Max.x), outline.Y, static_cast<float>(outline.Min.y));
			const glm::vec3 c(static_cast<float>(outline.Max.x), outline.Y, static_cast<float>(outline.Max.y));
			const glm::vec3 d(static_cast<float>(outline.Min.x), outline.Y, static_cast<float>(outline.Max.y));
			// Over the scene: a cell's floor would hide its own square.
			debugDraw.Line(a, b, color, DebugDrawDepth::OnTop);
			debugDraw.Line(b, c, color, DebugDrawDepth::OnTop);
			debugDraw.Line(c, d, color, DebugDrawDepth::OnTop);
			debugDraw.Line(d, a, color, DebugDrawDepth::OnTop);
			m_DrawnCells++;
		}
	}

}
