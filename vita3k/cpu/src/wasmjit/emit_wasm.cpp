// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include "emit_wasm.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(uint32_t, vita3k_promoted_state_options, (), {
    const read = (name) => {
        if (typeof Module !== 'undefined' && Module[name] !== undefined)
            return String(Module[name]);
        return (typeof process !== 'undefined' && process.env) ? process.env[name] : undefined;
    };
    const both = read('VITA3K_WASMJIT_PROMOTED_STATE') === '1';
    // Production default (R2): promoted flags are ON unless explicitly set
    // to "0" (reference A kept behind that diagnostic switch). Accounting
    // promotion stays opt-in ("1", or the PROMOTED_STATE umbrella).
    const enabled = (name, def) => {
        const value = read(name);
        return value === undefined ? def : value === '1';
    };
    return (enabled('VITA3K_WASMJIT_PROMOTE_FLAGS', true) ? 1 : 0)
        | (enabled('VITA3K_WASMJIT_PROMOTE_ACCOUNTING', both) ? 2 : 0);
});
// Bench-only opt-in is read from the JS side (Node process.env), the same
// channel as the existing VITA3K_DUMP_JIT hook in wasm_jit_cpu.cpp: C-level
// getenv does not see Node env under Emscripten. Native builds use getenv.
// Diagnostic only (default off): re-derive the exact slow-path fallback reason
// in the probe's cold arm. The production shape reports kSlowOther for every
// fallback; enabling this doubles module size and halves speed (cold bloat
// blocks Wasm tier-up), so it is strictly a fallback-storm debugging aid.
// Same channel as the ablate hooks: Module prop first, then Node process.env.
EM_JS(uint32_t, vita3k_slow_reason_detail, (), {
    if (typeof Module !== 'undefined' && Module['VITA3K_WASMJIT_SLOW_REASONS'] !== undefined)
        return String(Module['VITA3K_WASMJIT_SLOW_REASONS']) === '1' ? 1 : 0;
    const v = (typeof process !== 'undefined' && process.env) ? process.env.VITA3K_WASMJIT_SLOW_REASONS : null;
    return v === '1' ? 1 : 0;
});
EM_JS(uint32_t, vita3k_ablate_entry_pc, (), {
    const v = (typeof process !== 'undefined' && process.env) ? process.env.VITA3K_ABLATE_PC : null;
    if (!v)
        return 0;
    const n = parseInt(String(v), 16);
    return Number.isFinite(n) ? (n >>> 0) : 0;
});
EM_JS(uint32_t, vita3k_ablate_env_flags, (), {
    const v = (typeof process !== 'undefined' && process.env) ? process.env.VITA3K_ABLATE : null;
    if (!v)
        return 0;
    let f = 0;
    for (const ch of String(v).toUpperCase()) {
        if (ch === 'B')
            f |= 1;
        else if (ch === 'C')
            f |= 2;
        else if (ch === 'D')
            f |= 4;
        else if (ch === 'F')
            f |= 7;
        else if (ch === 'E')
            f |= 8;
        else if (ch === 'G')
            f |= 30;
    }
    return f;
});
#endif
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/ir/opcodes.h>

namespace vita3k::wasmjit {
namespace {
using Bytes = std::vector<uint8_t>;
using Op = Dynarmic::IR::Opcode;
using Type = Dynarmic::IR::Type;
using Value = Dynarmic::IR::Value;
using Inst = Dynarmic::IR::Inst;
using Cond = Dynarmic::IR::Cond;
using Location = Dynarmic::A32::LocationDescriptor;
namespace Term = Dynarmic::IR::Term;

// Only MVP opcodes. In particular, no sign-extension proposal or multivalue.
enum Wasm : uint8_t {
    Block = 0x02, Loop = 0x03, If = 0x04, Else = 0x05, End = 0x0b,
    Br = 0x0c, BrIf = 0x0d, BrTable = 0x0e, Return = 0x0f, Call = 0x10,
    CallIndirect = 0x11, Select = 0x1b,
    Get = 0x20, Set = 0x21, Load = 0x28, Load8U = 0x2d, Load16U = 0x2f,
    Store = 0x36, Store8 = 0x3a, Store16 = 0x3b, Const = 0x41,
    Eqz = 0x45, Eq = 0x46, Ne = 0x47, LtU = 0x49, GtU = 0x4b, LeU = 0x4d, GeU = 0x4e,
    Clz = 0x67, Add = 0x6a, Sub = 0x6b, Mul = 0x6c, And = 0x71, Or = 0x72, Xor = 0x73,
    Shl = 0x74, ShrS = 0x75, ShrU = 0x76, RotR = 0x78,
    Add64 = 0x7c, Mul64 = 0x7e, Or64 = 0x84, Shl64 = 0x86, ShrU64 = 0x88, ShrS64 = 0x87, Wrap = 0xa7, ExtendU = 0xad,
};

void uleb(Bytes &out, uint32_t n) {
    do {
        const uint8_t byte = n & 0x7f;
        n >>= 7;
        out.push_back(byte | (n ? 0x80 : 0));
    } while (n);
}

void constant(Bytes &out, uint32_t n) {
    out.push_back(Const);
    // Use division rather than implementation-defined right shift of negative.
    int64_t s = n <= INT32_MAX ? int64_t(n) : int64_t(n) - (int64_t(1) << 32);
    for (;;) {
        const uint8_t byte = static_cast<uint8_t>(s) & 0x7f;
        s = (s - byte) / 128;
        const bool done = (s == 0 && !(byte & 0x40)) || (s == -1 && (byte & 0x40));
        out.push_back(byte | (done ? 0 : 0x80));
        if (done)
            break;
    }
}

void section(Bytes &module, uint8_t id, const Bytes &payload) {
    module.push_back(id);
    uleb(module, static_cast<uint32_t>(payload.size()));
    module.insert(module.end(), payload.begin(), payload.end());
}

// Free byte helpers so the region dispatcher can share the exact emitter
// encoding without an Emitter instance.
void b_op(Bytes &c, uint8_t byte) { c.push_back(byte); }
void b_imm(Bytes &c, uint32_t n) { constant(c, n); }
void b_get(Bytes &c, uint32_t index) { b_op(c, Get); uleb(c, index); }
void b_set(Bytes &c, uint32_t index) { b_op(c, Set); uleb(c, index); }
void b_load(Bytes &c, uint32_t offset) { b_get(c, 0); b_op(c, Load); uleb(c, 2); uleb(c, offset); }
void b_store(Bytes &c, uint32_t offset) { b_op(c, Store); uleb(c, 2); uleb(c, offset); }

bool scalar(Type type) {
    return type == Type::U1 || type == Type::U8 || type == Type::U16 || type == Type::U32 || type == Type::U64 || type == Type::NZCVFlags;
}

bool arithmetic(Op op) {
    return op == Op::Add32 || op == Op::Sub32;
}

bool shift(Op op) {
    return op == Op::LogicalShiftLeft32 || op == Op::LogicalShiftRight32
        || op == Op::ArithmeticShiftRight32 || op == Op::RotateRight32 || op == Op::RotateRightExtended;
}

// Region dispatch-index sentinel (light dispatch path, REGION_ABI.md v1.2):
// loop iterations start with local 6 holding this value, which routes them
// through the generic PC reload + static search. Statically-chained edges
// overwrite local 6 with the successor's constant block index before
// branching, so they skip the reload and the search entirely.
constexpr uint32_t kLightDispatchSentinel = 0xffffffffu;
// member_index() result for a location that is not a region member.
constexpr uint32_t kNoMember = 0xffffffffu;
constexpr uint32_t kRegionRegBase = 11;
constexpr uint32_t kCachedRegCount = 15;
constexpr uint32_t kRegionSsaBase = kRegionRegBase + kCachedRegCount;

// Emission-time policy shared by bodies, dispatch, entry and the epilogue.
// Reference methods emit the original memory operations. Promoted state is
// private to ONE run invocation; no locals cross M16 call_indirect boundaries.
class RegionState {
public:
    explicit RegionState(RegionStateOptions options = {}) : options(options) {}

