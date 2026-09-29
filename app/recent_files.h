#pragma once
// 最近打开的文件：持久化、去重、固定与分组。纯逻辑，不依赖界面库，便于单元测试。
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace lpdf {
namespace fs = std::filesystem;
struct RecentEntry {
    fs::path path;
    int64_t opened{};   // Unix 秒
    bool pinned{};
};
enum class RecentGroup { Pinned, Today, Yesterday, Week, Earlier };
class RecentFiles {
public:
    static constexpr size_t kLimit = 30;
    void Load(const fs::path& store);
    // 原子写入（临时文件 + 替换）；失败时保留旧文件并返回 false。只读模式下不写。
    bool Save() const;
    void ReadOnly(bool value) noexcept { readOnly_ = value; }
    // 多个窗口共享同一存储：修改前重新读取磁盘上的最新内容。
    void Reload() { if (!store_.empty()) Load(store_); }
    void Touch(const fs::path& path, int64_t now);
    void Remove(const fs::path& path);
    void Pin(const fs::path& path, bool pinned);
    // 清除历史；默认保留已固定的文件。
    void Clear(bool keepPinned = true);
    bool Pinned(const fs::path& path) const;
    // 固定的在前，其余按打开时间倒序。
    const std::vector<RecentEntry>& Entries() const noexcept { return entries_; }
    static std::vector<RecentEntry> Parse(std::string_view text);
    static std::string Serialize(const std::vector<RecentEntry>& entries);
    static bool SamePath(const fs::path& a, const fs::path& b);
private:
    void Sort();
    fs::path store_;
    std::vector<RecentEntry> entries_;
    bool readOnly_{};
};
// 每个文件的阅读位置（页码、页内偏移、缩放方式）。独立于最近打开列表保存，互不影响格式兼容。
struct ReadingPosition {
    fs::path path;
    int page{};
    float offset{};     // 页内纵向偏移（PDF 点）
    int fit{1};         // 0=自定，1=适合页面，2=适合宽度
    float zoom{1};
    int64_t saved{};
};
class ReadingPositions {
public:
    static constexpr size_t kLimit = 300;
    void Load(const fs::path& store);
    bool Save();
    void ReadOnly(bool value) noexcept { readOnly_ = value; }
    const ReadingPosition* Find(const fs::path& path) const;
    // 返回 true 表示内容有变化（需要保存）。
    bool Remember(ReadingPosition position);
    void Forget(const fs::path& path);
    bool Dirty() const noexcept { return dirty_; }
    size_t Size() const noexcept { return items_.size(); }
    static std::vector<ReadingPosition> Parse(std::string_view text);
    static std::string Serialize(const std::vector<ReadingPosition>& items);
private:
    fs::path store_;
    std::vector<ReadingPosition> items_;   // 最近保存的在前
    std::vector<fs::path> touched_;         // 本进程修改过的文件；保存时只用它们覆盖磁盘内容（多窗口合并）
    bool readOnly_{}, dirty_{};
};
RecentGroup GroupOf(const RecentEntry& entry, int64_t now);
std::wstring_view GroupTitle(RecentGroup group);
// “刚刚”“5 分钟前”“今天 14:32”“昨天 09:10”“周三 18:00”“2026-08-01”
std::wstring RelativeTime(int64_t then, int64_t now);
int64_t UnixNow();
}