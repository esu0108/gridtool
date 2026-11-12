#pragma once

#include <cstdint>
#include <filesystem>

// Writes a 24-bit uncompressed BMP.
// `bgrTopDown` is w*h*3 bytes, rows ordered top to bottom, pixel order B,G,R, no row padding.
bool WriteBmp24(const std::filesystem::path& path, int w, int h, const uint8_t* bgrTopDown);
