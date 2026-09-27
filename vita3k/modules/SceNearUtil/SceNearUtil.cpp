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

#include <kernel/state.h>
#include <np/state.h>
#include <rtc/rtc.h>

#include <algorithm>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNearUtil);

// Firmware 3.74 SceNearUtil. The library talks to the NearUtilService app
// (NPXS10032), which answers from its near.db; on this console that database
// is empty, so no neighbour or discovered gift ever exists. Nothing on the
// way checks PSN sign-in or the network. Error names are not public; the
// values are the firmware's.
enum SceNearErrorCode : uint32_t {
    NEAR_ERROR_INVALID_ARGUMENT = 0x80104901,
    NEAR_ERROR_INVALID_COMMUNICATION_ID = 0x80104902,
    NEAR_ERROR_ALREADY_INITIALIZED = 0x80104903,
    NEAR_ERROR_NO_MEMORY = 0x80104904,
    NEAR_ERROR_NOT_INITIALIZED = 0x80104905,
    NEAR_ERROR_INVALID_TEXT = 0x80104906,
    NEAR_ERROR_THUMBNAIL_TOO_LARGE = 0x80104907,
    NEAR_ERROR_DATA_TOO_LARGE = 0x80104908,
    NEAR_ERROR_INVALID_VALIDITY = 0x80104909,
    NEAR_ERROR_INVALID_GIFT_PARAM = 0x8010490A,
    NEAR_ERROR_GIFT_NOT_FOUND = 0x80104912,
    NEAR_ERROR_THUMBNAIL_BUFFER_TOO_SMALL = 0x80104913,
    NEAR_ERROR_DATA_BUFFER_TOO_SMALL = 0x80104914,
    NEAR_ERROR_NO_OWN_GIFT = 0x80104918,
    NEAR_ERROR_NOT_OPENED = 0x8010491B,
    NEAR_ERROR_NO_DISCOVERED_GIFT = 0x8010491E,
    NEAR_ERROR_NEWER_GIFT_VERSION = 0x80104920,
    NEAR_ERROR_INVALID_GIFT_ID = 0x80104925,
};

struct SceNearInitParam {
    Ptr<void> mem;
    SceSize memSize;
};

// The layout sceNearSetGift reads and sceNearGetGift returns.
struct SceNearGiftText {
    SceUInt32 nameLength;
    char name[136];
    SceUInt32 descriptionLength;
    char description[272];
};
static_assert(sizeof(SceNearGiftText) == 0x1a0);

#define NEAR_REQUIRE_INITIALIZED()                            \
    do {                                                      \
        if (!emuenv.np.near.inited)                           \
            return RET_ERROR(NEAR_ERROR_NOT_INITIALIZED);     \
    } while (0)

// The discovered-gift lookup: id 0 is refused, and the list is empty.
static uint32_t missing_discovered_gift(SceUInt32 gift_id) {
    return gift_id == 0 ? NEAR_ERROR_GIFT_NOT_FOUND : NEAR_ERROR_NO_DISCOVERED_GIFT;
}

// The communication ID as np_common prints it must read AAAA99999_99.
static bool is_near_communication_id(const np::CommunicationID &id) {
    for (int i = 0; i < 4; ++i) {
        if (id.data[i] < 'A' || id.data[i] > 'Z')
            return false;
    }
    for (int i = 4; i < 9; ++i) {
        if (id.data[i] < '0' || id.data[i] > '9')
            return false;
    }
    return id.num < 100;
}

EXPORT(int, sceNearCloseDiscoveredGiftImage, SceUInt32 giftId) {
    TRACY_FUNC(sceNearCloseDiscoveredGiftImage, giftId);
    NEAR_REQUIRE_INITIALIZED();
    return RET_ERROR(NEAR_ERROR_NOT_OPENED);
}

EXPORT(int, sceNearCloseReceivedGiftData, SceUInt32 giftId) {
    TRACY_FUNC(sceNearCloseReceivedGiftData, giftId);
    NEAR_REQUIRE_INITIALIZED();
    return RET_ERROR(NEAR_ERROR_NOT_OPENED);
}

// param is the SceAppUtilNearGiftParam of a Near gift app event.
EXPORT(int, sceNearConvertDiscoveredGiftParam, const uint8_t *param, SceUInt32 *giftId) {
    TRACY_FUNC(sceNearConvertDiscoveredGiftParam, param, giftId);
    NEAR_REQUIRE_INITIALIZED();
    if (!param || !giftId)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (memcmp(param, &emuenv.np.near.comm_id, sizeof(np::CommunicationID)) != 0)
        return RET_ERROR(NEAR_ERROR_GIFT_NOT_FOUND);
    SceUInt32 version, id;
    memcpy(&version, param + 52, sizeof(version));
    memcpy(&id, param + 12, sizeof(id));
    if (version > emuenv.np.near.gift_version)
        return RET_ERROR(NEAR_ERROR_NEWER_GIFT_VERSION);
    return RET_ERROR(missing_discovered_gift(id));
}

