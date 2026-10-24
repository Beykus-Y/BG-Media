// Single translation unit for stb_image. VRM embeds PNG (and occasionally
// JPEG) textures in the GLB BIN chunk; the importer decodes them to RGBA8.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
