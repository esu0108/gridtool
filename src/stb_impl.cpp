// Single translation unit that compiles the stb implementations.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

// Avoid the global failure-reason string so stbi_load can be called from multiple threads.
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
