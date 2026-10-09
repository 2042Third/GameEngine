#include <doctest/doctest.h>

#include "Editor/EditorContext.h"
#include "Editor/TransformEdit.h"

#include <Strata/Math/Math.h>
#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace Strata;

namespace
{

	constexpr float c_Epsilon = 1e-4f;

	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = c_Epsilon)
	{
		return Math::IsNearlyEqual(a, b, epsilon);
	}

	bool Near(const glm::mat4& a, const glm::mat4& b, float epsilon = c_Epsilon)
	{
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				if (std::abs(a[column][row] - b[column][row]) > epsilon)
					return false;
			}
		}
		return true;
	}

	glm::mat4 Compose(const glm::vec3& translation, const glm::vec3& eulerDegrees, const glm::vec3& scale)
	{
		return Math::ComposeTransform(translation, Math::EulerDegreesToQuat(eulerDegrees), scale);
	}

	Entity CreateEntity(Scene& scene, const std::string& name, const glm::vec3& translation, const glm::vec3& eulerDegrees = glm::vec3(0.0f),
		const glm::vec3& scale = glm::vec3(1.0f), Entity parent = {})
	{
		Entity entity = parent ? scene.CreateChildEntity(parent, name) : scene.CreateEntity(name);
		TransformComponent& transform = entity.GetComponent<TransformComponent>();
		transform.Translation = translation;
		transform.Rotation = Math::EulerDegreesToQuat(eulerDegrees);
		transform.Scale = scale;
		return entity;
	}

	glm::vec3 WorldPosition(const Scene& scene, Entity entity)
	{
		return glm::vec3(scene.GetWorldTransform(entity)[3]);
	}

	nlohmann::json Snapshot(const EditorContext& context)
	{
		return SceneSerializer::Serialize(*context.GetEditScene())["Scene"]["Entities"];
	}

	// Counts the registry's update signals for transforms (what physics listens to).
	struct TransformSignalCounter
	{
		int Count = 0;
		void OnUpdate(entt::registry&, entt::entity) { Count++; }
	};

}

