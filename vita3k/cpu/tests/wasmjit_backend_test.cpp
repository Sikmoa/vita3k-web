// Focused wasm32 backend integration tests. Compile this TU INSTEAD OF
// wasm_jit_cpu.cpp so the anonymous checked helpers can also be tested directly.
// Uses production MemState and Dynarmic translation; no interpreter or mocks.
#include "../src/wasm_jit_cpu.cpp"
#include <mem/ptr.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/ir/opcodes.h>
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

    // A speculative fetch beyond the last mapped instruction must not discard
    // that instruction. Retrying run also reuses the shortened cached block.
    for (const bool thumb : {false, true}) {
        const uint32_t last_word = thumb ? 0x3001bf00 : 0xe2800001; // ADD r0,#1
        CHECK(mem_write(mem, code + page - 4, &last_word, sizeof(last_word)));
        jit.invalidate_jit_cache(code, page);
        const Address entry = code + page - (thumb ? 2 : 4);
        uint64_t compiled_after_first = 0;
        for (unsigned run = 0; run < 2; ++run) {
            jit.set_cpsr(0x10);
            jit.set_pc(entry | (thumb ? 1 : 0));
            jit.set_reg(0, 41);
            const auto count = jit.instructions_executed();
            const auto old_hits = jit.cache_hits();
            CHECK(jit.run() == -1 && !parent.svc_called);
            CHECK(jit.get_reg(0) == 42 && jit.get_pc() == code + page);
            CHECK(jit.instructions_executed() == count + 1);
            CHECK(jit.get_last_error().find("instruction fetch failed") != std::string::npos);
            if (run == 0) compiled_after_first = jit.compiled_blocks();
            else {
                CHECK(jit.compiled_blocks() == compiled_after_first);
                CHECK(jit.cache_hits() > old_hits);
            }
        }
        const auto at_fault = jit.save_context();
        const auto count = jit.instructions_executed();
        CHECK(jit.run() == -1); // The first instruction itself is now unmapped.
        equal_context(jit.save_context(), at_fault);
        CHECK(jit.instructions_executed() == count);
    }
}
// M14c region formation: DFS membership, one-block-per-PC, PSR metadata,
// tick accounting and sorted output on a real Thumb CFG with a loop, an
// unconditional branch over dead code, and a SVC-terminated block.
void formation(MemState &mem) {
    // 0x00 movs r0,#0; 0x02 adds r0,#1; 0x04 cmp r0,#3; 0x06 bne 0x02;
    // 0x08 b 0x0c; 0x0a nop (unreachable); 0x0c svc 0x42
    const std::array<uint32_t, 4> words{0x30012000, 0xd1fc2803, 0xbf00e000, 0xbf00df42};
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
    const std::array<uint32_t, 4> words{0x30012000, 0xd1fc2803, 0xbf00e000, 0xbf00df42};
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);
    CHECK(jit.get_reg(0) == 3); // movs r0,#1, then adds runs twice (1->2->3)
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
    CHECK(jit.run() == 0); // cannot-fit slice boundary: clean return, no error
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
    CHECK(jit.get_pc() == code + 2); // faulting ldr, not the block entry
    CHECK(jit.get_last_error().find("guest memory read fault") != std::string::npos);

    // Budget fully consumed is an error in BOTH modes (the interpreter-oracle
    // runaway-guard contract); only the cannot-fit slice boundary returns 0.
    // Restore the loop CFG first: the fault program overwrote it.
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    jit.set_instruction_budget(4); // exactly block A's 4 ticks
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    const uint64_t exhausted_before = jit.instructions_executed();
    CHECK(jit.run() == -1);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);
    CHECK(jit.instructions_executed() - exhausted_before == 4);
    jit.set_region_mode(false);
    jit.set_instruction_budget(2); // the program cannot finish in 2 instructions
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    CHECK(jit.run() == -1);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);
}

void region_budget_continuations(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(1);

    // A dynamic branch consumes the last tick and returns Miss, not Budget.
    put(mem, jit, {0xe12fff11, 0xef000042}); // BX r1; SVC 0x42
    jit.set_reg(1, code + 4);
    auto count = jit.instructions_executed();
    CHECK(jit.run() == -1 && !parent.svc_called);
    CHECK(jit.instructions_executed() == count + 1 && jit.get_pc() == code + 4);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);

    // SMC wins over the dispatch budget check. Exhaustion must still be an
    // error after invalidating code, before executing the modified instruction.
    put(mem, jit, {0xe5810000, 0xe3a02001, 0xef000042});
    jit.set_reg(0, 0xe3a0202a); // Replace MOV r2,#1 with MOV r2,#42.
    jit.set_reg(1, code + 4);
    jit.set_reg(2, 9);
    count = jit.instructions_executed();
    const auto invalidated = jit.invalidated_blocks();
    CHECK(jit.run() == -1 && !parent.svc_called);
    CHECK(jit.instructions_executed() == count + 1 && jit.get_pc() == code + 4);
    CHECK(jit.get_reg(2) == 9 && jit.invalidated_blocks() > invalidated);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);
    jit.set_instruction_budget(2);
    CHECK(jit.run() == 0 && parent.svc_called && jit.get_reg(2) == 42);

    // A completed SVC still succeeds when it consumes exactly the last tick.
    put(mem, jit, {0xef000042});
    jit.set_instruction_budget(1);
    CHECK(jit.run() == 0 && parent.svc_called);
    jit.set_pc(code);
    jit.set_instruction_budget(0);
    count = jit.instructions_executed();
    CHECK(jit.run() == -1 && !parent.svc_called);
    CHECK(jit.instructions_executed() == count && jit.get_pc() == code);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);
}

