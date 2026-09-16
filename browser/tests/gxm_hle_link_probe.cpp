// Compile/link probe of the REAL SceGxm implementation, with normal bridge
// registration suppressed to measure only the selected call graph. Not a HLE
// replacement and never linked into the browser application.
#include <module/module.h>
#undef EXPORT
#define EXPORT(ret, name, ...) DECL_EXPORT(ret, name, ##__VA_ARGS__)
#include "../../vita3k/modules/SceGxm/SceGxm.cpp"

// Retain real AAPCS import bridges so --gc-sections cannot hide missing backend
// dependencies. No guest instructions are executed by this compile/link probe.
extern "C" __attribute__((used, visibility("default")))
ImportFn gxm_probe_bridge(unsigned index) {
    static const ImportFn bridges[] = {
        make_bridge<&export_sceGxmInitialize>("sceGxmInitialize", &export_sceGxmInitialize),
        make_bridge<&export_sceGxmCreateContext>("sceGxmCreateContext", &export_sceGxmCreateContext),
        make_bridge<&export_sceGxmCreateRenderTarget>("sceGxmCreateRenderTarget", &export_sceGxmCreateRenderTarget),
        make_bridge<&export_sceGxmBeginScene>("sceGxmBeginScene", &export_sceGxmBeginScene),
        make_bridge<&export_sceGxmDraw>("sceGxmDraw", &export_sceGxmDraw),
        make_bridge<&export_sceGxmEndScene>("sceGxmEndScene", &export_sceGxmEndScene),
        make_bridge<&export_sceGxmShaderPatcherCreateVertexProgram>("sceGxmShaderPatcherCreateVertexProgram", &export_sceGxmShaderPatcherCreateVertexProgram),
        make_bridge<&export_sceGxmShaderPatcherCreateFragmentProgram>("sceGxmShaderPatcherCreateFragmentProgram", &export_sceGxmShaderPatcherCreateFragmentProgram),
    };
    return index < sizeof(bridges) / sizeof(*bridges) ? bridges[index] : nullptr;
}
