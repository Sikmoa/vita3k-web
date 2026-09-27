// Asset-free inline-mutex runtime regression tests (no user assets, no timing).
// Included by browser/tests/guest_thread_lifecycle_test.cpp which defines REQUIRE
// and provides EmuEnvState / GuestThreadRuntime / production HLE (call_import,
// mutex_create/lock/unlock/delete). Owns a fresh EmuEnvState + GuestThreadRuntime.
#pragma once
#include <cpu/inline_mutex.h>
#include <cstdio>
#include <cstring>

namespace inline_mutex_fixture {
constexpr uint32_t kLockNid = 0x46e7be7b;
constexpr uint32_t kUnlockNid = 0x120afc8c;
constexpr uint32_t kUnlockAltNid = 0x91fa6614;
constexpr uint32_t kDiagNid = 0xD1A60001; // synthetic, handled by test callback only

inline void emit_stub(MemState &mem, Address addr, uint32_t nid) {
    const uint32_t words[] = { 0xef000000u, 0xe1a0f00eu, nid }; // svc #0; mov pc,lr; NID word
    std::memcpy(Ptr<void>(addr).get(mem), words, sizeof(words));
}

// Tight lock/unlock loop body at `code`. Separate 3-word lock/unlock/diag stubs.
// r4 = data base (callee-saved across BLs), r5 = loop counter, LR preserved via push.
// data+0x00: result word (last lock r0), +0x04: unlock r0, +0x08: diag r0, +0x0c: iters done.
inline void build_tight_loop(MemState &mem, Address code, Address data, Address work,
    Address lock_stub, Address unlock_stub, Address diag_stub, unsigned iters) {
    emit_stub(mem, lock_stub, kLockNid);
    emit_stub(mem, unlock_stub, kUnlockNid);
    emit_stub(mem, diag_stub, kDiagNid);
    guest_thread_fixture::Arm p(code);
    p.emit(0xe92d4030); // push {r4,r5,lr}
    p.constant(4, data);
    p.constant(5, iters);
    const Address loop = p.pc();
    p.constant(0, work); p.constant(1, 1); p.constant(2, 0);
    p.call(lock_stub);
    p.store(0, 0x00);
    p.constant(0, work); p.constant(1, 1);
    p.call(unlock_stub);
    p.store(0, 0x04);
    // subs r5,r5,#1; bne loop
    p.emit(0xe2555001);
    {
        const int32_t d = static_cast<int32_t>(loop - (p.pc() + 8));
        p.emit(0x1a000000u | ((static_cast<uint32_t>(d) >> 2) & 0xffffffu));
    }
    p.store(5, 0x0c); // iters remaining (expect 0)
    p.call(diag_stub);
    p.store(0, 0x08);
    p.constant(0, 42);
    p.emit(0xe8bd8030); // pop {r4,r5,pc} return sentinel 42 in r0
    p.finish(mem);
}

// Holder/spinner: lock, publish marker, spin on host gate, unlock, return.
inline void build_holder(MemState &mem, Address code, Address data, Address work,
    Address lock_stub, Address unlock_stub) {
    emit_stub(mem, lock_stub, kLockNid);
    emit_stub(mem, unlock_stub, kUnlockNid);
    guest_thread_fixture::Arm p(code);
    p.emit(0xe92d4010);
    p.constant(4, data);
    p.constant(0, work); p.constant(1, 1); p.constant(2, 0);
    p.call(lock_stub);
    p.store(0, 0x40);
    p.constant(0, 1); p.store(0, 0x44);
    const Address gate = p.pc();
    p.load(0, 0x54);
    p.emit(0xe3500000);
    { const int32_t d = static_cast<int32_t>(gate - (p.pc() + 8)); p.emit(0x0a000000u | ((static_cast<uint32_t>(d) >> 2) & 0xffffffu)); }
    p.constant(0, work); p.constant(1, 1);
    p.call(unlock_stub);
    p.store(0, 0x4c);
    p.constant(0, 42);
    p.emit(0xe8bd8010);
    p.finish(mem);
}

inline void build_contender(MemState &mem, Address code, Address data, Address work, Address lock_stub) {
    emit_stub(mem, lock_stub, kLockNid);
    guest_thread_fixture::Arm p(code);
    p.emit(0xe92d4010);
    p.constant(4, data);
    const Address m = p.pc();
    p.load(0, 0x44);
    p.emit(0xe3500001);
    { const int32_t d = static_cast<int32_t>(m - (p.pc() + 8)); p.emit(0x1a000000u | ((static_cast<uint32_t>(d) >> 2) & 0xffffffu)); }
    p.constant(0, work); p.constant(1, 1); p.constant(2, 0);
    p.call(lock_stub);
    p.store(0, 0x50);
    p.constant(0, 2); p.store(0, 0x48);
    p.constant(0, 43);
    p.emit(0xe8bd8010);
    p.finish(mem);
}

inline void test_inline_mutex_runtime() {
    using vita3k::wasmjit::inline_mutex_index;
    using vita3k::wasmjit::kInlineMutexEntries;
    auto env = std::make_unique<EmuEnvState>();
    REQUIRE(init(env->mem, true));
    const Address code = alloc(env->mem, 8192, "inline mutex code");
    constexpr uint32_t data_size = 128 * 1024; // enough to find an actual 1024-slot collision
    const Address data = alloc(env->mem, data_size, "inline mutex data");
    REQUIRE(code && data);
    const Address work = data + 0x300;
    const Address lock_stub = code + 0x1000, unlock_stub = code + 0x1010, diag_stub = code + 0x1020;
    unsigned hle_locks = 0, hle_unlocks = 0, diag_calls = 0;
    SceUID observed_mutex = -1;
    const CallImportFunc import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        if (nid == kDiagNid) {
            ++diag_calls;
            // Fast-path state must already be committed before ANY HLE runs.
            if (env->kernel.inline_mutex_table)
                REQUIRE(env->kernel.inline_mutex_table->dirty_head == 0);
            if (observed_mutex >= 0 && env->kernel.lwmutexes.contains(observed_mutex)) {
                const auto m = env->kernel.lwmutexes.at(observed_mutex);
                const auto *w = Ptr<SceKernelLwMutexWork>(work).get(env->mem);
                REQUIRE(!m->owner && m->lock_count == 0);
                REQUIRE(w->owner == uint32_t(-1) && w->lockCount == 0);
            }
            write_reg(cpu, 0, 0);
            return;
        }
        if (nid == kLockNid) ++hle_locks;
        if (nid == kUnlockNid || nid == kUnlockAltNid) ++hle_unlocks;
        call_import(*env, cpu, nid, tid);
        REQUIRE(env->missing_nids.empty());
    };
    REQUIRE(env->kernel.init(env->mem, import, false));
    vita3k::web::GuestThreadRuntime runtime(128);
    REQUIRE(runtime.attach(*env));
    const bool has_table = static_cast<bool>(env->kernel.inline_mutex_table);
    REQUIRE(has_table); // This suite proves acceleration, not just HLE correctness.

