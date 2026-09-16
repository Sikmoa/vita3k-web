// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
struct EmuEnvState;
namespace browser {
int gxm_initialize(EmuEnvState &env);
int gxm_terminate(EmuEnvState &env);
}
