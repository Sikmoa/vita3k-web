# Task #22 — Support-library triage (workstream D)

Owner: workstream D (support-library triage). No runtime selection changed.
Constraint compliance: this note and `SUPPORT_LIB_TRIAGE_TASK22_NIDS.tsv` are
**new files under `browser/tests/` only**. `browser/runtime_hle.cmake`,
renderer, CPU JIT, proprietary paths, and other workstreams' files were not
touched. No builds or tests were run.

## Inputs read

- `/home/user/.vscratch/static-imports.log` (Limbo static imports)
- `/home/user/.vscratch/unselected-imports.tsv` (509 rows: 430 NOT_SELECTED + 79 UNKNOWN_NID)
- `/home/user/.vscratch/all-module-imports.log` (773-line link trace incl. guest sysmodules)
- `vita3k/nids/include/nids/nids.inc` (authoritative NID DB)
- `vita3k/modules/{SceNet,SceNetCtl,SceSsl,SceHttp,SceNpManager,SceNpBasic,`
  `SceNpCommon,SceNpTrophy,SceNpSignaling,SceNpCommerce2,SceNpActivity,`
  `SceRtc,SceDriverUser,SceLibc,SceLibft2,ScePvf,SceSblACMgr,SceSysmodule}/*`
- `browser/runtime_hle.cmake` (read-only, for proposal context)

## Import universe (measured)

- Limbo (`app0:eboot.bin`) statically imports **371** NIDs; **284** are
  Limbo-only NOT_SELECTED rows.
- Guest sysmodules load as guest code in this trace and bring their own
  internal imports (all-module-imports.log `Loading module` lines):
  `app0:sce_module/libc.suprx`, `app0:sce_module/libfios2.suprx`,
  `vs0:sys/external/libhttp.suprx` (= SceLibHttp), `libssl.suprx`,
  `libpvf.suprx`, `libSceFt2.suprx`, `libnet*.suprx`, `np_*.suprx`, plus
  `os0:kd/sysmodule.skprx` (= SceSysmodule).
- **79 UNKNOWN_NID rows: 0 imported by Limbo.** Split: 66 × SceLibHttp,
  3 × SceLibPvf, 10 × SceSysmodule. **0/79 are present in `nids.inc`**
  (verified by case-insensitive `0x<NID>` search over the whole file), and the
  loader logs every one of them as `name=UNRECOGNISED`.
- Limbo NOT_SELECTED by family (Limbo-only rows): NP 48, GXM 48, C++/CRT
  35, C-runtime named ~100 total with libm/stdio (see §6–7), NEAR 21,
  NET 17 (incl. SSL/HTTP/NetCtl), APP 12, KERNEL 8, DIALOG 8, rest
  (CTRL/TOUCH/IME/AUDIO/SYSMOD/RTC/HTTP/FIOS/DISPLAY/POWER/LIVEAREA) small.
  GXM/NEAR/DIALOG/APP/AUDIO/KERNEL/IME/CTRL/TOUCH/DISPLAY/SYSMOD belong to
  other workstreams and are listed here only as out-of-scope pointers.

## Classification taxonomy used in the TSV

- `guest-provided` — import belongs to a Sony sysmodule that loads as guest
  code (SceLibHttp/Ssl/Pvf/ft2/Fios2/Libc, SceSysmodule.skprx). Satisfied by
  loading that guest module, never by HLE selection.
- `conditionally-used` — Limbo statically imports it, but it only executes on
  a path the Measurement boot may never take (online/NP/friends/commerce) or
  needs a host backend the browser may not have (sockets/OpenSSL).
- `upstream-implemented` — real body in `vita3k/modules` (no UNIMPLEMENTED).
- `upstream-stubbed` — body is `STUBBED(...)` with fixed offline values.
- `genuinely-unknown` — absent from `nids.inc` and from all `EXPORT`s; no
  semantics recoverable offline.

## 1. Network (17 Limbo rows)

