// SPDX-License-Identifier: GPL-2.0-or-later
// Browser-only consumer of the production renderer command ABI. No GL/Vulkan.
//
// Each GXM command list (one scene) becomes one GXS1 stream for
// browser/web/gxm_scene.js: pass begin/end, draws with their fixed-function
// state, and the vertex/index/uniform/texture bytes the draws reference, all
// copied at submission so the guest may reuse its buffers immediately. The
// stream is submitted synchronously: render targets live on the GPU, sampled
// when a texture covers their texels, and are presented from there.
// Command-list completion (notifications, sync objects) is published as soon
// as the scene is submitted. Opt-in surface sync (VITA3K_SURFACE_SYNC=1, like
// desktop's disable-surface-sync=false) also reads each rendered target back
// into guest memory first, suspending the guest until the copy arrives.
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
#include <util/align.h>
#include <emscripten.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <fmt/format.h>
#include <algorithm>
#include <memory>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Device and GXP compiler: once, at sceGxmInitialize (the calling guest thread
// may suspend). Benchmark-only VITA3K_NULL_GPU (see host_abi.js) keeps every
// scene call a no-op so the CPU path runs under Node.
EM_ASYNC_JS(int, web_gxm_init, (), {
    if (Module['vita3kNullGpu']) return 0;
    try {
        const scene = await import(new URL('gxm_scene.js', globalThis.location.href).href);
        const base = globalThis.location.href;
        await scene.init({
            compilerURL: new URL('shaders/gxp_compiler.mjs', base).href,
            nagaURL: new URL('shaders/naga.wasm', base).href,
            wasiShimURL: new URL('shaders/wasi/index.js', base).href,
            logger: message => err(message),
        });
        Module['vita3kGxm'] = scene;
        if (globalThis.vita3kGxmReady) globalThis.vita3kGxmReady(scene);
        return 0;
    } catch (error) {
        err('[vita3k-web] GXM WebGPU initialization failed: ' + (error.stack || error));
        return -1;
    }
});
// GXP -> WGSL for one program, the first time a draw uses it.
EM_ASYNC_JS(int, web_gxm_register_program, (uint32_t id, const void *gxp, uint32_t size, int fragment), {
    const scene = Module['vita3kGxm'];
    if (!scene) return 0;
    try {
        await scene.registerProgram(id, Module['vita3kHostBytes'](gxp, size).slice(), fragment !== 0);
        return 0;
    } catch (error) {
        err('[vita3k-web] GXP program ' + id + ' translation failed: ' + (error.stack || error));
        return -1;
    }
});
EM_JS(int, web_gxm_submit, (const uint32_t *words, uint32_t count, const uint8_t *data, uint32_t size), {
    const scene = Module['vita3kGxm'];
    if (!scene) return 0;
    try {
        const offset = Module['vita3kHostOffset'](words, count * 4);
        scene.submitScene(new Uint32Array(wasmMemory.buffer, offset, count),
            Module['vita3kHostBytes'](data, size));
        return 0;
    } catch (error) {
        err('[vita3k-web] GXM scene submission failed: ' + (error.stack || error));
        return -1;
    }
});

// Surface sync: copies the render target at `address`, box-filtered to its
// guest size, as tight rows of guest texels into the host buffer `dest`
// (the caller stores them in the surface's layout). The guest thread
// suspends until the GPU copy is mapped.
EM_ASYNC_JS(int, web_gxm_sync_surface, (uint32_t address, uint8_t *dest, uint32_t width, uint32_t height,
    uint32_t pixel_bytes), {
    const scene = Module['vita3kGxm'];
    if (!scene) return 0;
    try {
        await scene.readTarget(address >>> 0, width, height, pixel_bytes, (mapped, bytesPerRow) => {
            const row = width * pixel_bytes;
            const rows = Module['vita3kHostBytes'](dest, height * row);
            for (let y = 0; y < height; ++y)
                rows.set(mapped.subarray(y * bytesPerRow, y * bytesPerRow + row), y * row);
        });
        return 0;
    } catch (error) {
        err('[vita3k-web] GXM surface sync of ' + (address >>> 0).toString(16) + ' failed: ' + (error.stack || error));
        return -1;
    }
});

// Presents the GPU render target at `address` (1) or reports that none exists
// there (0). Frames go to the Worker's page hook; pixels are read back only
// every Module.VITA3K_FRAME_READBACK frames (0 = never; default every frame
// when the page attached no canvas, else never).
EM_JS(int, web_gxm_present, (uint32_t address), {
    const scene = Module['vita3kGxm'];
    if (!scene) return 0;
    const configured = Module['VITA3K_FRAME_READBACK'];
    const every = configured !== undefined ? Number(configured) : (globalThis.vita3kHasCanvas ? 0 : 1);
    return scene.presentTarget(address >>> 0, (generation, width, height, pixels) => {
        if (globalThis.vita3kWebOnGpuFrame) globalThis.vita3kWebOnGpuFrame(generation, width, height, pixels);
    }, every) ? 1 : 0;
});

// Benchmark survey (VITA3K_GXM_SURVEY=1 with VITA3K_NULL_GPU=1): a command the
// consumer cannot represent is counted and skipped instead of failing the
// guest thread, so one Node run lists every GXM feature a title needs.
EM_JS(int, web_gxm_survey_enabled, (), {
    return (Module['vita3kNullGpu'] && (Module['VITA3K_GXM_SURVEY'] === '1'
        || (typeof process !== 'undefined' && process.env?.VITA3K_GXM_SURVEY === '1'))) ? 1 : 0;
});

