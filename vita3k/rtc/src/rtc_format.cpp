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

#include <rtc/rtc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Firmware 3.74 date/time rules. Dates convert with proleptic Gregorian day
// arithmetic, so out-of-range fields roll over (day 0 is the last day of the
// previous month, hour 25 the next day), as the firmware's conversion does.
namespace {
constexpr std::int64_t usec_per_day = 86400LL * VITA_CLOCKS_PER_SEC;

std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t &y, unsigned &m, unsigned &d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}

constexpr std::int64_t epoch_days = 719162; // 0001-01-01 to 1970-01-01

// Local date/time fields to a tick, rolling over out-of-range day/hour/minute.
std::uint64_t fields_to_tick(std::int64_t year, int month, std::int64_t day, std::int64_t hour, std::int64_t minute, std::int64_t second, std::int64_t usec) {
    const std::int64_t days = days_from_civil(year, month, 1) + epoch_days + (day - 1);
    return static_cast<std::uint64_t>(days * usec_per_day + ((hour * 60 + minute) * 60 + second) * VITA_CLOCKS_PER_SEC + usec);
}

int lower(char c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : static_cast<unsigned char>(c);
}

// strncasecmp for ASCII: the strings compared here are ASCII names.
bool starts_with_nocase(const char *text, const char *prefix, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (lower(text[i]) != lower(prefix[i]))
            return false;
    }
    return true;
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Exactly `count` digits.
bool digits(const char *&p, int count, int &value) {
    value = 0;
    for (int i = 0; i < count; ++i) {
        if (!is_digit(p[i]))
            return false;
        value = value * 10 + (p[i] - '0');
    }
    p += count;
    return true;
}

// One or two digits.
bool digits12(const char *&p, int &value) {
    if (!is_digit(*p))
        return false;
    value = *p++ - '0';
    if (is_digit(*p))
        value = value * 10 + (*p++ - '0');
    return true;
}

bool match_name(const char *&p, const char *name) {
    // The full name or its first three letters, case-insensitively.
    const size_t full = strlen(name);
    if (starts_with_nocase(p, name, full)) {
        p += full;
        return true;
    }
    if (starts_with_nocase(p, name, 3)) {
        p += 3;
        return true;
    }
    return false;
}

constexpr const char *month_names[12] = { "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December" };
constexpr const char *weekday_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

bool parse_month(const char *&p, int &month) {
    for (int i = 0; i < 12; ++i) {
        if (match_name(p, month_names[i])) {
            month = i + 1;
            return true;
        }
    }
    return false;
}

// Time-zone abbreviations of firmware 3.74 driver_us (table at 0x8100d880):
// the first case-insensitive prefix match wins.
struct ZoneName {
    const char *name;
    int minutes;
};
constexpr ZoneName zone_names[] = { { "GMT", 0 }, { "EST", -300 }, { "EDT", -240 }, { "CST", -360 }, { "CDT", -300 },
    { "MST", -420 }, { "MDT", -360 }, { "PST", -480 }, { "PDT", -420 }, { "NZDT", 780 }, { "NZST", 720 }, { "IDLE", 720 },
    { "NZT", 720 }, { "AESST", 660 }, { "ACSST", 630 }, { "CADT", 630 }, { "SADT", 630 }, { "AEST", 600 }, { "EAST", 600 },
    { "GST", 600 }, { "LIGT", 600 }, { "ACST", 570 }, { "SAST", 570 }, { "CAST", 570 }, { "AWSST", 540 }, { "JST", 540 },
    { "KST", 540 }, { "WDT", 540 }, { "MT", 510 }, { "AWST", 480 }, { "CCT", 480 }, { "WADT", 480 }, { "WST", 480 },
    { "JT", 450 }, { "WAST", 420 }, { "IT", 210 }, { "BT", 180 }, { "EETDST", 180 }, { "EET", 120 }, { "CETDST", 120 },
    { "FWT", 120 }, { "IST", 120 }, { "MEST", 120 }, { "METDST", 120 }, { "SST", 120 }, { "BST", 60 }, { "CET", 60 },
    { "DNT", 60 }, { "FST", 60 }, { "MET", 60 }, { "MEWT", 60 }, { "MEZ", 60 }, { "NOR", 60 }, { "SET", 60 }, { "SWT", 60 },
    { "WETDST", 60 }, { "WET", 0 }, { "WAT", -60 }, { "NDT", -90 }, { "ADT", -180 }, { "NFT", -150 }, { "NST", -150 },
    { "AST", -240 }, { "YDT", -480 }, { "HDT", -540 }, { "YST", -540 }, { "AHST", -600 }, { "CAT", -600 }, { "NT", -660 },
    { "IDLW", -720 } };

