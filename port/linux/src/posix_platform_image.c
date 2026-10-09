/* Bounded native PNG/JPEG decoding for desktop mesh and Forge UI assets. */
#if defined(HALO_64BIT) && (defined(HALO_FEATURE_PLATFORM) || defined(HALO_FEATURE_FORGE)) && !defined(HALO_ANDROID)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 4096
#include "stb_image.h"

unsigned char *platform_image_decode(const unsigned char *bytes, unsigned int size,
    int *width, int *height)
{
    int channels;
    if (!bytes || size > 64u * 1024u * 1024u ||
        !stbi_info_from_memory(bytes, (int)size, width, height, &channels) ||
        *width <= 0 || *height <= 0 || *width > 4096 || *height > 4096)
        return NULL;
    return stbi_load_from_memory(bytes, (int)size, width, height, &channels, 4);
}

void platform_image_free(unsigned char *pixels)
{
    stbi_image_free(pixels);
}
#endif
