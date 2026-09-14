// Focused wasm32 backend integration tests. Compile this TU INSTEAD OF
// wasm_jit_cpu.cpp so the anonymous checked helpers can also be tested directly.
// Uses production MemState and Dynarmic translation; no interpreter or mocks.
#include "../src/wasm_jit_cpu.cpp"
#include <mem/ptr.h>
#include <cstdlib>

namespace {
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)
constexpr Address code = 0x81000000, data = 0x82000000;
constexpr uint32_t page = 4096;

void helpers(MemState &mem) {
    JitState state{};
    state.memory_cookie = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&mem));
    const std::array<uint32_t, 4> lanes{0x76543210, 0xfedcba98, 0x89abcdef, 0x01234567};
    // Each valid size is deliberately unaligned; wider accesses cross pages.
    for (uint32_t bytes : {1, 2, 4, 8, 16}) {
        const Address address = data + page - 1;
        std::copy(lanes.begin(), lanes.end(), state.memory_value);
        CHECK(checked_memory_write(&state, address, bytes) == 0);
        std::fill_n(state.memory_value, 4, UINT32_MAX);
        CHECK(checked_memory_read(&state, address, bytes) == 0);
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t actual = state.memory_value[i / 4] >> ((i % 4) * 8);
            const uint8_t expected = i < bytes ? lanes[i / 4] >> ((i % 4) * 8) : 0;
            CHECK(actual == expected);
        }
    }
    for (uint32_t bytes : {0u, 3u, 5u, 15u, 17u, UINT32_MAX}) {
        CHECK(checked_memory_read(&state, data, bytes) == 2);
        CHECK(state.fault_address == data && state.fault_write == 0);
        CHECK(checked_memory_write(&state, data, bytes) == 2);
        CHECK(state.fault_address == data && state.fault_write == 1);
    }
    for (Address address : {0u, 0x83000000u, UINT32_MAX}) {
        CHECK(checked_memory_read(&state, address, 4) == 2);
        CHECK(state.fault_address == address && state.fault_write == 0);
        CHECK(checked_memory_write(&state, address, 4) == 2);
        CHECK(state.fault_address == address && state.fault_write == 1);
    }
    CHECK(mem_set_permissions(mem, data + page, page, MemPerm::ReadOnly));
    std::array<uint8_t, 16> before{}, after{};
    CHECK(mem_read(mem, data + page - 3, before.data(), before.size()));
    std::fill_n(state.memory_value, 4, 0x55555555);
    CHECK(checked_memory_write(&state, data + page - 3, 16) == 2);
    CHECK(mem_read(mem, data + page - 3, after.data(), after.size()));
    CHECK(before == after); // WHOLE access checked before even the first write.
    CHECK(mem_set_permissions(mem, data + page, page, MemPerm::WriteOnly));
    const auto saved = state;
    CHECK(checked_memory_read(&state, data + page - 3, 16) == 2);
    CHECK(std::memcmp(saved.memory_value, state.memory_value, sizeof(state.memory_value)) == 0);
    CHECK(mem_set_permissions(mem, data + page, page, MemPerm::ReadWrite));
    state.memory_cookie = 0;
    CHECK(checked_memory_read(&state, data, 4) == 2);
    CHECK(checked_memory_write(&state, data, 4) == 2);
}

void put(MemState &mem, WasmJitCPU &jit, std::initializer_list<uint32_t> words) {
    CHECK(mem_write(mem, code, words.begin(), words.size() * sizeof(uint32_t)));
    jit.invalidate_jit_cache(code, page);
    jit.set_cpsr(0x10);
    jit.set_pc(code);
}
void put_thumb(MemState &mem, WasmJitCPU &jit, std::initializer_list<uint32_t> words) {
    CHECK(mem_write(mem, code, words.begin(), words.size() * sizeof(uint32_t)));
    jit.invalidate_jit_cache(code, page);
    jit.set_cpsr(0x10);
    jit.set_pc(code | 1); // T bit from the low PC bit, descriptor PC aligned
}
void equal_context(const CPUContext &a, const CPUContext &b) {
    CHECK(a.cpu_registers == b.cpu_registers);
    CHECK(a.cpsr == b.cpsr && a.fpscr == b.fpscr);
    CHECK(std::memcmp(a.fpu_registers.data(), b.fpu_registers.data(), sizeof(a.fpu_registers)) == 0);
}

