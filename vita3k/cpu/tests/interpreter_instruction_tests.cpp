#include <cpu/impl/interpreter_cpu.h>
#include <cpu/state.h>
#include <mem/functions.h>

#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
#include "interpreter_instruction_cases.inc"
constexpr uint32_t N = 1u << 31, Z = 1u << 30, C = 1u << 29, V = 1u << 28;
constexpr uint32_t NZCV = N | Z | C | V;
constexpr uint32_t data = 0x82000000, stack = data + 0x3000;
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); throw std::runtime_error(#expr); } } while (false)

struct Fixture {
    MemState memory;
    CPUState state;
    InterpreterCPU cpu{&state, 0};
    Fixture() {
        CHECK(::init(memory, true));
        state.mem = &memory;
        CHECK(try_alloc_at(memory, 0x81000000, 0x1000, "instruction tests") == 0x81000000);
        CHECK(try_alloc_at(memory, data, 0x4000, "instruction data/stack") == data);
        CHECK(mem_write(memory, 0x81000000, instruction_bytes, sizeof(instruction_bytes)));
    }
    ~Fixture() { deinit_mem(memory); }
    void start(uint32_t pc, uint32_t flags = 0, bool thumb = true) {
        cpu.load_context(CPUContext{});
        cpu.set_cpsr(flags);
        cpu.set_pc(pc | unsigned(thumb));
        cpu.set_sp(stack);
        cpu.set_instruction_budget(1000);
    }
    void run() { CHECK(cpu.run() == 0); CHECK(state.svc_called); }
    uint32_t reg(unsigned r) { return cpu.get_reg(uint8_t(r)); }
    void reg(unsigned r, uint32_t value) { cpu.set_reg(uint8_t(r), value); }
    void word(uint32_t address, uint32_t value) { CHECK(mem_write(memory, address, &value, 4)); }
    uint32_t word(uint32_t address) { uint32_t v = 0; CHECK(mem_read(memory, address, &v, 4)); return v; }
};

void arithmetic(Fixture &f) {
    struct Case { uint32_t pc, a, b, flags, expected, expected_flags; };
    const Case cases[] = {
        {test_flags, 0x7fffffff, 1, 0, 0x80000000, N | V},
        {test_flags, 0xffffffff, 1, V, 0, Z | C},
        {test_flags, 0x80000000, 0x80000000, 0, 0, Z | C | V},
        {test_sub, 0, 1, C, 0xffffffff, N},
        {test_sub, 1, 1, V, 0, Z | C},
        {test_sub, 0x80000000, 1, 0, 0x7fffffff, C | V},
        {test_adc, 0x7fffffff, 0, C, 0x80000000, N | V},
        {test_adc, 0xffffffff, 0, C, 0, Z | C},
        {test_sbc, 0, 0, 0, 0xffffffff, N},
        {test_sbc, 0, 0, C, 0, Z | C},
        {test_sbc, 0x80000000, 0x7fffffff, 0, 0, Z | C | V},
        {test_wide_flags, 0x7ffffffc, 1, C, 0x80000000, N | V},
    };
    for (const auto &t : cases) {
        f.start(t.pc, t.flags); f.reg(0, t.a); f.reg(1, t.b); f.run();
        CHECK(f.reg(0) == t.expected);
        CHECK((f.cpu.get_cpsr() & NZCV) == t.expected_flags);
    }
    const Case shifts[] = {
        {test_shift, 0x80000001, 0, C | V, 0x80000001, N | C | V},
        {test_shift, 0x80000001, 1, V, 2, C | V},
        {test_shift, 0x80000001, 32, 0, 0, Z | C},
        {test_shift, 0xffffffff, 33, C, 0, Z},
        {test_lsr, 0x80000001, 32, V, 0, Z | C | V},
        {test_lsr, 0x80000001, 33, C, 0, Z},
        {test_asr, 0x80000001, 31, 0, 0xffffffff, N},
        {test_asr, 0x80000001, 32, 0, 0xffffffff, N | C},
        {test_asr, 0x7fffffff, 255, C, 0, Z},
        {test_ror, 0x80000001, 32, 0, 0x80000001, N | C},
        {test_ror, 1, 1, 0, 0x80000000, N | C},
        {test_ror, 0x80000001, 256, V, 0x80000001, N | V},
    };
    for (const auto &t : shifts) {
        f.start(t.pc, t.flags); f.reg(0, t.a); f.reg(1, t.b); f.run();
        CHECK(f.reg(0) == t.expected);
        CHECK((f.cpu.get_cpsr() & NZCV) == t.expected_flags);
    }
    f.start(test_imm_shift); f.reg(0, 0x80000001); f.reg(1, 0x80000001); f.run();
    CHECK(f.reg(0) == 0 && f.reg(1) == 0xffffffff);
    CHECK((f.cpu.get_cpsr() & NZCV) == (N | C));
}
void it_context(Fixture &f) {
    f.start(test_it); f.reg(1, 99); f.reg(2, 0xffffffff); f.reg(3, 0x1234);
    CHECK(f.cpu.step() == 0); // CMP
    CHECK(f.cpu.step() == 0); // ITTE EQ
    const auto context = f.cpu.save_context();
    CHECK((context.cpsr & ((3u << 25) | (0x3fu << 10))) == ((2u << 25) | (4u << 8)));
    f.run();
    CHECK(f.reg(1) == 0 && f.reg(2) == 0 && f.reg(3) == 0x1234);
    CHECK((f.cpu.get_cpsr() & NZCV) == (Z | C)); // implicit S suppressed inside IT
    CHECK((f.cpu.get_cpsr() & ((3u << 25) | (0x3fu << 10))) == 0);
    CHECK(f.state.svc == 0x25 && f.cpu.get_pc() == test_it + 18);
    auto expected = f.cpu.save_context();
    f.cpu.load_context(context);
    f.run();
    auto actual = f.cpu.save_context();
    CHECK(actual.cpu_registers == expected.cpu_registers && actual.cpsr == expected.cpsr);

    f.start(test_it); f.reg(0, 1); f.reg(1, 99); f.reg(2, 5); f.reg(5, data); f.word(data, 77);
    f.run();
    CHECK(f.reg(1) == 99 && f.reg(2) == 5 && f.reg(3) == 0xdead && f.reg(4) == 77);
}
void memory_and_stack(Fixture &f) {
    f.start(test_memory); f.reg(0, data); f.reg(1, 0x1234fedc); f.run();
    CHECK(f.reg(0) == data + 20);
    CHECK(f.reg(2) == 0x1234fedc && f.reg(3) == 0xdc && f.reg(4) == 0xfedc);
    CHECK(f.reg(6) == 0xffffffdc && f.reg(7) == 0xfffffedc);
    CHECK(f.reg(8) == 0x1234fedc && f.reg(9) == 0x1234fedc);
    CHECK(f.reg(10) == 0x1234fedc && f.reg(11) == 0x1234fedc && f.reg(12) == 0xfedc);
    f.start(test_stack);
    for (unsigned i = 4; i <= 11; ++i) f.reg(i, 0x12340000 + i);
    f.cpu.set_lr(test_mov + 0x21); // return to Thumb SVC
    f.run();
    for (unsigned i = 4; i <= 11; ++i) CHECK(f.reg(i) == 0x12340000 + i);
    CHECK(f.cpu.get_sp() == stack && f.cpu.is_thumb_mode());
    f.start(test_multi); f.reg(0, data); f.reg(1, 11); f.reg(2, 22); f.reg(3, 33); f.reg(7, data + 0x80);
    f.word(data + 0x80, 44); f.word(data + 0x84, 55); f.run();
    CHECK(f.reg(0) == 44 && f.reg(4) == 11 && f.reg(5) == 22 && f.reg(6) == 33 && f.reg(7) == 55);
}
void control_and_vectors(Fixture &f) {
    f.start(test_pc); f.run();
    CHECK(f.reg(0) == test_pc + 8 && f.reg(1) == 0x1234abcd && f.reg(2) == test_pc + 8);
    f.start(test_branch); f.run();
    CHECK(f.reg(0) == 42 && f.state.svc == 7);
    f.start(test_interwork); f.run();
    CHECK(f.reg(0) == 0x80000000 && f.cpu.get_lr() == test_interwork + 5);
    CHECK(f.cpu.is_thumb_mode() && f.state.svc == 9);
    f.start(test_arm, 0, false); f.reg(7, 0xdeadbeef); f.run();
    CHECK(f.reg(0) == 0x80000000 && f.reg(1) == 0 && f.reg(2) == 42);
    CHECK(f.reg(7) == 0xdeadbeef && f.state.svc == 0x123456); // never a synthetic r7 ABI
    CHECK(!f.cpu.is_thumb_mode() && f.cpu.get_pc() == test_arm + 16);
    CHECK(f.word(f.cpu.get_pc() + 4) == 0xfeedcafe);
    CHECK((f.cpu.get_cpsr() & NZCV) == (N | C));
    f.start(test_veneer); f.run();
    CHECK(f.reg(0) == 42 && f.state.svc == 11 && f.cpu.is_thumb_mode());
    f.start(test_load_pc); f.reg(0, data); f.word(data, test_arm); f.run();
    CHECK(f.state.svc == 0x123456 && !f.cpu.is_thumb_mode());
    f.start(test_mov_pc); f.reg(0, test_mov + 32); f.run();
    CHECK(f.state.svc == 0x73 && f.cpu.is_thumb_mode());
    f.start(test_blx_reg); f.reg(0, test_interwork + 8); f.run();
    CHECK(f.reg(0) == 0x80000000 && f.state.svc == 12 && f.cpu.is_thumb_mode());
    CHECK(f.cpu.get_lr() == test_blx_reg + 3);
    f.start(test_it_svc); f.run();
    CHECK(f.state.svc == 14 && f.cpu.get_pc() == test_it_svc + 6);
    CHECK((f.cpu.get_cpsr() & ((3u << 25) | (0x3fu << 10))) == 0);
    f.run(); CHECK(f.state.svc == 15); // resume after SVC, do not execute it again
    f.start(test_vfp); f.reg(0, data); f.reg(1, 0xaabbccdd);
    f.word(data + 8, 0xabcd330e); f.word(data + 12, 0xe66d1234);
    for (unsigned i = 16; i < 24; ++i) f.cpu.set_float_reg(i, std::bit_cast<float>(0x7fc00000u + i));
    f.cpu.set_fpscr(0x12345678);
    const auto before = f.cpu.save_context();
    f.run();
    CHECK(f.reg(0) == data + 16 && f.cpu.get_sp() == stack);
    CHECK(f.word(data + 16) == 0xabcd330e && f.word(data + 20) == 0xe66d1234);
    for (unsigned i = 0; i < 4; ++i) CHECK(f.word(data + i * 4) == 0xaabbccdd);
    const auto after = f.cpu.save_context();
    CHECK(std::memcmp(before.fpu_registers.data() + 16, after.fpu_registers.data() + 16, 8 * sizeof(float)) == 0);
    CHECK(f.cpu.get_fpscr() == 0x12345678);
    f.cpu.load_context(before);
    CHECK(std::bit_cast<uint32_t>(f.cpu.get_float_reg(17)) == 0x7fc00011);
}
void errors(Fixture &f) {
    f.start(test_unsupported);
    CHECK(f.cpu.run() == -1);
    CHECK(f.cpu.get_pc() == test_unsupported && !f.state.svc_called);
    CHECK(f.cpu.get_last_error().find("opcode=0x0000de2a") != std::string::npos);
    f.start(test_loop); f.cpu.set_instruction_budget(16);
    CHECK(f.cpu.run() == -1 && f.cpu.get_pc() == test_loop);
    CHECK(f.cpu.get_last_error().find("instruction budget exhausted") != std::string::npos);
    CHECK(f.cpu.get_last_error().find("opcode=0xf7ffbffe") != std::string::npos);
    f.start(test_memory); f.reg(0, 0); f.reg(1, 1);
    CHECK(f.cpu.run() == -1);
    CHECK(f.cpu.get_last_error().find("guest write fault address=0x00000004") != std::string::npos);
    f.start(0x83000000);
    CHECK(f.cpu.run() == -1);
    CHECK(f.cpu.get_last_error().find("guest fetch fault") != std::string::npos);
    CHECK(mem_set_permissions(f.memory, 0x81000000, 0x1000, MemPerm::ReadOnly));
    f.start(test_mov); CHECK(f.cpu.run() == -1);
    CHECK(f.cpu.get_last_error().find("guest fetch fault") != std::string::npos);
    CHECK(mem_set_permissions(f.memory, 0x81000000, 0x1000, MemPerm::ReadWriteExecute));
    // A split wide instruction must validate the second page before execution.
    const uint16_t movw_prefix = 0xf240;
    CHECK(mem_write(f.memory, 0x81000ffe, &movw_prefix, 2));
    f.start(0x81000ffe); CHECK(f.cpu.run() == -1);
    CHECK(f.cpu.get_pc() == 0x81000ffe && f.cpu.get_last_error().find("guest fetch fault") != std::string::npos);
    // Denied stores do not modify bytes, and denied loads do not change Rt.
    CHECK(mem_set_permissions(f.memory, data, 0x1000, MemPerm::ReadOnly));
    const uint32_t original = f.word(data + 4);
    f.start(test_memory); f.reg(0, data); f.reg(1, 0xbaadf00d); CHECK(f.cpu.run() == -1);
    CHECK(f.word(data + 4) == original);
    CHECK(mem_set_permissions(f.memory, data, 0x1000, MemPerm::WriteOnly));
    f.start(test_memory + 2); f.reg(0, data); f.reg(2, 0xdeadbeef); CHECK(f.cpu.run() == -1);
    CHECK(f.reg(2) == 0xdeadbeef && f.cpu.get_last_error().find("guest read fault") != std::string::npos);
    CHECK(mem_set_permissions(f.memory, data, 0x1000, MemPerm::ReadWriteExecute));
}
}
int main() {
    try {
        Fixture f;
        f.start(test_mov, N | C | V); f.run();
        CHECK(f.reg(3) == 0x8103face && f.reg(4) == 0x08000000 && f.reg(5) == 0xffffffff);
        CHECK(f.reg(6) == 0x00ab00ab && f.reg(8) == 0xab00ab00 && f.reg(9) == 0xabababab);
        CHECK((f.cpu.get_cpsr() & NZCV) == (N | C | V));
        CHECK(f.state.svc == 0x73 && f.cpu.get_pc() == test_mov + 34);
        arithmetic(f);
        it_context(f);
        memory_and_stack(f);
        control_and_vectors(f);
        errors(f);
        std::printf("Interpreter instruction checks passed (%u assertions)\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Interpreter instruction tests failed: %s\n", error.what());
        return 1;
    }
}
