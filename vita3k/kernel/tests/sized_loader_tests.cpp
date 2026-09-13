// Tests exercise the production loader, relocation engine, linker and native
// memory allocator. No mock parser or alternate loader implementation.
#include <kernel/load_self.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <util/elf.h>
#include <util/log.h>
#define SCE_ELF_DEFS_TARGET
#include <sce-elf-defs.h>
#undef SCE_ELF_DEFS_TARGET
#include <self.h>
#include <miniz.h>

#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using Bytes = std::vector<uint8_t>;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

static unsigned checks = 0;
static constexpr Address BASE = 0x81000000;
static constexpr size_t PAYLOAD = 0x100;
static constexpr size_t MODULE = 0x20;
static constexpr uint32_t TEST_NID = 0x12345678;

template <typename T> static T get(const Bytes &bytes, size_t offset = 0) {
    CHECK(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset);
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
template <typename T> static void put(Bytes &bytes, size_t offset, const T &value) {
    CHECK(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset);
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
template <typename T, typename F> static void change(Bytes &bytes, size_t offset, F fn) {
    auto value = get<T>(bytes, offset);
    fn(value);
    put(bytes, offset, value);
}
static Bytes synthetic_elf() {
    Bytes bytes(PAYLOAD + 0x400);
    Elf32_Ehdr elf{};
    elf.e_ident[0] = 0x7f; elf.e_ident[1] = 'E'; elf.e_ident[2] = 'L'; elf.e_ident[3] = 'F';
    elf.e_ident[EI_CLASS] = ELFCLASS32;
    elf.e_ident[EI_DATA] = ELFDATA2LSB;
    elf.e_ident[EI_VERSION] = EV_CURRENT;
    elf.e_type = ET_SCE_RELEXEC;
    elf.e_machine = EM_ARM;
    elf.e_version = EV_CURRENT;
    elf.e_entry = MODULE;
    elf.e_phoff = sizeof(elf);
    elf.e_ehsize = sizeof(elf);
    elf.e_phentsize = sizeof(Elf32_Phdr);
    elf.e_phnum = 1;
    put(bytes, 0, elf);
    Elf32_Phdr ph{};
    ph.p_type = PT_LOAD;
    ph.p_offset = PAYLOAD;
    ph.p_vaddr = BASE;
    ph.p_filesz = 0x400;
    ph.p_memsz = 0x800;
    ph.p_flags = 5; // PF_R | PF_X
    ph.p_align = 0x1000;
    put(bytes, elf.e_phoff, ph);
    sce_module_info_raw module{};
    std::memcpy(module.name, "sized-test", 11);
    module.module_nid = 0x1234;
    module.module_start = 0x201;
    module.module_stop = UINT32_MAX;
    put(bytes, PAYLOAD + MODULE, module);
    put<uint32_t>(bytes, PAYLOAD + 0x200, 0xe12fff1e); // bx lr (never executed)
    return bytes;
}
static Bytes synthetic_import(bool short_record = false) {
    auto bytes = synthetic_elf();
    change<sce_module_info_raw>(bytes, PAYLOAD + MODULE, [&](auto &m) {
        m.import_top = 0x100;
        m.import_end = 0x100 + (short_record ? sizeof(sce_module_imports_short_raw) : sizeof(sce_module_imports_raw));
    });
    sce_module_imports_raw imports{};
    imports.size = sizeof(imports);
    imports.num_syms_funcs = 1;
    imports.library_nid = 0x42;
    imports.library_name = BASE + 0x240;
    imports.func_nid_table = BASE + 0x180;
    imports.func_entry_table = BASE + 0x190;
    if (short_record) {
        sce_module_imports_short_raw s{};
        s.size = sizeof(s);
        s.num_syms_funcs = imports.num_syms_funcs;
        s.library_nid = imports.library_nid;
        s.library_name = imports.library_name;
        s.func_nid_table = imports.func_nid_table;
        s.func_entry_table = imports.func_entry_table;
        put(bytes, PAYLOAD + 0x100, s);
    } else {
        put(bytes, PAYLOAD + 0x100, imports);
    }
    put(bytes, PAYLOAD + 0x180, TEST_NID);
    put<uint32_t>(bytes, PAYLOAD + 0x190, BASE + 0x200);
    return bytes;
}
static Bytes synthetic_export() {
    auto bytes = synthetic_elf();
    change<sce_module_info_raw>(bytes, PAYLOAD + MODULE, [](auto &m) { m.export_top = 0x100; m.export_end = 0x120; });
    sce_module_exports_raw exports{};
    exports.size = sizeof(exports);
    exports.num_syms_funcs = 1;
    exports.nid_table = BASE + 0x180;
    exports.entry_table = BASE + 0x190;
    put(bytes, PAYLOAD + 0x100, exports);
    put(bytes, PAYLOAD + 0x180, TEST_NID);
    put<uint32_t>(bytes, PAYLOAD + 0x190, BASE + 0x201);
    return bytes;
}
static Bytes synthetic_self(bool compressed) {
    const auto elf = synthetic_elf();
    Bytes payload(elf.begin() + PAYLOAD, elf.end());
    if (compressed) {
        Bytes zipped(mz_compressBound(payload.size()));
        mz_ulong length = zipped.size();
        CHECK(mz_compress2(zipped.data(), &length, payload.data(), payload.size(), MZ_BEST_COMPRESSION) == MZ_OK);
        zipped.resize(length);
        payload = std::move(zipped);
    }
    SCE_header header{};
    header.magic = 0x00454353;
    header.version = 3;
    header.header_type = 1;
    header.header_len = 0x100;
    header.elf_filesize = elf.size();
    header.self_filesize = header.header_len + payload.size();
    header.elf_offset = sizeof(header);
    header.phdr_offset = header.elf_offset + sizeof(Elf32_Ehdr);
    header.section_info_offset = header.phdr_offset + sizeof(Elf32_Phdr);
    Bytes bytes(header.self_filesize);
    put(bytes, 0, header);
    put(bytes, header.elf_offset, get<Elf32_Ehdr>(elf));
    put(bytes, header.phdr_offset, get<Elf32_Phdr>(elf, sizeof(Elf32_Ehdr)));
    segment_info info{header.header_len, payload.size(), compressed ? 2u : 1u, 2};
    put(bytes, header.section_info_offset, info);
    std::memcpy(bytes.data() + header.header_len, payload.data(), payload.size());
    return bytes;
}
static void reject(MemState &mem, const Bytes &bytes, const char *name, SceUID expected = SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER) {
    KernelState kernel;
    const auto allocated = mem.page_name_map.size();
    const auto result = load_self_sized(kernel, mem, bytes.data(), bytes.size(), name, {});
    if (result != expected)
        throw std::runtime_error(std::string(name) + ": expected " + std::to_string(expected) + ", got " + std::to_string(result));
    CHECK(kernel.loaded_modules.empty());
    CHECK(kernel.export_nids.empty() && kernel.func_binding_infos.empty());
    CHECK(mem.page_name_map.size() == allocated);
    ++checks;
}
static void accept(MemState &mem, const Bytes &bytes, const char *name, bool legacy = false, bool unaligned = false, bool dump = false) {
    KernelState kernel;
    kernel.debugger.dump_elfs = dump;
    kernel.debugger.log_imports = true;
    kernel.debugger.log_exports = true;
    Bytes storage(bytes.size() + 1);
    std::memcpy(storage.data() + 1, bytes.data(), bytes.size());
    const void *data = unaligned ? storage.data() + 1 : bytes.data();
    const auto dump_dir = fs::temp_directory_path() / "vita3k-sized-loader-tests";
    if (dump) fs::remove_all(dump_dir);
    const auto uid = legacy ? load_self(kernel, mem, data, name, dump_dir)
                            : load_self_sized(kernel, mem, data, bytes.size(), name, dump_dir);
    if (uid < 0) throw std::runtime_error(std::string(name) + ": load failed " + std::to_string(uid));
    CHECK(kernel.loaded_modules.size() == 1);
    auto module = kernel.loaded_modules.at(uid);
    CHECK(module->info.start_entry.address() != 0);
    CHECK(module->info.segments[0].vaddr.address() == BASE);
    CHECK(module->info.module_name[27] == '\0');
    if (std::string(name).starts_with("synthetic")) {
        CHECK(module->info.start_entry.address() == BASE + 0x201);
        CHECK(module->info.segments[0].memsz == 0x800);
        CHECK(module->info.segments[0].filesz == 0x400);
        CHECK(*Ptr<uint32_t>(BASE + 0x400).get(mem) == 0); // BSS
    } else {
        CHECK(module->info.segments[1].memsz == 0x237e0);
        CHECK(module->info.segments[1].filesz == 0x638);
        CHECK(!kernel.func_binding_infos.empty()); // Real VitaSDK imports linked.
    }
    if (!kernel.func_binding_infos.empty()) {
        const auto address = kernel.func_binding_infos.begin()->second.entry_address;
        CHECK(Ptr<const uint32_t>(address).get(mem)[0] == 0xef000000);
        CHECK(Ptr<const uint32_t>(address).get(mem)[2] == kernel.func_binding_infos.begin()->first);
    }
    if (dump) {
        CHECK(fs::exists(dump_dir));
        unsigned files = 0;
        for (const auto &entry : fs::directory_iterator(dump_dir)) {
            std::ifstream stream(entry.path(), std::ios::binary);
            Elf32_Ehdr header{};
            stream.read(reinterpret_cast<char *>(&header), sizeof(header));
            CHECK(stream.good() && EHDR_HAS_VALID_MAGIC(header));
            ++files;
        }
        CHECK(files == 1);
        fs::remove_all(dump_dir);
    }
    CHECK(unload_self(kernel, mem, *module) == 0);
    CHECK(kernel.loaded_modules.empty());
    ++checks;
}
static void bounds_tests(MemState &mem) {
    const auto elf = synthetic_elf();
    KernelState kernel;
    CHECK(load_self_sized(kernel, mem, nullptr, elf.size(), "null", {}) == SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER);
    CHECK(load_self_sized(kernel, mem, elf.data(), 0, "zero", {}) == SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER);
    checks += 2;
    // Exact-sized allocations make ASan detect reads beyond the supplied span.
    for (size_t length = 0; length < elf.size(); ++length)
        reject(mem, Bytes(elf.begin(), elf.begin() + length), "ELF truncated prefix");
    auto mutate_elf = [&](auto fn, const char *name) { auto b = elf; change<Elf32_Ehdr>(b, 0, fn); reject(mem, b, name); };
    mutate_elf([](auto &h) { h.e_phoff = UINT32_MAX; }, "phoff overflow");
    mutate_elf([](auto &h) { h.e_phnum = UINT16_MAX; }, "phnum overflow");
    mutate_elf([](auto &h) { h.e_phnum = 0; }, "no segments");
    mutate_elf([](auto &h) { --h.e_phentsize; }, "small stride");
    mutate_elf([](auto &h) { h.e_phentsize += 4; }, "large stride");
    mutate_elf([](auto &h) { --h.e_ehsize; }, "ehsize");
    mutate_elf([](auto &h) { h.e_version = 0; }, "version");
    mutate_elf([](auto &h) { h.e_entry = 0xc0000000; }, "missing module segment");
    mutate_elf([](auto &h) { h.e_entry = 0x400 - sizeof(sce_module_info_raw) + 4; }, "partial module info");
    mutate_elf([](auto &h) { h.e_entry = 0x400; }, "module in BSS");
    mutate_elf([](auto &h) { h.e_entry |= 1; }, "unaligned module info");
    auto mutate_ph = [&](auto fn, const char *name) { auto b = elf; change<Elf32_Phdr>(b, sizeof(Elf32_Ehdr), fn); reject(mem, b, name); };
    mutate_ph([](auto &p) { p.p_offset = UINT32_MAX; }, "segment offset");
    mutate_ph([](auto &p) { p.p_memsz = p.p_filesz - 1; }, "filesz > memsz");
    mutate_ph([](auto &p) { p.p_memsz = 0; }, "filesz with zero memsz");
    mutate_ph([](auto &p) { p.p_memsz = UINT32_MAX; }, "allocation wrap");
    mutate_ph([](auto &p) { p.p_vaddr = 0xffffff00; }, "address wrap");
    mutate_ph([](auto &p) { p.p_type = PT_NULL; }, "non-load module segment");
    auto mutate_module = [&](auto fn, const char *name) { auto b = elf; change<sce_module_info_raw>(b, PAYLOAD + MODULE, fn); reject(mem, b, name); };
    mutate_module([](auto &m) { m.import_top = 4; }, "reversed imports");
    mutate_module([](auto &m) { m.export_end = 0x804; }, "export end outside segment");
    mutate_module([](auto &m) { m.import_top = m.import_end = UINT32_MAX; }, "empty out-of-bounds imports");
    mutate_module([](auto &m) { m.module_start = 0x801; }, "start outside segment");
    mutate_module([](auto &m) { m.module_stop = 0x800; }, "stop outside segment");
    mutate_module([](auto &m) { m.tls_start = 0x800; m.tls_filesz = m.tls_memsz = 4; }, "TLS extent");
    mutate_module([](auto &m) { m.tls_start = 0x100; m.tls_filesz = 8; m.tls_memsz = 4; }, "TLS filesz > memsz");
    mutate_module([](auto &m) { m.exidx_end = 0x804; }, "exception table extent");

    for (bool compressed : {false, true}) {
        const auto self = synthetic_self(compressed);
        for (size_t length = 4; length < self.size(); ++length)
            reject(mem, Bytes(self.begin(), self.begin() + length), "SELF truncated prefix", SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER);
        const auto h = get<SCE_header>(self);
        CHECK(h.elf_offset + h.elf_filesize > self.size());
        if (compressed)
            CHECK(h.header_len + get<Elf32_Phdr>(self, h.phdr_offset).p_filesz > self.size());
        auto mutate_header = [&](auto fn, const char *name, SceUID error) { auto b = self; change<SCE_header>(b, 0, fn); reject(mem, b, name, error); };
        mutate_header([](auto &s) { s.elf_offset = UINT64_MAX; }, "SELF ELF offset", SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER);
        mutate_header([](auto &s) { s.phdr_offset = UINT64_MAX; }, "SELF phdr offset", SCE_KERNEL_ERROR_ILLEGAL_ELF_HEADER);
        mutate_header([](auto &s) { s.section_info_offset = UINT64_MAX; }, "SELF sinfo offset", SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER);
        mutate_header([](auto &s) { s.header_len = UINT64_MAX; }, "SELF header length", SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER);
        mutate_header([](auto &s) { s.self_filesize = 4; }, "SELF declared size", SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER);
        auto mutate_info = [&](auto fn, const char *name) { auto b = self; change<segment_info>(b, h.section_info_offset, fn); reject(mem, b, name, SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER); };
        mutate_info([](auto &s) { s.offset = UINT64_MAX; }, "stored offset overflow");
        mutate_info([](auto &s) { s.length = UINT64_MAX; }, "stored length overflow");
        mutate_info([](auto &s) { s.compression = 3; }, "unknown compression");
        mutate_info([](auto &s) { s.encryption = 1; }, "encrypted segment");
        mutate_info([](auto &s) { --s.length; }, "short stored data");
        if (compressed) {
            auto corrupt = self;
            corrupt[h.header_len] = 0;
            reject(mem, corrupt, "invalid zlib stream", SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER);
            for (int delta : {-4, 4}) {
                auto b = self;
                change<Elf32_Phdr>(b, h.phdr_offset, [&](auto &p) { p.p_filesz += delta; });
                reject(mem, b, "wrong decompressed size", SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER);
            }
        }
        accept(mem, self, "synthetic SELF", false, true, true);
    }

    for (bool short_record : {false, true}) {
        const auto imports = synthetic_import(short_record);
        for (uint16_t length : {0, 4, 0x28, 0xffff}) {
            auto b = imports; put(b, PAYLOAD + 0x100, length); reject(mem, b, "bad import stride");
        }
        auto b = imports;
        put<uint16_t>(b, PAYLOAD + 0x100 + 10, 1);
        reject(mem, b, "unsupported TLS import");
        b = imports;
        put<uint32_t>(b, PAYLOAD + 0x190, BASE + 0x7f4);
        reject(mem, b, "truncated function stub");
        b = imports;
        put<uint32_t>(b, PAYLOAD + 0x20c, BASE + 0x7fc);
        put<uint32_t>(b, PAYLOAD + 0x3fc, 0); // reftable in zeroed BSS has size zero
        reject(mem, b, "invalid stub reftable");
        accept(mem, imports, "synthetic imports");
    }
    auto imp = synthetic_import();
    change<sce_module_imports_raw>(imp, PAYLOAD + 0x100, [](auto &i) { i.func_nid_table = UINT32_MAX; });
    reject(mem, imp, "invalid NID table");
    imp = synthetic_import();
    change<sce_module_imports_raw>(imp, PAYLOAD + 0x100, [](auto &i) { i.library_name = UINT32_MAX; });
    reject(mem, imp, "invalid library string");
    imp = synthetic_import();
    change<sce_module_imports_raw>(imp, PAYLOAD + 0x100, [](auto &i) { i.library_name = 0; });
    reject(mem, imp, "null import library string");
    imp = synthetic_import();
    change<Elf32_Phdr>(imp, sizeof(Elf32_Ehdr), [](auto &p) { p.p_memsz = p.p_filesz; });
    change<sce_module_imports_raw>(imp, PAYLOAD + 0x100, [](auto &i) { i.library_name = BASE + 0x3fc; });
    put<uint32_t>(imp, PAYLOAD + 0x3fc, UINT32_MAX);
    reject(mem, imp, "unterminated import library string");
    for (uint32_t reloc_size : {0, 3, 0x1000}) {
        imp = synthetic_import();
        change<sce_module_imports_raw>(imp, PAYLOAD + 0x100, [](auto &i) {
            i.num_syms_funcs = 0; i.num_syms_vars = 1;
            i.var_nid_table = BASE + 0x180; i.var_entry_table = BASE + 0x190;
        });
        put<uint32_t>(imp, PAYLOAD + 0x200, reloc_size << 4);
        reject(mem, imp, "invalid variable reftable size");
    }
    const auto exports = synthetic_export();
    for (uint16_t length : {0, 4, 0xffff}) {
        auto b = exports; put(b, PAYLOAD + 0x100, length); reject(mem, b, "bad export stride");
    }
    auto exp = exports;
    change<sce_module_exports_raw>(exp, PAYLOAD + 0x100, [](auto &e) { e.num_syms_vars = UINT32_MAX; });
    reject(mem, exp, "export count overflow");
    exp = exports; put<uint32_t>(exp, PAYLOAD + 0x190, 0xfffffffc);
    reject(mem, exp, "export target");
    exp = exports;
    change<sce_module_exports_raw>(exp, PAYLOAD + 0x100, [](auto &e) { e.num_syms_funcs = 0; e.num_syms_vars = 1; });
    put<uint32_t>(exp, PAYLOAD + 0x180, 0x70fba1e7); // process param
    put<uint32_t>(exp, PAYLOAD + 0x190, BASE + 0x7f8);
    reject(mem, exp, "truncated process param version");
    put<uint32_t>(exp, PAYLOAD + 0x190, BASE + 0x3f4);
    change<Elf32_Phdr>(exp, sizeof(Elf32_Ehdr), [](auto &p) { p.p_memsz = p.p_filesz; });
    put<uint32_t>(exp, PAYLOAD + 0x3fc, 1);
    reject(mem, exp, "truncated nonzero-version process param");
    for (uint32_t length = 1; length < 12; ++length) {
        auto b = elf;
        Elf32_Phdr reloc{};
        reloc.p_type = PT_SCE_RELA;
        reloc.p_offset = b.size();
        reloc.p_filesz = length;
        b.resize(b.size() + length); // format 0 needs a complete 12-byte record
        change<Elf32_Ehdr>(b, 0, [](auto &h) { h.e_phnum = 2; });
        put(b, sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr), reloc);
        reject(mem, b, "partial relocation record");
    }
    accept(mem, exports, "synthetic exports");
    accept(mem, elf, "synthetic ELF", false, true, true);
    accept(mem, elf, "synthetic legacy ELF", true);
}
static Bytes read_file(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    return Bytes(std::istreambuf_iterator<char>(input), {});
}
int main(int argc, char **argv) {
    try {
        CHECK(argc == 4); // fixture.velf eboot.bin SDK-compressed eboot.bin
        spdlog::set_level(spdlog::level::off);
        MemState mem;
        CHECK(init(mem, false));
        bounds_tests(mem);
        for (int i = 1; i < argc; ++i) {
            const auto bytes = read_file(argv[i]);
            accept(mem, bytes, argv[i], false, true, true);
            accept(mem, bytes, argv[i], true);
            // Corrupt the actual SDK relocation payload, after load segments
            // have been allocated. Verify runtime failures also free them.
            if (get<uint32_t>(bytes) == 0x00454353) {
                const auto self = get<SCE_header>(bytes);
                const auto elf = get<Elf32_Ehdr>(bytes, self.elf_offset);
                for (unsigned j = 0; j < elf.e_phnum; ++j) {
                    const auto ph = get<Elf32_Phdr>(bytes, self.phdr_offset + j * sizeof(Elf32_Phdr));
                    const auto info = get<segment_info>(bytes, self.section_info_offset + j * sizeof(segment_info));
                    if (ph.p_type != PT_SCE_RELA || info.compression != 2) continue;
                    auto broken = bytes;
                    broken[info.offset] = 0; // invalid zlib header
                    reject(mem, broken, "corrupt compressed SDK relocations", SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER);
                    broken = bytes;
                    change<Elf32_Phdr>(broken, self.phdr_offset + j * sizeof(Elf32_Phdr), [](auto &p) { p.p_filesz += 4; });
                    reject(mem, broken, "short decompressed SDK relocations", SCE_KERNEL_ERROR_ILLEGAL_SELF_HEADER);
                }
            }
        }
        deinit_mem(mem);
        std::cout << "PASS: " << checks << " sized-loader checks (including 6 genuine-fixture loads)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
