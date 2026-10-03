// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <cpu/common.h>
#include <cpu/inline_mutex.h>
#include <kernel/callback.h>
#include <kernel/debugger.h>
#include <kernel/object_store.h>
#include <kernel/sync_primitives.h>
#include <kernel/types.h>
#include <mem/allocator.h>
#include <mem/block.h>
#include <mem/ptr.h>
#include <mem/util.h>
#include <rtc/rtc.h>
#include <util/containers.h>
#include <util/types.h>

#include <emuenv/app_launch_request.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

struct ThreadState;
struct MemState;

struct CodecEngineBlock;

struct KernelModule {
    SceKernelModuleInfo info{};
    Ptr<const uint8_t> info_segment_address;
    uint32_t info_offset = 0;
    // Addresses the module's own relocations wrote as absolute values: the
    // pointers its code and data hold (see relocate()).
    std::vector<Address> absolute_relocations;
    uint64_t program_authority_id = 0; // SELF app info auth id (0 for a plain ELF)
    // Module flag 0x1000 of firmware 3.74 modulemgr: loaded with load flag
    // 0x8000, as process preloads and sysmodule loads are. It decides
    // sceKernelIsCalledFromSysModule and the sceKernelGetModuleList classes.
    bool system_loaded = false;
    // Between a successful module_start and module_stop (see start_module).
    std::atomic<bool> started = false;
};
typedef std::shared_ptr<KernelModule> SceKernelModulePtr;

typedef std::shared_ptr<ThreadState> ThreadStatePtr;
typedef std::map<SceUID, CodecEngineBlock> CodecEngineBlocks;
typedef std::map<SceUID, Ptr<Ptr<void>>> SlotToAddress;
typedef std::map<SceUID, ThreadStatePtr> ThreadStatePtrs;
typedef std::map<SceUID, SceKernelModulePtr> SceKernelModuleInfoPtrs;
typedef std::map<SceUID, CallbackPtr> CallbackPtrs;
typedef unordered_map_fast<uint32_t, Address> ExportNids;
// A NID hashes the function name alone, so same-named exports from different libraries collide.
typedef unordered_map_fast<uint64_t, Address> LibExportNids;
// The plain NID entry outlives taiHEN and HLE redirects, so its owner is tracked apart from its address.
typedef unordered_map_fast<uint32_t, uint32_t> ExportNidOwners;
constexpr uint64_t lib_export_key(uint32_t library_nid, uint32_t nid) {
    return (static_cast<uint64_t>(library_nid) << 32) | nid;
}

typedef std::map<Address, uint32_t> NotFoundVars;
typedef std::function<void(CPUState &cpu, uint32_t nid, SceUID thread_id)> CallImportFunc;

struct CodecEngineBlock {
    uint32_t size;
    int32_t vaddr;
};

using LoadedSysmodules = std::map<SceSysmoduleModuleId, std::vector<SceUID>>;
using LoadedInternalSysmodules = std::vector<SceSysmoduleInternalModuleId>;

struct CorenumAllocator {
    BitmapAllocator alloc;
    std::mutex lock;

    void set_max_core_count(const std::size_t max);

    int new_corenum();
    void free_corenum(const int num);
};

struct VarBindingInfo {
    void *entries;
    uint32_t size;
    uint32_t module_nid;
};

struct FuncBindingInfo {
    Address entry_address;
    uint32_t library_nid;
};

typedef std::multimap<uint32_t, VarBindingInfo> VarBindingInfos;
typedef std::multimap<uint32_t, FuncBindingInfo> FuncBindingInfos;

typedef std::map<uint32_t, uint32_t> ModuleUidByNid;