EXPORT(int, sceNearDeleteDiscoveredGift, SceUInt32 giftId) {
    TRACY_FUNC(sceNearDeleteDiscoveredGift, giftId);
    NEAR_REQUIRE_INITIALIZED();
    return RET_ERROR(giftId == 0 ? NEAR_ERROR_NO_OWN_GIFT : NEAR_ERROR_NO_DISCOVERED_GIFT);
}

EXPORT(int, sceNearDeleteGift, SceUInt32 giftId) {
    TRACY_FUNC(sceNearDeleteGift, giftId);
    NEAR_REQUIRE_INITIALIZED();
    auto &near = emuenv.np.near;
    if (!near.has_gift || near.gift_id != giftId)
        return RET_ERROR(NEAR_ERROR_NO_OWN_GIFT);
    near.has_gift = false;
    near.gift_id = 0;
    near.gift_text = {};
    near.gift_validity = 0;
    near.gift_param = {};
    near.gift_thumbnail.clear();
    near.gift_data.clear();
    return 0;
}

EXPORT(int, sceNearFinalize, const np::CommunicationID *commId) {
    TRACY_FUNC(sceNearFinalize, commId);
    NEAR_REQUIRE_INITIALIZED();
    if (!commId)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (memcmp(commId, &emuenv.np.near.comm_id, sizeof(*commId)) != 0)
        return RET_ERROR(NEAR_ERROR_INVALID_COMMUNICATION_ID);
    emuenv.np.near = {};
    return 0;
}

// The launches build a near: URI and return sceAppMgrLaunchAppByUri2, which
// only queues the URI for the shell and returns 0; the Near app starting in
// the foreground afterwards is outside the calling game. Type 1 opens Near,
// type 2 (a 4-byte gift id) a discovered gift, which the empty near.db lacks.
EXPORT(int, sceNearFinalizeAndLaunchNearApp, SceUInt32 type, SceSize size, const SceUInt32 *arg) {
    TRACY_FUNC(sceNearFinalizeAndLaunchNearApp, type, size, arg);
    NEAR_REQUIRE_INITIALIZED();
    if (!(type == 1 && size == 0) && !(type == 2 && size == sizeof(SceUInt32)))
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (type == 2)
        return RET_ERROR(arg ? missing_discovered_gift(*arg) : NEAR_ERROR_INVALID_ARGUMENT);
    emuenv.np.near = {}; // as sceNearFinalize
    return 0;
}

EXPORT(int, sceNearGetDiscoveredGiftInfo, SceUInt32 giftId, void *info) {
    TRACY_FUNC(sceNearGetDiscoveredGiftInfo, giftId, info);
    NEAR_REQUIRE_INITIALIZED();
    if (!info)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    return RET_ERROR(missing_discovered_gift(giftId));
}

EXPORT(int, sceNearGetDiscoveredGiftSender, SceUInt32 giftId, void *sender) {
    TRACY_FUNC(sceNearGetDiscoveredGiftSender, giftId, sender);
    NEAR_REQUIRE_INITIALIZED();
    if (!sender)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    return RET_ERROR(missing_discovered_gift(giftId));
}

EXPORT(int, sceNearGetDiscoveredGiftStatus, SceUInt32 giftId, SceInt32 *status) {
    TRACY_FUNC(sceNearGetDiscoveredGiftStatus, giftId, status);
    NEAR_REQUIRE_INITIALIZED();
    if (!status)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    return RET_ERROR(missing_discovered_gift(giftId));
}

// With *num 0 the result is the count; otherwise the list is copied, *num
// clamped to the count, and the count returned. Both lists are empty.
EXPORT(int, sceNearGetDiscoveredGifts, SceUInt32 *num, SceUInt32 *giftIds) {
    TRACY_FUNC(sceNearGetDiscoveredGifts, num, giftIds);
    NEAR_REQUIRE_INITIALIZED();
    if (!num)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (*num == 0)
        return 0;
    if (!giftIds)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    *num = 0;
    return 0;
}

