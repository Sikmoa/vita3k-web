// Decrypts a NoNpDrm dump one file per call, for decrypt_worker.js.
//
// psvpfsparser's PfsFilesystem parses files.db/unicv.db, maps pages to
// files, then decrypts every file in one call. Here the same steps are split
// so the worker can open each output file in browser storage (an async API)
// between calls and the decrypted game never sits in memory:
//
//   vd_open(source, work.bin)  parse the dump; build the entry list
//   vd_count / vd_entry(i)     "kind\tsize\tpath": dir, empty, copy (the
//                              worker copies unencrypted files itself) or
//                              decrypt; path is the real file's, relative
//   vd_run(i, dest)            decrypt entry i to dest/path
//   vd_decrypt_self(in, out)   a decrypted SELF (eboot.bin, modules): 1, or 0
//                              when it needs none, with the dump's klicensee
//
// Sony's packages, for the same worker:
//
//   vd_pkg_open(pkg)           a .pkg (PSN download): its entry count
//   vd_pkg_info()              "type\ttitle id\tcontent id\tcategory"
//   vd_pkg_entry(i)            "dir|file\tsize\tname"
//   vd_pkg_extract(i, out)     entry i, its AES-CTR layer removed: the app
//                              folder of an encrypted dump (vd_open next)
//   vd_zrif_to_rif(zrif, out)  a zRIF string as the license work.bin holds
//   vd_install_pup(pup, out)   a firmware .PUP (system software, font package
//                              or pre-install) into out/os0, vs0, sa0, pd0;
//                              with its modules decrypted; vd_result() is
//                              its version
//
// Errors return < 0; vd_error() says why.
#include <CryptoOperationsFactory.h>
#include <F00DKeyEncryptorFactory.h>
#include <FilesDbParser.h>
#include <PfsFile.h>
#include <PfsPageMapper.h>
#include <UnicvDbParser.h>
#include <UnicvDbTypes.h>
#include <packages/sce_types.h>

#include <packages/functions.h>
#include <zrif2rif.h>