void backend(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 3);
    jit.set_region_mode(false); // this section verifies single-block semantics
    jit.set_instruction_budget(128);
    CHECK(jit.processor_id() == 3);
    // Full context, including payload NaNs and both halves of every D register.
    CPUContext initial{};
    for (unsigned i = 0; i < 16; ++i) initial.cpu_registers[i] = 0x11100000 + i;
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t bits = 0x7f800001 + i;
        std::memcpy(&initial.fpu_registers[i], &bits, sizeof(bits));
    }
    initial.cpsr = 0x800f0010;
    initial.fpscr = 0x01400010;
    jit.set_tpidruro(0x87654321);
    jit.load_context(initial);
    equal_context(jit.save_context(), initial);
    CHECK(jit.get_tpidruro() == 0x87654321);
    jit.set_fpscr(0);

    // Multi-instruction translation is retried at limit=1 for memory IR; the
    // requested limit remains the cache key. Repeated run must NOT recompile.
    put(mem, jit, {0xe5910000, 0xe2800001, 0xe5810000, 0xef000042});
    uint32_t value = 40;
    CHECK(mem_write(mem, data, &value, sizeof(value)));
    jit.set_reg(1, data);
    auto executed = jit.instructions_executed();
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);
    CHECK(jit.get_reg(0) == 41 && jit.get_pc() == code + 16);
    CHECK(jit.instructions_executed() == executed + 4);
    const auto compiled = jit.compiled_blocks(), hits = jit.cache_hits();
    jit.set_pc(code);
    CHECK(jit.run() == 0 && parent.svc_called);
    CHECK(jit.get_reg(0) == 42);
    CHECK(jit.compiled_blocks() == compiled && jit.cache_hits() > hits);
    CHECK(mem_read(mem, data, &value, sizeof(value)) && value == 42);

    // Host Ptr modifications must invalidate a hot code block even without an
    // explicit invalidate_jit_cache call.
    auto invalidated = jit.invalidated_blocks();
    *Ptr<uint32_t>(code + 4).get(mem) = 0xe2800002; // ADD R0, R0, #2
    jit.set_pc(code);
    CHECK(jit.run() == 0 && jit.get_reg(0) == 44);
    CHECK(jit.invalidated_blocks() > invalidated);

    // A generated store replaces the next instruction before its cached block
    // can execute. Every entry's byte snapshot must observe the changed opcode.
    put(mem, jit, {0xe5810000, 0xe3a02001, 0xef000042});
    jit.set_reg(0, 0xe3a0202a); // MOV R2, #42
    jit.set_reg(1, code + 4);
    CHECK(jit.run() == 0 && jit.get_reg(2) == 42);
    invalidated = jit.invalidated_blocks();
    jit.set_pc(code);
    jit.set_reg(0, 0xe3a0202b); // MOV R2, #43
    CHECK(jit.run() == 0 && jit.get_reg(2) == 43);
    CHECK(jit.invalidated_blocks() > invalidated);

    // Faulting LDM changes R0 before its second load fails. The backend must
    // restore ALL CPU state, leave PC at the instruction, and retain metadata.
    put(mem, jit, {0xe8b10005, 0xef000042}); // LDMIA R1!, {R0,R2}
    jit.set_reg(1, data + 2 * page - 4);
    auto before = jit.save_context();
    executed = jit.instructions_executed();
    CHECK(jit.step() == -1);
    equal_context(jit.save_context(), before);
    CHECK(jit.get_tpidruro() == 0x87654321);
    CHECK(jit.instructions_executed() == executed && !parent.svc_called);
    CHECK(jit.get_fault_address() == data + 2 * page && !jit.get_fault_write());
    CHECK(jit.get_last_error().find("guest memory read fault") != std::string::npos);

    // Earlier stores in one guest STM instruction commit before a later fault;
    // CPU state rolls back, but memory intentionally is not transactional.
    put(mem, jit, {0xe8a10005, 0xef000042}); // STMIA R1!, {R0,R2}
    jit.set_reg(0, 0x12345678);
    jit.set_reg(1, data + 2 * page - 4);
    before = jit.save_context();
    CHECK(jit.step() == -1);
    equal_context(jit.save_context(), before);
    CHECK(jit.get_fault_address() == data + 2 * page && jit.get_fault_write());
    CHECK(mem_read(mem, data + 2 * page - 4, &value, sizeof(value)) && value == 0x12345678);

    // Permission denial also faults through the imported checked helper.
    put(mem, jit, {0xe5810000, 0xef000042});
    jit.set_reg(1, data);
    CHECK(mem_set_permissions(mem, data, page, MemPerm::ReadOnly));
    before = jit.save_context();
    CHECK(jit.step() == -1);
    equal_context(jit.save_context(), before);
    CHECK(jit.get_fault_address() == data && jit.get_fault_write());
    CHECK(mem_set_permissions(mem, data, page, MemPerm::ReadWrite));

    // The fixture's NEON memset loop (0x81000e24..0x81000e36) with its own
    // encodings, in Thumb: vdup.32 q8,lr; then vst1.32 {d16-d17},[ip]! /
    // cmp r3,ip / bne back to the vst1, run twice; svc terminates. Proves
    // the Q8/D16/D17 word mapping, guest stores and ip writeback through
    // the real backend and a taken branch into a 32-bit instruction.
    put_thumb(mem, jit, {0xeb90eea0, // vdup.32 q8, lr
        0x0a8df94c,                 // vst1.32 {d16-d17}, [ip]!
        0xd1fb4563,                 // cmp r3, ip; bne -10 -> vst1
        0xbf00df33});               // svc 0x33; nop
    jit.set_reg(14, 0x5a5a5a5a);
    jit.set_reg(3, data + 32);
    jit.set_reg(12, data);
    executed = jit.instructions_executed();
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x33);
    CHECK(jit.get_pc() == code + 14);
    CHECK(jit.get_reg(12) == data + 32 && jit.get_reg(3) == data + 32);
    CHECK(jit.get_cpsr() == 0x60000030); // final cmp equal: Z+C set
    CHECK(jit.instructions_executed() == executed + 8);
    const auto neon = jit.save_context();
    for (unsigned i = 32; i < 36; ++i) { // every q8 lane, through the context
        uint32_t lane = 0;
        std::memcpy(&lane, &neon.fpu_registers[i], sizeof(lane));
        CHECK(lane == 0x5a5a5a5a);
    }
    for (unsigned i = 0; i < 32 / 4; ++i) {
        uint32_t word = 0;
        CHECK(mem_read(mem, data + i * 4, &word, sizeof(word)));
        CHECK(word == 0x5a5a5a5a);
    }
}
// M14c region formation: DFS membership, one-block-per-PC, PSR metadata,
// tick accounting and sorted output on a real Thumb CFG with a loop, an
// unconditional branch over dead code, and a SVC-terminated block.
void formation(MemState &mem) {
    // 0x00 movs r0,#0; 0x02 adds r0,#1; 0x04 cmp r0,#3; 0x06 bne 0x02;
    // 0x08 b 0x0c; 0x0a nop (unreachable); 0x0c svc 0x42
    const std::array<uint32_t, 4> words{0x30002000, 0xd1fc2803, 0xbf00e000, 0xbf00df42};
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    Region region;
    std::vector<Dynarmic::IR::Block> ir;
    CHECK(form_region(mem, code, 0x30, 0, region, ir)); // Thumb, user mode
    CHECK(region.blocks.size() == 4); // entry, loop body, b, svc
    CHECK(ir.size() == region.blocks.size());
    std::vector<uint32_t> pcs;
    for (const auto &block : region.blocks) {
        pcs.push_back(block.pc);
        CHECK(block.psr_mask == PSR_DISPATCH_MASK);
        CHECK(block.psr_value == 0x20); // T bit, no IT/E
        CHECK(block.ticks >= 1);
        CHECK(!block.original.empty());
    }
    CHECK(std::is_sorted(pcs.begin(), pcs.end()));
    CHECK(std::adjacent_find(pcs.begin(), pcs.end()) == pcs.end()); // one per PC
    CHECK(std::find(pcs.begin(), pcs.end(), code) != pcs.end());
    CHECK(std::find(pcs.begin(), pcs.end(), code + 2) != pcs.end()); // loop body
    CHECK(std::find(pcs.begin(), pcs.end(), code + 8) != pcs.end()); // b
    CHECK(std::find(pcs.begin(), pcs.end(), code + 0xc) != pcs.end()); // svc
    CHECK(std::find(pcs.begin(), pcs.end(), code + 0xa) == pcs.end()); // dead nop
    uint64_t sum = 0;
    for (const auto &block : region.blocks) sum += block.ticks;
    CHECK(region.total_ticks == sum);
    CHECK(region.total_ticks < REGION_MAX_TICKS && region.blocks.size() < REGION_MAX_BLOCKS);
    CHECK(region.page_begin == code / 4096 && region.page_end == (code + 0x10 + 4095) / 4096);
    // An unmapped entry cannot form a region.
    Region bad;
    std::vector<Dynarmic::IR::Block> bad_ir;
    CHECK(!form_region(mem, 0x83000000, 0x30, 0, bad, bad_ir) && bad.blocks.empty());
    // Formation is deterministic.
    Region again;
    std::vector<Dynarmic::IR::Block> again_ir;
    CHECK(form_region(mem, code, 0x30, 0, again, again_ir));
    CHECK(again.blocks.size() == region.blocks.size());
    for (size_t i = 0; i < again.blocks.size(); ++i) {
        CHECK(again.blocks[i].pc == region.blocks[i].pc);
        CHECK(again.blocks[i].ticks == region.blocks[i].ticks);
        CHECK(again.blocks[i].original == region.blocks[i].original);
        CHECK(Dynarmic::A32::LocationDescriptor(again_ir[i].Location()).PC() == again.blocks[i].pc);
    }
    // The emitted region module is valid Wasm with the run(state,budget) export.
    std::vector<const Dynarmic::IR::Block *> ptrs;
    std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
    for (size_t i = 0; i < region.blocks.size(); ++i) {
        ptrs.push_back(&ir[i]);
        meta.push_back({region.blocks[i].pc, region.blocks[i].psr_mask,
            region.blocks[i].psr_value, region.blocks[i].ticks});
    }
    const auto module = vita3k::wasmjit::emit_region(ptrs, meta);
    CHECK(!module.empty());
    // Mismatched meta is rejected: wrong ticks, wrong PSR, unsorted, dup PCs.
    auto bad_meta = meta;
    bad_meta[1].ticks += 1;
    CHECK(vita3k::wasmjit::emit_region(ptrs, bad_meta).empty());
    bad_meta = meta;
    bad_meta[2].entry_pc = bad_meta[1].entry_pc; // duplicate PC
    CHECK(vita3k::wasmjit::emit_region(ptrs, bad_meta).empty());
    CHECK(vita3k::wasmjit::emit_region({}, {}).empty());
}

