#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vita3k::web {

/**
 * Small, host-independent memory model used by the browser bring-up target.
 * Vita addresses are offsets into the Wasm linear-memory backing store; page
 * permissions are metadata because Wasm cannot reproduce native page faults.
 */
enum class MemoryPermission : std::uint8_t {
    None = 0,
    Read = 1,
    Write = 2,
    ReadWrite = 3,
    Execute = 4,
    ReadExecute = 5,
    ReadWriteExecute = 7,
};

constexpr std::uint32_t page_size = 4096;

struct Translation {
    std::uint8_t *data = nullptr;
    std::uint32_t size = 0;
};

class Memory final {
public:
    // Capacity is a bounded low-address test arena, not the full Vita address space.
    // Partial trailing pages are never allocated. Page zero is reserved.
    explicit Memory(std::uint32_t size_bytes);
    Memory(const Memory &) = delete;
    Memory &operator=(const Memory &) = delete;

    std::uint32_t size() const noexcept;
    std::uint32_t allocate(std::uint32_t size_bytes, const char *name = nullptr);
    bool allocate_at(std::uint32_t address, std::uint32_t size_bytes, const char *name = nullptr);
    bool release(std::uint32_t address);

    bool set_permission(std::uint32_t address, std::uint32_t size_bytes, MemoryPermission permission);
    MemoryPermission permission(std::uint32_t address) const;
    bool valid(std::uint32_t address) const;
    bool valid_range(std::uint32_t address, std::uint32_t size_bytes) const;
    bool can_access(std::uint32_t address, std::uint32_t size_bytes, bool write) const;
    // Borrowed pointer: check permissions on every new access; do not retain it
    // across release or protection changes. Prefer read/write for guest accesses.
    std::optional<Translation> translate(std::uint32_t address, std::uint32_t size_bytes, bool write = false);
    bool read(std::uint32_t address, void *destination, std::uint32_t size_bytes) const;
    bool write(std::uint32_t address, const void *source, std::uint32_t size_bytes);

    const char *name(std::uint32_t address) const;

private:
    struct Allocation {
        std::uint32_t address;
        std::uint32_t first_page;
        std::uint32_t pages;
        std::string name;
    };

    std::vector<std::uint8_t> bytes_;
    std::vector<bool> allocated_;
    std::vector<MemoryPermission> permissions_;
    std::vector<Allocation> allocations_;
};

} // namespace vita3k::web
