#pragma once

#include <Strata/Core/UUID.h>
#include <Strata/Scene/Components.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Scene;

	// What the viewport's transform gizmo does.
	enum class GizmoOperation : uint8_t
	{
		None = 0,
		Translate,
		Rotate,
		Scale
	};

	// The axes the gizmo shows: the entity's own (local) or the world's. Scaling always uses the entity's axes.
	enum class GizmoSpace : uint8_t
	{
		Local = 0,
		World
	};

	const char* GizmoOperationToString(GizmoOperation operation);
	std::optional<GizmoOperation> GizmoOperationFromString(std::string_view name);
	const char* GizmoSpaceToString(GizmoSpace space);
	std::optional<GizmoSpace> GizmoSpaceFromString(std::string_view name);

	namespace TransformEdit
	{

		// The local transform that places an entity at `world` under a parent whose world transform is `parentWorld`
		// (identity for top-level entities). Exact when the combination has no shear (parents with uniform scale, or
		// scale along the child's axes); otherwise the nearest translation, rotation and scale. Nullopt when the parent
		// cannot be inverted (zero scale) or the result is degenerate or not finite.
		std::optional<TransformComponent> WorldToLocal(const glm::mat4& parentWorld, const glm::mat4& world);

	}

	// One drag of the viewport's transform gizmo. The gizmo sits on the primary selected entity; the drag moves the
	// selected entities that have no selected ancestor (descendants follow their parents): translation moves them by the
	// gizmo's offset, rotation turns them around the gizmo's origin and scaling multiplies their scale by the gizmo's
	// factor along their own axes. Values are computed from the state at the start of the drag, so they never drift.
	// Writes notify systems (physics follows); in edit mode the whole drag becomes one undo step, while playing it
	// changes the running scene without undo. Main thread only.
	class TransformDrag
	{
	public:
		// Starts dragging the selection of the active scene; nullopt without a primary selection or for GizmoOperation::None.
		static std::optional<TransformDrag> Begin(EditorContext& context, GizmoOperation operation);

		// World transform of the primary entity when the drag started.
		const glm::mat4& GetStartTransform() const { return m_PrimaryStart; }
		GizmoOperation GetOperation() const { return m_Operation; }
		// The entities the drag writes (top-level selected entities).
		std::vector<UUID> GetEntities() const;
		// Whether the context's primary selection is still the entity the drag started on. Another selection (a click, or
		// automation) ends the drag: the gizmo now sits on another entity.
		bool IsForSelection(const EditorContext& context) const;

		// Applies the gizmo's world transform of the primary entity. Fails (changing nothing) when the active scene was
		// replaced since Begin (play mode started or stopped), the primary selection changed, the primary entity is gone
		// or the matrix is degenerate. Entities deleted meanwhile are skipped.
		bool Update(EditorContext& context, const glm::mat4& primaryWorld, std::string* outError = nullptr);
		// Ends the drag: the next edit starts a new undo step.
		void End(EditorContext& context);
	private:
		struct DraggedEntity
		{
			UUID ID;
			glm::mat4 StartWorld = glm::mat4(1.0f);
			TransformComponent StartLocal;
		};

		TransformDrag() = default;
	private:
		std::weak_ptr<Scene> m_Scene;
		GizmoOperation m_Operation = GizmoOperation::Translate;
		UUID m_Primary = UUID::Null();
		glm::mat4 m_PrimaryStart = glm::mat4(1.0f);
		std::vector<DraggedEntity> m_Entities;
		std::string m_MergeKey;
		std::string m_Name;
	};

}
