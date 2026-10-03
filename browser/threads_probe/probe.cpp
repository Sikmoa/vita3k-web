// Phase 0 of THREADS.md: can this browser run the threaded build at all, and
// what do its building blocks cost? Built like the emulator (Memory64, fixed
// 8 GiB memory) plus -pthread, and run from a dedicated Worker like the
// emulator's coordinator. Every result is one "[probe] name: value" line.
#include <emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

double now_ms() { return emscripten_get_now(); }

std::atomic<int> failures{0};
void check(bool ok, const char *what) {
    if (!ok) {
        ++failures;
        std::printf("[probe] FAIL: %s\n", what);
    }
}

// Shared memory: the whole fixed memory is a SharedArrayBuffer, and a store a
// pthread makes far above 4 GiB is visible to another one.
EM_JS(int, memory_is_shared, (), { return wasmMemory.buffer instanceof SharedArrayBuffer ? 1 : 0; });

struct HighMemory {
    volatile std::uint8_t *byte;
    std::uint8_t seen = 0;
};
void *read_high(void *arg) {
    auto *test = static_cast<HighMemory *>(arg);
    test->seen = *test->byte;
    return nullptr;
}
void test_memory() {
    const std::uint64_t bytes = std::uint64_t(__builtin_wasm_memory_size(0)) * 65536;
    std::printf("[probe] memory: %.2f GiB shared=%d\n", bytes / 1073741824.0, memory_is_shared());
    check(memory_is_shared(), "memory is a SharedArrayBuffer");
    // The last byte of the memory, far above 4 GiB and far above the heap
    // (malloc does not reach it here): the emulator's guest window lives up there.
    HighMemory test{reinterpret_cast<volatile std::uint8_t *>(static_cast<std::uintptr_t>(bytes - 1))};
    *test.byte = 0x5a;
    pthread_t thread;
    pthread_create(&thread, nullptr, read_high, &test);
    pthread_join(thread, nullptr);
    std::printf("[probe] store at %p seen by another thread: %s\n", (void *)test.byte, test.seen == 0x5a ? "yes" : "no");
    check(test.seen == 0x5a, "store above 4 GiB visible across threads");
}

// Start latency from the pool: create, the thread notes when it runs, join.
struct Started {
    double at = 0;
};
void *note_start(void *arg) {
    static_cast<Started *>(arg)->at = now_ms();
    return nullptr;
}
void print_latencies(const char *name, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    std::printf("[probe] %s: min %.3f ms, median %.3f ms, max %.3f ms (%zu)\n", name, values.front(),
        values[values.size() / 2], values.back(), values.size());
}
void test_pool_start() {
    std::vector<double> latencies;
    for (int i = 0; i < 64; ++i) {
        Started started;
        pthread_t thread;
        const double before = now_ms();
        pthread_create(&thread, nullptr, note_start, &started);
        pthread_join(thread, nullptr);
        latencies.push_back(started.at - before);
    }
    print_latencies("thread start from the pool (reused Worker)", latencies);
}

// Futex ping-pong: the cost of one guest thread waking another (condition
// variables and kernel waits come down to this).
std::atomic<std::uint32_t> turn{0};
constexpr int kRoundTrips = 20000;
void *pong(void *) {
    for (int i = 0; i < kRoundTrips; ++i) {
        while (turn.load() != 1)
            emscripten_futex_wait(&turn, 0, 100);
        turn.store(0);
        emscripten_futex_wake(&turn, 1);
    }
    return nullptr;
}
void test_futex() {
    pthread_t thread;
    pthread_create(&thread, nullptr, pong, nullptr);
    const double start = now_ms();
    for (int i = 0; i < kRoundTrips; ++i) {
        turn.store(1);
        emscripten_futex_wake(&turn, 1);
        while (turn.load() != 0)
            emscripten_futex_wait(&turn, 1, 100);
    }
    const double elapsed = now_ms() - start;
    pthread_join(thread, nullptr);
    std::printf("[probe] futex wake/wait round trip: %.2f us\n", elapsed * 1000 / kRoundTrips);
}