Upstream file: `vita3k/modules/SceNet/SceNet.cpp`, `SceNetCtl/SceNetCtl.cpp`,
`SceSsl/SceSsl.cpp`, `SceHttp/SceHttp.cpp`, dialogs in
`SceCommonDialog/SceCommonDialog.cpp` (out of scope, noted only).

| NID | Name | Verdict | Upstream evidence |
|---|---|---|---|
| 4C30B03C | sceNetHtonl | upstream-implemented, propose SELECT | SceNet.cpp:426 `return htonl(n);` pure |
| 9FA3207B | sceNetHtons | upstream-implemented, propose SELECT | SceNet.cpp:436 `return htons(n);` pure |
| D2EAA645 | sceNetNtohl | upstream-implemented, propose SELECT | SceNet.cpp:501 `return ntohl(n);` pure |
| 07845128 | sceNetNtohs | upstream-implemented, propose SELECT | SceNet.cpp:511 `return ntohs(n);` pure |
| EB03E265 | sceNetInit | upstream-implemented, conditionally-used, DEFER | SceNet.cpp:471, needs `emuenv.net`, host sockets |
| EA3CC286 | sceNetTerm | upstream-implemented, conditionally-used, DEFER | SceNet.cpp:698 |
| F084FCE3 | sceNetSocket | upstream-implemented, conditionally-used, DEFER | SceNet.cpp:661, `PosixSocket`/`P2PSocket` host backend |
| 29822B4D | sceNetSocketClose | upstream-implemented, conditionally-used, DEFER | SceNet.cpp:681 |
| 1296A94B | sceNetBind | upstream-implemented, conditionally-used, DEFER | SceNet.cpp:155 |
| 52DB31D5 | sceNetSendto | upstream-implemented, conditionally-used, DEFER | SceNet.cpp:606 |
| B226138B | sceNetRecvfrom | upstream-implemented, conditionally-used, DEFER | SceNet.cpp:523 |
| 495CA1DB | sceNetCtlInit | upstream-implemented, conditionally-used, DEFER | SceNetCtl.cpp:727; CALL_EXPORT risk (see §9) |
| CD188648 | sceNetCtlTerm | upstream-stubbed, DEFER | SceNetCtl.cpp:744 `STUBBED("Stub")` |
| 214926D9 | sceHttpInit | upstream-implemented(+STUBBED poolSize), DEFER | SceHttp.cpp:758; **needs OpenSSL** (`openssl/ssl.h:37-38`) |
| C9076666 | sceHttpTerm | upstream-implemented, DEFER | SceHttp.cpp:1357; CALL_EXPORTs DeleteTemplate/Connection/Request |
| 3C733316 | sceSslInit | upstream-implemented(+STUBBED poolSize), DEFER | SceSsl.cpp:85; **needs OpenSSL** (`openssl/ssl.h:21`), calls `SSL_library_init`, `SSL_CTX_new` |
| 03CE6E3A | sceSslTerm | upstream-implemented, DEFER | SceSsl.cpp:105 |

Rationale: the 4 byte-order helpers are pure (no emuenv, no CALL_EXPORT) and
safe to select; everything else needs a host socket/SSL backend that the
browser HLE library does not currently link (no OpenSSL in `browser/*.cmake`;
SceNet.cpp uses POSIX sockets, SceHttp.cpp even includes `<winsock2.h>` /
`<sys/socket.h>`). Selecting socket/SSL bodies before the HLE owner provides
those backends converts "unsupported import" into link- or runtime-failure
inside newly compiled TUs. The 4 NetCheckDialog rows are dialog-workstream
property (SceCommonDialog.cpp:561/571/582/607 — GetResult/GetStatus STUBBED,
Init implemented, Term UNIMPLEMENTED).

## 2. NP (48 Limbo rows)

### 2a. Recommend SELECT (offline-pure, no CALL_EXPORT, no fs/net)

