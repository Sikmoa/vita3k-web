// SPDX-License-Identifier: GPL-2.0-or-later
// Browser-only consumer of the production renderer command ABI. No GL/Vulkan.
#include "gxm_webgpu_bridge.h"
#include "gxm_webgpu_program.h"
#include <display/state.h>
#include <emuenv/state.h>
#include <gxm/functions.h>
#include <gxm/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <emscripten.h>
#include <mutex>
#include <stdexcept>
#include <array>
#include <vector>
#include <cmath>
#include <cstring>
#include <exception>

// Suspend only the host HLE stack: JIT regions have returned at the SVC boundary.
// The imported module owns the device; rejection must never signal completion.
EM_ASYNC_JS(int, web_gxm_fence, (), {
    try {
        const bridge = await import(new URL('gxm_hle_bridge.js', globalThis.location.href).href);
        await bridge.finishGuestQueue();
        out('[vita3k-web] GXM WebGPU queue fence completed');
        return 0;
    } catch (error) {
        err('[vita3k-web] GXM WebGPU fence failed: ' + error);
        return -1;
    }
});

EM_ASYNC_JS(int, web_gxm_fill, (uint32_t color, uint32_t width, uint32_t height, uint32_t stride, void *dest), {
    try {
        const bridge = await import(new URL('gxm_hle_bridge.js', globalThis.location.href).href);
        const pixels = await bridge.fillGuestSurface(color, width, height);
        // Reacquire the memory view after suspension; never retain a heap view
        // across device work or copy padded WebGPU rows into guest storage.
        const bytes = Module['vita3kHostBytes'](dest, (height - 1) * stride + width * 4);
        for (let y = 0; y < height; ++y)
            bytes.set(pixels.subarray(y * width * 4, (y + 1) * width * 4), y * stride);
        out('[vita3k-web] GXM WebGPU transfer fill readback completed');
        return 0;
    } catch (error) {
        err('[vita3k-web] GXM WebGPU fill failed: ' + error + ' dest=' + dest
            + ' byteLength=' + wasmMemory.buffer.byteLength);
        return -1;
    }
});

EM_ASYNC_JS(int, web_gxm_draw, (const void *packet, uint32_t size, uint32_t width, uint32_t height, uint32_t stride, void *dest), {
    try {
        const owned = Module['vita3kHostBytes'](packet, size).slice();
        const initial = new Uint8Array(width * height * 4);
        const source = Module['vita3kHostBytes'](dest, (height - 1) * stride + width * 4);
        for (let y = 0; y < height; ++y)
            initial.set(source.subarray(y * stride, y * stride + width * 4), y * width * 4);
        const bridge = await import(new URL('gxm_hle_bridge.js', globalThis.location.href).href);
        const pixels = await bridge.drawGuestSurface(owned, initial, width, height);
        const bytes = Module['vita3kHostBytes'](dest, (height - 1) * stride + width * 4);
        for (let y = 0; y < height; ++y)
            bytes.set(pixels.subarray(y * width * 4, (y + 1) * width * 4), y * stride);
        out('[vita3k-web] GXM WebGPU GXP indexed draw readback completed');
        return 0;
    } catch (error) {
        err('[vita3k-web] GXM WebGPU draw failed: ' + (error.stack || error));
        return -1;
    }
});

