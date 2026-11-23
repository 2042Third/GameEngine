#ifndef STRATA_SPHERICAL_HARMONICS_SLANG
#define STRATA_SPHERICAL_HARMONICS_SLANG

// The real spherical harmonics of bands 0 to 2 of a unit direction, in the order reflection probes store their irradiance
// coefficients in (IBL/ProbeIrradiance.comp writes them, Include/ReflectionProbes.slang reads them).
void ProbeSHBasis(float3 n, out float basis[9])
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

// ProbeSHBasis with every product and difference rounded as written (`precise`: no product fused into the difference after it),
// for sums that must give the same bits from the same directions however a driver compiles them (GI/FieldReduce.comp). Its last
// bit may differ from ProbeSHBasis's, which is evaluated per pixel and leaves the compiler free: keep the two in step.
void ProbeSHBasisPrecise(float3 n, out float basis[9])
{
	precise float values[9];
	values[0] = 0.282095;
	values[1] = 0.488603 * n.y;
	values[2] = 0.488603 * n.z;
	values[3] = 0.488603 * n.x;
	values[4] = 1.092548 * n.x * n.y;
	values[5] = 1.092548 * n.y * n.z;
	values[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
	values[7] = 1.092548 * n.x * n.z;
	values[8] = 0.546274 * (n.x * n.x - n.y * n.y);
	basis = values;
}

#endif