| NID | Name | Upstream evidence |
|---|---|---|
| 54060DF6 | sceNpGetServiceState | SceNpManager.cpp:81, writes `psn_signed_in?…` then `STUBBED("Stub")` |
| AF0073B2 | sceNpManagerGetContentRatingFlag | SceNpManager.cpp:119, `*isRestricted=0; *age=21;` + STUBBED |
| FB8D82E5 | sceNpCmpNpId | SceNpCommon.cpp:100, pure id compare, error-checked |

### 2b. Conditional — implemented, owner decides (needs np state / callbacks / trophy fs)

sceNpInit 04D9F484 (SceNpManager.cpp:88), sceNpTerm 19E40AE1 (:156),
sceNpCheckCallback 3B0AE9A9 (:69, runs guest callbacks via
`thread->run_callback` — JIT interaction risk), sceNpManagerGetNpId 3C94B4B4
(:126), sceNpRegisterServiceStateCallback 44239C35 (:143),
sceNpUnregisterServiceStateCallback D9E6E56C (:165),
sceNpTrophyInit 34516838 (SceNpTrophy.cpp:486),
sceNpTrophyTerm BFE0F28F (:499),
sceNpTrophyCreateContext C49FD33F (:146, needs TRP files via `emuenv.io`;
returns `SCE_NP_TROPHY_ERROR_TRP_FILE_NOT_FOUND` without game trophy data),
sceNpTrophyDestroyContext 56F5CBA5 (:191),
sceNpTrophyCreateHandle 4EBC6977 (:182 STUBBED handle=1),
sceNpTrophyDestroyHandle FF142071 (:202 STUBBED),
sceNpTrophyUnlockTrophy B397AA24 (:551, writes trophy state to host fs),
sceNpTrophyGetTrophyUnlockState C8D2A4DE (:463),
trophy-setup-dialog 9E2C02C9/C3A59547/E37069D5/A81082DD
(SceCommonDialog.cpp:771/765/755/782, all implemented; module already in HLE
sources so low risk),
sceNpBasicGetFriendListEntryCount DF41F308 (SceNpBasic.cpp:48 STUBBED "No
friends", `*nb_friends=0`),
sceNpAuthCreateStartRequest ED42079F (SceNpCommon.cpp:51 STUBBED but
**immediately runs a guest ticket callback** — same JIT-callback risk as
sceNpCheckCallback).

### 2c. Recommend DO NOT SELECT — upstream UNIMPLEMENTED (stub parity only)

sceNpAuthInit 441D8B4E (SceNpCommon.cpp:90), sceNpAuthTerm 6093B689 (:95),
sceNpAuthGetTicket 59608D1C (:80), sceNpGetPlatformType E9A003DE (:156),
sceNpBasicInit EFB91A99 (SceNpBasic.cpp:93), sceNpBasicTerm 389BCB3B (:125),
sceNpBasicRegisterHandler 26E6E048 (:105),
sceNpBasicUnregisterHandler 050AE072 (:129),
sceNpBasicGetBlockListEntries 1211AE8E (:32),
sceNpBasicGetBlockListEntryCount 407E1E6F (:36),
sceNpBasicGetFriendListEntries FF07E787 (:44),
sceNpBasicGetFriendOnlineStatus 5183A4B5 (:57),
sceNpBasicGetGamePresenceOfFriend EF8A91BC (:73),
sceNpBasicCheckIfPlayerIsBlocked F51545D8 (:28),
sceNpSignalingInit 4B6ACF47, SetCtxOpt 0B48FADB, GetConnectionInfo 51883EAE,
ActivateConnection 92FFBDE3, TerminateConnection A413F8C2, DestroyCtx
EAA4B1F3, CreateCtx F77EF683, Term BC892D18 (all SceNpSignaling.cpp:20–88),
sceNpCommerce2Init C73F209A (:156), sceNpCommerce2Term B99958AE (:184),
sceNpActivityPostStatus BC7FDC77 (SceNpActivity.cpp:28).
Rationale: selecting these only swaps the loader's "unsupported import"
failure for an `UNIMPLEMENTED()` warning + error return. If Limbo's online
path tolerates absence (likely — boot measurement proceeds offline), the
current failure mode is more diagnosable; if a future workstream needs them,
implement first, then select.

## 3. RTC (Limbo 4 rows, 2 shared with guest modules)

All four are thin wrappers in `vita3k/modules/SceDriverUser/SceRtcUser.cpp`
over `vita3k/modules/SceRtc/SceRtc.cpp` via CALL_EXPORT:

| NID | Name | Wrapper | Callee (NID) |
|---|---|---|---|
| 0572EDDC | sceRtcGetCurrentClockLocalTime | SceRtcUser.cpp:187 | _sceRtcGetCurrentClockLocalTime 41A6C861, SceRtc.cpp:108 (host clock + tz, implemented) |
| 23F79274 | sceRtcGetCurrentTick | SceRtcUser.cpp:207 | _sceRtcGetCurrentTick 247EE33B, SceRtc.cpp:155 (`rtc_get_ticks`, implemented) |
| 811313B3 | sceRtcGetTickResolution | SceRtcUser.cpp:343 | none (`return VITA_CLOCKS_PER_SEC`) |
| C995DE02 | sceRtcGetTime64_t | SceRtcUser.cpp:358 | none (pure tick math, implemented) |

Propose SELECT all four **plus** the two callee NIDs (resolution uses
`startup_nids.inc` alone — any CALL_EXPORT target must be listed, per the
cmake comment). Requires adding two module sources (see §8). Also note
23F79274/C995DE02 are imported by SceLibHttp/SceLibSsl/SceLibFios2 too, but
those are guest-provided chains that never load in the browser, so selecting
the user exports only affects Limbo. This aligns with
`browser/tests/HLE_TIME_SELECTION.md` (nonblocking-clock selection
precedent). No dependency risk beyond the two callees.

## 4. FreeType (0 Limbo rows — guest-provided chain only)

SceLibPvf imports 17 named `FT_*` from `SceFt2` (lib nid 47D4E06B) plus 3
UNKNOWNs (§5). Every sampled upstream body in
`vita3k/modules/SceLibft2/SceFt2.cpp` is UNIMPLEMENTED
(FT_Get_Glyph:176, FT_New_Library:380, FT_New_Memory_Face:384,
FT_Set_Char_Size:528). Verdict: guest-provided + upstream-stubbed (+3
genuinely unknown). **No cmake proposal**: SceLibPvf/SceFt2 never load in the
browser HLE, so these imports are dormant; selecting FT stubs would be stub
parity with no guest consumer. Pointer: SceLibPvf's non-FT imports (sceIo*,
sceClib*, memblock) already overlap the selected HLE surface.