// Returns 1 with the own gift, 0 (nothing written) without one. A size
// pointer holding 0 asks for the size.
EXPORT(int, sceNearGetGift, SceUInt32 *giftId, SceNearGiftText *text, SceUInt32 *thumbnailSize, void *thumbnail,
    SceUInt32 *dataSize, void *data, SceUInt32 *validity, void *param) {
    TRACY_FUNC(sceNearGetGift, giftId, text, thumbnailSize, thumbnail, dataSize, data, validity, param);
    NEAR_REQUIRE_INITIALIZED();
    const auto &near = emuenv.np.near;
    if (!giftId)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (!near.has_gift)
        return 0;
    *giftId = near.gift_id;
    if (text)
        memcpy(text, near.gift_text.data(), near.gift_text.size());
    const auto copy_out = [](SceUInt32 *size, void *buffer, const std::vector<uint8_t> &bytes, uint32_t too_small) -> uint32_t {
        if (!size)
            return 0;
        const auto length = static_cast<SceUInt32>(bytes.size());
        if (*size == 0) {
            *size = length;
            return 0;
        }
        if (!buffer)
            return NEAR_ERROR_INVALID_ARGUMENT;
        if (*size < length)
            return too_small;
        *size = length;
        memcpy(buffer, bytes.data(), length);
        return 0;
    };
    if (const uint32_t error = copy_out(thumbnailSize, thumbnail, near.gift_thumbnail, NEAR_ERROR_THUMBNAIL_BUFFER_TOO_SMALL))
        return RET_ERROR(error);
    if (const uint32_t error = copy_out(dataSize, data, near.gift_data, NEAR_ERROR_DATA_BUFFER_TOO_SMALL))
        return RET_ERROR(error);
    if (validity)
        *validity = near.gift_validity;
    if (param)
        memcpy(param, near.gift_param.data(), near.gift_param.size());
    return 1;
}

EXPORT(int, sceNearGetGiftStatus) {
    TRACY_FUNC(sceNearGetGiftStatus);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNearGetLastNeighborFoundDateTime) {
    TRACY_FUNC(sceNearGetLastNeighborFoundDateTime);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNearGetMyStatus) {
    TRACY_FUNC(sceNearGetMyStatus);
    return UNIMPLEMENTED();
}

// neighbors points to the caller's array pointer.
EXPORT(int, sceNearGetNeighbors, SceUInt32 *num, Ptr<void> *neighbors) {
    TRACY_FUNC(sceNearGetNeighbors, num, neighbors);
    NEAR_REQUIRE_INITIALIZED();
    if (!num)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (*num == 0)
        return 0;
    if (!neighbors || !*neighbors)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    *num = 0;
    return 0;
}

EXPORT(int, sceNearGetNewNeighbors) {
    TRACY_FUNC(sceNearGetNewNeighbors);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNearGetRecentNeighbors) {
    TRACY_FUNC(sceNearGetRecentNeighbors);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNearIgnoreDiscoveredGift) {
    TRACY_FUNC(sceNearIgnoreDiscoveredGift);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNearInitialize, const np::CommunicationID *commId, const SceNearInitParam *param, SceUInt32 giftVersion) {
    TRACY_FUNC(sceNearInitialize, commId, param, giftVersion);
    auto &near = emuenv.np.near;
    if (near.inited)
        return RET_ERROR(NEAR_ERROR_ALREADY_INITIALIZED);
    near = {};
    if (!commId || !param || !param->mem)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (param->memSize < 0x40000)
        return RET_ERROR(NEAR_ERROR_NO_MEMORY);
    if (!is_near_communication_id(*commId))
        return RET_ERROR(NEAR_ERROR_INVALID_COMMUNICATION_ID);
    near.comm_id = *commId;
    near.gift_version = giftVersion;
    near.inited = true;
    return 0;
}

EXPORT(int, sceNearLaunchNearAppForDownload) {
    TRACY_FUNC(sceNearLaunchNearAppForDownload);
    return UNIMPLEMENTED();
}

// No check at all, not even initialization: the "near:010" launch is queued.
EXPORT(int, sceNearLaunchNearAppForUpdate) {
    TRACY_FUNC(sceNearLaunchNearAppForUpdate);
    return 0;
}

// The gift id is the handle; there is nothing to open.
EXPORT(int, sceNearOpenDiscoveredGiftImage, SceUInt32 giftId) {
    TRACY_FUNC(sceNearOpenDiscoveredGiftImage, giftId);
    NEAR_REQUIRE_INITIALIZED();
    return RET_ERROR(missing_discovered_gift(giftId));
}