// Compare-and-swap from several threads, the operation STREX becomes.
constexpr int kCasThreads = 4, kCasIterations = 500000;
std::atomic<std::uint32_t> counter32{0};
std::atomic<std::uint64_t> counter64{0};
void *cas_worker(void *) {
    for (int i = 0; i < kCasIterations; ++i) {
        std::uint32_t old32 = counter32.load(std::memory_order_relaxed);
        while (!counter32.compare_exchange_weak(old32, old32 + 1)) {
        }
        std::uint64_t old64 = counter64.load(std::memory_order_relaxed);
        while (!counter64.compare_exchange_weak(old64, old64 + 3)) {
        }
    }
    return nullptr;
}
void test_cas() {
    pthread_t threads[kCasThreads];
    const double start = now_ms();
    for (auto &thread : threads)
        pthread_create(&thread, nullptr, cas_worker, nullptr);
    for (auto &thread : threads)
        pthread_join(thread, nullptr);
    const double elapsed = now_ms() - start;
    const bool ok = counter32 == std::uint32_t(kCasThreads) * kCasIterations
        && counter64 == std::uint64_t(kCasThreads) * kCasIterations * 3;
    std::printf("[probe] compare-and-swap, %d threads: %s in %.1f ms\n", kCasThreads, ok ? "exact" : "LOST UPDATES", elapsed);
    check(ok, "compare-and-swap counters exact");
}

// More threads than pooled Workers, all alive at once: the extra ones wait for
// a Worker the coordinator creates. Runs on a pthread so the main thread (the
// coordinator) stays free to create them, as THREADS.md requires.
constexpr int kBurst = 40;
std::atomic<int> burst_started{0};
std::atomic<std::uint32_t> burst_release{0};
struct BurstThread {
    double created = 0;
    double started = 0;
};
void *burst_member(void *arg) {
    static_cast<BurstThread *>(arg)->started = now_ms();
    ++burst_started;
    while (burst_release.load() == 0)
        emscripten_futex_wait(&burst_release, 0, 1000);
    return nullptr;
}
// The pool lives on the main runtime thread (the coordinator): call there only.
EM_JS(int, unused_workers, (), { return PThread.unusedWorkers.length; });
EM_JS(void, report_done, (), { if (globalThis.probeDone) globalThis.probeDone(); });
// Background top-up as THREADS.md describes it: the coordinator starts Workers
// on its event loop and reports when they are ready.
EM_JS(void, top_up, (int count), {
    const start = performance.now();
    const loads = [];
    for (let i = 0; i < count; ++i)
        loads.push(PThread.loadWasmModuleToWorker(PThread.allocateUnusedWorker()));
    Promise.all(loads).then(() => {
        out(`[probe] background top-up of ${count} Workers: ${(performance.now() - start).toFixed(1)} ms` +
            ` (${PThread.unusedWorkers.length} idle now)`);
        report_done();
    });
});
void *burst(void *) {
    std::vector<BurstThread> members(kBurst);
    std::vector<pthread_t> threads(kBurst);
    const double start = now_ms();
    for (int i = 0; i < kBurst; ++i) {
        members[i].created = now_ms();
        pthread_create(&threads[i], nullptr, burst_member, &members[i]);
    }
    while (burst_started.load() < kBurst)
        emscripten_futex_wait(&burst_started, burst_started.load(), 10);
    const double all_running = now_ms() - start;
    burst_release = 1;
    emscripten_futex_wake(&burst_release, INT32_MAX);
    for (auto &thread : threads)
        pthread_join(thread, nullptr);
    std::vector<double> latencies;
    for (const auto &member : members)
        latencies.push_back(member.started - member.created);
    print_latencies("thread start, 40 at once (pool exhausted past its size)", latencies);
    std::printf("[probe] all 40 running after %.1f ms\n", all_running);
    std::printf("[probe] failures: %d\n", failures.load());
    // Back on the coordinator: top the pool up in the background.
    emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, top_up, 8);
    return nullptr;
}

} // namespace

int main() {
    std::printf("[probe] start (pool of %d Workers ready)\n", unused_workers());
    test_memory();
    test_pool_start();
    test_futex();
    test_cas();
    std::printf("[probe] idle pooled Workers before the burst: %d\n", unused_workers());
    // The burst must not block this thread: Workers are created here.
    pthread_t thread;
    pthread_create(&thread, nullptr, burst, nullptr);
    pthread_detach(thread);
    return 0;
}
