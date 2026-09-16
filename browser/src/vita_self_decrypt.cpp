// Browser pass-through for Vita3K's retail SELF decryption hook.
//
// module_parent.cpp unconditionally routes every loaded module through
// decrypt_fself() (vita3k/packages). The full retail decryptor needs OpenSSL
// and the title klicensee, neither of which belongs in the minimal Wasm
// target yet. Content staged for the browser is decrypted OFFLINE (same
// decrypt_fself, run on the host with the title work.bin); this TU provides
// the link symbol with identical observable behavior for already-decrypted
// input and a loud, diagnosable failure for still-encrypted input instead of
// silently mapping ciphertext as guest code.
//
// A follow-up can replace this file with the real packages decryptor (AES in
// Wasm + a staged klic) without touching any caller: the contract is the
// packages/sce_types.h declaration.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Declaration contract lives in the real header; do not fork it.
#include <packages/sce_types.h>

// Minimal SCE header view (external/vita-toolchain/src/self.h). Only the
// fields needed to locate the segment-info table are mirrored here.
namespace {
struct SelfHeaderPrefix {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint16_t sdk_type;
    std::uint16_t header_type;
    std::uint32_t metadata_offset;
    std::uint64_t header_len;
    std::uint64_t elf_filesize;
    std::uint64_t self_filesize;
    std::uint64_t unknown;
    std::uint64_t self_offset;
    std::uint64_t appinfo_offset;
    std::uint64_t elf_offset;
    std::uint64_t phdr_offset;
    std::uint64_t shdr_offset;
    std::uint64_t section_info_offset;
};
struct SegmentInfoView {
    std::uint64_t offset;
    std::uint64_t length;
    std::uint64_t compression;
    std::uint64_t encryption; // 1 = encrypted, 2 = plain (decrypted)
};
constexpr std::uint32_t WEB_SCE_MAGIC = 0x00454353; // "SCE\0" (SCE_MAGIC macro lives in sce_types.h)
} // namespace

std::vector<std::uint8_t> decrypt_fself(const std::vector<std::uint8_t> &fself, const std::uint8_t *klic) {
    (void)klic; // Offline packaging holds the key; nothing to unwrap here.
    if (fself.size() < sizeof(SelfHeaderPrefix)) {
        std::fprintf(stderr, "[vita3k-web] decrypt_fself: input too small (%zu bytes)\n", fself.size());
        return {};
    }
    SelfHeaderPrefix header{};
    std::memcpy(&header, fself.data(), sizeof(header));
    if (header.magic != WEB_SCE_MAGIC) {
        // Not a SELF container (plain ELF, e.g. homebrew): nothing to do.
        return fself;
    }
    if (header.version != 3 || header.header_type != 1) {
        std::fprintf(stderr, "[vita3k-web] decrypt_fself: unsupported SELF version=%u type=%u\n",
            header.version, header.header_type);
        return {};
    }
    // Segment count comes from the embedded ELF header at elf_offset.
    constexpr std::size_t E_PHNUM_OFF = 44;
    if (fself.size() < header.elf_offset + E_PHNUM_OFF + 2) {
        std::fprintf(stderr, "[vita3k-web] decrypt_fself: truncated ELF header\n");
        return {};
    }
    std::uint16_t phnum = 0;
    std::memcpy(&phnum, fself.data() + header.elf_offset + E_PHNUM_OFF, sizeof(phnum));
    if (phnum == 0 || phnum > 16) {
        std::fprintf(stderr, "[vita3k-web] decrypt_fself: implausible segment count %u\n", phnum);
        return {};
    }
    const std::uint64_t table_end = header.section_info_offset + std::uint64_t(phnum) * sizeof(SegmentInfoView);
    if (table_end > fself.size()) {
        std::fprintf(stderr, "[vita3k-web] decrypt_fself: truncated segment-info table\n");
        return {};
    }
    for (std::uint16_t i = 0; i < phnum; ++i) {
        SegmentInfoView info{};
        std::memcpy(&info, fself.data() + header.section_info_offset + std::uint64_t(i) * sizeof(info), sizeof(info));
        if (info.encryption != 2 || (info.compression != 1 && info.compression != 2)) {
            std::fprintf(stderr,
                "[vita3k-web] decrypt_fself: segment %u still retail-encrypted (enc=%llu comp=%llu); "
                "decrypt this file OFFLINE with Vita3K's host decrypt_fself + title work.bin first\n",
                i, (unsigned long long)info.encryption, (unsigned long long)info.compression);
            return {};
        }
    }
    return fself;
}
