# Real VitaSDK/newlib startup HLE. Include after runtime_core.cmake.
# This deliberately does not claim all imports linked into newlib (notably
# networking and general filesystem operations). Unselected NIDs remain unsupported.
include_guard(GLOBAL)
set(_HLE_BROWSER_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(_HLE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../vita3k")
set(_HLE_EXT "${CMAKE_CURRENT_LIST_DIR}/../external")

set(_hle_exports
    sceGxmInitialize sceGxmTerminate sceGxmCreateContext sceGxmDestroyContext sceGxmFinish
    sceGxmTransferFill
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
    sceKernelGetMainModuleSdkVersion
    sceKernelGetThreadCurrentPriority sceKernelGetThreadExitStatus
    sceKernelGetThreadCpuAffinityMask sceKernelGetThreadCpuAffinityMask2
    sceKernelGetSemaInfo sceKernelTryLockMutex
    sceKernelGetStdin sceKernelGetStdout sceKernelGetStderr
    ksceKernelCreateProcessLocalStorage ksceKernelRegisterProcEventHandler
    ksceKernelGetProcessLocalStorageAddr ksceKernelGetProcessLocalStorageAddrForPid
    ksceKernelCreateMutex ksceKernelDeleteMutex ksceKernelLockMutex ksceKernelUnlockMutex
    sceClibMemcpy sceClibMemset
    # Production guest-backed heaps. Statistics/stub exports remain unselected.
    sceClibMspaceCreate sceClibMspaceDestroy sceClibMspaceMalloc
    sceClibMspaceCalloc sceClibMspaceRealloc sceClibMspaceMemalign sceClibMspaceFree
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
    # sceAppUtilInit is UNIMPLEMENTED() upstream (returns 0), so this matches the
    # desktop behaviour the game already runs with instead of adding a new stub.
    sceAppUtilInit sceAppUtilShutdown
    # Parameter queries read EmuEnvState config/licence fields only; no new stubs.
    sceAppUtilAppParamGetInt sceAppUtilSystemParamGetInt sceAppUtilSystemParamGetString
    sceAppUtilDrmOpen sceAppUtilDrmClose
    # Common-dialog entry points are UNIMPLEMENTED()/STUBBED() upstream; selecting
    # them reproduces desktop behaviour instead of inventing new return values.
    sceCommonDialogSetConfigParam sceCommonDialogUpdate sceCommonDialogIsRunning sceCommonDialogGetWorkerThreadId
    sceKernelCallAbortHandler
    sceIoWrite
    sceKernelUnlockLwMutex2 sceKernelRegisterThreadEventHandler
    # Firmware import: retain the upstream UNIMPLEMENTED warning/return value.
    # Identity and semantics remain unknown; this is stub parity, not support.
    SceThreadmgrForDriver_20C228E4
    # Observed next firmware import; also an upstream warning-producing stub.
    # No QAF semantics are implemented or inferred from its return value.
    SceQafMgrForDriver_B9770A13
    # Remaining SceSysmodule static imports, resolved by one-pass enumeration
    # (VITA3K_TRACE_MODULE_IMPORTS). Named exports reuse the upstream bodies:
    # UNIMPLEMENTED() warning stubs below are stub parity with desktop Vita3K,
    # not implemented semantics; production bodies are noted per name.
    ksceKernelSetPermission ksceKernelGetThreadId
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
    # Network init/term: production bodies are net-state writes only
    # (SceNet.cpp:471/698); net_utils gets a loopback Emscripten branch.
    sceNetInit sceNetTerm
    # Offline control initialization only; upstream stub parity, no connection.
    sceNetCtlInit sceNetCtlTerm
    # NP state initialization only; no sign-in or remote service is supplied.
    sceNpInit sceNpTerm
    # Config-based upstream service state (STUBBED); default is signed out.
    sceNpGetServiceState sceNpManagerGetNpId
    sceNpRegisterServiceStateCallback sceNpUnregisterServiceStateCallback
    sceNpCheckCallback
    # Existing upstream UNIMPLEMENTED bodies: desktop stub parity, not commerce support.
    sceNpCommerce2Init sceNpCommerce2Term
    # Upstream NP Basic lifecycle/handler stubs only; no social service support.
    sceNpBasicInit sceNpBasicTerm
    sceNpBasicRegisterHandler sceNpBasicUnregisterHandler
    # Upstream auth/signaling init stubs do not supply authentication or connections.
    sceNpAuthInit sceNpAuthTerm sceNpSignalingInit sceNpSignalingTerm
    # Production local trophy-state lifecycle (no online trophy service).
    sceNpTrophyInit sceNpTrophyTerm
    # Limbo trophy frontier (imports=30690 missing_nids=1 PC=8126bac0):
    # upstream production context/handle lifecycle over the staged TRP files.
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
    # Limbo AK::IOThread teardown after the FP vector comparison frontier:
    # upstream UNIMPLEMENTED/no-op body, desktop parity only. This does not
    # implement per-open event-flag handle ownership or close semantics.
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
    # execution_host branch); timeout and cancel return without re-acquiring.
    sceKernelWaitLwCond
    # Keep DelayThreadCB, WaitThreadEndCB, WaitEventFlagCB and WaitLwCondCB
    # unselected: their production paths wait on host condition variables
    # (or sleep) and cannot yield guest threads.
    # GetSystemTime and GetThreadRunStatus are UNIMPLEMENTED upstream. There
    # is no user sceKernelGetSystemTimeLow in nids.inc; do not invent one.
    # Immediate Limbo frontier (imports=3995 missing_nids=1 PC=8126af40):
    # sceCtrlSetSamplingMode is a production body (validates the mode range,
    # stores emuenv.ctrl.input_mode, returns the previous mode), not a stub.
    sceCtrlSetSamplingMode
    # Next Limbo frontier (imports=4005 missing_nids=1 PC=8126af50):
    # sceTouchGetPanelInfo fills constant panel geometry, sceTouchSetSamplingState
    # stores the per-port mode, and sceTouchPeek takes the non-blocking
    # touch_get peek path (no vblank wait; the blocking sceTouchRead is not
    # imported by the game). All three are production bodies, not stubs.
    sceTouchGetPanelInfo sceTouchSetSamplingState sceTouchPeek
    # Limbo graphics-memory mapping (imports=4033 missing_nids=1 PC=8126b2d0):
    # sceGxmMap/UnmapMemory record regions in gxm.memory_mapped_regions and
    # return 0 while enable_memory_mapping stays false (the default); the
    # USSE map/unmap pair are always-success upstream stubs (desktop parity).
    sceGxmMapMemory sceGxmUnmapMemory
    sceGxmMapFragmentUsseMemory sceGxmUnmapFragmentUsseMemory
    sceGxmMapVertexUsseMemory sceGxmUnmapVertexUsseMemory
    # Remaining Limbo texture descriptor imports (imports=4146 missing_nids=1
    # PC=8126b050): pure descriptor readers/writers, no renderer waits or
    # threads. Unsupported layouts still reject later at draw validation.
    sceGxmTextureSetMipFilter sceGxmTextureSetLodBias sceGxmTextureSetLodMin
    sceGxmTextureGetStride sceGxmTextureGetType
    sceGxmTextureInitLinearStrided sceGxmTextureInitSwizzled sceGxmTextureInitSwizzledArbitrary
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
    # App-state poll (imports=610599 missing_nids=1 PC=8126b640): the 513 s
    # browser run rendered 43 frames then stopped here. Upstream
    # _sceAppMgrGetAppState is a forwarder (CALL_EXPORT) to
    # __sceAppMgrGetAppState, whose body memsets the struct to 0 and returns
    # 0 — the game asks about system/app events and UI overlay, gets "none",
    # and proceeds. Production parity, not a new stub.
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
    sceNpTrophyUnlockTrophy
    # Files and directories:
    sceIoDopen sceIoDread sceIoDclose sceIoMkdir sceIoRmdir sceIoRemove sceIoRename
    sceIoPread sceIoPwrite sceIoSync sceIoSyncByFd sceIoChstat
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
    # Keep sceClibStrlcpy/Strlcat (strncpy/strncat bodies: wrong return value,
    # no terminator guarantee), sceClibVsnprintf (returns 0, not the length)
    # and sceClibStrtoll (stores a host char* through the guest endptr)
    # unselected: their upstream bodies are wrong, not merely incomplete.
    # Every utils::snprintf user (sceClibPrintf/Snprintf/Vprintf/Vsnprintf,
    # sceDbg handlers) also stays unselected: external/printf reads %ld/%lu/%z
    # arguments with the host's 8-byte long/size_t on wasm64, not the guest's 4.
    # LwCond signal/delete: signal unlinks parked waiters and sets them
    # running (notify only); delete with waiters returns ILLEGAL_CONTEXT
    # under the fiber host. No host condition variable is reached.
    sceKernelSignalLwCond sceKernelSignalLwCondAll sceKernelDeleteLwCond
    # libc time conversions and memblock lookups: host libc time math and
    # SysmemState reads; SceDateTime fill from a time_t is pure.
    sceKernelLibcGmtime_r sceKernelLibcLocaltime_r sceKernelLibcMktime
    sceKernelFindMemBlockByAddr sceKernelOpenMemBlock
    sceRtcSetTime64_t
    # libfios2 overlays: production IOState overlay table (create/resolve),
    # the scheduler query is a pure path test.
    sceFiosOverlayAddForProcess02 sceFiosOverlayResolveWithRangeSync02
    sceFiosOverlayGetRecommendedScheduler02
    # Byte-order, TLS errno slot and address formatting: pure, no socket.
    sceNetHtonl sceNetHtons sceNetNtohl sceNetNtohs sceNetErrnoLoc sceNetInetNtop
    # NP ID comparison: pure struct comparison.
    sceNpCmpNpId
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
# in module_parent.cpp uses startup_nids.inc alone. Only SceSysmem among these
# source files defines LIBRARY_INIT; adding e.g. LIBRARY(SceLibKernel) would
# reference a nonexistent import_library_init_SceLibKernel symbol.
file(CONFIGURE OUTPUT "${_hle_generated}/startup_libraries.inc" CONTENT "LIBRARY(SceSysmem)\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_HLE_ROOT}/nids/include/nids/nids.inc")

set(_hle_module_sources
    "${_HLE_ROOT}/modules/SceGxm/SceGxm.cpp"
    "${_HLE_ROOT}/modules/SceLibKernel/SceLibKernel.cpp"
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
    "${_HLE_ROOT}/modules/SceNet/SceNet.cpp"
    "${_HLE_ROOT}/modules/SceNetCtl/SceNetCtl.cpp"
    "${_HLE_ROOT}/modules/SceNpManager/SceNpManager.cpp"
    "${_HLE_ROOT}/modules/SceNpCommerce2/SceNpCommerce2.cpp"
    "${_HLE_ROOT}/modules/SceNpBasic/SceNpBasic.cpp"
    "${_HLE_ROOT}/modules/SceNpCommon/SceNpCommon.cpp"
    "${_HLE_ROOT}/modules/SceNpSignaling/SceNpSignaling.cpp"
    "${_HLE_ROOT}/modules/SceNpTrophy/SceNpTrophy.cpp"
    "${_HLE_ROOT}/modules/SceAppUtil/SceAppUtil.cpp"
    "${_HLE_ROOT}/modules/SceCommonDialog/SceCommonDialog.cpp"
    # No LIBRARY_INIT; startup_libraries.inc stays LIBRARY(SceSysmem).
    "${_HLE_ROOT}/modules/SceCtrl/SceCtrl.cpp"
    # ctrl_get implementation behind the selected SceCtrl bridges (chrono +
    # controller-state reads only; no host input device needed).
    "${_HLE_ROOT}/ctrl/src/ctrl.cpp"
    "${_HLE_ROOT}/modules/SceTouch/SceTouch.cpp"
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
    "${_HLE_BROWSER_ROOT}/src/gxm_webgpu_bridge.cpp"
    "${_lang_root}/src/lang.cpp"
    "${_lang_generated}/generated_catalog.cpp"
    "${_HLE_ROOT}/renderer/src/renderer.cpp"
    "${_HLE_ROOT}/renderer/src/creation.cpp"
    "${_HLE_ROOT}/gxm/src/textures.cpp"
    # Guest texel layout helpers (swizzle/tiled -> linear) for the scene stream.
    "${_HLE_ROOT}/renderer/src/texture/format.cpp"
    "${_HLE_ROOT}/renderer/src/texture/pvrt-dec.cpp"
    "${_HLE_ROOT}/gxm/src/attributes.cpp"
    "${_HLE_ROOT}/shader/src/usse_program_analyzer.cpp"
    "${_HLE_ROOT}/shader/src/gxp_parser.cpp"
    "${_HLE_ROOT}/gxm/src/gxp.cpp"
    "${_HLE_ROOT}/gxm/src/color.cpp"
    "${_HLE_ROOT}/gxm/src/stream.cpp"
    "${_HLE_BROWSER_ROOT}/src/gxm_hash.cpp"
    "${_HLE_EXT}/vita-toolchain/src/utils/sha256.c"
    "${_HLE_ROOT}/modules/module_parent.cpp"
    "${_HLE_ROOT}/module/src/write_return_value.cpp"
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
    "${_HLE_ROOT}/util/src/string_utils.cpp"
    "${_HLE_ROOT}/util/src/net_utils.cpp"
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
    "${_HLE_ROOT}/camera/src/camera.cpp"
    "${_HLE_ROOT}/overlay/src/display_manager.cpp"
)
file(GLOB _hle_includes "${_HLE_ROOT}/*/include")
target_include_directories(vita3k_web_runtime_hle PUBLIC ${_hle_includes}
    "${_HLE_EXT}/yaml-cpp/include")
target_include_directories(vita3k_web_runtime_hle PRIVATE
    "${_hle_generated}" "${_lang_generated}" "${_HLE_BROWSER_ROOT}/src" "${_HLE_EXT}/dlmalloc" "${_HLE_EXT}/printf"
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
    foreach(_sub AUDIO VIDEO RENDER CAMERA JOYSTICK HAPTIC SENSOR POWER DIALOG)
        set(SDL_${_sub} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory("${_HLE_EXT}/sdl" "${CMAKE_CURRENT_BINARY_DIR}/runtime-deps/sdl" EXCLUDE_FROM_ALL)
endif()
# Separate from Emscripten's process allocator: emit only mspace symbols.
# The bundled header disables mmap/morecore, so heaps stay in guest backing.
add_library(vita3k_web_mspace STATIC "${_HLE_EXT}/dlmalloc/dlmalloc.cc")
target_include_directories(vita3k_web_mspace PUBLIC "${_HLE_EXT}/dlmalloc")
target_compile_definitions(vita3k_web_mspace PRIVATE ONLY_MSPACES=1)
target_link_libraries(vita3k_web_runtime_hle
    PUBLIC vita3k_web_runtime_core
    PRIVATE SDL3::SDL3-static vita3k_web_mspace)

if(EMSCRIPTEN)
    target_compile_options(vita3k_web_runtime_hle PRIVATE
        "-include${_HLE_BROWSER_ROOT}/src/hle_host_stat.h")
endif()
# The linker drops unselected original functions, not substitute implementations.
target_compile_options(vita3k_web_runtime_hle PRIVATE -ffunction-sections -fdata-sections)
