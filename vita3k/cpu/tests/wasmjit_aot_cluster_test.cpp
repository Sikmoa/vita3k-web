// AOT-1 minimal real-Limbo feasibility experiment.
//
// Target: verified real PCSE00268 eboot Thumb cluster [0x811622da,0x811622f2).
// Prior evidence (ignored .limbo_work/REALCODE_PROFILE_2026-09-24.md + replay
// oracle in /tmp/vita3k-realprof) reports one exact replay needs 458,745 guest
// instructions (65,535 x 7 ticks), ends with lr=65535, and fills all 65,535
// halfwords with their indices.
//
// The committed fixture below carries ONLY the 24 code bytes of that cluster
// (seven standard Thumb-2 instruction encodings implementing
// `for (lr = 0; lr < 65535; ++lr) ((uint16_t *)r7)[lr] = lr`), verified
// byte-identical against the ignored staging capture before commit. No retail
// data bytes are embedded: the cluster performs no guest loads, so the buffer
// starts from a rotating host pattern and every run independently proves the
// fill. A 2-byte SVC #0 harness terminator is mapped immediately past the
// cluster exit so run() has a clean cooperative boundary; it is outside the
// cluster range and its 1 tick is accounted separately.
//
// Stage 1 (static closure gate): test-side CFG/SCC over the existing
// translate_block, following static LinkBlock/LinkBlockFast and
// condition-failed successors, calling existing validate_region_block.
// Stage 2 (codegen/measurement gate): interpreter vs lazy region JIT vs
// precompiled (WasmJitCPU::precompile_region) AOT on identical inputs,
// repeating the exact cluster to separate steady execution from
// load/emit/install costs. Fails closed (abort) on any gate violation.
#include "../src/wasm_jit_cpu.cpp"
#include <cpu/impl/interpreter_cpu.h>
#include <dynarmic/ir/opcodes.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)

// --- Fixture ---------------------------------------------------------------
constexpr uint32_t kCodeBase = 0x81162000; // one RX page; mirrors capture mapping
constexpr uint32_t kCodeSize = 0x1000;
constexpr uint32_t kEntry = 0x811622da;
constexpr uint32_t kExit = 0x811622f2; // terminator lives here (outside cluster)
// Store-continuation resume point: the STRH.W at [kEntry+4,kEntry+8) delimits
// the loop block's first segment (2 ticks), so the per-iteration stop/SMC poll
// and slice-budget side exit publish the ADD.W address below as the resume PC
// (REGION_ABI.md "Store continuations"). The resume CPSR/FPSCR are the
// measured mid-run values: T set, IT/E/mode bits at entry values, NZCV holding
// the previous iteration's CMP result (N=1: lr < 0xffff unsigned), FPSCR
// untouched by the loop. Observed byte-identical on every slice-boundary
// event (first-slice Miss of run 0 and epoch-stale re-resolve of run 1) and
// confirmed by dozens of chained in-Wasm transfers on later runs.
constexpr uint32_t kCont = 0x811622e2;
constexpr uint32_t kContCpsr = 0x80000020;
constexpr uint32_t kContFpscr = 0x80000010;
constexpr uint32_t kBufBase = 0x81949000;
constexpr uint32_t kBufSize = 0x22000; // 34 pages RW; mirrors capture mapping
constexpr uint32_t kIters = 65535;
constexpr uint64_t kLoopTicks = 458745; // kIters * 7, oracle-verified
// 24 cluster bytes, little-endian halfwords with per-instruction comments:
//   EA4F004E LSL.W  R0,LR,#1 | F827E000 STRH.W LR,[R7,R0] |
//   F10E0E01 ADD.W  LR,LR,#1 | FA1FFE8E UXTH.W LR,LR |
//   F64FFF70 MOVW   R0,#0xFFFF | 4586 CMP LR,R0 | D3F3 BLO ->kEntry
constexpr uint8_t kCluster[24] = {
    0x4f, 0xea, 0x4e, 0x00, 0x27, 0xf8, 0x00, 0xe0,
    0x0e, 0xf1, 0x01, 0x0e, 0x1f, 0xfa, 0x8e, 0xfe,
    0x4f, 0xf6, 0xff, 0x70, 0x86, 0x45, 0xf3, 0xd3,
};
constexpr uint8_t kTerminator[2] = {0x00, 0xdf}; // SVC #0 (harness, past exit)
// Live-captured entry registers (facts from the ignored profile; r7 selects
// the buffer offset, lr is rewound to the loop start).
constexpr uint32_t kRegs[16] = {0x00000002, 0x00000001, 0x00000000, 0x00020000,
    0x8116c3af, 0x81723a4c, 0x00000008, 0x81949890,
    0x81723a6c, 0x81723a18, 0x817239f8, 0x81723818,
    0x0000001c, 0x80404e90, 0x00000000, kEntry};
constexpr uint32_t kCpsr = 0x80000020; // N + Thumb; NZCV are runtime inputs
constexpr uint32_t kFpscr = 0x80000010;
constexpr uint32_t kTpidruro = 0x80003800;
constexpr uint64_t kBudget = 4000000;

double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
uint32_t fnv1a(const void *data, size_t size) {
    uint32_t hash = 0x811c9dc5;
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x01000193;
    }
    return hash;
}

#ifdef __EMSCRIPTEN__
EM_JS(double, aot1_js_heap_used, (), {
    try {
        if (typeof performance !== 'undefined' && performance.memory
            && performance.memory.usedJSHeapSize)
            return performance.memory.usedJSHeapSize;
    } catch (e) {}
    return -1;
});
#else
double aot1_js_heap_used() { return -1; }
#endif