## 5. 79 UNKNOWN_NID rows — all genuinely unknown + guest-provided

Verified: 0/79 present in `nids.inc`; no `EXPORT` in `vita3k/modules` matches
any of them; loader logs each as `name=UNRECOGNISED`. **No cmake change is
possible** (the generator `FATAL_ERROR`s on names absent from `nids.inc`, and
inventing names is forbidden). Full table:

- **66 × SceLibHttp → SceSslInternal (lib nid 68D6C3FF)**: 04BAED82,
  08179EBD, 08ED1B25, 0DB6C206, 144FA354, 16D7C7CA, 170D595D, 17BF2367,
  19B2BB58, 2074A6EB, 2185BB05, 243DE290, 2464F9D0, 24C6257F, 260F5A85,
  29AAC768, 2F0FC3CD, 306E2A22, 33AC7A16, 349B0E73, 3746501F, 3DE4F3DB,
  461C5990, 46358909, 4A1557A7, 4AAF0D45, 4D80D3F7, 4F781BB6, 568F0E80,
  5E50CB55, 64BDF1BB, 67FB1CD3, 6CDB4EE8, 6D2A86F0, 7D30F0A6, 81FA5DBE,
  88391E03, 91C2531C, 93EAE08A, 9E38815D, A374CD7B, A4EA9DE0, AC1DBE40,
  AF50B945, B0E4CB2F, B5A099D2, B8E6E249, BF99D664, C4D984C2, CC7F4211,
  CE37B69E, CEE46B33, D1A82B1A, D28834E6, D3C049B9, D4ADF069, D68D8421,
  E5EDD91B, E746465E, EC9FAF42, EF9EBAC7, F030A0DE, F2C9B744, F71DB06F,
  FBEB1FD7, FFE2ED51. Only 3 named siblings exist upstream
  (`sceSslInternalGetCertificateAuthority` 064DFC99,
  `sceSslInternalIsInitalized` 4B4A2307, `sceSslInternalWrite` F39A49C6 —
  SceSslInternal.cpp:20/24/28); the 66 unknowns are plausibly
  newer-firmware SSL internals. Unknowable offline.
