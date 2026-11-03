#include "stpch.h"
#include "Strata/Scene/Scene.h"

#include "Strata/Core/Assert.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

// Interpolation of what is drawn: the set of entities the fixed steps moved and their drawn transforms (Scene.h,
// "Interpolation of what is drawn"; Docs/DeveloperGuide.md, "Interpolation"). The capture and the per-frame rules hook
// into Scene::UpdateWorldTransforms (Scene.cpp); everything else is here.

namespace Strata
{

	namespace
	{

		// Sets of more entries than this are built and blended on the job system (as Scene.cpp's c_ParallelTransformWidth).
		constexpr uint32_t c_ParallelInterpolationWidth = 1024;
		// A per-frame write that moves a Blend entry's translation by more than this share of its distance from the origin (at
		// least 1 m) asked the entity to be there; less is the rounding of SetWorldTransform under a parent.
		constexpr float c_PerFrameMoveTolerance = 1.0e-5f;
		// Columns of a world matrix whose cosine exceeds this are sheared: no pose describes the matrix.
		constexpr float c_ShearTolerance = 1.0e-4f;
		// A rotation whose length differs from 1 by more than this is not a rotation the blend can take.
		constexpr float c_UnitRotationTolerance = 1.0e-3f;

		bool IsFiniteVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFiniteRotation(const glm::quat& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		bool IsBlendablePose(const InterpolationPose& pose)
		{
			return IsFiniteVector(pose.Translation) && IsFiniteVector(pose.Scale) && IsFiniteRotation(pose.Rotation)
				&& std::abs(glm::length(pose.Rotation) - 1.0f) <= c_UnitRotationTolerance && pose.Scale.x != 0.0f && pose.Scale.y != 0.0f
				&& pose.Scale.z != 0.0f;
		}

		bool SameMatrix(const glm::mat4& a, const glm::mat4& b)
		{
			return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
		}

		// Whether the scales have their negative components on the same axes: a blend between them never passes through zero.
		bool SameScaleSigns(const glm::vec3& a, const glm::vec3& b)
		{
			return (a.x < 0.0f) == (b.x < 0.0f) && (a.y < 0.0f) == (b.y < 0.0f) && (a.z < 0.0f) == (b.z < 0.0f);
		}

		// Rewrites the pose with the signs of `reference`'s scale, describing the same matrix. A root's pose keeps its authored
		// signs, e.g. Scale (1, 1, -1), where a decomposed matrix has the reflection folded into X (Math::DecomposeTransform):
		// Scale (-1, 1, 1) turned half around Y. Blending one into the other would squash the entity through zero scale and turn
		// it. False when an odd number of signs differ: the matrices' handedness differs, so no pose of either describes both.
		bool MatchScaleSigns(InterpolationPose& pose, const glm::vec3& reference)
		{
			glm::vec3 flip(1.0f);
			int flipped = 0;
			int kept = 0;
			for (int axis = 0; axis < 3; axis++)
			{
				if ((pose.Scale[axis] < 0.0f) != (reference[axis] < 0.0f))
				{
					flip[axis] = -1.0f;
					flipped++;
				}
				else
				{
					kept = axis;
				}
			}
			if (flipped == 0)
				return true;
			if (flipped != 2)
				return false;
			// R diag(s) = (R D) (D diag(s)) with D = diag(flip): two signs flipped make D the half turn about the third axis.
			glm::vec3 halfTurnAxis(0.0f);
			halfTurnAxis[kept] = 1.0f;
			pose.Rotation = pose.Rotation * glm::quat(0.0f, halfTurnAxis);
			pose.Scale *= flip;
			return true;
		}

		bool MovedPerFrame(const glm::vec3& before, const glm::vec3& after)
		{
			return glm::length(after - before) > c_PerFrameMoveTolerance * std::max(1.0f, glm::length(before));
		}

		// The pose `alpha` of the way from the entry's From to its To. The translation as From + (To - From) * alpha, not
		// glm::mix: monotonic and exact at both ends far from the origin. The rotation by a normalized lerp along the shorter
		// arc: within 0.03 degrees of a slerp at 30 degrees a step, 0.9 at 90.
		glm::mat4 BlendPose(const InterpolatedEntry& entry, float alpha)
		{
			const InterpolationPose& from = entry.From;
			const InterpolationPose& to = entry.To;
			const glm::vec3 translation = from.Translation + (to.Translation - from.Translation) * alpha;
			const glm::quat target = glm::dot(from.Rotation, to.Rotation) < 0.0f ? -to.Rotation : to.Rotation;
			const glm::quat rotation = glm::normalize(from.Rotation * (1.0f - alpha) + target * alpha);
			const glm::vec3 scale = from.Scale + (to.Scale - from.Scale) * alpha;
			return Math::ComposeTransform(translation, rotation, scale);
		}

		// A root's world pose: UpdateWorldTransforms composes its world matrix from exactly these values.
		InterpolationPose GetLocalPose(const TransformComponent& transform, bool& outExact)
		{
			InterpolationPose pose;
			pose.Translation = transform.Translation;
			pose.Rotation = transform.Rotation;
			pose.Scale = transform.Scale;
			outExact = !IsBlendablePose(pose);
			return pose;
		}

		// Any world matrix's pose; exact (not blendable) when it is sheared or does not decompose.
		InterpolationPose DecomposePose(const glm::mat4& world, bool& outExact)
		{
			InterpolationPose pose;
			outExact = true;
			const glm::vec3 columns[3] = { glm::vec3(world[0]), glm::vec3(world[1]), glm::vec3(world[2]) };
			const float lengths[3] = { glm::length(columns[0]), glm::length(columns[1]), glm::length(columns[2]) };
			for (int first = 0; first < 3; first++)
			{
				for (int second = first + 1; second < 3; second++)
				{
					if (std::abs(glm::dot(columns[first], columns[second])) > c_ShearTolerance * lengths[first] * lengths[second])
						return pose;
				}
			}
			if (!Math::DecomposeTransform(world, pose.Translation, pose.Rotation, pose.Scale))
				return pose;
			outExact = !IsBlendablePose(pose);
			return pose;
		}

		void ForSlots(uint32_t count, const std::function<void(uint32_t begin, uint32_t end)>& function)
		{
			if (count > c_ParallelInterpolationWidth && JobSystem::IsInitialized())
			{
				const uint32_t batchSize = std::max<uint32_t>(256, count / (std::max<uint32_t>(1, JobSystem::GetWorkerThreadCount() + 1) * 4));
				JobSystem::ParallelFor(count, batchSize, function);
			}
			else
			{
				function(0, count);
			}
		}

	}

