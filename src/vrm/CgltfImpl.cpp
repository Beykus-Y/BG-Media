// Single translation unit for cgltf's C implementation. The parser is used
// only by the VRM importer; its symbols stay private to the bgmedia_vrm target.
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
