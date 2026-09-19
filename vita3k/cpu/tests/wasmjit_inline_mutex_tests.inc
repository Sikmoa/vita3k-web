// Included by wasmjit_backend_test.cpp; no guest assets or kernel mocks.
// Direct emitted-module matrix plus real ARM stubs through the CPU dispatcher.
namespace inline_mutex_tests {
using namespace vita3k::wasmjit;
using Reason = ExitReason;
constexpr uint32_t work = data + page + 0x100;
constexpr uint32_t tid = 17, uid = 91;

void mirror(MemState &mem, const InlineMutexEntry &entry) {
    const uint32_t words[8]{entry.owner, 0x12345678, entry.count, entry.attr,
        entry.uid, 0x87654321, 0xabcdef01, 0xfedcba98};
    CHECK(mem_write(mem, entry.workarea, words, sizeof(words)));
}
InlineMutexEntry seed(InlineMutexTable &table, MemState &mem,
    uint32_t count = 0, uint32_t owner = kInlineMutexNoOwner, uint32_t attr = 0) {
    table = {};
    auto &entry = table.entries[inline_mutex_index(work)];
    entry = {work, uid, owner, count, attr, 1, 0, 0};
    mirror(mem, entry);
    return entry;
}
std::array<uint32_t, 8> read_work(MemState &mem) {
    std::array<uint32_t, 8> words{};
    CHECK(mem_read(mem, work, words.data(), sizeof(words)));
    return words;
}
JitState state_for(MemState &mem, InlineMutexTable &table, uint32_t request) {
    auto s = vector_tests::initial(2);
    s.memory_cookie = reinterpret_cast<uintptr_t>(&mem);
    s.page_table_base = mem.sparse_host_memory ? reinterpret_cast<uintptr_t>(mem.page_table.get()) : 0;
    s.page_perms_base = reinterpret_cast<uintptr_t>(mem.page_permissions.get());
    s.code_pages_base = reinterpret_cast<uintptr_t>(g_code_pages.data());
    s.mutex_table = reinterpret_cast<uintptr_t>(&table);
    s.guest_thread_id = tid;
    s.regs[0] = work;
    s.regs[1] = request;
    s.exclusive_size = 4;
    return s;
}

void emitted_matrix(MemState &mem) {
    const uint32_t svc = 0xef000000;
    CHECK(mem_write(mem, code, &svc, sizeof(svc)));
    auto ir = translate_block(mem, code, 0x10, 1, 0);
    InlineMutexTable table;
    // Single-block + region flags/accounting/bases matrix. One module per
    // policy/NID; exercise all guards without recompiling the cases.
    for (unsigned mode = 0; mode < 9; ++mode) {
        const bool region = mode != 0;
        const unsigned policy = mode - 1;
        const RegionStateOptions options{bool(policy & 1), bool(policy & 2), bool(policy & 4)};
        for (const auto nid : {kInlineMutexLockNid, kInlineMutexUnlockNid, kInlineMutexUnlock2Nid}) {
            const bool locking = nid == kInlineMutexLockNid;
            const auto bytes = region
                ? emit_region({&ir}, {{code, PSR_DISPATCH_MASK, 0x10 & PSR_DISPATCH_MASK, 1, {}, nid}}, options)
                : emit_block(ir, nid);
            if (bytes.empty()) {
                std::fprintf(stderr, "Inline emit rejected mode=%u nid=%08x pc=%08x end=%08x ticks=%zu size=%zu\n",
                    mode, nid, Dynarmic::A32::LocationDescriptor(ir.Location()).PC(),
                    Dynarmic::A32::LocationDescriptor(ir.EndLocation()).PC(), ir.CycleCount(), ir.size());
                for (const auto &inst : ir)
                    std::fprintf(stderr, "  %s\n", Dynarmic::IR::GetNameOf(inst.GetOpcode()).c_str());
            }
            CHECK(!bytes.empty());
            const int slot = region
                ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
                : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
            CHECK(slot >= 0);
            const auto invoke = [&](JitState &s) {
                return static_cast<Reason>(region
                    ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&s), 1)
                    : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&s)));
            };
            const auto check_case = [&](uint32_t count, uint32_t owner, uint32_t attr,
                                        uint32_t request, bool fast, auto alter) {
                seed(table, mem, count, owner, attr);
                auto s = state_for(mem, table, request);
                alter(s, table.entries[inline_mutex_index(work)]);
                const auto before = s;
                const auto table_before = table;
                const auto words_before = read_work(mem);
                const auto reason = invoke(s);
                CHECK(reason == (fast ? (region ? Reason::Miss : Reason::Continue) : Reason::Svc));
                CHECK(s.regs[15] == code + 4 && s.executed == 1);
                CHECK(s.cpsr == before.cpsr && s.fpscr == before.fpscr);
                CHECK(std::memcmp(s.fpu, before.fpu, sizeof(s.fpu)) == 0);
                for (unsigned r = 1; r < 15; ++r) CHECK(s.regs[r] == before.regs[r]);
                CHECK(s.mutex_fast_take == unsigned(fast && locking));
                CHECK(s.mutex_fast_release == unsigned(fast && !locking));
                CHECK(s.mutex_fast_fallback == unsigned(!fast));
                const auto words = read_work(mem);
                if (fast) {
                    const auto &entry = table.entries[inline_mutex_index(work)];
                    const uint32_t result = locking ? count + request : count - request;
                    CHECK(s.regs[0] == 0 && s.exclusive_size == 0);
                    CHECK(entry.count == result && words[2] == result);
                    CHECK(entry.owner == (result ? tid : kInlineMutexNoOwner));
                    CHECK(words[0] == entry.owner);
                    CHECK(entry.dirty == 1 && entry.next_dirty == 0);
                    CHECK(table.dirty_head == inline_mutex_index(work) + 1);
                    for (unsigned i : {1, 3, 4, 5, 6, 7}) CHECK(words[i] == words_before[i]);
                } else {
                    CHECK(s.regs[0] == before.regs[0] && s.exclusive_size == before.exclusive_size);
                    CHECK(std::memcmp(&table, &table_before, sizeof(table)) == 0);
                    CHECK(words == words_before);
                }
            };
            const auto unchanged = [](auto &, auto &) {};
            const auto initial_count = locking ? 0u : 1u;
            const auto initial_owner = locking ? kInlineMutexNoOwner : tid;
            check_case(initial_count, initial_owner, 0, 1, true, unchanged);
            check_case(locking ? 0u : 3u, initial_owner, 0, 3, true, unchanged);
            check_case(3, tid, kInlineMutexRecursive, 2, true, unchanged);
            check_case(1, tid + 1, 0, 1, false, unchanged); // contention/non-owner unlock
            check_case(1, tid, 0, locking ? 1u : 2u, false, unchanged); // recursive/underflow error
            if (locking) check_case(INT32_MAX, tid, 2, 1, false, unchanged);
            for (const auto request : {0u, UINT32_MAX, 0x80000000u})
                check_case(initial_count, initial_owner, 0, request, false, unchanged);
            const auto decline = [&](auto change) {
                check_case(initial_count, initial_owner, 0, 1, false, change);
            };
            decline([](auto &s, auto &) { s.mutex_table = 0; });
            decline([](auto &, auto &e) { e.enabled = 0; }); // includes queued waiters and live HLE scopes
            decline([](auto &, auto &e) { ++e.uid; });
            decline([](auto &, auto &e) { e.workarea += 32; }); // hash collision/tag check
            decline([](auto &, auto &e) { e.attr ^= 2; });
            decline([](auto &, auto &e) { ++e.count; });
            decline([](auto &, auto &e) { e.owner ^= 64; });
            decline([](auto &s, auto &) { s.guest_thread_id = 0; });
            decline([](auto &s, auto &) { s.guest_thread_id = UINT32_MAX; });
            decline([](auto &s, auto &) { s.regs[0] = 0; });
            decline([](auto &s, auto &) { s.regs[0] = work + 1; });
            decline([](auto &s, auto &) { s.regs[0] = data + page - 4; });
            decline([](auto &s, auto &) { s.regs[0] = 0x83000000; }); // unmapped
            decline([](auto &s, auto &) { s.page_perms_base = 0; });
            decline([](auto &s, auto &) { s.code_pages_base = 0; });
            if (!vita3k::memory::direct_memory64)
                decline([](auto &s, auto &) { s.page_table_base = 0; });
            decline([&](auto &, auto &) {
                CHECK(mem_set_permissions(mem, data + page, page, MemPerm::ReadOnly));
            });
            CHECK(mem_set_permissions(mem, data + page, page, MemPerm::ReadWrite));
            ++g_code_pages[work >> 12];
            decline(unchanged); // inlining must not bypass code-write/SMC checks
            --g_code_pages[work >> 12];

            // Dirty-list membership is once per phase, not once per call.
            seed(table, mem, locking ? 0 : 4, locking ? kInlineMutexNoOwner : tid, 2);
            for (unsigned i = 0; i < 4; ++i) {
                auto s = state_for(mem, table, 1);
                CHECK(invoke(s) == (region ? Reason::Miss : Reason::Continue));
                CHECK(table.dirty_head == inline_mutex_index(work) + 1);
                CHECK(table.entries[inline_mutex_index(work)].next_dirty == 0);
            }
            if (region) vita3k_jit_release_region(slot);
            else vita3k_jit_release(slot);
        }
    }
    std::puts("Inline mutex: emitted guard/commit matrix passed (9 state policies, 3 NIDs)");
}

