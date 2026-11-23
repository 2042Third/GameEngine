#ifndef STRATA_AREA_SHADOWS_SLANG
#define STRATA_AREA_SHADOWS_SLANG

// Rect lights' soft shadows (DeveloperGuide "Area lights"): percentage-closer soft shadows from the light's six local shadow
// views at its face's center, filtered with the light's own size, as the sun's disk is with the sun's. Per receiver, in the
// cube face that sees it: the atlas's depth ranges (u_LocalShadowRanges, Scene/LocalShadowRanges.comp) classify it as lit or
// in umbra without a comparison, else give the blockers' mean depth, from which the light's face seen through the receiver
// is the kernel: the parallelogram of its two half axes projected into the view, scaled by (z_r - z_b) / (z_r z_b) (a light of
// half size s and a blocker at z_b give a penumbra half width s (z_r - z_b) / z_b at the receiver z_r), long along a strip
// light and narrow across it. Each tap stands for a point of the face and looks three times along the path from that point to
// the receiver: where the path crosses the tap (an occluder beside every path, such as a wall beside a floor point, blocks
// nothing), at two thirds of its offset (a solid the path entered between the two) and at a third of it against the receiver's
// plane (solid blockers at a contact); the plane, exact for planar receivers (in a perspective view its depth is affine in the
// image), is kept from half the receiver's depth to its own. Under a texel of
// kernel the sun's four-tap tent filters, from one to two texels the wide filter takes over: ShadowFilterQuality taps of a
// fixed lattice shifted per pixel by the blue-noise tile (fixed noise: a still view is the same in every frame), whose grain
// the opaque surfaces' mask blur takes (Effects/AreaScreenShadows.frag). Where the light's center does not see the receiver,
// each tap also looks along its path for points inside solids, between the view's front faces and its back faces
// (u_LocalShadowBackAtlas): one depth per texel cannot tell a solid's hidden lower part from the space under an overhang. The
// kernel is clamped to RenderData::c_AreaShadowReach of the tile, which the views' guard band holds, so penumbrae cross the
// cube's seams without a step. For a one-sided light, taps whose ray from the center lies behind the light's plane (no path from
// the face meets what they see) and cells wholly behind it are left out, and taps standing for points of the face behind the
// receiver's plane always (the shading's horizon). A receiver whose plane has the light's center behind it is not seen by the
// views: its taps march their paths instead (EvaluateAreaShadowBeyondCenter).

#ifndef ST_LOCAL_SHADOW_SAMPLING
#define ST_LOCAL_SHADOW_SAMPLING
#endif
#include "Include/LocalShadows.slang"
#include "Include/AreaLights.slang"
#include "Include/BlueNoise.slang"

// Levels of cells of 4 << level atlas texels each way: the nearest depth (toward the light, the largest) rounded up in the
// high 16 bits and the farthest rounded down in the low 16 bits, for the tiles of rect lights' views.
Texture2D<uint4> u_LocalShadowRanges : register(t58);
// The rect lights' views drawn again with the winding reversed and no depth bias (SceneRenderer::EnsureLocalShadowBackAtlas):
// per texel the nearest back face, where the solid the view sees first ends (0, the far plane, where it sees none), in the
// atlas's tiles.
Texture2D<float4> u_LocalShadowBackAtlas : register(t60);

static const float c_AreaShadowReach = 0.125;          // RenderData::c_AreaShadowReach
static const float c_LocalShadowRangeMinTileShare = 8.0; // RenderData::c_LocalShadowRangeMinTileShare
static const float c_AreaShadowTentTexels = 1.5;        // The tent's support beyond the receiver's texel
static const float c_InverseGoldenRatio = 0.61803398875;

// What the filter found at a receiver.
struct AreaShadowSample
{
	float Visibility;    // 1 = fully lit (also outside the views), faded toward lit by the light's strength
	float PenumbraWorld; // The kernel's half extent at the receiver, in world units (0 where the ranges classified it)
	float DiskWeight;    // How much of the result came from the wide filter (0: the tent alone)
	bool Compared;       // Whether it compared depths (false where the depth ranges classified it lit or in umbra)
};

