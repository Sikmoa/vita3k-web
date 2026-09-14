// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone native fixture generator; run wasmjit_emitter_test.mjs afterward.
#include "../src/wasmjit/emit_wasm.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <vector>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/frontend/A32/translate/a32_translate.h>
#include <dynarmic/frontend/A32/translate/translate_callbacks.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/ir/opcodes.h>

#define CHECK(expr) do { if (!(expr)) { \
    std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " #expr << '\n'; \
    std::abort(); \
} } while (false)

namespace {
namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;
using vita3k::wasmjit::JitState;
using vita3k::wasmjit::emit_block;
using A32::Reg;
using IR::Cond;
using IR::Opcode;
using IR::Value;

A32::LocationDescriptor loc(bool thumb = false, uint32_t pc = 0x1000) {
    return {pc, A32::PSR{thumb ? 0x30u : 0x10u}, A32::FPSCR{0}};
}

IR::Block blank() {
    IR::Block block{loc()};
    block.SetEndLocation(loc(false, 0x1004));
    block.SetTerminal(IR::Term::LinkBlock{loc(false, 0x1004)});
    block.CycleCount() = 1;
    return block;
}

Value append(IR::Block &block, Opcode op, std::initializer_list<Value> args) {
    block.AppendNewInst(op, args);
    return Value{&block.back()};
}

Value reg(IR::Block &block, Reg r) { return append(block, Opcode::A32GetRegister, {Value{r}}); }
void set(IR::Block &block, Reg r, Value v) { append(block, Opcode::A32SetRegister, {Value{r}, v}); }

JitState initial(bool thumb = false) {
    JitState state{};
    for (uint32_t i = 0; i < 16; ++i)
        state.regs[i] = 0xdead0000 + i;
    state.regs[15] = 0x1000;
    state.cpsr = thumb ? 0x080f0030 : 0x080f0010; // Q and GE must survive NZCV writes
    state.fpscr = 0xa000001f; // mode bits match descriptor, other bits preserved
    state.svc = 0xbad;
    state.exit_reason = 99;
    state.executed = 99;
    return state;
}

JitState next(JitState state, uint32_t count = 1, uint32_t pc = 0x1004) {
    state.regs[15] = pc;
    state.executed = count;
    state.exit_reason = 0;
    state.svc = 0;
    return state;
}

struct Case {
    JitState input, expected;
    // Guest-memory expectations for the JS harness's linear memory (little-
    // endian words): `pre` seeds words before execution (loads), `mem`
    // verifies words after execution (stores). Address -> word.
    std::map<uint32_t, uint32_t> pre, mem;
    Case(const JitState &in, const JitState &out) : input(in), expected(out) {}
    Case() = default;
};

void json_state(std::ostream &os, const JitState &s) {
    os << '[';
    for (const auto r : s.regs)
        os << r << ',';
    os << s.cpsr << ',' << s.fpscr << ',' << s.svc << ',' << s.exit_reason << ',' << s.executed;
    os << ',' << s.memory_cookie << ',' << s.fault_address << ',' << s.fault_write;
    for (auto v : s.memory_value) os << ',' << v;
    for (auto v : s.fpu) os << ',' << v;
    os << ',' << s.tpidruro << ']';
}

class Suite {
public:
    explicit Suite(std::filesystem::path path) : path(std::move(path)) {
        std::filesystem::create_directories(this->path);
        manifest.open(this->path / "cases.json");
        manifest << '[';
    }
    ~Suite() { manifest << "]\n"; }

    void add(const std::string &name, const IR::Block &block, const std::vector<Case> &cases) {
        const auto bytes = emit_block(block);
        if (bytes.empty()) {
            std::cerr << "Unexpected rejection: " << name << '\n' << IR::DumpBlock(block);
            std::abort();
        }
        CHECK(bytes == emit_block(block)); // deterministic, no IR mutation
        std::ofstream wasm(path / (name + ".wasm"), std::ios::binary);
        wasm.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        CHECK(wasm.good());
        if (modules++) manifest << ',';
        manifest << "{\"name\":\"" << name << "\",\"cases\":[";
        bool first = true;
        for (const auto &test : cases) {
            if (!first) manifest << ',';
            first = false;
            manifest << "{\"in\":";
            json_state(manifest, test.input);
            manifest << ",\"out\":";
            json_state(manifest, test.expected);
            const auto words = [&](const char *name, const std::map<uint32_t, uint32_t> &region) {
                if (region.empty()) return;
                manifest << ",\"" << name << "\":{";
                bool first_word = true;
                for (const auto &[address, word] : region) {
                    if (!first_word) manifest << ',';
                    first_word = false;
                    manifest << '"' << address << "\":" << word;
                }
                manifest << '}';
            };
            words("pre", test.pre);
            words("mem", test.mem);
            manifest << '}';
            ++runs;
        }
        manifest << "]}";
    }