	void Scene::SetInterpolation(bool enabled)
	{
		if (enabled == m_InterpolationEnabled)
			return;
		m_InterpolationEnabled = enabled;
		if (!enabled)
			ClearInterpolation();
	}

	uint32_t Scene::FindInterpolationSlot(entt::entity handle) const
	{
		if (handle == entt::null)
			return c_NoInterpolationSlot;
		const size_t index = static_cast<size_t>(entt::to_entity(handle));
		if (index >= m_InterpolationSlots.size())
			return c_NoInterpolationSlot;
		const uint32_t slot = m_InterpolationSlots[index];
		if (slot >= m_InterpolatedEntities.size() || m_InterpolatedEntities[slot] != handle)
			return c_NoInterpolationSlot;
		return slot;
	}

	InterpolationPose Scene::GetWorldPose(entt::entity handle, const glm::mat4& world, bool& outExact) const
	{
		const entt::registry& registry = m_Registry;
		const HierarchyComponent* node = handle != entt::null ? registry.try_get<HierarchyComponent>(handle) : nullptr;
		if (node && node->Parent == entt::null)
			return GetLocalPose(registry.get<TransformComponent>(handle), outExact);
		return DecomposePose(world, outExact);
	}

	void Scene::ApplyPerFrameRule(uint32_t slot, entt::entity handle, const glm::mat4& previous, bool ownChange, bool ancestorMoved)
	{
		const entt::registry& registry = m_Registry;
		InterpolatedEntry& entry = m_InterpolatedEntries[slot];
		if (entry.Role == InterpolationRole::Exact)
			return;
		if (entry.Role == InterpolationRole::Compose)
		{
			// It composes from its current local transform every frame; only a reparent leaves its parent slot.
			const entt::entity parent = registry.get<HierarchyComponent>(handle).Parent;
			if (entry.ParentSlot == c_NoInterpolationSlot || m_InterpolatedEntities[entry.ParentSlot] != parent)
				entry.Role = InterpolationRole::Exact;
			return;
		}

		// A Blend entry. Moved with an ancestor: its world poses no longer describe it.
		if (ancestorMoved || !ownChange)
		{
			entry.Role = InterpolationRole::Exact;
			return;
		}
		const glm::mat4& world = registry.get<WorldTransformComponent>(handle).Matrix;
		if (MovedPerFrame(glm::vec3(previous[3]), glm::vec3(world[3])))
		{
			// A per-frame move: it asked to be there.
			entry.Role = InterpolationRole::Exact;
			return;
		}
		// A turn or a scale (the player turning to walk, root motion's turn): drawn as written on every frame, while the
		// translation keeps blending. Blending the turn from the captured rotation would draw it lagging and catching up.
		bool exact = false;
		const InterpolationPose written = GetWorldPose(handle, world, exact);
		if (exact)
		{
			entry.Role = InterpolationRole::Exact;
			return;
		}
		entry.From.Rotation = written.Rotation;
		entry.To.Rotation = written.Rotation;
		entry.From.Scale = written.Scale;
		entry.To.Scale = written.Scale;
		entry.To.Translation = written.Translation;
	}

