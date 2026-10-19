#pragma once

#include <imgui.h>

#include <string>
#include <vector>

namespace nvrhi
{
	class ITexture;
}

namespace Strata
{

	class EditorContext;
	class ViewportRenderer;
	struct EditorFrameStats;
	struct ViewportView;

	// What the Scene and Game views share: a toolbar strip at the top of the panel that holds their chips, so that none of
	// them ever covers the image (the game's HUD, its canvases), and the image's area below it. Main thread, inside the
	// panel's window.
	namespace ViewportToolbar
	{

		struct Layout
		{
			ImVec2 Min;       // The toolbar strip
			ImVec2 Max;
			ImVec2 ChipsMin;  // Where its first chip goes
			ImVec2 ImageMin;  // The area under it (the image, letterboxed or not)
			ImVec2 ImageSize;
		};

		// From the window's cursor and content region (call before drawing anything in the panel).
		Layout Compute();
		// The toolbar's chrome band, its bottom border in the play state's color while the scene runs (changes made now are
		// discarded when it stops).
		void DrawBackground(ImDrawList* drawList, const Layout& layout, const EditorContext& context);
		// The play state's theme color (paused, playing, simulating, editing).
		const ImVec4& GetPlayStateColor(const EditorContext& context);
		// Lines of text over the image, on a dark box so they stay readable on any scene.
		void DrawTextBox(ImDrawList* drawList, const ImVec2& position, const std::vector<std::string>& lines, const ImVec4& color);
		ImTextureID ToTextureID(nvrhi::ITexture* texture);
		// The renderer's statistics over the image: frame time, draws, the render scene's proxies and uploads, asset requests.
		void DrawStatsOverlay(ImDrawList* drawList, const ImVec2& position, const ViewportRenderer& renderer, const ViewportView& view,
			const EditorFrameStats& frame);

	}

}
