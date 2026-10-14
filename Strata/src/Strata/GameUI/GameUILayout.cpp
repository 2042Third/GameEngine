#include "stpch.h"
#include "Strata/GameUI/GameUIScene.h"

#include "Strata/Core/Profiling.h"
#include "Strata/GameUI/GameUISceneData.h"

#include <algorithm>
#include <cmath>

// The layout of a canvas: rects are computed per axis, widths first, then heights (a wrapped text's height depends on its
// width). On each axis, preferred sizes go bottom up (what a fitted rect or a layout group needs of its children) and
// boxes top down (each element in its parent's box, or where its parent's layout group places it).
namespace Strata
{

	namespace
	{

		bool FitsAxis(UIFitContent fit, int axis)
		{
			return fit == UIFitContent::Both || (axis == 0 ? fit == UIFitContent::Width : fit == UIFitContent::Height);
		}

		// Where on an axis the alignment puts a block in the space left: 0 (start), 0.5 or 1 (end).
		float GetAlignment(UIChildAlignment alignment, int axis)
		{
			const int value = static_cast<int>(alignment);
			const int step = axis == 0 ? value % 3 : value / 3;
			return static_cast<float>(step) * 0.5f;
		}

		// A slider's normalized value, and whether its direction runs against the axis.
		float GetSliderFraction(const UISliderComponent& slider)
		{
			const float range = slider.MaxValue - slider.MinValue;
			if (!(std::abs(range) > 0.0f) || !std::isfinite(range))
				return 0.0f;
			const float fraction = (slider.Value - slider.MinValue) / range;
			return std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : 0.0f;
		}

		int GetDirectionAxis(UIFillDirection direction)
		{
			return direction == UIFillDirection::LeftToRight || direction == UIFillDirection::RightToLeft ? 0 : 1;
		}

		bool IsReversed(UIFillDirection direction)
		{
			// Canvas units grow right and down: left to right and top to bottom run along the axes.
			return direction == UIFillDirection::RightToLeft || direction == UIFillDirection::BottomToTop;
		}

	}

	UIBox GameUIScene::GetRootBox(const Canvas& canvas, const glm::vec2& unitSize) const
	{
		const UICanvasComponent* settings = m_Registry->try_get<UICanvasComponent>(canvas.Entity);
		const glm::vec4 padding = settings ? glm::max(settings->SafeAreaPadding, glm::vec4(0.0f)) : glm::vec4(0.0f);
		UIBox box;
		box.Min = glm::vec2(padding.x, padding.y);
		box.Size = glm::max(unitSize - glm::vec2(padding.x + padding.z, padding.y + padding.w), glm::vec2(0.0f));
		return box;
	}

	uint32_t GameUIScene::GetGridColumns(const UILayoutComponent& group, float innerWidth, uint32_t children)
	{
		if (group.Columns > 0)
			return group.Columns;
		// As many cells as the width holds, at least one.
		const float cell = std::max(group.CellSize.x, 1.0f);
		const float pitch = cell + group.Spacing.x;
		if (!(pitch > 0.0f))
			return std::max(children, 1u);
		const float fit = std::floor((innerWidth + group.Spacing.x) / pitch + 1e-4f);
		return std::max(1u, static_cast<uint32_t>(std::clamp(fit, 1.0f, 1.0e6f)));
	}

	bool GameUIScene::IsPlacedByGroup(const Canvas& canvas, uint32_t element) const
	{
		const Element& data = canvas.Elements[element];
		if (data.Parent == c_NoElement || !data.Active || !m_Registry->all_of<UILayoutComponent>(canvas.Elements[data.Parent].Entity))
			return false;
		const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(data.Entity);
		return rect && !rect->IgnoreLayout;
	}