    size_t modules = 0, runs = 0;
private:
    std::filesystem::path path;
    std::ofstream manifest;
};

uint32_t nz(uint32_t r) { return (r & 0x80000000) | (r == 0 ? 0x40000000 : 0); }
int64_t signed32(uint32_t n) { return n <= INT32_MAX ? int64_t(n) : int64_t(n) - (int64_t(1) << 32); }

void arithmetic(Suite &suite) {
    std::mt19937 rng(0x12345678);
    for (const bool sub : {false, true}) {
        auto block = blank();
        const auto a = reg(block, Reg::R0), b = reg(block, Reg::R1);
        const auto c = append(block, Opcode::A32GetCFlag, {});
        const auto result = append(block, sub ? Opcode::Sub32 : Opcode::Add32, {a, b, c});
        set(block, Reg::R0, result); // pseudos must still use original input values!
        set(block, Reg::R2, append(block, Opcode::ZeroExtendByteToWord, {
            append(block, Opcode::LeastSignificantByte, {result})}));
        const auto carry = append(block, Opcode::GetCarryFromOp, {result});
        const auto overflow = append(block, Opcode::GetOverflowFromOp, {result});
        // U1 cannot be assigned to a U32 register; select produces the U32.
        set(block, Reg::R3, append(block, Opcode::LogicalShiftLeft32, {Value{uint32_t(1)}, Value{uint8_t(0)}, carry}));
        append(block, Opcode::A32SetCpsrNZC, {append(block, Opcode::GetNZFromOp, {result}), carry});
        set(block, Reg::R4, append(block, Opcode::A32GetCpsr, {}));
        append(block, Opcode::A32SetCpsrNZC, {append(block, Opcode::GetNZFromOp, {result}), overflow});
        set(block, Reg::R5, append(block, Opcode::A32GetCpsr, {}));
        append(block, Opcode::A32SetCpsrNZCV, {append(block, Opcode::GetNZCVFromOp, {result})});
        std::vector<Case> cases;
        const std::array<uint32_t, 10> edges{0, 1, 2, 0x7ffffffe, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff, 0x12345678};
        const auto test = [&](uint32_t a, uint32_t b, uint32_t c) {
            auto in = initial();
            in.regs[0] = a; in.regs[1] = b; in.cpsr |= (c << 29) | 0x10000000;
            auto out = next(in);
            const uint64_t wide = sub ? uint64_t(a) + uint32_t(~b) + c : uint64_t(a) + b + c;
            const uint32_t r = static_cast<uint32_t>(wide);
            const uint32_t carry = wide >> 32;
            const int64_t signed_result = sub ? signed32(a) - signed32(b) - (1 - c) : signed32(a) + signed32(b) + c;
            const uint32_t overflow = signed_result < INT32_MIN || signed_result > INT32_MAX;
            out.regs[0] = r; out.regs[2] = r & 0xff; out.regs[3] = 1;
            out.regs[4] = (in.cpsr & 0x1fffffff) | nz(r) | (carry << 29);
            out.regs[5] = (in.cpsr & 0x1fffffff) | nz(r) | (overflow << 29);
            out.cpsr = (in.cpsr & 0x0fffffff) | nz(r) | (carry << 29) | (overflow << 28);
            cases.push_back({in, out});
        };
        for (const auto a : edges) for (const auto b : edges) for (uint32_t c = 0; c < 2; ++c) test(a, b, c);
        for (unsigned i = 0; i < 2000; ++i) test(rng(), rng(), rng() & 1);
        suite.add(sub ? "sub" : "add", block, cases);
    }
}

void shifts(Suite &suite) {
    const std::array<Opcode, 5> ops{Opcode::LogicalShiftLeft32, Opcode::LogicalShiftRight32, Opcode::ArithmeticShiftRight32, Opcode::RotateRight32, Opcode::RotateRightExtended};
    for (size_t k = 0; k < ops.size(); ++k) {
        auto block = blank();
        const auto a = reg(block, Reg::R0);
        const auto n = append(block, Opcode::LeastSignificantByte, {reg(block, Reg::R1)});
        const auto c = append(block, Opcode::A32GetCFlag, {});
        const auto r = k == 4 ? append(block, ops[k], {a, c}) : append(block, ops[k], {a, n, c});
        set(block, Reg::R0, r);
        append(block, Opcode::A32SetCpsrNZC, {append(block, Opcode::GetNZFromOp, {r}), append(block, Opcode::GetCarryFromOp, {r})});
        std::vector<Case> cases;
        for (uint32_t a : {0u, 1u, 0x80000000u, 0x80000001u, 0xffffffffu, 0x12345678u}) {
            for (uint32_t n = 0; n <= 256; ++n) {
                for (uint32_t c = 0; c < 2; ++c) {
                    auto in = initial();
                    in.regs[0] = a; in.regs[1] = n; in.cpsr |= (c << 29) | 0x10000000;
                    auto out = next(in);
                    const uint32_t amount = n & 0xff;
                    uint32_t r = a, carry = c;
                    if (k == 4) { r = (a >> 1) | (c << 31); carry = a & 1; }
                    else if (amount != 0) {
                        if (k == 0) { r = amount < 32 ? a << amount : 0; carry = amount <= 32 ? (a >> (32 - amount)) & 1 : 0; }
                        if (k == 1) { r = amount < 32 ? a >> amount : 0; carry = amount <= 32 ? (a >> (amount - 1)) & 1 : 0; }
                        if (k == 2) {
                            const uint32_t s = std::min(amount, 31u);
                            r = a >> s;
                            if (a & 0x80000000) r |= ~(0xffffffffu >> s);
                            carry = (a >> std::min(amount - 1, 31u)) & 1;
                        }
                        if (k == 3) {
                            const auto s = amount % 32;
                            r = s ? (a >> s) | (a << (32 - s)) : a;
                            carry = r >> 31;
                        }
                    }
                    out.regs[0] = r;
                    out.cpsr = (in.cpsr & 0x1fffffff) | nz(r) | (carry << 29);
                    cases.push_back({in, out});
                }
            }
        }
        suite.add("shift" + std::to_string(k), block, cases);
    }
}

bool passes(unsigned cond, uint32_t flags) {
    const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
    const bool table[]{z, !z, c, !c, n, !n, v, !v, c && !z, !c || z, n == v, n != v, !z && n == v, z || n != v, true};
    return table[cond];
}

void conditions(Suite &suite) {
    for (unsigned cond = 0; cond < 15; ++cond) {
        for (bool entry : {false, true}) {
            auto block = blank();
            if (entry) {
                block.SetCondition(static_cast<Cond>(cond));
                block.SetConditionFailedLocation(loc(false, 0x1008));
                block.ConditionFailedCycleCount() = 2;
                block.CycleCount() = 3;
                set(block, Reg::R0, Value{uint32_t(42)});
                // Entry condition must not be re-evaluated after flags change.
                append(block, Opcode::A32SetCpsrNZCVRaw, {Value{uint32_t(0)}});
            } else {
                block.ReplaceTerminal(IR::Term::If{static_cast<Cond>(cond),
                    IR::Term::LinkBlock{loc(false, 0x1004)}, IR::Term::LinkBlockFast{loc(false, 0x1008)}});
            }
            std::vector<Case> cases;
            for (uint32_t flags = 0; flags < 16; ++flags) {
                auto in = initial(); in.cpsr |= flags << 28;
                const bool pass = passes(cond, flags);
                auto out = next(in, entry ? pass ? 3 : 2 : 1, pass ? 0x1004 : 0x1008);
                if (entry && pass) { out.regs[0] = 42; out.cpsr &= 0x0fffffff; }
                cases.push_back({in, out});
            }
            suite.add(std::string(entry ? "entry" : "term") + std::to_string(cond), block, cases);
        }
    }
}

void scalars(Suite &suite) {
    auto block = blank();
    append(block, Opcode::Void, {});
    const auto a = reg(block, Reg::R0), b = reg(block, Reg::R1);
    const auto identity = append(block, Opcode::Identity, {a});
    set(block, Reg::R2, append(block, Opcode::And32, {identity, b}));
    set(block, Reg::R3, append(block, Opcode::Or32, {a, b}));
    set(block, Reg::R4, append(block, Opcode::Eor32, {a, b}));
    set(block, Reg::R5, append(block, Opcode::Not32, {a}));
    set(block, Reg::R6, append(block, Opcode::AndNot32, {a, b}));
    set(block, Reg::R7, append(block, Opcode::ZeroExtendHalfToWord, {append(block, Opcode::LeastSignificantHalf, {a})}));
    set(block, Reg::R8, append(block, Opcode::RotateRightExtended, {Value{uint32_t(0)}, append(block, Opcode::MostSignificantBit, {a})}));
    set(block, Reg::R9, append(block, Opcode::RotateRightExtended, {Value{uint32_t(0)}, append(block, Opcode::IsZero32, {a})}));
    const auto flags = append(block, Opcode::NZCVFromPackedFlags, {a});
    append(block, Opcode::A32SetCpsrNZCV, {flags});
    set(block, Reg::R10, append(block, Opcode::RotateRightExtended, {Value{uint32_t(0)}, append(block, Opcode::GetCFlagFromNZCV, {flags})}));
    set(block, Reg::R11, append(block, Opcode::ConditionalSelect32, {Value{Cond::HI}, a, b}));
    const auto other = append(block, Opcode::NZCVFromPackedFlags, {b});
    append(block, Opcode::A32SetCpsrNZCV, {append(block, Opcode::ConditionalSelectNZCV, {Value{Cond::HI}, flags, other})});
    const auto immediate_identity = append(block, Opcode::Identity, {Value{uint32_t(0xfedcba98)}});
    set(block, Reg::R12, immediate_identity);
    std::vector<Case> cases;
    for (uint32_t a : {0u, 1u, 0xffffffffu, 0x80000000u, 0x20000000u, 0x60000000u, 0x12345678u}) {
        auto in = initial(); in.regs[0] = a; in.regs[1] = 0xa5a5a5a5;
        auto out = next(in);
        const auto b = in.regs[1];
        out.regs[2] = a & b; out.regs[3] = a | b; out.regs[4] = a ^ b;
        out.regs[5] = ~a; out.regs[6] = a & ~b; out.regs[7] = a & 0xffff;
        out.regs[8] = a & 0x80000000; out.regs[9] = a == 0 ? 0x80000000 : 0;
        out.regs[10] = (a & 0x20000000) << 2;
        const bool hi = (a & 0x20000000) && !(a & 0x40000000);
        out.regs[11] = hi ? a : b; out.regs[12] = 0xfedcba98;
        out.cpsr = (in.cpsr & 0x0fffffff) | ((hi ? a : b) & 0xf0000000);
        cases.push_back({in, out});
    }
    suite.add("scalars", block, cases);
    auto large = blank();
    for (unsigned i = 0; i < 4096; ++i) append(large, Opcode::Void, {});
    const auto in = initial();
    suite.add("local_limit", large, {{in, next(in)}});
    auto exchange = blank();
    append(exchange, Opcode::A32BXWritePC, {Value{uint32_t(0x2001)}});
    append(exchange, Opcode::A32UpdateUpperLocationDescriptor, {});
    exchange.ReplaceTerminal(IR::Term::ReturnToDispatch{});
    auto out = next(in, 1, 0x2000); out.cpsr |= 0x20;
    suite.add("bx_late_upper", exchange, {{in, out}});
}

struct Code final : A32::TranslateCallbacks {
    std::vector<uint8_t> bytes;
    explicit Code(const std::vector<uint32_t> &code, bool thumb) {
        for (auto instruction : code) {
            for (unsigned i = 0; i < (thumb ? 2 : 4); ++i)
                bytes.push_back(static_cast<uint8_t>(instruction >> (8 * i)));
        }
    }
    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        if (address < 0x1000 || address >= 0x1000 + bytes.size()) return {};
        uint32_t word = 0;
        for (unsigned i = 0; i < 4 && address - 0x1000 + i < bytes.size(); ++i)
            word |= uint32_t(bytes[address - 0x1000 + i]) << (8 * i);
        return word;
    }
    bool PreCodeReadHook(bool, uint32_t pc, A32::IREmitter &ir) override {
        if (pc < 0x1000 + bytes.size()) return true;
        ir.SetTerm(IR::Term::LinkBlock{ir.current_location});
        return false;
    }
    void PreCodeTranslationHook(bool, uint32_t, A32::IREmitter &) override {}
    uint64_t GetTicksForCode(bool, uint32_t, uint32_t) override { return 1; }
};

