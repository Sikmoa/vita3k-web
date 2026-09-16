// SPDX-License-Identifier: GPL-2.0-or-later
// Reuse Vita toolchain SHA-256 without bringing OpenSSL into the browser.
#include <util/hash.h>
extern "C" {
#include <utils/sha256.h>
}
#include <algorithm>
#include <limits>
Sha256Hash sha256(const void *data, size_t size) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    auto bytes = static_cast<const uint8_t *>(data);
    while (size) {
        const auto count = static_cast<uint32_t>(std::min(size, size_t(UINT32_MAX)));
        sha256_update(&ctx, const_cast<uint8_t *>(bytes), count);
        bytes += count;
        size -= count;
    }
    Sha256Hash result;
    sha256_final(&ctx, result.data());
    return result;
}
