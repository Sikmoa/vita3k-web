#include "memory.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace vita3k::web {
namespace {
constexpr std::uint32_t page_count(std::uint32_t size) {
    return size == 0 ? 0 : 1 + (size - 1) / page_size;
}

bool range_overflows(std::uint32_t address, std::uint32_t size) {
    return size > std::numeric_limits<std::uint32_t>::max() - address;
}
} // namespace

Memory::Memory(std::uint32_t size_bytes)
    : bytes_(size_bytes)
    , allocated_((static_cast<std::uint64_t>(size_bytes) + page_size - 1) / page_size, false)
    , permissions_(allocated_.size(), MemoryPermission::None) {
}

std::uint32_t Memory::size() const noexcept {
    return static_cast<std::uint32_t>(bytes_.size());
}

std::uint32_t Memory::allocate(std::uint32_t size_bytes, const char *name) {
    const auto pages = page_count(size_bytes);
    if (pages == 0 || pages > permissions_.size())
        return 0;

    std::uint32_t run = 0;
    for (std::uint32_t page = 1; page < permissions_.size(); ++page) {
        if (!allocated_[page]) {
            if (++run == pages) {
                    const auto address = (page + 1 - pages) * page_size;
                return allocate_at(address, size_bytes, name) ? address : 0;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

bool Memory::allocate_at(std::uint32_t address, std::uint32_t size_bytes, const char *name) {
    const auto pages = static_cast<std::uint32_t>((static_cast<std::uint64_t>(address % page_size) + size_bytes + page_size - 1) / page_size);
    const auto first = address / page_size;
    const auto rounded_size = static_cast<std::uint64_t>(pages) * page_size;
    if (first == 0 || size_bytes == 0 || range_overflows(address, size_bytes)
        || static_cast<std::uint64_t>(address) + size_bytes > bytes_.size()
        || static_cast<std::uint64_t>(first) * page_size + rounded_size > bytes_.size())
        return false;
    for (std::uint32_t page = first; page < first + pages; ++page) {
        if (allocated_[page])
            return false;
    }
    allocations_.push_back({ address, first, pages, name ? name : "" });
    std::fill(allocated_.begin() + first, allocated_.begin() + first + pages, true);
    std::fill(permissions_.begin() + first, permissions_.begin() + first + pages, MemoryPermission::ReadWrite);
    const auto rounded_address = first * page_size;
    std::fill(bytes_.begin() + rounded_address, bytes_.begin() + rounded_address + rounded_size, 0);
    return true;
}

bool Memory::release(std::uint32_t address) {
    const auto it = std::find_if(allocations_.begin(), allocations_.end(), [address](const auto &allocation) {
        return allocation.address == address;
    });
    if (it == allocations_.end())
        return false;
    std::fill(allocated_.begin() + it->first_page,
        allocated_.begin() + it->first_page + it->pages, false);
    std::fill(permissions_.begin() + it->first_page,
        permissions_.begin() + it->first_page + it->pages, MemoryPermission::ReadWrite);
    allocations_.erase(it);
    return true;
}

bool Memory::set_permission(std::uint32_t address, std::uint32_t size_bytes, MemoryPermission permission_value) {
    if (static_cast<unsigned>(permission_value) > 3 || !valid_range(address, size_bytes))
        return false;
    const auto first = address / page_size;
    const auto last = static_cast<std::uint32_t>((static_cast<std::uint64_t>(address) + size_bytes + page_size - 1) / page_size);
    if (last > permissions_.size())
        return false;
    std::fill(permissions_.begin() + first, permissions_.begin() + last, permission_value);
    return true;
}

MemoryPermission Memory::permission(std::uint32_t address) const {
    return address < bytes_.size() && allocated_[address / page_size] ? permissions_[address / page_size] : MemoryPermission::None;
}

bool Memory::valid(std::uint32_t address) const {
    return address < bytes_.size() && allocated_[address / page_size];
}

bool Memory::valid_range(std::uint32_t address, std::uint32_t size_bytes) const {
    if (size_bytes == 0 || address == 0 || range_overflows(address, size_bytes)
        || static_cast<std::uint64_t>(address) + size_bytes > bytes_.size())
        return false;
    const auto first = address / page_size;
    const auto last = static_cast<std::uint32_t>((static_cast<std::uint64_t>(address) + size_bytes + page_size - 1) / page_size);
    return std::all_of(allocated_.begin() + first, allocated_.begin() + last,
        [](bool allocated) { return allocated; });
}

bool Memory::can_access(std::uint32_t address, std::uint32_t size_bytes, bool write) const {
    if (!valid_range(address, size_bytes))
        return false;
    const auto required = write ? MemoryPermission::Write : MemoryPermission::Read;
    const auto first = address / page_size;
    const auto last = static_cast<std::uint32_t>((static_cast<std::uint64_t>(address) + size_bytes + page_size - 1) / page_size);
    return std::all_of(permissions_.begin() + first, permissions_.begin() + last,
        [required](auto value) { return (static_cast<unsigned>(value) & static_cast<unsigned>(required)) != 0; });
}

std::optional<Translation> Memory::translate(std::uint32_t address, std::uint32_t size_bytes, bool write_access) {
    if (!can_access(address, size_bytes, write_access))
        return std::nullopt;
    return Translation { bytes_.data() + address, size_bytes };
}

bool Memory::read(std::uint32_t address, void *destination, std::uint32_t size_bytes) const {
    if (!destination || !can_access(address, size_bytes, false))
        return false;
    std::memcpy(destination, bytes_.data() + address, size_bytes);
    return true;
}

bool Memory::write(std::uint32_t address, const void *source, std::uint32_t size_bytes) {
    if (!source || !can_access(address, size_bytes, true))
        return false;
    std::memcpy(bytes_.data() + address, source, size_bytes);
    return true;
}

const char *Memory::name(std::uint32_t address) const {
    const auto it = std::find_if(allocations_.begin(), allocations_.end(), [address](const auto &allocation) {
        return address / page_size >= allocation.first_page && address / page_size < allocation.first_page + allocation.pages;
    });
    return it == allocations_.end() || it->name.empty() ? "" : it->name.c_str();
}

} // namespace vita3k::web
