// 在线升级界面：自动检查（启动约 10 秒后、每天最多一次、可在设置中关闭）与手动检查（更多 → 检查更新）。
// 只在用户点击“下载并安装”后才下载；下载完成并通过 SHA-256 校验后，再由用户确认安装。
// 联网与校验逻辑在 update_net.cpp / update_core.cpp，安装在 update_install.cpp。
#include "application.h"
#include "app_log.h"
#include "ui.h"
#include "update_install.h"
#include "update_net.h"
#include <lumen/Dialog.h>
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

namespace lpdf {
using namespace lumen;

struct Application::UpdateSession {
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    bool checking{}, downloading{}, ready{}, offered{}, installOnExit{};
    std::optional<update::Manifest> manifest;
    update::InstallKind kind{update::InstallKind::Portable};
    update::DownloadProgress progress;
    fs::path file, staged;
    ProgressBar* bar{};
    Label* line{};
};

namespace {
std::wstring SpeedText(double bytesPerSecond) {
    if (bytesPerSecond <= 0) return L"";
    wchar_t text[32];
    if (bytesPerSecond >= 1024 * 1024) swprintf_s(text, L"%.1f MB/s", bytesPerSecond / 1048576.0);
    else swprintf_s(text, L"%.0f KB/s", bytesPerSecond / 1024.0);
    return text;
}
std::wstring ProgressText(const update::DownloadProgress& p) {
    if (p.probing) return L"正在测速并选择最快的下载线路…";
    if (p.verifying) return L"正在校验 SHA-256…";
    std::wstring text = update::Widen(update::FormatSize(p.done)) + L" / " + update::Widen(update::FormatSize(p.total));
    if (const auto speed = SpeedText(p.speed); !speed.empty()) text += L"  ·  " + speed;
    if (!p.host.empty()) text += L"  ·  线路 " + update::Widen(p.host);
    return text;
}
int Percent(const update::DownloadProgress& p) { return p.total ? static_cast<int>(p.done * 100 / p.total) : 0; }
long long Now() { return static_cast<long long>(std::time(nullptr)); }
fs::path ExeDir() { return ExecutablePath().parent_path(); }
}

Application::UpdateSession& Application::Updates() {
    if (!updateSession_) updateSession_ = std::make_shared<UpdateSession>();
    return *updateSession_;
}

void Application::ScheduleUpdateCheck() {
    if (smoke_) return;
    const bool testing = GetEnvironmentVariableW(L"LPDF_UPDATE_SOURCES", nullptr, 0) > 0;
    if (!testing) {
        std::thread([dir = ExeDir()] { update::CleanupLeftovers(dir); }).detach();
        if (!settings_.autoUpdate || settingsFile_.empty()) return;
        if (Now() - settings_.lastUpdateCheck < 24 * 3600 && Now() >= settings_.lastUpdateCheck) return;
    }
    wchar_t delay[16]{};
    float seconds = 10;
    if (GetEnvironmentVariableW(L"LPDF_UPDATE_DELAY", delay, 16)) seconds = std::clamp(static_cast<float>(_wtof(delay)), 0.5f, 600.0f);
    window_.SetTimeout(seconds, [this] { CheckForUpdates(false); });
}

std::wstring Application::UpdateMenuLabel() {
    if (!updateSession_) return L"检查更新…";
    auto& s = *updateSession_;
    if (s.ready && s.manifest) return L"安装已下载的 " + update::Widen(s.manifest->version) + L"…";
    if (s.downloading) return L"正在下载升级 " + std::to_wstring(Percent(s.progress)) + L"%（查看进度）";
    if (s.offered && s.manifest) return L"发现新版本 " + update::Widen(s.manifest->version) + L"…";
    if (s.checking) return L"正在检查更新…";
    return L"检查更新…";
}

void Application::CheckForUpdates(bool manual) {
    auto& s = Updates();
    if (s.ready) { if (manual) ConfirmUpdateInstall(); return; }
    if (s.downloading) { if (manual) ShowUpdateProgress(); return; }
    if (s.offered && s.manifest && manual) { ShowUpdateOffer(true); return; }
    if (s.checking) { if (manual) status_->Text(L"正在检查更新…"); return; }
    s.checking = true;
    if (manual) status_->Text(L"正在检查更新…");
    log::Info(manual ? L"手动检查更新" : L"自动检查更新");
    auto post = window_.Dispatcher();
    auto alive = alive_;
    auto session = updateSession_;
    std::thread([post, alive, session, manual, this] {
        auto result = update::FetchManifest(session->cancel.get());
        post.Post([this, alive, session, manual, result = std::move(result)]() mutable {
            if (!alive->load()) return;
            session->checking = false;
            if (!settingsFile_.empty()) { settings_.lastUpdateCheck = Now(); SaveSettings(); }
            if (!result.manifest) {
                log::Warn(L"检查更新失败：" + result.error);
                if (manual) { status_->Text(L"检查更新失败"); window_.Alert(L"检查更新失败", result.error); }
                return;
            }
            log::Info(L"升级清单来自 " + update::Widen(update::HostOf(result.source)) + L"，最新版本 " + update::Widen(result.manifest->version));
            if (!update::IsNewer(result.manifest->version, update::kCurrentVersion)) {
                if (manual) {
                    status_->Text(L"已是最新版本");
                    window_.Alert(L"已是最新版本", L"当前版本 " + update::Widen(update::kCurrentVersion) + L" 已是最新。");
                }
                return;
            }
            if (!manual && settings_.skipVersion == result.manifest->version) { log::Info(L"已跳过此版本"); return; }
            session->manifest = std::move(result.manifest);
            session->offered = true;
            if (!manual && busy_) {
                // 不打断正在进行的操作：状态栏提示，菜单里保留入口。
                status_->Text(L"发现新版本 " + update::Widen(session->manifest->version) + L"  ·  在“更多 → 发现新版本”中查看");
                return;
            }
            ShowUpdateOffer(manual);
        });
    }).detach();
}

void Application::ShowUpdateOffer(bool manual, int attempt) {
    auto& s = Updates();
    if (!s.manifest) return;
    if (window_.DialogActive()) {
        // 其它对话框 / 菜单正在关闭（有动画）：稍后重试，仍被占用就只在状态栏和菜单里提示。
        if (attempt < 10) window_.SetTimeout(.4f, [this, manual, attempt] { ShowUpdateOffer(manual, attempt + 1); });
        else status_->Text(L"发现新版本 " + update::Widen(s.manifest->version) + L"  ·  在“更多 → 发现新版本”中查看");
        return;
    }
    const auto& m = *s.manifest;
    s.kind = update::DetectInstallKind(ExeDir());
    const auto& asset = s.kind == update::InstallKind::Installer ? m.setup : m.portable;
    DialogSpec dialog;
    dialog.title = L"发现新版本 LumenPDF " + update::Widen(m.version);
    dialog.message = L"当前版本 " + update::Widen(update::kCurrentVersion) + (m.published.empty() ? L"" : L"  ·  发布于 " + update::Widen(m.published)) +
                     L"  ·  下载 " + update::Widen(update::FormatSize(asset.size)) + (s.kind == update::InstallKind::Installer ? L"（安装版）" : L"（便携版）");
    dialog.size = DialogSize::Standard;
    const std::wstring notes = update::Widen(m.notes.size() > 3000 ? m.notes.substr(0, 3000) + "…" : m.notes);
    const std::wstring page = update::Widen(m.page);
    dialog.content = [notes, page](Panel& panel) {
        if (!notes.empty()) { auto& text = panel.Add<Label>(notes, TextRole::Body); text.Wrap(true); }
        auto& hint = panel.Add<Label>(L"国内网络会自动测速并选择加速线路；下载后校验 SHA-256，安装前还会再询问你。", TextRole::Caption);
        hint.Secondary(true).Wrap(true);
        if (!page.empty()) {
            auto& link = panel.Add<Button>(L"在 GitHub 查看发布说明", ButtonKind::Subtle);
            link.Glyph(icon::kInfo).SizeClass(ButtonSize::Small).Role(TextRole::Caption);
            link.OnClick([page] { ShellExecuteW(nullptr, L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        }
    };
    dialog.primary = {L"下载并安装", {}};
    dialog.secondary = {L"跳过此版本", {}};
    dialog.close = {manual ? L"关闭" : L"以后再说", {}};
    auto session = updateSession_;
    dialog.on_result = [this, session](DialogResult result) {
        if (!session->manifest) return;
        if (result == DialogResult::Primary) window_.Dispatcher().Post([this] { StartUpdateDownload(); });
        else if (result == DialogResult::Secondary) {
            settings_.skipVersion = session->manifest->version;
            SaveSettings();
            session->offered = false;
            status_->Text(L"已跳过 " + update::Widen(session->manifest->version) + L"，仍可在“更多 → 检查更新”中手动升级");
        }
    };
    window_.ShowDialog(std::move(dialog));
}

void Application::StartUpdateDownload() {
    auto& s = Updates();
    if (!s.manifest || s.downloading || s.ready) return;
    s.downloading = true;
    s.cancel->store(false);
    s.progress = {};
    s.progress.probing = true;
    s.kind = update::DetectInstallKind(ExeDir());
    const auto manifest = *s.manifest;
    const auto asset = s.kind == update::InstallKind::Installer ? manifest.setup : manifest.portable;
    const auto folder = update::UpdatesRoot() / update::Widen(manifest.version);
    log::Info(L"开始下载升级 " + update::Widen(manifest.version) + (s.kind == update::InstallKind::Installer ? L"（安装版）" : L"（便携版）"));
    ShowUpdateProgress();
    auto post = window_.Dispatcher();
    auto alive = alive_;
    auto session = updateSession_;
    const bool portable = s.kind == update::InstallKind::Portable;
    std::thread([this, post, alive, session, manifest, asset, folder, portable] {
        auto result = update::DownloadAsset(asset, manifest.mirrors, folder, [&](const update::DownloadProgress& p) {
            post.Post([this, alive, session, p] {
                if (!alive->load()) return;
                session->progress = p;
                if (session->bar) { session->bar->Indeterminate(p.probing || p.verifying); if (!p.probing && !p.verifying) session->bar->Value(p.total ? static_cast<float>(p.done) / p.total : 0.f); }
                if (session->line) session->line->Text(ProgressText(p));
                else if (!busy_) status_->Text(L"正在下载升级 " + std::to_wstring(Percent(p)) + L"%  ·  " + SpeedText(p.speed));
            });
        }, session->cancel.get());
        fs::path staged;
        std::wstring error = result.error;
        if (error.empty() && portable) {
            post.Post([alive, session] {
                if (!alive->load()) return;
                if (session->bar) session->bar->Indeterminate(true);
                if (session->line) session->line->Text(L"SHA-256 校验通过，正在解压…");
            });
            staged = folder / L"staged";
            if (!update::ExtractZip(result.file, staged, error)) staged.clear();
        }
        post.Post([this, alive, session, result, staged, error] {
            if (!alive->load()) return;
            session->downloading = false;
            const bool dialog = session->bar != nullptr;
            session->bar = nullptr;
            session->line = nullptr;
            if (dialog) window_.CloseDialog();
            if (result.cancelled) { status_->Text(L"已取消下载升级（已下载部分会保留）"); log::Info(L"取消下载升级"); return; }
            if (!error.empty()) {
                log::Warn(L"下载升级失败：" + error);
                status_->Text(L"下载升级失败");
                std::wstring page = session->manifest ? update::Widen(session->manifest->page) : L"";
                window_.Dispatcher().Post([this, error, page] {
                    window_.Alert(L"下载升级失败", error + (page.empty() ? L"" : L"\n\n可以稍后重试，或手动下载：" + page));
                });
                return;
            }
            log::Info(L"升级已下载并校验：" + update::Widen(update::HostOf(result.source)));
            session->file = result.file;
            session->staged = staged;
            session->ready = true;
            session->offered = false;
            window_.Dispatcher().Post([this] { ConfirmUpdateInstall(); });
        });
    }).detach();
}

void Application::ShowUpdateProgress() {
    auto& s = Updates();
    if (!s.downloading || !s.manifest) return;
    if (window_.DialogActive()) return;
    DialogSpec dialog;
    dialog.title = L"正在下载 LumenPDF " + update::Widen(s.manifest->version);
    dialog.message = L"可以继续使用 LumenPDF；关闭此窗口后在后台下载。";
    dialog.size = DialogSize::Standard;
    auto session = updateSession_;
    dialog.content = [session](Panel& panel) {
        session->bar = &panel.Add<ProgressBar>();
        session->bar->Indeterminate(session->progress.probing || session->progress.verifying);
        session->bar->Value(session->progress.total ? static_cast<float>(session->progress.done) / session->progress.total : 0.f);
        session->line = &panel.Add<Label>(ProgressText(session->progress), TextRole::Caption);
        session->line->Secondary(true).Wrap(true);
    };
    dialog.primary = {L"后台下载", {}};
    dialog.close = {L"取消下载", {}};
    dialog.default_button = DialogCommand::Primary;
    dialog.on_result = [this, session](DialogResult result) {
        session->bar = nullptr;
        session->line = nullptr;
        if (result == DialogResult::Close && session->downloading) { session->cancel->store(true); status_->Text(L"正在取消下载…"); }
    };
    window_.ShowDialog(std::move(dialog));
}

void Application::ConfirmUpdateInstall(int attempt) {
    auto& s = Updates();
    if (!s.ready || !s.manifest) return;
    if (window_.DialogActive()) {
        if (attempt < 10) window_.SetTimeout(.4f, [this, attempt] { ConfirmUpdateInstall(attempt + 1); });
        else status_->Text(L"升级已下载  ·  在“更多”菜单中安装");
        return;
    }
    DialogSpec dialog;
    dialog.title = L"升级已就绪：LumenPDF " + update::Widen(s.manifest->version);
    dialog.message = L"SHA-256 校验通过。安装时 LumenPDF 会退出（未保存的文档会先逐份询问），完成后自动重新打开。";
    dialog.size = DialogSize::Standard;
    dialog.primary = {L"立即安装", {}};
    dialog.secondary = {L"退出时安装", {}};
    dialog.close = {L"暂不安装", {}};
    auto session = updateSession_;
    dialog.on_result = [this, session](DialogResult result) {
        if (result == DialogResult::Primary) {
            session->installOnExit = true;
            log::Info(L"用户确认立即安装升级");
            status_->Text(L"正在退出以安装升级…");
            window_.Dispatcher().Post([this] {
                window_.Close();
                // 若用户在保存确认中取消退出，升级保留到下次关闭时安装。
                window_.SetTimeout(1.5f, [this] { if (!closing_) status_->Text(L"升级将在关闭 LumenPDF 后自动安装"); });
            });
        } else if (result == DialogResult::Secondary) {
            session->installOnExit = true;
            status_->Text(L"升级将在关闭 LumenPDF 后自动安装");
        } else {
            session->installOnExit = false;
            status_->Text(L"升级已下载，可随时在“更多”菜单中安装");
        }
    };
    window_.ShowDialog(std::move(dialog));
}

void Application::RunPendingUpdate() {
    if (!updateSession_) return;
    auto& s = *updateSession_;
    if (!s.ready || !s.installOnExit) return;
    std::wstring error;
    const bool ok = s.kind == update::InstallKind::Installer ? update::LaunchInstaller(s.file, error) : update::LaunchPortableApply(s.staged, ExeDir(), error);
    if (!ok) {
        log::Error(L"启动升级失败：" + error);
        MessageBoxW(nullptr, (L"无法启动升级：" + error + L"\n\nLumenPDF 未作任何更改。").c_str(), L"LumenPDF", MB_ICONWARNING | MB_OK);
    }
}

void Application::ShowAbout() {
    if (window_.DialogActive()) return;
    DialogSpec dialog;
    dialog.title = L"关于 LumenPDF";
    dialog.message = L"版本 " + update::Widen(update::kCurrentVersion) + (update::DetectInstallKind(ExeDir()) == update::InstallKind::Installer ? L"（安装版）" : L"（便携版）") +
                     L"  ·  本地 PDF 阅读、批注与页面整理";
    dialog.size = DialogSize::Standard;
    dialog.content = [](Panel& panel) {
        auto& license = panel.Add<Label>(L"LumenPDF 是自由软件，按 GNU Affero 通用公共许可证第 3 版（AGPL-3.0）发布，源代码公开在 GitHub。"
                                         L"\n第三方组件：MuPDF 1.28.4（AGPL-3.0，Artifex）、LUMEN 界面库（MIT）、LumaText 文字排版（MIT）。许可文本随程序附在 licenses 与 notices 文件夹中。"
                                         L"\n\n联网说明：只有“检查更新”会访问网络（GitHub / jsDelivr / 下载加速镜像），只下载版本信息与安装包，不上传任何文档或使用数据。",
                                         TextRole::Caption);
        license.Wrap(true);
        auto& row = panel.Add<Row>();
        row.Spacing(8);
        auto& source = row.Add<Button>(L"源代码（GitHub）", ButtonKind::Standard);
        source.SizeClass(ButtonSize::Small).Role(TextRole::Caption);
        source.OnClick([] { ShellExecuteW(nullptr, L"open", L"https://github.com/jimmgreen/LumenPDF", nullptr, nullptr, SW_SHOWNORMAL); });
        auto& releases = row.Add<Button>(L"所有版本", ButtonKind::Subtle);
        releases.SizeClass(ButtonSize::Small).Role(TextRole::Caption);
        releases.OnClick([] { ShellExecuteW(nullptr, L"open", L"https://github.com/jimmgreen/LumenPDF/releases", nullptr, nullptr, SW_SHOWNORMAL); });
    };
    dialog.primary = {L"检查更新", {}};
    dialog.close = {L"关闭", {}};
    dialog.on_result = [this](DialogResult result) { if (result == DialogResult::Primary) window_.Dispatcher().Post([this] { CheckForUpdates(true); }); };
    window_.ShowDialog(std::move(dialog));
}
}
