#ifndef STRATA_LOD_SELECTION_GLSL
#define STRATA_LOD_SELECTION_GLSL

// The level of detail drawn for a screen fraction (projected bounding sphere diameter / viewport height), exactly as
// LODSelection::SelectLOD (Strata/Renderer/LODSelection.cpp) chooses it: 0 at or above the threshold, else 1 plus the
// number of times the fraction can be doubled while staying below the threshold, at most lodCount - 1. Only comparisons
// and exact doublings, so the CPU and the GPU agree for the same fraction.
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

#endif
