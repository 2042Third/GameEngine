#ifndef STRATA_ORDERED_FLOAT_SLANG
#define STRATA_ORDERED_FLOAT_SLANG

// Floats as uints that order like the floats, so atomicMax and atomicMin work on signed values. No float encodes to 0, which
// stands for "nothing written" in buffers cleared to zero.
uint EncodeOrderedFloat(float value)
{
	uint bits = asuint(value);
	return (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
}

float DecodeOrderedFloat(uint encoded)
{
	return asfloat((encoded & 0x80000000u) != 0u ? (encoded & 0x7FFFFFFFu) : ~encoded);
}

#endif
