#pragma once

#include "Strata/Core/Base.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/GameUI/GameUIHitTest.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/Input/UIActions.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSystem.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// The "UI" scene system (Play mode, before the scripts): the game UI's input. Each update it lays the canvases out for
	// the game's input screen (GameUIScene::GetInputScreen), hit tests the pointer, owns the presses that start over the UI
	// (Input's pointer capture hides them from the game until their release; a press elsewhere stays the game's), clicks
	// buttons, flips toggles, drags sliders, moves the focus with the UI actions (keyboard and gamepad) and shows each
	// selectable's state with tints that fade over unscaled time, and edits the focused input field (typed text, the editing
	// keys with repeat, the caret's blink; the keyboard is captured meanwhile). Widget events go to IUIEvents listeners at the end of its
	// update, before the scripts' OnUpdate of the frame, in the order their inputs happened: pointer, navigation, submit.
	// Toggle and slider values are written with registry.patch (the running scene, without undo). A frame without UI under
	// the pointer and without a focus changes nothing the game sees. It also offers IUIControl, the UI's elements for the
	// script host. Main thread only.
	class GameUISystem final : public SceneSystem, public IUIEvents, public IUIControl
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

		bool GetText(Entity element, std::string& outText, std::string* outError = nullptr) override;
		bool SetText(Entity element, std::string_view text, std::string* outError = nullptr) override;
		bool GetImage(Entity element, AssetHandle& outTexture, std::string* outError = nullptr) override;
		bool SetImage(Entity element, AssetHandle texture, std::string* outError = nullptr) override;
		bool GetColor(Entity element, glm::vec4& outColor, std::string* outError = nullptr) override;
		bool SetColor(Entity element, const glm::vec4& color, std::string* outError = nullptr) override;
		bool IsInteractable(Entity element, bool& outInteractable, std::string* outError = nullptr) override;
		bool SetInteractable(Entity element, bool interactable, std::string* outError = nullptr) override;
		bool GetValue(Entity element, float& outValue, std::string* outError = nullptr) override;
		bool SetValue(Entity element, float value, std::string* outError = nullptr) override;
		Entity GetFocus() override { return GetFocused(); }
		// Gives an interactable, visible selectable the focus (FocusChanged events at the end of the next update, or of this
		// one when called during it), or clears it with an invalid entity. False (nothing changes) for anything else.
		bool SetFocus(Entity selectable, std::string* outError = nullptr) override;
		bool IsPointerOverUI() override { return m_PointerOverUI; }
		bool IsActionPressed(uint32_t action) override;
		bool IsActionDown(uint32_t action) override;

		// The selectable with the focus (keyboard and gamepad navigation, Submit), or an invalid entity.
		Entity GetFocused() const;
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

		// The keys that edit a field's text, each repeating while held (c_EditKeys).
		static constexpr uint32_t c_EditKeyCount = 6;
		// The caret shows and hides for this long each (unscaled seconds).
		static constexpr float c_CaretBlinkInterval = 0.53f;

		struct EditKeyRepeat
		{
			float HeldTime = 0.0f;
			float NextRepeat = 0.0f;
		};

		void UpdatePointer(GameUIScene& ui, const UIInputScreen& screen, bool layoutChanged);
		// Typed text and the editing keys of the input field being edited.
		void UpdateEditing(float deltaTime);
		bool IsEditKeyRepeated(uint32_t index, float deltaTime);
		// Starts editing a focused input field (the caret at its end, the keyboard captured), or stops (EndEditing).
		void BeginEditing(entt::entity field);
		void EndEditing();
		// Moves the caret of the field being edited one character (or to an end) and shows it; with `select` the selection's
		// other end stays.
		void MoveCaret(const std::string& text, int direction, bool toEnd, bool select);
		void ShowFieldState();
		void UpdateNavigation(GameUIScene& ui, std::span<const UICanvasView> views);
		void UpdateVisualStates(GameUIScene& ui, float deltaTime);
		void DispatchEvents();
		// Lays the canvases out for the screen and keeps their views for this update's queries.
		void RefreshViews(GameUIScene& ui, const UIScreen& screen);

		// The entity an element function of IUIControl acts on, with the component it needs (null, with the reason, when
		// there is none).
		template<typename Component>
		Component* FindElementComponent(Entity element, const char* componentName, std::string* outError);
		// Switches the other toggles of an on toggle's group off (without events).
		void EnforceGroup(entt::entity toggle);

		// The selectable a hit element belongs to: itself, or its nearest ancestor with a UISelectable.
		entt::entity FindSelectable(entt::entity element) const;
		bool IsInteractable(entt::entity selectable) const;
		// Whether the element is a visible element of a canvas that receives input, and where it is (canvas units) on which
		// canvas view.
		bool FindOnScreen(GameUIScene& ui, entt::entity entity, UIBox& outBox, UICanvasView* outView = nullptr) const;
		// The point of a canvas view the pointer is at (canvas units): on the screen its pixel over the scale, in the scene
		// where the pointer's ray crosses the canvas (nullopt when it does not).
		std::optional<glm::vec2> GetPointerOn(GameUIScene& ui, const UICanvasView& view) const;
		void Focus(entt::entity selectable);
		void Click(entt::entity selectable);
		void SetToggle(entt::entity toggle, bool on);
		void SetSliderValue(entt::entity slider, float value);
		void DragSlider(GameUIScene& ui, entt::entity slider);
		bool StepSlider(entt::entity slider, UIAction action);
		entt::entity FindStartSelectable(GameUIScene& ui, std::span<const UICanvasView> views) const;
		entt::entity Navigate(GameUIScene& ui, std::span<const UICanvasView> views, entt::entity from, UIAction direction) const;
		void QueueEvent(UIEventType type, entt::entity widget, float value = 0.0f, bool focused = false);
		UIVisualState GetDesiredState(entt::entity selectable) const;
	private:
		Scene& m_Scene;
		// The scene's canvases, looked up once: a scene without UI costs a size check per frame.
		const entt::storage_for_t<UICanvasComponent>* m_CanvasStorage = nullptr;
		GameUIHitTester m_HitTester;
		UIActionState m_Actions;
		std::vector<UICanvasView> m_Views; // This update's (EnsureLayout's results move as layouts change)
		UIScreen m_ViewScreen;             // The screen m_Views are laid out for

		glm::vec2 m_Pointer = glm::vec2(0.0f); // Screen pixels
		bool m_PointerKnown = false;
		// The pointer's ray from the game camera, while the scene has world-space canvases and a camera.
		std::optional<UIPointerRay> m_PointerRay;
		entt::entity m_HoveredTarget = entt::null; // The raycast target under the pointer
		entt::entity m_Hovered = entt::null;       // Its selectable
		bool m_PointerOverUI = false;
		Gesture m_Gesture = Gesture::None;
		entt::entity m_Pressed = entt::null;  // The selectable a UI press started on
		entt::entity m_Dragging = entt::null; // A slider following the pointer
		entt::entity m_Focused = entt::null;
		entt::entity m_Editing = entt::null; // The input field being edited: the focused one, while it is interactable
		UIInputFieldState m_FieldState;
		float m_CaretBlink = 0.0f;
		std::array<EditKeyRepeat, c_EditKeyCount> m_EditKeys = {};
		// Escape ended editing: the keyboard stays captured until the next update, so the game never sees that press.
		bool m_KeepCaptureThisFrame = false;

		std::unordered_map<entt::entity, Fade> m_Fades;
		std::vector<UIEventInfo> m_PendingEvents;
		std::vector<Ref<EventListener>> m_Listeners;
		UIEventListenerID m_NextListenerID = 1;
	};

}