	void GameUIScene::UpdateLayout(Canvas& canvas, Layout& layout)
	{
		const uint32_t count = static_cast<uint32_t>(canvas.Elements.size());
		layout.Boxes.resize(count);
		layout.Texts.resize(count);
		layout.DirtyMarks.resize(count, 0);
		if (count == 0)
			return;

		auto markQuads = [&layout](uint32_t element)
		{
			if (layout.QuadsRepack || element >= layout.QuadsDirtyMarks.size() || layout.QuadsDirtyMarks[element])
				return;
			layout.QuadsDirtyMarks[element] = 1;
			layout.QuadsDirty.push_back(element);
		};

		if (layout.AllDirty)
		{
			ST_PROFILE_SCOPE("GameUIScene::FullLayout");
			LayOutSubtree(canvas, layout, 0, layout.Boxes);
			m_Stats.ElementsLaidOut += count;
			m_Stats.FullRebuilds++;
			layout.AllDirty = false;
			layout.Dirty.clear();
			std::fill(layout.DirtyMarks.begin(), layout.DirtyMarks.end(), 0);
			layout.QuadsRepack = true;
			layout.HitStamp = ++m_HitStamp;
			return;
		}
		if (layout.Dirty.empty())
			return;

		// Each marked element's subtree, from the highest ancestor whose rect depends on its children; nested roots are
		// covered by the root above them.
		std::vector<uint32_t>& roots = layout.Dirty;
		for (uint32_t& root : roots)
		{
			layout.DirtyMarks[root] = 0;
			while (canvas.Elements[root].Parent != c_NoElement && DependsOnChildren(canvas.Elements[root].Parent, canvas))
				root = canvas.Elements[root].Parent;
		}
		std::sort(roots.begin(), roots.end());
		roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
		uint32_t coveredEnd = 0;
		for (const uint32_t root : roots)
		{
			if (root < coveredEnd)
				continue;
			const uint32_t end = canvas.Elements[root].SubtreeEnd;
			coveredEnd = end;
			m_BoxScratch.assign(layout.Boxes.begin() + root, layout.Boxes.begin() + end);
			LayOutSubtree(canvas, layout, root, layout.Boxes);
			m_Stats.ElementsLaidOut += end - root;
			// Only the quads of elements that moved or resized follow.
			for (uint32_t element = root; element < end; element++)
			{
				const UIBox& before = m_BoxScratch[element - root];
				const UIBox& after = layout.Boxes[element];
				if (before.Min != after.Min || before.Size != after.Size)
				{
					markQuads(element);
					layout.HitStamp = m_HitStamp + 1;
				}
			}
		}
		roots.clear();
		m_HitStamp = std::max(m_HitStamp, layout.HitStamp);
	}

	void GameUIScene::LayOutSubtree(const Canvas& canvas, Layout& layout, uint32_t root, std::vector<UIBox>& boxes)
	{
		const uint32_t count = canvas.Elements[root].SubtreeEnd - root;
		m_PreferredMemo.assign(count, 0.0f);
		m_PlacedPosition.assign(count, 0.0f);
		m_PlacedSize.assign(count, 0.0f);
		for (int axis = 0; axis < 2; axis++)
		{
			m_PreferredKnown.assign(count, 0);
			m_Placed.assign(count, 0);
			LayOutAxis(canvas, layout, root, axis, boxes);
		}
	}

	void GameUIScene::LayOutAxis(const Canvas& canvas, Layout& layout, uint32_t root, int axis, std::vector<UIBox>& boxes)
	{
		const uint32_t end = canvas.Elements[root].SubtreeEnd;
		for (uint32_t element = root; element < end; element++)
		{
			const Element& data = canvas.Elements[element];
			float position = 0.0f;
			float size = 0.0f;
			if (element == 0)
			{
				// The canvas: the root rect.
				const UIBox rootBox = GetRootBox(canvas, layout.UnitSize);
				position = rootBox.Min[axis];
				size = rootBox.Size[axis];
			}
			else if (element != root && m_Placed[element - root])
			{
				position = m_PlacedPosition[element - root];
				size = m_PlacedSize[element - root];
			}
			else
			{
				// Anchored in the parent's box.
				const UIBox& parent = boxes[data.Parent];
				const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(data.Entity);
				const UIRectComponent defaults;
				const UIRectComponent& values = rect ? *rect : defaults;
				float anchorMin = values.AnchorMin[axis];
				float anchorMax = values.AnchorMax[axis];
				if (data.SliderOf != c_NoElement)
				{
					// A slider's fill spans from the slider's start to its value along its direction, its handle sits at the value.
					const UISliderComponent* slider = m_Registry->try_get<UISliderComponent>(canvas.Elements[data.SliderOf].Entity);
					if (slider && GetDirectionAxis(slider->Direction) == axis)
					{
						const float fraction = GetSliderFraction(*slider);
						const bool reversed = IsReversed(slider->Direction);
						if (data.SliderHandle)
						{
							anchorMin = anchorMax = reversed ? 1.0f - fraction : fraction;
						}
						else
						{
							anchorMin = reversed ? 1.0f - fraction : 0.0f;
							anchorMax = reversed ? 1.0f : fraction;
						}
					}
				}
				const float anchor0 = parent.Min[axis] + anchorMin * parent.Size[axis];
				const float anchor1 = parent.Min[axis] + anchorMax * parent.Size[axis];
				if (FitsAxis(values.FitContent, axis))
					size = std::max(GetContentSize(canvas, layout, root, element, axis, boxes), 0.0f);
				else
					size = std::max(anchor1 - anchor0 + values.Size[axis], 0.0f);
				const float pivotPoint = anchor0 + (anchor1 - anchor0) * values.Pivot[axis] + values.Position[axis];
				position = pivotPoint - size * values.Pivot[axis];
			}
			if (!std::isfinite(position) || !std::isfinite(size))
			{
				position = 0.0f;
				size = 0.0f;
			}
			boxes[element].Min[axis] = position;
			boxes[element].Size[axis] = size;

			// A layout group places its children once its own box is known on this axis.
			if (m_Registry->all_of<UILayoutComponent>(data.Entity))
				PlaceGroupChildren(canvas, layout, root, element, axis, boxes);
		}
	}

