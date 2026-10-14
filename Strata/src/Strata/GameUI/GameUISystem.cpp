#include "stpch.h"
#include "Strata/GameUI/GameUISystem.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Log.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/MouseCodes.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneHierarchy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Strata
{

	namespace
	{

		constexpr UIAction c_Directions[] = { UIAction::Up, UIAction::Down, UIAction::Left, UIAction::Right };

		glm::vec2 GetDirectionVector(UIAction action)
		{
			// Canvas and screen coordinates grow right and down.
			switch (action)
			{
				case UIAction::Up: return { 0.0f, -1.0f };
				case UIAction::Down: return { 0.0f, 1.0f };
				case UIAction::Left: return { -1.0f, 0.0f };
				case UIAction::Right: return { 1.0f, 0.0f };
				default: return { 0.0f, 0.0f };
			}
		}

		glm::vec4 GetStateColor(const UISelectableComponent& selectable, UIVisualState state)
		{
			if (selectable.Transition != UISelectableTransition::ColorTint)
				return glm::vec4(1.0f);
			switch (state)
			{
				case UIVisualState::Normal: return selectable.NormalColor;
				case UIVisualState::Hover: return selectable.HoverColor;
				case UIVisualState::Pressed: return selectable.PressedColor;
				case UIVisualState::Focused: return selectable.FocusedColor;
				case UIVisualState::Disabled: return selectable.DisabledColor;
			}
			return selectable.NormalColor;
		}

		// Snaps and clamps a slider value to its range (either way round).
		float ClampSliderValue(const UISliderComponent& slider, float value)
		{
			const float low = std::min(slider.MinValue, slider.MaxValue);
			const float high = std::max(slider.MinValue, slider.MaxValue);
			if (!std::isfinite(value))
				value = low;
			value = std::clamp(value, low, high);
			if (slider.WholeNumbers)
				value = std::clamp(std::round(value), std::ceil(low), std::floor(high));
			return value;
		}

	}

	GameUISystem::GameUISystem(Scene& scene)
		: m_Scene(scene), m_CanvasStorage(&scene.GetRegistry().storage<UICanvasComponent>())
	{
		scene.RegisterService<IUIEvents>(*this);
		scene.RegisterService<IUIControl>(*this);
	}

	GameUISystem::~GameUISystem()
	{
		m_Scene.UnregisterService<IUIControl>(*this);
		m_Scene.UnregisterService<IUIEvents>(*this);
	}

	void GameUISystem::OnRuntimeStop()
	{
		// Clicks belong to the game again, and selectables show their rest states.
		Input::SetPointerCapture(false);
		if (GameUIScene* ui = GameUIScene::Find(m_Scene))
			ui->ClearSelectableStates();
		m_Fades.clear();
		m_PendingEvents.clear();
		m_Gesture = Gesture::None;
		m_Pressed = m_Dragging = m_Hovered = m_HoveredTarget = m_Focused = entt::null;
		m_PointerOverUI = false;
	}

	void GameUISystem::OnUpdate(Timestep)
	{
		if (m_CanvasStorage->empty())
		{
			// No UI: the game gets every click (one storage-size check per frame).
			if (m_Gesture != Gesture::None || m_PointerOverUI || m_Focused != entt::null || Input::IsPointerCaptured())
			{
				Input::SetPointerCapture(false);
				m_Gesture = Gesture::None;
				m_PointerOverUI = false;
				m_Pressed = m_Dragging = m_Hovered = m_HoveredTarget = m_Focused = entt::null;
			}
			return;
		}

		// Menus work at time scale 0: everything here runs on unscaled time.
		const float deltaTime = m_Scene.GetUnscaledDeltaTime();
		GameUIScene& ui = GameUIScene::Get(m_Scene);
		const UIInputScreen screen = GameUIScene::GetInputScreen();
		RefreshViews(ui, screen.Screen);
		const bool layoutChanged = m_HitTester.Update(ui, m_Views);
		m_Actions.Update(deltaTime);

		// A focus that became unusable (disabled, hidden, gone) is dropped.
		if (m_Focused != entt::null)
		{
			UIBox box;
			if (!IsInteractable(m_Focused) || !FindOnScreen(ui, m_Focused, box))
				Focus(entt::null);
		}

		UpdatePointer(ui, screen, layoutChanged);
		UpdateNavigation(ui, m_Views);
		UpdateVisualStates(ui, deltaTime);
		DispatchEvents();
	}

	void GameUISystem::OnEntityDestroying(const Entity& entity)
	{
		const entt::entity handle = entity.GetHandle();
		for (entt::entity* reference : { &m_HoveredTarget, &m_Hovered, &m_Pressed, &m_Dragging, &m_Focused })
		{
			if (*reference == handle)
				*reference = entt::null;
		}
		if (m_Fades.erase(handle) != 0)
		{
			if (GameUIScene* ui = GameUIScene::Find(m_Scene))
				ui->ClearSelectableState(handle);
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Pointer
	////////////////////////////////////////////////////////////////////////////////

	void GameUISystem::UpdatePointer(GameUIScene& ui, const UIInputScreen& screen, bool layoutChanged)
	{
		const PointerState pointer = Input::GetPointerState();
		const glm::vec2 pixel = pointer.Position * screen.InputToPixels;
		const bool moved = !m_PointerKnown || pixel != m_Pointer;
		m_Pointer = pixel;
		m_PointerKnown = true;

		// Hover follows only what can change it: the pointer, the layout (which destroyed elements change), presses and
		// releases.
		if (moved || layoutChanged || pointer.Pressed != 0 || pointer.Released != 0)
		{
			const std::optional<UIHitResult> hit = m_HitTester.HitTest(pixel);
			m_HoveredTarget = hit ? hit->Entity : entt::null;
			m_PointerOverUI = hit.has_value();
			const entt::entity selectable = hit ? FindSelectable(hit->Entity) : entt::null;
			m_Hovered = selectable != entt::null && IsInteractable(selectable) ? selectable : entt::null;
		}

		constexpr uint32_t c_Left = 1u << Mouse::ButtonLeft;
		// A press starts a gesture: the UI's when it is over the UI, else the game's, each until every button is released.
		if (pointer.Pressed != 0 && m_Gesture == Gesture::None)
		{
			m_Gesture = m_PointerOverUI ? Gesture::UI : Gesture::Game;
			if (m_Gesture == Gesture::UI && (pointer.Pressed & c_Left) != 0 && m_Hovered != entt::null)
			{
				m_Pressed = m_Hovered;
				Focus(m_Pressed); // A click on a selectable focuses it
				if (m_Scene.GetRegistry().all_of<UISliderComponent>(m_Pressed))
					m_Dragging = m_Pressed;
			}
		}
		// A slider follows the pointer while it is dragged, also outside its rect.
		if (m_Dragging != entt::null && (pointer.Down & c_Left) != 0)
			DragSlider(ui, m_Dragging, pixel);
		if ((pointer.Released & c_Left) != 0)
		{
			// A click is a release over what was pressed: a press dragged off and released elsewhere clicks nothing.
			if (m_Pressed != entt::null && m_Pressed == m_Hovered && m_Pressed != m_Dragging)
				Click(m_Pressed);
			m_Pressed = entt::null;
			m_Dragging = entt::null;
		}

		// The UI's gestures and the scrolling over it are hidden from the game, also in the frame of the release.
		Input::SetPointerCapture(m_Gesture == Gesture::UI || (m_Gesture == Gesture::None && m_PointerOverUI));
		if (pointer.Down == 0)
			m_Gesture = Gesture::None;
	}

	entt::entity GameUISystem::FindSelectable(entt::entity element) const
	{
		const entt::registry& registry = m_Scene.GetRegistry();
		for (entt::entity current = element; current != entt::null && registry.valid(current);)
		{
			if (registry.all_of<UISelectableComponent>(current))
				return current;
			if (registry.all_of<UICanvasComponent>(current))
				break; // Not beyond the canvas
			const HierarchyComponent* hierarchy = registry.try_get<HierarchyComponent>(current);
			current = hierarchy ? hierarchy->Parent : entt::null;
		}
		return entt::null;
	}

	bool GameUISystem::IsInteractable(entt::entity selectable) const
	{
		const entt::registry& registry = m_Scene.GetRegistry();
		if (selectable == entt::null || !registry.valid(selectable))
			return false;
		const UISelectableComponent* component = registry.try_get<UISelectableComponent>(selectable);
		const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(selectable);
		return component && component->Interactable && (!world || world->ActiveInHierarchy);
	}

	void GameUISystem::RefreshViews(GameUIScene& ui, const UIScreen& screen)
	{
		const std::span<const UICanvasView> views = ui.EnsureLayout(screen);
		m_Views.assign(views.begin(), views.end());
		m_ViewScreen = screen;
	}

	bool GameUISystem::FindOnScreen(GameUIScene& ui, entt::entity entity, UIBox& outPixels) const
	{
		UICanvasView view;
		uint32_t element = 0;
		if (!ui.FindElement(entity, m_ViewScreen, view, element) || !view.ReceivesInput || !ui.IsElementVisible(view, element))
			return false;
		const UIBox box = ui.GetElementBox(view, element);
		outPixels.Min = box.Min * view.Scale;
		outPixels.Size = box.Size * view.Scale;
		return true;
	}

	void GameUISystem::Click(entt::entity selectable)
	{
		entt::registry& registry = m_Scene.GetRegistry();
		if (registry.all_of<UIButtonComponent>(selectable))
			QueueEvent(UIEventType::Click, selectable);
		if (const UIToggleComponent* toggle = registry.try_get<UIToggleComponent>(selectable))
			SetToggle(selectable, !toggle->IsOn);
	}

	void GameUISystem::SetToggle(entt::entity toggle, bool on)
	{
		entt::registry& registry = m_Scene.GetRegistry();
		const UIToggleComponent& component = registry.get<UIToggleComponent>(toggle);
		if (component.IsOn == on)
			return;
		const UUID group = component.Group;
		if (group.IsValid())
		{
			// One of a group is on at a time: switching the one that is on off does nothing, switching another on turns it off.
			if (!on)
				return;
			for (const auto [other, otherToggle] : registry.view<UIToggleComponent>().each())
			{
				if (other == toggle || !otherToggle.IsOn || otherToggle.Group != group)
					continue;
				registry.patch<UIToggleComponent>(other, [](UIToggleComponent& value) { value.IsOn = false; });
				QueueEvent(UIEventType::ValueChanged, other, 0.0f);
			}
		}
		registry.patch<UIToggleComponent>(toggle, [on](UIToggleComponent& value) { value.IsOn = on; });
		QueueEvent(UIEventType::ValueChanged, toggle, on ? 1.0f : 0.0f);
	}

	void GameUISystem::SetSliderValue(entt::entity slider, float value)
	{
		entt::registry& registry = m_Scene.GetRegistry();
		const UISliderComponent& component = registry.get<UISliderComponent>(slider);
		const float clamped = ClampSliderValue(component, value);
		if (clamped == component.Value)
			return;
		registry.patch<UISliderComponent>(slider, [clamped](UISliderComponent& current) { current.Value = clamped; });
		QueueEvent(UIEventType::ValueChanged, slider, clamped);
	}

	void GameUISystem::DragSlider(GameUIScene& ui, entt::entity slider, const glm::vec2& pixel)
	{
		const UISliderComponent* component = m_Scene.GetRegistry().try_get<UISliderComponent>(slider);
		UIBox box;
		if (!component || !FindOnScreen(ui, slider, box))
			return;
		const bool horizontal = component->Direction == UIFillDirection::LeftToRight || component->Direction == UIFillDirection::RightToLeft;
		const int axis = horizontal ? 0 : 1;
		if (!(box.Size[axis] > 0.0f))
			return;
		float fraction = std::clamp((pixel[axis] - box.Min[axis]) / box.Size[axis], 0.0f, 1.0f);
		if (component->Direction == UIFillDirection::RightToLeft || component->Direction == UIFillDirection::BottomToTop)
			fraction = 1.0f - fraction;
		SetSliderValue(slider, component->MinValue + (component->MaxValue - component->MinValue) * fraction);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Focus and navigation
	////////////////////////////////////////////////////////////////////////////////

	Entity GameUISystem::GetFocused() const
	{
		return m_Focused != entt::null ? Entity(m_Focused, &m_Scene) : Entity();
	}

	Entity GameUISystem::GetHovered() const
	{
		return m_Hovered != entt::null ? Entity(m_Hovered, &m_Scene) : Entity();
	}

	bool GameUISystem::SetFocus(Entity selectable, std::string* outError)
	{
		if (!selectable)
		{
			Focus(entt::null);
			return true;
		}
		auto fail = [&](std::string reason)
		{
			if (outError)
				*outError = std::move(reason);
			return false;
		};
		if (selectable.GetScene() != &m_Scene || !m_Scene.GetRegistry().valid(selectable.GetHandle()))
			return fail("the entity is not in the playing scene");
		if (!selectable.HasComponent<UISelectableComponent>())
			return fail(fmt::format("'{}' has no UISelectable component", selectable.GetName()));
		if (!IsInteractable(selectable.GetHandle()))
			return fail(fmt::format("'{}' is not interactable or not active", selectable.GetName()));
		if (!GameUIScene::HasCanvases(m_Scene))
			return fail(fmt::format("'{}' is not on a canvas", selectable.GetName()));
		// Also before the first update, and after changes since the latest one: the layout the player will see next.
		GameUIScene& ui = GameUIScene::Get(m_Scene);
		RefreshViews(ui, GameUIScene::GetInputScreen().Screen);
		UIBox box;
		if (!FindOnScreen(ui, selectable.GetHandle(), box))
			return fail(fmt::format("'{}' is not shown on a canvas that receives input", selectable.GetName()));
		Focus(selectable.GetHandle());
		return true;
	}

	void GameUISystem::Focus(entt::entity selectable)
	{
		if (selectable == m_Focused)
			return;
		const entt::entity previous = m_Focused;
		m_Focused = selectable;
		if (previous != entt::null && m_Scene.GetRegistry().valid(previous))
			QueueEvent(UIEventType::FocusChanged, previous, 0.0f, false);
		if (selectable != entt::null)
			QueueEvent(UIEventType::FocusChanged, selectable, 0.0f, true);
	}

	void GameUISystem::UpdateNavigation(GameUIScene& ui, std::span<const UICanvasView> views)
	{
		for (const UIAction direction : c_Directions)
		{
			if (!m_Actions.IsRepeated(direction))
				continue;
			// Navigating without a focus starts it.
			if (m_Focused == entt::null)
			{
				Focus(FindStartSelectable(ui, views));
				continue;
			}
			// A focused slider steps along its direction instead of navigating.
			if (m_Scene.GetRegistry().all_of<UISliderComponent>(m_Focused) && StepSlider(m_Focused, direction))
				continue;
			const entt::entity next = Navigate(ui, views, m_Focused, direction);
			if (next != entt::null)
				Focus(next);
		}
		if (m_Actions.IsPressed(UIAction::Submit))
		{
			if (m_Focused == entt::null)
				Focus(FindStartSelectable(ui, views));
			else
				Click(m_Focused);
		}
	}

	bool GameUISystem::StepSlider(entt::entity slider, UIAction action)
	{
		const UISliderComponent& component = m_Scene.GetRegistry().get<UISliderComponent>(slider);
		const bool horizontal = component.Direction == UIFillDirection::LeftToRight || component.Direction == UIFillDirection::RightToLeft;
		const bool along = horizontal ? (action == UIAction::Left || action == UIAction::Right) : (action == UIAction::Up || action == UIAction::Down);
		if (!along)
			return false;
		// Towards the end of the slider's direction raises the value.
		const bool towardEnd = (component.Direction == UIFillDirection::LeftToRight && action == UIAction::Right)
			|| (component.Direction == UIFillDirection::RightToLeft && action == UIAction::Left)
			|| (component.Direction == UIFillDirection::TopToBottom && action == UIAction::Down)
			|| (component.Direction == UIFillDirection::BottomToTop && action == UIAction::Up);
		float step = component.KeyboardStep > 0.0f ? component.KeyboardStep : std::abs(component.MaxValue - component.MinValue) / 10.0f;
		if (component.WholeNumbers)
			step = std::max(1.0f, std::round(step));
		const float sign = component.MaxValue >= component.MinValue ? 1.0f : -1.0f;
		SetSliderValue(slider, component.Value + (towardEnd ? step : -step) * sign);
		return true;
	}

	entt::entity GameUISystem::FindStartSelectable(GameUIScene& ui, std::span<const UICanvasView> views) const
	{
		const entt::registry& registry = m_Scene.GetRegistry();
		// The topmost canvas that receives input and has a selectable: its FirstSelected, else its top-left selectable.
		for (size_t index = views.size(); index-- > 0;)
		{
			const UICanvasView& view = views[index];
			if (!view.ReceivesInput)
				continue;
			const UICanvasComponent* canvas = registry.try_get<UICanvasComponent>(view.Canvas);
			if (canvas && canvas->FirstSelected.IsValid())
			{
				const Entity first = m_Scene.GetEntityByUUID(canvas->FirstSelected);
				UIBox box;
				if (first && IsInteractable(first.GetHandle()) && FindOnScreen(ui, first.GetHandle(), box))
					return first.GetHandle();
			}
			entt::entity best = entt::null;
			glm::vec2 bestCorner(std::numeric_limits<float>::max());
			for (uint32_t element = 0; element < ui.GetElementCount(view); element++)
			{
				const entt::entity entity = ui.GetElementEntity(view, element);
				const UISelectableComponent* selectable = registry.try_get<UISelectableComponent>(entity);
				if (!selectable || selectable->Navigation == UINavigation::None || !IsInteractable(entity) || !ui.IsElementVisible(view, element))
					continue;
				const glm::vec2 corner = ui.GetElementBox(view, element).Min;
				if (corner.y < bestCorner.y - 1e-3f || (std::abs(corner.y - bestCorner.y) <= 1e-3f && corner.x < bestCorner.x))
				{
					best = entity;
					bestCorner = corner;
				}
			}
			if (best != entt::null)
				return best;
		}
		return entt::null;
	}

	entt::entity GameUISystem::Navigate(GameUIScene& ui, std::span<const UICanvasView> views, entt::entity from, UIAction direction) const
	{
		const entt::registry& registry = m_Scene.GetRegistry();
		const UISelectableComponent* selectable = registry.try_get<UISelectableComponent>(from);
		if (!selectable || selectable->Navigation == UINavigation::None)
			return entt::null;
		if (selectable->Navigation == UINavigation::Explicit)
		{
			const UUID target = direction == UIAction::Up ? selectable->NavigateUp
				: direction == UIAction::Down ? selectable->NavigateDown
				: direction == UIAction::Left ? selectable->NavigateLeft : selectable->NavigateRight;
			const Entity next = m_Scene.GetEntityByUUID(target);
			UIBox box;
			return next && next.GetHandle() != from && IsInteractable(next.GetHandle()) && FindOnScreen(ui, next.GetHandle(), box) ? next.GetHandle() : entt::null;
		}

		// Automatic: the selectable ahead with the lowest along + 2 x across distance between the centers (in screen pixels,
		// so canvases of different scales compare); ties keep draw order.
		UIBox fromBox;
		if (!FindOnScreen(ui, from, fromBox))
			return entt::null;
		const glm::vec2 origin = fromBox.GetCenter();
		const glm::vec2 axis = GetDirectionVector(direction);
		entt::entity best = entt::null;
		float bestScore = std::numeric_limits<float>::max();
		for (const UICanvasView& view : views)
		{
			if (!view.ReceivesInput)
				continue;
			for (uint32_t element = 0; element < ui.GetElementCount(view); element++)
			{
				const entt::entity entity = ui.GetElementEntity(view, element);
				if (entity == from)
					continue;
				const UISelectableComponent* candidate = registry.try_get<UISelectableComponent>(entity);
				if (!candidate || candidate->Navigation == UINavigation::None || !IsInteractable(entity) || !ui.IsElementVisible(view, element))
					continue;
				const glm::vec2 offset = ui.GetElementBox(view, element).GetCenter() * view.Scale - origin;
				const float along = glm::dot(offset, axis);
				if (along <= 1e-3f * view.Scale) // Ahead by more than 1e-3 canvas units
					continue;
				const float across = std::abs(offset.x * axis.y - offset.y * axis.x);
				const float score = along + 2.0f * across;
				if (score < bestScore)
				{
					bestScore = score;
					best = entity;
				}
			}
		}
		return best;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Visual states
	////////////////////////////////////////////////////////////////////////////////

	UIVisualState GameUISystem::GetDesiredState(entt::entity selectable) const
	{
		if (!IsInteractable(selectable))
			return UIVisualState::Disabled;
		if (selectable == m_Pressed && selectable == m_Hovered)
			return UIVisualState::Pressed;
		if (selectable == m_Hovered)
			return UIVisualState::Hover;
		if (selectable == m_Focused)
			return UIVisualState::Focused;
		return UIVisualState::Normal;
	}

	void GameUISystem::UpdateVisualStates(GameUIScene& ui, float deltaTime)
	{
		// Only what shows a state of the system's (hovered, pressed, focused) or still fades: idle frames cost nothing.
		const entt::registry& registry = m_Scene.GetRegistry();
		for (const entt::entity selectable : { m_Hovered, m_Pressed, m_Focused })
		{
			if (selectable != entt::null && registry.all_of<UISelectableComponent>(selectable) && !m_Fades.contains(selectable))
			{
				const UISelectableComponent& component = registry.get<UISelectableComponent>(selectable);
				Fade fade;
				fade.From = fade.To = GetStateColor(component, UIVisualState::Normal);
				m_Fades.emplace(selectable, fade);
			}
		}
		for (auto it = m_Fades.begin(); it != m_Fades.end();)
		{
			const entt::entity selectable = it->first;
			const UISelectableComponent* component = registry.valid(selectable) ? registry.try_get<UISelectableComponent>(selectable) : nullptr;
			if (!component)
			{
				ui.ClearSelectableState(selectable);
				it = m_Fades.erase(it);
				continue;
			}
			Fade& fade = it->second;
			const UIVisualState desired = GetDesiredState(selectable);
			if (desired != fade.State)
			{
				// From wherever the fade is now, to the new state's color.
				const float progress = fade.Duration > 0.0f ? std::min(fade.Elapsed / fade.Duration, 1.0f) : 1.0f;
				fade.From = glm::mix(fade.From, fade.To, progress);
				fade.To = GetStateColor(*component, desired);
				fade.State = desired;
				fade.Elapsed = 0.0f;
				fade.Duration = component->Transition == UISelectableTransition::ColorTint ? std::max(component->FadeDuration, 0.0f) : 0.0f;
			}
			else
			{
				fade.Elapsed += deltaTime;
			}
			const float progress = fade.Duration > 0.0f ? std::min(fade.Elapsed / fade.Duration, 1.0f) : 1.0f;
			// The rest states (Normal; Disabled the scene shows by itself) need no record once their fade is done.
			if (progress >= 1.0f && (desired == UIVisualState::Normal || desired == UIVisualState::Disabled))
			{
				ui.ClearSelectableState(selectable);
				it = m_Fades.erase(it);
				continue;
			}
			ui.SetSelectableState(selectable, desired, glm::mix(fade.From, fade.To, progress));
			++it;
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// IUIControl
	////////////////////////////////////////////////////////////////////////////////

	template<typename Component>
	Component* GameUISystem::FindElementComponent(Entity element, const char* componentName, std::string* outError)
	{
		if (!element || element.GetScene() != &m_Scene || !m_Scene.GetRegistry().valid(element.GetHandle()))
		{
			if (outError)
				*outError = "the entity is not in the playing scene";
			return nullptr;
		}
		Component* component = m_Scene.GetRegistry().try_get<Component>(element.GetHandle());
		if (!component && outError)
			*outError = fmt::format("'{}' has no {} component", element.GetName(), componentName);
		return component;
	}

	bool GameUISystem::GetText(Entity element, std::string& outText, std::string* outError)
	{
		const UITextComponent* text = FindElementComponent<UITextComponent>(element, "UIText", outError);
		if (!text)
			return false;
		outText = text->Text;
		return true;
	}

	bool GameUISystem::SetText(Entity element, std::string_view text, std::string* outError)
	{
		const UITextComponent* component = FindElementComponent<UITextComponent>(element, "UIText", outError);
		if (!component)
			return false;
		// Unchanged text is no change: games set their HUD every frame, and a signal would reshape it.
		if (component->Text != text)
			m_Scene.GetRegistry().patch<UITextComponent>(element.GetHandle(), [text](UITextComponent& value) { value.Text = std::string(text); });
		return true;
	}

	bool GameUISystem::GetImage(Entity element, AssetHandle& outTexture, std::string* outError)
	{
		const UIImageComponent* image = FindElementComponent<UIImageComponent>(element, "UIImage", outError);
		if (!image)
			return false;
		outTexture = image->Texture;
		return true;
	}

	bool GameUISystem::SetImage(Entity element, AssetHandle texture, std::string* outError)
	{
		const UIImageComponent* image = FindElementComponent<UIImageComponent>(element, "UIImage", outError);
		if (!image)
			return false;
		if (texture.IsValid())
		{
			const AssetManagerBase* assets = m_Scene.GetAssetManager();
			if (!assets || assets->GetAssetType(texture) != AssetType::Texture)
			{
				if (outError)
					*outError = fmt::format("asset {} is not a texture", texture.ToString());
				return false;
			}
		}
		if (image->Texture != texture)
			m_Scene.GetRegistry().patch<UIImageComponent>(element.GetHandle(), [texture](UIImageComponent& value) { value.Texture = texture; });
		return true;
	}

	bool GameUISystem::GetColor(Entity element, glm::vec4& outColor, std::string* outError)
	{
		entt::registry& registry = m_Scene.GetRegistry();
		if (!element || element.GetScene() != &m_Scene || !registry.valid(element.GetHandle()))
			return FindElementComponent<UIImageComponent>(element, "UIImage", outError) != nullptr;
		if (const UIImageComponent* image = registry.try_get<UIImageComponent>(element.GetHandle()))
		{
			outColor = image->Color;
			return true;
		}
		if (const UITextComponent* text = registry.try_get<UITextComponent>(element.GetHandle()))
		{
			outColor = text->Color;
			return true;
		}
		if (outError)
			*outError = fmt::format("'{}' has neither a UIImage nor a UIText component", element.GetName());
		return false;
	}

	bool GameUISystem::SetColor(Entity element, const glm::vec4& color, std::string* outError)
	{
		entt::registry& registry = m_Scene.GetRegistry();
		if (!element || element.GetScene() != &m_Scene || !registry.valid(element.GetHandle()))
			return FindElementComponent<UIImageComponent>(element, "UIImage", outError) != nullptr;
		if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b) || !std::isfinite(color.a))
		{
			if (outError)
				*outError = "the color must be finite";
			return false;
		}
		const glm::vec4 clamped = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
		if (const UIImageComponent* image = registry.try_get<UIImageComponent>(element.GetHandle()))
		{
			if (image->Color != clamped)
				registry.patch<UIImageComponent>(element.GetHandle(), [&clamped](UIImageComponent& value) { value.Color = clamped; });
			return true;
		}
		if (const UITextComponent* text = registry.try_get<UITextComponent>(element.GetHandle()))
		{
			if (text->Color != clamped)
				registry.patch<UITextComponent>(element.GetHandle(), [&clamped](UITextComponent& value) { value.Color = clamped; });
			return true;
		}
		if (outError)
			*outError = fmt::format("'{}' has neither a UIImage nor a UIText component", element.GetName());
		return false;
	}

	bool GameUISystem::IsInteractable(Entity element, bool& outInteractable, std::string* outError)
	{
		const UISelectableComponent* selectable = FindElementComponent<UISelectableComponent>(element, "UISelectable", outError);
		if (!selectable)
			return false;
		outInteractable = selectable->Interactable;
		return true;
	}

	bool GameUISystem::SetInteractable(Entity element, bool interactable, std::string* outError)
	{
		const UISelectableComponent* selectable = FindElementComponent<UISelectableComponent>(element, "UISelectable", outError);
		if (!selectable)
			return false;
		if (selectable->Interactable != interactable)
			m_Scene.GetRegistry().patch<UISelectableComponent>(element.GetHandle(), [interactable](UISelectableComponent& value) { value.Interactable = interactable; });
		// A disabled selectable loses the focus at the next update (OnUpdate checks it), with its events.
		return true;
	}

	bool GameUISystem::GetValue(Entity element, float& outValue, std::string* outError)
	{
		entt::registry& registry = m_Scene.GetRegistry();
		if (!element || element.GetScene() != &m_Scene || !registry.valid(element.GetHandle()))
			return FindElementComponent<UISliderComponent>(element, "UISlider", outError) != nullptr;
		if (const UISliderComponent* slider = registry.try_get<UISliderComponent>(element.GetHandle()))
		{
			outValue = slider->Value;
			return true;
		}
		if (const UIToggleComponent* toggle = registry.try_get<UIToggleComponent>(element.GetHandle()))
		{
			outValue = toggle->IsOn ? 1.0f : 0.0f;
			return true;
		}
		if (outError)
			*outError = fmt::format("'{}' has neither a UISlider nor a UIToggle component", element.GetName());
		return false;
	}

	bool GameUISystem::SetValue(Entity element, float value, std::string* outError)
	{
		entt::registry& registry = m_Scene.GetRegistry();
		if (!element || element.GetScene() != &m_Scene || !registry.valid(element.GetHandle()))
			return FindElementComponent<UISliderComponent>(element, "UISlider", outError) != nullptr;
		if (!std::isfinite(value))
		{
			if (outError)
				*outError = "the value must be finite";
			return false;
		}
		if (const UISliderComponent* slider = registry.try_get<UISliderComponent>(element.GetHandle()))
		{
			const float clamped = ClampSliderValue(*slider, value);
			if (clamped != slider->Value)
				registry.patch<UISliderComponent>(element.GetHandle(), [clamped](UISliderComponent& current) { current.Value = clamped; });
			return true;
		}
		if (const UIToggleComponent* toggle = registry.try_get<UIToggleComponent>(element.GetHandle()))
		{
			const bool on = value != 0.0f;
			if (toggle->IsOn != on)
				registry.patch<UIToggleComponent>(element.GetHandle(), [on](UIToggleComponent& current) { current.IsOn = on; });
			if (on)
				EnforceGroup(element.GetHandle());
			return true;
		}
		if (outError)
			*outError = fmt::format("'{}' has neither a UISlider nor a UIToggle component", element.GetName());
		return false;
	}

	void GameUISystem::EnforceGroup(entt::entity toggle)
	{
		entt::registry& registry = m_Scene.GetRegistry();
		const UUID group = registry.get<UIToggleComponent>(toggle).Group;
		if (!group.IsValid())
			return;
		for (const auto [other, otherToggle] : registry.view<UIToggleComponent>().each())
		{
			if (other != toggle && otherToggle.IsOn && otherToggle.Group == group)
				registry.patch<UIToggleComponent>(other, [](UIToggleComponent& value) { value.IsOn = false; });
		}
	}

	bool GameUISystem::IsActionPressed(uint32_t action)
	{
		return action < c_UIActionCount && m_Actions.IsPressed(static_cast<UIAction>(action));
	}

	bool GameUISystem::IsActionDown(uint32_t action)
	{
		return action < c_UIActionCount && m_Actions.IsDown(static_cast<UIAction>(action));
	}

	////////////////////////////////////////////////////////////////////////////////
	// Events
	////////////////////////////////////////////////////////////////////////////////

	void GameUISystem::QueueEvent(UIEventType type, entt::entity widget, float value, bool focused)
	{
		const entt::registry& registry = m_Scene.GetRegistry();
		UIEventInfo event;
		event.Type = type;
		event.Widget = Entity(widget, &m_Scene);
		event.WidgetID = registry.all_of<IDComponent>(widget) ? registry.get<IDComponent>(widget).ID : UUID::Null();
		event.Value = value;
		event.Focused = focused;
		// The widget's event target, when it names another entity.
		UUID target = UUID::Null();
		if (const UIButtonComponent* button = registry.try_get<UIButtonComponent>(widget))
			target = button->EventTarget;
		else if (const UIToggleComponent* toggle = registry.try_get<UIToggleComponent>(widget))
			target = toggle->EventTarget;
		else if (const UISliderComponent* slider = registry.try_get<UISliderComponent>(widget))
			target = slider->EventTarget;
		const Entity targetEntity = m_Scene.GetEntityByUUID(target);
		if (targetEntity && targetEntity.GetHandle() != widget)
			event.Target = targetEntity;
		m_PendingEvents.push_back(event);
	}

	void GameUISystem::DispatchEvents()
	{
		if (m_PendingEvents.empty())
			return;
		// Listeners may add listeners, remove them and change the UI: each event goes to the listeners there were when the
		// dispatch started, unless they were removed meanwhile.
		std::vector<UIEventInfo> events;
		events.swap(m_PendingEvents);
		const std::vector<Ref<EventListener>> listeners = m_Listeners;
		for (const UIEventInfo& event : events)
		{
			for (const Ref<EventListener>& listener : listeners)
			{
				if (!listener->Removed)
					listener->Callback(event);
			}
		}
	}

	UIEventListenerID GameUISystem::AddUIEventListener(UIEventCallback callback)
	{
		if (!callback)
			return 0;
		Ref<EventListener> listener = CreateRef<EventListener>();
		listener->ID = m_NextListenerID++;
		listener->Callback = std::move(callback);
		m_Listeners.push_back(listener);
		return listener->ID;
	}

	bool GameUISystem::RemoveUIEventListener(UIEventListenerID id)
	{
		auto it = std::find_if(m_Listeners.begin(), m_Listeners.end(), [id](const Ref<EventListener>& listener) { return listener->ID == id; });
		if (it == m_Listeners.end())
			return false;
		(*it)->Removed = true;
		m_Listeners.erase(it);
		return true;
	}

}