namespace {
[[noreturn]] void unsupported(const char *what) {
    throw std::runtime_error(std::string("WebGPU GXM unsupported: ") + what);
}
struct WebContext final : renderer::Context {
    bool has_surface = false;
    // Recorded GXM viewport state: [xOffset, yOffset, zOffset, xScale, yScale, zScale].
    // has_viewport tracks whether a Viewport command arrived; a draw without one
    // is rejected rather than rendered with an implicit viewport.
    std::array<float, 6> viewport{};
    bool has_viewport = false;
    std::array<uint32_t, 4> clip{};
    std::array<std::vector<uint8_t>, 2> uniforms;
    bool has_fragment_texture = false;
    SceGxmTexture fragment_texture{}; // Command-owned descriptor, not a guest pointer.
    // Depth-stencil attachment presence. The descriptor itself stays in
    // record.depth_stencil_surface; a null or disabled guest surface clears it
    // exactly like scene.cpp handle_set_context.
    bool has_depth = false;
};

struct WebState final : renderer::State {
    MemState &mem;
    explicit WebState(MemState &m) : mem(m) {
        current_backend = renderer::Backend::WebGPU;
        context = nullptr; res_multiplier = 1; disable_surface_sync = false;
        should_display = false; stretch_the_display_area = false;
        fullscreen_hd_res_pixel_perfect = false;
    }
    bool init() override { return true; }
    void late_init(const Config &, std::string_view, MemState &) override {}
    renderer::TextureCache *get_texture_cache() override { return nullptr; }
    void render_frame(DisplayState &, const GxmState &, MemState &) override { unsupported("native presentation"); }
    void swap_window() override { unsupported("native window"); }
    std::vector<uint32_t> dump_frame(DisplayState &, uint32_t &, uint32_t &) override { unsupported("native screenshot"); }
    int get_supported_filters() override { return 0; }
    void set_screen_filter(const std::string_view &) override { unsupported("screen filter"); }
    int get_max_anisotropic_filtering() override { return 1; }
    void set_anisotropic_filtering(int n) override { if (n != 1) unsupported("anisotropy"); }
    int get_max_2d_texture_width() override { return 4096; }
    std::string_view get_gpu_name() override { return "WebGPU"; }
    void precompile_shader(const renderer::ShadersHash &) override { unsupported("native shader cache"); }
    void preclose_action() override {}
};
}
namespace browser {
int gxm_initialize(EmuEnvState &env) {
    if (env.renderer) return SCE_GXM_ERROR_ALREADY_INITIALIZED;
    env.renderer = std::make_unique<WebState>(env.mem);
    env.gxm.notification_region = Ptr<uint32_t>(alloc(env.mem, 1024 * 1024, "SceGxmNotificationRegion"));
    if (!env.gxm.notification_region) { env.renderer.reset(); return SCE_GXM_ERROR_DRIVER; }
    memset(env.gxm.notification_region.get(env.mem), 0, 1024 * 1024);
    // Display-queue thread. The desktop backend runs this as a host std::thread
    // that blocks on the queue's condition variables and drives the guest
    // display callback (sceDisplaySetFrameBuf). The browser execution host has
    // no host threads, so the same guest thread is created here (null entry,
    // same priority) and driven cooperatively from
    // sceGxmDisplayQueueAddEntry, its only producer. A dormant thread parks in
    // run_loop without executing the null entry, exactly like desktop.
    const ThreadStatePtr display_queue_thread = env.kernel.create_thread(env.mem, "SceGxmDisplayQueue",
        Ptr<void>(0), SCE_KERNEL_HIGHEST_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
        SCE_KERNEL_STACK_SIZE_USER_DEFAULT, nullptr);
    if (!display_queue_thread) {
        free(env.mem, env.gxm.notification_region.address());
        env.gxm.notification_region.reset();
        env.renderer.reset();
        return SCE_GXM_ERROR_DRIVER;
    }
    env.gxm.display_queue_thread = display_queue_thread->id;
    env.gxm.display_queue.reset();
    std::printf("[vita3k-web] SceGxmDisplayQueue thread=%d callback=%08x dataSize=%u maxPending=%u\n",
        env.gxm.display_queue_thread, env.gxm.params.displayQueueCallback.address(),
        env.gxm.params.displayQueueCallbackDataSize, env.gxm.params.displayQueueMaxPendingCount);
    return 0;
}
int gxm_terminate(EmuEnvState &env) {
    if (!env.renderer) return SCE_GXM_ERROR_UNINITIALIZED;
    // Every queued entry was drained inline before its AddEntry returned, so
    // only the guest thread is left; release it the way desktop terminate does.
    if (env.gxm.display_queue_thread) {
        const ThreadStatePtr display_thread = env.kernel.get_thread(env.gxm.display_queue_thread);
        if (display_thread) display_thread->exit_delete(false);
        env.gxm.display_queue_thread = 0;
    }
    gxm::destroy_all_contexts(env, true);
    gxm::destroy_all_render_targets(env, true);
    free(env.mem, env.gxm.notification_region.address());
    env.gxm.notification_region.reset();
    env.renderer.reset();
    return 0;
}
}
namespace renderer {
SyncWaitResult wishlist(SceGxmSyncObject *sync, uint32_t timestamp, int32_t timeout_micros) {
    const double start = emscripten_get_now();
    unsigned spins = 0;
    while (sync->timestamp_current < timestamp) {
        if (sync->being_deleted) return SyncWaitResult::Shutdown;
        if (timeout_micros >= 0 && (emscripten_get_now() - start) * 1000 >= timeout_micros)
            return SyncWaitResult::TimedOut;
        // A wait that never becomes ready would otherwise look like a silent
        // hang (the display-queue drain waits here); report the first waits.
        static unsigned pending_reports = 0;
        if (spins++ == 1 && pending_reports < 8) {
            ++pending_reports;
            std::printf("[gxm-wait] sync current=%u target=%u not ready\n",
                sync->timestamp_current.load(), timestamp);
        }
        emscripten_sleep(1);
    }
    return sync->being_deleted ? SyncWaitResult::Shutdown : SyncWaitResult::Ready;
}
void subject_done(SceGxmSyncObject *sync, uint32_t timestamp) {
    assert(timestamp <= sync->timestamp_ahead);
    sync->timestamp_current = std::max(sync->timestamp_current.load(), timestamp);
    sync->cond.notify_all();
}
Command *generic_command_allocate() { return new Command{}; }
void generic_command_free(Command *cmd) { delete cmd; }
void reset_command_list(CommandList &list) { list.first = list.last = nullptr; }
void destroy_command_payload(Command &cmd) {
    if (cmd.opcode == CommandOpcode::SetContext) {
        CommandHelper h(&cmd); h.pop<RenderTarget *>();
        delete h.pop<SceGxmColorSurface *>(); delete h.pop<SceGxmDepthStencilSurface *>();
    } else if (cmd.opcode == CommandOpcode::TransferFill) {
        CommandHelper h(&cmd); h.pop<uint32_t>();
        delete h.pop<const SceGxmTransferImage *>();
    }
    // NewFrame owns a host DisplayFrameInfo*, released in-handler exactly like
    // sync.cpp new_frame (copied into display state, then deleted). No guest
    // pointers or variable payloads cross the other accepted opcodes.
}
bool create_context(State &s, std::unique_ptr<Context> &ctx) {
    ctx = std::make_unique<WebContext>();
    s.context = ctx.get();
    return true;
}
void destroy_context_during_shutdown(State &s, std::unique_ptr<Context> &ctx) { if (s.context == ctx.get()) s.context = nullptr; ctx.reset(); }
void destroy_context(State &s, std::unique_ptr<Context> &ctx) { destroy_context_during_shutdown(s, ctx); }
bool create_render_target(State &, std::unique_ptr<RenderTarget> &rt, const SceGxmRenderTargetParams *p) {
    if (p->multisampleMode != SCE_GXM_MULTISAMPLE_NONE || !p->width || !p->height || p->width > 4096 || p->height > 4096) return false;
    rt = std::make_unique<RenderTarget>(); rt->multisample_mode = p->multisampleMode;
    return true;
}
void destroy_render_target_during_shutdown(State &, std::unique_ptr<RenderTarget> &rt) { rt.reset(); }
void destroy_render_target(State &s, std::unique_ptr<RenderTarget> &rt) { destroy_render_target_during_shutdown(s, rt); }
// Bounded diagnostic for rejected scenes. Decode the same ABI as the
// production command producers without executing or acknowledging the batch.
static void trace_scene(CommandList &list, MemState &mem) {
    auto dump = [&](const char *name, Ptr<const uint8_t> ptr, size_t size) {
        printf("[gxm-decode] %s address=%08x size=%zu bytes=", name, ptr.address(), size);
        const size_t count = std::min(size, size_t(96));
        if (!ptr || !count || uint64_t(ptr.address()) + size > (uint64_t(1) << 32)
            || !is_valid_addr_range(mem, ptr.address(), uint64_t(ptr.address()) + size)) {
            printf("invalid\n"); return;
        }
        const auto *data = ptr.get(mem);
        for (size_t i = 0; i < count; ++i) printf("%02x", data[i]);
        printf("\n");
    };
    unsigned count = 0;
    for (auto *cmd = list.first; cmd && count++ < 64; cmd = cmd->next) {
        CommandHelper h(cmd);
        switch (cmd->opcode) {
        case CommandOpcode::SetContext: {
            // SetContext is opcode 10; its first payload is a native host
            // RenderTarget* (8 bytes under Memory64), not a 32-bit guest Ptr.
            printf("[gxm-decode] raw opcode=%u first8=", unsigned(cmd->opcode));
            for (unsigned i = 0; i < 8; ++i) printf("%02x", cmd->data[i]);
            const auto *target = h.pop<RenderTarget *>();
            printf(" pointer_bytes=%zu decoded_target=%p\n", sizeof(target), static_cast<const void *>(target));
            const auto *color = h.pop<SceGxmColorSurface *>();
            const auto *depth = h.pop<SceGxmDepthStencilSurface *>();
            if (color) printf("[gxm-decode] surface width=%u height=%u stride=%u address=%08x format=%08x depth=%d\n",
                color->width, color->height, color->strideInPixels, color->data.address(), unsigned(color->colorFormat), depth != nullptr);
            break;
        }
        case CommandOpcode::SetState: {
            const auto kind = h.pop<GXMState>();
            switch (kind) {
            case GXMState::RegionClip: {
                const auto mode = h.pop<SceGxmRegionClipMode>();
                const auto xmin = h.pop<uint32_t>(), xmax = h.pop<uint32_t>();
                const auto ymin = h.pop<uint32_t>(), ymax = h.pop<uint32_t>();
                printf("[gxm-decode] clip mode=%u bounds=%u,%u,%u,%u\n", unsigned(mode), xmin, xmax, ymin, ymax);
                break;
            }
            case GXMState::Viewport: {
                const bool flat = h.pop<bool>();
                printf("[gxm-decode] viewport flat=%d", flat);
                if (!flat) for (int i = 0; i < 6; ++i) printf(" %g", double(h.pop<float>()));
                printf("\n"); break;
            }
            case GXMState::Program: {
                const auto ptr = h.pop<Ptr<const void>>();
                const bool fragment = h.pop<bool>();
                printf("[gxm-decode] program fragment=%d address=%08x\n", fragment, ptr.address());
                break;
            }
            case GXMState::UniformBuffer: {
                const auto ptr = h.pop<Ptr<const uint8_t>>();
                const bool vertex = h.pop<bool>();
                const int block = h.pop<int>();
                const auto size = h.pop<uint32_t>();
                printf("[gxm-decode] uniform vertex=%d block=%d\n", vertex, block);
                dump("uniform", ptr, size); break;
            }
            case GXMState::VertexStream: {
                const auto ptr = h.pop<Ptr<const uint8_t>>();
                const auto index = h.pop<size_t>(), size = h.pop<size_t>();
                printf("[gxm-decode] stream index=%zu\n", index);
                dump("vertices", ptr, size); break;
            }
            default: printf("[gxm-decode] state=%u\n", unsigned(kind)); break;
            }
            break;
        }
        case CommandOpcode::Draw: {
            const auto primitive = h.pop<SceGxmPrimitiveType>();
            const auto format = h.pop<SceGxmIndexFormat>();
            const auto ptr = h.pop<Ptr<const uint8_t>>();
            const auto indices = h.pop<uint32_t>(), instances = h.pop<uint32_t>();
            printf("[gxm-decode] draw primitive=%u format=%u count=%u instances=%u\n", unsigned(primitive), unsigned(format), indices, instances);
            dump("indices", ptr, size_t(indices) * (format == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4));
            break;
        }
        case CommandOpcode::SyncSurfaceData: {
            const auto v = h.pop<SceGxmNotification>(), f = h.pop<SceGxmNotification>();
            printf("[gxm-decode] sync vertex=%08x fragment=%08x\n", v.address.address(), f.address.address());
            break;
        }
        case CommandOpcode::SignalSyncObject:
        case CommandOpcode::WaitSyncObject: {
            const auto sync = h.pop<Ptr<SceGxmSyncObject>>();
            const auto timestamp = h.pop<uint32_t>();
            printf("[gxm-decode] %s sync=%08x timestamp=%u\n",
                cmd->opcode == CommandOpcode::SignalSyncObject ? "signal" : "wait", sync.address(), timestamp);
            break;
        }
        case CommandOpcode::SignalNotification: {
            const auto n = h.pop<SceGxmNotification>();
            printf("[gxm-decode] notification address=%08x value=%u\n", n.address.address(), n.value);
            break;
        }
        case CommandOpcode::NewFrame:
            printf("[gxm-decode] new frame (display-queue entry)\n");
            break;
        default: printf("[gxm-decode] opcode=%u\n", unsigned(cmd->opcode)); break;
        }
    }
}
static void require_guest(MemState &mem, Address address, size_t size) {
    if (!address || !size || uint64_t(address) + size > (uint64_t(1) << 32)
        || !is_valid_addr_range(mem, address, uint64_t(address) + size))
        unsupported("invalid guest draw range");
}

// Guest depth bytes per sample, mirroring vulkan surface_cache.
static uint32_t depth_bytes_per_sample(SceGxmDepthStencilFormat format) {
    switch (format) {
    case SCE_GXM_DEPTH_STENCIL_FORMAT_S8:
        return 1;
    case SCE_GXM_DEPTH_STENCIL_FORMAT_D16:
        return 2;
    default:
        return 4;
    }
}

// Depth formats the WebGPU consumer represents exactly:
//   D16   -> depth16unorm           (16-bit unorm guest depth)
//   DF32  -> depth32float           (32-bit float guest depth)
//   S8D24 -> depth24plus-stencil8   (24-bit unorm depth, 8-bit stencil)
// S8, DF32M, DF32_S8 and DF32M_S8 have no matching WebGPU format; accepting
// them would silently reinterpret guest depth values. All of them reject here,
// before any packet is built.
static bool supported_depth_format(SceGxmDepthStencilFormat format) {
    switch (format) {
    case SCE_GXM_DEPTH_STENCIL_FORMAT_D16:
    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32:
    case SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24:
        return true;
    default:
        return false;
    }
}

// Guest attribute formats the WebGPU consumer can fetch exactly, and the byte
// size of one element for each accepted shape (0 when no WebGPU vertex format
// represents the shape). Normalized and float attributes are delivered to the
// shader as floats, which is how the SPIR-V converter declares them. The
// non-normalized U8/S8/U16/S16 are integer formats whose float form needs a
// scaled vertex format WebGPU does not have, and UNTYPED needs an integer
// shader input; both reject. WebGPU also has no 1- or 3-component 8/16-bit
// vertex format, so those component counts reject at the accepted size.
static uint32_t webgpu_vertex_element_size(SceGxmAttributeFormat format, uint32_t components) {
    if (components < 1 || components > 4) return 0;
    switch (format) {
    case SCE_GXM_ATTRIBUTE_FORMAT_U8N:
    case SCE_GXM_ATTRIBUTE_FORMAT_S8N:
        return components == 2 || components == 4 ? components : 0;
    case SCE_GXM_ATTRIBUTE_FORMAT_U16N:
    case SCE_GXM_ATTRIBUTE_FORMAT_S16N:
    case SCE_GXM_ATTRIBUTE_FORMAT_F16:
        return components == 2 || components == 4 ? components * 2 : 0;
    case SCE_GXM_ATTRIBUTE_FORMAT_F32:
        return components * 4;
    default:
        return 0;
    }
}

// Short guest-format label for rejection diagnostics.
static const char *attribute_format_name(SceGxmAttributeFormat format) {
    switch (format) {
    case SCE_GXM_ATTRIBUTE_FORMAT_U8: return "U8";
    case SCE_GXM_ATTRIBUTE_FORMAT_S8: return "S8";
    case SCE_GXM_ATTRIBUTE_FORMAT_U16: return "U16";
    case SCE_GXM_ATTRIBUTE_FORMAT_S16: return "S16";
    case SCE_GXM_ATTRIBUTE_FORMAT_U8N: return "U8N";
    case SCE_GXM_ATTRIBUTE_FORMAT_S8N: return "S8N";
    case SCE_GXM_ATTRIBUTE_FORMAT_U16N: return "U16N";
    case SCE_GXM_ATTRIBUTE_FORMAT_S16N: return "S16N";
    case SCE_GXM_ATTRIBUTE_FORMAT_F16: return "F16";
    case SCE_GXM_ATTRIBUTE_FORMAT_F32: return "F32";
    default: return "UNTYPED/unknown";
    }
}

// The only stencil state the consumer can honour: always pass, keep on every
// outcome, full masks and zero reference. Anything else would need a stencil
// stage (and stencil writes back into guest memory) that does not exist yet.
static bool stencil_state_default(const GxmStencilStateOp &op, const GxmStencilStateValues &values) {
    return op.func == SCE_GXM_STENCIL_FUNC_ALWAYS
        && op.stencil_fail == SCE_GXM_STENCIL_OP_KEEP
        && op.depth_fail == SCE_GXM_STENCIL_OP_KEEP
        && op.depth_pass == SCE_GXM_STENCIL_OP_KEEP
        && values.compare_mask == 0xff && values.write_mask == 0xff && values.ref == 0;
}

// Bounded reject diagnostic: print every descriptor field the validator reads
// (plus the raw control words) so a rejected guest texture names itself instead
// of leaving only the generic "unsupported" reason. Only runs on the rejecting
// path, which aborts the draw anyway.
static void dump_fragment_texture(const SceGxmTexture &t) {
    uint32_t words[4];
    memcpy(words, &t, sizeof(words));
    printf("[gxm-reject] fragment texture words=%08x %08x %08x %08x type=%u format=%08x %ux%u "
        "true_mips=%u mip_count=%u mip_filter=%u lod_bias=%u lod_min0=%u lod_min1=%u "
        "gamma=%u normalize=%u format0=%u swizzle=%u palette=%08x addr=%08x "
        "filters min=%u mag=%u uv=%u,%u unk=%u,%u,%u\n",
        words[0], words[1], words[2], words[3], unsigned(t.texture_type()),
        unsigned(gxm::get_format(t)), gxm::get_width(t), gxm::get_height(t),
        t.true_mip_count(), t.mip_count, t.mip_filter, t.lod_bias, t.lod_min0, t.lod_min1,
        t.gamma_mode, t.normalize_mode, t.format0, t.swizzle_format, t.palette_addr,
        uint32_t(t.data_addr) << 2, t.min_filter, t.mag_filter, t.uaddr_mode, t.vaddr_mode,
        t.unk0, t.unk1, t.unk2);
}

// Guest texel layouts the WebGPU consumer represents exactly. GXM's
// two-component formats carry a component swizzle that a WebGPU texture cannot
// express, so the guest texels are expanded to RGBA8 here with the same
// component mapping the Vulkan and GL backends apply (translate_swizzle2).
// `bytes_per_texel` is the guest texel size; `expand` writes `texels` RGBA8
// pixels.
struct FragmentTextureFormat {
    SceGxmTextureFormat guest_format;
    uint32_t bytes_per_texel;
    void (*expand)(uint8_t *dst, const uint8_t *src, uint32_t texels);
};
static void expand_u8u8u8u8_abgr(uint8_t *dst, const uint8_t *src, uint32_t texels) {
    // Little-endian ABGR word: byte 0 is R through byte 3 is A, which is
    // already rgba8unorm byte order.
    memcpy(dst, src, size_t(texels) * 4);
}
// U8U8_GRRR: little-endian GR word (byte 0 = R, byte 1 = G) sampled through the
// SWIZZLE2_GRRR mapping { R, R, R, G }, so RGB replicates the first byte and
// alpha comes from the second.
static void expand_u8u8_grrr(uint8_t *dst, const uint8_t *src, uint32_t texels) {
    for (uint32_t i = 0; i < texels; ++i) {
        const uint8_t r = src[i * 2], a = src[i * 2 + 1];
        dst[i * 4 + 0] = r; dst[i * 4 + 1] = r; dst[i * 4 + 2] = r; dst[i * 4 + 3] = a;
    }
}
static const FragmentTextureFormat fragment_texture_formats[] = {
    { SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 4, expand_u8u8u8u8_abgr },
    { SCE_GXM_TEXTURE_FORMAT_U8U8_GRRR, 2, expand_u8u8_grrr },
};
static const FragmentTextureFormat *find_fragment_texture_format(SceGxmTextureFormat format) {
    for (const auto &candidate : fragment_texture_formats)
        if (candidate.guest_format == format) return &candidate;
    return nullptr;
}

// Match gxm/src/textures.cpp and SceGxm's accessors, not a tightly packed
// interpretation of the guest descriptor. LINEAR_STRIDED has different packed
// control fields and is deliberately NOT accepted here.
static void validate_fragment_texture(const SceGxmTexture &t) {
    // mip_filter only has an effect when the descriptor owns a mip chain: with
    // a single level the hardware cannot blend levels, so the bit is inert and
    // the one-level WebGPU texture matches it exactly.
    if (t.texture_type() != SCE_GXM_TEXTURE_LINEAR
        || !find_fragment_texture_format(gxm::get_format(t))
        || (t.mip_filter && t.true_mip_count() != 1)
        || !t.normalize_mode || t.gamma_mode || t.lod_bias != 31
        || t.lod_min0 || t.lod_min1 || t.palette_addr || t.unk0 || t.unk1 || t.unk2) {
        dump_fragment_texture(t);
        unsupported("fragment texture layout/format/mips/LOD/normalization");
    }
    if ((t.min_filter != SCE_GXM_TEXTURE_FILTER_POINT && t.min_filter != SCE_GXM_TEXTURE_FILTER_LINEAR)
        || (t.mag_filter != SCE_GXM_TEXTURE_FILTER_POINT && t.mag_filter != SCE_GXM_TEXTURE_FILTER_LINEAR)
        || t.uaddr_mode > SCE_GXM_TEXTURE_ADDR_CLAMP || t.vaddr_mode > SCE_GXM_TEXTURE_ADDR_CLAMP) {
        dump_fragment_texture(t);
        unsupported("fragment texture sampler");
    }
}

static void consume_state(WebContext &ctx, CommandHelper &h, MemState &mem) {
    switch (h.pop<GXMState>()) {
    case GXMState::RegionClip:
        ctx.record.region_clip_mode = h.pop<SceGxmRegionClipMode>();
        for (auto &n : ctx.clip) n = h.pop<uint32_t>();
        break;
    case GXMState::Viewport:
        // Mirror state_set.cpp COMMAND_SET_STATE(viewport) record fields, minus
        // the MSAA/downscale factor (unsupported render targets are rejected).
        ctx.record.viewport_flat = h.pop<bool>();
        ctx.has_viewport = true;
        if (!ctx.record.viewport_flat) {
            for (auto &n : ctx.viewport) n = h.pop<float>();
            const float ymin = ctx.viewport[1] + ctx.viewport[4];
            const float ymax = ctx.viewport[1] - ctx.viewport[4] - 1;
            ctx.record.viewport_flip = { 1.0f, (ymin < ymax) ? -1.0f : 1.0f, 1.0f, 1.0f };
            ctx.record.z_offset = ctx.viewport[2];
            ctx.record.z_scale = ctx.viewport[5];
        } else {
            ctx.record.viewport_flip = { 1.0f, -1.0f, 1.0f, 1.0f };
            ctx.record.z_offset = 0.0f;
            ctx.record.z_scale = 1.0f;
        }
        break;
    case GXMState::Program: {
        const auto ptr = h.pop<Ptr<const void>>();
        const bool fragment = h.pop<bool>();
        if (fragment) {
            require_guest(mem, ptr.address(), sizeof(SceGxmFragmentProgram));
            ctx.record.fragment_program = ptr.cast<SceGxmFragmentProgram>();
            ctx.uniforms[1].clear();
        } else {
            require_guest(mem, ptr.address(), sizeof(SceGxmVertexProgram));
            ctx.record.vertex_program = ptr.cast<SceGxmVertexProgram>();
            ctx.uniforms[0].clear();
        }
        break;
    }
    case GXMState::UniformBuffer: {
        const auto ptr = h.pop<Ptr<const uint8_t>>();
        const bool vertex = h.pop<bool>();
        const int block = h.pop<int>();
        const auto size = h.pop<uint32_t>();
        ShaderProgram *program = nullptr;
        if (vertex && ctx.record.vertex_program)
            program = ctx.record.vertex_program.get(mem)->renderer_data.get();
        if (!vertex && ctx.record.fragment_program)
            program = ctx.record.fragment_program.get(mem)->renderer_data.get();
        if (!program || block < 0 || size_t(block) >= program->uniform_buffer_sizes.size())
            unsupported("uniform without valid program/block");
        const auto offset = program->uniform_buffer_data_offsets[block];
        if (offset == UINT32_MAX) break;
        const size_t total = program->max_total_uniform_buffer_storage * 4;
        const size_t copied = std::min(size_t(size), size_t(program->uniform_buffer_sizes[block]) * 4);
        if (total > 16 * 1024 * 1024 || uint64_t(offset) * 4 + copied > total)
            unsupported("uniform packing overflow");
        require_guest(mem, ptr.address(), copied);
        auto &bytes = ctx.uniforms[vertex ? 0 : 1];
        bytes.resize(total);
        memcpy(bytes.data() + size_t(offset) * 4, ptr.get(mem), copied);
        break;
    }
    case GXMState::Texture: {
        // renderer::set_texture sends uint32_t index then SceGxmTexture by
        // value. Fragment indices start at 0; vertex indices start at 16.
        const auto index = h.pop<uint32_t>();
        const auto texture = h.pop<SceGxmTexture>();
        if (index != 0) unsupported("only fragment texture unit zero supported");
        validate_fragment_texture(texture);
        ctx.fragment_texture = texture;
        ctx.has_fragment_texture = true;
        break;
    }
    case GXMState::VertexStream: {
        const auto ptr = h.pop<Ptr<const uint8_t>>();
        const auto index = h.pop<size_t>(), size = h.pop<size_t>();
        if (index != 0) unsupported("multiple vertex streams");
        require_guest(mem, ptr.address(), size);
        ctx.record.vertex_streams[index] = {ptr, size};
        break;
    }
    case GXMState::CullMode:
        ctx.record.cull_mode = h.pop<SceGxmCullMode>();
        break;
    case GXMState::PolygonMode: {
        const bool front = h.pop<bool>();
        const auto mode = h.pop<SceGxmPolygonMode>();
        if (front) ctx.record.front_polygon_mode = mode;
        else ctx.record.back_polygon_mode = mode;
        break;
    }
    case GXMState::DepthFunc: {
        const bool front = h.pop<bool>();
        const auto func = h.pop<SceGxmDepthFunc>();
        if (front) ctx.record.front_depth_func = func;
        else ctx.record.back_depth_func = func;
        break;
    }
    case GXMState::DepthWriteEnable: {
        const bool front = h.pop<bool>();
        const auto mode = h.pop<SceGxmDepthWriteMode>();
        if (front) ctx.record.front_depth_write_mode = mode;
        else ctx.record.back_depth_write_mode = mode;
        break;
    }
    default: unsupported("render state not implemented");
    }
}

static int consume_draw(WebContext &ctx, CommandHelper &h, MemState &mem) {
    const auto primitive = h.pop<SceGxmPrimitiveType>();
    const auto format = h.pop<SceGxmIndexFormat>();
    const auto indices = h.pop<Ptr<const uint8_t>>();
    const auto count = h.pop<uint32_t>(), instances = h.pop<uint32_t>();
    // The first draw headers of a run describe the draw stream shape
    // (primitive/index format/instancing); bounded so a long run stays quiet.
    static unsigned draw_headers = 0;
    if (draw_headers < 16) {
        ++draw_headers;
        printf("[gxm-decode] draw primitive=%u indexformat=%u count=%u instances=%u indices=%08x\n",
            unsigned(primitive), unsigned(format), count, instances, indices.address());
    }
    if ((primitive != SCE_GXM_PRIMITIVE_TRIANGLES && primitive != SCE_GXM_PRIMITIVE_TRIANGLE_FAN)
        || instances != 1 || count < 3
        || (primitive == SCE_GXM_PRIMITIVE_TRIANGLES && count % 3)
        || (format != SCE_GXM_INDEX_FORMAT_U16 && format != SCE_GXM_INDEX_FORMAT_U32)) {
        printf("[gxm-reject] draw header primitive=%u indexformat=%u count=%u instances=%u count%%3=%u\n",
            unsigned(primitive), unsigned(format), count, instances, count % 3);
        unsupported("only non-instanced indexed triangles or triangle fans supported");
    }
    if (!ctx.has_surface || !ctx.record.vertex_program || !ctx.record.fragment_program)
        unsupported("draw without surface/programs");
    if (!ctx.has_viewport)
        unsupported("draw without viewport state");
    const auto &surface = ctx.record.color_surface;
    const auto w = surface.width, height = surface.height;
    // Arbitrary GXM viewports are forwarded to the WebGPU viewport (GXM3).
    // Non-default region clip (scissor) stays rejected: no scissor stage exists.
    if (ctx.record.region_clip_mode != SCE_GXM_REGION_CLIP_OUTSIDE
        || ctx.clip != std::array<uint32_t, 4>{0, w - 1, 0, height - 1})
        unsupported("non-default region clip");
    // Fixed-function state the consumer cannot express yet. Draws proceed
    // only on default state; anything else fails loudly naming the state
    // instead of rendering incorrectly.
    if (ctx.record.cull_mode != SCE_GXM_CULL_NONE)
        unsupported("cull mode not implemented");
    if (ctx.record.front_polygon_mode != SCE_GXM_POLYGON_MODE_TRIANGLE_FILL
        || ctx.record.back_polygon_mode != SCE_GXM_POLYGON_MODE_TRIANGLE_FILL)
        unsupported("polygon mode not implemented");
    // GXM records front and back depth state separately, but no modern API can
    // express that: Vita3K's own backends collapse it to the front face
    // (vulkan/pipeline_cache.cpp:857-858, gl/sync_state.cpp:205-213), and this
    // consumer follows the reference renderer instead of rejecting the draw.
    // The approximation is never silent: the first mismatch is reported, which
    // matters because cull mode is restricted to NONE, so the state applies to
    // every rasterized face.
    if (ctx.record.front_depth_func != ctx.record.back_depth_func
        || ctx.record.front_depth_write_mode != ctx.record.back_depth_write_mode) {
        static bool two_sided_depth_reported = false;
        if (!two_sided_depth_reported) {
            two_sided_depth_reported = true;
            printf("[gxm-approx] two-sided depth state front func=%u write=%u back func=%u write=%u; using the front state\n",
                unsigned(ctx.record.front_depth_func), unsigned(ctx.record.front_depth_write_mode),
                unsigned(ctx.record.back_depth_func), unsigned(ctx.record.back_depth_write_mode));
        }
    }
    if (ctx.record.front_depth_func > SCE_GXM_DEPTH_FUNC_ALWAYS)
        unsupported("depth function not implemented");
    // Without a depth attachment the recorded depth state is not representable:
    // WebGPU would silently test against nothing. Only the inert default
    // (the same state the pre-attachment consumer required) is accepted.
    if (!ctx.has_depth && (ctx.record.front_depth_func != SCE_GXM_DEPTH_FUNC_LESS_EQUAL
            || ctx.record.front_depth_write_mode != SCE_GXM_DEPTH_WRITE_ENABLED))
        unsupported("depth test state without depth surface");
    // No stencil stage exists, so only the inert GXM default is accepted. The
    // S8D24 attachment is cleared/discarded, never written back to guest memory.
    if (ctx.has_depth && !(stencil_state_default(ctx.record.front_stencil_state_op, ctx.record.front_stencil_state_values)
            && stencil_state_default(ctx.record.back_stencil_state_op, ctx.record.back_stencil_state_values)))
        unsupported("stencil state not implemented");
    const auto *vp = ctx.record.vertex_program.get(mem);
    const auto *fp = ctx.record.fragment_program.get(mem);
    if (!vp->renderer_data || !fp->renderer_data || fp->is_maskupdate
        || vp->renderer_data->textures_used.any()
        || (fp->renderer_data->textures_used >> 1).any()
        || vp->streams.size() != 1 || vp->attributes.empty() || vp->attributes.size() > 16)
        unsupported("vertex/nonzero fragment textures/mask/multiple streams or missing program metadata");
    const bool textured = fp->renderer_data->textures_used[0];
    std::vector<uint8_t> texture_pixels;
    uint32_t texture_width = 0, texture_height = 0;
    if (textured) {
        if (!ctx.has_fragment_texture) unsupported("missing fragment texture unit zero");
        const auto &t = ctx.fragment_texture;
        validate_fragment_texture(t);
        const auto *format = find_fragment_texture_format(gxm::get_format(t));
        texture_width = gxm::get_width(t); texture_height = gxm::get_height(t);
        if (!texture_width || !texture_height || texture_width > 4096 || texture_height > 4096)
            unsupported("fragment texture dimensions");
        // LINEAR guest rows are aligned to 8 pixels (gxm::texture_size_first_mip
        // and the texture cache both align a LINEAR stride to 8), then sized in
        // the guest texel size of the format.
        const uint64_t pitch = uint64_t((texture_width + 7) & ~7u) * format->bytes_per_texel;
        const uint64_t footprint = pitch * texture_height;
        if (footprint > 16 * 1024 * 1024) unsupported("fragment texture upload size");
        const Address address = uint32_t(t.data_addr) << 2; // sceGxmTextureGetData
        require_guest(mem, address, footprint);
        // Snapshot every draw, even without a dirty descriptor: guest pixels
        // can change independently. Expand to the sampled RGBA8 and strip row
        // padding before the first await.
        texture_pixels.resize(size_t(texture_width) * texture_height * 4);
        const auto *source = Ptr<const uint8_t>(address).get(mem);
        for (uint32_t y = 0; y < texture_height; ++y)
            format->expand(texture_pixels.data() + size_t(y) * texture_width * 4,
                source + size_t(y) * pitch, texture_width);
    }
    const size_t index_size = format == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4;
    const size_t source_index_bytes = size_t(count) * index_size;
    // WebGPU has no triangle-fan topology, so a fan is expanded to the
    // equivalent triangle list with the same index buffer (the fan centre is
    // the first guest index, not vertex zero). GXM and WebGPU both take the
    // provoking vertex from the first index of each triangle, so emitting
    // (centre, k, k+1) preserves flat shading, and cull mode is restricted to
    // NONE so the winding of the expansion cannot matter.
    const bool fan = primitive == SCE_GXM_PRIMITIVE_TRIANGLE_FAN;
    const size_t index_bytes = fan ? size_t(count - 2) * 3 * index_size : source_index_bytes;
    std::vector<uint8_t> fan_indices;
    if (fan) {
        const uint8_t *guest_indices = indices.get(mem);
        // Explicit little-endian reads/writes: the guest index buffer carries
        // no alignment guarantee and wasm is little-endian like the JS decoder.
        const auto read_index = [&](uint32_t i) {
            const uint8_t *p = guest_indices + size_t(i) * index_size;
            if (index_size == 2) return uint32_t(p[0] | (p[1] << 8));
            return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24));
        };
        fan_indices.resize(index_bytes);
        uint8_t *out = fan_indices.data();
        const auto write_index = [&](uint32_t value) {
            for (size_t byte = 0; byte < index_size; ++byte) *out++ = uint8_t(value >> (8 * byte));
        };
        const uint32_t centre = read_index(0);
        for (uint32_t k = 1; k + 1 < count; ++k) {
            write_index(centre); write_index(read_index(k)); write_index(read_index(k + 1));
        }
    }
    const auto &stream = ctx.record.vertex_streams[0];
    const size_t stride = vp->streams[0].stride;
    if (gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(vp->streams[0].indexSource)))
        unsupported("instanced vertex stream");
    if (!stride || stream.size > 16 * 1024 * 1024 || index_bytes > 16 * 1024 * 1024)
        unsupported("draw upload size");
    require_guest(mem, indices.address(), source_index_bytes);
    require_guest(mem, stream.data.address(), stream.size);
    std::vector<std::array<uint32_t, 4>> attributes;
    for (const auto &a : vp->attributes) {
        const auto info = vp->renderer_data->attribute_infos.find(a.regIndex);
        const uint32_t element = webgpu_vertex_element_size(a.format, a.componentCount);
        // The element must fit entirely inside the stride, and the shader input
        // must be a float: every accepted format is fetched as normalized or
        // float data, while an integer shader input would need an integer
        // vertex format this consumer does not build.
        if (a.streamIndex != 0 || !element || a.offset + element > stride
            || info == vp->renderer_data->attribute_infos.end() || info->second.is_integer) {
            // Name the exact attribute: the rejected shape must be visible
            // without a debugger (regIndex, format, layout, and the program
            // metadata the pipeline layout is built from).
            printf("[gxm-reject] vertex attribute regIndex=%u stream=%u format=%u(%s) components=%u offset=%u stride=%zu element=%u metadata=%s\n",
                unsigned(a.regIndex), unsigned(a.streamIndex), unsigned(a.format),
                attribute_format_name(a.format), unsigned(a.componentCount), a.offset, stride,
                element, info == vp->renderer_data->attribute_infos.end() ? "missing" : "present");
            if (info != vp->renderer_data->attribute_infos.end())
                printf("[gxm-reject] vertex attribute metadata location=%u type=%u componentCount=%u integer=%d signed=%d regformat=%d\n",
                    info->second.location, unsigned(info->second.gxm_type), unsigned(info->second.component_count),
                    info->second.is_integer, info->second.is_signed, info->second.regformat);
            unsupported("vertex attribute format");
        }
        attributes.push_back({info->second.location, a.offset, a.componentCount, uint32_t(a.format)});
    }
    const auto shader = [&](Ptr<const SceGxmProgram> ptr) {
        require_guest(mem, ptr.address(), sizeof(SceGxmProgram));
        const auto *gxp = ptr.get(mem);
        if (gxp->size < sizeof(SceGxmProgram) || gxp->size > 16 * 1024 * 1024)
            unsupported("GXP size");
        require_guest(mem, ptr.address(), gxp->size);
        return gxp;
    };
    const auto *vs = shader(vp->program), *fs = shader(fp->program);
    for (unsigned i = 0; i < 2; ++i) {
        const auto *p = i == 0 ? static_cast<const ShaderProgram *>(vp->renderer_data.get()) : fp->renderer_data.get();
        if (ctx.uniforms[i].size() != p->max_total_uniform_buffer_storage * 4)
            unsupported("missing guest shader uniforms");
    }
    std::vector<uint8_t> packet;
    const auto append = [&](const void *data, size_t size) {
        if (!size) return;
        const auto *bytes = static_cast<const uint8_t *>(data);
        packet.insert(packet.end(), bytes, bytes + size);
    };
    const auto word = [&](uint32_t value) { append(&value, 4); };
    // The bound fragment program carries the guest blend descriptor retained at
    // program creation (gxm_webgpu_program.h); the packet ships it in guest
    // units so the JS decoder owns the single GXM -> WebGPU translation, like
    // the texture sampler/format fields. Every draw reaches the consumer as a
    // triangle list: triangle fans are expanded above before the packet exists.
    const auto *const webgpu_fp = static_cast<const browser::WebGPUFragmentProgram *>(fp->renderer_data.get());
    const auto &blend = webgpu_fp->blend;
    const bool blend_enabled = blend.color_func != SCE_GXM_BLEND_FUNC_NONE
        || blend.alpha_func != SCE_GXM_BLEND_FUNC_NONE;
    // GXM5 fixed words: magic, stride, indexSize, six payload lengths,
    // attribute count, blend enabled u32, seven guest blend words (colorMask,
    // colorFunc, alphaFunc, colorSrc, colorDst, alphaSrc, alphaDst), depth
    // enabled u32, optional five depth words (format, compare, write mode,
    // load mode 0=clear, clear value f32), texture count (0/1), optional eight
    // texture words, viewport flat u32, viewport
    // xOffset,yOffset,zOffset,xScale,yScale,zScale f32 bits. Then render info
    // (48 bytes), four words per attribute (location, offset, componentCount,
    // guest SceGxmAttributeFormat), the six payloads, and packed texture bytes.
    // The vertex shader consumes only flip/flag/screen/z from render info; x/y
    // mapping is the WebGPU viewport, computed in JS from these exact GXM
    // floats. GXM4 is no longer accepted: native and JS deploy together, old
    // packets must fail loudly. All words are little-endian wasm u32.
    word(0x47584d35); word(stride); word(index_size);
    for (auto size : {index_bytes, stream.size, size_t(vs->size), size_t(fs->size), ctx.uniforms[0].size(), ctx.uniforms[1].size()}) word(size);
    word(attributes.size());
    word(blend_enabled ? 1u : 0u);
    word(blend.color_mask); word(blend.color_func); word(blend.alpha_func);
    word(blend.color_src); word(blend.color_dst); word(blend.alpha_src); word(blend.alpha_dst);
    word(ctx.has_depth ? 1u : 0u);
    if (ctx.has_depth) {
        const auto &depth = ctx.record.depth_stencil_surface;
        word(static_cast<uint32_t>(depth.get_format()));
        word(static_cast<uint32_t>(ctx.record.front_depth_func));
        word(static_cast<uint32_t>(ctx.record.front_depth_write_mode));
        // Load mode is always clear: force_load was rejected when the surface
        // was recorded, so the draw starts from background_depth.
        word(0);
        const float clear_depth = depth.background_depth;
        append(&clear_depth, sizeof(clear_depth));
    }
    word(textured ? 1 : 0);
    if (textured) {
        const auto &t = ctx.fragment_texture;
        word(texture_width); word(texture_height); word(gxm::get_format(t));
        word(t.min_filter); word(t.mag_filter); word(t.uaddr_mode); word(t.vaddr_mode);
        word(texture_pixels.size());
    }
    word(ctx.record.viewport_flat ? 1u : 0u);
    append(ctx.viewport.data(), sizeof(float) * 6);
    // RenderVertUniformBlock fields, mirroring gl/draw.cpp from record state:
    // flip, flat?0:1 flag, surface dimensions, z offset/scale. Previously the
    // flag was hardcoded to 1, mis-describing flat viewports to the shader.
    const float *flip = ctx.record.viewport_flip.data();
    const float info[12] = {flip[0], flip[1], flip[2], flip[3],
        ctx.record.viewport_flat ? 0.0f : 1.0f, float(w), float(height),
        ctx.record.z_offset, ctx.record.z_scale, 0, 0, 0};
    append(info, sizeof(info));
    for (const auto &a : attributes) for (auto value : a) word(value);
    append(fan_indices.empty() ? indices.get(mem) : fan_indices.data(), index_bytes);
    append(stream.data.get(mem), stream.size);
    append(vs, vs->size); append(fs, fs->size);
    for (const auto &data : ctx.uniforms) append(data.data(), data.size());
    append(texture_pixels.data(), texture_pixels.size());
    return web_gxm_draw(packet.data(), packet.size(), w, height, surface.strideInPixels * 4, surface.data.get(mem));
}

