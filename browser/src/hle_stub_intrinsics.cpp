// SPDX-License-Identifier: GPL-2.0-or-later
// HLE stub intrinsics: a few GXM functions only read one field of a structure
// the HLE itself laid out, yet titles call them thousands of times per frame
// (Persona 4 Golden's field: ~4,000 calls per frame to the four below). Each
// HLE call leaves the JIT and re-enters it, so their stubs are replaced with
// the equivalent ARM code, which the JIT compiles into the caller's region.
// VITA3K_HLE_INTRINSICS=0 keeps the plain HLE stubs (for A/B measurements).
#include "hle_stub_intrinsics.h"

#include <gxm/types.h>
#include <kernel/state.h>
#include <renderer/gxm_types.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {
// A32, condition AL.
constexpr uint32_t ldr_r0_r0(uint32_t offset) { return 0xe5900000u | offset; } // ldr r0, [r0, #offset]
constexpr uint32_t ldrb_r0_r0(uint32_t offset) { return 0xe5d00000u | offset; } // ldrb r0, [r0, #offset]
constexpr uint32_t and_r0_15 = 0xe200000fu; // and r0, r0, #15
constexpr uint32_t lsr_r0_4 = 0xe1a00220u; // mov r0, r0, lsr #4
constexpr uint32_t bx_lr = 0xe12fff1eu;

constexpr uint32_t kGetArraySize = 0xdba8d061; // sceGxmProgramParameterGetArraySize
constexpr uint32_t kGetComponentCount = 0xbd2998d1; // sceGxmProgramParameterGetComponentCount
constexpr uint32_t kVertexDefaultUniformBuffer = 0xbe5a68ef; // sceGxmPrecomputedVertexStateGetDefaultUniformBuffer
constexpr uint32_t kFragmentDefaultUniformBuffer = 0xcecb584a; // sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer

static_assert(sizeof(UniformBuffer) == 4 && SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX == 0);
static_assert(offsetof(SceGxmPrecomputedVertexState, uniform_buffers) < 4096);
static_assert(offsetof(SceGxmPrecomputedFragmentState, uniform_buffers) < 4096);
static_assert(offsetof(SceGxmProgramParameter, array_size) < 4096 && sizeof(SceGxmProgramParameter::array_size) == 4);

// component_count is a bitfield: find its byte and nibble from the compiler's
// own layout instead of assuming one.
bool component_count_location(uint32_t &byte, bool &high) {
    SceGxmProgramParameter parameter;
    std::memset(&parameter, 0, sizeof(parameter));
    parameter.component_count = 0xf;
    const auto *bytes = reinterpret_cast<const uint8_t *>(&parameter);
    for (uint32_t i = 0; i < sizeof(parameter); ++i) {
        if (!bytes[i])
            continue;
        if (bytes[i] != 0x0f && bytes[i] != 0xf0)
            return false;
        byte = i;
        high = bytes[i] == 0xf0;
        return true;
    }
    return false;
}
} // namespace

namespace browser {
void install_hle_stub_intrinsics(KernelState &kernel) {
    if (const char *option = std::getenv("VITA3K_HLE_INTRINSICS"); option && std::strcmp(option, "0") == 0)
        return;
    uint32_t count_byte = 0;
    bool count_high = false;
    const bool count_known = component_count_location(count_byte, count_high);
    kernel.patch_hle_stub = [count_known, count_byte, count_high](uint32_t nid, uint32_t *stub) {
        switch (nid) {
        case kGetArraySize:
            stub[0] = ldr_r0_r0(offsetof(SceGxmProgramParameter, array_size));
            stub[1] = bx_lr;
            break;
        case kGetComponentCount:
            if (!count_known)
                return;
            stub[0] = ldrb_r0_r0(count_byte);
            stub[1] = count_high ? lsr_r0_4 : and_r0_15;
            stub[2] = bx_lr;
            break;
        case kVertexDefaultUniformBuffer:
            stub[0] = ldr_r0_r0(offsetof(SceGxmPrecomputedVertexState, uniform_buffers));
            stub[1] = ldr_r0_r0(0);
            stub[2] = bx_lr;
            break;
        case kFragmentDefaultUniformBuffer:
            stub[0] = ldr_r0_r0(offsetof(SceGxmPrecomputedFragmentState, uniform_buffers));
            stub[1] = ldr_r0_r0(0);
            stub[2] = bx_lr;
            break;
        default:
            break;
        }
    };
}
} // namespace browser