namespace {
// Host milliseconds per stage, reported with the run progress.
struct Timing {
    double build = 0, decode = 0, submit = 0, sync = 0;
    unsigned decodes = 0, hashes = 0, clean = 0, untracked = 0, syncs = 0;
};
// VITA3K_TEXTURE_VERIFY=1: hash every bound texture even when no write was
// tracked, and report changes the write tracking missed.
// Internal resolution: surfaces whose render pixels (their size, doubled when
// downscaled) are at least the display size are rendered at
// VITA3K_RESOLUTION_SCALE times that in each dimension (default 2: 960x544 ->
// 1920x1088). Smaller intermediate surfaces (blur and bloom
// chains) stay at guest resolution: titles sample them with offsets of one
// guest texel, which upscaled would skip rows (Limbo's blur atlas streaks).
// gxm_scene.js scales the GPU resources; shaders divide gl_FragCoord by the
// surface's scale (res_multiplier).
uint32_t resolution_scale() {
    static const uint32_t scale = [] {
        const char *value = std::getenv("VITA3K_RESOLUTION_SCALE");
        const unsigned long parsed = value ? std::strtoul(value, nullptr, 10) : 2;
        return static_cast<uint32_t>(parsed >= 1 && parsed <= 4 ? parsed : 2);
    }();
    return scale;
}
// `width` x `height`: the surface's render pixels. The GPU texture (render
// pixels times the scale) stays within WebGPU's default 8192 texel limit.
uint32_t surface_scale(uint32_t width, uint32_t height) {
    if (width < 960 || height < 544)
        return 1;
    return std::max<uint32_t>(1, std::min({resolution_scale(), 8192 / width, 8192 / height}));
}
// VITA3K_SURFACE_SYNC=1: every scene that drew into a color surface reads the
// target back into guest memory before its completions are published, for
// titles that read rendered pixels with the CPU. Off by default: each sync
// stalls the guest on a GPU round trip.
bool surface_sync() {
    static const bool enabled = [] {
        const char *value = std::getenv("VITA3K_SURFACE_SYNC");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
bool texture_verify() {
    static const bool enabled = std::getenv("VITA3K_TEXTURE_VERIFY") != nullptr;
    return enabled;
}
Timing &timing() {
    static Timing t;
    return t;
}
bool survey_mode() {
    static const bool enabled = web_gxm_survey_enabled() != 0;
    return enabled;
}
std::map<std::string, uint64_t> &survey_counts() {
    static std::map<std::string, uint64_t> counts;
    return counts;
}
[[noreturn]] void unsupported(const char *what) {
    throw std::runtime_error(std::string("WebGPU GXM unsupported: ") + what);
}
// A draw the consumer cannot represent yet is skipped and reported once per
// reason (the scene and the guest thread continue), like the desktop backends.
// Out of line with a C string: the draw path then holds no std::string whose
// cleanup would route its calls through invoke_* wrappers.
[[gnu::noinline]] void skip_draw(const char *why, const char *detail = "") {
    static std::string reason;
    reason.assign(why).append(detail);
    auto &count = survey_counts()[reason];
    if (count++ == 0)
        std::printf("[gxm-skip] %s%s\n", why, detail);
}

// --- Surfaces in guest memory ---------------------------------------------------
// Where a color surface's texels live in guest memory. Rendering on the GPU
// is layout-independent; everything that touches guest memory (surface sync,
// textures aliasing a target, transfers) addresses texels through this.
enum class Layout : uint32_t { Linear, Tiled, Swizzled };
struct SurfaceGeometry {
    uint32_t width = 0, height = 0, stride_px = 0, pixel_bytes = 0;
    Layout layout = Layout::Linear;
    // Tiled: 32x32-texel tiles of 1024 contiguous texels, tile rows
    // stride_px / 32 tiles wide (renderer/src/transfer.cpp compute_offset).
    // Swizzled: Morton order over the power-of-two surface (encode_morton).
    uint64_t texel_index(uint32_t x, uint32_t y) const {
        switch (layout) {
        case Layout::Tiled:
            return (uint64_t((y >> 5) * (stride_px >> 5) + (x >> 5)) << 10) | ((y & 31) << 5) | (x & 31);
        case Layout::Swizzled:
            return renderer::texture::encode_morton(uint16_t(x), uint16_t(y), uint16_t(width), uint16_t(height));
        default:
            return uint64_t(y) * stride_px + x;
        }
    }
    uint64_t byte_offset(uint32_t x, uint32_t y) const { return texel_index(x, y) * pixel_bytes; }
    // Bytes from the surface base to the end of its last texel.
    uint64_t footprint() const {
        switch (layout) {
        case Layout::Tiled: return uint64_t((height + 31) >> 5) * (stride_px >> 5) * 1024 * pixel_bytes;
        case Layout::Swizzled: return uint64_t(width) * height * pixel_bytes;
        default: return (uint64_t(height) - 1) * stride_px * pixel_bytes + uint64_t(width) * pixel_bytes;
        }
    }
    // Texel of a byte offset inside the footprint; false for padding (a
    // stride gap, tile texels past the width or height).
    bool texel_at(uint64_t offset, uint32_t &x, uint32_t &y) const {
        if (offset % pixel_bytes)
            return false;
        const uint64_t index = offset / pixel_bytes;
        switch (layout) {
        case Layout::Tiled: {
            const uint64_t tile = index >> 10, row_tiles = stride_px >> 5;
            if (!row_tiles) return false;
            x = uint32_t((tile % row_tiles) * 32 + (index & 31));
            y = uint32_t((tile / row_tiles) * 32 + ((index >> 5) & 31));
            break;
        }
        case Layout::Swizzled: {
            // As renderer/src/texture/format.cpp swizzled_texture_to_linear_texture.
            const uint32_t min = std::min(width, height), k = std::bit_width(min) - 1;
            x = renderer::texture::decode_morton2_x(uint32_t(index)) & (min - 1);
            y = renderer::texture::decode_morton2_y(uint32_t(index)) & (min - 1);
            const uint32_t upper = uint32_t(index >> (2 * k)) << k;
            (width >= height ? x : y) |= upper;
            break;
        }
        default:
            x = uint32_t(index % stride_px);
            y = uint32_t(index / stride_px);
            break;
        }
        return x < width && y < height;
    }
};
struct TextureUnit {
    bool bound = false;
    SceGxmTexture texture{};
};

struct WebContext final : renderer::Context {
    bool has_surface = false;
    bool has_viewport = false;
    std::array<float, 6> viewport{};  // xOffset, yOffset, zOffset, xScale, yScale, zScale
    std::array<std::vector<uint8_t>, 2> uniforms;
    std::array<TextureUnit, 16> fragment_textures{};
    std::array<TextureUnit, 16> vertex_textures{};
    SceGxmDepthStencilSurface depth{};
    bool has_depth_surface = false;
    // The open scene's pixel spaces (renderer/src/state_set.cpp viewport and
    // region_clip): guest coordinates times `samples` are render pixels (2
    // with a multisampled render target, whose samples each get a pixel);
    // render pixels are `downscale` times the surface's pixels (2 with
    // SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE, box-filtered into memory).
    SurfaceGeometry geometry;
    uint32_t samples = 1, downscale = 1, internal_scale = 1;
    std::array<uint32_t, 4> region_clip{}; // guest x_min, x_max, y_min, y_max
};

struct WebState final : renderer::State {
    MemState &mem;
    explicit WebState(MemState &m) : mem(m) {
        current_backend = renderer::Backend::WebGPU;
        context = nullptr; res_multiplier = 1; disable_surface_sync = !surface_sync();
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
} // namespace

namespace browser {
void gxm_timing_report() {
    const auto &t = timing();
    std::printf("[gxm] scene_ms=%.0f (decode_ms=%.0f decodes=%u hashes=%u clean=%u untracked=%u js_submit_ms=%.0f)",
        t.build, t.decode, t.decodes, t.hashes, t.clean, t.untracked, t.submit);
    if (surface_sync())
        std::printf(" surface_syncs=%u surface_sync_ms=%.0f", t.syncs, t.sync);
    std::printf("\n");
}
void gxm_survey_report() {
    for (const auto &[reason, count] : survey_counts())
        std::printf("[gxm-%s] %8llu %s\n", survey_mode() ? "survey" : "skip",
            static_cast<unsigned long long>(count), reason.c_str());
}
int gxm_initialize(EmuEnvState &env) {
    if (env.renderer) return SCE_GXM_ERROR_ALREADY_INITIALIZED;
    if (web_gxm_init() != 0) return SCE_GXM_ERROR_DRIVER;
    env.renderer = std::make_unique<WebState>(env.mem);
    if (surface_sync())
        std::puts("[vita3k-web] GXM surface sync: on (rendered targets are read back into guest memory)");
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
    } else if (cmd.opcode == CommandOpcode::TransferCopy) {
        // renderer.cpp transfer_copy: key value, key mask, key mode, images[2].
        CommandHelper h(&cmd); h.pop<uint32_t>(); h.pop<uint32_t>(); h.pop<SceGxmTransferColorKeyMode>();
        delete[] h.pop<const SceGxmTransferImage *>();
    } else if (cmd.opcode == CommandOpcode::TransferDownscale) {
        CommandHelper h(&cmd);
        delete h.pop<const SceGxmTransferImage *>();
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
// Multisampled targets render one pixel per sample (see WebContext::samples),
// as the desktop renderers do.
bool create_render_target(State &, std::unique_ptr<RenderTarget> &rt, const SceGxmRenderTargetParams *p) {
    if (p->multisampleMode > SCE_GXM_MULTISAMPLE_4X || !p->width || !p->height || p->width > 4096 || p->height > 4096) return false;
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

// --- GXS1 scene stream (browser/web/gxm_scene.js is the only consumer) -------
namespace scene {
constexpr uint32_t kMagic = 0x31535847; // "GXS1"
enum Command : uint32_t { BeginPass = 1, Draw = 2, Texture = 3, EndPass = 4, Region = 6, WriteTexels = 7 };
// Dynamic uniform/storage offsets must honour WebGPU's 256-byte minimum.
constexpr size_t kUniformAlign = 256;

struct Writer {
    std::vector<uint32_t> words;
    // Payload bytes. Appends stay inline; growth is the only call, so
    // callers with cleanups do not reach every append through a JS invoke_*
    // wrapper (Emscripten JS exceptions) as they did with vector::resize.
    struct Bytes {
        std::unique_ptr<uint8_t[]> buffer;
        size_t used = 0, capacity = 0;
        uint8_t *data() const { return buffer.get(); }
        size_t size() const { return used; }
        void clear() { used = 0; }
        // Extends to offset + size (zero-filling the alignment gap) and
        // returns the bytes at offset.
        uint8_t *extend(size_t offset, size_t size) {
            if (offset + size > capacity)
                grow(offset + size);
            std::memset(buffer.get() + used, 0, offset - used);
            used = offset + size;
            return buffer.get() + offset;
        }
        [[gnu::noinline]] void grow(size_t needed) {
            const size_t grown = std::max(needed, capacity * 2 + 65536);
            auto replacement = std::make_unique<uint8_t[]>(grown);
            if (used)
                std::memcpy(replacement.get(), buffer.get(), used);
            buffer = std::move(replacement);
            capacity = grown;
        }
    } data;
    bool pass_open = false;
    // Open pass: its color address and the index of its snapshot flag word.
    Address pass_address = 0;
    size_t pass_begin_word = 0, pass_snapshot_word = 0;
    std::vector<uint32_t> pass_regions; // region texture ids copied for the open pass
    // Commands that must run before the open pass begins (target region copies).
    void insert_before_pass(std::initializer_list<uint32_t> command) {
        words.insert(words.begin() + pass_begin_word, command);
        for (auto &stream : stream_sites)
            if (stream.site >= pass_begin_word)
                stream.site += static_cast<uint32_t>(command.size());
        pass_begin_word += command.size();
        pass_snapshot_word += command.size();
    }
    uint32_t draws = 0;
    uint32_t pass_first_draw = 0; // `draws` when the open pass began
    // Vertex streams: Vita3K sizes a stream from its base to the draw's
    // largest index, and a frame's draws index one growing buffer, so copying
    // per draw re-sends the same prefix many times. Draws of this submission
    // that read the same base share one copy of the largest range, taken when
    // the submission is sent (the guest is stopped until then) or before the
    // bridge itself writes guest memory.
    // A flat list, grouped by base when copied: a hash map would allocate a
    // node per base and submission.
    struct StreamSite { uint32_t site; Address base; uint32_t size; }; // site: offset word
    std::vector<StreamSite> stream_sites;
    void reset() {
        words.assign(1, kMagic);
        data.clear();
        pass_open = false;
        draws = 0;
        stream_sites.clear();
    }
    // Emits the scene-data offset of guest bytes [base, base + size) as a
    // word filled in by copy_streams().
    void stream(Address base, uint32_t size) {
        stream_sites.push_back({static_cast<uint32_t>(words.size()), base, size});
        words.push_back(0);
    }
    void copy_streams(MemState &mem) {
        std::sort(stream_sites.begin(), stream_sites.end(),
            [](const StreamSite &a, const StreamSite &b) { return a.base < b.base; });
        for (size_t first = 0; first < stream_sites.size();) {
            size_t last = first;
            uint32_t size = 0;
            for (; last < stream_sites.size() && stream_sites[last].base == stream_sites[first].base; ++last)
                size = std::max(size, stream_sites[last].size);
            const uint32_t offset = bytes(Ptr<const uint8_t>(stream_sites[first].base).get(mem), size, 4);
            for (; first < last; ++first)
                words[stream_sites[first].site] = offset;
        }
        stream_sites.clear();
    }
    // Not noexcept: the draw path holds no locals with destructors, so these
    // are direct calls, and noexcept would add a terminate landing pad (an
    // invoke_* wrapper) around their growth calls.
    void word(uint32_t value) { words.push_back(value); }
    void real(float value) { words.push_back(std::bit_cast<uint32_t>(value)); }
    uint32_t bytes(const void *source, size_t size, size_t alignment) {
        const size_t offset = align(data.size(), alignment);
        uint8_t *dest = data.extend(offset, size);
        if (size)
            std::memcpy(dest, source, size);
        return static_cast<uint32_t>(offset);
    }
    // Reserve `size` bytes and return a pointer the caller fills in place.
    uint8_t *reserve(size_t size, size_t alignment, uint32_t &offset) {
        offset = static_cast<uint32_t>(align(data.size(), alignment));
        return data.extend(offset, size);
    }
};
Writer &writer() {
    static Writer instance;
    return instance;
}
} // namespace scene

// Program ids: one per distinct GXP (renderer_data hash). Translation happens
// on first use; a failed translation disables only draws using that program.
struct ProgramRegistry {
    std::map<Sha256Hash, uint32_t> ids;
    std::map<uint32_t, bool> ready; // id -> translated successfully
};
ProgramRegistry &programs() {
    static ProgramRegistry registry;
    return registry;
}
static int program_id(MemState &mem, const renderer::ShaderProgram &program, Ptr<const SceGxmProgram> gxp, bool fragment) {
    auto &registry = programs();
    auto [it, inserted] = registry.ids.emplace(program.hash, static_cast<uint32_t>(registry.ids.size() + 1));
    const uint32_t id = it->second;
    if (inserted) {
        const auto *bytes = gxp.get(mem);
        registry.ready[id] = web_gxm_register_program(id, bytes, bytes->size, fragment ? 1 : 0) == 0;
    }
    return registry.ready[id] ? int(id) : -1;
}

// The layout of a color surface, or false with a reason when the consumer
// cannot address it.
static bool surface_geometry(const SceGxmColorSurface &color, SurfaceGeometry &g, const char *&why) {
    g.width = color.width;
    g.height = color.height;
    g.stride_px = color.strideInPixels;
    g.pixel_bytes = uint32_t(gxm::bits_per_pixel(
        static_cast<SceGxmColorBaseFormat>(color.colorFormat & SCE_GXM_COLOR_BASE_FORMAT_MASK)) / 8);
    switch (color.surfaceType) {
    case SCE_GXM_COLOR_SURFACE_LINEAR: g.layout = Layout::Linear; break;
    case SCE_GXM_COLOR_SURFACE_TILED:
        g.layout = Layout::Tiled;
        if (g.stride_px % 32) { why = "tiled color surface whose stride is not whole tiles"; return false; }
        break;
    case SCE_GXM_COLOR_SURFACE_SWIZZLED:
        g.layout = Layout::Swizzled;
        if (!std::has_single_bit(g.width) || !std::has_single_bit(g.height)) {
            why = "swizzled color surface without power-of-two size";
            return false;
        }
        break;
    default: why = "color surface type"; return false;
    }
    if (!g.pixel_bytes) { why = "color surface format"; return false; }
    return true;
}

// --- Textures -----------------------------------------------------------------
// Render targets the scene stream has rendered, by guest color address. A
// texture whose texels are one's texels samples the GPU target (or a copy of
// the rectangle it covers) instead of memory.
struct RenderedTarget {
    SurfaceGeometry geometry;
    SceGxmColorBaseFormat format = SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8;
    // Guest writes from this write epoch on are not the GPU's: rendering
    // starts it, and the bridge's own writes of target texels (surface sync,
    // transfers) move it past themselves.
    uint32_t rendered_epoch = 0;
};
std::unordered_map<Address, RenderedTarget> &rendered_targets() {
    static std::unordered_map<Address, RenderedTarget> targets;
    return targets;
}

// A texture is rehashed only when a page of its guest bytes was written
// (MemState write epochs) since the submission that last checked it.
struct CachedTexture {
    uint32_t id = 0;
    uint64_t hash = 0;
    uint32_t checked_epoch = 0; // 0 = never checked
    Address source = 0;
    uint32_t footprint = 0;
};
struct TextureCache {
    std::unordered_map<uint64_t, CachedTexture> entries;
    uint32_t next_id = 1;
    uint32_t epoch = 0; // write epoch of the submission being built
};
TextureCache &texture_cache() {
    static TextureCache cache;
    return cache;
}
// After the bridge itself wrote guest memory: later writes get a new epoch,
// which rendered targets and texture checks compare against.
static uint32_t advance_write_epoch(MemState &mem) {
    return texture_cache().epoch = mem_next_write_epoch(mem);
}

// Output channel sources: 0..3 = decoded component, 4 = zero, 5 = one.
using ChannelMap = std::array<uint8_t, 4>;
constexpr uint8_t Z = 4, O = 5;
static bool swizzle_map(SceGxmTextureBaseFormat base, uint32_t swizzle, ChannelMap &map) {
    switch (base) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8: {
        static constexpr ChannelMap one[8] = {{0, Z, Z, O}, {0, Z, Z, Z}, {0, O, O, O}, {0, 0, 0, 0},
            {0, 0, 0, Z}, {0, 0, 0, O}, {Z, Z, Z, 0}, {O, O, O, 0}}; // R 000R 111R RRRR 0RRR 1RRR R000 R111
        map = one[(swizzle >> 12) & 7];
        return true;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8: {
        static constexpr ChannelMap two[6] = {{0, 1, Z, O}, {0, 1, Z, Z}, {0, 0, 0, 1}, {1, 1, 1, 0},
            {0, 1, 0, 1}, {1, 0, Z, Z}}; // GR 00GR GRRR RGGG GRGR 00RG
        const uint32_t mode = (swizzle >> 12) & 7;
        if (mode >= 6)
            return false;
        map = two[mode];
        return true;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5: {
        // Components are decoded in memory order R(low) G B A(high).
        static constexpr ChannelMap four[8] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {1, 2, 3, 0},
            {0, 1, 2, O}, {2, 1, 0, O}, {3, 2, 1, O}, {1, 2, 3, O}}; // ABGR ARGB RGBA BGRA 1BGR 1RGB RGB1 BGR1
        map = four[(swizzle >> 12) & 7];
        return true;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5: {
        static constexpr ChannelMap three[2] = {{0, 1, 2, O}, {2, 1, 0, O}}; // BGR RGB
        const uint32_t mode = (swizzle >> 12) & 7;
        if (mode >= 2)
            return false;
        map = three[mode];
        return true;
    }
    default:
        return false;
    }
}
// Decodes one texel of an uncompressed base format into 4 unorm8 components.
static void decode_texel(SceGxmTextureBaseFormat base, const uint8_t *src, uint8_t out[4]) {
    switch (base) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8: out[0] = src[0]; break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8: out[0] = uint8_t(int8_t(src[0]) + 128); break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8: out[0] = src[0]; out[1] = src[1]; break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8: out[0] = uint8_t(int8_t(src[0]) + 128); out[1] = uint8_t(int8_t(src[1]) + 128); break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8: std::memcpy(out, src, 4); break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4: {
        const uint16_t v = uint16_t(src[0] | (src[1] << 8));
        for (int i = 0; i < 4; ++i) out[i] = uint8_t(((v >> (4 * i)) & 15) * 17);
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5: {
        const uint16_t v = uint16_t(src[0] | (src[1] << 8));
        for (int i = 0; i < 3; ++i) out[i] = uint8_t((((v >> (5 * i)) & 31) * 255 + 15) / 31);
        out[3] = (v >> 15) ? 255 : 0;
        break;
    }
    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5: {
        const uint16_t v = uint16_t(src[0] | (src[1] << 8));
        out[0] = uint8_t(((v & 31) * 255 + 15) / 31);
        out[1] = uint8_t((((v >> 5) & 63) * 255 + 31) / 63);
        out[2] = uint8_t((((v >> 11) & 31) * 255 + 15) / 31);
        break;
    }
    default: break;
    }
}

// RGBA8 mip chain of a 2D texture, laid out like renderer/src/texture/cache.cpp
// upload_texture (levels, alignment and swizzle/tiled order), or false with a
// reason when the layout/format is not supported yet.
struct DecodedTexture {
    Address source = 0;     // guest bytes the chain is decoded from
    uint32_t footprint = 0;
    uint32_t width = 0, height = 0, levels = 0;
    std::array<std::pair<uint32_t, uint32_t>, 13> level_bytes; // offset, size in the scene data (4096 = 13 levels)
};
// `known_hash`: source hash of the copy the GPU already has; when the guest
// bytes still hash to it, nothing is decoded and `decoded.levels` stays 0.
static bool decode_texture(MemState &mem, const SceGxmTexture &t, scene::Writer &out, DecodedTexture &decoded,
    uint64_t known_hash, uint64_t &source_hash, const char *&why) {
    const auto type = t.texture_type();
    const auto format = gxm::get_format(t);
    const auto base = gxm::get_base_format(format);
    ChannelMap map;
    if (!swizzle_map(base, format & SCE_GXM_TEXTURE_SWIZZLE_MASK, map)) {
        static std::map<uint32_t, std::string> reasons;
        auto &reason = reasons[uint32_t(format)];
        if (reason.empty())
            reason = fmt::format("texture base format/swizzle {:#010x}", uint32_t(format));
        why = reason.c_str();
        return false;
    }
    if (type == SCE_GXM_TEXTURE_CUBE || type == SCE_GXM_TEXTURE_CUBE_ARBITRARY) {
        why = "cube texture";
        return false;
    }
    const bool swizzled = type == SCE_GXM_TEXTURE_SWIZZLED || type == SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY;
    const uint32_t width = gxm::get_width(t), height = gxm::get_height(t);
    const uint32_t bpp = gxm::bits_per_pixel(base), bytes_per_pixel = bpp / 8;
    if (!width || !height || width > 4096 || height > 4096 || !bytes_per_pixel) {
        why = "texture dimensions";
        return false;
    }
    const uint32_t max_levels = std::bit_width(std::min(width, height));
    const uint32_t levels = std::min<uint32_t>(std::max<uint32_t>(t.true_mip_count(), 1), max_levels);
    uint32_t layout_width = width, layout_height = height;
    if (!(t.mip_count == 0xF && type == SCE_GXM_TEXTURE_LINEAR)) {
        layout_width = std::bit_ceil(width);
        layout_height = std::bit_ceil(height);
    }
    uint32_t align_width = 1, align_height = 1;
    if (type == SCE_GXM_TEXTURE_LINEAR)
        align_width = 8;
    else if (type == SCE_GXM_TEXTURE_TILED)
        align_width = align_height = 32;
    // Source footprint of the whole chain (hashing and bounds).
    uint64_t footprint = 0;
    {
        uint32_t lw = layout_width, lh = layout_height, w = width, h = height;
        for (uint32_t level = 0; level < levels; ++level) {
            uint32_t stride = w, rows = h;
            if (type == SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY) { stride = std::bit_ceil(w); rows = std::bit_ceil(h); }
            if (type == SCE_GXM_TEXTURE_LINEAR_STRIDED) stride = gxm::get_stride_in_bytes(t) / bytes_per_pixel;
            stride = align(stride, align_width); rows = align(rows, align_height);
            const uint64_t level_end = footprint + uint64_t(stride) * rows * bytes_per_pixel;
            const uint64_t mip_size = uint64_t(align(lw, align_width)) * align(lh, align_height) * bytes_per_pixel;
            footprint = std::max(level_end, footprint + mip_size);
            lw = std::max(lw / 2, 1u); lh = std::max(lh / 2, 1u); w = std::max(w / 2, 1u); h = std::max(h / 2, 1u);
        }
    }
    const Address address = t.data_addr << 2;
    if (!address || footprint > (64u << 20) || !is_valid_addr_range(mem, address, uint64_t(address) + footprint)) {
        why = "texture memory range";
        return false;
    }
    const uint8_t *source = Ptr<const uint8_t>(address).get(mem);
    decoded.source = address;
    decoded.footprint = static_cast<uint32_t>(footprint);
    source_hash = XXH3_64bits(source, footprint);
    if (known_hash && source_hash == known_hash)
        return true;
    decoded.width = width;
    decoded.height = height;
    decoded.levels = levels;
    // Kept across calls: the draw path holds no locals with destructors (skip_draw).
    static std::vector<uint8_t> linear;
    uint64_t level_source = 0;
    uint32_t lw = layout_width, lh = layout_height, w = width, h = height;
    for (uint32_t level = 0; level < levels; ++level) {
        uint32_t stride = w, rows = h;
        if (type == SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY) { stride = std::bit_ceil(w); rows = std::bit_ceil(h); }
        if (type == SCE_GXM_TEXTURE_LINEAR_STRIDED) stride = gxm::get_stride_in_bytes(t) / bytes_per_pixel;
        stride = align(stride, align_width);
        rows = align(rows, align_height);
        const uint8_t *pixels = source + level_source;
        if (swizzled || type == SCE_GXM_TEXTURE_TILED) {
            linear.resize(size_t(stride) * rows * bytes_per_pixel);
            if (swizzled)
                renderer::texture::swizzled_texture_to_linear_texture(linear.data(), pixels, uint16_t(stride), uint16_t(rows), uint8_t(bpp));
            else
                renderer::texture::tiled_texture_to_linear_texture(linear.data(), pixels, uint16_t(stride), uint16_t(rows), uint8_t(bpp));
            pixels = linear.data();
        }
        uint32_t offset = 0;
        uint8_t *dest = out.reserve(size_t(w) * h * 4, 4, offset);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *row = pixels + size_t(y) * stride * bytes_per_pixel;
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t c[4] = {0, 0, 0, 255};
                decode_texel(base, row + size_t(x) * bytes_per_pixel, c);
                uint8_t *texel = dest + (size_t(y) * w + x) * 4;
                for (int i = 0; i < 4; ++i)
                    texel[i] = map[i] == Z ? 0 : map[i] == O ? 255 : c[map[i]];
            }
        }
        decoded.level_bytes[level] = {offset, w * h * 4};
        level_source += uint64_t(align(lw, align_width)) * align(lh, align_height) * bytes_per_pixel;
        lw = std::max(lw / 2, 1u); lh = std::max(lh / 2, 1u); w = std::max(w / 2, 1u); h = std::max(h / 2, 1u);
    }
    return true;
}

// Texture binding for one unit: id (bit 31 = render-target alias, whose low
// bits are the guest address / 4) plus sampler words, emitting an upload
// when the guest bytes changed since the last check.
struct BoundTexture {
    uint32_t id = 0;
    uint32_t min = 0, mag = 0, mip = 0, u = 0, v = 0, lod_max = 0;
};
// The texels of a texture that lie in a rendered target, when they are the
// target's texels: same memory layout and texel format, a whole rectangle
// inside the surface, and not written by the guest since it was rendered
// (memory reused for other data). `overlaps` reports a texture that starts
// inside a target without matching it: like desktop Vita3K it reads guest
// memory, which holds the rendered texels only with surface sync.
struct TargetTexels {
    Address base = 0;
    uint32_t x = 0, y = 0;
    bool whole = false;
};
static bool texels_in_target(const MemState &mem, const SceGxmTexture &t, TargetTexels &at, bool &overlaps) {
    overlaps = false;
    const Address address = t.data_addr << 2;
    const auto type = t.texture_type();
    const auto format = gxm::get_format(t);
    const uint32_t width = gxm::get_width(t), height = gxm::get_height(t);
    for (const auto &[base, target] : rendered_targets()) {
        const auto &g = target.geometry;
        if (address < base || address - base >= g.footprint())
            continue;
        overlaps = true;
        // Texel format: the GPU target holds the surface's components in
        // memory order, which a texture reads unswizzled only in the same
        // base format with the identity (ABGR) component order.
        SceGxmTextureBaseFormat expected;
        switch (target.format) {
        case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8: expected = SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8; break;
        case SCE_GXM_COLOR_BASE_FORMAT_U2U10U10U10: expected = SCE_GXM_TEXTURE_BASE_FORMAT_U2U10U10U10; break;
        case SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16: expected = SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16; break;
        default: return false;
        }
        if (gxm::get_base_format(format) != expected || (format & SCE_GXM_TEXTURE_SWIZZLE_MASK) != 0)
            return false;
        // Layout: the texture's texel (u, v) must be the surface's texel
        // (x + u, y + v) for every texel.
        const uint64_t offset = address - base;
        uint32_t x = 0, y = 0;
        if (!g.texel_at(offset, x, y))
            return false;
        switch (g.layout) {
        case Layout::Linear: {
            const uint32_t stride = type == SCE_GXM_TEXTURE_LINEAR_STRIDED ? gxm::get_stride_in_bytes(t)
                : type == SCE_GXM_TEXTURE_LINEAR ? align(width, 8) * g.pixel_bytes : 0;
            if (stride != g.stride_px * g.pixel_bytes)
                return false;
            break;
        }
        case Layout::Tiled:
            // Whole tile rows of the same width, starting at a row of tiles.
            if (type != SCE_GXM_TEXTURE_TILED || align(width, 32) != g.stride_px || x || y % 32)
                return false;
            break;
        case Layout::Swizzled:
            // The surface itself, or an aligned square block of its Morton order.
            if (type != SCE_GXM_TEXTURE_SWIZZLED
                || !(width == g.width && height == g.height)
                && !(width == height && width <= std::min(g.width, g.height) && offset % (uint64_t(width) * height * g.pixel_bytes) == 0))
                return false;
            break;
        }
        if (x + width > g.width || y + height > g.height)
            return false;
        // Bytes the texture covers: to its last row, tile row or Morton block.
        const uint64_t span = g.layout == Layout::Linear
            ? (uint64_t(height) - 1) * g.stride_px * g.pixel_bytes + uint64_t(width) * g.pixel_bytes
            : g.layout == Layout::Tiled ? uint64_t((height + 31) / 32) * g.stride_px * 32 * g.pixel_bytes
                                        : uint64_t(width) * height * g.pixel_bytes;
        if (mem_written_epoch(mem, address, span) >= target.rendered_epoch) {
            overlaps = false; // the guest's own bytes now
            return false;
        }
        at = {base, x, y, x == 0 && y == 0 && width == g.width && height == g.height};
        return true;
    }
    return false;
}
// Reported once per texture format and layout: sampled from guest memory,
// which does not hold the render target's texels without surface sync.
[[gnu::noinline]] static void note_stale_texture(const SceGxmTexture &t) {
    // Plain arrays: this is inlined into the command loop, which must hold
    // no container whose cleanup would route calls through invoke_* wrappers.
    static std::array<std::array<uint32_t, 4>, 32> noted;
    static size_t count = 0;
    const std::array<uint32_t, 4> key = {uint32_t(gxm::get_format(t)), uint32_t(t.texture_type()), gxm::get_width(t), gxm::get_height(t)};
    if (std::find(noted.begin(), noted.begin() + count, key) != noted.begin() + count || count == noted.size())
        return;
    noted[count++] = key;
    std::printf("[gxm-note] texture over a render target in another layout or texel format reads guest memory "
                "(texture format %#010x type %#010x %ux%u)\n", key[0], key[1], key[2], key[3]);
}
// A texture on a target's texels samples the target itself when it covers
// the whole surface (bit 31; bit 30 selects the pre-pass snapshot of the open
// pass's own target), else a copy of its rectangle taken before the open
// pass: what a tile-based GPU reads from memory during the scene.
static void bind_target_texels(const SceGxmTexture &t, const TargetTexels &at, scene::Writer &out, BoundTexture &bound) {
    bound.lod_max = 0;
    if (at.whole) {
        bound.id = 0x80000000u | (at.base >> 2);
        if (out.pass_open && at.base == out.pass_address) {
            out.words[out.pass_snapshot_word] = 1;
            bound.id |= 0x40000000u;
        }
        return;
    }
    const Address address = t.data_addr << 2;
    const uint32_t width = gxm::get_width(t), height = gxm::get_height(t);
    auto &cache = texture_cache();
    const uint32_t identity[4] = {address, width, height, 0x52474e52u /* region */};
    auto &entry = cache.entries[XXH3_64bits(identity, sizeof(identity))];
    if (!entry.id)
        entry.id = cache.next_id++;
    if (std::find(out.pass_regions.begin(), out.pass_regions.end(), entry.id) == out.pass_regions.end()) {
        out.pass_regions.push_back(entry.id);
        out.insert_before_pass({scene::Region, entry.id, at.base, at.x, at.y, width, height});
    }
    bound.id = entry.id;
}
static bool upload_texture(MemState &mem, const SceGxmTexture &t, scene::Writer &out, CachedTexture &entry, bool unwritten,
    const char *&why);
static bool bind_texture(MemState &mem, const SceGxmTexture &t, scene::Writer &out, BoundTexture &bound, const char *&why) {
    const Address address = t.data_addr << 2;
    // Linear-strided descriptors keep the pitch where the min/mip filters
    // live: min follows mag and there are no mips (sceGxmTextureGetMinFilter).
    const bool strided = t.texture_type() == SCE_GXM_TEXTURE_LINEAR_STRIDED;
    bound.mag = t.mag_filter == SCE_GXM_TEXTURE_FILTER_LINEAR ? 1 : 0;
    bound.min = strided ? bound.mag : t.min_filter == SCE_GXM_TEXTURE_FILTER_LINEAR ? 1 : 0;
    bound.mip = !strided && t.mip_filter ? 1 : 0;
    bound.u = t.uaddr_mode;
    bound.v = t.vaddr_mode;
    bound.lod_max = std::max<uint32_t>(t.true_mip_count(), 1) - 1;
    TargetTexels at;
    bool overlaps = false;
    if (texels_in_target(mem, t, at, overlaps)) {
        bind_target_texels(t, at, out, bound);
        return true;
    }
    if (overlaps && !surface_sync())
        note_stale_texture(t);
    auto &cache = texture_cache();
    // Identity of the guest image: address, format, size, layout and mips
    // (sampler fields do not change the uploaded texels).
    const uint32_t identity[7] = {address, uint32_t(gxm::get_format(t)), gxm::get_width(t), gxm::get_height(t),
        uint32_t(t.texture_type()), t.true_mip_count(), strided ? uint32_t(gxm::get_stride_in_bytes(t)) : 0u};
    const uint64_t key = XXH3_64bits(identity, sizeof(identity));
    auto &entry = cache.entries[key];
    if (!entry.id)
        entry.id = cache.next_id++;
    bound.id = entry.id;
    const bool unwritten = entry.checked_epoch
        && mem_written_epoch(mem, entry.source, entry.footprint) < entry.checked_epoch;
    if (unwritten && !texture_verify()) {
        ++timing().clean;
        return true;
    }
    return upload_texture(mem, t, out, entry, unwritten, why);
}
// Written (or never seen): one hash of its guest bytes, decoded only if changed.
[[gnu::noinline]] static bool upload_texture(MemState &mem, const SceGxmTexture &t, scene::Writer &out, CachedTexture &entry,
    bool unwritten, const char *&why) {
    auto &cache = texture_cache();
    static scene::Writer scratch; // reused: no locals with destructors (skip_draw)
    scratch.data.clear();
    DecodedTexture decoded;
    uint64_t hash = 0;
    const double decode_started = emscripten_get_now();
    const bool decoded_ok = decode_texture(mem, t, scratch, decoded, entry.hash, hash, why);
    timing().decode += emscripten_get_now() - decode_started;
    if (!decoded_ok)
        return false;
    ++timing().hashes;
    entry.checked_epoch = cache.epoch;
    entry.source = decoded.source;
    entry.footprint = decoded.footprint;
    if (!decoded.levels)
        return true;
    if (unwritten) {
        // VITA3K_TEXTURE_VERIFY: the bytes changed without a tracked write.
        if (++timing().untracked <= 20)
            std::printf("[gxm-verify] untracked write to texture %08x (+%x bytes)\n", entry.source, entry.footprint);
    }
    ++timing().decodes;
    entry.hash = hash;
    out.word(scene::Texture);
    out.word(entry.id);
    out.word(decoded.width);
    out.word(decoded.height);
    out.word(decoded.levels);
    for (uint32_t level = 0; level < decoded.levels; ++level) {
        const auto [offset, size] = decoded.level_bytes[level];
        out.word(out.bytes(scratch.data.data() + offset, size, 4));
        out.word(size);
    }
    return true;
}

// --- State -----------------------------------------------------------------
static void consume_state(WebContext &ctx, CommandHelper &h, MemState &mem) {
    switch (h.pop<GXMState>()) {
    case GXMState::RegionClip: {
        // Snapped to the tile grid in render pixels at the draw (the scene's
        // sample factor comes with SetContext).
        ctx.record.region_clip_mode = h.pop<SceGxmRegionClipMode>();
        for (auto &bound : ctx.region_clip)
            bound = h.pop<uint32_t>();
        break;
    }
    case GXMState::Viewport:
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
            ctx.record.is_maskupdate = ctx.record.fragment_program.get(mem)->is_maskupdate;
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
        const renderer::ShaderProgram *program = nullptr;
        if (vertex && ctx.record.vertex_program)
            program = ctx.record.vertex_program.get(mem)->renderer_data.get();
        if (!vertex && ctx.record.fragment_program)
            program = ctx.record.fragment_program.get(mem)->renderer_data.get();
        if (!program || block < 0 || size_t(block) >= program->uniform_buffer_sizes.size())
            break;
        const auto offset = program->uniform_buffer_data_offsets[block];
        if (offset == UINT32_MAX) break;
        const size_t total = program->max_total_uniform_buffer_storage * 4;
        const size_t copied = std::min(size_t(size), size_t(program->uniform_buffer_sizes[block]) * 4);
        if (total > 16 * 1024 * 1024 || uint64_t(offset) * 4 + copied > total)
            unsupported("uniform packing overflow");
        require_guest(mem, ptr.address(), copied);
        auto &bytes = ctx.uniforms[vertex ? 0 : 1];
        bytes.resize(total);
        std::memcpy(bytes.data() + size_t(offset) * 4, ptr.get(mem), copied);
        break;
    }
    case GXMState::Texture: {
        // renderer::set_texture: fragment units 0..15, vertex units from 16.
        const auto index = h.pop<uint32_t>();
        const auto texture = h.pop<SceGxmTexture>();
        auto &units = index < 16 ? ctx.fragment_textures : ctx.vertex_textures;
        units[index & 15] = { true, texture };
        break;
    }
    case GXMState::VertexStream: {
        const auto ptr = h.pop<Ptr<const uint8_t>>();
        const auto index = h.pop<size_t>(), size = h.pop<size_t>();
        if (index < ctx.record.vertex_streams.size())
            ctx.record.vertex_streams[index] = {ptr, size};
        break;
    }
    case GXMState::CullMode:
        ctx.record.cull_mode = h.pop<SceGxmCullMode>();
        break;
    case GXMState::TwoSided:
        ctx.record.two_sided = h.pop<SceGxmTwoSidedMode>();
        break;
    case GXMState::PolygonMode: {
        const bool front = h.pop<bool>();
        const auto mode = h.pop<SceGxmPolygonMode>();
        (front ? ctx.record.front_polygon_mode : ctx.record.back_polygon_mode) = mode;
        break;
    }
    case GXMState::DepthFunc: {
        const bool front = h.pop<bool>();
        const auto func = h.pop<SceGxmDepthFunc>();
        (front ? ctx.record.front_depth_func : ctx.record.back_depth_func) = func;
        break;
    }
    case GXMState::DepthWriteEnable: {
        const bool front = h.pop<bool>();
        const auto mode = h.pop<SceGxmDepthWriteMode>();
        (front ? ctx.record.front_depth_write_mode : ctx.record.back_depth_write_mode) = mode;
        break;
    }
    case GXMState::DepthBias: {
        h.pop<bool>();
        ctx.record.depth_bias_slope = h.pop<int>();
        ctx.record.depth_bias_unit = h.pop<int>();
        break;
    }
    case GXMState::PointLineWidth: {
        const bool front = h.pop<bool>();
        const auto width = h.pop<uint32_t>();
        if (front) ctx.record.line_width = width;
        break;
    }
    case GXMState::StencilFunc: {
        const bool front = h.pop<bool>();
        auto &op = front ? ctx.record.front_stencil_state_op : ctx.record.back_stencil_state_op;
        auto &values = front ? ctx.record.front_stencil_state_values : ctx.record.back_stencil_state_values;
        op.func = h.pop<SceGxmStencilFunc>();
        op.stencil_fail = h.pop<SceGxmStencilOp>();
        op.depth_fail = h.pop<SceGxmStencilOp>();
        op.depth_pass = h.pop<SceGxmStencilOp>();
        values.compare_mask = h.pop<uint8_t>();
        values.write_mask = h.pop<uint8_t>();
        if (ctx.record.is_maskupdate)
            ctx.record.writing_mask = op.func == SCE_GXM_STENCIL_FUNC_NEVER ? 0.0f : 1.0f;
        break;
    }
    case GXMState::StencilRef: {
        const bool front = h.pop<bool>();
        const uint8_t ref = h.pop<unsigned char>();
        (front ? ctx.record.front_stencil_state_values : ctx.record.back_stencil_state_values).ref = ref;
        break;
    }
    case GXMState::FragmentProgramEnable: {
        const bool front = h.pop<bool>();
        const auto mode = h.pop<SceGxmFragmentProgramMode>();
        (front ? ctx.record.front_side_fragment_program_mode : ctx.record.back_side_fragment_program_mode) = mode;
        break;
    }
    case GXMState::VisibilityBuffer:
        h.pop<Ptr<uint32_t>>(); h.pop<uint32_t>();
        break;
    case GXMState::VisibilityIndex:
        h.pop<uint32_t>(); h.pop<bool>(); h.pop<bool>();
        break;
    default:
        unsupported("render state not implemented");
    }
}

// --- Passes ----------------------------------------------------------------
static uint32_t webgpu_color_format(SceGxmColorFormat format) {
    return static_cast<uint32_t>(format);
}
static void end_pass(scene::Writer &out) {
    if (out.pass_open) {
        out.word(scene::EndPass);
        out.pass_open = false;
    }
}
static void begin_pass(WebContext &ctx, scene::Writer &out) {
    end_pass(out);
    const auto &color = ctx.record.color_surface;
    out.pass_begin_word = out.words.size();
    out.pass_regions.clear();
    out.word(scene::BeginPass);
    out.word(color.data.address());
    out.word(webgpu_color_format(color.colorFormat));
    out.word(color.width);
    out.word(color.height);
    // Depth: a guest surface (2) keeps its contents on the GPU across scenes;
    // without one the tile's on-chip buffer (1) is cleared per scene.
    const auto &depth = ctx.depth;
    out.word(ctx.has_depth_surface ? 2 : 1);
    out.word(ctx.has_depth_surface ? static_cast<uint32_t>(depth.get_format()) : 0);
    out.word(ctx.has_depth_surface && depth.force_load ? 1 : 0);
    out.word(ctx.has_depth_surface && depth.force_store ? 1 : 0);
    out.real(ctx.has_depth_surface ? depth.background_depth : 1.0f);
    out.word(ctx.has_depth_surface ? depth.stencil : 0);
    // A guest depth/stencil surface keeps its GPU copy by its own addresses.
    out.word(ctx.has_depth_surface ? depth.depth_data.address() : 0);
    out.word(ctx.has_depth_surface ? depth.stencil_data.address() : 0);
    // Snapshot flag, set by bind_texture when a draw samples this pass's own
    // target: the tile-based GPU reads what memory held before the scene.
    out.pass_snapshot_word = out.words.size();
    out.word(0);
    // GPU texels per surface texel, and per render pixel (viewport, scissor).
    out.word(ctx.internal_scale * ctx.downscale);
    out.word(ctx.internal_scale);
    out.pass_address = color.data.address();
    out.pass_first_draw = out.draws;
    out.pass_open = true;
    rendered_targets()[color.data.address()] = { ctx.geometry,
        static_cast<SceGxmColorBaseFormat>(color.colorFormat & SCE_GXM_COLOR_BASE_FORMAT_MASK), texture_cache().epoch };
}

// --- Draws -----------------------------------------------------------------
static uint32_t topology_index(SceGxmPrimitiveType primitive) {
    switch (primitive) {
    case SCE_GXM_PRIMITIVE_TRIANGLES: return 0;
    case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP: return 1;
    case SCE_GXM_PRIMITIVE_LINES: return 2;
    case SCE_GXM_PRIMITIVE_POINTS: return 4;
    default: return 0; // fans are expanded to lists by the caller
    }
}

[[gnu::noinline]] static void skip_attribute(uint32_t format, uint32_t components) {
    char detail[32];
    std::snprintf(detail, sizeof(detail), "%ux%u", format, components);
    skip_draw("vertex attribute format ", detail);
}
static void consume_draw(WebContext &ctx, CommandHelper &h, MemState &mem, scene::Writer &out) {
    const auto primitive = h.pop<SceGxmPrimitiveType>();
    const auto format = h.pop<SceGxmIndexFormat>();
    const auto indices = h.pop<Ptr<const uint8_t>>();
    const auto count = h.pop<uint32_t>(), instances = h.pop<uint32_t>();
    if (!out.pass_open || !ctx.has_surface || !ctx.record.vertex_program || !ctx.record.fragment_program)
        return skip_draw("draw without surface or programs");
    if (instances != 1)
        return skip_draw("instanced draw");
    if (primitive == SCE_GXM_PRIMITIVE_TRIANGLE_EDGES)
        return skip_draw("triangle-edges primitive");
    if (!count || (format != SCE_GXM_INDEX_FORMAT_U16 && format != SCE_GXM_INDEX_FORMAT_U32))
        return skip_draw("index format");
    const auto *vp = ctx.record.vertex_program.get(mem);
    const auto *fp = ctx.record.fragment_program.get(mem);
    if (!vp->renderer_data || !fp->renderer_data)
        return skip_draw("program without renderer data");
    const int vs = program_id(mem, *vp->renderer_data, vp->program, false);
    const int fs = program_id(mem, *fp->renderer_data, fp->program, true);
    if (vs < 0 || fs < 0)
        return skip_draw("untranslated program");
    for (int i = 0; i < 2; ++i) {
        const auto &program = i == 0 ? static_cast<const renderer::ShaderProgram &>(*vp->renderer_data) : *fp->renderer_data;
        if (ctx.uniforms[i].size() != program.max_total_uniform_buffer_storage * 4)
            ctx.uniforms[i].resize(program.max_total_uniform_buffer_storage * 4);
    }

    // Textures first: their upload commands precede the draw that samples them.
    // Fixed arrays: the draw path keeps no locals with destructors (see skip_draw).
    struct UnitBinding { uint32_t unit; BoundTexture bound; };
    std::array<UnitBinding, 32> units;
    uint32_t unit_count = 0;
    for (int stage = 0; stage < 2; ++stage) {
        const auto used = stage == 0 ? fp->renderer_data->textures_used : vp->renderer_data->textures_used;
        auto &bound_units = stage == 0 ? ctx.fragment_textures : ctx.vertex_textures;
        for (uint32_t unit = 0; unit < 16; ++unit) {
            if (!used[unit])
                continue;
            if (!bound_units[unit].bound)
                return skip_draw("sampled texture unit without a texture");
            BoundTexture bound;
            const char *why = "";
            if (!bind_texture(mem, bound_units[unit].texture, out, bound, why))
                return skip_draw("texture: ", why);
            units[unit_count++] = {unit | (stage == 1 ? 16u : 0u), bound};
        }
    }

    // Vertex streams and attributes.
    const size_t stream_count = vp->streams.size();
    std::array<Address, SCE_GXM_MAX_VERTEX_STREAMS> stream_base{};
    std::array<uint32_t, SCE_GXM_MAX_VERTEX_STREAMS> stream_size{};
    for (size_t i = 0; i < stream_count; ++i) {
        const auto &stream = ctx.record.vertex_streams[i];
        if (gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(vp->streams[i].indexSource)))
            return skip_draw("instanced vertex stream");
        if (!stream.data || !stream.size || stream.size > (16u << 20))
            return skip_draw("vertex stream range");
        require_guest(mem, stream.data.address(), stream.size);
        stream_base[i] = stream.data.address();
        stream_size[i] = static_cast<uint32_t>(stream.size);
    }
    struct Attribute { uint32_t location, stream, offset, format, components; };
    std::array<Attribute, 16> attributes; // SCE_GXM_MAX_VERTEX_ATTRIBUTES
    uint32_t attribute_count = 0;
    for (const auto &a : vp->attributes) {
        const auto info = vp->renderer_data->attribute_infos.find(a.regIndex);
        if (info == vp->renderer_data->attribute_infos.end())
            continue; // stripped symbol: the shader does not read it
        const bool small = a.format <= SCE_GXM_ATTRIBUTE_FORMAT_F16 && a.componentCount != 2 && a.componentCount != 4;
        if (small || a.format > SCE_GXM_ATTRIBUTE_FORMAT_F32 || a.streamIndex >= stream_count)
            return skip_attribute(a.format, a.componentCount);
        if (attribute_count == attributes.size())
            unsupported("more vertex attributes than GXM allows");
        attributes[attribute_count++] = {info->second.location, a.streamIndex, a.offset, uint32_t(a.format), a.componentCount};
    }

    // Indices; fans become lists with the same provoking vertex.
    const size_t index_size = format == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4;
    require_guest(mem, indices.address(), size_t(count) * index_size);
    uint32_t index_offset = 0, index_count = count;
    if (primitive == SCE_GXM_PRIMITIVE_TRIANGLE_FAN) {
        if (count < 3)
            return;
        index_count = (count - 2) * 3;
        uint8_t *dest = out.reserve(size_t(index_count) * index_size, 4, index_offset);
        const uint8_t *source = indices.get(mem);
        for (uint32_t k = 1; k + 1 < count; ++k) {
            std::memcpy(dest, source, index_size); dest += index_size;
            std::memcpy(dest, source + k * index_size, index_size); dest += index_size;
            std::memcpy(dest, source + (k + 1) * index_size, index_size); dest += index_size;
        }
    } else {
        index_offset = out.bytes(indices.get(mem), size_t(count) * index_size, 4);
    }

    // Render info blocks (see renderer/src/vulkan/scene.cpp). Render pixels:
    // the surface times its downscale; the shaders see guest coordinates,
    // render pixels / samples (screen size, gl_FragCoord / res_multiplier).
    const int32_t render_width = int32_t(ctx.geometry.width * ctx.downscale);
    const int32_t render_height = int32_t(ctx.geometry.height * ctx.downscale);
    const float samples = float(ctx.samples);
    const float vs_info[12] = {ctx.record.viewport_flip[0], ctx.record.viewport_flip[1],
        ctx.record.viewport_flip[2], ctx.record.viewport_flip[3], ctx.record.viewport_flat ? 0.0f : 1.0f,
        render_width / samples, render_height / samples, ctx.record.z_offset, ctx.record.z_scale, 0, 0, 0};
    const float fs_info[8] = {
        ctx.record.back_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED ? 1.0f : 0.0f,
        ctx.record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED ? 1.0f : 0.0f,
        ctx.record.writing_mask, 0.0f, float(ctx.internal_scale * ctx.samples), 0, 0, 0}; // [4] res_multiplier
    const uint32_t vs_info_offset = out.bytes(vs_info, sizeof(vs_info), scene::kUniformAlign);
    const uint32_t fs_info_offset = out.bytes(fs_info, sizeof(fs_info), scene::kUniformAlign);
    const uint32_t vs_uniforms = out.bytes(ctx.uniforms[0].data(), ctx.uniforms[0].size(), scene::kUniformAlign);
    const uint32_t fs_uniforms = out.bytes(ctx.uniforms[1].data(), ctx.uniforms[1].size(), scene::kUniformAlign);

    // Viewport rect (vulkan sync_viewport_real, positive height) and scissor,
    // in render pixels.
    float vx = 0, vy = 0, vw = float(render_width), vh = float(render_height);
    if (!ctx.record.viewport_flat && ctx.has_viewport) {
        vw = std::abs(2 * ctx.viewport[3]) * samples;
        vh = 2 * ctx.viewport[4] * samples;
        vy = (ctx.viewport[1] - ctx.viewport[4]) * samples;
        vx = (ctx.viewport[0] - std::abs(ctx.viewport[3])) * samples;
        if (vh < 0) { vy += vh; vh = -vh; }
    }
    int32_t sx = 0, sy = 0, sw = render_width, sh = render_height;
    switch (ctx.record.region_clip_mode) {
    case SCE_GXM_REGION_CLIP_ALL: sw = sh = 0; break;
    case SCE_GXM_REGION_CLIP_OUTSIDE: {
        // state_set.cpp region_clip: render-pixel bounds snap to the tile grid.
        const auto &clip = ctx.region_clip;
        sx = int32_t(align_down(clip[0] * ctx.samples, SCE_GXM_TILE_SIZEX));
        sy = int32_t(align_down(clip[2] * ctx.samples, SCE_GXM_TILE_SIZEY));
        sw = std::max(int32_t(align(clip[1] * ctx.samples, SCE_GXM_TILE_SIZEX)) - sx, 0);
        sh = std::max(int32_t(align(clip[3] * ctx.samples, SCE_GXM_TILE_SIZEY)) - sy, 0);
        break;
    }
    default: break; // NONE, and INSIDE (unimplemented upstream as well)
    }
    sx = std::clamp(sx, 0, render_width); sy = std::clamp(sy, 0, render_height);
    sw = std::clamp(sw, 0, render_width - sx); sh = std::clamp(sh, 0, render_height - sy);

    const auto *webgpu_fp = static_cast<const browser::WebGPUFragmentProgram *>(fp->renderer_data.get());
    const auto &blend = webgpu_fp->blend;
    const bool two_sided = ctx.record.two_sided == SCE_GXM_TWO_SIDED_ENABLED;
    const auto &front_op = ctx.record.front_stencil_state_op;
    const auto &back_op = two_sided ? ctx.record.back_stencil_state_op : front_op;
    const auto &front_values = ctx.record.front_stencil_state_values;
    const bool fragment_disabled = ctx.record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED
        || fp->program.get(mem)->has_no_effect();

    out.word(scene::Draw);
    out.word(uint32_t(vs));
    out.word(uint32_t(fs));
    out.word(ctx.record.cull_mode == SCE_GXM_CULL_CW ? 1 : ctx.record.cull_mode == SCE_GXM_CULL_CCW ? 2 : 0);
    out.word(topology_index(primitive));
    for (const uint32_t value : {blend.color_mask, blend.color_func, blend.alpha_func, blend.color_src,
             blend.color_dst, blend.alpha_src, blend.alpha_dst})
        out.word(value);
    out.word(fragment_disabled ? 1 : 0);
    out.word(uint32_t(ctx.record.front_depth_func) >> 22);
    out.word(ctx.record.front_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED ? 1 : 0);
    for (const auto *op : {&front_op, &back_op}) {
        out.word(uint32_t(op->func) >> 25);
        out.word(uint32_t(op->stencil_fail));
        out.word(uint32_t(op->depth_fail));
        out.word(uint32_t(op->depth_pass));
    }
    out.word(front_values.compare_mask);
    out.word(front_values.write_mask);
    out.word(front_values.ref);
    out.word(uint32_t(stream_count));
    for (size_t i = 0; i < stream_count; ++i) {
        out.word(vp->streams[i].stride);
        out.stream(stream_base[i], stream_size[i]);
        out.word(stream_size[i]);
    }
    out.word(attribute_count);
    for (uint32_t i = 0; i < attribute_count; ++i) {
        const auto &a = attributes[i];
        for (const uint32_t value : {a.location, a.stream, a.offset, a.format, a.components})
            out.word(value);
    }
    out.word(uint32_t(index_size));
    out.word(index_count);
    out.word(index_offset);
    out.real(vx); out.real(vy); out.real(vw); out.real(vh);
    out.word(uint32_t(sx)); out.word(uint32_t(sy)); out.word(uint32_t(sw)); out.word(uint32_t(sh));
    out.word(vs_info_offset);
    out.word(fs_info_offset);
    out.word(vs_uniforms);
    out.word(uint32_t(ctx.uniforms[0].size()));
    out.word(fs_uniforms);
    out.word(uint32_t(ctx.uniforms[1].size()));
    out.word(unit_count);
    for (uint32_t i = 0; i < unit_count; ++i) {
        const auto &[unit, bound] = units[i];
        for (const uint32_t value : {unit, bound.id, bound.min, bound.mag, bound.mip, bound.u, bound.v, bound.lod_max})
            out.word(value);
    }
    ++out.draws;
}

// False when gxm_scene.js rejected the stream (its error is logged).
static bool submit_scene(scene::Writer &out, MemState &mem) {
    end_pass(out);
    out.copy_streams(mem);
    int result = 0;
    if (out.words.size() > 1) {
        const double started = emscripten_get_now();
        result = web_gxm_submit(out.words.data(), uint32_t(out.words.size()), out.data.data(), uint32_t(out.data.size()));
        timing().submit += emscripten_get_now() - started;
    }
    out.reset();
    return result == 0;
}

// Surface sync (VITA3K_SURFACE_SYNC=1) of one color surface a submitted scene
// drew into: its GPU target is copied into the guest texels the surface
// describes, in its layout, and the write is tracked so textures cached from
// those bytes are checked again.
struct SurfaceReadback {
    Address address = 0;
    SurfaceGeometry geometry;
};
static void sync_surface(MemState &mem, const SurfaceReadback &r) {
    const auto &g = r.geometry;
    const uint64_t footprint = g.footprint();
    require_guest(mem, r.address, footprint);
    const double started = emscripten_get_now();
    static std::vector<uint8_t> rows; // kept: no locals with destructors on the command path
    const size_t row = size_t(g.width) * g.pixel_bytes;
    rows.resize(row * g.height);
    if (web_gxm_sync_surface(r.address, rows.data(), g.width, g.height, g.pixel_bytes) != 0)
        unsupported("surface sync failed (see browser log)");
    uint8_t *guest = Ptr<uint8_t>(r.address).get(mem);
    for (uint32_t y = 0; y < g.height; ++y) {
        const uint8_t *source = rows.data() + y * row;
        if (g.layout == Layout::Linear) {
            std::memcpy(guest + g.byte_offset(0, y), source, row);
            continue;
        }
        for (uint32_t x = 0; x < g.width; ++x)
            std::memcpy(guest + g.byte_offset(x, y), source + size_t(x) * g.pixel_bytes, g.pixel_bytes);
    }
    mem_mark_written(mem, r.address, footprint);
    // The GPU target and these bytes now agree (texels_in_target).
    rendered_targets()[r.address].rendered_epoch = advance_write_epoch(mem);
    timing().sync += emscripten_get_now() - started;
    ++timing().syncs;
}

// --- Transfers ----------------------------------------------------------------
// Transfers run on the CPU over guest memory, as renderer/src/transfer.cpp
// does. A source inside a rendered target is first read back (its GPU texels
// are the truth unless surface sync already keeps memory current), and every
// rendered target whose texels a transfer wrote gets them from memory again,
// in stream order with the scenes around it.
struct TransferImage {
    Address address = 0;
    SurfaceGeometry geometry; // texel addressing of the image
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint64_t begin = 0, end = 0; // byte range of the rectangle, relative to address
};
// Addressing of a transfer image (transfer.cpp compute_offset: tiled rows of
// stride / 32 tiles, swizzled Morton order over the transfer size), or false
// for an image the guest cannot have meant.
static bool transfer_image(MemState &mem, const SceGxmTransferImage &image, SceGxmTransferType type,
    uint32_t width, uint32_t height, TransferImage &out) {
    const uint32_t bits = gxm::get_bits_per_pixel(image.format);
    if (!bits || bits % 8 || !width || !height || width > 4096 || height > 4096 || image.x > 4096 || image.y > 4096)
        return false;
    auto &g = out.geometry;
    g.pixel_bytes = bits / 8;
    switch (type) {
    case SCE_GXM_TRANSFER_LINEAR: g.layout = Layout::Linear; break;
    case SCE_GXM_TRANSFER_TILED: g.layout = Layout::Tiled; break;
    case SCE_GXM_TRANSFER_SWIZZLED: g.layout = Layout::Swizzled; break;
    default: return false;
    }
    if (g.layout == Layout::Swizzled) {
        if (!std::has_single_bit(width) || !std::has_single_bit(height) || image.x || image.y)
            return false;
        g.width = width;
        g.height = height;
        g.stride_px = width;
    } else {
        if (image.stride <= 0 || image.stride % g.pixel_bytes)
            return false;
        g.stride_px = uint32_t(image.stride) / g.pixel_bytes;
        g.width = image.x + width;
        g.height = image.y + height;
        if (g.width > g.stride_px || (g.layout == Layout::Tiled && g.stride_px % 32))
            return false;
    }
    out.address = image.address.address();
    out.x = image.x;
    out.y = image.y;
    out.width = width;
    out.height = height;
    out.begin = g.byte_offset(image.x, image.y);
    out.end = g.footprint();
    if (!out.address || uint64_t(out.address) + out.end > (uint64_t(1) << 32)
        || !is_valid_addr_range(mem, out.address, uint64_t(out.address) + out.end))
        return false;
    return true;
}
static bool overlaps(Address a, uint64_t a_size, Address b, uint64_t b_size) {
    return uint64_t(a) < uint64_t(b) + b_size && uint64_t(b) < uint64_t(a) + a_size;
}
// Before a transfer reads guest bytes: pending draws read memory from before
// it, and rendered targets under the source are copied back into memory.
static void prepare_transfer(MemState &mem, scene::Writer &out, const TransferImage *source) {
    if (out.pass_open)
        unsupported("transfer inside a scene");
    out.copy_streams(mem);
    if (!source || surface_sync())
        return;
    for (const auto &[base, target] : rendered_targets()) {
        if (!overlaps(base, target.geometry.footprint(), source->address, source->end))
            continue;
        if (!submit_scene(out, mem)) // the scenes rendering it, before the read
            unsupported("scene submission failed (see browser log)");
        sync_surface(mem, {base, target.geometry});
    }
}
// After a transfer wrote `dest`'s rectangle (`written`: which of its texels,
// row by row, when not all): each rendered target over those bytes gets the
// written texels from memory (WRITE_TEXELS: the rectangle around them in the
// target's texel bytes and, unless all of it was written, a mask).
static void refresh_targets(MemState &mem, scene::Writer &out, const TransferImage &dest,
    const uint8_t *written_texels = nullptr) {
    mem_mark_written(mem, dest.address, dest.end);
    const auto &d = dest.geometry;
    for (auto &[base, target] : rendered_targets()) {
        const auto &g = target.geometry;
        if (!overlaps(base, g.footprint(), dest.address + dest.begin, dest.end - dest.begin))
            continue;
        // Texels of the target whose first byte the transfer wrote.
        static std::vector<uint8_t> written; // kept: no locals with destructors on the command path
        uint32_t x0 = g.width, y0 = g.height, x1 = 0, y1 = 0, count = 0;
        const uint8_t *guest = Ptr<const uint8_t>(base).get(mem);
        uint32_t tx, ty;
        if (!written_texels && g.layout == Layout::Linear && d.layout == Layout::Linear && d.pixel_bytes == g.pixel_bytes
            && d.stride_px == g.stride_px && dest.address + dest.begin >= base
            && g.texel_at(dest.address + dest.begin - base, tx, ty)) {
            // Same rows (a clear of a linear target): the rectangle itself.
            x0 = tx; y0 = ty;
            x1 = std::min(tx + dest.width, g.width) - 1;
            y1 = std::min(ty + dest.height, g.height) - 1;
            count = (x1 - x0 + 1) * (y1 - y0 + 1);
        } else {
            written.assign(size_t(g.width) * g.height, 0);
            for (uint32_t y = 0; y < g.height; ++y) {
                for (uint32_t x = 0; x < g.width; ++x) {
                    const uint64_t address = uint64_t(base) + g.byte_offset(x, y);
                    if (address < dest.address)
                        continue;
                    uint32_t dx, dy;
                    if (!d.texel_at(address - dest.address, dx, dy) || dx < dest.x || dy < dest.y
                        || dx >= dest.x + dest.width || dy >= dest.y + dest.height
                        || (written_texels && !written_texels[size_t(dy - dest.y) * dest.width + (dx - dest.x)]))
                        continue;
                    written[size_t(y) * g.width + x] = 1;
                    x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                    ++count;
                }
            }
        }
        if (!count)
            continue;
        const uint32_t w = x1 - x0 + 1, h = y1 - y0 + 1;
        uint32_t data_offset = 0, mask_offset = 0xffffffffu;
        uint8_t *texels = out.reserve(size_t(w) * h * g.pixel_bytes, 4, data_offset);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                std::memcpy(texels + (size_t(y) * w + x) * g.pixel_bytes, guest + g.byte_offset(x0 + x, y0 + y), g.pixel_bytes);
        if (count != w * h) {
            uint8_t *mask = out.reserve(size_t(w) * h, 4, mask_offset);
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(mask + size_t(y) * w, written.data() + size_t(y0 + y) * g.width + x0, w);
            for (size_t i = 0; i < size_t(w) * h; ++i)
                mask[i] = mask[i] ? 0xff : 0;
        }
        for (const uint32_t value : {uint32_t(scene::WriteTexels), base, x0, y0, w, h, data_offset, mask_offset})
            out.word(value);
        target.rendered_epoch = advance_write_epoch(mem);
    }
}

// renderer/src/transfer.cpp handle_transfer_fill: the fill color's low bytes
// in every texel of the linear rectangle.
static int transfer_fill(MemState &mem, uint32_t color, const SceGxmTransferImage &image, scene::Writer &out) {
    TransferImage dest;
    if (!transfer_image(mem, image, SCE_GXM_TRANSFER_LINEAR, image.width, image.height, dest) || dest.geometry.pixel_bytes > 4)
        return -1;
    prepare_transfer(mem, out, nullptr);
    uint8_t *base = Ptr<uint8_t>(dest.address).get(mem);
    for (uint32_t y = dest.y; y < dest.y + dest.height; ++y)
        for (uint32_t x = dest.x; x < dest.x + dest.width; ++x)
            std::memcpy(base + dest.geometry.byte_offset(x, y), &color, dest.geometry.pixel_bytes);
    refresh_targets(mem, out, dest);
    return 0;
}

// renderer/src/transfer.cpp perform_transfer_copy_impl, with its color keys.
static int transfer_copy(MemState &mem, uint32_t key_value, uint32_t key_mask, SceGxmTransferColorKeyMode mode,
    const SceGxmTransferImage *images, SceGxmTransferType source_type, SceGxmTransferType dest_type, scene::Writer &out) {
    TransferImage source, dest;
    if (images[0].format != images[1].format
        || !transfer_image(mem, images[0], source_type, images[0].width, images[0].height, source)
        || !transfer_image(mem, images[1], dest_type, images[0].width, images[0].height, dest))
        return -1;
    const uint32_t bytes = source.geometry.pixel_bytes;
    if (mode != SCE_GXM_TRANSFER_COLORKEY_NONE && bytes != 4)
        return -1; // desktop keys only 32-bit texels
    prepare_transfer(mem, out, &source);
    const uint8_t *from = Ptr<const uint8_t>(source.address).get(mem);
    uint8_t *to = Ptr<uint8_t>(dest.address).get(mem);
    // Through a copy: the rectangles may overlap in guest memory.
    static std::vector<uint8_t> texels, copied;
    texels.resize(size_t(source.width) * source.height * bytes);
    copied.assign(mode == SCE_GXM_TRANSFER_COLORKEY_NONE ? 0 : size_t(source.width) * source.height, 0);
    for (uint32_t y = 0; y < source.height; ++y)
        for (uint32_t x = 0; x < source.width; ++x)
            std::memcpy(texels.data() + (size_t(y) * source.width + x) * bytes,
                from + source.geometry.byte_offset(source.x + x, source.y + y), bytes);
    for (uint32_t y = 0; y < source.height; ++y) {
        for (uint32_t x = 0; x < source.width; ++x) {
            const uint8_t *texel = texels.data() + (size_t(y) * source.width + x) * bytes;
            if (mode != SCE_GXM_TRANSFER_COLORKEY_NONE) {
                uint32_t value;
                std::memcpy(&value, texel, 4);
                const bool keyed = (value & key_mask) == key_value;
                if (mode == SCE_GXM_TRANSFER_COLORKEY_PASS ? !keyed : keyed)
                    continue;
                copied[size_t(y) * source.width + x] = 1;
            }
            std::memcpy(to + dest.geometry.byte_offset(dest.x + x, dest.y + y), texel, bytes);
        }
    }
    // Keyed-out texels keep what the target holds, which memory may not.
    refresh_targets(mem, out, dest, copied.empty() ? nullptr : copied.data());
    return 0;
}

// renderer/src/transfer.cpp handle_transfer_downscale: each destination texel
// is the average of a 2x2 source block (its SWS_AREA filter), per 8-bit
// channel for U8U8U8U8/U8U8U8 and per 5/6-bit field for U5U6U5.
static int transfer_downscale(MemState &mem, const SceGxmTransferImage &source_image,
    const SceGxmTransferImage &dest_image, scene::Writer &out) {
    TransferImage source, dest;
    const auto format = source_image.format;
    if (format != dest_image.format || source_image.width < 2 || source_image.height < 2
        || (format != SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR && format != SCE_GXM_TRANSFER_FORMAT_U8U8U8_BGR
            && format != SCE_GXM_TRANSFER_FORMAT_U5U6U5_BGR)
        || !transfer_image(mem, source_image, SCE_GXM_TRANSFER_LINEAR, source_image.width / 2 * 2, source_image.height / 2 * 2, source)
        || !transfer_image(mem, dest_image, SCE_GXM_TRANSFER_LINEAR, source_image.width / 2, source_image.height / 2, dest))
        return -1;
    prepare_transfer(mem, out, &source);
    const uint32_t bytes = source.geometry.pixel_bytes;
    const uint8_t *from = Ptr<const uint8_t>(source.address).get(mem);
    uint8_t *to = Ptr<uint8_t>(dest.address).get(mem);
    const auto texel = [&](uint32_t x, uint32_t y) { return from + source.geometry.byte_offset(source.x + x, source.y + y); };
    for (uint32_t y = 0; y < dest.height; ++y) {
        for (uint32_t x = 0; x < dest.width; ++x) {
            const uint8_t *block[4] = {texel(2 * x, 2 * y), texel(2 * x + 1, 2 * y), texel(2 * x, 2 * y + 1), texel(2 * x + 1, 2 * y + 1)};
            uint8_t *target = to + dest.geometry.byte_offset(dest.x + x, dest.y + y);
            if (format == SCE_GXM_TRANSFER_FORMAT_U5U6U5_BGR) {
                uint32_t sums[3] = {};
                for (const uint8_t *b : block) {
                    const uint16_t v = uint16_t(b[0] | b[1] << 8);
                    sums[0] += v & 31; sums[1] += (v >> 5) & 63; sums[2] += v >> 11;
                }
                const uint16_t v = uint16_t(((sums[0] + 2) / 4) | ((sums[1] + 2) / 4) << 5 | ((sums[2] + 2) / 4) << 11);
                target[0] = uint8_t(v); target[1] = uint8_t(v >> 8);
            } else {
                for (uint32_t c = 0; c < bytes; ++c)
                    target[c] = uint8_t((block[0][c] + block[1][c] + block[2][c] + block[3][c] + 2) / 4);
            }
        }
    }
    refresh_targets(mem, out, dest);
    return 0;
}

// NewFrame owns a host DisplayFrameInfo*, released here exactly like sync.cpp
// new_frame (copied into display state, then deleted).
[[gnu::noinline]] static void new_frame(State &state, DisplayState &display, DisplayFrameInfo *frame) {
    const std::lock_guard<std::mutex> guard(display.display_info_mutex);
    display.next_rendered_frame = *frame;
    delete frame;
    state.should_display = true;
}
[[noreturn, gnu::noinline]] static void unknown_opcode(Command *cmd, MemState &mem) {
    CommandList rest;
    rest.first = cmd;
    trace_scene(rest, mem);
    unsupported("command opcode not implemented");
}
// Guest-visible results of a command list (notifications, sync signals,
// command statuses), published in order once its scene stream was accepted
// and, with surface sync, its rendered surfaces are back in guest memory.
struct Completion {
    enum Kind : uint32_t { Notification, SyncSignal, Status } kind;
    Address address; // notification word or sync object
    uint32_t value;  // notification value or sync timestamp
    int *status;
};
struct Submission {
    std::vector<Completion> completions;
    std::vector<SurfaceReadback> readbacks;
    int result = 0;
};
Submission &submission() {
    static Submission instance;
    return instance;
}
[[gnu::noinline]] static void publish(State &state, MemState &mem, scene::Writer &out, Submission &sub) {
    if (!submit_scene(out, mem))
        unsupported("scene submission failed (see browser log)");
    for (const auto &readback : sub.readbacks)
        sync_surface(mem, readback);
    sub.readbacks.clear();
    bool notified = false;
    {
        std::unique_lock<std::mutex> lock(state.notification_mutex);
        for (const auto &c : sub.completions) {
            if (c.kind == Completion::Notification) {
                *Ptr<uint32_t>(c.address).get(mem) = c.value;
                notified = true;
            }
        }
    }
    for (const auto &c : sub.completions) {
        if (c.kind == Completion::SyncSignal)
            subject_done(Ptr<SceGxmSyncObject>(c.address).get(mem), c.value);
        else if (c.kind == Completion::Status)
            *c.status = static_cast<int>(c.value);
    }
    sub.completions.clear();
    if (notified)
        state.notification_ready.notify_all();
}
static void release_command(Context *ctx, Command *cmd) {
    destroy_command_payload(*cmd);
    if (ctx) ctx->free_func(cmd);
    else generic_command_free(cmd);
}
// Consumes the commands from `cursor` on. It holds no locals with
// destructors and no try: under Emscripten's JS exceptions either would send
// every call below through an invoke_* wrapper, whose 64-bit arguments are
// BigInts allocated per call (the worker's largest garbage source in Limbo).
// An exception leaves `cursor` at the failing command for the caller.
static void consume_commands(State &state, Context *ctx, Command *&cursor, MemState &mem, scene::Writer &out,
    Submission &sub) {
    while (Command *cmd = cursor) {
        CommandHelper helper(cmd);
        int code = 0;
        if (sub.result == 0) switch (cmd->opcode) {
        case CommandOpcode::SetContext: {
            if (!ctx) unsupported("scene without context");
            auto &web = static_cast<WebContext &>(*ctx);
            auto *target = helper.pop<RenderTarget *>();
            const auto *color = helper.pop<SceGxmColorSurface *>();
            const auto *depth = helper.pop<SceGxmDepthStencilSurface *>();
            ctx->current_render_target = target;
            web.has_surface = false;
            const char *why = "scene without a usable color surface";
            if (!target || !color || color->disabled || !color->width || !color->height
                || color->width > 4096 || color->height > 4096
                || !surface_geometry(*color, web.geometry, why)) {
                skip_draw(why);
                end_pass(out);
                break;
            }
            web.samples = target->multisample_mode != SCE_GXM_MULTISAMPLE_NONE ? 2 : 1;
            web.downscale = color->downscale ? 2 : 1;
            web.internal_scale = surface_scale(color->width * web.downscale, color->height * web.downscale);
            web.record.color_surface = *color;
            web.has_surface = true;
            web.has_depth_surface = depth && !depth->disabled();
            if (web.has_depth_surface)
                web.depth = *depth;
            web.record.depth_stencil_surface = web.has_depth_surface ? *depth : SceGxmDepthStencilSurface{};
            begin_pass(web, out);
            break;
        }
        case CommandOpcode::SetState:
            if (!ctx) unsupported("state without context");
            consume_state(static_cast<WebContext &>(*ctx), helper, mem);
            break;
        case CommandOpcode::Draw:
            if (!ctx) unsupported("draw without context");
            consume_draw(static_cast<WebContext &>(*ctx), helper, mem, out);
            break;
        case CommandOpcode::SyncSurfaceData: {
            // Published after the scene is submitted, as the desktop path
            // does (renderer/src/vulkan/scene.cpp signal_notifications).
            const auto vertex = helper.pop<SceGxmNotification>(), fragment = helper.pop<SceGxmNotification>();
            for (const auto &n : {vertex, fragment}) if (n.address)
                require_guest(mem, n.address.address(), sizeof(uint32_t));
            // Surface sync (desktop sync_surface_data): the scene's surface,
            // when it drew anything, is read back before these notifications.
            if (!state.disable_surface_sync && ctx && static_cast<WebContext &>(*ctx).has_surface
                && out.pass_open && out.draws > out.pass_first_draw)
                sub.readbacks.push_back({ctx->record.color_surface.data.address(), static_cast<WebContext &>(*ctx).geometry});
            for (const auto &n : {vertex, fragment}) if (n.address)
                sub.completions.push_back({Completion::Notification, n.address.address(), n.value, nullptr});
            break;
        }
        case CommandOpcode::SignalSyncObject: {
            const auto sync = helper.pop<Ptr<SceGxmSyncObject>>();
            const auto timestamp = helper.pop<uint32_t>();
            if (!sync) unsupported("sync signal without object");
            require_guest(mem, sync.address(), sizeof(SceGxmSyncObject));
            sub.completions.push_back({Completion::SyncSignal, sync.address(), timestamp, nullptr});
            break;
        }
        case CommandOpcode::WaitSyncObject: {
            const auto sync = helper.pop<Ptr<SceGxmSyncObject>>();
            const auto timestamp = helper.pop<uint32_t>();
            if (!sync) unsupported("sync wait without object");
            require_guest(mem, sync.address(), sizeof(SceGxmSyncObject));
            // A signal earlier in this list must be visible to the wait.
            if (!sub.completions.empty() && !out.pass_open)
                publish(state, mem, out, sub);
            if (wishlist(sync.get(mem), timestamp) != SyncWaitResult::Ready)
                sub.result = -1;
            break;
        }
        case CommandOpcode::NewFrame: {
            auto *frame = helper.pop<DisplayFrameInfo *>();
            auto *display = helper.pop<DisplayState *>();
            helper.pop<Context *>();
            if (!display) unsupported("new frame without display state");
            if (frame)
                new_frame(state, *display, frame);
            break;
        }
        case CommandOpcode::Nop:
            // sceGxmFinish: every scene was submitted synchronously and no
            // guest-visible result waits on the GPU.
            code = helper.pop<int>();
            break;
        case CommandOpcode::TransferFill: {
            const uint32_t color = helper.pop<uint32_t>();
            const auto *d = helper.pop<const SceGxmTransferImage *>();
            sub.result = transfer_fill(mem, color, *d, out);
            break;
        }
        case CommandOpcode::TransferCopy: {
            const uint32_t key_value = helper.pop<uint32_t>(), key_mask = helper.pop<uint32_t>();
            const auto key_mode = helper.pop<SceGxmTransferColorKeyMode>();
            const auto *images = helper.pop<const SceGxmTransferImage *>();
            const auto source_type = helper.pop<SceGxmTransferType>(), dest_type = helper.pop<SceGxmTransferType>();
            sub.result = transfer_copy(mem, key_value, key_mask, key_mode, images, source_type, dest_type, out);
            break;
        }
        case CommandOpcode::TransferDownscale: {
            const auto *source = helper.pop<const SceGxmTransferImage *>();
            const auto *dest = helper.pop<const SceGxmTransferImage *>();
            sub.result = transfer_downscale(mem, *source, *dest, out);
            break;
        }
        case CommandOpcode::SignalNotification: {
            const auto n = helper.pop<SceGxmNotification>();
            if (n.address) {
                if (!is_valid_addr_range(mem, n.address.address(), uint64_t(n.address.address()) + sizeof(uint32_t)))
                    sub.result = -1;
                else
                    sub.completions.push_back({Completion::Notification, n.address.address(), n.value, nullptr});
            }
            break;
        }
        default:
            unknown_opcode(cmd, mem);
        }
        if (cmd->status)
            sub.completions.push_back({Completion::Status, 0, uint32_t(sub.result == 0 ? code : -1), cmd->status});
        cursor = cmd->next;
        release_command(ctx, cmd);
    }
}

void submit_command_list(State &state, Context *ctx, CommandList &list) {
    const double started = emscripten_get_now();
    auto &web_state = static_cast<WebState &>(state);
    auto &mem = web_state.mem;
    if (!list.first) return;
    // Writes after this point (the guest's next scene, stream-ordered
    // transfer fills) belong to a new epoch the next check will see.
    texture_cache().epoch = mem_next_write_epoch(mem);
    auto &out = scene::writer();
    out.reset();
    auto &sub = submission();
    sub.completions.clear();
    sub.readbacks.clear();
    sub.result = 0;
    Command *cursor = list.first;
    reset_command_list(list);
    std::exception_ptr failure;
    while (cursor) {
        try {
            consume_commands(state, ctx, cursor, mem, out, sub);
        } catch (const std::exception &error) {
            // `cursor` is the command that threw.
            Command *failed = cursor;
            cursor = failed->next;
            if (survey_mode()) {
                if (++survey_counts()[error.what()] == 1)
                    std::printf("[gxm-survey] first: %s\n", error.what());
                if (failed->status)
                    sub.completions.push_back({Completion::Status, 0, uint32_t(sub.result == 0 ? 0 : -1), failed->status});
                release_command(ctx, failed);
                continue;
            }
            // No later notification/status may acknowledge a failed batch.
            failure = std::current_exception();
            release_command(ctx, failed);
            while (cursor) {
                Command *next = cursor->next;
                release_command(ctx, cursor);
                cursor = next;
            }
        }
    }
    if (!failure)
        publish(state, mem, out, sub);
    timing().build += emscripten_get_now() - started;
    if (failure) std::rethrow_exception(failure);
    if (sub.result != 0 && !survey_mode())
        unsupported("GXM command failed (see browser log)");
}
int wait_for_status(State &, int *status, int signal, bool equal) {
    if ((*status == signal) != equal) unsupported("uncompleted command");
    return *status;
}
void finish(State &s, Context *ctx) {
    send_single_command(s, ctx, CommandOpcode::Nop, true, 1);
}
} // namespace renderer

namespace browser {
// Presentation hook (vita_display_bridge.cpp): a frame whose base is a GPU
// render target is shown from the GPU; otherwise the caller presents guest
// memory.
bool gxm_present_gpu_target(Address base) {
    return web_gxm_present(base) != 0;
}
} // namespace browser