IR::Block translate(const std::vector<uint32_t> &instructions, bool thumb) {
    Code code(instructions, thumb);
    return A32::Translate(loc(thumb), &code, {A32::ArchVersion::v7, false, false});
}

void frontend(Suite &suite) {
    for (bool thumb : {false, true}) {
        // MOV r0,#1; ADD r0,#2; SUB r0,#1; CMP r0,#2; BNE .+4
        auto block = translate(thumb ? std::vector<uint32_t>{0x2001, 0x3002, 0x3801, 0x2802, 0xd100}
                                     : std::vector<uint32_t>{0xe3a00001, 0xe2800002, 0xe2400001, 0xe3500002, 0x1a000000}, thumb);
        auto in = initial(thumb), out = next(in, thumb ? 5 : 4, thumb ? 0x100a : 0x1010);
        // A32 frontend splits before conditional BNE after the preceding body.
        out.regs[0] = 2; out.cpsr |= 0x60000000;
        suite.add(thumb ? "thumb_program" : "arm_program", block, {{in, out}});
        auto branch = translate(thumb ? std::vector<uint32_t>{0xd100} : std::vector<uint32_t>{0x1a000000}, thumb);
        std::vector<Case> branches;
        for (bool z : {false, true}) {
            in = initial(thumb); if (z) in.cpsr |= 0x40000000;
            out = next(in, 1, thumb ? z ? 0x1002 : 0x1004 : z ? 0x1004 : 0x1008);
            branches.push_back({in, out});
        }
        suite.add(thumb ? "thumb_bne" : "arm_bne", branch, branches);
        auto svc = translate(thumb ? std::vector<uint32_t>{0xdfab} : std::vector<uint32_t>{0xef123456}, thumb);
        in = initial(thumb); out = next(in, 1, thumb ? 0x1002 : 0x1004);
        out.svc = thumb ? 0xab : 0x123456; out.exit_reason = 1;
        suite.add(thumb ? "thumb_svc" : "arm_svc", svc, {{in, out}});
        auto bx = translate(thumb ? std::vector<uint32_t>{0x4770} : std::vector<uint32_t>{0xe12fff1e}, thumb);
        std::vector<Case> exchanges;
        for (uint32_t target : {0x2001u, 0x2002u, 0x2003u, 0xfffffffdu}) {
            in = initial(thumb); in.regs[14] = target;
            out = next(in, 1, target & ((target & 1) ? 0xfffffffeu : 0xfffffffcu));
            out.cpsr = (in.cpsr & ~0x20u) | ((target & 1) << 5);
            exchanges.push_back({in, out});
        }
        suite.add(thumb ? "thumb_bx" : "arm_bx", bx, exchanges);
    }
        auto movne = translate({0x13a0002a}, false);
    auto in = initial(), pass = next(in); pass.regs[0] = 42;
    auto failin = in; failin.cpsr |= 0x40000000;
    suite.add("arm_movne", movne, {{in, pass}, {failin, next(failin)}});
    // Unoptimized register MOV emits a zero-count shift and a carry pseudo.
    auto movs = translate({0xe1b00001}, false);
    in = initial(); in.regs[1] = 0x80000000; in.cpsr |= 0x30000000;
    auto out = next(in); out.regs[0] = in.regs[1]; out.cpsr |= 0x80000000;
    suite.add("arm_movs_reg", movs, {{in, out}});
}