	void GameUIScene::PlaceGroupChildren(const Canvas& canvas, Layout& layout, uint32_t root, uint32_t group, int axis, std::vector<UIBox>& boxes)
	{
		const UILayoutComponent& settings = m_Registry->get<UILayoutComponent>(canvas.Elements[group].Entity);
		const glm::vec4 padding = glm::max(settings.Padding, glm::vec4(0.0f));
		const float paddingStart = axis == 0 ? padding.x : padding.y;
		const float paddingEnd = axis == 0 ? padding.z : padding.w;
		const float innerStart = boxes[group].Min[axis] + paddingStart;
		const float innerSize = std::max(boxes[group].Size[axis] - paddingStart - paddingEnd, 0.0f);
		const float spacing = settings.Spacing[axis];
		const float alignment = GetAlignment(settings.ChildAlignment, axis);
		const uint32_t end = canvas.Elements[group].SubtreeEnd;

		uint32_t placedCount = 0;
		for (uint32_t child = group + 1; child < end; child = canvas.Elements[child].SubtreeEnd)
			placedCount += IsPlacedByGroup(canvas, child) ? 1 : 0;
		if (placedCount == 0)
			return;

		auto place = [&](uint32_t child, float position, float size)
		{
			m_Placed[child - root] = 1;
			m_PlacedPosition[child - root] = position;
			m_PlacedSize[child - root] = std::max(size, 0.0f);
		};

		if (settings.Type == UILayoutType::Grid)
		{
			// Cells of CellSize in rows of the grid's columns; the block of cells is aligned in the padded rect.
			const float innerWidth = std::max(boxes[group].Size.x - padding.x - padding.z, 0.0f);
			const uint32_t columns = GetGridColumns(settings, innerWidth, placedCount);
			const uint32_t usedColumns = std::min(columns, placedCount);
			const uint32_t rows = (placedCount + columns - 1) / columns;
			const float cell = std::max(settings.CellSize[axis], 1.0f);
			const uint32_t cellsAlong = axis == 0 ? usedColumns : rows;
			const float block = static_cast<float>(cellsAlong) * cell + static_cast<float>(cellsAlong - 1) * spacing;
			const float start = innerStart + (innerSize - block) * alignment;
			uint32_t index = 0;
			for (uint32_t child = group + 1; child < end; child = canvas.Elements[child].SubtreeEnd)
			{
				if (!IsPlacedByGroup(canvas, child))
					continue;
				const uint32_t cellIndex = axis == 0 ? index % columns : index / columns;
				place(child, start + static_cast<float>(cellIndex) * (cell + spacing), cell);
				index++;
			}
			return;
		}

		const int mainAxis = settings.Type == UILayoutType::Horizontal ? 0 : 1;
		if (axis == mainAxis)
		{
			// Stacked at their preferred sizes, the block aligned in the padded rect.
			float block = 0.0f;
			for (uint32_t child = group + 1; child < end; child = canvas.Elements[child].SubtreeEnd)
			{
				if (IsPlacedByGroup(canvas, child))
					block += std::max(GetPreferredSize(canvas, layout, root, child, axis, boxes), 0.0f);
			}
			block += spacing * static_cast<float>(placedCount - 1);
			float position = innerStart + (innerSize - block) * alignment;
			for (uint32_t child = group + 1; child < end; child = canvas.Elements[child].SubtreeEnd)
			{
				if (!IsPlacedByGroup(canvas, child))
					continue;
				const float size = std::max(GetPreferredSize(canvas, layout, root, child, axis, boxes), 0.0f);
				place(child, position, size);
				position += size + spacing;
			}
		}
		else
		{
			// Across the stack: the padded rect's extent when stretched, else each child's preferred size, aligned.
			for (uint32_t child = group + 1; child < end; child = canvas.Elements[child].SubtreeEnd)
			{
				if (!IsPlacedByGroup(canvas, child))
					continue;
				const float size = settings.StretchChildren ? innerSize : std::max(GetPreferredSize(canvas, layout, root, child, axis, boxes), 0.0f);
				place(child, innerStart + (innerSize - size) * alignment, size);
			}
		}
	}