// What the receiver's view says of the filter: where the receiver lies in its tile, its plane, the light's kernel per unit of
// the blockers' spread, and the view's depth terms (AreaShadowView, EvaluateAreaLightShadow).
struct AreaShadowView
{
	float2 Texel;         // The receiver's lookup in atlas texels
	float2 TileMin;       // Its tile's first texel and the texel past its last
	float2 TileMax;
	float TileTexels;
	float Reference;      // The receiver plane's NDC depth at the lookup, `Gradient` per texel, clamped to `Low` .. `High`
	float2 Gradient;
	float Low;
	float High;
	float Unbiased;       // The receiver's own NDC depth (without the biases), and how much nearer the biases put it
	float TieMargin;
	float ReceiverDepth;  // Its depth along the view's axis
	float DepthA;         // A depth along the axis for an NDC depth d: DepthB / (d + DepthA)
	float DepthB;
	float2 ColumnX;       // The kernel's columns (atlas texels) per unit of the blockers' spread (z_r - z_b) / (z_r z_b)
	float2 ColumnY;
	float4 Inverse;       // The columns' inverse (spread units per texel of offset: x, y the first row, z, w the second)
	bool HasPyramid;      // The columns span the image (not a face seen edge-on): the taps' paths are known
	float Reach;          // The columns' longest (c_AreaShadowReach of the tile)
	bool OneSided;        // Leave out what lies behind the light's plane: a ray's side of it is Front + dot(FrontGradient, texel -
	float Front;          // TileMin), positive in front
	float2 FrontGradient;
	float HorizonCenter;  // The receiver's horizon: the light's point at `lattice` on its face lies in front of the receiver's
	float2 HorizonGradient; // plane where HorizonCenter + dot(HorizonGradient, lattice) > 0
};

// Where a tap looks along its path besides the kernel's depth, as fractions of its offset from the receiver (EvaluateAreaLightShadow's
// taps): the middle look finds the paths that enter a solid blocker between the two (the near face of S1's wall, under its top);
// the near look, compared with the receiver's plane, the solid blockers just above the receiver, as at a sphere's or a post's
// foot. In stage 3's filter (the plane alone at the near look) a third gave the Cornell box's floor within 0.124 of the reference
// outside the penumbrae and 0.199 inside; a half 0.216 and 0.312, and a ninth more (with every look on the path) closed the light
// under the sphere's foot but darkened its penumbra by 0.24.
static const float c_AreaShadowMiddleTap = 2.0 / 3.0;
static const float c_AreaShadowNearTap = 1.0 / 3.0;
// The looks inside solids (GetAreaShadowInside): this many, the first at this fraction of a tap's offset, each next at 1 / sqrt(2)
// of the one before, down to a forty-fifth. With six from a sixth halving the box's foot in the Cornell box still leaked half the
// light at its corner (paths cross the box's corner in 3% to 8% of their offset); six from a quarter at this ratio left the
// strafe's flicker above the sun's (0.90% against 0.84% of blocks at the 99th percentile), at 1 ms less in
// PerfGPU.Renderer.AreaShadows4.
static const int c_AreaShadowInsideLooks = 10;
static const float c_AreaShadowInsideFirst = 0.5;
static const float c_AreaShadowInsideRatio = 0.70710678;

// Beyond the center, the looks along each tap's path from the light toward the receiver (EvaluateAreaShadowBeyondCenter): this
// many, the first at the tap's whole offset, each next at this fraction of the one before, down to a thirty-second of it.
static const int c_AreaShadowBeyondLooks = 11;
static const float c_AreaShadowBeyondRatio = 0.70710678;

// Whether the light's point at `lattice` on its face lies in front of the receiver's plane: the shading counts no light from
// behind it (AreaLights' horizon), so a tap standing for such a point is left out of the shadow too.
bool IsAreaShadowTapAboveHorizon(AreaShadowView view, float2 lattice)
{
	return view.HorizonCenter + dot(view.HorizonGradient, lattice) > 0.0;
}

// The receiver's plane's NDC depth at `offset` atlas texels from its lookup, kept from half the receiver's depth to its own:
// what lies beyond the receiver shadows nothing.
float GetAreaShadowPlaneReference(AreaShadowView view, float2 offset)
{
	return clamp(view.Reference + dot(view.Gradient, offset), view.Low, view.High);
}

// The spread (z_r - z) / (z_r z) at which the path from the light's point at `lattice` on its face to the receiver crosses the view
// at `offset` atlas texels from the receiver's lookup: the offset in the kernel's own units over the lattice's reach (-1 where
// the paths are not known: a face seen edge-on, or the lattice's center).
float GetAreaShadowTapSpread(AreaShadowView view, float2 offset, float2 lattice)
{
	float reach = max(abs(lattice.x), abs(lattice.y));
	if (!(view.HasPyramid && reach > 1e-3))
		return -1.0;
	float2 spread = float2(dot(view.Inverse.xy, offset), dot(view.Inverse.zw, offset));
	return max(abs(spread.x), abs(spread.y)) / reach;
}

