// Guest printf family, SceClib string helpers and SceDbg handlers through the
// production bridges. Argument words are laid out by hand per the ARM EABI
// (AAPCS: r0-r3, then the stack; 8-byte values in an even register pair or at
// an 8-byte aligned stack slot), independently of the code under test.
#pragma once
#include <module/guest_format.h>
#include <util/align.h>
#include <emscripten/heap.h>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace guest_clib_test {
constexpr uint32_t kPrintf = 0xFA26BC62, kVprintf = 0x5EA3B6CE, kSnprintf = 0x8CBA03D5,
                   kVsnprintf = 0xFA6BE467, kStrlcpy = 0x2CDFCD1C, kStrlcat = 0x70CBC2D5,
                   kStrtoll = 0x2E581B88, kAssert = 0x1AF3678B, kLog = 0x6605AB19;
inline uint32_t lo(uint64_t v) { return static_cast<uint32_t>(v); }
inline uint32_t hi(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
inline uint64_t bits(double v) { return std::bit_cast<uint64_t>(v); }
} // namespace guest_clib_test

inline void test_guest_clib(EmuEnvState &env, ThreadState &thread) {
    using namespace guest_clib_test;
    auto &cpu = *thread.cpu;
    const Address page = alloc(env.mem, 8192, "clib fixture");
    REQUIRE(page);
    Address next = page;
    auto put = [&](const char *text) {
        const Address address = next;
        std::strcpy(Ptr<char>(address).get(env.mem), text);
        next = align(next + static_cast<Address>(std::strlen(text)) + 1, 8);
        return address;
    };
    auto words = [&](const std::vector<uint32_t> &values) {
        const Address address = next;
        for (size_t i = 0; i < values.size(); ++i)
            Ptr<uint32_t>(address + 4 * i).get(env.mem)[0] = values[i];
        next = align(next + static_cast<Address>(4 * values.size()), 8);
        return address;
    };
    const Address out = page + 0x1800, cell = page + 0x1f00;
    auto text = [&](Address address) { return std::string(Ptr<const char>(address).get(env.mem)); };
    auto word = [&](Address address) -> uint32_t & { return *Ptr<uint32_t>(address).get(env.mem); };
    // r0-r3, then the caller's outgoing stack words at an 8-byte aligned SP
    // (the thread has not started, so its own SP is not a usable stack yet).
    auto call64 = [&](uint32_t nid, std::vector<uint32_t> args) -> uint64_t {
        const uint32_t saved_sp = read_sp(cpu);
        const uint32_t sp = page + 0x1400;
        std::memset(Ptr<void>(sp).get(env.mem), 0xcc, 0x100);
        write_sp(cpu, sp);
        for (size_t i = 0; i < args.size(); ++i) {
            if (i < 4)
                write_reg(cpu, i, args[i]);
            else
                word(sp + 4 * (i - 4)) = args[i];
        }
        call_import(env, cpu, nid, thread.id);
        REQUIRE(env.missing_nids.empty());
        REQUIRE(read_sp(cpu) == sp);
        write_sp(cpu, saved_sp);
        return read_reg(cpu, 0) | uint64_t(read_reg(cpu, 1)) << 32;
    };
    auto call = [&](uint32_t nid, std::vector<uint32_t> args) { return lo(call64(nid, std::move(args))); };
    const uint32_t invalid_argument = static_cast<uint32_t>(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
    auto check = [&](const char *format, std::vector<uint32_t> varargs, const char *expected) {
        std::memset(Ptr<void>(out).get(env.mem), 0xcc, 256);
        std::vector<uint32_t> args = { out, 256, put(format) };
        args.insert(args.end(), varargs.begin(), varargs.end());
        const int32_t length = static_cast<int32_t>(call(kSnprintf, args));
        if (text(out) != expected || length != static_cast<int32_t>(std::strlen(expected))) {
            std::fprintf(stderr, "sceClibSnprintf(\"%s\") -> %d \"%s\", expected \"%s\"\n", format, length, text(out).c_str(), expected);
            REQUIRE(false);
        }
    };
    const Address hello = put("hello");

    // Varargs start at r3. Every length modifier, each followed by a %d
    // sentinel that is only right when the previous argument had its guest size.
    check("%d|%i|%u", { lo(-5), 7, 4000000000u }, "-5|7|4000000000");
    check("%ld %lu %lx %li|%d", { lo(-2), 4000000000u, 0xdeadbeef, lo(-9), 99 }, "-2 4000000000 deadbeef -9|99");
    check("%zu %zd %zx %tu %td|%d", { 4000000000u, lo(-3), 0xabcu, 12, lo(-12), 99 }, "4000000000 -3 abc 12 -12|99");
    check("%hhd %hd %hhu %hu %hx|%d", { 0x1ff, 0x18000, 0x1ff, 0x18000, 0x12345, 99 }, "-1 -32768 255 32768 2345|99");
    check("%o %X %#x %#o|%d", { 8, 0xbeef, 0x1f, 8, 99 }, "10 BEEF 0x1f 010|99");
    // r3 is skipped: a 64-bit argument needs an even register pair, so it goes
    // to the stack, and later 32-bit arguments do not back-fill r3.
    check("%lld|%d", { 0xdeadbeef, lo(-5000000000ll), hi(-5000000000ll), 99 }, "-5000000000|99");
    // Stack slot of a 64-bit argument is 8-byte aligned: one padding word.
    check("%d %d %llu %d", { 1, 2, 0xdeadbeef, lo(18000000000000000000ull), hi(18000000000000000000ull), 3 },
        "1 2 18000000000000000000 3");
    check("%d %llx %jd %ju", { 1, lo(0x123456789abull), hi(0x123456789abull),
                                  lo(-7ll), hi(-7ll), lo(9000000000ull), hi(9000000000ull) },
        "1 123456789ab -7 9000000000");
    check("%f|%.2f|%e|%g|%Lf|%lf|%d", { 0xdeadbeef, lo(bits(1.5)), hi(bits(1.5)), lo(bits(3.14159)), hi(bits(3.14159)),
                                          lo(bits(12345.678)), hi(bits(12345.678)), lo(bits(0.0001)), hi(bits(0.0001)),
                                          lo(bits(-2.25)), hi(bits(-2.25)), lo(bits(8.0)), hi(bits(8.0)), 99 },
        "1.500000|3.14|1.234568e+04|0.0001|-2.250000|8.000000|99");
    // A guest pointer is 32-bit: eight digits on every host.
    check("%p %p|%d", { 0x81234560, 0x10, 99 }, "81234560 00000010|99");
    check("%s|%c|%%|%5d|%-5d|%05d|%+d|% d", { hello, 'x', 42, 42, 42, 42, 42 }, "hello|x|%|   42|42   |00042|+42| 42");
    check("%*d|%-*d|%.*s|%.3s|%s", { 4, 7, lo(-4), 7, 2, hello, hello, 0 }, "   7|7   |he|hel|(null)");
    check("", {}, "");

    // %n stores the count so far through a guest pointer, sized by its modifier.
    word(cell) = 0xcccccccc;
    word(cell + 4) = 0xcccccccc;
    word(cell + 8) = 0xcccccccc;
    check("abc%n%hhn|%d", { cell, cell + 8, 99 }, "abc|99");
    REQUIRE(word(cell) == 3 && word(cell + 4) == 0xcccccccc && word(cell + 8) == 0xcccccc03);

    // C snprintf contract: the untruncated length, and a terminated prefix.
    std::memset(Ptr<void>(out).get(env.mem), 0xcc, 16);
    REQUIRE(call(kSnprintf, { out, 4, put("abc%s"), hello }) == 8);
    REQUIRE(text(out) == "abc");
    REQUIRE(call(kSnprintf, { out, 0, put("%s!"), hello }) == 6);
    REQUIRE(text(out) == "abc");
    REQUIRE(call(kSnprintf, { 0, 0, put("%d"), 12345 }) == 5);
    // A huge field width is counted, not materialised in host memory.
    const size_t heap_before = emscripten_get_heap_size();
    REQUIRE(call(kSnprintf, { out, 4, put("%*s|%n"), 2000000000, hello, cell }) == 2000000001);
    REQUIRE(text(out) == "   " && word(cell) == 2000000001);
    REQUIRE(emscripten_get_heap_size() == heap_before);
    // Conversions it cannot perform faithfully fail instead of misreading arguments.
    REQUIRE(call(kSnprintf, { out, 256, put("%ls"), hello }) == invalid_argument);
    REQUIRE(call(kSnprintf, { out, 256, put("%b"), 1 }) == invalid_argument);
    REQUIRE(call(kSnprintf, { out, 256, put("%"), 1 }) == invalid_argument);

    // va_list forms: the list is a guest pointer; 8-byte arguments sit at
    // 8-byte aligned addresses within it.
    const Address list = words({ 5, 0xdeadbeef, lo(-6000000000ll), hi(-6000000000ll), lo(bits(0.5)), hi(bits(0.5)), hello, cell });
    REQUIRE(list % 8 == 0);
    std::memset(Ptr<void>(out).get(env.mem), 0xcc, 256);
    word(cell) = 0xcccccccc;
    REQUIRE(call(kVsnprintf, { out, 256, put("%d %lld %.1f %s%n"), list }) == 23);
    REQUIRE(text(out) == "5 -6000000000 0.5 hello");
    REQUIRE(word(cell) == 23);
    REQUIRE(call(kVsnprintf, { out, 3, put("%s"), words({ hello }) }) == 5);
    REQUIRE(text(out) == "he");
    // sceClibVprintf reads the va_list in r1, not r1 itself as the first argument.
    word(cell) = 0xcccccccc;
    REQUIRE(call(kVprintf, { put("abcd%n"), words({ cell }) }) == SCE_KERNEL_OK);
    REQUIRE(word(cell) == 4);
    word(cell) = 0xcccccccc;
    REQUIRE(call(kPrintf, { put("%lld%n"), 0xdeadbeef, 1, 0, cell }) == SCE_KERNEL_OK);
    REQUIRE(word(cell) == 1);

    // Register-pair doubles are read as bit patterns, not converted integers.
    write_reg(cpu, 1, 7);
    write_reg(cpu, 2, lo(bits(2.5)));
    write_reg(cpu, 3, hi(bits(2.5)));
    module::vargs from_r1(LayoutArgsState{ 1, 0, 0 });
    const auto formatted = module::format_guest("%d %.1f", cpu, env.mem, from_r1, 64);
    REQUIRE(formatted && formatted->text == "7 2.5" && formatted->length == 5);

    // BSD strlcpy: returns strlen(src); terminates whenever size > 0.
    std::memset(Ptr<void>(out).get(env.mem), 'z', 16);
    REQUIRE(call(kStrlcpy, { out, hello, 4 }) == 5);
    REQUIRE(text(out) == "hel");
    REQUIRE(call(kStrlcpy, { out, hello, 16 }) == 5);
    REQUIRE(text(out) == "hello");
    std::memset(Ptr<void>(out).get(env.mem), 'z', 16);
    REQUIRE(call(kStrlcpy, { out, hello, 0 }) == 5);
    REQUIRE(Ptr<char>(out).get(env.mem)[0] == 'z');
    // BSD strlcat: returns strlen(dst) + strlen(src) within size.
    std::strcpy(Ptr<char>(out).get(env.mem), "ab");
    REQUIRE(call(kStrlcat, { out, hello, 16 }) == 7);
    REQUIRE(text(out) == "abhello");
    std::strcpy(Ptr<char>(out).get(env.mem), "ab");
    REQUIRE(call(kStrlcat, { out, hello, 5 }) == 7);
    REQUIRE(text(out) == "abhe");
    std::memset(Ptr<void>(out).get(env.mem), 'z', 16);
    REQUIRE(call(kStrlcat, { out, hello, 4 }) == 9); // no NUL within size: size + strlen(src)
    REQUIRE(std::memcmp(Ptr<char>(out).get(env.mem), "zzzzzz", 6) == 0);

    // strtoll stores the guest address of the first unparsed character.
    const Address number = put("  -123xyz"), big = put("0x7fffffffffffffff!");
    word(cell) = 0xcccccccc;
    word(cell + 4) = 0xcccccccc;
    REQUIRE(call64(kStrtoll, { number, cell, 10 }) == uint64_t(-123ll));
    REQUIRE(word(cell) == number + 6 && word(cell + 4) == 0xcccccccc);
    REQUIRE(call64(kStrtoll, { big, cell, 0 }) == 0x7fffffffffffffffull);
    REQUIRE(word(cell) == big + 18);
    REQUIRE(call64(kStrtoll, { number, 0, 10 }) == uint64_t(-123ll));

    // sceDbg handlers: five named arguments, so the message and its arguments
    // are on the stack. The assertion handler reports and returns its third
    // argument; it never ends the process.
    bool exit_requested = false;
    auto saved_exit = env.kernel.process_exit_callback;
    env.kernel.process_exit_callback = [&](int, std::optional<AppLaunchRequest>) { exit_requested = true; };
    const Address file = put("fixture.c"), component = put("clib");
    for (uint32_t unk : { 0u, 1u }) {
        word(cell) = 0xcccccccc;
        REQUIRE(call(kAssert, { file, 12, unk, component, put("%s=%lld%n"), hello, lo(-1ll), hi(-1ll), cell }) == unk);
        REQUIRE(word(cell) == 8);
    }
    REQUIRE(!exit_requested);
    word(cell) = 0xcccccccc;
    REQUIRE(call(kLog, { file, 13, 2, component, put("%d %lld%n"), 4, lo(5ll), hi(5ll), cell }) == 0);
    REQUIRE(word(cell) == 3);
    // Output beyond 511 characters is truncated and reported as a negative result.
    REQUIRE(call(kLog, { file, 14, 2, component, put("%511d"), 1 }) == 0);
    REQUIRE(static_cast<int32_t>(call(kLog, { file, 15, 2, component, put("%512d"), 1 })) < 0);
    REQUIRE(call(kAssert, { file, 16, 1, component, put("%600d"), 1 }) == 1);
    env.kernel.process_exit_callback = saved_exit;

    free(env.mem, page);
    std::puts("Guest clib: printf length modifiers, va_list alignment, strlcpy/strlcat, strtoll and sceDbg handlers passed");
}