struct Fixture {
    MemState mem{};
    bool owned = false;
    ~Fixture() {
        if (owned)
            deinit_mem(mem);
    }
};
// Map code+buffer at the captured guest addresses, install the verified
// cluster bytes plus the harness terminator, and prove fetch-byte identity.
void map_fixture(Fixture &fixture) {
    CHECK(init(fixture.mem, true));
    fixture.owned = true;
    CHECK(alloc_at(fixture.mem, kCodeBase, kCodeSize, "aot1-code") == kCodeBase);
    CHECK(alloc_at(fixture.mem, kBufBase, kBufSize, "aot1-buffer") == kBufBase);
    CHECK(mem_write(fixture.mem, kEntry, kCluster, sizeof(kCluster)));
    CHECK(mem_write(fixture.mem, kExit, kTerminator, sizeof(kTerminator)));
    std::array<uint8_t, sizeof(kCluster)> fetched{};
    CHECK(mem_fetch(fixture.mem, kEntry, fetched.data(), fetched.size()));
    CHECK(std::equal(std::begin(kCluster), std::end(kCluster), fetched.begin()));
    CHECK(mem_set_permissions(fixture.mem, kCodeBase, kCodeSize, MemPerm::ReadExecute));
    CHECK(mem_set_permissions(fixture.mem, kBufBase, kBufSize, MemPerm::ReadWrite));
}
// Rotating host pattern: every run independently proves the fill because no
// pattern halfword can survive where an index was written (and the first run
// starts from a fully non-identity buffer).
void pattern_buffer(MemState &mem, uint32_t run) {
    std::array<uint32_t, 1024> chunk{};
    const uint32_t word = 0xa5a50000u | (run & 0xffffu);
    chunk.fill(word);
    for (uint32_t offset = 0; offset < kBufSize; offset += sizeof(chunk))
        CHECK(mem_write(mem, kBufBase + offset, chunk.data(), sizeof(chunk)));
}
void verify_fill(MemState &mem) {
    std::array<uint8_t, 4096> chunk{};
    for (uint32_t i = 0; i < kIters;) {
        const uint32_t left = kIters - i;
        const uint32_t take = std::min<uint32_t>(left, 2048);
        CHECK(mem_read(mem, 0x81949890 + i * 2, chunk.data(), take * 2));
        for (uint32_t j = 0; j < take; ++j) {
            const uint32_t want = i + j;
            const uint32_t got = chunk[2 * j] | (uint32_t(chunk[2 * j + 1]) << 8);
            if (got != want) {
                std::fprintf(stderr, "buffer[%u]: got %u\n", want, got);
                std::abort();
            }
        }
        i += take;
    }
}
// Test-side emission must mirror the per-CPU predicate in WasmJitCPU::Impl
// (assume_fast_bases from the live MemState); process defaults alone emit
// the guarded variant (+55 bytes here) instead of the installed module.
vita3k::wasmjit::RegionStateOptions test_emit_options(MemState &mem) {
    auto options = vita3k::wasmjit::region_state_options();
    options.assume_fast_bases = mem.page_permissions != nullptr
        && (mem.direct_host_memory || (mem.sparse_host_memory && mem.page_table != nullptr));
    return options;
}
uint32_t hash_fill(MemState &mem) {
    uint32_t hash = 0x811c9dc5;
    std::array<uint8_t, 4096> chunk{};
    for (uint32_t i = 0; i < kIters;) {
        const uint32_t take = std::min<uint32_t>(kIters - i, 2048);
        CHECK(mem_read(mem, 0x81949890 + i * 2, chunk.data(), take * 2));
        hash = fnv1a(chunk.data(), take * 2) ^ (hash * 0x01000193);
        i += take;
    }
    return hash;
}

void reset_regs(InterpreterCPU &cpu) {
    for (unsigned r = 0; r < 16; ++r)
        cpu.set_reg(r, kRegs[r]);
    cpu.set_cpsr(kCpsr);
    cpu.set_fpscr(kFpscr);
    cpu.set_tpidruro(kTpidruro);
    cpu.clear_exclusive();
}
void reset_regs(WasmJitCPU &cpu) {
    for (unsigned r = 0; r < 16; ++r)
        cpu.set_reg(r, kRegs[r]);
    cpu.set_cpsr(kCpsr);
    cpu.set_fpscr(kFpscr);
    cpu.set_tpidruro(kTpidruro);
    cpu.clear_exclusive();
}

// --- Stage 1: static closure gate ------------------------------------------
struct CfgNode {
    uint32_t pc = 0, cpsr = 0, fpscr = 0;
    bool operator<(const CfgNode &other) const {
        return std::tie(pc, cpsr, fpscr) < std::tie(other.pc, other.cpsr, other.fpscr);
    }
};
struct TermInfo {
    std::string name;
    std::vector<CfgNode> static_targets;
    bool dynamic = false; // PopRSBHint / FastDispatchHint: runtime-resolved
};
struct TermVisitor {
    TermInfo *out;
    void add_link(const Dynarmic::IR::LocationDescriptor &raw) {
        const Dynarmic::A32::LocationDescriptor next{raw};
        out->static_targets.push_back(
            CfgNode{next.PC(), next.CPSR().Value(), next.FPSCR().Value()});
    }
    void operator()(const Dynarmic::IR::Term::Invalid &) { out->name = "Invalid"; }
    void operator()(const Dynarmic::IR::Term::Interpret &) { out->name = "Interpret"; }
    void operator()(const Dynarmic::IR::Term::ReturnToDispatch &) {
        out->name = "ReturnToDispatch";
    }
    void operator()(const Dynarmic::IR::Term::LinkBlock &t) {
        out->name = "LinkBlock";
        add_link(t.next);
    }
    void operator()(const Dynarmic::IR::Term::LinkBlockFast &t) {
        out->name = "LinkBlockFast";
        add_link(t.next);
    }
    void operator()(const Dynarmic::IR::Term::PopRSBHint &) {
        out->name = "PopRSBHint";
        out->dynamic = true;
    }
    void operator()(const Dynarmic::IR::Term::FastDispatchHint &) {
        out->name = "FastDispatchHint";
        out->dynamic = true;
    }
    void operator()(const boost::recursive_wrapper<Dynarmic::IR::Term::If> &t) {
        out->name = "If";
        boost::apply_visitor(*this, t.get().then_);
        boost::apply_visitor(*this, t.get().else_);
    }
    void operator()(const boost::recursive_wrapper<Dynarmic::IR::Term::CheckBit> &t) {
        out->name = "CheckBit";
        boost::apply_visitor(*this, t.get().then_);
        boost::apply_visitor(*this, t.get().else_);
    }
    void operator()(const boost::recursive_wrapper<Dynarmic::IR::Term::CheckHalt> &t) {
        out->name = "CheckHalt";
        boost::apply_visitor(*this, t.get().else_);
    }
};
struct BlockRecord {
    CfgNode node;
    uint64_t ticks = 0;
    TermInfo term;
    bool has_cond_failed = false;
    CfgNode cond_failed{};
    bool validated = false;
    std::vector<std::string> ir_names;
    bool has_svc = false;
    bool has_bx_pc = false;
    uint32_t import_nid = 0;
};

// Tarjan SCC (iterative) over node indices.
std::vector<std::vector<size_t>> strongly_connected(
    const std::vector<std::vector<size_t>> &edges) {
    const size_t n = edges.size();
    std::vector<size_t> index(n, SIZE_MAX), low(n, 0);
    std::vector<char> on_stack(n, 0);
    std::vector<size_t> stack, call;
    std::vector<size_t> next_child(n, 0);
    std::vector<std::vector<size_t>> sccs;
    size_t counter = 0;
    for (size_t root = 0; root < n; ++root) {
        if (index[root] != SIZE_MAX)
            continue;
        call.push_back(root);
        while (!call.empty()) {
            const size_t v = call.back();
            if (index[v] == SIZE_MAX) {
                index[v] = low[v] = counter++;
                stack.push_back(v);
                on_stack[v] = 1;
            }
            bool descended = false;
            while (next_child[v] < edges[v].size()) {
                const size_t w = edges[v][next_child[v]++];
                if (index[w] == SIZE_MAX) {
                    call.push_back(w);
                    descended = true;
                    break;
                }
                if (on_stack[w])
                    low[v] = std::min(low[v], index[w]);
            }
            if (descended)
                continue;
            call.pop_back();
            if (!call.empty()) {
                const size_t parent = call.back();
                low[parent] = std::min(low[parent], low[v]);
            }
            if (low[v] == index[v]) {
                std::vector<size_t> scc;
                size_t w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    on_stack[w] = 0;
                    scc.push_back(w);
                } while (w != v);
                sccs.push_back(std::move(scc));
            }
        }
    }
    return sccs;
}

