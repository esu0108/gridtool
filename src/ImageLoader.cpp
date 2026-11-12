#include "ImageLoader.h"

#include <stb_image.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

struct FileCloser {
    void operator()(FILE* f) const {
        if (f) fclose(f);
    }
};

struct StbiFree {
    void operator()(stbi_uc* p) const { stbi_image_free(p); }
};

bool IsSupportedExtension(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".tga" || ext == L".gif";
}

}  // namespace

std::vector<fs::path> EnumerateImages(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2) || ec2) continue;
        if (IsSupportedExtension(it->path())) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool LoadThumbnail(const fs::path& path, std::vector<uint8_t>& outRgba) {
    // _wfopen so that non-ASCII paths work.
    std::unique_ptr<FILE, FileCloser> fp(_wfopen(path.c_str(), L"rb"));
    if (!fp) return false;

    int w = 0, h = 0, comp = 0;
    std::unique_ptr<stbi_uc, StbiFree> src(stbi_load_from_file(fp.get(), &w, &h, &comp, 4));
    if (!src || w <= 0 || h <= 0) return false;

    // Composite onto black and force opaque, so alpha never leaks into the resize / BMP.
    {
        stbi_uc* p = src.get();
        const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
        for (size_t i = 0; i < count; ++i, p += 4) {
            const unsigned a = p[3];
            if (a != 255) {
                p[0] = static_cast<stbi_uc>(p[0] * a / 255);
                p[1] = static_cast<stbi_uc>(p[1] * a / 255);
                p[2] = static_cast<stbi_uc>(p[2] * a / 255);
                p[3] = 255;
            }
        }
    }

    // Fit inside kThumbSize x kThumbSize (also upscales small images), keeping the aspect ratio.
    const double scale = static_cast<double>(kThumbSize) / static_cast<double>(std::max(w, h));
    const int nw = std::clamp(static_cast<int>(std::lround(w * scale)), 1, kThumbSize);
    const int nh = std::clamp(static_cast<int>(std::lround(h * scale)), 1, kThumbSize);

    std::vector<uint8_t> scaled(static_cast<size_t>(nw) * nh * 4);
    if (!stbir_resize(src.get(), w, h, 0, scaled.data(), nw, nh, 0, STBIR_RGBA, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP,
                      STBIR_FILTER_CATMULLROM)) {
        return false;
    }

    // Black, opaque square canvas; paste the scaled image in the center.
    outRgba.assign(static_cast<size_t>(kThumbSize) * kThumbSize * 4, 0);
    for (size_t i = 3; i < outRgba.size(); i += 4) outRgba[i] = 255;

    const int ox = (kThumbSize - nw) / 2;
    const int oy = (kThumbSize - nh) / 2;
    for (int y = 0; y < nh; ++y) {
        const uint8_t* s = scaled.data() + static_cast<size_t>(y) * nw * 4;
        uint8_t* d = outRgba.data() + (static_cast<size_t>(oy + y) * kThumbSize + ox) * 4;
        std::copy(s, s + static_cast<size_t>(nw) * 4, d);
    }
    return true;
}
