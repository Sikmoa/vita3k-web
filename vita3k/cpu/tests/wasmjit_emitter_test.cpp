// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone native fixture generator; run wasmjit_emitter_test.mjs afterward.
#include "../src/wasmjit/emit_wasm.h"

#include <array>
#include <bit>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
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
    const auto words = std::bit_cast<std::array<uint32_t, sizeof(JitState) / sizeof(uint32_t)>>(s);
    os << '[';
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) os << ',';
        os << words[i];
    }
    os << ']';
}

class Suite {
public:
    explicit Suite(std::filesystem::path path) : path(std::move(path)) {
        std::filesystem::create_directories(this->path);
        manifest.open(this->path / "cases.json");
        manifest << '[';
    }
    ~Suite() { manifest << "]\n"; }

    void add(const std::string &name, const IR::Block &block, const std::vector<Case> &cases,
        std::optional<uint32_t> region_budget = std::nullopt, bool differential = false) {
        const auto emit = [&](vita3k::wasmjit::RegionStateOptions options = {}) {
            if (!region_budget) return emit_block(block);
            const A32::LocationDescriptor at(block.Location());
            return vita3k::wasmjit::emit_region({&block}, {{at.PC(),
                A32::LocationDescriptor::CPSR_MODE_MASK,
                at.CPSR().Value() & A32::LocationDescriptor::CPSR_MODE_MASK,
                static_cast<uint32_t>(block.CycleCount() + block.ConditionFailedCycleCount())}}, options);
        };
        const auto bytes = emit();
        if (bytes.empty()) {
            std::cerr << "Unexpected rejection: " << name << '\n' << IR::DumpBlock(block);
            std::abort();
        }
        CHECK(bytes == emit()); // deterministic, no IR mutation
        std::ofstream wasm(path / (name + ".wasm"), std::ios::binary);
        wasm.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        CHECK(wasm.good());
        if (modules++) manifest << ',';
        manifest << "{\"name\":\"" << name << '"';
        if (region_budget) manifest << ",\"budget\":" << *region_budget;
        if (differential) manifest << ",\"differential\":true";
        if (region_budget) {
            manifest << ",\"variants\":[";
            bool first_variant = true;
            for (unsigned mode = 1; mode <= 3; ++mode) {
                for (unsigned fast = 0; fast <= 1; ++fast) {
                    vita3k::wasmjit::RegionStateOptions options{(mode & 1) != 0, (mode & 2) != 0};
                    options.assume_fast_bases = fast != 0;
                    const auto variant = emit(options);
                    CHECK(!variant.empty() && variant == emit(options));
                    const auto filename = name + (mode == 1 ? "_P" : mode == 2 ? "_K" : "_PK")
                        + (fast ? "_F" : "") + ".wasm";
                    std::ofstream output(path / filename, std::ios::binary);
                    output.write(reinterpret_cast<const char *>(variant.data()), variant.size());
                    CHECK(output.good());
                    ++candidate_modules;
                    if (!first_variant) manifest << ',';
                    first_variant = false;
                    manifest << '"' << filename << '"';
                }
            }
            manifest << ']';
        }
        manifest << ",\"cases\":[";
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
        // Exercise the same actual IR and input corpus as a one-member region.
        // Region accounting/exit ABI differs from block(), so these additional
        // cases compare against reference run(), not block()'s expected bytes.
        if (!region_budget)
            add(name + "_region", block, cases,
                static_cast<uint32_t>(block.CycleCount() + block.ConditionFailedCycleCount()), true);
    }