- **3 × SceLibPvf → SceFt2 (lib nid 47D4E06B)**: 04DCBE87, E9176B09,
  F6AFBB09. Unknown FreeType-for-Vita internals; upstream SceFt2 has no
  match.
- **10 × SceSysmodule**: SceThreadmgrForDriver (E2C40624) 1AAFA818,
  5053B005; SceProcessmgrForDriver (746EC971) 61B9B6FA, 6599E5D9, B1C3EFCA,
  D141C076; SceSblACMgrForDriver (9AD8E213) 6C5AB07F, 8612B243, C98D82EE,
  E87D1777. Upstream `SceSblACMgr.cpp` exports only
  `_sceSblACMgrIsGameProgram` (:20); `SceProcessmgrForDriver.cpp` has no
  matching NIDs. Kernel-driver internals, unknowable offline.
- Uncertainty: if any workstream ever LLE-loads guest `libhttp.suprx`,
  these stay unresolvable; that is a loader-workstream concern, not HLE
  selection. Re-examine only if a newer `nids.inc` names them.

## 6. C-runtime / libm (Limbo named imports via SceLibc)

### 6a. Recommend SELECT (upstream-implemented, guest-mem safe)

malloc 775A0CB2 (:767 `alloc`), free 5B9BB802 (:476), memalign A9363E6B
(:842), memcpy 7205BFDB (:859), memmove AF5C218D (:869), memset 6DC1F0D8
(:879), strcat 1434FA46 (:1177), strcpy 85B924B7 (:1203), strlen 8AECC873
(:1244), strncmp E4299DCB (:1267), strncpy 9F87712D (:1272), strrchr CEFDD143
(:1293), snprintf A1BFF606 (:1126), printf 9A004680 (:964), fopen FFFBE239
(:430), fclose EC97321C (:375), vsnprintf FE83F2E4 (:1503, CALL_EXPORTs
`snprintf` — already in this block), _sceLibcErrnoLoc 9E248B76 (:253).
All operate on `emuenv.mem`/guest pointers or are pure host formatting.

### 6b. Recommend DO NOT SELECT (upstream UNIMPLEMENTED — selecting corrupts logic)

