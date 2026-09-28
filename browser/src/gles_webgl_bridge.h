// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

struct CPUState;
struct EmuEnvState;
struct SceKernelModuleInfo;

namespace browser::gles {
// Opt-in, per-app lifetime. The graphics adapter only replaces the named PVR
// user libraries; it does not pretend to implement the PowerVR kernel driver.
class Session {
public:
    explicit Session(EmuEnvState &env);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
};

bool replace_module(EmuEnvState &env, const SceKernelModuleInfo &info);
bool call_import(EmuEnvState &env, CPUState &cpu, std::uint32_t nid);
const char *import_name(std::uint32_t nid);
} // namespace browser::gles