void stage1() {
    Fixture fixture;
    map_fixture(fixture);
    MemState &mem = fixture.mem;
    // Test-side DFS with the existing frontend, mirroring production
    // form_region(): the same 3-level translation fallback (continuations,
    // then plain, then single-instruction) and one member per PC. Full
    // (cpsr, fpscr) descriptors are RECORDED per PC for the mode-variant
    // metric instead of forking the CFG: NZCV are runtime inputs, while
    // T/E/IT/FPSCR-mode bits select code. The harness terminator is mapped,
    // so the exit successor translates; anything past it is unmapped and
    // becomes an enumerable Miss escape.
    std::map<uint32_t, size_t> ids; // guest PC -> member index
    std::map<uint32_t, std::set<std::pair<uint32_t, uint32_t>>> descriptors;
    std::vector<BlockRecord> blocks;
    std::vector<CfgNode> stack{CfgNode{kEntry, kCpsr, kFpscr}};
    size_t unmapped_fallthrough = 0;
    std::set<uint32_t> code_pages;
    while (!stack.empty()) {
        const CfgNode node = stack.back();
        stack.pop_back();
        descriptors[node.pc].insert({node.cpsr, node.fpscr});
        if (ids.count(node.pc))
            continue;
        const size_t id = blocks.size();
        ids[node.pc] = id;
        blocks.push_back(BlockRecord{});
        BlockRecord &record = blocks.back();
        record.node = node;
        // Production-equivalent candidate ladder.
        Dynarmic::IR::Block ir{Dynarmic::A32::LocationDescriptor{node.pc,
            Dynarmic::A32::PSR{node.cpsr}, Dynarmic::A32::FPSCR{node.fpscr}}};
        std::vector<vita3k::wasmjit::StoreContinuation> continuations;
        bool translated = false;
        for (int attempt = 0; attempt < 3 && !translated; ++attempt) {
            continuations.clear();
            const bool use_cont = attempt == 0;
            const uint32_t limit = attempt == 2 ? 1 : REGION_BLOCK_INSTR_LIMIT;
            try {
                ir = vita3k::wasmjit::translate_block(mem, node.pc, node.cpsr,
                    limit, node.fpscr, use_cont ? &continuations : nullptr);
                const uint64_t end =
                    Dynarmic::A32::LocationDescriptor(ir.EndLocation()).PC();
                const uint64_t ticks =
                    ir.CycleCount() + ir.ConditionFailedCycleCount();
                if (end <= node.pc || end - node.pc > REGION_MAX_CODE_BYTES || !ticks
                    || ticks > REGION_MAX_TICKS)
                    continue;
                if (!vita3k::wasmjit::validate_region_block(ir, continuations))
                    continue;
                translated = true;
            } catch (const std::exception &) {
            }
        }
        CHECK(translated); // fail closed: every reachable block must lower
        const uint64_t end = Dynarmic::A32::LocationDescriptor(ir.EndLocation()).PC();
        for (uint32_t page = node.pc / 4096; page <= (end - 1) / 4096; ++page)
            code_pages.insert(page);
        record.ticks = ir.CycleCount() + ir.ConditionFailedCycleCount();
        TermVisitor visitor{&record.term};
        boost::apply_visitor(visitor, ir.GetTerminal());
        if (ir.GetCondition() != Dynarmic::IR::Cond::AL && ir.HasConditionFailedLocation()) {
            const auto failed = Dynarmic::A32::LocationDescriptor(ir.ConditionFailedLocation());
            record.has_cond_failed = true;
            record.cond_failed = CfgNode{failed.PC(), failed.CPSR().Value(), failed.FPSCR().Value()};
        }
        record.validated = true; // validated in the ladder above
        for (const auto &insn : ir) {
            const auto op = insn.GetOpcode();
            record.ir_names.push_back(Dynarmic::IR::GetNameOf(op));
            if (op == Dynarmic::IR::Opcode::A32CallSupervisor)
                record.has_svc = true;
            if (op == Dynarmic::IR::Opcode::A32BXWritePC)
                record.has_bx_pc = true;
        }
        record.import_nid = hot_stub_nid(mem, node.pc, node.cpsr);
        // Expand static successors; unmapped fetch targets are Miss escapes.
        auto expand = [&](const CfgNode &target) {
            std::array<uint8_t, 4> probe{};
            if (!mem_fetch(mem, target.pc, probe.data(), probe.size())) {
                ++unmapped_fallthrough;
                return;
            }
            stack.push_back(target);
        };
        for (const auto &target : record.term.static_targets)
            expand(target);
        if (record.has_cond_failed)
            expand(record.cond_failed);
        std::printf("AOT1_STAGE1_BLOCK pc=%08x cpsr=%08x fpscr=%08x ticks=%llu term=%s%s valid=%d ir=%zu svc=%d nid=%08x targets=[",
            node.pc, node.cpsr, node.fpscr, (unsigned long long)record.ticks,
            record.term.name.c_str(), record.term.dynamic ? "/dynamic" : "",
            record.validated ? 1 : 0, record.ir_names.size(), record.has_svc ? 1 : 0,
            record.import_nid);
        bool first_target = true;
        for (const auto &target : record.term.static_targets) {
            std::printf("%s%08x", first_target ? "" : ",", target.pc);
            first_target = false;
        }
        if (record.has_cond_failed)
            std::printf("%scondfailed:%08x", first_target ? "" : ",", record.cond_failed.pc);
        std::printf("] ir_first=%s ir_last=%s\n",
            record.ir_names.empty() ? "-" : record.ir_names.front().c_str(),
            record.ir_names.empty() ? "-" : record.ir_names.back().c_str());
    }
    // Edges + SCCs.
    std::vector<std::vector<size_t>> edges(blocks.size());
    size_t edge_count = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        auto link = [&](const CfgNode &target) {
            auto it = ids.find(target.pc);
            if (it == ids.end())
                return; // unmapped Miss escape, counted above
            edges[i].push_back(it->second);
            ++edge_count;
        };
        for (const auto &target : blocks[i].term.static_targets)
            link(target);
        if (blocks[i].has_cond_failed)
            link(blocks[i].cond_failed);
    }
    const auto sccs = strongly_connected(edges);
    size_t cyclic = 0;
    for (const auto &scc : sccs) {
        if (scc.size() > 1) {
            ++cyclic;
            continue;
        }
        const auto &outs = edges[scc[0]];
        if (std::find(outs.begin(), outs.end(), scc[0]) != outs.end())
            ++cyclic;
    }
    // Histograms and boundary inventory.
    std::map<std::string, size_t> term_hist, unsupported_hist;
    size_t unsupported_blocks = 0, svc_sites = 0, import_stubs = 0, dynamic_terms = 0;
    size_t bx_pc = 0;
    // Histograms and boundary inventory. Descriptor variants are counted
    // across every visit (branch edges legitimately carry NZCV states);
    // the gate only forbids forks in actual mode bits.
    size_t raw_variants = 0, max_variants_per_pc = 0;
    std::set<std::pair<uint32_t, uint32_t>> masked_modes;
    std::set<uint32_t> it_states;
    for (const auto &[pc, set] : descriptors) {
        raw_variants += set.size();
        max_variants_per_pc = std::max(max_variants_per_pc, set.size());
        for (const auto &[cpsr, fpscr] : set) {
            // Production dispatch keys mask both registers (CPSR_MODE_MASK /
            // FPSCR_MODE_MASK): NZCV and reserved bits are runtime inputs.
            masked_modes.insert({cpsr & PSR_DISPATCH_MASK,
                fpscr & Dynarmic::A32::LocationDescriptor::FPSCR_MODE_MASK});
            it_states.insert(cpsr & 0x0600fc00u); // IT[7:0] bits
        }
    }
    for (const auto &block : blocks) {
        term_hist[block.term.name]++;
        if (block.term.dynamic)
            ++dynamic_terms;
        if (!block.validated) {
            ++unsupported_blocks;
            for (const auto &name : block.ir_names)
                unsupported_hist[name]++;
        }
        if (block.has_svc)
            ++svc_sites;
        if (block.has_bx_pc)
            ++bx_pc;
        if (block.import_nid)
            ++import_stubs;
    }
    const auto in_cluster = [&](uint32_t pc) { return pc >= kEntry && pc < kExit; };
    size_t cluster_blocks = 0;
    bool cluster_clean = true;
    size_t dynamic_outside_terminator = 0;
    bool terminator_is_svc_boundary = false;
    for (const auto &block : blocks) {
        // The single allowed dynamic terminal is the harness SVC terminator
        // at kExit (outside the cluster): the enumerable Svc escape.
        if (block.term.dynamic && block.node.pc != kExit)
            ++dynamic_outside_terminator;
        if (block.node.pc == kExit && block.has_svc && !in_cluster(block.node.pc))
            terminator_is_svc_boundary = true;
        if (!in_cluster(block.node.pc))
            continue;
        ++cluster_blocks;
        if (block.has_svc || block.import_nid || block.has_bx_pc || block.term.dynamic)
            cluster_clean = false;
    }
    // Static gate. In-cluster successors must be enumerable (no dynamic
    // terminals); the harness SVC terminator at kExit is the one allowed
    // dynamic terminal (Svc escape). Unsupported IR concentrated at zero,
    // mode bits single-valued under the production dispatch masks (NZCV
    // variants are runtime inputs, recorded above), and escapes mappable
    // onto Miss (unmapped fallthrough) / Svc (harness terminator) / Fault /
    // Budget / Smc behavior. The cluster itself must hold no SVC, no import
    // stub, and no indirect branch.
    const bool gate = unsupported_blocks == 0 && dynamic_outside_terminator == 0 && bx_pc == 0
        && masked_modes.size() == 1 && code_pages.size() == 1 && cluster_blocks >= 1
        && cluster_clean && svc_sites == 1 && terminator_is_svc_boundary;
    std::printf("AOT1_STAGE1 nodes=%zu edges=%zu sccs=%zu cyclic=%zu",
        blocks.size(), edge_count, sccs.size(), cyclic);
    std::printf(" unsupported=%zu dynamic_terms=%zu bx_pc=%zu masked_modes=%zu",
        unsupported_blocks, dynamic_terms, bx_pc, masked_modes.size());
    std::printf(" raw_variants=%zu max_per_pc=%zu code_pages=%zu",
        raw_variants, max_variants_per_pc, code_pages.size());
    std::printf(" svc_sites=%zu import_stubs=%zu unmapped_escapes=%zu dyn_outside_term=%zu term_svc=%d gate=%s\n",
        svc_sites, import_stubs, unmapped_fallthrough, dynamic_outside_terminator,
        terminator_is_svc_boundary ? 1 : 0, gate ? "PASS" : "FAIL");
    std::printf("AOT1_STAGE1_SCC sizes=[");
    for (size_t i = 0; i < sccs.size(); ++i)
        std::printf("%s%zu", i ? "," : "", sccs[i].size());
    std::printf("] terminals={");
    bool first = true;
    for (const auto &[name, count] : term_hist) {
        std::printf("%s%s:%zu", first ? "" : ",", name.c_str(), count);
        first = false;
    }
    std::printf("} pcs=[");
    for (size_t i = 0; i < blocks.size(); ++i)
        std::printf("%s%08x", i ? "," : "", blocks[i].node.pc);
    std::printf("]\n");
    std::printf("AOT1_STAGE1_FETCH ");
    for (unsigned i = 0; i < sizeof(kCluster); ++i)
        std::printf("%02x", kCluster[i]);
    std::printf(" entry_modes=cpsr:%08x/fpscr:%08x\n", kCpsr, kFpscr);
    CHECK(gate);
    std::printf("AOT1_STAGE1_GATE PASS\n");
}

