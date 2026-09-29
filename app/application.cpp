#include "application.h"
#include "speech.h"
#include "shell_integration.h"
#include "app_log.h"
#include "ui.h"
#include "text_layout.h"
#include <lumen/ScrollViewer.h>
#include <lumen/ImageView.h>
#include <lumen/TitleBar.h>
#include <lumen/PasswordBox.h>
#include <lumen/Dialog.h>
#include <lumen/Splitter.h>
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <fstream>
#include <cmath>
#include <ctime>
namespace lpdf {
using namespace lumen;
namespace {
class DropColumn final:public Column{
public:std::function<void(std::vector<fs::path>)> dropped;
protected:
    bool AcceptsFileDrop()const noexcept override{return true;}
    std::vector<std::wstring> FilterFileDrop(std::vector<std::wstring> p)const override{return p;}
    void OnFileDrop(std::vector<std::wstring> p)override{std::vector<fs::path> files;for(auto&s:p)files.emplace_back(s);if(dropped)dropped(std::move(files));}
};
Button& Btn(Panel& parent,std::wstring_view text,std::function<void()> fn,ButtonKind kind=ButtonKind::Subtle){
    auto& button=parent.Add<Button>(text,kind).Height(34).SizeClass(ButtonSize::Small).Role(TextRole::Caption).Glyph(ui::Glyph(text)).ToolTip(text).AccessibleName(text).OnClick(std::move(fn));
    if(text==L"−"||text==L"＋"||text==L"上一处"||text==L"下一处"||text==L"撤销"||text==L"重做"||text==L"查找")button.Text(L"");
    return button;
}
void Width(Control& c,float w){c.MinSize({w,0});c.MaxSize({w,100000});}
// 阅读布局图标：与 Phosphor 同一套线宽与圆角（256 视图、16 线宽、外圈 16 圆角），单页 / 双页 / 网格一眼可分。
constexpr wchar_t kSinglePageIcon[]=L"\uF8B1";
constexpr wchar_t kTwoPageIcon[]=L"\uF8B2";
constexpr wchar_t kPageGridIcon[]=L"\uF8B3";
void RegisterLayoutIcons(){
    static const char single[]="M184,32H72A16,16,0,0,0,56,48V208a16,16,0,0,0,16,16H184a16,16,0,0,0,16-16V48A16,16,0,0,0,184,32Zm0,176H72V48H184Z"
        "M160,88H96a8,8,0,0,0,0,16h64a8,8,0,0,0,0-16Zm0,40H96a8,8,0,0,0,0,16h64a8,8,0,0,0,0-16Z";
    static const char two[]="M104,40H40A16,16,0,0,0,24,56V200a16,16,0,0,0,16,16h64a16,16,0,0,0,16-16V56A16,16,0,0,0,104,40Zm0,160H40V56h64Z"
        "M216,40H152a16,16,0,0,0-16,16V200a16,16,0,0,0,16,16h64a16,16,0,0,0,16-16V56A16,16,0,0,0,216,40Zm0,160H152V56h64Z";
    static const char grid[]="M104,32H48A16,16,0,0,0,32,48v56a16,16,0,0,0,16,16h56a16,16,0,0,0,16-16V48A16,16,0,0,0,104,32Zm0,72H48V48h56Z"
        "M208,32H152a16,16,0,0,0-16,16v56a16,16,0,0,0,16,16h56a16,16,0,0,0,16-16V48A16,16,0,0,0,208,32Zm0,72H152V48h56Z"
        "M104,136H48a16,16,0,0,0-16,16v56a16,16,0,0,0,16,16h56a16,16,0,0,0,16-16V152A16,16,0,0,0,104,136Zm0,72H48V152h56Z"
        "M208,136H152a16,16,0,0,0-16,16v56a16,16,0,0,0,16,16h56a16,16,0,0,0,16-16V152A16,16,0,0,0,208,136Zm0,72H152V152h56Z";
    icon::Register(kSinglePageIcon[0],single);icon::Register(kTwoPageIcon[0],two);icon::Register(kPageGridIcon[0],grid);
}
constexpr wchar_t AllFilter[]=L"支持的文件\0*.pdf;*.doc;*.docx;*.docm;*.rtf;*.odt;*.xls;*.xlsx;*.xlsm;*.xlsb;*.ods;*.ppt;*.pptx;*.pptm;*.pps;*.ppsx;*.odp;*.txt;*.png;*.jpg;*.jpeg\0PDF 文档\0*.pdf\0Office 文档\0*.doc;*.docx;*.docm;*.rtf;*.odt;*.xls;*.xlsx;*.xlsm;*.xlsb;*.ods;*.ppt;*.pptx;*.pptm;*.pps;*.ppsx;*.odp\0所有文件\0*.*\0";
constexpr wchar_t PdfFilter[]=L"PDF 文档\0*.pdf\0";
void Clipboard(HWND owner,const std::wstring& text){
    if(text.empty())return;
    const SIZE_T bytes=(text.size()+1)*sizeof(wchar_t);
    HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!data)throw std::runtime_error("Clipboard allocation failed");
    void* p=GlobalLock(data);if(!p){GlobalFree(data);throw std::runtime_error("Clipboard unavailable");}
    memcpy(p,text.c_str(),bytes);GlobalUnlock(data);
    if(!OpenClipboard(owner)){GlobalFree(data);throw std::runtime_error("Clipboard is busy");}
    EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,data))GlobalFree(data);CloseClipboard();
}
std::wstring ErrorText(const std::exception& e){try{return Wide(e.what());}catch(...){return L"文件操作失败，请检查文件路径、权限和可用空间。";}}

}
Application::Application():window_(WindowSpec{.title=L"LumenPDF",.size={1320,860},.backdrop=Backdrop::None}),worker_(std::make_shared<Worker>()){
    for(size_t i=0;i<toolStyles_.size();++i)toolStyles_[i]=DefaultAnnotationStyle(static_cast<Tool>(i));
    ui::RegisterIcons();window_.GlowIntensity(ui::GlowIntensity);window_.MinSize({1050,720});Build();
    PWSTR local=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))){
        recoveryRoot_=fs::path(local)/L"LumenPDF/Recovery";CoTaskMemFree(local);
        std::error_code error;fs::create_directories(recoveryRoot_,error);
        if(!error)recoveryFile_=UniquePath(recoveryRoot_,L".pdf");
    }
    window_.SetInterval(60,[this]{Backup();SavePosition();positions_.Save();});
    window_.SetInterval(1.5f,[this]{CheckExternalChange();});
    window_.SetInterval(1,[this]{
        for(auto it=retiredWorkers_.begin();it!=retiredWorkers_.end();){if(it->first->Finished()){std::error_code error;if(!it->second.empty())fs::remove(it->second,error);it=retiredWorkers_.erase(it);}else ++it;}
    });
    window_.OnClosing([this]{
        if(closing_)return true;
        if(window_.DialogActive())return false;
        if(textEditor_.Active()){FinishText(true,[this]{window_.Close();});return false;}
        if(busy_){window_.Alert(L"任务仍在进行",L"请等待当前任务完成，或先点击取消。完成后将逐份确认未保存的文档，不会丢弃其他标签页。");return false;}
        window_.Dispatcher().Post([this]{ConfirmWorkspaceExit();});return false;
    });
    window_.BindShortcut(L"Ctrl+O",[this]{ChooseOpen();});
    window_.BindShortcut(L"Ctrl+S",[this]{Save(false);});
    window_.BindShortcut(L"Ctrl+Shift+S",[this]{Save(true);});
    window_.BindShortcut(L"Ctrl+F",[this]{if(home_&&loaded_){recentFilter_->Focus();return;}Mode(0);search_->Focus();});
    window_.BindShortcut(L"Ctrl+Z",[this]{if(textEditor_.Active()){textEditor_.UndoText();return;}if(loaded_&&info_.canUndo&&!dynamic_cast<TextBox*>(window_.Focused()))Task(L"撤销",[](Engine&e,const Cancel&){e.document.Undo();});});
    window_.BindShortcut(L"Ctrl+Shift+Z",[this]{if(textEditor_.Active()){textEditor_.RedoText();return;}if(loaded_&&info_.canRedo&&!dynamic_cast<TextBox*>(window_.Focused()))Task(L"重做",[](Engine&e,const Cancel&){e.document.Redo();});});
    window_.BindShortcut(L"Ctrl+Y",[this]{if(textEditor_.Active()){textEditor_.RedoText();return;}if(loaded_&&info_.canRedo&&!dynamic_cast<TextBox*>(window_.Focused()))Task(L"重做",[](Engine&e,const Cancel&){e.document.Redo();});});
    // 阅读快捷键：只在阅读/批注/页面视图且没有文字编辑时生效。
    auto reading=[this]{return loaded_&&mode_!=3&&!busy_&&!textEditor_.Active()&&!window_.DialogActive();};
    auto focusedCanvas=[this]{return splitMode_&&window_.Focused()==referenceCanvas_?referenceCanvas_:canvas_;};
    window_.BindShortcut(L"Ctrl+0",[this,reading,focusedCanvas]{if(reading())focusedCanvas()->FitPage();});
    window_.BindShortcut(L"Ctrl+1",[this,reading,focusedCanvas]{if(reading())focusedCanvas()->ActualSize();});
    window_.BindShortcut(L"Ctrl+2",[this,reading,focusedCanvas]{if(reading())focusedCanvas()->FitWidth();});
    window_.BindShortcut(L"Ctrl+Plus",[this,reading,focusedCanvas]{if(reading())focusedCanvas()->Zoom(1.2f);});
    window_.BindShortcut(L"Ctrl+Minus",[this,reading,focusedCanvas]{if(reading())focusedCanvas()->Zoom(1/1.2f);});
    // 跳页沿用 Acrobat 的 Ctrl+Shift+N；Ctrl+G 在有搜索结果时与 Acrobat 一样跳到下一处，否则仍是跳页。
    auto goToPage=[this,reading,focusedCanvas]{if(reading()){pageBox_->Focus();pageBox_->Select(0,pageBox_->Text().size());}};
    window_.BindShortcut(L"Ctrl+Shift+N",goToPage);
    window_.BindShortcut(L"Ctrl+G",[this,goToPage]{if(loaded_&&!busy_&&!hits_.empty()&&mode_!=3)Hit(1);else goToPage();});
    window_.BindShortcut(L"Ctrl+Shift+G",[this]{if(loaded_&&!busy_&&!hits_.empty()&&mode_!=3)Hit(-1);});
    window_.BindShortcut(L"Ctrl+C",[this]{
        if(splitMode_&&window_.Focused()==referenceCanvas_){CopyReference(referenceCanvas_->Selected());return;}
        // 文本框有焦点时交给文本框；否则复制页面上选中的文字。
        if(dynamic_cast<TextBox*>(window_.Focused())||textEditor_.Active())return;
        if(loaded_&&mode_==0&&canvas_->HasTextSelection())CopySelection(canvas_->Selected());
    });
    window_.BindShortcut(L"Ctrl+Alt+N",[this,reading]{if(reading())SetTone(tone_==PageTone::Night?PageTone::Normal:PageTone::Night);});
    // Ctrl+Shift+D：页面视图里与 Acrobat 一样是“删除页面”（有确认、可撤销）；阅读视图仍切换单/双页。
    window_.BindShortcut(L"Ctrl+Shift+D",[this,reading]{if(!reading())return;if(mode_==2)DeleteSelectedPages();else ApplyLayout(settings_.layout==0?1:0);});
    window_.BindShortcut(L"Ctrl+Shift+T",[this,reading]{if(reading()&&!home_){int p=page_;Task(L"插入页面",[p](Engine&e,const Cancel&){e.document.InsertBlank(p);});}});
    window_.BindShortcut(L"Ctrl+D",[this]{if(loaded_&&!busy_&&!window_.DialogActive())DocumentProperties();});
    window_.BindShortcut(L"Ctrl+K",[this]{if(!window_.DialogActive())ShowSettings();});
    window_.BindShortcut(L"F11",[this]{FullScreen();});
    window_.BindShortcut(L"Ctrl+H",[this]{ReadingMode();});
    window_.BindShortcut(L"F5",[this,reading]{if(presenting_){Present(false);return;}if(reading()&&!home_)Present(true);});
    window_.BindShortcut(L"F4",[this,reading]{if(reading()&&!home_&&sidebar_)sidebar_->Visible(!sidebar_->Visible());});
    // Acrobat 单键工具：焦点在页面或按钮上时生效，输入框、列表里照常打字。
    auto singleKey=[this,reading]{
        if(!reading()||home_||mode_==2||presenting_||window_.PopupActive())return false;
        Control* focused=window_.Focused();
        return !focused||focused==canvas_||dynamic_cast<Button*>(focused)!=nullptr;
    };
    window_.BindShortcut(L"H",[this,singleKey]{if(singleKey())SetHand(true);});
    window_.BindShortcut(L"V",[this,singleKey]{if(singleKey()){SetHand(false);if(mode_==1)ChooseTool(Tool::Select);}});
    window_.BindShortcut(L"S",[this,singleKey]{if(singleKey())ToolKey(Tool::Note);});
    window_.BindShortcut(L"X",[this,singleKey]{if(singleKey())ToolKey(Tool::Text);});
    window_.BindShortcut(L"K",[this,singleKey]{if(singleKey())ToolKey(Tool::Stamp);});
    window_.BindShortcut(L"U",[this,singleKey]{if(singleKey())ToolKey(lastMarkup_);});
    window_.BindShortcut(L"D",[this,singleKey]{if(singleKey())ToolKey(lastDrawing_);});
    window_.BindShortcut(L"Shift+U",[this,singleKey]{if(singleKey())CycleTool({Tool::Highlight,Tool::Underline,Tool::StrikeOut},lastMarkup_);});
    window_.BindShortcut(L"Shift+D",[this,singleKey]{if(singleKey())CycleTool({Tool::Rectangle,Tool::Ellipse,Tool::Line,Tool::Arrow,Tool::Ink},lastDrawing_);});
    window_.BindShortcut(L"Ctrl+Shift+W",[this]{if(!LaunchNewWindow())Fail(L"无法启动新窗口。");});
    window_.BindShortcut(L"Ctrl+Tab",[this]{CycleDocument(1);});
    window_.BindShortcut(L"Ctrl+Shift+Tab",[this]{CycleDocument(-1);});
    window_.BindShortcut(L"Ctrl+W",[this]{if(activeDocument_)CloseDocument(activeDocument_->id);});
    window_.BindShortcut(L"Ctrl+N",[this]{NewDocument();});
    // Ctrl+V：截图 / 复制的图片 → 新建 PDF、图片批注或新页（文本框有焦点时照常粘贴文字）。
    window_.BindShortcut(L"Ctrl+V",[this]{PasteShortcut();});
    // 临时视图旋转（与常见阅读器一致）：Ctrl+Shift+= 顺时针，Ctrl+Shift+- 逆时针；只影响显示。
    window_.BindShortcut(L"Ctrl+Shift+Plus",[this]{RotateView(90);});
    window_.BindShortcut(L"Z",[this,singleKey]{if(singleKey())ToggleZoomBox();});
    window_.BindShortcut(L"Shift+Z",[this,singleKey]{if(singleKey())ZoomBackKey();});
    window_.BindShortcut(L"L",[this,singleKey]{if(singleKey())ToggleMagnifier();});
    // 朗读（与 Acrobat 相同）：Ctrl+Shift+V 本页、Ctrl+Shift+B 到文末、Ctrl+Shift+C 暂停 / 继续、Ctrl+Shift+E 停止。
    window_.BindShortcut(L"Ctrl+Shift+V",[this]{if(!dynamic_cast<TextBox*>(window_.Focused()))ReadAloud(false);});
    window_.BindShortcut(L"Ctrl+Shift+B",[this]{if(!dynamic_cast<TextBox*>(window_.Focused()))ReadAloud(true);});
    window_.BindShortcut(L"Ctrl+Shift+C",[this]{ReadPause();});
    window_.BindShortcut(L"Ctrl+Shift+E",[this]{StopReading();});
    window_.BindShortcut(L"Ctrl+Shift+Minus",[this]{RotateView(-90);});
    window_.BindShortcut(L"Ctrl+Comma",[this]{ShowSettings();});
    window_.BindShortcut(L"Ctrl+P",[this,reading]{if(reading())Print();});
    window_.BindShortcut(L"Alt+Left",[this,reading]{if(reading())Navigate(-1);});
    window_.BindShortcut(L"Alt+Right",[this,reading]{if(reading())Navigate(1);});
    window_.BindShortcut(L"Delete",[this]{if(mode_==3&&!busy_&&window_.Focused()==queueList_&&!window_.DialogActive())RemoveQueueItem();});
    window_.BindShortcut(L"F3",[this]{if(loaded_&&!busy_&&!hits_.empty())Hit(1);});
    window_.BindShortcut(L"Shift+F3",[this]{if(loaded_&&!busy_&&!hits_.empty())Hit(-1);});
    window_.OnNativeMessage([this](uint32_t msg,std::uintptr_t w,std::intptr_t l){
        if((msg==WM_MOVE||msg==WM_SIZE)&&canvas_&&textEditor_.Active())canvas_->RepositionEditor();
        // 空格按住/松开：控件树只有按下事件，松开从原生消息得知。按下先于分发，可把焦点从按钮移到页面。
        if(canvas_&&w==VK_SPACE&&(msg==WM_KEYDOWN||msg==WM_KEYUP))SpaceKey(msg==WM_KEYDOWN,(l&(1<<30))!=0);
        if(canvas_&&(msg==WM_KILLFOCUS||(msg==WM_ACTIVATE&&LOWORD(w)==WA_INACTIVE)))canvas_->SpaceHeld(false);
    });
    window_.BindShortcut(L"Escape",[this]{
        if(presenting_&&!window_.DialogActive()&&!window_.PopupActive()){Present(false);return;}
        if(aloud_.active&&!window_.DialogActive()&&!window_.PopupActive()){StopReading();return;}
        if(!window_.DialogActive()&&!window_.PopupActive()){if(fullScreen_){FullScreen();return;}if(readingMode_){ReadingMode();return;}if(autoScroll_){autoScroll_=false;status_->Text(L"已停止自动滚动");return;}}
        // 拖动页面时 Esc 只取消拖动（页面平滑退回），不再落到切换工具等其它处理。
        if(canvas_->PageDragging()){canvas_->CancelPageDrag();return;}
        if(thumbs_->PageDragging()){thumbs_->CancelPageDrag();return;}
        if(mode_==2&&!window_.DialogActive()&&!window_.PopupActive()&&canvas_->SelectedPages().size()>1){canvas_->SelectedPages({page_});PageSelectionHint(1);return;}
        if(textEditor_.Active()){FinishText(false);return;}if(canvas_->HasTextSelection()){canvas_->ClearTextSelection();status_->Text(L"已取消文字选择");return;}if(redacting_&&!window_.DialogActive()&&!window_.PopupActive()){ExitRedaction(false);return;}if(InPreview()&&mode_==0&&!home_&&!window_.DialogActive()&&!window_.PopupActive()&&annotation_<0){ClosePreview(true);return;}if(home_&&loaded_&&mode_!=3){Home(false);return;}ChooseTool(Tool::Select);});
}
Application::~Application(){
    speech_.reset();   // 先停语音线程，之后不再有回调投递
    try{SavePosition();CaptureDocument();SaveReferencePosition();positions_.Save();}catch(...){}
    alive_->store(false);if(cancel_)cancel_->store(true);if(searchCancel_)searchCancel_->store(true);++*generation_;++*referenceGeneration_;
    for(auto& doc:documents_)doc->worker->Stop();if(worker_)worker_->Stop();worker_.reset();activeDocument_.reset();
    for(auto& doc:documents_){doc->worker.reset();std::error_code error;if(!doc->recovery.empty())fs::remove(doc->recovery,error);}documents_.clear();
    for(auto& retired:retiredWorkers_){retired.first.reset();std::error_code error;if(!retired.second.empty())fs::remove(retired.second,error);}retiredWorkers_.clear();
}
void Application::ApplyIcon(){
    // 任务栏 / Alt+Tab：从 ICON 资源按当前 DPI 取最合适的尺寸（资源内含 16–256 px）。
    // 句柄在 Show() 后才存在；图标随进程存活，不需释放。
    auto hwnd=static_cast<HWND>(window_.NativeHandle());if(!hwnd)return;
    const HINSTANCE module=GetModuleHandleW(nullptr);const UINT dpi=GetDpiForWindow(hwnd);
    auto load=[&](int metric){const int size=GetSystemMetricsForDpi(metric,dpi);
        return static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(1),IMAGE_ICON,size,size,LR_DEFAULTCOLOR|LR_SHARED));};
    if(HICON big=load(SM_CXICON))SendMessageW(hwnd,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(big));
    if(HICON little=load(SM_CXSMICON))SendMessageW(hwnd,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(little));
    // 自绘标题栏：用同一 ICO 的原始字节（WIC 解码最大帧后缩放绘制）。
    if(auto* bar=window_.TitleBar())
        if(HRSRC res=FindResourceW(module,MAKEINTRESOURCEW(2),RT_RCDATA))
            if(HGLOBAL mem=LoadResource(module,res))
                if(const void* bytes=LockResource(mem))
                    bar->LoadIconMemory({static_cast<const std::byte*>(bytes),static_cast<size_t>(SizeofResource(module,res))});
}
int Application::Run(const fs::path& initial,bool smoke,const fs::path& mergeFile){
    std::jthread workspaceClock;
    if(smoke&&GetEnvironmentVariableW(L"LPDF_WORKSPACE_TEST",nullptr,0)){
        recoveryRoot_=fs::absolute(L"workspace-test-output/recovery");fs::create_directories(recoveryRoot_);recoveryFile_.clear();
    }
    smoke_=smoke;LoadRecent();log::InstallCrashHandler();log::Info(L"启动 LumenPDF " LPDF_VERSION_W);window_.Show();ApplyIcon();
    if(!smoke_)RepairShell();
    ScheduleUpdateCheck();
    if(!mergeFile.empty())AcceptMerge(mergeFile);
    else if(!initial.empty())window_.SetTimeout(.1f,[this,initial]{Open(initial);});
    else if(!smoke_&&!recoveryRoot_.empty()){
        std::error_code error;std::vector<std::pair<fs::file_time_type,fs::path>> backups;
        for(const auto& entry:fs::directory_iterator(recoveryRoot_,error))if(entry.path().extension()==L".pdf"){
            auto modified=entry.last_write_time(error);if(!error)backups.push_back({modified,entry.path()});
        }
        std::sort(backups.begin(),backups.end(),[](const auto& a,const auto& b){return a.first>b.first;});
        std::vector<fs::path> recover;for(size_t i=0;i<std::min<size_t>(64,backups.size());++i)recover.push_back(backups[i].second);
        if(!recover.empty())window_.SetTimeout(.2f,[this,recover]{window_.Confirm(L"发现未完成的编辑备份",L"将最近的 "+std::to_wstring(recover.size())+L" 份自动备份恢复为独立标签页？恢复后请逐份另存为。",[this,recover](bool yes){if(yes)OpenFiles(recover);},L"恢复全部",L"暂不恢复");});
    }
    // 只主动询问一次：不是默认阅读器时，启动后提示设为默认。
    // LPDF_NO_DEFAULT_PROMPT：自动化测试以正常模式启动时不弹出、也不记为已询问。
    if(!smoke_&&!settings_.askedDefault&&!GetEnvironmentVariableW(L"LPDF_NO_DEFAULT_PROMPT",nullptr,0)&&!IsDefaultPdfHandler()&&!settingsFile_.empty()){
        settings_.askedDefault=true;SaveSettings();
        window_.SetTimeout(1.2f,[this]{
            if(window_.DialogActive()||busy_)return;
            window_.Confirm(L"将 LumenPDF 设为默认 PDF 阅读器？",L"之后双击 PDF 文件会直接用 LumenPDF 打开。Windows 会打开系统“默认应用”设置，在 “.pdf” 一项中选择 LumenPDF 即可。也可以稍后在“更多”菜单中设置。",
                [this](bool yes){if(yes)window_.Dispatcher().Post([this]{MakeDefault();});},L"设为默认",L"以后再说");
        });
    }
    if(smoke_){
        if(GetEnvironmentVariableW(L"LPDF_WORKSPACE_TEST",nullptr,0)){
            // WM_TIMER can be postponed by continuous dialog animation; drive only
            // the test harness with ordinary dispatcher messages, like worker completions.
            auto post=window_.Dispatcher();auto alive=alive_;
            workspaceClock=std::jthread([this,post,alive](std::stop_token stop){
                while(!stop.stop_requested()){
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    if(!stop.stop_requested())post.Post([this,alive]{if(alive->load())WorkspaceSmokeTick();});
                }
            });
        }
        wchar_t dimensions[40]{};GetEnvironmentVariableW(L"LPDF_SMOKE_SIZE",dimensions,40);int width=0,height=0;
        if(swscanf_s(dimensions,L"%dx%d",&width,&height)==2&&width>=1050&&height>=720){
            auto hwnd=static_cast<HWND>(window_.NativeHandle());float scale=GetDpiForWindow(hwnd)/96.0f;
            SetWindowPos(hwnd,nullptr,0,0,static_cast<int>(width*scale),static_cast<int>(height*scale),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        window_.SetTimeout(3,[this,initial]{
            wchar_t value[8]{};GetEnvironmentVariableW(L"LPDF_SMOKE_MODE",value,8);int mode=std::clamp(_wtoi(value),0,3);
            if(mode==3&&!initial.empty())Queue({initial,initial.parent_path()/L"Word转换.pdf"});else Mode(mode);
            if(mode==1&&!annotations_.empty()){Select(page_,annotations_.front().id);canvas_->Selection(page_,annotations_,annotation_);}
            wchar_t dialogName[16]{};if(GetEnvironmentVariableW(L"LPDF_SMOKE_DIALOG",dialogName,16)){const std::wstring d=dialogName;if(d==L"settings")ShowSettings();else if(d==L"security")ShowSecurityDialog();else if(d==L"more")ShowMoreMenu();}
            wchar_t side[8]{};if(GetEnvironmentVariableW(L"LPDF_SMOKE_SIDE",side,8)){const int n=std::clamp(_wtoi(side),0,2);sideTabs_->SelectedIndex(n);SidePanel(n);}
        });
        // 签名冒烟：LPDF_SIGNATURE_SMOKE 指定输出文件时，走一遍签名 / 日期 / ✓ / ✗ 的放置链路后另存并退出。
        wchar_t signatureTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_SIGNATURE_SMOKE",signatureTest,MAX_PATH))window_.SetTimeout(3.5f,[this,output=fs::path(signatureTest)]{SignatureSmoke(output,0);});
        // 表单冒烟：LPDF_FORM_SMOKE 指定输出文件时，走一遍 复选 / 单选 / 下拉 / 文本 / 下一项 的填写链路后另存并退出。
        wchar_t formTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_FORM_SMOKE",formTest,MAX_PATH))window_.SetTimeout(3.5f,[this,output=fs::path(formTest)]{FormSmoke(output,0);});
        // 涂黑冒烟：LPDF_REDACT_SMOKE 指定输出文件时，搜索 → 标记 → 应用并另存后退出。
        wchar_t redactTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_REDACT_SMOKE",redactTest,MAX_PATH))window_.SetTimeout(3.5f,[this,output=fs::path(redactTest)]{RedactionSmoke(output,0);});
        // 打印预览冒烟：LPDF_PRINT_PREVIEW_SMOKE 指定输出目录，检查各种排版的预览后打印到 PDF 并退出。
        wchar_t previewTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_PRINT_PREVIEW_SMOKE",previewTest,MAX_PATH))window_.SetTimeout(3.5f,[this,dir=fs::path(previewTest)]{PrintPreviewSmoke(dir,0);});
        // 剪贴板冒烟：LPDF_CLIPBOARD_SMOKE 指定输出目录（测试程序事先把位图放到剪贴板）。
        wchar_t clipTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_CLIPBOARD_SMOKE",clipTest,MAX_PATH))window_.SetTimeout(3.5f,[this,dir=fs::path(clipTest)]{ClipboardSmoke(dir,0);});
        wchar_t rotateTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_ROTATE_SMOKE",rotateTest,MAX_PATH))window_.SetTimeout(3.5f,[this,dir=fs::path(rotateTest)]{RotationSmoke(dir,0);});
        wchar_t zoomTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_ZOOM_SMOKE",zoomTest,MAX_PATH))window_.SetTimeout(3.5f,[this,dir=fs::path(zoomTest)]{ZoomSmoke(dir,0);});
        wchar_t speechTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_SPEECH_SMOKE",speechTest,MAX_PATH))window_.SetTimeout(3.5f,[this,dir=fs::path(speechTest)]{SpeechSmoke(dir,0);});
        // 打印测试：LPDF_PRINT_TO 指定输出文件时，载入后自动打印全部页面到该文件。
        wchar_t printTest[MAX_PATH]{};
        if(GetEnvironmentVariableW(L"LPDF_PRINT_TO",printTest,MAX_PATH))window_.SetTimeout(4,[this]{if(loaded_&&!busy_)Print();});
        wchar_t closeTest[8]{};
        if(GetEnvironmentVariableW(L"LPDF_SMOKE_CLOSE",closeTest,8)){
            window_.SetTimeout(4,[this]{
                if(loaded_&&!busy_)Task(L"退出测试",[](Engine&e,const Cancel&){e.document.MarkDirty();},
                    [this]{window_.Close();});
            });
        }
        wchar_t timeoutText[16]{};float timeout=15;
        if(GetEnvironmentVariableW(L"LPDF_SMOKE_TIMEOUT",timeoutText,16))timeout=std::clamp(static_cast<float>(_wtof(timeoutText)),5.0f,120.0f);
        window_.SetTimeout(timeout,[this]{if(GetEnvironmentVariableW(L"LPDF_WORKSPACE_TEST",nullptr,0)&&workspaceSmokeStage_<100)workspaceSmokeExit_=2;closing_=true;window_.Close();});
    }
    const int result=app_.Run();workspaceClock.request_stop();
    RunPendingUpdate();   // 用户确认过的升级：窗口关闭（未保存文档已逐份确认）后才启动安装
    return workspaceSmokeExit_?workspaceSmokeExit_:result;
}
void Application::Build(){
    auto& root=window_.Root();root.Padding(16,12);root.Spacing(12);root.Background(Color::Hex(0x0d0d0d));
    ThemeOverride compact{};compact.input_height=34;compact.radius_control=6;compact.list_row_height=44;root.Style(compact);
    documentBar_=&root.Add<Row>();documentBar_->Spacing(6);documentBar_->Visible(false);
    documentTabs_=&documentBar_->Add<DocumentTabs>();documentTabs_->Grow();documentTabs_->MinSize({0,40});documentTabs_->MaxSize({100000,40});documentTabs_->Role(TextRole::Caption).SelectedRole(TextRole::CaptionStrong);
    documentTabs_->OnSelectionChanged([this](ptrdiff_t,ptrdiff_t){if(syncingTabs_)return;const auto id=documentTabs_->SelectedId();uint64_t target=0;try{target=std::stoull(id);}catch(...){return;}if(activeDocument_&&target!=activeDocument_->id){syncingTabs_=true;documentTabs_->SelectedId(std::to_wstring(activeDocument_->id));syncingTabs_=false;}window_.Dispatcher().Post([this,target]{ActivateDocument(target);});});
    documentTabs_->OnTabClosing([this](std::wstring_view id){if(syncingTabs_)return true;uint64_t target=0;try{target=std::stoull(std::wstring(id));}catch(...){return false;}window_.Dispatcher().Post([this,target]{CloseDocument(target);});return false;});
    Btn(*documentBar_,L"＋",[this]{ChooseOpen();}).ToolTip(L"打开文件到新标签页 · Ctrl+O");
    splitButton_=&Btn(*documentBar_,L"分屏",[this]{ShowSplitMenu();});
    header_=&root.Add<Row>();header_->Spacing(6);header_->AlignCross(CrossAlign::Center);
    const wchar_t* labels[]{L"阅读",L"批注",L"页面",L"合并"};
    for(int i=0;i<4;++i){
        auto& tab=header_->Add<ui::Tab>(labels[i]);tab.Glyph(ui::Glyph(labels[i])).Height(38).SizeClass(ButtonSize::Small).OnClick([this,i]{home_=false;Mode(i);SetBusy(busy_);});
        modes_.push_back(&tab);
    }
    titleSep_=&header_->Add<Label>(L" / ",TextRole::Caption);titleSep_->Secondary(true).Margin(6,0);
    name_=&header_->Add<Label>(L"",TextRole::CaptionStrong);name_->AccessibleName(L"当前文件");
    dirtyMark_=&header_->Add<Label>(L"",TextRole::Caption);dirtyMark_->Foreground(Color::Hex(0xf0b44c)).Margin(6,0);
    header_->Add<Label>().Grow();
    homeButton_=&Btn(*header_,L"主页",[this]{Home(!home_);});homeButton_->Glyph(icon::kHome).Text(L"").ToolTip(L"主页 · 最近打开").AccessibleName(L"主页");
    undo_=&Btn(*header_,L"撤销",[this]{if(info_.canUndo)Task(L"撤销",[](Engine&e,const Cancel&){e.document.Undo();});});
    redo_=&Btn(*header_,L"重做",[this]{if(info_.canRedo)Task(L"重做",[](Engine&e,const Cancel&){e.document.Redo();});});
    Btn(*header_,L"打开",[this]{ChooseOpen();},ButtonKind::Standard);
    Btn(*header_,L"新建",[this]{NewDocument();});
    moreButton_=&Btn(*header_,L"更多",[this]{ShowMoreMenu();});moreButton_->Glyph(icon::kMore).Text(L"").ToolTip(L"更多：新窗口、导出、加密、设置").AccessibleName(L"更多");
    save_=&Btn(*header_,L"另存为",[this]{Save();},ButtonKind::Primary);
    tools_=&root.Add<Row>();tools_->Spacing(8);
    readerTools_=&tools_->Add<Row>();readerTools_->Spacing(6);readerTools_->Grow();
    handButton_=&Btn(*readerTools_,L"手型",[this]{SetHand(!canvas_->HandTool());});
    handButton_->ToolTip(L"手型工具：拖动平移页面  ·  H；按住空格临时使用，V 返回选择").AccessibleName(L"手型工具");
    Btn(*readerTools_,L"适合页面",[this]{canvas_->FitPage();}).ToolTip(L"适合页面  ·  Ctrl+0");
    Btn(*readerTools_,L"适合宽度",[this]{canvas_->FitWidth();}).ToolTip(L"适合宽度  ·  Ctrl+2");
    toneButton_=&Btn(*readerTools_,L"页面配色",[this]{ShowToneMenu();});
    toneButton_->ToolTip(L"页面配色：普通 / 护眼 / 夜间  ·  Ctrl+Alt+N 切换夜间");
    ViewSwitch(*readerTools_,0);
    Btn(*readerTools_,L"复制本页",[this]{Copy(page_);});
    Btn(*readerTools_,L"打印",[this]{Print();}).ToolTip(L"打印预览与打印  ·  Ctrl+P");
    readerTools_->Add<Label>().Grow();
    search_=&readerTools_->Add<TextBox>().Role(TextRole::Caption).Glyph(icon::kSearch).Placeholder(L"搜索文档  ·  Ctrl+F").OnSubmit([this]{Search();});Width(*search_,260);
    search_->ToolTip(L"回车搜索；再按回车跳到下一处，Shift+回车上一处");
    search_->OnTextChanged([this](std::wstring_view text){
        if(searchCancel_)searchCancel_->store(true);
        if(searchResults_)searchResults_->ItemCount(0,false);
        // 改了关键字：旧结果不再对应，清掉高亮与计数，提示重新回车。
        if(!hits_.empty()&&text!=lastQuery_){hits_.clear();hit_=-1;canvas_->SearchResults({},-1);SyncSearchButtons();}
        hitLabel_->Text(text.empty()||text==lastQuery_?hitLabel_->Text():L"回车搜索");if(text.empty())hitLabel_->Text(L"");
    });
    Btn(*readerTools_,L"搜索选项",[this]{SearchSettings();}).Text(L"").Glyph(icon::kSettings);
    Btn(*readerTools_,L"查找",[this]{Search();}).ToolTip(L"查找  ·  Enter");
    prevHit_=&Btn(*readerTools_,L"上一处",[this]{Hit(-1);});prevHit_->ToolTip(L"上一处  ·  Shift+F3");
    nextHit_=&Btn(*readerTools_,L"下一处",[this]{Hit(1);});nextHit_->ToolTip(L"下一处  ·  F3");
    hitLabel_=&readerTools_->Add<Label>(L"",TextRole::Caption).Secondary(true).AccessibleName(L"搜索结果位置");hitLabel_->MinSize({0,34});
    annotationTools_=&tools_->Add<Row>();annotationTools_->Spacing(4);annotationTools_->Grow();
    // 常用工具带文字；新增的标记与形状工具只显示图标（悬停提示名称），保证窄窗口下工具栏不溢出。
    const std::pair<Tool,bool> items[]={{Tool::Select,true},{Tool::Text,true},{Tool::Note,true},{Tool::Highlight,true},{Tool::Underline,false},{Tool::StrikeOut,false},
        {Tool::Rectangle,true},{Tool::Ellipse,false},{Tool::Arrow,true},{Tool::Line,false},{Tool::Ink,true},{Tool::Image,true},{Tool::Stamp,false}};
    for(auto [tool,labelled]:items){
        const std::wstring label=ui::ToolName(tool);
        auto& button=Btn(*annotationTools_,label,[this,tool]{SetHand(false);ChooseTool(tool);});
        button.Glyph(ui::ToolGlyph(tool));
        const wchar_t* key=tool==Tool::Select?L"V":tool==Tool::Text?L"X":tool==Tool::Note?L"S":tool==Tool::Stamp?L"K":tool==Tool::Highlight?L"U":
            IsMarkupTool(tool)?L"Shift+U 切换":tool==Tool::Image?nullptr:L"D / Shift+D 切换";
        if(key)button.ToolTip(label+L"  ·  "+key);
        if(!labelled)button.Text(L"");
        toolButtons_.push_back({&button,tool});
    }
    signButton_=&Btn(*annotationTools_,L"签名",[this]{SetHand(false);SignatureMenu();});
    signButton_->Glyph(ui::Sign).ToolTip(L"填写与签名：签名库、日期、✓ / ✗");
    annotationTools_->Add<Label>().Grow();
    finishText_=&Btn(*annotationTools_,L"完成",[this]{FinishText(true);},ButtonKind::Primary);
    cancelText_=&Btn(*annotationTools_,L"取消",[this]{FinishText(false);});
    finishText_->Visible(false);cancelText_->Visible(false);
    pageTools_=&tools_->Add<Row>();pageTools_->Spacing(6);pageTools_->Grow();
    Btn(*pageTools_,L"更多页面工具",[this]{PageToolsMenu();}).Glyph(icon::kMore);
    Btn(*pageTools_,L"旋转 90°",[this]{auto pages=SelectedPages();Task(L"旋转页面",[pages](Engine&e,const Cancel&){e.document.RotatePages(pages,90);});});
    Btn(*pageTools_,L"插入空白页",[this]{int p=page_;Task(L"插入页面",[p](Engine&e,const Cancel&){e.document.InsertBlank(p);});});
    Btn(*pageTools_,L"插入 PDF",[this]{auto files=Pick(false,PdfFilter);if(files.empty())return;int p=page_;auto file=files[0];
        window_.Confirm(L"插入 PDF 页面",L"将插入到当前页之后；来源批注和表单的可见外观会固化。",[this,p,file](bool yes){if(yes)Task(L"插入 PDF",[p,file](Engine&e,const Cancel&){e.document.InsertPdf(p,file);});},L"插入",L"取消");
    });
    Btn(*pageTools_,L"提取选中页",[this]{auto pages=SelectedPages();auto path=Destination(L"提取页面.pdf");if(!path.empty())Task(L"提取页面",[pages,path](Engine&e,const Cancel&){e.document.Extract(pages,path);});});
    Btn(*pageTools_,L"删除选中页",[this]{DeleteSelectedPages();}).ToolTip(L"删除选中页  ·  Delete（可撤销）");
    pageHint_=&pageTools_->Add<Label>(L"",TextRole::Caption);pageHint_->Secondary(true).Margin(12,0);pageHint_->Grow();PageSelectionHint(1);
    ViewSwitch(*pageTools_,1);
    // 主页：左侧为主要操作（品牌只在标题栏出现一次），右侧为最近打开（继续阅读卡片 + 按时间分组的列表）。
    auto& welcome=root.Add<DropColumn>();welcomePane_=&welcome;welcome.Grow();welcome.Spacing(0);
    welcome.dropped=[this](std::vector<fs::path> files){OpenFiles(std::move(files));};
    auto& home=welcome.Add<Row>();home.Grow();home.Spacing(28);home.Padding(8,4);
    auto& hero=home.Add<Column>();Width(hero,300);hero.Spacing(12);hero.Padding(24,28);
    hero.Card(Panel::CardStyle::Subtle,16);
    // 标题栏已有图标与“LumenPDF”，这里不再重复品牌：与右侧“最近打开”对称，直接给出开始操作。
    hero.Add<Label>(L"开始",TextRole::Title).MinSize({0,32});
    hero.Add<Label>(L"打开、合并或新建 PDF",TextRole::Caption).Secondary(true).Wrap(true).MinSize({0,18});
    auto& heroActions=hero.Add<Column>();heroActions.Spacing(8);heroActions.Margin(0,6);
    Btn(heroActions,L"打开文档",[this]{ChooseOpen();},ButtonKind::Primary).Height(42).Role(TextRole::BodyStrong).ToolTip(L"打开文档  ·  Ctrl+O");
    Btn(heroActions,L"合并文件",[this]{Mode(3);},ButtonKind::Standard).Glyph(ui::Merge).Height(40).Role(TextRole::Body);
    Btn(heroActions,L"新建空白 PDF",[this]{NewDocument();}).Glyph(icon::kAdd).Height(40).Role(TextRole::Body);
    Btn(heroActions,L"从剪贴板新建",[this]{PasteFromClipboard(PasteIntent::NewDocument);}).Glyph(icon::kImage).Height(40).Role(TextRole::Body).ToolTip(L"用截图或复制的图片新建 PDF  ·  Ctrl+V");
    hero.Add<Label>().Grow();
    defaultApp_=&Btn(hero,L"设为默认 PDF 阅读器",[this]{MakeDefault();});
    defaultApp_->Glyph(icon::kPin).ToolTip(L"双击 PDF 时用 LumenPDF 打开。Windows 需要你在“默认应用”中确认").Margin(0,4);
    auto& dropHint=hero.Add<Column>();dropHint.Spacing(4);dropHint.Padding(14,12);dropHint.Card(Panel::CardStyle::Input,10);
    auto& dropRow=dropHint.Add<Row>();dropRow.Spacing(8);dropRow.AlignCross(CrossAlign::Center);
    dropRow.Add<IconView>(icon::kUpload).Box(20).IconSize(13);
    dropRow.Add<Label>(L"拖入文件即可打开",TextRole::CaptionStrong);
    dropHint.Add<Label>(L"多个文件在同一窗口的标签页中打开",TextRole::Caption).Secondary(true);
    // 隐私说明原在品牌简介里，保留为底部一行小字。
    // 左栏内容宽约 252 px：文字占满剩余宽度并自动换行，不能超出卡片。
    auto& footnote=hero.Add<Row>();footnote.Spacing(6);footnote.AlignCross(CrossAlign::Start);footnote.Margin(0,4);
    footnote.Add<IconView>(icon::kShield).Box(16).IconSize(11);
    auto& footnoteText=footnote.Add<Label>(L"文件只在本机处理 · 支持 PDF、Word、Excel、PPT、TXT、图片",TextRole::Caption);
    footnoteText.Foreground(Color::Hex(0x6a6a6a)).Wrap(true);footnoteText.Grow();
    auto& recentColumn=home.Add<Column>();recentColumn.Grow();recentColumn.Spacing(14);recentColumn.Padding(0,8);
    auto& recentHeader=recentColumn.Add<Row>();recentHeader.Spacing(10);recentHeader.AlignCross(CrossAlign::Center);
    auto& titles=recentHeader.Add<Column>();titles.Grow();titles.Spacing(2);
    titles.Add<Label>(L"最近打开",TextRole::Title).MinSize({0,32});
    recentCount_=&titles.Add<Label>(L"",TextRole::Caption);recentCount_->Secondary(true).MinSize({0,18});
    recentFilter_=&recentHeader.Add<TextBox>().Role(TextRole::Caption).Glyph(icon::kSearch).Placeholder(L"筛选文件名或文件夹").AccessibleName(L"筛选最近打开的文件");Width(*recentFilter_,240);
    recentFilter_->OnTextChanged([this](std::wstring_view value){recentView_->Filter(std::wstring(value));});
    clearRecent_=&Btn(recentHeader,L"清除历史",[this]{
        window_.Confirm(L"清除最近打开记录？",L"已固定的文件会保留，文件本身不受影响。",[this](bool yes){if(yes){recent_.Clear(true);recent_.Save();RefreshRecent();}},L"清除",L"取消");
    });
    recentView_=&recentColumn.Add<RecentView>();recentView_->Grow();
    recentView_->open=[this](const fs::path& path){
        if(loaded_&&!source_.empty()&&RecentFiles::SamePath(path,source_)){Home(false);return;}
        std::error_code error;if(!fs::exists(path,error)){ForgetRecent(path);return;}
        Open(path);
    };
    recentView_->browse=[this]{ChooseOpen();};
    recentView_->remove=[this](const fs::path& path){recent_.Remove(path);recent_.Save();RefreshRecent();};
    recentView_->pin=[this](const fs::path& path,bool pinned){recent_.Pin(path,pinned);recent_.Save();RefreshRecent();};
    recentView_->reveal=[](const fs::path& path){
        if(PIDLIST_ABSOLUTE item=ILCreateFromPathW(path.c_str())){SHOpenFolderAndSelectItems(item,0,nullptr,0);ILFree(item);}
    };
    recentView_->copy_path=[this](const fs::path& path){try{Clipboard(static_cast<HWND>(window_.NativeHandle()),path.wstring());status_->Text(L"已复制路径");}catch(const std::exception& e){Fail(ErrorText(e));}};
    recentView_->request_info=[this](const fs::path& path,int64_t opened){
        auto post=window_.Dispatcher();auto alive=alive_;
        // 低优先级：只做存在性、大小与首页缩略图；用独立的 Document，不影响当前打开的文档。
        worker_->Submit([this,post,alive,path,opened](Engine&){
            if(!alive->load())return;
            RecentInfo info;std::error_code error;
            const auto status=fs::status(path,error);
            if(error||!fs::is_regular_file(status)){info.state=RecentInfo::State::Missing;}
            else{
                info.state=RecentInfo::State::Present;info.bytes=fs::file_size(path,error);
                if(FileKind(path)==InputFileKind::Pdf&&info.bytes<256ull*1024*1024){
                    try{
                        Document preview;preview.Open(path);
                        const auto meta=preview.Info();info.pages=static_cast<int>(meta.pages.size());
                        if(!meta.pages.empty()){
                            const auto& first=meta.pages.front();
                            const float scale=std::clamp(std::min(360.0f/std::max(1.0f,first.width),300.0f/std::max(1.0f,first.height)),.05f,2.0f);
                            info.thumb=preview.Render(0,scale);
                        }
                    }catch(const PasswordRequired&){info.locked=true;}
                    catch(...){/* 损坏或暂不支持的文件：仅显示图标与大小。 */}
                }
            }
            post.Post([this,alive,path,opened,info=std::move(info)]()mutable{if(alive->load())recentView_->AcceptInfo(path,opened,std::move(info));});
        },false);
    };
    readPane_=&root.Add<Column>();readPane_->Grow();readPane_->Spacing(8);
    // 合并预览提示条：说明当前是临时预览，并提供保存与结束预览的入口。
    previewBar_=&readPane_->Add<Row>();previewBar_->Spacing(8);previewBar_->AlignCross(CrossAlign::Center);previewBar_->Padding(12,6);
    previewBar_->Background(Color::Hex(0x1c1a14));previewBar_->Visible(false);
    previewBar_->Add<IconView>(ui::Merge).Box(22).IconSize(14);
    previewText_=&previewBar_->Add<Label>(L"合并预览",TextRole::Caption);previewText_->Grow();
    Btn(*previewBar_,L"返回合并列表",[this]{ClosePreview(true);}).Glyph(icon::kArrowLeft).ToolTip(L"结束预览并回到合并列表，可调整顺序后重新生成  ·  Esc");
    Btn(*previewBar_,L"关闭预览",[this]{ClosePreview(false);}).Glyph(icon::kClose).ToolTip(L"结束预览并回到主页；合并列表保留");
    Btn(*previewBar_,L"保存合并结果…",[this]{Save(true);},ButtonKind::Primary).Glyph(icon::kSave);
    auto& body=readPane_->Add<Row>();body.Grow();body.Spacing(2);
    // 侧栏外壳：内容列 + 右边缘拖动条（拖动调整宽度，松手后保存）。间距由拖动条补足，与原来一致。
    auto& sideHost=body.Add<Row>();sideHost.Spacing(0);sidebar_=&sideHost;
    auto& sidebar=sideHost.Add<Column>();Width(sidebar,260);sidebar.Spacing(12);sidebar.Background(Color::Hex(0x101010));sideColumn_=&sidebar;
    auto& grip=sideHost.Add<Splitter>(Splitter::Orientation::Vertical);grip.Thickness(8);
    grip.OnDrag([this](float dx){SetSidebarWidth(sidebarWidth_+dx,false);});
    grip.OnDragEnded([this]{SetSidebarWidth(sidebarWidth_,true);status_->Text(L"侧栏宽度 "+std::to_wstring(settings_.sidebarWidth)+L" 像素（已保存）");});
    // 侧栏：缩略图 / 目录（可折叠树）/ 批注列表，分段切换。
    sideTabs_=&sidebar.Add<Segmented>();sideTabs_->Role(TextRole::Caption);
    sideTabs_->AddItem(L"缩略图");sideTabs_->AddItem(L"目录");sideTabs_->AddItem(L"批注");sideTabs_->AddItem(L"搜索");sideTabs_->SelectedIndex(0);
    sideTabs_->AccessibleName(L"侧栏内容");
    sideTabs_->OnSelectionChanged([this](ptrdiff_t,ptrdiff_t now){
        SidePanel(static_cast<int>(now));
        // 记住上次选中的侧栏页，下次启动沿用。
        if(settings_.sidebarTab!=sidePanel_){settings_.sidebarTab=sidePanel_;SaveSettings();}
    });
    searchPanel_=&sidebar.Add<Column>();searchPanel_->Grow();searchPanel_->Visible(false);
    auto& searchActions=searchPanel_->Add<Row>();searchActions.Spacing(4);
    Btn(searchActions,L"选项",[this]{SearchSettings();});Btn(searchActions,L"停止",[this]{if(searchCancel_)searchCancel_->store(true);hitLabel_->Text(L"已停止");});
    searchResults_=&searchPanel_->Add<ListView>();searchResults_->Grow();searchResults_->ItemTextRole(TextRole::Caption);
    searchResults_->EmptyTitle(L"没有搜索结果").EmptyHint(L"输入关键字后回车；扫描件需要先 OCR");
    searchResults_->ItemText([this](size_t n,std::wstring& text){text=n<hits_.size()?L"第 "+std::to_wstring(hits_[n].page+1)+L" 页":L"";});
    searchResults_->ItemSecondaryText([this](size_t n,std::wstring& text){text=n<hits_.size()?hits_[n].context:L"";});
    searchResults_->OnActivate([this](size_t n){if(n>=hits_.size())return;hit_=static_cast<int>(n)-1;Hit(1);});
    thumbs_=&sidebar.Add<PdfCanvas>(PdfCanvas::View::Thumbnails);thumbs_->Grow();
    outline_=&sidebar.Add<ListView>();outline_->Grow();outline_->Visible(false);outline_->EmptyTitle(L"没有目录").EmptyHint(L"此文档未包含书签").EmptyGlyph(icon::kBookmark);
    outlineTree_=&sidebar.Add<TreeView>();outlineTree_->Grow();outlineTree_->Visible(false);outlineTree_->Role(TextRole::Caption);outlineTree_->AccessibleName(L"文档目录");
    outlineTree_->ItemText([this](size_t n,std::wstring& s){s=n<info_.outline.size()?info_.outline[n].title:L"";});
    auto jump=[this](size_t n){if(outlineSync_||n>=info_.outline.size())return;PushHistory();Page(info_.outline[n].page,true);};
    outlineTree_->OnSelectionChanged(jump);outlineTree_->OnActivate(jump);
    // 整理模式：平铺列表按层级缩进，拖动行（连同其子书签）重排；层级用 ←/→ 调整。
    outlineEdit_=&sidebar.Add<ListView>();outlineEdit_->Grow();outlineEdit_->Visible(false);outlineEdit_->ItemTextRole(TextRole::Caption);
    outlineEdit_->AccessibleName(L"整理书签");outlineEdit_->CanReorder(true);
    outlineEdit_->ItemText([this](size_t n,std::wstring& s){
        s.clear();if(n>=info_.outline.size())return;
        s.assign(static_cast<size_t>(std::clamp(info_.outline[n].depth,0,12))*2,L'\u2003');s+=info_.outline[n].title;
    });
    outlineEdit_->ItemSecondaryText([this](size_t n,std::wstring& s){s=n<info_.outline.size()?L"第 "+std::to_wstring(info_.outline[n].page+1)+L" 页":L"";});
    outlineEdit_->OnActivate([this](size_t n){if(n<info_.outline.size()){PushHistory();Page(info_.outline[n].page,true);}});
    outlineEdit_->OnReordered([this](size_t from,size_t to){
        auto items=info_.outline;if(busy_||from>=items.size()||to>=items.size()||from==to){outlineEdit_->ItemCount(items.size(),false);return;}
        // 被拖动的是一整棵子树：先取出，再放到目标行的位置。
        size_t end=from+1;while(end<items.size()&&items[end].depth>items[from].depth)++end;
        if(to>from&&to<end){outlineEdit_->ItemCount(items.size(),false);return;}
        std::vector<OutlineItem> moved(items.begin()+static_cast<ptrdiff_t>(from),items.begin()+static_cast<ptrdiff_t>(end));
        items.erase(items.begin()+static_cast<ptrdiff_t>(from),items.begin()+static_cast<ptrdiff_t>(end));
        size_t at=to>from?to-(end-from)+1:to;
        if(to>from){while(at<items.size()&&at>0&&items[at].depth>moved.front().depth&&items[at-1].depth>=moved.front().depth)++at;}
        at=std::min(at,items.size());
        items.insert(items.begin()+static_cast<ptrdiff_t>(at),moved.begin(),moved.end());
        CommitOutline(std::move(items),L"移动书签",at);
    });
    outlineBar_=&sidebar.Add<Row>();outlineBar_->Spacing(0);outlineBar_->Visible(false);
    auto tool=[this](const wchar_t* glyph,const wchar_t* tip,std::function<void()> fn)->Button&{
        auto& b=outlineBar_->Add<Button>(L"",ButtonKind::Subtle);b.Height(28).SizeClass(ButtonSize::Small).Glyph(glyph).ToolTip(tip).AccessibleName(tip).OnClick(std::move(fn));Width(b,24);return b;
    };
    tool(icon::kAdd,L"在当前页添加书签",[this]{AddBookmark();});
    tool(icon::kEdit,L"重命名书签",[this]{RenameBookmark();});
    tool(icon::kDelete,L"删除书签",[this]{DeleteBookmark();});
    tool(icon::kArrowUp,L"上移书签",[this]{MoveBookmark(-1);});
    tool(icon::kArrowDown,L"下移书签",[this]{MoveBookmark(1);});
    tool(icon::kChevronLeft,L"提升一级",[this]{IndentBookmark(-1);});
    tool(icon::kChevronRight,L"降低一级（成为上一项的子书签）",[this]{IndentBookmark(1);});
    outlineArrange_=&tool(icon::kList,L"拖动整理书签",[this]{outlineArranging_=!outlineArranging_;outlineArrange_->Kind(outlineArranging_?ButtonKind::Standard:ButtonKind::Subtle);RebuildOutline();});
    annotationPanel_=&sidebar.Add<Column>();annotationPanel_->Grow();annotationPanel_->Visible(false);annotationPanel_->Spacing(6);
    annotationFilter_=&annotationPanel_->Add<TextBox>().Placeholder(L"筛选作者、文字或类型");annotationFilter_->OnTextChanged([this](std::wstring_view){FilterAnnotations();});
    annotationSort_=&annotationPanel_->Add<ComboBox>().AddItems({L"按页码",L"按作者",L"按修改时间（新到旧）",L"按类型"}).SelectedIndex(0);
    annotationSort_->OnSelectionChanged([this](ptrdiff_t,ptrdiff_t){FilterAnnotations();});
    Btn(*annotationPanel_,L"导出批注摘要…",[this]{ExportAnnotationSummary();});
    annotList_=&annotationPanel_->Add<ListView>();annotList_->Grow();
    annotList_->EmptyTitle(L"还没有批注").EmptyHint(L"在“批注”工作区添加的批注会列在这里").EmptyGlyph(icon::kChat).AccessibleName(L"批注列表");
    annotList_->ItemText([this](size_t n,std::wstring& s){
        if(n>=annotRows_.size()){s.clear();return;}
        const auto& a=annotRows_[n].annotation;
        s=std::wstring(a.type==Tool::Select?L"批注":ui::ToolName(a.type))+L" · 第 "+std::to_wstring(annotRows_[n].page+1)+L" 页";
    });
    annotList_->ItemSecondaryText([this](size_t n,std::wstring& s){
        s.clear();if(n>=annotRows_.size())return;
        s=annotRows_[n].annotation.text;for(auto& ch:s)if(ch==L'\r'||ch==L'\n'||ch==L'\t')ch=L' ';
        if(s.size()>40){s.resize(40);s+=L"…";}
        if(s.empty())s=annotRows_[n].annotation.readOnly?L"只读":L"（无文字）";
        const auto& a=annotRows_[n].annotation;if(!a.author.empty())s=a.author+L" · "+s;
        if(a.modified>0){time_t stamp=static_cast<time_t>(a.modified);tm date{};if(localtime_s(&date,&stamp)==0){wchar_t text[32]{};wcsftime(text,32,L"%Y-%m-%d",&date);s+=L" · "+std::wstring(text);}}
    });
    annotList_->ItemGlyph([this](size_t n,std::wstring& s){
        s.clear();if(n>=annotRows_.size())return;
        s=ui::ToolGlyph(annotRows_[n].annotation.type);
    });
    annotList_->OnActivate([this](size_t n){
        if(n>=annotRows_.size()||busy_)return;
        const auto row=annotRows_[n];
        PushHistory();canvas_->FocusText(row.page,row.annotation.bounds);
        pendingSelect_={row.page,row.annotation.id};
        if(page_==row.page&&!annotations_.empty()){Select(row.page,row.annotation.id);canvas_->Selection(row.page,annotations_,annotation_);pendingSelect_.reset();}
        else Page(row.page,false);
        thumbs_->GoTo(row.page);ShowPageNumber(row.page);
    });
    splitBody_=&body.Add<DocumentSplit>();splitBody_->Grow();splitBody_->Clip(true);
    primaryPane_=&splitBody_->Add<Column>();primaryPane_->Spacing(4);primaryPane_->Clip(true);
    primaryCaption_=&primaryPane_->Add<Label>(L"",TextRole::CaptionStrong);primaryCaption_->Visible(false);
    canvas_=&primaryPane_->Add<PdfCanvas>();canvas_->Grow();
    referencePane_=&splitBody_->Add<Column>();referencePane_->Spacing(4);referencePane_->Visible(false);referencePane_->Clip(true);
    auto& referenceHeader=referencePane_->Add<Row>();referenceHeader.Spacing(4);
    referenceTitle_=&Btn(referenceHeader,L"对照文件",[this]{ChooseReference();});referenceTitle_->Grow();
    Btn(referenceHeader,L"编辑 / 交换",[this]{ActivateDocument(referenceId_);}).ToolTip(L"将对照文档切到主窗格编辑，两份文档交换位置");
    Btn(referenceHeader,L"×",[this]{SetSplit(0);}).ToolTip(L"关闭分屏（不关闭文件）");
    referenceTools_=&referencePane_->Add<Row>();referenceTools_->Spacing(3);
    Btn(*referenceTools_,L"‹",[this]{referenceCanvas_->StepPage(-1);});
    referencePage_=&referenceTools_->Add<TextBox>().Text(L"1").OnSubmit([this]{const int p=_wtoi(referencePage_->Text().c_str());if(p>0&&p<=static_cast<int>(referenceInfo_.pages.size()))referenceCanvas_->GoTo(p-1);else referencePage_->Text(std::to_wstring(referenceCanvas_->CurrentPage()+1));});Width(*referencePage_,44);
    referenceCount_=&referenceTools_->Add<Label>(L"",TextRole::Caption);
    Btn(*referenceTools_,L"›",[this]{referenceCanvas_->StepPage(1);});
    Btn(*referenceTools_,L"−",[this]{referenceCanvas_->Zoom(1/1.2f);});Btn(*referenceTools_,L"＋",[this]{referenceCanvas_->Zoom(1.2f);});
    Btn(*referenceTools_,L"适页",[this]{referenceCanvas_->FitPage();});
    Btn(*referenceTools_,L"适宽",[this]{referenceCanvas_->FitWidth();});
    referenceCanvas_=&referencePane_->Add<PdfCanvas>();referenceCanvas_->Grow();WireReference();
    textEditor_.Attach(*canvas_);textEditor_.changed=[this]{TextChanged();};
    auto& propScroll=body.Add<ScrollViewer>();propViewport_=&propScroll;Width(propScroll,248);propScroll.Margin(Thickness{8,0,0,0});
    properties_=&propScroll.Add<Column>();Width(*properties_,248);properties_->Padding(16,16);properties_->Spacing(12);properties_->Background(Color::Hex(0x141414));
    BuildAnnotationProperties();
    auto& drop=root.Add<DropColumn>();drop.dropped=[this](std::vector<fs::path> files){Queue(std::move(files));};mergePane_=&drop;mergePane_->Grow();mergePane_->Spacing(16);mergePane_->Padding(4,8);
    auto& mergeHeader=mergePane_->Add<Row>();mergeHeader.Spacing(8);
    mergeHeader.Add<Label>(L"合并文件",TextRole::Title).Grow();
    addFiles_=&Btn(mergeHeader,L"添加文件",[this]{Queue(Pick(true));},ButtonKind::Primary);
    mergeProgressPanel_=&mergePane_->Add<Column>();mergeProgressPanel_->Padding(14,12);mergeProgressPanel_->Spacing(6);
    mergeProgressPanel_->Card(Panel::CardStyle::Subtle,12).Visible(false);
    auto& progressHeader=mergeProgressPanel_->Add<Row>();progressHeader.Spacing(10);progressHeader.AlignCross(CrossAlign::Center);
    mergeTitle_=&progressHeader.Add<Label>(L"准备与转换文件",TextRole::BodyStrong).Grow();
    mergeElapsedLabel_=&progressHeader.Add<Label>(L"已用时 00:00",TextRole::Caption).Secondary(true);
    mergeCancel_=&Btn(progressHeader,L"取消任务",[this]{RequestCancel();},ButtonKind::Standard);
    mergeCancel_->AccessibleName(L"取消转换与合并");
    mergeFileLabel_=&mergeProgressPanel_->Add<Label>(L"",TextRole::Caption).MinSize({0,20});
    mergeProgressBar_=&mergeProgressPanel_->Add<ProgressBar>().AccessibleName(L"当前阶段进度");
    mergeDetailLabel_=&mergeProgressPanel_->Add<Label>(L"",TextRole::Caption).Secondary(true).Wrap(true).MinSize({0,20});
    auto& mergeBody=mergePane_->Add<Row>();mergeBody_=&mergeBody;mergeBody.Grow();mergeBody.Spacing(20);
    auto& left=mergeBody.Add<Column>();left.Grow();left.Spacing(8);
    left.Add<Label>(L"拖动调整顺序，选择文件设置页码范围",TextRole::Caption).Secondary(true).MinSize({0,24});
    queueList_=&left.Add<ListView>();queueList_->Grow();queueList_->EmptyTitle(L"将文件拖到这里").EmptyHint(L"支持 PDF、Word、Excel、PPT、TXT 和图片").EmptyGlyph(ui::Merge);
    queueList_->AccessibleName(L"合并文件队列");
    queueList_->ItemIcon([this](size_t n,Painter& p,const Theme& theme,const lumen::Rect& r){
        if(n>=queueRows_.size())return;
        ui::DrawFileIcon(p,queueRows_[n].kind,r,36,20);
        if(queueRows_[n].failed){
            // 失败角标：文件图标右下角的 danger 圆点，副行文字仍写“处理失败”。
            const float s=std::min({r.w,r.h,36.0f});
            const lumen::Rect badge{r.x+(r.w+s)/2-11,r.y+(r.h+s)/2-11,14,14};
            p.FillRoundedRect(badge.Inset(-1.5f,-1.5f),8.5f,lumen::Color{0,0,0,.72f});
            p.FillRoundedRect(badge,7,theme.danger);
            p.DrawText(L"!",badge,TextRole::Caption,theme.danger_text,Align::Center);
        }
    });
    queueList_->ItemText([this](size_t n,std::wstring& s){s=n<queueRows_.size()?queueRows_[n].title:L"";});
    queueList_->ItemSecondaryText([this](size_t n,std::wstring& s){s=n<queueRows_.size()?queueRows_[n].secondary:L"";});
    queueList_->CanReorder(true).OnReordered([this](size_t from,size_t to){if(busy_||from>=queue_.size()||to>=queue_.size())return;SyncQueueFields();auto item=queue_[from];queue_.erase(queue_.begin()+from);queue_.insert(queue_.begin()+to,std::move(item));QueueChanged();queueList_->SelectedIndex(static_cast<ptrdiff_t>(to));});
    queueList_->OnSelectionChanged([this](ptrdiff_t,ptrdiff_t now){
        if(queueUpdating_)return;SyncQueueFields();queueSelection_=now;QueueSelection();range_->Text(now>=0&&now<static_cast<ptrdiff_t>(queue_.size())?queue_[now].range:L"");
        password_->Text(now>=0&&now<static_cast<ptrdiff_t>(queue_.size())?queue_[now].password:L"");
    });
    auto& order=left.Add<Row>();order.Spacing(6);
    queueUp_=&Btn(order,L"上移",[this]{MoveQueue(-1);});queueDown_=&Btn(order,L"下移",[this]{MoveQueue(1);});
    queueUp_->ToolTip(L"上移选中文件（也可直接拖动）");queueDown_->ToolTip(L"下移选中文件（也可直接拖动）");
    queueRemove_=&Btn(order,L"移除",[this]{RemoveQueueItem();});queueRemove_->ToolTip(L"从列表移除，不删除文件  ·  Delete");
    mergeStatus_=&left.Add<Label>(L"添加文件后，预览转换结果再导出。");mergeStatus_->Role(TextRole::Caption).Wrap(true).MinSize({0,24}).Secondary(true);
    auto& optionsPane=mergeBody.Add<Column>();Width(optionsPane,280);optionsPane.Background(Color::Hex(0x141414));
    auto& optionsScroll=optionsPane.Add<ScrollViewer>();optionsScroll.Grow();auto& options=optionsScroll.Add<Column>();Width(options,280);options.Padding(16,16);options.Spacing(12);options.Background(Color::Hex(0x141414));
    options.Add<Label>(L"选中文件",TextRole::BodyStrong);
    range_=&options.Add<TextBox>().Placeholder(L"页码：全部或 1-3,5").AccessibleName(L"合并文件页码范围");
    range_->OnTextChanged([this](std::wstring_view value){auto n=queueSelection_;if(!busy_&&n>=0&&n<static_cast<ptrdiff_t>(queue_.size())){queue_[n].range=value;ResetMergeStatus();RefreshQueueRows();}});
    password_=&options.Add<TextBox>().Placeholder(L"加密 PDF 密码（如需要）").Password(true).AccessibleName(L"合并文件打开密码");
    password_->OnTextChanged([this](std::wstring_view value){auto n=queueSelection_;if(!busy_&&n>=0&&n<static_cast<ptrdiff_t>(queue_.size())){queue_[n].password=value;ResetMergeStatus();RefreshQueueRows();}});
    options.Add<Label>(L"TXT 编码",TextRole::Caption);
    encoding_=&options.Add<ComboBox>().AddItems({L"自动（UTF-8 / BOM）",L"UTF-8",L"UTF-16 LE",L"UTF-16 BE",L"GB18030 / GBK"}).SelectedIndex(0);
    options.Add<Label>(L"Office 转换（Word / Excel / PPT）",TextRole::Caption);
    office_=&options.Add<ComboBox>().AddItems({L"自动：Office 优先",L"Microsoft Office",L"LibreOffice"}).SelectedIndex(0);
    a4_=&options.Add<CheckBox>(L"图片适配 A4").Checked(true);
    bookmarks_=&options.Add<CheckBox>(L"按文件名生成目录").Checked(true);
    encoding_->OnSelectionChanged([this](ptrdiff_t,ptrdiff_t){if(!busy_){ResetMergeStatus();RefreshQueueRows();}});
    office_->OnSelectionChanged([this](ptrdiff_t,ptrdiff_t){if(!busy_){ResetMergeStatus();RefreshQueueRows();}});
    a4_->OnToggled([this](bool){if(!busy_){ResetMergeStatus();RefreshQueueRows();}});
    bookmarks_->OnToggled([this](bool){if(!busy_){ResetMergeStatus();RefreshQueueRows();}});
    options.Add<Label>(L"导出为阅读文件：来源批注和表单外观会固化；正文保留文字与矢量。原文件不变。",TextRole::Caption).Wrap(true).MinSize({0,52}).Secondary(true);
    auto& exportActions=optionsPane.Add<Column>();exportActions.Padding(16,12);exportActions.Spacing(10);
    previewButton_=&Btn(exportActions,L"生成预览",[this]{Merge(false);},ButtonKind::Standard);
    exportButton_=&Btn(exportActions,L"合并并导出",[this]{Merge(true);},ButtonKind::Primary);
    auto& footer=root.Add<Row>();footer.Spacing(8);footer_=&footer;
    footer.Add<IconView>(icon::kShield).Box(20).IconSize(13).Background(Color::Hex(0x0d0d0d));
    status_=&footer.Add<Label>(L"文件仅在本机处理",TextRole::Caption).Grow().Secondary(true);
    cancelButton_=&Btn(footer,L"取消任务",[this]{RequestCancel();});
    auto& viewGroup=footer.Add<Row>();viewGroup.Spacing(8);viewGroup.AlignCross(CrossAlign::Center);viewGroup_=&viewGroup;
    Btn(viewGroup,L"−",[this]{canvas_->Zoom(1/1.2f);}).AccessibleName(L"缩小页面").ToolTip(L"缩小  ·  Ctrl+-");
    // 中间的百分比本身就是按钮：显示当前缩放，点击弹出适合页面 / 适合宽度 / 常用比例。
    zoomButton_=&Btn(viewGroup,L"适合页面",[this]{ShowZoomMenu();});zoomButton_->AccessibleName(L"页面缩放比例").ToolTip(L"缩放比例 · 点击选择适合页面、适合宽度或常用比例  ·  Ctrl+滚轮缩放");
    Btn(viewGroup,L"＋",[this]{canvas_->Zoom(1.2f);}).AccessibleName(L"放大页面");
    WireZoomTools();
    canvas_->rotated_edit_blocked=[this]{status_->Text(L"视图已旋转 "+std::to_wstring(canvas_->ViewRotation())+L"°：旋转状态下不能输入或编辑文字批注。右键 → 恢复视图方向 后再编辑。");};
    canvas_->zoom_changed=[this](float value){auto text=std::to_wstring(static_cast<int>(std::lround(value*100)))+L"%";
        if(const int r=canvas_->ViewRotation())text+=L" · ↻"+std::to_wstring(r)+L"°";if(zoomButton_->Text()!=text){zoomButton_->Text(text);zoomButton_->AccessibleName(L"页面缩放 "+text);}};
    pageBox_=&viewGroup.Add<TextBox>().Role(TextRole::Caption).MaxLength(6).AccessibleName(L"当前页码，输入后回车跳转");Width(*pageBox_,56);
    pageBox_->ToolTip(L"输入页码后回车跳转  ·  Ctrl+Shift+N");pageBox_->OnSubmit([this]{GoToPageText();});pageBox_->Visible(false);
    pageLabel_=&viewGroup.Add<Label>(L"",TextRole::Caption);pageLabel_->MinSize({0,34});
    Wire(canvas_);Wire(thumbs_);
    canvas_->exit_presentation=[this]{Present(false);};
    thumbs_->page_changed=[this](int p){Page(p,true);};
    QueueSelection();Mode(0);SetBusy(false);
}
void Application::ChooseTool(Tool tool){
    if(tool!=Tool::Select){canvas_->HideAnnotations(false);thumbs_->HideAnnotations(false);}
    if(textEditor_.Active()){FinishText(true,[this,tool]{ChooseTool(tool);});return;}
    placement_={};   // 任何工具切换（含 Esc、点击工具按钮）都结束签名 / 标记放置
    // 选择其它批注工具时暂停涂黑模式（标记保留显示，“更多 → 涂黑”继续）。
    if(tool!=Tool::Select&&redacting_){redacting_=false;canvas_->RedactMode(false);status_->Text(L"已暂停涂黑模式，标记保留；“更多 → 继续涂黑”可继续");}
    currentTool_=tool;
    if(tool!=Tool::Select&&canvas_->HandTool())SetHand(false);
    if(IsMarkupTool(tool))lastMarkup_=tool;
    if(IsBoxShape(tool)||IsLineTool(tool)||tool==Tool::Ink)lastDrawing_=tool;
    if(tool!=Tool::Select){annotation_=-1;canvas_->Selection(page_,annotations_,-1);}
    canvas_->EditingTool(tool);canvas_->DrawingStyle(toolStyles_[static_cast<size_t>(tool)],toolOpacities_[static_cast<size_t>(tool)]);
    Select(page_,annotation_);
    const wchar_t* hints[]={L"选择 · 沿轮廓拖动批注，Delete 删除，Esc 取消操作",L"文字 · 单击选中，双击编辑；点击空白处新建，拖动确定换行宽度",L"便签 · 点击页面添加评论，双击图标编辑",L"高亮 · 沿文字拖动；Ctrl 拖动进行区域高亮",L"矩形 · 拖动绘制；Shift 保持正方形",L"箭头 · 拖动指向目标；Shift 约束 45°",L"手绘 · 连续绘制，圆润笔触；Esc 返回选择",L"图片 · 点击或拖框选择图片，角点等比缩放",
        L"下划线 · 沿文字拖动，逐行贴合",L"删除线 · 沿文字拖动，逐行贴合",L"椭圆 · 拖动绘制；Shift 保持正圆",L"直线 · 拖动绘制；Shift 约束 45°",
        L"印章 · 点击放置（或拖框定大小），选择“已批准”“机密”等或输入文字"};
    static_assert(std::size(hints)==ToolCount);
    status_->Text(mode_==1?std::wstring(hints[static_cast<size_t>(tool)]):ContextHint());
    for(auto [button,value]:toolButtons_)button->Kind(value==tool&&!canvas_->HandTool()?ButtonKind::Standard:ButtonKind::Subtle);
}
void Application::SetHand(bool on){
    if(!canvas_||canvas_->HandTool()==on)return;
    if(on&&textEditor_.Active()){FinishText(true,[this]{SetHand(true);});return;}
    if(on&&mode_==1&&currentTool_!=Tool::Select)ChooseTool(Tool::Select);
    canvas_->HandTool(on);
    if(handButton_)handButton_->Kind(on?ButtonKind::Standard:ButtonKind::Subtle);
    for(auto [button,value]:toolButtons_)button->Kind(!on&&value==currentTool_?ButtonKind::Standard:ButtonKind::Subtle);
    if(loaded_&&!home_)status_->Text(on?std::wstring(L"手型工具 · 拖动平移页面，单击链接可跳转  ·  V 返回选择"):ContextHint());
}
void Application::ToolKey(Tool tool){
    if(mode_!=1)Mode(1);
    SetHand(false);ChooseTool(tool);
}
void Application::CycleTool(std::initializer_list<Tool> order,Tool fallback){
    // 已在该组内：切到下一个；否则先回到该组上次使用的工具（与 Acrobat 的 Shift+U / Shift+D 一致）。
    Tool next=fallback;
    if(mode_==1&&!canvas_->HandTool()){
        auto it=std::find(order.begin(),order.end(),currentTool_);
        if(it!=order.end())next=++it==order.end()?*order.begin():*it;
    }
    ToolKey(next);
}
void Application::SpaceKey(bool down,bool repeat){
    HWND hwnd=static_cast<HWND>(window_.NativeHandle());
    auto refreshCursor=[hwnd]{POINT pt{};if(GetCursorPos(&pt)&&WindowFromPoint(pt)==hwnd)PostMessageW(hwnd,WM_SETCURSOR,reinterpret_cast<WPARAM>(hwnd),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));};
    if(!down){canvas_->SpaceHeld(false);refreshCursor();return;}
    if(repeat||!loaded_||home_||(mode_!=0&&mode_!=1)||busy_||textEditor_.Active()||window_.DialogActive()||window_.PopupActive())return;
    // 输入框、复选框、分段按钮等保留空格原义；普通按钮刚被点过时焦点在它身上，空格改为给页面。
    Control* focused=window_.Focused();
    if(focused&&focused!=canvas_&&!dynamic_cast<Button*>(focused))return;
    if(focused!=canvas_)canvas_->Focus();
    canvas_->SpaceHeld(true);refreshCursor();
}
void Application::Mode(int mode){
    if(textEditor_.Active()){FinishText(true,[this,mode]{Mode(mode);});return;}
    const int previous=mode_;mode_=mode;
    if((mode==2||mode==3)&&canvas_&&canvas_->HandTool())SetHand(false);
    for(int i=0;i<4;++i)static_cast<ui::Tab*>(modes_[i])->Active(i==mode);
    const bool reading=mode!=3&&loaded_&&!home_;
    readPane_->Visible(reading);welcomePane_->Visible(mode!=3&&!reading);mergePane_->Visible(mode==3);tools_->Visible(reading);
    readerTools_->Visible(mode==0);annotationTools_->Visible(mode==1);pageTools_->Visible(mode==2);viewGroup_->Visible(reading);homeButton_->Kind(home_&&mode!=3?ButtonKind::Standard:ButtonKind::Subtle);
    propViewport_->Visible(mode==1);if(previous!=mode){canvas_->Display(mode==2?PdfCanvas::View::Pages:PdfCanvas::View::Reading);if(loaded_)canvas_->GoTo(page_);}
    if(mode!=1)ChooseTool(Tool::Select);else if(previous!=mode)ChooseTool(currentTool_);
    if(mode==2)PageSelectionHint(canvas_->SelectedPages().size());
    SyncViewSwitch();
    UpdateTitle();
}
void Application::SetBusy(bool value,std::wstring caption){
    busy_=value;header_->Enabled(!value);tools_->Enabled(!value&&loaded_);
    mergePane_->Enabled(true);mergeBody_->Enabled(!value);addFiles_->Enabled(!value);QueueSelection();
    for(int i=0;i<4;++i)modes_[i]->Enabled(!value&&(loaded_||i==0||i==3));
    undo_->Enabled(!value&&loaded_&&info_.canUndo);redo_->Enabled(!value&&loaded_&&info_.canRedo);
    save_->Enabled(!value&&loaded_);properties_->Enabled(!value&&loaded_);
    canvas_->Editable(!value);thumbs_->Editable(!value);cancelButton_->Visible(value&&!mergeRunning_);
    const bool reading=mode_!=3&&loaded_&&!home_;
    readPane_->Visible(reading);welcomePane_->Visible(mode_!=3&&!reading);tools_->Visible(reading);undo_->Visible(loaded_);redo_->Visible(loaded_);
    homeButton_->Visible(loaded_);viewGroup_->Visible(reading);homeButton_->Kind(home_&&mode_!=3?ButtonKind::Standard:ButtonKind::Subtle);
    header_->Visible(!readingMode_&&!presenting_);if(readingMode_||presenting_){tools_->Visible(false);sidebar_->Visible(false);}
    SyncDocumentTabs();
    if(presenting_){footer_->Visible(false);previewBar_->Visible(false);}
    if(!caption.empty())status_->Text(caption);
}
void Application::Fail(std::wstring message){message=FriendlyConversionError(std::move(message));log::Error(message);SetBusy(false,L"操作未完成");window_.Alert(L"无法完成操作",message);}
void Application::Task(std::wstring caption,std::function<void(Engine&,const Cancel&)> work,std::function<void()> done,std::function<void()> password,std::function<void(std::wstring)> failed,std::function<void()> cancelled){
    if(busy_)return;
    cancel_=std::make_shared<std::atomic_bool>(false);auto token=cancel_;auto alive=alive_;auto post=window_.Dispatcher();int requested=page_;
    if(searchCancel_)searchCancel_->store(true);
    ++*generation_;SetBusy(true,caption+L"…");const bool keep=std::exchange(keepSearch_,false);
    worker_->Submit([this,alive,post,token,work=std::move(work),done=std::move(done),password=std::move(password),failed=std::move(failed),cancelled=std::move(cancelled),requested,keep](Engine& e){
        try{
            CheckCancel(token);work(e,token);
            DocumentInfo info;std::vector<Annotation> annotations;int p=0;
            if(e.open){info=e.document.Info();p=std::clamp(requested,0,std::max(0,static_cast<int>(info.pages.size())-1));annotations=e.document.Annotations(p);}
            post.Post([this,alive,info=std::move(info),annotations=std::move(annotations),p,done,keep]{if(!alive->load())return;keepSearch_=keep;Refresh(info,annotations,p);SetBusy(false,L"已完成");if(done)done();UpdateTitle();});
        }catch(const PasswordRequired&){
            post.Post([this,alive,password]{if(!alive->load())return;SetBusy(false);canvas_->DocumentPages(info_.pages,generation_->load());thumbs_->DocumentPages(info_.pages,generation_->load());canvas_->Selection(page_,annotations_,annotation_);if(password)password();else Fail(L"此 PDF 需要密码。请在合并列表中填写密码，或先单独打开。");});
        }catch(const Cancelled&){post.Post([this,alive,failed,cancelled]{if(alive->load()){SetBusy(false,L"任务已取消");canvas_->DocumentPages(info_.pages,generation_->load());thumbs_->DocumentPages(info_.pages,generation_->load());if(cancelled)cancelled();else if(failed)failed(L"任务已取消");}});}
        catch(const std::exception& ex){auto message=ErrorText(ex);post.Post([this,alive,message,failed]{if(alive->load()){canvas_->DocumentPages(info_.pages,generation_->load());thumbs_->DocumentPages(info_.pages,generation_->load());canvas_->Selection(page_,annotations_,annotation_);if(failed){SetBusy(false);failed(message);}else Fail(message);}});}
    });
}
void Application::Refresh(DocumentInfo info,std::vector<Annotation> annotations,int p){
    info_=std::move(info);annotations_=std::move(annotations);page_=p;loaded_=!info_.pages.empty();
    canvas_->DocumentPages(info_.pages,generation_->load());thumbs_->DocumentPages(info_.pages,generation_->load());
    canvas_->Selection(page_,annotations_,annotation_);thumbs_->SelectedPages({page_});Select(page_,annotation_);
    RebuildOutline();
    if(sidePanel_==2)RequestAnnotationList();
    // 编辑后旧搜索结果的位置不再可信；复制、保存等只读任务保留结果。
    if(!keepSearch_){searchResults_->ItemCount(0,false);hits_.clear();hit_=-1;lastQuery_.clear();hitLabel_->Text(L"");SyncSearchButtons();}
    else if(!hits_.empty())canvas_->SearchResults(hits_,hit_,false);
    keepSearch_=false;ShowPageNumber(page_);
    UpdateTitle();
    CheckRedactionMarks();
    // 每个文档首次出现表单时提示一次（延后，避免被任务完成提示覆盖）。
    const auto doc=activeDocument_?activeDocument_->id:0;
    if(info_.hasForm&&formHinted_.insert(doc).second)window_.SetTimeout(.8f,[this]{
        if(loaded_&&info_.hasForm&&!busy_)status_->Text(L"此文档包含可填写的表单：单击蓝色高亮区域填写，“更多 → 重置表单”可恢复默认值");});
}
void Application::Guard(std::function<void()> action){
    if(textEditor_.Active()){FinishText(true,[this,action]{Guard(action);});return;}
    if(busy_||window_.DialogActive())return;
    if(!info_.dirty){action();return;}
    const auto id=activeDocument_?activeDocument_->id:0;
    DialogSpec dialog;
    dialog.title=L"保存“"+name_->Text()+L"”的修改？";
    dialog.message=L"可以保存后继续，或放弃本次修改。放弃不会更改原文件。";
    dialog.primary={L"保存",[this,action,id]{window_.Dispatcher().Post([this,action,id]{ActivateDocument(id);if(activeDocument_&&activeDocument_->id==id)Save(false,action);});}};
    dialog.secondary={L"放弃修改",[this,action]{window_.Dispatcher().Post(action);}};
    dialog.close={L"取消",{}};
    dialog.default_button=DialogCommand::Close;
    dialog.cancel_button=DialogCommand::Close;
    window_.ShowDialog(std::move(dialog));
}
void Application::Open(const fs::path& path,std::wstring password){
    if(busy_||window_.DialogActive()){OpenFiles({path});return;}
    if(textEditor_.Active()){FinishText(true,[this,path,password]{Open(path,password);});return;}
    CaptureDocument();
    for(const auto& doc:documents_)if((!doc->source.empty()&&RecentFiles::SamePath(path,doc->source))||(!doc->restored.empty()&&RecentFiles::SamePath(path,doc->restored))){ActivateDocument(doc->id);return;}
    if(!BeginDocument())return;const auto id=activeDocument_->id;
    const auto options=ConversionOptions{};const bool recovery=path.parent_path()==recoveryRoot_;
    Task(L"打开 "+path.filename().wstring(),[path,password,options,recovery](Engine&e,const Cancel& c){
        const auto converted=e.converter.Convert(path,options,c);CheckCancel(c);e.document.Open(converted.pdf,password);if(recovery)e.document.MarkDirty();e.open=true;
    },[this,path,recovery]{source_=recovery?fs::path{}:path;restoredFile_=recovery?path:fs::path{};StampSource();if(!recovery)RememberRecent(path);annotation_=-1;name_->Text(recovery?L"恢复文档.pdf":path.filename().wstring());home_=false;Mode(0);if(recovery||!settings_.restorePosition||!RestorePosition()){if(settings_.defaultFit==1)canvas_->FitWidth();else canvas_->FitPage();Page(0,true);}UpdateTitle();if(pendingSplitMode_){const int split=pendingSplitMode_;pendingSplitMode_=0;SetSplit(split);}},
    [this,path,id]{window_.Prompt(L"需要 PDF 密码",L"输入文档的打开密码。取消只关闭新标签，不影响其他文档。",[this,path,id](std::optional<std::wstring> p){window_.Dispatcher().Post([this,path,id,p]{if(!FindDocument(id))return;if(p){ActivateDocument(id);if(activeDocument_&&activeDocument_->id==id)Open(path,*p);}else{pendingSplitMode_=0;DropDocument(id);}});});},
    [this,id](std::wstring error){DropDocument(id);pendingSplitMode_=0;Fail(error);},[this,id]{DropDocument(id);pendingSplitMode_=0;});
}
void Application::ChooseOpen(){if(busy_)return;auto files=Pick(true);if(!files.empty())OpenFiles(std::move(files));}
void Application::Save(bool as,std::function<void()> after){
    if(textEditor_.Active()){FinishText(true,[this,as,after]{Save(as,after);});return;}
    if(!loaded_||busy_)return;
    auto path=source_;
    if(as||path.empty()||path.extension()!=L".pdf")path=Destination(source_.empty()?L"未命名.pdf":source_.stem().wstring()+L"_编辑.pdf");
    if(path.empty())return;
    for(const auto& doc:documents_)if(doc!=activeDocument_&&!doc->source.empty()&&RecentFiles::SamePath(path,doc->source)){Fail(L"目标文件已在另一个标签页打开。请切换到那个标签保存，或另选文件名，避免覆盖另一份未保存修改。");return;}
    keepSearch_=true;
    Task(L"保存 PDF",[path](Engine&e,const Cancel& c){e.document.Save(path,c);},[this,path,after]{source_=path;StampSource();RememberRecent(path);std::error_code error;if(!restoredFile_.empty()){fs::remove(restoredFile_,error);restoredFile_.clear();}if(!recoveryFile_.empty())fs::remove(recoveryFile_,error);name_->Text(path.filename().wstring());status_->Text(L"已保存："+path.wstring());if(save_)save_->Flash(StatusTone::Success);if(after)after();});
}
void Application::Page(int p,bool scroll){
    if(textEditor_.Active()&&p!=textPage_){FinishText(true,[this,p,scroll]{Page(p,scroll);});return;}
    if(!loaded_||p<0||p>=static_cast<int>(info_.pages.size()))return;
    if(scroll)canvas_->GoTo(p);
    if(scroll||page_!=p)thumbs_->GoTo(p);
    thumbs_->SelectedPages({p});
    ShowPageNumber(p);SyncOutline(p);
    if(page_==p&&!annotations_.empty())return;
    page_=p;annotation_=-1;annotations_.clear();canvas_->Selection(p,{});Select(p,-1);
    auto post=window_.Dispatcher();auto alive=alive_;auto gen=generation_->load();
    worker_->Submit([this,post,alive,p,gen](Engine&e){
        try{if(!e.open)return;auto a=e.document.Annotations(p);post.Post([this,alive,p,gen,a=std::move(a)]{if(alive->load()&&generation_->load()==gen&&page_==p){annotations_=a;canvas_->Selection(p,a);if(pendingSelect_&&pendingSelect_->first==p){const int id=pendingSelect_->second;pendingSelect_.reset();Select(p,id);canvas_->Selection(p,annotations_,annotation_);}}});}catch(...){}
    });
}
void Application::Wire(PdfCanvas* c){
    auto alive=alive_;auto post=window_.Dispatcher();auto generation=generation_;
    struct LayerCache {std::mutex mutex;uint64_t generation{};std::map<int,std::shared_ptr<std::vector<Annotation>>> pages;};
    auto layers=std::make_shared<LayerCache>();
    c->request_tile=[this,c,alive,post,generation,layers](TileRequest request){
        worker_->Submit([this,c,alive,post,generation,layers,request](Engine&e){
            if(!alive->load()||generation->load()!=request.generation||(request.cancelled&&request.cancelled->load())||!e.open)return;
            try{
                std::shared_ptr<std::vector<Annotation>> text;
                if(request.key.textLayer){
                    std::lock_guard lock(layers->mutex);
                    if(layers->generation!=request.generation){layers->generation=request.generation;layers->pages.clear();}
                    auto& entry=layers->pages[request.key.page];
                    if(!entry)entry=std::make_shared<std::vector<Annotation>>(e.document.Annotations(request.key.page));
                    text=entry;
                }
                auto bitmap=e.document.Render(request.key.page,request.scale,request.clip,request.hiddenAnnotation,request.key.textLayer);
                ApplyTone(request.tone,bitmap.bgra,bitmap.width,bitmap.height,bitmap.stride);
                if(request.cancelled&&request.cancelled->load())return;
                post.Post([c,alive,request,text,bitmap=std::move(bitmap)]()mutable{if(alive->load()){
                    if(text)c->AcceptTextLayer(request.key.page,request.generation,*text);
                    c->AcceptTile(request,std::move(bitmap));
                }});
            }catch(const std::exception& ex){auto message=ErrorText(ex);post.Post([this,c,alive,request,generation,message]{if(alive->load()&&generation->load()==request.generation){c->FailTile(request.key);status_->Text(L"页面渲染失败："+message);}});}
        },false);
    };
    c->page_changed=[this](int p){if(!switchingDocuments_)Page(p);};
    if(c==canvas_){
        c->pages_selected=[this](size_t n){PageSelectionHint(n);};
        c->delete_pages=[this]{if(loaded_&&!busy_&&mode_==2)DeleteSelectedPages();};
    }
    c->request_sprite=[this,c,alive,post,generation](SpriteRequest request){
        worker_->Submit([c,alive,post,generation,request](Engine&e){
            if(!alive->load()||generation->load()!=request.generation||!e.open)return;
            try{auto sprite=e.document.RenderAnnotation(request.page,request.id,request.scale);
                post.Post([c,alive,request,sprite=std::move(sprite)]()mutable{if(alive->load())c->AcceptSprite(request,std::move(sprite));});
            }catch(...){/* 预览失败时退回原有行为：拖动中仅移动选框。 */}
        });
    };
    if(c==canvas_){
        c->edit_text=[this](int p,int id,Point caret){
            auto it=std::find_if(annotations_.begin(),annotations_.end(),[id](const auto& a){return a.id==id;});
            if(it!=annotations_.end()){if(it->type==Tool::Note)EditNote(p,*it);else if(it->type==Tool::Stamp)EditStamp(p,*it);else BeginText(p,*it,caret);}
        };
        c->editor_layout=[this](lumen::Rect bounds,lumen::Rect viewport,float scale,float dpi){
            textSync_=true;textEditor_.Place(bounds,viewport,scale,dpi);textFormatBar_.Place(bounds,viewport,dpi);textSync_=false;
        };
        c->draft_resized=[this](Rect bounds,bool resized){
            if(textDraft_){
                const bool heightChanged=std::abs(textDraft_->bounds.h-bounds.h)>.5f;
                textGeometryChanged_=true;textDraft_->bounds=bounds;
                if(resized){textDraft_->fixedTextBox=true;textDraft_->textSizing=heightChanged?TextSizing::Fixed:TextSizing::Width;textMinHeight_=heightChanged?bounds.h:0;}
                TextChanged();textEditor_.Focus();
            }
        };
        c->commit_text=[this]{FinishText(true);};
    }
    c->request_highlight=[this,c,alive,post,generation](HighlightRequest request){
        worker_->Submit([c,alive,post,generation,request](Engine& e){
            if(!alive->load()||request.cancelled->load()||generation->load()!=request.generation||!e.open)return;
            try{auto resolved=request;auto quads=ResolveHighlight(e.document,resolved);
                post.Post([c,alive,resolved,quads=std::move(quads)]()mutable{if(alive->load())c->AcceptHighlight(resolved,std::move(quads));});
            }catch(...){/* Preview failure never mutates the document; creation reports errors. */}
        });
    };
    c->delete_annotation=[this](int p,int id){DeleteAnnotation(p,id);};
    c->create_annotation=[this](int p,Tool t,Rect r,std::vector<Point> points){Annotate(p,t,r,std::move(points));};
    c->update_annotation=[this](int p,Annotation a){Task(L"调整批注",[p,a](Engine&e,const Cancel&){e.document.UpdateAnnotation(p,a);});};
    c->select_annotation=[this](int p,int id){Select(p,id);};
    c->reorder_page=[this](int from,int to){
        if(busy_||from==to)return;std::vector<int> order(info_.pages.size());std::iota(order.begin(),order.end(),0);
        if(from<0||to<0||from>=static_cast<int>(order.size())||to>=static_cast<int>(order.size()))return;
        const int n=order[from];order.erase(order.begin()+from);order.insert(order.begin()+to,n);
        Task(L"重排页面",[order](Engine&e,const Cancel&){e.document.Reorder(order);});
    };
    // 网格拖动重排（页面视图 / 侧栏缩略图）：发起方已在本地按新顺序排好并播放落位动画；
    // 另一视图同步预排，文档任务完成后恢复选中与当前页，全程不闪旧内容。
    c->reorder_pages=[this,c](std::vector<int> order,std::vector<int> selected,int current)->bool{
        if(busy_||!loaded_||textEditor_.Active()||order.size()!=info_.pages.size()||current<0||current>=static_cast<int>(order.size()))return false;
        const std::wstring message=selected.size()>1?L"已移动 "+std::to_wstring(selected.size())+L" 页"
            :L"第 "+std::to_wstring(order[current]+1)+L" 页已移到第 "+std::to_wstring(current+1)+L" 位";
        PdfCanvas* other=c==canvas_?thumbs_:canvas_;
        page_=current;other->PreviewOrder(order);
        Task(L"重排页面",[order](Engine&e,const Cancel&){e.document.Reorder(order);},
            [this,selected,current,message]{
                if(mode_==2){canvas_->SelectedPages(selected);PageSelectionHint(selected.size());}
                Page(current,false);status_->Text(message+L"  ·  Ctrl+Z 撤销");
            },{},[this](std::wstring error){canvas_->ResetTiles();thumbs_->ResetTiles();Fail(error);});
        return true;
    };
    c->files_dropped=[this](std::vector<fs::path> paths){if(mode_!=3)OpenFiles(std::move(paths));else Queue(std::move(paths));};
    c->text_selection=[this](int p,Rect r){Copy(p,r);};
    c->copy_selection=[this](PdfCanvas::TextSelection selection){CopySelection(selection);};
    c->selection_changed=[this](PdfCanvas::TextSelection selection){
        if(selection.page>=0&&!selection.quads.empty())status_->Text(L"已选中文字 · Ctrl+C 复制，右键可高亮或搜索");
    };
    c->request_links=[this,c,alive,post](int page,uint64_t gen){
        worker_->Submit([c,alive,post,page,gen](Engine& e){
            if(!alive->load()||!e.open)return;
            std::vector<Link> links;std::vector<FormField> fields;
            try{links=e.document.Links(page);}catch(...){/* 链接读取失败不影响阅读 */}
            try{fields=e.document.FormFields(page);}catch(...){/* 表单字段读取失败时仅不可点击填写 */}
            post.Post([c,alive,page,gen,links=std::move(links),fields=std::move(fields)]()mutable{if(alive->load())c->AcceptLinks(page,gen,std::move(links),std::move(fields));});
        },false);
    };
    if(c==canvas_){
        c->activate_field=[this](int p,FormField f){FillField(p,f);};
        c->redact_add=[this](int p,Rect r){AddRedactions({{p,r}},L"区域");};
        c->redact_remove=[this](size_t i){RemoveRedaction(i);};
        c->redact_escape=[this]{ExitRedaction(false);};
    }
    c->link_hover=[this](const Link* link){
        if(!link){if(!linkStatus_.empty()&&status_->Text()==linkStatus_)status_->Text(statusBeforeLink_);linkStatus_.clear();return;}
        if(linkStatus_.empty())statusBeforeLink_=status_->Text();
        linkStatus_=link->External()?L"链接 · "+link->uri:L"单击跳转到第 "+std::to_wstring(link->page+1)+L" 页";
        status_->Text(linkStatus_);
    };
    c->activate_link=[this](Link link){FollowLink(link);};
    c->context_menu=[this](lumen::Point point){ShowCanvasMenu(point);};
    c->page_menu=[this](lumen::Point point,int page){ShowPageMenu(point,page);};
    if(c==canvas_)c->open_page=[this](int p){if(!loaded_||busy_)return;Mode(0);Page(p,true);};
    if(c==canvas_)c->navigate=[this](int d){if(loaded_&&!busy_&&!textEditor_.Active())Navigate(d);};
}
void Application::BeginText(int p,Annotation value,std::optional<Point> caret){
    if(busy_||textEditor_.Active()||value.type!=Tool::Text||value.readOnly)return;
    if(value.rotation!=0){window_.Alert(L"暂不支持",L"旋转文字的页内编辑尚未完成。");return;}
    // Editing is an in-place operation in either Select or Text. Do not switch
    // workspaces/tools: showing the properties pane or redisplaying the canvas
    // would change fit scale/scroll before placing the caret.
    textPage_=p;annotation_=value.id;textOriginal_=value.id>=0?std::optional<Annotation>(value):std::nullopt;
    lumen::TextLayout initialLayout;
    value.bounds=FitTextBounds(value,info_.pages[p],initialLayout);
    textGeometryChanged_=false;textDraft_=value;textMinHeight_=value.textSizing==TextSizing::Fixed?value.bounds.h:0;
    if(!caret)canvas_->FocusText(p,value.bounds);
    // Entering editing preserves page zoom and document data; only the UI frame fits content.
    textSync_=true;const auto owner=static_cast<HWND>(window_.NativeHandle());
    textEditor_.Begin(owner,value,[this](bool apply){FinishText(apply);});
    textEditor_.save=[this]{Save(false);};textEditor_.format_shortcut=[this](wchar_t key){textFormatBar_.Toggle(key);};
    textFormatBar_.Begin(owner,value,[this](TextFormat format,float size){FormatText(std::move(format),size);},
        [this](bool apply){FinishText(apply);},[this]{textEditor_.Focus();},[this]{FitText();});
    textSync_=false;
    canvas_->Draft(value,p);textEditor_.Focus();
    if(caret)textEditor_.CaretAt(canvas_->ClientPoint(p,*caret),GetDpiForWindow(owner)/96.0f);
    finishText_->Visible(true);cancelText_->Visible(true);editText_->Visible(false);
    deleteText_->Enabled(false);undo_->Enabled(false);redo_->Enabled(false);
    ShowProperties(value,value.id>=0);propEmpty_->Visible(false);propEditor_->Visible(true);propLabel_->Text(L"正在页内编辑文字");editText_->Visible(false);deleteText_->Enabled(false);
    size_->Enabled(true);opacity_->Enabled(true);size_->Text(std::to_wstring(static_cast<int>(value.fontSize)));opacity_->Text(std::to_wstring(static_cast<int>(value.opacity*100)));
    textFormatBar_.Overflow(textEditor_.RequiredHeight()>value.bounds.h+.5f);
    status_->Text(L"框旁修改字体和格式 · Enter 换行 · 点击页面空白 / Ctrl+Enter 完成 · Esc 取消");
}
void Application::FormatText(TextFormat format,float size){
    if(!textDraft_)return;
    textDraft_->textFormat=std::move(format);textDraft_->fontSize=size;
    textSync_=true;textEditor_.Style(size,textDraft_->textFormat);textSync_=false;
    size_->Text(std::to_wstring(size));TextChanged();
}
void Application::FitText(){
    if(!textDraft_||!textEditor_.Active())return;
    textDraft_->textSizing=TextSizing::Auto;textGeometryChanged_=true;textMinHeight_=0;
    TextChanged();textEditor_.ScrollToTop();
}
void Application::TextChanged(){
    if(textSync_||!textEditor_.Active()||!textDraft_)return;
    const auto& page=info_.pages[textPage_];
    const auto old=textDraft_->bounds;
    const float available=std::max(6.0f,page.originX+page.width-old.x);
    const bool automatic=textDraft_->textSizing==TextSizing::Auto;
    const float width=automatic?available:std::clamp(old.w,6.0f,available);
    const auto needed=textEditor_.FittedSize(width);
    auto box=old;
    if(automatic)box.w=std::clamp(std::ceil((needed.w+.5f)*64.0f)/64.0f,std::min(available,6.0f),available);
    box.h=std::clamp(std::max(needed.h,textMinHeight_),std::min(page.height,6.0f),page.height);
    if(automatic&&textDraft_->textFormat.alignment==1)box.x+=(old.w-box.w)*.5f;
    if(automatic&&textDraft_->textFormat.alignment==2)box.x+=old.w-box.w;
    box.x=std::clamp(box.x,page.originX,std::max(page.originX,page.originX+page.width-box.w));
    box.y=std::clamp(box.y,page.originY,std::max(page.originY,page.originY+page.height-box.h));
    if(std::abs(old.w-box.w)>.02f||std::abs(old.h-box.h)>.02f||std::abs(old.x-box.x)>.02f||std::abs(old.y-box.y)>.02f){
        textDraft_->bounds=box;canvas_->Draft(textDraft_,textPage_);
    }
    textFormatBar_.Overflow(textEditor_.RequiredHeight()>textDraft_->bounds.h+.5f);
}
void Application::FinishText(bool apply,std::function<void()> after){
    if(!textEditor_.Active()||!textDraft_){if(after)after();return;}
    if(textEditor_.Composing())return;
    if(apply&&!textFormatBar_.CommitFields())return;
    auto value=*textDraft_;value.text=textEditor_.Text();value.fixedTextWidth=true;value.fixedTextBox=true;value.lumenText=true;
    const auto original=textOriginal_;const int p=textPage_;
    // The fitted view rectangle is committed only when content/style/geometry actually changes.
    textEditor_.End();textFormatBar_.End();canvas_->Draft({});canvas_->Focus();
    textDraft_.reset();textOriginal_.reset();
    finishText_->Visible(false);cancelText_->Visible(false);editText_->Visible(true);
    deleteText_->Enabled(true);undo_->Enabled(info_.canUndo);redo_->Enabled(info_.canRedo);
    Select(page_,annotation_);canvas_->Selection(page_,annotations_,annotation_);
    if(!apply){status_->Text(L"已取消文字编辑，文档未更改");if(after)after();return;}
    defaultTextFormat_=value.textFormat;defaultFont_=value.fontSize;
    const bool empty=value.text.find_first_not_of(L" \t\n\r")==std::wstring::npos;
    if(empty&&!original){status_->Text(L"空文本未加入文档");if(after)after();return;}
    if(original&&value.text==original->text&&value.fontSize==original->fontSize&&value.opacity==original->opacity&&
        value.textFormat==original->textFormat&&!textGeometryChanged_){
        if(after)after();return;
    }
    Task(L"完成文字编辑",[p,value,original,empty](Engine& e,const Cancel&){
        if(empty&&original)e.document.DeleteAnnotation(p,original->id);
        else if(original)e.document.UpdateAnnotation(p,value);
        else e.document.AddTextAnnotation(p,value);
    },[this,p,after,original,empty]{
        annotation_=empty?-1:original?original->id:annotations_.empty()?-1:annotations_.back().id;
        Select(p,annotation_);canvas_->Selection(p,annotations_,annotation_);
        if(after)after();
    },{},[this,p,value,original](std::wstring error){
        BeginText(p,value);textOriginal_=original;textGeometryChanged_=true;
        status_->Text(L"未能完成，文字草稿已保留："+error);
    });
}
void Application::Backup(){
    if(busy_)return;CaptureDocument();SaveReferencePosition();
    for(const auto& doc:documents_){if(!doc->info.dirty||doc->recovery.empty()||doc->backupPending->exchange(true))continue;
        const auto path=doc->recovery;auto pending=doc->backupPending;auto post=window_.Dispatcher();auto alive=alive_;
        doc->worker->Submit([this,path,pending,post,alive](Engine& e){try{if(e.open&&e.document.Info().dirty)e.document.Snapshot(path);}catch(...){post.Post([this,alive]{if(alive->load())status_->Text(L"有文档自动备份失败，请及时保存各标签页。");});}pending->store(false);});
    }
}
void Application::Copy(int p,std::optional<Rect> selection){
    // 只读操作：不经过 Task，文档版本不变，页面瓦片、链接和文字选择都保留。
    if(!loaded_||busy_)return;auto post=window_.Dispatcher();auto alive=alive_;
    worker_->Submit([this,post,alive,p,selection](Engine& e){
        if(!e.open)return;
        try{auto text=e.document.Text(p,selection);
            post.Post([this,alive,text=std::move(text)]{if(!alive->load())return;
                try{Clipboard(static_cast<HWND>(window_.NativeHandle()),text);status_->Text(text.empty()?L"所选区域没有可复制的文字":L"文字已复制到剪贴板");}catch(const std::exception&ex){Fail(ErrorText(ex));}});
        }catch(const std::exception& ex){auto message=ErrorText(ex);post.Post([this,alive,message]{if(alive->load())Fail(message);});}
    });
}
void Application::CopySelection(const PdfCanvas::TextSelection& selection){
    if(!loaded_||busy_||selection.page<0)return;auto post=window_.Dispatcher();auto alive=alive_;
    worker_->Submit([this,post,alive,selection](Engine& e){
        if(!e.open)return;
        try{auto text=SelectedText(e.document,selection);
            post.Post([this,alive,text=std::move(text)]{if(!alive->load())return;
                try{Clipboard(static_cast<HWND>(window_.NativeHandle()),text);status_->Text(text.empty()?L"所选范围没有可复制的文字":L"已复制 "+std::to_wstring(text.size())+L" 个字符");}catch(const std::exception&ex){Fail(ErrorText(ex));}});
        }catch(const std::exception& ex){auto message=ErrorText(ex);post.Post([this,alive,message]{if(alive->load())Fail(message);});}
    });
}
void Application::HighlightSelection(const PdfCanvas::TextSelection& selection){
    if(!loaded_||busy_||selection.page<0)return;
    if(selection.spans.size()>1){
        // 跨页：每页各加一个高亮批注（同一任务，逐页可撤销）；未显示过的页由引擎按起止点换算。
        const auto spans=selection.spans;const auto style=toolStyles_[static_cast<size_t>(Tool::Highlight)];const float opacity=toolOpacities_[static_cast<size_t>(Tool::Highlight)];
        canvas_->ClearTextSelection();
        Task(L"高亮选中文字",[spans,style,opacity](Engine& e,const Cancel& cancel){
            int added=0;
            for(const auto& s:spans){
                CheckCancel(cancel);Point a=s.start,b=s.end;
                if(!e.document.ResolveSelection(s.page,a,b,s.fromStart,s.toEnd))continue;
                const auto quads=e.document.HighlightQuads(s.page,a,b);if(quads.empty())continue;
                float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;
                for(const auto& q:quads)for(const auto& pt:{q.ul,q.ur,q.ll,q.lr}){x0=std::min(x0,pt.x);y0=std::min(y0,pt.y);x1=std::max(x1,pt.x);y1=std::max(y1,pt.y);}
                e.document.AddAnnotation(s.page,Tool::Highlight,{x0,y0,std::max(2.0f,x1-x0),std::max(2.0f,y1-y0)},{},{},{a,b},12,opacity,style,true);++added;
            }
            if(!added)throw std::runtime_error("No text selected. Hold Ctrl and drag for an area highlight.");
        },[this,count=spans.size()]{status_->Text(L"已高亮跨 "+std::to_wstring(count)+L" 页的选中文字  ·  Ctrl+Z 逐页撤销");});
        return;
    }
    if(selection.quads.empty())return;
    float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;
    for(const auto& q:selection.quads)for(const auto& pt:{q.ul,q.ur,q.ll,q.lr}){x0=std::min(x0,pt.x);y0=std::min(y0,pt.y);x1=std::max(x1,pt.x);y1=std::max(y1,pt.y);}
    canvas_->ClearTextSelection();
    Annotate(selection.page,Tool::Highlight,{x0,y0,std::max(2.0f,x1-x0),std::max(2.0f,y1-y0)},{selection.start,selection.end});
}
void Application::PushHistory(){
    if(!loaded_)return;
    back_.push_back(canvas_->CurrentView());if(back_.size()>50)back_.erase(back_.begin());
    forward_.clear();
}
void Application::Navigate(int direction){
    auto& from=direction<0?back_:forward_;auto& to=direction<0?forward_:back_;
    if(from.empty()){status_->Text(direction<0?L"没有可返回的位置":L"没有可前进的位置");return;}
    to.push_back(canvas_->CurrentView());
    auto state=from.back();from.pop_back();state.rotation=canvas_->ViewRotation();   // 返回 / 前进只换位置，不撤销视图旋转
    canvas_->RestoreView(state);Page(state.page,false);thumbs_->GoTo(state.page);
    status_->Text(direction<0?L"已返回上一位置 · Alt+→ 前进":L"已前进 · Alt+← 返回");
}
void Application::FollowLink(const Link& link){
    if(!loaded_)return;
    if(!link.External()){
        if(link.page<0||link.page>=static_cast<int>(info_.pages.size()))return;
        PushHistory();canvas_->GoToPoint(link.page,link.targetY);Page(link.page,false);thumbs_->GoTo(link.page);
        status_->Text(L"已跳转到第 "+std::to_wstring(link.page+1)+L" 页 · Alt+← 返回");return;
    }
    // 只打开网页与邮件地址；其它协议（file:、脚本等）可能启动本机程序，拒绝。
    std::wstring lower=link.uri;for(auto& ch:lower)ch=static_cast<wchar_t>(towlower(ch));
    const bool safe=lower.rfind(L"http://",0)==0||lower.rfind(L"https://",0)==0||lower.rfind(L"mailto:",0)==0;
    if(!safe){window_.Alert(L"已阻止此链接",L"出于安全考虑，LumenPDF 只打开网页和邮件链接：\n"+link.uri);return;}
    const auto uri=link.uri;
    window_.Confirm(L"打开外部链接？",uri,[this,uri](bool yes){
        if(!yes)return;
        if(reinterpret_cast<INT_PTR>(ShellExecuteW(static_cast<HWND>(window_.NativeHandle()),L"open",uri.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)Fail(L"系统没有找到可以打开此链接的程序。");
    },L"打开",L"取消");
}
void Application::ShowCanvasMenu(lumen::Point point){
    if(!loaded_||busy_||textEditor_.Active())return;
    const auto selection=canvas_->Selected();const bool hasText=canvas_->HasTextSelection();
    Menu menu;
    menu.AddItem(L"复制",[this,selection]{CopySelection(selection);}).Shortcut(L"Ctrl+C").Glyph(icon::kCopy).Disabled(!hasText);
    menu.AddItem(L"高亮选中文字",[this,selection]{HighlightSelection(selection);}).Glyph(ui::Marker).Disabled(!hasText);
    menu.AddItem(L"涂黑选中文字",[this,selection]{RedactSelection(selection);}).Glyph(ui::Rectangle).Disabled(!hasText);
    menu.AddItem(L"搜索选中文字",[this,selection]{
        auto post=window_.Dispatcher();auto alive=alive_;
        worker_->Submit([this,post,alive,selection](Engine& e){
            if(!e.open)return;std::wstring text;
            try{text=SelectedText(e.document,selection);}catch(...){return;}
            for(auto& ch:text)if(ch==L'\r'||ch==L'\n'||ch==L'\t')ch=L' ';
            while(!text.empty()&&text.back()==L' ')text.pop_back();
            if(text.size()>80)text.resize(80);
            post.Post([this,alive,text]{if(!alive->load()||text.empty())return;if(mode_!=0)Mode(0);search_->Text(text);Search();});
        });
    }).Glyph(icon::kSearch).Disabled(!hasText);
    menu.AddSeparator();
    if(redacting_){
        menu.AddItem(L"应用涂黑并另存副本…",[this]{ShowRedactionDialog();}).Glyph(ui::Rectangle).Disabled(redactMarks_.empty());
        menu.AddItem(L"退出涂黑模式",[this]{ExitRedaction(false);}).Shortcut(L"Esc").Glyph(icon::kClose);
        menu.AddSeparator();
    }
    menu.AddItem(L"复制本页文字",[this]{Copy(page_);}).Glyph(icon::kCopy);
    if(aloud_.active)menu.AddItem(L"停止朗读",[this]{StopReading();}).Shortcut(L"Esc");
    else if(canvas_->HasTextSelection())menu.AddItem(L"朗读选中文字",[this]{ReadSelection();});
    else menu.AddItem(L"朗读本页",[this]{ReadAloud(false);}).Shortcut(L"Ctrl+Shift+V");
    menu.AddItem(L"返回上一位置",[this]{Navigate(-1);}).Shortcut(L"Alt+Left").Glyph(icon::kArrowLeft).Disabled(back_.empty());
    menu.AddSeparator();
    menu.AddItem(L"适合页面",[this]{canvas_->FitPage();}).Shortcut(L"Ctrl+0").Glyph(ui::Fit);
    menu.AddItem(L"适合宽度",[this]{canvas_->FitWidth();}).Shortcut(L"Ctrl+2").Glyph(icon::kMaximize);
    menu.AddItem(L"框选放大",[this]{ToggleZoomBox();}).Shortcut(L"Z").Glyph(icon::kSearch);
    if(canvas_->CanZoomBack())menu.AddItem(L"返回框选前的缩放",[this]{ZoomBackKey();}).Shortcut(L"Shift+Z");
    menu.AddItem(L"放大镜",[this]{ToggleMagnifier();}).Shortcut(L"L").Checked(canvas_->MagnifierActive());
    menu.AddItem(L"向右旋转视图",[this]{RotateView(90);}).Shortcut(L"Ctrl+Shift+=").Glyph(icon::kRefresh);
    menu.AddItem(L"向左旋转视图",[this]{RotateView(-90);}).Shortcut(L"Ctrl+Shift+-");
    if(canvas_->ViewRotation()){
        menu.AddItem(L"恢复视图方向",[this]{RotateView(0);});
        menu.AddItem(L"按此方向旋转页面（写入文件）",[this]{ApplyViewRotation();}).Disabled(busy_);
    }
    if(canvas_->HandTool())menu.AddItem(L"选择工具",[this]{SetHand(false);}).Shortcut(L"V").Glyph(ui::Cursor);
    else menu.AddItem(L"手型工具",[this]{SetHand(true);}).Shortcut(L"H").Glyph(ui::Hand);
    menu.AddSeparator();
    menu.AddItem(L"粘贴图片",[this]{PasteFromClipboard(PasteIntent::Annotation);}).Shortcut(L"Ctrl+V").Glyph(icon::kImage).Disabled(!ClipboardHasContent());
    menu.AddItem(L"打印…",[this]{Print();}).Shortcut(L"Ctrl+P").Glyph(icon::kPrint);
    menu.Popup(window_,point);
}
void Application::PrintExecute(){
    if(textEditor_.Active()){FinishText(true,[this]{PrintExecute();});return;}
    if(!loaded_||busy_||window_.DialogActive())return;
    PrintSettings settings=printSettings_;std::wstring error;
    // 预览中选择的页码（每次打开预览按当前文档重新计算）；越界的页码丢弃。
    std::erase_if(settings.pages,[this](int p){return p<0||p>=static_cast<int>(info_.pages.size());});
    const bool landscape=ResolveLandscape(settings,info_.pages);
    HDC dc=nullptr;fs::path output;
    // 自动化测试：LPDF_PRINT_TO 指定输出文件，直接使用“Microsoft Print to PDF”，不弹对话框。
    wchar_t target[MAX_PATH]{};
    if(GetEnvironmentVariableW(L"LPDF_PRINT_TO",target,MAX_PATH)){
        output=target;dc=CreatePrinterDC(L"Microsoft Print to PDF",landscape);
        if(settings.pages.empty())for(int p=0;p<static_cast<int>(info_.pages.size());++p)settings.pages.push_back(p);
        if(!dc)error=L"未找到 Microsoft Print to PDF 打印机。";
    }else dc=ChoosePrinter(static_cast<HWND>(window_.NativeHandle()),static_cast<int>(info_.pages.size()),page_,settings,error,landscape);
    if(!dc){if(!error.empty())Fail(error);return;}
    if(PlanPrint(settings).empty()){DeleteDC(dc);window_.Alert(L"没有可打印页面",L"页码范围与奇偶页过滤后没有页面。");return;}
    const std::wstring title=source_.empty()?name_->Text():source_.filename().wstring();
    if(!BeginPrintJob(dc,title,output)){DeleteDC(dc);Fail(L"打印机没有接受此任务。请检查打印机是否在线。");return;}
    auto post=window_.Dispatcher();auto alive=alive_;int total=0;for(const auto& sheet:PlanPrint(settings))for(int p:sheet)if(p>=0)++total;
    keepSearch_=true;
    auto release=[dc]{DeleteDC(dc);};
    Task(L"打印 "+std::to_wstring(total)+L" 页",[dc,settings,post,alive,this](Engine& e,const Cancel& c){
        PrintPages(e.document,dc,settings,c,[post,alive,this](int done,int all){
            post.Post([this,alive,done,all]{if(alive->load())status_->Text(L"正在打印 "+std::to_wstring(done)+L" / "+std::to_wstring(all)+L" 面…");});
        });
    },[this,release,total]{release();status_->Text(L"已发送 "+std::to_wstring(total)+L" 页到打印机");},{},
    [this,release](std::wstring message){release();Fail(L"打印未完成：" +message);},[this,release]{release();status_->Text(L"已取消打印，打印队列中不会留下未完成的文档");});
}
void Application::SavePosition(){
    if(!loaded_||source_.empty()||!canvas_)return;
    const auto state=canvas_->CurrentView();
    ReadingPosition position;position.path=source_;position.page=state.page;position.offset=state.offset;
    position.fit=static_cast<int>(state.fit);position.zoom=state.zoom;position.saved=UnixNow();
    positions_.Remember(std::move(position));
}
bool Application::RestorePosition(){
    const auto* saved=positions_.Find(source_);
    if(!saved||saved->page<0||saved->page>=static_cast<int>(info_.pages.size()))return false;
    PdfCanvas::ViewState state;state.page=saved->page;state.offset=saved->offset;state.fit=static_cast<PdfCanvas::Fit>(std::clamp(saved->fit,0,2));state.zoom=saved->zoom;
    if(state.page==0&&state.offset<1&&state.fit==PdfCanvas::Fit::Page)return false;
    canvas_->RestoreView(state);Page(state.page,false);thumbs_->GoTo(state.page);
    status_->Text(L"已回到上次阅读位置：第 "+std::to_wstring(state.page+1)+L" 页");
    return true;
}
void Application::AcceptExternal(const fs::path& path){
    if(!path.empty()&&settings_.externalNewWindow&&loaded_&&!(!source_.empty()&&RecentFiles::SamePath(path,source_))){
        if(LaunchNewWindow(path))return;
    }
    BringToFront(window_.NativeHandle());
    if(path.empty())return;
    if(loaded_&&!source_.empty()&&RecentFiles::SamePath(path,source_)){if(home_)Home(false);return;}
    OpenFiles({path});
}
bool Application::InPreview()const{return preview_&&loaded_&&source_.empty()&&name_->Text()==L"合并预览.pdf";}
void Application::ClosePreview(bool backToMerge){
    if(!InPreview()||!activeDocument_)return;const auto id=activeDocument_->id;
    Guard([this,id,backToMerge]{DropDocument(id);if(backToMerge){home_=false;Mode(3);}});
}
void Application::UpdateTitle(){
    const bool preview=InPreview();
    if(previewBar_){
        previewBar_->Visible(preview&&!home_&&mode_!=3);
        if(preview){
            std::wstring text=L"合并预览";
            if(previewFiles_)text+=L" · 来自 "+std::to_wstring(previewFiles_)+L" 个文件";
            text+=L" · "+std::to_wstring(info_.pages.size())+L" 页 · 临时文件，关闭前请保存";
            previewText_->Text(text);
        }
    }
    // 预览已另存或换成其它文档后，释放临时目录。
    if(!preview&&preview_&&!(loaded_&&source_.empty()&&name_->Text()==L"合并预览.pdf"))preview_.reset();
    std::wstring file=loaded_?name_->Text():std::wstring{};
    name_->Visible(loaded_);titleSep_->Visible(loaded_);
    dirtyMark_->Text(loaded_&&info_.dirty?L"● 未保存":L"");
    dirtyMark_->ToolTip(loaded_&&info_.dirty?L"有尚未保存的修改  ·  Ctrl+S 保存":L"");
    name_->ToolTip(source_.empty()?L"":source_.wstring());
    window_.Title(file.empty()?std::wstring(L"LumenPDF"):file+(info_.dirty?L" · 未保存":L"")+L" — LumenPDF");
    SyncDocumentTabs();
}
void Application::SidePanel(int index){
    sidePanel_=std::clamp(index,0,3);
    searchPanel_->Visible(sidePanel_==3);
    thumbs_->Visible(sidePanel_==0);
    const bool tree=sidePanel_==1&&!info_.outline.empty();
    outlineTree_->Visible(tree&&!outlineArranging_);outlineEdit_->Visible(tree&&outlineArranging_);outline_->Visible(sidePanel_==1&&!tree);
    outlineBar_->Visible(sidePanel_==1&&loaded_);
    annotationPanel_->Visible(sidePanel_==2);
    if(sidePanel_==0)thumbs_->GoTo(page_);
    if(sidePanel_==1)SyncOutline(page_);
    if(sidePanel_==2)RequestAnnotationList();
}
void Application::RebuildOutline(){
    // 平铺的（标题, 深度）序列 → 父节点数组；深度跳级时挂到最近的较浅节点上。
    std::vector<size_t> parents(info_.outline.size(),TreeView::kNone);std::vector<size_t> stack;
    for(size_t i=0;i<info_.outline.size();++i){
        const auto depth=static_cast<size_t>(std::max(0,info_.outline[i].depth));
        if(stack.size()>depth)stack.resize(depth);
        parents[i]=stack.empty()?TreeView::kNone:stack.back();
        stack.push_back(i);
    }
    outlineSync_=true;
    outlineTree_->SetFlatData(parents);
    // 条目不多时全部展开，否则只展开第一层。
    if(info_.outline.size()<=60)outlineTree_->ExpandAll();
    else for(size_t i=0;i<parents.size();++i)if(parents[i]==TreeView::kNone)outlineTree_->Expand(i);
    outlineSync_=false;
    outline_->ItemCount(0,false);outline_->EmptyHint(L"点下方 ＋ 在当前页添加书签");
    if(outlineEdit_->ItemCount()!=info_.outline.size())outlineEdit_->ItemCount(info_.outline.size(),false);else outlineEdit_->RefreshItems();
    if(sidePanel_==1)SidePanel(1);
}
void Application::SyncOutline(int page){
    if(sidePanel_!=1||info_.outline.empty()||!outlineTree_->Visible())return;
    // 当前页所在的章节：页码不超过当前页的最后一个目录项。
    size_t best=TreeView::kNone;
    for(size_t i=0;i<info_.outline.size();++i)if(info_.outline[i].page<=page&&(best==TreeView::kNone||info_.outline[i].page>=info_.outline[best].page))best=i;
    if(best==TreeView::kNone||outlineTree_->SelectedId()==best)return;
    outlineSync_=true;outlineTree_->SelectedId(best);outlineSync_=false;
}
void Application::RequestAnnotationList(){
    if(!loaded_||annotRowsLoading_)return;
    const auto gen=generation_->load();
    if(gen==annotRowsGeneration_)return;
    annotRowsLoading_=true;
    auto post=window_.Dispatcher();auto alive=alive_;const int pages=static_cast<int>(info_.pages.size());
    worker_->Submit([this,post,alive,gen,pages](Engine& e){
        std::vector<AnnotationRow> rows;
        try{
            if(e.open)for(int p=0;p<std::min(pages,5000);++p){
                if(!alive->load()||generation_->load()!=gen)break;
                for(auto& a:e.document.Annotations(p))if(a.type!=Tool::Select)rows.push_back({p,std::move(a)});
                if(rows.size()>=5000)break;
            }
        }catch(...){}
        post.Post([this,alive,gen,rows=std::move(rows)]()mutable{
            if(!alive->load())return;annotRowsLoading_=false;
            if(generation_->load()!=gen){if(sidePanel_==2)RequestAnnotationList();return;}
            allAnnotRows_=std::move(rows);annotRowsGeneration_=gen;FilterAnnotations();
        });
    },false);
}
void Application::SetTone(PageTone tone,bool save){
    tone_=tone;canvas_->Tone(tone);thumbs_->Tone(tone);if(referenceCanvas_)referenceCanvas_->Tone(tone);
    static const wchar_t* names[]={L"普通",L"护眼",L"夜间"};
    if(toneButton_){toneButton_->Text(std::wstring(L"页面：")+names[static_cast<int>(tone)]);toneButton_->Glyph(tone==PageTone::Night?icon::kMoon:icon::kSun);toneButton_->Kind(tone==PageTone::Normal?ButtonKind::Subtle:ButtonKind::Standard);}
    if(save){SaveSettings();status_->Text(std::wstring(L"页面配色：")+names[static_cast<int>(tone)]+L"（只影响显示，不修改文档）");}
}
void Application::ShowToneMenu(){
    Menu menu;
    menu.AddItem(L"普通",[this]{SetTone(PageTone::Normal);}).RadioGroup(L"tone").Checked(tone_==PageTone::Normal);
    menu.AddItem(L"护眼（米黄纸）",[this]{SetTone(PageTone::Sepia);}).RadioGroup(L"tone").Checked(tone_==PageTone::Sepia);
    menu.AddItem(L"夜间（深底浅字）",[this]{SetTone(PageTone::Night);}).RadioGroup(L"tone").Checked(tone_==PageTone::Night).Shortcut(L"Ctrl+Alt+N");
    menu.PopupTo(*toneButton_);
}
void Application::ShowPageMenu(lumen::Point point,int page){
    if(!loaded_||busy_)return;
    Menu menu;
    if(page<0){
        menu.AddItem(L"在末尾插入空白页",[this]{const int last=static_cast<int>(info_.pages.size())-1;Task(L"插入页面",[last](Engine&e,const Cancel&){e.document.InsertBlank(last);});}).Glyph(icon::kAdd);
        menu.Popup(window_,point);return;
    }
    const auto pages=SelectedPages();const bool many=pages.size()>1;
    const std::wstring label=many?std::to_wstring(pages.size())+L" 页":L"第 "+std::to_wstring(page+1)+L" 页";
    menu.AddHeader(label);
    if(mode_!=0)menu.AddItem(L"在阅读视图中打开",[this,page]{Mode(0);Page(page,true);}).Glyph(icon::kView);
    menu.AddItem(L"复制本页文字",[this,page]{Copy(page);}).Glyph(icon::kCopy).Disabled(many);
    menu.AddSeparator();
    menu.AddItem(L"向右旋转 90°",[this,pages]{Task(L"旋转页面",[pages](Engine&e,const Cancel&){e.document.RotatePages(pages,90);});}).Glyph(icon::kRefresh);
    menu.AddItem(L"在后面插入空白页",[this,page]{Task(L"插入页面",[page](Engine&e,const Cancel&){e.document.InsertBlank(page);});}).Glyph(icon::kAdd);
    menu.AddItem(L"提取为新 PDF…",[this,pages]{auto path=Destination(L"提取页面.pdf");if(!path.empty())Task(L"提取页面",[pages,path](Engine&e,const Cancel&){e.document.Extract(pages,path);});}).Glyph(icon::kDownload);
    menu.AddSeparator();
    menu.AddItem(L"删除"+label,[this,pages,label]{
        if(pages.size()>=info_.pages.size()){window_.Alert(L"无法删除",L"文档至少需要保留一页。");return;}
        window_.Confirm(L"删除"+label+L"？",L"保存前可以撤销。",[this,pages](bool yes){if(yes)Task(L"删除页面",[pages](Engine&e,const Cancel&){e.document.DeletePages(pages);});},L"删除",L"取消");
    }).Glyph(icon::kDelete);
    menu.Popup(window_,point);
}
void Application::LoadSettings(){
    PWSTR local=nullptr;
    if(!smoke_&&SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))){
        const fs::path root=fs::path(local)/L"LumenPDF";CoTaskMemFree(local);
        settingsFile_=root/L"settings.txt";settings_=LoadSettingsFile(settingsFile_);
        log::Init(settings_.logging?root/L"logs":fs::path{});
    }
    wchar_t forced[8]{};
    if(GetEnvironmentVariableW(L"LPDF_TONE",forced,8))settings_.tone=std::clamp(_wtoi(forced),0,2);
    if(GetEnvironmentVariableW(L"LPDF_LAYOUT",forced,8))settings_.layout=std::clamp(_wtoi(forced),0,2);
    SetTone(static_cast<PageTone>(settings_.tone),false);ApplyLayout(settings_.layout,false);
    SetSidebarWidth(static_cast<float>(settings_.sidebarWidth),false);SetPaged(settings_.paged,false);
    if(settings_.sidebarTab!=sidePanel_){const int tab=settings_.sidebarTab;sideTabs_->SelectedIndex(tab);SidePanel(tab);}
}
void Application::SaveSettings(){
    settings_.tone=static_cast<int>(tone_);
    if(!SaveSettingsFile(settingsFile_,settings_)&&!settingsFile_.empty())log::Warn(L"设置保存失败");
}
void Application::ViewSwitch(Panel& parent,int bar){
    // 同一组三个按钮同时出现在阅读与页面工具栏：单页 / 双页阅读，网格进入“页面”视图（双击缩略图回到阅读）。
    RegisterLayoutIcons();
    auto& group=parent.Add<Row>();group.Spacing(2);
    const wchar_t* glyphs[]={kSinglePageIcon,kTwoPageIcon,kPageGridIcon};
    const wchar_t* names[]={L"单页阅读",L"双页阅读",L"页面网格"};
    for(int i=0;i<3;++i){
        auto& b=Btn(group,names[i],[this,i]{ChooseView(i);});
        b.Glyph(glyphs[i]).Text(L"").AccessibleName(names[i]);
        viewButtons_[bar][i]=&b;
    }
    SyncViewSwitch();
}
void Application::ChooseView(int view){
    if(view==2){if(mode_!=2)Mode(2);status_->Text(L"页面网格  ·  拖动缩略图重排，双击在阅读视图打开");return;}
    // 双页沿用上次选择的封面方式（设置里可改）；从网格切回时先恢复阅读视图并保持当前页。
    const int layout=view==0?0:(settings_.layout!=0?settings_.layout:lastSpread_);
    ApplyLayout(layout);
    if(mode_==2)Mode(0);
}
void Application::SyncViewSwitch(){
    const int active=mode_==2?2:settings_.layout==0?0:1;
    const wchar_t* tips[]={L"单页连续阅读  ·  Ctrl+Shift+D 切换单 / 双页",
        settings_.layout==2||(settings_.layout==0&&lastSpread_==2)?L"双页阅读  ·  Ctrl+Shift+D 切换单 / 双页":L"双页阅读（封面单独）  ·  Ctrl+Shift+D 切换单 / 双页",
        L"页面网格：查看全部页面，拖动重排，双击打开"};
    for(auto& bar:viewButtons_)for(int i=0;i<3;++i)if(bar[i]){bar[i]->Kind(i==active?ButtonKind::Standard:ButtonKind::Subtle);bar[i]->ToolTip(tips[i]);}
}
void Application::ApplyLayout(int layout,bool save){
    settings_.layout=std::clamp(layout,0,2);if(settings_.layout!=0)lastSpread_=settings_.layout;
    canvas_->Spread(settings_.layout!=0,settings_.layout==1);
    SyncViewSwitch();
    if(save){SaveSettings();status_->Text(settings_.layout==0?L"单页连续阅读":settings_.layout==1?L"双页阅读（封面单独）  ·  Ctrl+Shift+D 切换":L"双页阅读  ·  Ctrl+Shift+D 切换");}
}
void Application::ShowMoreMenu(){
    Menu menu;const bool doc=loaded_&&!busy_;
    menu.AddItem(L"新窗口",[this]{if(!LaunchNewWindow())Fail(L"无法启动新窗口。");}).Glyph(icon::kAdd).Shortcut(L"Ctrl+Shift+W");
    menu.AddItem(L"在新窗口中打开…",[this]{auto files=Pick(false,AllFilter);if(!files.empty()&&!LaunchNewWindow(files[0]))Fail(L"无法启动新窗口。");}).Glyph(icon::kFolderOpen);
    menu.AddItem(L"下一个文档标签",[this]{CycleDocument(1);}).Glyph(icon::kMore).Shortcut(L"Ctrl+Tab");
    menu.AddSeparator();
    menu.AddHeader(L"视图");
    menu.AddItem(L"全屏",[this]{FullScreen();}).Checked(fullScreen_).Shortcut(L"F11");
    menu.AddItem(L"阅读模式（隐藏工具栏）",[this]{ReadingMode();}).Checked(readingMode_).Shortcut(L"Ctrl+H").Disabled(!doc);
    menu.AddItem(L"演示（全屏逐页）",[this]{Present(true);}).Shortcut(L"F5").Disabled(!doc||home_||mode_==3);
    menu.AddItem(L"框选放大",[this]{ToggleZoomBox();}).Shortcut(L"Z").Disabled(!doc||home_||mode_==3);
    menu.AddItem(L"放大镜",[this]{ToggleMagnifier();}).Shortcut(L"L").Checked(canvas_->MagnifierActive()).Disabled(!doc||home_||mode_==3);
    menu.AddHeader(L"朗读");
    if(!aloud_.active){
        menu.AddItem(L"朗读本页",[this]{ReadAloud(false);}).Shortcut(L"Ctrl+Shift+V").Disabled(!doc||home_||mode_==3);
        menu.AddItem(L"从本页朗读到文末",[this]{ReadAloud(true);}).Shortcut(L"Ctrl+Shift+B").Disabled(!doc||home_||mode_==3);
    }else{
        menu.AddItem(speech_&&speech_->Paused()?L"继续朗读":L"暂停朗读",[this]{ReadPause();}).Shortcut(L"Ctrl+Shift+C");
        menu.AddItem(L"停止朗读",[this]{StopReading();}).Shortcut(L"Ctrl+Shift+E");
    }
    menu.AddItem(L"朗读控制与设置（语速、声音）…",[this]{window_.SetTimeout(.05f,[this]{ShowReadMenu();});}).Disabled(!doc);
    menu.AddItem(canvas_->ViewRotation()?L"旋转视图（当前 "+std::to_wstring(canvas_->ViewRotation())+L"°，再转 90°）":std::wstring(L"旋转视图 90°（不改文件）"),[this]{RotateView(90);}).Shortcut(L"Ctrl+Shift+=").Disabled(!doc||home_||mode_==3);
    if(canvas_->ViewRotation())menu.AddItem(L"恢复视图方向",[this]{RotateView(0);});
    menu.AddItem(L"翻页模式（整页翻动）",[this]{SetPaged(!settings_.paged);}).Checked(settings_.paged);
    menu.AddItem(L"自动滚动（Esc 停止）",[this]{autoScroll_=!autoScroll_;if(autoScroll_){status_->Text(L"自动滚动中，Esc 停止");autoScrollFrame_=window_.OnFrame([this](float dt){if(!autoScroll_||!loaded_)return false;if(!busy_&&!home_&&mode_==0&&!window_.DialogActive())canvas_->AutoScroll(std::min(dt,.1f)*35);return true;});}}).Checked(autoScroll_).Disabled(!doc);
    menu.AddItem(L"隐藏全部批注",[this]{if(textEditor_.Active()){FinishText(true);return;}const bool hide=!canvas_->AnnotationsHidden();ChooseTool(Tool::Select);canvas_->HideAnnotations(hide);thumbs_->HideAnnotations(hide);canvas_->Selection(page_,hide?std::vector<Annotation>{}:annotations_);status_->Text(hide?L"批注已隐藏（只影响显示，不改变保存或打印设置）":L"批注已显示");}).Checked(canvas_->AnnotationsHidden()).Disabled(!doc);
    menu.AddSeparator();
    menu.AddHeader(L"文档工具");
    menu.AddItem(L"压缩 PDF(&C)…",[this]{ShowCompressDialog();}).Glyph(ui::Compress).Disabled(!doc);
    menu.AddItem(L"拆分 PDF(&S)…",[this]{ShowSplitDialog();}).Glyph(ui::Split).Disabled(!doc);
    menu.AddItem(L"页眉页脚、页码与水印(&H)…",[this]{ShowDecorateDialog();}).Glyph(ui::Watermark).Disabled(!doc);
    menu.AddItem(L"重置表单(&R)…",[this]{ResetFormFields();}).Glyph(icon::kRefresh).Disabled(!doc||!info_.hasForm);
    menu.AddItem(L"高亮表单字段",[this]{canvas_->HighlightFields(!canvas_->HighlightFields());status_->Text(canvas_->HighlightFields()?L"已显示表单字段高亮":L"已隐藏表单字段高亮（字段仍可单击填写）");}).Checked(canvas_->HighlightFields()).Disabled(!doc||!info_.hasForm);
    if(redacting_){
        menu.AddItem(L"应用涂黑并另存副本（"+std::to_wstring(redactMarks_.size())+L" 处）…",[this]{ShowRedactionDialog();}).Glyph(ui::Rectangle).Disabled(redactMarks_.empty());
        menu.AddItem(L"涂黑全部搜索结果",[this]{RedactSearchHits();}).Glyph(icon::kSearch).Disabled(hits_.empty());
        menu.AddItem(L"退出涂黑模式",[this]{ExitRedaction(false);}).Glyph(icon::kClose);
    }else{
        menu.AddItem(redactMarks_.empty()?L"涂黑（永久删除内容）(&D)…":L"继续涂黑（"+std::to_wstring(redactMarks_.size())+L" 处标记）",[this]{EnterRedaction();}).Glyph(ui::Rectangle).Disabled(!doc);
        menu.AddItem(L"从剪贴板新建 PDF",[this]{PasteFromClipboard(PasteIntent::NewDocument);}).Shortcut(L"Ctrl+V").Glyph(icon::kImage);
        if(!hits_.empty())menu.AddItem(L"涂黑全部搜索结果（"+std::to_wstring(hits_.size())+L" 处）",[this]{RedactSearchHits();}).Glyph(icon::kSearch).Disabled(!doc);
    }
    menu.AddItem(L"编辑书签(&B)…",[this]{Mode(0);sideTabs_->SelectedIndex(1);SidePanel(1);if(info_.outline.empty())AddBookmark();}).Glyph(icon::kBookmark).Disabled(!doc);
    menu.AddSeparator();
    menu.AddHeader(L"导出");
    menu.AddItem(L"加密或去除密码…",[this]{ShowSecurityDialog();}).Glyph(icon::kLock).Disabled(!doc);
    menu.AddItem(L"导出页面为 PNG 图片…",[this]{ExportImages();}).Glyph(icon::kImage).Disabled(!doc);
    menu.AddItem(L"导出全文为 TXT…",[this]{ExportText();}).Glyph(icon::kDownload).Disabled(!doc);
    menu.AddItem(L"恢复到上次保存…",[this]{if(source_.empty())return;window_.Confirm(L"恢复到上次保存？",L"将放弃当前未保存的修改；原文件不会更改。",[this](bool yes){if(!yes)return;const auto path=source_;Task(L"恢复文件",[path](Engine& e,const Cancel& c){const auto converted=e.converter.Convert(path,{},c);e.document.Reload(converted.pdf);e.open=true;},[this]{StampSource();Page(0,true);});},L"恢复",L"取消");}).Disabled(!doc||source_.empty());
    menu.AddItem(L"打印预览…",[this]{PrintOptions();}).Disabled(!doc);
    menu.AddItem(L"文档属性",[this]{DocumentProperties();}).Glyph(icon::kInfo).Disabled(!loaded_);
    menu.AddSeparator();
    menu.AddItem(L"侧栏宽度…",[this]{TextDialog(L"侧栏宽度",L"输入 180–600 像素。也可以直接拖动侧栏右边缘。",std::to_wstring(settings_.sidebarWidth),L"应用",[this](std::wstring value){int width=_wtoi(value.c_str());if(width<180||width>600){Fail(L"宽度必须是 180–600。");return;}SetSidebarWidth(static_cast<float>(width),true);});});
    menu.AddHeader(L"系统");
    const bool isDefault=IsDefaultPdfHandler();
    menu.AddItem(isDefault?L"已是默认 PDF 阅读器":L"设为默认 PDF 阅读器…",[this]{MakeDefault();}).Glyph(icon::kPin).Checked(isDefault);
    menu.AddItem(L"资源管理器右键“使用 LumenPDF 合并”",[this]{ToggleMergeMenu();}).Glyph(ui::Merge).Checked(MergeMenuRegistered());
    menu.AddSeparator();
    menu.AddItem(L"设置…",[this]{ShowSettings();}).Glyph(icon::kSettings).Shortcut(L"Ctrl+,");
    menu.AddItem(UpdateMenuLabel(),[this]{window_.SetTimeout(.05f,[this]{CheckForUpdates(true);});}).Glyph(icon::kDownload);
    menu.AddItem(L"关于 LumenPDF",[this]{window_.SetTimeout(.05f,[this]{ShowAbout();});}).Glyph(icon::kInfo);
    menu.AddItem(L"打开日志文件夹",[this]{const auto f=log::Folder();if(f.empty()){status_->Text(L"日志已关闭（可在设置中开启）");return;}ShellExecuteW(nullptr,L"open",f.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}).Glyph(icon::kFolder);
    menu.PopupTo(*moreButton_);
}
void Application::ShowSettings(){
    if(window_.DialogActive())return;
    struct Controls{ComboBox* tone{};ComboBox* layout{};ComboBox* fit{};CheckBox* restore{};ComboBox* external{};ComboBox* dpi{};CheckBox* logging{};CheckBox* mergeMenu{};CheckBox* paged{};CheckBox* autoReload{};CheckBox* autoUpdate{};};
    auto c=std::make_shared<Controls>();
    DialogSpec dialog;dialog.title=L"设置";dialog.message=L"偏好保存在本机，立即生效。";dialog.size=DialogSize::Standard;
    const int dpis[]={96,150,200,300};
    dialog.content=[this,c,dpis](Panel& panel){
        auto row=[&](const wchar_t* label)->Row&{auto& r=panel.Add<Row>();r.Spacing(12);r.AlignCross(CrossAlign::Center);auto& l=r.Add<Label>(label,TextRole::Caption);Width(l,120);return r;};
        c->tone=&row(L"页面配色").Add<ComboBox>().AddItems({L"普通",L"护眼（米黄纸）",L"夜间（深底浅字）"}).SelectedIndex(static_cast<int>(tone_)).AccessibleName(L"页面配色");c->tone->Grow();
        c->layout=&row(L"阅读布局").Add<ComboBox>().AddItems({L"单页连续",L"双页（封面单独）",L"双页"}).SelectedIndex(settings_.layout).AccessibleName(L"阅读布局");c->layout->Grow();
        c->fit=&row(L"首次打开").Add<ComboBox>().AddItems({L"适合页面",L"适合宽度"}).SelectedIndex(settings_.defaultFit).AccessibleName(L"首次打开的缩放");c->fit->Grow();
        c->external=&row(L"双击 PDF 时").Add<ComboBox>().AddItems({L"同一窗口的新标签页",L"总是打开新窗口"}).SelectedIndex(settings_.externalNewWindow?1:0).AccessibleName(L"双击 PDF 时");c->external->Grow();
        ptrdiff_t dpiIndex=1;for(ptrdiff_t i=0;i<4;++i)if(dpis[i]==settings_.exportDpi)dpiIndex=i;
        c->dpi=&row(L"图片导出").Add<ComboBox>().AddItems({L"96 DPI（屏幕）",L"150 DPI（默认）",L"200 DPI",L"300 DPI（打印）"}).SelectedIndex(dpiIndex).AccessibleName(L"图片导出分辨率");c->dpi->Grow();
        auto& system=row(L"系统集成");
        const bool isDefault=!smoke_&&IsDefaultPdfHandler();
        auto& makeDefault=system.Add<Button>(isDefault?L"已是默认 PDF 阅读器":L"设为默认 PDF 阅读器…",isDefault?ButtonKind::Subtle:ButtonKind::Standard);
        makeDefault.Glyph(icon::kPin).Height(34).SizeClass(ButtonSize::Small).Role(TextRole::Caption);makeDefault.Enabled(!isDefault);
        makeDefault.OnClick([this]{window_.CloseDialog();window_.Dispatcher().Post([this]{MakeDefault();});});
        c->mergeMenu=&panel.Add<CheckBox>(L"在资源管理器右键菜单中显示“使用 LumenPDF 合并”");c->mergeMenu->Checked(!smoke_&&MergeMenuRegistered());
        c->restore=&panel.Add<CheckBox>(L"重新打开文件时回到上次阅读的位置");c->restore->Checked(settings_.restorePosition);
        c->paged=&panel.Add<CheckBox>(L"翻页模式：一次显示一页，滚动到页边时整页翻过");c->paged->Checked(settings_.paged);
        c->autoReload=&panel.Add<CheckBox>(L"文件被其它程序修改后自动重新载入（有未保存修改时仍会询问）");c->autoReload->Checked(settings_.autoReload);
        c->logging=&panel.Add<CheckBox>(L"记录运行日志（不含文档内容与口令）");c->logging->Checked(settings_.logging);
        c->autoUpdate=&panel.Add<CheckBox>(L"自动检查更新（每天最多一次，只获取版本信息，不上传任何数据）");c->autoUpdate->Checked(settings_.autoUpdate);
        auto& shortcuts=panel.Add<Label>(L"快捷键（沿用 Acrobat）：按住空格拖动平移 · H 手型 / V 选择 · Ctrl+O 打开 · Ctrl+S 保存 · Ctrl+F 查找 · Ctrl+Shift+N 跳页 · Ctrl+D 文档属性 · Ctrl+P 打印 · F4 侧栏 · Ctrl+Alt+N 夜间 · Ctrl+Shift+D 双页 · Ctrl+Shift+W 新窗口 · Ctrl+Tab 切换标签 · Ctrl+W 关闭标签 · Alt+←/→ 返回/前进 · F5 演示 · 双击选词 / 三击选段",TextRole::Caption);
        shortcuts.Secondary(true).Wrap(true);
    };
    dialog.primary={L"保存",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,c,dpis](DialogResult result){
        if(result!=DialogResult::Primary)return;
        settings_.defaultFit=static_cast<int>(std::max<ptrdiff_t>(0,c->fit->SelectedIndex()));
        settings_.restorePosition=c->restore->Checked();
        settings_.autoReload=c->autoReload->Checked();
        settings_.autoUpdate=c->autoUpdate->Checked();
        SetPaged(c->paged->Checked(),false);
        settings_.externalNewWindow=c->external->SelectedIndex()==1;
        settings_.exportDpi=dpis[std::clamp<ptrdiff_t>(c->dpi->SelectedIndex(),0,3)];
        if(!smoke_&&c->mergeMenu->Checked()!=MergeMenuRegistered()){
            if(c->mergeMenu->Checked()){const auto error=RegisterMergeMenu(ExecutablePath());if(!error.empty())Fail(error);}else UnregisterMergeMenu();
        }
        const bool logging=c->logging->Checked();
        if(logging!=settings_.logging){
            settings_.logging=logging;
            if(!settingsFile_.empty())log::Init(logging?settingsFile_.parent_path()/L"logs":fs::path{});
            if(logging)log::Info(L"日志已开启");
        }
        SetTone(static_cast<PageTone>(std::clamp<ptrdiff_t>(c->tone->SelectedIndex(),0,2)),false);
        ApplyLayout(static_cast<int>(std::clamp<ptrdiff_t>(c->layout->SelectedIndex(),0,2)),false);
        SaveSettings();status_->Text(settingsFile_.empty()?L"设置已应用（测试模式不保存）":L"设置已保存");
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::ShowSecurityDialog(){
    if(textEditor_.Active()){FinishText(true,[this]{ShowSecurityDialog();});return;}
    if(!loaded_||busy_||window_.DialogActive())return;
    struct Controls{ComboBox* mode{};PasswordBox* user{};PasswordBox* confirm{};PasswordBox* owner{};CheckBox* print{};CheckBox* copy{};CheckBox* modify{};CheckBox* annotate{};};
    auto c=std::make_shared<Controls>();
    DialogSpec dialog;dialog.title=L"加密或去除密码";
    dialog.message=info_.encrypted?L"当前文件已加密。可另存为不需要密码的副本，或换一个新密码。原文件不会被修改。":L"另存一份带密码（AES-256）的副本。原文件与当前编辑不受影响。";
    dialog.size=DialogSize::Standard;dialog.default_button=DialogCommand::None;
    dialog.content=[this,c](Panel& panel){
        c->mode=&panel.Add<ComboBox>().AddItems({L"设置打开密码",L"仅限制权限（无需密码即可打开）",L"去除所有密码与限制"}).SelectedIndex(info_.encrypted?2:0).AccessibleName(L"加密方式");
        c->user=&panel.Add<PasswordBox>();c->user->Revealable(true);c->user->Placeholder(L"打开密码").AccessibleName(L"打开密码");
        c->confirm=&panel.Add<PasswordBox>();c->confirm->Placeholder(L"再次输入打开密码").AccessibleName(L"确认打开密码");
        c->owner=&panel.Add<PasswordBox>();c->owner->Revealable(true);c->owner->Placeholder(L"权限密码（可选；留空则自动生成随机密码）").AccessibleName(L"权限密码");
        auto& perms=panel.Add<Row>();perms.Spacing(24);
        auto& permsLeft=perms.Add<Column>();permsLeft.Spacing(8);permsLeft.Grow();
        auto& permsRight=perms.Add<Column>();permsRight.Spacing(8);permsRight.Grow();
        c->print=&permsLeft.Add<CheckBox>(L"允许打印");c->print->Checked(true);
        c->copy=&permsLeft.Add<CheckBox>(L"允许复制文字");c->copy->Checked(true);
        c->modify=&permsRight.Add<CheckBox>(L"允许修改与整理页面");c->modify->Checked(true);
        c->annotate=&permsRight.Add<CheckBox>(L"允许批注与填写表单");c->annotate->Checked(true);
        panel.Add<Label>(L"提示：权限限制依赖阅读器自觉遵守，只有“打开密码”能真正阻止他人查看内容。请妥善保管密码，遗失后无法找回。",TextRole::Caption).Secondary(true).Wrap(true);
        auto sync=[c](ptrdiff_t,ptrdiff_t now){
            const bool open=now==0,none=now==2;
            c->user->Visible(open);c->confirm->Visible(open);c->owner->Visible(!none);
            for(auto* box:{c->print,c->copy,c->modify,c->annotate})box->Visible(!none);
        };
        c->mode->OnSelectionChanged(sync);sync(-1,c->mode->SelectedIndex());
    };
    dialog.primary={L"另存副本…",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,c](DialogResult result){
        if(result!=DialogResult::Primary)return;
        const auto mode=c->mode->SelectedIndex();
        Security sec;
        if(mode!=2){
            sec.ownerPassword=c->owner->Text();
            sec.allowPrint=c->print->Checked();sec.allowCopy=c->copy->Checked();sec.allowModify=c->modify->Checked();sec.allowAnnotate=c->annotate->Checked();
        }
        if(mode==0){
            sec.userPassword=c->user->Text();
            if(sec.userPassword.empty()){window_.Alert(L"未设置密码",L"请输入打开密码，或选择“仅限制权限”。");return;}
            if(sec.userPassword!=c->confirm->Text()){window_.Alert(L"两次输入的密码不一致",L"请重新输入。文件未保存。");return;}
            if(!sec.ownerPassword.empty()&&sec.ownerPassword==sec.userPassword){window_.Alert(L"密码相同",L"权限密码应与打开密码不同，否则任何能打开文件的人都拥有全部权限。");return;}
        }
        if(mode==1&&sec.allowPrint&&sec.allowCopy&&sec.allowModify&&sec.allowAnnotate){window_.Alert(L"没有限制任何权限",L"请至少取消一项权限，或选择其它方式。");return;}
        const auto stem=source_.empty()?std::wstring(L"文档"):source_.stem().wstring();
        auto path=Destination(stem+(mode==2?L"_无密码.pdf":L"_加密.pdf"));
        if(path.empty())return;
        if(!source_.empty()&&RecentFiles::SamePath(path,source_)&&info_.dirty){window_.Alert(L"请先保存",L"要覆盖当前文件，请先保存修改或选择其它文件名。");return;}
        log::Info(mode==2?L"导出无密码副本":L"导出加密副本");
        Task(mode==2?L"去除密码":L"加密导出",[path,sec](Engine& e,const Cancel& cancel){e.document.ExportSecured(path,sec,cancel);},
            [this,path,mode]{status_->Text((mode==2?L"已另存无密码副本：":L"已另存加密副本：")+path.filename().wstring());window_.ShowToast((mode==2?L"已去除密码：":L"已加密：")+path.filename().wstring(),ToastKind::Success);},
            {},[this](std::wstring error){log::Error(L"加密导出失败："+error);Fail(L"未能导出，目标文件保持不变。\n"+error);});
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::ExportImages(){
    if(textEditor_.Active()){FinishText(true,[this]{ExportImages();});return;}
    if(!loaded_||busy_)return;
    auto pages=mode_==2?SelectedPages():std::vector<int>{};
    const bool selection=pages.size()>1||mode_==2;
    std::wstring label=selection?L"选中的 "+std::to_wstring(pages.size())+L" 页":L"全部 "+std::to_wstring(info_.pages.size())+L" 页";
    if(!selection){pages.resize(info_.pages.size());std::iota(pages.begin(),pages.end(),0);}
    // 选择文件夹：用保存对话框取“第一张图”的路径，文件夹与文件名前缀由它决定。
    const auto stem=source_.empty()?std::wstring(L"页面"):source_.stem().wstring();
    wchar_t buffer[32768]{};wcsncpy_s(buffer,(stem+L"-1.png").c_str(),_TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=static_cast<HWND>(window_.NativeHandle());
    ofn.lpstrFilter=L"PNG 图片\0*.png\0";ofn.lpstrFile=buffer;ofn.nMaxFile=32768;ofn.lpstrDefExt=L"png";
    const std::wstring title=L"导出"+label+L"为 PNG（"+std::to_wstring(settings_.exportDpi)+L" DPI）";ofn.lpstrTitle=title.c_str();
    ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST;
    if(!GetSaveFileNameW(&ofn))return;
    const fs::path chosen=buffer;const auto folder=chosen.parent_path();
    std::wstring prefix=chosen.stem().wstring();
    if(const auto dash=prefix.find_last_of(L'-');dash!=std::wstring::npos&&dash+1<prefix.size()&&std::all_of(prefix.begin()+static_cast<ptrdiff_t>(dash)+1,prefix.end(),iswdigit))prefix.resize(dash);
    if(prefix.empty())prefix=L"页面";
    const float dpi=static_cast<float>(settings_.exportDpi);
    auto count=std::make_shared<size_t>(0);
    log::Info(L"导出 PNG："+std::to_wstring(pages.size())+L" 页");
    Task(L"导出图片",[pages,folder,prefix,dpi,count](Engine& e,const Cancel& cancel){*count=e.document.ExportImages(pages,folder,prefix,dpi,cancel).size();},
        [this,folder,count]{
            status_->Text(L"已导出 "+std::to_wstring(*count)+L" 张图片到 "+folder.wstring());
            window_.ShowToast(L"导出完成："+std::to_wstring(*count)+L" 张 PNG",ToastKind::Success);
        },{},[this](std::wstring error){log::Error(L"导出图片失败："+error);Fail(error);});
}
void Application::ExportText(){
    if(textEditor_.Active()){FinishText(true,[this]{ExportText();});return;}
    if(!loaded_||busy_)return;
    const auto stem=source_.empty()?std::wstring(L"文档"):source_.stem().wstring();
    wchar_t buffer[32768]{};wcsncpy_s(buffer,(stem+L".txt").c_str(),_TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=static_cast<HWND>(window_.NativeHandle());
    ofn.lpstrFilter=L"文本文件\0*.txt\0";ofn.lpstrFile=buffer;ofn.nMaxFile=32768;ofn.lpstrDefExt=L"txt";ofn.Flags=OFN_EXPLORER|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&ofn))return;
    const fs::path path=buffer;
    Task(L"导出文本",[path](Engine& e,const Cancel& cancel){e.document.ExportText(path,cancel);},
        [this,path]{status_->Text(L"已导出文本："+path.filename().wstring());},{},[this](std::wstring error){log::Error(L"导出文本失败："+error);Fail(error);});
}
void Application::DocumentProperties(){
    if(!loaded_||busy_)return;auto metadata=std::make_shared<DocumentMetadata>();keepSearch_=true;
    Task(L"读取文档属性",[metadata](Engine& e,const Cancel&){*metadata=e.document.Metadata();},[this,metadata]{
        struct Controls{TextBox *title{},*author{},*subject{},*keywords{};};auto c=std::make_shared<Controls>();DialogSpec d;d.title=L"文档属性";
        d.message=(source_.empty()?name_->Text():source_.wstring())+L"\n"+metadata->format+L" · "+std::to_wstring(info_.pages.size())+L" 页"+(info_.encrypted?L" · 加密":L"");
        std::error_code error;if(!source_.empty()){const auto bytes=fs::file_size(source_,error);if(!error)d.message+=L" · "+std::to_wstring(bytes/1024)+L" KiB";}
        if(!info_.pages.empty()){const auto& page=info_.pages.front();d.message+=L"\n首页："+std::to_wstring(static_cast<int>(page.width))+L" × "+std::to_wstring(static_cast<int>(page.height))+L" pt";}
        d.message+=L" · "+std::to_wstring(info_.outline.size())+L" 个书签"+(info_.dirty?L" · 有未保存修改":L"");
        d.content=[c,metadata](Panel& p){auto field=[&](const wchar_t* label,const std::wstring& text){p.Add<Label>(label,TextRole::Caption);return &p.Add<TextBox>().Text(text);};c->title=field(L"标题",metadata->title);c->author=field(L"作者",metadata->author);c->subject=field(L"主题",metadata->subject);c->keywords=field(L"关键词",metadata->keywords);
            p.Add<Label>(L"创建程序："+metadata->creator+L"\nPDF 生成器："+metadata->producer+L"\n创建日期（PDF 原始格式）："+metadata->created+L"\n修改日期："+metadata->modified,TextRole::Caption).Wrap(true);
            p.Add<Label>(L"编辑修改 PDF 信息字典；不执行隐私清理，XMP 等隐藏元数据可能仍保留。",TextRole::Caption).Wrap(true);
        };d.primary={L"应用",{}};d.close={L"关闭",{}};d.on_result=[this,c,metadata](DialogResult result){if(result!=DialogResult::Primary)return;auto m=*metadata;m.title=c->title->Text();m.author=c->author->Text();m.subject=c->subject->Text();m.keywords=c->keywords->Text();window_.Dispatcher().Post([this,m]{Task(L"修改属性",[m](Engine& e,const Cancel&){e.document.SetMetadata(m);});});};window_.ShowDialog(std::move(d));
    });
}
void Application::EditPageLabels(){
    if(!loaded_||busy_)return;struct Controls{TextBox *prefix{},*start{};ComboBox* style{};};auto c=std::make_shared<Controls>();const int page=page_;
    DialogSpec d;d.title=L"页码标签";d.message=L"从当前物理页 "+std::to_wstring(page+1)+L" 起设定标签，直到下一个已有标签区段。标签不改变页面上印刷的页码。";
    d.content=[c](Panel& p){p.Add<Label>(L"编号形式",TextRole::Caption);c->style=&p.Add<ComboBox>().AddItems({L"1, 2, 3",L"I, II, III",L"i, ii, iii",L"A, B, C",L"a, b, c",L"只显示前缀"}).SelectedIndex(0);p.Add<Label>(L"前缀",TextRole::Caption);c->prefix=&p.Add<TextBox>();p.Add<Label>(L"起始编号",TextRole::Caption);c->start=&p.Add<TextBox>().Text(L"1");};
    d.primary={L"应用",{}};d.close={L"取消",{}};d.on_result=[this,c,page](DialogResult result){if(result!=DialogResult::Primary)return;const int styles[]={'D','R','r','A','a',0};const int style=styles[std::clamp<ptrdiff_t>(c->style->SelectedIndex(),0,5)];const auto prefix=c->prefix->Text();int start=0;try{size_t end=0;start=std::stoi(c->start->Text(),&end);if(end!=c->start->Text().size())start=0;}catch(...){start=0;}
        window_.Dispatcher().Post([this,page,style,prefix,start]{Task(L"页码标签",[page,style,prefix,start](Engine& e,const Cancel&){e.document.SetPageLabels(page,style,prefix,start);});});};window_.ShowDialog(std::move(d));
}
size_t Application::OutlineSelection()const{
    if(outlineArranging_){const auto n=outlineEdit_->SelectedIndex();return n>=0?static_cast<size_t>(n):TreeView::kNone;}
    const auto id=outlineTree_->SelectedId();return id<info_.outline.size()?id:TreeView::kNone;
}
void Application::CommitOutline(std::vector<OutlineItem> items,std::wstring caption,size_t select){
    if(!loaded_||busy_)return;
    // 规范层级：第一项为 0 级，任何一项最多比上一项深一级。
    for(size_t i=0;i<items.size();++i)items[i].depth=std::clamp(items[i].depth,0,i?items[i-1].depth+1:0);
    auto run=[this,items=std::move(items),caption,select]{
        Task(caption,[items](Engine& e,const Cancel&){e.document.SetOutline(items);},[this,select]{
            if(select<info_.outline.size()){
                outlineSync_=true;
                if(outlineArranging_)outlineEdit_->SelectedIndex(static_cast<ptrdiff_t>(select));else outlineTree_->RevealId(select).SelectedId(select);
                outlineSync_=false;
            }
            status_->Text(L"书签已更新  ·  Ctrl+Z 撤销");
        });
    };
    if(info_.outlineSkipped>0){
        window_.Confirm(L"编辑书签",L"此文档有 "+std::to_wstring(info_.outlineSkipped)+L" 个书签指向外部链接或无效位置，无法按页编辑。继续将移除这些书签（可撤销）。",
            [run](bool yes){if(yes)run();},L"继续",L"取消");
        return;
    }
    run();
}
void Application::TextDialog(std::wstring title,std::wstring message,std::wstring initial,std::wstring action,std::function<void(std::wstring)> then){
    if(window_.DialogActive())return;
    auto box=std::make_shared<TextBox*>(nullptr);
    DialogSpec dialog;dialog.title=std::move(title);dialog.message=std::move(message);dialog.size=DialogSize::Compact;dialog.default_button=DialogCommand::Primary;
    dialog.content=[box,initial](Panel& panel){*box=&panel.Add<TextBox>().Text(initial).AccessibleName(L"名称");(*box)->Focus();};
    dialog.primary={action,{}};dialog.close={L"取消",{}};
    dialog.on_result=[box,then](DialogResult result){
        if(result!=DialogResult::Primary)return;
        auto text=(*box)->Text();
        for(auto& ch:text)if(ch==L'\r'||ch==L'\n'||ch==L'\t')ch=L' ';
        while(!text.empty()&&text.back()==L' ')text.pop_back();
        while(!text.empty()&&text.front()==L' ')text.erase(text.begin());
        if(!text.empty())then(text.substr(0,500));
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::AddBookmark(){
    if(!loaded_||busy_)return;
    const int page=page_;
    TextDialog(L"添加书签",L"书签指向第 "+std::to_wstring(page+1)+L" 页，添加在所选书签之后（同一级）。",L"第 "+std::to_wstring(page+1)+L" 页",L"添加",[this,page](std::wstring title){
        auto items=info_.outline;const auto sel=OutlineSelection();
        size_t at=items.size();int depth=0;
        if(sel!=TreeView::kNone){depth=items[sel].depth;at=sel+1;while(at<items.size()&&items[at].depth>depth)++at;}
        else{
            // 未选中时按页码顺序插入顶层。
            at=0;while(at<items.size()&&(items[at].depth>0||items[at].page<=page))++at;
        }
        items.insert(items.begin()+static_cast<ptrdiff_t>(at),OutlineItem{title,page,depth});
        CommitOutline(std::move(items),L"添加书签",at);
    });
}
void Application::RenameBookmark(){
    const auto sel=OutlineSelection();if(sel==TreeView::kNone||busy_){status_->Text(L"先在目录中选择一个书签");return;}
    TextDialog(L"重命名书签",L"",info_.outline[sel].title,L"重命名",[this,sel](std::wstring title){
        if(sel>=info_.outline.size()||title==info_.outline[sel].title)return;
        auto items=info_.outline;items[sel].title=title;CommitOutline(std::move(items),L"重命名书签",sel);
    });
}
void Application::DeleteBookmark(){
    const auto sel=OutlineSelection();if(sel==TreeView::kNone||busy_){status_->Text(L"先在目录中选择一个书签");return;}
    auto items=info_.outline;size_t end=sel+1;while(end<items.size()&&items[end].depth>items[sel].depth)++end;
    const size_t children=end-sel-1;
    auto remove=[this,items,sel,end]()mutable{
        items.erase(items.begin()+static_cast<ptrdiff_t>(sel),items.begin()+static_cast<ptrdiff_t>(end));
        const size_t next=items.empty()?TreeView::kNone:std::min(sel,items.size()-1);
        CommitOutline(std::move(items),L"删除书签",next);
    };
    if(children)window_.Confirm(L"删除书签“"+items[sel].title+L"”？",L"它的 "+std::to_wstring(children)+L" 个子书签也会一起删除（可撤销）。",[remove](bool yes)mutable{if(yes)remove();},L"删除",L"取消");
    else remove();
}
void Application::MoveBookmark(int delta){
    const auto sel=OutlineSelection();if(sel==TreeView::kNone||busy_)return;
    auto items=info_.outline;const int depth=items[sel].depth;
    size_t end=sel+1;while(end<items.size()&&items[end].depth>depth)++end;
    // 在同级兄弟之间移动（连同子书签）。
    if(delta<0){
        size_t prev=sel;while(prev>0){--prev;if(items[prev].depth<depth){prev=TreeView::kNone;break;}if(items[prev].depth==depth)break;}
        if(prev==TreeView::kNone||prev==sel||items[prev].depth!=depth)return;
        std::rotate(items.begin()+static_cast<ptrdiff_t>(prev),items.begin()+static_cast<ptrdiff_t>(sel),items.begin()+static_cast<ptrdiff_t>(end));
        CommitOutline(std::move(items),L"上移书签",prev);
    }else{
        if(end>=items.size()||items[end].depth!=depth)return;
        size_t next=end+1;while(next<items.size()&&items[next].depth>depth)++next;
        std::rotate(items.begin()+static_cast<ptrdiff_t>(sel),items.begin()+static_cast<ptrdiff_t>(end),items.begin()+static_cast<ptrdiff_t>(next));
        CommitOutline(std::move(items),L"下移书签",sel+(next-end));
    }
}
void Application::IndentBookmark(int delta){
    const auto sel=OutlineSelection();if(sel==TreeView::kNone||busy_)return;
    auto items=info_.outline;const int depth=items[sel].depth;
    size_t end=sel+1;while(end<items.size()&&items[end].depth>depth)++end;
    if(delta>0&&(sel==0||items[sel-1].depth<depth))return;          // 需要有上一个同级（或更深）书签作为父级
    if(delta<0&&depth==0)return;
    for(size_t i=sel;i<end;++i)items[i].depth+=delta>0?1:-1;
    CommitOutline(std::move(items),delta>0?L"降低书签层级":L"提升书签层级",sel);
}
void Application::ShowCompressDialog(){
    if(textEditor_.Active()){FinishText(true,[this]{ShowCompressDialog();});return;}
    if(!loaded_||busy_||window_.DialogActive())return;
    auto level=std::make_shared<ComboBox*>(nullptr);
    DialogSpec dialog;dialog.title=L"压缩 PDF";dialog.size=DialogSize::Standard;
    dialog.message=L"另存一份更小的副本（包含当前未保存的修改）。原文件与当前编辑不受影响。";
    dialog.content=[level](Panel& panel){
        *level=&panel.Add<ComboBox>().AddItems({L"无损：去除冗余、压缩对象流与字体",L"均衡：图片降到 150 DPI（推荐）",L"最小：图片降到 100 DPI，画质较低"}).SelectedIndex(1).AccessibleName(L"压缩级别");
        panel.Add<Label>(L"只有结果更小的图片才会被替换；矢量文字、批注与书签保持不变。扫描件压缩效果最明显。",TextRole::Caption).Secondary(true).Wrap(true);
    };
    dialog.primary={L"另存副本…",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,level](DialogResult result){
        if(result!=DialogResult::Primary)return;
        const auto mode=static_cast<CompressLevel>(std::clamp<ptrdiff_t>((*level)->SelectedIndex(),0,2));
        const auto stem=source_.empty()?std::wstring(L"文档"):source_.stem().wstring();
        auto path=Destination(stem+L"_压缩.pdf");if(path.empty())return;
        auto sizes=std::make_shared<CompressResult>();
        log::Info(L"压缩 PDF");
        Task(L"压缩 PDF",[path,mode,sizes](Engine& e,const Cancel& cancel){*sizes=e.document.Compress(path,mode,cancel);},[this,path,sizes]{
            auto mb=[](uint64_t b){wchar_t t[32];if(b<1048576)swprintf_s(t,L"%.0f KB",std::max(1.0,b/1024.0));else swprintf_s(t,L"%.2f MB",b/1048576.0);return std::wstring(t);};
            const double ratio=sizes->before?100.0*(1.0-static_cast<double>(sizes->after)/static_cast<double>(sizes->before)):0;
            wchar_t pct[32];swprintf_s(pct,L"%.1f%%",std::abs(ratio));
            const std::wstring text=L"原文件 "+mb(sizes->before)+L" → "+mb(sizes->after)+(ratio>=1?L"（减小 "+std::wstring(pct)+L"）":ratio<=-1?L"（含新增批注/字体，比原文件大）":L"（已接近最小）");
            status_->Text(L"已另存压缩副本："+path.filename().wstring()+L"  ·  "+text);
            window_.ShowToast(L"压缩完成："+mb(sizes->after)+(ratio>=1?L"，减小 "+std::wstring(pct):L""),ToastKind::Success);
        },{},[this](std::wstring error){log::Error(L"压缩失败："+error);Fail(L"未能压缩，目标文件保持不变。\n"+error);});
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::ShowSplitDialog(){
    if(textEditor_.Active()){FinishText(true,[this]{ShowSplitDialog();});return;}
    if(!loaded_||busy_||window_.DialogActive())return;
    struct Controls{ComboBox* mode{};TextBox* amount{};Label* hint{};};
    auto c=std::make_shared<Controls>();
    const int pages=static_cast<int>(info_.pages.size());
    if(pages<2){window_.Alert(L"无法拆分",L"此文档只有 1 页。");return;}
    const bool bookmarks=std::any_of(info_.outline.begin(),info_.outline.end(),[](const auto& o){return o.depth==0;});
    DialogSpec dialog;dialog.title=L"拆分 PDF";dialog.size=DialogSize::Standard;
    dialog.message=L"把当前文档（共 "+std::to_wstring(pages)+L" 页，含未保存的修改）拆成多个 PDF，保存到同一文件夹。";
    dialog.content=[c,pages,bookmarks](Panel& panel){
        c->mode=&panel.Add<ComboBox>().AddItems({L"每 N 页一份",L"按顶层书签（每章一份）",L"按文件大小（每份不超过 N MB）"}).SelectedIndex(0).AccessibleName(L"拆分方式");
        auto& row=panel.Add<Row>();row.Spacing(12);row.AlignCross(CrossAlign::Center);
        c->amount=&row.Add<TextBox>().Text(std::to_wstring(std::max(1,std::min(10,pages/2)))).AccessibleName(L"拆分数量");c->amount->Grow();
        c->hint=&row.Add<Label>(L"页",TextRole::Caption);
        auto& note=panel.Add<Label>(bookmarks?L"按书签拆分时，文件名使用章节标题。":L"此文档没有顶层书签，不能按书签拆分。",TextRole::Caption);note.Secondary(true).Wrap(true);
        c->mode->OnSelectionChanged([c](ptrdiff_t,ptrdiff_t now){c->amount->Visible(now!=1);c->hint->Text(now==2?L"MB":now==1?L"":L"页");if(now==2&&c->amount->Text().size()>3)c->amount->Text(L"10");});
    };
    dialog.primary={L"选择位置并拆分…",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,c,pages](DialogResult result){
        if(result!=DialogResult::Primary)return;
        const auto mode=static_cast<SplitMode>(std::clamp<ptrdiff_t>(c->mode->SelectedIndex(),0,2));
        int amount=0;
        if(mode!=SplitMode::TopBookmarks){
            try{size_t end=0;amount=std::stoi(c->amount->Text(),&end);if(end!=c->amount->Text().size())throw 0;}catch(...){amount=0;}
            if(mode==SplitMode::EveryPages&&(amount<1||amount>=pages)){window_.Alert(L"页数无效",L"请输入 1 到 "+std::to_wstring(std::max(1,pages-1))+L" 之间的整数。");return;}
            if(mode==SplitMode::MaxSize&&(amount<1||amount>4096)){window_.Alert(L"大小无效",L"请输入 1 到 4096 之间的整数（MB）。");return;}
        }
        const auto stem=source_.empty()?std::wstring(L"文档"):source_.stem().wstring();
        wchar_t buffer[32768]{};wcsncpy_s(buffer,(stem+L"-01.pdf").c_str(),_TRUNCATE);
        OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=static_cast<HWND>(window_.NativeHandle());
        ofn.lpstrFilter=PdfFilter;ofn.lpstrFile=buffer;ofn.nMaxFile=32768;ofn.lpstrDefExt=L"pdf";ofn.lpstrTitle=L"选择拆分文件的保存位置与名称前缀";
        ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST;
        if(!GetSaveFileNameW(&ofn))return;
        const fs::path chosen=buffer;const auto folder=chosen.parent_path();
        std::wstring prefix=chosen.stem().wstring();
        if(const auto dash=prefix.find_last_of(L'-');dash!=std::wstring::npos&&dash+1<prefix.size()&&std::all_of(prefix.begin()+static_cast<ptrdiff_t>(dash)+1,prefix.end(),iswdigit))prefix.resize(dash);
        if(prefix.empty())prefix=L"拆分";
        auto files=std::make_shared<std::vector<fs::path>>();
        log::Info(L"拆分 PDF");
        Task(L"拆分 PDF",[mode,amount,folder,prefix,files](Engine& e,const Cancel& cancel){*files=e.document.Split(mode,amount,folder,prefix,cancel);},[this,folder,files]{
            status_->Text(L"已拆分为 "+std::to_wstring(files->size())+L" 个文件，保存在 "+folder.wstring());
            window_.ShowToast(L"拆分完成："+std::to_wstring(files->size())+L" 个 PDF",ToastKind::Success);
            if(!files->empty()){const std::wstring args=L"/select,\""+files->front().wstring()+L"\"";ShellExecuteW(nullptr,L"open",L"explorer.exe",args.c_str(),nullptr,SW_SHOWNORMAL);}
        },{},[this](std::wstring error){log::Error(L"拆分失败："+error);Fail(L"未能拆分：\n"+error);});
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::ShowDecorateDialog(){
    if(textEditor_.Active()){FinishText(true,[this]{ShowDecorateDialog();});return;}
    if(!loaded_||busy_||window_.DialogActive())return;
    struct Controls{TextBox *header{},*footer{},*size{},*start{},*watermark{},*opacity{},*angle{},*range{};ComboBox *headerAlign{},*footerAlign{},*preset{};CheckBox* behind{};};
    auto c=std::make_shared<Controls>();
    DialogSpec dialog;dialog.title=L"页眉页脚、页码与水印";dialog.size=DialogSize::Wide;dialog.default_button=DialogCommand::None;
    dialog.message=L"文字写入页面内容（一次可撤销的修改，保存后生效）。{page} 为页码，{total} 为总页数。";
    dialog.content=[c](Panel& panel){
        auto row=[&](const wchar_t* label)->Row&{auto& r=panel.Add<Row>();r.Spacing(10);r.AlignCross(CrossAlign::Center);auto& l=r.Add<Label>(label,TextRole::Caption);Width(l,72);return r;};
        auto& pr=row(L"页码样式");
        c->preset=&pr.Add<ComboBox>().AddItems({L"不添加页码",L"{page}",L"{page} / {total}",L"第 {page} 页",L"第 {page} 页，共 {total} 页",L"- {page} -"}).SelectedIndex(2).AccessibleName(L"页码样式");c->preset->Grow();
        auto& h=row(L"页眉");
        c->header=&h.Add<TextBox>().Placeholder(L"留空则不添加").AccessibleName(L"页眉文字");c->header->Grow();
        c->headerAlign=&h.Add<ComboBox>().AddItems({L"左",L"中",L"右"}).SelectedIndex(1).AccessibleName(L"页眉对齐");Width(*c->headerAlign,72);
        auto& f=row(L"页脚");
        c->footer=&f.Add<TextBox>().Text(L"{page} / {total}").Placeholder(L"留空则不添加").AccessibleName(L"页脚文字");c->footer->Grow();
        c->footerAlign=&f.Add<ComboBox>().AddItems({L"左",L"中",L"右"}).SelectedIndex(1).AccessibleName(L"页脚对齐");Width(*c->footerAlign,72);
        auto& n=row(L"字号 / 起始");
        c->size=&n.Add<TextBox>().Text(L"10").AccessibleName(L"页眉页脚字号");c->size->Grow();
        c->start=&n.Add<TextBox>().Text(L"1").AccessibleName(L"起始页码");c->start->Grow();
        auto& w=row(L"水印");
        c->watermark=&w.Add<TextBox>().Placeholder(L"例如：机密 · 内部资料（留空不加）").AccessibleName(L"水印文字");c->watermark->Grow();
        auto& wo=row(L"不透明度 / 角度");
        c->opacity=&wo.Add<TextBox>().Text(L"15").AccessibleName(L"水印不透明度");c->opacity->Grow();
        c->angle=&wo.Add<TextBox>().Text(L"45").AccessibleName(L"水印角度");c->angle->Grow();
        c->behind=&panel.Add<CheckBox>(L"水印置于内容下方（不遮挡文字，但可能被整页图片挡住）");
        auto& r=row(L"页面范围");
        c->range=&r.Add<TextBox>().Placeholder(L"全部，或如 1-3,5").AccessibleName(L"应用页面范围");c->range->Grow();
        c->preset->OnSelectionChanged([c](ptrdiff_t,ptrdiff_t now){
            static const wchar_t* formats[]={L"",L"{page}",L"{page} / {total}",L"第 {page} 页",L"第 {page} 页，共 {total} 页",L"- {page} -"};
            if(now>=0&&now<6)c->footer->Text(formats[now]);
        });
    };
    dialog.primary={L"应用",{}};dialog.close={L"取消",{}};
    dialog.on_result=[this,c](DialogResult result){
        if(result!=DialogResult::Primary)return;
        PageDecoration d;
        auto number=[](TextBox* box,float lo,float hi,float& out){
            try{size_t end=0;const float v=std::stof(box->Text(),&end);if(end!=box->Text().size()||!std::isfinite(v)||v<lo||v>hi)return false;out=v;return true;}catch(...){return false;}
        };
        d.header=c->header->Text();d.footer=c->footer->Text();d.watermark=c->watermark->Text();
        d.headerAlign=static_cast<int>(std::clamp<ptrdiff_t>(c->headerAlign->SelectedIndex(),0,2));
        d.footerAlign=static_cast<int>(std::clamp<ptrdiff_t>(c->footerAlign->SelectedIndex(),0,2));
        float start=1,opacity=15;
        if(!number(c->size,4,72,d.fontSize)||!number(c->start,-100000,100000,start)||!number(c->opacity,2,100,opacity)||!number(c->angle,-360,360,d.watermarkAngle)){
            window_.Alert(L"设置格式不正确",L"字号 4–72，起始页码为整数，不透明度 2–100%，角度 −360–360。");return;
        }
        d.startNumber=static_cast<int>(std::lround(start));d.watermarkOpacity=opacity/100;d.watermarkBehind=c->behind->Checked();
        if(d.header.empty()&&d.footer.empty()&&d.watermark.empty()){window_.Alert(L"没有要添加的内容",L"请填写页眉、页脚（页码）或水印。");return;}
        try{if(!c->range->Text().empty())d.pages=ParsePageRange(c->range->Text(),static_cast<int>(info_.pages.size()));}
        catch(...){window_.Alert(L"页面范围无效",L"请输入如 1-3,5 的页码，或留空表示全部页面。");return;}
        Task(L"添加页眉页脚与水印",[d](Engine& e,const Cancel& cancel){e.document.Decorate(d,cancel);},[this]{
            status_->Text(L"已添加页眉页脚 / 水印  ·  Ctrl+Z 撤销，保存后写入文件");
        });
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::MakeDefault(){
    if(smoke_){status_->Text(L"测试模式不修改系统设置");return;}
    const auto exe=ExecutablePath();
    const auto error=RegisterPdfHandler(exe,false);
    if(!error.empty()){log::Error(error);Fail(error);return;}
    if(IsDefaultPdfHandler()){status_->Text(L"LumenPDF 已是默认 PDF 阅读器");RefreshDefaultApp();return;}
    // Windows 10/11 不允许程序自行改写默认应用。按微软推荐做法：登记能力后打开系统“默认应用”
    // 中本程序的页面，由用户确认。系统设置在独立进程中运行，本窗口保持可操作，不会卡住。
    // （旧做法 SHOpenWithDialog 会在界面线程上跑模态循环，刚关闭的对话框重入绘制导致崩溃。）
    log::Info(L"请求设为默认 PDF 阅读器：打开系统默认应用设置");
    OpenDefaultAppsSettings();
    status_->Text(L"已打开系统设置：在 “.pdf” 一项中选择 LumenPDF，完成后回到这里即可");
    window_.ShowToast(L"在系统设置的 “.pdf” 一项中选择 LumenPDF",ToastKind::Info);
    WatchDefault();
}
void Application::WatchDefault(){
    // 轮询 2 分钟：用户在设置里选好后，这里自动更新按钮与提示；不需要重启程序。
    if(defaultWatch_)window_.ClearTimer(defaultWatch_);
    defaultWatchTicks_=0;
    defaultWatch_=window_.SetInterval(1.5f,[this]{
        if(IsDefaultPdfHandler()){
            window_.ClearTimer(defaultWatch_);defaultWatch_=0;RefreshDefaultApp();
            status_->Text(L"已设为默认 PDF 阅读器");window_.ShowToast(L"LumenPDF 已是默认 PDF 阅读器",ToastKind::Success);log::Info(L"已成为默认 PDF 阅读器");
            return;
        }
        if(++defaultWatchTicks_>=80){window_.ClearTimer(defaultWatch_);defaultWatch_=0;RefreshDefaultApp();}
    });
}
void Application::ToggleMergeMenu(){
    if(smoke_){status_->Text(L"测试模式不修改系统设置");return;}
    if(MergeMenuRegistered()){UnregisterMergeMenu();status_->Text(L"已移除资源管理器右键“使用 LumenPDF 合并”");log::Info(L"移除合并右键菜单");return;}
    const auto error=RegisterMergeMenu(ExecutablePath());
    if(!error.empty()){Fail(error);return;}
    log::Info(L"添加合并右键菜单");
    window_.Alert(L"已添加右键菜单",L"在资源管理器中选中若干 PDF、Word、Excel、PPT 或图片，右键选择“使用 LumenPDF 合并”，文件会按顺序加入合并列表。\n\nWindows 11 中该项位于“显示更多选项”（Shift+F10）里。");
}
void Application::RepairShell(){
    // 程序被移动（例如便携版换了目录）后，已登记的命令会指向不存在的路径：静默改为当前位置。
    const auto exe=ExecutablePath();
    if(PdfHandlerRegistered()&&PdfHandlerStale()){RegisterPdfHandler(exe,false);log::Info(L"已修复 PDF 打开程序路径");}
    else if(!smoke_&&RepairPdfDocumentIcon(exe))log::Info(L"已更新 PDF 文件图标");
    if(MergeMenuRegistered()&&MergeMenuStale()){RegisterMergeMenu(exe);log::Info(L"已修复合并右键菜单路径");}
}
void Application::AcceptMerge(const fs::path& path){
    BringToFront(window_.NativeHandle());
    if(path.empty())return;
    std::error_code error;auto absolute=fs::absolute(path,error);const auto file=error?path:absolute;
    if(std::none_of(pendingMerge_.begin(),pendingMerge_.end(),[&](const fs::path& p){return RecentFiles::SamePath(p,file);}))pendingMerge_.push_back(file);
    // 资源管理器为每个选中文件各启动一次，几乎同时到达：稍等片刻再一次性加入，保持选中顺序。
    if(!mergeFlushScheduled_){mergeFlushScheduled_=true;window_.SetTimeout(.6f,[this]{FlushMerge();});}
}
void Application::FlushMerge(){
    mergeFlushScheduled_=false;
    if(pendingMerge_.empty())return;
    if(busy_||textEditor_.Active()||window_.DialogActive()){mergeFlushScheduled_=true;window_.SetTimeout(1,[this]{FlushMerge();});return;}
    auto files=std::move(pendingMerge_);pendingMerge_.clear();
    // Explorer 启动顺序不保证与选中顺序一致：按文件名自然排序，用户可在列表中再调整。
    std::sort(files.begin(),files.end(),[](const fs::path& a,const fs::path& b){return StrCmpLogicalW(a.filename().c_str(),b.filename().c_str())<0;});
    const auto count=files.size();home_=false;Queue(std::move(files));
    status_->Text(L"已从右键菜单加入 "+std::to_wstring(count)+L" 个文件，可拖动调整顺序后合并");
    log::Info(L"右键合并加入 "+std::to_wstring(count)+L" 个文件");
}
void Application::RefreshDefaultApp(){
    if(defaultApp_)defaultApp_->Visible(!smoke_&&!IsDefaultPdfHandler());
}
void Application::Search(){
    if(!loaded_||busy_)return;const auto query=search_->Text();
    if(query.empty()){search_->Focus();return;}
    if(query==lastQuery_&&!hits_.empty()){Hit((GetKeyState(VK_SHIFT)&0x8000)?-1:1);return;}
    if(searchCancel_)searchCancel_->store(true);
    searchCancel_=std::make_shared<std::atomic_bool>(false);lastQuery_=query;hits_.clear();hit_=-1;
    canvas_->SearchResults({},-1);searchResults_->ItemCount(0,false);SyncSearchButtons();
    sidebar_->Visible(true);sideTabs_->SelectedIndex(3);SidePanel(3);
    SearchNextPage(0,static_cast<int>(info_.pages.size()),generation_->load(),searchCancel_);
}
void Application::SearchNextPage(int page,int total,uint64_t gen,Cancel token){
    if(token->load()||generation_->load()!=gen)return;
    if(page>=total){hitLabel_->Text(L"共 "+std::to_wstring(hits_.size())+L" 处");if(hits_.empty())Hit(1);return;}
    hitLabel_->Text(L"搜索 "+std::to_wstring(page+1)+L" / "+std::to_wstring(total));
    const auto query=lastQuery_;const auto options=searchOptions_;auto alive=alive_;auto post=window_.Dispatcher();auto generation=generation_;
    worker_->Submit([this,alive,post,generation,gen,token,query,options,page,total](Engine& e){
        if(!alive->load()||token->load()||generation->load()!=gen||!e.open)return;
        try{
            auto found=e.document.SearchPage(page,query,options,token);
            post.Post([this,alive,gen,token,page,total,found=std::move(found)]() mutable {
                if(!alive->load()||token->load()||generation_->load()!=gen)return;
                const bool first=hits_.empty();const auto available=20000-hits_.size();const bool limited=found.size()>available;
                if(limited)found.resize(available);
                hits_.insert(hits_.end(),found.begin(),found.end());searchResults_->ItemCount(hits_.size(),false);SyncSearchButtons();
                if(first&&!hits_.empty())Hit(1);
                if(limited||hits_.size()>=20000){token->store(true);hitLabel_->Text(L"已达 20,000 处，请缩小关键词范围");return;}
                SearchNextPage(page+1,total,gen,token);
            });
        }catch(const Cancelled&){}catch(const std::exception& ex){const auto error=ErrorText(ex);post.Post([this,alive,gen,token,error]{if(alive->load()&&!token->load()&&generation_->load()==gen){token->store(true);hitLabel_->Text(L"搜索未完成");window_.Alert(L"搜索失败",error);}});}
    },false);
}
void Application::SearchSettings(){
    if(window_.DialogActive())return;
    struct Controls{CheckBox *match{},*word{},*annotations{},*bookmarks{};};auto c=std::make_shared<Controls>();
    DialogSpec d;d.title=L"搜索选项";d.message=L"正文始终搜索；结果逐页出现。整词匹配按字母/数字边界判断，中文短语通常不需要启用。";
    d.content=[this,c](Panel& p){c->match=&p.Add<CheckBox>(L"区分大小写");c->match->Checked(searchOptions_.matchCase);c->word=&p.Add<CheckBox>(L"整词匹配");c->word->Checked(searchOptions_.wholeWord);c->annotations=&p.Add<CheckBox>(L"同时搜索批注文字");c->annotations->Checked(searchOptions_.annotations);c->bookmarks=&p.Add<CheckBox>(L"同时搜索书签标题");c->bookmarks->Checked(searchOptions_.bookmarks);};
    d.primary={L"应用并搜索",{}};d.close={L"取消",{}};
    d.on_result=[this,c](DialogResult result){if(result!=DialogResult::Primary)return;searchOptions_={c->match->Checked(),c->word->Checked(),c->annotations->Checked(),c->bookmarks->Checked()};lastQuery_.clear();window_.Dispatcher().Post([this]{Search();});};window_.ShowDialog(std::move(d));
}
void Application::Hit(int direction){
    SyncSearchButtons();
    if(hits_.empty()){
        if(lastQuery_.empty()){hitLabel_->Text(L"");status_->Text(L"请先在搜索框输入文字  ·  Ctrl+F");search_->Focus();return;}
        hitLabel_->Text(L"无结果");canvas_->SearchResults({},-1);status_->Text(L"没有找到“"+lastQuery_+L"”，可换个关键字或检查是否为扫描件");return;
    }
    hit_=(hit_+direction+static_cast<int>(hits_.size()))%static_cast<int>(hits_.size());
    Page(hits_[hit_].page,true);canvas_->SearchResults(hits_,hit_);
    if(hits_[hit_].annotation>=0){pendingSelect_=std::make_pair(hits_[hit_].page,hits_[hit_].annotation);Select(hits_[hit_].page,hits_[hit_].annotation);}
    const auto counter=L"第 "+std::to_wstring(hit_+1)+L" / "+std::to_wstring(hits_.size())+L" 处";
    hitLabel_->Text(counter);status_->Text(L"“"+lastQuery_+L"” "+counter+L"  ·  回车 / F3 下一处，Shift+F3 上一处");
}
void Application::ShowPageNumber(int page){
    pageBox_->Visible(loaded_);
    if(!loaded_){pageLabel_->Text(L"");return;}
    if(window_.Focused()!=pageBox_)pageBox_->Text(std::to_wstring(page+1));
    const auto label=static_cast<size_t>(page)<info_.labels.size()?info_.labels[page]:L"";
    pageLabel_->Text(L"/ "+std::to_wstring(info_.pages.size())+(label.empty()||label==std::to_wstring(page+1)?L"":L"  ["+label+L"]"));
}
void Application::GoToPageText(){
    if(!loaded_)return;
    const auto& text=pageBox_->Text();int value=0;bool digits=!text.empty();
    for(wchar_t c:text){if(c<L'0'||c>L'9'){digits=false;break;}value=std::min(value*10+(c-L'0'),1000000);}
    const int count=static_cast<int>(info_.pages.size());
    if(!digits||value<1||value>count){status_->Text(L"请输入 1 到 "+std::to_wstring(count)+L" 之间的页码");pageBox_->Text(std::to_wstring(page_+1));return;}
    canvas_->Focus();Page(value-1,true);ShowPageNumber(value-1);
}
void Application::LoadRecent(){
    // 测试可通过 LPDF_RECENT_STORE 指定只读的历史文件，截图时不会改写用户数据。
    wchar_t custom[MAX_PATH]{};
    RefreshDefaultApp();LoadSettings();
    positions_.ReadOnly(true);
    if(GetEnvironmentVariableW(L"LPDF_RECENT_STORE",custom,MAX_PATH)){recent_.Load(custom);recent_.ReadOnly(true);RefreshRecent();return;}
    if(smoke_){RefreshRecent();return;}
    PWSTR local=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))){RefreshRecent();return;}
    const auto root=fs::path(local)/L"LumenPDF";CoTaskMemFree(local);
    std::error_code error;
    // 旧版本 recent.txt（每行一个路径）自动迁移为 v2（时间 + 固定）。
    if(!fs::exists(root/L"recent-v2.txt",error)&&fs::exists(root/L"recent.txt",error)){
        RecentFiles legacy;legacy.Load(root/L"recent.txt");const auto now=UnixNow();int64_t offset=0;
        recent_.Load(root/L"recent-v2.txt");
        auto entries=legacy.Entries();
        for(auto it=entries.rbegin();it!=entries.rend();++it)recent_.Touch(it->path,now-static_cast<int64_t>(entries.size())*60+(offset+=60));
        recent_.Save();
    }else recent_.Load(root/L"recent-v2.txt");
    positions_.Load(root/L"positions-v1.txt");positions_.ReadOnly(false);
    RefreshRecent();
}
void Application::RememberRecent(const fs::path& path){
    if(path.empty())return;
    recent_.Reload();recent_.Touch(path,UnixNow());recent_.Save();RefreshRecent();
}
void Application::ForgetRecent(const fs::path& path){
    window_.Confirm(L"找不到此文件",L"“"+path.filename().wstring()+L"”可能已被移动或删除。要从最近打开中移除吗？",
        [this,path](bool yes){if(yes){recent_.Remove(path);recent_.Save();RefreshRecent();}},L"移除",L"保留");
}
void Application::RefreshRecent(){
    if(!recentView_)return;
    recentView_->Entries(recent_.Entries());
    const auto total=recent_.Entries().size();
    size_t pinned=0;for(const auto& e:recent_.Entries())pinned+=e.pinned?1:0;
    recentCount_->Text(total==0?L"打开过的文件会保存在这里":std::to_wstring(total)+L" 个文件"+(pinned?L"  ·  "+std::to_wstring(pinned)+L" 个已固定":L"")+L"  ·  右键更多操作");
    recentFilter_->Visible(total>0);clearRecent_->Visible(total>pinned);
}
void Application::Home(bool show){
    if(textEditor_.Active()){FinishText(true,[this,show]{Home(show);});return;}
    if(!loaded_)show=true;
    if(mode_==3)Mode(0);home_=show;
    if(show){SavePosition();positions_.Save();recent_.Reload();RefreshRecent();recentView_->Focus();}
    SetBusy(busy_);
    status_->Text(ContextHint());
    UpdateTitle();
}
std::wstring Application::ContextHint()const{
    if(mode_==3)return L"拖入文件或点“添加文件” · 拖动调整顺序 · 选中文件可设页码范围与密码";
    if(!loaded_)return L"就绪  ·  Ctrl+O 打开文件  ·  Ctrl+, 设置";
    if(home_)return L"主页 · 单击继续阅读，右键固定或移除  ·  Esc 返回文档";
    if(mode_==2)return L"单击选择页面 · Ctrl / Shift 多选 · Ctrl+A 全选 · 拖动重排 · Delete 删除 · 右键更多";
    if(mode_==1)return L"选择工具栏中的批注工具后在页面上操作  ·  Esc 回到选择";
    return L"拖动选择文字 · 按住空格拖动平移 · Ctrl+滚轮缩放 · Ctrl+F 搜索 · Ctrl+Shift+N 跳页 · 右键更多";
}
void Application::PageSelectionHint(size_t count){
    if(!pageHint_)return;
    pageHint_->Text(count>1?L"已选 "+std::to_wstring(count)+L" 页 · 操作将作用于全部选中页 · Esc 取消多选":std::wstring(L"Ctrl / Shift 多选 · 拖动重排 · 双击打开 · Ctrl+滚轮调整大小"));
}
void Application::DeleteSelectedPages(){
    if(!loaded_||busy_)return;auto pages=SelectedPages();
    if(pages.size()>=info_.pages.size()){window_.Alert(L"无法删除全部页面",L"PDF 至少需要保留一页。若要清空内容，可先插入空白页再删除其余页面。");return;}
    const std::wstring what=pages.size()>1?std::to_wstring(pages.size())+L" 个页面":L"第 "+std::to_wstring(pages[0]+1)+L" 页";
    window_.Confirm(L"删除"+what+L"？",L"保存前可用 Ctrl+Z 撤销，原文件不会被改动。",[this,pages,what](bool yes){
        if(yes)Task(L"删除页面",[pages](Engine&e,const Cancel&){e.document.DeletePages(pages);},[this,what]{canvas_->SelectedPages({page_});PageSelectionHint(1);status_->Text(L"已删除"+what+L"  ·  Ctrl+Z 撤销");});
    },L"删除",L"取消");
}
void Application::ShowZoomMenu(){
    if(!loaded_)return;
    Menu menu;
    menu.AddItem(L"适合页面",[this]{canvas_->FitPage();}).Shortcut(L"Ctrl+0");
    menu.AddItem(L"适合宽度",[this]{canvas_->FitWidth();}).Shortcut(L"Ctrl+2");
    menu.AddItem(L"翻页模式（整页翻动）",[this]{SetPaged(!settings_.paged);}).Checked(settings_.paged);
    menu.AddItem(L"框选放大",[this]{ToggleZoomBox();}).Shortcut(L"Z");
    if(canvas_->CanZoomBack())menu.AddItem(L"返回框选前的缩放",[this]{ZoomBackKey();}).Shortcut(L"Shift+Z");
    menu.AddItem(L"放大镜",[this]{ToggleMagnifier();}).Shortcut(L"L").Checked(canvas_->MagnifierActive());
    menu.AddSeparator();
    const int current=static_cast<int>(std::lround(canvas_->ZoomValue()*100));
    for(int value:{50,75,100,125,150,200,300}){
        auto& item=menu.AddItem(std::to_wstring(value)+L"%",[this,value]{canvas_->Zoom(value/100.0f/std::max(.01f,canvas_->ZoomValue()));}).RadioGroup(L"zoom").Checked(value==current);
        if(value==100)item.Shortcut(L"Ctrl+1");
    }
    menu.PopupTo(*zoomButton_);
}
void Application::SyncSearchButtons(){
    if(prevHit_)prevHit_->Enabled(hits_.size()>0);if(nextHit_)nextHit_->Enabled(hits_.size()>0);
}
void Application::RemoveQueueItem(){
    if(busy_)return;SyncQueueFields();const auto n=queueList_->SelectedIndex();
    if(n<0||n>=static_cast<ptrdiff_t>(queue_.size()))return;
    const auto name=queue_[static_cast<size_t>(n)].path.filename().wstring();
    queue_.erase(queue_.begin()+n);QueueChanged();
    // 移除后选中相邻一项，连续按 Delete / 点“移除”即可逐个清理。
    if(!queue_.empty())queueList_->SelectedIndex(std::min<ptrdiff_t>(n,static_cast<ptrdiff_t>(queue_.size())-1));
    status_->Text(L"已从列表移除 "+name+L"（文件本身未删除）");
}
std::vector<int> Application::SelectedPages(){auto result=canvas_->SelectedPages();if(result.empty())result.push_back(page_);return result;}
std::vector<fs::path> Application::Pick(bool multiple,const wchar_t* filter){
    std::vector<wchar_t> buffer(65536);OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=static_cast<HWND>(window_.NativeHandle());ofn.lpstrFilter=filter?filter:AllFilter;
    ofn.lpstrFile=buffer.data();ofn.nMaxFile=static_cast<DWORD>(buffer.size());ofn.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR|(multiple?OFN_ALLOWMULTISELECT:0);
    if(!GetOpenFileNameW(&ofn))return {};std::vector<fs::path> result;fs::path first=buffer.data();const wchar_t* p=buffer.data()+wcslen(buffer.data())+1;
    if(!*p)result.push_back(first);else for(;*p;p+=wcslen(p)+1)result.push_back(first/p);return result;
}
fs::path Application::Destination(std::wstring name){
    wchar_t buffer[32768]{};wcsncpy_s(buffer,name.c_str(),_TRUNCATE);OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=static_cast<HWND>(window_.NativeHandle());
    ofn.lpstrFilter=PdfFilter;ofn.lpstrFile=buffer;ofn.nMaxFile=32768;ofn.lpstrDefExt=L"pdf";ofn.Flags=OFN_EXPLORER|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn)?fs::path(buffer):fs::path{};
}
void Application::PageToolsMenu(){
    if(!loaded_||busy_)return;const auto pages=SelectedPages();Menu menu;
    menu.AddItem(L"向左旋转 90°",[this,pages]{Task(L"旋转页面",[pages](Engine& e,const Cancel&){e.document.RotatePages(pages,-90);});});
    menu.AddItem(L"旋转 180°",[this,pages]{Task(L"旋转页面",[pages](Engine& e,const Cancel&){e.document.RotatePages(pages,180);});});
    menu.AddItem(L"复制选中页…",[this,pages]{window_.Confirm(L"复制选中页？",L"副本插入各原页之后。副本中的批注和表单将固化为可见外观；原页保持不变。",[this,pages](bool yes){if(yes)Task(L"复制页面",[pages](Engine& e,const Cancel&){e.document.DuplicatePages(pages);});},L"复制",L"取消");});
    menu.AddItem(L"从剪贴板插入页面",[this]{PasteFromClipboard(PasteIntent::NewPage);}).Glyph(icon::kImage).Disabled(!ClipboardHasContent());
    menu.AddItem(L"页码标签…",[this]{EditPageLabels();});
    menu.AddItem(L"裁剪选中页…",[this]{CropSelectedPages();});
    menu.AddItem(L"用文件替换选中页…",[this,pages]{auto files=Pick(false,AllFilter);if(files.empty())return;const auto file=files[0];window_.Confirm(L"替换选中页面？",L"来源转换后页数须与选中页数相同。目标页原内容/批注会被替换，来源批注/表单会固化。可以撤销。",[this,pages,file](bool yes){if(yes)Task(L"替换页面",[pages,file](Engine& e,const Cancel& c){const auto converted=e.converter.Convert(file,{},c);e.document.ReplacePages(pages,converted.pdf);});},L"替换",L"取消");});
    menu.AddItem(L"插入文件（PDF / Office / 图片 / 文本）…",[this]{auto files=Pick(false,AllFilter);if(files.empty())return;const auto file=files[0];const int p=page_;window_.Confirm(L"插入文件？",L"在当前页之后插入；来源批注和表单固化为可见外观。",[this,p,file](bool yes){if(yes)Task(L"插入文件",[p,file](Engine& e,const Cancel& c){const auto converted=e.converter.Convert(file,{},c);e.document.InsertPdf(p,converted.pdf);});},L"插入",L"取消");});
    menu.PopupTo(*pageTools_);
}
void Application::CropSelectedPages(){
    const auto pages=SelectedPages();TextDialog(L"裁剪选中页面",L"输入 左 上 右 下 四边裁去的毫米数，例如 10 10 10 10。只改变可见区域，不会删除隐藏内容（不能用于保密涂黑）。",L"0 0 0 0",L"裁剪",[this,pages](std::wstring value){
        std::wistringstream in(value);float l,t,r,b;std::wstring extra;
        if(!(in>>l>>t>>r>>b)||(in>>extra)){Fail(L"请输入四个非负数，以空格分隔。");return;}
        Task(L"裁剪页面",[pages,l,t,r,b](Engine& e,const Cancel&){e.document.CropPages(pages,l*72/25.4f,t*72/25.4f,r*72/25.4f,b*72/25.4f);});
    });
}
void Application::Print(){
    wchar_t target[MAX_PATH]{};if(GetEnvironmentVariableW(L"LPDF_PRINT_TO",target,MAX_PATH)){PrintExecute();return;}
    if(textEditor_.Active()){FinishText(true,[this]{Print();});return;}PrintOptions();
}
void Application::SetPaged(bool on,bool save){
    settings_.paged=on;canvas_->Paged(on);
    if(save){SaveSettings();status_->Text(on?L"翻页模式：一次一页，滚动到页边整页翻过":L"连续滚动");}
}
void Application::SetSidebarWidth(float width,bool save){
    sidebarWidth_=std::clamp(width,180.0f,600.0f);Width(*sideColumn_,std::round(sidebarWidth_));
    if(save){settings_.sidebarWidth=static_cast<int>(std::lround(sidebarWidth_));SaveSettings();}
}
void Application::Present(bool on){
    if(on==presenting_)return;
    auto& root=window_.Root();
    if(on){
        if(!loaded_||busy_||home_||mode_==3||window_.DialogActive())return;
        if(textEditor_.Active()){FinishText(true,[this]{Present(true);});return;}
        presentState_={fullScreen_,splitMode_,referenceId_,sidebar_->Visible(),previewBar_->Visible()};
        if(splitMode_)SetSplit(0);
        if(mode_!=0)Mode(0);
        ChooseTool(Tool::Select);SetHand(false);autoScroll_=false;presenting_=true;
        root.Padding(0,0);root.Spacing(0);root.Background(Color::Hex(0x000000));readPane_->Spacing(0);
        SetBusy(false);
        if(!fullScreen_)FullScreen();
        canvas_->Presenting(true);canvas_->Focus();
        POINT at{};GetCursorPos(&at);cursorX_=at.x;cursorY_=at.y;cursorMoved_=GetTickCount64();
        presentTimer_=window_.SetInterval(.25f,[this]{PresentCursor();});
    }else{
        presenting_=false;
        if(presentTimer_){window_.ClearTimer(presentTimer_);presentTimer_={};}
        if(cursorHidden_){ShowCursor(TRUE);cursorHidden_=false;}
        canvas_->Presenting(false);
        root.Padding(16,12);root.Spacing(12);root.Background(Color::Hex(0x0d0d0d));readPane_->Spacing(8);
        footer_->Visible(true);previewBar_->Visible(presentState_.preview);
        SetBusy(false);sidebar_->Visible(presentState_.sidebar&&!readingMode_);
        if(!presentState_.fullScreen&&fullScreen_)FullScreen();
        if(presentState_.split)SetSplit(presentState_.split,presentState_.reference);
        status_->Text(L"已退出演示");
    }
}
void Application::PresentCursor(){
    if(!presenting_)return;
    const auto hwnd=static_cast<HWND>(window_.NativeHandle());POINT at{};GetCursorPos(&at);const auto now=GetTickCount64();
    const bool front=GetForegroundWindow()==hwnd&&!window_.DialogActive()&&!window_.PopupActive();
    if(at.x!=cursorX_||at.y!=cursorY_||!front){
        cursorX_=at.x;cursorY_=at.y;cursorMoved_=now;
        if(cursorHidden_){ShowCursor(TRUE);cursorHidden_=false;}
    }else if(!cursorHidden_&&now-cursorMoved_>2000){ShowCursor(FALSE);cursorHidden_=true;}
}
namespace {
std::optional<FileStamp> StampOf(const fs::path& path){
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(path.empty()||!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))return std::nullopt;
    return FileStamp{(static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime,(static_cast<uint64_t>(data.nFileSizeHigh)<<32)|data.nFileSizeLow};
}
}
void Application::StampSource(){
    pendingStamp_.reset();if(!activeDocument_)return;
    if(auto stamp=StampOf(source_))stamps_[activeDocument_->id]=*stamp;else stamps_.erase(activeDocument_->id);
}
void Application::CheckExternalChange(){
    if(smoke_||!loaded_||busy_||!activeDocument_||source_.empty()||window_.DialogActive()||window_.PopupActive()||textEditor_.Active())return;
    const auto id=activeDocument_->id;
    const auto now=StampOf(source_);
    if(!now){pendingStamp_.reset();return;}   // 暂时不存在：其它程序可能正在替换写入，下次再看
    auto known=stamps_.find(id);
    if(known==stamps_.end()){stamps_[id]=*now;return;}
    if(*now==known->second){pendingStamp_.reset();return;}
    // 连续两次读到同一个新文件戳才处理，避免在对方写到一半时载入。
    if(!pendingStamp_||pendingStamp_->first!=id||!(pendingStamp_->second==*now)){pendingStamp_={id,*now};return;}
    pendingStamp_.reset();const auto stamp=*now;
    if(settings_.autoReload&&!info_.dirty){ReloadFromDisk(id,stamp);return;}
    // 询问等用户回到窗口再弹出。
    if(GetForegroundWindow()!=static_cast<HWND>(window_.NativeHandle())){pendingStamp_={id,stamp};return;}
    DialogSpec dialog;
    dialog.title=L"“"+name_->Text()+L"”已被其它程序修改";
    dialog.message=info_.dirty?L"磁盘上的文件已更改。重新载入会放弃此标签页中未保存的修改；也可以保留当前内容，稍后另存为。"
        :L"磁盘上的文件已更改。重新载入后停留在当前阅读位置。";
    dialog.primary={info_.dirty?L"放弃修改并重新载入":L"重新载入",{}};
    dialog.close={L"保留当前",{}};
    dialog.default_button=info_.dirty?DialogCommand::Close:DialogCommand::Primary;
    dialog.cancel_button=DialogCommand::Close;
    dialog.on_result=[this,id,stamp](DialogResult result){
        if(result==DialogResult::Primary)window_.Dispatcher().Post([this,id,stamp]{ReloadFromDisk(id,stamp);});
        else stamps_[id]=stamp;   // 保留当前：同一版本不再询问
    };
    window_.ShowDialog(std::move(dialog));
}
void Application::ReloadFromDisk(uint64_t id,FileStamp stamp){
    if(!activeDocument_||activeDocument_->id!=id||source_.empty())return;
    if(busy_||textEditor_.Active()){pendingStamp_.reset();return;}   // 稍后的检查会再次发现
    const auto path=source_;const auto view=canvas_->CurrentView();
    Task(L"重新载入 "+path.filename().wstring(),[path](Engine& e,const Cancel& c){
        const auto converted=e.converter.Convert(path,{},c);CheckCancel(c);e.document.Reload(converted.pdf);e.open=true;
    },[this,view]{
        StampSource();auto v=view;v.page=std::clamp(v.page,0,std::max(0,static_cast<int>(info_.pages.size())-1));
        canvas_->RestoreView(v);Page(v.page,false);thumbs_->GoTo(v.page);
        status_->Text(L"已载入磁盘上的新版本，阅读位置保持不变");
    },{},[this,id,stamp](std::wstring error){stamps_[id]=stamp;Fail(L"重新载入失败，当前内容保持不变。\n"+error);});
}
void Application::FullScreen(){
    if(window_.DialogActive())return;auto hwnd=static_cast<HWND>(window_.NativeHandle());
    if(!fullScreen_){savedWindowStyle_=GetWindowLongPtrW(hwnd,GWL_STYLE);savedWindowPlacement_.length=sizeof(savedWindowPlacement_);if(!GetWindowPlacement(hwnd,&savedWindowPlacement_))return;MONITORINFO monitor{sizeof(monitor)};if(!GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))return;
        fullScreen_=true;window_.CaptionVisible(false);SetWindowLongPtrW(hwnd,GWL_STYLE,savedWindowStyle_&~WS_OVERLAPPEDWINDOW);SetWindowPos(hwnd,nullptr,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_NOZORDER|SWP_FRAMECHANGED);
    }else{fullScreen_=false;window_.CaptionVisible(true);SetWindowLongPtrW(hwnd,GWL_STYLE,savedWindowStyle_);SetWindowPlacement(hwnd,&savedWindowPlacement_);SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);}
}
void Application::ReadingMode(){
    if(!loaded_||busy_||window_.DialogActive())return;if(textEditor_.Active()){FinishText(true,[this]{ReadingMode();});return;}
    readingMode_=!readingMode_;if(readingMode_){home_=false;Mode(0);}else sidebar_->Visible(true);SetBusy(false);status_->Text(readingMode_?L"阅读模式 · Esc 或 Ctrl+H 退出":ContextHint());
}
void Application::FilterAnnotations(){
    if(!annotList_)return;const auto filter=annotationFilter_->Text();annotRows_.clear();
    for(const auto& row:allAnnotRows_){const auto text=row.annotation.author+L" "+row.annotation.text+L" "+ui::ToolName(row.annotation.type);if(filter.empty()||StrStrIW(text.c_str(),filter.c_str()))annotRows_.push_back(row);}
    const auto sort=annotationSort_->SelectedIndex();std::stable_sort(annotRows_.begin(),annotRows_.end(),[sort](const auto& a,const auto& b){if(sort==1)return a.annotation.author<b.annotation.author;if(sort==2)return a.annotation.modified>b.annotation.modified;if(sort==3)return a.annotation.type<b.annotation.type;return a.page<b.page;});annotList_->ItemCount(annotRows_.size(),false);
}
void Application::ExportAnnotationSummary(){
    if(busy_||annotRowsLoading_)return;
    wchar_t file[32768]=L"批注摘要.txt";OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=static_cast<HWND>(window_.NativeHandle());dialog.lpstrFile=file;dialog.nMaxFile=32768;dialog.lpstrFilter=L"文本摘要\0*.txt\0";dialog.lpstrDefExt=L"txt";dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT;
    if(!GetSaveFileNameW(&dialog))return;const fs::path path=file;if(RecentFiles::SamePath(path,source_)){Fail(L"不能覆盖当前 PDF 原件。");return;}
    const auto rows=annotRows_;keepSearch_=true;Task(L"导出批注摘要",[path,rows](Engine&,const Cancel& cancel){std::wstring text=L"LumenPDF 批注摘要（当前筛选结果）\r\n";for(const auto& row:rows){CheckCancel(cancel);text+=L"\r\n第 "+std::to_wstring(row.page+1)+L" 页 · "+ui::ToolName(row.annotation.type)+L" · "+row.annotation.author+L"\r\n"+row.annotation.text+L"\r\n";}
        const auto utf8=std::string("\xef\xbb\xbf")+Utf8(text);const auto temp=UniquePath(path.parent_path(),L".txt");try{WriteBytes(temp,std::vector<unsigned char>(utf8.begin(),utf8.end()));CheckCancel(cancel);AtomicReplace(temp,path);}catch(...){std::error_code error;fs::remove(temp,error);throw;}
    });
}
}