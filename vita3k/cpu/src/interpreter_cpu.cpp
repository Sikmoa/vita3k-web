#include <cpu/impl/interpreter_cpu.h>
#include <cpu/state.h>
#include <mem/ptr.h>

#include <bit>
#include <cstring>

namespace {
constexpr uint32_t n_flag = 1u << 31;
constexpr uint32_t z_flag = 1u << 30;
constexpr uint32_t c_flag = 1u << 29;
constexpr uint32_t v_flag = 1u << 28;

uint32_t load32(CPUState *state, uint32_t address) {
    return *Ptr<const uint32_t>(address).get(*state->mem);
}
uint16_t load16(CPUState *state, uint32_t address) {
    return *Ptr<const uint16_t>(address).get(*state->mem);
}
}

InterpreterCPU::InterpreterCPU(CPUState *state, std::size_t processor_id)
    : parent(state), core_id(processor_id) {}

int InterpreterCPU::run() {
    stopped = false;
    parent->svc_called = false;
    while (!stopped) {
        const int result = step();
        if (result != 0 || parent->svc_called)
            return result;
    }
    return 1;
}

void InterpreterCPU::stop() { stopped = true; }
uint32_t InterpreterCPU::get_reg(uint8_t idx) { return regs[idx & 15]; }
void InterpreterCPU::set_reg(uint8_t idx, uint32_t value) { regs[idx & 15] = value; }
uint32_t InterpreterCPU::get_sp() { return regs[13]; }
void InterpreterCPU::set_sp(uint32_t value) { regs[13] = value; }
uint32_t InterpreterCPU::get_pc() { return regs[15]; }
void InterpreterCPU::set_pc(uint32_t value) {
    if (value & 1) { cpsr |= 0x20; regs[15] = value & ~1u; }
    else { cpsr &= ~0x20u; regs[15] = value & ~3u; }
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

int InterpreterCPU::step() {
    parent->svc_called = false;
    if (stopped) return 1;
    const uint32_t pc = regs[15];
    if (is_thumb_mode()) {
        const uint16_t instruction = load16(parent, pc);
        regs[15] = pc + 2;
        if ((instruction & 0xff00u) == 0xdf00u) {
            regs[15] = pc;
            parent->svc = instruction & 0xffu;
            parent->svc_called = true;
            return 0;
        }
        if ((instruction & 0xf800u) == 0x2000u) {
            const uint32_t rd = (instruction >> 8) & 7;
            regs[rd] = instruction & 0xffu;
            cpsr = (cpsr & ~(n_flag | z_flag)) | (regs[rd] & n_flag) | (regs[rd] ? 0 : z_flag);
            return 0;
        }
        return -1;
    }
    const uint32_t instruction = load32(parent, pc);
    regs[15] = pc + 4;
    if ((instruction & 0x0f000000u) == 0x0f000000u) {
        regs[15] = pc;
        parent->svc = instruction & 0x00ffffffu;
        parent->svc_called = true;
        return 0;
    }
    if ((instruction & 0x0fe00000u) == 0x03a00000u) {
        const uint32_t rd = (instruction >> 12) & 15;
        regs[rd] = instruction & 0xffu;
        if (instruction & (1u << 20))
            cpsr = (cpsr & ~(n_flag | z_flag)) | (regs[rd] & n_flag) | (regs[rd] ? 0 : z_flag);
        return 0;
    }
    return -1;
}

bool InterpreterCPU::is_thumb_mode() { return (cpsr & 0x20) != 0; }
bool InterpreterCPU::hit_breakpoint() { return breakpoint; }
void InterpreterCPU::trigger_breakpoint() { breakpoint = true; stopped = true; }
void InterpreterCPU::set_log_code(bool value) { log_code = value; }
void InterpreterCPU::set_log_mem(bool value) { log_mem = value; }
bool InterpreterCPU::get_log_code() { return log_code; }
bool InterpreterCPU::get_log_mem() { return log_mem; }
void InterpreterCPU::clear_exclusive() {}
std::size_t InterpreterCPU::processor_id() const { return core_id; }
