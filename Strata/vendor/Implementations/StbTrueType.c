// The implementations of stb_rect_pack and stb_truetype (font rasterization and glyph atlases), in one object: stb_truetype
// uses rect_pack's packer when it sees its implementation.

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