// Region-mode execution: the same guest programs run through REGION modules
// with in-Wasm dispatch, chaining, budget and fault semantics.
void region_exec(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0); // region mode is the default
    jit.set_instruction_budget(4096);

    // The loop CFG: blocks chain inside ONE region; only the SVC exits.
    // 0x00 movs r0,#0; 0x02 adds r0,#1; 0x04 cmp r0,#3; 0x06 bne 0x02;
    // 0x08 b 0x0c; 0x0a nop (unreachable); 0x0c svc 0x42
    // 0x3001 = adds r0,#1 (GAS-verified; 0x3008 would decode as adds r0,#8).
    const std::array<uint32_t, 4> words{0x30012000, 0xd1fc2803, 0xbf00e000, 0xbf00df42};;
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);
    CHECK(jit.get_reg(0) == 3); // loop executed adds three times
    CHECK(jit.get_pc() == code + 0xe); // PC past the 2-byte svc (Thumb)
    CHECK(jit.compiled_blocks() == 0); // no single-block modules were built
    CHECK(jit.get_last_error().empty());

    // Host-side code patch: the cached region must be dropped and rebuilt.
    const std::array<uint32_t, 2> patch{0xdf432000, 0}; // movs r0,#0; svc 0x43
    CHECK(mem_write(mem, code, patch.data(), patch.size() * sizeof(uint32_t)));
    jit.set_pc(code | 1);
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x43);
    CHECK(jit.get_reg(0) == 0);
    CHECK(jit.invalidated_blocks() > 0);

    // Budget exhaustion: block 0x00 costs 4 ticks (movs+adds+cmp+bne); with a
    // 5-tick budget it completes, then dispatch refuses the 3-tick successor.
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    jit.set_instruction_budget(5);
    const uint64_t executed_before = jit.instructions_executed();
    CHECK(jit.run() == 0); // budget consumed cleanly, no error
    CHECK(jit.instructions_executed() - executed_before == 4);
    CHECK(jit.get_pc() == code + 2); // stopped at the successor entry

    // Memory fault mid-region: fault_address is the guest address; executed
    // counts only blocks completed before the faulting instruction.
    const std::array<uint32_t, 2> faultprog{0x68012007, 0xbf00df42}; // movs r0,#7; ldr r1,[r0]; svc
    CHECK(mem_write(mem, code, faultprog.data(), faultprog.size() * sizeof(uint32_t)));
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    jit.set_instruction_budget(4096);
    const uint64_t fault_executed_before = jit.instructions_executed();
    CHECK(jit.run() == -1);
    CHECK(jit.get_fault_address() == 7 && !jit.get_fault_write());
    CHECK(jit.instructions_executed() - fault_executed_before == 0); // ticks count only COMPLETED blocks (REGION_ABI); the movs' block faulted at the ldr
    CHECK(jit.get_last_error().find("guest memory read fault") != std::string::npos);
}
} // namespace

int main() {
    MemState mem;
    CHECK(init(mem, true));
    CHECK(try_alloc_at(mem, code, page, "JIT backend tests") == code);
    CHECK(try_alloc_at(mem, data, 2 * page, "JIT checked memory") == data);
    helpers(mem);
    backend(mem);
    formation(mem);
    region_exec(mem);
    deinit_mem(mem);
    std::printf("WasmJit backend: %u checks passed (real memory, no interpreter)\n", checks);
}
