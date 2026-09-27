// Production HLE bridge tests; no game assets or replacement allocator.
#pragma once
#include <dlmalloc.h>
#include <cstring>

inline void test_guest_mspace(EmuEnvState &env, ThreadState &thread) {
    constexpr uint32_t capacity = 65536;
    const Address backing = alloc(env.mem, capacity, "mspace fixture");
    REQUIRE(backing);
    auto call = [&](uint32_t nid, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
        auto &cpu = *thread.cpu;
        write_reg(cpu, 0, a); write_reg(cpu, 1, b); write_reg(cpu, 2, c);
        const auto sp = read_sp(cpu);
        call_import(env, cpu, nid, thread.id);
        REQUIRE(env.missing_nids.empty());
        REQUIRE(read_sp(cpu) == sp);
        return read_reg(cpu, 0);
    };
    auto in_heap = [&](Address p, uint32_t size) {
        REQUIRE(p >= backing);
        REQUIRE(uint64_t(p) + size <= uint64_t(backing) + capacity);
    };
    constexpr uint32_t is_empty = 0xD1D59701, stats = 0x8CC1D38E;
    const Address stats_out = alloc(env.mem, 16, "mspace stats");
    REQUIRE(stats_out);
    auto *out = Ptr<uint32_t>(stats_out).get(env.mem);
    const auto heap = call(0x3b9e301a, backing, capacity);
    REQUIRE(heap); in_heap(heap, 1);
    // The in-use counters follow dlmalloc's own count after every call.
    auto check_usage = [&](uint32_t peak) {
        std::memset(out, 0xcc, 16);
        REQUIRE(call(stats, heap, stats_out) == 0);
        const mspace host = Ptr<void>(heap).get(env.mem);
        REQUIRE(out[0] == mspace_max_footprint(host) && out[1] == mspace_footprint(host));
        REQUIRE(out[3] == mspace_mallinfo(host).uordblks && out[2] >= out[3]);
        if (peak)
            REQUIRE(out[2] == peak);
        return out[3];
    };
    const uint32_t empty = check_usage(0);
    REQUIRE(call(is_empty, heap) == 1);
    REQUIRE(call(stats, heap, 0) == 0);
    REQUIRE(call(stats, heap + 0x40, stats_out) == 1); // not an mspace: the heap check fails
    auto p = call(0x86ef7680, heap, 128);
    REQUIRE(p); in_heap(p, 128);
    std::memset(Ptr<void>(p).get(env.mem), 0xa5, 128);
    const auto zero = call(0x678374ad, heap, 32, 4);
    REQUIRE(zero && zero != p); in_heap(zero, 128);
    for (unsigned i = 0; i < 128; ++i) REQUIRE(Ptr<uint8_t>(zero).get(env.mem)[i] == 0);
    const auto aligned = call(0x3c847d57, heap, 256, 512);
    REQUIRE(aligned && aligned % 256 == 0); in_heap(aligned, 512);
    REQUIRE(call(is_empty, heap) == 0);
    const uint32_t before_realloc = check_usage(0);
    p = call(0x774891d6, heap, p, 4096);
    REQUIRE(p); in_heap(p, 4096);
    const uint32_t peak = check_usage(0);
    REQUIRE(peak > before_realloc);
    for (unsigned i = 0; i < 128; ++i) REQUIRE(Ptr<uint8_t>(p).get(env.mem)[i] == 0xa5);
    // No mmap/morecore fallback may allocate outside the supplied guest range.
    REQUIRE(call(0x86ef7680, heap, capacity * 2) == 0);
    REQUIRE(call(0x774891d6, heap, p, capacity * 2) == 0);
    for (unsigned i = 0; i < 128; ++i) REQUIRE(Ptr<uint8_t>(p).get(env.mem)[i] == 0xa5);
    check_usage(peak); // failed allocations change nothing
    call(0x9c56b4d1, heap, p);
    call(0x9c56b4d1, heap, zero);
    call(0x9c56b4d1, heap, aligned);
    REQUIRE(check_usage(peak) == empty && call(is_empty, heap) == 1);
    const auto reused = call(0x86ef7680, heap, 32768);
    REQUIRE(reused); in_heap(reused, 32768);
    check_usage(0);
    call(0x9c56b4d1, heap, reused);
    REQUIRE(call(is_empty, heap) == 1);
    // External backing belongs to the caller, not destroy_mspace.
    REQUIRE(call(0xae1a21ec, heap) == 0);
    std::memset(Ptr<void>(backing).get(env.mem), 0x5a, capacity);
    free(env.mem, backing);
    free(env.mem, stats_out);
    std::puts("Guest mspace: allocation, calloc, alignment, realloc, exhaustion, reuse, destroy, stats and empty check passed");
}
