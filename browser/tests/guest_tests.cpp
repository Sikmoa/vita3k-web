#include "../src/guest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace vita3k::web;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (false)

int main() {
    Memory memory(4 * page_size);
    const std::uint32_t arm[] = { 0xe3a0002au, 0xef000001u };
    GuestImage image;
    image.entry = page_size;
    image.code.assign(reinterpret_cast<const std::uint8_t *>(arm), reinterpret_cast<const std::uint8_t *>(arm) + sizeof(arm));
    std::string error;
    CHECK(load_guest_image(memory, image, image.entry, error));
    CHECK(std::strcmp(memory.name(image.entry), "guest code") == 0);
    const auto result = run_guest(memory, image.entry, false, 8);
    CHECK(result.status == GuestResult::Status::Exited);
    CHECK(result.exit_code == 42 && result.instructions == 1);
    CHECK(std::strstr(result.message.c_str(), "sceKernelExitProcess") != nullptr);
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
