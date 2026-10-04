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

#include <cpu/functions.h>
#include <kernel/load_self.h>
#include <kernel/relocation.h>
#include <kernel/state.h>
#include <kernel/types.h>

#include <nids/functions.h>
#include <util/arm.h>
#include <util/fs.h>
#include <util/log.h>

#include <util/elf.h>
// clang-format off
#define SCE_ELF_DEFS_TARGET
#include <sce-elf-defs.h>
#undef SCE_ELF_DEFS_TARGET
// clang-format on
#include <miniz.h>
#include <self.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>

static constexpr uint32_t NID_MODULE_STOP = 0x79F8E492;
static constexpr uint32_t NID_MODULE_EXIT = 0x913482A9;
static constexpr uint32_t NID_MODULE_START = 0x935CD196;
static constexpr uint32_t NID_MODULE_INFO = 0x6C2224BA;
static constexpr uint32_t NID_SYSLYB = 0x936c8a78;
static constexpr uint32_t NID_PROCESS_PARAM = 0x70FBA1E7;

static constexpr bool LOG_MODULE_LOADING = false;

struct VarImportsHeader {
    uint32_t unk : 4; // Must be zero
    uint32_t reloc_data_size : 24; // Size of Relocation data in bytes, includes this header.
    uint32_t unk2 : 4; // Must be zero
};
static_assert(sizeof(VarImportsHeader) == sizeof(uint32_t));

static bool load_var_imports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, const SegmentInfosForReloc &segments, KernelState &kernel, MemState &mem, uint32_t module_id) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        if (kernel.debugger.log_imports) {
            const char *const name = import_name(nid);
            LOG_DEBUG("\tNID {} ({}). entry: {}, *entry: {}", log_hex(nid), name, entry, log_hex(*entry.get(mem)));
        }

        VarImportsHeader *const var_reloc_header = entry.cast<VarImportsHeader>().get(mem);
        const auto var_reloc_entries = static_cast<void *>(var_reloc_header + 1);
        const uint32_t reloc_size = (var_reloc_header->reloc_data_size > sizeof(VarImportsHeader)) ? (var_reloc_header->reloc_data_size - sizeof(VarImportsHeader)) : 0;

        const char *const name = import_name(nid);
        Address export_address;
        kernel.var_binding_infos.emplace(nid, VarBindingInfo{ var_reloc_entries, reloc_size, module_id });
        const ExportNids::iterator export_address_it = kernel.export_nids.find(nid);
        if (export_address_it != kernel.export_nids.end()) {
            export_address = export_address_it->second;
        } else {
            constexpr auto STUB_SYMVAL = 0xDEADBEEF;
            LOG_DEBUG("\tNID NOT FOUND {} ({}) at {}, setting to stub value {}", log_hex(nid), name, log_hex(entry.address()), log_hex(STUB_SYMVAL));

            auto alloc_name = fmt::format("Stub var import reloc symval, NID {} ({})", log_hex(nid), name);
            auto stub_symval_ptr = Ptr<uint32_t>(alloc(mem, 4, alloc_name.c_str()));
            *stub_symval_ptr.get(mem) = STUB_SYMVAL;

            export_address = stub_symval_ptr.address();

            // Use same stub for other var imports
            kernel.export_nids.emplace(nid, export_address);
        }

        if (reloc_size)
            if (!relocate(var_reloc_entries, reloc_size, segments, mem, true, export_address))
                return false;
    }

    return true;
}

static bool unload_var_imports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, KernelState &kernel, MemState &mem, uint32_t module_id) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        VarImportsHeader *const var_reloc_header = reinterpret_cast<VarImportsHeader *>(entry.get(mem));
        const auto var_reloc_entries = static_cast<void *>(var_reloc_header + 1);
        const uint32_t reloc_size = (var_reloc_header->reloc_data_size > sizeof(VarImportsHeader)) ? (var_reloc_header->reloc_data_size - sizeof(VarImportsHeader)) : 0;

        // remove the binding info from the map
        VarBindingInfo binding_info{ var_reloc_entries, reloc_size, module_id };
        auto range = kernel.var_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            if (memcmp(&it->second, &binding_info, sizeof(VarBindingInfo)) == 0) {
                kernel.var_binding_infos.erase(it);
                break;
            }
        }
    }

    return true;
}

static bool load_func_imports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, uint32_t library_nid, const SegmentInfosForReloc &segments, KernelState &kernel, const MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        if (kernel.debugger.log_imports) {
            const char *const name = import_name(nid);
            LOG_DEBUG("\tNID {} ({}) at {}", log_hex(nid), name, log_hex(entry.address()));
        }

        // Fall back to the plain NID for exports registered without library information.
        Address func_address = 0;
        if (const auto it = kernel.export_nids_by_lib.find(lib_export_key(library_nid, nid)); it != kernel.export_nids_by_lib.end()) {
            func_address = it->second;
        } else if (const auto nid_it = kernel.export_nids.find(nid); nid_it != kernel.export_nids.end()) {
            func_address = nid_it->second;
        }
        uint32_t *const stub = entry.get(mem);

        kernel.func_binding_infos.emplace(nid, FuncBindingInfo{ entry.address(), library_nid });
        if (!func_address) {
            stub[0] = 0xef000000; // svc #0 - Call our interrupt hook.
            stub[1] = 0xe1a0f00e; // mov pc, lr - Return to the caller.
            stub[2] = nid; // Our interrupt hook will read this.
            if (kernel.patch_hle_stub)
                kernel.patch_hle_stub(nid, stub);
        } else {
            stub[0] = encode_arm_inst(INSTRUCTION_MOVW, (uint16_t)func_address, 12);
            stub[1] = encode_arm_inst(INSTRUCTION_MOVT, (uint16_t)(func_address >> 16), 12);
            stub[2] = encode_arm_inst(INSTRUCTION_BRANCH, 0, 12);
        }
        if (stub[3]) { // if function's associated reftable exists
            VarImportsHeader *const var_reloc_header = Ptr<VarImportsHeader>(stub[3]).get(mem);
            const auto var_reloc_entries = static_cast<void *>(var_reloc_header + 1);
            const uint32_t reloc_size = (var_reloc_header->reloc_data_size > sizeof(VarImportsHeader)) ? (var_reloc_header->reloc_data_size - sizeof(VarImportsHeader)) : 0;
            if (reloc_size) {
                if (!relocate(var_reloc_entries, reloc_size, segments, mem, true, entry.address())) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool unload_func_imports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, KernelState &kernel) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        // remove the stub from the table
        auto range = kernel.func_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.entry_address == entry.address()) {
                kernel.func_binding_infos.erase(it);
                break;
            }
        }
    }
    return true;
}