    uint32_t ssa_base() const {
        return options.promote_flags || options.promote_accounting ? kNextPc + 1 : kRegionSsaBase;
    }
    // True when the emitter may drop the per-access fast-path-enabled
    // guard (T3): the host proved all bases nonzero for this module's
    // lifetime. Single-block emission always keeps the guard.
    bool skip_fast_guard() const { return options.assume_fast_bases; }
    void load_region_state(Bytes &c) const {
        reload_flags(c);
        if (options.promote_accounting) {
            b_load(c, kPcOffset); b_set(c, kPc);
            // Fault exits intentionally leave next_pc untouched. Initialize
            // from memory rather than synthesizing it from regs[15].
            b_load(c, offsetof(JitState, next_pc)); b_set(c, kNextPc);
        }
    }
    void reload_flags(Bytes &c) const {
        if (!options.promote_flags) return;
        b_load(c, offsetof(JitState, cpsr)); b_set(c, kOtherPsr);
        for (unsigned bit = 28; bit <= 31; ++bit) {
            b_get(c, kOtherPsr); b_imm(c, bit); b_op(c, ShrU);
            b_imm(c, 1); b_op(c, And); b_set(c, flag_local(bit));
        }
        b_get(c, kOtherPsr); b_imm(c, ~kNzcv); b_op(c, And); b_set(c, kOtherPsr);
    }
    void read_flag(Bytes &c, unsigned bit) const {
        if (options.promote_flags) {
            b_get(c, flag_local(bit));
        } else {
            b_load(c, offsetof(JitState, cpsr)); b_imm(c, bit); b_op(c, ShrU);
            b_imm(c, 1); b_op(c, And);
        }
    }
    // packed() emits a CPSR-positioned NZ/NZCV value; carry() emits a U1.
    // NZ and NZC must not overwrite preserved C/V locals.
    template <typename Packed, typename Carry>
    void write_nzcv(Bytes &c, uint32_t bits, const Packed &packed, const Carry &carry) const {
        if (!options.promote_flags) {
            b_get(c, 0); b_load(c, offsetof(JitState, cpsr)); b_imm(c, ~bits); b_op(c, And);
            packed(); b_imm(c, bits == 0xe0000000 ? 0xc0000000 : bits); b_op(c, And); b_op(c, Or);
            if (bits == 0xe0000000) { carry(); b_imm(c, 29); b_op(c, Shl); b_op(c, Or); }
            b_store(c, offsetof(JitState, cpsr));
            return;
        }
        for (unsigned bit = 28; bit <= 31; ++bit) {
            if (!(bits & (1u << bit))) continue;
            if (bits == 0xe0000000 && bit == 29) {
                carry(); // IR U1; reference lowering also relies on this type.
            } else {
                packed(); b_imm(c, bit); b_op(c, ShrU); b_imm(c, 1); b_op(c, And);
            }
            b_set(c, flag_local(bit));
        }
    }
    template <typename Value>
    void write_psr_field(Bytes &c, uint32_t bits, const Value &value) const {
        // Only modeled T/E/IT updates use this operation. NZCV have their
        // own representation; full CPSR/Q/GE writers remain unsupported IR.
        if (options.promote_flags) {
            b_get(c, kOtherPsr); b_imm(c, ~bits); b_op(c, And);
            value(); b_op(c, Or); b_set(c, kOtherPsr);
        } else {
            b_get(c, 0); b_load(c, offsetof(JitState, cpsr)); b_imm(c, ~bits); b_op(c, And);
            value(); b_op(c, Or); b_store(c, offsetof(JitState, cpsr));
        }
    }
    void read_dispatch_psr(Bytes &c) const {
        // Entry masks contain only T/E/IT, never arithmetic flags.
        if (options.promote_flags) b_get(c, kOtherPsr);
        else b_load(c, offsetof(JitState, cpsr));
    }
    void materialize_flags(Bytes &c) const {
        if (!options.promote_flags) return;
        // Conservative dirty policy: initialized locals are authoritative
        // until reload; publish them even on a zero-work exit. No path-sensitive
        // C++ dirty bit may describe dynamically taken Wasm branches.
        b_get(c, 0); b_get(c, kOtherPsr);
        for (unsigned bit = 28; bit <= 31; ++bit) {
            b_get(c, flag_local(bit)); b_imm(c, bit); b_op(c, Shl); b_op(c, Or);
        }
        b_store(c, offsetof(JitState, cpsr));
    }
    void read_full_cpsr(Bytes &c) const {
        materialize_flags(c);
        b_load(c, offsetof(JitState, cpsr));
    }
    void read_pc(Bytes &c) const {
        if (options.promote_accounting) b_get(c, kPc);
        else b_load(c, kPcOffset);
    }
    template <typename Value>
    void write_pc(Bytes &c, const Value &value) const {
        write_accounting(c, kPcOffset, kPc, value);
    }
    template <typename Value>
    void set_next_pc(Bytes &c, const Value &value) const {
        write_accounting(c, offsetof(JitState, next_pc), kNextPc, value);
    }
    void set_next_pc_from_pc(Bytes &c) const {
        set_next_pc(c, [&] { read_pc(c); });
    }
    void read_executed(Bytes &c) const { b_get(c, 2); }
    void add_ticks(Bytes &c, uint32_t ticks) const {
        read_executed(c); b_imm(c, ticks); b_op(c, Add); b_set(c, 2);
    }
    void materialize_accounting(Bytes &c) const {
        if (options.promote_accounting) {
            b_get(c, 0); b_get(c, kPc); b_store(c, kPcOffset);
            b_get(c, 0); b_get(c, kNextPc); b_store(c, offsetof(JitState, next_pc));
        }
        // These accumulators were already local in the reference path.
        // Commit ONCE, only in the shared exit epilogue; never before helpers
        // or full CPSR reads (which would double-account completed work).
        b_get(c, 0); b_load(c, offsetof(JitState, executed)); read_executed(c); b_op(c, Add);
        b_store(c, offsetof(JitState, executed));
        b_get(c, 0); b_load(c, offsetof(JitState, dispatches)); b_get(c, 7); b_op(c, Add);
        b_store(c, offsetof(JitState, dispatches));
    }
    void materialize_all_on_exit(Bytes &c) const {
        materialize_flags(c);
        materialize_accounting(c);
    }
    void before_checked_memory_helper() const {
        // Explicit non-observer ABI: checked_memory_read/write may touch
        // memory_value, fault and SMC fields only. They cannot inspect, mutate
        // or reenter CPU state. Therefore no spill/reload is required.
        // Any new observer/mutator helper requires a separate synchronized ABI;
        // it must NOT be routed through this entry point.
    }

private:
    static constexpr uint32_t kNzcv = 0xf0000000;
    static_assert((Location::CPSR_MODE_MASK & kNzcv) == 0);
    static constexpr uint32_t kOtherPsr = kRegionSsaBase + 4;
    static constexpr uint32_t kPc = kOtherPsr + 1, kNextPc = kPc + 1;
    static constexpr uint32_t kPcOffset = offsetof(JitState, regs) + 15 * sizeof(uint32_t);
    static uint32_t flag_local(unsigned bit) { return kRegionSsaBase + (31 - bit); }
    template <typename Value>
    void write_accounting(Bytes &c, uint32_t offset, uint32_t local, const Value &value) const {
        if (options.promote_accounting) { value(); b_set(c, local); }
        else { b_get(c, 0); value(); b_store(c, offset); }
    }
    RegionStateOptions options;
};

// Task #10 benchmark-only ablation harness (fenced, default-off).
// Bit-identical default: ablate_flags() == 0 unless VITA3K_ABLATE env or the
// test-only set_ablate_flags() setter opts in. Letters: B=skip SMC polling,
// C=skip per-edge budget checks (single slice bound kept), D=skip PSR/FPSCR
// entry validation, F=B+C+D, E=direct-thread member back-edges,
// G=E+C+D+loop-top-SMC-hoist (never B: store-point SMC kept). Read once per process for the env part so runs
// stay deterministic; the setter ORs in (bench uses env only).
constexpr uint32_t kAblateSmc = 1u;
constexpr uint32_t kAblateBudget = 2u;
constexpr uint32_t kAblatePsr = 4u;
constexpr uint32_t kAblateDirect = 8u;
constexpr uint32_t kAblateHoistSmc = 16u;
} // namespace
// Exported override + flag reader live in wasmjit scope (header-declared).
// Anonymous-namespace constants above stay visible here (same TU).
uint32_t g_ablate_override = 0;
void set_ablate_flags(uint32_t flags) { g_ablate_override = flags; }
RegionStateOptions region_state_options() {
    static const RegionStateOptions options = [] {
#ifdef __EMSCRIPTEN__
        const uint32_t flags = vita3k_promoted_state_options();
        return RegionStateOptions{(flags & 1) != 0, (flags & 2) != 0};
#else
        const char *both = std::getenv("VITA3K_WASMJIT_PROMOTED_STATE");
        const bool fallback = both && std::strcmp(both, "1") == 0;
        const auto enabled = [&](const char *name, bool def) {
            const char *value = std::getenv(name);
            return value ? std::strcmp(value, "1") == 0 : def;
        };
        // Production default (R2): promoted flags ON unless explicitly "0".
        return RegionStateOptions{enabled("VITA3K_WASMJIT_PROMOTE_FLAGS", true),
            enabled("VITA3K_WASMJIT_PROMOTE_ACCOUNTING", fallback)};
#endif
    }();
    return options;
}
namespace {
uint32_t ablate_flags() {
    static uint32_t env_flags = []() -> uint32_t {
#ifdef __EMSCRIPTEN__
        return vita3k_ablate_env_flags();
#else
        uint32_t f = 0;
        if (const char *e = std::getenv("VITA3K_ABLATE")) {
            for (const char *p = e; *p; ++p) {
                if (*p == 'B' || *p == 'b')
                    f |= kAblateSmc;
                else if (*p == 'C' || *p == 'c')
                    f |= kAblateBudget;
                else if (*p == 'D' || *p == 'd')
                    f |= kAblatePsr;
                else if (*p == 'F' || *p == 'f')
                    f |= (kAblateSmc | kAblateBudget | kAblatePsr);
                else if (*p == 'E' || *p == 'e')
                    f |= kAblateDirect;
                else if (*p == 'G' || *p == 'g')
                    f |= (kAblateDirect | kAblateHoistSmc | kAblateBudget | kAblatePsr);
            }
        }
        return f;
#endif
    }();
    return env_flags | g_ablate_override;
}
uint32_t ablate_entry_env() {
    static uint32_t entry = []() -> uint32_t {
#ifdef __EMSCRIPTEN__
        return vita3k_ablate_entry_pc();
#else
        if (const char *e = std::getenv("VITA3K_ABLATE_PC"))
            return static_cast<uint32_t>(std::strtoul(e, nullptr, 16));
        return 0;
#endif
    }();
    return entry;
}
namespace {
bool slow_reason_detail() {
    static bool detail = []() -> bool {
#ifdef __EMSCRIPTEN__
        return vita3k_slow_reason_detail() != 0;
#else
        if (const char *e = std::getenv("VITA3K_WASMJIT_SLOW_REASONS"))
            return std::strcmp(e, "1") == 0;
        return false;
#endif
    }();
    return detail;
}
} // namespace


uint32_t entry_ticks(const RegionBlockMeta &meta) {
    // Continued blocks are unconditional. Admit the first store-delimited
    // segment; each continuation checks the cumulative cost of the next one.
    return meta.store_continuations.empty() ? meta.ticks
                                          : meta.store_continuations.front().completed_ticks;
}

class Emitter {
public:
    explicit Emitter(const Dynarmic::IR::Block &block)
        : block(block), start(block.Location()), finish(block.EndLocation()) {}

    // Region layout (REGION_ABI.md): run(state=0, budget=1) with locals
    // 2=executed_call, 3=pc, 4=CheckBit, 5=i64 scratch, 6=dispatch index,
    // 7=dispatch count, 8..10=hoisted memory bases, 11..25=registers
    // retained across bodies; RegionState reserves candidate locals above
    // these only when enabled, followed by per-block SSA words.
    Emitter(const Dynarmic::IR::Block &block, unsigned index,
        const std::vector<const Dynarmic::IR::Block *> &members,
        const std::vector<RegionBlockMeta> &metadata, RegionStateOptions options = {})
        : block(block), start(block.Location()), finish(block.EndLocation())
        , state(options)
        , region(true), body_index(index), members(&members), metadata(&metadata) {
        check_bit_local = 4;
        scratch_local = 5;
        page_table_local = 8;
        page_perms_local = 9;
        code_pages_local = 10;
        reg_base = kRegionRegBase;
        ssa_base = state.ssa_base();
        next_local = ssa_base;
    }

    uint32_t meta_entry_pc_for_dbg() const { return start.PC(); }
    Bytes run() {
        bool has_memory = false;
        for (const Inst &inst : block) {
            const auto opcode = inst.GetOpcode();
            has_memory |= opcode == Op::A32ReadMemory8 || opcode == Op::A32ReadMemory16
                || opcode == Op::A32ReadMemory32 || opcode == Op::A32ReadMemory64
                || opcode == Op::A32WriteMemory8 || opcode == Op::A32WriteMemory16
                || opcode == Op::A32WriteMemory32 || opcode == Op::A32WriteMemory64;
        }
        if (has_memory && block.CycleCount() != 1)
            return {};
        if (block.size() > 4096 || block.CycleCount() == 0 || block.CycleCount() > 4096
            || block.ConditionFailedCycleCount() > 4096 || !valid_location(start) || !valid_location(finish))
            return {};
        analyze_register_cache();

        store_constant(offsetof(JitState, svc), 0);
        store_constant(offsetof(JitState, exit_reason), 0);
        store_constant(offsetof(JitState, executed), 0);

        if (block.GetCondition() != Cond::AL) {
            if (!block.HasConditionFailedLocation() || block.ConditionFailedCycleCount() == 0)
                return {};
            condition(block.GetCondition());
            op(Eqz);
            begin_if();
            location(Location(block.ConditionFailedLocation()));
            store_constant(offsetof(JitState, executed), static_cast<uint32_t>(block.ConditionFailedCycleCount()));
            ret(ExitReason::Continue);
            end_if();
        }

        for (const Inst &inst : block)
            has_bx |= inst.GetOpcode() == Op::A32BXWritePC;
        initialize_register_cache();
        for (const Inst &inst : block) {
            // SVC must be the final side effect: the host handles it on return.
            if (svc || !instruction(inst)) {
                return {};
            }
            locals.emplace(&inst, next_local);
            next_local += 10; // four value words, carry/GE, overflow, spare
        }
        store_constant(offsetof(JitState, executed), static_cast<uint32_t>(block.CycleCount()));
        terminal(block.GetTerminal(), 0);
        if (!ok)
            return {};
        op(End);

        Bytes body;
        uleb(body, 3); // CheckBit, i64 scratch, then cached registers and SSA words
        uleb(body, 1); body.push_back(0x7f);
        uleb(body, 1); body.push_back(0x7e);
        uleb(body, next_local - 3); body.push_back(0x7f);
        body.insert(body.end(), code.begin(), code.end());
        Bytes functions{1};
        uleb(functions, static_cast<uint32_t>(body.size()));
        functions.insert(functions.end(), body.begin(), body.end());

        Bytes module{0, 'a', 's', 'm', 1, 0, 0, 0};
        section(module, 1, {2, 0x60, 1, 0x7f, 1, 0x7f,
            0x60, 3, 0x7f, 0x7f, 0x7f, 1, 0x7f}); // block and checked helpers
        section(module, 2, {3,
            3, 'e', 'n', 'v', 6, 'm', 'e', 'm', 'o', 'r', 'y', 2, 0, 1,
            3, 'e', 'n', 'v', 8, 'm', 'e', 'm', '_', 'r', 'e', 'a', 'd', 0, 1,
            3, 'e', 'n', 'v', 9, 'm', 'e', 'm', '_', 'w', 'r', 'i', 't', 'e', 0, 1});
        section(module, 3, {1, 0}); // one function, type 0
        section(module, 7, {1, 5, 'b', 'l', 'o', 'c', 'k', 0, 2});
        section(module, 10, functions);
        return module;
    }

private:
    const Dynarmic::IR::Block &block;
    Location start, finish;
    Bytes code;
    RegionState state; // single-block emission always uses the reference policy
    std::unordered_map<const Inst *, uint32_t> locals;
    // Single-block layout: 1=CheckBit, 2=i64 scratch, 3..17=registers,
    // SSA from 18. Architectural registers must never alias SSA temporaries.
    uint32_t check_bit_local = 1, scratch_local = 2, ssa_base = 3 + kCachedRegCount;
    uint32_t page_table_local = 0, page_perms_local = 0, code_pages_local = 0;
    uint32_t reg_base = 3;
    std::array<bool, kCachedRegCount> cached_regs{};
    std::array<bool, kCachedRegCount> dirty_regs{};
    uint32_t next_local = ssa_base;
    unsigned terminal_nodes = 0;
    bool ok = true;
public:
    std::string rejection;
private:
    void reject(const char *why) { ok = false; if (rejection.empty()) rejection = why; }
    bool svc = false;
    bool pc_written = false;
    bool has_bx = false;
    bool check_bit_written = false;
    // Region mode state.
    bool region = false;
    unsigned body_index = 0;   // br-to-dispatch depth equals the body index
    unsigned extra_labels = 0; // all open ifs, including memory probes
    const std::vector<const Dynarmic::IR::Block *> *members = nullptr;
    const std::vector<RegionBlockMeta> *metadata = nullptr;
    uint32_t completed_store_ticks = 0;

