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

#include <algorithm>
#include <cstring>

#include <io/state.h>
#include <kernel/state.h>
#include <np/common.h>
#include <np/state.h>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNpCommon);

enum SceNpAuthErrorCode {
    SCE_NP_AUTH_ERROR_ALREADY_INITIALIZED = 0x80550301,
    SCE_NP_AUTH_ERROR_NOT_INITIALIZED = 0x80550302,
    SCE_NP_AUTH_ERROR_INVALID_ARGUMENT = 0x80550303,
    // Names unknown below.
    SCE_NP_AUTH_ERROR_REQUEST_NOT_FOUND = 0x80550305,
    SCE_NP_AUTH_ERROR_REQUEST_MAX = 0x80550306,
    SCE_NP_AUTH_ERROR_INVALID_SERVICE_ID = 0x80550308,
    SCE_NP_AUTH_ERROR_NO_LOGIN = 0x80550309, // no PSN login id or password stored
};

enum SceNpUtilErrorCode {
    SCE_NP_UTIL_Ok = 0x0,
    SCE_NP_UTIL_ERROR_INVALID_ARGUMENT = 0x80550601,
    SCE_NP_UTIL_ERROR_INVALID_NP_ID = 0x80550605,
    SCE_NP_UTIL_ERROR_NOT_MATCH = 0x80550609,
};

EXPORT(int, sceNpAuthAbortRequest) {
    TRACY_FUNC(sceNpAuthAbortRequest);
    return UNIMPLEMENTED();
}

struct SceNpAuthRequestParameter {
    SceSize size;
    uint32_t version;
    Ptr<const char> serviceId;
    Ptr<const void> cookie;
    SceSize cookieSize;
    Ptr<const char> entitlementId;
    SceUInt32 consumedCount;
    Ptr<void> ticketCb; // int (*ticketCb)(SceNpAuthRequestId, int, void *);
    Ptr<void> cbArg;
};
static_assert(sizeof(SceNpAuthRequestParameter) == 0x24);

// np_common 0x81004d85 checks the arguments, stores the callback in a free
// slot and hands the request to the shell's NP auth service (0x81324b80),
// which refuses it at once while no PSN login is stored (0x813252b8). The
// callback never runs and the slot stays taken.
EXPORT(int, sceNpAuthCreateStartRequest, const SceNpAuthRequestParameter *param) {
    TRACY_FUNC(sceNpAuthCreateStartRequest, param);
    if (!emuenv.np.auth_inited)
        return RET_ERROR(SCE_NP_AUTH_ERROR_NOT_INITIALIZED);
    if (!param || !param->ticketCb || param->cookieSize > 1024)
        return RET_ERROR(SCE_NP_AUTH_ERROR_INVALID_ARGUMENT);
    auto &slots = emuenv.np.auth_requests;
    const auto slot = std::find_if(slots.begin(), slots.end(), [](const auto &s) { return !s.callback; });
    if (slot == slots.end())
        return RET_ERROR(SCE_NP_AUTH_ERROR_REQUEST_MAX);
    slot->callback = param->ticketCb.address();
    slot->arg = param->cbArg.address();
    if (param->size != sizeof(SceNpAuthRequestParameter))
        return RET_ERROR(SCE_NP_AUTH_ERROR_INVALID_ARGUMENT);
    const char *service_id = param->serviceId.get(emuenv.mem);
    if (!service_id || !*service_id || strnlen(service_id, 24) >= 24)
        return RET_ERROR(SCE_NP_AUTH_ERROR_INVALID_SERVICE_ID);
    return RET_ERROR(SCE_NP_AUTH_ERROR_NO_LOGIN);
}

EXPORT(int, sceNpAuthDestroyRequest) {
    TRACY_FUNC(sceNpAuthDestroyRequest);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthGetEntitlementById) {
    TRACY_FUNC(sceNpAuthGetEntitlementById);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthGetEntitlementByIdPrefix) {
    TRACY_FUNC(sceNpAuthGetEntitlementByIdPrefix);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthGetEntitlementIdList) {
    TRACY_FUNC(sceNpAuthGetEntitlementIdList);
    return UNIMPLEMENTED();
}

