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
    // EQ is taken after CMP, while NE skips without changing the register.
    const std::uint32_t conditional[] = { 0x02833001u, 0x12833001u };
    CHECK(memory.write(page_size + 0x20, conditional, sizeof(conditional)));
    cpu.reset(page_size + 0x20);
    cpu.state().registers[3] = 9;
    cpu.state().cpsr |= 1u << 30;
    CHECK(cpu.step() && cpu.state().registers[3] == 10);
    cpu.state().cpsr |= 1u << 30;
    CHECK(cpu.step() && cpu.state().registers[3] == 10);

    // ARM: str r0,[r3,#0]; mov r0,#0; ldr r0,[r3,#0].
    const std::uint32_t arm_memory[] = { 0xe5830000u, 0xe3a00000u, 0xe5930000u };
    CHECK(memory.write(page_size, arm_memory, sizeof(arm_memory)));
    cpu.reset(page_size);
    cpu.state().registers[3] = page_size + 0x100;
    cpu.state().registers[0] = 0xaabbccddu;
    CHECK(cpu.run(3) == 3 && cpu.state().registers[0] == 0xaabbccddu);
    CHECK(memory.read(page_size + 0x100, &cpu.state().registers[0], sizeof(std::uint32_t)));

    // Thumb: movs r0,#3; adds r0,#4; subs r0,#1; b -2 (self-loop).
    const std::uint16_t thumb[] = { 0x2003u, 0x3004u, 0x3801u, 0xe7feu };
    CHECK(memory.write(page_size, thumb, sizeof(thumb)));
    cpu.reset(page_size | 1, true);
    CHECK(cpu.run(3) == 3 && cpu.state().registers[0] == 6);
    CHECK(cpu.step());
    CHECK(cpu.state().registers[15] == page_size + 4);

    // Thumb: str r0,[r1,#0]; movs r0,#0; ldr r0,[r1,#0].
    const std::uint16_t thumb_memory[] = { 0x6008u, 0x2000u, 0x6808u };
    CHECK(memory.write(page_size, thumb_memory, sizeof(thumb_memory)));
    cpu.reset(page_size | 1, true);
    cpu.state().registers[1] = page_size + 0x100;
    cpu.state().registers[0] = 0x11223344u;
    CHECK(cpu.run(3) == 3 && cpu.state().registers[0] == 0x11223344u);

    // Thumb register ALU: ADDS, EORS, TST, CMP, ORRS, BICS, MOVS.
    const std::uint16_t thumb_alu[] = { 0x1840u, 0x4048u, 0x4208u, 0x4288u, 0x4308u, 0x4388u, 0x4608u };
    CHECK(memory.write(page_size, thumb_alu, sizeof(thumb_alu)));
    cpu.reset(page_size | 1, true);
    cpu.state().registers[0] = 2;
    cpu.state().registers[1] = 3;
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 5);
    cpu.state().registers[1] = 1;
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 4);
    CHECK(cpu.run(1) == 1);
    CHECK(cpu.run(1) == 1);
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 5);
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 4);
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 1);

    // Thumb: LSLS r0,r1,#1; LSRS r0,r1,#1; ASRS r0,r1,#1.
    const std::uint16_t thumb_shifts[] = { 0x0048u, 0x0848u, 0x1048u };
    CHECK(memory.write(page_size, thumb_shifts, sizeof(thumb_shifts)));
    cpu.reset(page_size | 1, true);
    cpu.state().registers[1] = 0x80000002u;
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 4);
    cpu.state().registers[1] = 0x80000002u;
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 0x40000001u);
    cpu.state().registers[1] = 0x80000002u;
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 0xc0000001u);

    // Thumb: CMP r0,r1; BEQ +2; MOVS r2,#1; MOVS r2,#2.
    const std::uint16_t thumb_branch[] = { 0x4288u, 0xd001u, 0x2201u, 0x2202u };
    CHECK(memory.write(page_size, thumb_branch, sizeof(thumb_branch)));
    cpu.reset(page_size | 1, true);
    cpu.state().registers[0] = 7;
    cpu.state().registers[1] = 7;
    CHECK(cpu.run(2) == 2 && cpu.state().registers[15] == page_size + 6);
    CHECK(cpu.step() && cpu.state().registers[2] == 2);

    // Thumb: PUSH {r0,r1,lr}; POP {r0,r1,pc} restores registers and branches.
    const std::uint16_t thumb_stack[] = { 0xb503u, 0xbd03u };
    CHECK(memory.write(page_size, thumb_stack, sizeof(thumb_stack)));
    cpu.reset(page_size | 1, true);
    cpu.state().registers[13] = page_size + 0x800;
    cpu.state().registers[0] = 0x11111111u;
    cpu.state().registers[1] = 0x22222222u;
    cpu.state().registers[14] = page_size | 1;
    CHECK(cpu.step() && cpu.state().registers[13] == page_size + 0x7f4);
    cpu.state().registers[0] = 0;
    cpu.state().registers[1] = 0;
    CHECK(cpu.step() && cpu.state().registers[0] == 0x11111111u);
    CHECK(cpu.state().registers[1] == 0x22222222u && cpu.state().registers[15] == page_size);

    // Thumb byte/halfword stores and loads use checked guest memory.
    const std::uint16_t thumb_narrow_memory[] = {
        0x7008u, 0x7808u, 0x8008u, 0x8808u, 0x5008u, 0x5c08u,
    };
    CHECK(memory.write(page_size, thumb_narrow_memory, sizeof(thumb_narrow_memory)));
    cpu.reset(page_size | 1, true);
    cpu.state().registers[0] = 0x0000ff80u;
    cpu.state().registers[1] = page_size + 0x180;
    CHECK(cpu.run(1) == 1);
    cpu.state().registers[0] = 0;
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 0x80);
    cpu.state().registers[0] = 0xabcd1234u;
    CHECK(cpu.run(1) == 1);
    cpu.state().registers[0] = 0;
    CHECK(cpu.run(1) == 1 && cpu.state().registers[0] == 0x1234);

    // Unsupported instructions and memory faults halt execution explicitly.
    cpu.reset(0);
    CHECK(!cpu.step() && cpu.state().halted);
    CHECK(cpu.step_result() == StepResult::MemoryFault);
    CHECK(!cpu.step());
    CHECK(cpu.step_result() == StepResult::Halted);
    std::puts("M3 interpreter checks passed");
}
