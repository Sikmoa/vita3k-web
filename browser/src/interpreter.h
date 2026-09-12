#pragma once

#include "memory.h"

#include <cstddef>
#include <cstdint>

namespace vita3k::web {

enum class StepResult {
    Executed,
    MemoryFault,
    Unsupported,
    Halted,
};

struct InterpreterState {
    std::uint32_t registers[16]{};
    std::uint32_t cpsr = 0;
    bool thumb = false;
    bool halted = false;
    StepResult last_result = StepResult::Halted;
};

/** A deliberately small ARMv7 bring-up interpreter for browser tests.
 *
 * This is not yet a replacement for Vita3K's CPUInterface. It provides a
 * dependency-free execution seam over Memory and supports only the simple
 * control-flow/data-processing instructions listed in interpreter.cpp.
 */
class Interpreter final {
public:
    explicit Interpreter(Memory &memory) noexcept;

    void reset(std::uint32_t entry, bool thumb = false) noexcept;
    const InterpreterState &state() const noexcept;
    InterpreterState &state() noexcept;

    // Returns false for an unsupported instruction or a memory fault.
    bool step();
    StepResult step_result() const noexcept;
    std::size_t run(std::size_t instruction_limit);

private:
    Memory *memory_;
    InterpreterState state_;
};

} // namespace vita3k::web
