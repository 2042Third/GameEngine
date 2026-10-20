#pragma once

#include "Strata/Physics/PhysicsTypes.h"

#include <box3d/types.h>

#include <array>
#include <cstdint>

// How the physics module maps collision layers to box3d's filters (internal to the Physics module, which alone includes
// box3d). A shape on user layer L (0-31, see c_PhysicsLayerCount) has the category bit L and, as its mask, the layers L
// collides with in the scene's matrix: box3d lets two shapes collide only if each one's mask holds the other's category,
// which is the matrix's rule (both masks must allow the pair). Every mask also holds c_QueryCategory, which no shape has
// and every query asks for, so queries find a shape whatever its layer collides with.
namespace Strata::PhysicsFilters
{

	constexpr uint64_t c_QueryCategory = uint64_t(1) << 63;
	static_assert(c_PhysicsLayerCount <= 63, "The user layers and the query category share box3d's 64 category bits");

	inline b3Filter MakeShapeFilter(uint32_t layer, const std::array<uint32_t, c_PhysicsLayerCount>& masks)
	{
		b3Filter filter = b3DefaultFilter();
		filter.categoryBits = uint64_t(1) << layer;
		filter.maskBits = static_cast<uint64_t>(masks[layer]) | c_QueryCategory;
		filter.groupIndex = 0;
		return filter;
	}

	// A query of the shapes on the layers of `layerMask` (bit L: user layer L).
	inline b3QueryFilter MakeQueryFilter(uint32_t layerMask)
	{
		b3QueryFilter filter = b3DefaultQueryFilter();
		filter.categoryBits = c_QueryCategory;
		filter.maskBits = static_cast<uint64_t>(layerMask);
		return filter;
	}

	// A query of the shapes a body on `layer` collides with in the matrix (a character controller's move).
	inline b3QueryFilter MakeLayerQueryFilter(uint32_t layer, const std::array<uint32_t, c_PhysicsLayerCount>& masks)
	{
		b3QueryFilter filter = b3DefaultQueryFilter();
		filter.categoryBits = uint64_t(1) << layer;
		filter.maskBits = static_cast<uint64_t>(masks[layer]);
		return filter;
	}

}
