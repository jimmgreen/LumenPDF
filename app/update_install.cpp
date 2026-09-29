#include "update_install.h"
#include "app_log.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
namespace lpdf::update {
namespace {
constexpr const wchar_t* kBackupFolder = L".update-backup";
std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }
bool Start(const fs::path& exe, const std::wstring& arguments, const fs::path& cwd, bool hidden, DWORD waitMs, DWORD* exitCode, std::wstring& error) {
    std::wstring command = Quote(exe.wstring()) + L" " + arguments;
    STARTUPINFOW si{sizeof si};
    if (hidden) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        error = L"无法启动 " + exe.filename().wstring() + L"（错误 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    bool ok = true;
    if (waitMs) {
        if (WaitForSingleObject(pi.hProcess, waitMs) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 1); error = L"解压超时"; ok = false; }
        else if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}
}

fs::path UpdatesRoot() {
    PWSTR local = nullptr;
    fs::path root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) { root = fs::path(local) / L"LumenPDF" / L"Updates"; CoTaskMemFree(local); }
    return root;
}

bool ExtractZip(const fs::path& zipPath, const fs::path& destinationPath, std::wstring& error) {
    std::error_code ec;
    const fs::path zip = fs::absolute(zipPath, ec), destination = fs::absolute(destinationPath, ec);
    fs::remove_all(destination, ec);
    fs::create_directories(destination, ec);
    if (ec) { error = L"无法创建解压目录"; return false; }
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const fs::path tar = fs::path(system) / L"tar.exe";
    if (!fs::exists(tar, ec)) { error = L"系统缺少 tar.exe（需要 Windows 10 1803 或更高版本）"; return false; }
    DWORD code = 1;
    if (!Start(tar, L"-xf " + Quote(zip.wstring()) + L" -C " + Quote(destination.wstring()), destination, true, 120000, &code, error)) return false;
    if (code != 0) { error = L"解压升级包失败（tar 返回 " + std::to_wstring(code) + L"）"; return false; }
    if (!fs::exists(destination / L"LumenPDF.exe", ec)) { error = L"升级包内容不完整（缺少 LumenPDF.exe）"; return false; }
    return true;
}

bool LaunchInstaller(const fs::path& setup, std::wstring& error) {
    // /SILENT 显示进度但不提问；/CLOSEAPPLICATIONS 关闭仍在运行的其它 LumenPDF 窗口；/LPDFRELAUNCH=1 安装后重新打开。
    const std::wstring parameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LPDFRELAUNCH=1";
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = setup.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) { error = L"无法启动安装程序（错误 " + std::to_wstring(GetLastError()) + L"）"; return false; }
    log::Info(L"已启动升级安装程序：" + setup.filename().wstring());
    return true;
}

bool LaunchPortableApply(const fs::path& staged, const fs::path& target, std::wstring& error) {
    const std::wstring arguments = L"--apply-update " + Quote(staged.wstring()) + L" " + Quote(target.wstring()) + L" " + std::to_wstring(GetCurrentProcessId());
    if (!Start(staged / L"LumenPDF.exe", arguments, staged, false, 0, nullptr, error)) return false;
    log::Info(L"已启动便携版升级助手");
    return true;
}

int ApplyUpdateMain(const fs::path& staged, const fs::path& target, unsigned long pid) {
    std::error_code ec;
    auto fail = [&](const std::wstring& message, bool relaunch) {
        MessageBoxW(nullptr, (message + L"\n\n原版本保持不变。可以稍后在“更多 → 检查更新”中重试，或从 GitHub Release 页面手动下载。").c_str(),
                    L"LumenPDF 升级失败", MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
        if (relaunch && fs::exists(target / L"LumenPDF.exe", ec)) {
            std::wstring ignored;
            Start(target / L"LumenPDF.exe", L"", target, false, 0, nullptr, ignored);
        }
        return 3;
    };
    // 只替换真正的 LumenPDF 目录，防止参数被误用去覆盖其它文件夹。
    if (!fs::exists(staged / L"LumenPDF.exe", ec) || !fs::exists(target / L"LumenPDF.exe", ec)) return fail(L"升级参数无效。", false);
    if (fs::equivalent(staged, target, ec)) return fail(L"升级参数无效。", false);
    if (pid) {
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            const DWORD waited = WaitForSingleObject(process, 60000);
            CloseHandle(process);
            if (waited != WAIT_OBJECT_0) return fail(L"等待 LumenPDF 退出超时。", false);
        }
    }
    const auto result = ApplyStagedFiles(staged, target, target / kBackupFolder);
    if (!result.ok) return fail(result.error + L"\n已恢复原来的文件。", true);
    std::wstring error;
    Start(target / L"LumenPDF.exe", L"", target, false, 0, nullptr, error);
    return 0;
}

void CleanupLeftovers(const fs::path& exeDir) {
    std::error_code ec;
    fs::remove_all(exeDir / kBackupFolder, ec);
    const auto root = UpdatesRoot();
    if (root.empty() || !fs::exists(root, ec)) return;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const auto name = it->path().filename().string();
        // 已经是当前版本或更旧的下载缓存没有用了；暂存目录里的程序可能刚完成替换还在退出，删不掉就留到下次。
        if (ParseVersion(name) && !IsNewer(name, kCurrentVersion)) { std::error_code ignored; fs::remove_all(it->path(), ignored); }
    }
}
}
