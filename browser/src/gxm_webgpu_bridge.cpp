// SPDX-License-Identifier: GPL-2.0-or-later
// Browser-only consumer of the production renderer command ABI. No GL/Vulkan.
#include "gxm_webgpu_bridge.h"
#include <emuenv/state.h>
#include <gxm/functions.h>
#include <gxm/state.h>
#include <mem/functions.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <emscripten.h>
#include <stdexcept>
#include <array>
#include <vector>
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
    std::array<float, 6> viewport{};
    std::array<uint32_t, 4> clip{};
    std::array<std::vector<uint8_t>, 2> uniforms;
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
    env.gxm.display_queue.reset();
    return 0;
}
int gxm_terminate(EmuEnvState &env) {
    if (!env.renderer) return SCE_GXM_ERROR_UNINITIALIZED;
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
    while (sync->timestamp_current < timestamp) {
        if (sync->being_deleted) return SyncWaitResult::Shutdown;
        if (timeout_micros >= 0 && (emscripten_get_now() - start) * 1000 >= timeout_micros)
            return SyncWaitResult::TimedOut;
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
        default: printf("[gxm-decode] opcode=%u\n", unsigned(cmd->opcode)); break;
        }
    }
}
static void require_guest(MemState &mem, Address address, size_t size) {
    if (!address || !size || uint64_t(address) + size > (uint64_t(1) << 32)
        || !is_valid_addr_range(mem, address, uint64_t(address) + size))
        unsupported("invalid guest draw range");
}

