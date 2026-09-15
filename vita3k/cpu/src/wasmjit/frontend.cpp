// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend.h"

#include <array>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A32/translate/a32_translate.h>
#include <dynarmic/frontend/A32/translate/translate_callbacks.h>
#include <dynarmic/ir/cond.h>
#include <dynarmic/ir/opcodes.h>
#include <fmt/format.h>
#include <mem/functions.h>

namespace vita3k::wasmjit {
namespace {

// Bound on merged store-delimited segments per translated block.
// Measured 2026-09-15 (display fixture, interleaved A/B, same box): any
// inline store-continuation side exit raises per-call entry cost enough to
// outweigh the dispatch savings (uncapped: 56.6 FPS / run_js ~1170ms / 12.8M
// dispatches; cap 2: ~58.5 FPS / ~1100ms / 16.3M; cap 0: 60.0 FPS / 857ms /
// 32.3M dispatches — identical to pre-continuation levels). The side-exit
// shape itself, not the merge depth, is what hurts, so continuations stay
// off until the poll can be re-shaped (e.g. routed through the dispatch
// loop instead of inline flag/budget checks). Kept as a parameter so tests
// still exercise the machinery (see kDefaultMaxStoreContinuations in frontend.h).

class FetchCallbacks final : public Dynarmic::A32::TranslateCallbacks {
public:
    using BlockIterator = decltype(std::declval<Dynarmic::IR::Block>().cbegin());

    FetchCallbacks(MemState &mem, uint32_t budget,
        std::vector<StoreContinuation> *store_continuations, size_t max_store_continuations)
        : mem(mem), remaining(budget), store_continuations(store_continuations)
        , max_store_continuations(max_store_continuations) {}

    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        std::array<uint8_t, 4> bytes{};
        if (!mem_fetch(mem, address, bytes.data(), bytes.size())) {
            // Returning nullopt would emit NoExecuteFault IR. Throw instead so
            // the caller cannot mistake a partial translation for a valid block.
            throw std::runtime_error(fmt::format(
                "wasmjit instruction fetch failed at {:#010x} (unmapped or non-executable)", address));
        }
        return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8)
            | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    }

    bool PreCodeReadHook(bool, uint32_t, Dynarmic::A32::IREmitter &ir) override {
        // This boundary follows a COMPLETE guest instruction, including all
        // elements of STM/VST1 and register writeback. Region emission can
        // put an SMC side exit here without returning to the dispatcher for
        // ordinary stores. Predicated blocks retain their original boundary
        // so their condition-failed tick/budget contract is unchanged.
        const auto scan_begin = previous_boundary && *previous_boundary != ir.block.cend()
            ? std::next(*previous_boundary) : ir.block.cbegin();
        bool wrote_memory = false;
        for (auto it = scan_begin; it != ir.block.cend(); ++it) {
            using Op = Dynarmic::IR::Opcode;
            const auto op = it->GetOpcode();
            wrote_memory |= op == Op::A32WriteMemory8 || op == Op::A32WriteMemory16
                || op == Op::A32WriteMemory32 || op == Op::A32WriteMemory64;
            ++ir_count;
        }
        if (remaining == 0 || (wrote_memory
                && (!store_continuations || ir.block.GetCondition() != Dynarmic::IR::Cond::AL))) {
            // current_location already includes the previous instruction's PC
            // and IT advance; reconstructing from the initial CPSR loses this.
            ir.SetTerm(Dynarmic::IR::Term::LinkBlock{ir.current_location});
            return false;
        }
        if (wrote_memory) {
            if (store_continuations->size() >= max_store_continuations) {
                ir.SetTerm(Dynarmic::IR::Term::LinkBlock{ir.current_location});
                return false;
            }
            store_continuations->push_back({ir_count,
                Dynarmic::IR::LocationDescriptor{ir.current_location}.Value(),
                static_cast<uint32_t>(ir.block.CycleCount())});
        }
        // The hook runs before Dynarmic translates the next instruction. Keep
        // the beginning of just the preceding instruction's IR so the store
        // check stays linear in the translation instead of rescanning the
        // entire accumulated block on every frontend iteration.
        previous_boundary = ir.block.empty()
            ? std::optional<BlockIterator>{ir.block.cend()}
            : std::optional<BlockIterator>{std::prev(ir.block.cend())};
        return true;
    }

    void PreCodeTranslationHook(bool, uint32_t, Dynarmic::A32::IREmitter &) override {
        --remaining;
    }

    uint64_t GetTicksForCode(bool, uint32_t, uint32_t) override {
        return 1; // Instruction budget accounting, not hardware timing.
    }

private:
    MemState &mem;
    uint32_t remaining;
    std::vector<StoreContinuation> *store_continuations;
    size_t max_store_continuations;
    uint32_t ir_count = 0;
    std::optional<BlockIterator> previous_boundary;
};

} // namespace

Dynarmic::IR::Block translate_block(MemState &mem, uint32_t pc, uint32_t cpsr,
    uint32_t max_instructions, uint32_t fpscr,
    std::vector<StoreContinuation> *store_continuations, size_t max_store_continuations) {
    if (store_continuations)
        store_continuations->clear();
    if (max_instructions == 0) {
        throw std::invalid_argument("wasmjit translation budget must be nonzero");
    }
    const bool thumb = (cpsr & (1u << 5)) != 0;
    if ((pc & (thumb ? 1u : 3u)) != 0) {
        throw std::invalid_argument("wasmjit PC must be an aligned instruction address (CPSR.T selects Thumb)");
    }

    const Dynarmic::A32::LocationDescriptor location{
        pc, Dynarmic::A32::PSR{cpsr}, Dynarmic::A32::FPSCR{fpscr}};
    const Dynarmic::A32::TranslationOptions options{
        .arch_version = Dynarmic::A32::ArchVersion::v7,
        .define_unpredictable_behaviour = false,
        .hook_hint_instructions = false,
    };
    FetchCallbacks callbacks{mem, max_instructions, store_continuations, max_store_continuations};
    auto block = Dynarmic::A32::Translate(location, &callbacks, options);
    // The next decoded instruction may have forced a conditional split
    // without contributing any ticks. The ordinary terminal owns that exit.
    if (store_continuations) {
        while (!store_continuations->empty()
            && store_continuations->back().completed_ticks >= block.CycleCount())
            store_continuations->pop_back();
    }
    return block;
}

} // namespace vita3k::wasmjit
