#pragma once

#include "StrataScript/Entity.h"
#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Strata
{

	// The game UI's actions: what menus react to, the input actions of the project's map UI ("UI/Submit", "UI/Cancel",
	// "UI/Up"...). By default Submit is Enter, keypad Enter, Space or gamepad A, Cancel Escape or gamepad B, and the
	// directions the arrow keys, the d-pad and the left stick of every gamepad; the project and the players may rebind them.
	enum class UIAction : uint32_t
	{
		Submit = StrataScriptUIAction_Submit,
		Cancel = StrataScriptUIAction_Cancel,
		Up = StrataScriptUIAction_Up,
		Down = StrataScriptUIAction_Down,
		Left = StrataScriptUIAction_Left,
		Right = StrataScriptUIAction_Right
	};

	// An element of the game UI while the scene plays: an entity with UI components (a UIText, UIImage, UISelectable,
	// UISlider or UIToggle). Functions fail (false, empty, 0) on entities without the component they need, and on engines
	// without the game UI; the engine logs why. Values set here raise no OnUIValueChanged: the callbacks report the player's
	// input. Scripts receive their widgets' clicks, value changes and focus changes through Script::OnUIClick,
	// OnUIValueChanged and OnUIFocusChanged.
	class UIElement
	{
	public:
		explicit UIElement(Entity entity)
			: m_Entity(entity)
		{
		}

		Entity GetEntity() const { return m_Entity; }

		// The text of its UIText.
		std::string GetText() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIGetText);
			if (!host || m_Entity.GetID() == 0)
				return {};
			const uint64_t entity = m_Entity.GetID();
			return Detail::ReadHostString([&](char* buffer, uint64_t capacity) { return host->UIGetText(Detail::GetContext(), entity, buffer, capacity); });
		}

		// Sets the text of its UIText (setting the same text again costs nothing, so a HUD may set its texts every frame).
		bool SetText(std::string_view text)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UISetText);
			return host && m_Entity.GetID() != 0 && host->UISetText(Detail::GetContext(), m_Entity.GetID(), Detail::ToABIString(text));
		}

		// The texture of its UIImage (the null asset: the image's color alone).
		AssetHandle GetImage() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIGetImage);
			return host && m_Entity.GetID() != 0 ? AssetHandle(host->UIGetImage(Detail::GetContext(), m_Entity.GetID())) : AssetHandle();
		}

		// Shows a texture in its UIImage (the null asset: the color alone); other assets than textures are refused.
		bool SetImage(AssetHandle texture)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UISetImage);
			return host && m_Entity.GetID() != 0 && host->UISetImage(Detail::GetContext(), m_Entity.GetID(), texture.ID);
		}

		// The color of its UIImage (its tint), else of its UIText: display (sRGB) RGBA from 0 to 1.
		glm::vec4 GetColor() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIGetColor);
			float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			if (!host || m_Entity.GetID() == 0 || !host->UIGetColor(Detail::GetContext(), m_Entity.GetID(), color))
				return glm::vec4(0.0f);
			return glm::vec4(color[0], color[1], color[2], color[3]);
		}

		// Sets the color of its UIImage, else of its UIText (clamped to 0 to 1; non-finite colors are refused).
		bool SetColor(const glm::vec4& color)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UISetColor);
			const float values[4] = { color.r, color.g, color.b, color.a };
			return host && m_Entity.GetID() != 0 && host->UISetColor(Detail::GetContext(), m_Entity.GetID(), values);
		}

		// Whether its UISelectable takes input (a disabled one shows its disabled state and never gets the focus).
		bool IsInteractable() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIIsInteractable);
			return host && m_Entity.GetID() != 0 && host->UIIsInteractable(Detail::GetContext(), m_Entity.GetID());
		}

		bool SetInteractable(bool interactable)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UISetInteractable);
			return host && m_Entity.GetID() != 0 && host->UISetInteractable(Detail::GetContext(), m_Entity.GetID(), interactable);
		}

		// The value of its UISlider, or 1 for a UIToggle that is on and 0 for one that is off.
		float GetValue() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIGetValue);
			return host && m_Entity.GetID() != 0 ? host->UIGetValue(Detail::GetContext(), m_Entity.GetID()) : 0.0f;
		}

		// Sets the value of its UISlider (clamped to its range, rounded with WholeNumbers), or switches its UIToggle on
		// (any value but 0; the others of its group switch off) or off.
		bool SetValue(float value)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UISetValue);
			return host && m_Entity.GetID() != 0 && host->UISetValue(Detail::GetContext(), m_Entity.GetID(), value);
		}

		// Gives its UISelectable the keyboard and gamepad focus (it must be interactable and shown on a canvas that takes
		// input).
		bool Focus()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UISetFocus);
			return host && m_Entity.GetID() != 0 && host->UISetFocus(Detail::GetContext(), m_Entity.GetID());
		}

		bool IsFocused() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIGetFocus);
			return host && m_Entity.GetID() != 0 && host->UIGetFocus(Detail::GetContext()) == m_Entity.GetID();
		}
	private:
		Entity m_Entity;
	};

	// The game UI as a whole while the scene plays: the focus, the pointer and the actions, as of the UI's update of the
	// frame (it runs before the scripts' OnUpdate).
	class UI
	{
	public:
		// The selectable with the keyboard and gamepad focus (an invalid entity without one).
		static Entity GetFocused()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIGetFocus);
			return Entity(host ? host->UIGetFocus(Detail::GetContext()) : 0);
		}

		static bool ClearFocus()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UISetFocus);
			return host && host->UISetFocus(Detail::GetContext(), 0);
		}

		// Whether the pointer is over the UI: gameplay that aims or shoots with the mouse ignores it then (the UI already
		// hides its clicks from Input).
		static bool IsPointerOverUI()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIIsPointerOverUI);
			return host && host->UIIsPointerOverUI(Detail::GetContext());
		}

		// Whether an action was pressed this frame (Cancel closes a menu: the UI raises no event for it).
		static bool IsActionPressed(UIAction action)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIIsActionPressed);
			return host && host->UIIsActionPressed(Detail::GetContext(), static_cast<uint32_t>(action));
		}

		static bool IsActionDown(UIAction action)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(UIIsActionDown);
			return host && host->UIIsActionDown(Detail::GetContext(), static_cast<uint32_t>(action));
		}
	};

	inline UIElement Entity::GetUI() const
	{
		return UIElement(*this);
	}

}
