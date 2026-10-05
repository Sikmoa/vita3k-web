// The two Vita3K util functions sce_utils.cpp calls, without the rest of
// vita3k/util (SDL, the config and log setup the decryptor has no use for).
#include <util/fs.h>
#include <util/string_utils.h>

namespace fs_utils {
std::string path_to_utf8(const fs::path &path) { return path.string(); }
} // namespace fs_utils

namespace string_utils {
std::vector<uint8_t> string_to_byte_array(std::string_view text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < text.size(); i += 2)
        out.push_back(uint8_t(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return out;
}
} // namespace string_utils
