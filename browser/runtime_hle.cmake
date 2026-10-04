# Real VitaSDK/newlib startup HLE. Include after runtime_core.cmake.
# This deliberately does not claim all imports linked into newlib (notably
# networking and general filesystem operations). Unselected NIDs remain unsupported.
include_guard(GLOBAL)
set(_HLE_BROWSER_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(_HLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../vita3k")
set(_HLE_EXT "${CMAKE_CURRENT_LIST_DIR}/../external")
include(${CMAKE_CURRENT_LIST_DIR}/runtime_codec.cmake)

set(_hle_exports
    sceGxmInitialize sceGxmTerminate sceGxmCreateContext sceGxmDestroyContext sceGxmFinish
    sceGxmTransferFill sceGxmTransferCopy sceGxmTransferDownscale sceGxmTransferFinish
    # Real GXP indexed-draw guest probe (production validation/state handling).
    sceGxmShaderPatcherCreate sceGxmShaderPatcherRegisterProgram
    sceGxmProgramFindParameterByName sceGxmProgramParameterGetResourceIndex
    sceGxmShaderPatcherCreateVertexProgram sceGxmShaderPatcherCreateFragmentProgram
    sceGxmCreateRenderTarget sceGxmDestroyRenderTarget sceGxmColorSurfaceInit
    sceGxmBeginScene sceGxmEndScene sceGxmSetVertexProgram sceGxmSetFragmentProgram
    sceGxmSetVertexStream sceGxmSetVertexDefaultUniformBuffer sceGxmDraw
    # Bounded fragment unit-zero LINEAR ABGR8 path. Keep production descriptor
    # validation/setters; unsupported layouts remain rejected at draw time.
    sceGxmTextureInitLinear sceGxmSetFragmentTexture sceGxmTextureSetData
    sceGxmTextureSetMinFilter sceGxmTextureSetMagFilter
    sceGxmTextureSetUAddrMode sceGxmTextureSetVAddrMode
    # Notification buffer for guest side channel checks.
    sceGxmGetNotificationRegion
    sceKernelExitProcess
    sceKernelAllocMemBlock sceKernelFreeMemBlock sceKernelGetMemBlockBase
    # Balatro save-manager frontier (exit -8, NID e2d7e137): production
    # SceSysmem body (page-aligned alloc + UID bookkeeping under a short
    # mutex, same TU as the selected memblock entries); no host wait.
    sceKernelAllocMemBlockForVM
    # Balatro unserviced-import sweep (AOT build scan, 66 NIDs without an HLE
    # body): everything below is either a production body that is host-free
    # in the fiber runtime, or an upstream stub whose return is the truth on
    # this host (same precedent as the selected Hid enumerates). Deliberately
    # NOT selected, with reasons: msgpipe send/recv (host-condvar waits, no
    # execution_host branch — would hand fibers ILLEGAL_CONTEXT or hang),
    # sceKernelWaitThreadEndCB (host wait_thread_end, needs a DelayThreadCB-
    # style cooperative mirror), AudioIn open/input/release (needs host mic
    # capture; SDL AUDIO is off), Motion ×7 (motion.cpp needs SDL_OpenSensor,
    # absent from the browser SDL build), sceGxmDisplayQueueFinish (host
    # wait_empty), sceIoChstatByFd/sceGxmSetYuvProfile/sceImeSetText/
    # sceSblDmac5HashTransform (bare stubs with no truth argument; crypto
    # fake-success is dangerous), taiGetModuleInfo (TU defines LIBRARY_INIT),
    # ksceSblACMgr* (no upstream EXPORT exists), egl×20 + PVRSRV×2 (covered
    # by the ?gles=1 bridge, verified in gles_functions.inc), Power ×4
    # (SDL_GetPowerInfo absent from the browser SDL build).
    # VM domains (vitasdk: open/close take no UID; they make all VM-domain
    # memblocks executable/non-executable globally, and Sync flushes caches
    # for a location). The emulator has no NX enforcement on guest RAM, so
    # the upstream open/close stubs (return 0) are already the full
    # executable-toggle semantics here; Sync keeps its production range
    # check + jit invalidate over vm_blocks.
    sceKernelSyncVMDomain sceKernelOpenVMDomain sceKernelCloseVMDomain
    # Pure SysmemState struct fill (SFO-adjusted user budget).
    sceKernelGetFreeMemorySize
    # Forwards to the already-selected _sceKernelStopUnloadModule; the
    # Modulemgr TU is compiled (same machinery as selected LoadStartModule).
    sceKernelStopUnloadModule
    # Non-blocking close; harmless with no pipes, required once creation is.
    sceKernelDeleteMsgPipe
    # Stub returns 0: correct for a game process (cf. selected IsGameProgram).
    sceAppMgrIsNonGameProgram
    # Cosmetic output stubs: 0 with no device is the honest answer.
    sceCtrlSetLightBar sceHidKeyboardRead
    # Production truth on this host (NOT_SUPPORTED off PSTV; rtl rumble loop
    # is a no-op with no controllers).
    sceCtrlSetActuator
    # Pure display-state read; underscore twin selected alongside (same
    # pattern as the SetFrameBuf pair).
    sceDisplayGetFrameBuf _sceDisplayGetFrameBuf
    # GXM descriptor readers and context-state writers (SetCullMode
    # precedent: record + renderer append under alloc_space).
    sceGxmTextureGetData sceGxmTextureGetFormat sceGxmSetUniformDataF
    sceGxmReserveFragmentDefaultUniformBuffer sceGxmReserveVertexDefaultUniformBuffer
    sceGxmSetFrontStencilRef sceGxmSetFrontStencilFunc
    # Production body; same guest-callback pattern as selected sceImeUpdate.
    sceImeSetCaret
    sceKernelCreateLwMutex sceKernelDeleteLwMutex sceKernelLockLwMutex
    sceKernelTryLockLwMutex sceKernelUnlockLwMutex
    sceKernelCreateMutex sceKernelDeleteMutex sceKernelLockMutex sceKernelUnlockMutex
    sceKernelGetTLSAddr sceKernelGetThreadTLSAddr sceKernelGetThreadId
    sceKernelGetThreadInfo sceKernelGetProcessId
    sceKernelExitThread sceKernelExitDeleteThread
    # Nonblocking clocks: use the production implementations and AAPCS bridges
    # (the process-time wrappers live in SceLibKernel, not SceProcessmgr).
    sceKernelGetProcessTime sceKernelGetProcessTimeLow sceKernelGetProcessTimeWide
    sceKernelGetSystemTimeWide sceKernelLibcClock sceKernelLibcTime
    sceKernelLibcGettimeofday sceKernelGetProcessParam
    # Limbo gameplay: resets the power-save timer; the production body only
    # returns SCE_KERNEL_OK.
    sceKernelPowerTick
    # Balatro boot frontier (imports=23 missing_nids=1 PC=81469104): the
    # title's first import is the RNG; the production SceLibRng body
    # (random_device-seeded mt19937_64, capped at 64 bytes) fills guest
    # memory directly and needs no host device.
    sceKernelGetRandomNumber
    sceKernelGetMainModuleSdkVersion
    sceKernelGetThreadCurrentPriority sceKernelGetThreadExitStatus
    sceKernelGetThreadCpuAffinityMask sceKernelGetThreadCpuAffinityMask2
    sceKernelGetSemaInfo sceKernelTryLockMutex
    sceKernelGetStdin sceKernelGetStdout sceKernelGetStderr
    ksceKernelCreateProcessLocalStorage ksceKernelRegisterProcEventHandler
    ksceKernelGetProcessLocalStorageAddr ksceKernelGetProcessLocalStorageAddrForPid
    ksceKernelCreateMutex ksceKernelDeleteMutex ksceKernelLockMutex ksceKernelUnlockMutex
    sceClibMemcpy sceClibMemset
    # Production guest-backed heaps.
    sceClibMspaceCreate sceClibMspaceDestroy sceClibMspaceMalloc
    sceClibMspaceCalloc sceClibMspaceRealloc sceClibMspaceMemalign sceClibMspaceFree
    # Usage counters kept beside dlmalloc (KernelState::mspace_usage).
    sceClibMspaceMallocStats sceClibMspaceIsHeapEmpty
    # Firmware 3.74 system software version; the low word of the system clock.
    sceKernelGetSystemSwVersion ksceKernelGetSystemTimeLow
    # Module classes (KernelModule::system_loaded): libc's exit() runs the
    # module_exit of class 1, libhttp asks whether its caller is a system
    # module.
    sceKernelCallModuleExit sceKernelIsCalledFromSysModule
    sceKernelGetModuleList sceKernelGetModuleInfo
    # Production kernel memset/memcpy, required by observed firmware imports.
    kmemset kmemcpy
    sceIoOpen sceIoClose
    # Synchronous file metadata and reads/seeks reuse production IO implementations.
    sceIoGetstat sceIoGetstatByFd sceIoRead sceIoLseek sceIoLseek32
    _sceDisplaySetFrameBuf sceDisplaySetFrameBuf sceDisplayWaitVblankStart
    # Frame pacing frontier (retail Limbo, 180s headless run: 12 frames, 522,205ms
    # of HLE thread-time, 0.07fps). The game calls sceDisplayWaitSetFrameBuf and
    # sceDisplayWaitSetFrameBufMulti to pace itself; with only WaitVblankStart
    # bridged they resolved to no import at all, the guest saw a failed display
    # wait, and it quit (process_exit(0), missing_nids=1, NID 7d9864A8). All of
    # these share display_wait -> wait_vblank, which registers the thread in
    # display.vblank_wait_infos and drives the emulated vblank clock in the
    # browser build, so they return instead of blocking a host thread. The CB
    # variants stay unselected on purpose: they run host callbacks, which the
    # single-threaded fiber runtime cannot do.
    sceDisplayWaitSetFrameBuf sceDisplayWaitSetFrameBufMulti
    sceDisplayWaitVblankStartMulti
    sceDisplayGetVcount sceDisplayGetRefreshRate
    sceKernelCreateLwCond
    sceFiosOverlayGetList02
    sceKernelGetModuleInfoByAddr
    # AppUtil lifecycle and queries with firmware 3.74's initialized-flag and
    # argument checks (sceAppUtilInit writes the boot parameter).
    sceAppUtilInit sceAppUtilShutdown
    # Parameter queries read EmuEnvState config/licence fields only; no new stubs.
    sceAppUtilAppParamGetInt sceAppUtilSystemParamGetInt sceAppUtilSystemParamGetString
    sceAppUtilDrmOpen sceAppUtilDrmClose
    # Common-dialog configuration and update with firmware 3.74's checks
    # (the host draws the dialog: desktop overlay, browser page); IsRunning and
    # GetWorkerThreadId are upstream UNIMPLEMENTED()/STUBBED() bodies.
    sceCommonDialogSetConfigParam sceCommonDialogUpdate sceCommonDialogIsRunning sceCommonDialogGetWorkerThreadId
    # CALL_EXPORT dependency of sceCommonDialogUpdate: the immediate context's
    # scene state.
    sceGxmRenderingContextIsWithinSceneInternal
    # libc's abort path: a game's calling thread never returns and the
    # process ends (firmware 3.74 processmgr).
    sceKernelCallAbortHandler
    sceIoWrite
    sceKernelUnlockLwMutex2
    # Thread event handlers: a UID per registration, firmware 3.74's target and
    # event checks; libc unregisters its exit handler at teardown.
    sceKernelRegisterThreadEventHandler sceKernelUnregisterThreadEventHandler
    # SceSysmodule imports: whether the calling thread runs its callbacks, a
    # QA flag (clear on retail) and the per-thread permission swap.
    SceThreadmgrForDriver_20C228E4 SceQafMgrForDriver_B9770A13 ksceKernelSetPermission
    # Remaining SceSysmodule static imports (VITA3K_TRACE_MODULE_IMPORTS),
    # with firmware 3.74 semantics: GetThreadId is the caller,
    # MemcpyKernelToUser copies, the retail DIPSW table is clear,
    # __kstack_chk_fail (bkpt) fails the thread; the module ForPid wrappers
    # and ksceDebugPrintf forward to production bodies.
    ksceKernelGetThreadId
    __kstack_chk_fail
    ksceKernelMemcpyKernelToUser ksceKernelMemcpyUserToKernel
    ksceKernelCheckDipsw
    ksceDebugPrintf
    ksceKernelLoadStartModuleForPid ksceKernelLoadStartSharedModuleForPid
    ksceKernelStopUnloadModuleForPid ksceKernelStopUnloadSharedModuleForPid
    # CALL_EXPORT dependencies of the ForPid wrappers above must also be
    # registered: retained bodies call into these user-side entry points.
    _sceKernelStopUnloadModule
    # Nonblocking semaphore operations use the production kernel objects.
    # Blocking waits need cooperative scheduling before they can be selected.
    sceKernelCreateSema sceKernelDeleteSema sceKernelPollSema sceKernelSignalSema
    # Lifecycle/semaphore integration under test; requires GuestThreadRuntime.
    sceKernelCreateThread sceKernelStartThread sceKernelDeleteThread
    sceKernelWaitSema sceKernelCancelSema
    # Mutex cancel and LwCond info: production kernel objects, no waits.
    sceKernelCancelMutex sceKernelGetLwCondInfo sceKernelGetLwCondInfoById
    # Network init/term: production bodies are net-state writes only;
    # net_utils gets a loopback Emscripten branch.
    sceNetInit sceNetTerm
    # Offline control initialization (firmware 3.74: a second Init is
    # 0x80412102); no connection.
    sceNetCtlInit sceNetCtlTerm
    # NP state initialization only; no sign-in or remote service is supplied.
    sceNpInit sceNpTerm
    # Signed-out service state with firmware 3.74's checks; one state
    # notification per registered callback.
    sceNpGetServiceState sceNpManagerGetNpId
    sceNpRegisterServiceStateCallback sceNpUnregisterServiceStateCallback
    sceNpCheckCallback
    # Init/Term state of firmware 3.74 (a second Init fails); no store,
    # social, authentication or signaling service is reachable offline.
    sceNpCommerce2Init sceNpCommerce2Term
    sceNpBasicInit sceNpBasicTerm
    sceNpBasicRegisterHandler sceNpBasicUnregisterHandler
    sceNpAuthInit sceNpAuthTerm sceNpSignalingInit sceNpSignalingTerm
    # Production local trophy-state lifecycle (no online trophy service).
    sceNpTrophyInit sceNpTrophyTerm
    # Limbo trophy frontier (imports=30690 missing_nids=1 PC=8126bac0):
    # production contexts over the staged TRP files; handles with firmware
    # 3.74's limits (four live) and checks.
    sceNpTrophyCreateContext sceNpTrophyDestroyContext
    sceNpTrophyCreateHandle sceNpTrophyDestroyHandle sceNpTrophyAbortHandle
    # Limbo trophy-state frontier (imports=40062 missing_nids=1 PC=8126c330):
    # production read of the local TRP-backed context (unlock flags/count).
    sceNpTrophyGetTrophyUnlockState
    # Limbo trophy-setup-dialog frontier (imports=40013 missing_nids=1 PC=8126c330,
    # reached after null-sink audio pacing): production SceCommonDialog bodies
    # (already sourced). Init arms a host-tick deadline, GetStatus completes
    # to FINISHED/OK once it passes, Term closes the dialog; offline-clean.
    sceNpTrophySetupDialogInit sceNpTrophySetupDialogGetStatus sceNpTrophySetupDialogTerm
    sceNpTrophySetupDialogGetResult
    # Limbo event-flag frontier (imports=39403 missing_nids=1 PC=8126c3e0):
    # production kernel eventflag objects; the wait path parks cooperatively
    # on the fiber runtime (sync_primitives execution_host branch, same
    # contract as the semaphore/mutex waits), set/cancel unlink and resume.
    sceKernelCreateEventFlag sceKernelDeleteEventFlag
    sceKernelSetEventFlag sceKernelClearEventFlag
    sceKernelWaitEventFlag sceKernelPollEventFlag sceKernelCancelEventFlag
    # Limbo AK::IOThread teardown: closes an event-flag handle; the last
    # handle destroys the flag and wakes its waiters with WAIT_DELETE.
    sceKernelCloseEventFlag
    # Limbo eventflag-info frontier (imports=40059 missing_nids=1 PC=8126c330):
    # production struct fill in the already-sourced SceThreadmgr; both the
    # NID name and the underscore EXPORT name (display precedent).
    sceKernelGetEventFlagInfo _sceKernelGetEventFlagInfo
    # Limbo audio frontier (imports=39506 missing_nids=1 PC=8126bbd0):
    # production SceAudio port registry against the browser null sink
    # (hle_audio_null.cpp); output drains instantly, volumes tracked.
    sceAudioOutOpenPort sceAudioOutReleasePort sceAudioOutOutput
    sceAudioOutGetRestSample sceAudioOutSetVolume sceAudioOutSetConfig
    # Limbo dialog/app frontiers (imports=39895 missing_nids=2): both are
    # trivial production bodies (STATUS_NONE with no active dialog; constant
    # 0 game-program query). SceCommonDialog.cpp is already sourced.
    sceNetCheckDialogGetStatus sceAppMgrIsGameProgram
    # Message dialog: production SceCommonDialog bodies; button and system
    # message text comes from the lang catalog (default English). No overlay
    # renders it here, so only the guest's Close/Abort finishes a dialog.
    sceMsgDialogInit sceMsgDialogGetStatus sceMsgDialogGetResult
    sceMsgDialogClose sceMsgDialogAbort sceMsgDialogTerm
    sceMsgDialogProgressBarInc sceMsgDialogProgressBarSetMsg sceMsgDialogProgressBarSetValue
    # Limbo RTC frontier (imports=39419 missing_nids=2 PC=8126bce0):
    # sceRtcGetCurrentTick reads the host clock via base_tick, and
    # sceRtcGetTickResolution is the VITA_CLOCKS_PER_SEC constant.
    sceRtcGetCurrentTick sceRtcGetTickResolution
    # Limbo RTC frontier (imports=40011 missing_nids=1 PC=8126c330):
    # pure SceDateTime->time_t conversion in the already-sourced SceRtcUser.
    sceRtcGetTime64_t
    # Limbo sleep frontier: sceKernelDelayThread parks cooperatively until
    # the deadline (SceThreadmgr execution_host branch, no host sleep).
    sceKernelDelayThread sceKernelDelayThread200
    # Thread join: _sceKernelWaitThreadEnd parks cooperatively until the
    # target goes dormant (SceThreadmgr execution_host branch).
    sceKernelWaitThreadEnd
    # LwCond wait: releases the LwMutex, parks on the condition queue and
    # re-acquires through the cooperative mutex wait (sync_primitives
    # execution_host branch), also after a timeout; deletion returns without it.
    sceKernelWaitLwCond
    # Keep DelayThreadCB, WaitThreadEndCB, WaitEventFlagCB and WaitLwCondCB
    # unselected: their production paths wait on host condition variables
    # (or sleep) and cannot yield guest threads.
    # Narrow Balatro exception (imports=9936 missing_nids=1 PC=814690c4):
    # delay_thread takes the execution_host branch (fiber-safe wait_sync),
    # and delay_thread_cb is that plus a guarded empty callback walk when
    # nothing is registered. Without the bridge the classifier passes but
    # resolution fails, missing_nids fills, and run_app_impl aborts the run.
    sceKernelDelayThreadCB sceKernelDelayThreadCB200
    # GetSystemTime and GetThreadRunStatus are UNIMPLEMENTED upstream. There
    # is no user sceKernelGetSystemTimeLow in nids.inc; do not invent one.
    # Immediate Limbo frontier (imports=3995 missing_nids=1 PC=8126af40):
    # sceCtrlSetSamplingMode is a production body (validates the mode range,
    # stores emuenv.ctrl.input_mode, returns the previous mode), not a stub.
    sceCtrlSetSamplingMode
    # Balatro boot frontier (imports=23 missing_nids=1 PC=81469104): the Ext
    # twin validates the range, stores emuenv.ctrl.input_mode_ext and
    # returns the previous mode. Production body, not a stub.
    sceCtrlSetSamplingModeExt
    # Balatro controller init (imports=8141 missing_nids=1 PC=814688c4):
    # port info reports the physical pad, the sampling-mode getters read
    # emuenv state. All three are host-free production bodies.
    sceCtrlGetControllerPortInfo sceCtrlGetSamplingMode sceCtrlGetSamplingModeExt
    # Next Limbo frontier (imports=4005 missing_nids=1 PC=8126af50):
    # sceTouchGetPanelInfo fills constant panel geometry, sceTouchSetSamplingState
    # stores the per-port mode, and sceTouchPeek takes the non-blocking
    # touch_get peek path (no vblank wait; the blocking sceTouchRead is not
    # imported by the game). All three are production bodies, not stubs.
    sceTouchGetPanelInfo sceTouchSetSamplingState sceTouchPeek
    # Balatro touch init (imports=8296 missing_nids=1 PC=81468a04): enable/
    # disable touch force validate the port and store the mode; no host I/O.
    sceTouchEnableTouchForce sceTouchDisableTouchForce
    # Limbo graphics-memory mapping (imports=4033 missing_nids=1 PC=8126b2d0):
    # sceGxmMap/UnmapMemory record regions in gxm.memory_mapped_regions and
    # return 0 while enable_memory_mapping stays false (the default); the
    # USSE map/unmap pair validate their arguments as libgxm does.
    sceGxmMapMemory sceGxmUnmapMemory
    sceGxmMapFragmentUsseMemory sceGxmUnmapFragmentUsseMemory
    sceGxmMapVertexUsseMemory sceGxmUnmapVertexUsseMemory
    # Remaining Limbo texture descriptor imports (imports=4146 missing_nids=1
    # PC=8126b050): pure descriptor readers/writers, no renderer waits or
    # threads. Unsupported layouts still reject later at draw validation.
    sceGxmTextureSetMipFilter sceGxmTextureSetLodBias sceGxmTextureSetLodMin
    sceGxmTextureGetStride sceGxmTextureGetType
    sceGxmTextureInitLinearStrided sceGxmTextureInitSwizzled sceGxmTextureInitSwizzledArbitrary
    sceGxmTextureInitTiled
    # Fixed-function state setters (imports=4155 missing_nids=1 PC=8126b340):
    # pure context-state writes plus renderer command-queue appends, no waits.
    # The bridge records cull/polygon/depth state and draws proceed only on
    # default state; anything else fails loudly naming the state.
    sceGxmSetCullMode sceGxmSetViewport sceGxmSetViewportEnable
    sceGxmSetFrontDepthFunc sceGxmSetFrontDepthWriteEnable sceGxmSetFrontPolygonMode
    sceGxmSetFragmentDefaultUniformBuffer sceGxmSetFragmentUniformBuffer sceGxmSetVertexUniformBuffer
    # Depth-stencil surface descriptor init (imports=4218 missing_nids=1
    # PC=8126b300): pure descriptor fill, no renderer interaction. Binding a
    # real depth surface still rejects later at scene setup with a named error.
    sceGxmDepthStencilSurfaceInit
    # Sync objects (imports=4228 missing_nids=1 PC=8126b140): guest allocation
    # plus renderer::create/destroy, which only initialize object fields.
    sceGxmSyncObjectCreate sceGxmSyncObjectDestroy
    # Program validation (imports=4429 missing_nids=1 PC=8126b3a0): memcmp of
    # the GXP magic, no state changes.
    sceGxmProgramCheck
    # Patcher program lookup (imports=4433 missing_nids=1 PC=8126b200): pure
    # guest-memory pointer chase, no renderer interaction.
    sceGxmShaderPatcherGetProgramFromId
    # Color-surface format query (imports=4706 missing_nids=1 PC=8126b3e0):
    # pure struct field read.
    sceGxmColorSurfaceGetFormat
    # Power configuration (imports=4711 missing_nids=1 PC=8126bbb0): argument
    # validation returning 0; no host power interaction.
    scePowerSetConfigurationMode
    # Program metadata readers (imports=4434 missing_nids=1 PC=8126b320): pure
    # guest-struct field reads, no state changes or renderer interaction.
    sceGxmProgramGetParameter sceGxmProgramGetParameterCount
    sceGxmProgramFindParameterBySemantic
    sceGxmProgramParameterGetArraySize sceGxmProgramParameterGetCategory
    sceGxmProgramParameterGetComponentCount sceGxmProgramParameterGetContainerIndex
    sceGxmProgramParameterGetName sceGxmProgramParameterGetType
    # Per-frame heartbeat (imports=45276 missing_nids=1 RenderThread PC=8126c330):
    # Limbo calls it right after the first frame's scene is submitted. The
    # upstream body only null-checks its two pointers and returns 0, so
    # selecting it is production parity, not a new stub. A missing NID kills
    # the calling thread, which is what ended the run before any present.
    sceGxmPadHeartbeat
    # Parks the fiber until the (synchronously written) notification matches.
    sceGxmNotificationWait
    # Display queue (imports=45189 missing_nids=1 RenderThread PC=8126c330):
    # the frame presentation entry point. Production body; under
    # VITA3K_BROWSER_GXM it drains the queue inline (guest display callback on
    # the SceGxmDisplayQueue thread, whose sceDisplaySetFrameBuf presents) since
    # the browser execution host has no display host thread. Limbo does not
    # import sceGxmDisplayQueueFinish.
    sceGxmDisplayQueueAddEntry
    # Controller poll (imports=616715 missing_nids=1 PC=8126af30): the 600 s
    # headless run presented 43 frames then stopped here — the game finished
    # loading and polls the gamepad. Production body via ctrl_get: zeroes the
    # buffers, returns no buttons without a host controller (browser has no
    # SDL), and the non-peek path waits for the next vblank, which the
    # presenting display queue advances. No input wired yet (keyboard/gamepad
    # mapping is follow-up); the game will sit at its input screen, correctly.
    sceCtrlReadBufferPositive
    # Balatro input poll (imports=8164 missing_nids=1 PC=814688d4): the peek
    # family takes ctrl_get's non-blocking path (is_peek, no vblank wait),
    # same TU as the already-selected blocking read above.
    sceCtrlPeekBufferPositive sceCtrlPeekBufferPositive2 sceCtrlPeekBufferPositiveExt
    sceCtrlPeekBufferPositiveExt2 sceCtrlPeekBufferNegative sceCtrlPeekBufferNegative2
    # Balatro media init (imports=17640 missing_nids=1 PC=81469034, right after
    # a VideoWorker thread spawns): load-and-start by path via CALL_EXPORT to
    # _sceKernelLoadStartModule. Production body; a missing file fails the
    # call, it does not hang the thread.
    sceKernelLoadStartModule
    # Balatro HID probe (imports=8318 missing_nids=1 PC=81468a44, then 81468a64):
    # keyboard/mouse enumeration on a host with neither. Upstream
    # UNIMPLEMENTED() logs once and returns 0 (no devices), which is the
    # truth here; the mouse read takes the same non-blocking path.
    sceHidKeyboardEnumerate sceHidMouseEnumerate sceHidMouseRead
    # App-state poll (imports=610599 missing_nids=1 PC=8126b640):
    # _sceAppMgrGetAppState forwards (CALL_EXPORT) to __sceAppMgrGetAppState,
    # firmware 3.74's size/version/pointer checks over a state with nothing
    # pending (no system or app event, no system UI overlay).
    _sceAppMgrGetAppState
    # Found statically by the AOT build's unserviced-import scan (vita_app.cpp
    # build_aot_image) before the guest reached them. Production bodies in TUs
    # this graph already compiles; none of them waits on a host primitive.
    # GXM descriptor setters and shader-patcher teardown:
    sceGxmDepthStencilSurfaceSetForceLoadMode sceGxmDepthStencilSurfaceSetForceStoreMode
    sceGxmShaderPatcherReleaseVertexProgram sceGxmShaderPatcherReleaseFragmentProgram
    sceGxmShaderPatcherUnregisterProgram sceGxmShaderPatcherDestroy
    # Thread attributes (no waits):
    sceKernelChangeThreadPriority sceKernelChangeThreadCpuAffinityMask sceKernelChangeThreadVfpException
    # Save data and trophies (host-filesystem backed):
    sceAppUtilSaveDataDataSave sceAppUtilSaveDataDataRemove sceAppUtilSaveDataSlotGetParam
    sceAppUtilReceiveAppEvent
    # App-event text parsers of firmware 3.74 apputil (app_event_parse.h,
    # checked against the firmware code under emulation).
    sceAppUtilAppEventParseNpInviteMessage sceAppUtilAppEventParseNpAppDataMessage
    sceAppUtilAppEventParseNpBasicJoinablePresence sceAppUtilAppEventParseNearGift
    sceAppUtilAppEventParseLiveArea
    # SceNearUtil of firmware 3.74 over an empty near.db (its service answers
    # locally, without PSN): the own gift is kept; the Near app launches only
    # queue their URI with SceAppMgr.
    sceNearInitialize sceNearFinalize sceNearRefresh
    sceNearFinalizeAndLaunchNearApp sceNearLaunchNearAppForUpdate
    sceNearGetNeighbors sceNearGetDiscoveredGifts sceNearGetDiscoveredGiftInfo
    sceNearGetDiscoveredGiftSender sceNearGetDiscoveredGiftStatus
    sceNearConvertDiscoveredGiftParam sceNearDeleteDiscoveredGift
    sceNearOpenDiscoveredGiftImage sceNearReadDiscoveredGiftImage sceNearCloseDiscoveredGiftImage
    sceNearOpenReceivedGiftData sceNearReadReceivedGiftData sceNearCloseReceivedGiftData
    sceNearSetGift sceNearGetGift sceNearDeleteGift
    sceNpTrophyUnlockTrophy
    # Files and directories:
    sceIoDopen sceIoDread sceIoDclose sceIoMkdir sceIoRmdir sceIoRemove sceIoRename
    sceIoPread sceIoPwrite sceIoSync sceIoSyncByFd sceIoChstat
    # Device and file control as iofilemgr, exfatfs and PfsMgr answer a game.
    sceIoDevctl sceIoIoctl
    # Clock:
    sceRtcGetCurrentClock sceRtcGetCurrentClockLocalTime sceRtcGetTick sceRtcTickAddSeconds
    # Remaining static imports of the eboot and the preloaded LLE modules
    # (libc, libfios2, libSceFt2, libpvf, libhttp, libssl) whose production
    # bodies are pure or state-only: no host wait, thread, sleep or device.
    # SceClib string/memory helpers over guest buffers (memmove marks written
    # pages like the selected memcpy/memset; memcpy_safe forwards to memcpy):
    sceClibMemcmp sceClibMemmove sceClibMemcpy_safe sceClibLookCtypeTable
    sceClibStrchr sceClibStrrchr sceClibStrncmp sceClibStrncasecmp sceClibStrnlen
    sceClibStrncpy sceClibStrncat sceClibTolower sceClibToupper
    # BSD strlcpy/strlcat returns; strtoll stores the guest end address and
    # reports EINVAL/ERANGE in the SceLibKernel errno word, TLS slot 0x20.
    sceClibStrlcpy sceClibStrlcat sceClibStrtoll
    # Persona 4 Golden (an LLE module): strcmp/strstr over guest strings.
    sceClibStrcmp sceClibStrstr
    # Guest printf family: module::format_guest reads each argument with its
    # guest ARM EABI width (32-bit long, size_t and pointers; 8-byte aligned
    # long long and double), from registers, stack or a guest va_list; each
    # returns the formatted length.
    sceClibPrintf sceClibVprintf sceClibSnprintf sceClibVsnprintf
    # Log the formatted message; the assertion handler returns its third
    # argument and leaves stopping to the guest's own break instruction.
    sceDbgAssertionHandler sceDbgLoggingHandler
    # Firmware bkpt handlers (division by zero, stack check): the calling
    # thread fails with a diagnostic, as on any guest fault.
    __sce_aeabi_idiv0 __sce_aeabi_ldiv0 __stack_chk_fail
    # LwCond signal/delete: signal unlinks parked waiters and sets them
    # running (notify only); delete wakes waiters with WAIT_DELETE_LW_COND.
    # No host condition variable is reached.
    sceKernelSignalLwCond sceKernelSignalLwCondAll sceKernelSignalLwCondTo sceKernelDeleteLwCond
    # libc time conversions and memblock lookups: host libc time math and
    # SysmemState reads; SceDateTime fill from a time_t is pure.
    sceKernelLibcGmtime_r sceKernelLibcLocaltime_r sceKernelLibcMktime
    sceKernelFindMemBlockByAddr sceKernelOpenMemBlock
    sceRtcSetTime64_t
    # Balatro's settings loader uses the 32-bit Vita newlib time API. These
    # are the production, pointer-checked date/tick conversions (no host wait).
    sceRtcSetTime_t sceRtcGetTime_t sceRtcSetTick
    sceRtcConvertUtcToLocalTime _sceRtcConvertUtcToLocalTime
    # libfios2 overlays: the IOState overlay table with SceFios2Kernel's
    # access rules and per-thread disable flag; the scheduler query is a pure
    # path test.
    sceFiosOverlayAddForProcess02 sceFiosOverlayResolveWithRangeSync02
    sceFiosOverlayGetInfoForProcess02 sceFiosOverlayModifyForProcess02 sceFiosOverlayRemoveForProcess02
    sceFiosOverlayThreadIsDisabled02 sceFiosOverlayThreadSetDisabled02
    sceFiosOverlayGetRecommendedScheduler02
    # Byte-order, TLS errno slot and address formatting: pure, no socket.
    sceNetHtonl sceNetHtons sceNetNtohl sceNetNtohs sceNetErrnoLoc sceNetInetNtop
    # NP ID comparison: pure struct comparison.
    sceNpCmpNpId
    # Registry reads (libhttp asks /CONFIG/NET/ssl_cert_ignorable): production
    # regmgr over the staged firmware template os0/kd/registry.db0, loaded at
    # launch as desktop does (vita_app.cpp).
    sceRegMgrSystemParamGetInt
    # Offline network: a Vita with no connection (net/offline_socket.h). No
    # host socket exists; sockets bind, datagrams to 127/8 are delivered,
    # other destinations have no route, the resolver has no DNS server and
    # SceNetCtl reports disconnected. Receives and epoll waits park the guest
    # thread (SO_RCVTIMEO, abort and delivery end them).
    sceNetSocket sceNetSocketClose sceNetSocketAbort sceNetBind sceNetConnect
    sceNetShutdown sceNetSetsockopt sceNetGetsockopt sceNetGetsockname sceNetGetpeername
    sceNetSend sceNetSendto sceNetRecv sceNetRecvfrom
    sceNetEpollCreate sceNetEpollControl sceNetEpollWait sceNetEpollDestroy sceNetEpollAbort
    # The CB wait runs the thread's notified callbacks before it parks; ICM
    # connect finds no interface to bring up and completes at once.
    sceNetEpollWaitCB sceNetInternalIcmConnect
    # Socket info: the process's sockets and epolls as SceNetPs lists them.
    sceNetGetSockInfo
    sceNetResolverCreate sceNetResolverStartNtoa sceNetResolverGetError
    sceNetResolverAbort sceNetResolverDestroy
    sceNetInetPton sceNetInternalInetPton
    sceNetCtlInetGetState sceNetCtlInetGetInfo
    # On-screen keyboard: production SceIme bodies; the page shows the text
    # field (ime_bridge.cpp) and sceImeUpdate runs the guest's event handler
    # on the calling thread (run_callback), as on desktop.
    sceImeOpen sceImeUpdate sceImeClose
    # NP without a PSN server, as firmware 3.74 answers it (np_basic,
    # np_signaling, np_activity_sdk, np_common, np_manager and the shell's NP
    # service): friend/block lists need sign-in and are never fetched,
    # presence needs the online state, there is no ticket for the content
    # rating, a ticket request is refused without a stored PSN login;
    # signaling contexts are local and a connection is reported dead to its
    # handler on SceNpSignalingMain, then freed.
    sceNpBasicGetFriendListEntryCount sceNpBasicGetFriendListEntries
    sceNpBasicGetBlockListEntryCount sceNpBasicGetBlockListEntries
    sceNpBasicCheckIfPlayerIsBlocked sceNpBasicGetFriendOnlineStatus
    sceNpBasicGetGamePresenceOfFriend
    sceNpSignalingCreateCtx sceNpSignalingDestroyCtx sceNpSignalingSetCtxOpt
    sceNpSignalingTerminateConnection sceNpSignalingGetConnectionInfo
    sceNpActivityPostStatus sceNpGetPlatformType sceNpManagerGetContentRatingFlag
    sceNpAuthCreateStartRequest sceNpAuthGetTicket sceNpSignalingActivateConnection
    # Firmware 3.74 apputil/livearea_util checks; the background-download
    # queue is empty and the browser has no LiveArea (a valid update
    # completes at once).
    sceAppUtilBgdlGetStatus sceLiveAreaUpdateFrameAsync
    # Store browsing: firmware 3.74's checks, then the psts: launch SceAppMgr
    # queues; the Store app itself runs outside the game (closes at once).
    sceAppUtilStoreBrowse
    # SceAppMgr of a game process (firmware 3.74): no system event is ever
    # pending; the vs0 user drives are per-app "sd<hex>:" names the IO layer
    # resolves. RTC RFC 3339 / RFC 1123 formatting and parsing, checked
    # against the firmware code run in an emulator.
    sceAppMgrReceiveSystemEvent sceAppMgrConvertVs0UserDrivePath
    sceAppMgrGetVs0UserDataDrive sceAppMgrGetVs0UserModuleDrive
    sceRtcParseDateTime sceRtcParseRFC3339 sceRtcFormatRFC3339 sceRtcFormatRFC3339LocalTime
    sceRtcCheckValid
    # Kernel imports of the LLE sysmodule.skprx, from firmware 3.74 threadmgr,
    # processmgr and acmgr (retail console: DIPSW and QA flags clear).
    SceThreadmgrForDriver_1AAFA818 SceThreadmgrForDriver_5053B005
    SceProcessmgrForDriver_61B9B6FA SceProcessmgrForDriver_B1C3EFCA
    SceProcessmgrForDriver_D141C076 SceProcessmgrForDriver_6599E5D9 sceKernelGetPMUSERENR
    ksceSblACMgrIsDevelopmentMode ksceSblACMgrIsGameProgram ksceSblACMgrIsPSMDevAssistant
    # Persona 4 Golden (PCSE00120). Sysmodule loads: LLE modules come from the
    # staged firmware (vs0:sys/external) or app0:sce_module, the rest are HLE
    # and only marked loaded.
    sceSysmoduleLoadModule sceSysmoduleUnloadModule sceSysmoduleIsLoaded
    sceSysmoduleLoadModuleInternal sceSysmoduleLoadModuleInternalWithArg sceSysmoduleIsLoadedInternal
    # The movie player (libscemp4 LLE) asks for the title id.
    sceKernelGetProcessTitleId
    # Persona 4 Golden: the remaining static imports of the eboot, libc and
    # libfios2 (AOT build unserviced-import scan) whose bodies need no codec,
    # plus their CALL_EXPORT targets. The CB waits run notified callbacks on
    # the calling fiber (run_guest_function) and then take the cooperative wait.
    sceAppMgrAcquireBgmPort sceAppUtilSaveDataSlotDelete sceAppUtilSaveDataSlotSetParam
    sceAudioOutGetAdopt
    sceCameraClose sceCameraOpen sceCameraRead sceCameraStart sceCameraStop
    _sceFiosKernelOverlayAdd _sceFiosKernelOverlayAddForProcess _sceFiosKernelOverlayDHChstatSync
    _sceFiosKernelOverlayDHCloseSync _sceFiosKernelOverlayDHOpenSync _sceFiosKernelOverlayDHReadSync
    _sceFiosKernelOverlayDHStatSync _sceFiosKernelOverlayDHSyncSync _sceFiosKernelOverlayGetInfo
    _sceFiosKernelOverlayGetInfoForProcess _sceFiosKernelOverlayGetList
    _sceFiosKernelOverlayGetRecommendedScheduler _sceFiosKernelOverlayModify
    _sceFiosKernelOverlayModifyForProcess _sceFiosKernelOverlayRemove
    _sceFiosKernelOverlayRemoveForProcess _sceFiosKernelOverlayResolveSync
    _sceFiosKernelOverlayResolveWithRangeSync _sceFiosKernelOverlayThreadIsDisabled
    _sceFiosKernelOverlayThreadSetDisabled
    sceGxmDrawInstanced sceGxmDrawPrecomputed sceGxmGetPrecomputedDrawSize
    sceGxmGetPrecomputedFragmentStateSize sceGxmGetPrecomputedVertexStateSize
    sceGxmGetRenderTargetMemSize sceGxmPrecomputedDrawInit sceGxmPrecomputedDrawSetAllVertexStreams
    sceGxmPrecomputedDrawSetParams sceGxmPrecomputedDrawSetParamsInstanced
    sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer sceGxmPrecomputedFragmentStateInit
    sceGxmPrecomputedFragmentStateSetAllTextures
    sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer sceGxmPrecomputedFragmentStateSetTexture
    sceGxmPrecomputedVertexStateGetDefaultUniformBuffer sceGxmPrecomputedVertexStateInit
    sceGxmPrecomputedVertexStateSetDefaultUniformBuffer _sceGxmProgramFindParameterBySemantic
    sceGxmProgramGetDefaultUniformBufferSize sceGxmProgramGetSize sceGxmSetBackPolygonMode
    sceGxmSetFrontDepthBias sceGxmSetFrontPointLineWidth sceGxmSetPrecomputedFragmentState
    sceGxmSetPrecomputedVertexState sceGxmSetRegionClip sceGxmTextureGetHeight sceGxmTextureGetWidth
    sceGxmTextureSetFormat sceGxmTextureSetPalette sceGxmTextureSetUAddrModeSafe
    sceGxmTextureSetVAddrModeSafe
    sceImeDialogGetResult sceImeDialogGetStatus sceImeDialogInit sceImeDialogTerm
    ksceKernelGetThreadCurrentPriority sceKernelCloseMutex _sceKernelCreateCond sceKernelCreateCond
    _sceKernelCreateSema sceKernelCreateSema_16XX sceKernelDeleteCond sceKernelGetMemBlockInfoByAddr
    _sceKernelLockLwMutex sceKernelLockLwMutexCB sceKernelSignalCond _sceKernelWaitCond
    sceKernelWaitCond _sceKernelWaitEventFlagCB sceKernelWaitEventFlagCB _sceKernelWaitLwCondCB
    sceKernelWaitLwCondCB _sceKernelWaitSemaCB sceKernelWaitSemaCB _sceKernelWaitThreadEndCB
    sceKernelWaitThreadEndCB
    sceMotionGetBasicOrientation sceMotionGetDeadband sceMotionGetState sceMotionGetTiltCorrection
    sceMotionReset sceMotionRotateYaw sceMotionSetDeadband sceMotionSetTiltCorrection
    sceMotionStartSampling sceMotionStopSampling
    sceNetCheckDialogGetResult sceNetCheckDialogInit sceNetCheckDialogTerm
    sceNpActivityInit sceNpActivityTerm sceNpManagerGetChatRestrictionFlag
    sceNpMatching2AbortContextStart sceNpMatching2ContextStart sceNpMatching2ContextStop
    sceNpMatching2CreateContext sceNpMatching2CreateJoinRoom sceNpMatching2DestroyContext
    sceNpMatching2GetServerLocal sceNpMatching2GetWorldInfoList sceNpMatching2Init
    sceNpMatching2JoinRoom sceNpMatching2RegisterContextCallback
    sceNpMatching2RegisterRoomEventCallback sceNpMatching2RegisterRoomMessageCallback
    sceNpMatching2SearchRoom sceNpMatching2SendRoomMessage sceNpMatching2SetDefaultRequestOptParam
    sceNpMatching2Term sceNpScoreCreateRequest sceNpScoreCreateTitleCtx sceNpScoreDeleteRequest
    sceNpScoreDeleteTitleCtx sceNpScoreGetRankingByRange sceNpScoreGetRankingByRangeAsync
    sceNpScoreInit sceNpScorePollAsync sceNpScoreRecordScore sceNpScoreRecordScoreAsync
    sceNpScoreTerm
    _sceRtcGetCurrentNetworkTick sceRtcGetCurrentNetworkTick sceRtcGetDayOfWeek
    sceSaveDataDialogFinish sceSaveDataDialogGetResult sceSaveDataDialogGetStatus
    sceSaveDataDialogGetSubStatus sceSaveDataDialogInit sceSaveDataDialogTerm
    sceTouchGetSamplingState sceTouchRead
    # Persona 4 Golden audio and video: NGS (ATRAC9 voices, mixing, buss
    # effects), ATRAC9/AAC/MP3 decoders and the AVC decoder behind libscemp4,
    # over vita3k/codec and vita3k/ngs (runtime_codec.cmake).
    sceAudiodecCreateDecoderExternal sceAudiodecDecode sceAudiodecDeleteDecoder
    sceAudiodecDeleteDecoderExternal sceAudiodecGetContextSize
    sceAvcdecCreateDecoder sceAvcdecDecode sceAvcdecDecodeFlush sceAvcdecDecodeStop
    sceAvcdecDeleteDecoder sceAvcdecQueryDecoderMemSize
    sceCodecEngineAllocMemoryFromUnmapMemBlock sceCodecEngineCloseUnmapMemBlock
    sceCodecEngineFreeMemoryFromUnmapMemBlock sceCodecEngineOpenUnmapMemBlock
    sceNgsPatchCreateRouting sceNgsPatchGetInfo sceNgsPatchRemoveRouting
    sceNgsRackGetRequiredMemorySize sceNgsRackGetVoiceHandle sceNgsRackInit sceNgsRackRelease
    sceNgsSystemGetRequiredMemorySize sceNgsSystemInit sceNgsSystemRelease sceNgsSystemUpdate
    sceNgsVoiceBypassModule sceNgsVoiceDefGetAtrac9Voice sceNgsVoiceDefGetMasterBuss
    sceNgsVoiceDefGetReverbBuss sceNgsVoiceDefGetTemplate1 sceNgsVoiceGetInfo
    sceNgsVoiceGetParamsOutOfRange sceNgsVoiceGetStateData sceNgsVoiceKeyOff sceNgsVoiceKill
    sceNgsVoiceLockParams sceNgsVoicePatchSetVolumesMatrix sceNgsVoicePause sceNgsVoicePlay
    sceNgsVoiceResume sceNgsVoiceSetModuleCallback sceNgsVoiceUnlockParams
    sceVideodecInitLibraryWithUnmapMem sceVideodecQueryMemSize sceVideodecTermLibrary
)

# Take NID values from the one authoritative database, never a second resolver.
set(_hle_generated "${CMAKE_CURRENT_BINARY_DIR}/runtime-hle-generated")
file(MAKE_DIRECTORY "${_hle_generated}")
file(STRINGS "${_HLE_ROOT}/nids/include/nids/nids.inc" _hle_nid_lines)
set(_hle_nids "// Generated from Vita3K nids.inc; do not edit.\n")
foreach(_export IN LISTS _hle_exports)
    set(_found FALSE)
    foreach(_line IN LISTS _hle_nid_lines)
        if(_line MATCHES "^NID\\(${_export},")
            string(APPEND _hle_nids "${_line}\n")
            set(_found TRUE)
        endif()
    endforeach()
    if(NOT _found)
        message(FATAL_ERROR "Startup HLE export absent from nids.inc: ${_export}")
    endif()
endforeach()
file(CONFIGURE OUTPUT "${_hle_generated}/startup_nids.inc" CONTENT "${_hle_nids}" @ONLY)
# This is an initializer list, NOT an import-library allowlist. NID resolution
# in module_parent.cpp uses startup_nids.inc alone. List every selected source
# file that defines LIBRARY_INIT (SceSysmem, SceAudiodec): one that is left out
# never creates its state, and its exports then read a null object store entry.
# Adding e.g. LIBRARY(SceLibKernel) would reference a nonexistent
# import_library_init_SceLibKernel symbol.
file(CONFIGURE OUTPUT "${_hle_generated}/startup_libraries.inc" CONTENT "LIBRARY(SceSysmem)\nLIBRARY(SceAudiodec)\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_HLE_ROOT}/nids/include/nids/nids.inc")

set(_hle_module_sources
    "${_HLE_ROOT}/modules/SceGxm/SceGxm.cpp"
    "${_HLE_ROOT}/modules/SceGxm/SceGxmInternal.cpp"
    "${_HLE_ROOT}/modules/SceNearUtil/SceNearUtil.cpp"
    "${_HLE_ROOT}/modules/SceLibKernel/SceLibKernel.cpp"
    # Balatro boot: home of sceKernelGetRandomNumber (see above).
    "${_HLE_ROOT}/modules/SceLibKernel/SceLibRng.cpp"
    "${_HLE_ROOT}/modules/SceLibDbg/SceDbg.cpp"
    "${_HLE_ROOT}/modules/SceKernelThreadMgr/SceThreadmgr.cpp"
    "${_HLE_ROOT}/modules/SceKernelThreadMgr/SceThreadmgrCoredumpTime.cpp"
    "${_HLE_ROOT}/modules/SceKernelThreadMgr/SceThreadmgrForDriver.cpp"
    "${_HLE_ROOT}/modules/SceSysmem/SceSysmem.cpp"
    "${_HLE_ROOT}/modules/SceSysmem/SceSysclibForDriver.cpp"
    "${_HLE_ROOT}/modules/SceSysmem/SceQafMgrForDriver.cpp"
    "${_HLE_ROOT}/modules/SceSysmem/SceDebugForDriver.cpp"
    "${_HLE_ROOT}/modules/SceSysmem/SceDipswForDriver.cpp"
    "${_HLE_ROOT}/modules/SceKernelModulemgr/SceModulemgrForDriver.cpp"
    "${_HLE_ROOT}/modules/SceKernelModulemgr/SceModulemgr.cpp"
    "${_HLE_ROOT}/modules/SceSysmem/SceSysmemForDriver.cpp"
    "${_HLE_ROOT}/modules/SceSysmem/SceProcEventForDriver.cpp"
    "${_HLE_ROOT}/modules/SceProcessmgr/SceProcessmgr.cpp"
    "${_HLE_ROOT}/modules/SceProcessmgr/SceProcessmgrForDriver.cpp"
    "${_HLE_ROOT}/modules/SceIofilemgr/SceIofilemgr.cpp"
    "${_HLE_ROOT}/modules/SceDisplay/SceDisplay.cpp"
    "${_HLE_ROOT}/modules/SceDriverUser/SceDisplayUser.cpp"
    "${_HLE_ROOT}/modules/SceDriverUser/SceFios2User.cpp"
    # Limbo RTC frontier (imports=39419 missing_nids=2 PC=8126bce0):
    # host-clock production bodies (rtc.cpp is chrono-only, no tz data).
    "${_HLE_ROOT}/modules/SceRtc/SceRtc.cpp"
    "${_HLE_ROOT}/modules/SceDriverUser/SceRtcUser.cpp"
    "${_HLE_ROOT}/rtc/src/rtc.cpp"
    "${_HLE_ROOT}/rtc/src/rtc_format.cpp"
    "${_HLE_ROOT}/modules/SceNet/SceNet.cpp"
    "${_HLE_ROOT}/modules/SceNetCtl/SceNetCtl.cpp"
    "${_HLE_ROOT}/modules/SceNetInternal/SceNetInternal.cpp"
    "${_HLE_ROOT}/modules/SceIme/SceIme.cpp"
    "${_HLE_ROOT}/modules/SceNpManager/SceNpManager.cpp"
    "${_HLE_ROOT}/modules/SceNpCommerce2/SceNpCommerce2.cpp"
    "${_HLE_ROOT}/modules/SceNpBasic/SceNpBasic.cpp"
    "${_HLE_ROOT}/modules/SceNpCommon/SceNpCommon.cpp"
    "${_HLE_ROOT}/modules/SceNpSignaling/SceNpSignaling.cpp"
    "${_HLE_ROOT}/modules/SceNpActivity/SceNpActivity.cpp"
    "${_HLE_ROOT}/modules/SceLiveArea/SceLiveAreaUtil.cpp"
    "${_HLE_ROOT}/modules/SceSblACMgr/SceSblACMgrForDriver.cpp"
    "${_HLE_ROOT}/modules/SceNpTrophy/SceNpTrophy.cpp"
    "${_HLE_ROOT}/modules/SceAppUtil/SceAppUtil.cpp"
    "${_HLE_ROOT}/modules/SceCommonDialog/SceCommonDialog.cpp"
    # No LIBRARY_INIT (startup_libraries.inc lists the sources that have one).
    "${_HLE_ROOT}/modules/SceCtrl/SceCtrl.cpp"
    "${_HLE_ROOT}/modules/SceRegistryMgr/SceRegMgrForGame.cpp"
    # ctrl_get implementation behind the selected SceCtrl bridges (chrono +
    # controller-state reads only; no host input device needed).
    "${_HLE_ROOT}/ctrl/src/ctrl.cpp"
    "${_HLE_ROOT}/modules/SceTouch/SceTouch.cpp"
    # Balatro HID probe: SceHid.cpp is all UNIMPLEMENTED() stubs (see above).
    "${_HLE_ROOT}/modules/SceHid/SceHid.cpp"
    "${_HLE_ROOT}/modules/ScePower/ScePower.cpp"
    # Audio port registry (null device sink is hle_audio_null.cpp, linked
    # separately below; this adapter only registers the selected bridges).
    "${_HLE_ROOT}/modules/SceAudio/SceAudio.cpp"
    # Limbo app-manager frontier: sceAppMgrIsGameProgram is a constant-0
    # production body (full TU needed for the adapter scan).
    "${_HLE_ROOT}/modules/SceAppMgr/SceAppMgr.cpp"
    # _sceAppMgrGetAppState forwarder (CALL_EXPORT to __sceAppMgrGetAppState).
    "${_HLE_ROOT}/modules/SceDriverUser/SceAppMgrUser.cpp"
    # sceTouchPeek needs touch_get; touch_get's vblank wait needs wait_vblank.
    # Both TUs are Emscripten-aware (browser cooperative vblank, no host
    # threads) and define no EXPORTs, so they only contribute link symbols.
    "${_HLE_ROOT}/touch/src/touch.cpp"
    "${_HLE_ROOT}/display/src/display.cpp"
    # Persona 4 Golden.
    "${_HLE_ROOT}/modules/SceSysmodule/SceSysmodule.cpp"
    "${_HLE_ROOT}/modules/SceCamera/SceCamera.cpp"
    "${_HLE_ROOT}/modules/SceFios2Kernel/SceFios2Kernel.cpp"
    "${_HLE_ROOT}/modules/SceDriverUser/SceMotion.cpp"
    "${_HLE_ROOT}/modules/SceNpMatching2/SceNpMatching2.cpp"
    "${_HLE_ROOT}/modules/SceNpScore/SceNpScore.cpp"
    "${_HLE_ROOT}/modules/SceNgsUser/SceNgs.cpp"
    "${_HLE_ROOT}/modules/SceAudiodec/SceAudiodecUser.cpp"
    "${_HLE_ROOT}/modules/SceVideodec/SceVideodecUser.cpp"
    "${_HLE_ROOT}/modules/SceCodecEngine/SceCodecEngineUser.cpp"
)
# Compile the existing implementation files through registration-only adapters.
# This is necessary because EXPORT's make_bridge initialization roots even
# unselected bridges. No function body, bridge implementation or call is copied.
set(_hle_bridges "// Generated bridge selection; do not edit.\n")
set(_hle_adapters)
set(_hle_found_exports)
foreach(_source IN LISTS _hle_module_sources)
    file(STRINGS "${_source}" _export_lines REGEX "^EXPORT\\(")
    foreach(_line IN LISTS _export_lines)
        if(NOT _line MATCHES "^EXPORT\\([^,]+, *([A-Za-z0-9_]+)")
            message(FATAL_ERROR "Cannot parse HLE EXPORT: ${_line}")
        endif()
        set(_name "${CMAKE_MATCH_1}")
        if(_name IN_LIST _hle_exports)
            set(_emit VITA3K_HLE_EMIT_BRIDGE)
            list(APPEND _hle_found_exports "${_name}")
        else()
            set(_emit VITA3K_HLE_SKIP_BRIDGE)
        endif()
        string(APPEND _hle_bridges "#define VITA3K_HLE_BRIDGE_${_name} ${_emit}\n")
    endforeach()
    get_filename_component(_stem "${_source}" NAME_WE)
    set(_adapter "${_hle_generated}/${_stem}_selected.cpp")
    file(CONFIGURE OUTPUT "${_adapter}" CONTENT
        "// Registration-only adapter; original implementation follows.\n#include \"${_HLE_BROWSER_ROOT}/src/hle_select_exports.h\"\n#include \"${_source}\"\n" @ONLY)
    list(APPEND _hle_adapters "${_adapter}")
endforeach()
foreach(_name IN LISTS _hle_exports)
    if(NOT _name IN_LIST _hle_found_exports)
        message(FATAL_ERROR "Selected HLE export has no source implementation: ${_name}")
    endif()
endforeach()
file(CONFIGURE OUTPUT "${_hle_generated}/startup_bridge_selection.inc" CONTENT "${_hle_bridges}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_hle_module_sources})

# String catalog behind lang::get (SceCommonDialog button/system texts), built
# by the same generator and inputs as vita3k/lang. That CMakeLists is not
# reused because its util target does not exist in the web graph.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_lang_root "${_HLE_ROOT}/lang")
set(_lang_generated "${CMAKE_CURRENT_BINARY_DIR}/runtime-lang-generated")
set(_lang_generator "${_HLE_ROOT}/../tools/i18n/generate_lang_catalog.py")
set(_lang_json_dir "${_HLE_ROOT}/../i18n/lang")
file(GLOB _lang_json CONFIGURE_DEPENDS "${_lang_json_dir}/overlay_*.json")
add_custom_command(
    OUTPUT "${_lang_generated}/generated_catalog.h" "${_lang_generated}/generated_catalog.cpp"
    COMMAND ${Python3_EXECUTABLE} "${_lang_generator}"
        --input-dir "${_lang_json_dir}"
        --strings-def "${_lang_root}/include/lang/strings.def"
        --output-header "${_lang_generated}/generated_catalog.h"
        --output-source "${_lang_generated}/generated_catalog.cpp"
    DEPENDS "${_lang_generator}" "${_lang_root}/include/lang/strings.def" ${_lang_json}
    VERBATIM)

add_library(vita3k_web_runtime_hle STATIC
    ${VITA3K_WEB_CODEC_SOURCES}
    $<$<BOOL:${VITA3K_WEB_THREADS}>:${_HLE_BROWSER_ROOT}/src/thread_bridge.cpp>
    # decrypt_fself for module_parent.cpp's module loader.
    "${_HLE_BROWSER_ROOT}/src/vita_self_decrypt.cpp"
    "${_HLE_BROWSER_ROOT}/src/gxm_webgpu_bridge.cpp"
    "${_HLE_BROWSER_ROOT}/src/hle_stub_intrinsics.cpp"
    "${_HLE_BROWSER_ROOT}/src/gles_webgl_bridge.cpp"
    "${_HLE_BROWSER_ROOT}/src/msg_dialog_bridge.cpp"
    "${_HLE_BROWSER_ROOT}/src/ime_bridge.cpp"
    "${_lang_root}/src/lang.cpp"
    "${_lang_generated}/generated_catalog.cpp"
    "${_HLE_ROOT}/renderer/src/renderer.cpp"
    "${_HLE_ROOT}/renderer/src/creation.cpp"
    "${_HLE_ROOT}/gxm/src/textures.cpp"
    # Guest texel layout helpers (swizzle/tiled -> linear) for the scene stream.
    "${_HLE_ROOT}/renderer/src/texture/format.cpp"
    # P4/P8 palette expansion for the scene stream.
    "${_HLE_ROOT}/renderer/src/texture/palette.cpp"
    "${_HLE_ROOT}/renderer/src/texture/pvrt-dec.cpp"
    "${_HLE_ROOT}/gxm/src/attributes.cpp"
    "${_HLE_ROOT}/shader/src/usse_program_analyzer.cpp"
    "${_HLE_ROOT}/shader/src/gxp_parser.cpp"
    "${_HLE_ROOT}/gxm/src/gxp.cpp"
    "${_HLE_ROOT}/gxm/src/color.cpp"
    "${_HLE_ROOT}/gxm/src/stream.cpp"
    "${_HLE_ROOT}/gxm/src/transfer.cpp"
    "${_HLE_BROWSER_ROOT}/src/gxm_hash.cpp"
    "${_HLE_EXT}/vita-toolchain/src/utils/sha256.c"
    "${_HLE_ROOT}/modules/module_parent.cpp"
    "${_HLE_ROOT}/module/src/write_return_value.cpp"
    "${_HLE_ROOT}/module/src/guest_format.cpp"
    "${_HLE_ROOT}/module/src/load_module.cpp"
    ${_hle_adapters}
    "${_HLE_ROOT}/kernel/src/sync_primitives.cpp"
    "${_HLE_BROWSER_ROOT}/src/hle_io.cpp"
    # Null audio sink (no device in the web runtime); production SceAudio
    # bodies (a _hle_module_sources adapter) run unchanged against it.
    "${_HLE_BROWSER_ROOT}/src/hle_audio_null.cpp"
    "${_HLE_ROOT}/io/src/device.cpp"
    "${_HLE_ROOT}/io/src/filesystem.cpp"
    "${_HLE_ROOT}/io/src/state_functions.cpp"
    "${_HLE_ROOT}/util/src/net_utils.cpp"
    # Portable f32->f16 for the selected GXM uniform setters: the native TU
    # needs x86 AVX+F16C, so the browser builds the scalar path (bit-identical
    # over the same header-only encoder) from browser/src instead.
    "${_HLE_BROWSER_ROOT}/src/float_to_half_portable.cpp"
    # SFO reader for sceKernelGetFreeMemorySize's memory-expansion query
    # (header-only boost + fmt, both already in this graph).
    "${_HLE_ROOT}/packages/src/sfo.cpp"
    "${_HLE_ROOT}/regmgr/src/regmgr.cpp"
    "${_HLE_ROOT}/net/src/offline_socket.cpp"
    "${_HLE_ROOT}/net/src/epoll.cpp"
    "${_HLE_ROOT}/np/src/init.cpp"
    # Trophy context lifecycle for sceNpTrophyCreateContext and friends:
    # upstream production bodies over the staged TRP files (pugixml parses
    # the trophy config; TRP framing comes from trp_parser, no miniz link).
    "${_HLE_ROOT}/np/src/trophy/context.cpp"
    "${_HLE_ROOT}/np/src/trophy/trp_parser.cpp"
    "${_HLE_EXT}/pugixml/src/pugixml.cpp"
    "${_HLE_ROOT}/emuenv/src/emuenv.cpp"
    "${_HLE_ROOT}/display/src/display.cpp"
    "${_HLE_ROOT}/motion/src/motion_input.cpp"
    # MotionState without SDL sensors (no sensor subsystem in the browser SDL).
    "${_HLE_BROWSER_ROOT}/src/motion_browser.cpp"
    # stb_image for camera.cpp (its desktop provider is a renderer TU).
    "${_HLE_BROWSER_ROOT}/src/stb_image_impl.c"
    "${_HLE_ROOT}/camera/src/camera.cpp"
    "${_HLE_ROOT}/overlay/src/display_manager.cpp"
)
file(GLOB _hle_includes "${_HLE_ROOT}/*/include")
target_include_directories(vita3k_web_runtime_hle PUBLIC ${_hle_includes}
    "${_HLE_EXT}/yaml-cpp/include")
target_include_directories(vita3k_web_runtime_hle PRIVATE
    "${_hle_generated}" "${_lang_generated}" "${_HLE_BROWSER_ROOT}/src" "${_HLE_EXT}/dlmalloc"
    "${_HLE_EXT}/stb" "${_HLE_EXT}/xxHash" "${_HLE_EXT}/vita-toolchain/src"
    "${_HLE_EXT}/pugixml/src")
target_compile_definitions(vita3k_web_runtime_hle PRIVATE
    VITA3K_BROWSER_GXM=1
    ONLY_MSPACES=1
    VITA3K_HLE_NID_LIST="startup_nids.inc"
    VITA3K_HLE_LIBRARY_LIST="startup_libraries.inc")
# Full EmuEnvState construction is used unchanged, including all subsystem
# state objects. Its camera and motion destructors need actual SDL3 symbols;
# building SDL with device backends disabled avoids inventing state types or
# no-op destructors. No desktop window, renderer, audio or camera is started.
if(NOT TARGET SDL3::SDL3-static)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    # The threaded build creates guest host threads through SDL (kernel.cpp).
    set(SDL_PTHREADS ${VITA3K_WEB_THREADS} CACHE BOOL "" FORCE)
    set(SDL_PTHREADS_SEM ${VITA3K_WEB_THREADS} CACHE BOOL "" FORCE)
    foreach(_sub AUDIO VIDEO RENDER CAMERA JOYSTICK HAPTIC SENSOR POWER DIALOG)
        set(SDL_${_sub} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory("${_HLE_EXT}/sdl" "${CMAKE_CURRENT_BINARY_DIR}/runtime-deps/sdl" EXCLUDE_FROM_ALL)
endif()
# Separate from Emscripten's process allocator: emit only mspace symbols.
# The bundled header disables mmap/morecore, so heaps stay in guest backing.
add_library(vita3k_web_mspace STATIC "${_HLE_EXT}/dlmalloc/dlmalloc.cc")
target_include_directories(vita3k_web_mspace PUBLIC "${_HLE_EXT}/dlmalloc")
target_compile_definitions(vita3k_web_mspace PUBLIC ONLY_MSPACES=1)
target_link_libraries(vita3k_web_runtime_hle
    PUBLIC vita3k_web_runtime_core
    PRIVATE SDL3::SDL3-static vita3k_web_mspace vita3k_web_ffmpeg vita3k_web_libatrac9)

if(EMSCRIPTEN)
    target_compile_options(vita3k_web_runtime_hle PRIVATE
        "-include${_HLE_BROWSER_ROOT}/src/hle_host_stat.h")
endif()
# The linker drops unselected original functions, not substitute implementations.
target_compile_options(vita3k_web_runtime_hle PRIVATE -ffunction-sections -fdata-sections)
