// Minimal genuine VitaSDK homebrew fixture for the Vita3K browser bring-up.
//
// Out-of-source build recipe: CMakeLists.txt and README.md in this directory.
//
// The observable HLE-visible result is the process exit status (42), passed
// through sceKernelExitProcess by the VitaSDK crt0 when main returns.

#include <psp2/kernel/processmgr.h>

int main(int argc, char *argv[]) {
    // Deliberately tiny: the value returned here is forwarded by crt0's exit
    // path to sceKernelExitProcess, which is the HLE boundary we want to
    // observe from the browser runtime.
    return 42;
}