realloc 006B54BA (:1041), memchr 2F3E5B16 (:849), memcmp 7747F6D7 (:854),
strcat-family: strchr B9336E16 (:1188), strcmp 1B58FA3B (:1193), strcspn
0E29D27A (:1214), strdup FF6F77C7 (:1219), strcasecmp 184C4B07 (:1171),
strncasecmp AF1CA2F1 (:1251), strncat FBA69BC2 (:1257), strspn 4203B663
(:1308), strstr 0D5200CB (:1313), strtok 0289B8B3 (:1333),
stdio: clearerr 72BA4468 (:340), fdopen B2F318FE (:380), ferror B724BFC1
(:390), fflush 5AAD2996 (:395), fgetc 672C58E0 (:400), fgetpos 3CDA3118
(:405), fputc 7E6A6108 (:451), fread B31C73A9 (:471), fseek C3A7CDE1 (:504),
fsetpos DC1BDBD7 (:509), ftell 41C2AF95 (:517), fwrite 8BCDCC4E (:537),
fprintf E0C79764 (:441), sprintf 7449B359 (:1144), sscanf EC585241 (:1159),
vsprintf 802FDDF9 (:1520), vsscanf A9889307 (:1530), setvbuf 2CA980A0
(:1121), ungetc 2BCB3F01 (:1433), wcsrtombs D8889FC8 (:1680),
stdlib: abort 20FE0FFF (:260), exit 826BBBAF (:370), qsort A7CBE4A6 (:1016),
rand C0883865 (:1031), ldiv D63330DA (:737), time DAE8D60F (:1403),
libm: acosf 27EAB8C1 (SceLibm.cpp:151), atan2f 4E09DD53 (:203), ceilf
6BBFEC89 (:247), expf 56473BC7 (:331), floor 22BB8237 (:375), floorf CD7C05BD
(:379), fmod 798587E4 (:423), fmodf 1CD8F88E (:427), pow 640DB443 (:663),
tanf A98E941B (:787).
Rationale: e.g. `strcmp`/`strlen`-adjacent UNIMPLEMENTED stubs return a fixed
error/0; selecting `strcmp` while keeping `strlen` would silently invert game
branches. `realloc` UNIMPLEMENTED vs `malloc/free` implemented is an
especially dangerous split — flagging so the HLE owner selects the allocator
trio only together (or not at all). Newlib startup owns the rest until
implemented upstream.

## 7. C++ / CRT-internal (35 Limbo rows — recommend DO NOT SELECT)

_ZNSt6_Mutex7_UnlockEv 0402C9F8 / _LockEv F9FAB558 / _C1Ev A4F99AE7 / _D1Ev
9B37CAD9 (SceLibstdcxx.cpp:1411/1407/1419/1431), _ZNSt6_WinitC1Ev ABB11CF9
(:1439) / _D1Ev 579C349B (:1447), _ZNSt8ios_base4InitC1Ev 5E60B2B3 (:1523) /
_D1Ev 65D88619 (:1531) / _InitEv 78CB190E (:1539) /
_5clear… C9DE8208 (:1547) / _D2Ev DCE89E71 (:1595),
_ZSt14_Xlength_errorPKc 2F6A7265 (:1787), _ZSt14_Xout_of_rangePKc 553E56BD
(:1791), _ZSt7_Fiopen… FD276300 (:1863), __aeabi_atexit EDC939E1
(SceLibc.cpp:191), __cxa_atexit 33B83B70 (:201), __cxa_guard_acquire D0310E31
(:216), __cxa_guard_release 4ED1056F (:221), __tls_get_addr BC529B7B (:238),
_sceLdTlsRegisterModuleInfo 54F87EAC (:243), _Lockfilelock 5044FC32 (:61),
_Unlockfilelock FD5DD98C (:151), _Stod BF94193B (:81), _Stoll 6B9E23FE
(:111), _Stoul E71C5CDE (:126), _Stoull 6794B3C6 (:131),
_Dtest C5B9C8D8 / _FLog D5BD8D5C / _FSin 4A496BC0 / _Log 67E99979 / _Sin
D92A7F85 (SceLibm.cpp:55/67/71/131/135) — **all UNIMPLEMENTED** with one
exception: _FSinx 93FC85ED (SceLibm.cpp:79, real cosf/sinf dispatch).
`__cxa_set_dso_handle_main` BFE02B3A (SceLibc.cpp:226) is implemented
(stores `emuenv.kernel.libc_dso_handle_main`) but startup-order sensitive.
Rationale: the browser links a real host toolchain whose headers/startup own
static-init guards, iostream init, and TLS. Selecting UNIMPLEMENTED C++
stubs would replace real startup semantics with `UNIMPLEMENTED()` error
returns (e.g. mutex lock/unlock no-ops risk data races; guard-acquire
failure breaks static locals). Defer all 35, including the two implemented
exceptions, until C++ startup ownership is decided with the runtime owner.

