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

// The app-event text parsers of firmware 3.74 apputil.suprx
// (sceAppUtilAppEventParse*). An event's dat is URL-encoded key=value text;
// each parser fills its output structure from a few keys. Pure functions
// over bytes, so they can be checked against the firmware code itself.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace apputil {

constexpr int APPEVENT_PARAMETER = static_cast<int>(0x80100600);
constexpr int APPEVENT_INVALID_DATA = static_cast<int>(0x80100620);
constexpr size_t APPEVENT_TEXT = 1024;

// Output sizes (the firmware clears exactly these many bytes).
constexpr size_t NP_MESSAGE_SIZE = 0x53; // SceAppUtilNpInviteMessageParam, ...NpAppDataMessageParam
constexpr size_t JOINABLE_PRESENCE_SIZE = 0x30;
constexpr size_t NEAR_GIFT_SIZE = 0x138;
constexpr size_t LIVE_AREA_SIZE = 0x41b;

enum AppEventType : uint32_t {
    APPEVENT_NP_INVITE_MESSAGE = 1,
    APPEVENT_NP_APP_DATA_MESSAGE = 2,
    APPEVENT_NP_BASIC_JOINABLE_PRESENCE = 3,
    APPEVENT_NEAR_GIFT = 4,
    APPEVENT_LIVE_AREA = 5,
};