// The M14b stall: the fixture's NEON memset loop (VitaSDK libc, Thumb).
// vdup.32 q8,lr fills the stored vector; vst1.32 {d16-d17},[ip]! writes 16
// bytes and post-increments ip; cmp r3,ip + bne close the loop. The runtime
// splits memory instructions into single-instruction blocks; mirror that.
void vector_loop(Suite &suite) {
    // vdup.32 q8, lr: broadcast into all four Q8 lanes (fpu words 32..35).
    auto dup_q = translate({0xeea0, 0xeb90}, true);
    {
        auto in = initial(true);
        in.regs[14] = 0x81000e36;
        auto out = next(in, 1, 0x1004);
        for (unsigned i = 0; i < 4; ++i) out.fpu[32 + i] = 0x81000e36;
        suite.add("thumb_vdup32_q8_lr", dup_q, {{in, out}});
    }
    // vdup.32 d16, lr: a D write must not touch its neighbour's words.
    auto dup_d = translate({0xee80, 0xeb90}, true);
    {
        auto in = initial(true);
        in.regs[14] = 0x13579bdf;
        in.fpu[34] = 0x0bad0bad; // d17 low word must survive the d16 write
        in.fpu[35] = 0xf00dbee0; // d17 high word
        auto out = next(in, 1, 0x1004);
        out.fpu[32] = out.fpu[33] = 0x13579bdf;
        suite.add("thumb_vdup32_d16_lr", dup_d, {{in, out}});
    }
    // vst1.32 {d16-d17},[ip]!: four 32-bit element stores at ip+0,4,8,12
    // followed by the writeback ip += 16. Data must land in guest memory at
    // the addressed buffer, never at the block's own code, and every lane
    // (both halves of both D registers) must be preserved.
    auto store = translate({0xf94c, 0x0a8d}, true);
    {
        auto in = initial(true);
        in.regs[12] = 0x3000; // guest buffer, clear of both state offsets
        in.fpu[32] = 0x11112222; in.fpu[33] = 0x33334444; // d16 low/high
        in.fpu[34] = 0x55556666; in.fpu[35] = 0x77778888; // d17 low/high
        auto out = next(in, 1, 0x1004);
        out.regs[12] = 0x3010; // post-increment writeback: 8 * nelem * regs
        out.memory_value[0] = 0x77778888; // last stored element
        Case store_case{in, out};
        store_case.mem = {{0x3000, 0x11112222}, {0x3004, 0x33334444},
            {0x3008, 0x55556666}, {0x300c, 0x77778888}};
        suite.add("thumb_vst1_postinc", store, {store_case});
    }
    // cmp r3,ip; bne back to 0x1000: the loop terminator taken/not-taken.
    auto branch = translate({0x4563, 0xd1fd}, true);
    std::vector<Case> cases;
    for (bool equal : {false, true}) {
        auto in = initial(true);
        in.regs[3] = 0x3010;
        in.regs[12] = equal ? 0x3010 : 0x3000;
        auto out = next(in, 2, equal ? 0x1004 : 0x1000);
        // cmp r3,ip: no borrow (C=1); Z only when equal; V/N clear here.
        out.cpsr = (in.cpsr & 0x0fffffff) | (equal ? 0x60000000u : 0x20000000u);
        cases.push_back({in, out});
    }
    suite.add("thumb_memset_branch", branch, cases);
}