    void op(uint8_t byte) { code.push_back(byte); }
    void imm(uint32_t n) { constant(code, n); }
    void get(uint32_t index) { op(Get); uleb(code, index); }
    void set(uint32_t index) { op(Set); uleb(code, index); }
    void begin_if(bool result = false) { op(If); op(result ? 0x7f : 0x40); ++extra_labels; }
    void end_if() { op(End); --extra_labels; }
    void load(uint32_t offset) { get(0); op(Load); uleb(code, 2); uleb(code, offset); }
    void store(uint32_t offset) { op(Store); uleb(code, 2); uleb(code, offset); }
    void store_constant(uint32_t offset, uint32_t n) { get(0); imm(n); store(offset); }
    void mask(uint32_t bits) { imm(bits); op(And); }
    void analyze_register_cache() {
        // The cache is a region-mode optimization: its locals (11..25) are
        // reserved by the region constructor above the SSA range. In
        // single-block mode SSA words start at 3 and would collide, so the
        // cache stays disabled there (get/set touch state.regs directly).
        if (!region)
            return;
        for (const Inst &inst : block) {
            if (inst.GetOpcode() != Op::A32GetRegister
                && inst.GetOpcode() != Op::A32SetRegister)
                continue;
            const auto reg = inst.GetArg(0);
            if (reg.IsImmediate() && reg.GetType() == Type::A32Reg) {
                const auto index = static_cast<uint32_t>(reg.GetA32RegRef());
                if (index < cached_regs.size())
                    cached_regs[index] = true;
            }
        }
    }
    void initialize_register_cache() {
        for (uint32_t i = 0; i < cached_regs.size(); ++i) {
            if (!cached_regs[i])
                continue;
            load(offsetof(JitState, regs) + i * sizeof(uint32_t));
            set(reg_base + i);
        }
    }
    void flush_register_cache() {
        for (uint32_t i = 0; i < dirty_regs.size(); ++i) {
            if (!dirty_regs[i])
                continue;
            get(0); get(reg_base + i);
            store(offsetof(JitState, regs) + i * sizeof(uint32_t));
        }
    }
    void checked_status() {
        // The helper result is still on the stack: branch on it directly
        // without staging through a local (in region layout local 1 is the
        // budget and must never be clobbered).
        op(Eqz); begin_if();
        op(Else);
        if (region) {
            // Fault mid-region: recover the faulting instruction's mode and PC
            // (arg0 of the memory op is its own U64 location descriptor) and
            // return after publishing completed-block ticks (the current
            // block has not reached its terminal) and WITHOUT rollback
            // (REGION_ABI fault contract).
            const Location fault_location{Dynarmic::IR::LocationDescriptor{pending_fault_location}};
            // IT may have advanced since block entry. Preserve the current
            // arithmetic flags while restoring the faulting instruction's mode.
            upper_location(fault_location);
            store_constant(offsetof(JitState, fault_pc), fault_location.PC());
            // Earlier store-delimited segments have completed, exactly as
            // they did when every store ended a separate block. The current
            // segment (and any partial multi-access instruction) is uncounted.
            if (completed_store_ticks)
                add_ticks(completed_store_ticks);
        } else {
            store_constant(offsetof(JitState, executed), 0);
        }
        ret(ExitReason::Fault);
        end_if();
    }
    // This Dynarmic IR prefixes every A32 memory op with the translation
    // location as an immediate and appends the access type after the data:
    // A32ReadMemoryN(imm loc:U64, vaddr:U32, imm acctype) and
    // A32WriteMemoryN(imm loc:U64, vaddr:U32, value, imm acctype). The guest
    // address is therefore arg1 and the stored data arg2; arg0 is NOT the
    // address (its low word is the instruction's own PC). Write helpers
    // consume memory_value, so the value must be published BEFORE the call.
    uint64_t pending_fault_location = 0;

    // M15 inline memory fast path (REGION_ABI.md "Inline memory fast path").
    // 1/2/4-byte accesses lower INLINE when provably equivalent to the checked
    // path: page-table lookup + permission/refcount probes + direct Wasm
    // load/store against the sparse page backing. Any doubt falls back to the
    // imported helper, which keeps whole-range preflight, fault and SMC
    // semantics; the fallback REASON is encoded in the high byte of the
    // helper's `bytes` argument (1=unmapped, 2=perms, 3=cross-page, 4=code
    // page, 5=other/disabled) and masked off by the helper. Alignment is
    // never checked: Wasm unaligned access is a little-endian byte-wise access
    // with zero-extending narrow loads, exactly mem_read/mem_write semantics
    // (every A32 load width lowers to a raw zero-extending ReadMemoryN;
    // sign extension is separate IR).
    enum : uint32_t {
        kSlowUnmapped = 1,
        kSlowPerms = 2,
        kSlowCrossPage = 3,
        kSlowCodePage = 4,
        kSlowOther = 5,
    };
    // The checked-helper call, shared by the always-slow widths and every
    // fast-path fallback. `reason` rides in the `bytes` argument's high byte.
    void memory_slow_call(const Inst &inst, bool write, unsigned bytes, uint32_t reason = 0) {
        if (write) {
            const auto &value = inst.GetArg(2);
            get(0); value_word(value, 0);
            op(Store); uleb(code, 2); uleb(code, offsetof(JitState, memory_value));
            if (bytes > 4) {
                get(0); value_word(value, 1);
                op(Store); uleb(code, 2); uleb(code, offsetof(JitState, memory_value) + 4);
            }
        }
        state.before_checked_memory_helper();
        get(0); value_word(inst.GetArg(1)); imm(bytes | (reason << 8)); op(Call); uleb(code, write ? 1 : 0);
        checked_status();
        if (!write) {
            for (unsigned i = 0; i < (bytes + 3) / 4; ++i) { get(0); op(Load); uleb(code, 2); uleb(code, offsetof(JitState, memory_value) + i * 4); set(next_local + i); }
        }
    }
    void memory_base(uint32_t local, uint32_t offset) {
        if (region)
            get(local);
        else
            load(offset);
    }
    // Probe chain: page 0 -> fast-path enabled -> permissions -> mapped ->
    // page boundary -> (stores) code-page refcount. All ifs are void; every
    // arm fully defines the IR result (fast load sets it directly, fallbacks
    // via the helper's memory_value), so nothing stays on the cross-arm stack.
    // The mapped base, page and address live in three SPARE SSA words of
    // this instruction's ten-word slot (other lowerings use words 0..6).
    void memory_fast_or_slow(const Inst &inst, bool write, unsigned bytes) {
        const uint32_t base_local = next_local + 7;
        const uint32_t page_local = next_local + 8;
        const uint32_t addr_local = next_local + 9;
        // MemPerm::ReadOnly(1) / WriteOnly(2): the bits mem_read/mem_write
        // require of every touched page (mem/functions.h).
        const uint32_t required = write ? 2 : 1;
        value_word(inst.GetArg(1));
        set(addr_local);
        get(addr_local); imm(12); op(ShrU); set(page_local);

        // Branchless single-gate probe (R3f). Every probe load below is
        // provably in-bounds: the page index is 20 bits into fixed
        // 1M-entry/4M-byte arrays whose bases are either proven nonzero
        // for the module's lifetime (assume_fast_bases, region) or
        // re-checked by the kept enabled guard just below. Evaluating all
        // predicates speculatively has no observable effect (pure loads),
        // so the five nested taken-never branches collapse into one hot
        // gate. The cold arm re-derives the first-failing reason in probe
        // order (exact profiling) and takes the identical slow path.
        //
        // Predicate order matches the checked helper's preflight: page
        // nonzero (addr < host_page_size rejects even when sparse backing
        // was force-allocated at page 0), permissions (the only failure
        // detected before mapping), mapped (null PTE = unallocated),
        // in-page (offset + size <= 4096; the only cross-page path) and,
        // for stores, code-page refcount == 0 (cached code stores must own
        // smc_dirty via the helper).
        const bool emit_fast_guard = !(region && state.skip_fast_guard());
        if (emit_fast_guard) {
            memory_base(page_table_local, offsetof(JitState, page_table_base)); op(Eqz);
            memory_base(page_perms_local, offsetof(JitState, page_perms_base)); op(Eqz); op(Or);
            memory_base(code_pages_local, offsetof(JitState, code_pages_base)); op(Eqz); op(Or);
            begin_if();
            memory_slow_call(inst, write, bytes, kSlowOther);
            op(Else);
        }
        // ok = page_nonzero & perm_ok & mapped & in_page [& code_ok].
        get(page_local); op(Eqz); op(Eqz);
        memory_base(page_perms_local, offsetof(JitState, page_perms_base));
        get(page_local); op(Add);
        op(Load8U); uleb(code, 0); uleb(code, 0);
        imm(required); op(And); imm(required); op(Eq);
        op(And);
        memory_base(page_table_local, offsetof(JitState, page_table_base));
        get(page_local); imm(2); op(Shl); op(Add);
        op(Load); uleb(code, 2); uleb(code, 0);
        set(base_local);
        get(base_local); op(Eqz); op(Eqz);
        op(And);
        get(addr_local); imm(0xfff); op(And); imm(4096 - bytes); op(GtU); op(Eqz);
        op(And);
        if (write) {
            memory_base(code_pages_local, offsetof(JitState, code_pages_base));
            get(page_local); imm(2); op(Shl); op(Add);
            op(Load); uleb(code, 2); uleb(code, 0);
            op(Eqz);
            op(And);
        }
        op(Eqz);
        begin_if();
        memory_slow_reason(inst, write, bytes, emit_fast_guard, addr_local, page_local, base_local);
        op(Else);
        if (write) {
            // Direct store: base + (addr & 0xfff).
            get(base_local);
            get(addr_local); imm(0xfff); op(And); op(Add);
            value_word(inst.GetArg(2), 0);
            if (bytes == 1) { op(Store8); uleb(code, 0); uleb(code, 0); }
            else if (bytes == 2) { op(Store16); uleb(code, 1); uleb(code, 0); }
            else { op(Store); uleb(code, 2); uleb(code, 0); }
            get(0); load(offsetof(JitState, mem_fast_writes)); imm(1); op(Add);
            store(offsetof(JitState, mem_fast_writes));
        } else {
            // Direct load: base + (addr & 0xfff); 8/16-bit zero-extending.
            get(base_local);
            get(addr_local); imm(0xfff); op(And); op(Add);
            if (bytes == 1) { op(Load8U); uleb(code, 0); uleb(code, 0); }
            else if (bytes == 2) { op(Load16U); uleb(code, 1); uleb(code, 0); }
            else { op(Load); uleb(code, 2); uleb(code, 0); }
            set(next_local);
            get(0); load(offsetof(JitState, mem_fast_reads)); imm(1); op(Add);
            store(offsetof(JitState, mem_fast_reads));
        }
        end_if(); // single probe gate
        if (emit_fast_guard)
            end_if(); // fast-path enabled
    }

    // Cold arm of the single probe gate: re-derive the first-failing
    // reason in probe order and take the identical slow path. The hot gate
    // guarantees at least one predicate fails (same values, no calls in
    // between), so the trailing else is unreachable; it still calls the
    // helper (correct, conservatively profiled) rather than leaving the
    // result undefined. base_local was set unconditionally by the hot
    // gate and is reused for the mapping re-check.
    void memory_slow_reason(const Inst &inst, bool write, unsigned bytes, bool emit_fast_guard,
        uint32_t addr_local, uint32_t page_local, uint32_t base_local) {
        // Production shape: one slow call, reason always Other. The exact
        // reason only feeds process profiling counters (the helper masks it
        // off before use), and the nested re-derive below doubles module
        // size and halves speed, so it lives behind VITA3K_WASMJIT_SLOW_REASONS.
        if (!slow_reason_detail()) {
            memory_slow_call(inst, write, bytes, kSlowOther);
            return;
        }
        const uint32_t required = write ? 2 : 1;
        get(page_local); op(Eqz);
        begin_if();
        memory_slow_call(inst, write, bytes, kSlowOther);
        op(Else);
        if (emit_fast_guard) {
            memory_base(page_table_local, offsetof(JitState, page_table_base)); op(Eqz);
            memory_base(page_perms_local, offsetof(JitState, page_perms_base)); op(Eqz); op(Or);
            memory_base(code_pages_local, offsetof(JitState, code_pages_base)); op(Eqz); op(Or);
            begin_if();
            memory_slow_call(inst, write, bytes, kSlowOther);
            op(Else);
        }
        memory_base(page_perms_local, offsetof(JitState, page_perms_base));
        get(page_local); op(Add);
        op(Load8U); uleb(code, 0); uleb(code, 0);
        imm(required); op(And); imm(required); op(Ne);
        begin_if();
        memory_slow_call(inst, write, bytes, kSlowPerms);
        op(Else);
        get(base_local);
        op(Eqz);
        begin_if();
        memory_slow_call(inst, write, bytes, kSlowUnmapped);
        op(Else);
        get(addr_local); imm(0xfff); op(And); imm(4096 - bytes); op(GtU);
        begin_if();
        memory_slow_call(inst, write, bytes, kSlowCrossPage);
        op(Else);
        if (write) {
            memory_base(code_pages_local, offsetof(JitState, code_pages_base));
            get(page_local); imm(2); op(Shl); op(Add);
            op(Load); uleb(code, 2); uleb(code, 0);
            op(Eqz); op(Eqz);
            begin_if();
            memory_slow_call(inst, write, bytes, kSlowCodePage);
            op(Else);
        }
        memory_slow_call(inst, write, bytes, kSlowOther); // unreachable; safe fallback
        if (write) {
            end_if();
        }
        end_if(); // page boundary
        end_if(); // mapping
        end_if(); // permissions
        if (emit_fast_guard)
            end_if(); // fast-path enabled
        end_if(); // guest page 0
    }

    void memory_call(const Inst &inst, bool write, unsigned bytes) {
        if (inst.GetArg(1).GetType() != Type::U32) { reject("memory_call arg1 not U32"); return; }
        if (!inst.GetArg(0).IsImmediate()) { reject("memory_call arg0 not imm"); return; }
        pending_fault_location = inst.GetArg(0).GetImmediateAsU64();
        if (bytes <= 4)
            memory_fast_or_slow(inst, write, bytes);
        else
            memory_slow_call(inst, write, bytes);
    }

    void value_word(const Value &v, unsigned word = 0) {
        const auto type = v.GetType();
        const unsigned words = type == Type::U128 ? 4 : type == Type::U64 ? 2 : 1;
        if (word >= words) { reject("value word>=words"); return; }
        if (v.IsImmediate()) {
            if (type == Type::NZCVFlags || type == Type::Opaque || type == Type::Void) { reject("value NZCV/Opaque/Void"); return; }
            imm(static_cast<uint32_t>(v.GetImmediateAsU64() >> (word * 32)));
        } else {
            const auto it = locals.find(v.GetInstRecursive());
            if (it == locals.end()) { reject("value producer missing"); return; }
            get(it->second + word);
        }
    }

