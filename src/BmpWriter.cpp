#include "BmpWriter.h"

#include <cstring>
#include <fstream>
#include <vector>

bool WriteBmp24(const std::filesystem::path& path, int w, int h, const uint8_t* bgrTopDown) {
    if (w <= 0 || h <= 0 || !bgrTopDown) return false;

    const uint32_t stride = (static_cast<uint32_t>(w) * 3u + 3u) & ~3u;  // rows are padded to 4 bytes
    const uint32_t imageSize = stride * static_cast<uint32_t>(h);
    constexpr uint32_t kHeaderSize = 14 + 40;

    uint8_t hdr[kHeaderSize] = {};
    auto put16 = [&](int off, uint16_t v) {
        hdr[off] = static_cast<uint8_t>(v & 0xFF);
        hdr[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto put32 = [&](int off, uint32_t v) {
        for (int i = 0; i < 4; ++i) hdr[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    };

    // BITMAPFILEHEADER
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(2, kHeaderSize + imageSize);
    put32(10, kHeaderSize);
    // BITMAPINFOHEADER (positive height = bottom-up)
    put32(14, 40);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    put16(26, 1);
    put16(28, 24);
    put32(34, imageSize);
    put32(38, 2835);  // 72 DPI
    put32(42, 2835);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(hdr), kHeaderSize);

    std::vector<uint8_t> row(stride, 0);
    const size_t rowBytes = static_cast<size_t>(w) * 3;
    for (int y = h - 1; y >= 0; --y) {
        std::memcpy(row.data(), bgrTopDown + static_cast<size_t>(y) * rowBytes, rowBytes);
        f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(stride));
    }
    return f.good();
}
