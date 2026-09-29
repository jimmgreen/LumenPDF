#pragma once
#include <atomic>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace lpdf {
namespace fs = std::filesystem;
struct Cancelled {};
using Cancel = std::shared_ptr<std::atomic_bool>;
inline void CheckCancel(const Cancel& c) { if (c && c->load()) throw Cancelled{}; }
std::string Utf8(std::wstring_view s);
std::wstring Wide(std::string_view s);
std::vector<unsigned char> ReadBytes(const fs::path& path);
void WriteBytes(const fs::path& path, const std::vector<unsigned char>& bytes);
fs::path UniquePath(const fs::path& directory, std::wstring_view suffix);
void AtomicReplace(const fs::path& temporary, const fs::path& destination);
fs::path ExecutablePath();
std::wstring QuoteArg(std::wstring_view argument);
int RunProcess(const fs::path& executable, const std::vector<std::wstring>& arguments,
               const Cancel& cancel, unsigned timeoutSeconds = 120,
               const fs::path& ownedWordRecord = {});
struct TempDirectory {
    fs::path path;
    TempDirectory();
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
};
}