#include <emscripten/emscripten.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
enum class Kind { Dir, Empty, Copy, Decrypt };
const char *kind_name(Kind kind) {
    switch (kind) {
    case Kind::Dir: return "dir";
    case Kind::Empty: return "empty";
    case Kind::Copy: return "copy";
    default: return "decrypt";
    }
}
struct Entry {
    Kind kind;
    std::string path; // real path, relative to the source root, '/'-separated
    std::uint64_t size = 0;
    const sce_ng_pfs_file_t *file = nullptr;
    const sce_junction *junction = nullptr;
    std::shared_ptr<sce_iftbl_base_t> table;
};
struct Session {
    std::filesystem::path source;
    unsigned char klicensee[16] = {};
    std::ostringstream output; // psvpfsparser's progress text
    std::shared_ptr<ICryptoOperations> crypto;
    std::shared_ptr<IF00DKeyEncryptor> f00d;
    std::unique_ptr<FilesDbParser> files;
    std::unique_ptr<UnicvDbParser> unicv;
    std::unique_ptr<PfsPageMapper> pages;
    std::vector<Entry> entries;
};
std::unique_ptr<Session> session;
std::string error_text, entry_text;

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return text;
}
// Paths in files.db are joined to the source root and match real files
// case-insensitively.
std::string virtual_key(const std::filesystem::path &path, const std::filesystem::path &root) {
    std::string text = path.generic_string();
    const std::string prefix = root.generic_string() + "/";
    if (text.starts_with(prefix))
        text.erase(0, prefix.size());
    while (!text.empty() && text.front() == '/')
        text.erase(text.begin());
    return upper(text);
}
int fail(const std::string &message) {
    error_text = message;
    if (session)
        error_text += "\n" + session->output.str();
    return -1;
}
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE int vd_open(const char *source, const char *work_bin) {
    session.reset();
    error_text.clear();
    try {
        auto s = std::make_unique<Session>();
        s->source = source;
        std::ifstream license(work_bin, std::ios::binary);
        license.seekg(0x50);
        if (!license.read(reinterpret_cast<char *>(s->klicensee), 16))
            return fail("cannot read the license key from sce_sys/package/work.bin");
        s->crypto = CryptoOperationsFactory::create(CryptoOperationsTypes::openssl);
        s->f00d = F00DKeyEncryptorFactory::create(F00DEncryptorTypes::native, s->crypto);
        s->files = std::make_unique<FilesDbParser>(s->crypto, s->f00d, s->output, s->klicensee, s->source);
        s->unicv = std::make_unique<UnicvDbParser>(s->source, s->output);
        s->pages = std::make_unique<PfsPageMapper>(s->crypto, s->f00d, s->output, s->klicensee, s->source);
        session = std::move(s);
        Session &S = *session;
        if (S.files->parse() < 0)
            return fail("cannot parse sce_pfs/files.db (wrong license, or not a PFS image)");
        if (S.unicv->parse() < 0)
            return fail("cannot parse sce_pfs/unicv.db or icv.db");
        if (S.pages->bruteforce_map(S.files, S.unicv) < 0)
            return fail("cannot map the PFS pages to files");

        std::map<std::string, std::string> real; // virtual key -> real relative path
        for (const auto &item : std::filesystem::recursive_directory_iterator(S.source)) {
            const std::string relative = std::filesystem::relative(item.path(), S.source).generic_string();
            real.emplace(upper(relative), relative);
        }
        const auto resolve = [&](const sce_junction &junction) {
            const auto found = real.find(virtual_key(junction.get_value(), S.source));
            return found == real.end() ? std::string() : found->second;
        };
        std::map<std::string, const sce_ng_pfs_file_t *> by_path;
        for (const auto &file : S.files->get_files())
            by_path[virtual_key(file.path().get_value(), S.source)] = &file;

        for (const auto &dir : S.files->get_dirs()) {
            std::string path = resolve(dir.path());
            if (path.empty())
                path = virtual_key(dir.path().get_value(), S.source);
            S.entries.push_back({Kind::Dir, path});
        }
        for (const auto &empty : S.pages->get_emptyFiles()) {
            if (!by_path.count(virtual_key(empty.get_value(), S.source)))
                continue;
            std::string path = resolve(empty);
            if (path.empty())
                path = virtual_key(empty.get_value(), S.source);
            S.entries.push_back({Kind::Empty, path});
        }
        const auto &page_map = S.pages->get_pageMap();
        for (const auto &table : S.unicv->get_idatabase()->m_tables) {
            if (table->get_header()->get_numSectors() == 0)
                continue;
            const auto page = page_map.find(table->get_icv_salt());
            if (page == page_map.end())
                return fail("a PFS page has no file");
            const auto file = by_path.find(virtual_key(page->second.get_value(), S.source));
            if (file == by_path.end())
                return fail("a PFS file is missing from files.db: " + page->second.get_value().generic_string());
            const auto type = file->second->file.m_info.header.type;
            if (is_directory(type) || is_unexisting(type))
                return fail("unexpected PFS file type for " + page->second.get_value().generic_string());
            const std::string path = resolve(page->second);
            if (path.empty())
                return fail("file in files.db not in the dump: " + page->second.get_value().generic_string());
            Entry entry{is_encrypted(type) ? Kind::Decrypt : Kind::Copy, path, file->second->file.m_info.header.size};
            entry.file = file->second;
            entry.junction = &page->second;
            entry.table = table;
            S.entries.push_back(std::move(entry));
        }
        return int(S.entries.size());
    } catch (const std::exception &error) {
        return fail(std::string("decryption setup failed: ") + error.what());
    }
}

EMSCRIPTEN_KEEPALIVE int vd_count() { return session ? int(session->entries.size()) : 0; }

EMSCRIPTEN_KEEPALIVE const char *vd_entry(int index) {
    entry_text.clear();
    if (session && index >= 0 && size_t(index) < session->entries.size()) {
        const Entry &entry = session->entries[index];
        entry_text = std::string(kind_name(entry.kind)) + "\t" + std::to_string(entry.size) + "\t" + entry.path;
    }
    return entry_text.c_str();
}

EMSCRIPTEN_KEEPALIVE int vd_run(int index, const char *dest) {
    if (!session || index < 0 || size_t(index) >= session->entries.size())
        return fail("no such entry");
    Session &S = *session;
    const Entry &entry = S.entries[index];
    if (entry.kind != Kind::Decrypt)
        return fail("entry " + entry.path + " needs no decryption");
    try {
        S.output.str({});
        PfsFile file(S.crypto, S.f00d, S.output, S.klicensee, S.source, *entry.file, *entry.junction,
            S.files->get_header(), entry.table);
        const std::filesystem::path destination = dest;
        if (file.decrypt_file(destination) < 0)
            return fail("cannot decrypt " + entry.path);
        return 0;
    } catch (const std::exception &error) {
        return fail("cannot decrypt " + entry.path + ": " + error.what());
    }
}