// --- Stage 2: codegen/measurement gate -------------------------------------
struct RunSample {
    double ms = 0;
    uint64_t executed = 0;
};

template <typename Cpu>
struct PhaseResult {
    std::string name;
    std::vector<RunSample> runs;
    uint64_t total_executed = 0;
    uint32_t end_lr = 0, end_r0 = 0, end_r7 = 0, end_cpsr = 0, end_fpscr = 0, end_tpidruro = 0;
    uint32_t buffer_hash = 0;
    CPUContext end_context{};
    std::string profile;
};

constexpr unsigned kRepeats = 11; // run[0] = warm/load, runs[1..] = steady

PhaseResult<InterpreterCPU> phase_interpreter() {
    PhaseResult<InterpreterCPU> out{"interpreter"};
    Fixture fixture;
    map_fixture(fixture);
    CPUState parent{};
    parent.mem = &fixture.mem;
    parent.thread_id = 45;
    InterpreterCPU cpu(&parent, 0);
    cpu.set_instruction_budget(kBudget);
    for (unsigned run = 0; run < kRepeats; ++run) {
        pattern_buffer(fixture.mem, run);
        reset_regs(cpu);
        parent.svc_called = false;
        const uint64_t executed_before = cpu.instructions_executed();
        const double start = now_ms();
        const int rc = cpu.run();
        const double elapsed = now_ms() - start;
        CHECK(rc == 0);
        CHECK(parent.svc_called && parent.svc == 0);
        CHECK(cpu.get_pc() == kExit + 2);
        const uint64_t executed_run = cpu.instructions_executed() - executed_before;
        CHECK(executed_run == kLoopTicks + 1); // oracle count every run
        out.runs.push_back({elapsed, executed_run});
        verify_fill(fixture.mem);
    }
    out.total_executed = cpu.instructions_executed();
    out.end_lr = cpu.get_reg(14);
    out.end_r0 = cpu.get_reg(0);
    out.end_r7 = cpu.get_reg(7);
    out.end_cpsr = cpu.get_cpsr();
    out.end_fpscr = cpu.get_fpscr();
    out.end_tpidruro = cpu.get_tpidruro();
    out.end_context = cpu.save_context();
    out.buffer_hash = hash_fill(fixture.mem);
    return out;
}

