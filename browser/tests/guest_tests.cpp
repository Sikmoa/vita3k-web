#include "../src/guest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace vita3k::web;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (false)

int main() {
    Memory memory(4 * page_size);

    // ELF32 little-endian ARM image: MOVS-equivalent ARM MOV r0,#42,
    // r7 = test service, SVC #0, followed by zero-filled BSS.
    std::vector<std::uint8_t> elf(0x100, 0);
    auto put16 = [&elf](std::size_t o, std::uint16_t v) {
        elf[o] = static_cast<std::uint8_t>(v); elf[o + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    auto put32 = [&elf](std::size_t o, std::uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) elf[o + i] = static_cast<std::uint8_t>(v >> (i * 8));
    };
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F'; elf[4] = 1; elf[5] = 1;
    put16(16, 2); put16(18, 40); put32(24, page_size); put32(28, 52);
    put16(40, 52); put16(42, 32); put16(44, 1);
    put32(52, 1); put32(56, 0x80); put32(64, page_size); put32(68, 12);
    put32(72, 16); put32(76, 5); // file size 16, memory size 20, R-X
    const std::uint32_t elf_code[] = { 0xe3a0002au, 0xe3a07000u, 0xef000000u, 0 };
    std::memcpy(elf.data() + 0x80, elf_code, sizeof(elf_code));
    ElfLoadResult elf_result;
    std::string elf_error;
    CHECK(load_elf32(memory, elf, elf_result, elf_error));
    CHECK(elf_result.entry == page_size && !elf_result.thumb);
    CHECK(memory.read(page_size + 12, elf.data(), 1) && elf[0] == 0);
    CHECK(memory.can_access(page_size, 4, false) && !memory.can_access(page_size, 4, true));
    CHECK(run_guest(memory, elf_result.entry, false, 8).status == GuestResult::Status::Exited);
    CHECK(memory.release(page_size));
    const std::uint32_t arm[] = { 0xe3a0002au, 0xef000001u };
    GuestImage image;
    image.entry = page_size;
    image.code.assign(reinterpret_cast<const std::uint8_t *>(arm), reinterpret_cast<const std::uint8_t *>(arm) + sizeof(arm));
    std::string error;
    CHECK(load_guest_image(memory, image, image.entry, error));
    CHECK(std::strcmp(memory.name(image.entry), "guest code") == 0);
    const auto result = run_guest(memory, image.entry, false, 8);
    CHECK(result.status == GuestResult::Status::Exited);
    CHECK(result.exit_code == 42 && result.instructions == 2);
    CHECK(std::strstr(result.message.c_str(), "test SVC") != nullptr);
    CHECK(memory.release(image.entry));

    const std::uint16_t thumb[] = { 0x202au, 0xdf01u };
    image.code.assign(reinterpret_cast<const std::uint8_t *>(thumb), reinterpret_cast<const std::uint8_t *>(thumb) + sizeof(thumb));
    CHECK(load_guest_image(memory, image, image.entry, error));
    const auto thumb_result = run_guest(memory, image.entry | 1, true, 8);
    CHECK(thumb_result.status == GuestResult::Status::Exited && thumb_result.exit_code == 42);
    CHECK(memory.release(image.entry));

    const std::uint32_t loop[] = { 0xeafffffeu };
    image.code.assign(reinterpret_cast<const std::uint8_t *>(loop), reinterpret_cast<const std::uint8_t *>(loop) + sizeof(loop));
    CHECK(load_guest_image(memory, image, image.entry, error));
    const auto limited = run_guest(memory, image.entry, false, 3);
    CHECK(limited.status == GuestResult::Status::Limit && limited.instructions == 3);
    CHECK(memory.release(image.entry));

    CHECK(!load_guest_image(memory, image, 0, error));
    CHECK(error == "guest image does not fit in memory");
    std::puts("Guest execution checks passed");
}
