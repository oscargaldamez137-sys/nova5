#include "graphics/host_gpu/renderer/image/linearImageUpload.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace Libs::Graphics::LinearImageUpload;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "LinearImageUploadTests:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)

int main() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::vector<Rect> out;
    std::mt19937 random(193);
    // Independent per-texel oracle: every intersected texel, and no untouched
    // texel, must be copied once. Covers non-power-of-two pitches and padding.
    for (uint32_t bytes : {1u, 2u, 4u, 8u, 16u}) {
        for (uint32_t trial = 0; trial < 1000; ++trial) {
            const uint32_t width = 1 + random() % 31, height = 1 + random() % 17;
            const uint32_t pitch = width + random() % 9;
            const uint64_t size = uint64_t(pitch) * height * bytes;
            std::vector<std::pair<uint64_t, uint64_t>> dirty;
            for (unsigned i = 0; i < 6; ++i) {
                uint64_t a = random() % (size + 33), b = random() % (size + 33);
                dirty.emplace_back(std::min(a, b), std::max(a, b));
            }
            CHECK(Plan(width, height, pitch, bytes, size, dirty, false, out));
            std::vector<unsigned> visits(width * height);
            for (auto r : out) {
                CHECK(r.x + r.width <= width && r.y + r.height <= height);
                CHECK(r.offset == (uint64_t(r.y) * pitch + r.x) * bytes);
                CHECK(r.size == uint64_t(r.width) * r.height * bytes);
                CHECK(r.height == 1 || r.width == pitch);
                CHECK(r.offset + r.size <= size);
                for (uint32_t y = r.y; y < r.y + r.height; ++y)
                    for (uint32_t x = r.x; x < r.x + r.width; ++x) ++visits[y * width + x];
            }
            for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                const uint64_t a = (uint64_t(y) * pitch + x) * bytes;
                bool expected = false;
                for (auto [begin, end] : dirty) expected |= begin < end && begin < a + bytes && end > a;
                CHECK(visits[y * width + x] == unsigned(expected));
            }
        }
    }
    // Observed partial GPU fill: 1.5 MiB of a 960x540 RGBA16F allocation.
    std::vector<std::pair<uint64_t, uint64_t>> dirty{{0, 0x180000}};
    CHECK(Plan(960, 540, 960, 8, 0x3f4800, dirty, true, out));
    uint64_t copied = 0;
    for (auto r : out) copied += r.size;
    CHECK(copied == 0x180000 && out.size() == 2);
    // Both video-sized R8 allocations, including non-page-aligned bases: the
    // relative dirty ranges come from clipping pages to each image's range.
    for (uint64_t base : {0x403c10800ull, 0x4047ee100ull}) {
        dirty = {{0, 4096 - base % 4096}, {4096 - base % 4096, 8192 - base % 4096}};
        CHECK(Plan(3840, 3240, 3840, 1, 0xbdd800, dirty, false, out));
        copied = 0; for (auto r : out) copied += r.size;
        CHECK(copied == 8192 - base % 4096);
    }
    dirty = {{4096, 8192}};
    CHECK(Plan(1024, 1024, 1024, 8, 0x800000, dirty, false, out));
    CHECK(out.size() == 1 && out[0].x == 512 && out[0].width == 512 && out[0].size == 4096);
    dirty = {{1, 8}};
    CHECK(!Plan(960, 540, 960, 8, 0x3f4800, dirty, true, out));
    dirty = {{40, 64}}; // only row padding
    CHECK(Plan(10, 1, 16, 4, 64, dirty, true, out) && out.empty());
    CHECK(!Plan(10, 1, 9, 4, 64, dirty, false, out));
    CHECK(!Plan(10, 1, 16, 4, 39, dirty, false, out));
    dirty = {{0, 2050}};
    CHECK(!Plan(1, 1025, 2, 1, 2050, dirty, false, out) && out.empty());
    std::puts("LinearImageUploadTests: 5000 byte-oracle cases, observed layouts and GPU preservation passed");
}