PhaseResult<WasmJitCPU> phase_lazy() {
    PhaseResult<WasmJitCPU> out{"lazy-jit"};
    Fixture fixture;
    map_fixture(fixture);
    CPUState parent{};
    parent.mem = &fixture.mem;
    parent.thread_id = 45;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    cpu.set_instruction_budget(kBudget);
    const uint64_t regions_before = cpu.regions_formed();
    for (unsigned run = 0; run < kRepeats; ++run) {
        pattern_buffer(fixture.mem, run);
        reset_regs(cpu);
        const auto counters_before = cpu.pump_counters();
        const uint64_t executed_before = cpu.instructions_executed();
        const double start = now_ms();
        const int rc = cpu.run();
        const double elapsed = now_ms() - start;
        CHECK(rc == 0);
        CHECK(parent.svc_called && parent.svc == 0);
        CHECK(cpu.get_pc() == kExit + 2);
        const uint64_t executed_run = cpu.instructions_executed() - executed_before;
        CHECK(executed_run == kLoopTicks + 1); // oracle count every run
        out.runs.push_back({elapsed, executed_run});
        (void)counters_before;
        verify_fill(fixture.mem);
    }
    out.total_executed = cpu.instructions_executed();
    out.end_lr = cpu.get_reg(14);
    out.end_r0 = cpu.get_reg(0);
    out.end_r7 = cpu.get_reg(7);
    out.end_cpsr = cpu.get_cpsr();
    out.end_fpscr = cpu.get_fpscr();
    out.end_tpidruro = cpu.get_tpidruro();
    out.end_context = cpu.save_context();
    out.buffer_hash = hash_fill(fixture.mem);
    out.profile = cpu.get_profile();
    // Production publishes only the region ENTRY key to the dispatch map
    // (execute_regions inserts one key per ensure_region). The loop-exit arm
    // to kExit stays in-region (light-chained member of the entry closure),
    // so no standalone terminator region ever forms. The second lazy compile
    // is the store-continuation resume point at kCont: every 131072-tick pump
    // slice ends mid-loop-iteration at the STRH.W boundary, whose Budget side
    // exit publishes kCont; the Wasm dispatcher converts that to a Miss while
    // kCont is unmapped, and the host forms the 3-block continuation region.
    // Two lazy compiles total (entry + continuation), then cache hits apart
    // from one epoch-stale re-resolve per fresh CPU (see phase_aot).
    std::printf("AOT1_LAZY regions_formed=%llu regions_before=%llu\n  profile=%s\n",
        (unsigned long long)cpu.regions_formed(), (unsigned long long)regions_before,
        out.profile.c_str());
    { // Production closure from the entry: loop block + terminator block.
        Region region;
        std::vector<Dynarmic::IR::Block> ir;
        CHECK(form_region(fixture.mem, kEntry, kCpsr, kFpscr, region, ir));
        CHECK(region.blocks.size() == 2 && region.total_ticks == 8);
    }
    { // Production closure from the continuation resume point: partial loop
        // body (post-store) + full loop block + terminator block.
        Region region;
        std::vector<Dynarmic::IR::Block> ir;
        CHECK(form_region(fixture.mem, kCont, kContCpsr, kContFpscr, region, ir));
        CHECK(region.blocks.size() == 3);
        bool has_cont = false;
        for (const auto &block : region.blocks)
            has_cont = has_cont || block.pc == kCont;
        CHECK(has_cont); // members are PC-sorted: [kEntry, kCont, kExit]
    }
    CHECK(cpu.regions_formed() == regions_before + 2); // entry + continuation
    return out;
}

struct AotOutcome {
    PhaseResult<WasmJitCPU> runs;
    WasmJitCPU::PrecompileResult pre{}; // entry closure (loop + terminator)
    WasmJitCPU::PrecompileResult pre_cont{}; // continuation region (resume at kCont)
    double pre_cont_wall_ms = 0;
    double heap_before = -1, heap_after_install = -1, heap_after_runs = -1;
    size_t wasm_functions = 0, wasm_imports = 0, wasm_defined = 0;
    size_t cont_wasm_functions = 0, cont_wasm_imports = 0, cont_wasm_defined = 0;
    size_t cont_wasm_bytes = 0;
};

bool parse_wasm_counts(const std::vector<uint8_t> &bytes, size_t &functions,
    size_t &imports, size_t &defined) {
    functions = imports = defined = 0;
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "\0asm", 4) != 0)
        return false;
    size_t pos = 8;
    const auto u32 = [&](uint32_t &value) {
        value = 0;
        unsigned shift = 0;
        while (pos < bytes.size()) {
            const uint8_t byte = bytes[pos++];
            value |= uint32_t(byte & 0x7f) << shift;
            shift += 7;
            if (!(byte & 0x80))
                return shift <= 35;
            if (shift >= 35)
                return false;
        }
        return false;
    };
    while (pos < bytes.size()) {
        const uint8_t id = bytes[pos++];
        uint32_t size = 0;
        if (!u32(size))
            return false;
        const size_t end = pos + size;
        if (end > bytes.size())
            return false;
        if (id == 2) { // imports
            uint32_t count = 0;
            if (!u32(count))
                return false;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t length = 0;
                if (!u32(length) || pos + length > end)
                    return false;
                pos += length;
                if (!u32(length) || pos + length > end)
                    return false;
                pos += length;
                if (pos >= end)
                    return false;
                const uint8_t kind = bytes[pos++];
                if (kind == 0) {
                    uint32_t index = 0;
                    if (!u32(index))
                        return false;
                    ++imports;
                } else if (kind == 1) {
                    if (pos >= end)
                        return false;
                    ++pos; // element type
                    uint32_t flags = 0, initial = 0, max = 0;
                    if (!u32(flags) || !u32(initial))
                        return false;
                    if ((flags & 1) && !u32(max))
                        return false;
                } else if (kind == 2) {
                    uint32_t flags = 0, initial = 0, max = 0;
                    if (!u32(flags) || !u32(initial))
                        return false;
                    if ((flags & 1) && !u32(max))
                        return false;
                } else if (kind == 3) {
                    if (pos + 2 > end)
                        return false;
                    pos += 2; // value type + mutability
                } else {
                    return false;
                }
            }
        } else if (id == 3) { // functions
            uint32_t count = 0;
            if (!u32(count))
                return false;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t index = 0;
                if (!u32(index))
                    return false;
                ++defined;
            }
        }
        pos = end;
    }
    functions = imports + defined;
    return true;
}

