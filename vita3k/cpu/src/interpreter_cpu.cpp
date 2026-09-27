#include <cpu/impl/interpreter_cpu.h>
#include <cpu/state.h>
#include <mem/functions.h>

#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
constexpr uint32_t n_flag = 1u << 31;
constexpr uint32_t z_flag = 1u << 30;
constexpr uint32_t c_flag = 1u << 29;
constexpr uint32_t v_flag = 1u << 28;
constexpr uint32_t t_flag = 1u << 5;
constexpr uint32_t it_mask = (3u << 25) | (0x3fu << 10);

// All guest accesses use the checked MemState seam, including instruction fetch.
// Explicit byte packing is independent of host endianness and alignment.
uint32_t read_memory(CPUState *state, uint32_t address, unsigned size, bool fetch = false) {
    uint8_t bytes[4]{};
    if (!(fetch ? mem_fetch(*state->mem, address, bytes, size) : mem_read(*state->mem, address, bytes, size))) {
        char text[100];
        std::snprintf(text, sizeof(text), "guest %s fault address=0x%08x size=%u", fetch ? "fetch" : "read", address, size);
        throw std::runtime_error(text);
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= uint32_t(bytes[i]) << (8 * i);
    return value;
}
void write_memory(CPUState *state, uint32_t address, uint32_t value, unsigned size) {
    uint8_t bytes[4]{};
    for (unsigned i = 0; i < size; ++i) bytes[i] = uint8_t(value >> (8 * i));
    if (!mem_write(*state->mem, address, bytes, size)) {
        char text[100];
        std::snprintf(text, sizeof(text), "guest write fault address=0x%08x size=%u", address, size);
        throw std::runtime_error(text);
    }
}
uint32_t sign_extend(uint32_t value, unsigned bits) {
    const uint32_t sign = 1u << (bits - 1);
    return (value ^ sign) - sign;
}
struct Shift { uint32_t value; bool carry; };
// ARM Shift_C; immediate LSR/ASR #0 and RRX are decoded by the caller.
Shift shift(uint32_t value, unsigned type, unsigned count, bool carry) {
    if (!count) return {value, carry};
    switch (type) {
    case 0: return {count < 32 ? value << count : 0, count <= 32 && ((value >> (32 - count)) & 1)};
    case 1: return {count < 32 ? value >> count : 0, count <= 32 && ((value >> (count - 1)) & 1)};
    case 2: {
        const bool sign = (value & n_flag) != 0;
        return {count >= 32 ? (sign ? ~0u : 0) : (value >> count) | (sign ? (~0u << (32 - count)) : 0),
            count >= 32 ? sign : bool((value >> (count - 1)) & 1)};
    }
    default: return {std::rotr(value, int(count & 31)), bool((std::rotr(value, int(count & 31)) >> 31) & 1)};
    }
}
Shift immediate_shift(uint32_t value, unsigned type, unsigned count, bool carry) {
    if (type == 3 && count == 0) return {(uint32_t(carry) << 31) | (value >> 1), bool(value & 1)};
    return shift(value, type, (!count && (type == 1 || type == 2)) ? 32 : count, carry);
}
Shift thumb_expand(uint32_t imm, bool carry) {
    if ((imm & 0xc00) == 0) {
        const uint32_t b = imm & 255;
        switch ((imm >> 8) & 3) {
        case 0: return {b, carry};
        case 1: return {(b << 16) | b, carry};
        case 2: return {(b << 24) | (b << 8), carry};
        default: return {b * 0x01010101u, carry};
        }
    }
    const uint32_t v = std::rotr(0x80u | (imm & 127), int((imm >> 7) & 31));
    return {v, bool(v & n_flag)};
}
}

InterpreterCPU::InterpreterCPU(CPUState *state, std::size_t processor_id)
    : parent(state), core_id(processor_id) {}