// Optional single-OS-thread execution host. Notifications/creation NEVER switch
// stacks and may run under kernel/thread locks. run_cpu, checkpoint, park,
// wait_sync and run_guest_function are called with NO kernel locks held.
// The owner must remain alive until all continuations have retired and detach
// before KernelState/MemState destruction. Desktop leaves execution_host null.
struct KernelExecutionHost {
    enum class WaitResult { ready, timeout, cancelled };
    virtual ~KernelExecutionHost() = default;
    virtual CPUStatePtr make_cpu(SceUID id, MemState &mem) = 0;
    virtual bool created(const ThreadStatePtr &thread) = 0;
    virtual void notify(ThreadState &thread, bool deleting = false) noexcept = 0;
    virtual int run_cpu(ThreadState &thread, bool step) = 0; // 0 continue, 1 return, <0 fault
    virtual void checkpoint(ThreadState &thread) = 0;
    virtual void park(ThreadState &thread) = 0;
    // Only the opted-in semaphore, mutex, condition and event-flag queues use
    // this continuation contract.
    virtual WaitResult wait_sync(ThreadState &thread, std::optional<uint32_t> timeout_us) = 0;
    virtual uint32_t run_guest_function(ThreadState &thread, Address entry, SceSize args, Ptr<void> argp) = 0;
    virtual bool stopping() const noexcept = 0;
    virtual void process_exit() = 0;
};

struct KernelState {
    KernelState();

    std::mutex mutex;
    CodecEngineBlocks codec_blocks;

    // sceClibMspace* usage counters, kept in the mspace header by firmware
    // 3.74: bytes in use as dlmalloc counts them (its own state included),
    // their peak, and the count of an empty mspace.
    struct MspaceUsage {
        size_t in_use;
        size_t peak;
        size_t empty;
    };
    std::map<Address, MspaceUsage> mspace_usage;

    // SceLibKernel's PMUSERENR word (sceKernelGetPMUSERENR), which
    // SceProcessmgrForDriver_6599E5D9 writes for the process.
    SceUInt32 pmuserenr = 0;

    Ptr<const void> tls_address = Ptr<const void>(0);
    unsigned int tls_psize = 0;
    unsigned int tls_msize = 0;

    // sceKernelRegisterThreadEventHandler registrations, in registration
    // order (firmware 3.74 appends to one list). thread_event_mutex guards
    // only this list; nothing else is locked while it is held.
    struct ThreadEventHandler {
        SceUID uid;
        SceUID target; // a thread, or SCE_KERNEL_THREAD_ID_USER: every user thread
        SceUInt32 mask; // SCE_KERNEL_THREAD_EVENT_TYPE_START / _END
        Ptr<const void> handler;
        Address common;
    };
    std::mutex thread_event_mutex;
    std::vector<ThreadEventHandler> thread_event_handlers;
    std::vector<ThreadEventHandler> thread_event_handlers_for(SceUID thread_id, SceUInt32 type);

    SimpleEventPtrs simple_events;
    TimerPtrs timers;
    SemaphorePtrs semaphores;
    CondvarPtrs condvars;
    CondvarPtrs lwcondvars;
    MutexPtrs mutexes;
    MutexPtrs lwmutexes; // also Mutexes for now
    RWLockPtrs rwlocks;
    EventFlagPtrs eventflags;
    MsgPipePtrs msgpipes;
    CallbackPtrs callbacks;

    ThreadStatePtrs threads;
    void *jni_env;
    void *jni_activity;

    SceKernelModuleInfoPtrs loaded_modules;
    LoadedSysmodules loaded_sysmodules;
    // Names of the libraries loaded modules import (load_imports), e.g. so a
    // host can preload the system modules that provide them.
    std::set<std::string> imported_libraries;
    LoadedInternalSysmodules loaded_internal_sysmodules;

    // the variables in this block must be accessed by first locking export_nids_mutex
    std::mutex export_nids_mutex;
    ExportNids export_nids;
    LibExportNids export_nids_by_lib;
    ExportNidOwners export_nid_owners;
    FuncBindingInfos func_binding_infos;
    VarBindingInfos var_binding_infos;
    ModuleUidByNid module_uid_by_nid;

    bool cpu_opt;
    CorenumAllocator corenum_allocator;
    CallImportFunc call_import;
    KernelExecutionHost *execution_host = nullptr; // borrowed; opt-in only
    // Host threads without an execution host (desktop paths): the CPU each new
    // thread gets, on its allocated core. Unset, init_cpu picks the build's
    // backend; the threaded browser build sets it to its Wasm JIT.
    std::function<CPUStatePtr(SceUID, std::size_t core, MemState &)> make_cpu;
    // Program authority id (PAID) of the process: its main module's.
    uint64_t process_program_authority_id = 0;
    // ksceSblACMgrIsGameProgram (firmware 3.74 acmgr): a game has PAID class
    // 0x210 (bits 52..63), or is the fake-SELF id 0x2F00000000000001.
    bool process_is_game_program() const {
        return (process_program_authority_id >> 52) == 0x210 || process_program_authority_id == 0x2F00000000000001ULL;
    }

