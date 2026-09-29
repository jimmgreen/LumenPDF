#include "update_net.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace lpdf::update {
namespace {
using Clock = std::chrono::steady_clock;
struct Handle {
    HINTERNET h{};
    Handle() = default;
    explicit Handle(HINTERNET value) : h(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    explicit operator bool() const { return h != nullptr; }
};
std::wstring LastError(const wchar_t* what) {
    const DWORD code = GetLastError();
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return std::wstring(what) + L"超时";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return std::wstring(what) + L"：无法解析域名";
    case ERROR_WINHTTP_CANNOT_CONNECT: return std::wstring(what) + L"：无法连接";
    case ERROR_WINHTTP_CONNECTION_ERROR: return std::wstring(what) + L"：连接被重置";
    case ERROR_WINHTTP_SECURE_FAILURE: return std::wstring(what) + L"：HTTPS 证书校验失败";
    default: return std::wstring(what) + L"失败（错误 " + std::to_wstring(code) + L"）";
    }
}
Handle OpenSession(int connectMs, int receiveMs) {
    const std::wstring agent = L"LumenPDF/" + Widen(kCurrentVersion) + L" (Windows; update)";
    const bool direct = GetEnvironmentVariableW(L"LPDF_UPDATE_NO_PROXY", nullptr, 0) > 0;   // 测试：模拟不走代理的网络
    HINTERNET session = WinHttpOpen(agent.c_str(), direct ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session && !direct)
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) {
        WinHttpSetTimeouts(session, connectMs, connectMs, receiveMs, receiveMs);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);
    }
    return Handle(session);
}
struct Response {
    int status{};
    uint64_t contentLength{};        // 0 = 未知
    uint64_t rangeStart{}, rangeTotal{};
    bool hasRange{};
    std::wstring error;
};
// 发起 GET；sink 返回 false 时中止。rangeFrom/rangeTo < 0 表示不带 Range。
Response Get(HINTERNET session, const std::string& url, long long rangeFrom, long long rangeTo,
             const std::function<bool(const char*, size_t)>& sink, const std::atomic<bool>* cancel,
             const std::function<bool(const Response&)>& onHeaders = {}) {
    Response r;
    const std::wstring wide = Widen(url);
    URL_COMPONENTS parts{sizeof(URL_COMPONENTS)};
    wchar_t host[256]{}, path[4096]{};
    parts.lpszHostName = host; parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = 4096;
    wchar_t extra[2048]{};
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) { r.error = L"下载地址无效"; return r; }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    Handle connection(WinHttpConnect(session, host, parts.nPort, 0));
    if (!connection) { r.error = LastError(L"连接"); return r; }
    const std::wstring target = std::wstring(path) + extra;
    Handle request(WinHttpOpenRequest(connection.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) { r.error = LastError(L"请求"); return r; }
    std::wstring headers = L"Cache-Control: no-cache\r\n";
    if (rangeFrom >= 0) headers += L"Range: bytes=" + std::to_wstring(rangeFrom) + L"-" + (rangeTo >= 0 ? std::to_wstring(rangeTo) : L"") + L"\r\n";
    if (!WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) { r.error = LastError(L"连接"); return r; }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);
    wchar_t value[128]{};
    size = sizeof value;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, value, &size, WINHTTP_NO_HEADER_INDEX))
        r.contentLength = _wcstoui64(value, nullptr, 10);
    size = sizeof value;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, value, &size, WINHTTP_NO_HEADER_INDEX)) {
        // Content-Range: bytes 100-199/2000
        unsigned long long a = 0, b = 0, total = 0;
        if (swscanf_s(value, L"bytes %llu-%llu/%llu", &a, &b, &total) == 3) { r.hasRange = true; r.rangeStart = a; r.rangeTotal = total; }
    }
    if (r.status != 200 && r.status != 206) { r.error = L"服务器返回 HTTP " + std::to_wstring(r.status); return r; }
    if (onHeaders && !onHeaders(r)) { if (r.error.empty()) r.error = L"响应不符合要求"; return r; }
    std::vector<char> buffer(64 * 1024);
    while (true) {
        if (cancel && cancel->load()) { r.error = L"已取消"; return r; }
        DWORD read = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) { r.error = LastError(L"下载"); return r; }
        if (read == 0) break;
        if (!sink(buffer.data(), read)) { if (r.error.empty()) r.error = L"已中止"; return r; }
    }
    return r;
}
}

HttpResult HttpGet(const std::string& url, size_t maxBytes, const std::atomic<bool>* cancel) {
    HttpResult out;
    Handle session = OpenSession(6000, 10000);
    if (!session) { out.error = LastError(L"初始化网络"); return out; }
    bool tooLarge = false;
    const auto r = Get(session.h, url, -1, -1, [&](const char* data, size_t n) {
        if (out.body.size() + n > maxBytes) { tooLarge = true; return false; }
        out.body.append(data, n); return true;
    }, cancel);
    out.status = r.status;
    out.error = tooLarge ? L"文件过大" : r.error;
    return out;
}

