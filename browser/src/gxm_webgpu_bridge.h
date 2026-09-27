// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
struct EmuEnvState;
namespace browser {
int gxm_initialize(EmuEnvState &env);
// VITA3K_GXM_SURVEY: print the skipped-command histogram (no-op otherwise).
void gxm_survey_report();
// Cumulative host time of scene building, texture decoding and JS submission.
void gxm_timing_report();
// Presents the GPU render target at `base`; false when none exists there.
bool gxm_present_gpu_target(uint32_t base);
int gxm_terminate(EmuEnvState &env);
}
