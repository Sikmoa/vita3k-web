#include "../src/memory.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace vita3k::web;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (false)

int main() {
    constexpr auto P = page_size;
    Memory empty(0);
    CHECK(empty.allocate(1) == 0);
    CHECK(!empty.valid(0));
    Memory partial(P + 1);
    CHECK(partial.allocate(1) == 0);
    CHECK(!partial.allocate_at(P, 1));
    Memory m(5 * P);
    CHECK(!m.allocate_at(1, 1));
    CHECK(m.allocate(0) == 0);
    CHECK(m.allocate(UINT32_MAX) == 0);
    CHECK(!m.allocate_at(UINT32_MAX - 2, 8));
    CHECK(!m.valid_range(P, UINT32_MAX));
    CHECK(!m.valid_range(P, 0));
    CHECK(!m.set_permission(P, 1, MemoryPermission::ReadWrite));

    // Unaligned fixed requests reserve every covered page, including the tail.
    std::string label = "owned name";
    CHECK(m.allocate_at(P + 3, P, label.c_str()));
    label.clear();
    CHECK(std::strcmp(m.name(P), "owned name") == 0);
    CHECK(m.valid_range(P, 2 * P));
    CHECK(!m.valid(3 * P));
    CHECK(!m.allocate_at(2 * P, 1));
    CHECK(!m.release(P));
    CHECK(m.allocate(1) == 3 * P);
    CHECK(m.allocate(1) == 4 * P);
    CHECK(m.allocate(1) == 0);
    CHECK(m.valid_range(5 * P - 1, 1));
    CHECK(!m.valid_range(5 * P - 1, 2));

    std::uint32_t value = 0x12345678, output = 0;
    CHECK(m.read(P + 1, &output, sizeof(output)) && output == 0);
    CHECK(m.write(2 * P - 1, &value, sizeof(value)));
    CHECK(m.read(2 * P - 1, &output, sizeof(output)) && output == value);
    auto ptr = m.translate(P, 1);
    CHECK(ptr && m.translate(P + 5, 1)->data == ptr->data + 5);
    CHECK(m.set_permission(2 * P, 1, MemoryPermission::Read));
    CHECK(!m.write(2 * P - 1, &value, sizeof(value)));
    CHECK(m.read(2 * P - 1, &output, sizeof(output)) && output == value);
    CHECK(!m.translate(2 * P, 1, true));
    CHECK(m.set_permission(P, P, MemoryPermission::None));
    CHECK(m.valid(P) && m.valid_range(P, P));
    CHECK(!m.read(P, &output, 1) && !m.write(P, &value, 1));
    CHECK(!m.allocate_at(P, 1));
    CHECK(m.set_permission(P, P, MemoryPermission::Write));
    CHECK(!m.read(P, &output, 1) && m.write(P, &value, 1));
    CHECK(m.set_permission(P, P, MemoryPermission::ReadWrite));
    CHECK(!m.set_permission(P, P, static_cast<MemoryPermission>(4)));
    CHECK(!m.read(P, nullptr, 1));
    CHECK(!m.write(P, nullptr, 1));
    CHECK(m.release(P + 3));
    CHECK(!m.release(P + 3));
    CHECK(!m.valid(P) && !m.valid(2 * P));
    CHECK(m.allocate(2 * P) == P);
    output = 1;
    CHECK(m.read(2 * P - 1, &output, sizeof(output)) && output == 0);
    CHECK(m.release(3 * P));
    CHECK(!m.valid_range(2 * P, 2 * P));
    CHECK(!m.set_permission(2 * P, 2 * P, MemoryPermission::None));
    CHECK(m.can_access(2 * P, 1, true));
    CHECK(m.translate(P, 1)->data == ptr->data);
    std::puts("M2 memory checks passed");
}
