#ifndef STRATA_AREA_SHADOWS_SLANG
#define STRATA_AREA_SHADOWS_SLANG

// Rect lights' soft shadows (DeveloperGuide "Area lights"): percentage-closer soft shadows from the light's six local shadow
// views at its face's center, filtered with the light's own size, as the sun's disk is with the sun's. Per receiver, in the
// cube face that sees it: the atlas's depth ranges (u_LocalShadowRanges, Scene/LocalShadowRanges.comp) classify it as lit or
// in umbra without a comparison, else give the blockers' mean depth, from which the light's face seen through the receiver
// is the kernel: the parallelogram of its two half axes projected into the view, scaled by (z_r - z_b) / (z_r z_b) (a light of
// half size s and a blocker at z_b give a penumbra half width s (z_r - z_b) / z_b at the receiver z_r), long along a strip
// light and narrow across it. Its taps compare with the receiver's plane at each tap (in a perspective view a plane's depth is
// affine in the image, so this is exact for planar receivers), from half the receiver's depth to its own. Under a texel of
// kernel the sun's four-tap tent filters, from one to two texels the wide filter takes over: ShadowFilterQuality taps of a
// fixed lattice shifted per pixel by the blue-noise tile (fixed noise: a still view is the same in every frame), whose grain
// the opaque surfaces' mask blur takes (Effects/AreaScreenShadows.frag). The kernel is clamped to RenderData::c_AreaShadowReach
// of the tile, which the views' guard band holds, so penumbrae cross the cube's seams without a step. For a one-sided light,
// taps whose ray from the center lies behind the light's plane (no path from the face meets what they see) and cells wholly
// behind it are left out.

#ifndef ST_LOCAL_SHADOW_SAMPLING
#define ST_LOCAL_SHADOW_SAMPLING
#endif
#include "Include/LocalShadows.slang"
#include "Include/AreaLights.slang"
#include "Include/BlueNoise.slang"

// Levels of cells of 4 << level atlas texels each way: the nearest depth (toward the light, the largest) rounded up in the
// high 16 bits and the farthest rounded down in the low 16 bits, for the tiles of rect lights' views.
Texture2D<uint4> u_LocalShadowRanges : register(t57);

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
	float ReceiverDepth;  // Its depth along the view's axis
	float DepthA;         // A depth along the axis for an NDC depth d: DepthB / (d + DepthA)
	float DepthB;
	float2 ColumnX;       // The kernel's columns (atlas texels) per unit of the blockers' spread (z_r - z_b) / (z_r z_b)
	float2 ColumnY;
	float Reach;          // The columns' longest (c_AreaShadowReach of the tile)
	bool OneSided;        // Leave out what lies behind the light's plane: a ray's side of it is Front + dot(FrontGradient, texel -
	float Front;          // TileMin), positive in front
	float2 FrontGradient;
};

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

	// A one-sided light: a ray's side of its plane, affine in the texel (positive in front), from the tile's corner, NDC (-1, 1).
	shadowView.OneSided = !light.TwoSided;
	shadowView.FrontGradient = float2(dot(rowX, light.Normal), dot(rowY, light.Normal)) * ndcPerTexel;
	shadowView.Front = dot(forward, light.Normal) - (shadowView.FrontGradient.x + shadowView.FrontGradient.y) * shadowView.TileTexels * 0.5;

	// The kernel's columns per unit of the blockers' spread: the light's half axes in the view through the receiver's ray.
	float4 axisX = mul(viewProjection, float4(light.AxisX, 0.0));
	float4 axisY = mul(viewProjection, float4(light.AxisY, 0.0));
	shadowView.ColumnX = (axisX.xy - ndc.xy * axisX.w) * float2(0.5, -0.5) * shadowView.TileTexels;
	shadowView.ColumnY = (axisY.xy - ndc.xy * axisY.w) * float2(0.5, -0.5) * shadowView.TileTexels;
	shadowView.Reach = c_AreaShadowReach * shadowView.TileTexels;

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
	float2 gradient = shadowView.Gradient;
	float reference = shadowView.Reference;
	float2 atlasTexel = 1.0 / float2(atlasSize);
	float2 lowest = shadowView.TileMin + 0.5;
	float2 highest = shadowView.TileMax - 0.5;
	// The comparison sampler tests reference < stored: true where a caster is nearer to the light (reversed-Z).
	float tent = 0.0;
	for (int tap = 0; tap < 4; tap++)
	{
		float2 offset = float2((tap & 1) == 0 ? -0.5 : 0.5, (tap & 2) == 0 ? -0.5 : 0.5);
		float tapReference = clamp(reference + dot(gradient, offset), shadowView.Low, shadowView.High);
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
		for (int tap = 0; tap < taps; tap++)
		{
			float2 lattice = frac(float2((float(tap) + 0.5) / float(taps), (float(tap) + 0.5) * c_InverseGoldenRatio) + shift) * 2.0 - 1.0;
			float2 offset = kernelX * lattice.x + kernelY * lattice.y;
			float2 tapTexel = clamp(texel + offset, lowest, highest);
			if (shadowView.OneSided && !(shadowView.Front + dot(shadowView.FrontGradient, tapTexel - shadowView.TileMin) > 0.0))
				continue;
			float tapReference = clamp(reference + dot(gradient, tapTexel - texel), shadowView.Low, shadowView.High);
			disk += u_LocalShadowAtlas.SampleCmpLevelZero(u_ShadowCompareSampler, tapTexel * atlasTexel, tapReference);
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