## 8. Exact proposed `browser/runtime_hle.cmake` changes (PROPOSAL — do not apply)

Three blocks, in dependency order. Names are existing `EXPORT`s; NIDs resolve
through the file's existing `nids.inc` loop — no new resolver. New module
sources get `CMAKE_CONFIGURE_DEPENDS` automatically via the existing glob
mechanism.

### Block A — safe pure helpers (recommend)

```cmake
# Workstream D Task #22, Block A: pure byte-order helpers (SceNet.cpp:426/436/501/511).
set(_hle_exports ${hle_exports} sceNetHtonl sceNetHtons sceNetNtohl sceNetNtohs)
# New TU compiled through the registration-only adapter (whole file must build
# under Emscripten; bridges for unselected SceNet exports are SKIPped):
list(APPEND _hle_module_sources "${_HLE_ROOT}/modules/SceNet/SceNet.cpp")
```

NIDs (already in nids.inc): 4C30B03C, 9FA3207B, D2EAA645, 07845128.
No CALL_EXPORT targets. Risk: LOW — but the owner must confirm SceNet.cpp
(needs `<net/state.h>`, `macos_net_helper.h`, `<net/if.h>`) compiles for the
browser target; that check alone gates Block A.

### Block B — RTC + implemented C-runtime (recommend, with caveats)

```cmake
# Workstream D Task #22, Block B: RTC user wrappers + kernel callees.
set(_hle_exports ${hle_exports}
    sceRtcGetCurrentClockLocalTime sceRtcGetCurrentTick
    sceRtcGetTickResolution sceRtcGetTime64_t
    _sceRtcGetCurrentClockLocalTime _sceRtcGetCurrentTick)
list(APPEND _hle_module_sources
    "${_HLE_ROOT}/modules/SceDriverUser/SceRtcUser.cpp"
    "${_HLE_ROOT}/modules/SceRtc/SceRtc.cpp")
# Workstream D Task #22, Block B: implemented SceLibc subset (§6a).
set(_hle_exports ${hle_exports}
    malloc free memalign memcpy memmove memset strcat strcpy strlen strncmp
    strncpy strrchr snprintf printf fopen fclose vsnprintf _sceLibcErrnoLoc)
list(APPEND _hle_module_sources
    "${_HLE_ROOT}/modules/SceLibc/SceLibc.cpp")
```

NIDs: 0572EDDC 23F79274 811313B3 C995DE02 + callees 41A6C861 247EE33B;
775A0CB2 5B9BB802 A9363E6B 7205BFDB AF5C218D 6DC1F0D8 1434FA46 85B924B7
8AECC873 E4299DCB 9F87712D CEFDD143 A1BFF606 9A004680 FFFBE239 EC97321C
FE83F2E4 9E248B76. Transitive CALL_EXPORTs covered inside the block:
sceRtc* → the two `_sceRtc*` (included); vsnprintf → snprintf (included).
Caveats: `printf`/`snprintf` format guest strings on host — fine; `fopen`
warns on non-read modes (`LOG_WARN_IF(mode[0] != 'r')`); `realloc` stays
unselected (deliberate — see §6b allocator-split warning).

### Block C — conditional NP/pure selection (owner opt-in, default OFF)

```cmake
# Workstream D Task #22, Block C (OPT-IN): offline-pure NP queries (§2a).
set(_hle_exports ${hle_exports}
    sceNpGetServiceState sceNpManagerGetContentRatingFlag sceNpCmpNpId)
list(APPEND _hle_module_sources
    "${_HLE_ROOT}/modules/SceNpManager/SceNpManager.cpp"
    "${_HLE_ROOT}/modules/SceNpCommon/SceNpCommon.cpp")
```

NIDs: 54060DF6 AF0073B2 FB8D82E5. No CALL_EXPORTs, no new backends.
Everything in §2b (np lifecycle/callbacks/trophy/friends-stub) is documented
for the owner but NOT proposed for selection here: trophy needs TRP content,
callbacks need JIT `run_callback` support, and §2c stays excluded.

