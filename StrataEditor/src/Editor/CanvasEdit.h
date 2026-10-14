#pragma once

#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Scene;

	// What a drag in the viewport's 2D canvas mode grabs of the selected element: its body (moves it), an edge or a corner
	// (resizes it from there).
	enum class CanvasHandle : uint8_t
	{
		None = 0,
		Move,
		Left, Right, Top, Bottom,
		TopLeft, TopRight, BottomLeft, BottomRight
	};

	namespace CanvasEdit
	{

		const char* GetHandleName(CanvasHandle handle);
		// The handles of a box (canvas units): the eight on its edges and corners, where they are drawn.
		struct HandlePoint
		{
			CanvasHandle Handle = CanvasHandle::None;
			glm::vec2 Position = glm::vec2(0.0f);
		};
		std::vector<HandlePoint> GetHandles(const UIBox& box);

		// The box a drag of `handle` by `delta` makes of `box` (canvas units): a move shifts it, an edge or corner moves those
		// sides (never past the opposite one). With `snap` the sides that move land on whole canvas units.
		UIBox DragBox(const UIBox& box, CanvasHandle handle, const glm::vec2& delta, bool snap);

		// The rect values that put a rect at `box` in `parent` (canvas units), keeping its anchors and pivot (and its size
		// along axes it fits to its content).
		UIRectComponent PlaceRect(const UIRectComponent& rect, const UIBox& box, const UIBox& parent);

		// The canvas the 2D canvas mode shows: the selection's (its root canvas), else the scene's first root canvas in draw
		// order; invalid when the scene has none.
		Entity FindShownCanvas(EditorContext& context);

		// The aspect ratio (width / height) the canvas mode shows a canvas at: its reference resolution's when it scales with
		// the screen, else `fallback` (the viewport's).
		float GetCanvasAspect(Entity canvas, float fallback);

		// What a click at a pixel of a screen selects in canvas mode: the topmost visible element there (any element but the
		// root canvas, drawn or not), in the canvas `canvas`. Invalid when there is none.
		Entity PickElement(Scene& scene, Entity canvas, const UIScreen& screen, const glm::vec2& pixel);

		// A drag of a handle of an element in canvas mode: every update is an edit of its rect that merges with the drag's
		// earlier ones, so the whole drag is one undo step (End breaks the merge). Main thread only.
		class CanvasRectDrag
		{
		public:
			// Starts a drag at `pointer` (canvas units) at the screen the canvas mode lays the canvas out for. Nullopt (outError
			// says why) for an element that is not laid out, a root canvas, or a scene that cannot be edited.
			static std::optional<CanvasRectDrag> Begin(EditorContext& context, Entity element, CanvasHandle handle, const glm::vec2& pointer,
				const UIScreen& screen, std::string* outError = nullptr);

			// Moves the grabbed handle to `pointer` (canvas units). False (nothing changed) when the element is gone.
			bool Update(EditorContext& context, const glm::vec2& pointer, bool snap, std::string* outError = nullptr);
			void End(EditorContext& context);

			UUID GetElement() const { return m_Element; }
			CanvasHandle GetHandle() const { return m_Handle; }
		private:
			CanvasRectDrag() = default;
		private:
			UUID m_Element = UUID::Null();
			CanvasHandle m_Handle = CanvasHandle::None;
			glm::vec2 m_Start = glm::vec2(0.0f);
			UIBox m_StartBox;
			UIBox m_Parent;
			std::string m_MergeKey;
		};

	}

}
