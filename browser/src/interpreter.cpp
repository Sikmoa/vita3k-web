#include "interpreter.h"

#include <cstddef>
#include <cstdint>

namespace vita3k::web {
namespace {
constexpr std::uint32_t n_flag = 1u << 31;
constexpr std::uint32_t z_flag = 1u << 30;
void set_nz(InterpreterState &s, std::uint32_t value) {
    s.cpsr = (s.cpsr & ~(n_flag | z_flag))
        | (value & n_flag ? n_flag : 0)
        | (value == 0 ? z_flag : 0);
}

bool arm_step(InterpreterState &s, Memory &memory) {
    const auto pc = s.registers[15];
    std::uint32_t instruction = 0;
    if (!memory.read(pc, &instruction, sizeof(instruction)))
        return false;
    s.registers[15] = pc + 4;

    // B / BL, with an ARM PC-relative signed word offset.
    if ((instruction & 0x0e000000u) == 0x0a000000u) {
        const auto offset = static_cast<std::int32_t>(instruction << 8) >> 6;
        if (instruction & 0x01000000u)
            s.registers[14] = pc + 4;
        s.registers[15] = static_cast<std::uint32_t>(pc + 8 + offset);
        return true;
    }

    // LDR/STR with a positive, pre-indexed immediate word offset (condition AL).
    if ((instruction >> 28) == 0xe && (instruction & 0x0e500000u) == 0x04100000u) {
        const auto rn = (instruction >> 16) & 0xf;
        const auto rd = (instruction >> 12) & 0xf;
        const auto address = s.registers[rn] + (instruction & 0xfffu);
        return memory.read(address, &s.registers[rd], sizeof(s.registers[rd]));
    }
    if ((instruction >> 28) == 0xe && (instruction & 0x0e500000u) == 0x04000000u) {
        const auto rn = (instruction >> 16) & 0xf;
        const auto rd = (instruction >> 12) & 0xf;
        const auto address = s.registers[rn] + (instruction & 0xfffu);
        return memory.write(address, &s.registers[rd], sizeof(s.registers[rd]));
    }

    // MOV, ADD, SUB and CMP with an immediate operand (condition AL only).
    if ((instruction >> 28) != 0xe || !(instruction & (1u << 25)))
        return false;
    const auto opcode = (instruction >> 21) & 0xf;
    const auto set_flags = (instruction & (1u << 20)) != 0;
    const auto rd = (instruction >> 12) & 0xf;
    const auto rn = (instruction >> 16) & 0xf;
    const auto rotate = ((instruction >> 8) & 0xf) * 2;
    const auto immediate = (instruction & 0xffu);
    const auto operand = (immediate >> rotate) | (immediate << ((32 - rotate) & 31));
    const auto lhs = s.registers[rn];
    std::uint32_t result = 0;
    switch (opcode) {
    case 0xd: // MOV
        result = operand;
        s.registers[rd] = result;
        if (set_flags) set_nz(s, result);
        return true;
    case 0x4: // ADD
        result = lhs + operand;
        s.registers[rd] = result;
        if (set_flags) set_nz(s, result);
        return true;
    case 0x2: // SUB
        result = lhs - operand;
        s.registers[rd] = result;
        if (set_flags) set_nz(s, result);
        return true;
    case 0xa: // CMP
        result = lhs - operand;
        if (set_flags || true) set_nz(s, result);
        return true;
    default:
        return false;
    }
}

bool thumb_step(InterpreterState &s, Memory &memory) {
    const auto pc = s.registers[15];
    std::uint16_t instruction = 0;
    if (!memory.read(pc, &instruction, sizeof(instruction)))
        return false;
    s.registers[15] = pc + 2;

    // MOVS Rd, #imm8.
    if ((instruction & 0xf800u) == 0x2000u) {
        const auto rd = (instruction >> 8) & 7;
        s.registers[rd] = instruction & 0xff;
        set_nz(s, s.registers[rd]);
        return true;
    }
    // ADDS/SUBS Rd, #imm8.
    if ((instruction & 0xf800u) == 0x3000u || (instruction & 0xf800u) == 0x3800u) {
        const auto rd = (instruction >> 8) & 7;
        const auto immediate = instruction & 0xff;
        const bool subtract = (instruction & 0x0800u) != 0;
        s.registers[rd] = subtract ? s.registers[rd] - immediate : s.registers[rd] + immediate;
        set_nz(s, s.registers[rd]);
        return true;
    }
    // STR/LDR Rt, [Rn, #imm5 * 4].
    if ((instruction & 0xf800u) == 0x6000u || (instruction & 0xf800u) == 0x6800u) {
        const auto rd = instruction & 7;
        const auto rn = (instruction >> 3) & 7;
        const auto address = s.registers[rn] + ((instruction >> 6) & 0x1f) * 4;
        if (instruction & 0x0800u)
            return memory.read(address, &s.registers[rd], sizeof(s.registers[rd]));
        return memory.write(address, &s.registers[rd], sizeof(s.registers[rd]));
    }
    // B, unconditional, 11-bit signed halfword offset.
    if ((instruction & 0xf800u) == 0xe000u) {
        const auto offset = static_cast<std::int32_t>(static_cast<std::int16_t>((instruction & 0x07ffu) << 5)) >> 4;
        s.registers[15] = static_cast<std::uint32_t>(pc + 2 + offset);
        return true;
    }
    // BX Rm.
    if ((instruction & 0xff87u) == 0x4700u) {
        const auto rm = (instruction >> 3) & 0xf;
        const auto target = s.registers[rm];
        s.thumb = (target & 1) != 0;
        s.cpsr = s.thumb ? s.cpsr | 0x20 : s.cpsr & ~0x20u;
        s.registers[15] = target & (s.thumb ? ~1u : ~3u);
        return true;
    }
    return false;
}
} // namespace

Interpreter::Interpreter(Memory &memory) noexcept : memory_(&memory) {}

void Interpreter::reset(std::uint32_t entry, bool thumb) noexcept {
    state_ = {};
    state_.registers[15] = entry & (thumb ? ~1u : ~3u);
    state_.thumb = thumb;
    if (thumb) state_.cpsr |= 0x20;
}

const InterpreterState &Interpreter::state() const noexcept { return state_; }
InterpreterState &Interpreter::state() noexcept { return state_; }

bool Interpreter::step() {
    if (state_.halted)
        return false;
    const bool result = state_.thumb ? thumb_step(state_, *memory_) : arm_step(state_, *memory_);
    if (!result) state_.halted = true;
    return result;
}

std::size_t Interpreter::run(std::size_t instruction_limit) {
    std::size_t executed = 0;
    while (executed < instruction_limit && step()) ++executed;
    return executed;
}
} // namespace vita3k::web