static bool load_imports(const sce_module_info_raw &module, Ptr<const void> segment_address, const SegmentInfosForReloc &segments, KernelState &kernel, MemState &mem, bool is_unload = false) {
    // Diagnostic only: enumerate the static import surface before execution,
    // including modules embedded in the bootimage. Do not alter resolution.
    // The filter is an exact module name; '*' traces every loaded module.
    const char *const trace_filter = std::getenv("VITA3K_TRACE_MODULE_IMPORTS");
    const std::string module_name(module.name, strnlen(module.name, sizeof(module.name)));
    const bool trace_imports = !is_unload && trace_filter &&
        (std::strcmp(trace_filter, "*") == 0 || module_name == trace_filter);
    const uint8_t *const base = segment_address.cast<const uint8_t>().get(mem);
    const sce_module_imports_raw *const imports_begin = reinterpret_cast<const sce_module_imports_raw *>(base + module.import_top);
    const sce_module_imports_raw *const imports_end = reinterpret_cast<const sce_module_imports_raw *>(base + module.import_end);

    for (const sce_module_imports_raw *imports = imports_begin; imports < imports_end; imports = reinterpret_cast<const sce_module_imports_raw *>(reinterpret_cast<const uint8_t *>(imports) + imports->size)) {
        assert(imports->num_syms_tls_vars == 0);

        Address library_name{};
        uint32_t library_nid{};
        Address func_nid_table{};
        Address func_entry_table{};
        Address var_nid_table{};
        Address var_entry_table{};

        if (imports->size == 0x24) {
            auto short_imports = reinterpret_cast<const sce_module_imports_short_raw *>(imports);
            library_nid = short_imports->library_nid;
            library_name = short_imports->library_name;
            func_nid_table = short_imports->func_nid_table;
            func_entry_table = short_imports->func_entry_table;
            var_nid_table = short_imports->var_nid_table;
            var_entry_table = short_imports->var_entry_table;
        } else if (imports->size == 0x34) {
            auto long_imports = imports;
            library_nid = long_imports->library_nid;
            library_name = long_imports->library_name;
            func_nid_table = long_imports->func_nid_table;
            func_entry_table = long_imports->func_entry_table;
            var_nid_table = long_imports->var_nid_table;
            var_entry_table = long_imports->var_entry_table;
        }

        if (!is_unload && library_name)
            kernel.imported_libraries.insert(Ptr<const char>(library_name).get(mem));
        std::string lib_name;
        if (kernel.debugger.log_imports) {
            lib_name = Ptr<const char>(library_name).get(mem);
            LOG_INFO("Loading func imports from {}", lib_name);
        }

        const uint32_t *const nids = Ptr<const uint32_t>(func_nid_table).get(mem);
        const Ptr<uint32_t> *const entries = Ptr<Ptr<uint32_t>>(func_entry_table).get(mem);

        const size_t num_syms_funcs = imports->num_syms_funcs;
        if (trace_imports) {
            for (size_t i = 0; i < num_syms_funcs; ++i)
                LOG_INFO("Static import: module={} library={} library_nid={:08X} kind=function nid={:08X} name={} entry={:08X}",
                    module_name, Ptr<const char>(library_name).get(mem), library_nid,
                    nids[i], import_name(nids[i]), entries[i].address());
        }
        if (!is_unload && !load_func_imports(nids, entries, num_syms_funcs, library_nid, segments, kernel, mem))
            return false;
        if (is_unload && !unload_func_imports(nids, entries, num_syms_funcs, kernel))
            return false;

        const uint32_t *const var_nids = Ptr<const uint32_t>(var_nid_table).get(mem);
        const Ptr<uint32_t> *const var_entries = Ptr<Ptr<uint32_t>>(var_entry_table).get(mem);

        const auto var_count = imports->num_syms_vars;
        if (trace_imports) {
            for (size_t i = 0; i < var_count; ++i)
                LOG_INFO("Static import: module={} library={} library_nid={:08X} kind=variable nid={:08X} name={} entry={:08X}",
                    module_name, Ptr<const char>(library_name).get(mem), library_nid,
                    var_nids[i], import_name(var_nids[i]), var_entries[i].address());
        }

        if (kernel.debugger.log_imports && var_count > 0)
            LOG_INFO("Loading var imports from {}", lib_name);

        if (!is_unload && !load_var_imports(var_nids, var_entries, var_count, segments, kernel, mem, module.module_nid))
            return false;
        if (is_unload && !unload_var_imports(var_nids, var_entries, var_count, kernel, mem, module.module_nid))
            return false;
    }

    return true;
}

static bool load_func_exports(SceKernelModuleInfo *kernel_module_info, const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, uint32_t library_nid, KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        if (nid == NID_MODULE_START) {
            kernel_module_info->start_entry = entry;
            continue;
        }

        if (nid == NID_MODULE_STOP) {
            kernel_module_info->stop_entry = entry;
            continue;
        }
        if (nid == NID_MODULE_EXIT) {
            kernel_module_info->exit_entry = entry;
            continue;
        }

        // Only the library that actually took the plain NID entry may drop it again.
        if (kernel.export_nids.emplace(nid, entry.address()).second)
            kernel.export_nid_owners.insert_or_assign(nid, library_nid);
        kernel.export_nids_by_lib.insert_or_assign(lib_export_key(library_nid, nid), entry.address());
        // substitute supervisor calls to direct function calls in loaded modules
        auto range = kernel.func_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            // A same-named export from an unrelated library must not hijack these importers.
            if (it->second.library_nid != library_nid)
                continue;
            auto address = it->second.entry_address;
            uint32_t *const stub = Ptr<uint32_t>(address).get(mem);
            stub[0] = encode_arm_inst(INSTRUCTION_MOVW, (uint16_t)entry.address(), 12);
            stub[1] = encode_arm_inst(INSTRUCTION_MOVT, (uint16_t)(entry.address() >> 16), 12);
            stub[2] = encode_arm_inst(INSTRUCTION_BRANCH, 0, 12);
            kernel.invalidate_jit_cache(address, 3 * sizeof(uint32_t));
        }

        if (kernel.debugger.log_exports) {
            const char *const name = import_name(nid);

            LOG_DEBUG("\tNID {} ({}) at {}", log_hex(nid), name, log_hex(entry.address()));
        }
    }

    return true;
}

// An import stub resolved to an export branches through MOVW/MOVT of its address.
static bool stub_targets(const uint32_t *stub, Address address) {
    return stub[0] == encode_arm_inst(INSTRUCTION_MOVW, (uint16_t)address, 12)
        && stub[1] == encode_arm_inst(INSTRUCTION_MOVT, (uint16_t)(address >> 16), 12);
}

