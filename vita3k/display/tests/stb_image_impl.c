// stb image implementation for the display tests.
//
// The tests construct a real EmuEnvState. Its destructor transitively pulls
// camera.cpp (unique_ptr<CameraState>), which uses stbi_* functions. The only
// in-tree TU providing those lives in the renderer
// (renderer/src/texture/replacement.cpp), which nothing in this test target
// references otherwise, so the archive member is never extracted. Compile the
// same header-only implementation here — exactly as replacement.cpp does —
// instead of dragging the renderer into the test graph.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
