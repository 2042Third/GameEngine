#include "stpch.h"
#include "Strata/GameUI/GameUISystem.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Log.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/KeyCodes.h"
#include "Strata/Input/MouseCodes.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneHierarchy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

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

		// The keys that edit an input field's text (GameUISystem::c_EditKeyCount), by index.
		constexpr std::array<KeyCode, 6> c_EditKeys = { Key::Backspace, Key::Delete, Key::Left, Key::Right, Key::Home, Key::End };
		constexpr uint32_t c_EditBackspace = 0;
		constexpr uint32_t c_EditDelete = 1;
		constexpr uint32_t c_EditLeft = 2;
		constexpr uint32_t c_EditRight = 3;
		constexpr uint32_t c_EditHome = 4;
		constexpr uint32_t c_EditEnd = 5;

		bool IsContinuationByte(char byte)
		{
			return (static_cast<uint8_t>(byte) & 0xC0) == 0x80;
		}

		// Character boundaries of UTF-8 text around a byte.
		uint32_t PreviousBoundary(const std::string& text, uint32_t position)
		{
			uint32_t index = std::min(position, static_cast<uint32_t>(text.size()));
			if (index == 0)
				return 0;
			index--;
			while (index > 0 && IsContinuationByte(text[index]))
				index--;
			return index;
		}

		uint32_t NextBoundary(const std::string& text, uint32_t position)
		{
			const uint32_t size = static_cast<uint32_t>(text.size());
			if (position >= size)
				return size;
			uint32_t index = position + 1;
			while (index < size && IsContinuationByte(text[index]))
				index++;
			return index;
		}

		// A byte made a character boundary (moved back to the start of its character, and into the text).
		uint32_t ToBoundary(const std::string& text, uint32_t position)
		{
			uint32_t index = std::min(position, static_cast<uint32_t>(text.size()));
			while (index > 0 && index < text.size() && IsContinuationByte(text[index]))
				index--;
			return index;
		}

		size_t CountCharacters(std::string_view text)
		{
			return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](char byte) { return !IsContinuationByte(byte); }));
		}

		// Typed text a line holds: control characters (line breaks, tabs) dropped.
		std::string FilterTypedText(std::string_view text)
		{
			std::string kept;
			kept.reserve(text.size());
			for (const char byte : text)
			{
				const uint8_t value = static_cast<uint8_t>(byte);
				if (value >= 0x20 && value != 0x7F)
					kept.push_back(byte);
			}
			return kept;
		}

		// The longest start of `text` with at most `count` whole characters.
		std::string_view TakeCharacters(std::string_view text, size_t count)
		{
			size_t characters = 0;
			for (size_t index = 0; index < text.size(); index++)
			{
				if (IsContinuationByte(text[index]))
					continue;
				if (characters == count)
					return text.substr(0, index);
				characters++;
			}
			return text;
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
		// Clicks and keys belong to the game again, and selectables show their rest states.
		m_KeepCaptureThisFrame = false;
		EndEditing();
		Input::SetKeyboardCapture(false);
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
			if (m_Gesture != Gesture::None || m_PointerOverUI || m_Focused != entt::null || Input::IsPointerCaptured() || m_KeepCaptureThisFrame)
			{
				m_KeepCaptureThisFrame = false;
				EndEditing();
				Input::SetKeyboardCapture(false);
				Input::SetPointerCapture(false);
				m_Gesture = Gesture::None;
				m_PointerOverUI = false;
				m_Pressed = m_Dragging = m_Hovered = m_HoveredTarget = m_Focused = entt::null;
			}
			return;
		}

		// The key that ended editing last update was the UI's: the keyboard is the game's again from this update on.
		if (m_KeepCaptureThisFrame)
		{
			m_KeepCaptureThisFrame = false;
			if (m_Editing == entt::null)
				Input::SetKeyboardCapture(false);
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
		UpdateEditing(deltaTime);
		UpdateNavigation(ui, m_Views);
		UpdateVisualStates(ui, deltaTime);
		DispatchEvents();
	}

	void GameUISystem::OnEntityDestroying(const Entity& entity)
	{
		const entt::entity handle = entity.GetHandle();
		if (handle == m_Editing)
			EndEditing();
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
		// Canvases in the scene meet the pointer's ray from the game camera, which may move every frame.
		m_PointerRay.reset();
		const bool worldCanvases = ui.GetWorldCanvasCount() > 0;
		if (worldCanvases)
		{
			if (const std::optional<glm::mat4> viewProjection = GameUIPointer::GetGameViewProjection(m_Scene, screen.Screen))
				m_PointerRay = GameUIPointer::MakeRay(*viewProjection, screen.Screen.Size, pixel);
		}

		// Hover follows only what can change it: the pointer, the layout (which destroyed elements change), presses and
		// releases, and with canvases in the scene the camera.
		if (moved || layoutChanged || pointer.Pressed != 0 || pointer.Released != 0 || worldCanvases)
		{
			const std::optional<UIHitResult> hit = m_HitTester.HitTest(pixel, false, m_PointerRay ? &*m_PointerRay : nullptr);
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
				// A click on an input field puts its caret at the character nearest the pointer.
				if (m_Editing != entt::null && m_Editing == m_Pressed)
				{
					UIBox fieldBox;
					UICanvasView fieldView;
					const std::optional<glm::vec2> point = FindOnScreen(ui, m_Editing, fieldBox, &fieldView) ? GetPointerOn(ui, fieldView) : std::nullopt;
					if (const std::optional<uint32_t> caret = point ? ui.FindInputFieldCaret(m_Editing, screen.Screen, *point) : std::nullopt)
					{
						m_FieldState.Caret = *caret;
						m_FieldState.Anchor = *caret;
						m_CaretBlink = 0.0f;
						ShowFieldState();
					}
				}
			}
		}
		// A slider follows the pointer while it is dragged, also outside its rect.
		if (m_Dragging != entt::null && (pointer.Down & c_Left) != 0)
			DragSlider(ui, m_Dragging);
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

	bool GameUISystem::FindOnScreen(GameUIScene& ui, entt::entity entity, UIBox& outBox, UICanvasView* outView) const
	{
		UICanvasView view;
		uint32_t element = 0;
		if (!ui.FindElement(entity, m_ViewScreen, view, element) || !view.ReceivesInput || !ui.IsElementVisible(view, element))
			return false;
		outBox = ui.GetElementBox(view, element);
		if (outView)
			*outView = view;
		return true;
	}

	std::optional<glm::vec2> GameUISystem::GetPointerOn(GameUIScene& ui, const UICanvasView& view) const
	{
		if (!view.WorldSpace)
			return m_Pointer / view.Scale;
		if (!m_PointerRay)
			return std::nullopt;
		const std::optional<std::pair<glm::vec2, float>> crossing = GameUIPointer::IntersectCanvas(ui.GetCanvasWorldMatrix(view), *m_PointerRay);
		return crossing ? std::optional<glm::vec2>(crossing->first) : std::nullopt;
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

	void GameUISystem::DragSlider(GameUIScene& ui, entt::entity slider)
	{
		const UISliderComponent* component = m_Scene.GetRegistry().try_get<UISliderComponent>(slider);
		UIBox box;
		UICanvasView view;
		if (!component || !FindOnScreen(ui, slider, box, &view))
			return;
		const std::optional<glm::vec2> point = GetPointerOn(ui, view);
		if (!point)
			return;
		const bool horizontal = component->Direction == UIFillDirection::LeftToRight || component->Direction == UIFillDirection::RightToLeft;
		const int axis = horizontal ? 0 : 1;
		if (!(box.Size[axis] > 0.0f))
			return;
		float fraction = std::clamp(((*point)[axis] - box.Min[axis]) / box.Size[axis], 0.0f, 1.0f);
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
		// The focused input field is the one edited.
		EndEditing();
		if (selectable != entt::null && m_Scene.GetRegistry().all_of<UIInputFieldComponent>(selectable))
			BeginEditing(selectable);
		if (previous != entt::null && m_Scene.GetRegistry().valid(previous))
			QueueEvent(UIEventType::FocusChanged, previous, 0.0f, false);
		if (selectable != entt::null)
			QueueEvent(UIEventType::FocusChanged, selectable, 0.0f, true);
	}

	void GameUISystem::UpdateNavigation(GameUIScene& ui, std::span<const UICanvasView> views)
	{
		const bool editing = m_Editing != entt::null && m_Editing == m_Focused;
		for (const UIAction direction : c_Directions)
		{
			if (!m_Actions.IsRepeated(direction))
				continue;
			// An edited field's caret moves left and right (a gamepad's: the keyboard is the field's meanwhile).
			if (editing && (direction == UIAction::Left || direction == UIAction::Right))
			{
				if (const UIInputFieldComponent* field = m_Scene.GetRegistry().try_get<UIInputFieldComponent>(m_Editing))
				{
					MoveCaret(field->Text, direction == UIAction::Left ? -1 : 1, false, false);
					ShowFieldState();
				}
				continue;
			}
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
			else if (m_Editing != entt::null && m_Focused == m_Editing)
				QueueEvent(UIEventType::Submit, m_Editing);
			else
				Click(m_Focused);
		}
		// Cancel stops editing a field (a gamepad's: Escape is the field's while it is edited).
		if (m_Editing != entt::null && m_Actions.IsPressed(UIAction::Cancel))
			Focus(entt::null);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Input fields
	////////////////////////////////////////////////////////////////////////////////

	void GameUISystem::BeginEditing(entt::entity field)
	{
		const UIInputFieldComponent& component = m_Scene.GetRegistry().get<UIInputFieldComponent>(field);
		m_Editing = field;
		const uint32_t end = static_cast<uint32_t>(component.Text.size());
		m_FieldState = UIInputFieldState { end, end, true };
		m_CaretBlink = 0.0f;
		m_EditKeys = {};
		// The keyboard is the field's: typing a name never moves the player.
		Input::SetKeyboardCapture(true);
		ShowFieldState();
	}

	void GameUISystem::EndEditing()
	{
		if (m_Editing == entt::null)
			return;
		if (GameUIScene* ui = GameUIScene::Find(m_Scene))
			ui->ClearInputFieldState(m_Editing);
		m_Editing = entt::null;
		if (!m_KeepCaptureThisFrame)
			Input::SetKeyboardCapture(false);
	}

	void GameUISystem::ShowFieldState()
	{
		if (m_Editing == entt::null)
			return;
		if (GameUIScene* ui = GameUIScene::Find(m_Scene))
			ui->SetInputFieldState(m_Editing, m_FieldState);
	}

	bool GameUISystem::IsEditKeyRepeated(uint32_t index, float deltaTime)
	{
		// Like the UI actions: on the press, then after c_RepeatDelay held and every c_RepeatInterval after that.
		const KeyState key = Input::GetKeyState(c_EditKeys[index]);
		EditKeyRepeat& repeat = m_EditKeys[index];
		if (key.Pressed)
		{
			repeat.HeldTime = 0.0f;
			repeat.NextRepeat = UIActionState::c_RepeatDelay;
			return true;
		}
		if (!key.Down)
		{
			repeat = {};
			return false;
		}
		repeat.HeldTime += deltaTime;
		if (repeat.HeldTime < repeat.NextRepeat)
			return false;
		while (repeat.NextRepeat <= repeat.HeldTime)
			repeat.NextRepeat += UIActionState::c_RepeatInterval;
		return true;
	}

	void GameUISystem::MoveCaret(const std::string& text, int direction, bool toEnd, bool select)
	{
		UIInputFieldState& state = m_FieldState;
		const uint32_t selectionStart = std::min(state.Caret, state.Anchor);
		const uint32_t selectionEnd = std::max(state.Caret, state.Anchor);
		if (toEnd)
			state.Caret = direction < 0 ? 0 : static_cast<uint32_t>(text.size());
		else if (!select && selectionStart != selectionEnd)
			state.Caret = direction < 0 ? selectionStart : selectionEnd; // A selection collapses to the side moved to
		else
			state.Caret = direction < 0 ? PreviousBoundary(text, state.Caret) : NextBoundary(text, state.Caret);
		if (!select)
			state.Anchor = state.Caret;
		m_CaretBlink = 0.0f;
		state.CaretVisible = true;
	}

	void GameUISystem::UpdateEditing(float deltaTime)
	{
		if (m_Editing == entt::null)
			return;
		entt::registry& registry = m_Scene.GetRegistry();
		const UIInputFieldComponent* field = registry.try_get<UIInputFieldComponent>(m_Editing);
		if (!field)
		{
			EndEditing();
			return;
		}
		// Every key's repeat follows the frame, used or not.
		std::array<bool, c_EditKeyCount> repeated = {};
		for (uint32_t index = 0; index < c_EditKeyCount; index++)
			repeated[index] = IsEditKeyRepeated(index, deltaTime);
		const bool shift = Input::GetKeyState(Key::LeftShift).Down || Input::GetKeyState(Key::RightShift).Down;
		const bool command = Input::GetKeyState(Key::LeftControl).Down || Input::GetKeyState(Key::RightControl).Down
			|| Input::GetKeyState(Key::LeftSuper).Down || Input::GetKeyState(Key::RightSuper).Down;

		// Code may have changed the text since: the caret and the selection stay on its characters.
		std::string text = field->Text;
		UIInputFieldState& state = m_FieldState;
		state.Caret = ToBoundary(text, state.Caret);
		state.Anchor = ToBoundary(text, state.Anchor);
		const UIInputFieldState before = state;
		bool edited = false;
		auto eraseSelection = [&]()
		{
			const uint32_t start = std::min(state.Caret, state.Anchor);
			const uint32_t end = std::max(state.Caret, state.Anchor);
			if (start == end)
				return false;
			text.erase(start, end - start);
			state.Caret = state.Anchor = start;
			return true;
		};

		if (command && Input::GetKeyState(Key::A).Pressed)
		{
			state.Anchor = 0;
			state.Caret = static_cast<uint32_t>(text.size());
		}
		// Backspace and Delete remove the selection, else the character before or after the caret.
		if (repeated[c_EditBackspace])
		{
			if (eraseSelection())
			{
				edited = true;
			}
			else if (state.Caret > 0)
			{
				const uint32_t start = PreviousBoundary(text, state.Caret);
				text.erase(start, state.Caret - start);
				state.Caret = state.Anchor = start;
				edited = true;
			}
		}
		if (repeated[c_EditDelete])
		{
			if (eraseSelection())
			{
				edited = true;
			}
			else if (state.Caret < text.size())
			{
				text.erase(state.Caret, NextBoundary(text, state.Caret) - state.Caret);
				edited = true;
			}
		}
		if (repeated[c_EditLeft])
			MoveCaret(text, -1, false, shift);
		if (repeated[c_EditRight])
			MoveCaret(text, 1, false, shift);
		if (repeated[c_EditHome])
			MoveCaret(text, -1, true, shift);
		if (repeated[c_EditEnd])
			MoveCaret(text, 1, true, shift);

		// Typed characters replace the selection, as many as the character limit leaves room for.
		const std::string typed = FilterTypedText(Input::GetUncapturedTypedText());
		if (!typed.empty())
		{
			edited = eraseSelection() || edited;
			std::string_view inserted = typed;
			if (field->CharacterLimit > 0)
			{
				const size_t count = CountCharacters(text);
				inserted = TakeCharacters(inserted, count < field->CharacterLimit ? field->CharacterLimit - count : 0);
			}
			if (!inserted.empty())
			{
				text.insert(state.Caret, inserted);
				state.Caret += static_cast<uint32_t>(inserted.size());
				state.Anchor = state.Caret;
				edited = true;
			}
		}

		const entt::entity editedField = m_Editing;
		if (edited && text != field->Text)
			registry.patch<UIInputFieldComponent>(editedField, [&text](UIInputFieldComponent& value) { value.Text = text; });
		// The caret blinks, and shows at once where it moved to.
		if (!(state == before) || edited)
			m_CaretBlink = 0.0f;
		else
			m_CaretBlink += deltaTime;
		state.CaretVisible = std::fmod(m_CaretBlink, 2.0f * c_CaretBlinkInterval) < c_CaretBlinkInterval;
		ShowFieldState();

		// Enter submits, Escape stops editing.
		if (Input::GetKeyState(Key::Enter).Pressed || Input::GetKeyState(Key::KPEnter).Pressed)
			QueueEvent(UIEventType::Submit, editedField);
		if (Input::GetKeyState(Key::Escape).Pressed)
		{
			m_KeepCaptureThisFrame = true;
			Focus(entt::null);
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
		// The topmost canvas that receives input and has a selectable: its FirstSelected, else its top-left selectable. The
		// screen's canvases come first, then those in the scene.
		for (size_t step = 0; step < views.size() * 2; step++)
		{
			const size_t index = views.size() - 1 - step % views.size();
			const UICanvasView& view = views[index];
			if (!view.ReceivesInput || view.WorldSpace != (step >= views.size()))
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
		// so canvases of different scales compare); ties keep draw order. A canvas in the scene navigates within itself.
		UIBox fromBox;
		UICanvasView fromView;
		if (!FindOnScreen(ui, from, fromBox, &fromView))
			return entt::null;
		const glm::vec2 origin = fromBox.GetCenter() * fromView.Scale;
		const glm::vec2 axis = GetDirectionVector(direction);
		entt::entity best = entt::null;
		float bestScore = std::numeric_limits<float>::max();
		for (const UICanvasView& view : views)
		{
			if (!view.ReceivesInput || view.WorldSpace != fromView.WorldSpace || (view.WorldSpace && view.CanvasIndex != fromView.CanvasIndex))
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
		// An input field's text first.
		if (element && element.GetScene() == &m_Scene && m_Scene.GetRegistry().valid(element.GetHandle()))
		{
			if (const UIInputFieldComponent* field = m_Scene.GetRegistry().try_get<UIInputFieldComponent>(element.GetHandle()))
			{
				outText = field->Text;
				return true;
			}
		}
		const UITextComponent* text = FindElementComponent<UITextComponent>(element, "UIText", outError);
		if (!text)
			return false;
		outText = text->Text;
		return true;
	}

	bool GameUISystem::SetText(Entity element, std::string_view text, std::string* outError)
	{
		if (element && element.GetScene() == &m_Scene && m_Scene.GetRegistry().valid(element.GetHandle()))
		{
			if (const UIInputFieldComponent* field = m_Scene.GetRegistry().try_get<UIInputFieldComponent>(element.GetHandle()))
			{
				// A field holds one line within its character limit.
				std::string line = FilterTypedText(text);
				if (field->CharacterLimit > 0)
					line = std::string(TakeCharacters(line, field->CharacterLimit));
				if (field->Text != line)
					m_Scene.GetRegistry().patch<UIInputFieldComponent>(element.GetHandle(), [&line](UIInputFieldComponent& value) { value.Text = line; });
				return true;
			}
		}
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
		else if (const UIInputFieldComponent* field = registry.try_get<UIInputFieldComponent>(widget))
			target = field->EventTarget;
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
