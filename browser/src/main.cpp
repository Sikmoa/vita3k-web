// Vita3K browser bootstrap (M1).
// This target intentionally validates only the browser lifecycle and runtime
// capability boundary. Emulator subsystems are added in later milestones.

#include <emscripten/emscripten.h>

#include "guest.h"

#include <cstdio>
#include <cstdint>
#include <string>

namespace {

extern "C" {
EMSCRIPTEN_KEEPALIVE
int vita3k_web_initialize() {
    std::puts("[vita3k-web] initialize: browser bootstrap ready");
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int vita3k_web_run_guest_probe() {
    vita3k::web::Memory memory(4 * vita3k::web::page_size);
    const std::uint32_t program[] = { 0xe3a0002au, 0xef000001u };
    vita3k::web::GuestImage image;
    image.code.assign(reinterpret_cast<const std::uint8_t *>(program),
        reinterpret_cast<const std::uint8_t *>(program) + sizeof(program));
    std::string error;
    if (!vita3k::web::load_guest_image(memory, image, vita3k::web::page_size, error))
        return -1;
    const auto result = vita3k::web::run_guest(memory, vita3k::web::page_size, false, 8);
    return result.status == vita3k::web::GuestResult::Status::Exited ? result.exit_code : -1;
}

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
