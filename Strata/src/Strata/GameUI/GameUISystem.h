#pragma once

#include "Strata/Core/Base.h"
#include "Strata/GameUI/GameUIHitTest.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/Input/UIActions.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSystem.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// The "UI" scene system (Play mode, before the scripts): the game UI's input. Each update it lays the canvases out for
	// the game's input screen (GameUIScene::GetInputScreen), hit tests the pointer, owns the presses that start over the UI
	// (Input's pointer capture hides them from the game until their release; a press elsewhere stays the game's), clicks
	// buttons, flips toggles, drags sliders, moves the focus with the UI actions (keyboard and gamepad) and shows each
	// selectable's state with tints that fade over unscaled time. Widget events go to IUIEvents listeners at the end of its
	// update, before the scripts' OnUpdate of the frame, in the order their inputs happened: pointer, navigation, submit.
	// Toggle and slider values are written with registry.patch (the running scene, without undo). A frame without UI under
	// the pointer and without a focus changes nothing the game sees. Main thread only.
	class GameUISystem final : public SceneSystem, public IUIEvents
	{
	public:
		explicit GameUISystem(Scene& scene);
		~GameUISystem() override;

		GameUISystem(const GameUISystem&) = delete;
		GameUISystem& operator=(const GameUISystem&) = delete;

		void OnRuntimeStop() override;
		void OnUpdate(Timestep timestep) override;
		void OnEntityDestroying(const Entity& entity) override;

		UIEventListenerID AddUIEventListener(UIEventCallback callback) override;
		bool RemoveUIEventListener(UIEventListenerID id) override;

		// The selectable with the focus (keyboard and gamepad navigation, Submit), or an invalid entity.
		Entity GetFocused() const;
		// Gives an interactable, visible selectable the focus (FocusChanged events next update), or clears it with an
		// invalid entity. False (nothing changes) for anything else.
		bool SetFocus(Entity selectable);
		// Whether the pointer is over a raycast target of a canvas that receives input (as of this frame's update).
		bool IsPointerOverUI() const { return m_PointerOverUI; }
		const UIActionState& GetActions() const { return m_Actions; }
		// The selectable the pointer hovers, or an invalid entity.
		Entity GetHovered() const;
	private:
		struct EventListener
		{
			UIEventListenerID ID = 0;
			UIEventCallback Callback;
			bool Removed = false;
		};

		// A selectable that shows a state other than its rest one, or fades: its tint goes from From to To over Duration.
		struct Fade
		{
			UIVisualState State = UIVisualState::Normal;
			glm::vec4 From = glm::vec4(1.0f);
			glm::vec4 To = glm::vec4(1.0f);
			float Elapsed = 0.0f;
			float Duration = 0.0f;
		};

		enum class Gesture : uint8_t
		{
			None = 0,
			UI,  // A press that started over the UI: the UI's until every button is released
			Game // A press that started elsewhere: the game's until every button is released
		};

		void UpdatePointer(GameUIScene& ui, const UIInputScreen& screen, bool layoutChanged);
		void UpdateNavigation(GameUIScene& ui, std::span<const UICanvasView> views);
		void UpdateVisualStates(GameUIScene& ui, float deltaTime);
		void DispatchEvents();
		// Lays the canvases out for the screen and keeps their views for this update's queries.
		void RefreshViews(GameUIScene& ui, const UIScreen& screen);

		// The selectable a hit element belongs to: itself, or its nearest ancestor with a UISelectable.
		entt::entity FindSelectable(entt::entity element) const;
		bool IsInteractable(entt::entity selectable) const;
		// Whether the element is a visible element of a canvas that receives input, and where it is (pixels).
		bool FindOnScreen(GameUIScene& ui, entt::entity entity, UIBox& outPixels) const;
		void Focus(entt::entity selectable);
		void Click(entt::entity selectable);
		void SetToggle(entt::entity toggle, bool on);
		void SetSliderValue(entt::entity slider, float value);
		void DragSlider(GameUIScene& ui, entt::entity slider, const glm::vec2& pixel);
		bool StepSlider(entt::entity slider, UIAction action);
		entt::entity FindStartSelectable(GameUIScene& ui, std::span<const UICanvasView> views) const;
		entt::entity Navigate(GameUIScene& ui, std::span<const UICanvasView> views, entt::entity from, UIAction direction) const;
		void QueueEvent(UIEventType type, entt::entity widget, float value = 0.0f, bool focused = false);
		UIVisualState GetDesiredState(entt::entity selectable) const;
	private:
		Scene& m_Scene;
		GameUIHitTester m_HitTester;
		UIActionState m_Actions;
		std::vector<UICanvasView> m_Views; // This update's (EnsureLayout's results move as layouts change)
		UIScreen m_ViewScreen;             // The screen m_Views are laid out for

		glm::vec2 m_Pointer = glm::vec2(0.0f); // Screen pixels
		bool m_PointerKnown = false;
		entt::entity m_HoveredTarget = entt::null; // The raycast target under the pointer
		entt::entity m_Hovered = entt::null;       // Its selectable
		bool m_PointerOverUI = false;
		Gesture m_Gesture = Gesture::None;
		entt::entity m_Pressed = entt::null;  // The selectable a UI press started on
		entt::entity m_Dragging = entt::null; // A slider following the pointer
		entt::entity m_Focused = entt::null;

		std::unordered_map<entt::entity, Fade> m_Fades;
		std::vector<UIEventInfo> m_PendingEvents;
		std::vector<Ref<EventListener>> m_Listeners;
		UIEventListenerID m_NextListenerID = 1;
	};

}