TEST_SUITE("Editor.TransformEdit")
{
	TEST_CASE("World transforms convert to local ones under rotated and scaled parents")
	{
		const glm::mat4 world = Compose(glm::vec3(3.0f, -2.0f, 5.0f), glm::vec3(10.0f, 70.0f, -30.0f), glm::vec3(1.5f, 0.5f, 2.0f));

		// Top level: the world transform is the local one.
		std::optional<TransformComponent> local = TransformEdit::WorldToLocal(glm::mat4(1.0f), world);
		REQUIRE(local);
		CHECK(Near(local->GetTransform(), world));

		// Rotated parents with uniform scale (also mirrored) and parents scaled along the child's axes are exact.
		const glm::mat4 parents[] = {
			Compose(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f, 45.0f, 0.0f), glm::vec3(1.0f)),
			Compose(glm::vec3(-4.0f, 0.5f, 9.0f), glm::vec3(30.0f, -60.0f, 15.0f), glm::vec3(2.5f)),
			Compose(glm::vec3(0.0f), glm::vec3(0.0f, 90.0f, 0.0f), glm::vec3(-2.0f, 2.0f, 2.0f)),
			Compose(glm::vec3(1.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(3.0f, 0.5f, 1.25f))
		};
		const glm::mat4 children[] = {
			world,
			Compose(glm::vec3(0.5f, 1.0f, -2.0f), glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f))
		};
		for (size_t parentIndex = 0; parentIndex < std::size(parents); parentIndex++)
		{
			for (const glm::mat4& child : children)
			{
				CAPTURE(parentIndex);
				// Only shear-free combinations: a non-uniformly scaled parent with a rotated child cannot be expressed exactly.
				if (parentIndex == 3 && &child == &children[0])
					continue;
				local = TransformEdit::WorldToLocal(parents[parentIndex], child);
				REQUIRE(local);
				CHECK(Near(parents[parentIndex] * local->GetTransform(), child, 1e-3f));
			}
		}

		// A non-uniformly scaled parent with a rotated child gives the nearest transform: the position stays exact.
		local = TransformEdit::WorldToLocal(parents[3], world);
		REQUIRE(local);
		CHECK(Near(glm::vec3((parents[3] * local->GetTransform())[3]), glm::vec3(world[3]), 1e-3f));

		// Singular parents and non-finite matrices cannot be converted.
		CHECK_FALSE(TransformEdit::WorldToLocal(Compose(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 1.0f)), world));
		glm::mat4 broken = world;
		broken[3][0] = std::nanf("");
		CHECK_FALSE(TransformEdit::WorldToLocal(glm::mat4(1.0f), broken));
		CHECK_FALSE(TransformEdit::WorldToLocal(glm::mat4(1.0f), glm::mat4(0.0f)));
	}

	TEST_CASE("Gizmo names parse back")
	{
		for (GizmoOperation operation : { GizmoOperation::None, GizmoOperation::Translate, GizmoOperation::Rotate, GizmoOperation::Scale })
			CHECK(GizmoOperationFromString(GizmoOperationToString(operation)) == operation);
		for (GizmoSpace space : { GizmoSpace::Local, GizmoSpace::World })
			CHECK(GizmoSpaceFromString(GizmoSpaceToString(space)) == space);
		CHECK_FALSE(GizmoOperationFromString("translate"));
		CHECK_FALSE(GizmoSpaceFromString("Global"));
	}

	TEST_CASE("Translating under a rotated, scaled parent moves by the world offset and is one undo step")
	{
		EditorContext context(EditorContextSpecification { false });
		Scene& scene = *context.GetEditScene();
		Entity parent = CreateEntity(scene, "Parent", glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f, 90.0f, 0.0f), glm::vec3(2.0f));
		Entity child = CreateEntity(scene, "Child", glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 30.0f), glm::vec3(1.0f, 0.5f, 1.0f), parent);
		const TransformComponent childStart = child.GetComponent<TransformComponent>();
		const glm::vec3 start = WorldPosition(scene, child);
		const nlohmann::json before = Snapshot(context);
		context.Select(child.GetUUID());

		CHECK_FALSE(TransformDrag::Begin(context, GizmoOperation::None));
		std::optional<TransformDrag> drag = TransformDrag::Begin(context, GizmoOperation::Translate);
		REQUIRE(drag);
		CHECK(Near(drag->GetStartTransform(), scene.GetWorldTransform(child)));
		CHECK(drag->GetEntities() == std::vector<UUID> { child.GetUUID() });

		// The gizmo reports absolute matrices frame by frame; only the last one counts.
		for (int frame = 1; frame <= 5; frame++)
		{
			const glm::mat4 gizmo = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f * static_cast<float>(frame), 0.0f, -1.0f)) * drag->GetStartTransform();
			std::string error;
			REQUIRE_MESSAGE(drag->Update(context, gizmo, &error), error);
		}
		drag->End(context);
		CHECK(Near(WorldPosition(scene, child), start + glm::vec3(2.5f, 0.0f, -1.0f)));
		const TransformComponent& moved = child.GetComponent<TransformComponent>();
		CHECK(Math::IsNearlyEqual(moved.Rotation, childStart.Rotation));
		CHECK(moved.Scale == childStart.Scale);

		const std::vector<std::string> history = context.GetUndoStack().GetHistory();
		REQUIRE(history.size() == 1);
		CHECK(history[0] == "Move Entity");
		const nlohmann::json after = Snapshot(context);
		REQUIRE(context.Undo());
		CHECK(Snapshot(context) == before);
		REQUIRE(context.Redo());
		CHECK(Snapshot(context) == after);

		// A second drag is a second step, even for the same entity.
		drag = TransformDrag::Begin(context, GizmoOperation::Translate);
		REQUIRE(drag);
		REQUIRE(drag->Update(context, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * drag->GetStartTransform()));
		drag->End(context);
		CHECK(context.GetUndoStack().GetHistory().size() == 2);
	}

	TEST_CASE("Selected entities follow the primary one; selected descendants are not moved twice")
	{
		EditorContext context(EditorContextSpecification { false });
		Scene& scene = *context.GetEditScene();
		Entity a = CreateEntity(scene, "A", glm::vec3(0.0f));
		Entity b = CreateEntity(scene, "B", glm::vec3(4.0f, 0.0f, 0.0f), glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(2.0f, 1.0f, 1.0f));
		Entity bChild = CreateEntity(scene, "BChild", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), b);
		context.SetSelection({ b.GetUUID(), bChild.GetUUID(), a.GetUUID() }); // A is primary
		const glm::vec3 bStart = WorldPosition(scene, b);
		const glm::vec3 childStart = WorldPosition(scene, bChild);

		SUBCASE("Translate")
		{
			std::optional<TransformDrag> drag = TransformDrag::Begin(context, GizmoOperation::Translate);
			REQUIRE(drag);
			const std::vector<UUID> dragged = drag->GetEntities();
			CHECK(dragged.size() == 2);
			CHECK(std::find(dragged.begin(), dragged.end(), bChild.GetUUID()) == dragged.end());
			REQUIRE(drag->Update(context, glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f))));
			drag->End(context);
			CHECK(Near(WorldPosition(scene, a), glm::vec3(1.0f, 2.0f, 3.0f)));
			CHECK(Near(WorldPosition(scene, b), bStart + glm::vec3(1.0f, 2.0f, 3.0f)));
			CHECK(Near(WorldPosition(scene, bChild), childStart + glm::vec3(1.0f, 2.0f, 3.0f)));
			CHECK(context.GetUndoStack().GetHistory() == std::vector<std::string> { "Move Entities" });
		}

		SUBCASE("Rotate around the primary entity's origin")
		{
			std::optional<TransformDrag> drag = TransformDrag::Begin(context, GizmoOperation::Rotate);
			REQUIRE(drag);
			const glm::quat turn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			REQUIRE(drag->Update(context, glm::mat4_cast(turn)));
			CHECK(Near(WorldPosition(scene, a), glm::vec3(0.0f)));
			CHECK(Math::IsNearlyEqual(a.GetComponent<TransformComponent>().Rotation, turn));
			// B orbits A: +X becomes -Z after 90 degrees around +Y; its own turn adds up and its scale is kept.
			CHECK(Near(WorldPosition(scene, b), glm::vec3(0.0f, 0.0f, -4.0f), 1e-3f));
			CHECK(Math::IsNearlyEqual(b.GetComponent<TransformComponent>().Rotation, turn * Math::EulerDegreesToQuat(glm::vec3(0.0f, 30.0f, 0.0f)), 1e-4f));
			CHECK(b.GetComponent<TransformComponent>().Scale == glm::vec3(2.0f, 1.0f, 1.0f));
			drag->End(context);
			CHECK(context.GetUndoStack().GetHistory() == std::vector<std::string> { "Rotate Entities" });
		}

		SUBCASE("Scale by the primary entity's factor")
		{
			std::optional<TransformDrag> drag = TransformDrag::Begin(context, GizmoOperation::Scale);
			REQUIRE(drag);
			REQUIRE(drag->Update(context, glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 0.5f))));
			drag->End(context);
			CHECK(Near(a.GetComponent<TransformComponent>().Scale, glm::vec3(2.0f, 3.0f, 0.5f)));
			CHECK(Near(b.GetComponent<TransformComponent>().Scale, glm::vec3(4.0f, 3.0f, 0.5f)));
			CHECK(Near(WorldPosition(scene, b), bStart)); // Positions stay
			CHECK(bChild.GetComponent<TransformComponent>().Scale == glm::vec3(1.0f));
		}
	}

	TEST_CASE("Drag edits notify systems, skip deleted entities and stop when the scene changes")
	{
		EditorContext context(EditorContextSpecification { false });
		Scene& scene = *context.GetEditScene();
		Entity a = CreateEntity(scene, "A", glm::vec3(0.0f));
		Entity b = CreateEntity(scene, "B", glm::vec3(2.0f, 0.0f, 0.0f));
		context.SetSelection({ b.GetUUID(), a.GetUUID() });

		TransformSignalCounter counter;
		scene.GetRegistry().on_update<TransformComponent>().connect<&TransformSignalCounter::OnUpdate>(counter);
		std::optional<TransformDrag> drag = TransformDrag::Begin(context, GizmoOperation::Translate);
		REQUIRE(drag);
		REQUIRE(drag->Update(context, glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f))));
		CHECK(counter.Count == 2);

		// A degenerate gizmo matrix changes nothing.
		std::string error;
		CHECK_FALSE(drag->Update(context, glm::mat4(0.0f), &error));
		CHECK_FALSE(error.empty());
		CHECK(Near(WorldPosition(scene, a), glm::vec3(1.0f, 0.0f, 0.0f)));

		// Entities deleted during the drag are skipped; without the primary entity the drag fails.
		scene.DestroyEntity(b);
		REQUIRE(drag->Update(context, glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f))));
		CHECK(Near(WorldPosition(scene, a), glm::vec3(2.0f, 0.0f, 0.0f)));
		scene.DestroyEntity(a);
		CHECK_FALSE(drag->Update(context, glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f)), &error));
		scene.GetRegistry().on_update<TransformComponent>().disconnect(&counter);

		// Without a selection there is nothing to drag.
		context.ClearSelection();
		CHECK_FALSE(TransformDrag::Begin(context, GizmoOperation::Translate));

		// Starting or stopping play mode replaces the active scene: the drag cannot continue.
		Entity c = CreateEntity(scene, "C", glm::vec3(0.0f));
		context.Select(c.GetUUID());
		drag = TransformDrag::Begin(context, GizmoOperation::Translate);
		REQUIRE(drag);
		REQUIRE(context.Play());
		CHECK_FALSE(drag->Update(context, glm::mat4(1.0f), &error));
		CHECK(error.find("scene changed") != std::string::npos);
		context.Stop();
	}

	TEST_CASE("While playing, drags move the running scene without undo and physics follows")
	{
		EditorContext context(EditorContextSpecification { false });
		Entity wall = CreateEntity(*context.GetEditScene(), "Wall", glm::vec3(0.0f, 0.0f, -5.0f));
		wall.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(1.0f);
		const nlohmann::json edited = Snapshot(context);
		REQUIRE(context.Play());
		Scene& running = *context.GetActiveScene();
		Entity runningWall = running.GetEntityByUUID(wall.GetUUID());
		REQUIRE(runningWall);
		context.Select(wall.GetUUID());

		PhysicsSystem* physics = running.GetSystem<PhysicsSystem>();
		REQUIRE(physics);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		CHECK(physics->Raycast(glm::vec3(0.0f, 10.0f, -5.0f), down, 20.0f));

		// The static collider only follows signalled edits; the drag signals them.
		std::optional<TransformDrag> drag = TransformDrag::Begin(context, GizmoOperation::Translate);
		REQUIRE(drag);
		REQUIRE(drag->Update(context, glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)) * drag->GetStartTransform()));
		drag->End(context);
		running.OnUpdateRuntime(running.GetSettings().FixedTimestep);
		CHECK_FALSE(physics->Raycast(glm::vec3(0.0f, 10.0f, -5.0f), down, 20.0f));
		const std::optional<RaycastHit> hit = physics->Raycast(glm::vec3(10.0f, 10.0f, -5.0f), down, 20.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == runningWall);

		CHECK(context.GetUndoStack().GetHistory().empty());
		context.Stop();
		CHECK(Snapshot(context) == edited);
	}
}