### Explicitly NOT proposed

Sockets/SSL/HTTP bodies, NetCtl, all §2c UNIMPLEMENTED NP, all §6b/§7
UNIMPLEMENTED C/C++ stubs, all FT_*, all 79 UNKNOWNs, GXM/NEAR/dialog/APP/
kernel/thread/sysmodule rows (other workstreams).

## 9. CALL_EXPORT / dependency risk register

| Selecting… | Drags in (must also list) | Status in proposal | Risk |
|---|---|---|---|
| sceRtcGetCurrentClockLocalTime / GetCurrentTick | _sceRtcGetCurrentClockLocalTime 41A6C861, _sceRtcGetCurrentTick 247EE33B | included (Block B) | LOW; pure clock reads |
| vsnprintf | snprintf A1BFF606 | included (Block B) | LOW |
| sceHttpTerm (deferred) | sceHttpDeleteTemplate/Connection/Request | not proposed | MEDIUM; template/conn state |
| sceNetCtlInit (deferred) | sceNpManagerGetNpId (SceNetCtl.cpp:286) + sceAppUtilSystemParamGetString (:285, already selected) | NpId not proposed | MEDIUM-HIGH; adhoc path also touches net sockets |
| sceNpCheckCallback / sceNpAuthCreateStartRequest (deferred) | guest callback PCs via `thread->run_callback` | n/a | HIGH; needs JIT callback machinery |
| Any SceNet.cpp export incl. pure helpers | whole SceNet.cpp TU must compile (POSIX/socket headers) | Block A flags it | MEDIUM; Emscripten check required |
| Any SceSsl/SceHttp export | OpenSSL link (`SSL_library_init`, `SSL_CTX_new`) | none proposed | HIGH; browser does not link OpenSSL |

## 10. Uncertainties

1. Whether Limbo ever calls its NP/socket/SSL imports on the measured path —
   static imports prove linkage, not execution. A CALL_TRACE run would shrink
   Block C and confirm the DEFERs.
2. Emscripten buildability of SceNet.cpp / SceLibc.cpp / SceRtc*.cpp through
   the adapter scheme (untested by design — no builds in this workstream).
3. Trophy content availability for §2b trophy APIs (no TRP in the staged
   app dir listing available to this workstream).
4. The 79 UNKNOWNs cannot be resolved without a newer NID database or Sony
   headers; NID order/ordinals were deliberately not used for guessing.
5. `_sceAppMgrGetAppState` 5E86319A (Limbo-imported, implemented via
   CALL_EXPORT to `__sceAppMgrGetAppState`) is left to the app-management
   workstream; same for sceSysmoduleLoad/UnloadModule (implemented,
   SceSysmodule.cpp:200/242) → module-manager owner.

## Files created

- `browser/tests/SUPPORT_LIB_TRIAGE_TASK22.md` (this file)
- `browser/tests/SUPPORT_LIB_TRIAGE_TASK22_NIDS.tsv` (per-NID verdict table)

## Key findings (TL;DR for the HLE owner)

1. The 79 UNKNOWNs are all guest-module internals (66 SSL-internal, 3 FT2,
   10 driver), absent from `nids.inc`, none imported by Limbo — no action
   possible, document and move on.
2. Safe to select now: 4 pure byte-order helpers + 4 RTC (+2 callees) + 18
   implemented SceLibc functions + 3 offline-pure NP queries (Blocks A–C).
3. Defer: all socket/SSL/HTTP bodies (missing host backends in browser link),
   NP lifecycle/callbacks/trophy (need JIT callbacks + TRP content), all
   UNIMPLEMENTED C/C++/FT2 stubs (stub parity would corrupt game logic).
4. Hard CALL_EXPORT rule restated: every callee NID must be added alongside
   (RTC callees, snprintf, and — if ever selected — HttpDelete*,
   sceNpManagerGetNpId for NetCtl).