	void Scene::CaptureInterpolation(uint32_t steps, float timestep)
	{
		m_Captures.clear();
		UpdateWorldTransforms(TransformPass::Capture);

		const entt::registry& registry = m_Registry;
		const uint32_t count = static_cast<uint32_t>(m_Captures.size());
		const uint32_t oldCount = static_cast<uint32_t>(m_InterpolatedEntities.size());
		// Every captured entity has these (UpdateWorldTransforms computed it), so they exist whenever there are records; a scene
		// without entities may have none, so they are read only through records. Fetched once: a lookup through the registry
		// costs more than the rest of an entry's work.
		const auto* hierarchy = registry.storage<HierarchyComponent>();
		const auto* transforms = registry.storage<TransformComponent>();
		const auto* worlds = registry.storage<WorldTransformComponent>();
		ST_CORE_ASSERT(count == 0 || (hierarchy && transforms && worlds), "Scene '{}' captured transforms without their storages", m_Name);
		m_NextEntities.resize(count);
		m_NextEntries.resize(count);
		m_KeptSlots.assign(oldCount, 0);
		m_SlotScratch.assign(count, 0); // Created during the steps: decided by its parent below
		// The safety net: a step faster than box3d's maximum speed is a teleport.
		const float maxDistance = c_MaxInterpolatedSpeed * static_cast<float>(steps) * timestep;

		// 1. Fill: each record reads the registry and the old set, and writes only its own new entry (and the old slot it
		// keeps, which belongs to its entity alone).
		ForSlots(count, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end; index++)
			{
				const TransformCapture& capture = m_Captures[index];
				m_NextEntities[index] = capture.Entity;
				InterpolatedEntry& entry = m_NextEntries[index];
				entry = InterpolatedEntry {};
				const uint32_t old = FindInterpolationSlot(capture.Entity);
				if (old != c_NoInterpolationSlot)
					m_KeptSlots[old] = 1;
				if (!capture.HadWorld)
				{
					m_SlotScratch[index] = 1;
					continue;
				}
				if (!capture.OwnChange)
				{
					entry.Role = InterpolationRole::Compose;
					continue;
				}
				const glm::mat4& world = worlds->get(capture.Entity).Matrix;
				if (SameMatrix(world, capture.Previous))
					continue;
				bool exactTo = false;
				entry.To = hierarchy->get(capture.Entity).Parent == entt::null ? GetLocalPose(transforms->get(capture.Entity), exactTo)
					: DecomposePose(world, exactTo);
				bool exactFrom = false;
				// The per-frame rules keep a Blend entry's To equal to its world pose before the steps.
				if (old != c_NoInterpolationSlot && m_InterpolatedEntries[old].Role == InterpolationRole::Blend)
					entry.From = m_InterpolatedEntries[old].To;
				else
					entry.From = DecomposePose(capture.Previous, exactFrom);
				// Both poses in one convention: a root's To keeps its authored signs, a decomposed From folds them into X.
				if (!exactTo && !exactFrom && MatchScaleSigns(entry.From, entry.To.Scale)
					&& glm::length(entry.To.Translation - entry.From.Translation) <= maxDistance)
					entry.Role = InterpolationRole::Blend;
			}
		});