void submit_command_list(State &state, Context *ctx, CommandList &list) {
    // Reject unsupported opcodes before publishing any batch completion.
    // Signal/WaitSyncObject and NewFrame are the display-queue/sync slice:
    // steady-state waits are already signaled, and NewFrame only records the
    // predicted frame (presentation is a later slice). Everything else still
    // rejects here instead of partially executing the batch.
    for (Command *cmd = list.first; cmd; cmd = cmd->next) {
        if (cmd->opcode != CommandOpcode::Nop && cmd->opcode != CommandOpcode::TransferFill
            && cmd->opcode != CommandOpcode::SignalNotification && cmd->opcode != CommandOpcode::SetContext
            && cmd->opcode != CommandOpcode::SetState && cmd->opcode != CommandOpcode::Draw
            && cmd->opcode != CommandOpcode::SyncSurfaceData && cmd->opcode != CommandOpcode::SignalSyncObject
            && cmd->opcode != CommandOpcode::WaitSyncObject && cmd->opcode != CommandOpcode::NewFrame) {
            trace_scene(list, static_cast<WebState &>(state).mem);
            unsupported("command opcode not implemented");
        }
    }
    if (!list.first) return;
    int result = 0;
    std::exception_ptr failure;
    Command *cmd = list.first;
    reset_command_list(list);
    while (cmd) {
        Command *next = cmd->next;
        CommandHelper helper(cmd);
        int code = 0;
        try {
        if (result == 0) switch (cmd->opcode) {
        case CommandOpcode::SetContext: {
            if (!ctx) unsupported("scene without context");
            auto &web = static_cast<WebContext &>(*ctx);
            auto *target = helper.pop<RenderTarget *>();
            const auto *color = helper.pop<SceGxmColorSurface *>();
            const auto *depth = helper.pop<SceGxmDepthStencilSurface *>();
            if (!target || !color || color->disabled || color->downscale || color->gamma
                || color->colorFormat != SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR
                || color->surfaceType != SCE_GXM_COLOR_SURFACE_LINEAR
                || !color->width || !color->height || color->width > 4096 || color->height > 4096
                || color->strideInPixels < color->width || color->strideInPixels > UINT32_MAX / 4) {
                printf("[gxm-reject] surface target=%d color=%d depth=%d\n",
                    target != nullptr, color != nullptr, depth != nullptr);
                if (color) {
                    printf("[gxm-reject] color format=%08x type=%u width=%u height=%u stride=%u disabled=%d downscale=%u gamma=%u\n",
                        unsigned(color->colorFormat), unsigned(color->surfaceType), color->width, color->height,
                        color->strideInPixels, bool(color->disabled), unsigned(color->downscale), unsigned(color->gamma));
                }
                unsupported("color surface format");
            }
            require_guest(static_cast<WebState &>(state).mem, color->data.address(),
                (uint64_t(color->height) - 1) * color->strideInPixels * 4 + uint64_t(color->width) * 4);
            web.record.color_surface = *color; web.has_surface = true;
            ctx->current_render_target = target;
            // Depth-stencil attachment, mirroring scene.cpp handle_set_context:
            // a null or disabled guest surface clears the recorded addresses.
            // The attachment always matches the color surface dimensions, which
            // is what a single GXM render target guarantees.
            web.has_depth = false;
            web.record.depth_stencil_surface = SceGxmDepthStencilSurface{};
            if (depth && !depth->disabled()) {
                const auto format = depth->get_format();
                const uint32_t bytes = depth_bytes_per_sample(format);
                // Validation bound, not a layout model: the guest depth memory is
                // never read or written (force_load/force_store are rejected
                // below), so LINEAR and TILED differ only in a buffer the
                // consumer does not touch. A tiled allocation is never smaller
                // than this row-major estimate, so the check cannot false-reject.
                const uint64_t footprint = uint64_t(depth->get_stride()) * bytes * color->height;
                const Address data = depth->depth_data.address();
                if (!supported_depth_format(format) || !depth->depth_data
                    || depth->get_stride() < color->width || !std::isfinite(depth->background_depth)
                    || depth->background_depth < 0.0f || depth->background_depth > 1.0f
                    || !footprint || footprint > 64 * 1024 * 1024
                    || !is_valid_addr_range(static_cast<WebState &>(state).mem, data, uint64_t(data) + footprint)) {
                    printf("[gxm-reject] depth tiling=%s format=%08x strideSamples=%u depth=%d stencil=%d forceLoad=%d forceStore=%d background=%g footprint=%llu\n",
                        depth->get_type() == SCE_GXM_DEPTH_STENCIL_SURFACE_TILED ? "tiled" : "linear",
                        unsigned(format), depth->get_stride(),
                        depth->depth_data.address() != 0, depth->stencil_data.address() != 0,
                        bool(depth->force_load), bool(depth->force_store), double(depth->background_depth),
                        static_cast<unsigned long long>(footprint));
                    unsupported("depth surface format/layout");
                }
                // force_load means the previous depth-stencil contents must be
                // preserved; force_store means they must be written back to
                // guest memory. The consumer has a per-draw attachment and no
                // depth readback, so both reject instead of losing contents.
                if (depth->force_load || depth->force_store)
                    unsupported(depth->force_load ? "depth force load (guest depth contents)"
                                                 : "depth force store (guest depth writeback)");
                web.record.depth_stencil_surface = *depth;
                web.has_depth = true;
            }
            break;
        }
        case CommandOpcode::SetState:
            if (!ctx) unsupported("state without context");
            consume_state(static_cast<WebContext &>(*ctx), helper, static_cast<WebState &>(state).mem);
            break;
        case CommandOpcode::Draw:
            if (!ctx) unsupported("draw without context");
            result = consume_draw(static_cast<WebContext &>(*ctx), helper, static_cast<WebState &>(state).mem);
            break;
        case CommandOpcode::SyncSurfaceData: {
            // Draw readback is awaited before execution reaches notifications.
            // Mirror scene.cpp signal_notifications: publish under the mutex
            // and wake sceGxmNotificationWait. No notify on validation failure.
            auto &mem = static_cast<WebState &>(state).mem;
            const auto vertex = helper.pop<SceGxmNotification>(), fragment = helper.pop<SceGxmNotification>();
            for (const auto &n : {vertex, fragment}) if (n.address)
                require_guest(mem, n.address.address(), sizeof(uint32_t));
            // Signal only when at least one waiter address exists, mirroring
            // the were_notifications_signaled guard (memory mapping is never
            // enabled on this backend; disable_surface_sync is false while
            // the draw readback await stands in for fence completion).
            if (vertex.address || fragment.address) {
                // Unlock before notifying, exactly like the desktop path.
                std::unique_lock<std::mutex> lock(state.notification_mutex);
                for (const auto &n : {vertex, fragment}) if (n.address) *n.address.get(mem) = n.value;
                lock.unlock();
                state.notification_ready.notify_all();
            }
            break;
        }
        case CommandOpcode::SignalSyncObject: {
            // EndScene emits this after SyncSurfaceData when a fragment sync
            // object is bound. Advance exactly like renderer::subject_done so
            // display-queue waits observe scene completion.
            const auto sync = helper.pop<Ptr<SceGxmSyncObject>>();
            const auto timestamp = helper.pop<uint32_t>();
            auto &mem = static_cast<WebState &>(state).mem;
            if (!sync) unsupported("sync signal without object");
            require_guest(mem, sync.address(), sizeof(SceGxmSyncObject));
            subject_done(sync.get(mem), timestamp);
            break;
        }
        case CommandOpcode::WaitSyncObject: {
            // BeginScene emits this for the bound fragment sync object. Steady
            // state is already signaled; genuine backpressure blocks with
            // desktop wishlist semantics rather than skipping the wait.
            const auto sync = helper.pop<Ptr<SceGxmSyncObject>>();
            const auto timestamp = helper.pop<uint32_t>();
            auto &mem = static_cast<WebState &>(state).mem;
            if (!sync) unsupported("sync wait without object");
            require_guest(mem, sync.address(), sizeof(SceGxmSyncObject));
            if (wishlist(sync.get(mem), timestamp) != SyncWaitResult::Ready)
                result = -1;
            break;
        }
        case CommandOpcode::NewFrame: {
            // sceGxmDisplayQueueAddEntry path (always sent with null context).
            // Record the predicted frame for a future presentation slice;
            // pixels are NOT presented yet. Mirrors sync.cpp new_frame minus
            // the backend-specific frame advance.
            auto *frame = helper.pop<DisplayFrameInfo *>();
            auto *display = helper.pop<DisplayState *>();
            helper.pop<Context *>();
            if (!display) unsupported("new frame without display state");
            if (frame) {
                const std::lock_guard<std::mutex> guard(display->display_info_mutex);
                display->next_rendered_frame = *frame;
                delete frame;
                state.should_display = true;
            }
            break;
        }
        case CommandOpcode::Nop:
            code = helper.pop<int>();
            result = web_gxm_fence();
            break;
        case CommandOpcode::TransferFill: {
            const uint32_t color = helper.pop<uint32_t>();
            const auto *d = helper.pop<const SceGxmTransferImage *>();
            auto &mem = static_cast<WebState &>(state).mem;
            const uint64_t start = uint64_t(d->address.address()) + uint64_t(d->y) * d->stride + uint64_t(d->x) * 4;
            const uint64_t end = start + uint64_t(d->height ? d->height - 1 : 0) * d->stride + uint64_t(d->width) * 4;
            if (d->format != SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR || !d->width || !d->height
                || d->width > 4096 || d->height > 4096 || d->stride <= 0
                || uint64_t(d->stride) < (uint64_t(d->x) + d->width) * 4
                || start > UINT32_MAX || end > uint64_t(UINT32_MAX) + 1
                || !is_valid_addr_range(mem, static_cast<Address>(start), end)) {
                result = -1;
            } else {
                result = web_gxm_fill(color, d->width, d->height, d->stride,
                    Ptr<void>(static_cast<Address>(start)).get(mem));
            }
            break;
        }
        case CommandOpcode::SignalNotification: {
            // Mirror sync.cpp handle_notification: publish under the mutex and
            // wake sceGxmNotificationWait. An invalid address fails the batch
            // without publishing, exactly as before.
            const auto n = helper.pop<SceGxmNotification>();
            auto &mem = static_cast<WebState &>(state).mem;
            if (n.address) {
                if (!is_valid_addr_range(mem, n.address.address(), uint64_t(n.address.address()) + sizeof(uint32_t)))
                    result = -1;
                else {
                    std::unique_lock<std::mutex> lock(state.notification_mutex);
                    *n.address.get(mem) = n.value;
                    lock.unlock();
                }
            }
            // handle_notification notifies unconditionally after the locked
            // publish; waiters re-check their own predicates.
            state.notification_ready.notify_all();
            break;
        }
        default: break; // Preflight above excludes all other opcodes.
        }
        } catch (...) {
            // Drain and free the detached batch even on validation failure.
            // No later notification/status may acknowledge a failed draw.
            failure = std::current_exception();
            result = -1;
        }
        if (cmd->status) *cmd->status = result == 0 ? code : -1;
        destroy_command_payload(*cmd);
        if (ctx) ctx->free_func(cmd);
        else generic_command_free(cmd);
        cmd = next;
    }
    if (failure) std::rethrow_exception(failure);
    if (result != 0) unsupported("WebGPU command failed (see browser log)");
}
int wait_for_status(State &, int *status, int signal, bool equal) {
    if ((*status == signal) != equal) unsupported("uncompleted command");
    return *status;
}
void finish(State &s, Context *ctx) {
    printf("[vita3k-web] GXM finish entered\n");
    send_single_command(s, ctx, CommandOpcode::Nop, true, 1);
}
}
