#pragma once
// 用户偏好：%LOCALAPPDATA%\LumenPDF\settings.txt，key=value 文本，原子写入；未知键忽略、越界值夹紧。
#include <filesystem>
#include <string>
#include <string_view>
namespace lpdf {
struct AppSettings {
    int tone{0};               // 0 普通 1 护眼 2 夜间
    int layout{0};             // 0 单页 1 双页（封面单独） 2 双页
    int defaultFit{0};         // 新打开且无记忆位置时：0 适合页面 1 适合宽度
    bool restorePosition{true};// 重新打开时回到上次阅读位置
    bool externalNewWindow{false}; // 双击 PDF 时：false 在当前窗口打开 true 在新窗口打开
    bool logging{true};        // 写运行日志
    int exportDpi{150};        // PNG 导出分辨率
    bool askedDefault{false};  // 已询问过是否设为默认 PDF 阅读器（只主动询问一次）
    int sidebarWidth{260};     // 侧栏宽度（180–600，拖动侧栏右边缘调整）
    int sidebarTab{0};         // 上次选中的侧栏页：0 缩略图 1 目录 2 批注 3 搜索
    bool paged{false};         // 翻页模式：一次显示一页，滚动到页边整页翻过
    bool autoReload{false};    // 文件在外部修改后：false 询问 true 自动重新载入（有未保存修改时仍询问）
    int speechRate{0};         // 朗读语速 -10..10
    std::string speechVoice;   // 朗读语音的 SAPI 标识（空 = 自动：中文句用中文语音）
    bool autoUpdate{true};     // 自动检查更新（启动后、每天最多一次）
    long long lastUpdateCheck{0}; // 上次检查更新的时间（Unix 秒）
    std::string skipVersion;   // 用户选择“跳过此版本”的版本号
    static AppSettings Parse(std::string_view text);
    std::string Serialize() const;
    bool operator==(const AppSettings&) const = default;
};
AppSettings LoadSettingsFile(const std::filesystem::path& file);
bool SaveSettingsFile(const std::filesystem::path& file, const AppSettings& settings);
}
