// Production USSE compiler adapter, shared by the native oracle and browser Wasm.
// Trusted GXP only: header checks do not validate every self-relative GXP table.
#include <shader/spirv_recompiler.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static shader::usse::SpirvCode output;
static std::string error;
extern "C" {
// Formats is either null (RGBA8), or 32 uint32 values: vertex[16], fragment[16].
// Return 0 on success. Output/error remain owned here until the next compile.
EXPORT int gxp_compile(const uint8_t *bytes, uint32_t size, const uint32_t *formats) {
    output.clear(); error.clear();
    try {
        if (!bytes || size < sizeof(SceGxmProgram) || size > 16*1024*1024)
            throw std::runtime_error("GXP size out of range");
        std::vector<uint64_t> storage((size+7)/8);
        std::memcpy(storage.data(), bytes, size);
        const auto &program = *reinterpret_cast<const SceGxmProgram *>(storage.data());
        if (program.magic != 0x00505847 || program.size != size)
            throw std::runtime_error("GXP header magic/size mismatch");
        FeatureState features{};
        shader::Hints hints{};
        hints.color_format = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
        for (size_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; ++i) {
            hints.vertex_textures[i] = formats ? static_cast<SceGxmTextureFormat>(formats[i]) : SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
            hints.fragment_textures[i] = formats ? static_cast<SceGxmTextureFormat>(formats[SCE_GXM_MAX_TEXTURE_UNITS+i]) : SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
        }
        output = shader::convert_gxp(program, "webgpu-runtime", features, shader::Target::SpirVWebGPU, hints).spirv;
        if (output.empty()) throw std::runtime_error("Compiler produced no SPIR-V");
        return 0;
    } catch (const std::exception &e) { error = e.what(); return 1; }
}
EXPORT const uint32_t *gxp_output_data() { return output.data(); }
EXPORT uint32_t gxp_output_size() { return output.size()*4; }
EXPORT const char *gxp_error() { return error.c_str(); }
}
#ifndef __EMSCRIPTEN__
int main(int argc, char **argv) {
    if (argc != 3) { std::cerr << "usage: gxp_compile input.gxp output.spv\n"; return 2; }
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < 0 || in.tellg() > 16*1024*1024) return 2;
    std::vector<uint8_t> bytes(static_cast<size_t>(in.tellg()));
    in.seekg(0); in.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (!in || gxp_compile(bytes.data(), bytes.size(), nullptr)) {
        std::cerr << error << '\n'; return 1;
    }
    std::string disassembly;
    shader::spirv_disasm_print(output, &disassembly);
    std::ofstream(std::string(argv[2])+".txt") << disassembly;
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char *>(output.data()), output.size()*4);
    std::cout << "Translated GXP to " << output.size() << " SPIR-V words\n";
    return out ? 0 : 1;
}
#endif