    size_t modules = 0, candidate_modules = 0, runs = 0;
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

void shifts_imm(Suite &suite) {
    // Constant-count specialization (R3h): immediate U8 amounts for the four
    // counted shift ops, across every lowering class (0, 1..31, 32, 33+),
    // each with carry consumed and carry dead (R3d interaction). Expected
    // values mirror shifts() above.
    const std::array<Opcode, 4> ops{Opcode::LogicalShiftLeft32, Opcode::LogicalShiftRight32,
        Opcode::ArithmeticShiftRight32, Opcode::RotateRight32};
    for (size_t k = 0; k < ops.size(); ++k) {
        for (const bool carry_live : {false, true}) {
            // One block per amount (immediates are per-instruction).
            for (uint32_t amount : {0u, 1u, 2u, 5u, 8u, 16u, 31u, 32u, 33u, 64u, 255u}) {
                auto block = blank();
                const auto a = reg(block, Reg::R0);
                const auto c = append(block, Opcode::A32GetCFlag, {});
                const auto r = append(block, ops[k], {a, Value{uint8_t(amount)}, c});
                set(block, Reg::R0, r);
                if (carry_live)
                    append(block, Opcode::A32SetCpsrNZC, {append(block, Opcode::GetNZFromOp, {r}), append(block, Opcode::GetCarryFromOp, {r})});
                else
                    append(block, Opcode::A32SetCpsrNZ, {append(block, Opcode::GetNZFromOp, {r})});
                std::vector<Case> cases;
                for (uint32_t av : {0u, 1u, 0x80000000u, 0xffffffffu, 0x12345678u}) {
                    for (uint32_t cv : {0u, 1u}) {
                        auto in = initial();
                        in.regs[0] = av; in.cpsr |= (cv << 29) | 0x10000000;
                        auto out = next(in);
                        uint32_t result = av, carry = cv;
                        if (amount != 0) {
                            if (k == 0) { result = amount < 32 ? av << amount : 0; carry = amount <= 32 ? (av >> (32 - amount)) & 1 : 0; }
                            if (k == 1) { result = amount < 32 ? av >> amount : 0; carry = amount <= 32 ? (av >> (amount - 1)) & 1 : 0; }
                            if (k == 2) {
                                const uint32_t s = std::min(amount, 31u);
                                result = av >> s;
                                if (av & 0x80000000) result |= ~(0xffffffffu >> s);
                                carry = (av >> std::min(amount - 1, 31u)) & 1;
                            }
                            if (k == 3) {
                                const auto s = amount % 32;
                                result = s ? (av >> s) | (av << (32 - s)) : av;
                                carry = result >> 31;
                            }
                        }
                        out.regs[0] = result;
                        out.cpsr = (in.cpsr & 0x1fffffff) | nz(result) | ((carry_live ? carry : cv) << 29);
                        cases.push_back({in, out});
                    }
                }
                suite.add("shiftimm" + std::to_string(k) + (carry_live ? "c" : "n") + "_" + std::to_string(amount), block, cases);
            }
        }
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

    // An architectural write must not overwrite an older SSA value. R10
    // also used to alias the second instruction's ten-word temporary slot.
    auto cached = blank();
    const auto old_r0 = reg(cached, Reg::R0);
    const auto old_r1 = reg(cached, Reg::R1);
    const auto old_r10 = reg(cached, Reg::R10);
    set(cached, Reg::R0, Value{uint32_t(1)});
    set(cached, Reg::R2, old_r0);
    set(cached, Reg::R3, old_r1);
    set(cached, Reg::R4, old_r10);
    out = next(in);
    out.regs[0] = 1;
    out.regs[2] = in.regs[0];
    out.regs[3] = in.regs[1];
    out.regs[4] = in.regs[10];
    suite.add("register_ssa_isolation", cached, {{in, out}});
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

// Real frontend flag producers immediately followed by SVC. Each case also
// becomes an A/P/K/PK region differential via Suite::add. Unexecuted here.
void frontend_flag_boundaries(Suite &suite) {
    struct Operation { const char *name; unsigned opcode; bool writes_register; };
    const std::array<Operation, 12> operations{{
        {"and", 0, true}, {"eor", 1, true}, {"sub", 2, true},
        {"rsb", 3, true}, {"add", 4, true}, {"adc", 5, true},
        {"sbc", 6, true}, {"cmp", 10, false}, {"cmn", 11, false},
        {"orr", 12, true}, {"mov", 13, true}, {"mvn", 15, true},
    }};
    for (const auto &operation : operations) {
        for (const bool set_flags : {false, true}) {
            if (!set_flags && !operation.writes_register) continue;
            // AL data processing, Rn=r0, Rd=r2, unshifted Rm=r1.
            const uint32_t instruction = 0xe0000001u | (operation.opcode << 21)
                | (set_flags ? 1u << 20 : 0) | (operation.writes_register ? 2u << 12 : 0);
            const auto block = translate({instruction, 0xef000042}, false);
            std::vector<Case> cases;
            for (const uint32_t a : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
                for (const uint32_t b : {0u, 1u, 0x80000000u, 0xffffffffu}) {
                    for (uint32_t flags = 0; flags < 16; ++flags) {
                        auto in = initial();
                        in.cpsr |= flags << 28;
                        in.regs[0] = a; in.regs[1] = b;
                        auto out = next(in, 2, 0x1008);
                        out.svc = 0x42; out.exit_reason = 1;
                        uint32_t result = 0, carry = (flags >> 1) & 1, overflow = flags & 1;
                        const unsigned op = operation.opcode;
                        if ((op >= 2 && op <= 6) || op == 10 || op == 11) {
                            const bool sub = op == 2 || op == 3 || op == 6 || op == 10;
                            const uint32_t lhs = op == 3 ? b : a, rhs = op == 3 ? a : b;
                            const uint32_t cin = op == 5 || op == 6 ? carry : sub ? 1 : 0;
                            const uint64_t wide = uint64_t(lhs) + (sub ? uint32_t(~rhs) : rhs) + cin;
                            const int64_t signed_result = sub
                                ? signed32(lhs) - signed32(rhs) - (1 - cin)
                                : signed32(lhs) + signed32(rhs) + cin;
                            result = static_cast<uint32_t>(wide); carry = wide >> 32;
                            overflow = signed_result < INT32_MIN || signed_result > INT32_MAX;
                        } else {
                            switch (op) {
                            case 0: result = a & b; break;
                            case 1: result = a ^ b; break;
                            case 12: result = a | b; break;
                            case 13: result = b; break;
                            case 15: result = ~b; break;
                            default: CHECK(false);
                            }
                        }
                        if (operation.writes_register) out.regs[2] = result;
                        if (set_flags)
                            out.cpsr = (in.cpsr & 0x0fffffff) | nz(result) | (carry << 29) | (overflow << 28);
                        cases.push_back({in, out});
                    }
                }
            }
            suite.add(std::string("svc_flags_") + operation.name + (set_flags ? "_s" : ""), block, cases);
        }
    }

    // NZ-only writes preserve BOTH C/V; subsequent carry consumption must
    // see the local C, and a full CPSR read must merge every preserved field.
    auto partial = blank();
    append(partial, Opcode::A32SetCpsrNZCVRaw, {reg(partial, Reg::R0)});
    append(partial, Opcode::A32SetCpsrNZ, {append(partial, Opcode::GetNZFromOp, {reg(partial, Reg::R1)})});
    set(partial, Reg::R2, append(partial, Opcode::A32GetCpsr, {}));
    const auto sum = append(partial, Opcode::Add32,
        {Value{uint32_t(0)}, Value{uint32_t(0)}, append(partial, Opcode::A32GetCFlag, {})});
    set(partial, Reg::R3, sum);
    std::vector<Case> cases;
    for (uint32_t flags = 0; flags < 16; ++flags) {
        for (uint32_t value : {0u, 1u, 0x80000000u}) {
            auto in = initial(); in.regs[0] = flags << 28; in.regs[1] = value;
            auto out = next(in);
            out.cpsr = (in.cpsr & 0x0fffffff) | (in.regs[0] & 0x30000000) | nz(value);
            out.regs[2] = out.cpsr; out.regs[3] = (flags >> 1) & 1;
            cases.push_back({in, out});
        }
    }
    suite.add("partial_nz_preserves_cv", partial, cases);

    // ADDS; LDR/STR fault: flags and completed IR survive, but this segment
    // contributes no ticks. Fault next_pc remains the incoming sentinel.
    for (bool write : {false, true}) {
        auto fault_block = translate({0xe0900001, write ? 0xe5823000u : 0xe5923000u}, false);
        auto in = initial(); in.regs[0] = 0x7fffffff; in.regs[1] = 1;
        in.regs[2] = 0x80000000; in.next_pc = 0xabcdef00;
        auto out = in; out.regs[0] = 0x80000000;
        out.cpsr = (in.cpsr & 0x0fffffff) | 0x90000000;
        out.fault_pc = 0x1004; out.fault_address = in.regs[2]; out.fault_write = write;
        out.exit_reason = 2; out.dispatches = in.dispatches + 1;
        if (write) out.memory_value[0] = in.regs[3];
        suite.add(write ? "flags_then_write_fault" : "flags_then_read_fault",
            fault_block, {{in, out}}, 2);
    }
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

void shifts64(Suite &suite) {
    const std::array<uint64_t, 4> values{0, 1, 0x8000000012345678ULL, UINT64_MAX};
    const auto make_block = [](std::optional<uint8_t> count) {
        auto block = blank();
        const auto input = append(block, Opcode::Pack2x32To1x64,
            {reg(block, Reg::R0), reg(block, Reg::R1)});
        const auto amount = count ? Value{*count}
            : append(block, Opcode::LeastSignificantByte, {reg(block, Reg::R2)});
        const auto shifted = append(block, Opcode::LogicalShiftRight64, {input, amount});
        set(block, Reg::R3, append(block, Opcode::LeastSignificantWord, {shifted}));
        set(block, Reg::R4, append(block, Opcode::MostSignificantWord, {shifted}));
        return block;
    };
    const auto make_case = [](uint64_t value, unsigned count) {
        auto in = initial();
        in.regs[0] = static_cast<uint32_t>(value);
        in.regs[1] = static_cast<uint32_t>(value >> 32);
        in.regs[2] = count;
        auto out = next(in);
        const unsigned shift = static_cast<uint8_t>(count);
        const uint64_t expected = shift < 64 ? value >> shift : 0;
        out.regs[3] = static_cast<uint32_t>(expected);
        out.regs[4] = static_cast<uint32_t>(expected >> 32);
        return Case{in, out};
    };
    std::vector<Case> cases;
    for (unsigned count = 0; count <= 256; ++count)
        for (const auto value : values)
            cases.push_back(make_case(value, count));
    suite.add("lsr64_register", make_block(std::nullopt), cases);
    for (const uint8_t count : {0, 63, 64, 65, 255}) {
        cases.clear();
        for (const auto value : values)
            cases.push_back(make_case(value, count));
        suite.add("lsr64_immediate_" + std::to_string(count), make_block(count), cases);
    }
}

void memory_bases(Suite &suite) {
    // Distinct guest and sparse backing values reveal which path actually ran.
    constexpr uint32_t guest = 0x3000, backing = 0xb000;
    constexpr uint32_t guest_word = 0xaabbccdd, backing_word = 0x11223344;
    for (const unsigned width : {1u, 2u, 4u}) {
        const uint32_t mask = width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1;
        for (const bool write : {false, true}) {
            const uint32_t opcode = width == 1 ? (write ? 0xe5c10000 : 0xe5d10000)
                : width == 2 ? (write ? 0xe1c100b0 : 0xe1d100b0)
                             : (write ? 0xe5810000 : 0xe5910000);
            const auto block = translate({opcode}, false);
            std::vector<Case> cases;
            for (unsigned missing = 0; missing < 4; ++missing) {
                auto in = initial();
                in.regs[0] = 0xe5b6c7d8;
                in.regs[1] = guest;
                in.page_table_base = missing == 1 ? 0 : 0x8000;
                in.page_perms_base = missing == 2 ? 0 : 0x9000;
                in.code_pages_base = missing == 3 ? 0 : 0xa000;
                auto out = next(in);
                const bool fast = missing == 0;
                if (write) {
                    if (fast) out.mem_fast_writes = 1;
                    else out.memory_value[0] = in.regs[0] & mask;
                } else {
                    out.regs[0] = (fast ? backing_word : guest_word) & mask;
                    if (fast) out.mem_fast_reads = 1;
                    else out.memory_value[0] = out.regs[0];
                }
                Case test{in, out};
                // The low addresses deliberately resemble valid metadata.
                // Missing bases must disable probing regardless of those bytes.
                test.pre = {{0, 0x03000000}, {12, 0}, {0x800c, backing},
                    {0x9000, 0x03000000}, {0xa00c, 0},
                    {guest, guest_word}, {backing, backing_word}};
                test.mem = {{guest, guest_word}, {backing, backing_word}};
                if (write) {
                    const uint32_t old = fast ? backing_word : guest_word;
                    test.mem[fast ? backing : guest] = (old & ~mask) | (in.regs[0] & mask);
                }
                cases.push_back(test);
            }
            suite.add(std::string(write ? "memory_bases_write" : "memory_bases_read")
                    + std::to_string(width * 8), block, cases);
        }
    }
}

void memory_address_faults(Suite &suite) {
    for (const bool write : {false, true}) {
        const auto block = translate({write ? 0xe5810000u : 0xe5910000u}, false);
        std::vector<Case> cases;
        // Upper-half addresses arrive at JS imports as negative i32 values.
        for (const uint32_t address : {0x1ffffu, 0x80000000u, 0xfffffffcu, UINT32_MAX}) {
            auto in = initial();
            in.regs[1] = address;
            auto out = in;
            out.svc = out.executed = 0;
            out.exit_reason = 2;
            out.fault_address = address;
            out.fault_write = write;
            if (write) out.memory_value[0] = in.regs[0];
            cases.push_back({in, out});
        }
        suite.add(write ? "write32_address_faults" : "read32_address_faults", block, cases);
    }
}

void region_it_faults(Suite &suite) {
    for (const bool write : {false, true}) {
        // Start in ITT EQ: MOV r0,r3 completes, then LDR/STR r2,[r1] faults.
        Code code({0x4618, write ? 0x600au : 0x680au}, true);
        const auto at = loc(true).SetIT(A32::ITState{0x04});
        const auto block = A32::Translate(at, &code, {A32::ArchVersion::v7, false, false});
        CHECK(block.CycleCount() == 2 && block.GetCondition() == Cond::EQ);
        auto in = initial(true);
        in.cpsr = 0xf80f0430; // NZCV, Q and GE must survive the mode recovery.
        in.svc = in.exit_reason = 0;
        in.executed = 17;
        in.regs[1] = 0x80000000;
        auto fault = in;
        fault.regs[0] = in.regs[3]; // Completed instruction remains committed.
        fault.cpsr = 0xf80f0830; // IT advanced to the second slot.
        fault.exit_reason = 2;
        fault.fault_pc = 0x1002;
        fault.fault_address = in.regs[1];
        fault.fault_write = write;
        fault.dispatches = 1;
        if (write) fault.memory_value[0] = in.regs[2];

        auto skipped_in = in;
        skipped_in.cpsr &= ~0x40000000u;
        auto skipped = skipped_in;
        skipped.cpsr &= ~0x400u; // Both IT slots are skipped.
        skipped.regs[15] = skipped.next_pc = 0x1004;
        skipped.executed += 2;
        skipped.exit_reason = 4;
        skipped.dispatches = 2;
        suite.add(write ? "region_it_write_fault" : "region_it_read_fault", block,
            {{in, fault}, {skipped_in, skipped}}, 4);
    }
}

// Bitmask of region policies (bit P=1, bit K=2) whose emission accepts the
// block. Used to distinguish whole-emitter coverage gaps (nothing accepts)
// from shapes only one path supports (e.g. multi-tick memory blocks the
// block path refuses so the runtime can split, while the region path lowers
// them through store continuations).
unsigned region_accept_mask(const IR::Block &block) {
    unsigned accepted = 0;
    const A32::LocationDescriptor at(block.Location());
    for (unsigned mode = 0; mode < 4; ++mode) {
        const auto bytes = vita3k::wasmjit::emit_region({&block}, {{at.PC(),
            A32::LocationDescriptor::CPSR_MODE_MASK,
            at.CPSR().Value() & A32::LocationDescriptor::CPSR_MODE_MASK,
            static_cast<uint32_t>(block.CycleCount() + block.ConditionFailedCycleCount())}},
            {(mode & 1) != 0, (mode & 2) != 0});
        if (!bytes.empty()) accepted |= 1u << mode;
    }
    return accepted;
}

void reject_block(const IR::Block &block, const char *context = "") {
    CHECK(emit_block(block).empty());
    const unsigned accepted = region_accept_mask(block);
    if (accepted != 0)
        std::cerr << "region policies " << accepted << " accept block rejected by emit_block (" << context << ")\n";
    CHECK(accepted == 0);
}

void rejects() {
    const auto reject = [](const IR::Block &b) { reject_block(b); };
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

// Task #13: vitaslop ARM/NEON conformance import. Each case file is split by
// `# --- generated by regen ---` into a human top (description/asm/seed) and
// a machine bottom (assembled bytes + qemu golden). The importer reads the
// mode switch and [in].regs seed from the top but NEVER the `asm` text; the
// bytes and goldens come strictly from below the marker. Every case loads at
// the same 0x1000 base as the existing corpus, translates through the real
// Dynarmic translator, and becomes a reference + P/K/PK fixture exactly like
// existing entries. Cases our emitter cannot lower are pinned fail-closed
// with reject_block (the suite's existing expected-unsupported mechanism)
// and reported; programs spanning several blocks cannot carry a
// whole-program golden in one single-block fixture and are reported too.
namespace vitaslop {
constexpr uint32_t kBase = 0x1000;
constexpr const char *kDefaultDir =
    ".limbo_work/vitaslop/projects/vitaslop-conformance-suite-arm/cases";

struct VCase {
    std::string name;
    bool thumb = false;
    std::vector<uint8_t> bin;
    std::map<unsigned, uint32_t> in_regs, out_regs;
    bool n = false, z = false, c = false, v = false;
};

struct Gap {
    std::string name, kind, detail;
};

std::vector<uint8_t> base64_decode(const std::string &text) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    unsigned accumulator = 0;
    int bits = 0;
    for (const char ch : text) {
        if (ch == '=') break;
        const size_t digit = alphabet.find(ch);
        if (digit == std::string::npos) continue; // whitespace/newlines
        accumulator = (accumulator << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(accumulator >> bits));
            accumulator &= (bits == 0) ? 0 : (1u << bits) - 1;
        }
    }
    return out;
}

std::string trim(const std::string &s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

uint32_t parse_number(const std::string &token) {
    size_t done = 0;
    const unsigned long value = std::stoul(token, &done, 0); // 0x or decimal
    CHECK(done == token.size() && value <= 0xfffffffful);
    return static_cast<uint32_t>(value);
}

// Parses `rN = V` pairs: one per line for [out.regs], comma-separated inside
// the [in] `regs = { ... }` braces. r13/r15 are harness-owned, never seeded
// or captured, so anything else is a loud error, not a default.
void parse_reg_list(const std::string &payload, std::map<unsigned, uint32_t> &regs) {
    size_t pos = 0;
    while (pos <= payload.size()) {
        const size_t comma = payload.find(',', pos);
        const std::string piece = trim(comma == std::string::npos
            ? payload.substr(pos)
            : payload.substr(pos, comma - pos));
        if (!piece.empty()) {
            const size_t eq = piece.find('=');
            CHECK(eq != std::string::npos);
            const std::string name = trim(piece.substr(0, eq));
            CHECK(name.size() > 1 && name[0] == 'r');
            const unsigned reg = static_cast<unsigned>(std::stoul(name.substr(1)));
            CHECK(reg <= 12 || reg == 14);
            regs[reg] = parse_number(trim(piece.substr(eq + 1)));
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

// Returns nullopt for host-capture programs (svc/output, not reg goldens).
std::optional<VCase> parse_file(const std::filesystem::path &path) {
    std::ifstream file(path);
    CHECK(file.good());
    const std::string content((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    const size_t marker = content.find("# --- generated by");
    if (marker == std::string::npos) {
        std::cerr << "vitaslop case missing generated half: " << path << '\n';
        std::abort(); // a case without goldens is an error, not a skip
    }
    VCase c;
    c.name = path.stem().string();
    std::string section;
    std::string pending_regs;
    bool in_regs_brace = false;
    std::string b64;
    bool in_b64 = false;
    unsigned flags_seen = 0;
    size_t line_start = 0;
    while (line_start <= content.size()) {
        size_t line_end = content.find('\n', line_start);
        if (line_end == std::string::npos) line_end = content.size();
        const std::string line = trim(content.substr(line_start, line_end - line_start));
        const bool below = line_start >= marker;
        line_start = line_end + 1;
        if (in_b64) {
            const size_t close = line.find("'''");
            b64 += (close == std::string::npos) ? line : line.substr(0, close);
            if (close != std::string::npos) in_b64 = false;
            continue;
        }
        if (in_regs_brace) {
            const size_t close = line.find('}');
            pending_regs += (close == std::string::npos) ? line + "," : line.substr(0, close);
            if (close != std::string::npos) {
                in_regs_brace = false;
                parse_reg_list(pending_regs, c.in_regs);
            }
            continue;
        }
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            section = line;
            continue;
        }
        if (line.empty()) continue;
        if (!below) {
            if (line.rfind("mode", 0) == 0) {
                const size_t first = line.find('"'), last = line.rfind('"');
                CHECK(first != std::string::npos && last > first);
                const std::string mode = line.substr(first + 1, last - first - 1);
                CHECK(mode == "arm" || mode == "thumb");
                c.thumb = mode == "thumb";
            } else if (line.rfind("capture", 0) == 0) {
                if (line.find("output") != std::string::npos) return std::nullopt;
                CHECK(line.find("regs") != std::string::npos);
            } else if (section == "[in]" && line.rfind("regs", 0) == 0) {
                const size_t open = line.find('{');
                CHECK(open != std::string::npos);
                const std::string rest = line.substr(open + 1);
                const size_t close = rest.find('}');
                if (close == std::string::npos) {
                    pending_regs = rest + ",";
                    in_regs_brace = true;
                } else {
                    parse_reg_list(rest.substr(0, close), c.in_regs);
                }
            }
            // `asm`/description are never read: the bytes below are the truth.
        } else if (section == "[bin]" && line.rfind("base64", 0) == 0) {
            const size_t open = line.find("'''");
            CHECK(open != std::string::npos);
            const std::string rest = line.substr(open + 3);
            const size_t close = rest.find("'''");
            if (close == std::string::npos) {
                b64 = rest;
                in_b64 = true;
            } else {
                b64 = rest.substr(0, close);
            }
        } else if (section == "[out.regs]" && line.find('=') != std::string::npos) {
            parse_reg_list(line, c.out_regs);
        } else if (section == "[out.flags]" && line.find('=') != std::string::npos) {
            const size_t eq = line.find('=');
            const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            CHECK(key == "n" || key == "z" || key == "c" || key == "v");
            CHECK(value == "true" || value == "false");
            const bool set = value == "true";
            if (key == "n") c.n = set;
            if (key == "z") c.z = set;
            if (key == "c") c.c = set;
            if (key == "v") c.v = set;
            ++flags_seen;
        }
    }
    CHECK(!in_b64 && !in_regs_brace);
    c.bin = base64_decode(b64);
    CHECK(!c.bin.empty());
    CHECK(c.thumb ? c.bin.size() % 2 == 0 : c.bin.size() % 4 == 0);
    CHECK(flags_seen == 4);
    return c;
}

// Byte-backed translator callbacks (mirrors Code, but keeps Thumb 32-bit
// pairs byte-exact instead of truncating each word to one halfword).
struct BytesCode final : A32::TranslateCallbacks {
    std::vector<uint8_t> bytes;
    explicit BytesCode(std::vector<uint8_t> code) : bytes(std::move(code)) {}
    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        if (address < kBase || address >= kBase + bytes.size()) return {};
        uint32_t word = 0;
        for (unsigned i = 0; i < 4 && address - kBase + i < bytes.size(); ++i)
            word |= uint32_t(bytes[address - kBase + i]) << (8 * i);
        return word;
    }
    bool PreCodeReadHook(bool, uint32_t pc, A32::IREmitter &ir) override {
        if (pc < kBase + bytes.size()) return true;
        ir.SetTerm(IR::Term::LinkBlock{ir.current_location});
        return false;
    }
    void PreCodeTranslationHook(bool, uint32_t, A32::IREmitter &) override {}
    uint64_t GetTicksForCode(bool, uint32_t, uint32_t) override { return 1; }
};

std::string opcode_list(const IR::Block &block) {
    std::set<std::string> names;
    for (const auto &inst : block) names.insert(IR::GetNameOf(inst.GetOpcode()));
    std::string out;
    for (const auto &name : names) {
        if (!out.empty()) out += ' ';
        out += name;
    }
    return out;
}

std::string hex_bytes(const std::vector<uint8_t> &bytes) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (uint8_t byte : bytes) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

// Single-block fixtures can only carry a whole-program golden when the
// translation consumes every byte and falls through past the final one.
bool covers_whole_program(const IR::Block &block, const VCase &c, uint32_t &ticks) {
    if (block.GetCondition() != Cond::AL) return false;
    const IR::Terminal terminal = block.GetTerminal();
    uint32_t target = 0;
    if (const auto *link = boost::get<IR::Term::LinkBlock>(&terminal)) {
        target = A32::LocationDescriptor{link->next}.PC();
    } else if (const auto *fast = boost::get<IR::Term::LinkBlockFast>(&terminal)) {
        target = A32::LocationDescriptor{fast->next}.PC();
    } else {
        return false;
    }
    const uint32_t end = kBase + static_cast<uint32_t>(c.bin.size());
    if (target != end) return false;
    ticks = static_cast<uint32_t>(block.CycleCount() + block.ConditionFailedCycleCount());
    return ticks > 0;
}

// Symbolic fpu tracker. Input fpu is zero; words become concrete when a
// vmov seed writes them from input regs, or golden-constrained when a vector
// ALU result is fully read back to integer regs (every corpus case
// round-trips its result through vmov, so integer-reg comparison suffices and
// this only fills the scratch words the Wasm is known to write). A written
// word left neither concrete nor constrained, or any unexpected shape, is a
// loud abort, never a guessed zero.
struct FpWords {
    bool known = false;
    std::array<uint32_t, 4> lane{};
    unsigned lanes = 0;
};

struct FpTracker {
    const JitState &in;
    const VCase &c;
    std::array<std::optional<uint32_t>, 64> words{};
    std::array<bool, 64> defined{};
    std::array<bool, 64> constrained{};
};

FpWords fp_eval_value(const Value &v, FpTracker &fp);

FpWords fp_eval_inst(const IR::Inst &inst, FpTracker &fp) {
    const auto arg = [&](size_t i) { return fp_eval_value(inst.GetArg(i), fp); };
    switch (inst.GetOpcode()) {
    case Opcode::A32GetRegister: {
        const unsigned r = static_cast<unsigned>(inst.GetArg(0).GetA32RegRef());
        CHECK(r < 16);
        FpWords out;
        out.known = true;
        out.lane[0] = fp.in.regs[r];
        out.lanes = 1;
        return out;
    }
    case Opcode::Identity:
        return arg(0);
    case Opcode::Pack2x32To1x64: {
        const FpWords a = arg(0), b = arg(1);
        if (!a.known || !b.known || a.lanes != 1 || b.lanes != 1) return {};
        FpWords out;
        out.known = true;
        out.lane[0] = a.lane[0];
        out.lane[1] = b.lane[0];
        out.lanes = 2;
        return out;
    }
    case Opcode::LeastSignificantWord: {
        const FpWords a = arg(0);
        if (!a.known || a.lanes < 1) return {};
        FpWords out;
        out.known = true;
        out.lane[0] = a.lane[0];
        out.lanes = 1;
        return out;
    }
    case Opcode::MostSignificantWord: {
        const FpWords a = arg(0);
        if (!a.known || a.lanes != 2) return {};
        FpWords out;
        out.known = true;
        out.lane[0] = a.lane[1];
        out.lanes = 1;
        return out;
    }
    case Opcode::A32GetVector:
    case Opcode::A32GetExtendedRegister64: {
        const A32::ExtReg e = inst.GetArg(0).GetA32ExtRegRef();
        unsigned base = 0, count = 0;
        if (A32::IsDoubleExtReg(e)) {
            base = 2 * (static_cast<unsigned>(e) - static_cast<unsigned>(A32::ExtReg::D0));
            count = 2;
        } else if (A32::IsQuadExtReg(e)) {
            base = 4 * (static_cast<unsigned>(e) - static_cast<unsigned>(A32::ExtReg::Q0));
            count = 4;
        } else {
            std::cerr << "vitaslop " << fp.c.name << ": S-register vector read\n";
            std::abort();
        }
        FpWords out;
        out.lanes = count;
        out.known = true;
        for (unsigned i = 0; i < count; ++i) {
            if (!fp.words[base + i]) return {};
            out.lane[i] = *fp.words[base + i];
        }
        return out;
    }
    default:
        return {}; // vector/FP ALU and flag pseudos stay symbolic
    }
}

FpWords fp_eval_value(const Value &v, FpTracker &fp) {
    if (v.IsImmediate()) {
        FpWords out;
        out.lanes = 1;
        out.known = true;
        if (v.GetType() == IR::Type::U1) out.lane[0] = v.GetU1() ? 1 : 0;
        else if (v.GetType() == IR::Type::U8) out.lane[0] = v.GetU8();
        else if (v.GetType() == IR::Type::U16) out.lane[0] = v.GetU16();
        else if (v.GetType() == IR::Type::U32) out.lane[0] = v.GetU32();
        else if (v.GetType() == IR::Type::U64) {
            const uint64_t wide = v.GetU64();
            out.lane[0] = static_cast<uint32_t>(wide);
            out.lane[1] = static_cast<uint32_t>(wide >> 32);
            out.lanes = 2;
        } else return {}; // NZCV marker and friends stay symbolic
        return out;
    }
    IR::Inst *def = v.GetInst();
    if (!def) return {};
    return fp_eval_inst(*def, fp);
}

bool fp_touches(const Value &v) {
    if (v.IsImmediate() || !v.GetInst()) return false;
    const IR::Inst &inst = *v.GetInst();
    const auto op = inst.GetOpcode();
    if (op == Opcode::A32GetVector || op == Opcode::A32GetExtendedRegister64) return true;
    for (size_t i = 0; i < IR::GetNumArgsOf(op); ++i)
        if (fp_touches(inst.GetArg(i))) return true;
    return false;
}

// Resolves an fpu-derived value to the exact fpu word it reads, through the
// readback idiom only (Identity/LSW/MSW over an explicit D/Q read).
std::optional<unsigned> fp_resolve(const Value &v, unsigned lane, const VCase &c) {
    auto fail = [&]() -> std::optional<unsigned> {
        std::cerr << "vitaslop " << c.name << ": unresolvable fpu readback\n";
        std::abort();
    };
    if (v.IsImmediate() || !v.GetInst()) return fail();
    const IR::Inst &inst = *v.GetInst();
    const auto op = inst.GetOpcode();
    if (op == Opcode::Identity) return fp_resolve(inst.GetArg(0), lane, c);
    if (op == Opcode::LeastSignificantWord) {
        if (lane != 0) return fail();
        return fp_resolve(inst.GetArg(0), 0, c);
    }
    if (op == Opcode::MostSignificantWord) {
        if (lane != 0) return fail();
        return fp_resolve(inst.GetArg(0), 1, c);
    }
    if (op == Opcode::A32GetVector || op == Opcode::A32GetExtendedRegister64) {
        const A32::ExtReg e = inst.GetArg(0).GetA32ExtRegRef();
        if (A32::IsDoubleExtReg(e)) {
            if (lane > 1) return fail();
            return 2 * (static_cast<unsigned>(e) - static_cast<unsigned>(A32::ExtReg::D0)) + lane;
        }
        if (A32::IsQuadExtReg(e)) {
            if (lane > 3) return fail();
            return 4 * (static_cast<unsigned>(e) - static_cast<unsigned>(A32::ExtReg::Q0)) + lane;
        }
        return fail();
    }
    return fail();
}

void fp_write(FpTracker &fp, A32::ExtReg e, const FpWords &v) {
    unsigned base = 0, count = 0;
    if (A32::IsDoubleExtReg(e)) {
        base = 2 * (static_cast<unsigned>(e) - static_cast<unsigned>(A32::ExtReg::D0));
        count = 2;
    } else if (A32::IsQuadExtReg(e)) {
        base = 4 * (static_cast<unsigned>(e) - static_cast<unsigned>(A32::ExtReg::Q0));
        count = 4;
    } else {
        std::cerr << "vitaslop " << fp.c.name << ": S-register vector write\n";
        std::abort();
    }
    if (!v.known || v.lanes != count) {
        for (unsigned i = 0; i < count; ++i) { // symbolic result: cleared until golden-constrained
            fp.words[base + i].reset();
            fp.defined[base + i] = true;
        }
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (fp.constrained[base + i]) { // a readback-pinned word must never be overwritten
            std::cerr << "vitaslop " << fp.c.name << ": overwrite of constrained fpu word\n";
            std::abort();
        }
        fp.words[base + i] = v.lane[i];
        fp.defined[base + i] = true;
    }
}

uint32_t fp_golden_reg(const VCase &c, unsigned r) {
    return c.out_regs.count(r) ? c.out_regs.at(r) : 0;
}

void fp_track_block(const IR::Block &block, FpTracker &fp) {
    for (const auto &inst : block) {
        const auto op = inst.GetOpcode();
        if (op == Opcode::A32SetRegister) {
            const unsigned r = static_cast<unsigned>(inst.GetArg(0).GetA32RegRef());
            if (r == 13 || r == 15) {
                std::cerr << "vitaslop " << fp.c.name << ": unexpected write to r" << r << '\n';
                std::abort();
            }
            const Value v = inst.GetArg(1);
            const FpWords known = fp_eval_value(v, fp);
            if (known.known) {
                CHECK(known.lanes == 1); // cross-checks the tracker against the golden
                CHECK(known.lane[0] == fp_golden_reg(fp.c, r));
            } else if (fp_touches(v)) {
                const std::optional<unsigned> word = fp_resolve(v, 0, fp.c);
                CHECK(word.has_value());
                if (fp.words[*word] && *fp.words[*word] != fp_golden_reg(fp.c, r)) {
                    std::cerr << "vitaslop " << fp.c.name << ": seed/readback contradiction\n";
                    std::abort();
                }
                fp.words[*word] = fp_golden_reg(fp.c, r);
                fp.defined[*word] = true;
                fp.constrained[*word] = true;
            }
            // Pure scalar results need no fpu action; the golden covers them.
        } else if (op == Opcode::A32SetExtendedRegister64 || op == Opcode::A32SetVector) {
            fp_write(fp, inst.GetArg(0).GetA32ExtRegRef(), fp_eval_value(inst.GetArg(1), fp));
        }
    }
    for (unsigned i = 0; i < 64; ++i)
        if (fp.defined[i] && !fp.words[i]) {
            std::cerr << "vitaslop " << fp.c.name << ": fpu word " << i
                      << " written but not covered by regs or golden\n";
            std::abort();
        }
}

// Vitaslop seeds zeroed regs + [in].regs with cleared flags; unlisted integer
// regs read back as 0, r13/r15 are harness-owned (sp gets a scratch slot the
// goldens never capture), and NZCV comes from [out.flags] over the preserved
// low cpsr bits (Q/GE/IT/mode start clear, matching the cleared-flags seed).
Case make_case(const VCase &c, const IR::Block &block, uint32_t ticks) {
    JitState in{};
    for (const auto &[reg, value] : c.in_regs) in.regs[reg] = value;
    in.regs[13] = 0x5000;
    in.regs[15] = kBase;
    in.cpsr = c.thumb ? 0x30u : 0x10u;
    in.svc = 0xbad;
    in.exit_reason = 99;
    in.executed = 99;
    FpTracker fp{in, c};
    fp_track_block(block, fp);
    JitState out = in;
    for (unsigned i = 0; i < 64; ++i)
        if (fp.words[i]) out.fpu[i] = *fp.words[i]; // post-execution scratch; input stays zeroed
    for (unsigned r = 0; r <= 12; ++r) out.regs[r] = c.out_regs.count(r) ? c.out_regs.at(r) : 0;
    out.regs[14] = c.out_regs.count(14) ? c.out_regs.at(14) : 0;
    out.regs[15] = kBase + static_cast<uint32_t>(c.bin.size());
    out.cpsr = (in.cpsr & 0x0fffffff)
        | (static_cast<uint32_t>(c.n) << 31) | (static_cast<uint32_t>(c.z) << 30)
        | (static_cast<uint32_t>(c.c) << 29) | (static_cast<uint32_t>(c.v) << 28);
    out.svc = 0;
    out.exit_reason = 0;
    out.executed = ticks;
    return {in, out};
}

void import_conformance(Suite &suite, const std::string &dir,
    size_t &passed, size_t &skipped, std::vector<Gap> &gaps) {
    CHECK(std::filesystem::is_directory(dir));
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".toml") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    CHECK(!files.empty());
    for (const auto &path : files) {
        auto parsed = parse_file(path);
        if (!parsed) {
            ++skipped;
            std::cout << "vitaslop skip (host capture, not regs): " << path.stem().string() << '\n';
            continue;
        }
        VCase &c = *parsed;
        BytesCode code(c.bin);
        IR::Block block =
            A32::Translate(loc(c.thumb, kBase), &code, {A32::ArchVersion::v7, false, false});
        const std::string ops = opcode_list(block);
        uint32_t ticks = 0;
        if (!covers_whole_program(block, c, ticks)) {
            gaps.push_back({c.name, "multi-block-program",
                "bytes " + hex_bytes(c.bin) + " IR: " + ops});
            std::cout << "VITASLOP_GAP multi-block-program " << c.name << " IR: " << ops << '\n';
            continue;
        }
        if (emit_block(block).empty()) {
            const std::string context = "vitaslop " + c.name;
            const unsigned accepted = region_accept_mask(block);
            if (accepted == 0) {
                reject_block(block, context.c_str()); // pin fail-closed everywhere
                gaps.push_back({c.name, "ir-coverage",
                    "bytes " + hex_bytes(c.bin) + " IR: " + ops});
                std::cout << "VITASLOP_GAP ir-coverage " << c.name << " IR: " << ops << '\n';
            } else {
                gaps.push_back({c.name, "needs-runtime-split",
                    "bytes " + hex_bytes(c.bin) + " region policies accepting: "
                    + std::to_string(accepted) + " IR: " + ops});
                std::cout << "VITASLOP_GAP needs-runtime-split " << c.name
                          << " region-accept-mask=" << accepted << " IR: " << ops << '\n';
            }
            continue;
        }
        if (ops.find("A32SetVector") != std::string::npos
            || ops.find("A32SetExtendedRegister64") != std::string::npos
            || ops.find("A32SetCpsrNZCVRaw") != std::string::npos
            || ops.find("Memory") != std::string::npos)
            std::cout << "VITASLOP_NOTE side-channel IR in " << c.name << ": " << ops << '\n';
        suite.add("vitaslop_" + c.name, block, {make_case(c, block, ticks)});
        ++passed;
        std::cout << "vitaslop fixture " << c.name << " ticks=" << ticks << " IR: " << ops << '\n';
    }
}
} // namespace vitaslop
} // namespace

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    rejects();
    Suite suite{argv[1]};
    arithmetic(suite);
    shifts(suite);
    shifts_imm(suite);
    conditions(suite);
    scalars(suite);
    frontend(suite);
    frontend_flag_boundaries(suite);
    vector_loop(suite);
    vfp_memory(suite);
    most_significant_word(suite);
    shifts64(suite);
    memory_bases(suite);
    memory_address_faults(suite);
    region_it_faults(suite);
    size_t vitaslop_passed = 0, vitaslop_skipped = 0;
    std::vector<vitaslop::Gap> vitaslop_gaps;
    vitaslop::import_conformance(suite, argc == 3 ? argv[2] : vitaslop::kDefaultDir,
        vitaslop_passed, vitaslop_skipped, vitaslop_gaps);
    std::cout << "Native rejection/determinism checks passed; generated " << suite.modules
              << " reference modules + " << suite.candidate_modules << " candidate modules and "
              << suite.runs << " input cases (region inputs also run under P/K/PK)\n";
    std::cout << "vitaslop import: " << vitaslop_passed << " fixtures, "
              << vitaslop_skipped << " host-capture skips, " << vitaslop_gaps.size()
              << " coverage gaps\n";
    for (const auto &gap : vitaslop_gaps)
        std::cout << "VITASLOP_GAP " << gap.kind << ' ' << gap.name << ' ' << gap.detail << '\n';
}
