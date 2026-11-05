// Runs every file in the given directories through the fuzz target. Used as a regression
// test with the checked-in corpus on compilers without libFuzzer.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
    std::size_t count = 0;
    for (int i = 1; i < argc; ++i) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[i])) {
            if (!entry.is_regular_file()) continue;
            std::ifstream file(entry.path(), std::ios::binary);
            const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
            ++count;
        }
    }
    std::printf("replayed %zu inputs\n", count);
    return count > 0 ? 0 : 1;
}