// np_common 0x81004ea1: a slot is found by its request id; a result not yet
// delivered by sceNpCheckCallback is 0 and copies nothing. Refused requests
// keep id 0, so id 0 finds such a slot (or a free one).
EXPORT(int, sceNpAuthGetTicket, SceInt32 req_id, Ptr<void> buf, SceSize len) {
    TRACY_FUNC(sceNpAuthGetTicket, req_id, buf, len);
    if (!emuenv.np.auth_inited)
        return RET_ERROR(SCE_NP_AUTH_ERROR_NOT_INITIALIZED);
    if (!buf || !len)
        return RET_ERROR(SCE_NP_AUTH_ERROR_INVALID_ARGUMENT);
    const auto &slots = emuenv.np.auth_requests;
    if (std::none_of(slots.begin(), slots.end(), [&](const auto &s) { return s.id == req_id; }))
        return RET_ERROR(SCE_NP_AUTH_ERROR_REQUEST_NOT_FOUND);
    return 0;
}

EXPORT(int, sceNpAuthGetTicketParam) {
    TRACY_FUNC(sceNpAuthGetTicketParam);
    return UNIMPLEMENTED();
}

// Firmware 3.74 np_common: Init registers with the shell's NP service and
// fails only when already initialized; Term succeeds either way.
EXPORT(int, sceNpAuthInit) {
    TRACY_FUNC(sceNpAuthInit);
    if (emuenv.np.auth_inited)
        return RET_ERROR(SCE_NP_AUTH_ERROR_ALREADY_INITIALIZED);
    emuenv.np.auth_inited = true;
    emuenv.np.auth_requests = {};
    return 0;
}

EXPORT(int, sceNpAuthTerm) {
    TRACY_FUNC(sceNpAuthTerm);
    emuenv.np.auth_inited = false;
    return 0;
}

EXPORT(int, sceNpCmpNpId, np::SceNpId *npid1, np::SceNpId *npid2) {
    TRACY_FUNC(sceNpCmpNpId, npid1, npid2);

    if (npid1 == nullptr || npid2 == nullptr)
        return SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;
    if (!npid1->isIdValid || !npid2->isIdValid)
        return SCE_NP_UTIL_ERROR_INVALID_NP_ID;
    if (std::strncmp(npid1->handle.data, npid2->handle.data, SCE_NP_ONLINEID_MAX_LENGTH) != 0)
        return SCE_NP_UTIL_ERROR_NOT_MATCH;

    if (std::memcmp(npid1->opt.unknown, npid2->opt.unknown, 4) != 0)
        return SCE_NP_UTIL_ERROR_NOT_MATCH;

    if (npid1->opt.platformType[0] == 0 && npid2->opt.platformType[0] == 0)
        return SCE_NP_UTIL_Ok;
    if (std::memcmp(&npid1->opt.platformType, &npid2->opt.platformType, 4) != 0)
        return SCE_NP_UTIL_ERROR_NOT_MATCH;

    return SCE_NP_UTIL_Ok;
}

EXPORT(int, sceNpCmpNpIdInOrder) {
    TRACY_FUNC(sceNpCmpNpIdInOrder);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCmpOnlineId) {
    TRACY_FUNC(sceNpCmpOnlineId);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonBase64Encode) {
    TRACY_FUNC(sceNpCommonBase64Encode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonFreeNpServerName) {
    TRACY_FUNC(sceNpCommonFreeNpServerName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonGetNpEnviroment) {
    TRACY_FUNC(sceNpCommonGetNpEnviroment);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonGetSystemSwVersion) {
    TRACY_FUNC(sceNpCommonGetSystemSwVersion);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonMallocNpServerName) {
    TRACY_FUNC(sceNpCommonMallocNpServerName);
    return UNIMPLEMENTED();
}

// Firmware 3.74 np_common: the platform field of the id, locally.
EXPORT(int, sceNpGetPlatformType, const np::SceNpId *np_id) {
    TRACY_FUNC(sceNpGetPlatformType, np_id);
    constexpr uint32_t SCE_NP_ERROR_INVALID_ARGUMENT_ID = 0x80550601, SCE_NP_ERROR_UNKNOWN_PLATFORM_TYPE = 0x80550004;
    if (!np_id)
        return RET_ERROR(SCE_NP_ERROR_INVALID_ARGUMENT_ID);
    const std::string platform(np_id->opt.platformType, strnlen(np_id->opt.platformType, sizeof(np_id->opt.platformType)));
    if (platform.empty())
        return 0;
    if (platform == "ps3")
        return 1;
    if (platform == "psp2")
        return 2;
    if (platform == "ps4")
        return 3;
    return RET_ERROR(SCE_NP_ERROR_UNKNOWN_PLATFORM_TYPE);
}

EXPORT(int, sceNpSetPlatformType) {
    TRACY_FUNC(sceNpSetPlatformType);
    return UNIMPLEMENTED();
}
