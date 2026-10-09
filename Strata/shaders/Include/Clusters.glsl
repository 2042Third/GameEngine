#ifndef STRATA_CLUSTERS_GLSL
#define STRATA_CLUSTERS_GLSL

// Clustered light assignment (see Scene/LightClusters.comp). The view frustum is split into ClusterGrid.x by
// ClusterGrid.y screen tiles and ClusterGrid.z depth slices, spaced exponentially in view depth. Each cluster owns
// 1 + ClusterGrid.w uints of the cluster buffer: its light count, then the indices of the lights that reach it in
// ascending order. The light buffer is sorted by relevance, so a full cluster keeps the most relevant lights.

#include "Include/Scene.glsl"

layout(set = 0, binding = ST_SRV(12), std430) readonly buffer ClusterBuffer
{
	uint u_ClusterLights[];
};

uint GetClusterSlice(float viewDepth)
{
	float slice = log(max(viewDepth, 1e-6)) * u_Frame.ClusterDepth.x + u_Frame.ClusterDepth.y;
	return uint(clamp(slice, 0.0, float(u_Frame.ClusterGrid.z - 1u)));
}

// Cluster containing a pixel (framebuffer coordinates) at a view depth.
uint GetClusterIndex(vec2 pixel, float viewDepth)
{
	uvec2 tile = min(uvec2(pixel * u_Frame.ViewportSize.zw * vec2(u_Frame.ClusterGrid.xy)), u_Frame.ClusterGrid.xy - 1u);
	return (GetClusterSlice(viewDepth) * u_Frame.ClusterGrid.y + tile.y) * u_Frame.ClusterGrid.x + tile.x;
}

uint GetClusterStride()
{
	return u_Frame.ClusterGrid.w + 1u;
}

// View depth where depth slice `slice` begins; slice ClusterGrid.z is where the last one ends.
float GetClusterSliceStart(uint slice)
{
	if (slice == 0u)
		return u_Frame.ClusterDepth.z;
	if (slice >= u_Frame.ClusterGrid.z)
		return u_Frame.ClusterDepth.w;
	return exp((float(slice) - u_Frame.ClusterDepth.y) / u_Frame.ClusterDepth.x);
}

#endif