// The NDC depth of the path from the light's point at `lattice` on its face to the receiver where it crosses the view at `offset`
// atlas texels from the receiver's lookup, made a little deeper by the receiver's biases scaled to that depth: an occluder seen
// there blocks the path if it is nearer. The path crosses there at the depth whose spread (z_r - z) / (z_r z) is the offset in the
// kernel's own units over the lattice's reach; NDC depths DepthB / z - DepthA are affine in 1 / z, so the path's is the receiver's
// plus DepthB times the spread. The margin puts a blocker's face that the path crosses at the kernel's own depth (the top of S1's
// wall) on the near side of the reference, as the near look's test of depth below needs: without it the biases kept the path
// nearer than the face, and S1 read 0.18 off the reference beside its umbra. An occluder
// beside every path, such as a wall rising beside a floor point, is farther than the path where a tap sees it and blocks nothing;
// compared with the receiver's plane alone (the floor's, extended under the wall) it darkened a band along every wall of the
// Cornell box (0.40 of the light at its worst).
float GetAreaShadowPathReference(AreaShadowView view, float2 offset, float2 lattice)
{
	float pathSpread = GetAreaShadowTapSpread(view, offset, lattice);
	if (!(pathSpread >= 0.0))
		return view.Low;
	return view.Unbiased + view.DepthB * pathSpread - view.TieMargin * (1.0 + view.ReceiverDepth * pathSpread);
}

// The bilinear weights of a gather's four texels at `texel` (in the gathered texture's texels), in the gather's order: x (0, 1),
// y (1, 1), z (1, 0), w (0, 0) of the footprint.
float4 GetAreaShadowGatherWeights(float2 texel)
{
	float2 fraction = frac(texel - 0.5);
	return float4((1.0 - fraction.x) * fraction.y, fraction.x * fraction.y, fraction.x * (1.0 - fraction.y), (1.0 - fraction.x) * (1.0 - fraction.y));
}

// How much of the bilinear footprint at `texel` (atlas texels) holds the point of a path at the NDC depth `depth` inside a solid,
// texel by texel: the view sees a front face nearer than the point there (as the atlas holds it, a little farther by its depth
// bias) and the back face behind it farther than the point by `margin` (the receiver's biases at that depth: a receiver the view
// sees from behind is its own back face). Where the view sees no back face (an open surface) nothing is known to be solid: none.
float GetAreaShadowInside(AreaShadowView view, float2 texel, float atlasSize, float depth, float margin)
{
	float4 front = u_LocalShadowAtlas.GatherRed(u_LinearClampSampler, texel / atlasSize);
	float4 nearer = step(float4(depth), front);
	if (!any(nearer > 0.0))
		return 0.0;
	float4 back = u_LocalShadowBackAtlas.GatherRed(u_LinearClampSampler, texel / atlasSize);
	float4 beyond = (1.0 - step(float4(depth - margin), back)) * (1.0 - step(back, float4(0.0)));
	return dot(GetAreaShadowGatherWeights(texel), nearer * beyond);
}

// The comparison sampler's test (stored nearer to the light than the reference) over the bilinear footprint at `uv`, for two
// references at once from one gather of the stored depths.
float2 CompareAreaShadowTwice(float2 uv, float2 atlasSize, float first, float second)
{
	float4 stored = u_LocalShadowAtlas.GatherRed(u_LinearClampSampler, uv);
	float2 fraction = frac(uv * atlasSize - 0.5);
	// The gather's order: x (0, 1), y (1, 1), z (1, 0), w (0, 0) of the footprint.
	float4 weights = float4((1.0 - fraction.x) * fraction.y, fraction.x * fraction.y, fraction.x * (1.0 - fraction.y), (1.0 - fraction.x) * (1.0 - fraction.y));
	return float2(dot(weights, 1.0 - step(stored, float4(first))), dot(weights, 1.0 - step(stored, float4(second))));
}

// A kernel column clamped to the reach.
float2 ClampAreaShadowColumn(float2 column, float reach)
{
	float columnLength = length(column);
	return columnLength > reach ? column * (reach / columnLength) : column;
}

// The kernel's half extents each way (atlas texels) for blockers at the NDC depth `depth`, and the tent's support a texel beyond.
float2 GetAreaShadowExtent(AreaShadowView view, float depth)
{
	float blockerDepth = view.DepthB / max(depth + view.DepthA, 1e-12);
	float spread = max(view.ReceiverDepth - blockerDepth, 0.0) / max(view.ReceiverDepth * blockerDepth, 1e-12);
	return abs(ClampAreaShadowColumn(view.ColumnX * spread, view.Reach)) + abs(ClampAreaShadowColumn(view.ColumnY * spread, view.Reach)) + c_AreaShadowTentTexels
		+ 1.0;
}

