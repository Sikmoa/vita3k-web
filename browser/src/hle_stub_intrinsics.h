// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

struct KernelState;

namespace browser {
// Replace the HLE stubs of a few pure field getters with equivalent ARM code
// as modules load (KernelState::patch_hle_stub). Call before loading modules.
void install_hle_stub_intrinsics(KernelState &kernel);
}
