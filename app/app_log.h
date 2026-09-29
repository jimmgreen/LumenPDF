#pragma once
// 轻量运行日志：%LOCALAPPDATA%\LumenPDF\logs\lumenpdf.log，超过 1 MB 轮换为 .1。
// 线程安全；只记录操作类别、耗时与错误，不记录文档内容或口令。
#include <filesystem>
#include <string>
#include <string_view>
namespace lpdf::log {
void Init(const std::filesystem::path& folder);   // 空路径 = 禁用（冒烟测试）
void Write(std::string_view level, std::wstring_view message);
inline void Info(std::wstring_view m){Write("INFO",m);}
inline void Warn(std::wstring_view m){Write("WARN",m);}
inline void Error(std::wstring_view m){Write("ERROR",m);}
std::filesystem::path Folder();
// 进程崩溃时写入最后一条日志（未处理的 SEH 异常）。
void InstallCrashHandler();
}
