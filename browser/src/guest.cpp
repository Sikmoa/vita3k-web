#include "guest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace vita3k::web {
namespace {
constexpr std::uint32_t svc_exit_arm = 0xef000001u;
constexpr std::uint16_t svc_exit_thumb = 0xdf01u;

}

bool load_guest_image(Memory &memory, const GuestImage &image, std::uint32_t address, std::string &error) {
    if (image.code.empty() || image.code.size() > memory.size() || address == 0
        || static_cast<std::uint64_t>(address) + image.code.size() > memory.size()) {
        error = "guest image does not fit in memory";
        return false;
    }
    if (!memory.allocate_at(address, static_cast<std::uint32_t>(image.code.size()), "guest code")) {
        error = "guest code address is unavailable";
        return false;
    }
    if (!memory.write(address, image.code.data(), static_cast<std::uint32_t>(image.code.size()))) {
        error = "guest code write failed";
        memory.release(address);
        return false;
    }
    return true;
}

GuestResult run_guest(Memory &memory, std::uint32_t entry, bool thumb, std::size_t instruction_limit) {
    GuestResult result;
    Interpreter interpreter(memory);
    interpreter.reset(entry, thumb);
    while (result.instructions < instruction_limit) {
        const auto pc = interpreter.state().registers[15];
        if ((!thumb && memory.valid_range(pc, 4))) {
            std::uint32_t instruction = 0;
            memory.read(pc, &instruction, sizeof(instruction));
            if (instruction == svc_exit_arm) {
                result.status = GuestResult::Status::Exited;
                result.exit_code = static_cast<std::int32_t>(interpreter.state().registers[0]);
                result.message = "guest exited through sceKernelExitProcess probe";
                return result;
            }
        } else if (thumb && memory.valid_range(pc, 2)) {
            std::uint16_t instruction = 0;
            memory.read(pc, &instruction, sizeof(instruction));
            if (instruction == svc_exit_thumb) {
                result.status = GuestResult::Status::Exited;
                result.exit_code = static_cast<std::int32_t>(interpreter.state().registers[0]);
                result.message = "guest exited through sceKernelExitProcess probe";
                return result;
            }
        }
        if (!interpreter.step()) {
            result.status = interpreter.step_result() == StepResult::MemoryFault
                ? GuestResult::Status::Fault : GuestResult::Status::Unsupported;
            result.message = result.status == GuestResult::Status::Fault
                ? "guest memory fault" : "guest instruction unsupported";
            return result;
        }
        ++result.instructions;
    }
    result.status = GuestResult::Status::Limit;
    result.message = "guest instruction limit reached";
    return result;
}

} // namespace vita3k::web
