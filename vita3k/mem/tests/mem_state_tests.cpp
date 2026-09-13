// Focused tests of the production MemState implementation (no memory API mocks).
#include <mem/ptr.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

static unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)
constexpr uint32_t P = 4096;
static uint8_t *ptr(MemState &mem, Address addr) { return Ptr<uint8_t>(addr).get(mem); }

static void allocator_edges() {
    BitmapAllocator bitmap(65);
    uint32_t count = 2;
    CHECK(bitmap.allocate_from(17, count) == 17);
    CHECK(bitmap.free_slot_count(0, 17) == 17);
    CHECK(bitmap.allocate_at(64, 2) == -1);
    CHECK(bitmap.allocate_at(0, 0) == -1);
    CHECK(bitmap.allocate_at(UINT32_MAX, 2) == -1);
    CHECK(bitmap.free_slot_count(64, 66) == -1);
    count = 1;
    CHECK(bitmap.allocate_from(65, count) == -1);
    count = 0;
    CHECK(bitmap.allocate_from(0, count) == -1);
    bitmap.reset();
    CHECK(bitmap.max_offset == 0);
    CHECK(bitmap.allocate_at(0, 1) == -1);
    bitmap.set_maximum(65);
    count = 1;
    CHECK(bitmap.allocate_from(64, count, true) == 64);
    CHECK(bitmap.allocate_from(64, count, true) == -1);
}

static void initial_state(MemState &mem, bool table) {
    CHECK(init(mem, table));
    CHECK(mem.allocator.max_offset == 1048576);
    CHECK(mem_available(mem) == uint64_t(0x100000000ULL) - mem.host_page_size);
#ifdef __EMSCRIPTEN__
    CHECK(sizeof(size_t) == 4);
    CHECK(mem.sparse_host_memory && mem.use_page_table && !mem.memory);
    CHECK(mem.sparse_allocations.empty());
    CHECK(mem.page_table[0] == nullptr && mem.page_table[1048575] == nullptr);
#else
    CHECK(!mem.sparse_host_memory && mem.use_page_table == table && mem.memory);
    if (table) {
        CHECK(mem.page_table[0] == mem.memory.get());
        CHECK(mem.page_table[1048575] == mem.memory.get());
    }
#endif
    CHECK(!is_valid_addr(mem, 0) && !is_valid_addr(mem, 1));
    CHECK(!is_valid_addr_range(mem, 0, P));
    CHECK(!is_valid_addr_range(mem, 0x90000000, 0x80000000));
    CHECK(!is_valid_addr_range(mem, 0x80000000, 0x80000000));
    CHECK(!Ptr<uint8_t>(0).get(mem));
    CHECK(alloc(mem, 0, "zero") == 0);
    CHECK(alloc(mem, UINT32_MAX, "overflow") == 0);
    CHECK(try_alloc_at(mem, 0, P, "null") == 0);
    uint32_t out = 0x12345678;
    CHECK(!mem_read(mem, 0, &out, sizeof(out)) && out == 0x12345678);
    CHECK(!mem_write(mem, 1, &out, sizeof(out)));
    CHECK(!mem_fetch(mem, 0x80000000, &out, sizeof(out)));
    CHECK(mem_read(mem, 0, nullptr, 0));
    CHECK(mem_write(mem, UINT32_MAX, nullptr, 0));
    CHECK(mem_fetch(mem, 0, nullptr, 0));
    CHECK(!mem_read(mem, 0x80000000, nullptr, 1));
}

