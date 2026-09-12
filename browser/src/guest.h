#pragma once

#include "interpreter.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <span>

namespace vita3k::web {

struct GuestResult {
    enum class Status : std::uint8_t { Exited, Fault, Unsupported, Limit };
    Status status = Status::Fault;
    std::int32_t exit_code = 0;
    std::size_t instructions = 0;
    std::string message;
};

// Minimal versioned image used only by the browser vertical-slice test.
struct GuestImage {
    static constexpr std::uint32_t magic = 0x33544756; // "VGT3"
    static constexpr std::uint32_t header_size = 16;
    std::uint32_t entry = 0;
    bool thumb = false;
    std::vector<std::uint8_t> code;
};

struct ElfImport {
    std::uint32_t module_name = 0;
    std::uint32_t function_table = 0;
    std::uint32_t variable_table = 0;
    std::uint16_t function_count = 0;
    std::uint16_t variable_count = 0;
};

struct ElfLoadResult {
    std::uint32_t entry = 0;
    bool thumb = false;
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> segments;
    std::vector<ElfImport> imports;
};

// Loads a minimal ELF32 little-endian ARM image (PT_LOAD segments only).
bool load_elf32(Memory &memory, std::span<const std::uint8_t> bytes, ElfLoadResult &result, std::string &error);

bool load_guest_image(Memory &memory, const GuestImage &image, std::uint32_t address, std::string &error);
GuestResult run_guest(Memory &memory, std::uint32_t entry, bool thumb, std::size_t instruction_limit);

} // namespace vita3k::web
