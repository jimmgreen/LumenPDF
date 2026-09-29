#include "update_core.h"
#include "update_public_key.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
namespace lpdf::update {
namespace {
// ---- 极简 JSON 解析（只用于升级清单：大小受限、深度受限、不信任输入） ----
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type{Type::Null};
    bool boolean{};
    double number{};
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    const Json* Get(std::string_view key) const {
        for (const auto& [k, v] : fields) if (k == key) return &v;
        return nullptr;
    }
};
class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}
    bool Parse(Json& out) { Space(); if (!Value(out, 0)) return false; Space(); return i_ == s_.size(); }
private:
    std::string_view s_;
    size_t i_{};
    void Space() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_; }
    bool Literal(std::string_view word) { if (s_.substr(i_, word.size()) != word) return false; i_ += word.size(); return true; }
    static void Utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    bool Hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp{};
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low{};
                    if (!Literal("\\u") || !Hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                Utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Value(Json& out, int depth) {
        if (depth > 16 || i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            out.type = Json::Type::Object; ++i_; Space();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            while (true) {
                std::string key; Space();
                if (!String(key)) return false;
                Space();
                if (i_ >= s_.size() || s_[i_++] != ':') return false;
                Space();
                Json value;
                if (!Value(value, depth + 1)) return false;
                out.fields.emplace_back(std::move(key), std::move(value));
                Space();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            out.type = Json::Type::Array; ++i_; Space();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            while (true) {
                Space();
                Json value;
                if (!Value(value, depth + 1)) return false;
                out.items.push_back(std::move(value));
                Space();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                if (s_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { out.type = Json::Type::String; return String(out.text); }
        if (Literal("true")) { out.type = Json::Type::Bool; out.boolean = true; return true; }
        if (Literal("false")) { out.type = Json::Type::Bool; return true; }
        if (Literal("null")) return true;
        const size_t start = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) ++i_;
        if (start == i_) return false;
        const std::string number(s_.substr(start, i_ - start));
        char* end = nullptr;
        out.number = std::strtod(number.c_str(), &end);
        if (end != number.c_str() + number.size() || !std::isfinite(out.number)) return false;
        out.type = Json::Type::Number;
        return true;
    }
};

std::string Str(const Json* j) { return j && j->type == Json::Type::String ? j->text : std::string{}; }
bool TestMode() { return GetEnvironmentVariableW(L"LPDF_UPDATE_SOURCES", nullptr, 0) > 0; }
bool AllowedUrl(std::string_view url) {
    if (url.size() > 2048) return false;
    if (url.rfind("https://", 0) == 0) return url.size() > 8;
    return TestMode() && url.rfind("http://127.0.0.1:", 0) == 0;   // 本地端到端测试
}
bool SafeName(std::string_view name) {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") return false;
    for (char c : name) if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    return name.find("..") == std::string_view::npos;
}
bool Hex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool ParseAsset(const Json* j, Asset& a, std::string& error, const char* label) {
    if (!j || j->type != Json::Type::Object) { error = std::string("缺少安装包 ") + label; return false; }
    a.name = Str(j->Get("name"));
    auto sha = Str(j->Get("sha256"));
    std::transform(sha.begin(), sha.end(), sha.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    a.sha256 = sha;
    const Json* size = j->Get("size");
    if (!SafeName(a.name)) { error = std::string(label) + " 文件名不合法"; return false; }
    if (!Hex64(a.sha256)) { error = std::string(label) + " 的 SHA-256 不合法"; return false; }
    if (!size || size->type != Json::Type::Number || size->number < 1 || size->number > 1024.0 * 1024 * 1024 || std::floor(size->number) != size->number) {
        error = std::string(label) + " 的大小不合法"; return false;
    }
    a.size = static_cast<uint64_t>(size->number);
    if (const Json* urls = j->Get("urls"); urls && urls->type == Json::Type::Array)
        for (const auto& u : urls->items) if (u.type == Json::Type::String && AllowedUrl(u.text)) a.urls.push_back(u.text);
    if (a.urls.empty()) { error = std::string(label) + " 没有可用的下载地址"; return false; }
    return true;
}
std::vector<uint8_t> Base64(std::string_view text) {
    static const std::string_view table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    uint32_t buffer = 0; int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const auto pos = table.find(c);
        if (pos == std::string_view::npos) return {};
        buffer = (buffer << 6) | static_cast<uint32_t>(pos); bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF)); }
    }
    return out;
}
struct AlgCloser { void operator()(void* h) const { if (h) BCryptCloseAlgorithmProvider(h, 0); } };
struct KeyCloser { void operator()(void* h) const { if (h) BCryptDestroyKey(h); } };
struct HashCloser { void operator()(void* h) const { if (h) BCryptDestroyHash(h); } };
class Sha256 {
public:
    Sha256() {
        BCRYPT_ALG_HANDLE alg{};
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
        alg_.reset(alg);
        BCRYPT_HASH_HANDLE hash{};
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) hash_.reset(hash);
    }
    bool Ok() const { return static_cast<bool>(hash_); }
    void Add(const void* data, size_t size) {
        auto p = static_cast<const uint8_t*>(data);
        while (size) {
            const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size, 1u << 30));
            BCryptHashData(hash_.get(), const_cast<PUCHAR>(p), chunk, 0);
            p += chunk; size -= chunk;
        }
    }
    std::array<uint8_t, 32> Finish() { std::array<uint8_t, 32> out{}; BCryptFinishHash(hash_.get(), out.data(), 32, 0); return out; }