static void contiguous_segment(MemState &mem) {
    constexpr Address base = 0x81001000;
    constexpr size_t size = 0x32123;
    CHECK(alloc_at(mem, base, size, "segment") == base);
    auto *host = ptr(mem, base);
    CHECK(host != nullptr);
    CHECK(std::all_of(host, host + size, [](uint8_t v) { return v == 0; }));
    std::vector<uint8_t> data(size), output(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = uint8_t(i * 37 + 11);
    // This bulk Ptr+length copy was invalid with independent malloc(4096) pages.
    std::memcpy(host, data.data(), size);
    CHECK(mem_read(mem, base, output.data(), size) && output == data);
    for (size_t i = 0; i < size; i += P) {
        CHECK(ptr(mem, base + i) == host + i);
        CHECK(Ptr<uint8_t>(host + i + 7, mem).address() == base + i + 7);
    }
    CHECK(Ptr<const uint8_t>(host + size - 1, mem).address() == base + size - 1);
    Address reverse = 123;
    CHECK(!mem_host_to_guest(mem, data.data(), reverse) && reverse == 0);
    CHECK(Ptr<uint8_t>(nullptr, mem).address() == 0);
    CHECK(!mem_host_to_guest(mem, reinterpret_cast<void *>(uintptr_t(1)), reverse));
    if (!mem.sparse_host_memory)
        CHECK(host == mem.memory.get() + base);
    else
        CHECK(mem.sparse_allocations.size() == 1);
    CHECK(mem_set_permissions(mem, base, size, MemPerm::ReadExecute));
    CHECK(mem_fetch(mem, base + P - 2, output.data(), 4));
    CHECK(!mem_write(mem, base, data.data(), 1));
    unprotect_inner(mem, base, size);
    free(mem, base);
    CHECK(!is_valid_addr(mem, base));
    CHECK(!mem_host_to_guest(mem, host, reverse));
    CHECK(!mem_read(mem, base, output.data(), 1));
    if (mem.sparse_host_memory) {
        CHECK(ptr(mem, base) == nullptr);
        CHECK(mem.sparse_allocations.empty());
    }
}

static void adjacent_and_permissions(MemState &mem) {
    constexpr Address base = 0x83000000;
    CHECK(alloc_at(mem, base, P, "left") == base);
    CHECK(alloc_at(mem, base + P, P, "right") == base + P);
    const std::array<uint8_t, 8> value = {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<uint8_t, 8> out{};
    CHECK(mem_write(mem, base + P - 3, value.data(), value.size()));
    CHECK(mem_read(mem, base + P - 3, out.data(), out.size()) && out == value);
    CHECK(mem_set_permissions(mem, base + P + 100, 1, MemPerm::ReadOnly));
    CHECK(mem.page_permissions[base / P + 1] == MemPerm::ReadOnly);
    std::array<uint8_t, 8> replacement{};
    CHECK(!mem_write(mem, base + P - 3, replacement.data(), replacement.size()));
    CHECK(mem_read(mem, base + P - 3, out.data(), out.size()) && out == value);
    out.fill(0xa5);
    CHECK(!mem_fetch(mem, base + P - 3, out.data(), out.size()));
    CHECK(std::all_of(out.begin(), out.end(), [](uint8_t v) { return v == 0xa5; }));
    CHECK(mem_set_permissions(mem, base + P, P, MemPerm::WriteOnly));
    CHECK(!mem_read(mem, base + P, out.data(), 1));
    CHECK(mem_write(mem, base + P, value.data(), 1));
    CHECK(!mem_fetch(mem, base + P, out.data(), 1));
    CHECK(mem_set_permissions(mem, base + P, P, MemPerm::Execute));
    CHECK(mem_fetch(mem, base + P, out.data(), 1) && out[0] == 1);
    CHECK(!mem_read(mem, base + P, out.data(), 1));
    CHECK(!mem_write(mem, base + P, value.data(), 1));
    protect_inner(mem, base + P, P, MemPerm::None);
    CHECK(!mem_fetch(mem, base + P, out.data(), 1));
    CHECK(!mem_read(mem, base + P, out.data(), 1));
    CHECK(!mem_write(mem, base + P, value.data(), 1));
    CHECK(!mem_set_permissions(mem, base, 3 * P, MemPerm::None));
    CHECK(mem.page_permissions[base / P] == MemPerm::ReadWriteExecute);
    CHECK(!mem_set_permissions(mem, base, 1, static_cast<MemPerm>(8)));
    unprotect_inner(mem, base + P, P);
    free(mem, base + P);
    out.fill(0xa5);
    CHECK(!mem_read(mem, base + P - 3, out.data(), out.size()));
    CHECK(std::all_of(out.begin(), out.end(), [](uint8_t v) { return v == 0xa5; }));
    CHECK(!mem_write(mem, base + P - 3, replacement.data(), replacement.size()));
    CHECK(std::memcmp(ptr(mem, base + P - 3), value.data(), 3) == 0);
    CHECK(alloc_at(mem, base + P, P, "reused") == base + P);
    CHECK(mem.page_permissions[base / P + 1] == MemPerm::ReadWriteExecute);
    CHECK(std::all_of(ptr(mem, base + P), ptr(mem, base + P) + P, [](uint8_t v) { return v == 0; }));
    free(mem, base);
    free(mem, base + P);
}

static void high_addresses(MemState &mem) {
    CHECK(alloc_at(mem, 0xfffff000, P, "last-page") == 0xfffff000);
    uint8_t value = 0x5a, out = 0;
    CHECK(mem_write(mem, UINT32_MAX, &value, 1));
    CHECK(mem_read(mem, UINT32_MAX, &out, 1) && out == value);
    CHECK(Ptr<uint8_t>(ptr(mem, UINT32_MAX), mem).address() == UINT32_MAX);
    CHECK(mem_fetch(mem, UINT32_MAX, &out, 1));
    CHECK(is_valid_addr(mem, UINT32_MAX));
    CHECK(is_valid_addr_range(mem, 0xfffff000, UINT32_MAX));
    CHECK(!is_valid_addr_range(mem, 0xfffff000, 0));
    CHECK(!mem_read(mem, UINT32_MAX, &out, 2));
    CHECK(!mem_write(mem, UINT32_MAX, &value, 2));
    CHECK(!mem_fetch(mem, UINT32_MAX, &out, 2));
    CHECK(!mem_read(mem, 0xfffff000, &out, std::numeric_limits<size_t>::max()));
    CHECK(!mem_set_permissions(mem, UINT32_MAX, 2, MemPerm::None));
    CHECK(mem_set_permissions(mem, UINT32_MAX, 1, MemPerm::ReadExecute));
    CHECK(mem_fetch(mem, UINT32_MAX, &out, 1));
    CHECK(!mem_write(mem, UINT32_MAX, &value, 1));
    CHECK(add_protect(mem, UINT32_MAX, 1, MemPerm::None, [](Address, bool) { return true; }));
    CHECK(is_protecting(mem, UINT32_MAX));
    CHECK(!mem_fetch(mem, UINT32_MAX, &out, 1));
    CHECK(try_alloc_at(mem, UINT32_MAX, 2, "wrap") == 0);
    CHECK(try_alloc_at(mem, 0xfffff001, UINT32_MAX, "overflow") == 0);
    free(mem, 0xfffff000);
    CHECK(try_alloc_at(mem, UINT32_MAX, 1, "last-byte") == UINT32_MAX);
    CHECK(*ptr(mem, UINT32_MAX) == 0);
    free(mem, UINT32_MAX);
}

static void alignment_and_reuse(MemState &mem) {
    constexpr Address start = 0x85001000;
    const auto available = mem_available(mem);
    CHECK(alloc_aligned(mem, 0, "zero", 65536, start) == 0);
    CHECK(alloc_aligned(mem, 4, "bad-align", 3, start) == 0);
    CHECK(alloc_aligned(mem, UINT32_MAX, "overflow", 65536, start) == 0);
    CHECK(alloc_aligned(mem, P, "no-room", 65536, 0xfffff000) == 0);
    CHECK(mem_available(mem) == available);
    const Address aligned = alloc_aligned(mem, 3 * P + 19, "stack", 65536, start);
    CHECK(aligned == 0x85010000);
    CHECK(!is_valid_addr(mem, start));
    auto *stack = ptr(mem, aligned);
    std::memset(stack, 0x6c, 3 * P + 19);
    CHECK(ptr(mem, aligned + 3 * P) == stack + 3 * P);
    CHECK(Ptr<uint8_t>(stack + P + 1, mem).address() == aligned + P + 1);
    if (mem.sparse_host_memory) {
        CHECK(mem.sparse_allocations.size() == 1);
        CHECK(ptr(mem, start) == nullptr);
        Address reverse;
        CHECK(!mem_host_to_guest(mem, mem.sparse_allocations.begin()->second.memory.get(), reverse));
    }
    CHECK(alloc_at(mem, start, P, "trimmed-front") == start);
    std::memset(ptr(mem, start), 0x17, P);
    CHECK(stack[P] == 0x6c);
    free(mem, aligned);
    CHECK(ptr(mem, start)[0] == 0x17);
    free(mem, start);
    CHECK(mem_available(mem) == available);
    const Address unaligned = 0x85020003;
    CHECK(alloc_at(mem, unaligned, 2 * P, "unaligned") == unaligned);
    CHECK(ptr(mem, unaligned + 2 * P - 1) == ptr(mem, unaligned) + 2 * P - 1);
    CHECK(try_alloc_at(mem, unaligned + P, P, "overlap") == 0);
    free(mem, unaligned);
    CHECK(mem_available(mem) == available);
    const Address searched = alloc(mem, P, "search-start", start);
    CHECK(searched == start);
    free(mem, searched);
}

static void protections_and_atomic(MemState &mem) {
    constexpr Address base = 0x87000000;
    CHECK(alloc_at(mem, base, 3 * P, "protect") == base);
    unsigned callbacks = 0;
    auto callback = [&](Address address, bool write) { CHECK(address == base + P && write); ++callbacks; return true; };
    CHECK(add_protect(mem, base + P, P, MemPerm::ReadOnly, callback));
    CHECK(add_protect(mem, base + P - 10, 20, MemPerm::ReadOnly, callback));
    CHECK(mem.protect_tree.size() == 1);
    CHECK(is_protecting(mem, base));
    CHECK(!is_protecting(mem, base + 2 * P));
    uint32_t value = 5;
    CHECK(!mem_write(mem, base + P, &value, 4));
    CHECK(callbacks == 0); // Checked access never silently clears a permission.
    CHECK(handle_access_violation(mem, ptr(mem, base + P), true));
    CHECK(callbacks == 2 && mem.protect_tree.empty());
#ifndef __EMSCRIPTEN__
    CHECK(add_protect(mem, base + P, P, MemPerm::ReadOnly, callback));
    *reinterpret_cast<volatile uint32_t *>(ptr(mem, base + P)) = 0;
    CHECK(callbacks == 3 && mem.protect_tree.empty()); // Actual native OS fault path.
#endif
    auto atomic = Ptr<uint32_t>(base + P);
    CHECK(atomic.atomic_compare_and_swap(mem, uint32_t(9), uint32_t(0)));
    CHECK(!atomic.atomic_compare_and_swap(mem, uint32_t(3), uint32_t(0)));
    CHECK(*atomic.get(mem) == 9);
    CHECK(!Ptr<uint32_t>(0).atomic_compare_and_swap(mem, uint32_t(1), uint32_t(0)));
    CHECK(add_protect(mem, base, P, MemPerm::None, callback));
    free(mem, base);
    CHECK(mem.protect_tree.empty());
    CHECK(alloc_at(mem, base, P, "clean-protection") == base);
    CHECK(mem_write(mem, base, &value, 4));
    free(mem, base);
    CHECK(!add_protect(mem, base, P, MemPerm::None, callback));
}

static void external_mapping(MemState &mem) {
    if (!mem.use_page_table)
        return;
    constexpr Address base = 0x89000000;
    CHECK(alloc_at(mem, base, 2 * P, "external") == base);
    const size_t alignment = std::max<size_t>(P, mem.host_page_size);
    auto *external = static_cast<uint8_t *>(std::aligned_alloc(alignment, 2 * alignment));
    CHECK(external != nullptr);
    auto *original = ptr(mem, base);
    std::memset(original, 0x41, 2 * P);
    add_external_mapping(mem, base, 2 * P, external);
    CHECK(mem.external_mapping.size() == 1);
    CHECK(ptr(mem, base) == external && ptr(mem, base + P) == external + P);
    CHECK(external[0] == 0x41 && external[2 * P - 1] == 0x41);
    CHECK(Ptr<uint8_t>(external + P + 3, mem).address() == base + P + 3);
    Address reverse;
    CHECK(!mem_host_to_guest(mem, original, reverse));
    uint32_t value = 0x12345678;
    CHECK(mem_write(mem, base + P - 2, &value, 4));
    CHECK(std::memcmp(external + P - 2, &value, 4) == 0);
    remove_external_mapping(mem, external, 2 * P); // empty protection tree must be safe
    CHECK(mem.external_mapping.empty());
    CHECK(ptr(mem, base) == original);
    CHECK(std::memcmp(original + P - 2, &value, 4) == 0);
    CHECK(!mem_host_to_guest(mem, external, reverse));
    add_external_mapping(mem, base, 2 * P, external);
    free(mem, base); // must detach borrowed mapping before freeing owned backing
    CHECK(mem.external_mapping.empty());
    CHECK(!mem_host_to_guest(mem, external, reverse));
    std::free(external);

    // Teardown with a protected, still-borrowed external buffer must unprotect
    // and detach it; the caller remains responsible for the buffer's lifetime.
    CHECK(alloc_at(mem, base, 2 * P, "external-at-deinit") == base);
    external = static_cast<uint8_t *>(std::aligned_alloc(alignment, 2 * alignment));
    CHECK(external != nullptr);
    add_external_mapping(mem, base, 2 * P, external);
    CHECK(mem_set_permissions(mem, base, 2 * P, MemPerm::None));
    const bool table = mem.use_page_table;
    deinit_mem(mem);
    std::memset(external, 0, 2 * P);
    std::free(external);
    CHECK(init(mem, table));
}

static void growth_and_oom(MemState &mem) {
#ifdef __EMSCRIPTEN__
    const Address base = alloc(mem, 4 * P, "growth");
    CHECK(base != 0);
    auto *host = ptr(mem, base);
    std::memset(host, 0x73, 4 * P);
    const size_t before = emscripten_get_heap_size();
    CHECK(emscripten_resize_heap(48 * 1024 * 1024));
    CHECK(emscripten_get_heap_size() > before);
    CHECK(ptr(mem, base) == host && host[3 * P] == 0x73);
    CHECK(Ptr<uint8_t>(host + 3 * P, mem).address() == base + 3 * P);
    const auto available = mem_available(mem);
    const auto owners = mem.sparse_allocations.size();
    CHECK(alloc(mem, 64 * 1024 * 1024, "host-oom") == 0);
    CHECK(mem_available(mem) == available && mem.sparse_allocations.size() == owners);
    CHECK(try_alloc_at(mem, 0x92000000, 64 * 1024 * 1024, "fixed-oom") == 0);
    CHECK(mem_available(mem) == available);
    CHECK(alloc_aligned(mem, 64 * 1024 * 1024, "aligned-oom", 65536) == 0);
    CHECK(mem_available(mem) == available);
    CHECK(!is_valid_addr(mem, 0x92000000));
    CHECK(ptr(mem, base) == host && host[3 * P] == 0x73);
    free(mem, base);
    std::printf("Wasm heap growth: %zu -> %zu bytes; OOM rollback: PASS\n", before, emscripten_get_heap_size());
#else
    (void)mem;
#endif
}

int main() {
    allocator_edges();
#ifdef __EMSCRIPTEN__
    const unsigned modes = 1;
#else
    const unsigned modes = 2;
#endif
    for (unsigned mode = 0; mode < modes; ++mode) {
        MemState mem;
        initial_state(mem, mode != 0);
        const auto available = mem_available(mem);
        contiguous_segment(mem);
        adjacent_and_permissions(mem);
        high_addresses(mem);
        alignment_and_reuse(mem);
        protections_and_atomic(mem);
        external_mapping(mem);
        growth_and_oom(mem);
        CHECK(mem_available(mem) == available);
        CHECK(alloc(mem, P, "live-at-deinit") != 0);
        deinit_mem(mem);
        CHECK(!mem.memory && !mem.page_table && !mem.alloc_table && !mem.page_permissions);
        CHECK(mem.sparse_allocations.empty() && mem.allocator.max_offset == 0);
        CHECK(mem_available(mem) == 0 && !is_valid_addr(mem, 0x80000000));
        uint8_t output;
        CHECK(!mem_read(mem, 0x80000000, &output, 1));
        CHECK(!Ptr<uint8_t>(0x80000000).get(mem));
        CHECK(init(mem, mode != 0));
        CHECK(mem_available(mem) == available);
        const auto reused = alloc(mem, P, "after-reinit");
        CHECK(reused == 0x80000000 && *ptr(mem, reused) == 0);
        deinit_mem(mem);
        deinit_mem(mem);
        std::printf("MemState mode %u: PASS\n", mode);
    }
    std::printf("PASS: %u checks, real MemState (%s)\n", checks,
#ifdef __EMSCRIPTEN__
        "wasm32"
#else
        "native direct + native page table"
#endif
    );
}
