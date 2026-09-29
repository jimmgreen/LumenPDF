#include "shell_integration.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <memory>
#include <vector>
#include <algorithm>
#include <string_view>
#include <thread>
namespace lpdf {
namespace {
constexpr wchar_t kMutex[]=L"Local\\LumenPDF.SingleInstance.v1";
constexpr wchar_t kClass[]=L"LumenPDF.InstanceReceiver";
constexpr ULONG_PTR kOpenMagic=0x4c504446; // 'LPDF'
constexpr ULONG_PTR kMergeMagic=0x4c504d47; // 'LPMG'
constexpr wchar_t kMergeVerb[]=L"LumenPDF.Merge";
constexpr wchar_t kProgId[]=L"LumenPDF.Document";
bool SetValue(HKEY root,const std::wstring& key,const wchar_t* name,const std::wstring& value){
    HKEY handle{};
    if(RegCreateKeyExW(root,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,nullptr)!=ERROR_SUCCESS)return false;
    const auto bytes=static_cast<DWORD>((value.size()+1)*sizeof(wchar_t));
    const bool ok=RegSetValueExW(handle,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),bytes)==ERROR_SUCCESS;
    RegCloseKey(handle);return ok;
}
std::wstring GetValue(HKEY root,const std::wstring& key,const wchar_t* name){
    wchar_t buffer[4096]{};DWORD size=sizeof(buffer);
    if(RegGetValueW(root,key.c_str(),name,RRF_RT_REG_SZ,nullptr,buffer,&size)!=ERROR_SUCCESS)return {};
    return buffer;
}
// 从 "\"C:\x\LumenPDF.exe\" --merge \"%1\"" 取出可执行文件路径。
fs::path CommandExecutable(const std::wstring& command){
    if(command.empty())return {};
    if(command[0]==L'"'){const auto end=command.find(L'"',1);return end==std::wstring::npos?fs::path{}:fs::path(command.substr(1,end-1));}
    return fs::path(command.substr(0,command.find(L' ')));
}
bool MissingExecutable(const std::wstring& command){
    const auto exe=CommandExecutable(command);if(exe.empty())return false;
    std::error_code error;return !fs::exists(exe,error);
}
}
SingleInstance::SingleInstance(){
    mutex_=CreateMutexW(nullptr,TRUE,kMutex);
    secondary_=mutex_&&GetLastError()==ERROR_ALREADY_EXISTS;
}
SingleInstance::~SingleInstance(){
    if(window_)DestroyWindow(static_cast<HWND>(window_));
    if(mutex_){if(!secondary_)ReleaseMutex(mutex_);CloseHandle(mutex_);}
}
bool SingleInstance::Forward(const fs::path& path)const{return Send(kOpenMagic,path,2000);}
bool SingleInstance::ForwardMerge(const fs::path& path,unsigned waitMs)const{return Send(kMergeMagic,path,waitMs);}
bool SingleInstance::Send(unsigned long long magic,const fs::path& path,unsigned waitMs)const{
    std::wstring text;
    if(!path.empty()){std::error_code error;auto absolute=fs::absolute(path,error);text=(error?path:absolute).wstring();}
    // 主实例可能仍在启动，稍等接收窗口出现。
    HWND target=nullptr;
    for(unsigned waited=0;!target;waited+=50){target=FindWindowExW(HWND_MESSAGE,nullptr,kClass,nullptr);if(target||waited>=waitMs)break;Sleep(50);}
    if(!target)return false;
    DWORD pid=0;GetWindowThreadProcessId(target,&pid);AllowSetForegroundWindow(pid);
    COPYDATASTRUCT data{};data.dwData=static_cast<ULONG_PTR>(magic);data.cbData=static_cast<DWORD>((text.size()+1)*sizeof(wchar_t));data.lpData=const_cast<wchar_t*>(text.c_str());
    DWORD_PTR result=0;
    return SendMessageTimeoutW(target,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG,5000,&result)!=0&&result==1;
}
long long __stdcall SingleInstance::Proc(void* hwnd,unsigned message,unsigned long long wparam,long long lparam){
    auto* self=reinterpret_cast<SingleInstance*>(GetWindowLongPtrW(static_cast<HWND>(hwnd),GWLP_USERDATA));
    if(message==WM_COPYDATA&&self){
        const auto* data=reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if(data&&(data->dwData==kOpenMagic||data->dwData==kMergeMagic)&&data->cbData%sizeof(wchar_t)==0&&data->cbData<=65536*sizeof(wchar_t)){
            std::wstring text(static_cast<const wchar_t*>(data->lpData),data->cbData/sizeof(wchar_t));
            while(!text.empty()&&text.back()==L'\0')text.pop_back();
            // 发送方在 SendMessage 中等待：先返回，再在消息循环中处理（可能弹出对话框）。
            auto path=std::make_unique<fs::path>(text);
            PostMessageW(static_cast<HWND>(hwnd),WM_APP+1,data->dwData==kMergeMagic?1:0,reinterpret_cast<LPARAM>(path.release()));
            return 1;
        }
        return 0;
    }
    if(message==WM_APP+1){
        std::unique_ptr<fs::path> path(reinterpret_cast<fs::path*>(lparam));
        if(self&&self->received_)self->received_(*path,wparam==1);
        return 0;
    }
    return DefWindowProcW(static_cast<HWND>(hwnd),message,static_cast<WPARAM>(wparam),static_cast<LPARAM>(lparam));
}
void SingleInstance::Listen(std::function<void(fs::path,bool)> received){
    if(secondary_||window_)return;
    received_=std::move(received);
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=reinterpret_cast<WNDPROC>(&SingleInstance::Proc);wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kClass;
    RegisterClassExW(&wc);
    HWND hwnd=CreateWindowExW(0,kClass,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
    if(!hwnd)return;
    SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
    // 允许低完整性级别之外的普通进程发送 WM_COPYDATA。
    ChangeWindowMessageFilterEx(hwnd,WM_COPYDATA,MSGFLT_ALLOW,nullptr);
    window_=hwnd;
}
void BringToFront(void* handle){
    auto hwnd=static_cast<HWND>(handle);if(!hwnd)return;
    if(IsIconic(hwnd))ShowWindow(hwnd,SW_RESTORE);
    SetForegroundWindow(hwnd);
    FLASHWINFO flash{sizeof(flash),hwnd,FLASHW_TRAY|FLASHW_TIMERNOFG,2,0};
    if(GetForegroundWindow()!=hwnd)FlashWindowEx(&flash);
}
std::wstring RegisterPdfHandler(const fs::path& executable,bool openSettings){
    const std::wstring exe=executable.wstring();
    const std::wstring command=L"\""+exe+L"\" \"%1\"";
    const std::wstring classes=L"Software\\Classes\\";
    bool ok=true;
    ok&=SetValue(HKEY_CURRENT_USER,classes+kProgId,nullptr,L"PDF 文档 (LumenPDF)");
    ok&=SetValue(HKEY_CURRENT_USER,classes+kProgId,L"FriendlyTypeName",L"PDF 文档");
    // 文档图标是 exe 中资源 ID 3 的 ICON（app/pdf_file.ico），与应用图标区分。
    ok&=SetValue(HKEY_CURRENT_USER,classes+kProgId+L"\\DefaultIcon",nullptr,L"\""+exe+L"\",-3");
    ok&=SetValue(HKEY_CURRENT_USER,classes+kProgId+L"\\shell\\open\\command",nullptr,command);
    // “打开方式”列表显示的名称与图标（否则显示 exe 的文件说明）。
    ok&=SetValue(HKEY_CURRENT_USER,classes+kProgId+L"\\Application",L"ApplicationName",L"LumenPDF");
    ok&=SetValue(HKEY_CURRENT_USER,classes+kProgId+L"\\Application",L"ApplicationIcon",L"\""+exe+L"\",0");
    ok&=SetValue(HKEY_CURRENT_USER,classes+L".pdf\\OpenWithProgids",kProgId,L"");
    const std::wstring app=classes+L"Applications\\"+executable.filename().wstring();
    ok&=SetValue(HKEY_CURRENT_USER,app,L"FriendlyAppName",L"LumenPDF");
    ok&=SetValue(HKEY_CURRENT_USER,app+L"\\shell\\open\\command",nullptr,command);
    ok&=SetValue(HKEY_CURRENT_USER,app+L"\\SupportedTypes",L".pdf",L"");
    ok&=SetValue(HKEY_CURRENT_USER,L"Software\\LumenPDF\\Capabilities",L"ApplicationName",L"LumenPDF");
    ok&=SetValue(HKEY_CURRENT_USER,L"Software\\LumenPDF\\Capabilities",L"ApplicationDescription",L"本地 PDF 阅读、批注与页面整理");
    ok&=SetValue(HKEY_CURRENT_USER,L"Software\\LumenPDF\\Capabilities\\FileAssociations",L".pdf",kProgId);
    ok&=SetValue(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",L"LumenPDF",L"Software\\LumenPDF\\Capabilities");
    if(!ok)return L"写入当前用户的文件关联设置失败，请检查注册表权限。";
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
    if(openSettings){
        // Windows 11 可直接定位到本程序的默认应用页；较旧系统退回通用页面。
        OpenDefaultAppsSettings();
    }
    return {};
}
bool IsDefaultPdfHandler(){
    wchar_t progid[256]{};DWORD size=256;
    if(AssocQueryStringW(ASSOCF_NONE,ASSOCSTR_PROGID,L".pdf",nullptr,progid,&size)==S_OK&&_wcsicmp(progid,kProgId)==0)return true;
    // 用户在“打开方式”里选的可能是 Applications\LumenPDF.exe：按实际打开程序判断。
    wchar_t exe[32768]{};size=32768;
    if(AssocQueryStringW(ASSOCF_NONE,ASSOCSTR_EXECUTABLE,L".pdf",L"open",exe,&size)!=S_OK)return false;
    wchar_t self[32768]{};const DWORD n=GetModuleFileNameW(nullptr,self,32768);if(!n||n>=32768)return false;
    return _wcsicmp(exe,self)==0;
}
void OpenDefaultAppsSettings(){
    // 微软推荐做法（Windows 10/11）：程序在 RegisteredApplications 中登记能力后，
    // 打开 ms-settings:defaultapps?registeredAppUser=<名称>，由用户在系统设置中确认。
    // Windows 11 21H2/22H2（2023-04 更新）及 23H2+ 直接定位到本程序页面；更早系统退回通用页面。
    // ShellExecute 可能在内部等待 COM / 激活，放到独立线程，绝不阻塞界面线程。
    std::thread([]{
        const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
        auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",L"ms-settings:defaultapps?registeredAppUser=LumenPDF",nullptr,nullptr,SW_SHOWNORMAL));
        if(result<=32)ShellExecuteW(nullptr,L"open",L"ms-settings:defaultapps",nullptr,nullptr,SW_SHOWNORMAL);
        if(SUCCEEDED(com))CoUninitialize();
    }).detach();
}
bool PromptDefaultPdf(void* owner,const fs::path& sample){
    OPENASINFO info{};const std::wstring file=sample.wstring();
    info.pcszFile=file.c_str();info.pcszClass=nullptr;
    info.oaifInFlags=OAIF_ALLOW_REGISTRATION|OAIF_REGISTER_EXT|OAIF_FORCE_REGISTRATION;
    const HRESULT hr=SHOpenWithDialog(static_cast<HWND>(owner),&info);
    return SUCCEEDED(hr)||hr==HRESULT_FROM_WIN32(ERROR_CANCELLED);
}
bool PdfHandlerRegistered(){
    return !GetValue(HKEY_CURRENT_USER,std::wstring(L"Software\\Classes\\")+kProgId+L"\\shell\\open\\command",nullptr).empty();
}
bool RepairPdfDocumentIcon(const fs::path& executable){
    // 旧版本把应用图标（,0）写作文档图标。只有登记的打开命令指向当前这个 exe 时才改，
    // 保证引用的图标资源一定存在（开发版运行不会改动安装版的登记）。
    const std::wstring classes=std::wstring(L"Software\\Classes\\")+kProgId;
    const auto command=GetValue(HKEY_CURRENT_USER,classes+L"\\shell\\open\\command",nullptr);
    const std::wstring exe=executable.wstring();
    if(command.size()<exe.size()+2||command[0]!=L'"'||_wcsnicmp(command.c_str()+1,exe.c_str(),exe.size())!=0||command[exe.size()+1]!=L'"')return false;
    const std::wstring wanted=L"\""+exe+L"\",-3";
    if(_wcsicmp(GetValue(HKEY_CURRENT_USER,classes+L"\\DefaultIcon",nullptr).c_str(),wanted.c_str())==0)return false;
    if(!SetValue(HKEY_CURRENT_USER,classes+L"\\DefaultIcon",nullptr,wanted))return false;
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
    return true;
}
bool PdfHandlerStale(){
    return MissingExecutable(GetValue(HKEY_CURRENT_USER,std::wstring(L"Software\\Classes\\")+kProgId+L"\\shell\\open\\command",nullptr));
}
std::wstring RegisterMergeMenu(const fs::path& executable){
    const std::wstring exe=executable.wstring();bool ok=true;
    for(const wchar_t* ext:kMergeExtensions){
        const std::wstring key=std::wstring(L"Software\\Classes\\SystemFileAssociations\\")+ext+L"\\shell\\"+kMergeVerb;
        ok&=SetValue(HKEY_CURRENT_USER,key,nullptr,L"使用 LumenPDF 合并");
        ok&=SetValue(HKEY_CURRENT_USER,key,L"MUIVerb",L"使用 LumenPDF 合并");
        // 资源管理器右键菜单的 Icon 值不能带引号（带引号时 Win11 经典菜单不显示图标），路径含空格也无妨。
        ok&=SetValue(HKEY_CURRENT_USER,key,L"Icon",exe);
        // Player：多选时不受 15 个文件的限制；每个文件各启动一次，由单实例汇总到同一个合并列表。
        ok&=SetValue(HKEY_CURRENT_USER,key,L"MultiSelectModel",L"Player");
        ok&=SetValue(HKEY_CURRENT_USER,key+L"\\command",nullptr,L"\""+exe+L"\" --merge \"%1\"");
    }
    if(!ok)return L"写入右键菜单失败，请检查注册表权限。";
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
    return {};
}
void UnregisterMergeMenu(){
    for(const wchar_t* ext:kMergeExtensions)
        RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\SystemFileAssociations\\")+ext+L"\\shell\\"+kMergeVerb).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
}
bool MergeMenuRegistered(){
    return !GetValue(HKEY_CURRENT_USER,std::wstring(L"Software\\Classes\\SystemFileAssociations\\.pdf\\shell\\")+kMergeVerb+L"\\command",nullptr).empty();
}
bool MergeMenuStale(){
    // 旧版本写入了带引号的 Icon 值（经典右键菜单因此不显示图标），也视为需要修复。
    const auto icon=GetValue(HKEY_CURRENT_USER,std::wstring(L"Software\\Classes\\SystemFileAssociations\\.pdf\\shell\\")+kMergeVerb,L"Icon");
    if(icon.empty()||icon.front()==L'"')return true;
    // 新版本增加了扩展名（Excel / PPT）：缺任何一个都重新注册。
    for(const wchar_t* ext:kMergeExtensions)
        if(GetValue(HKEY_CURRENT_USER,std::wstring(L"Software\\Classes\\SystemFileAssociations\\")+ext+L"\\shell\\"+kMergeVerb+L"\\command",nullptr).empty())return true;
    return MissingExecutable(GetValue(HKEY_CURRENT_USER,std::wstring(L"Software\\Classes\\SystemFileAssociations\\.pdf\\shell\\")+kMergeVerb+L"\\command",nullptr));
}
void UnregisterShell(){
    UnregisterMergeMenu();
    RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(L"Software\\Classes\\")+kProgId).c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER,L"Software\\Classes\\.pdf\\OpenWithProgids",kProgId);
    RegDeleteTreeW(HKEY_CURRENT_USER,L"Software\\Classes\\Applications\\LumenPDF.exe");
    RegDeleteTreeW(HKEY_CURRENT_USER,L"Software\\LumenPDF\\Capabilities");
    RegDeleteKeyValueW(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",L"LumenPDF");
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
}
}
namespace lpdf {
bool LaunchNewWindow(const fs::path& path){
    wchar_t exe[32768];const DWORD n=GetModuleFileNameW(nullptr,exe,32768);if(!n||n>=32768)return false;
    std::wstring args=L"--new-window";
    if(!path.empty())args+=L" \""+path.wstring()+L"\"";
    const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",exe,args.c_str(),nullptr,SW_SHOWNORMAL));
    return result>32;
}
std::vector<void*> LumenWindows(){
    // 以“同一可执行文件 + 可见顶层窗口 + 标题以 LumenPDF 结尾”识别，不依赖界面库的窗口类名。
    struct Search {std::wstring exe;std::vector<void*> found;} search;
    wchar_t self[32768];const DWORD n=GetModuleFileNameW(nullptr,self,32768);if(!n||n>=32768)return {};
    search.exe=self;
    EnumWindows([](HWND hwnd,LPARAM data)->BOOL{
        auto& s=*reinterpret_cast<Search*>(data);
        if(!IsWindowVisible(hwnd)||GetWindow(hwnd,GW_OWNER))return TRUE;
        wchar_t title[512];const int len=GetWindowTextW(hwnd,title,512);
        if(len<8||std::wstring_view(title,static_cast<size_t>(len)).substr(static_cast<size_t>(len)-8)!=L"LumenPDF")return TRUE;
        DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return TRUE;
        wchar_t path[32768];DWORD size=32768;
        if(QueryFullProcessImageNameW(process,0,path,&size)&&_wcsicmp(path,s.exe.c_str())==0)s.found.push_back(hwnd);
        CloseHandle(process);return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.found;
}
bool CycleWindows(void* self,int direction){
    auto windows=LumenWindows();if(windows.size()<2)return false;
    // 按窗口句柄排序得到稳定的循环顺序（Z 序会随激活而变化）。
    std::sort(windows.begin(),windows.end());
    auto it=std::find(windows.begin(),windows.end(),self);
    const ptrdiff_t index=it==windows.end()?0:it-windows.begin();
    const ptrdiff_t count=static_cast<ptrdiff_t>(windows.size());
    BringToFront(windows[static_cast<size_t>(((index+direction)%count+count)%count)]);
    return true;
}
}