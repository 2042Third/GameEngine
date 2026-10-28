#ifndef STRATA_LIGHTING_VIEW_GLSL
#define STRATA_LIGHTING_VIEW_GLSL

#include "Include/Scene.glsl"
#include "Include/Exposure.glsl"

// The lighting debug views' overlay (Overlay/LightingView.frag) and statistics (Effects/LightingViewStats.comp). Keep in sync
// with SceneRenderData.h (LightingViewParameters, LightingViewStatsData and the c_LightingView* constants).

#define ST_LIGHTING_VIEW_EV_BINS      128u
#define ST_LIGHTING_VIEW_MIN_EV100    -8.0
#define ST_LIGHTING_VIEW_BINS_PER_EV  4.0
#define ST_LIGHTING_VIEW_VALUE_BINS   64u
#define ST_LIGHTING_VIEW_LUMINANCE_COLORS 12
#define ST_ALBEDO_LOW  0.03
#define ST_ALBEDO_HIGH 0.85

// Word offsets in the statistics buffer (LightingViewStatsData).
#define ST_LIGHTING_STATS_SURFACE_EV    0u
#define ST_LIGHTING_STATS_BACKGROUND_EV 128u
#define ST_LIGHTING_STATS_VALUE         256u
#define ST_LIGHTING_STATS_SURFACE_PIXELS    320u
#define ST_LIGHTING_STATS_BACKGROUND_PIXELS 321u
#define ST_LIGHTING_STATS_BELOW_RANGE       322u
#define ST_LIGHTING_STATS_ABOVE_RANGE       323u
#define ST_LIGHTING_STATS_SKY_IRRADIANCE    324u // Four words: rgb as float bits, one unused

layout(push_constant) uniform LightingViewParameters
{
	uint View;
	float InversePreExposure;
	float LuminanceFirstEV100;
	uint Padding0;
	uvec4 LuminanceColors[3]; // Twelve RGBA8 colors, four per vector
	uint AlbedoLow;
	uint AlbedoHigh;
	uint Background;
	uint Padding1;
} u_LightingView;

// The luminance of an HDR target's pixel as EV100 (log2 of nits plus 3): pre-exposed values are divided by the pre-exposure.
// Black reads as a very dark value rather than minus infinity.
float GetPixelEV100(vec3 hdr)
{
	return log2(max(Luminance(hdr) * u_LightingView.InversePreExposure, exp2(-10.0))) + 3.0;
}

// A packed RGBA8 color (red in the low byte) as sRGB values.
vec3 UnpackDisplayColor(uint packed)
{
	return unpackUnorm4x8(packed).rgb;
}

vec3 GetLuminanceBandColor(int band)
{
	uint packed = u_LightingView.LuminanceColors[band / 4][band % 4];
	return UnpackDisplayColor(packed);
}

#endif