inline int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// sceClibLookCtypeTable(c) & 7: an ASCII letter or digit.
inline bool is_alnum(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// 0x810044d4: every '%' takes the next two characters, whatever they are
// (an invalid digit counts -1 low, -16 high).
inline void decode_lenient(char *dst, const char *src) {
    size_t i = 0;
    while (src[i]) {
        if (src[i] == '%') {
            const int low = hex_digit(src[i + 2]);
            const int high = hex_digit(src[i + 1]);
            *dst++ = static_cast<char>(low + (high >= 0 ? high << 4 : -16));
            i += 3;
        } else {
            *dst++ = src[i++];
        }
    }
    *dst = '\0';
}

// 0x8100458e: '%' and two hex digits decode, anything else is kept.
inline void decode_strict(char *dst, const char *src) {
    size_t i = 0;
    while (src[i]) {
        const int high = src[i] == '%' && src[i + 1] ? hex_digit(src[i + 1]) : -1;
        const int low = high >= 0 && src[i + 2] ? hex_digit(src[i + 2]) : -1;
        if (low >= 0) {
            *dst++ = static_cast<char>((high << 4) + low);
            i += 3;
        } else {
            *dst++ = src[i++];
        }
    }
    *dst = '\0';
}

// 0x810046b6: the value after the first "key=" in the first 1024 characters,
// up to '&' or the end. Missing keys leave dst alone; only the last three
// keys are required.
enum AppEventKey {
    KEY_TYPE = 1,
    KEY_NPCOMMID,
    KEY_UID,
    KEY_JID,
    KEY_GIFTID,
    KEY_VERSION,
    KEY_PARAM,
    KEY_EVENT_TYPE,
    KEY_TRIGGER_TYPE,
    KEY_PATH,
    KEY_FACTOR,
    KEY_SESSIONID,
    KEY_INVITATIONID,
    KEY_ITEMID,
};

inline int extract_value(const char *text, char *dst, int key) {
    static const char *const keys[] = { "type=", "npcommid=", "uid=", "jid=", "giftid=", "version=", "param=",
        "event_type=", "trigger_type=", "path=", "factor=", "sessionid=", "invitationid=", "itemid=" };
    if (key < KEY_TYPE || key > KEY_ITEMID)
        return 0;
    const char *name = keys[key - 1];
    const size_t name_length = strlen(name);
    for (size_t i = 0; i < APPEVENT_TEXT && text[i]; ++i) {
        if (strncmp(text + i, name, name_length) != 0)
            continue;
        // The firmware keeps one character of trigger_type.
        const size_t limit = key == KEY_TRIGGER_TYPE ? 1 : SIZE_MAX;
        for (size_t j = i + name_length, n = 0; text[j] && text[j] != '&' && n < limit; ++j, ++n)
            *dst++ = text[j];
        return 0;
    }
    return key >= KEY_SESSIONID ? APPEVENT_INVALID_DATA : 0;
}

// 0x81004ac6: "ABCD12345_00" -> SceNpCommunicationId {data, NUL, num}.
inline int parse_npcommid(const char *value, uint8_t *out) {
    if (strnlen(value, 1023) > 12 || value[9] != '_')
        return APPEVENT_INVALID_DATA;
    memcpy(out, value, 9);
    out[10] = static_cast<uint8_t>(strtoll(value + 10, nullptr, 10));
    return 0;
}

// 0x81004b12: a JID "handle@XX.YY.label.playstation.net/res" -> SceNpId:
// handle (at most 16) at +0, the XX and YY letters at +20, the resource (at
// most 4) at +24, 1 at +28.
inline int parse_jid(const char *jid, uint8_t *out) {
    const size_t length = strnlen(jid, 1023);
    char parts[APPEVENT_TEXT] = {};
    size_t i = 0;
    while (i < length && jid[i] == ' ')
        ++i;
    if (!jid[i])
        return APPEVENT_INVALID_DATA;
    // The handle counts only when '@' follows it.
    size_t handle_length = 0;
    while (i + handle_length < length) {
        const uint8_t c = jid[i + handle_length];
        if (!is_alnum(c) && c != '+' && c != '-' && c != '_' && c != '.')
            break;
        ++handle_length;
    }
    size_t used = 1;
    if (handle_length && jid[i + handle_length] == '@') {
        memcpy(parts, jid + i, handle_length);
        used = handle_length + 1;
        i += handle_length;
    }
    if (jid[i] == '@' && i < length)
        ++i;
    // Domain, without trailing dots.
    char *domain = parts + used;
    size_t domain_length = 0;
    while (i + domain_length < length) {
        const uint8_t c = jid[i + domain_length];
        if (!is_alnum(c) && c != '.' && c != '_' && c != '-')
            break;
        ++domain_length;
    }
    while (domain_length && jid[i + domain_length - 1] == '.')
        --domain_length;
    if (domain_length + 1 > APPEVENT_TEXT - used)
        return APPEVENT_INVALID_DATA;
    memcpy(domain, jid + i, domain_length);
    used += domain_length + 1;
    i += domain_length;
    // One separator, then the resource.
    if (i < length)
        ++i;
    char *resource = parts + used;
    size_t resource_length = 0;
    while (i + resource_length < length && jid[i + resource_length])
        ++resource_length;
    if (resource_length + 1 > APPEVENT_TEXT - used)
        return APPEVENT_INVALID_DATA;
    memcpy(resource, jid + i, resource_length);

    const size_t handle_size = strnlen(parts, 16);
    if (!handle_size)
        return APPEVENT_INVALID_DATA;
    if (!is_alnum(domain[0]) || !is_alnum(domain[1]) || domain[2] != '.' || !is_alnum(domain[3]) || !is_alnum(domain[4]) || domain[5] != '.')
        return APPEVENT_INVALID_DATA;
    const char *dot = domain + 6;
    while (*dot != '.') {
        if (!*dot)
            return APPEVENT_INVALID_DATA;
        ++dot;
    }
    if (strncmp(dot + 1, "playstation.net", 16) != 0)
        return APPEVENT_INVALID_DATA;
    const size_t resource_size = strnlen(resource, 4);
    memcpy(out, parts, handle_size);
    out[20] = domain[0];
    out[21] = domain[1];
    out[22] = domain[3];
    out[23] = domain[4];
    memcpy(out + 24, resource, resource_size);
    out[28] = 1;
    return 0;
}

// The parsers after their pointer and type checks: the output is cleared,
// then filled. dat is the event's 1024-byte text.
inline int parse_np_message(const char *dat, uint8_t *out) {
    char text[APPEVENT_TEXT + 4] = {}, value[APPEVENT_TEXT] = {};
    memset(out, 0, NP_MESSAGE_SIZE);
    decode_lenient(text, dat);
    extract_value(text, value, KEY_NPCOMMID);
    if (const int error = parse_npcommid(value, out))
        return error;
    memset(value, 0, sizeof(value));
    extract_value(text, value, KEY_UID);
    memcpy(out + 12, value, 70);
    return 0;
}

inline int parse_joinable_presence(const char *dat, uint8_t *out) {
    char text[APPEVENT_TEXT + 4] = {}, value[APPEVENT_TEXT] = {};
    memset(out, 0, JOINABLE_PRESENCE_SIZE);
    decode_lenient(text, dat);
    extract_value(text, value, KEY_NPCOMMID);
    if (const int error = parse_npcommid(value, out))
        return error;
    memset(value, 0, sizeof(value));
    extract_value(text, value, KEY_JID);
    return parse_jid(value, out + 12);
}

inline int parse_near_gift(const char *dat, uint8_t *out) {
    char text[APPEVENT_TEXT + 4] = {}, value[APPEVENT_TEXT] = {};
    memset(out, 0, NEAR_GIFT_SIZE);
    decode_lenient(text, dat);
    extract_value(text, value, KEY_NPCOMMID);
    if (const int error = parse_npcommid(value, out))
        return error;
    memset(value, 0, sizeof(value));
    extract_value(text, value, KEY_JID);
    if (const int error = parse_jid(value, out + 16))
        return error;
    const auto store_number = [&](int key, size_t offset) {
        memset(value, 0, sizeof(value));
        extract_value(text, value, key);
        const uint32_t number = static_cast<uint32_t>(strtoll(value, nullptr, 0));
        memcpy(out + offset, &number, sizeof(number));
    };
    store_number(KEY_GIFTID, 12);
    store_number(KEY_VERSION, 52);
    memset(value, 0, sizeof(value));
    extract_value(text, value, KEY_PARAM);
    memcpy(out + 56, value, 256);
    return 0;
}

inline int parse_live_area(const char *dat, uint8_t *out) {
    char text[APPEVENT_TEXT + 4] = {};
    memset(out, 0, LIVE_AREA_SIZE);
    decode_strict(text, dat);
    if (strncmp(text, "psla:", 5) != 0)
        return APPEVENT_INVALID_DATA;
    strncpy(reinterpret_cast<char *>(out), text + 5, 1018);
    return 0;
}

} // namespace apputil
