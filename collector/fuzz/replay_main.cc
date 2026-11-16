// Without libFuzzer (GCC builds): replay the corpus files given on the command line through the target, so the
// seeds and any saved crash inputs stay regression tests in the normal ctest run.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv) {
  int n = 0;
  for (int i = 1; i < argc; ++i) {
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(argv[i])) {
      for (const auto& e : std::filesystem::directory_iterator(argv[i])) files.push_back(e.path());
    } else {
      files.emplace_back(argv[i]);
    }
    for (const auto& f : files) {
      std::ifstream in(f, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
      ++n;
    }
  }
  std::printf("replayed %d input(s)\n", n);
  return n > 0 ? 0 : 1;
}