    void value(const Value &v) { value_word(v); }
    void value64(const Value &v) {
        value_word(v, 0); op(ExtendU);
        value_word(v, 1); op(ExtendU); op(0x42); uleb(code, 32); op(Shl64); op(Or64);
    }

    bool valid_location(const Location &loc) const {
        return (loc.TFlag() || loc.IT().Value() == 0) && (loc.PC() & (loc.TFlag() ? 1 : 3)) == 0
            && loc.FPSCR() == start.FPSCR();
    }

    void upper_location(const Location &loc) {
        if (!valid_location(loc)) {
            ok = false;
            return;
        }
        state.write_psr_field(code, Location::CPSR_MODE_MASK, [&] {
            imm(loc.CPSR().Value() & Location::CPSR_MODE_MASK);
        });
    }

    void location(const Location &loc) {
        upper_location(loc);
        state.write_pc(code, [&] { imm(loc.PC()); });
    }
    // Member-edge publish: when the target's CPSR mode bits equal the
    // block's end-of-body bits, the mode RMW is idempotent and skipped.
    // The PC write always stays: edge Budget/Stop/Smc exits publish
    // next_pc from regs[15], and the next block consumes the pending PC.
    // Sound by the light-edge invariant (member_index requires full
    // descriptor equality; other_psr provably holds the current end bits
    // via run-entry reload, in-body maintenance and chained exact-match
    // induction), and member targets are validated at their own emission.
    // Non-member (Miss) paths keep the full location() write.
    void location_member(const Location &loc) {
        constexpr uint32_t mask = Location::CPSR_MODE_MASK;
        if ((loc.CPSR().Value() & mask) == (finish.CPSR().Value() & mask)) {
            state.write_pc(code, [&] { imm(loc.PC()); });
            return;
        }
        location(loc);
    }

    void flag(unsigned bit) {
        state.read_flag(code, bit);
    }

    void condition(Cond cond) {
        switch (cond) {
        case Cond::EQ: flag(30); break;
        case Cond::NE: flag(30); op(Eqz); break;
        case Cond::CS: flag(29); break;
        case Cond::CC: flag(29); op(Eqz); break;
        case Cond::MI: flag(31); break;
        case Cond::PL: flag(31); op(Eqz); break;
        case Cond::VS: flag(28); break;
        case Cond::VC: flag(28); op(Eqz); break;
        case Cond::HI: flag(29); flag(30); op(Eqz); op(And); break;
        case Cond::LS: flag(29); op(Eqz); flag(30); op(Or); break;
        case Cond::GE: flag(31); flag(28); op(Eq); break;
        case Cond::LT: flag(31); flag(28); op(Ne); break;
        case Cond::GT: flag(30); op(Eqz); flag(31); flag(28); op(Eq); op(And); break;
        case Cond::LE: flag(30); flag(31); flag(28); op(Ne); op(Or); break;
        case Cond::AL: imm(1); break;
        default: reject("cond NV"); break; // NV is not an unconditional alias
        }
    }

    void ret(ExitReason reason) {
        if (region) {
            // All exits join the epilogue outside the dispatch loop. Reuse
            // the dispatch-index local for the return reason once dispatch
            // has finished. This also keeps register spills out of each
            // memory fallback and each search-tree leaf.
            imm(static_cast<uint32_t>(reason)); set(6);
            op(Br); uleb(code, body_index + extra_labels + 1);
            return;
        }
        flush_register_cache();
        store_constant(offsetof(JitState, exit_reason), static_cast<uint32_t>(reason));
        imm(static_cast<uint32_t>(reason));
        op(Return);
    }

    void terminal(const Term::Terminal &term, unsigned depth) {
        if (depth > 16 || ++terminal_nodes > 256) {
            ok = false;
            return;
        }
        if (const auto *link = boost::get<Term::LinkBlock>(&term)) {
            // LinkBlock carries the architecturally sequential next location;
            // ordinary memory/VFP instructions do not emit an explicit
            // SetRegister(PC), so the terminal itself owns the PC write.
            location(Location(link->next));
            ret(ExitReason::Continue);
        } else if (const auto *fast = boost::get<Term::LinkBlockFast>(&term)) {
            location(Location(fast->next));
            ret(ExitReason::Continue);
        } else if (const auto *test = boost::get<Term::If>(&term)) {
            condition(test->if_);
            begin_if();
            terminal(test->then_, depth + 1);
            op(Else);
            terminal(test->else_, depth + 1);
            end_if();
            // Both arms return; Wasm's validator still requires a value after
            // a void if when validating the enclosing function's result type.
            op(0x00); // unreachable
        } else if (const auto *test = boost::get<Term::CheckBit>(&term)) {
            if (!check_bit_written) { reject("single check_bit unwritten"); return; }
            get(check_bit_local); begin_if();
            terminal(test->then_, depth + 1);
            op(Else);
            terminal(test->else_, depth + 1);
            end_if(); op(0x00);
        } else if (boost::get<Term::ReturnToDispatch>(&term) || boost::get<Term::PopRSBHint>(&term)
            || boost::get<Term::FastDispatchHint>(&term)) {
            if (!pc_written)
                reject("single terminal a");
            ret(svc ? ExitReason::Svc : ExitReason::Continue);
        } else if (const auto *halt = boost::get<Term::CheckHalt>(&term)) {
            // No halt field/import: only accept a check whose two outcomes
            // already return to host without further guest-state changes.
            if (!boost::get<Term::ReturnToDispatch>(&halt->else_)
                && !boost::get<Term::PopRSBHint>(&halt->else_)
                && !boost::get<Term::FastDispatchHint>(&halt->else_)) {
                reject("single terminal b");
                return;
            }
            terminal(halt->else_, depth + 1);
        } else {
            reject("single terminal c"); // Invalid, Interpret, CheckBit are never silently skipped
        }
        // SVC cannot be followed by a link/conditional terminal: such a block
        // would resume execution without allowing its host callback to run.
        if (svc && (boost::get<Term::LinkBlock>(&term) || boost::get<Term::LinkBlockFast>(&term) || boost::get<Term::If>(&term) || boost::get<Term::CheckBit>(&term)))
            reject("single terminal d");
    }

    void nz(const Value &v) {
        value(v); mask(0x80000000);
        value(v); op(Eqz); imm(30); op(Shl); op(Or);
    }

    // Flag-word consumer pre-scan: arithmetic/shift producers write carry
    // (+4) and overflow (+5) SSA words unconditionally, but only the
    // GetCarry/GetOverflow/GetNZCV pseudo-ops read them (GetGE is included
    // though its non-PackedAddU8 producer is rejected). The scan runs once
    // per block and marks a superset of real readers; skipping a marked
    // word never happens, and every reader marks its producer, so eliding
    // unmarked words is exact. Measured 75-79% of dynamic carry/overflow
    // writes dead (lower bound) on the display workload.
    bool consumers_scanned = false;
    std::unordered_set<const Inst *> carry_needed, overflow_needed;
    void scan_flag_consumers() {
        if (consumers_scanned) return;
        consumers_scanned = true;
        for (const Inst &inst : block) {
            const auto opcode = inst.GetOpcode();
            const bool carry = opcode == Op::GetCarryFromOp || opcode == Op::GetNZCVFromOp
                || opcode == Op::GetGEFromOp;
            const bool overflow = opcode == Op::GetOverflowFromOp || opcode == Op::GetNZCVFromOp;
            if (!carry && !overflow) continue;
            const auto arg = inst.GetArg(0);
            if (arg.IsImmediate()) continue;
            const auto *producer = arg.GetInstRecursive();
            if (carry) carry_needed.insert(producer);
            if (overflow) overflow_needed.insert(producer);
        }
    }

    void add_sub(const Inst &inst) {
        scan_flag_consumers(); // idempotent; instruction() also scans first
        const bool sub = inst.GetOpcode() == Op::Sub32;
        // ARM subtraction is a + NOT(b) + carry_in. Widen *after* NOT to
        // compute no-borrow carry exactly, including 0xffffffff + 1 cases.
        const auto sum = [&] {
            value(inst.GetArg(0)); op(ExtendU);
            value(inst.GetArg(1));
            if (sub) { imm(0xffffffff); op(Xor); }
            op(ExtendU); op(Add64);
            value(inst.GetArg(2)); op(ExtendU); op(Add64);
        };
        // Emit the widened sum once into the i64 scratch local (dead across
        // IR-op boundaries; Pack/LSR64 use it write-before-read the same way)
        // instead of materializing it twice: saves ~7 Wasm ops per Add/Sub.
        // Dead flag words are elided (see scan_flag_consumers): a plain ADD
        // with no flag consumer keeps only the widened-sum result, saving
        // ~19 Wasm ops (carry extraction, overflow idiom, scratch traffic).
        const bool need_carry = carry_needed.count(&inst) != 0;
        const bool need_overflow = overflow_needed.count(&inst) != 0;
        sum();
        if (need_carry) {
            set(scratch_local);
            get(scratch_local); op(Wrap); set(next_local);
            get(scratch_local); op(0x42); op(32); op(ShrU64); op(Wrap); set(next_local + 4);
        } else {
            op(Wrap); set(next_local);
        }
        if (!need_overflow) return;
        // V = (~(a ^ b) & (a ^ result)) >> 31 for add; for subtract
        // use (a ^ b) instead. This also accounts for carry/borrow input.
        value(inst.GetArg(0)); value(inst.GetArg(1)); op(Xor);
        if (!sub) { imm(0xffffffff); op(Xor); }
        value(inst.GetArg(0)); get(next_local); op(Xor); op(And);
        imm(31); op(ShrU); set(next_local + 5);
    }

    void shifted(const Inst &inst) {
        const auto a = inst.GetArg(0);
        const auto n = inst.GetArg(1);
        const auto carry = inst.GetArg(inst.GetOpcode() == Op::RotateRightExtended ? 1 : 2);
        const Op kind = inst.GetOpcode();
        // Dead shift carry (+4) is elided per arm below (see
        // scan_flag_consumers); the result computation is unchanged.
        scan_flag_consumers(); // idempotent; instruction() also scans first
        const bool need_carry = carry_needed.count(&inst) != 0;
        if (kind == Op::RotateRightExtended) {
            value(a); imm(1); op(ShrU); value(carry); imm(31); op(Shl); op(Or); set(next_local);
            if (need_carry) { value(a); mask(1); set(next_local + 4); }
            return;
        }
        // Every shift count is U8, not Wasm's count modulo 32. Handle zero,
        // exactly 32 and >32 explicitly; only ROR may use modulo semantics.
        value(n); op(Eqz); begin_if();
        value(a); set(next_local);
        if (need_carry) { value(carry); set(next_local + 4); }
        op(Else);
        if (kind == Op::RotateRight32) {
            value(a); value(n); op(RotR); set(next_local);
            if (need_carry) { get(next_local); imm(31); op(ShrU); set(next_local + 4); }
        } else {
            value(n); imm(32); op(LtU); begin_if();
            value(a); value(n);
            op(kind == Op::LogicalShiftLeft32 ? Shl : kind == Op::LogicalShiftRight32 ? ShrU : ShrS);
            set(next_local);
            if (need_carry) {
                value(a);
                if (kind == Op::LogicalShiftLeft32) { imm(32); value(n); op(Sub); }
                else { value(n); imm(1); op(Sub); }
                op(ShrU); mask(1); set(next_local + 4);
            }
            op(Else);
            if (kind == Op::ArithmeticShiftRight32) {
                value(a); imm(31); op(ShrS); set(next_local);
                if (need_carry) { value(a); imm(31); op(ShrU); set(next_local + 4); }
            } else {
                imm(0); set(next_local);
                if (need_carry) {
                    value(n); imm(32); op(Eq); begin_if(true);
                    value(a);
                    if (kind == Op::LogicalShiftRight32) { imm(31); op(ShrU); }
                    else { mask(1); }
                    op(Else); imm(0); end_if(); set(next_local + 4);
                }
            }
            end_if();
        }
        end_if();
    }

    bool pseudo(const Inst &inst) {
        const auto arg = inst.GetArg(0);
        if (inst.GetOpcode() == Op::GetNZFromOp) {
            if (arg.GetType() != Type::U32)
                return false;
            nz(arg);
        } else {
            if (arg.IsImmediate())
                return false;
            const auto *producer = arg.GetInstRecursive();
            const auto it = locals.find(producer);
            if (it == locals.end())
                return false;
            const bool arith = arithmetic(producer->GetOpcode());
            switch (inst.GetOpcode()) {
            case Op::GetGEFromOp:
                if (producer->GetOpcode() != Op::PackedAddU8) return false;
                get(it->second + 4);
                break;
            case Op::GetCarryFromOp:
                if (!arith && !shift(producer->GetOpcode()) && producer->GetOpcode() != Op::MostSignificantWord) return false;
                get(it->second + 4);
                break;
            case Op::GetOverflowFromOp:
                if (!arith) return false;
                get(it->second + 5);
                break;
            case Op::GetNZCVFromOp:
                if (!arith) return false;
                nz(arg);
                get(it->second + 4); imm(29); op(Shl); op(Or);
                get(it->second + 5); imm(28); op(Shl); op(Or);
                break;
            default: return false;
            }
        }
        set(next_local);
        return ok;
    }

    // --- Region mode (REGION_ABI.md) ---------------------------------

    void add_ticks(uint32_t ticks) {
        // Keep per-call accounting in a local. The monotonic state counter is
        // committed once on exit rather than once per completed block.
        state.add_ticks(code, ticks);
    }

