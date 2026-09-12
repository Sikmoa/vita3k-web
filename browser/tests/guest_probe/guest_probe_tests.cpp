#include "../../src/guest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace vita3k::web;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (false)

int main(int argc, char **argv) {
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    CHECK(input.good());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});

    Memory memory(4 * page_size);
    ElfLoadResult loaded;
    std::string error;
    CHECK(load_elf32(memory, bytes, loaded, error));
    CHECK(loaded.entry == page_size && loaded.thumb);
    CHECK(loaded.segments.size() == 1);
    CHECK(memory.permission(page_size) == MemoryPermission::ReadExecute);

    const auto result = run_guest(memory, loaded.entry, loaded.thumb, 8);
    CHECK(result.status == GuestResult::Status::Exited);
    CHECK(result.exit_code == 42 && result.instructions == 3);
    std::puts("Generated ELF guest probe checks passed");
}