int InterpreterCPU::run() {
    stopped = false;
    parent->svc_called = false;
    last_error.clear();
    for (uint64_t count = 0; count < instruction_budget; ++count) {
        if (stopped) return 1;
        const int result = step();
        if (result != 0 || parent->svc_called) return result;
    }
    uint32_t opcode = 0;
    try {
        if (is_thumb_mode()) {
            const uint32_t hi = read_memory(parent, regs[15], 2, true);
            opcode = (hi & 0xf800) >= 0xe800 ? (hi << 16) | (read_memory(parent, regs[15], 4, true) >> 16) : hi;
        } else opcode = read_memory(parent, regs[15], 4, true);
    } catch (const std::runtime_error &) {
        // The budget error is primary; do not replace it with a speculative fetch fault.
    }
    return fail(regs[15], opcode, "instruction budget exhausted");
}
void InterpreterCPU::stop() { stopped = true; }
uint32_t InterpreterCPU::get_reg(uint8_t idx) { return regs[idx & 15]; }
void InterpreterCPU::set_reg(uint8_t idx, uint32_t value) { regs[idx & 15] = value; }
uint32_t InterpreterCPU::get_sp() { return regs[13]; }
void InterpreterCPU::set_sp(uint32_t value) { regs[13] = value; }
uint32_t InterpreterCPU::get_pc() noexcept { return regs[15]; }
void InterpreterCPU::set_pc(uint32_t value) {
    if (value & 1) { cpsr |= t_flag; regs[15] = value & ~1u; }
    else { cpsr &= ~t_flag; regs[15] = value & ~3u; }
}
uint32_t InterpreterCPU::get_lr() { return regs[14]; }
void InterpreterCPU::set_lr(uint32_t value) { regs[14] = value; }
uint32_t InterpreterCPU::get_cpsr() { return cpsr; }
void InterpreterCPU::set_cpsr(uint32_t value) { cpsr = value; }
uint32_t InterpreterCPU::get_tpidruro() { return tpidruro; }
void InterpreterCPU::set_tpidruro(uint32_t value) { tpidruro = value; }
float InterpreterCPU::get_float_reg(uint8_t idx) { return std::bit_cast<float>(float_regs[idx & 63]); }
void InterpreterCPU::set_float_reg(uint8_t idx, float value) { float_regs[idx & 63] = std::bit_cast<uint32_t>(value); }
uint32_t InterpreterCPU::get_fpscr() { return fpscr; }
void InterpreterCPU::set_fpscr(uint32_t value) { fpscr = value; }
CPUContext InterpreterCPU::save_context() {
    CPUContext context;
    context.cpu_registers = regs;
    std::memcpy(context.fpu_registers.data(), float_regs.data(), sizeof(float_regs));
    context.cpsr = cpsr;
    context.fpscr = fpscr;
    return context;
}
void InterpreterCPU::load_context(const CPUContext &context) {
    regs = context.cpu_registers;
    std::memcpy(float_regs.data(), context.fpu_registers.data(), sizeof(float_regs));
    cpsr = context.cpsr;
    fpscr = context.fpscr;
}
uint32_t InterpreterCPU::operand(unsigned reg, uint32_t pc, bool thumb) const {
    return reg == 15 ? pc + (thumb ? 4 : 8) : regs[reg];
}
void InterpreterCPU::nz(uint32_t value) {
    cpsr = (cpsr & ~(n_flag | z_flag)) | (value & n_flag) | (value ? 0 : z_flag);
}
void InterpreterCPU::nzc(uint32_t value, bool carry) {
    nz(value);
    cpsr = (cpsr & ~c_flag) | (carry ? c_flag : 0);
}
uint32_t InterpreterCPU::add(uint32_t a, uint32_t b, bool carry, bool flags) {
    const uint64_t sum = uint64_t(a) + b + unsigned(carry);
    const uint32_t result = uint32_t(sum);
    if (flags) {
        nzc(result, (sum >> 32) != 0);
        cpsr = (cpsr & ~v_flag) | ((~(a ^ b) & (a ^ result) & n_flag) ? v_flag : 0);
    }
    return result;
}
bool InterpreterCPU::condition(unsigned cond) const {
    const bool n = cpsr & n_flag, z = cpsr & z_flag, c = cpsr & c_flag, v = cpsr & v_flag;
    switch (cond) {
    case 0: return z; case 1: return !z; case 2: return c; case 3: return !c;
    case 4: return n; case 5: return !n; case 6: return v; case 7: return !v;
    case 8: return c && !z; case 9: return !c || z; case 10: return n == v; case 11: return n != v;
    case 12: return !z && n == v; case 13: return z || n != v; case 14: return true;
    default: return false;
    }
}
uint8_t InterpreterCPU::itstate() const { return uint8_t(((cpsr >> 25) & 3) | ((cpsr >> 8) & 0xfc)); }
void InterpreterCPU::set_itstate(uint8_t value) {
    cpsr = (cpsr & ~it_mask) | (uint32_t(value & 3) << 25) | (uint32_t(value & 0xfc) << 8);
}
void InterpreterCPU::advance_it() {
    const uint8_t it = itstate();
    set_itstate((it & 7) == 0 ? 0 : (it & 0xe0) | ((it << 1) & 0x1f));
}
int InterpreterCPU::fail(uint32_t pc, uint32_t opcode, const char *reason) {
    char text[240];
    std::snprintf(text, sizeof(text), "InterpreterCPU: %s at PC=0x%08x opcode=0x%08x (%s)", reason, pc, opcode, is_thumb_mode() ? "Thumb" : "ARM");
    last_error = text;
    std::fprintf(stderr, "%s\n", text);
    return -1;
}
int InterpreterCPU::step() {
    parent->svc_called = false;
    ++executed_instructions;
    if (stopped) return 1;
    const uint32_t pc = regs[15], saved_cpsr = cpsr;
    uint32_t opcode = 0;
    try {
        int result;
        if (is_thumb_mode()) {
            const uint16_t hi = uint16_t(read_memory(parent, pc, 2, true));
            const bool wide = (hi & 0xf800) >= 0xe800;
            opcode = hi;
            // Validate a wide fetch as one range, including the 4 GiB boundary.
            const uint16_t lo = wide ? uint16_t(read_memory(parent, pc, 4, true) >> 16) : 0;
            opcode = wide ? (uint32_t(hi) << 16) | lo : hi;
            regs[15] = pc + (wide ? 4 : 2);
            if (log_code) std::fprintf(stderr, "Thumb PC=%08x opcode=%08x CPSR=%08x\n", pc, opcode, cpsr);
            const bool in_it = itstate() != 0;
            if (in_it && !condition(itstate() >> 4)) result = 0;
            else result = wide ? thumb32(hi, lo, pc) : thumb16(hi, pc, in_it);
            if (result == 0 && in_it) advance_it();
        } else {
            opcode = read_memory(parent, pc, 4, true);
            regs[15] = pc + 4;
            if (log_code) std::fprintf(stderr, "ARM PC=%08x opcode=%08x CPSR=%08x\n", pc, opcode, cpsr);
            result = arm(opcode, pc);
        }
        if (result == 0) return 0;
        regs[15] = pc;
        cpsr = saved_cpsr;
        return fail(pc, opcode, "unsupported or invalid instruction");
    } catch (const std::runtime_error &error) {
        regs[15] = pc;
        cpsr = saved_cpsr;
        return fail(pc, opcode, error.what());
    }
}

