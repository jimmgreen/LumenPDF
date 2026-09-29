// 在线升级测试：
//  离线：版本比较、清单解析与拒绝、ECDSA 签名（真实公钥 + 篡改）、SHA-256、下载地址、安装方式判断、便携版替换与回滚。
//  本地网络：内置一个只监听 127.0.0.1 的 HTTP 服务器，模拟“断流 / 返回错误页 / 内容被篡改 / 忽略 Range”等坏线路，
//  验证 FetchManifest 只接受签名有效的清单、DownloadAsset 能测速换线、断点续传并最终通过 SHA-256 校验。
// 用法：update_tests <fixtures/update> <输出目录>
#include "update_core.h"
#include "update_install.h"
#include "update_net.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using namespace lpdf::update;
namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
std::string ReadAll(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::ostringstream s; s << in.rdbuf(); return s.str(); }
void WriteAll(const fs::path& p, std::string_view data) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size())); }

// ---- 极简本地 HTTP 服务器 ----
struct Route {
    std::string body;
    enum class Mode { Normal, IgnoreRange, CutHalf, NotFound } mode{Mode::Normal};
};
class Server {
public:
    std::map<std::string, Route> routes;
    std::mutex mutex;
    std::map<std::string, int> hits;
    std::map<std::string, int> rangedHits;   // 带非零起点 Range 的请求（续传）
    int port{};
    bool Start() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listener_, 16) != 0) return false;
        int len = sizeof addr;
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        thread_ = std::thread([this] { Loop(); });
        return true;
    }
    void Stop() { stop_ = true; closesocket(listener_); if (thread_.joinable()) thread_.join(); }
    std::string Url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
private:
    SOCKET listener_{INVALID_SOCKET};
    std::thread thread_;
    std::atomic<bool> stop_{};
    void Loop() {
        while (!stop_) {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            std::thread([this, client] { Serve(client); closesocket(client); }).detach();
        }
    }
    static void Send(SOCKET s, std::string_view data) {
        while (!data.empty()) { const int n = send(s, data.data(), static_cast<int>(std::min<size_t>(data.size(), 65536)), 0); if (n <= 0) return; data.remove_prefix(n); }
    }
    void Serve(SOCKET client) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) { const int n = recv(client, buffer, sizeof buffer, 0); if (n <= 0) return; request.append(buffer, n); }
        const auto sp1 = request.find(' '), sp2 = request.find(' ', sp1 + 1);
        const std::string path = request.substr(sp1 + 1, sp2 - sp1 - 1);
        long long from = -1, to = -1;
        if (const auto r = request.find("Range: bytes="); r != std::string::npos) {
            sscanf_s(request.c_str() + r + 13, "%lld-%lld", &from, &to);
        }
        Route route;
        {
            std::lock_guard lock(mutex);
            ++hits[path];
            if (from > 0) ++rangedHits[path];
            const auto it = routes.find(path);
            if (it == routes.end()) route.mode = Route::Mode::NotFound; else route = it->second;
        }
        if (route.mode == Route::Mode::NotFound) { Send(client, "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found"); return; }
        const auto& body = route.body;
        const long long size = static_cast<long long>(body.size());
        if (from >= 0 && route.mode != Route::Mode::IgnoreRange && from < size) {
            if (to < 0 || to >= size) to = size - 1;
            std::string part = body.substr(static_cast<size_t>(from), static_cast<size_t>(to - from + 1));
            if (route.mode == Route::Mode::CutHalf) part.resize(part.size() / 2);
            std::ostringstream head;
            head << "HTTP/1.1 206 Partial Content\r\nContent-Length: " << (to - from + 1) << "\r\nContent-Range: bytes " << from << "-" << to << "/" << size << "\r\nConnection: close\r\n\r\n";
            Send(client, head.str());
            Send(client, part);
            return;
        }
        std::string payload = body;
        if (route.mode == Route::Mode::CutHalf) payload.resize(payload.size() / 2);
        std::ostringstream head;
        head << "HTTP/1.1 200 OK\r\nContent-Length: " << size << "\r\nConnection: close\r\n\r\n";
        Send(client, head.str());
        Send(client, payload);
    }
};
std::string Blob(size_t size, uint32_t seed) {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; ++i) { seed = seed * 1664525u + 1013904223u; s[i] = static_cast<char>(seed >> 24); }
    return s;
}
}

