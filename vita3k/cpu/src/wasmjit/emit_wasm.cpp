// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include "emit_wasm.h"

#include <limits>
#include <unordered_map>

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
    If = 0x04, Else = 0x05, End = 0x0b, Return = 0x0f, Select = 0x1b,
    Get = 0x20, Set = 0x21, Load = 0x28, Store = 0x36, Const = 0x41,
    Eqz = 0x45, Eq = 0x46, Ne = 0x47, LtU = 0x49, GtU = 0x4b, LeU = 0x4d,
    Add = 0x6a, Sub = 0x6b, And = 0x71, Or = 0x72, Xor = 0x73,
    Shl = 0x74, ShrS = 0x75, ShrU = 0x76, RotR = 0x78,
    Add64 = 0x7c, ShrU64 = 0x88, Wrap = 0xa7, ExtendU = 0xad,
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

bool scalar(Type type) {
    return type == Type::U1 || type == Type::U8 || type == Type::U16 || type == Type::U32 || type == Type::NZCVFlags;
}

bool arithmetic(Op op) {
    return op == Op::Add32 || op == Op::Sub32;
}

bool shift(Op op) {
    return op == Op::LogicalShiftLeft32 || op == Op::LogicalShiftRight32
        || op == Op::ArithmeticShiftRight32 || op == Op::RotateRight32 || op == Op::RotateRightExtended;
}

class Emitter {
public:
    explicit Emitter(const Dynarmic::IR::Block &block)
        : block(block), start(block.Location()), finish(block.EndLocation()) {}

    Bytes run() {
        if (block.size() > 4096 || block.CycleCount() == 0 || block.CycleCount() > 4096
            || block.ConditionFailedCycleCount() > 4096 || !valid_location(start) || !valid_location(finish))
            return {};

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
            op(End);
        }

        for (const Inst &inst : block)
            has_bx |= inst.GetOpcode() == Op::A32BXWritePC;
        for (const Inst &inst : block) {
            // SVC must be the final side effect: the host handles it on return.
            if (svc || !instruction(inst))
                return {};
            locals.emplace(&inst, next_local);
            next_local += 3; // value, carry, overflow; stable even for void ops
        }
        store_constant(offsetof(JitState, executed), static_cast<uint32_t>(block.CycleCount()));
        terminal(block.GetTerminal(), 0);
        if (!ok)
            return {};
        op(End);

        Bytes body;
        uleb(body, 1); // one group of i32 locals (parameter is local 0)
        uleb(body, next_local - 1);
        body.push_back(0x7f);
        body.insert(body.end(), code.begin(), code.end());
        Bytes functions{1};
        uleb(functions, static_cast<uint32_t>(body.size()));
        functions.insert(functions.end(), body.begin(), body.end());

        Bytes module{0, 'a', 's', 'm', 1, 0, 0, 0};
        section(module, 1, {1, 0x60, 1, 0x7f, 1, 0x7f}); // (i32)->i32
        section(module, 2, {1, 3, 'e', 'n', 'v', 6, 'm', 'e', 'm', 'o', 'r', 'y', 2, 0, 1});
        section(module, 3, {1, 0}); // one function, type 0
        section(module, 7, {1, 5, 'b', 'l', 'o', 'c', 'k', 0, 0});
        section(module, 10, functions);
        return module;
    }

