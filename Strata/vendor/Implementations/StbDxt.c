// The implementation of stb_dxt (BC1/BC3 compression), an object of its own in the stb library.

// stb_dxt.h calls memcpy without including its header (the other stb headers included it before it when one file held
// them all); C compilers that refuse implicit declarations (GCC 14) need it here.
#include <string.h>

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