// What a classification of the cells around a receiver found.
struct AreaShadowCells
{
	// Of the cells that hold a blocker whose penumbra reaches the receiver: their nearest depth (NDC, the largest; 0 without
	// one), and the overlap-weighted mean of the nearest depths of those nearer than the receiver's plane over them.
	float Nearest;
	float BlockerDepth;
	float BlockerWeight;
	// Lit: every cell's nearest depth is no nearer than the least reference a comparison reading it takes (the receiver's plane
	// over the cell, and a texel of it more for the bilinear footprints that straddle two cells), or its blockers lie too far
	// from the receiver in the image for their penumbra (the kernel for the cell's nearest depth) to reach it. A planar receiver
	// is lit across its own cells however steep it lies, and geometry that cannot stand between the light and the receiver (the
	// floor beside a wall's foot) does not shadow it. Umbra: every cell's farthest depth is nearer than the greatest reference.
	bool Lit;
	bool Umbra;
};

// The cells of `level` within `region` atlas texels of the receiver's lookup (inside its tile), leaving out, for a one-sided
// light, the cells whose corners' rays all lie behind its plane.
AreaShadowCells ReadAreaShadowCells(AreaShadowView view, float2 region, int level)
{
	AreaShadowCells cells;
	cells.Nearest = 0.0;
	cells.BlockerDepth = 0.0;
	cells.BlockerWeight = 0.0;
	cells.Lit = true;
	cells.Umbra = false;
	bool anyCell = false;
	bool allUmbra = true;
	float cellSize = float(4 << level);
	int2 firstTileCell = int2(view.TileMin / cellSize);
	int2 lastTileCell = int2((view.TileMax - 1.0) / cellSize);
	int2 firstCell = clamp(int2(floor((view.Texel - region) / cellSize)), firstTileCell, lastTileCell);
	int2 lastCell = clamp(int2(floor((view.Texel + region) / cellSize)), firstTileCell, lastTileCell);
	float cellSpread = (abs(view.Gradient.x) + abs(view.Gradient.y)) * cellSize * 0.5;
	float texelSpread = abs(view.Gradient.x) + abs(view.Gradient.y);
	for (int cellY = firstCell.y; cellY <= lastCell.y; cellY++)
	{
		for (int cellX = firstCell.x; cellX <= lastCell.x; cellX++)
		{
			float2 cellMin = float2(cellX, cellY) * cellSize;
			if (view.OneSided)
			{
				// The rays' side of the light's plane is affine in the texel: the cell is behind it where its four corners are.
				float corner = view.Front + dot(view.FrontGradient, cellMin - view.TileMin);
				float largest = corner + max(view.FrontGradient.x * cellSize, 0.0) + max(view.FrontGradient.y * cellSize, 0.0);
				if (!(largest > 0.0))
					continue;
			}
			uint packed = u_LocalShadowRanges.Load(int3(cellX, cellY, level)).r;
			float2 range = float2(float(packed >> 16u), float(packed & 0xFFFFu)) / 65535.0;
			anyCell = true;
			float centerReference = view.Reference + dot(view.Gradient, cellMin + 0.5 * cellSize - view.Texel);
			allUmbra = allUmbra && range.y > clamp(centerReference + cellSpread + texelSpread, view.Low, view.High);
			if (range.x <= clamp(centerReference - cellSpread - texelSpread, view.Low, view.High))
				continue; // Lit over the whole cell
			// The cell's nearest blocker gives its widest penumbra: the cell matters only where that reaches the receiver.
			float2 extent = GetAreaShadowExtent(view, range.x);
			if (any((cellMin > view.Texel + extent)) || any((cellMin + cellSize < view.Texel - extent)))
				continue;
			cells.Lit = false;
			cells.Nearest = max(cells.Nearest, range.x);
			// A cell blocks where its nearest depth is nearer than the receiver's plane anywhere over it; its weight is its overlap
			// with its blockers' reach around the receiver, which changes continuously as the receiver moves (a blocker the receiver
			// nears enters the mean from nothing).
			float cellReference = clamp(centerReference, view.Low, view.High) + cellSpread;
			if (range.x > cellReference)
			{
				float2 overlap = clamp(min(cellMin + cellSize, view.Texel + extent) - max(cellMin, view.Texel - extent), 0.0, cellSize) / cellSize;
				float weight = overlap.x * overlap.y;
				cells.BlockerDepth += weight * range.x;
				cells.BlockerWeight += weight;
			}
		}
	}
	if (cells.BlockerWeight > 0.0)
		cells.BlockerDepth /= cells.BlockerWeight;
	cells.Umbra = anyCell && allUmbra;
	return cells;
}

// The level of cells that holds a region of `region` texels each way within 5 x 5 of them (a half extent of at most two cells),
// no coarser than an eighth of the tile (cells never hold two tiles).
int GetAreaShadowLevel(float2 region, float tileTexels)
{
	float extent = max(region.x, region.y);
	int level = 0;
	while (float(4 << level) * 2.0 < extent && float(8 << level) * c_LocalShadowRangeMinTileShare <= tileTexels)
		level++;
	return level;
}

