// Firmware (SceKernelThreadMgr, SceSysmem and SceGxm 3.74) process, thread and
// driver queries under the fiber runtime.
#pragma once
#include "guest_sync_delete_tests.h"
#include <gxm/state.h>
#include <gxm/types.h>

DECL_EXPORT(SceUID, sceKernelGetProcessId);
DECL_EXPORT(SceInt32, _sceKernelGetThreadInfo, SceUID threadId, Ptr<SceKernelThreadInfo> pInfo, const SceSize *pSize);
DECL_EXPORT(int, sceKernelCreateThreadForUser, const char *name, SceKernelThreadEntry entry, int init_priority, SceKernelCreateThread_opt *options);
DECL_EXPORT(SceInt32, sceKernelChangeThreadPriority, SceUID thid, SceInt32 priority);
DECL_EXPORT(int, _sceKernelGetThreadExitStatus, SceUID thid, SceInt32 *pExitStatus);
DECL_EXPORT(int, sceKernelGetThreadExitStatus, SceUID thid, SceInt32 *pExitStatus);
DECL_EXPORT(SceInt32, ksceKernelGetThreadInfo, SceUID thid, SceKernelThreadInfo *pInfo);
DECL_EXPORT(SceInt32, ksceKernelSetPermission, SceInt32 permission);
DECL_EXPORT(int, SceThreadmgrForDriver_20C228E4);
DECL_EXPORT(int, SceQafMgrForDriver_B9770A13);
DECL_EXPORT(int, sceGxmInitialize, const SceGxmInitializeParams *params);
DECL_EXPORT(int, sceGxmTerminate);
DECL_EXPORT(int, sceGxmMapVertexUsseMemory, Ptr<void> base, uint32_t size, uint32_t *offset);
DECL_EXPORT(int, sceGxmMapFragmentUsseMemory, Ptr<void> base, uint32_t size, uint32_t *offset);
DECL_EXPORT(int, sceGxmUnmapVertexUsseMemory, void *base);
DECL_EXPORT(int, sceGxmUnmapFragmentUsseMemory, void *base);
DECL_EXPORT(int, sceGxmMapVertexUsseMemoryInternal, Ptr<void> base, uint32_t size, uint32_t *offset);
DECL_EXPORT(int, sceGxmMapFragmentUsseMemoryInternal, Ptr<void> base, uint32_t size, uint32_t *offset);
DECL_EXPORT(int, sceGxmUnmapVertexUsseMemoryInternal, void *base);
DECL_EXPORT(int, sceGxmUnmapFragmentUsseMemoryInternal, void *base);
DECL_EXPORT(SceUID, sceGxmGetDisplayQueueThreadIdInternal);

namespace guest_kernel_info {
constexpr uint32_t kGetThreadId = 0x0fb972f9;
constexpr uint32_t kGetThreadInfo = 0x8d9c5461;
constexpr uint32_t kWaitSema = 0x0c7b834b;
} // namespace guest_kernel_info

