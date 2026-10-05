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
// Errors return < 0; vd_error() says why.
#include <CryptoOperationsFactory.h>
#include <F00DKeyEncryptorFactory.h>
#include <FilesDbParser.h>
#include <PfsFile.h>
#include <PfsPageMapper.h>
#include <UnicvDbParser.h>
#include <UnicvDbTypes.h>
#include <packages/sce_types.h>

#include <emscripten/emscripten.h>

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