// A receiver whose plane has the light's center behind it (the side of a post under a strip light that sees the strip's far
// part): the views, drawn from the center, do not see it, and what they show nearer than its plane lies behind it, so neither
// the depth ranges' classification nor the comparisons with the plane tell anything (both read the receiver's own solid as a
// blocker: a false umbra with blotches of light). Only the face's part in front of the plane lights it, along paths that stay
// in front of the plane. Each of that part's taps marches its path from the light toward the receiver: at a look, a surface
// the view shows nearer than the path's point but beyond the plane (in front of the receiver) blocks it. The kernel is the
// whole reach along the light's axes, so a tap's offset lies on its path at every look. A blocker the receiver's own solid
// hides from the center is not found.
AreaShadowSample EvaluateAreaShadowBeyondCenter(AreaShadowView view, int2 pixel, float filterTaps, float strength, float texelWorld, float atlasSize)
{
	AreaShadowSample result;
	result.Visibility = 1.0;
	result.PenumbraWorld = 0.0;
	result.DiskWeight = 0.0;
	result.Compared = false;
	// No part of the face in front of the plane (the shading has none of its light), or the face seen edge-on: lit.
	if (!(view.HorizonCenter + abs(view.HorizonGradient.x) + abs(view.HorizonGradient.y) > 0.0) || !view.HasPyramid)
		return result;
	float scale = view.Reach / max(length(view.ColumnX) + length(view.ColumnY), 1e-12);
	float2 lowest = view.TileMin + 0.5;
	float2 highest = view.TileMax - 0.5;
	float2 atlasTexel = 1.0 / float2(atlasSize);
	int taps = max(int(filterTaps + 0.5), 1);
	float2 shift = float2(BlueNoise(pixel, int2(17, 43)), BlueNoise(pixel, int2(43, 17)));
	float shadowed = 0.0;
	float counted = 0.0;
	for (int tap = 0; tap < taps; tap++)
	{
		float2 lattice = frac(float2((float(tap) + 0.5) / float(taps), (float(tap) + 0.5) * c_InverseGoldenRatio) + shift) * 2.0 - 1.0;
		if (!IsAreaShadowTapAboveHorizon(view, lattice))
			continue;
		float2 offset = (view.ColumnX * lattice.x + view.ColumnY * lattice.y) * scale;
		float blocked = 0.0;
		float fraction = 1.0;
		for (int look = 0; look < c_AreaShadowBeyondLooks && blocked < 1.0; look++)
		{
			float2 lookOffset = clamp(view.Texel + offset * fraction, lowest, highest) - view.Texel;
			float spread = scale * fraction;
			float margin = view.TieMargin * (1.0 + view.ReceiverDepth * spread);
			float pathReference = view.Unbiased + view.DepthB * spread - margin;
			float planeReference = view.Reference + dot(view.Gradient, lookOffset) - margin;
			// Stored nearer than the path's point, and not nearer than the plane (which lies behind the receiver here).
			float2 look = CompareAreaShadowTwice((view.Texel + lookOffset) * atlasTexel, float2(atlasSize, atlasSize), pathReference, planeReference);
			blocked = max(blocked, look.x - look.y);
			fraction *= c_AreaShadowBeyondRatio;
		}
		shadowed += blocked;
		counted += 1.0;
	}
	if (!(counted > 0.0))
		return result;
	result.Visibility = lerp(1.0, 1.0 - shadowed / counted, strength);
	result.PenumbraWorld = view.Reach * texelWorld;
	result.DiskWeight = 1.0;
	result.Compared = true;
	return result;
}