AotOutcome phase_aot() {
    AotOutcome outcome;
    outcome.runs.name = "aot";
    Fixture fixture;
    map_fixture(fixture);
    CPUState parent{};
    parent.mem = &fixture.mem;
    parent.thread_id = 45;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    cpu.set_instruction_budget(kBudget);
    reset_regs(cpu);
    outcome.heap_before = aot1_js_heap_used();
    // Precompile/install BEFORE first execution through the existing path.
    // Two precompiles cover the genuine runtime closure, mirroring what the
    // lazy path installs: the entry closure plus the store-continuation
    // resume region at kCont. Production publishes only region ENTRY keys to
    // the dispatch map, so the slice-boundary Budget side exit at kCont
    // resolves to its own map entry; the loop-exit arm to kExit needs none
    // (light-chained member of the entry closure, never separately selected).
    const double compile_start = now_ms();
    outcome.pre = cpu.precompile_region();
    const double compile_wall = now_ms() - compile_start;
    CHECK(outcome.pre.ok);
    CHECK(!outcome.pre.cache_hit);
    CHECK(outcome.pre.blocks == 2); // loop block + harness terminator block
    CHECK(outcome.pre.ticks == 8); // 7 loop ticks + 1 terminator tick
    CHECK(outcome.pre.wasm_bytes > 0);
    // Continuation descriptor measured mid-run at the STRH.W boundary
    // (pc=811622e2 cpsr=80000020 fpscr=80000010): entry mode bits with the
    // previous iteration's CMP flags. Under the production dispatch masks
    // this is the same key every slice-boundary resume carries (NZCV agree
    // here too, and dozens of chained transfers prove the key is stable).
    cpu.set_reg(15, kCont);
    cpu.set_cpsr(kContCpsr);
    cpu.set_fpscr(kContFpscr);
    const double cont_start = now_ms();
    outcome.pre_cont = cpu.precompile_region();
    outcome.pre_cont_wall_ms = now_ms() - cont_start;
    CHECK(outcome.pre_cont.ok);
    CHECK(!outcome.pre_cont.cache_hit);
    CHECK(outcome.pre_cont.blocks == 3); // partial loop body + loop + terminator
    CHECK(outcome.pre_cont.wasm_bytes > 0);
    // Re-precompile of each installed entry must be a pure cache hit: no new
    // region, same shape, map republished.
    reset_regs(cpu);
    const uint64_t regions_after_two = cpu.regions_formed();
    CHECK(regions_after_two == 2);
    const auto pre_again = cpu.precompile_region();
    CHECK(pre_again.ok && pre_again.cache_hit);
    CHECK(pre_again.blocks == outcome.pre.blocks);
    CHECK(cpu.regions_formed() == regions_after_two);
    cpu.set_reg(15, kCont);
    cpu.set_cpsr(kContCpsr);
    cpu.set_fpscr(kContFpscr);
    const auto pre_cont_again = cpu.precompile_region();
    CHECK(pre_cont_again.ok && pre_cont_again.cache_hit);
    CHECK(pre_cont_again.blocks == outcome.pre_cont.blocks);
    CHECK(cpu.regions_formed() == regions_after_two);
    reset_regs(cpu);
    // Re-emit the identical closure test-side for exact module metrics. The
    // CPU was built with process-default region options, so the same options
    // reproduce the installed module's shape (bytes are not retained by the
    // install path, only installed).
    {
        Region region;
        std::vector<Dynarmic::IR::Block> ir;
        CHECK(form_region(fixture.mem, kEntry, kCpsr, kFpscr, region, ir));
        CHECK(region.blocks.size() == outcome.pre.blocks);
        std::vector<const Dynarmic::IR::Block *> ptrs;
        std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
        for (size_t i = 0; i < ir.size(); ++i) {
            ptrs.push_back(&ir[i]);
            meta.push_back({region.blocks[i].pc, region.blocks[i].psr_mask,
                region.blocks[i].psr_value, region.blocks[i].ticks,
                region.blocks[i].store_continuations, region.blocks[i].hot_nid});
        }
        const auto bytes = vita3k::wasmjit::emit_region(
            ptrs, meta, test_emit_options(fixture.mem));
        CHECK(!bytes.empty());
        CHECK(bytes.size() == outcome.pre.wasm_bytes);
        CHECK(parse_wasm_counts(bytes, outcome.wasm_functions, outcome.wasm_imports,
            outcome.wasm_defined));
        CHECK(outcome.wasm_functions > 0);
    }
    outcome.heap_after_install = aot1_js_heap_used();
    // Same re-emission for the continuation region: byte-identity with what
    // the second precompile installed.
    {
        Region region;
        std::vector<Dynarmic::IR::Block> ir;
        CHECK(form_region(fixture.mem, kCont, kContCpsr, kContFpscr, region, ir));
        CHECK(region.blocks.size() == outcome.pre_cont.blocks);
        std::vector<const Dynarmic::IR::Block *> ptrs;
        std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
        for (size_t i = 0; i < ir.size(); ++i) {
            ptrs.push_back(&ir[i]);
            meta.push_back({region.blocks[i].pc, region.blocks[i].psr_mask,
                region.blocks[i].psr_value, region.blocks[i].ticks,
                region.blocks[i].store_continuations, region.blocks[i].hot_nid});
        }
        const auto bytes = vita3k::wasmjit::emit_region(
            ptrs, meta, test_emit_options(fixture.mem));
        CHECK(!bytes.empty());
        CHECK(bytes.size() == outcome.pre_cont.wasm_bytes);
        outcome.cont_wasm_bytes = bytes.size();
        CHECK(parse_wasm_counts(bytes, outcome.cont_wasm_functions,
            outcome.cont_wasm_imports, outcome.cont_wasm_defined));
        CHECK(outcome.cont_wasm_functions > 0);
    }
    for (unsigned run = 0; run < kRepeats; ++run) {
        pattern_buffer(fixture.mem, run);
        reset_regs(cpu);
        const uint64_t executed_before = cpu.instructions_executed();
        const double start = now_ms();
        const int rc = cpu.run();
        const double elapsed = now_ms() - start;
        CHECK(rc == 0);
        CHECK(parent.svc_called && parent.svc == 0);
        CHECK(cpu.get_pc() == kExit + 2);
        const uint64_t executed_run = cpu.instructions_executed() - executed_before;
        CHECK(executed_run == kLoopTicks + 1); // oracle count every run
        outcome.runs.runs.push_back({elapsed, executed_run});
        verify_fill(fixture.mem);
    }
    // No lazy compile may have happened after the precompiles: the steady
    // path must be pure execution of the installed regions. This gate also
    // proves the precompiled keys cover every runtime resume point: any
    // uncovered key (a wrong kCont descriptor, an unstable resume CPSR)
    // would form a new region here and fail closed.
    // Known non-formation traffic: run 1 re-resolves kCont through the host
    // cache (no new region) because the fresh CPU's first HLE-gated entry
    // takes the version-ack epoch bump, staling the installed map entries;
    // the loop-top key refreshes in place while kCont is only referenced
    // mid-pump. The lazy path carries the identical re-resolve.
    outcome.runs.profile = cpu.get_profile();
    CHECK(cpu.regions_formed() == regions_after_two);
    outcome.heap_after_runs = aot1_js_heap_used();
    outcome.runs.total_executed = cpu.instructions_executed();
    outcome.runs.end_lr = cpu.get_reg(14);
    outcome.runs.end_r0 = cpu.get_reg(0);
    outcome.runs.end_r7 = cpu.get_reg(7);
    outcome.runs.end_cpsr = cpu.get_cpsr();
    outcome.runs.end_fpscr = cpu.get_fpscr();
    outcome.runs.end_tpidruro = cpu.get_tpidruro();
    outcome.runs.end_context = cpu.save_context();
    outcome.runs.buffer_hash = hash_fill(fixture.mem);
    std::printf("AOT1_AOT entry_blocks=%zu entry_ticks=%u entry_compile_wall_ms=%.3f entry_emit_ms=%.3f entry_install_ms=%.3f entry_bytes=%zu\n",
        outcome.pre.blocks, outcome.pre.ticks, compile_wall, outcome.pre.emit_ms,
        outcome.pre.install_ms, outcome.pre.wasm_bytes);
    std::printf("AOT1_AOT cont_blocks=%zu cont_ticks=%u cont_compile_wall_ms=%.3f cont_emit_ms=%.3f cont_install_ms=%.3f cont_bytes=%zu\n",
        outcome.pre_cont.blocks, outcome.pre_cont.ticks, outcome.pre_cont_wall_ms,
        outcome.pre_cont.emit_ms, outcome.pre_cont.install_ms, outcome.pre_cont.wasm_bytes);
    return outcome;
}

