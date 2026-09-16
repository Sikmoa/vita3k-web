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

#include <kernel/relocation.h>
#include <mem/ptr.h>
#include <util/log.h>

#include <cassert>
#include <cstring>
#include <string>

static constexpr bool LOG_RELOCATIONS = false;

enum Code {
    None = 0,
    Abs32 = 2,
    Rel32 = 3,
    Abs8 = 8,
    ThumbCall = 10,
    Call = 28,
    Jump24 = 29,
    Target1 = 38,
    V4BX = 40,
    Target2 = 41,
    Prel31 = 42,
    MovwAbsNc = 43,
    MovtAbs = 44,
    ThumbMovwAbsNc = 47,
    ThumbMovtAbs = 48,
    RBase = 255
};

// Common type so we can static_cast between unknown/known formats
// This is based on https://github.com/vitasdk/vita-toolchain/blob/master/src/sce-elf.h
struct Entry {};

struct EntryFormatUnknown : Entry {
    uint32_t format : 4;
};

struct EntryFormat0 : Entry {
    uint32_t format : 4;
    uint32_t symbol_segment : 4;
    uint32_t code : 8;
    uint32_t patch_segment : 4;
    uint32_t code2 : 8;
    uint32_t dist2 : 4;

    uint32_t addend;

    uint32_t offset;
};

struct EntryFormat1 : Entry {
    uint32_t format : 4;
    uint32_t symbol_segment : 4;
    uint32_t code : 8;
    uint32_t patch_segment : 4;
    uint32_t offset_lo : 12;

    uint32_t offset_hi : 10;
    uint32_t addend : 22;
};

// Used by var import relocations
struct EntryFormat1Alt : Entry {
    uint32_t format : 4;
    uint32_t patch_segment : 4;
    uint32_t code : 8;
    uint32_t addend : 16;

    uint32_t offset;
};

struct EntryFormat2 : Entry {
    uint32_t format : 4;
    uint32_t symbol_segment : 4;
    uint32_t code : 8;
    uint32_t offset : 16;

    uint32_t addend;
};

// Used by var import relocations
struct EntryFormat2Alt : Entry {
    uint32_t format : 4;
    uint32_t patch_segment : 4;
    uint32_t code : 8;
    uint32_t padding : 16;

    uint32_t offset;
    uint32_t addend;
};

struct EntryFormat3 : Entry {
    uint32_t format : 4;
    uint32_t symbol_segment : 4;
    uint32_t mode : 1; // ARM = 0, THUMB = 1
    uint32_t offset : 18;
    uint32_t dist2 : 5;

    uint32_t addend : 22;
};

struct EntryFormat4 : Entry {
    uint32_t format : 4;
    uint32_t offset : 23;
    uint32_t dist2 : 5;
};

struct EntryFormat5 : Entry {
    uint32_t format : 4;
    uint32_t dist1 : 9;
    uint32_t dist2 : 5;
    uint32_t dist3 : 9;
    uint32_t dist4 : 5;
};

struct EntryFormat6 : Entry {
    uint32_t format : 4;
    uint32_t offset : 28;
};

struct EntryFormat7_8_9 : Entry {
    uint32_t format : 4;
    // format 7 has 4 offsets, 7 bits each
    // format 8 has 7 offsets, 4 bits each
    // format 9 has 14 offsets, 2 bits each
    uint32_t offsets : 28;
};

static void write(void *data, uint32_t value) {
    memcpy(data, &value, sizeof(value));
}

static void write_masked(void *data, uint32_t symbol, uint32_t mask) {
    write(data, symbol & mask);
}

static void write_abs8(void *data, uint8_t value) {
    uint8_t *const p = static_cast<uint8_t *>(data);
    *p = value;
}

static void write_call(void *data, uint32_t symbol) {
    struct Instruction {
        uint32_t imm24 : 24;
        uint32_t insn : 8;
    };

    static_assert(sizeof(Instruction) == 4, "Incorrect size.");

    Instruction *const insn = static_cast<Instruction *>(data);
    insn->imm24 = symbol;
}