		// 2. Exits and slots: entities of the old set that the new one does not keep are drawn where they are from now on, which
		// readers of the transform change log learn.
		std::vector<entt::entity>& exits = m_ChangeScratch;
		exits.clear();
		for (uint32_t old = 0; old < oldCount; old++)
		{
			const entt::entity entity = m_InterpolatedEntities[old];
			const size_t index = static_cast<size_t>(entt::to_entity(entity));
			if (index < m_InterpolationSlots.size() && m_InterpolationSlots[index] == old)
				m_InterpolationSlots[index] = c_NoInterpolationSlot;
			if (!m_KeptSlots[old] && registry.valid(entity))
				exits.push_back(entity);
		}
		if (!exits.empty())
			RecordTransformChanges(++m_TransformsVersion, exits, exits.size() > c_MaxTransformChanges);
		m_InterpolatedEntities.swap(m_NextEntities);
		m_InterpolatedEntries.swap(m_NextEntries);
		for (uint32_t slot = 0; slot < count; slot++)
		{
			const size_t index = static_cast<size_t>(entt::to_entity(m_InterpolatedEntities[slot]));
			if (index >= m_InterpolationSlots.size())
				m_InterpolationSlots.resize(std::max(index + 1, m_InterpolationSlots.size() * 2), c_NoInterpolationSlot);
			m_InterpolationSlots[index] = slot;
		}