double steady_mean_ms(const std::vector<RunSample> &runs) {
    CHECK(runs.size() == kRepeats);
    double sum = 0;
    for (unsigned i = 1; i < kRepeats; ++i)
        sum += runs[i].ms;
    return sum / (kRepeats - 1);
}
double steady_min_ms(const std::vector<RunSample> &runs) {
    double best = runs[1].ms;
    for (unsigned i = 2; i < kRepeats; ++i)
        best = std::min(best, runs[i].ms);
    return best;
}

// Fault equivalence: r7 leaves exactly one writable halfword, so iteration 1
// faults on the STRH. All modes must report the same PC/address/direction.
template <typename Cpu>
struct FaultExpect {
    int rc = 0;
    uint32_t pc = 0, address = 0;
    bool write = false;
    std::string error;
};
void fault_case_interpreter(FaultExpect<InterpreterCPU> &expect) {
    Fixture fixture;
    map_fixture(fixture);
    CPUState parent{};
    parent.mem = &fixture.mem;
    InterpreterCPU cpu(&parent, 0);
    cpu.set_instruction_budget(kBudget);
    reset_regs(cpu);
    cpu.set_reg(7, kBufBase + kBufSize - 2);
    expect.rc = cpu.run();
    expect.pc = cpu.get_pc();
    expect.error = cpu.get_last_error();
    char want[32];
    std::snprintf(want, sizeof(want), "%08x", kBufBase + kBufSize);
    expect.address = expect.error.find(want) != std::string::npos
        ? static_cast<uint32_t>(kBufBase + kBufSize)
        : 0;
    expect.write = expect.error.find("write") != std::string::npos;
}
void fault_case_jit(FaultExpect<WasmJitCPU> &expect, bool precompile) {
    Fixture fixture;
    map_fixture(fixture);
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    cpu.set_instruction_budget(kBudget);
    reset_regs(cpu);
    if (precompile) {
        const auto pre = cpu.precompile_region();
        CHECK(pre.ok);
    }
    cpu.set_reg(7, kBufBase + kBufSize - 2);
    expect.rc = cpu.run();
    expect.pc = cpu.get_pc();
    expect.address = cpu.get_fault_address();
    expect.write = cpu.get_fault_write();
    expect.error = cpu.get_last_error();
}

// SMC equivalence on the AOT path: patching the code page must drop the
// installed region (invalidation accounting fires), and restoring the exact
// bytes must reproduce the exact output.
void smc_case() {
    Fixture fixture;
    map_fixture(fixture);
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    cpu.set_instruction_budget(kBudget);
    reset_regs(cpu);
    const auto pre = cpu.precompile_region();
    CHECK(pre.ok);
    pattern_buffer(fixture.mem, 77);
    CHECK(cpu.run() == 0 && parent.svc_called);
    verify_fill(fixture.mem);
    const uint64_t regions_before = cpu.regions_formed();
    const uint64_t invalidated_before = cpu.invalidated_blocks();
    // Patch the loop entry to a bare NOP: the next run must observe the
    // changed bytes (SMC exit), drop the covering region, and terminate
    // immediately at the harness SVC... note the NOP still falls into the
    // real loop, so instead patch entry to SVC #0 for a hang-free probe.
    // The code page is mapped RX, so toggle writability around the patch;
    // the permission change itself is a tracked write (per-page generation
    // bump), and the changed bytes fail the entry validation below.
    const uint8_t svc_patch[2] = {0x00, 0xdf};
    CHECK(mem_set_permissions(fixture.mem, kCodeBase, kCodeSize, MemPerm::ReadWrite));
    CHECK(mem_write(fixture.mem, kEntry, svc_patch, sizeof(svc_patch)));
    CHECK(mem_set_permissions(fixture.mem, kCodeBase, kCodeSize, MemPerm::ReadExecute));
    reset_regs(cpu);
    parent.svc_called = false;
    CHECK(cpu.run() == 0 && parent.svc_called);
    CHECK(cpu.regions_formed() > regions_before);
    CHECK(cpu.invalidated_blocks() > invalidated_before);
    // Restore byte-identically and prove the exact output returns.
    CHECK(mem_set_permissions(fixture.mem, kCodeBase, kCodeSize, MemPerm::ReadWrite));
    CHECK(mem_write(fixture.mem, kEntry, kCluster, 2));
    CHECK(mem_set_permissions(fixture.mem, kCodeBase, kCodeSize, MemPerm::ReadExecute));
    std::array<uint8_t, sizeof(kCluster)> fetched{};
    CHECK(mem_fetch(fixture.mem, kEntry, fetched.data(), fetched.size()));
    CHECK(std::equal(std::begin(kCluster), std::end(kCluster), fetched.begin()));
    cpu.invalidate_jit_cache(kCodeBase, kCodeSize);
    pattern_buffer(fixture.mem, 78);
    reset_regs(cpu);
    parent.svc_called = false;
    CHECK(cpu.run() == 0 && parent.svc_called);
    CHECK(cpu.get_reg(14) == kIters && cpu.get_reg(0) == 0xffff);
    verify_fill(fixture.mem);
    std::printf("AOT1_SMC regions_formed_delta=%llu invalidated_delta=%llu restore=PASS\n",
        (unsigned long long)(cpu.regions_formed() - regions_before),
        (unsigned long long)(cpu.invalidated_blocks() - invalidated_before));
}

