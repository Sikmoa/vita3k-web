// SPDX-License-Identifier: GPL-2.0-or-later
#include "thread_bridge.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <atomic>
#include <cstdio>

namespace {
// One queue for calls and posts: the coordinator runs its tasks in order.
em_proxying_queue *coordinator_queue() {
    static em_proxying_queue *queue = em_proxying_queue_create();
    return queue;
}
// Blocked time per operation. Operations are string literals, so the
// pointer identifies one; a short fixed table needs no lock to update.
struct OperationStats {
    std::atomic<const char *> name{nullptr};
    std::atomic<uint64_t> calls{0}, wait_us{0};
};
std::array<OperationStats, 32> operation_stats;
OperationStats *stats_for(const char *operation) {
    for (auto &slot : operation_stats) {
        const char *current = slot.name.load(std::memory_order_acquire);
        if (current == operation) return &slot;
        if (!current) {
            const char *expected = nullptr;
            if (slot.name.compare_exchange_strong(expected, operation) || expected == operation) return &slot;
        }
    }
    return nullptr;
}
struct Call {
    const char *operation;
    std::array<uint64_t, 8> arguments;
    em_proxying_ctx *context = nullptr;
    int result = -1;
    bool posted = false; // coordinator_post: nobody waits, finish deletes it
};
}

extern "C" EMSCRIPTEN_KEEPALIVE void vita3k_web_proxy_finish(Call *call, int result) {
    if (call->posted) {
        delete call;
        return;
    }
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
    em_proxying_queue *queue = coordinator_queue();
    const double started = emscripten_get_now();
    Call call{operation, arguments};
    const bool completed = emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx *context, void *opaque) {
            auto &call = *static_cast<Call *>(opaque);
            call.context = context;
            vita3k_web_proxy_dispatch(&call, call.operation, call.arguments.data());
        }, &call);
    if (auto *stats = stats_for(operation)) {
        stats->calls.fetch_add(1, std::memory_order_relaxed);
        stats->wait_us.fetch_add(uint64_t((emscripten_get_now() - started) * 1000.0), std::memory_order_relaxed);
    }
    return completed ? call.result : -1;
}

bool coordinator_post(const char *operation, std::array<uint64_t, 8> arguments) {
    static const bool null_gpu = EM_ASM_INT({ return Module['vita3kNullGpu'] ? 1 : 0; });
    if (null_gpu) return false;
    em_proxying_queue *queue = coordinator_queue();
    auto *call = new Call{operation, arguments};
    call->posted = true;
    if (auto *stats = stats_for(operation)) stats->calls.fetch_add(1, std::memory_order_relaxed);
    return emscripten_proxy_async(queue, emscripten_main_runtime_thread_id(), [](void *opaque) {
        auto *call = static_cast<Call *>(opaque);
        vita3k_web_proxy_dispatch(call, call->operation, call->arguments.data());
    }, call) != 0;
}

std::string coordinator_report() {
    std::string line;
    for (auto &slot : operation_stats) {
        const char *name = slot.name.load(std::memory_order_acquire);
        if (!name) break;
        const uint64_t calls = slot.calls.exchange(0), wait_us = slot.wait_us.exchange(0);
        if (!calls) continue;
        char item[96];
        std::snprintf(item, sizeof(item), " %s=%llux/%.0fms", name, static_cast<unsigned long long>(calls), wait_us / 1000.0);
        line += item;
    }
    return line;
}
}
