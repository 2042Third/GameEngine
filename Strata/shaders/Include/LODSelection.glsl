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

// Level-of-detail cross-fades, exactly as LODSelection::UpdateLODFade keeps them (Strata/Renderer/LODSelection.cpp): the
// state of an instance (x: level, level fading out, step, frame; y: entity), the level drawn this frame, its level count, its
// entity and the frame's number. Returns the state to keep; outCode is the draw entry's code (0: no fade), outFrom the
// level fading out (drawn with outCode + ST_LOD_FADE_OUTGOING).
#define ST_LOD_FADE_FRAMES   16u          // RenderData::c_LODFadeFrames
#define ST_LOD_FADE_OUTGOING 16u          // RenderData::c_LODFadeOutgoing
#define ST_LOD_FADE_FRAME_MASK 0x7FFFFu   // The frame number's 19 bits kept

uvec2 UpdateLODFade(uvec2 state, uint target, uint lodCount, uint entityID, uint frame, bool enabled, out uint outCode, out uint outFrom)
{
	outCode = 0u;
	outFrom = target;
	if (!enabled || target > 15u)
		return uvec2(0u);
	uint lod = state.x & 15u;
	uint from = (state.x >> 4) & 15u;
	uint step = (state.x >> 8) & 31u;
	uint last = state.x >> 13;
	bool continuing = state.x != 0u && state.y == entityID && last == ((frame - 1u) & ST_LOD_FADE_FRAME_MASK) && lod < lodCount && from < lodCount
		&& step >= 1u && step <= ST_LOD_FADE_FRAMES;
	if (!continuing)
	{
		lod = target;
		from = target;
		step = ST_LOD_FADE_FRAMES;
	}
	else if (target != lod)
	{
		if (step < ST_LOD_FADE_FRAMES && target == from)
		{
			from = lod;
			step = ST_LOD_FADE_FRAMES - step;
		}
		else
		{
			from = step < ST_LOD_FADE_FRAMES && 2u * step < ST_LOD_FADE_FRAMES ? from : lod;
			step = 1u;
		}
		lod = target;
	}
	else if (step < ST_LOD_FADE_FRAMES)
	{
		step++;
	}
	if (from == lod)
		step = ST_LOD_FADE_FRAMES;
	outCode = step < ST_LOD_FADE_FRAMES ? step : 0u;
	outFrom = from;
	return uvec2(lod | (from << 4) | (step << 8) | ((frame & ST_LOD_FADE_FRAME_MASK) << 13), entityID);
}

#endif
