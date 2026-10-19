#ifndef STRATA_LOD_SELECTION_GLSL
#define STRATA_LOD_SELECTION_GLSL

// The level of detail drawn for a screen fraction (projected bounding sphere diameter / viewport height), exactly as
// LODSelection::SelectLOD (Strata/Renderer/LODSelection.cpp) chooses it: 0 at or above the threshold, else 1 plus the
// number of times the fraction can be doubled while staying below the threshold, at most lodCount - 1. Only comparisons
// and exact doublings, so the CPU and the GPU agree for the same fraction. Submeshes whose levels have no errors choose
// this way.
uint SelectLOD(float screenFraction, float lodThreshold, uint lodCount)
{
	if (lodCount <= 1u || !(screenFraction > 0.0 && screenFraction < lodThreshold))
		return 0u;
	uint lod = 1u;
	float doubled = screenFraction * 2.0;
	while (lod + 1u < lodCount && doubled < lodThreshold)
	{
		lod++;
		doubled *= 2.0;
	}
	return lod;
}

#ifdef ST_LOD_RELATIVE_ERROR
// The coarsest level whose projected error stays within the threshold, exactly as LODSelection::SelectLODByError chooses
// it: the last level, scanning down to 1, whose relative error times the screen fraction is at most errorThreshold (one
// rounded multiplication, `precise` so that nothing fuses or reorders it), else 0; 0 for a fraction that is not positive or
// not finite. The includer defines ST_LOD_RELATIVE_ERROR(record) to read the relative error of a level, firstLOD + level.
uint SelectLODByError(float screenFraction, uint firstLOD, uint lodCount, float errorThreshold)
{
	if (lodCount <= 1u || !(screenFraction > 0.0) || isinf(screenFraction))
		return 0u;
	for (uint level = lodCount - 1u; level > 0u; level--)
	{
		precise float projected = screenFraction * ST_LOD_RELATIVE_ERROR(firstLOD + level);
		if (projected <= errorThreshold)
			return level;
	}
	return 0u;
}
#endif

#endif
