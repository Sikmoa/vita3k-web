#include <nids/functions.h>

#include <cstdio>
#include <cstring>

struct Entry {
    uint32_t nid;
    const char *name;
};

static constexpr Entry entries[] = {
#define NID(name, nid) { nid, #name },
#define VAR_NID(name, nid) { nid, #name },
#include <nids/nids.inc>
#undef VAR_NID
#undef NID
#define KNOWN_NID(name, nid) { nid, name },
#include <nids/known_nids.inc>
#undef KNOWN_NID
};

struct Alias {
    uint32_t nid;
    uint32_t library;
    const char *name;
};

static constexpr Alias aliases[] = {
#define NID_ALIAS(name, nid, library) { nid, library, name },
#include <nids/nid_aliases.inc>
#undef NID_ALIAS
};

static bool check(const char *actual, const char *expected, uint32_t nid) {
    if (std::strcmp(actual, expected) == 0)
        return true;
    std::fprintf(stderr, "NID %08X: expected %s, got %s\n", nid, expected, actual);
    return false;
}

int main() {
    bool ok = true;
    for (const auto &entry : entries) {
        ok &= check(import_name(entry.nid), entry.name, entry.nid);
        ok &= check(import_name(entry.nid, 0), entry.name, entry.nid);
    }
    for (const auto &alias : aliases)
        ok &= check(import_name(alias.nid, alias.library), alias.name, alias.nid);

    // These source conflicts must not rename an established HLE export.
    ok &= check(import_name(0x58ABAD62, 0xD46680E4), "pss_errno_loc", 0x58ABAD62);
    ok &= check(import_name(0), "UNRECOGNISED", 0);
    ok &= check(import_name(0xFFFFFFFF, 0xFFFFFFFF), "UNRECOGNISED", 0xFFFFFFFF);
    if (ok)
        std::printf("Checked %zu NID names, %zu library aliases, and unknown-NID fallback\n",
            sizeof(entries) / sizeof(entries[0]), sizeof(aliases) / sizeof(aliases[0]));
    return ok ? 0 : 1;
}