// The fixture's VFPv3 save/restore and 64-bit store sites, from the same
// IR-coverage run: vpush/vpop split into two 32-bit helper stores/loads via
// GetExtendedRegister64's LeastSignificantWord/MostSignificantWord words,
// while STRD lowers to a single 8-byte WriteMemory64 of a packed U64. All
// encodings are the fixture's own bytes (0x81000c1c..0x81000e68).
void vfp_memory(Suite &suite) {
    // vpush {d8}: sp -= 8, then d8's two words stored at [sp] and [sp+4].
    auto vpush = translate({0xed2d, 0x8b02}, true);
    {
        auto in = initial(true);
        in.regs[13] = 0x2000;
        in.fpu[16] = 0x9e3779b9; // d8 low
        in.fpu[17] = 0x0badc0de; // d8 high
        auto out = next(in, 1, 0x1004);
        out.regs[13] = 0x1ff8;
        out.memory_value[0] = 0x0badc0de; // last published store word
        Case push_case{in, out};
        push_case.mem = {{0x1ff8, 0x9e3779b9}, {0x1ffc, 0x0badc0de}};
        suite.add("thumb_vpush_d8", vpush, {push_case});
    }
    // vldr d8,[pc,#140]: two reads at Align(PC,4)+4+140 = 0x1090/0x1094,
    // packed little-endian into d8. Exercises the read side of the helper.
    auto vldr = translate({0xed9f, 0x8b23}, true);
    {
        auto in = initial(true);
        auto out = next(in, 1, 0x1004);
        out.fpu[16] = 0x11223344;
        out.fpu[17] = 0x55667788;
        out.memory_value[0] = 0x55667788; // last read; helper zeroes the rest
        Case load_case{in, out};
        load_case.pre = {{0x1090, 0x11223344}, {0x1094, 0x55667788}};
        suite.add("thumb_vldr_d8_pc140", vldr, {load_case});
    }
    // vstr d8,[r4,#176]: d8's words stored at [r4+0xb0] and [r4+0xb4].
    auto vstr = translate({0xed84, 0x8b2c}, true);
    {
        auto in = initial(true);
        in.regs[4] = 0x2000;
        in.fpu[16] = 0xcafebabe;
        in.fpu[17] = 0x12345678;
        auto out = next(in, 1, 0x1004);
        out.memory_value[0] = 0x12345678;
        Case store_case{in, out};
        store_case.mem = {{0x20b0, 0xcafebabe}, {0x20b4, 0x12345678}};
        suite.add("thumb_vstr_d8_r4_176", vstr, {store_case});
    }
    // strd r5,r9,[r4,#20]: a single 8-byte WriteMemory64 publishes both
    // memory_value words; the helper must consume them across the words.
    auto strd = translate({0xe9c4, 0x5905}, true);
    {
        auto in = initial(true);
        in.regs[4] = 0x2100;
        in.regs[5] = 0x0badf00d;
        in.regs[9] = 0x0d15ea5e;
        auto out = next(in, 1, 0x1004);
        out.memory_value[0] = 0x0badf00d;
        out.memory_value[1] = 0x0d15ea5e;
        Case strd_case{in, out};
        strd_case.mem = {{0x2114, 0x0badf00d}, {0x2118, 0x0d15ea5e}};
        suite.add("thumb_strd_r5_r9_r4_20", strd, {strd_case});
    }
    // vpop {d8}: reads use the original sp, then sp += 8.
    auto vpop = translate({0xecbd, 0x8b02}, true);
    {
        auto in = initial(true);
        in.regs[13] = 0x2200;
        auto out = next(in, 1, 0x1004);
        out.regs[13] = 0x2208;
        out.fpu[16] = 0x31415926;
        out.fpu[17] = 0x27182818;
        out.memory_value[0] = 0x27182818;
        Case pop_case{in, out};
        pop_case.pre = {{0x2200, 0x31415926}, {0x2204, 0x27182818}};
        suite.add("thumb_vpop_d8", vpop, {pop_case});
    }
    // vpop {d8-d11}: four D registers restored from [sp..sp+0x1f], sp += 0x20.
    auto vpop4 = translate({0xecbd, 0x8b08}, true);
    {
        auto in = initial(true);
        in.regs[13] = 0x2300;
        auto out = next(in, 1, 0x1004);
        out.regs[13] = 0x2320;
        const std::array<uint32_t, 8> words{
            0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f,
            0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f};
        for (unsigned i = 0; i < 8; ++i) out.fpu[16 + i] = words[i]; // d8..d11
        out.memory_value[0] = words[7];
        Case pop4_case{in, out};
        for (unsigned i = 0; i < 8; ++i) pop4_case.pre[0x2300 + 4 * i] = words[i];
        suite.add("thumb_vpop_d8_d11", vpop4, {pop4_case});
    }
}


