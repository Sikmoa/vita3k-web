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

#include <np/common.h>
#include <np/trophy/context.h>

#include <mem/util.h> // Address.

#include <array>
#include <deque>
#include <optional>
#include <map>
#include <set>
#include <mutex>
#include <vector>

struct SceNpServiceStateCallback {
    Address pc;
    Address data;
};

typedef std::map<int, SceNpServiceStateCallback> np_callbacks;

struct NpTrophyUnlockCallbackData {
    std::string np_com_id;
    std::string trophy_id;
    std::string trophy_name;
    std::string trophy_detail;
    np::trophy::SceNpTrophyGrade trophy_kind{};
    std::vector<std::uint8_t> icon_buf;
};

using NpTrophyUnlockCallback = std::function<void(NpTrophyUnlockCallbackData &)>;

struct NpTrophyState {
    bool inited = false;
    std::mutex access_mutex;
    // Live handles (the shell's trophy server hands out their ids).
    std::set<int32_t> handles;
    int32_t next_handle = 1;

    std::vector<np::trophy::Context> contexts;
    std::mutex callback_mutex;
    std::vector<NpTrophyUnlockCallback> trophy_unlock_callbacks;

    void add_trophy_unlock_callback(NpTrophyUnlockCallback cb) {
        std::lock_guard lock(callback_mutex);
        trophy_unlock_callbacks.push_back(std::move(cb));
    }

    void clear_trophy_unlock_callbacks() {
        std::lock_guard lock(callback_mutex);
        trophy_unlock_callbacks.clear();
    }
};

enum SceNpServiceState : uint32_t {
    SCE_NP_SERVICE_STATE_UNKNOWN = 0,
    SCE_NP_SERVICE_STATE_SIGNED_OUT = 1,
    SCE_NP_SERVICE_STATE_SIGNED_IN = 2,
    SCE_NP_SERVICE_STATE_ONLINE = 3
};

// SceNearUtil of firmware 3.74 over an empty near.db: no neighbour and no
// discovered gift, so only the own gift (sceNearSetGift) is ever cached.
struct NearState {
    bool inited = false;
    np::CommunicationID comm_id{};
    uint32_t gift_version = 0;
    bool has_gift = false;
    uint32_t gift_id = 0;
    std::array<uint8_t, 0x1a0> gift_text{}; // name length, name, description length, description
    uint32_t gift_validity = 0;
    std::array<uint8_t, 0x2c> gift_param{};
    std::vector<uint8_t> gift_thumbnail, gift_data;
};

struct NpState {
    bool inited = false;
    np_callbacks cbs;
    SceUID state_cb_id;
    // Service-state callbacks owed one notification: the NP service queues
    // the current state when a callback registers; sceNpCheckCallback
    // delivers it once (firmware 3.74 shell SceNpImpsUtil).
    std::vector<SceUID> state_cb_pending;
    bool basic_inited = false; // SceNpBasic
    bool signaling_inited = false; // SceNpSignaling
    bool auth_inited = false; // SceNpCommon sceNpAuthInit
    // np_common's 16 ticket-request slots (sceNpAuthInit clears them). A slot
    // is taken once its ticket callback is stored and stays taken when the
    // shell refuses the request; only an accepted request gets an id.
    struct AuthRequestSlot {
        int32_t id;
        Address callback;
        Address arg;
    };
    std::array<AuthRequestSlot, 16> auth_requests{};
    bool commerce2_inited = false; // SceNpCommerce2
    struct SignalingCtx {
        np::SceNpId own_id;
        Address handler;
        Address arg;
    };
    std::map<int, SignalingCtx> signaling_ctxs; // context id (1..8)
    // SceNpSignalingMain (created by sceNpSignalingInit) runs guest code that
    // waits on a semaphore for dead-connection events in a guest ring and
    // calls their contexts' handlers (np/signaling.h).
    SceUID signaling_main_thread = 0;
    SceUID signaling_sema = 0;
    Address signaling_code = 0; // the loop, then the ring
    uint32_t signaling_queued = 0; // events written to the ring
    uint16_t signaling_last_conn_id = 0;
    // Connections whose event is still in the ring, by ring sequence.
    struct SignalingConnection {
        uint32_t seq;
        uint16_t id;
        np::SceNpId own_id, peer_id;
    };
    std::deque<SignalingConnection> signaling_pending;

    NpTrophyState trophy_state;
    NearState near;
    np::CommunicationID comm_id;
};