static void consume_state(WebContext &ctx, CommandHelper &h, MemState &mem) {
    switch (h.pop<GXMState>()) {
    case GXMState::RegionClip:
        ctx.record.region_clip_mode = h.pop<SceGxmRegionClipMode>();
        for (auto &n : ctx.clip) n = h.pop<uint32_t>();
        break;
    case GXMState::Viewport:
        ctx.record.viewport_flat = h.pop<bool>();
        if (!ctx.record.viewport_flat) for (auto &n : ctx.viewport) n = h.pop<float>();
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
    case GXMState::VertexStream: {
        const auto ptr = h.pop<Ptr<const uint8_t>>();
        const auto index = h.pop<size_t>(), size = h.pop<size_t>();
        if (index != 0) unsupported("multiple vertex streams");
        require_guest(mem, ptr.address(), size);
        ctx.record.vertex_streams[index] = {ptr, size};
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
    if (primitive != SCE_GXM_PRIMITIVE_TRIANGLES || instances != 1 || !count || count % 3
        || (format != SCE_GXM_INDEX_FORMAT_U16 && format != SCE_GXM_INDEX_FORMAT_U32))
        unsupported("only non-instanced indexed triangle lists supported");
    if (!ctx.has_surface || !ctx.record.vertex_program || !ctx.record.fragment_program)
        unsupported("draw without surface/programs");
    const auto &surface = ctx.record.color_surface;
    const auto w = surface.width, height = surface.height;
    const std::array<float, 6> full = {float(w) * .5f, float(height) * .5f, .5f, float(w) * .5f, -float(height) * .5f, .5f};
    if (ctx.record.viewport_flat || ctx.viewport != full
        || ctx.record.region_clip_mode != SCE_GXM_REGION_CLIP_OUTSIDE
        || ctx.clip != std::array<uint32_t, 4>{0, w - 1, 0, height - 1})
        unsupported("non-default viewport/clip");
    const auto *vp = ctx.record.vertex_program.get(mem);
    const auto *fp = ctx.record.fragment_program.get(mem);
    if (!vp->renderer_data || !fp->renderer_data || fp->is_maskupdate
        || vp->renderer_data->textures_used.any() || fp->renderer_data->textures_used.any()
        || vp->streams.size() != 1 || vp->attributes.empty() || vp->attributes.size() > 16)
        unsupported("textures/mask/multiple streams or missing program metadata");
    const size_t index_size = format == SCE_GXM_INDEX_FORMAT_U16 ? 2 : 4;
    const size_t index_bytes = size_t(count) * index_size;
    const auto &stream = ctx.record.vertex_streams[0];
    const size_t stride = vp->streams[0].stride;
    if (gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(vp->streams[0].indexSource)))
        unsupported("instanced vertex stream");
    if (!stride || stream.size > 16 * 1024 * 1024 || index_bytes > 16 * 1024 * 1024)
        unsupported("draw upload size");
    require_guest(mem, indices.address(), index_bytes);
    require_guest(mem, stream.data.address(), stream.size);
    std::vector<std::array<uint32_t, 3>> attributes;
    for (const auto &a : vp->attributes) {
        const auto info = vp->renderer_data->attribute_infos.find(a.regIndex);
        if (a.streamIndex != 0 || a.format != SCE_GXM_ATTRIBUTE_FORMAT_F32
            || !a.componentCount || a.componentCount > 4 || a.offset + a.componentCount * 4 > stride
            || info == vp->renderer_data->attribute_infos.end() || info->second.is_integer)
            unsupported("vertex attribute format");
        attributes.push_back({info->second.location, a.offset, a.componentCount});
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
    word(0x47584d31); word(stride); word(index_size);
    for (auto size : {index_bytes, stream.size, size_t(vs->size), size_t(fs->size), ctx.uniforms[0].size(), ctx.uniforms[1].size()}) word(size);
    word(attributes.size());
    const float info[12] = {1,1,1,1, 1,float(w),float(height),ctx.viewport[2], ctx.viewport[5],0,0,0};
    append(info, sizeof(info));
    for (const auto &a : attributes) for (auto value : a) word(value);
    append(indices.get(mem), index_bytes); append(stream.data.get(mem), stream.size);
    append(vs, vs->size); append(fs, fs->size);
    for (const auto &data : ctx.uniforms) append(data.data(), data.size());
    return web_gxm_draw(packet.data(), packet.size(), w, height, surface.strideInPixels * 4, surface.data.get(mem));
}

void submit_command_list(State &state, Context *ctx, CommandList &list) {
    // Reject unsupported opcodes before publishing any batch completion.
    for (Command *cmd = list.first; cmd; cmd = cmd->next) {
        if (cmd->opcode != CommandOpcode::Nop && cmd->opcode != CommandOpcode::TransferFill
            && cmd->opcode != CommandOpcode::SignalNotification && cmd->opcode != CommandOpcode::SetContext
            && cmd->opcode != CommandOpcode::SetState && cmd->opcode != CommandOpcode::Draw
            && cmd->opcode != CommandOpcode::SyncSurfaceData) {
            trace_scene(list, static_cast<WebState &>(state).mem);
            unsupported("command opcode (drawing not connected)");
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
            if (!target || !color || depth || color->disabled || color->downscale || color->gamma
                || color->colorFormat != SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR
                || color->surfaceType != SCE_GXM_COLOR_SURFACE_LINEAR
                || !color->width || !color->height || color->width > 4096 || color->height > 4096
                || color->strideInPixels < color->width || color->strideInPixels > UINT32_MAX / 4)
                unsupported("color/depth surface format");
            require_guest(static_cast<WebState &>(state).mem, color->data.address(),
                (uint64_t(color->height) - 1) * color->strideInPixels * 4 + uint64_t(color->width) * 4);
            web.record.color_surface = *color; web.has_surface = true;
            ctx->current_render_target = target;
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
            auto &mem = static_cast<WebState &>(state).mem;
            const auto vertex = helper.pop<SceGxmNotification>(), fragment = helper.pop<SceGxmNotification>();
            for (const auto &n : {vertex, fragment}) if (n.address)
                require_guest(mem, n.address.address(), sizeof(uint32_t));
            for (const auto &n : {vertex, fragment}) if (n.address) *n.address.get(mem) = n.value;
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
            const auto n = helper.pop<SceGxmNotification>();
            auto &mem = static_cast<WebState &>(state).mem;
            if (n.address) {
                if (!is_valid_addr_range(mem, n.address.address(), uint64_t(n.address.address()) + sizeof(uint32_t)))
                    result = -1;
                else *n.address.get(mem) = n.value;
            }
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