// RFC 1123/850 zone: "+HHMM", U/T forms, an abbreviation or a military letter.
int parse_zone(const char *p, int &minutes) {
    minutes = 0;
    if (*p == '+' || *p == '-') {
        int value;
        const char *q = p + 1;
        if (digits(q, 4, value))
            minutes = (*p == '-' ? -1 : 1) * (value / 100 * 60 + value % 100);
        return 0; // without four digits the offset is silently 0
    }
    if (p[0] == 'U' || (p[0] && p[1] == 'T'))
        return 0;
    for (const auto &zone : zone_names) {
        if (starts_with_nocase(p, zone.name, strlen(zone.name))) {
            minutes = zone.minutes;
            return 0;
        }
    }
    // Military letters as the firmware maps them: A..I and K..M are
    // +(c - 'A') hours, N..Y are -(c - 'N') hours.
    const char c = static_cast<char>(lower(*p) - 'a' + 'A');
    if (c >= 'A' && c <= 'M' && c != 'J')
        minutes = (c - 'A') * 60;
    else if (c >= 'N' && c <= 'Y')
        minutes = -(c - 'N') * 60;
    else if (c == 'Z')
        minutes = 0;
    else
        return SCE_RTC_ERROR_BAD_PARSE;
    return 0;
}
} // namespace

int rtc_check_valid(const SceDateTime *date) {
    if (!date)
        return SCE_RTC_ERROR_INVALID_POINTER;
    if (date->year < 1 || date->year > 9999)
        return SCE_RTC_ERROR_INVALID_YEAR;
    if (date->month < 1 || date->month > 12)
        return SCE_RTC_ERROR_INVALID_MONTH;
    static constexpr unsigned month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const bool leap = date->year % 4 == 0 && (date->year % 100 != 0 || date->year % 400 == 0);
    const unsigned days = month_days[date->month - 1] + (date->month == 2 && leap);
    if (date->day < 1 || date->day > days)
        return SCE_RTC_ERROR_INVALID_DAY;
    if (date->hour > 23)
        return SCE_RTC_ERROR_INVALID_HOUR;
    if (date->minute > 59)
        return SCE_RTC_ERROR_INVALID_MINUTE;
    if (date->second > 59)
        return SCE_RTC_ERROR_INVALID_SECOND;
    if (date->microsecond > 999999)
        return SCE_RTC_ERROR_INVALID_MICROSECOND;
    return 0;
}

int rtc_format_rfc3339(char *out, std::uint64_t utc_tick, int offset_minutes) {
    if (!out)
        return SCE_RTC_ERROR_INVALID_POINTER;
    if (offset_minutes < -1439 || offset_minutes > 1439)
        return SCE_RTC_ERROR_INVALID_VALUE;
    const std::int64_t local = static_cast<std::int64_t>(utc_tick) + static_cast<std::int64_t>(offset_minutes) * 60 * VITA_CLOCKS_PER_SEC;
    if (local < 0)
        return SCE_RTC_ERROR_INVALID_VALUE;
    std::int64_t year;
    unsigned month, day;
    civil_from_days(local / usec_per_day - epoch_days, year, month, day);
    const std::int64_t in_day = local % usec_per_day;
    const auto seconds = in_day / VITA_CLOCKS_PER_SEC;
    const int written = snprintf(out, 32, "%04d-%02u-%02uT%02d:%02d:%02d.%02d", static_cast<int>(year % 10000), month, day,
        static_cast<int>(seconds / 3600), static_cast<int>(seconds / 60 % 60), static_cast<int>(seconds % 60),
        static_cast<int>(in_day % VITA_CLOCKS_PER_SEC / 10000));
    if (offset_minutes == 0)
        snprintf(out + written, 32 - written, "Z");
    else
        snprintf(out + written, 32 - written, "%c%02d:%02d", offset_minutes < 0 ? '-' : '+', std::abs(offset_minutes) / 60, std::abs(offset_minutes) % 60);
    return 0;
}

