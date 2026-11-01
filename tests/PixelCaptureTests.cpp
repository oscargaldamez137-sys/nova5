#include "local/pixel-capture-layout.h"
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <cstdlib>
#endif
int main() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    using namespace PixelCaptureLayout;
    std::vector<Plane> p;
    uint64_t bytes = 0;
    const auto check = [](bool value, const char* label) {
        if (!value) std::fprintf(stderr, "pixel capture: FAILED %s\n", label);
        return value;
    };
    if (!check(Plan(2432, 1368, 1, 1, 0, 1, 0, 1, 8, p, bytes) &&
               p.size() == 1 && p[0].row_bytes == 19456 && bytes == 26615808,
               "native RGBA16F extent (not the 1920-wide guest alias)")) return 1;
    if (!check(Plan(1920, 1080, 1, 1, 0, 1, 0, 1, 8, p, bytes) && bytes == 16588800,
               "small interpretation")) return 1;
    if (!check(Plan(17, 9, 5, 4, 1, 3, 2, 2, 8, p, bytes) && p.size() == 6 &&
               p[0].width == 8 && p[0].height == 4 && p[0].layer == 2 &&
               p[1].layer == 3 && p[2].mip == 2 && p.back().width == 2 && p.back().height == 1,
               "view base mip/layer and tight mip rows")) return 1;
    if (!check(Plan(1, 1, 1, 2, 0, 1, 0, 2, 3, p, bytes) && p[1].offset == 12 && bytes == 15,
               "offset aligned to both four bytes and texel size")) return 1;
    if (!check(!Plan(65536, 65536, 1, 1, 0, 1, 0, 1, 32, p, bytes) && p.empty() && bytes == 0,
               "oversized image fails without overflow")) return 1;
    if (!check(!Plan(64, 64, 1, 1, 0, 1, 0, UINT32_MAX, 8, p, bytes) && p.empty() && bytes == 0,
               "invalid layer range")) return 1;
    if (!check(!Plan(1, 1, 33, 1, 0, 33, 0, 1, 8, p, bytes), "invalid mip shift")) return 1;
    if (!check(!Plan(1, 1, 1, 1, 0, 1, 0, 1, 0, p, bytes), "zero texel size")) return 1;
    if (!check(Plan(1920, 1080, 1, 1, 0, 1, 0, 1, 4, p, bytes) &&
               bytes == 8294400 && p[0].row_bytes == 7680,
               "D32 depth aspect without combined stencil bytes")) return 1;
    if (!check(Plan(3, 2, 1, 2, 0, 1, 0, 2, 2, p, bytes) &&
               bytes == 24 && p[0].row_bytes == 6 && p[1].offset == 12,
               "D16 depth aspect with aligned layer offsets")) return 1;
    std::puts("pixel capture layout: 10 cases passed (host only)");
    return 0;
}
