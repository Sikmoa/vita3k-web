// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include "emit_wasm.h"
#include "fp64.h"
#include "dynarmic/common/crypto/aes.h"
#include "dynarmic/common/crypto/sm4.h"

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
        | (enabled('VITA3K_WASMJIT_PROMOTE_ACCOUNTING', both) ? 2 : 0)
        | (enabled('VITA3K_WASMJIT_COUNT_FAST_MEMORY', false) ? 4 : 0);
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

// MVP operations plus the opt-in Memory64 address/import types. No Table64
// region table, sign-extension proposal, multivalue or additional memory.
enum Wasm : uint8_t {
    Unreachable = 0x00,
    Block = 0x02, Loop = 0x03, If = 0x04, Else = 0x05, End = 0x0b,
    Br = 0x0c, BrIf = 0x0d, BrTable = 0x0e, Return = 0x0f, Call = 0x10,
    CallIndirect = 0x11, ReturnCall = 0x12, ReturnCallIndirect = 0x13, Select = 0x1b,
    Get = 0x20, Set = 0x21, Tee = 0x22, GlobalGet = 0x23, Load = 0x28, Load64 = 0x29, Load8U = 0x2d, Load16U = 0x2f,
    Store = 0x36, Store8 = 0x3a, Store16 = 0x3b, Const = 0x41,
    Eqz = 0x45, Eq = 0x46, Ne = 0x47, LtS = 0x48, LtU = 0x49, GtS = 0x4a, GtU = 0x4b, LeS = 0x4c, LeU = 0x4d, GeS = 0x4e, GeU = 0x4f, Eqz64 = 0x50,
    Clz = 0x67, Add = 0x6a, Sub = 0x6b, Mul = 0x6c, And = 0x71, Or = 0x72, Xor = 0x73,
    Shl = 0x74, ShrS = 0x75, ShrU = 0x76, RotR = 0x78, RotL = 0x77,
    DivS = 0x6d, DivU = 0x6e,
    Eq64 = 0x51, Ne64 = 0x52, LtS64 = 0x53, LtU64 = 0x54, GtS64 = 0x55, GtU64 = 0x56,
    LeS64 = 0x57, LeU64 = 0x58, GeS64 = 0x59, GeU64 = 0x5a,
    Add64 = 0x7c, Sub64 = 0x7d, Mul64 = 0x7e, DivS64 = 0x7f, DivU64 = 0x80,
    And64 = 0x83, Or64 = 0x84, Xor64 = 0x85, Shl64 = 0x86, ShrS64 = 0x87, ShrU64 = 0x88,
    RotL64 = 0x89, RotR64 = 0x8a, Wrap = 0xa7, ExtendS = 0xac, ExtendU = 0xad,
};

constexpr bool memory64 = memory_address_type == MemoryAddressType::I64;
constexpr uint8_t host_type = memory64 ? 0x7e : 0x7f;
constexpr uint8_t host_eqz = memory64 ? Eqz64 : Eqz;

void uleb(Bytes &out, uint32_t n) {
    do {
        const uint8_t byte = n & 0x7f;
        n >>= 7;
        out.push_back(byte | (n ? 0x80 : 0));
    } while (n);
}

void uleb64(Bytes &out, uint64_t n) {
    do {
        const uint8_t byte = n & 0x7f;
        n >>= 7;
        out.push_back(byte | (n ? 0x80 : 0));
    } while (n);
}

void constant64(Bytes &out, int64_t n) {
    out.push_back(0x42); // i64.const, signed LEB128
    for (;;) {
        const uint8_t byte = static_cast<uint8_t>(n) & 0x7f;
        n = (n - byte) / 128;
        const bool done = (n == 0 && !(byte & 0x40)) || (n == -1 && (byte & 0x40));
        out.push_back(byte | (done ? 0 : 0x80));
        if (done) break;
    }
}

void memory_type(Bytes &out) {
    if (memory64) {
        // Maximum present, i64 address type, shared in the threaded build
        // (THREADS.md: a module importing a shared memory must say so). Import
        // the ONE main runtime memory, whose fixed extent is 8 GiB = 131072
        // Wasm pages.
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
        out.push_back(0x07);
#else
        out.push_back(0x05);
#endif
        uleb64(out, vita3k::memory::guest_window_end / vita3k::memory::wasm_page_size);
        uleb64(out, vita3k::memory::guest_window_end / vita3k::memory::wasm_page_size);
    } else {
        out.insert(out.end(), {0, 1}); // original unshared min=1, no maximum
    }
}

Bytes memory_imports(uint32_t count) {
    Bytes out;
    uleb(out, count);
    out.insert(out.end(), {3, 'e', 'n', 'v', 6, 'm', 'e', 'm', 'o', 'r', 'y', 2});
    memory_type(out);
    return out;
}

void memarg(Bytes &out, uint32_t alignment, uint64_t offset) {
    uleb(out, alignment);
    uleb64(out, offset);
}

