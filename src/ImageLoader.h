#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

constexpr int kThumbSize = 64;

// Lists supported image files (png/jpg/jpeg/bmp/tga/gif) directly inside `dir`, sorted by path.
std::vector<std::filesystem::path> EnumerateImages(const std::filesystem::path& dir);

// Loads an image, scales it to fit inside kThumbSize x kThumbSize keeping the aspect ratio
// (Catmull-Rom bicubic), centers it on a black square. Output: kThumbSize^2 * 4 bytes RGBA (alpha = 255).
// Transparent pixels are composited onto black. Thread-safe.
bool LoadThumbnail(const std::filesystem::path& path, std::vector<uint8_t>& outRgba);