inline void test_guest_kernel_info(EmuEnvState &env, vita3k::web::GuestThreadRuntime &runtime) {
    using namespace guest_kernel_info;
    const Address code = alloc(env.mem, 0x1000, "kernel info code");
    const Address data = alloc(env.mem, 0x1000, "kernel info data");
    REQUIRE(code && data);
    const auto word = [&](Address offset) -> uint32_t & { return *Ptr<uint32_t>(data + offset).get(env.mem); };
    env.kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        call_import(env, cpu, nid, tid);
        REQUIRE(env.missing_nids.empty());
    };
    const auto run_until = [&](auto done) {
        const auto limit = vita3k::web::GuestThreadRuntime::now_us() + 2000000;
        while (!done()) {
            REQUIRE(runtime.resume(64).failed == 0);
            REQUIRE(vita3k::web::GuestThreadRuntime::now_us() < limit);
        }
    };
    REQUIRE(runtime.attach(env));
    auto caller = env.kernel.create_thread(env.mem, "info caller", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    REQUIRE(caller);

    // sceKernelGetProcessId (SceKernelThreadMgr 0x810008a8) reports the calling
    // thread's process: one uid that no other kernel object gets.
    REQUIRE(export_sceKernelGetProcessId(env, caller->id, "fixture") == KernelState::process_id);
    REQUIRE(caller->tls.get_ptr<int>().get(env.mem)[TLS_PROCESS_ID] == KernelState::process_id);
    const SceUID sema = semaphore_create(env.kernel, "fixture", "info sema", 0, 0, 0, 1);
    REQUIRE(caller->id > KernelState::process_id && sema > KernelState::process_id);

    // GetThreadInfo (SceLibKernel 0x8100a790, syscall 0x81029c0c, record
    // 0x810059cc): a zeroed record carrying its own size; as many bytes as the
    // caller's size word says travel in and back out.
    const Address info_addr = data + 0x200, entry = code + 0x400;
    auto *info = Ptr<SceKernelThreadInfo>(info_addr).get(env.mem);
    const Ptr<SceKernelThreadInfo> info_ptr(info_addr);
    const auto reset = [&](SceSize size) {
        std::memset(info, 0xcc, sizeof(*info) + 8);
        info->size = size;
    };
    const auto untouched_from = [&](size_t offset) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(info);
        for (size_t i = offset; i < sizeof(*info) + 8; ++i)
            if (bytes[i] != 0xcc)
                return false;
        return true;
    };
    const auto untracked_zero = [&] {
        return info->currentCpuId == 0 && info->lastExecutedCpuId == 0 && info->waitType == 0 && info->waitId == 0
            && info->runClocks == 0 && info->intrPreemptCount == 0 && info->threadPreemptCount == 0
            && info->threadReleaseCount == 0 && info->changeCpuCount == 0 && info->fNotifyCallback == 0 && info->reserved == 0;
    };
    const auto get_info = [&](SceUID calling, SceUID thid, SceSize size) {
        return export__sceKernelGetThreadInfo(env, calling, "fixture", thid, info_ptr, &size);
    };
    guest_sync_delete::build_call(env.mem, entry, kGetThreadId, { 0, 0, 0, 0, 0 }, data);
    SceKernelCreateThread_opt options{};
    options.stack_size = 0x4000;
    options.attr = SCE_KERNEL_ATTR_TH_PRIO;
    options.cpu_affinity_mask = 0x20000;
    const SceUID target_id = export_sceKernelCreateThreadForUser(env, caller->id, "fixture", "info target",
        SceKernelThreadEntry(entry), 0x50, &options);
    REQUIRE(target_id > 0);
    const auto target = env.kernel.get_thread(target_id);
    // sceKernelCreateThreadForUser (0x8102e7a0, core 0x810061d8): options of
    // at most 0x1c bytes, affinity within the four cores, and attributes a
    // game may ask for (0x05002000) or, from a system module, any outside
    // 0x797f5fff; the user bit is added.
    {
        const auto create = [&](SceKernelCreateThread_opt &opt) {
            return export_sceKernelCreateThreadForUser(env, caller->id, "fixture", "checked thread", SceKernelThreadEntry(entry), 0x50, &opt);
        };
        auto *option = Ptr<SceKernelThreadOptParam>(data + 0x700).get(env.mem);
        option->size = 0x1d;
        option->attr = 0;
        SceKernelCreateThread_opt checked = options;
        checked.option = Ptr<SceKernelThreadOptParam>(data + 0x700);
        REQUIRE(create(checked) == SCE_KERNEL_ERROR_ILLEGAL_SIZE);
        option->size = 0x1c;
        checked.cpu_affinity_mask = 0x100000;
        REQUIRE(create(checked) == SCE_KERNEL_ERROR_ILLEGAL_CPU_AFFINITY_MASK);
        checked.cpu_affinity_mask = 0xf0000;
        checked.attr = 0x4000;
        REQUIRE(create(checked) == SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        checked.attr = 0x02000000;
        REQUIRE(create(checked) == SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        checked.attr = SCE_KERNEL_THREAD_ATTR_USER | 0x05002000;
        const SceUID game_thread = create(checked);
        REQUIRE(game_thread > 0);
        const auto made = env.kernel.get_thread(game_thread);
        REQUIRE(made->attr == (SCE_KERNEL_THREAD_ATTR_USER | 0x05002000) && made->affinity_mask == 0xf0000);
        made->exit_delete(false);
        // A caller in a system-loaded module.
        auto module = std::make_shared<KernelModule>();
        std::strcpy(module->info.path, "vs0:sys/external/libfixture.suprx");
        module->info.segments[0].vaddr = Ptr<const void>(code + 0xf00);
        module->info.segments[0].memsz = 0x100;
        module->system_loaded = true;
        const SceUID module_id = env.kernel.get_next_uid();
        env.kernel.loaded_modules[module_id] = module;
        checked.caller = code + 0xf10;
        REQUIRE(create(checked) == SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        checked.attr = 0x4000;
        REQUIRE(create(checked) == SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        checked.attr = 0x02000000 | 0x8000 | SCE_KERNEL_ATTR_TH_PRIO;
        const SceUID system_thread = create(checked);
        REQUIRE(system_thread > 0);
        REQUIRE(env.kernel.get_thread(system_thread)->attr == (SCE_KERNEL_THREAD_ATTR_USER | 0x02000000 | 0x8000 | SCE_KERNEL_ATTR_TH_PRIO));
        env.kernel.get_thread(system_thread)->exit_delete(false);
        env.kernel.loaded_modules.erase(module_id);
    }
    // GetThreadExitStatus (syscall 0x8102ea4c, core 0x81004638): an unknown
    // thread, then 0 or the caller itself, then a null status pointer; a
    // thread that never started has no exit status yet (DORMANT).
    SceInt32 exit_status = 0x12345678;
    const auto exit_status_of = [&](SceUID thid, SceInt32 *status) {
        return export__sceKernelGetThreadExitStatus(env, caller->id, "fixture", thid, status);
    };
    REQUIRE(exit_status_of(0x7ffffff0, nullptr) == SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    REQUIRE(exit_status_of(0, &exit_status) == SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID);
    REQUIRE(exit_status_of(caller->id, &exit_status) == SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID);
    REQUIRE(exit_status_of(target_id, nullptr) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    REQUIRE(export_sceKernelGetThreadExitStatus(env, caller->id, "fixture", target_id, &exit_status) == SCE_KERNEL_ERROR_DORMANT);
    REQUIRE(exit_status == 0x12345678);
    REQUIRE(export_sceKernelChangeThreadPriority(env, caller->id, "fixture", target_id, 0x60) == 0);
    reset(sizeof(*info));
    REQUIRE(get_info(caller->id, target_id, sizeof(*info)) == 0);
    REQUIRE(info->size == 0x80 && info->processId == KernelState::process_id && std::strcmp(info->name, "info target") == 0);
    REQUIRE(info->attr == (SCE_KERNEL_THREAD_ATTR_USER | SCE_KERNEL_ATTR_TH_PRIO) && info->status == SCE_THREAD_DORMANT);
    REQUIRE(info->entry.address() == entry && info->stack.address() == target->stack.get() && info->stackSize == 0x4000);
    REQUIRE(info->initPriority == 0x50 && info->currentPriority == 0x60);
    REQUIRE(info->initCpuAffinityMask == 0x20000 && info->currentCpuAffinityMask == 0x20000);
    REQUIRE(info->exitStatus == SCE_KERNEL_ERROR_DORMANT && untracked_zero() && untouched_from(sizeof(*info)));
    // A short record gets its prefix only.
    reset(0x10);
    REQUIRE(get_info(caller->id, target_id, 0x10) == 0);
    REQUIRE(info->size == 0x80 && info->processId == KernelState::process_id && std::memcmp(info->name, "info tar", 8) == 0);
    REQUIRE(untouched_from(0x10));
    // The record's own size limits it even when the syscall moves more.
    reset(0x10);
    REQUIRE(get_info(caller->id, target_id, sizeof(*info)) == 0);
    REQUIRE(info->size == 0x80 && untouched_from(0x10));
    // Errors in firmware order; a failed call writes the caller's bytes back.
    REQUIRE(export__sceKernelGetThreadInfo(env, caller->id, "fixture", target_id, info_ptr, nullptr) == SCE_KERNEL_ERROR_INVALID_MEMORY_ACCESS);
    reset(sizeof(*info));
    REQUIRE(get_info(caller->id, 0x7ffffff0, sizeof(*info) + 1) == SCE_KERNEL_ERROR_NO_MEMORY);
    REQUIRE(untouched_from(4));
    REQUIRE(get_info(caller->id, 0x7ffffff0, sizeof(*info)) == SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    reset(sizeof(*info) + 1);
    REQUIRE(get_info(caller->id, target_id, sizeof(*info)) == SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    REQUIRE(info->size == sizeof(*info) + 1 && untouched_from(4));
    SceSize size = sizeof(*info);
    REQUIRE(export__sceKernelGetThreadInfo(env, caller->id, "fixture", 0x7ffffff0, Ptr<SceKernelThreadInfo>(), &size) == SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    REQUIRE(export__sceKernelGetThreadInfo(env, caller->id, "fixture", target_id, Ptr<SceKernelThreadInfo>(), &size) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    reset(sizeof(*info));
    REQUIRE(get_info(0, 0, sizeof(*info)) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    // The exit status: DORMANT before the first start, the returned value after.
    REQUIRE(target->start(0, Ptr<void>{}, false) == 0);
    run_until([&] { return target->status == ThreadStatus::dormant; });
    reset(sizeof(*info));
    REQUIRE(get_info(caller->id, target_id, sizeof(*info)) == 0);
    REQUIRE(info->status == SCE_THREAD_DORMANT && info->exitStatus == target_id && word(0) == uint32_t(target_id));
    REQUIRE(exit_status_of(target_id, &exit_status) == 0 && exit_status == target_id);
    // ksceKernelGetThreadInfo is the record fill itself (0x810059cc).
    reset(sizeof(*info));
    REQUIRE(export_ksceKernelGetThreadInfo(env, caller->id, "fixture", target_id, info) == 0);
    REQUIRE(info->size == 0x80 && std::strcmp(info->name, "info target") == 0 && info->exitStatus == target_id && untouched_from(sizeof(*info)));
    reset(0x10);
    REQUIRE(export_ksceKernelGetThreadInfo(env, caller->id, "fixture", 0, info) == 0);
    REQUIRE(std::memcmp(info->name, "info cal", 8) == 0 && untouched_from(0x10));
    reset(sizeof(*info) + 1);
    REQUIRE(export_ksceKernelGetThreadInfo(env, caller->id, "fixture", target_id, info) == SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    REQUIRE(export_ksceKernelGetThreadInfo(env, caller->id, "fixture", target_id, nullptr) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    REQUIRE(export_ksceKernelGetThreadInfo(env, 0, "fixture", 0, info) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);

    // A running guest asks about itself through SceLibKernel (thid 0).
    const Address self_info = data + 0x300;
    Ptr<SceKernelThreadInfo>(self_info).get(env.mem)->size = sizeof(SceKernelThreadInfo);
    guest_sync_delete::build_call(env.mem, code + 0x600, kGetThreadInfo, { 0, self_info, 0, 0, 0 }, data + 4);
    auto self = env.kernel.create_thread(env.mem, "info self", Ptr<const void>(code + 0x600), SCE_KERNEL_DEFAULT_PRIORITY_USER,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    REQUIRE(self && self->start(0, Ptr<void>{}, false) == 0);
    run_until([&] { return self->status == ThreadStatus::dormant; });
    const auto *mine = Ptr<SceKernelThreadInfo>(self_info).get(env.mem);
    REQUIRE(word(4) == 0 && std::strcmp(mine->name, "info self") == 0);
    REQUIRE(mine->status == SCE_THREAD_RUNNING && mine->exitStatus == SCE_KERNEL_ERROR_NOT_DORMANT);

    // Waiting, then woken but not yet running.
    guest_sync_delete::build_call(env.mem, code + 0x800, kWaitSema, { uint32_t(sema), 1, 0, 0, 0 }, data + 8);
    auto waiter = env.kernel.create_thread(env.mem, "info waiter", Ptr<const void>(code + 0x800), SCE_KERNEL_DEFAULT_PRIORITY_USER,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    REQUIRE(waiter && waiter->start(0, Ptr<void>{}, false) == 0);
    run_until([&] { return waiter->status == ThreadStatus::wait; });
    reset(sizeof(*info));
    REQUIRE(get_info(caller->id, waiter->id, sizeof(*info)) == 0);
    REQUIRE(info->status == SCE_THREAD_WAITING && info->exitStatus == SCE_KERNEL_ERROR_NOT_DORMANT);
    REQUIRE(exit_status_of(waiter->id, &exit_status) == SCE_KERNEL_ERROR_NOT_DORMANT);
    REQUIRE(semaphore_signal(env.kernel, "fixture", 0, sema, 1) == 0);
    REQUIRE(get_info(caller->id, waiter->id, sizeof(*info)) == 0);
    REQUIRE(info->status == SCE_THREAD_READY);
    // Desktop runs every runnable thread on its own host thread.
    auto *const host = env.kernel.execution_host;
    env.kernel.execution_host = nullptr;
    REQUIRE(get_info(caller->id, waiter->id, sizeof(*info)) == 0);
    env.kernel.execution_host = host;
    REQUIRE(info->status == SCE_THREAD_RUNNING);
    run_until([&] { return waiter->status == ThreadStatus::dormant; });
    REQUIRE(word(8) == 0);
    std::puts("GetThreadInfo and GetProcessId passed");

    // ksceKernelSetPermission (0x81008940) swaps the calling thread's value.
    REQUIRE(export_ksceKernelSetPermission(env, caller->id, "fixture", 0x80) == 0);
    REQUIRE(export_ksceKernelSetPermission(env, caller->id, "fixture", -1) == SCE_KERNEL_ERROR_INVALID_ARGUMENT);
    REQUIRE(export_ksceKernelSetPermission(env, caller->id, "fixture", 0x10) == 0x80);
    REQUIRE(export_ksceKernelSetPermission(env, target_id, "fixture", 0x20) == 0);
    REQUIRE(export_ksceKernelSetPermission(env, caller->id, "fixture", 0) == 0x10);
    REQUIRE(export_ksceKernelSetPermission(env, 0, "fixture", 0x80) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    // SceThreadmgrForDriver_20C228E4 (0x810083f4): whether the calling thread
    // runs its callbacks; SceQafMgrForDriver_B9770A13 (0x8101f064): a QA flag.
    REQUIRE(export_SceThreadmgrForDriver_20C228E4(env, caller->id, "fixture") == 0);
    caller->is_processing_callbacks = true;
    REQUIRE(export_SceThreadmgrForDriver_20C228E4(env, caller->id, "fixture") == 1);
    REQUIRE(export_SceThreadmgrForDriver_20C228E4(env, 0, "fixture") == 0);
    caller->is_processing_callbacks = false;
    REQUIRE(export_SceQafMgrForDriver_B9770A13(env, caller->id, "fixture") == 0);
    std::puts("Driver thread permission, callback state and QA flag passed");

    // USSE mapping (libgxm 0x8100df34/0x8100e128, unmap 0x8100e084/0x8100e27c;
    // the internal heap's 0x8100e320/0x8100e518 and 0x8100e474/0x8100e66c
    // check the same).
    using UsseMap = int (*)(EmuEnvState &, SceUID, const char *, Ptr<void>, uint32_t, uint32_t *);
    using UsseUnmap = int (*)(EmuEnvState &, SceUID, const char *, void *);
    const std::pair<UsseMap, UsseUnmap> usse[] = {
        { export_sceGxmMapVertexUsseMemory, export_sceGxmUnmapVertexUsseMemory },
        { export_sceGxmMapFragmentUsseMemory, export_sceGxmUnmapFragmentUsseMemory },
        { export_sceGxmMapVertexUsseMemoryInternal, export_sceGxmUnmapVertexUsseMemoryInternal },
        { export_sceGxmMapFragmentUsseMemoryInternal, export_sceGxmUnmapFragmentUsseMemoryInternal },
    };
    const bool initialized = env.gxm.initialized;
    env.gxm.initialized = false;
    uint32_t offset = 0xcccccccc;
    const Ptr<void> base(data + 0x800);
    void *const host_base = base.get(env.mem);
    for (const auto &[map, unmap] : usse) {
        REQUIRE(map(env, caller->id, "fixture", base, 0x1000, &offset) == SCE_GXM_ERROR_UNINITIALIZED);
        REQUIRE(unmap(env, caller->id, "fixture", host_base) == SCE_GXM_ERROR_UNINITIALIZED);
    }
    REQUIRE(export_sceGxmTerminate(env, caller->id, "fixture") == SCE_GXM_ERROR_UNINITIALIZED);
    env.gxm.initialized = true;
    REQUIRE(export_sceGxmInitialize(env, caller->id, "fixture", nullptr) == SCE_GXM_ERROR_ALREADY_INITIALIZED);
    const Address param = data + 0xa00;
    auto *process = Ptr<SceProcessParam>(param).get(env.mem);
    std::memset(process, 0, sizeof(*process));
    process->magic = '2PSP';
    process->version = 1;
    process->fw_version = 0x03100000;
    for (const auto &[map, unmap] : usse) {
        REQUIRE(map(env, caller->id, "fixture", Ptr<void>(), 0x1000, &offset) == SCE_GXM_ERROR_INVALID_POINTER);
        REQUIRE(map(env, caller->id, "fixture", base, 0x1000, nullptr) == SCE_GXM_ERROR_INVALID_POINTER);
        // Only titles built for SDK 3.10 or later are limited to 8 MiB.
        REQUIRE(map(env, caller->id, "fixture", base, MiB(8) + 1, &offset) == 0);
        env.kernel.process_param = Ptr<SceProcessParam>(param);
        REQUIRE(map(env, caller->id, "fixture", base, MiB(8) + 1, &offset) == SCE_GXM_ERROR_INVALID_VALUE);
        REQUIRE(map(env, caller->id, "fixture", base, MiB(8), &offset) == 0);
        env.kernel.process_param = Ptr<SceProcessParam>();
        REQUIRE(unmap(env, caller->id, "fixture", nullptr) == SCE_GXM_ERROR_INVALID_POINTER);
        REQUIRE(unmap(env, caller->id, "fixture", host_base) == 0);
        REQUIRE(offset == base.address());
    }
    // sceGxmGetDisplayQueueThreadIdInternal (0x8100b8c0): the display queue
    // exists only when sceGxmInitialize had a display queue callback.
    const SceUID display_thread = env.gxm.display_queue_thread;
    const Ptr<void> display_callback = env.gxm.params.displayQueueCallback;
    env.gxm.display_queue_thread = caller->id;
    env.gxm.params.displayQueueCallback = Ptr<void>();
    REQUIRE(export_sceGxmGetDisplayQueueThreadIdInternal(env, caller->id, "fixture") == -1);
    env.gxm.params.displayQueueCallback = Ptr<void>(code);
    REQUIRE(export_sceGxmGetDisplayQueueThreadIdInternal(env, caller->id, "fixture") == caller->id);
    env.gxm.initialized = false;
    REQUIRE(export_sceGxmGetDisplayQueueThreadIdInternal(env, caller->id, "fixture") == -1);
    env.gxm.display_queue_thread = display_thread;
    env.gxm.params.displayQueueCallback = display_callback;
    env.gxm.initialized = initialized;
    std::puts("USSE map and unmap validation and the display queue thread passed");

    REQUIRE(semaphore_close(env.kernel, "fixture", 0, sema, HandleClose::Delete) == 0);
    REQUIRE(runtime.shutdown());
    REQUIRE(env.kernel.threads.empty());
    REQUIRE(get_current_cpu_state() == nullptr);
    env.kernel.call_import = {};
}