static void write_thumb_call(void *data, uint32_t symbol) {
    // This is cribbed from UVLoader, but I used bitfields to get rid of some shifting and masking.
    struct Upper {
        uint16_t imm10 : 10;
        uint16_t sign : 1;
        uint16_t ignored : 5;
    };

    struct Lower {
        uint16_t imm11 : 11;
        uint16_t j2 : 1;
        uint16_t unknown : 1;
        uint16_t j1 : 1;
        uint16_t unknown2 : 2;
    };

    struct Pair {
        Upper upper;
        Lower lower;
    };

    static_assert(sizeof(Pair) == 4, "Incorrect size.");

    Pair *const pair = static_cast<Pair *>(data);
    pair->lower.imm11 = symbol >> 1;
    pair->upper.imm10 = symbol >> 12;
    pair->upper.sign = symbol >> 24;
    pair->lower.j2 = pair->upper.sign ^ ((~symbol) >> 22);
    pair->lower.j1 = pair->upper.sign ^ ((~symbol) >> 23);
}

static void write_mov_abs(void *data, uint16_t symbol) {
    struct Instruction {
        uint32_t imm12 : 12;
        uint32_t ignored1 : 4;
        uint32_t imm4 : 4;
        uint32_t ignored2 : 12;
    };

    static_assert(sizeof(Instruction) == 4, "Incorrect size.");

    Instruction *const instruction = static_cast<Instruction *>(data);
    instruction->imm12 = symbol;
    instruction->imm4 = symbol >> 12;
}

static void write_thumb_mov_abs(void *data, uint16_t symbol) {
    // This is cribbed from UVLoader, but I used bitfields to get rid of some shifting and masking.
    struct Upper {
        uint16_t imm4 : 4;
        uint16_t ignored1 : 6;
        uint16_t i : 1;
        uint16_t ignored2 : 5;
    };

    struct Lower {
        uint16_t imm8 : 8;
        uint16_t ignored1 : 4;
        uint16_t imm3 : 3;
        uint16_t ignored2 : 1;
    };

    struct Pair {
        Upper upper;
        Lower lower;
    };

    static_assert(sizeof(Pair) == 4, "Incorrect size.");

    Pair *const pair = static_cast<Pair *>(data);
    pair->lower.imm8 = symbol;
    pair->lower.imm3 = symbol >> 8;
    pair->upper.i = symbol >> 11;
    pair->upper.imm4 = symbol >> 12;
}

static bool relocate_entry(void *data, uint32_t code, uint32_t symval, uint32_t addend, uint32_t addr) {
    LOG_DEBUG_IF(LOG_RELOCATIONS, "code: {}, data: {}, addr: {}, symval: {}, addend: {}", code, data, log_hex(addr), log_hex(symval), log_hex(addend));
    switch (code) {
    case None:
    case V4BX: // Untested.
    case RBase:
        return true;

    case Abs32:
    case Target1:
        write(data, symval + addend);
        return true;

    case Abs8:
        write_abs8(data, symval + addend);
        return true;

    case Rel32:
    case Target2:
        write(data, symval + addend - addr);
        return true;

    case Prel31:
        write_masked(data, symval + addend - addr, INT32_MAX);
        return true;

    case ThumbCall:
        write_thumb_call(data, symval + addend - addr);
        return true;

    case Call:
    case Jump24:
        write_call(data, (symval + addend - addr) >> 2);
        return true;

    case MovwAbsNc:
        write_mov_abs(data, symval + addend);
        return true;

    case MovtAbs:
        write_mov_abs(data, (symval + addend) >> 16);
        return true;

    case ThumbMovwAbsNc:
        write_thumb_mov_abs(data, symval + addend);
        return true;

    case ThumbMovtAbs:
        write_thumb_mov_abs(data, (symval + addend) >> 16);
        return true;
    }

    LOG_WARN("Unhandled relocation code {}.", code);
    return true; // ignore unhandled relocations
}