ManifestResult FetchManifest(const std::atomic<bool>* cancel) {
    const auto sources = ManifestSources();
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::optional<Manifest>> results;
        std::vector<std::wstring> errors;
        size_t done{};
        std::optional<Clock::time_point> firstSuccess;
        std::atomic<bool> stop{};   // 线程是分离的：只引用共享状态，不引用调用方的变量
    };
    auto shared = std::make_shared<Shared>();
    shared->results.resize(sources.size());
    shared->errors.resize(sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
        std::thread([shared, url = sources[i], i] {
            std::optional<Manifest> manifest;
            std::wstring error;
            auto body = HttpGet(url, 256 * 1024, &shared->stop);
            if (!body.error.empty()) error = body.error;
            else {
                auto sig = HttpGet(url + ".sig", 4096, &shared->stop);
                if (!sig.error.empty()) error = L"签名文件：" + sig.error;
                else if (!VerifyManifestSignature(body.body, sig.body)) error = L"签名无效（内容可能被篡改）";
                else {
                    std::string why;
                    manifest = ParseManifest(body.body, &why);
                    if (!manifest) error = L"清单无效：" + Widen(why);
                }
            }
            std::lock_guard lock(shared->mutex);
            shared->results[i] = std::move(manifest);
            shared->errors[i] = std::move(error);
            if (shared->results[i] && !shared->firstSuccess) shared->firstSuccess = Clock::now();
            ++shared->done;
            shared->cv.notify_all();
        }).detach();
    }
    ManifestResult out;
    out.tried = static_cast<int>(sources.size());
    std::unique_lock lock(shared->mutex);
    const auto deadline = Clock::now() + std::chrono::seconds(25);
    while (shared->done < sources.size()) {
        if (cancel && cancel->load()) break;
        // 第一份有效清单到达后再等 2 秒：jsDelivr 等缓存可能比 GitHub 旧，取版本最高者。
        if (shared->firstSuccess && Clock::now() > *shared->firstSuccess + std::chrono::seconds(2)) break;
        if (Clock::now() > deadline) break;
        shared->cv.wait_for(lock, std::chrono::milliseconds(200));
    }
    shared->stop = true;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& m = shared->results[i];
        if (!m) continue;
        ++out.verified;
        if (!out.manifest || IsNewer(m->version, out.manifest->version)) { out.manifest = *m; out.source = sources[i]; }
    }
    if (!out.manifest) {
        bool tampered = false;
        for (const auto& e : shared->errors) if (e.find(L"签名无效") != std::wstring::npos) tampered = true;
        out.error = tampered ? L"取得的升级信息签名无效，已拒绝。" : L"无法连接升级服务器（已尝试 " + std::to_wstring(sources.size()) + L" 个地址）。请检查网络后重试。";
        for (size_t i = 0; i < sources.size() && i < 2; ++i)
            if (!shared->errors[i].empty()) out.error += L"\n" + Widen(HostOf(sources[i])) + L"：" + shared->errors[i];
    }
    return out;
}

