// Vita3K browser bootstrap (M1).
// This target intentionally validates only the browser lifecycle and runtime
// capability boundary. Emulator subsystems are added in later milestones.

#include <emscripten/emscripten.h>

#include "guest.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <span>

namespace {

extern "C" {
EMSCRIPTEN_KEEPALIVE
int vita3k_web_initialize() {
    std::puts("[vita3k-web] initialize: browser bootstrap ready");
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int vita3k_web_run_elf_probe(const std::uint8_t *bytes, std::uint32_t size) {
    if (!bytes || size == 0) return -1;
    vita3k::web::Memory memory(4 * vita3k::web::page_size);
    vita3k::web::ElfLoadResult loaded;
    std::string error;
    if (!vita3k::web::load_elf32(memory, std::span<const std::uint8_t>(bytes, size), loaded, error))
        return -1;
    const auto result = vita3k::web::run_guest(memory, loaded.entry, loaded.thumb, 8);
    return result.status == vita3k::web::GuestResult::Status::Exited ? result.exit_code : -1;
}

// Browser callers provide bytes through vita3k_web_run_elf_probe. This
// compatibility export intentionally has no embedded or virtual-FS fallback.
EMSCRIPTEN_KEEPALIVE
int vita3k_web_run_guest_probe() { return -1; }

EMSCRIPTEN_KEEPALIVE
int vita3k_web_shutdown() {
    std::puts("[vita3k-web] shutdown: browser bootstrap stopped");
    return 0;
}
}

} // namespace

int main() {
    std::puts("[vita3k-web] M1 bootstrap starting");
    return vita3k_web_initialize();
}