// A rect light's shadow at a camera-relative position with its geometric normal (facing the viewer), at a screen pixel (which
// picks the lattice's fixed shift), from the shadow slot `slot` (its LocalShadowData) and the light as the shading takes it.
AreaShadowSample EvaluateAreaLightShadow(uint slot, float3 relativePosition, float3 normal, int2 pixel, AreaLight light)
{
	AreaShadowSample result;
	result.Visibility = 1.0;
	result.PenumbraWorld = 0.0;
	result.DiskWeight = 0.0;
	result.Compared = false;
	float4 parameters = u_LocalShadows[slot].Parameters;
	float strength = clamp(parameters.w, 0.0, 1.0);
	if (!(strength > 0.0))
		return result;
	uint view = SelectLocalShadowView(slot, relativePosition);
	float4x4 viewProjection = u_LocalShadows[slot].ViewProjection[view];
	float4 tile = u_LocalShadows[slot].Tiles[view];
	float3 lightPosition = u_LocalShadows[slot].Light.xyz;
	float3 toLight = lightPosition - relativePosition;
	float lightDistance = length(toLight);
	if (!(lightDistance > 0.0))
		return result;
	// The biases as SampleLocalShadow's: the normal bias off the surface, the depth bias toward the light, in texels of the tile at
	// the receiver's depth.
	float texelWorld = max((mul(viewProjection, float4(relativePosition, 1.0))).w, 0.0) * tile.w;
	float3 offsetPosition = relativePosition + normal * (parameters.y * texelWorld) + toLight * (parameters.x * texelWorld / lightDistance);
	float4 clip = mul(viewProjection, float4(offsetPosition, 1.0));
	if (!(clip.w > 0.0))
		return result;
	float3 ndc = clip.xyz / clip.w;
	float2 tileUV = NDCToUV(ndc.xy);
	if (any((tileUV < float2(0.0))) || any((tileUV > float2(1.0))))
		return result;
	float atlasSize = float(TextureSize(u_LocalShadowAtlas, 0).x);
	AreaShadowView shadowView;
	shadowView.TileTexels = tile.z * atlasSize;
	shadowView.TileMin = tile.xy * atlasSize;
	shadowView.TileMax = shadowView.TileMin + shadowView.TileTexels;
	shadowView.Texel = shadowView.TileMin + tileUV * shadowView.TileTexels;
	shadowView.ReceiverDepth = clip.w;

	// The view's terms: its depth along the axis for an NDC depth d is DepthB / (d + DepthA); a world direction of unit depth
	// through NDC (x, y) is forward + rowX x + rowY y.
	float3 forward = viewProjection[3].xyz;
	shadowView.DepthA = -dot(viewProjection[2].xyz, forward);
	shadowView.DepthB = dot(viewProjection[2], float4(lightPosition, 1.0));
	float3 rowX = viewProjection[0].xyz / max(dot(viewProjection[0].xyz, viewProjection[0].xyz), 1e-12);
	float3 rowY = viewProjection[1].xyz / max(dot(viewProjection[1].xyz, viewProjection[1].xyz), 1e-12);
	// NDC per atlas texel (v runs down the tile, NDC y up).
	float2 ndcPerTexel = float2(2.0, -2.0) / shadowView.TileTexels;

	// The receiver's plane (through the offset position, with its normal): its NDC depth is affine in the image,
	// DepthB * dot(normal, ray) / dot(normal, offset - light) - DepthA, compared from half the receiver's depth to the receiver's
	// own: what lies beyond the receiver along a ray shadows nothing (no path from the light to the receiver reaches it), so the
	// plane receding beyond it, or never met (rays of a receiver the light grazes), takes the receiver's depth. A curved receiver
	// near where the light grazes it then never shadows itself.
	float planeOffset = dot(normal, offsetPosition - lightPosition);
	shadowView.Reference = ndc.z;
	shadowView.Gradient = float2(0.0);
	if (abs(planeOffset) > 1e-6 * shadowView.ReceiverDepth)
		shadowView.Gradient = shadowView.DepthB * float2(dot(normal, rowX), dot(normal, rowY)) * ndcPerTexel / planeOffset;
	shadowView.High = shadowView.DepthB / (0.5 * shadowView.ReceiverDepth) - shadowView.DepthA;
	shadowView.Low = ndc.z;
	float4 unbiasedClip = mul(viewProjection, float4(relativePosition, 1.0));
	shadowView.Unbiased = unbiasedClip.z / max(unbiasedClip.w, 1e-12);
	shadowView.TieMargin = max(ndc.z - shadowView.Unbiased, 0.0);

	// A one-sided light: a ray's side of its plane, affine in the texel (positive in front), from the tile's corner, NDC (-1, 1).
	shadowView.OneSided = !light.TwoSided;
	shadowView.FrontGradient = float2(dot(rowX, light.Normal), dot(rowY, light.Normal)) * ndcPerTexel;
	shadowView.Front = dot(forward, light.Normal) - (shadowView.FrontGradient.x + shadowView.FrontGradient.y) * shadowView.TileTexels * 0.5;

	// The kernel's columns per unit of the blockers' spread: the light's half axes in the view through the receiver's ray.
	float4 axisX = mul(viewProjection, float4(light.AxisX, 0.0));
	float4 axisY = mul(viewProjection, float4(light.AxisY, 0.0));
	shadowView.ColumnX = (axisX.xy - ndc.xy * axisX.w) * float2(0.5, -0.5) * shadowView.TileTexels;
	shadowView.ColumnY = (axisY.xy - ndc.xy * axisY.w) * float2(0.5, -0.5) * shadowView.TileTexels;
	float determinant = shadowView.ColumnX.x * shadowView.ColumnY.y - shadowView.ColumnY.x * shadowView.ColumnX.y;
	float columnScale = max(dot(shadowView.ColumnX, shadowView.ColumnX), dot(shadowView.ColumnY, shadowView.ColumnY));
	shadowView.HasPyramid = abs(determinant) > 1e-6 * columnScale;
	shadowView.Inverse = shadowView.HasPyramid
		? float4(shadowView.ColumnY.y, -shadowView.ColumnY.x, -shadowView.ColumnX.y, shadowView.ColumnX.x) / determinant : float4(0.0);
	shadowView.Reach = c_AreaShadowReach * shadowView.TileTexels;
	shadowView.HorizonCenter = dot(normal, light.Center - relativePosition);
	shadowView.HorizonGradient = float2(dot(normal, light.AxisX), dot(normal, light.AxisY));
	if (planeOffset > 1e-6 * shadowView.ReceiverDepth)
		return EvaluateAreaShadowBeyondCenter(shadowView, pixel, parameters.z, strength, texelWorld, atlasSize);

	// The search: blockers anywhere from the view's near plane to the receiver, within the reach; then, as long as it lets a finer
	// level hold the region, again over the region the nearest blocker that reaches the receiver can shadow (at a contact the
	// cells there, so the blockers' depth is the contact's).
	float2 region = GetAreaShadowExtent(shadowView, 1.0);
	int level = GetAreaShadowLevel(region, shadowView.TileTexels);
	AreaShadowCells cells = ReadAreaShadowCells(shadowView, region, level);
	for (int pass = 0; pass < 3 && !cells.Lit && !cells.Umbra; pass++)
	{
		float2 finer = GetAreaShadowExtent(shadowView, cells.Nearest);
		int finerLevel = GetAreaShadowLevel(finer, shadowView.TileTexels);
		if (finerLevel >= level)
			break;
		region = finer;
		level = finerLevel;
		cells = ReadAreaShadowCells(shadowView, region, level);
	}
	if (cells.Lit)
		return result;
	if (cells.Umbra)
	{
		result.Visibility = 1.0 - strength;
		return result;
	}
	result.Compared = true;

	// The kernel from the blockers' mean depth (no blocking cell: no penumbra, the tent alone).
	float2 kernelX = float2(0.0);
	float2 kernelY = float2(0.0);
	if (cells.BlockerWeight > 0.0)
	{
		float blockerDepth = shadowView.DepthB / max(cells.BlockerDepth + shadowView.DepthA, 1e-12);
		float blockerSpread = max(shadowView.ReceiverDepth - blockerDepth, 0.0) / max(shadowView.ReceiverDepth * blockerDepth, 1e-12);
		kernelX = ClampAreaShadowColumn(shadowView.ColumnX * blockerSpread, shadowView.Reach);
		kernelY = ClampAreaShadowColumn(shadowView.ColumnY * blockerSpread, shadowView.Reach);
	}
	float penumbraTexels = max(length(kernelX), length(kernelY));

	float2 texel = shadowView.Texel;
	float2 atlasTexel = 1.0 / float2(atlasSize);
	float2 lowest = shadowView.TileMin + 0.5;
	float2 highest = shadowView.TileMax - 0.5;
	// The comparison sampler tests reference < stored: true where a caster is nearer to the light (reversed-Z).
	float tent = 0.0;
	for (int tap = 0; tap < 4; tap++)
	{
		float2 offset = float2((tap & 1) == 0 ? -0.5 : 0.5, (tap & 2) == 0 ? -0.5 : 0.5);
		float tapReference = GetAreaShadowPlaneReference(shadowView, offset);
		tent += 0.25 * u_LocalShadowAtlas.SampleCmpLevelZero(u_ShadowCompareSampler, clamp(texel + offset, lowest, highest) * atlasTexel, tapReference);
	}
	float shadowed = tent;
	float diskWeight = clamp(penumbraTexels - 1.0, 0.0, 1.0);
	if (diskWeight > 0.0)
	{
		// The light's face seen through the receiver: a lattice of the quality's taps over [-1, 1]^2 through the kernel's
		// columns, shifted per pixel by the blue-noise tile (two far shifts of its one channel).
		int taps = max(int(parameters.z + 0.5), 1);
		float2 shift = float2(BlueNoise(pixel, int2(17, 43)), BlueNoise(pixel, int2(43, 17)));
		float disk = 0.0;
		float counted = 0.0;
		bool insideLooks = tent > 0.0;
		for (int tap = 0; tap < taps; tap++)
		{
			float2 lattice = frac(float2((float(tap) + 0.5) / float(taps), (float(tap) + 0.5) * c_InverseGoldenRatio) + shift) * 2.0 - 1.0;
			// A point of the face behind the receiver's plane gives it no light (the shading's horizon): its tap, whose path lies
			// behind the plane, would weigh in whatever its looks read there.
			if (!IsAreaShadowTapAboveHorizon(shadowView, lattice))
				continue;
			float2 offset = kernelX * lattice.x + kernelY * lattice.y;
			float2 tapTexel = clamp(texel + offset, lowest, highest);
			if (shadowView.OneSided && !(shadowView.Front + dot(shadowView.FrontGradient, tapTexel - shadowView.TileMin) > 0.0))
				continue;
			// Three looks along the tap's path, from the light toward the receiver (measured in GPU.SceneRenderer.AreaShadows).
			// At the kernel's depth: an occluder nearer than the path there blocks it. At two thirds of the offset: an occluder
			// nearer than the path blocks it where the surface the first look sees is nearer than the path here too (the path
			// entered a solid whose face continues between the looks, as the near face of S1's wall under its top). Without that
			// condition an occluder the path only passed behind blocks it (P hidden by the wall's top from the light's center, the
			// light's point beyond the top): S1 read 0.10 off and the strafe's flicker rose to 1.17% of blocks at the 99th
			// percentile against the sun's 0.84%; without the look the wall's umbra edge read 0.21 lit. At a third: an occluder
			// nearer than the receiver's plane blocks it where it also lies between some point of the face and the receiver (the
			// face's reach: the depth of the path from the face's farthest point, the lattice's corner, through the look), and is
			// deeper than the path at the kernel's depth (the first look could not see it). Stage 3's near look, the plane alone,
			// counted the blocker the kernel was made from as solid down to the receiver (S1 0.19 off, dark beside its umbra; the
			// 6 cm post's shadow 1 m on 0.29 dark) and a wall rising beside a receiver as a contact (the Cornell box's back wall
			// beside the red wall 0.19 dark, its floor's penumbrae 0.199 off where they are 0.129 with the face's reach).
			float2 tapOffset = tapTexel - texel;
			float pathReference = GetAreaShadowPathReference(shadowView, tapOffset, lattice);
			float2 middleOffset = tapOffset * c_AreaShadowMiddleTap;
			float middleReference = max(GetAreaShadowPlaneReference(shadowView, middleOffset), GetAreaShadowPathReference(shadowView, middleOffset, lattice));
			float2 pathLook = CompareAreaShadowTwice(tapTexel * atlasTexel, float2(atlasSize, atlasSize), max(GetAreaShadowPlaneReference(shadowView, tapOffset), pathReference),
				middleReference);
			float middleBlocked = pathLook.y
				* u_LocalShadowAtlas.SampleCmpLevelZero(u_ShadowCompareSampler, clamp(texel + middleOffset, lowest, highest) * atlasTexel, middleReference);
			float2 nearOffset = tapOffset * c_AreaShadowNearTap;
			float2 nearLook = CompareAreaShadowTwice(clamp(texel + nearOffset, lowest, highest) * atlasTexel, float2(atlasSize, atlasSize),
				max(GetAreaShadowPlaneReference(shadowView, nearOffset), GetAreaShadowPathReference(shadowView, nearOffset, float2(1.0, 1.0))), pathReference);
			float blocked = max(pathLook.x, max(middleBlocked, nearLook.x * (1.0 - nearLook.y)));
			// Looks inside solids, where the light's center does not see the receiver (elsewhere the looks above found what the
			// one depth per texel can show): from half the offset toward the receiver, the path blocked where its point lies
			// between a front face the view sees and the back face behind it, as at a box's foot where only the box's hidden lower
			// part stands between the receiver and the face and the looks above pass beside it. Without them the Cornell box's
			// floor beside its tall box read up to 0.64 of the light too bright (0.23% of its pixels more than 0.3 off the
			// reference; 0.03% with them).
			float tapSpread = GetAreaShadowTapSpread(shadowView, tapOffset, lattice);
			float lookSpread = tapSpread * c_AreaShadowInsideFirst;
			for (int look = 0; insideLooks && look < c_AreaShadowInsideLooks && blocked < 1.0 && tapSpread > 0.0; look++)
			{
				float2 lookTexel = clamp(texel + tapOffset * (lookSpread / tapSpread), lowest, highest);
				blocked = max(blocked, GetAreaShadowInside(shadowView, lookTexel, atlasSize, shadowView.Unbiased + shadowView.DepthB * lookSpread,
					shadowView.TieMargin * (1.0 + shadowView.ReceiverDepth * lookSpread)));
				lookSpread *= c_AreaShadowInsideRatio;
			}
			disk += blocked;
			counted += 1.0;
		}
		shadowed = lerp(tent, counted > 0.0 ? disk / counted : 0.0, diskWeight);
	}
	result.Visibility = lerp(1.0, 1.0 - shadowed, strength);
	result.PenumbraWorld = penumbraTexels * shadowView.ReceiverDepth * tile.w;
	result.DiskWeight = diskWeight;
	return result;
}

// Fraction of a shadowed rect light's light reaching a surface (1 = fully lit): EvaluateAreaLightShadow's visibility.
float SampleAreaLightShadow(uint slot, float3 relativePosition, float3 normal, float2 pixel, AreaLight light)
{
	return EvaluateAreaLightShadow(slot, relativePosition, normal, int2(pixel), light).Visibility;
}

#endif
