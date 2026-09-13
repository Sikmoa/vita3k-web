// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend.h"

#include <array>
#include <stdexcept>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/frontend/A32/translate/a32_translate.h>
#include <dynarmic/frontend/A32/translate/translate_callbacks.h>
#include <fmt/format.h>
#include <mem/functions.h>

namespace vita3k::wasmjit {
namespace {

class FetchCallbacks final : public Dynarmic::A32::TranslateCallbacks {
public:
    FetchCallbacks(MemState &mem, uint32_t budget)
        : mem(mem), remaining(budget) {}

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
        if (remaining == 0) {
            // current_location already includes the previous instruction's PC
            // and IT advance; reconstructing from the initial CPSR loses this.
            ir.SetTerm(Dynarmic::IR::Term::LinkBlock{ir.current_location});
            return false;
        }
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
};

} // namespace

Dynarmic::IR::Block translate_block(MemState &mem, uint32_t pc, uint32_t cpsr,
    uint32_t max_instructions, uint32_t fpscr) {
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
    FetchCallbacks callbacks{mem, max_instructions};
    return Dynarmic::A32::Translate(location, &callbacks, options);
}

} // namespace vita3k::wasmjit