static bool unload_func_exports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, uint32_t library_nid, KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];

        if (nid == NID_MODULE_START || nid == NID_MODULE_STOP || nid == NID_MODULE_EXIT)
            continue;

        const Address unloaded_address = entries[i].address();
        // Another module may have taken this library key since, and its export is still live.
        if (const auto lib_it = kernel.export_nids_by_lib.find(lib_export_key(library_nid, nid)); lib_it != kernel.export_nids_by_lib.end() && lib_it->second == unloaded_address)
            kernel.export_nids_by_lib.erase(lib_it);
        // Redirects move the address but not the ownership, so only the owner may drop the plain entry.
        if (const auto owner_it = kernel.export_nid_owners.find(nid); owner_it != kernel.export_nid_owners.end() && owner_it->second == library_nid) {
            kernel.export_nids.erase(nid);
            kernel.export_nid_owners.erase(owner_it);
        }
        // invalidate all lle nid calls
        auto range = kernel.func_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            Address entry = it->second.entry_address;
            uint32_t *stub = Ptr<uint32_t>(entry).get(mem);
            // A stub bound through the plain NID fallback names another library but still branches here.
            if (it->second.library_nid != library_nid && !stub_targets(stub, unloaded_address))
                continue;

            stub[0] = 0xef000000; // svc #0 - Call our interrupt hook.
            stub[1] = 0xe1a0f00e; // mov pc, lr - Return to the caller.
            stub[2] = nid; // Our interrupt hook will read this.
            if (kernel.patch_hle_stub)
                kernel.patch_hle_stub(nid, stub);
            kernel.invalidate_jit_cache(entry, 3 * sizeof(uint32_t));
        }
    }

    return true;
}

static bool load_var_exports(const uint32_t *nids, const Ptr<uint32_t> *entries, size_t count, KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];
        const Ptr<uint32_t> entry = entries[i];

        if (nid == NID_PROCESS_PARAM) {
            if (!kernel.process_param)
                kernel.load_process_param(mem, entry);
            LOG_DEBUG("\tNID {} (SCE_PROC_PARAMS) at {}", log_hex(nid), log_hex(entry.address()));
            continue;
        }

        if (nid == NID_MODULE_INFO) {
            LOG_DEBUG("\tNID {} (NID_MODULE_INFO) at {}", log_hex(nid), log_hex(entry.address()));
            continue;
        }

        if (nid == NID_SYSLYB) {
            LOG_DEBUG("\tNID {} (SYSLYB) at {}", log_hex(nid), log_hex(entry.address()));
            continue;
        }

        if (kernel.debugger.log_exports) {
            const char *const name = import_name(nid);

            LOG_DEBUG("\tNID {} ({}) at {}", log_hex(nid), name, log_hex(entry.address()));
        }

        Address old_entry_address = 0;
        auto nid_it = kernel.export_nids.find(nid);
        if (nid_it != kernel.export_nids.end()) {
            LOG_DEBUG("Found previously not found variable. nid:{}, new_entry_point:{}", log_hex(nid), log_hex(entry.address()));
            old_entry_address = kernel.export_nids[nid];
        }
        kernel.export_nids[nid] = entry.address();

        auto range = kernel.var_binding_infos.equal_range(nid);
        for (auto j = range.first; j != range.second; ++j) {
            auto &var_binding_info = j->second;
            if (var_binding_info.size == 0)
                continue;

            SegmentInfosForReloc seg;
            const auto &module_info = kernel.loaded_modules[kernel.module_uid_by_nid[var_binding_info.module_nid]];
            if (!module_info) {
                LOG_ERROR("Module not found by nid: {} uid: {}", log_hex(var_binding_info.module_nid), kernel.module_uid_by_nid[var_binding_info.module_nid]);
            } else {
                for (int k = 0; k < MODULE_INFO_NUM_SEGMENTS; k++) {
                    const auto &segment = module_info->info.segments[k];
                    if (segment.size > 0) {
                        seg[k] = { segment.vaddr.address(), 0, segment.memsz }; // p_vaddr is not used in variable relocations
                    }
                }
            }

            // Note: We make the assumption that variables are not imported into executable code (wouldn't make a lot of sense)
            // If this is not the case, uncomment the following
            /* if (!seg.empty()) {
                for (const auto &[key, value] : seg) {
                    kernel.invalidate_jit_cache(value.addr, value.size);
                }
            }*/

            if (!seg.empty()) {
                if (!relocate(var_binding_info.entries, var_binding_info.size, seg, mem, true, entry.address())) {
                    LOG_ERROR("Failed to relocate late binding info");
                }
            }
        }
        if (old_entry_address)
            free(mem, old_entry_address);
    }
    return true;
}

static bool unload_var_exports(const uint32_t *nids, size_t count, KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t nid = nids[i];

        if (nid == NID_PROCESS_PARAM || nid == NID_MODULE_INFO || nid == NID_SYSLYB)
            continue;

        // replace again the nid by a stub
        constexpr auto STUB_SYMVAL = 0xDEADBEEF;

        auto alloc_name = fmt::format("Stub var import reloc symval, NID {} ({})", log_hex(nid), import_name(nid));
        auto stub_symval_ptr = Ptr<uint32_t>(alloc(mem, 4, alloc_name.c_str()));
        *stub_symval_ptr.get(mem) = STUB_SYMVAL;

        const Ptr<uint32_t> entry = stub_symval_ptr;

        // Use same stub for other var imports
        kernel.export_nids[nid] = entry.address();

        auto range = kernel.var_binding_infos.equal_range(nid);
        for (auto it = range.first; it != range.second; ++it) {
            auto &var_binding_info = it->second;
            if (var_binding_info.size == 0)
                continue;

            SegmentInfosForReloc seg;
            const auto &module_info = kernel.loaded_modules[kernel.module_uid_by_nid[var_binding_info.module_nid]];
            if (!module_info) {
                LOG_ERROR("Module not found by nid: {} uid: {}", log_hex(var_binding_info.module_nid), kernel.module_uid_by_nid[var_binding_info.module_nid]);
            } else {
                for (int k = 0; k < MODULE_INFO_NUM_SEGMENTS; k++) {
                    const auto &segment = module_info->info.segments[k];
                    if (segment.size > 0) {
                        seg[k] = { segment.vaddr.address(), 0, segment.memsz }; // p_vaddr is not used in variable relocations
                    }
                }
            }

            if (!seg.empty()) {
                if (!relocate(var_binding_info.entries, var_binding_info.size, seg, mem, true, entry.address())) {
                    LOG_ERROR("Failed to relocate late binding info");
                }
            }
        }
    }
    return true;
}