EMSCRIPTEN_KEEPALIVE int vd_decrypt_self(const char *in, const char *out) {
    if (!session)
        return fail("no dump open");
    try {
        std::ifstream input(in, std::ios::binary);
        std::vector<uint8_t> self((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (!input.eof() && !input)
            return fail(std::string("cannot read ") + in);
        const std::vector<uint8_t> elf = decrypt_fself(self, session->klicensee);
        if (elf.empty())
            return fail(std::string("cannot decrypt the SELF ") + in);
        if (elf == self)
            return 0;
        std::ofstream output(out, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(elf.data()), std::streamsize(elf.size()));
        return output ? 1 : fail(std::string("cannot write ") + out);
    } catch (const std::exception &error) {
        return fail(std::string("cannot decrypt the SELF ") + in + ": " + error.what());
    }
}

EMSCRIPTEN_KEEPALIVE const char *vd_error() { return error_text.c_str(); }

EMSCRIPTEN_KEEPALIVE void vd_close() { session.reset(); }
}

// --- .pkg -------------------------------------------------------------------
// The layout and keys of vita3k/packages (pkg.h, after mmozeiko's pkg2zip),
// without the emulator state install_pkg installs into.
namespace {
const uint8_t pkg_vita_2[] = { 0xe3, 0x1a, 0x70, 0xc9, 0xce, 0x1d, 0xd7, 0x2b, 0xf3, 0xc0, 0x62, 0x29, 0x63, 0xf2, 0xec, 0xcb };
const uint8_t pkg_vita_3[] = { 0x42, 0x3a, 0xca, 0x3a, 0x2b, 0xd5, 0x64, 0x9f, 0x96, 0x86, 0xab, 0xad, 0x6f, 0xd8, 0x80, 0x1f };
const uint8_t pkg_vita_4[] = { 0xaf, 0x07, 0xfd, 0x59, 0x65, 0x25, 0x27, 0xba, 0xf1, 0x33, 0x89, 0x66, 0x8b, 0x17, 0xd9, 0xea };
struct PkgHeader {
    uint32_t magic;
    uint16_t revision, type;
    uint32_t info_offset, info_count, header_size, file_count;
    uint64_t total_size, data_offset, data_size;
    char content_id[0x30];
    uint8_t digest[0x10], pkg_data_iv[0x10], pkg_signatures[0x40];
};
struct PkgExtHeader {
    uint32_t magic, unknown_01, header_size, data_size, data_offset, data_type;
    uint64_t pkg_data_size;
    uint32_t padding_01, data_type2, unknown_02, padding_02;
    uint64_t padding_03, padding_04;
};
struct PkgEntry {
    uint32_t name_offset, name_size;
    uint64_t data_offset, data_size;
    uint32_t type, padding;
};
uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }

struct PkgEntryInfo {
    bool dir;
    std::string name;
    uint64_t offset, size;
};
struct Pkg {
    std::ifstream file;
    PkgHeader header{};
    uint8_t key[16]{};
    std::string type, title_id, content_id, category;
    std::vector<PkgEntryInfo> entries;
};
std::unique_ptr<Pkg> pkg;
std::string result_text;

// AES-128-CTR over the package body from 16-byte block `block`.
bool pkg_ctr(const Pkg &p, uint64_t block, uint8_t *data, size_t size) {
    uint8_t counter[16];
    std::memcpy(counter, p.header.pkg_data_iv, 16);
    for (int i = 15; i >= 0 && block; --i) { // counter = iv + block, big-endian
        const uint32_t sum = counter[i] + uint32_t(block & 0xff);
        counter[i] = uint8_t(sum);
        block = (block >> 8) + (sum >> 8);
    }
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    const bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, p.key, counter)
        && EVP_DecryptUpdate(ctx, data, &length, data, int(size));
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}
// TITLE_ID and CATEGORY from the package's (plain) param.sfo.
std::string sfo_string(const std::vector<uint8_t> &sfo, const char *wanted) {
    if (sfo.size() < 20 || std::memcmp(sfo.data(), "\0PSF", 4) != 0)
        return {};
    uint32_t key_start, data_start, count;
    std::memcpy(&key_start, &sfo[8], 4);
    std::memcpy(&data_start, &sfo[12], 4);
    std::memcpy(&count, &sfo[16], 4);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= sfo.size(); ++i) {
        uint16_t key_offset;
        uint32_t length, data_offset;
        std::memcpy(&key_offset, &sfo[20 + i * 16], 2);
        std::memcpy(&length, &sfo[20 + i * 16 + 4], 4);
        std::memcpy(&data_offset, &sfo[20 + i * 16 + 12], 4);
        const size_t key_at = key_start + key_offset, value_at = data_start + data_offset;
        if (key_at >= sfo.size() || value_at + length > sfo.size())
            return {};
        if (std::strcmp(reinterpret_cast<const char *>(&sfo[key_at]), wanted) == 0)
            return std::string(reinterpret_cast<const char *>(&sfo[value_at]), strnlen(reinterpret_cast<const char *>(&sfo[value_at]), length));
    }
    return {};
}
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE int vd_pkg_open(const char *path) {
    pkg.reset();
    error_text.clear();
    auto p = std::make_unique<Pkg>();
    p->file.open(path, std::ios::binary);
    p->file.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(p->file.tellg());
    PkgExtHeader ext{};
    p->file.seekg(0);
    if (!p->file.read(reinterpret_cast<char *>(&p->header), sizeof(PkgHeader))
        || !p->file.read(reinterpret_cast<char *>(&ext), sizeof(PkgExtHeader)))
        return fail("not a .pkg file (too small)");
    if (be32(p->header.magic) != 0x7F504b47 || be32(ext.magic) != 0x7F657874)
        return fail("not a PS Vita .pkg file");
    if (size < be64(p->header.total_size))
        return fail("the .pkg file is truncated");
    uint32_t info_offset = be32(p->header.info_offset), content_type = 0, sfo_offset = 0, sfo_size = 0, items_offset = 0;
    for (uint32_t i = 0; i < be32(p->header.info_count); ++i) {
        uint32_t block[4];
        p->file.seekg(info_offset);
        if (!p->file.read(reinterpret_cast<char *>(block), sizeof(block)))
            return fail("the .pkg header is corrupt");
        switch (be32(block[0])) {
        case 2: content_type = be32(block[2]); break;
        case 13: items_offset = be32(block[2]); break;
        case 14: sfo_offset = be32(block[2]); sfo_size = be32(block[3]); break;
        default: break;
        }
        info_offset += 8 + be32(block[1]);
    }
    p->type = content_type == 0x15 ? "app" : content_type == 0x16 ? "dlc" : content_type == 0x1F ? "theme" : "";
    if (p->type.empty())
        return fail("unsupported .pkg content type " + std::to_string(content_type) + " (not a PS Vita game)");
    const uint8_t *vita_key = nullptr;
    switch (be32(ext.data_type2) & 7) {
    case 2: vita_key = pkg_vita_2; break;
    case 3: vita_key = pkg_vita_3; break;
    case 4: vita_key = pkg_vita_4; break;
    default: return fail("unknown .pkg encryption key");
    }
    { // main key = AES-128-ECB(vita key, iv)
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int length = 0;
        EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, vita_key, nullptr);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        EVP_EncryptUpdate(ctx, p->key, &length, p->header.pkg_data_iv, 16);
        EVP_CIPHER_CTX_free(ctx);
    }
    std::vector<uint8_t> sfo(sfo_size);
    p->file.seekg(sfo_offset);
    p->file.read(reinterpret_cast<char *>(sfo.data()), std::streamsize(sfo.size()));
    p->title_id = sfo_string(sfo, "TITLE_ID");
    p->category = sfo_string(sfo, "CATEGORY");
    p->content_id = std::string(p->header.content_id, strnlen(p->header.content_id, sizeof(p->header.content_id)));
    if (p->type == "app" && p->category == "gp")
        p->type = "patch";
    const uint64_t data_offset = be64(p->header.data_offset);
    for (uint32_t i = 0; i < be32(p->header.file_count); ++i) {
        PkgEntry entry{};
        const uint64_t at = items_offset + uint64_t(i) * 32;
        p->file.seekg(data_offset + at);
        if (!p->file.read(reinterpret_cast<char *>(&entry), sizeof(entry)) || !pkg_ctr(*p, at / 16, reinterpret_cast<uint8_t *>(&entry), sizeof(entry)))
            return fail("the .pkg file table is corrupt");
        if (size < data_offset + be32(entry.name_offset) + be32(entry.name_size) || size < data_offset + be64(entry.data_offset) + be64(entry.data_size))
            return fail("the .pkg file is truncated or corrupt");
        std::vector<uint8_t> name(be32(entry.name_size));
        p->file.seekg(data_offset + be32(entry.name_offset));
        p->file.read(reinterpret_cast<char *>(name.data()), std::streamsize(name.size()));
        pkg_ctr(*p, be32(entry.name_offset) / 16, name.data(), name.size());
        const uint32_t kind = be32(entry.type) & 0xff;
        p->entries.push_back({kind == 4 || kind == 18, std::string(name.begin(), name.end()), be64(entry.data_offset), be64(entry.data_size)});
    }
    pkg = std::move(p);
    return int(pkg->entries.size());
}

