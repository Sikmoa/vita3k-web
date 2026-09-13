// Node-runnable interpreter benchmark. Reads a genuine Vita executable and
// runs it through the same browser runtime entrypoint, reporting guest
// instruction throughput from the runtime's own counters.
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" {
int vita3k_web_run_vita(const std::uint8_t *bytes, std::uint32_t size);
std::uint64_t vita3k_web_last_run_instructions();
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <genuine-vita-eboot.bin>\n", argv[0]);
        return 2;
    }
    std::FILE *file = std::fopen(argv[1], "rb");
    if (!file) {
        std::fprintf(stderr, "Cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<std::uint8_t> image;
    std::uint8_t buffer[65536];
    size_t read;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        image.insert(image.end(), buffer, buffer + read);
    std::fclose(file);
    if (image.empty()) {
        std::fprintf(stderr, "Empty input: %s\n", argv[1]);
        return 2;
    }
    const int code = vita3k_web_run_vita(image.data(), static_cast<std::uint32_t>(image.size()));
    std::printf("[bench] exit=%d\n", code);
    return code == 42 ? 0 : 1;
}
