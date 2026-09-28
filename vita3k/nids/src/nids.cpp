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

#include <nids/functions.h>

#define VAR_NID(name, nid) extern const char name_##name[] = #name;
#define NID(name, nid) extern const char name_##name[] = #name;
#include <nids/nids.inc>
#undef NID
#undef VAR_NID

const char *import_name(uint32_t nid) {
    switch (nid) {
#define VAR_NID(name, nid) \
    case nid:              \
        return name_##name;
#define NID(name, nid) \
    case nid:          \
        return name_##name;
#include <nids/nids.inc>
#undef NID
#undef VAR_NID
#define KNOWN_NID(name, nid) \
    case nid:                \
        return name;
#include <nids/known_nids.inc>
#undef KNOWN_NID
    default:
        return "UNRECOGNISED";
    }
}

const char *import_name(uint32_t nid, uint32_t library_nid) {
    // NIDs can have different names in different libraries. Preserve the
    // established spelling for callers without library context.
    switch ((uint64_t{library_nid} << 32) | nid) {
#define NID_ALIAS(name, nid, library)    \
    case (uint64_t{library} << 32) | nid: \
        return name;
#include <nids/nid_aliases.inc>
#undef NID_ALIAS
    default:
        return import_name(nid);
    }
}