// Stack: host address, unsigned i32 byte offset -> host address. Guest
// arithmetic is still i32; widen only at this memory-address boundary.
void address_add_i32(Bytes &out) {
    if (memory64) out.push_back(ExtendU);
    out.push_back(memory64 ? Add64 : Add);
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
void b_shared_load32(Bytes &c) {
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
    b_op(c, 0xfe); uleb(c, 0x10); // i32.atomic.load
#else
    b_op(c, Load);
#endif
}
void b_shared_store32(Bytes &c) {
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
    b_op(c, 0xfe); uleb(c, 0x17); // i32.atomic.store
#else
    b_op(c, Store);
#endif
}
void b_imm(Bytes &c, uint32_t n) { constant(c, n); }
void b_get(Bytes &c, uint32_t index) { b_op(c, Get); uleb(c, index); }
void b_set(Bytes &c, uint32_t index) { b_op(c, Set); uleb(c, index); }
void b_load(Bytes &c, uint32_t offset) { b_get(c, 0); b_op(c, Load); memarg(c, 2, offset); }
void b_store(Bytes &c, uint32_t offset) { b_op(c, Store); memarg(c, 2, offset); }
void b_load_host(Bytes &c, uint32_t offset) {
    b_get(c, 0); b_op(c, memory64 ? Load64 : Load); memarg(c, memory64 ? 3 : 2, offset);
}

bool scalar(Type type) {
    return type == Type::U1 || type == Type::U8 || type == Type::U16 || type == Type::U32 || type == Type::U64 || type == Type::NZCVFlags;
}

bool arithmetic(Op op) {
    return op == Op::Add32 || op == Op::Sub32;
}

// Scalar saturations whose GetOverflowFromOp (CPSR.Q) reads word +5.
bool saturation(Op op) {
    return op == Op::SignedSaturation || op == Op::UnsignedSaturation || op == Op::SignedSaturatedAddWithFlag32
        || op == Op::SignedSaturatedSubWithFlag32;
}

// Differential-check canary (VITA3K_AOT_CANARY=1 while building an AOT
// image): every AOT Add32/Sub32 also toggles CPSR.GE[0], an architectural
// difference the JIT-vs-AOT comparison (VITA3K_AOT_DIFF) must report.
bool aot_canary() {
    static const bool enabled = [] {
        const char *value = std::getenv("VITA3K_AOT_CANARY");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
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
// Function index space counts IMPORTED functions first: mem_read=0,
// mem_write=1, fp64=2, run=3, and the outlined fault function=4.
constexpr uint32_t kFaultFuncIndex = 4;
// AOT modules place the PC -> table-slot lookup right after the fault function.
constexpr uint32_t kAotLookupFuncIndex = 5;
constexpr uint32_t kAotTransferFuncIndex = 6;
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
    // Fault-trampoline shape (R3j): promoted flag locals live in run()'s
    // frame, so the out-of-line fault function round-trips other_psr
    // through a param/return; reference emission RMWs memory instead.
    bool uses_flag_locals() const { return options.promote_flags; }
    uint32_t other_psr_local() const { return kOtherPsr; }
    // True when the emitter may drop the per-access fast-path-enabled
    // guard (T3): the host proved all bases nonzero for this module's
    // lifetime. Single-block emission always keeps the guard.
    bool skip_fast_guard() const { return options.assume_fast_bases; }
    bool count_fast_memory() const { return options.count_fast_memory; }
    bool aot() const { return options.aot; }
    bool unchecked_memory() const { return memory64 && options.aot && options.unchecked_memory; }
    bool fast_fp() const { return options.aot && options.fast_fp; }
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
        // Modeled T/E/IT updates, the sticky Q flag and GE writes use this
        // operation. NZCV have their own representation; full CPSR writers
        // remain unsupported IR.
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
        if (options.aot)
            return; // AOT functions keep no dispatch-iteration count
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
HostAddress scratch_write_epochs() {
#ifdef __EMSCRIPTEN__
    // Generated code shares this linear memory, so a host pointer is the
    // Wasm address of the table.
    static std::array<uint32_t, 1u << 20> table{};
    return reinterpret_cast<HostAddress>(table.data());
#else
    // Native builds only generate modules; they run in another address
    // space (the Node fixture runner), where no host pointer means anything.
    // That runner owns the table and the state must name it explicitly.
    return 0;
#endif
}
uint32_t g_ablate_override = 0;
void set_ablate_flags(uint32_t flags) { g_ablate_override = flags; }
RegionStateOptions region_state_options() {
    static const RegionStateOptions options = [] {
#ifdef __EMSCRIPTEN__
        const uint32_t flags = vita3k_promoted_state_options();
        return RegionStateOptions{(flags & 1) != 0, (flags & 2) != 0, false, (flags & 4) != 0};
#else
        const char *both = std::getenv("VITA3K_WASMJIT_PROMOTED_STATE");
        const bool fallback = both && std::strcmp(both, "1") == 0;
        const auto enabled = [&](const char *name, bool def) {
            const char *value = std::getenv(name);
            return value ? std::strcmp(value, "1") == 0 : def;
        };
        // Production default (R2): promoted flags ON unless explicitly "0".
        return RegionStateOptions{enabled("VITA3K_WASMJIT_PROMOTE_FLAGS", true),
            enabled("VITA3K_WASMJIT_PROMOTE_ACCOUNTING", fallback), false,
            enabled("VITA3K_WASMJIT_COUNT_FAST_MEMORY", false)};
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
} // namespace
bool ablation_removes_budget_checks() { return (ablate_flags() & kAblateBudget) != 0; }
namespace {
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
    // Options default to the reference policy; the diagnostic per-access
    // memory counters must be requested explicitly (see count_fast_memory).
    explicit Emitter(const Dynarmic::IR::Block &block, uint32_t hot_nid = 0,
        RegionStateOptions options = {})
        : block(block), start(block.Location()), finish(block.EndLocation()), state(options)
        , hot_nid(hot_nid) {}

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
        hot_nid = metadata[index].hot_nid;
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
                || opcode == Op::A32WriteMemory32 || opcode == Op::A32WriteMemory64
                || opcode == Op::A32ExclusiveReadMemory8 || opcode == Op::A32ExclusiveReadMemory16
                || opcode == Op::A32ExclusiveReadMemory32 || opcode == Op::A32ExclusiveReadMemory64
                || opcode == Op::A32ExclusiveWriteMemory8 || opcode == Op::A32ExclusiveWriteMemory16
                || opcode == Op::A32ExclusiveWriteMemory32 || opcode == Op::A32ExclusiveWriteMemory64;
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
        section(module, 1, {2, 0x60, 1, host_type, 1, 0x7f,
            0x60, 3, host_type, 0x7f, 0x7f, 1, 0x7f}); // block and checked helpers
        auto imports = memory_imports(4);
        imports.insert(imports.end(), {
            3, 'e', 'n', 'v', 8, 'm', 'e', 'm', '_', 'r', 'e', 'a', 'd', 0, 1,
            3, 'e', 'n', 'v', 9, 'm', 'e', 'm', '_', 'w', 'r', 'i', 't', 'e', 0, 1,
            3, 'e', 'n', 'v', 4, 'f', 'p', '6', '4', 0, 1});
        section(module, 2, imports);
        section(module, 3, {1, 0}); // one function, type 0
        section(module, 7, {1, 5, 'b', 'l', 'o', 'c', 'k', 0, 3});
        section(module, 10, functions);
        return module;
    }

private:
    const Dynarmic::IR::Block &block;
    Location start, finish;
    Bytes code;
    RegionState state; // reference policy unless the caller supplies options
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
    void reject(const char *why) {
        ok = false;
        if (rejection.empty()) rejection = why;
        // Env-gated diagnostic: a rejected route is otherwise only visible as an
        // empty module, which makes it hard to tell which case is missing.
        if (std::getenv("VITA3K_WASMJIT_REJECT_TRACE"))
            std::fprintf(stderr, "WasmJit emit reject: %s\n", why);
    }
    bool svc = false;
    uint32_t hot_nid = 0, svc_inline_success_local = 0;
    bool pc_written = false;
    bool has_bx = false;
    bool end_mode_written = false; // the body stored finish's mode bits
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
    void load(uint32_t offset) { b_load(code, offset); }
    void store(uint32_t offset) { b_store(code, offset); }
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
            // Outlined fault path (R3j): per-site fault arms share one
            // per-module cold function (state, fault pc, mode bits, and
            // other_psr in promoted mode), keeping hot functions small
            // enough to tier up. Ticks (site-const) accumulate in the
            // caller's executed_call; reason and epilogue branch stay
            // inline (depth-sensitive). Mode validation runs here at
            // emission, as it did inside upper_location.
            // IT may have advanced since block entry. Preserve the current
            // arithmetic flags while restoring the faulting instruction's mode.
            if (!valid_location(fault_location))
                ok = false;
            get(0);
            imm(fault_location.PC());
            imm(fault_location.CPSR().Value() & Location::CPSR_MODE_MASK);
            if (state.uses_flag_locals())
                get(state.other_psr_local());
            op(Call);
            uleb(code, kFaultFuncIndex);
            if (state.uses_flag_locals())
                set(state.other_psr_local());
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

    // sceKernelGetTLSAddr intrinsic (see kGetTlsAddrNid). Success writes r0
    // and clears the exclusive monitor exactly like the serviced SVC does.
    void inline_tls_addr() {
        const auto success = next_local, key = next_local + 1;
        svc_inline_success_local = success;
        if (region) get(reg_base); else load(offsetof(JitState, regs));
        set(key);
        get(key); imm(0x100); op(LeU); set(success);
        get(success); begin_if();
        const auto result = [&] {
            load(offsetof(JitState, tpidruro)); imm(kKernelTlsSize); op(Sub);
            get(key); imm(2); op(Shl); op(Add);
        };
        if (region) { result(); set(reg_base); }
        else { get(0); result(); store(offsetof(JitState, regs)); }
        store_constant(offsetof(JitState, exclusive_size), 0);
        end_if();
    }

    // HLE intrinsics run entirely in generated Wasm. Every failure jumps out
    // of this probe block WITHOUT changing registers, guest memory, or mutex
    // ownership, then takes the normal SVC terminal. All commit addresses
    // have been validated before the first write. No helper call can suspend
    // between the probe and commit: this is cooperative, NOT a parallel CAS.
    void inline_mutex() {
        const auto success = next_local;
        const auto address = next_local + 1, page = next_local + 2;
        const auto backing = next_local + 3, slot_offset = next_local + 4;
        const auto count = next_local + 5, next_count = next_local + 6;
        const auto owner = next_local + 7, tid = next_local + 8, request = next_local + 9;
        svc_inline_success_local = success;
        imm(0); set(success);
        const auto increment = [&](uint32_t offset) {
            get(0); load(offset); imm(1); op(Add); store(offset);
        };
        const auto reg = [&](uint32_t r) {
            if (region) get(reg_base + r);
            else load(offsetof(JitState, regs) + r * 4);
        };
        const auto entry_address = [&] {
            b_load_host(code, offsetof(JitState, mutex_table));
            get(slot_offset); address_add_i32(code);
        };
        const auto entry_load = [&](uint32_t offset) {
            entry_address(); op(Load); memarg(code, 2, offset);
        };
        const auto entry_store = [&](uint32_t offset, const auto &value) {
            entry_address(); value(); op(Store); memarg(code, 2, offset);
        };
        const auto work_load = [&](uint32_t offset) {
            guest_effective_address(address, backing); op(Load); memarg(code, 2, offset);
        };

        op(Block); op(0x40); ++extra_labels;
        const auto probe_labels = extra_labels;
        const auto decline_if = [&] {
            op(BrIf); uleb(code, extra_labels - probe_labels);
        };
        b_load_host(code, offsetof(JitState, mutex_table)); op(host_eqz); decline_if();
        memory_disabled(); decline_if();
        reg(0); set(address);
        reg(1); set(request);
        load(offsetof(JitState, guest_thread_id)); set(tid);
        get(tid); op(Eqz); decline_if();
        get(tid); imm(kInlineMutexNoOwner); op(Eq); decline_if();
        get(request); imm(0); op(GtS); op(Eqz); decline_if();

        // Workarea owner/count/attr/uid are within [r0,r0+20). Out-of-range,
        // cross-page, unaligned, readonly, or cached-code storage stays HLE.
        get(address); imm(3); op(And); decline_if();
        get(address); imm(12); op(ShrU); set(page);
        get(page); op(Eqz); decline_if();
        get(address); mask(0xfff); imm(4096 - 20); op(GtU); decline_if();
        memory_base(page_perms_local, offsetof(JitState, page_perms_base));
        get(page); address_add_i32(code);
        op(Load8U); memarg(code, 0, 0); mask(3); imm(3); op(Ne); decline_if();
        if (!memory64) {
            memory_base(page_table_local, offsetof(JitState, page_table_base));
            get(page); imm(2); op(Shl); op(Add);
            op(Load); memarg(code, 2, 0); set(backing);
            get(backing); op(Eqz); decline_if();
        }
        memory_base(code_pages_local, offsetof(JitState, code_pages_base));
        get(page); imm(2); op(Shl); address_add_i32(code);
        op(Load); memarg(code, 2, 0); decline_if();

        // Host-owned, fixed-size direct-mapped registry. Collisions, stale
        // guest UIDs, and guest-written mirror words must not alias a mutex.
        get(address); imm(5); op(ShrU);
        get(address); imm(15); op(ShrU); op(Xor);
        mask(kInlineMutexEntries - 1); imm(5); op(Shl);
        imm(offsetof(InlineMutexTable, entries)); op(Add); set(slot_offset);
        entry_load(offsetof(InlineMutexEntry, enabled)); imm(1); op(Ne); decline_if();
        entry_load(offsetof(InlineMutexEntry, workarea)); get(address); op(Ne); decline_if();
        entry_load(offsetof(InlineMutexEntry, uid)); work_load(16); op(Ne); decline_if();
        entry_load(offsetof(InlineMutexEntry, attr)); work_load(12); op(Ne); decline_if();
        entry_load(offsetof(InlineMutexEntry, count)); set(count);
        get(count); work_load(8); op(Ne); decline_if();
        get(count); imm(INT32_MAX); op(GtU); decline_if();
        entry_load(offsetof(InlineMutexEntry, owner)); set(owner);
        get(owner); work_load(0); op(Ne); decline_if();
        if (hot_nid == kInlineMutexLockNid) {
            get(count); op(Eqz); begin_if();
            get(owner); imm(kInlineMutexNoOwner); op(Ne); decline_if();
            op(Else);
            get(owner); get(tid); op(Ne); decline_if();
            entry_load(offsetof(InlineMutexEntry, attr)); mask(kInlineMutexRecursive);
            op(Eqz); decline_if();
            // Overflow and invalid counts remain owned by the ordinary HLE.
            get(count); imm(INT32_MAX); get(request); op(Sub); op(GtU); decline_if();
            end_if();
            get(count); get(request); op(Add); set(next_count);
            get(tid); set(owner);
        } else {
            get(owner); get(tid); op(Ne); decline_if();
            get(request); get(count); op(GtU); decline_if();
            get(count); get(request); op(Sub); set(next_count);
            get(next_count); op(Eqz); begin_if();
            imm(kInlineMutexNoOwner); set(owner);
            end_if();
        }

        // Commit: no further fallible operations, traps, host calls or yields.
        // Link each changed slot once; the host drains ONLY this short list
        // before exposing state to HLE, another fiber, or thread retirement.
        entry_load(offsetof(InlineMutexEntry, dirty)); op(Eqz); begin_if();
        entry_store(offsetof(InlineMutexEntry, next_dirty), [&] {
            b_load_host(code, offsetof(JitState, mutex_table));
            op(Load); memarg(code, 2, offsetof(InlineMutexTable, dirty_head));
        });
        b_load_host(code, offsetof(JitState, mutex_table));
        get(slot_offset); imm(offsetof(InlineMutexTable, entries)); op(Sub);
        imm(5); op(ShrU); imm(1); op(Add);
        op(Store); memarg(code, 2, offsetof(InlineMutexTable, dirty_head));
        entry_store(offsetof(InlineMutexEntry, dirty), [&] { imm(1); });
        end_if();
        entry_store(offsetof(InlineMutexEntry, count), [&] { get(next_count); });
        entry_store(offsetof(InlineMutexEntry, owner), [&] { get(owner); });
        guest_effective_address(address, backing); get(next_count);
        op(Store); memarg(code, 2, 8);
        guest_effective_address(address, backing); get(owner);
        op(Store); memarg(code, 2, 0);
        mark_written(address);
        store_constant(offsetof(JitState, exclusive_size), 0);
        if (region) { imm(0); set(reg_base); }
        else store_constant(offsetof(JitState, regs), 0);
        increment(hot_nid == kInlineMutexLockNid ? offsetof(JitState, mutex_fast_take)
                                               : offsetof(JitState, mutex_fast_release));
        imm(1); set(success);
        op(End); --extra_labels;
        get(success); op(Eqz); begin_if();
        increment(offsetof(JitState, mutex_fast_fallback));
        end_if();
    }

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
    void memory_slow_call(const Inst &inst, bool write, unsigned bytes, uint32_t reason = 0, uint32_t flags = 0) {
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
        get(0); value_word(inst.GetArg(1)); imm(bytes | (reason << 8) | flags); op(Call); uleb(code, write ? 1 : 0);
        checked_status();
        if (!write) {
            for (unsigned i = 0; i < (bytes + 3) / 4; ++i) { get(0); op(Load); uleb(code, 2); uleb(code, offsetof(JitState, memory_value) + i * 4); set(next_local + i); }
        }
    }
    void memory_base(uint32_t local, uint32_t offset) {
        if (region)
            get(local);
        else
            b_load_host(code, offset);
    }
    void memory_disabled() {
        if (!memory64) {
            memory_base(page_table_local, offsetof(JitState, page_table_base)); op(host_eqz);
        }
        memory_base(page_perms_local, offsetof(JitState, page_perms_base)); op(host_eqz);
        if (!memory64) op(Or);
        memory_base(code_pages_local, offsetof(JitState, code_pages_base)); op(host_eqz); op(Or);
    }
    // Guest write tracking: store the current write epoch into the page
    // entry of the byte at `address` + `offset` (mem_mark_written's inline form).
    void mark_written(uint32_t address, uint32_t offset = 0) {
        b_load_host(code, offsetof(JitState, write_epochs_base));
        get(address);
        if (offset) { imm(offset); op(Add); }
        imm(12); op(ShrU); imm(2); op(Shl); address_add_i32(code);
        load(offsetof(JitState, write_epoch));
        b_shared_store32(code); uleb(code, 2); uleb(code, 0);
    }
    void guest_effective_address(uint32_t address, uint32_t backing) {
        if (memory64) {
            get(address); op(ExtendU);
            constant64(code, vita3k::memory::guest_window_base); op(Add64);
        } else {
            get(backing); get(address); imm(0xfff); op(And); op(Add);
        }
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
            memory_disabled();
            begin_if();
            memory_slow_call(inst, write, bytes, kSlowOther);
            op(Else);
        }
        // ok = page_nonzero & perm_ok & mapped & in_page [& code_ok].
        get(page_local); op(Eqz); op(Eqz);
        memory_base(page_perms_local, offsetof(JitState, page_perms_base));
        get(page_local); address_add_i32(code);
        op(Load8U); uleb(code, 0); uleb(code, 0);
        imm(required); op(And); imm(required); op(Eq);
        op(And);
        if (!memory64) {
            memory_base(page_table_local, offsetof(JitState, page_table_base));
            get(page_local); imm(2); op(Shl); op(Add);
            op(Load); uleb(code, 2); uleb(code, 0);
            set(base_local);
            get(base_local); op(Eqz); op(Eqz);
            op(And);
        }
        // Direct mode: permission Read/Write implies a live allocation. Init,
        // free and aligned trimming clear permission bytes to None. No PTE,
        // physical-page pointer, or translated backing base is loaded here.
        get(addr_local); imm(0xfff); op(And); imm(4096 - bytes); op(GtU); op(Eqz);
        op(And);
        if (write) {
            memory_base(code_pages_local, offsetof(JitState, code_pages_base));
            get(page_local); imm(2); op(Shl); address_add_i32(code);
            b_shared_load32(code); uleb(code, 2); uleb(code, 0);
            op(Eqz);
            op(And);
        }
        op(Eqz);
        begin_if();
        memory_slow_reason(inst, write, bytes, emit_fast_guard, addr_local, page_local, base_local);
        op(Else);
        if (write) {
            // Direct store: base + (addr & 0xfff).
            guest_effective_address(addr_local, base_local);
            value_word(inst.GetArg(2), 0);
            if (bytes == 1) { op(Store8); uleb(code, 0); uleb(code, 0); }
            else if (bytes == 2) { op(Store16); uleb(code, 1); uleb(code, 0); }
            else { op(Store); uleb(code, 2); uleb(code, 0); }
            mark_written(addr_local); // the fast path never crosses a page
            if (state.count_fast_memory()) {
                get(0); load(offsetof(JitState, mem_fast_writes)); imm(1); op(Add);
                store(offsetof(JitState, mem_fast_writes));
            }
        } else {
            // Direct load: base + (addr & 0xfff); 8/16-bit zero-extending.
            guest_effective_address(addr_local, base_local);
            if (bytes == 1) { op(Load8U); uleb(code, 0); uleb(code, 0); }
            else if (bytes == 2) { op(Load16U); uleb(code, 1); uleb(code, 0); }
            else { op(Load); uleb(code, 2); uleb(code, 0); }
            set(next_local);
            if (state.count_fast_memory()) {
                get(0); load(offsetof(JitState, mem_fast_reads)); imm(1); op(Add);
                store(offsetof(JitState, mem_fast_reads));
            }
        }
        end_if(); // single probe gate
        if (emit_fast_guard)
            end_if(); // fast-path enabled
    }

    // Unchecked guest-window access (RegionStateOptions::unchecked_memory).
    // Wider accesses are consecutive little-endian words, exactly the
    // memory_value layout the checked helper produces and consumes.
    void memory_direct(const Inst &inst, bool write, unsigned bytes) {
        const uint32_t addr_local = next_local + 9;
        value_word(inst.GetArg(1));
        set(addr_local);
        // The fixed window base rides in the memarg offset (u64 in Memory64).
        for (unsigned word = 0; word < (bytes + 3) / 4; ++word) {
            const uint64_t offset = vita3k::memory::guest_window_base + word * 4;
            get(addr_local); op(ExtendU);
            if (write) {
                value_word(inst.GetArg(2), word);
                if (bytes == 1) { op(Store8); memarg(code, 0, offset); }
                else if (bytes == 2) { op(Store16); memarg(code, 1, offset); }
                else { op(Store); memarg(code, 2, offset); }
            } else {
                if (bytes == 1) { op(Load8U); memarg(code, 0, offset); }
                else if (bytes == 2) { op(Load16U); memarg(code, 1, offset); }
                else { op(Load); memarg(code, 2, offset); }
                set(next_local + word);
            }
        }
        if (write) {
            // At most 16 bytes span at most two pages: mark both ends.
            mark_written(addr_local);
            if (bytes > 1)
                mark_written(addr_local, bytes - 1);
        }
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
            memory_disabled();
            begin_if();
            memory_slow_call(inst, write, bytes, kSlowOther);
            op(Else);
        }
        memory_base(page_perms_local, offsetof(JitState, page_perms_base));
        get(page_local); address_add_i32(code);
        op(Load8U); uleb(code, 0); uleb(code, 0);
        imm(required); op(And); imm(required); op(Ne);
        begin_if();
        memory_slow_call(inst, write, bytes, kSlowPerms);
        op(Else);
        if (!memory64) {
            get(base_local);
            op(Eqz);
            begin_if();
            memory_slow_call(inst, write, bytes, kSlowUnmapped);
            op(Else);
        }
        get(addr_local); imm(0xfff); op(And); imm(4096 - bytes); op(GtU);
        begin_if();
        memory_slow_call(inst, write, bytes, kSlowCrossPage);
        op(Else);
        if (write) {
            memory_base(code_pages_local, offsetof(JitState, code_pages_base));
            get(page_local); imm(2); op(Shl); address_add_i32(code);
            b_shared_load32(code); uleb(code, 2); uleb(code, 0);
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
        if (!memory64) end_if(); // sparse mapping
        end_if(); // permissions
        if (emit_fast_guard)
            end_if(); // fast-path enabled
        end_if(); // guest page 0
    }

    void memory_call(const Inst &inst, bool write, unsigned bytes) {
        if (inst.GetArg(1).GetType() != Type::U32) { reject("memory_call arg1 not U32"); return; }
        if (!inst.GetArg(0).IsImmediate()) { reject("memory_call arg0 not imm"); return; }
        pending_fault_location = inst.GetArg(0).GetImmediateAsU64();
        if (state.unchecked_memory())
            memory_direct(inst, write, bytes);
        else if (bytes <= 4)
            memory_fast_or_slow(inst, write, bytes);
        else
            memory_slow_call(inst, write, bytes);
    }

    // Exclusive load/store (A32 LDREX/STREX family). Both accesses use the
    // checked helper path: an exclusive access must fault exactly like a plain
    // one, and the store needs a fresh read of the target to decide whether the
    // reservation is still valid. The reservation is (address, width, value);
    // STREX succeeds only when all three still match, which is the
    // compare-and-swap Dynarmic's native backend performs through
    // ExclusiveMonitor plus MemoryWriteExclusive. Ordinary stores do not clear
    // the reservation: the fresh read is what makes a lost update fail.
    void exclusive_read(const Inst &inst, unsigned bytes) {
        if (inst.GetArg(1).GetType() != Type::U32) { reject("exclusive read arg1 not U32"); return; }
        if (!inst.GetArg(0).IsImmediate()) { reject("exclusive read arg0 not imm"); return; }
        value_word(inst.GetArg(1)); set(next_local + 5);
        pending_fault_location = inst.GetArg(0).GetImmediateAsU64();
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
        memory_slow_call(inst, false, bytes, 0, kExclusiveAccessFlag);
#else
        memory_slow_call(inst, false, bytes);
#endif
        get(0); get(next_local + 5); store(offsetof(JitState, exclusive_address));
        for (unsigned i = 0; i < (bytes + 3) / 4; ++i) {
            get(0); get(next_local + i);
            store(offsetof(JitState, exclusive_value) + i * 4);
        }
        if (bytes <= 4)
            store_constant(offsetof(JitState, exclusive_value_hi), 0);
        store_constant(offsetof(JitState, exclusive_size), bytes);
    }
    void exclusive_write(const Inst &inst, unsigned bytes) {
        if (inst.GetArg(1).GetType() != Type::U32) { reject("exclusive write arg1 not U32"); return; }
        if (!inst.GetArg(0).IsImmediate()) { reject("exclusive write arg0 not imm"); return; }
        value_word(inst.GetArg(1)); set(next_local + 5);
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
        // Threaded build: other guest threads store concurrently, so the
        // re-read, compare and write below would race. The helper instead
        // compares and swaps atomically against the reserved value
        // (kExclusiveAccessFlag) when the reservation matches this store.
        imm(1); set(next_local + 4); // STREX fails unless the swap stores
        load(offsetof(JitState, exclusive_size)); imm(bytes); op(Eq);
        load(offsetof(JitState, exclusive_address)); get(next_local + 5); op(Eq); op(And);
        begin_if();
        pending_fault_location = inst.GetArg(0).GetImmediateAsU64();
        memory_slow_call(inst, true, bytes, 0, kExclusiveAccessFlag);
        get(0); op(Load); uleb(code, 2); uleb(code, offsetof(JitState, memory_value) + 8); set(next_local + 4);
        end_if();
        // Success or failure, the store consumes the reservation.
        store_constant(offsetof(JitState, exclusive_size), 0);
        get(next_local + 4); set(next_local);
        return;
#endif
        pending_fault_location = inst.GetArg(0).GetImmediateAsU64();
        // Words 0..1 take the fresh read; the result is staged in word 4 and
        // published after the branch, because the write arm reads inst arg2.
        memory_slow_call(inst, false, bytes);
        imm(1); set(next_local + 4); // STREX fails unless the branch stores
        load(offsetof(JitState, exclusive_size)); imm(bytes); op(Eq);
        load(offsetof(JitState, exclusive_address)); get(next_local + 5); op(Eq); op(And);
        for (unsigned i = 0; i < (bytes + 3) / 4; ++i) {
            load(offsetof(JitState, exclusive_value) + i * 4); get(next_local + i); op(Eq); op(And);
        }
        begin_if();
        pending_fault_location = inst.GetArg(0).GetImmediateAsU64();
        memory_slow_call(inst, true, bytes);
        store_constant(offsetof(JitState, exclusive_size), 0);
        imm(0); set(next_local + 4);
        op(Else);
        // A failed store still consumes the reservation on ARM.
        store_constant(offsetof(JitState, exclusive_size), 0);
        end_if();
        get(next_local + 4); set(next_local);
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
    // Scalar binary64 helpers. Wasm exposes exactly one i64 scratch local, so
    // double results are staged as two i32 SSA words and reinterpreted on
    // demand; declaring more locals would renumber the SSA range that region
    // metadata and dispatch depend on.
    void store_i64_words(uint32_t slot) {
        set(scratch_local);
        get(scratch_local); op(Wrap); set(slot);
        get(scratch_local); constant64(code, 32); op(ShrU64); op(Wrap); set(slot + 1);
    }
    void store_f64_words(uint32_t slot) {
        op(0xbd); // i64.reinterpret_f64
        store_i64_words(slot);
    }
    void push_i64_words(uint32_t slot) {
        get(slot); op(ExtendU);
        get(slot + 1); op(ExtendU); constant64(code, 32); op(Shl64); op(Or64);
    }
    void push_f64_words(uint32_t slot) {
        push_i64_words(slot);
        op(0xbf); // f64.reinterpret_i64
    }
    void push_f64_value(const Value &value) {
        value64(value);
        op(0xbf); // f64.reinterpret_i64
    }
    void push_f64_imm(int64_t bits) {
        constant64(code, bits);
        op(0xbf);
    }
    // |x| test on two words: result is 1 when the magnitude is a NaN.
    void f64_words_are_nan(uint32_t slot, uint32_t dest) {
        get(slot + 1); mask(0x7fffffff); set(dest);
        get(dest); imm(0x7ff00000); op(GtU);
        get(dest); imm(0x7ff00000); op(Eq);
        get(slot); op(Eqz); op(Eqz); op(And);
        op(Or); set(dest);
    }
    // 1 when |x| is nonzero and below the smallest normal binary64 value.
    void f64_words_are_subnormal(uint32_t slot, uint32_t dest) {
        get(slot + 1); mask(0x7fffffff); set(dest);
        get(dest); imm(0x00100000); op(LtU);
        get(dest); get(slot); op(Or); op(Eqz); op(Eqz); op(And);
        set(dest);
    }

    // Vectors use four consecutive i32 SSA words, not host SIMD locals. All
    // producers define every word; region members reuse the same SSA slots.
    bool vector_index(const Inst &inst, unsigned bits, unsigned &index) {
        const auto lane = inst.GetArg(1);
        if (!lane.IsImmediate() || lane.GetType() != Type::U8) {
            reject("vector lane must be an immediate U8");
            return false;
        }
        index = lane.GetU8();
        if (index >= 128 / bits) {
            reject("vector lane out of range");
            return false;
        }
        return true;
    }
    void vector_element_word(const Value &source, unsigned bits, unsigned index, unsigned word = 0) {
        const unsigned position = bits * index;
        value_word(source, position / 32 + word);
        if (position % 32) { imm(position % 32); op(ShrU); }
        if (bits < 32) mask((1u << bits) - 1);
    }
    bool vector_integer_arithmetic(const Inst &inst, unsigned bits, Wasm operation) {
        // Ordinary NEON integer arithmetic is modulo 2^esize, independent of
        // signedness. It changes neither CPSR nor FPSCR (including QC).
        // Read SSA sources and publish ALL four words before architectural
        // writeback, including for D-form operations whose high half is dead.
        if (bits == 64) {
            if (operation != Add && operation != Sub && operation != Mul) {
                reject("unsupported 64-bit vector integer arithmetic");
                return false;
            }
            for (unsigned word = 0; word < 4; word += 2) {
                for (unsigned source = 0; source < 2; ++source) {
                    value_word(inst.GetArg(source), word); op(ExtendU);
                    value_word(inst.GetArg(source), word + 1); op(ExtendU);
                    constant64(code, 32); op(Shl64); op(Or64);
                }
                op(operation == Add ? Add64 : operation == Mul ? Mul64 : Sub64);
                set(scratch_local);
                get(scratch_local); op(Wrap); set(next_local + word);
                get(scratch_local); constant64(code, 32); op(ShrU64); op(Wrap);
                set(next_local + word + 1);
            }
            return ok;
        }
        for (unsigned word = 0; word < 4; ++word) {
            // Build one packed i32 word explicitly. Without this accumulator,
            // the second lane would leave the previous result absent from the
            // Wasm stack when the shifted lane is ORed in.
            // scratch_local is an i64 local reserved for U64 lowering; use
            // this instruction's spare i32 SSA slot for packed lanes.
            imm(0); set(next_local + 4);
            for (unsigned shift = 0; shift < 32; shift += bits) {
                const unsigned lane = (word * 32 + shift) / bits;
                vector_element_word(inst.GetArg(0), bits, lane);
                vector_element_word(inst.GetArg(1), bits, lane);
                op(operation);
                // Truncate BEFORE combining: carry/borrow/product bits must
                // never leak from one packed lane to the next.
                if (bits < 32) mask((1u << bits) - 1);
                if (shift) { imm(shift); op(Shl); }
                get(next_local + 4); op(Or); set(next_local + 4);
            }
            get(next_local + 4); set(next_local + word);
        }
        return ok;
    }
    void byte_reverse_word_from_local(uint32_t source, uint32_t result) {
        // This is the scalar equivalent of Dynarmic's bswap. Keep every
        // partial byte in an i32 lane and combine only after its shift so the
        // emitter never depends on a host endianness or an i64 scratch slot.
        get(source); mask(0x000000ff); imm(24); op(Shl);
        get(source); mask(0x0000ff00); imm(8); op(Shl); op(Or);
        get(source); mask(0x00ff0000); imm(8); op(ShrU); op(Or);
        get(source); imm(24); op(ShrU); op(Or);
        set(result);
    }
    // VREV16/VREV32/VREV64: reverse `element`-bit lanes within each `group`.
    // A 64-bit group swaps its two words and reverses inside each of them.
    bool vector_reverse(const Inst &inst, unsigned element, unsigned group) {
        const auto source = next_local + 4;
        for (unsigned word = 0; word < 4; ++word) {
            value_word(inst.GetArg(0), group == 64 ? word ^ 1 : word); set(source);
            if (element == 8 && group != 16) {
                byte_reverse_word_from_local(source, next_local + word);
                continue;
            }
            if (element == 8) { // bytes within each halfword
                get(source); mask(0x00ff00ffu); imm(8); op(Shl);
                get(source); imm(8); op(ShrU); mask(0x00ff00ffu); op(Or);
            } else if (element == 16) { // halfwords within the word
                get(source); imm(16); op(Shl); get(source); imm(16); op(ShrU); op(Or);
            } else {
                get(source); // whole words: the swap above is the reversal
            }
            set(next_local + word);
        }
        return ok;
    }
    // ARM QADD/QSUB family (UQADD8/UQADD16/UQSUB8/UQSUB16 and signed QADD8/
    // QSUB8/QADD16/QSUB16): per-lane saturating arithmetic, no GE flags.
    // Dynarmic lowers each to one U32->U32 IR op; lower it here lane by lane
    // in plain i32 Wasm. Every partial sum/difference fits in i32, so the
    // clamp is exact; lanes are reassembled only after their final shift.
    // Uses reserved per-instruction SSA words +4..+8, like inline_mutex.
    bool packed_saturating(const Inst &inst, unsigned lane_bits, bool is_signed, bool is_add) {
        const uint32_t a = next_local + 4, b = next_local + 5;
        const uint32_t acc = next_local + 6, lane = next_local + 7, tmp = next_local + 8;
        const uint32_t lanes = 32 / lane_bits;
        const uint32_t lane_mask = lane_bits == 32 ? 0xffffffffu : ((1u << lane_bits) - 1);
        const uint32_t max = is_signed ? (lane_mask >> 1) : lane_mask;
        const uint32_t min = is_signed ? static_cast<uint32_t>(0 - int32_t(max) - 1) : 0;
        value(inst.GetArg(0)); set(a);
        value(inst.GetArg(1)); set(b);
        imm(0); set(acc);
        for (uint32_t i = 0; i < lanes; ++i) {
            const uint32_t shift = i * lane_bits;
            get(a); imm(shift); op(ShrU); mask(lane_mask);
            if (is_signed) { imm(32 - lane_bits); op(Shl); imm(32 - lane_bits); op(ShrS); }
            get(b); imm(shift); op(ShrU); mask(lane_mask);
            if (is_signed) { imm(32 - lane_bits); op(Shl); imm(32 - lane_bits); op(ShrS); }
            is_add ? op(Add) : op(Sub);
            set(tmp);
            if (!is_signed && is_add) {
                // sum <= 2*max: clamp up only.
                imm(max); get(tmp); get(tmp); imm(max); op(GtU); op(Select); set(lane);
            } else if (!is_signed) {
                // diff wrapped: a < b (borrow) saturates to zero.
                imm(0); get(tmp); get(a); imm(shift); op(ShrU); mask(lane_mask);
                get(b); imm(shift); op(ShrU); mask(lane_mask); op(LtU); op(Select); set(lane);
            } else {
                // Signed sum/difference fits in i32: clamp both sides.
                imm(max); get(tmp); get(tmp); imm(max); op(GtS); op(Select); set(lane);
                imm(min); get(lane); get(lane); imm(min); op(LtS); op(Select); set(lane);
            }
            get(acc); get(lane); mask(lane_mask); imm(shift); op(Shl); op(Or); set(acc);
        }
        get(acc); set(next_local);
        return ok;
    }
    enum class VectorLaneOp { Equal, Greater, Minimum, Maximum, Absolute, AbsoluteDifference };
    bool vector_integer_select(const Inst &inst, unsigned bits, VectorLaneOp operation, bool is_signed) {
        // A32 integer comparisons/min/max/abs use 8/16/32-bit lanes. Signed
        // ordering requires sign extension BEFORE comparison, but the result
        // always contains the original low lane bits. These are not saturating
        // operations: abs(INT_MIN) retains INT_MIN's bits and must not set QC.
        const uint32_t a = next_local + 4, b = next_local + 5;
        for (unsigned word = 0; word < 4; ++word) {
            for (unsigned shift = 0; shift < 32; shift += bits) {
                for (unsigned source = 0; source < (operation == VectorLaneOp::Absolute ? 1u : 2u); ++source) {
                    vector_element_word(inst.GetArg(source), bits, (word * 32 + shift) / bits);
                    if (is_signed && bits < 32) { imm(32 - bits); op(Shl); imm(32 - bits); op(ShrS); }
                    set(source == 0 ? a : b);
                }
                switch (operation) {
                case VectorLaneOp::Equal:
                case VectorLaneOp::Greater:
                    imm(0); get(a); get(b); op(operation == VectorLaneOp::Equal ? Eq : GtS);
                    op(Sub); // predicate 0/1 -> architectural lane 0/all-ones
                    break;
                case VectorLaneOp::Minimum:
                case VectorLaneOp::Maximum:
                    // wasm.select consumes (if_true, if_false, condition).
                    // Compare a>b once, then choose b/a for minimum or a/b
                    // for maximum. Re-reading locals keeps the stack typed.
                    if (operation == VectorLaneOp::Minimum) { get(b); get(a); }
                    else { get(a); get(b); }
                    get(a); get(b); op(is_signed ? GtS : GtU); op(Select);
                    break;
                case VectorLaneOp::Absolute:
                    // (0-a) when the sign-extended lane is negative, else a.
                    imm(0); get(a); op(Sub); get(a);
                    get(a); imm(31); op(ShrS); op(Select);
                    break;
                case VectorLaneOp::AbsoluteDifference:
                    // Select a-b when a>b, otherwise b-a. The comparison is
                    // made before the wrapping subtraction, so signed
                    // INT_MAX/INT_MIN produces the architectural lane bits.
                    get(a); get(b); op(Sub); get(b); get(a); op(Sub);
                    get(a); get(b); op(is_signed ? GtS : GtU); op(Select);
                    break;
                }
                if (bits < 32) mask((1u << bits) - 1);
                if (shift) { imm(shift); op(Shl); op(Or); }
            }
            set(next_local + word);
        }
        return ok;
    }
    bool vector_immediate_shift(const Inst &inst, unsigned bits, bool left, bool arithmetic) {
        const auto amount = inst.GetArg(1);
        if (!amount.IsImmediate() || amount.GetType() != Type::U8) return false;
        const unsigned count = amount.GetU8();
        if (bits == 64) {
            for (unsigned word = 0; word < 4; word += 2) {
                if (count >= bits && !arithmetic) {
                    constant64(code, 0);
                } else {
                    value_word(inst.GetArg(0), word); op(ExtendU);
                    value_word(inst.GetArg(0), word + 1); op(ExtendU);
                    constant64(code, 32); op(Shl64); op(Or64);
                    constant64(code, std::min(count, 63u));
                    op(left ? Shl64 : arithmetic ? ShrS64 : ShrU64);
                }
                store_i64_words(next_local + word);
            }
        } else {
            for (unsigned word = 0; word < 4; ++word) {
                for (unsigned offset = 0; offset < 32; offset += bits) {
                    if (count >= bits && !arithmetic) {
                        imm(0);
                    } else {
                        vector_element_word(inst.GetArg(0), bits, (word * 32 + offset) / bits);
                        if (arithmetic && bits < 32) {
                            imm(32 - bits); op(Shl); imm(32 - bits); op(ShrS);
                        }
                        imm(std::min(count, bits - 1));
                        op(left ? Shl : arithmetic ? ShrS : ShrU);
                        if (bits < 32) mask((1u << bits) - 1);
                    }
                    if (offset) { imm(offset); op(Shl); op(Or); }
                }
                set(next_local + word);
            }
        }
        return ok;
    }
    bool vector_broadcast(const Inst &inst, unsigned bits, bool element = false, bool lower = false) {
        unsigned index = 0;
        if (element && !vector_index(inst, bits, index)) return false;
        const unsigned words = bits == 64 ? 2 : 1;
        for (unsigned i = 0; i < words; ++i) {
            if (element) vector_element_word(inst.GetArg(0), bits, index, i);
            else value_word(inst.GetArg(0), i);
            if (bits < 32) {
                mask((1u << bits) - 1);
                imm(bits == 8 ? 0x01010101 : 0x00010001); op(Mul);
            }
            set(next_local + i);
        }
        for (unsigned i = words; i < 4; ++i) {
            if (lower && i >= 2) imm(0);
            else get(next_local + i % words);
            set(next_local + i);
        }
        return ok;
    }
    bool vector_get_element(const Inst &inst, unsigned bits) {
        unsigned index;
        if (!vector_index(inst, bits, index)) return false;
        for (unsigned i = 0; i < (bits == 64 ? 2u : 1u); ++i) {
            vector_element_word(inst.GetArg(0), bits, index, i); set(next_local + i);
        }
        return ok;
    }
    bool vector_set_element(const Inst &inst, unsigned bits) {
        unsigned index;
        if (!vector_index(inst, bits, index)) return false;
        const unsigned first = bits * index / 32, shift = bits * index % 32;
        for (unsigned word = 0; word < 4; ++word) {
            if (word < first || word >= first + (bits == 64 ? 2u : 1u)) {
                value_word(inst.GetArg(0), word);
            } else if (bits >= 32) {
                value_word(inst.GetArg(2), word - first);
            } else {
                const uint32_t lane_mask = (1u << bits) - 1;
                value_word(inst.GetArg(0), word); mask(~(lane_mask << shift));
                value_word(inst.GetArg(2)); mask(lane_mask);
                if (shift) { imm(shift); op(Shl); }
                op(Or);
            }
            set(next_local + word);
        }
        return ok;
    }
    bool vector_extract(const Inst &inst, bool lower) {
        const auto offset = inst.GetArg(2);
        const unsigned words = lower ? 2 : 4;
        if (!offset.IsImmediate() || offset.GetType() != Type::U8
            || offset.GetU8() > words * 32 || offset.GetU8() % 8) {
            reject("vector extract requires an in-range byte-aligned bit offset");
            return false;
        }
        const unsigned position = offset.GetU8();
        // Concatenate b:a, with a least significant. Lower uses just each
        // operand's low 64 bits, not a's possibly undefined upper lanes.
        const auto joined_word = [&](unsigned index) {
            value_word(inst.GetArg(index < words ? 0 : 1), index % words);
        };
        for (unsigned i = 0; i < words; ++i) {
            const unsigned index = position / 32 + i, shift = position % 32;
            joined_word(index);
            if (shift) {
                imm(shift); op(ShrU);
                joined_word(index + 1); imm(32 - shift); op(Shl); op(Or);
            }
            set(next_local + i);
        }
        for (unsigned i = words; i < 4; ++i) { imm(0); set(next_local + i); }
        return ok;
    }
    // Result words [0, words) from `bits`-wide lanes (bits <= 32) that
    // lane(i) pushes, possibly with garbage above the lane width; the
    // remaining words are zero. The partial word stays on the Wasm stack, so
    // lane() may only use this instruction's scratch words (+4..+9).
    template <typename Lane>
    void pack_lanes(unsigned bits, unsigned words, const Lane &lane) {
        for (unsigned word = 0; word < 4; ++word) {
            if (word >= words) { imm(0); set(next_local + word); continue; }
            for (unsigned shift = 0; shift < 32; shift += bits) {
                lane((word * 32 + shift) / bits);
                if (bits < 32) mask((1u << bits) - 1);
                if (shift) { imm(shift); op(Shl); op(Or); }
            }
            set(next_local + word);
        }
    }
    // Lane `index` of a vector of `bits`-wide lanes (bits <= 32), sign- or
    // zero-extended to i32.
    void vector_lane(const Value &source, unsigned bits, unsigned index, bool is_signed) {
        vector_element_word(source, bits, index);
        if (is_signed && bits < 32) { imm(32 - bits); op(Shl); imm(32 - bits); op(ShrS); }
    }
    // Lane semantics below follow Dynarmic's x64 emitters in
    // backend/x64/emit_x64_vector.cpp (the IR reference, including the
    // lower-64-bit operand shapes of the *Lower operations).
    // VPADD: x = a[63:0] then b[63:0]; result lane i = x[2i] + x[2i+1] in
    // the low 64 bits, upper 64 bits zero.
    bool vector_paired_add_lower(const Inst &inst, unsigned bits) {
        const unsigned per_operand = 64 / bits;
        const auto x = [&](unsigned i) {
            vector_element_word(inst.GetArg(i < per_operand ? 0 : 1), bits, i % per_operand);
        };
        pack_lanes(bits, 2, [&](unsigned i) { x(2 * i); x(2 * i + 1); op(Add); });
        return ok;
    }
    // VZIP: lane 2i = a[base + i], lane 2i + 1 = b[base + i], where base
    // selects the low (Lower) or high (Upper) half of each operand.
    bool vector_interleave(const Inst &inst, unsigned bits, bool upper) {
        const unsigned base = upper ? 64 / bits : 0;
        if (bits == 64) {
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(word / 2), 2 * base + word % 2);
                set(next_local + word);
            }
            return ok;
        }
        pack_lanes(bits, 4, [&](unsigned j) { vector_element_word(inst.GetArg(j % 2), bits, base + j / 2); });
        return ok;
    }
    // VUZP and VPMIN/VPMAX: the even (or odd) lanes of a, then those of b.
    // The Lower forms read only a[63:0] and b[63:0] and zero the upper half.
    bool vector_deinterleave(const Inst &inst, unsigned bits, bool odd, bool lower) {
        if (bits == 64) {
            for (unsigned operand = 0; operand < 2; ++operand) {
                push_i64_value(inst.GetArg(operand), odd ? 2 : 0);
                store_i64_words(next_local + operand * 2);
            }
            return ok;
        }
        const unsigned per_operand = (lower ? 32 : 64) / bits; // selected lanes per operand
        pack_lanes(bits, lower ? 2 : 4, [&](unsigned i) {
            vector_element_word(inst.GetArg(i / per_operand), bits, 2 * (i % per_operand) + (odd ? 1 : 0));
        });
        return ok;
    }

    bool vector_transpose(const Inst &inst, unsigned bits) {
        if (!inst.GetArg(2).IsImmediate()) return false;
        const unsigned part = inst.GetArg(2).GetU1();
        if (bits == 64) {
            for (unsigned operand = 0; operand < 2; ++operand) {
                push_i64_value(inst.GetArg(operand), part * 2);
                store_i64_words(next_local + operand * 2);
            }
        } else {
            pack_lanes(bits, 4, [&](unsigned i) {
                vector_element_word(inst.GetArg(i % 2), bits, (i / 2) * 2 + part);
            });
        }
        return ok;
    }

    bool vector_reduce_add(const Inst &inst, unsigned bits) {
        if (bits == 64) {
            push_i64_value(inst.GetArg(0), 0); push_i64_value(inst.GetArg(0), 2); op(Add64);
            store_i64_words(next_local);
        } else {
            for (unsigned lane = 0; lane < 128 / bits; ++lane) {
                vector_element_word(inst.GetArg(0), bits, lane);
                if (lane) op(Add);
            }
            if (bits < 32) mask((1u << bits) - 1);
            set(next_local); imm(0); set(next_local + 1);
        }
        imm(0); set(next_local + 2); imm(0); set(next_local + 3);
        return ok;
    }
    // VPADDL: result lane i (twice the width) = a[2i] + a[2i + 1], each
    // sign- or zero-extended first.
    bool vector_paired_add_widen(const Inst &inst, unsigned bits, bool is_signed) {
        if (bits == 32) {
            const uint8_t extend = is_signed ? 0xac : ExtendU;
            for (unsigned lane = 0; lane < 2; ++lane) {
                value_word(inst.GetArg(0), 2 * lane); op(extend);
                value_word(inst.GetArg(0), 2 * lane + 1); op(extend);
                op(Add64);
                store_i64_words(next_local + 2 * lane);
            }
            return ok;
        }
        pack_lanes(2 * bits, 4, [&](unsigned i) {
            vector_lane(inst.GetArg(0), bits, 2 * i, is_signed);
            vector_lane(inst.GetArg(0), bits, 2 * i + 1, is_signed);
            op(Add);
        });
        return ok;
    }
    // VMOVL.S: the low 64 bits' lanes sign-extended to twice their width.
    bool vector_sign_extend(const Inst &inst, unsigned bits) {
        if (bits == 32) {
            for (unsigned lane = 0; lane < 2; ++lane) {
                value_word(inst.GetArg(0), lane); set(next_local + 2 * lane);
                value_word(inst.GetArg(0), lane); imm(31); op(ShrS); set(next_local + 2 * lane + 1);
            }
            return ok;
        }
        pack_lanes(2 * bits, 4, [&](unsigned i) { vector_lane(inst.GetArg(0), bits, i, true); });
        return ok;
    }
    // VHADD: floor((a + b) / 2) per lane with the full-width intermediate;
    // VRHADD (rounding): floor((a + b + 1) / 2).
    bool vector_halving_add(const Inst &inst, unsigned bits, bool is_signed, bool rounding = false) {
        if (bits == 32) {
            const uint8_t extend = is_signed ? ExtendS : ExtendU;
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word); op(extend);
                value_word(inst.GetArg(1), word); op(extend);
                op(Add64);
                if (rounding) { constant64(code, 1); op(Add64); }
                constant64(code, 1); op(is_signed ? ShrS64 : ShrU64); op(Wrap);
                set(next_local + word);
            }
            return ok;
        }
        pack_lanes(bits, 4, [&](unsigned i) {
            vector_lane(inst.GetArg(0), bits, i, is_signed);
            vector_lane(inst.GetArg(1), bits, i, is_signed);
            op(Add);
            if (rounding) { imm(1); op(Add); }
            imm(1); op(is_signed ? ShrS : ShrU);
        });
        return ok;
    }
    // VSHL.S (register): shift each lane of a by the signed low byte s of
    // the matching lane of b: s >= 0 shifts left (0 from s >= bits), s < 0
    // shifts right arithmetically by -s (the sign fill from s <= -bits)
    // (Dynarmic VShift<signed>).
    bool vector_arithmetic_vshift(const Inst &inst, unsigned bits) {
        const auto amount = next_local + 4, lane = next_local + 5;
        if (bits == 64) {
            for (unsigned word = 0; word < 4; word += 2) {
                value_word(inst.GetArg(1), word); imm(24); op(Shl); imm(24); op(ShrS); set(amount);
                push_i64_value(inst.GetArg(0), word); set(scratch_local);
                // left: amount < 64 ? x << amount : 0
                get(scratch_local); get(amount); op(ExtendU); op(Shl64);
                constant64(code, 0);
                get(amount); imm(64); op(LtS);
                op(Select);
                // right: x >> min(-amount, 63)
                get(scratch_local);
                imm(0); get(amount); op(Sub); set(lane);
                get(lane); imm(63); get(lane); imm(63); op(LtU); op(Select); op(ExtendU);
                op(ShrS64);
                get(amount); imm(0); op(GeS);
                op(Select);
                store_i64_words(next_local + word);
            }
            return ok;
        }
        pack_lanes(bits, 4, [&](unsigned i) {
            // The amount byte is the lowest byte of b's lane i.
            const unsigned position = bits * i;
            value_word(inst.GetArg(1), position / 32);
            if (position % 32) { imm(position % 32); op(ShrU); }
            imm(24); op(Shl); imm(24); op(ShrS); set(amount);
            vector_lane(inst.GetArg(0), bits, i, true); set(lane);
            get(lane); get(amount); op(Shl);
            imm(0);
            get(amount); imm(bits); op(LtS);
            op(Select);
            get(lane);
            imm(0); get(amount); op(Sub);
            imm(bits - 1);
            imm(0); get(amount); op(Sub); imm(bits - 1); op(LtU);
            op(Select);
            op(ShrS);
            get(amount); imm(0); op(GeS);
            op(Select);
        });
        return ok;
    }

    bool scalar_shift64(const Inst &inst, bool left, bool arithmetic_, bool rotate, bool masked) {
        value64(inst.GetArg(0));
        value_word(inst.GetArg(1)); op(ExtendU);
        op(rotate ? RotR64 : left ? Shl64 : arithmetic_ ? ShrS64 : ShrU64);
        if (!masked && !rotate) {
            if (arithmetic_) { value64(inst.GetArg(0)); constant64(code, 63); op(ShrS64); }
            else constant64(code, 0);
            value_word(inst.GetArg(1)); imm(64); op(LtU); op(Select);
        }
        store_i64_words(next_local);
        return ok;
    }

    bool scalar_shift32(const Inst &inst, bool left, bool arithmetic_, bool rotate, bool masked) {
        value_word(inst.GetArg(0));
        value_word(inst.GetArg(1));
        op(rotate ? RotR : left ? Shl : arithmetic_ ? ShrS : ShrU);
        if (!masked && !rotate) {
            if (arithmetic_) { value_word(inst.GetArg(0)); imm(31); op(ShrS); }
            else imm(0);
            value_word(inst.GetArg(1)); imm(32); op(LtU); op(Select);
        }
        set(next_local); return ok;
    }

    bool scalar_div(const Inst &inst, bool wide, bool signed_) {
        if (wide) {
            value64(inst.GetArg(1));
            set(scratch_local);
            get(scratch_local); op(Eqz64); begin_if();
            constant64(code, 0); store_i64_words(next_local); op(Else);
            if (signed_) {
                value64(inst.GetArg(0)); constant64(code, INT64_MIN); op(Eq64);
                value64(inst.GetArg(1)); constant64(code, -1); op(Eq64); op(And); begin_if();
                constant64(code, INT64_MIN); store_i64_words(next_local); op(Else);
            }
            value64(inst.GetArg(0)); value64(inst.GetArg(1)); op(signed_ ? DivS64 : DivU64);
            store_i64_words(next_local);
            if (signed_) end_if();
            end_if();
        } else {
            value_word(inst.GetArg(1)); set(next_local + 4);
            get(next_local + 4); op(Eqz); begin_if(true);
            imm(0); op(Else);
            if (signed_) {
                value_word(inst.GetArg(0)); imm(0x80000000); op(Eq);
                get(next_local + 4); imm(0xffffffff); op(Eq); op(And); begin_if(true);
                imm(0x80000000); op(Else);
            }
            value_word(inst.GetArg(0)); get(next_local + 4); op(signed_ ? DivS : DivU);
            if (signed_) end_if();
            end_if(); set(next_local);
        }
        return ok;
    }

    bool scalar_minmax(const Inst &inst, unsigned bits, bool maximum, bool signed_) {
        if (bits == 64) {
            value64(inst.GetArg(0)); store_i64_words(next_local + 4);
            value64(inst.GetArg(1)); store_i64_words(next_local + 6);
            push_i64_words(next_local + 4); push_i64_words(next_local + 6);
            op(signed_ ? GtS64 : GtU64); set(next_local + 8);
            push_i64_words(maximum ? next_local + 4 : next_local + 6);
            push_i64_words(maximum ? next_local + 6 : next_local + 4);
            get(next_local + 8); op(Select);
            store_i64_words(next_local);
        } else {
            value_word(inst.GetArg(0)); set(next_local + 4);
            value_word(inst.GetArg(1)); set(next_local + 5);
            get(next_local + 4); get(next_local + 5);
            op(signed_ ? GtS : GtU); set(next_local + 6);
            get(next_local + (maximum ? 4 : 5)); get(next_local + (maximum ? 5 : 4));
            get(next_local + 6); op(Select); set(next_local);
        }
        return ok;
    }

    bool multiply_high64(const Inst &inst, bool is_signed) {
        const auto limb = [&](unsigned operand, unsigned word) {
            value_word(inst.GetArg(operand), word); op(ExtendU);
        };
        // Base-2^32 long multiplication. Both cross-term accumulators fit
        // in 64 bits; their upper halves carry into the high product.
        limb(0, 0); limb(1, 0); op(Mul64); constant64(code, 32); op(ShrU64);
        limb(0, 1); limb(1, 0); op(Mul64); op(Add64); store_i64_words(next_local + 4);
        limb(0, 0); limb(1, 1); op(Mul64);
        get(next_local + 4); op(ExtendU); op(Add64); constant64(code, 32); op(ShrU64);
        get(next_local + 5); op(ExtendU); op(Add64);
        limb(0, 1); limb(1, 1); op(Mul64); op(Add64);
        if (is_signed) {
            for (unsigned operand = 0; operand < 2; ++operand) {
                value64(inst.GetArg(1 - operand));
                value64(inst.GetArg(operand)); constant64(code, 63); op(ShrS64); op(And64); op(Sub64);
            }
        }
        store_i64_words(next_local);
        return ok;
    }

    // Compact constant table: five comparisons choose an eight-byte i64,
    // then a shift extracts its byte. No guest-memory table or host call.
    void byte_lookup(const std::array<uint8_t, 256> &table, uint32_t index, unsigned begin = 0, unsigned count = 256) {
        if (count == 8) {
            uint64_t bytes = 0;
            for (unsigned i = 0; i < 8; ++i) bytes |= uint64_t(table[begin + i]) << (8 * i);
            constant64(code, int64_t(bytes)); get(index); mask(7); imm(3); op(Shl); op(ExtendU); op(ShrU64); op(Wrap); mask(0xff);
            return;
        }
        get(index); imm(begin + count / 2); op(LtU); begin_if(true);
        byte_lookup(table, index, begin, count / 2); op(Else);
        byte_lookup(table, index, begin + count / 2, count / 2); end_if();
    }

    bool aes_round(const Inst &inst, bool inverse) {
        namespace AES = Dynarmic::Common::Crypto::AES;
        static const auto tables = [] {
            std::array<std::array<uint8_t, 256>, 2> result{};
            for (unsigned i = 0; i < 256; ++i) {
                AES::State input{}, output{}; input.fill(uint8_t(i));
                AES::EncryptSingleRound(output, input); result[0][i] = output[0];
                AES::DecryptSingleRound(output, input); result[1][i] = output[0];
            }
            return result;
        }();
        pack_lanes(8, 4, [&](unsigned i) {
            const unsigned row = i % 4, column = i / 4;
            const unsigned source = 4 * ((column + (inverse ? 4 - row : row)) % 4) + row;
            vector_element_word(inst.GetArg(0), 8, source); set(next_local + 4);
            byte_lookup(tables[inverse], next_local + 4);
        });
        return ok;
    }

    bool aes_mix_columns(const Inst &inst, bool inverse) {
        constexpr unsigned forward[4]{2, 3, 1, 1}, backward[4]{14, 11, 13, 9};
        pack_lanes(8, 4, [&](unsigned i) {
            const unsigned row = i % 4, column = i / 4;
            for (unsigned j = 0; j < 4; ++j) {
                vector_element_word(inst.GetArg(0), 8, column * 4 + j); set(next_local + 5);
                for (unsigned power = 1; power <= (inverse ? 3u : 1u); ++power) {
                    get(next_local + 4 + power); imm(1); op(Shl);
                    imm(0); get(next_local + 4 + power); imm(7); op(ShrU); op(Sub); mask(0x1b); op(Xor); mask(0xff);
                    set(next_local + 5 + power);
                }
                const unsigned multiplier = (inverse ? backward : forward)[(j + 4 - row) % 4];
                bool first = true;
                for (unsigned power = 0; power < 4; ++power) if (multiplier & (1u << power)) {
                    get(next_local + 5 + power); if (!first) op(Xor); first = false;
                }
                if (j) op(Xor);
            }
        });
        return ok;
    }

    bool sha256(const Inst &inst, Op kind) {
        const auto sigma = [&](uint32_t slot, unsigned a, unsigned b, unsigned c, bool shift) {
            get(slot); imm(a); op(RotR); get(slot); imm(b); op(RotR); op(Xor);
            get(slot); imm(c); op(shift ? ShrU : RotR); op(Xor);
        };
        if (kind == Op::SHA256Hash) {
            if (!inst.GetArg(3).IsImmediate()) return false;
            for (unsigned i = 0; i < 8; ++i) { value_word(inst.GetArg(i / 4), i % 4); set(next_local + i); }
            for (unsigned round = 0; round < 4; ++round) {
                get(next_local + 7); sigma(next_local + 4, 6, 11, 25, false); op(Add);
                get(next_local + 4); get(next_local + 5); op(And);
                get(next_local + 4); imm(UINT32_MAX); op(Xor); get(next_local + 6); op(And); op(Xor); op(Add);
                value_word(inst.GetArg(2), round); op(Add); set(next_local + 8);
                sigma(next_local, 2, 13, 22, false);
                get(next_local); get(next_local + 1); op(And);
                get(next_local); get(next_local + 2); op(And); op(Xor);
                get(next_local + 1); get(next_local + 2); op(And); op(Xor); op(Add); set(next_local + 9);
                for (unsigned i = 7; i > 0; --i) {
                    get(next_local + i - 1);
                    if (i == 4) { get(next_local + 8); op(Add); }
                    set(next_local + i);
                }
                get(next_local + 8); get(next_local + 9); op(Add); set(next_local);
            }
            if (!inst.GetArg(3).GetU1()) for (unsigned i = 0; i < 4; ++i) { get(next_local + 4 + i); set(next_local + i); }
        } else {
            for (unsigned i = 0; i < 4; ++i) {
                value_word(inst.GetArg(0), i);
                if (kind == Op::SHA256MessageSchedule0) {
                    value_word(inst.GetArg(i == 3 ? 1 : 0), (i + 1) % 4); set(next_local + 4);
                    sigma(next_local + 4, 7, 18, 3, true);
                } else {
                    value_word(inst.GetArg(i == 3 ? 2 : 1), (i + 1) % 4); op(Add);
                    if (i < 2) value_word(inst.GetArg(2), i + 2);
                    else get(next_local + i - 2);
                    set(next_local + 4); sigma(next_local + 4, 17, 19, 10, true);
                }
                op(Add); set(next_local + i);
            }
        }
        return ok;
    }

    bool crc32(const Inst &inst, unsigned bits, uint32_t polynomial) {
        value_word(inst.GetArg(0)); set(next_local);
        for (unsigned byte = 0; byte < bits / 8; ++byte) {
            get(next_local);
            value_word(inst.GetArg(1), byte / 4);
            if (byte % 4) { imm((byte % 4) * 8); op(ShrU); }
            mask(0xff); op(Xor); set(next_local);
            for (unsigned bit = 0; bit < 8; ++bit) {
                get(next_local); imm(1); op(ShrU);
                imm(0); get(next_local); mask(1); op(Sub); mask(polynomial); op(Xor); set(next_local);
            }
        }
        return ok;
    }

    bool vector_full_multiply(const Inst &inst, unsigned bits, bool is_signed) {
        for (unsigned word = 0; word < 4; ++word) {
            if (bits == 32) {
                value_word(inst.GetArg(0), word); op(is_signed ? ExtendS : ExtendU);
                value_word(inst.GetArg(1), word); op(is_signed ? ExtendS : ExtendU);
                op(Mul64); set(scratch_local);
                get(scratch_local); op(Wrap); set(next_local + word);
                get(scratch_local); constant64(code, 32); op(ShrU64); op(Wrap); set(next_local + 4 + word);
            } else {
                for (unsigned half = 0; half < 2; ++half) {
                    for (unsigned lane = 0; lane < 2; ++lane) {
                        vector_lane(inst.GetArg(0), 16, word * 2 + lane, is_signed);
                        vector_lane(inst.GetArg(1), 16, word * 2 + lane, is_signed); op(Mul);
                        if (half) { imm(16); op(ShrU); }
                        mask(0xffff);
                        if (lane) { imm(16); op(Shl); op(Or); }
                    }
                    set(next_local + half * 4 + word);
                }
            }
        }
        return ok;
    }

    bool scalar_misc(const Inst &inst, Op kind) {
        const unsigned bits = inst.GetType() == Type::U64 ? 64 : 32;
        if (kind == Op::IsZero64 || kind == Op::IsZero32) {
            if (kind == Op::IsZero64) value64(inst.GetArg(0)); else value_word(inst.GetArg(0));
            op(kind == Op::IsZero64 ? Eqz64 : Eqz); set(next_local); return ok;
        }
        if (kind == Op::TestBit) {
            value64(inst.GetArg(0)); value_word(inst.GetArg(1)); op(ExtendU); op(ShrU64);
            constant64(code, 1); op(And64); op(Wrap); set(next_local); return ok;
        }
        if (kind == Op::ConditionalSelect64) {
            if (!inst.GetArg(0).IsImmediate() || inst.GetArg(0).GetType() != Type::Cond) return false;
            value64(inst.GetArg(1)); value64(inst.GetArg(2)); condition(inst.GetArg(0).GetCond()); op(Select); store_i64_words(next_local); return ok;
        }
        if (kind == Op::SignExtendByteToLong) {
            value_word(inst.GetArg(0)); imm(24); op(Shl); imm(24); op(ShrS); op(ExtendS); store_i64_words(next_local); return ok;
        }
        if (kind == Op::CountLeadingZeros64) {
            value64(inst.GetArg(0)); op(0x79); store_i64_words(next_local); return ok;
        }
        if (kind == Op::ExtractRegister32 || kind == Op::ExtractRegister64) {
            if (!inst.GetArg(2).IsImmediate()) return false;
            const unsigned shift_amount = inst.GetArg(2).GetU8();
            if (shift_amount >= bits) return false;
            if (bits == 64) {
                value64(inst.GetArg(0));
                if (shift_amount) { constant64(code, shift_amount); op(ShrU64); value64(inst.GetArg(1)); constant64(code, 64 - shift_amount); op(Shl64); op(Or64); }
                store_i64_words(next_local);
            } else {
                value_word(inst.GetArg(0));
                if (shift_amount) { imm(shift_amount); op(ShrU); value_word(inst.GetArg(1)); imm(32 - shift_amount); op(Shl); op(Or); }
                set(next_local);
            }
            return ok;
        }
        if (kind == Op::ReplicateBit32 || kind == Op::ReplicateBit64) {
            if (bits == 64) {
                constant64(code, 0); value64(inst.GetArg(0)); value_word(inst.GetArg(1)); op(ExtendU); op(ShrU64); constant64(code, 1); op(And64); op(Sub64); store_i64_words(next_local);
            } else {
                imm(0); value_word(inst.GetArg(0)); value_word(inst.GetArg(1)); op(ShrU); mask(1); op(Sub); set(next_local);
            }
            return ok;
        }
        return false;
    }

    bool packed_arithmetic(const Inst &inst, unsigned bits, bool signed_, bool subtract, bool halving, bool exchange = false) {
        imm(0); set(next_local);
        imm(0); set(next_local + 4);
        for (unsigned lane = 0; lane < 32 / bits; ++lane) {
            const bool sub = exchange ? (lane == 0 ? subtract : !subtract) : subtract;
            vector_lane(inst.GetArg(0), bits, lane, signed_);
            vector_lane(inst.GetArg(1), bits, exchange ? lane ^ 1 : lane, signed_);
            op(sub ? Sub : Add); set(next_local + 5);
            if (!halving && carry_needed.count(&inst)) {
                get(next_local + 4);
                imm(0); get(next_local + 5);
                imm(signed_ || sub ? 0 : 1u << bits); op(GeS); op(Sub);
                mask(((1u << bits) - 1) << (lane * bits)); op(Or); set(next_local + 4);
            }
            get(next_local + 5);
            if (halving) { imm(1); op(signed_ ? ShrS : ShrU); }
            mask((1u << bits) - 1);
            if (lane) { imm(lane * bits); op(Shl); }
            get(next_local); op(Or); set(next_local);
        }
        return ok;
    }

    bool scalar_saturating(const Inst &inst, unsigned bits, bool signed_, bool add) {
        return vector_saturated_add_sub(inst, bits, signed_, add, true);
    }

    bool vector_mixed_accumulate(const Inst &inst, unsigned bits, bool signed_result) {
        const auto saturated = next_local + 4, overflow = next_local + 5;
        imm(0); set(saturated);
        if (bits < 64) {
            pack_lanes(bits, 4, [&](unsigned i) {
                vector_lane(inst.GetArg(0), bits, i, !signed_result); op(signed_result ? ExtendU : ExtendS);
                vector_lane(inst.GetArg(1), bits, i, signed_result); op(signed_result ? ExtendS : ExtendU);
                op(Add64); set(scratch_local);
                clamp_i64_low(signed_result ? -(int64_t(1) << (bits - 1)) : 0,
                    signed_result ? (int64_t(1) << (bits - 1)) - 1 : (int64_t(1) << bits) - 1, saturated);
            });
        } else {
            for (unsigned word = 0; word < 4; word += 2) {
                const auto a = [&] { push_i64_value(inst.GetArg(0), word); };
                const auto b = [&] { push_i64_value(inst.GetArg(1), word); };
                a(); b(); op(Add64); set(scratch_local);
                if (signed_result) {
                    // a is unsigned, b signed; INT64_MAX-b fits in u64.
                    a(); constant64(code, INT64_MAX); b(); op(Sub64); op(GtU64); set(overflow);
                    constant64(code, INT64_MAX);
                } else {
                    // a is signed, b unsigned. A negative a can underflow;
                    // a nonnegative a can carry above UINT64_MAX.
                    b(); constant64(code, 0); a(); op(Sub64); op(LtU64);
                    get(scratch_local); b(); op(LtU64);
                    a(); constant64(code, 0); op(LtS64); op(Select); set(overflow);
                    constant64(code, 0); constant64(code, -1);
                    a(); constant64(code, 0); op(LtS64); op(Select);
                }
                get(scratch_local); get(overflow); op(Select); store_i64_words(next_local + word);
                get(saturated); get(overflow); op(Or); set(saturated);
            }
        }
        set_qc_if(saturated);
        return ok;
    }

    bool saturated_doubling_multiply(const Inst &inst, unsigned bits, bool rounding, bool widen, bool scalar = false) {
        const auto saturated = next_local + 4, overflow = next_local + 5;
        imm(0); set(saturated);
        const auto lane = [&](unsigned i) {
            vector_lane(inst.GetArg(0), bits, i, true); op(ExtendS);
            vector_lane(inst.GetArg(1), bits, i, true); op(ExtendS); op(Mul64);
            if (widen && bits == 32) {
                set(scratch_local);
                get(scratch_local); constant64(code, INT64_C(1) << 62); op(Eq64); set(overflow);
                get(saturated); get(overflow); op(Or); set(saturated);
                constant64(code, INT64_MAX); get(scratch_local); constant64(code, 1); op(Shl64);
                get(overflow); op(Select);
            } else {
                if (widen) { constant64(code, 1); op(Shl64); }
                else {
                    if (rounding) { constant64(code, int64_t(1) << (bits - 2)); op(Add64); }
                    constant64(code, bits - 1); op(ShrS64);
                }
                set(scratch_local);
                const unsigned output_bits = widen ? bits * 2 : bits;
                clamp_i64_low(-(int64_t(1) << (output_bits - 1)), (int64_t(1) << (output_bits - 1)) - 1, saturated);
            }
        };
        if (widen && bits == 32) {
            for (unsigned i = 0; i < 2; ++i) { lane(i); store_i64_words(next_local + i * 2); }
        } else if (scalar) {
            lane(0); if (bits == 16) mask(0xffff); set(next_local);
        } else pack_lanes(widen ? bits * 2 : bits, 4, lane);
        set_qc_if(saturated);
        return ok;
    }

    bool vector_rounding_or_saturated_shift(const Inst &inst, unsigned bits, bool signed_, bool saturate, bool unsigned_output = false) {
        const auto saturated = next_local + 4, amount = next_local + 5, overflow = next_local + 6;
        if (unsigned_output && (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() >= bits)) return false;
        imm(0); set(saturated);
        const auto lane = [&](unsigned i) {
            const auto source = [&] {
                if (bits == 64) push_i64_value(inst.GetArg(0), i * 2);
                else { vector_lane(inst.GetArg(0), bits, i, signed_); op(signed_ ? ExtendS : ExtendU); }
            };
            const auto clamp = [&] {
                if (signed_ && !unsigned_output) {
                    constant64(code, bits == 64 ? INT64_MIN : -(int64_t(1) << (bits - 1)));
                    constant64(code, bits == 64 ? INT64_MAX : (int64_t(1) << (bits - 1)) - 1);
                    source(); constant64(code, 0); op(LtS64); op(Select);
                } else constant64(code, bits == 64 ? -1 : (int64_t(1) << bits) - 1);
            };
            if (unsigned_output) imm(inst.GetArg(1).GetU8());
            else {
                value_word(inst.GetArg(1), i * bits / 32);
                if ((i * bits) % 32) { imm((i * bits) % 32); op(ShrU); }
                imm(24); op(Shl); imm(24); op(ShrS);
            }
            set(amount);
            if (unsigned_output) {
                source(); constant64(code, 0); op(LtS64); begin_if();
                constant64(code, 0); set(scratch_local); imm(1); set(saturated); op(Else);
            }
            get(amount); imm(0); op(GeS); begin_if();
            get(amount); imm(bits); op(GeU); begin_if();
            if (saturate) {
                source(); op(Eqz64); op(Eqz); set(overflow);
                clamp(); constant64(code, 0); get(overflow); op(Select); set(scratch_local);
                get(saturated); get(overflow); op(Or); set(saturated);
            } else { constant64(code, 0); set(scratch_local); }
            op(Else);
            source(); get(amount); op(ExtendU); op(Shl64);
            if (bits < 64) {
                constant64(code, 64 - bits); op(Shl64); constant64(code, 64 - bits);
                op(signed_ && !unsigned_output ? ShrS64 : ShrU64);
            }
            set(scratch_local);
            if (saturate) {
                get(scratch_local); get(amount); op(ExtendU); op(signed_ && !unsigned_output ? ShrS64 : ShrU64);
                source(); op(Ne64); set(overflow);
                clamp(); get(scratch_local); get(overflow); op(Select); set(scratch_local);
                get(saturated); get(overflow); op(Or); set(saturated);
            }
            end_if();
            op(Else);
            imm(0); get(amount); op(Sub); set(amount);
            get(amount); imm(bits); op(GtU); begin_if();
            if (saturate && signed_) { source(); constant64(code, 63); op(ShrS64); }
            else constant64(code, 0);
            set(scratch_local);
            op(Else);
            get(amount); imm(64); op(Eq); begin_if();
            if (signed_) { source(); constant64(code, 63); op(ShrS64); }
            else constant64(code, 0);
            set(scratch_local);
            op(Else);
            source(); get(amount); op(ExtendU); op(signed_ ? ShrS64 : ShrU64); set(scratch_local);
            end_if();
            if (!saturate) {
                get(scratch_local); source(); get(amount); imm(1); op(Sub); op(ExtendU); op(ShrU64);
                constant64(code, 1); op(And64); op(Add64); set(scratch_local);
            }
            end_if(); end_if();
            if (unsigned_output) end_if();
            get(scratch_local);
        };
        if (bits == 64) {
            for (unsigned i = 0; i < 2; ++i) { lane(i); store_i64_words(next_local + i * 2); }
        } else pack_lanes(bits, 4, [&](unsigned i) { lane(i); op(Wrap); });
        if (saturate) set_qc_if(saturated);
        return ok;
    }

    bool vector_simple64(const Inst &inst, Op kind) {
        if (kind == Op::VectorSignExtend64) {
            for (unsigned word = 0; word < 2; ++word) {
                value_word(inst.GetArg(0), word); set(next_local + word);
            }
            value_word(inst.GetArg(0), 1); imm(31); op(ShrS); set(next_local + 2);
            get(next_local + 2); set(next_local + 3);
        } else if (kind == Op::VectorEqual128) {
            imm(0);
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word); value_word(inst.GetArg(1), word); op(Eq);
                if (word) op(And);
            }
            op(Sub); set(next_local);
            for (unsigned word = 1; word < 4; ++word) { get(next_local); set(next_local + word); }
        } else {
            for (unsigned word = 0; word < 4; word += 2) {
                push_i64_value(inst.GetArg(0), word); set(scratch_local);
                constant64(code, 0); get(scratch_local); op(Sub64);
                get(scratch_local);
                get(scratch_local); constant64(code, 0); op(LtS64); op(Select);
                store_i64_words(next_local + word);
            }
        }
        return ok;
    }

    bool vector_halving_sub(const Inst &inst, unsigned bits, bool signed_, bool rounding) {
        if (bits == 32) {
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word); op(signed_ ? ExtendS : ExtendU);
                value_word(inst.GetArg(1), word); op(signed_ ? ExtendS : ExtendU); op(Sub64);
                if (rounding) { constant64(code, 1); op(Add64); }
                constant64(code, 1); op(signed_ ? ShrS64 : ShrU64); op(Wrap); set(next_local + word);
            }
        } else {
            pack_lanes(bits, 4, [&](unsigned index) {
                vector_lane(inst.GetArg(0), bits, index, signed_);
                vector_lane(inst.GetArg(1), bits, index, signed_); op(Sub);
                if (rounding) { imm(1); op(Add); }
                imm(1); op(signed_ ? ShrS : ShrU);
            });
        }
        return ok;
    }

    bool vector_paired(const Inst &inst, unsigned bits, bool lower, unsigned mode, bool signed_) {
        const unsigned count = (lower ? 64 : 128) / bits;
        if (bits == 64) {
            if (lower || mode != 0) return false;
            for (unsigned i = 0; i < 2; ++i) {
                push_i64_value(inst.GetArg(i), 0); push_i64_value(inst.GetArg(i), 2);
                op(Add64); store_i64_words(next_local + 2 * i);
            }
            return ok;
        }
        pack_lanes(bits, lower ? 2 : 4, [&](unsigned i) {
            const auto source = inst.GetArg(i < count / 2 ? 0 : 1);
            const unsigned index = 2 * (i % (count / 2));
            vector_lane(source, bits, index, signed_); set(next_local + 4);
            vector_lane(source, bits, index + 1, signed_); set(next_local + 5);
            if (mode != 0) {
                get(next_local + 4); get(next_local + 5); op(signed_ ? GtS : GtU); set(next_local + 6);
                if (mode == 2) { get(next_local + 6); op(Eqz); set(next_local + 6); }
                get(next_local + 4); get(next_local + 5); get(next_local + 6); op(Select);
            } else { get(next_local + 4); get(next_local + 5); op(Add); }
        });
        if (lower) { imm(0); set(next_local + 2); imm(0); set(next_local + 3); }
        return ok;
    }

    bool vector_count_leading_zeros(const Inst &inst, unsigned bits) {
        pack_lanes(bits, 4, [&](unsigned i) {
            vector_element_word(inst.GetArg(0), bits, i); op(Clz); imm(32 - bits); op(Sub);
        });
        return ok;
    }

    bool fp_extended(const Inst &inst, FPOperation operation, unsigned bits, unsigned result_bits = 0, bool unsigned_ = false) {
        if (!result_bits) result_bits = bits;
        if (operation == FPOperation::Convert || operation == FPOperation::RoundInt) {
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() > (operation == FPOperation::Convert ? 5 : 4)) return false;
        }
        if (operation == FPOperation::FromFixed || operation == FPOperation::ToFixed) {
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() > (operation == FPOperation::FromFixed ? bits : result_bits)
                || !inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetU8() > (operation == FPOperation::FromFixed ? 5 : 4)) return false;
        }
        load(offsetof(JitState, fpscr)); mask(0x00009f00u);
        begin_if(); ret(ExitReason::Unsupported); end_if();
        fp_extended_call(fp_operation(operation, bits, result_bits, unsigned_), [&](unsigned i) {
            if (i >= inst.NumArgs()) constant64(code, 0);
            else if (inst.GetArg(i).GetType() == Type::U64) value64(inst.GetArg(i));
            else { value_word(inst.GetArg(i)); op(ExtendU); }
        }, true);
        if (result_bits == 64) store_i64_words(next_local);
        else { op(Wrap); set(next_local); }
        return ok;
    }

    // Operand emitter pushes i64 bit patterns; the returned i64 stays on the
    // Wasm stack. This works inside pack_lanes without overwriting SSA words.
    template <typename Operand>
    void fp_extended_call(uint32_t selector, const Operand &operand, bool controlled) {
        for (unsigned i = 0; i < 6; ++i) {
            load(offsetof(JitState, fp_arguments) + 4 * i); set(next_local + 4 + i);
        }
        for (unsigned i = 0; i < 3; ++i) {
            operand(i); set(scratch_local);
            get(0); get(scratch_local); op(Wrap); store(offsetof(JitState, fp_arguments) + 8 * i);
            get(0); get(scratch_local); constant64(code, 32); op(ShrU64); op(Wrap);
            store(offsetof(JitState, fp_arguments) + 8 * i + 4);
        }
        get(0); load(offsetof(JitState, fpscr));
        get(0); imm(selector); load(offsetof(JitState, fpscr));
        if (!controlled) { mask(0x04080000); imm(0x03000000); op(Or); }
        op(Call); uleb(code, 2); op(Or); store(offsetof(JitState, fpscr));
        load(offsetof(JitState, fp_arguments)); op(ExtendU);
        load(offsetof(JitState, fp_arguments) + 4); op(ExtendU); constant64(code, 32); op(Shl64); op(Or64); set(scratch_local);
        for (unsigned i = 0; i < 6; ++i) {
            get(0); get(next_local + 4 + i); store(offsetof(JitState, fp_arguments) + 4 * i);
        }
        get(scratch_local);
    }

    bool fp_vector_extended(const Inst &inst, FPOperation operation, unsigned bits,
        unsigned result_bits = 0, bool unsigned_ = false, bool paired = false, bool lower = false) {
        if (!result_bits) result_bits = bits;
        const auto control = inst.GetArg(inst.NumArgs() - 1);
        if (!control.IsImmediate() || control.GetType() != Type::U1) return false;
        const bool controlled = control.GetU1();
        if (operation == FPOperation::Convert || operation == FPOperation::RoundInt) {
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() > (operation == FPOperation::Convert ? 5 : 4)) return false;
        }
        if (operation == FPOperation::FromFixed || operation == FPOperation::ToFixed) {
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() > (operation == FPOperation::FromFixed ? bits : result_bits)
                || !inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetU8() > (operation == FPOperation::FromFixed ? 5 : 4)) return false;
        }
        if (controlled) {
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
        }
        const unsigned count = (lower ? 64u : 128u) / std::max(bits, result_bits);
        const auto lane = [&](unsigned index) {
            fp_extended_call(fp_operation(operation, bits, result_bits, unsigned_), [&](unsigned operand) {
                if (operand >= inst.NumArgs() - 1) { constant64(code, 0); return; }
                auto source = inst.GetArg(operand);
                unsigned source_lane = index;
                if (paired && operand < 2) {
                    if (lower && bits == 64) source_lane = 0;
                    else {
                        source = inst.GetArg(index / (count / 2));
                        source_lane = (index % (count / 2)) * 2 + operand;
                    }
                }
                if (source.GetType() == Type::U128) {
                    if (bits == 64) push_i64_value(source, source_lane * 2);
                    else { vector_element_word(source, bits, source_lane); op(ExtendU); }
                } else { value_word(source); op(ExtendU); }
            }, controlled);
        };
        if (result_bits == 64) {
            for (unsigned i = 0; i < count; ++i) { lane(i); store_i64_words(next_local + i * 2); }
            if (lower) { imm(0); set(next_local + 2); imm(0); set(next_local + 3); }
        } else {
            pack_lanes(result_bits, count * result_bits / 32, [&](unsigned i) { lane(i); op(Wrap); });
        }
        return ok;
    }

    bool fp_fixed_to_float(const Inst &inst, unsigned source_bits, unsigned target_bits, bool signed_) {
        if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() > source_bits
            || !inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetU8() > 5) return false;
        if (source_bits == 64 || (source_bits == 32 && target_bits == 32))
            return fp_extended(inst, FPOperation::FromFixed, source_bits, target_bits, !signed_);
        // These integers fit exactly in the destination significand. Scaling
        // by 2^-fbits stays normal and exact, independent of rounding mode.
        value_word(inst.GetArg(0));
        if (source_bits == 16) {
            mask(0xffff);
            if (signed_) { imm(16); op(Shl); imm(16); op(ShrS); }
        }
        const unsigned fraction = inst.GetArg(1).GetU8();
        if (target_bits == 32) {
            op(signed_ ? 0xb2 : 0xb3);
            if (fraction) { imm((127 - fraction) << 23); op(0xbe); op(0x94); }
            op(0xbc); set(next_local);
        } else {
            op(signed_ ? 0xb7 : 0xb8);
            if (fraction) { constant64(code, uint64_t(1023 - fraction) << 52); op(0xbf); op(0xa2); }
            store_f64_words(next_local);
        }
        return ok;
    }
    // Pushes the i64 formed by words `word` and `word + 1` of a value.
    void push_i64_value(const Value &v, unsigned word) {
        value_word(v, word); op(ExtendU);
        value_word(v, word + 1); op(ExtendU); constant64(code, 32); op(Shl64); op(Or64);
    }
    // Sets the sticky FPSCR.QC (bit 27) when the i32 in `slot` is nonzero,
    // like Dynarmic's saturating vector emitters.
    void set_qc_if(uint32_t slot) {
        get(slot);
        begin_if();
        get(0); load(offsetof(JitState, fpscr)); imm(0x08000000u); op(Or); store(offsetof(JitState, fpscr));
        end_if();
    }
    // Pushes clamp(value in `slot`, lo, hi) for i32 lanes and ORs "clamped"
    // into `saturated`. Signed comparisons: every caller's value is exact in
    // i32 (sign- or zero-extended lanes below 32 bits).
    void clamp_i32(uint32_t slot, int32_t lo, int32_t hi, uint32_t saturated) {
        get(saturated); get(slot); imm(uint32_t(lo)); op(LtS); get(slot); imm(uint32_t(hi)); op(GtS); op(Or); op(Or);
        set(saturated);
        imm(uint32_t(lo)); imm(uint32_t(hi)); get(slot); get(slot); imm(uint32_t(hi)); op(GtS); op(Select);
        get(slot); imm(uint32_t(lo)); op(LtS); op(Select);
    }
    // The same for the i64 in scratch_local, pushing the low 32 bits.
    void clamp_i64_low(int64_t lo, int64_t hi, uint32_t saturated) {
        get(saturated); get(scratch_local); constant64(code, lo); op(LtS64);
        get(scratch_local); constant64(code, hi); op(GtS64); op(Or); op(Or); set(saturated);
        imm(uint32_t(lo)); imm(uint32_t(hi)); get(scratch_local); op(Wrap);
        get(scratch_local); constant64(code, hi); op(GtS64); op(Select);
        get(scratch_local); constant64(code, lo); op(LtS64); op(Select);
    }
    // VQMOVN.S/VQMOVUN (and the narrow of VQ(R)SHR(U)N.S): each signed
    // `2 * bits` lane clamped to the signed (or unsigned) `bits` range,
    // packed into the low 64 bits (upper 64 bits zero). Any clamp sets
    // FPSCR.QC (Dynarmic EmitVectorSignedSaturatedNarrowTo{Signed,Unsigned}).
    bool vector_signed_saturated_narrow(const Inst &inst, unsigned bits, bool to_unsigned) {
        const auto saturated = next_local + 4, wide = next_local + 5;
        imm(0); set(saturated);
        if (bits == 32) {
            for (unsigned lane = 0; lane < 2; ++lane) {
                push_i64_value(inst.GetArg(0), 2 * lane); set(scratch_local);
                clamp_i64_low(to_unsigned ? 0 : INT32_MIN, to_unsigned ? int64_t(UINT32_MAX) : INT32_MAX, saturated);
                set(next_local + lane);
            }
            imm(0); set(next_local + 2); imm(0); set(next_local + 3);
        } else {
            const int32_t lo = to_unsigned ? 0 : -(1 << (bits - 1));
            const int32_t hi = to_unsigned ? (1 << bits) - 1 : (1 << (bits - 1)) - 1;
            pack_lanes(bits, 2, [&](unsigned i) {
                vector_lane(inst.GetArg(0), 2 * bits, i, true); set(wide);
                clamp_i32(wide, lo, hi, saturated);
            });
        }
        set_qc_if(saturated);
        return ok;
    }
    // VQADD/VQSUB .S/.U: per-lane saturating add or subtract; any clamp
    // sets FPSCR.QC (Dynarmic Emit{Signed,Unsigned}SaturatedOp).
    bool vector_saturated_add_sub(const Inst &inst, unsigned bits, bool is_signed, bool is_add, bool scalar = false) {
        const auto saturated = next_local + 4, sum = next_local + 5, overflow = next_local + 6;
        imm(0); set(saturated);
        if (bits < 32) {
            const int32_t lo = is_signed ? -(1 << (bits - 1)) : 0;
            const int32_t hi = is_signed ? (1 << (bits - 1)) - 1 : (1 << bits) - 1;
            const auto lane = [&](unsigned i) {
                vector_lane(inst.GetArg(0), bits, i, is_signed);
                vector_lane(inst.GetArg(1), bits, i, is_signed);
                op(is_add ? Add : Sub); set(sum);
                clamp_i32(sum, lo, hi, saturated);
            };
            if (scalar) { lane(0); mask((1u << bits) - 1); set(next_local); }
            else pack_lanes(bits, 4, lane);
        } else if (bits == 32) {
            const uint8_t extend = is_signed ? ExtendS : ExtendU;
            for (unsigned word = 0; word < (scalar ? 1u : 4u); ++word) {
                value_word(inst.GetArg(0), word); op(extend);
                value_word(inst.GetArg(1), word); op(extend);
                op(is_add ? Add64 : Sub64); set(scratch_local);
                clamp_i64_low(is_signed ? INT32_MIN : 0, is_signed ? INT32_MAX : int64_t(UINT32_MAX), saturated);
                set(next_local + word);
            }
        } else {
            for (unsigned word = 0; word < (scalar ? 2u : 4u); word += 2) {
                push_i64_value(inst.GetArg(0), word); push_i64_value(inst.GetArg(1), word);
                op(is_add ? Add64 : Sub64); set(scratch_local);
                if (is_signed) {
                    // Signed overflow: the result's sign differs from a's
                    // and from b's (add) or a's and b's differ (sub); the
                    // clamp takes a's sign.
                    push_i64_value(inst.GetArg(0), word); get(scratch_local); op(Xor64);
                    if (is_add) { push_i64_value(inst.GetArg(1), word); get(scratch_local); op(Xor64); }
                    else { push_i64_value(inst.GetArg(0), word); push_i64_value(inst.GetArg(1), word); op(Xor64); }
                    op(And64); constant64(code, 0); op(LtS64); set(overflow);
                    constant64(code, INT64_MIN); constant64(code, INT64_MAX);
                    value_word(inst.GetArg(0), word + 1); imm(31); op(ShrU); op(Select);
                } else {
                    // Unsigned: a carry out (sum below a) or a borrow (a below b).
                    if (is_add) { get(scratch_local); push_i64_value(inst.GetArg(0), word); }
                    else { push_i64_value(inst.GetArg(0), word); push_i64_value(inst.GetArg(1), word); }
                    op(LtU64); set(overflow);
                    constant64(code, is_add ? -1 : 0);
                }
                get(scratch_local); get(overflow); op(Select);
                store_i64_words(next_local + word);
                get(saturated); get(overflow); op(Or); set(saturated);
            }
        }
        set_qc_if(saturated);
        return ok;
    }
    // VQABS/VQNEG: the most negative lane saturates to the most positive
    // one and sets FPSCR.QC; other lanes take their absolute value or
    // negation (Dynarmic EmitVectorSignedSaturated{Abs,Neg}).
    bool vector_saturated_abs_neg(const Inst &inst, unsigned bits, bool negate) {
        const auto saturated = next_local + 4, lane = next_local + 5, minimum = next_local + 6;
        imm(0); set(saturated);
        if (bits == 64) {
            for (unsigned word = 0; word < 4; word += 2) {
                push_i64_value(inst.GetArg(0), word); set(scratch_local);
                get(scratch_local); constant64(code, INT64_MIN); op(Eq64); set(minimum);
                get(saturated); get(minimum); op(Or); set(saturated);
                constant64(code, INT64_MAX);
                constant64(code, 0); get(scratch_local); op(Sub64);
                if (!negate) { get(scratch_local); get(scratch_local); constant64(code, 0); op(LtS64); op(Select); }
                get(minimum); op(Select);
                store_i64_words(next_local + word);
            }
        } else {
            const int32_t min = bits == 32 ? INT32_MIN : -(1 << (bits - 1));
            const int32_t max = bits == 32 ? INT32_MAX : (1 << (bits - 1)) - 1;
            pack_lanes(bits, 4, [&](unsigned i) {
                vector_lane(inst.GetArg(0), bits, i, true); set(lane);
                get(lane); imm(uint32_t(min)); op(Eq); set(minimum);
                get(saturated); get(minimum); op(Or); set(saturated);
                imm(uint32_t(max));
                imm(0); get(lane); op(Sub);
                if (!negate) { get(lane); get(lane); imm(0); op(LtS); op(Select); }
                get(minimum); op(Select);
            });
        }
        set_qc_if(saturated);
        return ok;
    }
    // VSHL.U (register): shift each lane of a by the signed low byte s of the
    // matching lane of b: s >= 0 shifts left, s < 0 shifts right logically
    // by -s, and |s| >= bits gives 0 (Dynarmic VShift<unsigned>).
    bool vector_logical_vshift(const Inst &inst, unsigned bits) {
        const auto amount = next_local + 4, lane = next_local + 5;
        const auto signed_amount = [&](unsigned word) {
            value_word(inst.GetArg(1), word); imm(24); op(Shl); imm(24); op(ShrS); set(amount);
        };
        // in range: -bits < s < bits, i.e. s + bits - 1 below 2 * bits - 1
        const auto in_range = [&] { get(amount); imm(bits - 1); op(Add); imm(2 * bits - 1); op(LtU); };
        if (bits == 64) {
            for (unsigned word = 0; word < 4; word += 2) {
                signed_amount(word);
                const auto a64 = [&] {
                    value_word(inst.GetArg(0), word); op(ExtendU);
                    value_word(inst.GetArg(0), word + 1); op(ExtendU); constant64(code, 32); op(Shl64); op(Or64);
                };
                a64(); get(amount); op(ExtendU); op(Shl64);
                a64(); imm(0); get(amount); op(Sub); op(ExtendU); op(ShrU64);
                get(amount); imm(0); op(GeS); op(Select);
                constant64(code, 0);
                in_range(); op(Select);
                store_i64_words(next_local + word);
            }
            return ok;
        }
        pack_lanes(bits, 4, [&](unsigned i) {
            // The amount byte is the lowest byte of b's lane i.
            const unsigned position = bits * i;
            value_word(inst.GetArg(1), position / 32);
            if (position % 32) { imm(position % 32); op(ShrU); }
            imm(24); op(Shl); imm(24); op(ShrS); set(amount);
            vector_element_word(inst.GetArg(0), bits, i); set(lane);
            get(lane); get(amount); op(Shl);
            get(lane); imm(0); get(amount); op(Sub); op(ShrU);
            get(amount); imm(0); op(GeS); op(Select);
            imm(0);
            in_range(); op(Select);
        });
        return ok;
    }
    // VMULL.S/U (and VMLAL/VMLSL's product): the low 64 bits' lanes widened
    // to twice their width and multiplied (Dynarmic's polyfill for
    // VectorMultiply{Signed,Unsigned}Widen: extend, then VectorMultiply).
    bool vector_multiply_widen(const Inst &inst, unsigned bits, bool is_signed) {
        if (bits == 32) {
            const uint8_t extend = is_signed ? 0xac : ExtendU;
            for (unsigned lane = 0; lane < 2; ++lane) {
                value_word(inst.GetArg(0), lane); op(extend);
                value_word(inst.GetArg(1), lane); op(extend);
                op(Mul64);
                store_i64_words(next_local + 2 * lane);
            }
            return ok;
        }
        pack_lanes(2 * bits, 4, [&](unsigned i) {
            vector_lane(inst.GetArg(0), bits, i, is_signed);
            vector_lane(inst.GetArg(1), bits, i, is_signed);
            op(Mul);
        });
        return ok;
    }
    // VMULL.P8: carry-less product of the low 8 byte lanes into 16-bit lanes.
    // Two lanes per i32 word: each partial product stays below bit 15 of its
    // half, so the halves never interact.
    void polynomial_multiply_long8(const Inst &inst) {
        const auto a = next_local + 4, b = next_local + 5, acc = next_local + 6;
        const auto spread = [&](const Value &source, unsigned word, uint32_t slot) {
            const unsigned shift = (word % 2) * 16;
            value_word(source, word / 2); if (shift) { imm(shift); op(ShrU); } mask(0xff);
            value_word(source, word / 2); imm(shift + 8); op(ShrU); mask(0xff); imm(16); op(Shl);
            op(Or); set(slot);
        };
        for (unsigned word = 0; word < 4; ++word) {
            spread(inst.GetArg(0), word, a);
            spread(inst.GetArg(1), word, b);
            imm(0); set(acc);
            for (unsigned k = 0; k < 8; ++k) {
                get(acc);
                get(a); if (k) { imm(k); op(Shl); }
                get(b); if (k) { imm(k); op(ShrU); } mask(0x00010001u); imm(0xffff); op(Mul);
                op(And); op(Xor); set(acc);
            }
            get(acc); set(next_local + word);
        }
    }

    // VQMOVN.U: each unsigned `2 * bits` lane clamped to the unsigned
    // `bits` range, packed into the low 64 bits (upper 64 bits zero).
    // Saturating any lane sets the sticky FPSCR.QC (bit 27), like Dynarmic's
    // EmitVectorUnsignedSaturatedNarrow*.
    bool vector_unsigned_saturated_narrow(const Inst &inst, unsigned bits) {
        const auto saturated = next_local + 4, wide = next_local + 5;
        imm(0); set(saturated);
        if (bits == 32) {
            for (unsigned lane = 0; lane < 2; ++lane) {
                value_word(inst.GetArg(0), 2 * lane + 1); op(Eqz); op(Eqz); set(wide);
                get(saturated); get(wide); op(Or); set(saturated);
                imm(UINT32_MAX); value_word(inst.GetArg(0), 2 * lane); get(wide); op(Select);
                set(next_local + lane);
            }
            imm(0); set(next_local + 2); imm(0); set(next_local + 3);
        } else {
            const uint32_t max = (1u << bits) - 1;
            pack_lanes(bits, 2, [&](unsigned i) {
                vector_element_word(inst.GetArg(0), 2 * bits, i); set(wide);
                get(saturated); get(wide); imm(max); op(GtU); op(Or); set(saturated);
                imm(max); get(wide); get(wide); imm(max); op(GtU); op(Select);
            });
        }
        get(saturated);
        begin_if();
        get(0); load(offsetof(JitState, fpscr)); imm(0x08000000u); op(Or); store(offsetof(JitState, fpscr));
        end_if();
        return ok;
    }
    // VMUL.P8: the low byte of each carry-less byte product, four lanes per
    // word; the shift masks drop the bits a left shift moves into the next
    // byte.
    void polynomial_multiply8(const Inst &inst) {
        const auto a = next_local + 4, b = next_local + 5, acc = next_local + 6;
        for (unsigned word = 0; word < 4; ++word) {
            value_word(inst.GetArg(0), word); set(a);
            value_word(inst.GetArg(1), word); set(b);
            imm(0); set(acc);
            for (unsigned k = 0; k < 8; ++k) {
                get(acc);
                get(a); if (k) { imm(k); op(Shl); } mask(0x01010101u * ((0xffu << k) & 0xffu));
                get(b); if (k) { imm(k); op(ShrU); } mask(0x01010101u); imm(0xff); op(Mul);
                op(And); op(Xor); set(acc);
            }
            get(acc); set(next_local + word);
        }
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
    // block's end-of-body bits AND the live mode already holds them, the
    // mode RMW is idempotent and skipped. The live mode holds the entry bits
    // until the body writes the end bits (UpdateUpperLocationDescriptor,
    // BXWritePC): a block inside an IT block enters with IT != 0 and ends
    // with the advanced IT, so its entry bits must not be assumed to be the
    // end bits. The PC write always stays: edge Budget/Stop/Smc exits
    // publish next_pc from regs[15], and the next block consumes it.
    // Non-member (Miss) paths keep the full location() write.
    void location_member(const Location &loc) {
        constexpr uint32_t mask = Location::CPSR_MODE_MASK;
        const bool live_is_finish = end_mode_written
            || (start.CPSR().Value() & mask) == (finish.CPSR().Value() & mask);
        if (live_is_finish && (loc.CPSR().Value() & mask) == (finish.CPSR().Value() & mask)) {
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
            if (svc && hot_nid) {
                get(svc_inline_success_local); begin_if();
                ret(ExitReason::Continue);
                end_if();
            }
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
    // GetCarry/GetOverflow/GetNZCV/GetGE pseudo-ops read them (GetGE only
    // from PackedAddU8). The scan runs once
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
        if (need_carry) {
            sum();
            set(scratch_local);
            get(scratch_local); op(Wrap); set(next_local);
            get(scratch_local); op(0x42); op(32); op(ShrU64); op(Wrap); set(next_local + 4);
        } else if (const auto &carry_in = inst.GetArg(2); carry_in.IsImmediate()) {
            // No carry consumer: plain i32 arithmetic (mod 2^32 is exact).
            // a + ~b + c == a - b - 1 + c.
            value(inst.GetArg(0)); value(inst.GetArg(1));
            op(sub ? Sub : Add);
            if (sub != carry_in.GetU1()) { imm(1); op(sub ? Sub : Add); }
            set(next_local);
        } else {
            value(inst.GetArg(0)); value(inst.GetArg(1));
            if (sub) { imm(0xffffffff); op(Xor); }
            op(Add); value(carry_in); op(Add);
            set(next_local);
        }
        if (!need_overflow) return;
        // V = (~(a ^ b) & (a ^ result)) >> 31 for add; for subtract
        // use (a ^ b) instead. This also accounts for carry/borrow input.
        value(inst.GetArg(0)); value(inst.GetArg(1)); op(Xor);
        if (!sub) { imm(0xffffffff); op(Xor); }
        value(inst.GetArg(0)); get(next_local); op(Xor); op(And);
        imm(31); op(ShrU); set(next_local + 5);
    }

    // Constant-count shift body. Returns false for ROR #0 (RRX encoding;
    // the generic path implements n==0 as passthrough) so the caller falls
    // back. Every other (kind, k) lowers straight-line; carry exact per
    // the generic arms (verified class by class).
    bool shift_imm(const Inst &inst, Op kind, const Value &a, const Value &carry_in, uint32_t k) {
        scan_flag_consumers(); // idempotent; instruction() also scans first
        const bool need_carry = carry_needed.count(&inst) != 0;
        const auto carry_bit = [&](uint32_t bit) {
            value(a); imm(bit); op(ShrU); mask(1); set(next_local + 4);
        };
        if (kind == Op::RotateRight32) {
            const uint32_t s = k & 31;
            if (k == 0) return false;
            if (s == 0) {
                value(a); set(next_local);
                if (need_carry) carry_bit(31);
            } else {
                value(a); imm(s); op(RotR); set(next_local);
                if (need_carry) carry_bit(s - 1);
            }
            return true;
        }
        if (k == 0) {
            value(a); set(next_local);
            if (need_carry) { value(carry_in); set(next_local + 4); }
            return true;
        }
        if (kind == Op::LogicalShiftLeft32) {
            if (k < 32) {
                value(a); imm(k); op(Shl); set(next_local);
                if (need_carry) carry_bit(32 - k);
            } else {
                imm(0); set(next_local);
                if (need_carry) {
                    if (k == 32) { value(a); mask(1); set(next_local + 4); }
                    else { imm(0); set(next_local + 4); }
                }
            }
            return true;
        }
        if (kind == Op::LogicalShiftRight32) {
            if (k < 32) {
                value(a); imm(k); op(ShrU); set(next_local);
                if (need_carry) carry_bit(k - 1);
            } else {
                imm(0); set(next_local);
                if (need_carry) {
                    if (k == 32) { value(a); imm(31); op(ShrU); set(next_local + 4); }
                    else { imm(0); set(next_local + 4); }
                }
            }
            return true;
        }
        if (kind == Op::ArithmeticShiftRight32) {
            if (k < 32) {
                value(a); imm(k); op(ShrS); set(next_local);
                if (need_carry) carry_bit(k - 1);
            } else {
                value(a); imm(31); op(ShrS); set(next_local);
                if (need_carry) { value(a); imm(31); op(ShrU); set(next_local + 4); }
            }
            return true;
        }
        return false;
    }
    void shifted(const Inst &inst) {
        const auto a = inst.GetArg(0);
        const auto n = inst.GetArg(1);
        const auto carry = inst.GetArg(inst.GetOpcode() == Op::RotateRightExtended ? 1 : 2);
        const Op kind = inst.GetOpcode();
        // Constant-count specialization (R3h): the generic path spends two
        // runtime branches (zero check, <32 check) plus branchy >32 arms on
        // a static outcome. Straight-line per class below; result and carry
        // verified arm-for-arm against the generic sequences. RRX keeps its
        // branchless generic path; ROR #0 (RRX encoding) never reaches here
        // as ROR from the frontend and falls back defensively.
        if (kind != Op::RotateRightExtended && n.IsImmediate() && n.GetType() == Type::U8) {
            const uint64_t raw = n.GetImmediateAsU64();
            if (raw <= 255 && shift_imm(inst, kind, a, carry, static_cast<uint32_t>(raw)))
                return;
        }
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
                switch (producer->GetOpcode()) {
                case Op::PackedAddU8:
                case Op::PackedAddS8:
                case Op::PackedSubU8:
                case Op::PackedSubS8:
                case Op::PackedAddU16:
                case Op::PackedAddS16:
                case Op::PackedSubU16:
                case Op::PackedSubS16:
                case Op::PackedAddSubU16:
                case Op::PackedAddSubS16:
                case Op::PackedSubAddU16:
                case Op::PackedSubAddS16:
                    break;
                default: return false;
                }
                get(it->second + 4);
                break;
            case Op::GetCarryFromOp:
                if (!arith && !shift(producer->GetOpcode()) && producer->GetOpcode() != Op::MostSignificantWord) return false;
                get(it->second + 4);
                break;
            case Op::GetOverflowFromOp:
                if (!arith && !saturation(producer->GetOpcode())) return false;
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
        if (state.aot()) {
            // Only backward edges can close a loop inside one call (members
            // are sorted by PC), so only they pay the budget check; forward
            // chains are bounded by the function's total ticks. A backward
            // body is enclosed by its target's label: branch straight to it.
            // ABLATE C does not apply: without this check a loop that never
            // calls out cannot stop, so build_aot refuses to run under it.
            if (target_index <= body_index) {
                state.read_executed(code); imm(entry_ticks((*metadata)[target_index])); op(Add);
                get(1); op(GtU);
                begin_if();
                set_next_pc_runtime();
                ret(ExitReason::Budget);
                end_if();
            }
            if (target_index < body_index) {
                op(Br); uleb(code, (body_index - 1 - target_index) + extra_labels);
                return;
            }
            imm(target_index); set(6);
            br_redispatch();
            return;
        }
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
        if (state.aot()) {
            // AOT: no stop/SMC polls (AOT text is read-execute, host stops
            // arrive between calls) and the block's budget was paid in full.
            completed_store_ticks = point.completed_ticks;
            return;
        }
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
            if (svc && hot_nid) {
                get(svc_inline_success_local); begin_if();
                // Keep the real stub return instruction (including ARM/Thumb
                // interworking) and its normal tick/budget accounting.
                if (const auto idx = member_index(finish); idx != kNoMember) {
                    location_member(finish);
                    light_redispatch(idx);
                } else {
                    set_next_pc_runtime();
                    ret(ExitReason::Miss);
                }
                end_if();
            }
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
    // Float-to-integer and float-to-fixed conversion (VCVT.S32/U32.F32/F64
    // with fbits 0..32 fraction bits, VCVTR and VCVT{A,N,P,M}). Pure
    // bit-pattern integer lowering: no host FP and no trapping Wasm
    // conversion. Results and cumulative flags are the ARM ARM FPToFixed
    // (round, then saturate), which the Vita's Cortex-A9 implements; Dynarmic's
    // FPToFixed differs for a negative signed binary64 value that rounds
    // towards zero into range from just below -(2^31 - 1) and for a negative
    // value that rounds to an unsigned 0 (tests/arm_fp_to_fixed.h).
    // Scaling by 2^fbits only adds fbits to the exponent before the range
    // decisions.
    // ibits is 32, or 16 for VCVT.{S,U}16 (a U16 result in the low half).
    bool fp_to_fixed(const Inst &inst, bool is_signed, bool is_double, uint32_t ibits = 32) {
        if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() > ibits) return false;
        const uint32_t fbits = inst.GetArg(1).GetU8();
        if (!inst.GetArg(2).IsImmediate()) return false;
        // Dynarmic::FP::RoundingMode: 0 nearest-even, 1 towards +inf,
        // 2 towards -inf, 3 towards zero, 4 nearest-away. ToOdd (5) is not
        // an FPToFixed mode.
        const uint8_t rounding = inst.GetArg(2).GetU8();
        if (rounding > 4) return false;
        if (inst.GetArg(0).GetType() != (is_double ? Type::U64 : Type::U32)) return false;
        // Exception enables are live state, not part of the location key.
        load(offsetof(JitState, fpscr)); mask(0x00009f00u);
        begin_if(); ret(ExitReason::Unsupported); end_if();
        // Emission-time scratch assignment. The sign slot is shared; the
        // magnitude slot holds the rounded i32 magnitude for the tail.
        uint32_t sign_slot = 0, flags_slot = 0, mag_slot = 0;
        const auto accumulate = [&](uint32_t bit) {
            get(flags_slot); imm(bit); op(Or); set(flags_slot);
        };
        // Pushes the saturated result for a NaN/infinity/overflow input:
        // the signed extrema by sign, or unsigned max/zero by sign.
        const uint32_t signed_max = (1u << (ibits - 1)) - 1, unsigned_max = ibits == 32 ? 0xffffffffu : 0xffffu;
        const auto saturate = [&] {
            if (is_signed) { imm(~signed_max); imm(signed_max); }
            else { imm(0); imm(unsigned_max); }
            get(sign_slot); op(Select);
        };
        // Result for a nonzero magnitude below one unit (IXC is raised by
        // the caller): 1 or -1 when the directed mode rounds away from zero,
        // otherwise 0 (below 2^-8, the nearest modes never reach one half).
        // A negative unsigned input never gets here.
        // A nonzero finite negative input to an unsigned conversion: 0 with
        // IXC when it rounds to 0, else saturated to 0 with IOC. Its
        // magnitude rounds to 0 below one (towards zero or +inf), below one
        // half (nearest, ties away) or up to one half (nearest even), never
        // towards -inf. `exponent` holds the biased exponent before scaling.
        const auto negative_unsigned = [&](uint32_t exponent, uint32_t bias, const auto &fraction_zero) {
            switch (rounding) {
            case 1: case 3: get(exponent); imm(bias - fbits); op(LtU); break;
            case 4: get(exponent); imm(bias - 1 - fbits); op(LtU); break;
            case 0:
                get(exponent); imm(bias - 1 - fbits); op(LtU);
                get(exponent); imm(bias - 1 - fbits); op(Eq); fraction_zero(); op(And); op(Or);
                break;
            default: imm(0); break;
            }
            begin_if(); accumulate(0x10); op(Else); accumulate(1); end_if();
            imm(0); set(next_local);
        };
        const auto tiny = [&] {
            if (rounding == 1) { get(sign_slot); op(Eqz); }
            else if (rounding == 2 && is_signed) { imm(0); get(sign_slot); op(Sub); }
            else imm(0);
        };
        // Pushes the magnitude increment (0/1) for the selected mode from
        // the dropped fraction: top = its half bit, below = any lower bit,
        // any = top | below, odd = the kept magnitude's low bit.
        const auto increment = [&](const auto &top, const auto &below, const auto &any) {
            switch (rounding) {
            case 0: top(); below(); get(mag_slot); imm(1); op(And); op(Or); op(And); break;
            case 1: any(); get(sign_slot); op(Eqz); op(And); break;
            case 2: any(); get(sign_slot); op(And); break;
            case 4: top(); break;
            default: imm(0); break;
            }
        };
        // Overflow tail shared by every magnitude path. A rounded 2^31 stays
        // exact for a negative signed result only; anything larger (or a
        // carry out of the 32-bit magnitude) saturates with IOC and never
        // additionally IXC, which the dropped fraction has already raised.
        const auto publish_mag = [&](uint32_t carry_slot) {
            if (is_signed) {
                get(mag_slot); imm(signed_max + 1); imm(signed_max); get(sign_slot); op(Select); op(GtU);
            } else if (ibits == 32) {
                imm(0);
            } else {
                get(mag_slot); imm(unsigned_max); op(GtU);
            }
            if (carry_slot) { get(carry_slot); op(Or); }
            begin_if();
            get(flags_slot); mask(~0x10u); imm(1); op(Or); set(flags_slot);
            saturate(); set(next_local);
            op(Else);
            if (is_signed) { imm(0); get(mag_slot); op(Sub); get(mag_slot); get(sign_slot); op(Select); }
            else get(mag_slot);
            set(next_local);
            end_if();
        };
        if (!is_double) {
            const auto bits = next_local + 1, expr = next_local + 3, frac = next_local + 4;
            const auto count = next_local + 7, trunc = next_local + 8, half = next_local + 9;
            sign_slot = next_local + 2; flags_slot = next_local + 5; mag_slot = next_local + 6;
            value_word(inst.GetArg(0)); set(bits);
            get(bits); imm(31); op(ShrU); set(sign_slot);
            get(bits); imm(23); op(ShrU); mask(0xff); set(expr);
            get(bits); mask(0x7fffff); set(frac);
            imm(0); set(flags_slot);
            // Whole magnitudes of 2^23 and above shift left exactly; the
            // remaining range shifts right with at most 31 dropped bits.
            const auto range = [&] {
                // From here on expr is the exponent of value * 2^fbits.
                if (fbits) { get(expr); imm(fbits); op(Add); set(expr); }
                // Magnitudes below 2^-8, including non-FZ denormals even
                // scaled by 2^32, are tiny.
                get(expr); imm(118); op(LeU);
                begin_if(); accumulate(0x10); tiny(); set(next_local);
                op(Else);
                // Overflow: magnitudes of 2^32 and above exceed every range.
                get(expr); imm(159); op(GeU);
                begin_if(); accumulate(1); saturate(); set(next_local);
                op(Else);
                get(expr); imm(150); op(GeU);
                begin_if();
                get(expr); imm(150); op(Sub); set(count);
                get(frac); imm(0x800000); op(Or); set(frac);
                get(frac); get(count); op(Shl); set(mag_slot);
                publish_mag(0);
                op(Else);
                imm(150); get(expr); op(Sub); set(count);
                get(frac); imm(0x800000); op(Or); set(frac);
                get(frac); get(count); op(ShrU); set(mag_slot);
                imm(1); get(count); op(Shl); imm(1); op(Sub);
                get(frac); op(And); set(trunc);
                get(flags_slot); get(trunc); op(Eqz); op(Eqz); imm(0x10); op(Mul); op(Or);
                set(flags_slot);
                if (rounding != 3) {
                    // A 24-bit magnitude cannot carry out of 32 bits.
                    imm(1); get(count); imm(1); op(Sub); op(Shl); set(half);
                    increment([&] { get(trunc); get(half); op(GeU); },
                        [&] { get(trunc); get(half); imm(1); op(Sub); op(And); op(Eqz); op(Eqz); },
                        [&] { get(trunc); op(Eqz); op(Eqz); });
                    get(mag_slot); op(Add); set(mag_slot);
                }
                publish_mag(0);
                end_if(); end_if(); end_if();
            };
            const auto classify = [&] {
                // Signed zeros convert exactly, with no flags.
                get(expr); get(frac); op(Or); op(Eqz);
                begin_if(); imm(0); set(next_local);
                op(Else);
                // Any NaN converts to zero with IOC, like FPToFixed.
                get(expr); imm(0xff); op(Eq); get(frac); op(Eqz); op(Eqz); op(And);
                begin_if(); accumulate(1); imm(0); set(next_local);
                op(Else);
                // Infinity saturates with IOC.
                get(expr); imm(0xff); op(Eq);
                begin_if(); accumulate(1); saturate(); set(next_local);
                op(Else);
                if (!is_signed) {
                    get(sign_slot);
                    begin_if(); negative_unsigned(expr, 127, [&] { get(frac); op(Eqz); });
                    op(Else);
                }
                range();
                if (!is_signed) end_if();
                end_if(); end_if(); end_if();
            };
            // FZ flushes a denormal input to a signed zero and raises IDC,
            // exactly like the other FP lowerings; without FZ it is tiny.
            if (start.FPSCR().FTZ()) {
                get(expr); op(Eqz); get(frac); op(Eqz); op(Eqz); op(And);
                begin_if(); accumulate(0x80); imm(0); set(next_local);
                op(Else); classify(); end_if();
            } else classify();
            if (ibits == 16) { get(next_local); mask(0xffff); set(next_local); }
            get(0); load(offsetof(JitState, fpscr)); get(flags_slot); op(Or);
            store(offsetof(JitState, fpscr));
            return ok;
        }
        const auto hi = next_local + 1, expr = next_local + 3;
        const auto frac_lo = next_local + 4, frac_hi = next_local + 5;
        const auto mag_lo = next_local + 6, mag_hi = next_local + 7;
        const auto count = next_local + 9;
        sign_slot = next_local + 2; flags_slot = next_local + 8; mag_slot = next_local + 1;
        // mag_lo/mag_hi are free once the magnitude is in mag_slot.
        const auto inc = mag_lo, carry = mag_hi;
        value_word(inst.GetArg(0), 1); set(hi);
        value_word(inst.GetArg(0), 0); set(frac_lo);
        get(hi); imm(31); op(ShrU); set(sign_slot);
        get(hi); imm(20); op(ShrU); mask(0x7ff); set(expr);
        get(hi); mask(0xfffff); set(frac_hi);
        imm(0); set(flags_slot);
        const auto push_frac = [&] {
            get(frac_lo); op(ExtendU);
            get(frac_hi); op(ExtendU); constant64(code, 32); op(Shl64); op(Or64);
        };
        const auto push_mag = [&] {
            get(mag_lo); op(ExtendU);
            get(mag_hi); op(ExtendU); constant64(code, 32); op(Shl64); op(Or64);
        };
        // In-range doubles always shift right (counts 21..63); the
        // truncated bits are recovered by shifting the mantissa left, so
        // no third i64 word pair is needed for the rounding decision.
        const auto range = [&] {
            // From here on expr is the exponent of value * 2^fbits.
            if (fbits) { get(expr); imm(fbits); op(Add); set(expr); }
            // Magnitudes below 2^-11 (denormals too, even scaled by 2^32)
            // are tiny.
            get(expr); imm(1011); op(LeU);
            begin_if(); accumulate(0x10); tiny(); set(next_local);
            op(Else);
            // Overflow at 2^32 and above.
            get(expr); imm(1055); op(GeU);
            begin_if(); accumulate(1); saturate(); set(next_local);
            op(Else);
            imm(1075); get(expr); op(Sub); set(count);
            push_frac(); constant64(code, 0x10000000000000LL); op(Or64);
            get(count); op(ExtendU); op(ShrU64);
            store_i64_words(mag_lo);
            // The dropped bits, aligned so the half bit is bit 63.
            push_frac(); constant64(code, 0x10000000000000LL); op(Or64);
            imm(64); get(count); op(Sub); op(ExtendU); op(Shl64);
            store_i64_words(frac_lo);
            get(flags_slot); get(frac_lo); get(frac_hi); op(Or); op(Eqz); op(Eqz);
            imm(0x10); op(Mul); op(Or); set(flags_slot);
            push_mag(); op(Wrap); set(mag_slot);
            if (rounding != 3) {
                increment([&] { push_frac(); constant64(code, 63); op(ShrU64); op(Wrap); },
                    [&] { push_frac(); constant64(code, 1); op(Shl64); op(Eqz64); op(Eqz); },
                    [&] { get(frac_lo); get(frac_hi); op(Or); op(Eqz); op(Eqz); });
                set(inc);
                // Magnitudes up to 2^32 - 1 are in range here; rounding one
                // up carries out of the i32 magnitude.
                get(mag_slot); imm(0xffffffffu); op(Eq); get(inc); op(And); set(carry);
                get(mag_slot); get(inc); op(Add); set(mag_slot);
            }
            publish_mag(rounding != 3 ? carry : 0);
            end_if(); end_if();
        };
        const auto classify = [&] {
            get(expr); get(frac_lo); get(frac_hi); op(Or); op(Or); op(Eqz);
            begin_if(); imm(0); set(next_local);
            op(Else);
            get(expr); imm(0x7ff); op(Eq);
            get(frac_lo); get(frac_hi); op(Or); op(Eqz); op(Eqz); op(And);
            begin_if(); accumulate(1); imm(0); set(next_local);
            op(Else);
            get(expr); imm(0x7ff); op(Eq);
            begin_if(); accumulate(1); saturate(); set(next_local);
            op(Else);
            if (!is_signed) {
                get(sign_slot);
                begin_if();
                negative_unsigned(expr, 1023, [&] { get(frac_lo); get(frac_hi); op(Or); op(Eqz); });
                op(Else);
            }
            range();
            if (!is_signed) end_if();
            end_if(); end_if(); end_if();
        };
        if (start.FPSCR().FTZ()) {
            get(expr); op(Eqz);
            get(frac_lo); get(frac_hi); op(Or); op(Eqz); op(Eqz); op(And);
            begin_if(); accumulate(0x80); imm(0); set(next_local);
            op(Else); classify(); end_if();
        } else classify();
        if (ibits == 16) { get(next_local); mask(0xffff); set(next_local); }
        get(0); load(offsetof(JitState, fpscr)); get(flags_slot); op(Or);
        store(offsetof(JitState, fpscr));
        return ok;
    }

    bool instruction(const Inst &inst) {
        const Op kind = inst.GetOpcode();
        // One consumer pre-scan per block (short-circuits after the first
        // instruction); producers below consult it for dead flag words.
        scan_flag_consumers();
        if (arithmetic(kind)) {
            add_sub(inst);
            if (state.aot() && aot_canary()) {
                // Toggle GE[0]: not sticky, so every sample whose call runs an
                // odd number of these differs (a sticky bit would already be
                // set in both runs' starting state).
                state.write_psr_field(code, 0x00010000u, [&] {
                    state.read_dispatch_psr(code); imm(0x00010000u); op(Xor); imm(0x00010000u); op(And);
                });
            }
            return ok;
        }
        if (shift(kind)) { shifted(inst); return ok; }
        const auto arg = [&](size_t n) { value(inst.GetArg(n)); };
        switch (kind) {
        case Op::SignedMultiplyHigh64: return multiply_high64(inst, true);
        case Op::UnsignedMultiplyHigh64: return multiply_high64(inst, false);
        case Op::AESEncryptSingleRound: return aes_round(inst, false);
        case Op::AESDecryptSingleRound: return aes_round(inst, true);
        case Op::AESMixColumns: return aes_mix_columns(inst, false);
        case Op::AESInverseMixColumns: return aes_mix_columns(inst, true);
        case Op::SM4AccessSubstitutionBox: {
            static const auto table = [] {
                std::array<uint8_t, 256> result{};
                for (unsigned i = 0; i < 256; ++i) result[i] = Dynarmic::Common::Crypto::SM4::AccessSubstitutionBox(uint8_t(i));
                return result;
            }();
            arg(0); mask(0xff); set(next_local + 4); byte_lookup(table, next_local + 4); break;
        }
        case Op::SHA256Hash:
        case Op::SHA256MessageSchedule0:
        case Op::SHA256MessageSchedule1: return sha256(inst, kind);
        case Op::CRC32Castagnoli8: return crc32(inst, 8, 0x82f63b78u);
        case Op::CRC32Castagnoli16: return crc32(inst, 16, 0x82f63b78u);
        case Op::CRC32Castagnoli32: return crc32(inst, 32, 0x82f63b78u);
        case Op::CRC32Castagnoli64: return crc32(inst, 64, 0x82f63b78u);
        case Op::CRC32ISO8: return crc32(inst, 8, 0xedb88320u);
        case Op::CRC32ISO16: return crc32(inst, 16, 0xedb88320u);
        case Op::CRC32ISO32: return crc32(inst, 32, 0xedb88320u);
        case Op::CRC32ISO64: return crc32(inst, 64, 0xedb88320u);
        case Op::VectorSignedMultiply16: return vector_full_multiply(inst, 16, true);
        case Op::VectorSignedMultiply32: return vector_full_multiply(inst, 32, true);
        case Op::VectorUnsignedMultiply16: return vector_full_multiply(inst, 16, false);
        case Op::VectorUnsignedMultiply32: return vector_full_multiply(inst, 32, false);
        case Op::IsZero64:
        case Op::TestBit:
        case Op::ConditionalSelect64:
        case Op::SignExtendByteToLong:
        case Op::CountLeadingZeros64:
        case Op::ExtractRegister32:
        case Op::ExtractRegister64:
        case Op::ReplicateBit32:
        case Op::ReplicateBit64:
            return scalar_misc(inst, kind);
        case Op::LogicalShiftLeft64: return scalar_shift64(inst, true, false, false, false);
        case Op::LogicalShiftRight64: return scalar_shift64(inst, false, false, false, false);
        case Op::ArithmeticShiftRight64: return scalar_shift64(inst, false, true, false, false);
        case Op::RotateRight64: return scalar_shift64(inst, false, false, true, false);
        case Op::LogicalShiftLeftMasked32: return scalar_shift32(inst, true, false, false, true);
        case Op::LogicalShiftRightMasked32: return scalar_shift32(inst, false, false, false, true);
        case Op::ArithmeticShiftRightMasked32: return scalar_shift32(inst, false, true, false, true);
        case Op::RotateRightMasked32: return scalar_shift32(inst, false, false, true, true);
        case Op::LogicalShiftLeftMasked64: return scalar_shift64(inst, true, false, false, true);
        case Op::LogicalShiftRightMasked64: return scalar_shift64(inst, false, false, false, true);
        case Op::ArithmeticShiftRightMasked64: return scalar_shift64(inst, false, true, false, true);
        case Op::RotateRightMasked64: return scalar_shift64(inst, false, false, true, true);
        case Op::UnsignedDiv32: return scalar_div(inst, false, false);
        case Op::SignedDiv32: return scalar_div(inst, false, true);
        case Op::UnsignedDiv64: return scalar_div(inst, true, false);
        case Op::SignedDiv64: return scalar_div(inst, true, true);
        case Op::MaxSigned32: return scalar_minmax(inst, 32, true, true);
        case Op::MaxUnsigned32: return scalar_minmax(inst, 32, true, false);
        case Op::MinSigned32: return scalar_minmax(inst, 32, false, true);
        case Op::MinUnsigned32: return scalar_minmax(inst, 32, false, false);
        case Op::MaxSigned64: return scalar_minmax(inst, 64, true, true);
        case Op::MaxUnsigned64: return scalar_minmax(inst, 64, true, false);
        case Op::MinSigned64: return scalar_minmax(inst, 64, false, true);
        case Op::MinUnsigned64: return scalar_minmax(inst, 64, false, false);
        case Op::VectorRoundingShiftLeftS8: return vector_rounding_or_saturated_shift(inst, 8, true, false);
        case Op::VectorRoundingShiftLeftU8: return vector_rounding_or_saturated_shift(inst, 8, false, false);
        case Op::VectorSignedSaturatedShiftLeft8: return vector_rounding_or_saturated_shift(inst, 8, true, true);
        case Op::VectorUnsignedSaturatedShiftLeft8: return vector_rounding_or_saturated_shift(inst, 8, false, true);
        case Op::VectorSignedSaturatedShiftLeftUnsigned8: return vector_rounding_or_saturated_shift(inst, 8, true, true, true);
        case Op::VectorRoundingShiftLeftS16: return vector_rounding_or_saturated_shift(inst, 16, true, false);
        case Op::VectorRoundingShiftLeftU16: return vector_rounding_or_saturated_shift(inst, 16, false, false);
        case Op::VectorSignedSaturatedShiftLeft16: return vector_rounding_or_saturated_shift(inst, 16, true, true);
        case Op::VectorUnsignedSaturatedShiftLeft16: return vector_rounding_or_saturated_shift(inst, 16, false, true);
        case Op::VectorSignedSaturatedShiftLeftUnsigned16: return vector_rounding_or_saturated_shift(inst, 16, true, true, true);
        case Op::VectorRoundingShiftLeftS32: return vector_rounding_or_saturated_shift(inst, 32, true, false);
        case Op::VectorRoundingShiftLeftU32: return vector_rounding_or_saturated_shift(inst, 32, false, false);
        case Op::VectorSignedSaturatedShiftLeft32: return vector_rounding_or_saturated_shift(inst, 32, true, true);
        case Op::VectorUnsignedSaturatedShiftLeft32: return vector_rounding_or_saturated_shift(inst, 32, false, true);
        case Op::VectorSignedSaturatedShiftLeftUnsigned32: return vector_rounding_or_saturated_shift(inst, 32, true, true, true);
        case Op::VectorRoundingShiftLeftS64: return vector_rounding_or_saturated_shift(inst, 64, true, false);
        case Op::VectorRoundingShiftLeftU64: return vector_rounding_or_saturated_shift(inst, 64, false, false);
        case Op::VectorSignedSaturatedShiftLeft64: return vector_rounding_or_saturated_shift(inst, 64, true, true);
        case Op::VectorUnsignedSaturatedShiftLeft64: return vector_rounding_or_saturated_shift(inst, 64, false, true);
        case Op::VectorSignedSaturatedShiftLeftUnsigned64: return vector_rounding_or_saturated_shift(inst, 64, true, true, true);
        case Op::VectorSignedSaturatedAccumulateUnsigned8: return vector_mixed_accumulate(inst, 8, true);
        case Op::VectorUnsignedSaturatedAccumulateSigned8: return vector_mixed_accumulate(inst, 8, false);
        case Op::VectorSignedSaturatedAccumulateUnsigned16: return vector_mixed_accumulate(inst, 16, true);
        case Op::VectorUnsignedSaturatedAccumulateSigned16: return vector_mixed_accumulate(inst, 16, false);
        case Op::VectorSignedSaturatedAccumulateUnsigned32: return vector_mixed_accumulate(inst, 32, true);
        case Op::VectorUnsignedSaturatedAccumulateSigned32: return vector_mixed_accumulate(inst, 32, false);
        case Op::VectorSignedSaturatedAccumulateUnsigned64: return vector_mixed_accumulate(inst, 64, true);
        case Op::VectorUnsignedSaturatedAccumulateSigned64: return vector_mixed_accumulate(inst, 64, false);
        case Op::SignedSaturatedDoublingMultiplyReturnHigh16: return saturated_doubling_multiply(inst, 16, false, false, true);
        case Op::VectorSignedSaturatedDoublingMultiplyHigh16: return saturated_doubling_multiply(inst, 16, false, false);
        case Op::VectorSignedSaturatedDoublingMultiplyHighRounding16: return saturated_doubling_multiply(inst, 16, true, false);
        case Op::VectorSignedSaturatedDoublingMultiplyLong16: return saturated_doubling_multiply(inst, 16, false, true);
        case Op::SignedSaturatedDoublingMultiplyReturnHigh32: return saturated_doubling_multiply(inst, 32, false, false, true);
        case Op::VectorSignedSaturatedDoublingMultiplyHigh32: return saturated_doubling_multiply(inst, 32, false, false);
        case Op::VectorSignedSaturatedDoublingMultiplyHighRounding32: return saturated_doubling_multiply(inst, 32, true, false);
        case Op::VectorSignedSaturatedDoublingMultiplyLong32: return saturated_doubling_multiply(inst, 32, false, true);
        case Op::SignedSaturatedAdd8: return scalar_saturating(inst, 8, true, true);
        case Op::SignedSaturatedAdd16: return scalar_saturating(inst, 16, true, true);
        case Op::SignedSaturatedAdd32: return scalar_saturating(inst, 32, true, true);
        case Op::SignedSaturatedAdd64: return scalar_saturating(inst, 64, true, true);
        case Op::SignedSaturatedSub8: return scalar_saturating(inst, 8, true, false);
        case Op::SignedSaturatedSub16: return scalar_saturating(inst, 16, true, false);
        case Op::SignedSaturatedSub32: return scalar_saturating(inst, 32, true, false);
        case Op::SignedSaturatedSub64: return scalar_saturating(inst, 64, true, false);
        case Op::UnsignedSaturatedAdd8: return scalar_saturating(inst, 8, false, true);
        case Op::UnsignedSaturatedAdd16: return scalar_saturating(inst, 16, false, true);
        case Op::UnsignedSaturatedAdd32: return scalar_saturating(inst, 32, false, true);
        case Op::UnsignedSaturatedAdd64: return scalar_saturating(inst, 64, false, true);
        case Op::UnsignedSaturatedSub8: return scalar_saturating(inst, 8, false, false);
        case Op::UnsignedSaturatedSub16: return scalar_saturating(inst, 16, false, false);
        case Op::UnsignedSaturatedSub32: return scalar_saturating(inst, 32, false, false);
        case Op::UnsignedSaturatedSub64: return scalar_saturating(inst, 64, false, false);
        case Op::Breakpoint: op(Unreachable); return true;
        case Op::CallHostFunction:
            reject("native function addresses are unavailable in portable Wasm"); return false;
        case Op::A32CoprocInternalOperation:
        case Op::A32CoprocSendTwoWords:
        case Op::A32CoprocGetTwoWords:
        case Op::A32CoprocLoadWords:
        case Op::A32CoprocStoreWords:
            reject("no browser callback is registered for this coprocessor operation"); return false;
        case Op::Void: return true; // Dynarmic's invalidated/dead instruction marker
        case Op::A32DataMemoryBarrier:
        case Op::A32DataSynchronizationBarrier:
        case Op::A32InstructionSynchronizationBarrier:
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
            // WebAssembly atomic.fence has a required zero flags immediate.
            op(0xfe); uleb(code, 0x03); op(0x00);
#else
            // Single host thread: no other observer of guest memory exists,
            // and the JIT holds no stale translations needing an ISB flush.
#endif
            return true;
        case Op::Identity: {
            const auto type = inst.GetType();
            if (type != Type::U128 && !scalar(type)) return false;
            const unsigned words = type == Type::U128 ? 4 : type == Type::U64 ? 2 : 1;
            for (unsigned i = 0; i < words; ++i) { value_word(inst.GetArg(0), i); set(next_local + i); }
            return ok;
        }
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
        case Op::A32CoprocGetOneWord: {
            const auto info = inst.GetArg(0);
            // MRC p15,0,Rt,c13,c0,3: read the current thread's TLS base.
            // All other coprocessor operations remain unsupported.
            if (!info.IsImmediate() || info.GetType() != Type::CoprocInfo
                || info.GetCoprocInfo() != Dynarmic::IR::Value::CoprocessorInfo{15, 0, 0, 13, 0, 3})
                return false;
            load(offsetof(JitState, tpidruro));
            break;
        }
        case Op::A32CoprocSendOneWord: {
            // MCR p15,0,Rt,c13,c0,3: store the guest TLS base. The matching
            // MRC read above observes it; all other registers remain
            // unsupported. Both SendOneWord and GetOneWord carry the same
            // six fields; only InternalOperation includes CRd.
            const auto info = inst.GetArg(0);
            if (!info.IsImmediate() || info.GetType() != Type::CoprocInfo
                || info.GetCoprocInfo() != Dynarmic::IR::Value::CoprocessorInfo{15, 0, 0, 13, 0, 3})
                return false;
            get(0); arg(1); store(offsetof(JitState, tpidruro));
            return ok; // Void operation: do not publish a scalar SSA result.
        }
        case Op::FPCompare32: {
            // Compare IEEE-754 bit patterns without host FP conversion: preserve
            // signaling NaNs, signed zeros and guest flush-to-zero behavior.
            if (!inst.GetArg(2).IsImmediate()) return false;
            // Exception enables are NOT in Dynarmic's location key. Check
            // the live FPSCR rather than silently ignoring enabled traps.
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const bool signal = inst.GetArg(2).GetU1();
            imm(0); set(next_local + 3); // any NaN
            imm(0); set(next_local + 4); // invalid-operation cumulative bit
            for (unsigned i = 0; i < 2; ++i) {
                const auto slot = next_local + 1 + i;
                arg(i); set(slot);
                get(slot); mask(0x7fffffff); imm(0x7f800000); op(GtU);
                begin_if();
                imm(1); set(next_local + 3);
                get(next_local + 4);
                if (signal) imm(1);
                else { get(slot); mask(0x00400000); op(Eqz); }
                op(Or); set(next_local + 4);
                end_if();
                if (start.FPSCR().Value() & (1u << 24)) {
                    get(slot); mask(0x7fffffff); imm(0x00800000); op(LtU);
                    get(slot); mask(0x7fffffff); op(Eqz); op(Eqz); op(And);
                    begin_if();
                    get(0); load(offsetof(JitState, fpscr)); imm(0x80); op(Or); store(offsetof(JitState, fpscr));
                    get(slot); mask(0x80000000); set(slot);
                    end_if();
                }
                // Canonicalize both zeros before forming an unsigned sortable
                // key: complement negatives, toggle the sign of nonnegatives.
                get(slot); mask(0x7fffffff); op(Eqz); begin_if();
                imm(0); set(slot); end_if();
                get(slot); imm(0xffffffff); imm(0x80000000);
                get(slot); imm(31); op(ShrU); op(Select); op(Xor); set(slot);
            }
            get(0); load(offsetof(JitState, fpscr)); get(next_local + 4); op(Or); store(offsetof(JitState, fpscr));
            imm(0x30000000); // unordered: C,V
            imm(0x60000000); // equal: Z,C
            imm(0x80000000); // less: N
            imm(0x20000000); // greater: C
            get(next_local + 1); get(next_local + 2); op(LtU); op(Select);
            get(next_local + 1); get(next_local + 2); op(Eq); op(Select);
            get(next_local + 3); op(Select);
            break;
        }
        case Op::FPVectorEqual32:
        case Op::FPVectorGreater32:
        case Op::FPVectorGreaterEqual32: {
            // A32 VCEQ/VCGT/VCGE.f32 use StandardFPSCRValue: RN, FZ=DN=1,
            // regardless of the live mode bits. LT/LE are GT/GE with swapped
            // operands in the frontend. Compare integer bit-pattern keys so
            // Wasm cannot quiet an sNaN before we account for its exception.
            // Ordered GT/GE signal IOC for ANY NaN; EQ only for an sNaN.
            if (!inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetType() != Type::U1
                || inst.GetArg(2).GetU1())
                return fp_vector_extended(inst, kind == Op::FPVectorEqual32 ? FPOperation::Equal : kind == Op::FPVectorGreater32 ? FPOperation::Greater : FPOperation::GreaterEqual, 32);
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const auto a = next_local + 4, b = next_local + 5;
            const auto nan = next_local + 6, flags = next_local + 7;
            imm(0); set(flags);
            for (unsigned word = 0; word < 4; ++word) {
                imm(0); set(nan);
                for (unsigned i = 0; i < 2; ++i) {
                    const auto slot = a + i;
                    value_word(inst.GetArg(i), word); set(slot);
                    get(slot); mask(0x7fffffff); imm(0x7f800000); op(GtU);
                    begin_if();
                    imm(1); set(nan);
                    get(flags);
                    if (kind == Op::FPVectorEqual32) { get(slot); mask(0x00400000); op(Eqz); }
                    else imm(1);
                    op(Or); set(flags);
                    end_if();
                    // FZ is unconditionally on. Unpack BOTH operands even
                    // for unordered lanes: a NaN must not suppress IDC.
                    get(slot); mask(0x7fffffff); imm(0x00800000); op(LtU);
                    get(slot); mask(0x7fffffff); op(Eqz); op(Eqz); op(And);
                    begin_if();
                    get(flags); imm(0x80); op(Or); set(flags);
                    get(slot); mask(0x80000000); set(slot);
                    end_if();
                    // +/-0 compare equal. Negatives sort by complemented
                    // bits; nonnegatives by bits with the sign bit toggled.
                    get(slot); mask(0x7fffffff); op(Eqz);
                    begin_if(); imm(0); set(slot); end_if();
                    get(slot); imm(0xffffffff); imm(0x80000000);
                    get(slot); imm(31); op(ShrU); op(Select); op(Xor); set(slot);
                }
                imm(0);
                get(a); get(b);
                op(kind == Op::FPVectorEqual32 ? Eq : kind == Op::FPVectorGreater32 ? GtU : GeU);
                get(nan); op(Eqz); op(And);
                op(Sub); set(next_local + word); // 0 - predicate = all ones/zero
            }
            get(0); load(offsetof(JitState, fpscr)); get(flags); op(Or);
            store(offsetof(JitState, fpscr));
            return ok;
        }
        case Op::FPFixedS32ToSingle:
        case Op::FPFixedU32ToSingle:
            // The IR carries an explicit rounding mode (scalar integer VCVT
            // gets it from FPSCR). Keep the unscaled nearest-even fast path.
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() != 0
                || !inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetU8() != 0)
                return fp_extended(inst, FPOperation::FromFixed, 32, 32, kind == Op::FPFixedU32ToSingle);
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            arg(0); op(kind == Op::FPFixedU32ToSingle ? 0xb3 : 0xb2); // f32.convert_i32_u/s
            op(0xbc); set(next_local); // i32.reinterpret_f32, retain result bits
            get(0); load(offsetof(JitState, fpscr));
            // f64 represents every i32 exactly. Compare the rounded result
            // against that exact value to accumulate FPSCR.IXC.
            arg(0); op(kind == Op::FPFixedU32ToSingle ? 0xb8 : 0xb7); // f64.convert_i32_u/s
            get(next_local); op(0xbe); op(0xbb); // f32 bits -> f64.promote_f32
            op(0x62); imm(4); op(Shl); op(Or); store(offsetof(JitState, fpscr));
            return ok;
        case Op::FPVectorFromSignedFixed32:
        case Op::FPVectorFromUnsignedFixed32: {
            // Per-lane i32 -> binary32. The A32 translator emits fbits=0,
            // ToNearest_TieEven and fpcr_controlled=false (standard FPSCR);
            // each rides the IR as an immediate. Wasm's f32.convert_i32_s/u
            // is exactly that conversion and is independent of FPSCR, so only
            // the architectural shape is accepted and anything else rejects
            // the block. Like Dynarmic's native backends, no exception flags
            // are raised: an i32 magnitude cannot overflow binary32 and, with
            // fbits == 0, cannot underflow below the normal range either; the
            // rounding-loss flags of the abstract standard-FPSCR result are
            // discarded there, so bit-exact parity keeps them discarded here.
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() != 0
                || !inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetU8() != 0
                || !inst.GetArg(3).IsImmediate() || inst.GetArg(3).GetU1() != 0)
                return fp_vector_extended(inst, FPOperation::FromFixed, 32, 32, kind == Op::FPVectorFromUnsignedFixed32);
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word);
                op(kind == Op::FPVectorFromUnsignedFixed32 ? 0xb3 : 0xb2); // f32.convert_i32_u/s
                op(0xbc); // i32.reinterpret_f32 keeps the result bits
                set(next_local + word);
            }
            return ok;
        }
        case Op::FPVectorAbs16:
        case Op::FPVectorAbs32:
        case Op::FPVectorAbs64: {
            // VABS.f16/f32/f64: per-lane sign clear. Exact in every FPSCR
            // mode and raising nothing (an SNaN keeps its payload, sign
            // cleared), so no FPSCR guard or flag accumulation. The D-form
            // upper U128 lanes pass through to SetVector, which stores only
            // its architectural footprint.
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word);
                if (kind == Op::FPVectorAbs16) mask(0x7fff7fff);
                else if (kind == Op::FPVectorAbs32) mask(0x7fffffff);
                else if (word & 1) mask(0x7fffffff);
                set(next_local + word);
            }
            return ok;
        }
        case Op::FPVectorNeg32:
            // VNEG.F32 flips the sign bit only; it raises no FP exception.
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word); imm(0x80000000u); op(Xor); set(next_local + word);
            }
            return ok;
        case Op::FPVectorMin32:
        case Op::FPVectorMax32: {
            // VMIN/VMAX.F32 under the A32 standard FPSCR (fpcr_controlled=0):
            // denormal inputs flush to signed zero (IDC), any NaN gives the
            // default NaN (IOC for a signaling one), min(+0,-0) = -0 and
            // max(+0,-0) = +0 (Wasm f32.min/max order zeros the same way).
            // fast_fp drops the cumulative flags like the other FP operations.
            if (!inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetU1())
                return fp_vector_extended(inst, kind == Op::FPVectorMin32 ? FPOperation::Min : FPOperation::Max, 32);
            const bool minimum = kind == Op::FPVectorMin32;
            const bool flags = !state.fast_fp();
            const auto la = next_local + 4, lb = next_local + 5, lflags = next_local + 6;
            if (flags) {
                load(offsetof(JitState, fpscr)); mask(0x00009f00u);
                begin_if(); ret(ExitReason::Unsupported); end_if();
                imm(0); set(lflags);
            }
            for (unsigned word = 0; word < 4; ++word) {
                for (unsigned i = 0; i < 2; ++i) {
                    const auto lane = i ? lb : la;
                    value_word(inst.GetArg(i), word); set(lane);
                    if (flags) {
                        // IOC: signaling NaN (quiet bit clear, nonzero mantissa).
                        get(lane); mask(0x7fffffff); imm(0x7f800000); op(GtU);
                        get(lane); mask(0x00400000); op(Eqz); op(And);
                        // IDC: nonzero denormal input about to be flushed.
                        get(lane); mask(0x7fffffff); imm(0x00800000); op(LtU);
                        get(lane); mask(0x7fffffff); op(Eqz); op(Eqz); op(And);
                        imm(7); op(Shl); op(Or);
                        get(lflags); op(Or); set(lflags);
                    }
                    // flush: |x| < min normal -> sign only
                    get(lane); mask(0x80000000u);
                    get(lane);
                    get(lane); mask(0x7fffffff); imm(0x00800000); op(LtU);
                    op(Select); set(lane);
                }
                imm(0x7fc00000);
                get(la); op(0xbe); get(lb); op(0xbe); op(minimum ? 0x96 : 0x97); op(0xbc);
                get(la); mask(0x7fffffff); imm(0x7f800000); op(GtU);
                get(lb); mask(0x7fffffff); imm(0x7f800000); op(GtU); op(Or);
                op(Select); set(next_local + word);
            }
            if (flags) {
                get(0); load(offsetof(JitState, fpscr)); get(lflags); op(Or);
                store(offsetof(JitState, fpscr));
            }
            return ok;
        }
        case Op::FPSqrt64:
            if (state.fast_fp() && !start.FPSCR().FTZ() && !(start.FPSCR().Value() & 0x00c00000u)) {
                value64(inst.GetArg(0)); op(0xbf); op(0x9f); // f64.sqrt
                store_f64_words(next_local);
                return ok;
            }
            // Exact result and cumulative flags from the native helper
            // (fp64.cpp operation 10) under the live FPSCR, like FPAdd64.
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            for (unsigned word = 0; word < 2; ++word) {
                get(0); value_word(inst.GetArg(0), word);
                store(offsetof(JitState, memory_value) + 4 * word);
            }
            get(0); imm(10); load(offsetof(JitState, fpscr));
            op(Call); uleb(code, 2); set(next_local + 2);
            get(0); load(offsetof(JitState, fpscr)); get(next_local + 2); op(Or);
            store(offsetof(JitState, fpscr));
            load(offsetof(JitState, memory_value)); set(next_local);
            load(offsetof(JitState, memory_value) + 4); set(next_local + 1);
            return ok;
        case Op::VectorZeroExtend8:
            // Low eight bytes widen to eight halfwords (pmovzxbw).
            for (unsigned word = 0; word < 4; ++word) {
                const unsigned source = word / 2, shift = (word % 2) * 16;
                value_word(inst.GetArg(0), source); if (shift) { imm(shift); op(ShrU); } mask(0xff);
                value_word(inst.GetArg(0), source); imm(shift + 8); op(ShrU); mask(0xff); imm(16); op(Shl);
                op(Or); set(next_local + word);
            }
            return ok;
        case Op::VectorTable:
            // Tuple of 1..4 table registers; VectorTableLookup reads its args.
            return ok;
        case Op::VectorTableLookup64:
        case Op::VectorTableLookup128: {
            const bool wide = kind == Op::VectorTableLookup128;
            const unsigned register_words = wide ? 4 : 2;
            // VTBL/VTBX: byte i = idx < 8n ? table byte idx : defaults byte i.
            const auto &table_value = inst.GetArg(1);
            if (table_value.IsImmediate() || table_value.GetInst()->GetOpcode() != Op::VectorTable)
                return false;
            const Inst *table = table_value.GetInst();
            std::vector<Value> registers;
            for (size_t i = 0; i < table->NumArgs(); ++i) {
                const Value reg = table->GetArg(i);
                if (reg.IsEmpty()) break;
                if (reg.GetType() != (wide ? Type::U128 : Type::U64)) return false;
                registers.push_back(reg);
            }
            if (registers.empty()) return false;
            const uint32_t table_words = static_cast<uint32_t>(registers.size() * register_words);
            const auto idx = next_local + 4, acc = next_local + 5;
            for (unsigned i = 0; i < (wide ? 16u : 8u); ++i) {
                const unsigned word = i / 4, shift = (i % 4) * 8;
                if (shift == 0) { imm(0); set(acc); }
                value_word(inst.GetArg(2), word); if (shift) { imm(shift); op(ShrU); } mask(0xff); set(idx);
                // Word (idx >> 2) of the table by a select chain.
                value_word(registers[0], 0);
                for (uint32_t k = 1; k < table_words; ++k) {
                    set(next_local + 6);
                    value_word(registers[k / register_words], k % register_words);
                    get(next_local + 6);
                    get(idx); imm(2); op(ShrU); imm(k); op(Eq);
                    op(Select);
                }
                get(idx); imm(3); op(And); imm(3); op(Shl); op(ShrU); mask(0xff);
                value_word(inst.GetArg(0), word); if (shift) { imm(shift); op(ShrU); } mask(0xff);
                get(idx); imm(table_words * 4); op(LtU);
                op(Select);
                if (shift) { imm(shift); op(Shl); }
                get(acc); op(Or); set(acc);
                if (i % 4 == 3) { get(acc); set(next_local + word); }
            }
            if (!wide) { imm(0); set(next_local + 2); imm(0); set(next_local + 3); }
            return ok;
        }
        case Op::VectorZeroExtend16:
            // Low four halfwords widen to four words (pmovzxwd).
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word / 2);
                if (word % 2) { imm(16); op(ShrU); } else mask(0xffff);
                set(next_local + word);
            }
            return ok;
        case Op::VectorNarrow16:
            // Low byte of each 16-bit lane into the low doubleword (VMOVN.I16).
            for (unsigned word = 0; word < 2; ++word) {
                value_word(inst.GetArg(0), word * 2); mask(0xff);
                value_word(inst.GetArg(0), word * 2); imm(8); op(ShrU); mask(0xff00); op(Or);
                value_word(inst.GetArg(0), word * 2 + 1); mask(0xff); imm(16); op(Shl); op(Or);
                value_word(inst.GetArg(0), word * 2 + 1); imm(8); op(Shl); mask(0xff000000u); op(Or);
                set(next_local + word);
            }
            imm(0); set(next_local + 2); imm(0); set(next_local + 3);
            return ok;
        case Op::VectorReverseElementsInHalfGroups8: return vector_reverse(inst, 8, 16);
        case Op::VectorReverseElementsInWordGroups8: return vector_reverse(inst, 8, 32);
        case Op::VectorReverseElementsInWordGroups16: return vector_reverse(inst, 16, 32);
        case Op::VectorReverseElementsInLongGroups8: return vector_reverse(inst, 8, 64);
        case Op::VectorReverseElementsInLongGroups16: return vector_reverse(inst, 16, 64);
        case Op::VectorReverseElementsInLongGroups32: return vector_reverse(inst, 32, 64);
        case Op::VectorNarrow32:
            // Low halfword of each 32-bit lane into the low doubleword (pmovdw).
            for (unsigned word = 0; word < 2; ++word) {
                value_word(inst.GetArg(0), word * 2); mask(0xffff);
                value_word(inst.GetArg(0), word * 2 + 1); imm(16); op(Shl);
                op(Or); set(next_local + word);
            }
            imm(0); set(next_local + 2); imm(0); set(next_local + 3);
            return ok;
        case Op::VectorTranspose8: return vector_transpose(inst, 8);
        case Op::VectorTranspose16: return vector_transpose(inst, 16);
        case Op::VectorTranspose32: return vector_transpose(inst, 32);
        case Op::VectorTranspose64: return vector_transpose(inst, 64);
        case Op::VectorHalvingAddS8: return vector_halving_add(inst, 8, true);
        case Op::VectorHalvingAddS16: return vector_halving_add(inst, 16, true);
        case Op::VectorHalvingAddS32: return vector_halving_add(inst, 32, true);
        case Op::VectorHalvingAddU8: return vector_halving_add(inst, 8, false);
        case Op::VectorHalvingAddU16: return vector_halving_add(inst, 16, false);
        case Op::VectorHalvingAddU32: return vector_halving_add(inst, 32, false);
        case Op::VectorPairedAddLower8: return vector_paired_add_lower(inst, 8);
        case Op::VectorPairedAddLower16: return vector_paired_add_lower(inst, 16);
        case Op::VectorPairedAddLower32: return vector_paired_add_lower(inst, 32);
        case Op::VectorInterleaveLower8: return vector_interleave(inst, 8, false);
        case Op::VectorInterleaveLower16: return vector_interleave(inst, 16, false);
        case Op::VectorInterleaveLower32: return vector_interleave(inst, 32, false);
        case Op::VectorInterleaveLower64: return vector_interleave(inst, 64, false);
        case Op::VectorInterleaveUpper8: return vector_interleave(inst, 8, true);
        case Op::VectorInterleaveUpper16: return vector_interleave(inst, 16, true);
        case Op::VectorInterleaveUpper32: return vector_interleave(inst, 32, true);
        case Op::VectorInterleaveUpper64: return vector_interleave(inst, 64, true);
        case Op::VectorDeinterleaveEven8: return vector_deinterleave(inst, 8, false, false);
        case Op::VectorDeinterleaveEven16: return vector_deinterleave(inst, 16, false, false);
        case Op::VectorDeinterleaveEven64: return vector_deinterleave(inst, 64, false, false);
        case Op::VectorDeinterleaveEven32: return vector_deinterleave(inst, 32, false, false);
        case Op::VectorDeinterleaveOdd8: return vector_deinterleave(inst, 8, true, false);
        case Op::VectorDeinterleaveOdd16: return vector_deinterleave(inst, 16, true, false);
        case Op::VectorDeinterleaveOdd64: return vector_deinterleave(inst, 64, true, false);
        case Op::VectorDeinterleaveOdd32: return vector_deinterleave(inst, 32, true, false);
        case Op::VectorDeinterleaveEvenLower8: return vector_deinterleave(inst, 8, false, true);
        case Op::VectorDeinterleaveEvenLower16: return vector_deinterleave(inst, 16, false, true);
        case Op::VectorDeinterleaveEvenLower32: return vector_deinterleave(inst, 32, false, true);
        case Op::VectorDeinterleaveOddLower8: return vector_deinterleave(inst, 8, true, true);
        case Op::VectorDeinterleaveOddLower16: return vector_deinterleave(inst, 16, true, true);
        case Op::VectorDeinterleaveOddLower32: return vector_deinterleave(inst, 32, true, true);
        case Op::VectorPairedAddSignedWiden8: return vector_paired_add_widen(inst, 8, true);
        case Op::VectorPairedAddSignedWiden16: return vector_paired_add_widen(inst, 16, true);
        case Op::VectorPairedAddSignedWiden32: return vector_paired_add_widen(inst, 32, true);
        case Op::VectorPairedAddUnsignedWiden8: return vector_paired_add_widen(inst, 8, false);
        case Op::VectorPairedAddUnsignedWiden16: return vector_paired_add_widen(inst, 16, false);
        case Op::VectorPairedAddUnsignedWiden32: return vector_paired_add_widen(inst, 32, false);
        case Op::VectorSignExtend8: return vector_sign_extend(inst, 8);
        case Op::VectorSignExtend16: return vector_sign_extend(inst, 16);
        case Op::VectorSignExtend32: return vector_sign_extend(inst, 32);
        case Op::VectorLogicalVShift8: return vector_logical_vshift(inst, 8);
        case Op::VectorLogicalVShift16: return vector_logical_vshift(inst, 16);
        case Op::VectorLogicalVShift32: return vector_logical_vshift(inst, 32);
        case Op::VectorLogicalVShift64: return vector_logical_vshift(inst, 64);
        case Op::VectorMultiplySignedWiden8: return vector_multiply_widen(inst, 8, true);
        case Op::VectorMultiplySignedWiden16: return vector_multiply_widen(inst, 16, true);
        case Op::VectorMultiplySignedWiden32: return vector_multiply_widen(inst, 32, true);
        case Op::VectorMultiplyUnsignedWiden8: return vector_multiply_widen(inst, 8, false);
        case Op::VectorMultiplyUnsignedWiden16: return vector_multiply_widen(inst, 16, false);
        case Op::VectorMultiplyUnsignedWiden32: return vector_multiply_widen(inst, 32, false);
        case Op::VectorUnsignedSaturatedNarrow16: return vector_unsigned_saturated_narrow(inst, 8);
        case Op::VectorUnsignedSaturatedNarrow32: return vector_unsigned_saturated_narrow(inst, 16);
        case Op::VectorUnsignedSaturatedNarrow64: return vector_unsigned_saturated_narrow(inst, 32);
        case Op::VectorPolynomialMultiply8: polynomial_multiply8(inst); return ok;
        case Op::VectorPolynomialMultiplyLong8: polynomial_multiply_long8(inst); return ok;
        case Op::VectorArithmeticVShift8: return vector_arithmetic_vshift(inst, 8);
        case Op::VectorArithmeticVShift16: return vector_arithmetic_vshift(inst, 16);
        case Op::VectorArithmeticVShift32: return vector_arithmetic_vshift(inst, 32);
        case Op::VectorArithmeticVShift64: return vector_arithmetic_vshift(inst, 64);
        case Op::VectorRoundingHalvingAddS8: return vector_halving_add(inst, 8, true, true);
        case Op::VectorRoundingHalvingAddS16: return vector_halving_add(inst, 16, true, true);
        case Op::VectorRoundingHalvingAddS32: return vector_halving_add(inst, 32, true, true);
        case Op::VectorRoundingHalvingAddU8: return vector_halving_add(inst, 8, false, true);
        case Op::VectorRoundingHalvingAddU16: return vector_halving_add(inst, 16, false, true);
        case Op::VectorRoundingHalvingAddU32: return vector_halving_add(inst, 32, false, true);
        case Op::VectorSignedSaturatedNarrowToSigned16: return vector_signed_saturated_narrow(inst, 8, false);
        case Op::VectorSignedSaturatedNarrowToSigned32: return vector_signed_saturated_narrow(inst, 16, false);
        case Op::VectorSignedSaturatedNarrowToSigned64: return vector_signed_saturated_narrow(inst, 32, false);
        case Op::VectorSignedSaturatedNarrowToUnsigned16: return vector_signed_saturated_narrow(inst, 8, true);
        case Op::VectorSignedSaturatedNarrowToUnsigned32: return vector_signed_saturated_narrow(inst, 16, true);
        case Op::VectorSignedSaturatedNarrowToUnsigned64: return vector_signed_saturated_narrow(inst, 32, true);
        case Op::VectorSignedSaturatedAdd8: return vector_saturated_add_sub(inst, 8, true, true);
        case Op::VectorSignedSaturatedAdd16: return vector_saturated_add_sub(inst, 16, true, true);
        case Op::VectorSignedSaturatedAdd32: return vector_saturated_add_sub(inst, 32, true, true);
        case Op::VectorSignedSaturatedAdd64: return vector_saturated_add_sub(inst, 64, true, true);
        case Op::VectorSignedSaturatedSub8: return vector_saturated_add_sub(inst, 8, true, false);
        case Op::VectorSignedSaturatedSub16: return vector_saturated_add_sub(inst, 16, true, false);
        case Op::VectorSignedSaturatedSub32: return vector_saturated_add_sub(inst, 32, true, false);
        case Op::VectorSignedSaturatedSub64: return vector_saturated_add_sub(inst, 64, true, false);
        case Op::VectorUnsignedSaturatedAdd8: return vector_saturated_add_sub(inst, 8, false, true);
        case Op::VectorUnsignedSaturatedAdd16: return vector_saturated_add_sub(inst, 16, false, true);
        case Op::VectorUnsignedSaturatedAdd32: return vector_saturated_add_sub(inst, 32, false, true);
        case Op::VectorUnsignedSaturatedAdd64: return vector_saturated_add_sub(inst, 64, false, true);
        case Op::VectorUnsignedSaturatedSub8: return vector_saturated_add_sub(inst, 8, false, false);
        case Op::VectorUnsignedSaturatedSub16: return vector_saturated_add_sub(inst, 16, false, false);
        case Op::VectorUnsignedSaturatedSub32: return vector_saturated_add_sub(inst, 32, false, false);
        case Op::VectorUnsignedSaturatedSub64: return vector_saturated_add_sub(inst, 64, false, false);
        case Op::VectorSignedSaturatedAbs8: return vector_saturated_abs_neg(inst, 8, false);
        case Op::VectorSignedSaturatedAbs16: return vector_saturated_abs_neg(inst, 16, false);
        case Op::VectorSignedSaturatedAbs32: return vector_saturated_abs_neg(inst, 32, false);
        case Op::VectorSignedSaturatedAbs64: return vector_saturated_abs_neg(inst, 64, false);
        case Op::VectorSignedSaturatedNeg8: return vector_saturated_abs_neg(inst, 8, true);
        case Op::VectorSignedSaturatedNeg16: return vector_saturated_abs_neg(inst, 16, true);
        case Op::VectorSignedSaturatedNeg32: return vector_saturated_abs_neg(inst, 32, true);
        case Op::VectorSignedSaturatedNeg64: return vector_saturated_abs_neg(inst, 64, true);
        case Op::VectorNarrow64:
            // Low word of each 64-bit lane into the low doubleword (VMOVN.I64).
            value_word(inst.GetArg(0), 0); set(next_local);
            value_word(inst.GetArg(0), 2); set(next_local + 1);
            imm(0); set(next_local + 2); imm(0); set(next_local + 3);
            return ok;
        case Op::VectorZeroExtend32:
            // Low two words widen to two doublewords (VMOVL.U32).
            value_word(inst.GetArg(0), 0); set(next_local);
            imm(0); set(next_local + 1);
            value_word(inst.GetArg(0), 1); set(next_local + 2);
            imm(0); set(next_local + 3);
            return ok;
        case Op::VectorZeroExtend64:
            value_word(inst.GetArg(0), 0); set(next_local);
            value_word(inst.GetArg(0), 1); set(next_local + 1);
            imm(0); set(next_local + 2); imm(0); set(next_local + 3);
            return ok;
        case Op::A32GetFpscr:
            load(offsetof(JitState, fpscr)); set(next_local);
            return ok;
        case Op::A32SetFpscr:
            // VMSR ends its block (PopRSBHint); later blocks are keyed by the
            // new FPSCR mode through the normal location checks.
            get(0); arg(0); store(offsetof(JitState, fpscr));
            return ok;
        case Op::FPVectorAdd64: return fp_vector_extended(inst, FPOperation::Add, 64);
        case Op::FPVectorSub64: return fp_vector_extended(inst, FPOperation::Sub, 64);
        case Op::FPVectorMul64: return fp_vector_extended(inst, FPOperation::Mul, 64);
        case Op::FPVectorDiv32: return fp_vector_extended(inst, FPOperation::Div, 32);
        case Op::FPVectorDiv64: return fp_vector_extended(inst, FPOperation::Div, 64);
        case Op::FPVectorEqual16: return fp_vector_extended(inst, FPOperation::Equal, 16);
        case Op::FPVectorEqual64: return fp_vector_extended(inst, FPOperation::Equal, 64);
        case Op::FPVectorGreater64: return fp_vector_extended(inst, FPOperation::Greater, 64);
        case Op::FPVectorGreaterEqual64: return fp_vector_extended(inst, FPOperation::GreaterEqual, 64);
        case Op::FPVectorMin64: return fp_vector_extended(inst, FPOperation::Min, 64);
        case Op::FPVectorMax64: return fp_vector_extended(inst, FPOperation::Max, 64);
        case Op::FPVectorMinNumeric32: return fp_vector_extended(inst, FPOperation::MinNumeric, 32);
        case Op::FPVectorMinNumeric64: return fp_vector_extended(inst, FPOperation::MinNumeric, 64);
        case Op::FPVectorMaxNumeric32: return fp_vector_extended(inst, FPOperation::MaxNumeric, 32);
        case Op::FPVectorMaxNumeric64: return fp_vector_extended(inst, FPOperation::MaxNumeric, 64);
        case Op::FPVectorMulAdd16: return fp_vector_extended(inst, FPOperation::MulAdd, 16);
        case Op::FPVectorMulAdd32: return fp_vector_extended(inst, FPOperation::MulAdd, 32);
        case Op::FPVectorMulAdd64: return fp_vector_extended(inst, FPOperation::MulAdd, 64);
        case Op::FPVectorMulX32: return fp_vector_extended(inst, FPOperation::MulX, 32);
        case Op::FPVectorMulX64: return fp_vector_extended(inst, FPOperation::MulX, 64);
        case Op::FPVectorRecipEstimate16: return fp_vector_extended(inst, FPOperation::RecipEstimate, 16);
        case Op::FPVectorRecipEstimate64: return fp_vector_extended(inst, FPOperation::RecipEstimate, 64);
        case Op::FPVectorRecipStepFused16: return fp_vector_extended(inst, FPOperation::RecipStep, 16);
        case Op::FPVectorRecipStepFused64: return fp_vector_extended(inst, FPOperation::RecipStep, 64);
        case Op::FPVectorRSqrtEstimate16: return fp_vector_extended(inst, FPOperation::RSqrtEstimate, 16);
        case Op::FPVectorRSqrtEstimate64: return fp_vector_extended(inst, FPOperation::RSqrtEstimate, 64);
        case Op::FPVectorRSqrtStepFused16: return fp_vector_extended(inst, FPOperation::RSqrtStep, 16);
        case Op::FPVectorRSqrtStepFused64: return fp_vector_extended(inst, FPOperation::RSqrtStep, 64);
        case Op::FPVectorRoundInt16: return fp_vector_extended(inst, FPOperation::RoundInt, 16);
        case Op::FPVectorRoundInt32: return fp_vector_extended(inst, FPOperation::RoundInt, 32);
        case Op::FPVectorRoundInt64: return fp_vector_extended(inst, FPOperation::RoundInt, 64);
        case Op::FPVectorSqrt32: return fp_vector_extended(inst, FPOperation::Sqrt, 32);
        case Op::FPVectorSqrt64: return fp_vector_extended(inst, FPOperation::Sqrt, 64);
        case Op::FPVectorToSignedFixed16: return fp_vector_extended(inst, FPOperation::ToFixed, 16, 16, false);
        case Op::FPVectorToSignedFixed64: return fp_vector_extended(inst, FPOperation::ToFixed, 64, 64, false);
        case Op::FPVectorToUnsignedFixed16: return fp_vector_extended(inst, FPOperation::ToFixed, 16, 16, true);
        case Op::FPVectorToUnsignedFixed64: return fp_vector_extended(inst, FPOperation::ToFixed, 64, 64, true);
        case Op::FPVectorFromSignedFixed64: return fp_vector_extended(inst, FPOperation::FromFixed, 64, 64, false);
        case Op::FPVectorFromUnsignedFixed64: return fp_vector_extended(inst, FPOperation::FromFixed, 64, 64, true);
        case Op::FPVectorPairedAdd32: return fp_vector_extended(inst, FPOperation::Add, 32, 32, false, true, false);
        case Op::FPVectorPairedAdd64: return fp_vector_extended(inst, FPOperation::Add, 64, 64, false, true, false);
        case Op::FPVectorPairedAddLower32: return fp_vector_extended(inst, FPOperation::Add, 32, 32, false, true, true);
        case Op::FPVectorPairedAddLower64: return fp_vector_extended(inst, FPOperation::Add, 64, 64, false, true, true);
        case Op::FPVectorFromHalf32: return fp_vector_extended(inst, FPOperation::Convert, 16, 32);
        case Op::FPVectorToHalf32: return fp_vector_extended(inst, FPOperation::Convert, 32, 16);
        case Op::FPVectorNeg16:
        case Op::FPVectorNeg64:
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word);
                imm(kind == Op::FPVectorNeg16 ? 0x80008000 : word % 2 ? 0x80000000 : 0); op(Xor);
                set(next_local + word);
            }
            return ok;
        case Op::FPVectorMul32:
        case Op::FPVectorAdd32:
        case Op::FPVectorSub32: {
            if (state.fast_fp()) {
                const uint8_t lane_op = kind == Op::FPVectorMul32 ? 0x94 : kind == Op::FPVectorSub32 ? 0x93 : 0x92;
                for (unsigned word = 0; word < 4; ++word) {
                    value_word(inst.GetArg(0), word); op(0xbe);
                    value_word(inst.GetArg(1), word); op(0xbe);
                    op(lane_op); op(0xbc); set(next_local + word);
                }
                return ok;
            }
            // Four-lane binary32 add/sub/mul (vadd/vsub/vmul.f32). A32's
            // standard FPSCR (fpcr_controlled=false) means RN, FZ=DN=1:
            // denormals flush to signed zero, and NaNs become the default
            // NaN. The full U128 executes (D-form upper lanes read as zero).
            // As in scalar FP, f64 detects inexactness and pre-rounding
            // tininess; reverse sums catch lost addends in add/sub. A tiny
            // nonzero result is flushed with UFC but without IXC.
            if (!inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetU1() != 0)
                return fp_vector_extended(inst, kind == Op::FPVectorMul32 ? FPOperation::Mul : kind == Op::FPVectorSub32 ? FPOperation::Sub : FPOperation::Add, 32);
            const bool multiply = kind == Op::FPVectorMul32;
            const bool subtract = kind == Op::FPVectorSub32;
            const uint8_t narrow_op = multiply ? 0x94 : subtract ? 0x93 : 0x92;
            const uint8_t wide_op = multiply ? 0xa2 : subtract ? 0xa1 : 0xa0;
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const auto la = next_local + 4, lb = next_local + 5;
            const auto laa = next_local + 6, lab = next_local + 7;
            const auto lnan = next_local + 8, lflags = next_local + 9;
            imm(0); set(lflags);
            const auto laccumulate = [&](uint32_t bits) {
                get(lflags); imm(bits); op(Or); set(lflags);
            };
            const auto wide_combine = [&] {
                get(la); op(0xbe); op(0xbb);
                get(lb); op(0xbe); op(0xbb); op(wide_op);
            };
            for (unsigned word = 0; word < 4; ++word) {
                imm(0); set(lnan);
                for (unsigned i = 0; i < 2; ++i) {
                    const auto s = la + i, sab = laa + i;
                    value_word(inst.GetArg(i), word); set(s);
                    get(s); mask(0x7fffffff); set(sab);
                    get(sab); imm(0x00800000); op(LtU);
                    get(sab); op(Eqz); op(Eqz); op(And);
                    begin_if();
                    laccumulate(0x80);
                    get(s); mask(0x80000000); set(s);
                    imm(0); set(sab);
                    end_if();
                    get(sab); imm(0x7f800000); op(GtU);
                    begin_if();
                    imm(1); set(lnan);
                    get(s); mask(0x00400000); op(Eqz);
                    begin_if(); laccumulate(1); end_if();
                    end_if();
                }
                get(lnan); begin_if();
                imm(0x7fc00000); set(next_local + word); // standard DN=1
                op(Else);
                if (multiply) {
                    get(laa); op(Eqz); get(lab); imm(0x7f800000); op(Eq); op(And);
                    get(lab); op(Eqz); get(laa); imm(0x7f800000); op(Eq); op(And); op(Or);
                } else {
                    // inf +/- inf with matching (add: differing, sub: same)
                    // operand signs is invalid.
                    get(laa); imm(0x7f800000); op(Eq);
                    get(lab); imm(0x7f800000); op(Eq); op(And);
                    get(la); get(lb); op(Xor); mask(0x80000000);
                    if (subtract) op(Eqz);
                    else { op(Eqz); op(Eqz); }
                    op(And);
                }
                begin_if();
                laccumulate(1); imm(0x7fc00000); set(next_local + word);
                op(Else);
                get(la); op(0xbe); get(lb); op(0xbe); op(narrow_op);
                op(0xbc); set(next_local + word);
                // Surviving infinite inputs produce exact infinities. Avoid
                // their reverse sums (inf-inf=NaN) and spurious IXC/OFC.
                get(laa); imm(0x7f800000); op(LtU);
                get(lab); imm(0x7f800000); op(LtU); op(And); begin_if();
                wide_combine(); op(0x99); // f64.abs
                imm(0x00800000); op(0xbe); op(0xbb); op(0x63); // < min-normal
                wide_combine(); imm(0); op(0xbe); op(0xbb); op(0x62); // != 0
                op(And); begin_if();
                laccumulate(8); // FZ flush: no additional IXC, even if exact
                get(next_local + word); mask(0x80000000); set(next_local + word);
                op(Else);
                wide_combine(); get(next_local + word); op(0xbe); op(0xbb); op(0x62);
                if (!multiply) {
                    // If the exponent gap exceeds binary64 precision, even the
                    // wide result can lose a nonzero addend: reverse both sums.
                    wide_combine(); get(la); op(0xbe); op(0xbb); op(0xa1);
                    get(lb); op(0xbe); op(0xbb); if (subtract) op(0x9a);
                    op(0x62); op(Or);
                    wide_combine(); get(lb); op(0xbe); op(0xbb); op(subtract ? 0xa0 : 0xa1);
                    get(la); op(0xbe); op(0xbb); op(0x62); op(Or);
                }
                begin_if();
                laccumulate(0x10);
                get(next_local + word); mask(0x7fffffff); imm(0x7f800000); op(Eq);
                begin_if(); laccumulate(4); end_if();
                end_if(); end_if(); end_if();
                end_if(); end_if();
            }
            get(0); load(offsetof(JitState, fpscr)); get(lflags); op(Or);
            store(offsetof(JitState, fpscr));
            return ok;
        }
        case Op::FPVectorRecipEstimate32:
        case Op::FPVectorRecipStepFused32:
        case Op::FPVectorRSqrtEstimate32:
        case Op::FPVectorRSqrtStepFused32: {
            // ARM vector reciprocal (square root) estimates and steps
            // (vrecpe/vrecps/vrsqrte/vrsqrts .f32), one
            // helper call per lane. The A32 translator passes
            // fpcr_controlled=false for both, so the estimate always runs
            // under the standard FPSCR value; the native helper re-derives
            // every result from the vendored Dynarmic FP implementation.
            // Like FPAdd64 and friends, live exception enables must be clear.
            const bool fused = kind == Op::FPVectorRecipStepFused32 || kind == Op::FPVectorRSqrtStepFused32;
            const auto control = inst.GetArg(fused ? 2 : 1);
            if (!control.IsImmediate() || control.GetType() != Type::U1 || control.GetU1())
                return fp_vector_extended(inst, kind == Op::FPVectorRecipEstimate32 ? FPOperation::RecipEstimate : kind == Op::FPVectorRecipStepFused32 ? FPOperation::RecipStep : kind == Op::FPVectorRSqrtEstimate32 ? FPOperation::RSqrtEstimate : FPOperation::RSqrtStep, 32);
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            // fp64.h contract for operations 4/5: the a lane rides in the
            // low 32 bits of memory_value[0] and the b lane in memory_value[2]
            // (the two i64 operand words the scalar ops pack). The b lane
            // is republished per lane: the helper overwrites memory_value[0]
            // with the result bits on every call, and lanes generally differ.
            for (unsigned word = 0; word < 4; ++word) {
                if (fused) {
                    get(0); value_word(inst.GetArg(1), word);
                    store(offsetof(JitState, memory_value) + 8);
                }
                get(0); value_word(inst.GetArg(0), word);
                store(offsetof(JitState, memory_value));
                get(0);
                imm(kind == Op::FPVectorRecipEstimate32 ? 4 : kind == Op::FPVectorRecipStepFused32 ? 5
                    : kind == Op::FPVectorRSqrtEstimate32 ? 8 : 9);
                load(offsetof(JitState, fpscr));
                op(Call); uleb(code, 2); set(next_local + word);
                // OR the returned cumulative-flag bits into FPSCR.
                get(0); load(offsetof(JitState, fpscr));
                get(next_local + word); imm(0x000000ffu); op(And);
                op(Or); store(offsetof(JitState, fpscr));
                // Collect the result lane from the helper's scratch words.
                load(offsetof(JitState, memory_value)); set(next_local + word);
            }
            return ok;
        }
        case Op::FPVectorToSignedFixed32:
        case Op::FPVectorToUnsignedFixed32: {
            // ARM vector float-to-int VCVT (vcvt.s32/u32.f32, fixed-point
            // #fbits) and VCVT{A,N,P,M} (v8), one helper call per lane. The
            // A32 translator emits fpcr_controlled=false for these, so the
            // conversion always runs under the standard FPSCR value; the
            // native helper implements the ARM ARM FPToFixed (operations 6/7
            // towards zero without fraction bits, 11/12 with the rounding
            // mode and fraction bits in memory_value[2]; memory_value[3] is
            // not read and may be stale). Any other immediate shape rejects
            // the block, as does any live exception enable.
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() > 32
                || !inst.GetArg(2).IsImmediate() || inst.GetArg(2).GetU8() > 4
                || !inst.GetArg(3).IsImmediate() || inst.GetArg(3).GetU1() != 0)
                return fp_vector_extended(inst, FPOperation::ToFixed, 32, 32, kind == Op::FPVectorToUnsignedFixed32);
            const uint8_t fbits = inst.GetArg(1).GetU8();
            const uint8_t rounding = inst.GetArg(2).GetU8();
            const bool towards_zero = rounding == 3 && fbits == 0;
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            // fp64.h contract for operations 6/7/11/12: the lane rides in the
            // low 32 bits of memory_value[0] (the first i64 operand word the
            // scalar ops pack). The helper overwrites memory_value[0] with
            // the result bits on every call, so the lane is republished
            // per lane exactly like the reciprocal estimates.
            if (!towards_zero) store_constant(offsetof(JitState, memory_value) + 8, rounding | uint32_t(fbits) << 8);
            for (unsigned word = 0; word < 4; ++word) {
                get(0); value_word(inst.GetArg(0), word);
                store(offsetof(JitState, memory_value));
                get(0);
                const bool is_signed = kind == Op::FPVectorToSignedFixed32;
                imm(towards_zero ? (is_signed ? 6 : 7) : (is_signed ? 11 : 12));
                load(offsetof(JitState, fpscr));
                op(Call); uleb(code, 2); set(next_local + word);
                // OR the returned cumulative-flag bits into FPSCR.
                get(0); load(offsetof(JitState, fpscr));
                get(next_local + word); imm(0x000000ffu); op(And);
                op(Or); store(offsetof(JitState, fpscr));
                // Collect the result lane from the helper's scratch words.
                load(offsetof(JitState, memory_value)); set(next_local + word);
            }
            return ok;
        }
        case Op::FPAdd64:
        case Op::FPSub64:
        case Op::FPMul64:
        case Op::FPDiv64: {
            if (state.fast_fp() && !start.FPSCR().FTZ() && !(start.FPSCR().Value() & 0x00c00000u)) {
                const auto wide = [&](const Value &v) {
                    value_word(v, 0); op(ExtendU);
                    value_word(v, 1); op(ExtendU); constant64(code, 32); op(Shl64); op(Or64);
                    op(0xbf); // f64.reinterpret_i64
                };
                wide(inst.GetArg(0)); wide(inst.GetArg(1));
                op(kind == Op::FPAdd64 ? 0xa0 : kind == Op::FPSub64 ? 0xa1 : kind == Op::FPMul64 ? 0xa2 : 0xa3);
                op(0xbd); // i64.reinterpret_f64
                set(scratch_local);
                get(scratch_local); op(Wrap); set(next_local);
                get(scratch_local); constant64(code, 32); op(ShrU64); op(Wrap); set(next_local + 1);
                return ok;
            }
            // Exact integer arithmetic in a native Wasm helper, not a JS or
            // interpreter fallback. The helper observes only memory_value and
            // the explicitly supplied FPSCR; cached registers/flags stay local.
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            for (unsigned operand = 0; operand < 2; ++operand)
                for (unsigned word = 0; word < 2; ++word) {
                    get(0); value_word(inst.GetArg(operand), word);
                    store(offsetof(JitState, memory_value) + 4 * (operand * 2 + word));
                }
            get(0);
            imm(kind == Op::FPAdd64 ? 0 : kind == Op::FPSub64 ? 1 : kind == Op::FPMul64 ? 2 : 3);
            load(offsetof(JitState, fpscr));
            op(Call); uleb(code, 2); set(next_local + 2);
            get(0); load(offsetof(JitState, fpscr)); get(next_local + 2); op(Or);
            store(offsetof(JitState, fpscr));
            load(offsetof(JitState, memory_value)); set(next_local);
            load(offsetof(JitState, memory_value) + 4); set(next_local + 1);
            return ok;
        }
        case Op::FPAdd32:
        case Op::FPSub32:
        case Op::FPMul32:
        case Op::FPDiv32: {
            const bool division = kind == Op::FPDiv32;
            const bool multiply = kind == Op::FPMul32;
            const bool subtract = kind == Op::FPSub32;
            const uint8_t narrow_op = division ? 0x95 : multiply ? 0x94 : subtract ? 0x93 : 0x92;
            const uint8_t wide_op = division ? 0xa3 : multiply ? 0xa2 : subtract ? 0xa1 : 0xa0;
            // Wasm arithmetic rounds to nearest-even. Other guest rounding
            // modes use the exact arithmetic helper.
            if (start.FPSCR().Value() & 0x00c00000u)
                return fp_extended(inst, division ? FPOperation::Div : multiply ? FPOperation::Mul : subtract ? FPOperation::Sub : FPOperation::Add, 32);
            if (state.fast_fp() && !start.FPSCR().FTZ()) {
                arg(0); op(0xbe); arg(1); op(0xbe); op(narrow_op); op(0xbc); set(next_local);
                return ok;
            }
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const auto a = next_local + 1, b = next_local + 2;
            const auto aa = next_local + 3, ab = next_local + 4;
            const auto flags = next_local + 5, nan = next_local + 6;
            imm(0); set(flags); imm(0); set(nan);
            const auto accumulate = [&](uint32_t bits) {
                get(flags); imm(bits); op(Or); set(flags);
            };
            for (unsigned i = 0; i < 2; ++i) {
                arg(i); set(a + i);
                get(a + i); mask(0x7fffffff); set(aa + i);
                if (start.FPSCR().FTZ()) {
                    get(aa + i); imm(0x00800000); op(LtU);
                    get(aa + i); op(Eqz); op(Eqz); op(And); begin_if();
                    accumulate(0x80); // input denormal flushed to signed zero
                    get(a + i); mask(0x80000000); set(a + i);
                    imm(0); set(aa + i); end_if();
                }
            }
            // ARM NaN priority: first signaling operand, then first quiet
            // operand. Reverse iteration lets the first of each class win.
            for (bool signaling : {false, true}) for (int i = 1; i >= 0; --i) {
                get(aa + i); imm(0x7f800000); op(GtU);
                if (signaling) { get(a + i); mask(0x00400000); op(Eqz); op(And); }
                begin_if();
                get(a + i); imm(0x00400000); op(Or); set(nan);
                if (signaling) accumulate(1);
                end_if();
            }
            get(nan); begin_if();
            if (start.FPSCR().DN()) imm(0x7fc00000);
            else get(nan);
            set(next_local);
            op(Else);
            if (division) {
                get(aa); op(Eqz); get(ab); op(Eqz); op(And);
                get(aa); imm(0x7f800000); op(Eq);
                get(ab); imm(0x7f800000); op(Eq); op(And); op(Or);
            } else if (multiply) {
                get(aa); op(Eqz); get(ab); imm(0x7f800000); op(Eq); op(And);
                get(ab); op(Eqz); get(aa); imm(0x7f800000); op(Eq); op(And); op(Or);
            } else {
                get(aa); imm(0x7f800000); op(Eq);
                get(ab); imm(0x7f800000); op(Eq); op(And);
                get(a); get(b); op(Xor); mask(0x80000000);
                if (subtract) op(Eqz);
                else { op(Eqz); op(Eqz); }
                op(And);
            }
            begin_if(); // invalid finite/infinity combination
            accumulate(1); imm(0x7fc00000); set(next_local);
            op(Else);
            get(a); op(0xbe); get(b); op(0xbe); op(narrow_op);
            op(0xbc); set(next_local); // publish result bits to an i32 SSA slot
            if (division) {
                get(ab); op(Eqz);
                get(aa); imm(0x7f800000); op(LtU); op(And);
            } else imm(0);
            begin_if();
            accumulate(2); // finite nonzero / zero: DZC
            op(Else);
            get(aa); imm(0x7f800000); op(LtU);
            get(ab); imm(0x7f800000); op(LtU); op(And); begin_if();
            // Every binary32 product is exact in binary64; binary64 division
            // also distinguishes exact from inexact binary32 quotients.
            // Addition/subtraction need a lost-addend check (below).
            const auto wide_result = [&] {
                get(a); op(0xbe); op(0xbb);
                get(b); op(0xbe); op(0xbb); op(wide_op);
            };
            wide_result(); op(0x99); // f64.abs
            imm(0x00800000); op(0xbe); op(0xbb); op(0x63); // f64.lt min-normal
            wide_result(); imm(0); op(0xbe); op(0xbb); op(0x62);
            op(And); set(next_local + 7);
            if (start.FPSCR().FTZ()) {
                get(next_local + 7); begin_if();
                accumulate(8); // FZ underflow does not additionally raise IXC
                get(next_local); mask(0x80000000); set(next_local);
                op(Else);
            }
            wide_result(); get(next_local); op(0xbe); op(0xbb); op(0x62);
            if (!division && !multiply) {
                // If the exponent gap exceeds binary64 precision, even the
                // wide result can lose a nonzero addend. Reverse both sums
                // to detect that case instead of missing FPSCR.IXC.
                wide_result(); get(a); op(0xbe); op(0xbb); op(0xa1);
                get(b); op(0xbe); op(0xbb); if (subtract) op(0x9a);
                op(0x62); op(Or);
                wide_result(); get(b); op(0xbe); op(0xbb); op(subtract ? 0xa0 : 0xa1);
                get(a); op(0xbe); op(0xbb); op(0x62); op(Or);
            }
            begin_if();
            accumulate(0x10);
            get(next_local + 7); begin_if(); accumulate(8); end_if();
            get(next_local); mask(0x7fffffff); imm(0x7f800000); op(Eq);
            begin_if(); accumulate(4); end_if();
            end_if();
            if (start.FPSCR().FTZ()) end_if();
            end_if(); end_if(); end_if(); end_if();
            get(0); load(offsetof(JitState, fpscr)); get(flags); op(Or); store(offsetof(JitState, fpscr));
            return ok;
        }
        case Op::FPSqrt32: {
            // Scalar binary32 square root (vsqrts). Wasm f32.sqrt is RN-exact,
            // so only RN locations emit; FTZ/DN follow the location FPSCR
            // exactly like the scalar arithmetic above, and live exception
            // enables must be clear. Negative nonzero inputs raise IOC with
            // the default NaN; inexact results raise IXC (plus UFC when the
            // result is tiny — overflow is impossible for a square root).
            // Exactness via the f64 square: w*w holds exactly for a 24-bit w.
            if (start.FPSCR().Value() & 0x00c00000u) return fp_extended(inst, FPOperation::Sqrt, 32);
            if (state.fast_fp() && !start.FPSCR().FTZ()) {
                arg(0); op(0xbe); op(0x91); op(0xbc); set(next_local);
                return ok;
            }
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const auto sa = next_local + 1, saa = next_local + 2;
            const auto sflags = next_local + 3, snan = next_local + 4;
            imm(0); set(sflags);
            arg(0); set(sa);
            get(sa); mask(0x7fffffff); set(saa);
            if (start.FPSCR().FTZ()) {
                get(saa); imm(0x00800000); op(LtU);
                get(saa); op(Eqz); op(Eqz); op(And); begin_if();
                get(sflags); imm(0x80); op(Or); set(sflags);
                get(sa); mask(0x80000000); set(sa);
                imm(0); set(saa); end_if();
            }
            get(saa); imm(0x7f800000); op(GtU); begin_if();
            get(sa); imm(0x00400000); op(Or); set(snan);
            get(sa); mask(0x00400000); op(Eqz); begin_if();
            get(sflags); imm(1); op(Or); set(sflags); end_if();
            if (start.FPSCR().DN()) imm(0x7fc00000);
            else get(snan);
            set(next_local);
            op(Else);
            get(sa); mask(0x80000000); begin_if(); // negative input
            get(saa); op(Eqz); begin_if();
            get(sa); set(next_local); // sqrt(-0) = -0, exact
            op(Else);
            get(sflags); imm(1); op(Or); set(sflags);
            imm(0x7fc00000); set(next_local); end_if();
            op(Else);
            get(sa); op(0xbe); op(0x91); // f32.sqrt
            op(0xbc); set(next_local);
            // Inexact iff the exact f64 square of the result misses the input.
            get(next_local); op(0xbe); op(0xbb);
            get(next_local); op(0xbe); op(0xbb); op(0xa2); // w*w exact
            get(sa); op(0xbe); op(0xbb); op(0x62); // f64.ne
            begin_if();
            get(sflags); imm(0x10); op(Or); set(sflags);
            get(next_local); mask(0x7fffffff); imm(0x00800000); op(LtU);
            get(next_local); mask(0x7fffffff); op(Eqz); op(Eqz); op(And);
            begin_if();
            get(sflags); imm(8); op(Or); set(sflags); end_if();
            end_if();
            end_if(); end_if();
            get(0); load(offsetof(JitState, fpscr)); get(sflags); op(Or);
            store(offsetof(JitState, fpscr));
            return ok;
        }
        case Op::FPNeg32: arg(0); imm(0x80000000); op(Xor); break;
        case Op::FPAbs32: arg(0); mask(0x7fffffff); break;
        case Op::FPNeg16: arg(0); imm(0x8000); op(Xor); break;
        case Op::FPAbs16: arg(0); mask(0x7fff); break;
        case Op::FPMax32: return fp_extended(inst, FPOperation::Max, 32);
        case Op::FPMax64: return fp_extended(inst, FPOperation::Max, 64);
        case Op::FPMin32: return fp_extended(inst, FPOperation::Min, 32);
        case Op::FPMin64: return fp_extended(inst, FPOperation::Min, 64);
        case Op::FPMaxNumeric32: return fp_extended(inst, FPOperation::MaxNumeric, 32);
        case Op::FPMaxNumeric64: return fp_extended(inst, FPOperation::MaxNumeric, 64);
        case Op::FPMinNumeric32: return fp_extended(inst, FPOperation::MinNumeric, 32);
        case Op::FPMinNumeric64: return fp_extended(inst, FPOperation::MinNumeric, 64);
        case Op::FPMulX32: return fp_extended(inst, FPOperation::MulX, 32);
        case Op::FPMulX64: return fp_extended(inst, FPOperation::MulX, 64);
        case Op::FPMulAdd16: return fp_extended(inst, FPOperation::MulAdd, 16);
        case Op::FPMulAdd32: return fp_extended(inst, FPOperation::MulAdd, 32);
        case Op::FPMulAdd64: return fp_extended(inst, FPOperation::MulAdd, 64);
        case Op::FPMulSub16: return fp_extended(inst, FPOperation::MulSub, 16);
        case Op::FPMulSub32: return fp_extended(inst, FPOperation::MulSub, 32);
        case Op::FPMulSub64: return fp_extended(inst, FPOperation::MulSub, 64);
        case Op::FPRecipEstimate16: return fp_extended(inst, FPOperation::RecipEstimate, 16);
        case Op::FPRecipEstimate32: return fp_extended(inst, FPOperation::RecipEstimate, 32);
        case Op::FPRecipEstimate64: return fp_extended(inst, FPOperation::RecipEstimate, 64);
        case Op::FPRecipExponent16: return fp_extended(inst, FPOperation::RecipExponent, 16);
        case Op::FPRecipExponent32: return fp_extended(inst, FPOperation::RecipExponent, 32);
        case Op::FPRecipExponent64: return fp_extended(inst, FPOperation::RecipExponent, 64);
        case Op::FPRecipStepFused16: return fp_extended(inst, FPOperation::RecipStep, 16);
        case Op::FPRecipStepFused32: return fp_extended(inst, FPOperation::RecipStep, 32);
        case Op::FPRecipStepFused64: return fp_extended(inst, FPOperation::RecipStep, 64);
        case Op::FPRSqrtEstimate16: return fp_extended(inst, FPOperation::RSqrtEstimate, 16);
        case Op::FPRSqrtEstimate32: return fp_extended(inst, FPOperation::RSqrtEstimate, 32);
        case Op::FPRSqrtEstimate64: return fp_extended(inst, FPOperation::RSqrtEstimate, 64);
        case Op::FPRSqrtStepFused16: return fp_extended(inst, FPOperation::RSqrtStep, 16);
        case Op::FPRSqrtStepFused32: return fp_extended(inst, FPOperation::RSqrtStep, 32);
        case Op::FPRSqrtStepFused64: return fp_extended(inst, FPOperation::RSqrtStep, 64);
        case Op::FPRoundInt16: return fp_extended(inst, FPOperation::RoundInt, 16);
        case Op::FPRoundInt32: return fp_extended(inst, FPOperation::RoundInt, 32);
        case Op::FPRoundInt64: return fp_extended(inst, FPOperation::RoundInt, 64);
        case Op::FPHalfToDouble: return fp_extended(inst, FPOperation::Convert, 16, 64);
        case Op::FPSingleToHalf: return fp_extended(inst, FPOperation::Convert, 32, 16);
        case Op::FPDoubleToHalf: return fp_extended(inst, FPOperation::Convert, 64, 16);
        case Op::FPHalfToFixedS16: return fp_extended(inst, FPOperation::ToFixed, 16, 16, false);
        case Op::FPHalfToFixedU16: return fp_extended(inst, FPOperation::ToFixed, 16, 16, true);
        case Op::FPHalfToFixedS32: return fp_extended(inst, FPOperation::ToFixed, 16, 32, false);
        case Op::FPHalfToFixedU32: return fp_extended(inst, FPOperation::ToFixed, 16, 32, true);
        case Op::FPHalfToFixedS64: return fp_extended(inst, FPOperation::ToFixed, 16, 64, false);
        case Op::FPHalfToFixedU64: return fp_extended(inst, FPOperation::ToFixed, 16, 64, true);
        case Op::FPSingleToFixedS64: return fp_extended(inst, FPOperation::ToFixed, 32, 64, false);
        case Op::FPSingleToFixedU64: return fp_extended(inst, FPOperation::ToFixed, 32, 64, true);
        case Op::FPDoubleToFixedS64: return fp_extended(inst, FPOperation::ToFixed, 64, 64, false);
        case Op::FPDoubleToFixedU64: return fp_extended(inst, FPOperation::ToFixed, 64, 64, true);
        case Op::FPNeg64:
        case Op::FPAbs64:
            // Sign-bit operations on the two binary64 words; NaN payloads and
            // signed zeros are preserved exactly like FPNeg32/FPAbs32.
            value_word(inst.GetArg(0), 0); set(next_local);
            value_word(inst.GetArg(0), 1);
            if (kind == Op::FPNeg64) { imm(0x80000000u); op(Xor); }
            else mask(0x7fffffffu);
            set(next_local + 1);
            return ok;
        case Op::FPFixedU32ToDouble: return fp_fixed_to_float(inst, 32, 64, false);
        case Op::FPFixedS32ToDouble: return fp_fixed_to_float(inst, 32, 64, true);
        case Op::FPFixedS16ToSingle: return fp_fixed_to_float(inst, 16, 32, true);
        case Op::FPFixedU16ToSingle: return fp_fixed_to_float(inst, 16, 32, false);
        case Op::FPFixedS16ToDouble: return fp_fixed_to_float(inst, 16, 64, true);
        case Op::FPFixedU16ToDouble: return fp_fixed_to_float(inst, 16, 64, false);
        case Op::FPFixedS64ToSingle: return fp_fixed_to_float(inst, 64, 32, true);
        case Op::FPFixedU64ToSingle: return fp_fixed_to_float(inst, 64, 32, false);
        case Op::FPFixedS64ToDouble: return fp_fixed_to_float(inst, 64, 64, true);
        case Op::FPFixedU64ToDouble: return fp_fixed_to_float(inst, 64, 64, false);
        case Op::FPSingleToFixedS32: return fp_to_fixed(inst, true, false);
        case Op::FPSingleToFixedU32: return fp_to_fixed(inst, false, false);
        case Op::FPDoubleToFixedS32: return fp_to_fixed(inst, true, true);
        case Op::FPDoubleToFixedU32: return fp_to_fixed(inst, false, true);
        case Op::FPSingleToFixedS16: return fp_to_fixed(inst, true, false, 16);
        case Op::FPSingleToFixedU16: return fp_to_fixed(inst, false, false, 16);
        case Op::FPDoubleToFixedS16: return fp_to_fixed(inst, true, true, 16);
        case Op::FPDoubleToFixedU16: return fp_to_fixed(inst, false, true, 16);
        case Op::FPSingleToDouble: {
            // Widening binary32 to binary64 is exact for every finite input,
            // so the rounding mode cannot change the result. FZ flushes a
            // denormal input to a signed zero and raises IDC; a signaling NaN
            // raises IOC and is quieted with its payload widened explicitly
            // instead of relying on the host's NaN propagation.
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() != 0) return fp_extended(inst, FPOperation::Convert, 32, 64);
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const auto a = next_local + 2, flags = next_local + 3;
            value_word(inst.GetArg(0)); set(a); // binary32 bits
            imm(0); set(flags);
            if (start.FPSCR().FTZ()) {
                get(a); mask(0x7fffffff); imm(0x00800000); op(LtU);
                get(a); mask(0x7fffffff); op(Eqz); op(Eqz); op(And);
                begin_if();
                get(flags); imm(0x80); op(Or); set(flags);
                get(a); mask(0x80000000); set(a);
                end_if();
            }
            get(a); mask(0x7fffffff); imm(0x7f800000); op(GtU);
            begin_if(); // NaN input
            get(a); mask(0x00400000); op(Eqz);
            begin_if(); get(flags); imm(1); op(Or); set(flags); end_if();
            if (start.FPSCR().DN()) {
                constant64(code, 0x7ff8000000000000LL);
            } else {
                // sign | quieted payload widened from binary32
                get(a); op(ExtendU);
                constant64(code, 31); op(ShrU64); // sign bit 0/1
                constant64(code, 63); op(Shl64);
                constant64(code, 0x7ff8000000000000LL); op(Or64);
                get(a); op(ExtendU); constant64(code, 0x007fffff); op(0x83); // i64.and
                constant64(code, 29); op(Shl64); op(Or64);
            }
            store_i64_words(next_local);
            op(Else);
            get(a); op(0xbe); // f32.reinterpret_i32
            op(0xbb); // f64.promote_f32
            store_f64_words(next_local);
            end_if();
            get(0); load(offsetof(JitState, fpscr)); get(flags); op(Or); store(offsetof(JitState, fpscr));
            return ok;
        }
        case Op::FPHalfToSingle: {
            // VCVTB/VCVTT.F32.F16, Dynarmic FPConvert<u32, u16>: every half
            // value is exactly representable, so the rounding operand is
            // irrelevant. FZ16 is outside the A32 location's FPSCR mode mask
            // (always clear): half subnormals convert exactly and never raise
            // IDC. With AHP, exponent 31 is an ordinary binade (no NaN or
            // infinity). Only a signaling NaN raises a flag (IOC).
            if (!inst.GetArg(1).IsImmediate()) return false;
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const auto h = next_local + 4, fraction = next_local + 5;
            value_word(inst.GetArg(0)); mask(0xffff); set(h);
            get(h); mask(0x3ff); set(fraction);
            // Normal (and AHP exponent-31) inputs: rebias 15 -> 127.
            get(h); mask(0x8000); imm(16); op(Shl);
            get(h); mask(0x7fff); imm(13); op(Shl); imm(112u << 23); op(Add);
            op(Or); set(next_local);
            get(h); mask(0x7c00); op(Eqz);
            begin_if();
            get(h); mask(0x8000); imm(16); op(Shl); set(next_local); // signed zero
            get(fraction);
            begin_if(); // subnormal: 2^-24 * fraction, normalized
            get(next_local);
            imm(134); get(fraction); op(Clz); op(Sub); imm(23); op(Shl); op(Or);
            get(fraction); get(fraction); op(Clz); imm(8); op(Sub); op(Shl); mask(0x007fffffu); op(Or);
            set(next_local);
            end_if();
            end_if();
            if (!start.FPSCR().AHP()) {
                get(h); mask(0x7c00); imm(0x7c00); op(Eq);
                begin_if();
                get(h); mask(0x8000); imm(16); op(Shl); imm(0x7f800000u); op(Or); set(next_local);
                get(fraction);
                begin_if(); // NaN: IOC when signaling, then default or quieted payload
                get(fraction); mask(0x200); op(Eqz);
                begin_if();
                get(0); load(offsetof(JitState, fpscr)); imm(1); op(Or); store(offsetof(JitState, fpscr));
                end_if();
                if (start.FPSCR().DN()) {
                    imm(0x7fc00000u);
                } else {
                    get(h); mask(0x8000); imm(16); op(Shl); imm(0x7fc00000u); op(Or);
                    get(fraction); mask(0x1ff); imm(13); op(Shl); op(Or);
                }
                set(next_local);
                end_if();
                end_if();
            }
            return ok;
        }
        case Op::FPDoubleToSingle: {
            // Narrowing conversion. Only round-to-nearest is emitted (wasm's
            // f32.demote_f64); every flag below is derived from the exact
            // binary64 operand rather than approximated.
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() != 0) return fp_extended(inst, FPOperation::Convert, 64, 32);
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const auto a = next_local + 2, flags = next_local + 4;
            const auto result32 = next_local + 5, inexact = next_local + 6;
            const auto overflow = next_local + 7, underflow = next_local + 8, flushed = next_local + 9;
            value_word(inst.GetArg(0), 0); set(a);
            value_word(inst.GetArg(0), 1); set(a + 1);
            imm(0); set(flags);
            if (start.FPSCR().FTZ()) {
                f64_words_are_subnormal(a, result32);
                get(result32); begin_if();
                get(flags); imm(0x80); op(Or); set(flags);
                get(a + 1); mask(0x80000000); set(a + 1);
                imm(0); set(a);
                end_if();
            }
            f64_words_are_nan(a, result32);
            get(result32); begin_if();
            // A signaling NaN raises IOC; the result is a quiet NaN.
            get(a + 1); mask(0x00080000); op(Eqz);
            begin_if(); get(flags); imm(1); op(Or); set(flags); end_if();
            if (start.FPSCR().DN()) {
                imm(0x7fc00000);
            } else {
                push_i64_words(a); constant64(code, 29); op(ShrU64); op(Wrap); mask(0x003fffff);
                get(a + 1); mask(0x80000000); op(Or); imm(0x7fc00000); op(Or);
            }
            set(next_local);
            op(Else);
            push_f64_words(a); op(0xb6); op(0xbc); set(result32); // f32.demote_f64
            get(result32); op(0xbe); op(0xbb); // widen the rounded result
            push_f64_words(a); op(0x62); set(inexact); // f64.ne
            get(result32); mask(0x7fffffff); imm(0x7f800000); op(Eq);
            get(a + 1); mask(0x7fffffff); imm(0x7ff00000); op(Eq);
            get(a); op(Eqz); op(And); op(Eqz);
            op(And); set(overflow);
            // Tininess is decided against the exact operand (binary64 holds
            // it losslessly), so no pre-rounding value has to be estimated.
            push_f64_words(a); op(0x99); // f64.abs
            push_f64_imm(0x3810000000000000LL); // 2^-126
            op(0x63); // f64.lt
            set(underflow);
            imm(0); set(flushed);
            if (start.FPSCR().FTZ()) {
                get(underflow);
                get(a); get(a + 1); mask(0x7fffffff); op(Or); op(Eqz); op(Eqz); op(And);
                begin_if();
                get(result32); mask(0x80000000); set(result32);
                imm(1); set(flushed);
                end_if();
            }
            get(flags);
            get(inexact); get(flushed); op(Eqz); op(And); imm(0x10); op(Mul); op(Or); set(flags);
            // Underflow needs tininess plus either inexactness or an FZ flush;
            // a flushed result is not additionally reported as inexact.
            get(flags);
            get(underflow); get(inexact); get(flushed); op(Or); op(And); imm(8); op(Mul); op(Or); set(flags);
            get(flags); get(overflow); imm(4); op(Mul); op(Or); set(flags);
            get(result32); set(next_local);
            end_if();
            get(0); load(offsetof(JitState, fpscr)); get(flags); op(Or); store(offsetof(JitState, fpscr));
            return ok;
        }
        case Op::FPCompare64: {
            // Same bit-pattern comparison as FPCompare32, on two binary64
            // words: no host FP, so signaling NaNs, signed zeros and FZ
            // flushing stay exact. Exception enables are not in the location
            // key, so the live FPSCR is checked instead.
            if (!inst.GetArg(2).IsImmediate()) return false;
            load(offsetof(JitState, fpscr)); mask(0x00009f00u);
            begin_if(); ret(ExitReason::Unsupported); end_if();
            const bool signal = inst.GetArg(2).GetU1();
            imm(0); set(next_local + 5); // any NaN
            imm(0); set(next_local + 6); // invalid-operation cumulative bit
            for (unsigned i = 0; i < 2; ++i) {
                const auto slot = next_local + 1 + i * 2;
                value64(inst.GetArg(i));
                store_i64_words(slot);
                f64_words_are_nan(slot, next_local + 7);
                get(next_local + 7); begin_if();
                imm(1); set(next_local + 5);
                get(next_local + 6);
                if (signal) imm(1);
                else { get(slot + 1); mask(0x00080000); op(Eqz); }
                op(Or); set(next_local + 6);
                end_if();
                if (start.FPSCR().FTZ()) {
                    f64_words_are_subnormal(slot, next_local + 8);
                    get(next_local + 8); begin_if();
                    get(0); load(offsetof(JitState, fpscr)); imm(0x80); op(Or); store(offsetof(JitState, fpscr));
                    get(slot + 1); mask(0x80000000); set(slot + 1);
                    imm(0); set(slot);
                    end_if();
                }
                // Canonicalize both zeros, then build the unsigned sortable
                // key: negatives are complemented, non-negatives get the sign
                // bit set.
                get(slot + 1); mask(0x7fffffff); op(Eqz);
                get(slot); op(Eqz); op(And);
                begin_if(); imm(0); set(slot); imm(0); set(slot + 1); end_if();
                constant64(code, -1);
                constant64(code, INT64_MIN);
                push_i64_words(slot); constant64(code, 63); op(ShrU64); op(Wrap);
                op(Select);
                push_i64_words(slot); op(0x85); // i64.xor
                store_i64_words(slot);
            }
            get(0); load(offsetof(JitState, fpscr)); get(next_local + 6); op(Or); store(offsetof(JitState, fpscr));
            imm(0x30000000); // unordered: C,V
            imm(0x60000000); // equal: Z,C
            imm(0x80000000); // less: N
            imm(0x20000000); // greater: C
            push_i64_words(next_local + 1); push_i64_words(next_local + 3); op(0x54); op(Select); // i64.lt_u
            push_i64_words(next_local + 1); push_i64_words(next_local + 3); op(0x51); op(Select); // i64.eq
            get(next_local + 5); op(Select);
            break;
        }
        case Op::A32GetFpscrNZCV:
            load(offsetof(JitState, fpscr)); mask(0xf0000000); break;
        case Op::A32SetFpscrNZCV:
            get(0); load(offsetof(JitState, fpscr)); mask(0x0fffffff);
            arg(0); mask(0xf0000000); op(Or); store(offsetof(JitState, fpscr));
            return ok;
        case Op::A32GetCpsr: state.read_full_cpsr(code); break;
        case Op::A32ReadMemory8: memory_call(inst, false, 1); return ok;
        case Op::A32ReadMemory16: memory_call(inst, false, 2); return ok;
        case Op::A32ReadMemory32: memory_call(inst, false, 4); return ok;
        case Op::A32ReadMemory64: memory_call(inst, false, 8); return ok;
        case Op::A32WriteMemory8: memory_call(inst, true, 1); return ok;
        case Op::A32WriteMemory16: memory_call(inst, true, 2); return ok;
        case Op::A32WriteMemory32: memory_call(inst, true, 4); return ok;
        case Op::A32WriteMemory64: memory_call(inst, true, 8); return ok;
        case Op::A32ClearExclusive:
            store_constant(offsetof(JitState, exclusive_size), 0);
            return ok;
        case Op::A32ExclusiveReadMemory8: exclusive_read(inst, 1); return ok;
        case Op::A32ExclusiveReadMemory16: exclusive_read(inst, 2); return ok;
        case Op::A32ExclusiveReadMemory32: exclusive_read(inst, 4); return ok;
        case Op::A32ExclusiveReadMemory64: exclusive_read(inst, 8); return ok;
        case Op::A32ExclusiveWriteMemory8: exclusive_write(inst, 1); return ok;
        case Op::A32ExclusiveWriteMemory16: exclusive_write(inst, 2); return ok;
        case Op::A32ExclusiveWriteMemory32: exclusive_write(inst, 4); return ok;
        case Op::A32ExclusiveWriteMemory64: exclusive_write(inst, 8); return ok;
        case Op::A32GetCFlag: flag(29); break;
        case Op::A32OrQFlag:
            // Sticky saturation flag (SMLA<x><y>, SMLAD, QADD...): set CPSR.Q
            // when the U1 argument is 1, never clear it.
            state.write_psr_field(code, 0x08000000u, [&] {
                state.read_dispatch_psr(code); mask(0x08000000u);
                arg(0); imm(27); op(Shl); op(Or);
            });
            return ok;
        case Op::A32SetCpsr:
            state.write_nzcv(code, 0xf0000000, [&] { arg(0); }, [] {});
            state.write_psr_field(code, 0x0fffffff, [&] { arg(0); mask(0x090f03ff); });
            return ok;
        case Op::A32SetCpsrNZCVQ:
            state.write_nzcv(code, 0xf0000000, [&] { arg(0); }, [] {});
            state.write_psr_field(code, 0x08000000, [&] { arg(0); mask(0x08000000); });
            return ok;
        case Op::A32SetGEFlagsCompressed:
            state.write_psr_field(code, 0x000f0000, [&] { arg(0); mask(0x000f0000); });
            return ok;
        case Op::A32SetCpsrNZ:
        case Op::A32SetCpsrNZC:
        case Op::A32SetCpsrNZCV:
        case Op::A32SetCpsrNZCVRaw: {
            const uint32_t bits = kind == Op::A32SetCpsrNZ ? 0xc0000000
                : kind == Op::A32SetCpsrNZC ? 0xe0000000 : 0xf0000000;
            state.write_nzcv(code, bits, [&] { arg(0); }, [&] { arg(1); });
            return ok;
        }
        case Op::GetUpperFromOp:
        case Op::GetLowerFromOp: {
            const auto source = inst.GetArg(0);
            if (source.IsImmediate()) return false;
            const auto *producer = source.GetInstRecursive();
            switch (producer->GetOpcode()) {
            case Op::VectorSignedMultiply16:
            case Op::VectorSignedMultiply32:
            case Op::VectorUnsignedMultiply16:
            case Op::VectorUnsignedMultiply32: break;
            default: return false;
            }
            const auto it = locals.find(producer);
            if (it == locals.end()) return false;
            const unsigned offset = kind == Op::GetUpperFromOp ? 4 : 0;
            for (unsigned word = 0; word < 4; ++word) { get(it->second + offset + word); set(next_local + word); }
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
        case Op::CountLeadingZeros32: arg(0); op(Clz); break;
        case Op::Mul32: arg(0); arg(1); op(Mul); break;
        case Op::PackedSaturatedAddU8: return packed_saturating(inst, 8, false, true);
        case Op::PackedSaturatedSubU8: return packed_saturating(inst, 8, false, false);
        case Op::PackedSaturatedAddS8: return packed_saturating(inst, 8, true, true);
        case Op::PackedSaturatedSubS8: return packed_saturating(inst, 8, true, false);
        case Op::PackedSaturatedAddU16: return packed_saturating(inst, 16, false, true);
        case Op::PackedSaturatedSubU16: return packed_saturating(inst, 16, false, false);
        case Op::PackedSaturatedAddS16: return packed_saturating(inst, 16, true, true);
        case Op::PackedSaturatedSubS16: return packed_saturating(inst, 16, true, false);
        case Op::PackedAddU8: {
            // UADD8: bytewise sums modulo 256. GetGEFromOp reads +4 in
            // Dynarmic's expanded form (EmitPackedAddU8): 0xff in each byte
            // whose sum carried out. That carry is the majority of the two
            // bit-7 inputs and the carry into bit 7.
            const auto a = next_local + 5, b = next_local + 6;
            arg(0); set(a); arg(1); set(b);
            get(a); mask(0x7f7f7f7fu); get(b); mask(0x7f7f7f7fu); op(Add);
            get(a); get(b); op(Xor); mask(0x80808080u); op(Xor); set(next_local);
            if (carry_needed.count(&inst) != 0) {
                get(a); get(b); op(And);
                get(a); get(b); op(Or); get(next_local); imm(UINT32_MAX); op(Xor); op(And);
                op(Or); mask(0x80808080u); imm(7); op(ShrU); imm(0xff); op(Mul);
                set(next_local + 4);
            }
            return ok;
        }
        case Op::PackedAddSubU16: return packed_arithmetic(inst, 16, false, true, false, true);
        case Op::PackedAddSubS16: return packed_arithmetic(inst, 16, true, true, false, true);
        case Op::PackedSubAddU16: return packed_arithmetic(inst, 16, false, false, false, true);
        case Op::PackedSubAddS16: return packed_arithmetic(inst, 16, true, false, false, true);
        case Op::PackedHalvingAddSubU16: return packed_arithmetic(inst, 16, false, true, true, true);
        case Op::PackedHalvingAddSubS16: return packed_arithmetic(inst, 16, true, true, true, true);
        case Op::PackedHalvingSubAddU16: return packed_arithmetic(inst, 16, false, false, true, true);
        case Op::PackedHalvingSubAddS16: return packed_arithmetic(inst, 16, true, false, true, true);
        case Op::PackedAddS8: return packed_arithmetic(inst, 8, true, false, false);
        case Op::PackedSubU8: return packed_arithmetic(inst, 8, false, true, false);
        case Op::PackedSubS8: return packed_arithmetic(inst, 8, true, true, false);
        case Op::PackedAddU16: return packed_arithmetic(inst, 16, false, false, false);
        case Op::PackedAddS16: return packed_arithmetic(inst, 16, true, false, false);
        case Op::PackedSubU16: return packed_arithmetic(inst, 16, false, true, false);
        case Op::PackedSubS16: return packed_arithmetic(inst, 16, true, true, false);
        case Op::PackedHalvingAddU8: return packed_arithmetic(inst, 8, false, false, true);
        case Op::PackedHalvingAddS8: return packed_arithmetic(inst, 8, true, false, true);
        case Op::PackedHalvingSubU8: return packed_arithmetic(inst, 8, false, true, true);
        case Op::PackedHalvingSubS8: return packed_arithmetic(inst, 8, true, true, true);
        case Op::PackedHalvingAddU16: return packed_arithmetic(inst, 16, false, false, true);
        case Op::PackedHalvingAddS16: return packed_arithmetic(inst, 16, true, false, true);
        case Op::PackedHalvingSubU16: return packed_arithmetic(inst, 16, false, true, true);
        case Op::PackedHalvingSubS16: return packed_arithmetic(inst, 16, true, true, true);
        case Op::SignedSaturation: {
            // SSAT (and SSAT16 per halfword): clamp to the signed N-bit
            // range, 1 <= N <= 32; +5 = saturated, for CPSR.Q
            // (Dynarmic EmitSignedSaturation).
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() < 1 || inst.GetArg(1).GetU8() > 32) return false;
            const uint32_t n = inst.GetArg(1).GetU8();
            const auto a = next_local + 6;
            value_word(inst.GetArg(0)); set(a);
            if (n == 32) {
                get(a); set(next_local); imm(0); set(next_local + 5);
                return ok;
            }
            const uint32_t positive = (1u << (n - 1)) - 1;
            get(a); imm(1u << (n - 1)); op(Add); imm((1u << n) - 1); op(GtU); set(next_local + 5);
            get(a); imm(31); op(ShrS); imm(positive); op(Xor);
            get(a);
            get(next_local + 5); op(Select); set(next_local);
            return ok;
        }
        case Op::UnsignedSaturation: {
            // USAT (and USAT16): clamp the signed value to [0, 2^N - 1],
            // N <= 31; +5 = saturated (Dynarmic EmitUnsignedSaturation).
            if (!inst.GetArg(1).IsImmediate() || inst.GetArg(1).GetU8() > 31) return false;
            const uint32_t maximum = (1u << inst.GetArg(1).GetU8()) - 1;
            const auto a = next_local + 6;
            value_word(inst.GetArg(0)); set(a);
            get(a); imm(maximum); op(GtU); set(next_local + 5);
            imm(0); imm(maximum); get(a); imm(0); op(LtS); op(Select);
            get(a);
            get(next_local + 5); op(Select); set(next_local);
            return ok;
        }
        case Op::SignedSaturatedAddWithFlag32:
        case Op::SignedSaturatedSubWithFlag32: {
            // QADD/QSUB (and the doubling of QDADD/QDSUB): the exact i64
            // sum clamped to i32; +5 = saturated, for CPSR.Q.
            value_word(inst.GetArg(0)); op(ExtendS);
            value_word(inst.GetArg(1)); op(ExtendS);
            op(kind == Op::SignedSaturatedAddWithFlag32 ? Add64 : Sub64); set(scratch_local);
            imm(0); set(next_local + 5);
            clamp_i64_low(INT32_MIN, INT32_MAX, next_local + 5);
            set(next_local);
            return ok;
        }
        case Op::A32GetGEFlags:
            // CPSR.GE[3:0] in Dynarmic's expanded form: byte i is 0xff when
            // GE[i] is set. The multiply spreads bit i to bit 8i without
            // carries (the partial products occupy distinct bits).
            state.read_dispatch_psr(code); imm(16); op(ShrU); mask(0xf);
            imm(0x00204081u); op(Mul); mask(0x01010101u); imm(0xff); op(Mul);
            break;
        case Op::PackedAbsDiffSumU8:
            for (unsigned lane = 0; lane < 4; ++lane) {
                vector_element_word(inst.GetArg(0), 8, lane);
                vector_element_word(inst.GetArg(1), 8, lane); op(Sub); set(next_local + 4);
                imm(0); get(next_local + 4); op(Sub); get(next_local + 4);
                get(next_local + 4); imm(0); op(LtS); op(Select);
                if (lane) op(Add);
            }
            break;
        case Op::PackedSelect:
            // SEL: bytes of b where the GE byte mask is set, else of a
            // (Dynarmic EmitPackedSelect(ge, a, b)).
            arg(2); arg(0); op(And);
            arg(1); arg(0); imm(0xffffffffu); op(Xor); op(And);
            op(Or);
            break;
        case Op::A32SetGEFlags:
            // CPSR.GE[i] is bit 8i+7 of the expanded byte-mask form
            // (Dynarmic A32JitState::Cpsr); producers emit 0x00/0xff bytes.
            state.write_psr_field(code, 0x000f0000u, [&] {
                arg(0); imm(9); op(Shl); mask(0x00010000u);
                arg(0); imm(2); op(Shl); mask(0x00020000u); op(Or);
                arg(0); imm(5); op(ShrU); mask(0x00040000u); op(Or);
                arg(0); imm(12); op(ShrU); mask(0x00080000u); op(Or);
            });
            return ok;
        case Op::ByteReverseWord:
            value_word(inst.GetArg(0)); set(next_local + 4);
            byte_reverse_word_from_local(next_local + 4, next_local);
            return ok;
        case Op::ByteReverseHalf:
            // ByteReverseHalf is U16 -> U16. Mask before and after the
            // exchange so the surrounding U32 stack representation cannot
            // leak bits from the producer's unused high half.
            value_word(inst.GetArg(0)); set(next_local + 4);
            get(next_local + 4); mask(0xffff); imm(8); op(Shl);
            get(next_local + 4); mask(0xffff); imm(8); op(ShrU); op(Or);
            mask(0xffff); set(next_local);
            return ok;
        case Op::ByteReverseDual:
            // A U64 SSA value is two little-endian i32 words. Reversing all
            // eight bytes therefore reverses each word and swaps their
            // positions; publish both words explicitly for later consumers.
            value_word(inst.GetArg(0), 1); set(next_local + 4);
            byte_reverse_word_from_local(next_local + 4, next_local);
            value_word(inst.GetArg(0), 0); set(next_local + 4);
            byte_reverse_word_from_local(next_local + 4, next_local + 1);
            return ok;
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
        case Op::SignExtendByteToWord: arg(0); imm(24); op(Shl); imm(24); op(ShrS); break;
        case Op::SignExtendHalfToWord: arg(0); imm(16); op(Shl); imm(16); op(ShrS); break;
        case Op::SignExtendWordToLong:
            value_word(inst.GetArg(0)); set(next_local);
            // U64 SSA slots are two i32 words; replicate the source sign bit.
            value_word(inst.GetArg(0)); imm(31); op(ShrS); set(next_local + 1);
            return ok;
        case Op::SignExtendHalfToLong:
            // U16 source in a word slot (VCVT.S16.F64 fixed-point writeback).
            value_word(inst.GetArg(0)); imm(16); op(Shl); imm(16); op(ShrS); set(next_local);
            get(next_local); imm(31); op(ShrS); set(next_local + 1);
            return ok;
        case Op::ZeroExtendWordToLong:
            value_word(inst.GetArg(0)); set(next_local);
            imm(0); set(next_local + 1); // Region bodies reuse these locals.
            return ok;
        case Op::ZeroExtendByteToLong:
            // U8 source in a word slot; zero high word (VLD1 byte assembly).
            value_word(inst.GetArg(0)); mask(0xff); set(next_local);
            imm(0); set(next_local + 1);
            return ok;
        case Op::ZeroExtendHalfToLong:
            // U16 source in a word slot; zero high word (VLD1.16 assembly).
            value_word(inst.GetArg(0)); mask(0xffff); set(next_local);
            imm(0); set(next_local + 1);
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
            if (!has_bx) {
                upper_location(finish);
                end_mode_written = true;
            }
            return ok;
        case Op::A32BXWritePC:
            upper_location(finish);
            end_mode_written = true;
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
        case Op::VectorLogicalShiftLeft8: return vector_immediate_shift(inst, 8, true, false);
        case Op::VectorLogicalShiftLeft16: return vector_immediate_shift(inst, 16, true, false);
        case Op::VectorLogicalShiftLeft32: return vector_immediate_shift(inst, 32, true, false);
        case Op::VectorLogicalShiftLeft64: return vector_immediate_shift(inst, 64, true, false);
        case Op::VectorLogicalShiftRight8: return vector_immediate_shift(inst, 8, false, false);
        case Op::VectorLogicalShiftRight16: return vector_immediate_shift(inst, 16, false, false);
        case Op::VectorLogicalShiftRight32: return vector_immediate_shift(inst, 32, false, false);
        case Op::VectorLogicalShiftRight64: return vector_immediate_shift(inst, 64, false, false);
        case Op::VectorArithmeticShiftRight8: return vector_immediate_shift(inst, 8, false, true);
        case Op::VectorArithmeticShiftRight16: return vector_immediate_shift(inst, 16, false, true);
        case Op::VectorArithmeticShiftRight32: return vector_immediate_shift(inst, 32, false, true);
        case Op::VectorArithmeticShiftRight64: return vector_immediate_shift(inst, 64, false, true);
        case Op::VectorEqual8: return vector_integer_select(inst, 8, VectorLaneOp::Equal, false);
        case Op::VectorEqual16: return vector_integer_select(inst, 16, VectorLaneOp::Equal, false);
        case Op::VectorEqual32: return vector_integer_select(inst, 32, VectorLaneOp::Equal, false);
        case Op::VectorEqual64:
            // Both words equal -> all-ones lane (VQRSHRN's rounding correction).
            for (unsigned word = 0; word < 4; word += 2) {
                imm(0);
                value_word(inst.GetArg(0), word); value_word(inst.GetArg(1), word); op(Eq);
                value_word(inst.GetArg(0), word + 1); value_word(inst.GetArg(1), word + 1); op(Eq);
                op(And); op(Sub); set(next_local + word);
                get(next_local + word); set(next_local + word + 1);
            }
            return ok;
        case Op::VectorReduceAdd8: return vector_reduce_add(inst, 8);
        case Op::VectorReduceAdd16: return vector_reduce_add(inst, 16);
        case Op::VectorReduceAdd32: return vector_reduce_add(inst, 32);
        case Op::VectorReduceAdd64: return vector_reduce_add(inst, 64);
        case Op::VectorPolynomialMultiplyLong64:
            for (unsigned word = 0; word < 4; ++word) { imm(0); set(next_local + word); }
            for (unsigned bit = 0; bit < 64; ++bit) {
                constant64(code, 0); push_i64_value(inst.GetArg(1), 0);
                constant64(code, bit); op(ShrU64); constant64(code, 1); op(And64); op(Sub64); set(scratch_local);
                // Keep the selection mask in two i32 scratch words because
                // publishing an i64 result reuses the i64 scratch local.
                get(scratch_local); store_i64_words(next_local + 4);
                push_i64_words(next_local); push_i64_value(inst.GetArg(0), 0);
                constant64(code, bit); op(Shl64); push_i64_words(next_local + 4); op(And64); op(Xor64); store_i64_words(next_local);
                if (bit) {
                    push_i64_words(next_local + 2); push_i64_value(inst.GetArg(0), 0);
                    constant64(code, 64 - bit); op(ShrU64); push_i64_words(next_local + 4); op(And64); op(Xor64); store_i64_words(next_local + 2);
                }
            }
            return ok;
        case Op::VectorUnsignedRecipEstimate:
        case Op::VectorUnsignedRecipSqrtEstimate:
            for (unsigned word = 0; word < 4; ++word) {
                const bool square_root = kind == Op::VectorUnsignedRecipSqrtEstimate;
                value_word(inst.GetArg(0), word); imm(square_root ? 0x40000000 : 0x80000000); op(LtU); begin_if(true);
                imm(UINT32_MAX); op(Else);
                value_word(inst.GetArg(0), word); imm(23); op(ShrU); set(next_local + 4);
                if (square_root) {
                    get(next_local + 4); imm(1); op(Shl); imm(1); op(Or);
                    get(next_local + 4); imm(1); op(Or); imm(1); op(Shl);
                    get(next_local + 4); imm(256); op(LtU); op(Select); set(next_local + 4);
                    // The integer estimate is <=1023; binary64 sqrt has ample
                    // precision to distinguish adjacent squared integers.
                    imm(0x0fffffff); op(0xb8); get(next_local + 4); op(0xb8); op(0xa3); op(0x9f); op(0xab);
                } else {
                    imm(1u << 19); get(next_local + 4); imm(1); op(Shl); imm(1); op(Or); op(DivU);
                }
                imm(1); op(Add); imm(1); op(ShrU); mask(0xff); imm(0x100); op(Or); imm(23); op(Shl);
                end_if(); set(next_local + word);
            }
            return ok;
        case Op::VectorPopulationCount:
            pack_lanes(8, 4, [&](unsigned i) { vector_element_word(inst.GetArg(0), 8, i); op(0x69); });
            return ok;
        case Op::VectorReverseBits:
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), word); set(next_local + 4);
                for (unsigned shift : {1u, 2u, 4u}) {
                    const uint32_t m = shift == 1 ? 0x55555555 : shift == 2 ? 0x33333333 : 0x0f0f0f0f;
                    get(next_local + 4); imm(shift); op(ShrU); mask(m);
                    get(next_local + 4); mask(m); imm(shift); op(Shl); op(Or); set(next_local + 4);
                }
                get(next_local + 4); set(next_local + word);
            }
            return ok;
        case Op::VectorRotateWholeVectorRight: {
            if (!inst.GetArg(1).IsImmediate()) return false;
            const unsigned count = inst.GetArg(1).GetU8() % 128;
            for (unsigned word = 0; word < 4; ++word) {
                value_word(inst.GetArg(0), (word + count / 32) % 4);
                if (count % 32) {
                    imm(count % 32); op(ShrU);
                    value_word(inst.GetArg(0), (word + count / 32 + 1) % 4);
                    imm(32 - count % 32); op(Shl); op(Or);
                }
                set(next_local + word);
            }
            return ok;
        }
        case Op::VectorAbs64:
        case Op::VectorEqual128:
        case Op::VectorSignExtend64:
            return vector_simple64(inst, kind);
        case Op::VectorMultiply64: return vector_integer_arithmetic(inst, 64, Mul);
        case Op::VectorBroadcastLower8: return vector_broadcast(inst, 8, false, true);
        case Op::VectorBroadcastLower16: return vector_broadcast(inst, 16, false, true);
        case Op::VectorBroadcastLower32: return vector_broadcast(inst, 32, false, true);
        case Op::VectorBroadcastElementLower8: return vector_broadcast(inst, 8, true, true);
        case Op::VectorBroadcastElementLower16: return vector_broadcast(inst, 16, true, true);
        case Op::VectorBroadcastElementLower32: return vector_broadcast(inst, 32, true, true);
        case Op::VectorCountLeadingZeros8: return vector_count_leading_zeros(inst, 8);
        case Op::VectorCountLeadingZeros16: return vector_count_leading_zeros(inst, 16);
        case Op::VectorCountLeadingZeros32: return vector_count_leading_zeros(inst, 32);
        case Op::VectorPairedAdd8: return vector_paired(inst, 8, false, false, false);
        case Op::VectorPairedAdd16: return vector_paired(inst, 16, false, false, false);
        case Op::VectorPairedAdd64: return vector_paired(inst, 64, false, 0, false);
        case Op::VectorPairedAdd32: return vector_paired(inst, 32, false, false, false);
        case Op::VectorPairedMaxS8: return vector_paired(inst, 8, false, true, true);
        case Op::VectorPairedMaxS16: return vector_paired(inst, 16, false, true, true);
        case Op::VectorPairedMaxS32: return vector_paired(inst, 32, false, true, true);
        case Op::VectorPairedMaxU8: return vector_paired(inst, 8, false, true, false);
        case Op::VectorPairedMaxU16: return vector_paired(inst, 16, false, true, false);
        case Op::VectorPairedMaxU32: return vector_paired(inst, 32, false, true, false);
        case Op::VectorPairedMinS8: return vector_paired(inst, 8, false, 2, true);
        case Op::VectorPairedMinS16: return vector_paired(inst, 16, false, 2, true);
        case Op::VectorPairedMinS32: return vector_paired(inst, 32, false, 2, true);
        case Op::VectorPairedMinU8: return vector_paired(inst, 8, false, 2, false);
        case Op::VectorPairedMinU16: return vector_paired(inst, 16, false, 2, false);
        case Op::VectorPairedMinU32: return vector_paired(inst, 32, false, 2, false);
        case Op::VectorPairedMaxLowerS8: return vector_paired(inst, 8, true, true, true);
        case Op::VectorPairedMaxLowerS16: return vector_paired(inst, 16, true, true, true);
        case Op::VectorPairedMaxLowerS32: return vector_paired(inst, 32, true, true, true);
        case Op::VectorPairedMaxLowerU8: return vector_paired(inst, 8, true, true, false);
        case Op::VectorPairedMaxLowerU16: return vector_paired(inst, 16, true, true, false);
        case Op::VectorPairedMaxLowerU32: return vector_paired(inst, 32, true, true, false);
        case Op::VectorPairedMinLowerS8: return vector_paired(inst, 8, true, 2, true);
        case Op::VectorPairedMinLowerS16: return vector_paired(inst, 16, true, 2, true);
        case Op::VectorPairedMinLowerS32: return vector_paired(inst, 32, true, 2, true);
        case Op::VectorPairedMinLowerU8: return vector_paired(inst, 8, true, 2, false);
        case Op::VectorPairedMinLowerU16: return vector_paired(inst, 16, true, 2, false);
        case Op::VectorPairedMinLowerU32: return vector_paired(inst, 32, true, 2, false);
        case Op::VectorHalvingSubS8: return vector_halving_sub(inst, 8, true, false);
        case Op::VectorHalvingSubS16: return vector_halving_sub(inst, 16, true, false);
        case Op::VectorHalvingSubS32: return vector_halving_sub(inst, 32, true, false);
        case Op::VectorHalvingSubU8: return vector_halving_sub(inst, 8, false, false);
        case Op::VectorHalvingSubU16: return vector_halving_sub(inst, 16, false, false);
        case Op::VectorHalvingSubU32: return vector_halving_sub(inst, 32, false, false);
        case Op::VectorGreaterS64:
        case Op::VectorMinS64:
        case Op::VectorMinU64:
        case Op::VectorMaxS64:
        case Op::VectorMaxU64: {
            const bool maximum = kind == Op::VectorMaxS64 || kind == Op::VectorMaxU64;
            const bool signed_ = kind == Op::VectorGreaterS64 || kind == Op::VectorMinS64 || kind == Op::VectorMaxS64;
            for (unsigned word = 0; word < 4; word += 2) {
                push_i64_value(inst.GetArg(0), word); push_i64_value(inst.GetArg(1), word);
                op(signed_ ? GtS64 : GtU64); set(next_local + 4);
                if (kind == Op::VectorGreaterS64) {
                    constant64(code, 0); get(next_local + 4); op(ExtendU); op(Sub64);
                } else {
                    push_i64_value(inst.GetArg(maximum ? 0 : 1), word);
                    push_i64_value(inst.GetArg(maximum ? 1 : 0), word);
                    get(next_local + 4); op(Select);
                }
                store_i64_words(next_local + word);
            }
            return ok;
        }
        case Op::VectorGreaterS8: return vector_integer_select(inst, 8, VectorLaneOp::Greater, true);
        case Op::VectorGreaterS16: return vector_integer_select(inst, 16, VectorLaneOp::Greater, true);
        case Op::VectorGreaterS32: return vector_integer_select(inst, 32, VectorLaneOp::Greater, true);
        case Op::VectorMinS8: return vector_integer_select(inst, 8, VectorLaneOp::Minimum, true);
        case Op::VectorMinS16: return vector_integer_select(inst, 16, VectorLaneOp::Minimum, true);
        case Op::VectorMinS32: return vector_integer_select(inst, 32, VectorLaneOp::Minimum, true);
        case Op::VectorMinU8: return vector_integer_select(inst, 8, VectorLaneOp::Minimum, false);
        case Op::VectorMinU16: return vector_integer_select(inst, 16, VectorLaneOp::Minimum, false);
        case Op::VectorMinU32: return vector_integer_select(inst, 32, VectorLaneOp::Minimum, false);
        case Op::VectorMaxS8: return vector_integer_select(inst, 8, VectorLaneOp::Maximum, true);
        case Op::VectorMaxS16: return vector_integer_select(inst, 16, VectorLaneOp::Maximum, true);
        case Op::VectorMaxS32: return vector_integer_select(inst, 32, VectorLaneOp::Maximum, true);
        case Op::VectorMaxU8: return vector_integer_select(inst, 8, VectorLaneOp::Maximum, false);
        case Op::VectorMaxU16: return vector_integer_select(inst, 16, VectorLaneOp::Maximum, false);
        case Op::VectorMaxU32: return vector_integer_select(inst, 32, VectorLaneOp::Maximum, false);
        case Op::VectorAbs8: return vector_integer_select(inst, 8, VectorLaneOp::Absolute, true);
        case Op::VectorAbs16: return vector_integer_select(inst, 16, VectorLaneOp::Absolute, true);
        case Op::VectorAbs32: return vector_integer_select(inst, 32, VectorLaneOp::Absolute, true);
        case Op::VectorSignedAbsoluteDifference8: return vector_integer_select(inst, 8, VectorLaneOp::AbsoluteDifference, true);
        case Op::VectorSignedAbsoluteDifference16: return vector_integer_select(inst, 16, VectorLaneOp::AbsoluteDifference, true);
        case Op::VectorSignedAbsoluteDifference32: return vector_integer_select(inst, 32, VectorLaneOp::AbsoluteDifference, true);
        case Op::VectorUnsignedAbsoluteDifference8: return vector_integer_select(inst, 8, VectorLaneOp::AbsoluteDifference, false);
        case Op::VectorUnsignedAbsoluteDifference16: return vector_integer_select(inst, 16, VectorLaneOp::AbsoluteDifference, false);
        case Op::VectorUnsignedAbsoluteDifference32: return vector_integer_select(inst, 32, VectorLaneOp::AbsoluteDifference, false);
        case Op::VectorAdd8: return vector_integer_arithmetic(inst, 8, Add);
        case Op::VectorAdd16: return vector_integer_arithmetic(inst, 16, Add);
        case Op::VectorAdd32: return vector_integer_arithmetic(inst, 32, Add);
        case Op::VectorAdd64: return vector_integer_arithmetic(inst, 64, Add);
        case Op::VectorSub8: return vector_integer_arithmetic(inst, 8, Sub);
        case Op::VectorSub16: return vector_integer_arithmetic(inst, 16, Sub);
        case Op::VectorSub32: return vector_integer_arithmetic(inst, 32, Sub);
        case Op::VectorSub64: return vector_integer_arithmetic(inst, 64, Sub);
        case Op::VectorMultiply8: return vector_integer_arithmetic(inst, 8, Mul);
        case Op::VectorMultiply16: return vector_integer_arithmetic(inst, 16, Mul);
        case Op::VectorMultiply32: return vector_integer_arithmetic(inst, 32, Mul);
        case Op::VectorBroadcast8: return vector_broadcast(inst, 8);
        case Op::VectorBroadcast16: return vector_broadcast(inst, 16);
        case Op::VectorBroadcast32: return vector_broadcast(inst, 32);
        case Op::VectorBroadcast64: return vector_broadcast(inst, 64);
        case Op::VectorBroadcastElement8: return vector_broadcast(inst, 8, true);
        case Op::VectorBroadcastElement16: return vector_broadcast(inst, 16, true);
        case Op::VectorBroadcastElement32: return vector_broadcast(inst, 32, true);
        case Op::VectorBroadcastElement64: return vector_broadcast(inst, 64, true);
        case Op::VectorGetElement8: return vector_get_element(inst, 8);
        case Op::VectorGetElement16: return vector_get_element(inst, 16);
        case Op::VectorGetElement32: return vector_get_element(inst, 32);
        case Op::VectorGetElement64: return vector_get_element(inst, 64);
        case Op::VectorSetElement8: return vector_set_element(inst, 8);
        case Op::VectorSetElement16: return vector_set_element(inst, 16);
        case Op::VectorSetElement32: return vector_set_element(inst, 32);
        case Op::VectorSetElement64: return vector_set_element(inst, 64);
        case Op::VectorExtract: return vector_extract(inst, false);
        case Op::VectorExtractLower: return vector_extract(inst, true);
        case Op::And64:
        case Op::AndNot64:
        case Op::Or64:
        case Op::Eor64:
        case Op::Not64:
        case Op::VectorAnd:
        case Op::VectorAndNot:
        case Op::VectorOr:
        case Op::VectorEor:
        case Op::VectorNot: {
            // D modified immediates use scalar U64 IR; Q and register forms
            // use U128. Both keep high words and masks entirely in Wasm.
            const bool unary_not = kind == Op::VectorNot || kind == Op::Not64;
            const bool and_not = kind == Op::VectorAndNot || kind == Op::AndNot64;
            const unsigned words = inst.GetType() == Type::U64 ? 2 : 4;
            for (unsigned i = 0; i < words; ++i) {
                value_word(inst.GetArg(0), i);
                if (!unary_not) value_word(inst.GetArg(1), i);
                if (unary_not || and_not) { imm(UINT32_MAX); op(Xor); }
                if (!unary_not) {
                    op(kind == Op::VectorOr || kind == Op::Or64 ? Or
                        : kind == Op::VectorEor || kind == Op::Eor64 ? Xor : And);
                }
                set(next_local + i);
            }
            return ok;
        }
        case Op::ZeroVector:
            for (unsigned i = 0; i < 4; ++i) { imm(0); set(next_local + i); }
            return ok;
        case Op::ZeroExtendLongToQuad:
        case Op::VectorZeroUpper:
            for (unsigned i = 0; i < 4; ++i) {
                if (i < 2) value_word(inst.GetArg(0), i);
                else imm(0);
                set(next_local + i);
            }
            return ok;
        case Op::Pack2x64To1x128:
            for (unsigned i = 0; i < 4; ++i) { value_word(inst.GetArg(i / 2), i % 2); set(next_local + i); }
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
            // Match Dynarmic's D reload/forwarding contract: zero-extend the
            // temporary, without reading or modifying the paired D register.
            for (unsigned i = words; i < 4; ++i) { imm(0); set(next_local + i); }
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
        case Op::Mul64:
            // SSA storage uses i32 words even for U64 results. Publish both
            // halves before any overlapping architectural destination writes.
            value64(inst.GetArg(0)); value64(inst.GetArg(1)); op(Mul64);
            set(scratch_local);
            get(scratch_local); op(Wrap); set(next_local);
            get(scratch_local); op(0x42); uleb(code, 32); op(ShrU64); op(Wrap); set(next_local + 1);
            return ok;
        case Op::Add64:
        case Op::Sub64: {
            // SMLAL-style 64-bit accumulate (Balatro's OpenAL mixer hits
            // Add64 on boot: Mul64 + Pack2x32To1x64 + carry). The carry is a
            // U1 (immediate here, a flag word in ADC-style uses); value()
            // pushes either as i32. No CPSR flags in the IR for these ops.
            const bool sub = inst.GetOpcode() == Op::Sub64;
            value64(inst.GetArg(0)); value64(inst.GetArg(1)); op(sub ? Sub64 : Add64);
            value(inst.GetArg(2)); op(ExtendU); op(sub ? Sub64 : Add64);
            store_i64_words(next_local);
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
        case Op::A32ExceptionRaised:
            // UDF, BKPT, undefined/unpredictable encodings. With no
            // interpreter fallback the host must fail exactly as for an
            // untranslatable block, but only if this point is reached: the
            // instructions before it have executed. Ticks follow the memory
            // fault contract (completed store segments only).
            if (!inst.GetArg(0).IsImmediate() || inst.GetArg(0).GetType() != Type::U32) return false;
            {
                // The preceding UpdateUpperLocationDescriptor published the
                // mode AFTER the raising instruction (IT advanced); the host
                // resumes at the raising instruction, so publish its own mode.
                // It is the block's last instruction and IT advances once per
                // instruction; an NV-raised block does not advance at all.
                const uint32_t raise_pc = inst.GetArg(0).GetU32();
                Location raising = finish;
                if (finish.PC() != raise_pc) {
                    if (block.CycleCount() == 0) return false;
                    raising = start;
                    for (uint64_t i = 1; i < block.CycleCount(); ++i) raising = raising.AdvanceIT();
                    raising = raising.SetPC(raise_pc);
                }
                upper_location(raising);
                get(0); imm(raise_pc); store(offsetof(JitState, fault_pc));
            }
            if (!region) store_constant(offsetof(JitState, executed), 0);
            else if (completed_store_ticks) add_ticks(completed_store_ticks);
            ret(ExitReason::Exception);
            return ok;
        case Op::A32CallSupervisor:
            if (!pc_written) return false;
            if (hot_nid) {
                if (!is_hle_intrinsic_nid(hot_nid) || start.TFlag() || start.EFlag()
                    || finish.PC() != start.PC() + 4 || block.CycleCount() != 1
                    || !inst.GetArg(0).IsImmediate() || inst.GetArg(0).GetU32() != 0)
                    return false;
                if (hot_nid == kGetTlsAddrNid)
                    inline_tls_addr();
                else
                    inline_mutex();
            }
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

std::vector<uint8_t> emit_block(const Dynarmic::IR::Block &block, uint32_t hot_nid,
    RegionStateOptions options) {
    Emitter emitter(block, hot_nid, options);
    auto bytes = emitter.run();
    if (bytes.empty() && std::getenv("VITA3K_WASMJIT_REJECT_TRACE"))
        std::fprintf(stderr, "WasmJit emit_block rejected: %s\n", emitter.rejection.c_str());
    return bytes;
}

bool validate_region_block(const Dynarmic::IR::Block &block,
    const std::vector<StoreContinuation> &store_continuations, std::string *rejection,
    RegionStateOptions options) {
    const std::vector<const Dynarmic::IR::Block *> members{&block};
    const Location at(block.Location());
    const std::vector<RegionBlockMeta> metadata{{at.PC(), Location::CPSR_MODE_MASK,
        at.CPSR().Value() & Location::CPSR_MODE_MASK,
        static_cast<uint32_t>(block.CycleCount() + block.ConditionFailedCycleCount()), store_continuations}};
    Emitter emitter(block, 0, members, metadata, options);
    const bool valid = !emitter.region_body().empty();
    if (!valid && rejection)
        *rejection = emitter.rejection.empty() ? "unspecified" : emitter.rejection;
    return valid;
}

namespace {
// Function body (locals + code) of one region run(state, budget) function.
// With `aot`, entry-search misses report EntryMiss and the shared epilogue
// tail-calls the AOT function owning next_pc (see emit_aot_module).
Bytes region_function_body(
    const std::vector<const Dynarmic::IR::Block *> &blocks,
    const std::vector<RegionBlockMeta> &meta, RegionStateOptions options, bool aot) {
    options.aot = aot;
    const RegionState state(options);
    constexpr size_t kMaxBlocks = 512;
    constexpr uint64_t kMaxTicks = 32768;
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

    // Intrinsics access argument/result registers without Get/SetRegister
    // IR nodes. Include them in the invocation-wide register cache contract.
    for (const auto &m : meta) {
        if (m.hot_nid) {
            used_regs[0] = used_regs[1] = true;
            written_regs[0] = true;
        }
    }

    // Per-block bodies share the region local layout; SSA words are reused.
    std::vector<Bytes> bodies(n);
    uint32_t max_ssa = 0;
    for (size_t i = 0; i < n; ++i) {
        Emitter emitter(*blocks[i], unsigned(i), blocks, meta, options);
        bodies[i] = emitter.region_body();
        if (bodies[i].empty()) {
            if (std::getenv("VITA3K_WASMJIT_REJECT_TRACE"))
                std::fprintf(stderr, "WasmJit emit_region rejected block %zu: %s\n", i, emitter.rejection.c_str());
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
    // An AOT function entered at a PC it does not own (or in another mode)
    // must return to the host instead of re-resolving to itself.
    const ExitReason entry_miss = aot ? ExitReason::EntryMiss : ExitReason::Miss;
    const auto exit_with = [&](ExitReason reason, uint32_t open_ifs) {
        state.set_next_pc_from_pc(d);
        b_imm(d, static_cast<uint32_t>(reason)); b_set(d, 6);
        // Skip the open ifs, $default, all n block labels and the loop to
        // reach $exit. All state publication is shared after that label.
        b_op(d, Br); uleb(d, static_cast<uint32_t>(n) + 2 + open_ifs);
    };
    if (!aot) {
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
                b_op(d, If); b_op(d, 0x40); exit_with(entry_miss, open_ifs + 2); b_op(d, End);
                // FPSCR mode bits are part of the block's translation key.
                const uint32_t fpscr = Location(blocks[lo]->Location()).FPSCR().Value()
                    & Location::FPSCR_MODE_MASK;
                b_get(d, 0); b_op(d, Load); uleb(d, 2); uleb(d, offsetof(JitState, fpscr));
                b_imm(d, Location::FPSCR_MODE_MASK); b_op(d, And);
                b_imm(d, fpscr); b_op(d, Ne);
                b_op(d, If); b_op(d, 0x40); exit_with(entry_miss, open_ifs + 2); b_op(d, End);
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
    } // !aot: AOT entries arrive with local 6 = state.aot_entry

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
    if (aot)
        b_load(code, offsetof(JitState, aot_entry));
    else
        b_imm(code, kLightDispatchSentinel);
    b_set(code, 6);
    b_imm(code, 0); b_set(code, 7);
    if (!memory64) { b_load_host(code, offsetof(JitState, page_table_base)); b_set(code, 8); }
    b_load_host(code, offsetof(JitState, page_perms_base)); b_set(code, 9);
    b_load_host(code, offsetof(JitState, code_pages_base)); b_set(code, 10);
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
        if (aot)
            state.set_next_pc_from_pc(code);
        else
            state.set_next_pc(code, [&] { b_get(code, 3); });
        b_imm(code, static_cast<uint32_t>(entry_miss)); b_set(code, 6);
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
    if (aot) {
        // Miss with a published next_pc: continue in the AOT function that
        // owns it (transfer), passing the unspent budget. All state is in memory.
        b_get(code, 6); b_imm(code, static_cast<uint32_t>(ExitReason::Miss)); b_op(code, Eq);
        b_op(code, If); b_op(code, 0x40);
        b_get(code, 0); b_get(code, 1); state.read_executed(code); b_op(code, Sub);
        b_load(code, offsetof(JitState, next_pc));
        b_op(code, ReturnCall); uleb(code, kAotTransferFuncIndex);
        b_op(code, End);
    }
    b_get(code, 6); // function result
    b_op(code, End); // end function body

    Bytes body;
    uleb(body, memory64 ? 5 : 3);
    uleb(body, 3); body.push_back(0x7f); // locals 2,3,4: executed_call, pc, CheckBit
    uleb(body, 1); body.push_back(0x7e); // local 5: i64 scratch
    if (memory64) {
        uleb(body, 2); body.push_back(0x7f); // 6,7: dispatch index and counter
        uleb(body, 3); body.push_back(host_type); // 8..10: host metadata pointers (8 unused)
        uleb(body, state.ssa_base() - 11 + max_ssa); body.push_back(0x7f);
    } else {
        uleb(body, state.ssa_base() - 6 + max_ssa); body.push_back(0x7f);
    }
    body.insert(body.end(), code.begin(), code.end());
    return body;
}

// Outlined region fault path (R3j): one cold function per module shared
    // by every fault arm. Promoted mode round-trips other_psr through a
    // param/return ((state, pc, bits, other) -> other'); reference emission
    // RMWs memory ((state, pc, bits) -> ()). Ticks stay caller-side (the
    // executed_call local is run()'s frame); reason and epilogue branch
// stay at the call site (depth-sensitive).
Bytes region_fault_body(const RegionState &state) {
    Bytes fault;
    uleb(fault, 0); // no additional locals beyond the params
    if (state.uses_flag_locals()) {
        b_get(fault, 3); b_imm(fault, ~Location::CPSR_MODE_MASK); b_op(fault, And);
        b_get(fault, 2); b_op(fault, Or); b_set(fault, 3);
        b_get(fault, 0); b_get(fault, 1); b_store(fault, offsetof(JitState, fault_pc));
        b_get(fault, 3);
    } else {
        b_get(fault, 0); b_load(fault, offsetof(JitState, cpsr));
        b_imm(fault, ~Location::CPSR_MODE_MASK); b_op(fault, And);
        b_get(fault, 2); b_op(fault, Or); b_store(fault, offsetof(JitState, cpsr));
        b_get(fault, 0); b_get(fault, 1); b_store(fault, offsetof(JitState, fault_pc));
    }
    b_op(fault, End);
    return fault;
}

void append_types(Bytes &types, const RegionState &state) {
    types.insert(types.end(), {
        0x60, 2, host_type, 0x7f, 1, 0x7f, // type 0: (host,i32)->i32 for run
        0x60, 3, host_type, 0x7f, 0x7f, 1, 0x7f}); // type 1: checked helpers
    if (state.uses_flag_locals()) {
        types.insert(types.end(), {0x60, 4, host_type, 0x7f, 0x7f, 0x7f, 1, 0x7f}); // type 2: fault (P)
    } else {
        types.insert(types.end(), {0x60, 3, host_type, 0x7f, 0x7f, 0}); // type 2: fault (A)
    }
}

void append_helper_imports(Bytes &imports) {
    imports.insert(imports.end(), {
        3, 'e', 'n', 'v', 8, 'm', 'e', 'm', '_', 'r', 'e', 'a', 'd', 0, 1,
        3, 'e', 'n', 'v', 9, 'm', 'e', 'm', '_', 'w', 'r', 'i', 't', 'e', 0, 1,
        3, 'e', 'n', 'v', 4, 'f', 'p', '6', '4', 0, 1});
}
} // namespace

std::vector<uint8_t> emit_region(
    const std::vector<const Dynarmic::IR::Block *> &blocks,
    const std::vector<RegionBlockMeta> &meta, RegionStateOptions options) {
    constexpr size_t kMaxModule = 4 << 20;
    const RegionState state(options);
    const Bytes body = region_function_body(blocks, meta, options, false);
    if (body.empty())
        return {};
    const Bytes fault = region_fault_body(state);
    Bytes functions{2};
    uleb(functions, static_cast<uint32_t>(body.size()));
    functions.insert(functions.end(), body.begin(), body.end());
    uleb(functions, static_cast<uint32_t>(fault.size()));
    functions.insert(functions.end(), fault.begin(), fault.end());

    Bytes module{0, 'a', 's', 'm', 1, 0, 0, 0};
    Bytes types{3};
    append_types(types, state);
    section(module, 1, types);
    auto imports = memory_imports(4);
    append_helper_imports(imports);
    section(module, 2, imports);
    section(module, 3, {2, 0, 2}); // run (type 0), fault (type 2)
    // Function index space counts IMPORTED functions first: mem_read=0,
    // mem_write=1, fp64=2, our run=3, fault=4.
    section(module, 7, {1, 3, 'r', 'u', 'n', 0, 3});
    section(module, 10, functions);
    if (module.size() > kMaxModule)
        return {};
    return module;
}

std::vector<uint8_t> emit_aot_function(
    const std::vector<const Dynarmic::IR::Block *> &blocks,
    const std::vector<RegionBlockMeta> &meta, RegionStateOptions options) {
    return region_function_body(blocks, meta, options, true);
}

uint64_t aot_lut_offset_words(const std::vector<AotRange> &ranges, size_t index) {
    uint64_t words = 0;
    for (size_t i = 0; i < index && i < ranges.size(); ++i)
        words += ranges[i].size / 2;
    return words;
}

std::vector<uint8_t> assemble_aot_module(
    const std::vector<std::vector<uint8_t>> &function_bodies,
    const std::vector<AotRange> &ranges, const std::vector<uint8_t> &metadata,
    RegionStateOptions options) {
    const RegionState state(options);
    const uint32_t count = static_cast<uint32_t>(function_bodies.size());
    if (!count || ranges.empty())
        return {};
    constexpr uint32_t kFirstRegion = kAotTransferFuncIndex + 1;
    static_assert(kFaultFuncIndex == 4 && kAotLookupFuncIndex == 5 && kAotTransferFuncIndex == 6);
    if (count > kAotMaxFunctions)
        return {};

    // entry(state, budget): continue at regs[15].
    Bytes entry;
    uleb(entry, 0);
    b_get(entry, 0); b_get(entry, 1);
    b_load(entry, offsetof(JitState, regs) + 15 * sizeof(uint32_t));
    b_op(entry, ReturnCall); uleb(entry, kAotTransferFuncIndex);
    b_op(entry, End);

    // transfer(state, remaining, pc): all guest state is in memory. Tail-call
    // the function owning pc at its member block, or return to the host.
    Bytes transfer;
    uleb(transfer, 1); uleb(transfer, 1); transfer.push_back(0x7f); // local 3: lookup entry
    const auto leave = [&](ExitReason reason) {
        b_get(transfer, 0); b_get(transfer, 2); b_store(transfer, offsetof(JitState, next_pc));
        b_get(transfer, 0); b_imm(transfer, static_cast<uint32_t>(reason));
        b_store(transfer, offsetof(JitState, exit_reason));
        b_imm(transfer, static_cast<uint32_t>(reason));
        b_op(transfer, Return);
    };
    b_get(transfer, 0); b_get(transfer, 2);
    b_store(transfer, offsetof(JitState, regs) + 15 * sizeof(uint32_t));
    // Budget spent (signed: the last function may overrun by its forward path).
    b_get(transfer, 1); b_imm(transfer, 0); b_op(transfer, LeS);
    b_op(transfer, If); b_op(transfer, 0x40); leave(ExitReason::Budget); b_op(transfer, End);
    b_get(transfer, 2); b_op(transfer, Call); uleb(transfer, kAotLookupFuncIndex);
    b_op(transfer, Tee); uleb(transfer, 3); b_op(transfer, Eqz);
    b_op(transfer, If); b_op(transfer, 0x40); leave(ExitReason::Miss); b_op(transfer, End);
    // Entries are recorded for IT=0, E=0 and the default FPSCR mode only.
    b_load(transfer, offsetof(JitState, cpsr)); b_imm(transfer, Location::CPSR_MODE_MASK); b_op(transfer, And);
    b_get(transfer, 3); b_imm(transfer, 30); b_op(transfer, ShrU); b_imm(transfer, 5); b_op(transfer, Shl);
    b_op(transfer, Ne);
    b_load(transfer, offsetof(JitState, fpscr)); b_imm(transfer, Location::FPSCR_MODE_MASK); b_op(transfer, And);
    b_op(transfer, Or);
    b_op(transfer, If); b_op(transfer, 0x40); leave(ExitReason::EntryMiss); b_op(transfer, End);
    b_get(transfer, 0);
    b_get(transfer, 3); b_imm(transfer, 20); b_op(transfer, ShrU); b_imm(transfer, kAotMaxBlocks - 1); b_op(transfer, And);
    b_store(transfer, offsetof(JitState, aot_entry));
    b_get(transfer, 0); b_get(transfer, 1);
    b_get(transfer, 3); b_imm(transfer, kAotMaxFunctions); b_op(transfer, And); b_imm(transfer, 1); b_op(transfer, Sub);
    b_op(transfer, ReturnCallIndirect); uleb(transfer, 0); uleb(transfer, 0);
    b_op(transfer, End);

    // lookup(pc) -> slot + 1, or 0 when no AOT function owns pc.
    Bytes lookup;
    uleb(lookup, 1); uleb(lookup, 1); lookup.push_back(0x7f); // local 1: offset
    for (size_t k = 0; k < ranges.size(); ++k) {
        if (!ranges[k].size || (ranges[k].size & 1) || (ranges[k].base & 1))
            return {};
        b_get(lookup, 0); b_imm(lookup, ranges[k].base); b_op(lookup, Sub);
        b_op(lookup, Tee); uleb(lookup, 1);
        b_imm(lookup, ranges[k].size); b_op(lookup, LtU);
        b_op(lookup, If); b_op(lookup, 0x40);
        b_op(lookup, GlobalGet); uleb(lookup, 0);
        b_get(lookup, 1); b_imm(lookup, 1); b_op(lookup, ShrU); b_imm(lookup, 2); b_op(lookup, Shl);
        address_add_i32(lookup);
        b_shared_load32(lookup); memarg(lookup, 2, aot_lut_offset_words(ranges, k) * 4);
        b_op(lookup, Return);
        b_op(lookup, End);
    }
    b_imm(lookup, 0);
    b_op(lookup, End);

    Bytes fault = region_fault_body(state);
    Bytes functions;
    uleb(functions, 4 + count);
    for (const Bytes *body : {&entry, &fault, &lookup, &transfer}) {
        uleb(functions, static_cast<uint32_t>(body->size()));
        functions.insert(functions.end(), body->begin(), body->end());
    }
    for (const auto &body : function_bodies) {
        if (body.empty())
            return {};
        uleb(functions, static_cast<uint32_t>(body.size()));
        functions.insert(functions.end(), body.begin(), body.end());
    }

    Bytes module{0, 'a', 's', 'm', 1, 0, 0, 0};
    Bytes types{4};
    append_types(types, state);
    types.insert(types.end(), {0x60, 1, 0x7f, 1, 0x7f}); // type 3: lookup
    section(module, 1, types);
    auto imports = memory_imports(5);
    append_helper_imports(imports);
    imports.insert(imports.end(), {3, 'e', 'n', 'v', 7, 'a', 'o', 't', '_', 'l', 'u', 't',
        3, host_type, 0});
    section(module, 2, imports);
    Bytes declared;
    uleb(declared, 4 + count);
    declared.insert(declared.end(), {0, 2, 3, 1});
    for (uint32_t i = 0; i < count; ++i)
        declared.push_back(0);
    section(module, 3, declared);
    Bytes table{1, 0x70, 1};
    uleb(table, count); uleb(table, count);
    section(module, 4, table);
    section(module, 7, {1, 5, 'e', 'n', 't', 'r', 'y', 0, 3});
    Bytes elements{1, 0};
    b_imm(elements, 0); b_op(elements, End);
    uleb(elements, count);
    for (uint32_t i = 0; i < count; ++i)
        uleb(elements, kFirstRegion + i);
    section(module, 9, elements);
    section(module, 10, functions);
    Bytes custom;
    const char name[] = "vita3k.aot";
    uleb(custom, sizeof(name) - 1);
    custom.insert(custom.end(), name, name + sizeof(name) - 1);
    custom.insert(custom.end(), metadata.begin(), metadata.end());
    section(module, 0, custom);
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
        // Mask to FPSCR mode bits exactly like LocationDescriptor::UniqueHash:
        // raw status bits (NZCV/QC/cumulative) are not part of the host key.
        b_load(c, offsetof(JitState, fpscr));
        b_imm(c, Location::FPSCR_MODE_MASK); b_op(c, And);
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
        b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); address_add_i32(c);
        b_imm(c, 12); address_add_i32(c);
        b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_op(c, Eqz);
        if_void(miss);
        // match = (epoch == *epoch_addr) & (lo==key_lo) & (hi==key_hi).
        b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); address_add_i32(c);
        b_imm(c, 12); address_add_i32(c);
        b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_get(c, 3); b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_op(c, Eq);
        b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); address_add_i32(c);
        b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_get(c, 7); b_op(c, Eq); b_op(c, And);
        b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); address_add_i32(c);
        b_imm(c, 4); address_add_i32(c);
        b_op(c, Load); uleb(c, 2); uleb(c, 0);
        b_get(c, 8); b_op(c, Eq); b_op(c, And);
        if_void([&] {
            b_get(c, 2); b_get(c, 9); b_imm(c, 4); b_op(c, Shl); address_add_i32(c);
            b_imm(c, 8); address_add_i32(c);
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
        0x60, 2, host_type, 0x7f, 1, 0x7f, // type 0: region run(state,budget)->reason
        0x60, 4, host_type, 0x7f, host_type, host_type, 1, 0x7f}); // type 1: dispatch
    auto imports = memory_imports(2);
    imports.insert(imports.end(), {
        3, 'e', 'n', 'v', 12, 'r', 'e', 'g', 'i', 'o', 'n', '_', 't', 'a', 'b', 'l', 'e',
        1, 0x70, 0, 1}); // ordinary i32-indexed funcref table in BOTH memory modes
    section(module, 2, imports);
    section(module, 3, {1, 1}); // one function, type 1
    section(module, 7, {1, 8, 'd', 'i', 's', 'p', 'a', 't', 'c', 'h', 0, 0});
    section(module, 10, funcs);
    if (module.size() > kMaxModule)
        return {};
    return module;
}
} // namespace vita3k::wasmjit
