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

#include <module/guest_format.h>

#include <util/log.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace module {
namespace {

enum class Length {
    none,
    hh,
    h,
    l,
    ll,
    j,
    z,
    t,
    L,
};

// Formats one already-read value with the host's snprintf. `spec` names only host types
// whose width matches the value passed (int, unsigned, long long, double, const char *).
template <typename T>
bool append_host(std::string &out, const std::string &spec, T value) {
    const int size = std::snprintf(nullptr, 0, spec.c_str(), value);
    if (size < 0)
        return false;
    const std::size_t start = out.size();
    out.resize(start + static_cast<std::size_t>(size) + 1);
    std::snprintf(out.data() + start, static_cast<std::size_t>(size) + 1, spec.c_str(), value);
    out.resize(start + static_cast<std::size_t>(size));
    return true;
}

// Width or precision digits; nullopt when they overflow int, which C leaves undefined.
std::optional<int> parse_decimal(const char *&p) {
    long long value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p++ - '0');
        if (value > INT_MAX)
            return std::nullopt;
    }
    return static_cast<int>(value);
}

template <typename T>
bool store_count(MemState &mem, Address address, std::size_t count) {
    T *const target = Ptr<T>(address).get(mem);
    if (!target)
        return false;
    *target = static_cast<T>(count);
    return true;
}

} // namespace

std::optional<std::string> format_guest(const char *format, CPUState &cpu, MemState &mem, vargs &args) {
    if (!format) {
        LOG_ERROR("Guest format string is null");
        return std::nullopt;
    }
    std::string out;
    const char *p = format;
    while (*p) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const char *const spec_begin = p++;
        const auto fail = [&](const char *reason) -> std::optional<std::string> {
            const std::string_view spec(spec_begin, static_cast<std::size_t>(p - spec_begin));
            LOG_ERROR("Guest format \"{}\": {} in conversion \"{}\"", format, reason, spec);
            return std::nullopt;
        };

        std::string flags;
        while (*p && std::strchr("-+ #0", *p))
            flags += *p++;

        std::optional<int> width;
        if (*p == '*') {
            ++p;
            const int32_t value = args.next<int32_t>(cpu, mem);
            if (value == INT32_MIN)
                return fail("width out of range");
            // A negative width argument is a '-' flag followed by a positive width.
            if (value < 0)
                flags += '-';
            width = value < 0 ? -value : value;
        } else if (*p >= '1' && *p <= '9') {
            width = parse_decimal(p);
            if (!width)
                return fail("width out of range");
        }

        std::optional<int> precision;
        if (*p == '.') {
            ++p;
            if (*p == '*') {
                ++p;
                const int32_t value = args.next<int32_t>(cpu, mem);
                // A negative precision argument is taken as if the precision were omitted.
                if (value >= 0)
                    precision = value;
            } else {
                precision = parse_decimal(p);
                if (!precision)
                    return fail("precision out of range");
            }
        }

        Length length = Length::none;
        switch (*p) {
        case 'h':
            ++p;
            length = *p == 'h' ? (++p, Length::hh) : Length::h;
            break;
        case 'l':
            ++p;
            length = *p == 'l' ? (++p, Length::ll) : Length::l;
            break;
        case 'j': ++p, length = Length::j; break;
        case 'z': ++p, length = Length::z; break;
        case 't': ++p, length = Length::t; break;
        case 'L': ++p, length = Length::L; break;
        default: break;
        }

        const char conversion = *p;
        if (!conversion)
            return fail("unterminated conversion");
        ++p;

        std::string host_spec = "%" + flags;
        if (width)
            host_spec += std::to_string(*width);
        if (precision)
            host_spec += "." + std::to_string(*precision);

        // Guest (ARM EABI) widths: long, size_t and ptrdiff_t are 32-bit like int;
        // long long and intmax_t are 64-bit.
        const bool integer_64 = length == Length::ll || length == Length::j;
        const char *const narrow = length == Length::hh ? "hh" : length == Length::h ? "h" : "";

        bool ok = true;
        switch (conversion) {
        case 'd':
        case 'i':
            if (length == Length::L)
                return fail("invalid length modifier");
            if (integer_64)
                ok = append_host<long long>(out, host_spec + "ll" + conversion, args.next<int64_t>(cpu, mem));
            else
                ok = append_host<int>(out, host_spec + narrow + conversion, args.next<int32_t>(cpu, mem));
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (length == Length::L)
                return fail("invalid length modifier");
            if (integer_64)
                ok = append_host<unsigned long long>(out, host_spec + "ll" + conversion, args.next<uint64_t>(cpu, mem));
            else
                ok = append_host<unsigned>(out, host_spec + narrow + conversion, args.next<uint32_t>(cpu, mem));
            break;
        case 'c':
            if (length != Length::none)
                return fail("unsupported wide character");
            ok = append_host<int>(out, host_spec + 'c', args.next<int32_t>(cpu, mem));
            break;
        case 's': {
            if (length != Length::none)
                return fail("unsupported wide string");
            const Ptr<const char> string = args.next<Ptr<const char>>(cpu, mem);
            const char *const host = string.get(mem);
            if (string.address() && !host)
                return fail("string argument is not guest memory");
            ok = append_host<const char *>(out, host_spec + 's', host ? host : "(null)");
            break;
        }
        case 'p': {
            if (length != Length::none)
                return fail("invalid length modifier");
            // A guest pointer is 32-bit: eight uppercase hex digits unless a precision says otherwise.
            if (!precision)
                host_spec += ".8";
            ok = append_host<unsigned>(out, host_spec + 'X', args.next<Ptr<const void>>(cpu, mem).address());
            break;
        }
        case 'n': {
            const Address target = args.next<Ptr<void>>(cpu, mem).address();
            bool stored = false;
            switch (length) {
            case Length::hh: stored = store_count<int8_t>(mem, target, out.size()); break;
            case Length::h: stored = store_count<int16_t>(mem, target, out.size()); break;
            case Length::ll:
            case Length::j: stored = store_count<int64_t>(mem, target, out.size()); break;
            case Length::L: return fail("invalid length modifier");
            default: stored = store_count<int32_t>(mem, target, out.size()); break;
            }
            if (!stored)
                return fail("count target is not guest memory");
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            // Variadic floats are promoted to double; the guest's long double is double.
            if (length != Length::none && length != Length::l && length != Length::L)
                return fail("invalid length modifier");
            ok = append_host<double>(out, host_spec + conversion, args.next<double>(cpu, mem));
            break;
        case '%':
            out += '%';
            break;
        default:
            return fail("unsupported conversion");
        }
        if (!ok)
            return fail("host formatting failed");
    }
    return out;
}

int snprintf_guest(char *buffer, std::uint32_t count, const char *format, CPUState &cpu, MemState &mem, vargs &args) {
    if (!buffer && count) {
        LOG_ERROR("Guest snprintf into a null buffer of size {}", count);
        return -1;
    }
    const auto text = format_guest(format, cpu, mem, args);
    if (!text || text->size() > INT_MAX)
        return -1;
    if (count) {
        const std::size_t stored = std::min<std::size_t>(text->size(), count - 1);
        std::memcpy(buffer, text->data(), stored);
        buffer[stored] = '\0';
    }
    return static_cast<int>(text->size());
}

} // namespace module
