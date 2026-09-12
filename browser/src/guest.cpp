#include "guest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace vita3k::web {
namespace {
constexpr std::uint32_t test_exit_service = InterpreterState::test_exit_service;
constexpr std::uint32_t elf_header_size = 52;
constexpr std::uint32_t elf_program_header_size = 32;
constexpr std::uint32_t load_flag_execute = 1;
constexpr std::uint32_t load_flag_write = 2;
constexpr std::uint32_t load_flag_read = 4;

std::uint16_t u16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t u32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
bool in_file(std::size_t offset, std::size_t size, std::size_t total) {
    return offset <= total && size <= total - offset;
}

} // namespace

bool load_elf32(Memory &memory, std::span<const std::uint8_t> bytes, ElfLoadResult &result, std::string &error) {
    result = {};
    if (bytes.size() < elf_header_size || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F'
        || bytes[4] != 1 || bytes[5] != 1 || u16(bytes.data() + 16) != 2 || u16(bytes.data() + 18) != 40
        || u16(bytes.data() + 40) != elf_header_size || u16(bytes.data() + 42) != elf_program_header_size) {
        error = "invalid ELF32 ARM image";
        return false;
    }
    const auto entry = u32(bytes.data() + 24);
    result.flags = u32(bytes.data() + 36);
    const auto phoff = u32(bytes.data() + 28);
    const auto phnum = u16(bytes.data() + 44);
    if (!in_file(phoff, static_cast<std::size_t>(phnum) * elf_program_header_size, bytes.size())) {
        error = "ELF program headers exceed image";
        return false;
    }
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const auto *ph = bytes.data() + phoff + static_cast<std::size_t>(i) * elf_program_header_size;
        if (u32(ph) != 1) continue;
        const auto address = u32(ph + 12);
        const auto file_offset = u32(ph + 4);
        const auto file_size = u32(ph + 16);
        const auto memory_size = u32(ph + 20);
        if (memory_size < file_size || address == 0 || !in_file(file_offset, file_size, bytes.size())
            || static_cast<std::uint64_t>(address) + memory_size > memory.size()) {
            error = "invalid ELF load segment";
            return false;
        }
        const auto first = address & ~(page_size - 1);
        const auto end = static_cast<std::uint64_t>(address) + memory_size;
        const auto mapped_size = static_cast<std::uint32_t>(end - first);
        if (!memory.allocate_at(first, mapped_size, "ELF PT_LOAD")) {
            error = "ELF load segment address is unavailable";
            return false;
        }
        if (file_size && !memory.write(address, bytes.data() + file_offset, file_size)) {
            error = "ELF load segment write failed";
            return false;
        }
        MemoryPermission permission = MemoryPermission::None;
        const auto flags = u32(ph + 24);
        if (flags & load_flag_read) permission = static_cast<MemoryPermission>(static_cast<unsigned>(permission) | 1);
        if (flags & load_flag_write) permission = static_cast<MemoryPermission>(static_cast<unsigned>(permission) | 2);
        if (flags & load_flag_execute) permission = static_cast<MemoryPermission>(static_cast<unsigned>(permission) | 4);
        if (!memory.set_permission(first, mapped_size, permission)) {
            error = "ELF load segment permission setup failed";
            return false;
        }
        result.segments.push_back(first);
    }
    if (result.segments.empty() || entry == 0) {
        error = "ELF image has no loadable segments";
        return false;
    }
    result.thumb = (entry & 1) != 0;
    result.entry = entry & (result.thumb ? ~1u : ~3u);
    if (!memory.valid(result.entry)) {
        error = "ELF entrypoint is not mapped";
        return false;
    }
    return true;
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
        const bool stepped = interpreter.step();
        if (stepped && interpreter.step_result() == StepResult::Trap && interpreter.state().trap
            && interpreter.state().trap->number == test_exit_service) {
            ++result.instructions;
            result.status = GuestResult::Status::Exited;
            result.exit_code = static_cast<std::int32_t>(interpreter.state().registers[0]);
            result.message = "guest exited through test SVC service";
            return result;
        }
        if (!stepped) {
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