int InterpreterCPU::thumb16(uint16_t op, uint32_t pc, bool in_it) {
    const unsigned rd = op & 7, rn = (op >> 3) & 7, rm = (op >> 6) & 7;
    const bool flags = !in_it;
    if ((op & 0xf800) < 0x1800) { // immediate shifts
        const auto s = immediate_shift(regs[rn], (op >> 11) & 3, (op >> 6) & 31, cpsr & c_flag);
        regs[rd] = s.value;
        if (flags) nzc(s.value, s.carry);
    } else if ((op & 0xf800) == 0x1800) {
        const uint32_t b = (op & 0x400) ? rm : regs[rm];
        regs[rd] = (op & 0x200) ? add(regs[rn], ~b, true, flags) : add(regs[rn], b, false, flags);
    } else if ((op & 0xe000) == 0x2000) {
        const unsigned d = (op >> 8) & 7, imm = op & 255;
        switch ((op >> 11) & 3) {
        case 0: regs[d] = imm; if (flags) nz(imm); break;
        case 1: add(regs[d], ~imm, true, true); break;
        case 2: regs[d] = add(regs[d], imm, false, flags); break;
        case 3: regs[d] = add(regs[d], ~imm, true, flags); break;
        }
    } else if ((op & 0xfc00) == 0x4000) {
        const unsigned kind = (op >> 6) & 15;
        const uint32_t a = regs[rd], b = regs[rn];
        uint32_t result = 0;
        switch (kind) {
        case 0: result = a & b; break;
        case 1: result = a ^ b; break;
        case 2: case 3: case 4: case 7: {
            auto s = shift(a, kind == 7 ? 3 : kind - 2, b & 255, cpsr & c_flag);
            regs[rd] = s.value; if (flags) nzc(s.value, s.carry); return 0;
        }
        case 5: regs[rd] = add(a, b, cpsr & c_flag, flags); return 0;
        case 6: regs[rd] = add(a, ~b, cpsr & c_flag, flags); return 0;
        case 8: nz(a & b); return 0;
        case 9: regs[rd] = add(0, ~b, true, flags); return 0;
        case 10: add(a, ~b, true, true); return 0;
        case 11: add(a, b, false, true); return 0;
        case 12: result = a | b; break;
        case 13: result = a * b; break;
        case 14: result = a & ~b; break;
        case 15: result = ~b; break;
        }
        regs[rd] = result; if (flags) nz(result);
    } else if ((op & 0xfc00) == 0x4400) {
        const unsigned d = (op & 7) | ((op >> 4) & 8), m = (op >> 3) & 15;
        const uint32_t b = operand(m, pc, true);
        switch ((op >> 8) & 3) {
        case 0: { const uint32_t v = operand(d, pc, true) + b; if (d == 15) regs[15] = v & ~1u; else regs[d] = v; break; }
        case 1: add(operand(d, pc, true), ~b, true, true); break;
        case 2: if (d == 15) regs[15] = b & ~1u; else regs[d] = b; break;
        case 3:
            if (op & 7) return -1;
            if (op & 0x80) regs[14] = (pc + 2) | 1;
            set_pc(b); break;
        }
    } else if ((op & 0xf800) == 0x4800) {
        regs[(op >> 8) & 7] = read_memory(parent, ((pc + 4) & ~3u) + ((op & 255) << 2), 4);
    } else if ((op & 0xf000) == 0x5000) {
        const unsigned kind = (op >> 9) & 7;
        const uint32_t address = regs[rn] + regs[rm];
        const unsigned size = (kind == 0 || kind == 4) ? 4 : (kind == 1 || kind == 5 || kind == 7) ? 2 : 1;
        if (kind < 3) write_memory(parent, address, regs[rd], size);
        else { uint32_t v = read_memory(parent, address, size); regs[rd] = kind == 3 || kind == 7 ? sign_extend(v, size * 8) : v; }
    } else if ((op & 0xe000) == 0x6000 || (op & 0xf000) == 0x8000) {
        const unsigned size = (op & 0x8000) ? 2 : (op & 0x1000) ? 1 : 4;
        const uint32_t address = regs[rn] + ((op >> 6) & 31) * size;
        if (op & 0x800) regs[rd] = read_memory(parent, address, size);
        else write_memory(parent, address, regs[rd], size);
    } else if ((op & 0xf000) == 0x9000) {
        const uint32_t address = regs[13] + ((op & 255) << 2);
        if (op & 0x800) regs[(op >> 8) & 7] = read_memory(parent, address, 4);
        else write_memory(parent, address, regs[(op >> 8) & 7], 4);
    } else if ((op & 0xf000) == 0xa000) {
        regs[(op >> 8) & 7] = ((op & 0x800) ? regs[13] : ((pc + 4) & ~3u)) + ((op & 255) << 2);
    } else if ((op & 0xff00) == 0xb000) {
        regs[13] += (op & 0x80) ? 0u - ((op & 127) << 2) : ((op & 127) << 2);
    } else if ((op & 0xf500) == 0xb100) { // CBZ/CBNZ
        if (in_it) return -1;
        if ((regs[rd] != 0) == bool(op & 0x800)) regs[15] = pc + 4 + ((op >> 2) & 0x3e) + ((op >> 3) & 0x40);
    } else if ((op & 0xff00) == 0xb200) {
        switch ((op >> 6) & 3) {
        case 0: regs[rd] = sign_extend(regs[rn] & 65535, 16); break;
        case 1: regs[rd] = sign_extend(regs[rn] & 255, 8); break;
        case 2: regs[rd] = regs[rn] & 65535; break;
        case 3: regs[rd] = regs[rn] & 255; break;
        }
    } else if ((op & 0xf600) == 0xb400) { // PUSH/POP
        const bool load = op & 0x800;
        const uint32_t list = (op & 255) | ((op & 0x100) ? (load ? 0x8000 : 0x4000) : 0);
        if (!list) return -1;
        uint32_t address = load ? regs[13] : regs[13] - 4 * std::popcount(list);
        const uint32_t next_sp = load ? regs[13] + 4 * std::popcount(list) : address;
        for (unsigned r = 0; r < 16; ++r) if (list & (1u << r)) {
            if (load) { const uint32_t v = read_memory(parent, address, 4); if (r == 15) set_pc(v); else regs[r] = v; }
            else write_memory(parent, address, regs[r], 4);
            address += 4;
        }
        regs[13] = next_sp;
    } else if ((op & 0xff00) == 0xbf00) {
        if (!(op & 15)) return op == 0xbf00 ? 0 : -1; // NOP only, not other hints
        if (in_it || ((op >> 4) & 15) >= 14) return -1;
        set_itstate(uint8_t(op));
    } else if ((op & 0xf000) == 0xc000) { // STMIA/LDMIA
        const unsigned base = (op >> 8) & 7;
        const bool load = op & 0x800;
        const uint32_t list = op & 255;
        if (!list) return -1;
        uint32_t address = regs[base];
        for (unsigned r = 0; r < 8; ++r) if (list & (1u << r)) {
            if (load) regs[r] = read_memory(parent, address, 4);
            else write_memory(parent, address, regs[r], 4);
            address += 4;
        }
        if (!load || !(list & (1u << base))) regs[base] = address;
    } else if ((op & 0xff00) == 0xdf00) {
        parent->svc = op & 255; parent->svc_called = true;
    } else if ((op & 0xf000) == 0xd000) {
        const unsigned cond = (op >> 8) & 15;
        if (cond >= 14 || in_it) return -1;
        if (condition(cond)) regs[15] = pc + 4 + sign_extend((op & 255) << 1, 9);
    } else if ((op & 0xf800) == 0xe000) {
        regs[15] = pc + 4 + sign_extend((op & 0x7ff) << 1, 12);
    } else return -1;
    return 0;
}