EMSCRIPTEN_KEEPALIVE const char *vd_pkg_info() {
    result_text = pkg ? pkg->type + "\t" + pkg->title_id + "\t" + pkg->content_id + "\t" + pkg->category : "";
    return result_text.c_str();
}

EMSCRIPTEN_KEEPALIVE const char *vd_pkg_entry(int index) {
    result_text.clear();
    if (pkg && index >= 0 && size_t(index) < pkg->entries.size()) {
        const auto &entry = pkg->entries[index];
        result_text = std::string(entry.dir ? "dir" : "file") + "\t" + std::to_string(entry.size) + "\t" + entry.name;
    }
    return result_text.c_str();
}

EMSCRIPTEN_KEEPALIVE int vd_pkg_extract(int index, const char *out) {
    if (!pkg || index < 0 || size_t(index) >= pkg->entries.size())
        return fail("no such .pkg entry");
    const auto &entry = pkg->entries[index];
    std::ofstream output(out, std::ios::binary | std::ios::trunc);
    if (!output)
        return fail(std::string("cannot write ") + out);
    std::vector<uint8_t> buffer(1 << 20);
    uint64_t offset = entry.offset, left = entry.size;
    pkg->file.clear();
    pkg->file.seekg(be64(pkg->header.data_offset) + offset);
    while (left) {
        const size_t chunk = size_t(std::min<uint64_t>(left, buffer.size()));
        if (!pkg->file.read(reinterpret_cast<char *>(buffer.data()), std::streamsize(chunk)) || !pkg_ctr(*pkg, offset / 16, buffer.data(), chunk))
            return fail("cannot read " + entry.name + " from the .pkg");
        output.write(reinterpret_cast<const char *>(buffer.data()), std::streamsize(chunk));
        offset += chunk;
        left -= chunk;
    }
    return output ? 0 : fail("cannot write " + entry.name);
}