		// 3. Parents: the walk recorded parents first, so a parent in the set has a smaller slot. Entities created during the
		// steps ride their parent when it is in the set, and are drawn where they are otherwise.
		ForSlots(count, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t slot = begin; slot < end; slot++)
			{
				InterpolatedEntry& entry = m_InterpolatedEntries[slot];
				entry.ParentSlot = FindInterpolationSlot(hierarchy->get(m_InterpolatedEntities[slot]).Parent);
				if (m_SlotScratch[slot])
					entry.Role = entry.ParentSlot != c_NoInterpolationSlot ? InterpolationRole::Compose : InterpolationRole::Exact;
				else if (entry.Role == InterpolationRole::Compose && entry.ParentSlot == c_NoInterpolationSlot)
					entry.Role = InterpolationRole::Exact;
			}
		});

		// 4. Breaks during the steps: the broken entities are drawn where the steps left them, with every Blend entry below.
		if (!m_PendingBreaks.empty())
		{
			std::fill(m_SlotScratch.begin(), m_SlotScratch.end(), uint8_t(0));
			for (const entt::entity entity : m_PendingBreaks)
			{
				const uint32_t slot = FindInterpolationSlot(entity);
				if (slot == c_NoInterpolationSlot)
					continue;
				m_SlotScratch[slot] = 1;
				m_InterpolatedEntries[slot].Role = InterpolationRole::Exact;
			}
			for (uint32_t slot = 0; slot < count; slot++)
			{
				InterpolatedEntry& entry = m_InterpolatedEntries[slot];
				if (m_SlotScratch[slot] || entry.ParentSlot == c_NoInterpolationSlot || !m_SlotScratch[entry.ParentSlot])
					continue;
				m_SlotScratch[slot] = 1;
				if (entry.Role == InterpolationRole::Blend)
					entry.Role = InterpolationRole::Exact;
			}
			m_PendingBreaks.clear();
		}

		m_DrawnMatrices.resize(count);
		m_InterpolationSteps = std::max(1u, steps);
		m_InterpolationTimestep = timestep;
		m_DrawnStale = true;
		m_DrawnFilled = false;
		m_DrawnVersion++;
	}

	void Scene::ClearInterpolation()
	{
		const bool hadSet = !m_InterpolatedEntities.empty();
		if (hadSet)
		{
			// Every entity of the set is drawn where it is from now on.
			std::vector<entt::entity>& valid = m_NextEntities;
			valid.clear();
			for (const entt::entity entity : m_InterpolatedEntities)
			{
				const size_t index = static_cast<size_t>(entt::to_entity(entity));
				if (index < m_InterpolationSlots.size())
					m_InterpolationSlots[index] = c_NoInterpolationSlot;
				if (m_Registry.valid(entity))
					valid.push_back(entity);
			}
			if (!valid.empty())
				RecordTransformChanges(++m_TransformsVersion, valid, valid.size() > c_MaxTransformChanges);
			valid.clear();
			m_InterpolatedEntities.clear();
			m_InterpolatedEntries.clear();
			m_DrawnMatrices.clear();
		}
		m_PendingBreaks.clear();
		m_StepAlpha = 1.0f;
		m_InterpolationSteps = 1;
		m_InterpolationTimestep = 0.0f;
		m_DrawnAlpha = -1.0f;
		m_DrawnStale = true;
		m_DrawnFilled = false;
		if (hadSet)
			m_DrawnVersion++;
	}

	void Scene::UpdateDrawnTransforms()
	{
		UpdateWorldTransforms();
		if (m_InterpolatedEntities.empty() || !IsInterpolating())
			return;
		if (m_DrawnFilled && !m_DrawnStale && m_DrawnAlpha == m_StepAlpha)
			return;
		// Part of keeping what UpdateWorldTransforms keeps, timed with it.
		ST_PROFILE_STAGE("Scene.Transforms");

		const entt::registry& registry = m_Registry;
		const uint32_t count = static_cast<uint32_t>(m_InterpolatedEntities.size());
		const float alpha = m_StepAlpha;
		// 1. Blend and Exact entries: each reads the registry and its own entry and writes its own drawn matrix.
		ForSlots(count, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t slot = begin; slot < end; slot++)
			{
				const entt::entity entity = m_InterpolatedEntities[slot];
				const InterpolatedEntry& entry = m_InterpolatedEntries[slot];
				if (entry.Role == InterpolationRole::Compose || !registry.valid(entity))
					continue;
				const glm::mat4& world = registry.get<WorldTransformComponent>(entity).Matrix;
				// At alpha 1 the world matrix itself, bit for bit, whatever the decomposition.
				m_DrawnMatrices[slot] = entry.Role == InterpolationRole::Exact || alpha >= 1.0f ? world : BlendPose(entry, alpha);
			}
		});
		// 2. Compose entries, parents first, with the association UpdateWorldTransforms uses: under a parent drawn where it is,
		// a Compose entry equals its world matrix bit for bit.
		for (uint32_t slot = 0; slot < count; slot++)
		{
			const InterpolatedEntry& entry = m_InterpolatedEntries[slot];
			const entt::entity entity = m_InterpolatedEntities[slot];
			if (entry.Role != InterpolationRole::Compose || !registry.valid(entity))
				continue;
			if (entry.ParentSlot == c_NoInterpolationSlot || !registry.valid(m_InterpolatedEntities[entry.ParentSlot]))
				m_DrawnMatrices[slot] = registry.get<WorldTransformComponent>(entity).Matrix;
			else
				m_DrawnMatrices[slot] = Math::ComposeWorldTransform(m_DrawnMatrices[entry.ParentSlot],
					registry.get<TransformComponent>(entity).GetTransform());
		}
		m_DrawnAlpha = alpha;
		m_DrawnStale = false;
		m_DrawnFilled = true;
		m_DrawnVersion++;
	}

	const glm::mat4* Scene::FindDrawnTransform(entt::entity handle) const
	{
		if (!m_DrawnFilled)
			return nullptr;
		const uint32_t slot = FindInterpolationSlot(handle);
		return slot != c_NoInterpolationSlot ? &m_DrawnMatrices[slot] : nullptr;
	}

	glm::mat4 Scene::GetDrawnTransform(Entity entity) const
	{
		if (!entity.IsValid())
			return glm::mat4(1.0f);
		if (entity.GetScene() != this)
			return entity.GetScene()->GetDrawnTransform(entity);
		const uint32_t slot = IsInterpolating() ? FindInterpolationSlot(entity.GetHandle()) : c_NoInterpolationSlot;
		if (slot == c_NoInterpolationSlot)
			return GetWorldTransform(entity);
		if (m_DrawnFilled && !m_DrawnStale && m_DrawnAlpha == m_StepAlpha && m_DirtyTransforms.empty())
			return m_DrawnMatrices[slot];
		return ComputeDrawnTransform(slot);
	}

	glm::mat4 Scene::ComputeDrawnTransform(uint32_t slot) const
	{
		const entt::registry& registry = m_Registry;
		const entt::entity handle = m_InterpolatedEntities[slot];
		const Entity entity(handle, const_cast<Scene*>(this));
		const InterpolatedEntry& entry = m_InterpolatedEntries[slot];
		const float alpha = m_StepAlpha;
		if (entry.Role == InterpolationRole::Exact)
			return GetWorldTransform(entity);
		const HierarchyComponent& node = registry.get<HierarchyComponent>(handle);
		if (entry.Role == InterpolationRole::Compose)
		{
			// The next update makes a reparented entry Exact.
			if (entry.ParentSlot == c_NoInterpolationSlot || m_InterpolatedEntities[entry.ParentSlot] != node.Parent)
				return GetWorldTransform(entity);
			return Math::ComposeWorldTransform(ComputeDrawnTransform(entry.ParentSlot),
				registry.get<TransformComponent>(handle).GetTransform());
		}

		if (alpha >= 1.0f)
			return GetWorldTransform(entity);
		if (FindCurrentWorldTransform(handle))
			return BlendPose(entry, alpha);
		// A write is pending: drawn as the next update's per-frame rule will draw it. Its own turn or scale keeps the
		// translation blending; a move, or a write to an ancestor, draws it where it is.
		const glm::mat4 world = GetWorldTransform(entity);
		if (!node.TransformDirty)
			return world;
		for (entt::entity ancestor = node.Parent; ancestor != entt::null; ancestor = registry.get<HierarchyComponent>(ancestor).Parent)
		{
			if (registry.get<HierarchyComponent>(ancestor).TransformDirty)
				return world;
		}
		if (MovedPerFrame(glm::vec3(registry.get<WorldTransformComponent>(handle).Matrix[3]), glm::vec3(world[3])))
			return world;
		bool exact = false;
		const InterpolationPose written = GetWorldPose(handle, world, exact);
		if (exact)
			return world;
		InterpolatedEntry turned = entry;
		turned.From.Rotation = written.Rotation;
		turned.To.Rotation = written.Rotation;
		turned.From.Scale = written.Scale;
		turned.To.Scale = written.Scale;
		turned.To.Translation = written.Translation;
		return BlendPose(turned, alpha);
	}

	void Scene::BreakInterpolation(Entity entity)
	{
		if (!entity.IsValid())
			return;
		if (entity.GetScene() != this)
		{
			entity.GetScene()->BreakInterpolation(entity);
			return;
		}
		if (!IsInterpolating())
			return;
		const uint32_t slot = FindInterpolationSlot(entity.GetHandle());
		if (m_InFixedSteps)
		{
			// The capture after the steps applies it to the poses they leave.
			m_PendingBreaks.push_back(entity.GetHandle());
			if (slot != c_NoInterpolationSlot)
				m_InterpolatedEntries[slot].Role = InterpolationRole::Exact;
			return;
		}
		if (slot != c_NoInterpolationSlot)
			BreakSlot(slot);
	}

	void Scene::BreakSlot(uint32_t slot)
	{
		const uint32_t count = static_cast<uint32_t>(m_InterpolatedEntities.size());
		std::vector<entt::entity>& jumped = m_ChangeScratch;
		jumped.clear();
		m_SlotScratch.assign(count, 0);
		m_SlotScratch[slot] = 1;
		m_InterpolatedEntries[slot].Role = InterpolationRole::Exact;
		jumped.push_back(m_InterpolatedEntities[slot]);
		// Descendants have larger slots (parents first); Compose entries below follow their parent by themselves.
		for (uint32_t below = slot + 1; below < count; below++)
		{
			InterpolatedEntry& entry = m_InterpolatedEntries[below];
			if (entry.ParentSlot == c_NoInterpolationSlot || !m_SlotScratch[entry.ParentSlot])
				continue;
			m_SlotScratch[below] = 1;
			if (entry.Role == InterpolationRole::Blend)
				entry.Role = InterpolationRole::Exact;
			if (m_Registry.valid(m_InterpolatedEntities[below]))
				jumped.push_back(m_InterpolatedEntities[below]);
		}
		RecordTransformChanges(++m_TransformsVersion, jumped, jumped.size() > c_MaxTransformChanges);
		m_DrawnStale = true;
		m_DrawnVersion++;
	}

	InterpolationStats Scene::GetInterpolationStats() const
	{
		InterpolationStats stats;
		for (uint32_t slot = 0; slot < m_InterpolatedEntities.size(); slot++)
		{
			if (!m_Registry.valid(m_InterpolatedEntities[slot]))
				continue;
			stats.Entities++;
			switch (m_InterpolatedEntries[slot].Role)
			{
				case InterpolationRole::Blend: stats.Blended++; break;
				case InterpolationRole::Compose: stats.Composed++; break;
				case InterpolationRole::Exact: stats.Exact++; break;
			}
		}
		return stats;
	}

	bool Scene::ValidateInterpolation(std::string* outError) const
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		const uint32_t count = static_cast<uint32_t>(m_InterpolatedEntities.size());
		if (m_InterpolatedEntries.size() != count || m_DrawnMatrices.size() != count)
			return fail("the interpolated set's arrays differ in size");
		if (count > 0 && !IsInterpolating())
			return fail("the scene keeps an interpolated set while it does not interpolate");
		const entt::registry& registry = m_Registry;
		for (uint32_t slot = 0; slot < count; slot++)
		{
			const entt::entity entity = m_InterpolatedEntities[slot];
			if (!registry.valid(entity))
				continue;
			if (FindInterpolationSlot(entity) != slot)
				return fail(fmt::format("entity {} of interpolation slot {} does not map back to it", static_cast<uint32_t>(entity), slot));
			const InterpolatedEntry& entry = m_InterpolatedEntries[slot];
			if (entry.ParentSlot != c_NoInterpolationSlot && entry.ParentSlot >= slot)
				return fail(fmt::format("interpolation slot {} comes before its parent's", slot));
			const HierarchyComponent& node = registry.get<HierarchyComponent>(entity);
			if (entry.Role == InterpolationRole::Compose && !node.TransformDirty
				&& (entry.ParentSlot == c_NoInterpolationSlot || m_InterpolatedEntities[entry.ParentSlot] != node.Parent))
				return fail(fmt::format("the composed entity {} does not name its hierarchy parent", static_cast<uint32_t>(entity)));
			if (entry.Role == InterpolationRole::Blend && (!IsBlendablePose(entry.From) || !IsBlendablePose(entry.To)))
				return fail(fmt::format("the blended entity {} has a pose that is not finite or not a rotation", static_cast<uint32_t>(entity)));
			if (entry.Role == InterpolationRole::Blend && !SameScaleSigns(entry.From.Scale, entry.To.Scale))
				return fail(fmt::format("the blended entity {} blends between scales of other signs", static_cast<uint32_t>(entity)));
		}
		return true;
	}

}
