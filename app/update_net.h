#pragma once
// 在线升级的联网部分（WinHTTP，自动使用系统代理）。全部为阻塞调用，只能在后台线程中使用。
// 只发送普通 GET 请求：不上传任何文档、设备标识或使用数据。
#include "update_core.h"
#include <functional>
namespace lpdf::update {
struct HttpResult {
    int status{};
    std::string body;
    std::wstring error;
};
// 取一个小文件（清单 / 签名），超过 maxBytes 视为失败。
HttpResult HttpGet(const std::string& url, size_t maxBytes, const std::atomic<bool>* cancel = nullptr);

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string source;     // 取得清单的地址
    std::wstring error;     // 所有来源都失败时的原因
    int tried{}, verified{};
};
// 并行向所有来源请求 latest.json + latest.json.sig，只接受签名有效的清单；多个来源可用时取版本最高者。
ManifestResult FetchManifest(const std::atomic<bool>* cancel = nullptr);

struct DownloadProgress {
    uint64_t done{}, total{};
    double speed{};          // 字节/秒
    std::string host;        // 当前下载线路
    bool probing{};          // 正在测速选线
    bool verifying{};        // 正在校验 SHA-256
};
struct DownloadResult {
    fs::path file;
    std::string source;
    std::wstring error;
    bool cancelled{};
};
// 先对各线路做小范围测速，按速度依次尝试；支持断点续传，失败自动换下一条线路；完成后校验大小与 SHA-256。
DownloadResult DownloadAsset(const Asset& asset, const std::vector<std::string>& mirrors, const fs::path& folder,
                             const std::function<void(const DownloadProgress&)>& progress, const std::atomic<bool>* cancel);
}
