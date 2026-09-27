// SPDX-License-Identifier: GPL-2.0-or-later
// JSON string literals for the bridges' page messages (EM_JS JSON.parse).
#pragma once

#include <cstdio>
#include <string>

namespace browser {
inline void json_quote(std::string &out, const std::string &text) {
    out += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
            out += escaped;
        } else {
            out += c;
        }
    }
    out += '"';
}
} // namespace browser
