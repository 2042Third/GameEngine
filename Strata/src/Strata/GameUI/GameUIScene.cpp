#include "stpch.h"
#include "Strata/GameUI/GameUIScene.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Assert.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Profiling.h"
#include "Strata/GameUI/GameUISceneData.h"
#include "Strata/Input/Input.h"
#include "Strata/Renderer/FontFace.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneHierarchy.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace Strata
{

	// Holds a scene's UI in its registry's context. The context is destroyed after the registry's storages, with their
	// signals, so the UI never disconnects its listeners: there is nothing left to disconnect them from (as RenderScene).
	struct GameUISceneContext
	{
		Scope<GameUIScene> Instance;
	};

	namespace
	{

		std::atomic<uint64_t> s_NextId = 1;

		// Whether two boxes are the same within the tolerance layouts are compared at.
		bool SameBox(const UIBox& a, const UIBox& b, float tolerance)
		{
			auto close = [tolerance](float x, float y) { return std::abs(x - y) <= tolerance * std::max(1.0f, std::max(std::abs(x), std::abs(y))); };
			return close(a.Min.x, b.Min.x) && close(a.Min.y, b.Min.y) && close(a.Size.x, b.Size.x) && close(a.Size.y, b.Size.y);
		}

		bool SameUnitSize(const glm::vec2& a, const glm::vec2& b)
		{
			const float tolerance = 1e-5f * std::max({ 1.0f, std::abs(a.x), std::abs(a.y) });
			return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// Lifetime and signals
	////////////////////////////////////////////////////////////////////////////////

	GameUIScene& GameUIScene::Get(Scene& scene)
	{
		entt::registry& registry = scene.GetRegistry();
		if (GameUISceneContext* context = registry.ctx().find<GameUISceneContext>())
			return *context->Instance;
		GameUISceneContext& context = registry.ctx().emplace<GameUISceneContext>();
		context.Instance = Scope<GameUIScene>(new GameUIScene(scene));
		return *context.Instance;
	}

	GameUIScene* GameUIScene::Find(Scene& scene)
	{
		GameUISceneContext* context = scene.GetRegistry().ctx().find<GameUISceneContext>();
		return context ? context->Instance.get() : nullptr;
	}

	bool GameUIScene::HasCanvases(const Scene& scene)
	{
		const auto* storage = scene.GetRegistry().storage<UICanvasComponent>();
		return storage && !storage->empty();
	}

	float GameUIScene::ComputeScale(const UICanvasComponent& canvas, const UIScreen& screen)
	{
		const float factor = std::clamp(canvas.ScaleFactor, 0.01f, 100.0f);
		float scale = 1.0f;
		switch (canvas.ScaleMode)
		{
		case UICanvasScaleMode::ConstantPixelSize:
			scale = factor;
			break;
		case UICanvasScaleMode::ScaleWithScreen:
		{
			const glm::vec2 reference = glm::clamp(canvas.ReferenceResolution, glm::vec2(1.0f), glm::vec2(16384.0f));
			const glm::vec2 size = glm::max(screen.Size, glm::vec2(1.0f));
			const float match = std::clamp(canvas.MatchWidthOrHeight, 0.0f, 1.0f);
			const float logWidth = std::log2(size.x / reference.x);
			const float logHeight = std::log2(size.y / reference.y);
			scale = std::exp2(logWidth + (logHeight - logWidth) * match);
			break;
		}
		case UICanvasScaleMode::ConstantPhysicalSize:
			scale = std::max(screen.ContentScale, 0.01f) * factor;
			break;
		}
		return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
	}

	GameUIScene::GameUIScene(Scene& scene)
		: m_Scene(&scene), m_Registry(&scene.GetRegistry()), m_HierarchyVersion(scene.GetHierarchyVersion()), m_TransformsVersion(scene.GetTransformsVersion()),
		m_Id(s_NextId.fetch_add(1, std::memory_order_relaxed))
	{
		// The connections are never released (see GameUISceneContext): this object lives exactly as long as the storages.
		entt::registry& registry = *m_Registry;
		registry.on_construct<UICanvasComponent>().connect<&GameUIScene::OnCanvasAddedOrRemoved>(*this);
		registry.on_update<UICanvasComponent>().connect<&GameUIScene::OnCanvasChanged>(*this);
		registry.on_destroy<UICanvasComponent>().connect<&GameUIScene::OnCanvasAddedOrRemoved>(*this);
		registry.on_construct<UIRectComponent>().connect<&GameUIScene::OnStructureChanged>(*this);
		registry.on_update<UIRectComponent>().connect<&GameUIScene::OnLayoutChanged>(*this);
		registry.on_destroy<UIRectComponent>().connect<&GameUIScene::OnStructureChanged>(*this);
		registry.on_construct<UILayoutComponent>().connect<&GameUIScene::OnLayoutChanged>(*this);
		registry.on_update<UILayoutComponent>().connect<&GameUIScene::OnLayoutChanged>(*this);
		registry.on_destroy<UILayoutComponent>().connect<&GameUIScene::OnLayoutChanged>(*this);
		registry.on_construct<UIImageComponent>().connect<&GameUIScene::OnVisualChanged>(*this);
		registry.on_update<UIImageComponent>().connect<&GameUIScene::OnVisualChanged>(*this);
		registry.on_destroy<UIImageComponent>().connect<&GameUIScene::OnImageDestroyed>(*this);
		registry.on_construct<UITextComponent>().connect<&GameUIScene::OnTextChanged>(*this);
		registry.on_update<UITextComponent>().connect<&GameUIScene::OnTextChanged>(*this);
		registry.on_destroy<UITextComponent>().connect<&GameUIScene::OnTextDestroyed>(*this);
		registry.on_construct<UISelectableComponent>().connect<&GameUIScene::OnVisualChanged>(*this);
		registry.on_update<UISelectableComponent>().connect<&GameUIScene::OnVisualChanged>(*this);
		registry.on_destroy<UISelectableComponent>().connect<&GameUIScene::OnSelectableDestroyed>(*this);
		registry.on_construct<UIToggleComponent>().connect<&GameUIScene::OnToggleChanged>(*this);
		registry.on_update<UIToggleComponent>().connect<&GameUIScene::OnToggleChanged>(*this);
		registry.on_destroy<UIToggleComponent>().connect<&GameUIScene::OnToggleChanged>(*this);
		registry.on_construct<UISliderComponent>().connect<&GameUIScene::OnSliderChanged>(*this);
		registry.on_update<UISliderComponent>().connect<&GameUIScene::OnSliderChanged>(*this);
		registry.on_destroy<UISliderComponent>().connect<&GameUIScene::OnSliderChanged>(*this);
		// A mask added or removed changes which clip rect each element of its subtree has: the canvas is rebuilt.
		registry.on_construct<UIMaskComponent>().connect<&GameUIScene::OnStructureChanged>(*this);
		registry.on_update<UIMaskComponent>().connect<&GameUIScene::OnMaskChanged>(*this);
		registry.on_destroy<UIMaskComponent>().connect<&GameUIScene::OnStructureChanged>(*this);
		registry.on_construct<UIInputFieldComponent>().connect<&GameUIScene::OnInputFieldAddedOrRemoved>(*this);
		registry.on_update<UIInputFieldComponent>().connect<&GameUIScene::OnInputFieldChanged>(*this);
		registry.on_destroy<UIInputFieldComponent>().connect<&GameUIScene::OnInputFieldAddedOrRemoved>(*this);
		if (const AssetManagerBase* assets = scene.GetAssetManager())
			m_AssetContentVersion = assets->GetContentVersion();
		// Index 0 is the default font (texts without a font, or whose font loads).
		m_Fonts.push_back(UUID::Null());
		m_FontIndices.emplace(UUID::Null(), 0);
		m_FontGlyphUses.emplace_back();
	}

	GameUIScene::~GameUIScene() = default;

	const GameUIScene::ElementRef* GameUIScene::FindRef(entt::entity entity) const
	{
		const size_t index = static_cast<size_t>(entt::to_entity(entity));
		if (index >= m_ElementRefs.size() || m_ElementRefs[index].Entity != entity)
			return nullptr;
		return &m_ElementRefs[index];
	}

	void GameUIScene::OnCanvasAddedOrRemoved(entt::registry&, entt::entity)
	{
		// Which canvases are roots may change: they are found again (rare).
		m_CanvasListDirty = true;
		m_CanvasSetStamp++;
	}

	void GameUIScene::OnCanvasChanged(entt::registry&, entt::entity entity)
	{
		// A root canvas's settings (scale, safe area, sort order) apply to all of its layouts. A nested canvas is a plain
		// element of the outer one: its settings change nothing.
		const ElementRef* ref = FindRef(entity);
		if (ref && ref->Element == 0)
		{
			MarkCanvasDirty(ref->Canvas);
			m_ViewsValid = false;
			m_CanvasSettingsStamp++; // Its render mode may have changed
		}
	}

	void GameUIScene::OnStructureChanged(entt::registry&, entt::entity entity)
	{
		// An element added or removed. Where an added one belongs is found once the hierarchy is complete (instantiation
		// links entities after adding their components): the next update looks for its canvas. An element whose rect was
		// replaced (removed and added again, as undo restores components) stays where it is but has new values: it is laid
		// out again, and the rebuild keeps that mark.
		if (const ElementRef* ref = FindRef(entity))
		{
			m_Canvases[ref->Canvas].StructureDirty = true;
			MarkElementLayoutDirty(ref->Canvas, ref->Element);
		}
		else
		{
			m_PendingStructure.push_back(entity);
		}
	}

	void GameUIScene::OnLayoutChanged(entt::registry&, entt::entity entity)
	{
		if (const ElementRef* ref = FindRef(entity))
		{
			MarkElementLayoutDirty(ref->Canvas, ref->Element);
			MarkElementQuadsDirty(ref->Canvas, ref->Element);
		}
	}

	void GameUIScene::OnVisualChanged(entt::registry&, entt::entity entity)
	{
		const ElementRef* ref = FindRef(entity);
		if (!ref)
			return;
		Canvas& canvas = m_Canvases[ref->Canvas];
		MarkElementQuadsDirty(ref->Canvas, ref->Element);
		UpdateRaycastTarget(entity);
		// A selectable tints its target image: the target's quads follow, and a new target is found again.
		const Element& element = canvas.Elements[ref->Element];
		const UISelectableComponent* selectable = m_Registry->try_get<UISelectableComponent>(entity);
		const UUID target = selectable ? selectable->TargetImage : UUID::Null();
		if (target != element.TargetImageID)
			canvas.StructureDirty = true;
		else if (element.TintTarget != c_NoElement)
			MarkElementQuadsDirty(ref->Canvas, element.TintTarget);
	}

	void GameUIScene::OnTextChanged(entt::registry&, entt::entity entity)
	{
		const ElementRef* ref = FindRef(entity);
		if (!ref)
			return;
		Canvas& canvas = m_Canvases[ref->Canvas];
		for (Layout& layout : canvas.Layouts)
		{
			if (ref->Element < layout.Texts.size())
				layout.Texts[ref->Element].Valid = false;
		}
		MarkElementQuadsDirty(ref->Canvas, ref->Element);
		UpdateRaycastTarget(entity);
		// Only a rect that fits its text measures it.
		const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(entity);
		if (rect && rect->FitContent != UIFitContent::None)
			MarkElementLayoutDirty(ref->Canvas, ref->Element);
	}

	void GameUIScene::OnImageDestroyed(entt::registry& registry, entt::entity entity)
	{
		OnVisualChanged(registry, entity);
		UpdateRaycastTarget(entity, true, false, false);
	}

	void GameUIScene::OnTextDestroyed(entt::registry& registry, entt::entity entity)
	{
		OnTextChanged(registry, entity);
		UpdateRaycastTarget(entity, false, true, false);
	}

	void GameUIScene::OnSelectableDestroyed(entt::registry& registry, entt::entity entity)
	{
		OnVisualChanged(registry, entity);
		UpdateRaycastTarget(entity, false, false, true);
	}

	void GameUIScene::UpdateRaycastTarget(entt::entity entity, bool ignoreImage, bool ignoreText, bool ignoreSelectable)
	{
		const ElementRef* ref = FindRef(entity);
		if (!ref)
			return;
		Element& element = m_Canvases[ref->Canvas].Elements[ref->Element];
		const bool target = ComputeRaycastTarget(entity, ignoreImage, ignoreText, ignoreSelectable);
		if (target == element.RaycastTarget)
			return;
		element.RaycastTarget = target;
		TouchHits(ref->Canvas);
	}

	bool GameUIScene::ComputeRaycastTarget(entt::entity entity, bool ignoreImage, bool ignoreText, bool ignoreSelectable) const
	{
		if (!m_Registry->valid(entity))
			return false;
		if (!ignoreSelectable && m_Registry->all_of<UISelectableComponent>(entity))
			return true;
		const UIImageComponent* image = ignoreImage ? nullptr : m_Registry->try_get<UIImageComponent>(entity);
		const UITextComponent* text = ignoreText ? nullptr : m_Registry->try_get<UITextComponent>(entity);
		return (image && image->RaycastTarget) || (text && text->RaycastTarget);
	}

	void GameUIScene::OnSliderChanged(entt::registry&, entt::entity entity)
	{
		const ElementRef* ref = FindRef(entity);
		if (!ref)
			return;
		Canvas& canvas = m_Canvases[ref->Canvas];
		const Element& element = canvas.Elements[ref->Element];
		const UISliderComponent* slider = m_Registry->try_get<UISliderComponent>(entity);
		const UUID fill = slider ? slider->Fill : UUID::Null();
		const UUID handle = slider ? slider->Handle : UUID::Null();
		if (fill != element.FillID || handle != element.HandleID)
		{
			canvas.StructureDirty = true;
			return;
		}
		// The value moves the fill and the handle (their anchors follow it).
		for (uint32_t part = 0; part < canvas.Elements.size(); part++)
		{
			if (canvas.Elements[part].SliderOf == ref->Element)
			{
				MarkElementLayoutDirty(ref->Canvas, part);
				MarkSubtreeQuadsDirty(ref->Canvas, part);
			}
		}
	}

	void GameUIScene::OnToggleChanged(entt::registry&, entt::entity entity)
	{
		const ElementRef* ref = FindRef(entity);
		if (!ref)
			return;
		Canvas& canvas = m_Canvases[ref->Canvas];
		const Element& element = canvas.Elements[ref->Element];
		const UIToggleComponent* toggle = m_Registry->try_get<UIToggleComponent>(entity);
		const UUID checkmark = toggle ? toggle->Checkmark : UUID::Null();
		if (checkmark != element.CheckmarkID)
		{
			canvas.StructureDirty = true;
			return;
		}
		// The checkmark shows while the toggle is on.
		for (uint32_t part = 0; part < canvas.Elements.size(); part++)
		{
			if (canvas.Elements[part].CheckmarkOf == ref->Element)
				MarkSubtreeQuadsDirty(ref->Canvas, part);
		}
		TouchHits(ref->Canvas);
	}

	void GameUIScene::OnMaskChanged(entt::registry&, entt::entity entity)
	{
		// Its padding or softness: the clip rects are computed again (the quads name them, so they stay).
		if (const ElementRef* ref = FindRef(entity))
		{
			for (Layout& layout : m_Canvases[ref->Canvas].Layouts)
				layout.ClipsDirty = true;
		}
	}

	void GameUIScene::OnInputFieldAddedOrRemoved(entt::registry&, entt::entity entity)
	{
		// Its parts change: found again with the canvas.
		if (const ElementRef* ref = FindRef(entity))
			m_Canvases[ref->Canvas].StructureDirty = true;
		m_FieldStates.erase(entity);
	}

	void GameUIScene::OnInputFieldChanged(entt::registry&, entt::entity entity)
	{
		const ElementRef* ref = FindRef(entity);
		const UIInputFieldComponent* field = m_Registry->try_get<UIInputFieldComponent>(entity);
		if (!ref || !field)
			return;
		Canvas& canvas = m_Canvases[ref->Canvas];
		Element& element = canvas.Elements[ref->Element];
		if (field->TextElement != element.TextElementID || field->Placeholder != element.PlaceholderID)
		{
			canvas.StructureDirty = true;
			return;
		}
		// Its text element shows the text (and the caret and selection colors) anew, laid out again when it fits its text.
		const uint32_t textPart = element.FieldTextPart;
		if (textPart != c_NoElement)
		{
			for (Layout& layout : canvas.Layouts)
			{
				if (textPart < layout.Texts.size())
					layout.Texts[textPart].Valid = false;
			}
			MarkElementQuadsDirty(ref->Canvas, textPart);
			const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(canvas.Elements[textPart].Entity);
			if (rect && rect->FitContent != UIFitContent::None)
				MarkElementLayoutDirty(ref->Canvas, textPart);
		}
		// The placeholder shows while the text is empty.
		const bool empty = field->Text.empty();
		if (empty != element.FieldEmpty)
		{
			element.FieldEmpty = empty;
			if (element.PlaceholderPart != c_NoElement)
				MarkSubtreeQuadsDirty(ref->Canvas, element.PlaceholderPart);
			TouchHits(ref->Canvas);
		}
	}

	void GameUIScene::MarkElementLayoutDirty(uint32_t canvasIndex, uint32_t element)
	{
		for (Layout& layout : m_Canvases[canvasIndex].Layouts)
		{
			if (layout.AllDirty || element >= layout.DirtyMarks.size() || layout.DirtyMarks[element])
				continue;
			layout.DirtyMarks[element] = 1;
			layout.Dirty.push_back(element);
		}
	}

	void GameUIScene::MarkElementQuadsDirty(uint32_t canvasIndex, uint32_t element)
	{
		for (Layout& layout : m_Canvases[canvasIndex].Layouts)
		{
			if (layout.QuadsRepack || element >= layout.QuadsDirtyMarks.size() || layout.QuadsDirtyMarks[element])
				continue;
			layout.QuadsDirtyMarks[element] = 1;
			layout.QuadsDirty.push_back(element);
		}
	}

	void GameUIScene::MarkSubtreeQuadsDirty(uint32_t canvasIndex, uint32_t element)
	{
		const Canvas& canvas = m_Canvases[canvasIndex];
		for (uint32_t index = element; index < canvas.Elements[element].SubtreeEnd; index++)
			MarkElementQuadsDirty(canvasIndex, index);
	}

	void GameUIScene::MarkCanvasDirty(uint32_t canvasIndex)
	{
		for (Layout& layout : m_Canvases[canvasIndex].Layouts)
		{
			layout.AllDirty = true;
			layout.QuadsRepack = true;
			layout.ClipsDirty = true;
		}
		TouchHits(canvasIndex);
	}

	void GameUIScene::TouchHits(uint32_t canvasIndex)
	{
		for (Layout& layout : m_Canvases[canvasIndex].Layouts)
			layout.HitStamp = ++m_HitStamp;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Structure
	////////////////////////////////////////////////////////////////////////////////

	uint32_t GameUIScene::FindRootCanvas(entt::entity entity) const
	{
		// The topmost canvas among the entity and its ancestors.
		entt::entity root = entt::null;
		for (entt::entity current = entity; current != entt::null && m_Registry->valid(current);)
		{
			if (m_Registry->all_of<UICanvasComponent>(current))
				root = current;
			const HierarchyComponent* hierarchy = m_Registry->try_get<HierarchyComponent>(current);
			current = hierarchy ? hierarchy->Parent : entt::null;
		}
		if (root == entt::null)
			return c_NoElement;
		for (uint32_t index = 0; index < m_Canvases.size(); index++)
		{
			if (m_Canvases[index].Entity == root)
				return index;
		}
		return c_NoElement;
	}

	void GameUIScene::RebuildCanvasList()
	{
		ST_PROFILE_FUNCTION();
		m_CanvasListDirty = false;
		m_ViewsValid = false;
		// Root canvases in hierarchy order: canvases without a canvas among their ancestors.
		std::vector<entt::entity> roots;
		for (const entt::entity entity : m_Registry->view<UICanvasComponent>())
		{
			bool nested = false;
			const HierarchyComponent* hierarchy = m_Registry->try_get<HierarchyComponent>(entity);
			for (entt::entity parent = hierarchy ? hierarchy->Parent : entt::null; parent != entt::null && !nested;)
			{
				nested = m_Registry->all_of<UICanvasComponent>(parent);
				const HierarchyComponent* parentHierarchy = m_Registry->try_get<HierarchyComponent>(parent);
				parent = parentHierarchy ? parentHierarchy->Parent : entt::null;
			}
			if (!nested && m_Registry->all_of<HierarchyComponent>(entity))
				roots.push_back(entity);
		}
		std::sort(roots.begin(), roots.end(), [this](entt::entity a, entt::entity b)
		{
			return m_Scene->CompareHierarchyOrder(Entity(a, m_Scene), Entity(b, m_Scene)) < 0;
		});

		// Canvases that stay keep their layouts (and with them their text and quads); new ones start empty.
		std::vector<Canvas> previous = std::move(m_Canvases);
		m_Canvases.clear();
		m_Canvases.reserve(roots.size());
		for (const entt::entity root : roots)
		{
			Canvas& canvas = m_Canvases.emplace_back();
			canvas.Entity = root;
			auto kept = std::find_if(previous.begin(), previous.end(), [root](const Canvas& old) { return old.Entity == root; });
			if (kept != previous.end())
			{
				canvas.Elements = std::move(kept->Elements);
				canvas.Layouts = std::move(kept->Layouts);
				kept->Entity = entt::null;
			}
			canvas.StructureDirty = true;
		}
		// What the quads of canvases that went used is no longer used.
		for (Canvas& old : previous)
		{
			if (old.Entity == entt::null)
				continue; // Moved into the new list
			for (Layout& layout : old.Layouts)
				ReleaseLayoutGlyphs(layout);
		}
		std::fill(m_ElementRefs.begin(), m_ElementRefs.end(), ElementRef {});
	}

	void GameUIScene::RebuildCanvas(uint32_t canvasIndex)
	{
		ST_PROFILE_FUNCTION();
		Canvas& canvas = m_Canvases[canvasIndex];
		canvas.StructureDirty = false;
		m_ViewsValid = false;
		const std::vector<Element> old = std::move(canvas.Elements);
		canvas.Elements.clear();

		// Depth-first walk of the canvas's subtree: entities with a UIRect are elements (the canvas always is).
		std::vector<WalkFrame>& stack = m_WalkScratch;
		stack.clear();
		stack.push_back({ canvas.Entity, c_NoElement });
		while (!stack.empty())
		{
			const WalkFrame frame = stack.back();
			stack.pop_back();
			uint32_t parentElement = frame.ParentElement;
			const bool isElement = frame.Entity == canvas.Entity || m_Registry->all_of<UIRectComponent>(frame.Entity);
			if (isElement)
			{
				const uint32_t index = static_cast<uint32_t>(canvas.Elements.size());
				Element& element = canvas.Elements.emplace_back();
				element.Entity = frame.Entity;
				element.Parent = frame.ParentElement;
				element.RaycastTarget = ComputeRaycastTarget(frame.Entity);
				const WorldTransformComponent* world = m_Registry->try_get<WorldTransformComponent>(frame.Entity);
				element.Active = !world || world->ActiveInHierarchy;
				parentElement = index;
			}
			// Children pushed in reverse, so the first child is walked first.
			const HierarchyComponent& hierarchy = m_Registry->get<HierarchyComponent>(frame.Entity);
			const size_t first = stack.size();
			for (entt::entity child = hierarchy.FirstChild; child != entt::null; child = m_Registry->get<HierarchyComponent>(child).NextSibling)
				stack.push_back({ child, parentElement });
			std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(first), stack.end());
		}
		// Subtree ends: an element's subtree ends where the next element that is not its descendant starts.
		const uint32_t count = static_cast<uint32_t>(canvas.Elements.size());
		for (uint32_t index = count; index-- > 0;)
		{
			Element& element = canvas.Elements[index];
			if (element.SubtreeEnd < index + 1)
				element.SubtreeEnd = index + 1;
			if (element.Parent != c_NoElement)
				canvas.Elements[element.Parent].SubtreeEnd = std::max(canvas.Elements[element.Parent].SubtreeEnd, element.SubtreeEnd);
		}
		// Masks: each takes a slot (parents before children), and every element is clipped by the nearest mask above it.
		canvas.Masks.clear();
		for (uint32_t index = 0; index < count; index++)
		{
			Element& element = canvas.Elements[index];
			element.MaskSlot = c_NoElement;
			element.Clip = 0;
			if (element.Parent != c_NoElement)
			{
				const Element& parent = canvas.Elements[element.Parent];
				element.Clip = parent.MaskSlot != c_NoElement ? parent.MaskSlot + 1 : parent.Clip;
			}
			if (m_Registry->all_of<UIMaskComponent>(element.Entity))
			{
				element.MaskSlot = static_cast<uint32_t>(canvas.Masks.size());
				canvas.Masks.push_back(index);
			}
		}

		// Where each element is, and where it was.
		std::unordered_map<entt::entity, uint32_t> oldIndices;
		oldIndices.reserve(old.size());
		for (uint32_t index = 0; index < old.size(); index++)
		{
			oldIndices.emplace(old[index].Entity, index);
			const size_t entityIndex = static_cast<size_t>(entt::to_entity(old[index].Entity));
			// Only this canvas's entries: an element that moved to a canvas rebuilt before this one keeps its new entry.
			if (entityIndex < m_ElementRefs.size() && m_ElementRefs[entityIndex].Entity == old[index].Entity && m_ElementRefs[entityIndex].Canvas == canvasIndex)
				m_ElementRefs[entityIndex] = ElementRef {};
		}
		for (uint32_t index = 0; index < count; index++)
		{
			const size_t entityIndex = static_cast<size_t>(entt::to_entity(canvas.Elements[index].Entity));
			if (entityIndex >= m_ElementRefs.size())
				m_ElementRefs.resize(std::max(entityIndex + 1, m_ElementRefs.size() * 2));
			m_ElementRefs[entityIndex] = ElementRef { canvas.Elements[index].Entity, canvasIndex, index };
		}
		ResolveParts(canvasIndex);

		// The layouts keep what they computed for elements that stay; what moved, appeared or disappeared, and the
		// parents of those, are laid out again. The quads are rebuilt (their slots follow the elements).
		std::vector<uint32_t> mapping(count, c_NoElement);
		for (uint32_t index = 0; index < count; index++)
		{
			auto found = oldIndices.find(canvas.Elements[index].Entity);
			if (found != oldIndices.end())
				mapping[index] = found->second;
		}
		// Changes marked before the rebuild stay marked (their element indices change with it).
		std::vector<entt::entity> markedBefore;
		for (const Layout& layout : canvas.Layouts)
		{
			for (const uint32_t index : layout.Dirty)
			{
				if (index < old.size())
					markedBefore.push_back(old[index].Entity);
			}
		}
		for (Layout& layout : canvas.Layouts)
		{
			std::vector<UIBox> boxes(count);
			std::vector<ElementText> texts(count);
			for (uint32_t index = 0; index < count; index++)
			{
				if (mapping[index] == c_NoElement)
					continue;
				if (mapping[index] < layout.Boxes.size())
					boxes[index] = layout.Boxes[mapping[index]];
				if (mapping[index] < layout.Texts.size())
					texts[index] = std::move(layout.Texts[mapping[index]]);
			}
			layout.Boxes = std::move(boxes);
			layout.Texts = std::move(texts);
			layout.Dirty.clear();
			layout.DirtyMarks.assign(count, 0);
			layout.QuadsRepack = true;
			layout.ClipsDirty = true;
			layout.HitStamp = ++m_HitStamp;
		}
		// An element moved when its parent or the sibling before it changed: each list's previous siblings in one pass.
		auto previousSiblings = [](const std::vector<Element>& elements, std::vector<entt::entity>& outPrevious)
		{
			std::vector<uint32_t> lastChild(elements.size(), c_NoElement);
			outPrevious.assign(elements.size(), entt::null);
			for (uint32_t index = 0; index < elements.size(); index++)
			{
				const uint32_t parent = elements[index].Parent;
				if (parent == c_NoElement)
					continue;
				if (lastChild[parent] != c_NoElement)
					outPrevious[index] = elements[lastChild[parent]].Entity;
				lastChild[parent] = index;
			}
		};
		std::vector<entt::entity> oldPrevious;
		std::vector<entt::entity> newPrevious;
		previousSiblings(old, oldPrevious);
		previousSiblings(canvas.Elements, newPrevious);
		auto parentEntity = [](const std::vector<Element>& elements, uint32_t index) { return elements[index].Parent != c_NoElement ? elements[elements[index].Parent].Entity : entt::null; };
		for (uint32_t index = 0; index < count; index++)
		{
			const uint32_t was = mapping[index];
			const bool moved = was == c_NoElement || parentEntity(old, was) != parentEntity(canvas.Elements, index) || oldPrevious[was] != newPrevious[index];
			if (!moved)
				continue;
			MarkElementLayoutDirty(canvasIndex, index);
			if (canvas.Elements[index].Parent != c_NoElement)
				MarkElementLayoutDirty(canvasIndex, canvas.Elements[index].Parent);
		}
		// The parents that lost elements (their elements are no longer in this canvas).
		for (uint32_t index = 0; index < old.size(); index++)
		{
			const ElementRef* stays = FindRef(old[index].Entity);
			if ((stays && stays->Canvas == canvasIndex) || old[index].Parent == c_NoElement)
				continue;
			const ElementRef* parent = FindRef(old[old[index].Parent].Entity);
			if (parent && parent->Canvas == canvasIndex)
				MarkElementLayoutDirty(canvasIndex, parent->Element);
		}
		for (const entt::entity entity : markedBefore)
		{
			const ElementRef* ref = FindRef(entity);
			if (ref && ref->Canvas == canvasIndex)
				MarkElementLayoutDirty(canvasIndex, ref->Element);
		}
	}

	void GameUIScene::ResolveParts(uint32_t canvasIndex)
	{
		Canvas& canvas = m_Canvases[canvasIndex];
		auto resolve = [&](UUID id) -> uint32_t
		{
			if (!id.IsValid())
				return c_NoElement;
			const Entity entity = m_Scene->GetEntityByUUID(id);
			const ElementRef* ref = entity ? FindRef(entity.GetHandle()) : nullptr;
			return ref && ref->Canvas == canvasIndex ? ref->Element : c_NoElement;
		};
		for (Element& element : canvas.Elements)
		{
			element.SliderOf = c_NoElement;
			element.SliderHandle = false;
			element.CheckmarkOf = c_NoElement;
			element.TintTarget = c_NoElement;
			element.TintFrom = c_NoElement;
			element.FillID = UUID::Null();
			element.HandleID = UUID::Null();
			element.CheckmarkID = UUID::Null();
			element.TargetImageID = UUID::Null();
			element.FieldTextOf = c_NoElement;
			element.PlaceholderOf = c_NoElement;
			element.FieldTextPart = c_NoElement;
			element.PlaceholderPart = c_NoElement;
			element.TextElementID = UUID::Null();
			element.PlaceholderID = UUID::Null();
			element.FieldEmpty = true;
		}
		for (uint32_t index = 0; index < canvas.Elements.size(); index++)
		{
			Element& element = canvas.Elements[index];
			if (const UISliderComponent* slider = m_Registry->try_get<UISliderComponent>(element.Entity))
			{
				element.FillID = slider->Fill;
				element.HandleID = slider->Handle;
				const uint32_t fill = resolve(slider->Fill);
				const uint32_t handle = resolve(slider->Handle);
				// A part that is the slider itself or one of its ancestors would move what places it: ignored.
				if (fill != c_NoElement && fill > index)
					canvas.Elements[fill].SliderOf = index;
				if (handle != c_NoElement && handle > index)
				{
					canvas.Elements[handle].SliderOf = index;
					canvas.Elements[handle].SliderHandle = true;
				}
			}
			if (const UIToggleComponent* toggle = m_Registry->try_get<UIToggleComponent>(element.Entity))
			{
				element.CheckmarkID = toggle->Checkmark;
				const uint32_t checkmark = resolve(toggle->Checkmark);
				if (checkmark != c_NoElement && checkmark != index)
					canvas.Elements[checkmark].CheckmarkOf = index;
			}
			if (const UIInputFieldComponent* field = m_Registry->try_get<UIInputFieldComponent>(element.Entity))
			{
				element.TextElementID = field->TextElement;
				element.PlaceholderID = field->Placeholder;
				element.FieldEmpty = field->Text.empty();
				const uint32_t text = resolve(field->TextElement);
				if (text != c_NoElement && text != index)
				{
					canvas.Elements[text].FieldTextOf = index;
					element.FieldTextPart = text;
				}
				const uint32_t placeholder = resolve(field->Placeholder);
				if (placeholder != c_NoElement && placeholder != index)
				{
					canvas.Elements[placeholder].PlaceholderOf = index;
					element.PlaceholderPart = placeholder;
				}
			}
			if (const UISelectableComponent* selectable = m_Registry->try_get<UISelectableComponent>(element.Entity))
			{
				element.TargetImageID = selectable->TargetImage;
				const uint32_t target = selectable->TargetImage.IsValid() ? resolve(selectable->TargetImage) : index;
				if (target != c_NoElement)
				{
					element.TintTarget = target;
					canvas.Elements[target].TintFrom = index;
				}
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Changes
	////////////////////////////////////////////////////////////////////////////////

	void GameUIScene::ProcessChanges()
	{
		// Hierarchy moves (reparenting, sibling order, children created): the canvases they leave and enter are rebuilt.
		m_ChangeScratch.clear();
		if (!m_Scene->GetHierarchyMoves(m_HierarchyVersion, m_ChangeScratch))
		{
			m_CanvasListDirty = true;
		}
		else
		{
			for (const entt::entity entity : m_ChangeScratch)
			{
				if (!m_Registry->valid(entity))
					continue;
				if (m_Registry->all_of<UICanvasComponent>(entity))
				{
					m_CanvasListDirty = true;
					continue;
				}
				const HierarchyComponent& hierarchy = m_Registry->get<HierarchyComponent>(entity);
				if (const ElementRef* ref = FindRef(entity))
					m_Canvases[ref->Canvas].StructureDirty = true;
				else if (hierarchy.ChildCount > 0)
				{
					// An entity that is no element moved with its descendants: canvases it carried or held may change.
					for (Canvas& canvas : m_Canvases)
					{
						if (m_Scene->IsDescendantOf(Entity(canvas.Entity, m_Scene), Entity(entity, m_Scene)))
							m_CanvasListDirty = true;
						canvas.StructureDirty = true;
					}
				}
				m_PendingStructure.push_back(entity);
			}
		}
		m_HierarchyVersion = m_Scene->GetHierarchyVersion();

		if (m_CanvasListDirty)
			RebuildCanvasList();

		// Elements added (or moved) since: their canvases are found now that the hierarchy is complete.
		for (const entt::entity entity : m_PendingStructure)
		{
			if (!m_Registry->valid(entity))
				continue;
			const uint32_t canvas = FindRootCanvas(entity);
			if (canvas != c_NoElement)
				m_Canvases[canvas].StructureDirty = true;
		}
		m_PendingStructure.clear();
		for (uint32_t index = 0; index < m_Canvases.size(); index++)
		{
			if (m_Canvases[index].StructureDirty)
				RebuildCanvas(index);
		}

		// Activity: the quads of elements that appeared or disappeared, and the layout of groups that place them.
		m_ChangeScratch.clear();
		const bool knownChanges = m_Scene->GetWorldTransformChanges(m_TransformsVersion, m_ChangeScratch);
		m_TransformsVersion = m_Scene->GetTransformsVersion();
		auto checkActivity = [&](uint32_t canvasIndex, uint32_t elementIndex)
		{
			Canvas& canvas = m_Canvases[canvasIndex];
			Element& element = canvas.Elements[elementIndex];
			const WorldTransformComponent* world = m_Registry->try_get<WorldTransformComponent>(element.Entity);
			const bool active = !world || world->ActiveInHierarchy;
			if (active == element.Active)
				return;
			element.Active = active;
			MarkElementQuadsDirty(canvasIndex, elementIndex);
			TouchHits(canvasIndex);
			if (element.Parent != c_NoElement && DependsOnChildren(element.Parent, canvas))
				MarkElementLayoutDirty(canvasIndex, element.Parent);
		};
		if (knownChanges)
		{
			for (const entt::entity entity : m_ChangeScratch)
			{
				if (const ElementRef* ref = FindRef(entity))
					checkActivity(ref->Canvas, ref->Element);
			}
		}
		else
		{
			for (uint32_t canvas = 0; canvas < m_Canvases.size(); canvas++)
			{
				for (uint32_t element = 0; element < m_Canvases[canvas].Elements.size(); element++)
					checkActivity(canvas, element);
			}
		}

		// Assets the UI uses that loaded, reloaded or went: texts reshape (a font that loads replaces the default font they
		// were shaped with), images rebuild (textures' sizes place sliced and aspect-kept images).
		AssetManagerBase* assets = m_Scene->GetAssetManager();
		if (assets && assets->GetContentVersion() != m_AssetContentVersion)
		{
			m_AssetScratch.clear();
			const bool known = assets->GetContentChanges(m_AssetContentVersion, m_AssetScratch);
			m_AssetContentVersion = assets->GetContentVersion();
			bool relevant = !known;
			for (const AssetHandle handle : m_AssetScratch)
				relevant = relevant || m_WatchedAssets.contains(handle);
			if (relevant)
				RefreshAssetUsers(known ? std::span<const AssetHandle>(m_AssetScratch) : std::span<const AssetHandle>());
		}
	}

	void GameUIScene::RefreshAssetUsers(std::span<const AssetHandle> changed)
	{
		// Empty `changed`: every asset may have changed.
		auto affected = [&](AssetHandle handle) { return handle.IsValid() && (changed.empty() || std::find(changed.begin(), changed.end(), handle) != changed.end()); };
		for (uint32_t canvasIndex = 0; canvasIndex < m_Canvases.size(); canvasIndex++)
		{
			Canvas& canvas = m_Canvases[canvasIndex];
			for (uint32_t index = 0; index < canvas.Elements.size(); index++)
			{
				const entt::entity entity = canvas.Elements[index].Entity;
				if (const UITextComponent* text = m_Registry->try_get<UITextComponent>(entity); text && affected(text->Font))
				{
					for (Layout& layout : canvas.Layouts)
					{
						if (index < layout.Texts.size())
							layout.Texts[index].Valid = false;
					}
					MarkElementQuadsDirty(canvasIndex, index);
					const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(entity);
					if (rect && rect->FitContent != UIFitContent::None)
						MarkElementLayoutDirty(canvasIndex, index);
				}
				if (const UIImageComponent* image = m_Registry->try_get<UIImageComponent>(entity); image && affected(image->Texture))
					MarkElementQuadsDirty(canvasIndex, index);
			}
		}
	}

	bool GameUIScene::DependsOnChildren(uint32_t element, const Canvas& canvas) const
	{
		const entt::entity entity = canvas.Elements[element].Entity;
		if (m_Registry->all_of<UILayoutComponent>(entity))
			return true;
		const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(entity);
		return element != 0 && rect && rect->FitContent != UIFitContent::None;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Screens
	////////////////////////////////////////////////////////////////////////////////

	std::span<const UICanvasView> GameUIScene::EnsureLayout(const UIScreen& screen, bool screenSpaceCanvases)
	{
		ST_PROFILE_FUNCTION();
		ProcessChanges();

		m_LayoutStamp++;
		m_Views.clear();
		for (uint32_t canvasIndex = 0; canvasIndex < m_Canvases.size(); canvasIndex++)
		{
			Canvas& canvas = m_Canvases[canvasIndex];
			const UICanvasComponent* settings = m_Registry->try_get<UICanvasComponent>(canvas.Entity);
			if (!settings)
				continue;
			// A world-space canvas is a plane of its reference size in units: the screen changes nothing of its layout.
			const bool world = settings->RenderMode == UICanvasRenderMode::WorldSpace;
			if (!world && !screenSpaceCanvases)
				continue;
			const float scale = world ? 1.0f : ComputeScale(*settings, screen);
			const glm::vec2 unitSize = world ? glm::clamp(settings->ReferenceResolution, glm::vec2(1.0f), glm::vec2(16384.0f))
				: glm::max(screen.Size, glm::vec2(0.0f)) / scale;
			uint32_t layoutIndex = 0;
			Layout& layout = GetLayout(canvas, unitSize, layoutIndex);
			layout.LastUsed = m_LayoutStamp;
			UpdateLayout(canvas, layout);
			UpdateClips(canvas, layout);

			UICanvasView& view = m_Views.emplace_back();
			view.Canvas = canvas.Entity;
			view.Scale = scale;
			view.UnitSize = unitSize;
			view.SortOrder = settings->SortOrder;
			view.ReceivesInput = settings->ReceivesInput;
			view.PixelSnap = settings->PixelSnap && !world;
			view.WorldSpace = world;
			view.CanvasIndex = canvasIndex;
			view.LayoutIndex = layoutIndex;
		}
		// Higher sort orders draw later (on top); equal ones keep hierarchy order.
		std::stable_sort(m_Views.begin(), m_Views.end(), [](const UICanvasView& a, const UICanvasView& b) { return a.SortOrder < b.SortOrder; });
		m_ViewsScreen = screen;
		m_ViewsValid = true;
		m_ViewsIncludeScreen = screenSpaceCanvases;

#ifdef ST_DEBUG
		std::string error;
		if (!Validate(&error))
			ST_CORE_ASSERT(false, "{}", error);
#endif
		return m_Views;
	}

	GameUIScene::Layout& GameUIScene::GetLayout(Canvas& canvas, const glm::vec2& unitSize, uint32_t& outIndex)
	{
		for (uint32_t index = 0; index < canvas.Layouts.size(); index++)
		{
			if (SameUnitSize(canvas.Layouts[index].UnitSize, unitSize))
			{
				outIndex = index;
				return canvas.Layouts[index];
			}
		}
		// A new size: a new layout, replacing the one used longest ago when the canvas has c_MaxLayoutsPerCanvas.
		uint32_t index = static_cast<uint32_t>(canvas.Layouts.size());
		if (canvas.Layouts.size() >= c_MaxLayoutsPerCanvas)
		{
			index = 0;
			for (uint32_t other = 1; other < canvas.Layouts.size(); other++)
			{
				if (canvas.Layouts[other].LastUsed < canvas.Layouts[index].LastUsed)
					index = other;
			}
			ReleaseLayoutGlyphs(canvas.Layouts[index]);
			canvas.Layouts[index] = Layout();
		}
		else
		{
			canvas.Layouts.emplace_back();
		}
		Layout& layout = canvas.Layouts[index];
		layout.UnitSize = unitSize;
		layout.AllDirty = true;
		layout.QuadsRepack = true;
		layout.QuadArrayId = m_NextQuadArrayId++;
		layout.Id = m_NextLayoutId++;
		layout.HitStamp = ++m_HitStamp;
		outIndex = index;
		m_ViewsValid = false;
		return layout;
	}

	uint32_t GameUIScene::GetRootCanvasCount()
	{
		// Counted once per hierarchy version, set of canvases and change of their settings: hidden UI costs a comparison per
		// frame.
		if (!HasCanvases(*m_Scene))
			return 0;
		if (m_RootCanvasCountVersion != m_Scene->GetHierarchyVersion() || m_RootCanvasCountStamp != m_CanvasSetStamp + m_CanvasSettingsStamp)
		{
			uint32_t count = 0;
			uint32_t world = 0;
			for (const auto [entity, canvas] : m_Registry->view<UICanvasComponent>().each())
			{
				if (!m_Registry->all_of<HierarchyComponent>(entity))
					continue;
				bool nested = false;
				for (entt::entity parent = m_Registry->get<HierarchyComponent>(entity).Parent; parent != entt::null && !nested;
					parent = m_Registry->get<HierarchyComponent>(parent).Parent)
					nested = m_Registry->all_of<UICanvasComponent>(parent);
				if (nested)
					continue;
				count++;
				world += canvas.RenderMode == UICanvasRenderMode::WorldSpace ? 1 : 0;
			}
			m_RootCanvasCount = count;
			m_WorldCanvasCount = world;
			m_RootCanvasCountVersion = m_Scene->GetHierarchyVersion();
			m_RootCanvasCountStamp = m_CanvasSetStamp + m_CanvasSettingsStamp;
		}
		return m_RootCanvasCount;
	}

	uint32_t GameUIScene::GetWorldCanvasCount()
	{
		GetRootCanvasCount();
		return HasCanvases(*m_Scene) ? m_WorldCanvasCount : 0;
	}

	glm::mat4 GameUIScene::GetCanvasWorldMatrix(const UICanvasView& view) const
	{
		const UICanvasComponent* settings = m_Registry->try_get<UICanvasComponent>(view.Canvas);
		if (!view.WorldSpace || !settings || !m_Registry->valid(view.Canvas))
			return glm::mat4(1.0f);
		const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(view.Canvas);
		const glm::vec2 pivot = rect ? rect->Pivot : glm::vec2(0.5f);
		const float units = std::isfinite(settings->WorldUnitsPerUnit) ? std::clamp(settings->WorldUnitsPerUnit, 1.0e-5f, 100.0f) : 0.01f;
		// Canvas units (+Y down) from the pivot point, to the entity's XY plane (+Y up), then the entity's world transform.
		const glm::mat4 local = glm::scale(glm::mat4(1.0f), glm::vec3(units, -units, units))
			* glm::translate(glm::mat4(1.0f), glm::vec3(-pivot * view.UnitSize, 0.0f));
		return m_Scene->GetWorldTransform(Entity(view.Canvas, m_Scene)) * local;
	}

	uint64_t GameUIScene::GetLayoutId(const UICanvasView& view) const
	{
		return m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex].Id;
	}

	uint64_t GameUIScene::GetHitStamp(const UICanvasView& view) const
	{
		return m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex].HitStamp;
	}

	UIInputScreen GameUIScene::GetInputScreen()
	{
		UIInputScreen input;
		const glm::vec2 viewport = Input::GetViewportSize();
		glm::vec2 pixels = viewport;
		float contentScale = 1.0f;
		if (IGameWindow* window = EngineServices::Get<IGameWindow>())
		{
			const GameWindowState state = window->GetState();
			if (state.Width > 0 && state.Height > 0)
				pixels = glm::vec2(static_cast<float>(state.Width), static_cast<float>(state.Height));
			if (std::isfinite(state.ContentScale) && state.ContentScale > 0.0f)
				contentScale = state.ContentScale;
		}
		if (!(pixels.x > 0.0f && pixels.y > 0.0f))
			return input; // c_DefaultScreen
		input.Screen = UIScreen { pixels, contentScale };
		if (viewport.x > 0.0f && viewport.y > 0.0f)
			input.InputToPixels = pixels / viewport;
		return input;
	}

	uint32_t GameUIScene::GetElementCount(const UICanvasView& view) const
	{
		return view.CanvasIndex < m_Canvases.size() ? static_cast<uint32_t>(m_Canvases[view.CanvasIndex].Elements.size()) : 0;
	}

	entt::entity GameUIScene::GetElementEntity(const UICanvasView& view, uint32_t element) const
	{
		return m_Canvases[view.CanvasIndex].Elements[element].Entity;
	}

	UIBox GameUIScene::GetElementBox(const UICanvasView& view, uint32_t element) const
	{
		const Layout& layout = m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex];
		return element < layout.Boxes.size() ? layout.Boxes[element] : UIBox {};
	}

	bool GameUIScene::IsElementVisible(const UICanvasView& view, uint32_t element) const
	{
		return IsVisible(m_Canvases[view.CanvasIndex], element);
	}

	uint32_t GameUIScene::GetElementSubtreeEnd(const UICanvasView& view, uint32_t element) const
	{
		return m_Canvases[view.CanvasIndex].Elements[element].SubtreeEnd;
	}

	bool GameUIScene::IsRaycastTarget(const UICanvasView& view, uint32_t element) const
	{
		return m_Canvases[view.CanvasIndex].Elements[element].RaycastTarget;
	}

	std::optional<UIBox> GameUIScene::GetElementClip(const UICanvasView& view, uint32_t element) const
	{
		const Canvas& canvas = m_Canvases[view.CanvasIndex];
		const Layout& layout = canvas.Layouts[view.LayoutIndex];
		const uint32_t clip = element < canvas.Elements.size() ? canvas.Elements[element].Clip : 0;
		if (clip == 0 || clip > layout.Clips.size())
			return std::nullopt;
		const glm::vec4& rect = layout.Clips[clip - 1].Rect;
		return UIBox { glm::vec2(rect.x, rect.y), glm::max(glm::vec2(rect.z, rect.w) - glm::vec2(rect.x, rect.y), glm::vec2(0.0f)) };
	}

	UIBox GameUIScene::GetVisibleBox(const UICanvasView& view, uint32_t element) const
	{
		const UIBox box = GetElementBox(view, element);
		const std::optional<UIBox> clip = GetElementClip(view, element);
		if (!clip)
			return box;
		const glm::vec2 min = glm::max(box.Min, clip->Min);
		const glm::vec2 max = glm::min(box.GetMax(), clip->GetMax());
		return UIBox { min, glm::max(max - min, glm::vec2(0.0f)) };
	}

	std::span<const RenderData::UIClipRect> GameUIScene::GetClipRects(const UICanvasView& view) const
	{
		return m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex].Clips;
	}

	uint64_t GameUIScene::GetClipVersion(const UICanvasView& view) const
	{
		return m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex].ClipVersion;
	}

	bool GameUIScene::IsVisible(const Canvas& canvas, uint32_t element) const
	{
		if (!canvas.Elements[element].Active)
			return false;
		// The checkmark of a toggle that is off, and the placeholder of a field with text, hide with their descendants.
		for (uint32_t current = element; current != c_NoElement; current = canvas.Elements[current].Parent)
		{
			const uint32_t field = canvas.Elements[current].PlaceholderOf;
			if (field != c_NoElement && !canvas.Elements[field].FieldEmpty)
				return false;
			const uint32_t toggle = canvas.Elements[current].CheckmarkOf;
			if (toggle == c_NoElement)
				continue;
			const UIToggleComponent* component = m_Registry->try_get<UIToggleComponent>(canvas.Elements[toggle].Entity);
			if (component && !component->IsOn)
				return false;
		}
		return true;
	}

	bool GameUIScene::FindElement(entt::entity entity, const UIScreen& screen, UICanvasView& outView, uint32_t& outElement)
	{
		if (!m_ViewsValid || !(m_ViewsScreen == screen) || !m_ViewsIncludeScreen)
			EnsureLayout(screen);
		const ElementRef* ref = FindRef(entity);
		if (!ref)
			return false;
		for (const UICanvasView& view : m_Views)
		{
			if (view.CanvasIndex == ref->Canvas)
			{
				outView = view;
				outElement = ref->Element;
				return true;
			}
		}
		return false;
	}

	void GameUIScene::SetSelectableState(entt::entity selectable, UIVisualState state, const glm::vec4& tint)
	{
		SelectableState& current = m_SelectableStates[selectable];
		if (current.State == state && current.Tint == tint)
			return;
		current.State = state;
		current.Tint = tint;
		if (const ElementRef* ref = FindRef(selectable))
		{
			const uint32_t target = m_Canvases[ref->Canvas].Elements[ref->Element].TintTarget;
			if (target != c_NoElement)
				MarkElementQuadsDirty(ref->Canvas, target);
		}
	}

	void GameUIScene::ClearSelectableState(entt::entity selectable)
	{
		if (m_SelectableStates.erase(selectable) == 0)
			return;
		if (const ElementRef* ref = FindRef(selectable))
		{
			const uint32_t target = m_Canvases[ref->Canvas].Elements[ref->Element].TintTarget;
			if (target != c_NoElement)
				MarkElementQuadsDirty(ref->Canvas, target);
		}
	}

	void GameUIScene::ClearSelectableStates()
	{
		for (const auto& [selectable, state] : m_SelectableStates)
		{
			const ElementRef* ref = FindRef(selectable);
			const uint32_t target = ref ? m_Canvases[ref->Canvas].Elements[ref->Element].TintTarget : c_NoElement;
			if (target != c_NoElement)
				MarkElementQuadsDirty(ref->Canvas, target);
		}
		m_SelectableStates.clear();
	}

	UIVisualState GameUIScene::GetVisualState(entt::entity selectable, const UISelectableComponent& component) const
	{
		if (!component.Interactable)
			return UIVisualState::Disabled;
		auto it = m_SelectableStates.find(selectable);
		return it != m_SelectableStates.end() ? it->second.State : UIVisualState::Normal;
	}

	glm::vec4 GameUIScene::GetTintColor(entt::entity selectable, const UISelectableComponent& component, UIVisualState state) const
	{
		auto it = m_SelectableStates.find(selectable);
		if (it != m_SelectableStates.end() && it->second.State == state)
			return it->second.Tint;
		switch (state)
		{
		case UIVisualState::Normal: return component.NormalColor;
		case UIVisualState::Hover: return component.HoverColor;
		case UIVisualState::Pressed: return component.PressedColor;
		case UIVisualState::Focused: return component.FocusedColor;
		case UIVisualState::Disabled: return component.DisabledColor;
		}
		return component.NormalColor;
	}

	void GameUIScene::SetInputFieldState(entt::entity field, const UIInputFieldState& state)
	{
		FieldRecord& record = m_FieldStates[field];
		if (record.State == state)
			return;
		record.State = state;
		if (const ElementRef* ref = FindRef(field))
		{
			const uint32_t text = m_Canvases[ref->Canvas].Elements[ref->Element].FieldTextPart;
			if (text != c_NoElement)
				MarkElementQuadsDirty(ref->Canvas, text);
		}
	}

	void GameUIScene::ClearInputFieldState(entt::entity field)
	{
		if (m_FieldStates.erase(field) == 0)
			return;
		if (const ElementRef* ref = FindRef(field))
		{
			const uint32_t text = m_Canvases[ref->Canvas].Elements[ref->Element].FieldTextPart;
			if (text != c_NoElement)
				MarkElementQuadsDirty(ref->Canvas, text);
		}
	}

	std::optional<uint32_t> GameUIScene::FindInputFieldCaret(entt::entity field, const UIScreen& screen, const glm::vec2& point)
	{
		UICanvasView view;
		uint32_t fieldElement = 0;
		const UIInputFieldComponent* component = m_Registry->try_get<UIInputFieldComponent>(field);
		if (!component || !FindElement(field, screen, view, fieldElement))
			return std::nullopt;
		Canvas& canvas = m_Canvases[view.CanvasIndex];
		const uint32_t textPart = canvas.Elements[fieldElement].FieldTextPart;
		const UITextComponent* text = textPart != c_NoElement ? m_Registry->try_get<UITextComponent>(canvas.Elements[textPart].Entity) : nullptr;
		if (!text)
			return std::nullopt;
		Layout& layout = canvas.Layouts[view.LayoutIndex];
		const UIBox box = layout.Boxes[textPart];
		const float size = std::clamp(text->FontSize, 1.0f, 1024.0f);
		AssetHandle shapedFont;
		const FontFace* face = GetTextFace(*text, shapedFont);
		const ShapedText& shaped = ShapeElementText(layout, textPart, *text, component->Text,
			GetElementShapeOptions(canvas, textPart, *text, m_Registry->try_get<UIRectComponent>(canvas.Elements[textPart].Entity), box));
		MeasureCaretStops(*face, component->Text, m_CaretScratch);
		auto found = m_FieldStates.find(field);
		const glm::vec2 line = PlaceFieldLine(*text, shaped, box, size, found != m_FieldStates.end() ? &found->second : nullptr, m_CaretScratch);
		// The stop nearest the point along the line.
		const float x = point.x;
		uint32_t best = 0;
		float bestDistance = std::numeric_limits<float>::max();
		for (const TextCaretStop& stop : m_CaretScratch)
		{
			const float distance = std::abs(line.x + stop.X * size - x);
			if (distance < bestDistance)
			{
				bestDistance = distance;
				best = stop.Byte;
			}
		}
		return best;
	}

	std::string_view GameUIScene::GetElementText(const UICanvasView& view, uint32_t element) const
	{
		const Canvas& canvas = m_Canvases[view.CanvasIndex];
		const UITextComponent* text = element < canvas.Elements.size() ? m_Registry->try_get<UITextComponent>(canvas.Elements[element].Entity) : nullptr;
		return text ? GetShownText(canvas, element, *text) : std::string_view();
	}

	GameUISceneStats GameUIScene::TakeStats()
	{
		const GameUISceneStats stats = m_Stats;
		m_Stats = {};
		return stats;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Validation
	////////////////////////////////////////////////////////////////////////////////

	bool GameUIScene::Validate(std::string* outError)
	{
		auto fail = [&](entt::entity entity, const std::string& what)
		{
			if (outError)
			{
				const IDComponent* id = m_Registry->try_get<IDComponent>(entity);
				const NameComponent* name = m_Registry->try_get<NameComponent>(entity);
				*outError = fmt::format("The UI layout of scene '{}' differs from a full recomputation at entity '{}' ({}): {} (was a UI component "
					"written without a signal? See GameUIScene)", m_Scene->GetName(), name ? name->Name : std::string(), id ? id->ID.ToString() : std::string("?"), what);
			}
			return false;
		};

		m_Validating = true;
		bool valid = true;
		for (Canvas& canvas : m_Canvases)
		{
			if (canvas.StructureDirty || !m_Registry->valid(canvas.Entity))
				continue; // Not brought up to date yet: compared once it is
			for (uint32_t index = 0; index < canvas.Elements.size() && valid; index++)
			{
				const Element& element = canvas.Elements[index];
				const WorldTransformComponent* world = m_Registry->try_get<WorldTransformComponent>(element.Entity);
				if (element.Active != (!world || world->ActiveInHierarchy))
					valid = fail(element.Entity, "its activity changed unnoticed");
				else if (element.RaycastTarget != ComputeRaycastTarget(element.Entity))
					valid = fail(element.Entity, "whether it is a raycast target changed unnoticed");
				else if (const UIInputFieldComponent* field = m_Registry->try_get<UIInputFieldComponent>(element.Entity);
					field && field->Text.empty() != element.FieldEmpty)
					valid = fail(element.Entity, "its input field's text changed unnoticed");
			}
			for (Layout& layout : canvas.Layouts)
			{
				if (!valid || layout.AllDirty || !layout.Dirty.empty())
					continue;
				std::vector<UIBox> boxes = layout.Boxes;
				LayOutSubtree(canvas, layout, 0, boxes);
				for (uint32_t index = 0; index < boxes.size() && valid; index++)
				{
					if (!SameBox(boxes[index], layout.Boxes[index], 1e-4f))
					{
						valid = fail(canvas.Elements[index].Entity, fmt::format("cached rect ({}, {}) {}x{}, recomputed ({}, {}) {}x{} (layout {}x{})",
							layout.Boxes[index].Min.x, layout.Boxes[index].Min.y, layout.Boxes[index].Size.x, layout.Boxes[index].Size.y, boxes[index].Min.x,
							boxes[index].Min.y, boxes[index].Size.x, boxes[index].Size.y, layout.UnitSize.x, layout.UnitSize.y));
					}
				}
				if (!valid || layout.ClipsDirty)
					continue;
				// The clip rects follow the boxes and the masks' values.
				std::vector<RenderData::UIClipRect> clips;
				ComputeClips(canvas, layout.Boxes, clips);
				for (uint32_t slot = 0; slot < clips.size() && valid; slot++)
				{
					const RenderData::UIClipRect* cached = slot < layout.Clips.size() ? &layout.Clips[slot] : nullptr;
					const UIBox recomputed { glm::vec2(clips[slot].Rect), glm::vec2(clips[slot].Rect.z, clips[slot].Rect.w) - glm::vec2(clips[slot].Rect) };
					const bool same = cached && SameBox(UIBox { glm::vec2(cached->Rect), glm::vec2(cached->Rect.z, cached->Rect.w) - glm::vec2(cached->Rect) },
						recomputed, 1e-4f) && std::abs(cached->Parameters.x - clips[slot].Parameters.x) <= 1e-4f;
					if (!same)
						valid = fail(canvas.Elements[canvas.Masks[slot]].Entity, "its mask's clip rect changed unnoticed");
				}
			}
		}
		m_Validating = false;
		return valid;
	}

}
