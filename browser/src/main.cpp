// Vita3K browser bootstrap (M1).
// This target intentionally validates only the browser lifecycle and runtime
// capability boundary. Emulator subsystems are added in later milestones.

#include <emscripten/emscripten.h>

#include <cstdio>

namespace {

extern "C" {
EMSCRIPTEN_KEEPALIVE
int vita3k_web_initialize() {
    std::puts("[vita3k-web] initialize: browser bootstrap ready");
    return 0;
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
