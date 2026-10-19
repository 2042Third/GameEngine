#include <doctest/doctest.h>

#include "Strata/Events/ApplicationEvent.h"
#include "Strata/ImGui/ImGuiLayer.h"

#include <imgui.h>

#include <limits>

using namespace Strata;

TEST_SUITE("ImGui.Layer")
{
	TEST_CASE("The UI scale is the content scale less what window coordinates in points apply already")
	{
		// Windows and X11 measure windows in pixels: the content scale is the UI scale.
		CHECK(ImGuiLayer::ToUIScale(1.5f, glm::uvec2(1600, 900), glm::uvec2(1600, 900)) == 1.5f);
		CHECK(ImGuiLayer::ToUIScale(2.0f, glm::uvec2(1600, 900), glm::uvec2(1600, 900)) == 2.0f);
		// A Retina window of 1600 x 900 points has 3200 x 1800 pixels: ImGui lays out in points, so the UI is not scaled
		// again; at a content scale above the pixel density (a larger text setting) the rest remains.
		CHECK(ImGuiLayer::ToUIScale(2.0f, glm::uvec2(1600, 900), glm::uvec2(3200, 1800)) == 1.0f);
		CHECK(ImGuiLayer::ToUIScale(3.0f, glm::uvec2(1600, 900), glm::uvec2(3200, 1800)) == 1.5f);
		// Sizes not known yet (a minimized window) leave the content scale.
		CHECK(ImGuiLayer::ToUIScale(2.0f, glm::uvec2(0), glm::uvec2(0)) == 2.0f);
		CHECK(ImGuiLayer::ToUIScale(2.0f, glm::uvec2(1600, 900), glm::uvec2(0)) == 2.0f);
	}

	TEST_CASE("The ImGuiLayer styles the UI for the window's content scale")
	{
		// The layer styles the current context; it need not be attached (no window or renderer).
		ImGuiContext* context = ImGui::CreateContext();
		{
			ImGuiLayer layer;
			CHECK(layer.GetUIScale() == 1.0f);

			// Without a style callback: ImGui's dark style with its sizes scaled.
			layer.SetContentScale(2.0f);
			ImGuiStyle expected;
			ImGui::StyleColorsDark(&expected);
			expected.ScaleAllSizes(2.0f);
			CHECK(ImGui::GetStyle().FramePadding.x == expected.FramePadding.x);
			CHECK(ImGui::GetStyle().ItemSpacing.y == expected.ItemSpacing.y);
			CHECK(ImGui::GetStyle().FontScaleDpi == 2.0f);

			// The callback styles the unscaled defaults for the scale, so restyling never compounds.
			int calls = 0;
			float received = 0.0f;
			layer.SetStyleCallback([&](ImGuiStyle& style, float scale)
			{
				calls++;
				received = scale;
				style.FramePadding = ImVec2(10.0f * scale, 5.0f * scale);
			});
			CHECK(calls == 1);
			CHECK(received == 2.0f);
			CHECK(ImGui::GetStyle().FramePadding.x == 20.0f);
			layer.SetContentScale(1.5f);
			CHECK(ImGui::GetStyle().FramePadding.x == 15.0f);
			CHECK(ImGui::GetStyle().FontScaleDpi == 1.5f);

			// The window reports a new scale: restyled, and the event stays available to other layers.
			WindowContentScaleEvent event(3.0f);
			layer.OnEvent(event);
			CHECK_FALSE(event.Handled);
			CHECK(layer.GetUIScale() == 3.0f);
			CHECK(ImGui::GetStyle().FramePadding.x == 30.0f);
			CHECK(ImGui::GetStyle().FontScaleDpi == 3.0f);

			// Meaningless scales are ignored, extreme ones clamped.
			for (const float invalid : { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
			{
				layer.SetContentScale(invalid);
				CHECK(layer.GetUIScale() == 3.0f);
			}
			layer.SetContentScale(100.0f);
			CHECK(layer.GetUIScale() == ImGuiLayer::c_MaxScale);
			layer.SetContentScale(0.01f);
			CHECK(layer.GetUIScale() == ImGuiLayer::c_MinScale);

			// An override (--ui-scale) wins over the window's scale until it is cleared.
			layer.SetContentScale(2.0f);
			layer.SetContentScaleOverride(1.0f);
			CHECK(layer.GetUIScale() == 1.0f);
			CHECK(ImGui::GetStyle().FontScaleDpi == 1.0f);
			WindowContentScaleEvent moved(1.5f);
			layer.OnEvent(moved);
			CHECK(layer.GetUIScale() == 1.0f);
			CHECK(ImGui::GetStyle().FramePadding.x == 10.0f);
			layer.SetContentScaleOverride(-2.0f);
			CHECK(layer.GetUIScale() == 1.0f);
			layer.SetContentScaleOverride(0.0f);
			CHECK(layer.GetUIScale() == 1.5f);
			CHECK(ImGui::GetStyle().FontScaleDpi == 1.5f);
			CHECK(calls > 1);
		}
		ImGui::DestroyContext(context);
	}

	TEST_CASE("WindowContentScaleEvent")
	{
		WindowContentScaleEvent event(1.5f);
		CHECK(event.GetScale() == 1.5f);
		CHECK(event.GetEventType() == EventType::WindowContentScale);
		CHECK(event.IsInCategory(EventCategoryApplication));
		CHECK(event.ToString() == "WindowContentScaleEvent: 1.5");
	}
}
