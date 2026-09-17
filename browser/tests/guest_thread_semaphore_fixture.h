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
} // namespace guest_thread_fixture