static bool load_exports(SceKernelModuleInfo *kernel_module_info, const sce_module_info_raw &module, Ptr<const void> segment_address, KernelState &kernel, MemState &mem, bool is_unload = false) {
    const uint8_t *const base = segment_address.cast<const uint8_t>().get(mem);
    const sce_module_exports_raw *const exports_begin = reinterpret_cast<const sce_module_exports_raw *>(base + module.export_top);
    const sce_module_exports_raw *const exports_end = reinterpret_cast<const sce_module_exports_raw *>(base + module.export_end);

    for (const sce_module_exports_raw *exports = exports_begin; exports < exports_end; exports = reinterpret_cast<const sce_module_exports_raw *>(reinterpret_cast<const uint8_t *>(exports) + exports->size)) {
        const char *const lib_name = Ptr<const char>(exports->library_name).get(mem);

        if (kernel.debugger.log_exports)
            LOG_INFO("Loading func exports from {}", lib_name ? lib_name : "unknown");

        const uint32_t *const nids = Ptr<const uint32_t>(exports->nid_table).get(mem);
        const Ptr<uint32_t> *const entries = Ptr<Ptr<uint32_t>>(exports->entry_table).get(mem);
        if (!is_unload && !load_func_exports(kernel_module_info, nids, entries, exports->num_syms_funcs, exports->library_nid, kernel, mem))
            return false;
        if (is_unload && !unload_func_exports(nids, entries, exports->num_syms_funcs, exports->library_nid, kernel, mem))
            return false;

        const auto var_count = exports->num_syms_vars;

        if (kernel.debugger.log_exports && var_count > 0) {
            LOG_INFO("Loading var exports from {}", lib_name ? lib_name : "unknown");
        }

        if (!is_unload && !load_var_exports(&nids[exports->num_syms_funcs], &entries[exports->num_syms_funcs], var_count, kernel, mem))
            return false;
        if (is_unload && !unload_var_exports(&nids[exports->num_syms_funcs], var_count, kernel, mem))
            return false;
    }

    return true;
}

// Bounds for the relocated metadata consumed by the linker below. Deliberately
// not a relocation interpreter: relocation targets/opcodes and late bindings
// still have the existing trusted-module contract.
static bool validate_module_tables(const sce_module_info_raw &module, Address base, uint32_t size,
    const SegmentInfosForReloc &segments, const MemState &mem) {
    const auto relative_range = [size](uint32_t begin, uint32_t end) {
        return !(begin & 3) && !(end & 3) && begin <= end && end <= size;
    };
    const auto guest_range = [&](Address address, uint64_t length, uint32_t alignment = 4) {
        if (length == 0)
            return true;
        if (!address || address % alignment)
            return false;
        for (const auto &[_, segment] : segments) {
            if (address >= segment.addr && address - segment.addr <= segment.size
                && length <= segment.size - (address - segment.addr))
                return true;
        }
        return false;
    };
    const auto guest_string = [&](Address address) {
        if (!address)
            return true; // Nameless main-module exports are normal.
        for (const auto &[_, segment] : segments) {
            if (address >= segment.addr && address - segment.addr < segment.size)
                return std::memchr(Ptr<const char>(address).get(mem), 0,
                           segment.size - (address - segment.addr)) != nullptr;
        }
        return false;
    };
    const auto entry_offset = [size](uint32_t offset) {
        return offset == 0 || offset == UINT32_MAX
            || ((offset & ~1u) < size && size - (offset & ~1u) >= 2);
    };
    if (!relative_range(module.export_top, module.export_end)
        || !relative_range(module.import_top, module.import_end)
        || !relative_range(module.exidx_top, module.exidx_end)
        || !relative_range(module.extab_top, module.extab_end)
        || !entry_offset(module.module_start) || !entry_offset(module.module_stop)
        || module.tls_filesz > module.tls_memsz
        || module.tls_start > size || module.tls_filesz > size - module.tls_start
        || (!module.tls_start && module.tls_filesz))
        return false;

    const auto var_reftable = [&](Address address) {
        if (!guest_range(address, sizeof(VarImportsHeader)))
            return false;
        const auto *header = Ptr<const VarImportsHeader>(address).get(mem);
        return header->reloc_data_size >= sizeof(VarImportsHeader)
            && guest_range(address, header->reloc_data_size);
    };
    const auto imports_table = [&](Address nids, Address entries, uint32_t count, bool functions) {
        if (!guest_range(nids, uint64_t(count) * 4) || !guest_range(entries, uint64_t(count) * 4))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            const auto address = Ptr<const uint32_t>(entries).get(mem)[i];
            if (functions) {
                // load_func_imports both patches the first three words and reads
                // the fourth word for the optional relocation reference table.
                if (!guest_range(address, 4 * sizeof(uint32_t)))
                    return false;
                const auto reftable = Ptr<const uint32_t>(address).get(mem)[3];
                if (reftable && !var_reftable(reftable))
                    return false;
            } else if (!var_reftable(address)) {
                return false;
            }
        }
        return true;
    };

    const auto *bytes = Ptr<const uint8_t>(base).get(mem);
    for (uint32_t offset = module.import_top; offset < module.import_end;) {
        if (module.import_end - offset < sizeof(uint16_t))
            return false;
        uint16_t record_size;
        std::memcpy(&record_size, bytes + offset, sizeof(record_size));
        if ((record_size != sizeof(sce_module_imports_raw) && record_size != sizeof(sce_module_imports_short_raw))
            || record_size > module.import_end - offset)
            return false;
        sce_module_imports_raw imports{};
        if (record_size == sizeof(sce_module_imports_short_raw)) {
            sce_module_imports_short_raw short_imports;
            std::memcpy(&short_imports, bytes + offset, sizeof(short_imports));
            imports.num_syms_funcs = short_imports.num_syms_funcs;
            imports.num_syms_vars = short_imports.num_syms_vars;
            imports.num_syms_tls_vars = short_imports.num_syms_tls_vars;
            imports.library_name = short_imports.library_name;
            imports.func_nid_table = short_imports.func_nid_table;
            imports.func_entry_table = short_imports.func_entry_table;
            imports.var_nid_table = short_imports.var_nid_table;
            imports.var_entry_table = short_imports.var_entry_table;
        } else {
            std::memcpy(&imports, bytes + offset, sizeof(imports));
        }
        if (imports.num_syms_tls_vars || !imports.library_name || !guest_string(imports.library_name)
            || !imports_table(imports.func_nid_table, imports.func_entry_table, imports.num_syms_funcs, true)
            || !imports_table(imports.var_nid_table, imports.var_entry_table, imports.num_syms_vars, false))
            return false;
        offset += record_size;
    }
    for (uint32_t offset = module.export_top; offset < module.export_end;) {
        if (module.export_end - offset < sizeof(sce_module_exports_raw))
            return false;
        sce_module_exports_raw exports;
        std::memcpy(&exports, bytes + offset, sizeof(exports));
        if (exports.size != sizeof(exports) || exports.num_syms_tls_vars
            || !guest_string(exports.library_name))
            return false;
        const uint64_t count = uint64_t(exports.num_syms_funcs) + exports.num_syms_vars;
        if (!guest_range(exports.nid_table, count * 4) || !guest_range(exports.entry_table, count * 4))
            return false;
        for (uint64_t i = 0; i < count; ++i) {
            const auto address = Ptr<const uint32_t>(exports.entry_table).get(mem)[i];
            const auto nid = Ptr<const uint32_t>(exports.nid_table).get(mem)[i];
            if (!guest_range(address & ~1u, 1, 1))
                return false;
            if (i >= exports.num_syms_funcs && nid == NID_PROCESS_PARAM) {
                // load_process_param always reads magic, version and fw_version,
                // then sce_libc_param except for old homebrew (version zero).
                if (!guest_range(address, offsetof(SceProcessParam, fw_version) + sizeof(uint32_t)))
                    return false;
                const auto *param = Ptr<const SceProcessParam>(address).get(mem);
                if (param->version && !guest_range(address, offsetof(SceProcessParam, sce_libc_param) + sizeof(uint32_t)))
                    return false;
            }
        }
        offset += exports.size;
    }
    return true;
}

