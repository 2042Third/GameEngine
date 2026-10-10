#pragma once

#include <entt/entt.hpp>

#include <cstdint>

namespace Strata
{

	// Runtime hierarchy links of a scene entity, kept by Scene next to RelationshipComponent (which stays the serialized form
	// and the authoritative child order). Hot paths (world transforms, subtrees, ancestry, hierarchy order) walk these EnTT
	// handles instead of looking UUIDs up. Every structural operation of Scene maintains them; nothing else may write them.
	// Not registered with ComponentRegistry (like WorldTransformComponent): never serialized, copied or shown.
	struct HierarchyComponent
	{
		entt::entity Parent = entt::null;      // entt::null for root entities
		entt::entity FirstChild = entt::null;
		entt::entity LastChild = entt::null;   // Appending a child is O(1)
		entt::entity NextSibling = entt::null; // Siblings in child order (roots: in root order)
		entt::entity PrevSibling = entt::null;
		uint32_t Depth = 0;                    // Roots have depth 0
		uint32_t ChildCount = 0;
		// Position among the siblings; current only while the parent's ChildIndicesValid (or, for roots, the scene's root
		// indices) are. Read it through Scene::GetSiblingIndex, which refreshes the whole sibling list in one pass.
		uint32_t SiblingIndex = 0;
		// Scratch of Scene::UpdateWorldTransforms (which entities have a dirty ancestor, memoized per update).
		uint32_t TransformPass = 0;
		bool DirtyAbove = false;
		// The entity's local transform or parent changed since the last UpdateWorldTransforms: its cached world transform and
		// those of its descendants are stale.
		bool TransformDirty = false;
		// The SiblingIndex of every child is current. Appending keeps it; other changes of the child list clear it.
		bool ChildIndicesValid = true;
	};

}