    void br_redispatch() {
        // Body i sits inside blocks $b0..$b(i-1) and the dispatch loop, so the
        // loop label is `body_index` labels away, plus any open if/else.
        op(Br); uleb(code, body_index + extra_labels);
    }

    // Re-enter the dispatcher through the generic PC lookup path.  The
    // dispatch index is normally left holding the block selected by the
    // previous iteration; retaining it here would make a non-member edge
    // branch straight back to that same block and skip the PC search.
    void generic_redispatch() {
        imm(kLightDispatchSentinel);
        set(6);
        br_redispatch();
    }

    void set_next_pc_runtime() {
        state.set_next_pc_from_pc(code);
    }

    // Statically-chained edges know the successor's block index at emission
    // time: member_index() requires full LocationDescriptor equality (PC + CPSR
    // mode/IT + FPSCR mode bits), so the dispatch leaf's PC/PSR/FPSCR checks
    // are redundant for them. FULL PSR+FPSCR match, or not a member.
    uint32_t member_index(const Location &loc) const {
        const auto it = std::lower_bound(members->begin(), members->end(), loc.PC(),
            [](const Dynarmic::IR::Block *candidate, uint32_t pc) {
                return Location(candidate->Location()).PC() < pc;
            });
        if (it != members->end() && Location((*it)->Location()) == loc)
            return static_cast<uint32_t>(it - members->begin());
        return kNoMember;
    }

    // Light dispatch path (REGION_ABI.md v1.2): the chained edge preloads the
    // successor's constant block index into the dispatch-index local (6) and
    // branches to the loop top. The next iteration skips the regs[15] reload
    // and the static PC search tree entirely; the per-iteration stop/smc
    // polling and the br_table still run. The edge ALSO pays the successor's
    // budget check (executed_call + ticks > budget -> Budget) with the SAME
    // arithmetic and exit values the skipped search leaf would have used,
    // so budget behavior is 1:1 with the old prologue except that a failing
    // chained iteration is not counted as a dispatch-loop trip.
    // ABLATE E (direct thread): a member edge whose target body is directly
    // reachable with one Wasm br (target index < body_index: br to $b(t)
    // lands at BODY(t), since each body sits just past its block's End)
    // skips the dispatch loop entirely: no dispatches++, no regs[15] reload,
    // no PC search, no br_table. The joined edge keeps the budget check and
    // the stop/smc polls inline (same exit values as the loop top), so exit
    // granularity and the next_pc/regs15 contract are unchanged; only the
    // routing changes. Forward/self edges keep the light path (unreachable
    // by construction: their labels are already closed at this point).
    uint32_t region_max_ticks() const {
        uint32_t m = 0;
        for (size_t i = 0; i < members->size(); ++i)
            m = std::max(m, (*metadata)[i].ticks);
        return m;
    }
    // Single-region scope (Sol brief): E/G direct edges apply globally when
    // VITA3K_ABLATE_PC is unset, otherwise only to regions containing the
    // named block entry (ablation target chosen by profiling, not formation).
    bool ablate_direct_here() const {
        if (!(ablate_flags() & kAblateDirect))
            return false;
        const uint32_t want = ablate_entry_env();
        if (want == 0)
            return true;
        for (size_t i = 0; i < members->size(); ++i)
            if ((*metadata)[i].entry_pc == want)
                return true;
        return false;
    }
    void light_redispatch(uint32_t target_index) {
        const uint32_t ablate = ablate_flags();
        // Direct-threaded back-edge (E/G): reachable iff target < body_index.
        if (ablate_direct_here() && target_index < body_index) {
            // Budget at the joined edge. Exact successor cost normally (1:1
            // with the skipped leaf); MAXT slice bound under C/G so a call
            // still never reports more ticks than its budget (M16
            // RegionOverrun invariant holds despite bypassing loop top).
            const uint32_t ticks = (ablate & kAblateBudget) ? region_max_ticks()
                                                            : entry_ticks((*metadata)[target_index]);
            state.read_executed(code); imm(ticks); op(Add);
            get(1); op(GtU);
            begin_if();
            set_next_pc_runtime();
            ret(ExitReason::Budget);
            end_if();
            // Loop-top polls, inlined: stop always; smc unless B/G-hoist.
            // regs[15] already holds the pending target PC (location()
            // precedes every light edge), so next_pc publication is exact.
            load(offsetof(JitState, stop_flag));
            begin_if();
            set_next_pc_runtime();
            ret(ExitReason::Stop);
            end_if();
            if (!(ablate & (kAblateSmc | kAblateHoistSmc))) {
                load(offsetof(JitState, smc_dirty));
                begin_if();
                set_next_pc_runtime();
                ret(ExitReason::Smc);
                end_if();
            }
            // Direct: br to $b(target); its End is immediately followed by
            // BODY(target). Depth from BODY(body_index): enclosing labels
            // are $b(body_index-1)..$b0, so $b(target) sits
            // (body_index-1-target) labels out, plus open ifs.
            op(Br); uleb(code, (body_index - 1 - target_index) + extra_labels);
            return;
        }
        const uint32_t ticks = entry_ticks((*metadata)[target_index]);
        // ABLATE C: per-edge budget checks are skipped; a single slice bound
        // at the dispatch loop top keeps runs terminating (documented).
        if (!(ablate_flags() & kAblateBudget)) {
        // Guard-style budget check (not if/else): the chained branch below
        // must stay OUTSIDE the if so br_redispatch's label depth is
        // unchanged. The skipped search leaf would have exited Budget with
        // next_pc = regs[15]; the pending target's PC is already there
        // (location(target) precedes every light edge).
        state.read_executed(code); imm(ticks); op(Add);
        get(1); op(GtU);
        begin_if();
        set_next_pc_runtime();
        ret(ExitReason::Budget);
        end_if();
        }
        // Chained: successor's constant index, then redispatch.
        imm(target_index); set(6);
        br_redispatch();
    }

    bool valid_store_continuations() const {
        const auto &continuations = (*metadata)[body_index].store_continuations;
        if (!continuations.empty() && block.GetCondition() != Cond::AL)
            return false;
        uint32_t previous_offset = 0, previous_ticks = 0, previous_pc = start.PC();
        for (const auto &point : continuations) {
            const Location next{Dynarmic::IR::LocationDescriptor{point.next_location}};
            if (point.ir_offset <= previous_offset || point.ir_offset > block.size()
                || point.completed_ticks <= previous_ticks || point.completed_ticks >= block.CycleCount()
                || !valid_location(next) || next.TFlag() != start.TFlag()
                || next.PC() <= previous_pc || next.PC() >= finish.PC())
                return false;
            previous_offset = point.ir_offset;
            previous_ticks = point.completed_ticks;
            previous_pc = next.PC();
        }
        return true;
    }

    void store_continuation(const StoreContinuation &point, uint32_t next_segment_end) {
        // Normal stores fall through with no PC/CPSR writes, accounting
        // updates or dispatcher transfer. Poll after the whole instruction,
        // including writeback, and before executing any following guest IR.
        // ABLATE B: smc_dirty poll skipped (stop kept). ABLATE C: budget term
        // skipped (loop-top slice bound kept instead); the trailing Budget
        // return is then omitted so a stop/smc exit still works and a
        // budget-only trip falls through to the slice bound.
        const uint32_t ablate = ablate_flags();
        load(offsetof(JitState, stop_flag));
        if (!(ablate & kAblateSmc)) {
            load(offsetof(JitState, smc_dirty)); op(Or);
        }
        if (!(ablate & kAblateBudget)) {
            state.read_executed(code); imm(next_segment_end); op(Add); get(1); op(GtU); op(Or);
        }
        begin_if();
        const Location next{Dynarmic::IR::LocationDescriptor{point.next_location}};
        location(next);
        state.set_next_pc(code, [&] { imm(next.PC()); });
        add_ticks(point.completed_ticks);
        load(offsetof(JitState, stop_flag)); begin_if();
        ret(ExitReason::Stop);
        end_if();
        if (!(ablate & kAblateSmc)) {
            load(offsetof(JitState, smc_dirty)); begin_if();
            ret(ExitReason::Smc);
            end_if();
        }
        if (!(ablate & kAblateBudget)) {
            ret(ExitReason::Budget);
        }
        end_if();
        // This is compile-time bookkeeping only. The terminal accounts the
        // full block once; faults account only earlier completed segments.
        completed_store_ticks = point.completed_ticks;
    }

    void terminal_region(const Term::Terminal &term, unsigned depth) {
        if (depth > 16 || ++terminal_nodes > 256) {
            reject("generic 517");
            return;
        }
        const uint32_t ticks = static_cast<uint32_t>(block.CycleCount());
        if (const auto *link = boost::get<Term::LinkBlock>(&term)) {
            const Location target(link->next);
            // Publish PC up front (edge exits read regs[15]); fold the
            // mode RMW when the member target provably matches end bits.
            const uint32_t idx = member_index(target);
            if (idx != kNoMember) {
                location_member(target);
            } else {
                location(target);
            }
            add_ticks(ticks);
            if (idx != kNoMember) {
                light_redispatch(idx); // chained: constant index, no PC search
            } else {
                state.set_next_pc(code, [&] { imm(target.PC()); });
                ret(ExitReason::Miss);
            }
        } else if (const auto *fast = boost::get<Term::LinkBlockFast>(&term)) {
            const Location target(fast->next);
            const uint32_t idx = member_index(target);
            if (idx != kNoMember) {
                location_member(target);
            } else {
                location(target);
            }
            add_ticks(ticks);
            if (idx != kNoMember) {
                light_redispatch(idx);
            } else {
                state.set_next_pc(code, [&] { imm(target.PC()); });
                ret(ExitReason::Miss);
            }
        } else if (const auto *test = boost::get<Term::If>(&term)) {
            condition(test->if_);
            begin_if();
            terminal_region(test->then_, depth + 1);
            op(Else);
            terminal_region(test->else_, depth + 1);
            end_if(); op(0x00);
        } else if (const auto *test = boost::get<Term::CheckBit>(&term)) {
            if (!check_bit_written) { reject("region check_bit unwritten"); return; }
            get(check_bit_local); begin_if();
            terminal_region(test->then_, depth + 1);
            op(Else);
            terminal_region(test->else_, depth + 1);
            end_if(); op(0x00);
        } else if (boost::get<Term::ReturnToDispatch>(&term) || boost::get<Term::PopRSBHint>(&term)
            || boost::get<Term::FastDispatchHint>(&term)) {
            if (!pc_written)
                reject("region terminal a");
            add_ticks(ticks);
            set_next_pc_runtime();
            // Non-SVC re-dispatch terminals return to the host with next_pc;
            // Continue(0) is a single-block-only concept (REGION_ABI.md).
            ret(svc ? ExitReason::Svc : ExitReason::Miss);
        } else if (const auto *halt = boost::get<Term::CheckHalt>(&term)) {
            if (!boost::get<Term::ReturnToDispatch>(&halt->else_)
                && !boost::get<Term::PopRSBHint>(&halt->else_)
                && !boost::get<Term::FastDispatchHint>(&halt->else_)) {
                reject("region terminal b");
                return;
            }
            terminal_region(halt->else_, depth + 1);
        } else {
            reject("region terminal c"); // Invalid, Interpret never silently skipped
        }
        if (svc && (boost::get<Term::LinkBlock>(&term) || boost::get<Term::LinkBlockFast>(&term) || boost::get<Term::If>(&term) || boost::get<Term::CheckBit>(&term)))
            reject("region terminal d");
    }

    // Emits ONE member block's body (conditional entry + instructions +
    // region terminal). The caller splices it after the matching `end` in
    // the region's nested-block chain.
public:
    Bytes region_body() {
        if (block.size() > 4096 || block.CycleCount() == 0 || block.CycleCount() > 4096
            || block.ConditionFailedCycleCount() > 4096 || !valid_location(start) || !valid_location(finish))
            return {};
        if (!valid_store_continuations())
            return {};
        if (block.GetCondition() != Cond::AL) {
            if (!block.HasConditionFailedLocation() || block.ConditionFailedCycleCount() == 0)
                return {};
            condition(block.GetCondition());
            op(Eqz);
            begin_if();
            // Condition failed: account its ticks and re-dispatch (the fail
            // target may be another member of this same region). A member
            // fail target takes the light path; a non-member keeps the full
            // PC write (the generic search needs the architectural regs[15]).
            add_ticks(static_cast<uint32_t>(block.ConditionFailedCycleCount()));
            const Location fail_loc(block.ConditionFailedLocation());
            // The architectural PC ALWAYS moves to the failed instruction
            // (fault/exit contract); the light path only skips the search.
            location(fail_loc);
            if (const uint32_t fail_idx = member_index(fail_loc); fail_idx != kNoMember)
                light_redispatch(fail_idx);
            else
                generic_redispatch();
            op(Else);
            emit_instructions_and_terminal();
            end_if();
        } else {
            emit_instructions_and_terminal();
        }
        if (!ok)
            return {};
        return std::move(code);
    }