private:
    std::unique_ptr<void, AlgCloser> alg_;
    std::unique_ptr<void, HashCloser> hash_;
};
std::string Hex(std::span<const uint8_t> bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
}

std::optional<Version> ParseVersion(std::string_view text) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
    Version v;
    int* parts[] = {&v.major, &v.minor, &v.patch};
    size_t i = 0;
    for (int k = 0; k < 3; ++k) {
        if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
        long value = 0; size_t digits = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { value = value * 10 + (text[i] - '0'); ++i; if (++digits > 6) return std::nullopt; }
        *parts[k] = static_cast<int>(value);
        if (k < 2) { if (i >= text.size() || text[i] != '.') return std::nullopt; ++i; }
    }
    if (i < text.size()) {
        if (text[i] != '-' || i + 1 >= text.size()) return std::nullopt;
        v.pre = std::string(text.substr(i + 1));
        if (v.pre.size() > 32 || !std::all_of(v.pre.begin(), v.pre.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-'; })) return std::nullopt;
    }
    return v;
}
int CompareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;
    if (b.pre.empty()) return -1;
    return a.pre < b.pre ? -1 : 1;
}
bool IsNewer(std::string_view candidate, std::string_view current) {
    const auto a = ParseVersion(candidate), b = ParseVersion(current);
    return a && b && CompareVersions(*a, *b) > 0;
}

std::optional<Manifest> ParseManifest(std::string_view json, std::string* error) {
    std::string why;
    auto fail = [&](std::string message) -> std::optional<Manifest> { if (error) *error = std::move(message); return std::nullopt; };
    if (json.size() > 256 * 1024) return fail("清单过大");
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xEF && static_cast<unsigned char>(json[1]) == 0xBB && static_cast<unsigned char>(json[2]) == 0xBF) json.remove_prefix(3);
    Json root;
    if (!Parser(json).Parse(root) || root.type != Json::Type::Object) return fail("清单不是有效的 JSON");
    const Json* schema = root.Get("schema");
    if (!schema || schema->type != Json::Type::Number || schema->number != 1) return fail("不支持的清单格式");
    if (Str(root.Get("product")) != "LumenPDF") return fail("清单不属于 LumenPDF");
    Manifest m;
    m.version = Str(root.Get("version"));
    if (!ParseVersion(m.version)) return fail("清单版本号不合法");
    m.tag = Str(root.Get("tag"));
    m.published = Str(root.Get("published"));
    m.page = Str(root.Get("page"));
    if (!m.page.empty() && m.page.rfind("https://", 0) != 0) m.page.clear();
    m.notes = Str(root.Get("notes"));
    if (m.notes.size() > 16000) m.notes.resize(16000);
    const Json* assets = root.Get("assets");
    if (!assets || assets->type != Json::Type::Object) return fail("清单缺少安装包");
    if (!ParseAsset(assets->Get("setup"), m.setup, why, "setup") || !ParseAsset(assets->Get("portable"), m.portable, why, "portable")) return fail(why);
    if (const Json* mirrors = root.Get("mirrors"); mirrors && mirrors->type == Json::Type::Array) {
        for (const auto& item : mirrors->items)
            if (item.type == Json::Type::String && item.text.rfind("https://", 0) == 0 && item.text.back() == '/' && item.text.size() < 200 && m.mirrors.size() < 8)
                m.mirrors.push_back(item.text);
    } else m.mirrors = DefaultMirrors();
    return m;
}

