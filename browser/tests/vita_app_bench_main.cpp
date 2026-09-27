// Node driver for the retail-app launch path (vita3k_web_run_app).
// Staged content root holds a Vita filesystem subtree:
//   <root>/ux0/app/<title>/...  <root>/vs0/...  <root>/os0/...
// SELF segments must be decrypted offline (see vita_self_decrypt.cpp).
// Usage: vita3k_web_app_bench <root> [title_id] [app_path]
#include <cstdint>
#include <cstdio>
#include <emscripten.h>

extern "C" {
void vita3k_web_set_app_paths(const char *vita_fs, const char *title_id, const char *app_path);
void vita3k_web_set_license_key(const std::uint8_t *key16);
int vita3k_web_run_app();
}

// VITA3K_BENCH_INPUT="<ms>:<hex SCE_CTRL mask>:<hold ms>,...": scripted pad
// input, applied when the run loop yields to the event loop (once per frame).
EM_JS(void, schedule_bench_input, (), {
    const script = (typeof process !== 'undefined' && process.env.VITA3K_BENCH_INPUT) || '';
    for (const entry of script.split(',').filter(Boolean)) {
        const [at, mask, hold] = entry.split(':');
        const set = (buttons) => Module['_vita3k_web_set_pad'](buttons >>> 0, 0, 0, 0, 0);
        setTimeout(() => set(parseInt(mask, 16)), Number(at));
        setTimeout(() => set(0), Number(at) + Number(hold || 200));
    }
});

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr, "Usage: %s <vita-fs-root> [title_id] [app_path]\n", argv[0]);
        return 2;
    }
    const char *title = argc > 2 ? argv[2] : "PCSE00268";
    const char *app = argc > 3 ? argv[3] : title;
    vita3k_web_set_app_paths(argv[1], title, app);
    vita3k_web_set_license_key(nullptr);
    schedule_bench_input();
    const int code = vita3k_web_run_app();
    std::printf("[app-bench] exit=%d\n", code);
    return code >= 0 ? 0 : 1;
}
