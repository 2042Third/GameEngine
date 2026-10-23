#ifndef STRATA_SPHERICAL_HARMONICS_GLSL
#define STRATA_SPHERICAL_HARMONICS_GLSL

// The real spherical harmonics of bands 0 to 2 of a unit direction, in the order reflection probes store their irradiance
// coefficients in (IBL/ProbeIrradiance.comp writes them, Include/ReflectionProbes.glsl reads them).
void ProbeSHBasis(vec3 n, out float basis[9])
{
	basis[0] = 0.282095;
	basis[1] = 0.488603 * n.y;
	basis[2] = 0.488603 * n.z;
	basis[3] = 0.488603 * n.x;
	basis[4] = 1.092548 * n.x * n.y;
	basis[5] = 1.092548 * n.y * n.z;
	basis[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
	basis[7] = 1.092548 * n.x * n.z;
	basis[8] = 0.546274 * (n.x * n.x - n.y * n.y);
}

#endif