EXPORT(int, sceNearOpenReceivedGiftData, SceUInt32 giftId) {
    TRACY_FUNC(sceNearOpenReceivedGiftData, giftId);
    NEAR_REQUIRE_INITIALIZED();
    return RET_ERROR(missing_discovered_gift(giftId));
}

EXPORT(int, sceNearReadDiscoveredGiftImage, SceUInt32 giftId, void *buffer, SceSize size, SceOff offset) {
    TRACY_FUNC(sceNearReadDiscoveredGiftImage, giftId, buffer, size, offset);
    NEAR_REQUIRE_INITIALIZED();
    if (size && !buffer)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    return RET_ERROR(NEAR_ERROR_NOT_OPENED);
}

EXPORT(int, sceNearReadReceivedGiftData, SceUInt32 giftId, void *buffer, SceSize size, SceOff offset) {
    TRACY_FUNC(sceNearReadReceivedGiftData, giftId, buffer, size, offset);
    NEAR_REQUIRE_INITIALIZED();
    if (size && !buffer)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    return RET_ERROR(NEAR_ERROR_NOT_OPENED);
}

// Reloads the lists from the service: still empty.
EXPORT(int, sceNearRefresh, const np::CommunicationID *commId) {
    TRACY_FUNC(sceNearRefresh, commId);
    NEAR_REQUIRE_INITIALIZED();
    if (!commId)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (memcmp(commId, &emuenv.np.near.comm_id, sizeof(*commId)) != 0)
        return RET_ERROR(NEAR_ERROR_INVALID_COMMUNICATION_ID);
    return 0;
}

EXPORT(int, sceNearSetGift, SceUInt32 giftId, const SceNearGiftText *text, SceUInt32 thumbnailSize, const uint8_t *thumbnail,
    SceUInt32 dataSize, const uint8_t *data, SceUInt32 validity, const uint8_t *param) {
    TRACY_FUNC(sceNearSetGift, giftId, text, thumbnailSize, thumbnail, dataSize, data, validity, param);
    NEAR_REQUIRE_INITIALIZED();
    if (!text)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (text->nameLength == 0 || text->nameLength >= 135 || text->descriptionLength == 0 || text->descriptionLength >= 270
        || !text->name[0] || !text->description[0])
        return RET_ERROR(NEAR_ERROR_INVALID_TEXT);
    if (!thumbnailSize || !thumbnail)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (thumbnailSize > 0x2000)
        return RET_ERROR(NEAR_ERROR_THUMBNAIL_TOO_LARGE);
    if (!dataSize || !data)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    if (dataSize > 0x19000)
        return RET_ERROR(NEAR_ERROR_DATA_TOO_LARGE);
    if (validity > 0x7fffffff && validity != 0xffffffff)
        return RET_ERROR(NEAR_ERROR_INVALID_VALIDITY);
    if (!param)
        return RET_ERROR(NEAR_ERROR_INVALID_ARGUMENT);
    // param: probability (0..100) at +0x28, 16 reserved zero bytes at +4 and
    // an optional date (SceDateTime) at +0x14.
    if (param[0x28] > 100 || std::any_of(param + 4, param + 20, [](uint8_t byte) { return byte != 0; }))
        return RET_ERROR(NEAR_ERROR_INVALID_GIFT_PARAM);
    if (std::any_of(param + 20, param + 36, [](uint8_t byte) { return byte != 0; })
        && rtc_check_valid(reinterpret_cast<const SceDateTime *>(param + 20)) != 0)
        return RET_ERROR(NEAR_ERROR_INVALID_GIFT_PARAM);
    const auto *process_param = emuenv.kernel.process_param.get(emuenv.mem);
    if (process_param && process_param->fw_version >= 0x02100000 && (giftId & 0x1f000000))
        return RET_ERROR(NEAR_ERROR_INVALID_GIFT_ID);

    auto &near = emuenv.np.near;
    near.has_gift = true;
    near.gift_id = giftId;
    near.gift_text = {};
    SceNearGiftText stored{};
    stored.nameLength = text->nameLength;
    memcpy(stored.name, text->name, text->nameLength);
    stored.descriptionLength = text->descriptionLength;
    memcpy(stored.description, text->description, text->descriptionLength);
    memcpy(near.gift_text.data(), &stored, sizeof(stored));
    near.gift_validity = validity;
    memcpy(near.gift_param.data(), param, near.gift_param.size());
    near.gift_thumbnail.assign(thumbnail, thumbnail + thumbnailSize);
    near.gift_data.assign(data, data + dataSize);
    return 0;
}

EXPORT(int, sceNearSetGift2) {
    TRACY_FUNC(sceNearSetGift2);
    return UNIMPLEMENTED();
}