    // HOST linear-memory inline-mutex table (vita3k::wasmjit::InlineMutexTable).
    // Allocated by the cooperative runtime on attach; nullptr on desktop.
    // Generated fast paths mutate entries + guest workareas only during a
    // no-observer phase; run_cpu drains dirty slots into kernel Mutex objects
    // before any HLE/scheduling/thread deletion can observe them.
    std::unique_ptr<vita3k::wasmjit::InlineMutexTable> inline_mutex_table;

    // Optional single-host-thread module entry runner. Desktop leaves this
    // unset and uses create_thread/run_guest_function. The browser supplies
    // a cooperative runner for both preloaded and dynamically loaded modules.
    std::function<uint32_t(const SceKernelModuleInfo &, Ptr<const void>, SceSize, Ptr<const void>)> run_module_entry;

    // Shared NOP+WFI sentinel used by the Dynarmic as the halt return address
    Block halt_instruction;
    Address halt_instruction_pc;

    ObjectStore obj_store;

    uint64_t start_tick;
    SceRtcTick base_tick;
    Ptr<SceProcessParam> process_param;
    // The SDK version the kernel keeps for the process (processmgr), which
    // ThreadMgr's SDK-dependent checks read: the main module's process
    // parameter fw_version whatever its version field, 0 without a parameter.
    // process_param stays null for a version-0 parameter, which SceLibKernel
    // does not use.
    SceUInt32 process_sdk_version = 0;
    // The emulated title runs as one process; its uid is reserved so that no
    // other kernel object can share it.
    static constexpr SceUID process_id = 1;
    static constexpr SceUID first_object_uid = process_id + 1;
    Ptr<void> client_vtable = Ptr<void>(0);
    Ptr<Address> shellsvc_client = Ptr<Address>(0);
    Ptr<void> libc_dso_handle_main = Ptr<void>(0);

    Debugger debugger;

    // kubridge exception handlers (DABT=0, PABT=1, UNDEF=2)
    static constexpr int EXCEPTION_HANDLER_MAX = 3;
    std::atomic<Address> exception_handlers[EXCEPTION_HANDLER_MAX]{};
    std::condition_variable thread_deleted_cond;

    SceUID get_next_uid() {
        return next_uid++;
    }

    bool init(MemState &mem, const CallImportFunc &call_import, bool cpu_opt);
    void deinit(MemState &mem);
    void load_process_param(MemState &mem, Ptr<uint32_t> ptr);
    // SceLibKernel's sceKernelGetMainModuleSdkVersion: 0 without a process
    // parameter or with a version-0 one.
    SceUInt32 main_module_sdk_version(MemState &mem) const;
    ThreadStatePtr create_thread(MemState &mem, const char *name, Ptr<const void> entry_point = Ptr<const void>(0));
    ThreadStatePtr create_thread(MemState &mem, const char *name, Ptr<const void> entry_point, int init_priority, SceInt32 affinity_mask, int stack_size, const SceKernelThreadOptParam *option);

    ThreadStatePtr get_thread(SceUID thread_id);
    Ptr<Ptr<void>> get_thread_tls_addr(MemState &mem, SceUID thread_id, int key);

    bool is_threads_paused() { return !paused_threads_status.empty(); }
    void pause_threads();
    void resume_threads();

    // Kill all guest threads and block until they have exited. Must only be called from a host thread.
    void process_exit();
    // Wakes the threads waiting on timers to see that they were deleted.
    // Called with the kernel mutex held.
    void wake_timer_waiters();
    std::function<void(int, std::optional<AppLaunchRequest>)> process_exit_callback;
    // Request process exit. Safe to call from a guest thread. Returns immediately.
    // The registered process_exit_callback is invoked to notify the host layer.
    void request_process_exit(int res, std::optional<AppLaunchRequest> relaunch = std::nullopt);

    void set_memory_watch(bool enabled);
    void invalidate_jit_cache(Address start, size_t length);
    SceKernelModuleInfo *find_module_by_addr(Address address);

private:
    std::atomic<SceUID> next_uid{ first_object_uid };
    std::map<SceUID, ThreadStatus> paused_threads_status;
};