void stage2() {
    dispatch_bump_epoch(); // normalize telemetry from any earlier phases
    const auto interp = phase_interpreter();
    const auto lazy = phase_lazy();
    const auto aot = phase_aot();
    // Correctness gates: identical fetch bytes (proven in map_fixture),
    // identical entry modes (same reset_regs), identical instruction counts,
    // identical GPR/CPSR/FPSCR/FPU/TPIDRURO state, identical memory outputs.
    CHECK(interp.total_executed == lazy.total_executed);
    CHECK(interp.total_executed == aot.runs.total_executed);
    // Counters accumulate across all runs: every run executes the oracle
    // loop ticks plus the 1-tick harness terminator (also gated per run).
    CHECK(interp.total_executed == (kLoopTicks + 1) * kRepeats);
    for (const auto *phase : {&interp.end_context, &lazy.end_context, &aot.runs.end_context}) {
        CHECK(phase->cpu_registers == interp.end_context.cpu_registers);
        CHECK(phase->cpsr == interp.end_context.cpsr);
        CHECK(phase->fpscr == interp.end_context.fpscr);
        CHECK(std::memcmp(phase->fpu_registers.data(), interp.end_context.fpu_registers.data(),
                          sizeof(interp.end_context.fpu_registers))
            == 0);
    }
    CHECK(lazy.end_tpidruro == interp.end_tpidruro);
    CHECK(aot.runs.end_tpidruro == interp.end_tpidruro);
    CHECK(interp.end_lr == kIters && interp.end_r0 == 0xffff && interp.end_r7 == 0x81949890);
    CHECK(lazy.end_lr == kIters && aot.runs.end_lr == kIters);
    CHECK(interp.buffer_hash == lazy.buffer_hash);
    CHECK(interp.buffer_hash == aot.runs.buffer_hash);
    // HLE observability: the loop holds no SVC; the only SVC is the harness
    // terminator (svc==0). No park/wake, no stop, no budget exhaustion: every
    // single run stays far below the runaway guard (counters accumulate, so
    // gate the per-run samples, each already proven equal to kLoopTicks+1).
    for (const auto &sample : interp.runs)
        CHECK(sample.executed < kBudget);
    // Fault equivalence across all three modes.
    FaultExpect<InterpreterCPU> interp_fault;
    FaultExpect<WasmJitCPU> lazy_fault, aot_fault;
    fault_case_interpreter(interp_fault);
    fault_case_jit(lazy_fault, false);
    fault_case_jit(aot_fault, true);
    CHECK(interp_fault.rc < 0 && lazy_fault.rc < 0 && aot_fault.rc < 0);
    CHECK(interp_fault.pc == kEntry + 4); // STRH.W
    CHECK(lazy_fault.pc == kEntry + 4 && aot_fault.pc == kEntry + 4);
    CHECK(interp_fault.address == kBufBase + kBufSize);
    CHECK(lazy_fault.address == kBufBase + kBufSize);
    CHECK(aot_fault.address == kBufBase + kBufSize);
    CHECK(interp_fault.write && lazy_fault.write && aot_fault.write);
    smc_case();
    // Throughput. Steady excludes run[0] (load/emit/install for lazy).
    const double interp_steady = steady_mean_ms(interp.runs);
    const double lazy_steady = steady_mean_ms(lazy.runs);
    const double aot_steady = steady_mean_ms(aot.runs.runs);
    const auto mips = [&](double ms) { return (double(kLoopTicks) / (ms / 1000.0)) / 1e6; };
    const double interp_mips = mips(interp_steady);
    const double lazy_mips = mips(lazy_steady);
    const double aot_mips = mips(aot_steady);
    const char *band = aot_mips >= 500 ? ">=500" : aot_mips >= 200 ? "200-500" : "<200";
    std::printf("AOT1_STAGE2 runs=%u loop_ticks=%llu (+1 terminator tick each run)\n", kRepeats,
        (unsigned long long)kLoopTicks);
    std::printf("AOT1_STAGE2_TIMES interp_first=%.3f steady_mean=%.3f steady_min=%.3f\n",
        interp.runs[0].ms, interp_steady, steady_min_ms(interp.runs));
    std::printf("AOT1_STAGE2_TIMES lazy_first=%.3f steady_mean=%.3f steady_min=%.3f\n",
        lazy.runs[0].ms, lazy_steady, steady_min_ms(lazy.runs));
    std::printf("AOT1_STAGE2_TIMES aot_first=%.3f steady_mean=%.3f steady_min=%.3f\n",
        aot.runs.runs[0].ms, aot_steady, steady_min_ms(aot.runs.runs));
    std::printf("AOT1_STAGE2_MIPS interp=%.1f lazy=%.1f aot=%.1f band=%s\n", interp_mips,
        lazy_mips, aot_mips, band);
    std::printf("AOT1_STAGE2_STATE lr=%u r0=%08x r7=%08x cpsr=%08x fpscr=%08x tpidruro=%08x bufhash=%08x\n",
        interp.end_lr, interp.end_r0, interp.end_r7, interp.end_cpsr, interp.end_fpscr,
        interp.end_tpidruro, interp.buffer_hash);
    std::printf("AOT1_STAGE2_MODULE entry_bytes=%zu entry_functions=%zu entry_imports=%zu entry_defined=%zu entry_blocks=%zu\n",
        aot.pre.wasm_bytes, aot.wasm_functions, aot.wasm_imports, aot.wasm_defined,
        aot.pre.blocks);
    std::printf("AOT1_STAGE2_MODULE cont_bytes=%zu cont_functions=%zu cont_imports=%zu cont_defined=%zu cont_blocks=%zu\n",
        aot.cont_wasm_bytes, aot.cont_wasm_functions, aot.cont_wasm_imports,
        aot.cont_wasm_defined, aot.pre_cont.blocks);
    std::printf("AOT1_STAGE2_HEAP js_before=%.0f js_after_install=%.0f js_after_runs=%.0f\n",
        aot.heap_before, aot.heap_after_install, aot.heap_after_runs);
    std::printf("AOT1_STAGE2_FAULT pc=%08x addr=%08x write=%d (identical interp/lazy/aot)\n",
        aot_fault.pc, aot_fault.address, aot_fault.write ? 1 : 0);
    std::printf("AOT1_STAGE2_PROFILE_LAZY %s\n", lazy.profile.c_str());
    std::printf("AOT1_STAGE2_PROFILE_AOT %s\n", aot.runs.profile.c_str());
    std::printf("AOT1_STAGE2_GATE PASS mips_band=%s\n", band);
}
} // namespace

int main() {
    stage1();
    stage2();
    std::printf("AOT1-CLUSTER: %u checks passed (real Limbo u16-fill cluster)\n", checks);
    return 0;
}
