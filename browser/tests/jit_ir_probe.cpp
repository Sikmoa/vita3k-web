// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Translation-only integration probe: no host JIT and no guest execution.
#include <wasmjit/frontend.h>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/ir/ir_emitter.h>
#include <dynarmic/ir/opt/passes.h>
#include <mem/functions.h>
#include <mem/state.h>

namespace {
using vita3k::wasmjit::translate_block;
namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;

unsigned checks = 0;
void require(bool ok, const char *message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}

template<typename Exception, typename F>
void expect_error(const char *label, F action) {
    bool caught = false;
    try {
        action();
    } catch (const Exception &error) {
        std::printf("expected %s: %s\n", label, error.what());
        caught = true;
    }
    require(caught, label);
}

void put16(MemState &mem, uint32_t address, uint16_t value) {
    const std::array<uint8_t, 2> bytes{uint8_t(value), uint8_t(value >> 8)};
    require(mem_write(mem, address, bytes.data(), bytes.size()), "put16");
}
void put32(MemState &mem, uint32_t address, uint32_t value) {
    put16(mem, address, uint16_t(value));
    put16(mem, address + 2, uint16_t(value >> 16));
}
std::string dump(const char *label, IR::Block &block) {
    Dynarmic::Optimization::VerificationPass(block);
    Dynarmic::Optimization::NamingPass(block);
    const auto text = IR::DumpBlock(block);
    std::printf("\n=== %s ===\n%s", label, text.c_str());
    return text;
}

void probe(MemState &mem) {
    constexpr uint32_t arm = 0x81000000;
    constexpr uint32_t thumb = arm + 0x100;
    require(alloc_at(mem, arm, 0x1000, "jit IR probe") == arm, "allocate code");
    // mov r0,#1; add r0,r0,#2; sub r0,r0,#1; cmp r0,#2; bne arm
    const std::array<uint32_t, 5> arm_code{0xe3a00001, 0xe2800002, 0xe2400001, 0xe3500002, 0x1afffffa};
    for (size_t i = 0; i < arm_code.size(); ++i) put32(mem, arm + uint32_t(i * 4), arm_code[i]);
    // movs r0,#1; adds r0,#2; subs r0,#1; cmp r0,#2; bne thumb
    const std::array<uint16_t, 5> thumb_code{0x2001, 0x3002, 0x3801, 0x2802, 0xd1fa};
    for (size_t i = 0; i < thumb_code.size(); ++i) put16(mem, thumb + uint32_t(i * 2), thumb_code[i]);

    auto a = translate_block(mem, arm, 0);
    const auto arm_dump = dump("ARM mov/add/sub/cmp (split before conditional branch)", a);
    require(a.CycleCount() == 4 && A32::LocationDescriptor{a.EndLocation()}.PC() == arm + 16, "ARM conditional split");
    require(arm_dump.find("terminal = LinkBlockFast{{0000000081000010}}") != std::string::npos, "ARM fallthrough terminal");
    auto ab = translate_block(mem, arm + 16, 0);
    dump("ARM bne (block entry condition, not Term::If)", ab);
    require(ab.GetCondition() == IR::Cond::NE && ab.CycleCount() == 1, "ARM bne entry condition");
    require(A32::LocationDescriptor{ab.ConditionFailedLocation()}.PC() == arm + 20, "ARM bne false PC");
    require(A32::LocationDescriptor{boost::get<IR::Term::LinkBlock>(ab.GetTerminal()).next}.PC() == arm, "ARM bne true PC");

    auto t = translate_block(mem, thumb, 0x20);
    const auto thumb_dump = dump("Thumb movs/adds/subs/cmp/bne", t);
    require(t.CycleCount() == 5 && A32::LocationDescriptor{t.EndLocation()}.PC() == thumb + 10, "Thumb block size");
    const auto branch = boost::get<IR::Term::If>(t.GetTerminal());
    require(branch.if_ == IR::Cond::NE, "Thumb bne condition");
    require(thumb_dump.find("If{ne, LinkBlock{{0000000181000100}}, LinkBlock{{000000018100010a}}}") != std::string::npos, "Thumb bne terminal");

    auto limited = translate_block(mem, arm, 0, 2);
    dump("ARM budget=2", limited);
    require(limited.CycleCount() == 2, "instruction budget");
    require(A32::LocationDescriptor{boost::get<IR::Term::LinkBlock>(limited.GetTerminal()).next}.PC() == arm + 8, "budget terminal PC");

    // Preserve descriptor bits; NZCV deliberately are not cache-key bits.
    A32::PSR cpsr{0x20 | 0x200};
    cpsr.IT(A32::ITState{0x08}); // last instruction of IT EQ
    constexpr uint32_t fpscr = 0x03c00000;
    auto it = translate_block(mem, thumb, cpsr.Value(), 1, fpscr);
    dump("Thumb IT EQ with E/FPSCR key", it);
    const A32::LocationDescriptor start{it.Location()}, end{it.EndLocation()};
    require(start.IT().Value() == 0x08 && start.TFlag() && start.EFlag(), "CPSR key");
    require(start.FPSCR().Value() == fpscr && end.FPSCR().Value() == fpscr, "FPSCR key");
    require(end.IT().Value() == 0 && end.PC() == thumb + 2, "IT advance");
    require(it.GetCondition() == IR::Cond::EQ, "IT condition");
    auto nzcv = translate_block(mem, thumb, cpsr.Value() | 0xf0000000, 1, fpscr);
    require(it.Location() == nzcv.Location(), "NZCV not part of key");

    // Thumb32 beginning at a halfword-aligned address must read the next word.
    put16(mem, thumb + 0x42, 0xf241); // movw r0,#0x1234
    put16(mem, thumb + 0x44, 0x2034);
    auto wide = translate_block(mem, thumb + 0x42, 0x20, 1);
    const auto wide_dump = dump("Thumb32 unaligned-to-word MOVW", wide);
    require(wide_dump.find("SetRegister r0, #0x1234") != std::string::npos, "Thumb32 immediate");
    require(wide.CycleCount() == 1 && A32::LocationDescriptor{wide.EndLocation()}.PC() == thumb + 0x46, "Thumb32 size");

    // The budget must stop BEFORE fetching a subsequent unmapped page.
    put32(mem, arm + 0xffc, 0xe3a00001);
    auto edge = translate_block(mem, arm + 0xffc, 0, 1);
    require(edge.CycleCount() == 1, "no fetch beyond budget");
    expect_error<std::runtime_error>("unmapped next instruction", [&] { translate_block(mem, arm + 0xffc, 0, 2); });
    put16(mem, arm + 0xffe, 0xf240);
    expect_error<std::runtime_error>("Thumb32 crosses unmapped page", [&] { translate_block(mem, arm + 0xffe, 0x20, 1); });
    expect_error<std::runtime_error>("unmapped initial PC", [&] { translate_block(mem, 0x70000000, 0); });
    require(mem_set_permissions(mem, arm, 0x1000, MemPerm::ReadOnly), "remove execute");
    expect_error<std::runtime_error>("non-executable code", [&] { translate_block(mem, arm, 0); });
    require(mem_set_permissions(mem, arm, 0x1000, MemPerm::Execute), "execute-only code");
    auto execute_only = translate_block(mem, arm, 0, 1);
    require(execute_only.CycleCount() == 1, "fetch requires X, not R");
    expect_error<std::invalid_argument>("zero budget", [&] { translate_block(mem, arm, 0, 0); });
    expect_error<std::invalid_argument>("unaligned ARM", [&] { translate_block(mem, arm + 2, 0); });
    expect_error<std::invalid_argument>("Thumb-tagged PC", [&] { translate_block(mem, thumb + 1, 0x20); });
    free(mem, arm);

    // Generated build-local IR emitter must fail closed on every native-only
    // host-call overload; no function/table pointer is encoded into portable IR.
    IR::Block host_block{IR::LocationDescriptor{0}};
    IR::IREmitter host_ir{host_block};
    expect_error<std::logic_error>("native host call (0 args)", [&] { host_ir.CallHostFunction(+[] {}); });
    expect_error<std::logic_error>("native host call (1 arg)", [&] { host_ir.CallHostFunction(+[](u64) {}, host_ir.Imm64(0)); });
    expect_error<std::logic_error>("native host call (2 args)", [&] { host_ir.CallHostFunction(+[](u64, u64) {}, host_ir.Imm64(0), host_ir.Imm64(0)); });
    expect_error<std::logic_error>("native host call (3 args)", [&] { host_ir.CallHostFunction(+[](u64, u64, u64) {}, host_ir.Imm64(0), host_ir.Imm64(0), host_ir.Imm64(0)); });
    require(host_block.empty(), "native host call must not emit IR");
}
} // namespace

int main() {
    MemState mem;
    if (!init(mem, true)) return 2;
    int result = 0;
    try {
        probe(mem);
        std::printf("\nPASS: %u checks; genuine Dynarmic A32 Translate + IR, no native backend\n", checks);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
        result = 1;
    }
    deinit_mem(mem);
    return result;
}