int InterpreterCPU::thumb32(uint16_t hi, uint16_t lo, uint32_t pc) {
    const unsigned rn = hi & 15, rd = (lo >> 8) & 15, rt = lo >> 12;
    const uint32_t imm12 = ((hi >> 10) & 1) << 11 | ((lo >> 12) & 7) << 8 | (lo & 255);
    if (hi == 0xf3af && lo == 0x8000) return 0; // NOP.W shares branch prefix
    if ((hi & 0xff00) == 0xf300 && (lo & 0x0010) == 0 && rd != 15) {
        // T2 bitfield ops (ARMv7-A A6.7), verified against VitaSDK binutils:
        //   UBFX r0,r1,#0,#9  = f3c1 0008  (hi&0xe0 == 0xc0)
        //   SBFX r2,r3,#4,#12 = f343 120b  (hi&0xe0 == 0x40)
        //   BFI  r4,r5,#8,#5  = f365 240c  (hi&0xe0 == 0x60)
        //   BFC  r6,#3,#7     = f36f 06c9  (hi&0xe0 == 0x60, rn==15)
        // lsb = imm3:imm2; low nibble of lo = msb (insert) / width-1 (extract).
        const unsigned lsb = (((lo >> 12) & 7) << 2) | ((lo >> 6) & 3);
        const unsigned field = lo & 0xf;
        switch (hi & 0xe0) {
        case 0x40: { // SBFX
            const unsigned width = field + 1;
            if (rn == 15 || lsb + width > 32) return -1;
            const uint32_t bits = (regs[rn] >> lsb) & (width >= 32 ? 0xffffffffu : (1u << width) - 1);
            regs[rd] = sign_extend(bits, width);
            break;
        }
        case 0xc0: { // UBFX
            const unsigned width = field + 1;
            if (rn == 15 || lsb + width > 32) return -1;
            regs[rd] = (regs[rn] >> lsb) & (width >= 32 ? 0xffffffffu : (1u << width) - 1);
            break;
        }
        case 0x60: { // BFI/BFC
            const unsigned msb = field;
            if (lsb > msb || msb > 31) return -1;
            const unsigned width = msb - lsb + 1;
            const uint32_t src = rn == 15 ? 0 : regs[rn];
            const uint32_t mask = ((1u << width) - 1) << lsb;
            regs[rd] = (regs[rd] & ~mask) | ((src << lsb) & mask);
            break;
        }
        default:
            return -1;
        }
    } else if ((hi & 0xff00) == 0xfa00 && (lo & 0xf000) == 0xf000 && ((lo >> 6) & 3) == 2
        && (((hi >> 5) & 7) == 0 || ((hi >> 5) & 7) == 2)) {
        // T2 extends, verified against VitaSDK GAS encodings:
        //   uxtb.w r0,r1 = fa5f f081; uxtab r8,r9,r2 = fa59 f882;
        //   uxth.w r4,r5,ror #16 = fa1f f4a5.
        // hi[7:5]=000 halfword / 010 byte, hi[4]=unsigned, hi[3:0]=Rn
        // (15 = plain extend, otherwise extend-and-add), lo[15:12]=1111,
        // lo[11:8]=Rd, lo[7:6]=10, lo[5:4]=rotation/8, lo[3:0]=Rm.
        if (rd == 15 || (lo & 15) == 15) return -1;
        const unsigned rot = ((lo >> 4) & 3) * 8;
        const uint32_t rotated = std::rotr(regs[lo & 15], int(rot));
        const bool halfword = ((hi >> 5) & 7) == 0;
        const uint32_t ext = halfword
            ? ((hi & 0x10) ? rotated & 0xffff : sign_extend(rotated & 0xffff, 16))
            : ((hi & 0x10) ? rotated & 0xff : sign_extend(rotated & 0xff, 8));
        regs[rd] = rn == 15 ? ext : regs[rn] + ext;
    } else if ((hi & 0xfff0) == 0xfa90 && (lo & 0xf000) == 0xf000) {
        // T2 reverse/bit-field ops, verified against VitaSDK GAS encodings:
        //   rev.w r8,r9 = fa99 f889; rev16.w r10,r11 = fa9b fa9b;
        //   rbit r6,r7 = fa97 f6a7; revsh.w r4,r5 = fa95 f4b5.
        // hi=0xfa9R (GAS repeats Rm in hi[3:0]); lo[7:4] selects the op,
        // Rm is lo[3:0] and Rd is lo[11:8].
        if (rd == 15 || (lo & 15) == 15) return -1;
        const uint32_t v = regs[lo & 15];
        switch ((lo >> 4) & 15) {
        case 8: // REV: reverse bytes
            regs[rd] = ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
            break;
        case 9: // REV16: reverse bytes within each halfword
            regs[rd] = ((v & 0x00ff00ff) << 8) | ((v >> 8) & 0x00ff00ff);
            break;
        case 0xa: { // RBIT: reverse bits
            uint32_t r = v;
            r = ((r >> 1) & 0x55555555) | ((r & 0x55555555) << 1);
            r = ((r >> 2) & 0x33333333) | ((r & 0x33333333) << 2);
            r = ((r >> 4) & 0x0f0f0f0f) | ((r & 0x0f0f0f0f) << 4);
            r = ((r >> 8) & 0x00ff00ff) | ((r & 0x00ff00ff) << 8);
            regs[rd] = (r >> 16) | (r << 16);
            break;
        }
        case 0xb: // REVSH: reverse bytes of low halfword, sign-extend
            regs[rd] = sign_extend(((v & 0xff) << 8) | ((v >> 8) & 0xff), 16);
            break;
        default:
            return -1;
        }
    } else if ((hi & 0xfff0) == 0xfab0 && (lo & 0xf0f0) == 0xf080) {
        // CLZ.W, verified against VitaSDK GAS: clz r1,r2 = fab2 f182.
        if (rd == 15 || (lo & 15) == 15) return -1;
        regs[rd] = regs[lo & 15] == 0 ? 32u : static_cast<uint32_t>(std::countl_zero(regs[lo & 15]));
    } else if ((hi & 0xf800) == 0xf000 && (lo & 0x8000)) { // branches, not data processing
        const unsigned s = (hi >> 10) & 1, j1 = (lo >> 13) & 1, j2 = (lo >> 11) & 1;
        if ((lo & 0xd000) == 0x8000) {
            const unsigned cond = (hi >> 6) & 15;
            if (cond >= 14 || itstate()) return -1;
            const uint32_t imm = (s << 20) | (j2 << 19) | (j1 << 18) | ((hi & 63) << 12) | ((lo & 0x7ff) << 1);
            if (condition(cond)) regs[15] = pc + 4 + sign_extend(imm, 21);
        } else {
            const uint32_t imm = (s << 24) | ((!(j1 ^ s)) << 23) | ((!(j2 ^ s)) << 22) | ((hi & 0x3ff) << 12) | ((lo & 0x7ff) << 1);
            const bool link = lo & 0x4000, exchange = link && !(lo & 0x1000);
            if (exchange && (lo & 1)) return -1;
            if (link) regs[14] = (pc + 4) | 1;
            const uint32_t target = (exchange ? ((pc + 4) & ~3u) : pc + 4) + sign_extend(imm, 25);
            if (exchange) set_pc(target); else regs[15] = target & ~1u;
        }
    } else if ((hi & 0xfbf0) == 0xf240 || (hi & 0xfbf0) == 0xf2c0) {
        if ((lo & 0x8000) || rd == 15) return -1;
        const uint32_t imm = ((hi & 15) << 12) | imm12;
        regs[rd] = (hi & 0x80) ? (regs[rd] & 65535) | (imm << 16) : imm;
    } else if ((hi & 0xfa00) == 0xf000 && !(lo & 0x8000)) { // modified immediate
        const unsigned kind = (hi >> 5) & 15;
        const bool flags = hi & 0x10;
        const auto b = thumb_expand(imm12, cpsr & c_flag);
        const uint32_t a = rn == 15 ? 0 : regs[rn];
        uint32_t result;
        bool logical = true;
        switch (kind) {
        case 0: result = a & b.value; break;
        case 1: result = a & ~b.value; break;
        case 2: result = a | b.value; break; // MOV when rn == 15
        case 3: result = a | ~b.value; break; // MVN when rn == 15
        case 4: result = a ^ b.value; break;
        case 8: result = add(a, b.value, false, flags); logical = false; break;
        case 10: result = add(a, b.value, cpsr & c_flag, flags); logical = false; break;
        case 11: result = add(a, ~b.value, cpsr & c_flag, flags); logical = false; break;
        case 13: result = add(a, ~b.value, true, flags); logical = false; break;
        case 14: result = add(~a, b.value, true, flags); logical = false; break;
        default: return -1;
        }
        if (logical && flags) nzc(result, b.carry);
        if (rd != 15) regs[rd] = result;
        else if (!flags) return -1;
    } else if ((hi & 0xfe00) == 0xea00) { // shifted register logical/arithmetic
        const unsigned kind = (hi >> 5) & 15;
        const bool flags = hi & 0x10;
        if (lo & 0x8000) return -1;
        const auto b = immediate_shift(regs[lo & 15], (lo >> 4) & 3, ((lo >> 10) & 0x1c) | ((lo >> 6) & 3), cpsr & c_flag);
        const uint32_t a = rn == 15 ? 0 : regs[rn];
        uint32_t result;
        bool logical = true;
        switch (kind) {
        case 0: result = a & b.value; break;
        case 1: result = a & ~b.value; break;
        case 2: result = a | b.value; break;
        case 3: result = a | ~b.value; break;
        case 4: result = a ^ b.value; break;
        case 8: result = add(a, b.value, false, flags); logical = false; break;
        case 10: result = add(a, b.value, cpsr & c_flag, flags); logical = false; break;
        case 11: result = add(a, ~b.value, cpsr & c_flag, flags); logical = false; break;
        case 13: result = add(a, ~b.value, true, flags); logical = false; break;
        case 14: result = add(~a, b.value, true, flags); logical = false; break;
        default: return -1;
        }
        if (logical && flags) nzc(result, b.carry);
        if (rd != 15) regs[rd] = result; else if (!flags) return -1;
    } else if ((hi & 0xff80) == 0xfa00 && (lo & 0xf0f0) == 0xf000) { // register shift
        const auto s = shift(regs[rn], (hi >> 5) & 3, regs[lo & 15] & 255, cpsr & c_flag);
        if (rd == 15 || rn == 15 || (lo & 15) == 15) return -1;
        regs[rd] = s.value; if (hi & 0x10) nzc(s.value, s.carry);
    } else if ((hi & 0xfe40) == 0xe800) { // STM/LDM IA or DB
        const bool load = hi & 0x10, decrement = hi & 0x100, wb = hi & 0x20;
        if (!lo || rn == 15 || (wb && (lo & (1u << rn)))) return -1;
        const uint32_t size = 4 * std::popcount(lo);
        uint32_t address = regs[rn] - (decrement ? size : 0);
        const uint32_t end = decrement ? address : address + size;
        for (unsigned r = 0; r < 16; ++r) if (lo & (1u << r)) {
            if (load) { const uint32_t v = read_memory(parent, address, 4); if (r == 15) set_pc(v); else regs[r] = v; }
            else write_memory(parent, address, regs[r], 4);
            address += 4;
        }
        if (wb) regs[rn] = end;
    } else if ((hi & 0xfe40) == 0xe840) { // LDRD/STRD immediate (not exclusives)
        const bool pre = hi & 0x100, up = hi & 0x80, wb = hi & 0x20, load = hi & 0x10;
        if ((!pre && !wb) || rt >= 15 || rd >= 15 || rt == rd || (wb && (rn == rt || rn == rd || rn == 15))) return -1;
        const uint32_t base = rn == 15 ? ((pc + 4) & ~3u) : regs[rn];
        const uint32_t indexed = base + (up ? uint32_t(lo & 255) * 4 : 0u - uint32_t(lo & 255) * 4);
        const uint32_t address = pre ? indexed : base;
        if (load) { regs[rt] = read_memory(parent, address, 4); regs[rd] = read_memory(parent, address + 4, 4); }
        else { write_memory(parent, address, regs[rt], 4); write_memory(parent, address + 4, regs[rd], 4); }
        if (wb) regs[rn] = indexed;
    } else if ((hi & 0xff00) == 0xf800 || (hi & 0xff10) == 0xf910) { // single load/store
        const unsigned kind = (hi >> 4) & 7;
        const bool load = kind & 1, signed_load = hi & 0x100;
        const unsigned size_code = kind >> 1;
        if (size_code > 2 || (signed_load && (!load || size_code == 2))) return -1;
        const unsigned size = 1u << size_code;
        const uint32_t base = rn == 15 ? ((pc + 4) & ~3u) : regs[rn];
        uint32_t address = base, indexed = base;
        bool wb = false;
        if (rn == 15) {
            if (!load) return -1;
            address += (hi & 0x80) ? (lo & 0xfff) : 0u - (lo & 0xfff);
        } else if (hi & 0x80) address += lo & 0xfff;
        else if (lo & 0x800) {
            const bool pre = lo & 0x400, up = lo & 0x200;
            wb = lo & 0x100;
            if ((!pre && !wb) || (wb && rn == rt)) return -1;
            indexed = base + (up ? uint32_t(lo & 255) : 0u - uint32_t(lo & 255));
            address = pre ? indexed : base;
        } else {
            if (lo & 0xfc0) return -1;
            address += regs[lo & 15] << ((lo >> 4) & 3);
        }
        if (rt == 15 && (!load || size != 4)) return -1;
        if (load) {
            uint32_t v = read_memory(parent, address, size);
            if (signed_load) v = sign_extend(v, size * 8);
            if (rt == 15) set_pc(v); else regs[rt] = v;
        } else write_memory(parent, address, regs[rt], size);
        if (wb) regs[rn] = indexed;
    } else if ((hi & 0xffbf) == 0xed2d && (lo & 0xf00) == 0xb00) { // VPUSH double registers
        const unsigned first = (((hi >> 6) & 1) * 16 + rt) * 2, count = lo & 255;
        if (!count || (count & 1) || first + count > 64) return -1;
        const uint32_t address = regs[13] - count * 4;
        for (unsigned i = 0; i < count; ++i) write_memory(parent, address + 4 * i, float_regs[first + i], 4);
        regs[13] = address;
    } else if ((hi & 0xffbf) == 0xecbd && (lo & 0xf00) == 0xb00) { // VPOP double registers
        const unsigned first = (((hi >> 6) & 1) * 16 + rt) * 2, count = lo & 255;
        if (!count || (count & 1) || first + count > 64) return -1;
        for (unsigned i = 0; i < count; ++i) float_regs[first + i] = read_memory(parent, regs[13] + 4 * i, 4);
        regs[13] += count * 4;
    } else if ((hi & 0xff20) == 0xed00 && (lo & 0xf00) == 0xb00) { // VLDR/VSTR D
        const unsigned first = (((hi >> 6) & 1) * 16 + rt) * 2;
        const uint32_t base = rn == 15 ? ((pc + 4) & ~3u) : regs[rn];
        const uint32_t address = base + ((hi & 0x80) ? uint32_t(lo & 255) * 4 : 0u - uint32_t(lo & 255) * 4);
        for (unsigned i = 0; i < 2; ++i) {
            if (hi & 0x10) float_regs[first + i] = read_memory(parent, address + 4 * i, 4);
            else write_memory(parent, address + 4 * i, float_regs[first + i], 4);
        }
    } else if ((hi & 0xffb0) == 0xeea0 && (lo & 0xf7f) == 0xb10) { // VDUP.32 Q, Rt
        const unsigned first = (((lo >> 7) & 1) * 16 + rn) * 2;
        if ((first & 3) || first + 4 > 64 || rt == 15) return -1;
        for (unsigned i = 0; i < 4; ++i) float_regs[first + i] = regs[rt];
    } else if ((hi & 0xffb0) == 0xf900 && (lo & 0xf00) == 0xa00) { // VST1 {Dd,Dd+1}, [Rn]{!}
        const unsigned first = (((hi >> 6) & 1) * 16 + rt) * 2;
        const unsigned size = (lo >> 6) & 3, align = (lo >> 4) & 3, m = lo & 15;
        if (size == 3 || align != 0 || first + 4 > 64 || rn == 15) return -1;
        const uint32_t address = regs[rn];
        for (unsigned i = 0; i < 4; ++i) write_memory(parent, address + 4 * i, float_regs[first + i], 4);
        if (m != 15) regs[rn] += m == 13 ? 16 : regs[m];
    } else return -1;
    return 0;
}

