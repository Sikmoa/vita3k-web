// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Browser-portable float32 -> float16 conversion. The native TU
// (vita3k/util/src/float_to_half.cpp) dispatches to x86 AVX+F16C code and
// cannot compile for WebAssembly; this is its scalar path verbatim
// (float_to_half_basic), over the same header-only util::encode_flt16, so
// converted uniform data is bit-identical. Needed by the selected GXM
// uniform-data setters (sceGxmSetUniformDataF).
#include <util/bytes.h>
#include <util/float_to_half.h>

void float_to_half(const float *src, std::uint16_t *dest, const int total) {
    for (int i = 0; i < total; i++) {
        dest[i] = util::encode_flt16(src[i]);
    }
}
