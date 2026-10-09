// Single compilation unit for miniaudio, with stb_vorbis enabling Ogg Vorbis decoding.
// stb_vorbis must be declared before the miniaudio implementation and defined after it.

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