int InterpreterCPU::arm(uint32_t op, uint32_t pc) {
    const unsigned cond = op >> 28;
    if (cond == 15) {
        if ((op & 0xfe000000) != 0xfa000000) return -1; // BLX immediate
        regs[14] = pc + 4;
        set_pc((pc + 8 + sign_extend(((op & 0xffffff) << 2) | ((op >> 23) & 2), 26)) | 1);
        return 0;
    }
    if (!condition(cond)) return 0;
    if ((op & 0x0ffffff0) == 0x012fff10 || (op & 0x0ffffff0) == 0x012fff30) {
        const uint32_t target = operand(op & 15, pc, false);
        if (op & 0x20) regs[14] = pc + 4;
        set_pc(target);
    } else if ((op & 0x0e000000) == 0x0a000000) {
        if (op & 0x1000000) regs[14] = pc + 4;
        regs[15] = pc + 8 + sign_extend((op & 0xffffff) << 2, 26);
    } else if ((op & 0x0f000000) == 0x0f000000) {
        parent->svc = op & 0xffffff; parent->svc_called = true;
    } else if ((op & 0x0fb00000) == 0x03000000) { // MOVW/MOVT import veneers
        const unsigned d = (op >> 12) & 15;
        if (d == 15) return -1;
        const uint32_t imm = ((op >> 4) & 0xf000) | (op & 0xfff);
        regs[d] = (op & 0x400000) ? (regs[d] & 65535) | (imm << 16) : imm;
    } else if ((op & 0x0c000000) == 0 && ((op & 0x02000000) || !(op & 0x10)) && (op & 0x01900000) != 0x01000000) {
        // Data processing, immediate or immediate-shifted register operand.
        const unsigned opcode = (op >> 21) & 15, n = (op >> 16) & 15, d = (op >> 12) & 15;
        const bool flags = op & 0x100000;
        if (d == 15 && flags) return -1; // exception return is outside this user-mode subset
        Shift b{};
        if (op & 0x02000000) {
            const unsigned rotation = ((op >> 8) & 15) * 2;
            b.value = std::rotr(uint32_t(op & 255), int(rotation));
            b.carry = rotation ? bool(b.value & n_flag) : bool(cpsr & c_flag);
        } else b = immediate_shift(operand(op & 15, pc, false), (op >> 5) & 3, (op >> 7) & 31, cpsr & c_flag);
        const uint32_t a = operand(n, pc, false);
        const bool carry = cpsr & c_flag;
        uint32_t value = 0;
        switch (opcode) {
        case 0: case 8: value = a & b.value; break; // AND, TST
        case 1: case 9: value = a ^ b.value; break; // EOR, TEQ
        case 2: case 10: value = add(a, ~b.value, true, flags); break; // SUB, CMP
        case 3: value = add(~a, b.value, true, flags); break; // RSB
        case 4: case 11: value = add(a, b.value, false, flags); break; // ADD, CMN
        case 5: value = add(a, b.value, carry, flags); break; // ADC
        case 6: value = add(a, ~b.value, carry, flags); break; // SBC
        case 7: value = add(~a, b.value, carry, flags); break; // RSC
        case 12: value = a | b.value; break; // ORR
        case 13: value = b.value; break; // MOV
        case 14: value = a & ~b.value; break; // BIC
        default: value = ~b.value; break; // MVN
        }
        const bool logical = !(opcode >= 2 && opcode <= 7) && opcode != 10 && opcode != 11;
        if (logical && flags) nzc(value, b.carry);
        if (opcode >= 8 && opcode <= 11) return 0; // TST, TEQ, CMP, CMN write no register
        if (d == 15) set_pc(value); else regs[d] = value; // ARMv7 ALUWritePC interworks
    } else if ((op & 0x0e500000) == 0x04100000) { // LDR word immediate
        const unsigned n = (op >> 16) & 15, d = (op >> 12) & 15;
        const bool pre = op & 0x1000000, up = op & 0x800000, wb = !pre || (op & 0x200000);
        if (wb && (n == 15 || n == d)) return -1;
        const uint32_t base = operand(n, pc, false), indexed = base + (up ? op & 0xfff : 0u - (op & 0xfff));
        const uint32_t value = read_memory(parent, pre ? indexed : base, 4);
        if (d == 15) set_pc(value); else regs[d] = value;
        if (wb) regs[n] = indexed;
    } else return -1;
    return 0;
}
bool InterpreterCPU::is_thumb_mode() { return (cpsr & t_flag) != 0; }
bool InterpreterCPU::hit_breakpoint() noexcept { return breakpoint; }
void InterpreterCPU::trigger_breakpoint() { breakpoint = true; stopped = true; }
void InterpreterCPU::set_log_code(bool value) { log_code = value; }
void InterpreterCPU::set_log_mem(bool value) { log_mem = value; }
bool InterpreterCPU::get_log_code() { return log_code; }
bool InterpreterCPU::get_log_mem() { return log_mem; }
void InterpreterCPU::clear_exclusive() noexcept {}
std::size_t InterpreterCPU::processor_id() const { return core_id; }