DownloadResult DownloadAsset(const Asset& asset, const std::vector<std::string>& mirrors, const fs::path& folder,
                             const std::function<void(const DownloadProgress&)>& progress, const std::atomic<bool>* cancel) {
    DownloadResult out;
    std::error_code error;
    fs::create_directories(folder, error);
    const fs::path final = folder / Widen(asset.name);
    fs::path part = final;
    part += L".part";
    auto report = [&](DownloadProgress p) { if (progress) progress(p); };
    // 已下载且校验通过：直接使用。
    if (fs::exists(final, error) && fs::file_size(final, error) == asset.size) {
        report({.done = asset.size, .total = asset.size, .verifying = true});
        if (Sha256File(final, cancel) == asset.sha256) { out.file = final; out.source = "cache"; return out; }
        fs::remove(final, error);
    }
    const auto candidates = DownloadCandidates(asset, mirrors);
    // ---- 测速：并行请求前 256 KB，响应必须与清单中的文件大小一致（排除镜像返回的错误页）----
    report({.total = asset.size, .probing = true});
    struct Probe { double speed{}; bool ok{}; };
    struct ProbeState { std::mutex mutex; std::condition_variable cv; std::vector<Probe> probes; size_t done{}; std::atomic<bool> stop{}; };
    auto state = std::make_shared<ProbeState>();
    state->probes.resize(candidates.size());
    const uint64_t expected = asset.size;
    const long long probeBytes = static_cast<long long>(std::min<uint64_t>(asset.size, 256 * 1024));
    for (size_t i = 0; i < candidates.size(); ++i) {
        std::thread([state, i, probeBytes, expected, url = candidates[i]] {
            Probe p;
            Handle session = OpenSession(5000, 8000);
            if (session) {
                uint64_t got = 0;
                bool enough = false;   // 服务器忽略 Range 返回整个文件时，读够测速量就主动断开
                const auto start = Clock::now();
                const auto r = Get(session.h, url, 0, probeBytes - 1, [&](const char*, size_t n) {
                    got += n;
                    if (got >= static_cast<uint64_t>(probeBytes)) { enough = true; return false; }
                    return true;
                }, &state->stop);
                const double seconds = std::max(0.001, std::chrono::duration<double>(Clock::now() - start).count());
                const bool sizeOk = (r.status == 206 && r.hasRange && r.rangeTotal == expected) || (r.status == 200 && r.contentLength == expected);
                if ((r.error.empty() || enough) && sizeOk && got > 0) { p.ok = true; p.speed = got / seconds; }
            }
            std::lock_guard lock(state->mutex);
            state->probes[i] = p;
            ++state->done;
            state->cv.notify_all();
        }).detach();
    }
    {
        std::unique_lock lock(state->mutex);
        const auto deadline = Clock::now() + std::chrono::seconds(12);
        while (state->done < candidates.size() && !(cancel && cancel->load()) && Clock::now() < deadline)
            state->cv.wait_for(lock, std::chrono::milliseconds(200));
        state->stop = true;
    }
    if (cancel && cancel->load()) { out.cancelled = true; out.error = L"已取消"; return out; }
    std::vector<size_t> order(candidates.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    {
        std::lock_guard lock(state->mutex);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const auto& pa = state->probes[a]; const auto& pb = state->probes[b];
            if (pa.ok != pb.ok) return pa.ok;
            return pa.speed > pb.speed;
        });
    }
    // ---- 依次尝试各线路，断点续传 ----
    std::wstring lastError = L"所有下载线路均不可用";
    for (size_t index : order) {
        const auto& url = candidates[index];
        const std::string host = HostOf(url);
        Handle session = OpenSession(8000, 20000);
        if (!session) { lastError = LastError(L"初始化网络"); continue; }
        uint64_t have = fs::exists(part, error) ? fs::file_size(part, error) : 0;
        if (have >= asset.size) { fs::remove(part, error); have = 0; }
        HANDLE file = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { out.error = L"无法写入下载文件：" + part.wstring(); return out; }
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(have);
        SetFilePointerEx(file, position, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
        auto lastReport = Clock::now() - std::chrono::seconds(1);
        auto windowStart = Clock::now();
        uint64_t windowBytes = 0;
        double speed = 0;
        bool writeFailed = false, overflow = false, mismatch = false;
        const auto r = Get(session.h, url, have > 0 ? static_cast<long long>(have) : -1, -1, [&](const char* data, size_t n) {
            if (have + n > asset.size) { overflow = true; return false; }
            DWORD written = 0;
            if (!WriteFile(file, data, static_cast<DWORD>(n), &written, nullptr) || written != n) { writeFailed = true; return false; }
            have += n; windowBytes += n;
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - windowStart).count();
            if (elapsed >= 1.0) { speed = windowBytes / elapsed; windowBytes = 0; windowStart = now; }
            if (now - lastReport > std::chrono::milliseconds(150)) { lastReport = now; report({.done = have, .total = asset.size, .speed = speed, .host = host}); }
            return true;
        }, cancel, [&](const Response& head) {
            if (head.status == 206) {
                // 续传：服务器给出的起点与总大小必须和本地一致。
                if (!head.hasRange || head.rangeStart != have || head.rangeTotal != asset.size) { mismatch = true; return false; }
                return true;
            }
            // 200：服务器忽略了 Range 或本来就是全新下载 —— 从头写。
            if (head.contentLength && head.contentLength != asset.size) { mismatch = true; return false; }
            if (have > 0) {
                LARGE_INTEGER zero{};
                SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
                SetEndOfFile(file);
                have = 0;
            }
            return true;
        });
        CloseHandle(file);
        if (cancel && cancel->load()) { out.cancelled = true; out.error = L"已取消（已下载部分会保留，下次继续）"; return out; }
        if (writeFailed) { out.error = L"写入下载文件失败，请检查磁盘空间"; return out; }
        if (mismatch) { lastError = Widen(host) + L"：文件大小与清单不符"; continue; }   // 该线路返回的不是这个文件（错误页等），换线路
        if (overflow) { fs::remove(part, error); lastError = L"文件大小与清单不符"; continue; }
        if (!r.error.empty()) { lastError = Widen(host) + L"：" + r.error; continue; }   // 保留已下载部分，换线路续传
        if (have != asset.size) { lastError = Widen(host) + L"：下载不完整"; continue; }
        report({.done = have, .total = asset.size, .host = host, .verifying = true});
        const auto digest = Sha256File(part, cancel);
        if (cancel && cancel->load()) { out.cancelled = true; out.error = L"已取消"; return out; }
        if (digest != asset.sha256) { fs::remove(part, error); lastError = L"SHA-256 校验失败（" + Widen(host) + L" 提供的文件已损坏或被篡改），已换线路重试"; continue; }
        fs::remove(final, error);
        if (!MoveFileExW(part.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING)) { out.error = L"无法保存下载文件"; return out; }
        out.file = final;
        out.source = url;
        return out;
    }
    out.error = lastError;
    return out;
}
}
