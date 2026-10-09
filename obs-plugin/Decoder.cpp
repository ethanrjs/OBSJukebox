// Keep third-party implementations in exactly one translation unit.
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define STB_VORBIS_HEADER_ONLY
#include "../vendor/miniaudio/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "../vendor/miniaudio/miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "../vendor/miniaudio/stb_vorbis.c"