void region_regressions(MemState &mem) {
    namespace A32 = Dynarmic::A32;
    namespace IR = Dynarmic::IR;
    using Op = IR::Opcode;
    using Value = IR::Value;
    using Reason = vita3k::wasmjit::ExitReason;
    const auto loc = [](uint32_t pc) {
        return A32::LocationDescriptor{pc, A32::PSR{0x10}, A32::FPSCR{0}};
    };
    const auto blank = [&](uint32_t pc) {
        IR::Block block{loc(pc)};
        block.SetEndLocation(loc(pc + 4));
        block.SetTerminal(IR::Term::LinkBlock{loc(pc + 4)});
        block.CycleCount() = 1;
        return block;
    };
    const auto append = [](IR::Block &block, Op op,
                            std::initializer_list<Value> args) {
        block.AppendNewInst(op, args);
        return Value{&block.back()};
    };
    const auto emit = [](std::initializer_list<const IR::Block *> input) {
        std::vector<const IR::Block *> blocks(input);
        std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
        for (const auto *block : blocks) {
            const A32::LocationDescriptor at(block->Location());
            meta.push_back({at.PC(), PSR_DISPATCH_MASK,
                at.CPSR().Value() & PSR_DISPATCH_MASK,
                static_cast<uint32_t>(block->CycleCount()
                    + block->ConditionFailedCycleCount())});
        }
        return vita3k::wasmjit::emit_region(blocks, meta);
    };
    const auto run = [](const std::vector<uint8_t> &bytes, JitState &state,
                         uint32_t budget) {
        CHECK(!bytes.empty());
        const int slot = vita3k_jit_install_region(bytes.data(), bytes.size(),
            checked_memory_read, checked_memory_write);
        CHECK(slot >= 0);
        const auto reason = vita3k_jit_run(slot,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&state)), budget);
        vita3k_jit_release_region(slot);
        return static_cast<Reason>(reason);
    };

    // Rejection after a valid prefix must discard the entire body.
    auto bad = blank(code);
    append(bad, Op::A32SetRegister, {Value{A32::Reg::R0}, Value{uint32_t(7)}});
    append(bad, Op::Breakpoint, {});
    CHECK(emit({&bad}).empty());

    // First SSA slot: a wide producer in A, then zero-extension in B.
    auto a = blank(code), b = blank(code + 4);
    const auto wide = append(a, Op::Pack2x32To1x64,
        {Value{uint32_t(0)}, Value{uint32_t(0xdeadbeef)}});
    append(a, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D0}, wide});
    const auto narrow = append(b, Op::ZeroExtendWordToLong, {Value{uint32_t(1)}});
    append(b, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D1}, narrow});
    append(b, Op::A32SetRegister,
        {Value{static_cast<A32::Reg>(15)}, Value{uint32_t(code + 8)}});
    append(b, Op::A32CallSupervisor, {Value{uint32_t(0x66)}});
    b.ReplaceTerminal(IR::Term::ReturnToDispatch{});
    const auto module = emit({&a, &b});
    JitState state{};
    state.regs[15] = code;
    state.cpsr = 0x10;
    CHECK(run(module, state, 2) == Reason::Svc);
    CHECK(state.fpu[1] == 0xdeadbeef);
    CHECK(state.fpu[2] == 1 && state.fpu[3] == 0);
    CHECK(state.next_pc == code + 8 && state.executed == 2);

    // No side effects after SVC, including dead/invalidated IR markers.
    append(b, Op::Void, {});
    CHECK(emit({&b}).empty());

    for (const auto reason : {Reason::Stop, Reason::Smc}) {
        state = JitState{};
        state.regs[15] = code + 4;
        state.cpsr = 0x10;
        state.executed = 17;
        state.stop_flag = reason == Reason::Stop;
        state.smc_dirty = reason == Reason::Smc;
        CHECK(run(module, state, 2) == reason);
        CHECK(state.next_pc == code + 4 && state.executed == 17);
    }

    auto bx = blank(code);
    append(bx, Op::A32BXWritePC, {Value{uint32_t(code + 0x21)}});
    bx.ReplaceTerminal(IR::Term::ReturnToDispatch{});
    state = JitState{};
    state.regs[15] = code;
    state.cpsr = 0x10;
    CHECK(run(emit({&bx}), state, 1) == Reason::Miss);
    CHECK(state.regs[15] == code + 0x20 && state.next_pc == code + 0x20);
    CHECK((state.cpsr & 0x20) != 0);

    // Real region execution across both 32-bit counter wrap boundaries.
    auto loop = blank(code);
    const auto r0 = append(loop, Op::A32GetRegister, {Value{A32::Reg::R0}});
    const auto sum = append(loop, Op::Add32,
        {r0, Value{uint32_t(1)}, Value{false}});
    append(loop, Op::A32SetRegister, {Value{A32::Reg::R0}, sum});
    loop.ReplaceTerminal(IR::Term::LinkBlock{loc(code)});
    state = JitState{};
    state.regs[15] = code;
    state.cpsr = 0x10;
    state.executed = state.dispatches = 0xfffffff0;
    CHECK(run(emit({&loop}), state, 32) == Reason::Budget);
    CHECK(state.regs[0] == 32 && state.executed == 0x10);
    CHECK(counter_delta(0xfffffff0, state.executed) == 32);
    // Light dispatch path: the loop-back edge pre-checks the next block's
    // budget (REGION_ABI.md v1.2), so the failing iteration is NOT counted;
    // the old search-leaf check consumed one extra dispatch-loop trip (33).
    CHECK(counter_delta(0xfffffff0, state.dispatches) == 32);

    // Reference counts, cross-page writes, and full-width address rounding.
    Region tracked;
    RegionBlock tracked_block;
    tracked_block.pc = data + page;
    tracked_block.original.resize(4);
    tracked.blocks.push_back(std::move(tracked_block));
    collect_code_pages(tracked);
    CHECK(g_code_pages[data / page] == 0);
    CHECK(g_code_pages[data / page + 1] == 0);
    for (unsigned i = 0; i < 256; ++i) mark_code_pages(tracked, +1);
    CHECK(g_code_pages[data / page + 1] == 256);
    state = JitState{};
    state.memory_cookie = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&mem));
    state.memory_value[0] = 0x12345678;
    CHECK(checked_memory_write(&state, data + page - 2, 4) == 0);
    CHECK(state.smc_dirty == 1);
    for (unsigned i = 0; i < 256; ++i) mark_code_pages(tracked, -1);
    CHECK(g_code_pages[data / page + 1] == 0);
    tracked.blocks[0].pc = 0xfffff000;
    collect_code_pages(tracked);
    CHECK(tracked.page_begin == 0xfffff && tracked.page_end == 0x100000);

    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(4096);

    // A guest store changes the next instruction before it executes.
    put(mem, jit, {0xe5810000, 0xe3a02001, 0xef000042});
    jit.set_reg(0, 0xe3a0202a);
    jit.set_reg(1, code + 4);
    CHECK(jit.run() == 0 && parent.svc_called && jit.get_reg(2) == 42);
    CHECK(jit.invalidated_blocks() != 0);

    // 64 ARM instructions need a 256-byte snapshot.
    std::array<uint32_t, 65> long_code{};
    long_code.fill(0xe2800001);
    long_code.back() = 0xef000042;
    CHECK(mem_write(mem, code, long_code.data(), sizeof(long_code)));
    jit.invalidate_jit_cache(code, page);
    jit.set_cpsr(0x10);
    jit.set_pc(code);
    jit.set_reg(0, 0);
    CHECK(jit.run() == 0 && parent.svc_called && jit.get_reg(0) == 64);

    // BNE targets UDF, but Z=1 takes the supported SVC fallthrough.
    put(mem, jit, {0x1a000000, 0xef000042, 0xe7f000f0});
    jit.set_cpsr(0x40000010);
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);

    // LRU eviction releases compiled regions and their page references.
    std::vector<uint32_t> calls(REGION_CACHE_LIMIT + 1, 0xef000042);
    CHECK(calls.size() * sizeof(uint32_t) <= page);
    CHECK(mem_write(mem, code, calls.data(), calls.size() * sizeof(uint32_t)));
    jit.invalidate_jit_cache(code, page);
    jit.set_cpsr(0x10);
    for (size_t i = 0; i < calls.size(); ++i) {
        jit.set_pc(code + static_cast<uint32_t>(i * 4));
        CHECK(jit.run() == 0 && parent.svc_called);
    }
    const auto formed = jit.regions_formed();
    jit.set_pc(code);
    CHECK(jit.run() == 0 && parent.svc_called);
    CHECK(jit.regions_formed() == formed + 1);
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
    region_budget_continuations(mem);
    region_regressions(mem);
    deinit_mem(mem);
    std::printf("WasmJit backend: %u checks passed (real memory, no interpreter)\n", checks);
}