	float GameUIScene::GetPreferredSize(const Canvas& canvas, Layout& layout, uint32_t root, uint32_t element, int axis, const std::vector<UIBox>& boxes)
	{
		const uint32_t slot = element - root;
		if (m_PreferredKnown[slot])
			return m_PreferredMemo[slot];
		const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(canvas.Elements[element].Entity);
		float size = 0.0f;
		if (rect && FitsAxis(rect->FitContent, axis))
			size = GetContentSize(canvas, layout, root, element, axis, boxes);
		else
			size = rect ? rect->Size[axis] : 0.0f;
		size = std::isfinite(size) ? std::max(size, 0.0f) : 0.0f;
		m_PreferredMemo[slot] = size;
		m_PreferredKnown[slot] = 1;
		return size;
	}

	float GameUIScene::GetContentSize(const Canvas& canvas, Layout& layout, uint32_t root, uint32_t element, int axis, const std::vector<UIBox>& boxes)
	{
		const entt::entity entity = canvas.Elements[element].Entity;
		if (m_Registry->all_of<UILayoutComponent>(entity))
			return GetGroupContentSize(canvas, layout, root, element, axis, boxes);
		if (const UITextComponent* text = m_Registry->try_get<UITextComponent>(entity))
		{
			// Widths are unwrapped; a height is the text's at the width it has (known once widths are laid out).
			const float fontSize = std::clamp(text->FontSize, 1.0f, 1024.0f);
			const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(entity);
			const bool fitsWidth = rect && FitsAxis(rect->FitContent, 0);
			const float wrapWidth = axis == 1 && text->Wrap && !fitsWidth ? boxes[element].Size.x / fontSize : 0.0f;
			const ShapedText& shaped = ShapeElementText(layout, element, *text, GetMeasureOptions(*text, wrapWidth));
			return (axis == 0 ? shaped.Width : shaped.Height) * fontSize;
		}
		const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(entity);
		return rect ? rect->Size[axis] : 0.0f;
	}

	float GameUIScene::GetGroupContentSize(const Canvas& canvas, Layout& layout, uint32_t root, uint32_t group, int axis, const std::vector<UIBox>& boxes)
	{
		const UILayoutComponent& settings = m_Registry->get<UILayoutComponent>(canvas.Elements[group].Entity);
		const glm::vec4 padding = glm::max(settings.Padding, glm::vec4(0.0f));
		const float paddingTotal = axis == 0 ? padding.x + padding.z : padding.y + padding.w;
		const uint32_t end = canvas.Elements[group].SubtreeEnd;
		uint32_t placedCount = 0;
		for (uint32_t child = group + 1; child < end; child = canvas.Elements[child].SubtreeEnd)
			placedCount += IsPlacedByGroup(canvas, child) ? 1 : 0;
		if (placedCount == 0)
			return paddingTotal;

		if (settings.Type == UILayoutType::Grid)
		{
			// Widths: the columns asked for, or one row; heights: the rows the laid-out width holds.
			uint32_t columns = settings.Columns > 0 ? settings.Columns : placedCount;
			if (axis == 1)
				columns = GetGridColumns(settings, std::max(boxes[group].Size.x - padding.x - padding.z, 0.0f), placedCount);
			const uint32_t cells = axis == 0 ? std::min(columns, placedCount) : (placedCount + columns - 1) / columns;
			const float cell = std::max(settings.CellSize[axis], 1.0f);
			return paddingTotal + static_cast<float>(cells) * cell + static_cast<float>(cells - 1) * settings.Spacing[axis];
		}

		const int mainAxis = settings.Type == UILayoutType::Horizontal ? 0 : 1;
		float content = 0.0f;
		for (uint32_t child = group + 1; child < end; child = canvas.Elements[child].SubtreeEnd)
		{
			if (!IsPlacedByGroup(canvas, child))
				continue;
			const float size = GetPreferredSize(canvas, layout, root, child, axis, boxes);
			content = axis == mainAxis ? content + size : std::max(content, size);
		}
		if (axis == mainAxis)
			content += settings.Spacing[axis] * static_cast<float>(placedCount - 1);
		return paddingTotal + content;
	}

}
