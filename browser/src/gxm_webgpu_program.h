// SPDX-License-Identifier: GPL-2.0-or-later
// WebGPU-owned shader program state.
//
// renderer::create(FragmentProgram, ...) receives the guest blend descriptor
// exactly once, at sceGxmShaderPatcherCreateFragmentProgram time. The browser
// consumer needs it again on every draw, and the guest SceGxmBlendInfo pointer
// is not retained, so the descriptor is copied onto a WebGPU-owned
// FragmentProgram subclass in *guest* units. Translation to WebGPU happens
// where the scene stream is consumed (browser/web/gxm_scene.js), like the
// sampler and vertex format fields, so no WebGPU enum value is stored in state
// that guest code can observe.
#pragma once

#include <gxm/types.h>
#include <renderer/types.h>

#include <cstdint>

namespace browser {

// Guest blend descriptor, mirroring SceGxmBlendInfo field semantics without the
// bitfield layout (the scene stream carries plain u32 words).
struct WebGPUBlendState {
    uint32_t color_mask; // SceGxmColorMask bits (A=1, R=2, G=4, B=8)
    uint32_t color_func; // SceGxmBlendFunc
    uint32_t alpha_func; // SceGxmBlendFunc
    uint32_t color_src; // SceGxmBlendFactor
    uint32_t color_dst;
    uint32_t alpha_src;
    uint32_t alpha_dst;
};

// The default sceGxmShaderPatcherCreateFragmentProgram documents for a null
// blendInfo (same value SceGxm.cpp hashes into its program cache key): write
// every channel, blending disabled for both color and alpha.
constexpr WebGPUBlendState webgpu_default_blend_state() {
    return { SCE_GXM_COLOR_MASK_ALL, SCE_GXM_BLEND_FUNC_NONE, SCE_GXM_BLEND_FUNC_NONE,
        SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
        SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO };
}

constexpr WebGPUBlendState webgpu_blend_state_from_guest(const SceGxmBlendInfo *blend) {
    if (!blend)
        return webgpu_default_blend_state();
    return { static_cast<uint32_t>(blend->colorMask), static_cast<uint32_t>(blend->colorFunc),
        static_cast<uint32_t>(blend->alphaFunc), static_cast<uint32_t>(blend->colorSrc),
        static_cast<uint32_t>(blend->colorDst), static_cast<uint32_t>(blend->alphaSrc),
        static_cast<uint32_t>(blend->alphaDst) };
}

// Shapes the WebGPU blend state cannot express, rejected at program creation so
// the guest keeps seeing SCE_GXM_ERROR_DRIVER for state no consumer can render
// (the same fail-closed contract the previous whole-descriptor rejection had).
// WebGPU has no per-channel "blend disabled", so a NONE func is translated as
// ADD by both this consumer and the desktop backends (translate_blend_func).
constexpr bool webgpu_blend_state_supported(const WebGPUBlendState &blend) {
    if (blend.color_mask & ~uint32_t(SCE_GXM_COLOR_MASK_ALL))
        return false;
    for (uint32_t func : { blend.color_func, blend.alpha_func })
        if (func > uint32_t(SCE_GXM_BLEND_FUNC_MAX))
            return false;
    for (uint32_t factor : { blend.color_src, blend.color_dst, blend.alpha_src, blend.alpha_dst })
        if (factor > uint32_t(SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE))
            return false;
    // WebGPU accepts src-alpha-saturate only as the color source factor
    // (GPUBlendComponent validation); desktop maps it to eDstAlpha as a TODO.
    return blend.color_dst != SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE
        && blend.alpha_src != SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE
        && blend.alpha_dst != SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE
        && blend.color_src != SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE
        && blend.color_dst != SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE
        && blend.alpha_src != SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE
        && blend.alpha_dst != SCE_GXM_BLEND_FACTOR_DST_ALPHA_SATURATE;
}

struct WebGPUFragmentProgram final : renderer::FragmentProgram {
    WebGPUBlendState blend = webgpu_default_blend_state();
};

} // namespace browser