// Bound record reads before calling the existing relocation engine. This does
// NOT validate relocation segment references, patch targets or opcode semantics.
static bool has_complete_relocation_records(const uint8_t *bytes, uint32_t size) {
    constexpr uint8_t record_sizes[] = { 12, 8, 8, 8, 4, 4, 4, 4, 4, 4 };
    uint32_t offset = 0;
    while (offset < size) {
        const auto format = bytes[offset] & 0xf;
        if (format >= sizeof(record_sizes) || record_sizes[format] > size - offset)
            return false;
        offset += record_sizes[format];
    }
    return true;
}

// Unknown size is private to the trusted native wrapper, never a public opt-out.
static SceUID load_self_impl(KernelState &kernel, MemState &mem, const void *self, std::size_t self_size, const std::string &self_path, const fs::path &dump_path) {
    if (!self)
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    const uint8_t *const image_bytes = static_cast<const uint8_t *>(self);
    std::size_t image_size = self_size;
    auto has_bytes = [&image_size, self_size](std::uint64_t offset, std::uint64_t size) {
        return self_size == 0 || (offset <= image_size && size <= image_size - offset);
    };
    if (self_size != 0 && self_size < sizeof(uint32_t))
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;

    constexpr uint32_t SCE_MAGIC = 0x00454353; // "SCE\0"
    uint32_t magic = 0;
    std::memcpy(&magic, self, sizeof(magic));
    const bool is_self = (magic == SCE_MAGIC);
    if (is_self && self_size != 0 && self_size < sizeof(SCE_header))
        return SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER;
    if (!is_self && self_size != 0 && self_size < sizeof(Elf32_Ehdr))
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    // Input can be byte-aligned. Do not bind typed references into it.
    SCE_header self_header{};
    if (is_self) {
        std::memcpy(&self_header, self, sizeof(self_header));
        if (self_size != 0) {
            if (self_header.self_filesize < sizeof(SCE_header)
                || self_header.self_filesize > self_size
                || self_header.header_len < sizeof(SCE_header)
                || self_header.header_len > self_header.self_filesize)
                return SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER;
            image_size = static_cast<std::size_t>(self_header.self_filesize);
        }
    }

    if (is_self) {
        // assumes little endian host
        if (self_header.version != 3) {
            LOG_CRITICAL("SELF {} version {} is not supported.", self_path, self_header.version);
            return -1;
        }

        if (self_header.header_type != 1) {
            LOG_CRITICAL("SELF {} header type {} is not supported.", self_path, self_header.header_type);
            return -1;
        }

        if (self_path == "app0:sce_module/steroid.suprx") {
            LOG_CRITICAL("You're trying to load a vitamin dump. It is not supported.");
            return -1;
        }
    }

    // elf_filesize describes the original ELF, NOT bytes embedded in the SELF.
    // Only its ELF header is read here; each segment has its own stored length.
    if (!has_bytes(is_self ? self_header.elf_offset : 0, sizeof(Elf32_Ehdr)))
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    const uint8_t *const elf_bytes = is_self ? (image_bytes + self_header.elf_offset) : image_bytes;
    Elf32_Ehdr elf;
    std::memcpy(&elf, elf_bytes, sizeof(elf));
    const uint32_t module_info_offset = elf.e_entry & 0x3fffffff;

    // Verify ELF header is correct
    if (!EHDR_HAS_VALID_MAGIC(elf)) {
        LOG_CRITICAL("Cannot load file {}: invalid ELF magic.", self_path);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    if (elf.e_ident[EI_CLASS] != ELFCLASS32) {
        LOG_CRITICAL("Cannot load ELF {}: unexpected EI_CLASS {}.", self_path, elf.e_ident[EI_CLASS]);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    if (elf.e_ident[EI_DATA] != ELFDATA2LSB) {
        LOG_CRITICAL("Cannot load ELF {}: unexpected EI_DATA {}.", self_path, elf.e_ident[EI_DATA]);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    if (elf.e_ident[EI_VERSION] != EV_CURRENT) {
        LOG_CRITICAL("Cannot load ELF {}: invalid EI_VERSION {}.", self_path, elf.e_ident[EI_VERSION]);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    if (elf.e_machine != EM_ARM) {
        LOG_CRITICAL("Cannot load ELF {}: unexpected e_machine {}.", self_path, elf.e_machine);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    // log elf header
    LOG_TRACE("ELF Header: e_type: {}, e_machine: {}, e_version: {}, e_entry: {}, e_phoff: {}, e_shoff: {}, e_flags: {}, e_ehsize: {}, e_phentsize: {}, e_phnum: {}, e_shentsize: {}, e_shnum: {}, e_shstrndx: {}",
        log_hex(elf.e_type), log_hex(elf.e_machine), log_hex(elf.e_version), log_hex(elf.e_entry), log_hex(elf.e_phoff), log_hex(elf.e_shoff), log_hex(elf.e_flags), log_hex(elf.e_ehsize), log_hex(elf.e_phentsize), log_hex(elf.e_phnum), log_hex(elf.e_shentsize), log_hex(elf.e_shnum), log_hex(elf.e_shstrndx));

    // The loader indexes native Elf32_Phdr records, so accepting a larger
    // advertised stride would validate a different table from the one consumed.
    if (self_size != 0 && (elf.e_ehsize != sizeof(Elf32_Ehdr)
                             || elf.e_version != EV_CURRENT || elf.e_phnum == 0
                             || elf.e_phentsize != sizeof(Elf32_Phdr)))
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    const auto phdr_offset = is_self ? self_header.phdr_offset : elf.e_phoff;
    const auto phdr_size = static_cast<std::uint64_t>(elf.e_phnum) * sizeof(Elf32_Phdr);
    if (!has_bytes(phdr_offset, phdr_size))
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    if (is_self && !has_bytes(self_header.section_info_offset,
                       static_cast<std::uint64_t>(elf.e_phnum) * sizeof(segment_info)))
        return SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER;

    std::vector<Elf32_Phdr> segments(elf.e_phnum);
    std::vector<segment_info> seg_infos(is_self ? elf.e_phnum : 0);
    if (phdr_size)
        std::memcpy(segments.data(), image_bytes + phdr_offset, phdr_size);
    if (!seg_infos.empty())
        std::memcpy(seg_infos.data(), image_bytes + self_header.section_info_offset, seg_infos.size() * sizeof(segment_info));

    // Validate every file span before constructing segment pointers or allocating
    // guest memory. A compressed segment consumes length bytes, produces filesz.
    if (self_size != 0) {
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const auto &seg = segments[i];
            if (is_self) {
                const auto &info = seg_infos[i];
                if (!has_bytes(info.offset, info.length)
                    || (info.compression != 1 && info.compression != 2)
                    || info.encryption != 2
                    || (info.compression == 1 && info.length < seg.p_filesz)
                    || info.length > std::numeric_limits<mz_ulong>::max())
                    return SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER;
            } else if (!has_bytes(seg.p_offset, seg.p_filesz)) {
                return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
            }
            if (seg.p_type == PT_LOAD
                && (seg.p_filesz > seg.p_memsz
                    || i >= MODULE_INFO_NUM_SEGMENTS
                    || (seg.p_vaddr & 3)
                    || seg.p_memsz > UINT32_MAX - 0xfff
                    || !vita3k::memory::guest_range_fits(seg.p_vaddr, seg.p_memsz)))
                return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
        }
        const auto index = elf.e_entry >> 30;
        if (index >= segments.size() || segments[index].p_type != PT_LOAD
            || (module_info_offset & 3)
            || module_info_offset > segments[index].p_filesz
            || sizeof(sce_module_info_raw) > segments[index].p_filesz - module_info_offset)
            return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    bool isRelocatable;
    if (elf.e_type == ET_SCE_EXEC) {
        isRelocatable = false;
    } else if (elf.e_type == ET_SCE_RELEXEC) {
        isRelocatable = true;
    } else if (elf.e_type == ET_SCE_PSP2RELEXEC) {
        LOG_CRITICAL("Cannot load ELF {}: ET_SCE_PSP2RELEXEC is not supported.", self_path);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    } else {
        LOG_CRITICAL("Cannot load ELF {}: unexpected e_type {}.", self_path, elf.e_type);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    // TODO: is OSABI always 0?
    // TODO: is ABI_VERSION always 0?

    if (is_self) {
        LOG_DEBUG_IF(LOG_MODULE_LOADING, "Loading SELF at {}... (ELF type: {}, self_filesize: {}, self_offset: {}, module_info_offset: {})", self_path, log_hex(elf.e_type), log_hex(self_header.self_filesize), log_hex(self_header.self_offset), log_hex(module_info_offset));
    } else {
        LOG_DEBUG_IF(LOG_MODULE_LOADING, "Loading ELF at {}... (ELF type: {}, module_info_offset: {})", self_path, log_hex(elf.e_type), log_hex(module_info_offset));
    }

    auto get_seg_header_string = [](uint32_t p_type) {
        if (p_type == PT_NULL) {
            return "NULL";
        } else if (p_type == PT_LOAD) {
            return "LOAD";
        } else if (p_type == PT_SCE_COMMENT) {
            return "SCE Comment";
        } else if (p_type == PT_SCE_VERSION) {
            return "SCE Version";
        } else if ((PT_LOOS <= p_type) && (p_type <= PT_HIOS)) {
            return "OS-specific";
        } else if ((PT_LOPROC <= p_type) && (p_type <= PT_HIPROC)) {
            return "Processor-specific";
        } else {
            return "Unknown";
        }
    };

    SegmentInfosForReloc segment_reloc_info;
    std::vector<Address> absolute_relocations;

    auto free_all_segments = [](MemState &mem, SegmentInfosForReloc &segs_info) {
        for (auto &[_, segment] : segs_info) {
            free(mem, segment.addr);
        }
    };

    for (Elf_Half seg_index = 0; seg_index < elf.e_phnum; ++seg_index) {
        const Elf32_Phdr &seg_header = segments[seg_index];
        // A SELF says where each segment is in the segment info, and the
        // program header's p_offset describes the ELF the SELF was made from
        // rather than the SELF itself. The two agree only when the whole ELF
        // was embedded at header_len, which is what fake SELFs used to do and
        // no real one does.
        const uint8_t *const seg_bytes = is_self
            ? (image_bytes + seg_infos[seg_index].offset)
            : (elf_bytes + seg_header.p_offset);

        const auto uncompress_segment = [&](void *dst) {
            mz_ulong dest_bytes = seg_header.p_filesz;
            const int res = mz_uncompress(static_cast<unsigned char *>(dst), &dest_bytes,
                seg_bytes, static_cast<mz_ulong>(seg_infos[seg_index].length));
            return res == MZ_OK && dest_bytes == seg_header.p_filesz;
        };

        LOG_DEBUG_IF(LOG_MODULE_LOADING, "    [{}] (p_type: {}): p_offset: {}, p_vaddr: {}, p_paddr: {}, p_filesz: {}, p_memsz: {}, p_flags: {}, p_align: {}", get_seg_header_string(seg_header.p_type), log_hex(seg_header.p_type), log_hex(seg_header.p_offset), log_hex(seg_header.p_vaddr), log_hex(seg_header.p_paddr), log_hex(seg_header.p_filesz), log_hex(seg_header.p_memsz), log_hex(seg_header.p_flags), log_hex(seg_header.p_align));

        if (is_self && seg_infos[seg_index].encryption != 2) { // 0 should also be valid?
            LOG_ERROR("Cannot load ELF {}: invalid segment encryption status {}.", self_path, seg_infos[seg_index].encryption);
            free_all_segments(mem, segment_reloc_info);
            return -1;
        }

        if (seg_header.p_type == PT_NULL) {
            // Nothing to do.
        } else if (seg_header.p_type == PT_LOAD) {
            if (seg_header.p_memsz != 0) {
                Address segment_address = 0;
                auto alloc_name = fmt::format("{}:seg{}", self_path, seg_index);

                segment_address = try_alloc_at(mem, seg_header.p_vaddr, seg_header.p_memsz, alloc_name.c_str());

                if (!segment_address) {
                    if (isRelocatable) { // Try allocating somewhere else
                        segment_address = alloc(mem, seg_header.p_memsz, alloc_name.c_str());
                    }

                    if (!segment_address) {
                        LOG_CRITICAL("Loading {} ELF {} failed: Could not allocate {} bytes @ {} for segment {}.", (isRelocatable) ? "relocatable" : "fixed", self_path, log_hex(seg_header.p_memsz), log_hex(seg_header.p_vaddr), seg_index);
                        free_all_segments(mem, segment_reloc_info);
                        return SCE_KERNEL_ERROR_NO_MEMORY; // TODO is this correct?
                    }
                }

                segment_reloc_info[seg_index] = { segment_address, seg_header.p_vaddr, seg_header.p_memsz };
                const Ptr<uint8_t> seg_ptr(segment_address);
                if (is_self && seg_infos[seg_index].compression == 2) {
                    if (!uncompress_segment(seg_ptr.get(mem))) {
                        free_all_segments(mem, segment_reloc_info);
                        return SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER;
                    }
                } else {
                    memcpy(seg_ptr.get(mem), seg_bytes, seg_header.p_filesz);
                }
            }
        } else if (seg_header.p_type == PT_SCE_RELA) {
            const void *reloc_data = seg_bytes;
            std::unique_ptr<uint8_t[]> uncompressed;

            if (is_self && seg_infos[seg_index].compression == 2) {
                uncompressed = std::make_unique<uint8_t[]>(seg_header.p_filesz);
                if (!uncompress_segment(uncompressed.get())) {
                    free_all_segments(mem, segment_reloc_info);
                    return SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER;
                }
                reloc_data = uncompressed.get();
            }

            if (self_size != 0 && !has_complete_relocation_records(static_cast<const uint8_t *>(reloc_data), seg_header.p_filesz)) {
                free_all_segments(mem, segment_reloc_info);
                return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
            }
            // The relocation engine uses word-aligned records; byte transports
            // and SELF segment offsets need not have host alignment.
            std::vector<uint32_t> aligned_relocations;
            if (reinterpret_cast<uintptr_t>(reloc_data) % alignof(uint32_t)) {
                aligned_relocations.resize((static_cast<size_t>(seg_header.p_filesz) + 3) / 4);
                std::memcpy(aligned_relocations.data(), reloc_data, seg_header.p_filesz);
                reloc_data = aligned_relocations.data();
            }
            if (!relocate(reloc_data, seg_header.p_filesz, segment_reloc_info, mem, false, 0, &absolute_relocations)) {
                free_all_segments(mem, segment_reloc_info);
                return -1;
            }
        } else if ((seg_header.p_type == PT_SCE_COMMENT) || (seg_header.p_type == PT_SCE_VERSION)
            || (seg_header.p_type == PT_ARM_EXIDX) /* TODO: this may be important and require being loaded */) {
            LOG_INFO("{}: Skipping special segment {}...", self_path, log_hex(seg_header.p_type));
        } else {
            LOG_CRITICAL("{}: Skipping segment with unknown p_type {}!", self_path, log_hex(seg_header.p_type));
        }
    }

    if (kernel.debugger.dump_elfs && !segment_reloc_info.empty()) {
        // SELF payloads are independently stored (and may be compressed), not
        // an ELF beginning at header_len. Reconstruct only the loaded image from
        // the already checked headers instead of interpreting payload as headers.
        uint64_t dump_size = std::max<uint64_t>(sizeof(elf), uint64_t(elf.e_phoff) + phdr_size);
        for (const auto &[index, _] : segment_reloc_info)
            dump_size = std::max(dump_size, uint64_t(segments[index].p_offset) + segments[index].p_filesz);
        const uint64_t original_size = is_self ? self_header.elf_filesize : image_size;
        if (dump_size > std::numeric_limits<size_t>::max()
            || (self_size != 0 && (dump_size > original_size || elf.e_phoff < sizeof(elf)))) {
            LOG_WARN("Not dumping {}: invalid original ELF layout", self_path);
        } else {
            std::vector<uint8_t> dump_elf(static_cast<size_t>(dump_size));
            std::memcpy(dump_elf.data(), &elf, sizeof(elf));
            auto dump_segments = segments;
            for (const auto &[index, segment] : segment_reloc_info) {
                std::memcpy(dump_elf.data() + segments[index].p_offset,
                    Ptr<const uint8_t>(segment.addr).get(mem), segments[index].p_filesz);
                dump_segments[index].p_vaddr = segment.addr;
            }
            std::memcpy(dump_elf.data() + elf.e_phoff, dump_segments.data(), phdr_size);
            fs::create_directories(dump_path);
            const auto first = segment_reloc_info.begin()->first;
            const auto last = segment_reloc_info.rbegin()->first;
            const auto start = dump_segments[first].p_vaddr;
            const auto end = uint64_t(dump_segments[last].p_vaddr) + dump_segments[last].p_filesz;
            const auto elf_name = fs::path(self_path).filename().stem().string();
            const auto filename = dump_path / fmt::format("{}-{}_{}.elf", log_hex_full(start), log_hex_full(end), elf_name);
            fs_utils::dump_data(filename, dump_elf.data(), dump_elf.size());
        }
    }

    const unsigned int module_info_segment_index = elf.e_entry >> 30;
    const auto module_segment = segment_reloc_info.find(module_info_segment_index);
    if (module_segment == segment_reloc_info.end()) {
        free_all_segments(mem, segment_reloc_info);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }
    const Ptr<const uint8_t> module_info_segment_address(module_segment->second.addr);
    const uint8_t *const module_info_segment_bytes = module_info_segment_address.get(mem);
    const sce_module_info_raw *const module_info = reinterpret_cast<const sce_module_info_raw *>(module_info_segment_bytes + module_info_offset);
    if (self_size != 0 && !validate_module_tables(*module_info, module_segment->second.addr, module_segment->second.size, segment_reloc_info, mem)) {
        free_all_segments(mem, segment_reloc_info);
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    }

    for (const auto &[seg, infos] : segment_reloc_info) {
        LOG_INFO("Loaded module segment {} @ [0x{:08X} - 0x{:08X} / 0x{:08X}] (size: 0x{:08X}) of module {}", seg, infos.addr, uint64_t(infos.addr) + infos.size, infos.p_vaddr, infos.size, self_path);
    }

    const SceKernelModulePtr kernelModuleInfo = std::make_shared<KernelModule>();

    kernelModuleInfo->absolute_relocations = std::move(absolute_relocations);
    kernelModuleInfo->info_segment_address = module_info_segment_address;
    kernelModuleInfo->info_offset = module_info_offset;
    if (is_self && has_bytes(self_header.appinfo_offset, sizeof(SCE_appinfo))) {
        SCE_appinfo appinfo;
        std::memcpy(&appinfo, image_bytes + self_header.appinfo_offset, sizeof(appinfo));
        kernelModuleInfo->program_authority_id = appinfo.authid;
    }

    auto *sceKernelModuleInfo = &kernelModuleInfo->info;
    sceKernelModuleInfo->size = sizeof(*sceKernelModuleInfo);
    std::memcpy(sceKernelModuleInfo->module_name, module_info->name, sizeof(module_info->name));
    sceKernelModuleInfo->module_name[sizeof(module_info->name)] = '\0';
    // unk28
    if (module_info->module_start != 0xffffffff && module_info->module_start != 0)
        sceKernelModuleInfo->start_entry = module_info_segment_address + module_info->module_start;
    // unk30
    if (module_info->module_stop != 0xffffffff && module_info->module_stop != 0)
        sceKernelModuleInfo->stop_entry = module_info_segment_address + module_info->module_stop;

    sceKernelModuleInfo->exidx_top = Ptr<const void>(module_info->exidx_top);
    sceKernelModuleInfo->exidx_btm = Ptr<const void>(module_info->exidx_end);
    sceKernelModuleInfo->extab_top = Ptr<const void>(module_info->extab_top);
    sceKernelModuleInfo->extab_btm = Ptr<const void>(module_info->extab_end);

    sceKernelModuleInfo->tlsInit = Ptr<const void>(!module_info->tls_start ? 0 : (module_info_segment_address.address() + module_info->tls_start));
    sceKernelModuleInfo->tlsInitSize = module_info->tls_filesz;
    sceKernelModuleInfo->tlsAreaSize = module_info->tls_memsz;

    if (sceKernelModuleInfo->tlsInit) {
        kernel.tls_address = sceKernelModuleInfo->tlsInit;
        kernel.tls_psize = sceKernelModuleInfo->tlsInitSize;
        kernel.tls_msize = sceKernelModuleInfo->tlsAreaSize;
    }

    strncpy(sceKernelModuleInfo->path, self_path.c_str(), 255);

    for (Elf_Half segment_index = 0; segment_index < elf.e_phnum; ++segment_index) {
        // Skip non-loadable segments
        auto it = segment_reloc_info.find(segment_index);
        if (it == segment_reloc_info.end())
            continue;

        if (segment_index >= MODULE_INFO_NUM_SEGMENTS) {
            LOG_ERROR("Segment {} should not be loadable", segment_index);
            continue;
        }

        SceKernelSegmentInfo &segment = sceKernelModuleInfo->segments[segment_index];
        segment.size = sizeof(segment);
        segment.vaddr = it->second.addr;
        segment.memsz = segments[segment_index].p_memsz;
        segment.filesz = segments[segment_index].p_filesz;
    }

    sceKernelModuleInfo->state = module_info->type;

    LOG_INFO("Linking {} {}...", is_self ? "SELF" : "ELF", self_path);
    if (self_path.contains("eboot.bin"))
        LOG_INFO("eboot.bin module NID: {}", log_hex(module_info->module_nid));

    if (!load_exports(sceKernelModuleInfo, *module_info, module_info_segment_address, kernel, mem)) {
        return -1;
    }

    if (!load_imports(*module_info, module_info_segment_address, segment_reloc_info, kernel, mem)) {
        return -1;
    }
    const SceUID uid = kernel.get_next_uid();
    sceKernelModuleInfo->modid = uid;
    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);
        kernel.loaded_modules[uid] = kernelModuleInfo;
    }
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        kernel.module_uid_by_nid[module_info->module_nid] = uid;
    }

    return uid;
}

SceUID load_self(KernelState &kernel, MemState &mem, const void *self, const std::string &self_path, const fs::path &dump_path) {
    return load_self_impl(kernel, mem, self, 0, self_path, dump_path);
}

SceUID load_self_sized(KernelState &kernel, MemState &mem, const void *self, std::size_t self_size, const std::string &self_path, const fs::path &dump_path) {
    if (!self || self_size == 0)
        return SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER;
    return load_self_impl(kernel, mem, self, self_size, self_path, dump_path);
}

int unload_self(KernelState &kernel, MemState &mem, KernelModule &module) {
    LOG_INFO("Unlinking self...");
    const sce_module_info_raw *const module_info = reinterpret_cast<const sce_module_info_raw *>(module.info_segment_address.get(mem) + module.info_offset);
    if (!load_exports(&module.info, *module_info, module.info_segment_address, kernel, mem, true)) {
        return -1;
    }

    SegmentInfosForReloc segment_reloc_info;
    for (int i = 0; i < MODULE_INFO_NUM_SEGMENTS; i++) {
        const auto &segment = module.info.segments[i];
        if (segment.size == 0)
            continue;

        segment_reloc_info[i] = { segment.vaddr.address(), 0, segment.memsz }; // p_vaddr is not used in variable relocations
    }

    if (!load_imports(*module_info, module.info_segment_address, segment_reloc_info, kernel, mem, true)) {
        return -1;
    }

    SceUID mod_nid = module_info->module_nid;

    // last step: free the memory
    for (int i = 0; i < MODULE_INFO_NUM_SEGMENTS; i++) {
        const auto &segment = module.info.segments[i];
        if (segment.size == 0)
            continue;

        kernel.invalidate_jit_cache(segment.vaddr.address(), segment.memsz);
        free(mem, module.info.segments[i].vaddr.address());
    }

    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);
        kernel.loaded_modules.erase(module.info.modid);
    }
    {
        const std::lock_guard<std::mutex> guard(kernel.export_nids_mutex);
        kernel.module_uid_by_nid.erase(mod_nid);
    }

    return 0;
}