    void emit_instructions_and_terminal() {
        // Region registers are initialized once by run's prologue and stay
        // live through every linked or condition-failed block boundary.
        for (const Inst &inst : block)
            has_bx |= inst.GetOpcode() == Op::A32BXWritePC;
        const auto &continuations = (*metadata)[body_index].store_continuations;
        size_t continuation_index = 0;
        uint32_t ir_offset = 0;
        for (const Inst &inst : block) {
            if (svc || !instruction(inst)) {
                ok = false; // Never publish a partially emitted body.
                if (rejection.empty())
                    rejection = std::string("instruction ") + Dynarmic::IR::GetNameOf(inst.GetOpcode());
                return;
            }
            locals.emplace(&inst, next_local);
            next_local += 10;
            ++ir_offset;
            if (continuation_index < continuations.size()
                && continuations[continuation_index].ir_offset == ir_offset) {
                const uint32_t next_end = continuation_index + 1 < continuations.size()
                    ? continuations[continuation_index + 1].completed_ticks
                    : static_cast<uint32_t>(block.CycleCount());
                if (svc) { reject("store continuation after SVC"); return; }
                store_continuation(continuations[continuation_index++], next_end);
            }
        }
        if (continuation_index != continuations.size()) {
            reject("store continuation offset not emitted");
            return;
        }
        terminal_region(block.GetTerminal(), 0);
    }

    uint32_t ssa_words() const { return next_local - ssa_base; }

private:
    bool instruction(const Inst &inst) {
        const Op kind = inst.GetOpcode();
        // One consumer pre-scan per block (short-circuits after the first
        // instruction); producers below consult it for dead flag words.
        scan_flag_consumers();
        if (arithmetic(kind)) { add_sub(inst); return ok; }
        if (shift(kind)) { shifted(inst); return ok; }
        const auto arg = [&](size_t n) { value(inst.GetArg(n)); };
        switch (kind) {
        case Op::Void: return true; // Dynarmic's invalidated/dead instruction marker
        case Op::Identity: arg(0); break;
        case Op::A32GetRegister:
        case Op::A32SetRegister: {
            const auto reg = inst.GetArg(0);
            if (!reg.IsImmediate() || reg.GetType() != Type::A32Reg)
                return false;
            const auto index = static_cast<uint32_t>(reg.GetA32RegRef());
            if (index >= 16) return false;
            const uint32_t offset = offsetof(JitState, regs) + index * sizeof(uint32_t);
            if (region && index < cached_regs.size()) {
                if (kind == Op::A32GetRegister) {
                    get(reg_base + index);
                    break;
                }
                arg(1); set(reg_base + index);
                if (!region)
                    dirty_regs[index] = true;
            } else if (kind == Op::A32GetRegister) {
                if (index == 15) state.read_pc(code);
                else load(offset);
                break;
            } else if (index == 15) {
                state.write_pc(code, [&] { arg(1); });
            } else {
                get(0); arg(1); store(offset);
            }
            pc_written |= index == 15;
            return ok;
        }
        case Op::A32GetCpsr: state.read_full_cpsr(code); break;
        case Op::A32ReadMemory8: memory_call(inst, false, 1); return ok;
        case Op::A32ReadMemory16: memory_call(inst, false, 2); return ok;
        case Op::A32ReadMemory32: memory_call(inst, false, 4); return ok;
        case Op::A32ReadMemory64: memory_call(inst, false, 8); return ok;
        case Op::A32WriteMemory8: memory_call(inst, true, 1); return ok;
        case Op::A32WriteMemory16: memory_call(inst, true, 2); return ok;
        case Op::A32WriteMemory32: memory_call(inst, true, 4); return ok;
        case Op::A32WriteMemory64: memory_call(inst, true, 8); return ok;
        case Op::A32GetCFlag: flag(29); break;
        case Op::A32SetCpsrNZ:
        case Op::A32SetCpsrNZC:
        case Op::A32SetCpsrNZCV:
        case Op::A32SetCpsrNZCVRaw: {
            const uint32_t bits = kind == Op::A32SetCpsrNZ ? 0xc0000000
                : kind == Op::A32SetCpsrNZC ? 0xe0000000 : 0xf0000000;
            state.write_nzcv(code, bits, [&] { arg(0); }, [&] { arg(1); });
            return ok;
        }
        case Op::GetNZFromOp:
        case Op::GetNZCVFromOp:
        case Op::GetCarryFromOp:
        case Op::GetOverflowFromOp:
        case Op::GetGEFromOp: return pseudo(inst);
        case Op::NZCVFromPackedFlags: arg(0); mask(0xf0000000); break;
        case Op::GetCFlagFromNZCV: arg(0); imm(29); op(ShrU); mask(1); break;
        case Op::And32: arg(0); arg(1); op(And); break;
        case Op::Eor32: arg(0); arg(1); op(Xor); break;
        case Op::Or32: arg(0); arg(1); op(Or); break;
        case Op::Not32: arg(0); imm(0xffffffff); op(Xor); break;
        case Op::AndNot32: arg(0); arg(1); imm(0xffffffff); op(Xor); op(And); break;
        case Op::IsZero32: arg(0); op(Eqz); break;
        case Op::MostSignificantBit: arg(0); imm(31); op(ShrU); break;
        case Op::LeastSignificantByte: arg(0); mask(0xff); break;
        case Op::LeastSignificantHalf: arg(0); mask(0xffff); break;
        case Op::LeastSignificantWord: arg(0); break;
        case Op::MostSignificantWord:
            // SHR by 32 returns word 1; its carry is original bit 31.
            value_word(inst.GetArg(0), 1); set(next_local);
            if (carry_needed.count(&inst) != 0) {
                value_word(inst.GetArg(0), 0); imm(31); op(ShrU); set(next_local + 4);
            }
            return ok;
        case Op::LogicalShiftRight64:
            // U64 result: publish both words via the i64 scratch local,
            // exactly like Pack2x32To1x64, so word-1 consumers never see a
            // stale/unset slot.
            value64(inst.GetArg(0)); value(inst.GetArg(1)); op(ExtendU); op(ShrU64);
            // Wasm masks the shift count modulo 64; Dynarmic requires zero
            // for every U8 count >= 64.
            op(0x42); op(0); // i64.const 0
            value(inst.GetArg(1)); imm(64); op(LtU); op(Select);
            set(scratch_local);
            get(scratch_local); op(Wrap); set(next_local);
            get(scratch_local); op(0x42); uleb(code, 32); op(ShrU64); op(Wrap); set(next_local + 1);
            return ok;
        case Op::SignExtendByteToWord: arg(0); imm(24); op(Shl); imm(24); op(ShrS); break;
        case Op::SignExtendHalfToWord: arg(0); imm(16); op(Shl); imm(16); op(ShrS); break;
        case Op::ZeroExtendWordToLong:
            value_word(inst.GetArg(0)); set(next_local);
            imm(0); set(next_local + 1); // Region bodies reuse these locals.
            return ok;
        case Op::ZeroExtendByteToWord: arg(0); mask(0xff); break;
        case Op::ZeroExtendHalfToWord: arg(0); mask(0xffff); break;
        case Op::ConditionalSelect32:
        case Op::ConditionalSelectNZCV:
            if (!inst.GetArg(0).IsImmediate() || inst.GetArg(0).GetType() != Type::Cond) return false;
            arg(1); arg(2); condition(inst.GetArg(0).GetCond()); op(Select); break;
        case Op::A32UpdateUpperLocationDescriptor:
            // Dynarmic suppresses this update throughout a block containing
            // BXWritePC: that op supplies EndLocation with a dynamic T bit.
            if (!has_bx) upper_location(finish);
            return ok;
        case Op::A32BXWritePC:
            upper_location(finish);
            state.write_psr_field(code, 0x20, [&] { arg(0); mask(1); imm(5); op(Shl); });
            state.write_pc(code, [&] {
                arg(0); imm(0xfffffffe); imm(0xfffffffc); arg(0); mask(1); op(Select); op(And);
            });
            pc_written = true;
            return ok;
        case Op::PushRSB:
            // Pure prediction hint. Like PopRSBHint, the no-RSB backend always
            // goes through the parent dispatcher instead of speculating.
            return inst.GetArg(0).IsImmediate() && inst.GetArg(0).GetType() == Type::U64;
        case Op::A32SetCheckBit:
            value_word(inst.GetArg(0)); set(check_bit_local); check_bit_written = true; return ok;
        case Op::VectorBroadcast32:
            // Lanes live in words 0..3 of the slot: value_word reads lane i
            // from word i, and a 4-word stride would collide with the
            // carry/overflow words and the next instruction's slot.
            for (unsigned i = 0; i < 4; ++i) { arg(0); set(next_local + i); }
            return ok;
        case Op::A32GetVector: {
            const auto reg = inst.GetArg(0);
            if (reg.GetType() != Type::A32ExtReg) return false;
            const auto ext = reg.GetA32ExtRegRef();
            unsigned base, words;
            if (Dynarmic::A32::IsQuadExtReg(ext)) {
                if (Dynarmic::A32::RegNumber(ext) >= 16) return false;
                base = Dynarmic::A32::RegNumber(ext) * 4; words = 4;
            } else if (Dynarmic::A32::IsDoubleExtReg(ext)) {
                if (Dynarmic::A32::RegNumber(ext) >= 32) return false;
                base = Dynarmic::A32::RegNumber(ext) * 2; words = 2;
            } else {
                return false; // single registers use GetExtendedRegister32
            }
            for (unsigned i = 0; i < words; ++i) { load(offsetof(JitState, fpu) + (base + i) * sizeof(uint32_t)); set(next_local + i); }
            for (unsigned i = words; i < 4; ++i) { imm(0); set(next_local + i); } // undefined upper lanes
            return ok; // U128 result: all four words were just defined
        }
        case Op::A32SetVector: {
            const auto reg = inst.GetArg(0);
            if (reg.GetType() != Type::A32ExtReg) return false;
            const auto ext = reg.GetA32ExtRegRef();
            // The IR value is always U128 (opcodes.inc); the register kind
            // selects the architectural footprint. Qn covers fpu words
            // [4n,4n+3]; a D access must not touch its neighbour's words.
            unsigned base, words;
            if (Dynarmic::A32::IsQuadExtReg(ext)) {
                if (Dynarmic::A32::RegNumber(ext) >= 16) return false;
                base = Dynarmic::A32::RegNumber(ext) * 4; words = 4;
            } else if (Dynarmic::A32::IsDoubleExtReg(ext)) {
                if (Dynarmic::A32::RegNumber(ext) >= 32) return false;
                base = Dynarmic::A32::RegNumber(ext) * 2; words = 2;
            } else {
                return false; // single registers use SetExtendedRegister32
            }
            for (unsigned i = 0; i < words; ++i) { get(0); value_word(inst.GetArg(1), i); store(offsetof(JitState, fpu) + (base + i) * sizeof(uint32_t)); }
            return ok;
        }
        case Op::Pack2x32To1x64:
            // U64 result: assemble in the i64 scratch local, then publish as
            // two words. Leaving the i64 on the stack would type-error on the
            // i32 locals used by every SSA slot.
            value_word(inst.GetArg(0)); op(ExtendU);
            value_word(inst.GetArg(1)); op(ExtendU); op(0x42); uleb(code, 32); op(Shl64); op(Or64);
            set(scratch_local);
            get(scratch_local); op(Wrap); set(next_local);
            get(scratch_local); op(0x42); uleb(code, 32); op(ShrU64); op(Wrap); set(next_local + 1);
            return ok;
        case Op::A32GetExtendedRegister32: {
            const auto reg = inst.GetArg(0);
            if (reg.GetType() != Type::A32ExtReg) return false;
            if (!Dynarmic::A32::IsSingleExtReg(reg.GetA32ExtRegRef())) return false;
            const auto n = Dynarmic::A32::RegNumber(reg.GetA32ExtRegRef());
            if (n >= 32) return false;
            // U32 result: one word; Sn is fpu word n.
            load(offsetof(JitState, fpu) + n * sizeof(uint32_t)); set(next_local);
            return ok;
        }
        case Op::A32SetExtendedRegister32: {
            const auto reg = inst.GetArg(0);
            if (reg.GetType() != Type::A32ExtReg) return false;
            if (!Dynarmic::A32::IsSingleExtReg(reg.GetA32ExtRegRef())) return false;
            const auto n = Dynarmic::A32::RegNumber(reg.GetA32ExtRegRef());
            if (n >= 32) return false;
            get(0); value_word(inst.GetArg(1), 0); store(offsetof(JitState, fpu) + n * sizeof(uint32_t));
            return ok;
        }
        case Op::A32GetExtendedRegister64: {
            const auto reg = inst.GetArg(0);
            if (reg.GetType() != Type::A32ExtReg) return false;
            if (!Dynarmic::A32::IsDoubleExtReg(reg.GetA32ExtRegRef())) return false;
            const auto n = Dynarmic::A32::RegNumber(reg.GetA32ExtRegRef());
            if (n >= 32) return false;
            // U64 result: both little-endian words must be defined, or
            // consumers reading word 1 (e.g. vst1 upper elements) see zeros.
            load(offsetof(JitState, fpu) + (n * 2 + 0) * sizeof(uint32_t)); set(next_local);
            load(offsetof(JitState, fpu) + (n * 2 + 1) * sizeof(uint32_t)); set(next_local + 1);
            return ok;
        }
        case Op::A32SetExtendedRegister64: {
            const auto reg = inst.GetArg(0);
            if (!reg.IsImmediate()) return false;
            // RegNumber alone cannot distinguish S/D/Q; only a D register has
            // this 64-bit footprint (the x64 backend asserts the same).
            if (!Dynarmic::A32::IsDoubleExtReg(reg.GetA32ExtRegRef())) return false;
            const auto n = Dynarmic::A32::RegNumber(reg.GetA32ExtRegRef());
            if (n >= 32) return false;
            get(0); value_word(inst.GetArg(1), 0); store(offsetof(JitState, fpu) + n * 2 * sizeof(uint32_t));
            get(0); value_word(inst.GetArg(1), 1); store(offsetof(JitState, fpu) + (n * 2 + 1) * sizeof(uint32_t)); return ok;
        }
        case Op::A32CallSupervisor:
            if (!pc_written) return false;
            get(0); arg(0); store(offsetof(JitState, svc));
            svc = true;
            return ok;
        default: return false;
        }
        if (!scalar(inst.GetType()))
            return false;
        set(next_local);
        return ok;
    }
};
} // namespace

std::vector<uint8_t> emit_block(const Dynarmic::IR::Block &block) {
    return Emitter(block).run();
}

bool validate_region_block(const Dynarmic::IR::Block &block,
    const std::vector<StoreContinuation> &store_continuations) {
    const std::vector<const Dynarmic::IR::Block *> members{&block};
    const Location at(block.Location());
    const std::vector<RegionBlockMeta> metadata{{at.PC(), Location::CPSR_MODE_MASK,
        at.CPSR().Value() & Location::CPSR_MODE_MASK,
        static_cast<uint32_t>(block.CycleCount() + block.ConditionFailedCycleCount()), store_continuations}};
    Emitter emitter(block, 0, members, metadata);
    return !emitter.region_body().empty();
}

std::vector<uint8_t> emit_region(
    const std::vector<const Dynarmic::IR::Block *> &blocks,
    const std::vector<RegionBlockMeta> &meta, RegionStateOptions options) {
    const RegionState state(options);
    constexpr size_t kMaxBlocks = 512;
    constexpr uint64_t kMaxTicks = 32768;
    constexpr size_t kMaxModule = 4 << 20;
    const size_t n = blocks.size();
    if (n == 0 || n != meta.size() || n > kMaxBlocks)
        return {};
    uint64_t total_ticks = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto &b = *blocks[i];
        const Location loc(b.Location());
        if (meta[i].entry_pc != loc.PC() || meta[i].psr_mask != Location::CPSR_MODE_MASK
            || meta[i].psr_value != (loc.CPSR().Value() & Location::CPSR_MODE_MASK)
            || meta[i].ticks != static_cast<uint32_t>(b.CycleCount() + b.ConditionFailedCycleCount()))
            return {};
        if (i && meta[i].entry_pc <= meta[i - 1].entry_pc) // strictly ascending, one per PC
            return {};
        total_ticks += meta[i].ticks;
    }
    if (total_ticks > kMaxTicks)
        return {};

