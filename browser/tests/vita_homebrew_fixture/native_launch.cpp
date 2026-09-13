// Native probe for the genuine VitaSDK fixture. Uses production runtime objects;
// no test imports, hand-built SELF, or replacement EmuEnvState implementation.
#include <cpu/functions.h>
#include <emuenv/state.h>
#include <kernel/load_self.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <util/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

// A CPU backend or HLE call need not cooperate with stop(). Bound the entire
// probe, including teardown, without racing a running CPU to inspect registers.
class Deadline {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    std::thread watchdog;

public:
    Deadline()
        : watchdog([this] {
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, 10s, [this] { return finished; })) {
                std::fputs("FAIL: native fixture deadline exceeded (10s); no completion inferred\n", stderr);
                std::fflush(stderr);
                std::_Exit(124);
            }
        }) {}
    ~Deadline() {
        {
            std::lock_guard lock(mutex);
            finished = true;
        }
        cv.notify_one();
        watchdog.join();
    }
};

void report_cpu_error(ThreadState &thread, MemState &mem) {
    // Called only with the thread mutex held after run_loop parks dormant.
    auto &cpu = *thread.cpu;
    const auto pc = read_pc(cpu);
    const bool thumb = is_thumb_mode(cpu);
    LOG_ERROR("CPU error: tid={} returned=0x{:08X} pc=0x{:08X} cpsr=0x{:08X} lr=0x{:08X} sp=0x{:08X}",
        thread.id, thread.returned_value, pc, read_cpsr(cpu), read_lr(cpu), read_sp(cpu));

    // InterpreterCPU::step restores the faulting PC on failure and emits its
    // own opcode/reason trace. Decode that address without changing guest state.
    uint8_t bytes[4]{};
    if (mem_read(mem, pc, bytes, sizeof(bytes))) {
        LOG_ERROR("Instruction at stopped PC: {} address=0x{:08X} bytes={:02X} {:02X} {:02X} {:02X} disassembly='{}'",
            thumb ? "Thumb" : "ARM", pc, bytes[0], bytes[1], bytes[2], bytes[3],
            disassemble(cpu, pc, thumb));
    }
    for (unsigned reg = 0; reg < 13; ++reg)
        LOG_ERROR("  r{}=0x{:08X}", reg, read_reg(cpu, reg));
}

int launch(const char *path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        LOG_ERROR("Cannot open fixture: {}", path);
        return 2;
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)), {});
    if (image.empty() || file.bad()) {
        LOG_ERROR("Empty/unreadable fixture: {}", path);
        return 2;
    }

    std::atomic<bool> exit_requested{ false };
    std::atomic<int> exit_status{ 0 };
    std::atomic<unsigned> import_calls{ 0 };
    EmuEnvState emuenv;
    if (!init(emuenv.mem, false)) {
        LOG_ERROR("Memory initialization failed");
        return 2;
    }
    const auto dispatch = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        ++import_calls;
        LOG_INFO("Import: nid=0x{:08X} tid={} pc=0x{:08X} lr=0x{:08X}", nid, tid, read_pc(cpu), read_lr(cpu));
        ::call_import(emuenv, cpu, nid, tid);
    };
    if (!emuenv.kernel.init(emuenv.mem, dispatch, false)) {
        LOG_ERROR("Kernel initialization failed");
        deinit_mem(emuenv.mem);
        return 2;
    }
    struct Cleanup {
        EmuEnvState &env;
        ~Cleanup() {
            env.kernel.deinit(env.mem);
            deinit_mem(env.mem);
        }
    } cleanup{ emuenv };
    emuenv.kernel.process_exit_callback = [&](int status, std::optional<AppLaunchRequest>) {
        // This is the real sceKernelExitProcess -> request_process_exit boundary.
        // Do not stop/join guest threads inside an import callback.
        exit_status.store(status);
        exit_requested.store(true);
    };
    init_libraries(emuenv);
    init_exported_vars(emuenv);

    int result = 1;
    const SceUID module_id = load_self_sized(emuenv.kernel, emuenv.mem,
        image.data(), image.size(), "app0:eboot.bin", {});
    if (module_id < 0) {
        LOG_ERROR("load_self_sized failed: 0x{:08X} ({} bytes)", static_cast<uint32_t>(module_id), image.size());
    } else {
        const auto &module = emuenv.kernel.loaded_modules.at(module_id)->info;
        LOG_INFO("Loaded genuine fixture: {} bytes, module={}, start_entry=0x{:08X}",
            image.size(), module_id, module.start_entry.address());
        if (module.start_entry) {
            // Match run_app's process-parameter selection, without initializing
            // graphics/audio or starting a frontend sync thread.
            SceInt32 priority = SCE_KERNEL_DEFAULT_PRIORITY_USER;
            SceInt32 stack_size = SCE_KERNEL_STACK_SIZE_USER_MAIN;
            SceInt32 affinity = SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT;
            if (const auto *param = emuenv.kernel.process_param.get(emuenv.mem)) {
                if (param->main_thread_priority)
                    priority = *Ptr<SceInt32>(param->main_thread_priority).get(emuenv.mem);
                if (param->main_thread_stacksize)
                    stack_size = *Ptr<SceInt32>(param->main_thread_stacksize).get(emuenv.mem);
                if (param->main_thread_cpu_affinity_mask)
                    affinity = *Ptr<SceInt32>(param->main_thread_cpu_affinity_mask).get(emuenv.mem);
            }
            // create_thread's real SDL host thread executes ThreadState::run_loop.
            // Never run the CPU directly or substitute a test thread scheduler.
            auto thread = emuenv.kernel.create_thread(emuenv.mem, "vitasdk-fixture",
                module.start_entry, priority, affinity, stack_size, nullptr);
            if (thread) {
                emuenv.main_thread_id = thread->id;
                LOG_INFO("Starting tid={} at {}", thread->id,
                    disassemble(*thread->cpu, module.start_entry.address() & ~1u,
                        (module.start_entry.address() & 1u) != 0));
                if (thread->start(0, Ptr<void>{}, true) == SCE_KERNEL_OK) {
                    while (!exit_requested.load()) {
                        {
                            std::unique_lock lock(thread->mutex);
                            if (thread->status == ThreadStatus::dormant) {
                                if (thread->returned_value == 0xDEADDEAD)
                                    report_cpu_error(*thread, emuenv.mem);
                                else
                                    LOG_ERROR("Thread returned 0x{:08X} without a process exit request", thread->returned_value);
                                break;
                            }
                        }
                        std::this_thread::sleep_for(1ms);
                    }
                } else {
                    LOG_ERROR("ThreadState::start failed");
                }
            } else {
                LOG_ERROR("KernelState::create_thread failed");
            }
        } else {
            LOG_ERROR("Fixture has no module start entry");
        }
    }

    // Real teardown wakes and waits for all production host threads. Keep the
    // deadline armed here too. A clean thread return alone is NOT success.
    emuenv.kernel.process_exit();
    LOG_INFO("Result: process_exit_requested={} status={} import_calls={} missing_nids={}",
        exit_requested.load(), exit_status.load(), import_calls.load(), emuenv.missing_nids.size());
    if (exit_requested.load() && exit_status.load() == 42 && emuenv.missing_nids.empty())
        result = 0;
    return result;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s /build/tree/eboot.bin\n", argv[0]);
        return 2;
    }
    Deadline deadline;
    try {
        return launch(argv[1]);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: native fixture exception: %s\n", error.what());
        return 2;
    }
}
