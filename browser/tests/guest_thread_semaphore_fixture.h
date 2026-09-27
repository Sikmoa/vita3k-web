// Asset-free ARM guest program for the browser thread integration test.
// This builds real import stubs. The test dispatches them through call_import.
#pragma once
#include <mem/functions.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace guest_thread_fixture {
struct Shared {
    int32_t semaphore;
    int32_t child;
    uint32_t allow_signal; // Test sets this immediately before the blocking wait.
    int32_t first_wait;
    int32_t second_wait;
    int32_t start_result;
    int32_t signal_result;
    int32_t second_signal_result;
    uint32_t parent_done;
    uint32_t child_done;
};
struct Program { Address parent, child, shared; };

class Arm {
    Address base;
    std::vector<uint32_t> words;
public:
    explicit Arm(Address address) : base(address) {}
    Address pc() const { return base + static_cast<Address>(words.size() * 4); }
    void emit(uint32_t instruction) { words.push_back(instruction); }
    void constant(unsigned reg, uint32_t value) {
        // MOVW/MOVT avoid a literal pool inside executable code.
        emit(0xe3000000u | ((value & 0xf000u) << 4) | (reg << 12) | (value & 0xfffu));
        value >>= 16;
        emit(0xe3400000u | ((value & 0xf000u) << 4) | (reg << 12) | (value & 0xfffu));
    }
    void load(unsigned reg, unsigned offset) { emit(0xe5940000u | (reg << 12) | offset); }
    void store(unsigned reg, unsigned offset) { emit(0xe5840000u | (reg << 12) | offset); }
    void call(Address target) {
        const int32_t displacement = static_cast<int32_t>(target - (pc() + 8));
        emit(0xeb000000u | ((static_cast<uint32_t>(displacement) >> 2) & 0xffffffu));
    }
    void finish(MemState &mem) const {
        std::memcpy(Ptr<void>(base).get(mem), words.data(), words.size() * sizeof(uint32_t));
    }
};

inline Program build(MemState &mem, Address code, Address data) {
    const Address parent = code, child = code + 0x400, stubs = code + 0x800;
    const Address sema_name = data + 0x100, child_name = data + 0x140;
    std::memset(Ptr<void>(data).get(mem), 0xcc, sizeof(Shared));
    Ptr<Shared>(data).get(mem)->allow_signal = 0;
    Ptr<Shared>(data).get(mem)->parent_done = 0;
    Ptr<Shared>(data).get(mem)->child_done = 0;
    std::strcpy(Ptr<char>(sema_name).get(mem), "fiber semaphore");
    std::strcpy(Ptr<char>(child_name).get(mem), "fiber signaler");
    const uint32_t nids[] = {0x1bd67366, 0xc5c11ee7, 0xf08de149, 0x0c7b834b, 0xe6b761d1};
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t stub[] = {0xef000000, 0xe1a0f00e, nids[i]};
        std::memcpy(Ptr<void>(stubs + 16 * i).get(mem), stub, sizeof(stub));
    }
    Arm p(parent);
    p.emit(0xe92d4010); // push {r4,lr}
    p.constant(4, data);
    p.emit(0xe24dd010); // sub sp,sp,#16: arguments 5..7, preserve alignment
    p.constant(0, 0);
    p.emit(0xe58d0000); // options pointer for CreateSema
    p.constant(0, sema_name); p.constant(1, 0); p.constant(2, 1); p.constant(3, 1);
    p.call(stubs); // CreateSema(name, attr=0, initial=1, max=1, options=null)
    p.store(0, offsetof(Shared, semaphore));
    p.constant(1, 1); p.constant(2, 0);
    p.call(stubs + 48); // first wait consumes the initial count without blocking
    p.store(0, offsetof(Shared, first_wait));
    p.constant(0, 0); p.emit(0xe58d0000); p.emit(0xe58d0008); // attr/options
    p.constant(0, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT); p.emit(0xe58d0004);
    p.constant(0, child_name); p.constant(1, child);
    p.constant(2, SCE_KERNEL_DEFAULT_PRIORITY_USER); p.constant(3, SCE_KERNEL_STACK_SIZE_USER_MAIN);
    p.call(stubs + 16); // CreateThread: must remain dormant
    p.store(0, offsetof(Shared, child));
    p.constant(1, 0); p.constant(2, 0);
    p.call(stubs + 32); // StartThread(child,0,null)
    p.store(0, offsetof(Shared, start_result));
    p.load(0, offsetof(Shared, semaphore)); p.constant(1, 1); p.constant(2, 0);
    p.call(stubs + 48); // second wait: count=0, child must wake the parent
    p.store(0, offsetof(Shared, second_wait));
    p.constant(0, 1); p.store(0, offsetof(Shared, parent_done));
    p.constant(0, 42); p.emit(0xe28dd010); p.emit(0xe8bd8010); // return 42
    p.finish(mem);

    Arm c(child);
    c.emit(0xe92d4010); c.constant(4, data);
    // Parent-side HLE observer opens this gate before the second WaitSema.
    // This also exercises bounded JIT scheduling if the child runs first.
    c.load(0, offsetof(Shared, allow_signal));
    c.emit(0xe3500000); c.emit(0x0afffffc); // cmp r0,#0; beq load
    c.load(0, offsetof(Shared, semaphore)); c.constant(1, 1);
    c.call(stubs + 64); // consumes the parent's queued request: count stays 0
    c.store(0, offsetof(Shared, signal_result));
    c.load(0, offsetof(Shared, semaphore)); c.constant(1, 1);
    c.call(stubs + 64); // no queued request remains: count becomes 1
    c.store(0, offsetof(Shared, second_signal_result));
    c.constant(0, 1); c.store(0, offsetof(Shared, child_done));
    c.constant(0, 43); c.emit(0xe8bd8010); // return 43
    c.finish(mem);
    return {parent, child, data};
}
// r4 preserves the result pointer across the production HLE bridge.
inline void build_waiter(MemState &mem, Address code, SceUID sema, Address timeout, Address result) {
    const Address stub_address = code + 0x100;
    const uint32_t stub[] = {0xef000000, 0xe1a0f00e, 0x0c7b834b};
    std::memcpy(Ptr<void>(stub_address).get(mem), stub, sizeof(stub));
    Arm p(code);
    p.emit(0xe92d4010);
    p.constant(4, result); p.constant(0, static_cast<uint32_t>(sema));
    p.constant(1, 1); p.constant(2, timeout);
    p.call(stub_address);
    p.store(0, 0);
    p.emit(0xe8bd8010);
    p.finish(mem);
}

