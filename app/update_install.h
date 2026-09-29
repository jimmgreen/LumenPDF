#pragma once
// 升级安装：解压便携包、启动安装程序 / 便携替换助手，以及 --apply-update 助手进程本身。
#include "update_core.h"
namespace lpdf::update {
fs::path UpdatesRoot();                                   // %LOCALAPPDATA%\LumenPDF\Updates
// 用系统自带的 tar.exe 解压 zip（Windows 10 1803+）。
bool ExtractZip(const fs::path& zip, const fs::path& destination, std::wstring& error);
// 安装版：静默运行新的安装程序（按原安装方式、原任务选项升级），完成后重新打开 LumenPDF。
bool LaunchInstaller(const fs::path& setup, std::wstring& error);
// 便携版：由新版本自身（在暂存目录中运行）等待当前进程退出后替换文件，失败则回滚并重新打开旧版本。
bool LaunchPortableApply(const fs::path& staged, const fs::path& target, std::wstring& error);
// 助手入口：LumenPDF.exe --apply-update <staged> <target> <pid>
int ApplyUpdateMain(const fs::path& staged, const fs::path& target, unsigned long pid);
// 启动时清理：上次替换留下的备份、已安装版本（及更旧版本）的下载缓存。
void CleanupLeftovers(const fs::path& exeDir);
}
