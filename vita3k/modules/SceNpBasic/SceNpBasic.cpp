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

#include <module/module.h>

#include <config/state.h>
#include <np/state.h>

// Error values of firmware 3.74 np_basic.suprx; names where known.
enum SceNpBasicError : uint32_t {
    SCE_NP_ERROR_NOT_INITIALIZED = 0x80550002,
    SCE_NP_BASIC_ERROR_INVALID_ARGUMENT = 0x80551d02,
    SCE_NP_BASIC_ERROR_NOT_INITIALIZED = 0x80551d04,
    SCE_NP_BASIC_ERROR_ALREADY_INITIALIZED = 0x80551d05,
    SCE_NP_BASIC_ERROR_NO_COMMUNICATION_ID = 0x80551d0b, // sceNpInit had none
    // The NP service's friend/block lists need PSN sign-in (0x80551a08 from
    // the service), presence needs the online state (0x80551a09).
    SCE_NP_BASIC_ERROR_NOT_SIGNED_IN = 0x80551d06,
    SCE_NP_BASIC_ERROR_NOT_ONLINE = 0x80551d07,
    // Signed in, the service's list was never fetched from the server
    // (np_basic 0x8100024c: the service answers 0 with its loaded flag clear).
    SCE_NP_BASIC_ERROR_LIST_NOT_LOADED = 0x80551d0d, // name unknown
};

// Friend and block lists and presence come from the NP service in the
// shell. It keeps the lists in memory only, filled from the PSN server; no
// server ever answers here, so signed in they stay unloaded.
static bool signed_in(EmuEnvState &emuenv) {
    return emuenv.cfg.current_config.psn_signed_in;
}

EXPORT(int, sceNpBasicCheckCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicCheckIfPlayerIsBlocked, Ptr<const void> np_id, SceInt32 *result) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!np_id || !result)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (!signed_in(emuenv))
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_SIGNED_IN);
    // The service writes one result byte even with the list unloaded.
    *reinterpret_cast<uint8_t *>(result) = 0;
    return RET_ERROR(SCE_NP_BASIC_ERROR_LIST_NOT_LOADED);
}

EXPORT(int, sceNpBasicGetBlockListEntries, SceUInt32 start, Ptr<void> entries, SceUInt32 num, SceSize *retrieved) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!entries || !num || !retrieved || start >= 100)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (!signed_in(emuenv))
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_SIGNED_IN);
    return RET_ERROR(SCE_NP_BASIC_ERROR_LIST_NOT_LOADED);
}

EXPORT(int, sceNpBasicGetBlockListEntryCount, SceSize *count) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!count)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (!signed_in(emuenv))
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_SIGNED_IN);
    *count = 0;
    return RET_ERROR(SCE_NP_BASIC_ERROR_LIST_NOT_LOADED);
}

EXPORT(int, sceNpBasicGetFriendContextState) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetFriendListEntries, SceUInt32 start, Ptr<void> entries, SceUInt32 num, SceSize *retrieved) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!entries || !num || !retrieved || start >= 100)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (!signed_in(emuenv))
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_SIGNED_IN);
    return RET_ERROR(SCE_NP_BASIC_ERROR_LIST_NOT_LOADED);
}

EXPORT(int, sceNpBasicGetFriendListEntryCount, SceSize *count) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!count)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    if (!signed_in(emuenv))
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_SIGNED_IN);
    *count = 0;
    return RET_ERROR(SCE_NP_BASIC_ERROR_LIST_NOT_LOADED);
}

EXPORT(int, sceNpBasicGetFriendOnlineStatus, Ptr<const void> np_id, SceInt32 *status) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!np_id || !status)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    // The service answers status 0 unless its session is online.
    *status = 0;
    return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_ONLINE);
}

EXPORT(int, sceNpBasicGetFriendRequestEntries) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetFriendRequestEntryCount) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetGameJoiningPresence) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetGamePresenceOfFriend, Ptr<const void> np_id, Ptr<void> presence) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!np_id || !presence)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_ONLINE);
}

EXPORT(int, sceNpBasicGetPlaySessionLog) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetPlaySessionLogSize) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetRequestedFriendRequestEntries) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetRequestedFriendRequestEntryCount) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicInit, Ptr<void> param) {
    if (emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_ALREADY_INITIALIZED);
    emuenv.np.basic_inited = true;
    return 0;
}

EXPORT(int, sceNpBasicJoinGameAckResponseSend) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicRecordPlaySessionLog) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicRegisterHandler, Ptr<const void> handlers, Ptr<const void> comm_id, Ptr<void> arg) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!handlers)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    // Without a communication id the one given to sceNpInit is used.
    if (!comm_id && !emuenv.np.inited)
        return RET_ERROR(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!comm_id && !emuenv.np.comm_id.data[0])
        return RET_ERROR(SCE_NP_BASIC_ERROR_NO_COMMUNICATION_ID);
    return 0; // no event reaches the handlers while offline
}

EXPORT(int, sceNpBasicRegisterInGameDataMessageHandler) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicRegisterJoinGameAckHandler) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicSendInGameDataMessage) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicSetInGamePresence) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicTerm) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    emuenv.np.basic_inited = false;
    return 0;
}

EXPORT(int, sceNpBasicUnregisterHandler) {
    if (!emuenv.np.basic_inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    return 0;
}

EXPORT(int, sceNpBasicUnregisterInGameDataMessageHandler) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicUnregisterJoinGameAckHandler) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicUnsetInGamePresence) {
    return UNIMPLEMENTED();
}
