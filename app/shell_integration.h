#pragma once
// 与 Windows 外壳的集成：单实例（再次打开文件时交给已运行的窗口）与 PDF 文件关联注册。
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace lpdf {
namespace fs = std::filesystem;
// 单实例：第一个进程持有命名互斥体并创建隐藏的消息窗口接收路径。
class SingleInstance {
public:
    SingleInstance();
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    // 已有实例在运行时返回 true。
    bool Secondary() const noexcept { return secondary_; }
    // 次实例：把路径（可为空，仅激活窗口）交给主实例。成功返回 true。
    bool Forward(const fs::path& path) const;
    // 次实例：把一个待合并文件交给主实例的合并列表。主实例可能正在启动，最多等待 waitMs。
    bool ForwardMerge(const fs::path& path, unsigned waitMs = 8000) const;
    // 主实例：在界面线程创建接收窗口。回调在界面线程执行。
    // merge=true 表示来自右键“使用 LumenPDF 合并”。
    void Listen(std::function<void(fs::path, bool merge)> received);
private:
    void* mutex_{};
    void* window_{};
    bool secondary_{};
    std::function<void(fs::path, bool)> received_;
    bool Send(unsigned long long magic, const fs::path& path, unsigned waitMs) const;
    static long long __stdcall Proc(void* hwnd, unsigned message, unsigned long long wparam, long long lparam);
};
// 把窗口带到前台（最小化时先还原）。
void BringToFront(void* hwnd);
// 以当前用户身份注册为 PDF 候选打开程序（不需要管理员权限），并打开系统“默认应用”设置页。
// 返回空字符串表示成功，否则为错误说明。Windows 不允许程序静默修改默认应用，最终由用户确认。
std::wstring RegisterPdfHandler(const fs::path& executable, bool openSettings = true);
// 当前 .pdf 的用户默认程序是否为本程序（按 ProgId 或实际可执行文件判断）。
bool IsDefaultPdfHandler();
// 弹出 Windows 的“选择打开方式”窗口。注意：SHOpenWithDialog 在调用线程上运行模态循环，
// 不能在界面线程调用（界面会重入并可能崩溃）；应用内已改用 OpenDefaultAppsSettings。
bool PromptDefaultPdf(void* owner, const fs::path& sample);
// 异步打开系统“默认应用”中本程序的页面（不阻塞调用线程）。
void OpenDefaultAppsSettings();
// 已登记的打开命令指向的程序不存在（程序被移动或删除）时返回 true。
bool PdfHandlerStale();
// 旧版本把 PDF 文档图标指向程序图标（,0）；打开命令属于当前程序时改为专用文档图标（,-3）。
bool RepairPdfDocumentIcon(const fs::path& executable);
bool PdfHandlerRegistered();
// 资源管理器右键菜单“使用 LumenPDF 合并”（PDF、Word、图片；当前用户，无需管理员）。
std::wstring RegisterMergeMenu(const fs::path& executable);
void UnregisterMergeMenu();
bool MergeMenuRegistered();
bool MergeMenuStale();
// 删除本程序写入的全部外壳注册（卸载时调用）。
void UnregisterShell();
// 右键菜单注册的扩展名。
inline constexpr const wchar_t* kMergeExtensions[] = {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".png", L".jpg", L".jpeg"};
// 以独立进程打开新窗口（--new-window），可带文件路径。成功返回 true。
bool LaunchNewWindow(const fs::path& path = {});
// 本机所有 LumenPDF 主窗口（按 Z 序，含自身）。用于窗口间切换。
std::vector<void*> LumenWindows();
// 切换到下一个/上一个 LumenPDF 窗口；只有一个窗口时返回 false。
bool CycleWindows(void* self, int direction);
}