#include "../src/interpreter.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace vita3k::web;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (false)

int main() {
    Memory memory(2 * page_size);
    CHECK(memory.allocate_at(page_size, page_size, "code"));

    // ARM: mov r0,#7; add r1,r0,#5; sub r2,r1,#2; cmp r2,#10; b +0.
    const std::uint32_t arm[] = {
        0xe3a00007u, 0xe2801005u, 0xe2412002u, 0xe352000au, 0xeafffffeu,
    };
    CHECK(memory.write(page_size, arm, sizeof(arm)));
    Interpreter cpu(memory);
    cpu.reset(page_size);
    CHECK(cpu.run(4) == 4);
    CHECK(cpu.state().registers[0] == 7 && cpu.state().registers[1] == 12);
    CHECK(cpu.state().registers[2] == 10 && (cpu.state().cpsr & (1u << 30)));
    CHECK(cpu.step());
    CHECK(cpu.state().registers[15] == page_size + 16);

    // Thumb: movs r0,#3; adds r0,#4; subs r0,#1; b -2 (self-loop).
    const std::uint16_t thumb[] = { 0x2003u, 0x3004u, 0x3801u, 0xe7feu };
    CHECK(memory.write(page_size, thumb, sizeof(thumb)));
    cpu.reset(page_size | 1, true);
    CHECK(cpu.run(3) == 3 && cpu.state().registers[0] == 6);
    CHECK(cpu.step());
    CHECK(cpu.state().registers[15] == page_size + 4);

    // Unsupported instructions and memory faults halt execution explicitly.
    cpu.reset(0);
    CHECK(!cpu.step() && cpu.state().halted);
    CHECK(!cpu.step());
    std::puts("M3 interpreter checks passed");
}
