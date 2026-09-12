#include "interpreter.h"

#include <cstddef>
#include <cstdint>

namespace vita3k::web {
namespace {
constexpr std::uint32_t n_flag = 1u << 31;
constexpr std::uint32_t z_flag = 1u << 30;
constexpr std::uint32_t c_flag = 1u << 29;
constexpr std::uint32_t v_flag = 1u << 28;

bool condition_passed(const InterpreterState &s, std::uint32_t condition) {
    const bool n = (s.cpsr & n_flag) != 0;
    const bool z = (s.cpsr & z_flag) != 0;
    const bool c = (s.cpsr & c_flag) != 0;
    const bool v = (s.cpsr & v_flag) != 0;
    switch (condition) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xa: return n == v;
    case 0xb: return n != v;
    case 0xc: return !z && n == v;
    case 0xd: return z || n != v;
    case 0xe: return true;
    default: return false;
    }
}

void set_nz(InterpreterState &s, std::uint32_t value) {
    s.cpsr = (s.cpsr & ~(n_flag | z_flag))
        | (value & n_flag ? n_flag : 0)
        | (value == 0 ? z_flag : 0);
}

void set_add_flags(InterpreterState &s, std::uint32_t lhs, std::uint32_t rhs, std::uint32_t result) {
    set_nz(s, result);
    const auto wide = static_cast<std::uint64_t>(lhs) + rhs;
    if (wide >> 32) s.cpsr |= c_flag; else s.cpsr &= ~c_flag;
    const bool overflow = ((~(lhs ^ rhs) & (lhs ^ result)) & n_flag) != 0;
    if (overflow) s.cpsr |= v_flag; else s.cpsr &= ~v_flag;
}

void set_sub_flags(InterpreterState &s, std::uint32_t lhs, std::uint32_t rhs, std::uint32_t result) {
    set_nz(s, result);
    if (lhs >= rhs) s.cpsr |= c_flag; else s.cpsr &= ~c_flag;
    const bool overflow = (((lhs ^ rhs) & (lhs ^ result)) & n_flag) != 0;
    if (overflow) s.cpsr |= v_flag; else s.cpsr &= ~v_flag;
}

bool arm_step(InterpreterState &s, Memory &memory) {
    const auto pc = s.registers[15];
    std::uint32_t instruction = 0;
    if (!memory.read(pc, &instruction, sizeof(instruction)))
        return false;
    s.registers[15] = pc + 4;

    // B / BL, with an ARM PC-relative signed word offset.
    if ((instruction & 0x0e000000u) == 0x0a000000u) {
        if (!condition_passed(s, instruction >> 28))
            return true;
        const auto offset = static_cast<std::int32_t>(instruction << 8) >> 6;
        if (instruction & 0x01000000u)
            s.registers[14] = pc + 4;
        s.registers[15] = static_cast<std::uint32_t>(pc + 8 + offset);
        return true;
    }

    // LDR/STR with a positive, pre-indexed immediate word offset.
    if ((instruction & 0x0e500000u) == 0x04100000u) {
        if (!condition_passed(s, instruction >> 28)) return true;
        const auto rn = (instruction >> 16) & 0xf;
        const auto rd = (instruction >> 12) & 0xf;
        const auto address = s.registers[rn] + (instruction & 0xfffu);
        return memory.read(address, &s.registers[rd], sizeof(s.registers[rd]));
    }
    if ((instruction & 0x0e500000u) == 0x04000000u) {
        if (!condition_passed(s, instruction >> 28)) return true;
        const auto rn = (instruction >> 16) & 0xf;
        const auto rd = (instruction >> 12) & 0xf;
        const auto address = s.registers[rn] + (instruction & 0xfffu);
        return memory.write(address, &s.registers[rd], sizeof(s.registers[rd]));
    }

    // MOV, ADD, SUB and CMP with an immediate operand.
    if (!(instruction & (1u << 25)))
        return false;
    if (!condition_passed(s, instruction >> 28)) return true;
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
        if (set_flags) set_add_flags(s, lhs, operand, result);
        return true;
    case 0x2: // SUB
        result = lhs - operand;
        s.registers[rd] = result;
        if (set_flags) set_sub_flags(s, lhs, operand, result);
        return true;
    case 0xa: // CMP
        result = lhs - operand;
        set_sub_flags(s, lhs, operand, result);
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
    // ADD/SUB register (low registers).
    if ((instruction & 0xfe00u) == 0x1800u || (instruction & 0xfe00u) == 0x1a00u) {
        const auto rd = instruction & 7;
        const auto rn = (instruction >> 3) & 7;
        const auto rm = (instruction >> 6) & 7;
        const auto lhs = s.registers[rn];
        const auto rhs = s.registers[rm];
        s.registers[rd] = (instruction & 0x0200u) ? lhs - rhs : lhs + rhs;
        set_nz(s, s.registers[rd]);
        return true;
    }
    // MOV high-register form (the test uses a low destination).
    if ((instruction & 0xffc0u) == 0x4600u) {
        const auto rd = (instruction & 7) | ((instruction >> 4) & 8);
        const auto rm = (instruction >> 3) & 0xf;
        s.registers[rd] = s.registers[rm];
        return true;
    }
    // Register ALU operations: AND, EOR, TST, CMP, ORR, BIC, and MOV.
    if ((instruction & 0xfc00u) == 0x4000u) {
        const auto opcode = (instruction >> 6) & 0xf;
        const auto rm = (instruction >> 3) & 7;
        const auto rd = instruction & 7;
        const auto lhs = s.registers[rd];
        const auto rhs = s.registers[rm];
        std::uint32_t result = 0;
        switch (opcode) {
        case 0x0: result = lhs & rhs; break; // ANDS
        case 0x1: result = lhs ^ rhs; break; // EORS
        case 0x4: result = lhs + rhs; break; // ADDS
        case 0x8: result = lhs & rhs; break; // TST
        case 0xa: result = lhs - rhs; break; // CMP
        case 0xc: result = lhs | rhs; break; // ORRS
        case 0xd: result = rhs; break; // MOVS
        case 0xe: result = lhs - rhs; break; // BICS is approximated below
        default: return false;
        }
        if (opcode == 0xe) result = lhs & ~rhs;
        if (opcode != 0x8 && opcode != 0xa) s.registers[rd] = result;
        set_nz(s, result);
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
    // Conditional B, 8-bit signed halfword offset.
    if ((instruction & 0xf000u) == 0xd000u && (instruction & 0x0f00u) != 0x0f00u) {
        if (condition_passed(s, (instruction >> 8) & 0xf)) {
            const auto offset = static_cast<std::int32_t>(static_cast<std::int8_t>(instruction & 0xff)) << 1;
            s.registers[15] = static_cast<std::uint32_t>(pc + 2 + offset);
        }
        return true;
    }
    // Shift immediate: LSLS/LSRS/ASRS Rd, Rm, #imm5.
    if ((instruction & 0xe000u) == 0x0000u) {
        const auto opcode = (instruction >> 11) & 3;
        const auto rd = instruction & 7;
        const auto rm = (instruction >> 3) & 7;
        const auto shift = (instruction >> 6) & 0x1f;
        const auto value = s.registers[rm];
        if (opcode == 0) {
            if (shift) s.cpsr = (s.cpsr & ~c_flag) | ((value >> (32 - shift) & 1) << 29);
            s.registers[rd] = shift ? value << shift : value;
        } else if (opcode == 1) {
            if (shift) s.cpsr = (s.cpsr & ~c_flag) | (((value >> (shift - 1)) & 1) << 29);
            s.registers[rd] = shift ? value >> shift : 0;
        } else if (opcode == 2) {
            if (shift) s.cpsr = (s.cpsr & ~c_flag) | (((value >> (shift - 1)) & 1) << 29);
            s.registers[rd] = shift ? static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >> shift)
                                    : (value & n_flag ? 0xffffffffu : 0u);
        } else return false;
        set_nz(s, s.registers[rd]);
        return true;
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