// Same ARM call shape for a heavy UID or a lightweight workarea address.
inline void build_mutex_waiter(MemState &mem, Address code, uint32_t lock_argument,
    bool light, int count, Address timeout, Address result) {
    const Address stub_address = code + 0x100;
    const uint32_t stub[] = {0xef000000, 0xe1a0f00e, light ? 0x46e7be7bu : 0x1d8d7945u};
    std::memcpy(Ptr<void>(stub_address).get(mem), stub, sizeof(stub));
    Arm p(code);
    p.emit(0xe92d4010);
    p.constant(4, result); p.constant(0, lock_argument);
    p.constant(1, count); p.constant(2, timeout);
    p.call(stub_address);
    p.store(0, 0);
    p.emit(0xe8bd8010);
    p.finish(mem);
}

// Contended lightweight-mutex pair. The host creates the mutex (init 0) at
// data+0x300. Parent: lock (uncontended), publish marker, poll a host gate,
// unlock, poll child completion. Child: poll marker, lock (contended -> must
// PARK on the runtime), publish result, signal state 2. Offsets into data:
// 0x40 parent lock result, 0x44 marker, 0x48 child state, 0x4c unlock result,
// 0x50 child lock result, 0x54 host gate.
inline void build_lwmutex_pair(MemState &mem, Address code, Address data) {
    const Address lock_stub = code + 0x300, unlock_stub = code + 0x320;
    const uint32_t lock_words[] = {0xef000000, 0xe1a0f00e, 0x46e7be7b};   // sceKernelLockLwMutex
    const uint32_t unlock_words[] = {0xef000000, 0xe1a0f00e, 0x120afc8c}; // sceKernelUnlockLwMutex2
    std::memcpy(Ptr<void>(lock_stub).get(mem), lock_words, sizeof(lock_words));
    std::memcpy(Ptr<void>(unlock_stub).get(mem), unlock_words, sizeof(unlock_words));
    const Address work = data + 0x300;
    Arm p(code);
    p.emit(0xe92d4010); // push {r4,lr}
    p.constant(4, data);
    p.constant(0, work); p.constant(1, 1); p.constant(2, 0);
    p.call(lock_stub);
    p.store(0, 0x40);
    p.constant(0, 1); p.store(0, 0x44); // marker: child may contend now
    const Address gate_loop = p.pc();
    p.load(0, 0x54); p.emit(0xe3500000); p.emit(0x0a000000u | ((static_cast<uint32_t>(static_cast<int32_t>(gate_loop - (p.pc() + 8))) >> 2) & 0xffffffu)); // beq gate_loop
    p.constant(0, work); p.constant(1, 1);
    p.call(unlock_stub);
    p.store(0, 0x4c);
    const Address done_loop = p.pc();
    p.load(0, 0x48); p.emit(0xe3500002); p.emit(0x1a000000u | ((static_cast<uint32_t>(static_cast<int32_t>(done_loop - (p.pc() + 8))) >> 2) & 0xffffffu));
    p.constant(0, 42);
    p.emit(0xe8bd8010); // pop {r4,pc}
    p.finish(mem);

    Arm c(code + 0x400);
    c.emit(0xe92d4010);
    c.constant(4, data);
    const Address marker_loop = c.pc();
    c.load(0, 0x44); c.emit(0xe3500001); c.emit(0x1a000000u | ((static_cast<uint32_t>(static_cast<int32_t>(marker_loop - (c.pc() + 8))) >> 2) & 0xffffffu));
    c.constant(0, work); c.constant(1, 1); c.constant(2, 0);
    c.call(lock_stub);
    c.store(0, 0x50);
    c.constant(0, 2); c.store(0, 0x48);
    c.constant(0, 43);
    c.emit(0xe8bd8010);
    c.finish(mem);
}
// Thread-end join pair. Target (code+0x400): poll the host gate at data+0x60,
// sleeping in sceKernelDelayThread(1000) between polls (so it parks rather
// than spins), then return 43 or, with exit_delete, call
// sceKernelExitDeleteThread(43).
// Waiter (code): sceKernelWaitThreadEnd(id at data+0x6c, stat=data+0x64,
// timeout pointer at data+0x70), result to data+0x68, return 42.
inline void build_thread_end_pair(MemState &mem, Address code, Address data, bool exit_delete) {
    const Address stub = code + 0x300, exit_stub = code + 0x320, delay_stub = code + 0x340;
    const uint32_t delay_words[] = {0xef000000, 0xe1a0f00e, 0x4b675d05}; // sceKernelDelayThread
    std::memcpy(Ptr<void>(delay_stub).get(mem), delay_words, sizeof(delay_words));
    const uint32_t words[] = {0xef000000, 0xe1a0f00e, 0xddb395a9}; // sceKernelWaitThreadEnd
    const uint32_t exit_words[] = {0xef000000, 0xe1a0f00e, 0x1d17decf}; // sceKernelExitDeleteThread
    std::memcpy(Ptr<void>(stub).get(mem), words, sizeof(words));
    std::memcpy(Ptr<void>(exit_stub).get(mem), exit_words, sizeof(exit_words));
    Arm w(code);
    w.emit(0xe92d4010); // push {r4,lr}
    w.constant(4, data);
    w.load(0, 0x6c); w.constant(1, data + 0x64); w.load(2, 0x70);
    w.call(stub);
    w.store(0, 0x68);
    w.constant(0, 42);
    w.emit(0xe8bd8010); // pop {r4,pc}
    w.finish(mem);

    Arm t(code + 0x400);
    t.emit(0xe92d4010);
    t.constant(4, data);
    const Address gate_loop = t.pc();
    t.load(0, 0x60); t.emit(0xe3500000);
    t.emit(0x1a000003); // bne past the delay (+3 instructions)
    t.constant(0, 1000);
    t.call(delay_stub);
    t.emit(0xea000000u | ((static_cast<uint32_t>(static_cast<int32_t>(gate_loop - (t.pc() + 8))) >> 2) & 0xffffffu)); // b gate_loop
    t.constant(0, 43);
    if (exit_delete)
        t.call(exit_stub);
    t.emit(0xe8bd8010);
    t.finish(mem);
}
// Lightweight condition variable fixture. Host creates the LwMutex at
// data+0x300 and the LwCond at data+0x340 (associated with that mutex).
// Stubs live at code+0xf00. Offsets are into data.
namespace lwcond {
constexpr Address kMutex = 0x300, kCond = 0x340, kTimeout = 0x60, kSignaler = 0x80;
constexpr Address waiter_area(unsigned slot) { return 0x100 + 0x20 * slot; }
// Waiter area: +0 lock result, +4 wait result, +8 workarea owner after the
// wait, +0xc unlock result, +0x10 done. Signaler area: +0 gate, +4 lock
// result, +8 signal result, +0xc phase (1 = signalled, mutex held), +0x10
// unlock gate, +0x14 unlock result.
constexpr uint32_t kLock = 0x46e7be7b, kUnlock = 0x91fa6614, kWait = 0xe1878282,
    kSignal = 0x3ac63b9a, kSignalAll = 0xe5241a0c, kDelay = 0x4b675d05;

inline Address stub(Address code, unsigned index) { return code + 0xf00 + 16 * index; }

inline void branch(Arm &a, uint32_t cond, Address target) {
    a.emit((cond << 28) | 0x0a000000u | ((static_cast<uint32_t>(static_cast<int32_t>(target - (a.pc() + 8))) >> 2) & 0xffffffu));
}

// Poll a word at [r4+offset] until nonzero, parking in DelayThread(1000).
inline void gate(Arm &a, Address code, unsigned offset) {
    const Address loop = a.pc();
    a.load(0, offset); a.emit(0xe3500000);
    a.emit(0x1a000003); // bne past the delay (+3 instructions)
    a.constant(0, 1000);
    a.call(stub(code, 5));
    branch(a, 0xe, loop);
}

inline void build(MemState &mem, Address code, Address data) {
    const uint32_t nids[] = {kLock, kUnlock, kWait, kSignal, kSignalAll, kDelay};
    for (unsigned i = 0; i < 6; ++i) {
        const uint32_t words[] = {0xef000000, 0xe1a0f00e, nids[i]};
        std::memcpy(Ptr<void>(stub(code, i)).get(mem), words, sizeof(words));
    }
    // Waiters 0..3 at code + 0x100 * slot: lock, wait(timeout pointer at
    // data+kTimeout+4*slot, zero = none), read the workarea owner, unlock.
    for (unsigned slot = 0; slot < 4; ++slot) {
        Arm w(code + 0x100 * slot);
        w.emit(0xe92d4010); // push {r4,lr}
        w.constant(4, data + waiter_area(slot));
        w.constant(0, data + kMutex); w.constant(1, 1); w.constant(2, 0);
        w.call(stub(code, 0));
        w.store(0, 0);
        w.constant(0, data + kCond); w.constant(1, data + kTimeout + 4 * slot);
        w.emit(0xe5911000); // ldr r1,[r1]: the timeout pointer itself
        w.call(stub(code, 2));
        w.store(0, 4);
        w.constant(0, data + kMutex); w.emit(0xe5900000); // ldr r0,[r0]
        w.store(0, 8);
        w.constant(0, data + kMutex); w.constant(1, 1);
        w.call(stub(code, 1));
        w.store(0, 0xc);
        w.constant(0, 1); w.store(0, 0x10);
        w.constant(0, 42);
        w.emit(0xe8bd8010); // pop {r4,pc}
        w.finish(mem);
    }
    // Signalers: 0x400 = SignalLwCond, 0x600 = SignalLwCondAll. Take the
    // mutex, signal, publish the phase, hold the mutex until the unlock gate.
    for (const bool all : {false, true}) {
        Arm s(code + (all ? 0x600 : 0x400));
        s.emit(0xe92d4010);
        s.constant(4, data + kSignaler);
        gate(s, code, 0);
        s.constant(0, data + kMutex); s.constant(1, 1); s.constant(2, 0);
        s.call(stub(code, 0));
        s.store(0, 4);
        s.constant(0, data + kCond);
        s.call(stub(code, all ? 4 : 3));
        s.store(0, 8);
        s.constant(0, 1); s.store(0, 0xc);
        gate(s, code, 0x10);
        s.constant(0, data + kMutex); s.constant(1, 1);
        s.call(stub(code, 1));
        s.store(0, 0x14);
        s.constant(0, 43);
        s.emit(0xe8bd8010);
        s.finish(mem);
    }
}
} // namespace lwcond
} // namespace guest_thread_fixture
