#include "UI/CellMap.h"

#include "Editor/EditorWorld.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/World/WorldAsset.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Strata
{

	namespace
	{

		// The legend's lines, in this order.
		enum class LegendState : uint8_t
		{
			Unloaded = 0,
			Wanted,
			Loading,
			Loaded,
			Unloading,
			Failed
		};

		constexpr const char* c_LegendLabels[] = { "Unloaded", "Wanted", "Loading", "Loaded", "Unloading", "Failed" };
		// Zoom limits, in pixels per world unit: a 20 km world fits in a few hundred pixels, a 1 m cell fills a panel.
		constexpr double c_MinScale = 1e-3;
		constexpr double c_MaxScale = 256.0;
		constexpr double c_WheelZoom = 1.25;
		// The view frames the sources' largest radius this many times across (the circles with a margin).
		constexpr double c_FrameDiameters = 2.4;

		LegendState GetLegendState(const CellMap::Cell& cell)
		{
			switch (cell.Status)
			{
				case WorldCellStatus::Loading: return LegendState::Loading;
				case WorldCellStatus::Loaded: return LegendState::Loaded;
				case WorldCellStatus::Unloading: return LegendState::Unloading;
				case WorldCellStatus::Failed: return LegendState::Failed;
				case WorldCellStatus::Unloaded: break;
			}
			return cell.Wanted ? LegendState::Wanted : LegendState::Unloaded;
		}

		ImVec4 GetLegendColor(LegendState state)
		{
			const UI::ThemeColors& colors = UI::GetThemeColors();
			switch (state)
			{
				case LegendState::Unloaded: return colors.Control;
				case LegendState::Wanted: return ImVec4(colors.TextSecondary.x, colors.TextSecondary.y, colors.TextSecondary.z, 0.45f);
				case LegendState::Loading: return colors.Warning;
				case LegendState::Loaded: return colors.Accent;
				case LegendState::Unloading: return colors.TextSecondary;
				case LegendState::Failed: return colors.Error;
			}
			return colors.Control;
		}

		// The radius a source streams `layer` within (its own, or the layer's distance times the world's scale).
		double GetSourceRadius(const WorldStreamingSource& source, const WorldLayer& layer, float distanceScale)
		{
			return source.Radius > 0.0f ? static_cast<double>(source.Radius) : static_cast<double>(layer.StreamingDistance) * static_cast<double>(distanceScale);
		}

	}

	bool CellMap::Update(const WorldStreamer& streamer, const EditorWorld* world)
	{
		const Ref<WorldAsset> asset = streamer.GetWorldAsset();
		const uint64_t stateVersion = streamer.GetStats().StateVersion;
		const uint64_t dirtyVersion = world ? world->GetDirtyVersion() : 0;
		if (m_HasCells && &streamer == m_Streamer && m_WorldAsset.lock() == asset && stateVersion == m_StateVersion && world == m_DirtyWorld
			&& dirtyVersion == m_DirtyVersion)
			return false;

		// Another streamer or world (play started or stopped, another world opened): frame its sources anew.
		if (&streamer != m_Streamer || streamer.GetWorld() != m_World)
		{
			m_Scale = 0.0;
			m_Selected.reset();
			m_ContextCell.reset();
			if (streamer.GetWorld() != m_World)
				m_ShownLayer.reset();
		}
		m_Streamer = &streamer;
		m_World = streamer.GetWorld();
		m_WorldAsset = asset;
		m_StateVersion = stateVersion;
		m_DirtyWorld = world;
		m_DirtyVersion = dirtyVersion;
		m_HasCells = true;
		m_Rebuilds++;

		m_Cells.clear();
		m_CellIndex.clear();
		m_StateCounts = {};
		if (asset)
		{
			const WorldData& data = asset->GetData();
			if (m_ShownLayer && *m_ShownLayer >= data.Layers.size())
				m_ShownLayer.reset();
			streamer.GetCellStates(m_States);
			m_Cells.reserve(m_States.size());
			for (WorldCellState& state : m_States)
			{
				const auto layer = std::find_if(data.Layers.begin(), data.Layers.end(), [&](const WorldLayer& item) { return item.ID == state.Key.Layer; });
				if (layer == data.Layers.end())
					continue;
				Cell cell;
				cell.Key = state.Key;
				cell.Scene = state.Scene;
				cell.LayerIndex = static_cast<size_t>(layer - data.Layers.begin());
				cell.Status = state.Status;
				cell.Wanted = state.Wanted;
				cell.Pinned = state.Pinned;
				cell.Dirty = world && world->IsCellDirty(state.Scene);
				cell.Entities = state.Entities;
				if (const WorldCellSummary* summary = asset->FindSummary(state.Key))
					cell.Bodies = summary->Bodies;
				cell.Error = std::move(state.Error);
				m_StateCounts[static_cast<size_t>(GetLegendState(cell))]++;
				m_CellIndex.emplace(cell.Key.Pack(), m_Cells.size());
				m_Cells.push_back(std::move(cell));
			}
		}
		m_Legend.clear();
		for (size_t state = 0; state < m_StateCounts.size(); state++)
			m_Legend.push_back({ GetLegendColor(static_cast<LegendState>(state)), c_LegendLabels[state], fmt::format("{}", m_StateCounts[state]) });
		return true;
	}

	void CellMap::SetView(const glm::dvec2& center, double scale)
	{
		m_Center = center;
		m_Scale = std::clamp(scale, c_MinScale, c_MaxScale);
	}

	const CellMap::Cell* CellMap::FindCell(const WorldCellKey& key) const
	{
		const auto found = m_CellIndex.find(key.Pack());
		return found != m_CellIndex.end() ? &m_Cells[found->second] : nullptr;
	}

	const CellMap::Cell* CellMap::FindCellAt(const WorldAsset& world, const glm::dvec2& point) const
	{
		const WorldData& data = world.GetData();
		const glm::dvec3 position(point.x, 0.0, point.y);
		if (m_ShownLayer)
			return FindCell(WorldGrid::CellAt(data.Layers[*m_ShownLayer], data.GridOrigin, position));
		// The finest layer's cell is drawn on top.
		const Cell* found = nullptr;
		float foundSize = std::numeric_limits<float>::max();
		for (const WorldLayer& layer : data.Layers)
		{
			const Cell* cell = FindCell(WorldGrid::CellAt(layer, data.GridOrigin, position));
			if (cell && layer.CellSize < foundSize)
			{
				found = cell;
				foundSize = layer.CellSize;
			}
		}
		return found;
	}

	ImVec2 CellMap::ToScreen(const glm::dvec2& point, const ImVec2& min, const ImVec2& max) const
	{
		return ImVec2(static_cast<float>((min.x + max.x) * 0.5 + (point.x - m_Center.x) * m_Scale),
			static_cast<float>((min.y + max.y) * 0.5 + (point.y - m_Center.y) * m_Scale));
	}

	glm::dvec2 CellMap::ToGround(const ImVec2& point, const ImVec2& min, const ImVec2& max) const
	{
		return glm::dvec2(m_Center.x + (static_cast<double>(point.x) - (min.x + max.x) * 0.5) / m_Scale,
			m_Center.y + (static_cast<double>(point.y) - (min.y + max.y) * 0.5) / m_Scale);
	}

	void CellMap::FrameSources(const WorldStreamer& streamer, const WorldAsset& world, const ImVec2& size)
	{
		const WorldData& data = world.GetData();
		const std::vector<WorldStreamingSource>& sources = streamer.GetSources();
		// A wide, short map (a bottom dock) frames by the mean of its sides: the circles reach past its top and bottom, but the
		// cells near the sources stay large enough to read.
		const double side = std::max(std::sqrt(static_cast<double>(size.x) * static_cast<double>(size.y)), 1.0);
		if (!sources.empty())
		{
			glm::dvec2 center(0.0);
			double radius = 0.0;
			for (const WorldStreamingSource& source : sources)
			{
				center += glm::dvec2(source.Position.x, source.Position.z);
				for (size_t layer = 0; layer < data.Layers.size(); layer++)
				{
					if (!m_ShownLayer || *m_ShownLayer == layer)
						radius = std::max(radius, GetSourceRadius(source, data.Layers[layer], streamer.GetDistanceScale()));
				}
			}
			center /= static_cast<double>(sources.size());
			SetView(center, side / std::max(radius * c_FrameDiameters, 1.0));
			return;
		}
		// No source yet: every cell.
		glm::dvec2 lower(std::numeric_limits<double>::max());
		glm::dvec2 upper(std::numeric_limits<double>::lowest());
		for (const Cell& cell : m_Cells)
		{
			const WorldCellSquare square = WorldGrid::CellBounds(data.Layers[cell.LayerIndex], data.GridOrigin, cell.Key.X, cell.Key.Z);
			lower = glm::min(lower, square.Min);
			upper = glm::max(upper, square.Max);
		}
		if (m_Cells.empty())
		{
			const double cellSize = data.Layers.empty() ? 64.0 : static_cast<double>(data.Layers.front().CellSize);
			SetView(data.GridOrigin, side / (cellSize * 4.0));
			return;
		}
		const glm::dvec2 extent = upper - lower;
		SetView((lower + upper) * 0.5, side / std::max(std::max(extent.x, extent.y) * 1.1, 1.0));
	}

	std::optional<CellMap::Request> CellMap::Draw(const WorldStreamer& streamer, float height, bool editable)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<WorldAsset> world = streamer.GetWorldAsset();
		if (!world)
		{
			ImGui::TextDisabled("The world's asset is loading");
			m_DrawnCells = 0;
			return std::nullopt;
		}
		DrawChips(*world);

		// As tall as the space left allows (with the legend's line under it), between c_MinHeightInFontSizes and `height`: a
		// short bottom dock still shows it whole.
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const float minHeight = ImGui::GetFontSize() * c_MinHeightInFontSizes;
		const float legendHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
		const ImVec2 size(std::max(available.x, ImGui::GetFontSize() * 4.0f), std::clamp(available.y - legendHeight, minHeight, std::max(height, minHeight)));
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 max(min.x + size.x, min.y + size.y);
		if (m_Scale <= 0.0)
			FrameSources(streamer, *world, size);
		ImGui::InvisibleButton("##CellMap", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		UI::ItemProbe::Record("Streaming.CellMap");
		const bool hovered = ImGui::IsItemHovered();
		const ImGuiIO& io = ImGui::GetIO();

		// Drag pans, the wheel zooms around the mouse, a double click frames the sources.
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			m_Center -= glm::dvec2(io.MouseDelta.x, io.MouseDelta.y) / m_Scale;
		if (hovered && io.MouseWheel != 0.0f)
		{
			const glm::dvec2 anchor = ToGround(io.MousePos, min, max);
			m_Scale = std::clamp(m_Scale * std::pow(c_WheelZoom, static_cast<double>(io.MouseWheel)), c_MinScale, c_MaxScale);
			m_Center += anchor - ToGround(io.MousePos, min, max);
		}
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			FrameSources(streamer, *world, size);
		const Cell* hoveredCell = hovered ? FindCellAt(*world, ToGround(io.MousePos, min, max)) : nullptr;
		// A click (a release without a drag) selects the cell under the mouse, or nothing.
		const float clickSlop = ImGui::GetFontSize() * 0.25f;
		if (ImGui::IsItemDeactivated() && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= clickSlop * clickSlop)
			m_Selected = hoveredCell ? std::optional<WorldCellKey>(hoveredCell->Key) : std::nullopt;
		if (hoveredCell && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
		{
			m_ContextCell = hoveredCell->Key;
			m_Selected = hoveredCell->Key;
			ImGui::OpenPopup("CellMapContext");
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(min, max, true);
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(colors.Panel));
		m_DrawnCells = 0;
		if (m_ShownLayer)
		{
			DrawCells(drawList, *world, min, max, *m_ShownLayer);
		}
		else
		{
			// Coarse layers first, so the finer ones lie on top.
			std::vector<size_t> order(world->GetData().Layers.size());
			for (size_t index = 0; index < order.size(); index++)
				order[index] = index;
			std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return world->GetData().Layers[a].CellSize > world->GetData().Layers[b].CellSize; });
			for (const size_t layer : order)
				DrawCells(drawList, *world, min, max, layer);
		}
		DrawSources(drawList, streamer, *world, min, max);
		drawList->PopClipRect();
		drawList->AddRect(min, max, ImGui::GetColorU32(colors.Border));

		if (hoveredCell && !ImGui::IsPopupOpen("CellMapContext"))
			DrawTooltip(*hoveredCell, *world);
		const std::optional<Request> request = DrawContextMenu(editable);
		DrawLegend();
		return request;
	}

	void CellMap::DrawChips(const WorldAsset& world)
	{
		const WorldData& data = world.GetData();
		const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
		const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		bool all = !m_ShownLayer;
		if (UI::ToggleChip("CellMap.Layer.All", Icons::Layers, "All", &all, "Every layer, overlaid (the finer cells on top)"))
			m_ShownLayer.reset();
		for (size_t layer = 0; layer < data.Layers.size(); layer++)
		{
			// The chips wrap to the next line rather than run out of a narrow panel.
			const char* name = data.Layers[layer].Name.c_str();
			if (ImGui::GetItemRectMax().x + spacing + UI::ChipWidth(Icons::Grid2X2, name) <= right)
				ImGui::SameLine(0.0f, spacing);
			bool shown = m_ShownLayer == layer;
			ImGui::PushID(static_cast<int>(layer));
			const std::string id = std::string("CellMap.Layer.") + name;
			if (UI::ToggleChip(id.c_str(), Icons::Grid2X2, name, &shown, "Only this layer's cells"))
				m_ShownLayer = layer;
			ImGui::PopID();
		}
	}

	void CellMap::DrawCells(ImDrawList* drawList, const WorldAsset& world, const ImVec2& min, const ImVec2& max, size_t layerIndex)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const WorldData& data = world.GetData();
		const WorldLayer& layer = data.Layers[layerIndex];
		const double cellPixels = static_cast<double>(layer.CellSize) * m_Scale;
		// A gap between cells while they are large enough to show one; outlines and pins need room too.
		const float inset = cellPixels >= 6.0 ? 1.0f : 0.0f;
		const float outline = std::max(1.0f, ImGui::GetFontSize() * 0.12f);
		const bool roomForPin = cellPixels >= ImGui::GetFontSize() * 1.2;

		auto drawCell = [&](const Cell& cell)
		{
			const WorldCellSquare square = WorldGrid::CellBounds(layer, data.GridOrigin, cell.Key.X, cell.Key.Z);
			const ImVec2 a = ToScreen(square.Min, min, max);
			const ImVec2 b = ToScreen(square.Max, min, max);
			if (b.x < min.x || a.x > max.x || b.y < min.y || a.y > max.y)
				return;
			m_DrawnCells++;
			const ImVec2 lower(a.x + inset, a.y + inset);
			const ImVec2 upper(std::max(b.x - inset, lower.x + 1.0f), std::max(b.y - inset, lower.y + 1.0f));
			drawList->AddRectFilled(lower, upper, ImGui::GetColorU32(GetLegendColor(GetLegendState(cell))));
			if (cell.Dirty)
				drawList->AddRect(lower, upper, ImGui::GetColorU32(colors.Warning), 0.0f, outline);
			if (m_Selected && *m_Selected == cell.Key)
				drawList->AddRect(ImVec2(lower.x - outline, lower.y - outline), ImVec2(upper.x + outline, upper.y + outline), ImGui::GetColorU32(colors.Text), 0.0f, outline);
			if (cell.Pinned)
			{
				if (roomForPin)
				{
					const ImVec2 glyph = ImGui::CalcTextSize(Icons::MapPin);
					drawList->AddText(ImVec2((lower.x + upper.x - glyph.x) * 0.5f, (lower.y + upper.y - glyph.y) * 0.5f), ImGui::GetColorU32(colors.TextOnAccent), Icons::MapPin);
				}
				else
				{
					drawList->AddCircleFilled(ImVec2((lower.x + upper.x) * 0.5f, (lower.y + upper.y) * 0.5f), outline, ImGui::GetColorU32(colors.TextOnAccent));
				}
			}
		};

		// Walk the squares in view when there are fewer of them than cells of the layer; otherwise the cells, culled.
		const glm::dvec2 viewMin = ToGround(min, min, max);
		const glm::dvec2 viewMax = ToGround(max, min, max);
		const WorldCellKey first = WorldGrid::CellAt(layer, data.GridOrigin, glm::dvec3(viewMin.x, 0.0, viewMin.y));
		const WorldCellKey last = WorldGrid::CellAt(layer, data.GridOrigin, glm::dvec3(viewMax.x, 0.0, viewMax.y));
		const double squares = (static_cast<double>(last.X) - first.X + 1.0) * (static_cast<double>(last.Z) - first.Z + 1.0);
		if (squares <= static_cast<double>(m_Cells.size()))
		{
			for (int32_t z = first.Z; z <= last.Z; z++)
			{
				for (int32_t x = first.X; x <= last.X; x++)
				{
					if (const Cell* cell = FindCell(WorldCellKey { layer.ID, x, z }))
						drawCell(*cell);
				}
			}
			return;
		}
		for (const Cell& cell : m_Cells)
		{
			if (cell.LayerIndex == layerIndex)
				drawCell(cell);
		}
	}

	void CellMap::DrawSources(ImDrawList* drawList, const WorldStreamer& streamer, const WorldAsset& world, const ImVec2& min, const ImVec2& max) const
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const WorldData& data = world.GetData();
		const float dot = ImGui::GetFontSize() * 0.3f;
		const float facing = ImGui::GetFontSize() * 1.4f;
		const float thickness = std::max(1.0f, ImGui::GetFontSize() * 0.1f);
		for (const WorldStreamingSource& source : streamer.GetSources())
		{
			const ImVec2 center = ToScreen(glm::dvec2(source.Position.x, source.Position.z), min, max);
			double radius = 0.0;
			for (size_t layer = 0; layer < data.Layers.size(); layer++)
			{
				if ((!m_ShownLayer || *m_ShownLayer == layer) && (source.LayerMask & (1u << layer)) != 0)
					radius = std::max(radius, GetSourceRadius(source, data.Layers[layer], streamer.GetDistanceScale()));
			}
			if (radius > 0.0)
				drawList->AddCircle(center, static_cast<float>(radius * m_Scale), ImGui::GetColorU32(colors.Info), 0, thickness);
			const glm::vec2 forward(source.Forward.x, source.Forward.z);
			if (glm::length(forward) > 1e-4f)
			{
				const glm::vec2 direction = glm::normalize(forward);
				drawList->AddLine(center, ImVec2(center.x + direction.x * facing, center.y + direction.y * facing), ImGui::GetColorU32(colors.Text), thickness * 2.0f);
			}
			drawList->AddCircleFilled(center, dot, ImGui::GetColorU32(colors.Text));
		}
	}

	void CellMap::DrawTooltip(const Cell& cell, const WorldAsset& world) const
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const WorldLayer& layer = world.GetData().Layers[cell.LayerIndex];
		ImGui::BeginTooltip();
		UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Body);
		ImGui::Text("%s (%d, %d)", layer.Name.c_str(), cell.Key.X, cell.Key.Z);
		ImGui::PopFont();
		const LegendState state = GetLegendState(cell);
		ImGui::TextColored(state == LegendState::Unloaded ? colors.TextSecondary : GetLegendColor(state), "%s", c_LegendLabels[static_cast<size_t>(state)]);
		if (cell.Status == WorldCellStatus::Loading || cell.Status == WorldCellStatus::Loaded || cell.Status == WorldCellStatus::Unloading)
		{
			ImGui::SameLine();
			ImGui::Text("\xC2\xB7 %llu entities", static_cast<unsigned long long>(cell.Entities));
		}
		if (cell.Bodies)
			ImGui::TextColored(colors.TextSecondary, "%llu bodies (cooked)", static_cast<unsigned long long>(*cell.Bodies));
		if (cell.Pinned)
			ImGui::TextColored(colors.TextSecondary, "%s Kept loaded (pinned, selected or unsaved)", Icons::MapPin);
		if (cell.Dirty)
			ImGui::TextColored(colors.Warning, "Unsaved changes");
		if (!cell.Error.empty())
		{
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
			ImGui::TextColored(colors.Error, "%s", cell.Error.c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::EndTooltip();
		UI::ItemProbe::Record("Streaming.CellMap.Tooltip", true, 0, layer.Name);
	}

	std::optional<CellMap::Request> CellMap::DrawContextMenu(bool editable)
	{
		if (!ImGui::BeginPopup("CellMapContext"))
		{
			m_ContextCell.reset();
			return std::nullopt;
		}
		std::optional<Request> request;
		const Cell* cell = m_ContextCell ? FindCell(*m_ContextCell) : nullptr;
		if (!cell)
		{
			// The cell left the world (a reload) while the menu was open.
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return std::nullopt;
		}
		if (ImGui::MenuItem("Load and pin", nullptr, false, editable))
			request = Request { Request::Kind::LoadAndPin, cell->Key, cell->Scene };
		UI::ItemProbe::Record("Streaming.CellMap.LoadAndPin", editable);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", editable ? "Load the cell and keep it loaded wherever the camera goes (world.loadCells)" : "Cells are pinned in the edited world (stop play first)");
		if (ImGui::MenuItem("Unpin", nullptr, false, editable && cell->Pinned))
			request = Request { Request::Kind::Unpin, cell->Key, cell->Scene };
		UI::ItemProbe::Record("Streaming.CellMap.Unpin", editable && cell->Pinned);
		ImGui::Separator();
		if (ImGui::MenuItem("Show in Content Browser", nullptr, false, cell->Scene.IsValid()))
			request = Request { Request::Kind::Show, cell->Key, cell->Scene };
		UI::ItemProbe::Record("Streaming.CellMap.Show", cell->Scene.IsValid());
		ImGui::EndPopup();
		return request;
	}

	void CellMap::DrawLegend()
	{
		// One line under the map, wrapping in narrow panels: a swatch, the state and its cells, for each state.
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		const float swatch = ImGui::GetFontSize() * 0.7f;
		const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		for (size_t index = 0; index < m_Legend.size(); index++)
		{
			const UI::LegendLine& line = m_Legend[index];
			const float width = swatch + style.ItemInnerSpacing.x + ImGui::CalcTextSize(line.Label.c_str()).x + style.ItemInnerSpacing.x
				+ ImGui::CalcTextSize(line.Count.c_str()).x;
			if (index > 0 && ImGui::GetItemRectMax().x + style.ItemSpacing.x * 2.0f + width <= right)
				ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
			ImGui::BeginGroup();
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const float top = at.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f;
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + swatch, top + swatch), ImGui::GetColorU32(line.Color));
			ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			ImGui::TextColored(colors.TextSecondary, "%s", line.Label.c_str());
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			ImGui::TextUnformatted(line.Count.c_str());
			ImGui::EndGroup();
			UI::ItemProbe::Record(UI::NumberedProbeKey("Streaming.CellMap.Legend", index), true, 0, line.Count);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("Cells %s; unsaved cells are outlined, and pinned ones (kept loaded) carry a pin", line.Label.c_str());
		}
	}

}