bool relocate(const void *entries, uint32_t size, const SegmentInfosForReloc &segments, const MemState &mem, bool is_var_import, uint32_t explicit_symval) {
    const void *const end = static_cast<const uint8_t *>(entries) + size;
    const Entry *entry = static_cast<const Entry *>(entries);

    const auto segment_count = segments.size();

    // Range arithmetic is wider than a Vita address. Narrow only AFTER proving
    // the entire destination fits one mapped segment; symbol/addend/relocation
    // values themselves retain ARM's 32-bit arithmetic. Loader writes remain
    // intentionally unchecked with respect to guest permissions.
    const auto target_pointer = [&](uint64_t address, uint32_t bytes) -> uint8_t * {
        if (address >= vita3k::memory::guest_address_space_size
            || !vita3k::memory::guest_range_fits(static_cast<Address>(address), bytes)
            || !is_valid_addr_range(mem, static_cast<Address>(address), address + bytes))
            return nullptr;
        for (const auto &[_, segment] : segments) {
            if (address >= segment.addr && address - segment.addr <= segment.size
                && bytes <= segment.size - (address - segment.addr))
                return mem_guest_to_host(mem, static_cast<Address>(address));
        }
        return nullptr;
    };
    const auto relocate_guest = [&](uint64_t address, uint32_t code, uint32_t symbol, uint32_t addend) {
        if (code == None || code == V4BX || code == RBase)
            return true;
        auto *data = target_pointer(address, code == Abs8 ? 1 : 4);
        return data && relocate_entry(data, code, symbol, addend, static_cast<Address>(address));
    };

    if (LOG_RELOCATIONS) {
        LOG_DEBUG("Relocating patch of size: {}, # of segments: {}", log_hex(size), segment_count);
        for (const auto &seg : segments)
            LOG_DEBUG("    Segment: {} -> {} (size: {})", seg.first, log_hex(seg.second.addr), seg.second.size);
    }

    // initialized in format 1 and 2
    Address g_addr = 0, g_patchseg = 0;
    uint64_t g_offset = 0; // range accumulator, not a serialized Vita offset

    // initialized in format 0, 1, 2, and 3
    Address g_saddr = 0,
            g_addend = 0,
            g_type = 0,
            g_type2 = 0;

    const EntryFormatUnknown *generic_entry = nullptr;
    while (entry < end) {
        generic_entry = static_cast<const EntryFormatUnknown *>(entry);

        if (is_var_import)
            assert(generic_entry->format == 1 || generic_entry->format == 2);

        switch (generic_entry->format) {
        case 0: {
            const EntryFormat0 *const format0_entry = static_cast<const EntryFormat0 *>(entry);

            const auto symbol_seg = format0_entry->symbol_segment;
            const auto symbol_seg_start = segments.find(symbol_seg)->second.addr;
            const auto patch_seg = format0_entry->patch_segment;
            const auto patch_seg_start = segments.find(patch_seg)->second.addr;

            const Address s = (format0_entry->symbol_segment == 0xf) ? 0 : symbol_seg_start;
            const uint64_t p = uint64_t(patch_seg_start) + format0_entry->offset;
            const Address a = format0_entry->addend;

            LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT0]: offset: {}, code: {}, sym_seg: {}, sym_start: {}, patch_seg: {}, patch_start: {}, s: {}, p: {}, a: {}. {}",
                format0_entry->offset, format0_entry->code, symbol_seg, log_hex(symbol_seg_start), patch_seg, log_hex(patch_seg_start), log_hex(s), log_hex(p), log_hex(a), log_hex(reinterpret_cast<uintptr_t>(target_pointer(p, 1))));

            if (!relocate_guest(p, format0_entry->code, s, a)) {
                return false;
            }

            const uint64_t addr2 = p + format0_entry->dist2 * 2;

            if (format0_entry->code2 != 0) {
                LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT0/2]: code: {}, sym_seg: {}, sym_start: {}, s: {}, patch_seg: {}, p: {}, a: {}. {}",
                    format0_entry->code2, format0_entry->symbol_segment, symbol_seg_start, format0_entry->patch_segment, log_hex(patch_seg_start), log_hex(s), log_hex(addr2), log_hex(a), log_hex(reinterpret_cast<uintptr_t>(target_pointer(addr2, 1))));

                if (!relocate_guest(addr2, format0_entry->code2, s, a)) {
                    return false;
                }
            }

            g_addr = patch_seg_start;
            g_offset = format0_entry->offset;
            g_patchseg = format0_entry->patch_segment;
            g_saddr = s;
            g_addend = a;
            g_type = format0_entry->code;
            g_type2 = format0_entry->code2;

            break;
        }
        case 1: {
            if (!is_var_import) {
                const EntryFormat1 *const format1_entry = static_cast<const EntryFormat1 *>(entry);

                const auto symbol_seg = format1_entry->symbol_segment;
                const auto symbol_seg_start = segments.find(symbol_seg)->second.addr;
                const auto patch_seg = format1_entry->patch_segment;
                const auto patch_seg_start = segments.find(patch_seg)->second.addr;
                const Address s = (format1_entry->symbol_segment == 0xf) ? 0 : symbol_seg_start;

                const Address offset = format1_entry->offset_lo | (format1_entry->offset_hi << 12);
                const uint64_t p = uint64_t(patch_seg_start) + offset;
                const Address a = format1_entry->addend;

                LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT1]: code: {}, sym_seg: {}, sym_start: {}, patch_seg: {}, data_start: {}, s: {}, offset: {}, p: {}, a: {}",
                    format1_entry->code, symbol_seg, log_hex(symbol_seg_start), patch_seg, log_hex(patch_seg_start), log_hex(s), format1_entry->patch_segment, patch_seg_start, log_hex(offset), log_hex(p), log_hex(a));

                if (!relocate_guest(p, format1_entry->code, s, a)) {
                    return false;
                }

                g_addr = patch_seg_start;
                g_offset = offset;
                g_patchseg = format1_entry->patch_segment;
                g_saddr = s;
                g_addend = a;
                g_type = format1_entry->code;
                g_type2 = 0;

            } else {
                const EntryFormat1Alt *const format1_entry = static_cast<const EntryFormat1Alt *>(entry);

                const auto patch_seg = format1_entry->patch_segment;
                const auto patch_seg_start_it = segments.find(patch_seg);
                const Address s = explicit_symval;

                Address patch_seg_start{};
                if (patch_seg_start_it != segments.end())
                    patch_seg_start = patch_seg_start_it->second.addr;
                else {
                    LOG_WARN("[FORMAT1_VAR_IMPORT] patch segment {} not found. Skipping relocation. s: {} ", patch_seg, log_hex(s));
                    goto advance_entry;
                }

                const Address offset = format1_entry->offset;
                const uint64_t p = uint64_t(patch_seg_start) + offset;
                const Address a = format1_entry->addend;

                LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT1_VAR_IMPORT]: code: {}, patch_seg: {}, data_start: {}, s: {}, offset: {}, p: {}, a: {}",
                    format1_entry->code, patch_seg, log_hex(patch_seg_start), log_hex(s), format1_entry->patch_segment, patch_seg_start, log_hex(offset), log_hex(p), log_hex(a));

                if (!relocate_guest(p, format1_entry->code, s, a)) {
                    return false;
                }

                g_addr = patch_seg_start;
                g_offset = offset;
                g_patchseg = format1_entry->patch_segment;
                g_saddr = s;
                g_addend = a;
                g_type = format1_entry->code;
                g_type2 = 0;
            }

            break;
        }
        case 2: {
            if (!is_var_import) {
                const EntryFormat2 *const format2_entry = static_cast<const EntryFormat2 *>(entry);

                const auto symbol_seg = format2_entry->symbol_segment;
                const auto symbol_seg_start = segments.find(symbol_seg)->second.addr;

                g_offset += format2_entry->offset;
                g_saddr = (format2_entry->symbol_segment == 0xf) ? 0 : symbol_seg_start;
                g_addend = format2_entry->addend;
                g_type = format2_entry->code;

                const auto s = g_saddr;
                const auto a = g_addend;
                const auto p = g_addr + g_offset;

                LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT2]: code: {}, sym_seg: {}, sym_start: {}, offset: {}, s: {}, p: {}, a: {}",
                    format2_entry->code, symbol_seg, log_hex(symbol_seg_start), log_hex(format2_entry->offset), log_hex(s), log_hex(p), log_hex(a));

                if (!relocate_guest(p, g_type, s, a)) {
                    return false;
                }

                g_type2 = 0;
            } else {
                const EntryFormat2Alt *const format1_entry = static_cast<const EntryFormat2Alt *>(entry);

                const auto patch_seg = format1_entry->patch_segment;
                const auto patch_seg_start_it = segments.find(patch_seg);
                const Address s = explicit_symval;

                Address patch_seg_start{};
                if (patch_seg_start_it != segments.end())
                    patch_seg_start = patch_seg_start_it->second.addr;
                else {
                    LOG_WARN("[FORMAT2_VAR_IMPORT] patch segment {} not found. Skipping relocation. s: {} ", patch_seg, log_hex(s));
                    goto advance_entry;
                }

                const Address offset = format1_entry->offset;
                const uint64_t p = uint64_t(patch_seg_start) + offset;
                const Address a = format1_entry->addend;

                LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT2_VAR_IMPORT]: code: {}, patch_seg: {}, data_start: {}, s: {}, offset: {}, p: {}, a: {}",
                    format1_entry->code, patch_seg, log_hex(patch_seg_start), log_hex(s), format1_entry->patch_segment, patch_seg_start, log_hex(offset), log_hex(p), log_hex(a));

                if (!relocate_guest(p, format1_entry->code, s, a)) {
                    return false;
                }

                g_addr = patch_seg_start;
                g_offset = offset;
                g_patchseg = format1_entry->patch_segment;
                g_saddr = s;
                g_addend = a;
                g_type = format1_entry->code;
                g_type2 = 0;
            }

            break;
        }
        case 3: {
            const EntryFormat3 *const format3_entry = static_cast<const EntryFormat3 *>(entry);
            LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT3]: sym_seg: {}, mode: {} ({}), offset: {}, dist2: {}, addend: {}",
                log_hex(format3_entry->symbol_segment), format3_entry->mode, format3_entry->mode ? "THUMB" : "ARM", log_hex(format3_entry->offset), log_hex(format3_entry->dist2), log_hex(format3_entry->addend));

            const auto symbol_seg = format3_entry->symbol_segment;
            const auto symbol_seg_start = segments.find(symbol_seg)->second.addr;
            const Address s = (format3_entry->symbol_segment == 0xf) ? 0 : symbol_seg_start;
            const auto mode = format3_entry->mode;
            const auto offset = format3_entry->offset;
            const auto dist2 = format3_entry->dist2;

            if (mode == 1)
                g_type = ThumbMovwAbsNc;
            else if (mode == 0)
                g_type = MovwAbsNc;

            if (mode == 1)
                g_type2 = ThumbMovtAbs;
            else if (mode == 0)
                g_type2 = MovtAbs;

            g_offset += offset;
            g_saddr = s;
            g_addend = format3_entry->addend;

            const auto a = g_addend;
            const auto p = g_addr + g_offset;

            if (!relocate_guest(p, g_type, s, a)) {
                return false;
            }

            if (!relocate_guest(p + dist2, g_type2, s, a)) {
                return false;
            }

            break;
        }
        case 4: {
            const EntryFormat4 *const format4_entry = static_cast<const EntryFormat4 *>(entry);
            LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT4]: offset: {}, dist2: {}", log_hex(format4_entry->offset), log_hex(format4_entry->dist2));

            const auto offset = format4_entry->offset;
            const auto dist2 = format4_entry->dist2;

            g_offset += offset;

            const auto s = g_saddr;
            const auto a = g_addend;
            const auto p = g_addr + g_offset;

            if (!relocate_guest(p, g_type, s, a)) {
                return false;
            }

            if (!relocate_guest(p + dist2, g_type2, s, a)) {
                return false;
            }

            break;
        }
        case 5: {
            const EntryFormat5 *const format5_entry = static_cast<const EntryFormat5 *>(entry);

            g_offset += format5_entry->dist1;

            const auto s = g_saddr;
            const auto a = g_addend;
            const auto p = g_addr + g_offset;

            if (!relocate_guest(p, g_type, s, a)) {
                return false;
            }

            if (!relocate_guest(p + format5_entry->dist2, g_type2, s, a)) {
                return false;
            }

            g_offset += format5_entry->dist3;
            const auto p2 = g_addr + g_offset;

            if (!relocate_guest(p2, g_type, s, a)) {
                return false;
            }

            if (!relocate_guest(p2 + format5_entry->dist4, g_type2, s, a)) {
                return false;
            }

            break;
        }
        case 6: {
            const EntryFormat6 *const format6_entry = static_cast<const EntryFormat6 *>(entry);

            g_offset += format6_entry->offset;

            const auto patch_seg_start = segments.find(g_patchseg)->second.addr;

            const auto *original = target_pointer(uint64_t(patch_seg_start) + g_offset, 4);
            if (!original) return false;
            uint32_t orgval;
            std::memcpy(&orgval, original, sizeof(orgval));

            uint32_t segbase = 0;
            for (const auto &seg_ : segments) {
                const auto seg = seg_.second;
                if (orgval >= seg.p_vaddr && uint64_t(orgval) < uint64_t(seg.p_vaddr) + seg.size) {
                    segbase = seg.p_vaddr;
                    g_saddr = seg.addr;
                }
            }

            assert((uint32_t)orgval >= (uint32_t)segbase);
            const auto addend = orgval - segbase;

            g_type2 = 0;
            g_type = Abs32;

            const auto s = g_saddr;
            const auto a = addend;
            const auto p = g_addr + g_offset;

            if (!relocate_guest(p, g_type, s, a)) {
                return false;
            }

            break;
        }
        case 7:
        case 8:
        case 9: {
            const EntryFormat7_8_9 *const format7_8_9_entry = static_cast<const EntryFormat7_8_9 *>(entry);
            const auto format = format7_8_9_entry->format;
            uint32_t offsets = format7_8_9_entry->offsets;

            LOG_DEBUG_IF(LOG_RELOCATIONS, "[FORMAT{}]: offsets: {}", std::to_string(format), log_hex(offsets));

            uint32_t bitsize = 0;
            uint32_t mask = 0;
            // clang-format off
            switch (format) {
            case 7: bitsize = 7; mask = 0x7F; break;
            case 8: bitsize = 4; mask = 0x0F; break;
            case 9: bitsize = 2; mask = 0x03; break;
            }
            // clang-format on

            do {
                auto offset = (offsets & mask) * sizeof(uint32_t);
                g_offset += static_cast<Address>(offset);

                const auto patch_seg_start = segments.find(g_patchseg)->second.addr;

                const auto *original = target_pointer(uint64_t(patch_seg_start) + g_offset, 4);
                if (!original) return false;
                uint32_t orgval;
                std::memcpy(&orgval, original, sizeof(orgval));

                uint32_t segbase = 0;
                for (const auto &seg_ : segments) {
                    const auto seg = seg_.second;
                    if (orgval >= seg.p_vaddr && uint64_t(orgval) < uint64_t(seg.p_vaddr) + seg.size) {
                        segbase = seg.p_vaddr;
                        g_saddr = seg.addr;
                    }
                }

                assert((uint32_t)orgval >= (uint32_t)segbase);
                const auto addend = orgval - segbase;

                g_type2 = 0;
                g_type = Abs32;

                const auto s = g_saddr;
                const auto a = addend;
                const auto p = g_addr + g_offset;

                if (!relocate_guest(p, g_type, s, a)) {
                    return false;
                }
            } while (offsets >>= bitsize);

            break;
        }
        default: {
            LOG_WARN("Unknown relocation entry format {} ", generic_entry->format);
            return false;
        }
        }

    advance_entry:
        // clang-format off
        switch (generic_entry->format) {
        case 0: entry = static_cast<const EntryFormat0 *>(entry) + 1; break;
        case 1: entry = static_cast<const EntryFormat1 *>(entry) + 1; break;
        case 2: entry = static_cast<const EntryFormat2 *>(entry) + 1; break;
        case 3: entry = static_cast<const EntryFormat3 *>(entry) + 1; break;
        case 4: entry = static_cast<const EntryFormat4 *>(entry) + 1; break;
        case 5: entry = static_cast<const EntryFormat5 *>(entry) + 1; break;
        case 6: entry = static_cast<const EntryFormat6 *>(entry) + 1; break;
        case 7:
        case 8:
        case 9: entry = static_cast<const EntryFormat7_8_9 *>(entry) + 1; break;
        }
        // clang-format on
    }

    return true;
}
