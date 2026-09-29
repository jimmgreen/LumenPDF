#include "platform.h"
#include <windows.h>
#include <objbase.h>
#include <fstream>
#include <stdexcept>
#include <sstream>
namespace lpdf {
std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid Unicode text");
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::vector<unsigned char> ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open file: " + Utf8(path.wstring()));
    const auto end = input.tellg();
    if (end < 0 || end > 1024LL * 1024 * 1024) throw std::runtime_error("File exceeds the 1 GiB input limit");
    std::vector<unsigned char> data(static_cast<size_t>(end));
    input.seekg(0);
    if (!data.empty() && !input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Cannot read the complete file");
    return data;
}
void WriteBytes(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot write file");
    out.close();
    if (!out) throw std::runtime_error("Cannot finish writing file");
}
fs::path UniquePath(const fs::path& directory, std::wstring_view suffix) {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot create a temporary name");
    wchar_t text[40]{};
    StringFromGUID2(id, text, 40);
    return directory / (std::wstring(L"lumenpdf-") + text + std::wstring(suffix));
}
TempDirectory::TempDirectory() {
    path = UniquePath(fs::temp_directory_path(), L"");
    if (!fs::create_directory(path)) throw std::runtime_error("Cannot create temporary directory");
}
TempDirectory::~TempDirectory() {
    std::error_code error;
    // Only this random directory created by this instance is owned here.
    fs::remove_all(path, error);
}
void AtomicReplace(const fs::path& temporary, const fs::path& destination) {
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reopen output for flushing");
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!flushed) throw std::runtime_error("Cannot flush output to disk");
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace destination; it may be open in another application");
}
fs::path ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) throw std::runtime_error("Cannot locate executable");
    path.resize(n); return path;
}
std::wstring QuoteArg(std::wstring_view arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'"') { out.append(slashes * 2 + 1, L'\\'); out += ch; }
        else { out.append(slashes, L'\\'); out += ch; }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\'); out += L'"'; return out;
}
static void StopOwnedWord(const fs::path& record) {
    if (record.empty()) return;
    std::ifstream stream(record);
    DWORD pid = 0; unsigned long long expected = 0;
    if (!(stream >> pid >> expected) || !pid) return;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    if (!process) return;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER value{}; value.LowPart = created.dwLowDateTime; value.HighPart = created.dwHighDateTime;
        if (value.QuadPart == expected) TerminateProcess(process, ERROR_CANCELLED);
    }
    CloseHandle(process);
}
int RunProcess(const fs::path& exe, const std::vector<std::wstring>& args, const Cancel& cancel,
               unsigned timeoutSeconds, const fs::path& record) {
    CheckCancel(cancel);
    std::wstring command = QuoteArg(exe.wstring());
    for (const auto& arg : args) command += L" " + QuoteArg(arg);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) throw std::runtime_error("Cannot create conversion job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit))) {
        CloseHandle(job); throw std::runtime_error("Cannot configure conversion job");
    }
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &process)) {
        CloseHandle(job); throw std::runtime_error("Cannot start converter");
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, ERROR_CANCELLED);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job); StopOwnedWord(record);
        throw std::runtime_error("Cannot isolate conversion process");
    }
    ResumeThread(process.hThread); CloseHandle(process.hThread);
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutSeconds) * 1000;
    bool cancelled = false, timeout = false;
    while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) {
        cancelled = cancel && cancel->load(); timeout = GetTickCount64() > deadline;
        if (cancelled || timeout) { StopOwnedWord(record); TerminateJobObject(job, ERROR_CANCELLED); break; }
    }
    WaitForSingleObject(process.hProcess, 3000);
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess); CloseHandle(job); StopOwnedWord(record);
    if (cancelled) throw Cancelled{};
    if (timeout) throw std::runtime_error("Office conversion timed out after "+std::to_string(timeoutSeconds)+" seconds");
    return static_cast<int>(code);
}
}