bool VerifySignature(std::string_view data, std::string_view signatureBase64, std::span<const uint8_t, 64> publicKey) {
    const auto signature = Base64(signatureBase64);
    if (signature.size() != 64) return false;
    Sha256 sha;
    if (!sha.Ok()) return false;
    sha.Add(data.data(), data.size());
    auto digest = sha.Finish();
    BCRYPT_ALG_HANDLE algRaw{};
    if (BCryptOpenAlgorithmProvider(&algRaw, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
    std::unique_ptr<void, AlgCloser> alg(algRaw);
    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::copy(publicKey.begin(), publicKey.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    BCRYPT_KEY_HANDLE keyRaw{};
    if (BCryptImportKeyPair(algRaw, nullptr, BCRYPT_ECCPUBLIC_BLOB, &keyRaw, blob.data(), static_cast<ULONG>(blob.size()), 0) != 0) return false;
    std::unique_ptr<void, KeyCloser> key(keyRaw);
    return BCryptVerifySignature(keyRaw, nullptr, digest.data(), 32, const_cast<PUCHAR>(signature.data()), 64, 0) == 0;
}
bool VerifyManifestSignature(std::string_view data, std::string_view signatureBase64) {
    return VerifySignature(data, signatureBase64, std::span<const uint8_t, 64>(kUpdatePublicKey));
}

std::string Sha256Hex(std::span<const uint8_t> data) {
    Sha256 sha;
    if (!sha.Ok()) return {};
    sha.Add(data.data(), data.size());
    const auto digest = sha.Finish();
    return Hex(digest);
}
std::optional<std::string> Sha256File(const fs::path& file, const std::atomic<bool>* cancel) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    Sha256 sha;
    if (!sha.Ok()) return std::nullopt;
    std::vector<char> buffer(1 << 20);
    while (in) {
        if (cancel && cancel->load()) return std::nullopt;
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) sha.Add(buffer.data(), static_cast<size_t>(in.gcount()));
    }
    if (in.bad()) return std::nullopt;
    const auto digest = sha.Finish();
    return Hex(digest);
}

// 2026-09 在未走代理的国内网络实测均支持 Range 续传；第三方公益服务可能失效，清单中的 mirrors 可远程更换。
std::vector<std::string> DefaultMirrors() {
    return {"https://gh-proxy.com/", "https://ghfast.top/", "https://ghproxy.cxkpro.top/", "https://gh.zwy.one/", "https://ghproxy.net/"};
}
std::vector<std::string> ManifestSources() {
    std::vector<std::string> out;
    wchar_t custom[4096]{};
    if (GetEnvironmentVariableW(L"LPDF_UPDATE_SOURCES", custom, 4096)) {
        std::wstring all = custom;
        size_t start = 0;
        while (start <= all.size()) {
            const auto end = all.find(L';', start);
            const auto item = all.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!item.empty()) {
                const int n = WideCharToMultiByte(CP_UTF8, 0, item.c_str(), static_cast<int>(item.size()), nullptr, 0, nullptr, nullptr);
                std::string utf8(n, '\0');
                WideCharToMultiByte(CP_UTF8, 0, item.c_str(), static_cast<int>(item.size()), utf8.data(), n, nullptr, nullptr);
                out.push_back(utf8);
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return out;
    }
    const std::string repo = kRepo;
    const std::string release = "https://github.com/" + repo + "/releases/latest/download/latest.json";
    const std::string raw = "https://raw.githubusercontent.com/" + repo + "/main/update/latest.json";
    out.push_back(release);
    out.push_back(raw);
    // jsDelivr：国内访问 cdn / fastly 时会 301 到 raw.githubusercontent.com，gcore 节点直接提供内容。
    for (const char* cdn : {"gcore", "cdn", "fastly", "testingcf"})
        out.push_back(std::string("https://") + cdn + ".jsdelivr.net/gh/" + repo + "@main/update/latest.json");
    const auto mirrors = DefaultMirrors();
    for (const auto& mirror : mirrors) out.push_back(mirror + release);
    for (size_t i = 0; i < 2 && i < mirrors.size(); ++i) out.push_back(mirrors[i] + raw);
    return out;
}
std::vector<std::string> DownloadCandidates(const Asset& asset, const std::vector<std::string>& mirrors) {
    std::vector<std::string> out;
    auto add = [&](std::string url) { if (std::find(out.begin(), out.end(), url) == out.end()) out.push_back(std::move(url)); };
    for (const auto& url : asset.urls) add(url);
    for (const auto& url : asset.urls)
        if (url.rfind("https://github.com/", 0) == 0)
            for (const auto& mirror : mirrors) add(mirror + url);
    return out;
}
std::string HostOf(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return std::string(url);
    url.remove_prefix(scheme + 3);
    const auto slash = url.find('/');
    return std::string(url.substr(0, slash));
}

InstallKind DetectInstallKind(const fs::path& exeDir) {
    std::error_code error;
    return fs::exists(exeDir / L"unins000.exe", error) ? InstallKind::Installer : InstallKind::Portable;
}

ApplyResult ApplyStagedFiles(const fs::path& staged, const fs::path& target, const fs::path& backup, int retries) {
    ApplyResult result;
    std::error_code error;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(staged, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error)) files.push_back(fs::relative(it->path(), staged, error));
    if (error || files.empty()) { result.error = L"升级文件不完整"; return result; }
    fs::remove_all(backup, error);
    fs::create_directories(backup, error);
    if (error) { result.error = L"无法创建备份目录：" + backup.wstring(); return result; }
    std::vector<fs::path> moved, created;
    auto retry = [retries](auto&& attempt) {
        for (int i = 0; i <= retries; ++i) { if (attempt()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(250)); }
        return false;
    };
    auto rollback = [&] {
        std::error_code ignored;
        for (const auto& rel : created) { const auto dst = target / rel; retry([&] { return DeleteFileW(dst.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND; }); }
        for (const auto& rel : moved) {
            const auto dst = target / rel, saved = backup / rel;
            retry([&] { DeleteFileW(dst.c_str()); return MoveFileExW(saved.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; });
        }
        fs::remove_all(backup, ignored);
    };
    for (const auto& rel : files) {
        const auto src = staged / rel, dst = target / rel, saved = backup / rel;
        fs::create_directories(dst.parent_path(), error);
        if (fs::exists(dst, error)) {
            fs::create_directories(saved.parent_path(), error);
            // 改名而不是复制：正在运行的 exe / dll 也能改名，其它 LumenPDF 窗口不会阻止升级。
            if (!retry([&] { return MoveFileExW(dst.c_str(), saved.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; })) {
                rollback(); result.error = L"无法替换文件（可能被占用）：" + rel.wstring(); return result;
            }
            moved.push_back(rel);
        } else created.push_back(rel);
        if (!retry([&] { return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0; })) {
            if (std::find(created.begin(), created.end(), rel) == created.end()) created.push_back(rel);
            rollback(); result.error = L"无法写入文件：" + rel.wstring(); return result;
        }
        ++result.files;
    }
    fs::remove_all(backup, error);   // 仍在运行的旧 exe 删不掉：下次启动时清理
    result.ok = true;
    return result;
}

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}
std::string FormatSize(uint64_t bytes) {
    char text[32];
    if (bytes >= 1024ull * 1024) std::snprintf(text, sizeof text, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(text, sizeof text, "%.0f KB", bytes / 1024.0);
    else std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    return text;
}
}
