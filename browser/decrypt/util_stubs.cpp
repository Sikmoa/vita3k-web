// The Vita3K util functions sce_utils.cpp and pup.cpp call, without the rest of
// vita3k/util (SDL, the config and log setup the decryptor has no use for).
#include <util/fs.h>
#include <util/string_utils.h>

#include <fstream>
#include <iterator>

namespace fs_utils {
std::string path_to_utf8(const fs::path &path) { return path.string(); }
fs::path path_concat(const fs::path &path, const fs::path &suffix) { return fs::path(path.string() + suffix.string()); }
bool read_data(const fs::path &path, std::vector<char> &data) {
    std::ifstream file(path.string(), std::ios::binary);
    if (!file)
        return false;
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}
} // namespace fs_utils

namespace string_utils {
std::vector<uint8_t> string_to_byte_array(std::string_view text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < text.size(); i += 2)
        out.push_back(uint8_t(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return out;
}
} // namespace string_utils