    // --- Case 1: tight lock/unlock pair loop over a real Light mutex ---
    {
        SceUID id = -1;
        REQUIRE(mutex_create(&id, env->kernel, env->mem, "fixture", "inline lwmutex",
            0, 0, 0, Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
        Ptr<SceKernelLwMutexWork>(work).get(env->mem)->uid = id;
        observed_mutex = id;
        hle_locks = 0; hle_unlocks = 0; diag_calls = 0;
        build_tight_loop(env->mem, code, data, work, lock_stub, unlock_stub, diag_stub, 256);
        auto t = env->kernel.create_thread(env->mem, "inline looper", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(t && t->start(0, Ptr<void>{}, false) == 0);
        const auto pr = runtime.resume(512);
        REQUIRE(pr.failed == 0 && pr.idle);
        REQUIRE(t->status == ThreadStatus::dormant);
        REQUIRE(t->returned_value == 42);
        REQUIRE(*Ptr<uint32_t>(data + 0x00).get(env->mem) == 0u);
        REQUIRE(*Ptr<uint32_t>(data + 0x04).get(env->mem) == 0u);
        REQUIRE(*Ptr<uint32_t>(data + 0x08).get(env->mem) == 0u);
        REQUIRE(*Ptr<uint32_t>(data + 0x0c).get(env->mem) == 0u);
        REQUIRE(diag_calls == 1);
        if (has_table) {
            REQUIRE(env->kernel.inline_mutex_table->dirty_head == 0);
            // Successful uncontended pairs must take the fast path: zero HLE lock/unlock.
            REQUIRE(hle_locks == 0 && hle_unlocks == 0);
        }
        const auto m = env->kernel.lwmutexes.at(id);
        REQUIRE(!m->owner && m->lock_count == 0 && m->waiting_threads->empty());
        REQUIRE(mutex_close(env->kernel, env->mem, "fixture", t->id, id, SyncWeight::Light, HandleClose::Delete) == 0);
        if (has_table) {
            // Delete clears the lifetime tag so a cached stub cannot own it stale.
            bool any_uid = false;
            for (const auto &e : env->kernel.inline_mutex_table->entries)
                if (e.workarea == work && e.uid != 0) any_uid = true;
            REQUIRE(!any_uid);
        }
        REQUIRE(runtime.shutdown());
        REQUIRE(env->kernel.threads.empty());
        observed_mutex = -1;
    }

    // Two locks changed in one no-HLE slice: both strong owners must be
    // reconciled, including the tail of the intrusive dirty list, on return.
    {
        REQUIRE(runtime.attach(*env));
        const Address other_work = work + 64;
        SceUID a = -1, b = -1;
        REQUIRE(mutex_create(&a, env->kernel, env->mem, "fixture", "dirty head", 0, 0, 0,
            Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
        REQUIRE(mutex_create(&b, env->kernel, env->mem, "fixture", "dirty tail", 0, 0, 0,
            Ptr<SceKernelLwMutexWork>(other_work), SyncWeight::Light) == 0);
        guest_thread_fixture::Arm p(code);
        p.emit(0xe92d4010); // push {r4,lr}
        for (const auto at : {work, other_work}) {
            p.constant(0, at); p.constant(1, 1); p.constant(2, 0); p.call(lock_stub);
        }
        p.constant(0, 42); p.emit(0xe8bd8010); p.finish(env->mem);
        auto t = env->kernel.create_thread(env->mem, "dirty list", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        hle_locks = 0;
        REQUIRE(t && t->start(0, Ptr<void>{}, false) == 0);
        const auto done = runtime.resume(64);
        REQUIRE(done.failed == 0 && done.idle && t->returned_value == 42);
        REQUIRE(hle_locks == 0 && env->kernel.inline_mutex_table->dirty_head == 0);
        for (const auto id : {a, b}) {
            const auto m = env->kernel.lwmutexes.at(id);
            REQUIRE(m->owner == t && m->lock_count == 1);
            REQUIRE(env->kernel.inline_mutex_table->entries[inline_mutex_index(m->workarea.address())].dirty == 0);
            REQUIRE(mutex_unlock(env->kernel, "fixture", t->id, id, 1, SyncWeight::Light) == 0);
            REQUIRE(mutex_close(env->kernel, env->mem, "fixture", t->id, id, SyncWeight::Light, HandleClose::Delete) == 0);
        }
        REQUIRE(runtime.shutdown());
    }

    // --- Case 2: slice ends while HELD; second guest parks (slow path), unlock wakes ---
    {
        REQUIRE(runtime.attach(*env));
        SceUID id = -1;
        REQUIRE(mutex_create(&id, env->kernel, env->mem, "fixture", "held lwmutex",
            0, 0, 0, Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
        Ptr<SceKernelLwMutexWork>(work).get(env->mem)->uid = id;
        observed_mutex = -1; // diag not used here
        hle_locks = 0; hle_unlocks = 0;
        *Ptr<uint32_t>(data + 0x44).get(env->mem) = 0;
        *Ptr<uint32_t>(data + 0x48).get(env->mem) = 0;
        *Ptr<uint32_t>(data + 0x54).get(env->mem) = 0;
        build_holder(env->mem, code, data, work, lock_stub, unlock_stub);
        build_contender(env->mem, code + 0x800, data, work, lock_stub + 0x100);
        auto holder = env->kernel.create_thread(env->mem, "inline holder", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        auto contender = env->kernel.create_thread(env->mem, "inline contender", Ptr<const void>(code + 0x800),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(holder && contender);
        REQUIRE(holder->start(0, Ptr<void>{}, false) == 0);
        REQUIRE(contender->start(0, Ptr<void>{}, false) == 0);
        auto parked = runtime.resume(64);
        REQUIRE(parked.failed == 0);
        // Contender must be parked on the real queue; owner committed.
        REQUIRE(contender->status == ThreadStatus::wait);
        const auto m = env->kernel.lwmutexes.at(id);
        REQUIRE(m->owner == holder && m->lock_count == 1);
        REQUIRE(m->waiting_threads->size() == 1);
        REQUIRE((*m->waiting_threads->begin()).thread == contender);
        if (has_table) {
            REQUIRE(env->kernel.inline_mutex_table->dirty_head == 0);
            REQUIRE(env->kernel.inline_mutex_table->entries[inline_mutex_index(work)].enabled == 0);
        }
        // The contender parked => slow path was used at least once.
        REQUIRE(hle_locks >= 1);
        *Ptr<uint32_t>(data + 0x54).get(env->mem) = 1; // open host gate
        const auto done = runtime.resume(512);
        REQUIRE(done.failed == 0 && done.idle && done.dormant == 2);
        REQUIRE(holder->returned_value == 42 && contender->returned_value == 43);
        REQUIRE(m->owner == contender && m->lock_count == 1);
        REQUIRE(m->waiting_threads->empty());
        REQUIRE(hle_unlocks >= 1); // owner unlock took slow path to wake directly
        REQUIRE(mutex_unlock(env->kernel, "fixture", contender->id, id, 1, SyncWeight::Light) == 0);
        REQUIRE(mutex_close(env->kernel, env->mem, "fixture", contender->id, id, SyncWeight::Light, HandleClose::Delete) == 0);
        REQUIRE(runtime.shutdown());
        REQUIRE(env->kernel.threads.empty());
    }

    // --- Case 3: delete/recreate + hash-collision registration (host-level) ---
    {
        REQUIRE(runtime.attach(*env));
        SceUID a = -1, b = -1;
        REQUIRE(mutex_create(&a, env->kernel, env->mem, "fixture", "collide a",
            0, 0, 0, Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
        Ptr<SceKernelLwMutexWork>(work).get(env->mem)->uid = a;
        // Find a second workarea aliasing the same table index.
        Address work2 = 0;
        for (uint32_t probe = work + 0x20; probe < data + data_size - 32; probe += 0x20) {
            if (probe != work && inline_mutex_index(probe) == inline_mutex_index(work)) { work2 = probe; break; }
        }
        REQUIRE(work2 != 0);
        REQUIRE(mutex_create(&b, env->kernel, env->mem, "fixture", "collide b",
            0, 0, 0, Ptr<SceKernelLwMutexWork>(work2), SyncWeight::Light) == 0);
        Ptr<SceKernelLwMutexWork>(work2).get(env->mem)->uid = b;
        if (has_table) {
            const auto idx = inline_mutex_index(work);
            REQUIRE(inline_mutex_index(work2) == idx);
            // First registration wins the slot; second must stay safe (HLE).
            const auto &slot = env->kernel.inline_mutex_table->entries[idx];
            REQUIRE(slot.workarea == work && slot.uid == uint32_t(a));
        }
        // Deleting the colliding second object must not clear the first slot.
        REQUIRE(mutex_close(env->kernel, env->mem, "fixture", 0, b, SyncWeight::Light, HandleClose::Delete) == 0);
        if (has_table) {
            const auto idx = inline_mutex_index(work);
            REQUIRE(env->kernel.inline_mutex_table->entries[idx].workarea == work);
            REQUIRE(env->kernel.inline_mutex_table->entries[idx].uid == uint32_t(a));
        }
        // Compile the caller/stubs, then delete/recreate without changing
        // guest code or retiring its CPU/cache. The next invocation must use
        // the new mutex lifetime, not the previous owner/tag.
        build_tight_loop(env->mem, code, data, work, lock_stub, unlock_stub, diag_stub, 8);
        auto looper = env->kernel.create_thread(env->mem, "reuse looper", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(looper);
        observed_mutex = a;
        hle_locks = hle_unlocks = diag_calls = 0;
        REQUIRE(looper->start(0, Ptr<void>{}, false) == 0);
        auto first = runtime.resume(64);
        REQUIRE(first.failed == 0 && first.idle && looper->returned_value == 42);
        REQUIRE(diag_calls == 1 && hle_locks == 0 && hle_unlocks == 0);
        REQUIRE(mutex_close(env->kernel, env->mem, "fixture", 0, a, SyncWeight::Light, HandleClose::Delete) == 0);
        if (has_table) {
            const auto idx = inline_mutex_index(work);
            REQUIRE(env->kernel.inline_mutex_table->entries[idx].uid == 0);
        }
        SceUID c = -1;
        REQUIRE(mutex_create(&c, env->kernel, env->mem, "fixture", "recreated",
            0, 0, 0, Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
        Ptr<SceKernelLwMutexWork>(work).get(env->mem)->uid = c;
        REQUIRE(c != a);
        const auto m = env->kernel.lwmutexes.at(c);
        REQUIRE(!m->owner && m->lock_count == 0);
        if (has_table) {
            const auto idx = inline_mutex_index(work);
            const auto &slot = env->kernel.inline_mutex_table->entries[idx];
            REQUIRE(slot.workarea == work && slot.uid == uint32_t(c));
            REQUIRE(slot.owner == vita3k::wasmjit::kInlineMutexNoOwner && slot.count == 0);
        }
        observed_mutex = c;
        REQUIRE(looper->start(0, Ptr<void>{}, false) == 0);
        const auto second = runtime.resume(64);
        REQUIRE(second.failed == 0 && second.idle && looper->returned_value == 42);
        REQUIRE(diag_calls == 2 && hle_locks == 0 && hle_unlocks == 0);
        REQUIRE(mutex_close(env->kernel, env->mem, "fixture", 0, c, SyncWeight::Light, HandleClose::Delete) == 0);
        observed_mutex = -1;
        REQUIRE(runtime.shutdown());
        REQUIRE(env->kernel.threads.empty());
    }

    // --- Case 4: recursive counts + non-recursive self-lock fallback (host-level) ---
    {
        REQUIRE(runtime.attach(*env));
        SceUID id = -1;
        REQUIRE(mutex_create(&id, env->kernel, env->mem, "fixture", "recursive",
            0, SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
        Ptr<SceKernelLwMutexWork>(work).get(env->mem)->uid = id;
        auto owner = env->kernel.create_thread(env->mem, "rec owner", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(owner);
        const auto m = env->kernel.lwmutexes.at(id);
        REQUIRE(mutex_lock(env->kernel, env->mem, "fixture", owner->id, id, 1, nullptr, SyncWeight::Light) == 0);
        REQUIRE(mutex_lock(env->kernel, env->mem, "fixture", owner->id, id, 2, nullptr, SyncWeight::Light) == 0);
        REQUIRE(m->lock_count == 3);
        REQUIRE(mutex_unlock(env->kernel, "fixture", owner->id, id, 2, SyncWeight::Light) == 0);
        REQUIRE(m->lock_count == 1);
        REQUIRE(mutex_unlock(env->kernel, "fixture", owner->id, id, 1, SyncWeight::Light) == 0);
        REQUIRE(!m->owner && m->lock_count == 0);
        REQUIRE(mutex_close(env->kernel, env->mem, "fixture", owner->id, id, SyncWeight::Light, HandleClose::Delete) == 0);
        // Non-recursive self-lock must return the existing recursion error.
        SceUID id2 = -1;
        REQUIRE(mutex_create(&id2, env->kernel, env->mem, "fixture", "nonrec",
            0, 0, 0, Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
        Ptr<SceKernelLwMutexWork>(work).get(env->mem)->uid = id2;
        const auto m2 = env->kernel.lwmutexes.at(id2);
        REQUIRE(mutex_lock(env->kernel, env->mem, "fixture", owner->id, id2, 1, nullptr, SyncWeight::Light) == 0);
        const int self_rc = mutex_lock(env->kernel, env->mem, "fixture", owner->id, id2, 1, nullptr, SyncWeight::Light);
        REQUIRE(self_rc == SCE_KERNEL_ERROR_LW_MUTEX_RECURSIVE);
        REQUIRE(m2->lock_count == 1 && m2->owner == owner);
        REQUIRE(mutex_unlock(env->kernel, "fixture", owner->id, id2, 1, SyncWeight::Light) == 0);
        REQUIRE(mutex_close(env->kernel, env->mem, "fixture", owner->id, id2, SyncWeight::Light, HandleClose::Delete) == 0);
        REQUIRE(runtime.shutdown());
        REQUIRE(env->kernel.threads.empty());
    }

    // --- Case 5: shutdown leaves no table residue; objects destroyed before mem deinit ---
    REQUIRE(!runtime.attached() || runtime.shutdown());
    REQUIRE(env->kernel.threads.empty());
    REQUIRE(env->kernel.lwmutexes.empty());
    REQUIRE(!env->kernel.inline_mutex_table);
    REQUIRE(get_current_cpu_state() == nullptr);
    // Release the fixed Memory64 window before the next suite constructs its
    // own MemState. All owning blocks must be freed before deinit_mem.
    env->kernel.call_import = {};
    env->kernel.deinit(env->mem);
    deinit_mem(env->mem);
    std::puts("Inline mutex runtime: fast loop, held-slice park/wake, recreate/collision, recursion passed");
}
} // namespace inline_mutex_fixture

inline void test_inline_mutex_runtime() { inline_mutex_fixture::test_inline_mutex_runtime(); }