void most_significant_word(Suite &suite) {
    auto block = blank();
    const auto packed = append(block, Opcode::Pack2x32To1x64,
        {reg(block, Reg::R0), reg(block, Reg::R1)});
    const auto high = append(block, Opcode::MostSignificantWord, {packed});
    const auto carry = append(block, Opcode::GetCarryFromOp, {high});
    set(block, Reg::R2, high);
    append(block, Opcode::A32SetCpsrNZC,
        {append(block, Opcode::GetNZFromOp, {high}), carry});
    std::vector<Case> cases;
    for (const uint32_t lo : {0u, 0x80000000u}) {
        for (const uint32_t hi : {0u, 1u, 0x80000000u, 0xffffffffu}) {
            auto in = initial();
            in.regs[0] = lo;
            in.regs[1] = hi;
            in.cpsr |= 0x10000000;
            auto out = next(in);
            out.regs[2] = hi;
            out.cpsr = (in.cpsr & 0x1fffffff) | nz(hi) | ((lo >> 31) << 29);
            cases.push_back({in, out});
        }
    }
    suite.add("most_significant_word_carry", block, cases);
}

void rejects() {
    auto reject = [](const IR::Block &b) { CHECK(emit_block(b).empty()); };
    auto block = blank(); append(block, Opcode::Breakpoint, {}); reject(block);
    block = blank(); append(block, Opcode::A32GetFpscr, {}); reject(block);
    block = blank(); block.ReplaceTerminal(IR::Term::Interpret{loc()}); reject(block);
    block = blank(); block.ReplaceTerminal(IR::Term::Invalid{}); reject(block);
    block = blank(); block.ReplaceTerminal(IR::Term::ReturnToDispatch{}); reject(block);
    block = blank(); block.ReplaceTerminal(IR::Term::CheckHalt{IR::Term::LinkBlock{loc()}}); reject(block);
    block = blank(); block.ReplaceTerminal(IR::Term::CheckBit{IR::Term::LinkBlock{loc()}, IR::Term::LinkBlock{loc()}}); reject(block);
    block = blank(); block.ReplaceTerminal(IR::Term::If{Cond::AL, IR::Term::LinkBlock{loc()}, IR::Term::Interpret{loc()}}); reject(block);
    block = blank(); block.ReplaceTerminal(IR::Term::LinkBlock{loc(false, 0x1001)}); reject(block);
    block = blank(); block.ReplaceTerminal(IR::Term::LinkBlock{loc().SetFPSCR(0x01000000)}); reject(block);
    block = blank(); block.SetCondition(Cond::NV); block.SetConditionFailedLocation(loc()); block.ConditionFailedCycleCount() = 1; reject(block);
    block = blank(); block.SetCondition(Cond::EQ); reject(block); // missing failed location
    block = blank(); block.CycleCount() = 4097; reject(block);
    block = blank(); block.CycleCount() = 0; reject(block);
    block = blank(); for (unsigned i = 0; i < 4097; ++i) append(block, Opcode::Void, {}); reject(block);
    block = blank(); block.SetEndLocation(loc().SetIT(A32::ITState{0x18})); reject(block);
    block = blank(); append(block, Opcode::A32CallSupervisor, {Value{uint32_t(1)}}); reject(block);
    block = translate({0xef000000}, false); append(block, Opcode::Void, {}); reject(block);
    block = translate({0xe5900000}, false); // actual LDR memory IR is helper-backed
    block = translate({0xe7f000f0}, false); reject(block); // actual UDF exception IR
    block = blank();
    const auto x = append(block, Opcode::And32, {Value{uint32_t(1)}, Value{uint32_t(2)}});
    append(block, Opcode::GetCarryFromOp, {x}); reject(block); // invalid pseudo producer
    block = blank();
    IR::Terminal terminal = IR::Term::LinkBlock{loc()};
    for (unsigned i = 0; i < 18; ++i) terminal = IR::Term::If{Cond::NE, terminal, IR::Term::LinkBlock{loc()}};
    block.ReplaceTerminal(terminal); reject(block);
    // Vector/D-register selection must fail closed: RegNumber alone cannot
    // distinguish S/D/Q, so anything but an explicit D (for 64-bit access)
    // or D/Q (for vector access) must be rejected, never mis-lowered.
    block = blank(); append(block, Opcode::A32GetVector, {Value{A32::ExtReg::S0}}); reject(block);
    block = blank(); append(block, Opcode::A32GetExtendedRegister64, {Value{A32::ExtReg::S1}}); reject(block);
    block = blank(); append(block, Opcode::A32GetExtendedRegister64, {Value{A32::ExtReg::Q0}}); reject(block);
    block = blank();
    {
        const auto vec = append(block, Opcode::VectorBroadcast32, {Value{uint32_t(1)}});
        append(block, Opcode::A32SetVector, {Value{A32::ExtReg::S0}, vec}); reject(block);
    }
    block = blank();
    {
        const auto packed = append(block, Opcode::Pack2x32To1x64, {Value{uint32_t(1)}, Value{uint32_t(2)}});
        append(block, Opcode::A32SetExtendedRegister64, {Value{A32::ExtReg::S2}, packed}); reject(block);
    }
    block = blank();
    {
        const auto packed = append(block, Opcode::Pack2x32To1x64, {Value{uint32_t(1)}, Value{uint32_t(2)}});
        append(block, Opcode::A32SetExtendedRegister64, {Value{A32::ExtReg::Q1}, packed}); reject(block);
    }
}
} // namespace

int main(int argc, char **argv) {
    CHECK(argc == 2);
    rejects();
    Suite suite{argv[1]};
    arithmetic(suite);
    shifts(suite);
    conditions(suite);
    scalars(suite);
    frontend(suite);
    vector_loop(suite);
    vfp_memory(suite);
    most_significant_word(suite);
    std::cout << "Native rejection/determinism checks passed; generated " << suite.modules << " modules and " << suite.runs << " execution cases\n";
}
