// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <module/guest_format.h>
#include <module/module.h>

#include <kernel/state.h>
#include <util/lock_and_find.h>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceDbg);

// VitaSDK psp2/libdbg.h: both handlers truncate their output to 511 characters. That
// limit covers the module's own prefix, whose format is unknown, so it is applied to
// the formatted message alone.
constexpr std::size_t DBG_OUTPUT_LIMIT = 511;

// The third argument is not a stop request: the handler only reports and returns it. The
// guest's assertion macro stops by itself (VitaSDK psp2/libdbg.h: SCE_DBG_ASSERT calls this
// with 0 and then executes SCE_DBG_BREAK_ACTION, a bkpt), so the break instruction, not this
// handler, ends the thread.
EXPORT(int, sceDbgAssertionHandler, const char *filename, int line, int unk, const char *component, module::vargs messages) {
    TRACY_FUNC(sceDbgAssertionHandler, filename, line, unk, component);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);

    if (!thread) {
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    }

    const char *main_message = messages.next<Ptr<const char>>(*(thread->cpu), emuenv.mem).get(emuenv.mem);
    const auto text = module::format_guest(main_message, *(thread->cpu), emuenv.mem, messages, DBG_OUTPUT_LIMIT);

    LOG_ERROR("Guest assertion failed: component {}, file {}, line {}: {}", component ? component : "", filename ? filename : "", line,
        text ? text->text : "(message could not be formatted)");

    return unk;
}

EXPORT(int, sceDbgLoggingHandler, const char *pFile, int line, int severity, const char *pComponent, module::vargs messages) {
    TRACY_FUNC(sceDbgLoggingHandler, pFile, line, severity, pComponent);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);

    if (!thread) {
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    }

    std::string output = fmt::format("SCE libdbg LOG, LEVEL: {}", severity);

    if (pComponent && (pComponent[0] != '\0')) {
        output += fmt::format(", COMPONENT: {}", pComponent);
    }

    if (pFile && (pFile[0] != '\0')) {
        output += fmt::format(", FILE:{}, LINE:{}", pFile, line);
    }

    const char *main_message = messages.next<Ptr<const char>>(*(thread->cpu), emuenv.mem).get(emuenv.mem);
    const auto text = module::format_guest(main_message, *(thread->cpu), emuenv.mem, messages, DBG_OUTPUT_LIMIT);

    output += fmt::format(" {}", text ? text->text : "(message could not be formatted)");
    LOG_INFO(output);

    if (!text) {
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT;
    }
    // Documented only as negative on truncation; the exact value is unknown.
    return text->length > DBG_OUTPUT_LIMIT ? SCE_KERNEL_ERROR_ERROR : 0;
}

EXPORT(int, sceDbgSetBreakOnErrorState) {
    TRACY_FUNC(sceDbgSetBreakOnErrorState);
    return UNIMPLEMENTED();
}

EXPORT(int, sceDbgSetBreakOnWarningState) {
    TRACY_FUNC(sceDbgSetBreakOnWarningState);
    return UNIMPLEMENTED();
}

EXPORT(int, sceDbgSetMinimumLogLevel) {
    TRACY_FUNC(sceDbgSetMinimumLogLevel);
    return UNIMPLEMENTED();
}