private:
    const Dynarmic::IR::Block &block;
    Location start, finish;
    Bytes code;
    std::unordered_map<const Inst *, uint32_t> locals;
    uint32_t next_local = 1;
    unsigned terminal_nodes = 0;
    bool ok = true;
    bool svc = false;
    bool pc_written = false;
    bool has_bx = false;

    void op(uint8_t byte) { code.push_back(byte); }
    void imm(uint32_t n) { constant(code, n); }
    void get(uint32_t index) { op(Get); uleb(code, index); }
    void set(uint32_t index) { op(Set); uleb(code, index); }
    void begin_if(bool result = false) { op(If); op(result ? 0x7f : 0x40); }
    void load(uint32_t offset) { get(0); op(Load); uleb(code, 2); uleb(code, offset); }
    void store(uint32_t offset) { op(Store); uleb(code, 2); uleb(code, offset); }
    void store_constant(uint32_t offset, uint32_t n) { get(0); imm(n); store(offset); }
    void mask(uint32_t bits) { imm(bits); op(And); }

    void value(const Value &v) {
        if (!scalar(v.GetType())) {
            ok = false;
            return;
        }
        if (v.IsImmediate()) {
            if (v.GetType() == Type::NZCVFlags) { // EmptyNZCVImmediateMarker is NOT a flag value
                ok = false;
                return;
            }
            imm(static_cast<uint32_t>(v.GetImmediateAsU64()));
        } else {
            const auto it = locals.find(v.GetInstRecursive());
            if (it == locals.end()) {
                ok = false;
                return;
            }
            get(it->second);
        }
    }

    bool valid_location(const Location &loc) const {
        return loc.IT().Value() == 0 && (loc.PC() & (loc.TFlag() ? 1 : 3)) == 0
            && loc.FPSCR() == start.FPSCR();
    }

    void upper_location(const Location &loc) {
        if (!valid_location(loc)) {
            ok = false;
            return;
        }
        get(0);
        load(offsetof(JitState, cpsr));
        mask(~Location::CPSR_MODE_MASK);
        imm(loc.CPSR().Value() & Location::CPSR_MODE_MASK);
        op(Or);
        store(offsetof(JitState, cpsr));
    }

    void location(const Location &loc) {
        upper_location(loc);
        store_constant(offsetof(JitState, regs) + 15 * sizeof(uint32_t), loc.PC());
    }

    void flag(unsigned bit) {
        load(offsetof(JitState, cpsr)); imm(bit); op(ShrU); mask(1);
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
        default: ok = false; break; // NV is not an unconditional alias
        }
    }

    void ret(ExitReason reason) {
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
            op(End);
            // Both arms return; Wasm's validator still requires a value after
            // a void if when validating the enclosing function's result type.
            op(0x00); // unreachable
        } else if (boost::get<Term::ReturnToDispatch>(&term) || boost::get<Term::PopRSBHint>(&term)
            || boost::get<Term::FastDispatchHint>(&term)) {
            if (!pc_written)
                ok = false;
            ret(svc ? ExitReason::Svc : ExitReason::Continue);
        } else if (const auto *halt = boost::get<Term::CheckHalt>(&term)) {
            // No halt field/import: only accept a check whose two outcomes
            // already return to host without further guest-state changes.
            if (!boost::get<Term::ReturnToDispatch>(&halt->else_)
                && !boost::get<Term::PopRSBHint>(&halt->else_)
                && !boost::get<Term::FastDispatchHint>(&halt->else_)) {
                ok = false;
                return;
            }
            terminal(halt->else_, depth + 1);
        } else {
            ok = false; // Invalid, Interpret, CheckBit are never silently skipped
        }
        // SVC cannot be followed by a link/conditional terminal: such a block
        // would resume execution without allowing its host callback to run.
        if (svc && (boost::get<Term::LinkBlock>(&term) || boost::get<Term::LinkBlockFast>(&term) || boost::get<Term::If>(&term)))
            ok = false;
    }

    void nz(const Value &v) {
        value(v); mask(0x80000000);
        value(v); op(Eqz); imm(30); op(Shl); op(Or);
    }

    void add_sub(const Inst &inst) {
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
        sum(); op(Wrap); set(next_local);
        sum(); op(0x42); op(32); op(ShrU64); op(Wrap); set(next_local + 1);
        // V = (~(a ^ b) & (a ^ result)) >> 31 for add; for subtract
        // use (a ^ b) instead. This also accounts for carry/borrow input.
        value(inst.GetArg(0)); value(inst.GetArg(1)); op(Xor);
        if (!sub) { imm(0xffffffff); op(Xor); }
        value(inst.GetArg(0)); get(next_local); op(Xor); op(And);
        imm(31); op(ShrU); set(next_local + 2);
    }

    void shifted(const Inst &inst) {
        const auto a = inst.GetArg(0);
        const auto n = inst.GetArg(1);
        const auto carry = inst.GetArg(inst.GetOpcode() == Op::RotateRightExtended ? 1 : 2);
        const Op kind = inst.GetOpcode();
        if (kind == Op::RotateRightExtended) {
            value(a); imm(1); op(ShrU); value(carry); imm(31); op(Shl); op(Or); set(next_local);
            value(a); mask(1); set(next_local + 1);
            return;
        }
        // Every shift count is U8, not Wasm's count modulo 32. Handle zero,
        // exactly 32 and >32 explicitly; only ROR may use modulo semantics.
        value(n); op(Eqz); begin_if();
        value(a); set(next_local); value(carry); set(next_local + 1);
        op(Else);
        if (kind == Op::RotateRight32) {
            value(a); value(n); op(RotR); set(next_local);
            get(next_local); imm(31); op(ShrU); set(next_local + 1);
        } else {
            value(n); imm(32); op(LtU); begin_if();
            value(a); value(n);
            op(kind == Op::LogicalShiftLeft32 ? Shl : kind == Op::LogicalShiftRight32 ? ShrU : ShrS);
            set(next_local);
            value(a);
            if (kind == Op::LogicalShiftLeft32) { imm(32); value(n); op(Sub); }
            else { value(n); imm(1); op(Sub); }
            op(ShrU); mask(1); set(next_local + 1);
            op(Else);
            if (kind == Op::ArithmeticShiftRight32) {
                value(a); imm(31); op(ShrS); set(next_local);
                value(a); imm(31); op(ShrU); set(next_local + 1);
            } else {
                imm(0); set(next_local);
                value(n); imm(32); op(Eq); begin_if(true);
                value(a);
                if (kind == Op::LogicalShiftRight32) { imm(31); op(ShrU); }
                else { mask(1); }
                op(Else); imm(0); op(End); set(next_local + 1);
            }
            op(End);
        }
        op(End);
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
            case Op::GetCarryFromOp:
                if (!arith && !shift(producer->GetOpcode())) return false;
                get(it->second + 1);
                break;
            case Op::GetOverflowFromOp:
                if (!arith) return false;
                get(it->second + 2);
                break;
            case Op::GetNZCVFromOp:
                if (!arith) return false;
                nz(arg);
                get(it->second + 1); imm(29); op(Shl); op(Or);
                get(it->second + 2); imm(28); op(Shl); op(Or);
                break;
            default: return false;
            }
        }
        set(next_local);
        return ok;
    }

    bool instruction(const Inst &inst) {
        const Op kind = inst.GetOpcode();
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
            if (kind == Op::A32GetRegister) { load(offset); break; }
            get(0); arg(1); store(offset);
            pc_written |= index == 15;
            return ok;
        }
        case Op::A32GetCpsr: load(offsetof(JitState, cpsr)); break;
        case Op::A32GetCFlag: flag(29); break;
        case Op::A32SetCpsrNZ:
        case Op::A32SetCpsrNZC:
        case Op::A32SetCpsrNZCV:
        case Op::A32SetCpsrNZCVRaw: {
            const uint32_t bits = kind == Op::A32SetCpsrNZ ? 0xc0000000
                : kind == Op::A32SetCpsrNZC ? 0xe0000000 : 0xf0000000;
            get(0); load(offsetof(JitState, cpsr)); mask(~bits);
            arg(0); mask(kind == Op::A32SetCpsrNZC ? 0xc0000000 : bits); op(Or);
            if (kind == Op::A32SetCpsrNZC) { arg(1); imm(29); op(Shl); op(Or); }
            store(offsetof(JitState, cpsr));
            return ok;
        }
        case Op::GetNZFromOp:
        case Op::GetNZCVFromOp:
        case Op::GetCarryFromOp:
        case Op::GetOverflowFromOp: return pseudo(inst);
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
            get(0); load(offsetof(JitState, cpsr)); mask(~uint32_t(0x20));
            arg(0); mask(1); imm(5); op(Shl); op(Or); store(offsetof(JitState, cpsr));
            get(0); arg(0);
            imm(0xfffffffe); imm(0xfffffffc); arg(0); mask(1); op(Select); op(And);
            store(offsetof(JitState, regs) + 15 * sizeof(uint32_t));
            pc_written = true;
            return ok;
        case Op::PushRSB:
            // Pure prediction hint. Like PopRSBHint, the no-RSB backend always
            // goes through the parent dispatcher instead of speculating.
            return inst.GetArg(0).IsImmediate() && inst.GetArg(0).GetType() == Type::U64;
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
} // namespace vita3k::wasmjit