    // The cache belongs to the entire invocation, not an individual body.
    // Initialize even write-only registers: an early exit or a skipped
    // conditional body must preserve the incoming value. The epilogue spills
    // every register any member can write, including writes in prior blocks.
    std::array<bool, kCachedRegCount> used_regs{}, written_regs{};
    for (const auto *block : blocks) {
        for (const Inst &inst : *block) {
            const Op kind = inst.GetOpcode();
            if (kind != Op::A32GetRegister && kind != Op::A32SetRegister)
                continue;
            const auto reg = inst.GetArg(0);
            if (!reg.IsImmediate() || reg.GetType() != Type::A32Reg)
                continue; // The body emitter rejects malformed operands.
            const auto index = static_cast<uint32_t>(reg.GetA32RegRef());
            if (index < used_regs.size()) {
                used_regs[index] = true;
                written_regs[index] |= kind == Op::A32SetRegister;
            }
        }
    }

    // Per-block bodies share the region local layout; SSA words are reused.
    std::vector<Bytes> bodies(n);
    uint32_t max_ssa = 0;
    for (size_t i = 0; i < n; ++i) {
        Emitter emitter(*blocks[i], unsigned(i), blocks, meta, options);
        bodies[i] = emitter.region_body();
        if (bodies[i].empty()) {
            return {};
        }
        max_ssa = std::max(max_ssa, emitter.ssa_words());
    }

    // Dispatch prologue + static PC search tree + br_table (REGION_ABI.md
    // v1.2 light dispatch path). Every iteration runs dispatches++ and the
    // stop/smc polls; then EITHER the successor block index was preloaded by
    // a statically-chained edge (light path: straight to br_table) OR local 6
    // holds kLightDispatchSentinel (fresh entry or non-member edge) and the
    // generic path reloads regs[15] and runs the static PC search. A chained
    // edge whose budget check fails exits BEFORE the next loop-top poll, so
    // the HOST re-checks smc_dirty on every exit (see execute_regions).
    Bytes d;
    const auto exit_with = [&](ExitReason reason, uint32_t open_ifs) {
        state.set_next_pc_from_pc(d);
        b_imm(d, static_cast<uint32_t>(reason)); b_set(d, 6);
        // Skip the open ifs, $default, all n block labels and the loop to
        // reach $exit. All state publication is shared after that label.
        b_op(d, Br); uleb(d, static_cast<uint32_t>(n) + 2 + open_ifs);
    };
    // Keep dispatch accounting in local 7 and publish it only on return.
    b_get(d, 7); b_imm(d, 1); b_op(d, Add); b_set(d, 7);
    // if (state.stop_flag) { next_pc = regs[15]; return Stop }
    b_get(d, 0); b_op(d, Load); uleb(d, 2); uleb(d, offsetof(JitState, stop_flag));
    b_op(d, If); b_op(d, 0x40); exit_with(ExitReason::Stop, 1); b_op(d, End);
    // ABLATE B: smc_dirty loop-top poll skipped (stop poll kept). The host
    // still re-checks smc_dirty on every exit, and the display fixture has
    // zero SMC events, so smc_exits must stay 0 or the run is invalid.
    const uint32_t ablate_dispatch = ablate_flags();
    if (!(ablate_dispatch & (kAblateSmc | kAblateHoistSmc))) {
    // if (state.smc_dirty) { next_pc = regs[15]; return Smc }
    b_get(d, 0); b_op(d, Load); uleb(d, 2); uleb(d, offsetof(JitState, smc_dirty));
    b_op(d, If); b_op(d, 0x40); exit_with(ExitReason::Smc, 1); b_op(d, End);
    }
    // ABLATE C: per-entry budget checks (search leaf + chained edges) are
    // skipped; this single slice bound is what keeps runs terminating.
    // It MUST never let executed_call exceed budget: the M16 dispatcher
    // fails a region call that reports more ticks than its slice
    // (RegionOverrun). So the bound uses the region's maximum single-block
    // cost: if (executed_call + MAXT > budget) { next_pc = regs[15];
    // return Budget }. Placed before the generic-path selector so an
    // over-budget iteration exits before selecting or executing any block.
    // Resume is exact (regs[15] already holds the pending target PC), so
    // only exit granularity changes, never the instruction count.
    if (ablate_dispatch & kAblateBudget) {
        uint32_t max_ticks = 0;
        for (size_t i = 0; i < n; ++i)
            max_ticks = std::max(max_ticks, meta[i].ticks);
        state.read_executed(d); b_imm(d, max_ticks); b_op(d, Add);
        b_get(d, 1); b_op(d, GtU);
        b_op(d, If); b_op(d, 0x40); exit_with(ExitReason::Budget, 1); b_op(d, End);
    }
    // Generic-path selector: fresh entries and non-member edges carry the
    // sentinel in the dispatch-index local; chained edges overwrite it with
    // a constant block index and skip the reload + search entirely.
    b_get(d, 6);
    b_imm(d, kLightDispatchSentinel); b_op(d, Eq);
    b_op(d, If); b_op(d, 0x40);
    // Generic path: pc = regs[15]; idx = n (default)
    state.read_pc(d);
    b_set(d, 3);
    b_imm(d, static_cast<uint32_t>(n)); b_set(d, 6);

    // Balanced static search over the sorted, constant entry PCs.
    std::function<void(size_t, size_t, uint32_t)> tree = [&](size_t lo, size_t hi, uint32_t open_ifs) {
        if (hi - lo == 1) {
            b_get(d, 3); b_imm(d, meta[lo].entry_pc); b_op(d, Eq);
            b_op(d, If); b_op(d, 0x40);
            // ABLATE D: entry PSR/FPSCR validation skipped; the PC search
            // itself is kept (br_table routing unchanged).
            if (!(ablate_dispatch & kAblatePsr)) {
            {
                // Entry PSR mismatch -> Miss (host re-dispatches at regs[15]).
                state.read_dispatch_psr(d);
                b_imm(d, meta[lo].psr_mask); b_op(d, And);
                b_imm(d, meta[lo].psr_value); b_op(d, Ne);
                b_op(d, If); b_op(d, 0x40); exit_with(ExitReason::Miss, open_ifs + 2); b_op(d, End);
                // FPSCR mode bits are part of the block's translation key.
                const uint32_t fpscr = Location(blocks[lo]->Location()).FPSCR().Value()
                    & Location::FPSCR_MODE_MASK;
                b_get(d, 0); b_op(d, Load); uleb(d, 2); uleb(d, offsetof(JitState, fpscr));
                b_imm(d, Location::FPSCR_MODE_MASK); b_op(d, And);
                b_imm(d, fpscr); b_op(d, Ne);
                b_op(d, If); b_op(d, 0x40); exit_with(ExitReason::Miss, open_ifs + 2); b_op(d, End);
            }
            }
            {
                // Budget: executed_call + ticks > budget -> Budget.
                // ABLATE C: skipped (loop-top slice bound kept instead).
                if (!(ablate_dispatch & kAblateBudget)) {
                state.read_executed(d); b_imm(d, entry_ticks(meta[lo])); b_op(d, Add);
                b_get(d, 1); b_op(d, GtU);
                b_op(d, If); b_op(d, 0x40); exit_with(ExitReason::Budget, open_ifs + 2); b_op(d, End);
                }
                b_imm(d, static_cast<uint32_t>(lo)); b_set(d, 6);
            }
            b_op(d, End);
            return;
        }
        const size_t mid = (lo + hi) / 2;
        b_get(d, 3); b_imm(d, meta[mid].entry_pc); b_op(d, LtU);
        b_op(d, If); b_op(d, 0x40);
        tree(lo, mid, open_ifs + 1);
        b_op(d, Else);
        tree(mid, hi, open_ifs + 1);
        b_op(d, End);
    };
    tree(0, n, 1); // the generic-path if is already open
    b_op(d, End); // close generic-path if

    // br_table: vec of n labels then the default. Index i -> $bI (depth n-i
    // at this position); default -> $default (depth 0, innermost).
    b_get(d, 6);
    b_op(d, BrTable);
    uleb(d, static_cast<uint32_t>(n)); // vec count
    for (size_t i = 0; i < n; ++i)
        uleb(d, static_cast<uint32_t>(n - i));
    uleb(d, 0);

    // Function body: nested block chain, bodies in reverse label order.
    Bytes code;
    state.load_region_state(code);
    // Fresh entry selects the generic path: locals are zero-initialized per
    // invocation, so the dispatch-index local must start at the sentinel
    // (a stale 0 would wrongly route the first dispatch to block 0).
    b_imm(code, kLightDispatchSentinel); b_set(code, 6);
    b_imm(code, 0); b_set(code, 7);
    b_load(code, offsetof(JitState, page_table_base)); b_set(code, 8);
    b_load(code, offsetof(JitState, page_perms_base)); b_set(code, 9);
    b_load(code, offsetof(JitState, code_pages_base)); b_set(code, 10);
    for (uint32_t i = 0; i < used_regs.size(); ++i) {
        if (used_regs[i]) {
            b_load(code, offsetof(JitState, regs) + i * sizeof(uint32_t));
            b_set(code, kRegionRegBase + i);
        }
    }
    b_op(code, Block); b_op(code, 0x40); // $exit: publish state once per call
    b_op(code, Loop); b_op(code, 0x40); // $outer: br here = redispatch
    for (size_t i = 0; i < n; ++i) { b_op(code, Block); b_op(code, 0x40); } // $b0..$b(n-1)
    b_op(code, Block); b_op(code, 0x40); // $default (innermost)
    code.insert(code.end(), d.begin(), d.end());
    b_op(code, End); // close $default -> miss body
    {
        state.set_next_pc(code, [&] { b_get(code, 3); });
        b_imm(code, static_cast<uint32_t>(ExitReason::Miss)); b_set(code, 6);
        b_op(code, Br); uleb(code, static_cast<uint32_t>(n) + 1); // $exit ($default is closed)
    }
    for (size_t i = n; i-- > 0;) { // close $b(i) -> body i
        b_op(code, End);
        code.insert(code.end(), bodies[i].begin(), bodies[i].end());
    }
    b_op(code, End); // close loop
    b_op(code, 0x00); // unreachable: every body ends in br/return; the loop never falls through
    b_op(code, End); // close $exit -> shared epilogue
    for (uint32_t i = 0; i < written_regs.size(); ++i) {
        if (written_regs[i]) {
            b_get(code, 0); b_get(code, kRegionRegBase + i);
            b_store(code, offsetof(JitState, regs) + i * sizeof(uint32_t));
        }
    }
    state.materialize_all_on_exit(code);
    b_get(code, 0); b_get(code, 6); b_store(code, offsetof(JitState, exit_reason));
    b_get(code, 6); // function result
    b_op(code, End); // end function body

    Bytes body;
    uleb(body, 3); // three local runs
    uleb(body, 3); body.push_back(0x7f); // locals 2,3,4: executed_call, pc, CheckBit
    uleb(body, 1); body.push_back(0x7e); // local 5: i64 scratch
    uleb(body, state.ssa_base() - 6 + max_ssa); body.push_back(0x7f); // fixed region locals, optional state locals, then SSA
    body.insert(body.end(), code.begin(), code.end());
    Bytes functions{1};
    uleb(functions, static_cast<uint32_t>(body.size()));
    functions.insert(functions.end(), body.begin(), body.end());

