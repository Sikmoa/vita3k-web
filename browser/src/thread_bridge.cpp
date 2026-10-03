// SPDX-License-Identifier: GPL-2.0-or-later
#include "thread_bridge.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

namespace {
struct Call {
    const char *operation;
    std::array<uint64_t, 8> arguments;
    em_proxying_ctx *context = nullptr;
    int result = -1;
};
}

extern "C" EMSCRIPTEN_KEEPALIVE void vita3k_web_proxy_finish(Call *call, int result) {
    call->result = result;
    emscripten_proxy_finish(call->context);
}

EM_JS(void, vita3k_web_proxy_dispatch, (Call *call, const char *operation, const uint64_t *values), {
    const name = UTF8ToString(Module['vita3kHostOffset'](operation));
    const offset = Module['vita3kHostOffset'](values, 64);
    const args = Array.from(new BigUint64Array(wasmMemory.buffer, offset, 8), Number);
    Promise.resolve().then(() => {
        if (Module['vita3kThreadCall']) return Module['vita3kThreadCall'](name, args);
        if (Module['vita3kNullGpu']) return 0;
        throw new Error('coordinator bridge is unavailable: ' + name);
    }).then(
        result => _vita3k_web_proxy_finish(call, result ?? 0),
        error => {
            err('[vita3k-web] ' + name + ': ' + (error.stack || error));
            _vita3k_web_proxy_finish(call, -1);
        });
});

namespace browser {
int coordinator_call(const char *operation, std::array<uint64_t, 8> arguments) {
    // A dedicated queue permits asynchronous JS completions. The system
    // proxy queue is reserved for nonblocking runtime work.
    // Node's null-GPU bench has no browser consumers.
    static const bool null_gpu = EM_ASM_INT({ return Module['vita3kNullGpu'] ? 1 : 0; });
    if (null_gpu) return 0;
    static em_proxying_queue *queue = em_proxying_queue_create();
    Call call{operation, arguments};
    const bool completed = emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx *context, void *opaque) {
            auto &call = *static_cast<Call *>(opaque);
            call.context = context;
            vita3k_web_proxy_dispatch(&call, call.operation, call.arguments.data());
        }, &call);
    return completed ? call.result : -1;
}
}