int rtc_parse_rfc3339(std::uint64_t *utc_tick, const char *text) {
    if (!utc_tick || !text)
        return SCE_RTC_ERROR_INVALID_POINTER;
    const char *p = text;
    int year, month, day, hour, minute, second;
    if (!digits(p, 4, year) || *p++ != '-' || !digits(p, 2, month) || *p++ != '-' || !digits(p, 2, day)
        || (*p != 'T' && *p != 't') || !digits(++p, 2, hour) || *p++ != ':' || !digits(p, 2, minute) || *p++ != ':'
        || !digits(p, 2, second))
        return SCE_RTC_ERROR_BAD_PARSE;
    // A fraction keeps its first six digits as microseconds.
    int usec = 0;
    if (*p == '.') {
        int count = 0;
        for (++p; is_digit(*p); ++p) {
            if (count < 6) {
                usec = usec * 10 + (*p - '0');
                ++count;
            }
        }
        for (; count > 0 && count < 6; ++count)
            usec *= 10;
    }
    int offset = 0;
    if (*p == 'Z' || *p == 'z') {
        offset = 0;
    } else if (*p == '+' || *p == '-') {
        const char sign = *p++;
        int oh, om;
        if (!digits(p, 2, oh) || *p++ != ':' || !digits(p, 2, om))
            return SCE_RTC_ERROR_BAD_PARSE;
        offset = (sign == '-' ? -1 : 1) * (oh * 60 + om);
    } else {
        return SCE_RTC_ERROR_BAD_PARSE;
    }
    SceDateTime date{ static_cast<unsigned short>(year), static_cast<unsigned short>(month), static_cast<unsigned short>(day),
        static_cast<unsigned short>(hour), static_cast<unsigned short>(minute), static_cast<unsigned short>(second == 60 ? 59 : second),
        static_cast<unsigned int>(usec) };
    if (const int error = rtc_check_valid(&date))
        return error;
    *utc_tick = fields_to_tick(year, month, day, hour, minute, second, usec) - static_cast<std::uint64_t>(static_cast<std::int64_t>(offset) * 60 * VITA_CLOCKS_PER_SEC);
    return 0;
}

int rtc_parse_date_time(std::uint64_t *utc_tick, const char *text) {
    if (!utc_tick || !text)
        return SCE_RTC_ERROR_INVALID_POINTER;
    const char *p = text;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (is_digit(p[0]) && is_digit(p[1]) && is_digit(p[2]) && is_digit(p[3]))
        return rtc_parse_rfc3339(utc_tick, p);
    bool weekday = false;
    for (const char *name : weekday_names) {
        if (match_name(p, name)) {
            weekday = true;
            break;
        }
    }
    if (!weekday)
        return SCE_RTC_ERROR_BAD_PARSE;
    if (*p == ',')
        ++p;
    while (*p == ' ' || *p == '\t')
        ++p;
    int year, month, day, hour, minute, second = 0, offset = 0;
    if (parse_month(p, month)) {
        // asctime: "Mon DD HH:MM:SS YYYY", always UTC, fields unchecked.
        if (*p++ != ' ')
            return SCE_RTC_ERROR_BAD_PARSE;
        if (*p == ' ')
            ++p;
        if (!digits12(p, day) || *p++ != ' ' || !digits12(p, hour) || *p++ != ':' || !digits12(p, minute) || *p++ != ':'
            || !digits12(p, second) || *p++ != ' ' || !digits(p, 4, year))
            return SCE_RTC_ERROR_BAD_PARSE;
    } else {
        // RFC 1123 "DD Mon YYYY HH:MM[:SS] zone" or RFC 850 "DD-Mon-YY ...".
        if (!digits12(p, day) || (*p != ' ' && *p != '-'))
            return SCE_RTC_ERROR_BAD_PARSE;
        ++p;
        if (!parse_month(p, month) || (*p != ' ' && *p != '-'))
            return SCE_RTC_ERROR_BAD_PARSE;
        ++p;
        const char *q = p;
        if (digits(q, 4, year)) {
            p = q;
        } else if (digits(p, 2, year)) {
            year += year <= 49 ? 2000 : 1900;
        } else {
            return SCE_RTC_ERROR_BAD_PARSE;
        }
        if (*p++ != ' ' || !digits12(p, hour) || hour > 25 || *p++ != ':' || !digits12(p, minute))
            return SCE_RTC_ERROR_BAD_PARSE;
        if (*p == ':') {
            ++p;
            if (!digits12(p, second))
                return SCE_RTC_ERROR_BAD_PARSE;
        }
        if (p[0] == ' ' && p[1] != ' ') {
            if (const int error = parse_zone(p + 1, offset))
                return error;
        }
    }
    *utc_tick = fields_to_tick(year, month, day, hour, minute, second, 0) - static_cast<std::uint64_t>(static_cast<std::int64_t>(offset) * 60 * VITA_CLOCKS_PER_SEC);
    return 0;
}
