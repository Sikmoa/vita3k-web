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
// return 43. Waiter (code): sceKernelWaitThreadEnd(id at data+0x6c,
// stat=data+0x64, timeout pointer at data+0x70), result to data+0x68, return 42.
inline void build_thread_end_pair(MemState &mem, Address code, Address data) {
    const Address stub = code + 0x300;
    const uint32_t words[] = {0xef000000, 0xe1a0f00e, 0xddb395a9}; // sceKernelWaitThreadEnd
    std::memcpy(Ptr<void>(stub).get(mem), words, sizeof(words));
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
    t.load(0, 0x60); t.emit(0xe3500000); t.emit(0x0a000000u | ((static_cast<uint32_t>(static_cast<int32_t>(gate_loop - (t.pc() + 8))) >> 2) & 0xffffffu)); // beq gate_loop
    t.constant(0, 43);
    t.emit(0xe8bd8010);
    t.finish(mem);
}
} // namespace guest_thread_fixture
