#include "Editor/TransformEdit.h"

#include "Editor/EditorContext.h"
#include "Editor/SceneEdit.h"

#include <Strata/Core/Log.h>
#include <Strata/Math/Math.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		bool IsFinite(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		const char* GetActionName(GizmoOperation operation, size_t count)
		{
			const bool plural = count > 1;
			switch (operation)
			{
				case GizmoOperation::Translate: return plural ? "Move Entities" : "Move Entity";
				case GizmoOperation::Rotate:    return plural ? "Rotate Entities" : "Rotate Entity";
				case GizmoOperation::Scale:     return plural ? "Scale Entities" : "Scale Entity";
				case GizmoOperation::None:      break;
			}
			return "Transform Entity";
		}

	}

	const char* GizmoOperationToString(GizmoOperation operation)
	{
		switch (operation)
		{
			case GizmoOperation::None:      return "None";
			case GizmoOperation::Translate: return "Translate";
			case GizmoOperation::Rotate:    return "Rotate";
			case GizmoOperation::Scale:     return "Scale";
		}
		return "None";
	}

	std::optional<GizmoOperation> GizmoOperationFromString(std::string_view name)
	{
		for (GizmoOperation operation : { GizmoOperation::None, GizmoOperation::Translate, GizmoOperation::Rotate, GizmoOperation::Scale })
		{
			if (name == GizmoOperationToString(operation))
				return operation;
		}
		return std::nullopt;
	}

	const char* GizmoSpaceToString(GizmoSpace space)
	{
		return space == GizmoSpace::World ? "World" : "Local";
	}

	std::optional<GizmoSpace> GizmoSpaceFromString(std::string_view name)
	{
		if (name == "Local")
			return GizmoSpace::Local;
		if (name == "World")
			return GizmoSpace::World;
		return std::nullopt;
	}

	namespace TransformEdit
	{

		std::optional<TransformComponent> WorldToLocal(const glm::mat4& parentWorld, const glm::mat4& world)
		{
			if (!IsFinite(parentWorld) || !IsFinite(world))
				return std::nullopt;
			if (!(std::abs(glm::determinant(parentWorld)) > Scene::c_MinInvertibleDeterminant))
				return std::nullopt;
			TransformComponent local;
			if (!local.SetTransform(glm::inverse(parentWorld) * world))
				return std::nullopt;
			return local;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// TransformDrag
	////////////////////////////////////////////////////////////////////////////////

	std::optional<TransformDrag> TransformDrag::Begin(EditorContext& context, GizmoOperation operation)
	{
		if (operation == GizmoOperation::None)
			return std::nullopt;
		const Ref<Scene>& scene = context.GetActiveScene();
		Entity primary = context.GetPrimarySelection();
		if (!primary)
			return std::nullopt;

		TransformDrag drag;
		drag.m_Scene = scene;
		drag.m_Operation = operation;
		drag.m_Primary = primary.GetUUID();
		drag.m_PrimaryStart = scene->GetWorldTransform(primary);

		// Descendants of dragged entities move with them; writing them too would apply the change twice.
		std::vector<Entity> selected;
		for (UUID id : context.GetSelection())
		{
			if (Entity entity = scene->GetEntityByUUID(id))
				selected.push_back(entity);
		}
		for (Entity entity : selected)
		{
			bool hasSelectedAncestor = false;
			for (Entity other : selected)
				hasSelectedAncestor |= other != entity && scene->IsDescendantOf(entity, other);
			if (!hasSelectedAncestor)
				drag.m_Entities.push_back({ entity.GetUUID(), scene->GetWorldTransform(entity), entity.GetComponent<TransformComponent>() });
		}

		drag.m_Name = GetActionName(operation, drag.m_Entities.size());
		// Unique per drag: consecutive drags of the same entities never merge into one undo step.
		drag.m_MergeKey = fmt::format("TransformDrag/{}", UUID().ToString());
		context.GetUndoStack().BreakMerge();
		if (context.IsPlaying())
		{
			ST_WARN("The scene is running ({}): moving entities changes the running copy and is discarded by play.stop",
				SceneStateToString(context.GetSceneState()));
		}
		return drag;
	}

	std::vector<UUID> TransformDrag::GetEntities() const
	{
		std::vector<UUID> ids;
		ids.reserve(m_Entities.size());
		for (const DraggedEntity& entity : m_Entities)
			ids.push_back(entity.ID);
		return ids;
	}

	bool TransformDrag::Update(EditorContext& context, const glm::mat4& primaryWorld, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const Ref<Scene>& scene = context.GetActiveScene();
		if (m_Scene.lock() != scene)
			return fail("The scene changed during the drag");
		if (!scene->GetEntityByUUID(m_Primary))
			return fail("The dragged entity no longer exists");

		glm::vec3 startTranslation;
		glm::quat startRotation;
		glm::vec3 startScale;
		glm::vec3 translation;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(m_PrimaryStart, startTranslation, startRotation, startScale) || !Math::DecomposeTransform(primaryWorld, translation, rotation, scale))
			return fail("The gizmo transform is degenerate");

		// Computed for every entity before anything is written, so a failure changes nothing.
		std::vector<std::pair<Entity, TransformComponent>> results;
		for (const DraggedEntity& dragged : m_Entities)
		{
			Entity entity = scene->GetEntityByUUID(dragged.ID);
			if (!entity)
				continue; // Deleted during the drag (e.g. by a running script)
			Entity parent = entity.GetParent();
			const glm::mat4 parentWorld = parent ? scene->GetWorldTransform(parent) : glm::mat4(1.0f);

			TransformComponent local = dragged.StartLocal;
			switch (m_Operation)
			{
				case GizmoOperation::Translate:
				{
					// Only the position changes, so rotation and scale stay exactly as they were.
					const glm::vec3 worldPosition = glm::vec3(dragged.StartWorld[3]) + (translation - startTranslation);
					if (!(std::abs(glm::determinant(parentWorld)) > Scene::c_MinInvertibleDeterminant))
						return fail(fmt::format("'{}' cannot move: its parent's transform is singular", entity.GetName()));
					local.Translation = glm::vec3(glm::inverse(parentWorld) * glm::vec4(worldPosition, 1.0f));
					break;
				}
				case GizmoOperation::Rotate:
				{
					// Turned around the gizmo's origin; the local scale is kept exactly.
					const glm::mat4 turn = glm::translate(glm::mat4(1.0f), startTranslation) * glm::mat4_cast(rotation * glm::inverse(startRotation))
						* glm::translate(glm::mat4(1.0f), -startTranslation);
					std::optional<TransformComponent> turned = TransformEdit::WorldToLocal(parentWorld, turn * dragged.StartWorld);
					if (!turned)
						return fail(fmt::format("'{}' cannot rotate: its transform cannot be represented", entity.GetName()));
					local.Translation = turned->Translation;
					local.Rotation = turned->Rotation;
					break;
				}
				case GizmoOperation::Scale:
					local.Scale = dragged.StartLocal.Scale * (scale / startScale);
					break;
				case GizmoOperation::None:
					return fail("No gizmo operation");
			}
			if (!IsFinite(local.Translation) || !IsFinite(local.Scale) || !std::isfinite(local.Rotation.w))
				return fail(fmt::format("'{}': the transform is not finite", entity.GetName()));
			results.emplace_back(entity, local);
		}

		std::vector<UUID> ids;
		for (const auto& [entity, local] : results)
			ids.push_back(entity.GetUUID());
		SceneEditTransaction transaction(*scene, m_Name, ids, m_MergeKey);
		for (auto& [entity, local] : results)
		{
			entity.GetComponent<TransformComponent>() = local;
			// The registry's update signal tells systems (physics) about the move.
			entity.MarkModified<TransformComponent>();
		}
		context.CommitEdit(transaction);
		return true;
	}

	void TransformDrag::End(EditorContext& context)
	{
		context.GetUndoStack().BreakMerge();
	}

}