uint64_t metric(const WasmJitCPU &jit, const char *name) {
    const auto profile = jit.get_profile();
    const auto at = profile.find(name);
    CHECK(at != std::string::npos);
    return std::strtoull(profile.c_str() + at + std::strlen(name), nullptr, 10);
}
uint32_t branch(Address at, Address target, bool link = false, uint32_t condition = 0xe) {
    return (condition << 28) | (link ? 0x0b000000 : 0x0a000000)
        | (((target - at - 8) >> 2) & 0xffffff);
}
void backend(MemState &mem) {
    for (bool region : {false, true}) {
        CPUState parent{};
        parent.mem = &mem;
        parent.thread_id = tid;
        WasmJitCPU jit(&parent, 0);
        jit.set_region_mode(region);
        jit.set_instruction_budget(1000000);
        InlineMutexTable table;
        jit.set_inline_mutex_table(&table);
        const auto install = [&](uint32_t nid) {
            put(mem, jit, {0xef000000, 0xe1a0f00e, nid});
            const uint32_t tail = 0xef000042;
            CHECK(mem_write(mem, code + 0x100, &tail, 4));
        };
        const auto start = [&] {
            jit.set_cpsr(0x10);
            jit.set_pc(code);
            jit.set_lr(code + 0x100);
            jit.set_reg(0, work);
            jit.set_reg(1, 1);
        };
        install(kInlineMutexLockNid);
        seed(table, mem);
        start();
        CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);
        CHECK(table.entries[inline_mutex_index(work)].owner == tid);
        CHECK(jit.get_reg(0) == 0);
        // Patch ONLY the NID literal: it must be part of cache validation.
        seed(table, mem);
        const uint32_t unknown = 0xabcdef01;
        CHECK(mem_write(mem, code + 8, &unknown, 4));
        const auto invalidated = jit.invalidated_blocks();
        start();
        CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0);
        CHECK(jit.get_pc() == code + 4 && jit.get_reg(0) == work);
        CHECK(table.dirty_head == 0 && jit.invalidated_blocks() > invalidated);
        // A replaced return instruction must also remove the specialization.
        install(kInlineMutexLockNid);
        const uint32_t nop = 0xe1a00000;
        CHECK(mem_write(mem, code + 4, &nop, 4));
        seed(table, mem); start();
        CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0);
        CHECK(table.dirty_head == 0);

        // The real MOV PC,LR must retain interworking, not assume ARM callers.
        install(kInlineMutexLockNid);
        const uint32_t thumb_svc = 0x0000df42;
        CHECK(mem_write(mem, code + 0x100, &thumb_svc, 4));
        seed(table, mem); start();
        jit.set_cpsr(0xf80f0010);
        jit.set_lr((code + 0x100) | 1);
        CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);
        CHECK(jit.get_reg(0) == 0 && jit.get_pc() == code + 0x102);
        CHECK(jit.get_cpsr() == 0xf80f0030);

        // Tight real ARM caller loop. After warmup, every successful pair
        // stays inside the dispatch pump; only the final diagnostic SVC exits.
        constexpr Address unlock = code + 0x20, loop = code + 0x100;
        install(kInlineMutexLockNid);
        const uint32_t unlock_words[]{0xef000000, 0xe1a0f00e, kInlineMutexUnlock2Nid};
        CHECK(mem_write(mem, unlock, unlock_words, sizeof(unlock_words)));
        const uint32_t words[]{
            0xe1a00004, 0xe3a01001, branch(loop + 8, code, true),
            0xe1a00004, 0xe3a01001, branch(loop + 20, unlock, true),
            0xe2555001, branch(loop + 28, loop, false, 1), 0xef000042};
        CHECK(mem_write(mem, loop, words, sizeof(words)));
        jit.invalidate_jit_cache(code, page);
        seed(table, mem);
        for (unsigned pass = 0; pass < 2; ++pass) {
            const auto svc_before = metric(jit, "svc_exits=");
            const auto calls_before = metric(jit, "js_calls=");
            const auto take_before = metric(jit, "mutex_take=");
            const auto release_before = metric(jit, "mutex_release=");
            jit.set_cpsr(0x10); jit.set_pc(loop); jit.set_reg(4, work); jit.set_reg(5, 1000);
            CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);
            CHECK(jit.get_reg(5) == 0 && jit.get_reg(0) == 0);
            CHECK(metric(jit, "svc_exits=") - svc_before == 1);
            CHECK(metric(jit, "mutex_take=") - take_before == 1000);
            CHECK(metric(jit, "mutex_release=") - release_before == 1000);
            if (region && pass == 1) CHECK(metric(jit, "js_calls=") - calls_before == 1);
            const auto &entry = table.entries[inline_mutex_index(work)];
            CHECK(entry.count == 0 && entry.owner == kInlineMutexNoOwner);
            CHECK(entry.dirty == 1 && entry.next_dirty == 0);
        }
        jit.set_cpsr(0x10); jit.set_pc(loop); jit.set_reg(4, work); jit.set_reg(5, 100000);
        CHECK(jit.run_slice(128) == WasmJitCPU::slice_yield); // fairness is still bounded
        CHECK(!parent.svc_called && jit.get_reg(5) != 0);
        const auto words_after = read_work(mem);
        const auto &entry = table.entries[inline_mutex_index(work)];
        CHECK(words_after[0] == entry.owner && words_after[2] == entry.count);
    }
    std::puts("Inline mutex: real stubs, NID/return patching, 1000-pair single-phase loop and bounded slices passed");
}
void run(MemState &mem) { emitted_matrix(mem); backend(mem); }
} // namespace inline_mutex_tests