// 发布后的真实网络检查（不属于 ctest）：update_tests --live <输出目录> [setup|portable]
// 配合 LPDF_UPDATE_NO_PROXY=1 可模拟不走代理的国内网络。
int Live(const fs::path& output, bool setup) {
    SetConsoleOutputCP(CP_UTF8);
    const auto started = GetTickCount64();
    const auto fetched = FetchManifest();
    std::printf("manifest: %s  verified %d / tried %d  %.1fs\n", fetched.manifest ? fetched.manifest->version.c_str() : "NONE", fetched.verified, fetched.tried, (GetTickCount64() - started) / 1000.0);
    if (!fetched.manifest) { std::wprintf(L"%ls\n", fetched.error.c_str()); return 1; }
    std::printf("source: %s\n", fetched.source.c_str());
    const auto& asset = setup ? fetched.manifest->setup : fetched.manifest->portable;
    std::string lastHost;
    const auto t0 = GetTickCount64();
    const auto result = DownloadAsset(asset, fetched.manifest->mirrors, output, [&](const DownloadProgress& p) {
        if (p.host != lastHost && !p.host.empty()) { std::printf("downloading via %s\n", p.host.c_str()); lastHost = p.host; }
    }, nullptr);
    if (!result.error.empty()) { std::wprintf(L"download failed: %ls\n", result.error.c_str()); return 1; }
    std::printf("downloaded %s (%llu bytes) from %s in %.1fs, SHA-256 verified\n", asset.name.c_str(), static_cast<unsigned long long>(asset.size), result.source.c_str(), (GetTickCount64() - t0) / 1000.0);
    return 0;
}
int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--live") return Live(argv[2], argc >= 4 && std::string(argv[3]) == "setup");
    if (argc < 3) { std::puts("usage: update_tests <fixtures/update> <output>"); return 2; }
    const fs::path fixtures = argv[1], output = argv[2];
    std::error_code ec;
    fs::remove_all(output, ec);
    fs::create_directories(output);

    // ---- 版本 ----
    Check(IsNewer("0.4.1", "0.4.0") && IsNewer("v1.0.0", "0.9.9") && IsNewer("0.10.0", "0.9.0"), "版本号按数值比较");
    Check(!IsNewer("0.4.0", "0.4.0") && !IsNewer("0.3.9", "0.4.0"), "相同或更旧的版本不算新版本");
    Check(IsNewer("0.4.0", "0.4.0-beta.2") && !IsNewer("0.4.0-beta.1", "0.4.0"), "正式版高于同号预发布");
    Check(!ParseVersion("0.4") && !ParseVersion("x.y.z") && !ParseVersion("1.2.3 ") && !ParseVersion("1.2.3-") && !IsNewer("garbage", "0.1.0"), "拒绝不合法的版本号");
    Check(ParseVersion(kCurrentVersion).has_value(), "内置版本号合法");

    // ---- 签名（真实公钥 + 仓库中的已签名清单） ----
    const std::string manifestText = ReadAll(fixtures / "latest.json");
    const std::string signature = ReadAll(fixtures / "latest.json.sig");
    Check(!manifestText.empty() && !signature.empty(), "读取测试清单");
    Check(VerifyManifestSignature(manifestText, signature), "内置公钥验证已签名清单");
    std::string tampered = manifestText;
    if (const auto pos = tampered.find("9.9.0"); pos != std::string::npos) tampered.replace(pos, 5, "9.9.1");
    Check(!VerifyManifestSignature(tampered, signature), "改动一个字符后签名失效");
    std::string badSig = signature;
    badSig[5] = badSig[5] == 'A' ? 'B' : 'A';
    Check(!VerifyManifestSignature(manifestText, badSig) && !VerifyManifestSignature(manifestText, "") && !VerifyManifestSignature(manifestText, "@@@"), "错误或残缺的签名被拒绝");

    // ---- 清单解析 ----
    std::string why;
    const auto manifest = ParseManifest(manifestText, &why);
    Check(manifest.has_value(), "解析测试清单");
    if (manifest) {
        Check(manifest->version == "9.9.0" && manifest->setup.size == 12345 && manifest->portable.name == "LumenPDF-9.9.0-portable-x64.zip", "清单字段");
        Check(manifest->notes.find("中文") != std::string::npos, "清单中的中文说明（UTF-8 / \\u 转义）");
        Check(manifest->mirrors.size() == 2 && manifest->mirrors[0] == "https://gh-proxy.com/", "清单中的镜像列表");
        const auto urls = DownloadCandidates(manifest->setup, manifest->mirrors);
        Check(urls.size() == 3 && urls[0].rfind("https://github.com/", 0) == 0 && urls[1] == "https://gh-proxy.com/" + urls[0], "下载地址 = 原始地址 + 镜像前缀");
    }
    auto reject = [&](std::string text, const char* what) { Check(!ParseManifest(text), what); };
    auto variant = [&](std::string_view from, std::string_view to) { std::string s = manifestText; if (auto p = s.find(from); p != std::string::npos) s.replace(p, from.size(), to); return s; };
    reject(variant("\"LumenPDF\"", "\"Other\""), "拒绝其它产品的清单");
    reject(variant("\"schema\": 1", "\"schema\": 2"), "拒绝未知格式版本");
    reject(variant("LumenPDF-9.9.0-setup.exe", "..\\\\evil.exe"), "拒绝带路径的文件名");
    reject(variant("\"size\": 12345", "\"size\": -1"), "拒绝不合法的大小");
    reject(variant("\"version\": \"9.9.0\"", "\"version\": \"latest\""), "拒绝不合法的版本号");
    reject(manifestText.substr(0, manifestText.size() / 2), "拒绝截断的 JSON");
    reject(std::string(300 * 1024, ' '), "拒绝过大的清单");
    {
        auto s = manifestText;
        const auto p = s.find("\"sha256\": \"");
        s[p + 11] = 'Z';
        reject(s, "拒绝不合法的 SHA-256");
    }
    reject(variant("\"urls\": [\"https://github.com/jimmgreen/LumenPDF/releases/download/v9.9.0/LumenPDF-9.9.0-setup.exe\"]", "\"urls\": [\"http://example.com/a.exe\"]"), "拒绝非 HTTPS 下载地址");

    // ---- SHA-256 ----
    const std::string abc = "abc";
    Check(Sha256Hex({reinterpret_cast<const uint8_t*>(abc.data()), abc.size()}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 标准测试向量");
    WriteAll(output / "hash.bin", abc);
    Check(Sha256File(output / "hash.bin") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "文件 SHA-256");

    // ---- 安装方式判断 ----
    fs::create_directories(output / "installed");
    WriteAll(output / "installed" / "unins000.exe", "x");
    fs::create_directories(output / "portable");
    Check(DetectInstallKind(output / "installed") == InstallKind::Installer && DetectInstallKind(output / "portable") == InstallKind::Portable, "安装版 / 便携版判断");

    // ---- 便携版替换：成功 ----
    const fs::path staged = output / "staged", target = output / "target";
    WriteAll(staged / "LumenPDF.exe", "new-exe");
    WriteAll(staged / "lumatext.dll", "new-dll");
    WriteAll(staged / "licenses" / "NEW.txt", "new-license");
    WriteAll(target / "LumenPDF.exe", "old-exe");
    WriteAll(target / "lumatext.dll", "old-dll");
    WriteAll(target / "keep.txt", "user-file");
    auto applied = ApplyStagedFiles(staged, target, target / ".update-backup", 2);
    Check(applied.ok && applied.files == 3, "便携版替换成功");
    Check(ReadAll(target / "LumenPDF.exe") == "new-exe" && ReadAll(target / "licenses" / "NEW.txt") == "new-license" && ReadAll(target / "keep.txt") == "user-file", "新文件就位，其它文件保留");
    Check(!fs::exists(target / ".update-backup"), "成功后删除备份");

    // ---- 便携版替换：文件被独占时回滚 ----
    WriteAll(target / "LumenPDF.exe", "old-exe");
    WriteAll(target / "lumatext.dll", "old-dll");
    fs::remove_all(target / "licenses");
    WriteAll(staged / "zz-locked.dat", "new-locked");
    WriteAll(target / "zz-locked.dat", "old-locked");
    HANDLE lock = CreateFileW((target / "zz-locked.dat").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);   // 不允许共享：无法改名
    applied = ApplyStagedFiles(staged, target, target / ".update-backup", 2);
    CloseHandle(lock);
    Check(!applied.ok && !applied.error.empty(), "文件被占用时替换失败并报告原因");
    Check(ReadAll(target / "LumenPDF.exe") == "old-exe" && ReadAll(target / "lumatext.dll") == "old-dll" && ReadAll(target / "zz-locked.dat") == "old-locked", "失败后全部恢复原文件");
    Check(!fs::exists(target / "licenses" / "NEW.txt"), "失败后删除新增的文件");

    // ---- 解压（系统 tar.exe） ----
    {
        std::wstring error;
        const fs::path zipSource = output / "zipsrc";
        WriteAll(zipSource / "LumenPDF.exe", "zip-exe");
        WriteAll(zipSource / "licenses" / "A.txt", "a");
        const fs::path zip = output / "pkg.zip";
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        const std::wstring cmd = L"\"" + (fs::path(system) / L"tar.exe").wstring() + L"\" -a -cf \"" + zip.wstring() + L"\" -C \"" + zipSource.wstring() + L"\" LumenPDF.exe licenses";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        std::wstring mutableCmd = cmd;
        if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        const bool ok = ExtractZip(zip, output / "unzipped", error);
        Check(ok && ReadAll(output / "unzipped" / "LumenPDF.exe") == "zip-exe" && ReadAll(output / "unzipped" / "licenses" / "A.txt") == "a", "用系统 tar.exe 解压 zip");
        if (!ok) std::wprintf(L"  %ls\n", error.c_str());
    }

    // ---- 本地网络 ----
    Server server;
    Check(server.Start(), "启动本地 HTTP 服务器");
    SetEnvironmentVariableW(L"LPDF_UPDATE_SOURCES", L"test");   // 允许 http://127.0.0.1 地址
    SetEnvironmentVariableW(L"LPDF_UPDATE_NO_PROXY", L"1");
    {
        std::lock_guard l(server.mutex);
        server.routes["/good/latest.json"] = {manifestText};
        server.routes["/good/latest.json.sig"] = {signature};
        server.routes["/evil/latest.json"] = {tampered};
        server.routes["/evil/latest.json.sig"] = {signature};
    }
    // 篡改的来源排在前面，也必须被忽略。
    const std::wstring sources = L"http://127.0.0.1:" + std::to_wstring(server.port) + L"/evil/latest.json;http://127.0.0.1:" + std::to_wstring(server.port) +
                                 L"/missing/latest.json;http://127.0.0.1:" + std::to_wstring(server.port) + L"/good/latest.json";
    SetEnvironmentVariableW(L"LPDF_UPDATE_SOURCES", sources.c_str());
    const auto fetched = FetchManifest();
    Check(fetched.manifest.has_value() && fetched.manifest->version == "9.9.0" && fetched.verified == 1 && fetched.source.find("/good/") != std::string::npos, "只接受签名有效的清单来源");
    SetEnvironmentVariableW(L"LPDF_UPDATE_SOURCES", (L"http://127.0.0.1:" + std::to_wstring(server.port) + L"/evil/latest.json").c_str());
    const auto rejected = FetchManifest();
    Check(!rejected.manifest && rejected.error.find(L"签名无效") != std::wstring::npos, "全部来源签名无效时拒绝并提示");
    SetEnvironmentVariableW(L"LPDF_UPDATE_SOURCES", L"test");

    const std::string payload = Blob(3 * 1024 * 1024 + 123, 7);
    std::string corrupt = payload;
    corrupt[corrupt.size() / 2] ^= 0x5A;
    {
        std::lock_guard l(server.mutex);
        server.routes["/cut/pkg.zip"] = {payload, Route::Mode::CutHalf};
        server.routes["/html/pkg.zip"] = {"<html>rate limited</html>"};
        server.routes["/corrupt/pkg.zip"] = {corrupt};
        server.routes["/good/pkg.zip"] = {payload};
        server.routes["/norange/pkg.zip"] = {payload, Route::Mode::IgnoreRange};
    }
    Asset asset;
    asset.name = "pkg.zip";
    asset.size = payload.size();
    asset.sha256 = Sha256Hex({reinterpret_cast<const uint8_t*>(payload.data()), payload.size()});
    // 1) 错误页被测速阶段排除；内容被篡改的线路校验失败后换线路；最终从好线路得到正确文件。
    asset.urls = {server.Url("/html/pkg.zip"), server.Url("/corrupt/pkg.zip"), server.Url("/good/pkg.zip")};
    int reports = 0;
    bool probed = false, verified = false;
    auto result = DownloadAsset(asset, {}, output / "dl1", [&](const DownloadProgress& p) { ++reports; probed |= p.probing; verified |= p.verifying; }, nullptr);
    Check(result.error.empty() && fs::exists(result.file) && Sha256File(result.file) == asset.sha256, "坏线路自动换线，下载结果通过 SHA-256");
    Check(probed && verified && reports > 2, "报告测速 / 进度 / 校验阶段");
    Check(!fs::exists(output / "dl1" / "pkg.zip.part"), "完成后不留临时文件");
    // 2) 已下载且校验通过：直接复用，不再请求网络。
    int goodHits;
    { std::lock_guard l(server.mutex); goodHits = server.hits["/good/pkg.zip"]; }
    result = DownloadAsset(asset, {}, output / "dl1", {}, nullptr);
    { std::lock_guard l(server.mutex); Check(result.error.empty() && result.source == "cache" && server.hits["/good/pkg.zip"] == goodHits, "已下载的升级包直接复用"); }
    // 3) 断点续传：先放一半数据在 .part，再从好线路续传。
    WriteAll(output / "dl2" / "pkg.zip.part", payload.substr(0, payload.size() / 3));
    asset.urls = {server.Url("/good/pkg.zip")};
    result = DownloadAsset(asset, {}, output / "dl2", {}, nullptr);
    { std::lock_guard l(server.mutex); Check(result.error.empty() && Sha256File(result.file) == asset.sha256 && server.rangedHits["/good/pkg.zip"] >= 1, "断点续传（Range 从已有长度继续）"); }
    // 4) 服务器忽略 Range（返回完整 200）：从头写入，结果仍然正确。
    WriteAll(output / "dl3" / "pkg.zip.part", payload.substr(0, payload.size() / 3));
    asset.urls = {server.Url("/norange/pkg.zip")};
    result = DownloadAsset(asset, {}, output / "dl3", {}, nullptr);
    Check(result.error.empty() && Sha256File(result.file) == asset.sha256, "线路不支持续传时从头下载");
    // 5) 断流线路：保留已下载部分，换到下一条线路续传。
    asset.urls = {server.Url("/cut/pkg.zip"), server.Url("/good/pkg.zip")};
    result = DownloadAsset(asset, {}, output / "dl4", {}, nullptr);
    Check(result.error.empty() && Sha256File(result.file) == asset.sha256, "断流后换线路完成下载");
    // 6) 全部线路都坏：报告错误，不产生最终文件。
    asset.urls = {server.Url("/corrupt/pkg.zip"), server.Url("/html/pkg.zip")};
    result = DownloadAsset(asset, {}, output / "dl5", {}, nullptr);
    Check(!result.error.empty() && !fs::exists(output / "dl5" / "pkg.zip"), "全部线路失败时报告错误且不留下坏文件");
    // 7) 取消。
    std::atomic<bool> cancel{true};
    asset.urls = {server.Url("/good/pkg.zip")};
    result = DownloadAsset(asset, {}, output / "dl6", {}, &cancel);
    Check(result.cancelled && !fs::exists(output / "dl6" / "pkg.zip"), "取消下载");
    server.Stop();

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
