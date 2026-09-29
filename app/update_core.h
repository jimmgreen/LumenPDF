#pragma once
// 在线升级的纯逻辑部分（不联网）：版本比较、清单解析、签名与 SHA-256 校验、下载地址、安装方式判断、便携版文件替换。
// 联网部分见 update_net.h，界面见 update_ui.cpp。
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace lpdf::update {
namespace fs = std::filesystem;
inline constexpr const char* kCurrentVersion = LPDF_VERSION;
inline constexpr const char* kRepo = "jimmgreen/LumenPDF";

struct Version {
    int major{}, minor{}, patch{};
    std::string pre;   // 预发布标记（如 beta.1）；空 = 正式版，正式版高于同号预发布
};
std::optional<Version> ParseVersion(std::string_view text);   // 接受 "0.4.0" / "v0.4.0" / "0.4.0-beta.1"
int CompareVersions(const Version& a, const Version& b);      // <0 a 较旧，0 相同，>0 a 较新
bool IsNewer(std::string_view candidate, std::string_view current);

struct Asset {
    std::string name;
    uint64_t size{};
    std::string sha256;              // 小写十六进制
    std::vector<std::string> urls;   // 原始地址（GitHub Release）
};
struct Manifest {
    std::string version, tag, published, page, notes;
    Asset setup, portable;
    std::vector<std::string> mirrors;   // 下载加速前缀（签名保护，可远程更换）
};
// 解析 latest.json；字段缺失、版本号或哈希不合法、文件名含路径时返回空并给出原因。
std::optional<Manifest> ParseManifest(std::string_view json, std::string* error = nullptr);

// ECDSA P-256 / SHA-256 签名校验：signature 为 64 字节 r||s 的 Base64，publicKey 为 X||Y。
bool VerifySignature(std::string_view data, std::string_view signatureBase64, std::span<const uint8_t, 64> publicKey);
bool VerifyManifestSignature(std::string_view data, std::string_view signatureBase64);   // 使用内置公钥

std::string Sha256Hex(std::span<const uint8_t> data);
std::optional<std::string> Sha256File(const fs::path& file, const std::atomic<bool>* cancel = nullptr);

// 清单来源：GitHub Release、jsDelivr（国内可用的 CDN）、GitHub 加速镜像。LPDF_UPDATE_SOURCES（分号分隔）仅供测试覆盖。
std::vector<std::string> ManifestSources();
std::vector<std::string> DefaultMirrors();
// 某个安装包的候选下载地址：原始地址 + 每个镜像前缀 + 原始地址（只对 https://github.com/ 地址加前缀）。
std::vector<std::string> DownloadCandidates(const Asset& asset, const std::vector<std::string>& mirrors);
std::string HostOf(std::string_view url);

enum class InstallKind { Installer, Portable };
// 程序目录下有 Inno Setup 卸载程序（unins000.exe）即视为安装版，否则为便携版。
InstallKind DetectInstallKind(const fs::path& exeDir);

// 便携版替换：staged 中的全部文件覆盖到 target；覆盖前先备份到 backup，任一文件失败即全部回滚，target 恢复原样。
struct ApplyResult { bool ok{}; std::wstring error; size_t files{}; };
ApplyResult ApplyStagedFiles(const fs::path& staged, const fs::path& target, const fs::path& backup, int retries = 40);

std::wstring Widen(std::string_view utf8);
std::string FormatSize(uint64_t bytes);
}