EMSCRIPTEN_KEEPALIVE int vd_zrif_to_rif(const char *zrif, const char *out) {
    try {
        std::ofstream output(out, std::ios::binary | std::ios::trunc);
        zrif2rif(zrif, output);
        return 0;
    } catch (const std::exception &error) {
        return fail(std::string("not a zRIF license: ") + error.what());
    }
}

EMSCRIPTEN_KEEPALIVE void vd_pkg_close() { pkg.reset(); }

EMSCRIPTEN_KEEPALIVE int vd_install_pup(const char *pup, const char *out) {
    error_text.clear();
    try {
        result_text = install_pup(out, pup);
        // The runtime loads decrypted modules only (vita_app.cpp): system
        // SELFs need no license key.
        const uint8_t no_klic[16] = {};
        int failed = 0;
        for (const auto &item : std::filesystem::recursive_directory_iterator(out)) {
            const auto name = item.path().filename().string();
            const auto ext = item.path().extension().string();
            if (!item.is_regular_file() || (ext != ".suprx" && ext != ".skprx" && ext != ".self" && name != "eboot.bin"))
                continue;
            std::vector<uint8_t> self;
            {
                std::ifstream input(item.path(), std::ios::binary);
                self.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            }
            const std::vector<uint8_t> elf = decrypt_fself(self, no_klic);
            if (elf.empty()) {
                ++failed;
                continue;
            }
            if (elf != self) {
                std::ofstream output(item.path(), std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char *>(elf.data()), std::streamsize(elf.size()));
            }
        }
        if (failed)
            result_text += " (" + std::to_string(failed) + " modules not decrypted)";
        return 0;
    } catch (const std::exception &error) {
        return fail(std::string("cannot install the firmware: ") + error.what());
    }
}

EMSCRIPTEN_KEEPALIVE const char *vd_result() { return result_text.c_str(); }
}