    Bytes module{0, 'a', 's', 'm', 1, 0, 0, 0};
    section(module, 1, {2,
        0x60, 2, 0x7f, 0x7f, 1, 0x7f, // type 0: (i32,i32)->i32 for run
        0x60, 3, 0x7f, 0x7f, 0x7f, 1, 0x7f}); // type 1: checked helpers
    section(module, 2, {3,
        3, 'e', 'n', 'v', 6, 'm', 'e', 'm', 'o', 'r', 'y', 2, 0, 1,
        3, 'e', 'n', 'v', 8, 'm', 'e', 'm', '_', 'r', 'e', 'a', 'd', 0, 1,
        3, 'e', 'n', 'v', 9, 'm', 'e', 'm', '_', 'w', 'r', 'i', 't', 'e', 0, 1});
    section(module, 3, {1, 0}); // one function, type 0
    // Function index space counts IMPORTED functions first: mem_read=0,
    // mem_write=1, our run=2. (Same layout as the single-block module.)
    section(module, 7, {1, 3, 'r', 'u', 'n', 0, 2});
    section(module, 10, functions);
    if (module.size() > kMaxModule)
        return {};
    return module;
}

// M16 Wasm-side multi-region dispatcher (see emit_wasm.h for the ABI).
// mrun(state, remaining, map_base, epoch_addr) -> ExitReason. All branch
// depths below are counted from each emission site; resolve() takes the
// $out depth as a parameter because call sites nest differently.
std::vector<uint8_t> emit_dispatch() {
    constexpr size_t kMaxModule = 4 << 20;
    // Locals: params 0=state 1=remaining 2=map_base 3=epoch_addr;
    // 4=left 5=mark 6=slot 7=key_lo 8=key_hi 9=idx 10=nprobe 11=reason
    // 12=exec_before 13=slice 14=pc.
    Bytes c;
    const auto ret_out = [&](uint32_t reason, uint32_t out_depth) {
        b_imm(c, reason); b_set(c, 11);
        b_op(c, Br); uleb(c, out_depth);
    };
    const auto if_void = [&](const auto &body) {
        b_op(c, If); b_op(c, 0x40);
        body();
        b_op(c, End);
    };
    const auto load_state = [&](uint32_t offset) {
        b_get(c, 0); b_op(c, Load); uleb(c, 2); uleb(c, offset);
    };
    // Compute key_hi = fpscr | T | E<<1 | IT<<8 from state.cpsr/fpscr.
    // Matches Dynarmic A32 LocationDescriptor::UniqueHash (T=bit5, E=bit9,
    // ITSTATE=(bit26:25,bit15:10)); single_stepping is always false here.
    const auto compute_key_hi = [&] {
        b_load(c, offsetof(JitState, fpscr));
        b_load(c, offsetof(JitState, cpsr)); b_imm(c, 5); b_op(c, ShrU);
        b_imm(c, 1); b_op(c, And); b_op(c, Or);
        b_load(c, offsetof(JitState, cpsr)); b_imm(c, 9); b_op(c, ShrU);
        b_imm(c, 1); b_op(c, And); b_imm(c, 1); b_op(c, Shl); b_op(c, Or);
        b_load(c, offsetof(JitState, cpsr)); b_imm(c, 25); b_op(c, ShrU);
        b_imm(c, 3); b_op(c, And);
        b_load(c, offsetof(JitState, cpsr)); b_imm(c, 8); b_op(c, ShrU);
        b_imm(c, 0xfc); b_op(c, And); b_op(c, Or);
        b_imm(c, 8); b_op(c, Shl); b_op(c, Or);
        b_set(c, 8);
    };
    // Probe the map for local 14's PC; on hit leaves slot in local 6 and
    // falls out of the $r block, else publishes next_pc and br $out.
    // out_depth = $out depth measured WITHOUT the if_void level that always
    // wraps the miss() call sites; +1 is applied internally.
    const auto emit_resolve = [&](uint32_t out_depth) {
        const uint32_t br_out = out_depth + 1;
        b_get(c, 14); b_set(c, 7);
        compute_key_hi();
        b_get(c, 7); b_get(c, 8); b_imm(c, kDispatchHashK); b_op(c, Mul);
        b_op(c, Xor); b_imm(c, kDispatchMapMask); b_op(c, And); b_set(c, 9);
        b_imm(c, 0); b_set(c, 10);
        b_op(c, Block); b_op(c, 0x40); // $r
        b_op(c, Loop); b_op(c, 0x40);  // $p
        const auto miss = [&] {
            b_get(c, 0); b_get(c, 14); b_store(c, offsetof(JitState, next_pc));
            ret_out(static_cast<uint32_t>(ExitReason::Miss), br_out);
        };
        // if (nprobe >= MAX): miss.-GeU unavailable; use LtU+Eqz.
        b_get(c, 10); b_imm(c, kDispatchMaxProbe); b_op(c, LtU); b_op(c, Eqz);
        if_void(miss);
        // epoch = *(map_base + idx*16 + 12); if 0: miss (never written).
        b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); b_op(c, Add);
        b_imm(c, 12); b_op(c, Add);
        b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_op(c, Eqz);
        if_void(miss);
        // match = (epoch == *epoch_addr) & (lo==key_lo) & (hi==key_hi).
        b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); b_op(c, Add);
        b_imm(c, 12); b_op(c, Add);
        b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_get(c, 3); b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_op(c, Eq);
        b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); b_op(c, Add);
        b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_get(c, 7); b_op(c, Eq); b_op(c, And);
        b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); b_op(c, Add);
        b_imm(c, 4); b_op(c, Add);
        b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_get(c, 8); b_op(c, Eq); b_op(c, And);
        if_void([&] {
            b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); b_op(c, Add);
            b_imm(c, 8); b_op(c, Add);
            b_op(c, Load); uleb(c, 2); uleb(c, 0);
            b_set(c, 6);
            // Inside if_void at probe level: if=0,$p=1,$r=2 -> br 2 breaks $r.
            // miss() below always runs inside if_void too, so its $out br
            // needs out_depth+1; callers pass the UNADJUSTED $out depth and
            // this function adds one. All depths rechecked against nesting.
            b_op(c, Br); uleb(c, 2); // break $r: slot valid
        });
        b_get(c, 9); b_imm(c, 1); b_op(c, Add);
        b_imm(c, kDispatchMapMask); b_op(c, And); b_set(c, 9);
        b_get(c, 10); b_imm(c, 1); b_op(c, Add); b_set(c, 10);
        b_op(c, Br); uleb(c, 0); // next probe
        b_op(c, End); // $p
        b_op(c, End); // $r: fallthrough = slot valid
    };
    const auto tx_bump = [&] {
        // Stack for Store must be [addr, value]: duplicate addr first.
        b_get(c, 0); b_get(c, 0); b_op(c, Load); uleb(c, 2); uleb(c, offsetof(JitState, tx_wasm));
        b_imm(c, 1); b_op(c, Add);
        b_store(c, offsetof(JitState, tx_wasm));
    };
    Bytes body{1};
    uleb(body, 11); body.push_back(0x7f); // one run of 11 i32 locals (4..14)
    // left = remaining; mark = executed.
    b_get(c, 1); b_set(c, 4);
    load_state(offsetof(JitState, executed)); b_set(c, 5);
    b_op(c, Block); b_op(c, 0x40); // $out (depth 0 here)
    // left == 0 at entry: Budget, exactly like the host loop top failing.
    b_get(c, 4); b_op(c, Eqz);
    if_void([&] { ret_out(static_cast<uint32_t>(ExitReason::Budget), 1); });
    // Entry resolve from architectural regs[15].
    load_state(offsetof(JitState, regs) + 15 * sizeof(uint32_t)); b_set(c, 14);
    emit_resolve(2);
    b_op(c, Loop); b_op(c, 0x40); // $pump
    // NOTE: ret_out sites below run INSIDE if_void, so $out is one level
    // deeper than at the pump body (if=0,$pump=1,$out=2): they br 2, not 1.
    // stop_flag -> Stop.
    load_state(offsetof(JitState, stop_flag));
    if_void([&] { ret_out(static_cast<uint32_t>(ExitReason::Stop), 2); });
    // left == 0 -> Budget (host fails on full consumption, as today).
    b_get(c, 4); b_op(c, Eqz);
    if_void([&] { ret_out(static_cast<uint32_t>(ExitReason::Budget), 2); });
    // slice = min(left, kDispatchSliceTicks).
    b_get(c, 4); b_imm(c, kDispatchSliceTicks); b_op(c, GtU);
    b_op(c, If); b_op(c, 0x7f);
    b_imm(c, kDispatchSliceTicks);
    b_op(c, Else);
    b_get(c, 4);
    b_op(c, End);
    b_set(c, 13);
    // reason = table[slot](state, slice).
    load_state(offsetof(JitState, executed)); b_set(c, 12);
    b_get(c, 0); b_get(c, 13); b_get(c, 6);
    b_op(c, CallIndirect); uleb(c, 0); uleb(c, 0);
    b_set(c, 11);
    // Overrun guard: delta > slice must fail like the host check.
    load_state(offsetof(JitState, executed)); b_get(c, 12); b_op(c, Sub);
    b_get(c, 13); b_op(c, GtU);
    if_void([&] { ret_out(static_cast<uint32_t>(DispatchReason::RegionOverrun), 2); });
    b_get(c, 4); load_state(offsetof(JitState, executed)); b_get(c, 12);
    b_op(c, Sub); b_op(c, Sub); b_set(c, 4); // left -= delta
    // smc_dirty with a chainable reason must exit via the host normalizer;
    // Svc/Fault/Stop pass through untouched (host normalizes first, as today).
    b_get(c, 11); b_imm(c, static_cast<uint32_t>(ExitReason::Miss)); b_op(c, Eq);
    b_get(c, 11); b_imm(c, static_cast<uint32_t>(ExitReason::Budget)); b_op(c, Eq);
    b_op(c, Or);
    if_void([&] {
        load_state(offsetof(JitState, smc_dirty));
        // Inner if adds a level: if=0,outer=1,$pump=2,$out=3.
        if_void([&] { ret_out(static_cast<uint32_t>(ExitReason::Smc), 3); });
    });
    // Host-direct reasons pass through.
    b_get(c, 11); b_imm(c, static_cast<uint32_t>(ExitReason::Svc)); b_op(c, Eq);
    b_get(c, 11); b_imm(c, static_cast<uint32_t>(ExitReason::Fault)); b_op(c, Eq);
    b_op(c, Or);
    b_get(c, 11); b_imm(c, static_cast<uint32_t>(ExitReason::Stop)); b_op(c, Eq);
    b_op(c, Or);
    b_get(c, 11); b_imm(c, static_cast<uint32_t>(ExitReason::Smc)); b_op(c, Eq);
    b_op(c, Or);
    if_void([&] { b_op(c, Br); uleb(c, 2); }); // br $out, reason intact
    // Budget: host owns exhaustion/no-progress semantics; only continue here.
    b_get(c, 11); b_imm(c, static_cast<uint32_t>(ExitReason::Budget)); b_op(c, Eq);
    if_void([&] {
        // left == 0 or no progress: let the host arm decide (fail or clean slice).
        b_get(c, 4); b_op(c, Eqz);
        if_void([&] { b_op(c, Br); uleb(c, 3); }); // br $out
        load_state(offsetof(JitState, executed)); b_get(c, 5); b_op(c, Eq);
        if_void([&] { b_op(c, Br); uleb(c, 3); }); // br $out
        load_state(offsetof(JitState, executed)); b_set(c, 5);
        load_state(offsetof(JitState, regs) + 15 * sizeof(uint32_t)); b_set(c, 14);
        emit_resolve(4);
        tx_bump();
        b_op(c, Br); uleb(c, 1); // br $pump
    });
    // Miss (or fallthrough unknown reason, which the host rejects): resolve
    // next_pc; unmapped targets return Miss with next_pc published.
    b_get(c, 11); b_imm(c, static_cast<uint32_t>(ExitReason::Miss)); b_op(c, Eq);
    if_void([&] {
        load_state(offsetof(JitState, next_pc)); b_set(c, 14);
        emit_resolve(4);
        tx_bump();
        b_op(c, Br); uleb(c, 1); // br $pump
    });
    b_op(c, Br); uleb(c, 1); // unknown reason -> host default arm fails
    b_op(c, End); // $pump
    b_op(c, End); // $out
    b_get(c, 11); // function result
    b_op(c, End); // end function body
    Bytes funcs{1};
    uleb(funcs, static_cast<uint32_t>(body.size() + c.size()));
    funcs.insert(funcs.end(), body.begin(), body.end());
    funcs.insert(funcs.end(), c.begin(), c.end());
    Bytes module{0, 'a', 's', 'm', 1, 0, 0, 0};
    section(module, 1, {2,
        0x60, 2, 0x7f, 0x7f, 1, 0x7f, // type 0: region run(state,budget)->reason
        0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f}); // type 1: dispatch
    section(module, 2, {2,
        3, 'e', 'n', 'v', 6, 'm', 'e', 'm', 'o', 'r', 'y', 2, 0, 1,
        3, 'e', 'n', 'v', 12, 'r', 'e', 'g', 'i', 'o', 'n', '_', 't', 'a', 'b', 'l', 'e',
        1, 0x70, 0, 1});
    section(module, 3, {1, 1}); // one function, type 1
    section(module, 7, {1, 8, 'd', 'i', 's', 'p', 'a', 't', 'c', 'h', 0, 0});
    section(module, 10, funcs);
    if (module.size() > kMaxModule)
        return {};
    return module;
}
} // namespace vita3k::wasmjit
