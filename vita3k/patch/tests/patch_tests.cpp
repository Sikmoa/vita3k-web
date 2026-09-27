// Title patches: the patch-file format (desktop Vita3K's patch directory) and
// their application to loaded segments.
//
//   node vita3k_web_patch_tests.js <repository>/browser/patches
#include <patch/patch.h>

#include <mem/functions.h>
#include <mem/ptr.h>
#include <mem/state.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)

using Bytes = std::vector<uint8_t>;

void parses_values() {
    // Hex words are written in the order they are typed.
    Patch patch = parse_patch("0:0xFC402 4FF00001");
    CHECK(patch.seg == 0 && patch.offset == 0xFC402);
    CHECK((patch.values == Bytes{ 0x4F, 0xF0, 0x00, 0x01 }));
    // Instructions are little endian; vblank is one 60 Hz vblank.
    patch = parse_patch("1:0x10 t1_mov(1, vblank) 00BF");
    CHECK(patch.seg == 1 && patch.offset == 0x10);
    CHECK((patch.values == Bytes{ 0x01, 0x21, 0x00, 0xBF }));
    // Leading zero bytes count.
    patch = parse_patch("0:0x0 0x0000 01");
    CHECK((patch.values == Bytes{ 0x00, 0x00, 0x01 }));
}

// The shipped Limbo patch, as the browser app reads it.
void reads_limbo_patch(const char *dir) {
    fs::path path = dir;
    const Patches patches = get_patches(path, "PCSE00268", "app0:eboot.bin");
    CHECK(patches.size() == 2);
    CHECK(patches[0].seg == 0 && patches[0].offset == 0xFC402);
    CHECK((patches[0].values == Bytes{ 0x4F, 0xF0, 0x00, 0x01 }));
    CHECK(patches[1].seg == 0 && patches[1].offset == 0x16ADE6);
    CHECK((patches[1].values == Bytes{ 0x01, 0x21, 0x00, 0xBF }));
    CHECK(get_patches(path, "PCSE00000", "app0:eboot.bin").empty());
}

void applies_within_segments() {
    constexpr uint32_t kText = 0x81000000, kData = 0x81010000;
    MemState mem;
    CHECK(init(mem, true));
    CHECK(alloc_at(mem, kText, 0x1000, "patch-text") == kText);
    CHECK(alloc_at(mem, kData, 0x1000, "patch-data") == kData);
    const std::vector<PatchSegment> segments = { { kText, 0x100 }, { 0, 0 }, { kData, 0x40 } };
    const Patches patches = {
        { 0, 0x10, { 0xAA, 0xBB } },
        { 2, 0x3C, { 1, 2, 3, 4 } }, // ends exactly at memsz
        { 2, 0x3D, { 1, 2, 3, 4 } }, // one byte past memsz
        { 0, 0xFFFFFFFF, { 1 } }, // offset + size overflows 32 bits
        { 1, 0, { 1 } }, // unloaded segment
        { 3, 0, { 1 } }, // no such segment
    };
    const std::vector<PatchedRange> written = apply_patches(mem, patches, segments);
    CHECK(written.size() == 2);
    CHECK(written[0].address == kText + 0x10 && written[0].size == 2);
    CHECK(written[1].address == kData + 0x3C && written[1].size == 4);
    CHECK(*Ptr<uint8_t>(kText + 0x10).get(mem) == 0xAA && *Ptr<uint8_t>(kText + 0x11).get(mem) == 0xBB);
    CHECK(std::memcmp(Ptr<uint8_t>(kData + 0x3C).get(mem), "\x01\x02\x03\x04", 4) == 0);
    CHECK(*Ptr<uint8_t>(kData + 0x40).get(mem) == 0); // the rejected patch wrote nothing
    deinit_mem(mem);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: vita3k_web_patch_tests <browser/patches>\n");
        return 2;
    }
    parses_values();
    reads_limbo_patch(argv[1]);
    applies_within_segments();
    std::printf("patch: %u checks passed\n", checks);
    return 0;
}
